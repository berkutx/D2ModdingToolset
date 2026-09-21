#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define SDM_KEEP_WINDOW 8
static struct { BOOL windowed, fullscreen, resizable, border; RECT window_rect; } g_config;
static struct { HWND hwnd; int width, height; struct { int width, height; HANDLE thread; } render; } g_ddraw;
static BOOL test_maximized, recurse, client_ok = TRUE;
static LONG test_style = WS_OVERLAPPEDWINDOW;
static RECT test_client;
static unsigned calls, checks;
static DWORD last_flags;
void dd_ResizeWindowOutput(int width, int height, BOOL restore_request);
#define CHECK(c) do { ++checks; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static BOOL test_IsZoomed(HWND window) { (void)window; return test_maximized; }
#define IsZoomed test_IsZoomed
static BOOL real_ClientToScreen(HWND window, POINT* p) { (void)window; p->x = 20; p->y = 50; return TRUE; }
static BOOL real_GetClientRect(HWND window, RECT* r) { (void)window; *r = test_client; return client_ok; }
static LONG real_GetWindowLongA(HWND window, int index) { (void)window; (void)index; return test_style; }
static HRESULT dd_SetDisplayMode(DWORD width, DWORD height, DWORD bpp, DWORD flags)
{
    CHECK(width == 0 && height == 0 && bpp == 0);
    ++calls;
    last_flags = flags;
    if (recurse) dd_ResizeWindowOutput(1000, 700, FALSE);
    g_ddraw.render.width = g_config.window_rect.right;
    g_ddraw.render.height = g_config.window_rect.bottom;
    return S_OK;
}
#include "native_window_extracted.h"

int main(void)
{
    RECT before;
    g_config.windowed = g_config.resizable = g_config.border = TRUE;
    g_config.window_rect.left = 123; g_config.window_rect.top = 456;
    g_ddraw.hwnd = (HWND)1; g_ddraw.width = 1600; g_ddraw.height = 900;
    g_ddraw.render.thread = (HANDLE)1;
    g_ddraw.render.width = 1600; g_ddraw.render.height = 900;
    before = g_config.window_rect;

    test_maximized = TRUE;
    dd_ResizeWindowOutput(1920, 1001, FALSE);
    CHECK(calls == 1 && last_flags == SDM_KEEP_WINDOW);
    CHECK(g_ddraw.width == 1600 && g_ddraw.height == 900);
    CHECK(EqualRect(&g_config.window_rect, &before));
    test_maximized = FALSE;
    dd_ResizeWindowOutput(1600, 900, TRUE);
    CHECK(calls == 2 && EqualRect(&g_config.window_rect, &before));

    /* Snap scales the existing canvas, including to less than 1600 x 900. */
    dd_ResizeWindowOutput(952, 1001, FALSE);
    CHECK(calls == 3 && g_config.window_rect.right == 952);
    CHECK(g_config.window_rect.bottom == 1001 && g_config.window_rect.left == 20);
    CHECK(g_ddraw.width == 1600 && g_ddraw.height == 900);
    recurse = TRUE;
    dd_ResizeWindowOutput(960, 990, FALSE);
    CHECK(calls == 4 && g_ddraw.render.width == 960);
    recurse = FALSE;
    dd_ResizeWindowOutput(960, 990, FALSE); dd_ResizeWindowOutput(0, 0, FALSE);
    CHECK(calls == 4);
    g_config.fullscreen = TRUE; dd_ResizeWindowOutput(1000, 700, FALSE);
    g_config.fullscreen = FALSE; g_config.windowed = FALSE; dd_ResizeWindowOutput(1000, 700, FALSE);
    g_config.windowed = TRUE; g_config.resizable = FALSE; dd_ResizeWindowOutput(1000, 700, FALSE);
    g_config.resizable = TRUE; g_ddraw.render.thread = NULL; dd_ResizeWindowOutput(1000, 700, FALSE);
    CHECK(calls == 4); g_ddraw.render.thread = (HANDLE)1;

    SetRect(&test_client, 0, 0, 960, 990);
    CHECK(extracted_keep_native(0)); /* shader reload keeps snapped geometry */
    g_config.window_rect.right = 1700;
    CHECK(!extracted_keep_native(0)); /* explicit normal output choice applies */
    CHECK(extracted_keep_native(SDM_KEEP_WINDOW));
    test_maximized = TRUE;
    CHECK(extracted_keep_native(0)); /* maximized shader/backend reload */
    test_style = WS_POPUP;
    CHECK(!extracted_keep_native(0)); /* borderless -> normal must really restore */
    test_style = WS_OVERLAPPEDWINDOW;
    g_config.fullscreen = TRUE; CHECK(!extracted_keep_native(SDM_KEEP_WINDOW));
    g_config.fullscreen = FALSE; g_config.windowed = FALSE;
    CHECK(!extracted_keep_native(SDM_KEEP_WINDOW)); g_config.windowed = TRUE;
    client_ok = FALSE; CHECK(!extracted_keep_native(0)); client_ok = TRUE;
    test_client.bottom = 0; CHECK(!extracted_keep_native(0));
    /* Regression: maximize -> Snap is SIZE_RESTORED without SC_RESTORE. */
    g_config.window_rect.right = 0; g_config.window_rect.bottom = 0;
    test_maximized = TRUE;
    dd_ResizeWindowOutput(1920, 1001, FALSE);
    CHECK(g_config.window_rect.right == 0);
    test_maximized = FALSE;
    dd_ResizeWindowOutput(952, 1001, FALSE);
    SetRect(&test_client, 0, 0, 952, 1001);
    CHECK(g_config.window_rect.right == 952 && g_config.window_rect.bottom == 1001);
    CHECK(extracted_keep_native(0));
    g_config.border = FALSE;
    before = g_config.window_rect;
    dd_ResizeWindowOutput(1000, 700, FALSE);
    CHECK(EqualRect(&before, &g_config.window_rect));
    CHECK(!extracted_keep_native(SDM_KEEP_WINDOW));
    printf("PASS: %u actual native-window policy checks\n", checks);
    return 0;
}
