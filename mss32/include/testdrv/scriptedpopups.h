/*
 * Removable DebugTest-only, exact-once popup subscriber.
 *
 * The bind hook only observes/captures. A later natural UI frame proves the
 * exact dialog owner ready for at least 300 ms, then the module claims and
 * enqueues one functor invocation through autonav's shared command ledger.
 */

#ifndef TESTDRV_SCRIPTEDPOPUPS_H
#define TESTDRV_SCRIPTEDPOPUPS_H

#include <cstdint>

namespace game {
struct CButtonInterf;
}

namespace hooks {
namespace testdrv {
namespace scriptedpopups {

/** Validate the immutable opt-in and exact host/join role. Default-off. */
bool preflight(bool enabled, bool confirmations, const char* role);

/** Activate only after the UI reporter and natural-frame seam committed. */
void activateAfterHooks();

/** Published with the UI snapshot until the paired relay release is applied on the UI thread. */
bool startupActionsHeld();

/** Bridge-thread control receipt only; no native game state is read here. */
void receiveStartupRelease(std::uint32_t payloadSize);

/** Capture an eligible button after its stock functor was really bound.
 * This callback never invokes or queues an action. */
void onDialogBound(const char* dialogName, const char* buttonName,
                   std::uint32_t appearance, std::uint32_t ownerInstance,
                   game::CButtonInterf* exactButton);

/** Open post-battle successor capture after the native battle-result close
 * owns the common ledger and passes exact identity validation, but before its
 * sole callback can bind a successor reentrantly. */
void onBattleResultCloseClaimed(std::uint32_t appearance,
                                std::uint32_t ownerInstance);

/** Observe readiness/age and submit at most one exact-identity action. Called
 * once per natural UI frame, after the reporter refreshed the topmost dialog. */
void tick();

} // namespace scriptedpopups
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_SCRIPTEDPOPUPS_H

