#include "core/power/wakePolicy.h"

namespace notua::power {

WakeSourcePlan selectWakeSources(uint64_t requestedTimerUs, bool ext1Usable) {
    if (ext1Usable) return {true, requestedTimerUs};
    return {false, requestedTimerUs > STUCK_BUTTON_FALLBACK_US
        ? requestedTimerUs : STUCK_BUTTON_FALLBACK_US};
}

} // namespace notua::power
