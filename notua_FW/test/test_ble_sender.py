"""Behavioral tests for notification-timeout status-read recovery."""

import asyncio
import importlib
import struct
import sys
import types
import unittest

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


if __name__ == "__main__":
    unittest.main()
