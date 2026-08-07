#include "ApiCommon.h"
#include "Config.h"
#include "Zones.h"
#include "ValveController.h"
#include "MotorDrive.h"
#include "Logger.h"

#include <ArduinoJson.h>

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

    // Detected boards, for the zone-creation/remap channel picker — each
    // board contributes channels [b*8 .. b*8+7].
    JsonArray boardsArr = doc.createNestedArray("boards");
    for (uint8_t b = 0; b < valveBoardCount(); b++) {
        JsonObject bo = boardsArr.createNestedObject();
        bo["board"]   = b;
        char addrStr[7];
        snprintf(addrStr, sizeof(addrStr), "0x%02X", valveBoardAddr(b));
        bo["addr"]    = addrStr;
        bo["backend"] = valveBackendName(valveBoardBackend(b));
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

void registerZoneApi(RouteRegistrar on) {
    on("/api/zones",     HTTP_GET,  getZones);
    on("/api/zones/cmd", HTTP_POST, postZoneCmd);
}
