#ifndef ADE7758_H
#define ADE7758_H

#include <Arduino.h>

// ADE7758 3-phase energy meter driver.
//
// Provides per-phase RMS voltage, RMS current and line frequency, plus the
// hardware event flags (sag, peak, zero-crossing timeout, phase-sequence error)
// that the motor protection layer trips on.
//
// Threading: the IRQ handler only sets a flag.  All SPI traffic happens in
// adePoll() from the main loop — the previous-generation controller corrupted
// radio transactions by doing SPI inside the meter ISR.

enum AdePhase : uint8_t {
    ADE_PHASE_A = 0,
    ADE_PHASE_B = 1,
    ADE_PHASE_C = 2,
    ADE_PHASE_COUNT = 3,
};

// Latched hardware events, drained by adeTakeEvents().
#define ADE_EVT_SAG_A        (1UL << 0)
#define ADE_EVT_SAG_B        (1UL << 1)
#define ADE_EVT_SAG_C        (1UL << 2)
#define ADE_EVT_ZXTO_A       (1UL << 3)   // no zero crossing = phase A missing
#define ADE_EVT_ZXTO_B       (1UL << 4)
#define ADE_EVT_ZXTO_C       (1UL << 5)
#define ADE_EVT_PEAK_VOLTAGE (1UL << 6)
#define ADE_EVT_PEAK_CURRENT (1UL << 7)
#define ADE_EVT_SEQ_ERROR    (1UL << 8)   // phase rotation wrong
#define ADE_EVT_METER_RESET  (1UL << 9)

#define ADE_EVT_ANY_SAG  (ADE_EVT_SAG_A  | ADE_EVT_SAG_B  | ADE_EVT_SAG_C)
#define ADE_EVT_ANY_ZXTO (ADE_EVT_ZXTO_A | ADE_EVT_ZXTO_B | ADE_EVT_ZXTO_C)

struct AdeReadings {
    float volts[ADE_PHASE_COUNT];
    float amps[ADE_PHASE_COUNT];
    float frequency[ADE_PHASE_COUNT];
    bool  phasePresent[ADE_PHASE_COUNT];  // voltage above the noise floor
    bool  phaseSequenceOk;
    bool  valid;                          // false until a successful poll
    unsigned long lastUpdateMs;
};

struct AdeCalibration {
    float voltScale[ADE_PHASE_COUNT];     // raw VRMS count -> volts
    float ampScale[ADE_PHASE_COUNT];      // raw IRMS count -> amps
    bool  calibrated;                     // false until real CT data is entered
};

// Configure SPI, reset the meter and arm its interrupt sources.
bool adeInit();

// Read the meter. Rate-limited internally to ADE_POLL_INTERVAL_MS; call freely.
void adePoll();

const AdeReadings& adeGetReadings();

// True while the meter is responding. Goes false after ADE_FAIL_ALARM
// consecutive comms failures, having already attempted a soft reset.
bool adeIsHealthy();

// Returns and clears the latched event bits (ADE_EVT_*).
uint32_t adeTakeEvents();

// Highest current across all three phases — the value protection trips on.
float adeMaxAmps();

// Largest difference between phase currents, as a fraction of the mean (0..1).
// Returns 0 when current is too low to judge. Sustained imbalance means a
// failing phase; this is how single-phasing is caught while running.
float adeCurrentImbalance();

// Calibration is runtime-settable because the CT ratio is not yet known.
// Until adeSetCalibrated(true), current readings are reported but must not be
// used to trip protection — the scaling is a placeholder.
AdeCalibration& adeCalibration();
void adeLoadCalibration();
void adeSaveCalibration();
void adeSetCalibrated(bool done);

#endif // ADE7758_H
