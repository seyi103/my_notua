#!/usr/bin/env python3
"""Send one pre-generated 1600x1200 Y8 frame to a Notua over BLE."""

from __future__ import annotations

import argparse
import asyncio
import struct
import time
import zlib
from pathlib import Path

from bleak import BleakClient, BleakScanner

SERVICE = "7d2a4b70-8e67-4d8b-9f3a-36c89e210001"
CONTROL = "7d2a4b70-8e67-4d8b-9f3a-36c89e210003"
DATA = "7d2a4b70-8e67-4d8b-9f3a-36c89e210004"
STATUS = "7d2a4b70-8e67-4d8b-9f3a-36c89e210005"
IMAGE_BYTES = 1_920_000
WINDOW = 8

NAMES = {1: "START_ACCEPTED", 2: "ACK", 3: "COMMITTED", 4: "APPLYING",
         0x80: "BAD_COMMAND", 0x81: "BAD_SIZE", 0x82: "BAD_OFFSET",
         0x83: "QUEUE_FULL", 0x84: "CRC_MISMATCH", 0x85: "STORAGE_ERROR",
         0x86: "NOT_READY"}


def decode_status(raw: bytes) -> tuple[int, int, int]:
    if len(raw) != 12:
        raise RuntimeError(f"status value is {len(raw)} bytes, expected 12")
    version, code, _reserved, expected, detail = struct.unpack("<BBHII", raw)
    if version != 1:
        raise RuntimeError(f"status version was {version}, expected 1")
    return code, expected, detail


async def wait_for_status(notifications, client, timeout: float = 15.0):
    """Wait for notify, then perform exactly one status read as loss recovery."""
    try:
        return await asyncio.wait_for(notifications.get(), timeout), False
    except asyncio.TimeoutError:
        status = decode_status(bytes(await client.read_gatt_char(STATUS)))
        print(f"\nNotification timeout; recovered {NAMES.get(status[0], hex(status[0]))} "
              f"at {status[1]:,} by read")
        return status, True


async def find_device(name: str | None):
    print(f"Scanning for {name or 'Notua'} / {SERVICE} ...")
    devices = await BleakScanner.discover(timeout=10.0, return_adv=True)
    for device, advertisement in devices.values():
        uuids = {value.lower() for value in advertisement.service_uuids}
        if (name and device.name == name) or (not name and (device.name == "Notua" or SERVICE in uuids)):
            return device
    raise RuntimeError("Notua not found by device name or Service UUID")


async def send(args: argparse.Namespace) -> None:
    payload = args.file.read_bytes()
    if len(payload) != IMAGE_BYTES:
        raise ValueError(f"file is {len(payload):,} bytes; exactly {IMAGE_BYTES:,} required")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    print(f"File: {args.file} ({len(payload):,} bytes), CRC32={crc:08x}, slot={args.slot}")
    device = await find_device(args.name)
    notifications: asyncio.Queue[tuple[int, int, int]] = asyncio.Queue()

    def notified(_sender, value: bytearray) -> None:
        try:
            notifications.put_nowait(decode_status(bytes(value)))
        except RuntimeError as error:
            print(f"\nIgnored malformed status notification: {error}")

    try:
        async with BleakClient(device, timeout=20.0) as client:
            await client.start_notify(STATUS, notified)
            characteristic = client.services.get_characteristic(DATA)
            maximum = characteristic.max_write_without_response_size
            chunk_size = min(maximum, 512) - 4
            if chunk_size <= 0:
                raise RuntimeError(f"invalid write-without-response capacity: {maximum}")
            print(f"Connected; max write without response={maximum}, payload chunk={chunk_size}")

            async def command(packet: bytes, wanted: set[int]) -> tuple[int, int, int]:
                while not notifications.empty(): notifications.get_nowait()
                await client.write_gatt_char(CONTROL, packet, response=True)
                while True:
                    status, _recovered = await wait_for_status(notifications, client)
                    if status[0] in wanted or status[0] >= 0x80:
                        return status

            status = await command(struct.pack("<BBBBII", 1, 1, args.slot, 0, len(payload), crc), {1})
            if status[0] != 1: raise RuntimeError(f"START failed: {NAMES.get(status[0])}, detail={status[2]}")

            offset = retries = 0
            started = last_report = time.monotonic()
            while offset < len(payload):
                window_start = offset
                packets = []
                for _ in range(WINDOW):
                    if offset >= len(payload): break
                    part = payload[offset:offset + chunk_size]
                    packets.append((offset, part)); offset += len(part)
                for packet_offset, part in packets:
                    await client.write_gatt_char(DATA, struct.pack("<I", packet_offset) + part, response=False)
                target = offset
                while True:
                    (code, expected, detail), recovered = await wait_for_status(notifications, client)
                    if code == 2 and expected >= target: break
                    if recovered and code == 2:
                        retries += 1; offset = expected
                        print(f"Status read recovery: retry from {expected:,} (attempt {retries})")
                        break
                    if code in (0x82, 0x83):
                        retries += 1; offset = expected
                        print(f"\n{NAMES[code]}: retry from {expected:,} (attempt {retries})")
                        break
                    if code >= 0x80:
                        raise RuntimeError(f"DATA failed: {NAMES.get(code, hex(code))}; expected={expected}, detail={detail}")
                if offset == window_start:
                    await asyncio.sleep(0.05)
                now = time.monotonic()
                if now - last_report >= 0.5 or offset == len(payload):
                    rate = offset / max(now - started, 0.001) / 1024
                    print(f"\r{offset / len(payload):6.1%}  {rate:8.1f} KiB/s  retries={retries}", end="", flush=True)
                    last_report = now
            print()
            status = await command(b"\x01\x02", {3})
            if status[0] != 3:
                raise RuntimeError(f"FINISH failed: {NAMES.get(status[0])}, device CRC/detail={status[2]:08x}")
            print(f"COMMITTED: {status[1]:,} bytes; CRC32 verified ({crc:08x})")
            status = await command(b"\x01\x04", {4})
            if status[0] != 4: raise RuntimeError(f"APPLY failed: {NAMES.get(status[0])}")
            print("APPLYING: device will disconnect and restart")
    except Exception as error:
        raise RuntimeError(f"BLE transfer interrupted: {error}. Reconnect and restart from START; resume is not supported.") from error


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--slot", required=True, type=int, choices=range(3))
    parser.add_argument("--name", help="exact BLE device name (default: Notua or service UUID)")
    asyncio.run(send(parser.parse_args()))


if __name__ == "__main__":
    main()
