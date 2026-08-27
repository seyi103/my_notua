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
// Thread-safe hook for future GATT callbacks to refresh connected inactivity.
bool noteBleGattActivity();
BleState bleState();
const char* bleStateName(BleState state);
bool bleSessionExpired();
void stopBlePeripheral();
