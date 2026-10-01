/* Exact-build RGB565 software blending acceleration. Installation only; no game state polling. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "blend565.h"
#include "blend565_install.h"

namespace {

static_assert(sizeof(void*) == 4, "RGB565 native blending has an x86-only ABI");
static_assert(sizeof(c4blend565::Point) == 8 && sizeof(c4blend565::Size) == 8,
              "Native blend point/size ABI");

using AlphaFn = void(__stdcall*)(const void*, int, const c4blend565::Point*,
                                 void*, int, const c4blend565::Point*,
                                 const c4blend565::Size*, int, std::uint32_t);
using AddSubFn = void(__stdcall*)(const void*, int, const c4blend565::Point*,
                                  void*, int, const c4blend565::Point*,
                                  const c4blend565::Size*);

AlphaFn g_originalHalf = nullptr;
AddSubFn g_originalAdd = nullptr;
AddSubFn g_originalSubtract = nullptr;

enum State : LONG {
    NotAttempted = 0, Installing = 1, Installed = 2, CpuUnavailable = -1,
    UnsupportedImage = -2, ForeignCode = -3, ProtectFailed = -4,
    PatchCollision = -5, RestoreFailed = -6
};
volatile LONG g_state = NotAttempted;

void __stdcall halfHook(const void* source, int sourcePitch, const c4blend565::Point* sourcePos,
                        void* target, int targetPitch, const c4blend565::Point* targetPos,
                        const c4blend565::Size* size, int opacity, std::uint32_t key)
{
    if (!c4blend565::apply(c4blend565::Half, source, sourcePitch, sourcePos,
                          target, targetPitch, targetPos, size, opacity, key, true))
        g_originalHalf(source, sourcePitch, sourcePos, target, targetPitch,
                       targetPos, size, opacity, key);
}

void __stdcall addHook(const void* source, int sourcePitch, const c4blend565::Point* sourcePos,
                       void* target, int targetPitch, const c4blend565::Point* targetPos,
                       const c4blend565::Size* size)
{
    if (!c4blend565::apply(c4blend565::Add, source, sourcePitch, sourcePos,
                          target, targetPitch, targetPos, size, 300, 0, true))
        g_originalAdd(source, sourcePitch, sourcePos, target, targetPitch, targetPos, size);
}

void __stdcall subtractHook(const void* source, int sourcePitch, const c4blend565::Point* sourcePos,
                            void* target, int targetPitch, const c4blend565::Point* targetPos,
                            const c4blend565::Size* size)
{
    if (!c4blend565::apply(c4blend565::Subtract, source, sourcePitch, sourcePos,
                          target, targetPitch, targetPos, size, 400, 0, true))
        g_originalSubtract(source, sourcePitch, sourcePos, target, targetPitch, targetPos, size);
}

bool readableImageMemory(std::uintptr_t address, std::size_t bytes, bool executable)
{
    if (!bytes || address > UINTPTR_MAX - bytes)
        return false;
    const std::uintptr_t end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            reinterpret_cast<std::uintptr_t>(memory.AllocationBase) !=
                c4blend565install::kImageBase ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD protection = memory.Protect & 0xFF;
        const bool canRead = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                             protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                             protection == PAGE_EXECUTE_READWRITE ||
                             protection == PAGE_EXECUTE_WRITECOPY;
        const bool canExecute = protection == PAGE_EXECUTE_READ ||
                                protection == PAGE_EXECUTE_READWRITE ||
                                protection == PAGE_EXECUTE_WRITECOPY;
        if (!canRead || (executable && !canExecute))
            return false;
        const std::uintptr_t region = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > UINTPTR_MAX - region || region + memory.RegionSize <= address)
            return false;
        address = region + memory.RegionSize;
    }
    return true;
}

State exactImageGate()
{
    using namespace c4blend565install;
    HMODULE image = GetModuleHandleW(nullptr);
    if (reinterpret_cast<std::uintptr_t>(image) != kImageBase ||
        !readableImageMemory(kImageBase, 4096, false))
        return UnsupportedImage;
    // Only bounded reads of the existing mapped image; safe during DLL initialization.
    // No file reads, provider loading, CryptoAPI, heap allocation, or trace initialization.
    __try {
        std::uint8_t header[4096];
        std::memcpy(header, image, sizeof(header));
        PeLayout layout = {};
        if (!validPe(header, sizeof(header), reinterpret_cast<std::uintptr_t>(image), layout))
            return UnsupportedImage;

        HRSRC resource = FindResourceW(image, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
        if (!resource)
            return UnsupportedImage;
        const DWORD versionBytes = SizeofResource(image, resource);
        const HGLOBAL loaded = LoadResource(image, resource);
        const auto* version = static_cast<const std::uint8_t*>(LockResource(loaded));
        const auto versionAddress = reinterpret_cast<std::uintptr_t>(version);
        if (!version || versionBytes > 65536 || versionAddress < kImageBase ||
            !contains(layout.imageSize, versionAddress - kImageBase, versionBytes) ||
            !readableImageMemory(versionAddress, versionBytes, false) ||
            !validVersion(version, versionBytes))
            return UnsupportedImage;

        if (!contains(layout.imageSize, kVtable - kImageBase, sizeof(kExpectedVtable)) ||
            !readableImageMemory(kVtable, sizeof(kExpectedVtable), false) ||
            std::memcmp(reinterpret_cast<const void*>(kVtable), kExpectedVtable,
                        sizeof(kExpectedVtable)) != 0)
            return ForeignCode;
        for (unsigned i = 0; i < 3; ++i) {
            if (!contains(layout.imageSize, kEntries[i] - kImageBase, kLengths[i]) ||
                !readableImageMemory(kEntries[i], kLengths[i], true) ||
                !validCode(i, reinterpret_cast<const std::uint8_t*>(kEntries[i]), kLengths[i]))
                return ForeignCode;
        }
        return Installed; // Gate passed; no pointer has been changed yet.
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UnsupportedImage;
    }
}

bool beginWrite(void*, std::uintptr_t* cookie)
{
    using namespace c4blend565install;
    SYSTEM_INFO system = {};
    GetSystemInfo(&system);
    const std::uintptr_t begin = kSlots[0];
    constexpr std::size_t bytes = sizeof(std::uint32_t) * 3;
    if (!system.dwPageSize || (begin & 3) ||
        kSlots[1] != begin + 4 || kSlots[2] != begin + 8 ||
        begin / system.dwPageSize != (begin + bytes - 1) / system.dwPageSize ||
        !readableImageMemory(begin, bytes, false))
        return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(begin), bytes, PAGE_READWRITE, &oldProtection))
        return false;
    *cookie = oldProtection;
    return true;
}

bool endWrite(void*, std::uintptr_t cookie)
{
    DWORD ignored = 0;
    return VirtualProtect(reinterpret_cast<void*>(c4blend565install::kSlots[0]),
                          sizeof(std::uint32_t) * 3, static_cast<DWORD>(cookie), &ignored) != 0;
}

std::uint32_t compareExchange(void*, unsigned index, std::uint32_t expected,
                              std::uint32_t desired)
{
    return static_cast<std::uint32_t>(InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(c4blend565install::kSlots[index]),
        static_cast<LONG>(desired), static_cast<LONG>(expected)));
}

void publishOriginals(void*)
{
    g_originalHalf = reinterpret_cast<AlphaFn>(c4blend565install::kEntries[0]);
    g_originalAdd = reinterpret_cast<AddSubFn>(c4blend565install::kEntries[1]);
    g_originalSubtract = reinterpret_cast<AddSubFn>(c4blend565install::kEntries[2]);
    // The subsequent interlocked table stores expose only hooks with ready fallbacks.
    MemoryBarrier();
}

} // namespace

extern "C" void blend565_install()
{
    if (InterlockedCompareExchange(&g_state, Installing, NotAttempted) != NotAttempted)
        return;
    if (!IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE)) {
        InterlockedExchange(&g_state, CpuUnavailable);
        return;
    }
    const State gate = exactImageGate();
    if (gate != Installed) {
        InterlockedExchange(&g_state, gate);
        return;
    }
    const std::uint32_t replacements[3] = {
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&halfHook)),
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&addHook)),
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&subtractHook))
    };
    const c4blend565install::PatchOps ops = {
        nullptr, beginWrite, endWrite, compareExchange, publishOriginals
    };
    const auto result = c4blend565install::patchSlots(ops, replacements);
    State state = ProtectFailed;
    switch (result) {
    case c4blend565install::PatchResult::Installed: state = Installed; break;
    case c4blend565install::PatchResult::ProtectFailed: state = ProtectFailed; break;
    case c4blend565install::PatchResult::Collision: state = PatchCollision; break;
    case c4blend565install::PatchResult::RestoreFailed: state = RestoreFailed; break;
    }
    InterlockedExchange(&g_state, state);
}

extern "C" int blend565_is_available()
{
    return InterlockedCompareExchange(&g_state, 0, 0) == Installed;
}

extern "C" const char* blend565_status_text()
{
    switch (InterlockedCompareExchange(&g_state, 0, 0)) {
    case NotAttempted: return "not initialized";
    case Installing: return "initializing";
    case Installed: return "enabled (RGB565 SSE2)";
    case CpuUnavailable: return "native fallback (SSE2 unavailable)";
    case UnsupportedImage: return "native fallback (unsupported executable)";
    case ForeignCode: return "native fallback (code or vtable differs)";
    case ProtectFailed: return "native fallback (page protection failed)";
    case PatchCollision: return "native fallback (hook collision)";
    case RestoreFailed: return "native fallback (page protection restore failed)";
    default: return "native fallback";
    }
}
