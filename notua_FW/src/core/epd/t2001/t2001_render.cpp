#include "t2001_render.h"
#include <Arduino.h>
#include <string.h>

namespace epd::t2001::render {

size_t MemorySource::read(uint8_t* dst, size_t maxlen) {
    if (!dst || maxlen == 0)
        return 0;
    if (_off >= _len)
        return 0;

    const size_t rem = _len - _off;
    const size_t n = (maxlen < rem) ? maxlen : rem;
    memcpy(dst, _p + _off, n);
    _off += n;
    return n;
}

static inline void maybe_feed_wdt(const Hooks& hk, uint32_t& last_ms) {
    if (!hk.feed_wdt || hk.wdt_period_ms == 0)
        return;
    const uint32_t now = millis();
    if ((now - last_ms) >= hk.wdt_period_ms) {
        hk.feed_wdt();
        last_ms = now;
    }
}

Result send_8bpp_full_from_source(
    TransportSPI& tr,
    uint16_t panel_w,
    uint16_t panel_h,
    IDataSource& src,
    size_t total_len_bytes,
    const Config& cfg,
    const Hooks& hk) {
    if (panel_w == 0 || panel_h == 0)
        return Result::invalid_param;

    const uint32_t expected = (uint32_t) panel_w * (uint32_t) panel_h;
    if ((uint32_t) total_len_bytes != expected)
        return Result::invalid_param;

    uint16_t blockH = cfg.block_h;
    if (blockH == 0)
        blockH = 1;

    uint32_t ioChunk = cfg.io_chunk_bytes;
    if (ioChunk < 512)
        ioChunk = 512;
    if (ioChunk > 16384)
        ioChunk = 16384;

    static uint8_t ioBuf[16384];

    uint32_t sent = 0;
    uint32_t last_report = 0;
    const uint32_t report_step = 256 * 1024;
    uint32_t last_wdt_ms = millis();

    for (uint16_t y = 0; y < panel_h; y = (uint16_t)(y + blockH)) {
        const uint16_t h = (y + blockH <= panel_h) ? blockH : (uint16_t)(panel_h - y);
        const uint32_t blkBytes = (uint32_t) panel_w * (uint32_t) h;

        /* LD_IMG_AREA header (11 bytes) */
        const uint8_t cmd[11] = {
            kCmdLdImgArea,
            (uint8_t)(cfg.usArg >> 8),
            (uint8_t)(cfg.usArg & 0xFF),
            0x00,
            0x00, // x=0
            (uint8_t)(y >> 8),
            (uint8_t)(y & 0xFF),
            (uint8_t)(panel_w >> 8),
            (uint8_t)(panel_w & 0xFF),
            (uint8_t)(h >> 8),
            (uint8_t)(h & 0xFF),
        };

        Result r = tr.wait_ready_us(cfg.ready_timeout_us);
        if (!ok(r))
            return r;

        r = tr.stream_begin();
        if (!ok(r))
            return r;

        r = tr.stream_write(cmd, (uint32_t) sizeof(cmd));
        if (!ok(r)) {
            (void) tr.stream_end();
            return r;
        }

        uint32_t remain = blkBytes;
        while (remain) {
            maybe_feed_wdt(hk, last_wdt_ms);

            uint32_t want = remain;
            if (want > ioChunk)
                want = ioChunk;

            const size_t got = src.read(ioBuf, (size_t) want);
            if (got == 0) {
                (void) tr.stream_end();
                return Result::spi_io;
            }

            r = tr.stream_write(ioBuf, (uint32_t) got);
            if (!ok(r)) {
                (void) tr.stream_end();
                return r;
            }

            remain -= (uint32_t) got;
            sent += (uint32_t) got;

            if (hk.on_progress && (sent - last_report >= report_step || sent == expected)) {
                hk.on_progress(sent, expected);
                last_report = sent;
            }
        }

        r = tr.stream_end();
        if (!ok(r))
            return r;

        r = tr.wait_ready_us(cfg.ready_timeout_us);
        if (!ok(r))
            return r;

        r = tr.send_cmd(kCmdLdImgEnd);
        if (!ok(r))
            return r;
    }

    return (sent == expected) ? Result::ok : Result::spi_io;
}

} // namespace epd::t2001::render
