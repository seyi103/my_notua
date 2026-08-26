#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_sleep.h>

#include <algorithm>
#include <vector>

#include "core/diag/log.h"
#include "core/epd/t2001/t2001_service.h"
#include "core/power/boardPower.h"
#include "core/power/watchdog.h"

namespace {
constexpr const char* TAG = "PHOTO_CYCLE";
constexpr size_t IMAGE_WIDTH = 1600;
constexpr size_t IMAGE_HEIGHT = 1200;
constexpr size_t IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT;
constexpr uint64_t SLEEP_INTERVAL_US = 5ULL * 60ULL * 1000000ULL;
constexpr size_t MAX_IMAGES = 3;

bool isBinFile(const String& path) {
    String lower = path;
    lower.toLowerCase();
    return lower.endsWith(".bin");
}

std::vector<String> findValidImages() {
    std::vector<String> images;
    File directory = LittleFS.open("/images");
    if (!directory || !directory.isDirectory()) {
        logError(TAG, "missing LittleFS directory /images");
        return images;
    }

    for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
        const String path = file.path();
        if (file.isDirectory() || !isBinFile(path)) {
            continue;
        }
        if (file.size() != IMAGE_BYTES) {
            logError(TAG, "skip invalid image %s: size=%u expected=%u", path.c_str(),
                static_cast<unsigned>(file.size()), static_cast<unsigned>(IMAGE_BYTES));
            continue;
        }
        if (images.size() == MAX_IMAGES) {
            logWarn(TAG, "skip extra image %s (maximum %u)", path.c_str(),
                static_cast<unsigned>(MAX_IMAGES));
            continue;
        }
        images.push_back(path);
    }
    std::sort(images.begin(), images.end());
    return images;
}

uint8_t* loadImage(const String& path) {
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.size() != IMAGE_BYTES) {
        logError(TAG, "image changed or cannot be opened: %s", path.c_str());
        return nullptr;
    }

    uint8_t* image = static_cast<uint8_t*>(ps_malloc(IMAGE_BYTES));
    if (!image) {
        logError(TAG, "PSRAM allocation failed for %u bytes", static_cast<unsigned>(IMAGE_BYTES));
        return nullptr;
    }

    size_t loaded = 0;
    while (loaded < IMAGE_BYTES) {
        const size_t count = file.read(image + loaded, IMAGE_BYTES - loaded);
        if (count == 0) {
            break;
        }
        loaded += count;
        feedWatchdog();
        yield();
    }
    file.close();
    if (loaded != IMAGE_BYTES) {
        logError(TAG, "short read %s: read=%u expected=%u", path.c_str(),
            static_cast<unsigned>(loaded), static_cast<unsigned>(IMAGE_BYTES));
        free(image);
        return nullptr;
    }
    return image;
}

void powerDownPanel() {
    if (epd::t2001::svc::is_ready()) {
        const auto result = epd::t2001::epdPowerOn(false);
        if (!epd::t2001::ok(result)) {
            logWarn(TAG, "EPD power-off command failed: result=%d", static_cast<int>(result));
        }
        epd::t2001::svc::service_deinit();
    }
    boardPowerT2001Off();
}

void sleepUntilNextImage() {
    powerDownPanel();
    LittleFS.end();
    logInfo(TAG, "deep sleep for %llu seconds", SLEEP_INTERVAL_US / 1000000ULL);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US);
    esp_deep_sleep_start();
}
} // namespace

void setup() {
    initLog(115200, LOG_LEVEL_INFO);
    boardPowerT2001Off();

    if (!beginWatchdog()) {
        logError(TAG, "watchdog initialization failed");
    }
    if (!psramFound()) {
        logError(TAG, "PSRAM unavailable");
        sleepUntilNextImage();
        return;
    }
    if (!LittleFS.begin(false)) {
        logError(TAG, "LittleFS mount failed (automatic formatting disabled)");
        sleepUntilNextImage();
        return;
    }

    const std::vector<String> images = findValidImages();
    if (images.empty()) {
        logError(TAG, "no valid %ux%u Y8 BIN images found", static_cast<unsigned>(IMAGE_WIDTH),
            static_cast<unsigned>(IMAGE_HEIGHT));
        sleepUntilNextImage();
        return;
    }

    Preferences preferences;
    if (!preferences.begin("photo-cycle", false)) {
        logError(TAG, "cannot open persistent image index");
        sleepUntilNextImage();
        return;
    }
    const size_t index = preferences.getUInt("next", 0) % images.size();
    logInfo(TAG, "loading image %u/%u: %s", static_cast<unsigned>(index + 1),
        static_cast<unsigned>(images.size()), images[index].c_str());

    uint8_t* image = loadImage(images[index]);
    if (image) {
        epd::t2001::render::MemorySource source(image, IMAGE_BYTES);
        const auto result = epd::t2001::svc::display_8bpp_from_source(source, IMAGE_BYTES);
        free(image);
        if (epd::t2001::ok(result.low)) {
            const size_t next = (index + 1) % images.size();
            if (preferences.putUInt("next", next) == 0) {
                logError(TAG, "display succeeded but next image index was not persisted");
            } else {
                logInfo(TAG, "display complete; next image index=%u", static_cast<unsigned>(next));
            }
        } else {
            logError(TAG, "display failed: result=%d step=%d; index retained",
                static_cast<int>(result.low), static_cast<int>(result.step));
        }
    }
    preferences.end();
    sleepUntilNextImage();
}

void loop() {
    // setup() always enters deep sleep.
}
