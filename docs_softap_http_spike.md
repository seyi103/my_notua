# BLE-assisted SoftAP/HTTP hardware spike

This is an experimental transport, not a production migration. The normal BLE DATA characteristic remains intact. `esp32-s3-dev` enables the spike; `esp32-s3-release` disables it. The stable SSID is `NOTUA-<last-24-bits-of-device-id>` and the development-only WPA2 password is `notua-dev-2026`. It is deliberately documented and must not be treated as production security.

BLE service `7d2a4b70-8e67-4d8b-9f3a-36c89e210001` exposes read-only AP JSON on characteristic `...0007`. Writing ASCII `START_AP` to control characteristic `...0003` starts the AP. HTTP provides `GET /health` and `PUT /images/{candidate-unused-slot}`; the slot is checked against the active playlist, but data is committed only to `/images/softap_spike.bin`, never a production slot; upload requests require an exact 1,920,000-byte `Content-Length` and hexadecimal CRC32 in `X-Notua-CRC32`.

## Exact manual hardware acceptance test (still required)

1. Flash the development firmware to a real ESP32-S3, wake the button/EXT1 BLE path, and keep its serial log open.
2. On a real Android phone, leave **mobile data ON** and begin on normal Wi-Fi or mobile data. Install the debug APK and grant Bluetooth/Nearby Wi-Fi permissions.
3. Prepare an existing Y8 BIN and independently verify it is exactly 1,920,000 bytes and record its CRC32.
4. Tap **Select Y8 BIN**, **Connect to Notua**, and approve Android's exact `NOTUA-...` network request on the first attempt. Confirm the BLE-reported SSID is the selected AP.
5. Tap **Upload**. Keep mobile data enabled. Choose a candidate slot not present in the active playlist. Confirm logs show the AP client and that candidate endpoint, progress never stalls, and Flutter reports under 30 seconds and at least 100 KiB/s average.
6. Confirm the structured HTTP response reports 1,920,000 bytes and `crcMatch:true`; confirm firmware reports CRC match/commit to `/images/softap_spike.bin`. Confirm every `/images/slot_0.bin` through `/images/slot_4.bin` remains unchanged. A request naming an active slot must return `active-slot`.
7. Independently read or checksum `/images/softap_spike.bin` and confirm its size and CRC; do not modify or apply the production playlist, and do not use EPD display as an acceptance criterion.
8. Confirm the AP/network request is automatically released after success and the phone resumes its prior network. Verify unrelated Internet traffic stayed on mobile/normal connectivity and no upload request escaped through it (the native upload must use the callback's exact `Network.openConnection`).
9. Wake/start the AP again and repeat the upload. Record whether Android reconnects without another approval, subject to Android policy.

Do **not** claim viability until all steps pass on hardware. Automated builds only establish compilation. Viability requires no routing escape, no stall, <30 seconds, >=100 KiB/s, correct CRC in the dedicated spike file, and network cleanup. Photo selection and local dithering are out of scope and will be added later; this screen accepts only an existing 1,920,000-byte Y8 BIN.
