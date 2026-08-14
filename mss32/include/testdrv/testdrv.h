/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Installer / entry point. DllMain calls installEarly() as early as possible and
 * install() once the module handle and game version are known. Runtime workers
 * are deferred until the first typed UI callback, outside the loader lock.
 * Everything is runtime-gated by D2TESTDRV_*
 * env vars; compile-gated by D2_TESTDRV (no macro -> the whole system is absent).
 */

#ifndef TESTDRV_TESTDRV_H
#define TESTDRV_TESTDRV_H

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace game {
struct CPhaseGame;
}

namespace hooks {
namespace testdrv {

/** Apply the early boot byte-patches (skip-intro / fg-flag). Call first in DllMain. */
void installEarly();

/** Install only loader-safe hooks and capture the runtime plan. `self` is sent
 * to the relay in Hello once the UI starts. */
bool install(HMODULE self);

/** Start the UI-thread executor and optional bridge exactly once. Called by the
 * first typed UI callback, never from DllMain. */
void startRuntimeFromUi();

/** Resolve and validate the current game phase without retaining a scenario pointer. */
game::CPhaseGame* livePhaseGame();

/** True when the stock sequential client would admit a strategic action: it owns the turn and the
 * phase's existing object-lock predicate is clear. UI-thread only. */
bool strategicActionReady();

} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_TESTDRV_H
