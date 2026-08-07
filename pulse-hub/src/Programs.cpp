#include "Programs.h"
#include "Logger.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

#define NVS_PROG_NS "programs"

static ProgramState    programs[PROGRAM_MAX];
static ProgramDefaults defaults;
static uint8_t          count = 0;   // programs[0..count-1] are live

// ---------------------------------------------------------------------------
//  Persistence — one JSON blob per program, one for defaults. Far less NVS
//  key sprawl than a discrete key per field (12+ fields x 3 programs).
// ---------------------------------------------------------------------------

static void loadProgram(uint8_t i) {
    ProgramState& p = programs[i];
    p.enabled = false;
    snprintf(p.name, sizeof(p.name), "Program %d", i + 1);
    for (uint8_t s = 0; s < PROGRAM_MAX_STARTS; s++) p.startMin[s] = 0xFFFF;
    p.dayMode      = WDAY_SPECIFIC;
    p.dayMask      = 0x7F;   // every day
    p.intervalDays = 2;
    for (uint8_t z = 0; z < ZONE_MAX; z++) p.zoneMin[z] = 0;
    p.source       = MOTOR_WELL;
    p.running      = false;
    p.currentZone  = -1;
    p.lastRunEpoch = 0;

    Preferences prefs;
    prefs.begin(NVS_PROG_NS, true);
    String blob = prefs.getString((String("p") + i).c_str(), "");
    prefs.end();
    if (blob.isEmpty()) return;

    // ZONE_MAX grew from 8 to 32 (dynamic zone model) — zoneMin[] alone needs
    // roughly 32*16 bytes of ArduinoJson's DOM overhead, so 512 no longer has
    // margin. 1536 comfortably covers one program's full field set.
    StaticJsonDocument<1536> doc;
    if (deserializeJson(doc, blob) != DeserializationError::Ok) return;

    p.enabled = doc["en"] | false;
    if (doc.containsKey("nm")) {
        strncpy(p.name, doc["nm"].as<const char*>(), sizeof(p.name) - 1);
        p.name[sizeof(p.name) - 1] = '\0';
    }
    JsonArray starts = doc["st"];
    uint8_t si = 0;
    for (JsonVariant v : starts) { if (si >= PROGRAM_MAX_STARTS) break; p.startMin[si++] = v.as<uint16_t>(); }
    p.dayMode      = (WaterDayMode)(doc["dm"] | (int)WDAY_SPECIFIC);
    p.dayMask      = doc["dk"] | 0x7F;
    p.intervalDays = doc["iv"] | 2;
    JsonArray zm = doc["zm"];
    uint8_t zi = 0;
    for (JsonVariant v : zm) { if (zi >= ZONE_MAX) break; p.zoneMin[zi++] = v.as<uint16_t>(); }
    p.source       = (doc["src"] | 0) == 1 ? MOTOR_BORE : MOTOR_WELL;
    p.lastRunEpoch = doc["lr"] | 0;
}

static void saveProgramToNvs(uint8_t i) {
    ProgramState& p = programs[i];
    // ZONE_MAX grew from 8 to 32 (dynamic zone model) — zoneMin[] alone needs
    // roughly 32*16 bytes of ArduinoJson's DOM overhead, so 512 no longer has
    // margin. 1536 comfortably covers one program's full field set.
    StaticJsonDocument<1536> doc;
    doc["en"] = p.enabled;
    doc["nm"] = p.name;
    JsonArray st = doc.createNestedArray("st");
    for (uint8_t s = 0; s < PROGRAM_MAX_STARTS; s++) st.add(p.startMin[s]);
    doc["dm"]  = (int)p.dayMode;
    doc["dk"]  = p.dayMask;
    doc["iv"]  = p.intervalDays;
    JsonArray zm = doc.createNestedArray("zm");
    for (uint8_t z = 0; z < ZONE_MAX; z++) zm.add(p.zoneMin[z]);
    doc["src"] = p.source == MOTOR_BORE ? 1 : 0;
    doc["lr"]  = p.lastRunEpoch;

    String out;
    serializeJson(doc, out);
    Preferences prefs;
    prefs.begin(NVS_PROG_NS, false);
    prefs.putString((String("p") + i).c_str(), out);
    prefs.end();
}

static void loadDefaults() {
    defaults.source            = MOTOR_WELL;
    defaults.seasonalPct       = 100;
    defaults.rainDelayDays     = 0;
    defaults.rainDelaySetEpoch = 0;

    Preferences prefs;
    prefs.begin(NVS_PROG_NS, true);
    defaults.source        = prefs.getUChar("d_src", 0) == 1 ? MOTOR_BORE : MOTOR_WELL;
    defaults.seasonalPct   = prefs.getUChar("d_seas", 100);
    defaults.rainDelayDays = prefs.getUChar("d_rain", 0);
    defaults.rainDelaySetEpoch = prefs.getUInt("d_rainE", 0);
    prefs.end();
}

void programSaveDefaults() {
    Preferences prefs;
    prefs.begin(NVS_PROG_NS, false);
    prefs.putUChar("d_src",  defaults.source == MOTOR_BORE ? 1 : 0);
    prefs.putUChar("d_seas", defaults.seasonalPct);
    prefs.putUChar("d_rain", defaults.rainDelayDays);
    prefs.putUInt ("d_rainE", defaults.rainDelaySetEpoch);
    prefs.end();
}

void programSave(uint8_t idx) {
    if (idx >= count) return;
    saveProgramToNvs(idx);
}

static void saveCount() {
    Preferences prefs;
    prefs.begin(NVS_PROG_NS, false);
    prefs.putUChar("count", count);
    prefs.end();
}

void programsInit() {
    Preferences prefs;
    prefs.begin(NVS_PROG_NS, true);
    // Default 3 preserves what earlier firmware always created; genuinely new
    // units start here too since 3 is a reasonable, editable starting point,
    // not a hard limit (see programCreate() / programDelete()).
    count = prefs.getUChar("count", 3);
    prefs.end();
    if (count > PROGRAM_MAX) count = PROGRAM_MAX;

    for (uint8_t i = 0; i < count; i++) loadProgram(i);
    loadDefaults();
    Log(INFO, "[Programs] " + String(count) + " program(s) loaded");
}

uint8_t programCount() { return count; }

ProgramState&    programGet(uint8_t idx) { static ProgramState dummy = {}; return idx < count ? programs[idx] : dummy; }
ProgramDefaults& programDefaults()       { return defaults; }

uint8_t programCreate() {
    if (count >= PROGRAM_MAX) return 0xFF;
    uint8_t idx = count;
    loadProgram(idx);   // NVS has nothing for this slot yet -> pure defaults
    count++;
    saveCount();
    saveProgramToNvs(idx);
    Log(INFO, "[Programs] Created " + String(programs[idx].name));
    return idx;
}

bool programDelete(uint8_t idx) {
    if (idx >= count) return false;
    if (programs[idx].running) programStop(idx);
    for (uint8_t i = idx; i < count - 1; i++) {
        programs[i] = programs[i + 1];
        saveProgramToNvs(i);
    }
    count--;
    Preferences prefs;
    prefs.begin(NVS_PROG_NS, false);
    prefs.remove((String("p") + count).c_str());   // drop the now-unused last slot
    prefs.end();
    saveCount();
    Log(INFO, "[Programs] Deleted program " + String(idx));
    return true;
}

const char* waterDayModeName(WaterDayMode m) {
    switch (m) {
        case WDAY_SPECIFIC: return "specific days";
        case WDAY_ODD:      return "odd dates";
        case WDAY_EVEN:     return "even dates";
        case WDAY_INTERVAL: return "every N days";
        default:            return "?";
    }
}

// ---------------------------------------------------------------------------
//  Watering-day evaluation
// ---------------------------------------------------------------------------

static bool isWateringDay(const ProgramState& p, const struct tm& ti) {
    switch (p.dayMode) {
        case WDAY_SPECIFIC: return (p.dayMask & (1 << ti.tm_wday)) != 0;
        case WDAY_ODD:      return (ti.tm_mday % 2) == 1;
        case WDAY_EVEN:     return (ti.tm_mday % 2) == 0;
        case WDAY_INTERVAL: {
            if (p.lastRunEpoch == 0) return true;
            uint32_t elapsedDays = (uint32_t)((time(nullptr) - (time_t)p.lastRunEpoch) / 86400UL);
            return elapsedDays >= p.intervalDays;
        }
        default: return false;
    }
}

// Scaled by seasonal adjust; a zone with 0 minutes stays skipped regardless.
static uint16_t scaledMinutes(uint16_t base) {
    if (base == 0) return 0;
    uint32_t scaled = (uint32_t)base * defaults.seasonalPct / 100;
    if (scaled < 1) scaled = 1;
    return (uint16_t)scaled;
}

// ---------------------------------------------------------------------------
//  Sequencing — one zone at a time, in ascending zone-id order
// ---------------------------------------------------------------------------

static int8_t nextZoneAfter(const ProgramState& p, int8_t after) {
    for (int8_t z = after + 1; z < ZONE_MAX; z++) {
        if (p.zoneMin[z] > 0) return z;
    }
    return -1;
}

static void advanceProgram(uint8_t idx) {
    ProgramState& p = programs[idx];
    int8_t next = nextZoneAfter(p, p.currentZone);
    if (next < 0) {
        p.running     = false;
        p.currentZone = -1;
        Log(INFO, "[Programs] " + String(p.name) + " complete");
        return;
    }
    p.currentZone = next;
    zonesSetPreferredSource(p.source);
    ZoneReject r = zoneStart((uint8_t)next, scaledMinutes(p.zoneMin[next]), ZONE_SRC_PROGRAM);
    if (r != ZONE_REJ_NONE) {
        Log(WARN, "[Programs] " + String(p.name) + " zone " + String(next + 1)
                  + " refused: " + zoneRejectName(r) + " - skipping to next");
        advanceProgram(idx);   // try the next zone rather than abandoning the whole run
    }
}

bool programRunNow(uint8_t idx) {
    if (idx >= count) return false;
    ProgramState& p = programs[idx];
    if (nextZoneAfter(p, -1) < 0) return false;   // nothing to water
    p.running     = true;
    p.currentZone = -1;
    p.lastRunEpoch = (uint32_t)time(nullptr);
    saveProgramToNvs(idx);
    advanceProgram(idx);
    return true;
}

void programStop(uint8_t idx) {
    if (idx >= count || !programs[idx].running) return;
    programs[idx].running     = false;
    programs[idx].currentZone = -1;
    zonesStopAll(ZONE_STOP_OPERATOR);
}

// ---------------------------------------------------------------------------
//  Periodic task
// ---------------------------------------------------------------------------

void programsTask() {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 1000UL) return;
    lastCheck = millis();

    // Rain delay decrements once per calendar day, independent of any program.
    struct tm ti;
    if (!getLocalTime(&ti)) return;
    if (defaults.rainDelayDays > 0) {
        time_t nowT = time(nullptr);
        if (defaults.rainDelaySetEpoch == 0) defaults.rainDelaySetEpoch = (uint32_t)nowT;
        uint32_t elapsedDays = (uint32_t)((nowT - (time_t)defaults.rainDelaySetEpoch) / 86400UL);
        if (elapsedDays >= 1) {
            uint8_t dec = (elapsedDays > defaults.rainDelayDays) ? defaults.rainDelayDays : (uint8_t)elapsedDays;
            defaults.rainDelayDays -= dec;
            defaults.rainDelaySetEpoch = (uint32_t)nowT;
            programSaveDefaults();
            Log(INFO, "[Programs] Rain delay now " + String(defaults.rainDelayDays) + " day(s)");
        }
    }

    char nowBuf[6];
    snprintf(nowBuf, sizeof(nowBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    uint16_t nowMin = (uint16_t)(ti.tm_hour * 60 + ti.tm_min);

    for (uint8_t i = 0; i < count; i++) {
        ProgramState& p = programs[i];

        // Advance a run already in progress once its current zone finishes.
        if (p.running) {
            if (p.currentZone < 0 || !zoneGet((uint8_t)p.currentZone).open) advanceProgram(i);
            continue;
        }

        if (!p.enabled || defaults.rainDelayDays > 0) continue;
        if (!isWateringDay(p, ti)) continue;

        for (uint8_t s = 0; s < PROGRAM_MAX_STARTS; s++) {
            if (p.startMin[s] == 0xFFFF || p.startMin[s] != nowMin) continue;
            if (nextZoneAfter(p, -1) < 0) continue;   // every zone is 0 min - nothing to do
            Log(INFO, "[Programs] " + String(p.name) + " starting at " + String(nowBuf));
            p.running      = true;
            p.currentZone  = -1;
            p.lastRunEpoch = (uint32_t)time(nullptr);
            saveProgramToNvs(i);
            advanceProgram(i);
            break;
        }
    }
}
