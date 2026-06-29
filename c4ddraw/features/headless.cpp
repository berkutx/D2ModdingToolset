/*
 * Headless message-only window shim for C4dll-R, ACTIVE ONLY when ddraw.ini renderer=null.
 * Ported from the mss32 mod's noxwindow, relocated into the renderer wrapper (its natural home) and
 * converted from an exe-IAT patch to a function-level Detour.
 *
 * Under a headless null graphics driver (e.g. Wine's null driver with no X server) a real top-level
 * window fails: CreateWindowExA returns NULL. The game then (a) cannot finish window setup at boot,
 * and (b) silently skips creating its MP server-logic worker thread, whose 'CMqThread' top-level
 * window also returns NULL -> IMqNetSession::createServer is never called, so a headless host cannot
 * accept lobby joins. Route every top-level window to HWND_MESSAGE (needs no display), and fake the
 * degenerate screen/client geometry, so boot finalization passes AND the worker window succeeds.
 *
 * Function-level Detour (not an IAT patch): the worker window is created on another thread long after
 * boot, by which point the exe's user32 IAT has been re-snapped; an IAT patch reaches only the boot
 * window. A function detour covers every caller on every thread.
 *
 * NOTE: pair with renderer=null only; a normal GUI client has renderer != null so this never fires.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>
#include <cctype>
#include <cstdlib>
// config.h is a C header with no extern "C" guard; include it with C linkage so g_config resolves to
// the C symbol the cnc-ddraw .c objects define (windows.h above makes its re-include a guarded no-op).
extern "C" {
#include "config.h"
}

namespace {

HWND(WINAPI* realCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU,
                                  HINSTANCE, LPVOID) = CreateWindowExA;
int(WINAPI* realGetSystemMetrics)(int) = GetSystemMetrics;
BOOL(WINAPI* realGetClientRect)(HWND, LPRECT) = GetClientRect;
int(WINAPI* realGetDeviceCaps)(HDC, int) = GetDeviceCaps;

int g_fakeW = 800;
int g_fakeH = 600;
int g_fakeBpp = 16;

void dbg(const char* s)
{
    OutputDebugStringA(s);
}

HWND WINAPI hookedCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w,
                                  int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    // A headless null driver refuses real top-level windows; route them to a message-only window.
    // It needs no graphics driver, succeeds, and is enough for the game (it renders via DirectDraw,
    // never paints/reads this window) and for the server worker's message pump.
    if (parent == nullptr && (style & WS_CHILD) == 0)
        parent = HWND_MESSAGE;

    return realCreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

int WINAPI hookedGetSystemMetrics(int idx)
{
    if (idx == SM_CXSCREEN)
        return g_fakeW;
    if (idx == SM_CYSCREEN)
        return g_fakeH;

    return realGetSystemMetrics(idx);
}

BOOL WINAPI hookedGetClientRect(HWND hwnd, LPRECT rc)
{
    const BOOL ok = realGetClientRect(hwnd, rc);

    // A message-only window has no client area (0x0); the game's resize-verify treats that as a
    // failure. Substitute the faked client size so finalization passes.
    if (rc && (rc->right - rc->left == 0 || rc->bottom - rc->top == 0)) {
        rc->left = 0;
        rc->top = 0;
        rc->right = g_fakeW;
        rc->bottom = g_fakeH;
        return TRUE;
    }

    return ok;
}

int WINAPI hookedGetDeviceCaps(HDC dc, int idx)
{
    // The fullscreen display-mode setter bails if BITSPIXEL != the game's depth (16 for Disciples II).
    if (idx == BITSPIXEL)
        return g_fakeBpp;

    return realGetDeviceCaps(dc, idx);
}

} // namespace

// Called from cnc-ddraw's DllMain after hook_init (g_config is already populated by cfg_load). No-op
// unless renderer=null.
extern "C" void headless_install(void)
{
    // Gate: headless only. Match cnc-ddraw's first-letter renderer test (dd.c): 'n' => null.
    if (tolower((unsigned char)g_config.renderer[0]) != 'n')
        return;

    // Fake geometry from ddraw.ini fake_mode "WxHxbpp" (the same source cnc-ddraw's own fakes parse,
    // so both hook layers agree); keep the 800x600x16 defaults otherwise.
    if (g_config.fake_mode[0]) {
        char* e = &g_config.fake_mode[0];
        const int w = (int)strtoul(e, &e, 0);
        const int hh = (int)strtoul(e + 1, &e, 0);
        const int b = (int)strtoul(e + 1, &e, 0);
        if (w > 0)
            g_fakeW = w;
        if (hh > 0)
            g_fakeH = hh;
        if (b > 0)
            g_fakeBpp = b;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(realCreateWindowExA), hookedCreateWindowExA);
    DetourAttach(&reinterpret_cast<PVOID&>(realGetSystemMetrics), hookedGetSystemMetrics);
    DetourAttach(&reinterpret_cast<PVOID&>(realGetClientRect), hookedGetClientRect);
    DetourAttach(&reinterpret_cast<PVOID&>(realGetDeviceCaps), hookedGetDeviceCaps);
    if (DetourTransactionCommit() == NO_ERROR)
        dbg("[headless] renderer=null: top-level windows -> HWND_MESSAGE + fake screen geometry\n");
    else
        dbg("[headless] DetourAttach failed\n");
}
