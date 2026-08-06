package main

// github_poller.go — automatically polls GitHub Releases every 5 minutes for a
// new controller_firmware release.  When a newer version is found the firmware
// binary is downloaded and staged for every registered device so the ESP32's
// existing /api/ota/check poll picks it up without any manual upload step.
//
// Flow:
//   GitHub Releases (controller_firmware/vX.Y.Z)
//     → download firmware.bin asset
//       → stage for all registered devices  (otaDir/{mac}.bin)
//         → ESP32 /api/ota/check poll detects update
//           → ESP32 downloads, flashes, reboots

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"time"
)

const (
	ghReleasesURL  = "https://api.github.com/repos/nperiannan/TankMonitor/releases?per_page=20"
	ghPollInterval = 5 * time.Minute
)

// ghLastStagedVersion is the controller_firmware version most recently staged
// by the poller.  Persisted only in memory; resets on container restart so the
// first poll after a restart will always re-stage if a firmware file is already
// present on disk.
var ghLastStagedVersion string

// startGitHubPoller runs the polling loop in a background goroutine.
func startGitHubPoller() {
	go func() {
		// Small initial delay so MQTT and DB are fully ready before first poll.
		time.Sleep(30 * time.Second)
		for {
			if err := ghPollOnce(); err != nil {
				log.Printf("[GH-POLL] error: %v", err)
			}
			time.Sleep(ghPollInterval)
		}
	}()
}

// ghPollOnce fetches the latest controller_firmware release from GitHub and
// stages the firmware.bin for all registered devices if the version is newer
// than the last staged version.
func ghPollOnce() error {
	// ── 1. Fetch releases list ──────────────────────────────────────────────
	req, err := http.NewRequest(http.MethodGet, ghReleasesURL, nil)
	if err != nil {
		return fmt.Errorf("build request: %w", err)
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "TankMonitor-WebApp/"+webVersion)

	client := &http.Client{Timeout: 15 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("github request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusForbidden || resp.StatusCode == http.StatusTooManyRequests {
		return fmt.Errorf("github rate-limited (status %d)", resp.StatusCode)
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("github returned status %d", resp.StatusCode)
	}

	var releases []struct {
		TagName string `json:"tag_name"`
		Assets  []struct {
			Name               string `json:"name"`
			BrowserDownloadURL string `json:"browser_download_url"`
		} `json:"assets"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&releases); err != nil {
		return fmt.Errorf("decode releases: %w", err)
	}

	// ── 2. Find latest controller_firmware release ──────────────────────────
	var latestTag, assetURL string
	for _, r := range releases {
		if !strings.HasPrefix(r.TagName, "controller_firmware/") {
			continue
		}
		for _, a := range r.Assets {
			if a.Name == "firmware.bin" {
				latestTag = r.TagName
				assetURL = a.BrowserDownloadURL
				break
			}
		}
		if latestTag != "" {
			break // releases are newest-first; first match is latest
		}
	}

	if latestTag == "" {
		log.Printf("[GH-POLL] no controller_firmware release with firmware.bin found")
		return nil
	}

	// Strip "controller_firmware/v" prefix → bare semver e.g. "2.3.3"
	version := strings.TrimPrefix(latestTag, "controller_firmware/")
	version = strings.TrimPrefix(version, "v")

	if version == ghLastStagedVersion {
		log.Printf("[GH-POLL] controller_firmware v%s already staged — no action", version)
		return nil
	}

	log.Printf("[GH-POLL] new controller_firmware v%s found — downloading from GitHub...", version)

	// ── 3. Download firmware.bin ────────────────────────────────────────────
	binResp, err := client.Get(assetURL) //nolint:noctx
	if err != nil {
		return fmt.Errorf("download firmware: %w", err)
	}
	defer binResp.Body.Close()
	if binResp.StatusCode != http.StatusOK {
		return fmt.Errorf("firmware download returned status %d", binResp.StatusCode)
	}

	// Write to a temp file first so we don't corrupt a partially-staged file.
	if err := os.MkdirAll(otaDir, 0755); err != nil {
		return fmt.Errorf("mkdir otaDir: %w", err)
	}
	tmpPath := otaDir + "/_gh_download.tmp"
	tmp, err := os.Create(tmpPath)
	if err != nil {
		return fmt.Errorf("create temp file: %w", err)
	}
	written, err := io.Copy(tmp, binResp.Body)
	tmp.Close()
	if err != nil {
		os.Remove(tmpPath) //nolint:errcheck
		return fmt.Errorf("write firmware: %w", err)
	}
	log.Printf("[GH-POLL] downloaded firmware.bin (%d bytes)", written)

	// ── 4. Stage for every registered device ───────────────────────────────
	rows, err := db.Query(`SELECT mac FROM devices`)
	if err != nil {
		os.Remove(tmpPath) //nolint:errcheck
		return fmt.Errorf("query devices: %w", err)
	}
	var macs []string
	for rows.Next() {
		var mac string
		if rows.Scan(&mac) == nil {
			macs = append(macs, strings.ToUpper(mac))
		}
	}
	rows.Close()

	if len(macs) == 0 {
		os.Remove(tmpPath) //nolint:errcheck
		log.Printf("[GH-POLL] no registered devices — firmware downloaded but not staged")
		return nil
	}

	staged := 0
	for _, mac := range macs {
		dest := otaFilePath(mac)
		// Copy (not rename) so each device gets its own independent file.
		if err := ghCopyFile(tmpPath, dest); err != nil {
			log.Printf("[GH-POLL] stage %s: %v", mac, err)
			continue
		}
		otaMu.Lock()
		otaInfo[mac] = &OtaInfo{
			HasFirmware: true,
			Filename:    "firmware.bin",
			Size:        written,
			UploadedAt:  time.Now().UTC().Format(time.RFC3339),
			Phase:       "idle",
		}
		otaMu.Unlock()
		log.Printf("[GH-POLL] staged firmware.bin for %s", mac)
		staged++
	}
	os.Remove(tmpPath) //nolint:errcheck

	if staged > 0 {
		ghLastStagedVersion = version
		log.Printf("[GH-POLL] controller_firmware v%s staged for %d device(s) — ESP32 will pick up on next OTA poll", version, staged)
	}
	return nil
}

// ghCopyFile copies src to dst, creating dst if it doesn't exist.
func ghCopyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}
