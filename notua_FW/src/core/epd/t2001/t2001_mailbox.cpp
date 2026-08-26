// src/epd/t2001/t2001_mailbox.cpp
#include "core/epd/t2001/t2001_mailbox.h"
#include <string.h>
#include "core/diag/log.h"

namespace epd::t2001 {

Mailbox::Mailbox(TransportSPI& tr)
    : _tr(tr) {
}

Result Mailbox::i80cpcr_set_and_verify() {
    for (int i = 0; i < 3; i++) {
        (void) _tr.reg_write16(I80CPCR, 0x0001);
        delay(5);

        uint16_t v = 0;
        (void) _tr.reg_read16(I80CPCR, v);
        if (v == 0x0001)
            return Result::ok;

        logWarn(TAG, "I80CPCR readback try=%d got=0x%04X expect=0x0001", i + 1, (unsigned) v);
        delay(20);
    }
    return Result::mailbox_init_fail;
}

bool Mailbox::header_is_sane() const {
    static constexpr uint16_t EXPECT_SIG = 0x8957; /* IT8957 */

    if (_mbxHdr.usSignature != EXPECT_SIG)
        return false;

    uint32_t cmdTab = ((uint32_t) _mbxHdr.usCmdAddrH << 16) | _mbxHdr.usCmdAddrL;
    uint32_t rData = ((uint32_t) _mbxHdr.usRDataAddrH << 16) | _mbxHdr.usRDataAddrL;

    if (cmdTab == 0 || rData == 0)
        return false;
    if ((cmdTab & 0x1) != 0 || (rData & 0x1) != 0)
        return false;

    if ((_mbxHdr.usStatus & 0x0001) == 0) /* ready bit */
        return false;

    if (_mbxHdr.usMaxArgSize == 0 || _mbxHdr.usMaxArgSize > 0x0040)
        return false;

    return true;
}

Result Mailbox::read_mbox_header_retry() {
    static constexpr uint16_t EXPECT_SIG = 0x8957;
    _last_hdr_try = 0;

    for (int attempt = 1; attempt <= 5; attempt++) {
        _last_hdr_try = (uint16_t) attempt;
        memset(&_mbxHdr, 0, sizeof(_mbxHdr));

        Result wr = _tr.wait_hrdy_only_us(8000000);
        if (!ok(wr)) {
            delay(20);
            continue;
        }

        Result r = _tr.mem_burst_read(_mbox_base, &_mbxHdr, (uint32_t)(sizeof(TMBHdr) / 2));
        if (!ok(r)) {
            delay(20);
            continue;
        }

        _cmd_tab = ((uint32_t) _mbxHdr.usCmdAddrH << 16) | _mbxHdr.usCmdAddrL;
        uint32_t rData = rdata_addr();

        logInfo(TAG,
            "try%d Sig=0x%04X Status=0x%04X MaxArg=0x%04X CmdTab=0x%08lX RData=0x%08lX",
            attempt,
            (unsigned) _mbxHdr.usSignature,
            (unsigned) _mbxHdr.usStatus,
            (unsigned) _mbxHdr.usMaxArgSize,
            (unsigned long) _cmd_tab,
            (unsigned long) rData);

        if (header_is_sane())
            return Result::ok;

        const bool sig_ok = (_mbxHdr.usSignature == EXPECT_SIG);
        const bool looks_booting =
            sig_ok &&
            ((_mbxHdr.usMaxArgSize == 0) ||
                ((_mbxHdr.usStatus & 0x0001) == 0) ||
                (_cmd_tab == 0) ||
                (rData == 0));

        delay(looks_booting ? 80 : 20);
    }

    return Result::mailbox_init_fail;
}

Result Mailbox::init() {
    if (_inited)
        return Result::ok;
    if (!_tr.is_ready())
        return Result::not_initialized;

    delay(100);

    Result r = i80cpcr_set_and_verify();
    if (!ok(r)) {
        logError(TAG, "I80CPCR set/readback failed (see readback value in prior WARN)");
        return Result::mailbox_init_fail;
    }

    uint32_t m1 = _tr.read_mboxar_32();
    delay(2);
    uint32_t m2 = _tr.read_mboxar_32();

    auto valid_mbox = [](uint32_t v) -> bool {
        return (v != 0) && ((v & 0x1) == 0);
    };

    if (valid_mbox(m1) && (m1 == m2))
        _mbox_base = m1;
    else if (valid_mbox(m2))
        _mbox_base = m2;
    else if (valid_mbox(m1))
        _mbox_base = m1;
    else
        _mbox_base = 0;

    logInfo(TAG, "MBOXAR1=0x%08lX MBOXAR2=0x%08lX use=0x%08lX",
        (unsigned long) m1, (unsigned long) m2, (unsigned long) _mbox_base);

    if (_mbox_base == 0) {
        logError(TAG, "MBOXAR invalid (0 or not aligned)");
        return Result::mailbox_bad_addr;
    }

    r = read_mbox_header_retry();
    if (!ok(r)) {
        logError(TAG, "Mailbox header read failed after retries");
        return Result::mailbox_init_fail;
    }

    _inited = true;
    return Result::ok;
}

void Mailbox::reset() {
    _inited = false;
    _mbox_base = 0;
    _cmd_tab = 0;
    _last_hdr_try = 0;
    memset(&_mbxHdr, 0, sizeof(_mbxHdr));
}

Result Mailbox::getDeviceInfoRaw(DeviceInfoRaw& out) {
    if (!_inited)
        return Result::not_initialized;

    TUDefCmdArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.usCmdCode = USDEF_CMD_GET_DEV_INFO;
    arg.usCmdIndex = _cmd_index++;

    Result r = _tr.mem_burst_write(_cmd_tab, &arg.usCmdCode, 2); // cmd + index
    if (!ok(r))
        return r;

    r = _tr.send_cmd(USDEF_CMD_TRIGGER);
    if (!ok(r))
        return r;

    memset(&out, 0, sizeof(out));
    r = _tr.mem_burst_read(rdata_addr(), &out, (uint32_t)(sizeof(DeviceInfoRaw) / 2));
    if (!ok(r))
        return r;

    return Result::ok;
}

Result Mailbox::getDeviceInfo(SysInfo& out) {
    DeviceInfoRaw di;
    Result r = getDeviceInfoRaw(di);
    if (!ok(r))
        return r;

    uint32_t imgBuf0 = ((uint32_t) di.img_buf0_addr_h << 16) | di.img_buf0_addr_l;
    out.panel_w = di.panel_width;
    out.panel_h = di.panel_height;
    out.img_buf0 = imgBuf0;
    return Result::ok;
}

Result Mailbox::dpyArea(uint32_t imgBufAddr, uint16_t waveform, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!_inited)
        return Result::not_initialized;

    TUDefCmdArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.usCmdCode = USDEF_CMD_DPY_AREA;
    arg.usCmdIndex = _cmd_index++;

    arg.WData[0] = x;
    arg.WData[1] = y;
    arg.WData[2] = w;
    arg.WData[3] = h;
    arg.WData[4] = waveform;
    arg.WData[5] = (uint16_t)(imgBufAddr & 0xFFFF);
    arg.WData[6] = (uint16_t)((imgBufAddr >> 16) & 0xFFFF);

    Result r = _tr.mem_burst_write(_cmd_tab, &arg.usCmdCode, 2 + 7);
    if (!ok(r))
        return r;

    r = _tr.send_cmd(USDEF_CMD_TRIGGER);
    if (!ok(r))
        return r;

    return Result::ok;
}

Result Mailbox::forceSetTemperature(int16_t tempC) {
    if (!_inited)
        return Result::not_initialized;

    TUDefCmdArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.usCmdCode = USDEF_CMD_FORCE_SET_TEMP;
    arg.usCmdIndex = _cmd_index++;

    arg.WData[0] = 0x0001;
    arg.WData[1] = (uint16_t) tempC;

    Result r = _tr.mem_burst_write(_cmd_tab, &arg.usCmdCode, 2 + 2);
    if (!ok(r))
        return r;

    r = _tr.send_cmd(USDEF_CMD_TRIGGER);
    if (!ok(r))
        return r;

    return Result::ok;
}

Result Mailbox::getTemperature(int16_t& realTempC, int16_t& setTempC) {
    if (!_inited)
        return Result::not_initialized;

    TUDefCmdArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.usCmdCode = USDEF_CMD_FORCE_SET_TEMP;
    arg.usCmdIndex = _cmd_index++;

    arg.WData[0] = 0x0000;
    arg.WData[1] = 0x0000;

    Result r = _tr.mem_burst_write(_cmd_tab, &arg.usCmdCode, 2 + 2);
    if (!ok(r))
        return r;

    r = _tr.send_cmd(USDEF_CMD_TRIGGER);
    if (!ok(r))
        return r;

    uint16_t rbuf[2] = {0, 0};
    r = _tr.mem_burst_read(rdata_addr(), rbuf, 2);
    if (!ok(r))
        return r;

    realTempC = (int16_t) rbuf[0];
    setTempC = (int16_t) rbuf[1];
    return Result::ok;
}

Result Mailbox::epdPowerOn(bool on) {
    if (!_inited)
        return Result::not_initialized;

    TUDefCmdArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.usCmdCode = USDEF_CMD_EPD_PWR_ON;
    arg.usCmdIndex = _cmd_index++;
    arg.WData[0] = on ? 1 : 0;

    /* Vendor mailbox slot: 2 + 16 words for command 0x0038 (panel power on) */
    Result r = _tr.mem_burst_write(_cmd_tab, &arg.usCmdCode, 2 + 16);
    if (!ok(r))
        return r;

    r = _tr.send_cmd(USDEF_CMD_TRIGGER);
    if (!ok(r))
        return r;

    Result wr = _tr.wait_ready_us(5000000);
    if (!ok(wr)) {
        logWarn(TAG, "EPD_PWR_ON(0x0038)=%u wait fail", on ? 1u : 0u);
        return wr;
    }
    return Result::ok;
}

Result Mailbox::epdPwrVol(const TEPDPwrSet& pwr) {
    if (!_inited)
        return Result::not_initialized;

    TUDefCmdArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.usCmdCode = USDEF_CMD_EPD_PWR_VOL;
    arg.usCmdIndex = _cmd_index++;
    memcpy(arg.WData, &pwr.usOpCode, sizeof(TEPDPwrSet));

    Result r = _tr.mem_burst_write(_cmd_tab, &arg.usCmdCode, 2 + 8);
    if (!ok(r))
        return r;

    r = _tr.send_cmd(USDEF_CMD_TRIGGER);
    if (!ok(r))
        return r;

    return _tr.wait_ready_us(8000000);
}

Result Mailbox::applyReferencePanelVoltage() {
    auto setVsx = [this](uint8_t setNo, uint16_t vsh, uint16_t vsl) -> Result {
        TEPDPwrSet pwr {};
        pwr.usOpCode = 1;
        if (setNo == 1) {
            pwr.usVSH1 = vsh;
            pwr.usVSL1 = vsl;
        } else if (setNo == 2) {
            pwr.usVSH2 = vsh;
            pwr.usVSL2 = vsl;
        } else if (setNo == 3) {
            pwr.usVSH3 = vsh;
            pwr.usVSL3 = vsl;
        } else {
            return Result::invalid_param;
        }
        return epdPwrVol(pwr);
    };

    Result r = setVsx(1, 7000, 7000);
    if (!ok(r))
        return r;
    r = setVsx(2, 7200, 9750);
    if (!ok(r))
        return r;
    r = setVsx(3, 15000, 15000);
    if (!ok(r))
        return r;

    TEPDPwrSet vcom {};
    vcom.usOpCode = 1;
    vcom.usVCom = 1640;
    return epdPwrVol(vcom);
}

} // namespace epd::t2001
