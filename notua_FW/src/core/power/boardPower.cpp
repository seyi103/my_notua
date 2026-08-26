#include "core/power/boardPower.h"

#include <Arduino.h>
#include "core/board/pins.h"
#include "core/diag/log.h"

namespace {
constexpr const char* TAG = "BOARD_PWR";

void configureOutput(uint8_t pin) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}
} // namespace

bool boardPowerT2001OnForSpi() {
    configureOutput(PIN_1V25);
    configureOutput(PIN_1V8);
    configureOutput(PIN_6V5);
    configureOutput(PIN_T2001_3V3);
    configureOutput(T2001_RST);
    configureOutput(T2001_WK);

    digitalWrite(T2001_WK, HIGH);
    digitalWrite(PIN_1V25, HIGH);
    delay(5);
    digitalWrite(PIN_1V8, HIGH);
    delay(5);
    digitalWrite(PIN_T2001_3V3, HIGH);
    delay(10);
    digitalWrite(T2001_RST, HIGH);
    delay(10);
    logDebug(TAG, "T2001 SPI rails on");
    return true;
}

bool boardPowerPrepareEpdUpdate() {
    digitalWrite(PIN_6V5, HIGH);
    delay(10);
    return digitalRead(PIN_6V5) == HIGH;
}

void boardPowerFinishEpdUpdate() {
    digitalWrite(PIN_6V5, LOW);
    delay(5);
}

void boardPowerT2001Off() {
    boardPowerFinishEpdUpdate();
    digitalWrite(T2001_RST, LOW);
    digitalWrite(PIN_T2001_3V3, LOW);
    delay(5);
    digitalWrite(PIN_1V8, LOW);
    delay(5);
    digitalWrite(PIN_1V25, LOW);
    digitalWrite(T2001_WK, LOW);
    logDebug(TAG, "T2001 rails off");
}
