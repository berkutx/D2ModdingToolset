/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "simturns/battle_compat.h"
#include "simturns/state.h"

namespace hooks::simturns::battle_compat {

namespace {

struct ConcurrentBattleState;

using ConcurrentBattleMethod = bool(__thiscall*)(ConcurrentBattleState* thisptr, char* source);

ConcurrentBattleMethod g_concurrentBattleMethodOriginal{};

bool __fastcall concurrentBattleMethodHooked(ConcurrentBattleState* thisptr, int, char* source)
{
    if (!thisptr && phase() != Phase::Disabled && phase() != Phase::Stock)
        return false;
    return g_concurrentBattleMethodOriginal(thisptr, source);
}

} // namespace

bool preflight(std::string& error)
{
    error.clear();
    return russobit::expectBytes(russobit::concurrentBattleMethod,
                                 russobit::concurrentBattleMethodPrefix, "concurrent-battle method",
                                 error)
           && russobit::expectBytes(russobit::autoBattleStaleGate,
                                    russobit::autoBattleStaleGateBytes,
                                    "concurrent auto-battle stale gate", error)
           && russobit::expectBytes(russobit::autoBattleStaleFlagSet,
                                    russobit::autoBattleStaleFlagSetBytes,
                                    "concurrent auto-battle stale-flag set", error);
}

void appendDetours(DetourTargets& targets)
{
    targets.push_back({reinterpret_cast<void*>(russobit::concurrentBattleMethod),
                       reinterpret_cast<void*>(&concurrentBattleMethodHooked),
                       reinterpret_cast<void**>(&g_concurrentBattleMethodOriginal)});
}

} // namespace hooks::simturns::battle_compat
