# Notua BLE EPD firmware architecture

## Dependency audit

The imported T2001 EPD implementation is not a standalone driver. Its actual local dependency
chain is:

1. `t2001_service` owns serialization, retries, and the complete display transaction.
2. `t2001`, `t2001_mailbox`, `t2001_transport_spi`, and `t2001_render` implement the TCON
   protocol and 8-bpp streaming over Arduino `SPI`.
3. `boardPower` and `pins` provide the ordered 1.25 V, 1.8 V, 3.3 V, and 6.5 V rail control.
4. `watchdog` and `longOpPump` keep the ESP task watchdog and scheduler alive during long
   transfers and panel refreshes.
5. `diag/log` is the only diagnostic dependency and uses the USB serial port.

The earlier PlatformIO configuration also declared MQTT and ArduinoJson libraries, despite the
EPD path not using them. Those declarations are absent in the current configuration. The minimal
firmware does not include or initialize WiFi, MQTT, HTTP, or OTA, and its partition table contains
one factory application rather than OTA slots.

## Minimal runtime

```text
BLE GATT frame transfer
        |
        v
LittleFS /frame.tmp --atomic rename--> /frame.bin (metadata + pixels)
        |
        v (Arduino loop, never the BLE callback)
EPD service -> render -> SPI/mailbox -> T2001
        |                              |
        +-> watchdog/longOpPump        +-> boardPower
```

BLE is transport-only. A client writes a 9-byte little-endian control packet
`[0x01, width:u16, height:u16, byte_count:u32]`, writes raw 8-bpp pixels to the data
characteristic, then writes `[0x02]` to commit. `[0x03]` aborts an upload. The committed byte
count must equal `width * height`; the EPD service additionally rejects dimensions whose byte
count does not match the connected panel.

The BLE callback only persists and queues a completed frame. Rendering is deferred to the main
loop because an EPD refresh is long-running. On completion, the panel-power mailbox command,
SPI service, and all T2001 rails are shut down. LittleFS keeps the last committed frame across
resets; a failed or interrupted upload never replaces it.
