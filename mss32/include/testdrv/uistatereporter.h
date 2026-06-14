/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * UI-state reporter: Detours the game's button-bind helper
 * (CButtonInterfApi::assignFunctor) so the current dialog + its buttons can be
 * observed programmatically — the "true path" replacing screenshots. Native menus
 * bind through it, so one hook catches the whole menu chain and emits a live log
 * of every (dialog, button); the current dialog is exposed for the auto-nav
 * driver. Gated at runtime by D2TESTDRV_UI; compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_UISTATEREPORTER_H
#define TESTDRV_UISTATEREPORTER_H

namespace game {
struct CDialogInterf;
}

namespace hooks {
namespace testdrv {
namespace uistatereporter {

/** Install the assignFunctor Detour if D2TESTDRV_UI. No-op on a non-Russobit
 * image or if the env gate is off. */
void install();

/** The current (last-bound) dialog, or null before any bind. */
game::CDialogInterf* currentDialog();
/** Name of the current dialog (empty string before any bind). */
const char* currentDialogName();

/** Look up a bound dialog by name, or null if it was never bound / has closed.
 * D2 co-presents nested dialogs (e.g. DLG_CHOOSE_SKIRMISH inside DLG_HOST), so the
 * "current" dialog is not always the one a button lives in — auto-nav resolves the
 * target dialog by name through this registry, not by the last bind. */
game::CDialogInterf* findDialog(const char* name);

} // namespace uistatereporter
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_UISTATEREPORTER_H
