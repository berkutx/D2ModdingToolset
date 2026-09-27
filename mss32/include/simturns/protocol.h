/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * Wire codec for the simultaneous-turn control protocol. The coordinator is
 * the sole owner of turn days, leases, local-access policy, and merge timing.
 */

#ifndef SIMTURNS_PROTOCOL_H
#define SIMTURNS_PROTOCOL_H

#include "simturns/session_types.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hooks::simturns::protocol {

constexpr std::uint32_t version = 8;
constexpr std::uint32_t maxFrameLength = 64u * 1024u;
constexpr std::size_t maxErrorPayloadSize = 4096;

enum class Op : std::uint16_t
{
    Hello = 0x0001,
    HelloAck = 0x0002,
    Goodbye = 0x0003,
    LocalPlayerHandle = 0x0007,
    SessionPlan = 0x0008,
    SessionActivated = 0x000b,
    BootstrapBeginTurnApplied = 0x000c,
    BootstrapComplete = 0x000d,
    BootstrapCommitted = 0x000e,
    BootstrapCommitApplied = 0x000f,
    BootstrapOperational = 0x0010,
    BootstrapOperationalApplied = 0x0011,
    BootstrapReleased = 0x0015,
    EndTurnObserved = 0x1000,
    EngineAction = 0x1001,
    ActionResult = 0x1002,
    EndTurnApplied = 0x1007,
    MergeApplied = 0x1008,
    Error = 0x10ff,
};

enum class EngineActionKind : std::uint32_t
{
    ApplyTurnStart = 1,
    ActivateTurn = 2,
    HoldInput = 3,
    PrepareMerge = 4,
    ExecuteMerge = 5,
    ReleaseStock = 6,
};

using Bytes = std::vector<std::uint8_t>;

struct Frame
{
    Op op{};
    std::uint16_t flags{};
    Bytes payload;
};

struct HelloAck
{
    bool accepted{};
    std::uint32_t version{};
};

struct SessionPlan
{
    std::uint32_t epoch{};
    TurnMode mode{TurnMode::Stock};
    std::uint32_t hostHandle{};
    std::uint32_t joinHandle{};
    std::uint32_t mergeDay{};
    std::uint32_t hostLease{};
    std::uint32_t joinLease{};
};

struct BootstrapProgress
{
    std::uint32_t handle{};
    std::uint32_t day{};
};

struct TurnSignal
{
    std::uint32_t epoch{};
    std::uint32_t lease{};
};

struct EngineAction
{
    std::uint32_t epoch{};
    std::uint32_t actionId{};
    EngineActionKind kind{};
    std::uint32_t playerHandle{};
    std::uint32_t day{};
    std::uint32_t lease{};
};

struct ActionResult
{
    std::uint32_t epoch{};
    std::uint32_t actionId{};
    EngineActionKind kind{};
    bool success{};
};

struct MergeApplied
{
    std::uint32_t epoch{};
    std::uint32_t actionId{};
};

/** Streaming decoder for fragmented or coalesced byte-stream reads. */
class FrameDecoder
{
public:
    bool push(const std::uint8_t* data,
              std::size_t size,
              std::vector<Frame>& frames,
              std::string& error);
    void reset();

private:
    Bytes buffered;
    bool rejected{};
};

bool isKnownOp(Op op);
bool isKnownActionKind(EngineActionKind kind);
const char* opName(Op op);
const char* actionName(EngineActionKind kind);

/** All encode* functions return complete length-prefixed frames. */
Bytes encodeFrame(Op op, const Bytes& payload = {});
Bytes encodeHello(std::uint32_t pid, Role roleHint);
Bytes encodeLocalPlayerHandle(std::uint32_t handle);
Bytes encodeSessionActivated();
Bytes encodeBootstrapBeginTurnApplied(std::uint32_t handle, std::uint32_t day);
Bytes encodeBootstrapComplete(std::uint32_t handle, std::uint32_t day);
Bytes encodeBootstrapCommitApplied(std::uint32_t handle, std::uint32_t day);
Bytes encodeBootstrapOperationalApplied(std::uint32_t handle, std::uint32_t day);
Bytes encodeEndTurnObserved(std::uint32_t epoch, std::uint32_t lease);
Bytes encodeEndTurnApplied(std::uint32_t epoch, std::uint32_t lease);
Bytes encodeActionResult(const EngineAction& action, bool success);
Bytes encodeMergeApplied(std::uint32_t epoch, std::uint32_t actionId);

bool decodeHelloAck(const Bytes& payload, HelloAck& value, std::string& error);
bool decodeSessionPlan(const Bytes& payload, SessionPlan& value, std::string& error);
bool decodeBootstrapCommitted(const Bytes& payload,
                              BootstrapProgress& value,
                              std::string& error);
bool decodeBootstrapOperational(const Bytes& payload,
                                BootstrapProgress& value,
                                std::string& error);
bool decodeBootstrapReleased(const Bytes& payload,
                             BootstrapProgress& value,
                             std::string& error);
bool decodeEngineAction(const Bytes& payload, EngineAction& value, std::string& error);
bool decodeError(const Bytes& payload, std::string& value, std::string& error);
bool requireEmptyPayload(const Bytes& payload, const char* label, std::string& error);

} // namespace hooks::simturns::protocol

#endif // SIMTURNS_PROTOCOL_H
