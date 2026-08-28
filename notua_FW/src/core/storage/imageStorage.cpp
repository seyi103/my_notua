#include "core/storage/imageStorage.h"
#include "core/storage/imageCatalog.h"

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
    // A backup is only left while replacing finalPath; identify its slot from a sidecar name.
    File marker = LittleFS.open(MARKER_PATH, FILE_READ);
    String target = marker ? marker.readString() : String();
    if (marker) marker.close();
    if (!target.length()) return CleanupResult::backupRestoreFailed;
    // Never remove a usable final merely to attempt recovery. Both copies and the marker are
    // intentionally retained when final already exists so a later policy can resolve ambiguity.
    if (LittleFS.exists(target)) return CleanupResult::backupRestoreFailed;
    if (!LittleFS.rename(BACKUP_PATH, target)) return CleanupResult::backupRestoreFailed;
    if (!removeChecked(MARKER_PATH)) return CleanupResult::removeFailed;
    return CleanupResult::ok;
}

StartResult ImageStorage::start(uint8_t slot, uint32_t size, uint32_t crc32) {
    if (abort() != CleanupResult::ok) return StartResult::cleanupFailed;
    if (size != notua::transfer::IMAGE_BYTES) return StartResult::badSize;
    if (slot > 2) return StartResult::badSlot;
    if (!begin()) return StartResult::catalogError;
    bool tooMany = false;
    const auto images = collectImageCatalog(LittleFS, &tooMany);
    if (tooMany || slot > images.size()) return StartResult::badSlot;
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < notua::transfer::IMAGE_BYTES)
        return StartResult::noSpace;
    finalPath_ = slot < images.size() ? images[slot]
        : String("/images/notua_slot_") + String(slot) + ".bin";
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
        if (!marker || marker.print(finalPath_) != finalPath_.length()) {
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
