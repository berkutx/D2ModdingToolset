/*
 * Decorative background for fixed-size Disciples II screens on a Hor+ canvas.
 *
 * DisciplesGL renders these screens as three layers: a tiled back.png wallpaper,
 * a centered transparent frame, then the game's 800x600 (or WideBattle 990x600)
 * pixels.  cnc-ddraw receives a 16-bit primary surface without the alpha channel
 * used by that compositor, so C4dll-R builds a presentation-only scratch buffer
 * from the already-known centered rectangle.  The game-owned primary surface is
 * never modified.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#define LODEPNG_NO_COMPILE_CPP
extern "C" {
#include "lodepng.h"
}

extern "C" int horplus_get_decor_layout(int* contentWidth, int* contentHeight,
                                         int* wideBattle);
extern "C" void DDInvalidateDecorativeFrame(void);

namespace {

constexpr int kBackResource = 2200;
constexpr int kAltResource = 2201;
constexpr int kAltWideResource = 2202;

struct Image
{
    unsigned char* rgba;
    unsigned width;
    unsigned height;
};

SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_enabled = 1;
bool g_loadAttempted = false;
bool g_assetsReady = false;
Image g_back = {};
Image g_alt = {};
Image g_altWide = {};
unsigned g_offsetX = 0;
unsigned g_offsetY = 0;

unsigned char* g_base = nullptr;
unsigned char* g_present = nullptr;
SIZE_T g_capacity = 0;
int g_cacheWidth = 0;
int g_cacheHeight = 0;
int g_cachePitch = 0;
int g_cacheBpp = 0;
int g_cacheRgb555 = 0;
int g_cacheWide = -1;

const char* menuIni()
{
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
        char* slash = std::strrchr(path, '\\');
        if (slash)
            slash[1] = '\0';
        else
            path[0] = '\0';
        lstrcatA(path, "C4menu.ini");
    }
    return path;
}

HMODULE ownModule()
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(&ownModule), &mbi,
                     sizeof(mbi)) != sizeof(mbi))
        return nullptr;
    return static_cast<HMODULE>(mbi.AllocationBase);
}

bool loadPngResource(int id, unsigned expectedWidth, unsigned expectedHeight,
                     Image* image)
{
    if (!image)
        return false;

    HMODULE module = ownModule();
    HRSRC resource = module
        ? FindResourceA(module, MAKEINTRESOURCEA(id), RT_RCDATA)
        : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    HGLOBAL loaded = size ? LoadResource(module, resource) : nullptr;
    const auto* bytes = loaded
        ? static_cast<const unsigned char*>(LockResource(loaded))
        : nullptr;
    if (!bytes || !size)
        return false;

    unsigned char* rgba = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    const unsigned error =
        lodepng_decode32(&rgba, &width, &height, bytes, size);
    if (error || !rgba || width != expectedWidth || height != expectedHeight) {
        std::free(rgba);
        return false;
    }

    image->rgba = rgba;
    image->width = width;
    image->height = height;
    return true;
}

bool ensureAssetsLocked()
{
    if (g_loadAttempted)
        return g_assetsReady;
    g_loadAttempted = true;

    if (!loadPngResource(kBackResource, 1536, 1536, &g_back) ||
        !loadPngResource(kAltResource, 932, 728, &g_alt) ||
        !loadPngResource(kAltWideResource, 1122, 728, &g_altWide)) {
        std::free(g_back.rgba);
        std::free(g_alt.rgba);
        std::free(g_altWide.rgba);
        g_back = {};
        g_alt = {};
        g_altWide = {};
        OutputDebugStringA(
            "C4dll-R: decorative background resources could not be decoded\n");
        return false;
    }

    // DisciplesGL selects a static random crop once per process.  Keep the same
    // behavior without changing the CRT's global rand() state used by the game.
    std::uint32_t seed = GetTickCount() ^ (GetCurrentProcessId() * 0x9E3779B9u);
    seed = seed * 1664525u + 1013904223u;
    g_offsetX = seed % (g_back.width * 2u);
    seed = seed * 1664525u + 1013904223u;
    g_offsetY = seed % g_back.height;
    g_assetsReady = true;
    return true;
}

bool ensureBuffersLocked(SIZE_T size)
{
    if (!size || size > 128u * 1024u * 1024u)
        return false;
    if (g_capacity >= size && g_base && g_present)
        return true;

    HANDLE heap = GetProcessHeap();
    auto* base = static_cast<unsigned char*>(HeapAlloc(heap, 0, size));
    auto* present = static_cast<unsigned char*>(HeapAlloc(heap, 0, size));
    if (!base || !present) {
        if (base)
            HeapFree(heap, 0, base);
        if (present)
            HeapFree(heap, 0, present);
        return false;
    }

    if (g_base)
        HeapFree(heap, 0, g_base);
    if (g_present)
        HeapFree(heap, 0, g_present);
    g_base = base;
    g_present = present;
    g_capacity = size;
    return true;
}

void writePixel(unsigned char* dst, int bpp, int rgb555,
                unsigned r, unsigned g, unsigned b)
{
    if (bpp == 32) {
        dst[0] = static_cast<unsigned char>(b);
        dst[1] = static_cast<unsigned char>(g);
        dst[2] = static_cast<unsigned char>(r);
        dst[3] = 0xFF;
    } else if (rgb555) {
        const std::uint16_t value = static_cast<std::uint16_t>(
            ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        std::memcpy(dst, &value, sizeof(value));
    } else {
        const std::uint16_t value = static_cast<std::uint16_t>(
            ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        std::memcpy(dst, &value, sizeof(value));
    }
}

void readPixel(const unsigned char* src, int bpp, int rgb555,
               unsigned* r, unsigned* g, unsigned* b)
{
    if (bpp == 32) {
        *b = src[0];
        *g = src[1];
        *r = src[2];
        return;
    }

    std::uint16_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    if (rgb555) {
        *r = ((value >> 10) & 31u) * 255u / 31u;
        *g = ((value >> 5) & 31u) * 255u / 31u;
        *b = (value & 31u) * 255u / 31u;
    } else {
        *r = ((value >> 11) & 31u) * 255u / 31u;
        *g = ((value >> 5) & 63u) * 255u / 63u;
        *b = (value & 31u) * 255u / 31u;
    }
}

void buildBaseLocked(int width, int height, int pitch, int bpp,
                     int rgb555, bool wide)
{
    const int bytesPerPixel = bpp / 8;
    const SIZE_T size = static_cast<SIZE_T>(pitch) * height;
    std::memset(g_base, 0, size);

    // Exact DisciplesGL wallpaper tile: [top|bottom] above [bottom|top],
    // yielding an effective 3072x1536 repeat from the 1536-square source.
    const unsigned tileWidth = g_back.width * 2u;
    const unsigned halfHeight = g_back.height / 2u;
    for (int y = 0; y < height; ++y) {
        unsigned tileY = (static_cast<unsigned>(y) + g_offsetY) % g_back.height;
        unsigned char* dst = g_base + static_cast<SIZE_T>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            unsigned tileX =
                (static_cast<unsigned>(x) + g_offsetX) % tileWidth;
            unsigned srcX = tileX;
            unsigned srcY = tileY;
            if (tileX >= g_back.width) {
                srcX -= g_back.width;
                srcY = (srcY + halfHeight) % g_back.height;
            }
            const unsigned char* rgba =
                g_back.rgba +
                (static_cast<SIZE_T>(srcY) * g_back.width + srcX) * 4u;
            writePixel(dst + static_cast<SIZE_T>(x) * bytesPerPixel,
                       bpp, rgb555, rgba[0], rgba[1], rgba[2]);
        }
    }

    // The screenshot supplied by the user uses DisciplesGL's Alternative
    // style.  The wide asset is exclusively for the 990x600 battle surface.
    const Image& frame = wide ? g_altWide : g_alt;
    const int left = (width - static_cast<int>(frame.width)) / 2;
    const int top = (height - static_cast<int>(frame.height)) / 2;
    for (unsigned sy = 0; sy < frame.height; ++sy) {
        const int dy = top + static_cast<int>(sy);
        if (dy < 0 || dy >= height)
            continue;
        for (unsigned sx = 0; sx < frame.width; ++sx) {
            const int dx = left + static_cast<int>(sx);
            if (dx < 0 || dx >= width)
                continue;
            const unsigned char* rgba =
                frame.rgba +
                (static_cast<SIZE_T>(sy) * frame.width + sx) * 4u;
            const unsigned alpha = rgba[3];
            if (!alpha)
                continue;

            unsigned char* dst = g_base + static_cast<SIZE_T>(dy) * pitch +
                                 static_cast<SIZE_T>(dx) * bytesPerPixel;
            if (alpha == 255) {
                writePixel(dst, bpp, rgb555, rgba[0], rgba[1], rgba[2]);
            } else {
                unsigned dr = 0, dg = 0, db = 0;
                readPixel(dst, bpp, rgb555, &dr, &dg, &db);
                const unsigned inv = 255u - alpha;
                const unsigned r = (rgba[0] * alpha + dr * inv + 127u) / 255u;
                const unsigned g = (rgba[1] * alpha + dg * inv + 127u) / 255u;
                const unsigned b = (rgba[2] * alpha + db * inv + 127u) / 255u;
                writePixel(dst, bpp, rgb555, r, g, b);
            }
        }
    }

    g_cacheWidth = width;
    g_cacheHeight = height;
    g_cachePitch = pitch;
    g_cacheBpp = bpp;
    g_cacheRgb555 = rgb555;
    g_cacheWide = wide ? 1 : 0;
}

} // namespace

extern "C" void decorative_install(void)
{
    InterlockedExchange(
        &g_enabled,
        GetPrivateProfileIntA("menu", "decorativeBackground", 1, menuIni())
            ? 1
            : 0);
}

extern "C" int decorative_get_enabled(void)
{
    return InterlockedExchangeAdd(&g_enabled, 0) != 0;
}

extern "C" int decorative_is_available(void)
{
    AcquireSRWLockExclusive(&g_lock);
    const bool ready = ensureAssetsLocked();
    ReleaseSRWLockExclusive(&g_lock);
    return ready ? 1 : 0;
}

extern "C" int decorative_set_enabled(int enabled)
{
    const char* value = enabled ? "1" : "0";
    if (!WritePrivateProfileStringA(
            "menu", "decorativeBackground", value, menuIni()))
        return 0;
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    DDInvalidateDecorativeFrame();
    return 1;
}

extern "C" const void* DDGetDecoratedSurface(
    const void* source, int width, int height, int pitch, int bpp, int rgb555)
{
    if (!source || InterlockedExchangeAdd(&g_enabled, 0) == 0 ||
        width <= 0 || height <= 0 || pitch <= 0 ||
        (bpp != 16 && bpp != 32))
        return source;

    int contentWidth = 0;
    int contentHeight = 0;
    int wide = 0;
    if (!horplus_get_decor_layout(
            &contentWidth, &contentHeight, &wide) ||
        contentWidth <= 0 || contentHeight <= 0 ||
        contentWidth > width || contentHeight > height)
        return source;

    const int bytesPerPixel = bpp / 8;
    if (pitch < width * bytesPerPixel ||
        static_cast<unsigned long long>(pitch) *
                static_cast<unsigned long long>(height) >
            (std::numeric_limits<SIZE_T>::max)())
        return source;
    const SIZE_T size = static_cast<SIZE_T>(pitch) * height;

    AcquireSRWLockExclusive(&g_lock);
    if (!ensureAssetsLocked() || !ensureBuffersLocked(size)) {
        ReleaseSRWLockExclusive(&g_lock);
        return source;
    }

    const int isWide = wide ? 1 : 0;
    if (g_cacheWidth != width || g_cacheHeight != height ||
        g_cachePitch != pitch || g_cacheBpp != bpp ||
        g_cacheRgb555 != (rgb555 ? 1 : 0) || g_cacheWide != isWide) {
        buildBaseLocked(width, height, pitch, bpp, rgb555 ? 1 : 0,
                        isWide != 0);
    }

    std::memcpy(g_present, g_base, size);
    const int left = (width - contentWidth) / 2;
    const int top = (height - contentHeight) / 2;
    const SIZE_T rowBytes =
        static_cast<SIZE_T>(contentWidth) * bytesPerPixel;
    const auto* src = static_cast<const unsigned char*>(source) +
                      static_cast<SIZE_T>(top) * pitch +
                      static_cast<SIZE_T>(left) * bytesPerPixel;
    auto* dst = g_present + static_cast<SIZE_T>(top) * pitch +
                static_cast<SIZE_T>(left) * bytesPerPixel;
    for (int y = 0; y < contentHeight; ++y) {
        std::memcpy(dst, src, rowBytes);
        src += pitch;
        dst += pitch;
    }

    const void* result = g_present;
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}
