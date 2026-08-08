#include "ApiCommon.h"
#include "MotorDrive.h"
#include "MotorProtection.h"
#include "ADE7758.h"
#include "History.h"
#include "Zones.h"   // zonesBoreRoutingValid() - bore-only precondition check

#include <ArduinoJson.h>

// GET  /api/motor      — drive state plus the pre-start condition checklist
// POST /api/motor/cmd  — start / stop / lockout / enable / clearfault
//
// The checklist is the important part for an operator: when a start is
// refused, the UI can say exactly which condition failed rather than just
// rejecting the request.

static void getMotor() {
    StaticJsonDocument<896> doc;
    doc["enabled"]   = motorDriveEnabled();
    doc["state"]     = motorDriveStateName(motorDriveState());
    doc["state_id"]  = (int)motorDriveState();
    doc["running"]   = motorDriveIsRunning();
    doc["selected"]  = motorDriveSelected() == MOTOR_BORE ? "bore" : "well";
    doc["lockout"]   = motorDriveLockedOut();
    doc["run_s"]     = motorDriveRunningMs() / 1000UL;
    doc["last_trip"] = protTripName(motorDriveLastTrip());
    doc["armed"]     = protCurrentTripsArmed();
    doc["bypass_prestart"] = protConfig().bypassPreStart;
    doc["bypass_running"]  = protConfig().bypassRunning;

    ProtTrip supply = protSupplyStatus();
    const AdeReadings& r = adeGetReadings();
    JsonArray checks = doc.createNestedArray("checks");

    auto add = [&checks](const char* name, bool ok, const String& detail) {
        JsonObject c = checks.createNestedObject();
        c["name"]   = name;
        c["ok"]     = ok;
        c["detail"] = detail;
    };

    add("Meter online", adeIsHealthy(),
        adeIsHealthy() ? "ADE7758 responding" : "no SPI response");
    add("CT calibrated", adeCalibration().calibrated,
        adeCalibration().calibrated ? "ratio set" : "enter CT ratio to arm current trips");
    add("All three phases", r.phasePresent[0] && r.phasePresent[1] && r.phasePresent[2],
        String(r.volts[0], 0) + " / " + String(r.volts[1], 0) + " / " + String(r.volts[2], 0) + " V");
    add("Phase sequence", r.phaseSequenceOk,
        r.phaseSequenceOk ? "correct rotation" : "reversed or not verified");
    add("Voltage in range", supply != PROT_UNDER_VOLTAGE && supply != PROT_OVER_VOLTAGE,
        String(protConfig().voltLow, 0) + " - " + String(protConfig().voltHigh, 0) + " V");
    add("Frequency in range", supply != PROT_FREQUENCY, String(r.frequency[0], 1) + " Hz");
    add("Not locked out", !motorDriveLockedOut(),
        motorDriveLockedOut() ? "maintenance lockout engaged" : "clear");

    // Bore-only: which of the two routing valves (repurposed zone relays) is
    // open decides whether the bore has anywhere to send water. Meaningless
    // for the well (it has no diverter), so only shown when bore is the motor
    // currently selected in the UI - see the ?motor= query arg below.
    bool wantBoreCheck = apiServer.arg("motor") == "bore";
    String boreReason;
    bool boreOk = zonesBoreRoutingValid(&boreReason);
    if (wantBoreCheck) {
        add("Bore routing valve", boreOk,
            boreOk ? (zoneBoreWellTankValveId() == 0xFF ? "not configured - see Zones"
                                                          : "route confirmed") : boreReason);
    }

    doc["supply"]    = protTripName(supply);
    doc["can_start"] = (supply == PROT_OK || protConfig().bypassPreStart)
                       && !motorDriveLockedOut() && motorDriveEnabled()
                       && protCurrentTripsArmed() && motorDriveState() == MDRV_IDLE
                       && (!wantBoreCheck || boreOk);

    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

static void postMotorCmd() {
    String cmd = apiServer.arg("cmd");

    if (cmd == "start") {
        MotorId m = (apiServer.arg("motor") == "bore") ? MOTOR_BORE : MOTOR_WELL;
        if (!motorDriveRequestStart(m, REASON_MANUAL_WEB)) {
            String why = motorDriveLastRefusalReason();
            apiSendError(why.length() ? why.c_str() : "start refused - see the precondition list");
            return;
        }
    } else if (cmd == "stop") {
        motorDriveRequestStop(REASON_MANUAL_WEB);
    } else if (cmd == "lockout") {
        motorDriveSetLockout(apiServer.arg("on") == "1");
    } else if (cmd == "enable") {
        motorDriveSetEnabled(apiServer.arg("on") == "1");
    } else if (cmd == "clearfault") {
        motorDriveClearFault();
    } else {
        apiSendError("unknown command");
        return;
    }
    apiSendOk();
}

void registerDriveApi(RouteRegistrar on) {
    on("/api/motor",     HTTP_GET,  getMotor);
    on("/api/motor/cmd", HTTP_POST, postMotorCmd);
}
