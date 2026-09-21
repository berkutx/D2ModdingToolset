#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Experimental, default ON, restart-latched [menu] messageBatching=0 opts out.
 * Exact EXE only. Extra batching never synthesizes notifications. The selected
 * dispatch seam is also shared with netnotify, independently of this toggle. */
void messagebatch_install(HWND hwnd, const char* iniPath);
int messagebatch_dispatch_ready(void);
/* Observe lifecycle boundaries before the renderer/menu can consume them. */
void messagebatch_window_event(HWND hwnd, UINT message, WPARAM wParam);
#ifdef __cplusplus
}
#endif
