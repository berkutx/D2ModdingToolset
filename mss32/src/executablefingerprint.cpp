/*
 * Exact executable identity checks shared by production and test-only code.
 */

#include "executablefingerprint.h"
#include "utils.h"
#include "version.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace executablefingerprint {

namespace {

constexpr std::uint64_t kRussobitSize = 4187648;
constexpr std::uintptr_t kRussobitImageBase = 0x00400000;
constexpr std::array<std::uint8_t, 32> kRussobitSha256 = {
    0x13, 0x75, 0xCD, 0xEF, 0x09, 0xEC, 0x47, 0x0E,
    0xE6, 0x4F, 0xE5, 0x69, 0x3F, 0xB7, 0x34, 0xD7,
    0xC6, 0x9F, 0xB2, 0x15, 0x21, 0x23, 0x11, 0xD9,
    0x97, 0xF7, 0x92, 0xB2, 0x58, 0xA6, 0x42, 0xEB,
};

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u,
    0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u,
    0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u,
    0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au,
    0x5B9CCA4Fu, 0x682E6FF3u, 0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

std::uint32_t rotateRight(std::uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32 - bits));
}

class Sha256
{
public:
    void update(const std::uint8_t* data, std::size_t size)
    {
        totalBytes += size;
        if (bufferUsed) {
            const std::size_t take =
                (size < buffer.size() - bufferUsed) ? size : buffer.size() - bufferUsed;
            std::memcpy(buffer.data() + bufferUsed, data, take);
            bufferUsed += take;
            data += take;
            size -= take;
            if (bufferUsed == buffer.size()) {
                transform(buffer.data());
                bufferUsed = 0;
            }
        }
        while (size >= buffer.size()) {
            transform(data);
            data += buffer.size();
            size -= buffer.size();
        }
        if (size) {
            std::memcpy(buffer.data(), data, size);
            bufferUsed = size;
        }
    }

    std::array<std::uint8_t, 32> finish()
    {
        const std::uint64_t totalBits = totalBytes * 8;
        buffer[bufferUsed++] = 0x80;
        if (bufferUsed > 56) {
            std::memset(buffer.data() + bufferUsed, 0, buffer.size() - bufferUsed);
            transform(buffer.data());
            bufferUsed = 0;
        }
        std::memset(buffer.data() + bufferUsed, 0, 56 - bufferUsed);
        for (unsigned i = 0; i < 8; ++i)
            buffer[63 - i] = static_cast<std::uint8_t>(totalBits >> (i * 8));
        transform(buffer.data());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state.size(); ++i) {
            digest[4 * i + 0] = static_cast<std::uint8_t>(state[i] >> 24);
            digest[4 * i + 1] = static_cast<std::uint8_t>(state[i] >> 16);
            digest[4 * i + 2] = static_cast<std::uint8_t>(state[i] >> 8);
            digest[4 * i + 3] = static_cast<std::uint8_t>(state[i]);
        }
        return digest;
    }

private:
    void transform(const std::uint8_t* block)
    {
        std::uint32_t words[64];
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(block[4 * i + 0]) << 24)
                       | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16)
                       | (static_cast<std::uint32_t>(block[4 * i + 2]) << 8)
                       | static_cast<std::uint32_t>(block[4 * i + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotateRight(words[i - 15], 7)
                                     ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 = rotateRight(words[i - 2], 17)
                                     ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t sum1 =
                rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choose + kSha256RoundConstants[i] + words[i];
            const std::uint32_t sum0 =
                rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint32_t, 8> state = {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };
    std::array<std::uint8_t, 64> buffer{};
    std::size_t bufferUsed = 0;
    std::uint64_t totalBytes = 0;
};

struct FileHandle
{
    ~FileHandle()
    {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    HANDLE value = INVALID_HANDLE_VALUE;
};

bool verifyExactRussobit()
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return false;
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) != kRussobitImageBase)
        return false;

    FileHandle file;
    file.value = CreateFileW(exePath().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart != kRussobitSize)
        return false;

    Sha256 sha;
    std::array<std::uint8_t, 64 * 1024> chunk{};
    std::uint64_t totalRead = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file.value, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr))
            return false;
        if (!read)
            break;
        sha.update(chunk.data(), read);
        totalRead += read;
    }
    return totalRead == kRussobitSize && sha.finish() == kRussobitSha256;
}

} // namespace

bool isExactRussobit()
{
    static const bool exact = verifyExactRussobit();
    return exact;
}

} // namespace executablefingerprint
} // namespace hooks
