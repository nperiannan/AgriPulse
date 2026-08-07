#include "ApiCommon.h"
#include "Config.h"
#include "Zones.h"
#include "ValveController.h"
#include "MotorDrive.h"
#include "Logger.h"

#include <ArduinoJson.h>

// GET  /api/zones       — live zone state, valve backend, interlock limits
// POST /api/zones/cmd   — run / stop / stopall / rename / rescan
//
// Previously this file returned a hardcoded list with hardware_present:false
// and a command handler that always errored. It now drives real valves through
// ValveController, or a simulated backend when no board is fitted — the UI and
// the interlocks behave identically either way, so the irrigation layer is
// usable and testable before the board exists.

static void getZones() {
    StaticJsonDocument<1536> doc;
    doc["hardware_present"] = valveHardwarePresent();
    doc["backend"]          = valveBackendName();
    doc["addr"]             = valveI2CAddress();
    doc["bus_fault"]        = valveBusFault();
    doc["max_open"]         = ZONE_MAX_CONCURRENT;
    doc["open_count"]       = zoneOpenCount();
    doc["max_minutes"]      = ZONE_MAX_MINUTES;
    doc["note"]             = valveHardwarePresent()
                              ? "valve board online"
                              : "no valve board - simulated, nothing is energised";
    // Why watering last stopped on its own. The UI surfaces this prominently:
    // a dry run and a blocked valve both leave every valve shut and the field
    // dry, and only this distinguishes them.
    doc["stop_cause"]       = (int)zoneLastStopCause();
    doc["stop_reason"]      = zoneStopCauseName(zoneLastStopCause());
    doc["stop_zone"]        = zoneLastStopZoneName();

    JsonArray arr = doc.createNestedArray("zones");
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        const ZoneState& z = zoneGet(i);
        JsonObject o = arr.createNestedObject();
        o["id"]        = i;
        o["name"]      = z.name;
        o["kind"]      = z.kind == ZONE_KIND_DIVERTER ? "diverter" : "irrigation";
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
        // valveRescan() re-probes the I2C bus and calls valveAllClosed()
        // directly — a real physical write that bypasses Zones.cpp entirely.
        // It cannot be allowed to run while any zone is open or the motor may
        // be turning: it would de-energise every valve on the board while
        // zones[] and the motor stay completely unaware, leaving the pump
        // commanded to run into a system with nothing open — an unbounded
        // self-inflicted deadhead with no code path left to catch it.
        // (Found in the 2026-08-07 safety review.)
        if (zoneOpenCount() > 0 || (motorDriveEnabled() && motorDriveCurrentFlowing())) {
            apiSendError("cannot rescan while a zone is open or the pump may be running - "
                         "stop all first");
            return;
        }
        valveRescan();
        Log(INFO, "[Web] Valve board rescan requested");
        apiSendOk();
        return;
    }

    // Everything below addresses a specific zone.
    if (!apiServer.hasArg("id")) { apiSendError("id required"); return; }
    int id = apiServer.arg("id").toInt();
    if (id < 0 || id >= ZONE_COUNT) { apiSendError("no such zone"); return; }

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
    apiSendError("unknown command");
}

void registerZoneApi(RouteRegistrar on) {
    on("/api/zones",     HTTP_GET,  getZones);
    on("/api/zones/cmd", HTTP_POST, postZoneCmd);
}
