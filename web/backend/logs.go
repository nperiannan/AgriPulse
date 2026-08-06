package main

import (
	"fmt"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"
)

var (
	logsMu   sync.RWMutex
	logsData = make(map[string]logsEntry) // mac → entry
)

type logsEntry struct {
	raw    []byte
	seenAt time.Time
}

func logsStore(mac string, raw []byte) {
	logsMu.Lock()
	logsData[mac] = logsEntry{raw: raw, seenAt: time.Now()}
	logsMu.Unlock()
}

// handleDeviceLogs serves GET /api/devices/{mac}/logs
func handleDeviceLogs(w http.ResponseWriter, r *http.Request) {
	cors(w)
	uid := userIDFromRequest(r)
	mac := strings.ToUpper(macFromPath(r.URL.Path, "/api/devices/"))
	if idx := strings.Index(mac, "/"); idx >= 0 {
		mac = mac[:idx]
	}
	if !canAccessDevice(uid, mac) {
		jsonError(w, "forbidden", http.StatusForbidden)
		return
	}

	logsMu.RLock()
	entry, ok := logsData[mac]
	logsMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	if !ok || len(entry.raw) == 0 {
		w.Write([]byte(`{"logs":[],"note":"No logs received yet"}`)) //nolint:errcheck
		return
	}
	// Inject received_at
	trimmed := strings.TrimSuffix(strings.TrimSpace(string(entry.raw)), "}")
	out := trimmed + fmt.Sprintf(`,"received_at":"%s"}`, entry.seenAt.UTC().Format(time.RFC3339))
	w.Write([]byte(out)) //nolint:errcheck
}

func logsTopic() string {
	log.Println("[WARN] logsTopic() called — legacy, not used in platform mode")
	return ""
}
