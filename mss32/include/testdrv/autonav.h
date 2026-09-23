/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Auto-nav: drives the menu chain hands-free, on the UI thread, by invoking
 * buttons' onClicked functors directly (no synthetic input). A scripted step
 * targeting a dialog fires once that dialog is the current one (per the UI-state
 * reporter). The driver ticks from the shared natural screen-loop dispatcher on
 * the game's UI thread. The script is selected from D2TESTDRV_ROLE.
 * Compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_AUTONAV_H
#define TESTDRV_AUTONAV_H

#include <cstdint>

namespace game {
struct CButtonInterf;
struct CBFunctorDispatch0;
}

namespace hooks {
namespace testdrv {
namespace autonav {

/** Stage the immutable navigation mode and reserve the shared natural-frame
 * callback before the ordinary hook transaction commits. */
bool preflight(bool selfnav, bool relay, bool autoDismiss,
               bool autoBattlePrearm, bool scriptedPopups,
               bool scriptedPopupConfirmations);

/** Publish activation after every required hook/registration has committed.
 * Any violated preflight invariant terminates fail-closed. */
void activateAfterHooks();

/** Called from the bind hook (UI thread) whenever a dialog binds: arm the nav
 * the first time the UI exists and capture the first battle at the bind event,
 * before readiness polling. */
void onDialogBound(const char* dialogName, const char* buttonName,
                   std::uint32_t appearance, std::uint32_t ownerInstance,
                   game::CButtonInterf* exactButton);

/** Irrevocably claim one native scripted-popup action in the same semantic
 * ledger used by relay commands, log that claim, then enqueue the sole
 * exact-identity callback for a later point in this natural UI frame. */
void claimAndEnqueueScriptedPopupAction(const char* dialogName,
                                        const char* buttonName,
                                         std::uint32_t appearance,
                                         std::uint32_t ownerInstance,
                                         std::uint32_t bindAgeMs,
                                         game::CButtonInterf* exactButton,
                                         game::CBFunctorDispatch0* exactFunctor);

/** Advance the nav by one step on the dialog-owning natural UI frame. */
void tick();

} // namespace autonav
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_AUTONAV_H

