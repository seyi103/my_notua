#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

#include <algorithm>
#include <vector>

#include "core/diag/log.h"
#include "core/ble/blePeripheral.h"
#include "core/board/pins.h"
#include "core/epd/t2001/t2001_service.h"
#include "core/power/boardPower.h"
#include "core/power/watchdog.h"
#include "core/storage/imageCatalog.h"

namespace {
constexpr const char* TAG = "PHOTO_CYCLE";
constexpr size_t IMAGE_WIDTH = 1600;
constexpr size_t IMAGE_HEIGHT = 1200;
constexpr size_t IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT;
constexpr uint64_t SLEEP_INTERVAL_US = 5ULL * 60ULL * 1000000ULL;
constexpr uint64_t ERROR_RETRY_INTERVAL_US = 1ULL * 60ULL * 1000000ULL;
constexpr uint32_t RELEASE_LOG_FLUSH_MS = 1000;
constexpr uint64_t SW_WAKE_MASK = 1ULL << SW_PIN;
constexpr uint32_t BUTTON_RELEASE_TIMEOUT_MS = 10UL * 1000UL;

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

enum class WakePath {
    timerPhotoCycle,
    buttonBle,
    existingPolicy,
};

RunResult gLastRunResult = RunResult::success;
bool gTerminal = false;
uint32_t gLastTerminalLogMs = 0;
WakePath gWakePath = WakePath::existingPolicy;

const char* wakePathName(WakePath path) {
    switch (path) {
    case WakePath::timerPhotoCycle: return "timer_photo_cycle";
    case WakePath::buttonBle: return "button_ble";
    case WakePath::existingPolicy: return "existing_policy";
    }
    return "unknown";
}

void configureButtonWake() {
    pinMode(SW_PIN, INPUT);
    rtc_gpio_pullup_dis(static_cast<gpio_num_t>(SW_PIN));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(SW_PIN));
    const esp_err_t result = esp_sleep_enable_ext1_wakeup(
        SW_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);
    logInfo(TAG, "EXT1 configured: gpio=%u mask=0x%llx any_high=true result=%d",
        SW_PIN, SW_WAKE_MASK, static_cast<int>(result));
}

void enterDeepSleep(uint64_t timerUs, const char* reason) {
    configureButtonWake();
    const uint32_t started = millis();
    while (digitalRead(SW_PIN) == HIGH && millis() - started < BUTTON_RELEASE_TIMEOUT_MS) {
        feedWatchdog();
        delay(25);
    }
    if (digitalRead(SW_PIN) == HIGH) {
        logError(TAG, "GPIO16 remained HIGH for %u ms; disabling EXT1 for this sleep",
            static_cast<unsigned>(BUTTON_RELEASE_TIMEOUT_MS));
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    }
    if (timerUs != 0) {
        esp_sleep_enable_timer_wakeup(timerUs);
    }
    logInfo(TAG, "deep sleep: reason=%s timer_s=%llu", reason, timerUs / 1000000ULL);
    Serial.flush();
    esp_deep_sleep_start();
}

const char* runResultName(RunResult result) {
    switch (result) {
    case RunResult::success:
        return "success";
    case RunResult::psram_unavailable:
        return "psram_unavailable";
    case RunResult::filesystem_mount_failed:
        return "filesystem_mount_failed";
    case RunResult::no_valid_images:
        return "no_valid_images";
    case RunResult::preferences_open_failed:
        return "preferences_open_failed";
    case RunResult::image_load_failed:
        return "image_load_failed";
    case RunResult::display_failed:
        return "display_failed";
    case RunResult::index_persist_failed:
        return "index_persist_failed";
    }
    return "unknown";
}

std::vector<String> findValidImages() {
    bool tooMany = false;
    auto images = notua::storage::collectImageCatalog(LittleFS, &tooMany);
    if (tooMany) logWarn(TAG, "more than three valid images found; using first three sorted paths");
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
    gLastRunResult = result;
    powerDownPanel();
    LittleFS.end();

#if NOTUA_ALLOW_DEEP_SLEEP
    const uint64_t sleepIntervalUs = result == RunResult::success
        ? SLEEP_INTERVAL_US
        : ERROR_RETRY_INTERVAL_US;
    if (result == RunResult::success) {
        logInfo(TAG, "release complete: result=%s; deep sleep for %llu seconds",
            runResultName(result), sleepIntervalUs / 1000000ULL);
    } else {
        logError(TAG, "release failed: result=%s; retry deep sleep in %llu seconds",
            runResultName(result), sleepIntervalUs / 1000000ULL);
    }
    Serial.flush();
    const uint32_t flushStarted = millis();
    while ((millis() - flushStarted) < RELEASE_LOG_FLUSH_MS) {
        feedWatchdog();
        delay(25);
    }
    Serial.flush();
    enterDeepSleep(sleepIntervalUs, runResultName(result));
#else
    logInfo(TAG, "development run complete: result=%s; deep sleep disabled",
        runResultName(result));
#endif
    Serial.flush();
    gTerminal = true;
    gLastTerminalLogMs = millis() - 1000;
}
} // namespace

void setup() {
    initLog(115200, LOG_LEVEL_INFO);
    waitForLogHost(1500);
    const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
        gWakePath = WakePath::timerPhotoCycle;
    } else if (wakeCause == ESP_SLEEP_WAKEUP_EXT1
        && (esp_sleep_get_ext1_wakeup_status() & SW_WAKE_MASK) != 0) {
        gWakePath = WakePath::buttonBle;
    }
    const uint64_t ext1Mask = wakeCause == ESP_SLEEP_WAKEUP_EXT1
        ? esp_sleep_get_ext1_wakeup_status() : 0;
    logInfo(TAG, "boot: mode=%s wake_cause=%d wake_path=%s ext1_gpio_mask=0x%llx",
        NOTUA_ALLOW_DEEP_SLEEP ? "release" : "development",
        static_cast<int>(wakeCause), wakePathName(gWakePath), ext1Mask);
    boardPowerT2001Off();

    if (!beginWatchdog()) {
        logError(TAG, "watchdog initialization failed");
    }
    if (gWakePath == WakePath::buttonBle) {
        const bool initialized = beginBlePeripheral();
        logInfo(TAG, "button BLE path: initialization=%s state=%s",
            initialized ? "success" : "failed", bleStateName(bleState()));
        if (!initialized) {
#if NOTUA_ALLOW_DEEP_SLEEP
            enterDeepSleep(ERROR_RETRY_INTERVAL_US, "ble_initialization_failed");
#else
            gTerminal = true;
#endif
        }
        return;
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
    const String pending = preferences.getString("pending", "");
    size_t index = preferences.getUInt("next", 0) % images.size();
    bool displayingPending = false;
    if (pending.length()) {
        const auto found = std::find(images.begin(), images.end(), pending);
        if (found != images.end()) {
            index = static_cast<size_t>(found - images.begin());
            displayingPending = true;
            logInfo(TAG, "pending image takes priority: %s", pending.c_str());
        } else {
            logError(TAG, "pending image is unavailable or invalid; retaining pending value");
        }
    }
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
    if (displayingPending && !preferences.remove("pending")) {
        logError(TAG, "display succeeded but pending image marker was not cleared");
        preferences.end();
        finishRun(RunResult::index_persist_failed);
        return;
    }
    logInfo(TAG, "display complete; next image index=%u", static_cast<unsigned>(next));
    preferences.end();
    finishRun(RunResult::success);
}

void loop() {
    if (gWakePath == WakePath::buttonBle) {
        feedWatchdog();
        pollBlePeripheral();
        if (consumeBleApplyRequest()) {
            logInfo(TAG, "APPLY accepted; allowing notification delivery before restart");
            delay(350);
            stopBlePeripheral();
            Serial.flush();
            delay(50);
            ESP.restart();
        }
        const BleState currentBleState = bleState();
        if (currentBleState == BleState::initializationFailed && !gTerminal) {
            stopBlePeripheral();
#if NOTUA_ALLOW_DEEP_SLEEP
            enterDeepSleep(ERROR_RETRY_INTERVAL_US, "ble_runtime_error");
#else
            logError(TAG, "development policy: BLE runtime error; deep sleep disabled");
            gTerminal = true;
#endif
        } else if (bleSessionExpired()) {
            logInfo(TAG, "BLE inactivity timeout: state=%s", bleStateName(currentBleState));
            stopBlePeripheral();
#if NOTUA_ALLOW_DEEP_SLEEP
            enterDeepSleep(SLEEP_INTERVAL_US, "ble_timeout");
#else
            logInfo(TAG, "development policy: deep sleep disabled after BLE timeout");
            gTerminal = true;
#endif
        }
        if (gTerminal) {
            const uint32_t now = millis();
            if ((now - gLastTerminalLogMs) >= 1000) {
                gLastTerminalLogMs = now;
                logInfo(TAG, "BLE terminal: state=%s uptime_ms=%lu",
                    bleStateName(bleState()), static_cast<unsigned long>(now));
                Serial.flush();
            }
        }
        delay(25);
        return;
    }
    // Development builds remain observable; this is only a fallback if deep sleep returns in release.
    if (gTerminal) {
        feedWatchdog();
        const uint32_t now = millis();
        if ((now - gLastTerminalLogMs) >= 1000) {
            gLastTerminalLogMs = now;
            logInfo(TAG, "terminal: result=%s uptime_ms=%lu", runResultName(gLastRunResult),
                static_cast<unsigned long>(now));
            Serial.flush();
        }
        delay(25);
    }
}
