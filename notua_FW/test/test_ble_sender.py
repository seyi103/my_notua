"""Behavioral tests for notification-timeout status-read recovery."""

import asyncio
import importlib
import struct
import sys
import tempfile
import types
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

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


class StallingWriteClient:
    async def write_gatt_char(self, uuid, payload, response=False):
        assert uuid == sender.DATA
        assert response is False
        await asyncio.sleep(1)


class FakeCharacteristic:
    def __init__(self, values):
        self.values = iter(values)
        self.current = 20

    @property
    def max_write_without_response_size(self):
        self.current = next(self.values, self.current)
        return self.current


class SenderRecoveryTest(unittest.IsolatedAsyncioTestCase):
    async def test_max_write_is_immediately_negotiated(self):
        characteristic = FakeCharacteristic([244])
        self.assertEqual(await sender.wait_for_max_write(characteristic, timeout=0.01,
                                                        poll_interval=0.001), 244)

    async def test_max_write_poll_observes_negotiated_update(self):
        characteristic = FakeCharacteristic([20, 20, 244])
        self.assertEqual(await sender.wait_for_max_write(characteristic, timeout=0.1,
                                                        poll_interval=0.001), 244)

    async def test_max_write_default_times_out_instead_of_starting_transfer(self):
        characteristic = FakeCharacteristic([20])
        with self.assertRaisesRegex(RuntimeError, "remained at Bleak's default 20"):
            await sender.wait_for_max_write(characteristic, timeout=0.003, poll_interval=0.001)

    async def test_notification_timeout_reads_once_and_returns_persisted_offset(self):
        queue = asyncio.Queue()
        client = FakeClient(struct.pack("<BBHII", 1, 2, 0, 4096, 0))
        status, recovered = await sender.wait_for_status(queue, client, timeout=0.001)
        self.assertEqual(status, (2, 4096, 0))
        self.assertTrue(recovered)
        self.assertEqual(client.reads, 1)

    async def test_notification_and_fallback_counters(self):
        diagnostics = sender.TransferDiagnostics(notifications=3)
        client = FakeClient(struct.pack("<BBHII", 1, 2, 0, 4096, 0))
        status, recovered = await sender.wait_for_status(
            asyncio.Queue(), client, timeout=0.001, diagnostics=diagnostics)
        self.assertEqual(status, (2, 4096, 0)); self.assertTrue(recovered)
        diagnostics.record_ack(0.125)
        self.assertEqual(diagnostics.notifications, 3)
        self.assertEqual(diagnostics.notification_timeouts, 1)
        self.assertEqual(diagnostics.status_reads, 1)
        self.assertEqual(diagnostics.ack_count, 1)
        self.assertEqual(diagnostics.ack_latency_max, 0.125)

    async def test_notification_does_not_read_characteristic(self):
        queue = asyncio.Queue()
        queue.put_nowait((2, 8192, 0))
        client = FakeClient(b"")
        status, recovered = await sender.wait_for_status(queue, client, timeout=0.01)
        self.assertEqual(status, (2, 8192, 0))
        self.assertFalse(recovered)
        self.assertEqual(client.reads, 0)

    async def test_data_write_timeout_reports_transfer_context(self):
        diagnostics = sender.TransferDiagnostics(notifications=7, notification_timeouts=2, status_reads=2)
        with self.assertRaises(RuntimeError) as raised:
            await sender.write_data_packet(
                StallingWriteClient(), b"x" * 512, 4064, 4064, 8128, diagnostics, timeout=0.001)
        message = str(raised.exception)
        self.assertIn("DATA write timeout", message)
        self.assertIn("packet_offset=4064", message)
        self.assertIn("packet_length=512", message)
        self.assertIn("window_start=4064", message)
        self.assertIn("window_target=8128", message)
        self.assertIn("notifications=7", message)
        self.assertIn("notification_timeouts=2", message)
        self.assertIn("fallback_status_reads=2", message)

    async def test_diagnostic_summary_printed_on_normal_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "image.bin"
            image_path.write_bytes(b"\0" * sender.IMAGE_BYTES)
            args = SimpleNamespace(file=[image_path], name=None, revision=None, interval=300,
                                   allow_slow_write=False)
            with mock.patch.object(sender, "find_device", new=mock.AsyncMock(side_effect=RuntimeError("boom"))), \
                    mock.patch.object(sender.TransferDiagnostics, "print_summary") as summary:
                with self.assertRaisesRegex(RuntimeError, "sync interrupted: boom"):
                    await sender.synchronize(args)
                summary.assert_called_once()

    async def test_diagnostic_summary_printed_and_cancellation_not_wrapped(self):
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "image.bin"
            image_path.write_bytes(b"\0" * sender.IMAGE_BYTES)
            args = SimpleNamespace(file=[image_path], name=None, revision=None, interval=300,
                                   allow_slow_write=False)
            with mock.patch.object(sender, "find_device", new=mock.AsyncMock(side_effect=asyncio.CancelledError())), \
                    mock.patch.object(sender.TransferDiagnostics, "print_summary") as summary:
                with self.assertRaises(asyncio.CancelledError):
                    await sender.synchronize(args)
                summary.assert_called_once()

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

    def test_resume_requires_complete_target_equality(self):
        images = [sender.Image(Path("a"), b"", 10, 2), sender.Image(Path("b"), b"", 20, 4)]
        encoded = sender.encode_playlist(7, images, 300)
        stored = sender.decode_playlist(encoded)
        self.assertTrue(sender.playlist_matches(encoded, stored))
        variants = []
        reversed_images = list(reversed(images))
        variants.append(sender.encode_playlist(7, reversed_images, 300))
        variants.append(sender.encode_playlist(7,
            [sender.Image(Path("a"), b"", 11, 2), images[1]], 300))
        variants.append(sender.encode_playlist(7, images[:1], 300))
        variants.append(sender.encode_playlist(7, images, 301))
        for different in variants:
            self.assertFalse(sender.playlist_matches(different, stored))

    def test_interval_bounds_constants_match_firmware_policy(self):
        self.assertEqual(sender.MIN_INTERVAL_SECONDS, 60)
        self.assertEqual(sender.DEFAULT_INTERVAL_SECONDS, 300)
        self.assertEqual(sender.MAX_INTERVAL_SECONDS, 86400)
        image = sender.Image(Path("a"), b"", 1, 0)
        with self.assertRaisesRegex(ValueError, "interval"):
            sender.encode_playlist(1, [image], 59)
        with self.assertRaisesRegex(ValueError, "interval"):
            sender.encode_playlist(1, [image], 86401)

    def test_partial_catalog_reconnect_skips_completed_slot_and_resumes_rest(self):
        requested = [sender.Image(Path("new-a"), b"", 1000, 1),
                     sender.Image(Path("new-b"), b"", 2000, 3)]
        target_bytes = sender.encode_playlist(12, requested, 300)
        active = sender.encode_playlist(11,
            [sender.Image(Path("old-a"), b"", 10, 1),
             sender.Image(Path("old-b"), b"", 20, 3)], 300)
        entries = bytearray()
        current = {0: 0, 1: 1000, 2: 0, 3: 20, 4: 0}
        for slot in range(5):
            valid = current[slot] != 0
            entries += struct.pack("<BBII", slot, 3 if valid else 0,
                                   sender.IMAGE_BYTES if valid else 0, current[slot])
        packet = bytes([1, 5, 1, 1 << 1]) + entries + active + target_bytes
        catalog = sender.decode_catalog(packet)
        self.assertTrue(sender.playlist_matches(target_bytes, catalog["target"]))
        for image in requested:
            image.slot = dict(zip(catalog["target"]["crcs"],
                                  catalog["target"]["slots"]))[image.crc]
        uploads = [image for image in requested
                   if catalog["entries"][image.slot]["crc"] != image.crc]
        self.assertEqual([image.slot for image in uploads], [3])


if __name__ == "__main__":
    unittest.main()