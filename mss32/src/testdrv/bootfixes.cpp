/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Headless-boot fixes. See testdrv/bootfixes.h.
 *
 * Compile-gated by D2_TESTDRV: no test code is compiled without the macro.
 */

#ifdef D2_TESTDRV

#include "testdrv/bootfixes.h"
#include "testdrv/testenv.h"
#include "version.h"
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
const std::uint8_t kFgFlagPatch[10] = {0xC6, 0x47, 0x18, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

[[noreturn]] void failFast(const char* operation)
{
    spdlog::critical("[testdrv] boot-fix failed at {}; terminating", operation);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), 0xD2E77302u);
    std::abort();
}

bool bytesMatchEither(uintptr_t va, const std::uint8_t* expected,
                      const std::uint8_t* patch, size_t len)
{
    const auto* site = reinterpret_cast<const std::uint8_t*>(va);
    return memcmp(site, expected, len) == 0 || memcmp(site, patch, len) == 0;
}

bool writeBytes(uintptr_t va, const std::uint8_t* bytes, size_t len)
{
    void* site = reinterpret_cast<void*>(va);
    DWORD oldProt = 0;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    memcpy(site, bytes, len);
    DWORD ignored{};
    const BOOL restored = VirtualProtect(site, len, oldProt, &ignored);
    const BOOL flushed = FlushInstructionCache(GetCurrentProcess(), site, len);
    return restored && flushed;
}

bool patchBytes(const char* name, uintptr_t va, const std::uint8_t* expected,
                const std::uint8_t* patch, size_t len)
{
    const auto* site = reinterpret_cast<const std::uint8_t*>(va);
    if (memcmp(site, patch, len) == 0) {
        spdlog::info("[testdrv] {} already patched", name);
        return true;
    }
    if (memcmp(site, expected, len) != 0) {
        spdlog::error("[testdrv] {}: bytes at {:#x} don't match expected; refusing", name, va);
        return false;
    }
    if (!writeBytes(va, patch, len)) {
        spdlog::error("[testdrv] {}: VirtualProtect/write failed", name);
        return false;
    }
    if (va == kFgFlagVA) {
        // The original bytes were just checked against expected; retain the diagnostic log.
        spdlog::info("[testdrv] black-screen fg-flag patched ({:#x}); orig {:02X} {:02X} {:02X} "
                     "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                     va, expected[0], expected[1], expected[2], expected[3], expected[4],
                     expected[5], expected[6], expected[7], expected[8], expected[9]);
    } else {
        spdlog::info("[testdrv] {} patched ({:#x})", name, va);
    }
    return true;
}

} // namespace

void installEarly()
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return;
    const bool skipIntro = testenv::on("D2TESTDRV_SKIP_INTRO");
    const bool blackScreen = testenv::on("D2TESTDRV_BLACKSCREEN_FIX");

    // Validate every requested site before changing the first byte. A DebugTest
    // image with a mismatched executable must fail closed, never run half-patched.
    if (skipIntro && !bytesMatchEither(kSkipIntroVA, kSkipIntroExpected, kSkipIntroPatch,
                                       sizeof(kSkipIntroExpected)))
        failFast("skip-intro preflight");
    if (blackScreen && !bytesMatchEither(kFgFlagVA, kFgFlagExpected, kFgFlagPatch,
                                         sizeof(kFgFlagExpected)))
        failFast("fg-flag preflight");
    if (skipIntro && !patchBytes("skip-intro", kSkipIntroVA, kSkipIntroExpected, kSkipIntroPatch,
                                 sizeof(kSkipIntroPatch)))
        failFast("skip-intro commit");
    if (blackScreen && !patchBytes("fg-flag", kFgFlagVA, kFgFlagExpected, kFgFlagPatch,
                                   sizeof(kFgFlagPatch)))
        failFast("fg-flag commit");
}

} // namespace bootfixes
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
