#include "ApiCommon.h"
#include "ADE7758.h"
#include "MotorProtection.h"   // protConfig() - voltage thresholds for the UI's out-of-range indicator

#include <ArduinoJson.h>

// GET /api/power — live 3-phase supply readings.

static void getPower() {
    const AdeReadings& r = adeGetReadings();
    StaticJsonDocument<640> doc;
    doc["healthy"]    = adeIsHealthy();
    doc["valid"]      = r.valid;
    doc["calibrated"] = adeCalibration().calibrated;
    doc["seq_ok"]     = r.phaseSequenceOk;
    doc["imbalance"]  = adeCurrentImbalance();
    doc["max_amps"]   = adeMaxAmps();
    doc["age_s"]      = r.lastUpdateMs ? (millis() - r.lastUpdateMs) / 1000UL : 0;
    // Same thresholds Protection enforces - the UI marks a phase out-of-range
    // using these, not a hardcoded copy that could drift from the real ones.
    doc["v_low"]      = protConfig().voltLow;
    doc["v_high"]     = protConfig().voltHigh;

    JsonArray ph = doc.createNestedArray("phases");
    // R/Y/B (Red/Yellow/Blue) is the standard Indian 3-phase colour convention
    // this install uses, not the meter's internal A/B/C register naming —
    // ADE_PHASE_A/B/C (ADE7758.h) still refer to the same physical channels.
    const char* names[] = { "R", "Y", "B" };
    for (uint8_t i = 0; i < ADE_PHASE_COUNT; i++) {
        JsonObject p = ph.createNestedObject();
        p["name"]    = names[i];
        p["volts"]   = r.volts[i];
        p["amps"]    = r.amps[i];
        p["hz"]      = r.frequency[i];
        p["present"] = r.phasePresent[i];
    }
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

void registerPowerApi(RouteRegistrar on) {
    on("/api/power", HTTP_GET, getPower);
}
