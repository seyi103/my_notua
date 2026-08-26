#pragma once

#include <FS.h>
#include <stdint.h>

namespace frame_store {

struct Metadata {
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t bytes = 0;
};

bool begin();
bool beginWrite(const Metadata& metadata);
bool append(const uint8_t* data, size_t length);
bool commit();
void abortWrite();
bool hasFrame();
bool metadata(Metadata& out);
File openFrame();

} // namespace frame_store
