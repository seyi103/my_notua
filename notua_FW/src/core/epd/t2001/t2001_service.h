/**
 * @file t2001_service.h
 * @brief T2001 EPD 서비스 레이어 (초기화, 렌더링, 상태 관리)
 */
#pragma once

#include <stdint.h>
#include "core/epd/t2001/t2001.h" // epd::t2001::{Result, Policy, SysInfo, init, deinit, getSysInfo, ok}
#include "core/epd/t2001/t2001_render.h"

namespace epd::t2001::svc {

/** @brief 업무단 에러코드 (FSM/서버 보고용, 어느 단계에서 실패했는지 식별) */
enum class ErrStep : uint8_t {
    none = 0,
    ensure_ready,
    pwr_on,
    render,
    trigger,
    wait_done,
    pwr_off,
    mutex_acquire,
    cleanup
};

/** @brief display 호출 결과: low-level 원인 + 실패 단계 */
struct DisplayResult {
    Result low;   ///< low-level Result(원인)
    ErrStep step; ///< 어느 단계에서 실패했는지
};

/** @brief display 옵션: 재시도, 지연, 파형, 전원 제어 */
struct DisplayConfig {
    uint8_t max_attempts = 2; ///< display transaction 총 시도 횟수 (정책 EPD_RETRY_MAX)
    uint16_t backoff_ms = 200; ///< 실패 후 다음 시도 전 대기 (전원 사이클과 함께)
    uint16_t waveform = 0; ///< MODE_BASIC 등 파형 ID
    bool do_power = true;  ///< false면 전원 제어 생략 (회로 테스트용)
    uint32_t display_wait_timeout_ms = 120000; ///< DisplayArea 완료(DARCR0) 대기
};

/** @brief 8bpp 소스에서 패널로 이미지 렌더링 (display transaction 전체 수행) */
DisplayResult display_8bpp_from_source(
    render::IDataSource& src,
    size_t total_len_bytes,
    const DisplayConfig& dc = DisplayConfig());

/** @brief 서비스 수준 상태 (Transport/Mailbox 아래를 시스템 관점에서 감쌈) */
enum class State : uint8_t {
    UNINIT = 0,
    INITING,
    READY,
    FAILED,
};

/** @brief service_init 재시도/로깅 옵션 */
struct Config {
    uint8_t max_service_attempts = 3;  ///< init 전체 재시도 횟수 (메일박스/SPI retry 위에 강한 재시도)
    uint16_t service_backoff_ms = 250; ///< 실패 시 다음 attempt 전 대기 (boot settle 포함)
    bool deinit_before_retry = true;   ///< 실패 시 deinit 후 재시도
    bool log_bootstat = true;          ///< [BOOTSTAT] 1줄 로그 출력 여부
    uint8_t reset_fail_to_failed = 1; ///< 1이면 service_attempts 소진 시 FAILED 전이
};

/** @brief 서비스 통계 (부팅, 성공/실패 수, 패널 정보 캐시) */
struct Stats {
    uint32_t boot_seq = 0;     ///< service_init 호출 시 증가 (콜드부트 측정용)
    uint32_t last_init_ms = 0; ///< 마지막 init 총 소요(ms)
    uint32_t ok_count = 0;
    uint32_t fail_count = 0;
    uint8_t last_service_attempt = 0; ///< 마지막 service-level attempt 번호 (1..N)
    uint32_t last_err = 0;            ///< 마지막 에러 (uint32_t)Result
    SysInfo last_sys {};               ///< 패널 정보 캐시 (READY에서만 유효)
};

/** @brief 서비스 초기화 (안정화 레이어 포함)
 *  @details 내부에서 epd::t2001::init(policy) 수행. 실패 시 강한 재시도 (deinit->delay->init) */
Result service_init(const Policy& policy, const Config& cfg = Config());

/** @brief 이미 READY 상태인지 확인 */
bool is_ready();

/** @brief T2001 sub rail + SPI + service_init (lazy, EPD/풀스크린 직전). */
Result ensure_ready();

/** @brief READY 상태의 SysInfo 가져오기 (캐시 사용) */
Result get_sys(SysInfo& out);

/** @brief 현재 상태 조회 */
State get_state();

/** @brief 통계 조회 */
const Stats& get_stats();

/** @brief 서비스 리셋 (상태/통계 일부 유지 옵션 가능)
 *  @param clear_stats true면 통계 초기화 */
void service_deinit(bool clear_stats = false);

} // namespace epd::t2001::svc
