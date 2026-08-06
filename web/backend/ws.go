package main

import (
	"encoding/json"
	"log"
	"net/http"
	"strconv"
	"strings"
	"sync"

	"github.com/gorilla/websocket"
)

// wsDeviceHub manages per-device WebSocket subscriber sets.
type wsDeviceHub struct {
	mu      sync.Mutex
	clients map[string]map[*websocket.Conn]struct{} // mac → set of conns
}

var wsHub = &wsDeviceHub{
	clients: make(map[string]map[*websocket.Conn]struct{}),
}

func (h *wsDeviceHub) add(mac string, conn *websocket.Conn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.clients[mac] == nil {
		h.clients[mac] = make(map[*websocket.Conn]struct{})
	}
	h.clients[mac][conn] = struct{}{}
}

func (h *wsDeviceHub) remove(mac string, conn *websocket.Conn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if s := h.clients[mac]; s != nil {
		delete(s, conn)
	}
}

func (h *wsDeviceHub) broadcast(mac string, msg []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for conn := range h.clients[mac] {
		if err := conn.WriteMessage(websocket.TextMessage, msg); err != nil {
			conn.Close()
			delete(h.clients[mac], conn)
		}
	}
}

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

// wsRequireAuth is an auth middleware tailored for WebSocket routes.
// Unlike requireAuth, it upgrades the connection first and sends a close
// frame with code 4001 on auth failure so the browser can distinguish
// a bad/expired session from a network error and redirect to login.
func wsRequireAuth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		userID, ok := tokenVerify(extractToken(r))
		if !ok {
			conn, err := upgrader.Upgrade(w, r, nil)
			if err != nil {
				// Upgrade failed (client sent non-WS request); 400 already written.
				return
			}
			conn.WriteMessage(websocket.CloseMessage, //nolint:errcheck
				websocket.FormatCloseMessage(4001, "session expired"))
			conn.Close()
			return
		}
		r.Header.Set("X-User-ID", strconv.FormatInt(userID, 10))
		next(w, r)
	}
}

// handleWS serves /ws/{mac}?token=...
// Auth is enforced in requireAuth middleware before this handler is reached.
func handleWS(w http.ResponseWriter, r *http.Request) {
	uid := userIDFromRequest(r)
	mac := strings.ToUpper(macFromPath(r.URL.Path, "/ws/"))

	if mac == "" {
		http.Error(w, "missing device mac in path", http.StatusBadRequest)
		return
	}

	// Verify user has access to this device
	if !canAccessDevice(uid, mac) {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("[WS] Upgrade error: %v", err)
		return
	}

	wsHub.add(mac, conn)
	defer wsHub.remove(mac, conn)

	// Push current status immediately on connect
	deviceStatusMu.RLock()
	cur := deviceStatus[mac]
	deviceStatusMu.RUnlock()
	if cur != nil {
		conn.WriteMessage(websocket.TextMessage, cur) //nolint:errcheck
	}

	// Read loop — detects disconnect
	for {
		if _, _, err := conn.ReadMessage(); err != nil {
			conn.Close()
			return
		}
	}
}

// handleDeviceStatus serves GET /api/devices/{mac}/status
func handleDeviceStatus(w http.ResponseWriter, r *http.Request) {
	cors(w)
	uid := userIDFromRequest(r)
	mac := strings.ToUpper(macFromPath(r.URL.Path, "/api/devices/"))
	// Strip trailing /status from mac if present
	if idx := strings.Index(mac, "/"); idx >= 0 {
		mac = mac[:idx]
	}

	if !canAccessDevice(uid, mac) {
		jsonError(w, "forbidden", http.StatusForbidden)
		return
	}

	deviceStatusMu.RLock()
	cur := deviceStatus[mac]
	deviceStatusMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	if cur == nil {
		w.Write([]byte(`{}`)) //nolint:errcheck
		return
	}
	w.Write(cur) //nolint:errcheck
}

// handleDeviceControl serves POST /api/devices/{mac}/control
func handleDeviceControl(w http.ResponseWriter, r *http.Request) {
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
	mac := strings.ToUpper(macFromPath(r.URL.Path, "/api/devices/"))
	if idx := strings.Index(mac, "/"); idx >= 0 {
		mac = mac[:idx]
	}

	if !canAccessDevice(uid, mac) {
		jsonError(w, "forbidden", http.StatusForbidden)
		return
	}

	var body map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		jsonError(w, "invalid JSON", http.StatusBadRequest)
		return
	}
	cmd, _ := body["cmd"].(string)
	if !allowedCmds[cmd] {
		jsonError(w, "unknown command", http.StatusBadRequest)
		return
	}
	raw, _ := json.Marshal(body)
	if err := publishControl(mac, raw); err != nil {
		jsonError(w, "MQTT error: "+err.Error(), http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}
