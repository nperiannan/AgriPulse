#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <queue>

class BLEManager {
public:
    BLEManager();

    // Initialise BLE (loads enabled state from NVS).
    void begin();

    // Call from loop() – processes the command queue (avoids stack overflow in callbacks).
    void update();

    bool isConnected() const;
    bool isEnabled()   const;

    // Send current status JSON to connected phone.
    void sendStatus();

    // Send a plain-text response string.
    void sendResponse(const char* response);

    // Send a notification string (e.g. motor state change).
    void sendNotification(const char* message);

    // Enable / disable BLE at runtime and persist.
    void setEnabled(bool enabled);
    void stop();
    void restart();

    // PIN pairing helpers.
    uint32_t getPairingPin()      const;
    bool     isPairingInProgress() const;

    bool deviceConnected;
    bool oldDeviceConnected;

    // Command queue (public so BLE callbacks can push to it).
    std::queue<String> commandQueue;

private:
    BLEServer*         pServer;
    BLECharacteristic* pTxCharacteristic;
    BLECharacteristic* pRxCharacteristic;
    bool bleEnabled;
    bool bleInitialized;

    String buildStatusJson();
    void   processCommand(const String& cmd);
    void   sendFramed(const String& payload);
};

extern BLEManager bleManager;

#endif // BLE_MANAGER_H
