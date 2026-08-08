#include "ApiCommon.h"
#include "MotorProtection.h"
#include "MotorDrive.h"   // MOTOR_WELL/MOTOR_BORE indices into the per-motor current arrays
#include "ADE7758.h"
#include "Logger.h"

#include <ArduinoJson.h>

// GET  /api/protection   — trip thresholds and meter scaling
// POST /api/protection   — update thresholds
// POST /api/calibration  — update meter scaling; this is what arms current trips

static void getProtection() {
    ProtConfig& c = protConfig();
    AdeCalibration& cal = adeCalibration();
    StaticJsonDocument<768> doc;   // headroom - ArduinoJson silently drops fields past capacity
    doc["v_low"]      = c.voltLow;
    doc["v_high"]     = c.voltHigh;
    doc["i_low"]      = c.ampLow[MOTOR_WELL];
    doc["i_high"]     = c.ampHigh[MOTOR_WELL];
    doc["i_low_bore"]  = c.ampLow[MOTOR_BORE];
    doc["i_high_bore"] = c.ampHigh[MOTOR_BORE];
    doc["f_low"]      = c.freqLow;
    doc["f_high"]     = c.freqHigh;
    doc["imbalance"]  = c.imbalanceMax;
    doc["inrush_s"]   = c.inrushBlankMs / 1000UL;
    doc["dryrun_s"]   = c.dryRunBlankMs / 1000UL;
    doc["cal_v_a"]    = cal.voltScale[0];
    doc["cal_v_b"]    = cal.voltScale[1];
    doc["cal_v_c"]    = cal.voltScale[2];
    doc["cal_i"]      = cal.ampScale[0];
    doc["calibrated"] = cal.calibrated;
    doc["bypass_prestart"] = c.bypassPreStart;
    doc["bypass_running"]  = c.bypassRunning;
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

static void postProtection() {
    ProtConfig& c = protConfig();
    if (apiServer.hasArg("v_low"))     c.voltLow       = apiServer.arg("v_low").toFloat();
    if (apiServer.hasArg("v_high"))    c.voltHigh      = apiServer.arg("v_high").toFloat();
    if (apiServer.hasArg("i_low"))     c.ampLow[MOTOR_WELL]  = apiServer.arg("i_low").toFloat();
    if (apiServer.hasArg("i_high"))    c.ampHigh[MOTOR_WELL] = apiServer.arg("i_high").toFloat();
    if (apiServer.hasArg("i_low_bore"))  c.ampLow[MOTOR_BORE]  = apiServer.arg("i_low_bore").toFloat();
    if (apiServer.hasArg("i_high_bore")) c.ampHigh[MOTOR_BORE] = apiServer.arg("i_high_bore").toFloat();
    if (apiServer.hasArg("f_low"))     c.freqLow       = apiServer.arg("f_low").toFloat();
    if (apiServer.hasArg("f_high"))    c.freqHigh      = apiServer.arg("f_high").toFloat();
    if (apiServer.hasArg("imbalance")) c.imbalanceMax  = apiServer.arg("imbalance").toFloat();
    if (apiServer.hasArg("inrush_s"))  c.inrushBlankMs = apiServer.arg("inrush_s").toInt() * 1000UL;
    if (apiServer.hasArg("dryrun_s"))  c.dryRunBlankMs = apiServer.arg("dryrun_s").toInt() * 1000UL;

    // Temporary maintenance overrides - loud on every actual state change, not
    // just on boot, so flipping either one always leaves a paper trail.
    if (apiServer.hasArg("bypass_prestart")) {
        bool on = apiServer.arg("bypass_prestart") == "1";
        if (on != c.bypassPreStart) {
            Log(WARN, String("[Web] MAINTENANCE OVERRIDE - pre-start voltage/current bypass turned ")
                      + (on ? "ON" : "off"));
        }
        c.bypassPreStart = on;
    }
    if (apiServer.hasArg("bypass_running")) {
        bool on = apiServer.arg("bypass_running") == "1";
        if (on != c.bypassRunning) {
            Log(WARN, String("[Web] MAINTENANCE OVERRIDE - running protection bypass turned ")
                      + (on ? "ON" : "off"));
        }
        c.bypassRunning = on;
    }

    if (c.voltLow >= c.voltHigh) { apiSendError("voltage minimum must be below maximum"); return; }
    if (c.ampLow[MOTOR_WELL] >= c.ampHigh[MOTOR_WELL]) {
        apiSendError("well motor current minimum must be below maximum"); return;
    }
    if (c.ampLow[MOTOR_BORE] >= c.ampHigh[MOTOR_BORE]) {
        apiSendError("bore motor current minimum must be below maximum"); return;
    }

    protSaveConfig();
    Log(INFO, "[Web] Protection thresholds updated");
    apiSendOk();
}

static void postCalibration() {
    AdeCalibration& cal = adeCalibration();
    if (apiServer.hasArg("v_a")) cal.voltScale[0] = apiServer.arg("v_a").toFloat();
    if (apiServer.hasArg("v_b")) cal.voltScale[1] = apiServer.arg("v_b").toFloat();
    if (apiServer.hasArg("v_c")) cal.voltScale[2] = apiServer.arg("v_c").toFloat();
    if (apiServer.hasArg("i")) {
        float s = apiServer.arg("i").toFloat();
        cal.ampScale[0] = cal.ampScale[1] = cal.ampScale[2] = s;
    }
    if (apiServer.hasArg("calibrated")) cal.calibrated = (apiServer.arg("calibrated") == "1");

    adeSaveCalibration();
    Log(WARN, String("[Web] Meter calibration updated - current trips ") +
              (protCurrentTripsArmed() ? "ARMED" : "still disarmed"));
    apiSendOk();
}

void registerProtectionApi(RouteRegistrar on) {
    on("/api/protection",  HTTP_GET,  getProtection);
    on("/api/protection",  HTTP_POST, postProtection);
    on("/api/calibration", HTTP_POST, postCalibration);
}
