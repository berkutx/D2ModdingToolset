/*
 * DebugTest-only legacy live-stack reporter.
 *
 * Russobit proof (Discipl2.exe SHA-256 is checked by testenv):
 *   CMidStack::Stream entry       0x005EE3CA
 *   entry bytes                   B8 E0 F3 6B 00  (mov eax, 0x006BF3E0)
 *   adjusted scenario vtable      0x006F0A7C
 *   complete IMapElement vtable   0x006F0A94
 *
 * IDA shows that Stream receives an IMidScenarioObject-adjusted `this`:
 * id=this+4, owner=this+120, movement=this+132, and the complete CMidStack
 * begins at this-20. The sampler therefore stores the adjusted-to-complete
 * pointer once, then revalidates both vtables and the id under SEH before every
 * read. Production Debug/Release do not compile this translation unit.
 */

#ifdef D2_TESTDRV

#include "midstack.h"
#include "testdrv/legacystackreporter.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/testenv.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace legacystackreporter {

namespace {

static_assert(sizeof(void*) == 4,
              "the Russobit absolute-address reporter is x86-only");
static_assert(offsetof(game::CMidStack, id) == 24,
              "CMidStack adjusted-this proof requires id at +24");
static_assert(offsetof(game::CMidStack, ownerId) == 140,
              "CMidStack adjusted-this proof requires owner at +140");
static_assert(offsetof(game::CMidStack, movement) == 152,
              "CMidStack adjusted-this proof requires movement at +152");

constexpr std::uintptr_t kStreamAddress = 0x005EE3CAu;
constexpr std::size_t kPatchLength = 5;
constexpr std::uint8_t kExpectedEntry[kPatchLength] = {
    0xB8, 0xE0, 0xF3, 0x6B, 0x00,
};
constexpr std::uintptr_t kAdjustedScenarioVftable = 0x006F0A7Cu;
constexpr std::uintptr_t kCompleteMapVftable = 0x006F0A94u;
constexpr std::uintptr_t kAdjustedThisOffset = 20u;
constexpr DWORD kSamplePeriodMs = 500;
constexpr std::size_t kMaxStacks = 256;
constexpr std::size_t kWireRecordSize = 20;

struct RegistryEntry
{
    std::uint32_t id;
    std::uintptr_t completeStack;
};

struct WireStack
{
    std::uint32_t id;
    std::uint32_t owner;
    std::int32_t x;
    std::int32_t y;
    std::uint32_t movement;
};

static_assert(sizeof(WireStack) == kWireRecordSize,
              "legacy stack wire record must remain exactly 20 bytes");

SRWLOCK g_registryLock = SRWLOCK_INIT;
RegistryEntry g_registry[kMaxStacks]{};
std::size_t g_registrySize = 0;

bool g_preflighted = false;
bool g_preflightPassed = false;
bool g_requested = false;
bool g_committed = false;
std::atomic<bool> g_startClaimed{false};
std::uint8_t* g_resumeStub = nullptr;

bool exactHostRole()
{
    char role[8]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA(
        "D2TESTDRV_ROLE", role, static_cast<DWORD>(sizeof(role)));
    return length == 4 && std::memcmp(role, "host", 4) == 0;
}

// Kept free of C++ objects with destructors because MSVC forbids unwinding
// across an SEH __try region.
bool siteHasExactEntry()
{
    __try {
        return std::memcmp(reinterpret_cast<const void*>(kStreamAddress),
                           kExpectedEntry, kPatchLength) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool captureIdentity(std::uintptr_t adjustedThis,
                     std::uint32_t* id,
                     std::uintptr_t* completeStack)
{
    __try {
        if (adjustedThis < 0x10000u + kAdjustedThisOffset
            || adjustedThis >= 0x80000000u)
            return false;
        if (*reinterpret_cast<const std::uint32_t*>(adjustedThis)
            != kAdjustedScenarioVftable)
            return false;

        const std::uintptr_t complete = adjustedThis - kAdjustedThisOffset;
        if (*reinterpret_cast<const std::uint32_t*>(complete)
            != kCompleteMapVftable)
            return false;

        const std::uint32_t capturedId =
            *reinterpret_cast<const std::uint32_t*>(adjustedThis + 4);
        if (capturedId == 0 || capturedId == 0xFFFFFFFFu)
            return false;
        // Preserve the original harness admission rule: an early Stream call
        // for an object that is not placed yet must not consume a permanent
        // registry slot. A later placed Stream call may admit it.
        const std::int32_t x =
            *reinterpret_cast<const std::int32_t*>(complete + 4);
        const std::int32_t y =
            *reinterpret_cast<const std::int32_t*>(complete + 8);
        if (x < 0 || y < 0)
            return false;
        *id = capturedId;
        *completeStack = complete;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void upsert(std::uint32_t id, std::uintptr_t completeStack)
{
    AcquireSRWLockExclusive(&g_registryLock);
    for (std::size_t i = 0; i < g_registrySize; ++i) {
        if (g_registry[i].id == id) {
            g_registry[i].completeStack = completeStack;
            ReleaseSRWLockExclusive(&g_registryLock);
            return;
        }
    }
    // Exact legacy fixed-cap policy: ignore excess lifetime ids. Reporter
    // capacity is observational and must not create a new game crash condition.
    if (g_registrySize == kMaxStacks) {
        ReleaseSRWLockExclusive(&g_registryLock);
        return;
    }
    g_registry[g_registrySize++] = RegistryEntry{id, completeStack};
    ReleaseSRWLockExclusive(&g_registryLock);
}

std::size_t copyRegistry(RegistryEntry* out)
{
    AcquireSRWLockShared(&g_registryLock);
    const std::size_t count = g_registrySize;
    std::memcpy(out, g_registry, count * sizeof(RegistryEntry));
    ReleaseSRWLockShared(&g_registryLock);
    return count;
}

// Exact legacy observation fields. A freed/reused object is skipped rather
// than dereferenced: both vtables and the captured id must still match.
bool readLiveStack(const RegistryEntry* entry, WireStack* out)
{
    __try {
        const std::uintptr_t complete = entry->completeStack;
        if (complete < 0x10000u || complete >= 0x80000000u)
            return false;
        const std::uintptr_t adjusted = complete + kAdjustedThisOffset;
        if (*reinterpret_cast<const std::uint32_t*>(complete)
                != kCompleteMapVftable
            || *reinterpret_cast<const std::uint32_t*>(adjusted)
                != kAdjustedScenarioVftable
            || *reinterpret_cast<const std::uint32_t*>(adjusted + 4)
                != entry->id)
            return false;

        const std::int32_t x =
            *reinterpret_cast<const std::int32_t*>(complete + 4);
        const std::int32_t y =
            *reinterpret_cast<const std::int32_t*>(complete + 8);
        if (x < 0 || y < 0)
            return false;

        out->id = entry->id;
        out->owner =
            *reinterpret_cast<const std::uint32_t*>(adjusted + 120);
        out->x = x;
        out->y = y;
        out->movement =
            *reinterpret_cast<const std::uint8_t*>(adjusted + 132);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

extern "C" void __cdecl captureLegacyStackStream(std::uintptr_t adjustedThis)
{
    std::uint32_t id = 0;
    std::uintptr_t complete = 0;
    if (!captureIdentity(adjustedThis, &id, &complete))
        return;
    upsert(id, complete);
}

// Preserve the complete machine state, observe ECX, then execute the displaced
// original instruction in g_resumeStub and continue at 0x005EE3CF. This hook
// does not alter Stream arguments, return values, or control flow.
extern "C" __declspec(naked) void legacyStackStreamThunk()
{
    __asm {
        pushfd
        pushad
        push ecx
        call captureLegacyStackStream
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_resumeStub]
    }
}

bool buildAndInstallTrampoline()
{
    if (!siteHasExactEntry()) {
        spdlog::error(
            "[testdrv] legacy stack reporter Stream entry changed between preflight and commit");
        return false;
    }

    constexpr std::size_t stubSize = kPatchLength + 5;
    auto* stub = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, stubSize, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!stub) {
        spdlog::error(
            "[testdrv] legacy stack reporter could not allocate its resume trampoline");
        return false;
    }

    std::memcpy(stub, kExpectedEntry, kPatchLength);
    stub[kPatchLength] = 0xE9;
    const std::int32_t resumeRelative = static_cast<std::int32_t>(
        (kStreamAddress + kPatchLength)
        - (reinterpret_cast<std::uintptr_t>(stub) + stubSize));
    std::memcpy(stub + kPatchLength + 1, &resumeRelative,
                sizeof(resumeRelative));

    auto* site = reinterpret_cast<std::uint8_t*>(kStreamAddress);
    DWORD oldProtection = 0;
    if (!VirtualProtect(site, kPatchLength, PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        spdlog::error(
            "[testdrv] legacy stack reporter could not unprotect Stream entry");
        return false;
    }

    // Recheck while the page is owned. An intervening hook is a hard conflict;
    // there is intentionally no chaining or fallback address.
    if (std::memcmp(site, kExpectedEntry, kPatchLength) != 0) {
        DWORD ignored = 0;
        VirtualProtect(site, kPatchLength, oldProtection, &ignored);
        VirtualFree(stub, 0, MEM_RELEASE);
        spdlog::error(
            "[testdrv] legacy stack reporter detected a competing Stream patch");
        return false;
    }

    const std::int32_t hookRelative = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(&legacyStackStreamThunk)
        - (kStreamAddress + kPatchLength));
    // Publish the resume target before the entry can ever branch to the thunk.
    g_resumeStub = stub;
    std::memcpy(site + 1, &hookRelative, sizeof(hookRelative));
    site[0] = 0xE9;

    DWORD ignored = 0;
    if (!VirtualProtect(site, kPatchLength, oldProtection, &ignored)
        || !FlushInstructionCache(GetCurrentProcess(), site, kPatchLength)) {
        // The caller terminates the process immediately. Returning a partially
        // committed hook to normal execution would be less safe than fail-close.
        spdlog::error(
            "[testdrv] legacy stack reporter could not seal its committed Stream hook");
        return false;
    }

    return true;
}

DWORD WINAPI samplerThread(void*)
{
    RegistryEntry registry[kMaxStacks]{};
    alignas(std::uint32_t)
        std::uint8_t frame[sizeof(std::uint32_t)
                           + kMaxStacks * kWireRecordSize]{};

    for (;;) {
        Sleep(kSamplePeriodMs);
        const std::size_t captured = copyRegistry(registry);
        std::uint32_t live = 0;
        for (std::size_t i = 0; i < captured; ++i) {
            WireStack stack{};
            if (!readLiveStack(&registry[i], &stack))
                continue;
            std::memcpy(frame + sizeof(std::uint32_t)
                            + live * kWireRecordSize,
                        &stack, sizeof(stack));
            ++live;
        }
        if (live == 0)
            continue;

        std::memcpy(frame, &live, sizeof(live));
        bridge::send_legacy_stacks_snapshot(
            frame, static_cast<std::uint32_t>(
                       sizeof(std::uint32_t) + live * kWireRecordSize));
    }
}

} // namespace

bool readRequestedGate(bool& requested)
{
    requested = false;
    char value[8]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA(
        "D2TESTDRV_LEGACY_STACKS", value,
        static_cast<DWORD>(sizeof(value)));
    const DWORD error = GetLastError();
    if (length == 0 && error == ERROR_ENVVAR_NOT_FOUND)
        return true;
    if (length == 1 && value[0] == '1') {
        requested = true;
        return true;
    }
    spdlog::error(
        "[testdrv] D2TESTDRV_LEGACY_STACKS accepts only the exact value 1");
    return false;
}

bool preflight(bool requested)
{
    if (g_preflighted)
        return g_preflightPassed && g_requested == requested;

    g_preflighted = true;
    g_requested = requested;
    if (!requested) {
        g_preflightPassed = true;
        return true;
    }
    if (!testenv::supportedGameBuild()) {
        spdlog::error(
            "[testdrv] legacy stack reporter requires the exact Russobit image");
        return false;
    }
    if (!exactHostRole()) {
        spdlog::error(
            "[testdrv] D2TESTDRV_LEGACY_STACKS=1 is host-only and requires exact D2TESTDRV_ROLE=host");
        return false;
    }
    if (!siteHasExactEntry()) {
        spdlog::error(
            "[testdrv] Russobit CMidStack::Stream entry bytes do not match the proven 0x005EE3CA target");
        return false;
    }

    g_preflightPassed = true;
    spdlog::info(
        "[testdrv] legacy stack reporter preflight passed (host, Stream=0x005EE3CA, cadence={}ms)",
        static_cast<unsigned>(kSamplePeriodMs));
    return true;
}

bool commit()
{
    if (!g_preflighted || !g_preflightPassed)
        return false;
    if (!g_requested)
        return true;
    if (g_committed)
        return true;
    if (!buildAndInstallTrampoline())
        return false;

    g_committed = true;
    spdlog::info(
        "[testdrv] legacy stack reporter committed exact CMidStack::Stream trampoline");
    return true;
}

bool start()
{
    if (!g_requested)
        return true;
    if (!g_committed)
        return false;

    bool expected = false;
    if (!g_startClaimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return true;

    HANDLE thread = CreateThread(nullptr, 0, &samplerThread, nullptr,
                                 CREATE_SUSPENDED, nullptr);
    if (!thread) {
        g_startClaimed.store(false, std::memory_order_release);
        return false;
    }
    // Brief periodic reads must stay current even when the game pins its
    // normal-priority UI thread to the same CPU.
    if (!SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL)
        || ResumeThread(thread) == static_cast<DWORD>(-1)) {
        // Runtime startup is fail-closed in testdrv. The still-suspended helper
        // cannot execute and disappears when that caller terminates the process.
        CloseHandle(thread);
        g_startClaimed.store(false, std::memory_order_release);
        return false;
    }
    CloseHandle(thread);
    spdlog::info(
        "[testdrv] legacy stack reporter sampler started (host-only, 500ms, above-normal priority)");
    return true;
}

} // namespace legacystackreporter
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV

