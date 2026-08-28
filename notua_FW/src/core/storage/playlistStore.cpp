#include "core/storage/playlistStore.h"

namespace notua::storage {

bool PlaylistStore::begin() {
    opened_ = preferences_.begin("photo-cycle", false);
    return opened_;
}
void PlaylistStore::end() { if (opened_) preferences_.end(); opened_ = false; }

bool PlaylistStore::putPlaylist(const char* key, const Playlist& playlist) {
    uint8_t bytes[PLAYLIST_BYTES]; encodePlaylist(bytes, playlist);
    return preferences_.putBytes(key, bytes, sizeof(bytes)) == sizeof(bytes);
}
bool PlaylistStore::getPlaylist(const char* key, Playlist& playlist) {
    uint8_t bytes[PLAYLIST_BYTES];
    return preferences_.getBytesLength(key) == sizeof(bytes)
        && preferences_.getBytes(key, bytes, sizeof(bytes)) == sizeof(bytes)
        && decodePlaylist(bytes, sizeof(bytes), playlist);
}

bool PlaylistStore::loadActiveMetadata(Playlist& playlist) {
    return getPlaylist("playlist", playlist);
}

bool PlaylistStore::loadActiveValidated(const CatalogEntry catalog[MAX_IMAGES], Playlist& playlist) {
    if (getPlaylist("playlist", playlist)) {
        for (uint8_t i = 0; i < playlist.count; ++i) {
            const uint8_t slot = playlist.slots[i];
            if (!catalog[slot].valid || catalog[slot].crc32 != playlist.crc32[i]) return false;
        }
        return true;
    }
    playlist = {}; playlist.revision = 1; playlist.intervalSeconds = DEFAULT_INTERVAL_SECONDS;
    for (uint8_t slot = 0; slot < MAX_IMAGES; ++slot) if (catalog[slot].valid) {
        playlist.slots[playlist.count] = slot;
        playlist.crc32[playlist.count++] = catalog[slot].crc32;
    }
    return playlist.count && putPlaylist("playlist", playlist);
}

bool PlaylistStore::syncInProgress() { return opened_ && preferences_.getBool("sync", false); }
bool PlaylistStore::loadTarget(Playlist& target, uint8_t& completed, SyncStage& stage) {
    if (!syncInProgress() || !getPlaylist("sync_target", target)) return false;
    completed = preferences_.getUChar("sync_done", 0);
    stage = static_cast<SyncStage>(preferences_.getUChar("sync_stage", 0));
    return true;
}

bool PlaylistStore::beginSync(const Playlist& target) {
    Playlist active{};
    if (!validatePlaylist(target) || !getPlaylist("playlist", active)
        || !putPlaylist("sync_previous", active) || !putPlaylist("sync_target", target)) return false;
    const String pending = preferences_.getString("pending", "");
    if (preferences_.putBool("sync_hadpend", pending.length() != 0) == 0) return false;
    if (pending.length() && preferences_.putString("sync_pending", pending) == 0) return false;
    if (preferences_.putUChar("sync_done", 0) == 0
        || preferences_.putUChar("sync_stage", static_cast<uint8_t>(SyncStage::prepared)) == 0)
        return false;
    return preferences_.putBool("sync", true) != 0;
}

bool PlaylistStore::markSlotComplete(uint8_t slot) {
    if (!syncInProgress() || slot >= MAX_IMAGES) return false;
    return preferences_.putUChar("sync_done",
        preferences_.getUChar("sync_done", 0) | (1U << slot)) != 0;
}

bool PlaylistStore::reconcileCompleted(const CatalogEntry catalog[MAX_IMAGES]) {
    Playlist target{}; uint8_t completed = 0; SyncStage stage{};
    if (!loadTarget(target, completed, stage)) return true;
    uint8_t verified = 0;
    for (uint8_t i = 0; i < target.count; ++i) {
        const uint8_t slot = target.slots[i];
        if (catalog[slot].valid && catalog[slot].crc32 == target.crc32[i]) verified |= 1U << slot;
    }
    return verified == completed || preferences_.putUChar("sync_done", verified) != 0;
}

bool PlaylistStore::commitTarget(const CatalogEntry catalog[MAX_IMAGES], String& pendingPath) {
    Playlist target{}; uint8_t completed = 0; SyncStage stage{};
    if (!loadTarget(target, completed, stage)) return false;
    for (uint8_t i = 0; i < target.count; ++i) {
        const uint8_t slot = target.slots[i];
        if (!catalog[slot].valid || catalog[slot].crc32 != target.crc32[i]) return false;
    }
    Playlist old{}; if (!getPlaylist("sync_previous", old)) return false;
    const bool hadPending = preferences_.getBool("sync_hadpend", false);
    const String oldPending = preferences_.getString("sync_pending", "");
    if (preferences_.putUChar("sync_stage", static_cast<uint8_t>(SyncStage::committed)) == 0
        || !putPlaylist("playlist", target)) return false;
    pendingPath = String("/images/slot_") + target.slots[0] + ".bin";
    if (preferences_.putString("pending", pendingPath) == 0) {
        const bool playlistRestored = putPlaylist("playlist", old);
        const bool pendingRestored = hadPending
            ? preferences_.putString("pending", oldPending) != 0
            : (!preferences_.isKey("pending") || preferences_.remove("pending"));
        if (!playlistRestored || !pendingRestored) return false;
        return false;
    }
    if (preferences_.putBool("sync", false) == 0) return false;
    preferences_.remove("sync_target"); preferences_.remove("sync_previous");
    preferences_.remove("sync_pending"); preferences_.remove("sync_hadpend");
    preferences_.remove("sync_done"); preferences_.remove("sync_stage");
    return true;
}

} // namespace notua::storage
