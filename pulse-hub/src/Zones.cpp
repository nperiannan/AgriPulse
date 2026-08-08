#include "Zones.h"
#include "Config.h"
#include "Globals.h"
#include "Logger.h"
#include "MotorDrive.h"
#include "MotorProtection.h"
#include "History.h"
#include <Preferences.h>
#include <ArduinoJson.h>

#define NVS_ZONE_NS      "zones"
#define NVS_KEY_ZONES_V2 "zjson"   // {name,kind,channel,deleted}[] — current format
#define NVS_KEY_NAMES    "names"   // old newline-separated-names format; read once for migration

static ZoneState zones[ZONE_MAX];
static uint8_t   liveCount = 0;      // zones[0..liveCount-1] are populated (some may be !exists)

static void loadBoreValves();   // defined below, near the bore-routing functions; used by zonesInit()

// Seeded the first time a device ever boots, or migrated from the old
// fixed-8-zone format — see loadZones(). Channel N = board N/8, local N%8, so
// channel==index reproduces exactly the original single-board wiring.
static const char* DEFAULT_NAMES[8] = {
    "Zone 1", "Zone 2", "Zone 3", "Zone 4",
    "Zone 5", "Zone 6", "Zone 7", "Well Return",
};
static const ZoneKind DEFAULT_KINDS[8] = {
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

// Hardware-only relay test — see zoneTestRelay(). -1 = no test pulse active.
// Deliberately separate from ZoneState.open/pumpStopPending: this path never
// touches the motor and never sets .open, so it can never be mistaken for a
// real run by zoneOpenCount()/zonesWantPump() and can never be picked up by
// the pump-coordination block in zonesTask().
#define ZONE_TEST_MAX_MS 10000UL
static int8_t   testPulseZone  = -1;
static uint32_t testPulseEndMs = 0;

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
//  Persistence — one JSON array blob: [{n,k,ch,x}, ...], index = zone id.
//  x (deleted) entries are kept in place, never compacted: Programs.cpp
//  references zones by raw id in ProgramState.zoneMin[], so ids must never
//  move once assigned.
// ---------------------------------------------------------------------------

static void saveZones() {
    DynamicJsonDocument doc(ZONE_MAX * 48 + 64);
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < liveCount; i++) {
        JsonObject o = arr.createNestedObject();
        o["n"]  = zones[i].name;
        o["k"]  = (int)zones[i].kind;
        o["ch"] = zones[i].channel;
        if (!zones[i].exists) o["x"] = true;
    }
    String out;
    serializeJson(doc, out);
    Preferences p;
    p.begin(NVS_ZONE_NS, false);
    p.putString(NVS_KEY_ZONES_V2, out);
    p.end();
}

// One-time seed for a genuinely fresh device, or migration from the old
// newline-separated-names-only format (8 fixed zones, channel==index). Either
// way this reproduces the original single-board wiring exactly, so an
// upgraded unit's existing zone names and behaviour are unchanged.
static void seedOrMigrateDefaults() {
    String oldNamesBlob;
    Preferences p;
    p.begin(NVS_ZONE_NS, true);
    oldNamesBlob = p.getString(NVS_KEY_NAMES, "");
    p.end();

    liveCount = 8;
    for (uint8_t i = 0; i < 8; i++) {
        strncpy(zones[i].name, DEFAULT_NAMES[i], ZONE_NAME_MAX - 1);
        zones[i].name[ZONE_NAME_MAX - 1] = '\0';
        zones[i].kind    = DEFAULT_KINDS[i];
        zones[i].exists  = true;
        zones[i].channel = i;
    }

    if (!oldNamesBlob.isEmpty()) {
        int idx = 0, from = 0;
        while (idx < 8 && from <= (int)oldNamesBlob.length()) {
            int nl = oldNamesBlob.indexOf('\n', from);
            String one = (nl < 0) ? oldNamesBlob.substring(from) : oldNamesBlob.substring(from, nl);
            one.trim();
            if (one.length()) {
                strncpy(zones[idx].name, one.c_str(), ZONE_NAME_MAX - 1);
                zones[idx].name[ZONE_NAME_MAX - 1] = '\0';
            }
            idx++;
            if (nl < 0) break;
            from = nl + 1;
        }
        Log(INFO, "[Zones] Migrated names from the old single-board format");
    }

    saveZones();
}

static void loadZones() {
    Preferences p;
    p.begin(NVS_ZONE_NS, true);
    String blob = p.getString(NVS_KEY_ZONES_V2, "");
    p.end();

    if (blob.isEmpty()) {
        seedOrMigrateDefaults();
        return;
    }

    DynamicJsonDocument doc(ZONE_MAX * 48 + 64);
    if (deserializeJson(doc, blob) != DeserializationError::Ok) {
        Log(WARN, "[Zones] Stored zone list unreadable - reseeding defaults");
        seedOrMigrateDefaults();
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    liveCount = 0;
    for (JsonObject o : arr) {
        if (liveCount >= ZONE_MAX) break;
        ZoneState& z = zones[liveCount];
        const char* n = o["n"] | "";
        strncpy(z.name, n, ZONE_NAME_MAX - 1);
        z.name[ZONE_NAME_MAX - 1] = '\0';
        z.kind    = (ZoneKind)(o["k"] | 0);
        z.channel = (uint8_t)(o["ch"] | 0);
        z.exists  = !(o["x"] | false);
        liveCount++;
    }
    Log(INFO, "[Zones] Loaded " + String(liveCount) + " zone slot(s)");
}

bool zoneSetName(uint8_t id, const String& name) {
    if (id >= liveCount || !zones[id].exists) return false;
    String n = name;
    n.trim();
    if (n.isEmpty()) return false;
    strncpy(zones[id].name, n.c_str(), ZONE_NAME_MAX - 1);
    zones[id].name[ZONE_NAME_MAX - 1] = '\0';
    saveZones();
    return true;
}

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------

void zonesRefreshActive() {
    for (uint8_t i = 0; i < liveCount; i++) {
        if (!zones[i].exists) continue;
        zones[i].active = valveChannelAvailable(zones[i].channel);
    }
}

void zonesInit() {
    for (uint8_t i = 0; i < ZONE_MAX; i++) {
        zones[i].open     = false;
        zones[i].source   = ZONE_SRC_NONE;
        zones[i].totalSec = 0;
        zones[i].endsAtMs = 0;
        zones[i].exists   = false;
        zones[i].active   = false;
    }
    loadZones();
    loadBoreValves();
    valveInit();
    zonesRefreshActive();

    uint8_t active = 0;
    for (uint8_t i = 0; i < liveCount; i++) if (zones[i].exists && zones[i].active) active++;
    Log(INFO, "[Zones] " + String(liveCount) + " zone(s), " + String(active) + " active, "
              + (valveFullySimulated() ? String("no valve board (simulated)")
                                       : String(valveBoardCount()) + " valve board(s)"));
}

// ---------------------------------------------------------------------------
//  Queries
// ---------------------------------------------------------------------------

uint8_t zoneCount()             { return liveCount; }
bool    zoneExists(uint8_t id)  { return id < liveCount && zones[id].exists; }

const ZoneState& zoneGet(uint8_t id) {
    static ZoneState dummy = {};
    return zoneExists(id) ? zones[id] : dummy;
}

uint8_t zoneOpenCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < liveCount; i++) if (zones[i].open) n++;
    return n;
}

uint16_t zoneSecondsLeft(uint8_t id) {
    if (!zoneExists(id) || !zones[id].open) return 0;
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
        case ZONE_REJ_INACTIVE:      return "zone's relay channel is not detected - remap or reconnect the board";
        case ZONE_REJ_TEST_BUSY:     return "a test pulse or a real run is already active - wait for it to finish";
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
    for (uint8_t i = 0; i < liveCount; i++) {
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
//  Create / delete / remap
// ---------------------------------------------------------------------------

static bool channelTaken(uint8_t channel, uint8_t excludeId) {
    for (uint8_t i = 0; i < liveCount; i++) {
        if (i == excludeId || !zones[i].exists) continue;
        if (zones[i].channel == channel) return true;
    }
    return false;
}

uint8_t zoneCreate(const String& name, ZoneKind kind, uint8_t channel) {
    if (channel >= VALVE_CHANNELS) return 0xFF;
    if (channelTaken(channel, 0xFF)) return 0xFF;
    String n = name; n.trim();
    if (n.isEmpty()) return 0xFF;
    if (liveCount >= ZONE_MAX) return 0xFF;

    uint8_t id = liveCount++;
    ZoneState& z = zones[id];
    strncpy(z.name, n.c_str(), ZONE_NAME_MAX - 1);
    z.name[ZONE_NAME_MAX - 1] = '\0';
    z.kind     = kind;
    z.channel  = channel;
    z.exists   = true;
    z.active   = valveChannelAvailable(channel);
    z.open     = false;
    z.source   = ZONE_SRC_NONE;
    z.totalSec = 0;
    z.endsAtMs = 0;
    saveZones();
    Log(INFO, "[Zones] Created " + n + " on channel " + String(channel));
    return id;
}

bool zoneDelete(uint8_t id) {
    if (!zoneExists(id)) return false;
    if (zones[id].open) return false;   // refuse — see zoneStart()'s pumpStopPending check for why
    zones[id].exists = false;
    saveZones();
    Log(INFO, "[Zones] Deleted " + String(zones[id].name) + " (id " + String(id)
              + ") - channel " + String(zones[id].channel) + " freed for reassignment");
    return true;
}

bool zoneSetChannel(uint8_t id, uint8_t channel) {
    if (!zoneExists(id)) return false;
    if (zones[id].open) return false;
    if (channel >= VALVE_CHANNELS) return false;
    if (channelTaken(channel, id)) return false;
    zones[id].channel = channel;
    zones[id].active  = valveChannelAvailable(channel);
    saveZones();
    Log(INFO, "[Zones] " + String(zones[id].name) + " remapped to channel " + String(channel));
    return true;
}

void zonesRescanBoard() {
    valveRescan();
    zonesRefreshActive();
}

// ---------------------------------------------------------------------------
//  Hardware-only relay test — see the declaration in Zones.h for the full
//  rationale. Deliberately as small and separate from zoneStart()/zoneStop()
//  as possible: no supply gate, no drive interaction, no history record,
//  nothing that could accidentally grow into "another way to run a zone".
// ---------------------------------------------------------------------------

ZoneReject zoneTestRelay(uint8_t id, uint16_t ms) {
    if (!zoneExists(id))    return ZONE_REJ_BAD_ID;
    if (!zones[id].active)  return ZONE_REJ_INACTIVE;
    if (zones[id].open)     return ZONE_REJ_TEST_BUSY;   // already open via a real run
    if (testPulseZone >= 0) return ZONE_REJ_TEST_BUSY;   // another test pulse already running
    // Never overlap a bench test with a live irrigation cycle, even on a
    // different zone/channel — simplest safe rule, and this is a bench-test
    // feature that should never need to run while anything real is happening.
    if (zoneOpenCount() > 0)                                    return ZONE_REJ_TEST_BUSY;
    if (motorDriveEnabled() && motorDriveCurrentFlowing())      return ZONE_REJ_TEST_BUSY;
    if (ms == 0 || ms > ZONE_TEST_MAX_MS)                       return ZONE_REJ_BAD_DURATION;

    if (!valveSet(zones[id].channel, true)) return ZONE_REJ_VALVE_FAULT;
    testPulseZone  = (int8_t)id;
    testPulseEndMs = millis() + ms;
    Log(INFO, "[Zones] " + String(zones[id].name) + " TEST pulse (" + String(ms)
              + "ms, ch " + String(zones[id].channel) + ") - relay only, motor untouched");
    return ZONE_REJ_NONE;
}

// ---------------------------------------------------------------------------
//  Bore-motor routing valves — see the declaration in Zones.h for the full
//  rationale. Two existing zone ids, repurposed; 0xFF on either side means
//  "not configured yet", and the check is skipped rather than blocking every
//  bore start before the operator has had a chance to set this up.
// ---------------------------------------------------------------------------
#define NVS_KEY_BORE_WELLTANK "bore_wt"
#define NVS_KEY_BORE_FARM     "bore_fm"

static uint8_t boreWellTankValveId = 0xFF;
static uint8_t boreFarmValveId     = 0xFF;

static void loadBoreValves() {
    Preferences p;
    p.begin(NVS_ZONE_NS, true);
    boreWellTankValveId = (uint8_t)p.getUChar(NVS_KEY_BORE_WELLTANK, 0xFF);
    boreFarmValveId     = (uint8_t)p.getUChar(NVS_KEY_BORE_FARM,     0xFF);
    p.end();
}

static void saveBoreValves() {
    Preferences p;
    p.begin(NVS_ZONE_NS, false);
    p.putUChar(NVS_KEY_BORE_WELLTANK, boreWellTankValveId);
    p.putUChar(NVS_KEY_BORE_FARM,     boreFarmValveId);
    p.end();
}

void zonesSetBoreValves(uint8_t wellTankZoneId, uint8_t farmZoneId) {
    boreWellTankValveId = wellTankZoneId;
    boreFarmValveId     = farmZoneId;
    saveBoreValves();
    Log(INFO, "[Zones] Bore routing valves set: well/tank id=" + String(boreWellTankValveId)
              + " farm id=" + String(boreFarmValveId));
}

uint8_t zoneBoreWellTankValveId() { return boreWellTankValveId; }
uint8_t zoneBoreFarmValveId()     { return boreFarmValveId; }

bool zonesBoreRoutingValid(String* reasonOut) {
    if (boreWellTankValveId == 0xFF || boreFarmValveId == 0xFF) return true;   // not configured - skip

    if (!zoneExists(boreWellTankValveId) || !zoneExists(boreFarmValveId)) {
        // Configured zone was since deleted. Don't wedge every bore start on a
        // stale id - log it and let the operator notice and reconfigure.
        Log(WARN, "[Zones] Bore routing valve configuration references a deleted zone - "
                  "reconfigure in Zones. Skipping the routing check.");
        return true;
    }

    bool wellTankOpen = zones[boreWellTankValveId].open;
    bool farmOpen     = zones[boreFarmValveId].open;

    if (wellTankOpen == farmOpen) {   // both open, or neither - ambiguous either way
        if (reasonOut) {
            *reasonOut = wellTankOpen
                ? "both bore routing valves are open - route is ambiguous"
                : "no bore routing valve is open - nowhere for the bore to send water";
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Start / stop
// ---------------------------------------------------------------------------

ZoneReject zoneStart(uint8_t id, uint16_t minutes, ZoneSource src) {
    if (!zoneExists(id))                            return ZONE_REJ_BAD_ID;
    if (!zones[id].active)                          return ZONE_REJ_INACTIVE;
    // A relay test pulse is scheduled to auto-close on its own timer with no
    // interlock chain behind it — letting a real run start on the same (or
    // any) channel while one is pending is exactly the race that would let
    // the test's auto-close steal a valve out from under a real run, or vice
    // versa. Simplest safe rule: never overlap them at all.
    if (testPulseZone >= 0)                         return ZONE_REJ_TEST_BUSY;
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
    if (!valveSet(zones[id].channel, true)) return ZONE_REJ_VALVE_FAULT;

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
    if (!zoneExists(id) || !zones[id].open) return;

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

    if (!valveSet(zones[id].channel, false)) {
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
    // A test pulse never sets .open or pumpStopPending, so it's invisible to
    // the early-return below — handle it unconditionally first. An operator
    // hitting Stop All should stop literally everything, including a bench
    // test in progress.
    if (testPulseZone >= 0) {
        valveSet(zones[testPulseZone].channel, false);
        Log(INFO, "[Zones] Test pulse on " + String(zones[testPulseZone].name) + " cancelled by stop-all");
        testPulseZone = -1;
    }

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
    for (uint8_t i = 0; i < liveCount; i++) {
        if (!zones[i].open) continue;
        if (!valveSet(zones[i].channel, false)) {
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

    // --- Relay test pulse: auto-close on its own timer, independent of
    // everything below (it never touched the motor or .open in the first
    // place, so there is nothing else here that needs to know about it).
    if (testPulseZone >= 0 && (int32_t)(now - testPulseEndMs) >= 0) {
        valveSet(zones[testPulseZone].channel, false);
        Log(INFO, "[Zones] Test pulse on " + String(zones[testPulseZone].name) + " ended");
        testPulseZone = -1;
    }

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
            for (uint8_t i = 0; i < liveCount; i++) {
                if (!zones[i].open) continue;
                if (!valveSet(zones[i].channel, false)) {
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
    for (uint8_t i = 0; i < liveCount; i++) {
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
