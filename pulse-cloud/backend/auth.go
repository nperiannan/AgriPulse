package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/bcrypt"
)

var authSecret []byte

func initAuth() {
	secret := env("AUTH_SECRET", "")
	if secret != "" {
		authSecret = []byte(secret)
	} else {
		b := make([]byte, 32)
		if _, err := rand.Read(b); err != nil {
			panic(err)
		}
		authSecret = b
		log.Println("[AUTH] No AUTH_SECRET set — generated ephemeral secret (tokens invalidated on restart)")
	}

	// Ensure a default admin account exists if no users in DB yet
	ensureDefaultAdmin()
}

func ensureDefaultAdmin() {
	var count int
	db.QueryRow(`SELECT COUNT(*) FROM users`).Scan(&count) //nolint:errcheck
	if count > 0 {
		return
	}
	user := env("AUTH_USER", "admin")
	pass := env("AUTH_PASS", "tank1234")
	hash, err := bcrypt.GenerateFromPassword([]byte(pass), 12)
	if err != nil {
		log.Fatalf("[AUTH] bcrypt default admin: %v", err)
	}
	_, err = db.Exec(
		`INSERT INTO users(username, password_hash, is_admin) VALUES(?,?,1)`,
		user, string(hash),
	)
	if err != nil {
		log.Printf("[AUTH] ensureDefaultAdmin: %v", err)
		return
	}
	log.Printf("[AUTH] Created default admin user: %s", user)
}

// ---------------------------------------------------------------------------
// JWT-style tokens (HMAC-SHA256, stateless)
// Payload: base64url(userID:expiry)
// ---------------------------------------------------------------------------

func tokenMake(userID int64) string {
	expiry := strconv.FormatInt(time.Now().Add(30*24*time.Hour).Unix(), 10)
	payload := base64.RawURLEncoding.EncodeToString([]byte(strconv.FormatInt(userID, 10) + ":" + expiry))
	mac := hmac.New(sha256.New, authSecret)
	mac.Write([]byte(payload))
	sig := base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	return payload + "." + sig
}

// tokenVerify returns (userID, true) if valid and not expired.
func tokenVerify(token string) (int64, bool) {
	parts := strings.SplitN(token, ".", 2)
	if len(parts) != 2 {
		return 0, false
	}
	payload, sig := parts[0], parts[1]
	mac := hmac.New(sha256.New, authSecret)
	mac.Write([]byte(payload))
	expected := base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	if !hmac.Equal([]byte(sig), []byte(expected)) {
		return 0, false
	}
	raw, err := base64.RawURLEncoding.DecodeString(payload)
	if err != nil {
		return 0, false
	}
	idx := strings.LastIndex(string(raw), ":")
	if idx < 0 {
		return 0, false
	}
	expiry, err := strconv.ParseInt(string(raw[idx+1:]), 10, 64)
	if err != nil || time.Now().Unix() > expiry {
		return 0, false
	}
	userID, err := strconv.ParseInt(string(raw[:idx]), 10, 64)
	if err != nil {
		return 0, false
	}
	return userID, true
}

func extractToken(r *http.Request) string {
	if auth := r.Header.Get("Authorization"); strings.HasPrefix(auth, "Bearer ") {
		return strings.TrimPrefix(auth, "Bearer ")
	}
	return r.URL.Query().Get("token")
}

// requireAuth wraps a handler; injects X-User-ID header for downstream use.
func requireAuth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodOptions {
			cors(w)
			w.WriteHeader(http.StatusNoContent)
			return
		}
		userID, ok := tokenVerify(extractToken(r))
		if !ok {
			cors(w)
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusUnauthorized)
			w.Write([]byte(`{"error":"unauthorized"}`)) //nolint:errcheck
			return
		}
		// Pass userID via header so handlers can read it without re-parsing token
		r.Header.Set("X-User-ID", strconv.FormatInt(userID, 10))
		next(w, r)
	}
}

// requireAdmin wraps requireAuth + checks is_admin flag.
func requireAdmin(next http.HandlerFunc) http.HandlerFunc {
	return requireAuth(func(w http.ResponseWriter, r *http.Request) {
		uid := userIDFromRequest(r)
		var isAdmin bool
		db.QueryRow(`SELECT is_admin FROM users WHERE user_id=?`, uid).Scan(&isAdmin) //nolint:errcheck
		if !isAdmin {
			cors(w)
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusForbidden)
			w.Write([]byte(`{"error":"forbidden"}`)) //nolint:errcheck
			return
		}
		next(w, r)
	})
}

func userIDFromRequest(r *http.Request) int64 {
	id, _ := strconv.ParseInt(r.Header.Get("X-User-ID"), 10, 64)
	return id
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

func handleRegister(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		jsonError(w, "invalid JSON", http.StatusBadRequest)
		return
	}
	req.Username = strings.TrimSpace(req.Username)
	if len(req.Username) < 3 || len(req.Password) < 6 {
		jsonError(w, "username min 3 chars, password min 6 chars", http.StatusBadRequest)
		return
	}

	hash, err := bcrypt.GenerateFromPassword([]byte(req.Password), 12)
	if err != nil {
		jsonError(w, "server error", http.StatusInternalServerError)
		return
	}
	res, err := db.Exec(
		`INSERT INTO users(username, password_hash) VALUES(?,?)`,
		req.Username, string(hash),
	)
	if err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			jsonError(w, "username already taken", http.StatusConflict)
		} else {
			jsonError(w, "server error", http.StatusInternalServerError)
		}
		return
	}
	userID, _ := res.LastInsertId()
	token := tokenMake(userID)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(map[string]interface{}{ //nolint:errcheck
		"token":    token,
		"user_id":  userID,
		"username": req.Username,
		"is_admin": false,
	})
}

func handleLogin(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		jsonError(w, "invalid JSON", http.StatusBadRequest)
		return
	}

	var userID int64
	var hash string
	var isAdmin bool
	err := db.QueryRow(
		`SELECT user_id, password_hash, is_admin FROM users WHERE username=?`,
		req.Username,
	).Scan(&userID, &hash, &isAdmin)

	// Use constant-time comparison via bcrypt; invalid credentials always return same error
	if err != nil || bcrypt.CompareHashAndPassword([]byte(hash), []byte(req.Password)) != nil {
		// Always return same message — don't leak whether username exists
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnauthorized)
		w.Write([]byte(`{"error":"invalid credentials"}`)) //nolint:errcheck
		return
	}

	token := tokenMake(userID)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{ //nolint:errcheck
		"token":    token,
		"user_id":  userID,
		"username": req.Username,
		"is_admin": isAdmin,
	})
}

func handleMe(w http.ResponseWriter, r *http.Request) {
	cors(w)
	uid := userIDFromRequest(r)
	var username string
	var isAdmin bool
	err := db.QueryRow(`SELECT username, is_admin FROM users WHERE user_id=?`, uid).
		Scan(&username, &isAdmin)
	if err != nil {
		jsonError(w, "user not found", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{ //nolint:errcheck
		"user_id":  uid,
		"username": username,
		"is_admin": isAdmin,
	})
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func cors(w http.ResponseWriter) {
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS")
	w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization")
}

func jsonError(w http.ResponseWriter, msg string, code int) {
	cors(w)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": msg}) //nolint:errcheck
}

func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
