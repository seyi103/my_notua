# Notua EPD core firmware architecture

## Dependency audit

The imported T2001 implementation is a driver stack rather than a standalone source file. Its
complete project-local dependency chain is kept in this firmware:

1. `t2001_service` owns retries and the complete display transaction.
2. `t2001`, `t2001_mailbox`, `t2001_transport_spi`, and `t2001_render` implement the TCON
   protocol and 8-bpp streaming over Arduino `SPI`.
3. `boardPower` and `pins` provide the ordered 1.25 V, 1.8 V, 3.3 V, and 6.5 V rail control.
4. `watchdog` and `longOpPump` keep the ESP task watchdog and scheduler alive during blocking
   transfers and panel refreshes.
5. `diag/log` provides USB-serial diagnostics.

The missing local headers and implementations are stored under `src/core`, and all cross-directory
includes use the single PlatformIO `src` include root. For example, driver code includes
`core/epd/t2001/t2001_types.h` rather than location-dependent `epd/...`, `power/...`, or bare
`pins.h` paths.

## Current boundary

| Area | Decision |
| --- | --- |
| EPD | Keep the complete T2001 service, transport, mailbox, render, and type chain |
| Board power | Keep production pin definitions and ordered rail/reset control |
| Long operations | Keep watchdog feeding and scheduler yielding in blocking EPD paths |
| PSRAM | Keep OPI PSRAM build configuration and verify PSRAM availability at startup |
| WiFi / MQTT / HTTP / OTA | Removed from includes, libraries, initialization, and OTA partitioning |
| BLE | Future frame transport boundary; deliberately not implemented or initialized yet |
| Flutter | Future client boundary; no app implementation is part of this firmware change |

The boot application only establishes a safe hardware baseline: it disables the panel rails,
starts the watchdog, verifies PSRAM, and reports that no frame transport is configured. It does not
initialize the EPD because there is not yet an image source or application command boundary.

`scripts/check_offline_dependencies.py` runs before every PlatformIO build. It rejects unresolved
quoted includes and WiFi, MQTT, HTTP, OTA, or BLE framework headers, preventing either a broken
local driver dependency or a premature application transport from silently entering the core.

The 16 MB partition table has one factory application slot rather than OTA slots. The data region
is reserved for a future frame-storage design; no filesystem or transport is mounted by the current
boot application.
