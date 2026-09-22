/*
 * Narrow inline patches required by the exact Russobit simultaneous-turn path.
 * All sites are preflighted before the first write and every write is owned.
 */

#include "simturns/patches.h"
#include "simturns/russobit_sites.h"
#include "simturns/state.h"
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

namespace hooks::simturns::patches {

namespace {

constexpr std::uintptr_t virtualTurnCallAddress = 0x4232FF;
constexpr std::uintptr_t originalAdvanceAddress = 0x41E741;
constexpr std::uintptr_t beginTurnGateAddress = 0x491690;
constexpr std::uintptr_t dispatchGateAddress = 0x489994;
constexpr std::uintptr_t cascadeCastAddress = 0x4440D2;
constexpr std::uintptr_t cascadeEliminationAddress = 0x44417D;

void* g_originalAdvance = reinterpret_cast<void*>(originalAdvanceAddress);

struct BytePatch
{
    const char* name;
    std::uintptr_t address;
    const std::uint8_t* expected;
    const std::uint8_t* replacement;
    std::size_t size;
    bool applied;
};

constexpr std::array<std::uint8_t, 5> virtualTurnExpected{{0xE8, 0x3D, 0xB4, 0xFF, 0xFF}};
std::array<std::uint8_t, 5> virtualTurnReplacement{{0xE8, 0, 0, 0, 0}};
constexpr std::array<std::uint8_t, 2> beginGateExpected{{0x74, 0x23}};
constexpr std::array<std::uint8_t, 2> beginGateReplacement{{0xEB, 0x23}};
constexpr std::array<std::uint8_t, 6> dispatchGateExpected{{0x0F, 0x85, 0x66, 0x01, 0, 0}};
constexpr std::array<std::uint8_t, 6> dispatchGateReplacement{{0x90, 0x90, 0x90,
                                                               0x90, 0x90, 0x90}};
constexpr std::array<std::uint8_t, 5> cascadeCastExpected{{0xE8, 0x8F, 0x93, 0x22, 0}};
constexpr std::array<std::uint8_t, 5> cascadeCastReplacement{{0x33, 0xC0, 0x90, 0x90,
                                                              0x90}};
constexpr std::array<std::uint8_t, 8> cascadeEliminationExpected{{0x8B, 0x4D, 0xF0, 0xE8,
                                                                  0xD9, 0x1F, 0x1A, 0}};
constexpr std::array<std::uint8_t, 8> cascadeEliminationReplacement{{0x33, 0xC0, 0x90, 0x90,
                                                                     0x90, 0x90, 0x90, 0x90}};
constexpr std::array<std::uint8_t, 2> autoBattleStaleGateReplacement{{0x90, 0x90}};
constexpr std::array<std::uint8_t, 7> autoBattleStaleFlagSetReplacement{{
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
}};

BytePatch g_virtualTurn{"virtual turn call", virtualTurnCallAddress, virtualTurnExpected.data(),
                        virtualTurnReplacement.data(), virtualTurnExpected.size(), false};
BytePatch g_beginGate{"begin-turn UI gate", beginTurnGateAddress, beginGateExpected.data(),
                      beginGateReplacement.data(), beginGateExpected.size(), false};
BytePatch g_dispatchGate{"join dispatch gate", dispatchGateAddress, dispatchGateExpected.data(),
                         dispatchGateReplacement.data(), dispatchGateExpected.size(), false};
BytePatch g_cascadeCast{"cascade RTTI repair", cascadeCastAddress, cascadeCastExpected.data(),
                        cascadeCastReplacement.data(), cascadeCastExpected.size(), false};
BytePatch g_cascadeElimination{"cascade elimination repair", cascadeEliminationAddress,
                               cascadeEliminationExpected.data(),
                               cascadeEliminationReplacement.data(),
                               cascadeEliminationExpected.size(), false};
BytePatch g_autoBattleStaleGate{
    "concurrent auto-battle stale gate", russobit::autoBattleStaleGate,
    russobit::autoBattleStaleGateBytes.data(), autoBattleStaleGateReplacement.data(),
    russobit::autoBattleStaleGateBytes.size(), false};
BytePatch g_autoBattleStaleFlagSet{
    "concurrent auto-battle stale-flag set", russobit::autoBattleStaleFlagSet,
    russobit::autoBattleStaleFlagSetBytes.data(), autoBattleStaleFlagSetReplacement.data(),
    russobit::autoBattleStaleFlagSetBytes.size(), false};

class ThreadFreeze
{
public:
    ThreadFreeze() = default;
    ThreadFreeze(const ThreadFreeze&) = delete;
    ThreadFreeze& operator=(const ThreadFreeze&) = delete;

    ~ThreadFreeze()
    {
        if (!resumeAndClose()) {
            TerminateProcess(GetCurrentProcess(), 0xD2510010u);
            std::abort();
        }
    }

    bool begin(BytePatch* const* sites, std::size_t siteCount, bool onlyApplied = false)
    {
        if (!sites || !siteCount)
            return false;

        const DWORD processId = GetCurrentProcessId();
        const DWORD currentThreadId = GetCurrentThreadId();

        // Open the initial stable set before suspending anybody. Unlike
        // DetourUpdateThread this path performs no CRT allocation after a peer
        // may have been frozen while owning the process heap.
        if (!collectNewThreads(processId, currentThreadId, false, sites, siteCount,
                               onlyApplied))
            return abortAndClose();
        for (std::size_t i = 0; i < handleCount; ++i) {
            if (!suspendAndCheck(i, sites, siteCount, onlyApplied))
                return abortAndClose();
        }

        // Once every known peer is suspended it cannot create another thread.
        // Require two consecutive stable snapshots to close ordinary creation
        // races. A remote/kernel-created thread remains an unavoidable
        // user-mode limitation of this PoC safe-point.
        constexpr std::size_t maximumPasses = 8;
        std::size_t stablePasses = 0;
        for (std::size_t pass = 0; pass < maximumPasses; ++pass) {
            bool added = false;
            const std::size_t previousCount = handleCount;
            if (!collectNewThreads(processId, currentThreadId, true, sites, siteCount,
                                   onlyApplied))
                return abortAndClose();
            for (std::size_t i = previousCount; i < handleCount; ++i) {
                if (!suspendAndCheck(i, sites, siteCount, onlyApplied))
                    return abortAndClose();
                added = true;
            }
            stablePasses = added ? 0 : stablePasses + 1;
            if (stablePasses >= 2)
                return true;
        }
        return abortAndClose();
    }

    bool resume(bool)
    {
        return resumeAndClose();
    }

private:
    static constexpr std::size_t maximumThreads = 256;
    std::array<HANDLE, maximumThreads> handles{};
    std::array<DWORD, maximumThreads> threadIds{};
    std::array<bool, maximumThreads> suspended{};
    std::size_t handleCount{};

    bool containsLiveThread(DWORD threadId, bool& queryOk) const
    {
        queryOk = true;
        for (std::size_t i = 0; i < handleCount; ++i) {
            if (threadIds[i] != threadId)
                continue;

            DWORD exitCode = STILL_ACTIVE;
            if (!GetExitCodeThread(handles[i], &exitCode)) {
                queryOk = false;
                return false;
            }
            if (exitCode == STILL_ACTIVE)
                return true;
        }
        return false;
    }

    static bool instructionPointerTouches(const CONTEXT& context,
                                          BytePatch* const* sites,
                                          std::size_t siteCount,
                                          bool onlyApplied)
    {
        static_assert(sizeof(void*) == 4, "simultaneous-turn patches require x86");
        const std::uintptr_t instruction = static_cast<std::uintptr_t>(context.Eip);
        for (std::size_t i = 0; i < siteCount; ++i) {
            const auto* site = sites[i];
            if (site && onlyApplied && !site->applied)
                continue;
            // A suspended x86 EIP exactly at the patch start has not executed
            // that instruction yet.  It is safe to publish the replacement:
            // after FlushInstructionCache the resumed thread executes the new
            // instruction from its first byte.  Only an EIP strictly inside
            // the replaced byte span is unsafe; even an internal stock
            // instruction boundary is not relocatable after replacement.
            if (site && instruction > site->address
                && instruction < site->address + site->size) {
                return true;
            }
        }
        return false;
    }

    bool collectNewThreads(DWORD processId,
                           DWORD currentThreadId,
                           bool peersAlreadyFrozen,
                           BytePatch* const* sites,
                           std::size_t siteCount,
                           bool onlyApplied)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        BOOL haveEntry = Thread32First(snapshot, &entry);
        while (haveEntry) {
            bool identityQueryOk = true;
            const bool alreadyTracked = containsLiveThread(entry.th32ThreadID,
                                                           identityQueryOk);
            if (!identityQueryOk) {
                CloseHandle(snapshot);
                return false;
            }
            if (entry.th32OwnerProcessID == processId
                && entry.th32ThreadID != currentThreadId
                && !alreadyTracked) {
                if (handleCount == handles.size()) {
                    CloseHandle(snapshot);
                    return false;
                }
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                               | THREAD_QUERY_INFORMATION,
                                           FALSE, entry.th32ThreadID);
                if (!thread) {
                    if (GetLastError() != ERROR_INVALID_PARAMETER) {
                        CloseHandle(snapshot);
                        return false;
                    }
                } else {
                    handles[handleCount] = thread;
                    threadIds[handleCount] = entry.th32ThreadID;
                    suspended[handleCount] = false;
                    ++handleCount;
                    if (peersAlreadyFrozen
                        && !suspendAndCheck(handleCount - 1, sites, siteCount,
                                            onlyApplied)) {
                        CloseHandle(snapshot);
                        return false;
                    }
                }
            }
            entry.dwSize = sizeof(entry);
            haveEntry = Thread32Next(snapshot, &entry);
        }
        const DWORD enumerationError = GetLastError();
        CloseHandle(snapshot);
        return enumerationError == ERROR_NO_MORE_FILES;
    }

    bool suspendAndCheck(std::size_t index,
                         BytePatch* const* sites,
                         std::size_t siteCount,
                         bool onlyApplied)
    {
        if (index >= handleCount || !handles[index])
            return false;
        if (suspended[index])
            return true;
        if (SuspendThread(handles[index]) == static_cast<DWORD>(-1)) {
            DWORD exitCode = STILL_ACTIVE;
            return GetExitCodeThread(handles[index], &exitCode) && exitCode != STILL_ACTIVE;
        }
        suspended[index] = true;

        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(handles[index], &context)) {
            DWORD exitCode = STILL_ACTIVE;
            const bool terminated = GetExitCodeThread(handles[index], &exitCode)
                                    && exitCode != STILL_ACTIVE;
            // A terminated handle no longer contributes a suspend count. Do
            // not try to ResumeThread it during cleanup, which would turn a
            // harmless exit race into a process-fatal cleanup failure.
            if (terminated)
                suspended[index] = false;
            return terminated;
        }
        return !instructionPointerTouches(context, sites, siteCount, onlyApplied);
    }

    bool abortAndClose()
    {
        if (!resumeAndClose()) {
            TerminateProcess(GetCurrentProcess(), 0xD2510011u);
            std::abort();
        }
        return false;
    }

    bool resumeAndClose()
    {
        bool ok = true;
        for (std::size_t i = 0; i < handleCount; ++i) {
            if (suspended[i]
                && ResumeThread(handles[i]) == static_cast<DWORD>(-1)) {
                ok = false;
            }
            if (handles[i])
                CloseHandle(handles[i]);
            handles[i] = nullptr;
            threadIds[i] = 0;
            suspended[i] = false;
        }
        handleCount = 0;
        return ok;
    }
};

extern "C" int __cdecl virtualTurnShouldPass()
{
    return ownsVirtualTurn() ? 0 : 1;
}

extern "C" __declspec(naked) void virtualTurnThunk()
{
    __asm {
        push ecx
        call virtualTurnShouldPass
        pop ecx
        test eax, eax
        jnz pass_through
        ret 4
    pass_through:
        jmp dword ptr [g_originalAdvance]
    }
}

void buildVirtualTurnReplacement()
{
    const std::uintptr_t nextInstruction = virtualTurnCallAddress + virtualTurnReplacement.size();
    const std::int32_t relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(&virtualTurnThunk) - nextInstruction);
    std::memcpy(virtualTurnReplacement.data() + 1, &relative, sizeof(relative));
}

bool bytesEqual(const BytePatch& patch, const std::uint8_t* bytes)
{
    return std::memcmp(reinterpret_cast<const void*>(patch.address), bytes, patch.size) == 0;
}

bool checkExpected(const BytePatch& patch)
{
    if (patch.applied)
        return bytesEqual(patch, patch.replacement);
    if (bytesEqual(patch, patch.expected))
        return true;
    spdlog::error("[simturns] preflight mismatch for {} at {:#x}", patch.name, patch.address);
    return false;
}

bool writeBytes(BytePatch& patch, const std::uint8_t* bytes)
{
    void* const address = reinterpret_cast<void*>(patch.address);
    DWORD oldProtection = 0;
    if (!VirtualProtect(address, patch.size, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;

    std::memcpy(address, bytes, patch.size);
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(address, patch.size, oldProtection, &ignored);
    const BOOL flushed = FlushInstructionCache(GetCurrentProcess(), address, patch.size);
    if (!restored || !flushed) {
        // Bytes have already changed. Continuing would violate the all-or-nothing
        // contract and could run code under an indeterminate page protection.
        TerminateProcess(GetCurrentProcess(), 0xD2510001u);
        std::abort();
    }
    return true;
}

bool apply(BytePatch& patch)
{
    if (patch.applied)
        return bytesEqual(patch, patch.replacement);
    if (!bytesEqual(patch, patch.expected))
        return false;
    if (!writeBytes(patch, patch.replacement))
        return false;
    patch.applied = true;
    return true;
}

bool restore(BytePatch& patch)
{
    if (!patch.applied)
        return true;
    if (!bytesEqual(patch, patch.replacement))
        return false;
    if (!writeBytes(patch, patch.expected))
        return false;
    patch.applied = false;
    return true;
}

template <std::size_t N>
bool applyTransaction(const std::array<BytePatch*, N>& patches)
{
    ThreadFreeze freeze;
    if (!freeze.begin(patches.data(), patches.size())) {
        spdlog::error("[simturns] could not freeze all peer threads for inline-patch apply");
        return false;
    }

    // Recheck the entire set after every other process thread is suspended and
    // before the first byte changes.
    for (auto* patch : patches) {
        if (!patch || (!patch->applied && !bytesEqual(*patch, patch->expected))
            || (patch->applied && !bytesEqual(*patch, patch->replacement))) {
            if (!freeze.resume(false)) {
                TerminateProcess(GetCurrentProcess(), 0xD2510002u);
                std::abort();
            }
            spdlog::error("[simturns] inline-patch apply preflight changed at the safe point");
            return false;
        }
    }

    std::size_t completed = 0;
    for (; completed < patches.size(); ++completed) {
        if (!apply(*patches[completed]))
            break;
    }
    if (completed == patches.size()) {
        if (!freeze.resume(true)) {
            TerminateProcess(GetCurrentProcess(), 0xD2510003u);
            std::abort();
        }
        return true;
    }

    bool rollbackOk = true;
    while (completed > 0) {
        --completed;
        rollbackOk = restore(*patches[completed]) && rollbackOk;
    }
    if (!rollbackOk) {
        TerminateProcess(GetCurrentProcess(), 0xD2510004u);
        std::abort();
    }
    if (!freeze.resume(false)) {
        TerminateProcess(GetCurrentProcess(), 0xD2510005u);
        std::abort();
    }
    spdlog::error("[simturns] inline-patch apply failed; complete rollback retained stock bytes");
    return false;
}

template <std::size_t N>
bool restoreSet(const std::array<BytePatch*, N>& patches)
{
    ThreadFreeze freeze;
    if (!freeze.begin(patches.data(), patches.size(), true)) {
        spdlog::error("[simturns] could not freeze all peer threads for inline-patch restore");
        return false;
    }

    // A partial restore is never attempted if any active site is no longer
    // byte-for-byte owned by this module.
    for (auto* patch : patches) {
        if (patch && patch->applied && !bytesEqual(*patch, patch->replacement)) {
            if (!freeze.resume(false)) {
                TerminateProcess(GetCurrentProcess(), 0xD2510006u);
                std::abort();
            }
            spdlog::error("[simturns] inline-patch restore lost ownership before any write");
            return false;
        }
    }

    std::array<BytePatch*, N> restored{};
    std::size_t restoredCount = 0;
    bool ok = true;
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
        if (!(*it)->applied)
            continue;
        if (!restore(**it)) {
            ok = false;
            break;
        }
        restored[restoredCount++] = *it;
    }

    if (!ok) {
        bool reapplyOk = true;
        while (restoredCount > 0) {
            --restoredCount;
            reapplyOk = apply(*restored[restoredCount]) && reapplyOk;
        }
        if (!reapplyOk) {
            TerminateProcess(GetCurrentProcess(), 0xD2510007u);
            std::abort();
        }
        if (!freeze.resume(false)) {
            TerminateProcess(GetCurrentProcess(), 0xD2510008u);
            std::abort();
        }
        spdlog::error("[simturns] inline-patch restore failed; active set was reapplied");
        return false;
    }

    if (!freeze.resume(true)) {
        TerminateProcess(GetCurrentProcess(), 0xD2510009u);
        std::abort();
    }
    return true;
}

} // namespace

bool preflight()
{
    buildVirtualTurnReplacement();
    bool result = checkExpected(g_beginGate);
    result = checkExpected(g_autoBattleStaleGate) && result;
    result = checkExpected(g_autoBattleStaleFlagSet) && result;
    // Both room roles are possible over this process lifetime.
    result = checkExpected(g_virtualTurn) && result;
    result = checkExpected(g_cascadeCast) && result;
    result = checkExpected(g_cascadeElimination) && result;
    result = checkExpected(g_dispatchGate) && result;
    return result;
}

bool activate()
{
    bool activated = false;
    if (isHost()) {
        const std::array<BytePatch*, 6> hostPatches{{
            &g_virtualTurn, &g_beginGate, &g_cascadeCast, &g_cascadeElimination,
            &g_autoBattleStaleGate, &g_autoBattleStaleFlagSet}};
        activated = applyTransaction(hostPatches);
    } else {
        const std::array<BytePatch*, 4> joinPatches{{
            &g_beginGate, &g_dispatchGate, &g_autoBattleStaleGate,
            &g_autoBattleStaleFlagSet}};
        activated = applyTransaction(joinPatches);
    }
    if (activated) {
        spdlog::info(
            "[simturns] concurrent-battle compatibility patches active "
            "(0x635578=NOP2, 0x638886=NOP7)");
    }
    return activated;
}

bool restoreUiGates()
{
    // The legacy green implementation kept the two concurrent-battle
    // compatibility patches for the process lifetime. They prevent a stale
    // controller flag created during the independent phase from resurfacing
    // after merge, so only turn-ownership UI gates are restored here.
    if (isHost()) {
        const std::array<BytePatch*, 1> hostPatches{{&g_beginGate}};
        return restoreSet(hostPatches);
    }
    const std::array<BytePatch*, 2> joinPatches{{&g_beginGate, &g_dispatchGate}};
    return restoreSet(joinPatches);
}

bool restoreCascadeRepairs()
{
    if (!isHost())
        return true;
    const std::array<BytePatch*, 2> cascadePatches{{&g_cascadeCast, &g_cascadeElimination}};
    return restoreSet(cascadePatches);
}

bool rollbackAll()
{
    const std::array<BytePatch*, 7> allPatches{{
        &g_virtualTurn, &g_beginGate, &g_dispatchGate, &g_cascadeCast,
        &g_cascadeElimination, &g_autoBattleStaleGate,
        &g_autoBattleStaleFlagSet}};
    bool hasAppliedPatch = false;
    for (const auto* patch : allPatches)
        hasAppliedPatch = hasAppliedPatch || patch->applied;
    if (!hasAppliedPatch)
        return true;
    return restoreSet(allPatches);
}

} // namespace hooks::simturns::patches
