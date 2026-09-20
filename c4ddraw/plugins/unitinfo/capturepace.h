#pragma once

#include <cstdint>

namespace twitchstat {

// UI-thread scheduler only. Delays and the interval between observations must stay below
// half of the uint32_t clock range, as with GetTickCount-style signed-delta comparisons.
class CapturePace
{
public:
    void reset()
    {
        deferred_ = false;
    }

    bool ready(std::uint32_t now) const
    {
        return !deferred_ || static_cast<std::int32_t>(now - deadline_) >= 0;
    }

    // Call after the work finishes: even a slow capture must leave the game a full pause.
    void defer(std::uint32_t now, std::uint32_t delayMs)
    {
        deadline_ = now + delayMs;
        deferred_ = true;
    }

private:
    std::uint32_t deadline_ = 0;
    bool deferred_ = false;
};

} // namespace twitchstat
