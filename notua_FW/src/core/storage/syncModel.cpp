#include "core/storage/syncModel.h"
#include "core/ble/transferProtocol.h"

namespace notua::storage {

bool validatePlaylist(const Playlist& playlist) {
    if (playlist.count < 1 || playlist.count > MAX_IMAGES || playlist.intervalSeconds == 0) return false;
    uint8_t seen = 0;
    for (uint8_t i = 0; i < playlist.count; ++i) {
        if (playlist.slots[i] >= MAX_IMAGES) return false;
        const uint8_t bit = 1U << playlist.slots[i];
        if (seen & bit) return false;
        seen |= bit;
    }
    return true;
}

void encodePlaylist(uint8_t output[PLAYLIST_BYTES], const Playlist& value) {
    output[0] = 1; output[1] = value.count;
    notua::transfer::writeLe32(output + 2, value.revision);
    notua::transfer::writeLe32(output + 6, value.intervalSeconds);
    for (uint8_t i = 0; i < MAX_IMAGES; ++i) output[10 + i] = value.slots[i];
    for (uint8_t i = 0; i < MAX_IMAGES; ++i)
        notua::transfer::writeLe32(output + 15 + i * 4, value.crc32[i]);
}

bool decodePlaylist(const uint8_t* input, size_t length, Playlist& value) {
    if (!input || length != PLAYLIST_BYTES || input[0] != 1) return false;
    value.count = input[1]; value.revision = notua::transfer::readLe32(input + 2);
    value.intervalSeconds = notua::transfer::readLe32(input + 6);
    for (uint8_t i = 0; i < MAX_IMAGES; ++i) value.slots[i] = input[10 + i];
    for (uint8_t i = 0; i < MAX_IMAGES; ++i)
        value.crc32[i] = notua::transfer::readLe32(input + 15 + i * 4);
    return validatePlaylist(value);
}

void encodeCatalog(uint8_t output[CATALOG_BYTES], const CatalogEntry entries[MAX_IMAGES],
    const Playlist& active, const Playlist& target, SyncStage stage, uint8_t completedBitmap) {
    output[0] = 1; output[1] = MAX_IMAGES; output[2] = static_cast<uint8_t>(stage);
    output[3] = completedBitmap;
    for (uint8_t i = 0; i < MAX_IMAGES; ++i) {
        const size_t at = 4 + i * 10;
        output[at] = entries[i].slot;
        output[at + 1] = (entries[i].exists ? 1 : 0) | (entries[i].valid ? 2 : 0);
        notua::transfer::writeLe32(output + at + 2, entries[i].size);
        notua::transfer::writeLe32(output + at + 6, entries[i].crc32);
    }
    encodePlaylist(output + 54, active);
    encodePlaylist(output + 54 + PLAYLIST_BYTES, target);
}

uint8_t matchingSlot(const CatalogEntry entries[MAX_IMAGES], uint32_t crc32,
    uint8_t reservedBitmap) {
    for (uint8_t slot = 0; slot < MAX_IMAGES; ++slot)
        if (!(reservedBitmap & (1U << slot)) && entries[slot].valid && entries[slot].crc32 == crc32)
            return slot;
    return 0xff;
}

RecoveryAction recoveryAction(SyncStage stage, bool targetExists, bool backupExists) {
    if (!backupExists) return targetExists ? RecoveryAction::clean : RecoveryAction::unrecoverable;
    if (stage == SyncStage::prepared) return RecoveryAction::restoreBackup;
    if (stage == SyncStage::committed) return targetExists
        ? RecoveryAction::keepFinal : RecoveryAction::restoreBackup;
    return RecoveryAction::unrecoverable;
}

} // namespace notua::storage
