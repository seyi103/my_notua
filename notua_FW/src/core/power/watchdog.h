#pragma once
#include <stdint.h>

/** Register the Arduino loop task with the ESP32 task watchdog. */
bool beginWatchdog(uint32_t timeoutSeconds = 30);

/** Feed the watchdog for the task currently performing a long operation. */
void feedWatchdog();
