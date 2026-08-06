#include "ApiCommon.h"
#include "Config.h"

#include <ArduinoJson.h>

// GET /api/zones — zone inventory.
//
// The valve hardware (8-channel I2C relay board) is not built yet, so this
// reports the intended layout and an explicit `hardware_present:false` rather
// than pretending zones are controllable. The UI uses that flag to disable the
// controls instead of silently doing nothing when they are pressed.

struct ZoneDef {
    const char* name;
    const char* kind;   // valve | motor | aux
};

// Placeholder inventory until zones become user-configurable and stored on the
// unused SPIFFS partition.
static const ZoneDef zones[] = {
    { "Zone 1",      "valve" },
    { "Zone 2",      "valve" },
    { "Zone 3",      "valve" },
    { "Zone 4",      "valve" },
    { "Zone 5",      "valve" },
    { "Zone 6",      "valve" },
    { "Zone 7",      "valve" },
    { "Well Return", "valve" },
};
static const uint8_t ZONE_COUNT = sizeof(zones) / sizeof(zones[0]);

static void getZones() {
    StaticJsonDocument<1024> doc;
    doc["hardware_present"] = false;
    doc["max_open"]         = 3;   // 5 HP head limit
    doc["min_open_running"] = 1;   // never deadhead a running pump
    doc["note"]             = "8-channel I2C valve board not connected";

    JsonArray arr = doc.createNestedArray("zones");
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        JsonObject z = arr.createNestedObject();
        z["id"]    = i;
        z["name"]  = zones[i].name;
        z["kind"]  = zones[i].kind;
        z["open"]  = false;
        z["run_s"] = 0;
    }
    String out;
    serializeJson(doc, out);
    apiSendJson(200, out);
}

static void postZoneCmd() {
    apiSendError("valve board not connected");
}

void registerZoneApi(RouteRegistrar on) {
    on("/api/zones",     HTTP_GET,  getZones);
    on("/api/zones/cmd", HTTP_POST, postZoneCmd);
}
