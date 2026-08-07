#ifndef PROGRAMS_H
#define PROGRAMS_H

#include <Arduino.h>
#include "Zones.h"

// Zone-based irrigation programs — the replacement for the tank-shaped
// Scheduler (which schedules a motor, not a zone). Zones run sequentially:
// one pump feeds them all, so a program is a queue, never a broadcast.

#define PROGRAM_MAX         12   // storage cap; programCount() is the live number
#define PROGRAM_MAX_STARTS  4

enum WaterDayMode : uint8_t {
    WDAY_SPECIFIC = 0,   // dayMask bit0=Sun..bit6=Sat
    WDAY_ODD      = 1,   // odd calendar dates
    WDAY_EVEN     = 2,   // even calendar dates
    WDAY_INTERVAL = 3,   // every intervalDays, from lastRunEpoch
};

struct ProgramState {
    bool         enabled;
    char         name[16];
    uint16_t     startMin[PROGRAM_MAX_STARTS];   // minutes since midnight; 0xFFFF = unused slot
    WaterDayMode dayMode;
    uint8_t      dayMask;        // WDAY_SPECIFIC
    uint8_t      intervalDays;   // WDAY_INTERVAL
    uint16_t     zoneMin[ZONE_MAX];   // minutes per zone slot, 0 = skip/unused
    MotorId      source;         // which motor this program waters from

    // Runtime (not persisted)
    bool     running;
    int8_t   currentZone;        // index into zone list, -1 = not running
    uint32_t lastRunEpoch;       // for WDAY_INTERVAL and "ran today" de-dupe
};

// Global settings shared by every program.
struct ProgramDefaults {
    MotorId  source;             // default water source (well/bore)
    uint8_t  seasonalPct;        // 10-200, scales every zone's run time
    uint8_t  rainDelayDays;      // >0 = skip all watering, decremented daily
    uint32_t rainDelaySetEpoch;  // day the delay was set, for the daily decrement
};

void programsInit();
void programsTask();          // call from loop()

uint8_t programCount();       // number of programs that actually exist, 0..PROGRAM_MAX
ProgramState&    programGet(uint8_t idx);
ProgramDefaults& programDefaults();

void programSave(uint8_t idx);
void programSaveDefaults();

// Freely add/remove programs — not a fixed set. Returns the new program's id,
// or 0xFF if PROGRAM_MAX is reached.
uint8_t programCreate();
bool    programDelete(uint8_t idx);   // shifts later programs down to keep the list dense

// Manual "run now" — runs the program's full zone sequence immediately,
// ignoring start time and watering-day rules (rain delay still applies).
bool programRunNow(uint8_t idx);
void programStop(uint8_t idx);

const char* waterDayModeName(WaterDayMode m);

#endif // PROGRAMS_H
