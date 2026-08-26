/**
 * @file pins.h
 * @brief Notua mass-production board pin definitions
 */
#pragma once

/** SPI */
#define PIN_SPI_MOSI 11
#define PIN_SPI_MISO 13
#define PIN_SPI_SCK 12
#define PIN_SPI_CS 10

/** T-con HRDY (K2 RDY) */
#define PIN_HRDY 14

/** Battery sense: ADC input + divider enable */
#define BATT_ADC 4
#define BATT_ADC_SW 5

/** Front-panel switch: INPUT, external idle LOW, press HIGH; deep-sleep GPIO
 * wake */
#define SW_PIN 16

/** Charger IC (I2C — driver later) */
#define CHARGER_INT 17
#define CHARGER_EN 18
#define CHARGER_SDA 8
#define CHARGER_SCL 9

/**
 * T2001 power rails
 * Boot: IO40(1V25) → IO39(1V8); IO41(6V5) before EPD 0x0038; IO38(3V3) last
 * before SPI. OFF (sleep teardown): 6V5 via boardPowerFinishEpdUpdate; then
 * IO38(3V3) → 1V8 → 1V25 via boardPowerT2001Off
 */
#define PIN_T2001_3V3 38
#define PIN_1V8 39
#define PIN_1V25 40
#define PIN_6V5 41

/** T2001 control (active-LOW names; idle HIGH). RST pulse used at boot even if
 * PCB omits wire. */
#define T2001_RST 42
#define T2001_WK 2
