/*
 * timer.c4p - native (C4dll-R plugin API v2) reconstruction of the legacy turn-timer mod
 * (Mods\timer.mod). Rebuilt from a full decompile of timer.mod so the MENU and behaviour match the
 * original 1-to-1. The fragile per-version game hooks live in the HOST (C4dll-R), not here: the host
 * drives turn detection (host->get_turn_serial) and - for the advanced Force-Turn behaviours that need
 * game events (combat pause, on-elapse auto end-day/retreat, day boundaries) - will expose those via
 * C4P_Host. This file owns the exact menu, all config + persistence, the menu check/enable refresh,
 * the count-up / count-down clock with pause, and the GDI+ draw.
 *
 * Menu (legacy resource id 3, command ids base+1..base+0x16):
 *   Simple Mode { Enabled; On Day Start{Pause/Unpause/Reset}; On Day End{Pause/Unpause/Reset} }
 *   Force Turn Mode { Enabled; Animation Pause; Combat Pause{Off/PvP/PvAny}; On Elapse{End Day/Retreat};
 *                     Reset Extra Time; Timetable... }
 *   Pause(Alt+P); Reset(Alt+R); Set...; Always Visible; About...
 */

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include "../../features/c4plugin.h"
#include "timer_dlg.h"
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

// ---- config (mirrors timer.mod's globals + Disciple.ini [TIMER] keys) ----
int g_state = 0;        // Enabled: 0 = off, 1 = Simple Mode (count up), 2 = Force Turn Mode (count down)
int g_dayTurn = 10;     // bitmask: b0/1/2 = On Day Start Pause/Unpause/Reset; b3/4/5 = On Day End ...
int g_pauseOn = 0;      // Combat Pause: 0 = Off, 1 = Player vs Player, 2 = Player vs Any
int g_pauseAnim = 1;    // Animation Pause
int g_turnDay = 1;      // On Elapse -> End Day
int g_retreat = 1;      // On Elapse -> Retreat
int g_elapseFired = 0;  // latch: the on-elapse action fired this turn (re-armed when the clock is positive)
int g_resetExtra = 0;   // Reset Extra Time
int g_alwaysVisible = 1;
int g_durBase = 300;    // TableDuration_0 (seconds) - the per-turn budget in Force Turn Mode (day-1 base)
// Timetable: up to 3 per-day overrides. From day TableDay_i (when active), the budget = TableDuration_i.
int g_tblActive[3] = {0, 0, 0};
int g_tblDay[3] = {0, 0, 0};
int g_tblDur[3] = {300, 300, 300};
int g_anchorX = 0;      // 0..100 position in free space (0 = left/top)
int g_anchorY = 0;
REAL g_fontSize = 28.0f;

// ---- runtime clock ----
int g_paused = 0;       // manual pause (Pause / Alt+P) OR combat-pause (both freeze the clock)
int g_combatPausing = 0; // 1 if WE auto-paused for Combat Pause (so we only auto-resume our own pause)
int g_running = 0;      // a real turn has started (active) -> the clock counts. Like timer.mod's
                        // dword_10008050: the legacy countdown does NOT run until the turn actually
                        // begins (you reach the strategic view), NOT from launch / the main menu.
DWORD g_baseline = 0;   // tick the current count started from
DWORD g_pausedAt = 0;   // tick we paused at (clock frozen here while paused)
int g_extra = 0;        // the CURRENT player's carried extra time (ms), added to the Force budget
int g_bank[16] = {0};   // per-player Force time bank (ms); Fischer-increment (legacy dword_10008070..88)
int g_lastPlayer = -1;  // player whose turn it was last (to bank their remaining on turn change)
int g_curDayBudget = 0; // the day captured at the current turn-start (for the previous turn's budget)
uint32_t g_lastSerial = 0; // last host turn serial seen (a change while in-game = a turn (re)started)

// Guards config + clock shared between c4p_command (game UI thread) and c4p_tick (worker thread).
CRITICAL_SECTION g_lock;

// ---- menu ----
HMENU g_menu = nullptr; // our top-level popup (grafted under Plugins -> Native -> Timer)
int g_base = 0;         // base command id the host gave us (our 0x100 block)
// command offsets, identical to the legacy resource ids
enum {
    kSimpleOn = 1, kPause = 2, kReset = 3, kSet = 4,
    kDayStartPause = 5, kDayStartUnpause = 6, kDayStartReset = 7,
    kDayEndPause = 8, kDayEndUnpause = 9, kDayEndReset = 10,
    kForceOn = 0xB, kCombatOff = 0xC, kCombatPvP = 0xD, kCombatPvAny = 0xE, kAnimPause = 0xF,
    // tail IDs = legacy timer.mod resource-3 command offsets (decompiled sub_10004D40 WndProc):
    // +0x13 Timetable (Day/Duration grid, res 5), +0x15 Help (msgbox), +0x16 About (res 4)
    kElapseEndDay = 0x10, kElapseRetreat = 0x11, kResetExtra = 0x12, kTimetable = 0x13,
    kAlwaysVis = 0x14, kHelp = 0x15, kAbout = 0x16
};

// the rendered frame + a change signature (avoid pointless redraws)
wchar_t g_text[64] = L"";
DWORD g_textColor = 0xFFCC9900;
DWORD g_shadowColor = 0xFF660000;
bool g_visible = false;
unsigned g_sig = 0;

// timer.mod's exact GetId (12 bytes) - so the host drops the legacy .mod when this plugin is present.
const uint8_t kLegacyId[12] = {0xd5, 0x0e, 0x9b, 0xfd, 0xc8, 0x89, 0x67, 0x49, 0x8c, 0x31, 0xac, 0x64};

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

// Host battle state for Combat Pause. Guarded by struct_size so an older host (without the appended
// is_in_battle callback) is handled gracefully (returns 0 = never combat-pause).
int hostInBattle()
{
    return (g_host && g_host->struct_size >= sizeof(C4P_Host) && g_host->is_in_battle)
               ? g_host->is_in_battle()
               : 0;
}

// Current scenario day from the host (for the Timetable); -1 if unavailable. struct_size-guarded.
int hostDay()
{
    return (g_host && g_host->struct_size >= sizeof(C4P_Host) && g_host->get_day)
               ? g_host->get_day()
               : -1;
}

// Host keystone signals (struct_size-guarded). is_animating: a battle attack animation is playing.
// battle_kind: 0 none, 1 PvP (human-vs-human), 2 any combat.
int hostAnimating()
{
    return (g_host && g_host->struct_size >= sizeof(C4P_Host) && g_host->is_animating)
               ? g_host->is_animating()
               : 0;
}
// On-Elapse actions: queue an auto End-Day / Retreat in the host (the host presses on the game thread).
void hostEndDay()
{
    if (g_host && g_host->struct_size >= sizeof(C4P_Host) && g_host->end_day)
        g_host->end_day();
}
void hostRetreat()
{
    if (g_host && g_host->struct_size >= sizeof(C4P_Host) && g_host->retreat)
        g_host->retreat();
}
int hostBattleKind()
{
    return (g_host && g_host->struct_size >= sizeof(C4P_Host) && g_host->battle_kind)
               ? g_host->battle_kind()
               : 0;
}

// Per-turn budget (seconds) for a scenario day: the day-1 base (TableDuration_0) until the first
// active Timetable entry, then the duration of the active entry with the largest TableDay <= day.
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
    g_state = g_host->get_config_int(iniSection(), "Enabled", 0);
    if (g_state < 0 || g_state > 2) g_state = 0;
    { const int dt = g_host->get_config_int(iniSection(), "DayTurn", 10);
      g_dayTurn = ((unsigned)dt <= 0x3Fu) ? dt : 10; } // legacy: reset to default 0x0A if out of range
    g_pauseOn = g_host->get_config_int(iniSection(), "PauseOn", 0);
    if (g_pauseOn < 0 || g_pauseOn > 2) g_pauseOn = 0;
    g_pauseAnim = g_host->get_config_int(iniSection(), "PauseAnimation", 1) ? 1 : 0;
    g_turnDay = g_host->get_config_int(iniSection(), "TurnDay", 1) ? 1 : 0;
    g_retreat = g_host->get_config_int(iniSection(), "Retreat", 1) ? 1 : 0;
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
    g_anchorX = g_host->get_config_int(iniSection(), "AnchorX", 0);
    g_anchorY = g_host->get_config_int(iniSection(), "AnchorY", 0);
    if (g_anchorX < 0) g_anchorX = 0; if (g_anchorX > 100) g_anchorX = 100;
    if (g_anchorY < 0) g_anchorY = 0; if (g_anchorY > 100) g_anchorY = 100;
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

// ---- menu check/enable refresh (sub_10004010 + sub_10003B20, case 0) ----
void chk(int off, bool on) { if (g_menu) CheckMenuItem(g_menu, g_base + off, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); }
void ena(int off, bool on) { if (g_menu) EnableMenuItem(g_menu, g_base + off, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); }

void refreshMenu()
{
    if (!g_menu)
        return;
    // legacy greys Pause/Reset/Set in Force mode until a real strategic turn is active (the ACTIVE
    // flag = our g_running). Simple mode is always active when enabled.
    const bool active = (g_state == 1) || (g_state == 2 && g_running != 0);
    // mode enables
    chk(kSimpleOn, g_state == 1);
    chk(kForceOn, g_state == 2);
    ena(kPause, active);
    chk(kPause, g_paused != 0);
    ena(kReset, active);
    ena(kSet, active);
    // On Day Start (b0 Pause radio / b1 Unpause radio / b2 Reset toggle)
    chk(kDayStartPause, (g_dayTurn & 1) != 0);
    chk(kDayStartUnpause, (g_dayTurn & 2) != 0);
    chk(kDayStartReset, (g_dayTurn & 4) != 0);
    // On Day End (b3 / b4 / b5)
    chk(kDayEndPause, (g_dayTurn & 8) != 0);
    chk(kDayEndUnpause, (g_dayTurn & 0x10) != 0);
    chk(kDayEndReset, (g_dayTurn & 0x20) != 0);
    // Combat Pause radio
    chk(kCombatOff, g_pauseOn == 0);
    chk(kCombatPvP, g_pauseOn == 1);
    chk(kCombatPvAny, g_pauseOn == 2);
    chk(kAnimPause, g_pauseAnim != 0);
    chk(kElapseEndDay, g_turnDay != 0);
    ena(kElapseRetreat, g_turnDay != 0);
    chk(kElapseRetreat, g_turnDay && g_retreat);
    chk(kResetExtra, g_resetExtra != 0);
    chk(kAlwaysVis, g_alwaysVisible != 0);
}

// ---- pause helpers (freeze/resume the clock by adjusting baseline, like timer.mod) ----
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

void restart() { g_baseline = GetTickCount(); g_pausedAt = g_baseline; g_extra = 0; }

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
    out->supersedes_legacy_id = kLegacyId;
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
    int state, durMs, alwaysVis, paused, running, extra;
    DWORD baseline, pausedAt;
    EnterCriticalSection(&g_lock);
    const uint32_t serial = g_host->get_turn_serial();
    if (!inGame) {
        // At the main menu / between games the timer does NOT run (no self-start). Keep g_lastSerial
        // synced so the first real turn-start inside a game is detected as a change.
        g_running = 0;
        g_lastSerial = serial;
    } else if (serial != g_lastSerial) {
        // A real turn (re)started. Mirror the legacy off[6]/off[7] turn handlers: Force mode is a
        // Fischer time-bank (each turn +budget, unused carries per player); Simple mode is a count-up
        // stopwatch with optional On-Day pause/unpause/reset bits.
        const int newPlayer = g_host->get_turn_player();
        const int firstTurn = !g_running; // 0->1 transition = the first real turn of this game
        g_lastSerial = serial;
        if (firstTurn) {
            for (int i = 0; i < 16; ++i) g_bank[i] = 0; // fresh game
            g_extra = 0;
            g_baseline = now_ms;
            g_pausedAt = now_ms;
            g_curDayBudget = dayNow;
        } else if (g_state == 2) {
            // Force: bank the PREVIOUS player's remaining (budget + their extra - elapsed), then load
            // the NEW player's bank and start a fresh budget. ResetExtraTime drops the carry.
            if (g_lastPlayer >= 0 && g_lastPlayer < 16) {
                const int elapsedPrev = (int)(now_ms - g_baseline);
                const int budgetPrev = budgetSecForDay(g_curDayBudget) * 1000;
                g_bank[g_lastPlayer] = g_resetExtra ? 0 : (budgetPrev + g_extra - elapsedPrev);
            }
            g_curDayBudget = dayNow;
            g_extra = (newPlayer >= 0 && newPlayer < 16) ? g_bank[newPlayer] : 0;
            g_baseline = now_ms;
            g_pausedAt = now_ms;
        } else if (g_state == 1) {
            // Simple (count-up): apply On-Day-End then On-Day-Start DayTurn bits; otherwise keep
            // counting across turns (the legacy does not reset the stopwatch unless a Reset bit fires).
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

    // Force-mode auto-pause: combine Animation Pause (PauseAnimation while in a battle / attack
    // animation) with Combat Pause (PauseOn: PvP-only vs any combat), mirroring the legacy
    // sub_100034A0. battle_kind/is_animating come from the host keystone capture (off[9] BTN_DEFEND).
    // Routed through setPaused so it composes with a manual pause without double-counting the frozen
    // span; g_combatPausing remembers whether WE auto-paused so we never clear a pause the user set.
    int wantPause = 0;
    if (g_running && g_state == 2) {
        const int kind = hostBattleKind();
        if (g_pauseAnim && (hostInBattle() || hostAnimating()))
            wantPause = 1;
        else if (g_pauseOn == 1)
            wantPause = (kind == 1) ? 1 : 0; // PvP only
        else if (g_pauseOn == 2)
            wantPause = (kind != 0) ? 1 : 0; // any combat
    }
    if (wantPause && !g_paused) {
        setPaused(1);
        g_combatPausing = 1;
    } else if (!wantPause && g_combatPausing) {
        if (g_paused)
            setPaused(0);
        g_combatPausing = 0;
    }

    state = g_state;
    durMs = budgetSecForDay(dayNow) * 1000; // Timetable: per-day budget (base until the first active entry)
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
            v9 = (state == 2) ? (durMs + extra - elapsed) : elapsed; // Force: budget + carried extra - elapsed
        } else {
            v9 = 0; // not started yet: timer.mod shows 00:00 until the turn is active
        }
        formatTime(v9, text);
        pickColors(v9, state, paused, &tc, &sc);
        // On Elapse: when a Force-mode turn's time runs out, fire the queued game action ONCE per turn.
        // Re-armed automatically as soon as the clock is positive again (turn change / Set / Reset).
        if (v9 >= 0) {
            g_elapseFired = 0;
        } else if (state == 2 && running && inGame && !g_elapseFired) {
            g_elapseFired = 1;
            if (g_turnDay) {
                if (hostInBattle()) { if (g_retreat) hostRetreat(); }
                else hostEndDay();
            }
        }
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
    Bitmap bmp(canvas->width, canvas->height, canvas->stride, PixelFormat32bppARGB,
               (BYTE*)canvas->pixels);
    Graphics g(&bmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    const StringFormat* fmt = StringFormat::GenericTypographic();
    RectF layout(0.0f, 0.0f, (REAL)canvas->width, (REAL)canvas->height);
    RectF bounds;
    g.MeasureString(g_text, -1, g_font, layout, fmt, &bounds);
    const REAL x = (canvas->width - bounds.Width) * (g_anchorX / 100.0f);
    const REAL y = (canvas->height - bounds.Height) * (g_anchorY / 100.0f);
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

extern "C" void __cdecl c4p_shutdown(void)
{
    delete g_font; g_font = nullptr;
    delete g_family; g_family = nullptr;
    delete g_fonts; g_fonts = nullptr;
    if (g_gdiToken) { GdiplusShutdown(g_gdiToken); g_gdiToken = 0; }
}

// ---- the exact legacy menu (resource id 3), built programmatically ----
// Timetable dialog (port of legacy resource 5 / sub_100044E0): edit the per-day turn budget. Row 0 is
// the day-1 base (TableDuration_0); rows 1..3 are checkable "from day N -> duration D" overrides. A
// checkbox enables/greys its row. OK saves to config + applies live; Cancel discards. Runs modal on
// the game UI thread (from c4p_command).
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

// Set dialog (port of legacy resource 6 / DialogFunc): set the current turn's displayed time to N
// minutes. Simple mode sets the count-up elapsed; Force mode sets the count-down remaining within the
// current day's budget and clears the carried extra. Modal on the game UI thread (from c4p_command).
INT_PTR CALLBACK setDlgProc(HWND h, UINT m, WPARAM w, LPARAM)
{
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemInt(h, IDC_SET_MIN, 0, FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDOK: {
            BOOL ok;
            int mins = (int)GetDlgItemInt(h, IDC_SET_MIN, &ok, FALSE);
            if (mins < 0) mins = 0;
            const int day = hostDay();
            const DWORD now = GetTickCount();
            EnterCriticalSection(&g_lock);
            if (g_state == 2) {
                const int budgetMs = budgetSecForDay(day) * 1000;
                g_baseline = now - (DWORD)(budgetMs - mins * 1000); // remaining = mins
                g_extra = 0;
            } else if (g_state == 1) {
                g_baseline = now - (DWORD)(mins * 1000); // elapsed = mins
            } else {
                g_baseline = now;
            }
            g_pausedAt = now;
            g_running = 1; // a set time implies the clock is active
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
    AppendMenuA(elapse, MF_STRING, b + kElapseRetreat, "&Retreat");
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

// ---- menu command handling (sub_10004D40), faithful to the legacy bit/radio/toggle logic ----
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
        g_paused = 0;
        persist("Enabled", g_state);
        break;
    }
    case kPause:
        if (g_state) setPaused(g_paused ? 0 : 1);
        break;
    case kReset:
        if (g_state) { restart(); g_running = 1; } // explicit (re)start from now
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
    case kElapseEndDay:    g_turnDay = !g_turnDay;              persist("TurnDay", g_turnDay); break;
    case kElapseRetreat:   g_retreat = !g_retreat;             persist("Retreat", g_retreat); break;
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
