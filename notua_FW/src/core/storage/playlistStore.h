#pragma once

#include <Preferences.h>
#include "core/storage/syncModel.h"

namespace notua::storage {

class PlaylistStore {
public:
    bool begin();
    bool loadActive(const CatalogEntry catalog[MAX_IMAGES], Playlist& playlist);
    bool syncInProgress();
    bool loadTarget(Playlist& target, uint8_t& completedBitmap, SyncStage& stage);
    bool beginSync(const Playlist& target);
    bool markSlotComplete(uint8_t slot);
    bool reconcileCompleted(const CatalogEntry catalog[MAX_IMAGES]);
    bool commitTarget(const CatalogEntry catalog[MAX_IMAGES], String& pendingPath);
    void end();
private:
    bool putPlaylist(const char* key, const Playlist& playlist);
    bool getPlaylist(const char* key, Playlist& playlist);
    Preferences preferences_;
    bool opened_ = false;
};

} // namespace notua::storage
