#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
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

#ifndef NOTUA_ALLOW_DEEP_SLEEP
#define NOTUA_ALLOW_DEEP_SLEEP 0
#endif

static_assert(NOTUA_ALLOW_DEEP_SLEEP == 0 || NOTUA_ALLOW_DEEP_SLEEP == 1,
    "NOTUA_ALLOW_DEEP_SLEEP must be 0 or 1");

enum class RunResult {
    success,
    psram_unavailable,
    filesystem_mount_failed,
    no_valid_images,
    preferences_open_failed,
    image_load_failed,
    display_failed,
    index_persist_failed,
};

bool gTerminal = false;

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

    uint8_t* image = static_cast<uint8_t*>(
        heap_caps_malloc(IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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

void finishRun(RunResult result) {
    powerDownPanel();
    LittleFS.end();

#if NOTUA_ALLOW_DEEP_SLEEP
    if (result == RunResult::success) {
        logInfo(TAG, "release display succeeded; deep sleep for %llu seconds",
            SLEEP_INTERVAL_US / 1000000ULL);
        Serial.flush();
        esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US);
        esp_deep_sleep_start();
    }
    logError(TAG, "release run failed (reason=%d); staying awake for diagnostics",
        static_cast<int>(result));
#else
    logInfo(TAG, "development run complete (reason=%d); deep sleep disabled",
        static_cast<int>(result));
#endif
    Serial.flush();
    gTerminal = true;
}
} // namespace

void setup() {
    initLog(115200, LOG_LEVEL_INFO);
    waitForLogHost(1500);
    logInfo(TAG, "boot: mode=%s wake_cause=%d",
        NOTUA_ALLOW_DEEP_SLEEP ? "release" : "development",
        static_cast<int>(esp_sleep_get_wakeup_cause()));
    boardPowerT2001Off();

    if (!beginWatchdog()) {
        logError(TAG, "watchdog initialization failed");
    }
    if (!psramFound()) {
        logError(TAG, "PSRAM unavailable");
        finishRun(RunResult::psram_unavailable);
        return;
    }
    logInfo(TAG, "PSRAM ready: total=%u free=%u largest=%u",
        static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    if (!LittleFS.begin(false)) {
        logError(TAG, "LittleFS mount failed (automatic formatting disabled)");
        finishRun(RunResult::filesystem_mount_failed);
        return;
    }
    logInfo(TAG, "LittleFS mounted: total=%u used=%u", static_cast<unsigned>(LittleFS.totalBytes()),
        static_cast<unsigned>(LittleFS.usedBytes()));

    const std::vector<String> images = findValidImages();
    if (images.empty()) {
        logError(TAG, "no valid %ux%u Y8 BIN images found", static_cast<unsigned>(IMAGE_WIDTH),
            static_cast<unsigned>(IMAGE_HEIGHT));
        finishRun(RunResult::no_valid_images);
        return;
    }

    Preferences preferences;
    if (!preferences.begin("photo-cycle", false)) {
        logError(TAG, "cannot open persistent image index");
        finishRun(RunResult::preferences_open_failed);
        return;
    }
    const size_t index = preferences.getUInt("next", 0) % images.size();
    logInfo(TAG, "loading image %u/%u: %s", static_cast<unsigned>(index + 1),
        static_cast<unsigned>(images.size()), images[index].c_str());

    uint8_t* image = loadImage(images[index]);
    if (!image) {
        preferences.end();
        finishRun(RunResult::image_load_failed);
        return;
    }

    epd::t2001::render::MemorySource source(image, IMAGE_BYTES);
    const auto result = epd::t2001::svc::display_8bpp_from_source(source, IMAGE_BYTES);
    free(image);
    if (!epd::t2001::ok(result.low)) {
        logError(TAG, "display failed: result=%d step=%d; index retained",
            static_cast<int>(result.low), static_cast<int>(result.step));
        preferences.end();
        finishRun(RunResult::display_failed);
        return;
    }

    const size_t next = (index + 1) % images.size();
    if (preferences.putUInt("next", next) == 0) {
        logError(TAG, "display succeeded but next image index was not persisted");
        preferences.end();
        finishRun(RunResult::index_persist_failed);
        return;
    }
    logInfo(TAG, "display complete; next image index=%u", static_cast<unsigned>(next));
    preferences.end();
    finishRun(RunResult::success);
}

void loop() {
    // A development build and every release failure intentionally remain here.
    if (gTerminal) {
        feedWatchdog();
        Serial.flush();
        delay(250);
    }
}
