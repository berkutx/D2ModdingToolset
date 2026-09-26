/*
 * Exact-Russobit UI-loop dispatcher shared by gameplay and the optional
 * D2_TESTDRV harness. Either consumer can request the single sub_5629CA
 * Detour before the ordinary hook transaction. Queued game work runs before
 * the test observer; the dispatcher contains no gameplay-mode policy.
 */

#ifndef UIFRAMEDISPATCHER_H
#define UIFRAMEDISPATCHER_H

#ifdef D2_TESTDRV
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace hooks {

struct HookInfo;

namespace uiframedispatcher {

/** Read-only exact-Russobit fingerprint/byte preflight and process-lifetime
 * request. Duplicate requests are success; no executable memory is changed. */
bool request();
bool requested();

/** Hook descriptor consumed once by the ordinary MSS Detours transaction. */
HookInfo hookInfo();

/** Publish successful completion of that ordinary transaction. */
void markInstalled();
bool installed();

#ifdef D2_TESTDRV
using DebugFrameCallback = void (*)(HWND window);

/** Register the removable D2_TESTDRV observer. One process-lifetime callback is
 * allowed; registering the same callback twice is idempotent. Production
 * ordered work always retains the first position in the natural frame. */
bool setDebugFrameCallback(DebugFrameCallback callback);
#endif

} // namespace uiframedispatcher
} // namespace hooks

#endif // UIFRAMEDISPATCHER_H
