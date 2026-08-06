#include "ApiCommon.h"
#include "MotorProtection.h"
#include "ADE7758.h"
#include "Logger.h"

#include <ArduinoJson.h>

// GET  /api/protection   — trip thresholds and meter scaling
// POST /api/protection   — update thresholds
// POST /api/calibration  — update meter scaling; this is what arms current trips

static void getProtection() {
    ProtConfig& c = protConfig();
    AdeCalibration& cal = adeCalibration();
    StaticJsonDocument<512> doc;
    doc["v_low"]      = c.voltLow;
    doc["v_high"]     = c.voltHigh;
    doc["i_low"]      = c.ampLow;
    doc["i_high"]     = c.ampHigh;
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
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

static void postProtection() {
    ProtConfig& c = protConfig();
    if (apiServer.hasArg("v_low"))     c.voltLow       = apiServer.arg("v_low").toFloat();
    if (apiServer.hasArg("v_high"))    c.voltHigh      = apiServer.arg("v_high").toFloat();
    if (apiServer.hasArg("i_low"))     c.ampLow        = apiServer.arg("i_low").toFloat();
    if (apiServer.hasArg("i_high"))    c.ampHigh       = apiServer.arg("i_high").toFloat();
    if (apiServer.hasArg("f_low"))     c.freqLow       = apiServer.arg("f_low").toFloat();
    if (apiServer.hasArg("f_high"))    c.freqHigh      = apiServer.arg("f_high").toFloat();
    if (apiServer.hasArg("imbalance")) c.imbalanceMax  = apiServer.arg("imbalance").toFloat();
    if (apiServer.hasArg("inrush_s"))  c.inrushBlankMs = apiServer.arg("inrush_s").toInt() * 1000UL;
    if (apiServer.hasArg("dryrun_s"))  c.dryRunBlankMs = apiServer.arg("dryrun_s").toInt() * 1000UL;

    if (c.voltLow >= c.voltHigh) { apiSendError("voltage minimum must be below maximum"); return; }
    if (c.ampLow  >= c.ampHigh)  { apiSendError("current minimum must be below maximum"); return; }

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
