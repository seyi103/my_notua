/**
 * @file t2001_render.h
 * @brief T2001 EPD 8bpp 렌더링 (LD_IMG_AREA, LD_IMG_END, 데이터 소스 추상화)
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "t2001_transport_spi.h"
#include "t2001_types.h"


namespace epd::t2001::render {

/** @brief IT8957/T2001 SPI용 명령 코드 (테스트 코드 기준) */
static constexpr uint8_t kCmdLdImgArea = 0x21;
static constexpr uint8_t kCmdLdImgEnd = 0x22;

// ---- Data Source Abstraction ----
/** @brief 데이터 소스 추상화: dst에 최대 maxlen 채우고 읽은 바이트 반환
 *  @details 0 반환은 실패/EOF. 본 render는 정확한 길이 전제라 0이면 실패 처리 */
struct IDataSource {
    virtual size_t read(uint8_t* dst, size_t maxlen) = 0;
    virtual void rewind() {}
    virtual ~IDataSource() = default;
};

/** @brief PSRAM/메모리 버퍼에서 읽는 Source 구현체 */
class MemorySource final : public IDataSource {
public:
    MemorySource(const uint8_t* data, size_t len)
        : _p(data), _len(len), _off(0) {
    }

    size_t read(uint8_t* dst, size_t maxlen) override;

    void rewind() override {
        _off = 0;
    }

    size_t remaining() const {
        return (_off < _len) ? (_len - _off) : 0;
    }
    size_t size() const {
        return _len;
    }
    size_t offset() const {
        return _off;
    }

private:
    const uint8_t* _p;
    size_t _len;
    size_t _off;
};

/** @brief 렌더링 훅 (watchdog, 진행률 콜백) */
struct Hooks {
    void (*feed_wdt)() = nullptr;
    uint32_t wdt_period_ms = 0;
    void (*on_progress)(uint32_t sent, uint32_t total) = nullptr;
};

/** @brief send_8bpp_full_from_source 설정 */
struct Config {
    uint16_t usArg = 0x0030; ///< 테스트 코드 동일
    uint16_t block_h = 16;   ///< 테스트 코드 동일 (권장 8~32)
    uint32_t ready_timeout_us = 8000000;

    uint32_t io_chunk_bytes = 4096; ///< 내부 read buffer 크기 (16KB 내 clamp, 4096~8192 권장)
};

/** @brief panel_w*panel_h 바이트(8bpp)를 source에서 읽어 LD_IMG_AREA(블록 단위)로 전송, LD_IMG_END 블록마다 실행 */
Result send_8bpp_full_from_source(
    TransportSPI& tr,
    uint16_t panel_w,
    uint16_t panel_h,
    IDataSource& src,
    size_t total_len_bytes, ///< 반드시 panel_w*panel_h 와 동일 (양산 안전)
    const Config& cfg,
    const Hooks& hk);

} // namespace epd::t2001::render
