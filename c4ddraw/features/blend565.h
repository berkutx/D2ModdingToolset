#ifndef C4_BLEND565_H
#define C4_BLEND565_H

#include <limits.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace c4blend565 {

struct Point { int x, y; };
struct Size { int width, height; };
enum Operation { Half, Add, Subtract };

namespace detail {

// The caller owns the buffers. Validate arithmetic before forming row pointers.
static inline bool checkedRange(const void* base, int pitch, int x, int y,
                                int width, int height,
                                uintptr_t& begin, uintptr_t& end)
{
    if (!base || pitch <= 0 || (pitch & 1) || x < 0 || y < 0 ||
        x > INT_MAX / 16 || // Native x86 computes x through (x << 4) / 8.
        (uint64_t)x * 2 + (uint64_t)width * 2 > (unsigned int)pitch)
        return false;

    const uint64_t offset = (uint64_t)y * pitch + (uint64_t)x * 2;
    const uint64_t span = (uint64_t)(height - 1) * pitch + (uint64_t)width * 2;
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    if (offset > UINTPTR_MAX - address)
        return false;
    begin = address + (uintptr_t)offset;
    if (span > UINTPTR_MAX - begin)
        return false;
    end = begin + (uintptr_t)span;
    return true;
}

#if defined(_MSC_VER) || defined(__SSE2__)

static inline uint16_t read16(const void* pointer)
{
    uint16_t value;
    memcpy(&value, pointer, sizeof(value));
    return value;
}

static inline void write16(void* pointer, uint16_t value)
{
    memcpy(pointer, &value, sizeof(value));
}

static inline uint16_t halfPixel(uint16_t source, uint16_t destination)
{
    // Native rounds each operand down separately, even for white on white.
    return (uint16_t)(((source >> 1) & 0x7BEF) + ((destination >> 1) & 0x7BEF));
}

static inline void halfRows(const unsigned char* source, int srcPitch,
                            unsigned char* destination, int dstPitch,
                            int width, int height, uint32_t key)
{
    const __m128i mask = _mm_set1_epi16(0x7BEF);
    const __m128i key16 = _mm_set1_epi16((short)key);
    const uint32_t packedKey = key | (key << 16);
    const __m128i pairKey = _mm_set1_epi32((int)packedKey);
    const bool has16Key = key <= 0xFFFFu;

    for (int y = 0; y < height; ++y)
    {
        int x = 0;
        for (; x + 8 <= width; x += 8)
        {
            const __m128i pixels = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(source + x * 2));
            // Preserve the native pair skip, including key=0xFFFFFFFF.
            __m128i skip = _mm_cmpeq_epi32(pixels, pairKey);
            if (has16Key)
                skip = _mm_or_si128(skip, _mm_cmpeq_epi16(pixels, key16));
            if (_mm_movemask_epi8(skip) == 0xFFFF)
                continue;
            const __m128i old = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(destination + x * 2));
            const __m128i blended = _mm_add_epi16(
                _mm_and_si128(_mm_srli_epi16(pixels, 1), mask),
                _mm_and_si128(_mm_srli_epi16(old, 1), mask));
            const __m128i result = _mm_or_si128(
                _mm_and_si128(skip, old), _mm_andnot_si128(skip, blended));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + x * 2), result);
        }
        // Eight-pixel blocks keep the native pair boundaries relative to row start.
        for (; x + 2 <= width; x += 2)
        {
            uint32_t pair;
            memcpy(&pair, source + x * 2, sizeof(pair));
            if (pair == packedKey)
                continue;
            for (int i = 0; i < 2; ++i)
            {
                const uint16_t pixel = read16(source + (x + i) * 2);
                if ((uint32_t)pixel != key)
                    write16(destination + (x + i) * 2,
                            halfPixel(pixel, read16(destination + (x + i) * 2)));
            }
        }
        if (x < width)
        {
            const uint16_t pixel = read16(source + x * 2);
            if ((uint32_t)pixel != key)
                write16(destination + x * 2, halfPixel(pixel, read16(destination + x * 2)));
        }
        if (y + 1 < height)
        {
            source += srcPitch;
            destination += dstPitch;
        }
    }
}

template<bool subtract>
static inline void saturateRows(const unsigned char* source, int srcPitch,
                                unsigned char* destination, int dstPitch,
                                int width, int height)
{
    const __m128i red = _mm_set1_epi16(-2048);
    const __m128i green = _mm_set1_epi16(0x07E0);
    const __m128i blue = _mm_set1_epi16(0x001F);
    const __m128i zero = _mm_setzero_si128();

    for (int y = 0; y < height; ++y)
    {
        int x = 0;
        for (; x + 8 <= width; x += 8)
        {
            const __m128i pixels = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(source + x * 2));
            if (_mm_movemask_epi8(_mm_cmpeq_epi16(pixels, zero)) == 0xFFFF)
                continue;
            const __m128i old = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(destination + x * 2));
            __m128i r, g, b;
            if (subtract)
            {
                r = _mm_subs_epu16(_mm_and_si128(old, red), _mm_and_si128(pixels, red));
                g = _mm_subs_epu16(_mm_and_si128(old, green), _mm_and_si128(pixels, green));
                b = _mm_subs_epu16(_mm_and_si128(old, blue), _mm_and_si128(pixels, blue));
            }
            else
            {
                r = _mm_and_si128(_mm_adds_epu16(
                    _mm_and_si128(pixels, red), _mm_and_si128(old, red)), red);
                g = _mm_min_epi16(_mm_add_epi16(
                    _mm_and_si128(pixels, green), _mm_and_si128(old, green)), green);
                b = _mm_min_epi16(_mm_add_epi16(
                    _mm_and_si128(pixels, blue), _mm_and_si128(old, blue)), blue);
            }
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + x * 2),
                            _mm_or_si128(r, _mm_or_si128(g, b)));
        }
        for (; x < width; ++x)
        {
            const unsigned int pixel = read16(source + x * 2);
            if (!pixel)
                continue;
            const unsigned int old = read16(destination + x * 2);
            unsigned int r, g, b;
            if (subtract)
            {
                r = (old & 0xF800) > (pixel & 0xF800) ? (old & 0xF800) - (pixel & 0xF800) : 0;
                g = (old & 0x07E0) > (pixel & 0x07E0) ? (old & 0x07E0) - (pixel & 0x07E0) : 0;
                b = (old & 0x001F) > (pixel & 0x001F) ? (old & 0x001F) - (pixel & 0x001F) : 0;
            }
            else
            {
                r = (old & 0xF800) + (pixel & 0xF800);
                g = (old & 0x07E0) + (pixel & 0x07E0);
                b = (old & 0x001F) + (pixel & 0x001F);
                if (r > 0xF800) r = 0xF800;
                if (g > 0x07E0) g = 0x07E0;
                if (b > 0x001F) b = 0x001F;
            }
            write16(destination + x * 2, (uint16_t)(r | g | b));
        }
        if (y + 1 < height)
        {
            source += srcPitch;
            destination += dstPitch;
        }
    }
}

#endif
} // namespace detail

// False leaves all pixels untouched. The caller then invokes the original kernel.
// No-ops require no pixel access or SSE2; nonempty buffers must be valid RGB565.
static inline bool apply(Operation operation,
                         const void* src, int srcPitch, const Point* srcPos,
                         void* dst, int dstPitch, const Point* dstPos,
                         const Size* size, int opacity, uint32_t key, bool sse2)
{
    if (operation != Half && operation != Add && operation != Subtract)
        return false;
    if (operation == Half && ((uint8_t)opacity == 0 || (uint8_t)opacity == 255))
        return true;
    if (!size || size->width < 0 || size->height < 0)
        return false;
    const int width = size->width, height = size->height;
    if (!width || !height)
        return true;
    if (!sse2 || width < 8 || !srcPos || !dstPos)
        return false;

    uintptr_t srcBegin, srcEnd, dstBegin, dstEnd;
    if (!detail::checkedRange(src, srcPitch, srcPos->x, srcPos->y,
                              width, height, srcBegin, srcEnd) ||
        !detail::checkedRange(dst, dstPitch, dstPos->x, dstPos->y,
                              width, height, dstBegin, dstEnd))
        return false;
    // Include row padding in overlap detection to retain native traversal semantics.
    if (!(srcEnd <= dstBegin || dstEnd <= srcBegin))
        return false;

#if defined(_MSC_VER) || defined(__SSE2__)
    const unsigned char* source = reinterpret_cast<const unsigned char*>(srcBegin);
    unsigned char* destination = reinterpret_cast<unsigned char*>(dstBegin);
    if (operation == Half)
        detail::halfRows(source, srcPitch, destination, dstPitch, width, height, key);
    else if (operation == Add)
        detail::saturateRows<false>(source, srcPitch, destination, dstPitch, width, height);
    else
        detail::saturateRows<true>(source, srcPitch, destination, dstPitch, width, height);
    return true;
#else
    (void)key;
    return false;
#endif
}

} // namespace c4blend565

#endif
