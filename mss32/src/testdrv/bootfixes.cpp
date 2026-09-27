/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Headless-boot fixes. See testdrv/bootfixes.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro no test code is compiled.
 */

#ifdef D2_TESTDRV

#include "testdrv/bootfixes.h"
#include "executablefingerprint.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace bootfixes {

namespace {

// --- Russobit pinned addresses ----------------------------------------------
constexpr uintptr_t kSkipIntroVA = 0x67D5B2; // call [_BinkOpen@8] in sub_67D53D
constexpr uintptr_t kFgFlagVA = 0x5628BE;    // foreground-flag store in sub_56288A

// skip-intro: FF 15 38 E4 6C 00  ->  83 C4 08 33 C0 90 (add esp,8; xor eax,eax; nop)
const std::uint8_t kSkipIntroExpected[6] = {0xFF, 0x15, 0x38, 0xE4, 0x6C, 0x00};
const std::uint8_t kSkipIntroPatch[6] = {0x83, 0xC4, 0x08, 0x33, 0xC0, 0x90};

// fg-flag: 10 bytes -> mov byte [edi+0x18],1 (C6 47 18 01) + 6 nop. The original
// computes manager_state[+0x18] = (GetForegroundWindow()==hwnd); we force it := 1
// always so a headless launch (no foreground) still advances past the black screen.
const std::uint8_t kFgFlagExpected[10] = {0x2B, 0xC3, 0xF7, 0xD8, 0x1B,
                                          0xC0, 0x40, 0x88, 0x47, 0x18};
const std::uint8_t kFgFlagPatch[10] = {0xC6, 0x47, 0x18, 0x01, 0x90,
                                       0x90, 0x90, 0x90, 0x90, 0x90};

bool g_prepared = false;
bool g_committed = false;
bool g_skipIntro = false;
bool g_blackScreen = false;

[[noreturn]] void failFastCommit(const char* operation, unsigned exitCode)
{
    spdlog::critical("[testdrv] boot-fix commit failed at {}; terminating", operation);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

bool bytesMatchEither(std::uintptr_t va, const std::uint8_t* expected,
                      const std::uint8_t* patch, std::size_t length,
                      const char* name)
{
    const auto* site = reinterpret_cast<const std::uint8_t*>(va);
    if (std::memcmp(site, expected, length) == 0 || std::memcmp(site, patch, length) == 0)
        return true;
    spdlog::error("[testdrv] {}: bytes at {:#x} match neither exact stock nor owned patch",
                  name, va);
    return false;
}

bool validateRequestedSites()
{
    if (g_skipIntro
        && !bytesMatchEither(kSkipIntroVA, kSkipIntroExpected, kSkipIntroPatch,
                             sizeof(kSkipIntroExpected), "skip-intro"))
        return false;
    if (g_blackScreen
        && !bytesMatchEither(kFgFlagVA, kFgFlagExpected, kFgFlagPatch,
                             sizeof(kFgFlagExpected), "fg-flag"))
        return false;
    return true;
}

bool writeBytes(uintptr_t va, const std::uint8_t* bytes, size_t len)
{
    void* site = reinterpret_cast<void*>(va);
    DWORD oldProt = 0;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    memcpy(site, bytes, len);
    DWORD ignored = 0;
    const BOOL protectionRestored = VirtualProtect(site, len, oldProt, &ignored);
    const BOOL cacheFlushed = FlushInstructionCache(GetCurrentProcess(), site, len);
    if (!protectionRestored || !cacheFlushed) {
        spdlog::critical(
            "[testdrv] boot patch changed memory at {:#x} but finalization failed; terminating", va);
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0xD2E77302u);
        std::abort();
    }
    return true;
}

bool patchSkipIntro()
{
    const std::uint8_t* site = reinterpret_cast<const std::uint8_t*>(kSkipIntroVA);
    if (memcmp(site, kSkipIntroPatch, sizeof(kSkipIntroPatch)) == 0) {
        spdlog::info("[testdrv] skip-intro already patched");
        return true;
    }
    if (memcmp(site, kSkipIntroExpected, sizeof(kSkipIntroExpected)) != 0) {
        spdlog::error("[testdrv] skip-intro: bytes at {:#x} don't match expected; refusing",
                      kSkipIntroVA);
        return false;
    }
    if (writeBytes(kSkipIntroVA, kSkipIntroPatch, sizeof(kSkipIntroPatch))) {
        spdlog::info("[testdrv] skip-intro patched ({:#x})", kSkipIntroVA);
        return true;
    }
    spdlog::error("[testdrv] skip-intro: VirtualProtect/write failed");
    return false;
}

bool patchFgFlag()
{
    const std::uint8_t* site = reinterpret_cast<const std::uint8_t*>(kFgFlagVA);
    if (memcmp(site, kFgFlagPatch, sizeof(kFgFlagPatch)) == 0) {
        spdlog::info("[testdrv] fg-flag already patched");
        return true;
    }
    if (memcmp(site, kFgFlagExpected, sizeof(kFgFlagExpected)) != 0) {
        spdlog::error("[testdrv] fg-flag: bytes at {:#x} don't match expected; refusing",
                      kFgFlagVA);
        return false;
    }
    std::uint8_t orig[10];
    memcpy(orig, site, sizeof(orig));
    if (writeBytes(kFgFlagVA, kFgFlagPatch, sizeof(kFgFlagPatch))) {
        spdlog::info("[testdrv] black-screen fg-flag patched ({:#x}); orig {:02X} {:02X} {:02X} "
                     "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                     kFgFlagVA, orig[0], orig[1], orig[2], orig[3], orig[4], orig[5], orig[6],
                     orig[7], orig[8], orig[9]);
        return true;
    } else {
        spdlog::error("[testdrv] fg-flag: VirtualProtect/write failed");
        return false;
    }
}

} // namespace

bool preflight(bool skipIntro, bool blackScreen)
{
    if (g_prepared) {
        if (g_skipIntro != skipIntro || g_blackScreen != blackScreen) {
            spdlog::error("[testdrv] boot-fix preflight called with a different plan");
            return false;
        }
        return validateRequestedSites();
    }

    g_skipIntro = skipIntro;
    g_blackScreen = blackScreen;
    if ((g_skipIntro || g_blackScreen) && !executablefingerprint::isExactRussobit()) {
        spdlog::error("[testdrv] requested boot fixes require the exact Russobit image");
        return false;
    }

    if (!validateRequestedSites())
        return false;

    g_prepared = true;
    spdlog::info("[testdrv] boot-fix preflight passed (skip-intro={}, black-screen={})",
                 g_skipIntro, g_blackScreen);
    return true;
}

void commit()
{
    if (g_committed)
        return;
    if (!g_prepared)
        failFastCommit("commit without preflight", 0xD2E77303u);

    // Close the only useful race window before changing the first byte. After
    // this point every apply/finalization failure is terminal: DllMain must not
    // unload a DLL whose ordinary hooks or boot bytes are already live.
    if (!validateRequestedSites())
        failFastCommit("post-hook site revalidation", 0xD2E77304u);
    if (g_skipIntro && !patchSkipIntro())
        failFastCommit("skip-intro write", 0xD2E77305u);
    if (g_blackScreen && !patchFgFlag())
        failFastCommit("fg-flag write", 0xD2E77306u);

    g_committed = true;
}

} // namespace bootfixes
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV

