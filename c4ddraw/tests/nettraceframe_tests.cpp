// Portable tests of the production frame classifier. No game process is read.
#include "../features/nettraceframe.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace c4nettraceframe;
unsigned checks = 0;
unsigned failures = 0;

void check(bool value, const char* expression, int line)
{
    ++checks;
    if (value) return;
    ++failures;
    std::printf("FAIL line %d: %s\n", line, expression);
}
#define CHECK(value) check(!!(value), #value, __LINE__)

void put32(std::vector<unsigned char>& frame, std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        frame[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    }
}

std::vector<unsigned char> makeFrame(const char* name, std::size_t length)
{
    std::vector<unsigned char> frame(length, 0);
    put32(frame, 0, 0xffff);
    put32(frame, 4, static_cast<std::uint32_t>(length));
    std::memcpy(frame.data() + 8, name, std::strlen(name) + 1);
    for (std::size_t i = HeaderBytes; i < length; ++i) {
        frame[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    return frame;
}

void checkRejected(const Result& result, Status status)
{
    CHECK(result.status == status);
    CHECK(result.fingerprint == 0);
}

} // namespace

int main()
{
    checkRejected(inspect(nullptr, 0), Status::Empty);
    checkRejected(inspect(nullptr, HeaderBytes), Status::InvalidHeader);

    auto begin = makeFrame(".?AVCCmdBeginTurnMsg@@", 56);
    auto end = makeFrame(".?AVCCmdEndTurnMsg@@", 57);
    // Arbitrary opaque payload, not an assertion about the native TurnInfo size.
    auto info = makeFrame(".?AVCCmdTurnInfoMsg@@", 64);
    CHECK(inspect(begin.data(), begin.size()).kind == Kind::BeginTurn);
    CHECK(inspect(end.data(), end.size()).kind == Kind::EndTurn);
    CHECK(inspect(info.data(), info.size()).kind == Kind::TurnInfo);
    for (const auto* frame : {&begin, &end, &info}) {
        const Result full = inspect(frame->data(), frame->size());
        CHECK(full.status == Status::SelectedComplete);
        CHECK(full.length == frame->size());
        CHECK(full.fingerprint != 0);
        CHECK(full.fingerprint == inspect(frame->data(), frame->size()).fingerprint);
        // A copied header can identify the selected frame without touching body.
        const std::vector<unsigned char> header(frame->begin(), frame->begin() + HeaderBytes);
        const Result headerOnly = inspect(header.data(), header.size());
        checkRejected(headerOnly, Status::SelectedIncomplete);
        CHECK(headerOnly.kind == full.kind);
        CHECK(headerOnly.length == full.length);
        for (std::size_t n = 1; n < frame->size(); ++n) {
            const std::vector<unsigned char> truncated(frame->begin(), frame->begin() + n);
            checkRejected(inspect(truncated.data(), truncated.size()),
                          n < HeaderBytes ? Status::IncompleteHeader : Status::SelectedIncomplete);
        }
    }

    const auto baseline = inspect(begin.data(), begin.size()).fingerprint;
    // Fixed vector checked independently from this C++ helper.
    CHECK(baseline == UINT64_C(0xf17a92058d80e33f));
    auto unaligned = begin;
    unaligned.insert(unaligned.begin(), 0x80);
    CHECK(inspect(unaligned.data() + 1, begin.size()).fingerprint == baseline);
    auto changed = begin;
    changed.back() ^= 1;
    CHECK(inspect(changed.data(), changed.size()).fingerprint != baseline);
    changed = begin;
    changed[48] ^= 0x80; // Opaque bytes; no field semantics assumed by inspect().
    CHECK(inspect(changed.data(), changed.size()).fingerprint != baseline);
    changed = begin;
    changed.insert(changed.end(), 32, 0x42);
    CHECK(inspect(changed.data(), changed.size()).fingerprint == baseline);
    // Padding is part of the serialized frame and must also affect its digest.
    changed = begin;
    changed[43] = 0x7f;
    CHECK(inspect(changed.data(), changed.size()).fingerprint != baseline);

    for (std::uint32_t badType : {0u, 1u, 0xffffffffu, 0xffu}) {
        changed = begin;
        put32(changed, 0, badType);
        checkRejected(inspect(changed.data(), changed.size()), Status::InvalidHeader);
    }
    for (std::uint32_t badLength : {0u, 43u, MaxFrameBytesExclusive, 0xffffffffu}) {
        changed = begin;
        put32(changed, 4, badLength);
        checkRejected(inspect(changed.data(), changed.size()), Status::InvalidHeader);
    }
    changed = begin;
    std::memset(changed.data() + 8, 'A', 36);
    checkRejected(inspect(changed.data(), changed.size()), Status::InvalidHeader);
    changed[8] = 0;
    checkRejected(inspect(changed.data(), changed.size()), Status::InvalidHeader);

    for (const char* name : {".?AVCCmdMoveStackMsg@@", ".?AVCCmdTurnInfoMsg@@X", "CCmdTurnInfoMsg",
                             ".?AVCCmdturnInfoMsg@@", ".?AVCCmdBeginTurnMsg@"}) {
        auto other = makeFrame(name, 64);
        // Do not request/copy/hash even a selected-looking unsupported payload.
        put32(other, 4, MaxFrameBytesExclusive - 1);
        checkRejected(inspect(other.data(), HeaderBytes), Status::Unselected);
        CHECK(inspect(other.data(), HeaderBytes).kind == Kind::None);
    }

    auto bounded = makeFrame(".?AVCCmdTurnInfoMsg@@", CopyLimit);
    CHECK(inspect(bounded.data(), bounded.size()).status == Status::SelectedComplete);
    auto tooLarge = makeFrame(".?AVCCmdTurnInfoMsg@@", CopyLimit + 1);
    checkRejected(inspect(tooLarge.data(), HeaderBytes), Status::SelectedTooLarge);
    checkRejected(inspect(tooLarge.data(), tooLarge.size()), Status::SelectedTooLarge);
    CHECK(inspect(tooLarge.data(), HeaderBytes).kind == Kind::TurnInfo);

    // Identical bytes in unrelated copies have the same fingerprint. The result
    // is deliberately not presented as a globally unique message identifier.
    const auto copied = begin;
    CHECK(inspect(copied.data(), copied.size()).fingerprint == baseline);

    std::printf("nettraceframe: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
