#pragma once

#include <Arduino.h>

enum class BleState {
    idle,
    advertising,
    connected,
    reconnectAdvertising,
    stopped,
    initializationFailed,
};

bool beginBlePeripheral();
void pollBlePeripheral();
BleState bleState();
const char* bleStateName(BleState state);
bool bleSessionExpired();
void stopBlePeripheral();

