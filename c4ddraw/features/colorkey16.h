#ifndef C4_COLORKEY16_H
#define C4_COLORKEY16_H

#include <windows.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__SSE2__)
#include <emmintrin.h>

/* Return FALSE without touching pixels when the original scalar path is needed. */
static BOOL c4_blt_colorkey16(
    unsigned char* dst, int dst_x, int dst_y, int dst_w, int dst_h, int dst_p,
    unsigned char* src, int src_x, int src_y, int src_p,
    unsigned int key_low, unsigned int key_high, int bpp, BOOL use_sse2)
{
    uint64_t d_offset, s_offset, d_span, s_span;
    uintptr_t d_begin, s_begin, d_end, s_end;
    unsigned char* d;
    const unsigned char* s;
    __m128i key;
    int y;
    if (!use_sse2 || !dst || !src || bpp != 16 ||
        (unsigned short)key_low != (unsigned short)key_high ||
        dst_w < 8 || dst_h <= 0 || dst_p <= 0 || src_p <= 0 ||
        (dst_p & 1) || (src_p & 1) ||
        dst_x < 0 || dst_y < 0 || src_x < 0 || src_y < 0 ||
        (uint64_t)dst_x * 2 + (uint64_t)dst_w * 2 > (unsigned int)dst_p ||
        (uint64_t)src_x * 2 + (uint64_t)dst_w * 2 > (unsigned int)src_p)
        return FALSE;

    d_offset = (uint64_t)dst_y * dst_p + (uint64_t)dst_x * 2;
    s_offset = (uint64_t)src_y * src_p + (uint64_t)src_x * 2;
    d_span = (uint64_t)(dst_h - 1) * dst_p + (uint64_t)dst_w * 2;
    s_span = (uint64_t)(dst_h - 1) * src_p + (uint64_t)dst_w * 2;
    /* Check integer addresses before forming any adjusted pointer. */
    if (d_offset > UINTPTR_MAX - (uintptr_t)dst ||
        s_offset > UINTPTR_MAX - (uintptr_t)src)
        return FALSE;
    d_begin = (uintptr_t)dst + (uintptr_t)d_offset;
    s_begin = (uintptr_t)src + (uintptr_t)s_offset;
    if (d_span > UINTPTR_MAX - d_begin || s_span > UINTPTR_MAX - s_begin)
        return FALSE;
    d_end = d_begin + (uintptr_t)d_span;
    s_end = s_begin + (uintptr_t)s_span;
    /* Include row padding: conservative overlap keeps scalar self-blit semantics. */
    if (!(d_end <= s_begin || s_end <= d_begin))
        return FALSE;

    d = (unsigned char*)d_begin;
    s = (const unsigned char*)s_begin;
    key = _mm_set1_epi16((short)key_low);
    for (y = 0; y < dst_h; ++y)
    {
        int x = 0;
        for (; x + 8 <= dst_w; x += 8)
        {
            const __m128i pixels = _mm_loadu_si128((const __m128i*)(s + x * 2));
            const __m128i transparent = _mm_cmpeq_epi16(pixels, key);
            const int mask = _mm_movemask_epi8(transparent);
            if (mask == 0xFFFF)
                continue;
            if (mask == 0)
                _mm_storeu_si128((__m128i*)(d + x * 2), pixels);
            else
            {
                const __m128i old = _mm_loadu_si128((const __m128i*)(d + x * 2));
                const __m128i result = _mm_or_si128(
                    _mm_and_si128(transparent, old), _mm_andnot_si128(transparent, pixels));
                _mm_storeu_si128((__m128i*)(d + x * 2), result);
            }
        }
        for (; x < dst_w; ++x)
        {
            unsigned short pixel;
            memcpy(&pixel, s + x * 2, sizeof(pixel));
            if (pixel != (unsigned short)key_low)
                memcpy(d + x * 2, &pixel, sizeof(pixel));
        }
        if (y + 1 < dst_h)
        {
            d += dst_p;
            s += src_p;
        }
    }
    return TRUE;
}
#else
/* Non-SSE2 builds retain the upstream implementation. */
#define c4_blt_colorkey16(...) FALSE
#endif

#endif
