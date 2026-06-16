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
};
DlgEntry g_registry[64] = {};
int g_regCount = 0;

void registerDialog(const char* name, game::CDialogInterf* ptr)
{
    for (int i = 0; i < g_regCount; ++i) {
        if (lstrcmpA(g_registry[i].name, name) == 0) {
            g_registry[i].ptr = ptr;
            return;
        }
    }
    if (g_regCount < (int)(sizeof(g_registry) / sizeof(g_registry[0]))) {
        lstrcpynA(g_registry[g_regCount].name, name, sizeof(g_registry[g_regCount].name));
        g_registry[g_regCount].ptr = ptr;
        ++g_regCount;
    }
}

void recordBind(game::CDialogInterf* dialog, const char* dialogName, const char* buttonName)
{
    if (!dialogName || !buttonName)
        return;
    g_curDialog = dialog;
    registerDialog(dialogName, dialog);
    if (lstrcmpA(g_lastDialog, dialogName) != 0) {
        lstrcpynA(g_lastDialog, dialogName, sizeof(g_lastDialog));
        g_buttonsCsv[0] = 0; // new screen -> fresh button list
        spdlog::info("[testdrv] dialog now: {}", dialogName); // the "current screen" signal
    }
    // Accumulate the current dialog's buttons (comma-separated) for the live UI scan.
    if (lstrlenA(g_buttonsCsv) + lstrlenA(buttonName) + 2 < (int)sizeof(g_buttonsCsv)) {
        if (g_buttonsCsv[0])
            lstrcatA(g_buttonsCsv, ",");
        lstrcatA(g_buttonsCsv, buttonName);
    }
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
