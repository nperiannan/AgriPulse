#include "ApiCommon.h"
#include "Buzzer.h"
#include "Logger.h"

#include <ArduinoJson.h>

// GET  /api/settings   — general device-behaviour settings (not diagnostics,
//                         not credentials — see System and Accounts for those)
// POST /api/settings/cmd — update one

static void getSettings() {
    StaticJsonDocument<128> doc;
    doc["buzzer_countdown"] = buzzerCountdownEnabled();
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

static void postSettingsCmd() {
    if (apiServer.hasArg("buzzer_countdown")) {
        bool on = apiServer.arg("buzzer_countdown") == "1";
        buzzerSetCountdownEnabled(on);
        apiSendOk();
        return;
    }
    apiSendError("unknown command");
}

void registerSettingsApi(RouteRegistrar on) {
    on("/api/settings",     HTTP_GET,  getSettings);
    on("/api/settings/cmd", HTTP_POST, postSettingsCmd);
}
