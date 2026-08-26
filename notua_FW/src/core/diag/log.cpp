#include "log.h"
#include <Arduino.h>
#include <stdarg.h>

static LogLevel gLogLevel = LOG_LEVEL_INFO;

static void logV(LogLevel level, const char* prefix, const char* tag, const char* fmt, va_list args) {
    if (level > gLogLevel) {
        return;
    }

    // [I][TAG] message
    Serial.print(prefix);
    Serial.print("[");
    Serial.print(tag);
    Serial.print("] ");

    char buf[192];
    vsnprintf(buf, sizeof(buf), fmt, args);
    Serial.println(buf);
}

void initLog(uint32_t baudRate, LogLevel level) {
    gLogLevel = level;

    Serial.begin(baudRate);
    delay(200); // USB CDC 안정화
}

void logError(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(LOG_LEVEL_ERROR, "E", tag, fmt, args);
    va_end(args);
}

void logWarn(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(LOG_LEVEL_WARN, "W", tag, fmt, args);
    va_end(args);
}

void logInfo(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(LOG_LEVEL_INFO, "I", tag, fmt, args);
    va_end(args);
}

void logDebug(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(LOG_LEVEL_DEBUG, "D", tag, fmt, args);
    va_end(args);
}
