/*
 * Narrow bridge from C4dll-R features to cnc-ddraw internals.
 *
 * Kept as our own translation unit: screenshot/menu/drag-scroll behavior does not belong in a
 * source patch against cnc-ddraw's dllmain.c.
 */

#include <windows.h>
#include <ctype.h>
#include <intrin.h>
#include <stdlib.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

#include "config.h"
#include "dd.h"
#include "debug.h"
#include "ddsurface.h"
#include "hook.h"
#include "mouse.h"
#include "opengl_utils.h"
#include "render_d3d9.h"
#include "render_gdi.h"
#include "render_null.h"
#include "render_ogl.h"
#include "screenshot.h"
#include "utils.h"
#include "versionhelpers.h"
#include "eventtrace.h"

extern void HandleMessage(LPMSG, HWND, UINT, UINT, UINT);

extern const void* DDGetDecoratedSurface(
    const void* source, int width, int height, int pitch, int bpp, int rgb555);

/* featuremenu.cpp diagnostics (gated on the same C4menu.ini [menu] debugLog flag). */
extern int featuremenu_debug_enabled(void);
extern void featuremenu_debug_log_line(const char* line);
extern int horplus_get_decor_layout(int* contentWidth, int* contentHeight, int* wideBattle);

/* Read by the D2-only winapi_hooks patch. Generic cnc-ddraw users retain upstream semantics. */
volatile LONG g_c4_d2_cursor_ownership;

/*
 * Physical Win32 bridge for featuremenu.cpp.  cnc-ddraw's legacy hook=2 mode detours the
 * user32 entry points process-wide, so calling GetCursorPos/SetCursor/etc. by their import names
 * from our own code can recurse into fake_* just like a game call.  hook.c preserves the original
 * entry points in real_*; keep all wrapper-owned cursor/focus decisions on those functions.
 */
HCURSOR DDSetPhysicalCursor(HCURSOR cursor)
{
    return real_SetCursor(cursor);
}

BOOL DDGetPhysicalCursorPos(POINT* point)
{
    return real_GetCursorPos(point);
}

BOOL DDPhysicalScreenToClient(HWND hwnd, POINT* point)
{
    return real_ScreenToClient(hwnd, point);
}

BOOL DDGetPhysicalClientRect(HWND hwnd, RECT* rect)
{
    return real_GetClientRect(hwnd, rect);
}

HWND DDGetPhysicalForegroundWindow(void)
{
    return real_GetForegroundWindow();
}

HWND DDPhysicalWindowFromPoint(POINT point)
{
    return real_WindowFromPoint(point);
}

BOOL DDPhysicalPeekMessage(MSG* message, HWND hwnd, UINT first, UINT last, UINT remove)
{
    return real_PeekMessageA(message, hwnd, first, last, remove);
}

/* Batching separates raw FIFO inspection from one normal post-removal mapping.
 * No extra Peek, game limiter, drawing or MSS call belongs in these bridges. */
BOOL WINAPI DDMessageBatchPeekRaw(LPMSG message, HWND hwnd, UINT first, UINT last, UINT remove)
{
    return real_PeekMessageA(message, hwnd, first, last, remove);
}

void DDMessageBatchMapRemoved(LPMSG message)
{
    if (g_ddraw.ref)
        g_ddraw.last_msg_pull_tick = timeGetTime();
    eventtrace_pulled(C4TRACE_PEEK_RAW, message, TRUE, PM_REMOVE);
    HandleMessage(message, NULL, 0, 0, PM_REMOVE);
    eventtrace_pulled(C4TRACE_PEEK_MAPPED, message, TRUE, PM_REMOVE);
}

/* A plugin canvas update is a presentation update even when the game-owned primary did not change.
 * Wake the existing renderer semaphore so a paused/idle game still uploads the next timer second.
 * Deliberately do not publish pending Hor+ layout here: no matching game pixels were published. */
void DDInvalidatePluginFrame(void)
{
    if (InterlockedExchangeAdd(&g_ddraw.ref, 0) > 0 && g_ddraw.render.sem)
    {
        InterlockedExchange(&g_ddraw.render.surface_updated, TRUE);
        ReleaseSemaphore(g_ddraw.render.sem, 1, NULL);
    }
}

static HANDLE dd_create_live_limiter_timer(void)
{
    HANDLE timer = NULL;
    typedef HANDLE (WINAPI *CreateTimerExWFn)(
        LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    static CreateTimerExWFn create_timer_ex;
    static LONG resolved;

    if (InterlockedCompareExchange(&resolved, 1, 0) == 0 &&
        !IsWine() && IsWindows10Version1803OrGreater())
    {
        HMODULE kernel = real_LoadLibraryA("Kernel32.dll");
        if (kernel)
            create_timer_ex = (CreateTimerExWFn)real_GetProcAddress(
                kernel, "CreateWaitableTimerExW");
    }
    if (create_timer_ex)
        timer = create_timer_ex(
            NULL, NULL,
            CREATE_WAITABLE_TIMER_MANUAL_RESET |
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimer(NULL, TRUE, NULL);
    return timer;
}

/* Reconfigure cnc-ddraw's GUI-thread game limiter without rebuilding DirectDraw/the renderer.
 * Handles are retained until dd_Release: disabling a cap only makes the timer dormant, avoiding a
 * close-vs-wait race with unusual callers of WaitForVerticalBlank. Returns 1 only after an atomic
 * prepare/commit; the first frame after a change is deliberately immediate (due_time = 0). */
int DDSetMaxGameTicksLive(int ticks)
{
    HANDLE new_ticks = NULL;
    HANDLE new_flip = NULL;
    const BOOL custom = ticks > 0 && ticks <= 1000;
    const BOOL flip = ticks >= 0 || ticks == -2;
    DWORD ticks_ms = 0;
    LONGLONG ticks_ns = 0;
    const DWORD flip_ms = 17;
    const LONGLONG flip_ns = 166666;

    if ((!custom && ticks != -2 && ticks != -1 && ticks != 0) ||
        !g_ddraw.ref || !g_ddraw.gui_thread_id ||
        GetCurrentThreadId() != g_ddraw.gui_thread_id)
        return 0;

    if (custom)
    {
        const double length = 1000.0 / (double)ticks;
        ticks_ms = (DWORD)(length + 0.5);
        if (!ticks_ms)
            ticks_ms = 1;
        ticks_ns = 10000000LL / ticks;
        if (!g_ddraw.ticks_limiter.htimer)
        {
            new_ticks = dd_create_live_limiter_timer();
            if (!new_ticks)
                goto fail;
        }
    }
    if (flip && !g_ddraw.flip_limiter.htimer)
    {
        new_flip = dd_create_live_limiter_timer();
        if (!new_flip)
            goto fail;
    }

    /* Disable the published periods first. No caller can begin a new wait using half-old state. */
    InterlockedExchange((volatile LONG*)&g_ddraw.ticks_limiter.tick_length, 0);
    InterlockedExchange((volatile LONG*)&g_ddraw.flip_limiter.tick_length, 0);
    if (g_ddraw.ticks_limiter.htimer)
        CancelWaitableTimer(g_ddraw.ticks_limiter.htimer);
    if (g_ddraw.flip_limiter.htimer)
        CancelWaitableTimer(g_ddraw.flip_limiter.htimer);

    if (new_ticks)
        g_ddraw.ticks_limiter.htimer = new_ticks;
    if (new_flip)
        g_ddraw.flip_limiter.htimer = new_flip;
    g_ddraw.ticks_limiter.due_time.QuadPart = 0;
    g_ddraw.flip_limiter.due_time.QuadPart = 0;
    g_ddraw.ticks_limiter.tick_length_ns = custom ? ticks_ns : 0;
    g_ddraw.flip_limiter.tick_length_ns = flip ? flip_ns : 0;
    g_ddraw.ticks_limiter.dds_unlock_limiter_disabled = FALSE;
    g_ddraw.flip_limiter.dds_unlock_limiter_disabled = FALSE;
    g_config.maxgameticks = ticks;

    /* Period is the commit marker and is therefore published last. */
    if (custom)
        InterlockedExchange((volatile LONG*)&g_ddraw.ticks_limiter.tick_length,
                            (LONG)ticks_ms);
    if (flip)
        InterlockedExchange((volatile LONG*)&g_ddraw.flip_limiter.tick_length,
                            (LONG)flip_ms);
    return 1;

fail:
    if (new_ticks)
        CloseHandle(new_ticks);
    if (new_flip)
        CloseHandle(new_flip);
    return 0;
}

static unsigned dd_cursor_mask_shift(DWORD mask)
{
    unsigned shift = 0;
    while (mask && !(mask & 1u))
    {
        mask >>= 1u;
        ++shift;
    }
    return shift;
}

static unsigned dd_cursor_expand_channel(DWORD pixel, DWORD mask)
{
    const unsigned shift = dd_cursor_mask_shift(mask);
    const DWORD maximum = mask >> shift;
    const DWORD value = (pixel & mask) >> shift;
    return maximum ? (unsigned)((value * 255u + maximum / 2u) / maximum) : 0u;
}

/*
 * Copy one already-decoded cursor tile without calling IDirectDrawSurface7::Lock. The public Lock
 * path runs util_pull_messages(), which may dispatch a teardown message re-entrantly while the
 * game's CCursorImpl and renderer vtable are on the stack. This helper accepts only surfaces owned
 * by this embedded cnc-ddraw instance and reads their stable backing buffer directly.
 */
int DDSnapshotCursorSurfaceArgb(void* surface7,
                                int source_x,
                                int source_y,
                                int width,
                                int height,
                                DWORD* destination,
                                DWORD capacity,
                                int* any_opaque)
{
    IDirectDrawSurfaceImpl* surface = (IDirectDrawSurfaceImpl*)surface7;
    BOOL referenced = FALSE;
    BOOL locked = FALSE;
    int success = 0;

    if (any_opaque)
        *any_opaque = 0;
    if (!surface || !destination || !any_opaque || source_x < 0 || source_y < 0 ||
        width <= 0 || height <= 0 ||
        (ULONGLONG)(unsigned)width * (ULONGLONG)(unsigned)height > capacity)
        return 0;

    __try
    {
        __try
        {
            DWORD bits;
            DWORD bytes_per_pixel;
            DWORD surface_width;
            DWORD surface_height;
            DWORD pitch;
            DWORD red_mask;
            DWORD green_mask;
            DWORD blue_mask;
            DWORD pixel_mask;
            DWORD colour_low;
            DWORD colour_high;
            const BYTE* buffer;
            int y;

            if (surface->lpVtbl != &g_dds_vtbl)
                __leave;

            g_dds_vtbl.AddRef(surface);
            referenced = TRUE;
            if (g_config.lock_surfaces)
            {
                EnterCriticalSection(&surface->cs);
                locked = TRUE;
            }

            bits = surface->bpp;
            bytes_per_pixel = surface->bytes_pp;
            surface_width = surface->width;
            surface_height = surface->height;
            pitch = surface->pitch;
            if ((bits != 16 && bits != 32) || bytes_per_pixel != bits / 8u ||
                !surface_width || !surface_height ||
                !(surface->flags & DDSD_CKSRCBLT) ||
                (ULONGLONG)(unsigned)source_x + (unsigned)width > surface_width ||
                (ULONGLONG)(unsigned)source_y + (unsigned)height > surface_height ||
                (ULONGLONG)pitch < (ULONGLONG)surface_width * bytes_per_pixel ||
                (ULONGLONG)pitch * surface_height > surface->size)
                __leave;

            buffer = (const BYTE*)dds_GetBuffer(surface);
            if (!buffer)
                __leave;

            if (bits == 16)
            {
                red_mask = g_config.rgb555 ? 0x7C00u : 0xF800u;
                green_mask = g_config.rgb555 ? 0x03E0u : 0x07E0u;
                blue_mask = 0x001Fu;
                pixel_mask = 0x0000FFFFu;
            }
            else
            {
                red_mask = 0x00FF0000u;
                green_mask = 0x0000FF00u;
                blue_mask = 0x000000FFu;
                pixel_mask = 0x00FFFFFFu;
            }
            colour_low = surface->color_key.dwColorSpaceLowValue & pixel_mask;
            colour_high = surface->color_key.dwColorSpaceHighValue & pixel_mask;

            for (y = 0; y < height; ++y)
            {
                const BYTE* source = buffer +
                    (SIZE_T)(source_y + y) * pitch +
                    (SIZE_T)source_x * bytes_per_pixel;
                int x;
                for (x = 0; x < width; ++x)
                {
                    DWORD raw = 0;
                    DWORD output = 0;
                    memcpy(&raw, source + (SIZE_T)x * bytes_per_pixel,
                           bytes_per_pixel);
                    raw &= pixel_mask;
                    if (!(raw >= colour_low && raw <= colour_high))
                    {
                        const unsigned red = dd_cursor_expand_channel(raw, red_mask);
                        const unsigned green = dd_cursor_expand_channel(raw, green_mask);
                        const unsigned blue = dd_cursor_expand_channel(raw, blue_mask);
                        output = 0xFF000000u | (red << 16u) | (green << 8u) | blue;
                        *any_opaque = 1;
                    }
                    destination[(SIZE_T)y * width + x] = output;
                }
            }
            success = 1;
        }
        __finally
        {
            if (locked)
                LeaveCriticalSection(&surface->cs);
            if (referenced)
                g_dds_vtbl.Release(surface);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        success = 0;
    }
    return success;
}

int DDGetDisplayMode(void);
static BOOL dd_prepare_normal_window_output(RECT* saved_rect);
static void dd_restore_output_request(const RECT* saved_rect);

typedef DWORD (WINAPI *DDRendererProc)(void);
static volatile LONG g_renderer_switch_error;
static volatile LONG g_d3d9_available = -1;

static BOOL dd_compat_forces_d3d9on12(void)
{
    char compatibility[1024] = {0};
    char* context = NULL;
    char* token;

    if (!GetEnvironmentVariableA(
            "__COMPAT_LAYER", compatibility, sizeof(compatibility)))
        return FALSE;

    token = strtok_s(compatibility, " ", &context);
    while (token)
    {
        if (_strcmpi(token, "Win7RTM") == 0)
            return TRUE;
        token = strtok_s(NULL, " ", &context);
    }
    return FALSE;
}

/*
 * The upstream renderer choice is normally latched in dd_CreateEx().  A menu reload already
 * restarts the presentation thread, so a backend change only needs the missing middle step:
 * release the stopped backend and select the new function pointer before dd_SetDisplayMode().
 */
static DDRendererProc dd_resolve_renderer(BOOL* unavailable)
{
    if (unavailable)
        *unavailable = FALSE;

    /* These two are derived values in dd_CreateEx(), not independent live settings. */
    g_config.d3d9on12 = dd_compat_forces_d3d9on12();
    g_config.opengl_core = FALSE;
    if (_strcmpi(g_config.renderer, "direct3d9on12") == 0)
        g_config.d3d9on12 = TRUE;
    else if (_strcmpi(g_config.renderer, "openglcore") == 0)
        g_config.opengl_core = TRUE;

    if (tolower((unsigned char)g_config.renderer[0]) == 'd')
        return d3d9_render_main;
    if (tolower((unsigned char)g_config.renderer[0]) == 's' ||
        tolower((unsigned char)g_config.renderer[0]) == 'g')
        return gdi_render_main;
    if (tolower((unsigned char)g_config.renderer[0]) == 'o')
    {
        if (oglu_load_dll())
            return ogl_render_main;
        if (unavailable)
            *unavailable = TRUE;
        InterlockedExchange(&g_renderer_switch_error, 10);
        return gdi_render_main;
    }
    if (tolower((unsigned char)g_config.renderer[0]) == 'n')
        return null_render_main;

    /* auto: keep the exact startup preference order. */
    if (!IsWine())
    {
        LONG available;
        if (g_ddraw.renderer == d3d9_render_main)
            available = TRUE;
        else
        {
            available = InterlockedExchangeAdd(&g_d3d9_available, 0);
            if (available < 0)
            {
                available = d3d9_is_available() ? TRUE : FALSE;
                InterlockedExchange(&g_d3d9_available, available);
            }
        }
        if (available)
            return d3d9_render_main;
    }
    if (oglu_load_dll())
        return ogl_render_main;
    if (unavailable)
        *unavailable = TRUE;
    InterlockedExchange(&g_renderer_switch_error, 11);
    return gdi_render_main;
}

static BOOL dd_try_set_opengl_pixel_format(BYTE color_bits)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format;

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = color_bits;
    pfd.iLayerType = PFD_MAIN_PLANE;

    format = ChoosePixelFormat(g_ddraw.render.hdc, &pfd);
    if (!format)
    {
        InterlockedExchange(
            &g_renderer_switch_error, 2000 + (LONG)GetLastError());
        return FALSE;
    }
    if (!SetPixelFormat(g_ddraw.render.hdc, format, &pfd))
    {
        InterlockedExchange(
            &g_renderer_switch_error, 3000 + (LONG)GetLastError());
        return FALSE;
    }
    return TRUE;
}

static BOOL dd_prepare_opengl_pixel_format(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format;
    BYTE mode_bits;

    if (!g_ddraw.render.hdc)
    {
        InterlockedExchange(&g_renderer_switch_error, 1);
        return FALSE;
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    format = GetPixelFormat(g_ddraw.render.hdc);
    if (format)
    {
        if (!DescribePixelFormat(
                g_ddraw.render.hdc, format, sizeof(pfd), &pfd))
        {
            InterlockedExchange(
                &g_renderer_switch_error, 4000 + (LONG)GetLastError());
            return FALSE;
        }
        if ((pfd.dwFlags & (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                           PFD_DOUBLEBUFFER)) ==
            (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER))
            return TRUE;

        InterlockedExchange(&g_renderer_switch_error, 2);
        return FALSE;
    }

    mode_bits =
        (BYTE)(g_ddraw.mode.dmBitsPerPel ? g_ddraw.mode.dmBitsPerPel : 32);
    if (dd_try_set_opengl_pixel_format(mode_bits))
        return TRUE;
    if (mode_bits != 32 && dd_try_set_opengl_pixel_format(32))
        return TRUE;

    /* A renderer that never used its cached DC may leave a replaceable common DC behind.  Reacquire
     * once before giving up; CS_OWNDC windows simply return the same stable handle. */
    ReleaseDC(g_ddraw.hwnd, g_ddraw.render.hdc);
    g_ddraw.render.hdc = GetDC(g_ddraw.hwnd);
    if (!g_ddraw.render.hdc)
    {
        InterlockedExchange(
            &g_renderer_switch_error, 5000 + (LONG)GetLastError());
        return FALSE;
    }

    format = GetPixelFormat(g_ddraw.render.hdc);
    if (format)
        return dd_prepare_opengl_pixel_format();
    return dd_try_set_opengl_pixel_format(32);
}

/* Called only after dd_RestoreDisplayMode() has joined the old renderer thread. */
static BOOL dd_switch_renderer(DDRendererProc old_renderer,
                               DDRendererProc* requested_renderer)
{
    BOOL unavailable = FALSE;
    DDRendererProc selected;

    InterlockedExchange(&g_renderer_switch_error, 0);
    selected = dd_resolve_renderer(&unavailable);

    if (requested_renderer)
        *requested_renderer = selected;

    /* The menu never exposes null.  Refuse a hand-edited null transition because its window and
     * headless lifecycle is intentionally startup-only. */
    if (old_renderer == null_render_main || selected == null_render_main)
        return old_renderer == selected;

    if (unavailable)
    {
        g_ddraw.show_driver_warning = TRUE;
        return FALSE;
    }

    if (selected == ogl_render_main && !dd_prepare_opengl_pixel_format())
    {
        g_ddraw.show_driver_warning = TRUE;
        return FALSE;
    }

    /* A renderer command is a full backend recreation even when the function pointer is unchanged:
     * openglcore/opengl and direct3d9on12/direct3d9 share pointers but not device configuration.
     * Releasing both also cleans a stale GL context left by upstream's asynchronous GDI fallback. */
    ogl_release();
    d3d9_release();

    g_ddraw.renderer = selected;
    return TRUE;
}

int DDGetRendererSwitchError(void)
{
    return (int)InterlockedExchangeAdd(&g_renderer_switch_error, 0);
}

/* 0 OpenGL, 1 GDI, 2 Direct3D 9, 3 null/headless, -1 not initialized/unknown. */
int DDGetActiveRenderer(void)
{
    if (!g_ddraw.ref || !g_ddraw.renderer)
        return -1;
    if (g_ddraw.renderer == ogl_render_main)
        return 0;
    if (g_ddraw.renderer == gdi_render_main)
        return 1;
    if (g_ddraw.renderer == d3d9_render_main)
        return 2;
    if (g_ddraw.renderer == null_render_main)
        return 3;
    return -1;
}

/* Effective portable filter after D3D9 has had a chance to downgrade unsupported Lanczos. */
int DDGetActivePortableFilter(void)
{
    int filter;
    const int renderer = DDGetActiveRenderer();
    if (renderer != 1 && renderer != 2)
        return -1;
    filter = g_config.d3d9_filter;
    return filter >= 0 && filter <= 3 ? filter : -1;
}

/*
 * Keep cnc-ddraw's live fake desktop geometry aligned with a validated Hor+
 * canvas. Disciples II clamps its requested windowed mode to
 * GetSystemMetrics() before looking it up in the DirectDraw mode list. Leaving
 * the persisted 1024x768 fallback in memory for a 1280x720 canvas would create
 * the impossible key 1024x720x16. Hor+ also has to advertise its exact custom
 * mode through cnc-ddraw's single injected-resolution slot: fake_mode alone is
 * deliberately not returned by EnumDisplayModes, and the game aborts before
 * DirectDraw initialization when the requested mode is absent. Both changes
 * are process-local and do not rewrite ddraw.ini.
 */
void DDSetGameCanvasMetrics(int width, int height, int inject_resolution)
{
    if (width >= 800 && height >= 600)
    {
        wsprintfA(g_config.fake_mode, "%dx%dx16", width, height);
        if (inject_resolution)
            wsprintfA(g_config.inject_resolution, "%dx%d", width, height);
    }
}

/* A game-side bordered-dialog transition can happen without a renderer config
 * change. Force OpenGL/D3D9 to upload the corresponding raw/decorated frame. */
void DDInvalidateDecorativeFrame(void)
{
    /* Recompose/wake only. Publishing pending layout here could pair a classifier result with the
     * previous primary pixels; actual primary Blt/Flip/DC/Unlock owns that publication. */
    if (!g_ddraw.ref)
        return;

    InterlockedExchange(&g_ddraw.render.surface_updated, TRUE);
    if (g_ddraw.render.sem)
        ReleaseSemaphore(g_ddraw.render.sem, 1, NULL);
}

/*
 * Two presentation-only zoom stages share the existing final-viewport integration:
 *
 *  1. DisciplesGL 1.90 "Stretch windows" is a centered, caller-persisted 0..100 percent. At 100%
 *     it crops a wide canvas to its 600-pixel-high, 800x600-equivalent view and enlarges that crop
 *     back to the output. Zero disables this base stage.
 *  2. DisciplesGL 2.0.2 Ctrl+Wheel remains a process-local 1.0x..8.0x extra zoom around the cursor.
 *
 * Keep this state in the wrapper-owned bridge rather than an upstream cnc-ddraw patch:
 * wndproc/render backends only call the narrow integration functions below. Fixed point makes the
 * UI thread writes and renderer-thread reads atomic on 32-bit Windows.
 */
static volatile LONG g_window_stretch_percent = 100; /* 0 (off) .. 100 */
static volatile LONG g_simple_zoom_1000 = 1000; /* extra zoom: 1.0 .. 8.0 */
static volatile LONG g_simple_anchor_x_100000 = 50000; /* point inside the rendered viewport */
static volatile LONG g_simple_anchor_y_100000 = 50000;

static int dd_window_stretch_layout_active(void)
{
    int content_width = 0;
    int content_height = 0;
    int wide_battle = 0;

    /*
     * v1.90 IsZoomed required borders.active: "Stretch windows" enlarged fixed-size dialogs but
     * never the strategic map. The decorative compositor owns the equivalent address-gated,
     * per-surface signal in C4dll-R. Ctrl+Wheel is deliberately not gated and is composed below as
     * the independent extra stage.
     */
    return horplus_get_decor_layout(
        &content_width, &content_height, &wide_battle) != 0;
}

/* Exact integer source crop from DisciplesGL 1.90 Config::CalcZoomed. The original truncates each
 * positive reduction through DWORD independently; at 1366x768 this makes 100% exactly 1068x600. */
int DDCalcWindowStretchCrop(
    int game_width, int game_height, int percent,
    int* left, int* top, int* crop_width, int* crop_height)
{
    int width = game_width;
    int height = game_height;

    if (percent < 0)
        percent = 0;
    else if (percent > 100)
        percent = 100;

    if (percent > 0 && game_width > 0 && game_height > 0)
    {
        const float aspect = (float)game_width / (float)game_height;
        if (aspect >= 4.0f / 3.0f)
        {
            const float fixed_width = 600.0f * aspect;
            width = game_width - (int)(DWORD)(
                ((float)game_width - fixed_width) * (float)percent * 0.01f);
            height = game_height - (int)(DWORD)(
                ((float)game_height - 600.0f) * (float)percent * 0.01f);
        }
        else
        {
            const float fixed_height = 800.0f / aspect;
            width = game_width - (int)(DWORD)(
                ((float)game_width - 800.0f) * (float)percent * 0.01f);
            height = game_height - (int)(DWORD)(
                ((float)game_height - fixed_height) * (float)percent * 0.01f);
        }
    }

    if (game_width <= 0 || game_height <= 0)
    {
        width = game_width;
        height = game_height;
    }
    else
    {
        if (width < 1)
            width = 1;
        else if (width > game_width)
            width = game_width;
        if (height < 1)
            height = 1;
        else if (height > game_height)
            height = game_height;
    }

    if (left)
        *left = game_width > width ? (game_width - width) >> 1 : 0;
    if (top)
        *top = game_height > height ? (game_height - height) >> 1 : 0;
    if (crop_width)
        *crop_width = width;
    if (crop_height)
        *crop_height = height;

    return game_width > 0 && game_height > 0 &&
        (width != game_width || height != game_height);
}

/* The renderer consumes only the published surface layout, so scene classification and crop are
 * paired with the pixels which caused the primary-surface notification. */
int DDGetWindowStretchCrop(
    int game_width, int game_height,
    int* left, int* top, int* crop_width, int* crop_height)
{
    const LONG percent = InterlockedExchangeAdd(&g_window_stretch_percent, 0);

    if (percent > 0 && dd_window_stretch_layout_active())
        return DDCalcWindowStretchCrop(
            game_width, game_height, (int)percent,
            left, top, crop_width, crop_height);

    if (left)
        *left = 0;
    if (top)
        *top = 0;
    if (crop_width)
        *crop_width = game_width;
    if (crop_height)
        *crop_height = game_height;
    return 0;
}

static double dd_window_stretch_zoom(int game_width, int game_height)
{
    int crop_width = game_width;
    int crop_height = game_height;

    if (!DDGetWindowStretchCrop(
            game_width, game_height, NULL, NULL, &crop_width, &crop_height))
        return 1.0;

    if ((double)game_width / (double)game_height >= 4.0 / 3.0)
        return crop_height > 0 ? (double)game_height / crop_height : 1.0;

    return crop_width > 0 ? (double)game_width / crop_width : 1.0;
}

/* The caller owns persistence; changing the preset also restores a deterministic centered view. */
void DDSetWindowStretchPercent(int percent)
{
    if (percent < 0)
        percent = 0;
    else if (percent > 100)
        percent = 100;

    InterlockedExchange(&g_window_stretch_percent, percent);
    InterlockedExchange(&g_simple_zoom_1000, 1000);
    InterlockedExchange(&g_simple_anchor_x_100000, 50000);
    InterlockedExchange(&g_simple_anchor_y_100000, 50000);

    InterlockedExchange(&g_ddraw.render.clear_screen, TRUE);
    if (g_ddraw.render.sem)
        ReleaseSemaphore(g_ddraw.render.sem, 1, NULL);
}

int DDGetWindowStretchPercent(void)
{
    return (int)InterlockedExchangeAdd(&g_window_stretch_percent, 0);
}

int DDIsWindowStretchActive(void)
{
    return DDGetWindowStretchCrop(
        g_ddraw.width, g_ddraw.height, NULL, NULL, NULL, NULL);
}

int DDGetSimpleZoomExtra1000(void)
{
    return (int)InterlockedExchangeAdd(&g_simple_zoom_1000, 0);
}

/* Menu-side status: any base or extra zoom means the final presentation is no longer true 1:1. */
int DDGetSimpleZoom1000(void)
{
    const double base = dd_window_stretch_zoom(g_ddraw.width, g_ddraw.height);
    const double extra = (double)DDGetSimpleZoomExtra1000() / 1000.0;
    return (int)(base * extra * 1000.0 + 0.5);
}

/*
 * Exact extra-zoom wheel policy from DisciplesGL 2.0.2:
 *   Ctrl+wheel up   : +0.1
 *   Ctrl+wheel down : -0.4
 *   clamp           : 1.0 .. 8.0
 * Return nonzero only when Ctrl+Wheel was consumed. DisciplesGL also had a game-side isWheel hook
 * that suppressed the VK_UP/VK_DOWN map movement generated by its window procedure. Keeping the
 * wheel as a real wheel message avoids that address-dependent companion hook here.
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
        real_ScreenToClient(hwnd, &pt);
        {
            const LONG viewport_x =
                g_ddraw.child_window_exists ? 0 : g_ddraw.render.viewport.x;
            const LONG viewport_y =
                g_ddraw.child_window_exists ? 0 : g_ddraw.render.viewport.y;
            const LONG viewport_w =
                g_ddraw.child_window_exists ? g_ddraw.width : g_ddraw.render.viewport.width;
            const LONG viewport_h =
                g_ddraw.child_window_exists ? g_ddraw.height : g_ddraw.render.viewport.height;
            if (viewport_w > 0 && viewport_h > 0)
            {
                double anchor_x = (double)(pt.x - viewport_x) / (double)viewport_w;
                double anchor_y = (double)(pt.y - viewport_y) / (double)viewport_h;
                if (anchor_x < 0.0) anchor_x = 0.0;
                if (anchor_x > 1.0) anchor_x = 1.0;
                if (anchor_y < 0.0) anchor_y = 0.0;
                if (anchor_y > 1.0) anchor_y = 1.0;
                InterlockedExchange(&g_simple_anchor_x_100000,
                    (LONG)(anchor_x * 100000.0));
                InterlockedExchange(&g_simple_anchor_y_100000,
                    (LONG)(anchor_y * 100000.0));
            }
        }
        InterlockedExchange(&g_simple_zoom_1000, zoom);

        InterlockedExchange(&g_ddraw.render.clear_screen, TRUE);
        if (g_ddraw.render.sem)
            ReleaseSemaphore(g_ddraw.render.sem, 1, NULL);
        return 1;
    }

    return 0;
}

/* Undo the extra destination zoom, then map the normal output into the exact source crop. */
void DDApplySimpleZoomMouse(int* x, int* y, int game_width, int game_height)
{
    const LONG zoom_i = InterlockedExchangeAdd(&g_simple_zoom_1000, 0);
    int crop_left = 0;
    int crop_top = 0;
    int crop_width = game_width;
    int crop_height = game_height;

    if (!x || !y || game_width <= 0 || game_height <= 0)
        return;

    DDGetWindowStretchCrop(
        game_width, game_height,
        &crop_left, &crop_top, &crop_width, &crop_height);
    if (zoom_i == 1000 && crop_width == game_width && crop_height == game_height)
        return;

    {
        const double extra_zoom = (double)zoom_i / 1000.0;
        const double anchor_x =
            (double)InterlockedExchangeAdd(&g_simple_anchor_x_100000, 0) / 100000.0;
        const double anchor_y =
            (double)InterlockedExchangeAdd(&g_simple_anchor_y_100000, 0) / 100000.0;
        const double game_anchor_x = anchor_x * (double)game_width;
        const double game_anchor_y = anchor_y * (double)game_height;
        double logical_x = game_anchor_x + ((double)*x - game_anchor_x) / extra_zoom;
        double logical_y = game_anchor_y + ((double)*y - game_anchor_y) / extra_zoom;

        logical_x = (double)crop_left + logical_x * (double)crop_width / game_width;
        logical_y = (double)crop_top + logical_y * (double)crop_height / game_height;
        *x = (int)logical_x;
        *y = (int)logical_y;
        if (*x < 0) *x = 0;
        if (*x >= game_width) *x = game_width - 1;
        if (*y < 0) *y = 0;
        if (*y >= game_height) *y = game_height - 1;
    }
}

/* Expand only the v2.0.2 Ctrl+Wheel destination rectangle. The v1.90 Stretch windows stage is an
 * integer source crop, applied by the renderer before interpolation. */
void DDApplySimpleZoomViewport(int bottom_origin, int* x, int* y, int* width, int* height)
{
    const LONG zoom_i = InterlockedExchangeAdd(&g_simple_zoom_1000, 0);

    if (!x || !y || !width || !height || zoom_i == 1000 || *width <= 0 || *height <= 0)
        return;

    const double extra_zoom = (double)zoom_i / 1000.0;
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
    const double output_anchor_x = (double)base_x + anchor_x * (double)base_w;
    const double output_anchor_y = (double)base_y + anchor_y * (double)base_h;
    *x = (int)(output_anchor_x + ((double)base_x - output_anchor_x) * extra_zoom);
    *y = (int)(output_anchor_y + ((double)base_y - output_anchor_y) * extra_zoom);
    *width = (int)((double)base_w * extra_zoom);
    *height = (int)((double)base_h * extra_zoom);
}

static BOOL dd_reload_config(
    BOOL preserve_output_size,
    BOOL preserve_display_mode,
    BOOL clear_pending_display_mode,
    BOOL renderer_changed)
{
    DDRendererProc old_renderer;
    DDRendererProc requested_renderer = NULL;
    BOOL renderer_switch_ok = TRUE;

    TRACE("%s [%p]\n", __FUNCTION__, _ReturnAddress());

    if (!g_ddraw.ref || !g_ddraw.hwnd || !g_ddraw.width)
        return FALSE;

    old_renderer = g_ddraw.renderer;
    LONG saved_left = g_config.window_rect.left;
    LONG saved_top = g_config.window_rect.top;
    LONG saved_width = g_config.window_rect.right;
    LONG saved_height = g_config.window_rect.bottom;
    BOOL saved_windowed = g_config.windowed;
    BOOL saved_fullscreen = g_config.fullscreen;
    BOOL saved_toggle_borderless = g_config.toggle_borderless;
    BOOL saved_toggle_upscaled = g_config.toggle_upscaled;
    BOOL saved_singlecpu = g_config.singlecpu;
    int saved_maxgameticks = g_config.maxgameticks;

    /* Use the old backend/config while returning an exclusive device to the desktop and joining
     * its render thread.  Ordinary shader/viewport reloads keep the existing upstream path. */
    if (renderer_changed)
        dd_RestoreDisplayMode();

    cfg_load();
    /* Cursor ownership depends on unlocked absolute coordinates, but it is deliberately not
     * persisted as a user setting.  A live menu reload re-reads ddraw.ini and could otherwise
     * restore legacy devmode=false mid-process, resurrecting cnc-ddraw's synthetic-centre and
     * inactive-mouse suppression branches immediately after any Video menu change. */
    if (InterlockedExchangeAdd(&g_c4_d2_cursor_ownership, 0) != 0)
        g_config.devmode = TRUE;
    /* singlecpu is startup-latched. A later live renderer reload must not expose its pending
     * next-start value to DLL_THREAD_ATTACH while existing threads retain the startup policy. */
    g_config.singlecpu = saved_singlecpu;
    /* maxgameticks owns live limiter handles/deadlines. Only DDSetMaxGameTicksLive may change it;
     * a generic Video reload must not publish an INI value without reconfiguring that state. */
    g_config.maxgameticks = saved_maxgameticks;
    if (g_config.window_rect.left == -32000)
        g_config.window_rect.left = saved_left;
    if (g_config.window_rect.top == -32000)
        g_config.window_rect.top = saved_top;
    if (preserve_output_size)
    {
        g_config.window_rect.right = saved_width;
        g_config.window_rect.bottom = saved_height;
    }
    if (preserve_display_mode)
    {
        g_config.windowed = saved_windowed;
        g_config.fullscreen = saved_fullscreen;
        g_config.toggle_borderless = saved_toggle_borderless;
        g_config.toggle_upscaled = saved_toggle_upscaled;
    }
    if (clear_pending_display_mode)
    {
        /*
         * An explicit menu choice has already been written to the effective ini section. Do not
         * let window_state/upscaled_state left by an earlier hotkey overwrite it in cfg_save().
         */
        g_config.window_state = -1;
        g_config.upscaled_state = -1;
    }

    if (renderer_changed)
    {
        renderer_switch_ok =
            dd_switch_renderer(old_renderer, &requested_renderer);
        if (!renderer_switch_ok)
        {
            /* Preflight failed before old resources were released.  Keep the working backend and
             * restart its stopped thread; the requested INI value remains saved for next launch. */
            g_ddraw.renderer = old_renderer;
            requested_renderer = old_renderer;
        }
    }

    /*
     * C4dll-R owns its normal-window menu. Migrated/custom cnc-ddraw configs may contain
     * remove_menu=true; letting dd_SetDisplayMode consume our freshly attached bar would create an
     * attach/remove loop on every health poll. Preserve the setting, but suppress it for our relayout.
     */
    BOOL saved_remove_menu = g_config.remove_menu;
    g_config.remove_menu = FALSE;
    dd_SetDisplayMode(0, 0, 0, 0);
    g_config.remove_menu = saved_remove_menu;

    if (renderer_changed && g_ddraw.renderer != requested_renderer)
        renderer_switch_ok = FALSE;

    if (g_mouse_locked)
    {
        mouse_unlock();
        mouse_lock();
    }

    InterlockedExchange(&g_ddraw.render.clear_screen, TRUE);
    if (g_ddraw.render.sem)
        ReleaseSemaphore(g_ddraw.render.sem, 1, NULL);
    RedrawWindow(g_ddraw.hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);

    return renderer_switch_ok;
}

void DDReloadConfig(void)
{
    dd_reload_config(FALSE, FALSE, FALSE, FALSE);
}

/*
 * Menu reload policy is two-dimensional. A width/height choice applies the newly written output
 * while preserving a live hotkey mode; a display-mode choice applies the newly written mode while
 * preserving a manual resize; every other choice preserves both. Explicit mode selection also
 * supersedes pending window_state/upscaled_state left by an earlier F4/Alt+Enter.
 */
int DDReloadConfigForMenu(int output_size_changed,
                          int display_mode_changed,
                          int renderer_changed)
{
    return dd_reload_config(
        output_size_changed ? FALSE : TRUE,
        display_mode_changed ? FALSE : TRUE,
        display_mode_changed ? TRUE : FALSE,
        renderer_changed ? TRUE : FALSE);
}

/*
 * Recompute the window/client/viewport after C4dll-R attaches or detaches its Win32 menu.
 * Unlike DDReloadConfig(), this deliberately keeps the live mode selected by cnc-ddraw's
 * Alt+Enter hotkey instead of re-reading and restoring the persisted ddraw.ini state.
 */
void DDRelayoutCurrentMode(void)
{
    RECT saved_output_rect;
    BOOL prepared_normal_output = FALSE;

    TRACE("%s [%p]\n", __FUNCTION__, _ReturnAddress());

    if (!g_ddraw.ref || !g_ddraw.hwnd || !g_ddraw.width)
        return;

    if (DDGetDisplayMode() == 0)
        prepared_normal_output = dd_prepare_normal_window_output(&saved_output_rect);

    BOOL saved_remove_menu = g_config.remove_menu;
    g_config.remove_menu = FALSE;
    dd_SetDisplayMode(0, 0, 0, 0);
    g_config.remove_menu = saved_remove_menu;
    if (prepared_normal_output)
        dd_restore_output_request(&saved_output_rect);

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

/* 0 = normal window, 1 = borderless, 2 = exclusive, -1 = renderer not ready. */
int DDGetDisplayMode(void)
{
    if (!g_ddraw.ref || !g_ddraw.hwnd || !g_ddraw.width)
        return -1;

    return !g_config.windowed ? 2 : (g_config.fullscreen ? 1 : 0);
}

/*
 * C4dll-R <=1.4 persisted exclusive as windowed=false, fullscreen=true. cnc-ddraw treats
 * fullscreen as a desktop-output modifier, so leaving that stale live bit set makes the first
 * Alt+Enter land in borderless. Normalize only this unambiguous legacy pair.
 */
void DDNormalizeLegacyExclusive(void)
{
    if (!g_config.windowed && g_config.fullscreen)
    {
        g_config.fullscreen = FALSE;
        g_config.toggle_borderless = FALSE;
    }
}

/* Logical DirectDraw surface width requested by the game, not the output/window width. */
int DDGetGameWidth(void)
{
    return g_ddraw.ref ? (int)g_ddraw.width : 0;
}

/* Logical DirectDraw surface height requested by the game, paired with DDGetGameWidth. */
int DDGetGameHeight(void)
{
    return g_ddraw.ref ? (int)g_ddraw.height : 0;
}

/*
 * Live geometry after cnc-ddraw has applied output scaling, desktop borderless sizing and any
 * exclusive display-mode fallback. The menu uses this instead of duplicating renderer arithmetic.
 */
int DDGetScaleMetrics(
    int* game_width,
    int* game_height,
    int* output_width,
    int* output_height,
    int* viewport_x,
    int* viewport_y,
    int* viewport_width,
    int* viewport_height)
{
    if (!g_ddraw.ref || !g_ddraw.hwnd || !g_ddraw.width || !g_ddraw.height)
        return 0;

    if (game_width)
        *game_width = (int)g_ddraw.width;
    if (game_height)
        *game_height = (int)g_ddraw.height;
    if (output_width)
        *output_width = (int)g_ddraw.render.width;
    if (output_height)
        *output_height = (int)g_ddraw.render.height;
    if (viewport_x)
        *viewport_x = (int)g_ddraw.render.viewport.x;
    if (viewport_y)
        *viewport_y = (int)g_ddraw.render.viewport.y;
    if (viewport_width)
        *viewport_width = (int)g_ddraw.render.viewport.width;
    if (viewport_height)
        *viewport_height = (int)g_ddraw.render.viewport.height;

    return 1;
}

/*
 * The output size that cnc-ddraw currently owns in g_config.  Unlike render.width/height this
 * remains the normal-window request while borderless uses the desktop. A manual resize updates
 * this pair immediately. The third result is true only when cfg_save()'s destination cannot be
 * shadowed by the currently active per-process section on the next start.
 */
int DDGetOutputConfig(int* width, int* height, int* persists_next_start)
{
    int persists = 0;
    if (!g_ddraw.ref)
        return 0;

    if (width)
        *width = (int)g_config.window_rect.right;
    if (height)
        *height = (int)g_config.window_rect.bottom;
    /*
     * cfg_save() writes savesettings=1 to [ddraw], and every other nonzero value to
     * [process_file_name] -- not necessarily to the currently selected game_section. Be
     * conservative when a /2, /wine, or other active override can shadow that destination.
     */
    if (g_config.save_settings == 1) {
        if (!g_config.game_section[0])
            persists = 1;
    } else if (g_config.save_settings != 0) {
        if (g_config.game_section[0] &&
            lstrcmpiA(g_config.game_section,
                      g_config.process_file_name) == 0) {
            persists = 1;
        }
    }
    if (persists_next_start)
        *persists_next_start = persists;
    return 1;
}

/*
 * Read/write the same effective section as cnc-ddraw. A matching per-process section overrides
 * [ddraw]. Explicit menu writes must target that effective section; cfg_save() has its own,
 * narrower destination policy handled separately above.
 */
int DDReadConfigString(
    const char* key,
    const char* default_value,
    char* value,
    unsigned int capacity)
{
    if (!key || !value || !capacity || !g_config.ini_path[0])
        return 0;

    if (g_config.game_section[0] &&
        GetPrivateProfileStringA(
            g_config.game_section, key, "", value, capacity,
            g_config.ini_path) > 0)
        return 1;

    GetPrivateProfileStringA(
        "ddraw", key, default_value ? default_value : "", value, capacity,
        g_config.ini_path);
    return 1;
}

int DDWriteConfigString(const char* key, const char* value)
{
    const char* section;
    if (!key || !value || !g_config.ini_path[0])
        return 0;

    section = g_config.game_section[0] ? g_config.game_section : "ddraw";
    return WritePrivateProfileStringA(
        section, key, value, g_config.ini_path) != FALSE;
}

/* Exact reset destination: preserve non-active profiles and never hide a failed write. */
int DDGetConfigWriteTarget(char* path, unsigned int path_capacity,
                          char* section, unsigned int section_capacity)
{
    const char* target = g_config.game_section[0] ? g_config.game_section : "ddraw";
    if (!path || !section || !g_config.ini_path[0] ||
        path_capacity <= strlen(g_config.ini_path) || section_capacity <= strlen(target))
        return 0;
    lstrcpynA(path, g_config.ini_path, path_capacity);
    lstrcpynA(section, target, section_capacity);
    return 1;
}

/*
 * Enable the single-owner cursor model only after featuremenu has validated the exact MNS/SMNS
 * executable. DisciplesGL's model assumes unlocked, absolute pointer coordinates. Pin devmode for
 * this process only: cursor ownership is an implementation detail, not a user setting, and must not
 * rewrite an existing ddraw.ini merely because the validated runtime path became available.
 */
void DDEnableD2CursorOwnership(void)
{
    int count;

    if (!g_config.devmode)
        g_config.devmode = TRUE;

    /* Repair at most once any negative Win32 show-count left by an earlier hook/DLL. Region changes
     * are handle-only after this point, so there is no recurring counter-balancing algorithm. */
    count = real_ShowCursor(TRUE);
    if (count > 0)
    {
        real_ShowCursor(FALSE);
    }
    else
    {
        while (count < 0)
            count = real_ShowCursor(TRUE);
    }

    InterlockedExchange(&g_c4_d2_cursor_ownership, 1);
}

/*
 * A startup migration may repair width/height after cfg_load() but before DirectDraw is created.
 * Mirror the completed transactional write into memory so cfg_save() cannot resurrect the stale
 * pair at process exit. Normal live menu changes still use cfg_load through DDReloadConfigForMenu.
 */
void DDSetOutputConfigMemory(int width, int height)
{
    g_config.window_rect.right = width;
    g_config.window_rect.bottom = height;
}

/*
 * Fullscreen modes replace cnc-ddraw's live render geometry with the monitor dimensions.  On the
 * return trip a width=0/height=0 configuration can therefore inherit that fullscreen geometry
 * instead of becoming a visibly smaller normal window.  Keep the last real normal-window client
 * size when there is one; for a process that started fullscreen, derive it from the persisted
 * output request (or the game canvas for the 0x0 automatic request).  The selected live size is
 * clamped to the current monitor work area, including the caption/frame and C4dll-R menu.
 *
 * The g_config override is deliberately temporary.  In particular, a fitted automatic window must
 * not turn persisted width=0/height=0 into a fixed output when cnc-ddraw later calls cfg_save().
 */
static LONG g_last_normal_output_width;
static LONG g_last_normal_output_height;
static WINDOWPLACEMENT g_last_normal_window_placement;
static volatile LONG g_restore_normal_placement_pending;

/* Same process-exit path as upstream's default SC_CLOSE. Call only after
 * confirmation and successful INI writes, never from DllMain. For reset, the
 * persisted defaults are authoritative: cfg_save must not restore old geometry. */
void DDExitClientAfterSettingsChange(int discard_old_window_state)
{
    if (discard_old_window_state)
        g_config.save_settings = 0; /* memory only, not savesettings=0 in the INI */
    if (g_config.terminate_process)
        g_config.terminate_process = 2;
    ExitProcess(0);
}

static LONG dd_read_config_long(const char* key, LONG fallback)
{
    char fallback_text[32] = {0};
    char value[32] = {0};
    char* end = NULL;
    long parsed;

    wsprintfA(fallback_text, "%ld", fallback);
    if (!DDReadConfigString(key, fallback_text, value, sizeof(value)))
        return fallback;

    parsed = strtol(value, &end, 0);
    return end != value ? (LONG)parsed : fallback;
}

static void dd_get_current_work_area(RECT* work)
{
    MONITORINFO info;
    HMONITOR monitor;

    if (!work)
        return;

    SetRect(work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    monitor = MonitorFromWindow(g_ddraw.hwnd, MONITOR_DEFAULTTONEAREST);
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoA(monitor, &info))
        *work = info.rcWork;
    else
        SystemParametersInfoA(SPI_GETWORKAREA, 0, work, 0);
}

static void dd_fit_normal_output_to_work_area(LONG* width, LONG* height, const RECT* work)
{
    RECT chrome = {0, 0, 100, 100};
    LONG capacity_width;
    LONG capacity_height;
    LONG extra_width = 0;
    LONG extra_height = 0;

    if (!width || !height || !work || *width <= 0 || *height <= 0)
        return;

    if (AdjustWindowRectEx(&chrome, WS_OVERLAPPEDWINDOW, TRUE, 0))
    {
        extra_width = (chrome.right - chrome.left) - 100;
        extra_height = (chrome.bottom - chrome.top) - 100;
    }
    capacity_width = (work->right - work->left) - extra_width;
    capacity_height = (work->bottom - work->top) - extra_height;
    if (capacity_width <= 0 || capacity_height <= 0)
        return;

    if (*width > capacity_width || *height > capacity_height)
    {
        const LONGLONG requested_width = *width;
        const LONGLONG requested_height = *height;
        if (requested_width * capacity_height > requested_height * capacity_width)
        {
            *width = capacity_width;
            *height = (LONG)(requested_height * capacity_width / requested_width);
        }
        else
        {
            *height = capacity_height;
            *width = (LONG)(requested_width * capacity_height / requested_height);
        }
    }
}

static BOOL dd_prepare_normal_window_output(RECT* saved_rect)
{
    RECT work;
    RECT frame;
    LONG width;
    LONG height;
    LONG outer_width;
    LONG outer_height;

    if (!saved_rect || !g_ddraw.hwnd || !g_ddraw.width || !g_ddraw.height)
        return FALSE;

    *saved_rect = g_config.window_rect;
    width = g_last_normal_output_width;
    height = g_last_normal_output_height;
    if (width <= 0 || height <= 0)
    {
        width = dd_read_config_long("width", 0);
        height = dd_read_config_long("height", 0);
        if (width <= 0 || height <= 0)
        {
            width = (LONG)g_ddraw.width;
            height = (LONG)g_ddraw.height;
        }
    }

    dd_get_current_work_area(&work);
    dd_fit_normal_output_to_work_area(&width, &height, &work);
    if (width <= 0 || height <= 0)
        return FALSE;

    g_last_normal_output_width = width;
    g_last_normal_output_height = height;
    g_config.window_rect.right = width;
    g_config.window_rect.bottom = height;

    /* g_config stores the desired client origin.  Center the corresponding outer rectangle in the
     * work area so a side/top taskbar cannot leave the new normal window looking fullscreen. */
    SetRect(&frame, 0, 0, width, height);
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, TRUE, 0);
    outer_width = frame.right - frame.left;
    outer_height = frame.bottom - frame.top;
    g_config.window_rect.left =
        work.left + ((work.right - work.left) - outer_width) / 2 - frame.left;
    g_config.window_rect.top =
        work.top + ((work.bottom - work.top) - outer_height) / 2 - frame.top;
    return TRUE;
}

static void dd_restore_output_request(const RECT* saved_rect)
{
    if (saved_rect)
        g_config.window_rect = *saved_rect;
}

/* Capture the whole Win32 placement, not merely the client size. Every recognized F4/Alt+Enter
 * transition is routed through the C4-owned toggle below, so the snapshot is taken only on the
 * normal-to-fullscreen edge and cannot be overwritten by the following key-up. */
void DDCaptureNormalWindowPlacement(void)
{
    WINDOWPLACEMENT placement;

    if (DDGetDisplayMode() != 0 || !g_ddraw.hwnd)
        return;

    ZeroMemory(&placement, sizeof(placement));
    placement.length = sizeof(placement);
    if (real_GetWindowPlacement(g_ddraw.hwnd, &placement))
        g_last_normal_window_placement = placement;

    if (g_ddraw.render.width > 0 && g_ddraw.render.height > 0)
    {
        g_last_normal_output_width = (LONG)g_ddraw.render.width;
        g_last_normal_output_height = (LONG)g_ddraw.render.height;
    }
}

/*
 * F4 is owned by C4dll-R so it also works with an existing ddraw.ini that predates the hotkey.
 * Remember which kind of fullscreen was left, but always make the return trip a real normal
 * window. Temporarily selecting cnc-ddraw's borderless toggle gives util_toggle_fullscreen()
 * the right transition without reloading the user's configuration. The feature-menu layer observes
 * the completed transition and persists the final mode for the next process start.
 */
void DDToggleWindowedMode(void)
{
    static LONG last_fullscreen_mode = 1; /* safest first transition: normal -> borderless */
    const int mode = DDGetDisplayMode();
    BOOL saved_toggle_borderless;
    RECT saved_output_rect;
    BOOL prepared_normal_output = FALSE;

    if (mode < 0)
        return;

    saved_toggle_borderless = g_config.toggle_borderless;

    if (mode == 0)
    {
        DDCaptureNormalWindowPlacement();
        if (InterlockedExchangeAdd(&last_fullscreen_mode, 0) == 2)
        {
            g_config.fullscreen = FALSE;
            g_config.toggle_borderless = FALSE;
        }
        else
        {
            g_config.toggle_borderless = TRUE;
        }
    }
    else
    {
        prepared_normal_output = dd_prepare_normal_window_output(&saved_output_rect);
        InterlockedExchange(&last_fullscreen_mode, mode);
        if (mode == 1)
        {
            g_config.toggle_borderless = TRUE;
        }
        else
        {
            /* Old configs sometimes encoded exclusive as false+true. Normalize the live state so
             * cnc-ddraw's exclusive -> window transition cannot land in borderless instead. */
            g_config.fullscreen = FALSE;
            g_config.toggle_borderless = FALSE;
        }
    }

    util_toggle_fullscreen();
    if (prepared_normal_output)
        dd_restore_output_request(&saved_output_rect);
    /*
     * Keep the live transition policy coherent with the mode we just entered. In particular, after
     * F4 creates borderless from a config whose persisted toggle_borderless was false, Alt+Enter
     * must return to the window rather than unexpectedly jumping from borderless to exclusive.
     * Once back in a normal window restore the user's prior preference for the next transition.
     */
    {
        const int after = DDGetDisplayMode();
        if (mode != 0 && after == 0 &&
            g_last_normal_window_placement.length == sizeof(WINDOWPLACEMENT))
            InterlockedExchange(&g_restore_normal_placement_pending, 1);
        if (after == 1)
            g_config.toggle_borderless = TRUE;
        else if (after == 2)
            g_config.toggle_borderless = FALSE;
        else
            g_config.toggle_borderless = saved_toggle_borderless;
    }
}

/*
 * DisciplesGL restores the exact WINDOWPLACEMENT saved before fullscreen. C4dll-R first lets
 * cnc-ddraw rebuild the normal client and lets featuremenu reattach its menu; only then can the
 * original outer placement be restored without subtracting the menu height from the game image.
 */
void DDCompleteWindowedModeToggle(void)
{
    WINDOWPLACEMENT placement;

    if (InterlockedExchange(&g_restore_normal_placement_pending, 0) == 0 ||
        DDGetDisplayMode() != 0 || !g_ddraw.hwnd)
        return;

    placement = g_last_normal_window_placement;
    if (placement.length != sizeof(WINDOWPLACEMENT))
        return;

    real_SetWindowPlacement(g_ddraw.hwnd, &placement);
    RedrawWindow(g_ddraw.hwnd, NULL, NULL,
                 RDW_FRAME | RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
}

/*
 * Handle fullscreen hotkeys before the game WndProc: changing the renderer from inside that WndProc
 * can briefly update chrome but leave the fullscreen-sized nested relayout in control. Route both
 * fixed C4dll-R shortcuts and configured alternatives through DDToggleWindowedMode. Plain F4 is
 * wrapper-owned; Alt+F4 is not.
 */
int DDIsWindowModeToggleHotkey(int code, WPARAM key, LPARAM hook_flags)
{
    const DWORD flags = (DWORD)hook_flags;
    const BOOL alt_down = (flags & (1u << 29)) != 0;
    const BOOL key_down = (flags & (1u << 31)) == 0;
    const BOOL key_triggered = (flags & (1u << 30)) == 0;

    if (code < 0 || !key || !key_down || !key_triggered)
        return 0;

    if (key == VK_F4)
        return alt_down ? 0 : 1;

    if ((key == VK_RETURN || key == (WPARAM)g_config.hotkeys.toggle_fullscreen) && alt_down)
        return 1;

    return g_config.hotkeys.toggle_fullscreen2 &&
        key == (WPARAM)g_config.hotkeys.toggle_fullscreen2 ? 1 : 0;
}

void DDTakeScreenshot(void)
{
    TRACE("%s [%p]\n", __FUNCTION__, _ReturnAddress());
    if (!g_ddraw.ref || !g_ddraw.primary)
        return;

    /* Keep screenshots consistent with the presented frame.  Use a shallow
     * surface descriptor with the presentation-only scratch pointer; the
     * game-owned primary surface and its lifetime remain untouched. */
    EnterCriticalSection(&g_ddraw.cs);
    if (g_ddraw.ref && g_ddraw.primary)
    {
        IDirectDrawSurfaceImpl presented = *g_ddraw.primary;
        presented.surface = (void*)DDGetDecoratedSurface(
            g_ddraw.primary->surface,
            g_ddraw.primary->width,
            g_ddraw.primary->height,
            g_ddraw.primary->pitch,
            g_ddraw.primary->bpp,
            g_config.rgb555);
        /* Debug breadcrumbs for skewed/discolored screenshot reports: the exact
         * geometry the encoder sees, plus the decor decision.  A diagonal shear
         * means pitch!=true stride here; swapped colors mean bpp/rgb555 lies. */
        if (featuremenu_debug_enabled())
        {
            int cw = -1, ch = -1, wide = -1;
            const int decor = horplus_get_decor_layout(&cw, &ch, &wide);
            char line[192];
            _snprintf(line, sizeof(line) - 1,
                "[shot] primary %lux%lu pitch=%lu bpp=%lu bytes_pp=%lu rgb555=%d "
                "decor=%d(%dx%d wide=%d) surface=%p scratch=%p",
                (unsigned long)g_ddraw.primary->width,
                (unsigned long)g_ddraw.primary->height,
                (unsigned long)g_ddraw.primary->pitch,
                (unsigned long)g_ddraw.primary->bpp,
                (unsigned long)g_ddraw.primary->bytes_pp,
                (int)g_config.rgb555,
                decor, cw, ch, wide,
                g_ddraw.primary->surface, presented.surface);
            line[sizeof(line) - 1] = '\0';
            featuremenu_debug_log_line(line);
        }
        ss_take_screenshot(&presented);
    }
    LeaveCriticalSection(&g_ddraw.cs);
}
