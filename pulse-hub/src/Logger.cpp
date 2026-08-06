#include "Logger.h"
#include <Arduino.h>
#include <time.h>

static LogLevel activeLogLevel = INFO;

// ---------------------------------------------------------------------------
//  Ring buffer – 50 entries, each max 120 chars
// ---------------------------------------------------------------------------
#define LOG_BUF_SIZE   50
#define LOG_ENTRY_LEN  120

static char  logBuf[LOG_BUF_SIZE][LOG_ENTRY_LEN];
static int   logHead  = 0;   // next write position
static int   logCount = 0;   // total entries (capped at LOG_BUF_SIZE)

static const char* levelStr(LogLevel level) {
    switch (level) {
        case DEBUG: return "DEBUG";
        case INFO:  return "INFO";
        case WARN:  return "WARN";
        case ERROR: return "ERROR";
        default:    return "?";
    }
}

// Returns a human-readable timestamp: "HH:MM:SS" (wall clock if NTP synced,
// "+HH:MM:SS" uptime otherwise).
static void fmtTimestamp(char* buf, size_t len) {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        // Wall-clock time from NTP
        snprintf(buf, len, "%02d:%02d:%02d",
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        // Uptime fallback – prefix with '+' so it's distinguishable
        unsigned long ms = millis();
        unsigned long s  = ms / 1000;
        snprintf(buf, len, "+%02lu:%02lu:%02lu",
                 s / 3600, (s % 3600) / 60, s % 60);
    }
}

void Log(LogLevel level, const String& message) {
    if (level < activeLogLevel) return;
    char ts[12];
    fmtTimestamp(ts, sizeof(ts));
    char entry[LOG_ENTRY_LEN];
    snprintf(entry, sizeof(entry), "[%s][%s] %s",
             ts, levelStr(level), message.c_str());
    // Write to ring buffer
    strncpy(logBuf[logHead], entry, LOG_ENTRY_LEN - 1);
    logBuf[logHead][LOG_ENTRY_LEN - 1] = '\0';
    logHead = (logHead + 1) % LOG_BUF_SIZE;
    if (logCount < LOG_BUF_SIZE) logCount++;
    // Also echo to Serial
    Serial.println(entry);
}

// Returns the last 'count' log entries as a JSON array string
String getLogsJson(int count) {
    if (count > logCount) count = logCount;
    String out = "[";
    // Walk oldest → newest
    int start = (logCount < LOG_BUF_SIZE)
                ? 0
                : logHead;  // oldest slot when buffer is full
    // We want only the last 'count' entries
    int skip = logCount - count;
    for (int i = 0; i < logCount; i++) {
        int idx = (start + i) % LOG_BUF_SIZE;
        if (i < skip) continue;
        if (i > skip) out += ',';
        out += '"';
        // Escape characters that would break JSON strings
        for (const char* p = logBuf[idx]; *p; p++) {
            char c = *p;
            if      (c == '"')  { out += '\\'; out += '"';  }
            else if (c == '\\') { out += '\\'; out += '\\'; }
            else if (c == '\n') { out += '\\'; out += 'n';  }
            else if (c == '\r') { /* skip */                }
            else if (c == '\t') { out += '\\'; out += 't';  }
            else                { out += c;                 }
        }
        out += '"';
    }
    out += "]";
    return out;
}

void clearLogs() {
    logHead  = 0;
    logCount = 0;
    memset(logBuf, 0, sizeof(logBuf));
}

LogLevel getLogLevel() {
    return activeLogLevel;
}

void setLogLevel(LogLevel level) {
    activeLogLevel = level;
}
