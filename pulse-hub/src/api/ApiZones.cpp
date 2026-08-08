#include "ApiCommon.h"
#include "Config.h"
#include "Zones.h"
#include "ValveController.h"
#include "MotorDrive.h"
#include "Display.h"     // getLcdAddress() — used by getI2cExpansion()'s live scan
#include "Logger.h"

#include <ArduinoJson.h>
#include <Wire.h>

// GET  /api/zones       — live zone state, detected boards, interlock limits
// POST /api/zones/cmd   — run / stop / stopall / rename / rescan / create /
//                          delete / remap
//
// Zone ids are permanent once assigned: Programs.cpp references zones by raw
// id, so delete never shifts anything (see Zones.h) and this file must not
// either — getZones() skips tombstoned ids rather than compacting the array.

static void getZones() {
    // Heap-allocated: up to ZONE_MAX(32) zones plus up to VALVE_BOARDS_MAX(4)
    // boards comfortably exceeds what's worth reserving on the stack.
    DynamicJsonDocument doc(4096);
    doc["hardware_present"] = valveHardwarePresent();
    doc["fully_simulated"]  = valveFullySimulated();
    doc["bus_fault"]        = valveBusFault();
    doc["max_open"]         = ZONE_MAX_CONCURRENT;
    doc["open_count"]       = zoneOpenCount();
    doc["max_minutes"]      = ZONE_MAX_MINUTES;
    doc["valve_channels"]   = VALVE_CHANNELS;
    doc["note"]             = valveHardwarePresent()
                              ? "valve board online"
                              : "no valve board - simulated, nothing is energised";
    // Why watering last stopped on its own. The UI surfaces this prominently:
    // a dry run and a blocked valve both leave every valve shut and the field
    // dry, and only this distinguishes them.
    doc["stop_cause"]       = (int)zoneLastStopCause();
    doc["stop_reason"]      = zoneStopCauseName(zoneLastStopCause());
    doc["stop_zone"]        = zoneLastStopZoneName();

    // Bore-motor routing valves: two existing zone relays repurposed as
    // Valve_A (well/tank) and Valve_B (farm) — see Zones.h. -1 = unconfigured.
    doc["bore_welltank_id"] = zoneBoreWellTankValveId() == 0xFF ? -1 : (int)zoneBoreWellTankValveId();
    doc["bore_farm_id"]     = zoneBoreFarmValveId()     == 0xFF ? -1 : (int)zoneBoreFarmValveId();
    String boreReason;
    doc["bore_routing_ok"]     = zonesBoreRoutingValid(&boreReason);
    doc["bore_routing_reason"] = boreReason;

    // Board slots, for the zone-creation/remap channel picker — each
    // contributes channels [b*8 .. b*8+7]. Slot index is stable (declared
    // order), not scan order — "Expansion Board #(b+1)" in the UI is
    // literally b+1. present=false means declared but not answering right
    // now, distinct from not existing at all.
    JsonArray boardsArr = doc.createNestedArray("boards");
    for (uint8_t b = 0; b < valveBoardCount(); b++) {
        JsonObject bo = boardsArr.createNestedObject();
        bo["board"]   = b;
        char addrStr[7];
        snprintf(addrStr, sizeof(addrStr), "0x%02X", valveBoardAddr(b));
        bo["addr"]    = addrStr;
        bo["backend"] = valveBackendName(valveBoardBackend(b));
        bo["present"] = valveBoardPresent(b);
    }

    JsonArray arr = doc.createNestedArray("zones");
    for (uint8_t i = 0; i < zoneCount(); i++) {
        if (!zoneExists(i)) continue;
        const ZoneState& z = zoneGet(i);
        JsonObject o = arr.createNestedObject();
        o["id"]        = i;
        o["name"]      = z.name;
        o["kind"]      = z.kind == ZONE_KIND_DIVERTER ? "diverter" : "irrigation";
        o["channel"]   = z.channel;
        o["active"]    = z.active;
        o["open"]      = z.open;
        o["left_s"]    = zoneSecondsLeft(i);
        o["total_s"]   = z.totalSec;
        o["source"]    = z.source == ZONE_SRC_PROGRAM ? "program"
                       : z.source == ZONE_SRC_MANUAL  ? "manual" : "";
    }
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

static void postZoneCmd() {
    String cmd = apiServer.arg("cmd");

    if (cmd == "stopall") {
        zonesStopAll(ZONE_STOP_OPERATOR);
        apiSendOk();
        return;
    }
    if (cmd == "rescan") {
        // Re-probes every board address and re-derives which zones are
        // active — a real physical write that bypasses zoneStart/zoneStop
        // entirely. It cannot be allowed to run while any zone is open or the
        // motor may be turning: it would de-energise every valve on every
        // board while zones[] and the motor stay completely unaware, leaving
        // the pump commanded to run into a system with nothing open — an
        // unbounded self-inflicted deadhead with no code path left to catch
        // it. (Found in the 2026-08-07 safety review, still applies with
        // multiple boards.)
        if (zoneOpenCount() > 0 || (motorDriveEnabled() && motorDriveCurrentFlowing())) {
            apiSendError("cannot rescan while a zone is open or the pump may be running - "
                         "stop all first");
            return;
        }
        zonesRescanBoard();
        Log(INFO, "[Web] Valve board rescan requested");
        apiSendOk();
        return;
    }
    if (cmd == "create") {
        if (!apiServer.hasArg("name") || !apiServer.hasArg("channel")) {
            apiSendError("name and channel required"); return;
        }
        ZoneKind kind = (apiServer.arg("kind") == "diverter") ? ZONE_KIND_DIVERTER : ZONE_KIND_IRRIGATION;
        int ch = apiServer.arg("channel").toInt();
        if (ch < 0 || ch >= VALVE_CHANNELS) { apiSendError("invalid channel"); return; }
        uint8_t id = zoneCreate(apiServer.arg("name"), kind, (uint8_t)ch);
        if (id == 0xFF) { apiSendError("zone limit reached, name empty, or channel already in use"); return; }
        apiSendJson(200, "{\"ok\":true,\"id\":" + String(id) + "}");
        return;
    }

    if (cmd == "setborevalves") {
        // -1 (or omitted) = unconfigured on that side. Args are named after
        // the physical destination, not "a/b" — see Zones.h.
        int wt = apiServer.hasArg("welltank_id") ? apiServer.arg("welltank_id").toInt() : -1;
        int fm = apiServer.hasArg("farm_id")     ? apiServer.arg("farm_id").toInt()     : -1;
        if (wt >= 0 && !zoneExists((uint8_t)wt)) { apiSendError("no such well/tank zone"); return; }
        if (fm >= 0 && !zoneExists((uint8_t)fm)) { apiSendError("no such farm zone"); return; }
        if (wt >= 0 && fm >= 0 && wt == fm) {
            apiSendError("well/tank and farm valves must be two different zones"); return;
        }
        zonesSetBoreValves(wt < 0 ? 0xFF : (uint8_t)wt, fm < 0 ? 0xFF : (uint8_t)fm);
        apiSendOk();
        return;
    }

    if (cmd == "startrun") {
        // The Dashboard's Start button, both motors: by design there is no
        // default zone/route, so the operator always says where the water
        // goes and for how long before the motor is ever asked to turn.
        // motor=well: steps only, no valve of its own to route.
        // motor=bore, dest=welltank: recharges storage - a complete route on
        //   its own (see zonesBoreRoutingValid()), no steps needed.
        // motor=bore, dest=farm: the farm routing valve is just a gate with
        //   nowhere of its own to send water (found 2026-08-08), so it also
        //   needs steps - one or more zones, each with its own duration, and
        //   marked to start with the batch already running or only once it
        //   has fully closed (see AdHocStep in Zones.h).
        MotorId motor = (apiServer.arg("motor") == "bore") ? MOTOR_BORE : MOTOR_WELL;
        uint8_t farmValveId = 0xFF;   // 0xFF = nothing to route (WELL, or bore->welltank)

        if (motor == MOTOR_BORE) {
            uint8_t wt = zoneBoreWellTankValveId(), fm = zoneBoreFarmValveId();
            if (wt == 0xFF || fm == 0xFF) {
                apiSendError("bore routing valves are not configured - set them up in Zones first");
                return;
            }
            String dest = apiServer.arg("dest");
            if (dest == "welltank") {
                ZoneReject r = zoneStart(wt, ZONE_MAX_MINUTES, ZONE_SRC_MANUAL);
                if (r != ZONE_REJ_NONE) { apiSendError(zoneRejectName(r)); return; }
                zonesSetPreferredSource(MOTOR_BORE);
                if (!motorDriveRequestStart(MOTOR_BORE, REASON_MANUAL_WEB)) {
                    String why = motorDriveLastRefusalReason();
                    apiSendError(why.length() ? why.c_str()
                                 : "valve opened, but the bore motor start was refused - see the precondition list");
                    return;
                }
                apiSendOk();
                return;
            } else if (dest == "farm") {
                farmValveId = fm;
            } else {
                apiSendError("dest must be welltank or farm");
                return;
            }
        }

        // Everything below is the multi-zone ad-hoc queue - WELL always,
        // BORE only for dest=farm (handled and returned above otherwise).
        if (!apiServer.hasArg("steps")) { apiSendError("steps required"); return; }
        // ZONE_MAX(32) steps at roughly 40 bytes of DOM overhead each,
        // comfortably inside 2048.
        DynamicJsonDocument doc(2048);
        if (deserializeJson(doc, apiServer.arg("steps")) != DeserializationError::Ok
            || !doc.is<JsonArray>()) {
            apiSendError("bad steps payload"); return;
        }
        JsonArray arr = doc.as<JsonArray>();
        if (arr.size() == 0)      { apiSendError("pick at least one zone"); return; }
        if (arr.size() > ZONE_MAX) { apiSendError("too many steps"); return; }

        AdHocStep steps[ZONE_MAX];
        uint8_t n = 0;
        uint8_t wt = zoneBoreWellTankValveId(), fm = zoneBoreFarmValveId();
        for (JsonObject o : arr) {
            int zid  = o["id"]  | -1;
            int mins = o["min"] | 0;
            if (zid < 0 || !zoneExists((uint8_t)zid)) { apiSendError("no such zone in steps"); return; }
            if (motor == MOTOR_BORE && ((uint8_t)zid == wt || (uint8_t)zid == fm)) {
                apiSendError("pick a real field zone, not a routing valve"); return;
            }
            if (mins < 1 || mins > ZONE_MAX_MINUTES_ADHOC) {
                apiSendError("duration must be 1 minute to 10 hours"); return;
            }
            steps[n].zoneId       = (uint8_t)zid;
            steps[n].minutes      = (uint16_t)mins;
            steps[n].simultaneous = o["sim"] | false;
            n++;
        }

        if (!adhocRunStart(motor, farmValveId, steps, n)) {
            apiSendError("could not start - a run may already be active, or the routing valve refused to open");
            return;
        }
        if (!motorDriveRequestStart(motor, REASON_MANUAL_WEB)) {
            // Zone(s)/valve are already open and queued - leave them;
            // zonesTask()'s pump coordination retries on its own, same as
            // any other start refused on a transient supply condition.
            String why = motorDriveLastRefusalReason();
            apiSendError(why.length() ? why.c_str()
                         : "zone(s) opened, but the motor start was refused - see the precondition list");
            return;
        }
        apiSendOk();
        return;
    }

    // Everything below addresses a specific existing zone.
    if (!apiServer.hasArg("id")) { apiSendError("id required"); return; }
    int id = apiServer.arg("id").toInt();
    if (id < 0 || !zoneExists((uint8_t)id)) { apiSendError("no such zone"); return; }

    if (cmd == "run") {
        int mins = apiServer.hasArg("minutes") ? apiServer.arg("minutes").toInt() : 0;
        ZoneReject r = zoneStart((uint8_t)id, (uint16_t)mins, ZONE_SRC_MANUAL);
        if (r != ZONE_REJ_NONE) { apiSendError(zoneRejectName(r)); return; }
        apiSendOk();
        return;
    }
    if (cmd == "stop") {
        zoneStop((uint8_t)id);
        apiSendOk();
        return;
    }
    if (cmd == "test") {
        // Hardware-only relay pulse — see Zones.h/zoneTestRelay(). Fixed 3s,
        // not user-configurable: long enough to hear/see the click, short
        // enough that forgetting about it doesn't matter.
        ZoneReject r = zoneTestRelay((uint8_t)id, 3000);
        if (r != ZONE_REJ_NONE) { apiSendError(zoneRejectName(r)); return; }
        apiSendOk();
        return;
    }
    if (cmd == "rename") {
        if (!zoneSetName((uint8_t)id, apiServer.arg("name"))) {
            apiSendError("name required"); return;
        }
        apiSendOk();
        return;
    }
    if (cmd == "delete") {
        if (!zoneDelete((uint8_t)id)) {
            apiSendError("zone is open - stop it first"); return;
        }
        apiSendOk();
        return;
    }
    if (cmd == "remap") {
        if (!apiServer.hasArg("channel")) { apiSendError("channel required"); return; }
        int ch = apiServer.arg("channel").toInt();
        if (ch < 0 || ch >= VALVE_CHANNELS) { apiSendError("invalid channel"); return; }
        if (!zoneSetChannel((uint8_t)id, (uint8_t)ch)) {
            apiSendError("zone is open, or that channel is already in use"); return;
        }
        apiSendOk();
        return;
    }
    apiSendError("unknown command");
}

// GET  /api/i2cexp       — declared expansion-board addresses + a live scan
// POST /api/i2cexp/cmd   — add / remove a declared address
//
// Shown on the Network tab: declaring an address here is what keeps
// Display.cpp's LCD probe from ever mistaking a relay board for the LCD (see
// ValveController.h) — it is a safety statement, not what makes the relay
// board itself work; valveInit()'s own detection finds real boards by
// probing for the chip regardless of what's declared here.
static void getI2cExpansion() {
    StaticJsonDocument<1024> doc;
    // Declared addresses occupy board slots 0..valveDeclaredCount()-1 in
    // declared order (see ValveController.cpp's detectBoards()) — board+1 is
    // exactly the "Expansion Board #N" number shown in the UI, and it never
    // changes across reboots or rescans regardless of what is or isn't
    // plugged in at any given moment.
    JsonArray declared = doc.createNestedArray("declared");
    for (uint8_t i = 0; i < valveDeclaredCount(); i++) {
        JsonObject o = declared.createNestedObject();
        o["board"]   = i;
        o["addr"]    = valveDeclaredAddr(i);
        o["present"] = valveBoardPresent(i);
        o["backend"] = valveBoardPresent(i) ? valveBackendName(valveBoardBackend(i)) : "";
    }

    // A live scan of the two ranges a relay board can actually answer in, so
    // the UI can offer "here's what's on the bus right now" rather than
    // making the user type a hex address from memory. Excludes whatever
    // Display.cpp currently holds as the LCD, and anything already declared.
    JsonArray seen = doc.createNestedArray("detected");
    uint8_t lcd = getLcdAddress();
    const uint8_t ranges[] = {0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
                              0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F};
    for (uint8_t a : ranges) {
        if (a == lcd || valveIsDeclaredExpansion(a)) continue;
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) seen.add(a);
    }
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

static void postI2cExpansionCmd() {
    if (!apiServer.hasArg("addr")) { apiSendError("addr required"); return; }
    String addrStr = apiServer.arg("addr");
    int addr = addrStr.startsWith("0x") || addrStr.startsWith("0X")
             ? strtol(addrStr.c_str(), nullptr, 16)
             : addrStr.toInt();
    if (addr <= 0 || addr > 0xFF) { apiSendError("invalid address"); return; }

    String cmd = apiServer.arg("cmd");
    if (cmd == "add") {
        // Specific reasons, not one catch-all — "already declared" reads as a
        // bug ("I just tried to add it and it says no") when it is actually
        // confirmation the address is already configured correctly.
        bool inRange = (addr >= 0x20 && addr <= 0x27) || (addr >= 0x38 && addr <= 0x3F);
        if (!inRange) { apiSendError("address must be 0x20-0x27 or 0x38-0x3F"); return; }
        if (valveIsDeclaredExpansion((uint8_t)addr)) {
            apiSendError(("0x" + String(addr, HEX) + " is already declared").c_str()); return;
        }
        if (valveDeclaredCount() >= VALVE_DECLARED_MAX) {
            apiSendError(("limit of " + String(VALVE_DECLARED_MAX) + " declared boards reached").c_str()); return;
        }
        if (!valveAddDeclaredExpansion((uint8_t)addr)) {
            apiSendError("could not declare that address"); return;   // should not happen given the checks above
        }
        apiSendOk();
        return;
    }
    if (cmd == "remove") {
        if (!valveRemoveDeclaredExpansion((uint8_t)addr)) { apiSendError("not declared"); return; }
        apiSendOk();
        return;
    }
    apiSendError("unknown command");
}

void registerZoneApi(RouteRegistrar on) {
    on("/api/zones",       HTTP_GET,  getZones);
    on("/api/zones/cmd",   HTTP_POST, postZoneCmd);
    on("/api/i2cexp",      HTTP_GET,  getI2cExpansion);
    on("/api/i2cexp/cmd",  HTTP_POST, postI2cExpansionCmd);
}
