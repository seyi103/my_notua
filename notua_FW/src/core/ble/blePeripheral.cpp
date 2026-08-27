#include "core/ble/blePeripheral.h"

#include "core/diag/log.h"

#include <NimBLEDevice.h>

namespace {
constexpr const char* TAG = "BLE";
constexpr const char* DEVICE_NAME = "Notua";
constexpr const char* SERVICE_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210001";
constexpr const char* STATUS_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210002";
constexpr uint32_t INITIAL_ADVERTISING_MS = 60UL * 1000UL;
constexpr uint32_t RECONNECT_ADVERTISING_MS = 30UL * 1000UL;

volatile BleState gState = BleState::idle;
uint32_t gAdvertisingStartedMs = 0;

class ServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override {
        gState = BleState::connected;
        logInfo(TAG, "client connected");
    }

    void onDisconnect(NimBLEServer*) override {
        gState = BleState::reconnectAdvertising;
        gAdvertisingStartedMs = millis();
        NimBLEDevice::startAdvertising();
        logInfo(TAG, "client disconnected; reconnect advertising started=true timeout_s=%u",
            static_cast<unsigned>(RECONNECT_ADVERTISING_MS / 1000));
    }
};

ServerCallbacks gCallbacks;
} // namespace

const char* bleStateName(BleState state) {
    switch (state) {
    case BleState::idle: return "idle";
    case BleState::advertising: return "advertising";
    case BleState::connected: return "connected";
    case BleState::reconnectAdvertising: return "reconnect_advertising";
    case BleState::stopped: return "stopped";
    case BleState::initializationFailed: return "initialization_failed";
    }
    return "unknown";
}

bool beginBlePeripheral() {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);
    NimBLEServer* server = NimBLEDevice::createServer();
    if (!server) {
        gState = BleState::initializationFailed;
        return false;
    }
    server->setCallbacks(&gCallbacks);
    NimBLEService* service = server->createService(SERVICE_UUID);
    if (!service) {
        gState = BleState::initializationFailed;
        return false;
    }
    NimBLECharacteristic* status = service->createCharacteristic(
        STATUS_UUID, NIMBLE_PROPERTY::READ);
    if (!status) {
        gState = BleState::initializationFailed;
        return false;
    }
    status->setValue("ready");
    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    const bool started = advertising->start();
    gState = started ? BleState::advertising : BleState::initializationFailed;
    gAdvertisingStartedMs = millis();
    logInfo(TAG, "init=%s service_uuid=%s advertising_started=%s",
        server ? "success" : "failed", SERVICE_UUID, started ? "true" : "false");
    return started;
}

void pollBlePeripheral() {
    // Callbacks only update state and restart advertising; policy stays in the Arduino task.
}

BleState bleState() {
    return gState;
}

bool bleSessionExpired() {
    const BleState state = gState;
    const uint32_t timeout = state == BleState::advertising
        ? INITIAL_ADVERTISING_MS
        : RECONNECT_ADVERTISING_MS;
    return (state == BleState::advertising || state == BleState::reconnectAdvertising)
        && (millis() - gAdvertisingStartedMs >= timeout);
}

void stopBlePeripheral() {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
    gState = BleState::stopped;
    logInfo(TAG, "advertising stopped; BLE deinitialized");
}
