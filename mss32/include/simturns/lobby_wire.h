#ifndef SIMTURNS_LOBBY_WIRE_H
#define SIMTURNS_LOBBY_WIRE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace hooks::simturns::lobby {

// Payload of ID_LOBBY_SIMULTANEOUS_TURNS. Integers are explicitly little endian;
// the outer lobby message ID is not part of these bytes.
constexpr std::uint8_t wireVersion = 1;
constexpr std::uint32_t featureBit = 2;
constexpr std::size_t headerSize = 10;
constexpr std::size_t maxFrameSize = 65540; // v8 length prefix + 64 KiB body
enum class Operation : std::uint8_t { Arm = 0, ArmAck = 1, Frame = 2, Abort = 3 };
enum class AbortReason : std::uint16_t {
    Protocol = 1, LocalFailure = 2, Disconnected = 3, RoomLeft = 4, RoleMismatch = 5,
};
struct Envelope {
    Operation operation{};
    std::uint32_t room{}, epoch{};
    std::uint8_t role{}, status{};
    std::uint32_t mergeDay{};
    AbortReason reason{AbortReason::Protocol};
    std::vector<std::uint8_t> frame;
};
inline std::uint32_t read32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8)
        | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline void write32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
inline bool validMergeDay(std::uint32_t value) { return value == 0 || (value >= 2 && value <= 30); }
// Rooms' global table can contain empty cells for legacy rooms. Only a nonempty
// flag is authoritative; unknown nonempty values require admission, never stock
// fallback. The server must reject malformed properties before creating a room.
inline bool roomRequiresSimultaneousTurns(const char* enabled, const char* legacyDays) {
    if (enabled && *enabled) return std::strcmp(enabled, "0") != 0;
    return legacyDays && *legacyDays && std::strcmp(legacyDays, "0") != 0;
}
inline bool decode(const std::uint8_t* data, std::size_t size, Envelope& output) {
    if (!data || size < headerSize || data[0] != wireVersion || data[1] > 3) return false;
    Envelope value;
    value.operation = static_cast<Operation>(data[1]);
    value.room = read32(data + 2); value.epoch = read32(data + 6);
    if (!value.epoch) return false;
    switch (value.operation) {
    case Operation::Arm:
        if (size != headerSize + 5 || (data[10] != 1 && data[10] != 2)) return false;
        value.role = data[10]; value.mergeDay = read32(data + 11);
        if (!validMergeDay(value.mergeDay)) return false;
        break;
    case Operation::ArmAck:
        if (size != headerSize + 1 || data[10] > 1) return false;
        value.status = data[10]; break;
    case Operation::Frame:
        if (size < headerSize + 8 || size > headerSize + maxFrameSize
            || read32(data + headerSize) != size - headerSize - 4) return false;
        value.frame.assign(data + headerSize, data + size); break;
    case Operation::Abort: {
        if (size != headerSize + 2) return false;
        const auto reason = std::uint16_t(data[10]) | (std::uint16_t(data[11]) << 8);
        if (reason < 1 || reason > 5) return false;
        value.reason = static_cast<AbortReason>(reason); break;
    }
    }
    output = std::move(value); return true;
}
inline std::vector<std::uint8_t> encode(const Envelope& value) {
    std::vector<std::uint8_t> out{wireVersion, static_cast<std::uint8_t>(value.operation)};
    write32(out, value.room); write32(out, value.epoch);
    switch (value.operation) {
    case Operation::Arm: out.push_back(value.role); write32(out, value.mergeDay); break;
    case Operation::ArmAck: out.push_back(value.status); break;
    case Operation::Frame: out.insert(out.end(), value.frame.begin(), value.frame.end()); break;
    case Operation::Abort:
        out.push_back(static_cast<std::uint8_t>(value.reason)); out.push_back(0); break;
    default: return {};
    }
    Envelope checked;
    return decode(out.data(), out.size(), checked) ? out : std::vector<std::uint8_t>{};
}

} // namespace hooks::simturns::lobby
#endif
