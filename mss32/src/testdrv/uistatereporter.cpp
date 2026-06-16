/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * UI-state reporter — see testdrv/uistatereporter.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#include "testdrv/uistatereporter.h"
#include "testdrv/autonav.h"
#include "testdrv/testenv.h"
#include "button.h"
#include "dialoginterf.h"
#include "interfmanager.h"
#include "smartptr.h"
#include "version.h"
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

namespace hooks {
namespace testdrv {
namespace uistatereporter {

namespace {

using FnAssignFunctor = game::CButtonInterf*(__stdcall*)(game::CDialogInterf*, const char*,
                                                         const char*, game::SmartPointer*, int);
FnAssignFunctor g_origAssignFunctor = nullptr;

char g_lastDialog[48] = {};
char g_buttonsCsv[512] = {};                // buttons bound for the current dialog (reset on change)
game::CDialogInterf* g_curDialog = nullptr; // live ptr of the current (last-bound) dialog

// Registry of bound dialogs by name (D2 keeps several co-present). Last writer per
// name wins, so a re-opened dialog refreshes its pointer.
struct DlgEntry
{
    char name[48];
    game::CDialogInterf* ptr;
    char buttons[512]; // this dialog's bound buttons — restored when it becomes the topmost again
};
DlgEntry g_registry[64] = {};
int g_regCount = 0;
char g_pendingBind[48] = {}; // a dialog just bound -> associate the current topmost screen with it

// Engine topmost-interface ptr -> the dialog name currently hosted on it. A screen is reused
// (DLG_SESSION then DLG_LOBBY) and co-presents (DLG_ISO_PAL + DLG_STRATEGIC), so LAST-writer wins;
// the per-frame poll uses this to name the revealed dialog when a modal above it closes.
struct ScreenEntry
{
    void* screen;
    char name[48];
};
ScreenEntry g_screens[64] = {};
int g_screenCount = 0;

void mapScreen(void* screen, const char* name)
{
    if (!screen)
        return;
    for (int i = 0; i < g_screenCount; ++i) {
        if (g_screens[i].screen == screen) {
            lstrcpynA(g_screens[i].name, name, sizeof(g_screens[i].name)); // last writer wins
            return;
        }
    }
    if (g_screenCount < (int)(sizeof(g_screens) / sizeof(g_screens[0]))) {
        g_screens[g_screenCount].screen = screen;
        lstrcpynA(g_screens[g_screenCount].name, name, sizeof(g_screens[g_screenCount].name));
        ++g_screenCount;
    }
}

DlgEntry* findEntry(const char* name)
{
    for (int i = 0; i < g_regCount; ++i)
        if (lstrcmpA(g_registry[i].name, name) == 0)
            return &g_registry[i];
    return nullptr;
}

DlgEntry* registerDialog(const char* name, game::CDialogInterf* ptr)
{
    for (int i = 0; i < g_regCount; ++i) {
        if (lstrcmpA(g_registry[i].name, name) == 0) {
            if (g_registry[i].ptr != ptr) { // re-opened instance -> start its button list fresh
                g_registry[i].ptr = ptr;
                g_registry[i].buttons[0] = 0;
            }
            return &g_registry[i];
        }
    }
    if (g_regCount < (int)(sizeof(g_registry) / sizeof(g_registry[0]))) {
        DlgEntry* e = &g_registry[g_regCount++];
        lstrcpynA(e->name, name, sizeof(e->name));
        e->ptr = ptr;
        e->buttons[0] = 0;
        return e;
    }
    return nullptr;
}

void recordBind(game::CDialogInterf* dialog, const char* dialogName, const char* buttonName)
{
    if (!dialogName || !buttonName)
        return;
    g_curDialog = dialog;
    DlgEntry* e = registerDialog(dialogName, dialog);
    // Accumulate this dialog's buttons into its registry entry (its persistent list).
    if (e && lstrlenA(e->buttons) + lstrlenA(buttonName) + 2 < (int)sizeof(e->buttons)) {
        if (e->buttons[0])
            lstrcatA(e->buttons, ",");
        lstrcatA(e->buttons, buttonName);
    }
    // The just-bound dialog is being shown ON the current topmost screen — let the per-frame poll
    // learn that screen<->name association (so a later close that reveals it can be reported).
    lstrcpynA(g_pendingBind, dialogName, sizeof(g_pendingBind));
    // Immediate "current screen" update on open; refreshCurrentDialog() corrects staleness on close.
    if (lstrcmpA(g_lastDialog, dialogName) != 0) {
        lstrcpynA(g_lastDialog, dialogName, sizeof(g_lastDialog));
        spdlog::info("[testdrv] dialog now: {}", dialogName); // the "current screen" signal
    }
    if (e)
        lstrcpynA(g_buttonsCsv, e->buttons, sizeof(g_buttonsCsv));
    // Log every (dialog,button) so the real names can be discovered live.
    spdlog::info("[testdrv] bind {}::{}", dialogName, buttonName);
}

game::CButtonInterf* __stdcall hookAssignFunctor(game::CDialogInterf* dialog, const char* buttonName,
                                                 const char* dialogName, game::SmartPointer* functor,
                                                 int hotkey)
{
    recordBind(dialog, dialogName, buttonName);
    autonav::onDialogBound(); // arm the nav once the UI exists (ticking is done from the
                              // per-frame hook, not here — invoking here is reentrant)
    return g_origAssignFunctor(dialog, buttonName, dialogName, functor, hotkey);
}

} // namespace

game::CDialogInterf* currentDialog()
{
    return g_curDialog;
}

const char* currentDialogName()
{
    return g_lastDialog;
}

const char* currentButtonsCsv()
{
    return g_buttonsCsv;
}

game::CDialogInterf* findDialog(const char* name)
{
    if (!name)
        return nullptr;
    for (int i = 0; i < g_regCount; ++i) {
        if (lstrcmpA(g_registry[i].name, name) != 0)
            continue;
        game::CDialogInterf* d = g_registry[i].ptr;
        if (!d)
            return nullptr;
        // Self-heal: a freed dialog faults when we probe its vtable -> purge + miss.
        __try {
            volatile void* probe = *reinterpret_cast<void* volatile*>(d);
            (void)probe;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_registry[i].ptr = nullptr;
            return nullptr;
        }
        return d;
    }
    return nullptr;
}

void refreshCurrentDialog()
{
    // The assignFunctor hook only fires on a button-bind, so a modal that closes to reveal an
    // already-bound dialog underneath would otherwise leave a STALE current-dialog. Poll the engine's
    // real topmost INTERFACE (CInterfManager::getTopmostInterface) — note it is the SCREEN that hosts a
    // dialog, NOT the CDialogInterf, and co-present dialogs (e.g. DLG_ISO_PAL + DLG_STRATEGIC) share one
    // screen ptr. So we LEARN screen<->name at bind time (the just-bound dialog lives on the current
    // topmost screen) and LOOK UP on close. Ticked per frame from autonav. SEH-guarded.
    void* top = nullptr;
    __try {
        game::InterfManagerImplPtr mgr{};
        game::CInterfManagerImplApi::get().get(&mgr);
        if (mgr.data)
            top = mgr.data->CInterfManagerImpl::CInterfManager::vftable->getTopmostInterface(mgr.data);
        game::SmartPointerApi::get().createOrFree(reinterpret_cast<game::SmartPointer*>(&mgr), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!top)
        return;
    if (g_pendingBind[0]) { // a dialog just opened on this topmost screen -> learn (last writer wins)
        mapScreen(top, g_pendingBind);
        g_pendingBind[0] = 0; // g_lastDialog was already set to it by recordBind
        return;
    }
    // No fresh bind: if the topmost is a known screen whose dialog differs from the reported one, a
    // modal closed and revealed it -> switch to it.
    for (int i = 0; i < g_screenCount; ++i) {
        if (g_screens[i].screen != top)
            continue;
        if (lstrcmpA(g_lastDialog, g_screens[i].name) != 0) {
            lstrcpynA(g_lastDialog, g_screens[i].name, sizeof(g_lastDialog));
            DlgEntry* e = findEntry(g_screens[i].name);
            lstrcpynA(g_buttonsCsv, e ? e->buttons : "", sizeof(g_buttonsCsv));
            g_curDialog = e ? e->ptr : g_curDialog;
            spdlog::info("[testdrv] dialog now: {} (revealed)", g_lastDialog);
        }
        return;
    }
    // Topmost is a screen we never associated (no button-bound dialog) -> keep the last known.
}

void install()
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return;
    if (!testenv::on("D2TESTDRV_UI_REPORTER"))
        return;
    g_origAssignFunctor = game::CButtonInterfApi::get().assignFunctor;
    if (!g_origAssignFunctor) {
        spdlog::warn("[testdrv] UI-state: assignFunctor unresolved");
        return;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&g_origAssignFunctor), &hookAssignFunctor);
    const LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) {
        spdlog::info("[testdrv] UI-state reporter installed (assignFunctor hooked)");
        autonav::onUiReady(); // arm auto-nav if D2TESTDRV_ROLE is set
    } else {
        spdlog::error("[testdrv] UI-state: DetourAttach failed err={:d}", err);
    }
}

} // namespace uistatereporter
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
