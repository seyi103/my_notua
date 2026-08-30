#pragma once

#include <stdint.h>

namespace notua::power {

constexpr uint64_t STUCK_BUTTON_FALLBACK_US = 5ULL * 60ULL * 1000000ULL;

struct WakeSourcePlan {
    bool ext1Enabled;
    uint64_t timerUs;
};

// requestedTimerUs == 0 means an intentional EXT1-only sleep. A stuck-HIGH button
// removes EXT1 and therefore always receives a finite fallback timer.
WakeSourcePlan selectWakeSources(uint64_t requestedTimerUs, bool ext1Usable);

} // namespace notua::power
