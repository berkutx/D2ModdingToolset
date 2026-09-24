/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#ifndef SIMTURNS_BATTLE_COMPAT_H
#define SIMTURNS_BATTLE_COMPAT_H

#include "simturns/russobit_sites.h"
#include <string>

namespace hooks::simturns::battle_compat {

/** Read-only verification of the concurrent-battle method entry and the two
 * exact stale auto-battle flag instructions proved by the legacy 15/15 run. */
bool preflight(std::string& error);

/** Appends the null-this compatibility guard Detour. */
void appendDetours(DetourTargets& targets);

} // namespace hooks::simturns::battle_compat

#endif // SIMTURNS_BATTLE_COMPAT_H
