#pragma once
#include <stdint.h>

enum LogLevel : uint8_t {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG
};

void initLog(uint32_t baudRate, LogLevel level);
void waitForLogHost(uint32_t timeoutMs);

void logError(const char* tag, const char* fmt, ...);
void logWarn(const char* tag, const char* fmt, ...);
void logInfo(const char* tag, const char* fmt, ...);
void logDebug(const char* tag, const char* fmt, ...);
