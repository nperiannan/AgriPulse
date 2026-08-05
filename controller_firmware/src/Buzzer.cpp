#include "Buzzer.h"
#include "Logger.h"
#include "Config.h"

static bool          buzzerActive   = false;
static BuzzerPattern currentPattern = BUZZER_NONE;
static unsigned long buzzerStartMs  = 0;
static unsigned long nextToggleMs   = 0;
static bool          buzzerPinState = false;

// Countdown pattern: beep ON time is fixed 300 ms.
// OFF time starts at 2700 ms (3 s period) and shrinks linearly down to 100 ms
// over MOTOR_START_BUZZER_DELAY_MS milliseconds.
static const uint16_t COUNTDOWN_ON_MS      = 300;
static const uint16_t COUNTDOWN_OFF_START  = 2700;  // gap at t=0
static const uint16_t COUNTDOWN_OFF_END    = 100;   // gap near motor start

// Alarm escalation for SHORT_BEEPS: short snappy beeps that speed up over 12 s
static const uint16_t ALARM_ON_MS     = 80;    // short snappy pulse
static const uint16_t ALARM_OFF_START = 1420;  // wide gap at start (~1.5 s period)
static const uint16_t ALARM_OFF_END   = 120;   // tight gap at end  (~200 ms period)
static const uint32_t ALARM_RAMP_MS   = 12000; // full ramp duration

static uint16_t alarmOffMs() {
    unsigned long elapsed = millis() - buzzerStartMs;
    if (elapsed >= ALARM_RAMP_MS) return ALARM_OFF_END;
    float frac = (float)elapsed / (float)ALARM_RAMP_MS;
    return (uint16_t)(ALARM_OFF_START - frac * (ALARM_OFF_START - ALARM_OFF_END));
}

static uint16_t currentOffMs() {
    // Linearly interpolate gap based on elapsed time within the 30 s window
    unsigned long elapsed = millis() - buzzerStartMs;
    if (elapsed >= MOTOR_START_BUZZER_DELAY_MS) return COUNTDOWN_OFF_END;
    float frac = (float)elapsed / (float)MOTOR_START_BUZZER_DELAY_MS;
    return (uint16_t)(COUNTDOWN_OFF_START - frac * (COUNTDOWN_OFF_START - COUNTDOWN_OFF_END));
}

void initBuzzer() {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    Log(INFO, "[Buzzer] Init on GPIO " + String(BUZZER_PIN));
}

void startBuzzer(BuzzerPattern pattern) {
    buzzerActive    = true;
    currentPattern  = pattern;
    buzzerStartMs   = millis();
    buzzerPinState  = true;
    digitalWrite(BUZZER_PIN, HIGH);
    uint16_t firstOn = (pattern == BUZZER_SHORT_BEEPS) ? ALARM_ON_MS : COUNTDOWN_ON_MS;
    nextToggleMs = buzzerStartMs + firstOn;
    Log(INFO, "[Buzzer] Start pattern=" + String(pattern));
}

void stopBuzzer() {
    if (!buzzerActive) return;
    buzzerActive   = false;
    currentPattern = BUZZER_NONE;
    buzzerPinState = false;
    digitalWrite(BUZZER_PIN, LOW);  // active buzzer: LOW = off, no tone() needed
    Log(INFO, "[Buzzer] Stopped");
}

void updateBuzzer() {
    if (!buzzerActive) return;

    unsigned long now = millis();

    if (now - buzzerStartMs >= BUZZER_MAX_DURATION_MS) {
        stopBuzzer();
        return;
    }

    if (now < nextToggleMs) return;

    buzzerPinState = !buzzerPinState;
    digitalWrite(BUZZER_PIN, buzzerPinState ? HIGH : LOW);

    if (currentPattern == BUZZER_SHORT_BEEPS) {
        // Escalating alarm: beeps start slow then speed up over ALARM_RAMP_MS
        nextToggleMs = now + (buzzerPinState ? ALARM_ON_MS : alarmOffMs());
    } else {
        // BUZZER_COUNTDOWN: fixed ON, dynamic OFF that shrinks over time
        nextToggleMs = now + (buzzerPinState ? COUNTDOWN_ON_MS : currentOffMs());
    }
}

bool isBuzzerActive() {
    return buzzerActive;
}
