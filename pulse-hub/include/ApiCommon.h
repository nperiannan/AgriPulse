#ifndef API_COMMON_H
#define API_COMMON_H

#include <Arduino.h>
#include <WebServer.h>
#include <functional>

// Shared plumbing for the REST API modules.
//
// Each feature area registers its own routes through a RouteRegistrar supplied
// by HttpServer, so a module never sees the auth wrapper or the server
// lifecycle — it just declares endpoints. Adding a feature means adding one
// file and one register call, not editing a monolith.

// Registration callback. HttpServer wraps every route in authentication before
// handing it to the server, so a module cannot accidentally publish an
// unguarded endpoint.
using RouteRegistrar = std::function<void(const char*, HTTPMethod, WebServer::THandlerFunction)>;

// The live server, for reading request arguments inside handlers.
extern WebServer& apiServer;

void apiSendJson(int code, const String& json);
void apiSendOk();
void apiSendError(const char* msg);

// Feature modules.
void registerPowerApi(RouteRegistrar on);
void registerDriveApi(RouteRegistrar on);
void registerProtectionApi(RouteRegistrar on);
void registerZoneApi(RouteRegistrar on);
void registerProgramApi(RouteRegistrar on);
void registerSettingsApi(RouteRegistrar on);

#endif // API_COMMON_H
