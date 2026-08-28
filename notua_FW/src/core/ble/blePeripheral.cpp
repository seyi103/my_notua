#include "core/ble/blePeripheral.h"

#include "core/diag/log.h"
#include "core/ble/transferProtocol.h"
#include "core/storage/imageStorage.h"
#include "core/storage/imageCatalog.h"
#include "core/storage/playlistStore.h"

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
constexpr const char* CATALOG_UUID = "7d2a4b70-8e67-4d8b-9f3a-36c89e210006";
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
NimBLECharacteristic* gCatalogCharacteristic = nullptr;
bool gDeviceInitialized = false;
notua::storage::ImageStorage gStorage;
notua::storage::PlaylistStore gPlaylistStore;
bool gCommitted = false;
bool gPendingDurable = false;
bool gApplyRequested = false;

bool refreshCatalogValue() {
    using namespace notua::storage;
    CatalogEntry entries[MAX_IMAGES];
    if (!scanFixedCatalog(LittleFS, entries)) return false;
    Playlist active{};
    Playlist target{}; uint8_t completed = 0; SyncStage stage = SyncStage::idle;
    if (gPlaylistStore.syncInProgress()) {
        if (!gPlaylistStore.loadActiveMetadata(active)
            || !gPlaylistStore.reconcileCompleted(entries)
            || !gPlaylistStore.loadTarget(target, completed, stage)) return false;
    } else if (!gPlaylistStore.loadActiveValidated(entries, active)) return false;
    uint8_t bytes[CATALOG_BYTES]; encodeCatalog(bytes, entries, active, target, stage, completed);
    if (gCatalogCharacteristic) gCatalogCharacteristic->setValue(bytes, sizeof(bytes));
    return true;
}

enum class TransferEventType : uint8_t { control, data };
struct TransferEvent { TransferEventType type; uint16_t length; uint8_t bytes[notua::transfer::MAX_GATT_VALUE_BYTES]; };
QueueHandle_t gTransferQueue = nullptr;
struct FeedbackEvent { notua::transfer::Status status; uint32_t detail; };
QueueHandle_t gFeedbackQueue = nullptr;

bool publishStatus(notua::transfer::Status status, uint32_t offset, uint32_t detail = 0) {
    if (!gTransferStatus) { logError(TAG, "status update failed: characteristic unavailable"); return false; }
    uint8_t packet[notua::transfer::STATUS_BYTES];
    notua::transfer::encodeStatus(packet, status, offset, detail);
    gTransferStatus->setValue(packet, sizeof(packet));
    if (gState != BleState::connected) {
        logWarn(TAG, "status value updated without notification: code=%u not connected",
            static_cast<unsigned>(status));
        return false;
    }
    // NimBLE-Arduino 1.4.3 exposes notify() as void. Reaching this call while connected is the
    // strongest synchronous success result available; timeout recovery reads the retained value.
    gTransferStatus->notify();
    return true;
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
    gCatalogCharacteristic = nullptr;
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
    gPlaylistStore.end();
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
    if (!gLifecycleQueue || !gActivityQueue || !gTransferQueue || !gFeedbackQueue
        || !gStorage.begin() || !gPlaylistStore.begin()) {
        cleanupAfterInitializationFailure("event_queue");
        return false;
    }

    NimBLEDevice::init(DEVICE_NAME);
    gDeviceInitialized = true;
    constexpr uint16_t PREFERRED_MTU = 517;
    const int mtuResult = NimBLEDevice::setMTU(PREFERRED_MTU);
    logInfo(TAG, "preferred MTU: requested=%u result=%d success=%s",
        static_cast<unsigned>(PREFERRED_MTU), mtuResult, mtuResult == 0 ? "true" : "false");
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
    gCatalogCharacteristic = service->createCharacteristic(CATALOG_UUID, NIMBLE_PROPERTY::READ);
    if (!control || !data || !gTransferStatus || !gCatalogCharacteristic) {
        cleanupAfterInitializationFailure("transfer_characteristics"); return false;
    }
    control->setCallbacks(&gControlCallbacks); data->setCallbacks(&gDataCallbacks);
    uint8_t initial[notua::transfer::STATUS_BYTES];
    notua::transfer::encodeStatus(initial, notua::transfer::Status::notReady, 0, 0);
    gTransferStatus->setValue(initial, sizeof(initial));
    gCatalogCharacteristic->setCallbacks(&gStatusCallbacks);
    if (!refreshCatalogValue()) { cleanupAfterInitializationFailure("catalog"); return false; }
    Preferences pendingPreferences;
    if (pendingPreferences.begin("photo-cycle", true)) {
        gPendingDurable = pendingPreferences.getString("pending", "").length() != 0;
        gCommitted = !pendingPreferences.getBool("sync", false) && gPendingDurable;
        pendingPreferences.end();
    }
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
            {
                Preferences pending;
                if (pending.begin("photo-cycle", true)) {
                    gPendingDurable = pending.getString("pending", "").length() != 0;
                    gCommitted = !pending.getBool("sync", false) && gPendingDurable;
                    pending.end();
                }
            }
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
    notua::transfer::AckCoalescer acknowledgements;
    bool transferError = false;
    while (gTransferQueue && xQueueReceive(gTransferQueue, &transfer, 0) == pdTRUE) {
        using namespace notua::transfer;
        if (transfer.type == TransferEventType::data) {
            if (transfer.length < 5) {
                publishStatus(Status::badCommand, gStorage.offset(), transfer.length);
                transferError = true; continue;
            }
            const uint32_t offset = readLe32(transfer.bytes);
            if (!gStorage.active()) { publishStatus(Status::notReady, gStorage.offset()); transferError = true; }
            else if (classifyOffset(offset, gStorage.offset()) == OffsetDisposition::duplicate) {
                if (acknowledgements.persistedPacket()) publishStatus(Status::ack, gStorage.offset());
            } else if (classifyOffset(offset, gStorage.offset()) == OffsetDisposition::future) {
                publishStatus(Status::badOffset, gStorage.offset(), offset); transferError = true;
            } else if (!gStorage.append(transfer.bytes + 4, transfer.length - 4)) {
                publishStatus(Status::storageError, gStorage.offset()); transferError = true;
            } else if (acknowledgements.persistedPacket()) publishStatus(Status::ack, gStorage.offset());
            continue;
        }
        if (!transferError && acknowledgements.flush(true)) {
            publishStatus(Status::ack, gStorage.offset());
        }
        StartCommand start{};
        if (parseStart(transfer.bytes, transfer.length, start)) {
            if (start.size != IMAGE_BYTES) publishStatus(Status::badSize, 0, start.size);
            else if (start.slot >= notua::storage::MAX_IMAGES) publishStatus(Status::badCommand, 0, start.slot);
            else {
                notua::storage::Playlist target{}; uint8_t done = 0;
                notua::storage::SyncStage stage{};
                bool expected = false;
                if (gPlaylistStore.loadTarget(target, done, stage)) for (uint8_t i = 0; i < target.count; ++i)
                    if (target.slots[i] == start.slot && target.crc32[i] == start.crc32) expected = true;
                if (!expected) { publishStatus(Status::notReady, 0, start.slot); continue; }
                const auto result = gStorage.start(start.slot, start.size, start.crc32);
                using notua::storage::StartResult;
                if (result == StartResult::ok) { gCommitted = false; publishStatus(Status::startAccepted, 0); }
                else if (result == StartResult::badSize) publishStatus(Status::badSize, 0, start.size);
                else if (result == StartResult::badSlot) publishStatus(Status::badCommand, 0, start.slot);
                else if (result == StartResult::noSpace || result == StartResult::openFailed
                    || result == StartResult::cleanupFailed) publishStatus(Status::storageError, 0, static_cast<uint32_t>(result));
                else publishStatus(Status::notReady, 0, static_cast<uint32_t>(result));
            }
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::abort)) {
            gStorage.abort(); gCommitted = false; publishStatus(Status::ack, 0);
        } else if (transfer.length == 2 + notua::storage::PLAYLIST_BYTES
            && transfer.bytes[0] == VERSION && transfer.bytes[1] == static_cast<uint8_t>(Opcode::syncBegin)) {
            notua::storage::Playlist target{};
            if (!notua::storage::decodePlaylist(transfer.bytes + 2,
                    notua::storage::PLAYLIST_BYTES, target)
                || !gPlaylistStore.beginSync(target) || !refreshCatalogValue())
                publishStatus(Status::badCommand, 0);
            else { gCommitted = false; publishStatus(Status::syncAccepted, 0); }
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::finish)) {
            uint32_t detail = 0; const auto result = gStorage.finish(detail);
            using notua::storage::CommitResult;
            if (result == CommitResult::committed) {
                if (!gPlaylistStore.markSlotComplete(gStorage.slot())) {
                    const auto rollback = gStorage.rollbackCommit();
                    if (rollback != notua::storage::CleanupResult::ok)
                        logError(TAG, "commit rollback failed: result=%u", static_cast<unsigned>(rollback));
                    publishStatus(Status::storageError, gStorage.offset());
                } else {
                    const auto durable = gStorage.markCommitDurable();
                    if (durable != notua::storage::CleanupResult::ok) {
                        gStorage.rollbackCommit(); refreshCatalogValue();
                        publishStatus(Status::storageError, gStorage.offset(), static_cast<uint32_t>(durable));
                    } else {
                        const auto cleanup = gStorage.finalizeCommit();
                        if (cleanup != notua::storage::CleanupResult::ok)
                            logWarn(TAG, "commit cleanup incomplete: result=%u; recovery will finish cleanup",
                                static_cast<unsigned>(cleanup));
                        refreshCatalogValue(); publishStatus(Status::committed, gStorage.offset());
                    }
                }
            } else if (result == CommitResult::badSize) publishStatus(Status::badSize, gStorage.offset(), detail);
            else if (result == CommitResult::crcMismatch) publishStatus(Status::crcMismatch, gStorage.offset(), detail);
            else if (result == CommitResult::notReady) publishStatus(Status::notReady, gStorage.offset());
            else publishStatus(Status::storageError, gStorage.offset(), static_cast<uint32_t>(result));
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::playlistCommit)) {
            notua::storage::CatalogEntry catalog[notua::storage::MAX_IMAGES]; String pending;
            if (!notua::storage::scanFixedCatalog(LittleFS, catalog)
                || !gPlaylistStore.commitTarget(catalog, pending) || !refreshCatalogValue())
                publishStatus(Status::notReady, 0);
            else { gCommitted = true; gPendingDurable = true;
                publishStatus(Status::playlistCommitted, 0); }
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::getCatalog)) {
            if (!refreshCatalogValue()) publishStatus(Status::storageError, 0);
            else publishStatus(Status::ack, 0);
        } else if (isSimpleCommand(transfer.bytes, transfer.length, Opcode::apply)) {
            if (!notua::storage::applyAllowed(gPlaylistStore.syncInProgress(),
                    gCommitted, gPendingDurable))
                publishStatus(Status::notReady, gStorage.offset());
            else { publishStatus(Status::applying, gStorage.offset()); gApplyRequested = true; }
        } else publishStatus(Status::badCommand, gStorage.offset(), transfer.length);
    }
    if (!transferError && acknowledgements.flush(true))
        publishStatus(notua::transfer::Status::ack, gStorage.offset());
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
    gCatalogCharacteristic = nullptr;
    gStorage.abort();
    gPlaylistStore.end();
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
