#include "simturns/native_notification_policy.h"
#include "simturns/native_apply_fence.h"
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

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
}

int main()
{
    try {
        exactNotification(); onlyNormallyUnhandledNotification(); filteredNotificationKeepsEarlierFence();
        std::cout << "simturns native notification policy: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
