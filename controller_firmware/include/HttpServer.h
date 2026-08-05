#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <Arduino.h>

// Register all HTTP routes and start the web server.
void setupWebServer();

// Call from loop() to handle pending HTTP requests.
void handleWebClients();

#endif // HTTP_SERVER_H
