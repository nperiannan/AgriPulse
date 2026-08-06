package main

import (
	"encoding/json"
	"log"
	"net/http"
	"strings"
	"time"
)

// DeviceRow is the full device record joined with user relationship.
type DeviceRow struct {
	MAC         string `json:"mac"`
	TypeID      string `json:"type_id"`
	DisplayName string `json:"display_name"`
	FWVersion   string `json:"fw_version"`
	LastSeen    string `json:"last_seen"`
	Role        string `json:"role,omitempty"` // owner | viewer | "" (unclaimed)
	Online      bool   `json:"online"`         // last_seen within 30 s
}

// upsertDevice auto-registers a device when it first publishes to MQTT.
// Safe to call on every message — uses INSERT OR IGNORE.
func upsertDevice(mac, typeID, fwVersion string) {
	_, err := db.Exec(`
		INSERT INTO devices(mac, type_id, fw_version, last_seen)
		VALUES(?,?,?,datetime('now'))
		ON CONFLICT(mac) DO UPDATE SET
			fw_version = excluded.fw_version,
			last_seen  = excluded.last_seen,
			type_id    = COALESCE(excluded.type_id, type_id)`,
		mac, typeID, fwVersion)
	if err != nil {
		log.Printf("[DB] upsertDevice %s: %v", mac, err)
	}
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

// GET /api/devices — returns devices owned/viewed by the authenticated user.
// Admins see all devices (claimed + unclaimed).
func handleListDevices(w http.ResponseWriter, r *http.Request) {
	cors(w)
	uid := userIDFromRequest(r)

	// The personal "My Devices" list only shows devices the user has claimed —
	// this holds for admins too, otherwise removing (unclaiming) a device would
	// never hide it because it would still appear via the all-devices view.
	// Admins can still see every device through the dedicated admin panel
	// (handleAdminListDevices).
	rows, err := db.Query(`
			SELECT d.mac, COALESCE(d.type_id,''), COALESCE(d.display_name,''),
			       COALESCE(d.fw_version,''), COALESCE(d.last_seen,''),
			       ud.role
			FROM devices d
			JOIN user_devices ud ON ud.mac = d.mac
			WHERE ud.user_id = ?
			ORDER BY d.last_seen DESC`, uid)
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	result := []DeviceRow{}
	for rows.Next() {
		var d DeviceRow
		rows.Scan(&d.MAC, &d.TypeID, &d.DisplayName, &d.FWVersion, &d.LastSeen, &d.Role) //nolint:errcheck
		d.Online = isOnline(d.LastSeen)
		result = append(result, d)
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(result) //nolint:errcheck
}

// POST /api/devices/claim — body: { mac, display_name }
func handleClaimDevice(w http.ResponseWriter, r *http.Request) {
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
		MAC         string `json:"mac"`
		DisplayName string `json:"display_name"`
		TypeID      string `json:"type_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		jsonError(w, "invalid JSON", http.StatusBadRequest)
		return
	}
	req.MAC = strings.ToUpper(strings.TrimSpace(req.MAC))
	req.DisplayName = strings.TrimSpace(req.DisplayName)
	if req.MAC == "" || req.DisplayName == "" {
		jsonError(w, "mac and display_name are required", http.StatusBadRequest)
		return
	}

	// Device must exist (was seen on MQTT)
	var exists int
	db.QueryRow(`SELECT COUNT(*) FROM devices WHERE mac=?`, req.MAC).Scan(&exists) //nolint:errcheck
	if exists == 0 {
		jsonError(w, "device not found — ensure device is powered on and connected", http.StatusNotFound)
		return
	}

	// Check if already claimed by someone else (admin can still claim)
	var ownerCount int
	db.QueryRow(`SELECT COUNT(*) FROM user_devices WHERE mac=? AND role='owner'`, req.MAC).
		Scan(&ownerCount) //nolint:errcheck

	var isAdmin bool
	db.QueryRow(`SELECT is_admin FROM users WHERE user_id=?`, uid).Scan(&isAdmin) //nolint:errcheck

	if ownerCount > 0 && !isAdmin {
		jsonError(w, "device already claimed", http.StatusConflict)
		return
	}

	// Set display name + assign ownership in one transaction
	tx, err := db.Begin()
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	defer tx.Rollback() //nolint:errcheck

	tx.Exec(`UPDATE devices SET display_name=? WHERE mac=?`, req.DisplayName, req.MAC) //nolint:errcheck
	if req.TypeID != "" {
		tx.Exec(`UPDATE devices SET type_id=? WHERE mac=?`, req.TypeID, req.MAC) //nolint:errcheck
	}
	_, err = tx.Exec(
		`INSERT INTO user_devices(user_id, mac, role) VALUES(?,?,'owner')
		 ON CONFLICT(user_id, mac) DO UPDATE SET role='owner'`,
		uid, req.MAC)
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	tx.Commit() //nolint:errcheck

	log.Printf("[DEVICE] user %d claimed %s as %q", uid, req.MAC, req.DisplayName)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}

// DELETE /api/devices/{mac} — remove device from user's list (unclaim).
func handleUnclaimDevice(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodDelete {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	uid := userIDFromRequest(r)
	mac := strings.ToUpper(strings.TrimSpace(macFromPath(r.URL.Path, "/api/devices/")))

	res, err := db.Exec(`DELETE FROM user_devices WHERE user_id=? AND UPPER(mac)=?`, uid, mac)
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	n, _ := res.RowsAffected()
	// Idempotent: if the device wasn't in the user's list there is nothing to
	// remove, but the desired end state (not in list) is already satisfied.
	if n == 0 {
		log.Printf("[DEVICE] user %d unclaim %s: nothing to remove (idempotent)", uid, mac)
	} else {
		log.Printf("[DEVICE] user %d unclaimed %s", uid, mac)
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}

// PATCH /api/devices/{mac} — rename: body { display_name }
func handleRenameDevice(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPatch {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	uid := userIDFromRequest(r)
	mac := strings.ToUpper(macFromPath(r.URL.Path, "/api/devices/"))

	// Must own or be admin
	if !canAccessDevice(uid, mac) {
		jsonError(w, "forbidden", http.StatusForbidden)
		return
	}

	var req struct {
		DisplayName string `json:"display_name"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		jsonError(w, "invalid JSON", http.StatusBadRequest)
		return
	}
	req.DisplayName = strings.TrimSpace(req.DisplayName)
	if req.DisplayName == "" {
		jsonError(w, "display_name required", http.StatusBadRequest)
		return
	}
	db.Exec(`UPDATE devices SET display_name=? WHERE mac=?`, req.DisplayName, mac) //nolint:errcheck
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}

// GET /api/admin/users — admin: all users with their claimed devices
func handleAdminListUsers(w http.ResponseWriter, r *http.Request) {
	cors(w)

	// 1. Fetch all users
	userRows, err := db.Query(`SELECT user_id, username, is_admin FROM users ORDER BY user_id`)
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	defer userRows.Close()

	type adminDevice struct {
		MAC         string `json:"mac"`
		DisplayName string `json:"display_name"`
		FWVersion   string `json:"fw_version"`
		Online      bool   `json:"online"`
		LastSeen    string `json:"last_seen"`
	}
	type adminUser struct {
		UserID   int64         `json:"user_id"`
		Username string        `json:"username"`
		IsAdmin  bool          `json:"is_admin"`
		Devices  []adminDevice `json:"devices"`
	}

	var users []adminUser
	for userRows.Next() {
		var u adminUser
		if err := userRows.Scan(&u.UserID, &u.Username, &u.IsAdmin); err != nil {
			continue
		}
		u.Devices = []adminDevice{}
		users = append(users, u)
	}
	userRows.Close()

	// 2. Fetch devices for each user
	for i, u := range users {
		devRows, err := db.Query(`
			SELECT d.mac, COALESCE(d.display_name,''), COALESCE(d.fw_version,''), COALESCE(d.last_seen,'')
			FROM user_devices ud
			JOIN devices d ON d.mac = ud.mac
			WHERE ud.user_id = ?
			ORDER BY d.display_name`, u.UserID)
		if err != nil {
			continue
		}
		for devRows.Next() {
			var d adminDevice
			devRows.Scan(&d.MAC, &d.DisplayName, &d.FWVersion, &d.LastSeen) //nolint:errcheck
			d.Online = isOnline(d.LastSeen)
			users[i].Devices = append(users[i].Devices, d)
		}
		devRows.Close()
	}

	if users == nil {
		users = []adminUser{}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(users) //nolint:errcheck
}

// GET /api/admin/devices — admin: all devices with owner info
func handleAdminListDevices(w http.ResponseWriter, r *http.Request) {
	cors(w)
	rows, err := db.Query(`
		SELECT d.mac, COALESCE(d.type_id,''), COALESCE(d.display_name,''),
		       COALESCE(d.fw_version,''), COALESCE(d.last_seen,''),
		       COALESCE(u.username,'')
		FROM devices d
		LEFT JOIN user_devices ud ON ud.mac = d.mac AND ud.role='owner'
		LEFT JOIN users u ON u.user_id = ud.user_id
		ORDER BY d.last_seen DESC`)
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	type adminRow struct {
		DeviceRow
		OwnerUsername string `json:"owner_username"`
	}
	result := []adminRow{}
	for rows.Next() {
		var d adminRow
		rows.Scan(&d.MAC, &d.TypeID, &d.DisplayName, &d.FWVersion, &d.LastSeen, &d.OwnerUsername) //nolint:errcheck
		d.Online = isOnline(d.LastSeen)
		result = append(result, d)
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(result) //nolint:errcheck
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func isOnline(lastSeen string) bool {
	if lastSeen == "" {
		return false
	}
	t, err := time.Parse("2006-01-02 15:04:05", lastSeen)
	if err != nil {
		return false
	}
	return time.Since(t) < 30*time.Second
}

func canAccessDevice(userID int64, mac string) bool {
	var isAdmin bool
	db.QueryRow(`SELECT is_admin FROM users WHERE user_id=?`, userID).Scan(&isAdmin) //nolint:errcheck
	if isAdmin {
		return true
	}
	var count int
	db.QueryRow(`SELECT COUNT(*) FROM user_devices WHERE user_id=? AND mac=?`, userID, mac).
		Scan(&count) //nolint:errcheck
	return count > 0
}

// macFromPath extracts the MAC segment from a URL path after a known prefix.
// e.g. /api/devices/AA:BB:CC:DD:EE:FF → AA:BB:CC:DD:EE:FF
func macFromPath(path, prefix string) string {
	after := strings.TrimPrefix(path, prefix)
	// Strip any trailing sub-path (e.g. /status, /control)
	if idx := strings.Index(after, "/"); idx >= 0 {
		return after[:idx]
	}
	return after
}
