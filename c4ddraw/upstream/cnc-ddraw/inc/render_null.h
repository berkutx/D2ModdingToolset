#ifndef RENDER_NULL_H
#define RENDER_NULL_H

#include <windows.h>

/*
 * Headless null renderer (ddraw.ini: renderer=null). Drains the present signal and paces like the
 * real backends, but draws nothing: no device context, no GL, no blit -> no GPU/display dependency.
 * The game still writes to its DirectDraw surfaces (in memory); we just never present them. Lets the
 * engine run with zero visual cost and without a graphics device (Wine/headless, many instances).
 */
DWORD WINAPI null_render_main(void);

#endif
