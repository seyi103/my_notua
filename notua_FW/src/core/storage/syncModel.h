#pragma once

#include <stddef.h>
#include <stdint.h>

namespace notua::storage {

constexpr uint8_t MAX_IMAGES = 5;
constexpr uint32_t IMAGE_BYTES = 1600U * 1200U;
constexpr uint32_t DEFAULT_INTERVAL_SECONDS = 300;
constexpr uint32_t MIN_INTERVAL_SECONDS = 60;
constexpr uint32_t MAX_INTERVAL_SECONDS = 24U * 60U * 60U;
constexpr uint32_t FILESYSTEM_SAFETY_MARGIN = 128U * 1024U;
constexpr size_t PLAYLIST_BYTES = 35;
constexpr size_t CATALOG_BYTES = 124;

enum class SyncStage : uint8_t { idle = 0, prepared = 1, committed = 2 };
enum class RecoveryAction : uint8_t { clean, restoreBackup, keepFinal, unrecoverable };
enum class RecoveryResult : uint8_t { ok, moveFailed, restoreFailed, cleanupFailed };

class RecoveryIo {
public:
    virtual ~RecoveryIo() = default;
    virtual bool targetExists() const = 0;
    virtual bool moveTargetAside() = 0;
    virtual bool restoreBackup() = 0;
    virtual bool restoreAside() = 0;
    virtual bool removeBackup() = 0;
    virtual bool removeAside() = 0;
    virtual bool removeMarker() = 0;
};

struct CatalogEntry {
    uint8_t slot = 0;
    bool exists = false;
    bool valid = false;
    uint32_t size = 0;
    uint32_t crc32 = 0;
};

struct Playlist {
    uint32_t revision = 0;
    uint8_t count = 0;
    uint8_t slots[MAX_IMAGES]{};
    uint32_t crc32[MAX_IMAGES]{};
    uint32_t intervalSeconds = DEFAULT_INTERVAL_SECONDS;
};

bool validatePlaylist(const Playlist& playlist);
void encodePlaylist(uint8_t output[PLAYLIST_BYTES], const Playlist& playlist);
bool decodePlaylist(const uint8_t* input, size_t length, Playlist& playlist);
void encodeCatalog(uint8_t output[CATALOG_BYTES], const CatalogEntry entries[MAX_IMAGES],
    const Playlist& active, const Playlist& target, SyncStage stage, uint8_t completedBitmap);
uint8_t matchingSlot(const CatalogEntry entries[MAX_IMAGES], uint32_t crc32,
    uint8_t reservedBitmap);
RecoveryAction recoveryAction(SyncStage stage, bool targetExists, bool backupExists);
bool applyAllowed(bool syncInProgress, bool playlistCommitted, bool pendingDurable);
RecoveryResult executeRecovery(RecoveryIo& io, SyncStage stage);

} // namespace notua::storage
