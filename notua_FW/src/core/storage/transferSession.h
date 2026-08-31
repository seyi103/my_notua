#pragma once
#include <stdint.h>

namespace notua::storage {
enum class TransferOwner : uint8_t { none, ble, softAp };
bool acquireTransferSession(TransferOwner owner);
void releaseTransferSession(TransferOwner owner);
TransferOwner transferSessionOwner();
}
