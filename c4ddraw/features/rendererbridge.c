/*
 * Narrow bridge from C4dll-R features to cnc-ddraw internals.
 *
 * Kept as our own translation unit: screenshot/menu/drag-scroll behavior does not belong in a
 * source patch against cnc-ddraw's dllmain.c.
 */

#include <windows.h>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

#include "config.h"
#include "dd.h"
#include "debug.h"
#include "mouse.h"
#include "screenshot.h"

/*
 * DisciplesGL 2.0.2 "simple zoom" state. Keep this in the wrapper-owned bridge rather than an
 * upstream cnc-ddraw patch: wndproc/render backends only call the narrow integration functions below.
 *
 * Fixed point makes the UI thread writes and renderer-thread reads atomic on 32-bit Windows.
 */
static volatile LONG g_simple_zoom_1000 = 1000; /* 1.0 .. 8.0 */
static volatile LONG g_simple_anchor_x_100000 = 50000; /* client point / outer window width */
static volatile LONG g_simple_anchor_y_100000 = 50000; /* client point / outer window height */

/*
 * Exact wheel policy from DisciplesGL 2.0.2:
 *   Ctrl+wheel up   : +0.1
 *   Ctrl+wheel down : -0.4
 *   clamp           : 1.0 .. 8.0
 * The caller always turns the wheel into VK_UP/VK_DOWN, including when Ctrl is not held.
 */
int DDHandleSimpleZoom(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    const short delta = (short)HIWORD(wParam);

    if (GetKeyState(VK_CONTROL) < 0)
    {
        LONG zoom = InterlockedExchangeAdd(&g_simple_zoom_1000, 0);
        zoom += delta > 0 ? 100 : -400;
        if (zoom < 1000)
            zoom = 1000;
        else if (zoom > 8000)
            zoom = 8000;

        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT wr = { 0 };
        ScreenToClient(hwnd, &pt);
        if (GetWindowRect(hwnd, &wr))
        {
            const LONG width = wr.right - wr.left;
            const LONG height = wr.bottom - wr.top;
            if (width > 0 && height > 0)
            {
                InterlockedExchange(&g_simple_anchor_x_100000,
                    (LONG)((double)pt.x * 100000.0 / (double)width));
                InterlockedExchange(&g_simple_anchor_y_100000,
                    (LONG)((double)pt.y * 100000.0 / (double)height));
            }
        }
        InterlockedExchange(&g_simple_zoom_1000, zoom);

        InterlockedExchange(&g_ddraw.render.clear_screen, TRUE);
        if (g_ddraw.render.sem)
            ReleaseSemaphore(g_ddraw.render.sem, 1, NULL);
    }

    return delta > 0 ? VK_UP : VK_DOWN;
}

/*
 * Expand the final destination rectangle around the cursor anchor. This is algebraically equivalent
 * to the original wrapper's final-quad transform and works for OpenGL, Direct3D 9 and GDI alike.
 * OpenGL viewports use a bottom-left origin; Direct3D/GDI use top-left.
 */
void DDApplySimpleZoomViewport(int bottom_origin, int* x, int* y, int* width, int* height)
{
    const LONG zoom_i = InterlockedExchangeAdd(&g_simple_zoom_1000, 0);
    if (!x || !y || !width || !height || zoom_i == 1000 || *width <= 0 || *height <= 0)
        return;

    const double zoom = (double)zoom_i / 1000.0;
    const double anchor_x =
        (double)InterlockedExchangeAdd(&g_simple_anchor_x_100000, 0) / 100000.0;
    double anchor_y =
        (double)InterlockedExchangeAdd(&g_simple_anchor_y_100000, 0) / 100000.0;
    if (bottom_origin)
        anchor_y = 1.0 - anchor_y;

    const int base_x = *x;
    const int base_y = *y;
    const int base_w = *width;
    const int base_h = *height;
    *x = base_x + (int)(anchor_x * base_w * (1.0 - zoom));
    *y = base_y + (int)(anchor_y * base_h * (1.0 - zoom));
    *width = (int)(base_w * zoom);
    *height = (int)(base_h * zoom);
}

void DDReloadConfig(void)
{
    TRACE("%s [%p]\n", __FUNCTION__, _ReturnAddress());

    if (!g_ddraw.ref || !g_ddraw.hwnd || !g_ddraw.width)
        return;

    LONG saved_left = g_config.window_rect.left;
    LONG saved_top = g_config.window_rect.top;
    cfg_load();
    if (g_config.window_rect.left == -32000)
        g_config.window_rect.left = saved_left;
    if (g_config.window_rect.top == -32000)
        g_config.window_rect.top = saved_top;

    dd_SetDisplayMode(0, 0, 0, 0);

    if (g_mouse_locked)
    {
        mouse_unlock();
        mouse_lock();
    }

    InterlockedExchange(&g_ddraw.render.clear_screen, TRUE);
    if (g_ddraw.render.sem)
        ReleaseSemaphore(g_ddraw.render.sem, 1, NULL);
    RedrawWindow(g_ddraw.hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
}

void DDTakeScreenshot(void)
{
    TRACE("%s [%p]\n", __FUNCTION__, _ReturnAddress());
    if (g_ddraw.ref && g_ddraw.primary)
        ss_take_screenshot(g_ddraw.primary);
}

void DDMapClientToGame(long cx, long cy, long* gx, long* gy)
{
    int x = (int)cx - g_ddraw.mouse.x_adjust;
    int y = (int)cy - g_ddraw.mouse.y_adjust;
    if (g_config.adjmouse && !g_ddraw.child_window_exists)
    {
        x = (int)(x * g_ddraw.mouse.unscale_x + (x >= 0 ? 0.5f : -0.5f));
        y = (int)(y * g_ddraw.mouse.unscale_y + (y >= 0 ? 0.5f : -0.5f));
    }
    if (x < 0)
        x = 0;
    else if (x > (int)g_ddraw.width - 1)
        x = (int)g_ddraw.width - 1;
    if (y < 0)
        y = 0;
    else if (y > (int)g_ddraw.height - 1)
        y = (int)g_ddraw.height - 1;
    if (gx)
        *gx = x;
    if (gy)
        *gy = y;
}
