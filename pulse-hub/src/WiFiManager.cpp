// =============================================================================
//  WiFiManager.cpp
//  Runs entirely on Core 0 via a FreeRTOS task so the main loop (Core 1) is
//  never blocked by WiFi scanning, association delays, or NTP requests.
//  AP is started once and never torn down; STA connects in the background.
// =============================================================================

#include "WiFiManager.h"
#include "Logger.h"
#include "Globals.h"
#include "Config.h"
#include "RTCManager.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <tcpip_adapter.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <TimeLib.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <Preferences.h>

// Task-local Preferences instance — avoids thread-safety crash with the
// global 'preferences' object used by MotorControl on Core 1.
static Preferences wifiPrefs;

// ---------------------------------------------------------------------------
//  AP / OTA passwords  (build-time defaults, overridable at deployment)
// ---------------------------------------------------------------------------

String getApPassword() {
    Preferences p;
    p.begin(NVS_APCFG_NS, true);
    String v = p.getString(NVS_KEY_AP_PASS, DEFAULT_AP_PASSWORD);
    p.end();
    return v.length() >= 8 ? v : String(DEFAULT_AP_PASSWORD);
}

String getOtaPassword() {
    Preferences p;
    p.begin(NVS_APCFG_NS, true);
    String v = p.getString(NVS_KEY_OTA_PASS, DEFAULT_OTA_PASSWORD);
    p.end();
    return v.length() ? v : String(DEFAULT_OTA_PASSWORD);
}

bool setApPassword(const String& pass) {
    if (pass.length() < 8) return false;   // WPA2 minimum
    Preferences p;
    p.begin(NVS_APCFG_NS, false);
    p.putString(NVS_KEY_AP_PASS, pass);
    p.end();
    Log(WARN, "[WiFi] AP password changed - restarting hotspot");
    if (isAPMode) {
        WiFi.softAPdisconnect(true);
        WiFi.softAP(DEFAULT_AP_SSID, pass.c_str());
    }
    return true;
}

bool setOtaPassword(const String& pass) {
    if (pass.length() < 8) return false;
    Preferences p;
    p.begin(NVS_APCFG_NS, false);
    p.putString(NVS_KEY_OTA_PASS, pass);
    p.end();
    Log(WARN, "[OTA] Password changed - effective after reboot; update --auth= to match");
    return true;
}

// ---------------------------------------------------------------------------
//  Internal state -- only touched inside the WiFi task (Core 0)
// ---------------------------------------------------------------------------

struct WifiNetwork {
    String ssid;
    String password;
};

static WifiNetwork   networks[MAX_WIFI_NETWORKS];
static int           networkCount     = 0;
static bool          staConnected     = false;
static bool          otaReady         = false;

// Reconnect state machine (priority-ordered, 3 attempts/SSID, 15-min cooldown)
static int           smTryIdx          = 0;
static int           smTryAttempts     = 0;
static unsigned long smNextActionMs    = 0;
static bool          smInCooldown      = false;
static unsigned long smCooldownUntilMs = 0;

// A totally empty async scan result (found<=0) has been observed to be
// intermittent in AP+STA mode — the manual /wifiscan blocking scan reliably
// finds the same networks the async pre-connect gate below sometimes misses
// entirely. Retry a few empty scans before believing "nothing in range" and
// paying the 15-minute cooldown for what may just have been a flaky scan.
#define WIFI_EMPTY_SCAN_MAX_RETRIES 3
static int smEmptyScanRetries = 0;
static bool          smScanPending     = false; // async scan in progress (shared radio resource)

// Manual scan requested by the web UI's "Scan for networks" button. Runs
// through this same task/radio instead of a raw WiFi.scanNetworks() call from
// HttpServer.cpp (Core 1) — two tasks driving the single ESP-IDF scan state
// concurrently is what caused "Scan failed or timed out": the manual blocking
// scan and this task's own async reconnect-gate scan would stomp on each
// other's scan-in-progress state. smScanPending is the single shared "radio
// busy scanning" flag both paths respect, so only one scan ever runs at a time.
static volatile bool smManualScanRequested = false;
static bool          smScanIsManual        = false;
static volatile bool smManualScanReady     = false;
static String        smManualScanJson      = "[]"; // guarded by wifiMutex

// NTP
static WiFiUDP       ntpUDP;
static NTPClient     ntpClient(ntpUDP, "pool.ntp.org", 19800, 3600000);
static bool          ntpStarted       = false;
static time_t        lastNtpSyncEpoch = 0;
static int32_t       lastNtpDriftSec  = 0;
static unsigned long lastNtpAutoSyncMs = 0;

// Mutex protects shared state read by main loop (Core 1)
static SemaphoreHandle_t wifiMutex = nullptr;

// Command queue: main task -> wifi task
typedef enum : uint8_t {
    WCMD_ADD_NETWORK,
    WCMD_REMOVE_NETWORK,
    WCMD_SET_PRIORITY,
    WCMD_SYNC_TIME,
    WCMD_MANUAL_SCAN,
} WifiCmd;

struct WifiCmdMsg {
    WifiCmd cmd;
    char    ssid[33];
    char    password[65];
    int     priority;
};
static QueueHandle_t cmdQueue = nullptr;

// Task handle
static TaskHandle_t wifiTaskHandle = nullptr;

// ---------------------------------------------------------------------------
//  NVS helpers (called from WiFi task only)
// ---------------------------------------------------------------------------

static void loadNetworks() {
    wifiPrefs.begin(NVS_WIFI_NS, true);
    String json = wifiPrefs.getString(NVS_KEY_WIFI_JSON, "[]");
    wifiPrefs.end();

    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;
    networkCount = 0;
    for (JsonObject obj : doc.as<JsonArray>()) {
        if (networkCount >= MAX_WIFI_NETWORKS) break;
        networks[networkCount].ssid     = obj["ssid"].as<String>();
        networks[networkCount].password = obj["pass"].as<String>();
        networkCount++;
    }
    Log(INFO, "[WiFi] Loaded " + String(networkCount) + " network(s)");
}

static void saveNetworks() {
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < networkCount; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["ssid"] = networks[i].ssid;
        obj["pass"] = networks[i].password;
    }
    String json;
    serializeJson(doc, json);
    wifiPrefs.begin(NVS_WIFI_NS, false);
    wifiPrefs.putString(NVS_KEY_WIFI_JSON, json);
    wifiPrefs.end();
}

// ---------------------------------------------------------------------------
//  NTP sync (called from WiFi task only)
// ---------------------------------------------------------------------------

static void doSynchronizeTime() {
    if (!staConnected) return;
    if (!ntpStarted) {
        ntpClient.begin();
        ntpStarted = true;
    }
    Log(INFO, "[NTP] Requesting time...");
    if (!ntpClient.forceUpdate()) {
        Log(WARN, "[NTP] forceUpdate failed");
        return;
    }
    time_t epoch = (time_t)ntpClient.getEpochTime();
    if (epoch < 1000000000UL) {
        Log(WARN, "[NTP] Invalid epoch");
        return;
    }
    time_t before      = time(nullptr);
    lastNtpDriftSec    = (int32_t)(epoch - before);
    lastNtpSyncEpoch   = epoch;
    lastNtpAutoSyncMs  = millis();

    wifiPrefs.begin(NVS_WIFI_NS, false);
    wifiPrefs.putUInt("ntp_epoch", (uint32_t)epoch);
    wifiPrefs.end();

    timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    setTime(epoch);

    if (ds3231Found) {
        rtc.adjust(DateTime((uint32_t)epoch));
    }
    Log(INFO, "[NTP] Synced. Drift=" + String(lastNtpDriftSec) + "s");
}

// ---------------------------------------------------------------------------
//  OTA setup (called from WiFi task only)
// ---------------------------------------------------------------------------

static void setupOTA() {
    if (otaReady) return;
    ArduinoOTA.setHostname("agripulse");
    ArduinoOTA.setPassword(getOtaPassword().c_str());
    ArduinoOTA.onStart([]() { Log(INFO, "[OTA] Start"); });
    ArduinoOTA.onEnd([]()   { Log(INFO, "[OTA] End");   });
    ArduinoOTA.onError([](ota_error_t e) { Log(ERROR, "[OTA] Error " + String(e)); });
    ArduinoOTA.begin();
    otaReady = true;
    Log(INFO, "[OTA] Ready");
}

// ---------------------------------------------------------------------------
//  mDNS — advertise agripulse.local + HTTP service
// ---------------------------------------------------------------------------
static bool mdnsStarted = false;

static void setupMDNS() {
    if (mdnsStarted) {
        MDNS.end();   // restart on reconnect to pick up new IP
    }
    if (MDNS.begin("agripulse")) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "fw", FW_VERSION);
        mdnsStarted = true;
        Log(INFO, "[mDNS] agripulse.local → " + WiFi.localIP().toString());
    } else {
        Log(WARN, "[mDNS] Failed to start");
    }
}

// ---------------------------------------------------------------------------
//  Command handlers (called from WiFi task only)
// ---------------------------------------------------------------------------

static void handleAddNetwork(const char* ssid, const char* password) {
    // Update password if already exists — never drop an active connection
    for (int i = 0; i < networkCount; i++) {
        if (networks[i].ssid == ssid) {
            networks[i].password = password;
            saveNetworks();
            Log(INFO, "[WiFi] Updated password for: " + String(ssid));
            // Correcting a password (e.g. after a scan-and-join typo) must
            // retry promptly too — without this, a wrong first attempt burns
            // through WIFI_MAX_ATTEMPTS_PER_NET and lands in the 15-minute
            // cooldown, and the corrected password then sits ignored for the
            // rest of that window because only the "brand new network" branch
            // below used to reset the state machine. Found 2026-08-07.
            if (WiFi.status() != WL_CONNECTED) {
                smTryIdx       = 0;
                smTryAttempts  = 0;
                smInCooldown   = false;
                smNextActionMs = millis() + 1000;
            }
            return;
        }
    }
    if (networkCount >= MAX_WIFI_NETWORKS) return;
    // Append at lowest priority; user can raise it via the up-arrow in the UI
    networks[networkCount].ssid     = ssid;
    networks[networkCount].password = password;
    networkCount++;
    saveNetworks();
    Log(INFO, "[WiFi] Added (priority " + String(networkCount) + "): " + String(ssid));
    // If not connected, restart state machine so it tries the new network too
    if (WiFi.status() != WL_CONNECTED) {
        smTryIdx       = 0;
        smTryAttempts  = 0;
        smInCooldown   = false;
        smNextActionMs = millis() + 1000;
    }
}

static void handleRemoveNetwork(const char* ssid) {
    for (int i = 0; i < networkCount; i++) {
        if (networks[i].ssid == ssid) {
            bool wasConn = (WiFi.status() == WL_CONNECTED && WiFi.SSID() == String(ssid));
            // Refuse to remove the network currently providing internet/remote
            // access, and refuse to remove the last remaining saved network —
            // either would strand the device with no way to reconnect and no
            // remote (app/web) access to fix it. The web/app UI already hides
            // the delete action for these cases; this is the authoritative
            // backstop in case a stale command slips through.
            if (wasConn) {
                Log(WARN, "[WiFi] Refused to remove active network: " + String(ssid));
                return;
            }
            if (networkCount <= 1) {
                Log(WARN, "[WiFi] Refused to remove last saved network: " + String(ssid));
                return;
            }
            for (int j = i; j < networkCount - 1; j++) networks[j] = networks[j+1];
            networkCount--;
            saveNetworks();
            Log(INFO, "[WiFi] Removed: " + String(ssid));
            return;
        }
    }
}

static void handleSetPriority(const char* ssid, int newPriority) {
    int from = -1;
    for (int i = 0; i < networkCount; i++) {
        if (networks[i].ssid == ssid) { from = i; break; }
    }
    if (from < 0) return;
    int to = constrain(newPriority - 1, 0, networkCount - 1);
    if (from == to) return;
    WifiNetwork tmp = networks[from];
    if (from > to) { for (int i = from; i > to; i--) networks[i] = networks[i-1]; }
    else           { for (int i = from; i < to; i++) networks[i] = networks[i+1]; }
    networks[to] = tmp;
    saveNetworks();
}

// Builds the same JSON shape the old blocking /wifiscan handler returned,
// from an already-completed scan result (found = WiFi.scanComplete() result,
// > 0 or 0; never call with WIFI_SCAN_RUNNING/WIFI_SCAN_FAILED).
static String buildScanResultJson(int found) {
    String json = "[";
    for (int i = 0; i < found; i++) {
        if (i > 0) json += ",";
        String s = WiFi.SSID(i);
        s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
        bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        json += "{\"ssid\":\"" + s + "\","
                "\"rssi\":"    + String(WiFi.RSSI(i)) + ","
                "\"ch\":"      + String(WiFi.channel(i)) + ","
                "\"open\":"    + (open ? "true" : "false") + "}";
    }
    json += "]";
    return json;
}

// ---------------------------------------------------------------------------
//  WiFi task -- runs on Core 0, never exits
// ---------------------------------------------------------------------------

static void wifiTask(void* /*param*/) {
    // AP setup
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);   // disable built-in 3s scan loop — we manage reconnects manually
    esp_wifi_set_country_code("IN", true);  // India: channels 1-13, prevents missing ch13 APs

    // STA modem sleep (WIFI_PS_MIN_MODEM, Arduino's default) periodically powers
    // the radio down between DTIM beacons to save energy. With only STA active
    // that's invisible; with AP+STA sharing one radio, the same sleep cycling
    // steals airtime from softAP client traffic, which is what made the HTTP UI
    // sluggish/unresponsive whenever a connection attempt (or the retry scan)
    // was in flight. This unit is mains-powered, so there is nothing to save.
    WiFi.setSleep(false);

    WiFi.softAP(DEFAULT_AP_SSID, getApPassword().c_str());
    vTaskDelay(pdMS_TO_TICKS(200));
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    Log(INFO, "[WiFi] AP up. IP=" + WiFi.softAPIP().toString());
    isAPMode = true;

    // Restore persisted NTP epoch
    wifiPrefs.begin(NVS_WIFI_NS, true);
    lastNtpSyncEpoch = (time_t)wifiPrefs.getUInt("ntp_epoch", 0);
    wifiPrefs.end();

    // If DS3231 lost power (battery dead/missing) initRTC() falls back to compile
    // time which is stale.  Restore the last-known-good NTP epoch from NVS so
    // the clock is at least as accurate as the previous session.
    if (lastNtpSyncEpoch > 1000000000UL && time(nullptr) < (time_t)lastNtpSyncEpoch) {
        timeval tv = { .tv_sec = (time_t)lastNtpSyncEpoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        setTime((time_t)lastNtpSyncEpoch);
        Log(INFO, "[WiFi] System time restored from NVS epoch (DS3231 battery lost)");
        if (ds3231Found) rtc.adjust(DateTime((uint32_t)lastNtpSyncEpoch));
    }

    // Delay STA start so AP beacons stabilise first
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Initialise state machine — first attempt fires on first loop tick
    smTryIdx       = 0;
    smTryAttempts  = 0;
    smInCooldown   = false;
    smNextActionMs = millis();

    bool prevConnected = false;

    for (;;) {
        // Process command queue
        WifiCmdMsg msg;
        while (xQueueReceive(cmdQueue, &msg, 0) == pdTRUE) {
            switch (msg.cmd) {
                case WCMD_ADD_NETWORK:    handleAddNetwork(msg.ssid, msg.password); break;
                case WCMD_REMOVE_NETWORK: handleRemoveNetwork(msg.ssid); break;
                case WCMD_SET_PRIORITY:   handleSetPriority(msg.ssid, msg.priority); break;
                case WCMD_SYNC_TIME:      doSynchronizeTime(); break;
                case WCMD_MANUAL_SCAN:    smManualScanRequested = true; smManualScanReady = false; break;
            }
        }

        // --- Manual scan (web UI "Scan for networks") ---------------------
        // Independent of connect state (works whether STA is connected or
        // not) and independent of the reconnect state machine below. Shares
        // smScanPending as the single "radio is scanning" gate so this task
        // never has two scans in flight against each other, and HttpServer.cpp
        // never touches WiFi.scanNetworks() directly (that cross-core call is
        // what caused "Scan failed or timed out" in the first place).
        if (smScanIsManual) {
            int found = WiFi.scanComplete();
            if (found != WIFI_SCAN_RUNNING) {
                String json = buildScanResultJson(found < 0 ? 0 : found);
                WiFi.scanDelete();
                xSemaphoreTake(wifiMutex, portMAX_DELAY);
                smManualScanJson = json;
                xSemaphoreGive(wifiMutex);
                smScanIsManual    = false;
                smScanPending     = false;
                smManualScanReady = true;
                Log(INFO, "[WiFi] Manual scan complete: " + String(found < 0 ? 0 : found) + " networks");
            }
        } else if (smManualScanRequested && !smScanPending) {
            smManualScanRequested = false;
            smScanPending  = true;
            smScanIsManual = true;
            WiFi.scanNetworks(true, true);
            Log(INFO, "[WiFi] Manual scan started (web UI request)");
        }

        bool nowConnected = (WiFi.status() == WL_CONNECTED);

        if (nowConnected && !prevConnected) {
            prevConnected     = true;
            staConnected      = true;
            isAPMode          = false;
            smTryIdx          = 0;
            smTryAttempts     = 0;
            smInCooldown      = false;
            xSemaphoreTake(wifiMutex, portMAX_DELAY);
            wifiSSID = WiFi.SSID();
            wifiRSSI = WiFi.RSSI();
            xSemaphoreGive(wifiMutex);
            Log(INFO, "[WiFi] STA connected. IP=" + WiFi.localIP().toString());
            setupOTA();
            setupMDNS();
            doSynchronizeTime();
        }

        if (!nowConnected && prevConnected) {
            prevConnected     = false;
            staConnected      = false;
            Log(WARN, "[WiFi] STA disconnected.");
            smTryIdx          = 0;
            smTryAttempts     = 0;
            smInCooldown      = false;
            smNextActionMs    = millis() + 5000; // brief pause before first reconnect
        }

        if (nowConnected) {
            static unsigned long lastRssiMs = 0;
            if (millis() - lastRssiMs >= 5000) {
                lastRssiMs = millis();
                xSemaphoreTake(wifiMutex, portMAX_DELAY);
                wifiRSSI = WiFi.RSSI();
                xSemaphoreGive(wifiMutex);
            }
            // Retry NTP every 30s until first sync succeeds, then hourly
            static unsigned long lastNtpRetryMs = 0;
            unsigned long ntpInterval = (lastNtpAutoSyncMs == 0)
                                        ? 30000UL : NTP_SYNC_INTERVAL_MS;
            if (millis() - lastNtpRetryMs >= ntpInterval) {
                lastNtpRetryMs = millis();
                doSynchronizeTime();
            }
        } else {
            // Priority-ordered reconnect: 3 attempts per SSID, then 15-min cooldown
            if (networkCount > 0) {
                if (smInCooldown) {
                    if (millis() >= smCooldownUntilMs) {
                        smInCooldown   = false;
                        smTryIdx       = 0;
                        smTryAttempts  = 0;
                        smNextActionMs = millis();
                        Log(INFO, "[WiFi] Cooldown ended, retrying all networks");
                    }
                } else if (smScanPending) {
                    // Async scan in progress — poll without blocking so AP stays alive
                    int found = WiFi.scanComplete();
                    if (found == WIFI_SCAN_RUNNING) {
                        // not done yet, check again next tick
                    } else {
                        smScanPending = false;
                        bool anySeen = false;
                        if (found <= 0) {
                            Log(WARN, "[WiFi] Scan found no networks");
                        } else {
                            smEmptyScanRetries = 0;   // real results this time
                            for (int s = 0; s < found; s++) {
                                for (int n = 0; n < networkCount; n++) {
                                    if (WiFi.SSID(s) == networks[n].ssid) anySeen = true;
                                }
                                Log(INFO, "[WiFi] Scan: " + WiFi.SSID(s)
                                          + " ch=" + String(WiFi.channel(s))
                                          + " RSSI=" + String(WiFi.RSSI(s)));
                            }
                        }
                        WiFi.scanDelete();

                        if (found <= 0 && smEmptyScanRetries < WIFI_EMPTY_SCAN_MAX_RETRIES) {
                            smEmptyScanRetries++;
                            Log(WARN, "[WiFi] Empty scan (" + String(smEmptyScanRetries) + "/"
                                      + String(WIFI_EMPTY_SCAN_MAX_RETRIES) + ") - retrying before giving up");
                            WiFi.scanNetworks(true, true);
                            smScanPending = true;
                        } else if (!anySeen) {
                            smEmptyScanRetries = 0;
                            // Associating with an absent SSID holds the shared radio for the
                            // whole attempt window, which makes the AP's web UI unreachable.
                            // Skip straight to cooldown rather than burning three of those.
                            smInCooldown      = true;
                            smCooldownUntilMs = millis() + WIFI_COOLDOWN_MS;
                            smTryIdx          = 0;
                            smTryAttempts     = 0;
                            Log(WARN, "[WiFi] No configured SSID in range - cooling down, AP stays responsive");
                        } else {
                            smEmptyScanRetries = 0;
                            smTryAttempts++;
                            Log(INFO, "[WiFi] Attempt " + String(smTryAttempts) + "/" + String(WIFI_MAX_ATTEMPTS_PER_NET)
                                      + " -> " + networks[smTryIdx].ssid);
                            WiFi.begin(networks[smTryIdx].ssid.c_str(), networks[smTryIdx].password.c_str());
                            smNextActionMs = millis() + WIFI_ATTEMPT_TIMEOUT_MS;
                        }
                    }
                } else if (millis() >= smNextActionMs) {
                    // Previous attempt window expired — log why it failed then advance
                    if (smTryAttempts > 0) {
                        wl_status_t ws = WiFi.status();
                        String reason;
                        if      (ws == WL_NO_SSID_AVAIL)  reason = "SSID not found (wrong name or 5GHz-only?)";
                        else if (ws == WL_CONNECT_FAILED)  reason = "Auth failed (wrong password?)";
                        else                               reason = "status=" + String((int)ws);
                        Log(WARN, "[WiFi] Attempt timed out -> " + networks[smTryIdx].ssid + " | " + reason);
                    }
                    // Advance if limit reached
                    if (smTryAttempts >= WIFI_MAX_ATTEMPTS_PER_NET) {
                        smTryIdx++;
                        smTryAttempts = 0;
                    }
                    if (smTryIdx >= networkCount) {
                        smInCooldown      = true;
                        smCooldownUntilMs = millis() + WIFI_COOLDOWN_MS;
                        smTryIdx          = 0;
                        Log(WARN, "[WiFi] All " + String(networkCount) + " SSID(s) failed. Cooling down 15 min.");
                    } else {
                        // Kick off async scan before first attempt of each round
                        if (smTryAttempts == 0 && smTryIdx == 0) {
                            WiFi.scanNetworks(true, true); // async=true — AP stays alive during scan
                            smScanPending = true;
                            Log(INFO, "[WiFi] Async scan started");
                            // smNextActionMs not updated here; scan completion drives next step
                        } else {
                            smTryAttempts++;
                            Log(INFO, "[WiFi] Attempt " + String(smTryAttempts) + "/" + String(WIFI_MAX_ATTEMPTS_PER_NET)
                                      + " -> " + networks[smTryIdx].ssid);
                            WiFi.begin(networks[smTryIdx].ssid.c_str(), networks[smTryIdx].password.c_str());
                            smNextActionMs = millis() + WIFI_ATTEMPT_TIMEOUT_MS;
                        }
                    }
                }
            }
        }

        if (otaReady) ArduinoOTA.handle();

        // Periodically save current epoch to NVS so that after a power cut
        // (dead DS3231 battery) the clock is restored to within ~5 min of
        // when power was lost, rather than to the last NTP sync time.
        static unsigned long lastEpochSaveMs = 0;
        if (lastNtpSyncEpoch > 0 && millis() - lastEpochSaveMs >= 300000UL) {
            lastEpochSaveMs = millis();
            time_t nowEpoch = time(nullptr);
            if (nowEpoch > (time_t)lastNtpSyncEpoch) {
                wifiPrefs.begin(NVS_WIFI_NS, false);
                wifiPrefs.putUInt("ntp_epoch", (uint32_t)nowEpoch);
                wifiPrefs.end();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------
//  Public API -- called from main loop (Core 1)
// ---------------------------------------------------------------------------

void initWiFi() {
    loadNetworks();

    wifiMutex = xSemaphoreCreateMutex();
    cmdQueue  = xQueueCreate(8, sizeof(WifiCmdMsg));

    xTaskCreatePinnedToCore(
        wifiTask,
        "wifiTask",
        8192,
        nullptr,
        2,
        &wifiTaskHandle,
        0   // Core 0
    );

    Log(INFO, "[WiFi] Task started on Core 0");
}

void checkWiFiConnection() {
    // No-op: everything runs in wifiTask on Core 0
}

void synchronizeTime() {
    if (cmdQueue == nullptr) return;
    WifiCmdMsg msg;
    msg.cmd = WCMD_SYNC_TIME;
    xQueueSend(cmdQueue, &msg, 0);
}

// Manual WiFi scan (web UI "Scan for networks"). Submit/poll pattern: this
// only queues the request and returns immediately — HttpServer.cpp never
// blocks its own task waiting on the radio, and never calls WiFi.scanNetworks()
// itself. See buildScanResultJson()/wifiTask() for where the scan actually runs.
void wifiRequestManualScan() {
    if (cmdQueue == nullptr) return;
    smManualScanReady = false;
    WifiCmdMsg msg;
    msg.cmd = WCMD_MANUAL_SCAN;
    xQueueSend(cmdQueue, &msg, 0);
}

bool wifiManualScanReady() {
    return smManualScanReady;
}

// Consumes the ready flag — call only after wifiManualScanReady() is true.
String wifiManualScanResultJson() {
    xSemaphoreTake(wifiMutex, portMAX_DELAY);
    String json = smManualScanJson;
    xSemaphoreGive(wifiMutex);
    smManualScanReady = false;
    return json;
}

bool hasNtpSynced() {
    return lastNtpSyncEpoch > 0;
}

int32_t getNtpDriftSeconds() {
    return lastNtpDriftSec;
}

uint32_t getNtpSyncAgeSeconds() {
    if (lastNtpSyncEpoch == 0) return 0;
    time_t now = time(nullptr);
    return (now > lastNtpSyncEpoch) ? (uint32_t)(now - lastNtpSyncEpoch) : 0;
}

String getFormattedTime() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    int hour12 = t.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = t.tm_hour < 12 ? "AM" : "PM";
    char buf[28];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s %02d-%02d-%04d",
             hour12, t.tm_min, t.tm_sec, ampm,
             t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    return String(buf);
}

bool addWifiNetwork(const String& ssid, const String& password) {
    if (cmdQueue == nullptr) return false;
    WifiCmdMsg msg;
    msg.cmd = WCMD_ADD_NETWORK;
    strncpy(msg.ssid,     ssid.c_str(),     sizeof(msg.ssid)     - 1); msg.ssid[sizeof(msg.ssid)-1]         = '\0';
    strncpy(msg.password, password.c_str(), sizeof(msg.password) - 1); msg.password[sizeof(msg.password)-1] = '\0';
    return xQueueSend(cmdQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool removeWifiNetwork(const String& ssid) {
    if (cmdQueue == nullptr) return false;
    WifiCmdMsg msg;
    msg.cmd = WCMD_REMOVE_NETWORK;
    strncpy(msg.ssid, ssid.c_str(), sizeof(msg.ssid) - 1); msg.ssid[sizeof(msg.ssid)-1] = '\0';
    return xQueueSend(cmdQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool setWifiPriority(const String& ssid, int newPriority) {
    if (cmdQueue == nullptr) return false;
    WifiCmdMsg msg;
    msg.cmd      = WCMD_SET_PRIORITY;
    msg.priority = newPriority;
    strncpy(msg.ssid, ssid.c_str(), sizeof(msg.ssid) - 1); msg.ssid[sizeof(msg.ssid)-1] = '\0';
    return xQueueSend(cmdQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}

void startAPMode() {
    isAPMode = true;
    WiFi.softAP(DEFAULT_AP_SSID, getApPassword().c_str());
}

void stopAPMode() {
    isAPMode = false;
    WiFi.softAPdisconnect(false);
}

String getStoredNetworksJson() {
    StaticJsonDocument<1536> doc;
    JsonArray arr = doc.createNestedArray("networks");

    if (wifiMutex) xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(50));
    int count = networkCount;
    WifiNetwork snap[MAX_WIFI_NETWORKS];
    for (int i = 0; i < count; i++) snap[i] = networks[i];
    if (wifiMutex) xSemaphoreGive(wifiMutex);

    String curSSID = WiFi.SSID();
    bool connected = (WiFi.status() == WL_CONNECTED);
    for (int i = 0; i < count; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["ssid"]      = snap[i].ssid;
        obj["priority"]  = i + 1;
        bool isConn = connected && (curSSID == snap[i].ssid);
        obj["connected"] = isConn;
        if (isConn) obj["ip"] = WiFi.localIP().toString();
    }

    wifi_sta_list_t stalist;
    tcpip_adapter_sta_list_t adapterlist;
    memset(&adapterlist, 0, sizeof(adapterlist));
    esp_wifi_ap_get_sta_list(&stalist);
    tcpip_adapter_get_sta_list(&stalist, &adapterlist);
    JsonArray clients = doc.createNestedArray("apClients");
    for (int i = 0; i < adapterlist.num; i++) {
        char mac[18], ip[16];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
            adapterlist.sta[i].mac[0], adapterlist.sta[i].mac[1],
            adapterlist.sta[i].mac[2], adapterlist.sta[i].mac[3],
            adapterlist.sta[i].mac[4], adapterlist.sta[i].mac[5]);
        uint32_t addr = adapterlist.sta[i].ip.addr;
        snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
            addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
        JsonObject cl = clients.createNestedObject();
        cl["mac"] = mac;
        cl["ip"]  = (addr != 0) ? ip : "...";
    }
    doc["apEnabled"] = (WiFi.getMode() & WIFI_AP) != 0;
    doc["apIP"]      = WiFi.softAPIP().toString();

    String out;
    serializeJson(doc, out);
    return out;
}
