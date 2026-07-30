/*
 * Disciples II cursor/edge-scroll guard layered on cnc-ddraw's public fake_GetCursorPos hook.
 *
 * Keep cnc-ddraw's transformed/clamped position while the game owns the foreground, including
 * when the physical cursor has moved outside the window. This preserves D2's native edge scroll.
 * Once the game loses the foreground, return a neutral center position so a background window
 * cannot keep scrolling. Detouring the already-installed fake function keeps this game-specific
 * behavior out of the upstream source patch.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

extern "C" {
#include "dd.h"
#include "winapi_hooks.h"
}

namespace {

using FakeGetCursorPos = BOOL(WINAPI*)(LPPOINT);
FakeGetCursorPos g_originalFakeGetCursorPos = fake_GetCursorPos;

BOOL WINAPI guardedGetCursorPos(LPPOINT point)
{
    const BOOL result = g_originalFakeGetCursorPos(point);
    if (!result || !point || !g_ddraw.ref || !g_ddraw.hwnd || !g_ddraw.width || !g_ddraw.height)
        return result;

    if (GetForegroundWindow() == g_ddraw.hwnd)
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
