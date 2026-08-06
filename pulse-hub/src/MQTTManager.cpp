#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <Update.h>

#include "MQTTManager.h"
#include "Config.h"
#include "Globals.h"
#include "Logger.h"
#include "MotorControl.h"
#include "LoRaManager.h"
#include "Scheduler.h"
#include "RTCManager.h"
#include "Buzzer.h"
#include "WiFiManager.h"
#include "History.h"
#include "Display.h"
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------
static WiFiClient   s_wifiClient;
static PubSubClient s_mqtt(s_wifiClient);

static char s_broker[64];
static int  s_port;
static char s_user[32];
static char s_pass[64];
static char s_topicStatus[64];
static char s_topicControl[64];
static char s_topicLogs[64];
static char s_topicWifi[64];
static char s_topicHeartbeat[64];
static char s_clientId[32];

static unsigned long s_lastPublishMs    = 0;
// s_lastLogPublishMs removed 2026-07-30 — logs are now published on-demand
// only (get_logs cmd / OTA events), not on a periodic timer (data-usage cut).
static unsigned long s_lastReconnectMs  = 0;

// Heartbeat round-trip health check state (see publishHeartbeatRequest() /
// "ping" cmd handling below). Detects a broken backend<->broker<->device
// command path even while status messages keep flowing normally.
static unsigned long s_lastHeartbeatMs   = 0;
static uint32_t      s_hbSeq             = 0;
static uint32_t      s_hbAwaitingSeq     = 0;   // 0 = no request outstanding
static unsigned long s_hbSentAtMs        = 0;

static int  s_portFallback    = MQTT_PORT_FALLBACK;
static int  s_connectFailures = 0;            // consecutive failures on current port
static bool s_usingFallback   = false;        // true when using fallback port

static unsigned long s_lastOtaPollMs = 0;     // HTTP OTA poll timer

// MQTT watchdog: tracks the last time the client was actually connected, so
// mqttLoop() can force a reboot if it's been disconnected too long (covers
// both "WiFi up but broker/internet unreachable" and "WiFi itself dropped",
// since it's checked before the WiFi-status early-return below).
static unsigned long s_lastMqttSuccessMs = 0;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static void loadConfig() {
    Preferences prefs;
    prefs.begin(MQTT_NVS_NS, true);
    String broker = prefs.getString("broker", MQTT_BROKER_DEFAULT);
    s_port        = prefs.getInt   ("port",   MQTT_PORT_DEFAULT);
    String user   = prefs.getString("user",   MQTT_USER_DEFAULT);
    String pass   = prefs.getString("pass",   MQTT_PASS_DEFAULT);
    prefs.end();

    strncpy(s_broker, broker.c_str(), sizeof(s_broker) - 1);
    strncpy(s_user,   user.c_str(),   sizeof(s_user)   - 1);
    strncpy(s_pass,   pass.c_str(),   sizeof(s_pass)   - 1);
    // Topics are MAC-based and are set in buildTopicsFromMAC() — called from mqttConnect()
}

static void seedDefaultsIfEmpty() {
    Preferences prefs;
    prefs.begin(MQTT_NVS_NS, false);
    // Migrate old LAN IP to freemyip domain if still stored
    if (prefs.getString("broker", "") == "192.168.0.102") {
        prefs.putString("broker", MQTT_BROKER_DEFAULT);
        Log(INFO, "[MQTT] Migrated broker from LAN IP to " MQTT_BROKER_DEFAULT);
    }
    // Seed remaining keys only if not already set
    if (!prefs.isKey("broker"))   prefs.putString("broker",   MQTT_BROKER_DEFAULT);
    if (!prefs.isKey("port"))     prefs.putInt   ("port",     MQTT_PORT_DEFAULT);
    if (!prefs.isKey("user"))     prefs.putString("user",     MQTT_USER_DEFAULT);
    if (!prefs.isKey("pass"))     prefs.putString("pass",     MQTT_PASS_DEFAULT);
    if (!prefs.isKey("location")) prefs.putString("location", MQTT_LOCATION_DEFAULT);
    prefs.end();
    Log(INFO, "[MQTT] Config seeded");
}

// ---------------------------------------------------------------------------
// Forward declarations
static void publishMQTTLogs();

// ---------------------------------------------------------------------------
// Build MQTT topics and client ID from the hardware MAC address.
// Called from mqttConnect() when WiFi is guaranteed to be up.
// Topics: tm/{AA:BB:CC:DD:EE:FF}/status|control|logs
// Client: esp32_{last6hex}  e.g. esp32_DDEEFF
// ---------------------------------------------------------------------------
static void buildTopicsFromMAC() {
    String macStr    = WiFi.macAddress();   // "AA:BB:CC:DD:EE:FF"
    String macNoDash = macStr;
    macNoDash.replace(":", "");             // "AABBCCDDEEFF"

    snprintf(s_topicStatus,    sizeof(s_topicStatus),    "tm/%s/status",  macStr.c_str());
    snprintf(s_topicControl,   sizeof(s_topicControl),   "tm/%s/control", macStr.c_str());
    snprintf(s_topicLogs,      sizeof(s_topicLogs),      "tm/%s/logs",    macStr.c_str());
    snprintf(s_topicWifi,      sizeof(s_topicWifi),      "tm/%s/wifi",    macStr.c_str());
    snprintf(s_topicHeartbeat, sizeof(s_topicHeartbeat), "tm/%s/hb",      macStr.c_str());
    // Use last 6 hex chars of MAC as a short unique suffix
    snprintf(s_clientId,     sizeof(s_clientId),     "esp32_%s",      macNoDash.substring(6).c_str());
}

// Pending command queue — executed from loop(), not from callback stack
// ---------------------------------------------------------------------------
struct PendingCmd {
    char msg[128];
    bool active;
};
static PendingCmd s_pending = {"", false};

// ---------------------------------------------------------------------------
// MQTT callback — only copies payload into queue, returns immediately
// ---------------------------------------------------------------------------
static void onMessage(char* topic, byte* payload, unsigned int len) {
    if (len == 0 || len > 127) return;
    if (s_pending.active) return;  // drop if previous not yet consumed
    memcpy(s_pending.msg, payload, len);
    s_pending.msg[len] = '\0';
    s_pending.active   = true;
}

// ---------------------------------------------------------------------------
// Process one queued command — called from mqttLoop() inside loop()
// ---------------------------------------------------------------------------
static void processPendingMQTT() {
    if (!s_pending.active) return;
    char msg[128];
    strncpy(msg, s_pending.msg, sizeof(msg));
    s_pending.active = false;   // mark consumed before processing

    Log(INFO, "[MQTT] Control rx: " + String(msg));

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, msg) != DeserializationError::Ok) {
        Log(WARN, "[MQTT] Bad JSON: " + String(msg));
        return;
    }
    const char* cmd = doc["cmd"] | "";

    // Motor on/off commands publish an immediate status update right after
    // being processed (instead of waiting for the next periodic tick, up to
    // MQTT_PUBLISH_MS later) so the app's ack detection is near-instant —
    // same pattern already used below for OTA/reboot commands.
    if      (strcmp(cmd, "oh_on")  == 0) { turnOnOHMotor(REASON_MANUAL_APP);  publishMQTTStatus(); }
    else if (strcmp(cmd, "oh_off") == 0) { turnOffOHMotor(REASON_MANUAL_APP); publishMQTTStatus(); }
    else if (strcmp(cmd, "ug_on")  == 0) { turnOnUGMotor(REASON_MANUAL_APP);  publishMQTTStatus(); }
    else if (strcmp(cmd, "ug_off") == 0) { turnOffUGMotor(REASON_MANUAL_APP); publishMQTTStatus(); }
    else if (strcmp(cmd, "ping") == 0) {
        // Reply to the backend's heartbeat ping — completes the round trip
        // started by publishHeartbeatRequest() below. Acked immediately on
        // the /hb topic (not /control) so it can't be confused with a normal
        // command echo.
        uint32_t seq = doc["seq"] | 0;
        char ack[48];
        snprintf(ack, sizeof(ack), "{\"mac\":\"%s\",\"ack_seq\":%u}", WiFi.macAddress().c_str(), (unsigned)seq);
        s_mqtt.publish(s_topicHeartbeat, ack, false);
        if (seq != 0 && seq == s_hbAwaitingSeq) {
            Log(INFO, "[HB] ping acked seq=" + String(seq) + " rtt=" + String(millis() - s_hbSentAtMs) + "ms");
            s_hbAwaitingSeq = 0;
        }
    }
    else if (strcmp(cmd, "sched_add") == 0) {
        int slot = -1;
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            if (!schedules[i].enabled) { slot = i; break; }
        }
        if (slot < 0) {
            Log(WARN, "[MQTT] sched_add: no free slots");
        } else {
            schedules[slot].enabled   = true;
            schedules[slot].motorType = (uint8_t)(doc["motor"] | 0);
            schedules[slot].time      = doc["time"]     | "00:00";
            schedules[slot].duration  = (uint16_t)(doc["duration"] | 2);
            schedules[slot].isRunning = false;
            schedules[slot].startTime = 0;
            schedules[slot].preBuzzing = false;
            saveSchedules();
            Log(INFO, "[MQTT] sched_add slot=" + String(slot) + " " + schedules[slot].time);
        }
    }
    else if (strcmp(cmd, "sched_remove") == 0) {
        int idx = doc["index"] | -1;
        if (idx >= 0 && idx < MAX_SCHEDULES) {
            schedules[idx].enabled   = false;
            schedules[idx].isRunning = false;
            schedules[idx].time      = "00:00";
            schedules[idx].preBuzzing = false;
            saveSchedules();
            Log(INFO, "[MQTT] sched_remove index=" + String(idx));
        } else {
            Log(WARN, "[MQTT] sched_remove: bad index");
        }
    }
    else if (strcmp(cmd, "sched_clear") == 0) {
        clearAllSchedules();
        Log(INFO, "[MQTT] sched_clear");
    }
    else if (strcmp(cmd, "set_setting") == 0) {
        const char* key = doc["key"] | "";
        bool changed = true;
        if      (strcmp(key, "oh_disp_only") == 0) ohDisplayOnly      = doc["value"] | false;
        else if (strcmp(key, "ug_disp_only") == 0) ugDisplayOnly      = doc["value"] | false;
        else if (strcmp(key, "ug_ignore")    == 0) ugIgnoreForOH      = doc["value"] | false;
        else if (strcmp(key, "buzzer_delay") == 0) buzzerDelayEnabled = doc["value"] | false;
        else if (strcmp(key, "manual_auto_stop") == 0) manualAutoStop = doc["value"] | true;
        else if (strcmp(key, "oh_start_level") == 0) {
            uint8_t v = doc["value"] | 1;
            if (v >= TANK_STATE_EMPTY && v <= TANK_STATE_HALF) ohStartLevel = (TankState)v;
        }
        else if (strcmp(key, "oh_stop_level") == 0) {
            uint8_t v = doc["value"] | 4;
            if (v >= TANK_STATE_LOW && v <= TANK_STATE_FULL) ohStopLevel = (TankState)v;
        }
        else if (strcmp(key, "oh_max_run_min") == 0) {
            int v = doc["value"] | 20;
            if (v >= 5 && v <= 60) ohMaxRunMin = (uint8_t)v;
        }
        else if (strcmp(key, "mqtt_watchdog_min") == 0) {
            int v = doc["value"] | MQTT_WATCHDOG_DEFAULT_MIN;
            if (v >= MQTT_WATCHDOG_MIN_MIN && v <= MQTT_WATCHDOG_MAX_MIN) mqttWatchdogMin = (uint8_t)v;
        }
        else changed = false;
        if (ohStopLevel <= ohStartLevel) { ohStartLevel = TANK_STATE_EMPTY; ohStopLevel = TANK_STATE_FULL; }
        if (changed) { saveMotorConfig(); Log(INFO, "[MQTT] set_setting " + String(key)); }
        else          Log(WARN, "[MQTT] set_setting unknown key: " + String(key));
    }
    else if (strcmp(cmd, "set_lcd_mode") == 0) {
        const char* mode = doc["mode"] | "";
        uint8_t newMode = LCD_BL_AUTO;
        if      (strcmp(mode, "always_on")  == 0) newMode = LCD_BL_ALWAYS_ON;
        else if (strcmp(mode, "always_off") == 0) newMode = LCD_BL_ALWAYS_OFF;
        lcdBacklightMode = newMode;
        saveMotorConfig();
        applyBacklightMode();
        Log(INFO, "[MQTT] set_lcd_mode=" + String(mode));
    }
    else if (strcmp(cmd, "sync_ntp") == 0) {
        synchronizeTime();
        Log(INFO, "[MQTT] sync_ntp triggered");
    }
    else if (strcmp(cmd, "get_logs") == 0) {
        publishMQTTLogs();
        Log(INFO, "[MQTT] get_logs: log snapshot published");
    }
    else if (strcmp(cmd, "sync") == 0) {
        // Manual "pull to refresh" — app open / RF icon tap / sync button.
        // Full status is otherwise event-driven (motor/tank change) plus a
        // slow keep-alive tick, so give the app an immediate on-demand
        // snapshot instead of waiting for the next tick.
        publishMQTTStatus();
        Log(INFO, "[MQTT] sync: status published on demand");
    }
    else if (strcmp(cmd, "ota_start") == 0) {
        const char* url = doc["url"] | "";
        if (strlen(url) == 0) {
            Log(WARN, "[MQTT] ota_start: no url provided");
        } else {
            Log(INFO, "[MQTT] OTA command received — downloading firmware from " + String(url));
            publishMQTTLogs();   // immediate log ack so backend/app knows ESP32 received the command
            publishMQTTStatus(); // status update triggers ack_received phase on backend

            HTTPClient http;
            http.begin(url);
            http.setTimeout(30000);
            int httpCode = http.GET();
            if (httpCode != HTTP_CODE_OK) {
                Log(WARN, "[MQTT] OTA HTTP error: " + String(httpCode));
                http.end();
            } else {
                int contentLen = http.getSize();
                Log(INFO, "[MQTT] OTA firmware size: " + String(contentLen));

                if (!Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
                    Log(WARN, "[MQTT] OTA Update.begin failed: " + String(Update.errorString()));
                    http.end();
                } else {
                    WiFiClient* stream = http.getStreamPtr();
                    size_t written = Update.writeStream(*stream);
                    http.end();

                    if (Update.end(true)) {
                        Log(INFO, "[MQTT] OTA success — " + String(written) + " bytes written, rebooting…");
                        publishMQTTStatus();
                        delay(500);
                        esp_restart();
                    } else {
                        Log(WARN, "[MQTT] OTA Update.end failed: " + String(Update.errorString()));
                    }
                }
            }
        }
    }
    else if (strcmp(cmd, "ota_rollback") == 0) {
        Log(INFO, "[MQTT] OTA rollback requested");
        const esp_partition_t* running  = esp_ota_get_running_partition();
        const esp_partition_t* previous = esp_ota_get_next_update_partition(NULL);
        if (previous == NULL || previous == running) {
            Log(WARN, "[MQTT] OTA rollback: no alternate partition found");
        } else {
            esp_err_t err = esp_ota_set_boot_partition(previous);
            if (err == ESP_OK) {
                Log(INFO, "[MQTT] OTA rollback: boot set to previous partition, rebooting…");
                publishMQTTStatus();
                delay(500);
                esp_restart();
            } else {
                Log(WARN, "[MQTT] OTA rollback failed: " + String(esp_err_to_name(err)));
            }
        }
    }
    else if (strcmp(cmd, "set_mqtt_creds") == 0) {
        const char* newPass = doc["pass"] | "";
        const char* newUser = doc["user"] | "";
        if (strlen(newPass) == 0) {
            Log(WARN, "[MQTT] set_mqtt_creds: empty pass, ignoring");
        } else {
            Preferences prefs;
            prefs.begin(MQTT_NVS_NS, false);
            prefs.putString("pass", newPass);
            if (strlen(newUser) > 0) prefs.putString("user", newUser);
            prefs.end();
            loadConfig();   // reload s_pass / s_user into memory
            Log(INFO, "[MQTT] set_mqtt_creds: credentials saved, reconnecting with new creds…");
            s_mqtt.disconnect();  // mqttLoop() will reconnect using new NVS creds
        }
    }
    else if (strcmp(cmd, "set_log_level") == 0) {
        const char* level = doc["level"] | "info";
        if (strcmp(level, "debug") == 0) {
            setLogLevel(DEBUG);
            Log(INFO, "[MQTT] Log level set to DEBUG");
        } else {
            setLogLevel(INFO);
            Log(INFO, "[MQTT] Log level set to INFO");
        }
    }
    else if (strcmp(cmd, "reboot") == 0) {
        Log(INFO, "[MQTT] reboot commanded");
        publishMQTTStatus();
        delay(300);
        esp_restart();
    }
    // ── WiFi management commands ──────────────────────────────────────────
    else if (strcmp(cmd, "wifi_list") == 0) {
        String json = getStoredNetworksJson();
        s_mqtt.publish(s_topicWifi, ("{\"type\":\"wifi_list\",\"data\":" + json + "}").c_str(), false);
        Log(INFO, "[MQTT] wifi_list published");
    }
    else if (strcmp(cmd, "wifi_scan") == 0) {
        int found = WiFi.scanNetworks(false, true);
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
        WiFi.scanDelete();
        s_mqtt.publish(s_topicWifi, ("{\"type\":\"wifi_scan\",\"data\":" + json + "}").c_str(), false);
        Log(INFO, "[MQTT] wifi_scan published (" + String(found) + " networks)");
    }
    else if (strcmp(cmd, "wifi_add") == 0) {
        const char* ssid = doc["ssid"] | "";
        const char* pass = doc["pass"] | "";
        if (strlen(ssid) == 0) {
            Log(WARN, "[MQTT] wifi_add: empty SSID");
        } else {
            addWifiNetwork(String(ssid), String(pass));
            Log(INFO, "[MQTT] wifi_add: " + String(ssid));
            // Publish updated list
            String json = getStoredNetworksJson();
            s_mqtt.publish(s_topicWifi, ("{\"type\":\"wifi_list\",\"data\":" + json + "}").c_str(), false);
        }
    }
    else if (strcmp(cmd, "wifi_delete") == 0) {
        const char* ssid = doc["ssid"] | "";
        if (strlen(ssid) == 0) {
            Log(WARN, "[MQTT] wifi_delete: empty SSID");
        } else {
            removeWifiNetwork(String(ssid));
            Log(INFO, "[MQTT] wifi_delete: " + String(ssid));
            String json = getStoredNetworksJson();
            s_mqtt.publish(s_topicWifi, ("{\"type\":\"wifi_list\",\"data\":" + json + "}").c_str(), false);
        }
    }
    else if (strcmp(cmd, "wifi_set_priority") == 0) {
        const char* ssid = doc["ssid"] | "";
        int pri = doc["priority"] | 0;
        if (strlen(ssid) == 0 || pri < 1) {
            Log(WARN, "[MQTT] wifi_set_priority: bad args");
        } else {
            setWifiPriority(String(ssid), pri);
            Log(INFO, "[MQTT] wifi_set_priority: " + String(ssid) + " → " + String(pri));
            String json = getStoredNetworksJson();
            s_mqtt.publish(s_topicWifi, ("{\"type\":\"wifi_list\",\"data\":" + json + "}").c_str(), false);
        }
    }
    // ── Event history commands ─────────────────────────────────────────────
    else if (strcmp(cmd, "history_list") == 0) {
        String json = getHistoryJson(100);
        s_mqtt.publish(s_topicWifi, ("{\"type\":\"history_list\",\"data\":" + json + "}").c_str(), false);
        Log(INFO, "[MQTT] history_list published");
    }
    else if (strcmp(cmd, "history_clear") == 0) {
        clearHistory();
        s_mqtt.publish(s_topicWifi, "{\"type\":\"history_list\",\"data\":{\"count\":0,\"records\":[]}}", false);
        Log(INFO, "[MQTT] history_clear done");
    }
    else {
        Log(WARN, "[MQTT] Unknown cmd: " + String(cmd));
    }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
static bool mqttConnect() {
    if (WiFi.status() != WL_CONNECTED) return false;

    // Build topics from MAC now that WiFi is up (MAC is stable hardware address)
    buildTopicsFromMAC();

    int port = s_usingFallback ? s_portFallback : s_port;
    s_mqtt.setServer(s_broker, port);

    Log(INFO, "[MQTT] Connecting to " + String(s_broker) + ":" + String(port)
             + " as " + String(s_clientId)
             + (s_usingFallback ? " (fallback)" : "") + " ...");

    if (s_mqtt.connect(s_clientId, s_user, s_pass)) {
        Log(INFO, "[MQTT] Connected on port " + String(port) + ". Subscribing " + String(s_topicControl));
        s_mqtt.subscribe(s_topicControl, 1);
        s_connectFailures = 0;   // reset on success
        publishMQTTStatus();
        return true;
    }

    s_connectFailures++;
    Log(WARN, "[MQTT] Failed on port " + String(port) + ", rc=" + String(s_mqtt.state())
             + " (fail #" + String(s_connectFailures) + ")");

    // Switch to the other port after MQTT_PORT_FAIL_LIMIT consecutive failures
    if (s_connectFailures >= MQTT_PORT_FAIL_LIMIT) {
        s_usingFallback = !s_usingFallback;
        s_connectFailures = 0;
        int nextPort = s_usingFallback ? s_portFallback : s_port;
        Log(INFO, "[MQTT] Switching to port " + String(nextPort));
    }
    return false;
}

// ---------------------------------------------------------------------------
// HTTP OTA poll — checks server for staged firmware independent of MQTT.
// Uses port 1880 (HTTP) which ISPs don't block, unlike MQTT 1883.
// ---------------------------------------------------------------------------
static void otaPollHTTP() {
    if (WiFi.status() != WL_CONNECTED || isAPMode) return;

    String mac = WiFi.macAddress();
    String url = String("http://") + s_broker + ":" + String(OTA_POLL_PORT) +
                 "/api/devices/" + mac + "/ota/check?fw=" + FW_VERSION;

    HTTPClient http;
    http.begin(url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;

    bool hasUpdate = doc["update"] | false;
    if (!hasUpdate) return;

    const char* fwUrl = doc["url"] | "";
    if (strlen(fwUrl) == 0) return;

    Log(INFO, "[OTA-POLL] Update available — downloading from " + String(fwUrl));
    publishMQTTLogs();

    HTTPClient dl;
    dl.begin(fwUrl);
    dl.setTimeout(30000);
    int dlCode = dl.GET();
    if (dlCode != HTTP_CODE_OK) {
        Log(WARN, "[OTA-POLL] Download HTTP error: " + String(dlCode));
        dl.end();
        return;
    }
    int contentLen = dl.getSize();
    Log(INFO, "[OTA-POLL] Firmware size: " + String(contentLen));

    if (!Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
        Log(WARN, "[OTA-POLL] Update.begin failed: " + String(Update.errorString()));
        dl.end();
        return;
    }
    WiFiClient* stream = dl.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    dl.end();

    if (Update.end(true)) {
        Log(INFO, "[OTA-POLL] Success — " + String(written) + " bytes, rebooting…");
        publishMQTTStatus();
        delay(500);
        esp_restart();
    } else {
        Log(WARN, "[OTA-POLL] Update.end failed: " + String(Update.errorString()));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void initMQTT() {
    seedDefaultsIfEmpty();
    loadConfig();

    s_mqtt.setServer(s_broker, s_port);
    s_mqtt.setCallback(onMessage);
    s_mqtt.setKeepAlive(60);
    s_mqtt.setSocketTimeout(10);
    s_mqtt.setBufferSize(4096);  // large enough for status + log payloads

    s_lastMqttSuccessMs = millis();  // start the watchdog clock from boot

    Log(INFO, "[MQTT] Init. Broker=" + String(s_broker) + ":" + String(s_port) + " (topics built from MAC on first connect)");
}

// ---------------------------------------------------------------------------
// Heartbeat — controller-initiated round-trip health check.
// Publishes a small request on tm/{mac}/hb; the backend replies with a
// "ping" control command (handled in processPendingMQTT() above), which we
// ack straight back on the same /hb topic. A completed round trip proves the
// full backend<->broker<->device command path is alive, not just that this
// device is publishing status. See web/backend/heartbeat.go for the backend
// side and CHANGELOG.md for the 2026-07-19 incident that motivated this.
// ---------------------------------------------------------------------------
static void publishHeartbeatRequest() {
    if (!s_mqtt.connected()) return;

    // Warn locally (visible via get_logs) if the previous request never got
    // a "ping" back within the timeout — mirrors the backend-side check in
    // checkHeartbeatTimeouts() so either end can catch a broken path.
    if (s_hbAwaitingSeq != 0 && (millis() - s_hbSentAtMs) > MQTT_HEARTBEAT_ACK_TIMEOUT_MS) {
        Log(WARN, "[HB] no ping received for seq=" + String(s_hbAwaitingSeq) + " within " + String(MQTT_HEARTBEAT_ACK_TIMEOUT_MS / 1000) + "s");
        s_hbAwaitingSeq = 0;
    }

    s_hbSeq++;
    s_hbAwaitingSeq = s_hbSeq;
    s_hbSentAtMs    = millis();

    char req[48];
    snprintf(req, sizeof(req), "{\"mac\":\"%s\",\"seq\":%u}", WiFi.macAddress().c_str(), (unsigned)s_hbSeq);
    s_mqtt.publish(s_topicHeartbeat, req, false);
}

void mqttLoop() {
    // Watchdog — reboot if MQTT has been disconnected for too long. Checked
    // BEFORE the WiFi/AP-mode early-return below so it covers a dropped WiFi
    // link too, not just "WiFi up but broker unreachable". Skipped in AP
    // (initial setup) mode so a user configuring WiFi isn't rebooted mid-setup.
    if (!isAPMode) {
        if (s_mqtt.connected()) {
            s_lastMqttSuccessMs = millis();
        } else {
            unsigned long downMs  = millis() - s_lastMqttSuccessMs;
            unsigned long limitMs = (unsigned long)mqttWatchdogMin * 60000UL;
            if (downMs >= limitMs) {
                Log(WARN, "[MQTT] Watchdog: disconnected " + String(downMs / 60000) + "min (limit "
                          + String(mqttWatchdogMin) + "min) — restarting");
                delay(200); // let the log line publish/flush before reboot
                esp_restart();
            }
        }
    }

    if (WiFi.status() != WL_CONNECTED || isAPMode) return;

    // HTTP OTA poll — runs regardless of MQTT connection state
    unsigned long nowOta = millis();
    if (nowOta - s_lastOtaPollMs >= OTA_POLL_INTERVAL_MS) {
        s_lastOtaPollMs = nowOta;
        otaPollHTTP();
    }

    if (!s_mqtt.connected()) {
        unsigned long now = millis();
        if (now - s_lastReconnectMs >= MQTT_RECONNECT_MS) {
            s_lastReconnectMs = now;
            mqttConnect();
        }
        return;
    }

    s_mqtt.loop();

    // Process any command queued by the callback (runs on loop() stack, not callback stack)
    processPendingMQTT();

    unsigned long now = millis();
    if (now - s_lastPublishMs >= MQTT_PUBLISH_MS) {
        s_lastPublishMs = now;
        publishMQTTStatus();
    }

    // Logs are published on-demand only (get_logs cmd / OTA events) — no
    // periodic auto-publish, since resending the same ~30-entry snapshot
    // every 60 s regardless of activity was the single biggest contributor
    // to controller MQTT data usage (2026-07-30 data-usage cut).

    // Heartbeat round-trip health check — every MQTT_HEARTBEAT_MS
    if (now - s_lastHeartbeatMs >= MQTT_HEARTBEAT_MS) {
        s_lastHeartbeatMs = now;
        publishHeartbeatRequest();
    }
}

void reloadMQTTConfig() {
    loadConfig();
    s_mqtt.disconnect();   // mqttLoop() will reconnect with new settings
    s_usingFallback  = false;
    s_connectFailures = 0;
    Log(INFO, "[MQTT] Config reloaded — broker=" + String(s_broker) + ":" + String(s_port));
}

const char* getMQTTBroker() { return s_broker; }
int         getMQTTPort()   { return s_port; }
bool        isMQTTConnected() { return s_mqtt.connected(); }

void publishMQTTStatus() {
    if (!s_mqtt.connected()) return;

    // Current time
    struct tm ti;
    char timeStr[12] = "??:??:??";
    if (getLocalTime(&ti)) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    }

    // Build compact schedules JSON array (enabled entries only)
    char schedJson[400] = "[";
    bool firstSched = true;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!schedules[i].enabled) continue;
        char entry[96];
        snprintf(entry, sizeof(entry), "%s{\"i\":%d,\"m\":\"%s\",\"t\":\"%s\",\"d\":%u,\"on\":%s}",
            firstSched ? "" : ",",
            i,
            schedules[i].motorType == 0 ? "OH" : "UG",
            schedules[i].time.c_str(),
            schedules[i].duration,
            schedules[i].isRunning ? "true" : "false");
        if (strlen(schedJson) + strlen(entry) + 2 < sizeof(schedJson)) {
            strcat(schedJson, entry);
            firstSched = false;
        }
    }
    strcat(schedJson, "]");

    // MAC address (colon-separated, uppercase) for device identity
    String macStr = WiFi.macAddress();
    String ipStr  = WiFi.localIP().toString();

    // Compute lastLoraReceived string
    char loraLastStr[32];
    if (lastLoraReceivedTime > 0) {
        snprintf(loraLastStr, sizeof(loraLastStr), "%lus ago", (millis() - lastLoraReceivedTime) / 1000UL);
    } else {
        strcpy(loraLastStr, "Never");
    }

    char payload[1800];
    snprintf(payload, sizeof(payload),
        "{\"mac\":\"%s\",\"ip\":\"%s\",\"device_type\":\"tank_monitor\","
        "\"oh_state\":\"%s\",\"ug_state\":\"%s\","
        "\"oh_last_known\":\"%s\","
        "\"oh_motor\":%s,\"ug_motor\":%s,"
        "\"lora_ok\":%s,\"tx_lost\":%s,\"wifi_rssi\":%d,\"wifi_ssid\":\"%s\","
        "\"loraRSSI\":%.1f,\"loraSNR\":%.1f,\"lastLoraReceived\":\"%s\","
        "\"uptime_s\":%lu,\"fw\":\"%s\","
        "\"time\":\"%s\","
        "\"oh_disp_only\":%s,\"ug_disp_only\":%s,"
        "\"ug_ignore\":%s,\"buzzer_delay\":%s,\"manual_auto_stop\":%s,"
        "\"lcd_bl_mode\":%u,"
        "\"oh_start_level\":%u,\"oh_stop_level\":%u,\"oh_max_run_min\":%u,\"mqtt_watchdog_min\":%u,"
        "\"log_level\":\"%s\","
        "\"buzzer_active\":%s,"
        "\"oh_buzzer\":%s,\"ug_buzzer\":%s,"
        "\"oh_cd\":%d,\"ug_cd\":%d,"
        "\"oh_rsn\":%u,\"ug_rsn\":%u,"
        "\"oh_rej\":%u,\"ug_rej\":%u,"
        "\"tx_fw\":\"%s\","
        "\"schedules\":%s}",
        macStr.c_str(),
        ipStr.c_str(),
        tankStateStr(ohTankState),
        tankStateStr(ugTankState),
        tankStateStr(ohLastKnownState),
        ohMotorRunning ? "true" : "false",
        ugMotorRunning ? "true" : "false",
        loraOperational ? "true" : "false",
        isTransmitterLost() ? "true" : "false",
        wifiRSSI,
        wifiSSID.c_str(),
        getLoraRSSI(), getLoraSNR(), loraLastStr,
        millis() / 1000UL,
        FW_VERSION,
        timeStr,
        ohDisplayOnly      ? "true" : "false",
        ugDisplayOnly      ? "true" : "false",
        ugIgnoreForOH      ? "true" : "false",
        buzzerDelayEnabled ? "true" : "false",
        manualAutoStop     ? "true" : "false",
        (unsigned)lcdBacklightMode,
        (unsigned)ohStartLevel,
        (unsigned)ohStopLevel,
        (unsigned)ohMaxRunMin,
        (unsigned)mqttWatchdogMin,
        getLogLevel() == DEBUG ? "debug" : "info",
        isBuzzerActive()     ? "true" : "false",
        isOHBuzzerPending()  ? "true" : "false",
        isUGBuzzerPending()  ? "true" : "false",
        getOHStartCountdown(),
        getUGStartCountdown(),
        (unsigned)getOHLastReason(),
        (unsigned)getUGLastReason(),
        (unsigned)getOHRejectCode(),
        (unsigned)getUGRejectCode(),
        TRANSMITTER_FW_VERSION,
        schedJson
    );

    if (s_mqtt.publish(s_topicStatus, payload, true)) {
        Log(DEBUG, "[MQTT] Published status");
    } else {
        Log(WARN, "[MQTT] Publish failed (buffer too small?)");
    }
}

// Publish the in-memory log ring buffer to the logs topic (last 30 entries)
static void publishMQTTLogs() {
    if (!s_mqtt.connected()) return;
    String logsJson = getLogsJson(30);
    // Wrap in an object so consumers can distinguish from plain arrays
    String payload = "{\"logs\":" + logsJson + "}";
    if (!s_mqtt.publish(s_topicLogs, payload.c_str(), false)) {
        Log(WARN, "[MQTT] Log publish failed");
    }
}
