#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>

#include "core/ble/transferProtocol.h"

namespace notua::storage {

class ImageStorage {
public:
    bool begin();
    bool start(uint8_t slot, uint32_t size, uint32_t crc32);
    bool append(const uint8_t* data, size_t length);
    notua::transfer::Status finish(uint32_t& detail);
    void finalizeCommit();
    void rollbackCommit();
    void abort();
    bool active() const { return active_; }
    uint32_t offset() const { return offset_; }
    const String& committedPath() const { return finalPath_; }
private:
    bool recover();
    std::vector<String> validImages() const;
    bool commitAtomic();
    File file_;
    String tempPath_;
    String finalPath_;
    uint32_t offset_ = 0;
    uint32_t expectedCrc_ = 0;
    bool mounted_ = false;
    bool active_ = false;
    bool replacementPending_ = false;
    notua::transfer::Crc32 crc_;
};

} // namespace notua::storage
