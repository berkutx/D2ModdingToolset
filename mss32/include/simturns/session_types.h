/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * Transport-independent simultaneous-turn session options.
 */

#ifndef SIMTURNS_SESSION_TYPES_H
#define SIMTURNS_SESSION_TYPES_H

#include <cstdint>

namespace hooks::simturns {

/** Highest day representable by the original engine's signed currentTurn. */
constexpr std::uint32_t maxEngineDay = 0x7fffffffu;

enum class Role : std::uint32_t
{
    Host = 1,
    Join = 2,
};

static_assert(static_cast<std::uint32_t>(Role::Host) == 1);
static_assert(static_cast<std::uint32_t>(Role::Join) == 2);

/** Lobby-owned strategic turn policy. Build-time support alone activates no
 * room: authenticated Arm selects the native role, and its matching SessionPlan
 * authorizes the actual gameplay mode. No environment override is consulted. */
enum class TurnMode : std::uint32_t
{
    Stock = 0,
    Simultaneous = 1,
};

static_assert(static_cast<std::uint32_t>(TurnMode::Stock) == 0);
static_assert(static_cast<std::uint32_t>(TurnMode::Simultaneous) == 1);

struct SimTurnsSessionOptions
{
    bool requested{};
    Role role{Role::Host};
};

} // namespace hooks::simturns

#endif // SIMTURNS_SESSION_TYPES_H
