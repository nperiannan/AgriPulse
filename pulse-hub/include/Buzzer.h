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

#endif // BUZZER_H
