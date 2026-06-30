#include <windows.h>
#include "dd.h"
#include "fps_limiter.h"
#include "config.h"
#include "render_null.h"

/*
 * Null renderer. Same wait-on-present-semaphore + fps pacing loop as the GDI backend, but with the
 * blit removed: present nothing. No HDC, no OpenGL context, no surface copy. The game's Flip/Blt just
 * signal the render semaphore (fire-and-forget, exactly as with GDI), so draining it here keeps the
 * engine running normally while nothing is ever drawn or shown.
 */
DWORD WINAPI null_render_main(void)
{
    fpsl_init();

    DWORD timeout = g_config.minfps > 0 ? g_ddraw.minfps_tick_len : INFINITE;

    while (g_ddraw.render.run &&
        (g_config.minfps < 0 || WaitForSingleObject(g_ddraw.render.sem, timeout) != WAIT_FAILED) &&
        g_ddraw.render.run)
    {
        fpsl_frame_start();

        /* headless: present nothing */

        if (!g_ddraw.render.run)
            break;

        fpsl_frame_end();
    }

    return 0;
}
