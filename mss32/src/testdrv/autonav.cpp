/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Auto-nav executor — see testdrv/autonav.h.
 *
 * A thin in-process agent. It invokes button functors / sets listbox selections on the
 * UI/dialog thread, ticked once per screen-loop iteration from a Detour of sub_5629CA
 * (the per-frame cursor helper) — required, since invoking a functor must happen on the
 * thread that owns the dialogs, and with no dependency on the message pump (which the
 * DisciplesGL software-render path on a GPU-less runner does not service). Two modes:
 *   - SELFNAV (D2TESTDRV_SELFNAV): run a built-in script for the role — for MINIMAL,
 *     single-instance tests (probe: menu->Multiplayer->TCP/IP->Continue; exit: quit).
 *   - DISPATCHER-DRIVEN (D2TESTDRV_RELAY_BRIDGE): execute invoke/select commands the
 *     PowerShell dispatcher sends over the relay. The agent itself holds NO test logic,
 *     verification or coordination — that lives in the dispatcher, which scans the live
 *     UI (uistatereporter -> relay) and drives both instances. For COMPLEX / 2-instance
 *     tests.
 * Compile-gated by D2_TESTDRV.
 */

#ifdef D2_TESTDRV

#include "testdrv/autonav.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "button.h"
#include "dialoginterf.h"
#include "listbox.h"
#include "smartptr.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

namespace hooks {
namespace testdrv {
namespace autonav {

namespace {

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
int g_scenarioIdx = 0;
constexpr DWORD kStepTimeoutMs = 15000;

// AutoDismiss per-step state.
bool g_adSeenAny = false;
DWORD g_adLastPopupMs = 0;
DWORD g_adLastClickMs = 0;
char g_adLastClickedDlg[48] = {};
constexpr DWORD kAdSameCooldownMs = 1200;
constexpr DWORD kAdQuietMs = 3000;

bool invokeButton(const char* dlgName, const char* btnName)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    if (!dlg)
        return false; // target dialog not bound (yet) — co-present dialogs resolve by name
    bool invoked = false;
    // SEH around resolve+invoke: a popup can close mid-tick, leaving a stale dialog ptr;
    // a benign C++ throw can unwind through the menu-phase switch.
    __try {
        game::CButtonInterf* btn = game::CDialogInterfApi::get().findButton(dlg, btnName);
        if (btn && btn->buttonData) {
            game::CBFunctorDispatch0* f = btn->buttonData->onClickedFunctor.data;
            if (f && f->vftable && f->vftable->runCallback) {
                spdlog::info("[testdrv] nav invoke {}::{}", dlgName, btnName);
                f->vftable->runCallback(f);
                invoked = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return invoked;
}

bool setListSelection(const char* dlgName, const char* lbName, int index)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    if (!dlg)
        return false;
    bool ok = false;
    __try {
        game::CListBoxInterf* lb = game::CDialogInterfApi::get().findListBox(dlg, lbName);
        if (lb) {
            game::CListBoxInterfApi::get().setSelectedIndex(lb, index);
            spdlog::info("[testdrv] nav select {}::{} = {:d} (total={:d})", dlgName, lbName, index,
                         lb->listBoxData ? lb->listBoxData->elementsTotal : -1);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// --- dispatcher-driven remote commands ---------------------------------------
// The dispatcher (via the relay) sends InvokeButton / SetSelection commands; the bridge
// hands them here on the BRIDGE thread, so we only queue them — the per-frame tick drains
// the queue on the UI/dialog thread, where invoking a functor is safe.
struct RemoteCmd
{
    int type; // 0 = invoke button, 1 = set listbox selection
    char dlg[48];
    char widget[48];
    int param;
};
std::mutex g_remoteMutex;
std::deque<RemoteCmd> g_remoteCmds;

void onRemoteCommand(std::uint16_t op, const std::uint8_t* p, std::uint32_t size)
{
    size_t off = 0;
    auto readStr = [&](char* out, size_t outsz) -> bool {
        if (off + 2 > size)
            return false;
        std::uint16_t len = *reinterpret_cast<const std::uint16_t*>(p + off);
        off += 2;
        if (off + len > size)
            return false;
        size_t n = (len < outsz - 1) ? len : (outsz - 1);
        memcpy(out, p + off, n);
        out[n] = 0;
        off += len;
        return true;
    };
    RemoteCmd cmd{};
    if (op == 0x0300) { // InvokeButton: u16 dlgLen|dlg | u16 btnLen|btn
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        cmd.type = 0;
    } else if (op == 0x0301) { // SetSelection: u16 dlgLen|dlg | u16 lbLen|lb | u32 index
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        if (off + 4 > size)
            return;
        cmd.param = *reinterpret_cast<const int*>(p + off);
        cmd.type = 1;
    } else {
        return; // not a command we own
    }
    std::lock_guard<std::mutex> lk(g_remoteMutex);
    // Coalesce: if an identical command is already pending, drop this one. The UI thread can
    // be briefly blocked (e.g. a synchronous DPlay EnumSessions stalls it ~10s), so the
    // dispatcher may re-fire a click while the first is still queued — executing both when
    // the thread resumes would double-act (two JoinSession -> error). One pending is enough.
    for (const auto& q : g_remoteCmds) {
        if (q.type == cmd.type && lstrcmpA(q.dlg, cmd.dlg) == 0 && lstrcmpA(q.widget, cmd.widget) == 0)
            return;
    }
    g_remoteCmds.push_back(cmd);
}

void drainRemoteCommands()
{
    for (;;) {
        RemoteCmd cmd;
        {
            std::lock_guard<std::mutex> lk(g_remoteMutex);
            if (g_remoteCmds.empty())
                break;
            cmd = g_remoteCmds.front();
            g_remoteCmds.pop_front();
        }
        if (cmd.type == 0)
            invokeButton(cmd.dlg, cmd.widget);
        else
            setListSelection(cmd.dlg, cmd.widget, cmd.param);
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

// Continuously dismiss known first-turn popups, in the tick (on the UI thread). The MP
// dispatcher enables this (D2TESTDRV_AUTODISMISS) so popups are cleared the instant they
// appear — driving the dismiss over the relay round-trip lands the click ~600ms late, and
// a begin-turn command stalled that long on a modal popup resumes into a moved-on game
// state and hangs the display reconciliation. Mechanical (a fixed popup list), so it stays
// in the agent; the dispatcher still owns coordination + verification.
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
        spdlog::warn("[testdrv] nav step {:d} ({}::{}) timed out — skipping", g_navIdx, s.dlg, s.widget);
        ++g_navIdx;
        g_stepStart = GetTickCount();
    }
}

// --- per-frame hook -----------------------------------------------------------
// sub_5629CA is the per-iteration helper the screen message loop (sub_56288A) calls on
// the UI/dialog thread. Detour it so the agent ticks once per iteration on the correct
// thread, with no dependency on the message pump being serviced.
using Sub5629CA_t = LONG(__stdcall*)(HWND, LPPOINT, LONG*);
Sub5629CA_t g_orig5629CA = reinterpret_cast<Sub5629CA_t>(0x5629CA);

LONG __stdcall hook5629CA(HWND hWnd, LPPOINT lpPoint, LONG* a3)
{
    tick();
    return g_orig5629CA(hWnd, lpPoint, a3);
}

bool g_frameHookInstalled = false;
void installFrameHook()
{
    if (g_frameHookInstalled)
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(g_orig5629CA), hook5629CA);
    if (DetourTransactionCommit() == NO_ERROR) {
        g_frameHookInstalled = true;
        spdlog::info("[testdrv] nav frame-hook installed (sub_5629CA per-frame tick)");
    } else {
        spdlog::error("[testdrv] nav frame-hook: DetourTransactionCommit failed");
    }
}

} // namespace

void onUiReady()
{
    char role[16]{};
    GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
    char sc[8]{};
    if (GetEnvironmentVariableA("D2TESTDRV_SCENARIO_INDEX", sc, sizeof(sc)) > 0)
        g_scenarioIdx = atoi(sc);

    const bool selfnav = testenv::on("D2TESTDRV_SELFNAV");
    const bool relay = testenv::on("D2TESTDRV_RELAY_BRIDGE");
    g_autoDismiss = testenv::on("D2TESTDRV_AUTODISMISS");

    if (selfnav) {
        // Built-in self-drive script for minimal single-instance tests.
        if (lstrcmpiA(role, "exit") == 0) {
            g_navScript = g_exitScript;
            g_navLen = (int)(sizeof(g_exitScript) / sizeof(g_exitScript[0]));
        } else { // probe / anything else
            g_navScript = g_probeScript;
            g_navLen = (int)(sizeof(g_probeScript) / sizeof(g_probeScript[0]));
        }
        g_navIdx = 0;
    }
    if (relay) {
        // Dispatcher-driven: execute the invoke/select commands it sends over the relay.
        bridge::setCommandCallback(&onRemoteCommand);
    }
    if (selfnav || relay) {
        g_active = true;
        installFrameHook();
    }
    spdlog::info("[testdrv] nav: role='{}' selfnav={} relay-driven={} scenario={}", role, selfnav,
                 relay, g_scenarioIdx);
}

void onDialogBound()
{
    if (g_navArmed || !g_active)
        return;
    g_navArmed = true;
    g_stepStart = GetTickCount();
    spdlog::info("[testdrv] nav armed");
}

// Ticked once per screen-loop iteration from the sub_5629CA frame hook — i.e. on the
// thread that owns the dialogs, so invoking a button functor is safe and needs no message
// pump. Reentrancy-guarded. Drains dispatcher commands, then advances any self-nav script.
void tick()
{
    if (!g_navArmed)
        return;
    static bool s_inTick = false;
    if (s_inTick)
        return;
    s_inTick = true;
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
