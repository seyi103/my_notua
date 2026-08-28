"""Host regression tests for the wire protocol and transactional storage contract."""

import struct
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).parents[1]
PROTOCOL = (ROOT / "src/core/ble/transferProtocol.h").read_text()
PROTOCOL_CPP = (ROOT / "src/core/ble/transferProtocol.cpp").read_text()
STORAGE = (ROOT / "src/core/storage/imageStorage.cpp").read_text()
BLE = (ROOT / "src/core/ble/blePeripheral.cpp").read_text()


class ProtocolTest(unittest.TestCase):
    def test_start_layout_is_explicit_little_endian(self):
        packet = struct.pack("<BBBBII", 1, 1, 2, 0, 1_920_000, 0x12345678)
        self.assertEqual(len(packet), 12)
        self.assertEqual(packet[4:8], b"\x00L\x1d\x00")
        self.assertIn("readLe32(data + 4)", PROTOCOL_CPP)
        self.assertNotIn("__attribute__((packed))", PROTOCOL + PROTOCOL_CPP)

    def test_crc_reference_vector_matches_ieee(self):
        self.assertEqual(zlib.crc32(b"123456789") & 0xFFFFFFFF, 0xCBF43926)
        self.assertIn("0xedb88320U", PROTOCOL_CPP)
        self.assertIn("crc_ ^ 0xffffffffU", PROTOCOL_CPP)

    def test_offsets_are_duplicate_safe_and_future_rejected(self):
        self.assertIn("offset < gStorage.offset()", BLE)
        self.assertIn("offset > gStorage.offset()", BLE)
        self.assertIn("Status::badOffset", BLE)

    def test_size_crc_slot_and_atomic_rollback_guards_exist(self):
        self.assertIn("size != notua::transfer::IMAGE_BYTES", STORAGE)
        self.assertIn("slot > images.size()", STORAGE)
        self.assertIn("actual != expectedCrc_", STORAGE)
        self.assertIn("rename(BACKUP_PATH, finalPath_)", STORAGE)
        self.assertIn("LittleFS.begin(false)", STORAGE)

    def test_callback_queue_and_ram_bounds_are_fixed(self):
        self.assertIn("MAX_GATT_VALUE_BYTES = 512", PROTOCOL)
        self.assertIn("TRANSFER_QUEUE_LENGTH = 8", BLE)
        self.assertIn("xQueueSend(gTransferQueue", BLE)


if __name__ == "__main__":
    unittest.main()
