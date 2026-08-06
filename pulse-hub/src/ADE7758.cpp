#include "ADE7758.h"
#include "Config.h"
#include "Logger.h"

#include <SPI.h>
#include <Preferences.h>

// -----------------------------------------------------------------------------
//  Register map (ADE7758 datasheet, Table 17)
// -----------------------------------------------------------------------------
#define ADE_REG_AIRMS     0x0A   // 24-bit
#define ADE_REG_BIRMS     0x0B
#define ADE_REG_CIRMS     0x0C
#define ADE_REG_AVRMS     0x0D   // 24-bit
#define ADE_REG_BVRMS     0x0E
#define ADE_REG_CVRMS     0x0F
#define ADE_REG_FREQ      0x10   // 12-bit, line period
#define ADE_REG_OPMODE    0x13   // 8-bit
#define ADE_REG_MMODE     0x14   // 8-bit
#define ADE_REG_LCYCMODE  0x17   // 8-bit
#define ADE_REG_MASK      0x18   // 24-bit
#define ADE_REG_STATUS    0x19   // 24-bit
#define ADE_REG_RSTATUS   0x1A   // 24-bit, clears on read
#define ADE_REG_ZXTOUT    0x1B   // 16-bit
#define ADE_REG_SAGCYC    0x1D   // 8-bit
#define ADE_REG_SAGLVL    0x1E   // 8-bit

#define ADE_WRITE_FLAG    0x80

// STATUS / RSTATUS bit positions
#define ADE_ST_ZXTOA   (1UL << 3)
#define ADE_ST_ZXTOB   (1UL << 4)
#define ADE_ST_ZXTOC   (1UL << 5)
#define ADE_ST_RESET   (1UL << 10)
#define ADE_ST_SAGA    (1UL << 11)
#define ADE_ST_SAGB    (1UL << 12)
#define ADE_ST_SAGC    (1UL << 13)
#define ADE_ST_PKV     (1UL << 14)
#define ADE_ST_PKI     (1UL << 15)
#define ADE_ST_SEQERR  (1UL << 19)

// OPMODE power-on value. A read of 0x00 means the SPI link is dead: the part
// never reports an all-zero operating mode in normal use.
#define ADE_OPMODE_EXPECTED_MIN 0x01

// -----------------------------------------------------------------------------
//  State
// -----------------------------------------------------------------------------
static SPIClass       adeSpi(FSPI);          // LoRa owns HSPI; keep them separate
static SPISettings    adeSpiSettings(ADE_SPI_HZ, MSBFIRST, SPI_MODE1);

static AdeReadings    readings;
static AdeCalibration calibration;

static volatile bool  irqPending   = false;
static uint32_t       latchedEvents = 0;
static uint8_t        failCount     = 0;
static bool           healthy       = false;
static bool           initialised   = false;
static unsigned long  lastPollMs    = 0;

static void IRAM_ATTR adeIsr() {
    irqPending = true;   // no SPI here — drained in adePoll()
}

// -----------------------------------------------------------------------------
//  Low-level register access
// -----------------------------------------------------------------------------
static void adeSelect(bool on) {
    digitalWrite(ADE_CS_PIN, on ? LOW : HIGH);
}

static void adeWrite8(uint8_t reg, uint8_t value) {
    adeSpi.beginTransaction(adeSpiSettings);
    adeSelect(true);
    adeSpi.transfer(reg | ADE_WRITE_FLAG);
    adeSpi.transfer(value);
    adeSelect(false);
    adeSpi.endTransaction();
}

static void adeWrite16(uint8_t reg, uint16_t value) {
    adeSpi.beginTransaction(adeSpiSettings);
    adeSelect(true);
    adeSpi.transfer(reg | ADE_WRITE_FLAG);
    adeSpi.transfer((uint8_t)(value >> 8));
    adeSpi.transfer((uint8_t)(value & 0xFF));
    adeSelect(false);
    adeSpi.endTransaction();
}

static void adeWrite24(uint8_t reg, uint32_t value) {
    adeSpi.beginTransaction(adeSpiSettings);
    adeSelect(true);
    adeSpi.transfer(reg | ADE_WRITE_FLAG);
    adeSpi.transfer((uint8_t)(value >> 16));
    adeSpi.transfer((uint8_t)(value >> 8));
    adeSpi.transfer((uint8_t)(value & 0xFF));
    adeSelect(false);
    adeSpi.endTransaction();
}

static uint8_t adeRead8(uint8_t reg) {
    adeSpi.beginTransaction(adeSpiSettings);
    adeSelect(true);
    adeSpi.transfer(reg);
    delayMicroseconds(5);          // datasheet t6: gap before the part drives data
    uint8_t v = adeSpi.transfer(0x00);
    adeSelect(false);
    adeSpi.endTransaction();
    return v;
}

static uint16_t adeRead16(uint8_t reg) {
    adeSpi.beginTransaction(adeSpiSettings);
    adeSelect(true);
    adeSpi.transfer(reg);
    delayMicroseconds(5);
    uint16_t hi = adeSpi.transfer(0x00);
    uint16_t lo = adeSpi.transfer(0x00);
    adeSelect(false);
    adeSpi.endTransaction();
    return (uint16_t)((hi << 8) | lo);
}

// Widen to uint32_t before shifting. The previous controller read garbage here
// because on AVR an unsigned char promotes to a 16-bit int and the <<16 was
// truncated; ESP32's 32-bit int makes that safe, but being explicit is free.
static uint32_t adeRead24(uint8_t reg) {
    adeSpi.beginTransaction(adeSpiSettings);
    adeSelect(true);
    adeSpi.transfer(reg);
    delayMicroseconds(5);
    uint32_t b2 = adeSpi.transfer(0x00);
    uint32_t b1 = adeSpi.transfer(0x00);
    uint32_t b0 = adeSpi.transfer(0x00);
    adeSelect(false);
    adeSpi.endTransaction();
    return (b2 << 16) | (b1 << 8) | b0;
}

// -----------------------------------------------------------------------------
//  Calibration persistence
// -----------------------------------------------------------------------------
AdeCalibration& adeCalibration() {
    return calibration;
}

void adeLoadCalibration() {
    Preferences prefs;
    prefs.begin(NVS_ADE_NS, true);
    calibration.voltScale[ADE_PHASE_A] = prefs.getFloat(NVS_KEY_ADE_VSCALE_A, ADE_VOLT_SCALE_A_DEFAULT);
    calibration.voltScale[ADE_PHASE_B] = prefs.getFloat(NVS_KEY_ADE_VSCALE_B, ADE_VOLT_SCALE_B_DEFAULT);
    calibration.voltScale[ADE_PHASE_C] = prefs.getFloat(NVS_KEY_ADE_VSCALE_C, ADE_VOLT_SCALE_C_DEFAULT);
    calibration.ampScale[ADE_PHASE_A]  = prefs.getFloat(NVS_KEY_ADE_ISCALE_A, ADE_AMP_SCALE_DEFAULT);
    calibration.ampScale[ADE_PHASE_B]  = prefs.getFloat(NVS_KEY_ADE_ISCALE_B, ADE_AMP_SCALE_DEFAULT);
    calibration.ampScale[ADE_PHASE_C]  = prefs.getFloat(NVS_KEY_ADE_ISCALE_C, ADE_AMP_SCALE_DEFAULT);
    calibration.calibrated             = prefs.getBool (NVS_KEY_ADE_CALIBRATED, false);
    prefs.end();

    if (!calibration.calibrated) {
        Log(WARN, "[ADE] Uncalibrated - CT ratio not set. Current-based trips stay disarmed.");
    }
}

void adeSaveCalibration() {
    Preferences prefs;
    prefs.begin(NVS_ADE_NS, false);
    prefs.putFloat(NVS_KEY_ADE_VSCALE_A, calibration.voltScale[ADE_PHASE_A]);
    prefs.putFloat(NVS_KEY_ADE_VSCALE_B, calibration.voltScale[ADE_PHASE_B]);
    prefs.putFloat(NVS_KEY_ADE_VSCALE_C, calibration.voltScale[ADE_PHASE_C]);
    prefs.putFloat(NVS_KEY_ADE_ISCALE_A, calibration.ampScale[ADE_PHASE_A]);
    prefs.putFloat(NVS_KEY_ADE_ISCALE_B, calibration.ampScale[ADE_PHASE_B]);
    prefs.putFloat(NVS_KEY_ADE_ISCALE_C, calibration.ampScale[ADE_PHASE_C]);
    prefs.putBool (NVS_KEY_ADE_CALIBRATED, calibration.calibrated);
    prefs.end();
}

void adeSetCalibrated(bool done) {
    calibration.calibrated = done;
    adeSaveCalibration();
    Log(INFO, String("[ADE] Calibration flag -> ") + (done ? "calibrated" : "uncalibrated"));
}

// -----------------------------------------------------------------------------
//  Init
// -----------------------------------------------------------------------------
static void adeConfigureEvents() {
    // Arm the events protection cares about. Everything else stays masked so a
    // busy energy-accumulation flag can't swamp the IRQ line.
    uint32_t mask = ADE_ST_SAGA | ADE_ST_SAGB | ADE_ST_SAGC |
                    ADE_ST_ZXTOA | ADE_ST_ZXTOB | ADE_ST_ZXTOC |
                    ADE_ST_PKI | ADE_ST_PKV | ADE_ST_SEQERR;
    adeWrite24(ADE_REG_MASK, mask);
    adeRead24(ADE_REG_RSTATUS);   // clear anything already latched
}

bool adeInit() {
    pinMode(ADE_CS_PIN, OUTPUT);
    adeSelect(false);             // deselect before the bus comes up

    adeSpi.begin(ADE_SCLK_PIN, ADE_MISO_PIN, ADE_MOSI_PIN, ADE_CS_PIN);

    pinMode(ADE_IRQ_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ADE_IRQ_PIN), adeIsr, FALLING);

    adeLoadCalibration();

    for (uint8_t i = 0; i < ADE_PHASE_COUNT; i++) {
        readings.volts[i]        = 0.0f;
        readings.amps[i]         = 0.0f;
        readings.frequency[i]    = 0.0f;
        readings.phasePresent[i] = false;
    }
    readings.phaseSequenceOk = false;
    readings.valid           = false;
    readings.lastUpdateMs    = 0;

    uint8_t opmode = adeRead8(ADE_REG_OPMODE);
    if (opmode < ADE_OPMODE_EXPECTED_MIN) {
        Log(ERROR, "[ADE] Not responding on SPI (OPMODE=0x00) - check wiring/pins");
        healthy     = false;
        initialised = false;
        return false;
    }

    adeConfigureEvents();
    healthy     = true;
    initialised = true;
    failCount   = 0;
    Log(INFO, "[ADE] ADE7758 online (OPMODE=0x" + String(opmode, HEX) + ")");
    return true;
}

// -----------------------------------------------------------------------------
//  Polling
// -----------------------------------------------------------------------------
static void adeHandleCommsFailure() {
    failCount++;
    if (failCount == ADE_FAIL_SOFT_RESET) {
        Log(WARN, "[ADE] Comms failure - attempting soft reset");
        adeWrite8(ADE_REG_OPMODE, 0x40);   // OPMODE bit 6 = software reset
        delay(50);
        adeConfigureEvents();
    } else if (failCount >= ADE_FAIL_ALARM) {
        if (healthy) {
            Log(ERROR, "[ADE] Meter unreachable - protection cannot be trusted");
        }
        healthy        = false;
        readings.valid = false;
    }
}

static void adeDrainEvents() {
    uint32_t status = adeRead24(ADE_REG_RSTATUS);
    if (status & ADE_ST_SAGA)   latchedEvents |= ADE_EVT_SAG_A;
    if (status & ADE_ST_SAGB)   latchedEvents |= ADE_EVT_SAG_B;
    if (status & ADE_ST_SAGC)   latchedEvents |= ADE_EVT_SAG_C;
    if (status & ADE_ST_ZXTOA)  latchedEvents |= ADE_EVT_ZXTO_A;
    if (status & ADE_ST_ZXTOB)  latchedEvents |= ADE_EVT_ZXTO_B;
    if (status & ADE_ST_ZXTOC)  latchedEvents |= ADE_EVT_ZXTO_C;
    if (status & ADE_ST_PKV)    latchedEvents |= ADE_EVT_PEAK_VOLTAGE;
    if (status & ADE_ST_PKI)    latchedEvents |= ADE_EVT_PEAK_CURRENT;
    if (status & ADE_ST_SEQERR) latchedEvents |= ADE_EVT_SEQ_ERROR;
    if (status & ADE_ST_RESET) {
        latchedEvents |= ADE_EVT_METER_RESET;
        Log(WARN, "[ADE] Meter reported a reset - re-arming interrupt mask");
        adeConfigureEvents();
    }
}

void adePoll() {
    if (!initialised) return;

    if (irqPending) {
        irqPending = false;
        adeDrainEvents();
    }

    if (lastPollMs != 0 && millis() - lastPollMs < ADE_POLL_INTERVAL_MS) return;
    lastPollMs = millis();

    uint8_t opmode = adeRead8(ADE_REG_OPMODE);
    if (opmode < ADE_OPMODE_EXPECTED_MIN) {
        adeHandleCommsFailure();
        return;
    }

    if (!healthy) Log(INFO, "[ADE] Meter responding again");
    healthy   = true;
    failCount = 0;

    const uint8_t vregs[ADE_PHASE_COUNT] = { ADE_REG_AVRMS, ADE_REG_BVRMS, ADE_REG_CVRMS };
    const uint8_t iregs[ADE_PHASE_COUNT] = { ADE_REG_AIRMS, ADE_REG_BIRMS, ADE_REG_CIRMS };

    for (uint8_t p = 0; p < ADE_PHASE_COUNT; p++) {
        float v = (float)adeRead24(vregs[p]) * calibration.voltScale[p];
        if (v < ADE_MIN_VALID_VOLTS) v = 0.0f;
        readings.volts[p]        = v;
        readings.phasePresent[p] = (v > 0.0f);

        float a = (float)adeRead24(iregs[p]) * calibration.ampScale[p];
        if (a < ADE_MIN_VALID_AMPS) a = 0.0f;
        readings.amps[p] = a;
    }

    // The period register counts clock ticks per line cycle; a missing phase
    // reads zero, so guard the division.
    uint16_t period = adeRead16(ADE_REG_FREQ);
    float lineHz = (period > 0) ? (223750.0f / (float)period) : 0.0f;
    for (uint8_t p = 0; p < ADE_PHASE_COUNT; p++) {
        readings.frequency[p] = readings.phasePresent[p] ? lineHz : 0.0f;
    }

    // Sequence is only meaningful with all three phases energised.
    bool allPresent = readings.phasePresent[ADE_PHASE_A] &&
                      readings.phasePresent[ADE_PHASE_B] &&
                      readings.phasePresent[ADE_PHASE_C];
    readings.phaseSequenceOk = allPresent && !(latchedEvents & ADE_EVT_SEQ_ERROR);

    readings.valid        = true;
    readings.lastUpdateMs = millis();
}

// -----------------------------------------------------------------------------
//  Accessors
// -----------------------------------------------------------------------------
const AdeReadings& adeGetReadings() {
    return readings;
}

bool adeIsHealthy() {
    return healthy;
}

uint32_t adeTakeEvents() {
    uint32_t e = latchedEvents;
    latchedEvents = 0;
    return e;
}

float adeMaxAmps() {
    float m = 0.0f;
    for (uint8_t p = 0; p < ADE_PHASE_COUNT; p++) {
        if (readings.amps[p] > m) m = readings.amps[p];
    }
    return m;
}

float adeCurrentImbalance() {
    float sum = 0.0f, lo = readings.amps[0], hi = readings.amps[0];
    for (uint8_t p = 0; p < ADE_PHASE_COUNT; p++) {
        sum += readings.amps[p];
        if (readings.amps[p] < lo) lo = readings.amps[p];
        if (readings.amps[p] > hi) hi = readings.amps[p];
    }
    float mean = sum / (float)ADE_PHASE_COUNT;
    if (mean < ADE_MIN_VALID_AMPS) return 0.0f;   // motor off; nothing to judge
    return (hi - lo) / mean;
}
