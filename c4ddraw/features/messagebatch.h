#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Experimental, default ON, restart-latched [menu] messageBatching=0 opts out.
 * Exact EXE only. Extra batching never synthesizes notifications. The selected
 * dispatch seam is also shared with netnotify, independently of this toggle. */
typedef LRESULT (*MessageBatchSelectedDispatch)(const MSG*, LRESULT(WINAPI*)(const MSG*), void*);
typedef int (*MessageBatchRecoveryRequested)(const char*);
/* The owner supplies optional recovery; batching does not depend on its module. */
#ifdef __cplusplus
void messagebatch_install(HWND hwnd, const char* iniPath,
                          MessageBatchRecoveryRequested recoveryRequested = nullptr,
                          MessageBatchSelectedDispatch recoveryDispatch = nullptr);
#else
void messagebatch_install(HWND hwnd, const char* iniPath,
                          MessageBatchRecoveryRequested recoveryRequested,
                          MessageBatchSelectedDispatch recoveryDispatch);
#endif
int messagebatch_dispatch_ready(void);
/* Observe lifecycle boundaries before the renderer/menu can consume them. */
void messagebatch_window_event(HWND hwnd, UINT message, WPARAM wParam);
#ifdef __cplusplus
}
#endif
