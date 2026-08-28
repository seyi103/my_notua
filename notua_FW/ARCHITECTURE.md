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
recovers interrupted storage transactions, loads the active playlist of up to five fixed physical slots,
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

A GPIO16 EXT1 wake mounts LittleFS solely to run mandatory transaction recovery and migration before
catalog access; it does not increment the image index, power the EPD, or refresh it. It then initializes
a peripheral named `Notua`, advertises the stable primary Service UUID
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
(`...0004`) is Write Without Response, Transfer Status (`...0005`) is Read + Notify, and Catalog
(`...0006`) is Read. The full
UUID prefix is `7d2a4b70-8e67-4d8b-9f3a-36c89e21`; the suffix notation is only shorthand here.

All integers are explicitly encoded little-endian; no packed C structure defines the wire format.

| Packet | Exact byte layout |
| --- | --- |
| START (12 bytes) | `u8 version=1, u8 opcode=0x01, u8 slot, u8 reserved=0, u32 size, u32 crc32` |
| FINISH | `u8 version=1, u8 opcode=0x02` |
| ABORT | `u8 version=1, u8 opcode=0x03` |
| APPLY | `u8 version=1, u8 opcode=0x04` |
| SYNC_BEGIN (37 bytes) | `u8 version=1, u8 opcode=0x10, PLAYLIST[35]` |
| PLAYLIST_COMMIT | `u8 version=1, u8 opcode=0x11` |
| GET_CATALOG | `u8 version=1, u8 opcode=0x12` |
| DATA | `u32 offset, u8 payload[]`; total characteristic value is 5..512 bytes |
| STATUS (12 bytes) | `u8 version=1, u8 code, u16 reserved=0, u32 next_expected_offset, u32 detail` |

Status codes are fixed as follows: `0x01 START_ACCEPTED`, `0x02 ACK`, `0x03 COMMITTED`,
`0x04 APPLYING`, `0x05 PLAYLIST_COMMITTED`, `0x06 SYNC_ACCEPTED`, `0x80 BAD_COMMAND`,
`0x81 BAD_SIZE`, `0x82 BAD_OFFSET`, `0x83 QUEUE_FULL`,
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

## Fixed catalog, playlist, capacity, and recovery

The LittleFS partition is `0xCE0000` = 13,500,416 bytes. Five active frames consume 9,600,000 bytes;
one upload temp consumes 1,920,000 bytes, for a maximum frame-file total of 11,520,000 bytes. START
also requires a 128 KiB explicit filesystem safety margin and logs total, used, free, required, and
margin. Physical paths are exactly `/images/slot_0.bin` through `/images/slot_4.bin`; the sole temp is
`/images/notua_upload.tmp`. Valid legacy `.bin` files such as `pattern_01.bin` are sorted and renamed
only into empty fixed slots. Unmapped or unknown files are never deleted.

The 124-byte Catalog value is `version, slot_count=5, sync_stage, completed_bitmap`, followed by five
10-byte entries (`slot, flags(exists|valid), size u32, crc32 u32`), active PLAYLIST, and target
PLAYLIST. PLAYLIST is 35 bytes: `version, count, revision u32, interval_seconds u32, slots[5],
crc32[5]`. Count is 1..5, slots are unique, and interval is bounded to 60..86,400 seconds with a
300-second default. The firmware decoder and Python CLI enforce the same bounds. Display
selection uses active playlist order, never filename order.

SYNC_BEGIN durably saves `sync_in_progress`, target/previous playlists, prior pending value,
completed bitmap, and PREPARED stage. Existing CRCs in any physical slot are reused. Only target
slots whose CRC differs use START/DATA/FINISH. Each slot transaction writes marker `P<slot>`, renames
old final to backup, and renames temp to final. After completed bitmap is durable, marker becomes
`C<slot>` and backup/marker are cleaned. PREPARED recovery restores backup; COMMITTED recovery keeps
new final. If restoration fails, backup and marker remain. If target and backup coexist, marker stage
selects the winner. Recovery runs after every mount and before every catalog/playlist read.

PLAYLIST_COMMIT rescans all five slots and atomically writes the target playlist only if every target
size/CRC matches. NVS failure restores the saved previous playlist and previous pending value.
`sync_in_progress` remains true until active playlist and pending are durable, so timer boots retain
the existing EPD contents and never display a partial sync. Disconnect removes only unfinished temp;
target, completed bitmap, and committed slots remain resumable. During resume, active playlist
metadata is returned without comparing its intentionally stale CRCs to partially replaced slots;
target completion alone is reconciled against the physical catalog. The sender resumes only when
the entire encoded target (version, revision, count, ordered slots/CRCs, and interval) matches. A
same-revision mismatch is rejected and the original sync must be completed first.

APPLY is accepted once after the single final playlist commit. On reconnect it is accepted only when
sync is no longer in progress and both the committed-playlist state and durable pending marker agree;
an old pending value can never make an incomplete sync APPLY-ready.

## Incomplete-sync power policy

Release builds encountering `sync_in_progress` preserve all metadata and the existing physical EPD
contents, enable GPIO16 `ESP_EXT1_WAKEUP_ANY_HIGH`, and enter EXT1-only deep sleep with no timer.
The user resumes via the externally driven idle-LOW/press-HIGH button. This is separate from the
normal playlist interval (300 seconds by default) and ordinary failures' 60-second retry timer.
Development builds remain awake with USB Serial diagnostics as before.

The same sync-wait policy is selected for BLE inactivity, advertising/runtime failure, or other BLE
session exits after persistent `sync_in_progress` is captured and before BLE teardown. A normal BLE
timeout without an incomplete sync retains the existing five-minute timer policy. Wake-source
selection is explicit: successful slideshow sleep uses configured playlist timer + EXT1; ordinary
error retry uses the retry timer + EXT1 when available; incomplete sync uses EXT1 only after GPIO16
returns LOW. If GPIO16 remains HIGH for the release timeout (or EXT1 configuration fails), EXT1 is
disabled and a five-minute fallback timer is armed. Thus the stuck-button protection cannot enter a
zero-wake-source sleep.

## Python test sender

Install Python 3.10+ and Bleak with `python -m pip install bleak`. On Windows, use a machine with
Bluetooth LE enabled and run in PowerShell; on macOS, grant the terminal Bluetooth permission in
System Settings. From `notua_FW`, run:

```sh
python scripts/ble_send_test.py \
  --file data/images/test_01.bin --file data/images/test_02.bin --file data/images/test_03.bin
```

The ordered `--file` option is repeated one to five times. The sender scans by `Notua` or the service
UUID, computes size and `zlib.crc32`, reads catalog plus active/target playlists, reuses equal CRCs in
any slot, uploads only changed images, commits the playlist, and sends APPLY exactly once. It prints
SKIP versus UPLOAD decisions. Re-running after disconnect uses durable target/completed state and
does not resend finished slots. It
uses the discovered Data characteristic's `max_write_without_response_size` minus the four-byte
offset (also capped to the firmware's 512-byte GATT value limit), subscribes before START, and prints
progress, rate, retry count, and final CRC. A disconnect is fatal and explicitly instructs the user
to reconnect and restart from START. Because NimBLE 1.4.3 has a void `notify()` API, firmware returns
whether it could synchronously issue notify while connected and logs failures; the characteristic's
read value is updated first in every case. On a notification timeout the sender reads Transfer Status
once, recovers its persisted offset, and retransmits from that offset.

## Transfer board verification (not CI)

CI/host checks compile and execute the real protocol, sync validation, APPLY policy, and recovery
transition algorithm with injected operation failures; Python tests execute complete-target matching,
incremental slot planning, and notification timeout recovery. LittleFS and NVS driver integration plus
power interruption remain hardware tests. Hardware
acceptance must separately migrate legacy patterns, sync 1 then 5 frames, reorder without DATA,
replace one frame, disconnect before FINISH, and cut power at marker, both renames, completed-bitmap,
marker-commit, playlist, pending, and cleanup boundaries. Confirm PREPARED restores old, COMMITTED
keeps new, an incomplete sync never refreshes EPD, reconnect skips completed CRCs, and exactly one
PLAYLIST_COMMITTED precedes one APPLYING. Finally allow five timer wakes to confirm playlist order and
the configured 300-second default interval.

Exact repository checks are:

```sh
python3 -m unittest discover -s test -v
python3 -m py_compile scripts/ble_send_test.py scripts/generate_test_images.py
python3 -m platformio run -e esp32-s3-dev -e esp32-s3-release
python3 -m platformio run -e esp32-s3-dev -t buildfs
```

Before any EXT1-capable release sleep, firmware waits at most 10 seconds for GPIO16 LOW while feeding
the watchdog. A stuck-HIGH input disables EXT1 for that sleep and logs the five-minute fallback timer.
The development build continues the established no-deep-sleep policy, including after BLE timeout.
Consequently button-wake sleep/re-wake acceptance testing must use the release environment.

## Button/BLE production-board test

1. Build and flash `esp32-s3-release`, retain the already verified LittleFS images, and monitor USB
   serial at 115200 baud.
2. Allow a normal power-on boot to display a frame and sleep. Confirm its route is `existing_policy`,
   then confirm the next timer wake is `timer_photo_cycle` and advances exactly one image.
3. While asleep, drive GPIO16 HIGH with the physical button, then release it. Confirm an EXT1 wake
   mask containing bit 16 (`0x10000`), route `button_ble`, successful BLE initialization, and
   `advertising_started=true`. The mandatory LittleFS recovery mount is expected; confirm there is no
   NVS slideshow-index update, panel power-up, or EPD-refresh log.
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
   playlist frame. Injected release failures must flush the final error, sleep for one minute, then wake
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
`0x10`, and `0x30`. Generate up to five deterministic pattern files from the firmware directory:

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
