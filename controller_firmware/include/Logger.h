#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

typedef enum : uint8_t {
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
} LogLevel;

// Log a message to Serial and in-memory ring buffer.
void Log(LogLevel level, const String& message);

// Returns last 'count' log entries as a JSON array string.
String getLogsJson(int count);

// Clears the in-memory log ring buffer.
void clearLogs();

// Returns the currently active minimum log level.
LogLevel getLogLevel();

// Sets the minimum log level (messages below this level are suppressed).
void setLogLevel(LogLevel level);

#endif // LOGGER_H
