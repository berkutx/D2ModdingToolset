#pragma once

#include <cstddef>
#include <cstdint>

// Only inspect bytes already copied safely by the caller. This helper performs no
// reads from game objects, hooks, clocks, or files, and never returns packet data.
namespace c4nettraceframe {

static const std::size_t HeaderBytes = 44;
static const std::size_t CopyLimit = 1024;
static const std::uint32_t MaxFrameBytesExclusive = 0x80000;

enum class Kind : std::uint32_t {
    None = 0,
    BeginTurn = 1,
    EndTurn = 2,
    TurnInfo = 3,
};

enum class Status : std::uint32_t {
    Empty = 0,
    IncompleteHeader = 1,
    InvalidHeader = 2,
    Unselected = 3,
    SelectedIncomplete = 4,
    SelectedTooLarge = 5,
    SelectedComplete = 6,
};

struct Result {
    Kind kind;
    Status status;
    std::uint32_t length;
    std::uint64_t fingerprint;
};

namespace detail {

inline std::uint32_t readLe32(const unsigned char* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

template <std::size_t N>
inline bool classEquals(const unsigned char* bytes, const char (&name)[N])
{
    static_assert(N <= 36, "Class name includes its terminator in the wire header");
    for (std::size_t i = 0; i < N; ++i) {
        if (bytes[i] != static_cast<unsigned char>(name[i])) return false;
    }
    return true;
}

} // namespace detail

// NetMessageHeader is {LE32 type, LE32 total length, char className[36]}.
// The selected frames are fingerprinted as opaque bytes: native CCommandMsg
// offsets are not wire offsets, and a complete frame does not prove valid game
// semantics. Stock BeginTurn/EndTurn frames observed in the relay parser are
// 56/57 bytes; no fixed TurnInfo payload length is assumed here.
//
// Call once with a copied header, then (only for a selected bounded frame) with
// the complete copy. Extra bytes beyond the declared length are ignored.
// FNV-1a is a correlation aid, not a unique sequence number or delivery receipt.
inline Result inspect(const unsigned char* bytes, std::size_t copiedBytes)
{
    Result result = {Kind::None, Status::Empty, 0, 0};
    if (copiedBytes == 0) return result;
    if (!bytes) {
        result.status = Status::InvalidHeader;
        return result;
    }
    if (copiedBytes < HeaderBytes) {
        result.status = Status::IncompleteHeader;
        return result;
    }

    result.length = detail::readLe32(bytes + 4);
    result.status = Status::InvalidHeader;
    if (detail::readLe32(bytes) != 0xffff || result.length < HeaderBytes ||
        result.length >= MaxFrameBytesExclusive) return result;

    bool terminated = false;
    for (std::size_t i = 8; i < HeaderBytes; ++i) {
        if (bytes[i] == 0) {
            terminated = true;
            break;
        }
    }
    if (!terminated || bytes[8] == 0) return result;

    const unsigned char* name = bytes + 8;
    if (detail::classEquals(name, ".?AVCCmdBeginTurnMsg@@")) {
        result.kind = Kind::BeginTurn;
    } else if (detail::classEquals(name, ".?AVCCmdEndTurnMsg@@")) {
        result.kind = Kind::EndTurn;
    } else if (detail::classEquals(name, ".?AVCCmdTurnInfoMsg@@")) {
        result.kind = Kind::TurnInfo;
    } else {
        result.status = Status::Unselected;
        return result;
    }

    if (result.length > CopyLimit) {
        result.status = Status::SelectedTooLarge;
        return result;
    }
    if (copiedBytes < result.length) {
        result.status = Status::SelectedIncomplete;
        return result;
    }

    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i < result.length; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= UINT64_C(1099511628211);
    }
    result.fingerprint = hash;
    result.status = Status::SelectedComplete;
    return result;
}

} // namespace c4nettraceframe
