#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

namespace notua::storage {

constexpr size_t MAX_IMAGES = 3;
constexpr size_t IMAGE_BYTES = 1600U * 1200U;

// Collect every valid Y8 BIN, sort the complete set, then apply the three-image limit.
std::vector<String> collectImageCatalog(fs::FS& filesystem, bool* tooMany = nullptr);

} // namespace notua::storage
