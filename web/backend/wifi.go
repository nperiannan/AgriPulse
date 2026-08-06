package main

import (
	"database/sql"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

var (
	wifiMu       sync.RWMutex
	wifiCache    = make(map[string]wifiEntry) // mac → latest wifi_list / wifi_scan
	historyCache = make(map[string]wifiEntry) // mac → latest history_list
)

type wifiEntry struct {
	raw    []byte
	seenAt time.Time
}

// onWifiMsg routes incoming tm/{mac}/wifi messages to the correct cache
// based on the "type" field in the JSON payload.
//
// Note: history is derived server-side from the status stream (see
// detectAndPushEdges) — we no longer persist the controller's history_list
// snapshot, so the two sources can't produce duplicate rows.
func onWifiMsg(topic string, raw []byte) {
	mac := macFromTopic(topic)
	if mac == "" {
		return
	}
	// Determine type from payload (fast prefix scan — avoids full JSON parse)
	rawStr := string(raw)
	isHistory := strings.Contains(rawStr, `"type":"history_list"`)

	wifiMu.Lock()
	if isHistory {
		historyCache[mac] = wifiEntry{raw: raw, seenAt: time.Now()}
	} else {
		wifiCache[mac] = wifiEntry{raw: raw, seenAt: time.Now()}
	}
	wifiMu.Unlock()
}

// handleDeviceWifi serves GET /api/devices/{mac}/wifi
// Returns the last WiFi response published by the device.
func handleDeviceWifi(w http.ResponseWriter, r *http.Request) {
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

	wifiMu.RLock()
	entry, ok := wifiCache[mac]
	wifiMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	if !ok || len(entry.raw) == 0 {
		w.Write([]byte(`{"type":"empty","data":{}}`)) //nolint:errcheck
		return
	}
	w.Write(entry.raw) //nolint:errcheck
}

// handleDeviceHistory serves GET /api/devices/{mac}/history
// Serves persisted history events from the DB (fast, survives restarts).
//
// Optional query params ?from=<unix>&to=<unix> return the full date range
// (including archived rows, for daily/weekly/monthly browsing of records that
// were previously "cleared"). Without them, the default "live" view returns
// only non-archived rows (most recent 1000), matching prior behaviour.
//
// DELETE archives all of this device's rows (archived=1) instead of deleting
// them — "Clear history" in the app no longer destroys the data; archived
// records remain queryable via the date-range params above for later
// debugging/troubleshooting.
func handleDeviceHistory(w http.ResponseWriter, r *http.Request) {
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
	if r.Method == http.MethodDelete {
		db.Exec(`UPDATE history_events SET archived=1 WHERE UPPER(mac)=UPPER(?) AND archived=0`, mac) //nolint:errcheck
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
		return
	}

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
			`SELECT record FROM history_events WHERE UPPER(mac)=UPPER(?) AND ts>=? AND ts<=? ORDER BY ts ASC LIMIT 20000`,
			mac, from, to)
	} else {
		rows, err = db.Query(
			`SELECT record FROM history_events WHERE UPPER(mac)=UPPER(?) AND archived=0 ORDER BY ts DESC LIMIT 1000`, mac)
	}
	if err != nil {
		jsonError(w, "db error", http.StatusInternalServerError)
		return
	}
	defer rows.Close()
	records := make([]string, 0, 256)
	for rows.Next() {
		var rec string
		if rows.Scan(&rec) == nil {
			records = append(records, rec)
		}
	}

	var b strings.Builder
	b.WriteString(`{"type":"history_list","data":{"count":`)
	b.WriteString(strconv.Itoa(len(records)))
	b.WriteString(`,"eeprom":true,"records":[`)
	b.WriteString(strings.Join(records, ","))
	b.WriteString(`]}}`)
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(b.String())) //nolint:errcheck
}
