/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Auto-nav executor. See testdrv/autonav.h.
 *
 * A thin in-process agent that invokes button functors / sets listbox selections on the UI
 * thread. It ticks from a Detour of sub_5629CA (the per-frame screen-loop helper), not the
 * message pump: the functor must run on the dialog-owning thread, and the GPU-less DisciplesGL
 * software-render path does not reliably service the pump. Two modes: SELFNAV runs a built-in
 * script (minimal single-instance tests); RELAY_BRIDGE executes the dispatcher's invoke/select
 * commands (the agent holds no test logic, the dispatcher owns that).
 * Compile-gated by D2_TESTDRV.
 */

#ifdef D2_TESTDRV

#include "testdrv/autonav.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/worldactions.h"
#include "testdrv/worldreporter.h"
#include "button.h"
#include "dialoginterf.h"
#include "editboxinterf.h"
#include "listbox.h"
#include "smartptr.h"
#include "spinbuttoninterf.h"
#include "togglebutton.h"
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
    Delay,        // wait <param> ms
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
const NavStep g_exitScript[] = {
    {NavAction::WaitDialog, "DLG_MAIN_MENU", "", 0},
    {NavAction::Delay, "", "", 800},
    {NavAction::Invoke, "DLG_MAIN_MENU", "BTN_QUIT", 0},
    {NavAction::Done, "", "", 0},
};

const NavStep* g_navScript = nullptr; // active self-nav script (null in dispatcher-only mode)
int g_navLen = 0;
int g_navIdx = 0;
DWORD g_stepStart = 0;
bool g_active = false;      // the agent acts (selfnav and/or dispatcher-driven)
bool g_navArmed = false;
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

// A command carries a sequence id so the dispatcher's POST can wait for its outcome. kNoSeq marks
// internal callers (self-nav, auto-dismiss) that have no POST waiting on a result.
constexpr std::uint32_t kNoSeq = 0xFFFFFFFFu;

// Report whether a command resolved its target. Sent BEFORE the action runs, so the POST returns
// even when the click triggers a ~10s blocking send; the action's effect is observed via UI state.
void reportFound(std::uint32_t seq, bool found)
{
    if (seq != kNoSeq)
        bridge::send_command_result(seq, found);
}

bool invokeButton(const char* dlgName, const char* btnName, std::uint32_t seq = kNoSeq)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    game::CBFunctorDispatch0* functor = nullptr;
    // SEH around the resolve: a popup can close mid-tick, leaving a stale dialog ptr.
    __try {
        game::CButtonInterf* btn = dlg ? game::CDialogInterfApi::get().findButton(dlg, btnName) : nullptr;
        if (btn && btn->buttonData) {
            game::CBFunctorDispatch0* f = btn->buttonData->onClickedFunctor.data;
            if (f && f->vftable && f->vftable->runCallback)
                functor = f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        functor = nullptr;
    }
    reportFound(seq, functor != nullptr);
    if (!functor)
        return false;
    bool invoked = false;
    __try {
        spdlog::info("[testdrv] nav invoke {}::{}", dlgName, btnName);
        functor->vftable->runCallback(functor);
        invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    }
    return ok;
}

bool setListSelection(const char* dlgName, const char* lbName, int index, std::uint32_t seq = kNoSeq)
{
    game::CDialogInterf* dlg = uistatereporter::findDialog(dlgName);
    game::CListBoxInterf* lb = nullptr;
    __try {
        lb = dlg ? game::CDialogInterfApi::get().findListBox(dlg, lbName) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lb = nullptr;
    }
    reportFound(seq, lb != nullptr);
    if (!lb)
        return false;
    bool ok = false;
    __try {
        game::CListBoxInterfApi::get().setSelectedIndex(lb, index);
        // A raw setSelectedIndex does not run the listbox callback that a real click runs. Menus
        // such as DLG_RANDOM_SCENARIO_MULTI rebuild their parameter spins from that callback; without
        // it the template matrix selects a new row while generation still reads stale/default options.
        auto* callback = lb->listBoxData ? lb->listBoxData->onSelectionConfirmed.data : nullptr;
        if (callback && callback->vftable && callback->vftable->runCallback)
            callback->vftable->runCallback(callback, index);
        spdlog::info("[testdrv] nav select {}::{} = {:d} (total={:d})", dlgName, lbName, index,
                     lb->listBoxData ? lb->listBoxData->elementsTotal : -1);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
        eb = nullptr;
    }
    reportFound(seq, eb != nullptr);
    if (!eb)
        return false;
    bool ok = false;
    __try {
        game::CEditBoxInterfApi::get().setString(eb, text);
        spdlog::info("[testdrv] nav edit {}::{} = '{}'", dlgName, editName, text);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

// --- dispatcher-driven remote commands ---------------------------------------
// Commands arrive on the BRIDGE thread (onRemoteCommand only queues); the per-frame tick
// drains them on the UI thread, where invoking a functor is safe.
struct RemoteCmd
{
    int type; // 0 = invoke button, 1 = listbox selection, 2 = spin option, 3 = edit-box text, 4 = move stack
    std::uint32_t seq; // echoed in the CommandResult so the relay can match the waiting POST
    char dlg[48];    // for move (type 4): the stack id string
    char widget[48];
    int param;       // index (listbox / spin)
    char value[64];  // text (edit box)
    int x, y;        // target tile (move stack)
};
std::mutex g_remoteMutex; // guards g_remoteCmds, g_inFlight, g_hasInFlight
std::deque<RemoteCmd> g_remoteCmds;
RemoteCmd g_inFlight{};     // command being executed right now (popped out of the queue)
bool g_hasInFlight = false; // whether g_inFlight is valid

void onRemoteCommand(std::uint16_t op, const std::uint8_t* p, std::uint32_t size)
{
    if (op != 0x0300 && op != 0x0301 && op != 0x0302 && op != 0x0303 && op != 0x0305
        && op != 0x0306 && op != 0x0307 && op != 0x0308 && op != 0x0309)
        return; // not a command we own
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
    if (off + 4 > size) // every command starts with a u32 seq
        return;
    cmd.seq = *reinterpret_cast<const std::uint32_t*>(p + off);
    off += 4;
    if (op == 0x0300 || op == 0x0306) { // InvokeButton / InvokeToggle: u16 dlgLen|dlg | u16 nameLen|name
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        cmd.type = (op == 0x0300) ? 0 : 5;
    } else if (op == 0x0301 || op == 0x0302) {
        // SetSelection (listbox) / SetSpin (spin button): u16 dlg | u16 widget | u32 index
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        if (off + 4 > size)
            return;
        cmd.param = *reinterpret_cast<const int*>(p + off);
        cmd.type = (op == 0x0301) ? 1 : 2;
    } else if (op == 0x0303) { // SetEditText: u16 dlg | u16 edit | u16 text
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget))
            || !readStr(cmd.value, sizeof(cmd.value)))
            return;
        cmd.type = 3;
    } else if (op == 0x0305) { // MoveStack: u16 stackId | u32 x | u32 y
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)))
            return;
        if (off + 8 > size)
            return;
        cmd.x = *reinterpret_cast<const int*>(p + off);
        cmd.y = *reinterpret_cast<const int*>(p + off + 4);
        cmd.type = 4;
    } else if (op == 0x0307) { // HireMerc: u16 campId | u16 stackId | u16 unitId
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget))
            || !readStr(cmd.value, sizeof(cmd.value)))
            return;
        cmd.type = 6;
    } else if (op == 0x0308) { // MoveGroupUnit: u16 stackId | u32 sourcePos | u32 targetPos
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)))
            return;
        if (off + 8 > size)
            return;
        cmd.x = *reinterpret_cast<const int*>(p + off);     // sourcePos
        cmd.y = *reinterpret_cast<const int*>(p + off + 4); // targetPos
        cmd.type = 7;
    } else { // 0x0309 DismissUnit: u16 stackId | u16 unitId
        if (!readStr(cmd.dlg, sizeof(cmd.dlg)) || !readStr(cmd.widget, sizeof(cmd.widget)))
            return;
        cmd.type = 8;
    }
    auto sameCmd = [&](const RemoteCmd& q) {
        return q.type == cmd.type && q.param == cmd.param && q.x == cmd.x && q.y == cmd.y
               && lstrcmpA(q.dlg, cmd.dlg) == 0 && lstrcmpA(q.widget, cmd.widget) == 0
               && lstrcmpA(q.value, cmd.value) == 0;
    };
    std::lock_guard<std::mutex> lk(g_remoteMutex);
    // Coalesce against the queue AND the in-flight command: while the UI thread is blocked
    // ~10s inside a synchronous DPlay send the executing command is no longer in the queue,
    // so without this a dispatcher re-fire would double-act (two JoinSession -> error). A
    // coalesced duplicate is acked as found so its POST returns at once instead of timing out;
    // a real not-found command never blocks, so it would not be in-flight to coalesce against.
    if (g_hasInFlight && sameCmd(g_inFlight)) {
        bridge::send_command_result(cmd.seq, true);
        return;
    }
    for (const auto& q : g_remoteCmds)
        if (sameCmd(q)) {
            bridge::send_command_result(cmd.seq, true);
            return;
        }
    g_remoteCmds.push_back(cmd);
}

// worldactions::moveStack reads live game objects and issues a net message; like safeRebuildWorld it
// allocates, so it cannot host __try itself (C2712). Guard the call here, in a frame with no unwinding
// locals, then report the outcome. (A move on the strategic map does not block like a DPlay join, so
// reporting after the issue is fine.)
void safeMoveStack(const RemoteCmd& cmd)
{
    bool ok = false;
    __try {
        ok = worldactions::moveStack(cmd.dlg, cmd.x, cmd.y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    reportFound(cmd.seq, ok);
}

// worldactions::hireMerc sends a client net-message after a group-slot scan; like safeMoveStack it
// touches game objects, so the __try lives here (a frame with no unwinding locals), not in the action.
void safeHireMerc(const RemoteCmd& cmd)
{
    bool ok = false;
    __try {
        ok = worldactions::hireMerc(cmd.dlg, cmd.widget, cmd.value); // dlg=campId, widget=stackId, value=unitId
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    reportFound(cmd.seq, ok);
}

// worldactions::moveGroupUnit sends a client net-message after a group-slot read; same __try seam.
void safeMoveGroupUnit(const RemoteCmd& cmd)
{
    bool ok = false;
    __try {
        ok = worldactions::moveGroupUnit(cmd.dlg, cmd.x, cmd.y); // dlg=stackId, x=sourcePos, y=targetPos
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    reportFound(cmd.seq, ok);
}

// worldactions::dismissUnit sends a client net-message after a group scan; same __try seam.
void safeDismissUnit(const RemoteCmd& cmd)
{
    bool ok = false;
    __try {
        ok = worldactions::dismissUnit(cmd.dlg, cmd.widget); // dlg=stackId, widget=unitId
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
            if (g_remoteCmds.empty()) {
                g_hasInFlight = false; // nothing executing
                break;
            }
            cmd = g_remoteCmds.front();
            g_remoteCmds.pop_front();
            g_inFlight = cmd; // publish before unlocking, so a re-fire during the DPlay block coalesces
            g_hasInFlight = true;
        }
        if (cmd.type == 0)
            invokeButton(cmd.dlg, cmd.widget, cmd.seq);
        else if (cmd.type == 1)
            setListSelection(cmd.dlg, cmd.widget, cmd.param, cmd.seq);
        else if (cmd.type == 2)
            setSpinOption(cmd.dlg, cmd.widget, cmd.param, cmd.seq);
        else if (cmd.type == 3)
            setEditText(cmd.dlg, cmd.widget, cmd.value, cmd.seq);
        else if (cmd.type == 4)
            safeMoveStack(cmd);
        else if (cmd.type == 5)
            invokeToggle(cmd.dlg, cmd.widget, cmd.seq);
        else if (cmd.type == 6)
            safeHireMerc(cmd);
        else if (cmd.type == 7)
            safeMoveGroupUnit(cmd);
        else if (cmd.type == 8)
            safeDismissUnit(cmd);
    }
}

// One self-nav step (minimal tests only).
void navStep()
{
    if (!g_navScript || g_navIdx >= g_navLen)
        return;

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
    case NavAction::Delay:
        advance = (GetTickCount() - g_stepStart) >= (DWORD)s.param;
        break;
    }

    if (advance) {
        ++g_navIdx;
        g_stepStart = GetTickCount();
        return;
    }

    if (s.action != NavAction::Delay && (GetTickCount() - g_stepStart) >= kStepTimeoutMs) {
        spdlog::warn("[testdrv] nav step {:d} ({}::{}) timed out, skipping", g_navIdx, s.dlg, s.widget);
        ++g_navIdx;
        g_stepStart = GetTickCount();
    }
}

// --- per-frame hook -----------------------------------------------------------
// sub_5629CA: the per-iteration helper the screen loop (sub_56288A) calls on the UI thread;
// Detour it to tick there once per iteration (see file header for why, not the message pump).
using Sub5629CA_t = LONG(__stdcall*)(HWND, LPPOINT, LONG*);
Sub5629CA_t g_orig5629CA = reinterpret_cast<Sub5629CA_t>(0x5629CA);

LONG __stdcall hook5629CA(HWND hWnd, LPPOINT lpPoint, LONG* a3)
{
    refreshRoleTitle(hWnd);
    tick();
    return g_orig5629CA(hWnd, lpPoint, a3);
}

bool g_frameHookInstalled = false;
bool installFrameHook()
{
    if (g_frameHookInstalled)
        return true;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(g_orig5629CA), hook5629CA);
    if (DetourTransactionCommit() == NO_ERROR) {
        g_frameHookInstalled = true;
        spdlog::info("[testdrv] nav frame-hook installed (sub_5629CA per-frame tick)");
    } else {
        spdlog::error("[testdrv] nav frame-hook: DetourTransactionCommit failed");
    }
    return g_frameHookInstalled;
}

} // namespace

bool onUiReady()
{
    char role[16]{};
    GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
    const bool selfnav = testenv::on("D2TESTDRV_SELFNAV");
    const bool relay = testenv::on("D2TESTDRV_RELAY_BRIDGE");

    if (selfnav && lstrcmpiA(role, "exit") == 0) {
        g_navScript = g_exitScript;
        g_navLen = (int)(sizeof(g_exitScript) / sizeof(g_exitScript[0]));
        g_navIdx = 0;
    }
    if (relay)
        bridge::setCommandCallback(&onRemoteCommand);
    if (selfnav || relay) {
        g_active = true;
        if (!installFrameHook())
            return false;
    }
    spdlog::info("[testdrv] nav: role='{}' exit-selfnav={} relay-driven={}", role,
                 selfnav && lstrcmpiA(role, "exit") == 0, relay);
    return true;
}

void onDialogBound()
{
    if (g_navArmed || !g_active)
        return;
    g_navArmed = true;
    g_stepStart = GetTickCount();
    spdlog::info("[testdrv] nav armed");
}

// worldreporter::rebuildSnapshot() reads live game objects through ScenarioView (which allocates), so
// it cannot host __try itself (MSVC C2712). Guard it here, in a frame with no unwinding locals, so a
// bad read during a scenario load/teardown can never crash the game (reporting is best-effort).
void rebuildWorldGuardedSeh()
{
    __try {
        worldreporter::rebuildSnapshot();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A single-instance (skirmish/hotseat) scenario load can also THROW a C++ exception while building an
// STL container over a half-built object map, which the SEH __except above does not catch, so wrap
// the guarded call in a C++ try/catch as well (the in-DLL CRT-report capture would otherwise fail-fast).
void safeRebuildWorld()
{
    try {
        rebuildWorldGuardedSeh();
    } catch (...) {
    }
}

// Ticked per screen-loop iteration on the dialog-owning thread (hook5629CA). Reentrancy-guarded.
void tick()
{
    if (!g_navArmed)
        return;
    static bool s_inTick = false;
    if (s_inTick)
        return;
    s_inTick = true;
    uistatereporter::refreshCurrentDialog(); // report the REAL topmost dialog (catch modal closes)
    safeRebuildWorld();                      // report players' resources + map stacks (world snapshot)
    drainRemoteCommands();
    if (g_navScript)
        navStep();
    s_inTick = false;
}

} // namespace autonav
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
