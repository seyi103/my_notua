#include <Arduino.h>

#include "core/diag/log.h"
#include "core/power/boardPower.h"
#include "core/power/watchdog.h"

namespace {
constexpr const char* TAG = "APP";
}

void setup() {
    initLog(115200, LOG_LEVEL_INFO);

    // A reset during an EPD transaction must never leave the panel rails enabled.
    boardPowerT2001Off();

    if (!beginWatchdog()) {
        logError(TAG, "watchdog initialization failed");
    }

    if (!psramFound()) {
        logError(TAG, "PSRAM unavailable");
    } else {
        logInfo(TAG, "PSRAM ready: %u bytes", static_cast<unsigned>(ESP.getPsramSize()));
    }

    // Transport and application layers are intentionally not started yet. The EPD
    // driver remains linked and ready for the future BLE frame-delivery boundary.
    logInfo(TAG, "offline EPD core ready; frame transport not configured");
}

void loop() {
    feedWatchdog();
    delay(20);
}
