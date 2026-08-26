/**
 * @file t2001_transport_spi.h
 * @brief T2001 SPI 트랜스포트 (TCON 명령, 레지스터, 메모리 버스트)
 */
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include "core/diag/log.h"
#include "t2001_types.h"

namespace epd::t2001 {

/** @brief SPI HW command codes (IT8957/T2001 host interface) */
enum : uint8_t {
    TCON_REG_RD = 0x10,
    TCON_REG_WR = 0x11,
    TCON_MEM_BST_RD_T = 0x12,
    TCON_MEM_BST_RD_S = 0x13,
    TCON_MEM_BST_WR = 0x14,
    TCON_MEM_BST_END = 0x15,

    TCON_LD_IMG_AREA = 0x21,
    TCON_LD_IMG_END = 0x22,

    USDEF_CMD_TRIGGER = 0xE2,
};

/** @brief Register addresses (T2001 MCSR / system map) */
enum : uint16_t {
    SYS_REG_BASE = 0x0000,
    I80CPCR = (SYS_REG_BASE + 0x0004),

    MCSR_BASE = 0x0200,
    HIRR = (MCSR_BASE + 0x0024),
    MBOXAR = (MCSR_BASE + 0x0030),

    INF_DARCR0 = 0x13C0, ///< bit6=1 while DisplayArea refresh in progress
};

struct SpiConfig {
    int pin_mosi = -1;
    int pin_miso = -1;
    int pin_sck = -1;
    int pin_cs = -1;
    int pin_hrdy = -1;

    uint32_t hz = 8000000;
    uint8_t mode = SPI_MODE0;
    bool msb_first = true;
};

class TransportSPI {
public:
    explicit TransportSPI(SPIClass& bus);

    Result begin(const SpiConfig& cfg, const Policy& policy);
    void end();

    bool is_ready() const {
        return _inited;
    }

    /** @brief ready 대기 (HRDY + HIRR polling) */
    Result wait_ready_us(uint32_t timeout_us);

    /** @brief ready 대기 (HRDY only) */
    Result wait_hrdy_only_us(uint32_t timeout_us);

    /** @brief raw 8-bit 명령 전송 */
    Result send_cmd(uint8_t cmd);

    /** @brief 레지스터 읽기/쓰기 */
    Result reg_read16(uint16_t addr, uint16_t& out);
    Result reg_write16(uint16_t addr, uint16_t val);

    Result reg_read16_noready(uint16_t addr, uint16_t& out);
    Result reg_write16_noready(uint16_t addr, uint16_t val);

    /** @brief HRDY-gated register read (wait HRDY only, no pre-command ready poll) */
    Result reg_read16_strict(uint16_t addr, uint16_t& out, uint32_t hrdy_timeout_us = 8000000);

    /** @brief 메모리 버스트 (word 단위) */
    Result mem_burst_write(uint32_t memAddr, const void* buf, uint32_t size_words);
    Result mem_burst_read(uint32_t memAddr, void* buf, uint32_t size_words);

    /** @brief MBOXAR 32비트 읽기 */
    uint32_t read_mboxar_32();

    Result stream_begin();
    Result stream_write(const uint8_t* buf, uint32_t n);
    Result stream_end();

private:
    void cs_low();
    void cs_high();

    Result long_write(const uint8_t* buf, uint32_t n);
    Result long_write_read_discard2(const uint8_t* outBuf, uint32_t outN, uint8_t* inBuf, uint32_t inN);

    void maybe_feed_wdt();

private:
    SPIClass& _spi;
    SpiConfig _cfg {};
    Policy _policy {};

    SPISettings _spiSet {8000000, MSBFIRST, SPI_MODE0};
    bool _inited = false;

    uint32_t _last_wdt_feed_ms = 0;

    static constexpr const char* TAG = "EPD_SPI";
};

} // namespace epd::t2001
