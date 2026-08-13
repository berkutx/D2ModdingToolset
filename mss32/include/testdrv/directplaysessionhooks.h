/*
 * DebugTest support for unattended same-machine DirectPlay sessions.
 * Compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_DIRECTPLAYSESSIONHOOKS_H
#define TESTDRV_DIRECTPLAYSESSIONHOOKS_H

namespace hooks {
namespace testdrv {
namespace directplaysessionhooks {

/** Replace an empty EnumSessions host with 127.0.0.1. This is the only
 * DirectPlay behavior the unattended two-instance harness requires. */
bool install();

} // namespace directplaysessionhooks
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_DIRECTPLAYSESSIONHOOKS_H
