/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Headless-boot fixes. See testdrv/bootfixes.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#include "testdrv/bootfixes.h"
#include "testdrv/testenv.h"
#include "version.h"
#include <cstdint>
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
const std::uint8_t kFgFlagPatch[10] = {0xC6, 0x47, 0x18, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

bool writeBytes(uintptr_t va, const std::uint8_t* bytes, size_t len)
{
    void* site = reinterpret_cast<void*>(va);
    DWORD oldProt = 0;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    memcpy(site, bytes, len);
    VirtualProtect(site, len, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

void patchSkipIntro()
{
    const std::uint8_t* site = reinterpret_cast<const std::uint8_t*>(kSkipIntroVA);
    if (memcmp(site, kSkipIntroPatch, sizeof(kSkipIntroPatch)) == 0) {
        spdlog::info("[testdrv] skip-intro already patched");
        return;
    }
    if (memcmp(site, kSkipIntroExpected, sizeof(kSkipIntroExpected)) != 0) {
        spdlog::error("[testdrv] skip-intro: bytes at {:#x} don't match expected; refusing",
                      kSkipIntroVA);
        return;
    }
    if (writeBytes(kSkipIntroVA, kSkipIntroPatch, sizeof(kSkipIntroPatch)))
        spdlog::info("[testdrv] skip-intro patched ({:#x})", kSkipIntroVA);
    else
        spdlog::error("[testdrv] skip-intro: VirtualProtect/write failed");
}

void patchFgFlag()
{
    const std::uint8_t* site = reinterpret_cast<const std::uint8_t*>(kFgFlagVA);
    if (memcmp(site, kFgFlagPatch, 4) == 0) { // first 4 bytes are the distinctive store
        spdlog::info("[testdrv] fg-flag already patched");
        return;
    }
    std::uint8_t orig[10];
    memcpy(orig, site, sizeof(orig));
    if (writeBytes(kFgFlagVA, kFgFlagPatch, sizeof(kFgFlagPatch))) {
        spdlog::info("[testdrv] black-screen fg-flag patched ({:#x}); orig {:02X} {:02X} {:02X} "
                     "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                     kFgFlagVA, orig[0], orig[1], orig[2], orig[3], orig[4], orig[5], orig[6],
                     orig[7], orig[8], orig[9]);
    } else {
        spdlog::error("[testdrv] fg-flag: VirtualProtect/write failed");
    }
}

} // namespace

void installEarly()
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return;
    if (testenv::on("D2TESTDRV_SKIP_INTRO"))
        patchSkipIntro();
    if (testenv::on("D2TESTDRV_BLACKSCREEN_FIX"))
        patchFgFlag();
}

} // namespace bootfixes
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
