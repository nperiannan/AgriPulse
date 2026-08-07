#include "I2CScan.h"
#include "Config.h"
#include "Logger.h"
#include <Wire.h>

// Standard 7-bit addressable range. 0x00-0x07 and 0x78-0x7F are reserved.
#define I2C_ADDR_FIRST 0x08
#define I2C_ADDR_LAST  0x77

// ---------------------------------------------------------------------------
//  Idle-level check and bus recovery
// ---------------------------------------------------------------------------

// Read the bus at rest, before Wire owns the pins.
//
// Read FLOATING, not with INPUT_PULLUP. The ESP32's internal pull-up would drag
// both lines HIGH whether or not the board has real pull-up resistors fitted and
// whether or not any device is present, so an INPUT_PULLUP reading of HIGH says
// nothing at all. Floating distinguishes the cases that matter:
//   floating HIGH          - external pull-ups fitted, bus idle: healthy
//   floating LOW + pull-up HIGH - no external pull-ups (or nothing powered)
//   LOW even with pull-up  - something is clamping: stuck slave or short
void i2cReadIdleLevels(bool* sdaHigh, bool* sclHigh) {
    pinMode(SDA_PIN, INPUT);
    pinMode(SCL_PIN, INPUT);
    delayMicroseconds(200);
    *sdaHigh = digitalRead(SDA_PIN) == HIGH;
    *sclHigh = digitalRead(SCL_PIN) == HIGH;
}

// Same two pins read with the internal pull-up engaged. Compared against the
// floating reading, this separates "no pull-ups" from "line held down".
void i2cReadPulledLevels(bool* sdaHigh, bool* sclHigh) {
    pinMode(SDA_PIN, INPUT_PULLUP);
    pinMode(SCL_PIN, INPUT_PULLUP);
    delayMicroseconds(200);
    *sdaHigh = digitalRead(SDA_PIN) == HIGH;
    *sclHigh = digitalRead(SCL_PIN) == HIGH;
}

// Are SDA and SCL the same electrical node?
//
// Worth testing explicitly because a short between the two lines is invisible to
// every passive check: they idle HIGH together, they pull HIGH together, nothing
// looks clamped — and yet no device can ever ACK, because clock and data cannot
// move independently. That reads as "healthy but empty bus", which is exactly
// the wrong conclusion. (This is a real fault found on this board, 2026-08-07.)
//
// The test is active: drive one line LOW and watch whether the other follows.
bool i2cLinesShorted() {
    // Drive SDA low, SCL released. If SCL follows SDA down, they are tied.
    pinMode(SCL_PIN, INPUT_PULLUP);
    pinMode(SDA_PIN, OUTPUT_OPEN_DRAIN);
    digitalWrite(SDA_PIN, LOW);
    delayMicroseconds(200);
    bool sclFollowedSda = (digitalRead(SCL_PIN) == LOW);
    digitalWrite(SDA_PIN, HIGH);
    pinMode(SDA_PIN, INPUT_PULLUP);
    delayMicroseconds(200);

    // And the mirror image, so a stuck line can't fake the result.
    pinMode(SDA_PIN, INPUT_PULLUP);
    pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(200);
    bool sdaFollowedScl = (digitalRead(SDA_PIN) == LOW);
    digitalWrite(SCL_PIN, HIGH);
    pinMode(SCL_PIN, INPUT_PULLUP);
    delayMicroseconds(200);

    return sclFollowedSda && sdaFollowedScl;
}

// Free a slave that is holding SDA low. It is waiting to finish a transfer that
// the reset interrupted, so give it the clocks it expects until it releases the
// line, then issue a STOP. Must run before Wire.begin() takes the pins.
bool i2cBusRecover() {
    pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(SDA_PIN, INPUT_PULLUP);
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(10);

    for (uint8_t i = 0; i < 16 && digitalRead(SDA_PIN) == LOW; i++) {
        digitalWrite(SCL_PIN, LOW);
        delayMicroseconds(10);
        digitalWrite(SCL_PIN, HIGH);
        delayMicroseconds(10);
    }

    // STOP: SDA rises while SCL is high.
    pinMode(SDA_PIN, OUTPUT_OPEN_DRAIN);
    digitalWrite(SDA_PIN, LOW);
    delayMicroseconds(10);
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(SDA_PIN, HIGH);
    delayMicroseconds(10);

    pinMode(SDA_PIN, INPUT_PULLUP);
    pinMode(SCL_PIN, INPUT_PULLUP);
    delayMicroseconds(50);
    return digitalRead(SDA_PIN) == HIGH;
}

static I2CScanResult scanBus(uint8_t sda, uint8_t scl) {
    I2CScanResult r = {};
    r.sda = sda;
    r.scl = scl;
    for (uint8_t a = I2C_ADDR_FIRST; a <= I2C_ADDR_LAST; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            if (r.found < (uint8_t)(sizeof(r.addrs))) r.addrs[r.found] = a;
            r.found++;
        }
    }
    return r;
}

I2CScanResult i2cScanCurrent() {
    return scanBus(SDA_PIN, SCL_PIN);
}

static String describe(const I2CScanResult& r) {
    String s = "SDA" + String(r.sda) + "/SCL" + String(r.scl) + ": ";
    if (r.found == 0) return s + "no devices";
    s += String(r.found) + " device(s) -";
    uint8_t n = r.found < 16 ? r.found : 16;
    for (uint8_t i = 0; i < n; i++) s += " 0x" + String(r.addrs[i], HEX);
    return s;
}

void i2cDiagnoseBothPinouts() {
    // Never probe the other revision's pinout on this board: on v1.x GPIO8 is
    // LORA_DIO1, already owned by RadioLib's interrupt. Reconfiguring it as SDA
    // hangs setup() before the radio, web server or zones ever start. The board
    // is v1.0, so the compiled-in pins are the only ones worth testing.

    // 1. Floating read: is anything holding the lines, and are pull-ups fitted?
    bool fSda = false, fScl = false;
    i2cReadIdleLevels(&fSda, &fScl);

    // 2. Same pins with the internal pull-up, to tell "no pull-ups" from "clamped".
    bool pSda = false, pScl = false;
    i2cReadPulledLevels(&pSda, &pScl);

    Log(INFO, String("[I2C] SDA") + SDA_PIN + " floating=" + (fSda ? "HIGH" : "LOW")
              + " pulled=" + (pSda ? "HIGH" : "LOW")
              + "   SCL" + SCL_PIN + " floating=" + (fScl ? "HIGH" : "LOW")
              + " pulled=" + (pScl ? "HIGH" : "LOW"));

    const bool clamped     = (!pSda || !pScl);          // low even with pull-up
    const bool noPullups   = (!fSda || !fScl) && !clamped;

    // Check for a short between the two lines before anything else. A crossed
    // bus mimics a perfectly healthy one on every passive reading, so without
    // this the verdict below would confidently say "healthy but empty".
    if (!clamped) {
        if (i2cLinesShorted()) {
            Log(ERROR, "==================================================");
            Log(ERROR, "[I2C] SDA" + String(SDA_PIN) + " and SCL" + String(SCL_PIN)
                       + " are SHORTED TOGETHER - driving either one pulls the other.");
            Log(ERROR, "[I2C] Clock and data cannot move independently, so no device");
            Log(ERROR, "[I2C] can ever acknowledge. Separate the two lines; every");
            Log(ERROR, "[I2C] other reading looks healthy and will mislead you.");
            Log(ERROR, "==================================================");
            // Restore the bus and stop: scanning a shorted bus tells you nothing.
            Wire.end();
            delay(20);
            Wire.begin(SDA_PIN, SCL_PIN);
            return;
        }
    }

    if (clamped) {
        Log(WARN, "[I2C] A line stays LOW even with the internal pull-up - "
                  "something is clamping the bus. Recovering...");
        bool freed = i2cBusRecover();
        Log(freed ? INFO : ERROR,
            String("[I2C] Bus recovery ") + (freed ? "succeeded - line released"
                                                   : "FAILED - still held LOW"));
    }

    // 3. Bring the bus up and scan.
    Wire.end();
    delay(20);
    Wire.begin(SDA_PIN, SCL_PIN);
    delay(20);

    I2CScanResult cur = i2cScanCurrent();
    Log(INFO, "[I2C] " + describe(cur));
    if (cur.found > 0) return;   // bus is alive; nothing further to diagnose

    Log(ERROR, "==================================================");
    if (clamped) {
        Log(ERROR, "[I2C] Bus is clamped LOW and recovery did not clear it.");
        Log(ERROR, "[I2C] Fully power-cycle (not a reset) to release a latched");
        Log(ERROR, "[I2C] slave. If it persists, one module is faulty or shorted.");
    } else if (noPullups) {
        Log(ERROR, "[I2C] Lines float LOW without the internal pull-up, so there");
        Log(ERROR, "[I2C] are no working pull-up resistors on the bus - and the");
        Log(ERROR, "[I2C] modules that would normally provide them are not being");
        Log(ERROR, "[I2C] seen. Check 3.3V and GND at each module first: an");
        Log(ERROR, "[I2C] unpowered breakout supplies neither ACK nor pull-up.");
    } else {
        Log(ERROR, "[I2C] Pull-ups are present and nothing is clamping the bus,");
        Log(ERROR, "[I2C] yet no device ACKs anywhere in 0x08-0x77. The bus is");
        Log(ERROR, "[I2C] electrically healthy but empty - the modules are not");
        Log(ERROR, "[I2C] answering. Check 3.3V/GND at each module and that SDA");
        Log(ERROR, "[I2C] and SCL are seated on the right header pins.");
    }
    Log(ERROR, "==================================================");
}

String i2cScanJson() {
    I2CScanResult cur = i2cScanCurrent();

    String out = "{\"sda\":" + String(cur.sda) + ",\"scl\":" + String(cur.scl);
    out += ",\"found\":" + String(cur.found) + ",\"addrs\":[";
    uint8_t n = cur.found < 16 ? cur.found : 16;
    for (uint8_t i = 0; i < n; i++) {
        if (i) out += ",";
        out += String(cur.addrs[i]);
    }
    out += "]";

    // Line levels travel with the scan: "0 devices" plus "floating LOW" is a
    // different problem from "0 devices" plus "pull-ups fine", and the UI can
    // say which without the operator reading a serial log.
    bool fSda, fScl, pSda, pScl;
    i2cReadIdleLevels(&fSda, &fScl);
    i2cReadPulledLevels(&pSda, &pScl);
    // Reading the pins directly took them away from Wire; hand them back.
    Wire.end();
    delay(10);
    Wire.begin(SDA_PIN, SCL_PIN);

    out += ",\"sda_float\":" + String(fSda ? "true" : "false");
    out += ",\"scl_float\":" + String(fScl ? "true" : "false");
    out += ",\"sda_pulled\":" + String(pSda ? "true" : "false");
    out += ",\"scl_pulled\":" + String(pScl ? "true" : "false");
    out += ",\"pullups\":"    + String((fSda && fScl) ? "true" : "false");
    out += ",\"clamped\":"    + String((!pSda || !pScl) ? "true" : "false");
    out += "}";
    return out;
}
