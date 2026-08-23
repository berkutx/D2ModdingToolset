/*
 * C4dll-R native plugin host: loads mods\*.c4p (BGRA32) plugins. Renderer-agnostic: publishes a
 * premultiplied canvas which the presentation compositor blends into the game frame.
 */

#include "c4plugin.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// logging -> OutputDebugString + C4plugins.log next to the exe
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

// Shared diagnostics gate (featuremenu.cpp: [menu] debugLog / C4DLL_DEBUG; OFF by default).
extern "C" int featuremenu_debug_enabled(void);

void plog(const char* fmt, ...)
{
    if (!featuremenu_debug_enabled()) // release stays silent: no C4plugins.log unless asked
        return;
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

// find the game's main MQ_UIManager window
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

// host services exposed to new-format plugins
volatile LONG g_dirty = 1; // a plugin asked for a redraw (start dirty for first frame)

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

// host-driven turn detection. g_turnSerial/g_turnPlayer are WRITTEN on the game thread (off[6]
// turn-info hook) and READ on the overlay worker (the plugin), so both are volatile and published
// together (player first, then the InterlockedIncrement serial). featuremenu owns the game-struct walk.
extern "C" int featuremenu_server_player(int* outInGame);
extern "C" int featuremenu_in_battle(void);
extern "C" int featuremenu_current_day(void);
extern "C" void featuremenu_refresh_day(void); // recompute the cached day on the game thread
extern "C" int timerhost_turn_active(void);
extern "C" int timerhost_is_animating(void);
extern "C" int timerhost_battle_kind(void);
extern "C" int timerhost_turn_player_id(void);
extern "C" int timerhost_retreat(void);
extern "C" int timerhost_end_day(void);
extern "C" int timerhost_cancel_elapse(void);
extern "C" uint32_t timerhost_begin_turn_ack_serial(void);
extern "C" int timerhost_battle_turn_active(void);
extern "C" int timerhost_force_auto_battle(void);
extern "C" int timerhost_get_battle_timer_state(C4P_BattleTimerState* out);
extern "C" BOOL DDGetPhysicalCursorPos(POINT* point);
extern "C" BOOL DDPhysicalScreenToClient(HWND hwnd, POINT* point);
extern "C" BOOL DDGetPhysicalClientRect(HWND hwnd, RECT* rect);
extern "C" int DDGetGameWidth(void);
extern "C" int DDGetGameHeight(void);
extern "C" int DDGetScaleMetrics(int* gameWidth, int* gameHeight, int* outputWidth,
                                   int* outputHeight, int* viewportX, int* viewportY,
                                   int* viewportWidth, int* viewportHeight);
extern "C" void DDApplySimpleZoomMouse(int* x, int* y, int gameWidth, int gameHeight);
extern "C" void DDApplySimpleZoomViewport(
    int bottomOrigin, int* x, int* y, int* width, int* height);
extern "C" void DDInvalidatePluginFrame(void);

volatile LONG g_turnSerial = 0; // bumped on every detected turn change (including a skip)
volatile LONG g_turnPlayer = -1; // current turn player index, -1 if unknown / not in a game (cross-thread)
int g_turnMiss = 0;             // consecutive -1 polls (debounce torn reads)
int g_inGame = 0;               // 1 while a turn is in progress (server poll; reliable on HOST)
volatile LONG g_hasServer = 0;  // 1 once in-process server seen -> trust g_inGame over off[6] (written
                                // on the worker poll AND the game thread via pluginhost_turn_reset)
volatile LONG g_menuLoopActive = 0; // game thread publishes native menu tracking to the overlay worker

uint32_t __cdecl host_get_turn_serial(void)
{
    return (uint32_t)g_turnSerial;
}
int __cdecl host_get_turn_player(void)
{
    return (int)g_turnPlayer;
}
volatile LONG g_inGameOff6 = 0; // set by off[6] turn-info hook (fires on BOTH MP clients); cleared on scenario change

// timerhost detours the off[6] turn-info handler (CCmdTurnInfoMsg, sub_48A680), a NETWORK message
// processed on every client, and calls these - so a pure client (MP joiner, no in-process server)
// still sees turn-starts and "in a game".
extern "C" void pluginhost_bump_turn(int player)
{
    featuremenu_refresh_day(); // off[6] runs on the game thread: refresh the cached day so it is current
                               // the instant the serial bumps (the worker rebanks the budget on serial)
    g_inGameOff6 = 1;
    g_turnPlayer = player; // publish the player BEFORE the serial bump (the worker keys off the serial)
    InterlockedIncrement(&g_turnSerial);
}
extern "C" void pluginhost_turn_reset(void)
{
    g_inGameOff6 = 0;
    g_turnPlayer = -1;
    g_hasServer = 0; // new scenario/game: re-decide host-vs-client so a joined client (no in-process
                     // server) does not keep trusting a stale g_inGame from a previously-hosted game
}

int __cdecl host_is_in_game(void)
{
    // HOST -> trust g_inGame (drops reliably on leaving). CLIENT -> off[6] flag (cleared by off[5]).
    // The split keeps the HOST from staying "in game" at the menu if off[5] is missed.
    return (g_hasServer ? g_inGame : g_inGameOff6) ? 1 : 0;
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
int __cdecl host_cancel_elapse(void) { return timerhost_cancel_elapse(); }
uint32_t __cdecl host_begin_turn_ack_serial(void)
{
    return timerhost_begin_turn_ack_serial();
}
int __cdecl host_battle_turn_active(void) { return timerhost_battle_turn_active(); }
int __cdecl host_force_auto_battle(void) { return timerhost_force_auto_battle(); }
int __cdecl host_get_battle_timer_state(C4P_BattleTimerState* out)
{
    return timerhost_get_battle_timer_state(out);
}
int __cdecl host_server_role(void)
{
    if (InterlockedCompareExchange(&g_hasServer, 0, 0) != 0)
        return 1;

    int inGame = 0;
    if (featuremenu_server_player(&inGame) >= 0) {
        InterlockedExchange(&g_hasServer, 1);
        return 1;
    }

    // off[6] is delivered on both multiplayer participants. Seeing it while the exact server chain
    // is absent distinguishes a pure joiner from the pre-scenario/unknown state.
    return InterlockedCompareExchange(&g_inGameOff6, 0, 0) != 0 ? 0 : -1;
}

C4P_Host g_host = {sizeof(C4P_Host),     host_get_hwnd,        host_invalidate,
                   host_get_config_int,  host_set_config_int,  host_config_path_cb,
                   host_get_turn_serial, host_get_turn_player, host_is_in_game,
                   host_is_in_battle,    host_get_day,
                   host_turn_active,     host_is_animating,    host_battle_kind,
                   host_turn_player_id,  host_retreat,         host_end_day,
                   host_cancel_elapse,   host_begin_turn_ack_serial,
                   host_battle_turn_active, host_force_auto_battle,
                   host_get_battle_timer_state, host_server_role};

// plugin records
using C4pQuery = int(__cdecl*)(C4P_Info*);
using C4pInit = int(__cdecl*)(const C4P_Host*);
using C4pTick = void(__cdecl*)(uint32_t);
using C4pDraw = int(__cdecl*)(C4P_Canvas*);
using C4pMenu = HMENU(__cdecl*)(int);  // optional: build the plugin's config submenu
using C4pCommand = void(__cdecl*)(int); // optional: handle a menu WM_COMMAND in its id block
using C4pMouse = int(__cdecl*)(UINT, WPARAM, int, int); // optional: logical game-space input
using C4pRefreshMenu = void(__cdecl*)(void); // optional: update live menu checks/enabled state
using C4pKey = int(__cdecl*)(UINT, WPARAM, LPARAM); // optional: wrapper-level keyboard shortcut

struct Plugin
{
    HMODULE mod;
    char id[64];
    char name[64];
    UINT menuBase; // base command id passed to c4p_menu (plugin's WM_COMMAND base)
    HMENU menu; // plugin's config submenu, grafted into the bar; null if none
    C4pTick tick;
    C4pDraw draw;
    C4pCommand command; // new-plugin menu command handler (c4p_command), or null
    C4pMouse mouse; // optional click-through overlay interaction (c4p_mouse), or null
    C4pRefreshMenu refreshMenu; // optional live menu refresh (c4p_refresh_menu), or null
    C4pKey key; // optional keyboard handler (c4p_key), or null
};

Plugin g_plugins[16];
int g_pluginCount = 0;
HANDLE g_pluginsReady = nullptr; // manual-reset event: signaled by the worker once plugins loaded

// loading
void loadOne(const char* path, const char* fileName)
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
    if (!info.id || !*info.id) {
        plog("[plugins] %s: empty native plugin id; skipped", fileName);
        FreeLibrary(m);
        return;
    }
    for (int i = 0; i < g_pluginCount; ++i) {
        if (!lstrcmpiA(g_plugins[i].id, info.id)) {
            plog("[plugins] %s: duplicate native id '%s'; skipped", fileName, info.id);
            FreeLibrary(m);
            return;
        }
    }
    lstrcpynA(p.id, info.id, sizeof(p.id));
    lstrcpynA(p.name, info.name ? info.name : fileName, sizeof(p.name));
    if (init(&g_host) != 1) {
        plog("[plugins] %s: c4p_init failed", fileName);
        FreeLibrary(m);
        return;
    }
    plog("[plugins] loaded '%s' (%s)", p.name, fileName);
    // Optional config submenu (grafted under "Plugins" by featuremenu) + command handler.
    p.menuBase = 0xB000 + g_pluginCount * 0x100;
    p.command = (C4pCommand)GetProcAddress(m, "c4p_command");
    p.mouse = (C4pMouse)GetProcAddress(m, "c4p_mouse");
    p.refreshMenu = (C4pRefreshMenu)GetProcAddress(m, "c4p_refresh_menu");
    p.key = (C4pKey)GetProcAddress(m, "c4p_key");
    if (auto buildPluginMenu = (C4pMenu)GetProcAddress(m, "c4p_menu"))
        p.menu = buildPluginMenu(p.menuBase);
    g_plugins[g_pluginCount++] = p;
}

void loadFolder(const char* pattern)
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
        loadOne(full, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// BGRA32 plugin canvas. The worker is the sole writer; renderer/screenshot threads take the shared
// side of g_frameLock while converting the premultiplied pixels into the presented 16/32-bit frame.
SRWLOCK g_frameLock = SRWLOCK_INIT;
HDC g_memDC = nullptr;
HBITMAP g_dibBmp = nullptr;
HBITMAP g_oldBmp = nullptr;
uint8_t* g_dib = nullptr; // BGRA32, top-down
int g_ovW = 0, g_ovH = 0;
volatile LONG g_frameHasPixels = 0;
int g_frameLeft = 0, g_frameTop = 0, g_frameRight = 0, g_frameBottom = 0;

// (re)create the DIB surface for w x h; returns true if (re)created
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

// Draw every plugin into g_dib (cleared transparent), then premultiply for source-over blending.
// Return whether the resulting canvas contains at least one visible pixel.
bool rebuildOverlay(int w, int h)
{
    memset(g_dib, 0, (size_t)w * h * 4);
    C4P_Canvas canvas = {sizeof(C4P_Canvas), g_dib, w, h, w * 4};
    for (int i = 0; i < g_pluginCount; ++i) {
        Plugin& p = g_plugins[i];
        if (p.draw)
            p.draw(&canvas);
    }

    // diagnostic: C4PLUGINS_TESTMARK=1 draws a visible box to confirm the overlay composites
    if (GetEnvironmentVariableA("C4PLUGINS_TESTMARK", nullptr, 0)) {
        for (int yy = 8; yy < 56 && yy < h; ++yy)
            for (int xx = 8; xx < 178 && xx < w; ++xx) {
                uint8_t* q = g_dib + ((size_t)yy * w + xx) * 4;
                q[0] = 235; q[1] = 110; q[2] = 30; q[3] = 215; // BGRA straight: orange-blue, a=215
            }
    }

    // Straight alpha -> premultiplied for the renderer-native source-over compositor.
    bool anyPixels = false;
    int left = w, top = h, right = 0, bottom = 0;
    for (int y = 0; y < h; ++y) {
        uint8_t* px = g_dib + static_cast<size_t>(y) * w * 4u;
        for (int x = 0; x < w; ++x, px += 4) {
            const unsigned a = px[3];
            if (a != 0) {
                anyPixels = true;
                if (x < left) left = x;
                if (y < top) top = y;
                if (x + 1 > right) right = x + 1;
                if (y + 1 > bottom) bottom = y + 1;
            }
            if (a == 0) {
                px[0] = px[1] = px[2] = 0;
            } else if (a < 255) {
                px[0] = (uint8_t)(px[0] * a / 255);
                px[1] = (uint8_t)(px[1] * a / 255);
                px[2] = (uint8_t)(px[2] * a / 255);
            }
        }
    }
    g_frameLeft = anyPixels ? left : 0;
    g_frameTop = anyPixels ? top : 0;
    g_frameRight = anyPixels ? right : 0;
    g_frameBottom = anyPixels ? bottom : 0;
    return anyPixels;
}

DWORD WINAPI overlayWorker(LPVOID)
{
    // Load plugins HERE on the worker thread, NOT in DllMain: LoadLibrary + c4p_init (GdiplusStartup)
    // are loader-lock-unsafe in DLL_PROCESS_ATTACH (deadlock). featuremenu buildMenu
    // waits on g_pluginsReady before reading the list (loading no longer serialized by the loader lock).
    loadFolder("*.c4p");
    if (g_pluginCount)
        plog("[plugins] %d plugin(s) loaded", g_pluginCount);
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
    plog("[plugins] renderer-native canvas up; compositing %d plugin(s)", g_pluginCount);

    bool announced = false;
    for (;;) {
        game = gameHwnd();
        if (game) {
            const int w = DDGetGameWidth();
            const int h = DDGetGameHeight();
            if (w > 0 && h > 0) {
                const DWORD now = GetTickCount();
                {
                    // Turn detection (player + serial + in-game) is driven by the off[6] turn-info
                    // hook (cross-client). This host-only server-player poll stays ONLY as an in-game
                    // cross-check so the HOST notices it left even if the off[5] hook is missed.
                    int tp = featuremenu_server_player(nullptr);
                    if (tp >= 0) {
                        g_turnMiss = 0;
                        g_inGame = 1;
                        g_hasServer = 1; // this client runs the in-process server (host)
                    } else if (++g_turnMiss >= 4) {
                        g_inGame = 0;
                    }
                }
                for (int i = 0; i < g_pluginCount; ++i)
                    if (g_plugins[i].tick)
                        g_plugins[i].tick(now);

                const bool dirty = InterlockedExchange(&g_dirty, 0) != 0;
                if (dirty || w != g_ovW || h != g_ovH) {
                    bool rebuilt = false;
                    AcquireSRWLockExclusive(&g_frameLock);
                    const bool sizeChanged = ensureSurface(w, h);
                    if (g_dib && (sizeChanged || dirty)) {
                        InterlockedExchange(&g_frameHasPixels,
                                            rebuildOverlay(w, h) ? 1 : 0);
                        rebuilt = true;
                    } else if (!g_dib) {
                        InterlockedExchange(&g_frameHasPixels, 0);
                    }
                    ReleaseSRWLockExclusive(&g_frameLock);

                    if (rebuilt) {
                        // Wake the renderer even when the game surface itself is idle. The next
                        // upload blends this canvas into the exact same frame used by OBS and shots.
                        DDInvalidatePluginFrame();
                        if (!announced) {
                            plog("[plugins] renderer-native first paint (%dx%d)", w, h);
                            announced = true;
                        }
                    }
                }
            }
        }
        Sleep(33);
    }
}

} // namespace

extern "C" int pluginhost_overlay_ready(int width, int height)
{
    if (InterlockedCompareExchange(&g_frameHasPixels, 0, 0) == 0)
        return 0;
    AcquireSRWLockShared(&g_frameLock);
    const int ready = g_dib && g_ovW == width && g_ovH == height &&
        InterlockedCompareExchange(&g_frameHasPixels, 0, 0) != 0;
    ReleaseSRWLockShared(&g_frameLock);
    return ready;
}

// Source-over the premultiplied BGRA plugin canvas into a wrapper-owned presentation buffer.
// This is deliberately after the game/decorative/cursor layers: the timer must be visible in the
// actual presented frame (OBS, screenshots and every renderer), not in a manifest-gated HWND.
extern "C" int pluginhost_blend_overlay(
    void* destination, int width, int height, int pitch, int bpp, int rgb555)
{
    if (!destination || width <= 0 || height <= 0 || pitch <= 0 ||
        (bpp != 16 && bpp != 32) ||
        InterlockedCompareExchange(&g_frameHasPixels, 0, 0) == 0)
        return 0;

    int blended = 0;
    AcquireSRWLockShared(&g_frameLock);
    if (g_dib && g_ovW == width && g_ovH == height &&
        InterlockedCompareExchange(&g_frameHasPixels, 0, 0) != 0) {
        const int bytesPerPixel = bpp / 8;
        auto* dstBase = static_cast<uint8_t*>(destination);
        const int left = g_frameLeft;
        const int top = g_frameTop;
        const int right = g_frameRight;
        const int bottom = g_frameBottom;
        if (left >= 0 && top >= 0 && right > left && bottom > top &&
            right <= width && bottom <= height) {
            for (int y = top; y < bottom; ++y) {
                const uint8_t* src = g_dib +
                    (static_cast<size_t>(y) * width + left) * 4u;
                uint8_t* dst = dstBase + static_cast<size_t>(y) * pitch +
                    static_cast<size_t>(left) * bytesPerPixel;
                for (int x = left; x < right; ++x, src += 4, dst += bytesPerPixel) {
                    const unsigned alpha = src[3];
                    if (!alpha)
                        continue;
                    const unsigned inv = 255u - alpha;
                    if (bpp == 32) {
                        dst[0] = static_cast<uint8_t>(src[0] + (dst[0] * inv + 127u) / 255u);
                        dst[1] = static_cast<uint8_t>(src[1] + (dst[1] * inv + 127u) / 255u);
                        dst[2] = static_cast<uint8_t>(src[2] + (dst[2] * inv + 127u) / 255u);
                        dst[3] = 0xFF;
                    } else {
                        uint16_t packed = 0;
                        memcpy(&packed, dst, sizeof(packed));
                        unsigned dr = 0, dg = 0, db = 0;
                        if (rgb555) {
                            dr = ((packed >> 10) & 31u) * 255u / 31u;
                            dg = ((packed >> 5) & 31u) * 255u / 31u;
                            db = (packed & 31u) * 255u / 31u;
                        } else {
                            dr = ((packed >> 11) & 31u) * 255u / 31u;
                            dg = ((packed >> 5) & 63u) * 255u / 63u;
                            db = (packed & 31u) * 255u / 31u;
                        }
                        const unsigned r = src[2] + (dr * inv + 127u) / 255u;
                        const unsigned g = src[1] + (dg * inv + 127u) / 255u;
                        const unsigned b = src[0] + (db * inv + 127u) / 255u;
                        packed = rgb555
                            ? static_cast<uint16_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))
                            : static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                        memcpy(dst, &packed, sizeof(packed));
                    }
                    blended = 1;
                }
            }
        }
    }
    ReleaseSRWLockShared(&g_frameLock);
    return blended;
}

// WM_ENTERMENULOOP / WM_EXITMENULOOP arrive on the game thread. A separate layered overlay belonged
// to the worker thread in older builds. Retain the ABI/flag while the native compositor keeps
// the canvas in the game frame and therefore naturally below real Win32 menus.
extern "C" void pluginhost_menu_loop(int active)
{
    InterlockedExchange(&g_menuLoopActive, active ? 1 : 0);
}

// Fast AI is meaningful only in the process that owns CMidServer.  Keep that decision in the
// already SEH-guarded server accessor instead of inferring it from window titles, MP role labels,
// or the mere presence of ThreadWindowClass (all of which proved too broad for mixed host/client
// test runs).  g_hasServer is the lock-free fast path maintained by the plugin worker; the lazy
// probe makes the predicate independent of whether any overlay plugin happened to be loaded.
extern "C" int pluginhost_has_server(void)
{
    if (InterlockedCompareExchange(&g_hasServer, 0, 0) != 0)
        return 1;

    int inGame = 0;
    if (featuremenu_server_player(&inGame) >= 0 && inGame) {
        g_inGame = 1;
        InterlockedExchange(&g_hasServer, 1);
        return 1;
    }
    return 0;
}

// Called from cnc-ddraw's DllMain (after embed + featuremenu). Starts the overlay worker; zero cost
// with no plugins. No teardown: a c4p_shutdown from DllMain/atexit would run under the loader lock
// and join the worker / call GdiplusShutdown (unsafe at process exit), so cleanup is left to the OS.
// c4p_shutdown is still called where safe: a plugin whose c4p_init fails calls it itself (see timer.cpp).
extern "C" void pluginhost_install(void)
{
    // DllMain context: only create the thread (CreateThread is loader-lock-safe); the actual loading
    // happens at the start of overlayWorker after the loader lock is released (see note there).
    g_pluginsReady = CreateEventA(nullptr, TRUE, FALSE, nullptr); // manual-reset; signaled once loaded
    HANDLE t = CreateThread(nullptr, 0, &overlayWorker, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
}

// list accessors for the menu bar (featuremenu builds a "Plugins" menu from these)
extern "C" int pluginhost_count(void)
{
    return g_pluginCount;
}
extern "C" const char* pluginhost_name(int i)
{
    return (i >= 0 && i < g_pluginCount) ? g_plugins[i].name : "";
}
extern "C" void* pluginhost_menu(int i) // plugin config submenu HMENU, or null
{
    return (i >= 0 && i < g_pluginCount) ? g_plugins[i].menu : nullptr;
}

// Route a menu WM_COMMAND to the owning plugin's c4p_command (ids in the 0xB000+ plugin block).
// Returns 1 if handled; 0 otherwise.
extern "C" int pluginhost_command(unsigned id)
{
    for (int i = 0; i < g_pluginCount; ++i) {
        Plugin& p = g_plugins[i];
        if (p.command && id >= p.menuBase && id < p.menuBase + 0x100) {
            p.command((int)id);
            return 1;
        }
    }
    return 0;
}

// Called from the game UI thread at WM_INITMENUPOPUP. Without this callback, a plugin item that was
// initially grayed can never receive WM_COMMAND and therefore can never update its own state.
extern "C" void pluginhost_refresh_menus(void)
{
    for (int i = 0; i < g_pluginCount; ++i)
        if (g_plugins[i].refreshMenu)
            g_plugins[i].refreshMenu();
}

extern "C" int pluginhost_key(UINT msg, WPARAM wParam, LPARAM lParam)
{
    for (int i = 0; i < g_pluginCount; ++i)
        if (g_plugins[i].key && g_plugins[i].key(msg, wParam, lParam))
            return 1;
    return 0;
}

// Forward mouse input without creating an interactive child window. Sample the physical OS cursor,
// then map it through cnc-ddraw's viewport and inverse simple zoom into C4P_Canvas game coordinates.
extern "C" int pluginhost_mouse(UINT msg, WPARAM wParam)
{
    // Lifecycle messages must never depend on cursor geometry. In particular, SetCapture can move
    // to another window while the pointer is outside our viewport; dropping this notification would
    // leave a plugin's drag latch active and make later map input appear frozen.
    if (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED) {
        int handled = 0;
        for (int i = 0; i < g_pluginCount; ++i) {
            Plugin& p = g_plugins[i];
            if (p.mouse && p.mouse(msg, wParam, 0, 0))
                handled = 1;
        }
        return handled;
    }

    HWND game = gameHwnd();
    POINT pt{};
    if (!game || !DDGetPhysicalCursorPos(&pt) || !DDPhysicalScreenToClient(game, &pt))
        return 0;

    // Plugins render in logical game-surface pixels so their canvas is part of the exact frame.
    // Convert the physical client point through cnc-ddraw's live viewport before hit-testing/drag.
    int gameWidth = 0, gameHeight = 0;
    int viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
    if (!DDGetScaleMetrics(&gameWidth, &gameHeight, nullptr, nullptr,
                           &viewportX, &viewportY, &viewportWidth, &viewportHeight) ||
        gameWidth <= 0 || gameHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0)
        return 0;
    int visibleX = viewportX;
    int visibleY = viewportY;
    int visibleWidth = viewportWidth;
    int visibleHeight = viewportHeight;
    DDApplySimpleZoomViewport(0, &visibleX, &visibleY, &visibleWidth, &visibleHeight);
    const bool insideVisibleFrame =
        visibleWidth > 0 && visibleHeight > 0 &&
        pt.x >= visibleX && pt.y >= visibleY &&
        pt.x < visibleX + visibleWidth && pt.y < visibleY + visibleHeight;
    // A new grab/ordinary hover belongs only to the pixels actually presented. Outside MOVE/UP is
    // forwarded solely while a plugin can legitimately own the game's Win32 capture.
    const bool capturedContinuation = GetCapture() == game &&
        (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP);
    if (!insideVisibleFrame && !capturedContinuation)
        return 0;
    pt.x = static_cast<LONG>((static_cast<LONGLONG>(pt.x - viewportX) * gameWidth) /
                             viewportWidth);
    pt.y = static_cast<LONG>((static_cast<LONGLONG>(pt.y - viewportY) * gameHeight) /
                             viewportHeight);
    int logicalX = static_cast<int>(pt.x);
    int logicalY = static_cast<int>(pt.y);
    DDApplySimpleZoomMouse(&logicalX, &logicalY, gameWidth, gameHeight);
    for (int i = 0; i < g_pluginCount; ++i) {
        Plugin& p = g_plugins[i];
        if (p.mouse && p.mouse(msg, wParam, logicalX, logicalY))
            return 1;
    }
    return 0;
}

// Block until the worker finished loading plugins (or timeout), so featuremenu's buildMenu reads a
// complete list (needed since loading moved off the loader-lock-serialized DllMain to the worker).
extern "C" void pluginhost_wait_ready(unsigned ms)
{
    if (g_pluginsReady)
        WaitForSingleObject(g_pluginsReady, ms);
}
