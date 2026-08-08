#ifndef BUZZER_H
#define BUZZER_H

#include <Arduino.h>

typedef enum : uint8_t {
    BUZZER_NONE        = 0,
    BUZZER_COUNTDOWN   = 1,  // Long gap shrinking to rapid over 30 s – motor-start warning
    BUZZER_SHORT_BEEPS = 2,  // 200 ms on / 800 ms off – alert / error
} BuzzerPattern;

void initBuzzer();
void startBuzzer(BuzzerPattern pattern);
void stopBuzzer();
void updateBuzzer();
bool isBuzzerActive();

// Whether the pre-start countdown warning (BUZZER_COUNTDOWN, sounded for
// MOTOR_START_BUZZER_DELAY_MS before a motor start) makes any sound.
// Persisted, defaults on. The warning DELAY itself is a fixed safety window
// and always runs regardless of this setting — only whether it's audible is
// configurable. Never affects BUZZER_SHORT_BEEPS: that's the welded-contactor
// fault alarm and stays audible no matter what this is set to.
void buzzerSetCountdownEnabled(bool on);
bool buzzerCountdownEnabled();

#endif // BUZZER_H
