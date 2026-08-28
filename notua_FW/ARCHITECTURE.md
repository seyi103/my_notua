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
| BLE | Button-wake NimBLE peripheral with Ready plus queued Y8 transfer characteristics |
| Flutter | Future client boundary; no app implementation is part of this firmware change |

On power-on/reset, an unrecognized EXT1 source, or any other non-timer/non-GPIO16 wake, the boot
application retains the existing photo-cycle policy: it initializes native USB CDC, verifies PSRAM, mounts LittleFS without formatting,
validates and sorts up to three `/images/*.bin` Y8 frames,
loads one 1600 x 1200 frame explicitly into byte-addressable PSRAM, and sends it through the existing
T2001 service. Its NVS index advances only after a successful refresh. Every exit powers the panel
down. The default `esp32-s3-dev` environment always remains awake, feeds the watchdog, preserves USB
diagnostics, and reports the final run result plus uptime once per second. An explicitly selected
`esp32-s3-release` build enters five-minute timer deep sleep after both display refresh and index
persistence succeed. Release failures flush their diagnostics and enter a separate one-minute retry
deep sleep instead of remaining awake indefinitely. Invalid-size files are logged and skipped;
filesystem errors never trigger an automatic format.

## Wake routing, button polarity, and minimal BLE state

Wake routing is represented by the `WakePath` enum (`timerPhotoCycle`, `buttonBle`, or
`existingPolicy`), and the peripheral lifecycle by `BleState`. A timer wake runs the verified photo
cycle unchanged. GPIO16 (`SW_PIN`) is an externally driven active-HIGH button: its idle level is LOW,
both RTC internal pulls are disabled, and deep sleep uses `ESP_EXT1_WAKEUP_ANY_HIGH`. The boot log
includes the numeric wake cause, selected route, and EXT1 GPIO mask.

A GPIO16 EXT1 wake does not mount LittleFS, open NVS, increment the image index, power the EPD, or
refresh it. It initializes a peripheral named `Notua`, advertises the stable primary Service UUID
`7d2a4b70-8e67-4d8b-9f3a-36c89e210001`, and exposes the read-only status characteristic UUID
`7d2a4b70-8e67-4d8b-9f3a-36c89e210002` with the UTF-8 value `ready`. Callbacks only record
events into a FreeRTOS queue; they never log, restart advertising, or perform file, NVS, or EPD work.
`pollBlePeripheral()` owns all state transitions, timestamps, logging, and reconnection advertising.
Initial advertising expires after 60 seconds without a connection. A disconnect starts a 30-second
reconnection window, and the return value from the advertising start is checked before success is
logged. A connection is closed and cleaned up after 120 seconds without GATT activity. Reading the
status characteristic refreshes that deadline, and future transfer characteristics can call the
thread-safe `noteBleGattActivity()` hook to do the same.

Every partial initialization failure stops advertising where applicable, deinitializes NimBLE, and
deletes its event queue. A reconnection-advertising failure transitions to `initializationFailed`;
release sleeps on the one-minute error-retry policy rather than treating it as a normal timeout.
Development builds never sleep on these terminal paths and print the final `BleState` and uptime once
per second so native USB diagnostics remain observable.

NimBLE-Arduino 1.4.3 is pinned rather than the legacy Bluedroid Arduino BLE library because NimBLE
has a substantially smaller RAM footprint on ESP32-S3. This preserves internal heap for control and
the planned chunked transfer protocol while the existing 1,920,000-byte frame remains in PSRAM.
No Flutter or image conversion behavior is included. The transport accepts only an already generated,
exactly 1,920,000-byte Y8 file.

## BLE image-transfer protocol (version 1)

The primary service remains `7d2a4b70-8e67-4d8b-9f3a-36c89e210001`. Ready remains read-only at
`...0002` and continues to return UTF-8 `ready`. Control (`...0003`) is Write With Response, Data
(`...0004`) is Write Without Response, and Transfer Status (`...0005`) is Read + Notify. The full
UUID prefix is `7d2a4b70-8e67-4d8b-9f3a-36c89e21`; the suffix notation is only shorthand here.

All integers are explicitly encoded little-endian; no packed C structure defines the wire format.

| Packet | Exact byte layout |
| --- | --- |
| START (12 bytes) | `u8 version=1, u8 opcode=0x01, u8 slot, u8 reserved=0, u32 size, u32 crc32` |
| FINISH | `u8 version=1, u8 opcode=0x02` |
| ABORT | `u8 version=1, u8 opcode=0x03` |
| APPLY | `u8 version=1, u8 opcode=0x04` |
| DATA | `u32 offset, u8 payload[]`; total characteristic value is 5..512 bytes |
| STATUS (12 bytes) | `u8 version=1, u8 code, u16 reserved=0, u32 next_expected_offset, u32 detail` |

Status codes are fixed as follows: `0x01 START_ACCEPTED`, `0x02 ACK`, `0x03 COMMITTED`,
`0x04 APPLYING`, `0x80 BAD_COMMAND`, `0x81 BAD_SIZE`, `0x82 BAD_OFFSET`, `0x83 QUEUE_FULL`,
`0x84 CRC_MISMATCH`, `0x85 STORAGE_ERROR`, and `0x86 NOT_READY`. `detail` is zero normally;
BAD_SIZE reports the supplied/observed byte count, BAD_OFFSET reports the received offset,
CRC_MISMATCH reports the calculated CRC, and command errors may report received length or slot.
`next_expected_offset` always identifies the durable, streaming-file offset from which the sender
can retry.

START validates version, reserved byte, slot policy, exact size, and opens
`/images/notua_upload.tmp`. DATA callbacks only bounds-check and copy at most 512 bytes into an
eight-entry FreeRTOS queue. `pollBlePeripheral()` performs LittleFS writes and streaming CRC32/IEEE
(the same result as Python `zlib.crc32`). Duplicate offsets are ACKed without another write; future
offsets receive BAD_OFFSET. A full queue returns QUEUE_FULL. ACK notifications are coalesced: poll
sends one after eight persisted packets or once the transfer queue drains, never one per packet.
Errors remain immediate. The reference sender transmits at most eight packets and waits until ACK
reaches the window end, retrying from the supplied expected offset.
FINISH is processed in queue order and succeeds only after all earlier DATA has been written, the
size is 1,920,000, and CRC matches.

On commit, firmware (1) durably writes the marker, (2) renames final to backup, (3) renames temp to
final, (4) writes NVS pending, (5) removes backup, then (6) removes marker before sending COMMITTED.
Marker-write, final-to-backup, temp-to-final, and backup-restore failures have distinct internal
results mapped to STORAGE_ERROR detail values. A marker records the backup target so boot-time
recovery restores it if replacement was interrupted; stale temporary files are removed. Unknown
image files are never deleted. Sorted valid images define slots 0..2: an existing index replaces that
path, the next contiguous index creates `/images/notua_slot_N.bin`, and gaps or more than three valid
images are rejected. LittleFS is always mounted with formatting disabled.

COMMITTED is notified only after the final path is stored as NVS `pending`. APPLY causes APPLYING to
be notified; the main loop then waits for notification delivery, shuts BLE down, and restarts. Boot
prioritizes the pending path through the established EPD path. Only a successful display clears
pending and advances the normal sorted-image cycle index. Disconnect and ABORT close and remove an
unfinished temp file; reconnect requires a new START. CRC, size, offset, queue, or storage failures
never replace an existing frame or alter the cycle index.

## Python test sender

Install Python 3.10+ and Bleak with `python -m pip install bleak`. On Windows, use a machine with
Bluetooth LE enabled and run in PowerShell; on macOS, grant the terminal Bluetooth permission in
System Settings. From `notua_FW`, run:

```sh
python scripts/ble_send_test.py --file data/images/test_01.bin --slot 0
```

The sender scans by `Notua` or the service UUID, computes size and `zlib.crc32` before connecting,
uses the discovered Data characteristic's `max_write_without_response_size` minus the four-byte
offset (also capped to the firmware's 512-byte GATT value limit), subscribes before START, and prints
progress, rate, retry count, and final CRC. A disconnect is fatal and explicitly instructs the user
to reconnect and restart from START. Because NimBLE 1.4.3 has a void `notify()` API, firmware returns
whether it could synchronously issue notify while connected and logs failures; the characteristic's
read value is updated first in every case. On a notification timeout the sender reads Transfer Status
once, recovers its persisted offset, and retransmits from that offset.

## Transfer board verification (not CI)

CI/host checks cover packet constants and endian helpers, CRC reference behavior, offset branches,
slot/size guards, bounded queues, rollback calls, the Python CLI, and both firmware builds. Hardware
acceptance still must separately transfer a real generated frame, induce wrong CRC and future offset,
disconnect mid-transfer, fill the queue, replace each slot, and cut power at both rename boundaries.
For every fault confirm the former image and NVS cycle index remain valid. Finally send a correct
frame, observe COMMITTED before APPLYING/disconnect, verify restart displays it, and allow three timer
wakes to confirm sorted rotation continues.

Before every release-build sleep, the firmware enables the five-minute timer and EXT1 wake. If the
button is still HIGH, it waits at most 10 seconds for LOW while feeding the watchdog. On timeout it
logs an error and disables EXT1 for that one sleep, preventing an immediate wake loop; the timer
remains available. The development build continues the established no-deep-sleep policy, including
after a BLE timeout. Consequently button-wake sleep/re-wake acceptance testing must use the release
environment.

## Button/BLE production-board test

1. Build and flash `esp32-s3-release`, retain the already verified LittleFS images, and monitor USB
   serial at 115200 baud.
2. Allow a normal power-on boot to display a frame and sleep. Confirm its route is `existing_policy`,
   then confirm the next timer wake is `timer_photo_cycle` and advances exactly one image.
3. While asleep, drive GPIO16 HIGH with the physical button, then release it. Confirm an EXT1 wake
   mask containing bit 16 (`0x10000`), route `button_ble`, successful BLE initialization, and
   `advertising_started=true`. Confirm there is no LittleFS, NVS-index, or EPD-refresh log.
4. In a phone BLE scanner, find `Notua`, connect, discover the documented service and characteristic,
   and read `ready`. Confirm the connect log. Disconnect, confirm the disconnect/re-advertising log,
   reconnect within 30 seconds, and confirm another connect log.
5. While connected, read `ready` before 120 seconds and confirm the connection remains active for
   another 120-second window. Then perform no GATT activity, confirm the firmware terminates the
   connection after 120 seconds, cleans up BLE, and enters deep sleep in release.
6. Disconnect again and do not reconnect. Confirm advertising stops after 30 seconds and the board
   enters deep sleep. Repeat a button wake without ever connecting and confirm the corresponding
   timeout is 60 seconds before sleep.
7. In a development build, induce initialization or advertising failure and an advertising timeout;
   confirm USB remains enumerated and a `BLE terminal` heartbeat reports the final state and increasing
   uptime once per second.
8. Hold the button HIGH during timeout. Confirm the board waits for release rather than immediately
   waking. For the fault case, hold it longer than 10 seconds; confirm the explicit error, EXT1 is
   disabled for that sleep only, and the timer later wakes the board.

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
5. The watchdog does not reset during file loading, SPI transfer, refresh waits, release log flushing,
   or the awake diagnostic loop. The development terminal log must report the same final result and
   increasing uptime every second. Verify that the NVS index changes only after a visibly completed
   refresh.
6. An explicitly flashed release build sleeps for five minutes after success and advances to the next
   sorted frame. Injected release failures must flush the final error, sleep for one minute, then wake
   and retry without advancing the index.

`scripts/check_offline_dependencies.py` runs before every PlatformIO build. It rejects unresolved
quoted includes and WiFi, MQTT, HTTP, or OTA framework headers, preventing either a broken local
driver dependency or an unintended IP transport from silently entering the core. BLE is limited to
the pinned NimBLE dependency above.

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
python3 -m platformio run --environment esp32-s3-dev --target buildfs
python3 -m platformio run --environment esp32-s3-dev --target uploadfs
python3 -m platformio run --environment esp32-s3-dev --target upload
python3 -m platformio run --environment esp32-s3-release
```

The last command verifies the release build; add `--target upload` only when intentionally deploying
it. Use `--environment esp32-s3-release` only for deployment testing. Upload the filesystem before
firmware on an initially blank device. `uploadfs` replaces the
filesystem contents. Serial logs at 115200 baud report rejected sizes, the selected path, display
result, persisted next index, and sleep entry. Panel appearance, rail sequencing, refresh timing,
and deep-sleep wake behavior still require verification on the actual production board.
