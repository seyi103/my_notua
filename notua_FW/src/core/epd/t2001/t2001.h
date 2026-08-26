/**
 * @file t2001.h
 * @brief T2001 EPD 저수준 API (init, transport, power, display)
 */
#pragma once
#include "core/epd/t2001/t2001_transport_spi.h"
#include "core/epd/t2001/t2001_types.h"

namespace epd::t2001 {

/** @brief 초기화 (diag/config optional로 service/FSM이 수집 가능) */
Result init(const Policy& policy, const InitConfig* cfg = nullptr, BootDiag* out_diag = nullptr);

/** @brief 초기화 여부 */
bool isInitialized();

/** @brief SysInfo 조회 */
Result getSysInfo(SysInfo& out);

/** @brief 리소스 해제 */
void deinit();

/** @brief TransportSPI 참조 */
epd::t2001::TransportSPI& transport();

/** @brief EPD 전원 on/off */
Result epdPowerOn(bool on);

/** @brief VSX/VCOM (0x0042) before DisplayArea */
Result applyReferencePanelVoltage();

/** @brief 온도 조회 (realTempC, setTempC) */
Result getTemperature(int16_t& realTempC, int16_t& setTempC);

/** @brief 온도 강제 설정 (파형/LUT 색상 정확도용, 0~50℃) */
Result forceSetTemperature(int16_t tempC);

/** @brief 영역 refresh (imgBufAddr, waveform, x, y, w, h) */
Result dpyArea(uint32_t imgBufAddr, uint16_t waveform, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/** @brief DisplayArea 완료 대기: DARCR0 bit6 busy assert 후 clear (timeout_ms = HRDY 포함 총 예산) */
Result waitDpyAreaDone(uint32_t timeout_ms);

/** @brief SysInfo 새로고침 */
Result refreshSysInfo(SysInfo& out);
} // namespace epd::t2001
