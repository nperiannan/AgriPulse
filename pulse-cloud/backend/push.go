package main

import (
	"bytes"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

// Firebase Cloud Messaging (HTTP v1) push notifications.
//
// Activation is entirely optional and self-contained: set FCM_CREDENTIALS to
// the path of a Firebase service-account JSON file. If it is unset or missing,
// every push call is a silent no-op, so the server runs unchanged without it.

type fcmServiceAccount struct {
	ClientEmail string `json:"client_email"`
	PrivateKey  string `json:"private_key"`
	TokenURI    string `json:"token_uri"`
	ProjectID   string `json:"project_id"`
}

var (
	fcmSA       *fcmServiceAccount
	fcmKey      *rsa.PrivateKey
	fcmTokenMu  sync.Mutex
	fcmToken    string
	fcmTokenExp time.Time
	fcmEnabled  bool
	fcmInitOnce sync.Once
)

// initFCM loads the service-account file once. Safe to call repeatedly.
func initFCM() {
	fcmInitOnce.Do(func() {
		path := env("FCM_CREDENTIALS", "")
		if path == "" {
			log.Printf("[PUSH] FCM_CREDENTIALS not set — push notifications disabled")
			return
		}
		data, err := os.ReadFile(path)
		if err != nil {
			log.Printf("[PUSH] cannot read FCM_CREDENTIALS (%s): %v — push disabled", path, err)
			return
		}
		var sa fcmServiceAccount
		if err := json.Unmarshal(data, &sa); err != nil {
			log.Printf("[PUSH] bad service-account JSON: %v — push disabled", err)
			return
		}
		block, _ := pem.Decode([]byte(sa.PrivateKey))
		if block == nil {
			log.Printf("[PUSH] service-account private_key is not valid PEM — push disabled")
			return
		}
		var key *rsa.PrivateKey
		if k, err := x509.ParsePKCS8PrivateKey(block.Bytes); err == nil {
			rk, ok := k.(*rsa.PrivateKey)
			if !ok {
				log.Printf("[PUSH] service-account key is not RSA — push disabled")
				return
			}
			key = rk
		} else if k, err := x509.ParsePKCS1PrivateKey(block.Bytes); err == nil {
			key = k
		} else {
			log.Printf("[PUSH] cannot parse service-account private key — push disabled")
			return
		}
		if sa.TokenURI == "" {
			sa.TokenURI = "https://oauth2.googleapis.com/token"
		}
		fcmSA = &sa
		fcmKey = key
		fcmEnabled = true
		log.Printf("[PUSH] FCM enabled — project %s", sa.ProjectID)
	})
}

// fcmAccessToken returns a cached OAuth2 access token, minting a new one via a
// signed JWT assertion when needed.
func fcmAccessToken() (string, error) {
	fcmTokenMu.Lock()
	defer fcmTokenMu.Unlock()
	if fcmToken != "" && time.Now().Before(fcmTokenExp.Add(-60*time.Second)) {
		return fcmToken, nil
	}
	now := time.Now()
	header := b64url([]byte(`{"alg":"RS256","typ":"JWT"}`))
	claims := map[string]any{
		"iss":   fcmSA.ClientEmail,
		"scope": "https://www.googleapis.com/auth/firebase.messaging",
		"aud":   fcmSA.TokenURI,
		"iat":   now.Unix(),
		"exp":   now.Add(time.Hour).Unix(),
	}
	claimsJSON, _ := json.Marshal(claims)
	signingInput := header + "." + b64url(claimsJSON)

	sum := sha256.Sum256([]byte(signingInput))
	sig, err := rsa.SignPKCS1v15(rand.Reader, fcmKey, crypto.SHA256, sum[:])
	if err != nil {
		return "", err
	}
	assertion := signingInput + "." + base64.RawURLEncoding.EncodeToString(sig)

	form := url.Values{}
	form.Set("grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer")
	form.Set("assertion", assertion)

	req, _ := http.NewRequest(http.MethodPost, fcmSA.TokenURI, strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("token endpoint %d: %s", resp.StatusCode, string(body))
	}
	var tr struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int    `json:"expires_in"`
	}
	if err := json.Unmarshal(body, &tr); err != nil {
		return "", err
	}
	fcmToken = tr.AccessToken
	fcmTokenExp = now.Add(time.Duration(tr.ExpiresIn) * time.Second)
	return fcmToken, nil
}

func b64url(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }

// motorFields is the minimal status subset used for edge detection.
type motorFields struct {
	OHMotor bool   `json:"oh_motor"`
	UGMotor bool   `json:"ug_motor"`
	TxLost  bool   `json:"tx_lost"`
	OHState string `json:"oh_state"`
	UGState string `json:"ug_state"`
	OHRsn   int    `json:"oh_rsn"`
	UGRsn   int    `json:"ug_rsn"`
}

// reasonCodeStr maps the controller's HistReason code (published as oh_rsn /
// ug_rsn in the status JSON) to the same human string the controller and app
// use. Returns "" for unknown/absent codes so callers can fall back to guessing.
func reasonCodeStr(code int) string {
	switch code {
	case 1:
		return "Auto (level)"
	case 2:
		return "Manual (app)"
	case 3:
		return "Manual (web)"
	case 4:
		return "Manual (touch)"
	case 5:
		return "Scheduled"
	case 6:
		return "Auto (tank full)"
	case 7:
		return "Max runtime"
	case 8:
		return "LoRa signal lost"
	case 9:
		return "Power cut"
	case 10:
		return "Power restored"
	default:
		return ""
	}
}

// ── Reason inference ────────────────────────────────────────────────────────
// The backend derives history purely from the status stream, so it can't see
// the controller's granular reason codes. It infers a basic reason: "Manual"
// when a remote ON/OFF command passed through just before the change.
var (
	remoteCmdMu sync.Mutex
	remoteCmdAt = map[string]time.Time{} // "MAC|OH" / "MAC|UG" → last remote cmd
)

func noteRemoteMotorCmd(mac string, body []byte) {
	var c struct {
		Cmd string `json:"cmd"`
	}
	if json.Unmarshal(body, &c) != nil {
		return
	}
	var motor string
	switch c.Cmd {
	case "oh_on", "oh_off":
		motor = "OH"
	case "ug_on", "ug_off":
		motor = "UG"
	default:
		return
	}
	remoteCmdMu.Lock()
	remoteCmdAt[strings.ToUpper(mac)+"|"+motor] = time.Now()
	remoteCmdMu.Unlock()
}

func inferReason(mac, motor string) string {
	remoteCmdMu.Lock()
	t, ok := remoteCmdAt[strings.ToUpper(mac)+"|"+motor]
	remoteCmdMu.Unlock()
	if ok && time.Since(t) < 25*time.Second {
		return "Manual (App/Web)"
	}
	return "Auto / Scheduled"
}

// recordHistoryEvent stores a derived event in the DB in the same JSON shape the
// controller (and the app) uses.
func recordHistoryEvent(mac, ev, ohState, ugState string, ohM, ugM bool, reason string) {
	now := time.Now()
	rec := map[string]any{
		"ts":     now.Unix(),
		"time":   now.Format("15:04 Mon 02-01-2006"),
		"ev":     ev,
		"oh":     ohState,
		"ug":     ugState,
		"ohM":    ohM,
		"ugM":    ugM,
		"rsn":    0,
		"rsnStr": reason,
	}
	b, _ := json.Marshal(rec)
	db.Exec(`INSERT OR IGNORE INTO history_events(mac, ts, ev, record) VALUES(?,?,?,?)`,
		strings.ToUpper(mac), now.Unix(), ev, string(b)) //nolint:errcheck
}

func onOffWord(on bool) string {
	if on {
		return "ON"
	}
	return "OFF"
}

// detectAndPushEdges compares previous and current status, records derived
// history events, and (when enabled) pushes a notification on each change.
func detectAndPushEdges(mac string, prevRaw, newRaw []byte) {
	if len(prevRaw) == 0 {
		return // need a prior sample to detect an edge
	}
	var prev, cur motorFields
	if json.Unmarshal(prevRaw, &prev) != nil || json.Unmarshal(newRaw, &cur) != nil {
		return
	}
	name := deviceDisplayName(mac)

	if prev.OHMotor != cur.OHMotor {
		reason := reasonCodeStr(cur.OHRsn)
		if reason == "" {
			reason = inferReason(mac, "OH")
		}
		recordHistoryEvent(mac, "OH Motor "+onOffWord(cur.OHMotor),
			cur.OHState, cur.UGState, cur.OHMotor, cur.UGMotor, reason)
		if fcmEnabled {
			pushToDeviceOwners(mac, name, "OH motor turned "+onOffWord(cur.OHMotor))
		}
	}
	if prev.UGMotor != cur.UGMotor {
		reason := reasonCodeStr(cur.UGRsn)
		if reason == "" {
			reason = inferReason(mac, "UG")
		}
		recordHistoryEvent(mac, "UG Motor "+onOffWord(cur.UGMotor),
			cur.OHState, cur.UGState, cur.OHMotor, cur.UGMotor, reason)
		if fcmEnabled {
			pushToDeviceOwners(mac, name, "UG motor turned "+onOffWord(cur.UGMotor))
		}
	}
	if !prev.TxLost && cur.TxLost {
		recordHistoryEvent(mac, "Transmitter Lost",
			cur.OHState, cur.UGState, cur.OHMotor, cur.UGMotor, "Signal lost")
		if fcmEnabled {
			pushToDeviceOwners(mac, name, "⚠ Transmitter signal lost")
		}
	}
}

func deviceDisplayName(mac string) string {
	var name string
	db.QueryRow(`SELECT COALESCE(display_name,'') FROM devices WHERE UPPER(mac)=UPPER(?)`, mac).Scan(&name) //nolint:errcheck
	if strings.TrimSpace(name) == "" {
		return "Tank Monitor"
	}
	return name
}

// sendPushToToken delivers one notification. Returns whether the token is stale
// (UNREGISTERED / invalid) so the caller can prune it.
func sendPushToToken(token, title, body string) (stale bool, err error) {
	access, err := fcmAccessToken()
	if err != nil {
		return false, err
	}
	msg := map[string]any{
		"message": map[string]any{
			"token": token,
			"notification": map[string]any{
				"title": title,
				"body":  body,
			},
			"android": map[string]any{
				"priority": "high",
				"notification": map[string]any{
					"channel_id": "motor_status",
				},
			},
		},
	}
	payload, _ := json.Marshal(msg)
	endpoint := fmt.Sprintf("https://fcm.googleapis.com/v1/projects/%s/messages:send", fcmSA.ProjectID)
	req, _ := http.NewRequest(http.MethodPost, endpoint, bytes.NewReader(payload))
	req.Header.Set("Authorization", "Bearer "+access)
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return false, err
	}
	defer resp.Body.Close()
	rb, _ := io.ReadAll(resp.Body)
	if resp.StatusCode == http.StatusOK {
		return false, nil
	}
	// 404 UNREGISTERED / 400 invalid token → prune
	if resp.StatusCode == http.StatusNotFound ||
		(resp.StatusCode == http.StatusBadRequest && bytes.Contains(rb, []byte("INVALID_ARGUMENT"))) {
		return true, errors.New(string(rb))
	}
	return false, fmt.Errorf("fcm send %d: %s", resp.StatusCode, string(rb))
}

// pushToDeviceOwners sends a notification to every user who has claimed the
// given device and registered a push token. No-op when FCM is disabled.
func pushToDeviceOwners(mac, title, body string) {
	initFCM()
	if !fcmEnabled {
		return
	}
	rows, err := db.Query(`
		SELECT pt.token
		FROM push_tokens pt
		JOIN user_devices ud ON ud.user_id = pt.user_id
		WHERE UPPER(ud.mac) = UPPER(?)`, mac)
	if err != nil {
		log.Printf("[PUSH] token query: %v", err)
		return
	}
	var tokens []string
	for rows.Next() {
		var t string
		if rows.Scan(&t) == nil && t != "" {
			tokens = append(tokens, t)
		}
	}
	rows.Close()

	for _, t := range tokens {
		stale, err := sendPushToToken(t, title, body)
		if stale {
			db.Exec(`DELETE FROM push_tokens WHERE token=?`, t) //nolint:errcheck
			log.Printf("[PUSH] pruned stale token")
		} else if err != nil {
			log.Printf("[PUSH] send failed: %v", err)
		}
	}
}

// handleRegisterPushToken stores/refreshes the caller's FCM token.
// POST /api/push/register  body: { "token": "..." }
func handleRegisterPushToken(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	uid := userIDFromRequest(r)
	var req struct {
		Token string `json:"token"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		jsonError(w, "invalid JSON", http.StatusBadRequest)
		return
	}
	req.Token = strings.TrimSpace(req.Token)
	if req.Token == "" {
		jsonError(w, "token required", http.StatusBadRequest)
		return
	}
	// A token belongs to exactly one user — reassign if it moved devices/accounts.
	_, err := db.Exec(`
		INSERT INTO push_tokens(token, user_id, updated_at)
		VALUES(?,?,datetime('now'))
		ON CONFLICT(token) DO UPDATE SET user_id=excluded.user_id, updated_at=excluded.updated_at`,
		req.Token, uid)
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}
