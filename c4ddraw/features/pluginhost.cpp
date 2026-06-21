/*
 * C4dll-R plugin host. DisciplesGL hosted plugins in <game>\mods\ and composited their overlay
 * into the game frame. Replacing DGL with C4dll-R removed that host, so plugins (e.g. the turn
 * timer) stopped working. This module restores hosting inside C4dll-R for BOTH formats:
 *   - the NEW 32bpp-native format (c4plugin.h, "*.c4p"): draws on demand (only when it invalidates);
 *   - the LEGACY DisciplesGL format ("*.mod"): GetName/SetHWND/Launch/DrawFrame, redrawn periodically.
 *
 * Rendering: instead of patching cnc-ddraw's renderer (D2 is 8bpp palettized with an in-shader
 * palette, and there are 3 backends OGL/D3D9/GDI), we composite via a transparent, click-through,
 * topmost LAYERED window sitting over the game's client area and updated with UpdateLayeredWindow.
 * This is renderer-agnostic and pairs with the new format's draw-on-dirty (we only repaint when the
 * overlay actually changes). Plugins draw straight (non-premultiplied) BGRA; the host premultiplies
 * once for the layered-window alpha blend.
 */

#include "c4plugin.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <detours.h>

namespace {

// --- logging (-> OutputDebugString + C4plugins.log next to the exe) --------------------
const char* exeDirFile(const char* leaf)
{
    static char base[MAX_PATH] = {};
    if (!base[0]) {
        GetModuleFileNameA(nullptr, base, sizeof(base));
        char* slash = strrchr(base, '\\');
        if (slash)
            slash[1] = 0;
        else
            base[0] = 0;
    }
    static char path[MAX_PATH];
    lstrcpynA(path, base, sizeof(path));
    lstrcatA(path, leaf);
    return path;
}

void plog(const char* fmt, ...)
{
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 2] = 0;
    size_t n = strlen(buf);
    buf[n] = '\n';
    buf[n + 1] = 0;
    OutputDebugStringA(buf);
    HANDLE h = CreateFileA(exeDirFile("C4plugins.log"), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &w, nullptr);
        CloseHandle(h);
    }
}

// --- game window discovery (the game's main MQ_UIManager window) -----------------------
struct FindCtx
{
    DWORD pid;
    HWND found;
};
BOOL CALLBACK findGameWindow(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == ctx->pid && IsWindowVisible(hwnd)) {
        char cls[64] = {};
        GetClassNameA(hwnd, cls, sizeof(cls));
        if (lstrcmpA(cls, "MQ_UIManager") == 0) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            if ((rc.right - rc.left) >= 640 && (rc.bottom - rc.top) >= 400) {
                ctx->found = hwnd;
                return FALSE;
            }
        }
    }
    return TRUE;
}
HWND g_gameHwnd = nullptr;
HWND gameHwnd()
{
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) {
        FindCtx ctx{GetCurrentProcessId(), nullptr};
        EnumWindows(&findGameWindow, reinterpret_cast<LPARAM>(&ctx));
        g_gameHwnd = ctx.found;
    }
    return g_gameHwnd;
}

// --- host services exposed to new-format plugins ---------------------------------------
volatile LONG g_dirty = 1; // a new plugin asked for a redraw (start dirty for the first frame)

HWND __cdecl host_get_hwnd(void)
{
    return gameHwnd();
}
void __cdecl host_invalidate(void)
{
    InterlockedExchange(&g_dirty, 1);
}
const char* hostConfigPath()
{
    return exeDirFile("C4plugins.ini");
}
const char* __cdecl host_config_path_cb(void)
{
    return hostConfigPath();
}
int __cdecl host_get_config_int(const char* section, const char* key, int def)
{
    return GetPrivateProfileIntA(section, key, def, hostConfigPath());
}
void __cdecl host_set_config_int(const char* section, const char* key, int value)
{
    char b[16];
    wsprintfA(b, "%d", value);
    WritePrivateProfileStringA(section, key, b, hostConfigPath());
}

// --- host-driven turn detection -------------------------------------------------------------------
// The host watches the game's current-turn player and exposes it to new plugins. The timer resets its
// countdown whenever g_turnSerial changes (the host bumps it on every turn change, incl. a skipped
// turn). pollServerTurn() (in the overlay worker) updates these; the plugin reads them on the SAME
// worker thread, so a plain read is fine. featuremenu owns the game-struct walk (SEH-guarded).
extern "C" int featuremenu_server_player(int* outInGame);
extern "C" int featuremenu_in_battle(void);
extern "C" int featuremenu_current_day(void);
extern "C" int timerhost_turn_active(void);
extern "C" int timerhost_is_animating(void);
extern "C" int timerhost_battle_kind(void);
extern "C" int timerhost_turn_player_id(void);
extern "C" int timerhost_retreat(void);
extern "C" int timerhost_end_day(void);

volatile LONG g_turnSerial = 0; // bumped on every detected turn change (including a skipped turn)
int g_turnPlayer = -1;          // current turn player index, -1 if unknown / not in a game
int g_turnMiss = 0;             // consecutive -1 polls (debounce transient/torn reads)
int g_inGame = 0;               // 1 while a turn is in progress

uint32_t __cdecl host_get_turn_serial(void)
{
    return (uint32_t)g_turnSerial;
}
int __cdecl host_get_turn_player(void)
{
    return g_turnPlayer;
}
int __cdecl host_is_in_game(void)
{
    return g_inGame;
}
int __cdecl host_is_in_battle(void)
{
    return featuremenu_in_battle();
}
int __cdecl host_get_day(void)
{
    return featuremenu_current_day();
}
int __cdecl host_turn_active(void) { return timerhost_turn_active(); }
int __cdecl host_is_animating(void) { return timerhost_is_animating(); }
int __cdecl host_battle_kind(void) { return timerhost_battle_kind(); }
int __cdecl host_turn_player_id(void) { return timerhost_turn_player_id(); }
int __cdecl host_retreat(void) { return timerhost_retreat(); }
int __cdecl host_end_day(void) { return timerhost_end_day(); }

C4P_Host g_host = {sizeof(C4P_Host),     host_get_hwnd,        host_invalidate,
                   host_get_config_int,  host_set_config_int,  host_config_path_cb,
                   host_get_turn_serial, host_get_turn_player, host_is_in_game,
                   host_is_in_battle,    host_get_day,
                   host_turn_active,     host_is_animating,    host_battle_kind,
                   host_turn_player_id,  host_retreat,         host_end_day};

// --- plugin records --------------------------------------------------------------------
using ModGetId = const void*(__stdcall*)(); // 12-byte plugin id (DGL used it for dedup)
using ModGetName = const char*(__stdcall*)();
using ModGetMenu = HMENU(__stdcall*)(int baseCmdId); // builds + returns the plugin's config submenu
using ModSetHWND = void(__stdcall*)(HWND);
using ModLaunch = void(__stdcall*)();
// DrawFrame(originX, originY, width, height, stride, formatFlags, scan0). flags bit0=1 -> 32bpp ARGB.
using ModDrawFrame = int(__stdcall*)(int, int, int, int, int, int, void*);
using C4pQuery = int(__cdecl*)(C4P_Info*);
using C4pInit = int(__cdecl*)(const C4P_Host*);
using C4pTick = void(__cdecl*)(uint32_t);
using C4pDraw = int(__cdecl*)(C4P_Canvas*);
using C4pMenu = HMENU(__cdecl*)(int);  // optional: build the plugin's config submenu
using C4pCommand = void(__cdecl*)(int); // optional: handle a menu WM_COMMAND in its id block

struct Plugin
{
    HMODULE mod;
    bool isNew;
    bool hwndSet;
    bool hasId;
    char id[12]; // legacy plugin id (GetId), used to dedup like DGL did
    bool hasSupersede;     // new plugin: declares a legacy id it replaces
    char supersedeId[12];  // the 12-byte legacy GetId this .c4p supersedes
    char name[64];
    UINT menuBase; // the base command id we passed to GetMenu (the plugin's WM_COMMAND base)
    HMENU menu; // legacy plugin's config submenu (from GetMenu), grafted into the bar; null if none
    ModSetHWND setHwnd;
    ModLaunch launch;
    ModDrawFrame drawFrame;
    C4pTick tick;
    C4pDraw draw;
    C4pCommand command; // new-plugin menu command handler (c4p_command), or null
};

Plugin g_plugins[16];
int g_pluginCount = 0;
bool g_hasLegacy = false;
HANDLE g_pluginsReady = nullptr; // manual-reset event: signaled by the worker once plugins are loaded

// --- loading ---------------------------------------------------------------------------
void loadOne(const char* path, const char* fileName, bool isNew)
{
    if (g_pluginCount >= (int)(sizeof(g_plugins) / sizeof(g_plugins[0])))
        return;
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        plog("[plugins] load failed: %s (err %lu)", fileName, GetLastError());
        return;
    }
    Plugin p = {};
    p.mod = m;
    p.isNew = isNew;

    if (isNew) {
        auto query = (C4pQuery)GetProcAddress(m, "c4p_query");
        auto init = (C4pInit)GetProcAddress(m, "c4p_init");
        p.tick = (C4pTick)GetProcAddress(m, "c4p_tick");
        p.draw = (C4pDraw)GetProcAddress(m, "c4p_draw");
        C4P_Info info = {sizeof(C4P_Info), 0, nullptr, nullptr, nullptr};
        if (!query || !init || !p.draw || query(&info) != 1) {
            plog("[plugins] %s: not a valid .c4p (missing exports / query failed)", fileName);
            FreeLibrary(m);
            return;
        }
        if (info.abi_version != C4P_ABI_VERSION) {
            plog("[plugins] %s: ABI %u != host %u; skipped", fileName, info.abi_version,
                 (unsigned)C4P_ABI_VERSION);
            FreeLibrary(m);
            return;
        }
        lstrcpynA(p.name, info.name ? info.name : fileName, sizeof(p.name));
        if (info.supersedes_legacy_id) {
            memcpy(p.supersedeId, info.supersedes_legacy_id, sizeof(p.supersedeId));
            p.hasSupersede = true;
        }
        if (init(&g_host) != 1) {
            plog("[plugins] %s: c4p_init failed", fileName);
            FreeLibrary(m);
            return;
        }
        plog("[plugins] loaded NEW '%s' (%s)", p.name, fileName);
        // Optional config submenu (grafted under the "Plugins" menu by featuremenu) + its command
        // handler. Reserve a 0x100-wide WM_COMMAND id block per plugin, same scheme as legacy.
        p.menuBase = 0xB000 + g_pluginCount * 0x100;
        p.command = (C4pCommand)GetProcAddress(m, "c4p_command");
        if (auto buildPluginMenu = (C4pMenu)GetProcAddress(m, "c4p_menu"))
            p.menu = buildPluginMenu(p.menuBase);
    } else {
        auto getId = (ModGetId)GetProcAddress(m, "GetId");
        auto getName = (ModGetName)GetProcAddress(m, "GetName");
        p.setHwnd = (ModSetHWND)GetProcAddress(m, "SetHWND");
        p.launch = (ModLaunch)GetProcAddress(m, "Launch");
        p.drawFrame = (ModDrawFrame)GetProcAddress(m, "DrawFrame");
        if (!p.drawFrame || !p.launch) {
            plog("[plugins] %s: not a valid .mod (missing DrawFrame/Launch)", fileName);
            FreeLibrary(m);
            return;
        }
        // Dedup by the 12-byte GetId, exactly as the DisciplesGL host did, so the same plugin
        // dropped in twice (e.g. .mod + a renamed copy) is not loaded/hooked twice.
        if (getId) {
            const void* idp = getId();
            if (idp) {
                memcpy(p.id, idp, sizeof(p.id));
                p.hasId = true;
                for (int i = 0; i < g_pluginCount; ++i)
                    if (g_plugins[i].hasId && memcmp(g_plugins[i].id, p.id, sizeof(p.id)) == 0) {
                        plog("[plugins] %s: duplicate id; skipped", fileName);
                        FreeLibrary(m);
                        return;
                    }
            }
        }
        // If a NEW .c4p already loaded declares it replaces this legacy id, drop the legacy one (no
        // double timer): this is how the new format ships "alongside legacy" cleanly - new supersedes
        // old. (.c4p are loaded before .mod, so the superseding plugin is already present.)
        if (p.hasId) {
            for (int i = 0; i < g_pluginCount; ++i)
                if (g_plugins[i].isNew && g_plugins[i].hasSupersede &&
                    memcmp(g_plugins[i].supersedeId, p.id, sizeof(p.id)) == 0) {
                    plog("[plugins] %s: superseded by new '%s'; legacy not loaded", fileName,
                         g_plugins[i].name);
                    FreeLibrary(m);
                    return;
                }
        }
        auto getMenu = (ModGetMenu)GetProcAddress(m, "GetMenu");
        const char* nm = getName ? getName() : nullptr;
        lstrcpynA(p.name, nm ? nm : fileName, sizeof(p.name));
        p.launch();
        // Build the plugin's config submenu (DGL did this); we graft it under our "Plugins" menu.
        // Use a command-id base well clear of featuremenu's 0xA1xx range.
        p.menuBase = 0xB000 + g_pluginCount * 0x100;
        if (getMenu)
            p.menu = getMenu(p.menuBase);
        g_hasLegacy = true;
        plog("[plugins] loaded LEGACY '%s' (%s), menu=%p", p.name, fileName, (void*)p.menu);
    }
    g_plugins[g_pluginCount++] = p;
}

void loadFolder(const char* pattern, bool isNew)
{
    char dir[MAX_PATH];
    lstrcpynA(dir, exeDirFile("mods\\"), sizeof(dir));
    char glob[MAX_PATH];
    lstrcpynA(glob, dir, sizeof(glob));
    lstrcatA(glob, pattern);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char full[MAX_PATH];
        lstrcpynA(full, dir, sizeof(full));
        lstrcatA(full, fd.cFileName);
        loadOne(full, fd.cFileName, isNew);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// --- Path A: drive the legacy turn timer's per-turn RESET, incl. on a SKIPPED turn -------------
// timer.mod resets its countdown on a NORMAL turn change via its own off[7] game hook, but a SKIP
// takes a game path it does not hook -> the next player keeps the previous countdown ("time
// continued"). We drive the reset: detour the timer's per-frame turn callback (RVA 0x1B90 = its
// off[6] handler, which reads the current player from the game every frame, including right after a
// skip) and, when the player index changes, post the timer its RESET menu command (base+3 = the
// same thing the timer does itself on a normal turn). This RVA/layout is specific to the user's
// timer.mod build; it is gated on the plugin being the "Timer" with a valid off_10008000 table.
using TimerTurnFn = unsigned char*(__cdecl*)();
TimerTurnFn g_realTimerTurn = nullptr;
HWND g_timerHwnd = nullptr;
UINT g_timerResetCmd = 0;
int g_lastTurnPlayer = -1;
bool g_timerFixDone = false;

unsigned char* __cdecl timerTurnHook()
{
    unsigned char* obj = g_realTimerTurn(); // run the timer's own callback (tracks per-player time)
    if (obj) {
        const int player = obj[0]; // the turn-controller object's first byte = current player index
        if (g_lastTurnPlayer >= 0 && player != g_lastTurnPlayer && g_timerHwnd)
            PostMessageA(g_timerHwnd, WM_COMMAND, g_timerResetCmd, 0); // reset for the new player
        g_lastTurnPlayer = player;
    }
    return obj;
}

void installTimerTurnFix(HWND gameHwnd)
{
    if (g_timerFixDone)
        return;
    for (int i = 0; i < g_pluginCount; ++i) {
        Plugin& p = g_plugins[i];
        if (p.isNew || lstrcmpiA(p.name, "Timer") != 0)
            continue;
        const uintptr_t base = (uintptr_t)p.mod;
        // sanity: off_10008000 (RVA 0x8000) must hold the matched per-version table whose off[6] is
        // a game code address (the game's preferred base is 0x400000), else this is not the known
        // timer and we must not patch a wrong RVA.
        uint32_t* table = *reinterpret_cast<uint32_t**>(base + 0x8000);
        if (!table)
            continue;
        const uint32_t off6 = table[6];
        if (off6 < 0x401000 || off6 > 0x700000)
            continue;
        g_realTimerTurn = reinterpret_cast<TimerTurnFn>(base + 0x1B90);
        g_timerHwnd = gameHwnd;
        g_timerResetCmd = p.menuBase + 3; // base+3 = the timer's RESET menu command
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&reinterpret_cast<PVOID&>(g_realTimerTurn), timerTurnHook);
        if (DetourTransactionCommit() == NO_ERROR) {
            g_timerFixDone = true;
            plog("[plugins] timer turn-fix installed (reset on every turn change incl. skip; cmd "
                 "%#x, off6 %#x)",
                 g_timerResetCmd, off6);
        } else {
            plog("[plugins] timer turn-fix: detour failed");
        }
        return;
    }
}

// --- transparent layered overlay window + BGRA32 DIB -----------------------------------
HWND g_overlayWnd = nullptr;
HDC g_memDC = nullptr;
HBITMAP g_dibBmp = nullptr;
HBITMAP g_oldBmp = nullptr;
uint8_t* g_dib = nullptr; // BGRA32, top-down
int g_ovW = 0, g_ovH = 0;

// (re)create the DIB surface for size w x h; returns true if it was (re)created.
bool ensureSurface(int w, int h)
{
    if (g_memDC && g_ovW == w && g_ovH == h)
        return false;
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_dibBmp);
        DeleteDC(g_memDC);
        g_memDC = nullptr;
        g_dibBmp = nullptr;
        g_dib = nullptr;
    }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    void* bits = nullptr;
    g_dibBmp = CreateDIBSection(g_memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!g_dibBmp) {
        DeleteDC(g_memDC);
        g_memDC = nullptr;
        return false;
    }
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dibBmp);
    g_dib = (uint8_t*)bits;
    g_ovW = w;
    g_ovH = h;
    return true;
}

HWND createOverlayWindow(HWND owner)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "C4dllROverlay";
        RegisterClassExA(&wc);
        registered = true;
    }
    // Layered (per-pixel alpha) + transparent (click-through) + no-activate, OWNED by the game
    // window. An owned, NON-topmost window is always kept just above its owner in z-order and goes
    // to the background WITH it - so the overlay is glued to the game window, never floats over other
    // apps, and needs no focus logic.
    return CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                           "C4dllROverlay", "", WS_POPUP, 0, 0, 16, 16, owner, nullptr,
                           GetModuleHandleA(nullptr), nullptr);
}

// Draw every plugin into g_dib (cleared transparent), then premultiply for the layered alpha blend.
void rebuildOverlay(int w, int h)
{
    memset(g_dib, 0, (size_t)w * h * 4);
    HWND game = gameHwnd();
    C4P_Canvas canvas = {sizeof(C4P_Canvas), g_dib, w, h, w * 4};
    for (int i = 0; i < g_pluginCount; ++i) {
        Plugin& p = g_plugins[i];
        if (p.isNew) {
            if (p.draw)
                p.draw(&canvas);
        } else {
            if (p.setHwnd && !p.hwndSet && game) {
                p.setHwnd(game);
                p.hwndSet = true;
            }
            p.drawFrame(0, 0, w, h, w * 4, 1, g_dib); // 32bpp ARGB (flags bit0=1), full area
        }
    }
    // Once the legacy plugins have their HWND (subclass active), arm the turn timer's skip fix.
    if (game)
        installTimerTurnFix(game);

    // Diagnostic: C4PLUGINS_TESTMARK=1 draws a visible box, to confirm the overlay composites over
    // the renderer (a turn timer is blank outside an active turn). Off by default.
    if (GetEnvironmentVariableA("C4PLUGINS_TESTMARK", nullptr, 0)) {
        for (int yy = 8; yy < 56 && yy < h; ++yy)
            for (int xx = 8; xx < 178 && xx < w; ++xx) {
                uint8_t* q = g_dib + ((size_t)yy * w + xx) * 4;
                q[0] = 235; q[1] = 110; q[2] = 30; q[3] = 215; // BGRA straight: orange-blue, a=215
            }
    }

    // straight alpha -> premultiplied (UpdateLayeredWindow / ULW_ALPHA expects premultiplied)
    uint8_t* px = g_dib;
    const int count = w * h;
    for (int i = 0; i < count; ++i, px += 4) {
        const unsigned a = px[3];
        if (a == 0) {
            px[0] = px[1] = px[2] = 0;
        } else if (a < 255) {
            px[0] = (uint8_t)(px[0] * a / 255);
            px[1] = (uint8_t)(px[1] * a / 255);
            px[2] = (uint8_t)(px[2] * a / 255);
        }
    }
}

DWORD WINAPI overlayWorker(LPVOID)
{
    // Load plugins HERE, on the worker thread, NOT in DllMain. LoadLibrary of arbitrary plugin DLLs +
    // their c4p_init (which calls GdiplusStartup, owning a background notification thread) + legacy
    // Launch() are all loader-lock-unsafe in DLL_PROCESS_ATTACH (a classic loader-lock deadlock). This
    // runs after DllMain has returned and the lock is released. The menu builder (featuremenu
    // buildMenu) waits on g_pluginsReady before reading the plugin list, since it can no longer rely
    // on the loader lock to serialize loading before the menu thread runs.
    loadFolder("*.c4p", true);  // our native format first
    loadFolder("*.mod", false); // legacy DisciplesGL plugins (minimal compatibility)
    if (g_pluginCount)
        plog("[plugins] %d plugin(s) loaded (legacy present: %d)", g_pluginCount, g_hasLegacy ? 1 : 0);
    if (g_pluginsReady)
        SetEvent(g_pluginsReady);
    if (!g_pluginCount)
        return 0; // nothing to host

    HWND game = nullptr;
    for (int i = 0; i < 800 && !game; ++i) { // up to ~200s for the game window
        game = gameHwnd();
        if (!game)
            Sleep(250);
    }
    if (!game) {
        plog("[plugins] game window not found; overlay not started");
        return 0;
    }
    g_overlayWnd = createOverlayWindow(game);
    if (!g_overlayWnd) {
        plog("[plugins] overlay window creation failed (err %lu)", GetLastError());
        return 0;
    }
    ShowWindow(g_overlayWnd, SW_SHOWNOACTIVATE);
    plog("[plugins] overlay window up; compositing %d plugin(s)", g_pluginCount);

    DWORD lastRebuild = 0;
    bool announced = false;
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, g_overlayWnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        game = gameHwnd();
        if (game) {
            RECT cr{};
            GetClientRect(game, &cr);
            POINT tl{0, 0};
            ClientToScreen(game, &tl);
            int w = cr.right - cr.left;
            int h = cr.bottom - cr.top;
            if (w > 0 && h > 0) {
                bool sizeChanged = ensureSurface(w, h);
                const DWORD now = GetTickCount();
                // Host-driven turn detection: poll the game's current-turn player and bump the turn
                // serial on a change, so new plugins (the timer) reset - including on a skipped turn.
                {
                    int tp = featuremenu_server_player(nullptr);
                    if (tp < 0) {
                        // Do NOT drop the tracked player on a single transient/torn -1 (the
                        // SEH-guarded chain read can momentarily fail off the game thread). Only
                        // clear after a few consecutive misses = genuinely out of game. This avoids
                        // both a spurious mid-turn reset and swallowing a change that straddles a
                        // one-off -1.
                        if (g_turnPlayer >= 0 && ++g_turnMiss >= 4)
                            g_turnPlayer = -1;
                    } else {
                        g_turnMiss = 0;
                        if (tp != g_turnPlayer) {
                            // first observation, re-acquire after out-of-game (a NEW scenario/load),
                            // or a real turn change incl. a skip: bump the serial so the timer
                            // (re)starts its countdown for this turn.
                            g_turnPlayer = tp;
                            InterlockedIncrement(&g_turnSerial);
                        }
                    }
                    g_inGame = (g_turnPlayer >= 0) ? 1 : 0;
                }
                for (int i = 0; i < g_pluginCount; ++i)
                    if (g_plugins[i].isNew && g_plugins[i].tick)
                        g_plugins[i].tick(now);
                const bool dirty = InterlockedExchange(&g_dirty, 0) != 0;
                // legacy plugins redraw periodically (~6 fps is plenty for a clock; it blinks when
                // time runs low, still readable at this rate).
                const bool legacyDue = g_hasLegacy && (now - lastRebuild >= 160);
                if (g_dib && (sizeChanged || dirty || legacyDue)) {
                    rebuildOverlay(w, h);
                    POINT src{0, 0};
                    POINT dst{tl.x, tl.y};
                    SIZE sz{w, h};
                    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    UpdateLayeredWindow(g_overlayWnd, nullptr, &dst, &sz, g_memDC, &src, 0, &bf,
                                        ULW_ALPHA);
                    lastRebuild = now;
                    if (!announced) {
                        plog("[plugins] overlay first paint (%dx%d at %d,%d)", w, h, tl.x, tl.y);
                        announced = true;
                    }
                } else {
                    // follow window moves without repainting the content or changing z-order (the
                    // owned window stays just above the game on its own).
                    SetWindowPos(g_overlayWnd, nullptr, tl.x, tl.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
                }
            }
        }
        Sleep(33);
    }
}

} // namespace

// Called from cnc-ddraw's DllMain (after the embed + featuremenu). Loads plugins and starts the
// overlay compositor thread. Zero cost if there are no plugins.
//
// Lifetime: C4dll-R is the renderer, loaded for the whole process; loaded plugins live until the
// process exits, when the OS reclaims everything. We intentionally do NOT tear plugins down from
// DllMain or atexit - a c4p_shutdown there would run under the loader lock and would have to join the
// overlay worker thread and call GdiplusShutdown (which owns its own thread), both unsafe at process
// termination (deadlock / use-after-teardown), so process-exit cleanup is left to the OS. c4p_shutdown
// is still exercised where it is safe and matters: a plugin whose c4p_init fails calls it itself to
// unwind its GDI+ state before the host FreeLibrary's the module (see timer.cpp c4p_init).
extern "C" void pluginhost_install(void)
{
    // DllMain context: only create the worker thread (CreateThread is loader-lock-safe). The actual
    // plugin loading (LoadLibrary + c4p_init/GdiplusStartup + legacy Launch) happens at the start of
    // overlayWorker, after DllMain returns and the loader lock is released - see the note there.
    g_pluginsReady = CreateEventA(nullptr, TRUE, FALSE, nullptr); // manual-reset; signaled once loaded
    HANDLE t = CreateThread(nullptr, 0, &overlayWorker, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
}

// --- list accessors for the menu bar (featuremenu builds a "Plugins" menu from these) ----
extern "C" int pluginhost_count(void)
{
    return g_pluginCount;
}
extern "C" const char* pluginhost_name(int i)
{
    return (i >= 0 && i < g_pluginCount) ? g_plugins[i].name : "";
}
extern "C" int pluginhost_is_new(int i)
{
    return (i >= 0 && i < g_pluginCount && g_plugins[i].isNew) ? 1 : 0;
}
extern "C" void* pluginhost_menu(int i) // plugin config submenu HMENU (legacy GetMenu / new c4p_menu), or null
{
    return (i >= 0 && i < g_pluginCount) ? g_plugins[i].menu : nullptr;
}

// Route a menu WM_COMMAND to the owning NEW plugin's c4p_command (featuremenu calls this for ids in
// the plugin id block 0xB000+). Returns 1 if a new plugin handled it; 0 otherwise (legacy plugin ids
// reach the legacy plugin via the WndProc forward chain, so the caller passes those through).
extern "C" int pluginhost_command(unsigned id)
{
    for (int i = 0; i < g_pluginCount; ++i) {
        Plugin& p = g_plugins[i];
        if (p.isNew && p.command && id >= p.menuBase && id < p.menuBase + 0x100) {
            p.command((int)id);
            return 1;
        }
    }
    return 0;
}

// Block until the worker has finished loading plugins (or the timeout elapses), so featuremenu's
// buildMenu reads a complete plugin list. Needed because loading moved off the loader-lock-serialized
// DllMain onto the worker thread: without this the menu could be built before plugins finish loading.
extern "C" void pluginhost_wait_ready(unsigned ms)
{
    if (g_pluginsReady)
        WaitForSingleObject(g_pluginsReady, ms);
}
