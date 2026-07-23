/*
 * Disciples II cursor/edge-scroll guard layered on cnc-ddraw's public fake_GetCursorPos hook.
 *
 * cnc-ddraw quite reasonably clamps the cursor to the render rectangle. In D2 that means leaving
 * the window can look exactly like holding the cursor on a map edge, so scrolling never stops.
 * Detouring the already-installed fake function keeps this game-specific behavior out of the
 * upstream source patch.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

extern "C" {
#include "config.h"
#include "dd.h"
#include "hook.h"
#include "winapi_hooks.h"
}

extern "C" int g_c4dll_dragActive;

namespace {

using FakeGetCursorPos = BOOL(WINAPI*)(LPPOINT);
FakeGetCursorPos g_originalFakeGetCursorPos = fake_GetCursorPos;

BOOL WINAPI guardedGetCursorPos(LPPOINT point)
{
    const BOOL result = g_originalFakeGetCursorPos(point);
    if (!result || !point || !g_ddraw.ref || !g_ddraw.hwnd || !g_ddraw.width || !g_ddraw.height)
        return result;

    POINT real{};
    if (!real_GetCursorPos(&real))
        return result;
    if (g_config.windowed && !real_ScreenToClient(g_ddraw.hwnd, &real))
        return result;

    int x = real.x - g_ddraw.mouse.x_adjust;
    int y = real.y - g_ddraw.mouse.y_adjust;
    if (g_config.adjmouse && !g_ddraw.child_window_exists) {
        x = static_cast<int>(x * g_ddraw.mouse.unscale_x + (x >= 0 ? 0.5f : -0.5f));
        y = static_cast<int>(y * g_ddraw.mouse.unscale_y + (y >= 0 ? 0.5f : -0.5f));
    }

    constexpr int margin = 50;
    if (!g_c4dll_dragActive && x >= -margin && x <= static_cast<int>(g_ddraw.width) - 1 + margin &&
        y >= -margin && y <= static_cast<int>(g_ddraw.height) - 1 + margin &&
        GetForegroundWindow() == g_ddraw.hwnd)
        return result;

    point->x = static_cast<LONG>(g_ddraw.width / 2);
    point->y = static_cast<LONG>(g_ddraw.height / 2);
    InterlockedExchange(reinterpret_cast<LONG*>(&g_ddraw.cursor.x), point->x);
    InterlockedExchange(reinterpret_cast<LONG*>(&g_ddraw.cursor.y), point->y);
    return TRUE;
}

} // namespace

extern "C" void cursorfix_install(void)
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(g_originalFakeGetCursorPos), guardedGetCursorPos);
    if (DetourTransactionCommit() == NO_ERROR)
        OutputDebugStringA("[cursor] edge-scroll guard installed over fake_GetCursorPos\n");
    else
        OutputDebugStringA("[cursor] failed to install edge-scroll guard\n");
}
