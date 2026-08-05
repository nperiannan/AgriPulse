#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <Arduino.h>

#define MAX_SCHEDULES 10

struct Schedule {
    bool     enabled;
    uint8_t  motorType;  // 0 = OH, 1 = UG
    String   time;       // "HH:MM"
    uint16_t duration;   // minutes
    bool     isRunning;
    unsigned long startTime; // millis() when started
    bool     preBuzzing;     // true while the pre-start warning buzzer (before `time`) is sounding
};

extern Schedule schedules[MAX_SCHEDULES];

void initScheduler();
void checkSchedules();
void saveSchedules();
void clearAllSchedules();

#endif // SCHEDULER_H
