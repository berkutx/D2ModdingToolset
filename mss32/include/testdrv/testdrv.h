/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Installer / entry point. DllMain first parses and validates one immutable
 * runtime plan, then commits it only after the ordinary MSS Detours transaction.
 * Everything is runtime-gated by D2TESTDRV_* env vars; compile-gated by
 * D2_TESTDRV (no macro -> the whole system is absent).
 */

#ifndef TESTDRV_TESTDRV_H
#define TESTDRV_TESTDRV_H

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace game {
struct CDialogInterf;
struct CPhaseGame;
}

namespace hooks {
namespace testdrv {

/** Parse every D2TESTDRV_* feature gate once and perform all recoverable,
 * read-only validation before setupHooks(). No native hook/byte patch is applied. */
bool preflight();

/** Commit the immutable plan after setupHooks() succeeds. Any impossible
 * post-commit invariant terminates fail-closed; this function never asks
 * DllMain to unload an already-mutated DLL. Never starts a thread. */
bool install(HMODULE self);

/** Earliest typed normal-runtime callback after DllMain; starts the one requested
 * relay helper exactly once. */
void startRuntimeFromUi(game::CDialogInterf* dialog);

/** Resolve the current live phase; no retained scenario pointer. */
game::CPhaseGame* livePhaseGame();

/** UI-thread observation of the current loaded scenario, independent of turn ownership/popups. */
bool mapLoaded();

/** Observe the real game's current turn/input admission and object lock. */
bool strategicActionReady();

} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_TESTDRV_H

