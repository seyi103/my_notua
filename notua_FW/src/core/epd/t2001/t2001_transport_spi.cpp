// src/epd/t2001/t2001_transport_spi.cpp
#include "core/epd/t2001/t2001_transport_spi.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "core/diag/log.h"

namespace epd::t2001 {

Result TransportSPI::stream_begin() {
    if (!_inited)
        return Result::not_initialized;
    cs_low();
    return Result::ok;
}

Result TransportSPI::stream_write(const uint8_t* buf, uint32_t n) {
    if (!_inited)
        return Result::not_initialized;
    if (!buf || n == 0)
        return Result::invalid_param;
    for (uint32_t i = 0; i < n; i++) {
        _spi.transfer(buf[i]);
        if ((i & 0x3FF) == 0)
            maybe_feed_wdt();
    }
    return Result::ok;
}

Result TransportSPI::stream_end() {
    if (!_inited)
        return Result::not_initialized;
    cs_high();
    return Result::ok;
}

static inline void swap_word_bytes(uint8_t* p, uint32_t wordCnt) {
    for (uint32_t i = 0; i < wordCnt; i++) {
        uint8_t* w = &p[i * 2];
        uint8_t t = w[0];
        w[0] = w[1];
        w[1] = t;
    }
}

TransportSPI::TransportSPI(SPIClass& bus)
    : _spi(bus) {
}

void TransportSPI::maybe_feed_wdt() {
    if (!_policy.feed_wdt)
        return;
    uint32_t now = millis();
    if ((now - _last_wdt_feed_ms) >= _policy.wdt_feed_period_ms) {
        _policy.feed_wdt();
        _last_wdt_feed_ms = now;
    }
}

Result TransportSPI::begin(const SpiConfig& cfg, const Policy& policy) {
    _cfg = cfg;
    _policy = policy;

    if (_cfg.pin_cs < 0 || _cfg.pin_hrdy < 0 || _cfg.pin_sck < 0 || _cfg.pin_miso < 0 || _cfg.pin_mosi < 0)
        return Result::invalid_param;

    pinMode(_cfg.pin_cs, OUTPUT);
    digitalWrite(_cfg.pin_cs, HIGH);
    pinMode(_cfg.pin_hrdy, INPUT_PULLUP);

    _spi.begin(_cfg.pin_sck, _cfg.pin_miso, _cfg.pin_mosi, _cfg.pin_cs);
    _spiSet = SPISettings(_cfg.hz, _cfg.msb_first ? MSBFIRST : LSBFIRST, _cfg.mode);

    _inited = true;
    _last_wdt_feed_ms = millis();
    logInfo(TAG, "begin hz=%lu mode=%u", (unsigned long) _cfg.hz, (unsigned) _cfg.mode);
    return Result::ok;
}

void TransportSPI::end() {
    if (!_inited)
        return;
    digitalWrite(_cfg.pin_cs, HIGH);
    _spi.end();
    _inited = false;
}

void TransportSPI::cs_low() {
    _spi.beginTransaction(_spiSet);
    digitalWrite(_cfg.pin_cs, LOW);
}
void TransportSPI::cs_high() {
    digitalWrite(_cfg.pin_cs, HIGH);
    _spi.endTransaction();
}

Result TransportSPI::long_write(const uint8_t* buf, uint32_t n) {
    if (!_inited)
        return Result::not_initialized;
    cs_low();
    for (uint32_t i = 0; i < n; i++) {
        _spi.transfer(buf[i]);
        if ((i & 0x3FF) == 0)
            maybe_feed_wdt();
    }
    cs_high();
    return Result::ok;
}

Result TransportSPI::long_write_read_discard2(const uint8_t* outBuf, uint32_t outN, uint8_t* inBuf, uint32_t inN) {
    if (!_inited)
        return Result::not_initialized;

    cs_low();
    for (uint32_t i = 0; i < outN; i++) {
        _spi.transfer(outBuf[i]);
        if ((i & 0x3FF) == 0)
            maybe_feed_wdt();
    }

    (void) _spi.transfer(0x00);
    (void) _spi.transfer(0x00);

    for (uint32_t i = 0; i < inN; i++) {
        inBuf[i] = _spi.transfer(0x00);
        if ((i & 0x3FF) == 0)
            maybe_feed_wdt();
    }
    cs_high();
    return Result::ok;
}

Result TransportSPI::wait_ready_us(uint32_t timeout_us) {
    if (!_inited)
        return Result::not_initialized;

    uint32_t start = micros();
    uint32_t last_yield_ms = millis();
    while ((micros() - start) < timeout_us) {
        maybe_feed_wdt();
        if (digitalRead(_cfg.pin_hrdy) == HIGH)
            return Result::ok;
        /* HRDY GPIO stuck LOW: poll HIRR (0x0224); non-zero = bus-ready fallback. Primary: HRDY HIGH. */
        uint16_t hirr = 0;
        if (ok(reg_read16_noready(HIRR, hirr)) && hirr != 0)
            return Result::ok;
        uint32_t now_ms = millis();
        if ((now_ms - last_yield_ms) >= 50) {
            vTaskDelay(1);
            last_yield_ms = now_ms;
        } else {
            delayMicroseconds(20);
        }
    }
    logWarn(TAG, "wait_ready timeout HRDY=%d", digitalRead(_cfg.pin_hrdy));
    return Result::hrdy_timeout;
}

Result TransportSPI::wait_hrdy_only_us(uint32_t timeout_us) {
    if (!_inited)
        return Result::not_initialized;

    uint32_t start = micros();
    uint32_t last_yield_ms = millis();
    while ((micros() - start) < timeout_us) {
        maybe_feed_wdt();
        if (digitalRead(_cfg.pin_hrdy) == HIGH)
            return Result::ok;
        /* Yield periodically so IDLE can run and task WDT does not trigger (e.g. mbox on CPU0) */
        uint32_t now_ms = millis();
        if ((now_ms - last_yield_ms) >= 50) {
            vTaskDelay(1);
            last_yield_ms = now_ms;
        } else {
            delayMicroseconds(20);
        }
    }
    return Result::hrdy_timeout;
}

Result TransportSPI::send_cmd(uint8_t cmd) {
    if (!_inited)
        return Result::not_initialized;
    Result r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;
    uint8_t b[1] = {cmd};
    return long_write(b, 1);
}

Result TransportSPI::reg_read16_noready(uint16_t addr, uint16_t& out) {
    if (!_inited)
        return Result::not_initialized;
    uint8_t cmd[3] = {TCON_REG_RD, (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF)};
    uint8_t payload2[2] = {0, 0};
    Result r = long_write_read_discard2(cmd, sizeof(cmd), payload2, 2);
    if (!ok(r))
        return r;
    out = (uint16_t)((payload2[0] << 8) | payload2[1]);
    return Result::ok;
}

Result TransportSPI::reg_write16_noready(uint16_t addr, uint16_t val) {
    if (!_inited)
        return Result::not_initialized;
    uint8_t cmd[5] = {TCON_REG_WR, (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return long_write(cmd, sizeof(cmd));
}

Result TransportSPI::reg_read16(uint16_t addr, uint16_t& out) {
    if (!_inited)
        return Result::not_initialized;
    Result r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;
    return reg_read16_noready(addr, out);
}

Result TransportSPI::reg_write16(uint16_t addr, uint16_t val) {
    if (!_inited)
        return Result::not_initialized;
    Result r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;
    return reg_write16_noready(addr, val);
}

Result TransportSPI::reg_read16_strict(uint16_t addr, uint16_t& out, uint32_t hrdy_timeout_us) {
    if (!_inited)
        return Result::not_initialized;
    Result r = wait_hrdy_only_us(hrdy_timeout_us);
    if (!ok(r))
        return r;
    return reg_read16_noready(addr, out);
}

uint32_t TransportSPI::read_mboxar_32() {
    uint16_t lo = 0, hi = 0;
    (void) reg_read16(MBOXAR, lo);
    (void) reg_read16((uint16_t)(MBOXAR + 2), hi);
    return ((uint32_t) hi << 16) | lo;
}

Result TransportSPI::mem_burst_write(uint32_t memAddr, const void* bufv, uint32_t size_words) {
    if (!_inited)
        return Result::not_initialized;
    if (!bufv || size_words == 0)
        return Result::invalid_param;

    const uint8_t* buf = (const uint8_t*) bufv;

    uint8_t hdr[9] = {
        TCON_MEM_BST_WR,
        (uint8_t)((memAddr >> 8) & 0xFF),
        (uint8_t)(memAddr & 0xFF),
        (uint8_t)((memAddr >> 24) & 0xFF),
        (uint8_t)((memAddr >> 16) & 0xFF),
        (uint8_t)((size_words >> 8) & 0xFF),
        (uint8_t)(size_words & 0xFF),
        (uint8_t)((size_words >> 24) & 0xFF),
        (uint8_t)((size_words >> 16) & 0xFF),
    };

    Result r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;

    cs_low();
    for (int i = 0; i < 9; i++)
        _spi.transfer(hdr[i]);

    // on-wire: [hi lo]
    for (uint32_t w = 0; w < size_words; w++) {
        uint32_t i = w * 2;
        uint8_t lo = buf[i + 0];
        uint8_t hi = buf[i + 1];
        _spi.transfer(hi);
        _spi.transfer(lo);
        if ((w & 0x1FF) == 0)
            maybe_feed_wdt();
    }
    cs_high();

    r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;

    const uint8_t endc[1] = {TCON_MEM_BST_END};
    return long_write(endc, 1);
}

Result TransportSPI::mem_burst_read(uint32_t memAddr, void* bufv, uint32_t size_words) {
    if (!_inited)
        return Result::not_initialized;
    if (!bufv || size_words == 0)
        return Result::invalid_param;

    uint8_t* buf = (uint8_t*) bufv;

    uint8_t trig[9] = {
        TCON_MEM_BST_RD_T,
        (uint8_t)((memAddr >> 8) & 0xFF),
        (uint8_t)(memAddr & 0xFF),
        (uint8_t)((memAddr >> 24) & 0xFF),
        (uint8_t)((memAddr >> 16) & 0xFF),
        (uint8_t)((size_words >> 8) & 0xFF),
        (uint8_t)(size_words & 0xFF),
        (uint8_t)((size_words >> 24) & 0xFF),
        (uint8_t)((size_words >> 16) & 0xFF),
    };
    const uint8_t rd_s[1] = {TCON_MEM_BST_RD_S};
    const uint8_t endc[1] = {TCON_MEM_BST_END};

    Result r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;

    r = long_write(trig, sizeof(trig));
    if (!ok(r))
        return r;

    r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;

    r = long_write_read_discard2(rd_s, 1, buf, size_words * 2);
    if (!ok(r))
        return r;

    r = wait_ready_us(_policy.hrdy_timeout_ms * 1000UL);
    if (!ok(r))
        return r;

    r = long_write(endc, 1);
    if (!ok(r))
        return r;

    swap_word_bytes(buf, size_words);
    return Result::ok;
}

} // namespace epd::t2001
