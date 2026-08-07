#include "Zones.h"
#include "Config.h"
#include "Globals.h"
#include "Logger.h"
#include "MotorDrive.h"
#include "MotorProtection.h"
#include "History.h"
#include <Preferences.h>

#define NVS_ZONE_NS   "zones"
#define NVS_KEY_NAMES "names"

static ZoneState zones[ZONE_COUNT];

// Default names. The last channel is the borewell diverter, not a field zone:
// it sends borewell water into the well to be stored for later instead of out
// to the crop. It is named rather than left as "Zone 8" because treating it as
// an ordinary zone is exactly the mistake that would water nothing.
static const char* DEFAULT_NAMES[ZONE_COUNT] = {
    "Zone 1", "Zone 2", "Zone 3", "Zone 4",
    "Zone 5", "Zone 6", "Zone 7", "Well Return",
};

static const ZoneKind DEFAULT_KINDS[ZONE_COUNT] = {
    ZONE_KIND_IRRIGATION, ZONE_KIND_IRRIGATION, ZONE_KIND_IRRIGATION, ZONE_KIND_IRRIGATION,
    ZONE_KIND_IRRIGATION, ZONE_KIND_IRRIGATION, ZONE_KIND_IRRIGATION, ZONE_KIND_DIVERTER,
};

// Latched explanation of the last unattended stop, for the UI.
static ZoneStopCause lastStopCause = ZONE_STOP_NONE;
static char          lastStopZone[ZONE_NAME_MAX] = "";

// Pump coordination: after the last valve shuts we ask the drive to stop, but
// the valve must stay open until current has actually gone. This tracks that
// wind-down so the pump is never left pushing into a closed system.
//
// SAFETY-CRITICAL: "stopped" for the purpose of closing a valve is decided
// ONLY by motorDriveCurrentFlowing() — actual measured current — never by
// motorDriveIsRunning() or a drive state name. state leaves MDRV_RUNNING the
// instant a stop is requested (MDRV_STOP_PULSE), well before the relay drops
// or current decays, so using state as the signal closes the valve on a pump
// that is still physically turning on every single ordinary shutdown. This
// was found and fixed 2026-08-07 after an adversarial safety review.
static bool     pumpStopPending    = false;
static uint32_t pumpStopStartedMs  = 0;
static uint8_t  pumpStopHistReason = REASON_MANUAL_WEB;  // for the HIST_ZONE_CLOSE records
                                                          // written once the wind-down completes

// How long to keep a valve open after commanding the pump to stop, waiting for
// current to fall. Backstop for when the meter is uncalibrated and a reading
// can't be fully trusted. NEVER applied while MDRV_WELDED: in that state
// current is CONFIRMED still flowing by definition (that is what "welded"
// means), so a timeout there would force-close valves on a motor provably
// still running — the exact deadhead this whole mechanism exists to prevent.
// Welded waits indefinitely for current to actually clear.
#define ZONE_PUMP_WINDDOWN_MS 8000UL

// Map a zone-level stop cause onto the closest-fitting persisted HistReason.
// The 4-bit reason field in HistoryRecord.flags (see History.cpp) is already
// fully populated 0-15, so this reuses existing codes rather than adding new
// ones that would not fit.
static uint8_t histReasonFor(ZoneStopCause c) {
    switch (c) {
        case ZONE_STOP_COMPLETED:   return REASON_MAX_RUNTIME;   // ran to its own time limit
        case ZONE_STOP_OPERATOR:    return REASON_MANUAL_WEB;
        default:                    return REASON_PROTECTION;    // dry run / blocked / supply / motor fault
    }
}

// ---------------------------------------------------------------------------
//  Name persistence
// ---------------------------------------------------------------------------

static void loadNames() {
    Preferences p;
    p.begin(NVS_ZONE_NS, true);
    String blob = p.getString(NVS_KEY_NAMES, "");
    p.end();

    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        strncpy(zones[i].name, DEFAULT_NAMES[i], ZONE_NAME_MAX - 1);
        zones[i].name[ZONE_NAME_MAX - 1] = '\0';
    }
    if (blob.isEmpty()) return;
    // Only names are user-editable; kind is fixed by how the plumbing is built.

    // Stored as newline-separated names, in zone order. A blank field keeps
    // the default, so a partially-filled record still loads cleanly.
    int idx = 0, from = 0;
    while (idx < ZONE_COUNT && from <= (int)blob.length()) {
        int nl = blob.indexOf('\n', from);
        String one = (nl < 0) ? blob.substring(from) : blob.substring(from, nl);
        one.trim();
        if (one.length()) {
            strncpy(zones[idx].name, one.c_str(), ZONE_NAME_MAX - 1);
            zones[idx].name[ZONE_NAME_MAX - 1] = '\0';
        }
        idx++;
        if (nl < 0) break;
        from = nl + 1;
    }
}

void zonesSaveNames() {
    String blob;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        blob += zones[i].name;
        if (i < ZONE_COUNT - 1) blob += '\n';
    }
    Preferences p;
    p.begin(NVS_ZONE_NS, false);
    p.putString(NVS_KEY_NAMES, blob);
    p.end();
}

bool zoneSetName(uint8_t id, const String& name) {
    if (id >= ZONE_COUNT) return false;
    String n = name;
    n.trim();
    if (n.isEmpty()) return false;
    strncpy(zones[id].name, n.c_str(), ZONE_NAME_MAX - 1);
    zones[id].name[ZONE_NAME_MAX - 1] = '\0';
    zonesSaveNames();
    return true;
}

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------

void zonesInit() {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        zones[i].kind     = DEFAULT_KINDS[i];
        zones[i].open     = false;
        zones[i].source   = ZONE_SRC_NONE;
        zones[i].totalSec = 0;
        zones[i].endsAtMs = 0;
    }
    loadNames();
    valveInit();
    Log(INFO, String("[Zones] ") + ZONE_COUNT + " zones ready, valve backend: " + valveBackendName());
}

// ---------------------------------------------------------------------------
//  Queries
// ---------------------------------------------------------------------------

const ZoneState& zoneGet(uint8_t id) {
    static ZoneState dummy = {};
    return (id < ZONE_COUNT) ? zones[id] : dummy;
}

uint8_t zoneOpenCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) if (zones[i].open) n++;
    return n;
}

uint16_t zoneSecondsLeft(uint8_t id) {
    if (id >= ZONE_COUNT || !zones[id].open) return 0;
    uint32_t now = millis();
    if ((int32_t)(zones[id].endsAtMs - now) <= 0) return 0;
    return (uint16_t)((zones[id].endsAtMs - now) / 1000UL);
}

bool zonesWantPump() { return zoneOpenCount() > 0; }

static MotorId preferredSource = MOTOR_WELL;
void    zonesSetPreferredSource(MotorId m) { preferredSource = m; }
MotorId zonesPreferredSource()             { return preferredSource; }

const char* zoneRejectName(ZoneReject r) {
    switch (r) {
        case ZONE_REJ_NONE:          return "ok";
        case ZONE_REJ_BAD_ID:        return "no such zone";
        case ZONE_REJ_TOO_MANY_OPEN: return "too many valves already open";
        case ZONE_REJ_VALVE_FAULT:   return "valve board did not respond";
        case ZONE_REJ_LOCKED_OUT:    return "maintenance lockout engaged";
        case ZONE_REJ_SUPPLY:        return "supply voltage or frequency out of range";
        case ZONE_REJ_PHASE:         return "a phase is missing or rotation is reversed";
        case ZONE_REJ_METER:         return "meter not calibrated - a start could not be verified";
        case ZONE_REJ_MOTOR_FAULT:   return "motor drive is in fault - clear it first";
        case ZONE_REJ_BAD_DURATION:  return "duration out of range";
        default:                     return "refused";
    }
}

const char* zoneStopCauseName(ZoneStopCause c) {
    switch (c) {
        case ZONE_STOP_COMPLETED:   return "run completed";
        case ZONE_STOP_OPERATOR:    return "stopped by operator";
        case ZONE_STOP_DRY_RUN:     return "DRY RUN - current fell away, no water reaching the pump";
        case ZONE_STOP_BLOCKED:     return "BLOCKED - current climbed, valve or line is restricted";
        case ZONE_STOP_SUPPLY:      return "supply fault while running";
        case ZONE_STOP_MOTOR_FAULT: return "motor drive fault";
        default:                    return "";
    }
}

ZoneStopCause zoneLastStopCause()    { return lastStopCause; }
const char*   zoneLastStopZoneName() { return lastStopZone; }

static void latchStop(ZoneStopCause c) {
    lastStopCause = c;
    lastStopZone[0] = '\0';
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        if (zones[i].open) {
            strncpy(lastStopZone, zones[i].name, ZONE_NAME_MAX - 1);
            lastStopZone[ZONE_NAME_MAX - 1] = '\0';
            break;
        }
    }
}

// Map a protection verdict onto the reason a zone could not be opened, so the
// operator is told which condition failed rather than a generic "refused".
static ZoneReject supplyGate() {
    if (!protCurrentTripsArmed()) return ZONE_REJ_METER;

    ProtTrip t = protCheckStartAllowed();
    switch (t) {
        case PROT_OK:              return ZONE_REJ_NONE;
        case PROT_PHASE_MISSING:
        case PROT_PHASE_SEQUENCE:  return ZONE_REJ_PHASE;
        case PROT_METER_UNHEALTHY: return ZONE_REJ_METER;
        default:                   return ZONE_REJ_SUPPLY;   // voltage / frequency
    }
}

// ---------------------------------------------------------------------------
//  Start / stop
// ---------------------------------------------------------------------------

ZoneReject zoneStart(uint8_t id, uint16_t minutes, ZoneSource src) {
    if (id >= ZONE_COUNT)                          return ZONE_REJ_BAD_ID;
    if (minutes == 0 || minutes > ZONE_MAX_MINUTES) return ZONE_REJ_BAD_DURATION;

    // Re-running an already-open zone just extends it; that is what an operator
    // pressing "run 10 min" again plainly means.
    if (!zones[id].open && zoneOpenCount() >= ZONE_MAX_CONCURRENT) {
        return ZONE_REJ_TOO_MANY_OPEN;
    }

    // The drive must own the relays for a zone to mean anything: with it
    // disabled, opening a valve here has nobody coordinating a motor start,
    // so a "successful" run would water nothing while reporting success.
    // (Found in the 2026-08-07 safety review — previously a zone would open,
    // run its full timer, and log ZONE_STOP_COMPLETED with no motor ever
    // asked to start.) The legacy relay path, if in control instead, has no
    // knowledge of zones at all and must not be second-guessed here.
    if (!motorDriveEnabled()) return ZONE_REJ_MOTOR_FAULT;

    // --- Supply is checked BEFORE the valve moves ---------------------------
    // Opening a valve is a request to run the pump, so the pump's preconditions
    // are this zone's preconditions. Checking after the valve opened would mean
    // sitting with an open valve and no water, which is how a line drains and
    // loses prime.
    ZoneReject gate = supplyGate();
    if (gate != ZONE_REJ_NONE) return gate;

    if (motorDriveLockedOut()) return ZONE_REJ_LOCKED_OUT;
    MotorDriveState st = motorDriveState();
    if (st == MDRV_FAULT || st == MDRV_WELDED) return ZONE_REJ_MOTOR_FAULT;

    // A stop is already committed to hardware (STOP relay pulsed) but the
    // motor hasn't confirmed off yet. Opening now would otherwise leave that
    // stop to run to completion and then have to restart from scratch a few
    // seconds later — an unwanted cycle on the starter's contacts. Make the
    // caller wait the short remainder rather than silently causing it.
    // (Found in the 2026-08-07 safety review.)
    if (pumpStopPending) return ZONE_REJ_MOTOR_FAULT;

    // --- Only now does the valve open --------------------------------------
    if (!valveSet(id, true)) return ZONE_REJ_VALVE_FAULT;

    zones[id].open     = true;
    zones[id].source   = src;
    zones[id].totalSec = (uint16_t)(minutes * 60);
    zones[id].endsAtMs = millis() + (uint32_t)minutes * 60000UL;

    addZoneHistoryRecord(HIST_ZONE_OPEN, id,
        src == ZONE_SRC_PROGRAM ? REASON_SCHEDULED : REASON_MANUAL_WEB);

    Log(INFO, "[Zones] " + String(zones[id].name) + " open for " + String(minutes)
              + " min (" + (src == ZONE_SRC_PROGRAM ? "program" : "manual") + ")");
    return ZONE_REJ_NONE;
}

void zoneStop(uint8_t id, uint8_t histReason) {
    if (id >= ZONE_COUNT || !zones[id].open) return;

    // If this is the last open zone and the pump might still be turning,
    // command it down first and let zonesTask()'s wind-down close the valve
    // once current actually confirms it — never here. Ground truth is
    // measured current, not drive state (see the block comment above
    // pumpStopPending): state leaves MDRV_RUNNING the instant a stop is
    // requested, well before the motor has actually stopped.
    bool last = (zoneOpenCount() == 1);
    bool pumpMayBeRunning = motorDriveEnabled() && motorDriveCurrentFlowing();
    if (last && pumpMayBeRunning) {
        if (!pumpStopPending) {
            if (motorDriveState() != MDRV_WELDED) motorDriveRequestStop(REASON_MANUAL_WEB);
            pumpStopPending    = true;
            pumpStopStartedMs  = millis();
            pumpStopHistReason = histReason;
            Log(INFO, "[Zones] " + String(zones[id].name)
                      + " finishing - stopping pump before closing the valve");
        }
        zones[id].endsAtMs = millis();   // expired; the wind-down owns it now
        return;
    }

    if (!valveSet(id, false)) {
        // I2C write failed: the valve may still be physically open. Do NOT
        // clear zones[id].open — that would under-count physically-open
        // valves, letting a later zoneStart() exceed ZONE_MAX_CONCURRENT, and
        // would show "closed" in the UI for a valve that is still energised.
        // (Found in the 2026-08-07 safety review.) Leave it flagged; the next
        // poll's bus_fault indicator tells the operator, and a retry (or
        // Rescan once the board answers again) is the way out.
        Log(ERROR, "[Zones] " + String(zones[id].name) + " close FAILED (I2C) - "
                   "leaving it flagged open; it may still be physically energised");
        return;
    }
    addZoneHistoryRecord(HIST_ZONE_CLOSE, id, histReason);
    zones[id].open     = false;
    zones[id].source   = ZONE_SRC_NONE;
    zones[id].totalSec = 0;
    zones[id].endsAtMs = 0;
    Log(INFO, "[Zones] " + String(zones[id].name) + " closed");
}

void zonesStopAll(ZoneStopCause cause) {
    if (zoneOpenCount() == 0 && !pumpStopPending) return;
    latchStop(cause);

    // Ground truth, not derived state (see the block comment above
    // pumpStopPending) — this is exactly the check whose absence let
    // zonesStopAll() close every valve on a WELDED contactor with current
    // still confirmed flowing. (2026-08-07 safety review.)
    bool pumpMayBeRunning = motorDriveEnabled() && motorDriveCurrentFlowing();

    if (pumpMayBeRunning) {
        MotorDriveState st = motorDriveState();
        // WELDED: a stop pulse already failed once; sending another achieves
        // nothing (MotorDrive.cpp holds WELDED until current clears on its
        // own). Just wait — closing now guarantees a deadhead on a motor
        // that is provably still turning.
        if (st != MDRV_WELDED) motorDriveRequestStop(REASON_MANUAL_WEB);

        pumpStopPending    = true;
        pumpStopStartedMs  = millis();
        pumpStopHistReason = histReasonFor(cause);
        Log(WARN, String("[Zones] Stopping all: ") + zoneStopCauseName(cause)
                  + " - valves close once current confirms the pump has stopped");
        return;   // zonesTask()'s wind-down finishes the job
    }

    // Current already confirmed not flowing (or the drive doesn't own the
    // relays) - nothing to wait for.
    uint8_t reason = histReasonFor(cause);
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        if (!zones[i].open) continue;
        if (!valveSet(i, false)) {
            Log(ERROR, "[Zones] " + String(zones[i].name) + " close FAILED (I2C) during stop-all");
            continue;   // leave zones[i].open true - see zoneStop() for why
        }
        addZoneHistoryRecord(HIST_ZONE_CLOSE, i, reason);
        zones[i].open     = false;
        zones[i].source   = ZONE_SRC_NONE;
        zones[i].totalSec = 0;
        zones[i].endsAtMs = 0;
    }
    pumpStopPending = false;
    Log(WARN, String("[Zones] All zones stopped: ") + zoneStopCauseName(cause));
}

// ---------------------------------------------------------------------------
//  Periodic task
// ---------------------------------------------------------------------------

void zonesTask() {
    uint32_t now = millis();

    // --- Safety first: a drive fault closes everything, immediately ---
    //
    // The drive already stops the motor on its own protection trip; what this
    // adds is the zone-level reading of *why*. Both failures the operator cares
    // about show up in current and are otherwise indistinguishable from the
    // valve's side:
    //     current fell away -> dry run, the pump has lost its water
    //     current climbed   -> the valve or line downstream is blocked
    // Naming which one it was is the difference between "go prime the pump" and
    // "go find the blockage".
    if (motorDriveEnabled()) {
        MotorDriveState st = motorDriveState();
        if (st == MDRV_FAULT || st == MDRV_WELDED) {
            if (zoneOpenCount() > 0) {
                ProtTrip trip = motorDriveLastTrip();
                ZoneStopCause cause =
                    (st == MDRV_WELDED)              ? ZONE_STOP_MOTOR_FAULT
                  : (trip == PROT_UNDER_CURRENT)     ? ZONE_STOP_DRY_RUN
                  : (trip == PROT_OVER_CURRENT)      ? ZONE_STOP_BLOCKED
                  : (trip == PROT_UNDER_VOLTAGE ||
                     trip == PROT_OVER_VOLTAGE   ||
                     trip == PROT_PHASE_MISSING  ||
                     trip == PROT_PHASE_SEQUENCE ||
                     trip == PROT_IMBALANCE      ||
                     trip == PROT_FREQUENCY)         ? ZONE_STOP_SUPPLY
                                                     : ZONE_STOP_MOTOR_FAULT;
                // zonesStopAll() itself now checks measured current before
                // closing anything — on MDRV_WELDED that check is what stops
                // this from deadheading a motor that is confirmed still
                // running. See its comment.
                zonesStopAll(cause);
            }
            return;
        }
    }

    // --- Wind-down: pump was told to stop; close valves once current is gone ---
    //
    // SAFETY-CRITICAL. "stopped" is decided ONLY by measured current
    // (motorDriveCurrentFlowing()), never by state or motorDriveIsRunning() —
    // state leaves MDRV_RUNNING the instant a stop is requested, long before
    // the relay drops or current actually decays. Using state here was the
    // bug that closed the valve ~1-6 s before the pump physically stopped on
    // every single ordinary shutdown. And the timeout backstop must NEVER
    // fire during MDRV_WELDED: that state means current is CONFIRMED still
    // flowing, so "give up waiting" there is not a backstop, it is a
    // guaranteed deadhead on a motor with no software path left to stop it.
    // (Both found in the 2026-08-07 safety review.)
    if (pumpStopPending) {
        bool weldedNow = motorDriveEnabled() && motorDriveState() == MDRV_WELDED;
        bool stopped   = !motorDriveCurrentFlowing();
        bool timeout   = !weldedNow && (now - pumpStopStartedMs) >= ZONE_PUMP_WINDDOWN_MS;

        if (stopped || timeout) {
            if (timeout && !stopped) {
                Log(WARN, "[Zones] Pump current did not confirm stopped within wind-down; "
                          "closing valves anyway");
            }
            for (uint8_t i = 0; i < ZONE_COUNT; i++) {
                if (!zones[i].open) continue;
                if (!valveSet(i, false)) {
                    Log(ERROR, "[Zones] " + String(zones[i].name)
                               + " close FAILED (I2C) at end of wind-down - leaving it flagged open");
                    continue;   // see zoneStop() for why zones[i].open stays true
                }
                addZoneHistoryRecord(HIST_ZONE_CLOSE, i, pumpStopHistReason);
                zones[i].open     = false;
                zones[i].source   = ZONE_SRC_NONE;
                zones[i].totalSec = 0;
                zones[i].endsAtMs = 0;
            }
            pumpStopPending = false;
        }
        // else: keep waiting, including indefinitely through MDRV_WELDED —
        // there is no safe timeout for a motor confirmed still turning.
        return;   // nothing else runs while winding down
    }

    // --- Expire finished zones ---
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        if (!zones[i].open) continue;
        if ((int32_t)(zones[i].endsAtMs - now) <= 0) {
            Log(INFO, "[Zones] " + String(zones[i].name) + " run complete");
            latchStop(ZONE_STOP_COMPLETED);
            zoneStop(i, histReasonFor(ZONE_STOP_COMPLETED));
            if (pumpStopPending) return;   // last zone: wind-down took over
        }
    }

    // --- Pump coordination ---
    // Only when the drive owns the relays. The valve is already open by the
    // time we get here, so starting the pump can never deadhead it.
    if (!motorDriveEnabled()) return;

    bool want    = zonesWantPump();
    bool running = motorDriveIsRunning();

    if (want && !running && motorDriveState() == MDRV_IDLE) {
        // WELL is the default source (more pressure); Programs.cpp can set a
        // different one via zonesSetPreferredSource() before opening a zone.
        // Only one motor may run at a time (the free agricultural supply is
        // granted on that condition), which the changeover contactor enforces.
        if (!motorDriveRequestStart(preferredSource, REASON_SCHEDULED)) {
            // Refused (supply, min-off window, lockout). Leave the valve open
            // and retry next tick — the drive logs why, and a transient supply
            // dip should not abandon the run.
            static uint32_t lastGripe = 0;
            if (now - lastGripe > 30000UL) {
                lastGripe = now;
                Log(WARN, "[Zones] Pump start refused while zones are open - will retry");
            }
        }
    } else if (!want && running) {
        // Should not normally happen (zoneStop handles it), but if the pump is
        // turning with every valve shut, that is a deadhead: stop it now.
        Log(WARN, "[Zones] Pump running with no zone open - stopping to avoid deadhead");
        motorDriveRequestStop(REASON_MANUAL_WEB);
    }
}
