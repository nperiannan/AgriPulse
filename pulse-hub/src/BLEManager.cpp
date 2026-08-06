#include "BLEManager.h"
#include "Logger.h"
#include "Config.h"
#include "Globals.h"
#include "MotorControl.h"
#include "Buzzer.h"
#include "Display.h"
#include "RTCManager.h"
#include "WiFiManager.h"
#include "LoRaManager.h"
#include <WiFi.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>

// Global instance
BLEManager bleManager;

// Pointer used by callbacks
static BLEManager* pBLEMgr = nullptr;

// PIN pairing state
static uint32_t pairingPin        = 0;
static bool     pinDisplayed      = false;
static bool     pairingInProgress = false;

// ---------------------------------------------------------------------------
//  Security callbacks – PIN display pairing
// ---------------------------------------------------------------------------

class SecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override {
        pairingPin        = pass_key;
        pairingInProgress = true;
        if (!pinDisplayed) {
            displayPairingPin(pairingPin);
            pinDisplayed = true;
        }
    }
    bool onSecurityRequest() override {
        pairingInProgress = true; return true;
    }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t c) override {
        pairingInProgress = false;
        pinDisplayed      = false;
        if (c.success) { delay(2000); clearPairingPinDisplay(); }
        else           { clearPairingPinDisplay(); }
    }
    bool onConfirmPIN(uint32_t) override { return true; }
};

// ---------------------------------------------------------------------------
//  Server callbacks
// ---------------------------------------------------------------------------

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer*)    override {
        if (pBLEMgr) { pBLEMgr->deviceConnected = true; pairingInProgress = false; }
    }
    void onDisconnect(BLEServer*) override {
        if (pBLEMgr) {
            pBLEMgr->deviceConnected = false;
            pinDisplayed = false;
            clearPairingPinDisplay();
        }
    }
};

// ---------------------------------------------------------------------------
//  RX callbacks – queues commands for main-thread processing
// ---------------------------------------------------------------------------

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        if (!c || !pBLEMgr) return;
        String val = c->getValue().c_str();
        if (val.length() > 0) pBLEMgr->commandQueue.push(val);
    }
};

// ---------------------------------------------------------------------------
//  Constructor
// ---------------------------------------------------------------------------

BLEManager::BLEManager()
    : deviceConnected(false), oldDeviceConnected(false),
      pServer(nullptr), pTxCharacteristic(nullptr), pRxCharacteristic(nullptr),
      bleEnabled(true), bleInitialized(false) {}

// ---------------------------------------------------------------------------
//  begin()
// ---------------------------------------------------------------------------

void BLEManager::begin() {
    pBLEMgr = this;

    preferences.begin(NVS_BLE_NS, true);
    bleEnabled = preferences.getBool(NVS_KEY_BLE_ENABLED, true);
    preferences.end();

    if (!bleEnabled) { Log(INFO, "[BLE] Disabled – skipping init"); return; }

    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(517);

    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

    BLESecurity* sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    sec->setCapability(ESP_IO_CAP_OUT);   // ESP32 displays PIN, user types on phone
    sec->setKeySize(16);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* svc = pServer->createService(BLE_SERVICE_UUID);

    pTxCharacteristic = svc->createCharacteristic(
        BLE_TX_CHARACTERISTIC, BLECharacteristic::PROPERTY_NOTIFY);
    pTxCharacteristic->addDescriptor(new BLE2902());
    pTxCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);

    pRxCharacteristic = svc->createCharacteristic(
        BLE_RX_CHARACTERISTIC,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pRxCharacteristic->setCallbacks(new RxCallbacks());
    pRxCharacteristic->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);

    svc->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();

    bleInitialized = true;
    Log(INFO, "[BLE] Ready. Device: " BLE_DEVICE_NAME);
}

// ---------------------------------------------------------------------------
//  buildStatusJson
//  Sends float-switch states plus backward-compat level fields (0 or 100)
//  so the existing Flutter app can still show a value.
// ---------------------------------------------------------------------------

String BLEManager::buildStatusJson() {
    static char buf[512];
    uint8_t ohLevel = (ohTankState == TANK_STATE_FULL)  ? 100 :
                      (ohTankState == TANK_STATE_LOW)   ?   0 : 50;
    uint8_t ugLevel = (ugTankState == TANK_STATE_FULL)  ? 100 :
                      (ugTankState == TANK_STATE_LOW)   ?   0 : 50;

    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    snprintf(buf, sizeof(buf),
        "{"
        "\"ugState\":\"%s\",\"ohState\":\"%s\","
        "\"undergroundTankLevel\":%d,\"overheadTankLevel\":%d,"
        "\"oh_motor\":\"%s\",\"ug_motor\":\"%s\","
        "\"overheadMotorStatus\":\"%s\",\"undergroundMotorStatus\":\"%s\","
        "\"loraOk\":%s,\"loraRSSI\":%.1f,\"loraSNR\":%.1f,"
        "\"wifiConnected\":%s,\"wifiSSID\":\"%s\",\"wifiIP\":\"%s\","
        "\"ohDisplayOnly\":%s,\"ugDisplayOnly\":%s,\"ugIgnore\":%s,"
        "\"buzzerDelay\":%s,\"manualAutoStop\":%s,\"time\":\"%s\""
        "}",
        tankStateStr(ugTankState), tankStateStr(ohTankState),
        ugLevel, ohLevel,
        ohMotorRunning ? "ON" : "OFF",
        ugMotorRunning ? "ON" : "OFF",
        ohMotorRunning ? "ON" : "OFF",
        ugMotorRunning ? "ON" : "OFF",
        isLoraOperational() ? "true" : "false",
        getLoraRSSI(), getLoraSNR(),
        wifiOk ? "true" : "false",
        wifiOk ? WiFi.SSID().c_str() : "",
        wifiOk ? WiFi.localIP().toString().c_str() : "",
        ohDisplayOnly    ? "true" : "false",
        ugDisplayOnly    ? "true" : "false",
        ugIgnoreForOH    ? "true" : "false",
        buzzerDelayEnabled ? "true" : "false",
        manualAutoStop     ? "true" : "false",
        getFormattedTime().c_str()
    );
    return String(buf);
}

// ---------------------------------------------------------------------------
//  sendFramed  – chunks large payloads over BLE MTU
// ---------------------------------------------------------------------------

void BLEManager::sendFramed(const String& payload) {
    if (!deviceConnected || !pTxCharacteristic) return;
    const size_t mtu = 500;
    if (payload.length() <= mtu) {
        pTxCharacteristic->setValue(payload.c_str());
        pTxCharacteristic->notify();
    } else {
        size_t sent = 0;
        while (sent < payload.length()) {
            String chunk = payload.substring(sent, sent + mtu);
            pTxCharacteristic->setValue(chunk.c_str());
            pTxCharacteristic->notify();
            sent += mtu;
            delay(20);
        }
    }
}

// ---------------------------------------------------------------------------
//  Public send helpers
// ---------------------------------------------------------------------------

void BLEManager::sendStatus() {
    if (!deviceConnected || !pTxCharacteristic) return;
    sendFramed(buildStatusJson());
}

void BLEManager::sendResponse(const char* response) {
    if (!deviceConnected || !pTxCharacteristic) return;
    sendFramed(String(response));
}

void BLEManager::sendNotification(const char* message) {
    if (!deviceConnected || !pTxCharacteristic) return;
    String s = "{\"type\":\"notification\",\"message\":\"";
    s += message;
    s += "\"}";
    sendFramed(s);
}

// ---------------------------------------------------------------------------
//  processCommand
// ---------------------------------------------------------------------------

void BLEManager::processCommand(const String& cmd) {
    Log(INFO, "[BLE] Command: " + cmd);

    if (cmd == CMD_GET_STATUS) {
        sendStatus();
    } else if (cmd == CMD_MOTOR_OH_ON)  {
        turnOnOHMotor(REASON_MANUAL_APP);   sendResponse("{\"ok\":true,\"msg\":\"OH motor ON\"}");
    } else if (cmd == CMD_MOTOR_OH_OFF) {
        turnOffOHMotor(REASON_MANUAL_APP);  sendResponse("{\"ok\":true,\"msg\":\"OH motor OFF\"}");
    } else if (cmd == CMD_MOTOR_UG_ON)  {
        turnOnUGMotor(REASON_MANUAL_APP);   sendResponse("{\"ok\":true,\"msg\":\"UG motor ON\"}");
    } else if (cmd == CMD_MOTOR_UG_OFF) {
        turnOffUGMotor(REASON_MANUAL_APP);  sendResponse("{\"ok\":true,\"msg\":\"UG motor OFF\"}");
    } else if (cmd == CMD_BUZZER_ON)  {
        startBuzzer(BUZZER_SHORT_BEEPS);
        sendResponse("{\"ok\":true}");
    } else if (cmd == CMD_BUZZER_OFF) {
        stopBuzzer();
        sendResponse("{\"ok\":true}");
    } else if (cmd.startsWith(CMD_SYNC_TIME)) {
        // Payload: "SYNC_TIME:<epoch>"
        int sep = cmd.indexOf(':');
        if (sep >= 0) {
            uint32_t epoch = (uint32_t)cmd.substring(sep + 1).toInt();
            syncRTCFromEpoch(epoch);
        }
        sendResponse("{\"ok\":true,\"msg\":\"Time synced\"}");
    } else if (cmd == CMD_GET_CONFIG) {
        // Return config fields from the status JSON
        sendStatus();
    } else if (cmd.startsWith(CMD_SET_CONFIG)) {
        // Payload: "SET_CONFIG:{json}"
        int sep = cmd.indexOf(':');
        if (sep >= 0) {
            String json = cmd.substring(sep + 1);
            StaticJsonDocument<256> doc;
            if (deserializeJson(doc, json) == DeserializationError::Ok) {
                if (doc.containsKey("ohDisplayOnly")) ohDisplayOnly = doc["ohDisplayOnly"];
                if (doc.containsKey("ugDisplayOnly")) ugDisplayOnly = doc["ugDisplayOnly"];
                if (doc.containsKey("ugIgnore"))      ugIgnoreForOH = doc["ugIgnore"];
                if (doc.containsKey("buzzerDelay"))   buzzerDelayEnabled = doc["buzzerDelay"];
                if (doc.containsKey("manualAutoStop")) manualAutoStop = doc["manualAutoStop"];
                saveMotorConfig();
            }
        }
        sendResponse("{\"ok\":true,\"msg\":\"Config updated\"}");
    } else {
        sendResponse("{\"ok\":false,\"msg\":\"Unknown command\"}");
    }
}

// ---------------------------------------------------------------------------
//  update()  – call from loop()
// ---------------------------------------------------------------------------

void BLEManager::update() {
    if (!bleInitialized) return;

    // Handle reconnect advertising
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        pServer->startAdvertising();
        oldDeviceConnected = false;
        Log(INFO, "[BLE] Re-advertising after disconnect");
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = true;
    }

    // Process queued commands
    while (!commandQueue.empty()) {
        String cmd = commandQueue.front();
        commandQueue.pop();
        processCommand(cmd);
    }
}

// ---------------------------------------------------------------------------
//  Misc
// ---------------------------------------------------------------------------

bool BLEManager::isConnected()          const { return deviceConnected; }
bool BLEManager::isEnabled()            const { return bleEnabled; }
uint32_t BLEManager::getPairingPin()    const { return pairingPin; }
bool BLEManager::isPairingInProgress()  const { return pairingInProgress; }

void BLEManager::setEnabled(bool enabled) {
    bleEnabled = enabled;
    preferences.begin(NVS_BLE_NS, false);
    preferences.putBool(NVS_KEY_BLE_ENABLED, enabled);
    preferences.end();
    if (!enabled) stop();
    else          restart();
}

void BLEManager::stop() {
    if (bleInitialized) {
        BLEDevice::stopAdvertising();
        bleInitialized = false;
        Log(INFO, "[BLE] Stopped");
    }
}

void BLEManager::restart() {
    if (!bleInitialized) begin();
}
