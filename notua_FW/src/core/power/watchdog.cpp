#include "core/power/watchdog.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

bool beginWatchdog(uint32_t timeoutSeconds) {
    if (timeoutSeconds == 0) {
        return false;
    }
    esp_err_t result = esp_task_wdt_init(timeoutSeconds, true);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return false;
    }
    result = esp_task_wdt_add(nullptr);
    return result == ESP_OK || result == ESP_ERR_INVALID_STATE;
}

void feedWatchdog() {
    (void) esp_task_wdt_reset();
}
