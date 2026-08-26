/**
 * @file t2001_types.h
 * @brief T2001 공통 타입 (SysInfo, Policy, Result, InitConfig, BootDiag)
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace epd::t2001 {

/** @brief 패널 정보 (해상도, framebuffer 주소) */
struct SysInfo {
    uint16_t panel_w = 0;
    uint16_t panel_h = 0;
    uint32_t img_buf0 = 0; ///< framebuffer base
};

/** @brief 영역 좌표 (x, y, w, h) */
struct Rect {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
};

enum class UpdateMode : uint8_t {
    basic = 0,
    fast = 1,
    gc = 2,
};

/** @brief T2001 정책 (timeout, streaming, retry, watchdog) */
struct Policy {
    uint32_t hrdy_timeout_ms = 8000;
    uint32_t cmd_timeout_ms = 8000;

    uint16_t stream_block_h = 16;
    size_t stream_chunk_bytes = 4096;

    uint8_t retry_max = 3;

    void (*feed_wdt)() = nullptr;      ///< (옵션) watchdog 예: ::feedWatchdog
    uint32_t wdt_feed_period_ms = 200; ///< feed_wdt 호출 주기(ms)
};

/** @brief T2001 API 결과 코드 */
enum class Result : uint16_t {
    ok = 0,

    hrdy_timeout,
    spi_io,

    mailbox_init_fail,
    mailbox_bad_signature,
    mailbox_bad_addr,

    invalid_param,
    not_initialized,
    mutex_busy_timeout,
    mutex_create_fail,
};

inline bool ok(Result r) {
    return r == Result::ok;
}

/** @brief init/connect 튜닝: connect 주파수, round, settle */
struct InitConfig {
    uint32_t connect_hz = 4000000;  ///< connect 시 4MHz 고정 권장
    uint16_t max_round = 10;        ///< round 최대 (SPI restart 단위)
    uint16_t settle_ms = 200;       ///< boot settle
    uint16_t round_backoff_ms = 80; ///< round 실패 후 backoff
};

/** @brief boot 진단 (콜드부트 수집용) */
struct BootDiag {
    uint32_t dt_ms = 0;        ///< init 전체 소요(ms)
    uint16_t last_round = 0;   ///< 성공한 round
    uint16_t last_mbx_try = 0; ///< 성공한 mailbox header try
    uint32_t connect_hz = 0;   ///< 실제 connect hz
    Result last_result = Result::ok;
};

} // namespace epd::t2001
