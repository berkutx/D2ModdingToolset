/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Auto-nav: drives the menu chain hands-free, on the UI thread, by invoking
 * buttons' onClicked functors directly (no synthetic input). A scripted step
 * targeting a dialog fires once that dialog is the current one (per the UI-state
 * reporter). The driver ticks from the game's screen-loop callback on the UI thread. The script
 * is selected from D2TESTDRV_ROLE. This is the mod's first real UI automation.
 * Compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_AUTONAV_H
#define TESTDRV_AUTONAV_H

namespace hooks {
namespace testdrv {
namespace autonav {

/** Called once the UI-state reporter is installed: select the nav script from
 * D2TESTDRV_ROLE (no-op if unset). */
bool onUiReady();

/** Called from the bind hook (UI thread) whenever a dialog binds: arm the nav the
 * first time the UI exists. */
void onDialogBound();

/** Advance the nav by one step. Called from the assignFunctor hook so it runs on
 * the dialog-owning thread (safe to invoke functors there). No-op until armed. */
void tick();

} // namespace autonav
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_AUTONAV_H
