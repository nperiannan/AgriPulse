#include "ApiCommon.h"
#include "Programs.h"
#include "Zones.h"
#include "Logger.h"
#include <ArduinoJson.h>

// GET  /api/programs      — all 3 programs + shared defaults
// POST /api/programs/save — save one program (id + full field set)
// POST /api/programs/cmd  — run / stop / defaults

static void getPrograms() {
    StaticJsonDocument<2048> doc;
    ProgramDefaults& d = programDefaults();
    JsonObject def = doc.createNestedObject("defaults");
    def["source"]  = d.source == MOTOR_BORE ? "bore" : "well";
    def["seasonalPct"]   = d.seasonalPct;
    def["rainDelayDays"] = d.rainDelayDays;

    JsonArray arr = doc.createNestedArray("programs");
    for (uint8_t i = 0; i < programCount(); i++) {
        ProgramState& p = programGet(i);
        JsonObject o = arr.createNestedObject();
        o["id"]      = i;
        o["name"]    = p.name;
        o["enabled"] = p.enabled;
        JsonArray st = o.createNestedArray("starts");
        for (uint8_t s = 0; s < PROGRAM_MAX_STARTS; s++) {
            if (p.startMin[s] != 0xFFFF) st.add(p.startMin[s]);
        }
        o["dayMode"]  = (int)p.dayMode;
        o["dayMask"]  = p.dayMask;
        o["interval"] = p.intervalDays;
        o["source"]   = p.source == MOTOR_BORE ? "bore" : "well";
        JsonArray zm = o.createNestedArray("zoneMin");
        for (uint8_t z = 0; z < ZONE_COUNT; z++) zm.add(p.zoneMin[z]);
        o["running"]     = p.running;
        o["currentZone"] = p.currentZone;
    }
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

// Parses "HH:MM,HH:MM,..." into minutes-since-midnight, up to PROGRAM_MAX_STARTS.
static uint8_t parseStarts(const String& csv, uint16_t* out) {
    uint8_t n = 0;
    int from = 0;
    while (from < (int)csv.length() && n < PROGRAM_MAX_STARTS) {
        int comma = csv.indexOf(',', from);
        String tok = (comma < 0) ? csv.substring(from) : csv.substring(from, comma);
        tok.trim();
        int colon = tok.indexOf(':');
        if (colon > 0) {
            int h = tok.substring(0, colon).toInt();
            int m = tok.substring(colon + 1).toInt();
            if (h >= 0 && h < 24 && m >= 0 && m < 60) out[n++] = (uint16_t)(h * 60 + m);
        }
        if (comma < 0) break;
        from = comma + 1;
    }
    return n;
}

static void postProgramSave() {
    if (!apiServer.hasArg("id")) { apiSendError("id required"); return; }
    int id = apiServer.arg("id").toInt();
    if (id < 0 || id >= programCount()) { apiSendError("no such program"); return; }
    ProgramState& p = programGet((uint8_t)id);

    p.enabled = apiServer.hasArg("enabled");
    if (apiServer.hasArg("name")) {
        String n = apiServer.arg("name"); n.trim();
        if (n.length()) { strncpy(p.name, n.c_str(), sizeof(p.name) - 1); p.name[sizeof(p.name) - 1] = '\0'; }
    }
    for (uint8_t s = 0; s < PROGRAM_MAX_STARTS; s++) p.startMin[s] = 0xFFFF;
    if (apiServer.hasArg("starts")) parseStarts(apiServer.arg("starts"), p.startMin);

    if (apiServer.hasArg("dayMode")) p.dayMode = (WaterDayMode)constrain(apiServer.arg("dayMode").toInt(), 0, 3);
    if (apiServer.hasArg("dayMask")) p.dayMask = (uint8_t)constrain(apiServer.arg("dayMask").toInt(), 0, 0x7F);
    if (apiServer.hasArg("interval")) p.intervalDays = (uint8_t)constrain(apiServer.arg("interval").toInt(), 1, 30);
    if (apiServer.hasArg("source")) p.source = (apiServer.arg("source") == "bore") ? MOTOR_BORE : MOTOR_WELL;

    for (uint8_t z = 0; z < ZONE_COUNT; z++) {
        String key = "zm" + String(z);
        if (apiServer.hasArg(key)) {
            int v = apiServer.arg(key).toInt();
            p.zoneMin[z] = (uint16_t)constrain(v, 0, ZONE_MAX_MINUTES);
        }
    }

    programSave((uint8_t)id);
    Log(INFO, "[Web] " + String(p.name) + " saved");
    apiSendOk();
}

static void postProgramCmd() {
    String cmd = apiServer.arg("cmd");

    if (cmd == "defaults") {
        ProgramDefaults& d = programDefaults();
        if (apiServer.hasArg("source")) d.source = (apiServer.arg("source") == "bore") ? MOTOR_BORE : MOTOR_WELL;
        if (apiServer.hasArg("seasonalPct")) d.seasonalPct = (uint8_t)constrain(apiServer.arg("seasonalPct").toInt(), 10, 200);
        if (apiServer.hasArg("rainDelayDays")) {
            d.rainDelayDays = (uint8_t)constrain(apiServer.arg("rainDelayDays").toInt(), 0, 14);
            d.rainDelaySetEpoch = 0;   // restart the daily-decrement clock from now
        }
        programSaveDefaults();
        apiSendOk();
        return;
    }

    if (cmd == "create") {
        uint8_t idx = programCreate();
        if (idx == 0xFF) { apiSendError("program limit reached (12)"); return; }
        apiSendJson(200, "{\"ok\":true,\"id\":" + String(idx) + "}");
        return;
    }

    if (!apiServer.hasArg("id")) { apiSendError("id required"); return; }
    int id = apiServer.arg("id").toInt();
    if (id < 0 || id >= programCount()) { apiSendError("no such program"); return; }

    if (cmd == "run") {
        if (!programRunNow((uint8_t)id)) { apiSendError("nothing to water - every zone is 0 min"); return; }
        apiSendOk();
        return;
    }
    if (cmd == "stop") {
        programStop((uint8_t)id);
        apiSendOk();
        return;
    }
    if (cmd == "toggle") {
        ProgramState& p = programGet((uint8_t)id);
        p.enabled = !p.enabled;
        programSave((uint8_t)id);
        apiSendOk();
        return;
    }
    if (cmd == "delete") {
        if (!programDelete((uint8_t)id)) { apiSendError("delete failed"); return; }
        apiSendOk();
        return;
    }
    apiSendError("unknown command");
}

void registerProgramApi(RouteRegistrar on) {
    on("/api/programs",      HTTP_GET,  getPrograms);
    on("/api/programs/save", HTTP_POST, postProgramSave);
    on("/api/programs/cmd",  HTTP_POST, postProgramCmd);
}
