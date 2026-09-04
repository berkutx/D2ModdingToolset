#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Experimental, default ON, restart-latched [menu] messageBatching=0 opts out.
 * Exact EXE only. No MSS patches, synthetic notifications or limiter changes. */
void messagebatch_install(HWND hwnd, const char* iniPath);
/* Observe lifecycle boundaries before the renderer/menu can consume them. */
void messagebatch_window_event(HWND hwnd, UINT message, WPARAM wParam);
#ifdef __cplusplus
}
#endif
