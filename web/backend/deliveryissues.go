package main

import (
	"database/sql"
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------
// Delivery issues archive
//
// The app keeps a local (SharedPreferences) log of "delivery issues" — motor
// commands the controller never acknowledged. Previously "CLEAR" in the app
// just wiped that local log forever. Per the 2026-07-19 request, these are
// "unacknowledged incidents" that may be needed later for debugging, so the
// app now archives them here (POST) before clearing its local copy, and they
// can be retrieved later (GET) for troubleshooting.
// ---------------------------------------------------------------------------

type deliveryIssueIn struct {
	Ts         int64  `json:"ts"`    // unix seconds
	Motor      string `json:"motor"` // "OH" | "UG"
	Start      bool   `json:"start"` // true = start command, false = stop command
	DeviceName string `json:"device_name"`
}

type deliveryIssueOut struct {
	Ts         int64  `json:"ts"`
	Motor      string `json:"motor"`
	Start      bool   `json:"start"`
	DeviceName string `json:"device_name"`
	ArchivedAt string `json:"archived_at"`
}

// handleDeviceDeliveryIssues serves:
//
//	POST /api/devices/{mac}/delivery-issues  — archive a batch of issues
//	GET  /api/devices/{mac}/delivery-issues[?from=&to=] — list archived issues
func handleDeviceDeliveryIssues(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	uid := userIDFromRequest(r)
	mac := strings.ToUpper(macFromPath(r.URL.Path, "/api/devices/"))
	if idx := strings.Index(mac, "/"); idx >= 0 {
		mac = mac[:idx]
	}
	if !canAccessDevice(uid, mac) {
		jsonError(w, "forbidden", http.StatusForbidden)
		return
	}

	switch r.Method {
	case http.MethodPost:
		var issues []deliveryIssueIn
		if err := json.NewDecoder(r.Body).Decode(&issues); err != nil {
			jsonError(w, "invalid JSON", http.StatusBadRequest)
			return
		}
		tx, err := db.Begin()
		if err != nil {
			jsonError(w, "db error", http.StatusInternalServerError)
			return
		}
		for _, iss := range issues {
			if iss.Motor == "" || iss.Ts == 0 {
				continue
			}
			startInt := 0
			if iss.Start {
				startInt = 1
			}
			tx.Exec( //nolint:errcheck
				`INSERT INTO delivery_issues_archive(mac, ts, motor, start, device_name) VALUES(?,?,?,?,?)`,
				mac, iss.Ts, iss.Motor, startInt, iss.DeviceName)
		}
		if err := tx.Commit(); err != nil {
			jsonError(w, "db error", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true}`)) //nolint:errcheck

	case http.MethodGet:
		fromStr := r.URL.Query().Get("from")
		toStr := r.URL.Query().Get("to")
		var rows *sql.Rows
		var err error
		if fromStr != "" || toStr != "" {
			from, _ := strconv.ParseInt(fromStr, 10, 64)
			to, _ := strconv.ParseInt(toStr, 10, 64)
			if to == 0 {
				to = time.Now().Unix()
			}
			rows, err = db.Query(
				`SELECT ts, motor, start, COALESCE(device_name,''), archived_at
				 FROM delivery_issues_archive WHERE UPPER(mac)=UPPER(?) AND ts>=? AND ts<=? ORDER BY ts DESC LIMIT 5000`,
				mac, from, to)
		} else {
			rows, err = db.Query(
				`SELECT ts, motor, start, COALESCE(device_name,''), archived_at
				 FROM delivery_issues_archive WHERE UPPER(mac)=UPPER(?) ORDER BY ts DESC LIMIT 200`, mac)
		}
		if err != nil {
			jsonError(w, "db error", http.StatusInternalServerError)
			return
		}
		defer rows.Close()
		out := make([]deliveryIssueOut, 0, 64)
		for rows.Next() {
			var d deliveryIssueOut
			var startInt int
			if rows.Scan(&d.Ts, &d.Motor, &startInt, &d.DeviceName, &d.ArchivedAt) == nil {
				d.Start = startInt != 0
				out = append(out, d)
			}
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(out) //nolint:errcheck

	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}
