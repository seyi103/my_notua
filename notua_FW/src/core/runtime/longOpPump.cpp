#include "core/runtime/longOpPump.h"

#include <Arduino.h>
#include "core/power/watchdog.h"

void longOpPump() {
    feedWatchdog();
    yield();
}
