// src/epd/t2001/t2001.cpp
#include "t2001.h"
#include <SPI.h>
#include "core/diag/log.h"
#include "core/power/watchdog.h"
#include "core/runtime/longOpPump.h"
#include "pins.h"

#include "t2001_mailbox.h"
#include "t2001_transport_spi.h"

static SPIClass gSpi(HSPI);
static epd::t2001::TransportSPI gTr(gSpi);
static epd::t2001::Mailbox gMbx(gTr);

static bool gInit = false;
static epd::t2001::SysInfo gSys {};

namespace epd::t2001 {

static constexpr const char* TAG = "EPD";

static void boot_settle_delay() {
    delay(200);
}

static void spi_restart(const SpiConfig& cfg, const Policy& policy) {
    logWarn(TAG, "spi_restart()");
    gTr.end();
    delay(30);
    (void) gTr.begin(cfg, policy);
    delay(30);
}

static Result mailbox_connect_lowhz(SpiConfig cfg, const Policy& policy, const InitConfig& icfg, BootDiag* diag) {
    cfg.hz = icfg.connect_hz;

    Result r = gTr.begin(cfg, policy);
    if (!ok(r))
        return r;

    boot_settle_delay();

    for (int round = 1; round <= icfg.max_round; round++) {
        logInfo(TAG, "mailbox connect round=%d hz=%lu", round, (unsigned long) cfg.hz);

        r = gMbx.init();
        if (ok(r)) {
            if (diag) {
                diag->last_round = round;
                diag->last_mbx_try = gMbx.last_hdr_try();
                diag->connect_hz = cfg.hz;
            }
            return Result::ok;
        }
        spi_restart(cfg, policy);

        // round backoff
        delay(80);
    }

    return Result::mailbox_init_fail;
}

Result init(const Policy& policy, const InitConfig* cfg, BootDiag* out_diag) {
    if (gInit) {
        if (out_diag)
            *out_diag = BootDiag {};
        return Result::ok;
    }

    InitConfig icfg;
    if (cfg)
        icfg = *cfg;

    const uint32_t t0 = millis();

    SpiConfig scfg;
    scfg.pin_mosi = PIN_SPI_MOSI;
    scfg.pin_miso = PIN_SPI_MISO;
    scfg.pin_sck = PIN_SPI_SCK;
    scfg.pin_cs = PIN_SPI_CS;
    scfg.pin_hrdy = PIN_HRDY;
    scfg.mode = SPI_MODE0;
    scfg.msb_first = true;

    BootDiag diag {};
    Result r = mailbox_connect_lowhz(scfg, policy, icfg, &diag);
    if (!ok(r)) {
        gTr.end();
        diag.last_result = r;
        diag.dt_ms = millis() - t0;
        if (out_diag)
            *out_diag = diag;
        logError(TAG, "mailbox connect failed r=%d", (int) r);
        return r;
    }

    r = gMbx.getDeviceInfo(gSys);
    if (!ok(r)) {
        gMbx.reset();
        gTr.end();
        diag.last_result = r;
        diag.dt_ms = millis() - t0;
        if (out_diag)
            *out_diag = diag;
        logError(TAG, "getDeviceInfo failed r=%d", (int) r);
        return r;
    }

    diag.last_result = Result::ok;
    diag.dt_ms = millis() - t0;
    if (out_diag)
        *out_diag = diag;

    logInfo(TAG, "init ok panel=%ux%u imgBuf0=0x%08lX",
        (unsigned) gSys.panel_w, (unsigned) gSys.panel_h, (unsigned long) gSys.img_buf0);

    gInit = true;
    return Result::ok;
}

bool isInitialized() {
    return gInit;
}

Result getSysInfo(SysInfo& out) {
    if (!gInit)
        return Result::not_initialized;
    out = gSys;
    return Result::ok;
}

void deinit() {
    if (!gInit)
        return;
    gTr.end();
    gInit = false;
}

TransportSPI& transport() {
    return gTr;
}

Result epdPowerOn(bool on) {
    if (!gInit)
        return Result::not_initialized;
    return gMbx.epdPowerOn(on);
}

Result applyReferencePanelVoltage() {
    if (!gInit)
        return Result::not_initialized;
    return gMbx.applyReferencePanelVoltage();
}

Result getTemperature(int16_t& realTempC, int16_t& setTempC) {
    if (!gInit)
        return Result::not_initialized;
    return gMbx.getTemperature(realTempC, setTempC);
}

Result forceSetTemperature(int16_t tempC) {
    if (!gInit)
        return Result::not_initialized;
    return gMbx.forceSetTemperature(tempC);
}

Result dpyArea(uint32_t imgBufAddr, uint16_t waveform, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!gInit)
        return Result::not_initialized;
    return gMbx.dpyArea(imgBufAddr, waveform, x, y, w, h);
}

Result waitDpyAreaDone(uint32_t timeout_ms) {
    if (!gInit)
        return Result::not_initialized;

    const uint32_t t0 = millis();
    Result r = gTr.wait_ready_us(8000000);
    if (!ok(r))
        return r;

    /* INF_DARCR0 bit6: set while DisplayArea refresh runs.
     * Idle value (e.g. 0x4509) already has bit6 clear — treating that as
     * "done" yields false success in ~30ms with a blank panel. Require busy
     * assert first, then wait for clear. */
    static constexpr uint16_t kDarcrBusyMask = (1u << 6);
    static constexpr uint32_t kBusyAssertTimeoutMs = 2000;

    uint16_t dar = 0;
    bool sawBusy = false;
    while ((millis() - t0) < kBusyAssertTimeoutMs) {
        r = gTr.reg_read16(INF_DARCR0, dar);
        if (!ok(r))
            return r;
        if ((dar & kDarcrBusyMask) != 0) {
            sawBusy = true;
            logInfo(TAG, "DPY busy assert %lums DARCR0=0x%04X",
                (unsigned long) (millis() - t0), (unsigned) dar);
            break;
        }
        longOpPump();
        delay(10);
    }
    if (!sawBusy) {
        logWarn(TAG, "DPY never busy %lums DARCR0=0x%04X",
            (unsigned long) (millis() - t0), (unsigned) dar);
        return Result::hrdy_timeout;
    }

    uint32_t lastLog = 0;
    while ((millis() - t0) < timeout_ms) {
        r = gTr.reg_read16(INF_DARCR0, dar);
        if (!ok(r))
            return r;
        if ((dar & kDarcrBusyMask) == 0) {
            logInfo(TAG, "DPY done %lums DARCR0=0x%04X",
                (unsigned long) (millis() - t0), (unsigned) dar);
            return Result::ok;
        }
        const uint32_t elapsed = millis() - t0;
        if (elapsed - lastLog >= 5000) {
            lastLog = elapsed;
            logInfo(TAG, "DPY busy ... %lums DARCR0=0x%04X",
                (unsigned long) elapsed, (unsigned) dar);
        }
        longOpPump();
        delay(50);
    }

    (void) gTr.reg_read16(INF_DARCR0, dar);
    logWarn(TAG, "DPY wait timeout %lums DARCR0=0x%04X",
        (unsigned long) timeout_ms, (unsigned) dar);
    return Result::hrdy_timeout;
}

Result refreshSysInfo(SysInfo& out) {
    if (!gInit)
        return Result::not_initialized;

    Result r = gMbx.getDeviceInfo(out);
    if (!ok(r))
        return r;

    gSys = out;
    return Result::ok;
}

} // namespace epd::t2001