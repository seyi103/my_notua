"""Behavioral tests for notification-timeout status-read recovery."""

import asyncio
import importlib
import struct
import sys
import types
import unittest
from pathlib import Path

fake_bleak = types.ModuleType("bleak")
fake_bleak.BleakClient = object
fake_bleak.BleakScanner = object
sys.modules.setdefault("bleak", fake_bleak)
sender = importlib.import_module("scripts.ble_send_test")


class FakeClient:
    def __init__(self, status):
        self.status = status
        self.reads = 0

    async def read_gatt_char(self, uuid):
        self.reads += 1
        assert uuid == sender.STATUS
        return self.status


class SenderRecoveryTest(unittest.IsolatedAsyncioTestCase):
    async def test_notification_timeout_reads_once_and_returns_persisted_offset(self):
        queue = asyncio.Queue()
        client = FakeClient(struct.pack("<BBHII", 1, 2, 0, 4096, 0))
        status, recovered = await sender.wait_for_status(queue, client, timeout=0.001)
        self.assertEqual(status, (2, 4096, 0))
        self.assertTrue(recovered)
        self.assertEqual(client.reads, 1)

    async def test_notification_does_not_read_characteristic(self):
        queue = asyncio.Queue()
        queue.put_nowait((2, 8192, 0))
        client = FakeClient(b"")
        status, recovered = await sender.wait_for_status(queue, client, timeout=0.01)
        self.assertEqual(status, (2, 8192, 0))
        self.assertFalse(recovered)
        self.assertEqual(client.reads, 0)

    def test_malformed_status_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "expected 12"):
            sender.decode_status(b"short")

    def test_crc_reuse_order_only_and_one_change_plans(self):
        catalog = {"entries": [
            {"slot": slot, "valid": True, "crc": 100 + slot} for slot in range(5)
        ]}
        reordered = [sender.Image(Path(str(crc)), b"", crc) for crc in (104, 102, 100)]
        sender.assign_slots(reordered, catalog)
        self.assertEqual([image.slot for image in reordered], [4, 2, 0])
        self.assertTrue(all(catalog["entries"][image.slot]["crc"] == image.crc for image in reordered))
        one_changed = [sender.Image(Path(str(crc)), b"", crc) for crc in (100, 101, 999)]
        sender.assign_slots(one_changed, catalog)
        uploads = [image for image in one_changed
                   if catalog["entries"][image.slot]["crc"] != image.crc]
        self.assertEqual(len(uploads), 1)

    def test_sixth_image_playlist_is_rejected(self):
        images = [sender.Image(Path(str(i)), b"", i + 1, i) for i in range(6)]
        with self.assertRaisesRegex(ValueError, "1-5"):
            sender.encode_playlist(1, images, 300)


if __name__ == "__main__":
    unittest.main()
