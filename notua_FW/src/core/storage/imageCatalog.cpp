#include "core/storage/imageCatalog.h"

#include <algorithm>

namespace notua::storage {

std::vector<String> collectImageCatalog(fs::FS& filesystem, bool* tooMany) {
    std::vector<String> images;
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
    if (images.size() > MAX_IMAGES) images.resize(MAX_IMAGES);
    return images;
}

} // namespace notua::storage
