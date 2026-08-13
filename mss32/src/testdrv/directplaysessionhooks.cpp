/*
 * DirectPlay session support for unattended DebugTest multiplayer runs.
 * Compile-gated by D2_TESTDRV.
 */

#ifdef D2_TESTDRV

#include "testdrv/directplaysessionhooks.h"
#include "version.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace directplaysessionhooks {

namespace {

constexpr std::uintptr_t kDPlayServiceVftVA = 0x6E6824;
constexpr int kEnumSlot = 2;
constexpr std::uintptr_t kEnumExpectedVA = 0x55D17D;

using FnEnumSessions = void(__fastcall*)(void*, void*, void*, const GUID*, const char*, char, char);

std::atomic<FnEnumSessions> g_origEnum{nullptr};
bool g_installed{};

void __fastcall hookEnumSessions(void* self, void* edx, void* sessions, const GUID* appGuid,
                                 const char* ipAddress, char allSessions, char requirePassword)
{
    const char* host = (ipAddress && ipAddress[0]) ? ipAddress : "127.0.0.1";
    spdlog::info("[testdrv][dplay] EnumSessions host='{}' (original='{}')", host,
                 ipAddress ? ipAddress : "(null)");
    if (auto original = g_origEnum.load(std::memory_order_acquire))
        original(self, edx, sessions, appGuid, host, allSessions, requirePassword);
}

} // namespace

bool install()
{
    if (g_installed)
        return true;
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return false;

    auto* address = reinterpret_cast<std::uintptr_t*>(kDPlayServiceVftVA) + kEnumSlot;
    if (*address != kEnumExpectedVA) {
        spdlog::error("[testdrv][dplay] EnumSessions slot is {:#x}, expected {:#x}; refusing",
                      *address, kEnumExpectedVA);
        return false;
    }
    g_origEnum.store(reinterpret_cast<FnEnumSessions>(kEnumExpectedVA),
                     std::memory_order_release);

    DWORD oldProtection{};
    if (!VirtualProtect(address, sizeof(*address), PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    *address = reinterpret_cast<std::uintptr_t>(&hookEnumSessions);
    DWORD ignored{};
    const bool restored = VirtualProtect(address, sizeof(*address), oldProtection, &ignored) != FALSE;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, sizeof(*address)) != FALSE;
    if (!restored || !flushed) {
        spdlog::critical("[testdrv][dplay] EnumSessions hook finalization failed; terminating");
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0xD2E77303u);
        std::abort();
    }

    g_installed = true;
    spdlog::info("[testdrv][dplay] EnumSessions localhost hook installed");
    return g_installed;
}

} // namespace directplaysessionhooks
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
