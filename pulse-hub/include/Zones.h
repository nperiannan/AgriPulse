#ifndef ZONES_H
#define ZONES_H

#include <Arduino.h>
#include "ValveController.h"
#include "History.h"      // HistReason (REASON_MANUAL_WEB etc.) for zoneStop()'s default arg
#include "MotorDrive.h"   // MotorId (MOTOR_WELL/MOTOR_BORE)

// Irrigation zones, and the safety rules that govern them.
//
// This is the layer that decides what is *allowed*; ValveController only knows
// how to flip bits on possibly-several boards.
//
// THE CONTROL CHAIN, in the order the real plant requires:
//
//   1. A zone is requested.
//   2. The supply is checked FIRST — voltage and all three phases. The motor is
//      the water source, so a valve with no motor behind it is pointless, and a
//      motor started on a bad supply is destroyed.
//   3. Only if every condition passes does the valve open AND the motor start.
//      The valve leads, so the pump never starts into a closed system.
//   4. From the moment it is running, current is watched continuously:
//         current DROPS  -> dry run: no water, lost prime
//         current RISES  -> blocked valve or restriction, load climbing
//      Either one stops the motor immediately, to protect both it and the valve.
//
// Other rules enforced here because breaking them damages hardware:
//   * At most ZONE_MAX_CONCURRENT valves open at once — more than the pump can
//     feed drops head across all of them and runs the pump off its curve.
//   * Never run the pump with everything shut (deadheading): it churns the same
//     water, heats it, and wrecks the seals. Valves close only AFTER the pump
//     has stopped.
//   * A protection trip, drive fault or welded contactor closes everything.
//
// Only one motor may run at a time (the electricity board supplies agriculture
// free on that condition), which is what the changeover contactor enforces.
// The WELL motor is the default — it has more pressure. There is no separate
// master valve: the motor itself is the source.
//
// ZONE MODEL — dynamic, not a fixed set of 8:
//   Zones live in a capacity-ZONE_MAX array (zoneCount() is the live number in
//   use). Each zone owns one global ValveController channel (0..VALVE_CHANNELS-1,
//   i.e. board*8+localChannel — see ValveController.h). A deleted zone is
//   tombstoned (exists=false), NEVER shifted and NEVER its id reused: Programs
//   reference zones by raw id in ProgramState.zoneMin[], and shifting ids out
//   from under a saved program would silently rewater the wrong ground. The
//   freed channel becomes available to a newly-created zone immediately, even
//   though the old id itself stays retired.
//   A zone whose channel currently has no board behind it (board unplugged, or
//   never fitted) is `active=false` — the UI marks it inactive and offers
//   remap/delete rather than letting it silently no-op on Run.

#define ZONE_MAX             32    // capacity; zoneCount() is the live number
#define ZONE_MAX_CONCURRENT  3     // 5 HP head limit
#define ZONE_NAME_MAX        17    // 16 chars + NUL, matches the LCD width
#define ZONE_MAX_MINUTES     240
// A bore-fed farm run legitimately runs much longer than a normal irrigation
// cycle (recharging a tank, or a long deep-watering session) — kept as its
// own constant rather than raising ZONE_MAX_MINUTES itself, so every OTHER
// zone (Zones tab, Programs) keeps the tighter 4h default instead of
// accidentally inheriting a 10h cap it was never asked for.
#define ZONE_MAX_MINUTES_BORE_FARM 600

// Not every valve waters a field. The borewell has a diverter downstream of it
// that either feeds the zone valves or sends water into the well to be stored
// for later. It is a legitimate destination for the pump's output, so it
// satisfies the never-deadhead rule on its own.
enum ZoneKind : uint8_t {
    ZONE_KIND_IRRIGATION = 0,
    ZONE_KIND_DIVERTER   = 1,   // borewell -> well, for storage
};

enum ZoneSource : uint8_t {
    ZONE_SRC_NONE = 0,
    ZONE_SRC_MANUAL,     // operator pressed run in the UI
    ZONE_SRC_PROGRAM,    // irrigation program
};

// Why a run request was refused. The UI shows this instead of failing silently —
// a valve that quietly does nothing is worse than one that says why.
enum ZoneReject : uint8_t {
    ZONE_REJ_NONE = 0,
    ZONE_REJ_BAD_ID,
    ZONE_REJ_TOO_MANY_OPEN,     // would exceed ZONE_MAX_CONCURRENT
    ZONE_REJ_VALVE_FAULT,       // I2C write failed
    ZONE_REJ_LOCKED_OUT,        // maintenance lockout engaged
    ZONE_REJ_SUPPLY,            // voltage or frequency outside limits
    ZONE_REJ_PHASE,             // a phase is missing, or rotation is reversed
    ZONE_REJ_METER,             // meter unhealthy/uncalibrated: start unverifiable
    ZONE_REJ_MOTOR_FAULT,       // drive in fault or welded, needs clearing first
    ZONE_REJ_BAD_DURATION,
    ZONE_REJ_INACTIVE,          // zone's mapped channel has no board behind it right now
    ZONE_REJ_TEST_BUSY,         // a test pulse (or a real run) is already active somewhere
};

// Why watering last stopped on its own. Worth distinguishing: a dry run and a
// blocked valve are indistinguishable from the valve's side but call for
// opposite responses from whoever walks out to the field.
enum ZoneStopCause : uint8_t {
    ZONE_STOP_NONE = 0,
    ZONE_STOP_COMPLETED,        // ran its allotted time
    ZONE_STOP_OPERATOR,
    ZONE_STOP_DRY_RUN,          // current fell: no water, lost prime
    ZONE_STOP_BLOCKED,          // current rose: blocked valve or restriction
    ZONE_STOP_SUPPLY,           // supply went out of limits while running
    ZONE_STOP_MOTOR_FAULT,      // drive fault or welded contactor
};

struct ZoneState {
    char       name[ZONE_NAME_MAX];
    ZoneKind   kind;
    bool       exists;        // false = deleted/tombstoned slot — id is never reused
    bool       active;        // false = mapped channel has no board behind it right now
    uint8_t    channel;       // global ValveController channel this zone drives
    bool       open;
    ZoneSource source;
    uint16_t   totalSec;      // requested run length
    uint32_t   endsAtMs;      // millis() deadline while open
};

void zonesInit();

// Drive timers, pump coordination and interlocks. Call every loop().
void zonesTask();

// Open zone `id` for `minutes`. Returns ZONE_REJ_NONE on success. maxMinutes
// defaults to ZONE_MAX_MINUTES; a caller that legitimately needs a longer cap
// (currently only the bore-to-farm start flow — see ApiZones.cpp) passes
// ZONE_MAX_MINUTES_BORE_FARM explicitly instead of this changing for everyone.
ZoneReject zoneStart(uint8_t id, uint16_t minutes, ZoneSource src,
                      uint16_t maxMinutes = ZONE_MAX_MINUTES);

// Close one zone, or all of them. Both defer to the pump: if closing this
// valve would leave the motor running with nothing open, the motor is stopped
// FIRST and the valve stays open until current confirms it, then the wind-down
// in zonesTask() closes it. Never closes into a still-turning pump.
// histReason is the HistReason code recorded against the HIST_ZONE_CLOSE entry
// (default REASON_MANUAL_WEB — the API's "stop" command is the common caller).
void zoneStop(uint8_t id, uint8_t histReason = REASON_MANUAL_WEB);
void zonesStopAll(ZoneStopCause cause);

// Hardware-only relay test: pulses zone `id`'s relay open for `ms`
// milliseconds then closes it again automatically — a direct valveSet() call,
// completely bypassing zoneStart()'s interlock chain (no supply check, no
// ZONE_MAX_CONCURRENT limit, no history record, and critically: the motor is
// NEVER asked to start). This is for answering exactly one question — "does
// clicking this zone's relay click the physical channel I expect" — safely,
// even with the starter panel disconnected or the supply abnormal. Refuses
// while the zone is already open via a real run, another test pulse is
// active, any zone anywhere is open, or the pump may be running — a bench
// test must never cross into a live irrigation cycle.
ZoneReject zoneTestRelay(uint8_t id, uint16_t ms);

// ---------------------------------------------------------------------------
//  Bore-motor routing valves
//
//  Two of the existing zone relays, repurposed — not new hardware — as the
//  bore motor's routing valves: one feeds the well/tank line, the other the
//  farm line. The fixed rule (not user-editable, matches the physical
//  plumbing): exactly one of the two must be open whenever BORE is about to
//  start. Both open, or neither open while something wants BORE, refuses the
//  start rather than guessing which way the water should actually go.
//
//  Enforced once, centrally, inside motorDriveRequestStart() itself (see
//  MotorDrive.cpp) — not in Zones.cpp's callers — because bore can be
//  requested from several independent places (zones pump-coordination, the
//  touch buttons, the web Control tab, MQTT) and a check placed in only one
//  of them would be silently bypassable from the others.
// ---------------------------------------------------------------------------

// 0xFF = unconfigured on either side — the whole check is skipped rather than
// blocking every bore start just because it hasn't been set up yet.
void    zonesSetBoreValves(uint8_t wellTankZoneId, uint8_t farmZoneId);
uint8_t zoneBoreWellTankValveId();
uint8_t zoneBoreFarmValveId();

// True if BORE may start right now. reasonOut, if non-null, is set to a
// human-readable explanation when it returns false — surfaced to the UI so a
// refused start says why instead of just not happening.
bool zonesBoreRoutingValid(String* reasonOut);

// zoneCount()/zoneExists() — iterate 0..zoneCount()-1, skip !zoneExists(i) for
// anything user-facing (a tombstoned id can never be open, so the interlock
// loops in Zones.cpp don't need the exists check — they already gate on .open).
uint8_t zoneCount();
bool    zoneExists(uint8_t id);

const ZoneState& zoneGet(uint8_t id);
uint8_t          zoneOpenCount();
uint16_t         zoneSecondsLeft(uint8_t id);

bool zoneSetName(uint8_t id, const String& name);

// Create/delete/remap — the dynamic zone list. All three refuse while the
// zone (or, for create, the target channel) is open, and refuse a channel
// already claimed by another existing zone — two zones sharing one relay
// would make zoneOpenCount()'s accounting (and so ZONE_MAX_CONCURRENT) wrong
// against what is physically energised.
// Returns the new zone's id, or 0xFF if ZONE_MAX is reached / channel is taken.
uint8_t zoneCreate(const String& name, ZoneKind kind, uint8_t channel);
bool    zoneDelete(uint8_t id);                         // refuses if open
bool    zoneSetChannel(uint8_t id, uint8_t channel);     // refuses if open or channel taken

// Re-probe which zones' channels currently have a board behind them. Call
// after a valve board rescan (zonesRescanBoard() below does this for you).
void zonesRefreshActive();
void zonesRescanBoard();   // valveRescan() + zonesRefreshActive(), in that order

const char* zoneRejectName(ZoneReject r);
const char* zoneStopCauseName(ZoneStopCause c);

// Why watering last stopped by itself, and which zone was open at the time.
// Latched so the UI can explain an unattended stop that happened hours ago.
ZoneStopCause zoneLastStopCause();
const char*   zoneLastStopZoneName();

// True when the pump should be running for irrigation right now — i.e. at
// least one zone is open. Used by the drive coordination in zonesTask().
bool zonesWantPump();

// Which motor the pump-coordination block in zonesTask() requests when a zone
// needs water. Set by Programs.cpp before opening a zone (default MOTOR_WELL —
// more pressure). A manual zone run from the UI always uses the last value set.
void    zonesSetPreferredSource(MotorId m);
MotorId zonesPreferredSource();

#endif // ZONES_H
