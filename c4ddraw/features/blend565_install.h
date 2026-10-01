#ifndef C4_BLEND565_INSTALL_H
#define C4_BLEND565_INSTALL_H

// Portable, bounded startup checks and pointer transaction. No Win32 or SIMD here.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace c4blend565install {

constexpr std::uint32_t kImageBase = 0x00400000u;
constexpr std::uint32_t kSlots[3] = {0x006F5E78u, 0x006F5E7Cu, 0x006F5E80u};
constexpr std::uint32_t kEntries[3] = {0x0067A41Bu, 0x0067A557u, 0x0067A654u};
constexpr std::size_t kLengths[3] = {316, 253, 225};
constexpr std::uint32_t kVtable = 0x006F5E4Cu;
constexpr std::uint32_t kExpectedVtable[17] = {
    0x679A3Cu, 0x679C38u, 0x679E1Fu, 0x679E47u, 0x67A083u,
    0x67A0AEu, 0x67C1E5u, 0x67A1ABu, 0x67A24Eu, 0x67A2B0u,
    0x67A347u, 0x67A41Bu, 0x67A557u, 0x67A654u, 0x67A735u,
    0x67C7B5u, 0x67A800u
};

inline std::uint16_t u16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}

inline std::uint32_t u32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

inline bool contains(std::size_t size, std::size_t offset, std::size_t length)
{
    return offset <= size && length <= size - offset;
}

struct PeLayout { std::uint32_t imageSize; };

inline bool validPe(const std::uint8_t* header, std::size_t headerBytes,
                    std::uintptr_t actualBase, PeLayout& layout)
{
    layout.imageSize = 0;
    if (!header || actualBase != kImageBase || headerBytes < 64 ||
        u16(header) != 0x5A4D)
        return false;
    const std::size_t pe = u32(header + 60);
    if (pe < 64 || !contains(headerBytes, pe, 24 + 224) ||
        u32(header + pe) != 0x00004550 || u16(header + pe + 4) != 0x014C ||
        u16(header + pe + 20) != 224)
        return false;
    const unsigned count = u16(header + pe + 6);
    const std::uint16_t characteristics = u16(header + pe + 22);
    if (count < 3 || count > 16 || (characteristics & 0x0102) != 0x0102 ||
        (characteristics & 0x2000))
        return false;
    const std::uint8_t* optional = header + pe + 24;
    const std::uint32_t imageSize = u32(optional + 56);
    const std::uint32_t headersSize = u32(optional + 60);
    const std::size_t sections = pe + 24 + 224;
    if (u16(optional) != 0x010B || u32(optional + 28) != kImageBase ||
        u32(optional + 16) != 0x0026D6E0 || u32(optional + 32) != 0x1000 ||
        u32(optional + 36) != 0x200 || imageSize < 0x0043C000 ||
        imageSize > 0x04000000 || (imageSize & 0xFFF) ||
        headersSize > headerBytes || headersSize < sections + count * 40 ||
        !contains(headerBytes, sections, count * 40))
        return false;

    // Resource/icon edits may move .rsrc/.reloc and change SizeOfImage/timestamp.
    // The three code/data sections and entry point must retain this exact layout.
    const char names[3][8] = {".text", ".rdata", ".data"};
    const std::uint32_t rvas[3] = {0x1000, 0x2CE000, 0x38E000};
    const std::uint32_t sizes[3] = {0x2CC2EC, 0xBFFE8, 0xAD234};
    const std::uint32_t flags[3] = {0x60000020, 0x40000040, 0xC0000040};
    unsigned found = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto* s = header + sections + i * 40;
        const std::uint32_t size = u32(s + 8);
        const std::uint32_t rva = u32(s + 12);
        if (!size || rva < headersSize || (rva & 0xFFF) ||
            !contains(imageSize, rva, size))
            return false;
        for (unsigned j = 0; j < i; ++j) {
            const auto* previous = header + sections + j * 40;
            const std::uint32_t otherRva = u32(previous + 12);
            const std::uint32_t otherSize = u32(previous + 8);
            if (rva < otherRva + otherSize && otherRva < rva + size)
                return false;
        }
        for (unsigned k = 0; k < 3; ++k) {
            if (std::memcmp(s, names[k], 8) == 0) {
                if ((found & (1u << k)) || rva != rvas[k] || size != sizes[k] ||
                    u32(s + 36) != flags[k])
                    return false;
                found |= 1u << k;
            }
        }
    }
    if (found != 7)
        return false;
    layout.imageSize = imageSize;
    return true;
}

inline bool validVersion(const std::uint8_t* resource, std::size_t size)
{
    // VS_VERSION_INFO root: 6-byte header, UTF-16 key, padding, VS_FIXEDFILEINFO.
    static const char key[] = "VS_VERSION_INFO";
    constexpr std::size_t valueOffset = 40;
    if (!resource || size < valueOffset + 52 || size > 65536 ||
        u16(resource) < valueOffset + 52 || u16(resource) > size ||
        u16(resource + 2) != 52 || u16(resource + 4) != 0)
        return false;
    for (std::size_t i = 0; i < sizeof(key); ++i)
        if (u16(resource + 6 + i * 2) != static_cast<unsigned char>(key[i]))
            return false;
    const auto* fixed = resource + valueOffset;
    return u32(fixed) == 0xFEEF04BD && u32(fixed + 4) == 0x00010000 &&
           u32(fixed + 8) == 0x07D3000C && u32(fixed + 12) == 0x000B0001;
}

inline std::uint32_t rotate(std::uint32_t n, unsigned r)
{
    return (n >> r) | (n << (32 - r));
}

inline void shaBlock(std::uint32_t state[8], const std::uint8_t block[64])
{
    static const std::uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    std::uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        const auto* p = block + i * 4;
        w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
               (std::uint32_t(p[2]) << 8) | p[3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        const std::uint32_t a = w[i - 15], b = w[i - 2];
        w[i] = w[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) +
               w[i - 7] + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 64; ++i) {
        const std::uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                                 ((e & f) ^ (~e & g)) + k[i] + w[i];
        const std::uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                                 ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

inline void sha256(const std::uint8_t* data, std::size_t length, std::uint8_t digest[32])
{
    std::uint32_t state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                              0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const std::uint64_t bits = std::uint64_t(length) * 8;
    while (length >= 64) {
        shaBlock(state, data);
        data += 64;
        length -= 64;
    }
    std::uint8_t tail[128] = {};
    if (length)
        std::memcpy(tail, data, length);
    tail[length] = 0x80;
    const std::size_t padded = length < 56 ? 64 : 128;
    for (unsigned i = 0; i < 8; ++i)
        tail[padded - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    shaBlock(state, tail);
    if (padded == 128)
        shaBlock(state, tail + 64);
    for (unsigned i = 0; i < 32; ++i)
        digest[i] = static_cast<std::uint8_t>(state[i / 4] >> (24 - 8 * (i % 4)));
}

inline bool validCode(unsigned index, const std::uint8_t* code, std::size_t length)
{
    // Independently extracted complete native functions, not only prologue signatures.
    static const std::uint8_t expected[3][32] = {
        {0x46,0x11,0x52,0x94,0x07,0xad,0xd8,0xe5,0x2d,0x4f,0x60,0xa4,0x48,0x09,0x36,0xa8,
         0x85,0x42,0x79,0x5f,0xea,0xa0,0xaa,0x58,0x34,0x2d,0xb5,0xc6,0x07,0xc7,0x0c,0x8a},
        {0xe2,0x26,0x4e,0x53,0x89,0x9e,0x25,0x68,0x29,0x36,0xa2,0x7e,0xb4,0x8d,0x4d,0xdd,
         0x5b,0x45,0x50,0x03,0xfd,0xcb,0x75,0x40,0x61,0x90,0xbb,0xdd,0xc4,0x09,0x28,0xbd},
        {0x54,0x44,0x09,0x34,0x81,0x1e,0x4d,0x89,0x2c,0xd4,0x7f,0xd0,0x54,0x6c,0xba,0x04,
         0xcb,0x6b,0xe1,0x22,0x6f,0x37,0x6f,0x4b,0xfc,0x1a,0x95,0x4a,0x41,0x7f,0x3e,0xc6}
    };
    if (index >= 3 || !code || length != kLengths[index])
        return false;
    std::uint8_t digest[32];
    sha256(code, length, digest);
    return std::memcmp(digest, expected[index], sizeof(digest)) == 0;
}

enum class PatchResult { Installed, ProtectFailed, Collision, RestoreFailed };

struct PatchOps {
    void* context;
    // The entire 12-byte pointer range is one page, made writable in one operation.
    bool (*beginWrite)(void*, std::uintptr_t* cookie);
    // On failure it remains writable, as with a failed single-page VirtualProtect.
    bool (*endWrite)(void*, std::uintptr_t cookie);
    std::uint32_t (*compareExchange)(void*, unsigned index,
                                    std::uint32_t expected, std::uint32_t desired);
    void (*publishOriginals)(void*);
};

inline PatchResult patchSlots(const PatchOps& ops, const std::uint32_t replacements[3])
{
    if (!ops.beginWrite || !ops.endWrite || !ops.compareExchange ||
        !ops.publishOriginals || !replacements || !replacements[0] ||
        !replacements[1] || !replacements[2])
        return PatchResult::ProtectFailed;
    std::uintptr_t cookie = 0;
    if (!ops.beginWrite(ops.context, &cookie))
        return PatchResult::ProtectFailed;
    unsigned installed = 0;
    bool collision = false;
    // Recheck every pointer after the page preflight, before publishing any hook.
    for (unsigned i = 0; i < 3; ++i) {
        if (ops.compareExchange(ops.context, i, kEntries[i], kEntries[i]) != kEntries[i]) {
            collision = true;
            break;
        }
    }
    if (!collision) {
        ops.publishOriginals(ops.context);
        for (; installed < 3; ++installed) {
            if (ops.compareExchange(ops.context, installed, kEntries[installed],
                                    replacements[installed]) != kEntries[installed]) {
                collision = true;
                break;
            }
        }
    }
    if (collision) {
        while (installed) {
            --installed;
            ops.compareExchange(ops.context, installed, replacements[installed],
                                kEntries[installed]);
        }
        if (ops.endWrite(ops.context, cookie))
            return PatchResult::Collision;
        ops.endWrite(ops.context, cookie);
        return PatchResult::RestoreFailed;
    }
    if (ops.endWrite(ops.context, cookie))
        return PatchResult::Installed;
    // Protection restoration failed while still writable: undo only our entries.
    for (unsigned i = 3; i-- != 0;)
        ops.compareExchange(ops.context, i, replacements[i], kEntries[i]);
    ops.endWrite(ops.context, cookie);
    return PatchResult::RestoreFailed;
}

} // namespace c4blend565install
#endif
