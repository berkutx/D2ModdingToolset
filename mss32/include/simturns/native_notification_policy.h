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

/** Per-binding boundary, protected by the binding mutex. Stock startup sends
 * object broadcasts to joiners still in the setup menu. Their own NewScenario
 * starts a separate, full snapshot; never excuse an unhandled delta after it.
 * This is NOT a receive filter: only a normally dispatched Unhandled result
 * can be retired, with the no-CMidClient lifetime checked on both sides. */
class PregameJoinSnapshotPolicy {
public:
    void observe(std::uint32_t type, std::uint32_t length,
                 const char (&name)[36], bool clientReceiver, std::uint32_t sender) noexcept
    {
        if (!serverToClient(type, length, clientReceiver, sender)) return;
        constexpr char newScenario[] = ".?AVCNewScenarioMsg@@";
        constexpr char startScenario[] = ".?AVCStartScenarioMsg@@";
        if (std::memcmp(name, newScenario, sizeof(newScenario)) == 0
            || std::memcmp(name, startScenario, sizeof(startScenario)) == 0)
            scenarioStarted_ = true;
    }

    bool allowsUnhandledRefresh(std::uint32_t type, std::uint32_t length,
                                const char (&name)[36], bool joinRole,
                                bool clientReceiver, std::uint32_t sender) const noexcept
    {
        constexpr char refresh[] = ".?AVCRefreshInfo@@";
        return !scenarioStarted_ && joinRole
            // Header (44), scenario ID (4), object count (4). Expansion frames
            // additionally carry an expansion marker; payload size varies.
            && serverToClient(type, length, clientReceiver, sender) && length >= 52
            && std::memcmp(name, refresh, sizeof(refresh)) == 0;
    }

    bool scenarioStarted() const noexcept { return scenarioStarted_; }

private:
    static bool serverToClient(std::uint32_t type, std::uint32_t length,
                               bool clientReceiver, std::uint32_t sender) noexcept
    {
        return clientReceiver && sender == 1 && type == 0xffff
            && length >= 44 && length < 0x80000;
    }
    bool scenarioStarted_{};
};

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
