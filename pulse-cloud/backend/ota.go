package main

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// Per-device OTA state
type OtaInfo struct {
	HasFirmware bool      `json:"has_firmware"`
	Filename    string    `json:"filename"`
	Size        int64     `json:"size"`
	UploadedAt  string    `json:"uploaded_at"`
	Phase       string    `json:"phase"` // idle | triggered | ack_received | downloading | success | failed
	PrevFw      string    `json:"prev_fw,omitempty"`
	TriggeredAt time.Time `json:"-"` // internal; used for elapsed tracking
}

var (
	otaMu   sync.RWMutex
	otaInfo = make(map[string]*OtaInfo) // mac → OtaInfo
)

const otaDir = "/data/ota"

func otaFilePath(mac string) string {
	// Sanitise mac for filesystem use (replace : with -)
	return otaDir + "/" + strings.ReplaceAll(mac, ":", "-") + ".bin"
}

// otaOnStatusReceived is called by mqtt.go on every status message.
func otaOnStatusReceived(mac, fw string) {
	otaMu.Lock()
	defer otaMu.Unlock()
	info := otaInfo[mac]
	if info == nil {
		return
	}
	// Device is alive and responded — advance triggered → ack_received.
	if info.Phase == "triggered" {
		info.Phase = "ack_received"
		log.Printf("[OTA] %s: ack received — device online", mac)
	}
	if (info.Phase == "ack_received" || info.Phase == "downloading") && fw != "" {
		if fw != info.PrevFw {
			info.Phase = "success"
			log.Printf("[OTA] %s: success — fw changed %s → %s", mac, info.PrevFw, fw)
		} else if info.Phase == "downloading" {
			// Device re-reported after downloading — same fw version means it already
			// had this firmware (re-flash of same version). Mark success to stop re-serving.
			info.Phase = "success"
			log.Printf("[OTA] %s: success — fw unchanged %s (same-version re-flash)", mac, fw)
		}
	}
}

// otaLoadFromDisk scans the OTA directory and restores in-memory state for
// any firmware files left over from a previous run (e.g. after container restart).
func otaLoadFromDisk() {
	entries, err := os.ReadDir(otaDir)
	if err != nil {
		return // directory may not exist yet — that's fine
	}
	otaMu.Lock()
	defer otaMu.Unlock()
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".bin") {
			continue
		}
		// Filename is "{mac-with-dashes}.bin" → convert back to colon MAC
		macDash := strings.TrimSuffix(e.Name(), ".bin")
		mac := strings.ToUpper(strings.ReplaceAll(macDash, "-", ":"))
		info, err2 := e.Info()
		if err2 != nil {
			continue
		}
		if _, exists := otaInfo[mac]; exists {
			continue // already tracked (uploaded this session)
		}
		otaInfo[mac] = &OtaInfo{
			HasFirmware: true,
			Filename:    "firmware.bin",
			Size:        info.Size(),
			UploadedAt:  info.ModTime().UTC().Format(time.RFC3339),
			Phase:       "idle",
		}
		log.Printf("[OTA] restored staged firmware for %s (%d bytes) from disk", mac, info.Size())
	}
}

// otaEnsureLoaded checks whether a firmware file exists on disk for mac and
// loads it into the in-memory map if it is not already tracked. This allows
// firmware files placed directly on disk (e.g. via SCP) after container start
// to be picked up automatically on the next OTA poll without requiring a restart.
func otaEnsureLoaded(mac string) {
	otaMu.RLock()
	_, tracked := otaInfo[mac]
	otaMu.RUnlock()
	if tracked {
		return // already in memory — nothing to do
	}
	fi, err := os.Stat(otaFilePath(mac))
	if err != nil {
		return // file doesn't exist
	}
	otaMu.Lock()
	defer otaMu.Unlock()
	// Re-check under write lock to avoid a race
	if _, exists := otaInfo[mac]; exists {
		return
	}
	otaInfo[mac] = &OtaInfo{
		HasFirmware: true,
		Filename:    "firmware.bin",
		Size:        fi.Size(),
		UploadedAt:  fi.ModTime().UTC().Format(time.RFC3339),
		Phase:       "idle",
	}
	log.Printf("[OTA] %s: found firmware on disk (%d bytes), loaded into memory", mac, fi.Size())
}

// ---------------------------------------------------------------------------
// HTTP handlers — all scoped to /api/devices/{mac}/ota/...
// ---------------------------------------------------------------------------

func handleOtaStatus(w http.ResponseWriter, r *http.Request) {
	cors(w)
	mac := otaMacFromPath(r.URL.Path)
	otaMu.RLock()
	info := otaInfo[mac]
	otaMu.RUnlock()
	w.Header().Set("Content-Type", "application/json")
	if info == nil {
		json.NewEncoder(w).Encode(OtaInfo{Phase: "idle"}) //nolint:errcheck
		return
	}
	json.NewEncoder(w).Encode(*info) //nolint:errcheck
}

func handleOtaUpload(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	mac := otaMacFromPath(r.URL.Path)

	if err := r.ParseMultipartForm(64 << 20); err != nil {
		http.Error(w, "file too large or invalid form", http.StatusBadRequest)
		return
	}
	file, header, err := r.FormFile("firmware")
	if err != nil {
		http.Error(w, "missing firmware field", http.StatusBadRequest)
		return
	}
	defer file.Close()

	if !strings.HasSuffix(strings.ToLower(header.Filename), ".bin") {
		http.Error(w, "only .bin files accepted", http.StatusBadRequest)
		return
	}

	if err := os.MkdirAll(otaDir, 0755); err != nil {
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}
	dest, err := os.Create(otaFilePath(mac))
	if err != nil {
		http.Error(w, "storage error", http.StatusInternalServerError)
		return
	}
	defer dest.Close()
	written, err := io.Copy(dest, file)
	if err != nil {
		http.Error(w, "write error", http.StatusInternalServerError)
		return
	}

	otaMu.Lock()
	otaInfo[mac] = &OtaInfo{
		HasFirmware: true,
		Filename:    header.Filename,
		Size:        written,
		UploadedAt:  time.Now().UTC().Format(time.RFC3339),
		Phase:       "idle",
	}
	otaMu.Unlock()

	log.Printf("[OTA] %s: firmware uploaded %s (%d bytes)", mac, header.Filename, written)
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}

func handleOtaTrigger(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	mac := otaMacFromPath(r.URL.Path)

	otaMu.RLock()
	info := otaInfo[mac]
	otaMu.RUnlock()
	if info == nil || !info.HasFirmware {
		http.Error(w, "no firmware staged for this device", http.StatusBadRequest)
		return
	}

	var firmwareURL string
	if base := env("OTA_BASE_URL", ""); base != "" {
		firmwareURL = strings.TrimRight(base, "/") + "/api/devices/" + mac + "/ota/firmware.bin"
	} else {
		host := r.Header.Get("X-Forwarded-Host")
		if host == "" {
			host = r.Host
		}
		scheme := "http"
		if r.Header.Get("X-Forwarded-Proto") == "https" || r.TLS != nil {
			scheme = "https"
		}
		firmwareURL = scheme + "://" + host + "/api/devices/" + mac + "/ota/firmware.bin"
	}

	payload, _ := json.Marshal(map[string]interface{}{"cmd": "ota_start", "url": firmwareURL})
	if err := publishControl(mac, payload); err != nil {
		http.Error(w, "MQTT error: "+err.Error(), http.StatusServiceUnavailable)
		return
	}
	log.Printf("[OTA] %s: triggered — URL: %s", mac, firmwareURL)

	// Capture current fw for change detection
	deviceStatusMu.RLock()
	var curSt struct {
		FW string `json:"fw"`
	}
	json.Unmarshal(deviceStatus[mac], &curSt) //nolint:errcheck
	deviceStatusMu.RUnlock()

	otaMu.Lock()
	otaInfo[mac].Phase = "triggered"
	otaInfo[mac].PrevFw = curSt.FW
	otaInfo[mac].TriggeredAt = time.Now()
	otaMu.Unlock()

	// Auto-fail after 150 s; clean up firmware file so next upload starts fresh.
	go func() {
		time.Sleep(150 * time.Second)
		otaMu.Lock()
		i := otaInfo[mac]
		if i != nil && (i.Phase == "triggered" || i.Phase == "ack_received" || i.Phase == "downloading") {
			i.Phase = "failed"
			i.HasFirmware = false
			log.Printf("[OTA] %s: timeout — marking failed, removing staged firmware", mac)
			os.Remove(otaFilePath(mac)) //nolint:errcheck
		}
		otaMu.Unlock()
	}()

	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}

func handleOtaRollback(w http.ResponseWriter, r *http.Request) {
	cors(w)
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	mac := otaMacFromPath(r.URL.Path)
	payload, _ := json.Marshal(map[string]string{"cmd": "ota_rollback"})
	if err := publishControl(mac, payload); err != nil {
		http.Error(w, "MQTT error: "+err.Error(), http.StatusServiceUnavailable)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
}

// handleOtaServeFirmware serves GET /api/devices/{mac}/ota/firmware.bin
// No auth — ESP32 downloads without token support.
func handleOtaServeFirmware(w http.ResponseWriter, r *http.Request) {
	mac := otaMacFromPath(r.URL.Path)
	// Pick up firmware files placed on disk after container start (e.g. via SCP)
	otaEnsureLoaded(mac)
	otaMu.RLock()
	info := otaInfo[mac]
	otaMu.RUnlock()
	if info == nil || !info.HasFirmware {
		http.Error(w, "no firmware", http.StatusNotFound)
		return
	}
	log.Printf("[OTA] %s: serving firmware to %s", mac, r.RemoteAddr)
	otaMu.Lock()
	if otaInfo[mac].Phase == "triggered" || otaInfo[mac].Phase == "idle" {
		otaInfo[mac].Phase = "downloading"
	}
	otaMu.Unlock()
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="firmware.bin"`)
	http.ServeFile(w, r, otaFilePath(mac))
}

// handleOtaCheck responds to GET /api/devices/{mac}/ota/check?fw=1.5.1
// Returns {"update":true,"url":"..."} if firmware is staged and version differs,
// otherwise {"update":false}. No auth — ESP32 polls this directly.
func handleOtaCheck(w http.ResponseWriter, r *http.Request) {
	mac := otaMacFromPath(r.URL.Path)
	currentFW := r.URL.Query().Get("fw")

	// Pick up firmware files placed on disk after container start (e.g. via SCP)
	otaEnsureLoaded(mac)

	otaMu.RLock()
	info := otaInfo[mac]
	otaMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")

	if info == nil || !info.HasFirmware {
		w.Write([]byte(`{"update":false}`)) //nolint:errcheck
		return
	}

	// If OTA is staged (any phase except success/failed) and we have a firmware file, offer it
	if info.Phase == "success" || info.Phase == "failed" {
		w.Write([]byte(`{"update":false}`)) //nolint:errcheck
		return
	}

	// Build download URL
	host := r.Header.Get("X-Forwarded-Host")
	if host == "" {
		host = r.Host
	}
	scheme := "http"
	if r.Header.Get("X-Forwarded-Proto") == "https" || r.TLS != nil {
		scheme = "https"
	}
	fwURL := scheme + "://" + host + "/api/devices/" + mac + "/ota/firmware.bin"

	resp := map[string]interface{}{"update": true, "url": fwURL}
	json.NewEncoder(w).Encode(resp) //nolint:errcheck
	log.Printf("[OTA] %s: check from fw=%s — update available", mac, currentFW)

	// Advance phase idle→triggered so otaOnStatusReceived can detect same-version re-flash
	otaMu.Lock()
	if otaInfo[mac] != nil && otaInfo[mac].Phase == "idle" {
		otaInfo[mac].Phase = "triggered"
		otaInfo[mac].PrevFw = currentFW
	}
	otaMu.Unlock()
}

func otaMacFromPath(path string) string {
	// /api/devices/{mac}/ota/... → mac
	after := strings.TrimPrefix(path, "/api/devices/")
	if idx := strings.Index(after, "/"); idx >= 0 {
		return strings.ToUpper(after[:idx])
	}
	return strings.ToUpper(after)
}
