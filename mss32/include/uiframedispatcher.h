/*
 * Exact-Russobit UI-loop dispatcher for ordered gameplay work.
 * The single sub_5629CA Detour is requested before the ordinary hook
 * transaction; the dispatcher contains no gameplay-mode policy.
 */

#ifndef UIFRAMEDISPATCHER_H
#define UIFRAMEDISPATCHER_H

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

} // namespace uiframedispatcher
} // namespace hooks

#endif // UIFRAMEDISPATCHER_H
