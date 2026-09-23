/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Auto-nav executor. See testdrv/autonav.h.
 *
 * A thin in-process agent that invokes button functors / sets listbox selections on the UI
 * thread. It subscribes to the shared sub_5629CA natural-frame dispatcher, not the
 * message pump: the functor must run on the dialog-owning thread. Two modes: SELFNAV runs a built-in
 * script (minimal single-instance tests); RELAY_BRIDGE executes the dispatcher's invoke/select
 * commands (the agent holds no test logic, the dispatcher owns that).
 * Compile-gated by D2_TESTDRV.
 */

#ifdef D2_TESTDRV

#include "testdrv/autonav.h"
#include "testdrv/nettracehooks.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/scriptedpopups.h"
#include "testdrv/testdrv.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/worldactions.h"
#include "testdrv/worldreporter.h"
#include "battleviewerinterf.h"
#include "button.h"
#include "dialoginterf.h"
#include "editboxinterf.h"
#include "listbox.h"
#include "menuphase.h"
#include "midgard.h"
#include "phasegame.h"
#include "scenariodata.h"
#include "scenariodataarray.h"
#include "smartptr.h"
#include "spinbuttoninterf.h"
#include "togglebutton.h"
#include "uiframedispatcher.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace autonav {

namespace {

// Exact native auto-battle callback identity, independent of any gameplay feature.
constexpr std::uintptr_t kAutoBattleToggleHandler = 0x00635509;
constexpr std::uintptr_t kAutoBattleFunctorVftable = 0x006F45D4;
constexpr std::uintptr_t kAutoBattleFunctorDispatch = 0x00644150;

enum class NavAction
{
    WaitDialog,   // wait until <dlg> is the current dialog (skips on timeout)
    Invoke,       // click <widget> in <dlg> (invoke its onClicked functor)
    SetSelection, // set listbox <widget> in <dlg> to index <param>
    Delay,        // wait <param> ms
    AutoDismiss,  // dismiss any known first-turn popup until quiet, up to <param> ms
    Done
};

struct NavStep
{
    NavAction action;
    const char* dlg;
    const char* widget;
    int param;
};

// --- built-in self-nav scripts (minimal single-instance tests only) ----------
const NavStep g_probeScript[] = {
    {NavAction::WaitDialog, "DLG_MAIN_MENU", "", 0},
    {NavAction::Invoke, "DLG_MAIN_MENU", "BTN_MULTI", 0},
    {NavAction::WaitDialog, "DLG_PROTOCOL", "", 0},
    {NavAction::SetSelection, "DLG_PROTOCOL", "TLBOX_PROTOCOL", 2}, // 2 = TCP/IP
    {NavAction::Delay, "", "", 400},
    {NavAction::Invoke, "DLG_PROTOCOL", "BTN_CONTINUE", 0},
    {NavAction::Done, "", "", 0},
};

const NavStep g_exitScript[] = {
    {NavAction::WaitDialog, "DLG_MAIN_MENU", "", 0},
    {NavAction::Delay, "", "", 800},
    {NavAction::Invoke, "DLG_MAIN_MENU", "BTN_QUIT", 0},
    {NavAction::Done, "", "", 0},
};

// First-turn / entry popups, dismissed in whatever order they appear (selfnav helper).
struct DismissCandidate
{
    const char* dlg;
    const char* btn;
};
const DismissCandidate kDismissCandidates[] = {
    {"DLG_SCENARIO_BRIEFING", "BTN_CONTINUE"},
    {"DLG_BEGIN_TURN", "BTN_OK"},
    {"DLG_GETINFO_BOX", "BTN_CLOSE"},
    {"DLG_EVENT_POPUP", "BTN_RIGHTSIDE"},
    {"DLG_MESSAGE_BOX", "BTN_OK"},
    {"DLG_MESSAGE_BOX", "BTN_YES"},
    {"DLG_MESSAGE_BOX", "BTN_NO"},
    {"DLG_MANAGE_STACK", "BTN_CLOSE"},
    {"DLG_ITEM", "BTN_OK"},
};

const NavStep* g_navScript = nullptr; // active self-nav script (null in dispatcher-only mode)
int g_navLen = 0;
int g_navIdx = 0;
DWORD g_stepStart = 0;
bool g_active = false;      // the agent acts (selfnav and/or dispatcher-driven)
bool g_navArmed = false;
bool g_autoDismiss = false; // continuously dismiss known first-turn popups in the tick
bool g_prepared = false;
char g_expectedLobbyRoom[256]{};
bool g_needsUiFrame = false;
bool g_selfnav = false;
bool g_relay = false;
bool g_autoBattlePrearm = false;
bool g_scriptedPopups = false;
bool g_scriptedPopupConfirmations = false;
char g_role[16] = {};
int g_scenarioIdx = 0;
constexpr DWORD kStepTimeoutMs = 15000;

// Keep the two test windows distinguishable without another hook or polling
// thread: the existing UI-frame callback already owns the exact HWND.
DWORD g_nextTitleRefresh{};
void refreshRoleTitle(HWND window)
{
    if (!window)
        return;
    char role[16]{};
    GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
    const char* tag = lstrcmpiA(role, "host") == 0
                          ? "HOST"
                          : ((lstrcmpiA(role, "join") == 0 || lstrcmpiA(role, "joiner") == 0)
                                 ? "CLIENT"
                                 : nullptr);
    if (!tag)
        return;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - g_nextTitleRefresh) < 0)
        return;
    g_nextTitleRefresh = now + 2000;

    char current[256]{};
    if (GetWindowTextA(window, current, sizeof(current) - 1) <= 0
        || strstr(current, "[HOST]") || strstr(current, "[CLIENT]"))
        return;
    char tagged[300]{};
    wsprintfA(tagged, "%s  [%s]", current, tag);
    SetWindowTextA(window, tagged);
}


// AutoDismiss per-step state.
bool g_adSeenAny = false;
DWORD g_adLastPopupMs = 0;
DWORD g_adLastClickMs = 0;
char g_adLastClickedDlg[48] = {};
constexpr DWORD kAdSameCooldownMs = 1200;
constexpr DWORD kAdQuietMs = 3000;

// A command carries a sequence id so the dispatcher's POST can wait for its outcome. kNoSeq marks
// internal callers (self-nav, auto-dismiss) that have no POST waiting on a result.
constexpr std::uint32_t kNoSeq = 0xFFFFFFFFu;
constexpr std::uint16_t kInvokePairedEndTurnOp = 0x030F;
constexpr std::uint16_t kReleasePairedEndTurnOp = 0x0310;
constexpr std::uint32_t kMinimumRemoteCommandTimeoutMs = 1000;
constexpr std::uint32_t kMaximumRemoteCommandTimeoutMs = 120000;
// CommandStarted reaches the relay after the native UI intent is armed. Keep one
// bounded failure-only second for that delivery and terminal socket-fault
// propagation; this never delays or retries the happy-path release.
constexpr DWORD kPairedEndTurnFaultPropagationMs = 1000;

// The relay releases only after both clients have armed an exact EndTurn intent.
// The UI thread keeps running normally while the bridge publishes that release;
// a later natural frame must revalidate the same target and native admission.
std::atomic<std::uint32_t> g_pairedEndTurnArmedSeq{0};
std::atomic<std::uint32_t> g_pairedEndTurnReleasedSeq{0};
ULONGLONG g_pairedEndTurnArmedAt = 0; // UI-thread-only pending intent
game::CButtonInterf* g_pairedEndTurnButton = nullptr;
game::CBFunctorDispatch0* g_pairedEndTurnFunctor = nullptr;

[[noreturn]] void failFastRemoteFault(const char* seam, std::uint32_t seq,
                                      unsigned exitCode)
{
    spdlog::critical("[testdrv] relay-driven UI/world fault at {} seq={}; terminating",
                     seam, seq);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

void rejectCaughtFaultIfStrict(const char* seam, std::uint32_t seq,
                               unsigned exitCode, bool forceStrict = false)
{
    if (forceStrict || g_relay || g_autoBattlePrearm || seq != kNoSeq)
        failFastRemoteFault(seam, seq, exitCode);
}

// Report whether a command resolved its target. Button commands report before the callback because
// a stock callback can block in DirectPlay for ~10 s. Selection commands deliberately defer this
// result until ten later natural-frame read-backs have proved the one write remained intact.
void reportFound(std::uint32_t seq, bool found)
{
    if (seq != kNoSeq)
        bridge::send_command_result(seq, found);
}

bool invokeButton(const char* dlgName, const char* btnName,
                  std::uint32_t seq = kNoSeq, bool forceStrict = false,
                  bool requireStrategicIdle = false)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    game::CBFunctorDispatch0* functor = nullptr;
    bool disabled = false;
    int protocolSelection = -1;
    int protocolTotal = -1;
    // SEH around the resolve: a popup can close mid-tick, leaving a stale dialog ptr.
    __try {
        game::CButtonInterf* btn = dlg ? game::CDialogInterfApi::get().findButton(dlg, btnName) : nullptr;
        if (btn && btn->buttonData) {
            if (!btn->buttonData->enabled) {
                disabled = true;
            } else {
                game::CBFunctorDispatch0* f = btn->buttonData->onClickedFunctor.data;
                if (f && f->vftable && f->vftable->runCallback)
                    functor = f;
            }
        }
        if (dlg && lstrcmpA(dlgName, "DLG_PROTOCOL") == 0
            && lstrcmpA(btnName, "BTN_CONTINUE") == 0) {
            game::CListBoxInterf* protocols =
                game::CDialogInterfApi::get().findListBox(dlg, "TLBOX_PROTOCOL");
            if (protocols && protocols->listBoxData) {
                protocolSelection = protocols->listBoxData->selectedElement;
                protocolTotal = protocols->listBoxData->elementsTotal;
            }
        }
        if (g_expectedLobbyRoom[0] && lstrcmpA(dlgName, "DLG_CUSTOM_LOBBY") == 0
            && lstrcmpA(btnName, "BTN_JOIN") == 0
            && !uistatereporter::isExpectedLobbyRoomSelected(dlg, g_expectedLobbyRoom))
            functor = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("invokeButton resolve", seq, 0xD2E77310u,
                                  forceStrict);
        functor = nullptr;
    }
    if (disabled && (forceStrict || seq != kNoSeq))
        failFastRemoteFault("invokeButton disabled", seq, 0xD2E77326u);
    if (protocolTotal >= 0)
        spdlog::info("[testdrv] nav protocol pre-invoke selected={:d} total={:d}",
                     protocolSelection, protocolTotal);
    if (!functor) {
        reportFound(seq, false);
        return false;
    }
    // For the exact EndTurn target, admission precedes even the positive
    // target-resolution result. False is terminal for this already-issued
    // command; it is never queued or retried.
    if (requireStrategicIdle && !uistatereporter::isStrategicIdle())
        failFastRemoteFault("strategic End Turn lost exact native idle admission",
                            seq, 0xD2E7734Eu);
    reportFound(seq, true);
    bool invoked = false;
    __try {
        spdlog::info("[testdrv] nav invoke {}::{}", dlgName, btnName);
        functor->vftable->runCallback(functor);
        spdlog::info("[testdrv] nav invoke returned {}::{}", dlgName, btnName);
        invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("invokeButton callback", seq, 0xD2E77311u,
                                  forceStrict);
    }
    return invoked;
}

bool invokeExactBoundButton(game::CButtonInterf* exactButton,
                            game::CBFunctorDispatch0* exactFunctor,
                            const char* dlgName, const char* btnName,
                            std::uint32_t seq)
{
    bool actionable = false;
    __try {
        actionable = exactButton && exactButton->buttonData
                     && exactButton->buttonData->enabled
                     && exactButton->buttonData->onClickedFunctor.data
                            == exactFunctor
                     && exactFunctor && exactFunctor->vftable
                     && exactFunctor->vftable->runCallback;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failFastRemoteFault("exact bound button validation", seq,
                            0xD2E7734Cu);
    }
    reportFound(seq, actionable);
    if (!actionable)
        return false;

    bool invoked = false;
    __try {
        spdlog::info("[testdrv] nav invoke exact-bound {}::{}",
                     dlgName, btnName);
        exactFunctor->vftable->runCallback(exactFunctor);
        spdlog::info("[testdrv] nav invoke exact-bound returned {}::{}",
                     dlgName, btnName);
        invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failFastRemoteFault("exact bound button callback", seq,
                            0xD2E7734Du);
    }
    return invoked;
}

// Toggle a CToggleButton (e.g. DLG_BATTLE_A::TOG_AUTOBATTLE) the way a click does: flip `checked` then
// fire its onClicked callback. invokeButton's findButton does not match toggles, hence a separate verb.
bool invokeToggle(const char* dlgName, const char* togName, std::uint32_t seq = kNoSeq)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    // The battle viewer (DLG_BATTLE_A) is not assignFunctor-registered, so findDialog misses it; it IS
    // the topmost interface though, so fall back to the current dialog when the name matches.
    if (!dlg) {
        const char* cur = uistatereporter::currentDialogName();
        if (cur && lstrcmpA(cur, dlgName) == 0)
            dlg = uistatereporter::currentDialog();
    }
    game::CToggleButton* tog = nullptr;
    __try {
        tog = dlg ? game::CDialogInterfApi::get().findToggleButton(dlg, togName) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("invokeToggle resolve", seq, 0xD2E77312u);
        tog = nullptr;
    }
    reportFound(seq, tog != nullptr);
    if (!tog)
        return false;
    bool ok = false;
    __try {
        const bool newChecked = tog->data ? !tog->data->checked : true;
        game::CToggleButtonApi::get().setChecked(tog, newChecked);
        if (tog->vftable && tog->vftable->callOnClicked)
            tog->vftable->callOnClicked(tog);
        spdlog::info("[testdrv] nav toggle {}::{} -> {}", dlgName, togName, newChecked);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("invokeToggle callback", seq, 0xD2E77313u);
    }
    return ok;
}

bool setListSelection(const char* dlgName, const char* lbName, int index,
                      std::uint32_t seq = kNoSeq, bool deferResult = false,
                      int* selectedIndex = nullptr, int* elementsTotal = nullptr)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    game::CListBoxInterf* lb = nullptr;
    __try {
        lb = dlg ? game::CDialogInterfApi::get().findListBox(dlg, lbName) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setListSelection resolve", seq, 0xD2E77314u);
        lb = nullptr;
    }
    if (!deferResult)
        reportFound(seq, lb != nullptr);
    if (!lb)
        return false;
    bool ok = false;
    __try {
        game::CListBoxInterfApi::get().setSelectedIndex(lb, index);
        if (selectedIndex)
            *selectedIndex = lb->listBoxData ? lb->listBoxData->selectedElement : -1;
        if (elementsTotal)
            *elementsTotal = lb->listBoxData ? lb->listBoxData->elementsTotal : -1;
        spdlog::info("[testdrv] nav select {}::{} = {:d} (total={:d})", dlgName, lbName, index,
                     lb->listBoxData ? lb->listBoxData->elementsTotal : -1);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setListSelection setSelectedIndex", seq, 0xD2E77315u);
    }
    return ok;
}

bool enableToggle(const char* dlgName, const char* togName, std::uint32_t seq)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    if (!dlg) {
        const char* current = uistatereporter::currentDialogName();
        if (current && lstrcmpA(current, dlgName) == 0)
            dlg = uistatereporter::currentDialog();
    }
    game::CToggleButton* toggle = nullptr;
    bool canEnable = false;
    __try {
        toggle = dlg ? game::CDialogInterfApi::get().findToggleButton(dlg, togName) : nullptr;
        canEnable = toggle && toggle->data && !toggle->data->checked && toggle->vftable
                    && toggle->vftable->isEnabled && toggle->vftable->isEnabled(toggle)
                    && toggle->vftable->callOnClicked;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("enableToggle resolve", seq, 0xD2E7732Bu);
        toggle = nullptr;
        canEnable = false;
    }
    reportFound(seq, canEnable);
    if (!canEnable)
        return false;
    bool ok = false;
    __try {
        game::CToggleButtonApi::get().setChecked(toggle, true);
        toggle->vftable->callOnClicked(toggle);
        spdlog::info("[testdrv] nav enable toggle {}::{} false -> true", dlgName, togName);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("enableToggle callback", seq, 0xD2E7732Cu);
    }
    return ok;
}

// Concrete layout of Russobit's bound CBFunctorDispatch2 implementation. The
// public base type intentionally exposes only its vftable; the old green lobby
// driver proved these three following fields at F+4/F+8/F+12.
struct BoundToggleFunctorLayout
{
    game::CBFunctorDispatch2<bool, game::CToggleButton*> base;
    game::CBattleViewerInterf* object;
    std::uintptr_t memberFunction;
    std::int32_t thisAdjustor;
};
static_assert(sizeof(BoundToggleFunctorLayout) == 16,
              "exact Russobit bound toggle functor layout changed");
static_assert(offsetof(BoundToggleFunctorLayout, object) == 4
                  && offsetof(BoundToggleFunctorLayout, memberFunction) == 8
                  && offsetof(BoundToggleFunctorLayout, thisAdjustor) == 12,
              "exact Russobit bound toggle functor offsets changed");
static_assert(offsetof(game::CBattleViewerInterf, data) == 0x1C
                  && offsetof(game::CBattleViewerInterf, data2) == 0x20,
              "exact Russobit battle viewer compatibility offsets changed");
constexpr std::uint32_t kAutoBattleMinimumBindAgeMs = 2500;
constexpr std::uint32_t kAutoBattleMaximumBindAgeMs = 20000;

enum class AutoBattleAdmission
{
    Invalid,
    Waiting,
    Ready
};

struct AutoBattleTarget
{
    game::CBFunctorDispatch2<bool, game::CToggleButton*>* functor;
    std::uint8_t* state;
    bridge::AutoBattleKickResult result;
    std::uintptr_t functorVftable;
    std::uintptr_t dispatchFunction;
    std::int32_t thisAdjustor;
    std::uint32_t callbackCount;
};

AutoBattleAdmission inspectAutoBattle(const char* dlgName, const char* togName,
                                      std::uint32_t seq, std::uint32_t bindAgeMs,
                                      AutoBattleTarget& target)
{
    target = {};
    target.result.controllerGateBefore = 0xFF;
    target.result.kickStateBefore = 0xFF;
    target.result.kickStateAfter = 0xFF;
    target.result.sideSelector = 0xFF;
    target.result.flag38Before = 0xFF;
    target.result.flag38After = 0xFF;
    target.result.flag39Before = 0xFF;
    target.result.flag39After = 0xFF;

    // This verb deliberately has one meaning. A generic toggle must never be
    // able to opt into raw Russobit viewer offsets accidentally.
    const bool exactTarget = lstrcmpA(dlgName, "DLG_BATTLE_A") == 0
                             && lstrcmpA(togName, "TOG_AUTOBATTLE") == 0;
    if (!exactTarget)
        return AutoBattleAdmission::Invalid;

    AutoBattleAdmission admission = AutoBattleAdmission::Invalid;
    __try {
        game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
        if (!dlg) {
            const char* current = uistatereporter::currentDialogName();
            if (current && lstrcmpA(current, dlgName) == 0)
                dlg = uistatereporter::currentDialog();
        }
        game::CToggleButton* toggle =
            dlg ? game::CDialogInterfApi::get().findToggleButton(dlg, togName) : nullptr;
        auto* functor = toggle && toggle->data
                            ? toggle->data->onClickedFunctor.data
                            : nullptr;
        const auto dispatch = functor && functor->vftable
                                  ? functor->vftable->runCallback
                                  : nullptr;
        target.functorVftable = functor
            ? reinterpret_cast<std::uintptr_t>(functor->vftable)
            : 0;
        target.dispatchFunction = reinterpret_cast<std::uintptr_t>(dispatch);
        const bool exactFunctorType = functor && dispatch
            && target.functorVftable == kAutoBattleFunctorVftable
            && target.dispatchFunction == kAutoBattleFunctorDispatch;

        // F+4/F+8/F+12 are concrete-layout fields. Prove both the exact
        // vftable and its slot-0 dispatcher before reading any of them.
        BoundToggleFunctorLayout* bound = nullptr;
        game::CDialogInterf* boundDialog = nullptr;
        std::uint8_t* controller = nullptr;
        std::uint8_t* state = nullptr;
        if (exactFunctorType) {
            bound = reinterpret_cast<BoundToggleFunctorLayout*>(functor);
            target.result.memberFunction =
                static_cast<std::uint32_t>(bound->memberFunction);
            target.thisAdjustor = bound->thisAdjustor;
            const bool exactBinding = bound->object && bound->thisAdjustor == 0
                && bound->memberFunction == kAutoBattleToggleHandler;
            if (exactBinding) {
                boundDialog = game::CDragAndDropInterfApi::get().getDialog(bound->object);
                controller = bound->object->data
                    ? reinterpret_cast<std::uint8_t*>(bound->object->data)
                    : nullptr;
                state = bound->object->data2
                    ? reinterpret_cast<std::uint8_t*>(bound->object->data2)
                    : nullptr;
            }
        }
        if (controller)
            target.result.controllerGateBefore = controller[0x14F9];
        if (state) {
            target.result.kickStateBefore = state[0x1D];
            target.result.sideSelector = state[0x3A];
            target.result.flag38Before = state[0x38];
            target.result.flag39Before = state[0x39];
        }

        const bool exactStructure = toggle && toggle->data && exactFunctorType && bound
            && bound->object && bound->thisAdjustor == 0 && boundDialog == dlg
            && bound->memberFunction == kAutoBattleToggleHandler
            && controller && state;
        if (exactStructure) {
            const bool selectedFlagClear =
                (target.result.sideSelector != 0 && target.result.flag38Before == 0)
                || (target.result.sideSelector == 0 && target.result.flag39Before == 0);
            const bool gatesOpen = bindAgeMs >= kAutoBattleMinimumBindAgeMs
                && target.result.controllerGateBefore == 0
                && target.result.kickStateBefore == 0 && selectedFlagClear;
            admission = gatesOpen ? AutoBattleAdmission::Ready
                                  : AutoBattleAdmission::Waiting;
            if (gatesOpen) {
                target.functor = functor;
                target.state = state;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("inspectAutoBattle exact layout", seq, 0xD2E7732Du);
    }
    return admission;
}

bool enableAutoBattle(AutoBattleTarget& target, std::uint32_t seq,
                      std::uint32_t expectedAppearance, std::uint32_t expectedOwner,
                      std::uint32_t bindAgeMs, bool publishBridgeResult)
{
    // Exact old green action: actionIssued is already claimed by the caller;
    // invoke F(true, nullptr) once. No generic click and no UI-bit synthesis.
    __try {
        ++target.callbackCount;
        target.functor->vftable->runCallback(target.functor, true, nullptr);
        target.result.kickStateAfter = target.state[0x1D];
        target.result.flag38After = target.state[0x38];
        target.result.flag39After = target.state[0x39];
        const bool selectedFlagSet =
            (target.result.sideSelector != 0 && target.result.flag38Before == 0
             && target.result.flag38After == 1
             && target.result.flag39After == target.result.flag39Before)
            || (target.result.sideSelector == 0 && target.result.flag39Before == 0
                && target.result.flag39After == 1
                && target.result.flag38After == target.result.flag38Before);
        target.result.succeeded = target.result.kickStateBefore == 0
            && target.result.kickStateAfter == 1 && selectedFlagSet;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("enableAutoBattle exact callback", seq, 0xD2E7732Du);
    }

    const char* outcome = target.result.succeeded ? "committed" : "rejected";
    spdlog::log(target.result.succeeded ? spdlog::level::info : spdlog::level::err,
        "[testdrv][auto-battle] {} appearance={} owner=0x{:08X} seq={} bindAgeMs={} "
        "vft=0x{:08X} op0=0x{:08X} mfp=0x{:08X} adj={} gate14F9={} X1D={}->{} "
        "side3A={} f38={}->{} f39={}->{}",
        outcome, expectedAppearance, expectedOwner, seq, bindAgeMs,
        static_cast<std::uint32_t>(target.functorVftable),
        static_cast<std::uint32_t>(target.dispatchFunction),
        target.result.memberFunction, target.thisAdjustor,
        target.result.controllerGateBefore, target.result.kickStateBefore,
        target.result.kickStateAfter, target.result.sideSelector,
        target.result.flag38Before, target.result.flag38After,
        target.result.flag39Before, target.result.flag39After);
    if (publishBridgeResult)
        bridge::send_auto_battle_kick_result(seq, target.result);
    return target.result.succeeded;
}

enum class PrearmedAutoBattleState
{
    Disabled,
    AwaitingFirstBattle,
    WaitingMinimumBindAge,
    Claimed,
    Committed
};

PrearmedAutoBattleState g_prearmedAutoBattleState =
    PrearmedAutoBattleState::Disabled;
std::uint32_t g_prearmedBattleAppearance = 0;
std::uint32_t g_prearmedBattleOwner = 0;

[[noreturn]] void failFastPrearmedAutoBattle(const char* seam, unsigned exitCode)
{
    spdlog::critical(
        "[testdrv] preboot first-battle auto-battle fault at {}; terminating", seam);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

std::string escapeJsonString(const char* value)
{
    std::string escaped;
    if (!value)
        return escaped;
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(value);
         *p; ++p) {
        const unsigned char c = *p;
        switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                char encoded[7] = {'\\', 'u', '0', '0', kHex[c >> 4],
                                   kHex[c & 0x0f], '\0'};
                escaped += encoded;
            } else {
                escaped += static_cast<char>(c);
            }
            break;
        }
    }
    return escaped;
}

void emitPrearmedAutoBattleProof(const AutoBattleTarget& target,
                                 std::uint32_t appearance,
                                 std::uint32_t owner,
                                 std::uint32_t bindAgeMs)
{
    const std::string role = escapeJsonString(g_role);
    spdlog::info(
        "[testdrv][auto-battle-proof] "
        "{{\"schema\":1,\"mode\":\"preboot-first-battle\",\"role\":\"{}\","
        "\"succeeded\":{},\"appearance\":{},\"owner\":{},\"bindAgeMs\":{},"
        "\"callbackCount\":{},\"functorVftable\":{},\"dispatchFunction\":{},"
        "\"memberFunction\":{},\"thisAdjustor\":{},\"controllerGateBefore\":{},"
        "\"kickStateBefore\":{},\"kickStateAfter\":{},\"sideSelector\":{},"
        "\"flag38Before\":{},\"flag38After\":{},\"flag39Before\":{},"
        "\"flag39After\":{}}}",
        role, target.result.succeeded ? "true" : "false", appearance, owner,
        bindAgeMs, target.callbackCount,
        static_cast<std::uint32_t>(target.functorVftable),
        static_cast<std::uint32_t>(target.dispatchFunction),
        target.result.memberFunction, target.thisAdjustor,
        static_cast<unsigned int>(target.result.controllerGateBefore),
        static_cast<unsigned int>(target.result.kickStateBefore),
        static_cast<unsigned int>(target.result.kickStateAfter),
        static_cast<unsigned int>(target.result.sideSelector),
        static_cast<unsigned int>(target.result.flag38Before),
        static_cast<unsigned int>(target.result.flag38After),
        static_cast<unsigned int>(target.result.flag39Before),
        static_cast<unsigned int>(target.result.flag39After));
}

void tickPrearmedAutoBattle()
{
    if (g_prearmedAutoBattleState == PrearmedAutoBattleState::Disabled
        || g_prearmedAutoBattleState == PrearmedAutoBattleState::Claimed
        || g_prearmedAutoBattleState == PrearmedAutoBattleState::Committed)
        return;

    std::uint32_t appearance = 0;
    std::uint32_t owner = 0;
    std::uint32_t bindAgeMs = 0;
    const bool exactReadyBattle =
        uistatereporter::getReadyCurrentDialogInstanceAge(
            "DLG_BATTLE_A", appearance, owner, bindAgeMs);

    if (g_prearmedAutoBattleState == PrearmedAutoBattleState::AwaitingFirstBattle)
        return; // capture is event-driven by onDialogBound; never infer it from readiness

    if (!exactReadyBattle
        || appearance != g_prearmedBattleAppearance
        || owner != g_prearmedBattleOwner) {
        failFastPrearmedAutoBattle(
            "first DLG_BATTLE_A retired before its one eligible frame",
            0xD2E77336u);
    }

    if (bindAgeMs < kAutoBattleMinimumBindAgeMs)
        return;

    AutoBattleTarget target{};
    const AutoBattleAdmission admission = inspectAutoBattle(
        "DLG_BATTLE_A", "TOG_AUTOBATTLE", kNoSeq, bindAgeMs, target);
    // The old green driver observed the engine's interactive gate after the
    // 2500 ms crash-safety floor and invoked the toggle only after that gate
    // opened. Waiting here is not another action attempt: no callback has been
    // claimed or issued yet. The first Ready frame owns the one callback.
    if (admission == AutoBattleAdmission::Waiting
        && bindAgeMs < kAutoBattleMaximumBindAgeMs)
        return;
    if (admission != AutoBattleAdmission::Ready) {
        spdlog::critical(
            "[testdrv] preboot auto-battle admission never opened "
            "appearance={} owner={} bindAgeMs={} state={}",
            appearance, owner, bindAgeMs, static_cast<int>(admission));
        failFastPrearmedAutoBattle(
            "exact admission was Invalid or timed out Waiting", 0xD2E77337u);
    }

    g_prearmedAutoBattleState = PrearmedAutoBattleState::Claimed;
    const bool committed = enableAutoBattle(
        target, kNoSeq, appearance, owner, bindAgeMs, false);
    if (target.callbackCount != 1)
        failFastPrearmedAutoBattle(
            "exact callback count was not one", 0xD2E77338u);
    emitPrearmedAutoBattleProof(target, appearance, owner, bindAgeMs);
    if (!committed)
        failFastPrearmedAutoBattle(
            "sole callback postcondition failed", 0xD2E77339u);
    g_prearmedAutoBattleState = PrearmedAutoBattleState::Committed;
}

bool setScenarioSelectionByPath(const char* dlgName, const char* lbName,
                                const char* exactPath, std::uint32_t seq,
                                bool deferResult = false, int* selectedIndex = nullptr,
                                int* elementsTotal = nullptr)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    game::CListBoxInterf* lb = nullptr;
    int exactIndex = -1;
    int matches = 0;
    __try {
        lb = dlg ? game::CDialogInterfApi::get().findListBox(dlg, lbName) : nullptr;
        auto* midgard = game::CMidgardApi::get().instance();
        auto* menuPhase = midgard && midgard->data ? midgard->data->menuPhase : nullptr;
        auto* wrapped = menuPhase && menuPhase->data ? menuPhase->data->scenarios : nullptr;
        auto* scenarios = wrapped ? &wrapped->data : nullptr;
        if (lb && lb->listBoxData && scenarios && scenarios->bgn && scenarios->end
            && scenarios->end >= scenarios->bgn) {
            const auto count = scenarios->size();
            if (count <= 4096 && lb->listBoxData->elementsTotal == static_cast<int>(count)) {
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& scenario = scenarios->bgn[i];
                    if (scenario.filePath.string
                        && lstrcmpiA(scenario.filePath.string, exactPath) == 0) {
                        exactIndex = static_cast<int>(i);
                        ++matches;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setScenarioSelectionByPath resolve", seq, 0xD2E77329u);
        lb = nullptr;
        matches = 0;
    }

    const bool found = lb && matches == 1 && exactIndex >= 0;
    if (!deferResult)
        reportFound(seq, found);
    if (!found)
        return false;
    bool ok = false;
    __try {
        game::CListBoxInterfApi::get().setSelectedIndex(lb, exactIndex);
        if (selectedIndex)
            *selectedIndex = lb->listBoxData ? lb->listBoxData->selectedElement : -1;
        if (elementsTotal)
            *elementsTotal = lb->listBoxData ? lb->listBoxData->elementsTotal : -1;
        spdlog::info("[testdrv] nav exact scenario select {}::{} = {:d} path='{}'",
                     dlgName, lbName, exactIndex, exactPath);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setScenarioSelectionByPath setSelectedIndex", seq,
                                  0xD2E7732Au);
    }
    return ok;
}

bool setSpinOption(const char* dlgName, const char* spinName, int option, std::uint32_t seq = kNoSeq)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    game::CSpinButtonInterf* spin = nullptr;
    __try {
        spin = dlg ? game::CDialogInterfApi::get().findSpinButton(dlg, spinName) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setSpinOption resolve", seq, 0xD2E77316u);
        spin = nullptr;
    }
    reportFound(seq, spin != nullptr);
    if (!spin)
        return false;
    bool ok = false;
    __try {
        game::CSpinButtonInterfApi::get().setSelectedOption(spin, option);
        spdlog::info("[testdrv] nav spin {}::{} = {:d}", dlgName, spinName, option);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setSpinOption callback", seq, 0xD2E77317u);
    }
    return ok;
}

bool setEditText(const char* dlgName, const char* editName, const char* text, std::uint32_t seq = kNoSeq)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    game::CEditBoxInterf* eb = nullptr;
    __try {
        eb = dlg ? game::CDialogInterfApi::get().findEditBox(dlg, editName) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setEditText resolve", seq, 0xD2E77318u);
        eb = nullptr;
    }
    reportFound(seq, eb != nullptr);
    if (!eb)
        return false;
    bool ok = false;
    __try {
        game::CEditBoxInterfApi::get().setString(eb, text);
        // Input may contain lobby credentials; never log its value.
        spdlog::info("[testdrv] nav edit {}::{} updated", dlgName, editName);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejectCaughtFaultIfStrict("setEditText callback", seq, 0xD2E77319u);
    }
    return ok;
}

// --- dispatcher-driven remote commands ---------------------------------------
// Commands arrive on the BRIDGE thread (onRemoteCommand only queues); the per-frame tick
// drains them on the UI thread, where invoking a functor is safe.
struct RemoteCmd
{
    int type; // 0..5 UI/exact move; 6 hire; 7 formation; 8 dismiss; 9..12 strict UI; 13 move-toward
    std::uint32_t seq; // echoed in the CommandResult so the relay can match the waiting POST
    // Protocol-v7 mutation identity. Appearance is the causal visible-screen
    // publication; owner is the exact native root/dialog object allowed to
    // receive the action. MoveStack uses a ready strategic-map root.
    std::uint32_t expectedDialogAppearance;
    std::uint32_t expectedDialogOwner;
    std::uint32_t releaseTimeoutMs; // paired EndTurn relay command timeout
    char dlg[48];    // for move (type 4): the stack id string
    char widget[48];
    int param;       // index (listbox / spin)
    char value[MAX_PATH]; // text (edit box) or exact registered scenario path
    int originX, originY; // exact caller-observed source tile (move stack)
    int expectedMovement; // optional causal source MP; -1 keeps legacy attack identity
    int x, y;              // target tile (move stack)
    // Scripted popups use the ordinary type-0 path, but claim the common
    // semantic ledger before queue insertion and require a terminal callback.
    bool internalScripted;
    bool preclaimed;
    game::CButtonInterf* exactButton;
    game::CBFunctorDispatch0* exactFunctor;
};
std::mutex g_remoteMutex; // guards g_remoteCmds, g_inFlight, g_hasInFlight
std::deque<RemoteCmd> g_remoteCmds;
std::deque<RemoteCmd> g_consumedRemoteCmds; // process-lifetime semantic-intent ledger
RemoteCmd g_inFlight{};     // command being executed right now (popped out of the queue)
bool g_hasInFlight = false; // whether g_inFlight is valid
constexpr std::size_t kMaxConsumedRemoteCommands = 4096;

int capturePairedEndTurnTarget(const RemoteCmd& cmd,
                               game::CButtonInterf*& exactButton,
                               game::CBFunctorDispatch0*& exactFunctor)
{
    __try {
        game::CDialogInterf* dlg = uistatereporter::findDialog(cmd.dlg);
        exactButton = dlg
            ? game::CDialogInterfApi::get().findButton(dlg, cmd.widget)
            : nullptr;
        if (!exactButton || !exactButton->buttonData
            || !exactButton->buttonData->enabled)
            return 0;
        exactFunctor = exactButton->buttonData->onClickedFunctor.data;
        return exactFunctor && exactFunctor->vftable
               && exactFunctor->vftable->runCallback ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool invokePairedEndTurnCallback(game::CBFunctorDispatch0* exactFunctor)
{
    __try {
        exactFunctor->vftable->runCallback(exactFunctor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool pairedEndTurnOwnsNativeTurn()
{
    __try {
        auto* phaseGame = testdrv::livePhaseGame();
        return phaseGame && phaseGame->data && phaseGame->data->clientTakesTurn;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// False means the one already-claimed intent is still armed, not a retry.
// Its in-flight slot remains occupied, so subsequent commands cannot overtake it.
bool invokePairedEndTurn(const RemoteCmd& cmd)
{
    game::CButtonInterf* exactButton = nullptr;
    game::CBFunctorDispatch0* exactFunctor = nullptr;
    const int capture = capturePairedEndTurnTarget(cmd, exactButton, exactFunctor);
    if (capture < 0)
        failFastRemoteFault("paired EndTurn exact button capture", cmd.seq,
                            0xD2E7734Fu);
    if (capture == 0)
        failFastRemoteFault("paired EndTurn exact button is not actionable",
                            cmd.seq, 0xD2E77350u);
    const auto armed = g_pairedEndTurnArmedSeq.load(std::memory_order_acquire);
    if (armed == 0) {
        if (!uistatereporter::isStrategicIdle())
            failFastRemoteFault("paired EndTurn lost pre-arm native idle admission",
                                cmd.seq, 0xD2E77351u);
        if (g_pairedEndTurnReleasedSeq.load(std::memory_order_acquire) != 0
            || g_pairedEndTurnButton || g_pairedEndTurnFunctor)
            failFastRemoteFault("paired EndTurn intent was not pristine", cmd.seq,
                                0xD2E77352u);
        g_pairedEndTurnButton = exactButton;
        g_pairedEndTurnFunctor = exactFunctor;
        g_pairedEndTurnArmedAt = GetTickCount64();
        std::uint32_t expected = 0;
        if (!g_pairedEndTurnArmedSeq.compare_exchange_strong(
                expected, cmd.seq, std::memory_order_acq_rel))
            failFastRemoteFault("paired EndTurn already had an armed owner",
                                cmd.seq, 0xD2E77353u);
        spdlog::info("[testdrv] paired EndTurn ARMED seq={} owner={} appearance={}",
                     cmd.seq, cmd.expectedDialogOwner,
                     cmd.expectedDialogAppearance);
        spdlog::default_logger()->flush();
        bridge::send_command_started(cmd.seq);
        return false;
    }

    if (armed != cmd.seq)
        failFastRemoteFault("paired EndTurn armed owner changed", cmd.seq,
                            0xD2E77353u);
    // Resolve through the current live dialog before comparing identities: a
    // retired button is never dereferenced merely because it was valid at arm.
    if (exactButton != g_pairedEndTurnButton || exactFunctor != g_pairedEndTurnFunctor)
        failFastRemoteFault("paired EndTurn leased button changed before release",
                            cmd.seq, 0xD2E77356u);
    if (!pairedEndTurnOwnsNativeTurn())
        failFastRemoteFault("paired EndTurn lost native turn ownership", cmd.seq,
                            0xD2E77355u);
    if (GetTickCount64() - g_pairedEndTurnArmedAt
        >= static_cast<ULONGLONG>(cmd.releaseTimeoutMs) + kPairedEndTurnFaultPropagationMs)
        failFastRemoteFault("paired EndTurn release timed out", cmd.seq,
                            0xD2E77354u);
    const auto released = g_pairedEndTurnReleasedSeq.load(std::memory_order_acquire);
    if (released == 0)
        return false;
    if (released != cmd.seq)
        failFastRemoteFault("paired EndTurn release mismatched", cmd.seq,
                            0xD2E77354u);
    if (!uistatereporter::isStrategicIdle())
        return false; // ordinary RX/commands may finish, within the same arm deadline

    // Normal RX and game frames have run since arm. Admission above is fresh;
    // the callback now uses the ordinary UI/transport path exactly once.
    spdlog::info("[testdrv] paired EndTurn RELEASED seq={}; invoking exact callback",
                 cmd.seq);
    if (!invokePairedEndTurnCallback(exactFunctor))
        failFastRemoteFault("paired EndTurn leased callback", cmd.seq,
                            0xD2E77357u);
    spdlog::info("[testdrv] paired EndTurn callback returned seq={}", cmd.seq);
    // This specialized CommandResult is stronger than generic InvokeButton:
    // true proves that the sole leased EndTurn callback returned successfully.
    reportFound(cmd.seq, true);

    std::uint32_t expected = cmd.seq;
    if (!g_pairedEndTurnArmedSeq.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel)
        || g_pairedEndTurnReleasedSeq.exchange(
               0, std::memory_order_acq_rel) != cmd.seq) {
        failFastRemoteFault("paired EndTurn intent cleanup", cmd.seq,
                            0xD2E77358u);
    }
    g_pairedEndTurnButton = nullptr;
    g_pairedEndTurnFunctor = nullptr;
    g_pairedEndTurnArmedAt = 0;
    return true;
}

// Literal contract carried over from the green lobby driver: selection is
// written once, then only read on ten subsequent natural UI frames. A reset,
// owner change, list-size change, or missing listbox is terminal; no frame is
// allowed to rewrite the value. CommandResult is withheld until read-back #10,
// so the PowerShell driver cannot submit BTN_CONTINUE/BTN_LOAD prematurely.
struct SelectionReadback
{
    bool active = false;
    int expectedIndex = -1;
    int expectedTotal = -1;
    int matches = 0;
};
SelectionReadback g_selectionReadback;
constexpr int kSelectionStableReadbacks = 10;

bool isUiCommandType(int type)
{
    return type == 0 || type == 1 || type == 2 || type == 3 || type == 5 || type == 9
           || type == 10 || type == 11 || type == 12;
}

bool isExactOnceCommandType(int type)
{
    return isUiCommandType(type) || type == 4;
}

bool readListSelection(const RemoteCmd& cmd, int& selectedIndex, int& elementsTotal)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(cmd.dlg);
    game::CListBoxInterf* lb = nullptr;
    bool found = false;
    selectedIndex = -1;
    elementsTotal = -1;
    __try {
        lb = dlg ? game::CDialogInterfApi::get().findListBox(dlg, cmd.widget) : nullptr;
        if (lb && lb->listBoxData) {
            selectedIndex = lb->listBoxData->selectedElement;
            elementsTotal = lb->listBoxData->elementsTotal;
            found = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failFastRemoteFault("selection read-back", cmd.seq, 0xD2E77331u);
    }
    return found;
}

bool sameSemanticIntent(const RemoteCmd& left, const RemoteCmd& right)
{
    // Sequence ids identify HTTP requests and are deliberately excluded: a
    // new seq cannot make an already queued/in-flight/consumed mutation new.
    if ((left.internalScripted || right.internalScripted)
        && isUiCommandType(left.type) && isUiCommandType(right.type))
        return left.expectedDialogAppearance == right.expectedDialogAppearance
               && left.expectedDialogOwner == right.expectedDialogOwner;
    const bool leftToggleMutation = left.type == 5 || left.type == 10 || left.type == 11;
    const bool rightToggleMutation = right.type == 5 || right.type == 10 || right.type == 11;
    const bool leftButtonMutation = left.type == 0 || left.type == 12;
    const bool rightButtonMutation = right.type == 0 || right.type == 12;
    if (left.type != right.type && !(leftToggleMutation && rightToggleMutation)
        && !(leftButtonMutation && rightButtonMutation))
        return false;
    if (left.type == 4) {
        // Legacy actions/attacks carry expectedMovement=-1 and retain the
        // target-only engagement identity: a broken controller cannot re-read
        // the attack's new adjacent origin and refire it. A pinned ordinary
        // movement ledger carries source MP on both commands. Only then is
        // (origin,MP,target) the identity, allowing a later legitimate shuttle
        // traversal after an observed MP spend while still rejecting a replay
        // from the same exact causal state.
        if (left.expectedMovement >= 0 && right.expectedMovement >= 0) {
            return left.expectedDialogAppearance == right.expectedDialogAppearance
                   && left.expectedDialogOwner == right.expectedDialogOwner
                   && left.originX == right.originX && left.originY == right.originY
                   && left.expectedMovement == right.expectedMovement
                   && left.x == right.x && left.y == right.y
                   && lstrcmpA(left.dlg, right.dlg) == 0;
        }
        return left.expectedDialogAppearance == right.expectedDialogAppearance
               && left.expectedDialogOwner == right.expectedDialogOwner
               && left.x == right.x && left.y == right.y
               && lstrcmpA(left.dlg, right.dlg) == 0;
    }
    return left.expectedDialogAppearance == right.expectedDialogAppearance
           && left.expectedDialogOwner == right.expectedDialogOwner
           && left.param == right.param && left.x == right.x && left.y == right.y
           && lstrcmpA(left.dlg, right.dlg) == 0
           && lstrcmpA(left.widget, right.widget) == 0
           && lstrcmpA(left.value, right.value) == 0;
}

[[noreturn]] void failFastDuplicateRemote(const RemoteCmd& current,
                                          const RemoteCmd& duplicate,
                                          const char* state)
{
    spdlog::critical(
        "[testdrv] duplicate remote semantic intent seq={} while identical seq={} is {}; "
        "terminating",
        duplicate.seq, current.seq, state);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), 0xD2E77301u);
    std::abort();
}

void onRemoteCommand(std::uint16_t op, const std::uint8_t* p, std::uint32_t size)
{
    if (op == 0x0312) { // ReleaseStartupActions: empty one-shot relay control.
        scriptedpopups::receiveStartupRelease(size);
        return;
    }
    if (op != 0x0300 && op != 0x0301 && op != 0x0302 && op != 0x0303
        && op != 0x0305 && op != 0x0306 && op != 0x030A && op != 0x030B
        && op != 0x030C && op != kInvokePairedEndTurnOp
        && op != kReleasePairedEndTurnOp
        && op != 0x0307 && op != 0x0308 && op != 0x0309 && op != 0x0311)
        return; // not a command we own
    if (op == kReleasePairedEndTurnOp) {
        if (size != sizeof(std::uint32_t))
            failFastRemoteFault("paired EndTurn release payload", kNoSeq,
                                0xD2E77359u);
        const std::uint32_t seq =
            *reinterpret_cast<const std::uint32_t*>(p);
        std::uint32_t expected = 0;
        if (!seq
            || g_pairedEndTurnArmedSeq.load(std::memory_order_acquire) != seq
            || !g_pairedEndTurnReleasedSeq.compare_exchange_strong(
                expected, seq, std::memory_order_acq_rel)) {
            failFastRemoteFault("paired EndTurn release did not match one armed owner",
                                seq, 0xD2E7735Au);
        }
        return;
    }
    if (g_autoBattlePrearm && op == 0x030C)
        failFastPrearmedAutoBattle(
            "remote 030C conflicts with immutable preboot intent",
            0xD2E7733Au);
    size_t off = 0;
    auto readStr = [&](char* out, size_t outsz) -> bool {
        if (off + 2 > size)
            return false;
        std::uint16_t len = *reinterpret_cast<const std::uint16_t*>(p + off);
        off += 2;
        if (off + len > size)
            return false;
        if (len >= outsz)
            return false;
        memcpy(out, p + off, len);
        out[len] = 0;
        off += len;
        return true;
    };
    RemoteCmd cmd{};
    cmd.expectedMovement = -1;
    if (off + 4 > size) // every command starts with a u32 seq
        return;
    cmd.seq = *reinterpret_cast<const std::uint32_t*>(p + off);
    off += 4;
    const bool causalMutation = op == 0x0300 || op == 0x0301 || op == 0x0302
                                || op == 0x0303 || op == 0x0305 || op == 0x0306
                                || op == 0x030A || op == 0x030B || op == 0x030C
                                || op == kInvokePairedEndTurnOp
                                || op == 0x0307 || op == 0x0308 || op == 0x0309
                                || op == 0x0311;
    if (causalMutation) {
        // Protocol v7: every mutation names both the published appearance and
        // its exact native owner. MoveStack names the ready bare-map root.
        if (off + 8 > size)
            return;
        cmd.expectedDialogAppearance =
            *reinterpret_cast<const std::uint32_t*>(p + off);
        off += 4;
        cmd.expectedDialogOwner = *reinterpret_cast<const std::uint32_t*>(p + off);
        off += 4;
        if (cmd.expectedDialogAppearance == 0 || cmd.expectedDialogOwner == 0)
            return;
    }
    if (op == 0x0300 || op == 0x0306 || op == 0x030B || op == 0x030C
        || op == kInvokePairedEndTurnOp) {
        // InvokeButton / InvokeToggle / EnableToggle / EnableAutoBattle /
        // InvokePairedEndTurn: u16 dlg | u16 widget.
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        if (op == kInvokePairedEndTurnOp) {
            if (off + sizeof(std::uint32_t) > size)
                return;
            cmd.releaseTimeoutMs =
                *reinterpret_cast<const std::uint32_t*>(p + off);
            off += sizeof(std::uint32_t);
            if (cmd.releaseTimeoutMs < kMinimumRemoteCommandTimeoutMs
                || cmd.releaseTimeoutMs > kMaximumRemoteCommandTimeoutMs)
                return;
        }
        cmd.type = (op == 0x0300) ? 0
                   : ((op == 0x0306) ? 5
                      : ((op == 0x030B) ? 10
                         : ((op == 0x030C) ? 11 : 12)));
    } else if (op == 0x0301 || op == 0x0302) {
        // SetSelection (listbox) / SetSpin (spin button): u16 dlg | u16 widget | u32 index
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        if (off + 4 > size)
            return;
        cmd.param = *reinterpret_cast<const int*>(p + off);
        off += 4;
        cmd.type = (op == 0x0301) ? 1 : 2;
    } else if (op == 0x0303) { // SetEditText: u16 dlg | u16 edit | u16 text
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget))
            || !readStr(cmd.value, sizeof(cmd.value)))
            return;
        cmd.type = 3;
    } else if (op == 0x030A) {
        // SelectScenarioPath: u16 dialog | u16 listbox | u16 exact path.
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget))
            || !readStr(cmd.value, sizeof(cmd.value)))
            return;
        cmd.type = 9;
    } else if (op == 0x0307) { // HireMerc: camp | stack | unit
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget))
            || !readStr(cmd.value, sizeof(cmd.value)))
            return;
        cmd.type = 6;
    } else if (op == 0x0308 || op == 0x0311) { // Formation / MoveStackToward
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || off + 8 > size)
            return;
        cmd.x = *reinterpret_cast<const int*>(p + off);
        cmd.y = *reinterpret_cast<const int*>(p + off + 4);
        off += 8;
        cmd.type = op == 0x0308 ? 7 : 13;
    } else if (op == 0x0309) { // DismissUnit: stack | unit
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        cmd.type = 8;
    } else { // 0x0305: u16 stackId | i32 fromX | i32 fromY | i32 fromMP | i32 x | i32 y
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)))
            return;
        if (off + 20 > size)
            return;
        cmd.originX = *reinterpret_cast<const int*>(p + off);
        cmd.originY = *reinterpret_cast<const int*>(p + off + 4);
        cmd.expectedMovement = *reinterpret_cast<const int*>(p + off + 8);
        cmd.x = *reinterpret_cast<const int*>(p + off + 12);
        cmd.y = *reinterpret_cast<const int*>(p + off + 16);
        off += 20;
        if (cmd.expectedMovement < -1 || cmd.expectedMovement > 255)
            return;
        cmd.type = 4;
    }
    if (off != size)
        return;
    const bool exactAutoBattleToggle =
        (cmd.type == 5 || cmd.type == 10)
        && lstrcmpA(cmd.dlg, "DLG_BATTLE_A") == 0
        && lstrcmpA(cmd.widget, "TOG_AUTOBATTLE") == 0;
    if (g_autoBattlePrearm && exactAutoBattleToggle)
        failFastPrearmedAutoBattle(
            "remote 0306/030B conflicts with immutable preboot intent",
            0xD2E7733Au);

    std::lock_guard<std::mutex> lk(g_remoteMutex);
    // Exact-once is owned by the caller. Never disguise a repeated mutation as
    // a successful result, even when it arrives under a fresh sequence id.
    if (g_hasInFlight && sameSemanticIntent(g_inFlight, cmd))
        failFastDuplicateRemote(g_inFlight, cmd, "in flight");
    for (const auto& q : g_remoteCmds)
        if (sameSemanticIntent(q, cmd))
            failFastDuplicateRemote(q, cmd, "queued");
    for (const auto& q : g_consumedRemoteCmds)
        if (sameSemanticIntent(q, cmd))
            failFastDuplicateRemote(q, cmd, "already consumed");
    g_remoteCmds.push_back(cmd);
}

// worldactions::moveStack reads live game objects and issues a net message; like safeRebuildWorld it
// allocates, so it cannot host __try itself (C2712). Guard the call here, in a frame with no unwinding
// locals, then report the outcome. (A move on the strategic map does not block like a DPlay join, so
// reporting after the issue is fine.)
void safeWorldCommand(const RemoteCmd& cmd)
{
    bool ok = false;
    // Only the strict move participates in the existing paired-start oracle.
    // Every world command reports its result after the sole native call returns.
    if (cmd.type == 4)
        bridge::send_command_started(cmd.seq);
    __try {
        switch (cmd.type) {
        case 4:
            ok = worldactions::moveStack(cmd.dlg, cmd.originX, cmd.originY,
                                         cmd.expectedMovement, cmd.x, cmd.y);
            break;
        case 6:
            ok = worldactions::hireMerc(cmd.dlg, cmd.widget, cmd.value);
            break;
        case 7:
            ok = worldactions::moveGroupUnit(cmd.dlg, cmd.x, cmd.y);
            break;
        case 8:
            ok = worldactions::dismissUnit(cmd.dlg, cmd.widget);
            break;
        case 13:
            ok = worldactions::moveStack(cmd.dlg, cmd.x, cmd.y);
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (cmd.type == 4)
            failFastRemoteFault("moveStack", cmd.seq, 0xD2E7731Au);
        ok = false;
    }
    reportFound(cmd.seq, ok);
}

void drainRemoteCommands()
{
    for (;;) {
        RemoteCmd cmd;
        {
            std::lock_guard<std::mutex> lk(g_remoteMutex);
            if (g_hasInFlight) {
                // Admission, selection read-back, and paired-release intents
                // stay in this FIFO slot across their natural UI frames.
                if (g_inFlight.type != 11 && g_inFlight.type != 1
                    && g_inFlight.type != 9 && g_inFlight.type != 12)
                    failFastRemoteFault("non-auto command escaped its dispatch frame",
                                        g_inFlight.seq, 0xD2E7732Fu);
                cmd = g_inFlight;
            } else {
                if (g_remoteCmds.empty())
                    break;
                cmd = g_remoteCmds.front();
                g_remoteCmds.pop_front();
                g_inFlight = cmd;
                g_hasInFlight = true;
                if (isExactOnceCommandType(cmd.type) && cmd.type != 11
                    && !cmd.preclaimed) {
                    if (g_consumedRemoteCmds.size() >= kMaxConsumedRemoteCommands)
                        failFastRemoteFault("consumed remote intent ledger exhausted", cmd.seq,
                                            0xD2E77327u);
                    g_consumedRemoteCmds.push_back(cmd);
                }
            }
        }

        if ((cmd.type == 1 || cmd.type == 9) && g_selectionReadback.active) {
            if (!uistatereporter::isReadyDialogInstance(
                    cmd.dlg, cmd.expectedDialogAppearance, cmd.expectedDialogOwner))
                failFastRemoteFault("selection read-back appearance/owner mismatch",
                                    cmd.seq, 0xD2E77332u);

            int selectedIndex = -1;
            int elementsTotal = -1;
            if (!readListSelection(cmd, selectedIndex, elementsTotal))
                failFastRemoteFault("selection read-back listbox missing",
                                    cmd.seq, 0xD2E77333u);
            if (selectedIndex != g_selectionReadback.expectedIndex
                || elementsTotal != g_selectionReadback.expectedTotal) {
                spdlog::critical(
                    "[testdrv] selection read-back mismatch {}::{} frame={} "
                    "selected={:d}/{:d} total={:d}/{:d}",
                    cmd.dlg, cmd.widget, g_selectionReadback.matches + 1,
                    selectedIndex, g_selectionReadback.expectedIndex,
                    elementsTotal, g_selectionReadback.expectedTotal);
                failFastRemoteFault("selection changed during exact read-backs",
                                    cmd.seq, 0xD2E77334u);
            }

            ++g_selectionReadback.matches;
            if (g_selectionReadback.matches < kSelectionStableReadbacks)
                return;

            spdlog::info(
                "[testdrv] nav select {}::{} = {:d} STABLE after {:d} natural-frame read-backs "
                "(total={:d})",
                cmd.dlg, cmd.widget, g_selectionReadback.expectedIndex,
                g_selectionReadback.matches, g_selectionReadback.expectedTotal);
            reportFound(cmd.seq, true);
            {
                std::lock_guard<std::mutex> lk(g_remoteMutex);
                g_hasInFlight = false;
            }
            g_selectionReadback = SelectionReadback{};
            continue;
        }

        if (cmd.type == 11) {
            std::uint32_t bindAgeMs = 0;
            if (!uistatereporter::getReadyDialogInstanceAge(
                    cmd.dlg, cmd.expectedDialogAppearance, cmd.expectedDialogOwner,
                    bindAgeMs)) {
                failFastRemoteFault("auto-battle appearance/owner/readiness mismatch",
                                    cmd.seq, 0xD2E77328u);
            }

            AutoBattleTarget target{};
            const AutoBattleAdmission admission = inspectAutoBattle(
                cmd.dlg, cmd.widget, cmd.seq, bindAgeMs, target);
            if (admission != AutoBattleAdmission::Ready) {
                if (admission == AutoBattleAdmission::Waiting
                    && bindAgeMs < kAutoBattleMaximumBindAgeMs)
                    return; // one armed intent; observe the next natural frame
                spdlog::critical(
                    "[testdrv][auto-battle] admission failed appearance={} owner=0x{:08X} "
                    "bindAgeMs={} state={}",
                    cmd.expectedDialogAppearance, cmd.expectedDialogOwner,
                    bindAgeMs, static_cast<int>(admission));
                failFastRemoteFault("auto-battle exact admission timeout/mismatch",
                                    cmd.seq, 0xD2E77330u);
            }

            {
                std::lock_guard<std::mutex> lk(g_remoteMutex);
                // Irreversibly claim actionIssued only after the observation
                // gates open, and still before the sole callback below.
                if (g_consumedRemoteCmds.size() >= kMaxConsumedRemoteCommands)
                    failFastRemoteFault("consumed remote intent ledger exhausted", cmd.seq,
                                        0xD2E77327u);
                g_consumedRemoteCmds.push_back(cmd);
            }

            const bool committed = enableAutoBattle(
                target, cmd.seq, cmd.expectedDialogAppearance,
                cmd.expectedDialogOwner, bindAgeMs, true);
            {
                std::lock_guard<std::mutex> lk(g_remoteMutex);
                g_hasInFlight = false;
            }
            if (!committed)
                return; // the sole post-callback proof already reported failure
            continue;
        }

        if ((cmd.type == 4 || cmd.type == 13)
            && !uistatereporter::isReadyStrategicMapInstance(
                cmd.expectedDialogAppearance, cmd.expectedDialogOwner))
            failFastRemoteFault("strategic-map appearance/owner/readiness mismatch", cmd.seq,
                                0xD2E77328u);
        if ((cmd.type == 6 || cmd.type == 7 || cmd.type == 8)
            && !uistatereporter::isReadyDialogInstance(
                uistatereporter::currentDialogName(),
                cmd.expectedDialogAppearance, cmd.expectedDialogOwner))
            failFastRemoteFault("world-command appearance/owner/readiness mismatch", cmd.seq,
                                0xD2E77328u);
        if (isUiCommandType(cmd.type)
            && !uistatereporter::isReadyDialogInstance(
                cmd.dlg, cmd.expectedDialogAppearance, cmd.expectedDialogOwner))
            failFastRemoteFault("dialog appearance/owner/readiness mismatch", cmd.seq,
                                0xD2E77328u);
        if (cmd.type == 1 || cmd.type == 9) {
            int selectedIndex = -1;
            int elementsTotal = -1;
            const bool written = cmd.type == 1
                ? setListSelection(cmd.dlg, cmd.widget, cmd.param, cmd.seq, true,
                                   &selectedIndex, &elementsTotal)
                : setScenarioSelectionByPath(cmd.dlg, cmd.widget, cmd.value, cmd.seq, true,
                                             &selectedIndex, &elementsTotal);
            if (!written) {
                reportFound(cmd.seq, false);
                {
                    std::lock_guard<std::mutex> lk(g_remoteMutex);
                    g_hasInFlight = false;
                }
                continue;
            }
            if (selectedIndex < 0 || elementsTotal <= 0
                || selectedIndex >= elementsTotal
                || (cmd.type == 1 && selectedIndex != cmd.param))
                failFastRemoteFault("selection one-write postcondition",
                                    cmd.seq, 0xD2E77335u);

            g_selectionReadback.active = true;
            g_selectionReadback.expectedIndex = selectedIndex;
            g_selectionReadback.expectedTotal = elementsTotal;
            g_selectionReadback.matches = 0;
            return; // read-back #1 is the next natural UI frame, never this write frame
        }
        if (cmd.type == 12) {
            if (lstrcmpA(cmd.dlg, "DLG_STRATEGIC") != 0
                || lstrcmpA(cmd.widget, "BTN_END_TURN") != 0) {
                failFastRemoteFault("paired EndTurn targeted a non-EndTurn control",
                                    cmd.seq, 0xD2E7735Bu);
            }
            if (!invokePairedEndTurn(cmd))
                return;
        }
        else if (cmd.type == 0) {
            const bool targetsBattleResultClose = g_scriptedPopups
                && lstrcmpA(cmd.dlg, "DLG_BATTLE_A") == 0
                && lstrcmpA(cmd.widget, "BTN_CLOSE") == 0;
            if (targetsBattleResultClose && !cmd.internalScripted)
                failFastRemoteFault(
                    "external battle-result close conflicts with native owner",
                    cmd.seq, 0xD2E7734Au);
            const bool battleResultClose =
                targetsBattleResultClose && cmd.internalScripted;
            const bool strategicEndTurn =
                !cmd.internalScripted
                && lstrcmpA(cmd.dlg, "DLG_STRATEGIC") == 0
                && lstrcmpA(cmd.widget, "BTN_END_TURN") == 0;
            if (battleResultClose)
                scriptedpopups::onBattleResultCloseClaimed(
                    cmd.expectedDialogAppearance, cmd.expectedDialogOwner);
            const bool committed = battleResultClose
                ? invokeExactBoundButton(
                    cmd.exactButton, cmd.exactFunctor, cmd.dlg, cmd.widget,
                    cmd.seq)
                : invokeButton(
                    cmd.dlg, cmd.widget, cmd.seq, cmd.internalScripted,
                    strategicEndTurn);
            if ((cmd.internalScripted || battleResultClose) && !committed)
                failFastRemoteFault(
                    battleResultClose
                        ? "battle-result close callback target missing"
                        : "scripted-popup exact callback target missing",
                    cmd.seq, battleResultClose ? 0xD2E77349u : 0xD2E77345u);
            if (cmd.internalScripted) {
                spdlog::info(
                    "[testdrv][scripted-popup] COMMITTED role={} dialog={} appearance={} "
                    "owner={} button={} tick={}",
                    g_role, cmd.dlg, cmd.expectedDialogAppearance,
                    cmd.expectedDialogOwner, cmd.widget,
                    static_cast<unsigned long long>(GetTickCount64()));
                spdlog::default_logger()->flush();
            }
            if (battleResultClose) {
                spdlog::info(
                    "[battle-close] role={} appearance={} owner={} BTN_CLOSE committed",
                    g_role, cmd.expectedDialogAppearance,
                    cmd.expectedDialogOwner);
                spdlog::default_logger()->flush();
            }
        }
        else if (cmd.type == 2)
            setSpinOption(cmd.dlg, cmd.widget, cmd.param, cmd.seq);
        else if (cmd.type == 3)
            setEditText(cmd.dlg, cmd.widget, cmd.value, cmd.seq);
        else if (cmd.type == 4 || cmd.type == 6 || cmd.type == 7
                 || cmd.type == 8 || cmd.type == 13)
            safeWorldCommand(cmd);
        else if (cmd.type == 5)
            invokeToggle(cmd.dlg, cmd.widget, cmd.seq);
        else if (cmd.type == 10)
            enableToggle(cmd.dlg, cmd.widget, cmd.seq);
        {
            std::lock_guard<std::mutex> lk(g_remoteMutex);
            g_hasInFlight = false;
        }
    }
}

// One AutoDismiss tick. Returns true once popups went quiet after one was dismissed, or
// the cap elapsed.
bool tickAutoDismiss(int capMs)
{
    const DWORD now = GetTickCount();
    for (const auto& c : kDismissCandidates) {
        const bool sameAsLast = (lstrcmpA(g_adLastClickedDlg, c.dlg) == 0);
        if (sameAsLast && (now - g_adLastClickMs) < kAdSameCooldownMs)
            continue;
        if (invokeButton(c.dlg, c.btn)) {
            g_adSeenAny = true;
            g_adLastPopupMs = now;
            g_adLastClickMs = now;
            lstrcpynA(g_adLastClickedDlg, c.dlg, sizeof(g_adLastClickedDlg));
            break;
        }
    }
    if ((now - g_stepStart) >= (DWORD)capMs)
        return true;
    if (g_adSeenAny && (now - g_adLastPopupMs) >= kAdQuietMs)
        return true;
    return false;
}

void resetAutoDismiss()
{
    g_adSeenAny = false;
    g_adLastPopupMs = 0;
    g_adLastClickMs = 0;
    g_adLastClickedDlg[0] = 0;
}

// Dismiss known first-turn popups every tick when D2TESTDRV_AUTODISMISS is set. The two-instance
// MP dispatcher does NOT set it, it paces dismissal itself, since clearing first-turn popups
// back-to-back hangs the begin-turn reconciliation. Kept for single-instance tests.
void dismissPopupsTick()
{
    const DWORD now = GetTickCount();
    for (const auto& c : kDismissCandidates) {
        if (lstrcmpA(g_adLastClickedDlg, c.dlg) == 0 && (now - g_adLastClickMs) < kAdSameCooldownMs)
            continue;
        if (invokeButton(c.dlg, c.btn)) {
            g_adLastClickMs = now;
            lstrcpynA(g_adLastClickedDlg, c.dlg, sizeof(g_adLastClickedDlg));
            break;
        }
    }
}

// One self-nav step (minimal tests only).
void navStep()
{
    if (!g_navScript || g_navIdx >= g_navLen)
        return;

    static int s_lastIdx = -1;
    if (g_navIdx != s_lastIdx) {
        s_lastIdx = g_navIdx;
        resetAutoDismiss();
    }

    const NavStep& s = g_navScript[g_navIdx];
    bool advance = false;
    switch (s.action) {
    case NavAction::Done:
        g_navScript = nullptr;
        spdlog::info("[testdrv] nav script complete");
        return;
    case NavAction::WaitDialog:
        advance = (uistatereporter::findDialog(s.dlg) != nullptr);
        break;
    case NavAction::Invoke:
        advance = invokeButton(s.dlg, s.widget);
        break;
    case NavAction::SetSelection: {
        const int idx = (lstrcmpA(s.dlg, "DLG_CHOOSE_SKIRMISH") == 0) ? g_scenarioIdx : s.param;
        advance = setListSelection(s.dlg, s.widget, idx);
        break;
    }
    case NavAction::Delay:
        advance = (GetTickCount() - g_stepStart) >= (DWORD)s.param;
        break;
    case NavAction::AutoDismiss:
        advance = tickAutoDismiss(s.param);
        break;
    }

    if (advance) {
        ++g_navIdx;
        g_stepStart = GetTickCount();
        return;
    }

    const DWORD timeout = (s.action == NavAction::SetSelection) ? 4000 : kStepTimeoutMs;
    if (s.action != NavAction::Delay && s.action != NavAction::AutoDismiss
        && (GetTickCount() - g_stepStart) >= timeout) {
        spdlog::warn("[testdrv] nav step {:d} ({}::{}) timed out, skipping", g_navIdx, s.dlg, s.widget);
        ++g_navIdx;
        g_stepStart = GetTickCount();
    }
}

// DebugTest is only a subscriber. The common dispatcher is the sole owner of
// exact Russobit sub_5629CA and invokes this once per natural outer UI frame.
void onNaturalUiFrame(HWND window)
{
    refreshRoleTitle(window);
    nettracehooks::onUiFrame();
    tick();
}

} // namespace

void claimAndEnqueueScriptedPopupAction(const char* dialogName,
                                        const char* buttonName,
                                        std::uint32_t appearance,
                                        std::uint32_t ownerInstance,
                                        std::uint32_t bindAgeMs,
                                        game::CButtonInterf* exactButton,
                                        game::CBFunctorDispatch0* exactFunctor)
{
    if (!g_active || !g_scriptedPopups || !dialogName || !dialogName[0]
        || !buttonName || !buttonName[0] || appearance == 0
        || ownerInstance == 0)
        failFastRemoteFault("invalid scripted-popup claim", kNoSeq,
                            0xD2E77344u);
    const bool battleResultClose =
        lstrcmpA(dialogName, "DLG_BATTLE_A") == 0
        && lstrcmpA(buttonName, "BTN_CLOSE") == 0;
    if ((battleResultClose && (!exactButton || !exactFunctor))
        || (!battleResultClose && (exactButton || exactFunctor)))
        failFastRemoteFault("invalid scripted-popup exact target", kNoSeq,
                            0xD2E7734Eu);

    RemoteCmd cmd{};
    cmd.type = 0;
    cmd.seq = kNoSeq;
    cmd.expectedDialogAppearance = appearance;
    cmd.expectedDialogOwner = ownerInstance;
    lstrcpynA(cmd.dlg, dialogName, sizeof(cmd.dlg));
    lstrcpynA(cmd.widget, buttonName, sizeof(cmd.widget));
    cmd.internalScripted = true;
    cmd.preclaimed = true;
    cmd.exactButton = exactButton;
    cmd.exactFunctor = exactFunctor;

    std::lock_guard<std::mutex> lk(g_remoteMutex);
    if (g_hasInFlight && sameSemanticIntent(g_inFlight, cmd))
        failFastDuplicateRemote(g_inFlight, cmd, "in flight");
    for (const auto& queued : g_remoteCmds)
        if (sameSemanticIntent(queued, cmd))
            failFastDuplicateRemote(queued, cmd, "queued");
    for (const auto& consumed : g_consumedRemoteCmds)
        if (sameSemanticIntent(consumed, cmd))
            failFastDuplicateRemote(consumed, cmd, "already consumed");
    if (g_consumedRemoteCmds.size() >= kMaxConsumedRemoteCommands)
        failFastRemoteFault("consumed scripted-popup intent ledger exhausted",
                            cmd.seq, 0xD2E77327u);

    // This ledger insertion is the irreversible claim. Its externally observed
    // marker is flushed before the command can enter the execution queue.
    g_consumedRemoteCmds.push_back(cmd);
    spdlog::info(
        "[testdrv][scripted-popup] CLAIMED role={} dialog={} appearance={} owner={} "
        "button={} bindAgeMs={} tick={}",
        g_role, cmd.dlg, cmd.expectedDialogAppearance, cmd.expectedDialogOwner,
        cmd.widget, bindAgeMs,
        static_cast<unsigned long long>(GetTickCount64()));
    spdlog::default_logger()->flush();
    g_remoteCmds.push_back(cmd);
}

bool preflight(bool selfnav, bool relay, bool autoDismiss,
               bool autoBattlePrearm, bool scriptedPopups,
               bool scriptedPopupConfirmations)
{
    if (g_prepared)
        return g_selfnav == selfnav && g_relay == relay
               && g_autoDismiss == autoDismiss
               && g_autoBattlePrearm == autoBattlePrearm
               && g_scriptedPopups == scriptedPopups
               && g_scriptedPopupConfirmations == scriptedPopupConfirmations;

    if (GetEnvironmentVariableA("D2TESTDRV_EXPECT_ROOM", g_expectedLobbyRoom,
                                sizeof(g_expectedLobbyRoom)) >= sizeof(g_expectedLobbyRoom)) {
        spdlog::error("[testdrv] expected lobby room name exceeds the supported bound");
        return false;
    }
    GetEnvironmentVariableA("D2TESTDRV_ROLE", g_role, sizeof(g_role));
    char sc[8]{};
    if (GetEnvironmentVariableA("D2TESTDRV_SCENARIO_INDEX", sc, sizeof(sc)) > 0)
        g_scenarioIdx = atoi(sc);

    g_selfnav = selfnav;
    g_relay = relay;
    g_autoDismiss = autoDismiss;
    g_autoBattlePrearm = autoBattlePrearm;
    g_scriptedPopups = scriptedPopups;
    g_scriptedPopupConfirmations = scriptedPopupConfirmations;
    g_needsUiFrame = selfnav || relay || g_autoDismiss || g_autoBattlePrearm
                     || g_scriptedPopups;
    if (!scriptedpopups::preflight(g_scriptedPopups,
                                   g_scriptedPopupConfirmations, g_role))
        return false;
    if (g_autoBattlePrearm)
        g_prearmedAutoBattleState =
            PrearmedAutoBattleState::AwaitingFirstBattle;

    if (selfnav) {
        if (lstrcmpiA(g_role, "exit") == 0) {
            g_navScript = g_exitScript;
            g_navLen = (int)(sizeof(g_exitScript) / sizeof(g_exitScript[0]));
        } else { // probe / anything else
            g_navScript = g_probeScript;
            g_navLen = (int)(sizeof(g_probeScript) / sizeof(g_probeScript[0]));
        }
        g_navIdx = 0;
    }
    if (relay) {
        bridge::setCommandCallback(&onRemoteCommand);
    }
    if (g_needsUiFrame) {
        if (!uiframedispatcher::requested()
            || !uiframedispatcher::setDebugFrameCallback(&onNaturalUiFrame)) {
            spdlog::error("[testdrv] shared natural-frame callback registration failed");
            if (relay) {
                bridge::setCommandCallback(nullptr);
            }
            return false;
        }
    }
    g_prepared = true;
    return true;
}

void activateAfterHooks()
{
    if (!g_prepared || (g_needsUiFrame && !uiframedispatcher::installed())) {
        spdlog::critical("[testdrv] prepared natural-frame callback was not committed; terminating");
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0xD2E77327u);
        std::abort();
    }
    g_active = g_needsUiFrame;
    scriptedpopups::activateAfterHooks();
    spdlog::info(
        "[testdrv] nav: role='{}' selfnav={} relay-driven={} auto-dismiss={} "
        "auto-battle-prearm={} scripted-popups={} popup-confirmations={} scenario={}",
        g_role, g_selfnav, g_relay, g_autoDismiss, g_autoBattlePrearm,
        g_scriptedPopups, g_scriptedPopupConfirmations, g_scenarioIdx);
}

void onDialogBound(const char* dialogName, const char* buttonName,
                   std::uint32_t appearance, std::uint32_t ownerInstance,
                   game::CButtonInterf* exactButton)
{
    scriptedpopups::onDialogBound(dialogName, buttonName, appearance,
                                   ownerInstance, exactButton);
    if (g_active && g_autoBattlePrearm
        && g_prearmedAutoBattleState
               == PrearmedAutoBattleState::AwaitingFirstBattle
        && dialogName && lstrcmpA(dialogName, "DLG_BATTLE_A") == 0) {
        if (appearance == 0 || ownerInstance == 0)
            failFastPrearmedAutoBattle(
                "first DLG_BATTLE_A bind had no exact identity", 0xD2E7733Bu);
        g_prearmedBattleAppearance = appearance;
        g_prearmedBattleOwner = ownerInstance;
        g_prearmedAutoBattleState =
            PrearmedAutoBattleState::WaitingMinimumBindAge;
        spdlog::info(
            "[testdrv] preboot auto-battle captured first DLG_BATTLE_A bind "
            "appearance={} owner={}",
            appearance, ownerInstance);
    }

    if (g_navArmed || !g_active)
        return;
    g_navArmed = true;
    g_stepStart = GetTickCount();
    spdlog::info("[testdrv] nav armed");
}

// worldreporter::rebuildSnapshot() reads live game objects through ScenarioView (which allocates), so
// it cannot host __try itself (MSVC C2712). Guard it here, in a frame with no unwinding locals. The
// reporter itself no-ops before a freshly validated map phase; a fault after that gate is terminal.
bool rebuildWorldGuardedSeh()
{
    __try {
        worldreporter::rebuildSnapshot();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[noreturn]] void failFastWorldReporter(const char* faultKind, unsigned exitCode)
{
    spdlog::critical("[testdrv] world reporter {} after live-map admission; terminating",
                     faultKind);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

// A C++ exception is just as terminal as an SEH fault. Neither path disables and retries the reporter
// on a later frame, so one admitted rebuild has one outcome.
void safeRebuildWorld()
{
    try {
        if (!rebuildWorldGuardedSeh())
            failFastWorldReporter("SEH fault", 0xD2E77329u);
    } catch (...) {
        failFastWorldReporter("C++ exception", 0xD2E7732Au);
    }
}

// Ticked by the shared natural-frame dispatcher on the dialog-owning thread.
void tick()
{
    if (!g_navArmed)
        return;
    static bool s_inTick = false;
    if (s_inTick)
        return;
    s_inTick = true;
    uistatereporter::refreshCurrentDialog(); // report the REAL topmost dialog (catch modal closes)
    tickPrearmedAutoBattle();                // preboot first-battle one-shot on its exact frame
    safeRebuildWorld();                      // report players' resources + map stacks (world snapshot)
    scriptedpopups::tick();                  // capture -> ready/300 ms -> one shared-ledger command
    drainRemoteCommands();
    if (g_navScript)
        navStep();
    if (g_autoDismiss)
        dismissPopupsTick();
    s_inTick = false;
}

} // namespace autonav
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
