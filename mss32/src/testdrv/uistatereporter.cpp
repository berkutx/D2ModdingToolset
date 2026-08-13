/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * UI-state reporter. See testdrv/uistatereporter.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#include "testdrv/uistatereporter.h"
#include "testdrv/autonav.h"
#include "testdrv/testenv.h"
#include "testdrv/testdrv.h"
#include "button.h"
#include "dialoginterf.h"
#include "editboxinterf.h"
#include "interfmanager.h"
#include "listbox.h"
#include "smartptr.h"
#include "spinbuttoninterf.h"
#include "textboxinterf.h"
#include "version.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
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

// The widget snapshot for the current dialog. Built on the UI thread (rebuildSnapshot),
// read by the bridge thread (copyUiSnapshot), both under g_snapMutex.
std::mutex g_snapMutex;
std::string g_snapJson;
std::uint32_t g_snapEpoch = 0;

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

const char* screenName(void* screen)
{
    for (int i = 0; i < g_screenCount; ++i)
        if (g_screens[i].screen == screen)
            return g_screens[i].name;
    return nullptr;
}

DlgEntry* registerDialog(const char* name, game::CDialogInterf* ptr)
{
    for (int i = 0; i < g_regCount; ++i) {
        if (lstrcmpA(g_registry[i].name, name) == 0) {
            g_registry[i].ptr = ptr; // re-opened instance -> refresh the pointer
            return &g_registry[i];
        }
    }
    if (g_regCount < (int)(sizeof(g_registry) / sizeof(g_registry[0]))) {
        DlgEntry* e = &g_registry[g_regCount++];
        lstrcpynA(e->name, name, sizeof(e->name));
        e->ptr = ptr;
        return e;
    }
    return nullptr;
}

// --- widget enumeration -------------------------------------------------------
// A dialog's controls live in CDialogInterfData::childControls (name -> child index). We harvest
// the names there and resolve each through the engine's typed finders (findButton/findListBox/...)
// to learn its type and read its live state. POD-only + SEH-guarded: a control can be half-built
// or a dialog torn down mid-walk, and __try cannot share a scope with std::string (unwinding).

enum WidgetKind
{
    WK_button = 0,
    WK_listbox,
    WK_spin,
    WK_edit,
    WK_text,
    WK_picture,
    WK_toggle,
    WK_radio,
    WK_scrollbar,
    WK_other
};

const char* kindName(int k)
{
    switch (k) {
    case WK_button: return "button";
    case WK_listbox: return "listbox";
    case WK_spin: return "spin";
    case WK_edit: return "edit";
    case WK_text: return "text";
    case WK_picture: return "picture";
    case WK_toggle: return "toggle";
    case WK_radio: return "radio";
    case WK_scrollbar: return "scrollbar";
    default: return "other";
    }
}

struct WidgetInfo
{
    char name[48];
    int kind;
    int i1;          // button: enabled(1/0/-1); listbox: selected; spin: option index
    int i2;          // listbox: total
    char text[128];  // edit/text content, spin current-option text
};
constexpr int kMaxWidgets = 96;

// SEH-guarded raw walk. No C++ unwinding objects in scope (POD only) so __try is legal.
int enumerateWidgetsRaw(game::CDialogInterf* dlg, WidgetInfo* out, int maxN)
{
    int n = 0;
    __try {
        auto& api = game::CDialogInterfApi::get();
        auto& controls = dlg->data->childControls;
        for (auto it = controls.begin(); it != controls.end() && n < maxN; ++it) {
            const char* name = it->first;
            if (!name || !name[0])
                continue;
            WidgetInfo& w = out[n];
            memset(&w, 0, sizeof(w));
            lstrcpynA(w.name, name, sizeof(w.name));
            w.kind = WK_other;
            w.i1 = -1;
            w.i2 = -1;

            if (game::CButtonInterf* b = api.findButton(dlg, name)) {
                w.kind = WK_button;
                if (b->buttonData)
                    w.i1 = b->buttonData->enabled ? 1 : 0;
            } else if (game::CListBoxInterf* lb = api.findListBox(dlg, name)) {
                w.kind = WK_listbox;
                if (lb->listBoxData) {
                    w.i1 = lb->listBoxData->selectedElement;
                    w.i2 = lb->listBoxData->elementsTotal;
                }
            } else if (game::CSpinButtonInterf* sp = api.findSpinButton(dlg, name)) {
                w.kind = WK_spin;
                if (sp->data) {
                    w.i1 = sp->data->selectedOption;
                    const auto& opts = sp->data->options;
                    const int total = (int)(opts.end - opts.bgn);
                    if (w.i1 >= 0 && w.i1 < total && opts.bgn[w.i1].string)
                        lstrcpynA(w.text, opts.bgn[w.i1].string, sizeof(w.text));
                }
            } else if (game::CEditBoxInterf* eb = api.findEditBox(dlg, name)) {
                w.kind = WK_edit;
                if (eb->data && eb->data->editBoxData.inputString.string)
                    lstrcpynA(w.text, eb->data->editBoxData.inputString.string, sizeof(w.text));
            } else if (game::CTextBoxInterf* tx = api.findTextBox(dlg, name)) {
                w.kind = WK_text;
                if (tx->data && tx->data->text.string)
                    lstrcpynA(w.text, tx->data->text.string, sizeof(w.text));
            } else if (api.findPicture(dlg, name)) {
                w.kind = WK_picture;
            } else if (api.findToggleButton(dlg, name)) {
                w.kind = WK_toggle;
            } else if (api.findRadioButton(dlg, name)) {
                w.kind = WK_radio;
            } else if (api.findScrollBar && api.findScrollBar(dlg, name)) {
                w.kind = WK_scrollbar; // editor-only finder; its game-side address may be unset
            }
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // a torn-down dialog faults mid-walk: return the widgets gathered so far
    }
    return n;
}

// --- JSON building (no SEH; std::string is fine here) -------------------------
void appendJsonString(std::string& out, const char* s)
{
    out += '"';
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
            const unsigned char c = *p;
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    // Escape control + non-ASCII bytes as \u00XX (Latin-1 codepoint): always valid
                    // JSON without a cp1251 table. Names/types are ASCII; only Russian body text hits this.
                    char buf[8];
                    wsprintfA(buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
            }
        }
    }
    out += '"';
}

void appendInt(std::string& out, const char* key, int v)
{
    out += '"';
    out += key;
    out += "\":";
    char buf[16];
    wsprintfA(buf, "%d", v);
    out += buf;
}

// Build the snapshot for the current dialog and publish it if it changed (bumping the epoch).
// UI-thread only: enumerateWidgetsRaw reads live game UI structures.
void rebuildSnapshot()
{
    game::CDialogInterf* dlg = g_curDialog;
    char dlgName[48];
    lstrcpynA(dlgName, g_lastDialog, sizeof(dlgName));

    static WidgetInfo s_widgets[kMaxWidgets]; // UI-thread only -> safe as a static scratch buffer
    int n = (dlg && dlgName[0]) ? enumerateWidgetsRaw(dlg, s_widgets, kMaxWidgets) : 0;

    std::string json;
    json.reserve(128 + (size_t)n * 80);
    json += "{\"dialog\":";
    appendJsonString(json, dlgName);
    json += ",\"widgets\":[";
    for (int i = 0; i < n; ++i) {
        const WidgetInfo& w = s_widgets[i];
        if (i)
            json += ',';
        json += "{\"name\":";
        appendJsonString(json, w.name);
        json += ",\"type\":\"";
        json += kindName(w.kind);
        json += "\",\"state\":{";
        switch (w.kind) {
        case WK_button:
            json += "\"enabled\":";
            json += (w.i1 == 1) ? "true" : (w.i1 == 0 ? "false" : "null");
            break;
        case WK_listbox:
            appendInt(json, "selected", w.i1);
            json += ',';
            appendInt(json, "total", w.i2);
            break;
        case WK_spin:
            appendInt(json, "index", w.i1);
            json += ",\"text\":";
            appendJsonString(json, w.text);
            break;
        case WK_edit:
        case WK_text:
            json += "\"text\":";
            appendJsonString(json, w.text);
            break;
        default:
            break;
        }
        json += "}}";
    }
    json += "]}";

    std::lock_guard<std::mutex> lk(g_snapMutex);
    if (json != g_snapJson) {
        g_snapJson.swap(json);
        ++g_snapEpoch;
    }
}

void recordBind(game::CDialogInterf* dialog, const char* dialogName, const char* buttonName)
{
    if (!dialogName || !buttonName)
        return;
    g_curDialog = dialog;
    registerDialog(dialogName, dialog);
    // The just-bound dialog is being shown ON the current topmost screen, let the per-frame poll
    // learn that screen<->name association (so a later close that reveals it can be reported).
    lstrcpynA(g_pendingBind, dialogName, sizeof(g_pendingBind));
    // Immediate "current screen" update on open; refreshCurrentDialog() corrects staleness on close.
    if (lstrcmpA(g_lastDialog, dialogName) != 0) {
        lstrcpynA(g_lastDialog, dialogName, sizeof(g_lastDialog));
        spdlog::info("[testdrv] dialog now: {}", dialogName); // the "current screen" signal
    }
    spdlog::info("[testdrv] bind {}::{}", dialogName, buttonName);
    rebuildSnapshot(); // first widgets appear promptly, before the next per-frame tick
}

game::CButtonInterf* __stdcall hookAssignFunctor(game::CDialogInterf* dialog, const char* buttonName,
                                                 const char* dialogName, game::SmartPointer* functor,
                                                 int hotkey)
{
    ::hooks::testdrv::startRuntimeFromUi();
    recordBind(dialog, dialogName, buttonName);
    autonav::onDialogBound(); // arm the nav once the UI exists (ticking is done from the
                              // per-frame hook, not here, invoking here is reentrant)
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

bool copyUiSnapshot(std::string& outJson, std::uint32_t& outEpoch)
{
    std::lock_guard<std::mutex> lk(g_snapMutex);
    if (g_snapJson.empty())
        return false;
    outJson = g_snapJson;
    outEpoch = g_snapEpoch;
    return true;
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
    // real topmost INTERFACE (CInterfManager::getTopmostInterface), note it is the SCREEN that hosts a
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
    if (!top) {
        rebuildSnapshot();
        return;
    }
    if (g_pendingBind[0]) { // a dialog just opened on this topmost screen -> learn (last writer wins)
        mapScreen(top, g_pendingBind);
        g_pendingBind[0] = 0; // g_lastDialog was already set to it by recordBind
        rebuildSnapshot();
        return;
    }
    // No fresh bind: if the topmost is a known screen whose dialog differs from the reported one, a
    // modal closed and revealed it -> switch to it.
    const char* name = screenName(top);
    if (name && lstrcmpA(g_lastDialog, name) != 0) {
        lstrcpynA(g_lastDialog, name, sizeof(g_lastDialog));
        for (int i = 0; i < g_regCount; ++i)
            if (lstrcmpA(g_registry[i].name, name) == 0) {
                g_curDialog = g_registry[i].ptr;
                break;
            }
        spdlog::info("[testdrv] dialog now: {} (revealed)", g_lastDialog);
    }
    rebuildSnapshot();
}

bool install()
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return false;
    if (!testenv::on("D2TESTDRV_UI_REPORTER"))
        return false;
    g_origAssignFunctor = game::CButtonInterfApi::get().assignFunctor;
    if (!g_origAssignFunctor) {
        spdlog::warn("[testdrv] UI-state: assignFunctor unresolved");
        return false;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&g_origAssignFunctor), &hookAssignFunctor);
    const LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) {
        spdlog::info("[testdrv] UI-state reporter installed (assignFunctor hooked)");
    } else {
        spdlog::error("[testdrv] UI-state: DetourAttach failed err={:d}", err);
    }
    return err == NO_ERROR;
}

} // namespace uistatereporter
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
