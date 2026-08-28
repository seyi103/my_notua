#include "core/storage/imageStorage.h"
#include "core/storage/imageCatalog.h"
#include "core/diag/log.h"

#include <algorithm>

namespace notua::storage {
namespace {
constexpr const char* TEMP_PATH = "/images/notua_upload.tmp";
constexpr const char* BACKUP_PATH = "/images/notua_replace.bak";
constexpr const char* MARKER_PATH = "/images/notua_replace.path";
constexpr const char* ROLLBACK_PATH = "/images/notua_rollback.tmp";
bool removeChecked(const char* path) {
    return !LittleFS.exists(path) || LittleFS.remove(path);
}
}

bool ImageStorage::begin() {
    if (mounted_) return true;
    mounted_ = LittleFS.begin(false);
    return mounted_ && recover() == CleanupResult::ok;
}

CleanupResult ImageStorage::recover() {
    if (!removeChecked(TEMP_PATH) || !removeChecked(ROLLBACK_PATH)) return CleanupResult::removeFailed;
    if (!LittleFS.exists(BACKUP_PATH)) {
        if (!removeChecked(MARKER_PATH)) return CleanupResult::removeFailed;
        return CleanupResult::ok;
    }
    // Marker is ASCII: P<slot> before durable sync progress, C<slot> after it.
    File marker = LittleFS.open(MARKER_PATH, FILE_READ);
    String value = marker ? marker.readString() : String();
    if (marker) marker.close();
    if (value.length() != 2 || (value[0] != 'P' && value[0] != 'C')
        || value[1] < '0' || value[1] >= '0' + MAX_IMAGES) return CleanupResult::backupRestoreFailed;
    const String target = String("/images/slot_") + String(value[1] - '0') + ".bin";
    const SyncStage stage = value[0] == 'C' ? SyncStage::committed : SyncStage::prepared;
    const RecoveryAction action = recoveryAction(stage, LittleFS.exists(target), true);
    if (action == RecoveryAction::keepFinal) {
        if (!removeChecked(BACKUP_PATH)) return CleanupResult::removeFailed;
    } else if (action == RecoveryAction::restoreBackup) {
        // PREPARED means old image wins. Move the new final aside so failed restoration never
        // destroys the only usable copy.
        if (LittleFS.exists(target)) {
            if (!LittleFS.rename(target, ROLLBACK_PATH)) return CleanupResult::renameFailed;
        }
        if (!LittleFS.rename(BACKUP_PATH, target)) {
            if (LittleFS.exists(ROLLBACK_PATH) && !LittleFS.rename(ROLLBACK_PATH, target))
                return CleanupResult::backupRestoreFailed;
            return CleanupResult::backupRestoreFailed;
        }
        if (!removeChecked(ROLLBACK_PATH)) return CleanupResult::removeFailed;
    } else return CleanupResult::backupRestoreFailed;
    if (!removeChecked(MARKER_PATH)) return CleanupResult::removeFailed;
    return CleanupResult::ok;
}

StartResult ImageStorage::start(uint8_t slot, uint32_t size, uint32_t crc32) {
    if (abort() != CleanupResult::ok) return StartResult::cleanupFailed;
    if (size != notua::transfer::IMAGE_BYTES) return StartResult::badSize;
    if (slot >= MAX_IMAGES) return StartResult::badSlot;
    if (!begin()) return StartResult::catalogError;
    const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes(), free = total - used;
    logInfo("STORAGE", "LittleFS upload capacity: total=%u used=%u free=%u required=%u margin=%u",
        static_cast<unsigned>(total), static_cast<unsigned>(used), static_cast<unsigned>(free),
        static_cast<unsigned>(IMAGE_BYTES + FILESYSTEM_SAFETY_MARGIN),
        static_cast<unsigned>(FILESYSTEM_SAFETY_MARGIN));
    if (free < IMAGE_BYTES + FILESYSTEM_SAFETY_MARGIN)
        return StartResult::noSpace;
    slot_ = slot; finalPath_ = String("/images/slot_") + slot + ".bin";
    tempPath_ = TEMP_PATH;
    if (!removeChecked(TEMP_PATH)) return StartResult::cleanupFailed;
    file_ = LittleFS.open(tempPath_, FILE_WRITE);
    if (!file_) return StartResult::openFailed;
    offset_ = 0; expectedCrc_ = crc32; crc_.reset(); active_ = true;
    return StartResult::ok;
}

bool ImageStorage::append(const uint8_t* data, size_t length) {
    if (!active_ || !data || length == 0 || offset_ + length > notua::transfer::IMAGE_BYTES) return false;
    if (file_.write(data, length) != length) return false;
    crc_.update(data, length); offset_ += length;
    return true;
}

CommitResult ImageStorage::commitAtomic() {
    const bool replacing = LittleFS.exists(finalPath_);
    if (replacing) {
        if (!removeChecked(BACKUP_PATH)) return CommitResult::finalToBackupFailed;
        File marker = LittleFS.open(MARKER_PATH, FILE_WRITE);
        const String markerValue = String("P") + slot_;
        if (!marker || marker.print(markerValue) != markerValue.length()) {
            if (marker) marker.close();
            if (!removeChecked(MARKER_PATH)) return CommitResult::markerWriteFailed;
            return CommitResult::markerWriteFailed;
        }
        marker.close();
        if (!LittleFS.rename(finalPath_, BACKUP_PATH)) {
            if (!removeChecked(MARKER_PATH)) return CommitResult::finalToBackupFailed;
            return CommitResult::finalToBackupFailed;
        }
    }
    if (!LittleFS.rename(tempPath_, finalPath_)) {
        if (replacing && !LittleFS.rename(BACKUP_PATH, finalPath_))
            return CommitResult::backupRestoreFailed; // Preserve backup + marker for recovery.
        if (replacing && !removeChecked(MARKER_PATH)) return CommitResult::markerWriteFailed;
        return CommitResult::tempToFinalFailed;
    }
    replacementPending_ = replacing;
    return CommitResult::committed;
}

CommitResult ImageStorage::finish(uint32_t& detail) {
    detail = 0;
    if (!active_) return CommitResult::notReady;
    file_.flush();
    if (file_.getWriteError()) { file_.close(); abort(); return CommitResult::flushFailed; }
    file_.close();
    if (offset_ != notua::transfer::IMAGE_BYTES) { detail = offset_; abort(); return CommitResult::badSize; }
    const uint32_t actual = crc_.value();
    if (actual != expectedCrc_) { detail = actual; abort(); return CommitResult::crcMismatch; }
    const CommitResult committed = commitAtomic();
    if (committed != CommitResult::committed) { abort(); return committed; }
    active_ = false;
    return CommitResult::committed;
}

CleanupResult ImageStorage::finalizeCommit() {
    if (replacementPending_ && !removeChecked(BACKUP_PATH)) return CleanupResult::removeFailed;
    if (!removeChecked(MARKER_PATH)) return CleanupResult::removeFailed;
    replacementPending_ = false;
    return CleanupResult::ok;
}

CleanupResult ImageStorage::markCommitDurable() {
    if (!replacementPending_) return CleanupResult::ok;
    File marker = LittleFS.open(MARKER_PATH, FILE_WRITE);
    const String value = String("C") + slot_;
    if (!marker || marker.print(value) != value.length()) {
        if (marker) marker.close();
        return CleanupResult::renameFailed;
    }
    marker.flush(); marker.close();
    return CleanupResult::ok;
}

CleanupResult ImageStorage::rollbackCommit() {
    if (replacementPending_) {
        if (LittleFS.exists(ROLLBACK_PATH) && !LittleFS.remove(ROLLBACK_PATH)) return CleanupResult::removeFailed;
        if (!LittleFS.rename(finalPath_, ROLLBACK_PATH)) return CleanupResult::renameFailed;
        if (!LittleFS.rename(BACKUP_PATH, finalPath_)) {
            // Restore the new valid final; backup and marker remain untouched and recoverable.
            if (!LittleFS.rename(ROLLBACK_PATH, finalPath_)) return CleanupResult::backupRestoreFailed;
            return CleanupResult::backupRestoreFailed;
        }
        if (!removeChecked(ROLLBACK_PATH)) return CleanupResult::removeFailed;
    } else if (finalPath_.length()) {
        if (!removeChecked(finalPath_.c_str())) return CleanupResult::removeFailed;
    }
    if (!removeChecked(MARKER_PATH)) return CleanupResult::removeFailed;
    replacementPending_ = false;
    return CleanupResult::ok;
}

CleanupResult ImageStorage::abort() {
    if (file_) file_.close();
    if (tempPath_.length() && !removeChecked(tempPath_.c_str())) return CleanupResult::removeFailed;
    active_ = false; offset_ = 0; tempPath_ = String();
    return CleanupResult::ok;
}

} // namespace notua::storage
