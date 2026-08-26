#include "core/storage/frameStore.h"

#include <LittleFS.h>

namespace frame_store {
namespace {
constexpr const char* kFramePath = "/frame.bin";
constexpr const char* kTempPath = "/frame.tmp";
File gUpload;
Metadata gPending {};
uint32_t gWritten = 0;
bool gMounted = false;
} // namespace

bool begin() {
    gMounted = LittleFS.begin(false);
    return gMounted;
}

bool beginWrite(const Metadata& metadata) {
    abortWrite();
    const uint32_t expected = static_cast<uint32_t>(metadata.width) * metadata.height;
    if (!gMounted || metadata.width == 0 || metadata.height == 0 || metadata.bytes != expected) {
        return false;
    }
    LittleFS.remove(kTempPath);
    gUpload = LittleFS.open(kTempPath, FILE_WRITE);
    if (!gUpload) {
        return false;
    }
    gPending = metadata;
    gWritten = 0;
    return gUpload.write(reinterpret_cast<const uint8_t*>(&gPending), sizeof(gPending))
        == sizeof(gPending);
}

bool append(const uint8_t* data, size_t length) {
    if (!gUpload || !data || length == 0 || gWritten + length > gPending.bytes) {
        return false;
    }
    const size_t written = gUpload.write(data, length);
    gWritten += written;
    return written == length;
}

bool commit() {
    if (!gUpload || gWritten != gPending.bytes) {
        abortWrite();
        return false;
    }
    gUpload.flush();
    gUpload.close();

    LittleFS.remove(kFramePath);
    if (!LittleFS.rename(kTempPath, kFramePath)) {
        return false;
    }
    return true;
}

void abortWrite() {
    if (gUpload) {
        gUpload.close();
    }
    if (gMounted) {
        LittleFS.remove(kTempPath);
    }
    gPending = {};
    gWritten = 0;
}

bool metadata(Metadata& out) {
    if (!gMounted) {
        return false;
    }
    File file = LittleFS.open(kFramePath, FILE_READ);
    if (!file || file.size() < sizeof(Metadata)
        || file.read(reinterpret_cast<uint8_t*>(&out), sizeof(out)) != sizeof(out)) {
        return false;
    }
    return out.bytes == static_cast<uint32_t>(out.width) * out.height;
}

bool hasFrame() {
    Metadata info {};
    File frame = LittleFS.open(kFramePath, FILE_READ);
    return metadata(info) && frame && frame.size() == info.bytes + sizeof(Metadata);
}

File openFrame() {
    File frame = gMounted ? LittleFS.open(kFramePath, FILE_READ) : File();
    if (frame) {
        frame.seek(sizeof(Metadata));
    }
    return frame;
}

} // namespace frame_store
