#ifndef SIMTURNS_NATIVE_NOTIFICATION_POLICY_H
#define SIMTURNS_NATIVE_NOTIFICATION_POLICY_H

#include "netintercept.h"
#include <cstdint>
#include <cstring>

namespace hooks::simturns {

/** Exact stock connection notification, never a strategic command. The source
 * buffer is already validated and copied by the custom-player receive path.
 * Compare the terminating NUL too: prefixes and longer class names do not match. */
inline bool isPregameConnectNotification(std::uint32_t type, std::uint32_t length,
                                         const char (&messageClass)[36],
                                         bool clientReceiver, std::uint32_t sender) noexcept
{
    constexpr char connectClass[] = ".?AVCConnectMsg@@";
    return clientReceiver && sender == 1 && type == 0xffff && length == 48
        && std::memcmp(messageClass, connectClass, sizeof(connectClass)) == 0;
}

/** Only a normally dispatched zero-handler notification in the same pregame
 * lifetime may retire its ticket without an engine handler. A policy Drop is
 * Failed, not Unhandled, and is never rescued by this exception. */
constexpr netintercept::NativeReceiveResult resolveLobbyNativeReceiveResult(
    netintercept::NativeReceiveResult result, bool allowedNotification,
    std::uint64_t beforeGeneration, std::uint64_t afterGeneration) noexcept
{
    if (result != netintercept::NativeReceiveResult::Unhandled) return result;
    return allowedNotification && beforeGeneration && beforeGeneration == afterGeneration
        ? netintercept::NativeReceiveResult::Filtered : netintercept::NativeReceiveResult::Failed;
}

} // namespace hooks::simturns
#endif
