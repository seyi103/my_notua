#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>

#include "core/ble/transferProtocol.h"

namespace notua::storage {

enum class StartResult { ok, badSize, badSlot, catalogError, noSpace, openFailed, cleanupFailed };
enum class CommitResult {
    committed, notReady, badSize, crcMismatch, flushFailed, markerWriteFailed,
    finalToBackupFailed, tempToFinalFailed, backupRestoreFailed,
};
enum class CleanupResult { ok, removeFailed, renameFailed, backupRestoreFailed };

class ImageStorage {
public:
    bool begin();
    StartResult start(uint8_t slot, uint32_t size, uint32_t crc32);
    StartResult startSpike(uint32_t size, uint32_t crc32);
    bool append(const uint8_t* data, size_t length);
    CommitResult finish(uint32_t& detail);
    CleanupResult finalizeCommit();
    CleanupResult markCommitDurable();
    CleanupResult rollbackCommit();
    CleanupResult abort();
    bool active() const { return active_; }
    uint32_t offset() const { return offset_; }
    const String& committedPath() const { return finalPath_; }
    uint8_t slot() const { return slot_; }
private:
    CleanupResult recover();
    CommitResult commitAtomic();
    File file_;
    String tempPath_;
    String finalPath_;
    uint32_t offset_ = 0;
    uint32_t expectedCrc_ = 0;
    bool mounted_ = false;
    bool active_ = false;
    bool replacementPending_ = false;
    uint8_t slot_ = 0xff;
    notua::transfer::Crc32 crc_;
};

} // namespace notua::storage
