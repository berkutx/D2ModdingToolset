#include "simturns/lobby_wire.h"
#ifdef NDEBUG
#undef NDEBUG // Protocol checks must remain active in the Release test runner.
#endif
#include <cassert>
#include <cstdio>

using namespace hooks::simturns::lobby;

int main() {
    assert(!roomRequiresSimultaneousTurns(nullptr, nullptr));
    assert(!roomRequiresSimultaneousTurns("", ""));
    assert(!roomRequiresSimultaneousTurns("", "0"));
    assert(!roomRequiresSimultaneousTurns("0", "7"));
    assert(roomRequiresSimultaneousTurns("1", "0")); // Unlimited OH is not stock.
    assert(roomRequiresSimultaneousTurns("", "7"));
    assert(roomRequiresSimultaneousTurns("invalid", "0")); // Never silently downgrade.
    Envelope arm;
    arm.operation = Operation::Arm; arm.room = 0x01020304; arm.epoch = 0xf1f2f3f4;
    arm.role = 1; arm.mergeDay = 7;
    const std::vector<std::uint8_t> expected{
        1, 0, 4, 3, 2, 1, 0xf4, 0xf3, 0xf2, 0xf1, 1, 7, 0, 0, 0};
    assert(encode(arm) == expected);
    Envelope decoded;
    assert(decode(expected.data(), expected.size(), decoded));
    assert(decoded.room == arm.room && decoded.epoch == arm.epoch
           && decoded.role == 1 && decoded.mergeDay == 7);
    for (std::size_t n = 0; n < expected.size(); ++n)
        assert(!decode(expected.data(), n, decoded));
    auto invalid = expected;
    invalid.push_back(0); assert(!decode(invalid.data(), invalid.size(), decoded));
    for (unsigned field : {0u, 1u, 10u}) {
        invalid = expected; invalid[field] = 0xff;
        assert(!decode(invalid.data(), invalid.size(), decoded));
    }
    invalid = expected;
    for (unsigned i = 6; i < 10; ++i) invalid[i] = 0;
    assert(!decode(invalid.data(), invalid.size(), decoded));
    for (std::uint32_t day : {0u, 2u, 7u, 30u}) {
        arm.mergeDay = day; assert(!encode(arm).empty());
    }
    for (std::uint32_t day : {1u, 31u, 0x7fffffffu, 0xffffffffu}) {
        arm.mergeDay = day; assert(encode(arm).empty());
    }
    arm.room = 0; arm.mergeDay = 0; arm.role = 2;
    assert(!encode(arm).empty()); // RoomsPlugin room zero is not an absent-room sentinel.

    Envelope ack;
    ack.operation = Operation::ArmAck; ack.epoch = 1;
    for (auto status : {0, 1}) {
        ack.status = static_cast<std::uint8_t>(status);
        const auto bytes = encode(ack);
        assert(bytes.size() == headerSize + 1 && decode(bytes.data(), bytes.size(), decoded));
    }
    ack.status = 2; assert(encode(ack).empty());

    Envelope frame;
    frame.operation = Operation::Frame; frame.epoch = 1;
    frame.frame = {4, 0, 0, 0, 8, 0, 0, 0};
    const auto minimal = encode(frame);
    assert(minimal.size() == headerSize + 8 && decode(minimal.data(), minimal.size(), decoded));
    assert(decoded.frame == frame.frame);
    frame.frame.push_back(0); assert(encode(frame).empty()); // No second frame or trailing byte.
    frame.frame.assign(maxFrameSize, 0);
    frame.frame[2] = 1; // LE length = 65536, four prefix bytes excluded.
    assert(encode(frame).size() == headerSize + maxFrameSize);
    frame.frame.push_back(0); assert(encode(frame).empty());

    Envelope abort;
    abort.operation = Operation::Abort; abort.epoch = 1;
    for (unsigned reason = 1; reason <= 5; ++reason) {
        abort.reason = static_cast<AbortReason>(reason);
        const auto bytes = encode(abort);
        assert(bytes.size() == headerSize + 2 && decode(bytes.data(), bytes.size(), decoded));
        assert(decoded.reason == abort.reason);
    }
    abort.reason = static_cast<AbortReason>(6); assert(encode(abort).empty());
    std::puts("simturns lobby wire: PASS");
}
