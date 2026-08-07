#include "HttpServer.h"
#include <esp_mac.h>
#include "Logger.h"
#include "Config.h"
#include "Globals.h"
#include "MotorDrive.h"
#include "MotorControl.h"   // status accessors (buzzer countdown, reject codes, saveMotorConfig)
#include "Buzzer.h"
#include "LoRaManager.h"
#include "WiFiManager.h"
#include "Display.h"
#include "Scheduler.h"
#include "History.h"
#include "MQTTManager.h"
#include "RTCManager.h"
#include "ApiCommon.h"
#include "I2CScan.h"
#include "web/PageHtml.h"
#include "web/PageCss.h"
#include "web/PageJs.h"
#include <WebServer.h>   // Arduino ESP32 WebServer library
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <Preferences.h>

static WebServer server(80);

// The API modules read request arguments through this reference rather than
// owning a server instance of their own.
WebServer& apiServer = server;

// ---------------------------------------------------------------------------
//  Authentication
// ---------------------------------------------------------------------------
// Guards the web UI only.  The panel touch buttons are deliberately exempt:
// standing at the panel is itself proof of authorisation, and requiring a
// password to stop a motor by hand would be actively dangerous.
//
// There is no TLS on this server, so the credentials cross the local link in
// clear either way. Basic is used because this WebServer's Digest support
// rejects every second request (see requireAuth).
//
// Ships with admin/password for bench work. That is a known-weak default and
// must be changed before the unit is deployed; it is logged loudly on every
// boot until it is.
static String webUser = "admin";
static String webPass;
static bool   webPassIsDefault = true;

#define WEB_PASS_DEFAULT "password"

static void loadWebCredentials() {
    Preferences p;
    p.begin(NVS_WEBAUTH_NS, false);
    webUser          = p.getString(NVS_KEY_WEB_USER, "admin");
    webPass          = p.getString(NVS_KEY_WEB_PASS, "");
    webPassIsDefault = p.getBool(NVS_KEY_WEB_PASS_GEN, true);

    // Anything the user has not explicitly set is forced back to the shipped
    // default, so an earlier build's generated password cannot linger in NVS
    // and lock the operator out.
    if (webPassIsDefault && webPass != WEB_PASS_DEFAULT) {
        webPass = WEB_PASS_DEFAULT;
        p.putString(NVS_KEY_WEB_PASS, webPass);
        p.putBool(NVS_KEY_WEB_PASS_GEN, true);
    }
    p.end();

    if (webPassIsDefault) {
        Log(WARN, "==================================================");
        Log(WARN, "[Web] DEFAULT LOGIN IN USE - user: " + webUser + "  password: " + webPass);
        Log(WARN, "[Web] Change it before deploying. Anyone on this network can");
        Log(WARN, "[Web] otherwise command the motor.");
        Log(WARN, "==================================================");
    }
}

static bool requireAuth() {
    if (server.authenticate(webUser.c_str(), webPass.c_str())) return true;
    // Basic, not Digest: this WebServer rotates its digest nonce on every
    // challenge, so a browser reusing the nonce it was just issued gets
    // rejected — every second request 401s and the page loads unstyled.
    // There is no TLS here regardless, so Digest bought little.
    server.requestAuthentication(BASIC_AUTH, "AgriPulse", "Authentication required");
    return false;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static void sendJson(int code, const String& json) {
    server.send(code, "application/json", json);
}

static void sendOk() {
    sendJson(200, "{\"ok\":true}");
}

static void sendError(const char* msg) {
    String s = "{\"ok\":false,\"error\":\"";
    s += msg;
    s += "\"}";
    sendJson(400, s);
}

// Same helpers, exposed to the API modules.
void apiSendJson(int code, const String& json) { sendJson(code, json); }
void apiSendOk()                               { sendOk(); }
void apiSendError(const char* msg)             { sendError(msg); }

// ---------------------------------------------------------------------------
//  Static assets  – markup, styles and behaviour are served separately so the
//  browser caches the CSS/JS and each can be reworked on its own.
// ---------------------------------------------------------------------------

static void handleRoot() {
    server.send_P(200, "text/html", PAGE_HTML);
}

static void handleCss() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "text/css", PAGE_CSS);
}

static void handleJs() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "application/javascript", PAGE_JS);
}

// ---------------------------------------------------------------------------
//  GET /status  – JSON status for BLE app and AJAX
// ---------------------------------------------------------------------------

// Why the device last restarted. Distinguishing a brownout (undersized supply
// or a contactor inrush dragging the rail down) from a panic or a clean OTA
// reboot is the first question worth asking when a controller in the field
// restarts unexpectedly, so surface it rather than leaving it to the logs.
static const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software / OTA";
        case ESP_RST_PANIC:    return "panic / exception";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout (supply dipped)";
        case ESP_RST_DEEPSLEEP:return "deep-sleep wake";
        case ESP_RST_EXT:      return "external reset";
        default:               return "unknown";
    }
}

static void handleStatus() {
    // Was 1280 — silently overflowed the moment btMacAddress was added
    // (ArduinoJson drops fields past capacity with no error, not a crash),
    // which is why i2cScl/boardRev went missing from the response without
    // any compile or runtime error to point at it. Found live: the System
    // tab showed "undefined" for both while everything added before them in
    // this function still worked fine.
    StaticJsonDocument<2048> doc;
    doc["ugState"]           = tankStateStr(ugTankState);
    doc["ohState"]           = tankStateStr(ohTankState);
    doc["ohLastKnown"]       = tankStateStr(ohLastKnownState);
    doc["undergroundTankLevel"] = (ugTankState == TANK_STATE_FULL) ? 100
                                : (ugTankState == TANK_STATE_HALF) ? 50
                                : (ugTankState == TANK_STATE_LOW)  ? 20
                                : (ugTankState == TANK_STATE_EMPTY) ? 0 : 50;
    doc["overheadTankLevel"]    = (ohTankState == TANK_STATE_FULL) ? 100
                                : (ohTankState == TANK_STATE_HALF) ? 50
                                : (ohTankState == TANK_STATE_LOW)  ? 20
                                : (ohTankState == TANK_STATE_EMPTY) ? 0 : 50;
    doc["oh_motor"]          = ohMotorRunning ? "ON" : "OFF";
    doc["ug_motor"]          = ugMotorRunning ? "ON" : "OFF";
    doc["overheadMotorStatus"]     = ohMotorRunning ? "ON" : "OFF";
    doc["undergroundMotorStatus"]  = ugMotorRunning ? "ON" : "OFF";
    doc["ohDisplayOnly"]     = ohDisplayOnly;
    doc["ugDisplayOnly"]     = ugDisplayOnly;
    doc["ugIgnore"]          = ugIgnoreForOH;
    doc["buzzerDelay"]       = buzzerDelayEnabled;
    doc["manualAutoStop"]    = manualAutoStop;
    doc["ohStartLevel"]      = (int)ohStartLevel;
    doc["ohStopLevel"]       = (int)ohStopLevel;
    doc["ohMaxRunMin"]       = (int)ohMaxRunMin;
    doc["mqttWatchdogMin"]   = (int)mqttWatchdogMin;
    doc["loraOk"]            = isLoraOperational();
    doc["txLost"]            = isTransmitterLost();
    doc["loraRSSI"]          = getLoraRSSI();
    doc["loraSNR"]           = getLoraSNR();
    doc["loraFreqMHz"]       = LORA_FREQUENCY;
    doc["lastLoraReceived"]  = lastLoraReceivedTime > 0
                               ? String((millis() - lastLoraReceivedTime) / 1000) + "s ago"
                               : "Never";
    doc["wifiConnected"]     = (WiFi.status() == WL_CONNECTED);
    doc["wifiSSID"]          = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
    doc["wifiIP"]            = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    // AP is started once in wifiTask() and never torn down (see WiFiManager.cpp),
    // so apEnabled is normally true regardless of STA state — isAPMode really
    // means "STA not yet connected", not "AP radio off". Report both modes
    // explicitly rather than let the UI infer radio state from that flag.
    doc["apEnabled"]         = (WiFi.getMode() & WIFI_AP) != 0;
    doc["staEnabled"]        = (WiFi.getMode() & WIFI_STA) != 0;
    doc["apIP"]              = WiFi.softAPIP().toString();
    doc["webUser"]           = webUser;
    doc["time"]              = getFormattedTime();
    doc["fwVersion"]         = FW_VERSION;
    doc["bleEnabled"]        = false;
    doc["bleConnected"]      = false;
    doc["lcdBlMode"]         = (int)lcdBacklightMode;
    doc["logLevel"]          = getLogLevel() == DEBUG ? "debug" : "info";
    doc["buzzerActive"]      = isBuzzerActive();
    doc["ohBuzzer"]          = isOHBuzzerPending();
    doc["ugBuzzer"]          = isUGBuzzerPending();
    doc["ohCd"]              = getOHStartCountdown();
    doc["ugCd"]              = getUGStartCountdown();
    doc["ohRsn"]             = (int)getOHLastReason();
    doc["ugRsn"]             = (int)getUGLastReason();
    doc["ohRej"]             = (int)getOHRejectCode();
    doc["ugRej"]             = (int)getUGRejectCode();
    doc["txFw"]              = TRANSMITTER_FW_VERSION;
    doc["uptime"]            = (uint32_t)(millis() / 1000UL);
    doc["ntpSynced"]         = hasNtpSynced();
    doc["ntpDriftSec"]       = getNtpDriftSeconds();
    doc["ntpSyncAge"]        = (uint32_t)getNtpSyncAgeSeconds();
    // Any schedule currently in its run window?
    bool anySchedRunning = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (schedules[i].isRunning) { anySchedRunning = true; break; }
    }
    doc["schedRunning"]      = anySchedRunning;
    // --- About / device info ---
    doc["lcdAddr"]           = getLcdAddress();
    doc["eepromAddr"]        = histEepromFound ? getEepromAddress() : 0;
    doc["eepromOk"]          = histEepromFound;
    doc["rtcAddr"]           = 0x68;
    doc["rtcOk"]             = ds3231Found;
    doc["freeHeap"]          = (uint32_t)ESP.getFreeHeap();
    doc["heapSize"]          = (uint32_t)ESP.getHeapSize();
    doc["sketchSize"]        = (uint32_t)ESP.getSketchSize();
    doc["freeSketch"]        = (uint32_t)ESP.getFreeSketchSpace();
    doc["flashSize"]         = (uint32_t)ESP.getFlashChipSize();
    // Always 0 — PSRAM is deliberately disabled (board_build.psram=disabled in
    // platformio.ini) so GPIO35 can be reused as CHANGEOVER_RELAY_PIN instead
    // of its PSRAM-bus role. Reported honestly rather than omitted, so the UI
    // can say "disabled" instead of just not mentioning it.
    doc["psramSize"]         = (uint32_t)ESP.getPsramSize();
    doc["cpuCores"]          = (uint32_t)ESP.getChipCores();
    doc["cpuFreqMHz"]        = (uint32_t)ESP.getCpuFreqMHz();
    doc["chipModel"]         = ESP.getChipModel();
    doc["chipRev"]           = (uint32_t)ESP.getChipRevision();
    doc["flashSpeedHz"]      = (uint32_t)ESP.getFlashChipSpeed();
    doc["sdkVersion"]        = ESP.getSdkVersion();
    doc["macAddress"]        = WiFi.macAddress();
    // esp_read_mac() reads the factory-programmed BT MAC straight from efuse —
    // it does not require the Bluetooth controller to actually be initialised,
    // which this firmware never does (no BLE stack built in). Formatted the
    // same way WiFi.macAddress() already comes back, for a direct comparison.
    {
        uint8_t btMac[6] = {0};
        char btMacStr[18];
        esp_read_mac(btMac, ESP_MAC_BT);
        snprintf(btMacStr, sizeof(btMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 btMac[0], btMac[1], btMac[2], btMac[3], btMac[4], btMac[5]);
        doc["btMacAddress"] = String(btMacStr);
    }
    doc["resetReason"]       = resetReasonStr();
    doc["i2cSda"]            = SDA_PIN;
    doc["i2cScl"]            = SCL_PIN;
    doc["boardRev"]          =
    #ifdef BOARD_V2
        "v2.0";
    #else
        "v1.x";
    #endif
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  Motor endpoints — legacy /motor and /undergroundmotor, retired 2026-08-07.
//  Kept as thin redirects to the drive (well/bore) for any old client still
//  calling them; /api/motor/cmd is the real interface now.
// ---------------------------------------------------------------------------

static void handleOHMotor() {
    String state = server.arg("state");
    if (state == "on")       motorDriveRequestStart(MOTOR_WELL, REASON_MANUAL_WEB);
    else if (state == "off") motorDriveRequestStop(REASON_MANUAL_WEB);
    else { sendError("Invalid state param"); return; }
    sendOk();
}

static void handleUGMotor() {
    String state = server.arg("state");
    if (state == "on")       motorDriveRequestStart(MOTOR_BORE, REASON_MANUAL_WEB);
    else if (state == "off") motorDriveRequestStop(REASON_MANUAL_WEB);
    else { sendError("Invalid state param"); return; }
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /wifiscan  – run a fresh scan and return visible APs as JSON
// ---------------------------------------------------------------------------

// Submit/poll pattern, not a blocking call: WiFi scanning is owned entirely
// by wifiTask on Core 0 (see WiFiManager.cpp). This handler used to call
// WiFi.scanNetworks(false,true) directly from here (Core 1, the single-
// threaded web server's task) — that raced the reconnect state machine's own
// scans on Core 0 for the one ESP-IDF scan slot, which is what caused
// "Scan failed or timed out" in the UI. This just queues the request and
// returns instantly; the browser polls /wifiscanresult for completion.
static void handleWifiScan() {
    wifiRequestManualScan();
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
}

static void handleWifiScanResult() {
    if (!wifiManualScanReady()) {
        server.send(200, "application/json", "{\"ready\":false}");
        return;
    }
    server.send(200, "application/json",
                "{\"ready\":true,\"networks\":" + wifiManualScanResultJson() + "}");
}

// ---------------------------------------------------------------------------
//  GET /wifilist
// ---------------------------------------------------------------------------

static void handleWifiList() {
    server.send(200, "application/json", getStoredNetworksJson());
}

// ---------------------------------------------------------------------------
//  POST /addwifi
// ---------------------------------------------------------------------------

static void handleAddWifi() {
    String ssid = server.arg("ssid");
    String pass = server.arg("password");
    if (ssid.isEmpty()) { sendError("SSID required"); return; }
    addWifiNetwork(ssid, pass);
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setwifipriority
// ---------------------------------------------------------------------------

static void handleSetWifiPriority() {
    String ssid = server.arg("ssid");
    int    pri  = server.arg("priority").toInt();
    if (ssid.isEmpty() || pri < 1) { sendError("ssid and priority required"); return; }
    setWifiPriority(ssid, pri);
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /deletewifi
// ---------------------------------------------------------------------------

static void handleDeleteWifi() {
    String ssid = server.arg("ssid");
    if (ssid.isEmpty()) { sendError("SSID required"); return; }
    removeWifiNetwork(ssid);
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setconfig
// ---------------------------------------------------------------------------

static void handleSetConfig() {
    ohDisplayOnly    = server.hasArg("oh_disp_only");
    ugDisplayOnly    = server.hasArg("ug_disp_only");
    ugIgnoreForOH    = server.hasArg("ug_ignore");
    buzzerDelayEnabled = server.hasArg("buzzer_delay");
    manualAutoStop     = server.hasArg("manual_auto_stop");
    if (server.hasArg("oh_start_level")) {
        uint8_t v = (uint8_t)server.arg("oh_start_level").toInt();
        if (v >= TANK_STATE_EMPTY && v <= TANK_STATE_HALF) ohStartLevel = (TankState)v;
    }
    if (server.hasArg("oh_stop_level")) {
        uint8_t v = (uint8_t)server.arg("oh_stop_level").toInt();
        if (v >= TANK_STATE_LOW && v <= TANK_STATE_FULL) ohStopLevel = (TankState)v;
    }
    // Validate: stop must be > start
    if (ohStopLevel <= ohStartLevel) {
        ohStartLevel = TANK_STATE_EMPTY;
        ohStopLevel  = TANK_STATE_FULL;
    }
    if (server.hasArg("oh_max_run")) {
        int v = server.arg("oh_max_run").toInt();
        if (v >= 5 && v <= 60) ohMaxRunMin = (uint8_t)v;
    }
    if (server.hasArg("mqtt_watchdog_min")) {
        int v = server.arg("mqtt_watchdog_min").toInt();
        if (v >= MQTT_WATCHDOG_MIN_MIN && v <= MQTT_WATCHDOG_MAX_MIN) mqttWatchdogMin = (uint8_t)v;
    }
    saveMotorConfig();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /buzzertest
// ---------------------------------------------------------------------------

static void handleBuzzerTest() {
    startBuzzer(BUZZER_SHORT_BEEPS);
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /reboot
// ---------------------------------------------------------------------------

static void handleReboot() {
    sendOk();
    delay(500);
    esp_restart();
}

// ---------------------------------------------------------------------------
//  POST /factoryreset
// ---------------------------------------------------------------------------

static void handleFactoryReset() {
    preferences.begin(NVS_MOTOR_NS,  false); preferences.clear(); preferences.end();
    preferences.begin(NVS_WIFI_NS,   false); preferences.clear(); preferences.end();
    preferences.begin(NVS_BLE_NS,    false); preferences.clear(); preferences.end();
    preferences.begin("scheduler",   false); preferences.clear(); preferences.end();
    Log(INFO, "[Web] Factory reset performed – all NVS namespaces cleared");
    sendOk();
    delay(500);
    esp_restart();
}

// ---------------------------------------------------------------------------
//  GET /systeminfo
// ---------------------------------------------------------------------------

static void handleSystemInfo() {
    StaticJsonDocument<256> doc;
    doc["fwVersion"]  = FW_VERSION;
    doc["freeHeap"]   = (int)ESP.getFreeHeap();
    doc["uptime"]     = (uint32_t)(millis() / 1000);
    doc["bleConn"]    = false;
    doc["apMode"]     = isAPMode;
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  POST /setbleenabled
// ---------------------------------------------------------------------------

static void handleSetBleEnabled() {
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /schedulelist
// ---------------------------------------------------------------------------

static void handleScheduleList() {
    StaticJsonDocument<768> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["enabled"]   = schedules[i].enabled;
        obj["motorType"] = schedules[i].motorType;
        obj["time"]      = schedules[i].time;
        obj["duration"]  = schedules[i].duration;
        obj["running"]   = schedules[i].isRunning;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  POST /updateAllSchedules
// ---------------------------------------------------------------------------

static void handleUpdateAllSchedules() {
    // Track which motor types had a running schedule that is now being disabled
    bool ohWasRunning = false, ugWasRunning = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!schedules[i].isRunning) continue;
        bool willBeDisabled = !server.hasArg("enabled" + String(i));
        String newTime = server.arg("time" + String(i));
        if (newTime.length() < 5) newTime = "00:00";
        bool cleared = (newTime == "00:00" || willBeDisabled);
        if (cleared) {
            if (schedules[i].motorType == 1) ugWasRunning = true;
            else                             ohWasRunning = true;
        }
    }
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        bool enabled   = server.hasArg("enabled" + String(i));
        uint8_t mtype  = (uint8_t)constrain(server.arg("motorType" + String(i)).toInt(), 0, 1);
        String time    = server.arg("time" + String(i));
        int duration   = server.arg("duration" + String(i)).toInt();
        if (time.length() < 5 || time.indexOf(':') == -1) time = "00:00";
        duration = constrain(duration, 1, 240);
        schedules[i].enabled   = enabled;
        schedules[i].motorType = mtype;
        schedules[i].time      = time;
        schedules[i].duration  = (uint16_t)duration;
        if (!enabled) schedules[i].isRunning = false;
    }
    saveSchedules();
    // Cancel the motor if its running schedule was deleted/disabled
    if (ohWasRunning || ugWasRunning) motorDriveRequestStop(REASON_SCHEDULED);
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /cancelSchedule – stop any running scheduled motor, cancel buzzer
// ---------------------------------------------------------------------------

static void handleCancelSchedule() {
    bool ohRunning = false, ugRunning = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!schedules[i].isRunning) continue;
        if (schedules[i].motorType == 1) ugRunning = true;
        else                             ohRunning = true;
        schedules[i].isRunning = false;
        schedules[i].startTime = 0;
    }
    // Also covers a pending buzzer-delay start that fired < 10s ago
    motorDriveRequestStop(REASON_SCHEDULED);
    (void)ohRunning; (void)ugRunning;
    Log(INFO, "[Sched] Active schedule cancelled via web");
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /clearSchedules
// ---------------------------------------------------------------------------

static void handleClearSchedules() {
    clearAllSchedules();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /syncntp
// ---------------------------------------------------------------------------

static void handleSyncNtp() {
    if (WiFi.status() != WL_CONNECTED) {
        server.send(200, "application/json", "{\"ok\":false,\"error\":\"Not connected to WiFi\"}");
        return;
    }
    synchronizeTime();
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /history  – returns last 100 events from EEPROM
// ---------------------------------------------------------------------------

static void handleHistory() {
    server.send(200, "application/json", getHistoryJson(100));
}

// ---------------------------------------------------------------------------
//  POST /clearhistory
// ---------------------------------------------------------------------------

static void handleClearHistory() {
    clearHistory();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setlcdmode  – set LCD backlight mode (auto / always_on / always_off)
// ---------------------------------------------------------------------------

static void handleSetLcdMode() {
    String mode = server.arg("mode");
    if      (mode == "always_on")  lcdBacklightMode = LCD_BL_ALWAYS_ON;
    else if (mode == "always_off") lcdBacklightMode = LCD_BL_ALWAYS_OFF;
    else                           lcdBacklightMode = LCD_BL_AUTO;
    saveMotorConfig();
    applyBacklightMode();
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setloglevel  – set log level (info / debug)
// ---------------------------------------------------------------------------

static void handleSetLogLevel() {
    String level = server.arg("level");
    if (level == "debug") {
        setLogLevel(DEBUG);
        Log(INFO, "[Web] Log level set to DEBUG");
    } else {
        setLogLevel(INFO);
        Log(INFO, "[Web] Log level set to INFO");
    }
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setmqttpass  – update MQTT password in NVS and reconnect
// ---------------------------------------------------------------------------

static void handleSetMqttPass() {
    String newPass = server.arg("pass");
    if (newPass.isEmpty()) { sendError("pass required"); return; }
    Preferences prefs;
    prefs.begin(MQTT_NVS_NS, false);
    prefs.putString("pass", newPass);
    prefs.end();
    Log(INFO, "[Web] MQTT password updated via HTTP");
    sendOk();
}

// ---------------------------------------------------------------------------
//  POST /setappass  – change the AP hotspot / OTA upload passwords
// ---------------------------------------------------------------------------

static void handleSetApPass() {
    String ap  = server.arg("ap");
    String ota = server.arg("ota");
    if (ap.isEmpty() && ota.isEmpty()) { sendError("nothing to change"); return; }

    if (!ap.isEmpty() && !setApPassword(ap)) {
        sendError("AP password must be at least 8 characters"); return;
    }
    if (!ota.isEmpty() && !setOtaPassword(ota)) {
        sendError("OTA password must be at least 8 characters"); return;
    }
    sendOk();
}

static void handleSetWebPass() {
    String newUser = server.arg("user");
    String newPass = server.arg("pass");
    if (newPass.length() < 8) { sendError("password must be at least 8 characters"); return; }
    if (newUser.isEmpty()) newUser = webUser;

    Preferences p;
    p.begin(NVS_WEBAUTH_NS, false);
    p.putString(NVS_KEY_WEB_USER, newUser);
    p.putString(NVS_KEY_WEB_PASS, newPass);
    p.putBool(NVS_KEY_WEB_PASS_GEN, false);
    p.end();

    webUser            = newUser;
    webPass            = newPass;
    webPassIsDefault   = false;
    Log(WARN, "[Web] Login changed - the default password is no longer valid");
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /mqttconfig  – return current MQTT broker, port, connection status
// ---------------------------------------------------------------------------

static void handleMqttConfig() {
    StaticJsonDocument<256> doc;
    doc["broker"]    = getMQTTBroker();
    doc["port"]      = getMQTTPort();
    doc["connected"] = isMQTTConnected();
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
//  POST /setmqttbroker  – update MQTT broker host & port in NVS, reconnect
// ---------------------------------------------------------------------------

static void handleSetMqttBroker() {
    String broker = server.arg("broker");
    int    port   = server.arg("port").toInt();
    if (broker.isEmpty()) { sendError("broker required"); return; }
    if (port < 1 || port > 65535) port = MQTT_PORT_DEFAULT;
    Preferences prefs;
    prefs.begin(MQTT_NVS_NS, false);
    prefs.putString("broker", broker);
    prefs.putInt("port", port);
    prefs.end();
    reloadMQTTConfig();
    Log(INFO, "[Web] MQTT broker set to " + broker + ":" + String(port));
    sendOk();
}

// ---------------------------------------------------------------------------
//  GET /api/i2cscan  — on-demand bus scan for the System page
// ---------------------------------------------------------------------------

static void handleI2CScan() {
    server.send(200, "application/json", i2cScanJson());
}

// ---------------------------------------------------------------------------
//  Setup & loop
// ---------------------------------------------------------------------------

void setupWebServer() {
    loadWebCredentials();

    // Every route is authenticated. Guarding at registration rather than inside
    // each handler means a newly added endpoint cannot be left unprotected by
    // forgetting a line.
    auto guarded = [](const char* uri, HTTPMethod method, WebServer::THandlerFunction fn) {
        server.on(uri, method, [fn]() { if (requireAuth()) fn(); });
    };

    guarded("/",                   HTTP_GET,  handleRoot);
    guarded("/app.css",            HTTP_GET,  handleCss);
    guarded("/app.js",             HTTP_GET,  handleJs);
    guarded("/api/i2cscan",        HTTP_GET,  handleI2CScan);
    guarded("/status",             HTTP_GET,  handleStatus);
    guarded("/motor",              HTTP_GET,  handleOHMotor);          // legacy: -> Well
    guarded("/undergroundmotor",   HTTP_GET,  handleUGMotor);          // legacy: -> Bore
    guarded("/wifiscan",           HTTP_GET,  handleWifiScan);
    guarded("/wifiscanresult",     HTTP_GET,  handleWifiScanResult);
    guarded("/wifilist",           HTTP_GET,  handleWifiList);
    guarded("/addwifi",            HTTP_POST, handleAddWifi);
    guarded("/setwifipriority",    HTTP_POST, handleSetWifiPriority);
    guarded("/deletewifi",         HTTP_GET,  handleDeleteWifi);
    guarded("/setconfig",          HTTP_POST, handleSetConfig);
    guarded("/reboot",             HTTP_POST, handleReboot);
    guarded("/factoryreset",       HTTP_POST, handleFactoryReset);
    guarded("/systeminfo",         HTTP_GET,  handleSystemInfo);
    guarded("/setbleenabled",      HTTP_POST, handleSetBleEnabled);
    guarded("/setlcdmode",         HTTP_POST, handleSetLcdMode);
    guarded("/setloglevel",        HTTP_POST, handleSetLogLevel);
    guarded("/setmqttpass",        HTTP_POST, handleSetMqttPass);
    guarded("/mqttconfig",         HTTP_GET,  handleMqttConfig);
    guarded("/setmqttbroker",      HTTP_POST, handleSetMqttBroker);
    guarded("/schedulelist",       HTTP_GET,  handleScheduleList);
    guarded("/updateAllSchedules", HTTP_POST, handleUpdateAllSchedules);
    guarded("/cancelSchedule",     HTTP_POST, handleCancelSchedule);
    guarded("/clearSchedules",     HTTP_POST, handleClearSchedules);
    guarded("/syncntp",            HTTP_POST, handleSyncNtp);
    guarded("/history",            HTTP_GET,  handleHistory);
    guarded("/clearhistory",       HTTP_POST, handleClearHistory);
    guarded("/setwebpass",         HTTP_POST, handleSetWebPass);
    guarded("/setappass",          HTTP_POST, handleSetApPass);

    // Feature APIs live in src/api/*. They receive the same guard, so a module
    // cannot publish an unauthenticated endpoint by omission.
    registerPowerApi(guarded);
    registerDriveApi(guarded);
    registerProtectionApi(guarded);
    registerZoneApi(guarded);
    registerProgramApi(guarded);
    guarded("/logs",               HTTP_GET,  [](){
        sendJson(200, getLogsJson(50));
    });
    guarded("/clearlogs",          HTTP_POST, [](){ clearLogs(); sendOk(); });
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    server.begin();
    Log(INFO, "[Web] Server started (authentication required)");
}

void handleWebClients() {
    server.handleClient();
}
