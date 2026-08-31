#include "core/storage/transferSession.h"
#include <freertos/FreeRTOS.h>

namespace notua::storage {
namespace { portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; TransferOwner owner = TransferOwner::none; }
bool acquireTransferSession(TransferOwner requested) {
    bool acquired = false;
    portENTER_CRITICAL(&mux);
    if (owner == TransferOwner::none) { owner = requested; acquired = true; }
    portEXIT_CRITICAL(&mux);
    return acquired;
}
void releaseTransferSession(TransferOwner releasing) {
    portENTER_CRITICAL(&mux);
    if (owner == releasing) owner = TransferOwner::none;
    portEXIT_CRITICAL(&mux);
}
TransferOwner transferSessionOwner() {
    portENTER_CRITICAL(&mux); const TransferOwner value = owner; portEXIT_CRITICAL(&mux); return value;
}
}
