/*
 * Widescreen battle port for the nine Discipl2.exe layouts supported by the
 * original DisciplesGL address table.
 *
 * The hook behavior and DLG_BATTLE_B layout are derived from DisciplesGL:
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
 * C4dll-R changes from the original implementation:
 *   - exact-build signatures are validated before any byte is written;
 *   - all writes are planned first and rolled back if a page write fails;
 *   - the option is latched at the next battle, so toggling it cannot mutate a
 *     live battle dialog halfway through its lifetime.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" int DDGetGameWidth(void);
extern "C" int DDGetGameHeight(void);
extern "C" int horplus_get_battle_view_width(void);
extern "C" void featuremenu_debug_log_line(const char* line);

namespace {

constexpr uintptr_t kImageBase = 0x00400000;

enum class BattleAbi : BYTE
{
    Expanded,
    CompactV2,
};

struct BattleLayout
{
    DWORD productVersionMS;
    DWORD productVersionLS;
    BattleAbi abi;
    uintptr_t probe;
    uintptr_t classMarker;
    uintptr_t centerBackground;
    uintptr_t centerUnits;
    uintptr_t reverseGroup;
    uintptr_t mouseCheck;
    uintptr_t swapGroup;
    uintptr_t groupsActive;
    uintptr_t initGroupOnePre;
    uintptr_t initGroupOneA;
    uintptr_t initGroupOneB;
    uintptr_t initGroupTwoPre;
    uintptr_t initGroupTwoA;
    uintptr_t initGroupTwoB;
    uintptr_t imageIndices;
    uintptr_t itemsUse;
    uintptr_t dialogOne;
    uintptr_t dialogTwo;
    uintptr_t localStreamRead;
};

// Verbatim fields 67..84 from the nine game records in the legacy table at
// C4dll-R.dll RVA 0x37A70. Duplicate product versions are distinguished by the
// original `push 0xCA0000` probe site, not by publisher or executable name.
constexpr BattleLayout kLayouts[] = {
    {0x07D30004, 0x00070001, BattleAbi::CompactV2, 0x00564A6B,
     0x00625CE5, 0x0063FD28, 0x00626DD3, 0x0062667F, 0x0062A06A,
     0x00630C74, 0x0062696D, 0x006275FE, 0x0062764D, 0x0062767B,
     0x00627AAD, 0x00627AFC, 0x00627B2A, 0x00626AD7, 0x00631B84,
     0x00625C1A, 0x00625EBC, 0},
    {0x07D30005, 0x00100001, BattleAbi::CompactV2, 0x005645FC,
     0x00625C15, 0x0063FB98, 0x00626D03, 0x006265AF, 0x00629F9A,
     0x00630BA4, 0x0062689D, 0x0062752E, 0x0062757D, 0x006275AB,
     0x006279DD, 0x00627A2C, 0x00627A5A, 0x00626A07, 0x00631AB4,
     0x00625B4A, 0x00625DEC, 0},
    {0x07D30005, 0x00100001, BattleAbi::CompactV2, 0x00564D68,
     0x00625FB5, 0x0063FE68, 0x006270A3, 0x0062694F, 0x0062A33A,
     0x00630F44, 0x00626C3D, 0x006278CE, 0x0062791D, 0x0062794B,
     0x00627D7D, 0x00627DCC, 0x00627DFA, 0x00626DA7, 0x00631E54,
     0x00625EEA, 0x0062618C, 0x0050B3AF},
    {0x07D3000A, 0x00170001, BattleAbi::CompactV2, 0x00564C4C,
     0x00625D25, 0x0063FCA8, 0x00626E13, 0x006266BF, 0x0062A0AA,
     0x00630CB4, 0x006269AD, 0x0062763E, 0x0062768D, 0x006276BB,
     0x00627AED, 0x00627B3C, 0x00627B6A, 0x00626B17, 0x00631BC4,
     0x00625C5A, 0x00625EFC, 0},
    {0x07D3000A, 0x00170001, BattleAbi::CompactV2, 0x0056339D,
     0x00624595, 0x0063E6B8, 0x00625683, 0x00624F2F, 0x0062891A,
     0x0062F524, 0x0062521D, 0x00625EAE, 0x00625EFD, 0x00625F2B,
     0x0062635D, 0x006263AC, 0x006263DA, 0x00625387, 0x00630434,
     0x006244CA, 0x0062476C, 0},
    {0x07D3000B, 0x00030001, BattleAbi::Expanded, 0x00566D63,
     0x0062CCE5, 0x00646BF8, 0x0062DE6E, 0x0062D6E8, 0x0063123E,
     0x006381F8, 0x0062DA08, 0x0062E75B, 0x0062E7A8, 0x0062E7D6,
     0x0062EC09, 0x0062EC5C, 0x0062EC90, 0x0062DB72, 0x00639247,
     0x0062CC1A, 0x0062CF25, 0},
    {0x07D3000B, 0x00030001, BattleAbi::Expanded, 0x005674DE,
     0x0062D6C5, 0x00647508, 0x0062E84E, 0x0062E0C8, 0x00631C1E,
     0x00638BD8, 0x0062E3E8, 0x0062F13B, 0x0062F188, 0x0062F1B6,
     0x0062F5E9, 0x0062F63C, 0x0062F670, 0x0062E552, 0x00639C27,
     0x0062D5FA, 0x0062D905, 0x0050E535},
    {0x07D3000C, 0x000B0001, BattleAbi::Expanded, 0x005676DA,
     0x0062E345, 0x006482A8, 0x0062F4CE, 0x0062ED48, 0x0063289E,
     0x00639858, 0x0062F068, 0x0062FDBB, 0x0062FE08, 0x0062FE36,
     0x00630269, 0x006302BC, 0x006302F0, 0x0062F1D2, 0x0063A8A7,
     0x0062E27A, 0x0062E585, 0},
    {0x07D3000C, 0x000B0001, BattleAbi::Expanded, 0x00566E04,
     0x0062CD85, 0x00646B28, 0x0062DF0E, 0x0062D788, 0x006312DE,
     0x00638298, 0x0062DAA8, 0x0062E7FB, 0x0062E848, 0x0062E876,
     0x0062ECA9, 0x0062ECFC, 0x0062ED30, 0x0062DC12, 0x006392E7,
     0x0062CCBA, 0x0062CFC5, 0},
};

volatile LONG g_wideAllowed = 1; // original wrapper's first-run default
volatile LONG g_wideActive = 0;  // latched by the battle-dialog creation hook
volatile LONG g_wideAvailable = 0;
volatile LONG g_battleCanvasWidth = 800;
volatile LONG g_battleCanvasHeight = 600;

uintptr_t g_swapGroupContinue = 0;
uintptr_t g_imageIndicesOriginal = 0;
uintptr_t g_itemsAttackBranch = 0;
uintptr_t g_imageEnd = 0;
uintptr_t g_centerUnitsCalls[2] = {};
const BattleLayout* g_layout = nullptr;

using CenterUnitsFn = void(__stdcall*)(DWORD*);
using MouseInLeftSideFn = BOOL(__thiscall*)(DWORD*, POINT*);
using FgetsFn = char*(__cdecl*)(char*, int, FILE*);
using LocalStreamReadFn = char*(__stdcall*)(char*, int, FILE*);

CenterUnitsFn g_centerUnitsOriginal = nullptr;
MouseInLeftSideFn g_mouseInLeftSideOriginal = nullptr;
FgetsFn g_fgetsOriginal = nullptr;
LocalStreamReadFn g_localStreamReadOriginal = nullptr;

const unsigned char* g_dialogData = nullptr;
DWORD g_dialogSize = 0;
DWORD g_dialogPosition = 0;
LONG g_dialogExhausted = 0;

struct RectI
{
    LONG x;
    LONG y;
    LONG width;
    LONG height;
};

struct SizeI
{
    DWORD width;
    DWORD height;
};

struct SpritePosition
{
    POINT dstPos;
    RectI srcRect;
};

struct ImageIndices
{
    SizeI size;
    const SpritePosition* indices;
    DWORD count;
};

struct PatchPlan
{
    void* address;
    BYTE before[8];
    BYTE after[8];
    SIZE_T size;
};

PatchPlan g_plans[24] = {};
int g_planCount = 0;

bool initializeImageRange()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != kImageBase)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage < 0x1000 ||
        nt->OptionalHeader.SizeOfImage >
            (std::numeric_limits<uintptr_t>::max)() - kImageBase)
        return false;
    g_imageEnd = kImageBase + nt->OptionalHeader.SizeOfImage;
    return true;
}

bool readableRange(uintptr_t address, SIZE_T size)
{
    if (!size || !g_imageEnd || address < kImageBase || address > g_imageEnd ||
        size > g_imageEnd - address)
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

bool readExecutableProductVersion(DWORD* productVersionMS, DWORD* productVersionLS)
{
    if (!productVersionMS || !productVersionLS)
        return false;
    HMODULE executable = GetModuleHandleA(nullptr);
    HRSRC resource = FindResourceA(executable, MAKEINTRESOURCEA(1), RT_VERSION);
    if (!resource)
        return false;
    HGLOBAL loaded = LoadResource(executable, resource);
    const BYTE* data = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    const DWORD size = loaded ? SizeofResource(executable, resource) : 0;
    if (!data || size < sizeof(VS_FIXEDFILEINFO))
        return false;

    // VS_FIXEDFILEINFO is DWORD-aligned inside VS_VERSION_INFO.  Scanning the
    // bounded resource avoids a loader-lock dependency on version.dll.
    for (DWORD offset = 0; offset <= size - sizeof(VS_FIXEDFILEINFO); offset += 4) {
        VS_FIXEDFILEINFO info = {};
        std::memcpy(&info, data + offset, sizeof(info));
        if (info.dwSignature != 0xFEEF04BD || info.dwStrucVersion != 0x00010000)
            continue;
        // DisciplesGL's selector uses ProductVersion, not FileVersion. Some
        // distributions keep FileVersion compatible while changing the
        // product identity, so using the other pair would select a wrong ABI.
        *productVersionMS = info.dwProductVersionMS;
        *productVersionLS = info.dwProductVersionLS;
        return true;
    }
    return false;
}

const BattleLayout* selectLayout()
{
    if (!initializeImageRange())
        return nullptr;
    DWORD productVersionMS = 0;
    DWORD productVersionLS = 0;
    if (!readExecutableProductVersion(&productVersionMS, &productVersionLS))
        return nullptr;

    static const BYTE probeBytes[] = {0x68, 0x00, 0x00, 0xCA, 0x00};
    const BattleLayout* selected = nullptr;
    for (const BattleLayout& layout : kLayouts) {
        if (layout.productVersionMS != productVersionMS ||
            layout.productVersionLS != productVersionLS ||
            !readableRange(layout.probe, sizeof(probeBytes)) ||
            std::memcmp(reinterpret_cast<const void*>(layout.probe), probeBytes,
                        sizeof(probeBytes)) != 0)
            continue;
        if (selected)
            return nullptr; // ambiguous duplicate-version probe: fail closed
        selected = &layout;
    }
    return selected;
}

bool bytesAre(uintptr_t address, const BYTE* expected, SIZE_T size)
{
    return readableRange(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
}

bool callTargets(uintptr_t address, uintptr_t target)
{
    if (!readableRange(address, 5))
        return false;
    const auto* p = reinterpret_cast<const BYTE*>(address);
    if (p[0] != 0xE8)
        return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, p + 1, sizeof(rel));
    return address + 5 + rel == target;
}

uintptr_t callTarget(uintptr_t address)
{
    if (!readableRange(address, 5) || *reinterpret_cast<const BYTE*>(address) != 0xE8)
        return 0;
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(address + 1), sizeof(rel));
    const std::intptr_t target = static_cast<std::intptr_t>(address + 5) + rel;
    return target >= static_cast<std::intptr_t>(kImageBase) &&
                   target < static_cast<std::intptr_t>(g_imageEnd)
               ? static_cast<uintptr_t>(target)
               : 0;
}

bool absoluteOperandInImage(uintptr_t address)
{
    if (!readableRange(address, sizeof(DWORD)))
        return false;
    DWORD operand = 0;
    std::memcpy(&operand, reinterpret_cast<const void*>(address), sizeof(operand));
    return operand >= kImageBase && operand < g_imageEnd;
}

bool findCenterUnitsCalls(const BattleLayout& layout)
{
    g_centerUnitsCalls[0] = 0;
    g_centerUnitsCalls[1] = 0;
    if (layout.reverseGroup >= layout.groupsActive ||
        !readableRange(layout.reverseGroup,
                       layout.groupsActive - layout.reverseGroup))
        return false;

    int found = 0;
    for (uintptr_t address = layout.reverseGroup;
         address + 5 <= layout.groupsActive; ++address) {
        if (*reinterpret_cast<const BYTE*>(address) != 0xE8 ||
            !callTargets(address, layout.centerUnits))
            continue;
        if (found >= 2)
            return false;
        g_centerUnitsCalls[found++] = address;
        address += 4;
    }
    return found == 2;
}

bool addPatch(void* address, const void* replacement, SIZE_T size)
{
    if (!address || !replacement ||
        !readableRange(reinterpret_cast<uintptr_t>(address), size) ||
        size > sizeof(g_plans[0].before) ||
        g_planCount >= static_cast<int>(sizeof(g_plans) / sizeof(g_plans[0])))
        return false;

    PatchPlan& plan = g_plans[g_planCount++];
    plan.address = address;
    plan.size = size;
    std::memcpy(plan.before, address, size);
    std::memcpy(plan.after, replacement, size);
    return true;
}

bool addNops(uintptr_t address, SIZE_T size)
{
    BYTE bytes[8];
    if (size > sizeof(bytes))
        return false;
    std::memset(bytes, 0x90, size);
    return addPatch(reinterpret_cast<void*>(address), bytes, size);
}

bool makeRel32(uintptr_t address, const void* target, BYTE opcode, BYTE* out, SIZE_T size)
{
    if (size < 5 || size > 8)
        return false;
    const std::intptr_t delta =
        reinterpret_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(address + 5);
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)())
        return false;

    out[0] = opcode;
    const std::int32_t rel = static_cast<std::int32_t>(delta);
    std::memcpy(out + 1, &rel, sizeof(rel));
    if (size > 5)
        std::memset(out + 5, 0x90, size - 5);
    return true;
}

bool addRelPatch(uintptr_t address, const void* target, BYTE opcode, SIZE_T size)
{
    BYTE bytes[8];
    if (!makeRel32(address, target, opcode, bytes, size))
        return false;
    return addPatch(reinterpret_cast<void*>(address), bytes, size);
}

struct WritableRegion
{
    void* base;
    SIZE_T size;
    DWORD oldProtect;
};

bool applyPlans()
{
    // Make every affected allocation region writable before copying the first byte. A failed
    // VirtualProtect therefore leaves the executable unmodified instead of requiring a best-effort
    // rollback of an already-partial hook set.
    WritableRegion regions[48] = {}; // each <=8-byte patch can cross at most two regions
    int regionCount = 0;
    for (int i = 0; i < g_planCount; ++i) {
        uintptr_t cursor = reinterpret_cast<uintptr_t>(g_plans[i].address);
        const uintptr_t end = cursor + g_plans[i].size;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT) {
                for (int j = regionCount - 1; j >= 0; --j) {
                    DWORD ignored = 0;
                    VirtualProtect(regions[j].base, regions[j].size,
                                   regions[j].oldProtect, &ignored);
                }
                return false;
            }

            bool known = false;
            for (int j = 0; j < regionCount; ++j)
                if (regions[j].base == mbi.BaseAddress &&
                    regions[j].size == mbi.RegionSize) {
                    known = true;
                    break;
                }
            if (!known) {
                if (regionCount >= static_cast<int>(sizeof(regions) / sizeof(regions[0]))) {
                    for (int j = regionCount - 1; j >= 0; --j) {
                        DWORD ignored = 0;
                        VirtualProtect(regions[j].base, regions[j].size,
                                       regions[j].oldProtect, &ignored);
                    }
                    return false;
                }
                WritableRegion& region = regions[regionCount];
                region.base = mbi.BaseAddress;
                region.size = mbi.RegionSize;
                if (!VirtualProtect(region.base, region.size, PAGE_EXECUTE_READWRITE,
                                    &region.oldProtect)) {
                    for (int j = regionCount - 1; j >= 0; --j) {
                        DWORD ignored = 0;
                        VirtualProtect(regions[j].base, regions[j].size,
                                       regions[j].oldProtect, &ignored);
                    }
                    return false;
                }
                ++regionCount;
            }

            const uintptr_t regionEnd =
                reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            cursor = regionEnd < end ? regionEnd : end;
        }
    }

    bool writesVerified = true;
    int written = 0;
    for (int i = 0; i < g_planCount; ++i) {
        std::memcpy(g_plans[i].address, g_plans[i].after, g_plans[i].size);
        ++written;
        if (std::memcmp(g_plans[i].address, g_plans[i].after,
                        g_plans[i].size) != 0) {
            writesVerified = false;
            break;
        }
    }

    if (!writesVerified) {
        for (int i = written - 1; i >= 0; --i)
            std::memcpy(g_plans[i].address, g_plans[i].before, g_plans[i].size);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    bool protectionsRestored = true;
    for (int i = regionCount - 1; i >= 0; --i) {
        DWORD ignored = 0;
        if (!VirtualProtect(regions[i].base, regions[i].size,
                            regions[i].oldProtect, &ignored))
            protectionsRestored = false;
    }
    if (!writesVerified)
        OutputDebugStringA(
            "C4dll-R: WideBattle patch verification failed; original bytes restored\n");
    if (!protectionsRestored)
        OutputDebugStringA(
            "C4dll-R: WideBattle hooks are complete, but a page protection restore failed\n");
    return writesVerified;
}

void** findImportSlot(const char* importName)
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    if (!base)
        return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        auto* names = desc->OriginalFirstThunk
                          ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
                          : nullptr;
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        if (!names)
            continue;
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const auto* entry =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<const char*>(entry->Name), importName) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

bool loadDialogResource()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(
                                reinterpret_cast<uintptr_t>(&loadDialogResource)),
                            &self))
        return false;
    HRSRC resource = FindResourceA(self, MAKEINTRESOURCEA(10), RT_RCDATA);
    if (!resource)
        return false;
    HGLOBAL loaded = LoadResource(self, resource);
    if (!loaded)
        return false;
    g_dialogData = static_cast<const unsigned char*>(LockResource(loaded));
    g_dialogSize = SizeofResource(self, resource);
    return g_dialogData && g_dialogSize > 32;
}

bool injectedDialogLine(char* buffer, int maxCount)
{
    if (!buffer || maxCount <= 0 || InterlockedExchangeAdd(&g_dialogExhausted, 0))
        return false;
    if (!g_dialogData || g_dialogPosition >= g_dialogSize) {
        InterlockedExchange(&g_dialogExhausted, 1);
        return false;
    }

    DWORD remaining = g_dialogSize - g_dialogPosition;
    const unsigned char* start = g_dialogData + g_dialogPosition;
    DWORD length = 0;
    while (length < remaining && start[length] != '\n' && start[length] != '\0')
        ++length;

    const DWORD capacity = static_cast<DWORD>(maxCount - 1);
    const DWORD copied = length <= capacity ? length : capacity;
    std::memcpy(buffer, start, copied);
    buffer[copied] = '\0';
    g_dialogPosition += copied;
    if (copied == length && length < remaining)
        ++g_dialogPosition; // consume the newline/NUL separator
    if (g_dialogPosition >= g_dialogSize)
        InterlockedExchange(&g_dialogExhausted, 1);
    return true;
}

char* __cdecl fgetsHook(char* buffer, int maxCount, FILE* stream)
{
    if (injectedDialogLine(buffer, maxCount))
        return buffer;
    return g_fgetsOriginal ? g_fgetsOriginal(buffer, maxCount, stream) : nullptr;
}

char* __stdcall localStreamReadHook(char* buffer, int maxCount, FILE* stream)
{
    if (injectedDialogLine(buffer, maxCount))
        return buffer;
    return g_localStreamReadOriginal
               ? g_localStreamReadOriginal(buffer, maxCount, stream)
               : nullptr;
}

void __stdcall calculateWideBattle()
{
    const int canvasWidth = DDGetGameWidth();
    const int canvasHeight = DDGetGameHeight();
    const int battleViewWidth = horplus_get_battle_view_width();
    const bool active = InterlockedExchangeAdd(&g_wideAvailable, 0) &&
                        InterlockedExchangeAdd(&g_wideAllowed, 0) &&
                        canvasWidth >= 990 &&
                        battleViewWidth >= 990;
    InterlockedExchange(&g_wideActive, active ? 1 : 0);
    InterlockedExchange(&g_battleCanvasWidth, canvasWidth > 0 ? canvasWidth : 800);
    InterlockedExchange(&g_battleCanvasHeight, canvasHeight > 0 ? canvasHeight : 600);

    char line[224] = {};
    const int sphereWidth = active ? 990 : 800;
    wsprintfA(line,
              "[widebattle] latch canvas=%dx%d fixedView=%d available=%d enabled=%d active=%d "
              "dialog=%s sphereOffset=%d,%d",
              canvasWidth, canvasHeight, battleViewWidth,
              InterlockedExchangeAdd(&g_wideAvailable, 0) != 0 ? 1 : 0,
              InterlockedExchangeAdd(&g_wideAllowed, 0) != 0 ? 1 : 0,
              active ? 1 : 0, active ? "DLG_BATTLE_B" : "DLG_BATTLE_A",
              (canvasWidth - sphereWidth) / 2, (canvasHeight - 600) / 2);
    featuremenu_debug_log_line(line);
}

// Original DisciplesGL "Sphere fix". The game computes the green summon/target marker in the
// centered battle-dialog coordinate system, but stores it as if that dialog started at canvas 0,0.
// Adjust by half the difference between the real canvas and the active 800/990 x 600 battle area.
// These thunks replace `mov eax,[ebp-38h/40h]; mov [eax],edx`, so EDX is the computed coordinate and
// EAX is the only scratch register the original sequence was allowed to change.
void __declspec(naked) sphereXHook()
{
    __asm {
        mov eax, dword ptr [g_battleCanvasWidth]
        sar eax, 1
        add edx, eax
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jnz wide
        mov eax, 800
        jmp center
    wide:
        mov eax, 990
    center:
        sar eax, 1
        sub edx, eax
        mov eax, [ebp-38h]
        mov [eax], edx
        retn
    }
}

void __declspec(naked) sphereYHook()
{
    __asm {
        mov eax, dword ptr [g_battleCanvasHeight]
        sar eax, 1
        add edx, eax
        sub edx, 300
        mov eax, [ebp-40h]
        mov [eax], edx
        retn
    }
}

void __stdcall centerUnitsHook(DWORD* object)
{
    if (!InterlockedExchangeAdd(&g_wideActive, 0))
        g_centerUnitsOriginal(object);
}

BOOL __fastcall mouseInLeftSideHook(DWORD* object, void*, POINT* point)
{
    if (InterlockedExchangeAdd(&g_wideActive, 0))
        return FALSE;
    return g_mouseInLeftSideOriginal(object, point);
}

void __declspec(naked) swapGroupExpandedHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        retn 4

    non_wide:
        push ebp
        mov ebp, esp
        sub esp, 10h
        jmp dword ptr [g_swapGroupContinue]
    }
}

void __declspec(naked) swapGroupCompactHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        retn

    non_wide:
        push ebp
        mov ebp, esp
        sub esp, 10h
        jmp dword ptr [g_swapGroupContinue]
    }
}

void __declspec(naked) setGroupsActiveExpandedHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide

        mov ecx, [ecx+1Ch]
        mov eax, [ecx+1384h]
        mov eax, [eax+8]
        mov byte ptr [eax+50h], 1
        mov eax, [ecx+1388h]
        mov eax, [eax+8]
        mov byte ptr [eax+50h], 1
        retn 4

    non_wide:
        push ebp
        mov ebp, esp
        sub esp, 10h
        jmp dword ptr [g_swapGroupContinue]
    }
}

void __declspec(naked) setGroupsActiveCompactHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide

        mov ecx, [ecx+1Ch]
        mov eax, [ecx+09E4h]
        mov eax, [eax+8]
        mov byte ptr [eax+50h], 1
        mov eax, [ecx+09E8h]
        mov eax, [eax+8]
        mov byte ptr [eax+50h], 1
        retn

    non_wide:
        push ebp
        mov ebp, esp
        sub esp, 10h
        jmp dword ptr [g_swapGroupContinue]
    }
}

void __declspec(naked) reverseGroupExpandedHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        mov dl, [ecx+14F7h]
        retn

    non_wide:
        mov dl, [ecx+14F6h]
        retn
    }
}

void __declspec(naked) reverseGroupCompactHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        inc edx
        retn

    non_wide:
        mov dl, [ecx+0B55h]
        retn
    }
}

void __declspec(naked) initGroupOneExpandedHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        add ecx, 1398h
        retn

    non_wide:
        add ecx, 1384h
        retn
    }
}

void __declspec(naked) initGroupTwoExpandedHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        add ecx, 1384h
        retn

    non_wide:
        add ecx, 1398h
        retn
    }
}

// The 2.xx ABI has compact group objects and two call sites whose overwritten
// instructions include argument setup. These thunks reproduce the original
// DisciplesGL stack choreography exactly before returning after the full
// eight-byte site.
void __declspec(naked) initGroupOnePreCompactHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        pop eax
        add ecx, 30h
        push ecx
        jz non_wide
        push 1
        push 1
        push eax
        retn

    non_wide:
        push 1
        push 0
        push eax
        retn
    }
}

void __declspec(naked) initGroupOneCompactHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        add ecx, 09F8h
        retn

    non_wide:
        add ecx, 09E4h
        retn
    }
}

void __declspec(naked) initGroupTwoPreCompactHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        pop eax
        add edx, 30h
        push edx
        jz non_wide
        push 1
        push 0
        push eax
        retn

    non_wide:
        push 1
        push 1
        push eax
        retn
    }
}

void __declspec(naked) initGroupTwoCompactHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide
        add ecx, 09E4h
        retn

    non_wide:
        add ecx, 09F8h
        retn
    }
}

const char* const g_dialogNames[2] = {"DLG_BATTLE_A", "DLG_BATTLE_B"};

void __declspec(naked) battleDialogOneHook()
{
    __asm {
        call calculateWideBattle
        lea ecx, g_dialogNames
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz selected
        add ecx, 4
    selected:
        mov ecx, [ecx]
        retn
    }
}

void __declspec(naked) battleDialogTwoHook()
{
    __asm {
        lea edx, g_dialogNames
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz selected
        add edx, 4
    selected:
        mov edx, [edx]
        retn
    }
}

void __fastcall changeBattleIndices(const ImageIndices*** object)
{
    static const SpritePosition sprites[] = {
        {{0, 96}, {346, 0, 279, 104}},
        {{279, 96}, {594, 0, 20, 104}},
        {{299, 96}, {550, 0, 180, 104}},

        {{511, 96}, {346, 104, 180, 104}},
        {{691, 96}, {458, 104, 20, 104}},
        {{711, 96}, {451, 104, 279, 104}},

        {{0, 64}, {346, 208, 148, 32}},
        {{843, 64}, {346, 240, 147, 32}},
        {{0, 34}, {346, 272, 123, 30}},
        {{869, 35}, {346, 302, 121, 29}},

        {{479, 94}, {346, 331, 32, 106}},
    };
    static const ImageIndices indices = {
        {990, 200}, sprites, static_cast<DWORD>(sizeof(sprites) / sizeof(sprites[0]))};

    object[1][7] = &indices;
}

void __declspec(naked) imageIndicesHook()
{
    __asm {
        mov edx, dword ptr [g_wideActive]
        test edx, edx
        jz original
        push ecx
        mov ecx, eax
        call changeBattleIndices
        pop ecx
    original:
        jmp dword ptr [g_imageIndicesOriginal]
    }
}

void __declspec(naked) centerBackgroundHook()
{
    __asm {
        push dword ptr [eax]
        sub esi, [eax]
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz non_wide

        sar esi, 1
        pop eax
        mov [ebp-8], esi
        retn

    non_wide:
        // DisciplesGL installs this centering correction independently of
        // WideBattle and uses it on the stock 800x600 canvas too. preparePlans
        // has already NOPed the native orientation branch, so falling back to
        // only `sub esi,[eax]` here would force the mirrored/right-aligned
        // offset onto both views (800-950=-150 on the stock battle).
        pop eax
        sub eax, 950
        shr eax, 1
        mov ecx, [ebp+10h]
        test cl, cl
        jnz mirror
        xor esi, esi
        sub esi, eax
        mov [ebp-8], esi
        retn

    mirror:
        add esi, eax
        mov [ebp-8], esi
        retn
    }
}

DWORD __fastcall checkItemRectV2(DWORD object, DWORD attackerItem)
{
    const BYTE side = *reinterpret_cast<BYTE*>(object + 2901);
    return object +
           ((attackerItem && side) || (!attackerItem && !side) ? 1824 : 2240);
}

DWORD __fastcall checkItemRectV3(DWORD object, DWORD attackerItem)
{
    BYTE side = *reinterpret_cast<BYTE*>(object + 5366);
    if (*reinterpret_cast<BYTE*>(object + 5367))
        side = !side;
    return object +
           ((attackerItem && side) || (!attackerItem && !side) ? 4704 : 4288);
}

DWORD __fastcall checkItemRect(DWORD object, DWORD attackerItem)
{
    return g_layout && g_layout->abi == BattleAbi::CompactV2
               ? checkItemRectV2(object, attackerItem)
               : checkItemRectV3(object, attackerItem);
}

void __declspec(naked) itemsUseHook()
{
    __asm {
        mov eax, dword ptr [g_wideActive]
        test eax, eax
        jz original_test

        push ecx
        mov edx, ecx
        mov eax, [ebp-108h]
        mov ecx, [eax+1Ch]
        call checkItemRect
        mov [ebp-24h], eax
        pop ecx

    original_test:
        test ecx, ecx
        jz attack_item
        retn

    attack_item:
        pop eax
        jmp dword ptr [g_itemsAttackBranch]
    }
}

bool nearConditionalTarget(uintptr_t address, BYTE condition, uintptr_t* target)
{
    if (!target || !readableRange(address, 6))
        return false;
    const auto* bytes = reinterpret_cast<const BYTE*>(address);
    if (bytes[0] != 0x0F || bytes[1] != condition)
        return false;
    std::int32_t relative = 0;
    std::memcpy(&relative, bytes + 2, sizeof(relative));
    const std::intptr_t destination =
        static_cast<std::intptr_t>(address + 6) + relative;
    if (destination < static_cast<std::intptr_t>(kImageBase) ||
        destination >= static_cast<std::intptr_t>(g_imageEnd))
        return false;
    *target = static_cast<uintptr_t>(destination);
    return true;
}

bool validateSites(const BattleLayout& layout)
{
    static const BYTE classMarker[] = {0xC7, 0x02};
    static const BYTE centerBranch[] = {0x74, 0x20};
    static const BYTE centerMath[] = {0x2B, 0x30, 0x89, 0x75, 0xF8};
    static const BYTE swap[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};
    static const BYTE reverseV2[] = {0x8A, 0x91, 0x55, 0x0B, 0x00, 0x00};
    static const BYTE reverseV3[] = {0x8A, 0x91, 0xF6, 0x14, 0x00, 0x00};
    static const BYTE initV2OnePre[] = {0x83, 0xC1, 0x30, 0x51,
                                        0x6A, 0x01, 0x6A, 0x00};
    static const BYTE initV2One[] = {0x81, 0xC1, 0xE4, 0x09, 0x00, 0x00};
    static const BYTE initV2TwoPre[] = {0x83, 0xC2, 0x30, 0x52,
                                        0x6A, 0x01, 0x6A, 0x01};
    static const BYTE initV2Two[] = {0x81, 0xC1, 0xF8, 0x09, 0x00, 0x00};
    static const BYTE initV3One[] = {0x81, 0xC1, 0x84, 0x13, 0x00, 0x00};
    static const BYTE initV3Two[] = {0x81, 0xC1, 0x98, 0x13, 0x00, 0x00};
    static const BYTE dialogOne[] = {0x8B, 0x0D};
    static const BYTE dialogTwo[] = {0x8B, 0x15};
    static const BYTE sphereX[] = {0x8B, 0x45, 0xC8, 0x89, 0x10};
    static const BYTE sphereY[] = {0x8B, 0x45, 0xC0, 0x89, 0x10};

    uintptr_t ignoredItemTarget = 0;
    const bool common =
        readableRange(layout.centerUnits, 1) &&
        bytesAre(layout.classMarker, classMarker, sizeof(classMarker)) &&
        absoluteOperandInImage(layout.classMarker + 2) &&
        bytesAre(layout.centerBackground, centerBranch, sizeof(centerBranch)) &&
        bytesAre(layout.centerBackground + 27, centerMath, sizeof(centerMath)) &&
        findCenterUnitsCalls(layout) && callTarget(layout.mouseCheck) != 0 &&
        bytesAre(layout.swapGroup, swap, sizeof(swap)) &&
        callTargets(layout.groupsActive, layout.swapGroup) &&
        callTarget(layout.imageIndices) != 0 &&
        nearConditionalTarget(layout.itemsUse, 0x84, &ignoredItemTarget) &&
        bytesAre(layout.dialogOne, dialogOne, sizeof(dialogOne)) &&
        absoluteOperandInImage(layout.dialogOne + 2) &&
        bytesAre(layout.dialogTwo, dialogTwo, sizeof(dialogTwo)) &&
        absoluteOperandInImage(layout.dialogTwo + 2);
    if (!common)
        return false;

    // The reported artifact and the release target are exact v3.01a Russobit. Keep this omitted
    // legacy hook target-specific until equivalent byte sites are independently gated per layout.
    if (layout.probe == 0x005676DA &&
        (!bytesAre(0x0065A018, sphereX, sizeof(sphereX)) ||
         !bytesAre(0x0065A064, sphereY, sizeof(sphereY))))
        return false;

    if (layout.abi == BattleAbi::CompactV2) {
        if (!bytesAre(layout.reverseGroup, reverseV2, sizeof(reverseV2)) ||
            !bytesAre(layout.initGroupOnePre, initV2OnePre,
                      sizeof(initV2OnePre)) ||
            !bytesAre(layout.initGroupOneA, initV2One, sizeof(initV2One)) ||
            !bytesAre(layout.initGroupOneB, initV2One, sizeof(initV2One)) ||
            !bytesAre(layout.initGroupTwoPre, initV2TwoPre,
                      sizeof(initV2TwoPre)) ||
            !bytesAre(layout.initGroupTwoA, initV2Two, sizeof(initV2Two)) ||
            !bytesAre(layout.initGroupTwoB, initV2Two, sizeof(initV2Two)))
            return false;
    } else {
        if (!bytesAre(layout.reverseGroup, reverseV3, sizeof(reverseV3)) ||
            !bytesAre(layout.initGroupOneA, initV3One, sizeof(initV3One)) ||
            !bytesAre(layout.initGroupOneB, initV3One, sizeof(initV3One)) ||
            !bytesAre(layout.initGroupTwoA, initV3Two, sizeof(initV3Two)) ||
            !bytesAre(layout.initGroupTwoB, initV3Two, sizeof(initV3Two)))
            return false;
    }

    if (layout.localStreamRead)
        return callTarget(layout.localStreamRead) != 0;

    void** fgetsSlot = findImportSlot("fgets");
    return fgetsSlot &&
           readableRange(reinterpret_cast<uintptr_t>(fgetsSlot), sizeof(*fgetsSlot)) &&
           *fgetsSlot;
}

bool preparePlans(const BattleLayout& layout)
{
    g_planCount = 0;
    g_centerUnitsOriginal = reinterpret_cast<CenterUnitsFn>(layout.centerUnits);
    g_mouseInLeftSideOriginal =
        reinterpret_cast<MouseInLeftSideFn>(callTarget(layout.mouseCheck));
    g_swapGroupContinue = layout.swapGroup + 6;
    g_imageIndicesOriginal = callTarget(layout.imageIndices);
    if (!nearConditionalTarget(layout.itemsUse, 0x84, &g_itemsAttackBranch))
        return false;

    g_fgetsOriginal = nullptr;
    g_localStreamReadOriginal = nullptr;
    void** fgetsSlot = nullptr;
    FgetsFn fgetsReplacement = &fgetsHook;
    if (layout.localStreamRead) {
        g_localStreamReadOriginal =
            reinterpret_cast<LocalStreamReadFn>(callTarget(layout.localStreamRead));
        if (!g_localStreamReadOriginal)
            return false;
    } else {
        fgetsSlot = findImportSlot("fgets");
        if (!fgetsSlot || !*fgetsSlot)
            return false;
        g_fgetsOriginal = reinterpret_cast<FgetsFn>(*fgetsSlot);
    }

    const bool compact = layout.abi == BattleAbi::CompactV2;
    if (!addNops(layout.centerBackground, 2) ||
        !addRelPatch(layout.centerBackground + 27,
                     reinterpret_cast<const void*>(&centerBackgroundHook), 0xE8, 5) ||
        !addRelPatch(g_centerUnitsCalls[0],
                     reinterpret_cast<const void*>(&centerUnitsHook), 0xE8, 5) ||
        !addRelPatch(g_centerUnitsCalls[1],
                     reinterpret_cast<const void*>(&centerUnitsHook), 0xE8, 5) ||
        !addRelPatch(layout.mouseCheck,
                     reinterpret_cast<const void*>(&mouseInLeftSideHook), 0xE8, 5) ||
        !addRelPatch(layout.swapGroup,
                     reinterpret_cast<const void*>(compact ? &swapGroupCompactHook
                                                           : &swapGroupExpandedHook),
                     0xE9, 5) ||
        !addRelPatch(layout.groupsActive,
                     reinterpret_cast<const void*>(compact ? &setGroupsActiveCompactHook
                                                           : &setGroupsActiveExpandedHook),
                     0xE8, 5) ||
        !addRelPatch(layout.reverseGroup,
                     reinterpret_cast<const void*>(compact ? &reverseGroupCompactHook
                                                           : &reverseGroupExpandedHook),
                     0xE8, 6))
        return false;

    if (compact) {
        if (!addRelPatch(layout.initGroupOnePre,
                         reinterpret_cast<const void*>(&initGroupOnePreCompactHook),
                         0xE8, 8) ||
            !addRelPatch(layout.initGroupOneA,
                         reinterpret_cast<const void*>(&initGroupOneCompactHook),
                         0xE8, 6) ||
            !addRelPatch(layout.initGroupOneB,
                         reinterpret_cast<const void*>(&initGroupOneCompactHook),
                         0xE8, 6) ||
            !addRelPatch(layout.initGroupTwoPre,
                         reinterpret_cast<const void*>(&initGroupTwoPreCompactHook),
                         0xE8, 8) ||
            !addRelPatch(layout.initGroupTwoA,
                         reinterpret_cast<const void*>(&initGroupTwoCompactHook),
                         0xE8, 6) ||
            !addRelPatch(layout.initGroupTwoB,
                         reinterpret_cast<const void*>(&initGroupTwoCompactHook),
                         0xE8, 6))
            return false;
    } else {
        if (!addRelPatch(layout.initGroupOneA,
                         reinterpret_cast<const void*>(&initGroupOneExpandedHook),
                         0xE8, 6) ||
            !addRelPatch(layout.initGroupOneB,
                         reinterpret_cast<const void*>(&initGroupOneExpandedHook),
                         0xE8, 6) ||
            !addRelPatch(layout.initGroupTwoA,
                         reinterpret_cast<const void*>(&initGroupTwoExpandedHook),
                         0xE8, 6) ||
            !addRelPatch(layout.initGroupTwoB,
                         reinterpret_cast<const void*>(&initGroupTwoExpandedHook),
                         0xE8, 6))
            return false;
    }

    if (!addRelPatch(layout.imageIndices,
                     reinterpret_cast<const void*>(&imageIndicesHook), 0xE8, 5) ||
        !addRelPatch(layout.itemsUse,
                     reinterpret_cast<const void*>(&itemsUseHook), 0xE8, 6) ||
        !addRelPatch(layout.dialogOne,
                     reinterpret_cast<const void*>(&battleDialogOneHook), 0xE8, 6) ||
        !addRelPatch(layout.dialogTwo,
                      reinterpret_cast<const void*>(&battleDialogTwoHook), 0xE8, 6))
        return false;

    if (layout.probe == 0x005676DA &&
        (!addRelPatch(0x0065A018, reinterpret_cast<const void*>(&sphereXHook), 0xE8, 5) ||
         !addRelPatch(0x0065A064, reinterpret_cast<const void*>(&sphereYHook), 0xE8, 5)))
        return false;

    if (layout.localStreamRead)
        return addRelPatch(layout.localStreamRead,
                           reinterpret_cast<const void*>(&localStreamReadHook), 0xE8, 5);
    return addPatch(fgetsSlot, &fgetsReplacement, sizeof(fgetsReplacement));
}

} // namespace

extern "C" void widebattle_install(void)
{
    g_layout = selectLayout();
    if (!g_layout || !validateSites(*g_layout) || !loadDialogResource() ||
        !preparePlans(*g_layout) || !applyPlans()) {
        InterlockedExchange(&g_wideAvailable, 0);
        OutputDebugStringA(
            "C4dll-R: WideBattle disabled (unknown layout, unsupported bytes, missing resource, or patch failure)\n");
        return;
    }

    InterlockedExchange(&g_wideAvailable, 1);
    OutputDebugStringA("C4dll-R: WideBattle hooks installed for a supported Discipl2.exe layout\n");
}

extern "C" void widebattle_set_enabled(int enabled)
{
    InterlockedExchange(&g_wideAllowed, enabled ? 1 : 0);
}

extern "C" int widebattle_get_enabled(void)
{
    return InterlockedExchangeAdd(&g_wideAllowed, 0) != 0;
}

extern "C" int widebattle_is_active(void)
{
    // Read-only bridge for Hor+'s legacy primary-surface centering gate. Unlike the user-facing
    // enabled flag, this is the layout choice latched when the current battle dialog was built.
    return InterlockedExchangeAdd(&g_wideActive, 0) != 0;
}

extern "C" int widebattle_is_available(void)
{
    return InterlockedExchangeAdd(&g_wideAvailable, 0) != 0 && DDGetGameWidth() >= 990;
}

extern "C" int widebattle_canvas_hook_is_available(void)
{
    // Hor+ needs the selected layout's shared battle-background centering hook
    // even when the optional 990-wide battle layout itself is switched off.
    return InterlockedExchangeAdd(&g_wideAvailable, 0) != 0;
}
