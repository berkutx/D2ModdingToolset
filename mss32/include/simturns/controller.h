/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * Production coordinator glue for the exact-Russobit simultaneous-turn PoC.
 */

#ifndef SIMTURNS_CONTROLLER_H
#define SIMTURNS_CONTROLLER_H

#include "hooks.h"
#include "simturns/session_types.h"
#include <cstdint>

namespace game {
struct CPhaseGame;
struct CMidObjectLock;
} // namespace game

namespace hooks::simturns {

/**
 * Transport-neutral strategic-input admission snapshot. DebugTest and future
 * UI integrations use this seam instead of depending on CoordinatorPort or
 * any concrete transport state machine.
 */
bool localActionAdmission(std::uint32_t currentPlayerHandle);

/**
 * Process-only exact-executable and byte preflight for both future room roles.
 * False leaves ordinary play available but must not advertise the OH capability.
 */
bool prepare();

/** Appends the already-preflighted Detours. Does nothing when the feature is disabled. */
void appendHooks(Hooks& hooks);

/**
 * Installs the shared RX/TX interception after the Detours transaction has
 * committed. This never starts the coordinator; onPhaseGame owns that step.
 */
bool install();

/** Exact native support is installed; this does not activate a room. */
bool available();

/** UI-only native application boundary for ordered lobby control delivery.
 * RX return alone is insufficient when it queued native commands. */
bool strategicQueueIdle();

/** UI-thread authenticated-room boundary before native map startup. */
bool beginSession(Role role);

/** Called before native clearNetworkState[AndService]. Closes admission,
 * invalidates callbacks, quiesces the coordinator and cancels owned ordered work.
 * False forbids native destruction: the caller must fail closed. */
bool beginSessionTeardown();

/** Called only after that native clear returned (workers joined, map destroyed).
 * Restores patches and forgets every borrowed native/session identity. False
 * forbids continuation into another map; this is not a stock-mode fallback. */
bool endSession();

/** UI-thread strategic-phase seam. Starts the one-shot coordinator port when ready. */
void onPhaseGame(game::CPhaseGame* phaseGame);

/**
 * Existing final command-queue subscriber seam. During join startup only,
 * confirms that older stock commands have drained, issues one directed
 * activation, and verifies its completion. There is no timer or reinjection.
 */
void onCommandQueueDrained(game::CMidObjectLock* objectLock);

} // namespace hooks::simturns

#endif // SIMTURNS_CONTROLLER_H
