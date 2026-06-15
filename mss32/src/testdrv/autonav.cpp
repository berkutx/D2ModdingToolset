/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Auto-nav driver — see testdrv/autonav.h.
 *
 * Drives the menu chain hands-free by invoking buttons' onClicked functors directly
 * (no synthetic input), ticked from a background driver thread (no message pump, so
 * it works even where the game's window/loop are split across threads, e.g. the
 * DisciplesGL wrapper on a headless runner). Roles (D2TESTDRV_ROLE):
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
#include <detours.h>

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
    WaitHostReady,     // wait until the host has FULLY entered strategic (orchestrator signal, join script)
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
    {NavAction::WaitDialog, "DLG_LOBBY", "", 0}, // sit in the multiplayer lobby...
    // ...and wait until the HOST has FULLY entered the strategic map before pressing
    // OK. The joiner's OK sends CMenusReqStartGameMsg, to which the host responds by
    // immediately serving its CURRENT scenario snapshot; if the host is still loading
    // (it is much slower on a GPU-less software-render runner) that snapshot is
    // inconsistent and the joiner dies applying it. The correct entry order is
    // host-fully-in-strategic THEN joiner: gate the OK on the host's actual readiness,
    // which the orchestrator observes from the host log and signals via the file named
    // by D2TESTDRV_HOST_READY_FILE (no timing guesses, no defensive masking).
    {NavAction::WaitHostReady, "", "", 0},
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
int g_scenarioIdx = 0;
constexpr DWORD kStepTimeoutMs = 15000; // menu transitions
constexpr DWORD kGateTimeoutMs = 60000; // RX gates (peer/host) — generous for a real pairing

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

// True once the orchestrator has signalled that the HOST has FULLY entered the
// strategic map — it observes the host log and creates the file named by
// D2TESTDRV_HOST_READY_FILE. The joiner waits on this before requesting start, so the
// host only ever serves a fully-loaded, consistent scenario snapshot. With no file
// configured (e.g. a standalone run) the gate is inert and passes immediately.
bool hostReady()
{
    char path[MAX_PATH]{};
    if (GetEnvironmentVariableA("D2TESTDRV_HOST_READY_FILE", path, sizeof(path)) == 0)
        return true; // not orchestrated -> nothing to wait for
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// One nav step. Driven directly on a background thread (no window, no message
// pump): invokeButton calls the button's onClicked functor via the dialog registry,
// and D2 has no strict UI-thread affinity, so the menu advances regardless of which
// thread owns the window or runs the message loop.
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
        g_navScript = nullptr; // stops the driver thread loop
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
    case NavAction::WaitHostReady:
        advance = hostReady();
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
                         || s.action == NavAction::WaitHostStrategic
                         || s.action == NavAction::WaitHostReady);
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

// --- per-frame hook -----------------------------------------------------------
// sub_5629CA is the per-iteration helper the screen message loop (sub_56288A) calls
// on the UI/dialog thread; it leads into the DisciplesGL frame render. Detour it so
// the nav ticks once per iteration on the correct thread, with no dependency on the
// message pump being serviced. Tick BEFORE the original: the render is the slow part
// (software GL on a GPU-less runner can take a long time per frame), and the nav must
// advance per loop iteration regardless of how long the frame itself takes. Ticking
// after would starve the nav if a frame blocks. Not reentrant with dialog
// construction either way (called from the loop, not from assignFunctor).
using Sub5629CA_t = LONG(__stdcall*)(HWND, LPPOINT, LONG*);
Sub5629CA_t g_orig5629CA = reinterpret_cast<Sub5629CA_t>(0x5629CA);
std::atomic<std::uint32_t> g_frameCount{0};

LONG __stdcall hook5629CA(HWND hWnd, LPPOINT lpPoint, LONG* a3)
{
    const std::uint32_t n = g_frameCount.fetch_add(1) + 1;
    if (n == 1 || (n & 0xFFF) == 0)
        spdlog::info("[testdrv] nav frame-tick #{} (sub_5629CA)", n); // heartbeat: is the loop iterating?
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
    installFrameHook(); // per-frame tick from the screen loop (sub_5629CA)
    spdlog::info("[testdrv] nav role='{}' -> {:d} steps (gates={}, scenario={})", role, g_navLen,
                 wantsGates, g_scenarioIdx);
}

void onDialogBound()
{
    if (g_navArmed || !g_navScript)
        return;
    g_navArmed = true;
    g_stepStart = GetTickCount();
    spdlog::info("[testdrv] nav driver armed ({:d} steps, bind-hook tick)", g_navLen);
}

// Ticked from the UI-state reporter's assignFunctor hook — i.e. on the exact thread
// that owns the dialogs, so invoking a button functor is safe (no cross-thread crash)
// and needs no message pump. Reentrancy-guarded: a functor invoke can re-enter the
// bind hook. The game re-binds functors frequently enough to drive the script.
void tick()
{
    if (!g_navArmed)
        return;
    static bool s_inTick = false;
    if (s_inTick)
        return;
    s_inTick = true;
    navStep();
    s_inTick = false;
}

} // namespace autonav
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
