#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include "core/storage/syncModel.h"

namespace notua::storage {

// Legacy migration only: collect every valid Y8 BIN and sort before filling fixed slots.
std::vector<String> collectImageCatalog(fs::FS& filesystem, bool* tooMany = nullptr);
bool scanFixedCatalog(fs::FS& filesystem, CatalogEntry output[MAX_IMAGES]);
bool migrateLegacyImages(fs::FS& filesystem);

} // namespace notua::storage
