#include "core/ble/frameTransfer.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "core/storage/frameStore.h"

namespace frame_transfer {
namespace {
constexpr const char* kServiceUuid = "7a4b0001-6f9d-4db7-8f65-8f9d2ec0a001";
constexpr const char* kControlUuid = "7a4b0002-6f9d-4db7-8f65-8f9d2ec0a001";
constexpr const char* kDataUuid = "7a4b0003-6f9d-4db7-8f65-8f9d2ec0a001";
constexpr const char* kStatusUuid = "7a4b0004-6f9d-4db7-8f65-8f9d2ec0a001";

BLECharacteristic* gStatus = nullptr;
volatile bool gDisplayRequested = false;

uint16_t readLe16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0] | (value[1] << 8));
}

uint32_t readLe32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0])
        | (static_cast<uint32_t>(value[1]) << 8)
        | (static_cast<uint32_t>(value[2]) << 16)
        | (static_cast<uint32_t>(value[3]) << 24);
}

void report(const char* status) {
    if (gStatus) {
        gStatus->setValue(status);
        gStatus->notify();
    }
}

class ControlCallbacks final : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        const std::string value = characteristic->getValue();
        if (value.empty()) {
            report("bad-control");
            return;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
        switch (bytes[0]) {
        case 0x01: {
            if (value.size() != 9) {
                report("bad-start");
                return;
            }
            frame_store::Metadata metadata {};
            metadata.width = readLe16(bytes + 1);
            metadata.height = readLe16(bytes + 3);
            metadata.bytes = readLe32(bytes + 5);
            report(frame_store::beginWrite(metadata) ? "ready" : "rejected");
            break;
        }
        case 0x02:
            if (frame_store::commit()) {
                gDisplayRequested = true;
                report("stored");
            } else {
                report("commit-failed");
            }
            break;
        case 0x03:
            frame_store::abortWrite();
            report("aborted");
            break;
        default:
            report("bad-opcode");
            break;
        }
    }
};

class DataCallbacks final : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        const std::string value = characteristic->getValue();
        if (value.empty() || !frame_store::append(
                reinterpret_cast<const uint8_t*>(value.data()), value.size())) {
            report("write-failed");
        }
    }
};
} // namespace

bool begin(const char* deviceName) {
    BLEDevice::init(deviceName);
    BLEServer* server = BLEDevice::createServer();
    if (!server) {
        return false;
    }
    BLEService* service = server->createService(kServiceUuid);
    BLECharacteristic* control = service->createCharacteristic(
        kControlUuid, BLECharacteristic::PROPERTY_WRITE);
    BLECharacteristic* data = service->createCharacteristic(
        kDataUuid, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    gStatus = service->createCharacteristic(
        kStatusUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    control->setCallbacks(new ControlCallbacks());
    data->setCallbacks(new DataCallbacks());
    gStatus->addDescriptor(new BLE2902());
    gStatus->setValue("idle");
    service->start();

    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(kServiceUuid);
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    return true;
}

bool takeDisplayRequest() {
    if (!gDisplayRequested) {
        return false;
    }
    gDisplayRequested = false;
    return true;
}

void reportDisplayResult(bool success) {
    report(success ? "displayed" : "display-failed");
}

} // namespace frame_transfer
