/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Auto-nav driver — see testdrv/autonav.h.
 *
 * Drives the menu chain hands-free on the UI thread by invoking buttons' functors
 * directly (no synthetic input). Ticked by a posted message via a game-window
 * subclass (so it works headless, where WM_TIMER is starved), with a SetTimer
 * fallback for an interactive desktop. Roles (D2TESTDRV_ROLE):
 *   probe — main menu -> Multiplayer -> TCP/IP -> Continue (stops at host/join screen)
 *   exit  — reach the menu, then quit (boot-reliability harness)
 *   host  — create a TCP/IP session, wait for the joiner, start the game
 *   join  — join the host's TCP/IP session, follow it into the started game
 * The host/join scripts sequence via DLL-side RX gates (the joiner connecting, the
 * host entering the game) detected from the network stream. Compile-gated by
 * D2_TESTDRV.
 */

#ifdef D2_TESTDRV

#include "testdrv/autonav.h"
#include "testdrv/nettracehooks.h"
#include "testdrv/uistatereporter.h"
#include "button.h"
#include "dialoginterf.h"
#include "listbox.h"
#include "smartptr.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace autonav {

namespace {

enum class NavAction
{
    WaitDialog,        // wait until <dlg> is the current dialog
    Invoke,            // click <widget> in <dlg> (invoke its onClicked functor)
    SetSelection,      // set listbox <widget> in <dlg> to index <param>
    Delay,             // wait <param> ms
    WaitPeer,          // wait until the joiner has connected (host script)
    WaitHostBriefing,  // wait until the host has entered the game (join script)
    WaitHostStrategic, // wait until the host reached the strategic map (join script)
    AutoDismiss,       // dismiss any known first-turn popup until quiet, up to <param> ms
    Done
};

struct NavStep
{
    NavAction action;
    const char* dlg;
    const char* widget;
    int param;
};

// --- RX gates (set by the network observer; ported from lobby hooks.cpp) ------
std::atomic<bool> g_peerObserved{false};      // a second player's CConnectMsg seen (host)
std::atomic<bool> g_hostInBriefing{false};    // host CJoinGameMsg seen (joiner)
std::atomic<bool> g_hostInStrategic{false};   // host CCmdBeginTurnMsg seen (joiner)
std::atomic<std::uint32_t> g_firstConnectDpid{0};

// Inbound-packet observer. `payload` points at the CNetMsg class name (raw RTTI
// mangled name); `size` is the full message length. Same offsets as the lobby.
void onRxGate(void* /*self*/, int /*sender*/, const std::uint8_t* payload, std::uint32_t size)
{
    if (!payload)
        return;
    // Host: a CConnectMsg whose embedded DPID (offset +36) differs from our own
    // (first one seen) means a real peer joined the DPlay session. Need >=48 bytes:
    // the DPID is a u32 at payload+36 (payload = message+8, so message offset 44..47).
    if (size >= 48 && memcmp(payload, ".?AVCConnectMsg@@", 17) == 0) {
        std::uint32_t dpid = *reinterpret_cast<const std::uint32_t*>(payload + 36);
        if (dpid > 1) {
            // Atomically claim "self DPID" so two RX threads can't both cache it.
            std::uint32_t expected = 0;
            if (g_firstConnectDpid.compare_exchange_strong(expected, dpid)) {
                spdlog::info("[testdrv] gate: cached self DPID={} (first CConnectMsg)", dpid);
            } else if (dpid != expected && !g_peerObserved.exchange(true)) {
                spdlog::info("[testdrv] gate: peer DPID={} joined — WaitPeer released", dpid);
            }
        }
    }
    // Joiner: host's own "game started" broadcast.
    if (size >= 18 && memcmp(payload, ".?AVCJoinGameMsg@@", 18) == 0) {
        if (!g_hostInBriefing.exchange(true))
            spdlog::info("[testdrv] gate: CJoinGameMsg — host in game, WaitHostBriefing released");
    }
    // Joiner: host reached the strategic map.
    if (size >= 22 && memcmp(payload, ".?AVCCmdBeginTurnMsg@@", 22) == 0) {
        if (!g_hostInStrategic.exchange(true))
            spdlog::info("[testdrv] gate: CCmdBeginTurnMsg — host strategic, WaitHostStrategic released");
    }
}

// --- nav scripts ------------------------------------------------------------
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

// Host: create a TCP/IP session, wait for the joiner, start the game. End-Turn is
// intentionally NOT pressed — we only need to reach the started game.
const NavStep g_hostScript[] = {
    {NavAction::WaitDialog, "DLG_MAIN_MENU", "", 0},
    {NavAction::Invoke, "DLG_MAIN_MENU", "BTN_MULTI", 0},
    {NavAction::WaitDialog, "DLG_PROTOCOL", "", 0},
    {NavAction::SetSelection, "DLG_PROTOCOL", "TLBOX_PROTOCOL", 2},
    {NavAction::Delay, "", "", 400},
    {NavAction::Invoke, "DLG_PROTOCOL", "BTN_CONTINUE", 0},
    {NavAction::WaitDialog, "DLG_LOAD_NEW_MULTI", "", 0},
    {NavAction::Invoke, "DLG_LOAD_NEW_MULTI", "BTN_HOST", 0},
    {NavAction::WaitDialog, "DLG_CHOOSE_SKIRMISH", "", 0},
    {NavAction::SetSelection, "DLG_CHOOSE_SKIRMISH", "TLBOX_GAME_SLOT", 0}, // scenario (env override)
    {NavAction::Invoke, "DLG_CHOOSE_SKIRMISH", "BTN_LOAD", 0},
    {NavAction::WaitPeer, "", "", 0},
    {NavAction::Delay, "settle-post-peer", "", 500},
    {NavAction::Invoke, "DLG_LOBBY", "BTN_OK", 0},
    {NavAction::Delay, "async-load", "", 500},
    {NavAction::AutoDismiss, "first-turn-popups", "", 25000},
    {NavAction::Delay, "strategic-settle", "", 2000},
    {NavAction::Done, "", "", 0},
};

// Joiner: join the host's session, follow it into the started game.
const NavStep g_joinScript[] = {
    {NavAction::WaitDialog, "DLG_MAIN_MENU", "", 0},
    {NavAction::Invoke, "DLG_MAIN_MENU", "BTN_MULTI", 0},
    {NavAction::WaitDialog, "DLG_PROTOCOL", "", 0},
    {NavAction::SetSelection, "DLG_PROTOCOL", "TLBOX_PROTOCOL", 2},
    {NavAction::Delay, "", "", 400},
    {NavAction::Invoke, "DLG_PROTOCOL", "BTN_CONTINUE", 0},
    {NavAction::WaitDialog, "DLG_LOAD_NEW_MULTI", "", 0},
    {NavAction::Invoke, "DLG_LOAD_NEW_MULTI", "BTN_JOIN", 0},
    {NavAction::Delay, "session-enum-settle", "", 2000},
    {NavAction::Invoke, "DLG_SESSION", "BTN_JOIN_GAME", 0},
    {NavAction::WaitHostBriefing, "", "", 0},
    {NavAction::WaitHostStrategic, "", "", 0},
    {NavAction::Delay, "settle/T-2s", "", 1000},
    {NavAction::Delay, "settle/T-1s", "", 1000},
    {NavAction::Invoke, "DLG_LOBBY", "BTN_OK", 0},
    {NavAction::Delay, "async-load", "", 500},
    {NavAction::AutoDismiss, "join-entry-popups", "", 20000},
    {NavAction::Delay, "host-turn-broadcast", "", 5000},
    {NavAction::AutoDismiss, "join-active-popups", "", 20000},
    {NavAction::Done, "", "", 0},
};

// First-turn / entry popups, dismissed in whatever order they appear.
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

const NavStep* g_navScript = nullptr;
int g_navLen = 0;
int g_navIdx = 0;
DWORD g_stepStart = 0;
bool g_navArmed = false;
UINT_PTR g_navTimer = 0;
int g_scenarioIdx = 0;
constexpr DWORD kStepTimeoutMs = 15000; // menu transitions
constexpr DWORD kGateTimeoutMs = 60000; // RX gates (peer/host) — generous for a real pairing

// Nav-tick delivery. A SetTimer(NULL,...) callback rides WM_TIMER, a synthesized
// post-when-idle message; on a headless/session-0 runner (CI) it is starved and
// never fires. A *posted* custom message fares no better — the game's pump does
// not retrieve it (filtered PeekMessage / custom DX loop). So we subclass the main
// window and have the driver deliver this tick *out-of-band* via SendMessage
// (delivered straight to the WndProc, bypassing the queue), and also tick on the
// game's own dispatched messages. WM_USER+0x4E is distinct from nettrace's
// deferred-packet message (WM_USER+0x46).
constexpr UINT WM_D2_NAV_TICK = WM_USER + 0x4E;
std::atomic<HWND> g_navHwnd{nullptr};
std::atomic<WNDPROC> g_navOrigWndProc{nullptr};
std::atomic<bool> g_navDriverStop{false};

// AutoDismiss per-step state.
bool g_adSeenAny = false;
DWORD g_adLastPopupMs = 0;
DWORD g_adLastClickMs = 0;
char g_adLastClickedDlg[48] = {};
constexpr DWORD kAdSameCooldownMs = 1200; // don't re-click the same dialog faster than this
constexpr DWORD kAdQuietMs = 3000;        // done once popups stop for this long (after seeing any)

bool invokeButton(const char* dlgName, const char* btnName)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    if (!dlg)
        return false; // target dialog not bound (yet) — co-present dialogs resolve by name
    bool invoked = false;
    // SEH around resolve+invoke: a popup can close mid-tick, leaving a stale
    // dialog ptr; a benign C++ throw can unwind through the menu-phase switch.
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

// One AutoDismiss tick. Returns true when the step is complete (popups went quiet
// after at least one was dismissed, or the cap elapsed).
bool tickAutoDismiss(int capMs)
{
    const DWORD now = GetTickCount();
    for (const auto& c : kDismissCandidates) {
        const bool sameAsLast = (lstrcmpA(g_adLastClickedDlg, c.dlg) == 0);
        if (sameAsLast && (now - g_adLastClickMs) < kAdSameCooldownMs)
            continue; // don't hammer the same popup faster than the cooldown
        if (invokeButton(c.dlg, c.btn)) { // resolves the dialog by name via the registry
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

// Tear down the nav driver: kill the fallback timer, restore the window's WndProc
// (subclass path), and stop the background pacer. Runs on the UI thread.
void navTeardown()
{
    if (g_navTimer) {
        KillTimer(nullptr, g_navTimer);
        g_navTimer = 0;
    }
    HWND h = g_navHwnd.exchange(nullptr);
    WNDPROC orig = g_navOrigWndProc.exchange(nullptr);
    if (h && orig)
        SetWindowLongPtrA(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
    g_navDriverStop.store(true);
}

void navStep(); // one nav tick — runs on the UI thread

// Window subclass: drive one nav step. The game's pump does not retrieve our
// queued custom message (filtered PeekMessage / custom DX loop), so the driver
// delivers it out-of-band via SendMessage; we also tick on whatever messages the
// game itself dispatches (throttled), so the nav advances regardless of which
// delivery the pump honours. Reentrancy-guarded (a button functor can pump).
LRESULT CALLBACK navWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    static std::atomic<bool> s_firstLogged{false};
    if (!s_firstLogged.exchange(true))
        spdlog::info("[testdrv] navWndProc receiving messages (first msg=0x{:04X})", msg);

    static DWORD s_lastTick = 0;
    static bool s_inStep = false;
    const DWORD now = GetTickCount();
    if (!s_inStep && (msg == WM_D2_NAV_TICK || (now - s_lastTick) >= 100)) {
        s_inStep = true;
        s_lastTick = now;
        navStep();
        s_inStep = false;
        if (msg == WM_D2_NAV_TICK)
            return 0; // consume our own tick
    }
    WNDPROC orig = g_navOrigWndProc.load(std::memory_order_acquire);
    return orig ? CallWindowProcA(orig, h, msg, w, l) : DefWindowProcA(h, msg, w, l);
}

// Background pacer: deliver a tick ~every 120ms out-of-band (SendMessage bypasses
// the queue filter that drops a posted message) until the script finishes or the
// window goes away. SMTO_ABORTIFHUNG + a short timeout so a busy UI never hangs us.
DWORD WINAPI navDriverThread(LPVOID)
{
    while (!g_navDriverStop.load()) {
        HWND h = g_navHwnd.load();
        if (!h || !IsWindow(h))
            break;
        DWORD_PTR res = 0;
        SendMessageTimeoutA(h, WM_D2_NAV_TICK, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 500, &res);
        Sleep(120);
    }
    return 0;
}

// EnumWindows picker: the process's main visible, titled window (the one whose
// thread runs the message pump) — same heuristic nettracehooks uses.
struct FindMainWndCtx
{
    DWORD pid;
    HWND found;
};
BOOL CALLBACK pick_main_window(HWND h, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindMainWndCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid == ctx->pid) {
        char title[64]{};
        GetWindowTextA(h, title, sizeof(title));
        if (title[0] && IsWindowVisible(h)) {
            ctx->found = h;
            return FALSE;
        }
    }
    return TRUE;
}

void navStep()
{
    if (!g_navScript || g_navIdx >= g_navLen)
        return;

    static int s_lastIdx = -1;
    if (g_navIdx != s_lastIdx) {
        s_lastIdx = g_navIdx;
        resetAutoDismiss(); // fresh state on entering any step (AutoDismiss needs it)
    }

    const NavStep& s = g_navScript[g_navIdx];
    bool advance = false;
    switch (s.action) {
    case NavAction::Done:
        navTeardown();
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
    case NavAction::WaitPeer:
        advance = g_peerObserved.load();
        break;
    case NavAction::WaitHostBriefing:
        advance = g_hostInBriefing.load();
        break;
    case NavAction::WaitHostStrategic:
        advance = g_hostInStrategic.load();
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

    // Timeouts: gates wait long; menu steps short; Delay/AutoDismiss self-complete.
    const bool isGate = (s.action == NavAction::WaitPeer || s.action == NavAction::WaitHostBriefing
                         || s.action == NavAction::WaitHostStrategic);
    const DWORD timeout = isGate                              ? kGateTimeoutMs
                          : (s.action == NavAction::SetSelection) ? 4000 // best-effort; don't stall
                                                                  : kStepTimeoutMs;
    if (s.action != NavAction::Delay && s.action != NavAction::AutoDismiss
        && (GetTickCount() - g_stepStart) >= timeout) {
        spdlog::warn("[testdrv] nav step {:d} ({} {}::{}) timed out — skipping", g_navIdx,
                     isGate ? "gate" : "ui", s.dlg, s.widget);
        ++g_navIdx;
        g_stepStart = GetTickCount();
    }
}

// Fallback path (interactive desktop, where WM_TIMER is delivered): tick the same
// step function from a thread timer.
void CALLBACK navTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    navStep();
}

} // namespace

void onUiReady()
{
    char role[16]{};
    GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
    if (!role[0])
        return;

    char sc[8]{};
    if (GetEnvironmentVariableA("D2TESTDRV_SCENARIO_INDEX", sc, sizeof(sc)) > 0)
        g_scenarioIdx = atoi(sc);

    bool wantsGates = false;
    if (lstrcmpiA(role, "exit") == 0) {
        g_navScript = g_exitScript;
        g_navLen = (int)(sizeof(g_exitScript) / sizeof(g_exitScript[0]));
    } else if (lstrcmpiA(role, "host") == 0) {
        g_navScript = g_hostScript;
        g_navLen = (int)(sizeof(g_hostScript) / sizeof(g_hostScript[0]));
        wantsGates = true;
    } else if (lstrcmpiA(role, "join") == 0 || lstrcmpiA(role, "joiner") == 0) {
        g_navScript = g_joinScript;
        g_navLen = (int)(sizeof(g_joinScript) / sizeof(g_joinScript[0]));
        wantsGates = true;
    } else {
        g_navScript = g_probeScript;
        g_navLen = (int)(sizeof(g_probeScript) / sizeof(g_probeScript[0]));
    }
    g_navIdx = 0;

    if (wantsGates) {
        // Sequencing needs the RX gates — ensure the network hooks exist and watch
        // the inbound stream for the connect/begin-turn markers.
        nettracehooks::install();
        nettracehooks::addRxObserver(&onRxGate);
    }
    spdlog::info("[testdrv] nav role='{}' -> {:d} steps (gates={}, scenario={})", role, g_navLen,
                 wantsGates, g_scenarioIdx);
}

void onDialogBound()
{
    if (g_navArmed || !g_navScript)
        return;
    g_navArmed = true;
    g_stepStart = GetTickCount();

    // Subclass the game's main window and pace ticks out-of-band from a background
    // thread. This survives a headless runner where WM_TIMER never fires and the
    // pump drops posted messages.
    FindMainWndCtx ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(&pick_main_window, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) {
        WNDPROC prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(ctx.found, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&navWndProc)));
        g_navOrigWndProc.store(prev, std::memory_order_release);
        g_navHwnd.store(ctx.found);
        g_navDriverStop.store(false);
        HANDLE th = CreateThread(nullptr, 0, &navDriverThread, nullptr, 0, nullptr);
        if (th)
            CloseHandle(th);
        spdlog::info("[testdrv] nav driver armed ({:d} steps, window {:p}, out-of-band tick)",
                     g_navLen, (void*)ctx.found);
    } else {
        g_navTimer = SetTimer(nullptr, 0, 250, navTimerProc); // WM_TIMER fallback
        spdlog::info("[testdrv] nav driver armed ({:d} steps, WM_TIMER fallback)", g_navLen);
    }
}

} // namespace autonav
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
