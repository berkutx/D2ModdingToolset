/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#ifndef SIMTURNS_SPELL_TIMING_H
#define SIMTURNS_SPELL_TIMING_H

#include "simturns/russobit_sites.h"
#include <cstdint>
#include <limits>
#include <string>

namespace hooks::simturns::spell_timing {

inline constexpr std::uint32_t maximumProvenDuration = 64;

struct TurnStopRebase
{
    std::uint32_t value{};
    bool valid{};
    bool changed{};
};

/** Pure, header-only helper so formula edge cases can be unit-tested. */
constexpr TurnStopRebase rebaseTurnStop(
    std::uint32_t stamped,
    std::uint32_t stampedFromDay,
    std::uint32_t ownerDay,
    std::uint32_t maximumDuration = maximumProvenDuration) noexcept
{
    if (!ownerDay || stamped < stampedFromDay)
        return {stamped, false, false};

    const std::uint32_t duration = stamped - stampedFromDay;
    if (duration > maximumDuration
        || ownerDay > std::numeric_limits<std::uint32_t>::max() - duration) {
        return {stamped, false, false};
    }

    const std::uint32_t corrected = ownerDay + duration;
    return {corrected, true, corrected != stamped};
}

/** Read-only verification of the typed caster dispatcher and owner rebase site. */
bool preflight(std::string& error);

/** Appends CCmdCastSpellMsg and CMidSpellEffects add Detours. CSpellCastMsg is
 * deliberately left natural: its callback argument called `idFrom` in legacy
 * headers is actually the RX frame size, not a proven player identity. */
void appendDetours(DetourTargets& targets);

} // namespace hooks::simturns::spell_timing

#endif // SIMTURNS_SPELL_TIMING_H
