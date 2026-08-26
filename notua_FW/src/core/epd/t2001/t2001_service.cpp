// src/epd/t2001/t2001_service.cpp
#include "t2001_service.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "core/diag/log.h"
#include "core/power/boardPower.h"
#include "core/power/watchdog.h"
#include "core/runtime/longOpPump.h"
#include "app/policy/policyParams.h"
#include "epd/t2001/t2001.h" // epd::t2001::init/deinit/getSysInfo
#include "epd/t2001/t2001_render.h"

namespace epd::t2001::svc {

static constexpr const char* TAG = "T2001_SVC";
static constexpr uint32_t EPD_MUTEX_TIMEOUT_MS = EPD_DPY_WAIT_TIMEOUT_MS;

static State gState = State::UNINIT;
static Stats gStats {};
static SemaphoreHandle_t gEpdMutex = nullptr;
static TaskHandle_t gEpdOwner = nullptr;

static Result ensure_display_mutex_created() {
    static StaticSemaphore_t mutexBuffer;
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&mutexBuffer);
    if (!mutex) {
        return Result::mutex_create_fail;
    }
    gEpdMutex = mutex;
    return Result::ok;
}

static Result take_display_mutex(const char* op) {
    configASSERT(gEpdMutex != nullptr);
    if (!gEpdMutex) {
        return Result::mutex_create_fail;
    }

    if (xSemaphoreTake(gEpdMutex, pdMS_TO_TICKS(EPD_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        TaskHandle_t owner = gEpdOwner;
        const char* ownerName = owner ? pcTaskGetName(owner) : "none";
        logWarn(TAG, "%s mutex timeout owner=%s timeout=%ums",
            op ? op : "display", ownerName ? ownerName : "unknown",
            (unsigned) EPD_MUTEX_TIMEOUT_MS);
        return Result::mutex_busy_timeout;
    }

    gEpdOwner = xTaskGetCurrentTaskHandle();
    return Result::ok;
}

static void give_display_mutex() {
    gEpdOwner = nullptr;
    xSemaphoreGive(gEpdMutex);
}

static inline void bootstat_ok(uint32_t boot_seq, uint8_t svc_attempt, uint32_t ms_total) {
    logInfo("BOOTSTAT", "OK,boot=%lu,svc=%u,ms=%lu", (unsigned long) boot_seq,
        (unsigned) svc_attempt, (unsigned long) ms_total);
}

static inline void bootstat_fail(uint32_t boot_seq, uint8_t svc_attempt, uint32_t ms_total, Result err) {
    logError("BOOTSTAT", "FAIL,boot=%lu,svc=%u,ms=%lu,err=%d",
        (unsigned long) boot_seq, (unsigned) svc_attempt,
        (unsigned long) ms_total, (int) err);
}

static Result service_init_impl(const Policy& policy, const Config& cfg, bool caller_holds_mutex) {
    gState = State::INITING;

    gStats.boot_seq++;
    gStats.last_init_ms = 0;
    gStats.last_service_attempt = 0;
    gStats.last_err = 0;

    const uint32_t t0 = millis();
    const uint8_t maxA =
        (cfg.max_service_attempts == 0) ? 1 : cfg.max_service_attempts;

    Result r = ensure_display_mutex_created();
    if (!epd::t2001::ok(r)) {
        gStats.last_err = (uint32_t) r;
        gStats.last_init_ms = millis() - t0;
        gStats.fail_count++;
        gState = State::FAILED;
        logError(TAG, "display mutex create failed r=%d", (int) r);
        return r;
    }

    for (uint8_t a = 1; a <= maxA; a++) {
        gStats.last_service_attempt = a;

        if (a > 1) {
            if (cfg.deinit_before_retry) {
                r = take_display_mutex("service retry deinit");
                if (!epd::t2001::ok(r)) {
                    gStats.last_err = (uint32_t) r;
                    logWarn(TAG, "deinit before retry skipped attempt=%u r=%d",
                        (unsigned) a, (int) r);
                    continue;
                }
                epd::t2001::deinit();
                give_display_mutex();
            }
            if (cfg.service_backoff_ms > 0) {
                delay(cfg.service_backoff_ms);
            }
        }

        if (!caller_holds_mutex) {
            r = take_display_mutex("service init");
            if (!epd::t2001::ok(r)) {
                gStats.last_err = (uint32_t) r;
                logWarn(TAG, "init mutex failed attempt=%u/%u r=%d", (unsigned) a,
                    (unsigned) maxA, (int) r);
                continue;
            }
        }

        r = epd::t2001::init(policy);
        if (!epd::t2001::ok(r)) {
            if (!caller_holds_mutex) {
                give_display_mutex();
            }
            gStats.last_err = (uint32_t) r;
            logWarn(TAG, "init attempt=%u/%u failed r=%d", (unsigned) a,
                (unsigned) maxA, (int) r);
            continue;
        }

        SysInfo sys {};
        r = epd::t2001::getSysInfo(sys);
        if (!epd::t2001::ok(r)) {
            if (!caller_holds_mutex) {
                give_display_mutex();
            }
            gStats.last_err = (uint32_t) r;
            logWarn(TAG, "getSysInfo failed after init attempt=%u r=%d", (unsigned) a,
                (int) r);
            continue;
        }

        gStats.last_sys = sys;
        gStats.last_init_ms = millis() - t0;
        gStats.ok_count++;
        gState = State::READY;
        if (!caller_holds_mutex) {
            give_display_mutex();
        }

        if (cfg.log_bootstat) {
            bootstat_ok(gStats.boot_seq, a, gStats.last_init_ms);
        }

        logInfo(TAG, "READY panel=%ux%u imgBuf0=0x%08lX (svc=%u, %lums)",
            (unsigned) sys.panel_w, (unsigned) sys.panel_h,
            (unsigned long) sys.img_buf0, (unsigned) a,
            (unsigned long) gStats.last_init_ms);

        return Result::ok;
    }

    gStats.last_init_ms = millis() - t0;
    gStats.fail_count++;
    gState = State::FAILED;

    Result err = (Result) gStats.last_err;
    if (cfg.log_bootstat) {
        bootstat_fail(gStats.boot_seq, gStats.last_service_attempt,
            gStats.last_init_ms, err);
    }

    logError(TAG, "FAILED after %u attempts (%lums), last_err=%d",
        (unsigned) gStats.last_service_attempt,
        (unsigned long) gStats.last_init_ms, (int) err);

    return err;
}

Result service_init(const Policy& policy, const Config& cfg) {
    return service_init_impl(policy, cfg, false);
}

bool is_ready() {
    return gState == State::READY;
}

static Policy defaultServicePolicy() {
    Policy p {};
    p.hrdy_timeout_ms = 8000;
    p.feed_wdt = feedWatchdog;
    p.wdt_feed_period_ms = 200;
    return p;
}

static Config defaultServiceConfig() {
    Config c {};
    c.max_service_attempts = 3;
    c.service_backoff_ms = 300;
    c.log_bootstat = true;
    return c;
}

Result ensure_ready() {
    Result rInit = ensure_display_mutex_created();
    if (!epd::t2001::ok(rInit)) {
        return rInit;
    }

    Result mr = take_display_mutex("ensure_ready");
    if (!epd::t2001::ok(mr)) {
        return mr;
    }

    if (is_ready()) {
        give_display_mutex();
        return Result::ok;
    }

    if (!boardPowerT2001OnForSpi()) {
        logError(TAG, "ensure_ready: boardPowerT2001OnForSpi failed");
        give_display_mutex();
        return Result::spi_io;
    }

    Config cfg = defaultServiceConfig();
    cfg.max_service_attempts = 1;
    const Result r = service_init_impl(defaultServicePolicy(), cfg, true);
    if (!epd::t2001::ok(r)) {
        boardPowerT2001Off();
    }
    give_display_mutex();
    return r;
}

State get_state() {
    return gState;
}
const Stats& get_stats() {
    return gStats;
}

Result get_sys(SysInfo& out) {
    if (gState != State::READY)
        return Result::not_initialized;
    out = gStats.last_sys;
    return Result::ok;
}

void service_deinit(bool clear_stats) {
    if (gEpdMutex) {
        Result r = take_display_mutex("service deinit");
        if (!epd::t2001::ok(r)) {
            logWarn(TAG, "service_deinit skipped r=%d", (int) r);
            return;
        }
    }

    epd::t2001::deinit();
    gState = State::UNINIT;

    if (clear_stats) {
        Stats z {};
        gStats = z;
    }

    if (gEpdMutex) {
        give_display_mutex();
    }
}

/** @brief Retry backoff with WDT/MQTT pump (same pattern as settle / DPY wait). */
static void backoffMsPump(uint16_t ms) {
    if (ms == 0)
        return;
    const uint32_t endMs = millis() + ms;
    while ((int32_t) (millis() - endMs) < 0) {
        longOpPump();
        delay(50);
    }
}

/**
 * @brief display 재시도 전 전원 복구.
 * @details HRDY stuck / LD_IMG 실패는 6V5만으로는 복구 불가 → T2001 풀 레일 OFF→RST→service_init.
 *          그 외(패널 전원 단계)는 6V5만 내림. caller가 display mutex를 이미 보유해야 함.
 * @note do_power=false(회로 테스트)면 전원 조작 없이 ok 반환.
 */
static Result recoverPowerForRetry(Result failReason, ErrStep failStep, SysInfo& sys, bool do_power) {
    if (!do_power || !EPD_PWR_CYCLE_ENABLE)
        return Result::ok;

    boardPowerFinishEpdUpdate();

    const bool hardReset = (failReason == Result::hrdy_timeout) || (failStep == ErrStep::render);
    if (!hardReset) {
        logWarn(TAG, "EPD 6V5 power cycle before retry");
        return Result::ok;
    }

    logWarn(TAG, "EPD hard rail reset before retry (r=%d step=%d)", (int) failReason,
        (int) failStep);
    /* mailbox off needs HRDY — skip; rail cut recovers stuck T-con */
    epd::t2001::deinit();
    gState = State::UNINIT;
    boardPowerT2001Off();
    backoffMsPump(100);

    if (!boardPowerT2001OnForSpi()) {
        logError(TAG, "hard reset: boardPowerT2001OnForSpi failed");
        return Result::spi_io;
    }

    Config cfg = defaultServiceConfig();
    cfg.max_service_attempts = 1;
    Result r = service_init_impl(defaultServicePolicy(), cfg, true);
    if (!epd::t2001::ok(r)) {
        logError(TAG, "hard reset: service_init failed r=%d", (int) r);
        boardPowerT2001Off();
        return r;
    }

    r = get_sys(sys);
    if (!epd::t2001::ok(r)) {
        logError(TAG, "hard reset: get_sys failed r=%d", (int) r);
        return r;
    }
    return Result::ok;
}

static inline DisplayResult dr(Result low, ErrStep step) {
    DisplayResult r {};
    r.low = low;
    r.step = step;
    return r;
}

DisplayResult display_8bpp_from_source(render::IDataSource& src,
    size_t total_len_bytes,
    const DisplayConfig& dc) {
    /* EPD refresh: LD_IMG → temp → 6V5 (once) → 0x0038 ON → VSX → DPY → DARCR; 6V5/0x0038 off at sleep only */
    Result rInit = ensure_display_mutex_created();
    if (!epd::t2001::ok(rInit)) {
        return dr(rInit, ErrStep::ensure_ready);
    }

    if (!is_ready()) {
        Result er = ensure_ready();
        if (!epd::t2001::ok(er)) {
            return dr(er, ErrStep::ensure_ready);
        }
    }

    Result mr = take_display_mutex("display");
    if (!epd::t2001::ok(mr)) {
        return dr(mr, ErrStep::mutex_acquire);
    }

    DisplayResult out = dr(Result::invalid_param, ErrStep::cleanup);

    SysInfo sys {};
    Result r = get_sys(sys);
    if (!epd::t2001::ok(r)) {
        give_display_mutex();
        return dr(r, ErrStep::ensure_ready);
    }

    const uint32_t expected = (uint32_t) sys.panel_w * (uint32_t) sys.panel_h;
    if ((uint32_t) total_len_bytes != expected) {
        give_display_mutex();
        return dr(Result::invalid_param, ErrStep::render);
    }

    auto& tr = epd::t2001::transport();
    render::Config rcfg {};
    rcfg.usArg = 0x0030;
    rcfg.block_h = 16;
    rcfg.ready_timeout_us = 8000000;
    rcfg.io_chunk_bytes = 4096;
    render::Hooks hk {};
    hk.feed_wdt = longOpPump;
    hk.wdt_period_ms = 200;
    const uint8_t maxA = (dc.max_attempts == 0) ? 1 : dc.max_attempts;

    for (uint8_t a = 1; a <= maxA; a++) {
        if (a > 1) {
            logWarn(TAG, "display retry a=%u/%u", (unsigned) a, (unsigned) maxA);
        }
        src.rewind();
        /* 1) LD_IMG — boot SPI rails only (before 6V5 / 0x0038) */
        r = render::send_8bpp_full_from_source(tr, sys.panel_w, sys.panel_h, src,
            total_len_bytes, rcfg, hk);
        if (!epd::t2001::ok(r)) {
            logWarn(TAG, "LD_IMG fail a=%u/%u r=%d", (unsigned) a, (unsigned) maxA, (int) r);
            if (a == maxA) {
                out = dr(r, ErrStep::render);
                break;
            }
            Result rr = recoverPowerForRetry(r, ErrStep::render, sys, dc.do_power);
            if (!epd::t2001::ok(rr)) {
                out = dr(rr, ErrStep::ensure_ready);
                break;
            }
            backoffMsPump(dc.backoff_ms);
            continue;
        }

        /* 2) Temperature (reference: after LD_IMG, before panel power) */
        {
            int16_t realT = 25, setT = 25;
            if (epd::t2001::ok(epd::t2001::getTemperature(realT, setT))) {
                int16_t useT = realT;
                if (useT < 0)
                    useT = 0;
                if (useT > 50)
                    useT = 50;
                (void) epd::t2001::forceSetTemperature(useT);
            } else {
                (void) epd::t2001::forceSetTemperature(20);
            }
        }

        if (dc.do_power) {
            /* IO41 6V5 must be ON before mailbox 0x0038 can reach the panel */
            if (!boardPowerPrepareEpdUpdate()) {
                logWarn(TAG, "PrepareEpdUpdate fail a=%u/%u", (unsigned) a, (unsigned) maxA);
                if (a == maxA) {
                    out = dr(Result::spi_io, ErrStep::pwr_on);
                    break;
                }
                Result rr = recoverPowerForRetry(Result::spi_io, ErrStep::pwr_on, sys, dc.do_power);
                if (!epd::t2001::ok(rr)) {
                    out = dr(rr, ErrStep::ensure_ready);
                    break;
                }
                backoffMsPump(dc.backoff_ms);
                continue;
            }

            /* 0x0038 ON only — T2001 manages EPD DCDC; off at sleep teardown only */
            r = epd::t2001::epdPowerOn(true);
            if (!epd::t2001::ok(r)) {
                logWarn(TAG, "epdPowerOn fail a=%u/%u r=%d", (unsigned) a, (unsigned) maxA,
                    (int) r);
                if (a == maxA) {
                    out = dr(r, ErrStep::pwr_on);
                    break;
                }
                Result rr = recoverPowerForRetry(r, ErrStep::pwr_on, sys, dc.do_power);
                if (!epd::t2001::ok(rr)) {
                    out = dr(rr, ErrStep::ensure_ready);
                    break;
                }
                backoffMsPump(dc.backoff_ms);
                continue;
            }

            /* reference: settle after 0x0038 before VSX/DPY */
            {
                const uint32_t settleEnd = millis() + 500;
                while ((int32_t) (millis() - settleEnd) < 0) {
                    longOpPump();
                    delay(50);
                }
            }

            r = epd::t2001::applyReferencePanelVoltage();
            if (!epd::t2001::ok(r)) {
                logWarn(TAG, "VSX fail a=%u/%u r=%d", (unsigned) a, (unsigned) maxA, (int) r);
                if (a == maxA) {
                    out = dr(r, ErrStep::pwr_on);
                    break;
                }
                Result rr = recoverPowerForRetry(r, ErrStep::pwr_on, sys, dc.do_power);
                if (!epd::t2001::ok(rr)) {
                    out = dr(rr, ErrStep::ensure_ready);
                    break;
                }
                backoffMsPump(dc.backoff_ms);
                continue;
            }
        }

        /* 5) DisplayArea + wait DARCR0 busy (6V5 stays ON until refresh done) */
        r = epd::t2001::dpyArea(sys.img_buf0, dc.waveform, 0, 0, sys.panel_w,
            sys.panel_h);
        if (!epd::t2001::ok(r)) {
            logWarn(TAG, "dpyArea fail a=%u/%u r=%d", (unsigned) a, (unsigned) maxA, (int) r);
            if (a == maxA) {
                out = dr(r, ErrStep::trigger);
                break;
            }
            Result rr = recoverPowerForRetry(r, ErrStep::trigger, sys, dc.do_power);
            if (!epd::t2001::ok(rr)) {
                out = dr(rr, ErrStep::ensure_ready);
                break;
            }
            backoffMsPump(dc.backoff_ms);
            continue;
        }

        r = epd::t2001::waitDpyAreaDone(dc.display_wait_timeout_ms);
        if (!epd::t2001::ok(r)) {
            logWarn(TAG, "waitDpy fail a=%u/%u r=%d", (unsigned) a, (unsigned) maxA, (int) r);
            if (a == maxA) {
                out = dr(r, ErrStep::wait_done);
                break;
            }
            Result rr = recoverPowerForRetry(r, ErrStep::wait_done, sys, dc.do_power);
            if (!epd::t2001::ok(rr)) {
                out = dr(rr, ErrStep::ensure_ready);
                break;
            }
            backoffMsPump(dc.backoff_ms);
            continue;
        }

        if (a > 1) {
            logInfo(TAG, "display ok after retry a=%u", (unsigned) a);
        }
        out = dr(Result::ok, ErrStep::none);
        break;
    }

    give_display_mutex();
    return out;
}

} // namespace epd::t2001::svc
