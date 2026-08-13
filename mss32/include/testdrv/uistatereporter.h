/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * UI-state reporter: Detours the game's button-bind helper
 * (CButtonInterfApi::assignFunctor) to track the current dialog, then enumerates
 * ALL of that dialog's controls (buttons, list boxes, spin buttons, edit boxes,
 * text) with their live state into a JSON snapshot, the "true path" replacing
 * screenshots. Native menus bind through assignFunctor, so one hook catches the
 * whole menu chain; the snapshot is the relay's GET /api/ui payload, and the
 * current dialog is exposed for the auto-nav driver. Gated at runtime by
 * D2TESTDRV_UI_REPORTER; compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_UISTATEREPORTER_H
#define TESTDRV_UISTATEREPORTER_H

#include <cstdint>
#include <string>

namespace game {
struct CDialogInterf;
}

namespace hooks {
namespace testdrv {
namespace uistatereporter {

/** Install the assignFunctor Detour if D2TESTDRV_UI_REPORTER. No-op on a non-Russobit
 * image or if the env gate is off. */
bool install();

/** The current (last-bound) dialog, or null before any bind. */
game::CDialogInterf* currentDialog();
/** Name of the current dialog (empty string before any bind). */
const char* currentDialogName();

/** Look up a bound dialog by name, or null if it was never bound / has closed.
 * D2 co-presents nested dialogs (e.g. DLG_CHOOSE_SKIRMISH inside DLG_HOST), so the
 * "current" dialog is not always the one a button lives in, auto-nav resolves the
 * target dialog by name through this registry, not by the last bind. */
game::CDialogInterf* findDialog(const char* name);

/** Re-sync the current dialog to the engine's REAL topmost interface (so a modal closing
 * over an already-bound dialog isn't reported stale) and rebuild the widget snapshot.
 * Cheap; called once per frame from the auto-nav tick (the UI thread). */
void refreshCurrentDialog();

/** Copy the current dialog's widget snapshot (JSON: {"dialog":..,"widgets":[{name,type,state}]})
 * and its change epoch. Thread-safe, the bridge thread calls this; the snapshot is built on
 * the UI thread under the same lock. Returns false before the first dialog exists. */
bool copyUiSnapshot(std::string& outJson, std::uint32_t& outEpoch);

} // namespace uistatereporter
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_UISTATEREPORTER_H
