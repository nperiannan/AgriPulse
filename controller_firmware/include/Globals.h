#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <Preferences.h>

// =============================================================================
//                              SHARED TYPES
// =============================================================================

// State reported by a float switch sensor.
typedef enum : uint8_t {
    TANK_STATE_UNKNOWN = 0,  // No valid reading yet (startup / sensor fault)
    TANK_STATE_EMPTY   = 1,  // No switches triggered (0 0 0)
    TANK_STATE_LOW     = 2,  // Only LOW switch triggered (0 0 1)
    TANK_STATE_HALF    = 3,  // LOW + HALF switches (0 1 1)
    TANK_STATE_FULL    = 4,  // All three switches (1 1 1)
} TankState;

// Returns a human-readable C-string for a TankState.
inline const char* tankStateStr(TankState s) {
    switch (s) {
        case TANK_STATE_EMPTY: return "EMPTY";
        case TANK_STATE_LOW:   return "LOW";
        case TANK_STATE_HALF:  return "HALF";
        case TANK_STATE_FULL:  return "FULL";
        default:               return "UNKNOWN";
    }
}

// Motor source tracking: who started the motor?
typedef enum : uint8_t {
    MOTOR_SRC_NONE      = 0,
    MOTOR_SRC_AUTO      = 1,
    MOTOR_SRC_MANUAL    = 2,
    MOTOR_SRC_SCHEDULED = 3,
} MotorSource;

// LoRa packet sent by the remote OH-tank node (float switch version).
// 4 bytes: type, sw_full, sw_half, sw_low
#pragma pack(push, 1)
struct FloatPacket {
    uint8_t type;      // LORA_PKT_FLOAT_SWITCH (0x02)
    uint8_t sw_full;   // 1 = FULL switch triggered
    uint8_t sw_half;   // 1 = HALF switch triggered
    uint8_t sw_low;    // 1 = LOW switch triggered
};
#pragma pack(pop)

// =============================================================================
//                              GLOBAL STATE (defined in main.cpp)
// =============================================================================
extern TankState ugTankState;         // Current UG tank float state
extern TankState ohTankState;         // Current OH tank float state (from LoRa)
extern TankState ohLastKnownState;    // Last valid OH state before UNKNOWN

extern bool ohMotorRunning;           // OH relay is energised
extern bool ugMotorRunning;           // UG relay is energised

extern MotorSource ohMotorSource;     // Who started OH motor
extern MotorSource ugMotorSource;     // Who started UG motor

extern bool loraOperational;          // LoRa radio is healthy
extern unsigned long lastLoraReceivedTime; // millis() of last valid LoRa packet

extern bool isAPMode;                 // ESP32 is in AP (hotspot) mode
extern String wifiSSID;               // Current connected SSID
extern int    wifiRSSI;               // Current WiFi RSSI

// Runtime configuration (loaded from NVS at boot, also written back on change)
extern bool    ohDisplayOnly;       // True → do NOT drive OH relay; monitor only
extern bool    ugDisplayOnly;       // True → do NOT drive UG relay; monitor only
extern bool    ugIgnoreForOH;       // True → start OH motor even if UG tank is LOW
extern bool    buzzerDelayEnabled;  // True → buzz before motor starts
extern bool    manualAutoStop;      // True → stop manually-started motors when tank is full
extern uint8_t lcdBacklightMode;    // LCD_BL_AUTO / LCD_BL_ALWAYS_ON / LCD_BL_ALWAYS_OFF

// Configurable OH motor start/stop thresholds (loaded from NVS)
extern TankState ohStartLevel;      // Start motor when OH <= this level (default EMPTY)
extern TankState ohStopLevel;       // Stop motor when OH >= this level (default FULL)
extern uint8_t   ohMaxRunMin;       // Max motor runtime in minutes (default 20, range 5-60)
extern uint8_t   mqttWatchdogMin;   // Reboot if MQTT disconnected this many minutes (default 15, range 10-60)

// Shared NVS preferences object (opened/closed per use in each module)
extern Preferences preferences;

#endif // GLOBALS_H
