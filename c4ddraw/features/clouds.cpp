/*
 * Optional isometric-map clouds for the Russobit/Mortling Discipl2.exe 3.01a build.
 *
 * The package-loading and cloud-layout hooks are derived from DisciplesGL:
 * https://github.com/HSerg/DisciplesGL
 *
 * MIT License
 *
 * Copyright (c) 2020 Oleksiy Ryabchun
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * C4dll-R differences from the original implementation:
 *   - the game's native [Settings] IsoBirds switch remains the single visibility setting;
 *     the C4dll-R menu is a restart-only alias for it;
 *   - Wrapper.ff is not loaded: this module owns only the IsoClouds.ff slot;
 *   - every game-code signature is checked before any byte is changed;
 *   - every target page is made writable before the complete hook set is copied,
 *     so protection failure cannot leave a partially installed feature;
 *   - a missing/invalid archive is reported to the feature menu instead of
 *     leaving an inert enabled switch;
 *   - IsoClouds.ff is external game data and is deliberately not redistributed.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" int DDGetGameWidth(void);
extern "C" int clouds_is_available(void);

namespace {

constexpr uintptr_t kImageBase = 0x00400000;

// Exact v3.01a / 2003.12.11.1 sites from the DisciplesGL address table.
constexpr uintptr_t kPackageObjectSize = 0x005AED4C;
constexpr uintptr_t kPackageLoadCall = 0x005AE861;
constexpr uintptr_t kPackageEntryLoad = 0x00523D50;
constexpr uintptr_t kPackageEntrySub = 0x0052EB29;
constexpr uintptr_t kPackageEntrySubTail = 0x0052EBB8;
constexpr uintptr_t kCloudPackageCall = 0x005C818B;
constexpr uintptr_t kCloudCountSite = 0x005BE762;
constexpr uintptr_t kCloudInitFinish = kCloudCountSite + 83;
constexpr uintptr_t kCloudInitEpilogue = 0x005BE836;
constexpr uintptr_t kCloudCheckSite = 0x005BEA1D;
constexpr uintptr_t kCloudCheckContinue = kCloudCheckSite + 89;
constexpr uintptr_t kCloudCheckTail = 0x005BEA68;
constexpr uintptr_t kRequiredImageEnd = 0x006CE000;

// The stock package owner has 24 entries. We add one pointer at store[24].
constexpr int kCloudPackageSlot = 24;

enum CloudStatus {
    kCloudUnsupported = 0,
    kCloudAssetMissing = 1,
    kCloudReady = 2,
    kCloudLoadFailed = 3,
};

volatile LONG g_supported = 0;
volatile LONG g_assetPresent = 0;
volatile LONG g_desired = 0;
volatile LONG g_startupVisible = 0;
volatile LONG g_startupEnabled = 0;
volatile LONG g_hooksInstalled = 0;
volatile LONG g_hookFailed = 0;
volatile LONG g_loadAttempted = 0;

char g_iniPath[MAX_PATH] = {};
char g_gameIniPath[MAX_PATH] = {};
char g_assetPath[MAX_PATH] = {};

// This is the smart-package owner stored by the game's archive loader. Interlocked access
// publishes a fully loaded owner to menu/render threads and clears stale owners before retries.
PVOID volatile g_cloudPackage = nullptr;

using LoadImgPackageFn = DWORD(__stdcall*)(DWORD**, char*, char*, DWORD);
// The exact game routine returns C++ bool in AL only. Declaring BOOL here would make MSVC
// consume stale upper EAX bits and could turn a failed stock lookup into a false success.
using PackageIndexFn = bool(__thiscall*)(DWORD, char*, DWORD, DWORD, DWORD);
using CloudPackageCallFn = void(__thiscall*)();

LoadImgPackageFn g_loadImgPackageOriginal = nullptr;
PackageIndexFn g_packageIndex = reinterpret_cast<PackageIndexFn>(kPackageEntrySub);
uintptr_t g_cloudPackageCallOriginal = 0;
uintptr_t g_cloudCheckContinue = kCloudCheckContinue;

struct CloudItem {
    POINT center;
    POINT offset;
    DWORD speed;
    BYTE isValid;
    BYTE hasShadow;
    WORD reserved;
};

struct CloudObject {
    CloudItem* list;
    int count;
    SIZE mapSize;
    SIZE boundsSize;
};

static_assert(sizeof(CloudItem) == 24, "Discipl2.exe CloudItem layout changed");
static_assert(sizeof(CloudObject) == 24, "Discipl2.exe CloudObject layout changed");

struct PatchPlan {
    BYTE* address;
    BYTE before[8];
    BYTE after[8];
    SIZE_T size;
};

PatchPlan g_plans[8] = {};
int g_planCount = 0;

struct ProtectedPage {
    BYTE* address;
    SIZE_T size;
    DWORD oldProtect;
    bool changed;
};

constexpr int kMaxProtectedPages = 16;

// Storage is required because the naked x87 hook references these symbols directly.
float g_cloudArchiveFactor = 7.0f / 3.0f;
float g_cloudCount = 15.0f;

bool pathNextToExe(char* out, size_t capacity, const char* relative)
{
    if (!out || capacity == 0 || !relative)
        return false;

    char exe[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return false;

    char* slash = std::strrchr(exe, '\\');
    if (!slash)
        slash = std::strrchr(exe, '/');
    if (!slash)
        return false;
    slash[1] = '\0';

    const int written = std::snprintf(out, capacity, "%s%s", exe, relative);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool combineGamePath(char* out, size_t capacity, const char* directory, const char* filename)
{
    if (!out || capacity == 0 || !filename || !filename[0])
        return false;

    // Match the path that the game's package loader receives. Absolute filenames need no prefix.
    const bool absolute =
        (filename[0] && filename[1] == ':') ||
        ((filename[0] == '\\' || filename[0] == '/') &&
         (filename[1] == '\\' || filename[1] == '/'));
    int written = 0;
    if (absolute || !directory || !directory[0]) {
        written = std::snprintf(out, capacity, "%s", filename);
    } else {
        const size_t length = std::strlen(directory);
        const bool hasSlash =
            length != 0 && (directory[length - 1] == '\\' || directory[length - 1] == '/');
        written =
            std::snprintf(out, capacity, hasSlash ? "%s%s" : "%s\\%s", directory, filename);
    }
    return written > 0 && static_cast<size_t>(written) < capacity;
}

struct Sha256State {
    std::uint32_t words[8];
    std::uint64_t totalBytes;
    BYTE block[64];
    size_t blockBytes;
};

constexpr std::uint32_t kSha256Constants[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u,
    0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u,
    0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u,
    0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au,
    0x5B9CCA4Fu, 0x682E6FF3u, 0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

std::uint32_t rotateRight(std::uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

void sha256Transform(Sha256State* state, const BYTE* block)
{
    std::uint32_t schedule[64] = {};
    for (size_t i = 0; i < 16; ++i) {
        const size_t offset = i * 4;
        schedule[i] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                      (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                      static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotateRight(schedule[i - 15], 7) ^
                                 rotateRight(schedule[i - 15], 18) ^
                                 (schedule[i - 15] >> 3);
        const std::uint32_t s1 = rotateRight(schedule[i - 2], 17) ^
                                 rotateRight(schedule[i - 2], 19) ^
                                 (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    std::uint32_t a = state->words[0];
    std::uint32_t b = state->words[1];
    std::uint32_t c = state->words[2];
    std::uint32_t d = state->words[3];
    std::uint32_t e = state->words[4];
    std::uint32_t f = state->words[5];
    std::uint32_t g = state->words[6];
    std::uint32_t h = state->words[7];
    for (size_t i = 0; i < 64; ++i) {
        const std::uint32_t sum1 =
            rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + sum1 + choose + kSha256Constants[i] + schedule[i];
        const std::uint32_t sum0 =
            rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state->words[0] += a;
    state->words[1] += b;
    state->words[2] += c;
    state->words[3] += d;
    state->words[4] += e;
    state->words[5] += f;
    state->words[6] += g;
    state->words[7] += h;
}

void sha256Init(Sha256State* state)
{
    static const std::uint32_t initial[8] = {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };
    std::memcpy(state->words, initial, sizeof(initial));
    state->totalBytes = 0;
    state->blockBytes = 0;
    std::memset(state->block, 0, sizeof(state->block));
}

void sha256Update(Sha256State* state, const BYTE* bytes, size_t length)
{
    if (!length)
        return;
    state->totalBytes += static_cast<std::uint64_t>(length);
    while (length) {
        const size_t available = sizeof(state->block) - state->blockBytes;
        const size_t take = length < available ? length : available;
        std::memcpy(state->block + state->blockBytes, bytes, take);
        state->blockBytes += take;
        bytes += take;
        length -= take;
        if (state->blockBytes == sizeof(state->block)) {
            sha256Transform(state, state->block);
            state->blockBytes = 0;
        }
    }
}

void sha256Final(Sha256State* state, BYTE digest[32])
{
    const std::uint64_t totalBits = state->totalBytes * 8u;
    BYTE padding[64] = {0x80};
    const size_t paddingBytes =
        state->blockBytes < 56 ? 56 - state->blockBytes : 120 - state->blockBytes;
    sha256Update(state, padding, paddingBytes);

    BYTE lengthBytes[8] = {};
    for (size_t i = 0; i < sizeof(lengthBytes); ++i)
        lengthBytes[sizeof(lengthBytes) - 1 - i] =
            static_cast<BYTE>(totalBits >> static_cast<unsigned>(i * 8));
    sha256Update(state, lengthBytes, sizeof(lengthBytes));

    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<BYTE>(state->words[i] >> 24);
        digest[i * 4 + 1] = static_cast<BYTE>(state->words[i] >> 16);
        digest[i * 4 + 2] = static_cast<BYTE>(state->words[i] >> 8);
        digest[i * 4 + 3] = static_cast<BYTE>(state->words[i]);
    }
}

bool reviewedAssetPresent(const char* path)
{
    constexpr DWORD kReviewedSize = 30393;
    static const BYTE kReviewedSha256[32] = {
        0x96, 0x2F, 0x33, 0x4E, 0x1C, 0xFA, 0x32, 0x26, 0xAF, 0x27, 0xB9,
        0x53, 0xAF, 0x0F, 0x6E, 0xBA, 0x6C, 0x1F, 0x82, 0xEF, 0x70, 0x8A,
        0x94, 0x8C, 0x0D, 0x4C, 0x2A, 0x76, 0xFF, 0x80, 0x4E, 0xE6,
    };

    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!path || !GetFileAttributesExA(path, GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.nFileSizeHigh != 0 ||
        data.nFileSizeLow != kReviewedSize)
        return false;

    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    Sha256State hash = {};
    sha256Init(&hash);
    BYTE chunk[4096] = {};
    std::uint64_t totalRead = 0;
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, chunk, sizeof(chunk), &read, nullptr)) {
            ok = false;
            break;
        }
        if (!read)
            break;
        sha256Update(&hash, chunk, static_cast<size_t>(read));
        totalRead += static_cast<std::uint64_t>(read);
    }
    CloseHandle(file);

    BYTE digest[32] = {};
    if (ok)
        sha256Final(&hash, digest);
    return ok && totalRead == kReviewedSize &&
           std::memcmp(digest, kReviewedSha256, sizeof(digest)) == 0;
}

bool addressIsExpected()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != kImageBase)
        return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->OptionalHeader.SizeOfImage >= kRequiredImageEnd - kImageBase;
}

bool readableRange(uintptr_t address, SIZE_T size)
{
    if (!size || address < kImageBase || address > kRequiredImageEnd ||
        size > kRequiredImageEnd - address)
        return false;

    uintptr_t cursor = address;
    const uintptr_t end = address + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = regionEnd < end ? regionEnd : end;
    }
    return true;
}

bool bytesAre(uintptr_t address, const BYTE* expected, SIZE_T size)
{
    return readableRange(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
}

bool queueBytes(uintptr_t address, const BYTE* expected, const BYTE* replacement, SIZE_T size)
{
    if (!expected || !replacement || !size || size > sizeof(PatchPlan::before) ||
        g_planCount >= static_cast<int>(sizeof(g_plans) / sizeof(g_plans[0])) ||
        !bytesAre(address, expected, size))
        return false;

    PatchPlan& plan = g_plans[g_planCount++];
    plan.address = reinterpret_cast<BYTE*>(address);
    plan.size = size;
    std::memcpy(plan.before, expected, size);
    std::memcpy(plan.after, replacement, size);
    return true;
}

bool queueBranch(uintptr_t address, BYTE opcode, const void* target, const BYTE* expected,
                 SIZE_T replacedSize)
{
    if (replacedSize < 5 || replacedSize > sizeof(PatchPlan::before))
        return false;

    const std::int64_t distance =
        reinterpret_cast<uintptr_t>(target) - static_cast<std::int64_t>(address + 5);
    if (distance < (std::numeric_limits<std::int32_t>::min)() ||
        distance > (std::numeric_limits<std::int32_t>::max)())
        return false;

    BYTE replacement[8] = {};
    std::memset(replacement, 0x90, replacedSize);
    replacement[0] = opcode;
    const std::int32_t rel = static_cast<std::int32_t>(distance);
    std::memcpy(replacement + 1, &rel, sizeof(rel));
    return queueBytes(address, expected, replacement, replacedSize);
}

bool collectProtectedPages(ProtectedPage* pages, int capacity, int* pageCount)
{
    if (!pages || capacity <= 0 || !pageCount)
        return false;

    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const SIZE_T pageSize = static_cast<SIZE_T>(systemInfo.dwPageSize);
    if (!pageSize)
        return false;

    int count = 0;
    for (int planIndex = 0; planIndex < g_planCount; ++planIndex) {
        const PatchPlan& plan = g_plans[planIndex];
        const uintptr_t first = reinterpret_cast<uintptr_t>(plan.address);
        const uintptr_t last = first + plan.size - 1;
        uintptr_t page = (first / pageSize) * pageSize;
        const uintptr_t lastPage = (last / pageSize) * pageSize;
        for (;;) {
            bool found = false;
            for (int pageIndex = 0; pageIndex < count; ++pageIndex) {
                if (reinterpret_cast<uintptr_t>(pages[pageIndex].address) == page) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (count >= capacity)
                    return false;
                pages[count].address = reinterpret_cast<BYTE*>(page);
                pages[count].size = pageSize;
                pages[count].oldProtect = 0;
                pages[count].changed = false;
                ++count;
            }
            if (page == lastPage)
                break;
            if (page > (std::numeric_limits<uintptr_t>::max)() - pageSize)
                return false;
            page += pageSize;
        }
    }
    *pageCount = count;
    return count != 0;
}

bool restorePageProtections(ProtectedPage* pages, int pageCount)
{
    bool restored = true;
    for (int i = pageCount; i-- > 0;) {
        if (!pages[i].changed)
            continue;
        DWORD ignored = 0;
        if (!VirtualProtect(pages[i].address, pages[i].size, pages[i].oldProtect, &ignored)) {
            restored = false;
        } else {
            pages[i].changed = false;
        }
    }
    return restored;
}

bool commitPlans()
{
    ProtectedPage pages[kMaxProtectedPages] = {};
    int pageCount = 0;
    if (!collectProtectedPages(pages, kMaxProtectedPages, &pageCount))
        return false;

    // No game byte is touched until every unique page has successfully become writable.
    for (int i = 0; i < pageCount; ++i) {
        DWORD oldProtect = 0;
        if (!VirtualProtect(pages[i].address, pages[i].size, PAGE_EXECUTE_READWRITE,
                            &oldProtect)) {
            const bool restored = restorePageProtections(pages, i);
            if (!restored) {
                OutputDebugStringA(
                    "C4dll-R: cloud hook preflight changed no bytes, but page protection "
                    "restoration failed\n");
            }
            return false;
        }
        pages[i].oldProtect = oldProtect;
        pages[i].changed = true;
    }

    // With all pages writable, memcpy cannot produce an API-level half-transaction: install the
    // complete reviewed set first, then flush and independently restore every page's protection.
    for (int i = 0; i < g_planCount; ++i)
        std::memcpy(g_plans[i].address, g_plans[i].after, g_plans[i].size);

    bool flushed = true;
    for (int i = 0; i < g_planCount; ++i) {
        if (!FlushInstructionCache(GetCurrentProcess(), g_plans[i].address, g_plans[i].size))
            flushed = false;
    }
    const bool restored = restorePageProtections(pages, pageCount);
    if (!flushed)
        OutputDebugStringA("C4dll-R: complete cloud hook set installed; instruction-cache flush "
                           "reported an error\n");
    if (!restored)
        OutputDebugStringA("C4dll-R: complete cloud hook set installed; some code-page "
                           "protections could not be restored\n");
    return true;
}

bool signaturesMatch()
{
    static const BYTE packageSize[] = {0x6A, 0x64};
    static const BYTE packageLoad[] = {0xE8, 0xF0, 0x03, 0x00, 0x00};
    static const BYTE packageEntry[] = {
        0x8B, 0x01, 0xFF, 0x70, 0x14, 0x8B, 0x48, 0x0C, 0xFF, 0x74,
        0x24, 0x10, 0xFF, 0x74, 0x24, 0x10, 0xFF, 0x74, 0x24, 0x10,
        0xE8, 0xC0, 0xAD, 0x00, 0x00, 0xC2, 0x0C, 0x00,
    };
    static const BYTE packageEntrySub[] = {
        0xB8, 0x38, 0xA3, 0x6A, 0x00, 0xE8, 0x9D, 0xE8, 0x13,
        0x00, 0x83, 0xEC, 0x10, 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xF6,
    };
    static const BYTE packageEntrySubTail[] = {
        0x8D, 0x4D, 0xE4, 0xE8, 0x7E, 0xBA, 0xED, 0xFF, 0x32, 0xC0, 0xEB,
        0x11, 0x8D, 0x4D, 0xE4, 0xC7, 0x45, 0xFC, 0x04, 0x00, 0x00, 0x00,
        0xE8, 0x6B, 0xBA, 0xED, 0xFF, 0xB0, 0x01, 0x8B, 0x4D, 0xF4, 0x5F,
        0x64, 0x89, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x5E, 0xC9, 0xC2, 0x10,
        0x00,
    };
    static const BYTE cloudPackage[] = {0xE8, 0xAC, 0xBB, 0xF5, 0xFF};
    static const BYTE cloudCount[] = {0xD8, 0x0D, 0xE0, 0xEF, 0x6E, 0x00};
    static const BYTE cloudInit[] = {0x39, 0x41, 0x04, 0x89, 0x45};
    static const BYTE cloudInitEpilogue[] = {
        0x8B, 0x4D, 0xF4, 0x8B, 0xC6, 0x5F, 0x5E, 0x64, 0x89,
        0x0D, 0x00, 0x00, 0x00, 0x00, 0x5B, 0xC9, 0xC3,
    };
    static const BYTE cloudCheck[] = {0x8B, 0x47, 0x04, 0x8B, 0x4E};
    static const BYTE cloudCheckTail[] = {
        0xE8, 0xFA, 0xA3, 0xF8, 0xFF, 0x8B, 0x4F, 0x04, 0x03,
        0x41, 0x0C, 0x89, 0x46, 0x0C, 0xFF, 0x76, 0x0C, 0xFF,
        0x76, 0x08, 0x56, 0x68, 0x90, 0xA5, 0x83, 0x00, 0x56,
    };

    return addressIsExpected() && bytesAre(kPackageObjectSize, packageSize, sizeof(packageSize)) &&
           bytesAre(kPackageLoadCall, packageLoad, sizeof(packageLoad)) &&
           bytesAre(kPackageEntryLoad, packageEntry, sizeof(packageEntry)) &&
           bytesAre(kPackageEntrySub, packageEntrySub, sizeof(packageEntrySub)) &&
           bytesAre(kPackageEntrySubTail, packageEntrySubTail, sizeof(packageEntrySubTail)) &&
           bytesAre(kCloudPackageCall, cloudPackage, sizeof(cloudPackage)) &&
           bytesAre(kCloudCountSite, cloudCount, sizeof(cloudCount)) &&
           bytesAre(kCloudInitFinish, cloudInit, sizeof(cloudInit)) &&
           bytesAre(kCloudInitEpilogue, cloudInitEpilogue, sizeof(cloudInitEpilogue)) &&
           bytesAre(kCloudCheckSite, cloudCheck, sizeof(cloudCheck)) &&
           bytesAre(kCloudCheckTail, cloudCheckTail, sizeof(cloudCheckTail));
}

int randomPoor(int maximum)
{
    return maximum > 0 ? std::rand() % maximum : 0;
}

DWORD* currentCloudPackage()
{
    return static_cast<DWORD*>(
        InterlockedCompareExchangePointer(&g_cloudPackage, nullptr, nullptr));
}

void publishCloudPackage(DWORD* package)
{
    InterlockedExchangePointer(&g_cloudPackage, package);
}

DWORD* __fastcall selectCloudPackage(DWORD* package, const char* name)
{
    DWORD* clouds = currentCloudPackage();
    if (g_startupEnabled && clouds && name && std::strncmp(name, "CLOUD", 5) == 0)
        return clouds;
    return package;
}

__declspec(naked) void cloudPackageCallHook()
{
    __asm {
        mov edx, [ebp+0x10]
        call selectCloudPackage
        mov ecx, eax
        jmp dword ptr [g_cloudPackageCallOriginal]
    }
}

DWORD __stdcall loadImgPackageHook(DWORD** store, char* directory, char* filename, DWORD unknown)
{
    if (g_startupEnabled && store) {
        // A package-owner retry must never retain a pointer published by an earlier attempt.
        publishCloudPackage(nullptr);
        store[kCloudPackageSlot] = nullptr;
        static char archiveName[] = "IsoClouds.ff";
        char actualPath[MAX_PATH] = {};
        const bool reviewed =
            combineGamePath(actualPath, sizeof(actualPath), directory, archiveName) &&
            reviewedAssetPresent(actualPath);
        InterlockedExchange(&g_loadAttempted, 1);
        if (!reviewed) {
            InterlockedExchange(&g_assetPresent, 0);
            OutputDebugStringA(
                "C4dll-R: package loader's actual IsoClouds.ff failed SHA-256 validation\n");
        } else if (g_loadImgPackageOriginal(store + kCloudPackageSlot, directory, archiveName,
                                            unknown) &&
                   store[kCloudPackageSlot]) {
            publishCloudPackage(store[kCloudPackageSlot]);
        }
    }

    return g_loadImgPackageOriginal(store, directory, filename, unknown);
}

bool __fastcall loadPackageIndexHook(DWORD** packageObject, DWORD, char* name, DWORD a3,
                                     DWORD a4)
{
    if (!packageObject || !*packageObject || !g_packageIndex)
        return false;

    DWORD* package = *packageObject;
    const bool original =
        g_packageIndex(package[3], name, a3, a4, package[5]);
    DWORD* cloudPackage = currentCloudPackage();
    if (original || !g_startupEnabled || !cloudPackage)
        return original;

    DWORD* clouds = reinterpret_cast<DWORD*>(*cloudPackage);
    return clouds && g_packageIndex(clouds[3], name, a3, a4, clouds[5]);
}

__declspec(naked) void cloudCountHook()
{
    __asm {
        cmp dword ptr [g_startupEnabled], 0
        je clouds_off
        cmp dword ptr [g_cloudPackage], 0
        je clouds_off
        fmul dword ptr [g_cloudArchiveFactor]
        fmul dword ptr [g_cloudCount]
        ret

    clouds_off:
        // Preserve DisciplesGL's stock-package fallback. Visibility is controlled by the
        // game's native IsoBirds flag; a package-load failure must not force the count to zero.
        fmul dword ptr [g_cloudCount]
        ret
    }
}

void __fastcall initClouds(CloudObject* object, SIZE* mapSize)
{
    if (!object || !object->list || object->count <= 0 || !mapSize)
        return;

    int width = DDGetGameWidth();
    if (width <= 0)
        width = 800;
    const SIZE mode = {width, 128};

    object->mapSize.cx = mapSize->cx + 1;
    object->mapSize.cy = mapSize->cy + 1;
    object->boundsSize.cx = object->mapSize.cx * 64 + mode.cx;
    object->boundsSize.cy = object->mapSize.cy * 32 + mode.cy;

    int x = 0;
    int y = 0;
    CloudItem* cloud = object->list;
    int count = object->count;
    do {
        cloud->isValid = 0;
        cloud->hasShadow = 0;
        cloud->reserved = 0;
        cloud->center.x = x;
        cloud->center.y = y;

        if (++y == object->mapSize.cy) {
            y = 0;
            if (++x == object->mapSize.cx)
                x = 0;
        }

        cloud->offset.x =
            randomPoor(object->boundsSize.cx) -
            (object->mapSize.cy - cloud->center.y + cloud->center.x) * 32 - mode.cx / 2;
        cloud->offset.y =
            randomPoor(object->boundsSize.cy) -
            (cloud->center.y + cloud->center.x) * 16 - mode.cy / 2;
        cloud->speed = static_cast<DWORD>(randomPoor(5) + 1);
        ++cloud;
    } while (--count);
}

__declspec(naked) void cloudInitFinishHook()
{
    __asm {
        lea edx, [ebp-0x2C]
        call initClouds
        mov eax, esi
        pop edi
        pop esi
        pop ebx
        mov ecx, [ebp-0xC]
        mov dword ptr fs:[0], ecx
        leave
        ret
    }
}

void __fastcall checkCloud(CloudItem* cloud, CloudObject* object)
{
    if (!cloud || !object)
        return;

    int width = DDGetGameWidth();
    if (width <= 0)
        width = 800;

    const LONG distance =
        (object->mapSize.cy - cloud->center.y + cloud->center.x) * 32 + width / 2;
    if (cloud->offset.x >= object->boundsSize.cx - distance) {
        cloud->offset.x = -distance;
        cloud->offset.y =
            randomPoor(object->boundsSize.cy) -
            (cloud->center.y + cloud->center.x) * 16 - 64;
    }

    if (cloud->hasShadow && cloud->speed > 3)
        cloud->speed = static_cast<DWORD>(randomPoor(3) + 1);
}

__declspec(naked) void cloudCheckHook()
{
    __asm {
        mov edx, [edi+0x4]
        mov ecx, esi
        call checkCloud
        jmp dword ptr [g_cloudCheckContinue]
    }
}

bool installHooks()
{
    static const BYTE packageSizeExpected[] = {0x6A, 0x64};
    static const BYTE packageSizeAfter[] = {0x6A, 0x68};
    static const BYTE packageLoadExpected[] = {0xE8, 0xF0, 0x03, 0x00, 0x00};
    static const BYTE packageEntryExpected[] = {0x8B, 0x01, 0xFF, 0x70, 0x14};
    static const BYTE cloudPackageExpected[] = {0xE8, 0xAC, 0xBB, 0xF5, 0xFF};
    static const BYTE cloudCountExpected[] = {0xD8, 0x0D, 0xE0, 0xEF, 0x6E, 0x00};
    static const BYTE cloudInitExpected[] = {0x39, 0x41, 0x04, 0x89, 0x45};
    static const BYTE cloudCheckExpected[] = {0x8B, 0x47, 0x04, 0x8B, 0x4E};

    g_planCount = 0;
    g_loadImgPackageOriginal = reinterpret_cast<LoadImgPackageFn>(0x005AEC56);
    g_cloudPackageCallOriginal = 0x00523D3C;

    return queueBytes(kPackageObjectSize, packageSizeExpected, packageSizeAfter,
                      sizeof(packageSizeExpected)) &&
           queueBranch(kPackageLoadCall, 0xE8, reinterpret_cast<const void*>(loadImgPackageHook),
                       packageLoadExpected, sizeof(packageLoadExpected)) &&
           queueBranch(kPackageEntryLoad, 0xE9,
                       reinterpret_cast<const void*>(loadPackageIndexHook),
                       packageEntryExpected, sizeof(packageEntryExpected)) &&
           queueBranch(kCloudPackageCall, 0xE8,
                       reinterpret_cast<const void*>(cloudPackageCallHook),
                       cloudPackageExpected, sizeof(cloudPackageExpected)) &&
           queueBranch(kCloudCountSite, 0xE8, reinterpret_cast<const void*>(cloudCountHook),
                       cloudCountExpected, sizeof(cloudCountExpected)) &&
           queueBranch(kCloudInitFinish, 0xE9,
                       reinterpret_cast<const void*>(cloudInitFinishHook),
                       cloudInitExpected, sizeof(cloudInitExpected)) &&
           queueBranch(kCloudCheckSite, 0xE9, reinterpret_cast<const void*>(cloudCheckHook),
                       cloudCheckExpected, sizeof(cloudCheckExpected)) &&
           commitPlans();
}

bool persistDesired(LONG desired)
{
    if (!g_gameIniPath[0])
        return false;
    return WritePrivateProfileStringA("Settings", "IsoBirds", desired ? "1" : "0",
                                      g_gameIniPath) != FALSE;
}

LONG readNativeDesired(bool* present = nullptr)
{
    if (present)
        *present = false;
    if (!g_gameIniPath[0])
        return 0;

    char raw[16] = {};
    GetPrivateProfileStringA("Settings", "IsoBirds", "\x01", raw,
                             static_cast<DWORD>(sizeof(raw)), g_gameIniPath);
    if (raw[0] == '\x01' && raw[1] == 0)
        return 0;
    if (present)
        *present = true;
    return std::atoi(raw) != 0 ? 1 : 0;
}

LONG migrateLegacyDesired(LONG nativeDesired, bool nativeSettingPresent)
{
    // A short-lived local diagnostic build wrote this key to the wrong section. Reconcile it
    // before the public migration so the test installation repairs itself on the next launch.
    char misplaced[16] = {};
    if (g_gameIniPath[0] &&
        GetPrivateProfileStringA("Disciple", "IsoBirds", "", misplaced,
                                 static_cast<DWORD>(sizeof(misplaced)), g_gameIniPath)) {
        bool reconciled = true;
        if (!nativeSettingPresent && std::atoi(misplaced) != 0 && !nativeDesired) {
            reconciled = persistDesired(1);
            if (reconciled)
                nativeDesired = 1;
        }
        if (reconciled)
            WritePrivateProfileStringA("Disciple", "IsoBirds", nullptr, g_gameIniPath);
    }

    if (!g_iniPath[0])
        return nativeDesired;

    char legacy[16] = {};
    if (!GetPrivateProfileStringA("menu", "clouds", "", legacy,
                                  static_cast<DWORD>(sizeof(legacy)), g_iniPath))
        return nativeDesired;

    // 1.5 development builds briefly stored a second cloud switch in C4menu.ini. Only an
    // explicit ON needs carrying forward: a generated/default OFF must not overwrite the
    // game's own option. Once reconciled, remove the duplicate key permanently.
    bool reconciled = true;
    if (!nativeSettingPresent && std::atoi(legacy) != 0 && !nativeDesired) {
        reconciled = persistDesired(1);
        if (reconciled)
            nativeDesired = 1;
    }
    if (reconciled)
        WritePrivateProfileStringA("menu", "clouds", nullptr, g_iniPath);
    return nativeDesired;
}

} // namespace

extern "C" void clouds_install(void)
{
    pathNextToExe(g_iniPath, sizeof(g_iniPath), "C4menu.ini");
    pathNextToExe(g_gameIniPath, sizeof(g_gameIniPath), "Disciple.ini");
    pathNextToExe(g_assetPath, sizeof(g_assetPath), "Imgs\\IsoClouds.ff");

    InterlockedExchange(&g_assetPresent, reviewedAssetPresent(g_assetPath) ? 1 : 0);
    const bool supported = signaturesMatch();
    if (supported)
        InterlockedExchange(&g_supported, 1);

    bool nativeSettingPresent = false;
    LONG nativeDesired = readNativeDesired(&nativeSettingPresent);
    // Clouds are ON by default only when this build can really provide them. Preserve every
    // explicit native IsoBirds=0 opt-out, and avoid creating a checked-but-unusable setting on an
    // unsupported executable or without the reviewed archive.
    if (!nativeSettingPresent && supported && g_assetPresent && persistDesired(1))
        nativeDesired = 1;
    const LONG desired = migrateLegacyDesired(nativeDesired, nativeSettingPresent);
    InterlockedExchange(&g_desired, desired);
    InterlockedExchange(&g_startupVisible, desired);

    if (!supported) {
        OutputDebugStringA("C4dll-R: cloud hooks unavailable for this Discipl2.exe build\n");
        return;
    }

    if (!g_assetPresent) {
        OutputDebugStringA("C4dll-R: Imgs\\IsoClouds.ff is missing; cloud pipeline disabled\n");
        return;
    }

    std::srand(GetTickCount() ^ GetCurrentProcessId());
    if (!installHooks()) {
        InterlockedExchange(&g_hookFailed, 1);
        OutputDebugStringA(
            "C4dll-R: cloud hook page preflight failed; no game bytes were changed\n");
        return;
    }

    InterlockedExchange(&g_hooksInstalled, 1);
    InterlockedExchange(&g_startupEnabled, 1);
    OutputDebugStringA("C4dll-R: optional IsoClouds.ff hooks installed (restart-latched)\n");
}

extern "C" int clouds_set_enabled(int enabled)
{
    const LONG desired = enabled ? 1 : 0;
    if (desired && clouds_is_available() == 0)
        return 0;
    if (!persistDesired(desired))
        return 0;
    InterlockedExchange(&g_desired, desired);
    return 1;
}

extern "C" int clouds_get_enabled(void)
{
    if (g_gameIniPath[0])
        InterlockedExchange(&g_desired, readNativeDesired());
    return g_desired != 0;
}

extern "C" int clouds_get_active(void)
{
    return g_startupVisible != 0 && g_startupEnabled != 0 && g_hooksInstalled != 0;
}

extern "C" int clouds_restart_pending(void)
{
    return clouds_get_enabled() != clouds_get_active();
}

extern "C" int clouds_get_status(void)
{
    if (!g_supported)
        return kCloudUnsupported;
    if (!g_assetPresent)
        return kCloudAssetMissing;
    if (g_hookFailed || (g_startupEnabled && g_loadAttempted && !currentCloudPackage()))
        return kCloudLoadFailed;
    return kCloudReady;
}

extern "C" int clouds_is_available(void)
{
    return clouds_get_status() == kCloudReady;
}
