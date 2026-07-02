/*
 * Headless message-only window FALLBACK for C4dll-R, installed only when ddraw.ini renderer=null.
 * Headless is not a mode, it is a fact of the environment: the detour first creates every window
 * for real; only when the environment CANNOT create a top-level window at all (Wine null driver,
 * no X server: CreateWindowExA returns NULL) it retries the same window as message-only, and only
 * a SUCCESSFUL retry switches the fake-geometry shims on. On a desktop the real calls succeed and
 * the shim stays fully inert: renderer=null keeps a normal window.
 * Ported from the mss32 mod's noxwindow, relocated into the renderer wrapper (its natural home) and
 * converted from an exe-IAT patch to a function-level Detour.
 *
 * Why the fallback is needed at all: without it, under a null graphics driver the game (a) cannot
 * finish window setup at boot, and (b) silently skips creating its MP server-logic worker thread,
 * whose 'CMqThread' top-level window also returns NULL -> IMqNetSession::createServer is never
 * called, so a headless host cannot accept lobby joins. Message-only windows need no display and
 * keep the game's window-message queues (its main loop IS a message pump) fully functional.
 *
 * Function-level Detour (not an IAT patch): the worker window is created on another thread long after
 * boot, by which point the exe's user32 IAT has been re-snapped; an IAT patch reaches only the boot
 * window. A function detour covers every caller on every thread.
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
// Set once, when the environment has PROVEN it cannot create real top-level windows (the real
// create failed AND the message-only retry succeeded). Gates every fake below.
volatile LONG g_noDisplay = 0;

void dbg(const char* s)
{
    OutputDebugStringA(s);
}

HWND WINAPI hookedCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w,
                                  int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    const bool topLevel = parent == nullptr && (style & WS_CHILD) == 0;

    // Environment already proven headless: route top-level windows straight to message-only.
    if (topLevel && g_noDisplay)
        return realCreateWindowExA(ex, cls, name, style, x, y, w, h, HWND_MESSAGE, menu, inst, param);

    HWND hwnd = realCreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);

    if (!hwnd && topLevel) {
        // A headless null driver refuses real top-level windows; retry message-only (needs no
        // graphics driver; enough for the game, it renders via DirectDraw and never paints/reads
        // this window, and for the server worker's message pump). Only a SUCCESSFUL retry flips
        // headless mode: a failure for any other reason (bad params) fails the retry too and
        // falls through with the original NULL.
        HWND retry = realCreateWindowExA(ex, cls, name, style, x, y, w, h, HWND_MESSAGE, menu, inst,
                                         param);
        if (retry) {
            if (InterlockedExchange(&g_noDisplay, 1) == 0)
                dbg("[headless] no display driver: top-level windows -> HWND_MESSAGE + fake "
                    "screen geometry\n");
            return retry;
        }
    }

    return hwnd;
}

int WINAPI hookedGetSystemMetrics(int idx)
{
    if (g_noDisplay) {
        if (idx == SM_CXSCREEN)
            return g_fakeW;
        if (idx == SM_CYSCREEN)
            return g_fakeH;
    }

    return realGetSystemMetrics(idx);
}

BOOL WINAPI hookedGetClientRect(HWND hwnd, LPRECT rc)
{
    const BOOL ok = realGetClientRect(hwnd, rc);

    // A message-only window has no client area (0x0); the game's resize-verify treats that as a
    // failure. Substitute the faked client size so finalization passes.
    if (g_noDisplay && rc && (rc->right - rc->left == 0 || rc->bottom - rc->top == 0)) {
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
    if (g_noDisplay && idx == BITSPIXEL)
        return g_fakeBpp;

    return realGetDeviceCaps(dc, idx);
}

} // namespace

// Called from cnc-ddraw's DllMain after hook_init (g_config is already populated by cfg_load). No-op
// unless renderer=null.
extern "C" void headless_install(void)
{
    // Gate: null renderer only (every other backend needs a real window/DC anyway). Match
    // cnc-ddraw's first-letter renderer test (dd.c): 'n' => null. The detours below are inert on a
    // desktop: they only act after the environment fails a real top-level window creation.
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
        dbg("[headless] renderer=null: message-only fallback armed (inert while real windows "
            "succeed)\n");
    else
        dbg("[headless] DetourAttach failed\n");
}
