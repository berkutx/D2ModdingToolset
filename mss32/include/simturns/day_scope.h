/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#ifndef SIMTURNS_DAY_SCOPE_H
#define SIMTURNS_DAY_SCOPE_H

#include "simturns/russobit_sites.h"
#include <cstdint>
#include <string>

namespace game {
struct CScenarioInfo;
struct IMidgardObjectMap;
} // namespace game

namespace hooks::simturns {

/**
 * Runs callback while holding the process-wide recursive scenario-day lock.
 * If temporaryDay is non-null and the simultaneous phase is still active after
 * taking the lock, CScenarioInfo::currentTurn is replaced for the call and is
 * restored by __finally on every exit path. While the pre-merge engine is
 * healthy, the callback is invoked exactly once and exceptions from it are
 * never consumed. A terminal simultaneous-turn fault blocks the callback.
 */
using SerializedCurrentTurnCallback = std::uintptr_t (*)(void* context,
                                                         game::CScenarioInfo* scenarioInfo);

std::uintptr_t runSerializedCurrentTurn(game::IMidgardObjectMap* objectMap,
                                        const std::uint32_t* temporaryDay,
                                        SerializedCurrentTurnCallback callback,
                                        void* context);

namespace day_scope {

/** Read-only verification of the two host-side effect apply identities. */
bool preflight(std::string& error);

/** Appends build and research Detours. Call only after preflight succeeds. */
void appendDetours(DetourTargets& targets);

} // namespace day_scope

/**
 * Full read-only production bundle preflight. Host adds day/cast hooks; both
 * roles verify the concurrent-battle guard. Also requires the exact executable
 * fingerprint and fixed 32-bit image base.
 */
bool preflightProductionHookBundle(bool host, std::string& error);

/**
 * Preflights into a temporary list and appends the complete bundle atomically
 * from the caller's point of view. On failure, targets is unchanged.
 */
bool appendProductionHookBundle(bool host, DetourTargets& targets, std::string& error);

} // namespace hooks::simturns

#endif // SIMTURNS_DAY_SCOPE_H
