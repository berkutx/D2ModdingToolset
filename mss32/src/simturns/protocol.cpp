/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "simturns/protocol.h"
#include <stdexcept>

namespace hooks::simturns::protocol {

namespace {

std::uint16_t readU16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
           | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t readU32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
           | (static_cast<std::uint32_t>(bytes[1]) << 8)
           | (static_cast<std::uint32_t>(bytes[2]) << 16)
           | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void appendU16(Bytes& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(Bytes& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

bool requireSize(const Bytes& payload,
                 std::size_t expected,
                 const char* label,
                 std::string& error)
{
    if (payload.size() == expected)
        return true;
    error = std::string(label) + " payload must be " + std::to_string(expected)
            + " bytes, got " + std::to_string(payload.size());
    return false;
}

bool validMergeDay(std::uint32_t value)
{
    return value == 0 || (value >= 2 && value <= maxEngineDay);
}

bool validUtf8(const Bytes& bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::uint8_t lead = bytes[offset++];
        if (lead <= 0x7f)
            continue;

        std::uint32_t codePoint = 0;
        std::size_t continuationCount = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            codePoint = lead & 0x1f;
            continuationCount = 1;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            codePoint = lead & 0x0f;
            continuationCount = 2;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            codePoint = lead & 0x07;
            continuationCount = 3;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (bytes.size() - offset < continuationCount)
            return false;
        for (std::size_t i = 0; i < continuationCount; ++i) {
            const std::uint8_t continuation = bytes[offset++];
            if ((continuation & 0xc0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (continuation & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff
            || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

Bytes encodeBootstrapProgress(Op op,
                              const char* label,
                              std::uint32_t handle,
                              std::uint32_t day)
{
    if (!handle || day != 1)
        throw std::invalid_argument(std::string(label) + " requires a non-zero handle at day 1");
    Bytes payload;
    payload.reserve(8);
    appendU32(payload, handle);
    appendU32(payload, day);
    return encodeFrame(op, payload);
}

bool decodeBootstrapProgress(const Bytes& payload,
                             const char* label,
                             BootstrapProgress& value,
                             std::string& error)
{
    if (!requireSize(payload, 8, label, error))
        return false;
    value.handle = readU32(payload.data());
    value.day = readU32(payload.data() + 4);
    if (!value.handle || value.day != 1) {
        error = std::string(label) + " requires a non-zero join handle at day 1";
        return false;
    }
    return true;
}

Bytes encodeTurnSignal(Op op, std::uint32_t epoch, std::uint32_t lease)
{
    if (!epoch || !lease)
        throw std::invalid_argument("turn signal requires a non-zero epoch and lease");
    Bytes payload;
    payload.reserve(8);
    appendU32(payload, epoch);
    appendU32(payload, lease);
    return encodeFrame(op, payload);
}

} // namespace

bool FrameDecoder::push(const std::uint8_t* data,
                        std::size_t size,
                        std::vector<Frame>& frames,
                        std::string& error)
{
    frames.clear();
    error.clear();
    if (rejected) {
        error = "frame decoder already rejected this stream";
        return false;
    }
    if (size && !data) {
        rejected = true;
        error = "frame decoder received a null non-empty input";
        return false;
    }
    if (!size)
        return true;

    buffered.insert(buffered.end(), data, data + size);
    std::size_t consumed = 0;
    while (buffered.size() - consumed >= sizeof(std::uint32_t)) {
        const auto* header = buffered.data() + consumed;
        const std::uint32_t length = readU32(header);
        if (length < 4) {
            rejected = true;
            error = "frame length must be at least 4, got " + std::to_string(length);
            buffered.clear();
            return false;
        }
        if (length > maxFrameLength) {
            rejected = true;
            error = "frame length " + std::to_string(length) + " exceeds limit "
                    + std::to_string(maxFrameLength);
            buffered.clear();
            return false;
        }

        const std::size_t total = sizeof(std::uint32_t) + length;
        if (buffered.size() - consumed < total)
            break;

        Frame frame;
        frame.op = static_cast<Op>(readU16(header + 4));
        frame.flags = readU16(header + 6);
        if (frame.flags != 0) {
            rejected = true;
            error = "protocol v8 frame flags must be zero";
            buffered.clear();
            return false;
        }
        frame.payload.assign(header + 8, header + total);
        frames.push_back(std::move(frame));
        consumed += total;
    }

    if (consumed)
        buffered.erase(buffered.begin(), buffered.begin() + consumed);
    if (buffered.size() > static_cast<std::size_t>(maxFrameLength) + sizeof(std::uint32_t)) {
        rejected = true;
        error = "incomplete frame exceeds the protocol buffer limit";
        buffered.clear();
        frames.clear();
        return false;
    }
    return true;
}

void FrameDecoder::reset()
{
    buffered.clear();
    rejected = false;
}

bool isKnownOp(Op op)
{
    switch (op) {
    case Op::Hello:
    case Op::HelloAck:
    case Op::Goodbye:
    case Op::LocalPlayerHandle:
    case Op::SessionPlan:
    case Op::SessionActivated:
    case Op::BootstrapBeginTurnApplied:
    case Op::BootstrapComplete:
    case Op::BootstrapCommitted:
    case Op::BootstrapCommitApplied:
    case Op::BootstrapOperational:
    case Op::BootstrapOperationalApplied:
    case Op::BootstrapReleased:
    case Op::EndTurnObserved:
    case Op::EngineAction:
    case Op::ActionResult:
    case Op::EndTurnApplied:
    case Op::MergeApplied:
    case Op::Error:
        return true;
    }
    return false;
}

bool isKnownActionKind(EngineActionKind kind)
{
    switch (kind) {
    case EngineActionKind::ApplyTurnStart:
    case EngineActionKind::ActivateTurn:
    case EngineActionKind::HoldInput:
    case EngineActionKind::PrepareMerge:
    case EngineActionKind::ExecuteMerge:
    case EngineActionKind::ReleaseStock:
        return true;
    }
    return false;
}

const char* opName(Op op)
{
    switch (op) {
    case Op::Hello: return "Hello";
    case Op::HelloAck: return "HelloAck";
    case Op::Goodbye: return "Goodbye";
    case Op::LocalPlayerHandle: return "LocalPlayerHandle";
    case Op::SessionPlan: return "SessionPlan";
    case Op::SessionActivated: return "SessionActivated";
    case Op::BootstrapBeginTurnApplied: return "BootstrapBeginTurnApplied";
    case Op::BootstrapComplete: return "BootstrapComplete";
    case Op::BootstrapCommitted: return "BootstrapCommitted";
    case Op::BootstrapCommitApplied: return "BootstrapCommitApplied";
    case Op::BootstrapOperational: return "BootstrapOperational";
    case Op::BootstrapOperationalApplied: return "BootstrapOperationalApplied";
    case Op::BootstrapReleased: return "BootstrapReleased";
    case Op::EndTurnObserved: return "EndTurnObserved";
    case Op::EngineAction: return "EngineAction";
    case Op::ActionResult: return "ActionResult";
    case Op::EndTurnApplied: return "EndTurnApplied";
    case Op::MergeApplied: return "MergeApplied";
    case Op::Error: return "Error";
    }
    return "Unknown";
}

const char* actionName(EngineActionKind kind)
{
    switch (kind) {
    case EngineActionKind::ApplyTurnStart: return "ApplyTurnStart";
    case EngineActionKind::ActivateTurn: return "ActivateTurn";
    case EngineActionKind::HoldInput: return "HoldInput";
    case EngineActionKind::PrepareMerge: return "PrepareMerge";
    case EngineActionKind::ExecuteMerge: return "ExecuteMerge";
    case EngineActionKind::ReleaseStock: return "ReleaseStock";
    }
    return "UnknownAction";
}

Bytes encodeFrame(Op op, const Bytes& payload)
{
    if (!isKnownOp(op))
        throw std::invalid_argument("cannot encode an unknown simultaneous-turn opcode");
    if (payload.size() > maxFrameLength - 4)
        throw std::length_error("simultaneous-turn frame payload exceeds the protocol limit");
    Bytes result;
    result.reserve(8 + payload.size());
    appendU32(result, static_cast<std::uint32_t>(4 + payload.size()));
    appendU16(result, static_cast<std::uint16_t>(op));
    appendU16(result, 0);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

Bytes encodeHello(std::uint32_t pid, Role roleHint)
{
    if (!pid)
        throw std::invalid_argument("Hello pid must be non-zero");
    if (roleHint != Role::Host && roleHint != Role::Join)
        throw std::invalid_argument("Hello role hint must be host or join");
    Bytes payload;
    payload.reserve(12);
    appendU32(payload, version);
    appendU32(payload, pid);
    appendU32(payload, static_cast<std::uint32_t>(roleHint));
    return encodeFrame(Op::Hello, payload);
}

Bytes encodeLocalPlayerHandle(std::uint32_t handle)
{
    if (!handle)
        throw std::invalid_argument("LocalPlayerHandle handle must be non-zero");
    Bytes payload;
    appendU32(payload, handle);
    return encodeFrame(Op::LocalPlayerHandle, payload);
}

Bytes encodeSessionActivated()
{
    return encodeFrame(Op::SessionActivated);
}

Bytes encodeBootstrapBeginTurnApplied(std::uint32_t handle, std::uint32_t day)
{
    return encodeBootstrapProgress(Op::BootstrapBeginTurnApplied,
                                   "BootstrapBeginTurnApplied", handle, day);
}

Bytes encodeBootstrapComplete(std::uint32_t handle, std::uint32_t day)
{
    return encodeBootstrapProgress(Op::BootstrapComplete, "BootstrapComplete", handle, day);
}

Bytes encodeBootstrapCommitApplied(std::uint32_t handle, std::uint32_t day)
{
    return encodeBootstrapProgress(Op::BootstrapCommitApplied,
                                   "BootstrapCommitApplied", handle, day);
}

Bytes encodeBootstrapOperationalApplied(std::uint32_t handle, std::uint32_t day)
{
    return encodeBootstrapProgress(Op::BootstrapOperationalApplied,
                                   "BootstrapOperationalApplied", handle, day);
}

Bytes encodeEndTurnObserved(std::uint32_t epoch, std::uint32_t lease)
{
    return encodeTurnSignal(Op::EndTurnObserved, epoch, lease);
}

Bytes encodeEndTurnApplied(std::uint32_t epoch, std::uint32_t lease)
{
    return encodeTurnSignal(Op::EndTurnApplied, epoch, lease);
}

Bytes encodeActionResult(const EngineAction& action, bool success)
{
    if (!action.epoch || !action.actionId || !isKnownActionKind(action.kind))
        throw std::invalid_argument("ActionResult requires an exact non-zero action identity");
    Bytes payload;
    payload.reserve(16);
    appendU32(payload, action.epoch);
    appendU32(payload, action.actionId);
    appendU32(payload, static_cast<std::uint32_t>(action.kind));
    appendU32(payload, success ? 1u : 0u);
    return encodeFrame(Op::ActionResult, payload);
}

Bytes encodeMergeApplied(std::uint32_t epoch, std::uint32_t actionId)
{
    if (!epoch || !actionId)
        throw std::invalid_argument("MergeApplied requires a non-zero epoch and actionId");
    Bytes payload;
    payload.reserve(8);
    appendU32(payload, epoch);
    appendU32(payload, actionId);
    return encodeFrame(Op::MergeApplied, payload);
}

bool decodeHelloAck(const Bytes& payload, HelloAck& value, std::string& error)
{
    if (!requireSize(payload, 8, "HelloAck", error))
        return false;
    const std::uint32_t accepted = readU32(payload.data());
    if (accepted > 1) {
        error = "HelloAck accepted must be 0 or 1";
        return false;
    }
    value.accepted = accepted != 0;
    value.version = readU32(payload.data() + 4);
    return true;
}

bool decodeSessionPlan(const Bytes& payload, SessionPlan& value, std::string& error)
{
    if (!requireSize(payload, 28, "SessionPlan", error))
        return false;
    value.epoch = readU32(payload.data());
    value.mode = static_cast<TurnMode>(readU32(payload.data() + 4));
    value.hostHandle = readU32(payload.data() + 8);
    value.joinHandle = readU32(payload.data() + 12);
    value.mergeDay = readU32(payload.data() + 16);
    value.hostLease = readU32(payload.data() + 20);
    value.joinLease = readU32(payload.data() + 24);
    if (!value.epoch || !value.hostHandle || !value.joinHandle
        || value.hostHandle == value.joinHandle) {
        error = "SessionPlan requires a non-zero epoch and two distinct handles";
        return false;
    }
    if (!validMergeDay(value.mergeDay)) {
        error = "SessionPlan mergeDay must be zero or in the engine range [2, INT32_MAX]";
        return false;
    }
    if (value.mode == TurnMode::Simultaneous) {
        if (!value.hostLease || !value.joinLease
            || value.hostLease == value.joinLease) {
            error = "simultaneous SessionPlan requires two distinct non-zero turn leases";
            return false;
        }
    } else if (value.mode == TurnMode::Stock) {
        if (value.mergeDay || value.hostLease || value.joinLease) {
            error = "stock SessionPlan must not carry merge or turn-lease state";
            return false;
        }
    } else {
        error = "SessionPlan contains an unknown turn mode";
        return false;
    }
    return true;
}

bool decodeBootstrapCommitted(const Bytes& payload,
                              BootstrapProgress& value,
                              std::string& error)
{
    return decodeBootstrapProgress(payload, "BootstrapCommitted", value, error);
}

bool decodeBootstrapOperational(const Bytes& payload,
                                BootstrapProgress& value,
                                std::string& error)
{
    return decodeBootstrapProgress(payload, "BootstrapOperational", value, error);
}

bool decodeBootstrapReleased(const Bytes& payload,
                             BootstrapProgress& value,
                             std::string& error)
{
    return decodeBootstrapProgress(payload, "BootstrapReleased", value, error);
}

bool decodeEngineAction(const Bytes& payload, EngineAction& value, std::string& error)
{
    if (!requireSize(payload, 24, "EngineAction", error))
        return false;
    value.epoch = readU32(payload.data());
    value.actionId = readU32(payload.data() + 4);
    value.kind = static_cast<EngineActionKind>(readU32(payload.data() + 8));
    value.playerHandle = readU32(payload.data() + 12);
    value.day = readU32(payload.data() + 16);
    value.lease = readU32(payload.data() + 20);
    if (!value.epoch || !value.actionId || !isKnownActionKind(value.kind)
        || !value.playerHandle || !value.day) {
        error = "EngineAction requires non-zero identity, player, and day fields";
        return false;
    }
    if (value.day > maxEngineDay) {
        error = "EngineAction day exceeds the engine's signed INT32_MAX limit";
        return false;
    }
    const bool turnGrant = value.kind == EngineActionKind::ApplyTurnStart
                           || value.kind == EngineActionKind::ActivateTurn;
    if (turnGrant != (value.lease != 0)) {
        error = turnGrant
                    ? "turn-grant EngineAction requires a non-zero lease"
                    : "non-turn EngineAction must not carry a lease";
        return false;
    }
    return true;
}

bool decodeError(const Bytes& payload, std::string& value, std::string& error)
{
    if (payload.size() > maxErrorPayloadSize) {
        error = "Error payload exceeds 4096 bytes";
        return false;
    }
    if (!validUtf8(payload)) {
        error = "Error payload is not valid UTF-8";
        return false;
    }
    value.assign(payload.begin(), payload.end());
    return true;
}

bool requireEmptyPayload(const Bytes& payload, const char* label, std::string& error)
{
    return requireSize(payload, 0, label ? label : "frame", error);
}

} // namespace hooks::simturns::protocol
