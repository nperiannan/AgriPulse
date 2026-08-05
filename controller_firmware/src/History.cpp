/**
 * History.cpp
 *
 * Circular-buffer event log stored in AT24C512 EEPROM (I2C).
 *
 * EEPROM layout (64 KB):
 *   Addr 0-7    : Header  – magic(2) + head(2) + count(2) + reserved(2)
 *   Addr 8+     : Records – 8 bytes each, 8191 records max
 *
 * The head pointer always points to the NEXT write slot.
 * Newest record is at (head - 1), second newest at (head - 2), etc.
 */

#include "History.h"
#include "Logger.h"
#include "Config.h"
#include <Wire.h>
#include <TimeLib.h>
#include <esp_system.h>   // esp_reset_reason() for boot-event tagging

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
static const uint16_t HIST_MAGIC       = 0xA5C3;
static const uint16_t HIST_DATA_START  = HIST_DATA_ADDR;                             // 8
static const uint16_t HIST_REC_SIZE    = (uint16_t)sizeof(HistoryRecord);            // 8
static const uint16_t HIST_MAX_RECORDS =
    (uint16_t)((EEPROM_SIZE_BYTES - HIST_DATA_START) / HIST_REC_SIZE);               // 8191

// ---------------------------------------------------------------------------
//  Runtime state
// ---------------------------------------------------------------------------
bool     histEepromFound = false;
static uint8_t  eepromAddr  = EEPROM_I2C_ADDR;
static uint16_t histHead    = 0;    // index of next write slot
static uint16_t histCount   = 0;    // number of valid records

// ---------------------------------------------------------------------------
//  Header struct (8 bytes at address 0)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct HistHeader {
    uint16_t magic;
    uint16_t head;
    uint16_t count;
    uint16_t reserved;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
//  Low-level EEPROM I2C helpers
// ---------------------------------------------------------------------------
static bool eepromWriteBytes(uint16_t addr, const uint8_t* data, uint8_t len) {
    Wire.beginTransmission(eepromAddr);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    Wire.write(data, len);
    if (Wire.endTransmission() != 0) return false;
    delay(10);  // AT24C512 write-cycle time (max 10 ms)
    return true;
}

static bool eepromReadBytes(uint16_t addr, uint8_t* data, uint8_t len) {
    Wire.beginTransmission(eepromAddr);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    if (Wire.endTransmission() != 0) return false;
    uint8_t n = (uint8_t)Wire.requestFrom(eepromAddr, len);
    if (n != len) return false;
    for (uint8_t i = 0; i < len; i++) data[i] = Wire.read();
    return true;
}

// ---------------------------------------------------------------------------
//  Header helpers
// ---------------------------------------------------------------------------
static bool writeHeader() {
    HistHeader h = { HIST_MAGIC, histHead, histCount, 0 };
    return eepromWriteBytes(HIST_HEADER_ADDR, (uint8_t*)&h, sizeof(h));
}

// ---------------------------------------------------------------------------
//  Record address calculation
// ---------------------------------------------------------------------------
static inline uint16_t recAddr(uint16_t idx) {
    return HIST_DATA_START + idx * HIST_REC_SIZE;
}

// ---------------------------------------------------------------------------
//  EEPROM auto-detect (tries 0x50-0x57)
// ---------------------------------------------------------------------------
static bool detectEeprom() {
    Wire.setClock(100000);  // Drop to 100 kHz for reliable detection
    delay(50);

    const uint8_t addrs[] = { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57 };
    for (int i = 0; i < 8; i++) {
        Wire.beginTransmission(addrs[i]);
        if (Wire.endTransmission() == 0) {
            eepromAddr = addrs[i];
            Log(INFO, "[History] EEPROM found at 0x" + String(addrs[i], HEX));
            Wire.setClock(400000);  // Restore bus speed
            return true;
        }
    }
    Wire.setClock(400000);
    Log(WARN, "[History] EEPROM not found at 0x50-0x57");
    return false;
}

// Map the ESP32 reset cause to a boot reason code for the HIST_BOOT record.
static uint8_t bootReasonCode() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return REASON_BOOT_POWER;
        case ESP_RST_BROWNOUT:  return REASON_BOOT_BROWN;
        case ESP_RST_SW:
        case ESP_RST_DEEPSLEEP:  return REASON_BOOT_SW;
        default:                return REASON_BOOT_OTHER;
    }
}

// ---------------------------------------------------------------------------
//  initHistory()
// ---------------------------------------------------------------------------
void initHistory() {
    if (!detectEeprom()) {
        histEepromFound = false;
        return;
    }
    histEepromFound = true;

    HistHeader h;
    if (!eepromReadBytes(HIST_HEADER_ADDR, (uint8_t*)&h, sizeof(h))) {
        Log(WARN, "[History] Header read failed – resetting");
        histHead  = 0;
        histCount = 0;
        writeHeader();
        addHistoryRecord(HIST_BOOT, ohTankState, ugTankState, bootReasonCode());
        return;
    }

    if (h.magic != HIST_MAGIC) {
        Log(INFO, "[History] Fresh EEPROM – initialising");
        histHead  = 0;
        histCount = 0;
        writeHeader();
    } else {
        histHead  = h.head  % HIST_MAX_RECORDS;
        histCount = min(h.count, HIST_MAX_RECORDS);
        Log(INFO, "[History] Loaded – " + String(histCount) + " records, head=" + String(histHead));
    }

    addHistoryRecord(HIST_BOOT, ohTankState, ugTankState, bootReasonCode());
}

// ---------------------------------------------------------------------------
//  addHistoryRecord()
// ---------------------------------------------------------------------------
void addHistoryRecord(HistEvent evt, TankState oh, TankState ug, uint8_t reason, uint32_t tsOverride) {
    if (!histEepromFound) return;

    HistoryRecord r;
    r.timestamp = tsOverride ? tsOverride : (uint32_t)now();
    r.event     = (uint8_t)evt;
    r.ohState   = (uint8_t)oh;
    r.ugState   = (uint8_t)ug;
    // Low nibble = motor-running bits; high nibble = reason code (0-15).
    r.flags     = (ohMotorRunning ? 0x01 : 0x00)
                | (ugMotorRunning ? 0x02 : 0x00)
                | (uint8_t)((reason & 0x0F) << 4);

    if (!eepromWriteBytes(recAddr(histHead), (uint8_t*)&r, sizeof(r))) {
        Log(WARN, "[History] Write failed at slot " + String(histHead));
        return;
    }

    histHead = (histHead + 1) % HIST_MAX_RECORDS;
    if (histCount < HIST_MAX_RECORDS) histCount++;
    writeHeader();
}

// ---------------------------------------------------------------------------
//  Formatting helpers
// ---------------------------------------------------------------------------
static const char* evtStr(uint8_t e) {
    switch (e) {
        case HIST_MOTOR_OH_ON:  return "OH Motor ON";
        case HIST_MOTOR_OH_OFF: return "OH Motor OFF";
        case HIST_MOTOR_UG_ON:  return "UG Motor ON";
        case HIST_MOTOR_UG_OFF: return "UG Motor OFF";
        case HIST_OH_STATE_CHG: return "OH State";
        case HIST_UG_STATE_CHG: return "UG State";
        case HIST_BOOT:         return "Boot";
        default:                return "?";
    }
}

static const char* stStr(uint8_t s) {
    switch (s) {
        case TANK_STATE_FULL: return "FULL";
        case TANK_STATE_LOW:  return "LOW";
        default:              return "?";
    }
}

static const char* reasonStr(uint8_t rc) {
    switch (rc) {
        case REASON_AUTO:          return "Auto (level)";
        case REASON_MANUAL_APP:    return "Manual (app)";
        case REASON_MANUAL_WEB:    return "Manual (web)";
        case REASON_MANUAL_TOUCH:  return "Manual (touch)";
        case REASON_SCHEDULED:     return "Scheduled";
        case REASON_AUTO_FULL:     return "Auto (tank full)";
        case REASON_MAX_RUNTIME:   return "Max runtime";
        case REASON_LORA_LOST:     return "LoRa signal lost";
        case REASON_POWER_CUT:     return "Power cut";
        case REASON_POWER_RESTORE: return "Power restored";
        case REASON_BOOT_POWER:    return "Power-on";
        case REASON_BOOT_SW:       return "Reboot/OTA";
        case REASON_BOOT_BROWN:    return "Brownout";
        case REASON_BOOT_OTHER:    return "Reset";
        default:                   return "";
    }
}

// Epoch is stored as local IST (same convention as system clock); use gmtime_r
// since no additional TZ offset should be applied.
static String fmtEpoch(uint32_t epoch) {
    struct tm t = {};
    time_t e = (time_t)epoch;
    gmtime_r(&e, &t);
    int h12 = t.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    const char* ap = (t.tm_hour < 12) ? "AM" : "PM";
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d %s %02d-%02d-%04d",
             h12, t.tm_min, ap, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    return String(buf);
}

// ---------------------------------------------------------------------------
//  getHistoryJson()  – returns newest-first JSON
// ---------------------------------------------------------------------------
String getHistoryJson(uint16_t maxRecords) {
    String out = "{\"count\":";
    out += histCount;
    out += ",\"eeprom\":";
    out += histEepromFound ? "true" : "false";
    out += ",\"records\":[";

    if (!histEepromFound || histCount == 0) {
        out += "]}";
        return out;
    }

    uint16_t actual = (histCount < maxRecords) ? histCount : maxRecords;
    bool first = true;

    for (uint16_t i = 0; i < actual; i++) {
        // Walk backwards from most recent
        uint32_t rawIdx = (uint32_t)histHead + HIST_MAX_RECORDS - 1 - i;
        uint16_t idx    = (uint16_t)(rawIdx % HIST_MAX_RECORDS);

        HistoryRecord r;
        if (!eepromReadBytes(recAddr(idx), (uint8_t*)&r, sizeof(r))) continue;

        if (!first) out += ',';
        first = false;

        out += "{\"ts\":";    out += r.timestamp;
        out += ",\"time\":\""; out += fmtEpoch(r.timestamp); out += '"';
        out += ",\"ev\":\"";   out += evtStr(r.event);        out += '"';
        out += ",\"oh\":\"";   out += stStr(r.ohState);       out += '"';
        out += ",\"ug\":\"";   out += stStr(r.ugState);       out += '"';
        out += ",\"ohM\":";    out += (r.flags & 0x01) ? "true" : "false";
        out += ",\"ugM\":";    out += (r.flags & 0x02) ? "true" : "false";
        uint8_t rc = (uint8_t)((r.flags >> 4) & 0x0F);
        out += ",\"rsn\":";    out += rc;
        out += ",\"rsnStr\":\""; out += reasonStr(rc); out += '"';
        out += '}';
    }

    out += "]}";
    return out;
}

// ---------------------------------------------------------------------------
//  clearHistory()
// ---------------------------------------------------------------------------
void clearHistory() {
    if (!histEepromFound) return;
    histHead  = 0;
    histCount = 0;
    writeHeader();
    Log(INFO, "[History] Cleared");
}

uint8_t getEepromAddress() { return eepromAddr; }
