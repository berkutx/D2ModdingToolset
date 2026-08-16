/*
 * Exact-build Disciples II software-cursor capture for the decorative compositor.
 *
 * The Russobit executable renders its cursor through CCursorImpl::draw.  Replacing that
 * cursor with a fixed HCURSOR loses context-sensitive and animated cursors.  This module
 * instead observes only the DrawTexture calls made by CCursorImpl, snapshots the already
 * decoded opaque/colour-keyed DirectDraw pixels on the game thread, and publishes owned POD
 * fragments that a presentation compositor can replay outside its copied game rectangle.
 *
 * Important lifetime rule: no game pointer, TextureHandle or DirectDraw surface crosses the
 * publication boundary.  cursorcapture_replay() reads only immutable wrapper-owned storage.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ddraw.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int DDSnapshotCursorSurfaceArgb(void* surface7,
                                              int sourceX,
                                              int sourceY,
                                              int width,
                                              int height,
                                              DWORD* destination,
                                              DWORD capacity,
                                              int* anyOpaque);

namespace {

static_assert(sizeof(void*) == 4, "cursor capture is an x86-only game ABI hook");

constexpr std::uintptr_t kExpectedImageBase = 0x00400000u;
constexpr DWORD kRussobitExeSize = 4187648u;
constexpr DWORD kRussobitCustomIconExeSize = 4214272u;

constexpr std::uintptr_t kCursorPresentationVtable = 0x006E3EBCu;
constexpr std::uintptr_t kCursorDrawSlot = 0x006E3EC0u;
constexpr std::uintptr_t kCursorDrawEntry = 0x00540AC4u;
constexpr std::uintptr_t kRendererVtable = 0x006E2564u;
constexpr std::uintptr_t kRendererDrawTexture = 0x00519330u;
constexpr std::uintptr_t kFindTextureSurface = 0x0051B300u;
constexpr std::uintptr_t kFindRenderData = 0x0051CB90u;
constexpr std::uintptr_t kComputeTile = 0x005191D0u;

constexpr unsigned kRendererVtableEntries = 13;
constexpr unsigned kMaxDrawOperations = 16;
constexpr unsigned kMaxTextureTiles = 20;
constexpr unsigned kMaxFragments = 64;
constexpr unsigned kMaxFragmentWidth = 256;
constexpr unsigned kMaxFragmentHeight = 256;
constexpr unsigned kMaxPixels = 131072;
constexpr int kMaxCoordinateMagnitude = 32768;
constexpr unsigned kFrameSlots = 3;

struct MqPoint
{
    int x;
    int y;
};

struct MqRect
{
    int left;
    int top;
    int right;
    int bottom;
};

struct TextureHandle
{
    MqPoint textureSize;
    std::uint32_t* indexPtr;
    int* refCount;
};

struct RenderData22
{
    void* texture;
    MqPoint textureSize;
    std::uint32_t paletteKey;
    std::uint32_t hint;
    bool hasCustomPalette;
    char padding;
    std::int16_t opacity;
};

struct TextureSurface
{
    int surfacesCount;
    IDirectDrawSurface7* surfaces[kMaxTextureTiles];
    MqRect areas[kMaxTextureTiles];
    bool unknown;
    char padding[3];
    int batchNumber;
    std::uint32_t hint;
};

struct Renderer
{
    void** vtable;
};

static_assert(sizeof(MqPoint) == 8, "Russobit CMqPoint ABI");
static_assert(sizeof(MqRect) == 16, "Russobit CMqRect ABI");
static_assert(sizeof(TextureHandle) == 16, "Russobit TextureHandle ABI");
static_assert(sizeof(RenderData22) == 24, "Russobit RenderData22 ABI");
static_assert(offsetof(RenderData22, opacity) == 22, "Russobit opacity offset");
static_assert(sizeof(TextureSurface) == 416, "Russobit TextureSurface ABI");
static_assert(offsetof(TextureSurface, surfaces) == 4, "Russobit surface-array offset");
static_assert(offsetof(TextureSurface, areas) == 84, "Russobit tile-area offset");

struct CursorFragment
{
    int x;
    int y;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pixelOffset;
};

struct CursorFrame
{
    DWORD tick;
    std::uint32_t sequence;
    LONG suppressionGeneration;
    std::uint32_t fragmentCount;
    std::uint32_t pixelCount;
    CursorFragment fragments[kMaxFragments];
    // Pixels are 0xAARRGGBB.  Alpha is deliberately binary: zero or 255.
    std::uint32_t pixels[kMaxPixels];
};

struct FrameSlot
{
    // -1: writer owns the slot; >= 0: number of readers.
    volatile LONG state;
    CursorFrame frame;
};

struct CaptureContext
{
    Renderer* renderer;
    void** originalRendererVtable;
    bool active;
    bool intercepting;
    bool valid;
    bool suppressed;
    LONG suppressionGeneration;
    unsigned drawOperations;
    CursorFrame scratch;
};

using CursorDrawFn = void(__thiscall*)(void* presentationThis, Renderer* renderer);
using DrawTextureFn = void(__thiscall*)(Renderer* renderer,
                                        TextureHandle* textureHandle,
                                        const MqPoint* start,
                                        const MqPoint* offset,
                                        const MqPoint* size,
                                        const MqRect* area);
using FindTextureSurfaceFn = TextureSurface*(__thiscall*)(void* mapHandle,
                                                          std::uint32_t key);
using FindRenderDataFn = RenderData22*(__thiscall*)(void* setHandle,
                                                    const TextureHandle* key);
using ComputeTileFn = BOOL(__stdcall*)(MqPoint* sourcePosition,
                                       MqPoint* destinationPosition,
                                       MqPoint* visibleSize,
                                       const MqPoint* sourceOffset,
                                       const MqPoint* destinationStart,
                                       const MqPoint* requestedSize,
                                       const MqRect* operationClip,
                                       const MqRect* tileArea);

CursorDrawFn g_originalCursorDraw = reinterpret_cast<CursorDrawFn>(kCursorDrawEntry);
DrawTextureFn g_originalDrawTexture = reinterpret_cast<DrawTextureFn>(kRendererDrawTexture);
FindTextureSurfaceFn g_findTextureSurface =
    reinterpret_cast<FindTextureSurfaceFn>(kFindTextureSurface);
FindRenderDataFn g_findRenderData = reinterpret_cast<FindRenderDataFn>(kFindRenderData);
ComputeTileFn g_computeTile = reinterpret_cast<ComputeTileFn>(kComputeTile);

void* g_captureRendererVtable[kRendererVtableEntries] = {};
DWORD g_tlsIndex = TLS_OUT_OF_INDEXES;
volatile LONG g_installState = 0; // 0 not attempted, 2 installing, 1 installed, -1 unavailable
volatile LONG g_suppressCursor = 1; // Windows menu/non-client owns the visible pointer until HTCLIENT hover
volatile LONG g_suppressionGeneration = 0; // even=stable, odd=mode transition in progress
volatile LONG g_activeFrameSlot = 0;
volatile LONG g_frameSequence = 0;
FrameSlot g_frames[kFrameSlots] = {};

volatile LONG g_loggedBadOpacity = 0;
volatile LONG g_loggedCaptureFailure = 0;
volatile LONG g_loggedPublishDrop = 0;

const std::uint32_t kExpectedCursorVtable[3] = {
    0x00540CD8u, 0x00540AC4u, 0x00000000u};

const std::uint8_t kExpectedCursorDrawBytes[17] = {
    0xB8, 0x0C, 0xC6, 0x6A, 0x00, 0xE8, 0x02, 0xC9, 0x12,
    0x00, 0x83, 0xEC, 0x20, 0x56, 0x57, 0x8B, 0xF9};

const std::uint32_t kExpectedRendererVtable[kRendererVtableEntries] = {
    0x00518420u, 0x00518430u, 0x00518440u, 0x00518460u, 0x00519330u,
    0x005193D0u, 0x005193E0u, 0x005199B0u, 0x005199D0u, 0x00519C90u,
    0x00519CC0u, 0x00519CD0u, 0x00519CF0u};

const std::uint8_t kExpectedDrawTextureBytes[17] = {
    0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x33, 0xC0,
    0x8B, 0xF1, 0x8B, 0x57, 0x08, 0x85, 0xD2, 0x0F};

const std::uint8_t kExpectedFindTextureSurfaceBytes[9] = {
    0x83, 0xEC, 0x24, 0x8D, 0x44, 0x24, 0x28, 0x56, 0x57};

const std::uint8_t kExpectedFindRenderDataBytes[9] = {
    0x83, 0xEC, 0x30, 0x8D, 0x44, 0x24, 0x00, 0x53, 0x8B};

const std::uint8_t kExpectedComputeTileBytes[12] = {
    0x8B, 0x4C, 0x24, 0x10, 0x53, 0x55,
    0x8B, 0x6C, 0x24, 0x0C, 0x8B, 0x01};

void debugLine(const char* text)
{
    OutputDebugStringA(text);
}

bool executableSizeMatches()
{
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path))))
        return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes) ||
        attributes.nFileSizeHigh != 0)
        return false;
    return attributes.nFileSizeLow == kRussobitExeSize ||
           attributes.nFileSizeLow == kRussobitCustomIconExeSize;
}

bool committedMemory(const void* address, SIZE_T size, bool executable)
{
    const auto* cursor = static_cast<const std::uint8_t*>(address);
    SIZE_T remaining = size;
    while (remaining) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(cursor, &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) ||
            memory.Protect == PAGE_NOACCESS)
            return false;
        if (executable) {
            const DWORD protection = memory.Protect & 0xFFu;
            if (protection != PAGE_EXECUTE &&
                protection != PAGE_EXECUTE_READ &&
                protection != PAGE_EXECUTE_READWRITE &&
                protection != PAGE_EXECUTE_WRITECOPY)
                return false;
        }
        const auto* regionEnd = static_cast<const std::uint8_t*>(memory.BaseAddress) +
                                memory.RegionSize;
        if (regionEnd <= cursor)
            return false;
        const SIZE_T available = static_cast<SIZE_T>(regionEnd - cursor);
        if (available >= remaining)
            return true;
        cursor += available;
        remaining -= available;
    }
    return true;
}

bool exactBuildGate()
{
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)) !=
            kExpectedImageBase ||
        !executableSizeMatches())
        return false;

    if (!committedMemory(reinterpret_cast<const void*>(kCursorPresentationVtable),
                         sizeof(kExpectedCursorVtable), false) ||
        !committedMemory(reinterpret_cast<const void*>(kCursorDrawEntry),
                         sizeof(kExpectedCursorDrawBytes), true) ||
        !committedMemory(reinterpret_cast<const void*>(kRendererVtable),
                         sizeof(kExpectedRendererVtable), false) ||
        !committedMemory(reinterpret_cast<const void*>(kRendererDrawTexture),
                         sizeof(kExpectedDrawTextureBytes), true) ||
        !committedMemory(reinterpret_cast<const void*>(kFindTextureSurface),
                         sizeof(kExpectedFindTextureSurfaceBytes), true) ||
        !committedMemory(reinterpret_cast<const void*>(kFindRenderData),
                         sizeof(kExpectedFindRenderDataBytes), true) ||
        !committedMemory(reinterpret_cast<const void*>(kComputeTile),
                         sizeof(kExpectedComputeTileBytes), true))
        return false;

    bool matches = false;
    __try {
        matches =
            std::memcmp(reinterpret_cast<const void*>(kCursorPresentationVtable),
                        kExpectedCursorVtable, sizeof(kExpectedCursorVtable)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(kCursorDrawEntry),
                        kExpectedCursorDrawBytes, sizeof(kExpectedCursorDrawBytes)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(kRendererVtable),
                        kExpectedRendererVtable, sizeof(kExpectedRendererVtable)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(kRendererDrawTexture),
                        kExpectedDrawTextureBytes, sizeof(kExpectedDrawTextureBytes)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(kFindTextureSurface),
                        kExpectedFindTextureSurfaceBytes,
                        sizeof(kExpectedFindTextureSurfaceBytes)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(kFindRenderData),
                        kExpectedFindRenderDataBytes,
                        sizeof(kExpectedFindRenderDataBytes)) == 0 &&
            std::memcmp(reinterpret_cast<const void*>(kComputeTile),
                        kExpectedComputeTileBytes, sizeof(kExpectedComputeTileBytes)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        matches = false;
    }
    return matches;
}

CaptureContext* captureContext()
{
    if (g_tlsIndex == TLS_OUT_OF_INDEXES)
        return nullptr;
    auto* context = static_cast<CaptureContext*>(TlsGetValue(g_tlsIndex));
    if (context)
        return context;
    context = static_cast<CaptureContext*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CaptureContext)));
    if (!context || !TlsSetValue(g_tlsIndex, context)) {
        if (context)
            HeapFree(GetProcessHeap(), 0, context);
        return nullptr;
    }
    return context;
}

CaptureContext* existingCaptureContext()
{
    return g_tlsIndex == TLS_OUT_OF_INDEXES
        ? nullptr
        : static_cast<CaptureContext*>(TlsGetValue(g_tlsIndex));
}

void clearScratch(CaptureContext* context)
{
    context->renderer = nullptr;
    context->originalRendererVtable = nullptr;
    context->active = false;
    context->intercepting = false;
    context->valid = true;
    context->suppressed = true;
    context->suppressionGeneration = 0;
    context->drawOperations = 0;
    context->scratch.tick = 0;
    context->scratch.sequence = 0;
    context->scratch.suppressionGeneration = 0;
    context->scratch.fragmentCount = 0;
    context->scratch.pixelCount = 0;
}

bool safeCoordinate(int value)
{
    return value >= -kMaxCoordinateMagnitude && value <= kMaxCoordinateMagnitude;
}

bool appendSurfaceFragmentReferenced(CaptureContext* context,
                                     IDirectDrawSurface7* surface,
                                     const MqRect& tileArea,
                                     const MqPoint& sourcePosition,
                                     const MqPoint& destinationPosition,
                                     const MqPoint& visibleSize)
{
    if (!context || !surface || visibleSize.x <= 0 || visibleSize.y <= 0 ||
        visibleSize.x > static_cast<int>(kMaxFragmentWidth) ||
        visibleSize.y > static_cast<int>(kMaxFragmentHeight) ||
        !safeCoordinate(destinationPosition.x) ||
        !safeCoordinate(destinationPosition.y) ||
        context->scratch.fragmentCount >= kMaxFragments)
        return false;

    const std::uint64_t fragmentPixels =
        static_cast<std::uint64_t>(visibleSize.x) *
        static_cast<std::uint64_t>(visibleSize.y);
    if (!fragmentPixels || fragmentPixels > kMaxPixels ||
        context->scratch.pixelCount > kMaxPixels - fragmentPixels)
        return false;

    int sourceX = sourcePosition.x;
    int sourceY = sourcePosition.y;
    CursorFragment& fragment =
        context->scratch.fragments[context->scratch.fragmentCount];
    fragment.x = destinationPosition.x;
    fragment.y = destinationPosition.y;
    fragment.width = static_cast<std::uint32_t>(visibleSize.x);
    fragment.height = static_cast<std::uint32_t>(visibleSize.y);
    fragment.pixelOffset = context->scratch.pixelCount;

    int anyOpaque = 0;
    auto* destination = reinterpret_cast<DWORD*>(
        context->scratch.pixels + fragment.pixelOffset);
    const DWORD capacity = static_cast<DWORD>(kMaxPixels - fragment.pixelOffset);
    int copied = DDSnapshotCursorSurfaceArgb(
        surface, sourceX, sourceY, visibleSize.x, visibleSize.y,
        destination, capacity, &anyOpaque);
    if (!copied) {
        // TextureSurface::areas are texture-space rectangles while decomposed DirectDraw surfaces
        // can be tile-local. Retry with tile-local coordinates without ever touching the wrapper's
        // internal surface fields or entering its message-pumping COM Lock path.
        const std::int64_t localX = static_cast<std::int64_t>(sourceX) - tileArea.left;
        const std::int64_t localY = static_cast<std::int64_t>(sourceY) - tileArea.top;
        if (localX < INT_MIN || localX > INT_MAX ||
            localY < INT_MIN || localY > INT_MAX)
            return false;
        sourceX = static_cast<int>(localX);
        sourceY = static_cast<int>(localY);
        copied = DDSnapshotCursorSurfaceArgb(
            surface, sourceX, sourceY, visibleSize.x, visibleSize.y,
            destination, capacity, &anyOpaque);
    }
    if (!copied)
        return false;
    if (anyOpaque) {
        context->scratch.pixelCount += static_cast<std::uint32_t>(fragmentPixels);
        ++context->scratch.fragmentCount;
    }
    return true;
}

bool appendSurfaceFragment(CaptureContext* context,
                           IDirectDrawSurface7* surface,
                           const MqRect& tileArea,
                           const MqPoint& sourcePosition,
                           const MqPoint& destinationPosition,
                           const MqPoint& visibleSize)
{
    return appendSurfaceFragmentReferenced(
        context, surface, tileArea, sourcePosition, destinationPosition, visibleSize);
}

bool captureTexture(CaptureContext* context,
                    Renderer* renderer,
                    TextureHandle* textureHandle,
                    const MqPoint* start,
                    const MqPoint* offset,
                    const MqPoint* size,
                    const MqRect* area)
{
    if (!context || !renderer || !textureHandle || !textureHandle->indexPtr ||
        !start || !offset || !size || !area || size->x <= 0 || size->y <= 0)
        return false;

    auto* rendererBytes = reinterpret_cast<std::uint8_t*>(renderer);
    void* dataSet = *reinterpret_cast<void**>(rendererBytes + 0x68);
    void* textureMap = *reinterpret_cast<void**>(rendererBytes + 0x70);
    if (!dataSet || !textureMap)
        return false;

    RenderData22* renderData = g_findRenderData(dataSet, textureHandle);
    if (!renderData)
        return false;
    if (renderData->opacity != 255) {
        if (InterlockedCompareExchange(&g_loggedBadOpacity, 1, 0) == 0)
            debugLine("[cursorcapture] non-opaque cursor rejected (opacity != 255)\n");
        return false;
    }

    const std::uint32_t textureKey = *textureHandle->indexPtr;
    TextureSurface* textureSurface =
        g_findTextureSurface(textureMap, textureKey);
    if (!textureSurface || textureSurface->surfacesCount <= 0 ||
        textureSurface->surfacesCount > static_cast<int>(kMaxTextureTiles))
        return false;

    for (int tile = 0; tile < textureSurface->surfacesCount; ++tile) {
        MqPoint sourcePosition = {};
        MqPoint destinationPosition = {};
        MqPoint visibleSize = {};
        if (!g_computeTile(&sourcePosition, &destinationPosition, &visibleSize,
                           offset, start, size, area,
                           &textureSurface->areas[tile]))
            continue;
        if (visibleSize.x <= 0 || visibleSize.y <= 0)
            continue;
        if (!appendSurfaceFragment(context, textureSurface->surfaces[tile],
                                   textureSurface->areas[tile], sourcePosition,
                                   destinationPosition, visibleSize))
            return false;
    }
    return true;
}

bool safeCaptureTexture(CaptureContext* context,
                        Renderer* renderer,
                        TextureHandle* textureHandle,
                        const MqPoint* start,
                        const MqPoint* offset,
                        const MqPoint* size,
                        const MqRect* area)
{
    bool captured = false;
    __try {
        captured = captureTexture(context, renderer, textureHandle,
                                  start, offset, size, area);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        captured = false;
    }
    if (!captured &&
        InterlockedCompareExchange(&g_loggedCaptureFailure, 1, 0) == 0)
        debugLine("[cursorcapture] cursor fragment capture failed closed\n");
    return captured;
}

void publishFrame(const CursorFrame* source)
{
    const LONG active = InterlockedCompareExchange(&g_activeFrameSlot, 0, 0);
    for (unsigned attempt = 0; attempt < kFrameSlots; ++attempt) {
        const unsigned index = (static_cast<unsigned>(active) + 1u + attempt) %
                               kFrameSlots;
        if (index == static_cast<unsigned>(active))
            continue;
        FrameSlot& slot = g_frames[index];
        if (InterlockedCompareExchange(&slot.state, -1, 0) != 0)
            continue;

        slot.frame.tick = GetTickCount();
        slot.frame.sequence = static_cast<std::uint32_t>(
            InterlockedIncrement(&g_frameSequence));
        slot.frame.suppressionGeneration = source
            ? source->suppressionGeneration
            : InterlockedCompareExchange(&g_suppressionGeneration, 0, 0);
        slot.frame.fragmentCount = source ? source->fragmentCount : 0;
        slot.frame.pixelCount = source ? source->pixelCount : 0;
        if (source && source->fragmentCount)
            std::memcpy(slot.frame.fragments, source->fragments,
                        source->fragmentCount * sizeof(CursorFragment));
        if (source && source->pixelCount)
            std::memcpy(slot.frame.pixels, source->pixels,
                        source->pixelCount * sizeof(std::uint32_t));

        MemoryBarrier();
        InterlockedExchange(&g_activeFrameSlot, static_cast<LONG>(index));
        InterlockedExchange(&slot.state, 0);
        return;
    }

    if (InterlockedCompareExchange(&g_loggedPublishDrop, 1, 0) == 0)
        debugLine("[cursorcapture] all publication slots busy; frame dropped\n");
}

FrameSlot* acquirePublishedFrame()
{
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const LONG index = InterlockedCompareExchange(&g_activeFrameSlot, 0, 0);
        if (index < 0 || index >= static_cast<LONG>(kFrameSlots))
            return nullptr;
        FrameSlot& slot = g_frames[index];
        LONG readers = InterlockedCompareExchange(&slot.state, 0, 0);
        while (readers >= 0) {
            const LONG acquired = InterlockedCompareExchange(
                &slot.state, readers + 1, readers);
            if (acquired == readers) {
                MemoryBarrier();
                if (InterlockedCompareExchange(&g_activeFrameSlot, 0, 0) == index)
                    return &slot;
                InterlockedDecrement(&slot.state);
                break;
            }
            readers = acquired;
        }
        YieldProcessor();
    }
    return nullptr;
}

void releasePublishedFrame(FrameSlot* slot)
{
    if (slot)
        InterlockedDecrement(&slot->state);
}

void __fastcall captureDrawTextureThunk(Renderer* renderer,
                                        void* /*edx*/,
                                        TextureHandle* textureHandle,
                                        const MqPoint* start,
                                        const MqPoint* offset,
                                        const MqPoint* size,
                                        const MqRect* area)
{
    // The renderer vtable is shared, so another renderer thread may briefly enter this thunk while
    // the cursor thread owns the swap.  Never allocate a half-megabyte TLS scratch block for such
    // unrelated work; only cursorDrawThunk creates a context for its owning thread.
    CaptureContext* context = existingCaptureContext();
    const bool ownedOperation = context && context->active && context->intercepting &&
        context->renderer == renderer;
    if (!ownedOperation) {
        // A DirectDraw Lock can pump messages. Any re-entrant renderer work is not part of the
        // cursor operation currently being sampled and must retain the native renderer vtable
        // semantics regardless of cursor visibility.
        g_originalDrawTexture(renderer, textureHandle, start, offset, size, area);
        return;
    }

    const bool suppressed = context->suppressed;
    if (!suppressed && context->valid) {
        ++context->drawOperations;
        bool captured = false;
        context->intercepting = false;
        __try {
            captured = context->drawOperations <= kMaxDrawOperations &&
                safeCaptureTexture(context, renderer, textureHandle,
                                   start, offset, size, area);
        } __finally {
            context->intercepting = true;
        }
        if (!captured)
            context->valid = false;
    }

    // CCursorImpl itself must always run so its SmartPtr/state cleanup remains native.  Only the
    // texture operations are suppressed while a real Windows menu/non-client cursor owns the
    // pointer.  In game-client mode capture is observational and the native operation is forwarded.
    if (!suppressed)
        g_originalDrawTexture(renderer, textureHandle, start, offset, size, area);
}

void __fastcall cursorDrawThunk(void* presentationThis,
                                void* /*edx*/,
                                Renderer* renderer)
{
    CaptureContext* context = captureContext();
    if (!context || context->active || !renderer) {
        g_originalCursorDraw(presentationThis, renderer);
        return;
    }

    clearScratch(context);
    LONG generationBefore = 0;
    LONG generationAfter = 0;
    do {
        generationBefore =
            InterlockedCompareExchange(&g_suppressionGeneration, 0, 0);
        if (generationBefore & 1) {
            YieldProcessor();
            continue;
        }
        context->suppressed =
            InterlockedCompareExchange(&g_suppressCursor, 0, 0) != 0;
        MemoryBarrier();
        generationAfter =
            InterlockedCompareExchange(&g_suppressionGeneration, 0, 0);
    } while (generationBefore != generationAfter || (generationAfter & 1));
    context->suppressionGeneration = generationAfter;
    context->scratch.suppressionGeneration = generationAfter;
    void** rendererVtable = nullptr;
    __try {
        rendererVtable = renderer->vtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rendererVtable = nullptr;
    }
    if (rendererVtable != reinterpret_cast<void**>(kRendererVtable)) {
        publishFrame(nullptr);
        g_originalCursorDraw(presentationThis, renderer);
        return;
    }

    context->renderer = renderer;
    context->originalRendererVtable = rendererVtable;
    context->active = true;
    context->intercepting = true;
    void* previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(&renderer->vtable),
        g_captureRendererVtable, rendererVtable);
    if (previous != rendererVtable) {
        context->active = false;
        context->valid = false;
        publishFrame(nullptr);
        g_originalCursorDraw(presentationThis, renderer);
        return;
    }

    bool completed = false;
    __try {
        g_originalCursorDraw(presentationThis, renderer);
        completed = true;
    } __finally {
        InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(&renderer->vtable),
            rendererVtable, g_captureRendererVtable);
        context->active = false;
        context->intercepting = false;
        context->renderer = nullptr;
        context->originalRendererVtable = nullptr;
        const LONG currentGeneration =
            InterlockedCompareExchange(&g_suppressionGeneration, 0, 0);
        if (currentGeneration != context->suppressionGeneration) {
            // A newer owner transition already cleared/published its own state. Never let this
            // older draw overwrite it, even with an empty frame.
        } else if (!completed || !context->valid || context->suppressed) {
            publishFrame(nullptr);
        } else {
            publishFrame(&context->scratch);
        }
    }
}

void writeDestinationPixel(std::uint8_t* destination, int bpp, int rgb555,
                           std::uint32_t colour)
{
    const unsigned red = (colour >> 16u) & 0xFFu;
    const unsigned green = (colour >> 8u) & 0xFFu;
    const unsigned blue = colour & 0xFFu;
    if (bpp == 32) {
        destination[0] = static_cast<std::uint8_t>(blue);
        destination[1] = static_cast<std::uint8_t>(green);
        destination[2] = static_cast<std::uint8_t>(red);
        destination[3] = 0xFF;
        return;
    }

    std::uint16_t pixel = 0;
    if (rgb555) {
        pixel = static_cast<std::uint16_t>(
            ((red >> 3u) << 10u) | ((green >> 3u) << 5u) | (blue >> 3u));
    } else {
        pixel = static_cast<std::uint16_t>(
            ((red >> 3u) << 11u) | ((green >> 2u) << 5u) | (blue >> 3u));
    }
    std::memcpy(destination, &pixel, sizeof(pixel));
}

} // namespace

extern "C" int cursorcapture_install(void)
{
    const LONG state = InterlockedCompareExchange(&g_installState, 2, 0);
    if (state == 1)
        return 1;
    if (state != 0)
        return 0;

    if (!exactBuildGate()) {
        debugLine("[cursorcapture] unsupported executable or signature mismatch\n");
        InterlockedExchange(&g_installState, -1);
        return 0;
    }

    g_tlsIndex = TlsAlloc();
    if (g_tlsIndex == TLS_OUT_OF_INDEXES) {
        debugLine("[cursorcapture] TLS allocation failed\n");
        InterlockedExchange(&g_installState, -1);
        return 0;
    }

    std::memcpy(g_captureRendererVtable,
                reinterpret_cast<const void*>(kRendererVtable),
                sizeof(g_captureRendererVtable));
    g_captureRendererVtable[4] =
        reinterpret_cast<void*>(&captureDrawTextureThunk);

    DWORD oldProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kCursorDrawSlot), sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &oldProtection)) {
        TlsFree(g_tlsIndex);
        g_tlsIndex = TLS_OUT_OF_INDEXES;
        debugLine("[cursorcapture] cursor draw slot is not writable\n");
        InterlockedExchange(&g_installState, -1);
        return 0;
    }

    void* previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(kCursorDrawSlot),
        reinterpret_cast<void*>(&cursorDrawThunk),
        reinterpret_cast<void*>(kCursorDrawEntry));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(kCursorDrawSlot), sizeof(void*),
                   oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(kCursorDrawSlot), sizeof(void*));

    if (previous != reinterpret_cast<void*>(kCursorDrawEntry)) {
        TlsFree(g_tlsIndex);
        g_tlsIndex = TLS_OUT_OF_INDEXES;
        debugLine("[cursorcapture] cursor draw slot already belongs to another hook\n");
        InterlockedExchange(&g_installState, -1);
        return 0;
    }

    publishFrame(nullptr);
    InterlockedExchange(&g_installState, 1);
    debugLine("[cursorcapture] dynamic software cursor capture installed\n");
    return 1;
}

extern "C" int cursorcapture_is_available(void)
{
    return InterlockedCompareExchange(&g_installState, 0, 0) == 1 ? 1 : 0;
}

extern "C" void cursorcapture_set_suppressed(int suppressed)
{
    const LONG next = suppressed ? 1 : 0;
    if (InterlockedCompareExchange(&g_suppressCursor, 0, 0) == next)
        return;
    InterlockedIncrement(&g_suppressionGeneration); // odd: readers retry
    const LONG previous = InterlockedExchange(&g_suppressCursor, next);
    MemoryBarrier();
    InterlockedIncrement(&g_suppressionGeneration); // even: new owner is stable
    if (previous != next)
        publishFrame(nullptr);
}

extern "C" void cursorcapture_clear(void)
{
    publishFrame(nullptr);
}

extern "C" int cursorcapture_replay(void* destination,
                                    int width,
                                    int height,
                                    int pitch,
                                    int bpp,
                                    int rgb555,
                                    int contentLeft,
                                    int contentTop,
                                    int contentWidth,
                                    int contentHeight)
{
    if (InterlockedCompareExchange(&g_installState, 0, 0) != 1 ||
        InterlockedCompareExchange(&g_suppressCursor, 0, 0) != 0 ||
        !destination || width <= 0 || height <= 0 || pitch <= 0 ||
        (bpp != 16 && bpp != 32) || contentWidth <= 0 || contentHeight <= 0 ||
        contentLeft < 0 || contentTop < 0 || contentWidth > width - contentLeft ||
        contentHeight > height - contentTop)
        return 0;

    const int bytesPerPixel = bpp / 8;
    if (pitch < width * bytesPerPixel)
        return 0;

    FrameSlot* slot = acquirePublishedFrame();
    if (!slot)
        return 0;
    const CursorFrame& frame = slot->frame;
    int written = 0;

    const LONG currentGeneration =
        InterlockedCompareExchange(&g_suppressionGeneration, 0, 0);
    if (!(currentGeneration & 1) &&
        frame.suppressionGeneration == currentGeneration &&
        // An inactive D2 renderer may legitimately sleep for longer than one second while its
        // last software cursor is still the current visual state. Ownership transitions publish
        // an empty frame and advance the generation, so wall-clock expiry only made the cursor
        // disappear under a stationary pointer; it was never a valid stale-frame guard.
        frame.tick &&
        frame.fragmentCount <= kMaxFragments && frame.pixelCount <= kMaxPixels) {
        auto* output = static_cast<std::uint8_t*>(destination);
        const int contentRight = contentLeft + contentWidth;
        const int contentBottom = contentTop + contentHeight;
        for (std::uint32_t i = 0; i < frame.fragmentCount; ++i) {
            const CursorFragment& fragment = frame.fragments[i];
            const std::uint64_t count =
                static_cast<std::uint64_t>(fragment.width) * fragment.height;
            if (!fragment.width || !fragment.height ||
                fragment.width > kMaxFragmentWidth ||
                fragment.height > kMaxFragmentHeight ||
                fragment.pixelOffset > frame.pixelCount ||
                count > frame.pixelCount - fragment.pixelOffset)
                break;

            for (std::uint32_t y = 0; y < fragment.height; ++y) {
                const int destinationY = fragment.y + static_cast<int>(y);
                if (destinationY < 0 || destinationY >= height)
                    continue;
                for (std::uint32_t x = 0; x < fragment.width; ++x) {
                    const int destinationX = fragment.x + static_cast<int>(x);
                    if (destinationX < 0 || destinationX >= width)
                        continue;
                    if (destinationX >= contentLeft && destinationX < contentRight &&
                        destinationY >= contentTop && destinationY < contentBottom)
                        continue;

                    const std::uint32_t colour = frame.pixels[
                        fragment.pixelOffset + y * fragment.width + x];
                    if ((colour >> 24u) != 0xFFu)
                        continue;
                    writeDestinationPixel(
                        output + static_cast<SIZE_T>(destinationY) * pitch +
                            static_cast<SIZE_T>(destinationX) * bytesPerPixel,
                        bpp, rgb555, colour);
                    ++written;
                }
            }
        }
    }

    releasePublishedFrame(slot);
    return written;
}
