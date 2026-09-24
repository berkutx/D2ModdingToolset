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

    // Stock broadcasts the host's entry into the scenario while a joiner can
    // still be in CMenuLobby. Only CMidClient registers CJoinGame; menu-owned
    // PlayerList/MenusAnsInfo and directed activation remain mandatory.
    // Body: player ID, NUL-inclusive name length, encoded name, lord category ID.
    bool allowsUnhandledJoinGame(std::uint32_t type, std::uint32_t length,
                                 const char (&name)[36], const std::uint8_t* body,
                                 std::size_t bodySize, bool joinRole,
                                 bool clientReceiver, std::uint32_t sender) const noexcept
    {
        constexpr char joinGame[] = ".?AVCJoinGameMsg@@";
        if (scenarioStarted_ || !joinRole
            || !serverToClient(type, length, clientReceiver, sender)
            || std::memcmp(name, joinGame, sizeof(joinGame)) != 0
            || !body || bodySize < 13 || bodySize != length - 44)
            return false;
        std::uint32_t player{}, nameLength{};
        std::memcpy(&player, body, sizeof(player));
        std::memcpy(&nameLength, body + 4, sizeof(nameLength));
        // Compare by subtraction: untrusted lengths must never wrap into a fit.
        // The stock string serializer uses a 256-byte temporary buffer.
        if (!player || !nameLength || nameLength > 256 || nameLength != bodySize - 12)
            return false;
        std::uint32_t lordCategory{};
        std::memcpy(&lordCategory, body + 8 + nameLength, sizeof(lordCategory));
        if (lordCategory > 2) return false; // Stock LLordCategory: mage/warrior/diplomat.
        const auto* encodedName = body + 8;
        return encodedName[nameLength - 1] == 0
            && std::memchr(encodedName, 0, nameLength - 1) == nullptr;
    }

    // The host's first broadcast also reaches the join menu before its own
    // scenario. rxGate must still validate/latch the exact startup identity;
    // a rejected or duplicate proof returns Failed and is never rescued here.
    bool allowsUnhandledStartupBeginTurn(std::uint32_t type, std::uint32_t length,
                                        const char (&name)[36],
                                        const std::uint32_t (&words)[3], bool joinRole,
                                        bool clientReceiver, std::uint32_t sender) const noexcept
    {
        constexpr char beginTurn[] = ".?AVCCmdBeginTurnMsg@@";
        return !scenarioStarted_ && joinRole && length == 56
            && serverToClient(type, length, clientReceiver, sender)
            && std::memcmp(name, beginTurn, sizeof(beginTurn)) == 0
            && words[0] == 0 && words[1] == 1 && words[2] != 0;
    }

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
