#include "core/storage/imageCatalog.h"

#include <algorithm>
#include "core/ble/transferProtocol.h"

namespace notua::storage {

std::vector<String> collectImageCatalog(fs::FS& filesystem, bool* tooMany) {
    std::vector<String> images;
    if (!filesystem.exists("/images")) {
        if (tooMany) *tooMany = false;
        return images;
    }
    File directory = filesystem.open("/images");
    if (!directory || !directory.isDirectory()) {
        if (tooMany) *tooMany = false;
        return images;
    }
    for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
        String path = file.path();
        String lower = path; lower.toLowerCase();
        if (!file.isDirectory() && lower.endsWith(".bin") && file.size() == IMAGE_BYTES)
            images.push_back(path);
    }
    std::sort(images.begin(), images.end());
    if (tooMany) *tooMany = images.size() > MAX_IMAGES;
    return images;
}

bool scanFixedCatalog(fs::FS& filesystem, CatalogEntry output[MAX_IMAGES]) {
    for (uint8_t slot = 0; slot < MAX_IMAGES; ++slot) {
        output[slot] = {}; output[slot].slot = slot;
        const String path = String("/images/slot_") + slot + ".bin";
        if (!filesystem.exists(path)) continue;
        File file = filesystem.open(path, FILE_READ);
        if (!file) continue;
        output[slot].exists = true; output[slot].size = file.size();
        if (file.size() != IMAGE_BYTES) { file.close(); continue; }
        notua::transfer::Crc32 crc;
        uint8_t buffer[1024];
        while (file.available()) {
            const size_t count = file.read(buffer, sizeof(buffer));
            if (!count) { file.close(); return false; }
            crc.update(buffer, count);
        }
        file.close(); output[slot].crc32 = crc.value(); output[slot].valid = true;
    }
    return true;
}

bool migrateLegacyImages(fs::FS& filesystem) {
    bool ignored = false;
    const auto legacy = collectImageCatalog(filesystem, &ignored);
    size_t legacyIndex = 0;
    for (uint8_t slot = 0; slot < MAX_IMAGES && legacyIndex < legacy.size(); ++slot) {
        const String fixed = String("/images/slot_") + slot + ".bin";
        if (filesystem.exists(fixed)) continue;
        while (legacyIndex < legacy.size() && legacy[legacyIndex].startsWith("/images/slot_"))
            ++legacyIndex;
        if (legacyIndex == legacy.size()) break;
        if (!filesystem.rename(legacy[legacyIndex++], fixed)) return false;
    }
    return true;
}

} // namespace notua::storage