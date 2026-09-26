/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * UI-state reporter: Observes native call sites and the MSS API table for the
 * game's button-bind helper (CButtonInterfApi::assignFunctor), then enumerates
 * ALL of that dialog's controls (buttons, list boxes, spin buttons, edit boxes,
 * text) with their live state into a JSON snapshot, the "true path" replacing
 * screenshots. Native menus bind through assignFunctor, so one hook catches the
 * whole menu chain; the snapshot is the relay's GET /api/ui payload, and the
 * current dialog is exposed for the auto-nav driver. The canonical helper entry
 * remains owned by C4; native and MSS bindings each reach it exactly once. Gated at runtime by
 * D2TESTDRV_UI_REPORTER; compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_UISTATEREPORTER_H
#define TESTDRV_UISTATEREPORTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace game {
struct CButtonInterf;
struct CBFunctorDispatch0;
struct CDialogInterf;
}

namespace hooks {
namespace testdrv {
namespace uistatereporter {

/** Read-only exact-Russobit proof of the canonical assignFunctor target and
 * all 178 direct CALL sites. */
bool preflight();

/** Patch the already preflighted direct CALL operands as one rollback-safe
 * bundle, then attach the same observer to the MSS API table. The assignFunctor
 * entry remains owned by C4/timerhost. */
bool commit();

/** Compatibility one-shot wrapper. New startup uses preflight/commit. */
bool install();

/** UI-thread observation of the same ordered room names displayed by the menu.
 * No credentials or transport identities are retained. */
void observeLobbyRooms(game::CDialogInterf* dialog, std::vector<std::string> names);
bool isExpectedLobbyRoomSelected(game::CDialogInterf* dialog, const char* expected);

/** The current (last-bound) dialog, or null before any bind. */
game::CDialogInterf* currentDialog();
/** Name of the current dialog (empty string before any bind). */
const char* currentDialogName();

/** Look up a bound dialog by name, or null if it was never bound / has closed.
 * D2 co-presents nested dialogs (e.g. DLG_CHOOSE_SKIRMISH inside DLG_HOST), so the
 * "current" dialog is not always the one a button lives in, auto-nav resolves the
 * target dialog by name through this registry, not by the last bind. */
game::CDialogInterf* findDialog(const char* name);

/** UI-thread-only causal guard for a remote UI command. Returns true only while
 * `expectedAppearance` is the current visible-screen event and `expectedOwnerInstance`
 * is still its exact ready native CDialog owner. A co-present DLG_STRATEGIC owner is
 * accepted only when its independently published token maps to the same topmost screen. */
bool isReadyDialogInstance(const char* requestedDialog, std::uint32_t expectedAppearance,
                           std::uint32_t expectedOwnerInstance);

/** The same exact ready-owner guard plus its age measured from the first bind
 * of that native owner. For DLG_BATTLE_A this is the first bind of the battle
 * epoch: later result/control rebinds preserve the owner and clock until the
 * return to DLG_STRATEGIC. UI-thread only. */
bool getReadyDialogInstanceAge(const char* requestedDialog,
                               std::uint32_t expectedAppearance,
                               std::uint32_t expectedOwnerInstance,
                               std::uint32_t& elapsedMs);

/** Capture the exact currently visible ready owner, its appearance generation,
 * and age from that owner's first bind. Unlike the expected-identity overload,
 * this is for a preboot intent which cannot receive identity through HTTP. The
 * requested dialog must itself be current; co-present owners are rejected. */
bool getReadyCurrentDialogInstanceAge(const char* requestedDialog,
                                      std::uint32_t& appearance,
                                      std::uint32_t& ownerInstance,
                                      std::uint32_t& elapsedMs);

/** Prove that the exact active battle owner is still the ready topmost dialog
 * and that the exact post-result button returned by assignFunctor is enabled
 * and still owns the same stock callback. No fallible name re-resolution is
 * involved. UI-thread only. */
bool isReadyBattleResultCloseInstance(std::uint32_t expectedAppearance,
                                      std::uint32_t expectedOwnerInstance,
                                      game::CButtonInterf* exactButton,
                                      game::CBFunctorDispatch0* exactFunctor);

/** UI-thread-only causal guard for a world action. Returns true only while the
 * exact published root owner is a ready bare strategic map (DLG_STRATEGIC or
 * DLG_ISO_PAL). Unlike a world-snapshot epoch, this appearance retires when a
 * battle/modal takes over and therefore cannot admit an engaged-stack refire. */
bool isReadyStrategicMapInstance(std::uint32_t expectedAppearance,
                                 std::uint32_t expectedOwnerInstance);

/** UI-thread-only passive proof that the exact live strategic phase belongs to
 * the active client, owns its exact data cache/command queue, can take its turn,
 * and has no object-lock, RX, deferred packet, queued UI continuation, or native
 * command work. The current game's native admission predicate is also required, so any
 * installed gameplay feature enforces the same policy as for a real UI action. It never waits, drains, retries, or changes
 * game state. False is the fail-closed answer outside the exact live phase. */
bool isStrategicIdle();

/** Re-sync the current dialog to the engine's REAL topmost interface (so a modal closing
 * over an already-bound dialog isn't reported stale) and rebuild the widget snapshot.
 * Cheap; called once per frame from the auto-nav tick (the UI thread). */
void refreshCurrentDialog();

/** Copy the current dialog's widget snapshot
 * (JSON: {"dialog":..,"instance":uint,"ready":bool,"strategicIdle":bool,"widgets":[...],
 *          "targets":[{"dialog":..,"instance":ownerToken,"widgets":[...]}]}).
 * Root `instance` is a monotonic appearance generation, including consecutive same-named popup
 * pages. Each target token identifies the exact native CDialog owner that may receive an action.
 * `ready` is published only after the next screen-loop frame following the last bind,
 * so a remote action cannot re-enter a half-built modal. Thread-safe, the bridge thread calls this;
 * the snapshot is built on the UI thread under the same lock. Returns false before the first
 * dialog exists. */
bool copyUiSnapshot(std::string& outJson, std::uint32_t& outEpoch);

} // namespace uistatereporter
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_UISTATEREPORTER_H
