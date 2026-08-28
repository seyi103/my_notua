#include "core/ble/blePeripheral.h"

#include "core/diag/log.h"
#include "core/ble/transferProtocol.h"
#include "core/storage/imageStorage.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {
constexpr const char* TAG = "BLE";
constexpr const char* DEVICE_NAME = "Notua";
constexpr const char* SERVICE_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210001";
constexpr const char* STATUS_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210002";
constexpr const char* CONTROL_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210003";
constexpr const char* DATA_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210004";
constexpr const char* TRANSFER_STATUS_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210005";
constexpr uint32_t INITIAL_ADVERTISING_MS = 60UL * 1000UL;
constexpr uint32_t RECONNECT_ADVERTISING_MS = 30UL * 1000UL;
constexpr uint32_t CONNECTED_INACTIVITY_MS = 120UL * 1000UL;
constexpr UBaseType_t EVENT_QUEUE_LENGTH = 8;
constexpr UBaseType_t TRANSFER_QUEUE_LENGTH = 8;

enum class BleEventType : uint8_t { connected, disconnected, gattActivity };
struct BleEvent {
    BleEventType type;
};

BleState gState = BleState::idle;
uint32_t gStateStartedMs = 0;
QueueHandle_t gLifecycleQueue = nullptr;
QueueHandle_t gActivityQueue = nullptr;
NimBLEServer* gServer = nullptr;
NimBLECharacteristic* gTransferStatus = nullptr;
bool gDeviceInitialized = false;
notua::storage::ImageStorage gStorage;
bool gCommitted = false;
bool gApplyRequested = false;

enum class TransferEventType : uint8_t { control, data };
struct TransferEvent { TransferEventType type; uint16_t length; uint8_t bytes[notua::transfer::MAX_GATT_VALUE_BYTES]; };
QueueHandle_t gTransferQueue = nullptr;
struct FeedbackEvent { notua::transfer::Status status; uint32_t detail; };
QueueHandle_t gFeedbackQueue = nullptr;

void publishStatus(notua::transfer::Status status, uint32_t offset, uint32_t detail = 0) {
    if (!gTransferStatus) return;
    uint8_t packet[notua::transfer::STATUS_BYTES];
    notua::transfer::encodeStatus(packet, status, offset, detail);
    gTransferStatus->setValue(packet, sizeof(packet));
    if (gState == BleState::connected) gTransferStatus->notify();
}

bool enqueueTransfer(TransferEventType type, const std::string& value) {
    if (!gTransferQueue || value.size() > notua::transfer::MAX_GATT_VALUE_BYTES) return false;
    TransferEvent event{}; event.type = type; event.length = value.size();
    if (event.length) memcpy(event.bytes, value.data(), event.length);
    return xQueueSend(gTransferQueue, &event, 0) == pdTRUE;
}

void enqueueFeedback(notua::transfer::Status status, uint32_t detail = 0) {
    const FeedbackEvent event{status, detail};
    if (gFeedbackQueue) xQueueSend(gFeedbackQueue, &event, 0);
}

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

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic) override {
        noteBleGattActivity();
        if (!enqueueTransfer(TransferEventType::control, characteristic->getValue()))
            enqueueFeedback(notua::transfer::Status::queueFull);
    }
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic) override {
        noteBleGattActivity();
        const std::string value = characteristic->getValue();
        if (value.size() > notua::transfer::MAX_GATT_VALUE_BYTES)
            enqueueFeedback(notua::transfer::Status::badCommand, value.size());
        else if (!enqueueTransfer(TransferEventType::data, value))
            enqueueFeedback(notua::transfer::Status::queueFull);
    }
};

ServerCallbacks gServerCallbacks;
StatusCallbacks gStatusCallbacks;
ControlCallbacks gControlCallbacks;
DataCallbacks gDataCallbacks;

void cleanupAfterInitializationFailure(const char* step) {
    logError(TAG, "initialization failed: step=%s", step);
    if (gDeviceInitialized) {
        NimBLEDevice::stopAdvertising();
        NimBLEDevice::deinit(true);
    }
    gDeviceInitialized = false;
    gServer = nullptr;
    gTransferStatus = nullptr;
    if (gLifecycleQueue) {
        vQueueDelete(gLifecycleQueue);
        gLifecycleQueue = nullptr;
    }
    if (gActivityQueue) {
        vQueueDelete(gActivityQueue);
        gActivityQueue = nullptr;
    }
    if (gTransferQueue) { vQueueDelete(gTransferQueue); gTransferQueue = nullptr; }
    if (gFeedbackQueue) { vQueueDelete(gFeedbackQueue); gFeedbackQueue = nullptr; }
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
    gTransferQueue = xQueueCreate(TRANSFER_QUEUE_LENGTH, sizeof(TransferEvent));
    gFeedbackQueue = xQueueCreate(TRANSFER_QUEUE_LENGTH, sizeof(FeedbackEvent));
    if (!gLifecycleQueue || !gActivityQueue || !gTransferQueue || !gFeedbackQueue || !gStorage.begin()) {
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
    NimBLECharacteristic* control = service->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
    NimBLECharacteristic* data = service->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::WRITE_NR);
    gTransferStatus = service->createCharacteristic(TRANSFER_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    if (!control || !data || !gTransferStatus) {
        cleanupAfterInitializationFailure("transfer_characteristics"); return false;
    }
    control->setCallbacks(&gControlCallbacks); data->setCallbacks(&gDataCallbacks);
    uint8_t initial[notua::transfer::STATUS_BYTES];
    notua::transfer::encodeStatus(initial, notua::transfer::Status::notReady, 0, 0);
    gTransferStatus->setValue(initial, sizeof(initial));
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
            gStorage.abort(); gCommitted = false;
            if (gTransferQueue) xQueueReset(gTransferQueue);
            if (gFeedbackQueue) xQueueReset(gFeedbackQueue);
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
    TransferEvent transfer{};
    while (gTransferQueue && xQueueReceive(gTransferQueue, &transfer, 0) == pdTRUE) {
        using namespace notua::transfer;
        if (transfer.type == TransferEventType::data) {
            if (transfer.length < 5) { publishStatus(Status::badCommand, gStorage.offset(), transfer.length); continue; }
            const uint32_t offset = readLe32(transfer.bytes);
            if (!gStorage.active()) publishStatus(Status::notReady, gStorage.offset());
            else if (offset < gStorage.offset()) publishStatus(Status::ack, gStorage.offset());
            else if (offset > gStorage.offset()) publishStatus(Status::badOffset, gStorage.offset(), offset);
            else if (!gStorage.append(transfer.bytes + 4, transfer.length - 4))
                publishStatus(Status::storageError, gStorage.offset());
            else publishStatus(Status::ack, gStorage.offset());
            continue;
        }
        StartCommand start{};
        if (parseStart(transfer.bytes, transfer.length, start)) {
            if (start.size != IMAGE_BYTES) publishStatus(Status::badSize, 0, start.size);
            else if (start.slot > 2) publishStatus(Status::badCommand, 0, start.slot);
            else if (!gStorage.start(start.slot, start.size, start.crc32))
                publishStatus(Status::notReady, 0, start.slot);
            else { gCommitted = false; publishStatus(Status::startAccepted, 0); }
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::abort)) {
            gStorage.abort(); gCommitted = false; publishStatus(Status::ack, 0);
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::finish)) {
            uint32_t detail = 0; const Status result = gStorage.finish(detail);
            if (result == Status::committed) {
                Preferences preferences;
                const bool opened = preferences.begin("photo-cycle", false);
                const bool stored = opened
                    && preferences.putString("pending", gStorage.committedPath()) != 0;
                if (!stored) {
                    if (opened) { preferences.remove("pending"); preferences.end(); }
                    gStorage.rollbackCommit();
                    publishStatus(Status::storageError, gStorage.offset());
                } else {
                    preferences.end(); gStorage.finalizeCommit(); gCommitted = true;
                    publishStatus(result, gStorage.offset());
                }
            } else publishStatus(result, gStorage.offset(), detail);
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::apply)) {
            if (!gCommitted) publishStatus(Status::notReady, gStorage.offset());
            else { publishStatus(Status::applying, gStorage.offset()); gApplyRequested = true; }
        } else publishStatus(Status::badCommand, gStorage.offset(), transfer.length);
    }
    FeedbackEvent feedback{};
    while (gFeedbackQueue && xQueueReceive(gFeedbackQueue, &feedback, 0) == pdTRUE)
        publishStatus(feedback.status, gStorage.offset(), feedback.detail);
    if (gActivityQueue && xQueueReceive(gActivityQueue, &event, 0) == pdTRUE
        && gState == BleState::connected) {
        gStateStartedMs = millis();
        logDebug(TAG, "GATT activity; connected inactivity timeout refreshed");
    }
}

bool consumeBleApplyRequest() { const bool value = gApplyRequested; gApplyRequested = false; return value; }

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
    gTransferStatus = nullptr;
    gStorage.abort();
    if (gLifecycleQueue) {
        vQueueDelete(gLifecycleQueue);
        gLifecycleQueue = nullptr;
    }
    if (gActivityQueue) {
        vQueueDelete(gActivityQueue);
        gActivityQueue = nullptr;
    }
    if (gTransferQueue) { vQueueDelete(gTransferQueue); gTransferQueue = nullptr; }
    if (gFeedbackQueue) { vQueueDelete(gFeedbackQueue); gFeedbackQueue = nullptr; }
    gState = finalState;
    logInfo(TAG, "connections closed; advertising stopped; BLE deinitialized");
}
