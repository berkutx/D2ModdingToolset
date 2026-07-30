/*
 * C4dll-R monolith: in-game menu bar + feature toggles, embedded in the cnc-ddraw renderer.
 * Self-contained: no mss32 dependency; game version + GameSettings chain inlined as raw addresses.
 * MNS/SMNS-specific patch sites. Original from D2ModdingToolset (GPLv3+, see repo LICENSE).
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cwchar>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>
#include <intrin.h>
#include "featuremenu_resources.h"
#pragma intrinsic(_ReturnAddress)

// Renderer bridge (rendererbridge.c, same module): DDReloadConfig re-reads ddraw.ini + dd_SetDisplayMode;
// DDTakeScreenshot saves a PNG. Both no-op safely before the renderer is initialized.
extern "C" void DDReloadConfig(void);
extern "C" void DDReloadConfigForMenu(int outputSizeChanged,
                                        int displayModeChanged);
extern "C" void DDRelayoutCurrentMode(void);
extern "C" int DDGetDisplayMode(void);
extern "C" void DDNormalizeLegacyExclusive(void);
extern "C" void DDToggleWindowedMode(void);
extern "C" void DDTakeScreenshot(void);
extern "C" int DDGetScaleMetrics(int* gameWidth, int* gameHeight, int* outputWidth,
                                  int* outputHeight, int* viewportX, int* viewportY,
                                  int* viewportWidth, int* viewportHeight);
extern "C" int DDGetOutputConfig(int* width, int* height,
                                   int* persistsNextStart);
extern "C" int DDGetSimpleZoom1000(void);
extern "C" int DDReadConfigString(const char* key, const char* defaultValue,
                                    char* value, unsigned int capacity);
extern "C" int DDWriteConfigString(const char* key, const char* value);
extern "C" LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wParam, LPARAM lParam);
extern "C" HMODULE g_ddraw_module;

// Exact-build, signature-gated Widescreen Battle port for the layouts in the original wrapper.
extern "C" void widebattle_set_enabled(int enabled);
extern "C" int widebattle_get_enabled(void);
extern "C" int widebattle_is_available(void);

// Exact-build true Hor+ game-canvas patch (horplus.cpp). All choices are restart-only.
extern "C" int horplus_is_available(void);
extern "C" int horplus_is_active(void);
extern "C" int horplus_get_active_size(int* width, int* height);
extern "C" int horplus_get_requested(int* mode, int* width, int* height);
extern "C" int horplus_set_requested(int mode, int width, int height);
extern "C" int horplus_set_native_requested(int displaySize);
extern "C" int horplus_get_adaptive_for_output(int outputWidth,
                                                  int outputHeight,
                                                  int* width, int* height,
                                                  int* nativeDisplaySize);
extern "C" int horplus_get_primary_adaptive(int* outputWidth,
                                               int* outputHeight,
                                               int* width, int* height,
                                               int* nativeDisplaySize);

// Presentation-only DisciplesGL Alternative wallpaper/frame for fixed 4:3 screens.
extern "C" int decorative_set_enabled(int enabled);
extern "C" int decorative_get_enabled(void);
extern "C" int decorative_is_available(void);

// Optional real IsoClouds.ff pipeline (clouds.cpp). The choice is restart-latched.
extern "C" int clouds_set_enabled(int enabled);
extern "C" int clouds_get_enabled(void);
extern "C" int clouds_get_active(void);
extern "C" int clouds_restart_pending(void);
extern "C" int clouds_get_status(void);
extern "C" int clouds_is_available(void);

// Game text locale bridge (localization.cpp): LCID supplies OEM/ANSI code pages; Locale=0 disables it.
extern "C" unsigned localization_get_locale(void);
extern "C" int localization_set_locale(unsigned locale);

// Native plugin host (pluginhost.cpp): builds the "Plugins" menu for .c4p plugins.
extern "C" void pluginhost_wait_ready(unsigned ms);
extern "C" int pluginhost_count(void);
extern "C" const char* pluginhost_name(int i);
extern "C" void* pluginhost_menu(int i);
extern "C" int pluginhost_command(unsigned id); // route a plugin-block WM_COMMAND to its c4p_command
extern "C" int pluginhost_mouse(UINT msg, WPARAM wParam); // physical client input for overlay plugins

// Map drag-scroll lives in global scope (defined after the anon namespace); forward-declared here.
extern bool g_dragScrollActive;
void dragScrollWndMove(int gameX, int gameY);

namespace {

// --- minimal logger -> OutputDebugStringA + C4menu-<pid>.log next to the exe ---
const char* exeDirFile(const char* leaf)
{
    static char base[MAX_PATH] = {};
    if (!base[0]) {
        GetModuleFileNameA(nullptr, base, sizeof(base));
        char* slash = strrchr(base, '\\');
        if (slash)
            slash[1] = 0;
        else
            base[0] = 0;
    }
    static char path[MAX_PATH];
    lstrcpynA(path, base, sizeof(path));
    lstrcatA(path, leaf);
    return path;
}

// Per-process log leaf so the two MP instances write separate files (no interleaved lines).
const char* logLeaf()
{
    static char leaf[32] = {0};
    if (!leaf[0]) wsprintfA(leaf, "C4menu-%lu.log", GetCurrentProcessId());
    return leaf;
}

const char* iniFile(); // defined below (C4menu.ini next to the exe)

// Diagnostics are OFF by default (no C4menu-<pid>.log noise in the game folder).
// Enable with [menu] debugLog=1 in C4menu.ini or the C4DLL_DEBUG env var. The same gate
// silences the timer host and the plugin host (featuremenu_debug_enabled below), so a
// release build writes no log files unless diagnostics are explicitly enabled.
bool debugLogEnabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        char env[8] = {};
        enabled = (GetPrivateProfileIntA("menu", "debugLog", 0, iniFile()) != 0 ||
                   GetEnvironmentVariableA("C4DLL_DEBUG", env, sizeof(env)) > 0)
                      ? 1
                      : 0;
    }
    return enabled != 0;
}

void mlog(const char* fmt, ...)
{
    if (!debugLogEnabled())
        return;
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 2] = 0;
    size_t n = strlen(buf);
    buf[n] = '\n';
    buf[n + 1] = 0;
    OutputDebugStringA(buf);
    HANDLE h = CreateFileA(exeDirFile(logLeaf()), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &w, nullptr);
        CloseHandle(h);
    }
}

// --- game/editor version detection (validated executable PE sizes) ---
enum GameVer
{
    VerUnknown,
    VerAkella,
    VerRussobit,
    VerGog,
    VerEditor
};
GameVer g_ver = VerUnknown;
DWORD g_exeSize = 0;

void detectVersion()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(exe, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0)
        g_exeSize = fa.nFileSizeLow;

    switch (g_exeSize) {
    case 3907200:
        g_ver = VerAkella;
        break;
    case 4214272: // Mortling's mod, exe with custom icon
    case 4187648:
        g_ver = VerRussobit;
        break;
    case 4474880:
        g_ver = VerGog;
        break;
    case 2895872:
        g_ver = VerEditor;
        break;
    default:
        g_ver = VerUnknown;
        break;
    }
}

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

// --- always-active: force the foreground-flag store to 1 (no pause on focus loss) ---
// Russobit only, site/bytes from bootfixes.cpp 0x5628BE. Toggles live.
const std::uint8_t kFgOriginal[10] = {0x2B, 0xC3, 0xF7, 0xD8, 0x1B, 0xC0, 0x40, 0x88, 0x47, 0x18};
const std::uint8_t kFgPatched[10] = {0xC6, 0x47, 0x18, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

uintptr_t alwaysActiveVA()
{
    return g_ver == VerRussobit ? 0x5628BE : 0;
}

void applyAlwaysActive(bool on)
{
    const uintptr_t va = alwaysActiveVA();
    if (!va)
        return;
    const auto* site = reinterpret_cast<const std::uint8_t*>(va);
    if (on) {
        if (memcmp(site, kFgPatched, 4) == 0)
            return;
        if (memcmp(site, kFgOriginal, sizeof(kFgOriginal)) != 0) {
            mlog("[menu] always-active: unexpected bytes at %#x; not patching", (unsigned)va);
            return;
        }
        if (writeBytes(va, kFgPatched, sizeof(kFgPatched)))
            mlog("[menu] always-active ON (%#x)", (unsigned)va);
    } else {
        if (memcmp(site, kFgPatched, 4) != 0)
            return;
        writeBytes(va, kFgOriginal, sizeof(kFgOriginal));
        mlog("[menu] always-active OFF (%#x)", (unsigned)va);
    }
}

// --- animation speed: virtual clock over timeGetTime ---
// The game advances anim frames when timeGetTime() >= nextUpdate (66ms slow / 33ms fast lists). We
// hook the game exe's WINMM!timeGetTime IAT slot and return a virtual clock running factor/10 faster:
// every interval shrinks proportionally -> faster animation, zero game-state writes, race-free, live.
// Only the game's IAT slot is patched, so cnc-ddraw's own fps-limiter timeGetTime is untouched.
//
// factor: 10 = x1.0 (identity) .. 50 = x5.0. Re-anchors on factor change so virtual time stays continuous.
using TimeGetTimeFn = DWORD(WINAPI*)(void);
TimeGetTimeFn g_realTimeGetTime = nullptr;

// Two live multipliers (x10 fixed-point); hook picks battle vs map by g_inBattle. 10 = identity (off).
volatile LONG g_battleFactor = 10;
volatile LONG g_mapFactor = 10;
volatile LONG g_inBattle = 0;
// Latest visual event since the last 32ms pump: 1=start, 2=end. A single last-writer-wins value
// preserves ordering when an instant/x15 effect starts and ends before one timer tick.
volatile LONG g_attackVisualEvent = 0;
volatile LONG g_attackVisualActive = 0; // exact start/end state when the completion hook is available
volatile LONG g_attackEndHookInstalled = 0;
volatile LONG g_batViewer = 0;   // IBatViewer instance, captured by batUpdateThunk; cleared on battle end
DWORD g_attackExpiryTick = 0;    // start of decay, or fallback hold boundary
DWORD g_attackWatchdogTick = 0;  // exact-end safety net; never the normal end condition

DWORD g_vcLastFactor = 10;
DWORD g_vcRealAnchor = 0, g_vcVirtAnchor = 0;
DWORD g_vcRealNow = 0, g_vcVirtNow = 0;

DWORD WINAPI timeGetTimeHook(void)
{
    if (!g_realTimeGetTime)
        return 0;
    DWORD factor = static_cast<DWORD>(g_inBattle ? g_battleFactor : g_mapFactor);
    if (factor < 1)
        factor = 1;
    if (g_vcLastFactor != factor) { // factor changed (incl. battle<->map switch) -> re-anchor
        g_vcLastFactor = factor;
        g_vcRealAnchor = g_vcRealNow;
        g_vcVirtAnchor = g_vcVirtNow;
    }
    g_vcRealNow = g_realTimeGetTime();
    DWORD result = factor * (g_vcRealNow - g_vcRealAnchor) / 10u + g_vcVirtAnchor;
    g_vcVirtNow = result;
    return result;
}

// Russobit timeGetTime IAT slot 0x6CE420 (Discipl2.exe import, WINMM). At factor 10 it's identity.
uintptr_t timeGetTimeIatVA()
{
    return g_ver == VerRussobit ? 0x6CE420 : 0;
}

void installTimeScaleHook()
{
    const uintptr_t va = timeGetTimeIatVA();
    if (!va || g_realTimeGetTime) // unsupported, or already installed
        return;
    auto slot = reinterpret_cast<void**>(va);
    g_realTimeGetTime = reinterpret_cast<TimeGetTimeFn>(*slot);
    void* hook = reinterpret_cast<void*>(&timeGetTimeHook);
    if (writeBytes(va, reinterpret_cast<const std::uint8_t*>(&hook), sizeof(hook)))
        mlog("[menu] anim time-scale hook installed (IAT %#x, real=%p)", (unsigned)va,
             reinterpret_cast<void*>(g_realTimeGetTime));
    else
        mlog("[menu] anim time-scale hook FAILED at IAT %#x", (unsigned)va);
}

// --- battle vs map discriminator: g_inBattle ---
// IBatViewer vftable @0x6F4294 (Russobit); slots [0] dtor 0x645900, [1] update 0x630DE3,
// [2] showAttackEffect 0x63203B, [3] battleEnd 0x631FFC. We overwrite the 4 slots with naked thunks
// that latch g_inBattle then tail-jump the original: calling-convention agnostic (only a store + jmp,
// no registers clobbered). Patched once at install with no battle live; battles never nest.
void* g_batDtorOrig = nullptr;
void* g_batUpdateOrig = nullptr;
void* g_batShowOrig = nullptr;
void* g_batEndOrig = nullptr;
void* g_batUiStateOrig = reinterpret_cast<void*>(0x639743);

__declspec(naked) void batUpdateThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 1
        mov dword ptr [g_batViewer], ecx   // thiscall: ecx = IBatViewer instance (for per-unit burst)
        jmp dword ptr [g_batUpdateOrig]
    }
}
__declspec(naked) void batShowThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 1
        mov dword ptr [g_attackVisualEvent], 1
        mov dword ptr [g_batViewer], ecx   // slot[2] DOES fire (slot[1] doesn't); same IBatViewer this
        jmp dword ptr [g_batShowOrig]
    }
}
__declspec(naked) void batEndThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 0
        mov dword ptr [g_batViewer], 0
        mov dword ptr [g_attackVisualEvent], 0
        mov dword ptr [g_attackVisualActive], 0
        mov dword ptr [g_attackExpiryTick], 0
        mov dword ptr [g_attackWatchdogTick], 0
        jmp dword ptr [g_batEndOrig]
    }
}
__declspec(naked) void batDtorThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 0
        mov dword ptr [g_batViewer], 0
        mov dword ptr [g_attackVisualEvent], 0
        mov dword ptr [g_attackVisualActive], 0
        mov dword ptr [g_attackExpiryTick], 0
        mov dword ptr [g_attackWatchdogTick], 0
        jmp dword ptr [g_batDtorOrig]
    }
}

// Called only by the final-zero branch of CBatViewerUtils::CAnimCounter. The original target owns
// thiscall's stack cleanup (ret 4), so a store + tail jump preserves every register/flag/argument.
__declspec(naked) void batAttackEndThunk()
{
    __asm {
        mov dword ptr [g_attackVisualEvent], 2
        jmp dword ptr [g_batUiStateOrig]
    }
}

// --- per-unit frame-speed hook: CBatUnitAnim vftable @0x6F48CC slot[1] update = 0x65615E ---
// CBatUnitAnim::update does --CBatUnitAnimData[+0x1024] each frame and advances a frame at 0. We patch the
// slot so EVERY per-unit update (every game frame) decrements the countdown extra for ANY actively-animating
// unit (cd>=2) -> attacker + all units taking the hit speed up; idle units (cd<2) untouched. g_perUnitExtra
// set by the pump; ==0 = passthrough. Scratch regs only (eax/ecx/edx), stack untouched -> safe tail-jmp.
void* g_batUnitAnimUpdOrig = nullptr;
volatile LONG g_perUnitExtra = 0; // extra countdown decrement per frame; 0 = off
volatile LONG g_perUnitHits = 0;  // diag: times the hook actually sped a unit
volatile LONG g_thunkCalls = 0;   // diag: times the per-unit update ran at all (hook reached?)
volatile LONG g_maxCd = 0;        // diag: largest data[4132] countdown ever observed

__declspec(naked) void batUnitAnimUpdateThunk()
{
    __asm {
        inc dword ptr [g_thunkCalls]     // always: proves the hook is reached
        mov edx, dword ptr [g_perUnitExtra]
        test edx, edx
        jz pu_pass
        mov eax, dword ptr [esp+4]       // this (CBatUnitAnim*), __stdcall(this)
        test eax, eax
        jz pu_pass
        mov ecx, dword ptr [eax+4]       // data (CBatUnitAnimData*)
        test ecx, ecx
        jz pu_pass
        mov eax, dword ptr [ecx+1024h]   // cd = data[4132] countdown
        cmp eax, dword ptr [g_maxCd]     // track the biggest cd we ever see
        jle pu_nomax
        mov dword ptr [g_maxCd], eax
    pu_nomax:
        cmp eax, 2                       // ANY animating unit (attacker + targets); idle is cd<2
        jl pu_pass
        sub eax, edx
        cmp eax, 1
        jge pu_store
        mov eax, 1
    pu_store:
        mov dword ptr [ecx+1024h], eax
        inc dword ptr [g_perUnitHits]
    pu_pass:
        jmp dword ptr [g_batUnitAnimUpdOrig]
    }
}

void installBattleDiscriminator()
{
    if (g_ver != VerRussobit)
        return;
    auto vt = reinterpret_cast<void**>(0x6F4294);
    g_batDtorOrig = vt[0];
    g_batUpdateOrig = vt[1];
    g_batShowOrig = vt[2];
    g_batEndOrig = vt[3];
    void* thunks[4] = {reinterpret_cast<void*>(&batDtorThunk), reinterpret_cast<void*>(&batUpdateThunk),
                       reinterpret_cast<void*>(&batShowThunk), reinterpret_cast<void*>(&batEndThunk)};
    if (writeBytes(0x6F4294, reinterpret_cast<const std::uint8_t*>(thunks), sizeof(thunks)))
        mlog("[menu] battle anim discriminator installed (vftable 0x6F4294)");
    else
        mlog("[menu] battle anim discriminator install FAILED");
    // per-unit frame-speed hook: CBatUnitAnim vftable 0x6F48CC, slot[1] (update 0x65615E) at 0x6F48D0
    g_batUnitAnimUpdOrig = reinterpret_cast<void**>(0x6F48CC)[1];
    void* uthunk = reinterpret_cast<void*>(&batUnitAnimUpdateThunk);
    if (writeBytes(0x6F48D0, reinterpret_cast<const std::uint8_t*>(&uthunk), sizeof(uthunk)))
        mlog("[menu] per-unit anim hook installed (vftable 0x6F48CC slot1, orig=%p)", g_batUnitAnimUpdOrig);
    else
        mlog("[menu] per-unit anim hook install FAILED");

    // Exact visual-attack end: after CAnimCounter reaches zero, this one call restores the battle UI.
    // Do not detour 0x639743 globally: it has several unrelated false callsites.
    constexpr uintptr_t sigVA = 0x638AD0;
    constexpr uintptr_t callVA = 0x638AD9;
    const std::uint8_t sig[14] = {
        0x85, 0xC0, 0x75, 0x34, 0x6A, 0x00, 0x8B, 0x4D, 0xFC, 0xE8, 0x65, 0x0C, 0x00, 0x00};
    if (memcmp(reinterpret_cast<const void*>(sigVA), sig, sizeof(sig)) == 0) {
        std::uint8_t call[5] = {0xE8, 0, 0, 0, 0};
        const std::int32_t rel = static_cast<std::int32_t>(
            reinterpret_cast<uintptr_t>(&batAttackEndThunk) - (callVA + sizeof(call)));
        memcpy(call + 1, &rel, sizeof(rel));
        if (writeBytes(callVA, call, sizeof(call))) {
            InterlockedExchange(&g_attackEndHookInstalled, 1);
            mlog("[menu] exact attack-end hook installed (CAnimCounter final call %#x)",
                 static_cast<unsigned>(callVA));
        } else {
            mlog("[menu] exact attack-end hook write failed; using timed fallback");
        }
    } else {
        mlog("[menu] exact attack-end signature mismatch; using timed fallback");
    }
}

// Speed 1..6 -> virtual-clock factor (x1.5/x2/x3/x4/x5/x15, fixed-point /10); 10 = identity (off).
const int kAnimFactor[6] = {15, 20, 30, 40, 50, 150};

// Map "Speed N" (1..5) to a virtual-clock factor for battle (which=0) or map (which=1). Off = x1.0.
// Runs on the game UI thread (menu WM_COMMAND).
void applyAnimSpeed(int which, bool enabled, int speed)
{
    if (speed < 1)
        speed = 1;
    if (speed > 6)
        speed = 6;
    LONG f = enabled ? kAnimFactor[speed - 1] : 10;
    if (which == 0)
        g_battleFactor = f;
    else
        g_mapFactor = f;
}

// --- persistence (C4menu.ini next to the exe) ---
const char* iniFile()
{
    return exeDirFile("C4menu.ini");
}

// --- menu state + command IDs (WM_COMMAND from the menu bar) ---
enum : UINT
{
    kIdAlwaysActive = 0xA100,
    kIdAnimOff = 0xA110, // BATTLE anim multiplier: off / 1.5x..5x
    kIdAnim1 = 0xA111,
    kIdAnim2 = 0xA112,
    kIdAnim3 = 0xA113,
    kIdAnim4 = 0xA114,
    kIdAnim5 = 0xA115,
    kIdAnim6 = 0xA116, // 15x (test/exaggerate)
    kIdEditorScenarios = 0xA120, // Disciple.ini [Disciple] ScenEditDatabase=0
    kIdEditorCampaigns = 0xA121, // Disciple.ini [Disciple] ScenEditDatabase=1
    // cnc-ddraw (ddraw.ini) settings
    kIdRendOpenGL = 0xA130,
    kIdRendGdi = 0xA131,
    kIdRendAuto = 0xA132,
    kIdShaderBase = 0xA140, // + index into kShaders[]
    kIdMaintas = 0xA150,
    kIdVsync = 0xA151,
    kIdBoxing = 0xA152,
    kIdScaleStretch = 0xA153,
    kIdScaleCustom = 0xA154, // disabled radio: non-empty ddraw.ini aspect_ratio
    kIdScaleGeometry = 0xA155, // disabled live-metrics line
    kIdScaleResult = 0xA156, // disabled live-metrics line
    kIdScaleFilterInfo = 0xA157, // disabled pixel/filter explanation
    kIdTicks0 = 0xA160,
    kIdTicks30 = 0xA161,
    kIdTicks60 = 0xA162,
    kIdTicks100 = 0xA163,
    kIdSingleCpu = 0xA164, // ddraw.ini singlecpu toggle (full restart)
    kIdHorplusBase = 0xA165, // + index into kHorplusSizes[] (restart)
    kIdHorplusAuto = 0xA16F, // monitor-adaptive stock/Hor+ selection (restart)
    kIdResBase = 0xA170, // + index into kRes[]
    kIdResInfo = 0xA17F, // legacy/hidden output-size diagnostics
    kIdFpsBase = 0xA180, // + index into kFpsValues[]
    // native game speeds (GameSettings + Disciple.ini); apply next battle / turn
    kIdBattle1 = 0xA190, // battleSpeed 1..4 (slow/normal/fast/instant)
    kIdBattle2 = 0xA191,
    kIdBattle3 = 0xA192,
    kIdBattle4 = 0xA193,
    kIdMap1 = 0xA1A0, // playerSpeed/opponentSpeed 1..3 (normal/fast/very fast)
    kIdMap2 = 0xA1A1,
    kIdMap3 = 0xA1A2,
    kIdModeWindowed = 0xA1B0, // ddraw.ini windowed/fullscreen pair
    kIdModeBorderless = 0xA1B1,
    kIdModeExclusive = 0xA1B2,
    kIdScreenshot = 0xA1C0, // action: take a screenshot
    kIdAnimMapOff = 0xA1D0, // MAP anim multiplier: off / 1.5x..5x
    kIdAnimMap1 = 0xA1D1,
    kIdAnimMap2 = 0xA1D2,
    kIdAnimMap3 = 0xA1D3,
    kIdAnimMap4 = 0xA1D4,
    kIdAnimMap5 = 0xA1D5,
    kIdAnimMap6 = 0xA1D6, // 15x (test/exaggerate)
    kIdDragScroll = 0xA1E0, // toggle: grab+drag map panning
    kIdAtkOff = 0xA1E1, // BATTLE attack burst: off / 1.5x..5x (extra speed only while a hit plays)
    kIdAtk1 = 0xA1E2,
    kIdAtk2 = 0xA1E3,
    kIdAtk3 = 0xA1E4,
    kIdAtk4 = 0xA1E5,
    kIdAtk5 = 0xA1E6,
    kIdAtk6 = 0xA1E7, // 15x (test)
    kIdPerUnit = 0xA1E8, // toggle: attack burst speeds ONLY acting units (experimental)
    kIdDialogVo = 0xA1E9, // toggle: auto-close voiced event popups after VO (logs dialog-vo-log.txt)
    kIdDialogVoInfo = 0xA1EA, // disabled info line: where the voiced-dialog log is written
    kIdWideBattle = 0xA1EB, // toggle: DLG_BATTLE_B with both panels (next battle, logical width >=990)
    kIdMenuLanguageAuto = 0xA1EC,
    kIdMenuLanguageEn = 0xA1ED,
    kIdMenuLanguageRu = 0xA1EE,
    kIdAutoConfirmUnitHire = 0xA1EF, // toggle: confirm X005TA0285 via normal BTN_YES functor
    kIdDisplaySize0 = 0xA1F0, // Disciple.ini [Disciple] DisplaySize=0 (restart)
    kIdDisplaySize1 = 0xA1F1,
    kIdDisplaySize2 = 0xA1F2,
    kIdDisplaySizeState = 0xA1F3, // disabled current -> after-restart line
    kIdDisplaySizeHelp = 0xA1F4, // disabled game-canvas/output boundary
    kIdOutputSizeCustom = 0xA1F5, // action: persisted ddraw.ini window/output size dialog
    kIdClouds = 0xA1F8, // restart-latched optional IsoClouds.ff pipeline
    kIdCloudsInfo = 0xA1F9, // disabled active -> requested / asset-status line
    kIdDecorativeBackground = 0xA1FA, // live presentation-only background/frame
    kIdLocaleNone = 0xA200, // disable wrapper OEM/ANSI recoding
    kIdLocaleBase = 0xA201, // + index into installed Windows locales
    kIdLast = 0xA2FF, // upper bound of our WM_COMMAND id block
};

// --- menu language (EN/RU). [menu] language = auto|en|ru; auto = Russian when the Windows UI
// language is Russian or the system codepage is 1251 (the Russobit audience). Wide strings +
// AppendMenuW keep the Cyrillic correct on any system codepage.
bool g_ru = false;
int g_menuLanguage = 0; // 0=auto, 1=en, 2=ru; selected from the Game menu, applied on restart
static const wchar_t* L(const wchar_t* en, const wchar_t* ru)
{
    return g_ru ? ru : en;
}

struct LocaleOption
{
    LCID locale;
    wchar_t label[160];
};

// 0xA201..0xA2FE: leave 0xA2FF as the command-block sentinel.
LocaleOption g_localeOptions[254] = {};
int g_localeCount = 0;

BOOL CALLBACK collectInstalledLocale(LPWSTR localeText)
{
    if (g_localeCount >= static_cast<int>(sizeof(g_localeOptions) / sizeof(g_localeOptions[0])))
        return FALSE;

    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(localeText, &end, 16);
    if (!value || end == localeText || *end)
        return TRUE;

    wchar_t name[128] = {};
    const LCID locale = static_cast<LCID>(value);
    if (!GetLocaleInfoW(locale, LOCALE_SLANGUAGE, name,
                        static_cast<int>(sizeof(name) / sizeof(name[0]))))
        swprintf_s(name, L"LCID 0x%04lX", value);

    LocaleOption& option = g_localeOptions[g_localeCount++];
    option.locale = locale;
    if (locale == GetUserDefaultLCID())
        swprintf_s(option.label, L"%s — %lu (%s)", name, value,
                   L(L"system default", L"системная по умолчанию"));
    else
        swprintf_s(option.label, L"%s — %lu", name, value);
    return TRUE;
}

void collectInstalledLocales()
{
    g_localeCount = 0;
    EnumSystemLocalesW(collectInstalledLocale, LCID_INSTALLED);
}

// cnc-ddraw renderer + shader tables (menu labels EN/RU + exact ddraw.ini value).
struct NameVal
{
    const wchar_t* en;
    const wchar_t* ru;
    const char* value;
};
const NameVal kRenderers[] = {
    {L"OpenGL - shaders + best upscaling (recommended)",
     L"OpenGL - шейдеры и лучший апскейл (рекомендуется)", "opengl"},
    {L"GDI - software, max compatibility (slower)",
     L"GDI - программный, максимальная совместимость (медленнее)", "gdi"},
    {L"Auto - picks D3D9 first (no shader filters)",
     L"Auto - сам выберет D3D9 (без шейдерных фильтров)", "auto"}};
const int kRendererCount = 3;
// Image filters, ranked best->basic for D2's hand-painted art.
const NameVal kShaders[] = {
    {L"Lanczos - sharp, detailed (best for D2 art)",
     L"Lanczos - чёткий, детальный (лучший для графики D2)",
     "Shaders\\interpolation\\lanczos2-sharp.glsl"},
    {L"xBRZ - pixel-art scaler, clean sprite edges",
     L"xBRZ - пиксель-арт скейлер, чистые края спрайтов",
     "Shaders\\xbrz\\xbrz-freescale-multipass.glsl"},
    {L"Bicubic - smooth, balanced (cnc default)",
     L"Bicubic - мягкий, сбалансированный (дефолт cnc)",
     "Shaders\\interpolation\\catmull-rom-bilinear.glsl"},
    {L"AMD FSR - modern edge sharpening, crisp",
     L"AMD FSR - современная резкость краёв",
     "Shaders\\interpolation\\fsr.glsl"},
    {L"xBR lv2 - pixel-art, lighter than xBRZ",
     L"xBR lv2 - пиксель-арт, легче xBRZ",
     "Shaders\\xbr\\xbr-lv2-noblend.glsl"},
    {L"Bilinear - simple smoothing, a bit soft",
     L"Bilinear - простое сглаживание, слегка мыльно",
     "Shaders\\interpolation\\bilinear.glsl"},
    {L"None - sharpest pixels, blocky on zoom",
     L"Без фильтра - самые чёткие пиксели, кубики при увеличении",
     "Shaders\\nearest-neighbor.glsl"},
    {L"CRT - retro scanlines (style, not sharper)",
     L"CRT - ретро-развёртка (стиль, не чёткость)",
     "Shaders\\crt\\crt-lottes-fast-no-warp-bilinear.glsl"}};
const int kShaderCount = 8;
// -1 = limiter fully off (cnc-ddraw treats 0 as "emulate 60hz flip", not off)
const int kTicksValues[] = {-1, 30, 60, 100};
const int kTicksCount = 4;
// Advanced output/window size (ddraw.ini width/height). 0,0 follows the game's selected logical
// resolution. Fixed values only scale the finished game canvas and never change the visible map.
// The regular UI exposes this as one compact custom-size dialog at the bottom of the unified
// Resolution popup; old/manual ini values remain supported and reflected by diagnostics below.
struct ResOpt
{
    const wchar_t* en;
    const wchar_t* ru;
    int w, h;
};
const ResOpt kRes[] = {
    {L"Automatic (follows game resolution)",
     L"Авто (следует разрешению игры)", 0, 0},
    {L"640 x 480\t(4:3)", L"640 x 480\t(4:3)", 640, 480},
    {L"800 x 600\t(4:3)", L"800 x 600\t(4:3)", 800, 600},
    {L"1024 x 768\t(4:3)", L"1024 x 768\t(4:3)", 1024, 768},
    {L"1152 x 864\t(4:3)", L"1152 x 864\t(4:3)", 1152, 864},
    {L"1280 x 720\t(16:9)", L"1280 x 720\t(16:9)", 1280, 720},
    {L"1280 x 960\t(4:3)", L"1280 x 960\t(4:3)", 1280, 960},
    {L"1280 x 1024\t(5:4)", L"1280 x 1024\t(5:4)", 1280, 1024},
    {L"1366 x 768\t(≈16:9)", L"1366 x 768\t(≈16:9)", 1366, 768},
    {L"1600 x 900\t(16:9)", L"1600 x 900\t(16:9)", 1600, 900},
    {L"1600 x 1200\t(4:3)", L"1600 x 1200\t(4:3)", 1600, 1200},
    {L"1920 x 1080\t(16:9)", L"1920 x 1080\t(16:9)", 1920, 1080},
    {L"1920 x 1440\t(4:3)", L"1920 x 1440\t(4:3)", 1920, 1440},
    {L"2560 x 1440\t(16:9)", L"2560 x 1440\t(16:9)", 2560, 1440},
    {L"3840 x 2160\t(16:9)", L"3840 x 2160\t(16:9)", 3840, 2160}};
const int kResCount = static_cast<int>(sizeof(kRes) / sizeof(kRes[0]));
static_assert(kResCount <= kIdResInfo - kIdResBase,
              "output-resolution command ids overlap the info row");
struct DisplaySizeOpt
{
    int w, h;
};
const DisplaySizeOpt kDisplaySizes[] = {
    {800, 600},
    {1024, 768},
    {1280, 1024},
};
const wchar_t* kDisplaySizeLabelsEn[] = {
    L"★ 800 x 600\t(4:3)",
    L"★ 1024 x 768\t(4:3)",
    L"★ 1280 x 1024\t(5:4)",
};
const wchar_t* kDisplaySizeLabelsRu[] = {
    L"★ 800 x 600\t(4:3)",
    L"★ 1024 x 768\t(4:3)",
    L"★ 1280 x 1024\t(5:4)",
};
const int kDisplaySizeCount = 3;
const ResOpt kHorplusSizes[] = {
    {L"1066 x 600\t(≈16:9)", L"1066 x 600\t(≈16:9)", 1066, 600},
    {L"1152 x 648\t(16:9)", L"1152 x 648\t(16:9)", 1152, 648},
    {L"1280 x 720\t(16:9)", L"1280 x 720\t(16:9)", 1280, 720},
    {L"1366 x 768\t(≈16:9)", L"1366 x 768\t(≈16:9)", 1366, 768},
    {L"1440 x 810\t(16:9)", L"1440 x 810\t(16:9)", 1440, 810},
    {L"1536 x 864\t(16:9)", L"1536 x 864\t(16:9)", 1536, 864},
    {L"1600 x 900\t(16:9)", L"1600 x 900\t(16:9)", 1600, 900},
    {L"1820 x 1024\t(≈16:9)", L"1820 x 1024\t(≈16:9)", 1820, 1024},
    {L"1920 x 1080\t(16:9)", L"1920 x 1080\t(16:9)", 1920, 1080},
    {L"2560 x 1440\t(16:9)", L"2560 x 1440\t(16:9)", 2560, 1440},
};
const int kHorplusSizeCount =
    static_cast<int>(sizeof(kHorplusSizes) / sizeof(kHorplusSizes[0]));
static_assert(kHorplusSizeCount <= kIdResBase - kIdHorplusBase,
              "game-canvas command ids overlap output-resolution ids");
static_assert(kIdHorplusBase + kHorplusSizeCount == kIdHorplusAuto,
              "adaptive command must immediately follow manual Hor+ ids");
const int kFpsValues[] = {-1, 30, 60, 144};
const wchar_t* kFpsLabelsEn[] = {L"Monitor refresh rate", L"30", L"60", L"144"};
const wchar_t* kFpsLabelsRu[] = {L"Частота монитора", L"30", L"60", L"144"};
const int kFpsCount = 4;
// Display mode = ddraw.ini windowed/fullscreen pair. The C4 menu is present only in the normal
// window and is detached in both fullscreen modes. Exclusive is false+false in cnc-ddraw:
// !windowed performs the real mode change; fullscreen=true means "force desktop-sized output".
struct ModeOpt
{
    const wchar_t* en;
    const wchar_t* ru;
    const char* windowed;
    const char* fullscreen;
};
const ModeOpt kModes[] = {
    {L"Windowed", L"Оконный", "true", "false"},
    {L"Fullscreen - desktop size (adaptive, borderless)",
     L"Полный экран - размер рабочего стола (адаптивно, без рамки)", "true", "true"},
    {L"Exclusive fullscreen (advanced)",
     L"Эксклюзивный полный экран (дополнительно)", "false", "false"}};
const int kModeCount = 3;

bool g_alwaysActive = false;
bool g_battleAnimEnabled = false; // BATTLE live anim multiplier
int g_battleAnimSpeed = 5;
bool g_mapAnimEnabled = false;    // MAP live anim multiplier
int g_mapAnimSpeed = 5;
bool g_battleAttackEnabled = false; // BATTLE attack burst: speed up only while a hit/effect plays
int g_battleAttackSpeed = 3;        // burst factor (1..5); idle stays at the battle base (vanilla unless set)
bool g_perUnitBurst = false;        // EXPERIMENTAL: scale only the acting animators' interval (not the global clock)
// cnc-ddraw (ddraw.ini) state, read at startup so the menu shows current values (-1 = unknown/custom)
int g_rendererIdx = 0;  // index into kRenderers
int g_shaderIdx = -1;   // index into kShaders
bool g_maintas = false, g_vsync = false, g_boxing = false;
char g_aspectRatio[32] = {}; // non-empty overrides native aspect and forces maintas in cnc-ddraw
bool g_singlecpu = true; // ddraw.ini singlecpu stability mode (cnc-ddraw default true)
bool g_dragScroll = true; // grab+drag map panning (ini [menu] dragScroll, default on)
bool g_dialogVoSkip = false; // auto-close voiced event popups after VO + log their text (default off)
bool g_autoConfirmUnitHire = false; // skip X005TA0285 through its BTN_YES functor (default off)
int g_editorDatabase = 0; // ScenEditDatabase: 0=scenarios, 1=campaigns (restart required)
int g_ticksIdx = -1;    // index into kTicksValues
int g_resIdx = -1;      // index into kRes
int g_requestedOutputW = 0, g_requestedOutputH = 0; // exact persisted ddraw.ini pair
int g_displaySizePending = 0; // Disciple.ini value selected for the next process start
int g_displaySizeCurrent = -1; // inferred from the live DirectDraw game canvas
bool g_gameCanvasExplicitlySelected = false; // native or Hor+ selector clicked this process
int g_fpsIdx = -1;      // index into kFpsValues
int g_battleSpeed = 2;  // native GameSettings.battleSpeed (1..4)
int g_mapSpeed = 1;     // native GameSettings.playerSpeed/opponentSpeed (1..3)
int g_modeIdx = 0;      // persisted / next-start index into kModes
int g_liveModeIdx = -1; // actual cnc-ddraw mode after F4 / Alt+Enter
int g_adaptiveNoticeOutputW = 0, g_adaptiveNoticeOutputH = 0;
int g_adaptiveNoticeCanvasW = 0, g_adaptiveNoticeCanvasH = 0;
HMENU g_bar = nullptr;
UINT g_relayoutMsg = 0; // registered msg: marshal menu/fullscreen chrome sync onto the GUI thread
HMENU g_gameMenu = nullptr, g_videoMenu = nullptr, g_perfMenu = nullptr; // top-level bar menus
HMENU g_battleAnimMenu = nullptr, g_mapAnimMenu = nullptr, g_battleAtkMenu = nullptr;
HMENU g_rendMenu = nullptr, g_shaderMenu = nullptr;
HMENU g_ticksMenu = nullptr, g_resMenu = nullptr, g_fpsMenu = nullptr, g_scaleMenu = nullptr;
HMENU g_displaySizeMenu = nullptr;
HMENU g_battleMenu = nullptr, g_mapMenu = nullptr, g_modeMenu = nullptr;
HMENU g_menuLanguageMenu = nullptr, g_localeMenu = nullptr;
HMENU g_editorModeMenu = nullptr;
int g_resolutionMenuPosition = -1; // position of the one Video -> Resolution popup
int g_ncCursorShown = 0;  // 1 while we've bumped the OS cursor visible for the non-client area
int g_ncCursorAdded = 0;  // how many ShowCursor(TRUE) we added (to remove exactly that many)

using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProcFn g_origWndProc = nullptr;
HWND g_gameHwnd = nullptr; // game window (drag-scroll SetCapture target); set in wndProcHook
const UINT_PTR kPressTimerId = 0xC4D7; // our WM_TIMER source: on-elapse END_TURN press fires ONLY on
                                       // WM_TIMER (idle-gated), like legacy SetTimer(hWnd,0,0x20,0)

uintptr_t gameWndProcVA()
{
    return g_ver == VerRussobit ? 0x562e0f : 0;
}

void persist()
{
    const char* f = iniFile();
    char buf[8];
    static const char* const menuLanguages[] = {"auto", "en", "ru"};
    WritePrivateProfileStringA("menu", "language", menuLanguages[g_menuLanguage], f);
    WritePrivateProfileStringA("menu", "alwaysActive", g_alwaysActive ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "battleAnimEnabled", g_battleAnimEnabled ? "1" : "0", f);
    wsprintfA(buf, "%d", g_battleAnimSpeed);
    WritePrivateProfileStringA("menu", "battleAnimSpeed", buf, f);
    WritePrivateProfileStringA("menu", "mapAnimEnabled", g_mapAnimEnabled ? "1" : "0", f);
    wsprintfA(buf, "%d", g_mapAnimSpeed);
    WritePrivateProfileStringA("menu", "mapAnimSpeed", buf, f);
    WritePrivateProfileStringA("menu", "battleAttackEnabled", g_battleAttackEnabled ? "1" : "0", f);
    wsprintfA(buf, "%d", g_battleAttackSpeed);
    WritePrivateProfileStringA("menu", "battleAttackSpeed", buf, f);
    WritePrivateProfileStringA("menu", "perUnitBurst", g_perUnitBurst ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "dragScroll", g_dragScroll ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "wideBattle",
                               widebattle_get_enabled() ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "decorativeBackground",
                               decorative_get_enabled() ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "dialogVoSkip", g_dialogVoSkip ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "autoConfirmUnitHire",
                               g_autoConfirmUnitHire ? "1" : "0", f);
}

// First run: if C4menu.ini is absent, generate a commented one (converting any old mss32menu.ini).
// Never touches the game's own Disciple.ini / settings.lua. Written directly since
// WritePrivateProfileStringA cannot emit comments.
void seedConfigFirstRun()
{
    // exeDirFile() intentionally reuses one scratch buffer. Keep both names locally: otherwise
    // resolving mss32menu.ini below overwrites the C4menu.ini pointer and CREATE_NEW targets the
    // legacy file instead of our own config.
    char f[MAX_PATH] = {};
    lstrcpynA(f, iniFile(), sizeof(f));
    if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES)
        return; // already present -> the user owns it

    // Convert from the old single-global anim config if mss32menu.ini is present.
    char old[MAX_PATH] = {};
    lstrcpynA(old, exeDirFile("mss32menu.ini"), sizeof(old));
    const int aa = GetPrivateProfileIntA("menu", "alwaysActive", 0, old) ? 1 : 0;
    const int oldAnimOn = GetPrivateProfileIntA("menu", "animationSpeedEnabled", 0, old);
    // Defaults: battle animation ON at 2x, map animation OFF. (Old single-global "on" -> battle speed 1.)
    const int battleEn = 1;
    const int mapEn = 0;
    const int bSp = oldAnimOn ? 1 : 2; // battle default 2x
    const int mSp = 2;

    char buf[3072];
    const int n = sprintf_s(buf, sizeof(buf),
        "; C4dll-R menu settings (auto-generated on first run).\r\n"
        "; Edit by hand, or use the in-game \"Game\" menu - changes are saved back here.\r\n"
        "; SEPARATE from the game's own Disciple.ini / Scripts\\settings.lua. C4dll-R changes\r\n"
        "; Disciple.ini only for explicit game/editor choices and one recognized legacy migration.\r\n"
        "\r\n"
        "[menu]\r\n"
        "; Menu language: auto = Russian on Russian systems, else English. Or force: en / ru.\r\n"
        "language=auto\r\n"
        "\r\n"
        "; Keep the game running (no pause) when the window loses focus.  0 = off, 1 = on.\r\n"
        "alwaysActive=%d\r\n"
        "\r\n"
        "; Live animation-speed multiplier, separate for battle and the strategic map\r\n"
        "; (timeGetTime virtual clock; safe, no game-memory patch).\r\n"
        ";   *Enabled : 0 = vanilla, 1 = on.   *Speed : 1..6  ->  1.5x / 2x / 3x / 4x / 5x / 15x.\r\n"
        "battleAnimEnabled=%d\r\n"
        "battleAnimSpeed=%d\r\n"
        "mapAnimEnabled=%d\r\n"
        "mapAnimSpeed=%d\r\n"
        "\r\n"
        "; Attack speed-up: extra burst ONLY while a hit/effect plays (on top of battle speed).\r\n"
        ";   battleAttackEnabled : 0 = off, 1 = on.   battleAttackSpeed : 1..6  ->  1.5x..5x / 15x.\r\n"
        "battleAttackEnabled=1\r\n"
        "battleAttackSpeed=5\r\n"
        "\r\n"
        "; Grab and drag the strategic map with the left mouse button. A normal click is preserved.\r\n"
        "; Native window-edge scrolling remains available.  0 = off, 1 = on (default).\r\n"
        "dragScroll=1\r\n"
        "\r\n"
        "; Show both unit panels at once in battle (validated Disciples II layout, logical width 990+).\r\n"
        "; The choice is latched when the next battle opens.  0 = off, 1 = on (default).\r\n"
        "wideBattle=1\r\n"
        "\r\n"
        "; Decorative DisciplesGL Alternative wallpaper/frame around fixed 4:3 screens.\r\n"
        "; Presentation only; it does not change the game canvas or mouse coordinates.\r\n"
        "decorativeBackground=1\r\n"
        "\r\n"
        "; Auto-close event dialogs that have a voiceover, once the VO finishes (streamer aid).\r\n"
        "; Their text is appended to dialog-vo-log.txt in the game folder.  0 = off (default), 1 = on.\r\n"
        "dialogVoSkip=0\r\n"
        "\r\n"
        "; Auto-confirm \"Do you want to hire this unit?\" through the normal BTN_YES action.\r\n"
        "; Applies only during the local player's active turn.  0 = off (default), 1 = on.\r\n"
        "autoConfirmUnitHire=0\r\n"
        "\r\n"
        "; Write C4menu-<pid>.log / C4plugins.log diagnostics next to the exe.\r\n"
        "; 0 = off (default), 1 = on.\r\n"
        "debugLog=0\r\n",
        aa, battleEn, bSp, mapEn, mSp);
    if (n <= 0)
        return;

    HANDLE h = CreateFileA(f, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, buf, static_cast<DWORD>(n), &wr, nullptr);
        CloseHandle(h);
        mlog("[menu] first-run C4menu.ini generated (alwaysActive=%d, battleAnim on=%d, b=%d m=%d)", aa,
             battleEn, bSp, mSp);
    }
}

// --- cnc-ddraw settings live in ddraw.ini (same dir as C4menu.ini) ---
const char* ddrawIni()
{
    return exeDirFile("ddraw.ini");
}

bool readDdrawBool(const char* key, bool def)
{
    char v[16] = {};
    if (!DDReadConfigString(key, def ? "true" : "false", v,
                            static_cast<unsigned int>(sizeof(v))))
        GetPrivateProfileStringA("ddraw", key, def ? "true" : "false", v,
                                 sizeof(v), ddrawIni());
    return lstrcmpiA(v, "true") == 0 || lstrcmpiA(v, "1") == 0 || lstrcmpiA(v, "yes") == 0;
}

void readDdrawStr(const char* key, const char* def, char* value,
                  unsigned int capacity)
{
    if (!DDReadConfigString(key, def, value, capacity))
        GetPrivateProfileStringA("ddraw", key, def, value, capacity,
                                 ddrawIni());
}

int readDdrawInt(const char* key, int def)
{
    char fallback[24] = {};
    char value[24] = {};
    wsprintfA(fallback, "%d", def);
    readDdrawStr(key, fallback, value,
                 static_cast<unsigned int>(sizeof(value)));
    return strstr(value, "0x") ? static_cast<int>(strtol(value, nullptr, 0))
                               : atoi(value);
}

void writeDdrawStr(const char* key, const char* value)
{
    // Preserves comment lines and writes a clean key=value (no inline comment), as cnc-ddraw's
    // strict parser needs.
    if (DDWriteConfigString(key, value))
        return;
    WritePrivateProfileStringA("ddraw", key, value, ddrawIni());
}

void writeDdrawBool(const char* key, bool on)
{
    writeDdrawStr(key, on ? "true" : "false");
}

// Re-apply ddraw settings live: we ARE the renderer, so call DDReloadConfig directly. Runs on the UI thread.
void applyDdrawLive(bool outputSizeChanged, bool displayModeChanged)
{
    DDReloadConfigForMenu(outputSizeChanged ? 1 : 0,
                          displayModeChanged ? 1 : 0);
}

void syncChrome(HWND hwnd); // defined after buildMenu(); always called on the window's GUI thread

// Take a screenshot via the renderer (timestamped PNG in .\Screenshots\).
void takeScreenshot()
{
    DDTakeScreenshot();
}

// Read current ddraw.ini values into menu state so the checks reflect reality.
void readDdrawState()
{
    char r[32] = {};
    readDdrawStr("renderer", "opengl", r,
                 static_cast<unsigned int>(sizeof(r)));
    g_rendererIdx = -1;
    for (int i = 0; i < kRendererCount; ++i)
        if (lstrcmpiA(r, kRenderers[i].value) == 0) {
            g_rendererIdx = i;
            break;
        }

    char sh[MAX_PATH] = {};
    readDdrawStr("shader", "", sh,
                 static_cast<unsigned int>(sizeof(sh)));
    g_shaderIdx = -1;
    for (int i = 0; i < kShaderCount; ++i)
        if (lstrcmpiA(sh, kShaders[i].value) == 0) {
            g_shaderIdx = i;
            break;
        }

    g_maintas = readDdrawBool("maintas", false);
    g_vsync = readDdrawBool("vsync", false);
    g_boxing = readDdrawBool("boxing", false);
    readDdrawStr("aspect_ratio", "", g_aspectRatio,
                 static_cast<unsigned int>(sizeof(g_aspectRatio)));
    g_singlecpu = readDdrawBool("singlecpu", true);

    const int ticks = readDdrawInt("maxgameticks", 0);
    g_ticksIdx = -1;
    for (int i = 0; i < kTicksCount; ++i)
        if (kTicksValues[i] == ticks) {
            g_ticksIdx = i;
            break;
        }

    const int w = readDdrawInt("width", 0);
    const int h = readDdrawInt("height", 0);
    g_requestedOutputW = w;
    g_requestedOutputH = h;
    g_resIdx = -1;
    for (int i = 0; i < kResCount; ++i)
        if (kRes[i].w == w && kRes[i].h == h) {
            g_resIdx = i;
            break;
        }

    const int fps = readDdrawInt("maxfps", -1);
    g_fpsIdx = -1;
    for (int i = 0; i < kFpsCount; ++i)
        if (kFpsValues[i] == fps) {
            g_fpsIdx = i;
            break;
        }

    const bool wnd = readDdrawBool("windowed", false);
    const bool fs = readDdrawBool("fullscreen", false);
    g_modeIdx = !wnd ? 2 : (fs ? 1 : 0); // !windowed=exclusive; windowed+fullscreen=borderless
    if (!wnd && fs) {
        // C4dll-R <= 1.4 wrote false+true for exclusive. In cnc-ddraw, the second bit means
        // "force desktop-sized output", not exclusivity; normalize the persisted pair for the next
        // reload while preserving the already-live mode until the user changes it.
        writeDdrawBool("fullscreen", false);
        writeDdrawBool("toggle_borderless", false);
        DDNormalizeLegacyExclusive();
    }
}

struct OutputSizeDialogState
{
    int width;       // persisted value; 0 x 0 means follow the game canvas
    int height;
    int editWidth;   // useful values retained while Automatic is checked
    int editHeight;
    int minWidth;    // downscaling needs adjmouse so clicks stay in game coordinates
    int minHeight;
};

constexpr int kOutputSizeMinWidth = 320;
constexpr int kOutputSizeMinHeight = 240;
constexpr int kOutputSizeMax = 8192;

void enableOutputSizeEdits(HWND dialog, bool enabled)
{
    EnableWindow(GetDlgItem(dialog, IDC_C4_OUTPUT_WIDTH_LABEL), enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_C4_OUTPUT_WIDTH), enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_C4_OUTPUT_HEIGHT_LABEL), enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_C4_OUTPUT_HEIGHT), enabled ? TRUE : FALSE);
}

INT_PTR CALLBACK outputSizeDialogProc(HWND dialog, UINT message, WPARAM wParam,
                                      LPARAM lParam)
{
    auto* state = reinterpret_cast<OutputSizeDialogState*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));

    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<OutputSizeDialogState*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        if (!state)
            return FALSE;

        SetWindowTextW(
            dialog,
            L(L"Window/stream output only", L"Только окно/вывод для стрима"));
        SetDlgItemTextW(
            dialog, IDC_C4_OUTPUT_AUTO,
            L(L"Automatic (follow game resolution)",
              L"Автоматически (следовать разрешению игры)"));
        SetDlgItemTextW(dialog, IDC_C4_OUTPUT_WIDTH_LABEL,
                        L(L"Width:", L"Ширина:"));
        SetDlgItemTextW(dialog, IDC_C4_OUTPUT_HEIGHT_LABEL,
                        L(L"Height:", L"Высота:"));
        wchar_t help[384] = {};
        if (state->minWidth > kOutputSizeMinWidth ||
            state->minHeight > kOutputSizeMinHeight) {
            swprintf_s(
                help,
                L(L"Does not change the game view. Image area only; frame, menu and title bar are outside. Borderless uses the desktop. Mouse remapping is off, so the minimum is %d x %d.",
                  L"Не меняет игровой обзор. Только область изображения; рамка, меню и заголовок снаружи. Без рамки — рабочий стол. Пересчёт мыши выключен: минимум %d x %d."),
                state->minWidth, state->minHeight);
        } else {
            wcscpy_s(
                help,
                L(L"Does not change the game view. Image area only; frame, menu and title bar are outside. Borderless fullscreen uses the desktop.",
                  L"Не меняет игровой обзор. Только область изображения; рамка, меню и заголовок снаружи. Полный экран без рамки использует рабочий стол."));
        }
        SetDlgItemTextW(dialog, IDC_C4_OUTPUT_HELP, help);
        SetDlgItemTextW(dialog, IDOK, L(L"OK", L"Сохранить"));
        SetDlgItemTextW(dialog, IDCANCEL, L(L"Cancel", L"Отмена"));

        SendDlgItemMessageW(dialog, IDC_C4_OUTPUT_WIDTH, EM_SETLIMITTEXT, 5, 0);
        SendDlgItemMessageW(dialog, IDC_C4_OUTPUT_HEIGHT, EM_SETLIMITTEXT, 5, 0);
        SetDlgItemInt(dialog, IDC_C4_OUTPUT_WIDTH,
                      static_cast<UINT>(state->editWidth), FALSE);
        SetDlgItemInt(dialog, IDC_C4_OUTPUT_HEIGHT,
                      static_cast<UINT>(state->editHeight), FALSE);
        const bool automatic = state->width == 0 && state->height == 0;
        CheckDlgButton(dialog, IDC_C4_OUTPUT_AUTO,
                       automatic ? BST_CHECKED : BST_UNCHECKED);
        enableOutputSizeEdits(dialog, !automatic);
        return TRUE;
    }

    if (message == WM_COMMAND && state) {
        const UINT command = LOWORD(wParam);
        if (command == IDC_C4_OUTPUT_AUTO && HIWORD(wParam) == BN_CLICKED) {
            enableOutputSizeEdits(
                dialog,
                IsDlgButtonChecked(dialog, IDC_C4_OUTPUT_AUTO) != BST_CHECKED);
            return TRUE;
        }
        if (command == IDOK) {
            if (IsDlgButtonChecked(dialog, IDC_C4_OUTPUT_AUTO) == BST_CHECKED) {
                state->width = 0;
                state->height = 0;
                EndDialog(dialog, IDOK);
                return TRUE;
            }

            BOOL widthValid = FALSE, heightValid = FALSE;
            const UINT width =
                GetDlgItemInt(dialog, IDC_C4_OUTPUT_WIDTH, &widthValid, FALSE);
            const UINT height =
                GetDlgItemInt(dialog, IDC_C4_OUTPUT_HEIGHT, &heightValid, FALSE);
            if (!widthValid || !heightValid ||
                width < static_cast<UINT>(state->minWidth) ||
                height < static_cast<UINT>(state->minHeight) ||
                width > kOutputSizeMax || height > kOutputSizeMax) {
                wchar_t error[320] = {};
                swprintf_s(
                    error,
                    L(L"Enter a width from %d to %d and a height from %d to %d.",
                      L"Введите ширину от %d до %d и высоту от %d до %d."),
                    state->minWidth, kOutputSizeMax,
                    state->minHeight, kOutputSizeMax);
                MessageBoxW(dialog, error, L(L"Window size", L"Размер окна"),
                             MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            state->width = static_cast<int>(width);
            state->height = static_cast<int>(height);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (command == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    } else if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool saveOutputSize(int width, int height, bool* rollbackComplete)
{
    if (rollbackComplete)
        *rollbackComplete = true;
    char oldWidth[24] = {}, oldHeight[24] = {};
    char newWidth[24] = {}, newHeight[24] = {};
    readDdrawStr("width", "0", oldWidth,
                 static_cast<unsigned int>(sizeof(oldWidth)));
    readDdrawStr("height", "0", oldHeight,
                 static_cast<unsigned int>(sizeof(oldHeight)));
    wsprintfA(newWidth, "%d", width);
    wsprintfA(newHeight, "%d", height);

    if (!DDWriteConfigString("width", newWidth))
        return false;
    if (DDWriteConfigString("height", newHeight))
        return true;

    // Keep the effective pair coherent if the second profile write fails.
    const bool widthRestored =
        DDWriteConfigString("width", oldWidth) != 0;
    const bool heightRestored =
        DDWriteConfigString("height", oldHeight) != 0;
    if (rollbackComplete)
        *rollbackComplete = widthRestored && heightRestored;
    return false;
}

bool chooseOutputSize(HWND owner, int* width, int* height)
{
    if (!width || !height)
        return false;

    int gameWidth = 0, gameHeight = 0, outputWidth = 0, outputHeight = 0;
    const bool haveMetrics =
        DDGetScaleMetrics(&gameWidth, &gameHeight, &outputWidth, &outputHeight,
                          nullptr, nullptr, nullptr, nullptr) != 0;
    int minWidth = kOutputSizeMinWidth;
    int minHeight = kOutputSizeMinHeight;
    if (!readDdrawBool("adjmouse", true) && haveMetrics) {
        if (gameWidth > minWidth)
            minWidth = gameWidth;
        if (gameHeight > minHeight)
            minHeight = gameHeight;
    }

    OutputSizeDialogState state = {
        *width, *height, *width, *height, minWidth, minHeight};
    if (state.editWidth < state.minWidth ||
        state.editHeight < state.minHeight) {
        if (haveMetrics && outputWidth >= state.minWidth &&
            outputHeight >= state.minHeight) {
            state.editWidth = outputWidth;
            state.editHeight = outputHeight;
        } else {
            state.editWidth = state.minWidth > 1280 ? state.minWidth : 1280;
            state.editHeight = state.minHeight > 720 ? state.minHeight : 720;
        }
    }

    SetLastError(ERROR_SUCCESS);
    const INT_PTR result = DialogBoxParamW(
        g_ddraw_module, MAKEINTRESOURCEW(IDD_C4_OUTPUT_SIZE), owner,
        outputSizeDialogProc, reinterpret_cast<LPARAM>(&state));
    if (result == -1) {
        const DWORD error = GetLastError();
        wchar_t message[320] = {};
        swprintf_s(
            message,
            L(L"Could not open the window-size dialog (Windows error %lu).",
              L"Не удалось открыть диалог размера окна (ошибка Windows %lu)."),
            static_cast<unsigned long>(error));
        mlog("[menu] DialogBoxParamW(IDD_C4_OUTPUT_SIZE) failed: %lu",
             static_cast<unsigned long>(error));
        MessageBoxW(owner, message, L(L"Window size", L"Размер окна"),
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (result != IDOK)
        return false;
    *width = state.width;
    *height = state.height;
    return true;
}

// --- native game speeds: battle vs map (the engine's own split) ---
// In GameSettings (read at battle/turn start) and Disciple.ini [Settings].
const char* discipleIni()
{
    return exeDirFile("Disciple.ini");
}

int displaySizeForCanvas(int width, int height)
{
    for (int i = 0; i < kDisplaySizeCount; ++i)
        if (kDisplaySizes[i].w == width && kDisplaySizes[i].h == height)
            return i;
    return -1;
}

int horplusSizeForCanvas(int width, int height)
{
    for (int i = 0; i < kHorplusSizeCount; ++i)
        if (kHorplusSizes[i].w == width && kHorplusSizes[i].h == height)
            return i;
    return -1;
}

bool readProfileInt(const char* section, const char* key, int* value)
{
    char raw[32] = {};
    GetPrivateProfileStringA(section, key, "\x01", raw, sizeof(raw), discipleIni());
    if (raw[0] == '\x01' && raw[1] == 0)
        return false;

    char* end = nullptr;
    const long parsed = strtol(raw, &end, 10);
    while (end && (*end == ' ' || *end == '\t'))
        ++end;
    if (!end || end == raw || *end)
        return false;
    if (value)
        *value = static_cast<int>(parsed);
    return true;
}

bool profileValuePresent(const char* section, const char* key)
{
    char raw[32] = {};
    GetPrivateProfileStringA(section, key, "\x01", raw,
                             static_cast<DWORD>(sizeof(raw)), discipleIni());
    return !(raw[0] == '\x01' && raw[1] == 0);
}

void readDisplaySize()
{
    int value = 0;
    if (!readProfileInt("Disciple", "DisplaySize", &value) || value < 0 ||
        value >= kDisplaySizeCount) {
        mlog("[menu] invalid/missing Disciple/DisplaySize; previewing the game's default 0");
        value = 0;
    }
    g_displaySizePending = value;
}

void showGameResolutionRestartModal(int width, int height, int nativeDisplaySize)
{
    wchar_t message[640] = {};
    if (nativeDisplaySize >= 0) {
        swprintf_s(
            message,
            L(L"Original game mode %d x %d (DisplaySize %d) was saved. The wide game canvas will be disabled on the next launch.\n\nFully close the game and start it again to apply the change.\n\nWindow/output size and scaling are separate settings; Automatic output will normally match this game canvas pixel-for-pixel.",
              L"Штатный режим игры %d x %d (DisplaySize %d) сохранён. При следующем запуске широкий игровой кадр будет выключен.\n\nЧтобы применить изменение, полностью закройте игру и запустите её снова.\n\nРазмер окна/вывода и масштаб — отдельные настройки; вывод «Авто» обычно совпадёт с игровым кадром пиксель в пиксель."),
            width, height, nativeDisplaySize);
    } else {
        swprintf_s(
            message,
            L(L"Widescreen game canvas %d x %d was saved. On the next launch it replaces the classic DisplaySize canvas and shows more map without stretching.\n\nFully close the game and start it again to apply the change.\n\nWindow/output size and scaling are separate settings; Automatic output will normally match this game canvas pixel-for-pixel.",
              L"Широкий игровой кадр %d x %d сохранён. При следующем запуске он заменит штатный кадр DisplaySize и покажет больше карты без растягивания.\n\nЧтобы применить изменение, полностью закройте игру и запустите её снова.\n\nРазмер окна/вывода и масштаб — отдельные настройки; вывод «Авто» обычно совпадёт с игровым кадром пиксель в пиксель."),
            width, height);
    }
    MessageBoxW(
        g_gameHwnd, message,
        L(L"Game resolution — restart required",
          L"Разрешение игры — нужен перезапуск"),
        MB_OK | MB_ICONINFORMATION);
}

double fitScale(int gameWidth, int gameHeight, int outputWidth,
                int outputHeight);
void formatScale(double scale, wchar_t* text, size_t capacity);

void showAdaptiveResolutionRestartModal(int currentWidth, int currentHeight,
                                        int outputWidth, int outputHeight,
                                        int selectedWidth,
                                        int selectedHeight,
                                        int nativeDisplaySize)
{
    wchar_t currentScale[32] = {};
    wchar_t selectedScale[32] = {};
    formatScale(fitScale(currentWidth, currentHeight,
                         outputWidth, outputHeight),
                currentScale,
                sizeof(currentScale) / sizeof(currentScale[0]));
    formatScale(fitScale(selectedWidth, selectedHeight,
                         outputWidth, outputHeight),
                selectedScale,
                sizeof(selectedScale) / sizeof(selectedScale[0]));

    wchar_t message[768] = {};
    if (nativeDisplaySize >= 0) {
        swprintf_s(
            message,
            L(L"The current %dx%d game canvas is scaled to the %dx%d screen (%s).\n\nFor this 4:3/5:4 screen, Automatic selected the game's stock %dx%d mode (DisplaySize %d, %s). Hor+ widescreen is not needed.\n\nFully close the game and start it again to apply the new game resolution. Automatic mode will recalculate it when the monitor resolution changes.",
              L"Сейчас игровой кадр %dx%d масштабируется на экран %dx%d (%s).\n\nДля этого экрана 4:3/5:4 автоматика выбрала штатный режим игры %dx%d (DisplaySize %d, %s). Широкий Hor+ здесь не нужен.\n\nПолностью закройте игру и запустите её снова, чтобы применить новое разрешение игры. При смене разрешения монитора автоматический режим пересчитает его."),
            currentWidth, currentHeight, outputWidth, outputHeight,
            currentScale, selectedWidth, selectedHeight, nativeDisplaySize,
            selectedScale);
    } else {
        swprintf_s(
            message,
            L(L"The current %dx%d game canvas is scaled to the %dx%d screen (%s).\n\nFor this widescreen display, Automatic selected the largest reviewed Hor+ game canvas with priority for clean integer scaling: %dx%d (%s).\n\nFully close the game and start it again to apply the new game resolution. Automatic mode will recalculate it when the monitor resolution changes.",
              L"Сейчас игровой кадр %dx%d масштабируется на экран %dx%d (%s).\n\nДля этого широкого экрана автоматика выбрала самый крупный проверенный игровой кадр Hor+ с приоритетом чёткого целого масштаба: %dx%d (%s).\n\nПолностью закройте игру и запустите её снова, чтобы применить новое разрешение игры. При смене разрешения монитора автоматический режим пересчитает его."),
            currentWidth, currentHeight, outputWidth, outputHeight,
            currentScale, selectedWidth, selectedHeight, selectedScale);
    }
    g_adaptiveNoticeOutputW = outputWidth;
    g_adaptiveNoticeOutputH = outputHeight;
    g_adaptiveNoticeCanvasW = selectedWidth;
    g_adaptiveNoticeCanvasH = selectedHeight;
    MessageBoxW(
        g_gameHwnd, message,
        L(L"Automatic game resolution — restart required",
          L"Автоматическое разрешение игры — нужен перезапуск"),
        MB_OK | MB_ICONINFORMATION);
}

void migrateLegacyDisplaySize()
{
    // C4dll-R's old renderer accepted only these game-native dimensions through [Wrapper].
    // cnc-ddraw intentionally does not consume them. Import a recognized pair once, without
    // deleting it, and never override a nonzero/invalid explicit [Disciple] DisplaySize.
    int migrated = 0;
    if (readProfileInt("Wrapper", "LegacyDisplaySizeMigrated", &migrated) && migrated != 0)
        return;

    int width = 0, height = 0;
    if (!readProfileInt("Wrapper", "DisplayWidth", &width) ||
        !readProfileInt("Wrapper", "DisplayHeight", &height))
        return;

    const int legacySize = displaySizeForCanvas(width, height);
    if (legacySize < 0)
        return;

    int configured = 0;
    const bool hasConfiguredValue =
        profileValuePresent("Disciple", "DisplaySize");
    const bool configuredValid =
        readProfileInt("Disciple", "DisplaySize", &configured);
    // Preserve an explicit malformed value as well as every explicit nonzero mode. Migration is
    // allowed only when the key is absent or it is the unambiguous stock default "0".
    if ((hasConfiguredValue &&
         (!configuredValid || configured < 0 ||
          configured >= kDisplaySizeCount)) ||
        (configuredValid && configured != 0))
        return;

    char raw[8] = {};
    wsprintfA(raw, "%d", legacySize);
    if (!WritePrivateProfileStringA("Disciple", "DisplaySize", raw, discipleIni())) {
        mlog("[menu] legacy DisplayWidth/Height migration failed to write DisplaySize");
        return;
    }

    WritePrivateProfileStringA("Wrapper", "LegacyDisplaySizeMigrated", "1", discipleIni());
    mlog("[menu] imported legacy DisplayWidth/Height=%dx%d as DisplaySize=%d (one time)",
         width, height, legacySize);
}

void readEditorDatabase()
{
    g_editorDatabase =
        GetPrivateProfileIntA("Disciple", "ScenEditDatabase", 0, discipleIni()) != 0 ? 1 : 0;
}

void writeEditorDatabase(int value)
{
    g_editorDatabase = value ? 1 : 0;
    WritePrivateProfileStringA("Disciple", "ScenEditDatabase",
                               g_editorDatabase ? "1" : "0", discipleIni());
}

// In-memory GameSettings (Russobit). CMidgardApi::instance() @0x401d35 (__cdecl) -> CMidgard.data @+8
// -> CMidgardData.settings @+60 is GameSettings**; fields: playerSpeed @+360, opponentSpeed @+364,
// battleSpeed @+372 (struct size 468).
char* gameSettings()
{
    if (g_ver != VerRussobit)
        return nullptr;
    auto instance = reinterpret_cast<char*(__cdecl*)()>(0x401d35);
    char* mid = instance();
    if (!mid)
        return nullptr;
    char* data = *reinterpret_cast<char**>(mid + 8);
    if (!data)
        return nullptr;
    char** pps = *reinterpret_cast<char***>(data + 60); // GameSettings** stored at data+60
    if (!pps)
        return nullptr;
    return *pps; // GameSettings*
}

static bool isUserPtr(const void* p)
{
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x7FFF0000;
}

// Shared diagnostics gate for the other feature TUs (timerhost tlog, pluginhost plog).
extern "C" int featuremenu_debug_enabled(void)
{
    return debugLogEnabled() ? 1 : 0;
}

// Battle state for the timer plugin's Combat Pause (g_inBattle = the 0x6F4294 vftable discriminator).
extern "C" int featuremenu_in_battle(void)
{
    return g_inBattle ? 1 : 0;
}

// Read CScenarioInfo.currentTurn from an IMidgardObjectMap: vftable[1] getId() -> scenario id, force
// id type to ScenarioInfo (27), vftable[5] findScenarioObjectById() -> CScenarioInfo, currentTurn @+32.
// Returns -1 on any bad pointer / out-of-range day. Caller is SEH-guarded.
static int dayFromObjectMap(void** objectMap)
{
    if (!isUserPtr(objectMap)) return -1;
    void** vft = *reinterpret_cast<void***>(objectMap); // IMidgardObjectMap::vftable
    if (!isUserPtr(vft)) return -1;
    using GetIdFn = const int*(__thiscall*)(void*);                  // vftable[1] getId
    const int* scenarioId = reinterpret_cast<GetIdFn>(vft[1])(objectMap);
    if (!isUserPtr(scenarioId)) return -1;
    const unsigned sv = static_cast<unsigned>(*scenarioId);
    const int infoId = static_cast<int>((sv & 0xFFC00000u) | 0x001B0000u); // keep cat+catIdx, type=27
    using FindFn = char*(__thiscall*)(void*, const int*);            // vftable[5] findScenarioObjectById
    char* obj = reinterpret_cast<FindFn>(vft[5])(objectMap, &infoId);
    if (!isUserPtr(obj)) return -1;
    const int day = *reinterpret_cast<int*>(obj + 32); // CScenarioInfo.currentTurn
    return (day < 0 || day > 100000) ? -1 : day;
}

// Heavy walk: read CScenarioInfo.currentTurn. SERVER map FIRST (authoritative; on the host the client
// cache can lag a day at a boundary), then the CLIENT map (the only one a pure-network joiner has).
// Russobit-only, SEH-guarded, READ-ONLY. MUST run on the GAME thread (calls game virtuals on the live
// object map); the result is cached. From CMidgard(instance)->data@+8:
//   server: CMidgardData.server@+44 -> data@+12 -> CMidServerLogic@+28 -> coreData@+4 -> objectMap@+20
//   client: CMidgardData.client@+40 -> CMidClientCore.data@+8 -> dataCache@+8 (core@+4 so core.data == client+8)
static int computeCurrentDay()
{
    if (g_ver != VerRussobit)
        return -1;
    int day = -1;
    __try {
        char* mid = reinterpret_cast<char*(__cdecl*)()>(0x401d35)();
        if (!isUserPtr(mid)) return -1;
        char* data = *reinterpret_cast<char**>(mid + 8);
        if (!isUserPtr(data)) return -1;

        // SERVER chain first (host): authoritative, no client-cache lag.
        char* server = *reinterpret_cast<char**>(data + 44); // CMidgardData.server
        if (isUserPtr(server)) {
            char* sdata = *reinterpret_cast<char**>(server + 12);
            if (isUserPtr(sdata)) {
                char* logic = *reinterpret_cast<char**>(sdata + 28); // CMidServerLogic
                if (isUserPtr(logic)) {
                    char* coreData = *reinterpret_cast<char**>(logic + 4); // CMidServerLogicCore::coreData
                    if (isUserPtr(coreData))
                        day = dayFromObjectMap(*reinterpret_cast<void***>(coreData + 20)); // objectMap
                }
            }
        }
        // CLIENT chain (a pure-network joiner has no server).
        if (day < 0) {
            char* client = *reinterpret_cast<char**>(data + 40); // CMidgardData.client
            if (isUserPtr(client)) {
                char* coreData = *reinterpret_cast<char**>(client + 8); // CMidClientCore.data (core@+4 -> +4)
                if (isUserPtr(coreData))
                    day = dayFromObjectMap(*reinterpret_cast<void***>(coreData + 8)); // CMidClientCoreData.dataCache
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return day;
}

static volatile LONG g_cachedDay = -1; // last computed day; refreshed on the GAME thread, read anywhere

// Refresh the cached day. MUST run on the game thread (computeCurrentDay calls game virtuals; running it
// from the worker raced the game mutating the map). Called from the WM_TIMER pump.
extern "C" void featuremenu_refresh_day(void) { g_cachedDay = computeCurrentDay(); }
// Cheap cross-thread accessor for the cached day.
extern "C" int featuremenu_current_day(void) { return (int)g_cachedDay; }

// Is it the LOCAL player's turn? Reads CPhaseGameData.clientTakesTurn - the game's own "this client acts
// now" flag, set per turn and IMMUNE to sub-dialogs (city/diplomacy/spellbook/etc.). 1/0, or -1 if
// unavailable (menu/loading/layout mismatch) so the caller falls back. Russobit-only, SEH-guarded, raw
// reads only (no game calls). Chain: CMidgard(0x401d35)->data@+8 -> client@+40 -> CMidClientData@+12 ->
// CPhase@+0 -> CPhaseGame.data (phase == CPhaseGame+8, so data == *(phase+8)) -> clientTakesTurn@+40;
// validated by CPhaseGameData.midClient@+36 == client (confirms the phase really is the CPhaseGame).
extern "C" int featuremenu_my_turn(void)
{
    if (g_ver != VerRussobit)
        return -1;
    int r = -1;
    __try {
        char* mid = reinterpret_cast<char*(__cdecl*)()>(0x401d35)();
        if (!isUserPtr(mid)) return -1;
        char* data = *reinterpret_cast<char**>(mid + 8);
        if (!isUserPtr(data)) return -1;
        char* client = *reinterpret_cast<char**>(data + 40);        // CMidgardData.client
        if (!isUserPtr(client)) return -1;
        char* clientData = *reinterpret_cast<char**>(client + 12);  // CMidClient.data
        if (!isUserPtr(clientData)) return -1;
        char* phase = *reinterpret_cast<char**>(clientData);        // CMidClientData.phase (CPhase*)
        if (!isUserPtr(phase)) return -1;
        char* pg = *reinterpret_cast<char**>(phase + 8);            // CPhaseGame.data
        if (!isUserPtr(pg)) return -1;
        if (*reinterpret_cast<char**>(pg + 36) != client) return -1; // consistency: CPhaseGameData.midClient
        r = *reinterpret_cast<unsigned char*>(pg + 40) ? 1 : 0;     // clientTakesTurn
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return r;
}

// X005TA0285 ("Do you want to hire this unit?") is created at the sole Russobit
// call site 0x503990 (`mov ecx,edi; call [eax+20h]`). The intercepted call
// already has the game's generic BTN_YES functor as argument 3. When explicitly
// enabled, invoke that functor with okPressed=true and destroy it normally; no
// mouse coordinates, resources or stack data are synthesized.
//
// Disabled/off-turn/invalid-state paths tail-jump to the untouched message-box
// virtual method with the original four arguments.
__declspec(naked) void unitHireConfirmThunk()
{
    __asm {
        cmp byte ptr [g_autoConfirmUnitHire], 0
        je manual
        call featuremenu_my_turn
        cmp eax, 1
        jne manual
        mov ecx, dword ptr [esp + 0x0C] // generic BTN_YES functor (argument 3)
        test ecx, ecx
        je manual
        mov eax, dword ptr [ecx]
        push 1                          // okPressed
        push 0                          // unused sender
        call dword ptr [eax + 4]        // functor->invoke(nullptr, true)
        mov ecx, dword ptr [esp + 0x0C]
        mov eax, dword ptr [ecx]
        push 1
        call dword ptr [eax]            // virtual destructor(delete=true)
        xor eax, eax
        ret 0x10                        // original virtual call has four arguments
    manual:
        mov ecx, edi
        mov eax, dword ptr [edi]
        jmp dword ptr [eax + 0x20]      // original message-box creation
    }
}

void installUnitHireConfirmHook()
{
    constexpr uintptr_t siteVa = 0x503990;
    static const std::uint8_t expected[5] = {0x8B, 0xCF, 0xFF, 0x50, 0x20};
    const auto* site = reinterpret_cast<const std::uint8_t*>(siteVa);
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        mlog("[hire-confirm] unexpected bytes at %#x; hook not installed", (unsigned)siteVa);
        return;
    }
    std::uint8_t call[5] = {0xE8, 0, 0, 0, 0};
    const std::int32_t rel = static_cast<std::int32_t>(
        reinterpret_cast<uintptr_t>(&unitHireConfirmThunk) - (siteVa + sizeof(call)));
    memcpy(call + 1, &rel, sizeof(rel));
    if (writeBytes(siteVa, call, sizeof(call)))
        mlog("[hire-confirm] hook installed (default %s)",
             g_autoConfirmUnitHire ? "on" : "off");
    else
        mlog("[hire-confirm] hook installation FAILED");
}

// Current turn player for the timer's host-driven turn detection (pluginhost.cpp polls this off-thread).
// CMidgard(instance@0x401d35)->data@+8 -> server@+44 -> data@+12 -> serverLogic@+28 ->
// currentPlayerIndex@+244. Advances on every turn change incl. a SKIPPED turn. -1 for a pure network
// client (no in-process server). SEH-guarded; *outInGame=1 when a player is read.
extern "C" int featuremenu_server_player(int* outInGame)
{
    if (outInGame)
        *outInGame = 0;
    if (g_ver != VerRussobit)
        return -1;
    int player = -1;
    __try {
        auto instance = reinterpret_cast<char*(__cdecl*)()>(0x401d35);
        char* mid = instance();
        if (!isUserPtr(mid))
            return -1;
        char* data = *reinterpret_cast<char**>(mid + 8);
        if (!isUserPtr(data))
            return -1;
        char* server = *reinterpret_cast<char**>(data + 44); // CMidgardData.server
        if (!isUserPtr(server))
            return -1; // no in-process server (pure network client) -> unknown here
        char* sdata = *reinterpret_cast<char**>(server + 12); // CMidServer.data
        if (!isUserPtr(sdata))
            return -1;
        char* logic = *reinterpret_cast<char**>(sdata + 28); // CMidServerData.serverLogic
        if (!isUserPtr(logic))
            return -1;
        player = *reinterpret_cast<int*>(logic + 244); // CMidServerLogic.currentPlayerIndex
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (player < 0 || player > 64)
        return -1;
    if (outInGame)
        *outInGame = 1;
    return player;
}

// Apply a native speed to in-memory GameSettings (takes effect next battle/turn) + persist to
// Disciple.ini. which: 0 = battle, 1 = map (player + opponent). Runs on the game UI thread.
void setNativeSpeed(int which, int value)
{
    char* gs = gameSettings();

    char b[8];
    wsprintfA(b, "%d", value);
    if (which == 0) {
        if (gs)
            *reinterpret_cast<int*>(gs + 372) = value; // battleSpeed
        WritePrivateProfileStringA("Settings", "BattleSpeed", b, discipleIni());
    } else {
        if (gs) {
            *reinterpret_cast<int*>(gs + 360) = value; // playerSpeed
            *reinterpret_cast<int*>(gs + 364) = value; // opponentSpeed
        }
        WritePrivateProfileStringA("Settings", "PlayerSpeed", b, discipleIni());
        WritePrivateProfileStringA("Settings", "OpponentSpeed", b, discipleIni());
    }
    mlog("[menu] native %s speed = %d (gameSettings=%p)", which == 0 ? "battle" : "map", value,
         reinterpret_cast<void*>(gs));
}

// Keep the native cloud option's live GameSettings byte in sync with Disciple.ini. Without this,
// a normal game shutdown can serialize the old in-memory value over the menu's persisted choice.
// Visibility is still presented as restart-only until changing it on an already open map has been
// verified across every supported scenario state.
void setNativeCloudVisibility(int enabled)
{
    char* gs = gameSettings();
    if (gs)
        *reinterpret_cast<unsigned char*>(gs + 400) = enabled ? 1 : 0; // GameSettings::isoBirds
    mlog("[clouds] native GameSettings visibility=%d (gameSettings=%p)", enabled ? 1 : 0,
         reinterpret_cast<void*>(gs));
}

void readNativeSpeeds()
{
    g_battleSpeed = GetPrivateProfileIntA("Settings", "BattleSpeed", 2, discipleIni());
    if (g_battleSpeed < 1)
        g_battleSpeed = 1;
    if (g_battleSpeed > 4)
        g_battleSpeed = 4;
    g_mapSpeed = GetPrivateProfileIntA("Settings", "PlayerSpeed", 1, discipleIni());
    if (g_mapSpeed < 1)
        g_mapSpeed = 1;
    if (g_mapSpeed > 3)
        g_mapSpeed = 3;
}

// Battle idle/attack split. Idle keeps the battle base factor (vanilla unless "Battle animation" is set),
// so waiting units don't twitch. showAttackEffect and the final CAnimCounter callback publish the
// latest visual event; the WM_TIMER pump drives the higher factor only between them.
const DWORD kAttackRampMs = 300; // starts at the exact visual end; ~10 smooth steps at the 32ms pump
const DWORD kAttackWatchdogMs = 5000; // emergency only: never leave burst stuck if an end callback is lost

DWORD attackHoldMs()
{
    // Signature-mismatch fallback only: keep roughly 1.5 seconds of VIRTUAL attack time.
    const DWORD factor = static_cast<DWORD>(kAnimFactor[g_battleAttackSpeed - 1]); // x10
    DWORD ms = 15000 / factor;
    if (ms < 100)
        ms = 100;
    if (ms > 1000)
        ms = 1000;
    return ms;
}

// Eased burst factor (x10): exact visual-active state stays at attack speed; the final animation
// callback starts a 300ms linear return. A future expiry is the old timer-only fallback.
int easedBurstFactor(int idle)
{
    if (!g_inBattle || (!g_attackVisualActive && !g_attackExpiryTick))
        return idle;
    const int atk = kAnimFactor[g_battleAttackSpeed - 1];
    if (g_attackVisualActive)
        return atk < idle ? idle : atk;
    const int since = static_cast<int>(GetTickCount() - g_attackExpiryTick);
    int f = idle;
    if (since < 0)
        f = atk;
    else if (since < static_cast<int>(kAttackRampMs))
        f = atk + (idle - atk) * since / static_cast<int>(kAttackRampMs);
    return f < idle ? idle : f;
}

// EXPERIMENTAL per-unit burst. The actual speed-up happens in batUnitAnimUpdateThunk (the patched per-unit
// CBatUnitAnim::update, runs every game frame). Here, from the 32ms pump, we just FEED that hook: which unit
// is acting (viewer's current unit id @+3996) and how much extra to decrement its countdown. The hook itself
// catches every frame, so the transient countdown is never missed (polling here did miss it).
const unsigned kFactoryImageAnimVft = 0x6E2FCC; // FactoryImageAnim full-object vftable (RTTI-confirmed)

// Advance ONE unit's sprite extra frames: scan its CBatUnitAnimData for a FactoryImageAnim* (vft match) and
// call FactoryImageAnim::update (0x52EA1F, advances currentFrame by 1, no time gate) `extra` times. Per-unit,
// no global clock touched. Guarded: only a confirmed FactoryImageAnim is touched.
const unsigned kFactoryImageAnimVft0 = 0x6E2FBC; // FactoryImageAnim's IMqAnimation sub-object vftable (at +4)

// True if p is a valid FactoryImageAnim (both vtables + sane state); advances it `extra` frames.
static bool tryAdvanceFactoryImageAnim(char* p, int extra, bool log, int o, int q)
{
    using UpdateFn = void(__stdcall*)(void*);
    static UpdateFn spriteUpdate = reinterpret_cast<UpdateFn>(0x52EA1F);
    if (!isUserPtr(p) || IsBadReadPtr(p, 40))
        return false;
    if (*reinterpret_cast<unsigned*>(p) != kFactoryImageAnimVft
        || *reinterpret_cast<unsigned*>(p + 4) != kFactoryImageAnimVft0)
        return false;
    const unsigned char paused = *reinterpret_cast<unsigned char*>(p + 36);
    const int cf = *reinterpret_cast<int*>(p + 32);
    if (paused > 1 || cf < 0 || cf > 4096)
        return false;
    if (log)
        mlog("[sprite] +0x%X.%d frame=%d paused=%d", o, q, cf, paused);
    for (int i = 0; i < extra; ++i)
        spriteUpdate(p + 4);
    return true;
}

// Advance ALL of a unit's FactoryImageAnims (body/portrait/effect), scanning the data AND one level into
// each pointer (SmartPtr/wrapper). Returns how many it touched.
int speedUnitSprite(char* ad, int extra, bool log)
{
    int n = 0;
    for (int o = 0; o < 4136; o += 4) {
        char* p = *reinterpret_cast<char**>(ad + o);
        if (!isUserPtr(p) || IsBadReadPtr(p, 64))
            continue;
        if (tryAdvanceFactoryImageAnim(p, extra, log, o, -4)) { // p is the FactoryImageAnim directly
            ++n;
            continue;
        }
        for (int q = 0; q <= 16; q += 4) { // p holds a FactoryImageAnim* in its first fields (SmartPtr)
            char* fa = *reinterpret_cast<char**>(p + q);
            if (tryAdvanceFactoryImageAnim(fa, extra, log, o, q)) {
                ++n;
                break;
            }
        }
    }
    return n;
}

// --- idle-slow: slow ONLY the idle frame clock (sub_548E10), leaving the action path (sub_648645) at
// normal speed. Idle units calm down; the attacker + units taking the hit (action path) stay normal-fast,
// so they visually stand out. Gated by g_perUnitBurst (the menu toggle) + g_inBattle.
void* g_origIdleClock = nullptr;    // Detours trampoline to sub_548E10 (clock->frame mapper)
volatile LONG g_idleClockCalls = 0; // diag: per-frame call rate proves this is the idle driver
int g_idleSlow = 1;                 // extra-calm multiplier on top of the auto-cancel (1 = idle exactly vanilla)

int __stdcall idleClockHook(int a1)
{
    InterlockedIncrement(&g_idleClockCalls);
    static int logged = 0;
    if (g_inBattle && logged < 24) { // find WHO calls it per-frame (return-address distribution)
        ++logged;
        mlog("[idle] sub_548E10 ret=%p a1=%d", _ReturnAddress(), a1);
    }
    int in = a1;
    // Cancel the global battle speedup on the idle path only: divide by g_battleFactor/10 so idle nets to
    // ~vanilla while the action path (sub_648645, untouched) keeps the full x15. g_idleSlow is a manual
    // extra-calm multiplier on top (1 = idle exactly vanilla).
    int slow = (g_battleFactor / 10) * (g_idleSlow < 1 ? 1 : g_idleSlow);
    if (g_inBattle && g_perUnitBurst && slow > 1 && a1 > slow)
        in = a1 / slow; // slow the idle animation clock -> idle frames advance slower
    return reinterpret_cast<int(__stdcall*)(int)>(g_origIdleClock)(in);
}

void installIdleSlowDetour()
{
    if (g_ver != VerRussobit)
        return;
    g_origIdleClock = reinterpret_cast<void*>(0x548E10);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_origIdleClock, reinterpret_cast<void*>(idleClockHook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_origIdleClock = reinterpret_cast<void*>(0x548E10);
        mlog("[menu] idle-slow detour FAILED (0x548E10)");
    } else {
        mlog("[menu] idle-slow detour installed (sub_548E10, slow=%d)", g_idleSlow);
    }
}

// --- FactoryImageAnim::update probe (sub_52EA1F = void __stdcall(int a1); a1=IMqAnimation sub-object,
// full obj = a1-4, currentFrame @a1+28, paused @a1+32). This is the no-time-gate per-frame sprite advancer.
// DECISIVE test: do global-list entries carry unit identity? Rich logging: object dump, unit-id scan
// (0xA3E4xxxx CMidgardID pattern), distinct-caller set, and a pseudo-callstack (stack scan for code addrs).
// Detailed dump re-armed on each attack pulse so we capture idle frames AND attack frames.
void* g_origFiaUpdate = nullptr;
volatile LONG g_fiaCalls = 0;
volatile LONG g_fiaDetailed = 0; // re-armed to 0 on attack pulse; hook dumps while < kFiaDumpMax
const LONG kFiaDumpMax = 90;

// Idle-FIA set: FIAs (full-object ptr) reached from IDLE-display units (+4064 vtable 0x6F4964), rebuilt by
// pollBattleUnits each ~150ms. The tick slows only these. Poll + tick both on the game thread (no race).
// FIAs (full-object ptr) reached from ACTING-display units (+4064 vtable 0x6F4964 = a unit currently
// playing an action: attacker + units taking the hit). Idle units use 0x6E2F8C/0x6F492C with NO reachable
// FIA, so they never enter this set. Rebuilt by pollBattleUnits each ~150ms. The tick SPEEDS these.
unsigned g_playFia[96];
DWORD g_playFiaExp[96]; // per-entry expiry tick (sticky TTL so brief action windows hold)
volatile LONG g_playFiaN = 0;
static bool isPlayFia(unsigned full)
{
    const LONG n = g_playFiaN;
    for (LONG i = 0; i < n; ++i)
        if (g_playFia[i] == full)
            return true;
    return false;
}

static void fiaScanUnitIds(const int* base, int dwords, const char* tag)
{
    __try {
        for (int i = 0; i < dwords; ++i) {
            unsigned v = static_cast<unsigned>(base[i]);
            if ((v & 0xFFFF0000) == 0xA3E40000) // CMidgardID unit pattern observed this scenario
                mlog("    [uid?] %s +0x%X = %08X", tag, i * 4, v);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// True while a hit/effect is visually active or in its short return ramp. Exact builds use the
// showAttackEffect start plus CAnimCounter final-zero end; signature mismatch uses a timed fallback.
static bool inAttackWindow()
{
    return g_attackVisualActive ||
           (g_attackExpiryTick &&
            static_cast<int>(GetTickCount() - g_attackExpiryTick) <
                static_cast<int>(kAttackRampMs));
}

const int kIdleDiv = 4; // idle/between-hits animation speed = 1/kIdleDiv (1/4); attacks run full speed

int __stdcall fiaUpdateHook(int a1)
{
    const LONG n = InterlockedIncrement(&g_fiaCalls);
    // (FIA-skip idle-slow reverted: skipping frames on the shared FIA tick breaks the cursor + target ring,
    // which are also FIAs and MUST run at full rate; and FIAs carry no unit identity to slow selectively.)
    if (g_inBattle) {
        __try {
            void** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());
            const unsigned caller = reinterpret_cast<unsigned>(sp[0]);
            // distinct immediate-caller set: answers "is it sub_51EB10 directly, or via a wrapper?"
            static unsigned seen[32];
            static int nseen = 0;
            bool isNew = true;
            for (int i = 0; i < nseen; ++i)
                if (seen[i] == caller) { isNew = false; break; }
            if (isNew && nseen < 32) {
                seen[nseen++] = caller;
                mlog("[fia] NEW caller=%08X (at call #%ld)", caller, n);
            }
            // Distinct-object table: dump every UNIQUE sprite once (full fields +0..+60), flag uid patterns.
            // This shows ALL list entries (~31), not just the first N calls, so we see who carries a unit ref.
            const int full = a1 - 4;
            static unsigned objs[80];
            static int nobjs = 0;
            bool newObj = true;
            for (int i = 0; i < nobjs; ++i)
                if (objs[i] == static_cast<unsigned>(full)) { newObj = false; break; }
            if (newObj && nobjs < 80) {
                objs[nobjs++] = static_cast<unsigned>(full);
                char line[256];
                int off = 0;
                off += _snprintf(line + off, sizeof(line) - off, "[fiaobj] #%d this=%08X", nobjs, static_cast<unsigned>(full));
                for (int d = 0; d <= 15; ++d) { // +0..+60
                    unsigned v = *reinterpret_cast<unsigned*>(full + d * 4);
                    const char* mark = ((v & 0xFFFF0000) == 0xA3E40000) ? "*" : "";
                    off += _snprintf(line + off, sizeof(line) - off, " +%d=%08X%s", d * 4, v, mark);
                }
                mlog("%s", line);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return reinterpret_cast<int(__stdcall*)(int)>(g_origFiaUpdate)(a1);
}

void installFiaProbe()
{
    if (g_ver != VerRussobit)
        return;
    g_origFiaUpdate = reinterpret_cast<void*>(0x52EA1F);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_origFiaUpdate, reinterpret_cast<void*>(fiaUpdateHook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_origFiaUpdate = reinterpret_cast<void*>(0x52EA1F);
        mlog("[menu] FIA probe FAILED (0x52EA1F)");
    } else {
        mlog("[menu] FIA probe installed (0x52EA1F)");
    }
}

// --- unit-update probe: sub_655006 = __thiscall(this=CBatUnitAnim, char a2, char a3). data=*(this+4);
// uid@+12, state flags @+35/+38/+39, display image @+4064. This is where a unit (state known) resolves
// its display. DECISIVE for tagging: (a) is it per-frame (rate), (b) can we reach the unit's FIA (0x6E2FCC)
// from its data here? If yes -> tag {FIA->idle?} on state, then slow idle-tagged in the tick.
void* g_origUnitUpd = nullptr;
volatile LONG g_unitUpdCalls = 0;
volatile LONG g_unitDetailed = 0;

int __fastcall unitUpdHook(void* thisptr, void* /*edx*/, char a2, char a3)
{
    InterlockedIncrement(&g_unitUpdCalls);
    if (g_inBattle && g_unitDetailed < 60) {
        __try {
            char* data = *reinterpret_cast<char**>(reinterpret_cast<char*>(thisptr) + 4);
            if (isUserPtr(data) && !IsBadReadPtr(data, 4140)) {
                const int uid = *reinterpret_cast<int*>(data + 12);
                char* img = *reinterpret_cast<char**>(data + 4064);
                const unsigned imgvft =
                    (isUserPtr(img) && !IsBadReadPtr(img, 4)) ? *reinterpret_cast<unsigned*>(img) : 0;
                // find a FIA (vtable 0x6E2FCC) reachable from the unit data: direct ptr, or one level deep
                int fiaOff = -1;
                unsigned fiaPtr = 0;
                for (int o = 0; o < 4136; o += 4) {
                    char* p = *reinterpret_cast<char**>(data + o);
                    if (!isUserPtr(p) || IsBadReadPtr(p, 8))
                        continue;
                    if (*reinterpret_cast<unsigned*>(p) == 0x6E2FCC) { fiaOff = o; fiaPtr = reinterpret_cast<unsigned>(p); break; }
                    char* q = *reinterpret_cast<char**>(p);
                    if (isUserPtr(q) && !IsBadReadPtr(q, 4) && *reinterpret_cast<unsigned*>(q) == 0x6E2FCC) {
                        fiaOff = o | 0x10000; fiaPtr = reinterpret_cast<unsigned>(q); break;
                    }
                }
                InterlockedIncrement(&g_unitDetailed);
                mlog("[unit] uid=%08X a2=%d a3=%d f35=%d f38=%d f39=%d img=%08X imgvft=%08X fiaOff=%X fiaPtr=%08X",
                     static_cast<unsigned>(uid), a2, a3, *reinterpret_cast<unsigned char*>(data + 35),
                     *reinterpret_cast<unsigned char*>(data + 38), *reinterpret_cast<unsigned char*>(data + 39),
                     reinterpret_cast<unsigned>(img), imgvft, fiaOff, fiaPtr);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return reinterpret_cast<int(__fastcall*)(void*, void*, char, char)>(g_origUnitUpd)(thisptr, nullptr, a2, a3);
}

void installUnitProbe()
{
    if (g_ver != VerRussobit)
        return;
    g_origUnitUpd = reinterpret_cast<void*>(0x655006);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_origUnitUpd, reinterpret_cast<void*>(unitUpdHook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_origUnitUpd = reinterpret_cast<void*>(0x655006);
        mlog("[menu] unit probe FAILED (0x655006)");
    } else {
        mlog("[menu] unit probe installed (0x655006)");
    }
}

// --- animation-NAME probe: sub_52E355 = int __stdcall(char* Str, int a2, int a3). Str = the image/anim
// frame NAME loaded for an animation. This is the "level higher" where idle-vs-attack is encoded by name.
// Log every DISTINCT name seen, to verify whether unit idle/attack/hit are distinguishable by name pattern.
void* g_origName355 = nullptr;

int __stdcall name355Hook(char* Str, int a2, int a3)
{
    __try {
        if (isUserPtr(Str) && !IsBadReadPtr(Str, 4)) {
            // printable, length 3..31
            int len = 0;
            bool printable = true;
            while (len < 32) {
                char c = Str[len];
                if (c == 0)
                    break;
                if (c < 0x20 || static_cast<unsigned char>(c) > 0x7E) { printable = false; break; }
                ++len;
            }
            if (printable && len >= 3 && len < 32) {
                static char seen[256][32];
                static int nseen = 0;
                bool isNew = true;
                for (int i = 0; i < nseen; ++i)
                    if (lstrcmpA(seen[i], Str) == 0) { isNew = false; break; }
                if (isNew && nseen < 256) {
                    lstrcpynA(seen[nseen], Str, 32);
                    ++nseen;
                    // callstack from this (safe, working) __stdcall hook: code addresses up the stack reveal
                    // the requesting unit/state path. Captured for the first batch of distinct names.
                    char cs[180];
                    int co = 0, nf = 0;
                    cs[0] = 0;
                    if (nseen <= 60) {
                        void** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());
                        for (int i = 0; i < 240 && nf < 16; ++i) {
                            const unsigned a = reinterpret_cast<unsigned>(sp[i]);
                            if (a >= 0x401000 && a < 0x6D5000)
                                co += _snprintf(cs + co, sizeof(cs) - co, " %08X", a), ++nf;
                        }
                        cs[co] = 0;
                    }
                    mlog("[name] #%d \"%s\" cs:%s", nseen, Str, cs);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return reinterpret_cast<int(__stdcall*)(char*, int, int)>(g_origName355)(Str, a2, a3);
}

void installNameProbe()
{
    if (g_ver != VerRussobit)
        return;
    g_origName355 = reinterpret_cast<void*>(0x52E355);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_origName355, reinterpret_cast<void*>(name355Hook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_origName355 = reinterpret_cast<void*>(0x52E355);
        mlog("[menu] name probe FAILED (0x52E355)");
    } else {
        mlog("[menu] name probe installed (0x52E355)");
    }
}

// --- FIA-origin probe: hook the FIA constructor sub_52E8AC (__thiscall, this=ecx = the tickable FIA,
// vtable 0x6E2FCC). At birth, capture {FIA pointer + code-address callstack + printable name strings on
// the stack}. The callstack reveals the requesting unit/state path (idle vs action) which I trace in IDA;
// the names show the animation identity. This links the anonymous tick entry back to its origin.
void* g_origFiaCtor = nullptr;

int __fastcall fiaCtorHook(void* ecx, void* edx, int arg0)
{
    __try {
        static int n = 0;
        if (n < 160) {
            ++n;
            void** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());
            char cs[180];
            int co = 0, nf = 0;
            char nm[180];
            int no = 0, nn = 0;
            nm[0] = 0;
            for (int i = 0; i < 240; ++i) {
                const unsigned a = reinterpret_cast<unsigned>(sp[i]);
                if (nf < 14 && a >= 0x401000 && a < 0x6D5000)
                    co += _snprintf(cs + co, sizeof(cs) - co, " %08X", a), ++nf;
                if (nn < 4 && isUserPtr(reinterpret_cast<void*>(a)) && !IsBadReadPtr(reinterpret_cast<void*>(a), 6)) {
                    char* s = reinterpret_cast<char*>(a);
                    int L = 0;
                    bool ok = true;
                    while (L < 24) { char c = s[L]; if (!c) break; if (c < 0x20 || static_cast<unsigned char>(c) > 0x7E) { ok = false; break; } ++L; }
                    if (ok && L >= 4 && L < 24)
                        no += _snprintf(nm + no, sizeof(nm) - no, " \"%s\"", s), ++nn;
                }
            }
            cs[co] = 0;
            mlog("[fiamap] #%d fia=%08X cs:%s names:%s", n, reinterpret_cast<unsigned>(ecx), cs, nm);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return reinterpret_cast<int(__fastcall*)(void*, void*, int)>(g_origFiaCtor)(ecx, edx, arg0);
}

void installFiaCtorProbe()
{
    if (g_ver != VerRussobit)
        return;
    g_origFiaCtor = reinterpret_cast<void*>(0x52E8AC);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_origFiaCtor, reinterpret_cast<void*>(fiaCtorHook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_origFiaCtor = reinterpret_cast<void*>(0x52E8AC);
        mlog("[menu] FIA-ctor probe FAILED (0x52E8AC)");
    } else {
        mlog("[menu] FIA-ctor probe installed (0x52E8AC)");
    }
}

// --- IDLE-SLOW (the real lever). Idle-breathing units have display +4064 = wrapper vtable 0x6E2F8C; its
// per-frame advance is sub_5B7C60 (forwards to the inner animator at *(+4064+4), which advances a counter).
// Acting/other units use different wrappers. So hook sub_5B7C60 and, ONLY for 0x6E2F8C wrappers, skip most
// advances -> idle animation slows while attacks (other wrappers) stay full speed. Reachable, stable, no poll.
const unsigned kIdleWrapVft = 0x6E2F8C;
void* g_orig5B7C60 = nullptr;
volatile LONG g_idleAdvCalls = 0;
volatile LONG g_idleAdvSkipped = 0;

int __fastcall idleAdvHook(void* thisp, void* edx, int a2)
{
    // (idle-adv skip reverted too: same root issue - 0x6E2F8C is shared by UI, and frame-skip is jittery.)
    return reinterpret_cast<int(__fastcall*)(void*, void*, int)>(g_orig5B7C60)(thisp, edx, a2);
}

void installIdleAdvHook()
{
    if (g_ver != VerRussobit)
        return;
    g_orig5B7C60 = reinterpret_cast<void*>(0x5B7C60);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_orig5B7C60, reinterpret_cast<void*>(idleAdvHook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_orig5B7C60 = reinterpret_cast<void*>(0x5B7C60);
        mlog("[menu] idle-adv hook FAILED (0x5B7C60)");
    } else {
        mlog("[menu] idle-adv hook installed (0x5B7C60)");
    }
}

const unsigned kFiaVft = 0x6E2FCC;
const unsigned kImgPlayVft = 0x6F4964;
const unsigned kImgActVft = 0x6F492C;

// CORRECTED per-unit probe using the toolset header offsets (CBatUnitAnimData): attacker @3968,
// unitPosition @3972, animCounter @3984, ptr1/ptr2/ptr3 SmartPointers @4036/4044/4052. These are the REAL
// per-unit animation fields (the +4064 I used before was unknown4 padding). Dump them + the objects' vtables.
void pollBattleUnits(void)
{
    static DWORD sLast = 0;
    if (!g_inBattle || (GetTickCount() - sLast) < 600)
        return;
    __try {
        char* viewer = reinterpret_cast<char*>(g_batViewer);
        if (!isUserPtr(viewer))
            return;
        char* vdata = *reinterpret_cast<char**>(viewer + 4);
        if (!isUserPtr(vdata))
            return;
        sLast = GetTickCount();
        static const int kBase[2] = {5032, 5056};
        for (int grp = 0; grp < 2; ++grp) {
            for (int k = 0; k < 6; ++k) {
                char* anim = *reinterpret_cast<char**>(vdata + kBase[grp] + 4 * k);
                if (!isUserPtr(anim) || IsBadReadPtr(anim, 8))
                    continue;
                char* d = *reinterpret_cast<char**>(anim + 4);
                if (!isUserPtr(d) || IsBadReadPtr(d, 4136))
                    continue;
                const int uid = *reinterpret_cast<int*>(d + 12);
                if (!uid)
                    continue;
                const unsigned char attacker = *reinterpret_cast<unsigned char*>(d + 3968);
                const int pos = *reinterpret_cast<int*>(d + 3972);
                mlog("[u] uid=%08X atk=%d pos=%d", uid, attacker, pos);
                // Probe the SHARED 2D engine (batViewer2dEngine @3976) ONCE: scan it for FIA pointers
                // (vtable 0x6E2FCC) - direct and via Vector<...>{begin,end} - to find per-unit body sprites.
                static DWORD sEngLog = 0;
                if (grp == 0 && k == 0 && GetTickCount() - sEngLog > 1500) {
                    sEngLog = GetTickCount();
                    char* eng = *reinterpret_cast<char**>(d + 3976);
                    if (isUserPtr(eng) && !IsBadReadPtr(eng, 800)) {
                        // eng+780 is a Vector {begin,end,cap}; dump every element (per-unit body FIAs?)
                        char* begin = *reinterpret_cast<char**>(eng + 780);
                        char* end = *reinterpret_cast<char**>(eng + 784);
                        mlog("    [eng] engine=%p vec780 begin=%p end=%p count=%d", eng, begin, end,
                             (isUserPtr(begin) && isUserPtr(end) && end > begin) ? static_cast<int>((end - begin) / 4) : -1);
                        if (isUserPtr(begin) && isUserPtr(end) && end > begin) {
                            int count = static_cast<int>((end - begin) / 4);
                            if (count > 32)
                                count = 32;
                            for (int i = 0; i < count; ++i) {
                                char* f = *reinterpret_cast<char**>(begin + i * 4);
                                unsigned fv = (isUserPtr(f) && !IsBadReadPtr(f, 36)) ? *reinterpret_cast<unsigned*>(f) : 0;
                                const int cf = (fv == kFiaVft) ? *reinterpret_cast<int*>(f + 32) : -1;
                                mlog("    [vec] [%d] %p vft=%08X cf=%d%s", i, f, fv, cf, fv == kFiaVft ? " FIA" : "");
                            }
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void applyPerUnitBurst(int f)
{
    static DWORD sLastLog = 0;
    g_perUnitExtra = 0; // old countdown hook stays a passthrough; the new path drives the sprite directly
    __try {
        if (f <= 10 || !g_inBattle)
            return; // safety is the strong FactoryImageAnim validation in speedUnitSprite, not a timed gate
        char* viewer = reinterpret_cast<char*>(g_batViewer);
        if (!isUserPtr(viewer))
            return;
        char* vdata = *reinterpret_cast<char**>(viewer + 4);
        if (!isUserPtr(vdata))
            return;
        char* animBase = vdata; // GetUnitAnimation base resolves to vdata (=*(viewer+4)); header-confirmed
        const int actingId = *reinterpret_cast<int*>(vdata + 3996); // viewer's current (acting) unit id
        int extra = (f - 10) / 30;                                  // x15 -> ~4; x3 -> 0..1; modest, tunable
        if (extra < 1)
            extra = 1;
        const bool logNow = GetTickCount() - sLastLog > 1000;
        if (logNow) {
            sLastLog = GetTickCount();
            mlog("[burst] enter f=%d extra=%d actingId=%08X", f, extra, static_cast<unsigned>(actingId));
        }
        static const int kBase[2] = {5032, 5056}; // unitAnimations[6] + unitAnimations2[6]
        int sped = 0;
        for (int grp = 0; grp < 2; ++grp) {
            for (int k = 0; k < 6; ++k) {
                char* anim = *reinterpret_cast<char**>(animBase + kBase[grp] + 4 * k);
                if (!isUserPtr(anim))
                    continue;
                char* ad = *reinterpret_cast<char**>(anim + 4);
                if (!isUserPtr(ad))
                    continue;
                if (*reinterpret_cast<int*>(ad + 12) != actingId) // only the acting unit (attacker) for now
                    continue;
                sped += speedUnitSprite(ad, extra, logNow);
            }
        }
        if (logNow)
            mlog("[burst] f=%d extra=%d actingId=%08X spritesAdvanced=%d", f, extra,
                 static_cast<unsigned>(actingId), sped);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void updateBattleBurst(void)
{
    // Start comes from showAttackEffect; end comes from the final CAnimCounter completion. The thunk
    // stores only the latest event, preserving true order even when both occur inside one 32ms tick.
    const DWORD now = GetTickCount();
    const LONG visualEvent = InterlockedExchange(&g_attackVisualEvent, 0);
    const int idle = g_battleAnimEnabled ? kAnimFactor[g_battleAnimSpeed - 1] : 10;

    if (!g_battleAttackEnabled || !g_inBattle) {
        g_attackVisualActive = 0;
        g_attackExpiryTick = 0;
        g_attackWatchdogTick = 0;
        g_battleFactor = idle;
        return;
    }

    if (visualEvent == 2 && g_attackVisualActive) {
        g_attackVisualActive = 0;
        g_attackExpiryTick = now; // exact end -> begin the 300ms decay immediately
    }
    if (visualEvent == 1) {
        if (g_attackEndHookInstalled) {
            g_attackVisualActive = 1;
            g_attackExpiryTick = 0;
            g_attackWatchdogTick = now + kAttackWatchdogMs;
        } else {
            // Unknown executable layout: retain the previous safe, speed-aware timing behavior.
            g_attackVisualActive = 0;
            g_attackExpiryTick = now + attackHoldMs();
            g_attackWatchdogTick = 0;
        }
    }
    if (g_attackVisualActive &&
        static_cast<int>(now - g_attackWatchdogTick) >= 0) {
        g_attackVisualActive = 0;
        g_attackExpiryTick = now;
        g_attackWatchdogTick = 0;
        mlog("[menu] attack-end watchdog fired");
    }

    g_battleFactor = easedBurstFactor(idle);
    if (!g_attackVisualActive && g_attackExpiryTick &&
        static_cast<int>(now - g_attackExpiryTick) >= static_cast<int>(kAttackRampMs))
        g_attackExpiryTick = 0;
}

// ---------------------------------------------------------------------------
// Dialog VO auto-skip + text logger (Russobit event popups). Only DLG_EVENT_POPUP
// with a REAL voiceover is touched: detour the VO-start (0x4BE403) to arm + log the
// voiced popup, the UI sample-EOS dispatcher (0x521352) to flag VO-end, then invoke the
// single close button (BTN_RIGHTSIDE) from the 32ms UI timer. Voiced-only; default off;
// every game-memory read SEH-guarded. Addresses/offsets from .idare\Discipl2.exe.i64 +
// toolset headers (CPopupInterf/CButtonInterf/CTextBoxInterf/CDialogInterfApi Russobit).
// Design: <game>\dialog-vo-autoclose-research.md.
void* g_dvOrigVoStart = reinterpret_cast<void*>(0x4BE403); // CEventPopup try-start-VO, thiscall(this)
void* g_dvOrigEos = reinterpret_cast<void*>(0x521352);     // UI: sample-finished dispatcher
typedef void*(__stdcall* DvFindFn)(void* dialog, const char* name);
const DvFindFn dvFindButton = reinterpret_cast<DvFindFn>(0x50BAAF);  // CDialogInterf::findButton
const DvFindFn dvFindTextBox = reinterpret_cast<DvFindFn>(0x50BB0F); // CDialogInterf::findTextBox

void* g_dvArmedPopup = nullptr;  // the armed CEventPopup; dialog = *(*(popup+0xC)) (CPopupDialogInterf.dialog**)
int g_dvArmedSound = 0;          // soundId of the armed VO (0 = nothing armed)
bool g_dvVoDone = false;         // set by the EOS hook when the armed soundId finished

const char* dvLogPath()
{
    return exeDirFile("dialog-vo-log.txt");
}

// Append a CP1251 in-game string to an open UTF-8 file (game text is CP1251).
void dvWriteUtf8(HANDLE h, const char* s)
{
    if (!s)
        return;
    wchar_t w[1024];
    int wn = MultiByteToWideChar(1251, 0, s, -1, w, 1024);
    if (wn <= 1)
        return; // empty or conversion failed
    char u8[3072];
    int un = WideCharToMultiByte(CP_UTF8, 0, w, wn - 1, u8, sizeof(u8), nullptr, nullptr);
    if (un > 0) {
        DWORD wr;
        WriteFile(h, u8, static_cast<DWORD>(un), &wr, nullptr);
    }
}

void dvLog(const char* sound, const char* text)
{
    HANDLE h = CreateFileA(dvLogPath(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD wr;
    if (GetFileSize(h, nullptr) == 0) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, bom, 3, &wr, nullptr);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char hdr[96];
    int n = wsprintfA(hdr, "%04d-%02d-%02d %02d:%02d:%02d | sound=", st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond);
    WriteFile(h, hdr, n, &wr, nullptr);
    dvWriteUtf8(h, (sound && sound[0]) ? sound : "");
    WriteFile(h, " | text=", 8, &wr, nullptr);
    dvWriteUtf8(h, text ? text : "");
    WriteFile(h, "\r\n", 2, &wr, nullptr);
    CloseHandle(h);
}

// Popup text: DLG_EVENT_POPUP always uses TXT_RIGHTSIDE (the side only switches the IMG_*side
// picture), but a modded Interf.dlg could use TXT_LEFTSIDE - try both, first non-empty wins.
const char* dvReadText(void* dialog)
{
    static const char* const names[2] = {"TXT_RIGHTSIDE", "TXT_LEFTSIDE"};
    for (int i = 0; i < 2; ++i) {
        void* tb = dvFindTextBox(dialog, names[i]);
        if (!tb)
            continue;
        char* td = *reinterpret_cast<char**>(reinterpret_cast<char*>(tb) + 8);
        if (!td)
            continue;
        const char* s = *reinterpret_cast<char**>(td + 0x14); // CTextBoxInterfData.text (String.ptr)
        if (s && s[0])
            return s;
    }
    return nullptr;
}

// Invoke the popup's single close button: BTN_RIGHTSIDE, with a BTN_LEFTSIDE fallback for a
// modded left-only layout. -> buttonData(+8)->onClickedFunctor.data(+0xC)->vftable[0](functor).
bool dvInvokeClose(void* dialog)
{
    static const char* const names[2] = {"BTN_RIGHTSIDE", "BTN_LEFTSIDE"};
    for (int i = 0; i < 2; ++i) {
        void* btn = dvFindButton(dialog, names[i]);
        if (!btn)
            continue;
        char* bd = *reinterpret_cast<char**>(reinterpret_cast<char*>(btn) + 8);
        void* fn = bd ? *reinterpret_cast<void**>(bd + 0xC) : nullptr;
        if (!fn)
            continue;
        void** vft = *reinterpret_cast<void***>(fn);
        if (vft && vft[0]) {
            reinterpret_cast<void(__thiscall*)(void*)>(vft[0])(fn);
            return true;
        }
    }
    return false;
}

// Detour: CEventPopup VO-start. Run original (it loads + plays the mp3, storing soundId at
// data+0x30), then - only for a real VO - log the popup text/sound and arm the auto-close.
void* __fastcall dvVoStartHook(void* self, void* /*edx*/)
{
    void* ret = reinterpret_cast<void*(__fastcall*)(void*, void*)>(g_dvOrigVoStart)(self, nullptr);
    if (!g_dialogVoSkip || !self)
        return ret;
    // This runs MID-CEventPopup-ctor (called from inside 0x4BDA84), so the dialog's controls
    // (TXT/BTN_RIGHTSIDE) are NOT loaded yet - touching them here faults. So ARM ONLY here (store the
    // sample id + dialog); the text read and the close are deferred to dvoPoll, when the dialog is built.
    __try {
        unsigned dataP = *reinterpret_cast<unsigned*>(reinterpret_cast<char*>(self) + 0x20); // CEventPopup data
        if (!dataP)
            return ret;
        char* data = reinterpret_cast<char*>(dataP);
        unsigned bufP = *reinterpret_cast<unsigned*>(data + 0x2C); // loaded mp3 buffer (set before the play)
        int soundId = *reinterpret_cast<int*>(data + 0x30);       // sample id, for EOS match; 0 = nothing
        if (!soundId && !bufP)
            return ret; // no VO -> leave the dialog to the player (voiced-only)
        g_dvArmedPopup = self; // the CEventPopup; its dialog (= *(*(self+0xC))) is derived at close time
        g_dvArmedSound = soundId;
        g_dvVoDone = false;
        mlog("[dvo] armed id=%d popup=%08x buf=%08x", soundId, (unsigned)(uintptr_t)self, bufP);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mlog("[dvo] voStart SEH");
    }
    return ret;
}

// Detour: UI sample-finished dispatcher. If the finished sample is our armed VO, flag it; the actual
// close runs on the 32ms UI timer (dvoPoll) to avoid touching dialogs mid-dispatch. Queue read mirrors
// the original's own empty-check (head ptr @+0x14C vs tail @+0x150, id = *head).
int __fastcall dvEosHook(void* self, void* /*edx*/, int a2, int a3)
{
    if (g_dialogVoSkip && g_dvArmedSound && self) {
        __try {
            char* base = *reinterpret_cast<char**>(self);
            if (base) {
                void* head = *reinterpret_cast<void**>(base + 0x14C);
                void* tail = *reinterpret_cast<void**>(base + 0x150);
                if (head && head != tail) {
                    int id = *reinterpret_cast<int*>(head);
                    mlog("[dvo] eos id=%d armed=%d", id, g_dvArmedSound);
                    if (id == g_dvArmedSound)
                        g_dvVoDone = true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return reinterpret_cast<int(__fastcall*)(void*, void*, int, int)>(g_dvOrigEos)(self, nullptr, a2, a3);
}

// UI timer (32ms): when the armed VO has finished, invoke BTN_RIGHTSIDE once. Disarm FIRST so a
// re-entrant close or the next popup cannot double-fire. SEH-guarded (the popup may be gone).
void dvoPoll()
{
    if (!g_dialogVoSkip || !g_dvVoDone || !g_dvArmedSound)
        return;
    void* popup = g_dvArmedPopup;
    g_dvVoDone = false;
    g_dvArmedSound = 0;
    g_dvArmedPopup = nullptr;
    if (!popup)
        return;
    // The dialog is fully built now (VO finished). CPopupDialogInterf.dialog sits at popup+0xC; whether
    // it is a CDialogInterf* or CDialogInterf** is version-fuzzy, so probe both derefs and pick the one
    // whose first dword is the real CDialogInterf vftable (0x6E1884). Self-correcting + logs the layout.
    __try {
        void* c1 = *reinterpret_cast<void**>(reinterpret_cast<char*>(popup) + 0xC);
        unsigned v1 = c1 ? *reinterpret_cast<unsigned*>(c1) : 0;
        void* c2 = c1 ? *reinterpret_cast<void**>(c1) : nullptr;
        unsigned v2 = c2 ? *reinterpret_cast<unsigned*>(c2) : 0;
        mlog("[dvo] poll popup=%08x c1=%08x(v=%08x) c2=%08x(v=%08x)", (unsigned)(uintptr_t)popup,
             (unsigned)(uintptr_t)c1, v1, (unsigned)(uintptr_t)c2, v2);
        void* dialog = (v1 == 0x6E1884u) ? c1 : (v2 == 0x6E1884u ? c2 : nullptr);
        if (!dialog) {
            mlog("[dvo] no CDialogInterf (vftable 0x6E1884) off popup+0xC");
            return;
        }
        bool closed = dvInvokeClose(dialog); // BTN_RIGHTSIDE -> onClicked functor
        const char* text = dvReadText(dialog);
        dvLog(nullptr, text); // sound name TBD (popup data+0x14 is a flag, not a string)
        mlog("[dvo] VO done -> close=%d text=%s", closed ? 1 : 0, text ? "logged" : "none");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mlog("[dvo] close SEH");
    }
}

void dvoInstall()
{
    mlog("[dvo] dvoInstall entered (skip=%d)", g_dialogVoSkip ? 1 : 0);
    g_dvOrigVoStart = reinterpret_cast<void*>(0x4BE403);
    g_dvOrigEos = reinterpret_cast<void*>(0x521352);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_dvOrigVoStart, reinterpret_cast<void*>(dvVoStartHook));
    DetourAttach(&g_dvOrigEos, reinterpret_cast<void*>(dvEosHook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_dvOrigVoStart = reinterpret_cast<void*>(0x4BE403);
        g_dvOrigEos = reinterpret_cast<void*>(0x521352);
        mlog("[dvo] detours FAILED (0x4BE403 VO / 0x521352 EOS)");
    } else {
        mlog("[dvo] dialog-VO skip installed (default %s, log dialog-vo-log.txt)",
             g_dialogVoSkip ? "on" : "off");
    }
}

const wchar_t* gameAspectLabel(int width, int height)
{
    const long long w = width;
    const long long h = height;
    if (w * 3 == h * 4)
        return L"4:3";
    if (w * 4 == h * 5)
        return L"5:4";
    if (w * 10 == h * 16)
        return L"16:10";
    if (w * 3 == h * 5)
        return L"5:3";
    // Common nominal 16:9 widths such as 1066 and 1366 cannot be mathematically
    // exact at their integer heights.  Treat a sub-pixel rounding difference as
    // 16:9 instead of calling those reviewed presets "custom".
    const long long wideDifference =
        w * 9 - h * 16;
    if (wideDifference == 0)
        return L"16:9";
    if (wideDifference >= -8 && wideDifference <= 8)
        return L"≈16:9";
    return L(L"custom", L"другие");
}

double fitScale(int gameWidth, int gameHeight, int outputWidth,
                int outputHeight)
{
    if (gameWidth <= 0 || gameHeight <= 0 || outputWidth <= 0 ||
        outputHeight <= 0)
        return 0.0;
    const double scaleW = static_cast<double>(outputWidth) / gameWidth;
    const double scaleH = static_cast<double>(outputHeight) / gameHeight;
    return scaleW < scaleH ? scaleW : scaleH;
}

void formatScale(double scale, wchar_t* text, size_t capacity)
{
    if (!text || capacity == 0)
        return;
    if (scale <= 0.0) {
        lstrcpynW(text, L"?", static_cast<int>(capacity));
        return;
    }
    const double rounded = std::floor(scale + 0.5);
    if (std::fabs(scale - rounded) < 0.005)
        swprintf_s(text, capacity, L"%.0fx", rounded);
    else
        swprintf_s(text, capacity, L"%.2fx", scale);
}

void refreshAdaptiveCanvasItem()
{
    if (!g_displaySizeMenu || g_ver == VerEditor)
        return;

    int outputWidth = 0, outputHeight = 0;
    int canvasWidth = 0, canvasHeight = 0;
    int nativeDisplaySize = -1;
    wchar_t label[256] = {};
    if (horplus_is_available() &&
        horplus_get_primary_adaptive(&outputWidth, &outputHeight,
                                     &canvasWidth, &canvasHeight,
                                     &nativeDisplaySize)) {
        wchar_t scale[32] = {};
        formatScale(fitScale(canvasWidth, canvasHeight,
                             outputWidth, outputHeight),
                    scale, sizeof(scale) / sizeof(scale[0]));
        if (nativeDisplaySize >= 0) {
            swprintf_s(
                label,
                L(L"Automatic for monitor: stock %d x %d -> screen %d x %d (%s; restart)",
                  L"Авто под монитор: штатное %d x %d -> экран %d x %d (%s; перезапуск)"),
                canvasWidth, canvasHeight, outputWidth, outputHeight, scale);
        } else {
            swprintf_s(
                label,
                L(L"Automatic for monitor: Hor+ %d x %d -> screen %d x %d (%s; restart)",
                  L"Авто под монитор: Hor+ %d x %d -> экран %d x %d (%s; перезапуск)"),
                canvasWidth, canvasHeight, outputWidth, outputHeight, scale);
        }
    } else {
        lstrcpynW(
            label,
            L(L"Automatic for monitor (unavailable for this executable)",
              L"Авто под монитор (недоступно для этого exe)"),
            static_cast<int>(sizeof(label) / sizeof(label[0])));
    }
    ModifyMenuW(g_displaySizeMenu, kIdHorplusAuto,
                MF_BYCOMMAND | MF_STRING |
                    (horplus_is_available() ? MF_ENABLED : MF_GRAYED),
                kIdHorplusAuto, label);
}

void refreshOutputSizeItem()
{
    if (!g_displaySizeMenu)
        return;

    wchar_t label[192] = {};
    if (g_requestedOutputW == 0 && g_requestedOutputH == 0) {
        swprintf_s(
            label,
            L(L"Window/stream only: Automatic (follows game resolution)...",
              L"Только окно/стрим: автоматически (следует разрешению игры)..."));
    } else if (g_requestedOutputW > 0 && g_requestedOutputH > 0) {
        swprintf_s(
            label,
            L(L"Window/stream only: %d x %d (%s); game view unchanged...",
              L"Только окно/стрим: %d x %d (%s); обзор не меняется..."),
            g_requestedOutputW, g_requestedOutputH,
            gameAspectLabel(g_requestedOutputW, g_requestedOutputH));
    } else {
        swprintf_s(
            label,
            L(L"Window/stream only: invalid saved value (%d x %d)...",
              L"Только окно/стрим: ошибочное значение (%d x %d)..."),
            g_requestedOutputW, g_requestedOutputH);
    }
    ModifyMenuW(g_displaySizeMenu, kIdOutputSizeCustom,
                MF_BYCOMMAND | MF_STRING, kIdOutputSizeCustom, label);
}

void refreshResolutionParentLabel(int currentW, int currentH,
                                  int pendingW, int pendingH, bool pending)
{
    if (!g_videoMenu || g_resolutionMenuPosition < 0 || currentW <= 0 ||
        currentH <= 0)
        return;

    wchar_t label[192] = {};
    if (pending) {
        swprintf_s(
            label,
            L(L"Game resolution: %d x %d -> %d x %d...",
              L"Разрешение игры: %d x %d -> %d x %d..."),
            currentW, currentH, pendingW, pendingH);
    } else {
        swprintf_s(
            label,
            L(L"Game resolution: %d x %d (%s)...",
              L"Разрешение игры: %d x %d (%s)..."),
            currentW, currentH, gameAspectLabel(currentW, currentH));
    }

    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING;
    info.dwTypeData = label;
    SetMenuItemInfoW(g_videoMenu, static_cast<UINT>(g_resolutionMenuPosition),
                     TRUE, &info);
}

void pendingGameCanvas(int* width, int* height)
{
    int index = g_displaySizePending;
    if (index < 0 || index >= kDisplaySizeCount)
        index = 0;
    if (width)
        *width = kDisplaySizes[index].w;
    if (height)
        *height = kDisplaySizes[index].h;
}

void selectedNextGameCanvas(int liveWidth, int liveHeight, int* width, int* height)
{
    int requestedMode = 0, requestedW = 0, requestedH = 0;
    const bool requestedValid =
        horplus_get_requested(&requestedMode, &requestedW, &requestedH) != 0;
    if (requestedValid && requestedMode == 0) {
        if (width)
            *width = requestedW;
        if (height)
            *height = requestedH;
        return;
    }
    if (requestedValid && requestedMode == 2 && horplus_is_available()) {
        if (width)
            *width = requestedW;
        if (height)
            *height = requestedH;
        return;
    }
    if (requestedValid && requestedMode == 1 && horplus_is_available() &&
        horplusSizeForCanvas(requestedW, requestedH) >= 0) {
        if (width)
            *width = requestedW;
        if (height)
            *height = requestedH;
        return;
    }

    if (liveWidth <= 0 || liveHeight <= 0) {
        pendingGameCanvas(width, height);
        return;
    }

    // Preserve an unknown externally supplied canvas until the user explicitly chooses one of our
    // native/Hor+ modes. This avoids silently advertising a rollback merely because DisplaySize is
    // still present underneath another renderer's custom mode.
    if (displaySizeForCanvas(liveWidth, liveHeight) < 0 &&
        !horplus_is_active() &&
        !g_gameCanvasExplicitlySelected) {
        if (width)
            *width = liveWidth;
        if (height)
            *height = liveHeight;
        return;
    }
    pendingGameCanvas(width, height);
}

bool customAspect(double* heightOverWidth)
{
    if (!g_aspectRatio[0])
        return false;

    char* separator = nullptr;
    const unsigned long width = strtoul(g_aspectRatio, &separator, 0);
    if (!separator || separator == g_aspectRatio || !*separator)
        return false;
    char* end = nullptr;
    const unsigned long height = strtoul(separator + 1, &end, 0);
    if (!end || end == separator + 1 || width == 0 || height == 0)
        return false;
    if (heightOverWidth)
        *heightOverWidth = static_cast<double>(height) / width;
    return true;
}

void predictViewport(int gameW, int gameH, int outW, int outH, int* viewX, int* viewY,
                     int* viewW, int* viewH)
{
    int width = outW;
    int height = outH;

    if (g_boxing) {
        // Match `for (int i = 20; i-- > 1;)` exactly: the post-decrement means its body sees
        // factors 19..1, including a centered exact 1x when 2x does not fit.
        for (int factor = 19; factor >= 1; --factor) {
            if (gameW * factor <= outW && gameH * factor <= outH) {
                width = gameW * factor;
                height = gameH * factor;
                break;
            }
        }
    } else if (g_maintas || g_aspectRatio[0]) {
        double targetRatio = static_cast<double>(gameH) / gameW;
        customAspect(&targetRatio);
        const double outputRatio = static_cast<double>(outH) / outW;

        width = outW;
        height = static_cast<int>(targetRatio * width + 0.5);
        if (outputRatio < targetRatio) {
            height = outH;
            width = static_cast<int>(height / targetRatio + 0.5);
        }
        if (width > outW)
            width = outW;
        if (height > outH)
            height = outH;
    }

    if (viewX)
        *viewX = outW / 2 - width / 2;
    if (viewY)
        *viewY = outH / 2 - height / 2;
    if (viewW)
        *viewW = width;
    if (viewH)
        *viewH = height;
}

void refreshDisplaySizeInfo(int currentW, int currentH)
{
    if (!g_displaySizeMenu || g_ver == VerEditor)
        return;

    g_displaySizeCurrent = displaySizeForCanvas(currentW, currentH);
    int requestedMode = 0, requestedW = 0, requestedH = 0;
    const bool requestedValid =
        horplus_get_requested(&requestedMode, &requestedW, &requestedH) != 0;
    const bool requestedHorplus =
        requestedValid && (requestedMode == 1 || requestedMode == 2) &&
        horplusSizeForCanvas(requestedW, requestedH) >= 0;
    const bool requestedAdaptive = requestedValid && requestedMode == 2;
    if (requestedValid && requestedMode == 0) {
        const int native = displaySizeForCanvas(requestedW, requestedH);
        if (native >= 0)
            g_displaySizePending = native;
    }
    const bool horplusAvailable = horplus_is_available() != 0;
    int pendingW = 0, pendingH = 0;
    selectedNextGameCanvas(currentW, currentH, &pendingW, &pendingH);
    // Equal dimensions do not imply equal game layout: an externally supplied 1280x720 canvas is
    // still pending when our Hor+ preset is selected, because its strategic/UI hooks become active
    // only on the next process start.
    const bool pending =
        currentW != pendingW || currentH != pendingH ||
        requestedHorplus != (horplus_is_active() != 0);
    refreshResolutionParentLabel(currentW, currentH, pendingW, pendingH, pending);

    wchar_t line[384] = {};
    if (!requestedValid) {
        swprintf_s(
            line,
            L(L"Invalid saved game resolution; using current %dx%d until another is selected",
                L"Некорректное сохранённое разрешение; остаётся текущее %dx%d до нового выбора"),
            currentW, currentH);
    } else if (requestedHorplus && !horplusAvailable) {
        swprintf_s(
            line,
            L(L"Widescreen %dx%d is unavailable for this game build; current is %dx%d",
                L"Широкий режим %dx%d недоступен для этой версии игры; сейчас %dx%d"),
            requestedW, requestedH, currentW, currentH);
    } else if (requestedAdaptive) {
        int outputWidth = 0, outputHeight = 0;
        int adaptiveWidth = requestedW, adaptiveHeight = requestedH;
        int nativeDisplaySize = -1;
        horplus_get_primary_adaptive(&outputWidth, &outputHeight,
                                     &adaptiveWidth, &adaptiveHeight,
                                     &nativeDisplaySize);
        wchar_t scale[32] = {};
        formatScale(fitScale(adaptiveWidth, adaptiveHeight,
                             outputWidth, outputHeight),
                    scale, sizeof(scale) / sizeof(scale[0]));
        const wchar_t* family = nativeDisplaySize >= 0
            ? L(L"stock", L"штатное")
            : L"Hor+";
        if (pending) {
            swprintf_s(
                line,
                L(L"Automatic: monitor %dx%d selects %s %dx%d (%s); current %dx%d -> restart required",
                  L"Авто: монитор %dx%d выбирает %s %dx%d (%s); сейчас %dx%d -> нужен перезапуск"),
                outputWidth, outputHeight, family, adaptiveWidth,
                adaptiveHeight, scale, currentW, currentH);
        } else {
            swprintf_s(
                line,
                L(L"Automatic: monitor %dx%d -> %s game %dx%d (%s)",
                  L"Авто: монитор %dx%d -> %s разрешение игры %dx%d (%s)"),
                outputWidth, outputHeight, family, adaptiveWidth,
                adaptiveHeight, scale);
        }
    } else if (g_displaySizeCurrent < 0 && !horplus_is_active() &&
               !g_gameCanvasExplicitlySelected) {
        swprintf_s(
            line,
            L(L"Current external game resolution: %dx%d; no replacement selected",
                L"Сейчас внешнее разрешение игры: %dx%d; замена не выбрана"),
            currentW, currentH);
    } else if (pending) {
        swprintf_s(
            line,
            L(L"Current: %dx%d (%s) -> after restart: %dx%d (%s)",
                L"Сейчас: %dx%d (%s) -> после перезапуска: %dx%d (%s)"),
            currentW, currentH, gameAspectLabel(currentW, currentH), pendingW, pendingH,
            gameAspectLabel(pendingW, pendingH));
    } else {
        swprintf_s(
            line,
            L(L"Current: %dx%d (%s)", L"Сейчас: %dx%d (%s)"),
            currentW, currentH, gameAspectLabel(currentW, currentH));
    }
    ModifyMenuW(g_displaySizeMenu, kIdDisplaySizeState,
                MF_BYCOMMAND | MF_STRING | MF_GRAYED, kIdDisplaySizeState, line);
}

void refreshScaleInfo()
{
    if (!g_scaleMenu)
        return;

    int gameW = 0, gameH = 0, outW = 0, outH = 0;
    int viewX = 0, viewY = 0, viewW = 0, viewH = 0;
    if (!DDGetScaleMetrics(&gameW, &gameH, &outW, &outH, &viewX, &viewY, &viewW,
                           &viewH))
        return;

    // A manual resize updates cnc-ddraw's live window_rect before cfg_save() reaches the ini. Use
    // it for next-start preview only when cfg_save's destination cannot be shadowed by an active
    // per-process override; DDGetOutputConfig deliberately answers conservatively.
    int configuredOutW = 0, configuredOutH = 0;
    int persistsNextStart = 0;
    if (DDGetOutputConfig(&configuredOutW, &configuredOutH,
                          &persistsNextStart) &&
        persistsNextStart) {
        g_requestedOutputW = configuredOutW;
        g_requestedOutputH = configuredOutH;
        g_resIdx = -1;
        for (int i = 0; i < kResCount; ++i) {
            if (kRes[i].w == configuredOutW &&
                kRes[i].h == configuredOutH) {
                g_resIdx = i;
                break;
            }
        }
    }

    refreshDisplaySizeInfo(gameW, gameH);

    int previewGameW = 0, previewGameH = 0;
    selectedNextGameCanvas(gameW, gameH, &previewGameW, &previewGameH);
    int requestedMode = 0, requestedW = 0, requestedH = 0;
    const bool requestedValid =
        horplus_get_requested(&requestedMode, &requestedW, &requestedH) != 0;
    const bool requestedWide =
        requestedValid && (requestedMode == 1 || requestedMode == 2) &&
        horplus_is_available() &&
        horplusSizeForCanvas(requestedW, requestedH) >= 0;
    const bool afterRestart =
        previewGameW != gameW || previewGameH != gameH ||
        (requestedValid && requestedWide != (horplus_is_active() != 0));

    // Downscaling is valid when adjmouse is enabled: the renderer filters the finished canvas and
    // cnc-ddraw maps mouse coordinates back to logical game pixels.
    if (g_resMenu) {
        for (int i = 0; i < kResCount; ++i) {
            EnableMenuItem(g_resMenu, kIdResBase + static_cast<UINT>(i),
                           MF_BYCOMMAND | MF_ENABLED);
        }
    }

    int previewOutW = outW, previewOutH = outH;
    int previewViewX = viewX, previewViewY = viewY;
    int previewViewW = viewW, previewViewH = viewH;
    if (afterRestart) {
        if (g_modeIdx == 1) {
            previewOutW = GetSystemMetrics(SM_CXSCREEN);
            previewOutH = GetSystemMetrics(SM_CYSCREEN);
            if (previewOutW <= 0 || previewOutH <= 0) {
                previewOutW = outW;
                previewOutH = outH;
            }
        } else {
            previewOutW = g_requestedOutputW > 0 ? g_requestedOutputW : previewGameW;
            previewOutH = g_requestedOutputH > 0 ? g_requestedOutputH : previewGameH;
        }
        predictViewport(previewGameW, previewGameH, previewOutW, previewOutH, &previewViewX,
                        &previewViewY, &previewViewW, &previewViewH);
    }

    const int previewMode = afterRestart ? g_modeIdx : DDGetDisplayMode();
    const bool geometryOneToOne =
        previewViewW == previewGameW && previewViewH == previewGameH &&
        (afterRestart || DDGetSimpleZoom1000() == 1000);
    // A pending exclusive request can fall back to another display mode. Mark it as requested
    // below, but do not promise the filled-star result until the live renderer confirms it.
    const bool exclusiveOneToOneUncertain =
        geometryOneToOne && previewMode == 2 && afterRestart;
    const bool oneToOne = geometryOneToOne && !exclusiveOneToOneUncertain;
    wchar_t line[512] = {};
    if (previewMode == 1) {
        swprintf_s(line,
                   L(L"Borderless uses desktop %dx%d; saved output presets are ignored in this mode.",
                       L"Без рамки: рабочий стол %dx%d; сохранённые пресеты вывода здесь игнорируются."),
                   previewOutW, previewOutH);
    } else if (previewMode == 2 && afterRestart) {
        swprintf_s(
            line,
            L(L"Exclusive requests about %dx%d after restart; the renderer may choose a supported fallback.",
                L"Эксклюзив запросит около %dx%d после перезапуска; враппер может выбрать доступный режим."),
            previewOutW, previewOutH);
    } else if (afterRestart && g_requestedOutputW == 0 && g_requestedOutputH == 0) {
        swprintf_s(
            line,
            L(L"Automatic output follows the game resolution: %dx%d now -> %dx%d after restart.",
                L"Вывод «Авто» следует разрешению игры: сейчас %dx%d -> после перезапуска %dx%d."),
            gameW, gameH, previewGameW, previewGameH);
    } else if (afterRestart) {
        swprintf_s(
            line,
            L(L"Fixed output stays %dx%d; scale and bars recalculate after restart.",
                L"Фикс. вывод останется %dx%d; после перезапуска пересчитаются масштаб и поля."),
            previewOutW, previewOutH);
    } else if (g_requestedOutputW == 0 && g_requestedOutputH == 0) {
        swprintf_s(line,
                   L(L"Automatic output follows the current game resolution (%dx%d).",
                       L"Вывод «Авто» следует текущему разрешению игры (%dx%d)."),
                   gameW, gameH);
    } else {
        swprintf_s(line,
                   L(L"Fixed output %dx%d; game %dx%d is filtered to fit.",
                       L"Фикс. вывод %dx%d; кадр игры %dx%d фильтруется до нужного размера."),
                   g_requestedOutputW, g_requestedOutputH, gameW, gameH);
    }
    if (g_resMenu)
        ModifyMenuW(g_resMenu, kIdResInfo, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                    kIdResInfo, line);

    wchar_t customValue[64] = {};
    MultiByteToWideChar(CP_ACP, 0, g_aspectRatio, -1, customValue,
                        static_cast<int>(sizeof(customValue) / sizeof(customValue[0])));
    if (g_aspectRatio[0] && g_boxing)
        swprintf_s(line, L(L"aspect_ratio=%s is ignored while Integer is active",
                            L"aspect_ratio=%s игнорируется в режиме «Целые»"),
                   customValue);
    else if (g_aspectRatio[0])
        swprintf_s(line, L(L"Custom aspect_ratio=%s (select above to clear)",
                            L"Свой aspect_ratio=%s (выбор выше сбросит его)"),
                   customValue);
    else
        lstrcpynW(line, L(L"Custom aspect_ratio from ddraw.ini (not active)",
                          L"Свой aspect_ratio из ddraw.ini (не активен)"),
                  static_cast<int>(sizeof(line) / sizeof(line[0])));
    ModifyMenuW(g_scaleMenu, kIdScaleCustom, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdScaleCustom, line);

    if (afterRestart) {
        swprintf_s(
            line,
            L(L"Now game %dx%d -> output %dx%d; restart preview %dx%d (%s) -> %dx%d",
                L"Сейчас игра %dx%d -> вывод %dx%d; после рестарта %dx%d (%s) -> %dx%d"),
            gameW, gameH, outW, outH, previewGameW, previewGameH,
            gameAspectLabel(previewGameW, previewGameH), previewOutW, previewOutH);
    } else {
        swprintf_s(line,
                   L(L"Game: %dx%d (%s) -> actual output: %dx%d",
                       L"Игра: %dx%d (%s) -> фактический вывод: %dx%d"),
                   gameW, gameH, gameAspectLabel(gameW, gameH), outW, outH);
    }
    ModifyMenuW(g_scaleMenu, kIdScaleGeometry, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdScaleGeometry, line);

    const int left = previewViewX;
    const int right = previewOutW - previewViewX - previewViewW;
    const int top = previewViewY;
    const int bottom = previewOutH - previewViewY - previewViewH;
    if (g_boxing) {
        const int nx = previewGameW ? previewViewW / previewGameW : 0;
        const int ny = previewGameH ? previewViewH / previewGameH : 0;
        if (nx >= 1 && nx == ny && previewViewW == previewGameW * nx &&
            previewViewH == previewGameH * ny) {
            if (afterRestart) {
                swprintf_s(
                    line,
                    L(L"After restart - integer %dx: 1 game px -> %dx%d; viewport %dx%d",
                        L"После рестарта — целые %dx: 1 пикс. игры -> %dx%d; кадр %dx%d"),
                    nx, nx, nx, previewViewW, previewViewH);
            } else {
                swprintf_s(
                    line,
                    L(L"Integer %dx: 1 game px -> %dx%d = %d output px; viewport %dx%d",
                        L"Целые %dx: 1 пикс. игры -> %dx%d = %d пикс. вывода; кадр %dx%d"),
                    nx, nx, nx, nx * nx, previewViewW, previewViewH);
            }
        } else {
            swprintf_s(
                line,
                afterRestart
                    ? L(L"After restart - no exact integer scale fits; renderer uses %dx%d.",
                        L"После рестарта — целый масштаб не помещается; рендер использует %dx%d.")
                    : L(L"No exact integer scale fits; renderer uses %dx%d.",
                        L"Целый масштаб не помещается; рендер использует %dx%d."),
                previewViewW, previewViewH);
        }
    } else if (g_maintas || g_aspectRatio[0]) {
        const double scale =
            previewGameW ? static_cast<double>(previewViewW) / previewGameW : 1.0;
        swprintf_s(
            line,
            afterRestart
                ? L(L"After restart - fit %.3gx: %dx%d; bars L/R %d/%d, T/B %d/%d",
                    L"После рестарта — вписано %.3gx: %dx%d; поля Л/П %d/%d, В/Н %d/%d")
                : L(L"Fit %.3gx: %dx%d; bars L/R %d/%d, T/B %d/%d",
                    L"Вписано %.3gx: %dx%d; поля Л/П %d/%d, В/Н %d/%d"),
            scale, previewViewW, previewViewH, left, right, top, bottom);
    } else {
        const double sx =
            previewGameW ? static_cast<double>(previewViewW) / previewGameW : 1.0;
        const double sy =
            previewGameH ? static_cast<double>(previewViewH) / previewGameH : 1.0;
        swprintf_s(
            line,
            afterRestart
                ? L(L"After restart - stretch %dx%d: X %.3gx, Y %.3gx; geometry distorts.",
                    L"После рестарта — растянуто %dx%d: X %.3gx, Y %.3gx; геометрия искажена.")
                : L(L"Stretch %dx%d: X %.3gx, Y %.3gx; geometry may distort.",
                    L"Растянуто %dx%d: X %.3gx, Y %.3gx; геометрия искажается."),
            previewViewW, previewViewH, sx, sy);
    }
    if (oneToOne) {
        wchar_t detail[512] = {};
        lstrcpynW(detail, line, static_cast<int>(sizeof(detail) / sizeof(detail[0])));
        swprintf_s(
            line,
            L(L"★ No scaling (1:1) — %s",
              L"★ Без масштабирования (1:1) — %s"),
            detail);
    } else if (exclusiveOneToOneUncertain) {
        wchar_t detail[512] = {};
        lstrcpynW(detail, line, static_cast<int>(sizeof(detail) / sizeof(detail[0])));
        swprintf_s(
            line,
            L(L"☆ No scaling requested (1:1) — %s",
              L"☆ Запрошено без масштабирования (1:1) — %s"),
            detail);
    }
    ModifyMenuW(g_scaleMenu, kIdScaleResult, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdScaleResult, line);

    lstrcpynW(
        line,
        oneToOne
            ? L(L"This is a result, not a game mode: 1 game pixel = 1 output pixel; Ctrl+Wheel changes it.",
                L"Это результат, не режим игры: 1 пиксель игры = 1 пиксель вывода; Ctrl+колесо меняет его.")
            : g_boxing
            ? L(L"Exact N x N blocks: OpenGL filter 'None'; others rebuild pixels.",
                L"Точные блоки N x N: OpenGL «Без фильтра»; остальные пересчитывают.")
            : L(L"Filters change resampling only; they do not widen the map.",
                L"Фильтры меняют только пересчёт и не расширяют карту."),
        static_cast<int>(sizeof(line) / sizeof(line[0])));
    ModifyMenuW(g_scaleMenu, kIdScaleFilterInfo, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdScaleFilterInfo, line);
}

void refreshCloudItem()
{
    if (!g_gameMenu)
        return;

    const int status = clouds_get_status();
    const bool ready = status == 2;
    const bool requested = clouds_get_enabled() != 0;
    const bool active = clouds_get_active() != 0;
    // Only the validated MNS/SMNS game layout owns these exact-address hooks. On
    // every other executable the option remains visible, labelled and disabled.
    const bool canToggle = g_ver == VerRussobit && (ready || requested);

    const wchar_t* label = nullptr;
    switch (status) {
    case 1:
        label = L(L"(MNS/SMNS) Map clouds (requires reviewed Imgs\\IsoClouds.ff)",
                  L"(MNS/SMNS) Облака на карте (нужен проверенный Imgs\\IsoClouds.ff)");
        break;
    case 2:
        label = L(L"(MNS/SMNS) Map clouds (game option; restart)",
                  L"(MNS/SMNS) Облака на карте (опция игры; перезапуск)");
        break;
    case 3:
        label = L(L"(MNS/SMNS) Map clouds (archive or hook failed)",
                  L"(MNS/SMNS) Облака на карте (ошибка архива или хука)");
        break;
    default:
        label = L(L"(MNS/SMNS) Map clouds (unsupported executable)",
                  L"(MNS/SMNS) Облака на карте (exe не поддерживается)");
        break;
    }
    ModifyMenuW(g_gameMenu, kIdClouds,
                MF_BYCOMMAND | MF_STRING | (canToggle ? MF_ENABLED : MF_GRAYED),
                kIdClouds, label);
    CheckMenuItem(g_gameMenu, kIdClouds,
                  MF_BYCOMMAND | (requested ? MF_CHECKED : MF_UNCHECKED));

    wchar_t info[224] = {};
    if (ready && clouds_restart_pending()) {
        swprintf_s(
            info,
            L(L"    Active now: %s -> after restart: %s",
                L"    Сейчас: %s -> после перезапуска: %s"),
            active ? L(L"on", L"вкл.") : L(L"off", L"выкл."),
            requested ? L(L"on", L"вкл.") : L(L"off", L"выкл."));
    } else if (ready) {
        swprintf_s(info,
                   L(L"    Active: %s; changes are applied at process start",
                       L"    Сейчас: %s; изменения применяются при запуске"),
                   active ? L(L"on", L"вкл.") : L(L"off", L"выкл."));
    } else if (status == 1) {
        lstrcpynW(info,
                  L(L"    Expected: Imgs\\IsoClouds.ff, 30393 bytes, exact reviewed SHA-256",
                    L"    Ожидается: Imgs\\IsoClouds.ff, 30393 байта, точный проверенный SHA-256"),
                  static_cast<int>(sizeof(info) / sizeof(info[0])));
    } else if (status == 3) {
        lstrcpynW(info,
                  L(L"    Keep clouds off; the game continues without their patches",
                    L"    Оставьте выключенными; игра продолжает без этих патчей"),
                  static_cast<int>(sizeof(info) / sizeof(info[0])));
    } else {
        lstrcpynW(info,
                  L(L"    Available only for a validated MNS/SMNS executable",
                    L"    Доступно только для проверенного exe MNS/SMNS"),
                  static_cast<int>(sizeof(info) / sizeof(info[0])));
    }
    ModifyMenuW(g_gameMenu, kIdCloudsInfo, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdCloudsInfo, info);
}

void refreshDecorativeItem()
{
    if (!g_videoMenu)
        return;

    int requestedMode = 0, requestedWidth = 0, requestedHeight = 0;
    const bool requestedWide =
        horplus_get_requested(&requestedMode, &requestedWidth,
                              &requestedHeight) != 0 &&
        (requestedMode == 1 || requestedMode == 2) &&
        horplusSizeForCanvas(requestedWidth, requestedHeight) >= 0;
    const bool activeWide = horplus_is_active() != 0;
    const bool available =
        horplus_is_available() != 0 && decorative_is_available() != 0;
    const bool applicable = available && (activeWide || requestedWide);

    const wchar_t* label = nullptr;
    if (!available) {
        label = L(L"Decorative background (unavailable for this executable)",
                  L"Декоративный фон (недоступен для этого exe)");
    } else if (activeWide && !requestedWide) {
        label = L(L"Decorative background on classic screens (active until restart)",
                  L"Декоративный фон классических экранов (до перезапуска)");
    } else if (!activeWide && requestedWide) {
        label = L(L"Decorative background on classic screens (after restart)",
                  L"Декоративный фон классических экранов (после перезапуска)");
    } else if (!activeWide) {
        label = L(L"Decorative background — select a widescreen game resolution above",
                  L"Декоративный фон — выберите выше широкое разрешение игры");
    } else {
        label = L(L"Decorative background around classic screens",
                  L"Декоративный фон вокруг классических экранов");
    }

    ModifyMenuW(g_videoMenu, kIdDecorativeBackground,
                MF_BYCOMMAND | MF_STRING |
                    (applicable ? MF_ENABLED : MF_GRAYED),
                kIdDecorativeBackground, label);
    CheckMenuItem(
        g_videoMenu, kIdDecorativeBackground,
        MF_BYCOMMAND |
            (decorative_get_enabled() ? MF_CHECKED : MF_UNCHECKED));
}

void refreshChecks()
{
    if (!g_gameMenu)
        return;
    if (g_editorModeMenu)
        CheckMenuRadioItem(g_editorModeMenu, kIdEditorScenarios, kIdEditorCampaigns,
                           g_editorDatabase ? kIdEditorCampaigns : kIdEditorScenarios,
                           MF_BYCOMMAND);
    // Game
    CheckMenuItem(g_gameMenu, kIdAlwaysActive,
                  MF_BYCOMMAND | (g_alwaysActive ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_gameMenu, kIdDragScroll,
                  MF_BYCOMMAND | (g_dragScroll ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_gameMenu, kIdWideBattle,
                   MF_BYCOMMAND |
                       ((widebattle_is_available() && widebattle_get_enabled())
                            ? MF_CHECKED
                            : MF_UNCHECKED));
    EnableMenuItem(g_gameMenu, kIdWideBattle,
                   MF_BYCOMMAND |
                       (widebattle_is_available() ? MF_ENABLED : MF_GRAYED));
    refreshCloudItem();
    CheckMenuItem(g_gameMenu, kIdDialogVo,
                  MF_BYCOMMAND | (g_dialogVoSkip ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_gameMenu, kIdAutoConfirmUnitHire,
                  MF_BYCOMMAND | (g_autoConfirmUnitHire ? MF_CHECKED : MF_UNCHECKED));
    if (g_menuLanguageMenu)
        CheckMenuRadioItem(g_menuLanguageMenu, kIdMenuLanguageAuto, kIdMenuLanguageRu,
                           kIdMenuLanguageAuto + static_cast<UINT>(g_menuLanguage),
                           MF_BYCOMMAND);
    if (g_localeMenu) {
        const LCID current = static_cast<LCID>(localization_get_locale());
        UINT selected = kIdLocaleNone;
        for (int i = 0; i < g_localeCount; ++i) {
            if (g_localeOptions[i].locale == current) {
                selected = kIdLocaleBase + static_cast<UINT>(i);
                break;
            }
        }
        CheckMenuRadioItem(g_localeMenu, kIdLocaleNone,
                           kIdLocaleBase + static_cast<UINT>(g_localeCount - 1), selected,
                           MF_BYCOMMAND);
    }
    const UINT bSel = g_battleAnimEnabled ? (kIdAnim1 + static_cast<UINT>(g_battleAnimSpeed - 1)) : kIdAnimOff;
    if (g_battleAnimMenu)
        CheckMenuRadioItem(g_battleAnimMenu, kIdAnimOff, kIdAnim6, bSel, MF_BYCOMMAND);
    const UINT mSel = g_mapAnimEnabled ? (kIdAnimMap1 + static_cast<UINT>(g_mapAnimSpeed - 1)) : kIdAnimMapOff;
    if (g_mapAnimMenu)
        CheckMenuRadioItem(g_mapAnimMenu, kIdAnimMapOff, kIdAnimMap6, mSel, MF_BYCOMMAND);
    const UINT aSel = g_battleAttackEnabled ? (kIdAtk1 + static_cast<UINT>(g_battleAttackSpeed - 1)) : kIdAtkOff;
    if (g_battleAtkMenu)
        CheckMenuRadioItem(g_battleAtkMenu, kIdAtkOff, kIdAtk6, aSel, MF_BYCOMMAND);
    if (g_battleMenu)
        CheckMenuRadioItem(g_battleMenu, kIdBattle1, kIdBattle4,
                           kIdBattle1 + static_cast<UINT>(g_battleSpeed - 1), MF_BYCOMMAND);
    if (g_mapMenu)
        CheckMenuRadioItem(g_mapMenu, kIdMap1, kIdMap3,
                           kIdMap1 + static_cast<UINT>(g_mapSpeed - 1), MF_BYCOMMAND);
    // Video
    // ModifyMenuW refreshes the dynamic Custom/info strings, so do it before applying radio checks.
    refreshScaleInfo();
    refreshOutputSizeItem();
    refreshAdaptiveCanvasItem();
    if (g_displaySizeMenu && g_ver != VerEditor) {
        const bool canvasMenuAvailable =
            horplus_is_available() || g_ver == VerRussobit;
        for (UINT id = kIdDisplaySize0; id <= kIdDisplaySize2; ++id)
        {
            CheckMenuItem(g_displaySizeMenu, id, MF_BYCOMMAND | MF_UNCHECKED);
            EnableMenuItem(g_displaySizeMenu, id,
                           MF_BYCOMMAND |
                               (canvasMenuAvailable ? MF_ENABLED : MF_GRAYED));
        }
        for (UINT id = kIdHorplusBase;
             id < kIdHorplusBase + static_cast<UINT>(kHorplusSizeCount);
             ++id) {
            CheckMenuItem(g_displaySizeMenu, id, MF_BYCOMMAND | MF_UNCHECKED);
            EnableMenuItem(g_displaySizeMenu, id,
                           MF_BYCOMMAND |
                                 (horplus_is_available() ? MF_ENABLED : MF_GRAYED));
        }
        CheckMenuItem(g_displaySizeMenu, kIdHorplusAuto,
                      MF_BYCOMMAND | MF_UNCHECKED);
        EnableMenuItem(g_displaySizeMenu, kIdHorplusAuto,
                       MF_BYCOMMAND |
                           (horplus_is_available() ? MF_ENABLED : MF_GRAYED));

        int requestedMode = 0, requestedW = 0, requestedH = 0;
        if (horplus_get_requested(&requestedMode, &requestedW, &requestedH)) {
            UINT selected = 0;
            if (requestedMode == 1) {
                const int custom = horplusSizeForCanvas(requestedW, requestedH);
                if (custom >= 0) {
                    selected = kIdHorplusBase + static_cast<UINT>(custom);
                    CheckMenuRadioItem(
                        g_displaySizeMenu, kIdHorplusBase,
                        kIdHorplusBase +
                            static_cast<UINT>(kHorplusSizeCount - 1),
                        selected, MF_BYCOMMAND);
                }
            } else if (requestedMode == 2) {
                selected = kIdHorplusAuto;
                CheckMenuRadioItem(g_displaySizeMenu, kIdHorplusBase,
                                   kIdHorplusAuto, selected, MF_BYCOMMAND);
            } else {
                selected =
                    kIdDisplaySize0 + static_cast<UINT>(g_displaySizePending);
                CheckMenuRadioItem(g_displaySizeMenu, kIdDisplaySize0,
                                   kIdDisplaySize2, selected, MF_BYCOMMAND);
            }
        }
    }
    const int checkedMode =
        g_liveModeIdx >= 0 ? g_liveModeIdx : g_modeIdx;
    if (g_modeMenu && checkedMode >= 0)
        CheckMenuRadioItem(g_modeMenu, kIdModeWindowed, kIdModeExclusive,
                           kIdModeWindowed + static_cast<UINT>(checkedMode),
                           MF_BYCOMMAND);
    if (g_resMenu && g_resIdx >= 0)
        CheckMenuRadioItem(g_resMenu, kIdResBase, kIdResBase + kResCount - 1,
                           kIdResBase + static_cast<UINT>(g_resIdx), MF_BYCOMMAND);
    if (g_shaderMenu && g_shaderIdx >= 0)
        CheckMenuRadioItem(g_shaderMenu, kIdShaderBase, kIdShaderBase + kShaderCount - 1,
                           kIdShaderBase + static_cast<UINT>(g_shaderIdx), MF_BYCOMMAND);
    if (g_rendMenu && g_rendererIdx >= 0)
        CheckMenuRadioItem(g_rendMenu, kIdRendOpenGL, kIdRendAuto,
                           kIdRendOpenGL + static_cast<UINT>(g_rendererIdx), MF_BYCOMMAND);
    if (g_scaleMenu) {
        const UINT scale = g_boxing         ? kIdBoxing
                           : g_aspectRatio[0] ? kIdScaleCustom
                           : g_maintas      ? kIdMaintas
                                             : kIdScaleStretch;
        CheckMenuRadioItem(g_scaleMenu, kIdMaintas, kIdScaleCustom, scale, MF_BYCOMMAND);
    }
    if (g_videoMenu)
        CheckMenuItem(g_videoMenu, kIdVsync, MF_BYCOMMAND | (g_vsync ? MF_CHECKED : MF_UNCHECKED));
    refreshDecorativeItem();
    // Performance
    if (g_fpsMenu && g_fpsIdx >= 0)
        CheckMenuRadioItem(g_fpsMenu, kIdFpsBase, kIdFpsBase + kFpsCount - 1,
                           kIdFpsBase + static_cast<UINT>(g_fpsIdx), MF_BYCOMMAND);
    if (g_ticksMenu && g_ticksIdx >= 0)
        CheckMenuRadioItem(g_ticksMenu, kIdTicks0, kIdTicks100,
                           kIdTicks0 + static_cast<UINT>(g_ticksIdx), MF_BYCOMMAND);
    if (g_perfMenu)
        CheckMenuItem(g_perfMenu, kIdSingleCpu,
                      MF_BYCOMMAND | (g_singlecpu ? MF_CHECKED : MF_UNCHECKED));
}

void onMenuCommand(UINT id)
{
    if (g_ver != VerRussobit &&
        (id == kIdAlwaysActive ||
         (id >= kIdAnimOff && id <= kIdAnim6) ||
         (id >= kIdBattle1 && id <= kIdBattle4) ||
         (id >= kIdMap1 && id <= kIdMap3) ||
         (id >= kIdAnimMapOff && id <= kIdAnimMap6) ||
         (id >= kIdDragScroll && id <= kIdDialogVoInfo) ||
         id == kIdAutoConfirmUnitHire || id == kIdClouds)) {
        // Disabled menu items normally cannot generate WM_COMMAND, but never
        // let a synthetic command reach an exact-address MNS/SMNS path.
        return;
    }

    bool restartItem = false;
    bool outputSizeChanged = false;
    bool displayModeChanged = false;
    if (id == kIdEditorScenarios || id == kIdEditorCampaigns) {
        const int value = id == kIdEditorCampaigns ? 1 : 0;
        if (value != g_editorDatabase) {
            writeEditorDatabase(value);
            refreshChecks();
            MessageBoxW(g_gameHwnd,
                        L(L"The editor database was changed. Restart the scenario editor to apply it.",
                          L"База редактора изменена. Перезапустите редактор сценариев для применения."),
                        L(L"Scenario editor", L"Редактор сценариев"),
                        MB_OK | MB_ICONINFORMATION);
        }
        return;
    } else if (id >= kIdDisplaySize0 && id <= kIdDisplaySize2 &&
               (horplus_is_available() || g_ver == VerRussobit)) {
        const int value = static_cast<int>(id - kIdDisplaySize0);
        int oldMode = 0, oldW = 0, oldH = 0;
        const bool oldValid =
            horplus_get_requested(&oldMode, &oldW, &oldH) != 0;
        const bool changed =
            !oldValid || oldMode != 0 || g_displaySizePending != value;
        const bool saved = horplus_set_native_requested(value) != 0;
        if (saved)
            g_gameCanvasExplicitlySelected = true;
        if (!saved) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save the game resolution to Disciple.ini.",
                    L"Не удалось сохранить разрешение игры в Disciple.ini."),
                L(L"Game resolution", L"Разрешение игры"), MB_OK | MB_ICONERROR);
        }
        readDisplaySize();
        if (saved)
            mlog("[menu] original DisplaySize=%d saved for next game start (%dx%d)",
                 value, kDisplaySizes[value].w, kDisplaySizes[value].h);
        // Deliberately no DDReloadConfig: the game owns this canvas and reads it only at process
        // startup. refreshChecks updates future preset eligibility and the after-restart preview.
        refreshChecks();
        if (saved && changed)
            showGameResolutionRestartModal(
                kDisplaySizes[value].w, kDisplaySizes[value].h, value);
        return;
    } else if (id == kIdHorplusAuto && horplus_is_available()) {
        int oldMode = 0, oldW = 0, oldH = 0;
        const bool oldValid =
            horplus_get_requested(&oldMode, &oldW, &oldH) != 0;
        int outputW = 0, outputH = 0;
        int selectedW = 0, selectedH = 0;
        int nativeDisplaySize = -1;
        if (!horplus_get_primary_adaptive(&outputW, &outputH,
                                          &selectedW, &selectedH,
                                          &nativeDisplaySize)) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not determine a safe automatic game resolution for this monitor.",
                  L"Не удалось определить безопасное автоматическое разрешение игры для этого монитора."),
                L(L"Automatic game resolution",
                  L"Автоматическое разрешение игры"),
                MB_OK | MB_ICONERROR);
            return;
        }

        int currentW = 0, currentH = 0;
        if (!DDGetScaleMetrics(&currentW, &currentH, nullptr, nullptr,
                               nullptr, nullptr, nullptr, nullptr))
            horplus_get_active_size(&currentW, &currentH);
        const bool saved = horplus_set_requested(2, 0, 0) != 0;
        if (saved)
            g_gameCanvasExplicitlySelected = true;
        else {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save automatic game resolution to Disciple.ini.",
                  L"Не удалось сохранить автоматическое разрешение игры в Disciple.ini."),
                L(L"Automatic game resolution",
                  L"Автоматическое разрешение игры"),
                MB_OK | MB_ICONERROR);
            return;
        }
        refreshChecks();
        if (currentW != selectedW || currentH != selectedH) {
            showAdaptiveResolutionRestartModal(
                currentW, currentH, outputW, outputH, selectedW, selectedH,
                nativeDisplaySize);
        } else if (!oldValid || oldMode != 2) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Automatic monitor selection is enabled. The current game resolution already matches the recommendation; future monitor-resolution changes are applied on the next full game start.",
                  L"Автоподбор под монитор включён. Текущее разрешение игры уже совпадает с рекомендацией; будущая смена разрешения монитора применится при следующем полном запуске игры."),
                L(L"Automatic game resolution",
                  L"Автоматическое разрешение игры"),
                MB_OK | MB_ICONINFORMATION);
        }
        return;
    } else if (id >= kIdHorplusBase &&
               id < kIdHorplusBase + static_cast<UINT>(kHorplusSizeCount) &&
               horplus_is_available()) {
        const int value = static_cast<int>(id - kIdHorplusBase);
        int oldMode = 0, oldW = 0, oldH = 0;
        const bool oldValid =
            horplus_get_requested(&oldMode, &oldW, &oldH) != 0;
        const bool changed =
            !oldValid || oldMode != 1 ||
            oldW != kHorplusSizes[value].w ||
            oldH != kHorplusSizes[value].h;
        const bool saved =
            horplus_set_requested(1, kHorplusSizes[value].w,
                                  kHorplusSizes[value].h) != 0;
        if (saved)
            g_gameCanvasExplicitlySelected = true;
        else
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save the game resolution to Disciple.ini.",
                    L"Не удалось сохранить разрешение игры в Disciple.ini."),
                L(L"Game resolution", L"Разрешение игры"),
                MB_OK | MB_ICONERROR);
        refreshChecks();
        if (saved && changed)
            showGameResolutionRestartModal(
                kHorplusSizes[value].w, kHorplusSizes[value].h, -1);
        return;
    } else if (id == kIdDecorativeBackground &&
               horplus_is_available() && decorative_is_available()) {
        if (!decorative_set_enabled(!decorative_get_enabled())) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save the decorative-background setting to C4menu.ini.",
                  L"Не удалось сохранить настройку декоративного фона в C4menu.ini."),
                L(L"Decorative background", L"Декоративный фон"),
                MB_OK | MB_ICONERROR);
        }
        refreshChecks();
        return;
    } else if (id == kIdAlwaysActive) {
        g_alwaysActive = !g_alwaysActive;
        applyAlwaysActive(g_alwaysActive);
    } else if (id == kIdDragScroll) {
        g_dragScroll = !g_dragScroll; // live: the detour reads this flag (persist() saves it)
    } else if (id == kIdWideBattle && widebattle_is_available()) {
        // Hook state is read only while constructing the next battle; never mutate a live dialog.
        widebattle_set_enabled(!widebattle_get_enabled());
    } else if (id == kIdClouds &&
               (clouds_is_available() || clouds_get_enabled())) {
        // Archive ownership and executable hooks are established only during process startup.
        const int enabled = clouds_get_enabled() ? 0 : 1;
        if (!clouds_set_enabled(enabled)) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save the native [Settings] IsoBirds option to Disciple.ini.",
                    L"Не удалось сохранить штатную опцию [Settings] IsoBirds в Disciple.ini."),
                L(L"Map clouds", L"Облака на карте"),
                MB_OK | MB_ICONERROR);
        } else
            setNativeCloudVisibility(enabled);
        refreshChecks();
        return;
    } else if (id == kIdDialogVo) {
        g_dialogVoSkip = !g_dialogVoSkip; // live: the detours read this flag (persist() saves it)
    } else if (id == kIdAutoConfirmUnitHire) {
        g_autoConfirmUnitHire = !g_autoConfirmUnitHire; // live pass-through hook; default off
    } else if (id >= kIdMenuLanguageAuto && id <= kIdMenuLanguageRu) {
        g_menuLanguage = static_cast<int>(id - kIdMenuLanguageAuto);
        persist();
        refreshChecks();
        return; // the full menu tree (including plugin-owned submenus) is rebuilt on next launch
    } else if (id == kIdLocaleNone) {
        if (!localization_set_locale(0))
            mlog("[menu] failed to persist Wrapper/Locale=0");
        refreshChecks();
        return;
    } else if (id >= kIdLocaleBase &&
               id < kIdLocaleBase + static_cast<UINT>(g_localeCount)) {
        const LCID locale = g_localeOptions[id - kIdLocaleBase].locale;
        if (!localization_set_locale(static_cast<unsigned>(locale)))
            mlog("[menu] failed to apply/persist Wrapper/Locale=%lu",
                 static_cast<unsigned long>(locale));
        refreshChecks();
        return;
    } else if (id == kIdAnimOff) {
        g_battleAnimEnabled = false;
        applyAnimSpeed(0, false, g_battleAnimSpeed);
    } else if (id >= kIdAnim1 && id <= kIdAnim6) {
        g_battleAnimEnabled = true;
        g_battleAnimSpeed = static_cast<int>(id - kIdAnim1) + 1;
        applyAnimSpeed(0, true, g_battleAnimSpeed);
    } else if (id == kIdAnimMapOff) {
        g_mapAnimEnabled = false;
        applyAnimSpeed(1, false, g_mapAnimSpeed);
    } else if (id >= kIdAnimMap1 && id <= kIdAnimMap6) {
        g_mapAnimEnabled = true;
        g_mapAnimSpeed = static_cast<int>(id - kIdAnimMap1) + 1;
        applyAnimSpeed(1, true, g_mapAnimSpeed);
    } else if (id == kIdAtkOff) {
        g_battleAttackEnabled = false;
        g_attackVisualEvent = 0;
        g_attackVisualActive = 0;
        g_attackExpiryTick = 0;
        g_attackWatchdogTick = 0;
        applyAnimSpeed(0, g_battleAnimEnabled, g_battleAnimSpeed); // hand g_battleFactor back to the base
    } else if (id >= kIdAtk1 && id <= kIdAtk6) {
        g_battleAttackEnabled = true;
        g_battleAttackSpeed = static_cast<int>(id - kIdAtk1) + 1;
        updateBattleBurst(); // apply immediately (idle base now, burst on next hit)
    } else if (id == kIdPerUnit) {
        g_perUnitBurst = !g_perUnitBurst;
        if (!g_perUnitBurst)
            applyPerUnitBurst(10); // restore any scaled intervals
        updateBattleBurst();
    } else if (id >= kIdRendOpenGL && id <= kIdRendAuto) {
        g_rendererIdx = static_cast<int>(id - kIdRendOpenGL);
        writeDdrawStr("renderer", kRenderers[g_rendererIdx].value);
        restartItem = true;
    } else if (id >= kIdShaderBase && id < kIdShaderBase + static_cast<UINT>(kShaderCount)) {
        g_shaderIdx = static_cast<int>(id - kIdShaderBase);
        writeDdrawStr("shader", kShaders[g_shaderIdx].value);
        restartItem = true;
    } else if (id == kIdMaintas) {
        // Fit: native game aspect, fractional scale and letter/pillar-boxing as needed.
        g_maintas = true;
        g_boxing = false;
        g_aspectRatio[0] = 0;
        writeDdrawStr("aspect_ratio", "");
        writeDdrawBool("maintas", g_maintas);
        writeDdrawBool("boxing", g_boxing);
        restartItem = true;
    } else if (id == kIdVsync) {
        g_vsync = !g_vsync;
        writeDdrawBool("vsync", g_vsync);
        restartItem = true;
    } else if (id == kIdBoxing) {
        // Integer fit: boxing owns the viewport and therefore maintas must not pretend to be active.
        g_maintas = false;
        g_boxing = true;
        g_aspectRatio[0] = 0;
        writeDdrawStr("aspect_ratio", "");
        writeDdrawBool("maintas", g_maintas);
        writeDdrawBool("boxing", g_boxing);
        restartItem = true;
    } else if (id == kIdScaleStretch) {
        g_maintas = false;
        g_boxing = false;
        g_aspectRatio[0] = 0;
        writeDdrawStr("aspect_ratio", "");
        writeDdrawBool("maintas", g_maintas);
        writeDdrawBool("boxing", g_boxing);
        restartItem = true;
    } else if (id >= kIdTicks0 && id <= kIdTicks100) {
        g_ticksIdx = static_cast<int>(id - kIdTicks0);
        char b[8];
        wsprintfA(b, "%d", kTicksValues[g_ticksIdx]);
        writeDdrawStr("maxgameticks", b);
        restartItem = true;
    } else if (id == kIdSingleCpu) {
        const bool requested = !g_singlecpu;
        if (!DDWriteConfigString(
                "singlecpu", requested ? "true" : "false")) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save the 1 CPU setting to the active ddraw.ini section. The setting was not changed.",
                  L"Не удалось сохранить настройку 1 CPU в активную секцию ddraw.ini. Настройка не изменена."),
                L(L"1 CPU stability", L"Стабильность 1 CPU"),
                MB_OK | MB_ICONERROR);
        } else {
            g_singlecpu = requested;
            refreshChecks();
        }
        // Do not reload g_config here. cnc-ddraw applies this policy once during startup; a live
        // cfg reload would let new and existing Windows 11 threads observe different policies.
        return;
    } else if (id == kIdOutputSizeCustom) {
        int width = g_requestedOutputW;
        int height = g_requestedOutputH;
        if (!chooseOutputSize(g_gameHwnd, &width, &height))
            return;
        if (width == g_requestedOutputW && height == g_requestedOutputH) {
            refreshChecks();
            return;
        }
        bool rollbackComplete = true;
        if (!saveOutputSize(width, height, &rollbackComplete)) {
            MessageBoxW(
                g_gameHwnd,
                rollbackComplete
                    ? L(L"Could not save width and height to the active ddraw.ini section. The window size was not changed.",
                        L"Не удалось сохранить width и height в активную секцию ddraw.ini. Размер окна не изменён.")
                    : L(L"Saving width and height failed, and the previous pair could not be fully restored. Check ddraw.ini before restarting the game.",
                        L"Сохранение width и height завершилось ошибкой, и прежнюю пару не удалось полностью восстановить. Проверьте ddraw.ini перед перезапуском игры."),
                L(L"Window size", L"Размер окна"),
                MB_OK | MB_ICONERROR);
            return;
        }
        g_requestedOutputW = width;
        g_requestedOutputH = height;
        g_resIdx = -1;
        for (int i = 0; i < kResCount; ++i) {
            if (kRes[i].w == width && kRes[i].h == height) {
                g_resIdx = i;
                break;
            }
        }
        // Apply first, then derive diagnostics from the new live g_config. The shared tail refreshes
        // before reload, which would briefly restore the old dimensions in this dynamic menu item.
        applyDdrawLive(true, false);
        if (g_gameHwnd)
            syncChrome(g_gameHwnd);
        refreshChecks();
        return;
    } else if (id >= kIdResBase && id < kIdResBase + static_cast<UINT>(kResCount)) {
        const int index = static_cast<int>(id - kIdResBase);
        g_resIdx = index;
        g_requestedOutputW = kRes[g_resIdx].w;
        g_requestedOutputH = kRes[g_resIdx].h;
        char b[12];
        wsprintfA(b, "%d", kRes[g_resIdx].w);
        writeDdrawStr("width", b);
        wsprintfA(b, "%d", kRes[g_resIdx].h);
        writeDdrawStr("height", b);
        outputSizeChanged = true;
        restartItem = true;
    } else if (id >= kIdFpsBase && id < kIdFpsBase + static_cast<UINT>(kFpsCount)) {
        g_fpsIdx = static_cast<int>(id - kIdFpsBase);
        char b[12];
        wsprintfA(b, "%d", kFpsValues[g_fpsIdx]);
        writeDdrawStr("maxfps", b);
        restartItem = true;
    } else if (id >= kIdModeWindowed && id <= kIdModeExclusive) {
        g_modeIdx = static_cast<int>(id - kIdModeWindowed);
        writeDdrawStr("windowed", kModes[g_modeIdx].windowed);
        writeDdrawStr("fullscreen", kModes[g_modeIdx].fullscreen);
        if (g_modeIdx == 1)
            writeDdrawBool("toggle_borderless", true);
        else if (g_modeIdx == 2)
            writeDdrawBool("toggle_borderless", false);
        displayModeChanged = true;
        restartItem = true;
    } else if (id == kIdScreenshot) {
        takeScreenshot(); // action: nothing to persist or re-check
        return;
    } else if (id >= kIdBattle1 && id <= kIdBattle4) {
        g_battleSpeed = static_cast<int>(id - kIdBattle1) + 1;
        setNativeSpeed(0, g_battleSpeed); // applies next battle; no restart, no C4menu.ini
        refreshChecks();
        return;
    } else if (id >= kIdMap1 && id <= kIdMap3) {
        g_mapSpeed = static_cast<int>(id - kIdMap1) + 1;
        setNativeSpeed(1, g_mapSpeed); // applies next turn; no restart, no C4menu.ini
        refreshChecks();
        return;
    } else {
        return;
    }
    refreshChecks();
    if (restartItem) {
        // We are the renderer: re-apply live without losing a manual resize or a hotkey-selected
        // mode. Explicit Output size and Display mode choices each replace only their own state.
        applyDdrawLive(outputSizeChanged, displayModeChanged);
        if (g_gameHwnd)
            syncChrome(g_gameHwnd);
    } else {
        persist(); // live mod toggles -> C4menu.ini
    }
}

// With devmode the game hides the OS cursor (draws its own in the client), so it's invisible over our
// caption + menu bar. Bump it visible over the non-client area, restore in the client. ShowCursor is a
// global counter, so keep our +1/-1 balanced.
void setNonClientCursor(bool overNonClient)
{
    if (overNonClient) {
        if (!g_ncCursorShown) {
            // Force the global show-count non-negative (game may have hidden it well below -1),
            // tracking how many we added so we can remove exactly that many.
            g_ncCursorAdded = 0;
            int c = ShowCursor(TRUE);
            ++g_ncCursorAdded;
            while (c < 0) {
                c = ShowCursor(TRUE);
                ++g_ncCursorAdded;
            }
            g_ncCursorShown = 1;
        }
        SetCursor(LoadCursorA(nullptr, IDC_ARROW));
    } else if (g_ncCursorShown) {
        for (int i = 0; i < g_ncCursorAdded; ++i)
            ShowCursor(FALSE);
        g_ncCursorAdded = 0;
        g_ncCursorShown = 0;
    }
}

extern "C" void timerhost_pump(void); // perform any queued on-elapse press on the game thread

LRESULT CALLBACK wndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_gameHwnd) {
        g_gameHwnd = hwnd; // remember the game window (drag-scroll SetCapture target)
        // Start our 32ms WM_TIMER on first sight of the window. The on-elapse press is WM_TIMER-driven
        // ONLY: the OS dispatches WM_TIMER when the queue is idle, so END_TURN waits until the hero has
        // finished moving (pressing mid-move fires onClick but the game rejects the turn-end). Legacy
        // timer.mod did this via SetTimer(hWnd,0,0x20,0) + its WM_TIMER handler.
        SetTimer(hwnd, kPressTimerId, 32, nullptr);
    }
    // Our private 32ms timer (idle WM_TIMER): refresh the cached scenario day on the GAME thread, then
    // run the queued auto-end-turn/retreat press. Gate on OUR id and CONSUME it (return 0) so the game's
    // own timers don't drive the pump and our id never leaks into the game's WndProc timer dispatch.
    if (msg == WM_TIMER && wParam == kPressTimerId) {
        featuremenu_refresh_day();
        updateBattleBurst(); // idle/attack split: drive g_battleFactor from exact visual events
        dvoPoll();           // auto-close a voiced event popup once its VO has finished
        timerhost_pump();
        return 0;
    }

    // Legacy C4dll-R used F4 for a one-key normal-window/fullscreen toggle. Keep it wrapper-owned:
    // old ddraw.ini files have no keytogglefullscreen2, and Alt+F4 remains a WM_SYSKEYDOWN.
    if (msg == WM_KEYDOWN && wParam == VK_F4 && !(lParam & 0x40000000)) {
        DDToggleWindowedMode();
        syncChrome(hwnd);
        return 0;
    }

    // The overlay is deliberately WS_EX_TRANSPARENT. Give native plugins first refusal only for
    // mouse/capture messages; timer.c4p uses this for its explicit Ctrl+Alt drag gesture. This must
    // precede map drag so a clock grab never reaches the strategic-map handler.
    if ((msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP ||
         msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED) &&
        pluginhost_mouse(msg, wParam)) {
        return 0;
    }

    // fake_WndProc has already transformed lParam into game coordinates before it calls the game's
    // WndProc. A second cnc-ddraw transform here shifts the drag anchor under scaling or letterboxing.
    if (g_dragScrollActive && msg == WM_MOUSEMOVE) {
        dragScrollWndMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                          static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;
    }
    // Keep the OS pointer visible over the caption + menu bar (non-client), invisible in the client.
    switch (msg) {
    case WM_NCMOUSEMOVE:
    case WM_ENTERMENULOOP:
        setNonClientCursor(true);
        break;
    case WM_MOUSEMOVE:
        setNonClientCursor(false);
        break;
    default:
        break;
    }

    // GUI-thread chrome sync. The worker only posts this message; SetMenu/DrawMenuBar and renderer
    // relayout must happen on the owning GUI thread.
    if (g_relayoutMsg && msg == g_relayoutMsg) {
        syncChrome(hwnd);
        return 0;
    }

    // Menu-bar command: WM_COMMAND with HIWORD==0 AND lParam==0. The lParam check is essential - a
    // control's WM_COMMAND (BN_CLICKED) also has HIWORD==0 but passes the control HWND in lParam and
    // would collide with our IDs. Real menu commands always have lParam==0.
    if (msg == WM_COMMAND && lParam == 0 && HIWORD(wParam) == 0) {
        const UINT id = LOWORD(wParam);
        if (id >= kIdAlwaysActive && id <= kIdLast) {
            onMenuCommand(id);
            return 0;
        }
        // Plugin config-menu commands (0xB000+ block): route to the owning plugin's c4p_command.
        if (id >= 0xB000 && id < 0xC000 && pluginhost_command(id))
            return 0;
    } else if (msg == WM_INITMENUPOPUP) {
        refreshChecks();
    }
    return g_origWndProc(hwnd, msg, wParam, lParam);
}

void buildMenu()
{
    if (g_bar)
        return;
    // Plugins load on the host worker thread (not loader-lock-serialized DllMain), so wait for that
    // before reading the list. Loading takes milliseconds vs seconds to the game window, so this
    // effectively never blocks; the timeout just means "build without plugins" if loading hangs.
    pluginhost_wait_ready(5000);
    readDdrawState(); // reflect current ddraw.ini in the checks/radios
    if (g_ver == VerEditor)
        readEditorDatabase();
    else if (horplus_is_available() || g_ver == VerRussobit) {
        readDisplaySize();
        if (g_ver == VerRussobit)
            readNativeSpeeds(); // exact in-memory GameSettings remains MNS/SMNS-only
    }
    if (g_ver == VerRussobit) {
        mlog("[clouds] native=%d active=%d status=%d restartPending=%d",
             clouds_get_enabled(), clouds_get_active(), clouds_get_status(),
             clouds_restart_pending());
    }

    const UINT mnsDisabled = g_ver == VerRussobit ? 0u : MF_GRAYED;
    if (g_ver == VerEditor) {
        // The original wrapper exposes this only in the supported ScenEdit executable:
        // [Disciple] ScenEditDatabase selects the scenario or campaign database, and the editor
        // must be restarted after changing it. This setting needs no mss32 integration.
        g_gameMenu = CreatePopupMenu();
        g_editorModeMenu = CreatePopupMenu();
        AppendMenuW(g_editorModeMenu, MF_STRING, kIdEditorScenarios,
                    L(L"Scenarios", L"Сценарии"));
        AppendMenuW(g_editorModeMenu, MF_STRING, kIdEditorCampaigns,
                    L(L"Campaigns", L"Кампании"));
        AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_editorModeMenu),
                    L(L"Editor mode", L"Режим редактора"));
        AppendMenuW(g_gameMenu, MF_SEPARATOR, 0, nullptr);

        g_menuLanguageMenu = CreatePopupMenu();
        AppendMenuW(g_menuLanguageMenu, MF_STRING, kIdMenuLanguageAuto,
                    L(L"Auto (Windows / editor locale)", L"Авто (Windows / локаль редактора)"));
        AppendMenuW(g_menuLanguageMenu, MF_STRING, kIdMenuLanguageEn, L"English");
        AppendMenuW(g_menuLanguageMenu, MF_STRING, kIdMenuLanguageRu, L"Русский");
        AppendMenuW(g_menuLanguageMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_menuLanguageMenu, MF_STRING | MF_GRAYED, 0,
                    L(L"Applied after restarting the editor",
                      L"Применяется после перезапуска редактора"));
        AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_menuLanguageMenu),
                    L(L"Menu language", L"Язык меню"));

        g_localeMenu = CreatePopupMenu();
        AppendMenuW(g_localeMenu, MF_STRING, kIdLocaleNone,
                    L(L"No wrapper recoding", L"Без перекодировки врапером"));
        collectInstalledLocales();
        for (int i = 0; i < g_localeCount; ++i)
            AppendMenuW(g_localeMenu, MF_STRING, kIdLocaleBase + static_cast<UINT>(i),
                        g_localeOptions[i].label);
        AppendMenuW(g_localeMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_localeMenu, MF_STRING | MF_GRAYED, 0,
                    L(L"Writes [Wrapper] Locale in Disciple.ini; applies immediately",
                      L"Пишет [Wrapper] Locale в Disciple.ini; применяется сразу"));
        AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_localeMenu),
                    L(L"Editor text locale", L"Локализация текста редактора"));
    } else {
        // ===== "Game" - gameplay / animation =====
        g_gameMenu = CreatePopupMenu();
    // Keep the existing alwaysActive config/patch path intact, but do not expose it in the menu
    // until its game-side behavior is verified.
    AppendMenuW(g_gameMenu, MF_STRING | mnsDisabled, kIdDragScroll,
                L(L"(MNS/SMNS) Map drag-scroll - hold left button to pan the map",
                  L"(MNS/SMNS) Перетаскивание карты - зажать левую кнопку и тянуть"));
    // WideBattle remains installed and enabled by its existing default/config path, but is not
    // exposed in the 1.5 menu until the user-facing switch semantics are ready.
    AppendMenuW(g_gameMenu,
                MF_STRING |
                    ((g_ver == VerRussobit &&
                      (clouds_is_available() || clouds_get_enabled()))
                         ? 0u
                         : MF_GRAYED),
                kIdClouds, L(L"(MNS/SMNS) Map clouds (game option; restart)",
                             L"(MNS/SMNS) Облака на карте (опция игры; перезапуск)"));
    AppendMenuW(g_gameMenu, MF_STRING | MF_GRAYED, kIdCloudsInfo, L"...");
    AppendMenuW(g_gameMenu, MF_STRING | mnsDisabled, kIdDialogVo,
                L(L"(MNS/SMNS) Skip voiced event dialogs - auto-close after the voiceover",
                  L"(MNS/SMNS) Пропускать озвученные диалоги - авто-закрытие после озвучки"));
    AppendMenuW(g_gameMenu, MF_STRING | MF_GRAYED, kIdDialogVoInfo,
                L(L"    (their text is saved to dialog-vo-log.txt in the game folder)",
                  L"    (их текст пишется в dialog-vo-log.txt в папке игры)"));
    AppendMenuW(g_gameMenu, MF_STRING | mnsDisabled, kIdAutoConfirmUnitHire,
                L(L"(MNS/SMNS) Auto-confirm unit hire - skip the confirmation question",
                  L"(MNS/SMNS) Автоподтверждать найм воинов - не задавать вопрос"));
    AppendMenuW(g_gameMenu, MF_SEPARATOR, 0, nullptr);
    g_menuLanguageMenu = CreatePopupMenu();
    AppendMenuW(g_menuLanguageMenu, MF_STRING, kIdMenuLanguageAuto,
                L(L"Auto (Windows / game locale)", L"Авто (Windows / локаль игры)"));
    AppendMenuW(g_menuLanguageMenu, MF_STRING, kIdMenuLanguageEn, L"English");
    AppendMenuW(g_menuLanguageMenu, MF_STRING, kIdMenuLanguageRu, L"Русский");
    AppendMenuW(g_menuLanguageMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_menuLanguageMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Applied after restarting the game", L"Применяется после перезапуска игры"));
    AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_menuLanguageMenu),
                L(L"Menu language", L"Язык меню"));
    g_localeMenu = CreatePopupMenu();
    AppendMenuW(g_localeMenu, MF_STRING, kIdLocaleNone,
                L(L"No wrapper recoding", L"Без перекодировки врапером"));
    collectInstalledLocales();
    for (int i = 0; i < g_localeCount; ++i)
        AppendMenuW(g_localeMenu, MF_STRING, kIdLocaleBase + static_cast<UINT>(i),
                    g_localeOptions[i].label);
    AppendMenuW(g_localeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_localeMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Writes [Wrapper] Locale in Disciple.ini; applies immediately",
                  L"Пишет [Wrapper] Locale в Disciple.ini; применяется сразу"));
    AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_localeMenu),
                L(L"Game text locale", L"Локализация текста игры"));
    g_battleAnimMenu = CreatePopupMenu();
    AppendMenuW(g_battleAnimMenu, MF_STRING, kIdAnimOff, L(L"Off (vanilla)", L"Выкл (оригинал)"));
    AppendMenuW(g_battleAnimMenu, MF_STRING, kIdAnim1, L"1.5x");
    AppendMenuW(g_battleAnimMenu, MF_STRING, kIdAnim2, L(L"2x  (default)", L"2x  (по умолчанию)"));
    AppendMenuW(g_battleAnimMenu, MF_STRING, kIdAnim3, L"3x");
    AppendMenuW(g_battleAnimMenu, MF_STRING, kIdAnim4, L"4x");
    AppendMenuW(g_battleAnimMenu, MF_STRING, kIdAnim5, L"5x");
    AppendMenuW(g_battleAnimMenu, MF_STRING, kIdAnim6,
                L(L"Super fast (15x, test)", L"Супербыстро (15x, тест)"));
    AppendMenuW(g_battleAnimMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_battleAnimMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Speeds up ALL battle animation. Applies instantly, safe.",
                  L"Ускоряет ВСЕ анимации боя. Применяется сразу, безопасно."));
    AppendMenuW(g_gameMenu, MF_POPUP | mnsDisabled,
                reinterpret_cast<UINT_PTR>(g_battleAnimMenu),
                L(L"(MNS/SMNS) Battle speed (whole battle)",
                  L"(MNS/SMNS) Скорость боя (весь бой)"));
    g_battleAtkMenu = CreatePopupMenu();
    AppendMenuW(g_battleAtkMenu, MF_STRING, kIdAtkOff, L(L"Off", L"Выкл"));
    AppendMenuW(g_battleAtkMenu, MF_STRING, kIdAtk1, L"1.5x");
    AppendMenuW(g_battleAtkMenu, MF_STRING, kIdAtk2, L"2x");
    AppendMenuW(g_battleAtkMenu, MF_STRING, kIdAtk3, L"3x");
    AppendMenuW(g_battleAtkMenu, MF_STRING, kIdAtk4, L"4x");
    AppendMenuW(g_battleAtkMenu, MF_STRING, kIdAtk5, L(L"5x  (default)", L"5x  (по умолчанию)"));
    AppendMenuW(g_battleAtkMenu, MF_STRING, kIdAtk6,
                L(L"Super fast (15x, test)", L"Супербыстро (15x, тест)"));
    AppendMenuW(g_battleAtkMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_battleAtkMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Extra speed only while a hit plays; waiting units stay calm.",
                  L"Доп. ускорение только на время удара; ожидающие юниты спокойны."));
    AppendMenuW(g_gameMenu, MF_POPUP | mnsDisabled,
                reinterpret_cast<UINT_PTR>(g_battleAtkMenu),
                L(L"(MNS/SMNS) Attack speed-up (burst on each hit)",
                  L"(MNS/SMNS) Ускорение атак (рывок на каждый удар)"));
    g_mapAnimMenu = CreatePopupMenu();
    AppendMenuW(g_mapAnimMenu, MF_STRING, kIdAnimMapOff, L(L"Off (vanilla)", L"Выкл (оригинал)"));
    AppendMenuW(g_mapAnimMenu, MF_STRING, kIdAnimMap1, L"1.5x");
    AppendMenuW(g_mapAnimMenu, MF_STRING, kIdAnimMap2, L"2x");
    AppendMenuW(g_mapAnimMenu, MF_STRING, kIdAnimMap3, L"3x");
    AppendMenuW(g_mapAnimMenu, MF_STRING, kIdAnimMap4, L"4x");
    AppendMenuW(g_mapAnimMenu, MF_STRING, kIdAnimMap5, L"5x");
    AppendMenuW(g_mapAnimMenu, MF_STRING, kIdAnimMap6,
                L(L"Super fast (15x, test)", L"Супербыстро (15x, тест)"));
    AppendMenuW(g_mapAnimMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_mapAnimMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Speeds up map animation (water, flags, effects).",
                  L"Ускоряет анимации карты (вода, флаги, эффекты)."));
    AppendMenuW(g_gameMenu, MF_POPUP | mnsDisabled,
                reinterpret_cast<UINT_PTR>(g_mapAnimMenu),
                L(L"(MNS/SMNS) Map animation speed",
                  L"(MNS/SMNS) Скорость анимаций карты"));
    g_battleMenu = CreatePopupMenu();
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle1, L(L"Slow", L"Медленно"));
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle2, L(L"Normal", L"Нормально"));
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle3, L(L"Fast", L"Быстро"));
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle4, L(L"Instant", L"Мгновенно"));
    AppendMenuW(g_battleMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_battleMenu, MF_STRING | MF_GRAYED, 0,
                L(L"The game's own option; applies from the next battle.",
                  L"Родная опция игры; действует со следующего боя."));
    AppendMenuW(g_gameMenu, MF_POPUP | mnsDisabled,
                reinterpret_cast<UINT_PTR>(g_battleMenu),
                L(L"(MNS/SMNS) Battle speed (game option)",
                  L"(MNS/SMNS) Скорость боя (опция игры)"));
    g_mapMenu = CreatePopupMenu();
    AppendMenuW(g_mapMenu, MF_STRING, kIdMap1, L(L"Normal", L"Нормально"));
    AppendMenuW(g_mapMenu, MF_STRING, kIdMap2, L(L"Fast", L"Быстро"));
    AppendMenuW(g_mapMenu, MF_STRING, kIdMap3, L(L"Very fast", L"Очень быстро"));
    AppendMenuW(g_mapMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_mapMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Walk speed of your and enemy stacks (the game's own option).",
                  L"Скорость шага ваших и вражеских отрядов (родная опция игры)."));
    AppendMenuW(g_gameMenu, MF_POPUP | mnsDisabled,
                reinterpret_cast<UINT_PTR>(g_mapMenu),
                L(L"(MNS/SMNS) Map movement speed (game option)",
                  L"(MNS/SMNS) Скорость передвижения на карте (опция игры)"));
    }

    // ===== "Video" - look (all live except Renderer) =====
    g_videoMenu = CreatePopupMenu();
    g_modeMenu = CreatePopupMenu();
    for (int i = 0; i < kModeCount; ++i)
        AppendMenuW(g_modeMenu, MF_STRING, kIdModeWindowed + i, g_ru ? kModes[i].ru : kModes[i].en);
    AppendMenuW(g_modeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_modeMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Hotkeys: Alt+Enter follows this mode; F4 returns to/from a window",
                  L"Клавиши: Alt+Enter следует режиму; F4 возвращает в/из окна"));
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_modeMenu),
                L(L"Display mode", L"Режим экрана"));
    g_displaySizeMenu = CreatePopupMenu();
    if (g_ver != VerEditor) {
        const bool canvasMenuAvailable =
            horplus_is_available() || g_ver == VerRussobit;
        const UINT nativeFlags =
            MF_STRING | (canvasMenuAvailable ? 0u : MF_GRAYED);
        const UINT wideFlags =
            MF_STRING | (horplus_is_available() ? 0u : MF_GRAYED);
        AppendMenuW(g_displaySizeMenu, wideFlags, kIdHorplusAuto,
                    L(L"Automatic for monitor...",
                      L"Авто под монитор..."));
        AppendMenuW(g_displaySizeMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(
            g_displaySizeMenu, MF_STRING | MF_GRAYED, 0,
            L(L"Original game modes — restart required",
              L"Штатные режимы игры — нужен перезапуск"));
        AppendMenuW(g_displaySizeMenu, nativeFlags, kIdDisplaySize0,
                    g_ru ? kDisplaySizeLabelsRu[0] : kDisplaySizeLabelsEn[0]);
        AppendMenuW(g_displaySizeMenu, nativeFlags, kIdDisplaySize1,
                    g_ru ? kDisplaySizeLabelsRu[1] : kDisplaySizeLabelsEn[1]);
        AppendMenuW(g_displaySizeMenu, nativeFlags, kIdDisplaySize2,
                    g_ru ? kDisplaySizeLabelsRu[2] : kDisplaySizeLabelsEn[2]);
        AppendMenuW(g_displaySizeMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(
            g_displaySizeMenu, MF_STRING | MF_GRAYED, 0,
            horplus_is_available()
                ? L(L"Widescreen game view — more map after restart",
                    L"Широкий обзор — больше карты после перезапуска")
                : L(L"Widescreen game view — unavailable for this executable",
                    L"Широкий обзор — недоступен для этого exe"));
        for (int i = 0; i < kHorplusSizeCount; ++i) {
            AppendMenuW(g_displaySizeMenu, wideFlags,
                        kIdHorplusBase + static_cast<UINT>(i),
                        g_ru ? kHorplusSizes[i].ru : kHorplusSizes[i].en);
        }
        AppendMenuW(g_displaySizeMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_displaySizeMenu, MF_STRING | MF_GRAYED, kIdDisplaySizeState, L"...");
        AppendMenuW(g_displaySizeMenu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(
        g_displaySizeMenu, MF_STRING | MF_GRAYED, 0,
        L(L"Window / streaming output only — live; game view unchanged",
          L"Только окно / вывод для стрима — сразу; обзор не меняется"));
    AppendMenuW(g_displaySizeMenu, MF_STRING, kIdOutputSizeCustom,
                L(L"Change window/output only...",
                  L"Изменить только окно/вывод..."));
    g_resolutionMenuPosition = GetMenuItemCount(g_videoMenu);
    AppendMenuW(g_videoMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(g_displaySizeMenu),
                g_ver == VerEditor
                    ? L(L"Resolution / window size...",
                        L"Разрешение / размер окна...")
                    : L(L"Resolution...", L"Разрешение..."));
    AppendMenuW(
        g_videoMenu,
        MF_STRING |
            ((horplus_is_available() && decorative_is_available())
                 ? 0u
                 : MF_GRAYED),
        kIdDecorativeBackground,
        L(L"Decorative background around classic 4:3 screens",
          L"Декоративный фон вокруг классических экранов 4:3"));
    // Old and hand-edited width/height values remain supported through the same popup, without a
    // second neighboring "resolution" concept in the Video menu.
    g_resMenu = nullptr;
    g_scaleMenu = CreatePopupMenu();
    AppendMenuW(g_scaleMenu, MF_STRING, kIdMaintas,
                L(L"Fit - preserve the selected game aspect",
                  L"Вписать - сохранить выбранные пропорции игры"));
    AppendMenuW(g_scaleMenu, MF_STRING, kIdBoxing,
                L(L"Integer pixel blocks - 2x means 2x2 = 4 output pixels",
                  L"Целые блоки пикселей - 2x значит 2x2 = 4 пикселя вывода"));
    AppendMenuW(g_scaleMenu, MF_STRING, kIdScaleStretch,
                L(L"Stretch - fill output (distorts geometry)",
                  L"Растянуть - заполнить вывод (искажает геометрию)"));
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleCustom, L"...");
    AppendMenuW(g_scaleMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleGeometry, L"...");
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleResult, L"...");
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleFilterInfo, L"...");
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_scaleMenu),
                L(L"Scaling", L"Масштаб"));
    g_shaderMenu = CreatePopupMenu();
    for (int i = 0; i < kShaderCount; ++i)
        AppendMenuW(g_shaderMenu, MF_STRING, kIdShaderBase + i,
                    g_ru ? kShaders[i].ru : kShaders[i].en);
    AppendMenuW(g_shaderMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_shaderMenu, MF_STRING | MF_GRAYED, 0,
                L(L"OpenGL renderer only", L"Только для рендерера OpenGL"));
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_shaderMenu),
                L(L"Filter", L"Фильтр"));
    g_rendMenu = CreatePopupMenu();
    for (int i = 0; i < kRendererCount; ++i)
        AppendMenuW(g_rendMenu, MF_STRING, kIdRendOpenGL + i,
                    g_ru ? kRenderers[i].ru : kRenderers[i].en);
    AppendMenuW(g_rendMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_rendMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Applies after a game restart", L"Применяется после перезапуска игры"));
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_rendMenu),
                L(L"Renderer (restart)", L"Рендерер (рестарт)"));
    AppendMenuW(g_videoMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_videoMenu, MF_STRING, kIdVsync,
                L(L"VSync - fixes tearing in exclusive fullscreen (a bit more lag)",
                  L"VSync - лечит разрывы в эксклюзивном фулскрине (чуть больше задержка)"));
    AppendMenuW(g_videoMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_videoMenu, MF_STRING, kIdScreenshot,
                L(L"Take screenshot (PrintScreen)", L"Сделать скриншот (PrintScreen)"));

    // ===== "Performance" - frame/CPU caps; affinity applies after restart =====
    g_perfMenu = CreatePopupMenu();
    g_fpsMenu = CreatePopupMenu();
    for (int i = 0; i < kFpsCount; ++i)
        AppendMenuW(g_fpsMenu, MF_STRING, kIdFpsBase + i,
                    g_ru ? kFpsLabelsRu[i] : kFpsLabelsEn[i]);
    AppendMenuW(g_fpsMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_fpsMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Render only - does not slow the game itself",
                  L"Только рендер - саму игру не замедляет"));
    AppendMenuW(g_perfMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_fpsMenu),
                L(L"Frame cap (restart)", L"Кап FPS (рестарт)"));
    g_ticksMenu = CreatePopupMenu();
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks0,
                L(L"Uncapped (high CPU)", L"Без капа (грузит CPU)"));
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks30,
                L(L"30 (cool CPU, sluggish)", L"30 (холодный CPU, задумчиво)"));
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks60, L"60");
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks100,
                L(L"100 (smoothest, default)", L"100 (плавнее всего, по умолчанию)"));
    AppendMenuW(g_ticksMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_ticksMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Game core speed: low values make the game think before every action",
                  L"Скорость ядра игры: низкие значения = пауза перед каждым действием"));
    AppendMenuW(g_perfMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_ticksMenu),
                L(L"Game speed cap (restart)", L"Кап скорости игры (рестарт)"));
    AppendMenuW(g_perfMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_perfMenu, MF_STRING, kIdSingleCpu,
                L(L"1 CPU stability (restart)",
                  L"1 CPU: стабильность (рестарт)"));
    AppendMenuW(g_perfMenu, MF_STRING | MF_GRAYED, 0,
                L(L"If the game randomly crashes on any map, enable this option.",
                  L"Если игра случайно вылетает на любой карте — включите эту опцию."));
    AppendMenuW(g_perfMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Enabled by default; a full game restart is required.",
                  L"По умолчанию включено; нужен полный перезапуск игры."));

    g_bar = CreateMenu();
    AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_gameMenu),
                g_ver == VerEditor ? L(L"File", L"Файл") : L(L"Game", L"Игра"));
    AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_videoMenu), L(L"Video", L"Видео"));
    AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_perfMenu),
                L(L"Performance", L"Производительность"));

    // ===== "Plugins" - native Mods\*.c4p plugins =====
    // Surface each plugin and graft its config submenu directly under the top-level popup.
    const int pc = pluginhost_count();
    if (pc > 0) {
        HMENU plugins = CreatePopupMenu();
        for (int i = 0; i < pc; ++i) {
            const char* nm = pluginhost_name(i);
            HMENU pm = reinterpret_cast<HMENU>(pluginhost_menu(i));
            if (pm) // the plugin's own config submenu (c4p_menu); commands route via c4p_command
                AppendMenuA(plugins, MF_POPUP, reinterpret_cast<UINT_PTR>(pm), nm);
            else
                AppendMenuA(plugins, MF_STRING | MF_GRAYED, 0, nm);
        }
        AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(plugins),
                    L(L"Plugins", L"Плагины"));
    }
}

// Keep the Win32 menu only in a normal window. cnc-ddraw removes it in exclusive mode; our old
// worker immediately put it back, producing a 1.5-second detach/reattach/reload flicker loop. The
// worker now only posts a message and this function owns all chrome mutations on the GUI thread.
using KeyboardHookFn = LRESULT(CALLBACK*)(int, WPARAM, LPARAM);
KeyboardHookFn g_origKeyboardHook = &keyboard_hook_proc;
volatile LONG g_keyboardObserverState = 0; // 0=not installed, 1=installing, 2=installed
HWND g_keyboardObserverHwnd = nullptr;

LRESULT CALLBACK keyboardHookObserver(int code, WPARAM wParam, LPARAM lParam)
{
    const int before = DDGetDisplayMode();
    const LRESULT result = g_origKeyboardHook(code, wParam, lParam);
    const int after = DDGetDisplayMode();

    // cnc-ddraw's WH_KEYBOARD hook consumes Alt+Enter (and any configured secondary hotkey), so no
    // WndProc key message follows. Marshal one private message after its synchronous mode change.
    if (before >= 0 && after >= 0 && before != after && g_relayoutMsg && g_keyboardObserverHwnd)
        PostMessageA(g_keyboardObserverHwnd, g_relayoutMsg, 0, 0);

    return result;
}

void installKeyboardObserver(HWND hwnd)
{
    g_keyboardObserverHwnd = hwnd;
    if (InterlockedCompareExchange(&g_keyboardObserverState, 1, 0) != 0)
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(g_origKeyboardHook),
                 reinterpret_cast<void*>(&keyboardHookObserver));
    if (DetourTransactionCommit() == NO_ERROR) {
        InterlockedExchange(&g_keyboardObserverState, 2);
        mlog("[menu] cnc-ddraw window-mode hotkey observer installed");
    } else {
        g_origKeyboardHook = &keyboard_hook_proc;
        InterlockedExchange(&g_keyboardObserverState, 0); // retry on the next health-sync message
        mlog("[menu] cnc-ddraw window-mode hotkey observer install failed");
    }
}

void maybeNotifyAdaptiveFullscreen(HWND hwnd, int liveMode)
{
    // Auto is deliberately based on the physical monitor. Borderless uses
    // that exact output. An explicitly lower exclusive video mode is a
    // separate renderer choice and must not masquerade as a new monitor
    // recommendation that Auto would only undo on the next start.
    if (!hwnd || liveMode != 1 || !horplus_is_available())
        return;

    int requestedMode = 0, requestedW = 0, requestedH = 0;
    if (!horplus_get_requested(&requestedMode, &requestedW, &requestedH) ||
        requestedMode != 2)
        return; // a manual game resolution is always respected silently

    int gameW = 0, gameH = 0, outputW = 0, outputH = 0;
    if (!DDGetScaleMetrics(&gameW, &gameH, &outputW, &outputH,
                           nullptr, nullptr, nullptr, nullptr) ||
        gameW <= 0 || gameH <= 0 || outputW <= 0 || outputH <= 0)
        return;

    int selectedW = 0, selectedH = 0;
    int nativeDisplaySize = -1;
    if (!horplus_get_adaptive_for_output(outputW, outputH,
                                         &selectedW, &selectedH,
                                         &nativeDisplaySize) ||
        (gameW == selectedW && gameH == selectedH))
        return;

    if (g_adaptiveNoticeOutputW == outputW &&
        g_adaptiveNoticeOutputH == outputH &&
        g_adaptiveNoticeCanvasW == selectedW &&
        g_adaptiveNoticeCanvasH == selectedH)
        return;

    g_gameHwnd = hwnd;
    showAdaptiveResolutionRestartModal(gameW, gameH, outputW, outputH,
                                       selectedW, selectedH,
                                       nativeDisplaySize);
}

void syncChrome(HWND hwnd)
{
    // Install only after cnc-ddraw has created its GUI-thread WH_KEYBOARD hook. Its callback address
    // is externally taken (not inlineable), making this a stable observation point for Alt+Enter and
    // custom fullscreen hotkeys without changing their renderer semantics.
    installKeyboardObserver(hwnd);

    const int liveMode = DDGetDisplayMode();
    if (liveMode < 0 || !g_bar)
        return;

    const bool wantMenu = liveMode == 0;
    const bool hasOurMenu = GetMenu(hwnd) == g_bar;
    bool changed = false;

    if (wantMenu && !hasOurMenu) {
        changed = SetMenu(hwnd, g_bar) != FALSE;
    } else if (!wantMenu && hasOurMenu) {
        setNonClientCursor(false);
        changed = SetMenu(hwnd, nullptr) != FALSE;
    }

    g_liveModeIdx = liveMode;
    int persistsNextStart = 0;
    if (DDGetOutputConfig(nullptr, nullptr, &persistsNextStart) &&
        persistsNextStart)
        g_modeIdx = liveMode;
    refreshChecks();
    maybeNotifyAdaptiveFullscreen(hwnd, liveMode);

    if (changed) {
        DrawMenuBar(hwnd);
        // Recompute the client area and mouse viewport without cfg_load(): Alt+Enter/F4 changes are
        // live-only, and re-reading ddraw.ini here would immediately undo the hotkey transition.
        DDRelayoutCurrentMode();
        mlog("[menu] chrome %s (mode=%d)", wantMenu ? "attached" : "detached", liveMode);
    }
}

struct FindCtx
{
    DWORD pid;
    HWND found;
    long long area;
};

BOOL CALLBACK findGameWindow(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == ctx->pid && IsWindowVisible(hwnd)) {
        // Match the game's MAIN window by class 'MQ_UIManager' (title may be empty). Several
        // MQ_UIManager windows exist, including a zero-size helper. Pick the largest visible one;
        // a fixed 640x400 threshold would make deliberately downscaled windows lose the menu.
        char cls[64] = {};
        GetClassNameA(hwnd, cls, sizeof(cls));
        if (lstrcmpA(cls, "MQ_UIManager") == 0) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            const long long width = rc.right - rc.left;
            const long long height = rc.bottom - rc.top;
            const long long area = width > 0 && height > 0 ? width * height : 0;
            if (area > ctx->area) {
                ctx->found = hwnd;
                ctx->area = area;
            }
        }
    }
    return TRUE;
}

DWORD WINAPI menuWorker(LPVOID)
{
    buildMenu();
    HWND hwnd = nullptr;
    for (int i = 0; i < 800 && !hwnd; ++i) { // wait up to ~200s for the game window
        FindCtx ctx{GetCurrentProcessId(), nullptr, 0};
        EnumWindows(&findGameWindow, reinterpret_cast<LPARAM>(&ctx));
        hwnd = ctx.found;
        if (!hwnd)
            Sleep(250);
    }
    if (!hwnd) {
        mlog("[menu] game window not found; menu bar not installed");
        return 0;
    }
    bool announced = false;
    for (;;) {
        // SetMenu/DrawMenuBar are deliberately NOT called from this worker. Posting repeatedly also
        // catches cnc-ddraw's live Alt+Enter transition without racing its renderer restart.
        if (g_relayoutMsg)
            PostMessageA(hwnd, g_relayoutMsg, 0, 0);
        if (!announced) {
            mlog("[menu] chrome sync scheduled on hwnd %p", reinterpret_cast<void*>(hwnd));
            announced = true;
        }
        Sleep(1500);
    }
}

void installWndProcDetour()
{
    const uintptr_t va = gameWndProcVA();
    if (!va) {
        mlog("[menu] no WndProc address for this game version; menu disabled");
        return;
    }
    g_origWndProc = reinterpret_cast<WndProcFn>(va);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(g_origWndProc), wndProcHook);
    if (DetourTransactionCommit() == NO_ERROR)
        mlog("[menu] game WndProc detour installed (%#x)", (unsigned)va);
    else
        mlog("[menu] game WndProc detour failed");
}

} // namespace

/*
 * Address-free renderer message bridge. cnc-ddraw's fake WndProc calls this before forwarding to
 * the application's WndProc. MNS/SMNS keeps its validated native detour; ScenEdit and other game
 * builds use this bridge for the universal renderer/menu commands.
 */
extern "C" int featuremenu_renderer_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            LRESULT* result)
{
    if (g_ver == VerRussobit || !result)
        return 0;

    if (!g_gameHwnd)
        g_gameHwnd = hwnd;

    switch (msg) {
    case WM_NCMOUSEMOVE:
    case WM_ENTERMENULOOP:
        setNonClientCursor(true);
        break;
    case WM_MOUSEMOVE:
        setNonClientCursor(false);
        break;
    default:
        break;
    }

    if (msg == WM_KEYDOWN && wParam == VK_F4 && !(lParam & 0x40000000)) {
        DDToggleWindowedMode();
        syncChrome(hwnd);
        *result = 0;
        return 1;
    }

    if (g_relayoutMsg && msg == g_relayoutMsg) {
        syncChrome(hwnd);
        *result = 0;
        return 1;
    }

    if (msg == WM_COMMAND && lParam == 0 && HIWORD(wParam) == 0) {
        const UINT id = LOWORD(wParam);
        if (id >= kIdAlwaysActive && id <= kIdLast) {
            onMenuCommand(id);
            *result = 0;
            return 1;
        }
        if (id >= 0xB000 && id < 0xC000 && pluginhost_command(id)) {
            *result = 0;
            return 1;
        }
    } else if (msg == WM_INITMENUPOPUP) {
        refreshChecks();
    }

    return 0;
}

// ===== Map drag-scroll (DGL-faithful grab+drag pan) =====
// Detours the in-game iso-view mouse handler (CStratInterf sub_48E8A0). While g_dragScroll is on, a
// left-press on open map terrain grabs the tile under the cursor; dragging pans so it stays put (like
// DGL MouseScroll). A press-release with no movement is replayed so a plain click still selects.
// Russobit addresses RE'd from the DGL backup + game exe. Game calls are __thiscall via typedefs
// (this file is /Gd); every game deref is SEH + isUserPtr guarded.
struct PointI { int x, y; };

using IsoMouseFn    = int   (__fastcall*)(void* view, void* edx, int msgId, PointI* pt); // __thiscall target
using ScreenToMapFn = int   (__thiscall*)(void* mg, PointI* screen, PointI* outTile, PointI* outPx);
using GetMapCenterFn = void (__thiscall*)(void* mg, PointI* mapCenter, int* offsetX, int* offsetY);
using SetMapCenterFn = void (__thiscall*)(void* mg, PointI* mapCenter, int offsetX, int offsetY);

void* g_origIsoMouse = nullptr; // Detours trampoline to the original CStratInterf iso mouse handler
void* g_origScrollDir = nullptr; // trampoline to the game's directional map scroll (edge-scroll executor)
int g_scrollDirDiag = 0;         // first-N diagnostic counter for the edge-scroll hook
bool g_dragScrollActive = false;
bool g_dragMoved = false;
PointI g_dragMapCenter{};     // exact center tile returned by the game at button-down
PointI g_dragPointerAnchor{}; // button-down cursor plus the center's sub-tile screen offset
PointI g_dragStart{};         // cursor at button-down (click-vs-drag detection)
constexpr int kDragStartThreshold = 1; // first changed game pixel starts the drag

bool dragThresholdExceeded(int x, int y)
{
    const int dx = x - g_dragStart.x;
    const int dy = y - g_dragStart.y;
    return dx <= -kDragStartThreshold || dx >= kDragStartThreshold
        || dy <= -kDragStartThreshold || dy >= kDragStartThreshold;
}

int callOrigIsoMouse(void* view, int msgId, PointI* pt)
{
    return reinterpret_cast<IsoMouseFn>(g_origIsoMouse)(view, nullptr, msgId, pt);
}

// MapGraphics singleton = SmartPtr<MapGraphics> { int* refCount; MapGraphics* data; } @0x837DA0.
// data field (offset 4) IS the MapGraphics - ONE deref (verified in sub_48E701). A two-deref read
// lands in MapGraphics+0 (a C2DEngine*) and faults in sub_5418BA.
void* mapGraphicsPtr()
{
    __try {
        void* mg = *reinterpret_cast<void**>(0x837DA4);
        return isUserPtr(mg) ? mg : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Pan from the exact center+offset snapshot captured on button-down. The game's center-on-tile
// (sub_541588) snaps scroll to the iso grid ctx[7]xctx[8], so a continuous drag steps by the grid (the
// jerk). Temporarily set the grid to 1 around the pan, then restore so click-to-center keeps its snap.
// Deref mg->engine2d->e0->ctx, grid @ctx+28/+32; isUserPtr-guarded.
void panMapCenterSmooth(void* mg, PointI* mapCenter, int dx, int dy)
{
    void* engine2d = isUserPtr(mg) ? *reinterpret_cast<void**>(mg) : nullptr;
    void* e0 = isUserPtr(engine2d) ? *reinterpret_cast<void**>(engine2d) : nullptr;
    void* ctx = isUserPtr(e0) ? *reinterpret_cast<void**>(reinterpret_cast<char*>(e0) + 4) : nullptr;
    if (isUserPtr(ctx)) {
        int* gx = reinterpret_cast<int*>(reinterpret_cast<char*>(ctx) + 28);
        int* gy = reinterpret_cast<int*>(reinterpret_cast<char*>(ctx) + 32);
        const int sx = *gx, sy = *gy; // snapshot (a fault here is before any write -> caller __except, clean)
        static int logged = 0;
        if (!logged) { logged = 1; mlog("[drag] iso scroll grid = %d x %d", sx, sy); }
        // __try/__finally so the LIVE game grid is ALWAYS restored: if sub_541588 faults, the callers'
        // __except would otherwise unwind past the restore and leave the grid pinned at 1x1, killing the
        // native scroll snap (edge-scroll / click-to-center) for the rest of the scenario.
        __try {
            *gx = 1; *gy = 1;
            reinterpret_cast<SetMapCenterFn>(0x541588)(mg, mapCenter, dx, dy);
        } __finally {
            *gx = sx; *gy = sy;
        }
    } else {
        reinterpret_cast<SetMapCenterFn>(0x541588)(mg, mapCenter, dx, dy);
    }
}

int __fastcall isoMouseHook(void* view, void* /*edx*/, int msgId, PointI* pt)
{
    if (!g_dragScroll)
        return callOrigIsoMouse(view, msgId, pt);

    __try {
        if (!pt)
            return callOrigIsoMouse(view, msgId, pt);

        if (msgId == WM_LBUTTONDOWN) {
            void* mg = mapGraphicsPtr();
            PointI tile{}, mapCenter{}, centerOffset{};
            // screen->map returns false over the ~160px minimap/resource corner -> only grab real map
            if (mg && reinterpret_cast<ScreenToMapFn>(0x5418BA)(mg, pt, &tile, nullptr)) {
                // sub_5414BC has no meaningful return value; DisciplesGL also ignores EAX here.
                reinterpret_cast<GetMapCenterFn>(0x5414BC)(
                    mg, &mapCenter, &centerOffset.x, &centerOffset.y);
                // Preserve the same center+sub-tile offset invariant as the original wrapper. The
                // cursor can then move that exact point immediately, without re-anchoring to a tile
                // center on the first WM_MOUSEMOVE.
                g_dragStart = *pt;
                g_dragMapCenter = mapCenter;
                g_dragPointerAnchor = {
                    pt->x + centerOffset.x,
                    pt->y + centerOffset.y,
                };
                g_dragScrollActive = true;
                g_dragMoved = false;
                // Capture the mouse so WM_MOUSEMOVE keeps reaching this handler while the button is held -
                // the game stops routing moves to the iso handler during a held button (DGL captured too).
                if (g_gameHwnd)
                    SetCapture(g_gameHwnd);
                return 1; // consume the down so the game does not select yet
            }
        } else if (g_dragScrollActive && msgId == WM_MOUSEMOVE) {
            // One unchanged sample is still a click; the first changed game pixel is a drag.
            if (!g_dragMoved && !dragThresholdExceeded(pt->x, pt->y))
                return 1;
            g_dragMoved = true;
            void* mg = mapGraphicsPtr();
            if (mg)
                panMapCenterSmooth(mg, &g_dragMapCenter,
                                   g_dragPointerAnchor.x - pt->x,
                                   g_dragPointerAnchor.y - pt->y);
            return 1; // consume moves while panning
        } else if (g_dragScrollActive && msgId == WM_LBUTTONUP) {
            g_dragScrollActive = false;
            ReleaseCapture();
            if (!g_dragMoved) {
                // Plain click: replay the down so it selects/opens. Do NOT then forward the up - the
                // replayed down can open a dialog (e.g. a city) and tear down the iso view, so forwarding
                // the up to that stale view derefs a freed child (game sub_5CA3F1, this[3]==NULL) -> crash.
                callOrigIsoMouse(view, WM_LBUTTONDOWN, pt);
                return 1;
            }
            return callOrigIsoMouse(view, msgId, pt); // a real drag: view intact, forward the up to end it
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dragScrollActive = false;
        return 1; // transient torn-down view mid dialog-transition: swallow, do NOT re-dispatch (re-crashes)
    }
    return callOrigIsoMouse(view, msgId, pt);
}

// WndProc-driven pan (called from wndProcHook with the cursor already mapped to GAME coords). The iso
// vtable handler stops receiving moves while the button is held; the captured WndProc does not.
void dragScrollWndMove(int gameX, int gameY)
{
    // A mapped coordinate change of one game pixel is already a drag. With no
    // changed sample, button-up still replays the ordinary selection click.
    if (!g_dragMoved && !dragThresholdExceeded(gameX, gameY))
        return;
    g_dragMoved = true; // a real drag (the button-up will not replay a select-click)
    __try {
        void* mg = mapGraphicsPtr();
        if (!mg)
            return;
        panMapCenterSmooth(mg, &g_dragMapCenter,
                           g_dragPointerAnchor.x - gameX,
                           g_dragPointerAnchor.y - gameY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The game's iso DIRECTIONAL scroll: sub_54249C -> sub_541BC1 -> sub_54301B. Single choke point for
// window-EDGE scroll (sub_541BC1 reached only through here). Programmatic centering uses sub_541588, so
// gating this kills edge-scroll without touching click-to-center or our drag-pan. Suppress it only
// during an actual held-button drag; merely enabling drag-scroll must not disable native edge-scroll.
// __thiscall(self, dir) via __fastcall(ecx=self, edx, dir); installed unconditionally.
char __fastcall scrollDirHook(void* self, void* /*edx*/, int dir)
{
    if (g_scrollDirDiag < 12) { ++g_scrollDirDiag; mlog("[edge] scrollDir dir=%d dragging=%d", dir, g_dragScrollActive ? 1 : 0); }
    if (g_dragScrollActive)
        return 0; // avoid fighting the grab-pan only while the button is held
    return reinterpret_cast<char(__fastcall*)(void*, void*, int)>(g_origScrollDir)(self, nullptr, dir);
}

void installDragScrollDetour()
{
    g_origIsoMouse = reinterpret_cast<void*>(0x48E8A0);
    g_origScrollDir = reinterpret_cast<void*>(0x54249C);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g_origIsoMouse, reinterpret_cast<void*>(isoMouseHook));
    DetourAttach(&g_origScrollDir, reinterpret_cast<void*>(scrollDirHook));
    if (DetourTransactionCommit() != NO_ERROR) {
        g_origIsoMouse = reinterpret_cast<void*>(0x48E8A0);
        g_origScrollDir = reinterpret_cast<void*>(0x54249C);
        mlog("[menu] drag-scroll/edge-scroll detours FAILED (0x48E8A0/0x54249C)");
    } else {
        mlog("[menu] drag-scroll + edge-scroll detours installed (0x48E8A0 iso, 0x54249C edge; default %s)",
             g_dragScroll ? "on" : "off");
    }
}

extern "C" void timerhost_install(void); // timer keystone (features/timerhost.cpp)

// Entry point called from cnc-ddraw's DllMain (DLL_PROCESS_ATTACH), after hook_init + the embed.
extern "C" void featuremenu_install(void)
{
    detectVersion();

    // Exact-address gameplay features are limited to the validated MNS/SMNS
    // layout. Other builds still receive the address-free renderer menu; its
    // MNS/SMNS-labelled entries are visible but disabled.
    if (g_ver != VerRussobit && g_ver != VerEditor)
        mlog("[menu] game exe %lu bytes: generic menu; MNS/SMNS patches disabled", g_exeSize);

    if (horplus_is_available() || g_ver == VerRussobit) {
        migrateLegacyDisplaySize();
        readDisplaySize();
    }

    // First run: generate a commented C4menu.ini (converting any old mss32menu.ini). This seed step
    // itself does not touch the game's own Disciple.ini / settings.lua; the explicit native-canvas
    // migration above is the sole startup exception.
    seedConfigFirstRun();

    const char* f = iniFile();
    // Menu language: [menu] language = auto|en|ru. auto = Russian when the Windows UI language is
    // Russian or the system codepage is 1251 (Russobit audience runs both kinds of systems).
    char lang[8] = {};
    GetPrivateProfileStringA("menu", "language", "auto", lang, sizeof(lang), f);
    if (lstrcmpiA(lang, "ru") == 0) {
        g_menuLanguage = 2;
        g_ru = true;
    } else if (lstrcmpiA(lang, "en") == 0) {
        g_menuLanguage = 1;
        g_ru = false;
    } else {
        g_menuLanguage = 0;
        g_ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN || GetACP() == 1251 ||
               PRIMARYLANGID(LANGIDFROMLCID(localization_get_locale())) == LANG_RUSSIAN;
    }

    // menuWorker posts this periodically; the GUI thread alone attaches/detaches the menu and
    // relayouts the renderer (generic builds/ScenEdit via fake_WndProc, MNS/SMNS via wndProcHook).
    g_relayoutMsg = RegisterWindowMessageA("C4dllR_MenuRelayout");

    if (g_ver == VerEditor) {
        readEditorDatabase();
        HANDLE thread = CreateThread(nullptr, 0, &menuWorker, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        mlog("[menu] ScenEdit menu scheduled (database=%s, exe=%lu bytes)",
             g_editorDatabase ? "campaigns" : "scenarios", g_exeSize);
        return;
    }

    // ---- Game menu state; exact hooks are installed only for MNS/SMNS below ----
    g_alwaysActive = GetPrivateProfileIntA("menu", "alwaysActive", 0, f) != 0;
    // Defaults: battle animation x2, attack burst x5 (fast hits, calm-ish battle). Map anim off.
    g_battleAnimEnabled = GetPrivateProfileIntA("menu", "battleAnimEnabled", 1, f) != 0;
    g_battleAnimSpeed = GetPrivateProfileIntA("menu", "battleAnimSpeed", 2, f);
    if (g_battleAnimSpeed < 1)
        g_battleAnimSpeed = 1;
    if (g_battleAnimSpeed > 6)
        g_battleAnimSpeed = 6;
    g_mapAnimEnabled = GetPrivateProfileIntA("menu", "mapAnimEnabled", 0, f) != 0;
    g_mapAnimSpeed = GetPrivateProfileIntA("menu", "mapAnimSpeed", 5, f);
    if (g_mapAnimSpeed < 1)
        g_mapAnimSpeed = 1;
    if (g_mapAnimSpeed > 6)
        g_mapAnimSpeed = 6;
    g_battleAttackEnabled = GetPrivateProfileIntA("menu", "battleAttackEnabled", 1, f) != 0;
    g_battleAttackSpeed = GetPrivateProfileIntA("menu", "battleAttackSpeed", 5, f);
    if (g_battleAttackSpeed < 1)
        g_battleAttackSpeed = 1;
    if (g_battleAttackSpeed > 6)
        g_battleAttackSpeed = 6;
    g_dragScroll = GetPrivateProfileIntA("menu", "dragScroll", 1, f) != 0;
    widebattle_set_enabled(GetPrivateProfileIntA("menu", "wideBattle", 1, f) != 0);
    g_dialogVoSkip = GetPrivateProfileIntA("menu", "dialogVoSkip", 0, f) != 0;
    g_autoConfirmUnitHire =
        GetPrivateProfileIntA("menu", "autoConfirmUnitHire", 0, f) != 0;
    if (g_ver == VerRussobit) {
        // Apply now (DllMain, before the game's main loop / anim init). The IAT is already populated by
        // the loader, so the time-scale hook installs cleanly; the battle discriminator patches the
        // idle vftable.
        applyAlwaysActive(g_alwaysActive);
        installTimeScaleHook();
        installBattleDiscriminator();
        timerhost_install(); // capture dialog/battle buttons + combat/animation state
        installDragScrollDetour(); // map grab+drag panning; pass-through when off
        dvoInstall(); // voiced-dialog auto-skip + logger; pass-through when off
        installUnitHireConfirmHook(); // X005TA0285; pass-through unless enabled
        applyAnimSpeed(0, g_battleAnimEnabled, g_battleAnimSpeed);
        applyAnimSpeed(1, g_mapAnimEnabled, g_mapAnimSpeed);
        installWndProcDetour();
    }

    HANDLE thread = CreateThread(nullptr, 0, &menuWorker, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    mlog("[menu] feature menu scheduled (alwaysActive=%d, battleAnim=%d/%d, mapAnim=%d/%d)",
         g_alwaysActive ? 1 : 0, g_battleAnimEnabled ? 1 : 0, g_battleAnimSpeed,
         g_mapAnimEnabled ? 1 : 0, g_mapAnimSpeed);
}
