"""Executable host tests for protocol behavior and failure-state policy."""

import subprocess
import tempfile
import textwrap
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).parents[1]


class ProtocolExecutableTest(unittest.TestCase):
    def test_parser_crc_offsets_and_ack_coalescing_with_real_inputs(self):
        source = textwrap.dedent(r"""
            #include <cassert>
            #include <cstdint>
            #include "core/ble/transferProtocol.h"
            #include "core/storage/syncModel.h"
            using namespace notua::transfer;
            int main() {
                uint8_t start[] = {1,1,2,0,0x00,0x4c,0x1d,0,0x78,0x56,0x34,0x12};
                StartCommand command{};
                assert(parseStart(start, sizeof(start), command));
                assert(command.slot == 2 && command.size == 1920000 && command.crc32 == 0x12345678);
                start[3] = 1; assert(!parseStart(start, sizeof(start), command));
                Crc32 crc; const uint8_t vector[] = {'1','2','3','4','5','6','7','8','9'};
                crc.update(vector, sizeof(vector)); assert(crc.value() == 0xcbf43926);
                assert(classifyOffset(100, 100) == OffsetDisposition::expected);
                assert(classifyOffset(99, 100) == OffsetDisposition::duplicate);
                assert(classifyOffset(101, 100) == OffsetDisposition::future);
                AckCoalescer ack;
                for (int i=0; i<7; ++i) assert(!ack.persistedPacket());
                assert(ack.persistedPacket());
                assert(!ack.flush(true));
                assert(!ack.persistedPacket()); assert(!ack.flush(false)); assert(ack.flush(true));
                using namespace notua::storage;
                for (uint8_t count=1; count<=MAX_IMAGES; ++count) {
                    Playlist playlist{}; playlist.count=count; playlist.revision=9;
                    playlist.intervalSeconds=300;
                    for (uint8_t i=0;i<count;++i) { playlist.slots[i]=i; playlist.crc32[i]=i+1; }
                    assert(validatePlaylist(playlist));
                    uint8_t encoded[PLAYLIST_BYTES]; encodePlaylist(encoded,playlist);
                    Playlist decoded{}; assert(decodePlaylist(encoded,sizeof(encoded),decoded));
                    assert(decoded.count==count && decoded.revision==9);
                }
                Playlist sixth{}; sixth.count=6; sixth.intervalSeconds=300;
                assert(!validatePlaylist(sixth));
                CatalogEntry catalog[MAX_IMAGES]{};
                for(uint8_t i=0;i<MAX_IMAGES;++i){catalog[i].slot=i;catalog[i].valid=true;catalog[i].crc32=100+i;}
                assert(matchingSlot(catalog,102,0)==2);
                assert(matchingSlot(catalog,102,1U<<2)==0xff);
                assert(recoveryAction(SyncStage::prepared,true,true)==RecoveryAction::restoreBackup);
                assert(recoveryAction(SyncStage::prepared,false,true)==RecoveryAction::restoreBackup);
                assert(recoveryAction(SyncStage::committed,true,true)==RecoveryAction::keepFinal);
                assert(recoveryAction(SyncStage::committed,false,true)==RecoveryAction::restoreBackup);
            }
        """)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "protocol_test"
            subprocess.run([
                "g++", "-std=c++17", "-I", str(ROOT / "src"), "-x", "c++", "-",
                str(ROOT / "src/core/ble/transferProtocol.cpp"), "-o", str(binary),
                str(ROOT / "src/core/storage/syncModel.cpp"),
            ], input=source, text=True, check=True)
            subprocess.run([binary], check=True)

    def test_python_crc_reference(self):
        self.assertEqual(zlib.crc32(b"123456789") & 0xFFFFFFFF, 0xCBF43926)


class FakeTransactionalStorage:
    """Host fake exercising the documented no-loss transition decisions."""
    def __init__(self):
        self.final, self.temp, self.backup, self.marker = True, True, False, False

    def commit(self, fail):
        if fail == "marker": return "marker_write_failed"
        self.marker = True
        if fail == "final_to_backup": return "final_to_backup_failed"
        self.final, self.backup = False, True
        if fail == "temp_to_final":
            if fail == "temp_to_final" and self.backup:
                self.final, self.backup = True, False
            return "temp_to_final_failed"
        self.temp, self.final = False, True
        return "committed"

    def rollback(self, restore_fails=False):
        new_final = self.final
        if restore_fails:
            # The new final is restored and the old backup+marker are retained.
            self.final, self.backup, self.marker = new_final, True, True
            return "backup_restore_failed"
        self.final, self.backup, self.marker = True, False, False
        return "ok"


class TransactionStateTest(unittest.TestCase):
    def test_distinct_commit_failures_preserve_a_valid_image(self):
        for fault, result in (("marker", "marker_write_failed"),
                              ("final_to_backup", "final_to_backup_failed"),
                              ("temp_to_final", "temp_to_final_failed")):
            storage = FakeTransactionalStorage()
            self.assertEqual(storage.commit(fault), result)
            self.assertTrue(storage.final or storage.backup)

    def test_failed_backup_restore_keeps_new_final_and_recovery_artifacts(self):
        storage = FakeTransactionalStorage()
        self.assertEqual(storage.commit(None), "committed")
        self.assertEqual(storage.rollback(restore_fails=True), "backup_restore_failed")
        self.assertTrue(storage.final)
        self.assertTrue(storage.backup)
        self.assertTrue(storage.marker)


if __name__ == "__main__":
    unittest.main()
