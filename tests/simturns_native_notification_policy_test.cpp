#include "simturns/native_notification_policy.h"
#include "simturns/native_apply_fence.h"
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

using hooks::netintercept::NativeReceiveResult;
using hooks::netintercept::nativeDispatchResult;
using namespace hooks::simturns;

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void exactNotification()
{
    char name[36] = ".?AVCConnectMsg@@";
    check(isPregameConnectNotification(0xffff, 48, name, true, 1), "exact client notification rejected");
    for (const auto type : {0u, 1u, 0xfffeu, 0x10000u})
        check(!isPregameConnectNotification(type, 48, name, true, 1), "wrong type admitted");
    for (const auto length : {0u, 43u, 44u, 47u, 49u, 0x80000u})
        check(!isPregameConnectNotification(0xffff, length, name, true, 1), "wrong frame size admitted");
    check(!isPregameConnectNotification(0xffff, 48, name, false, 1), "server receiver admitted");
    for (const auto sender : {0u, 2u, 0xffffffffu})
        check(!isPregameConnectNotification(0xffff, 48, name, true, sender), "foreign sender admitted");
    name[sizeof(".?AVCConnectMsg@@") - 1] = 'X'; // Replace the required terminating NUL.
    check(!isPregameConnectNotification(0xffff, 48, name, true, 1), "longer class prefix admitted");
    std::memset(name, 'A', sizeof(name));
    check(!isPregameConnectNotification(0xffff, 48, name, true, 1), "unterminated class admitted");
    for (const auto* other : {".?AVCPlayerListMsg@@", ".?AVCBeginTurnMsg@@", ".?AVCTurnInfoMsg@@",
                             ".?AVCMenusAnsInfoMsg@@", ".?AVCCmdMoveStackMsg@@"}) {
        std::memset(name, 0, sizeof(name));
        std::memcpy(name, other, std::strlen(other) + 1);
        check(!isPregameConnectNotification(0xffff, 48, name, true, 1), "other native class admitted");
    }
}

void onlyNormallyUnhandledNotification()
{
    check(nativeDispatchResult(1) == NativeReceiveResult::Applied
              && nativeDispatchResult(12) == NativeReceiveResult::Applied,
          "positive native handler count changed");
    check(nativeDispatchResult(0) == NativeReceiveResult::Unhandled, "zero handler count is not explicit");
    check(nativeDispatchResult(-1) == NativeReceiveResult::Failed, "negative native result admitted");
    check(resolveLobbyNativeReceiveResult(NativeReceiveResult::Unhandled, true, 17, 17)
              == NativeReceiveResult::Filtered, "same-pregame notification not retired");
    for (const auto result : {NativeReceiveResult::Applied, NativeReceiveResult::Filtered,
                              NativeReceiveResult::Failed}) {
        check(resolveLobbyNativeReceiveResult(result, true, 17, 17) == result,
              "exception altered applied, consumed or policy-dropped packet");
    }
    for (const bool allowed : {false, true}) {
        for (const auto before : {0u, 17u}) {
            for (const auto after : {0u, 17u, 18u}) {
                const bool accept = allowed && before && before == after;
                check(resolveLobbyNativeReceiveResult(NativeReceiveResult::Unhandled, allowed, before, after)
                          == (accept ? NativeReceiveResult::Filtered : NativeReceiveResult::Failed),
                      "phase/generation boundary weakened");
            }
        }
    }
}

void filteredNotificationKeepsEarlierFence()
{
    NativeApplyFence fence;
    const auto earlierCommand = fence.issue();
    const auto notification = fence.issue();
    const auto barrier = fence.watermark();
    const auto laterPacket = fence.issue();
    check(resolveLobbyNativeReceiveResult(NativeReceiveResult::Unhandled, true, 23, 23)
              == NativeReceiveResult::Filtered, "notification policy failed");
    check(fence.complete(notification), "notification completion rejected");
    check(!fence.reached(barrier), "notification bypassed earlier native command/drain");
    check(fence.complete(laterPacket) && !fence.reached(barrier), "later packet bypassed earlier command");
    check(fence.complete(earlierCommand) && fence.reached(barrier), "earlier command did not release barrier");
    check(!fence.complete(notification), "duplicate notification completion admitted");
}

void exactEarlyJoinRefresh()
{
    PregameJoinSnapshotPolicy policy;
    char name[36] = ".?AVCRefreshInfo@@";
    check(!policy.scenarioStarted(), "fresh binding inherited a scenario boundary");
    for (const auto length : {52u, 55u, 56u, 91u, 105u, 218u, 464u, 535u, 616u, 23896u, 0x7ffffu}) {
        policy.observe(0xffff, length, name, true, 1);
        check(policy.allowsUnhandledRefresh(0xffff, length, name, true, true, 1),
              "valid variable-length pre-scenario join Refresh rejected");
        check(!policy.scenarioStarted(), "Refresh advanced the scenario boundary");
    }
    for (const auto length : {0u, 43u, 44u, 48u, 51u, 0x80000u, 0xffffffffu})
        check(!policy.allowsUnhandledRefresh(0xffff, length, name, true, true, 1),
              "out-of-range Refresh length admitted");
    for (const auto type : {0u, 1u, 0xfffeu, 0x10000u})
        check(!policy.allowsUnhandledRefresh(type, 56, name, true, true, 1),
              "wrong Refresh message type admitted");
    check(!policy.allowsUnhandledRefresh(0xffff, 56, name, false, true, 1),
          "host received join-only Refresh exception");
    check(!policy.allowsUnhandledRefresh(0xffff, 56, name, true, false, 1),
          "server endpoint received client Refresh exception");
    for (const auto sender : {0u, 2u, 0xffffffffu})
        check(!policy.allowsUnhandledRefresh(0xffff, 56, name, true, true, sender),
              "foreign Refresh sender admitted");
    name[sizeof(".?AVCRefreshInfo@@") - 1] = 'X';
    check(!policy.allowsUnhandledRefresh(0xffff, 56, name, true, true, 1),
          "longer Refresh RTTI prefix admitted");
    std::memset(name, 'A', sizeof(name));
    check(!policy.allowsUnhandledRefresh(0xffff, 56, name, true, true, 1),
          "unterminated Refresh RTTI admitted");
    for (const auto* other : {".?AVCConnectMsg@@", ".?AVCPlayerListMsg@@",
                             ".?AVCCmdBeginTurnMsg@@", ".?AVCCmdTurnInfoMsg@@",
                             ".?AVCNewScenarioMsg@@", ".?AVCStartScenarioMsg@@",
                             ".?AVCCmdEraseObjMsg@@", ".?AVCCmdUpdateObjMsg@@",
                             ".?AVCMenusAnsInfoMsg@@", ".?AVCCmdMoveStackMsg@@"}) {
        std::memset(name, 0, sizeof(name));
        std::memcpy(name, other, std::strlen(other) + 1);
        check(!policy.allowsUnhandledRefresh(0xffff, 56, name, true, true, 1),
              "other state/notification class received Refresh exception");
    }
}

void scenarioBoundaryIsAuthoritativeAndPermanent()
{
    char refresh[36] = ".?AVCRefreshInfo@@";
    char connect[36] = ".?AVCConnectMsg@@";
    char beginTurn[36] = ".?AVCCmdBeginTurnMsg@@";
    for (const auto* boundaryClass : {".?AVCNewScenarioMsg@@", ".?AVCStartScenarioMsg@@"}) {
        char boundary[36]{};
        std::memcpy(boundary, boundaryClass, std::strlen(boundaryClass) + 1);
        PregameJoinSnapshotPolicy policy;
        // Wrong route or invalid envelope cannot retire this binding's window.
        for (const auto type : {0u, 1u, 0xfffeu, 0x10000u})
            policy.observe(type, 56, boundary, true, 1);
        for (const auto length : {0u, 43u, 0x80000u, 0xffffffffu})
            policy.observe(0xffff, length, boundary, true, 1);
        for (const auto sender : {0u, 2u, 0xffffffffu})
            policy.observe(0xffff, 56, boundary, true, sender);
        policy.observe(0xffff, 56, boundary, false, 1);
        boundary[std::strlen(boundaryClass)] = 'X';
        policy.observe(0xffff, 56, boundary, true, 1);
        boundary[std::strlen(boundaryClass)] = '\0';
        policy.observe(0xffff, 48, connect, true, 1);
        policy.observe(0xffff, 56, beginTurn, true, 1);
        check(!policy.scenarioStarted()
                  && policy.allowsUnhandledRefresh(0xffff, 56, refresh, true, true, 1),
              "unrelated or invalid boundary closed pre-scenario window");

        // Stage is the boundary: no native completion or handler count is needed.
        const auto length = std::strcmp(boundaryClass, ".?AVCNewScenarioMsg@@") == 0 ? 48u : 53u;
        policy.observe(0xffff, length, boundary, true, 1);
        check(policy.scenarioStarted(), "authoritative scenario stage did not close the window");
        check(!policy.allowsUnhandledRefresh(0xffff, 56, refresh, true, true, 1),
              "post-scenario Refresh was excused");
        policy.observe(0xffff, 48, connect, true, 1);
        policy.observe(0xffff, 23896, refresh, true, 1);
        policy.observe(0xffff, length, boundary, true, 1);
        check(policy.scenarioStarted()
                  && !policy.allowsUnhandledRefresh(0xffff, 23896, refresh, true, true, 1),
              "later traffic reopened a retired window");

        PregameJoinSnapshotPolicy replacement;
        check(!replacement.scenarioStarted()
                  && replacement.allowsUnhandledRefresh(0xffff, 56, refresh, true, true, 1),
              "new binding inherited old scenario state");
        policy.observe(0xffff, length, boundary, true, 1);
        check(!replacement.scenarioStarted(), "old binding changed replacement boundary");
        check(isPregameConnectNotification(0xffff, 48, connect, true, 1),
              "join snapshot boundary changed the separate Connect classifier");
    }
}

void refreshCompletionRequiresSamePregameLifetime()
{
    char refresh[36] = ".?AVCRefreshInfo@@";
    char newScenario[36] = ".?AVCNewScenarioMsg@@";
    for (const bool crossBoundary : {false, true}) {
        PregameJoinSnapshotPolicy policy;
        const bool stagedAllowed = policy.allowsUnhandledRefresh(0xffff, 23896, refresh, true, true, 1);
        check(stagedAllowed, "early Refresh could not stage");
        if (crossBoundary) policy.observe(0xffff, 48, newScenario, true, 1);
        const bool stillAllowed = stagedAllowed && !policy.scenarioStarted();
        for (const auto before : {0u, 17u}) {
            for (const auto after : {0u, 17u, 18u}) {
                const bool accept = stillAllowed && before && before == after;
                check(resolveLobbyNativeReceiveResult(nativeDispatchResult(0), stillAllowed, before, after)
                          == (accept ? NativeReceiveResult::Filtered : NativeReceiveResult::Failed),
                      "Refresh completion crossed a scenario/phase/generation boundary");
                for (const auto result : {NativeReceiveResult::Applied, NativeReceiveResult::Filtered,
                                          NativeReceiveResult::Failed})
                    check(resolveLobbyNativeReceiveResult(result, stillAllowed, before, after) == result,
                          "Refresh exception altered applied, consumed or failed result");
            }
        }
    }

    PregameJoinSnapshotPolicy policy;
    const bool allowed = policy.allowsUnhandledRefresh(0xffff, 56, refresh, true, true, 1);
    const auto resolved = resolveLobbyNativeReceiveResult(nativeDispatchResult(0), allowed, 23, 23);
    check(resolved == NativeReceiveResult::Filtered, "early zero-handler Refresh was not retired");
    // A later UI task consumes the already resolved outcome, not current phase.
    policy.observe(0xffff, 48, newScenario, true, 1);
    check(resolved == NativeReceiveResult::Filtered && policy.scenarioStarted(),
          "later boundary retroactively changed synchronous completion");
    check(resolveLobbyNativeReceiveResult(nativeDispatchResult(0),
              policy.allowsUnhandledRefresh(0xffff, 56, refresh, true, true, 1), 23, 23)
              == NativeReceiveResult::Failed, "later Refresh reused an earlier allowance");
}

void filteredRefreshKeepsEarlierFence()
{
    PregameJoinSnapshotPolicy policy;
    char refresh[36] = ".?AVCRefreshInfo@@";
    NativeApplyFence fence;
    const auto earlierCommand = fence.issue();
    const auto snapshot = fence.issue();
    const auto barrier = fence.watermark();
    const auto laterPacket = fence.issue();
    check(resolveLobbyNativeReceiveResult(nativeDispatchResult(0),
              policy.allowsUnhandledRefresh(0xffff, 23896, refresh, true, true, 1), 31, 31)
              == NativeReceiveResult::Filtered, "Refresh classification failed");
    check(fence.complete(snapshot), "filtered Refresh did not complete its ticket");
    check(!fence.reached(barrier), "filtered Refresh bypassed an earlier native command/drain");
    check(fence.complete(laterPacket) && !fence.reached(barrier),
          "later packet bypassed the native command/drain");
    check(fence.complete(earlierCommand) && fence.reached(barrier),
          "earlier native command/drain did not release the barrier");
    check(!fence.complete(snapshot), "duplicate Refresh completion admitted");
}

void exactEarlyStartupBeginTurn()
{
    PregameJoinSnapshotPolicy policy;
    char name[36] = ".?AVCCmdBeginTurnMsg@@";
    std::uint32_t words[3]{0, 1, 0xa3de0001u};
    const auto allows = [&](std::uint32_t type, std::uint32_t length, bool joinRole,
                            bool clientReceiver, std::uint32_t sender) {
        return policy.allowsUnhandledStartupBeginTurn(type, length, name, words,
                                                       joinRole, clientReceiver, sender);
    };
    check(allows(0xffff, 56, true, true, 1), "exact startup BeginTurn broadcast rejected");
    policy.observe(0xffff, 56, name, true, 1);
    check(!policy.scenarioStarted(), "startup BeginTurn closed the pre-scenario window");
    for (const auto type : {0u, 1u, 0xfffeu, 0x10000u})
        check(!allows(type, 56, true, true, 1), "wrong startup BeginTurn type admitted");
    for (const auto length : {0u, 43u, 44u, 48u, 52u, 55u, 57u, 23896u, 0x80000u})
        check(!allows(0xffff, length, true, true, 1), "non-exact startup BeginTurn size admitted");
    check(!allows(0xffff, 56, false, true, 1), "host received startup BeginTurn exception");
    check(!allows(0xffff, 56, true, false, 1), "server endpoint received startup BeginTurn exception");
    for (const auto sender : {0u, 2u, 0xffffffffu})
        check(!allows(0xffff, 56, true, true, sender), "foreign startup BeginTurn sender admitted");
    for (const auto addressee : {1u, 0xa3de0002u, 0xffffffffu}) {
        words[0] = addressee;
        check(!allows(0xffff, 56, true, true, 1), "directed addressee admitted as startup broadcast");
    }
    words[0] = 0;
    for (const auto sequence : {0u, 2u, 0xffffffffu}) {
        words[1] = sequence;
        check(!allows(0xffff, 56, true, true, 1), "non-startup sequence admitted");
    }
    words[0] = 0xa3de0002u;
    words[1] = 0xffffffffu;
    check(!allows(0xffff, 56, true, true, 1), "exact directed-activation tuple admitted");
    words[0] = 0;
    words[1] = 1;
    words[2] = 0;
    check(!allows(0xffff, 56, true, true, 1), "zero startup active handle admitted");
    for (const auto active : {1u, 0xa3de0001u, 0xffffffffu}) {
        words[2] = active;
        check(allows(0xffff, 56, true, true, 1), "nonzero startup active handle rejected");
    }
    name[sizeof(".?AVCCmdBeginTurnMsg@@") - 1] = 'X';
    check(!allows(0xffff, 56, true, true, 1), "longer BeginTurn RTTI prefix admitted");
    std::memset(name, 'A', sizeof(name));
    check(!allows(0xffff, 56, true, true, 1), "unterminated BeginTurn RTTI admitted");
    for (const auto* other : {".?AVCBeginTurnMsg@@", ".?AVCCmdTurnInfoMsg@@", ".?AVCRefreshInfo@@",
                             ".?AVCNewScenarioMsg@@", ".?AVCStartScenarioMsg@@",
                             ".?AVCCmdMoveStackMsg@@", ".?AVCConnectMsg@@"}) {
        std::memset(name, 0, sizeof(name));
        std::memcpy(name, other, std::strlen(other) + 1);
        check(!allows(0xffff, 56, true, true, 1), "other RTTI received startup BeginTurn exception");
    }
}

void startupBeginTurnCompletionKeepsLifetimeAndFailures()
{
    char name[36] = ".?AVCCmdBeginTurnMsg@@";
    const std::uint32_t words[3]{0, 1, 0xa3de0001u};
    for (const auto* boundaryClass : {".?AVCNewScenarioMsg@@", ".?AVCStartScenarioMsg@@"}) {
        char boundary[36]{};
        std::memcpy(boundary, boundaryClass, std::strlen(boundaryClass) + 1);
        for (const bool crossed : {false, true}) {
            PregameJoinSnapshotPolicy policy;
            const bool staged = policy.allowsUnhandledStartupBeginTurn(0xffff, 56, name, words,
                                                                        true, true, 1);
            check(staged, "early startup BeginTurn could not stage");
            // Model the native nested receive between staging and synchronous completion.
            if (crossed) policy.observe(0xffff, 56, boundary, true, 1);
            const bool allowed = staged && !policy.scenarioStarted();
            check(policy.allowsUnhandledStartupBeginTurn(0xffff, 56, name, words, true, true, 1)
                      == !crossed, "startup BeginTurn ignored the latched scenario boundary");
            for (const auto before : {0u, 41u}) {
                for (const auto after : {0u, 41u, 42u}) {
                    const bool accept = allowed && before && before == after;
                    check(resolveLobbyNativeReceiveResult(nativeDispatchResult(0), allowed, before, after)
                              == (accept ? NativeReceiveResult::Filtered : NativeReceiveResult::Failed),
                          "startup BeginTurn crossed a scenario/lifetime boundary");
                    // A duplicate/invalid startup proof is already Drop/Failed from rxGate.
                    // Class eligibility must never override that failure or a positive handler count.
                    for (const auto result : {NativeReceiveResult::Failed, NativeReceiveResult::Applied,
                                              NativeReceiveResult::Filtered})
                        check(resolveLobbyNativeReceiveResult(result, allowed, before, after) == result,
                              "startup BeginTurn exception rescued a failed proof or changed native result");
                }
            }
        }
    }
}

void filteredStartupBeginTurnKeepsEarlierFence()
{
    PregameJoinSnapshotPolicy policy;
    char name[36] = ".?AVCCmdBeginTurnMsg@@";
    const std::uint32_t words[3]{0, 1, 0xa3de0001u};
    NativeApplyFence fence;
    const auto earlierCommand = fence.issue();
    const auto beginTurn = fence.issue();
    const auto barrier = fence.watermark();
    const auto laterPacket = fence.issue();
    check(resolveLobbyNativeReceiveResult(nativeDispatchResult(0),
              policy.allowsUnhandledStartupBeginTurn(0xffff, 56, name, words, true, true, 1), 43, 43)
              == NativeReceiveResult::Filtered, "startup BeginTurn classification failed");
    check(fence.complete(beginTurn) && !fence.reached(barrier),
          "filtered startup BeginTurn bypassed an earlier native command/drain");
    check(fence.complete(laterPacket) && !fence.reached(barrier),
          "later packet bypassed the startup native barrier");
    check(fence.complete(earlierCommand) && fence.reached(barrier),
          "earlier native completion did not release startup barrier");
    check(!fence.complete(beginTurn), "duplicate startup BeginTurn completion admitted");
}

void exactEarlyJoinGame()
{
    char name[36] = ".?AVCJoinGameMsg@@";
    char boundary[36] = ".?AVCNewScenarioMsg@@";
    const auto bodyFor = [](std::uint32_t nameLength) {
        std::vector<std::uint8_t> body(12 + nameLength, 'x');
        const std::uint32_t player = 0xa3de0001u, lord = 2;
        std::memcpy(body.data(), &player, 4);
        std::memcpy(body.data() + 4, &nameLength, 4);
        if (nameLength) body[8 + nameLength - 1] = 0;
        std::memcpy(body.data() + 8 + nameLength, &lord, 4);
        return body;
    };
    PregameJoinSnapshotPolicy policy;
    const auto valid = bodyFor(6); // Synthetic name; captured frame was 62 bytes.
    const auto allowed = [&](const std::vector<std::uint8_t>& body, std::uint32_t length,
                              bool join = true, bool client = true, std::uint32_t sender = 1) {
        return policy.allowsUnhandledJoinGame(0xffff, length, name, body.data(), body.size(), join, client, sender);
    };
    for (const auto nameLength : {1u, 2u, 6u, 64u, 255u, 256u}) {
        const auto body = bodyFor(nameLength);
        check(allowed(body, 44 + static_cast<std::uint32_t>(body.size())), "valid variable JoinGame rejected");
    }
    check(!allowed(bodyFor(0), 56) && !allowed(bodyFor(257), 313), "out-of-range JoinGame name admitted");
    check(!allowed(valid, 62, false) && !allowed(valid, 62, true, false), "host/server JoinGame admitted");
    for (const auto sender : {0u, 2u, 0xffffffffu})
        check(!allowed(valid, 62, true, true, sender), "foreign JoinGame admitted");
    for (const auto length : {0u, 43u, 44u, 56u, 61u, 63u, 0x80000u, 0xffffffffu})
        check(!allowed(valid, length), "mismatched JoinGame envelope admitted");
    for (std::size_t n = 0; n < valid.size(); ++n) {
        std::vector<std::uint8_t> truncated(valid.begin(), valid.begin() + n);
        check(!allowed(truncated, 62) && !allowed(truncated, 44 + static_cast<std::uint32_t>(n)),
              "truncated JoinGame admitted");
    }
    for (const auto declared : {0u, 1u, 5u, 7u, 256u, 0xffffffffu}) {
        auto body = valid;
        std::memcpy(body.data() + 4, &declared, 4);
        check(!allowed(body, 62), "wrong/overflowing declared name length admitted");
    }
    auto bad = valid;
    std::memset(bad.data(), 0, 4);
    check(!allowed(bad, 62), "zero joined handle admitted");
    bad = valid; bad[13] = 'x';
    check(!allowed(bad, 62), "missing final name NUL admitted");
    bad = valid; bad[9] = 0;
    check(!allowed(bad, 62), "embedded name NUL admitted");
    bad = valid; bad.push_back(0);
    check(!allowed(bad, 63), "trailing JoinGame byte admitted");
    for (const auto lord : {0u, 1u, 2u, 3u, 0xffffffffu}) {
        bad = valid;
        std::memcpy(bad.data() + 14, &lord, 4);
        check(allowed(bad, 62) == (lord <= 2), "JoinGame lord category domain ignored");
    }
    for (const auto type : {0u, 1u, 0xfffeu, 0x10000u})
        check(!policy.allowsUnhandledJoinGame(type, 62, name, valid.data(), valid.size(), true, true, 1),
              "wrong JoinGame type admitted");
    check(!policy.allowsUnhandledJoinGame(0xffff, 62, name, nullptr, valid.size(), true, true, 1),
          "null JoinGame body admitted");
    name[sizeof(".?AVCJoinGameMsg@@") - 1] = 'X';
    check(!allowed(valid, 62), "longer JoinGame RTTI prefix admitted");
    std::memset(name, 'A', sizeof(name));
    check(!allowed(valid, 62), "unterminated JoinGame RTTI admitted");
    for (const auto* other : {".?AVCPlayerListMsg@@", ".?AVCMenusAnsInfoMsg@@", ".?AVCCmdBeginTurnMsg@@"}) {
        std::memset(name, 0, sizeof(name));
        std::memcpy(name, other, std::strlen(other) + 1);
        check(!allowed(valid, 62), "mandatory menu/activation class admitted as JoinGame");
    }
    std::memset(name, 0, sizeof(name));
    std::memcpy(name, ".?AVCJoinGameMsg@@", sizeof(".?AVCJoinGameMsg@@"));
    policy.observe(0xffff, 48, boundary, true, 1);
    check(!allowed(valid, 62), "post-scenario JoinGame zero-handler admitted");
}
}

int main()
{
    try {
        exactNotification(); onlyNormallyUnhandledNotification(); filteredNotificationKeepsEarlierFence();
        exactEarlyJoinRefresh(); scenarioBoundaryIsAuthoritativeAndPermanent();
        refreshCompletionRequiresSamePregameLifetime(); filteredRefreshKeepsEarlierFence();
        exactEarlyStartupBeginTurn(); startupBeginTurnCompletionKeepsLifetimeAndFailures();
        filteredStartupBeginTurnKeepsEarlierFence();
        exactEarlyJoinGame();
        std::cout << "simturns native notification policy: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
