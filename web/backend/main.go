package main

import (
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

const webVersion = "2.8.0"

func main() {
	// Init subsystems in order
	initDB()
	initAuth()
	initFCM()         // optional push notifications (no-op unless FCM_CREDENTIALS set)
	otaLoadFromDisk() // restore any firmware staged before this run
	go startMQTT()
	go startGitHubPoller() // auto-pull controller_firmware releases from GitHub

	port := env("PORT", "8080")
	staticDir := env("STATIC_DIR", "/app/static")

	mux := http.NewServeMux()

	// ---------------------------------------------------------------------------
	// Auth
	// ---------------------------------------------------------------------------
	mux.HandleFunc("/api/auth/register", handleRegister)
	mux.HandleFunc("/api/auth/login", handleLogin)
	mux.HandleFunc("/api/auth/me", requireAuth(handleMe))
	// Legacy login endpoint — keep working for old app versions
	mux.HandleFunc("/api/login", handleLogin)

	// ---------------------------------------------------------------------------
	// Version
	// ---------------------------------------------------------------------------
	mux.HandleFunc("/api/version", requireAuth(handleVersion))

	// ---------------------------------------------------------------------------
	// Device registry
	// ---------------------------------------------------------------------------
	mux.HandleFunc("/api/devices", requireAuth(func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet, http.MethodOptions:
			handleListDevices(w, r)
		default:
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		}
	}))
	mux.HandleFunc("/api/devices/claim", requireAuth(handleClaimDevice))

	// Push notification token registration
	mux.HandleFunc("/api/push/register", requireAuth(handleRegisterPushToken))

	// OTA endpoints — no auth, ESP32 fetches/polls directly
	mux.HandleFunc("/api/devices/", func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/ota/firmware.bin") {
			handleOtaServeFirmware(w, r)
			return
		}
		if strings.HasSuffix(r.URL.Path, "/ota/check") {
			handleOtaCheck(w, r)
			return
		}
		requireAuth(deviceRouter)(w, r)
	})

	// ---------------------------------------------------------------------------
	// WebSocket — per-device: /ws/{mac}?token=...
	// Legacy single-device:   /ws?token=...  (auto-picks first claimed device)
	// ---------------------------------------------------------------------------
	mux.HandleFunc("/ws/", wsRequireAuth(handleWS))
	mux.HandleFunc("/ws", wsRequireAuth(handleLegacyWS))

	// ---------------------------------------------------------------------------
	// Legacy single-device routes — for old React web frontend (v1.x)
	// These auto-select the user's first claimed device.
	// ---------------------------------------------------------------------------
	mux.HandleFunc("/api/control", requireAuth(handleLegacyControl))
	mux.HandleFunc("/api/ota/status", requireAuth(handleLegacyOtaStatus))
	mux.HandleFunc("/api/ota/upload", requireAuth(handleLegacyOtaUpload))
	mux.HandleFunc("/api/ota/trigger", requireAuth(handleLegacyOtaTrigger))
	mux.HandleFunc("/api/ota/rollback", requireAuth(handleLegacyOtaRollback))
	mux.HandleFunc("/api/logs", requireAuth(handleLegacyLogs))
	mux.HandleFunc("/api/history", requireAuth(handleLegacyHistory))

	// ---------------------------------------------------------------------------
	// Admin
	// ---------------------------------------------------------------------------
	mux.HandleFunc("/api/admin/devices", requireAdmin(handleAdminListDevices))
	mux.HandleFunc("/api/admin/users", requireAdmin(handleAdminListUsers))

	// ---------------------------------------------------------------------------
	// Static SPA fallback
	// ---------------------------------------------------------------------------
	fs := http.FileServer(http.Dir(staticDir))
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		fullPath := filepath.Join(staticDir, filepath.Clean("/"+r.URL.Path))
		if _, err := os.Stat(fullPath); os.IsNotExist(err) {
			http.ServeFile(w, r, filepath.Join(staticDir, "index.html"))
			return
		}
		fs.ServeHTTP(w, r)
	})

	log.Printf("[HTTP] TankMonitor Platform v%s — listening on :%s  static=%s", webVersion, port, staticDir)
	if err := http.ListenAndServe(":"+port, mux); err != nil {
		log.Fatal(err)
	}
}

// deviceRouter dispatches /api/devices/{mac}/{sub} to the correct handler.
func deviceRouter(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}

	rest := strings.TrimPrefix(r.URL.Path, "/api/devices/")
	parts := strings.SplitN(rest, "/", 2)
	sub := ""
	if len(parts) == 2 {
		sub = parts[1]
	}

	switch {
	case sub == "" || sub == "status":
		switch r.Method {
		case http.MethodGet:
			handleDeviceStatus(w, r)
		case http.MethodDelete:
			handleUnclaimDevice(w, r)
		case http.MethodPatch:
			handleRenameDevice(w, r)
		default:
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		}
	case sub == "control":
		handleDeviceControl(w, r)
	case sub == "logs":
		handleDeviceLogs(w, r)
	case sub == "wifi":
		handleDeviceWifi(w, r)
	case sub == "heartbeat":
		handleDeviceHeartbeat(w, r)
	case sub == "delivery-issues":
		handleDeviceDeliveryIssues(w, r)
	case sub == "history":
		handleDeviceHistory(w, r)
	case sub == "ota/status":
		handleOtaStatus(w, r)
	case sub == "ota/upload":
		handleOtaUpload(w, r)
	case sub == "ota/trigger":
		handleOtaTrigger(w, r)
	case sub == "ota/rollback":
		handleOtaRollback(w, r)
	case sub == "ota/check":
		handleOtaCheck(w, r)
	case sub == "ota/firmware.bin":
		// No auth needed — ESP32 fetches directly
		handleOtaServeFirmware(w, r)
	default:
		http.NotFound(w, r)
	}
}

func handleVersion(w http.ResponseWriter, r *http.Request) {
	cors(w)
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"web_version":"` + webVersion + `"}`)) //nolint:errcheck
}
