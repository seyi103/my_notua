#include <Arduino.h>

#include "core/ble/frameTransfer.h"
#include "core/diag/log.h"
#include "core/epd/t2001/t2001_service.h"
#include "core/power/boardPower.h"
#include "core/power/watchdog.h"
#include "core/storage/frameStore.h"

namespace {
constexpr const char* TAG = "APP";

class FileSource final : public epd::t2001::render::IDataSource {
public:
    explicit FileSource(File file)
        : _file(file) {
    }

    size_t read(uint8_t* destination, size_t maximum) override {
        return _file.read(destination, maximum);
    }

    void rewind() override {
        _file.seek(sizeof(frame_store::Metadata));
    }

    bool valid() const {
        return static_cast<bool>(_file);
    }

private:
    File _file;
};

bool displayStoredFrame() {
    frame_store::Metadata metadata {};
    if (!frame_store::metadata(metadata)) {
        logError(TAG, "frame metadata unavailable");
        return false;
    }
    FileSource source(frame_store::openFrame());
    if (!source.valid()) {
        logError(TAG, "frame file unavailable");
        return false;
    }

    const auto result = epd::t2001::svc::display_8bpp_from_source(source, metadata.bytes);
    const bool success = epd::t2001::ok(result.low);
    logInfo(TAG, "display result=%d step=%d", static_cast<int>(result.low),
        static_cast<int>(result.step));

    // An EPD retains its image without power; tear down every panel rail after refresh.
    if (epd::t2001::svc::is_ready()) {
        (void) epd::t2001::epdPowerOn(false);
        epd::t2001::svc::service_deinit();
    }
    boardPowerT2001Off();
    return success;
}
} // namespace

void setup() {
    initLog(115200, LOG_LEVEL_INFO);
    boardPowerT2001Off();

    if (!beginWatchdog()) {
        logError(TAG, "watchdog initialization failed");
    }
    if (!frame_store::begin()) {
        logError(TAG, "LittleFS mount failed");
        return;
    }
    if (!frame_transfer::begin()) {
        logError(TAG, "BLE initialization failed");
        return;
    }
    logInfo(TAG, "offline BLE frame receiver ready");
}

void loop() {
    feedWatchdog();
    if (frame_transfer::takeDisplayRequest()) {
        frame_transfer::reportDisplayResult(displayStoredFrame());
    }
    delay(20);
}
