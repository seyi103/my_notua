#include "core/storage/imageStorage.h"

#include <algorithm>

namespace notua::storage {
namespace {
constexpr const char* TEMP_PATH = "/images/notua_upload.tmp";
constexpr const char* BACKUP_PATH = "/images/notua_replace.bak";
bool validImage(File& file) {
    String path = file.path(); path.toLowerCase();
    return !file.isDirectory() && path.endsWith(".bin") && file.size() == notua::transfer::IMAGE_BYTES;
}
}

bool ImageStorage::begin() {
    if (mounted_) return true;
    mounted_ = LittleFS.begin(false);
    return mounted_ && recover();
}

bool ImageStorage::recover() {
    LittleFS.remove(TEMP_PATH);
    if (!LittleFS.exists(BACKUP_PATH)) return true;
    // A backup is only left while replacing finalPath; identify its slot from a sidecar name.
    File marker = LittleFS.open("/images/notua_replace.path", FILE_READ);
    String target = marker ? marker.readString() : String();
    if (marker) marker.close();
    if (target.length()) {
        if (LittleFS.exists(target)) LittleFS.remove(target);
        LittleFS.rename(BACKUP_PATH, target);
    }
    LittleFS.remove("/images/notua_replace.path");
    return true;
}

std::vector<String> ImageStorage::validImages() const {
    std::vector<String> result;
    File directory = LittleFS.open("/images");
    if (!directory || !directory.isDirectory()) return result;
    for (File file = directory.openNextFile(); file; file = directory.openNextFile())
        if (validImage(file)) result.push_back(file.path());
    std::sort(result.begin(), result.end());
    return result;
}

bool ImageStorage::start(uint8_t slot, uint32_t size, uint32_t crc32) {
    abort();
    if (!begin() || size != notua::transfer::IMAGE_BYTES || slot > 2) return false;
    const auto images = validImages();
    if (images.size() > 3 || slot > images.size()) return false; // Reject a gap.
    finalPath_ = slot < images.size() ? images[slot]
        : String("/images/notua_slot_") + String(slot) + ".bin";
    tempPath_ = TEMP_PATH;
    LittleFS.remove(tempPath_);
    file_ = LittleFS.open(tempPath_, FILE_WRITE);
    if (!file_) return false;
    offset_ = 0; expectedCrc_ = crc32; crc_.reset(); active_ = true;
    return true;
}

bool ImageStorage::append(const uint8_t* data, size_t length) {
    if (!active_ || !data || length == 0 || offset_ + length > notua::transfer::IMAGE_BYTES) return false;
    if (file_.write(data, length) != length) return false;
    crc_.update(data, length); offset_ += length;
    return true;
}

bool ImageStorage::commitAtomic() {
    const bool replacing = LittleFS.exists(finalPath_);
    if (replacing) {
        LittleFS.remove(BACKUP_PATH);
        File marker = LittleFS.open("/images/notua_replace.path", FILE_WRITE);
        if (!marker || marker.print(finalPath_) != finalPath_.length()) return false;
        marker.close();
        if (!LittleFS.rename(finalPath_, BACKUP_PATH)) return false;
    }
    if (!LittleFS.rename(tempPath_, finalPath_)) {
        if (replacing) LittleFS.rename(BACKUP_PATH, finalPath_);
        LittleFS.remove("/images/notua_replace.path");
        return false;
    }
    replacementPending_ = replacing;
    return true;
}

notua::transfer::Status ImageStorage::finish(uint32_t& detail) {
    detail = 0;
    if (!active_) return notua::transfer::Status::notReady;
    file_.flush(); file_.close();
    if (offset_ != notua::transfer::IMAGE_BYTES) { detail = offset_; abort(); return notua::transfer::Status::badSize; }
    const uint32_t actual = crc_.value();
    if (actual != expectedCrc_) { detail = actual; abort(); return notua::transfer::Status::crcMismatch; }
    if (!commitAtomic()) { abort(); return notua::transfer::Status::storageError; }
    active_ = false;
    return notua::transfer::Status::committed;
}

void ImageStorage::finalizeCommit() {
    if (replacementPending_) LittleFS.remove(BACKUP_PATH);
    LittleFS.remove("/images/notua_replace.path");
    replacementPending_ = false;
}

void ImageStorage::rollbackCommit() {
    if (replacementPending_) {
        LittleFS.remove(finalPath_);
        LittleFS.rename(BACKUP_PATH, finalPath_);
    } else if (finalPath_.length()) {
        LittleFS.remove(finalPath_);
    }
    LittleFS.remove("/images/notua_replace.path");
    replacementPending_ = false;
}

void ImageStorage::abort() {
    if (file_) file_.close();
    if (tempPath_.length()) LittleFS.remove(tempPath_);
    active_ = false; offset_ = 0; tempPath_ = String();
}

} // namespace notua::storage
