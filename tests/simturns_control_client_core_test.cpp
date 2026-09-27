/*
 * Dependency-free transcripts for the transport-independent simultaneous-turn
 * client FSM. The tests exercise the portable v8 wire contract and keep all
 * engine and transport adapters outside this translation unit.
 */

#include "simturns/control_client_core.h"
#include "simturns/turn_context.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using hooks::simturns::ControlClientCore;
using hooks::simturns::ControlFailure;
using hooks::simturns::ControlFailureKind;
using hooks::simturns::ControlInboundEvent;
using hooks::simturns::ControlInboundKind;
using hooks::simturns::ControlOutboundFrame;
using hooks::simturns::ControlOutboundKind;
using hooks::simturns::ControlPhase;
using hooks::simturns::LocalHandleBinding;
using hooks::simturns::Role;
using hooks::simturns::SimTurnsSessionOptions;
using hooks::simturns::TurnMode;
using hooks::simturns::EngineActionContextScope;
using hooks::simturns::ScopedEngineActionContext;
using hooks::simturns::TurnGrant;
using hooks::simturns::initializeTurnContextFromSessionPlan;
using hooks::simturns::installTurnGrant;
using hooks::simturns::maxEngineDay;
using hooks::simturns::resetTurnContext;
using hooks::simturns::resolveTurnGrant;
namespace protocol = hooks::simturns::protocol;

constexpr std::uint32_t epoch = 0x51u;
constexpr std::uint32_t hostHandle = 0xa3de0001u;
constexpr std::uint32_t joinHandle = 0xa3de0002u;
constexpr std::uint32_t mergeDay = 3u;
constexpr std::uint32_t hostLease1 = 0x101u;
constexpr std::uint32_t hostLease2 = 0x102u;
constexpr std::uint32_t joinLease1 = 0x201u;
constexpr std::uint32_t joinLease2 = 0x202u;

[[noreturn]] void fail(const char* expression,
                       const char* file,
                       int line,
                       const std::string& detail = {})
{
    std::ostringstream message;
    message << file << ':' << line << ": requirement failed: " << expression;
    if (!detail.empty())
        message << " (" << detail << ')';
    throw std::runtime_error(message.str());
}

#define REQUIRE(expression)                                                        \
    do {                                                                           \
        if (!(expression))                                                         \
            fail(#expression, __FILE__, __LINE__);                                 \
    } while (false)

void requireContains(const std::string& actual,
                     const std::string& expected,
                     const char* file,
                     int line)
{
    if (actual.find(expected) == std::string::npos) {
        fail("diagnostic contains expected text", file, line,
             "expected '" + expected + "', got '" + actual + "'");
    }
}

#define REQUIRE_CONTAINS(actual, expected)                                         \
    requireContains((actual), (expected), __FILE__, __LINE__)

void accepted(bool result, const std::string& error, const char* operation)
{
    if (!result)
        fail(operation, __FILE__, __LINE__, error);
    REQUIRE(error.empty());
}

std::uint16_t readU16(const protocol::Bytes& bytes, std::size_t offset)
{
    REQUIRE(offset + 2 <= bytes.size());
    return static_cast<std::uint16_t>(bytes[offset])
           | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t readU32(const protocol::Bytes& bytes, std::size_t offset)
{
    REQUIRE(offset + 4 <= bytes.size());
    return static_cast<std::uint32_t>(bytes[offset])
           | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
           | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
           | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void appendU32(protocol::Bytes& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

protocol::Frame inbound(protocol::Op op,
                        std::initializer_list<std::uint32_t> words,
                        std::uint16_t flags = 0)
{
    protocol::Frame frame;
    frame.op = op;
    frame.flags = flags;
    for (const std::uint32_t word : words)
        appendU32(frame.payload, word);
    return frame;
}

protocol::Bytes encoded(protocol::Op op,
                        std::initializer_list<std::uint32_t> words)
{
    protocol::Bytes payload;
    for (const std::uint32_t word : words)
        appendU32(payload, word);
    return protocol::encodeFrame(op, payload);
}

void expectOutbound(const ControlOutboundFrame& frame,
                    ControlOutboundKind expectedKind,
                    protocol::Op expectedOp,
                    std::initializer_list<std::uint32_t> expectedWords)
{
    REQUIRE(frame.kind == expectedKind);
    REQUIRE(frame.frame.size() >= 8);
    REQUIRE(static_cast<std::size_t>(readU32(frame.frame, 0)) + 4
            == frame.frame.size());
    REQUIRE(static_cast<protocol::Op>(readU16(frame.frame, 4)) == expectedOp);
    REQUIRE(readU16(frame.frame, 6) == 0);
    REQUIRE(frame.frame.size() == 8 + expectedWords.size() * 4);
    std::size_t offset = 8;
    for (const std::uint32_t word : expectedWords) {
        REQUIRE(readU32(frame.frame, offset) == word);
        offset += 4;
    }
}

struct Fixture
{
    explicit Fixture(Role selectedRole)
        : selectedRole(selectedRole)
    {
        core.configure(SimTurnsSessionOptions{true, selectedRole});
        REQUIRE(core.phase() == ControlPhase::Dormant);
        std::string error;
        accepted(core.activate(error), error, "activate control client");
        REQUIRE(core.phase() == ControlPhase::AwaitingSessionPlan);
    }

    std::uint32_t local() const
    {
        return selectedRole == Role::Host ? hostHandle : joinHandle;
    }

    ControlClientCore core;
    Role selectedRole;
};

struct Pair
{
    Fixture host{Role::Host};
    Fixture join{Role::Join};
};

template <typename Prepare>
ControlOutboundFrame queueOutbound(Fixture& fixture, Prepare prepare)
{
    ControlOutboundFrame frame;
    std::string error;
    accepted(prepare(frame, error), error, "prepare outbound frame");
    fixture.core.commitQueued(frame);
    return frame;
}

template <typename Prepare>
ControlOutboundFrame writeOutbound(Fixture& fixture, Prepare prepare)
{
    ControlOutboundFrame frame = queueOutbound(fixture, prepare);
    fixture.core.completeWritten(frame);
    return frame;
}

ControlInboundEvent acceptFrame(Fixture& fixture,
                                const protocol::Frame& frame,
                                ControlInboundKind expectedKind)
{
    ControlInboundEvent event;
    ControlFailure failure;
    if (!fixture.core.acceptInbound(frame, event, failure)) {
        fail("accept inbound frame", __FILE__, __LINE__, failure.message);
    }
    REQUIRE(failure.kind == ControlFailureKind::None);
    REQUIRE(failure.message.empty());
    REQUIRE(event.kind == expectedKind);
    return event;
}

void rejectFrame(Fixture& fixture,
                 const protocol::Frame& frame,
                 ControlFailureKind expectedKind,
                 const std::string& expectedText)
{
    ControlInboundEvent event;
    ControlFailure failure;
    REQUIRE(!fixture.core.acceptInbound(frame, event, failure));
    REQUIRE(event.kind == ControlInboundKind::None);
    REQUIRE(failure.kind == expectedKind);
    REQUIRE_CONTAINS(failure.message, expectedText);
}

protocol::Frame sessionPlan(TurnMode mode = TurnMode::Simultaneous,
                            std::uint32_t targetMergeDay = mergeDay,
                            std::uint32_t firstLease = hostLease1,
                            std::uint32_t secondLease = joinLease1,
                            std::uint32_t firstHandle = hostHandle,
                            std::uint32_t secondHandle = joinHandle)
{
    return inbound(protocol::Op::SessionPlan,
                   {epoch, static_cast<std::uint32_t>(mode), firstHandle,
                    secondHandle, targetMergeDay, firstLease, secondLease});
}

void publishLocalHandle(Fixture& fixture)
{
    REQUIRE(fixture.core.bindLocalHandle(fixture.local())
            == LocalHandleBinding::Stored);
    ControlOutboundFrame frame;
    REQUIRE(fixture.core.prepareStoredLocalPlayerHandle(frame));
    expectOutbound(frame, ControlOutboundKind::LocalPlayerHandle,
                   protocol::Op::LocalPlayerHandle, {fixture.local()});
    fixture.core.commitQueued(frame);
    fixture.core.completeWritten(frame);
    REQUIRE(fixture.core.localHandleSent());
}

void acceptSimultaneousPlan(Fixture& fixture)
{
    const ControlInboundEvent event = acceptFrame(
        fixture, sessionPlan(), ControlInboundKind::SessionPlan);
    REQUIRE(event.sessionPlan.epoch == epoch);
    REQUIRE(event.sessionPlan.mode == TurnMode::Simultaneous);
    REQUIRE(event.sessionPlan.hostHandle == hostHandle);
    REQUIRE(event.sessionPlan.joinHandle == joinHandle);
    REQUIRE(event.sessionPlan.mergeDay == mergeDay);
    REQUIRE(event.sessionPlan.hostLease == hostLease1);
    REQUIRE(event.sessionPlan.joinLease == joinLease1);
    REQUIRE(fixture.core.mode() == TurnMode::Simultaneous);
    REQUIRE(fixture.core.epoch() == epoch);
    REQUIRE(fixture.core.mergeDay() == mergeDay);
    REQUIRE(fixture.core.phase() == ControlPhase::Bootstrapping);
}

protocol::EngineAction action(std::uint32_t actionId,
                              protocol::EngineActionKind kind,
                              std::uint32_t playerHandle,
                              std::uint32_t day,
                              std::uint32_t lease = 0)
{
    return protocol::EngineAction{epoch, actionId, kind, playerHandle, day, lease};
}

protocol::Frame actionFrame(const protocol::EngineAction& value)
{
    return inbound(protocol::Op::EngineAction,
                   {value.epoch, value.actionId,
                    static_cast<std::uint32_t>(value.kind),
                    value.playerHandle, value.day, value.lease});
}

protocol::EngineAction acceptAction(Fixture& fixture,
                                    const protocol::EngineAction& expected)
{
    const ControlInboundEvent event = acceptFrame(
        fixture, actionFrame(expected), ControlInboundKind::EngineAction);
    REQUIRE(event.engineAction.epoch == expected.epoch);
    REQUIRE(event.engineAction.actionId == expected.actionId);
    REQUIRE(event.engineAction.kind == expected.kind);
    REQUIRE(event.engineAction.playerHandle == expected.playerHandle);
    REQUIRE(event.engineAction.day == expected.day);
    REQUIRE(event.engineAction.lease == expected.lease);
    return event.engineAction;
}

ControlOutboundFrame queueActionResult(Fixture& fixture,
                                       const protocol::EngineAction& acceptedAction,
                                       bool success)
{
    ControlOutboundFrame frame = queueOutbound(
        fixture,
        [&](ControlOutboundFrame& outbound, std::string& error) {
            return fixture.core.prepareActionResult(acceptedAction, success,
                                                    outbound, error);
        });
    expectOutbound(frame, ControlOutboundKind::ActionResult,
                   protocol::Op::ActionResult,
                   {epoch, acceptedAction.actionId,
                    static_cast<std::uint32_t>(acceptedAction.kind),
                    success ? 1u : 0u});
    return frame;
}

void writeActionResult(Fixture& fixture,
                       const protocol::EngineAction& acceptedAction,
                       bool success = true)
{
    const ControlOutboundFrame frame = queueActionResult(
        fixture, acceptedAction, success);
    fixture.core.completeWritten(frame);
}

void beginBootstrap(Fixture& fixture)
{
    publishLocalHandle(fixture);
    acceptSimultaneousPlan(fixture);
    const ControlOutboundFrame activated = writeOutbound(
        fixture,
        [&](ControlOutboundFrame& outbound, std::string& error) {
            return fixture.core.prepareSessionActivated(outbound, error);
        });
    expectOutbound(activated, ControlOutboundKind::SessionActivated,
                   protocol::Op::SessionActivated, {});
}

void finishBootstrap(Fixture& fixture)
{
    if (fixture.selectedRole == Role::Host) {
        const auto apply = acceptAction(
            fixture, action(1, protocol::EngineActionKind::ApplyTurnStart,
                            joinHandle, 1, joinLease1));
        writeActionResult(fixture, apply);
    } else {
        const ControlOutboundFrame began = writeOutbound(
            fixture,
            [&](ControlOutboundFrame& outbound, std::string& error) {
                return fixture.core.prepareBootstrapBeginTurnApplied(
                    joinHandle, 1, outbound, error);
            });
        expectOutbound(began, ControlOutboundKind::BootstrapBeginTurnApplied,
                       protocol::Op::BootstrapBeginTurnApplied,
                       {joinHandle, 1});
        const ControlOutboundFrame complete = writeOutbound(
            fixture,
            [&](ControlOutboundFrame& outbound, std::string& error) {
                return fixture.core.prepareBootstrapComplete(
                    joinHandle, 1, outbound, error);
            });
        expectOutbound(complete, ControlOutboundKind::BootstrapComplete,
                       protocol::Op::BootstrapComplete, {joinHandle, 1});
    }

    acceptFrame(fixture, inbound(protocol::Op::BootstrapCommitted,
                                 {joinHandle, 1}),
                ControlInboundKind::BootstrapCommitted);
    const ControlOutboundFrame commit = writeOutbound(
        fixture,
        [&](ControlOutboundFrame& outbound, std::string& error) {
            return fixture.core.prepareBootstrapCommitApplied(
                joinHandle, 1, outbound, error);
        });
    expectOutbound(commit, ControlOutboundKind::BootstrapCommitApplied,
                   protocol::Op::BootstrapCommitApplied, {joinHandle, 1});

    acceptFrame(fixture, inbound(protocol::Op::BootstrapOperational,
                                 {joinHandle, 1}),
                ControlInboundKind::BootstrapOperational);
    const ControlOutboundFrame operational = writeOutbound(
        fixture,
        [&](ControlOutboundFrame& outbound, std::string& error) {
            return fixture.core.prepareBootstrapOperationalApplied(
                joinHandle, 1, outbound, error);
        });
    expectOutbound(operational,
                   ControlOutboundKind::BootstrapOperationalApplied,
                   protocol::Op::BootstrapOperationalApplied,
                   {joinHandle, 1});
    REQUIRE(fixture.core.phase() == ControlPhase::BootstrapPrepared);

    const ControlInboundEvent released = acceptFrame(
        fixture, inbound(protocol::Op::BootstrapReleased, {joinHandle, 1}),
        ControlInboundKind::BootstrapReleased);
    std::string error;
    accepted(fixture.core.ackBootstrapReleased(released.bootstrapReleased, error),
             error, "ack BootstrapReleased");
    REQUIRE(fixture.core.phase() == ControlPhase::Active);
}

void bootstrap(Fixture& fixture)
{
    beginBootstrap(fixture);
    finishBootstrap(fixture);
}

void bootstrap(Pair& pair)
{
    bootstrap(pair.host);
    bootstrap(pair.join);
}

void claimAndWriteObserved(Fixture& fixture, std::uint32_t expectedLease)
{
    std::uint32_t lease = 0;
    std::string error;
    accepted(fixture.core.claimEndTurn(lease, error), error,
             "claim local EndTurn");
    REQUIRE(lease == expectedLease);
    REQUIRE(fixture.core.endTurnPending());
    const ControlOutboundFrame observed = writeOutbound(
        fixture,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return fixture.core.prepareEndTurnObserved(
                lease, outbound, prepareError);
        });
    expectOutbound(observed, ControlOutboundKind::EndTurnObserved,
                   protocol::Op::EndTurnObserved, {epoch, expectedLease});
}

void writeHostEndTurnApplied(Fixture& host,
                             std::uint32_t originHandle,
                             std::uint32_t expectedLease)
{
    const ControlOutboundFrame applied = writeOutbound(
        host,
        [&](ControlOutboundFrame& outbound, std::string& error) {
            return host.core.prepareEndTurnApplied(originHandle, outbound, error);
        });
    REQUIRE(applied.handle == originHandle);
    REQUIRE(applied.lease == expectedLease);
    expectOutbound(applied, ControlOutboundKind::EndTurnApplied,
                   protocol::Op::EndTurnApplied, {epoch, expectedLease});
}

void grantNextTurn(Pair& pair,
                   std::uint32_t playerHandle,
                   std::uint32_t day,
                   std::uint32_t lease,
                   std::uint32_t actionId)
{
    const auto apply = acceptAction(
        pair.host, action(actionId, protocol::EngineActionKind::ApplyTurnStart,
                          playerHandle, day, lease));
    writeActionResult(pair.host, apply);

    Fixture& local = playerHandle == hostHandle ? pair.host : pair.join;
    const auto activate = acceptAction(
        local, action(actionId, protocol::EngineActionKind::ActivateTurn,
                      playerHandle, day, lease));
    writeActionResult(local, activate);
    REQUIRE(local.core.phase() == ControlPhase::Active);
    REQUIRE(!local.core.endTurnPending());
}

void testFragmentedAndCoalescedFraming()
{
    const protocol::Bytes plan = encoded(
        protocol::Op::SessionPlan,
        {epoch, static_cast<std::uint32_t>(TurnMode::Simultaneous),
         hostHandle, joinHandle, mergeDay, hostLease1, joinLease1});
    const protocol::Bytes apply = encoded(
        protocol::Op::EngineAction,
        {epoch, 9, static_cast<std::uint32_t>(
                       protocol::EngineActionKind::ApplyTurnStart),
         joinHandle, 2, joinLease2});
    protocol::Bytes stream = plan;
    stream.insert(stream.end(), apply.begin(), apply.end());

    protocol::FrameDecoder decoder;
    std::vector<protocol::Frame> frames;
    std::string error;
    REQUIRE(decoder.push(stream.data(), 3, frames, error));
    REQUIRE(frames.empty());
    REQUIRE(error.empty());

    const std::size_t middle = plan.size() + 2;
    REQUIRE(decoder.push(stream.data() + 3, middle - 3, frames, error));
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].op == protocol::Op::SessionPlan);
    REQUIRE(frames[0].payload.size() == 28);

    REQUIRE(decoder.push(stream.data() + middle, stream.size() - middle,
                         frames, error));
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].op == protocol::Op::EngineAction);
    REQUIRE(frames[0].payload.size() == 24);

    decoder.reset();
    REQUIRE(decoder.push(stream.data(), stream.size(), frames, error));
    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].op == protocol::Op::SessionPlan);
    REQUIRE(frames[1].op == protocol::Op::EngineAction);

    protocol::Bytes flagged = plan;
    flagged[6] = 1;
    decoder.reset();
    REQUIRE(!decoder.push(flagged.data(), flagged.size(), frames, error));
    REQUIRE_CONTAINS(error, "flags must be zero");
    REQUIRE(!decoder.push(nullptr, 0, frames, error));
    REQUIRE_CONTAINS(error, "already rejected");
}

void testSessionPlanRequiresFullHandleWriteAndRoleMatch()
{
    Fixture queued(Role::Host);
    REQUIRE(queued.core.bindLocalHandle(hostHandle)
            == LocalHandleBinding::Stored);
    REQUIRE(queued.core.bindLocalHandle(hostHandle)
            == LocalHandleBinding::AlreadySame);
    REQUIRE(queued.core.bindLocalHandle(joinHandle)
            == LocalHandleBinding::Conflict);

    ControlOutboundFrame local;
    REQUIRE(queued.core.prepareStoredLocalPlayerHandle(local));
    queued.core.commitQueued(local);
    REQUIRE(!queued.core.localHandleSent());
    rejectFrame(queued, sessionPlan(), ControlFailureKind::Protocol,
                "before the exact local handle write");
    queued.core.completeWritten(local);
    acceptSimultaneousPlan(queued);

    Fixture wrongRole(Role::Join);
    REQUIRE(wrongRole.core.bindLocalHandle(hostHandle)
            == LocalHandleBinding::Stored);
    ControlOutboundFrame wrongLocal;
    REQUIRE(wrongRole.core.prepareStoredLocalPlayerHandle(wrongLocal));
    wrongRole.core.commitQueued(wrongLocal);
    wrongRole.core.completeWritten(wrongLocal);
    rejectFrame(wrongRole, sessionPlan(), ControlFailureKind::Protocol,
                "role does not match");

    Fixture flagged(Role::Host);
    publishLocalHandle(flagged);
    protocol::Frame nonzeroFlags = sessionPlan();
    nonzeroFlags.flags = 1;
    rejectFrame(flagged, nonzeroFlags, ControlFailureKind::Protocol,
                "flags must be zero");
}

void testExplicitStockPolicy()
{
    Fixture stock(Role::Host);
    publishLocalHandle(stock);
    const ControlInboundEvent event = acceptFrame(
        stock, sessionPlan(TurnMode::Stock, 0, 0, 0),
        ControlInboundKind::SessionPlan);
    REQUIRE(event.sessionPlan.mode == TurnMode::Stock);
    REQUIRE(stock.core.mode() == TurnMode::Stock);
    REQUIRE(stock.core.phase() == ControlPhase::Stock);

    rejectFrame(stock,
                actionFrame(action(1, protocol::EngineActionKind::ApplyTurnStart,
                                   hostHandle, 1, hostLease1)),
                ControlFailureKind::Protocol,
                "requires a simultaneous SessionPlan");
    ControlOutboundFrame outbound;
    std::string error;
    REQUIRE(!stock.core.prepareSessionActivated(outbound, error));
    REQUIRE_CONTAINS(error, "not available");

    Fixture invalidStock(Role::Host);
    publishLocalHandle(invalidStock);
    rejectFrame(invalidStock,
                sessionPlan(TurnMode::Stock, mergeDay, 0, 0),
                ControlFailureKind::Protocol,
                "stock SessionPlan must not carry merge");

    Fixture sharedLease(Role::Host);
    publishLocalHandle(sharedLease);
    rejectFrame(sharedLease,
                sessionPlan(TurnMode::Simultaneous, mergeDay,
                            hostLease1, hostLease1),
                ControlFailureKind::Protocol,
                "distinct non-zero turn leases");

    ControlClientCore optedOut;
    optedOut.configure(SimTurnsSessionOptions{false, Role::Host});
    error.clear();
    REQUIRE(!optedOut.activate(error));
    REQUIRE_CONTAINS(error, "explicit local opt-in");
}

void testBootstrapTranscriptsAndWriteBarriers()
{
    Fixture host(Role::Host);
    beginBootstrap(host);
    const auto apply = acceptAction(
        host, action(1, protocol::EngineActionKind::ApplyTurnStart,
                     joinHandle, 1, joinLease1));
    const ControlOutboundFrame result = queueActionResult(host, apply, true);
    rejectFrame(host, inbound(protocol::Op::BootstrapCommitted,
                              {joinHandle, 1}),
                ControlFailureKind::Protocol,
                "completed local day-1 bootstrap");
    host.core.completeWritten(result);
    acceptFrame(host, inbound(protocol::Op::BootstrapCommitted,
                              {joinHandle, 1}),
                ControlInboundKind::BootstrapCommitted);

    const ControlOutboundFrame commit = writeOutbound(
        host,
        [&](ControlOutboundFrame& outbound, std::string& error) {
            return host.core.prepareBootstrapCommitApplied(
                joinHandle, 1, outbound, error);
        });
    expectOutbound(commit, ControlOutboundKind::BootstrapCommitApplied,
                   protocol::Op::BootstrapCommitApplied, {joinHandle, 1});
    acceptFrame(host, inbound(protocol::Op::BootstrapOperational,
                              {joinHandle, 1}),
                ControlInboundKind::BootstrapOperational);
    const ControlOutboundFrame operational = queueOutbound(
        host,
        [&](ControlOutboundFrame& outbound, std::string& error) {
            return host.core.prepareBootstrapOperationalApplied(
                joinHandle, 1, outbound, error);
        });
    rejectFrame(host, inbound(protocol::Op::BootstrapReleased,
                              {joinHandle, 1}),
                ControlFailureKind::Protocol,
                "distributed prepare barrier");
    host.core.completeWritten(operational);
    const ControlInboundEvent released = acceptFrame(
        host, inbound(protocol::Op::BootstrapReleased, {joinHandle, 1}),
        ControlInboundKind::BootstrapReleased);
    std::string error;
    accepted(host.core.ackBootstrapReleased(released.bootstrapReleased, error),
             error, "ack host BootstrapReleased");
    REQUIRE(host.core.phase() == ControlPhase::Active);

    Fixture join(Role::Join);
    beginBootstrap(join);
    const ControlOutboundFrame began = writeOutbound(
        join,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return join.core.prepareBootstrapBeginTurnApplied(
                joinHandle, 1, outbound, prepareError);
        });
    expectOutbound(began, ControlOutboundKind::BootstrapBeginTurnApplied,
                   protocol::Op::BootstrapBeginTurnApplied, {joinHandle, 1});
    const ControlOutboundFrame complete = queueOutbound(
        join,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return join.core.prepareBootstrapComplete(
                joinHandle, 1, outbound, prepareError);
        });
    rejectFrame(join, inbound(protocol::Op::BootstrapCommitted,
                              {joinHandle, 1}),
                ControlFailureKind::Protocol,
                "completed local day-1 bootstrap");
    join.core.completeWritten(complete);
    acceptFrame(join, inbound(protocol::Op::BootstrapCommitted,
                              {joinHandle, 1}),
                ControlInboundKind::BootstrapCommitted);
    const ControlOutboundFrame joinCommit = writeOutbound(
        join,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return join.core.prepareBootstrapCommitApplied(
                joinHandle, 1, outbound, prepareError);
        });
    expectOutbound(joinCommit, ControlOutboundKind::BootstrapCommitApplied,
                   protocol::Op::BootstrapCommitApplied, {joinHandle, 1});
    acceptFrame(join, inbound(protocol::Op::BootstrapOperational,
                              {joinHandle, 1}),
                ControlInboundKind::BootstrapOperational);
    const ControlOutboundFrame joinOperational = writeOutbound(
        join,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return join.core.prepareBootstrapOperationalApplied(
                joinHandle, 1, outbound, prepareError);
        });
    expectOutbound(joinOperational,
                   ControlOutboundKind::BootstrapOperationalApplied,
                   protocol::Op::BootstrapOperationalApplied,
                   {joinHandle, 1});
    const ControlInboundEvent joinReleased = acceptFrame(
        join, inbound(protocol::Op::BootstrapReleased, {joinHandle, 1}),
        ControlInboundKind::BootstrapReleased);
    error.clear();
    accepted(join.core.ackBootstrapReleased(
                 joinReleased.bootstrapReleased, error),
             error, "ack join BootstrapReleased");
    REQUIRE(join.core.phase() == ControlPhase::Active);
}

void testLeaseSignalsAndSharedApplyActivateTransaction()
{
    Pair pair;
    bootstrap(pair);

    std::uint32_t lease = 0;
    std::string error;
    accepted(pair.host.core.claimEndTurn(lease, error), error,
             "claim host day 1");
    REQUIRE(lease == hostLease1);
    ControlOutboundFrame wrongObserved;
    error.clear();
    REQUIRE(!pair.host.core.prepareEndTurnObserved(
        hostLease2, wrongObserved, error));
    REQUIRE_CONTAINS(error, "claimed local turn lease");
    const ControlOutboundFrame observed = writeOutbound(
        pair.host,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return pair.host.core.prepareEndTurnObserved(
                hostLease1, outbound, prepareError);
        });
    expectOutbound(observed, ControlOutboundKind::EndTurnObserved,
                   protocol::Op::EndTurnObserved, {epoch, hostLease1});
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease1);

    ControlOutboundFrame duplicateApplied;
    error.clear();
    REQUIRE(!pair.host.core.prepareEndTurnApplied(
        hostHandle, duplicateApplied, error));
    REQUIRE_CONTAINS(error, "duplicate");

    grantNextTurn(pair, hostHandle, 2, hostLease2, 2);

    claimAndWriteObserved(pair.join, joinLease1);
    writeHostEndTurnApplied(pair.host, joinHandle, joinLease1);
    grantNextTurn(pair, joinHandle, 2, joinLease2, 3);

    error.clear();
    accepted(pair.host.core.claimEndTurn(lease, error), error,
             "claim host relay-granted day 2");
    REQUIRE(lease == hostLease2);
    accepted(pair.join.core.claimEndTurn(lease, error), error,
             "claim join relay-granted day 2");
    REQUIRE(lease == joinLease2);

    ControlOutboundFrame notHost;
    error.clear();
    REQUIRE(!pair.join.core.prepareEndTurnApplied(
        joinHandle, notHost, error));
    REQUIRE_CONTAINS(error, "on the host");
}

void testActionIdentityOrderingReplayAndFailure()
{
    std::string error;
    Fixture host(Role::Host);
    bootstrap(host);
    claimAndWriteObserved(host, hostLease1);
    writeHostEndTurnApplied(host, hostHandle, hostLease1);

    const auto applyValue = action(
        10, protocol::EngineActionKind::ApplyTurnStart,
        hostHandle, 2, hostLease2);
    const auto acceptedApply = acceptAction(host, applyValue);
    rejectFrame(host,
                actionFrame(action(11,
                                   protocol::EngineActionKind::ApplyTurnStart,
                                   joinHandle, 2, joinLease2)),
                ControlFailureKind::Protocol, "overtook");
    writeActionResult(host, acceptedApply);

    rejectFrame(host, actionFrame(applyValue),
                ControlFailureKind::Protocol, "duplicate");
    rejectFrame(host,
                actionFrame(action(9,
                                   protocol::EngineActionKind::ApplyTurnStart,
                                   joinHandle, 2, joinLease2)),
                ControlFailureKind::Protocol, "moved backwards");

    const auto activateValue = action(
        10, protocol::EngineActionKind::ActivateTurn,
        hostHandle, 2, hostLease2);
    const auto acceptedActivate = acceptAction(host, activateValue);
    writeActionResult(host, acceptedActivate);
    REQUIRE(!host.core.endTurnPending());
    rejectFrame(host, actionFrame(activateValue),
                ControlFailureKind::Protocol, "duplicate");

    Fixture failed(Role::Host);
    bootstrap(failed);
    writeHostEndTurnApplied(failed, joinHandle, joinLease1);
    const auto failedApply = acceptAction(
        failed, action(7, protocol::EngineActionKind::ApplyTurnStart,
                       joinHandle, 2, joinLease2));
    writeActionResult(failed, failedApply, false);
    REQUIRE(failed.core.phase() == ControlPhase::Failed);
    rejectFrame(failed, actionFrame(failedApply),
                ControlFailureKind::Protocol, "duplicate");
    ControlOutboundFrame unchangedLease;
    error.clear();
    REQUIRE(!failed.core.prepareEndTurnApplied(
        joinHandle, unchangedLease, error));
    REQUIRE_CONTAINS(error, "requires a negotiated origin on the host");

    Fixture remoteError(Role::Host);
    publishLocalHandle(remoteError);
    protocol::Bytes message{'b', 'a', 'd'};
    protocol::Frame errorFrame;
    errorFrame.op = protocol::Op::Error;
    errorFrame.payload = message;
    rejectFrame(remoteError, errorFrame, ControlFailureKind::RemoteError,
                "relay Error: bad");
}

void testTerminalFailureRejectsStaleEndTurnEvidence()
{
    Fixture host(Role::Host);
    bootstrap(host);

    std::uint32_t claimedLease = 0;
    std::string error;
    accepted(host.core.claimEndTurn(claimedLease, error), error,
             "claim host EndTurn before terminal action failure");
    REQUIRE(claimedLease == hostLease1);
    REQUIRE(host.core.endTurnPending());

    // A peer turn may be processed while this process still owns an unpublished
    // local claim. Once that engine action fails, the old claim must not leak a
    // later EndTurnObserved frame out of the terminal core.
    writeHostEndTurnApplied(host, joinHandle, joinLease1);
    const auto failedApply = acceptAction(
        host, action(2, protocol::EngineActionKind::ApplyTurnStart,
                     joinHandle, 2, joinLease2));
    writeActionResult(host, failedApply, false);
    REQUIRE(host.core.phase() == ControlPhase::Failed);

    ControlOutboundFrame staleObserved;
    error.clear();
    REQUIRE(!host.core.prepareEndTurnObserved(
        claimedLease, staleObserved, error));
    REQUIRE_CONTAINS(error, "claimed local turn lease");
}

void testTurnActionsRequireCausalEvidenceAndExactGrant()
{
    {
        Fixture host(Role::Host);
        bootstrap(host);
        rejectFrame(host,
                    actionFrame(action(
                        2, protocol::EngineActionKind::ApplyTurnStart,
                        hostHandle, 2, hostLease2)),
                    ControlFailureKind::Protocol, "EndTurnApplied");
    }
    {
        Fixture join(Role::Join);
        bootstrap(join);
        std::uint32_t lease = 0;
        std::string error;
        accepted(join.core.claimEndTurn(lease, error), error,
                 "claim join before causal ActivateTurn rejection");
        REQUIRE(lease == joinLease1);
        rejectFrame(join,
                    actionFrame(action(
                        2, protocol::EngineActionKind::ActivateTurn,
                        joinHandle, 2, joinLease2)),
                    ControlFailureKind::Protocol, "pending local turn");
    }
    {
        Pair pair;
        bootstrap(pair);
        claimAndWriteObserved(pair.host, hostLease1);
        writeHostEndTurnApplied(pair.host, hostHandle, hostLease1);
        const auto apply = acceptAction(
            pair.host, action(2, protocol::EngineActionKind::ApplyTurnStart,
                              hostHandle, 2, hostLease2));
        writeActionResult(pair.host, apply);

        rejectFrame(pair.host,
                    actionFrame(action(
                        2, protocol::EngineActionKind::ActivateTurn,
                        hostHandle, 2, 0x103u)),
                    ControlFailureKind::Protocol, "completed ApplyTurnStart");
    }
    {
        Pair pair;
        bootstrap(pair);
        claimAndWriteObserved(pair.join, joinLease1);
        writeHostEndTurnApplied(pair.host, joinHandle, joinLease1);
        rejectFrame(pair.host,
                    actionFrame(action(
                        2, protocol::EngineActionKind::ApplyTurnStart,
                        joinHandle, 2, hostLease1)),
                    ControlFailureKind::Protocol, "reused lease");
    }
}

void testHoldingHostAcceptsOnlyPeerCatchUp()
{
    Pair pair;
    bootstrap(pair);
    claimAndWriteObserved(pair.host, hostLease1);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease1);
    grantNextTurn(pair, hostHandle, 2, hostLease2, 2);
    claimAndWriteObserved(pair.host, hostLease2);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease2);
    const auto held = acceptAction(
        pair.host, action(3, protocol::EngineActionKind::HoldInput,
                          hostHandle, mergeDay - 1));
    writeActionResult(pair.host, held);
    REQUIRE(pair.host.core.phase() == ControlPhase::Holding);

    rejectFrame(pair.host,
                actionFrame(action(
                    4, protocol::EngineActionKind::ApplyTurnStart,
                    hostHandle, mergeDay - 1, 0x103u)),
                ControlFailureKind::Protocol, "invalid target");
}

void testMergeProofRequiresSuccessfulExecute()
{
    Pair pair;
    bootstrap(pair);
    claimAndWriteObserved(pair.host, hostLease1);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease1);
    grantNextTurn(pair, hostHandle, 2, hostLease2, 2);
    claimAndWriteObserved(pair.host, hostLease2);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease2);
    const auto held = acceptAction(
        pair.host, action(3, protocol::EngineActionKind::HoldInput,
                          hostHandle, mergeDay - 1));
    writeActionResult(pair.host, held);
    const auto prepare = acceptAction(
        pair.host, action(4, protocol::EngineActionKind::PrepareMerge,
                          hostHandle, mergeDay));
    writeActionResult(pair.host, prepare);
    REQUIRE(pair.host.core.phase() == ControlPhase::MergePrepared);

    ControlOutboundFrame mergeApplied;
    std::string error;
    REQUIRE(!pair.host.core.prepareMergeApplied(4, mergeApplied, error));
    REQUIRE_CONTAINS(error, "prepared merge transaction");

    const auto execute = acceptAction(
        pair.host, action(4, protocol::EngineActionKind::ExecuteMerge,
                          hostHandle, mergeDay));
    const ControlOutboundFrame failedResult = queueActionResult(
        pair.host, execute, false);
    error.clear();
    REQUIRE(!pair.host.core.prepareMergeApplied(4, mergeApplied, error));
    REQUIRE_CONTAINS(error, "prepared merge transaction");
    pair.host.core.completeWritten(failedResult);
    error.clear();
    REQUIRE(!pair.host.core.prepareMergeApplied(4, mergeApplied, error));
    REQUIRE_CONTAINS(error, "prepared merge transaction");
}

void testActionIdBoundaryDoesNotSilentlyWrap()
{
    Fixture host(Role::Host);
    bootstrap(host);
    claimAndWriteObserved(host, hostLease1);
    writeHostEndTurnApplied(host, hostHandle, hostLease1);
    const auto boundary = acceptAction(
        host, action(0xffffffffu,
                     protocol::EngineActionKind::ApplyTurnStart,
                     hostHandle, 2, hostLease2));
    writeActionResult(host, boundary, false);
    rejectFrame(host,
                actionFrame(action(
                    1, protocol::EngineActionKind::ApplyTurnStart,
                    hostHandle, 2, hostLease2)),
                ControlFailureKind::Protocol, "moved backwards");
}

void testHoldCatchUpAndStrictMergeTransaction()
{
    Pair pair;
    bootstrap(pair);

    // Host advances alone to day 2.
    claimAndWriteObserved(pair.host, hostLease1);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease1);
    grantNextTurn(pair, hostHandle, 2, hostLease2, 2);

    // The first completed day N-1 is held. No client computes N locally.
    claimAndWriteObserved(pair.host, hostLease2);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease2);
    const auto hostHold = acceptAction(
        pair.host, action(3, protocol::EngineActionKind::HoldInput,
                          hostHandle, mergeDay - 1));
    writeActionResult(pair.host, hostHold);
    REQUIRE(pair.host.core.phase() == ControlPhase::Holding);

    // While held, the host may still apply the lagging player's day-2 grant.
    claimAndWriteObserved(pair.join, joinLease1);
    writeHostEndTurnApplied(pair.host, joinHandle, joinLease1);
    const auto catchUp = acceptAction(
        pair.host, action(4, protocol::EngineActionKind::ApplyTurnStart,
                          joinHandle, mergeDay - 1, joinLease2));
    writeActionResult(pair.host, catchUp);
    REQUIRE(pair.host.core.phase() == ControlPhase::Holding);
    const auto joinActivate = acceptAction(
        pair.join, action(4, protocol::EngineActionKind::ActivateTurn,
                          joinHandle, mergeDay - 1, joinLease2));
    writeActionResult(pair.join, joinActivate);

    claimAndWriteObserved(pair.join, joinLease2);
    writeHostEndTurnApplied(pair.host, joinHandle, joinLease2);
    rejectFrame(pair.join,
                actionFrame(action(5, protocol::EngineActionKind::HoldInput,
                                   joinHandle, mergeDay)),
                ControlFailureKind::Protocol,
                "pre-merge barrier arrival");
    const auto joinHold = acceptAction(
        pair.join, action(5, protocol::EngineActionKind::HoldInput,
                          joinHandle, mergeDay - 1));
    writeActionResult(pair.join, joinHold);
    REQUIRE(pair.join.core.phase() == ControlPhase::Holding);

    rejectFrame(pair.host,
                actionFrame(action(6,
                                   protocol::EngineActionKind::ApplyTurnStart,
                                   hostHandle, mergeDay, 0x103u)),
                ControlFailureKind::Protocol, "pre-merge negotiated player");
    rejectFrame(pair.host,
                actionFrame(action(6, protocol::EngineActionKind::PrepareMerge,
                                   hostHandle, mergeDay - 1)),
                ControlFailureKind::Protocol, "merge target");

    const auto prepare = action(
        6, protocol::EngineActionKind::PrepareMerge,
        hostHandle, mergeDay);
    writeActionResult(pair.host, acceptAction(pair.host, prepare));
    writeActionResult(pair.join, acceptAction(pair.join, prepare));
    REQUIRE(pair.host.core.phase() == ControlPhase::MergePrepared);
    REQUIRE(pair.join.core.phase() == ControlPhase::MergePrepared);

    rejectFrame(pair.join,
                actionFrame(action(6, protocol::EngineActionKind::ExecuteMerge,
                                   hostHandle, mergeDay)),
                ControlFailureKind::Protocol, "prepared host merge");

    const auto execute = acceptAction(
        pair.host, action(6, protocol::EngineActionKind::ExecuteMerge,
                          hostHandle, mergeDay));
    ControlOutboundFrame mergeApplied;
    std::string error;
    REQUIRE(!pair.host.core.prepareMergeApplied(6, mergeApplied, error));
    REQUIRE_CONTAINS(error, "prepared merge transaction");

    const ControlOutboundFrame executeResult = queueActionResult(
        pair.host, execute, true);
    error.clear();
    REQUIRE(!pair.host.core.prepareMergeApplied(7, mergeApplied, error));
    REQUIRE_CONTAINS(error, "prepared merge transaction");
    const ControlOutboundFrame hostMerged = queueOutbound(
        pair.host,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return pair.host.core.prepareMergeApplied(
                6, outbound, prepareError);
        });
    expectOutbound(hostMerged, ControlOutboundKind::MergeApplied,
                   protocol::Op::MergeApplied, {epoch, 6});
    pair.host.core.completeWritten(executeResult);
    pair.host.core.completeWritten(hostMerged);
    REQUIRE(pair.host.core.phase() == ControlPhase::MergeApplied);

    const ControlOutboundFrame joinMerged = writeOutbound(
        pair.join,
        [&](ControlOutboundFrame& outbound, std::string& prepareError) {
            return pair.join.core.prepareMergeApplied(
                6, outbound, prepareError);
        });
    expectOutbound(joinMerged, ControlOutboundKind::MergeApplied,
                   protocol::Op::MergeApplied, {epoch, 6});
    REQUIRE(pair.join.core.phase() == ControlPhase::MergeApplied);

    const auto release = action(
        6, protocol::EngineActionKind::ReleaseStock,
        hostHandle, mergeDay);
    const auto hostRelease = acceptAction(pair.host, release);
    ControlOutboundFrame forbiddenResult;
    error.clear();
    REQUIRE(!pair.host.core.prepareActionResult(
        hostRelease, true, forbiddenResult, error));
    REQUIRE_CONTAINS(error, "sole pending engine action");
    error.clear();
    accepted(pair.host.core.ackReleaseStock(hostRelease, error), error,
             "ack host ReleaseStock");
    REQUIRE(pair.host.core.phase() == ControlPhase::Merged);

    const auto joinRelease = acceptAction(pair.join, release);
    accepted(pair.join.core.ackReleaseStock(joinRelease, error), error,
             "ack join ReleaseStock");
    REQUIRE(pair.join.core.phase() == ControlPhase::Merged);
    rejectFrame(pair.host, actionFrame(release),
                ControlFailureKind::Protocol, "duplicate");
}

void testMergeOrderingAndFailedPrepare()
{
    Fixture active(Role::Host);
    bootstrap(active);
    rejectFrame(active,
                actionFrame(action(2, protocol::EngineActionKind::PrepareMerge,
                                   hostHandle, mergeDay)),
                ControlFailureKind::Protocol, "held lobby-owned merge target");
    rejectFrame(active,
                actionFrame(action(2, protocol::EngineActionKind::ReleaseStock,
                                   hostHandle, mergeDay)),
                ControlFailureKind::Protocol, "applied merge");

    Pair pair;
    bootstrap(pair);
    claimAndWriteObserved(pair.host, hostLease1);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease1);
    grantNextTurn(pair, hostHandle, 2, hostLease2, 2);
    claimAndWriteObserved(pair.host, hostLease2);
    writeHostEndTurnApplied(pair.host, hostHandle, hostLease2);
    const auto held = acceptAction(
        pair.host, action(3, protocol::EngineActionKind::HoldInput,
                          hostHandle, mergeDay - 1));
    writeActionResult(pair.host, held);

    const auto failedPrepare = acceptAction(
        pair.host, action(4, protocol::EngineActionKind::PrepareMerge,
                          hostHandle, mergeDay));
    writeActionResult(pair.host, failedPrepare, false);
    REQUIRE(pair.host.core.phase() == ControlPhase::Failed);
    rejectFrame(pair.host, actionFrame(failedPrepare),
                ControlFailureKind::Protocol, "duplicate");

    rejectFrame(pair.host,
                actionFrame(action(
                    5, protocol::EngineActionKind::PrepareMerge,
                    hostHandle, mergeDay)),
                ControlFailureKind::Protocol,
                "held lobby-owned merge target");

    std::uint32_t lease = 0;
    std::string error;
    REQUIRE(!pair.host.core.claimEndTurn(lease, error));
    REQUIRE(lease == 0);
    REQUIRE_CONTAINS(error, "current local-access state");
}

void testServerIssuedTurnContextNeverInfersCalendar()
{
    resetTurnContext();
    TurnGrant resolved{0xffffffffu, 0xffffffffu, 0xffffffffu};
    REQUIRE(!resolveTurnGrant(hostHandle, resolved));
    REQUIRE(resolved.handle == 0 && resolved.day == 0 && resolved.lease == 0);

    const TurnGrant host{hostHandle, 1, hostLease1};
    const TurnGrant join{joinHandle, 1, joinLease1};
    REQUIRE(initializeTurnContextFromSessionPlan(host, join));
    REQUIRE(initializeTurnContextFromSessionPlan(host, join));
    REQUIRE(!initializeTurnContextFromSessionPlan(
        host, TurnGrant{joinHandle, 1, hostLease1}));

    REQUIRE(resolveTurnGrant(hostHandle, resolved));
    REQUIRE(resolved.handle == hostHandle && resolved.day == 1
            && resolved.lease == hostLease1);
    REQUIRE(installTurnGrant(TurnGrant{hostHandle, 7, hostLease2}));
    REQUIRE(resolveTurnGrant(hostHandle, resolved));
    REQUIRE(resolved.day == 7 && resolved.lease == hostLease2);
    REQUIRE(!installTurnGrant(TurnGrant{0xa3de0003u, 8, 0x303u}));
    REQUIRE(!installTurnGrant(TurnGrant{hostHandle, 8, 0}));

    {
        EngineActionContextScope joinScope{
            ScopedEngineActionContext{joinHandle, 11, 0}};
        REQUIRE(joinScope.valid());
        REQUIRE(resolveTurnGrant(joinHandle, resolved));
        REQUIRE(resolved.handle == joinHandle && resolved.day == 11
                && resolved.lease == 0);
        REQUIRE(!resolveTurnGrant(hostHandle, resolved));
        REQUIRE(resolved.handle == 0 && resolved.day == 0 && resolved.lease == 0);

        {
            EngineActionContextScope hostScope{
                ScopedEngineActionContext{hostHandle, 12, 0x404u}};
            REQUIRE(hostScope.valid());
            REQUIRE(resolveTurnGrant(hostHandle, resolved));
            REQUIRE(resolved.day == 12 && resolved.lease == 0x404u);
        }

        REQUIRE(resolveTurnGrant(joinHandle, resolved));
        REQUIRE(resolved.day == 11 && resolved.lease == 0);
    }

    {
        EngineActionContextScope invalidScope{
            ScopedEngineActionContext{0, 13, 0}};
        REQUIRE(!invalidScope.valid());
        REQUIRE(!resolveTurnGrant(hostHandle, resolved));
        REQUIRE(resolved.handle == 0 && resolved.day == 0 && resolved.lease == 0);
    }

    REQUIRE(resolveTurnGrant(hostHandle, resolved));
    REQUIRE(resolved.day == 7 && resolved.lease == hostLease2);
    resetTurnContext();
    REQUIRE(!resolveTurnGrant(hostHandle, resolved));
}

void testEngineDayDomainStopsAtSignedIntLimit()
{
    static_assert(maxEngineDay == 0x7fffffffu);

    protocol::SessionPlan plan;
    std::string error;
    REQUIRE(protocol::decodeSessionPlan(
        sessionPlan(TurnMode::Simultaneous, maxEngineDay).payload,
        plan, error));
    REQUIRE(plan.mergeDay == maxEngineDay);
    REQUIRE(error.empty());

    error.clear();
    REQUIRE(!protocol::decodeSessionPlan(
        sessionPlan(TurnMode::Simultaneous, maxEngineDay + 1u).payload,
        plan, error));
    REQUIRE_CONTAINS(error, "INT32_MAX");

    protocol::EngineAction decoded;
    error.clear();
    REQUIRE(protocol::decodeEngineAction(
        actionFrame(action(1, protocol::EngineActionKind::ApplyTurnStart,
                           hostHandle, maxEngineDay, hostLease1)).payload,
        decoded, error));
    REQUIRE(decoded.day == maxEngineDay);
    REQUIRE(error.empty());

    error.clear();
    REQUIRE(!protocol::decodeEngineAction(
        actionFrame(action(1, protocol::EngineActionKind::ApplyTurnStart,
                           hostHandle, maxEngineDay + 1u, hostLease1)).payload,
        decoded, error));
    REQUIRE_CONTAINS(error, "INT32_MAX");

    resetTurnContext();
    REQUIRE(initializeTurnContextFromSessionPlan(
        TurnGrant{hostHandle, 1, hostLease1},
        TurnGrant{joinHandle, 1, joinLease1}));
    REQUIRE(installTurnGrant(TurnGrant{hostHandle, maxEngineDay, hostLease2}));
    REQUIRE(!installTurnGrant(
        TurnGrant{hostHandle, maxEngineDay + 1u, hostLease2}));
    {
        EngineActionContextScope maximum{
            ScopedEngineActionContext{hostHandle, maxEngineDay, 0}};
        REQUIRE(maximum.valid());
    }
    {
        EngineActionContextScope overflow{
            ScopedEngineActionContext{hostHandle, maxEngineDay + 1u, 0}};
        REQUIRE(!overflow.valid());
    }
    resetTurnContext();
}

struct TestCase
{
    const char* name;
    void (*run)();
};

} // namespace

int main()
{
    const TestCase tests[]{
        {"fragmented and coalesced v8 framing",
         &testFragmentedAndCoalescedFraming},
        {"SessionPlan full-write causality and role match",
         &testSessionPlanRequiresFullHandleWriteAndRoleMatch},
        {"explicit Stock policy", &testExplicitStockPolicy},
        {"bootstrap transcripts and write barriers",
         &testBootstrapTranscriptsAndWriteBarriers},
        {"lease signals and shared Apply/Activate transaction",
         &testLeaseSignalsAndSharedApplyActivateTransaction},
        {"action ordering, replay, and failure",
         &testActionIdentityOrderingReplayAndFailure},
        {"terminal failure rejects stale EndTurn evidence",
         &testTerminalFailureRejectsStaleEndTurnEvidence},
        {"turn actions require causal evidence and exact grants",
         &testTurnActionsRequireCausalEvidenceAndExactGrant},
        {"Holding host accepts only peer catch-up",
         &testHoldingHostAcceptsOnlyPeerCatchUp},
        {"merge proof requires successful ExecuteMerge",
         &testMergeProofRequiresSuccessfulExecute},
        {"actionId boundary does not silently wrap",
         &testActionIdBoundaryDoesNotSilentlyWrap},
        {"Hold catch-up and strict merge transaction",
         &testHoldCatchUpAndStrictMergeTransaction},
        {"merge ordering and failed Prepare",
         &testMergeOrderingAndFailedPrepare},
        {"server-issued turn context never infers a calendar",
         &testServerIssuedTurnContextNeverInfersCalendar},
        {"engine day domain stops at signed-int limit",
         &testEngineDayDomainStopsAtSignedIntLimit},
    };

    std::size_t passed = 0;
    for (const TestCase& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "PASS: " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << '/' << (sizeof(tests) / sizeof(tests[0]))
              << " simturns control-client v8 transcript tests passed\n";
    return 0;
}
