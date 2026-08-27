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

The boot application initializes native USB CDC, verifies PSRAM, mounts LittleFS without formatting,
validates and sorts up to three `/images/*.bin` Y8 frames,
loads one 1600 x 1200 frame explicitly into byte-addressable PSRAM, and sends it through the existing
T2001 service. Its NVS index advances only after a successful refresh. Every exit powers the panel
down. The default `esp32-s3-dev` environment always remains awake, feeds the watchdog, and preserves
USB diagnostics. Only an explicitly selected `esp32-s3-release` build may enter five-minute timer
deep sleep, and only after both display refresh and index persistence succeed. Release failures also
remain awake. Invalid-size files are logged and skipped; filesystem errors never trigger an automatic
format.

## Runtime audit and board-verification boundary

The imported EPD driver's SPI protocol, production pin assignments, rail order (1.25 V, 1.8 V,
3.3 V, then 6.5 V immediately before panel power), reset timing, mailbox retry behavior, waveform,
and refresh sequence are intentionally unchanged. The application now makes terminal behavior
explicit rather than treating every error as a reason to sleep. It also reports the wake cause,
LittleFS capacity, and PSRAM capacity/largest block before allocating a frame.

The following cannot be established by a successful host build and must be checked on production
hardware:

1. Native USB CDC enumerates after reset and remains present after every injected failure in a
   development build (missing filesystem, empty/invalid images, allocation failure, T2001 failure,
   and NVS write failure).
2. The flashed 16 MB module has OPI PSRAM and the uploaded LittleFS image lands in the `littlefs`
   partition at `0x310000`; the capacity log must agree with the partition table.
3. GPIO levels and delays match the board: 1.25 V, 1.8 V, 3.3 V/reset for SPI, followed by 6.5 V and
   mailbox panel power for refresh; all rails turn off on every terminal path.
4. HRDY, SPI traffic, temperature handling, VSX application, DARCR busy assertion/completion,
   refresh appearance, and retry/power recovery match the original T2001 hardware behavior.
5. The watchdog does not reset during file loading, SPI transfer, refresh waits, or the awake
   diagnostic loop. Verify that the NVS index changes only after a visibly completed refresh.
6. An explicitly flashed release build sleeps only after success, wakes after five minutes, and
   advances to the next sorted frame. A release failure must retain USB and logs instead of sleeping.

`scripts/check_offline_dependencies.py` runs before every PlatformIO build. It rejects unresolved
quoted includes and WiFi, MQTT, HTTP, OTA, or BLE framework headers, preventing either a broken
local driver dependency or a premature application transport from silently entering the core.

The 16 MB partition table has one factory application slot rather than OTA slots. Its remaining
data region is a LittleFS filesystem for local frames; it is not a network transport.

## Preparing and uploading test images

Frames are headerless, row-major Y8 files: one byte per pixel, exactly 1600 x 1200 = 1,920,000
bytes. Their pixels use only the Spectra 6 Y8 palette values `0x00`, `0xF8`, `0x20`, `0x40`,
`0x10`, and `0x30`. Generate two or three deterministic patterns from the firmware directory:

```sh
cd notua_FW
python3 scripts/generate_test_images.py --count 3
```

The generator verifies every output is exactly 1,920,000 bytes and contains no values outside that
palette, failing immediately if either check does not pass. The generated files land in
`data/images/` and are ignored by Git. Connect the ESP32-S3, optionally
set `upload_port` in `platformio.ini`, then build and upload the LittleFS partition:

```sh
python3 -m platformio run --target buildfs
python3 -m platformio run --target uploadfs
python3 -m platformio run --environment esp32-s3-dev --target upload
```

Use `--environment esp32-s3-release` only for deployment testing. Upload the filesystem before
firmware on an initially blank device. `uploadfs` replaces the
filesystem contents. Serial logs at 115200 baud report rejected sizes, the selected path, display
result, persisted next index, and sleep entry. Panel appearance, rail sequencing, refresh timing,
and deep-sleep wake behavior still require verification on the actual production board.
