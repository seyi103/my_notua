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
            #include "core/power/wakePolicy.h"
            using namespace notua::transfer;
            struct FakeRecovery : notua::storage::RecoveryIo {
                bool target=true, backup=true, aside=false, marker=true;
                bool failRestore=false, failCleanup=false;
                bool targetExists() const override{return target;}
                bool moveTargetAside() override{if(!target)return false;target=false;aside=true;return true;}
                bool restoreBackup() override{if(failRestore)return false;backup=false;target=true;return true;}
                bool restoreAside() override{if(!aside)return false;aside=false;target=true;return true;}
                bool removeBackup() override{if(failCleanup)return false;backup=false;return true;}
                bool removeAside() override{if(failCleanup)return false;aside=false;return true;}
                bool removeMarker() override{if(failCleanup)return false;marker=false;return true;}
            };
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
                Playlist tooFast{}; tooFast.count=1; tooFast.slots[0]=0; tooFast.intervalSeconds=59;
                assert(!validatePlaylist(tooFast));
                tooFast.intervalSeconds=MIN_INTERVAL_SECONDS; assert(validatePlaylist(tooFast));
                tooFast.intervalSeconds=MAX_INTERVAL_SECONDS; assert(validatePlaylist(tooFast));
                tooFast.intervalSeconds=MAX_INTERVAL_SECONDS+1; assert(!validatePlaylist(tooFast));
                assert(!applyAllowed(true,true,true));
                assert(!applyAllowed(false,false,true));
                assert(applyAllowed(false,true,true));
                FakeRecovery prepared; assert(executeRecovery(prepared,SyncStage::prepared)==RecoveryResult::ok);
                assert(prepared.target && !prepared.backup && !prepared.marker);
                FakeRecovery installed; assert(executeRecovery(installed,SyncStage::committed)==RecoveryResult::ok);
                assert(installed.target && !installed.backup && !installed.marker);
                FakeRecovery failed; failed.failRestore=true;
                assert(executeRecovery(failed,SyncStage::prepared)==RecoveryResult::restoreFailed);
                assert(failed.target && failed.backup && failed.marker);
                FakeRecovery cleanup; cleanup.failCleanup=true;
                assert(executeRecovery(cleanup,SyncStage::committed)==RecoveryResult::cleanupFailed);
                assert(cleanup.target && cleanup.backup && cleanup.marker);
                using notua::power::selectWakeSources;
                auto slideshow=selectWakeSources(300000000ULL,true);
                assert(slideshow.ext1Enabled && slideshow.timerUs==300000000ULL);
                auto syncWait=selectWakeSources(0,true);
                assert(syncWait.ext1Enabled && syncWait.timerUs==0);
                auto stuck=selectWakeSources(0,false);
                assert(!stuck.ext1Enabled && stuck.timerUs==notua::power::STUCK_BUTTON_FALLBACK_US);
                auto error=selectWakeSources(60000000ULL,true);
                assert(error.ext1Enabled && error.timerUs==60000000ULL);
                auto stuckError=selectWakeSources(60000000ULL,false);
                assert(!stuckError.ext1Enabled && stuckError.timerUs==notua::power::STUCK_BUTTON_FALLBACK_US);
                assert(stuck.ext1Enabled || stuck.timerUs!=0); // Never zero wake sources.
            }
        """)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "protocol_test"
            subprocess.run([
                "g++", "-std=c++17", "-I", str(ROOT / "src"), "-x", "c++", "-",
                str(ROOT / "src/core/ble/transferProtocol.cpp"), "-o", str(binary),
                str(ROOT / "src/core/storage/syncModel.cpp"),
                str(ROOT / "src/core/power/wakePolicy.cpp"),
            ], input=source, text=True, check=True)
            subprocess.run([binary], check=True)

    def test_python_crc_reference(self):
        self.assertEqual(zlib.crc32(b"123456789") & 0xFFFFFFFF, 0xCBF43926)


if __name__ == "__main__":
    unittest.main()
