/*
 * True Hor+ game-canvas support for the nine Discipl2.exe game layouts in
 * DisciplesGL's original AddressSpaceV2 table.
 *
 * The patch sites and sizing formulae were reconstructed from the legacy
 * C4dll-R/DisciplesGL implementation.  Unlike an output stretch, this changes
 * the logical DirectDraw mode created by the game, its strategic-map layout,
 * and the backing allocation limits that depend on the logical dimensions.
 *
 * Safety properties of this port:
 *   - every mandatory byte of the selected executable layout is checked first;
 *   - no game byte is written for native DisplaySize mode or an invalid preset;
 *   - all writes are planned before the first write and rolled back on failure;
 *   - legacy [Wrapper] DisplayWidth/DisplayHeight are never used as arbitrary
 *     custom patch dimensions.
 *
 * The selected canvas is restart-only because the game creates its surfaces
 * and dependent buffers during startup.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#pragma comment(lib, "version.lib")

extern "C" int widebattle_canvas_hook_is_available(void);
extern "C" int widebattle_get_enabled(void);
extern "C" int widebattle_is_active(void);
extern "C" void DDSetGameCanvasMetrics(int width, int height, int injectResolution);
extern "C" void DDInvalidateDecorativeFrame(void);

namespace {

constexpr uintptr_t kImageBase = 0x00400000;

/*
 * The nine game rows from DisciplesGL's original AddressSpaceV2 table.
 * Keep the table's base addresses here and derive every patched operand below;
 * this makes the +N relationships reviewable against Hooks.cpp instead of
 * duplicating already-derived addresses for each executable.
 */
struct AddressLayout
{
    const char* name;
    DWORD productVersionMS;
    DWORD productVersionLS;
    uintptr_t check;
    uintptr_t resHook;
    uintptr_t resBack;
    uintptr_t borderNop;
    uintptr_t borderHook;
    uintptr_t blitSize;
    uintptr_t blitPatch1;
    uintptr_t blitPatch2;
    uintptr_t miniRectJump;
    uintptr_t miniRectPatch;
    uintptr_t rightCurve;
    uintptr_t minimapFill;
    uintptr_t maxSize1;
    uintptr_t maxSize2;
    uintptr_t maxSize3;
    uintptr_t battleClass;
    uintptr_t battleCenterBackground;
    uintptr_t debugPosition;
    uintptr_t messageIconPosition;
    uintptr_t messageTextPosition;
};

const AddressLayout kAddressLayouts[] = {
    {"2.00", 0x07D30004, 0x00070001, 0x00564A6B,
     0x006134D0, 0x00613534, 0x004024F9, 0x0053A3FB,
     0x00515E12, 0x0051A380, 0x00515502, 0x005C85E8,
     0x005C85A9, 0x00489498, 0x0048624A, 0x0051621F,
     0x0051632E, 0x006693B4, 0x00625CE5, 0x0063FD28,
     0x0052FFA3, 0x00485168, 0x0048518A},
    {"2.01", 0x07D30005, 0x00100001, 0x005645FC,
     0x0061331D, 0x00613381, 0x0040263B, 0x0053A1BD,
     0x00515A22, 0x00519FF0, 0x00515112, 0x005C829E,
     0x005C825F, 0x00488C30, 0x004859E2, 0x00515E2F,
     0x00515F3E, 0x00669434, 0x00625C15, 0x0063FB98,
     0x0052FD28, 0x00484900, 0x00484922},
    {"2.01 Steam", 0x07D30005, 0x00100001, 0x00564D68,
     0x00613712, 0x00613776, 0x00402253, 0x0053A89E,
     0x00516222, 0x0051A760, 0x00515912, 0x005C86BF,
     0x005C8680, 0x00488CC7, 0x00485A69, 0x0051662F,
     0x0051673E, 0x0066B804, 0x00625FB5, 0x0063FE68,
     0x00530462, 0x00484960, 0x00484982},
    {"2.02a", 0x07D3000A, 0x00170001, 0x00564C4C,
     0x00613530, 0x00613594, 0x0040254B, 0x0053A68D,
     0x00515F52, 0x0051A520, 0x00515642, 0x005C8610,
     0x005C85D1, 0x004892E5, 0x00486097, 0x0051635F,
     0x0051646E, 0x00669454, 0x00625D25, 0x0063FCA8,
     0x005301F8, 0x00484FB5, 0x00484FD7},
    {"2.02b", 0x07D3000A, 0x00170001, 0x0056339D,
     0x00611C8B, 0x00611CEF, 0x00402129, 0x00538FEB,
     0x00514DD2, 0x005193F0, 0x005144C2, 0x005C6F52,
     0x005C6F13, 0x00489124, 0x00485ED6, 0x005151DF,
     0x005152EE, 0x00667DC4, 0x00624595, 0x0063E6B8,
     0x0052EAAE, 0x00484DD1, 0x00484DF3},
    {"3.00", 0x07D3000B, 0x00030001, 0x00566D63,
     0x006190FD, 0x00619162, 0x00402482, 0x0053D170,
     0x005188B2, 0x0051CE80, 0x00517FA2, 0x005CB97F,
     0x005CB940, 0x0048BCF3, 0x00488954, 0x00518CBF,
     0x00518DCE, 0x006719E4, 0x0062CCE5, 0x00646BF8,
     0x00532D35, 0x00487853, 0x00487875},
    {"3.00 Steam", 0x07D3000B, 0x00030001, 0x005674DE,
     0x00619B3D, 0x00619BA2, 0x004021CE, 0x0053DA29,
     0x00519372, 0x0051D940, 0x00518A62, 0x005CC404,
     0x005CC3C5, 0x0048BF6E, 0x00488BB0, 0x0051977F,
     0x0051988E, 0x006721C4, 0x0062D6C5, 0x00647508,
     0x00533515, 0x00487AAD, 0x00487ACF},
    {"3.01a", 0x07D3000C, 0x000B0001, 0x005676DA,
     0x0061A6C5, 0x0061A72A, 0x00402444, 0x0053DAAC,
     0x005191B2, 0x0051D6E0, 0x005188A2, 0x005CCA34,
     0x005CC9F5, 0x0048BF35, 0x00488B9E, 0x005195BF,
     0x005196CE, 0x00672E84, 0x0062E345, 0x006482A8,
     0x005335EC, 0x00487AB9, 0x00487ADB},
    {"3.01b", 0x07D3000C, 0x000B0001, 0x00566E04,
     0x006191E3, 0x00619248, 0x0040218A, 0x0053D158,
     0x00518742, 0x0051CCA0, 0x00517E32, 0x005CB950,
     0x005CB911, 0x0048BB1E, 0x00488787, 0x00518B4F,
     0x00518C5E, 0x00671694, 0x0062CD85, 0x00646B28,
     0x00532B79, 0x0048768A, 0x004876AC},
};

struct PatchSites
{
    uintptr_t canvasModeSwitch;
    uintptr_t canvasModeContinue;
    uintptr_t wideImageBranch;
    uintptr_t wideImageNullBranch;
    uintptr_t allocationStride;
    uintptr_t largeAllocationBytes;
    uintptr_t largeAllocationCount;
    uintptr_t smallAllocationBytes;
    uintptr_t smallAllocationCount;
    uintptr_t strategicGridFirst;
    uintptr_t strategicGridSecond;
    uintptr_t strategicGridSkip1024;
    uintptr_t strategicGridSkip1280;
    uintptr_t strategicObjectBranch;
    uintptr_t surfaceStateCall;
    uintptr_t halfSizeDraw;
    uintptr_t halfSizeDrawContinue;
    uintptr_t originInit;
    uintptr_t offsetDrawFirst;
    uintptr_t offsetDrawSecond;
    uintptr_t widthLimitOperand1;
    uintptr_t heightLimitOperand1;
    uintptr_t widthLimitOperand2;
    uintptr_t heightLimitOperand2;
    uintptr_t heightLimitOperand3;
    uintptr_t widthLimitOperand3;
    uintptr_t battleClass;
    uintptr_t battleCenterBackground;
};

constexpr int kBaseWidth = 800;
constexpr int kBaseHeight = 600;
constexpr int kLimitWithoutPointerPatch = 1152;

struct CanvasPreset
{
    int width;
    int height;
};

// Explicit, bounded presets only. The legacy patch accepted arbitrary and much
// larger dimensions, but a free-form value can place the logical canvas below
// the game's base 800x600 UI or create an unreviewed allocation/layout case.
// 1066x600 is the compact lower bound: full native UI height with a ~16:9 view.
const CanvasPreset kCanvasPresets[] = {
    {1066, 600},
    {1152, 648},
    {1280, 720},
    {1366, 768},
    {1440, 810},
    {1536, 864},
    {1600, 900},
    {1820, 1024},
    {1920, 1080},
    {2560, 1440},
};

const CanvasPreset kNativeCanvasPresets[] = {
    {800, 600},
    {1024, 768},
    {1280, 1024},
};

struct RequestedCanvas
{
    int mode; // 0 = native DisplaySize, 1 = manual Hor+, 2 = monitor-adaptive family
    int width;
    int height;
    int nativeDisplaySize; // 0..2 for resolved native Auto, -1 for Hor+
    bool wide;
};

struct ProfileValueSnapshot
{
    bool present;
    char raw[256];
};

struct PatchPlan
{
    void* address;
    BYTE before[8];
    BYTE after[8];
    SIZE_T size;
};

struct ProtectedPage
{
    void* address;
    SIZE_T size;
    DWORD oldProtect;
};

PatchPlan g_plans[32] = {};
int g_planCount = 0;

volatile LONG g_installAttempted = 0;
volatile LONG g_available = 0;
volatile LONG g_active = 0;
volatile LONG g_activeCanvas[2] = {kBaseWidth, kBaseHeight};
volatile LONG g_zoomEnabled = 1;
volatile LONG g_surfaceAdjustActive = 0;
volatile LONG g_wideBattleSurface = 0;
PatchSites g_sites = {};
const AddressLayout* g_layout = nullptr;
uintptr_t g_imageEnd = kImageBase;
uintptr_t g_canvasModeContinue = 0;
uintptr_t g_halfSizeDrawContinue = 0;
DWORD g_wideBattleSurfaceVtable = 0;
bool g_battleCenterShared = false;

const char* discipleIni()
{
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
        char* slash = std::strrchr(path, '\\');
        if (slash)
            slash[1] = '\0';
        else
            path[0] = '\0';
        lstrcatA(path, "Disciple.ini");
    }
    return path;
}

bool readProfileInt(const char* section, const char* key, int* value)
{
    char raw[32] = {};
    GetPrivateProfileStringA(section, key, "\x01", raw,
                             static_cast<DWORD>(sizeof(raw)), discipleIni());
    if (raw[0] == '\x01' && raw[1] == '\0')
        return false;

    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    while (end && (*end == ' ' || *end == '\t'))
        ++end;
    if (!end || end == raw || *end || parsed < (std::numeric_limits<int>::min)() ||
        parsed > (std::numeric_limits<int>::max)())
        return false;
    if (value)
        *value = static_cast<int>(parsed);
    return true;
}

void nativeCanvas(int* width, int* height)
{
    int displaySize = 0;
    if (!readProfileInt("Disciple", "DisplaySize", &displaySize) ||
        displaySize < 0 || displaySize > 2)
        displaySize = 0;

    if (width)
        *width = kNativeCanvasPresets[displaySize].width;
    if (height)
        *height = kNativeCanvasPresets[displaySize].height;
}

bool supportedCustomCanvas(int width, int height)
{
    for (const CanvasPreset& preset : kCanvasPresets)
        if (preset.width == width && preset.height == height)
            return true;
    return false;
}

bool adaptiveCanvasForOutput(int outputWidth, int outputHeight,
                             int* width, int* height,
                             int* nativeDisplaySize)
{
    if (outputWidth <= 0 || outputHeight <= 0)
        return false;

    // 4:3 and 5:4 outputs use the game's three stock DisplaySize modes. 3:2 is
    // the deliberate boundary; 16:10, 16:9 and ultrawide outputs use Hor+.
    const bool wideOutput =
        static_cast<long long>(outputWidth) * 2 >=
        static_cast<long long>(outputHeight) * 3;
    const CanvasPreset* presets =
        wideOutput ? kCanvasPresets : kNativeCanvasPresets;
    const size_t presetCount = wideOutput
        ? sizeof(kCanvasPresets) / sizeof(kCanvasPresets[0])
        : sizeof(kNativeCanvasPresets) / sizeof(kNativeCanvasPresets[0]);

    const CanvasPreset* bestInteger = nullptr;
    const CanvasPreset* bestFit = nullptr;
    for (size_t i = 0; i < presetCount; ++i) {
        const CanvasPreset& preset = presets[i];
        if (preset.width > outputWidth || preset.height > outputHeight)
            continue;

        if (!bestFit ||
            static_cast<long long>(preset.width) * preset.height >
                static_cast<long long>(bestFit->width) * bestFit->height) {
            bestFit = &preset;
        }

        const int factorW = outputWidth / preset.width;
        const int factorH = outputHeight / preset.height;
        const int factor = factorW < factorH ? factorW : factorH;
        if (factor < 1)
            continue;

        // With aspect-preserving Fit, an integer scale is used only when the
        // scaled canvas reaches at least one output edge exactly. Extra space
        // on the other axis is a normal letter/pillar bar (ultrawide included).
        const bool exactInteger =
            preset.width * factor == outputWidth ||
            preset.height * factor == outputHeight;
        if (exactInteger &&
            (!bestInteger ||
             static_cast<long long>(preset.width) * preset.height >
                 static_cast<long long>(bestInteger->width) *
                     bestInteger->height)) {
            bestInteger = &preset;
        }
    }

    // Prefer the largest validated canvas with an exact integer Fit. If the
    // monitor has an unusual geometry, keep the largest canvas that fits and
    // let the selected shader handle the unavoidable fractional scale.
    const CanvasPreset* selected = bestInteger ? bestInteger : bestFit;
    if (!selected)
        selected = &presets[0]; // filtered down only on very small outputs
    if (width)
        *width = selected->width;
    if (height)
        *height = selected->height;
    if (nativeDisplaySize)
        *nativeDisplaySize = wideOutput
            ? -1
            : static_cast<int>(selected - kNativeCanvasPresets);
    return true;
}

bool primaryOutputPixels(int* width, int* height)
{
    // cnc-ddraw establishes per-monitor DPI awareness before installing this
    // feature, so GetSystemMetrics returns the physical primary-output pixels.
    const int outputWidth = GetSystemMetrics(SM_CXSCREEN);
    const int outputHeight = GetSystemMetrics(SM_CYSCREEN);
    if (outputWidth <= 0 || outputHeight <= 0)
        return false;
    if (width)
        *width = outputWidth;
    if (height)
        *height = outputHeight;
    return true;
}

bool readRequestedCanvas(RequestedCanvas* requested)
{
    if (!requested)
        return false;

    // A supported build defaults to monitor-adaptive selection only while
    // GameCanvasMode is absent. Explicit native/manual Hor+ modes remain authoritative.
    const bool widescreenAvailable =
        InterlockedExchangeAdd(&g_available, 0) != 0;
    requested->mode = widescreenAvailable ? 2 : 0;
    requested->nativeDisplaySize = -1;
    requested->wide = widescreenAvailable;
    if (widescreenAvailable) {
        int outputWidth = 0;
        int outputHeight = 0;
        if (!primaryOutputPixels(&outputWidth, &outputHeight) ||
            !adaptiveCanvasForOutput(outputWidth, outputHeight,
                                     &requested->width,
                                     &requested->height,
                                     &requested->nativeDisplaySize))
            return false;
        requested->wide = requested->nativeDisplaySize < 0;
    } else {
        nativeCanvas(&requested->width, &requested->height);
        int displaySize = 0;
        readProfileInt("Disciple", "DisplaySize", &displaySize);
        requested->nativeDisplaySize =
            displaySize >= 0 && displaySize <= 2 ? displaySize : 0;
        requested->wide = false;
    }

    int mode = 0;
    if (!readProfileInt("Wrapper", "GameCanvasMode", &mode))
        return true;
    if (mode != 0 && mode != 1 && mode != 2)
        return false;
    if (mode == 0) {
        // An explicitly stored zero is the user's request to use one of the
        // game's three original DisplaySize modes; never silently override it.
        requested->mode = 0;
        nativeCanvas(&requested->width, &requested->height);
        int displaySize = 0;
        readProfileInt("Disciple", "DisplaySize", &displaySize);
        requested->nativeDisplaySize =
            displaySize >= 0 && displaySize <= 2 ? displaySize : 0;
        requested->wide = false;
        return true;
    }
    if (mode == 2) {
        int outputWidth = 0;
        int outputHeight = 0;
        requested->mode = 2;
        const bool valid =
            primaryOutputPixels(&outputWidth, &outputHeight) &&
            adaptiveCanvasForOutput(outputWidth, outputHeight,
                                    &requested->width,
                                    &requested->height,
                                    &requested->nativeDisplaySize);
        requested->wide = valid && requested->nativeDisplaySize < 0;
        return valid;
    }
    requested->mode = 1;
    requested->nativeDisplaySize = -1;
    requested->wide = true;

    int width = 0;
    int height = 0;
    const bool haveWidth =
        readProfileInt("Wrapper", "GameCanvasWidth", &width);
    const bool haveHeight =
        readProfileInt("Wrapper", "GameCanvasHeight", &height);
    requested->width = width;
    requested->height = height;
    return haveWidth && haveHeight && supportedCustomCanvas(width, height);
}

void prepareLegacyZoomState()
{
    int enabled = 1;
    if (!readProfileInt("Disciple", "EnableZoom", &enabled))
        enabled = 1;

    InterlockedExchange(&g_zoomEnabled, enabled ? 1 : 0);
    InterlockedExchange(&g_surfaceAdjustActive, 0);
    InterlockedExchange(&g_wideBattleSurface, 0);
}

bool canvasAdjustmentActive()
{
    if (InterlockedExchangeAdd(&g_active, 0) == 0 ||
        InterlockedExchangeAdd(&g_zoomEnabled, 0) == 0 ||
        InterlockedExchangeAdd(&g_surfaceAdjustActive, 0) == 0)
        return false;

    // The legacy renderer suppresses the centered 800x600 adjustment only
    // for its special WideBattle surface while WideBattle is enabled but not
    // latched for the current battle.  Use the battle hook's latched state;
    // recomputing it here can disagree for a frame while a battle opens.
    return InterlockedExchangeAdd(&g_wideBattleSurface, 0) == 0 ||
           widebattle_canvas_hook_is_available() == 0 ||
           widebattle_get_enabled() == 0 ||
           widebattle_is_active() != 0;
}

bool initializeImageRange()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != kImageBase)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage == 0 ||
        nt->OptionalHeader.SizeOfImage >
            (std::numeric_limits<uintptr_t>::max)() - kImageBase)
        return false;

    g_imageEnd = kImageBase + nt->OptionalHeader.SizeOfImage;
    return g_imageEnd > kImageBase;
}

bool readableRange(uintptr_t address, SIZE_T size)
{
    if (!size || address < kImageBase || address >= g_imageEnd ||
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

bool bytesAre(uintptr_t address, const BYTE* expected, SIZE_T size)
{
    return readableRange(address, size) &&
           std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
}

bool readDword(uintptr_t address, DWORD* value)
{
    if (!value || !readableRange(address, sizeof(*value)))
        return false;
    std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
    return true;
}

bool executableProductVersion(DWORD* versionMS, DWORD* versionLS)
{
    if (!versionMS || !versionLS)
        return false;

    char path[MAX_PATH] = {};
    const DWORD pathLength =
        GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    if (!pathLength || pathLength >= sizeof(path))
        return false;

    DWORD ignored = 0;
    const DWORD dataSize = GetFileVersionInfoSizeA(path, &ignored);
    if (dataSize < sizeof(VS_FIXEDFILEINFO) || dataSize > 1024U * 1024U)
        return false;

    void* data = HeapAlloc(GetProcessHeap(), 0, dataSize);
    if (!data)
        return false;

    bool result = false;
    if (GetFileVersionInfoA(path, 0, dataSize, data)) {
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoSize = 0;
        if (VerQueryValueA(data, "\\", reinterpret_cast<void**>(&info),
                           &infoSize) &&
            info && infoSize >= sizeof(*info) &&
            info->dwSignature == VS_FFI_SIGNATURE) {
            // AddressSpaceV2 stores and compares ProductVersion.  The local
            // 3.01a executable happens to have equal file/product versions,
            // which would otherwise hide this distinction.
            *versionMS = info->dwProductVersionMS;
            *versionLS = info->dwProductVersionLS;
            result = true;
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
    return result;
}

bool deriveSites(const AddressLayout& layout, PatchSites* sites)
{
    if (!sites || !layout.check || !layout.resHook || !layout.resBack ||
        !layout.borderNop || !layout.borderHook || !layout.blitSize ||
        !layout.blitPatch1 || !layout.blitPatch2 || !layout.miniRectJump ||
        !layout.miniRectPatch || !layout.rightCurve || !layout.minimapFill ||
        !layout.maxSize1 || !layout.maxSize2 || !layout.maxSize3 ||
        !layout.battleClass || !layout.battleCenterBackground ||
        !layout.debugPosition || !layout.messageIconPosition ||
        !layout.messageTextPosition || layout.borderHook < kImageBase + 3)
        return false;

    const uintptr_t resolutionLength = layout.resBack - layout.resHook;
    if (layout.resBack <= layout.resHook ||
        (resolutionLength != 0x64 && resolutionLength != 0x65))
        return false;

    PatchSites derived = {};
    derived.canvasModeSwitch = layout.resHook;
    derived.canvasModeContinue = layout.resBack;
    derived.wideImageBranch = layout.borderNop;
    derived.wideImageNullBranch = layout.borderNop + 0x16;
    derived.allocationStride = layout.blitSize + 2;
    derived.largeAllocationBytes = layout.blitPatch1 + 1;
    derived.largeAllocationCount = layout.blitPatch1 + 0x0F;
    derived.smallAllocationBytes = layout.blitPatch2 + 1;
    derived.smallAllocationCount = layout.blitPatch2 + 0x1D;
    derived.strategicGridFirst = layout.miniRectPatch + 3;
    derived.strategicGridSecond = layout.miniRectPatch + 0x0B;
    derived.strategicGridSkip1024 = layout.miniRectJump;
    derived.strategicGridSkip1280 = layout.miniRectJump + 0x17;
    derived.strategicObjectBranch = layout.minimapFill;
    derived.surfaceStateCall = layout.borderHook - 3;
    derived.halfSizeDraw = layout.rightCurve;
    derived.halfSizeDrawContinue = layout.rightCurve + 18;
    derived.originInit = layout.debugPosition;
    derived.offsetDrawFirst = layout.messageIconPosition;
    derived.offsetDrawSecond = layout.messageTextPosition;
    derived.widthLimitOperand1 = layout.maxSize1 + 2;
    derived.heightLimitOperand1 = layout.maxSize1 + 29 + 2;
    derived.widthLimitOperand2 = layout.maxSize2 + 2;
    derived.heightLimitOperand2 = layout.maxSize2 + 28 + 2;
    derived.heightLimitOperand3 = layout.maxSize3 + 2;
    derived.widthLimitOperand3 = layout.maxSize3 + 6 + 2;
    derived.battleClass = layout.battleClass;
    derived.battleCenterBackground = layout.battleCenterBackground;

    *sites = derived;
    return true;
}

bool probeMatches(const AddressLayout& layout)
{
    // Original selection reads the DWORD following this PUSH and compares it
    // with WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX (0x00CA0000).
    static const BYTE windowStyleProbe[] = {0x68, 0x00, 0x00, 0xCA, 0x00};
    return bytesAre(layout.check, windowStyleProbe,
                    sizeof(windowStyleProbe));
}

bool selectAddressLayout(const AddressLayout** selected, PatchSites* sites)
{
    if (!selected || !sites)
        return false;

    DWORD versionMS = 0;
    DWORD versionLS = 0;
    if (!executableProductVersion(&versionMS, &versionLS))
        return false;

    const AddressLayout* match = nullptr;
    int matchCount = 0;
    for (const AddressLayout& layout : kAddressLayouts) {
        if (layout.productVersionMS == versionMS &&
            layout.productVersionLS == versionLS && probeMatches(layout)) {
            match = &layout;
            ++matchCount;
        }
    }
    if (matchCount != 1 || !deriveSites(*match, sites))
        return false;

    *selected = match;
    return true;
}

bool validateSites(const AddressLayout& layout, const PatchSites& sites,
                   bool battleCenterShared, DWORD* battleSurfaceVtable)
{
    static const BYTE canvasMode[] = {
        0x8B, 0x44, 0x24, 0x10, 0x3B, 0xC3, 0x75, 0x1E};
    static const BYTE wideImage[] = {0x7E, 0x5B};
    static const BYTE wideImageNull[] = {0x74};
    static const BYTE allocationStride[] = {0x40};
    static const BYTE largeAllocation[] = {
        0x68, 0x00, 0x00, 0x03, 0x00, 0x89, 0x7C, 0x24,
        0x14, 0x89, 0x5F, 0x04, 0xC7, 0x47, 0x08, 0x00,
        0x0C, 0x00, 0x00};
    static const BYTE smallAllocationStart[] = {
        0x68, 0x00, 0xF0, 0x00, 0x00, 0x0A, 0xD8, 0xC7, 0x05};
    static const BYTE smallAllocationMiddle[] = {
        0x00, 0x00, 0x00, 0x00, 0x88, 0x1D};
    static const BYTE smallAllocationStore[] = {0xC7, 0x05};
    static const BYTE smallAllocationCountValue[] = {
        0x00, 0x0C, 0x00, 0x00};
    static const BYTE strategicGrid[] = {
        0xC7, 0x45, 0xE8, 0x1B, 0x00, 0x00, 0x00, 0x50,
        0xC7, 0x45, 0xEC, 0x07, 0x00, 0x00, 0x00};
    static const BYTE skip1024[] = {0x75, 0x0E};
    static const BYTE skip1280[] = {0x75, 0x0E};
    static const BYTE strategicObject[] = {
        0x0F, 0x8E, 0x96, 0x00, 0x00, 0x00};
    static const BYTE surfaceState[] = {
        0xFF, 0x50, 0x24, 0x84, 0xC0};
    static const BYTE halfSizeDraw[] = {
        0x39, 0x5E, 0x64, 0x75, 0x0D};
    static const BYTE originInit[] = {
        0x89, 0x4D, 0xF8, 0x89, 0x4D, 0xFC};
    static const BYTE offsetDraw[] = {
        0x8B, 0x01, 0xFF, 0x50, 0x14};
    static const BYTE widthLimit1[] = {0x3B, 0x05};
    static const BYTE widthLimit2[] = {0x3B, 0x0D};
    static const BYTE heightLimit2[] = {0x3B, 0x3D};
    static const BYTE limitLoadHeight[] = {0x8B, 0x0D};
    static const BYTE limitLoadWidth[] = {0x8B, 0x15};
    static const BYTE battleClassMarker[] = {0xC7, 0x02};
    static const BYTE battleCenterBranch[] = {0x74, 0x20};
    static const BYTE battleCenterMath[] = {
        0x2B, 0x30, 0x89, 0x75, 0xF8};

    DWORD vtable = 0;
    DWORD firstVirtualMethod = 0;
    const bool battleClassValid =
        bytesAre(sites.battleClass, battleClassMarker,
                 sizeof(battleClassMarker)) &&
        readDword(sites.battleClass + 2, &vtable) &&
        readDword(vtable, &firstVirtualMethod) &&
        vtable >= kImageBase && vtable < g_imageEnd &&
        firstVirtualMethod >= kImageBase && firstVirtualMethod < g_imageEnd;
    const bool battleCenterValid = battleCenterShared ||
        (bytesAre(sites.battleCenterBackground, battleCenterBranch,
                  sizeof(battleCenterBranch)) &&
         bytesAre(sites.battleCenterBackground + 27, battleCenterMath,
                  sizeof(battleCenterMath)));

    const bool valid =
           probeMatches(layout) &&
           bytesAre(sites.canvasModeSwitch, canvasMode, sizeof(canvasMode)) &&
           bytesAre(sites.wideImageBranch, wideImage, sizeof(wideImage)) &&
           bytesAre(sites.wideImageNullBranch, wideImageNull,
                    sizeof(wideImageNull)) &&
           bytesAre(sites.allocationStride, allocationStride,
                    sizeof(allocationStride)) &&
           bytesAre(sites.largeAllocationBytes - 1, largeAllocation,
                    sizeof(largeAllocation)) &&
           bytesAre(sites.smallAllocationBytes - 1, smallAllocationStart,
                    sizeof(smallAllocationStart)) &&
           bytesAre(sites.smallAllocationBytes + 12,
                    smallAllocationMiddle,
                    sizeof(smallAllocationMiddle)) &&
           bytesAre(sites.smallAllocationBytes + 22,
                    smallAllocationStore,
                    sizeof(smallAllocationStore)) &&
           bytesAre(sites.smallAllocationBytes + 28,
                    smallAllocationCountValue,
                    sizeof(smallAllocationCountValue)) &&
           bytesAre(sites.strategicGridFirst - 3, strategicGrid,
                    sizeof(strategicGrid)) &&
           bytesAre(sites.strategicGridSkip1024, skip1024,
                    sizeof(skip1024)) &&
           bytesAre(sites.strategicGridSkip1280, skip1280,
                    sizeof(skip1280)) &&
           bytesAre(sites.strategicObjectBranch, strategicObject,
                     sizeof(strategicObject)) &&
           bytesAre(sites.surfaceStateCall, surfaceState,
                    sizeof(surfaceState)) &&
           bytesAre(sites.halfSizeDraw, halfSizeDraw,
                    sizeof(halfSizeDraw)) &&
           bytesAre(sites.originInit, originInit, sizeof(originInit)) &&
           bytesAre(sites.offsetDrawFirst, offsetDraw,
                    sizeof(offsetDraw)) &&
           bytesAre(sites.offsetDrawSecond, offsetDraw,
                    sizeof(offsetDraw)) &&
           bytesAre(sites.widthLimitOperand1 - 2, widthLimit1,
                     sizeof(widthLimit1)) &&
           bytesAre(sites.heightLimitOperand1 - 2, widthLimit1,
                    sizeof(widthLimit1)) &&
           bytesAre(sites.widthLimitOperand2 - 2, widthLimit2,
                     sizeof(widthLimit2)) &&
           bytesAre(sites.heightLimitOperand2 - 2, heightLimit2,
                    sizeof(heightLimit2)) &&
           bytesAre(sites.heightLimitOperand3 - 2, limitLoadHeight,
                    sizeof(limitLoadHeight)) &&
           bytesAre(sites.widthLimitOperand3 - 2, limitLoadWidth,
                    sizeof(limitLoadWidth)) &&
           battleClassValid && battleCenterValid;

    if (valid && battleSurfaceVtable)
        *battleSurfaceVtable = vtable;
    return valid;
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

template <typename T>
bool addValuePatch(uintptr_t address, const T& replacement)
{
    return addPatch(reinterpret_cast<void*>(address), &replacement,
                    sizeof(replacement));
}

bool addNops(uintptr_t address, SIZE_T size)
{
    BYTE bytes[8] = {};
    if (size > sizeof(bytes))
        return false;
    std::memset(bytes, 0x90, size);
    return addPatch(reinterpret_cast<void*>(address), bytes, size);
}

bool makeRel32(uintptr_t address, const void* target, BYTE opcode, BYTE* out,
               SIZE_T size)
{
    if (size < 5 || size > 8)
        return false;
    const std::intptr_t delta =
        reinterpret_cast<std::intptr_t>(target) -
        static_cast<std::intptr_t>(address + 5);
    if (delta < (std::numeric_limits<std::int32_t>::min)() ||
        delta > (std::numeric_limits<std::int32_t>::max)())
        return false;

    out[0] = opcode;
    const std::int32_t relative = static_cast<std::int32_t>(delta);
    std::memcpy(out + 1, &relative, sizeof(relative));
    if (size > 5)
        std::memset(out + 5, 0x90, size - 5);
    return true;
}

bool addRelPatch(uintptr_t address, const void* target, BYTE opcode, SIZE_T size)
{
    BYTE bytes[8] = {};
    if (!makeRel32(address, target, opcode, bytes, size))
        return false;
    return addPatch(reinterpret_cast<void*>(address), bytes, size);
}

bool restorePages(ProtectedPage* pages, int count)
{
    bool restored = true;
    while (count-- > 0) {
        DWORD ignored = 0;
        if (!VirtualProtect(pages[count].address, pages[count].size,
                            pages[count].oldProtect, &ignored))
            restored = false;
    }
    return restored;
}

bool protectAllPlanPages(ProtectedPage* pages, int capacity, int* pageCount)
{
    if (!pages || capacity <= 0 || !pageCount)
        return false;
    *pageCount = 0;

    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const uintptr_t pageSize =
        static_cast<uintptr_t>(systemInfo.dwPageSize);
    if (!pageSize)
        return false;

    // First enumerate every page without changing protection.  No memcpy is
    // allowed until every target page has passed VirtualQuery and
    // VirtualProtect.
    for (int i = 0; i < g_planCount; ++i) {
        const uintptr_t first =
            reinterpret_cast<uintptr_t>(g_plans[i].address);
        const uintptr_t last = first + g_plans[i].size - 1;
        uintptr_t page = first - first % pageSize;
        const uintptr_t lastPage = last - last % pageSize;
        for (;;) {
            bool present = false;
            for (int j = 0; j < *pageCount; ++j) {
                if (reinterpret_cast<uintptr_t>(pages[j].address) == page) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                if (*pageCount >= capacity)
                    return false;
                MEMORY_BASIC_INFORMATION mbi = {};
                if (!VirtualQuery(reinterpret_cast<const void*>(page), &mbi,
                                  sizeof(mbi)) ||
                    mbi.State != MEM_COMMIT ||
                    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    return false;
                pages[*pageCount].address = reinterpret_cast<void*>(page);
                pages[*pageCount].size = static_cast<SIZE_T>(pageSize);
                pages[*pageCount].oldProtect = 0;
                ++*pageCount;
            }
            if (page == lastPage)
                break;
            page += pageSize;
        }
    }

    int protectedCount = 0;
    for (; protectedCount < *pageCount; ++protectedCount) {
        if (!VirtualProtect(pages[protectedCount].address,
                            pages[protectedCount].size,
                            PAGE_EXECUTE_READWRITE,
                            &pages[protectedCount].oldProtect)) {
            restorePages(pages, protectedCount);
            return false;
        }
    }
    return true;
}

bool applyPlans()
{
    ProtectedPage pages[64] = {};
    int pageCount = 0;
    if (!protectAllPlanPages(
            pages, static_cast<int>(sizeof(pages) / sizeof(pages[0])),
            &pageCount))
        return false;

    // Recheck the complete snapshot after the protection preflight.  This
    // catches a competing patcher without ever committing a partial plan.
    for (int i = 0; i < g_planCount; ++i) {
        if (std::memcmp(g_plans[i].address, g_plans[i].before,
                        g_plans[i].size) != 0) {
            restorePages(pages, pageCount);
            return false;
        }
    }

    for (int i = 0; i < g_planCount; ++i)
        std::memcpy(g_plans[i].address, g_plans[i].after,
                    g_plans[i].size);

    bool flushed = true;
    for (int i = 0; i < g_planCount; ++i) {
        if (!FlushInstructionCache(GetCurrentProcess(),
                                   g_plans[i].address,
                                   g_plans[i].size))
            flushed = false;
    }
    if (!flushed) {
        for (int i = g_planCount; i-- > 0;)
            std::memcpy(g_plans[i].address, g_plans[i].before,
                        g_plans[i].size);
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        restorePages(pages, pageCount);
        return false;
    }

    // At this point every byte is committed.  A protection-restore failure
    // must not report the feature as inactive while its branches are live;
    // keep the coherent byte set and surface the exceptional page-state issue.
    if (!restorePages(pages, pageCount))
        OutputDebugStringA(
            "C4dll-R: Hor+ warning: game bytes committed but a page protection could not be restored\n");
    return true;
}

void __declspec(naked) canvasModeHook()
{
    __asm {
        mov eax, [esi]
        mov ecx, dword ptr [g_activeCanvas]
        mov [eax+198h], ecx
        mov ecx, dword ptr [g_activeCanvas+4]
        mov [eax+19Ch], ecx
        jmp dword ptr [g_canvasModeContinue]
    }
}

void __stdcall recordSurfaceState(const DWORD* object, DWORD result)
{
    const LONG oldActive = InterlockedExchange(
        &g_surfaceAdjustActive, (result & 1U) != 0 ? 1 : 0);
    LONG oldWide = InterlockedExchangeAdd(&g_wideBattleSurface, 0);

    // The legacy wrapper deliberately retains the previous surface kind while
    // this sentinel is present.
    if (object && object[2] != 0xFFFFFFFFU) {
        oldWide = InterlockedExchange(
            &g_wideBattleSurface,
            object[0] == g_wideBattleSurfaceVtable ? 1 : 0);
    }

    if (oldActive != InterlockedExchangeAdd(&g_surfaceAdjustActive, 0) ||
        oldWide != InterlockedExchangeAdd(&g_wideBattleSurface, 0))
        DDInvalidateDecorativeFrame();
}

void __declspec(naked) surfaceStateHook()
{
    __asm {
        call dword ptr [eax+24h]
        push eax
        push edi
        call recordSurfaceState
        xor eax, eax
        ret
    }
}

void __declspec(naked) halfSizeDrawHook()
{
    __asm {
        cmp [esi+68h], ebx
        je draw
        cmp dword ptr [g_surfaceAdjustActive], ebx
        je done

    draw:
        mov eax, [ecx]
        add esi, 64h
        mov edx, [esi+4]
        cmp dword ptr [g_surfaceAdjustActive], ebx
        je pushHeight
        shr edx, 1

    pushHeight:
        push edx
        mov edx, [esi]
        cmp dword ptr [g_surfaceAdjustActive], ebx
        je pushWidth
        shr edx, 1

    pushWidth:
        push edx
        mov esi, esp
        push ebx
        push ebx
        push ebx
        push esi
        push edi
        call dword ptr [eax+14h]
        add esp, 8

    done:
        jmp dword ptr [g_halfSizeDrawContinue]
    }
}

void __fastcall computeLegacyOrigin(LONG* point)
{
    point[0] = 0;
    point[1] = 0;
    if (!canvasAdjustmentActive())
        return;

    point[0] =
        (InterlockedExchangeAdd(&g_activeCanvas[0], 0) - kBaseWidth) / 2;
    point[1] =
        (InterlockedExchangeAdd(&g_activeCanvas[1], 0) - kBaseHeight) / 2;
}

void __declspec(naked) originInitHook()
{
    __asm {
        push eax
        lea ecx, [ebp-8]
        call computeLegacyOrigin
        pop eax
        xor ecx, ecx
        ret
    }
}

void __fastcall adjustLegacyDrawOrigin(const BYTE* owner, LONG* point)
{
    if (!canvasAdjustmentActive())
        return;

    const LONG horizontal =
        (InterlockedExchangeAdd(&g_activeCanvas[0], 0) - kBaseWidth) / 2;
    const LONG vertical =
        (InterlockedExchangeAdd(&g_activeCanvas[1], 0) - kBaseHeight) / 2;
    point[0] += horizontal;
    if (owner[96])
        point[1] += vertical;
    else
        point[1] -= vertical;
}

void __declspec(naked) offsetDrawHook()
{
    __asm {
        push ecx
        mov edx, [esp+0Ch]
        mov ecx, esi
        call adjustLegacyDrawOrigin
        pop ecx
        mov eax, [ecx]
        jmp dword ptr [eax+14h]
    }
}

// WideBattle normally owns this shared call site.  If its larger patch set is
// unavailable for an otherwise valid game layout, Hor+ still has enough
// information to install only the custom-canvas centering path here.
void __declspec(naked) battleCenterHook()
{
    __asm {
        push dword ptr [eax]
        sub esi, [eax]
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

int allocationEntryCount(int width, int height)
{
    // Preserve the legacy implementation's integer->float conversion followed
    // by x87 arithmetic and truncation toward zero.
    const double widthF =
        static_cast<float>(static_cast<unsigned>(width));
    const double heightF =
        static_cast<float>(static_cast<unsigned>(height));
    const double entries =
        heightF * (widthF * 2048.0) /
        (static_cast<double>(static_cast<float>(kBaseWidth)) *
         static_cast<double>(static_cast<float>(kBaseHeight)));
    return static_cast<int>(entries);
}

int roundHalfUp(float value)
{
    // The legacy helper used floor(x), selecting ceil(x) at and above .5.
    return static_cast<int>(std::floor(static_cast<double>(value) + 0.5));
}

void strategicGrid(int width, int height, int* first, int* second)
{
    // The original x87 code stores each of these three stages back to a float.
    const float horizontal = static_cast<float>(
        (20.0 / static_cast<float>(kBaseWidth - 160)) *
        static_cast<float>(width - 160));
    const float vertical = static_cast<float>(
        (34.0 / static_cast<float>(kBaseHeight)) *
        static_cast<float>(static_cast<unsigned>(height)));
    const float offset = static_cast<float>(
        (static_cast<double>(vertical) - horizontal) * 0.5);
    if (first)
        *first = roundHalfUp(horizontal + offset);
    if (second)
        *second = roundHalfUp(offset);
}

bool preparePlans(int width, int height)
{
    g_planCount = 0;

    const int count = allocationEntryCount(width, height);
    const int largeBytes = count * 0x40;
    const int smallBytes = count * 20;
    int gridFirst = 0;
    int gridSecond = 0;
    strategicGrid(width, height, &gridFirst, &gridSecond);

    const BYTE unconditionalJump = 0xEB;
    const std::uint32_t widthAddress =
        static_cast<std::uint32_t>(reinterpret_cast<uintptr_t>(&g_activeCanvas[0]));
    const std::uint32_t heightAddress =
        static_cast<std::uint32_t>(reinterpret_cast<uintptr_t>(&g_activeCanvas[1]));

    if (count <= 0 || largeBytes <= 0 || smallBytes <= 0 ||
        !addRelPatch(g_sites.canvasModeSwitch,
                     reinterpret_cast<const void*>(&canvasModeHook), 0xE9, 5) ||
        !addNops(g_sites.wideImageBranch, 2) ||
        !addValuePatch(g_sites.wideImageNullBranch, unconditionalJump) ||
        !addValuePatch(g_sites.largeAllocationBytes, largeBytes) ||
        !addValuePatch(g_sites.largeAllocationCount, count) ||
        !addValuePatch(g_sites.smallAllocationBytes, smallBytes) ||
        !addValuePatch(g_sites.smallAllocationCount, count) ||
        !addValuePatch(g_sites.strategicGridFirst, gridFirst) ||
        !addValuePatch(g_sites.strategicGridSecond, gridSecond) ||
        !addValuePatch(g_sites.strategicGridSkip1024, unconditionalJump) ||
        !addValuePatch(g_sites.strategicGridSkip1280, unconditionalJump) ||
        !addNops(g_sites.strategicObjectBranch, 6) ||
        !addRelPatch(g_sites.surfaceStateCall,
                     reinterpret_cast<const void*>(&surfaceStateHook),
                     0xE8, 5) ||
        !addRelPatch(g_sites.halfSizeDraw,
                     reinterpret_cast<const void*>(&halfSizeDrawHook),
                     0xE9, 5) ||
        !addRelPatch(g_sites.originInit,
                     reinterpret_cast<const void*>(&originInitHook),
                     0xE8, 6) ||
        !addRelPatch(g_sites.offsetDrawFirst,
                     reinterpret_cast<const void*>(&offsetDrawHook),
                     0xE8, 5) ||
        !addRelPatch(g_sites.offsetDrawSecond,
                     reinterpret_cast<const void*>(&offsetDrawHook),
                     0xE8, 5))
        return false;

    if (!g_battleCenterShared &&
        (!addNops(g_sites.battleCenterBackground, 2) ||
         !addRelPatch(g_sites.battleCenterBackground + 27,
                      reinterpret_cast<const void*>(&battleCenterHook),
                      0xE8, 5)))
        return false;

    if (width > kLimitWithoutPointerPatch &&
        (!addValuePatch(g_sites.widthLimitOperand1, widthAddress) ||
         !addValuePatch(g_sites.widthLimitOperand2, widthAddress) ||
         !addValuePatch(g_sites.widthLimitOperand3, widthAddress)))
        return false;

    if (height > kLimitWithoutPointerPatch &&
        (!addValuePatch(g_sites.heightLimitOperand1, heightAddress) ||
         !addValuePatch(g_sites.heightLimitOperand2, heightAddress) ||
         !addValuePatch(g_sites.heightLimitOperand3, heightAddress)))
        return false;

    return true;
}

bool writeProfileIntInSection(const char* section, const char* key, int value)
{
    char raw[16] = {};
    wsprintfA(raw, "%d", value);
    return WritePrivateProfileStringA(section, key, raw, discipleIni()) != FALSE;
}

bool writeProfileInt(const char* key, int value)
{
    return writeProfileIntInSection("Wrapper", key, value);
}

ProfileValueSnapshot snapshotProfileValueInSection(const char* section,
                                                   const char* key)
{
    ProfileValueSnapshot snapshot = {};
    GetPrivateProfileStringA(section, key, "\x01", snapshot.raw,
                             static_cast<DWORD>(sizeof(snapshot.raw)),
                             discipleIni());
    snapshot.present = !(snapshot.raw[0] == '\x01' && snapshot.raw[1] == '\0');
    if (!snapshot.present)
        snapshot.raw[0] = '\0';
    return snapshot;
}

ProfileValueSnapshot snapshotProfileValue(const char* key)
{
    return snapshotProfileValueInSection("Wrapper", key);
}

bool restoreProfileValueInSection(const char* section, const char* key,
                                  const ProfileValueSnapshot& snapshot)
{
    return WritePrivateProfileStringA(
               section, key, snapshot.present ? snapshot.raw : nullptr,
               discipleIni()) != FALSE;
}

bool restoreProfileValue(const char* key,
                         const ProfileValueSnapshot& snapshot)
{
    return restoreProfileValueInSection("Wrapper", key, snapshot);
}

bool restoreRequestedCanvas(const ProfileValueSnapshot& mode,
                            const ProfileValueSnapshot& width,
                            const ProfileValueSnapshot& height)
{
    // Keep Hor+ disabled while restoring the dimensions, then restore the old
    // mode last. This prevents a failed save from briefly activating a mixed
    // old/new width-height pair on the next process start.
    bool restored = writeProfileInt("GameCanvasMode", 0);
    bool dimensionsRestored = true;
    if (!restoreProfileValue("GameCanvasWidth", width))
        dimensionsRestored = false;
    if (!restoreProfileValue("GameCanvasHeight", height))
        dimensionsRestored = false;

    if (dimensionsRestored) {
        if (!restoreProfileValue("GameCanvasMode", mode))
            restored = false;
    } else {
        // Never reactivate an old mode=1 around a dimension that failed to
        // restore. Keep the safe native fallback selected even if doing so
        // means the exact previous request could not be recovered.
        writeProfileInt("GameCanvasMode", 0);
        restored = false;
    }
    return restored;
}

} // namespace

extern "C" void horplus_install(void)
{
    if (InterlockedCompareExchange(&g_installAttempted, 1, 0) != 0)
        return;

    int nativeWidth = kBaseWidth;
    int nativeHeight = kBaseHeight;
    nativeCanvas(&nativeWidth, &nativeHeight);
    InterlockedExchange(&g_activeCanvas[0], nativeWidth);
    InterlockedExchange(&g_activeCanvas[1], nativeHeight);

    PatchSites selectedSites = {};
    const AddressLayout* selectedLayout = nullptr;
    if (!initializeImageRange() ||
        !selectAddressLayout(&selectedLayout, &selectedSites)) {
        OutputDebugStringA(
            "C4dll-R: Hor+ unavailable (unsupported ProductVersion/probe)\n");
        return;
    }

    const bool sharedBattleCenter =
        widebattle_canvas_hook_is_available() != 0;
    DWORD battleSurfaceVtable = 0;
    if (!validateSites(*selectedLayout, selectedSites, sharedBattleCenter,
                       &battleSurfaceVtable)) {
        OutputDebugStringA(
            "C4dll-R: Hor+ unavailable (selected-layout signatures do not match)\n");
        return;
    }

    g_layout = selectedLayout;
    g_sites = selectedSites;
    g_canvasModeContinue = selectedSites.canvasModeContinue;
    g_halfSizeDrawContinue = selectedSites.halfSizeDrawContinue;
    g_wideBattleSurfaceVtable = battleSurfaceVtable;
    g_battleCenterShared = sharedBattleCenter;
    InterlockedExchange(&g_available, 1);

    RequestedCanvas requested = {};
    if (!readRequestedCanvas(&requested)) {
        OutputDebugStringA(
            "C4dll-R: Hor+ request ignored (invalid GameCanvasMode/Width/Height)\n");
        return;
    }
    if (!requested.wide) {
        // Automatic mode deliberately reuses the game's own DisplaySize path
        // on 4:3/5:4 outputs. Persist the resolved stock index before the game
        // reads Disciple.ini; no executable patch is needed for that branch.
        if (requested.mode == 2 && requested.nativeDisplaySize >= 0 &&
            !writeProfileIntInSection("Disciple", "DisplaySize",
                                      requested.nativeDisplaySize)) {
            OutputDebugStringA(
                "C4dll-R: adaptive native DisplaySize could not be saved\n");
            return;
        }
        InterlockedExchange(&g_activeCanvas[0], requested.width);
        InterlockedExchange(&g_activeCanvas[1], requested.height);
        // fake_mode is only a virtual 16-bit desktop/mode advertisement.  It
        // must follow the selected native canvas too (not just Hor+), otherwise
        // DisplaySize 0/2 can be mixed with the persisted 1024x768 fallback.
        DDSetGameCanvasMetrics(requested.width, requested.height, 0);
        char message[160] = {};
        wsprintfA(message,
                  "C4dll-R: native DisplaySize=%d canvas active at %dx%d (%s); Hor+ wrote no game bytes\n",
                  requested.nativeDisplaySize, requested.width,
                  requested.height,
                  requested.mode == 2 ? "monitor-adaptive" : "manual");
        OutputDebugStringA(message);
        return;
    }

    // DisplaySize is more than the stock canvas selector: code outside the
    // patched size switch still uses it to choose native layout/resources.
    // The original Hor+ implementation replaces the DisplaySize=0 path and
    // uses its 800x600 layout origin while the requested width/height become
    // the real DirectDraw canvas. Keep that compatibility selector stable;
    // selecting a larger stock mode underneath Hor+ would not increase the
    // canvas and would mix an unpatched stock layout with the wide hooks.
    const ProfileValueSnapshot storedDisplaySize =
        snapshotProfileValueInSection("Disciple", "DisplaySize");
    int compatibilityDisplaySize = 0;
    const bool validCompatibilityDisplaySize =
        readProfileInt("Disciple", "DisplaySize", &compatibilityDisplaySize);
    if (storedDisplaySize.present &&
        (!validCompatibilityDisplaySize || compatibilityDisplaySize != 0) &&
        !writeProfileIntInSection("Disciple", "DisplaySize", 0)) {
        OutputDebugStringA(
            "C4dll-R: Hor+ disabled (could not normalize DisplaySize=0)\n");
        return;
    }

    InterlockedExchange(&g_activeCanvas[0], requested.width);
    InterlockedExchange(&g_activeCanvas[1], requested.height);
    prepareLegacyZoomState();
    if (!preparePlans(requested.width, requested.height) || !applyPlans()) {
        InterlockedExchange(&g_activeCanvas[0], nativeWidth);
        InterlockedExchange(&g_activeCanvas[1], nativeHeight);
        InterlockedExchange(&g_surfaceAdjustActive, 0);
        InterlockedExchange(&g_wideBattleSurface, 0);
        InterlockedExchange(&g_available, 0);
        OutputDebugStringA(
            "C4dll-R: Hor+ disabled (could not apply the complete patch transaction)\n");
        return;
    }

    // Match both the fake desktop and cnc-ddraw's single injected mode to the
    // validated canvas. The game must find the exact Hor+ dimensions during
    // EnumDisplayModes or it exits before creating DirectDraw surfaces.
    DDSetGameCanvasMetrics(requested.width, requested.height, 1);
    InterlockedExchange(&g_active, 1);
    char message[160] = {};
    wsprintfA(message,
              "C4dll-R: true Hor+ %s game canvas active at %dx%d (%s, restart-only)\n",
              g_layout->name, requested.width, requested.height,
              requested.mode == 2 ? "monitor-adaptive" : "manual");
    OutputDebugStringA(message);
}

extern "C" int horplus_is_available(void)
{
    return InterlockedExchangeAdd(&g_available, 0) != 0;
}

extern "C" int horplus_is_active(void)
{
    return InterlockedExchangeAdd(&g_active, 0) != 0;
}

extern "C" int horplus_get_active_size(int* width, int* height)
{
    if (width)
        *width = InterlockedExchangeAdd(&g_activeCanvas[0], 0);
    if (height)
        *height = InterlockedExchangeAdd(&g_activeCanvas[1], 0);
    return InterlockedExchangeAdd(&g_installAttempted, 0) != 0;
}

extern "C" int horplus_get_decor_layout(int* contentWidth,
                                          int* contentHeight,
                                          int* wideBattle)
{
    if (!canvasAdjustmentActive())
        return 0;

    const bool wide =
        InterlockedExchangeAdd(&g_wideBattleSurface, 0) != 0 &&
        widebattle_canvas_hook_is_available() != 0 &&
        widebattle_get_enabled() != 0 && widebattle_is_active() != 0;
    if (contentWidth)
        *contentWidth = wide ? 990 : kBaseWidth;
    if (contentHeight)
        *contentHeight = kBaseHeight;
    if (wideBattle)
        *wideBattle = wide ? 1 : 0;
    return 1;
}

extern "C" int horplus_get_requested(int* mode, int* width, int* height)
{
    RequestedCanvas requested = {};
    const bool valid = readRequestedCanvas(&requested);
    if (mode)
        *mode = requested.mode;
    if (width)
        *width = requested.width;
    if (height)
        *height = requested.height;
    return valid ? 1 : 0;
}

extern "C" int horplus_get_adaptive_for_output(int outputWidth,
                                                   int outputHeight,
                                                   int* width, int* height,
                                                   int* nativeDisplaySize)
{
    return adaptiveCanvasForOutput(outputWidth, outputHeight, width, height,
                                   nativeDisplaySize)
        ? 1
        : 0;
}

extern "C" int horplus_get_primary_adaptive(int* outputWidth,
                                                int* outputHeight,
                                                int* width, int* height,
                                                int* nativeDisplaySize)
{
    int monitorWidth = 0;
    int monitorHeight = 0;
    if (!primaryOutputPixels(&monitorWidth, &monitorHeight) ||
        !adaptiveCanvasForOutput(monitorWidth, monitorHeight, width, height,
                                 nativeDisplaySize))
        return 0;
    if (outputWidth)
        *outputWidth = monitorWidth;
    if (outputHeight)
        *outputHeight = monitorHeight;
    return 1;
}

extern "C" int horplus_set_requested(int mode, int width, int height)
{
    if (mode != 0 && mode != 1 && mode != 2)
        return 0;
    if (mode == 1 && !supportedCustomCanvas(width, height))
        return 0;

    if (mode == 0) {
        if (!writeProfileInt("GameCanvasMode", 0))
            return 0;
        WritePrivateProfileStringA("Wrapper", "LegacyDisplaySizeMigrated", "1",
                                   discipleIni());
        return 1;
    }

    if (mode == 2) {
        int outputWidth = 0;
        int outputHeight = 0;
        int nativeDisplaySize = -1;
        if (!primaryOutputPixels(&outputWidth, &outputHeight) ||
            !adaptiveCanvasForOutput(outputWidth, outputHeight,
                                     nullptr, nullptr,
                                     &nativeDisplaySize))
            return 0;

        const ProfileValueSnapshot oldDisplaySize =
            snapshotProfileValueInSection("Disciple", "DisplaySize");
        const ProfileValueSnapshot oldMode =
            snapshotProfileValue("GameCanvasMode");
        const ProfileValueSnapshot oldMigration =
            snapshotProfileValue("LegacyDisplaySizeMigrated");
        // A wide adaptive canvas uses stock layout 0 as its compatibility
        // base. A 4:3/5:4 output keeps the actual native DisplaySize selected
        // by adaptiveCanvasForOutput().
        const int compatibilityDisplaySize =
            nativeDisplaySize >= 0 ? nativeDisplaySize : 0;
        if (!writeProfileIntInSection("Disciple", "DisplaySize",
                                      compatibilityDisplaySize) ||
            !writeProfileInt("GameCanvasMode", 2) ||
            !writeProfileInt("LegacyDisplaySizeMigrated", 1)) {
            bool restored = restoreProfileValueInSection(
                "Disciple", "DisplaySize", oldDisplaySize);
            if (!restoreProfileValue("GameCanvasMode", oldMode))
                restored = false;
            if (!restoreProfileValue("LegacyDisplaySizeMigrated", oldMigration))
                restored = false;
            if (!restored)
                OutputDebugStringA(
                    "C4dll-R: could not fully roll back a failed adaptive-canvas setting write\n");
            return 0;
        }
        return 1;
    }

    const ProfileValueSnapshot oldDisplaySize =
        snapshotProfileValueInSection("Disciple", "DisplaySize");
    const ProfileValueSnapshot oldMode =
        snapshotProfileValue("GameCanvasMode");
    const ProfileValueSnapshot oldWidth =
        snapshotProfileValue("GameCanvasWidth");
    const ProfileValueSnapshot oldHeight =
        snapshotProfileValue("GameCanvasHeight");

    // Disable first so an interrupted multi-key update can only fall back to
    // native DisplaySize, never start with a mixed pair. If a normal API write
    // fails, restore the exact previous strings before reporting failure.
    if (!writeProfileInt("GameCanvasMode", 0) ||
        !writeProfileIntInSection("Disciple", "DisplaySize", 0) ||
        !writeProfileInt("GameCanvasWidth", width) ||
        !writeProfileInt("GameCanvasHeight", height) ||
        !writeProfileInt("GameCanvasMode", 1)) {
        bool restored = restoreRequestedCanvas(oldMode, oldWidth, oldHeight);
        if (!restoreProfileValueInSection("Disciple", "DisplaySize",
                                          oldDisplaySize))
            restored = false;
        if (!restored)
            OutputDebugStringA(
                "C4dll-R: could not fully roll back a failed Hor+ setting write\n");
        return 0;
    }

    // A choice through the new menu is explicit.  Keep obsolete legacy keys
    // intact for rollback, but prevent the one-time native migration from
    // interpreting them as the active source of truth.
    WritePrivateProfileStringA("Wrapper", "LegacyDisplaySizeMigrated", "1",
                               discipleIni());
    return 1;
}

extern "C" int horplus_set_native_requested(int displaySize)
{
    if (displaySize < 0 || displaySize > 2)
        return 0;

    const ProfileValueSnapshot oldDisplaySize =
        snapshotProfileValueInSection("Disciple", "DisplaySize");
    const ProfileValueSnapshot oldMode =
        snapshotProfileValue("GameCanvasMode");
    const ProfileValueSnapshot oldMigration =
        snapshotProfileValue("LegacyDisplaySizeMigrated");

    // DisplaySize is written first while the previous mode is still in force.
    // An interrupted write therefore either keeps the old wide request or has a
    // complete native pair; it can never activate a half-written wide canvas.
    if (!writeProfileIntInSection("Disciple", "DisplaySize", displaySize) ||
        !writeProfileInt("GameCanvasMode", 0) ||
        !writeProfileInt("LegacyDisplaySizeMigrated", 1)) {
        bool restored =
            restoreProfileValueInSection("Disciple", "DisplaySize", oldDisplaySize);
        if (!restoreProfileValue("GameCanvasMode", oldMode))
            restored = false;
        if (!restoreProfileValue("LegacyDisplaySizeMigrated", oldMigration))
            restored = false;
        if (!restored)
            OutputDebugStringA(
                "C4dll-R: could not fully roll back a failed native-canvas setting write\n");
        return 0;
    }
    return 1;
}
