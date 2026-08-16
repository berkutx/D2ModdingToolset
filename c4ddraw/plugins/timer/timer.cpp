/*
 * timer.c4p - native C4dll-R plugin port of the legacy turn-timer mod (Mods\timer.mod).
 * Owns the menu, config/persistence, clock, and GDI+ draw; fragile per-version game hooks live in
 * the HOST (turn detection, combat/anim pause, day boundaries) and are reached via C4P_Host.
 *
 * Menu (legacy resource id 3, command ids base+1..base+0x16):
 *   Simple Mode { Enabled; On Day Start{Pause/Unpause/Reset}; On Day End{Pause/Unpause/Reset} }
 *   Force Turn Mode { Enabled; Animation Pause; Combat Pause{Off/PvP/PvAny};
 *                     On Elapse{End Day/Defend(disabled)/Retreat(disabled)/Auto Battle};
 *                     Reset Extra Time; Timetable... }
 *   Pause(Alt+P); Reset(Alt+R); Set...; Always Visible; About...
 */

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include "../../features/c4plugin.h"
#include "timer_dlg.h"
#include <cstdarg>
#include <cstring>
#include <stddef.h>
#include <stdint.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

HINSTANCE g_hinst = nullptr;
const C4P_Host* g_host = nullptr;
ULONG_PTR g_gdiToken = 0;
PrivateFontCollection* g_fonts = nullptr;
FontFamily* g_family = nullptr;
Font* g_font = nullptr;

// ---- config (C4plugins.ini [Timer] keys) ----
int g_state = 2;        // 0 = off, 1 = Simple (count up), 2 = Force Turn (count down)
int g_dayTurn = 10;     // bitmask: b0/1/2 = On Day Start Pause/Unpause/Reset; b3/4/5 = On Day End ...
int g_pauseOn = 1;      // Combat Pause: 0 = Off, 1 = PvP, 2 = PvAny
int g_pauseAnim = 1;    // Animation Pause
int g_turnDay = 1;      // On Elapse -> End Day
int g_autoBattle = 1;   // On Elapse -> forced native Auto Battle (PvP)
int g_elapseFired = 0;  // latch: on-elapse action fired this turn (re-armed when clock is positive)
int g_resetExtra = 0;   // Reset Extra Time
int g_alwaysVisible = 1;
int g_durBase = 300;    // TableDuration_0 (seconds): per-turn budget in Force mode (day-1 base)
// Timetable: up to 3 per-day overrides. From day TableDay_i the budget = TableDuration_i.
int g_tblActive[3] = {0, 0, 0};
int g_tblDay[3] = {0, 0, 0};
int g_tblDur[3] = {300, 300, 300};
int g_anchorX = 0;      // 0..10000 position in free space (0 = left/top)
int g_anchorY = 0;      // high-resolution form avoids visible percentage-sized jumps
REAL g_fontSize = 28.0f;

// ---- runtime clock ----
int g_paused = 0;       // manual pause OR combat-pause (both freeze the clock)
int g_combatPausing = 0; // 1 if WE auto-paused for Combat Pause (so we only auto-resume our own pause)
int g_running = 0;      // a real turn has started -> the clock counts. Does NOT run at launch / main
                        // menu, only once the turn begins (strategic view reached).
DWORD g_baseline = 0;   // tick the current count started from
DWORD g_pausedAt = 0;   // tick we paused at (clock frozen here while paused)
int g_extra = 0;        // current player's carried extra time (ms), added to the Force budget
// Per-player Force time bank {playerId, accumMs}, searched by id (not indexed): the id is the turn-info
// player byte and need not be a small index. id == -1 marks a free slot (so player id 0 is valid).
// 8 slots is ample (<=4 players + neutral/NPC).
struct BankSlot { int id; int accum; };
BankSlot g_bank[8] = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
void bankClear() { for (int i = 0; i < 8; ++i) { g_bank[i].id = -1; g_bank[i].accum = 0; } }
int* bankAccum(int playerId) // &accum for playerId; claims a free slot the first time the id is seen
{
    int freeSlot = -1;
    for (int i = 0; i < 8; ++i) {
        if (g_bank[i].id == playerId) return &g_bank[i].accum;
        if (freeSlot < 0 && g_bank[i].id < 0) freeSlot = i;
    }
    if (freeSlot < 0) freeSlot = 0; // table full (impossible with <=5 players): reuse first slot
    g_bank[freeSlot].id = playerId;
    g_bank[freeSlot].accum = 0;
    return &g_bank[freeSlot].accum;
}
int g_lastPlayer = -1;  // player whose turn it was last (to bank their remaining on turn change)
int g_wasActive = 0;    // previous tick's turn_active, to detect the 0->1 (my turn started) edge
int g_turnAccepted = 0; // Force clock is allowed to run for this local turn (Russobit: only after OK)
int g_expired = 0;      // Force timeout is clamped/frozen at exactly 00:00
int g_curDayBudget = 0; // day captured at the current turn-start (for the previous turn's budget)
uint32_t g_lastSerial = 0; // last processed turn serial; a bump = a real turn change (off[6] turn-info)
uint32_t g_lastBeginTurnAck = UINT32_MAX; // UINT32_MAX = host/exe has no precise acknowledgement hook

// Guards config + clock shared between c4p_command (game UI thread) and c4p_tick (worker thread).
CRITICAL_SECTION g_lock;

// ---- menu ----
HMENU g_menu = nullptr; // our top-level popup (grafted directly under Plugins -> Timer)
int g_base = 0;         // base command id the host gave us (our 0x100 block)
// command offsets = legacy resource-3 ids (decompiled sub_10004D40 WndProc):
// +0x13 Timetable (res 5), +0x15 Help (msgbox), +0x16 About (res 4)
enum {
    kSimpleOn = 1, kPause = 2, kReset = 3, kSet = 4,
    kDayStartPause = 5, kDayStartUnpause = 6, kDayStartReset = 7,
    kDayEndPause = 8, kDayEndUnpause = 9, kDayEndReset = 10,
    kForceOn = 0xB, kCombatOff = 0xC, kCombatPvP = 0xD, kCombatPvAny = 0xE, kAnimPause = 0xF,
    kElapseEndDay = 0x10, kElapseRetreat = 0x11, kResetExtra = 0x12, kTimetable = 0x13,
    kAlwaysVis = 0x14, kHelp = 0x15, kAbout = 0x16,
    kElapseDefend = 0x17, kElapseAutoBattle = 0x18
};

// the rendered frame + a change signature (avoid pointless redraws)
wchar_t g_text[64] = L"";
DWORD g_textColor = 0xFFCC9900;
DWORD g_shadowColor = 0xFF660000;
bool g_visible = false;
unsigned g_sig = 0;
int g_hitLeft = 0, g_hitTop = 0, g_hitWidth = 0, g_hitHeight = 0;
int g_canvasWidth = 0, g_canvasHeight = 0;
int g_dragging = 0, g_dragDx = 0, g_dragDy = 0;

// ---- time formatting + colours (faithful to timer.mod DrawFrame) ----
void formatTime(int v9, wchar_t* out)
{
    int v14;
    if (v9 < 0) {
        v14 = v9 / -1000;
    } else {
        v14 = v9 / 1000 + 1;
        if (v9 == 1000 * (v9 / 1000))
            v14 = v9 / 1000;
    }
    const int sec = v14 % 60;
    const wchar_t* sign = (v9 < 0 && v14) ? L"-" : L"";
    if (v14 / 60 / 60)
        wsprintfW(out, L"%s%02d:%02d:%02d", sign, v14 / 60 / 60, v14 / 60 % 60, sec);
    else
        wsprintfW(out, L"%s%02d:%02d", sign, v14 / 60 % 60, sec);
}

// On-screen ramp (R/B swapped vs the .mod's surface order): orange within budget -> red blink in the
// last 20s; white at 00:00 in count-up. No green spare-time branch in v1 (needs carried extra time).
void pickColors(int v9, int state, int paused, DWORD* text, DWORD* shadow)
{
    if (state == 2) {
        const bool blink = !paused && ((v9 < 0) || (v9 < 20000 && (v9 % 1000) > 500));
        *text = blink ? 0xFFFF3300 : 0xFFCC9900;
        *shadow = 0xFF660000;
    } else {
        if (paused || v9 / 1000 == 0) {
            *text = 0xFFFFFFFF;
            *shadow = 0xFF333333;
        } else {
            *text = 0xFFCC9900;
            *shadow = 0xFF660000;
        }
    }
}

const char* iniSection() { return "Timer"; }
void persist(const char* key, int val) { if (g_host) g_host->set_config_int(iniSection(), key, val); }

bool timerDiagnosticsEnabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        char env[4] = {};
        char ini[MAX_PATH] = {};
        if (g_host && g_host->config_path()) {
            lstrcpynA(ini, g_host->config_path(), MAX_PATH);
            char* slash = strrchr(ini, '\\');
            if (slash)
                lstrcpyA(slash + 1, "C4menu.ini");
        }
        enabled = (GetEnvironmentVariableA("C4DLL_DEBUG", env, sizeof(env)) != 0 ||
                   (ini[0] && GetPrivateProfileIntA("menu", "debugLog", 0, ini) != 0))
            ? 1
            : 0;
    }
    return enabled != 0;
}

void timerTrace(const char* fmt, ...)
{
    if (!timerDiagnosticsEnabled())
        return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(msg, fmt, ap);
    va_end(ap);
    OutputDebugStringA(msg);

    char path[MAX_PATH] = {};
    if (g_host && g_host->config_path()) {
        lstrcpynA(path, g_host->config_path(), MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash)
            wsprintfA(slash + 1, "C4menu-%lu.log", GetCurrentProcessId());
    }
    if (!path[0])
        return;
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char line[560];
        const int length = wsprintfA(line, "%s\r\n", msg);
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
}

bool hostHas(size_t memberEnd)
{
    return g_host && g_host->struct_size >= memberEnd;
}

#define HOST_HAS(member) \
    (hostHas(offsetof(C4P_Host, member) + sizeof(g_host->member)) && g_host->member)

// Host battle state for Combat Pause. struct_size-guarded so an older host (no is_in_battle) returns 0.
int hostInBattle()
{
    return HOST_HAS(is_in_battle)
               ? g_host->is_in_battle()
               : 0;
}

// LOCAL player's turn is "active" when the client's strategic END_TURN button exists and is enabled.
// Per-client, so it is valid on an MP joiner where the server turn serial (host-only) never changes.
// Defaults to 1 on an older host (no callback), preserving prior single-player behavior.
int hostTurnActive()
{
    return HOST_HAS(turn_active)
               ? g_host->turn_active()
               : 1;
}

// Current scenario day (for the Timetable); -1 if unavailable. struct_size-guarded.
int hostDay()
{
    return HOST_HAS(get_day)
               ? g_host->get_day()
               : -1;
}

// is_animating: a battle attack animation is playing. battle_kind: 0 none, 1 PvP, 2 any combat.
int hostAnimating()
{
    return HOST_HAS(is_animating)
               ? g_host->is_animating()
               : 0;
}
// On-Elapse actions are queued to the host; all game callbacks run later on its UI thread.
void hostEndDay()
{
    if (HOST_HAS(end_day))
        g_host->end_day();
}
void hostCancelElapse()
{
    if (HOST_HAS(cancel_elapse))
        g_host->cancel_elapse();
}
int hostBattleKind()
{
    return HOST_HAS(battle_kind)
               ? g_host->battle_kind()
               : 0;
}

int hostBattleTurnActive()
{
    return HOST_HAS(battle_turn_active)
        ? g_host->battle_turn_active()
        : -1;
}

bool hostBattleTimerState(C4P_BattleTimerState* out)
{
    if (!out)
        return false;
    *out = {};
    out->struct_size = sizeof(*out);
    out->local_active = -1;
    out->animation_active = -1;
    out->playback_local = -1;

    if (HOST_HAS(get_battle_timer_state)) {
        if (g_host->get_battle_timer_state(out))
            return true;
        // A new host that cannot publish a coherent snapshot must fail closed. Keep only the
        // legacy battle classification so policy sees combat + unknown ownership, never three
        // independently sampled fields from different controller generations.
        out->battle_kind = hostBattleKind();
        return false;
    }

    // Compatibility with older hosts which predate the atomic 1.7 state callback.
    out->battle_kind = hostBattleKind();
    out->local_active = out->battle_kind ? hostBattleTurnActive() : -1;
    out->animation_active = hostAnimating();
    return true;
}

int hostTurnPlayerId()
{
    return HOST_HAS(turn_player_id) ? g_host->turn_player_id() : -1;
}

int hostForceAutoBattle()
{
    return HOST_HAS(force_auto_battle) ? g_host->force_auto_battle() : 0;
}

// Russobit publishes a monotonic edge only after DLG_BEGIN_TURN's normal BTN_OK callback has
// completed. Other executable layouts return UINT32_MAX and retain the active-turn-edge behavior.
uint32_t hostBeginTurnAckSerial()
{
    const size_t required =
        offsetof(C4P_Host, begin_turn_ack_serial) +
        sizeof(g_host->begin_turn_ack_serial);
    return (g_host && g_host->struct_size >= required &&
            g_host->begin_turn_ack_serial)
        ? g_host->begin_turn_ack_serial()
        : UINT32_MAX;
}

// Per-turn budget (seconds) for a day: day-1 base until the first active Timetable entry, then the
// duration of the active entry with the largest TableDay <= day.
int budgetSecForDay(int day)
{
    if (day < 1) day = 1;
    int dur = g_durBase, bestDay = 0;
    for (int i = 0; i < 3; ++i) {
        if (g_tblActive[i] && g_tblDay[i] <= day && g_tblDay[i] > bestDay) {
            dur = g_tblDur[i];
            bestDay = g_tblDay[i];
        }
    }
    return dur < 1 ? 1 : dur;
}

void readConfig()
{
    g_state = g_host->get_config_int(iniSection(), "Enabled", 2);
    if (g_state < 0 || g_state > 2) g_state = 2;
    { const int dt = g_host->get_config_int(iniSection(), "DayTurn", 10);
      g_dayTurn = ((unsigned)dt <= 0x3Fu) ? dt : 10; } // legacy: reset to default 0x0A if out of range
    g_pauseOn = g_host->get_config_int(iniSection(), "PauseOn", 1);
    if (g_pauseOn < 0 || g_pauseOn > 2) g_pauseOn = 1;
    g_pauseAnim = g_host->get_config_int(iniSection(), "PauseAnimation", 1) ? 1 : 0;
    g_turnDay = g_host->get_config_int(iniSection(), "TurnDay", 1) ? 1 : 0;
    // Legacy Retreat is intentionally ignored; it is not migrated into the new action.
    g_autoBattle = g_host->get_config_int(iniSection(), "AutoBattle", 1) ? 1 : 0;
    g_resetExtra = g_host->get_config_int(iniSection(), "ResetExtraTime", 0) ? 1 : 0;
    g_alwaysVisible = g_host->get_config_int(iniSection(), "AlwaysVisible", 1) ? 1 : 0;
    g_durBase = g_host->get_config_int(iniSection(), "TableDuration_0", 300);
    if (g_durBase < 1) g_durBase = 1;
    for (int i = 0; i < 3; ++i) {
        char k[24];
        wsprintfA(k, "TableActive_%d", i + 1);  g_tblActive[i] = g_host->get_config_int(iniSection(), k, 0) ? 1 : 0;
        wsprintfA(k, "TableDay_%d", i + 1);      g_tblDay[i] = g_host->get_config_int(iniSection(), k, 0);
        wsprintfA(k, "TableDuration_%d", i + 1); { int d = g_host->get_config_int(iniSection(), k, 300); g_tblDur[i] = d < 1 ? 1 : d; }
    }
    // AnchorX/Y were whole percentages in the first c4p build. Keep them as a fallback, but use
    // 1/10000 anchors for smooth dragging at modern client sizes.
    const int oldAnchorX = g_host->get_config_int(iniSection(), "AnchorX", 0);
    const int oldAnchorY = g_host->get_config_int(iniSection(), "AnchorY", 0);
    g_anchorX = g_host->get_config_int(iniSection(), "AnchorX10000", oldAnchorX * 100);
    g_anchorY = g_host->get_config_int(iniSection(), "AnchorY10000", oldAnchorY * 100);
    if (g_anchorX < 0) g_anchorX = 0; if (g_anchorX > 10000) g_anchorX = 10000;
    if (g_anchorY < 0) g_anchorY = 0; if (g_anchorY > 10000) g_anchorY = 10000;
    int fs = g_host->get_config_int(iniSection(), "FontSize", 28);
    if (fs < 6) fs = 6; if (fs > 200) fs = 200;
    g_fontSize = (REAL)fs;
}

bool loadFont()
{
    g_fonts = new PrivateFontCollection();
    HRSRC hr = FindResourceA(g_hinst, MAKEINTRESOURCEA(7), (LPCSTR)RT_FONT);
    if (hr) {
        HGLOBAL hg = LoadResource(g_hinst, hr);
        void* data = LockResource(hg);
        DWORD size = SizeofResource(g_hinst, hr);
        if (data && size)
            g_fonts->AddMemoryFont(data, size);
    }
    int found = 0;
    g_family = new FontFamily();
    if (g_fonts->GetFamilyCount() > 0 &&
        g_fonts->GetFamilies(1, g_family, &found) == Ok && found > 0) {
        g_font = new Font(g_family, g_fontSize, FontStyleBold, UnitPixel);
        if (g_font->GetLastStatus() == Ok)
            return true;
    }
    delete g_font;
    g_font = new Font(L"Arial", g_fontSize, FontStyleBold, UnitPixel);
    return g_font->GetLastStatus() == Ok;
}

// ---- menu check/enable refresh ----
void chk(int off, bool on) { if (g_menu) CheckMenuItem(g_menu, g_base + off, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); }
void ena(int off, bool on) { if (g_menu) EnableMenuItem(g_menu, g_base + off, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); }

void refreshMenu()
{
    if (!g_menu)
        return;
    // Force/PvP is a non-stoppable clock: user Pause stays disabled, while automatic animation,
    // combat and not-my-turn freezes still work internally. Reset becomes available only after this
    // local turn has passed the precise begin-turn acknowledgement.
    const bool resettable =
        (g_state == 1) || (g_state == 2 && g_turnAccepted != 0);
    chk(kSimpleOn, g_state == 1);
    chk(kForceOn, g_state == 2);
    ena(kPause, g_state == 1);
    chk(kPause, g_paused != 0);
    ena(kReset, resettable);
    // Set is useful before Force mode has detected a live turn and remains safe while paused.
    // Only a completely disabled timer has no editable clock.
    ena(kSet, g_state != 0);
    // On Day Start (b0 Pause / b1 Unpause / b2 Reset)
    chk(kDayStartPause, (g_dayTurn & 1) != 0);
    chk(kDayStartUnpause, (g_dayTurn & 2) != 0);
    chk(kDayStartReset, (g_dayTurn & 4) != 0);
    // On Day End (b3 / b4 / b5)
    chk(kDayEndPause, (g_dayTurn & 8) != 0);
    chk(kDayEndUnpause, (g_dayTurn & 0x10) != 0);
    chk(kDayEndReset, (g_dayTurn & 0x20) != 0);
    chk(kCombatOff, g_pauseOn == 0);
    chk(kCombatPvP, g_pauseOn == 1);
    chk(kCombatPvAny, g_pauseOn == 2);
    chk(kAnimPause, g_pauseAnim != 0);
    chk(kElapseEndDay, g_turnDay != 0);
    ena(kElapseDefend, false);
    chk(kElapseDefend, false);
    ena(kElapseRetreat, false);
    chk(kElapseRetreat, false);
    ena(kElapseAutoBattle, g_state == 2);
    chk(kElapseAutoBattle, g_autoBattle != 0);
    chk(kResetExtra, g_resetExtra != 0);
    chk(kAlwaysVis, g_alwaysVisible != 0);
}

// ---- pause helpers (freeze/resume the clock by adjusting baseline) ----
void setPaused(int on)
{
    if (g_paused == on)
        return;
    const DWORD now = GetTickCount();
    if (on) {
        g_pausedAt = now; // freeze
    } else {
        g_baseline += now - g_pausedAt; // resume: skip the paused span
    }
    g_paused = on;
}

void restart()
{
    g_baseline = GetTickCount();
    g_pausedAt = g_baseline;
    g_extra = 0;
    g_elapseFired = 0;
    g_expired = 0;
    hostCancelElapse();
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
    }
    return TRUE;
}

extern "C" int __cdecl c4p_query(C4P_Info* out)
{
    if (!out || out->struct_size < sizeof(C4P_Info))
        return 0;
    out->abi_version = C4P_ABI_VERSION;
    out->id = "c4dll.timer";
    out->name = "Timer";
    out->reserved_v2 = nullptr;
    return 1;
}

extern "C" int __cdecl c4p_init(const C4P_Host* host)
{
    if (!host)
        return 0;
    g_host = host;
    GdiplusStartupInput in;
    if (GdiplusStartup(&g_gdiToken, &in, nullptr) != Ok) {
        g_gdiToken = 0;
        return 0;
    }
    readConfig();
    g_running = 0;                          // do NOT self-start; wait for a real turn (or a manual Reset)
    g_lastSerial = host->get_turn_serial(); // sync so the first real turn change is what starts the clock
    g_lastBeginTurnAck = hostBeginTurnAckSerial();
    g_turnAccepted = 0;
    g_expired = 0;
    if (!loadFont()) {
        c4p_shutdown();
        return 0;
    }
    return 1;
}

extern "C" void __cdecl c4p_tick(uint32_t now_ms)
{
    if (!g_host)
        return;

    const int inGame = g_host->is_in_game();
    const int dayNow = hostDay(); // read outside the lock; -1 (unknown) falls back to the day-1 base
    const int strategicActive = inGame ? hostTurnActive() : 0;
    C4P_BattleTimerState battleState = {};
    if (inGame)
        hostBattleTimerState(&battleState);
    else {
        battleState.struct_size = sizeof(battleState);
        battleState.local_active = -1;
        battleState.animation_active = 0;
        battleState.playback_local = -1;
    }
    const int battleKind = battleState.battle_kind;
    const int battleActive = battleState.local_active;
    const int animationActive = battleState.animation_active;
    const int playbackLocal = battleState.playback_local;
    const uint32_t beginTurnAck = hostBeginTurnAckSerial();
    int state, durMs, alwaysVis, paused, running, extra;
    DWORD baseline, pausedAt;
    EnterCriticalSection(&g_lock);
    const uint32_t serial = g_host->get_turn_serial();
    if (!inGame) {
        // Main menu / between games: timer does NOT run. Keep g_lastSerial synced so the first real
        // turn-start inside a game is seen as a change.
        if (g_running || g_elapseFired)
            hostCancelElapse();
        g_running = 0;
        g_turnAccepted = 0;
        g_elapseFired = 0;
        g_expired = 0;
        g_paused = 0;
        g_combatPausing = 0;
        g_curDayBudget = 1;
        g_lastSerial = serial;
        g_lastBeginTurnAck = beginTurnAck;
    } else {
        // A defender can be attacked before their first strategic turn. Create their one personal
        // Timetable budget on the first verified local PvP unit, without fabricating a strategic
        // turn edge. The next real strategic turn will bank this remainder and add its normal budget.
        if (g_state == 2 && battleKind == 1 &&
            (battleActive == 1 || playbackLocal == 1) && !g_running) {
            bankClear();
            const int localPlayer = hostTurnPlayerId();
            g_extra = localPlayer >= 0 ? *bankAccum(localPlayer) : 0;
            g_lastPlayer = localPlayer;
            g_baseline = now_ms;
            g_pausedAt = now_ms;
            g_curDayBudget = dayNow >= 0 ? dayNow : 1;
            g_running = 1;
            g_expired = 0;
            g_elapseFired = 0;
            hostCancelElapse();
            timerTrace("[timer] PvP defender bank initialized (player=%d day=%d budget=%ds)",
                       localPlayer, g_curDayBudget, budgetSecForDay(g_curDayBudget));
        }

        // On Russobit, the clock edge is the native DLG_BEGIN_TURN BTN_OK callback, not dialog
        // creation and not clientTakesTurn becoming true underneath the modal. If the callback ran
        // just before active became observable, its serial remains pending and is consumed here.
        // Other exe layouts have no validated callback and retain the reliable per-client 0->1 edge.
        const bool preciseAck = beginTurnAck != UINT32_MAX;
        const bool acceptedEdge =
            strategicActive && preciseAck && beginTurnAck != g_lastBeginTurnAck;
        const bool fallbackEdge = strategicActive && !preciseAck && !g_wasActive;
        if (strategicActive && !g_wasActive)
            g_turnAccepted = 0;

        if (acceptedEdge || fallbackEdge) {
            if (acceptedEdge)
                g_lastBeginTurnAck = beginTurnAck;
            g_turnAccepted = 1;
            // MY turn is now usable. Force = Fischer bank (each turn +budget, unused carries per
            // player); Simple = count-up + On-Day bits.
            int newPlayer = hostTurnPlayerId();
            if (newPlayer < 0)
                newPlayer = g_host->get_turn_player();
            const int firstTurn = !g_running;
            g_lastSerial = serial;
            hostCancelElapse();
            g_elapseFired = 0; // re-arm the on-elapse latch (even an out-of-time turn auto-skips)
            g_expired = 0;
            if (firstTurn) {
                bankClear(); // fresh game
                g_extra = 0;
                g_baseline = now_ms;
                g_pausedAt = now_ms;
                g_curDayBudget = dayNow >= 0 ? dayNow : 1;
            } else if (g_state == 2) {
                // Force: bank the PREVIOUS player's remaining (budget + extra - elapsed), then load
                // the NEW player's bank and start a fresh budget. ResetExtraTime drops the carry.
                if (g_lastPlayer >= 0) {
                    // FROZEN clock: the not-my-turn pause set g_pausedAt without advancing
                    // g_baseline, and this runs BEFORE the resume below. Clamp >=0 so a Set-future
                    // baseline or GetTickCount wrap cannot inflate carried time.
                    int elapsedPrev =
                        (int)((g_paused ? g_pausedAt : now_ms) - g_baseline);
                    if (elapsedPrev < 0) elapsedPrev = 0;
                    const int budgetPrev =
                        budgetSecForDay(g_curDayBudget) * 1000;
                    int remainingPrev = budgetPrev + g_extra - elapsedPrev;
                    if (remainingPrev < 0)
                        remainingPrev = 0;
                    *bankAccum(g_lastPlayer) = g_resetExtra ? 0 : remainingPrev;
                }
                if (dayNow >= 0)
                    g_curDayBudget = dayNow; // retain last-known day if get_day == -1
                g_extra = (newPlayer >= 0) ? *bankAccum(newPlayer) : 0;
                g_baseline = now_ms;
                g_pausedAt = now_ms;
            } else if (g_state == 1) {
                // Simple (count-up): apply On-Day-End then On-Day-Start DayTurn bits; otherwise keep
                // counting across turns (legacy resets only when a Reset bit fires).
                if (g_dayTurn & 0x20) restart();         // DayEnd Reset
                if (g_dayTurn & 0x08) setPaused(1);      // DayEnd Pause
                else if (g_dayTurn & 0x10) setPaused(0); // DayEnd Unpause
                if (g_dayTurn & 0x04) restart();         // DayStart Reset
                if (g_dayTurn & 0x01) setPaused(1);      // DayStart Pause
                else if (g_dayTurn & 0x02) setPaused(0); // DayStart Unpause
            }
            g_running = 1;
            g_lastPlayer = newPlayer;
        }

        if (!strategicActive)
            g_turnAccepted = 0;
    }
    g_wasActive = strategicActive;

    // Strategic ownership starts/banks budgets. In PvP there are two verified billable phases:
    // directed ChooseAction opens local decision time; broadcast BattleResult identifies whose
    // playback is being shown. Opponent decision/playback is never billed. Local playback is billed
    // only when Animation Pause is OFF, preserving that option for long slow-motion attacks.
    int wantPause = 0;
    const char* pauseReason = g_running ? "running" : "not-started";
    if (g_running && g_expired) {
        wantPause = 1;
        pauseReason = "expired";
    } else if (g_running && g_state == 2 && battleKind == 1) {
        if (g_pauseOn == 1 || g_pauseOn == 2) {
            wantPause = 1; // explicit Combat Pause overrides local decision-time counting
            pauseReason = "combat-pause-pvp";
        } else if (battleActive == 1 && battleState.selection_open == 1) {
            // An accepted local ChooseAction is authoritative decision time, including the first
            // choice while the viewer's aggregate ready byte can still report the battle intro as
            // busy, and the mandatory second hit while normal battle buttons are hidden. Exact
            // Result playback is attributed by playbackLocal below; the raw animation byte must
            // never mask an already-open decision.
            wantPause = 0;
            pauseReason = "local-battle-selection";
        } else if (playbackLocal == 1 && !g_pauseAnim) {
            wantPause = 0;
            pauseReason = "local-battle-playback";
        } else if (playbackLocal == 1) {
            wantPause = 1;
            pauseReason = "local-battle-playback-paused";
        } else if (battleActive < 0 && playbackLocal < 0) {
            wantPause = 1; // neither a verified local choice nor a classified Result is available
            pauseReason = "battle-state-unavailable";
        } else {
            wantPause = 1;
            pauseReason = playbackLocal == 0
                ? "opponent-battle-playback"
                : "battle-selection-closed";
        }
    } else if (g_running && inGame &&
               (!strategicActive || (g_state == 2 && !g_turnAccepted))) {
        wantPause = 1; // ordinary strategic boundary / begin-turn acknowledgement
        pauseReason = !strategicActive ? "not-local-strategic-turn" : "turn-not-acknowledged";
    } else if (g_running && g_state == 2) {
        if (g_pauseAnim && animationActive != 0) {
            wantPause = 1;
            pauseReason = animationActive < 0 ? "animation-unknown" : "animation";
        } else if (g_pauseOn == 1) {
            wantPause = (battleKind == 1) ? 1 : 0; // PvP only
            pauseReason = wantPause ? "combat-pause-pvp" : "running";
        } else if (g_pauseOn == 2) {
            wantPause = (battleKind != 0) ? 1 : 0; // any combat
            pauseReason = wantPause ? "combat-pause-any" : "running";
        }
    }

    // State transitions alone were insufficient diagnostics: the old broken producer left the
    // clock paused at -1 without ever changing it again. Publish every policy-input change with an
    // explicit reason so a two-client trace proves both the event source and the timer decision.
    static int lastWantPause = -1;
    static int lastStrategic = -2;
    static int lastBattleKind = -2;
    static int lastBattleActive = -2;
    static int lastAnimation = -2;
    static int lastCombatPause = -2;
    static int lastRunning = -2;
    static int lastAccepted = -2;
    static int lastSelection = -2;
    static int lastContinuation = -2;
    static int lastPlaybackLocal = -2;
    static int lastPauseAnimation = -2;
    static uint32_t lastBattleGeneration = UINT32_MAX;
    static uint32_t lastBattleInstance = UINT32_MAX;
    static const char* lastPauseReason = nullptr;
    if (wantPause != lastWantPause || strategicActive != lastStrategic ||
        battleKind != lastBattleKind || battleActive != lastBattleActive ||
        animationActive != lastAnimation || g_pauseOn != lastCombatPause ||
        g_running != lastRunning || g_turnAccepted != lastAccepted ||
        battleState.selection_open != lastSelection ||
        battleState.continuation != lastContinuation ||
        playbackLocal != lastPlaybackLocal || g_pauseAnim != lastPauseAnimation ||
        battleState.generation != lastBattleGeneration ||
        battleState.battle_instance != lastBattleInstance ||
        pauseReason != lastPauseReason) {
        timerTrace("[timer] policy pause=%d reason=%s strategic=%d accepted=%d "
                   "battleKind=%d battleLocal=%d selection=%d continuation=%d anim=%d "
                   "playbackLocal=%d instance=%u generation=%u combatPause=%d "
                   "animationPause=%d running=%d",
                   wantPause, pauseReason, strategicActive, g_turnAccepted,
                   battleKind, battleActive, battleState.selection_open,
                   battleState.continuation, animationActive, playbackLocal,
                   battleState.battle_instance, battleState.generation, g_pauseOn,
                   g_pauseAnim, g_running);
        lastWantPause = wantPause;
        lastStrategic = strategicActive;
        lastBattleKind = battleKind;
        lastBattleActive = battleActive;
        lastAnimation = animationActive;
        lastCombatPause = g_pauseOn;
        lastRunning = g_running;
        lastAccepted = g_turnAccepted;
        lastSelection = battleState.selection_open;
        lastContinuation = battleState.continuation;
        lastPlaybackLocal = playbackLocal;
        lastPauseAnimation = g_pauseAnim;
        lastBattleGeneration = battleState.generation;
        lastBattleInstance = battleState.battle_instance;
        lastPauseReason = pauseReason;
    }
    const int pausedBeforePolicy = g_paused;
    if (wantPause && !g_paused) {
        setPaused(1);
        g_combatPausing = 1;
    } else if (!wantPause && g_combatPausing) {
        if (g_paused)
            setPaused(0);
        g_combatPausing = 0;
    }
    if (g_paused != pausedBeforePolicy) {
        timerTrace("[timer] clock %s (reason=%s strategic=%d battleKind=%d "
                   "battleLocal=%d selection=%d anim=%d playbackLocal=%d "
                   "combatPause=%d animationPause=%d)",
                   g_paused ? "paused" : "resumed", pauseReason, strategicActive,
                   battleKind, battleActive, battleState.selection_open,
                   animationActive, playbackLocal, g_pauseOn, g_pauseAnim);
    }

    // Force timeouts never accrue negative overtime. Freeze the underlying clock at its exact
    // deadline so banking later also observes a zero remainder. The host latches Auto Battle
    // independently; Reset/config/cancel_elapse cannot release an already-forced battle.
    const int currentDurMs = budgetSecForDay(g_curDayBudget) * 1000;
    const bool localPvpSelection = battleKind == 1 && battleActive == 1 &&
                                   battleState.selection_open == 1;
    const bool localPvpPlayback = battleKind == 1 && playbackLocal == 1 && !g_pauseAnim;
    const bool billablePvpPhase = localPvpSelection || localPvpPlayback;
    const bool canExpireNow = battleKind == 0 ||
                              (battleKind == 1 ? billablePvpPhase : battleActive >= 0);
    // The host latch is battle-scoped while the personal bank remains at zero until the next
    // strategic increment. Reassert idempotently if this client is attacked in another PvP battle
    // before that increment; an already-forced current battle is unaffected by config changes.
    if (g_expired && billablePvpPhase && g_autoBattle)
        hostForceAutoBattle();
    if (g_state == 2 && g_running && inGame && !g_expired && canExpireNow) {
        const DWORD effectiveNow = g_paused ? g_pausedAt : now_ms;
        int remaining = currentDurMs + g_extra -
                        static_cast<int>(effectiveNow - g_baseline);
        if (remaining <= 0) {
            int allowance = currentDurMs + g_extra;
            if (allowance < 0)
                allowance = 0;
            g_pausedAt = g_baseline + static_cast<DWORD>(allowance);
            g_paused = 1;
            g_combatPausing = 1;
            g_expired = 1;
            g_elapseFired = 1;
            int autoResult = 0;
            if (battleKind == 1 && g_autoBattle)
                autoResult = hostForceAutoBattle();
            if (g_turnDay)
                hostEndDay();
            timerTrace("[timer] timeout clamped at 00:00 (pvp=%d local=%d auto=%d/%d endDay=%d)",
                       battleKind == 1 ? 1 : 0, battleActive, g_autoBattle,
                       autoResult, g_turnDay);
        }
    }

    state = g_state;
    durMs = currentDurMs; // captured-day budget (matches the bank; no mid-turn jump)
    alwaysVis = g_alwaysVisible;
    paused = g_paused;
    running = g_running;
    baseline = g_baseline;
    pausedAt = g_pausedAt;
    extra = g_extra;
    LeaveCriticalSection(&g_lock);

    // Visible = enabled AND (always-visible OR in a game). Counting = only after a real turn started.
    bool visible = (state != 0) && (alwaysVis || inGame);
    wchar_t text[64] = L"";
    DWORD tc = g_textColor, sc = g_shadowColor;
    if (visible) {
        int v9;
        if (running) {
            const DWORD effNow = paused ? pausedAt : now_ms; // clock frozen while paused
            const int elapsed = (int)(effNow - baseline);
            v9 = (state == 2) ? (durMs + extra - elapsed) : elapsed; // Force: budget + extra - elapsed
            if (state == 2 && v9 < 0)
                v9 = 0;
        } else {
            v9 = 0; // not started yet: show 00:00 until the turn is active
        }
        formatTime(v9, text);
        pickColors(v9, state, paused, &tc, &sc);
    }

    unsigned sig = visible ? 1u : 0u;
    for (const wchar_t* p = text; *p; ++p)
        sig = sig * 131 + (unsigned)*p;
    sig = sig * 131 + tc;
    sig = sig * 131 + sc;
    if (sig != g_sig) {
        g_sig = sig;
        lstrcpynW(g_text, text, 64);
        g_textColor = tc;
        g_shadowColor = sc;
        g_visible = visible;
        g_host->invalidate();
    }
}

extern "C" int __cdecl c4p_draw(C4P_Canvas* canvas)
{
    if (!canvas || !g_font || !g_visible || !g_text[0])
        return 0;
    EnterCriticalSection(&g_lock);
    const int anchorX = g_anchorX;
    const int anchorY = g_anchorY;
    LeaveCriticalSection(&g_lock);
    Bitmap bmp(canvas->width, canvas->height, canvas->stride, PixelFormat32bppARGB,
               (BYTE*)canvas->pixels);
    Graphics g(&bmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    const StringFormat* fmt = StringFormat::GenericTypographic();
    RectF layout(0.0f, 0.0f, (REAL)canvas->width, (REAL)canvas->height);
    RectF bounds;
    g.MeasureString(g_text, -1, g_font, layout, fmt, &bounds);
    // Use one integer extent for both drawing and drag normalization. Mixing the fractional GDI+
    // measure with a rounded hit rectangle produces a visible sub-pixel re-anchor on first move.
    const int textWidth = (int)(bounds.Width + 0.999f);
    const int textHeight = (int)(bounds.Height + 0.999f);
    const REAL x = (canvas->width - textWidth) * (anchorX / 10000.0f);
    const REAL y = (canvas->height - textHeight) * (anchorY / 10000.0f);
    EnterCriticalSection(&g_lock);
    g_canvasWidth = canvas->width;
    g_canvasHeight = canvas->height;
    g_hitLeft = (int)x;
    g_hitTop = (int)y;
    g_hitWidth = textWidth;
    g_hitHeight = textHeight;
    LeaveCriticalSection(&g_lock);
    const REAL w = (REAL)canvas->width, h = (REAL)canvas->height;
    Color shadowCol(g_shadowColor);
    SolidBrush shadowBrush(shadowCol);
    RectF shadowRect(x + 1.0f, y + 2.0f, w, h);
    g.DrawString(g_text, -1, g_font, shadowRect, fmt, &shadowBrush);
    Color textCol(g_textColor);
    SolidBrush textBrush(textCol);
    RectF textRect(x, y, w, h);
    g.DrawString(g_text, -1, g_font, textRect, fmt, &textBrush);
    return 1;
}

// Optional input export. The host forwards physical game-client coordinates while its layered
// overlay remains click-through. This mirrors timer.mod: Ctrl+Alt+LMB captures the point inside the
// rendered clock, then the same point stays under the cursor throughout the drag (no initial jump).
extern "C" int __cdecl c4p_mouse(UINT msg, WPARAM, int x, int y)
{
    if (!g_host)
        return 0;

    if (msg == WM_LBUTTONDOWN) {
        if (GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) >= 0)
            return 0;
        EnterCriticalSection(&g_lock);
        const bool hit = g_visible && g_hitWidth > 0 && x >= g_hitLeft && y >= g_hitTop &&
                         x < g_hitLeft + g_hitWidth && y < g_hitTop + g_hitHeight;
        if (hit) {
            g_dragging = 1;
            g_dragDx = x - g_hitLeft;
            g_dragDy = y - g_hitTop;
        }
        LeaveCriticalSection(&g_lock);
        if (!hit)
            return 0;
        SetCapture(g_host->get_hwnd());
        return 1;
    }

    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) {
        int anchorX = 0, anchorY = 0;
        EnterCriticalSection(&g_lock);
        if (!g_dragging) {
            LeaveCriticalSection(&g_lock);
            return 0;
        }
        const int freeX = g_canvasWidth > g_hitWidth ? g_canvasWidth - g_hitWidth : 0;
        const int freeY = g_canvasHeight > g_hitHeight ? g_canvasHeight - g_hitHeight : 0;
        int left = x - g_dragDx;
        int top = y - g_dragDy;
        if (left < 0) left = 0; else if (left > freeX) left = freeX;
        if (top < 0) top = 0; else if (top > freeY) top = freeY;
        g_anchorX = freeX ? (left * 10000 + freeX / 2) / freeX : 0;
        g_anchorY = freeY ? (top * 10000 + freeY / 2) / freeY : 0;
        g_hitLeft = left;
        g_hitTop = top;
        anchorX = g_anchorX;
        anchorY = g_anchorY;
        if (msg == WM_LBUTTONUP)
            g_dragging = 0;
        LeaveCriticalSection(&g_lock);

        g_host->invalidate();
        if (msg == WM_LBUTTONUP) {
            // Persist only on drop, retaining the old whole-percent keys for config readability.
            g_host->set_config_int(iniSection(), "AnchorX10000", anchorX);
            g_host->set_config_int(iniSection(), "AnchorY10000", anchorY);
            g_host->set_config_int(iniSection(), "AnchorX", (anchorX + 50) / 100);
            g_host->set_config_int(iniSection(), "AnchorY", (anchorY + 50) / 100);
            if (GetCapture() == g_host->get_hwnd())
                ReleaseCapture();
        }
        return 1;
    }

    if (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED) {
        EnterCriticalSection(&g_lock);
        const int wasDragging = g_dragging;
        g_dragging = 0;
        LeaveCriticalSection(&g_lock);
        return wasDragging;
    }
    return 0;
}

extern "C" void __cdecl c4p_shutdown(void)
{
    delete g_font; g_font = nullptr;
    delete g_family; g_family = nullptr;
    delete g_fonts; g_fonts = nullptr;
    if (g_gdiToken) { GdiplusShutdown(g_gdiToken); g_gdiToken = 0; }
}

// ---- the exact legacy menu (resource id 3), built programmatically ----
// Timetable dialog (legacy resource 5): edit the per-day turn budget. Row 0 is the day-1 base
// (TableDuration_0); rows 1..3 are checkable "from day N -> duration D" overrides. OK saves to config +
// applies live; Cancel discards. Modal on the game UI thread (from c4p_command).
INT_PTR CALLBACK timetableDlgProc(HWND h, UINT m, WPARAM w, LPARAM)
{
    switch (m) {
    case WM_INITDIALOG:
        EnterCriticalSection(&g_lock);
        SetDlgItemInt(h, IDC_TT_DUR0, (UINT)g_durBase, FALSE);
        for (int i = 0; i < 3; ++i) {
            CheckDlgButton(h, IDC_TT_ACT1 + i, g_tblActive[i] ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(h, IDC_TT_DAY1 + i, (UINT)g_tblDay[i], FALSE);
            SetDlgItemInt(h, IDC_TT_DUR1 + i, (UINT)g_tblDur[i], FALSE);
            EnableWindow(GetDlgItem(h, IDC_TT_DAY1 + i), g_tblActive[i]);
            EnableWindow(GetDlgItem(h, IDC_TT_DUR1 + i), g_tblActive[i]);
        }
        LeaveCriticalSection(&g_lock);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_TT_ACT1:
        case IDC_TT_ACT2:
        case IDC_TT_ACT3: {
            const int i = (int)LOWORD(w) - IDC_TT_ACT1;
            const BOOL on = IsDlgButtonChecked(h, LOWORD(w)) == BST_CHECKED;
            EnableWindow(GetDlgItem(h, IDC_TT_DAY1 + i), on);
            EnableWindow(GetDlgItem(h, IDC_TT_DUR1 + i), on);
            return TRUE;
        }
        case IDOK: {
            BOOL ok;
            int d0 = (int)GetDlgItemInt(h, IDC_TT_DUR0, &ok, FALSE);
            if (d0 < 1) d0 = 1;
            int act[3], day[3], dur[3];
            for (int i = 0; i < 3; ++i) {
                act[i] = IsDlgButtonChecked(h, IDC_TT_ACT1 + i) == BST_CHECKED ? 1 : 0;
                day[i] = (int)GetDlgItemInt(h, IDC_TT_DAY1 + i, &ok, FALSE);
                int du = (int)GetDlgItemInt(h, IDC_TT_DUR1 + i, &ok, FALSE);
                dur[i] = du < 1 ? 1 : du;
            }
            EnterCriticalSection(&g_lock);
            g_durBase = d0;
            for (int i = 0; i < 3; ++i) { g_tblActive[i] = act[i]; g_tblDay[i] = day[i]; g_tblDur[i] = dur[i]; }
            LeaveCriticalSection(&g_lock);
            char k[24];
            persist("TableDuration_0", d0);
            for (int i = 0; i < 3; ++i) {
                wsprintfA(k, "TableActive_%d", i + 1);   persist(k, act[i]);
                wsprintfA(k, "TableDay_%d", i + 1);       persist(k, day[i]);
                wsprintfA(k, "TableDuration_%d", i + 1);  persist(k, dur[i]);
            }
            if (g_host) g_host->invalidate();
            EndDialog(h, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Set dialog (legacy resource 6): set the current turn's displayed time to N seconds (legacy DialogFunc
// uses 1000*value, and the Timetable budget is in seconds too). Simple sets the count-up elapsed; Force
// sets the count-down remaining within the day's budget and clears extra. Modal on the game UI thread.
INT_PTR CALLBACK setDlgProc(HWND h, UINT m, WPARAM w, LPARAM)
{
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemInt(h, IDC_SET_SEC, 0, FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDOK: {
            BOOL ok;
            int secs = (int)GetDlgItemInt(h, IDC_SET_SEC, &ok, FALSE);
            if (secs < 0) secs = 0;
            const int day = hostDay();
            const DWORD now = GetTickCount();
            EnterCriticalSection(&g_lock);
            if (g_state == 2) {
                const int budgetMs = budgetSecForDay(day) * 1000;
                if (secs * 1000 > budgetMs) secs = budgetMs / 1000; // remaining can't exceed the budget
                g_baseline = now - (DWORD)(budgetMs - secs * 1000); // remaining = secs
                g_extra = 0;
            } else if (g_state == 1) {
                g_baseline = now - (DWORD)(secs * 1000); // elapsed = secs
            } else {
                g_baseline = now;
            }
            g_pausedAt = now;
            g_running = 1; // a set time implies the clock is active
            g_elapseFired = 0;
            g_expired = 0;
            hostCancelElapse();
            LeaveCriticalSection(&g_lock);
            if (g_host) g_host->invalidate();
            EndDialog(h, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

extern "C" HMENU __cdecl c4p_menu(int base_cmd_id)
{
    g_base = base_cmd_id;
    const int b = base_cmd_id;
    g_menu = CreatePopupMenu();

    HMENU simple = CreatePopupMenu();
    AppendMenuA(simple, MF_STRING, b + kSimpleOn, "&Enabled");
    AppendMenuA(simple, MF_SEPARATOR, 0, nullptr);
    HMENU dayStart = CreatePopupMenu();
    AppendMenuA(dayStart, MF_STRING, b + kDayStartPause, "&Pause");
    AppendMenuA(dayStart, MF_STRING, b + kDayStartUnpause, "&Unpause");
    AppendMenuA(dayStart, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(dayStart, MF_STRING, b + kDayStartReset, "&Reset");
    AppendMenuA(simple, MF_POPUP, (UINT_PTR)dayStart, "On Day &Start");
    HMENU dayEnd = CreatePopupMenu();
    AppendMenuA(dayEnd, MF_STRING, b + kDayEndPause, "&Pause");
    AppendMenuA(dayEnd, MF_STRING, b + kDayEndUnpause, "&Unpause");
    AppendMenuA(dayEnd, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(dayEnd, MF_STRING, b + kDayEndReset, "&Reset");
    AppendMenuA(simple, MF_POPUP, (UINT_PTR)dayEnd, "On Day &End");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)simple, "&Simple Mode");

    HMENU force = CreatePopupMenu();
    AppendMenuA(force, MF_STRING, b + kForceOn, "&Enabled");
    AppendMenuA(force, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(force, MF_STRING, b + kAnimPause, "&Animation Pause");
    HMENU combat = CreatePopupMenu();
    AppendMenuA(combat, MF_STRING, b + kCombatOff, "&Off");
    AppendMenuA(combat, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(combat, MF_STRING, b + kCombatPvP, "Player vs &Player");
    AppendMenuA(combat, MF_STRING, b + kCombatPvAny, "Player vs &Any");
    AppendMenuA(force, MF_POPUP, (UINT_PTR)combat, "&Combat Pause");
    AppendMenuA(force, MF_SEPARATOR, 0, nullptr);
    HMENU elapse = CreatePopupMenu();
    AppendMenuA(elapse, MF_STRING, b + kElapseEndDay, "&End Day");
    AppendMenuA(elapse, MF_STRING | MF_GRAYED, b + kElapseDefend, "&Defend");
    AppendMenuA(elapse, MF_STRING | MF_GRAYED, b + kElapseRetreat, "&Retreat");
    AppendMenuA(elapse, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(elapse, MF_STRING, b + kElapseAutoBattle, "&Auto Battle");
    AppendMenuA(force, MF_POPUP, (UINT_PTR)elapse, "On &Elapse");
    AppendMenuA(force, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(force, MF_STRING, b + kResetExtra, "&Reset Extra Time");
    AppendMenuA(force, MF_STRING, b + kTimetable, "&Timetable ...");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)force, "&Force Turn Mode");

    AppendMenuA(g_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(g_menu, MF_STRING, b + kPause, "&Pause\tAlt+P");
    AppendMenuA(g_menu, MF_STRING, b + kReset, "&Reset\tAlt+R");
    AppendMenuA(g_menu, MF_STRING, b + kSet, "&Set ...");
    AppendMenuA(g_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(g_menu, MF_STRING, b + kAlwaysVis, "&Always Visible");
    AppendMenuA(g_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(g_menu, MF_STRING, b + kAbout, "&About...");

    refreshMenu();
    return g_menu;
}

// ---- menu command handling (sub_10004D40): legacy bit/radio/toggle logic ----
extern "C" void __cdecl c4p_command(int cmd)
{
    if (!g_host)
        return;
    const int off = cmd - g_base;
    HWND hwnd = g_host->get_hwnd();

    EnterCriticalSection(&g_lock);
    switch (off) {
    case kSimpleOn:
    case kForceOn: {
        const int target = (off == kForceOn) ? 2 : 1;
        g_state = (g_state == target) ? 0 : target;
        if (g_state) restart();
        else { g_elapseFired = 0; g_expired = 0; hostCancelElapse(); }
        g_paused = 0;
        persist("Enabled", g_state);
        break;
    }
    case kPause:
        if (g_state == 1) setPaused(g_paused ? 0 : 1);
        break;
    case kReset:
        if (g_state == 1 || (g_state == 2 && g_turnAccepted)) {
            restart();
            g_running = 1;
        }
        break;
    case kDayStartPause:   g_dayTurn = (g_dayTurn & 0x3C) | 1;  persist("DayTurn", g_dayTurn); break;
    case kDayStartUnpause: g_dayTurn = (g_dayTurn & 0x3C) | 2;  persist("DayTurn", g_dayTurn); break;
    case kDayStartReset:   g_dayTurn ^= 4;                      persist("DayTurn", g_dayTurn); break;
    case kDayEndPause:     g_dayTurn = (g_dayTurn & 0x27) | 8;  persist("DayTurn", g_dayTurn); break;
    case kDayEndUnpause:   g_dayTurn = (g_dayTurn & 0x27) | 0x10; persist("DayTurn", g_dayTurn); break;
    case kDayEndReset:     g_dayTurn ^= 0x20;                   persist("DayTurn", g_dayTurn); break;
    case kCombatOff:
    case kCombatPvP:
    case kCombatPvAny:     g_pauseOn = off - kCombatOff;        persist("PauseOn", g_pauseOn); break;
    case kAnimPause:       g_pauseAnim = !g_pauseAnim;          persist("PauseAnimation", g_pauseAnim); break;
    case kElapseEndDay:
        g_turnDay = !g_turnDay;
        if (!g_turnDay) { g_elapseFired = 0; hostCancelElapse(); }
        persist("TurnDay", g_turnDay);
        break;
    case kElapseAutoBattle:
        g_autoBattle = !g_autoBattle;
        persist("AutoBattle", g_autoBattle);
        break;
    case kElapseDefend:
    case kElapseRetreat:
        break; // visible placeholders; both are permanently disabled
    case kResetExtra:      g_resetExtra = !g_resetExtra;        persist("ResetExtraTime", g_resetExtra); break;
    case kAlwaysVis:       g_alwaysVisible = !g_alwaysVisible;  persist("AlwaysVisible", g_alwaysVisible); break;
    default:
        break;
    }
    LeaveCriticalSection(&g_lock);

    // Dialogs / message boxes (UI thread, outside the lock).
    if (off == kTimetable) {
        DialogBoxParamW(g_hinst, MAKEINTRESOURCEW(IDD_TIMETABLE), hwnd, timetableDlgProc, 0);
    } else if (off == kAbout) {
        MessageBoxA(hwnd,
                    "C4dll-R\n"
                    "DirectDraw renderer for Disciples II, built on the open-source\n"
                    "cnc-ddraw (https://github.com/FunkyFr3sh/cnc-ddraw).",
                    "About", MB_OK | MB_ICONINFORMATION);
    } else if (off == kSet) {
        DialogBoxParamW(g_hinst, MAKEINTRESOURCEW(IDD_SET), hwnd, setDlgProc, 0);
    }

    refreshMenu();
    g_host->invalidate();
}
