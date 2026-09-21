#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Inert loop-depth hook, installed before EXE entry. No worker, callbacks,
 * network messages or file hashing from DllMain. */
void netnotify_bootstrap(void);
/* First GUI dispatch, after DLL initialization. Exact EXE and unmodified sites
 * only; default on, [menu] networkWakeRecovery=0 opts out at next restart. */
void netnotify_install(HWND hwnd, const char* iniPath);
int netnotify_requested(const char* iniPath);
/* Called before any renderer/menu consumer. Returns 1 for our private token,
 * which must not reach the game's registered-message dispatcher. */
int netnotify_window_event(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
/* Shared native DispatchMessage seam. Calls the original exactly once; only a
 * validated private token at the outer boundary becomes a native net message. */
LRESULT netnotify_dispatch(const MSG* msg, LRESULT(WINAPI* original)(const MSG*), void* kernel);
#ifdef __cplusplus
}
#endif
