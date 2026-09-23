/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * UI-state reporter. See testdrv/uistatereporter.h.
 *
 * Compile-gated by D2_TESTDRV: production configurations do not compile this
 * translation unit or any of its reporter dependencies.
 */

#ifdef D2_TESTDRV

#include "testdrv/uistatereporter.h"
#include "testdrv/autonav.h"
#include "testdrv/scriptedpopups.h"
#include "testdrv/testdrv.h"
#include "testdrv/testenv.h"
#include "button.h"
#include "dialoginterf.h"
#include "dynamiccast.h"
#include "editboxinterf.h"
#include "interfmanager.h"
#include "listbox.h"
#include "midobjectlock.h"
#include "testdrv/networkobservers.h"
#include "phasegame.h"
#include "testdrv/json.h"
#include "smartptr.h"
#include "spinbuttoninterf.h"
#include "textboxinterf.h"
#include "togglebutton.h"
#include "version.h"
#include <cstddef>
#include <algorithm>
#include <utility>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace uistatereporter {

namespace {

constexpr uintptr_t kAssignFunctorVA = 0x5C93D6;
// Exact Russobit direct xrefs to CButtonInterf::assignFunctor. This is the working lobby's 173-site
// seam plus five real calls hidden from its IDA xref query by two incorrectly inferred noreturn
// functions (0x6306F8, 0x630768, 0x6307D8, 0x638CE4, 0x638D98). The offline proof scans the exact
// .text image and requires set equality. C4's timerhost legitimately owns an entry Detour at
// 0x5C93D6; keeping that entry intact gives the deterministic chain
// game call-site -> MSS reporter -> 0x5C93D6 -> C4 timerhost -> stock function.
constexpr uintptr_t kBindCallSites[] = {
    0x480806, 0x4817d9, 0x4822c0, 0x482302, 0x482988, 0x4829cd, 0x482a12,
    0x482a57, 0x482a9c, 0x482ae1, 0x482b25, 0x486a15, 0x486a55, 0x488d18,
    0x488dfe, 0x488e6b, 0x488ed8, 0x488f8e, 0x4901b8, 0x490215, 0x49026c,
    0x4902b4, 0x49030b, 0x490353, 0x49039b, 0x4903f1, 0x490438, 0x490480,
    0x4905cd, 0x490aad, 0x492fcf, 0x4933d6, 0x496059, 0x4960a1, 0x496d70,
    0x496dd0, 0x49864a, 0x4998dc, 0x49af3d, 0x49af82, 0x49c7be, 0x49cb41,
    0x49db14, 0x49eafa, 0x49f4ab, 0x49f4ea, 0x4a214e, 0x4a2193, 0x4a4759,
    0x4a479e, 0x4a5256, 0x4a529b, 0x4a5ff4, 0x4a6039, 0x4a6826, 0x4a76a6,
    0x4ab70f, 0x4ab754, 0x4af55a, 0x4af59d, 0x4af5e1, 0x4b085e, 0x4b1645,
    0x4b1749, 0x4b178e, 0x4b17d3, 0x4b1832, 0x4b55ab, 0x4b55f3, 0x4b5fb3,
    0x4b5ff8, 0x4b6045, 0x4b6bb3, 0x4b6bf7, 0x4b77a3, 0x4b77e1, 0x4b781f,
    0x4b785d, 0x4b7ee5, 0x4b7f29, 0x4b9b59, 0x4ba197, 0x4ba5a3, 0x4bacbe,
    0x4bad03, 0x4bad78, 0x4baf0e, 0x4bcdc0, 0x4bce05, 0x4bdd16, 0x4be7b4,
    0x4bedf7, 0x4bf107, 0x4bf14a, 0x4bfe44, 0x4bfe88, 0x4bfecc, 0x4bff3d,
    0x4bffa2, 0x4c0003, 0x4c0069, 0x4c32b8, 0x4c3d2c, 0x4c3d70, 0x4c3db4,
    0x4c55b9, 0x4c7686, 0x4c76c8, 0x4c770a, 0x4c774c, 0x4ca2d3, 0x4ca31a,
    0x4ca445, 0x4cb1d9, 0x4dc486, 0x4dc4e3, 0x4dced4, 0x4dcf14, 0x4dcf54,
    0x4dd28f, 0x4dd2d1, 0x4dd538, 0x4dd575, 0x4dd5c0, 0x4df896, 0x4df8d5,
    0x4e1eb9, 0x4e1efb, 0x4e1f3d, 0x4e1f7f, 0x4e1fce, 0x4e201d, 0x4e48dd,
    0x4e4923, 0x4e4969, 0x4e49af, 0x4e6540, 0x4e6582, 0x4e6625, 0x4e6731,
    0x4e8b35, 0x4e8b75, 0x4e8bb5, 0x4e8bf5, 0x4e8c35, 0x4e8c75, 0x4e8cb5,
    0x4e90cf, 0x4e910f, 0x4e9151, 0x4e9191, 0x4e96f8, 0x4e9739, 0x4ecdab,
    0x4ecdeb, 0x4eeb50, 0x4eeb95, 0x4eebda, 0x4eec1f, 0x4ef65d, 0x4ef69f,
    0x4ef6e1, 0x4f12c8, 0x4f1308, 0x4f1348, 0x4f1388, 0x4f13c8, 0x4f1408,
    0x4f1448, 0x5c8894, 0x5c88ee, 0x5c892e, 0x630688, 0x6306f8, 0x630768,
    0x6307d8, 0x638ce4, 0x638d98,
};
constexpr std::size_t kBindCallSiteCount = sizeof(kBindCallSites) / sizeof(kBindCallSites[0]);
static_assert(kBindCallSiteCount == 178, "Russobit assignFunctor xref manifest changed");

using FnAssignFunctor = game::CButtonInterf*(__stdcall*)(game::CDialogInterf*, const char*,
                                                          const char*, game::SmartPointer*, int);
FnAssignFunctor g_origAssignFunctor = nullptr;
bool g_bindCallSitesInstalled = false;
game::CDialogInterf* g_lobbyRoomsDialog = nullptr;
std::vector<std::string> g_lobbyRoomNames; // UI thread only, menu display order

char g_lastDialog[48] = {};
game::CDialogInterf* g_curDialog = nullptr; // live ptr of the current (last-bound) dialog
// A bind is an early construction signal, not permission to invoke the dialog yet. It becomes
// ready only when the screen loop reaches the next frame after the last bind. This keeps a remote
// command out of the first re-entrant frame in which the game's command cascade is still building
// the modal (observed with DLG_EVENT_POPUP).
bool g_dialogReady = false;
// Dialog names are not identities: consecutive DLG_EVENT_POPUP pages have the same name but use
// distinct CDialogInterf objects. The monotonic appearance instance identifies the published UI
// transition; a separate owner token below identifies the exact CDialog that may receive an action.
std::uint32_t g_dialogInstance = 0;
std::uint32_t g_ownerInstanceCounter = 0;
std::uint32_t g_curOwnerInstance = 0;
DWORD g_curOwnerFirstBindTick = 0;
bool g_curOwnerFirstBindTickSet = false;
void* g_currentTopScreen = nullptr;
bool g_bindCycleOpen = false;

// The old green harness opened one battle clock on the first DLG_BATTLE_A bind and closed it only
// when DLG_STRATEGIC bound again. A battle rebinds controls after natural frames (notably the result
// BTN_CLOSE/PAPERDOLL path), so the generic construction-batch boundary cannot define its identity.
// Keep the exact native owner and first-bind tick for the whole battle epoch.
bool g_battleEpochActive = false;
game::CDialogInterf* g_battleEpochDialog = nullptr;
std::uint32_t g_battleEpochOwnerInstance = 0;
DWORD g_battleEpochFirstBindTick = 0;

// Registry of bound dialogs by name (D2 keeps several co-present). Last writer per
// name wins, so a re-opened dialog refreshes its pointer.
struct DlgEntry
{
    char name[48];
    game::CDialogInterf* ptr;
    std::uint32_t ownerInstance;
    DWORD firstBindTick;
    bool firstBindTickSet;
    void* screen;
    bool pendingScreen;
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

void advanceCounter(std::uint32_t& counter, unsigned exitCode, const char* label)
{
    if (counter == 0xffffffffu) {
        spdlog::critical("[testdrv] UI-state {} counter exhausted; terminating", label);
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), exitCode);
        ExitProcess(exitCode);
    }
    ++counter;
}

void selectDialogInstance(game::CDialogInterf* dialog, std::uint32_t ownerInstance,
                          DWORD firstBindTick, bool firstBindTickSet)
{
    if (dialog == g_curDialog && ownerInstance == g_curOwnerInstance)
        return;
    advanceCounter(g_dialogInstance, 0xD2E77322u, "dialog appearance");
    g_curDialog = dialog;
    g_curOwnerInstance = ownerInstance;
    g_curOwnerFirstBindTick = firstBindTick;
    g_curOwnerFirstBindTickSet = firstBindTickSet;
}

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

DlgEntry* findEntry(const char* name)
{
    if (!name)
        return nullptr;
    for (int i = 0; i < g_regCount; ++i)
        if (lstrcmpA(g_registry[i].name, name) == 0)
            return &g_registry[i];
    return nullptr;
}

DlgEntry* registerDialog(const char* name, game::CDialogInterf* ptr,
                         std::uint32_t ownerInstance, DWORD firstBindTick,
                         bool firstBindTickSet)
{
    if (DlgEntry* e = findEntry(name)) {
        if (e->ownerInstance != ownerInstance) {
            e->screen = nullptr;
            e->ownerInstance = ownerInstance;
            e->firstBindTick = firstBindTick;
            e->firstBindTickSet = firstBindTickSet;
        }
        e->ptr = ptr; // re-opened instance -> refresh the pointer
        e->pendingScreen = true;
        return e;
    }
    if (g_regCount < (int)(sizeof(g_registry) / sizeof(g_registry[0]))) {
        DlgEntry* e = &g_registry[g_regCount++];
        lstrcpynA(e->name, name, sizeof(e->name));
        e->ptr = ptr;
        e->ownerInstance = ownerInstance;
        e->firstBindTick = firstBindTick;
        e->firstBindTickSet = firstBindTickSet;
        e->screen = nullptr;
        e->pendingScreen = true;
        return e;
    }
    return nullptr;
}

// --- widget enumeration -------------------------------------------------------
// A dialog's controls live in CDialogInterfData::childControls (name -> child index). Resolve each
// name once through the raw CDialogInterf::findControl seam, then classify it from the object's
// read-only RTTI metadata. The typed
// findButton/findListBox/... wrappers are deliberately forbidden here: every mismatched probe calls
// the game's AutoDialog logger, which opens a blocking "Invalid control" modal (for example, probing
// IMG_FIREFLY as a button on DLG_MAIN_MENU). Calling MSVC _RTDynamicCast/_RTtypeid is forbidden too:
// a control being destroyed during a dialog transition can have no complete-object locator yet and
// _RTtypeid throws "Access violation - no RTTI data!" before our caller regains control. POD-only +
// SEH-guarded: a control can be half-built or a dialog torn down mid-walk, and __try cannot share a
// scope with std::string (unwinding).

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
    int i1;          // button: enabled; toggle: checked; listbox: selected; spin: option index
    int i2;          // listbox: total
    char text[128];  // edit/text content, spin current-option text
};
constexpr int kMaxWidgets = 96;

int kindFromRttiName(const char* name)
{
    if (!name)
        return WK_other;
    if (strstr(name, "CButtonInterf@@"))
        return WK_button;
    if (strstr(name, "CListBoxInterf@@"))
        return WK_listbox;
    if (strstr(name, "CSpinButtonInterf@@"))
        return WK_spin;
    if (strstr(name, "CEditBoxInterf@@"))
        return WK_edit;
    if (strstr(name, "CTextBoxInterf@@"))
        return WK_text;
    if (strstr(name, "CPictureInterf@@"))
        return WK_picture;
    if (strstr(name, "CToggleButton@@"))
        return WK_toggle;
    if (strstr(name, "CRadioButtonInterf@@"))
        return WK_radio;
    if (strstr(name, "CScrollBarInterf@@"))
        return WK_scrollbar;
    return WK_other;
}

// Read the MSVC CompleteObjectLocator stored immediately before the vtable. This is deliberately a
// metadata walk, not a call to _RTtypeid/_RTDynamicCast. A caught access fault is reported separately:
// it proves the enumeration was torn and must close readiness, never masquerade as a legitimate
// unknown control.
int classifyControlRaw(const game::CInterface* control, bool* faulted)
{
    if (faulted)
        *faulted = false;
    if (!control)
        return WK_other;
    __try {
        const auto* vftable = *reinterpret_cast<const void* const* const*>(control);
        if (!vftable)
            return WK_other;
        const auto* locator =
            reinterpret_cast<const game::CompleteObjectLocator* const*>(vftable)[-1];
        if (!locator || !locator->typeDescriptor)
            return WK_other;

        int kind = kindFromRttiName(locator->typeDescriptor->name);
        if (kind != WK_other)
            return kind;

        // Derived controls keep the same CInterface-at-offset-zero layout. Walk their base-class
        // descriptors so a derived textbox/button is still reported with its usable base kind.
        const auto* hierarchy = locator->classDescriptor;
        if (!hierarchy || !hierarchy->baseClassArray || hierarchy->numBaseClasses > 64)
            return WK_other;
        for (std::uint32_t i = 0; i < hierarchy->numBaseClasses; ++i) {
            const auto* base = hierarchy->baseClassArray->baseClasses[i];
            if (!base || !base->typeDescriptor)
                continue;
            kind = kindFromRttiName(base->typeDescriptor->name);
            if (kind != WK_other)
                return kind;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (faulted)
            *faulted = true;
        return WK_other;
    }
    return WK_other;
}

// SEH-guarded raw walk. No C++ unwinding objects in scope (POD only) so __try is legal.
// A negative result means the snapshot was torn while it was being enumerated; callers must
// close readiness instead of publishing the partial list as actionable state.
int enumerateWidgetsRaw(game::CDialogInterf* dlg, WidgetInfo* out, int maxN)
{
    int n = 0;
    __try {
        auto& api = game::CDialogInterfApi::get();
        auto& controls = dlg->data->childControls;
        auto it = controls.begin();
        const auto end = controls.end();
        for (; it != end && n < maxN; ++it) {
            const char* name = it->first;
            if (!name || !name[0])
                continue;
            WidgetInfo& w = out[n];
            memset(&w, 0, sizeof(w));
            lstrcpynA(w.name, name, sizeof(w.name));
            w.kind = WK_other;
            w.i1 = -1;
            w.i2 = -1;

            game::CInterface* raw = api.findControl(dlg, name);
            if (!raw) {
                ++n;
                continue;
            }

            bool classificationFault = false;
            w.kind = classifyControlRaw(raw, &classificationFault);
            if (classificationFault)
                return -1;
            if (w.kind == WK_button) {
                auto* b = reinterpret_cast<game::CButtonInterf*>(raw);
                w.kind = WK_button;
                if (b->buttonData)
                    w.i1 = b->buttonData->enabled ? 1 : 0;
            } else if (w.kind == WK_listbox) {
                auto* lb = reinterpret_cast<game::CListBoxInterf*>(raw);
                if (lb->listBoxData) {
                    w.i1 = lb->listBoxData->selectedElement;
                    w.i2 = lb->listBoxData->elementsTotal;
                }
            } else if (w.kind == WK_toggle) {
                auto* toggle = reinterpret_cast<game::CToggleButton*>(raw);
                if (toggle->data)
                    w.i1 = toggle->data->checked ? 1 : 0;
            } else if (w.kind == WK_edit) {
                auto* eb = reinterpret_cast<game::CEditBoxInterf*>(raw);
                if (eb->data) {
                    // Never copy credentials into a shared UI snapshot.
                    if (eb->data->editBoxData.patched.isPassword || strstr(name, "PASS"))
                        lstrcpynA(w.text, "[redacted]", sizeof(w.text));
                    else if (eb->data->editBoxData.inputString.string)
                        lstrcpynA(w.text, eb->data->editBoxData.inputString.string, sizeof(w.text));
                }
            } else if (w.kind == WK_text) {
                auto* tx = reinterpret_cast<game::CTextBoxInterf*>(raw);
                if (tx->data && tx->data->text.string)
                    lstrcpynA(w.text, tx->data->text.string, sizeof(w.text));
            } else if (w.kind == WK_spin) {
                auto* sp = reinterpret_cast<game::CSpinButtonInterf*>(raw);
                if (sp->data) {
                    w.i1 = sp->data->selectedOption;
                    const auto& opts = sp->data->options;
                    const int total = (int)(opts.end - opts.bgn);
                    if (w.i1 >= 0 && w.i1 < total && opts.bgn[w.i1].string)
                        lstrcpynA(w.text, opts.bgn[w.i1].string, sizeof(w.text));
                }
            }
            ++n;
        }
        if (it != end)
            return -1; // bounded buffer exhaustion is also an incomplete observation
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// --- JSON building (no SEH; std::string is fine here) -------------------------
using json::appendEscaped;

using json::kvInt;

void appendUInt(std::string& out, const char* key, std::uint32_t v)
{
    out += '"';
    out += key;
    out += "\":";
    char buf[16];
    wsprintfA(buf, "%u", static_cast<unsigned>(v));
    out += buf;
}

void appendWidgets(std::string& json, const WidgetInfo* widgets, int count)
{
    json += '[';
    for (int i = 0; i < count; ++i) {
        const WidgetInfo& w = widgets[i];
        if (i)
            json += ',';
        json += "{\"name\":";
        appendEscaped(json, w.name);
        json += ",\"type\":\"";
        json += kindName(w.kind);
        json += "\",\"state\":{";
        switch (w.kind) {
        case WK_button:
            json += "\"enabled\":";
            json += (w.i1 == 1) ? "true" : (w.i1 == 0 ? "false" : "null");
            break;
        case WK_toggle:
            json += "\"checked\":";
            json += (w.i1 == 1) ? "true" : (w.i1 == 0 ? "false" : "null");
            break;
        case WK_listbox:
            kvInt(json, "selected", w.i1);
            json += ',';
            kvInt(json, "total", w.i2);
            if (lstrcmpA(w.name, "LBOX_ROOMS") == 0
                && g_curDialog == g_lobbyRoomsDialog
                && w.i2 == static_cast<int>(g_lobbyRoomNames.size())) {
                json += ",\"items\":[";
                for (std::size_t row = 0; row < g_lobbyRoomNames.size(); ++row) {
                    if (row)
                        json += ',';
                    appendEscaped(json, g_lobbyRoomNames[row].c_str());
                }
                json += ']';
            }
            break;
        case WK_spin:
            kvInt(json, "index", w.i1);
            json += ",\"text\":";
            appendEscaped(json, w.text);
            break;
        case WK_edit:
        case WK_text:
            json += "\"text\":";
            appendEscaped(json, w.text);
            break;
        default:
            break;
        }
        json += "}}";
    }
    json += ']';
}

void appendTarget(std::string& json, const char* dialog, std::uint32_t ownerInstance,
                  const WidgetInfo* widgets, int count)
{
    json += "{\"dialog\":";
    appendEscaped(json, dialog);
    json += ',';
    appendUInt(json, "instance", ownerInstance);
    json += ",\"widgets\":";
    appendWidgets(json, widgets, count);
    json += '}';
}

bool orderedWorkIsEmpty(const netintercept::OrderedWorkSnapshot& snapshot)
{
    return snapshot.receiveHookDepth == 0
           && snapshot.originalDispatchDepth == 0
           && snapshot.deferredPacketCount == 0
           && snapshot.uiTaskCount == 0
           && !snapshot.naturalFrameHadOrderedWork;
}

bool inspectNativeStrategicIdle(std::uint32_t& currentPlayerHandle)
{
    bool idle = false;
    currentPlayerHandle = 0;
    __try {
        game::CPhaseGame* phaseGame = testdrv::livePhaseGame();
        game::CPhaseGameData* data = phaseGame ? phaseGame->data : nullptr;
        game::CPhaseData* phaseData = phaseGame ? phaseGame->phase.data : nullptr;
        game::CMidClient* client = data ? data->midClient : nullptr;
        game::CMidObjectLock* objectLock =
            data ? data->midObjectLock : nullptr;

        game::CMidCommandQueue2* commandQueue = nullptr;
        game::CMidDataCache2* dataCache = nullptr;
        if (phaseGame && data && phaseData && client
            && phaseData->midClient == client && objectLock) {
            commandQueue =
                game::CPhaseApi::get().getCommandQueue(&phaseGame->phase);
            dataCache = game::CPhaseApi::get().getDataCache(&phaseGame->phase);
        }

        // The final pointer comparisons close the native observation around
        // both phase API calls. No persistent notify iterator/latch is used:
        // commandUpdateApplied, notifyList, and currentNotify are not idle state.
        idle = phaseGame && data && phaseData && client && objectLock
               && commandQueue && dataCache
               && phaseGame->data == data
               && phaseGame->phase.data == phaseData
               && data->midClient == client
               && phaseData->midClient == client
               && data->midObjectLock == objectLock
               && objectLock->commandQueue == commandQueue
               && objectLock->dataCache == dataCache
               && data->clientTakesTurn
               && !objectLock->patched.exportingLeader
               && !objectLock->patched.movingStack
               && objectLock->pendingLocalUpdates == 0
               && objectLock->pendingNetworkUpdates == 0
               && commandQueue->started
               && !commandQueue->processingCommand
               && commandQueue->commandsList.length == 0;
        if (idle) {
            currentPlayerHandle =
                static_cast<std::uint32_t>(data->currentPlayerId.value);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        idle = false;
    }
    return idle;
}

bool nativeAdmissionAllows()
{
    return testdrv::strategicActionReady();
}

bool computeStrategicIdle()
{
    const DWORD uiThread = netintercept::mainThreadId();
    if (!uiThread || GetCurrentThreadId() != uiThread) {
        return false;
    }

    netintercept::OrderedWorkSnapshot before{};
    if (!netintercept::captureOrderedWorkSnapshotForTestdrv(before)
        || !orderedWorkIsEmpty(before))
        return false;

    std::uint32_t currentPlayerHandle = 0;
    if (!inspectNativeStrategicIdle(currentPlayerHandle)
        || !nativeAdmissionAllows())
        return false;

    netintercept::OrderedWorkSnapshot after{};
    return netintercept::captureOrderedWorkSnapshotForTestdrv(after)
           && orderedWorkIsEmpty(after)
           && before.epoch == after.epoch;
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
    if (n < 0) {
        // A partial enumeration is observation of teardown, never proof that a control is ready.
        g_dialogReady = false;
        n = 0;
    }

    // The map exposes two co-present CDia…7 tokens truncated…rategic owner separately so
    // BTN_END_TURN is tied to its own native token, rather than borrowing the ISO dialog's identity.
    DlgEntry* strategic = nullptr;
    static WidgetInfo s_strategicWidgets[kMaxWidgets];
    int strategicN = 0;
    if (g_dialogReady && lstrcmpA(dlgName, "DLG_ISO_PAL") == 0) {
        DlgEntry* candidate = findEntry("DLG_STRATEGIC");
        if (candidate && candidate->ptr && candidate->ownerInstance != 0
            && candidate->screen == g_currentTopScreen) {
            strategic = candidate;
            strategicN = enumerateWidgetsRaw(candidate->ptr, s_strategicWidgets, kMaxWidgets);
            if (strategicN < 0) {
                g_dialogReady = false;
                strategicN = 0;
            }
        }
    }

    std::string json;
    json.reserve(128 + (size_t)n * 80);
    json += "{\"dialog\":";
    appendEscaped(json, dlgName);
    json += ',';
    appendUInt(json, "instance", g_dialogInstance);
    json += ",\"ready\":";
    json += g_dialogReady ? "true" : "false";
    json += ",\"mapLoaded\":";
    json += testdrv::mapLoaded() ? "true" : "false";
    json += ",\"startupActionsHeld\":";
    json += scriptedpopups::startupActionsHeld() ? "true" : "false";
    json += ",\"strategicIdle\":";
    json += computeStrategicIdle() ? "true" : "false";
    json += ",\"widgets\":";
    appendWidgets(json, s_widgets, n);
    json += ",\"targets\":[";
    bool haveTarget = false;
    if (dlg && dlgName[0] && g_curOwnerInstance != 0) {
        appendTarget(json, dlgName, g_curOwnerInstance, s_widgets, n);
        haveTarget = true;
    }
    if (strategic) {
        if (haveTarget)
            json += ',';
        appendTarget(json, strategic->name, strategic->ownerInstance, s_strategicWidgets, strategicN);
    }
    json += "]}";

    std::lock_guard<std::mutex> lk(g_snapMutex);
    if (json != g_snapJson) {
        g_snapJson.swap(json);
        ++g_snapEpoch;
    }
}

void beginBind(game::CDialogInterf* dialog, const char* dialogName)
{
    if (!dialog || !dialogName || !dialogName[0])
        return;

    const bool isBattle = lstrcmpiA(dialogName, "DLG_BATTLE_A") == 0;
    const bool returnsToStrategic = lstrcmpiA(dialogName, "DLG_STRATEGIC") == 0;
    if (returnsToStrategic && g_battleEpochActive) {
        // This is the old hook's exact battle-active true -> false transition. Do not close the
        // epoch on readiness, timeout, or result-control rebinds.
        g_battleEpochActive = false;
        g_battleEpochDialog = nullptr;
        g_battleEpochOwnerInstance = 0;
        g_battleEpochFirstBindTick = 0;
    }

    if (isBattle) {
        if (!g_battleEpochActive) {
            advanceCounter(g_ownerInstanceCounter, 0xD2E77323u, "dialog owner");
            g_battleEpochActive = true;
            g_battleEpochDialog = dialog;
            g_battleEpochOwnerInstance = g_ownerInstanceCounter;
            g_battleEpochFirstBindTick = GetTickCount();
        } else if (dialog != g_battleEpochDialog) {
            // The working implementation allowed one local battle viewer until DLG_STRATEGIC.
            // A second native owner in that interval is an invariant violation, never a new clock.
            spdlog::critical(
                "[testdrv] UI-state saw a second DLG_BATTLE_A owner before DLG_STRATEGIC; "
                "terminating");
            spdlog::default_logger()->flush();
            TerminateProcess(GetCurrentProcess(), 0xD2E7732Eu);
            ExitProcess(0xD2E7732Eu);
        }
        selectDialogInstance(dialog, g_battleEpochOwnerInstance,
                             g_battleEpochFirstBindTick, true);
    } else {
        // Ordinary dialogs use one causal owner per construction batch. The allocator may reuse an
        // address for a later same-named popup; all binds before the next natural frame share it.
        const bool newOwner = !g_bindCycleOpen || dialog != g_curDialog
                              || lstrcmpA(g_lastDialog, dialogName) != 0;
        if (newOwner) {
            advanceCounter(g_ownerInstanceCounter, 0xD2E77323u, "dialog owner");
            selectDialogInstance(dialog, g_ownerInstanceCounter, GetTickCount(), true);
        }
    }
    g_bindCycleOpen = true;
    g_dialogReady = false;
    g_currentTopScreen = nullptr;
    if (lstrcmpA(g_lastDialog, dialogName) != 0) {
        lstrcpynA(g_lastDialog, dialogName, sizeof(g_lastDialog));
        spdlog::info("[testdrv] dialog now: {}", dialogName);
    }
    // Publish the closed gate before the stock helper mutates the functor/control graph.
    rebuildSnapshot();
}

void recordBind(game::CDialogInterf* dialog, const char* dialogName, const char* buttonName)
{
    if (!dialogName || !buttonName)
        return;
    if (dialog != g_curDialog || lstrcmpA(dialogName, g_lastDialog) != 0
        || g_curOwnerInstance == 0) {
        spdlog::critical("[testdrv] UI-state bind sequencing lost for {}::{}; terminating",
                         dialogName, buttonName);
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0xD2E77324u);
        ExitProcess(0xD2E77324u);
    }
    if (!registerDialog(dialogName, dialog, g_curOwnerInstance,
                        g_curOwnerFirstBindTick, g_curOwnerFirstBindTickSet)) {
        spdlog::critical("[testdrv] UI-state dialog registry exhausted; terminating");
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0xD2E77325u);
        ExitProcess(0xD2E77325u);
    }
    // The just-bound dialog is being shown ON the current topmost screen, let the per-frame poll
    // learn that screen<->name association (so a later close that reveals it can be reported).
    lstrcpynA(g_pendingBind, dialogName, sizeof(g_pendingBind));
    spdlog::info("[testdrv] bind {}::{}", dialogName, buttonName);
    rebuildSnapshot(); // construction snapshot; next screen-loop frame publishes ready=true
}

game::CButtonInterf* __stdcall hookAssignFunctor(game::CDialogInterf* dialog, const char* buttonName,
                                                 const char* dialogName, game::SmartPointer* functor,
                                                 int hotkey)
{
    // Close the prior generation synchronously, before stock mutates this dialog. This is the
    // earliest typed game callback after DllMain; helpers still start outside loader lock.
    beginBind(dialog, dialogName);
    ::hooks::testdrv::startRuntimeFromUi(dialog);
    game::CButtonInterf* result = g_origAssignFunctor(dialog, buttonName, dialogName, functor, hotkey);
    // Publish only after the stock helper has installed this button's functor. The dialog remains
    // ready=false until refreshCurrentDialog observes the next screen-loop frame.
    if (result) {
        recordBind(dialog, dialogName, buttonName);
        autonav::onDialogBound(dialogName, buttonName, g_dialogInstance,
                               g_curOwnerInstance, result);
    }
    // Only capture/arm here. Ticking and invocation remain on a later natural
    // frame; invoking from this construction callback would be reentrant.
    return result;
}

struct BindCallPatch
{
    uintptr_t site;
    std::int32_t originalDisplacement;
    std::int32_t hookDisplacement;
};

bool readCallInstruction(uintptr_t site, std::uint8_t (&bytes)[5])
{
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(site), bytes,
                           sizeof(bytes), &bytesRead)
        || bytesRead != sizeof(bytes)) {
        spdlog::error("[testdrv] UI-state: cannot read bind call-site 0x{:08X}",
                      static_cast<unsigned int>(site));
        return false;
    }
    return true;
}

std::int64_t decodedCallTarget(uintptr_t site, std::int32_t displacement)
{
    return static_cast<std::int64_t>(site) + 5 + static_cast<std::int64_t>(displacement);
}

bool prepareBindCallPatches(BindCallPatch (&patches)[kBindCallSiteCount])
{
    const auto hookAddress = reinterpret_cast<uintptr_t>(&hookAssignFunctor);
    for (std::size_t i = 0; i < kBindCallSiteCount; ++i) {
        const uintptr_t site = kBindCallSites[i];
        if (i != 0 && site <= kBindCallSites[i - 1]) {
            spdlog::error("[testdrv] UI-state: bind call-site manifest is not strictly ordered");
            return false;
        }

        std::uint8_t bytes[5] = {};
        if (!readCallInstruction(site, bytes))
            return false;
        if (bytes[0] != 0xE8) {
            spdlog::error(
                "[testdrv] UI-state: bind call-site 0x{:08X} opcode is 0x{:02X}, expected E8; refusing",
                static_cast<unsigned int>(site), static_cast<unsigned int>(bytes[0]));
            return false;
        }

        std::int32_t originalDisplacement = 0;
        std::memcpy(&originalDisplacement, bytes + 1, sizeof(originalDisplacement));
        const std::int64_t originalTarget = decodedCallTarget(site, originalDisplacement);
        if (originalTarget != static_cast<std::int64_t>(kAssignFunctorVA)) {
            spdlog::error(
                "[testdrv] UI-state: bind call-site 0x{:08X} targets 0x{:08X}, expected 0x{:08X}; refusing",
                static_cast<unsigned int>(site), static_cast<unsigned int>(originalTarget),
                static_cast<unsigned int>(kAssignFunctorVA));
            return false;
        }

        const std::int64_t hookDisplacement = static_cast<std::int64_t>(hookAddress)
                                              - static_cast<std::int64_t>(site + 5);
        if (hookDisplacement < std::numeric_limits<std::int32_t>::min()
            || hookDisplacement > std::numeric_limits<std::int32_t>::max()) {
            spdlog::error(
                "[testdrv] UI-state: reporter is outside rel32 range of bind call-site 0x{:08X}",
                static_cast<unsigned int>(site));
            return false;
        }

        patches[i] = {site, originalDisplacement, static_cast<std::int32_t>(hookDisplacement)};
    }
    return true;
}

bool verifyCallDisplacement(uintptr_t site, std::int32_t expectedDisplacement)
{
    std::uint8_t bytes[5] = {};
    if (!readCallInstruction(site, bytes) || bytes[0] != 0xE8)
        return false;
    std::int32_t actualDisplacement = 0;
    std::memcpy(&actualDisplacement, bytes + 1, sizeof(actualDisplacement));
    return actualDisplacement == expectedDisplacement;
}

bool writeCallDisplacement(uintptr_t site, std::int32_t displacement, bool& wrote)
{
    wrote = false;
    void* const operand = reinterpret_cast<void*>(site + 1);
    DWORD oldProtection = 0;
    if (!VirtualProtect(operand, sizeof(displacement), PAGE_EXECUTE_READWRITE, &oldProtection)) {
        spdlog::error("[testdrv] UI-state: VirtualProtect write failed at 0x{:08X}, err={}",
                      static_cast<unsigned int>(site), GetLastError());
        return false;
    }

    std::memcpy(operand, &displacement, sizeof(displacement));
    wrote = true;

    DWORD discardedProtection = 0;
    const bool protectionRestored =
        VirtualProtect(operand, sizeof(displacement), oldProtection, &discardedProtection) != FALSE;
    const DWORD protectionError = protectionRestored ? ERROR_SUCCESS : GetLastError();
    const bool cacheFlushed =
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(site), 5) != FALSE;
    const DWORD flushError = cacheFlushed ? ERROR_SUCCESS : GetLastError();
    const bool verified = verifyCallDisplacement(site, displacement);

    if (!protectionRestored)
        spdlog::error("[testdrv] UI-state: protection restore failed at 0x{:08X}, err={}",
                      static_cast<unsigned int>(site), protectionError);
    if (!cacheFlushed)
        spdlog::error("[testdrv] UI-state: instruction-cache flush failed at 0x{:08X}, err={}",
                      static_cast<unsigned int>(site), flushError);
    if (!verified)
        spdlog::error("[testdrv] UI-state: bind call-site read-back failed at 0x{:08X}",
                      static_cast<unsigned int>(site));
    return protectionRestored && cacheFlushed && verified;
}

bool rollbackBindCallPatches(const BindCallPatch (&patches)[kBindCallSiteCount],
                             std::size_t patchedCount)
{
    bool restoredAll = true;
    while (patchedCount != 0) {
        --patchedCount;
        bool wrote = false;
        if (!writeCallDisplacement(patches[patchedCount].site,
                                   patches[patchedCount].originalDisplacement, wrote))
            restoredAll = false;
    }
    return restoredAll;
}

[[noreturn]] void terminateAfterRollbackFailure()
{
    spdlog::critical(
        "[testdrv] UI-state: bind call-site rollback failed; partial hook set is unsafe; terminating");
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), 0xD2E77326u);
    ExitProcess(0xD2E77326u);
}

bool allBindCallSitesPointToHook()
{
    const auto hookAddress = reinterpret_cast<uintptr_t>(&hookAssignFunctor);
    for (std::size_t i = 0; i < kBindCallSiteCount; ++i) {
        const std::int64_t displacement = static_cast<std::int64_t>(hookAddress)
                                          - static_cast<std::int64_t>(kBindCallSites[i] + 5);
        if (displacement < std::numeric_limits<std::int32_t>::min()
            || displacement > std::numeric_limits<std::int32_t>::max()
            || !verifyCallDisplacement(kBindCallSites[i],
                                       static_cast<std::int32_t>(displacement)))
            return false;
    }
    return true;
}

bool installBindCallPatches()
{
    if (g_bindCallSitesInstalled) {
        if (!allBindCallSitesPointToHook()) {
            spdlog::error(
                "[testdrv] UI-state: owned bind call-site set changed after installation; refusing");
            return false;
        }
        return true;
    }

    BindCallPatch patches[kBindCallSiteCount] = {};
    if (!prepareBindCallPatches(patches))
        return false; // Full zero-write preflight failed.

    std::size_t patchedCount = 0;
    for (; patchedCount < kBindCallSiteCount; ++patchedCount) {
        bool wrote = false;
        if (writeCallDisplacement(patches[patchedCount].site,
                                  patches[patchedCount].hookDisplacement, wrote))
            continue;

        const std::size_t rollbackCount = patchedCount + (wrote ? 1 : 0);
        if (!rollbackBindCallPatches(patches, rollbackCount))
            terminateAfterRollbackFailure();
        spdlog::error(
            "[testdrv] UI-state: bind call-site installation failed at index {}; prior writes rolled back",
            patchedCount);
        return false;
    }

    if (!allBindCallSitesPointToHook()) {
        if (!rollbackBindCallPatches(patches, patchedCount))
            terminateAfterRollbackFailure();
        spdlog::error(
            "[testdrv] UI-state: final bind call-site verification failed; all writes rolled back");
        return false;
    }

    g_bindCallSitesInstalled = true;
    return true;
}

} // namespace

void observeLobbyRooms(game::CDialogInterf* dialog, std::vector<std::string> names)
{
    if (!g_bindCallSitesInstalled)
        return;
    g_lobbyRoomsDialog = dialog;
    g_lobbyRoomNames = std::move(names);
}

bool isExpectedLobbyRoomSelected(game::CDialogInterf* dialog, const char* expected)
{
    if (!dialog || dialog != g_lobbyRoomsDialog || !expected || !*expected)
        return false;
    const auto* list = game::CDialogInterfApi::get().findListBox(dialog, "LBOX_ROOMS");
    if (!list || !list->listBoxData
        || list->listBoxData->elementsTotal != static_cast<int>(g_lobbyRoomNames.size()))
        return false;
    const int selected = list->listBoxData->selectedElement;
    if (selected < 0 || static_cast<std::size_t>(selected) >= g_lobbyRoomNames.size()
        || g_lobbyRoomNames[selected] != expected)
        return false;
    return std::count(g_lobbyRoomNames.begin(), g_lobbyRoomNames.end(), expected) == 1;
}

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
            g_registry[i].ownerInstance = 0;
            g_registry[i].firstBindTick = 0;
            g_registry[i].firstBindTickSet = false;
            g_registry[i].screen = nullptr;
            g_registry[i].pendingScreen = false;
            return nullptr;
        }
        return d;
    }
    return nullptr;
}

bool isReadyDialogInstance(const char* requestedDialog, std::uint32_t expectedAppearance,
                           std::uint32_t expectedOwnerInstance)
{
    if (!requestedDialog || !requestedDialog[0] || expectedAppearance == 0
        || expectedOwnerInstance == 0 || !g_dialogReady || !g_currentTopScreen
        || expectedAppearance != g_dialogInstance)
        return false;
    DlgEntry* target = findEntry(requestedDialog);
    if (!target || !target->ptr || target->ownerInstance != expectedOwnerInstance
        || target->screen != g_currentTopScreen)
        return false;
    if (lstrcmpA(requestedDialog, g_lastDialog) == 0)
        return target->ptr == g_curDialog && target->ownerInstance == g_curOwnerInstance;
    // The only accepted co-present owner is proven by its own token and the same topmost screen.
    return lstrcmpA(requestedDialog, "DLG_STRATEGIC") == 0
           && lstrcmpA(g_lastDialog, "DLG_ISO_PAL") == 0;
}

bool getReadyDialogInstanceAge(const char* requestedDialog,
                               std::uint32_t expectedAppearance,
                               std::uint32_t expectedOwnerInstance,
                               std::uint32_t& elapsedMs)
{
    elapsedMs = 0;
    if (!isReadyDialogInstance(requestedDialog, expectedAppearance,
                               expectedOwnerInstance)
        || !g_curOwnerFirstBindTickSet)
        return false;
    elapsedMs = GetTickCount() - g_curOwnerFirstBindTick;
    return true;
}

bool getReadyCurrentDialogInstanceAge(const char* requestedDialog,
                                      std::uint32_t& appearance,
                                      std::uint32_t& ownerInstance,
                                      std::uint32_t& elapsedMs)
{
    appearance = 0;
    ownerInstance = 0;
    elapsedMs = 0;
    if (!requestedDialog || !requestedDialog[0]
        || lstrcmpA(g_lastDialog, requestedDialog) != 0)
        return false;

    const std::uint32_t currentAppearance = g_dialogInstance;
    const std::uint32_t currentOwner = g_curOwnerInstance;
    if (!isReadyDialogInstance(requestedDialog, currentAppearance, currentOwner)
        || !g_curOwnerFirstBindTickSet)
        return false;

    appearance = currentAppearance;
    ownerInstance = currentOwner;
    elapsedMs = GetTickCount() - g_curOwnerFirstBindTick;
    return true;
}

bool isReadyBattleResultCloseInstance(std::uint32_t expectedAppearance,
                                      std::uint32_t expectedOwnerInstance,
                                      game::CButtonInterf* exactButton,
                                      game::CBFunctorDispatch0* exactFunctor)
{
    if (!g_battleEpochActive || !g_battleEpochDialog || !exactButton
        || !exactFunctor
        || expectedOwnerInstance != g_battleEpochOwnerInstance
        || !isReadyDialogInstance("DLG_BATTLE_A", expectedAppearance,
                                  expectedOwnerInstance))
        return false;

    bool actionable = false;
    __try {
        game::CBFunctorDispatch0* callback =
            exactButton->buttonData
                ? exactButton->buttonData->onClickedFunctor.data
                : nullptr;
        actionable = exactButton->buttonData && exactButton->buttonData->enabled
                     && callback == exactFunctor && exactFunctor->vftable
                     && exactFunctor->vftable->runCallback;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        actionable = false;
    }
    return actionable;
}

bool isReadyStrategicMapInstance(std::uint32_t expectedAppearance,
                                 std::uint32_t expectedOwnerInstance)
{
    if (expectedAppearance == 0 || expectedOwnerInstance == 0 || !g_dialogReady
        || !g_currentTopScreen || expectedAppearance != g_dialogInstance
        || expectedOwnerInstance != g_curOwnerInstance)
        return false;
    return lstrcmpA(g_lastDialog, "DLG_STRATEGIC") == 0
           || lstrcmpA(g_lastDialog, "DLG_ISO_PAL") == 0;
}

bool isStrategicIdle()
{
    return computeStrategicIdle();
}

void refreshCurrentDialog()
{
    // The assignFunctor hook only fires on a button-bind, so a modal that closes to reveal an
    // already-bound dialog underneath would otherwise leave a STALE current-dialog. Poll the engine's
    // real topmost INTERFACE (CInterfManager::getTopmostInterface), note it is the SCREEN that hosts a
    // dialog, NOT the CDialogInterf, and co-present dialogs (e.g. DLG_ISO_PAL + DLG_STRATEGIC) share one
    // screen ptr. So we LEARN screen<->name at bind time (the just-bound dialog lives on the current
    // topmost screen) and LOOK UP on close. Ticked per frame from autonav. SEH-guarded.
    // A screen-loop boundary closes an ordinary-dialog construction batch even if observation below
    // fails. DLG_BATTLE_A deliberately keeps its separate first-bind epoch until DLG_STRATEGIC.
    g_bindCycleOpen = false;
    void* top = nullptr;
    __try {
        game::InterfManagerImplPtr mgr{};
        game::CInterfManagerImplApi::get().get(&mgr);
        if (mgr.data)
            top = mgr.data->CInterfManagerImpl::CInterfManager::vftable->getTopmostInterface(mgr.data);
        game::SmartPointerApi::get().createOrFree(reinterpret_cast<game::SmartPointer*>(&mgr), nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dialogReady = false;
        g_currentTopScreen = nullptr;
        rebuildSnapshot();
        return;
    }
    if (!top) {
        g_dialogReady = false;
        g_currentTopScreen = nullptr;
        rebuildSnapshot();
        return;
    }
    if (g_pendingBind[0]) { // a dialog just opened on this topmost screen -> learn (last writer wins)
        mapScreen(top, g_pendingBind);
        for (int i = 0; i < g_regCount; ++i) {
            if (!g_registry[i].pendingScreen)
                continue;
            g_registry[i].screen = top;
            g_registry[i].pendingScreen = false;
        }
        g_pendingBind[0] = 0; // g_lastDialog was already set to it by recordBind
        // Reaching this hook again proves the bind call stack returned to the screen loop. External
        // dispatch can now enqueue one action, which will run no earlier than the following frame.
        g_currentTopScreen = top;
        g_dialogReady = true;
        rebuildSnapshot();
        return;
    }
    // No fresh bind: if the topmost is a known screen whose dialog differs from the reported one, a
    // modal closed and revealed it -> switch to it.
    const char* name = screenName(top);
    if (!name) {
        g_dialogReady = false;
        g_currentTopScreen = nullptr;
        rebuildSnapshot();
        return;
    }
    if (lstrcmpA(g_lastDialog, name) != 0) {
        lstrcpynA(g_lastDialog, name, sizeof(g_lastDialog));
        DlgEntry* revealed = findEntry(name);
        if (!revealed || !revealed->ptr || revealed->ownerInstance == 0
            || revealed->screen != top) {
            g_dialogReady = false;
            g_currentTopScreen = nullptr;
            rebuildSnapshot();
            return;
        }
        selectDialogInstance(revealed->ptr, revealed->ownerInstance,
                             revealed->firstBindTick, revealed->firstBindTickSet);
        spdlog::info("[testdrv] dialog now: {} (revealed)", g_lastDialog);
    }
    DlgEntry* current = findEntry(g_lastDialog);
    if (!current || !current->ptr || current->ptr != g_curDialog
        || current->ownerInstance != g_curOwnerInstance || current->screen != top) {
        g_dialogReady = false;
        g_currentTopScreen = nullptr;
        rebuildSnapshot();
        return;
    }
    g_currentTopScreen = top;
    g_dialogReady = true;
    rebuildSnapshot();
}

bool preflight()
{
    if (!testenv::supportedGameBuild())
        return false;
    const FnAssignFunctor apiAssignFunctor = game::CButtonInterfApi::get().assignFunctor;
    if (reinterpret_cast<uintptr_t>(apiAssignFunctor) != kAssignFunctorVA) {
        spdlog::error("[testdrv] UI-state: assignFunctor API target is 0x{:08X}, expected "
                      "Russobit 0x{:08X}; refusing",
                      static_cast<unsigned int>(reinterpret_cast<uintptr_t>(apiAssignFunctor)),
                      static_cast<unsigned int>(kAssignFunctorVA));
        return false;
    }

    // Deliberately call the canonical entry from our hook. If C4 owns an entry Detour, that call
    // chains through C4 and its trampoline exactly once; on a pristine game it calls stock directly.
    g_origAssignFunctor = reinterpret_cast<FnAssignFunctor>(kAssignFunctorVA);
    BindCallPatch patches[kBindCallSiteCount] = {};
    if (!prepareBindCallPatches(patches))
        return false;
    spdlog::info("[testdrv] UI-state reporter preflight passed ({} Russobit bind call-sites)",
                 kBindCallSiteCount);
    return true;
}

bool commit()
{
    if (!installBindCallPatches())
        return false;
    spdlog::info("[testdrv] UI-state reporter installed ({} Russobit bind call-sites patched)",
                 kBindCallSiteCount);
    return true;
}

bool install()
{
    return preflight() && commit();
}

} // namespace uistatereporter
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
