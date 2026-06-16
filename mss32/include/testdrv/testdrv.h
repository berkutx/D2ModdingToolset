/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Installer / entry point. DllMain calls installEarly() as early as possible (the
 * boot byte-patches must land before the game's message pump runs) and install()
 * once the module handle and game version are known (UI-state reporter, auto-nav,
 * network trace hooks, relay bridge). Everything is runtime-gated by D2TESTDRV_*
 * env vars; compile-gated by D2_TESTDRV (no macro -> the whole system is absent).
 */

#ifndef TESTDRV_TESTDRV_H
#define TESTDRV_TESTDRV_H

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {

/** Apply the early boot byte-patches (skip-intro / fg-flag). Call first in DllMain. */
void installEarly();

/** Install the UI-state reporter + auto-nav executor and window tag; the network
 * interception layer if D2TESTDRV_NET_INTERCEPT; and the relay bridge if
 * D2TESTDRV_RELAY_BRIDGE. `self` is the mss32 module (sent to the relay in Hello). */
void install(HMODULE self);

} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_TESTDRV_H
