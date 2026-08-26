/**
 * @file t2001_mailbox.h
 * @brief T2001 메일박스 프로토콜 (cmd, cmdIndex, WData 패킷)
 */
#pragma once
#include <stdint.h>
#include "t2001_transport_spi.h"
#include "t2001_types.h"

namespace epd::t2001 {

#pragma pack(push, 1)
/** @brief 메일박스 헤더 */
struct TMBHdr {
    uint16_t usSignature;
    uint16_t usCmdAddrL;
    uint16_t usCmdAddrH;
    uint16_t usRDataAddrL;
    uint16_t usRDataAddrH;
    uint16_t usMaxArgSize;
    uint16_t usStatus;
    uint16_t usReserved;
};

static constexpr int MAX_ARG_CNT = 16;

/** @brief 메일박스 패킷 (cmd + cmdIndex + WData[0..15]) */
struct TUDefCmdArg {
    uint16_t usCmdCode;
    uint16_t usCmdIndex; ///< CmdIndex (이전 reserved 필드)
    uint16_t WData[MAX_ARG_CNT];
};

struct DeviceInfoRaw {
    uint16_t panel_width;
    uint16_t panel_height;
    uint16_t img_buf0_addr_l;
    uint16_t img_buf0_addr_h;
    uint16_t fw_version[8];
    uint16_t LUT_version[8];
    uint16_t WBF_buf_addr_l;
    uint16_t WBF_buf_addr_h;
    uint16_t img_buf1_addr_l;
    uint16_t img_buf1_addr_h;
    uint16_t usCFAImgBufAddrL;
    uint16_t usCFAImgBufAddrH;
};

/** @brief 0x0042 EPD_PWR_VOL mailbox payload (panel voltage before DPY) */
struct TEPDPwrSet {
    uint16_t usOpCode;
    uint16_t usVSH1;
    uint16_t usVSL1;
    uint16_t usVSH2;
    uint16_t usVSL2;
    uint16_t usVSH3;
    uint16_t usVSL3;
    uint16_t usVCom;
};
#pragma pack(pop)

/** @brief user-define 명령 코드 */
enum : uint16_t {
    USDEF_CMD_GET_DEV_INFO = 0x00E0,
    USDEF_CMD_DPY_AREA = 0x0034,
    USDEF_CMD_FORCE_SET_TEMP = 0x0033,
    USDEF_CMD_EPD_PWR_ON = 0x0038,
    USDEF_CMD_EPD_PWR_VOL = 0x0042,
};

class Mailbox {
public:
    explicit Mailbox(TransportSPI& tr);

    Result init();
    bool is_inited() const {
        return _inited;
    }

    /** @brief init 진단용: 마지막 헤더 시도 횟수 */
    uint16_t last_hdr_try() const {
        return _last_hdr_try;
    }

    Result getDeviceInfo(SysInfo& out);
    Result getDeviceInfoRaw(DeviceInfoRaw& out);

    Result dpyArea(uint32_t imgBufAddr, uint16_t waveform, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    Result forceSetTemperature(int16_t tempC);
    Result getTemperature(int16_t& realTempC, int16_t& setTempC);

    Result epdPowerOn(bool on);
    /** @brief VSX/VCOM (0x0042) before DisplayArea, after 0x0038 panel power on */
    Result applyReferencePanelVoltage();
    void reset();

private:
    Result epdPwrVol(const TEPDPwrSet& pwr);
    Result wait_mbox_ready_us(uint32_t timeout_us);
    Result i80cpcr_set_and_verify();
    Result read_mbox_header_retry();

    uint32_t rdata_addr() const {
        return ((uint32_t) _mbxHdr.usRDataAddrH << 16) | _mbxHdr.usRDataAddrL;
    }

    bool header_is_sane() const;

private:
    TransportSPI& _tr;
    bool _inited = false;

    uint32_t _mbox_base = 0;
    uint32_t _cmd_tab = 0;

    TMBHdr _mbxHdr {};

    uint16_t _cmd_index = 1;
    uint16_t _last_hdr_try = 0;
    static constexpr const char* TAG = "EPD_MBX";
};

} // namespace epd::t2001
