#ifndef HISTORY_H
#define HISTORY_H

#include <Arduino.h>
#include "Globals.h"

// =============================================================================
//  Event types recorded in EEPROM history
// =============================================================================
typedef enum : uint8_t {
    HIST_MOTOR_OH_ON  = 0,
    HIST_MOTOR_OH_OFF = 1,
    HIST_MOTOR_UG_ON  = 2,
    HIST_MOTOR_UG_OFF = 3,
    HIST_OH_STATE_CHG = 4,   // OH tank state changed (LoRa update)
    HIST_UG_STATE_CHG = 5,   // UG tank state changed (float switch)
    HIST_BOOT         = 6,
    // Added 2026-08-07 for irrigation zones. The 8-byte record has no spare
    // field, so for these two event types ONLY, HistoryRecord.ohState is
    // repurposed to carry the zone id (0-31, ZONE_MAX in Zones.h) instead of a
    // TankState — every record already in EEPROM predates these event codes,
    // so old records are unaffected and need no migration.
    HIST_ZONE_OPEN    = 7,   // ohState field holds the zone id
    HIST_ZONE_CLOSE   = 8,   // ohState field holds the zone id
} HistEvent;

// Reason a motor/boot event happened.  Packed into the HIGH nibble (bits 4-7)
// of HistoryRecord.flags — interpret together with the ON/OFF event type.
// Legacy records (written before this field existed) read back as REASON_NONE.
typedef enum : uint8_t {
    REASON_NONE          = 0,   // unknown / not applicable
    REASON_AUTO          = 1,   // ON: tank reached start level
    REASON_MANUAL_APP    = 2,   // manual via mobile app / BLE
    REASON_MANUAL_WEB    = 3,   // manual via web UI
    REASON_MANUAL_TOUCH  = 4,   // manual via touch button
    REASON_SCHEDULED     = 5,   // scheduler start/stop
    REASON_AUTO_FULL     = 6,   // OFF: tank reached stop/full level
    REASON_MAX_RUNTIME   = 7,   // OFF: max-runtime safety cutoff
    REASON_LORA_LOST     = 8,   // OFF: no-LoRa-signal safety cutoff
    REASON_POWER_CUT     = 9,   // OFF: inferred power failure (backdated)
    REASON_POWER_RESTORE = 10,  // ON: resumed after power/LoRa restore
    REASON_BOOT_POWER    = 11,  // BOOT: power-on reset
    REASON_BOOT_SW       = 12,  // BOOT: software / OTA reset
    REASON_BOOT_BROWN    = 13,  // BOOT: brownout reset
    REASON_BOOT_OTHER    = 14,  // BOOT: panic / watchdog / other
    REASON_PROTECTION    = 15,  // OFF: electrical protection trip
    REASON_LOCKOUT       = 16,  // OFF/refused: maintenance lockout active
} HistReason;

// =============================================================================
//  8-byte packed record stored in EEPROM circular buffer
// =============================================================================
#pragma pack(push, 1)
struct HistoryRecord {
    uint32_t timestamp;   // Seconds since epoch (local IST)
    uint8_t  event;       // HistEvent
    uint8_t  ohState;     // TankState for OH tank
    uint8_t  ugState;     // TankState for UG tank
    uint8_t  flags;       // bit0 = ohMotorRunning, bit1 = ugMotorRunning
};
#pragma pack(pop)

// =============================================================================
//  Public interface
// =============================================================================
extern bool histEepromFound;

// Call once from setup() after Wire.begin()
void    initHistory();

// Record one event. oh/ug = current tank states at time of event.
// reason     = why it happened (HistReason), stored in the flags high nibble.
// tsOverride = explicit epoch (0 = use current time); used to backdate a
//              synthetic power-cut OFF to when power was actually lost.
void    addHistoryRecord(HistEvent evt, TankState oh, TankState ug,
                         uint8_t reason = REASON_NONE, uint32_t tsOverride = 0);

// Convenience wrapper for HIST_ZONE_OPEN / HIST_ZONE_CLOSE — packs zoneId into
// the repurposed field so call sites in Zones.cpp never touch that detail.
void    addZoneHistoryRecord(HistEvent evt, uint8_t zoneId, uint8_t reason = REASON_NONE);

// Detected EEPROM I2C address (valid after initHistory when histEepromFound).
uint8_t getEepromAddress();

// Return last maxRecords events as a JSON string (newest first).
String  getHistoryJson(uint16_t maxRecords = 100);

// Wipe the circular buffer (keeps EEPROM magic intact).
void    clearHistory();

#endif // HISTORY_H
