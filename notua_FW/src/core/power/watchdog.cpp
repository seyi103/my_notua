#include "core/power/watchdog.h"

#include <esp_task_wdt.h>

void feedWatchdog() {
    // Reset is harmless when the calling task is not registered with the TWDT.
    (void) esp_task_wdt_reset();
}
