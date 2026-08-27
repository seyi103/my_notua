#include "core/ble/blePeripheral.h"

#include "core/diag/log.h"

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {
constexpr const char* TAG = "BLE";
constexpr const char* DEVICE_NAME = "Notua";
constexpr const char* SERVICE_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210001";
constexpr const char* STATUS_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210002";
constexpr uint32_t INITIAL_ADVERTISING_MS = 60UL * 1000UL;
constexpr uint32_t RECONNECT_ADVERTISING_MS = 30UL * 1000UL;
constexpr uint32_t CONNECTED_INACTIVITY_MS = 120UL * 1000UL;
constexpr UBaseType_t EVENT_QUEUE_LENGTH = 8;

enum class BleEventType : uint8_t { connected, disconnected, gattActivity };
struct BleEvent {
    BleEventType type;
};

BleState gState = BleState::idle;
uint32_t gStateStartedMs = 0;
QueueHandle_t gLifecycleQueue = nullptr;
QueueHandle_t gActivityQueue = nullptr;
NimBLEServer* gServer = nullptr;
bool gDeviceInitialized = false;

bool enqueueEvent(BleEventType type) {
    const BleEvent event{type};
    if (type == BleEventType::gattActivity) {
        return gActivityQueue && xQueueOverwrite(gActivityQueue, &event) == pdTRUE;
    }
    return gLifecycleQueue && xQueueSend(gLifecycleQueue, &event, 0) == pdTRUE;
}

class ServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, ble_gap_conn_desc*) override {
        enqueueEvent(BleEventType::connected);
    }

    void onDisconnect(NimBLEServer*, ble_gap_conn_desc*) override {
        enqueueEvent(BleEventType::disconnected);
    }
};

class StatusCallbacks final : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic*) override {
        enqueueEvent(BleEventType::gattActivity);
    }
};

ServerCallbacks gServerCallbacks;
StatusCallbacks gStatusCallbacks;

void cleanupAfterInitializationFailure(const char* step) {
    logError(TAG, "initialization failed: step=%s", step);
    if (gDeviceInitialized) {
        NimBLEDevice::stopAdvertising();
        NimBLEDevice::deinit(true);
    }
    gDeviceInitialized = false;
    gServer = nullptr;
    if (gLifecycleQueue) {
        vQueueDelete(gLifecycleQueue);
        gLifecycleQueue = nullptr;
    }
    if (gActivityQueue) {
        vQueueDelete(gActivityQueue);
        gActivityQueue = nullptr;
    }
    gState = BleState::initializationFailed;
}
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
    gLifecycleQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(BleEvent));
    gActivityQueue = xQueueCreate(1, sizeof(BleEvent));
    if (!gLifecycleQueue || !gActivityQueue) {
        cleanupAfterInitializationFailure("event_queue");
        return false;
    }

    NimBLEDevice::init(DEVICE_NAME);
    gDeviceInitialized = true;
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);
    gServer = NimBLEDevice::createServer();
    if (!gServer) {
        cleanupAfterInitializationFailure("server");
        return false;
    }
    gServer->setCallbacks(&gServerCallbacks);
    gServer->advertiseOnDisconnect(false);
    NimBLEService* service = gServer->createService(SERVICE_UUID);
    if (!service) {
        cleanupAfterInitializationFailure("service");
        return false;
    }
    NimBLECharacteristic* status = service->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ);
    if (!status) {
        cleanupAfterInitializationFailure("status_characteristic");
        return false;
    }
    status->setCallbacks(&gStatusCallbacks);
    status->setValue("ready");
    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (!advertising) {
        cleanupAfterInitializationFailure("advertising");
        return false;
    }
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    const bool started = advertising->start();
    if (!started) {
        cleanupAfterInitializationFailure("advertising_start");
        return false;
    }
    gState = BleState::advertising;
    gStateStartedMs = millis();
    logInfo(TAG, "init=success service_uuid=%s advertising_started=true", SERVICE_UUID);
    return true;
}

void pollBlePeripheral() {
    if (!gLifecycleQueue) {
        return;
    }
    BleEvent event{};
    while (xQueueReceive(gLifecycleQueue, &event, 0) == pdTRUE) {
        const uint32_t now = millis();
        switch (event.type) {
        case BleEventType::connected:
            gState = BleState::connected;
            gStateStartedMs = now;
            logInfo(TAG, "client connected; inactivity_timeout_s=%u",
                static_cast<unsigned>(CONNECTED_INACTIVITY_MS / 1000));
            break;
        case BleEventType::gattActivity: break; // Activity uses its dedicated coalescing queue.
        case BleEventType::disconnected: {
            const bool started = NimBLEDevice::startAdvertising();
            gStateStartedMs = now;
            if (started) {
                gState = BleState::reconnectAdvertising;
                logInfo(TAG, "client disconnected; advertising_started=true timeout_s=%u",
                    static_cast<unsigned>(RECONNECT_ADVERTISING_MS / 1000));
            } else {
                gState = BleState::initializationFailed;
                logError(TAG, "client disconnected; advertising_started=false");
            }
            break;
        }
        }
    }
    if (gActivityQueue && xQueueReceive(gActivityQueue, &event, 0) == pdTRUE
        && gState == BleState::connected) {
        gStateStartedMs = millis();
        logDebug(TAG, "GATT activity; connected inactivity timeout refreshed");
    }
}

bool noteBleGattActivity() {
    return enqueueEvent(BleEventType::gattActivity);
}

BleState bleState() {
    return gState;
}

bool bleSessionExpired() {
    uint32_t timeout = 0;
    switch (gState) {
    case BleState::advertising: timeout = INITIAL_ADVERTISING_MS; break;
    case BleState::reconnectAdvertising: timeout = RECONNECT_ADVERTISING_MS; break;
    case BleState::connected: timeout = CONNECTED_INACTIVITY_MS; break;
    default: return false;
    }
    return millis() - gStateStartedMs >= timeout;
}

void stopBlePeripheral() {
    const BleState finalState = gState == BleState::initializationFailed
        ? BleState::initializationFailed : BleState::stopped;
    if (gServer) {
        for (const uint16_t connection : gServer->getPeerDevices()) {
            gServer->disconnect(connection);
        }
    }
    if (gDeviceInitialized) {
        NimBLEDevice::stopAdvertising();
        NimBLEDevice::deinit(true);
    }
    gDeviceInitialized = false;
    gServer = nullptr;
    if (gLifecycleQueue) {
        vQueueDelete(gLifecycleQueue);
        gLifecycleQueue = nullptr;
    }
    if (gActivityQueue) {
        vQueueDelete(gActivityQueue);
        gActivityQueue = nullptr;
    }
    gState = finalState;
    logInfo(TAG, "connections closed; advertising stopped; BLE deinitialized");
}
