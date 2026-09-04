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
#include "eventtrace.h"
#include "messagebatch.h"
#include "c4trace.h"
#include "inisettingsreset.h"
#include "wrapperdefaults.h"
#pragma intrinsic(_ReturnAddress)

// Renderer bridge (rendererbridge.c, same module): DDReloadConfig re-reads ddraw.ini + dd_SetDisplayMode;
// DDTakeScreenshot saves a PNG. Both no-op safely before the renderer is initialized.
extern "C" void DDReloadConfig(void);
extern "C" int DDReloadConfigForMenu(int outputSizeChanged,
                                       int displayModeChanged,
                                       int rendererChanged);
extern "C" int DDGetActiveRenderer(void);
extern "C" int DDGetActivePortableFilter(void);
extern "C" int DDGetRendererSwitchError(void);
extern "C" int DDSetMaxGameTicksLive(int ticks);
extern "C" void DDRelayoutCurrentMode(void);
extern "C" int DDGetDisplayMode(void);
extern "C" void DDNormalizeLegacyExclusive(void);
extern "C" void DDToggleWindowedMode(void);
extern "C" void DDCompleteWindowedModeToggle(void);
extern "C" int DDIsWindowModeToggleHotkey(int code, WPARAM key, LPARAM hookFlags);
extern "C" void DDTakeScreenshot(void);
extern "C" int DDGetScaleMetrics(int* gameWidth, int* gameHeight, int* outputWidth,
                                  int* outputHeight, int* viewportX, int* viewportY,
                                  int* viewportWidth, int* viewportHeight);
extern "C" int DDGetOutputConfig(int* width, int* height,
                                   int* persistsNextStart);
extern "C" void DDSetOutputConfigMemory(int width, int height);
extern "C" int DDGetSimpleZoom1000(void);
extern "C" int DDGetSimpleZoomExtra1000(void);
extern "C" int DDGetWindowStretchPercent(void);
extern "C" int DDIsWindowStretchActive(void);
extern "C" int DDCalcWindowStretchCrop(
    int gameWidth, int gameHeight, int percent,
    int* left, int* top, int* cropWidth, int* cropHeight);
extern "C" void DDSetWindowStretchPercent(int percent);
extern "C" void DDApplySimpleZoomMouse(int* x, int* y,
                                         int gameWidth, int gameHeight);
extern "C" int DDReadConfigString(const char* key, const char* defaultValue,
                                    char* value, unsigned int capacity);
extern "C" int DDWriteConfigString(const char* key, const char* value);
extern "C" int DDGetConfigWriteTarget(char* path, unsigned int pathCapacity,
                                       char* section, unsigned int sectionCapacity);
extern "C" void DDExitClientAfterSettingsChange(int discardOldWindowState);
extern "C" void DDEnableD2CursorOwnership(void);
extern "C" HCURSOR DDSetPhysicalCursor(HCURSOR cursor);
extern "C" BOOL DDGetPhysicalCursorPos(POINT* point);
extern "C" BOOL DDPhysicalScreenToClient(HWND hwnd, POINT* point);
extern "C" BOOL DDGetPhysicalClientRect(HWND hwnd, RECT* rect);
extern "C" HWND DDGetPhysicalForegroundWindow(void);
extern "C" HWND DDPhysicalWindowFromPoint(POINT point);
extern "C" int cursorcapture_install(void);
extern "C" int cursorcapture_is_available(void);
extern "C" void cursorcapture_set_suppressed(int suppressed);
extern "C" void cursorcapture_clear(void);
extern "C" void fastai_install(void);
extern "C" int fastai_set_enabled(int enabled);
extern "C" int fastai_get_enabled(void);
extern "C" int fastai_is_available(void);
extern "C" void fastai_pump(void);
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
extern "C" int horplus_get_decor_layout(int* contentWidth,
                                           int* contentHeight,
                                           int* wideBattle);
extern "C" void horplus_set_window_stretch_percent(int percent);

// Presentation-only DisciplesGL Alternative wallpaper/frame for fixed 4:3 screens.
extern "C" int decorative_get_enabled(void);

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
extern "C" int pluginhost_mouse(UINT msg, WPARAM wParam); // logical game-space input for plugins
extern "C" void pluginhost_menu_loop(int active); // keep overlay pixels below tracked native menus
extern "C" void pluginhost_refresh_menus(void); // refresh live plugin checks at WM_INITMENUPOPUP
extern "C" int pluginhost_key(UINT msg, WPARAM wParam, LPARAM lParam); // plugin shortcuts
extern "C" int pluginhost_battle_state_message(UINT msg); // queued battle-scope lifecycle edge
extern "C" int timerhost_auto_battle_message(UINT msg); // queued native TOG_AUTOBATTLE command

// Map drag-scroll lives in global scope (defined after the anon namespace); forward-declared here.
extern bool g_dragScrollActive;
extern bool g_dragMoved;
void dragScrollWndMove(int gameX, int gameY);
void cancelDragScroll();

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
const char* discipleIni(); // defined with the native game settings helpers below

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
// factor: 10 = x1.0 (identity) .. 150 = x15.0. Re-anchors on factor change so virtual time stays continuous.
using TimeGetTimeFn = DWORD(WINAPI*)(void);
TimeGetTimeFn g_realTimeGetTime = nullptr;
volatile LONG g_timeScaleHookInstalled = 0;

// Two live multipliers (x10 fixed-point); hook picks battle vs map by g_inBattle. 10 = identity (off).
volatile LONG g_battleFactor = 10;
volatile LONG g_mapFactor = 10;
int g_battleBaseFactor = 20; // exact tenths selected by menu or Ctrl+/Ctrl- (10 = vanilla)
int g_mapBaseFactor = 10;
volatile LONG g_inBattle = 0;
// Latest visual event since the last 32ms pump: 1=start, 2=end. A single last-writer-wins value
// preserves ordering when an instant/x15 effect starts and ends before one timer tick.
volatile LONG g_attackVisualEvent = 0;
volatile LONG g_attackVisualActive = 0; // exact start/end state when the completion hook is available
// Timer-only native playback state. Keep it separate from g_attackVisualActive: disabling the
// optional attack-speed burst clears that flag, but must not disable Animation Pause.
volatile LONG g_attackPlaybackActive = 0;
volatile LONG g_attackEndHookInstalled = 0;
volatile LONG g_batViewer = 0;   // current IBatViewer, captured by choose-action/effect; cleared on end
DWORD g_attackExpiryTick = 0;    // start of decay, or fallback hold boundary
DWORD g_attackWatchdogTick = 0;  // exact-end safety net; never the normal end condition

DWORD g_vcLastFactor = 10;
DWORD g_vcRealAnchor = 0, g_vcVirtAnchor = 0;
DWORD g_vcRealNow = 0, g_vcVirtNow = 0;
SRWLOCK g_vcLock = SRWLOCK_INIT;
bool g_vcInitialized = false;

DWORD WINAPI timeGetTimeHook(void)
{
    if (!g_realTimeGetTime)
        return 0;
    AcquireSRWLockExclusive(&g_vcLock);
    DWORD factor = static_cast<DWORD>(g_inBattle ? g_battleFactor : g_mapFactor);
    if (factor < 1)
        factor = 1;
    // Sample the real clock only after serializing callers. Reading first allowed two threads to
    // acquire the lock in the opposite order and briefly publish a backwards virtual timestamp.
    const DWORD realNow = g_realTimeGetTime();
    if (!g_vcInitialized) {
        // Identity on the first call regardless of Windows uptime. The old zero anchor multiplied
        // uptime in 32 bits and could jump backwards after a few hours/days at large factors.
        g_vcInitialized = true;
        g_vcLastFactor = factor;
        g_vcRealAnchor = g_vcRealNow = realNow;
        g_vcVirtAnchor = g_vcVirtNow = realNow;
        ReleaseSRWLockExclusive(&g_vcLock);
        eventtrace_clock(realNow, realNow, factor, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return realNow;
    }
    if (g_vcLastFactor != factor) { // factor changed (incl. battle<->map switch) -> re-anchor
        g_vcLastFactor = factor;
        g_vcRealAnchor = g_vcRealNow;
        g_vcVirtAnchor = g_vcVirtNow;
    }
    g_vcRealNow = realNow;
    const DWORD realDelta = g_vcRealNow - g_vcRealAnchor; // wrap-safe timeGetTime delta
    const DWORD result = static_cast<DWORD>(
        static_cast<unsigned long long>(realDelta) * factor / 10u + g_vcVirtAnchor);
    g_vcVirtNow = result;
    ReleaseSRWLockExclusive(&g_vcLock);
    eventtrace_clock(realNow, result, factor, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return result;
}

// D2 recognizes a double-click with its own 250ms comparison at 0x53F237, using the same
// CMqUIKernel clock that ultimately comes from the timeGetTime IAT slot above. Multiplying only the
// clock would therefore shrink the real input window to 250/factor (about 17ms at x15). Scale the
// comparison threshold by the identical factor so mouse input remains on a real 250ms window while
// every animation consumer continues to see virtual time.
DWORD __stdcall doubleClickThreshold(void)
{
    if (InterlockedExchangeAdd(&g_timeScaleHookInstalled, 0) != 1)
        return 250;

    const LONG inBattle = InterlockedExchangeAdd(&g_inBattle, 0);
    LONG factor = InterlockedExchangeAdd(inBattle ? &g_battleFactor : &g_mapFactor, 0);
    if (factor < 1)
        factor = 1;
    return static_cast<DWORD>(250ull * static_cast<unsigned long long>(factor) / 10ull);
}

// Replaces only `mov ecx,250` in the native double-click predicate. EAX contains the new click
// timestamp and must survive until the following `sub eax,edi`; preserve EDX as well so the thunk is
// transparent to the surrounding function.
__declspec(naked) void doubleClickThresholdThunk()
{
    __asm {
        push eax
        push edx
        call doubleClickThreshold
        mov ecx, eax
        pop edx
        pop eax
        ret
    }
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
    if (writeBytes(va, reinterpret_cast<const std::uint8_t*>(&hook), sizeof(hook))) {
        InterlockedExchange(&g_timeScaleHookInstalled, 1);
        mlog("[menu] anim time-scale hook installed (IAT %#x, real=%p)", (unsigned)va,
             reinterpret_cast<void*>(g_realTimeGetTime));
    } else {
        // The original pointer was sampled before VirtualProtect/write. Do not leave a failed
        // installation looking active or prevent a later retry.
        g_realTimeGetTime = nullptr;
        InterlockedExchange(&g_timeScaleHookInstalled, 0);
        mlog("[menu] anim time-scale hook FAILED at IAT %#x", (unsigned)va);
    }
}

void installDoubleClickTimeFix()
{
    if (g_ver != VerRussobit ||
        InterlockedExchangeAdd(&g_timeScaleHookInstalled, 0) != 1)
        return;

    constexpr uintptr_t siteVA = 0x53F237;
    static const std::uint8_t expected[10] = {
        0xB9, 0xFA, 0x00, 0x00, 0x00, // mov ecx,250
        0x2B, 0xC7,                   // sub eax,edi
        0x5E,                         // pop esi
        0x3B, 0xC8                    // cmp ecx,eax
    };
    if (memcmp(reinterpret_cast<const void*>(siteVA), expected, sizeof(expected)) != 0) {
        mlog("[menu] double-click timing signature mismatch at %#x; fix not installed",
             static_cast<unsigned>(siteVA));
        return;
    }

    std::uint8_t call[5] = {0xE8, 0, 0, 0, 0};
    const std::int32_t rel = static_cast<std::int32_t>(
        reinterpret_cast<uintptr_t>(&doubleClickThresholdThunk) - (siteVA + sizeof(call)));
    memcpy(call + 1, &rel, sizeof(rel));
    if (writeBytes(siteVA, call, sizeof(call)))
        mlog("[menu] real-time double-click window installed at %#x",
             static_cast<unsigned>(siteVA));
    else
        mlog("[menu] double-click timing hook installation FAILED at %#x",
             static_cast<unsigned>(siteVA));
}

// --- battle vs map discriminator: g_inBattle ---
// IBatViewer vftable @0x6F4294 (Russobit); slots [0] dtor 0x645900, [1] update 0x630DE3,
// [2] showAttackEffect 0x63203B, [3] battleEnd 0x631FFC. Slots 0/2/3 are safe lifetime/
// animation discriminators. Local decision grants come from CTaskBattle's directed ChooseAction
// handler; playback attribution comes from the broadcast Result lifecycle. Both controller
// boundaries are upstream of the native viewer and the bundled toolset's update replacement.
void* g_batDtorOrig = nullptr;
void* g_battleChooseOrig = nullptr;
void* g_battleResultOrig = nullptr;
void* g_battleResultDoneOrig = nullptr;
void* g_batShowOrig = nullptr;
void* g_batEndOrig = nullptr;
void* g_batUiStateOrig = reinterpret_cast<void*>(0x639743);
volatile LONG g_battleChooseInstallState = 0; // 0 pending, 1 installing, 2 installed, -1 unavailable
volatile LONG g_battleDiscriminatorInstallState = 0;
bool g_battleHookLayoutVerified = false;

bool executableAddress(const void* address);

extern "C" void timerhost_on_battle_update(void* batViewer, const void* battleMsgData,
                                             const void* unitId, const void* actions);
extern "C" void timerhost_after_battle_update(void* batViewer);
extern "C" void timerhost_on_battle_result(void* batViewer, const void* battleMsgData);
extern "C" void timerhost_on_battle_result_end(void);
extern "C" void timerhost_on_battle_end(void);

// Exact Russobit/MNS CTaskBattle::onChooseActionMsg @0x4D8B3D. IDA verifies that this handler is
// reached from the CommandMsgId::BattleChooseAction receive dispatcher and then calls
// IBatViewer::update at 0x4D8CB6. CTaskBattle[+0x14]->[+0x0C] is the current IBatViewer pointer;
// CCmdBattleChooseActionMsg contains BattleMsgData/+0x10, active unit/+0xF60 and actions/+0xF64.
int __fastcall battleChooseThunk(void* self, void* /*edx*/, const void* chooseMsg)
{
    void* batViewer = nullptr;
    __try {
        char* taskData = *reinterpret_cast<char**>(reinterpret_cast<char*>(self) + 0x14);
        batViewer = *reinterpret_cast<void**>(taskData + 0x0C);
        const char* message = reinterpret_cast<const char*>(chooseMsg);
        InterlockedExchange(&g_inBattle, 1);
        InterlockedExchange(&g_batViewer, reinterpret_cast<LONG>(batViewer));
        timerhost_on_battle_update(batViewer, message + 0x10,
                                   message + 0xF60, message + 0xF64);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        batViewer = nullptr;
    }

    const int result = reinterpret_cast<int(__thiscall*)(void*, const void*)>(
        g_battleChooseOrig)(self, chooseMsg);
    if (batViewer)
        timerhost_after_battle_update(batViewer);
    return result;
}

// Unlike directed ChooseAction, BattleResult is delivered to both clients. The server snapshots
// BattleMsgData before removing turnsOrder[0], so timerhost can classify the exact side whose
// aggregate playback is about to begin without treating an effect/target id as the actor.
void __fastcall battleResultThunk(void* self, void* /*edx*/, const void* resultMsg)
{
    __try {
        char* taskData = *reinterpret_cast<char**>(reinterpret_cast<char*>(self) + 0x14);
        void* batViewer = *reinterpret_cast<void**>(taskData + 0x0C);
        const char* message = reinterpret_cast<const char*>(resultMsg);
        InterlockedExchange(&g_inBattle, 1);
        InterlockedExchange(&g_batViewer, reinterpret_cast<LONG>(batViewer));
        timerhost_on_battle_result(batViewer, message + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    reinterpret_cast<void(__thiscall*)(void*, const void*)>(
        g_battleResultOrig)(self, resultMsg);
}

// IBatNotify::battleResult @0x4D8EFC is called only after the saved Result has been fully shown;
// it releases that exact message and clears CTaskBattle data+4. This closes playback more exactly
// than a timer or a generic animation heuristic.
void __fastcall battleResultDoneThunk(void* self, void* /*edx*/)
{
    timerhost_on_battle_result_end();
    reinterpret_cast<void(__thiscall*)(void*)>(g_battleResultDoneOrig)(self);
}
__declspec(naked) void batShowThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 1
        mov dword ptr [g_attackVisualEvent], 1
        mov dword ptr [g_attackPlaybackActive], 1
        mov dword ptr [g_batViewer], ecx   // same IBatViewer instance as the choose-action receiver
        jmp dword ptr [g_batShowOrig]
    }
}
void clearBattleHookState()
{
    InterlockedExchange(&g_inBattle, 0);
    InterlockedExchange(&g_batViewer, 0);
    InterlockedExchange(&g_attackVisualEvent, 0);
    InterlockedExchange(&g_attackVisualActive, 0);
    InterlockedExchange(&g_attackPlaybackActive, 0);
    g_attackExpiryTick = 0;
    g_attackWatchdogTick = 0;
    timerhost_on_battle_end();
}

__declspec(naked) void batEndThunk()
{
    __asm {
        pushfd
        pushad
        call clearBattleHookState
        popad
        popfd
        jmp dword ptr [g_batEndOrig]
    }
}

__declspec(naked) void batDtorThunk()
{
    __asm {
        pushfd
        pushad
        call clearBattleHookState
        popad
        popfd
        jmp dword ptr [g_batDtorOrig]
    }
}

// Called only by the final-zero branch of CBatViewerUtils::CAnimCounter. The original target owns
// thiscall's stack cleanup (ret 4), so a store + tail jump preserves every register/flag/argument.
__declspec(naked) void batAttackEndThunk()
{
    __asm {
        mov dword ptr [g_attackVisualEvent], 2
        mov dword ptr [g_attackPlaybackActive], 0
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

void ensureBattleDiscriminator()
{
    if (g_ver != VerRussobit ||
        InterlockedExchangeAdd(&g_battleDiscriminatorInstallState, 0) == 2)
        return;
    if (InterlockedCompareExchange(&g_battleDiscriminatorInstallState, 1, 0) != 0)
        return;
    auto vt = reinterpret_cast<void**>(0x6F4294);
    void* current[4] = {};
    MEMORY_BASIC_INFORMATION mbi = {};
    __try {
        memcpy(current, vt, sizeof(current));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_battleDiscriminatorInstallState, -1);
        return;
    }
    const HMODULE exe = GetModuleHandleA(nullptr);
    if (exe != reinterpret_cast<HMODULE>(0x400000) ||
        !VirtualQuery(vt, &mbi, sizeof(mbi)) || mbi.AllocationBase != exe ||
        !executableAddress(current[0]) || !executableAddress(current[1]) ||
        !executableAddress(current[2]) || !executableAddress(current[3])) {
        mlog("[menu] battle hook layout rejected (vtable=%p targets=%p/%p/%p/%p)",
             vt, current[0], current[1], current[2], current[3]);
        InterlockedExchange(&g_battleDiscriminatorInstallState, -1);
        return;
    }
    // Capture and tail-chain whatever currently owns each EXE vtable slot. This deliberately avoids
    // inspecting or naming another module: whether a compatible third-party hook installed before
    // or after C4dll, the last writer chains the previous target instead of requiring pristine bytes.
    g_batDtorOrig = current[0];
    g_batShowOrig = current[2];
    g_batEndOrig = current[3];
    void* thunks[4] = {reinterpret_cast<void*>(&batDtorThunk), current[1],
                       reinterpret_cast<void*>(&batShowThunk), reinterpret_cast<void*>(&batEndThunk)};
    if (writeBytes(0x6F4294, reinterpret_cast<const std::uint8_t*>(thunks), sizeof(thunks))) {
        g_battleHookLayoutVerified = true;
        InterlockedExchange(&g_battleDiscriminatorInstallState, 2);
        mlog("[menu] battle anim discriminator chained; controller lifecycle hook deferred");
    } else {
        InterlockedExchange(&g_battleDiscriminatorInstallState, -1);
        InterlockedExchange(&g_battleChooseInstallState, -1);
        mlog("[menu] battle anim discriminator install FAILED");
        return;
    }
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

bool executableAddress(const void* address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    const DWORD protect = mbi.Protect & 0xFF;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

// --- native in-game status text (the same CStratInterf route used by DisciplesGL) ---
// The lifecycle sites and printer belong to the validated Discipl2.exe image, never to mss32.
// Capture the current CStratInterf at its unique construction call and clear it at the matching
// scalar destructor; tail-jumping the current rel32 targets keeps other call-site hooks chainable.
volatile LONG g_statusStratInterf = 0;
void* g_statusCtorChain = nullptr;
void* g_statusDtorChain = nullptr;
volatile LONG g_statusTextInstallState = 0; // 0 absent, 2 installed, -1 rejected

__declspec(naked) void statusStratCtorThunk()
{
    __asm {
        mov dword ptr [g_statusStratInterf], ecx
        jmp dword ptr [g_statusCtorChain]
    }
}

__declspec(naked) void statusStratDtorThunk()
{
    __asm {
        mov eax, dword ptr [g_statusStratInterf]
        cmp ecx, eax
        jne status_dtor_chain
        xor eax, eax
        mov dword ptr [g_statusStratInterf], eax
    status_dtor_chain:
        jmp dword ptr [g_statusDtorChain]
    }
}

void* rel32CallTarget(uintptr_t site)
{
    std::int32_t displacement = 0;
    memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
    return reinterpret_cast<void*>(site + 5 + displacement);
}

void makeRel32Call(uintptr_t site, const void* target, std::uint8_t (&call)[5])
{
    call[0] = 0xE8;
    const std::int32_t displacement = static_cast<std::int32_t>(
        reinterpret_cast<uintptr_t>(target) - (site + sizeof(call)));
    memcpy(call + 1, &displacement, sizeof(displacement));
}

void installStatusTextHooks()
{
    if (g_ver != VerRussobit ||
        InterlockedCompareExchange(&g_statusTextInstallState, 1, 0) != 0)
        return;

    constexpr uintptr_t kPrint = 0x4900C8;
    constexpr uintptr_t kCtorContext = 0x405C94;
    constexpr uintptr_t kCtorCall = 0x405C95;
    constexpr uintptr_t kDtorContext = 0x48D7A5;
    constexpr uintptr_t kDtorCall = 0x48D7A8;
    const std::uint8_t printExpected[27] = {
        0x6A, 0x01, 0x83, 0xC1, 0x04, 0xFF, 0x74, 0x24, 0x08,
        0xE8, 0x59, 0x84, 0x10, 0x00, 0x8B, 0xC8, 0x83, 0xC1,
        0x08, 0xE8, 0x62, 0x4E, 0xF7, 0xFF, 0xC2, 0x04, 0x00};
    const std::uint8_t ctorSuffix[13] = {
        0x8B, 0x4E, 0x10, 0x83, 0x4D, 0xFC, 0xFF,
        0x83, 0xC1, 0x0C, 0x51, 0x8B, 0xC8};
    const std::uint8_t dtorPrefix[3] = {0x56, 0x8B, 0xF1};
    const std::uint8_t dtorSuffix[8] = {0xF6, 0x44, 0x24, 0x08, 0x01, 0x74, 0x07, 0x56};

    if (!executableAddress(reinterpret_cast<const void*>(kPrint)) ||
        memcmp(reinterpret_cast<const void*>(kPrint), printExpected,
               sizeof(printExpected)) != 0 ||
        *reinterpret_cast<const std::uint8_t*>(kCtorContext) != 0x56 ||
        *reinterpret_cast<const std::uint8_t*>(kCtorCall) != 0xE8 ||
        memcmp(reinterpret_cast<const void*>(kCtorCall + 5), ctorSuffix,
               sizeof(ctorSuffix)) != 0 ||
        memcmp(reinterpret_cast<const void*>(kDtorContext), dtorPrefix,
               sizeof(dtorPrefix)) != 0 ||
        *reinterpret_cast<const std::uint8_t*>(kDtorCall) != 0xE8 ||
        memcmp(reinterpret_cast<const void*>(kDtorCall + 5), dtorSuffix,
               sizeof(dtorSuffix)) != 0) {
        InterlockedExchange(&g_statusTextInstallState, -1);
        mlog("[menu] native status-text signatures rejected; on-screen speed notice disabled");
        return;
    }

    void* const ctorTarget = rel32CallTarget(kCtorCall);
    void* const dtorTarget = rel32CallTarget(kDtorCall);
    if (!executableAddress(ctorTarget) || !executableAddress(dtorTarget) ||
        ctorTarget == reinterpret_cast<void*>(&statusStratCtorThunk) ||
        dtorTarget == reinterpret_cast<void*>(&statusStratDtorThunk)) {
        InterlockedExchange(&g_statusTextInstallState, -1);
        mlog("[menu] native status-text call chain rejected (ctor=%p dtor=%p)",
             ctorTarget, dtorTarget);
        return;
    }

    std::uint8_t originalCtor[5] = {};
    std::uint8_t originalDtor[5] = {};
    std::uint8_t ctorCall[5] = {};
    std::uint8_t dtorCall[5] = {};
    memcpy(originalCtor, reinterpret_cast<const void*>(kCtorCall), sizeof(originalCtor));
    memcpy(originalDtor, reinterpret_cast<const void*>(kDtorCall), sizeof(originalDtor));
    makeRel32Call(kCtorCall, reinterpret_cast<const void*>(&statusStratCtorThunk), ctorCall);
    makeRel32Call(kDtorCall, reinterpret_cast<const void*>(&statusStratDtorThunk), dtorCall);
    g_statusCtorChain = ctorTarget;
    g_statusDtorChain = dtorTarget;

    if (!writeBytes(kCtorCall, ctorCall, sizeof(ctorCall)) ||
        !writeBytes(kDtorCall, dtorCall, sizeof(dtorCall))) {
        // Roll back both sites even if only the second write failed. This runs before EXE entry,
        // so neither site can execute during the small installation transaction.
        writeBytes(kCtorCall, originalCtor, sizeof(originalCtor));
        writeBytes(kDtorCall, originalDtor, sizeof(originalDtor));
        g_statusCtorChain = nullptr;
        g_statusDtorChain = nullptr;
        InterlockedExchange(&g_statusTextInstallState, -1);
        mlog("[menu] native status-text hook write failed; rolled back");
        return;
    }

    InterlockedExchange(&g_statusTextInstallState, 2);
    mlog("[menu] native status-text lifecycle installed (ctor=%p dtor=%p)",
         ctorTarget, dtorTarget);
}

static bool isUserPtr(const void* p);

// Install after loader initialization, on the game GUI thread. These controller handlers are not
// touched by the toolset's viewer hook table: directed ChooseAction owns local decision windows,
// while broadcast BattleResult owns playback attribution on both clients.
void ensureBattleChooseActionHook()
{
    if (g_ver != VerRussobit || !g_battleHookLayoutVerified ||
        InterlockedExchangeAdd(&g_battleChooseInstallState, 0) == 2)
        return;
    if (InterlockedCompareExchange(&g_battleChooseInstallState, 1, 0) != 0)
        return;

    constexpr uintptr_t kChooseEntry = 0x4D8B3D;
    const std::uint8_t expected[24] = {
        0xB8, 0xE3, 0x01, 0x6A, 0x00, 0xE8, 0x89, 0x48,
        0x19, 0x00, 0xB8, 0x20, 0x10, 0x00, 0x00, 0xE8,
        0x3F, 0x49, 0x19, 0x00, 0x53, 0x56, 0x8B, 0xF1};
    constexpr uintptr_t kResultEntry = 0x4D8DC1;
    const std::uint8_t resultExpected[24] = {
        0xB8, 0x83, 0x02, 0x6A, 0x00, 0xE8, 0x05, 0x46,
        0x19, 0x00, 0x81, 0xEC, 0x54, 0x0F, 0x00, 0x00,
        0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7D, 0x08};
    constexpr uintptr_t kResultDoneEntry = 0x4D8EFC;
    const std::uint8_t resultDoneExpected[24] = {
        0x56, 0x8B, 0xF1, 0x6A, 0x00, 0x68, 0xE0, 0x27,
        0x79, 0x00, 0x8B, 0x46, 0x04, 0x68, 0xE0, 0xEE,
        0x78, 0x00, 0x6A, 0x00, 0xFF, 0x70, 0x04, 0xE8};
    void* const entry = reinterpret_cast<void*>(kChooseEntry);
    void* const resultEntry = reinterpret_cast<void*>(kResultEntry);
    void* const resultDoneEntry = reinterpret_cast<void*>(kResultDoneEntry);
    if (!executableAddress(entry) || !executableAddress(resultEntry) ||
        !executableAddress(resultDoneEntry)) {
        mlog("[timer] battle controller hooks rejected: choose=%p result=%p done=%p "
             "executable=%d/%d/%d", entry, resultEntry, resultDoneEntry,
             executableAddress(entry) ? 1 : 0, executableAddress(resultEntry) ? 1 : 0,
             executableAddress(resultDoneEntry) ? 1 : 0);
        InterlockedExchange(&g_battleChooseInstallState, -1);
        return;
    }
    if (memcmp(entry, expected, sizeof(expected)) != 0 ||
        memcmp(resultEntry, resultExpected, sizeof(resultExpected)) != 0 ||
        memcmp(resultDoneEntry, resultDoneExpected, sizeof(resultDoneExpected)) != 0) {
        mlog("[timer] CTaskBattle choose/result lifecycle signature mismatch; "
             "timer stays fail-closed");
        InterlockedExchange(&g_battleChooseInstallState, -1);
        return;
    }

    g_battleChooseOrig = entry;
    g_battleResultOrig = resultEntry;
    g_battleResultDoneOrig = resultDoneEntry;
    LONG error = DetourTransactionBegin();
    const bool transactionStarted = error == NO_ERROR;
    if (error == NO_ERROR)
        error = DetourUpdateThread(GetCurrentThread());
    if (error == NO_ERROR)
        error = DetourAttach(&g_battleChooseOrig, reinterpret_cast<void*>(&battleChooseThunk));
    if (error == NO_ERROR)
        error = DetourAttach(&g_battleResultOrig, reinterpret_cast<void*>(&battleResultThunk));
    if (error == NO_ERROR)
        error = DetourAttach(&g_battleResultDoneOrig,
                             reinterpret_cast<void*>(&battleResultDoneThunk));
    if (error == NO_ERROR)
        error = DetourTransactionCommit();
    else if (transactionStarted)
        DetourTransactionAbort();

    if (error != NO_ERROR) {
        g_battleChooseOrig = nullptr;
        g_battleResultOrig = nullptr;
        g_battleResultDoneOrig = nullptr;
        InterlockedExchange(&g_battleChooseInstallState, -1);
        mlog("[timer] CTaskBattle choose/result detour FAILED (error=%ld)", error);
        return;
    }

    InterlockedExchange(&g_battleChooseInstallState, 2);
    mlog("[timer] CTaskBattle decision/result lifecycle installed "
         "(choose=%p/%p result=%p/%p done=%p/%p)", entry, g_battleChooseOrig,
         resultEntry, g_battleResultOrig, resultDoneEntry, g_battleResultDoneOrig);
}

extern "C" int featuremenu_battle_animation_active(void)
{
    // On the verified layout the game model owns the animation lifetime. showAttackEffect clears
    // this UI-ready byte and CAnimCounter sets it only after aggregate attack playback reaches zero.
    // A wall-clock watchdog is wrong here: an inactive window can legitimately hold the animation.
    if (g_battleHookLayoutVerified && InterlockedExchangeAdd(&g_inBattle, 0)) {
        __try {
            char* viewer = reinterpret_cast<char*>(
                InterlockedExchangeAdd(&g_batViewer, 0));
            char* data = isUserPtr(viewer)
                ? *reinterpret_cast<char**>(viewer + 4)
                : nullptr;
            if (isUserPtr(data))
                return data[0x14E0] ? 0 : 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // Conservative fallback for an unsupported/missed native state.
    return InterlockedExchangeAdd(&g_attackPlaybackActive, 0) ? 1 : 0;
}

extern "C" int featuremenu_battle_timer_lifecycle_available(void)
{
    return InterlockedExchangeAdd(&g_battleChooseInstallState, 0) == 2 ? 1 : 0;
}

// Speed 1..6 -> virtual-clock factor (x1.5/x2/x3/x4/x5/x15, fixed-point /10); 10 = identity (off).
const int kAnimFactor[6] = {15, 20, 30, 40, 50, 150};

int animPresetForFactor(int factor)
{
    for (int i = 0; i < 6; ++i)
        if (kAnimFactor[i] == factor)
            return i + 1;
    return 0;
}

void applyAnimFactor(int which, bool enabled, int factor)
{
    if (factor < 10)
        factor = 10;
    if (factor > 150)
        factor = 150;
    if (which == 0) {
        g_battleBaseFactor = factor;
        g_battleFactor = enabled ? factor : 10;
    } else {
        g_mapBaseFactor = factor;
        g_mapFactor = enabled ? factor : 10;
    }
}

// Map a coarse menu preset to the exact-tenths base used by the live hotkeys. Off = x1.0.
// Runs on the game UI thread (menu WM_COMMAND).
void applyAnimSpeed(int which, bool enabled, int speed)
{
    if (speed < 1)
        speed = 1;
    if (speed > 6)
        speed = 6;
    applyAnimFactor(which, enabled, kAnimFactor[speed - 1]);
}

void showAnimationSpeedStatus(bool battleVisible, int factor);

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
    // DisciplesGL 1.90 Stretch windows: Off + 10..100%, applied live and centred.
    kIdWindowStretchBase = 0xA122, // + 0..10 (percentage = index * 10)
    kIdWindowStretchInfo = 0xA12D,
    kIdWindowStretchHelp = 0xA12E,
    // cnc-ddraw (ddraw.ini) settings
    kIdRendOpenGL = 0xA130,
    kIdRendGdi = 0xA131,
    kIdRendAuto = 0xA132,
    kIdRendererActive = 0xA133, // disabled diagnostic: actual backend, not persisted request
    kIdShaderBase = 0xA140, // + index into kShaders[]
    kIdShaderStatus = 0xA148, // disabled diagnostic: packaged OpenGL shader assets
    kIdMaintas = 0xA150,
    kIdVsync = 0xA151,
    kIdBoxing = 0xA152,
    kIdScaleCustom = 0xA154, // disabled radio: non-empty ddraw.ini aspect_ratio
    kIdScaleGeometry = 0xA155, // disabled live-metrics line
    kIdScaleResult = 0xA156, // disabled live-metrics line
    kIdScaleFilterInfo = 0xA157, // disabled pixel/filter explanation
    kIdTicks0 = 0xA160,
    kIdTicks30 = 0xA161,
    kIdTicks60 = 0xA162,
    kIdTicks100 = 0xA163,
    kIdSingleCpu = 0xA164, // ddraw.ini singlecpu toggle (full restart)
    // New isolated id: preserve old commands and avoid the derived kIdResBase[] range.
    kIdTicks180 = 0xA1F6,
    kIdResetWrapper = 0xA1F7, // all wrapper preferences, never timer/plugin settings
    kIdFastAi = 0xA17E, // bounded DisciplesGL-compatible host AI pump (live, experimental)
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
    kIdOutputSizeCustom = 0xA1F5, // persisted live ddraw.ini window/output-size dialog
    kIdClouds = 0xA1F8, // restart-latched optional IsoClouds.ff pipeline
    kIdCloudsInfo = 0xA1F9, // disabled active -> requested / asset-status line
    kIdNetTrace = 0xA1FA, // optional diagnostic recorder, restart by closing client
    kIdNetTraceInfo = 0xA1FB,
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
struct ShaderOption
{
    const wchar_t* en;
    const wchar_t* ru;
    const char* value;
    const char* requiredPass1;
};
const NameVal kRenderers[] = {
    {L"OpenGL - shaders + best upscaling (recommended)",
     L"OpenGL - шейдеры и лучший апскейл (рекомендуется)", "opengl"},
    {L"GDI - software, max compatibility (slower)",
     L"GDI - программный, максимальная совместимость (медленнее)", "gdi"},
    {L"Auto - picks D3D9 first (basic filters)",
     L"Auto - сам выберет D3D9 (основные фильтры)", "auto"}};
const int kRendererCount = 3;
// Image filters, ranked best->basic for D2's hand-painted art.
const ShaderOption kShaders[] = {
    {L"Lanczos - sharp, detailed (best for D2 art)",
     L"Lanczos - чёткий, детальный (лучший для графики D2)",
     "Shaders\\interpolation\\lanczos2-sharp.glsl", nullptr},
    {L"xBRZ - pixel-art scaler, clean sprite edges",
     L"xBRZ - пиксель-арт скейлер, чистые края спрайтов",
     "Shaders\\xbrz\\xbrz-freescale-multipass.glsl",
     "Shaders\\xbrz\\xbrz-freescale-multipass.glsl.pass1"},
    {L"Bicubic - smooth, balanced (cnc default)",
     L"Bicubic - мягкий, сбалансированный (дефолт cnc)",
     "Shaders\\interpolation\\catmull-rom-bilinear.glsl", nullptr},
    {L"AMD FSR - modern edge sharpening, crisp",
     L"AMD FSR - современная резкость краёв",
     "Shaders\\interpolation\\fsr.glsl",
     "Shaders\\interpolation\\fsr.glsl.pass1"},
    {L"xBR lv2 - pixel-art, lighter than xBRZ",
     L"xBR lv2 - пиксель-арт, легче xBRZ",
     "Shaders\\xbr\\xbr-lv2-noblend.glsl", nullptr},
    {L"Bilinear - simple smoothing, a bit soft",
     L"Bilinear - простое сглаживание, слегка мыльно",
     "Shaders\\interpolation\\bilinear.glsl", nullptr},
    {L"None - sharpest pixels, blocky on zoom",
     L"Без фильтра - самые чёткие пиксели, кубики при увеличении",
     "Shaders\\nearest-neighbor.glsl", nullptr},
    {L"CRT - retro scanlines (style, not sharper)",
     L"CRT - ретро-развёртка (стиль, не чёткость)",
     "Shaders\\crt\\crt-lottes-fast-no-warp-bilinear.glsl", nullptr}};
const int kShaderCount = 8;
static_assert(sizeof(kShaders) / sizeof(kShaders[0]) == kShaderCount,
              "the Filter menu must expose the eight packaged shaders");

// cnc-ddraw's Direct3D 9 filter values. GDI maps every non-nearest value to HALFTONE; OpenGL uses
// the full shader path above. A negative result means that filter genuinely requires OpenGL.
int portableFilterForShader(int shaderIndex)
{
    switch (shaderIndex) {
    case 0: return 3; // Lanczos
    case 2: return 2; // Catmull-Rom cubic
    case 5: return 1; // bilinear
    case 6: return 0; // nearest
    default: return -1;
    }
}

int shaderForPortableFilter(int filter)
{
    switch (filter) {
    case 3: return 0;
    case 2: return 2;
    case 1: return 5;
    case 0: return 6;
    default: return 2;
    }
}
// -1 = limiter fully off (cnc-ddraw treats 0 as "emulate 60hz flip", not off)
const int kTicksValues[] = {-1, 30, 60, 100, 180};
const UINT kTicksCommandIds[] = {
    kIdTicks0, kIdTicks30, kIdTicks60, kIdTicks100, kIdTicks180};
const int kTicksCount = static_cast<int>(sizeof(kTicksValues) / sizeof(kTicksValues[0]));
static_assert(sizeof(kTicksValues) / sizeof(kTicksValues[0]) ==
                  sizeof(kTicksCommandIds) / sizeof(kTicksCommandIds[0]),
              "game-speed values and command ids must stay aligned");
// Legacy/manual output size compatibility (ddraw.ini width/height). The game UI deliberately does
// not expose a second resolution concept: 0,0 follows the selected logical canvas, and every game
// resolution choice restores that link. The scenario editor keeps a direct window-size dialog.
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
int g_d3dFilter = 3;    // FILTER_LANCZOS; also selects GDI nearest vs HALFTONE fallback
bool g_maintas = false, g_vsync = false, g_boxing = false;
char g_aspectRatio[32] = {}; // non-empty overrides native aspect and forces maintas in cnc-ddraw
bool g_singlecpu = true; // ddraw.ini singlecpu stability mode (cnc-ddraw default true)
bool g_dragScroll = true; // grab+drag map panning (ini [menu] dragScroll, default on)
bool g_dialogVoSkip = false; // auto-close voiced event popups after VO + log their text (default off)
bool g_autoConfirmUnitHire = false; // skip X005TA0285 through BTN_YES (default on for validated MNS)
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
int g_windowStretchPercent = 100; // legacy centred fixed-window crop, 0=off, default=100
HMENU g_bar = nullptr;
UINT g_relayoutMsg = 0; // registered msg: marshal menu/fullscreen chrome sync onto the GUI thread
HMENU g_gameMenu = nullptr, g_videoMenu = nullptr, g_perfMenu = nullptr; // top-level bar menus
HMENU g_technicalMenu = nullptr; // technical diagnostics live one level below ordinary settings
HMENU g_battleAnimMenu = nullptr, g_mapAnimMenu = nullptr, g_battleAtkMenu = nullptr;
HMENU g_rendMenu = nullptr, g_shaderMenu = nullptr;
HMENU g_ticksMenu = nullptr, g_resMenu = nullptr, g_fpsMenu = nullptr, g_scaleMenu = nullptr;
HMENU g_windowStretchMenu = nullptr;
HMENU g_displaySizeMenu = nullptr;
HMENU g_battleMenu = nullptr, g_mapMenu = nullptr, g_modeMenu = nullptr;
HMENU g_menuLanguageMenu = nullptr, g_localeMenu = nullptr;
HMENU g_editorModeMenu = nullptr;
int g_resolutionMenuPosition = -1; // position of the one Video -> Resolution popup
int g_windowStretchMenuPosition = -1; // position of the live legacy Stretch windows popup
volatile LONG g_ncCursorMode = 2; // 0=the native D2 software cursor, 2=Windows menu/non-client arrow
volatile LONG g_cursorCaptureInstalled = 0;
bool g_menuLoopActive = false; // native popup menus need the OS cursor across the whole window
DWORD g_pendingRendererVerifyTick = 0; // OpenGL finishes its real self-test on the render thread
bool g_shaderFolderReady = false;
bool g_shaderPrimaryReady[kShaderCount] = {};
bool g_shaderPass1Ready[kShaderCount] = {};
bool g_shaderAvailable[kShaderCount] = {};
int g_missingShaderFileCount = 0;
volatile LONG g_shaderAssetsWarningShown = 0;

using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProcFn g_origWndProc = nullptr;
HWND g_gameHwnd = nullptr; // game window (drag-scroll SetCapture target); set in wndProcHook
const UINT_PTR kPressTimerId = 0xC4D7; // our WM_TIMER source: on-elapse END_TURN press fires ONLY on
                                       // WM_TIMER (idle-gated), like legacy SetTimer(hWnd,0,0x20,0)
HWND g_pressTimerHwnd = nullptr; // tracked independently: renderer bridge may set g_gameHwnd first
volatile LONG g_pressTimerArmFailureLogged = 0;

bool dllRelativePath(const char* relative, char* path, size_t capacity)
{
    if (!relative || !relative[0] || !path || capacity < 2)
        return false;

    path[0] = 0;
    const DWORD length = GetModuleFileNameA(
        g_ddraw_module, path, static_cast<DWORD>(capacity));
    if (!length || length >= capacity)
        return false;

    char* slash = strrchr(path, '\\');
    const size_t prefix = slash ? static_cast<size_t>(slash + 1 - path) : 0;
    const size_t relativeLength = strlen(relative);
    if (!prefix || prefix + relativeLength >= capacity)
        return false;

    memcpy(path + prefix, relative, relativeLength + 1);
    return true;
}

bool dllRelativeDirectoryExists(const char* relative)
{
    char path[MAX_PATH] = {};
    if (!dllRelativePath(relative, path, sizeof(path)))
        return false;
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool fileIsNonempty(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!path || !path[0] ||
        !GetFileAttributesExA(path, GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    return data.nFileSizeHigh != 0 || data.nFileSizeLow != 0;
}

// Match render_ogl.c exactly: an existing relative path wins, even when it is an empty file or a
// directory which fopen will reject; only a genuinely missing relative path falls back beside DLL.
bool resolveShaderPath(const char* relative, char* resolved, size_t capacity)
{
    if (!relative || !relative[0] || !resolved || strlen(relative) >= capacity)
        return false;
    if (GetFileAttributesA(relative) != INVALID_FILE_ATTRIBUTES) {
        memcpy(resolved, relative, strlen(relative) + 1);
        return true;
    }

    return dllRelativePath(relative, resolved, capacity);
}

void scanShaderAssets()
{
    const DWORD workingAttributes = GetFileAttributesA("Shaders");
    g_shaderFolderReady =
        (workingAttributes != INVALID_FILE_ATTRIBUTES &&
         (workingAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ||
        dllRelativeDirectoryExists("Shaders");
    int missing = 0;
    for (int i = 0; i < kShaderCount; ++i) {
        char resolved[MAX_PATH] = {};
        const bool resolvedPrimary =
            resolveShaderPath(kShaders[i].value, resolved, sizeof(resolved));
        g_shaderPrimaryReady[i] = resolvedPrimary && fileIsNonempty(resolved);
        g_shaderPass1Ready[i] = true;
        if (kShaders[i].requiredPass1) {
            // cnc-ddraw does not resolve pass1 independently: it appends the suffix to the already
            // selected main path. Keep that same root so a stray pass in CWD/DLL cannot mask a miss.
            const size_t length = resolvedPrimary ? strlen(resolved) : 0;
            if (!length || length > sizeof(resolved) - 8) {
                g_shaderPass1Ready[i] = false;
            } else {
                strcat_s(resolved, ".pass1");
                g_shaderPass1Ready[i] = fileIsNonempty(resolved);
            }
        }
        if (!g_shaderPrimaryReady[i])
            ++missing;
        if (!g_shaderPass1Ready[i])
            ++missing;
        g_shaderAvailable[i] = g_shaderPrimaryReady[i] && g_shaderPass1Ready[i];
    }
    g_missingShaderFileCount = missing;
}

void appendMissingShaderAsset(wchar_t* list, size_t capacity,
                              const char* relative)
{
    if (!list || !capacity || !relative)
        return;
    wchar_t wide[MAX_PATH] = {};
    if (!MultiByteToWideChar(CP_ACP, 0, relative, -1, wide,
                             static_cast<int>(sizeof(wide) / sizeof(wide[0]))))
        lstrcpynW(wide, L"?", static_cast<int>(sizeof(wide) / sizeof(wide[0])));
    const size_t used = wcslen(list);
    const size_t needed = 4 + wcslen(wide);
    if (used + needed >= capacity)
        return;
    swprintf_s(list + used, capacity - used, L"\r\n  %s", wide);
}

// Called only from the GUI-thread chrome/menu paths. Claim the notification before opening the
// modal because MessageBox owns a nested message loop and the periodic chrome message can re-enter.
void showShaderAssetsWarningOnce(HWND owner)
{
    if (!g_missingShaderFileCount ||
        InterlockedCompareExchange(&g_shaderAssetsWarningShown, 1, 0) != 0)
        return;

    wchar_t missing[1536] = {};
    for (int i = 0; i < kShaderCount; ++i) {
        if (!g_shaderPrimaryReady[i]) {
            appendMissingShaderAsset(missing,
                                     sizeof(missing) / sizeof(missing[0]),
                                     kShaders[i].value);
            mlog("[menu] missing/empty shader asset: %s", kShaders[i].value);
        }
        if (!g_shaderPass1Ready[i]) {
            appendMissingShaderAsset(missing,
                                     sizeof(missing) / sizeof(missing[0]),
                                     kShaders[i].requiredPass1);
            mlog("[menu] missing/empty shader asset: %s",
                 kShaders[i].requiredPass1);
        }
    }

    wchar_t message[2048] = {};
    swprintf_s(
        message,
        L(L"The Shaders folder next to C4dll-R.dll is missing or incomplete. %d required OpenGL shader file(s) could not be found or are empty. Unavailable OpenGL filters are disabled in Video > Filter.\n\nCopy the complete Shaders folder next to Discipl2.exe. The built-in Direct3D 9/GDI filters remain usable.\n\nMissing:%s",
          L"Папка Shaders рядом с C4dll-R.dll отсутствует или неполна. Не найдено или пусто обязательных файлов OpenGL-шейдеров: %d. Недоступные OpenGL-фильтры отключены в меню «Видео > Фильтр».\n\nСкопируйте папку Shaders целиком рядом с Discipl2.exe. Встроенные фильтры Direct3D 9/GDI остаются доступны.\n\nОтсутствуют:%s"),
        g_missingShaderFileCount, missing);
    MessageBoxW(owner, message,
                L(L"Incomplete Shaders folder", L"Неполная папка Shaders"),
                MB_OK | MB_ICONWARNING);
}

bool ensurePressTimer(HWND hwnd)
{
    if (!hwnd)
        return false;
    if (g_pressTimerHwnd == hwnd)
        return true;
    if (g_pressTimerHwnd)
        KillTimer(g_pressTimerHwnd, kPressTimerId);
    g_pressTimerHwnd = nullptr;
    if (!SetTimer(hwnd, kPressTimerId, 32, nullptr)) {
        if (InterlockedCompareExchange(&g_pressTimerArmFailureLogged, 1, 0) == 0)
            mlog("[timer] SetTimer(%p, %#Ix) failed error=%lu", hwnd,
                 kPressTimerId, GetLastError());
        return false;
    }
    InterlockedExchange(&g_pressTimerArmFailureLogged, 0);
    g_pressTimerHwnd = hwnd;
    mlog("[timer] idle pump armed hwnd=%p id=%#Ix", hwnd, kPressTimerId);
    return true;
}

void releasePressTimer(HWND hwnd)
{
    if (g_pressTimerHwnd != hwnd)
        return;
    KillTimer(hwnd, kPressTimerId);
    g_pressTimerHwnd = nullptr;
}

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
    wsprintfA(buf, "%d", g_battleBaseFactor);
    WritePrivateProfileStringA("menu", "battleAnimFactor", buf, f);
    WritePrivateProfileStringA("menu", "mapAnimEnabled", g_mapAnimEnabled ? "1" : "0", f);
    wsprintfA(buf, "%d", g_mapAnimSpeed);
    WritePrivateProfileStringA("menu", "mapAnimSpeed", buf, f);
    wsprintfA(buf, "%d", g_mapBaseFactor);
    WritePrivateProfileStringA("menu", "mapAnimFactor", buf, f);
    WritePrivateProfileStringA("menu", "battleAttackEnabled", g_battleAttackEnabled ? "1" : "0", f);
    wsprintfA(buf, "%d", g_battleAttackSpeed);
    WritePrivateProfileStringA("menu", "battleAttackSpeed", buf, f);
    WritePrivateProfileStringA("menu", "perUnitBurst", g_perUnitBurst ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "dragScroll", g_dragScroll ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "wideBattle",
                               widebattle_get_enabled() ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "dialogVoSkip", g_dialogVoSkip ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "autoConfirmUnitHire",
                               g_autoConfirmUnitHire ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "fastAI", fastai_get_enabled() ? "1" : "0", f);
    wsprintfA(buf, "%d", g_windowStretchPercent);
    WritePrivateProfileStringA("menu", "stretchWindows", buf, f);
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
    const int autoConfirmHire = g_ver == VerRussobit ? 1 : 0;
    const int fastAi =
        GetPrivateProfileIntA("Wrapper", "FastAI", 0, discipleIni()) != 0 ? 1 : 0;

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
        "; Live animation-speed multiplier, separate for battle and the strategic map.\r\n"
        ";   *Enabled : 0 = vanilla, 1 = on.   *Speed keeps the coarse menu preset (1..6).\r\n"
        ";   *Factor is the exact speed in tenths (10=1.0x .. 150=15.0x) used by Ctrl +/-;\r\n"
        ";   when present it takes precedence over the coarse preset.\r\n"
        "battleAnimEnabled=%d\r\n"
        "battleAnimSpeed=%d\r\n"
        "battleAnimFactor=%d\r\n"
        "mapAnimEnabled=%d\r\n"
        "mapAnimSpeed=%d\r\n"
        "mapAnimFactor=%d\r\n"
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
        "; Auto-close event dialogs that have a voiceover, once the VO finishes (streamer aid).\r\n"
        "; Their text is appended to dialog-vo-log.txt in the game folder.  0 = off (default), 1 = on.\r\n"
        "dialogVoSkip=0\r\n"
        "\r\n"
        "; Auto-confirm \"Do you want to hire this unit?\" through the normal BTN_YES action.\r\n"
        "; Applies only during the local player's active turn. Default: 1 on validated MNS, else 0.\r\n"
        "autoConfirmUnitHire=%d\r\n"
        "\r\n"
        "; DisciplesGL 1.90 Stretch windows: centred live crop of fixed screens.\r\n"
        "; 0 = off; 10..100 = strength. 100 fills the vertical canvas and is the default.\r\n"
        "stretchWindows=100\r\n"
        "\r\n"
        "; Experimental Fast AI: accelerates only the local host's hidden AI message pump.\r\n"
        "; It does not change AI decisions, but can expose native game races/crashes. Default off.\r\n"
        "fastAI=%d\r\n"
        "\r\n"
        "; Bounded native notification batches. 1 = on (default), 0 = off; restart required.\r\n"
        "messageBatching=1\r\n"
        "\r\n"
        "; Optional network/timing CSV diagnostics; toggling from the menu closes the client.\r\n"
        "; 0 = off (default), 1 = on at next launch. See NETWORK_TRACE.md.\r\n"
        "netTrace=0\r\n"
        "\r\n"
        "; Write C4menu-<pid>.log / C4plugins.log diagnostics next to the exe.\r\n"
        "; 0 = off (default), 1 = on.\r\n"
        "debugLog=0\r\n",
        aa, battleEn, bSp, kAnimFactor[bSp - 1],
        mapEn, mSp, 10, autoConfirmHire, fastAi);
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

bool writeDdrawStr(const char* key, const char* value)
{
    // Preserves comment lines and writes a clean key=value (no inline comment), as cnc-ddraw's
    // strict parser needs.
    if (DDWriteConfigString(key, value))
        return true;
    return WritePrivateProfileStringA("ddraw", key, value, ddrawIni()) != FALSE;
}

bool writeDdrawBool(const char* key, bool on)
{
    return writeDdrawStr(key, on ? "true" : "false");
}

bool saveLiveDisplayModeForNextStart(int liveMode)
{
    if (liveMode < 0 || liveMode >= kModeCount)
        return false;

    const bool windowedSaved =
        writeDdrawStr("windowed", kModes[liveMode].windowed);
    const bool fullscreenSaved =
        writeDdrawStr("fullscreen", kModes[liveMode].fullscreen);
    bool transitionSaved = true;
    if (liveMode == 1)
        transitionSaved = writeDdrawBool("toggle_borderless", true);
    else if (liveMode == 2)
        transitionSaved = writeDdrawBool("toggle_borderless", false);

    if (windowedSaved && fullscreenSaved && transitionSaved) {
        g_modeIdx = liveMode;
        return true;
    }
    return false;
}

bool resetOutputToFollowGame(); // defined with the transactional output-size helpers below
void refreshChecks(); // menu tree is built later; this helper only updates it after an accepted Auto choice

void showFirstFullscreenPersistenceNotice(HWND owner)
{
    const char* f = iniFile();
    if (GetPrivateProfileIntA("menu", "fullscreenPersistenceNoticeShown", 0, f) != 0)
        return;

    // Mark before opening the modal: its message loop can receive another periodic chrome-sync.
    WritePrivateProfileStringA("menu", "fullscreenPersistenceNoticeShown", "1", f);

    int requestedMode = 0, requestedW = 0, requestedH = 0;
    const bool requestedValid =
        horplus_get_requested(&requestedMode, &requestedW, &requestedH) != 0;
    if (requestedValid && requestedMode == 2 && horplus_is_available()) {
        int outputW = 0, outputH = 0;
        int selectedW = 0, selectedH = 0;
        int nativeDisplaySize = -1;
        int currentW = 0, currentH = 0;
        DDGetScaleMetrics(&currentW, &currentH, nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr);
        if (horplus_get_primary_adaptive(&outputW, &outputH,
                                         &selectedW, &selectedH,
                                         &nativeDisplaySize)) {
            wchar_t message[1000] = {};
            if (currentW != selectedW || currentH != selectedH) {
                wsprintfW(
                    message,
                    L(L"Fullscreen is saved as the startup mode.\n\nAfter the next complete game start, Automatic resolution will change the current %d x %d game canvas to %d x %d. Fullscreen scales that canvas to the desktop; the automatic size is deliberately chosen so F4 can also restore a normal 1:1 window without downscaling. A restart is required because the game canvas cannot change safely while the game is running.",
                      L"Полноэкранный режим сохранён для следующих запусков.\n\nПосле следующего полного запуска автоматическое разрешение сменит текущий игровой кадр %d x %d на %d x %d. Полный экран масштабирует этот кадр до рабочего стола; автоматический размер специально выбирается так, чтобы F4 мог вернуть обычное окно 1:1 без уменьшения. Нужен перезапуск, потому что игровой кадр нельзя безопасно менять во время работы игры."),
                    currentW, currentH, selectedW, selectedH);
            } else {
                wsprintfW(
                    message,
                    L(L"Fullscreen is saved as the startup mode.\n\nAutomatic game resolution already selects %d x %d. Fullscreen scales it to the desktop, while F4 can restore it as a normal 1:1 window. It will be recalculated after a monitor or work-area change on the next complete game start.",
                      L"Полноэкранный режим сохранён для следующих запусков.\n\nАвтоматическое разрешение уже выбирает %d x %d. Полный экран масштабирует его до рабочего стола, а F4 может вернуть обычное окно 1:1. После смены монитора или рабочей области размер будет пересчитан при следующем полном запуске игры."),
                    selectedW, selectedH);
            }
            MessageBoxW(owner, message,
                        L(L"Fullscreen and game resolution",
                          L"Полный экран и разрешение игры"),
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    if (!requestedValid || requestedW <= 0 || requestedH <= 0)
        DDGetScaleMetrics(&requestedW, &requestedH, nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr);
    wchar_t message[1000] = {};
    int outputW = 0, outputH = 0;
    int selectedW = 0, selectedH = 0;
    int nativeDisplaySize = -1;
    if (requestedValid && horplus_is_available() &&
        horplus_get_primary_adaptive(&outputW, &outputH,
                                     &selectedW, &selectedH,
                                     &nativeDisplaySize)) {
        wsprintfW(
            message,
            L(L"Fullscreen is saved as the startup mode.\n\nThe game resolution is currently fixed manually at %d x %d. Fullscreen will scale that canvas but will not silently replace it. Automatic resolution would select the F4-compatible %d x %d canvas on the next complete game start.\n\nEnable Automatic resolution now?",
              L"Полноэкранный режим сохранён для следующих запусков.\n\nРазрешение игры сейчас закреплено вручную: %d x %d. Полный экран масштабирует этот кадр, но не заменяет его без спроса. Автоматическое разрешение при следующем полном запуске выберет совместимый с F4 кадр %d x %d.\n\nВключить автоматическое разрешение сейчас?"),
            requestedW, requestedH, selectedW, selectedH);
        if (MessageBoxW(owner, message,
                        L(L"Fullscreen and game resolution",
                          L"Полный экран и разрешение игры"),
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            if (horplus_set_requested(2, 0, 0)) {
                g_gameCanvasExplicitlySelected = true;
                resetOutputToFollowGame();
                refreshChecks();
                mlog("[menu] automatic game resolution accepted from first-fullscreen notice (%dx%d for %dx%d)",
                     selectedW, selectedH, outputW, outputH);
            } else {
                MessageBoxW(
                    owner,
                    L(L"Could not save Automatic resolution to Disciple.ini. The manually selected game canvas remains active.",
                      L"Не удалось сохранить автоматическое разрешение в Disciple.ini. Выбранный вручную игровой кадр остаётся активным."),
                    L(L"Automatic game resolution",
                      L"Автоматическое разрешение игры"),
                    MB_OK | MB_ICONERROR);
            }
        }
        return;
    }

    wsprintfW(
        message,
        L(L"Fullscreen is saved as the startup mode.\n\nThe game resolution remains fixed manually at %d x %d because Automatic resolution is unavailable for this executable. Fullscreen scales the existing canvas without replacing it.",
          L"Полноэкранный режим сохранён для следующих запусков.\n\nРазрешение игры остаётся закреплённым вручную: %d x %d, потому что автоматический выбор недоступен для этого exe. Полный экран масштабирует существующий кадр, не заменяя его."),
        requestedW, requestedH);
    MessageBoxW(owner, message,
                L(L"Fullscreen and game resolution",
                  L"Полный экран и разрешение игры"),
                MB_OK | MB_ICONINFORMATION);
}

// Re-apply ddraw settings live: we ARE the renderer, so call DDReloadConfig directly. Runs on the UI thread.
int applyDdrawLive(bool outputSizeChanged,
                   bool displayModeChanged,
                   bool rendererChanged = false)
{
    return DDReloadConfigForMenu(outputSizeChanged ? 1 : 0,
                                 displayModeChanged ? 1 : 0,
                                 rendererChanged ? 1 : 0);
}

void syncChrome(HWND hwnd); // defined after buildMenu(); always called on the window's GUI thread
bool stageChromeForTargetMode(HWND hwnd, int targetMode); // no repaint before renderer transition
int toggleWindowModeWithChrome(HWND hwnd); // stage target chrome before cnc-ddraw rebuilds the mode

// Take a screenshot via the renderer (timestamped PNG in .\Screenshots\).
void takeScreenshot()
{
    DDTakeScreenshot();
}

// Read current ddraw.ini values into menu state so the checks reflect reality.
void readDdrawState()
{
    char r[32] = {};
    // Match cnc-ddraw's own missing-key default.  Older/hand-edited profiles can contain a bare
    // "opengl" line, which is not an INI assignment and therefore really means Auto/D3D9.
    readDdrawStr("renderer", "auto", r,
                 static_cast<unsigned int>(sizeof(r)));
    g_rendererIdx = -1;
    for (int i = 0; i < kRendererCount; ++i)
        if (lstrcmpiA(r, kRenderers[i].value) == 0) {
            g_rendererIdx = i;
            break;
        }

    char sh[MAX_PATH] = {};
    readDdrawStr("shader", kShaders[0].value, sh,
                 static_cast<unsigned int>(sizeof(sh)));
    g_shaderIdx = -1;
    for (int i = 0; i < kShaderCount; ++i)
        if (lstrcmpiA(sh, kShaders[i].value) == 0) {
            g_shaderIdx = i;
            break;
        }
    g_d3dFilter = readDdrawInt("d3d9_filter", 3);
    if (g_d3dFilter < 0 || g_d3dFilter > 3)
        g_d3dFilter = 2;

    g_maintas = readDdrawBool("maintas", false);
    g_vsync = readDdrawBool("vsync", false);
    g_boxing = readDdrawBool("boxing", false);
    readDdrawStr("aspect_ratio", "", g_aspectRatio,
                 static_cast<unsigned int>(sizeof(g_aspectRatio)));
    g_singlecpu = readDdrawBool("singlecpu", true);

    const int ticks = readDdrawInt("maxgameticks", 180);
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
    int minWidth;    // never below the current logical game canvas
    int minHeight;
};

constexpr int kOutputSizeMinWidth = 800;
constexpr int kOutputSizeMinHeight = 600;
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
                L(L"Does not change the game view. Image area only; frame, menu and title bar are outside. To avoid hidden downscaling, the minimum for the current game canvas is %d x %d.",
                  L"Не меняет игровой обзор. Только область изображения; рамка, меню и заголовок снаружи. Чтобы не было скрытого уменьшения, минимум для текущего кадра игры — %d x %d."),
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

// The original DisciplesGL Resolution command changed the logical game size and its output as one
// choice. Preserve hand-edited ddraw.ini compatibility internally, but every normal game-resolution
// selection re-links the next normal window to the game canvas (0 x 0).
bool resetOutputToFollowGame()
{
    if (g_requestedOutputW == 0 && g_requestedOutputH == 0)
        return true;

    bool rollbackComplete = true;
    if (!saveOutputSize(0, 0, &rollbackComplete)) {
        MessageBoxW(
            g_gameHwnd,
            rollbackComplete
                ? L(L"The game resolution was saved, but the window size could not be linked back to it. The previous ddraw.ini width/height values remain.",
                    L"Разрешение игры сохранено, но размер окна не удалось снова привязать к нему. В ddraw.ini остались прежние width/height.")
                : L(L"The game resolution was saved, but updating the window/output pair failed and rollback was incomplete. Check ddraw.ini before restarting.",
                    L"Разрешение игры сохранено, но обновление пары размера окна/вывода завершилось ошибкой, а откат — не полностью. Проверьте ddraw.ini перед перезапуском."),
            L(L"Window/output size", L"Размер окна/вывода"),
            MB_OK | MB_ICONWARNING);
        return false;
    }

    g_requestedOutputW = 0;
    g_requestedOutputH = 0;
    g_resIdx = 0;
    return true;
}

// C4dll-R 1.6 allowed an outer output smaller than the logical game canvas when adjmouse was on.
// That created a second, non-native effective size (for example a fixed 800x600 menu inside a
// 1024x768 canvas became about 625x469). Version 1.7 restores cnc-ddraw's native-size minimum.
// Repair every stale persisted pair that the renderer can no longer honour instead of leaving a
// misleading 800x600 request which upstream will silently clamp to the game canvas.
void normalizeUndersizedOutput()
{
    int gameWidth = 0, gameHeight = 0;
    if (!horplus_get_active_size(&gameWidth, &gameHeight) ||
        gameWidth <= 0 || gameHeight <= 0)
        return;

    const int outputWidth = readDdrawInt("width", 0);
    const int outputHeight = readDdrawInt("height", 0);
    if (outputWidth == 0 && outputHeight == 0)
        return;
    if (outputWidth >= gameWidth && outputHeight >= gameHeight)
        return;

    bool rollbackComplete = true;
    if (saveOutputSize(0, 0, &rollbackComplete)) {
        // cfg_load already ran before c4features_install(). Keep cfg_save from restoring the stale
        // pair when this process exits.
        DDSetOutputConfigMemory(0, 0);
        mlog("[menu] normalized undersized output %dx%d below game canvas %dx%d to follow-game 0x0",
             outputWidth, outputHeight, gameWidth, gameHeight);
    } else {
        mlog("[menu] FAILED to normalize undersized output %dx%d below game canvas %dx%d (rollback=%d)",
             outputWidth, outputHeight, gameWidth, gameHeight,
             rollbackComplete ? 1 : 0);
    }
}

bool chooseOutputSize(HWND owner, int* width, int* height)
{
    if (!width || !height)
        return false;

    int gameWidth = 0, gameHeight = 0, outputWidth = 0, outputHeight = 0;
    const bool haveMetrics =
        DDGetScaleMetrics(&gameWidth, &gameHeight, &outputWidth, &outputHeight,
                          nullptr, nullptr, nullptr, nullptr) != 0;
    const bool haveCanvas =
        haveMetrics || horplus_get_active_size(&gameWidth, &gameHeight) != 0;
    int minWidth = kOutputSizeMinWidth;
    int minHeight = kOutputSizeMinHeight;
    if (haveCanvas) {
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
    wchar_t message[512] = {};
    if (nativeDisplaySize >= 0) {
        swprintf_s(
            message,
            L(L"Selected game resolution: %d x %d (original Disciples II mode).\n\nIt will be applied after fully closing and restarting the game. The normal window will use the same image size, without additional scaling.",
              L"Выбрано разрешение игры: %d x %d (штатный режим Disciples II).\n\nОно применится после полного закрытия и повторного запуска игры. Размер изображения в обычном окне будет таким же, без дополнительного масштабирования."),
            width, height);
    } else {
        swprintf_s(
            message,
            L(L"Selected game resolution: %d x %d. It shows more map instead of stretching the picture.\n\nIt will be applied after fully closing and restarting the game. The normal window will use the same image size.",
              L"Выбрано разрешение игры: %d x %d. Оно показывает больше карты, а не растягивает изображение.\n\nРазрешение применится после полного закрытия и повторного запуска игры. Размер изображения в обычном окне будет таким же."),
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
    (void)outputWidth;
    (void)outputHeight;
    (void)nativeDisplaySize;
    wchar_t message[512] = {};
    swprintf_s(
        message,
        L(L"Current game resolution: %d x %d.\nAfter restart: %d x %d (Automatic).\n\nThe normal window will use the same image size, without additional scaling. Automatic recalculates the choice after a monitor or display-mode change.",
          L"Сейчас: %d x %d.\nПосле перезапуска: %d x %d (Авто).\n\nРазмер изображения в обычном окне будет соответствовать разрешению игры, без дополнительного масштабирования. После смены монитора или режима экрана Авто пересчитает выбор."),
        currentWidth, currentHeight, selectedWidth, selectedHeight);
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

extern "C" void featuremenu_debug_log_line(const char* line)
{
    if (line && *line)
        mlog("%s", line);
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
    const int idle = g_battleAnimEnabled ? g_battleBaseFactor : 10;

    // Animation Pause is a timer feature, not an attack-speed feature. Consume the exact native
    // start/end events even when the optional burst is disabled. The timer itself reads the
    // verified game-owned UI-ready byte and must never expire playback by wall time.
    if (visualEvent == 2) {
        InterlockedExchange(&g_attackPlaybackActive, 0);
    }

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
        (void)outputWidth;
        (void)outputHeight;
        (void)nativeDisplaySize;
        swprintf_s(
            label,
            L(L"Automatic (fit to screen): %d x %d after restart",
              L"Авто (подобрать по экрану): %d x %d после перезапуска"),
            canvasWidth, canvasHeight);
    } else {
        lstrcpynW(
            label,
            L(L"Automatic resolution (unavailable for this executable)",
              L"Автоматическое разрешение (недоступно для этого exe)"),
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
        lstrcpynW(
            label,
            g_ver == VerEditor
                ? L(L"Window size (live): Automatic...",
                    L"Размер окна (сразу): автоматически...")
                : L(L"Window/output (live): Automatic; follows game resolution...",
                    L"Окно/вывод (сразу): автоматически; следует разрешению игры..."),
            static_cast<int>(sizeof(label) / sizeof(label[0])));
    } else if (g_requestedOutputW > 0 && g_requestedOutputH > 0) {
        swprintf_s(
            label,
            g_ver == VerEditor
                ? L(L"Window size (live): %d x %d (%s)...",
                    L"Размер окна (сразу): %d x %d (%s)...")
                : L(L"Window/output (live): %d x %d (%s); game view unchanged...",
                    L"Окно/вывод (сразу): %d x %d (%s); обзор не меняется..."),
            g_requestedOutputW, g_requestedOutputH,
            gameAspectLabel(g_requestedOutputW, g_requestedOutputH));
    } else {
        swprintf_s(
            label,
            g_ver == VerEditor
                ? L(L"Window size: invalid saved value (%d x %d)...",
                    L"Размер окна: ошибочное значение (%d x %d)...")
                : L(L"Window/output: invalid saved value (%d x %d)...",
                    L"Окно/вывод: ошибочное значение (%d x %d)..."),
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
        (void)outputWidth;
        (void)outputHeight;
        (void)nativeDisplaySize;
        if (pending) {
            swprintf_s(
                line,
                L(L"Current: %dx%d -> after restart: %dx%d (Automatic)",
                  L"Сейчас: %dx%d -> после перезапуска: %dx%d (Авто)"),
                currentW, currentH, adaptiveWidth, adaptiveHeight);
        } else {
            swprintf_s(
                line,
                L(L"Current: %dx%d (Automatic)",
                  L"Сейчас: %dx%d (Авто)"),
                currentW, currentH);
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
                   L(L"Fixed output %dx%d; game canvas %dx%d keeps its native-size minimum.",
                       L"Фикс. вывод %dx%d; кадр игры %dx%d сохраняет минимум своего родного размера."),
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

double windowStretchScaleForPercent(int gameWidth, int gameHeight, int effectPercent)
{
    int cropWidth = gameWidth;
    int cropHeight = gameHeight;
    if (gameWidth <= 0 || gameHeight <= 0)
        return 1.0;

    DDCalcWindowStretchCrop(
        gameWidth, gameHeight, effectPercent,
        nullptr, nullptr, &cropWidth, &cropHeight);

    const double aspect = static_cast<double>(gameWidth) / gameHeight;
    if (aspect >= 4.0 / 3.0)
        return cropHeight > 0 ? static_cast<double>(gameHeight) / cropHeight : 1.0;

    return cropWidth > 0 ? static_cast<double>(gameWidth) / cropWidth : 1.0;
}

int windowStretchSizePercent(int gameWidth, int gameHeight, int effectPercent)
{
    return static_cast<int>(
        windowStretchScaleForPercent(gameWidth, gameHeight, effectPercent) * 100.0 + 0.5);
}

void refreshWindowStretchInfo()
{
    if (!g_windowStretchMenu)
        return;

    g_windowStretchPercent = DDGetWindowStretchPercent();
    if (g_windowStretchPercent < 0)
        g_windowStretchPercent = 0;
    if (g_windowStretchPercent > 100)
        g_windowStretchPercent = 100;

    CheckMenuRadioItem(
        g_windowStretchMenu, kIdWindowStretchBase,
        kIdWindowStretchBase + 10,
        kIdWindowStretchBase + static_cast<UINT>(g_windowStretchPercent / 10),
        MF_BYCOMMAND);

    int gameW = 0, gameH = 0, outW = 0, outH = 0;
    int viewW = 0, viewH = 0;
    DDGetScaleMetrics(&gameW, &gameH, &outW, &outH,
                      nullptr, nullptr, &viewW, &viewH);

    // DisciplesGL stores 0..100 as the strength of its crop, not as the displayed size.
    // Keep that compatible value in C4menu.ini, but expose the resulting 100%..N% size so
    // "100%" once again means no enlargement to a player. At 1366x768, legacy 100 becomes 128%.
    const int selectedSizePercent =
        windowStretchSizePercent(gameW, gameH, g_windowStretchPercent);
    const int maximumSizePercent =
        windowStretchSizePercent(gameW, gameH, 100);
    const bool canEnlarge = maximumSizePercent > 100;
    for (int i = 0; i <= 10; ++i) {
        const int sizePercent = windowStretchSizePercent(gameW, gameH, i * 10);
        wchar_t option[128] = {};
        if (i == 0) {
            lstrcpynW(option,
                      L(L"100% - no enlargement", L"100% - без увеличения"),
                      static_cast<int>(sizeof(option) / sizeof(option[0])));
        } else if (i == 10 && canEnlarge) {
            swprintf_s(option,
                       L(L"%d%% - fill height (default)",
                         L"%d%% - на всю высоту (по умолчанию)"),
                       sizePercent);
        } else if (i == 10) {
            lstrcpynW(option,
                      L(L"100% - already full height (default)",
                        L"100% - уже на всю высоту (по умолчанию)"),
                      static_cast<int>(sizeof(option) / sizeof(option[0])));
        } else {
            swprintf_s(option, L"%d%%", sizePercent);
        }

        MENUITEMINFOW optionInfo{};
        optionInfo.cbSize = sizeof(optionInfo);
        optionInfo.fMask = MIIM_STRING;
        optionInfo.dwTypeData = option;
        SetMenuItemInfoW(g_windowStretchMenu,
                         kIdWindowStretchBase + static_cast<UINT>(i),
                         FALSE, &optionInfo);
        EnableMenuItem(g_windowStretchMenu,
                       kIdWindowStretchBase + static_cast<UINT>(i),
                       MF_BYCOMMAND | (i == 0 || canEnlarge ? MF_ENABLED : MF_GRAYED));
    }

    int cropW = gameW;
    int cropH = gameH;
    DDCalcWindowStretchCrop(
        gameW, gameH, g_windowStretchPercent,
        nullptr, nullptr, &cropW, &cropH);

    const int effectiveZoom = DDGetSimpleZoom1000();
    const int extraZoom = DDGetSimpleZoomExtra1000();
    const bool stretchActive = DDIsWindowStretchActive() != 0;
    wchar_t line[384] = {};
    if (g_windowStretchPercent == 0) {
        swprintf_s(
            line,
            L(L"100%% (live): no enlargement; full game canvas %dx%d is shown.",
              L"100%% (сразу): без увеличения, показан полный игровой кадр %dx%d."),
            gameW, gameH);
    } else if (stretchActive && (cropW < gameW || cropH < gameH)) {
        swprintf_s(
            line,
            L(L"Active size %d%% (live): centred %dx%d -> viewport %dx%d; effective %.3gx.",
              L"Размер %d%% активен (сразу): центр %dx%d -> viewport %dx%d; итог %.3gx."),
            selectedSizePercent, cropW, cropH, viewW, viewH,
            effectiveZoom / 1000.0);
    } else if (!stretchActive) {
        swprintf_s(
            line,
            L(L"Selected size %d%% (live); waiting for a fixed screen. The strategic map stays unzoomed.",
              L"Выбран размер %d%% (сразу); ожидается фиксированное окно. Карта не приближается."),
            selectedSizePercent);
    } else {
        swprintf_s(
            line,
            L(L"Selected size %d%% (live), but %dx%d has no fixed-window border to crop.",
              L"Выбран размер %d%% (сразу), но в %dx%d нет полей фиксированного окна для обрезки."),
            selectedSizePercent, gameW, gameH);
    }
    ModifyMenuW(g_windowStretchMenu, kIdWindowStretchInfo,
                MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdWindowStretchInfo, line);

    lstrcpynW(
        line,
        extraZoom != 1000
            ? L(L"Ctrl+Wheel zoom is also active; selecting a size resets it and recentres.",
                L"Также активно Ctrl+колесо; выбор размера сбросит его и вернёт центр.")
            : stretchActive && (cropW < gameW || cropH < gameH)
            ? L(L"Fills vertically, crops side background and resamples pixels (quality loss is expected).",
                L"Заполняет по вертикали, обрезает боковой фон и пересчитывает пиксели (потеря качества ожидаема).")
            : L(L"It activates automatically on fixed menus/battles; the map view is never cropped.",
                L"Автоматически включается в фиксированных меню/боях; обзор карты не обрезается."),
        static_cast<int>(sizeof(line) / sizeof(line[0])));
    ModifyMenuW(g_windowStretchMenu, kIdWindowStretchHelp,
                MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdWindowStretchHelp, line);

    if (g_videoMenu && g_windowStretchMenuPosition >= 0) {
        wchar_t parent[160] = {};
        if (g_windowStretchPercent == 0)
            lstrcpynW(parent,
                      L(L"Fixed window: 100% (no enlargement, live)",
                        L"Центральное окно: 100% (без увеличения, сразу)"),
                      static_cast<int>(sizeof(parent) / sizeof(parent[0])));
        else if (g_windowStretchPercent == 100 && canEnlarge)
            swprintf_s(parent,
                       L(L"Fixed window: %d%%, fill height (live)",
                         L"Центральное окно: %d%%, на всю высоту (сразу)"),
                       selectedSizePercent);
        else if (!canEnlarge)
            lstrcpynW(parent,
                      L(L"Fixed window: 100% (already full height, live)",
                        L"Центральное окно: 100% (уже на всю высоту, сразу)"),
                      static_cast<int>(sizeof(parent) / sizeof(parent[0])));
        else
            swprintf_s(parent,
                       L(L"Fixed window: %d%% (live)",
                         L"Центральное окно: %d%% (сразу)"),
                       selectedSizePercent);
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_STRING;
        info.dwTypeData = parent;
        SetMenuItemInfoW(g_videoMenu,
                         static_cast<UINT>(g_windowStretchMenuPosition),
                         TRUE, &info);
    }
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
        label = L(L"(MNS/SMNS) Show map clouds (after restart)",
                  L"(MNS/SMNS) Показывать облака на карте (после перезапуска)");
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

bool shaderSupportedByActiveRenderer(int shaderIndex, int activeRenderer)
{
    if (shaderIndex < 0 || shaderIndex >= kShaderCount)
        return false;
    if (activeRenderer == 0)
        return g_shaderAvailable[shaderIndex];
    return (activeRenderer == 1 || activeRenderer == 2) &&
           portableFilterForShader(shaderIndex) >= 0;
}

void refreshShaderAssetStatus()
{
    if (!g_shaderMenu)
        return;

    int unavailable = 0;
    for (int i = 0; i < kShaderCount; ++i) {
        if (!g_shaderAvailable[i])
            ++unavailable;
    }

    wchar_t status[256] = {};
    if (!g_shaderFolderReady) {
        lstrcpynW(
            status,
            L(L"Shaders folder missing: OpenGL filter files are unavailable",
              L"Папка Shaders не найдена: файлы OpenGL-фильтров недоступны"),
            static_cast<int>(sizeof(status) / sizeof(status[0])));
    } else if (g_missingShaderFileCount) {
        swprintf_s(
            status,
            L(L"Shaders incomplete: required files missing: %d; unavailable OpenGL filters: %d",
              L"Папка Shaders неполна: обязательных файлов не найдено: %d; недоступно OpenGL-фильтров: %d"),
            g_missingShaderFileCount, unavailable);
    } else {
        swprintf_s(
            status,
            L(L"Shader files ready: all %d OpenGL filters are available",
              L"Файлы шейдеров готовы: доступны все %d OpenGL-фильтров"),
            kShaderCount);
    }
    ModifyMenuW(g_shaderMenu, kIdShaderStatus,
                MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                kIdShaderStatus, status);
}

void refreshChecks()
{
    if (g_technicalMenu) {
        const bool forced = c4trace_environment_forced() != 0;
        CheckMenuItem(g_technicalMenu, kIdNetTrace, MF_BYCOMMAND |
                      ((forced || c4trace_configured(iniFile())) ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(g_technicalMenu, kIdNetTrace, MF_BYCOMMAND |
                       (forced ? MF_GRAYED : MF_ENABLED));
        ModifyMenuW(g_technicalMenu, kIdNetTraceInfo, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                    kIdNetTraceInfo,
                    forced
                        ? L(L"Forced by C4DLL_NETTRACE=1; change the launch environment to disable.",
                            L"Включено через C4DLL_NETTRACE=1; отключается в среде запуска.")
                        : L(L"Changing this option closes the client. Logs are limited; see NETWORK_TRACE.md.",
                            L"Смена настройки закрывает клиент. Логи ограничены; см. NETWORK_TRACE.md."));
    }
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
    const int battlePreset = g_battleAnimEnabled ? animPresetForFactor(g_battleBaseFactor) : 0;
    if (g_battleAnimMenu) {
        const UINT selected = battlePreset
            ? kIdAnim1 + static_cast<UINT>(battlePreset - 1)
            : kIdAnimOff;
        CheckMenuRadioItem(g_battleAnimMenu, kIdAnimOff, kIdAnim6,
                           selected, MF_BYCOMMAND);
        // A fine Ctrl+/- value intentionally sits between the coarse menu presets. Leave every
        // radio item clear instead of claiming that the nearest preset is active.
        if (g_battleAnimEnabled && !battlePreset)
            CheckMenuItem(g_battleAnimMenu, kIdAnimOff, MF_BYCOMMAND | MF_UNCHECKED);
    }
    const int mapPreset = g_mapAnimEnabled ? animPresetForFactor(g_mapBaseFactor) : 0;
    if (g_mapAnimMenu) {
        const UINT selected = mapPreset
            ? kIdAnimMap1 + static_cast<UINT>(mapPreset - 1)
            : kIdAnimMapOff;
        CheckMenuRadioItem(g_mapAnimMenu, kIdAnimMapOff, kIdAnimMap6,
                           selected, MF_BYCOMMAND);
        if (g_mapAnimEnabled && !mapPreset)
            CheckMenuItem(g_mapAnimMenu, kIdAnimMapOff, MF_BYCOMMAND | MF_UNCHECKED);
    }
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
    // ModifyMenuW refreshes the dynamic Custom/info strings, so do it before applying checks.
    refreshScaleInfo();
    refreshWindowStretchInfo();
    refreshOutputSizeItem();
    refreshAdaptiveCanvasItem();
    if (g_displaySizeMenu && g_ver != VerEditor) {
        const bool canvasMenuAvailable =
            horplus_is_available() || g_ver == VerRussobit;
        for (UINT id = kIdDisplaySize0; id <= kIdDisplaySize2; ++id)
        {
            CheckMenuItem(g_displaySizeMenu, id,
                          MF_BYCOMMAND | MF_UNCHECKED);
            EnableMenuItem(g_displaySizeMenu, id,
                           MF_BYCOMMAND |
                               (canvasMenuAvailable ? MF_ENABLED : MF_GRAYED));
        }
        for (UINT id = kIdHorplusBase;
             id < kIdHorplusBase + static_cast<UINT>(kHorplusSizeCount);
             ++id) {
            CheckMenuItem(g_displaySizeMenu, id,
                          MF_BYCOMMAND | MF_UNCHECKED);
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
                    CheckMenuItem(g_displaySizeMenu, selected,
                                  MF_BYCOMMAND | MF_CHECKED);
                }
            } else if (requestedMode == 2) {
                selected = kIdHorplusAuto;
                CheckMenuItem(g_displaySizeMenu, selected,
                              MF_BYCOMMAND | MF_CHECKED);
            } else {
                selected =
                    kIdDisplaySize0 + static_cast<UINT>(g_displaySizePending);
                CheckMenuItem(g_displaySizeMenu, selected,
                              MF_BYCOMMAND | MF_CHECKED);
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
    const int activeRenderer = DDGetActiveRenderer();
    const int activePortableFilter = DDGetActivePortableFilter();
    if (g_shaderMenu) {
        refreshShaderAssetStatus();
        const int shownShader =
            activeRenderer == 0 ? g_shaderIdx
                                : shaderForPortableFilter(
                                      activePortableFilter >= 0
                                          ? activePortableFilter
                                          : g_d3dFilter);
        if (shownShader >= 0)
            CheckMenuRadioItem(g_shaderMenu, kIdShaderBase,
                               kIdShaderBase + kShaderCount - 1,
                               kIdShaderBase + static_cast<UINT>(shownShader),
                               MF_BYCOMMAND);
        for (UINT id = kIdShaderBase;
             id < kIdShaderBase + static_cast<UINT>(kShaderCount); ++id) {
            const int index = static_cast<int>(id - kIdShaderBase);
            const bool supported =
                shaderSupportedByActiveRenderer(index, activeRenderer);
            EnableMenuItem(g_shaderMenu, id,
                           MF_BYCOMMAND |
                               (supported ? MF_ENABLED : MF_GRAYED));
        }
    }
    if (g_rendMenu) {
        const bool openGlSelectable =
            g_shaderIdx < 0 || g_shaderAvailable[g_shaderIdx];
        EnableMenuItem(g_rendMenu, kIdRendOpenGL,
                       MF_BYCOMMAND |
                           (openGlSelectable ? MF_ENABLED : MF_GRAYED));
        if (g_rendererIdx >= 0)
            CheckMenuRadioItem(g_rendMenu, kIdRendOpenGL, kIdRendAuto,
                               kIdRendOpenGL + static_cast<UINT>(g_rendererIdx),
                               MF_BYCOMMAND);
        const wchar_t* active =
            activeRenderer == 0 ? L"OpenGL"
            : activeRenderer == 1 ? L"GDI"
            : activeRenderer == 2 ? L"Direct3D 9"
            : activeRenderer == 3 ? L(L"Null (headless)", L"Null (без экрана)")
                                  : L(L"not initialized", L"не инициализирован");
        wchar_t label[128] = {};
        wsprintfW(label, L(L"Active now: %s", L"Сейчас активен: %s"), active);
        ModifyMenuW(g_rendMenu, kIdRendererActive,
                    MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                    kIdRendererActive, label);
    }
    if (g_scaleMenu) {
        CheckMenuItem(g_scaleMenu, kIdMaintas, MF_BYCOMMAND | MF_UNCHECKED);
        CheckMenuItem(g_scaleMenu, kIdBoxing, MF_BYCOMMAND | MF_UNCHECKED);
        CheckMenuItem(g_scaleMenu, kIdScaleCustom, MF_BYCOMMAND | MF_UNCHECKED);
        const UINT scale = g_boxing          ? kIdBoxing
                           : g_aspectRatio[0] ? kIdScaleCustom
                           : g_maintas       ? kIdMaintas
                                             : 0;
        // A hand-edited independent-X/Y legacy mode intentionally has no GUI radio entry.
        if (scale)
            CheckMenuRadioItem(g_scaleMenu, kIdMaintas, kIdScaleCustom, scale,
                               MF_BYCOMMAND);
    }
    if (g_videoMenu)
        CheckMenuItem(g_videoMenu, kIdVsync, MF_BYCOMMAND | (g_vsync ? MF_CHECKED : MF_UNCHECKED));
    // Performance
    if (g_fpsMenu && g_fpsIdx >= 0)
        CheckMenuRadioItem(g_fpsMenu, kIdFpsBase, kIdFpsBase + kFpsCount - 1,
                           kIdFpsBase + static_cast<UINT>(g_fpsIdx), MF_BYCOMMAND);
    if (g_ticksMenu && g_ticksIdx >= 0)
        CheckMenuRadioItem(g_ticksMenu, kIdTicks0, kIdTicks180,
                           kTicksCommandIds[g_ticksIdx], MF_BYCOMMAND);
    if (g_perfMenu) {
        CheckMenuItem(g_perfMenu, kIdSingleCpu,
                      MF_BYCOMMAND | (g_singlecpu ? MF_CHECKED : MF_UNCHECKED));
        const bool fastAiAvailable = fastai_is_available() != 0;
        EnableMenuItem(g_perfMenu, kIdFastAi,
                       MF_BYCOMMAND | (fastAiAvailable ? MF_ENABLED : MF_GRAYED));
        CheckMenuItem(g_perfMenu, kIdFastAi,
                      MF_BYCOMMAND | (fastai_get_enabled() ? MF_CHECKED : MF_UNCHECKED));
    }
}

void verifyPendingRenderer()
{
    if (!g_pendingRendererVerifyTick ||
        static_cast<LONG>(GetTickCount() - g_pendingRendererVerifyTick) < 0)
        return;

    g_pendingRendererVerifyTick = 0;
    const int active = DDGetActiveRenderer();
    refreshChecks();
    if (active == 0)
        return;

    const char* activeName =
        active == 1 ? "GDI" : active == 2 ? "Direct3D 9"
        : active == 3 ? "null"
                      : "unknown";
    mlog("[menu] OpenGL runtime self-test fell back asynchronously: active=%s",
         activeName);
    MessageBoxW(
        g_gameHwnd,
        L(L"OpenGL loaded, but its runtime rendering test failed and the wrapper switched to a safe backend. This commonly means an incomplete local Mesa package or an RDP OpenGL 1.1 driver. The Renderer status row shows what is active. The Lanczos setting also drives the portable Auto/Direct3D 9 filter and GDI smoothing fallback.",
          L"OpenGL загрузился, но не прошёл runtime-проверку рендера, поэтому враппер включил безопасный fallback. Обычные причины — неполный локальный комплект Mesa или OpenGL 1.1 в RDP. Фактический backend показан в строке статуса. Lanczos также выбирает переносимый фильтр Auto/Direct3D 9 и сглаживающий fallback GDI."),
        L(L"OpenGL runtime fallback", L"Runtime-fallback OpenGL"),
        MB_OK | MB_ICONWARNING);
}

void resetWrapperSettings()
{
    if (MessageBoxW(
            g_gameHwnd,
            L(L"Restore all wrapper defaults and CLOSE THE CLIENT NOW?\n\nTimer/plugin configuration and the game's own gameplay options will NOT be reset. The current multiplayer connection will end. Unsaved progress is NOT saved automatically.\n\nAfter the client closes, launch it again to apply all defaults together, including OpenGL + Lanczos, scaling, CPU affinity, animation speeds and message batching.",
              L"Вернуть все стандартные настройки враппера и СЕЙЧАС ЗАКРЫТЬ КЛИЕНТ?\n\nНастройки таймера, плагинов и родные игровые параметры НЕ сбрасываются. Текущее сетевое соединение завершится. Несохранённый прогресс автоматически НЕ сохраняется.\n\nПосле закрытия запустите клиент снова: все стандартные настройки применятся вместе, включая OpenGL + Lanczos, масштаб, число CPU, скорости анимаций и обработку очереди."),
            L(L"Reset wrapper settings", L"Сброс настроек враппера"),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    char rendererPath[MAX_PATH] = {}, rendererSection[256] = {};
    if (!DDGetConfigWriteTarget(rendererPath, sizeof(rendererPath),
                                rendererSection, sizeof(rendererSection))) {
        MessageBoxW(g_gameHwnd,
                    L(L"Cannot determine the active ddraw.ini profile. Nothing was reset.",
                      L"Не удалось определить активный профиль ddraw.ini. Ничего не сброшено."),
                    L(L"Reset failed", L"Ошибка сброса"), MB_OK | MB_ICONERROR);
        return;
    }
    // exeDirFile reuses a scratch buffer: retain independent path strings.
    const std::string menuPath = iniFile();
    const std::string gamePath = discipleIni();
    std::vector<c4_ini_reset::Entry> entries;
    for (const auto& setting : c4defaults::renderer) {
        entries.push_back({rendererPath, "ddraw", setting.key, setting.value});
        // Empty profile values inherit [ddraw] (notably aspect_ratio), so reset both
        // the global defaults and the currently effective profile, never other profiles.
        if (lstrcmpiA(rendererSection, "ddraw") != 0)
            entries.push_back({rendererPath, rendererSection, setting.key, setting.value});
    }
    for (const auto& setting : c4defaults::menu)
        entries.push_back({menuPath, "menu", setting.key, setting.value});
    entries.push_back({menuPath, "menu", "autoConfirmUnitHire",
                       g_ver == VerRussobit ? "1" : "0"});
    for (const auto& setting : c4defaults::wrapper)
        entries.push_back({gamePath, "Wrapper", setting.key, setting.value});
    char locale[24] = {};
    wsprintfA(locale, "%lu", GetUserDefaultLCID());
    entries.push_back({gamePath, "Wrapper", "Locale", locale});

    if (horplus_is_available()) {
        // Auto uses a window-sized canvas, not the old fullscreen/custom output.
        // Resolve the monitor work area before the transaction; the live canvas is
        // never patched here. Its native compatibility base must match next start.
        MONITORINFO monitor = {sizeof(MONITORINFO)};
        int width = 0, height = 0, nativeSize = -1;
        if (!GetMonitorInfoW(MonitorFromWindow(g_gameHwnd, MONITOR_DEFAULTTOPRIMARY), &monitor) ||
            !horplus_get_adaptive_for_output(
                monitor.rcWork.right - monitor.rcWork.left,
                monitor.rcWork.bottom - monitor.rcWork.top,
                &width, &height, &nativeSize)) {
            MessageBoxW(g_gameHwnd,
                        L(L"Cannot determine a safe automatic game resolution. Nothing was reset.",
                          L"Не удалось определить безопасное автоматическое разрешение. Ничего не сброшено."),
                        L(L"Reset failed", L"Ошибка сброса"), MB_OK | MB_ICONERROR);
            return;
        }
        char nativeText[16] = {};
        wsprintfA(nativeText, "%d", nativeSize < 0 ? 0 : nativeSize);
        entries.push_back({gamePath, "Disciple", "DisplaySize", nativeText});
        entries.push_back({gamePath, "Wrapper", "GameCanvasMode", "2"});
        entries.push_back({gamePath, "Wrapper", "GameCanvasWidth", "0"});
        entries.push_back({gamePath, "Wrapper", "GameCanvasHeight", "0"});
        entries.push_back({gamePath, "Wrapper", "LegacyDisplaySizeMigrated", "1"});
    }

    const auto saved = c4_ini_reset::Apply(entries);
    if (!saved.success) {
        mlog("[menu] settings reset failed index=%u error=%lu rollback=%d",
             static_cast<unsigned>(saved.failedIndex), saved.error, saved.rollbackComplete);
        MessageBoxW(g_gameHwnd,
                    saved.rollbackComplete
                        ? L(L"Could not save the defaults. Previous settings were restored; live settings are unchanged.",
                            L"Не удалось сохранить стандартные настройки. Прежние значения восстановлены; текущие настройки не изменены.")
                        : L(L"Saving and rollback failed. Live settings are unchanged; check ddraw.ini, C4menu.ini and Disciple.ini before restarting.",
                            L"Сохранение и откат завершились ошибкой. Текущие настройки не изменены; проверьте ddraw.ini, C4menu.ini и Disciple.ini перед перезапуском."),
                    L(L"Reset failed", L"Ошибка сброса"), MB_OK | MB_ICONERROR);
        return;
    }

    // No partial live reset: all defaults become active together on the next start.
    // Suppress old runtime geometry persistence for this exit only.
    DDExitClientAfterSettingsChange(1);
}

void toggleNetworkTrace()
{
    if (c4trace_environment_forced()) {
        MessageBoxW(g_gameHwnd,
                    L(L"Diagnostics are forced by C4DLL_NETTRACE=1. Remove that variable from the launch environment and restart to disable recording.",
                      L"Диагностика принудительно включена через C4DLL_NETTRACE=1. Для отключения уберите эту переменную из среды запуска и перезапустите клиент."),
                    L(L"Diagnostics", L"Диагностика"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    const bool enabled = c4trace_configured(iniFile()) == 0;
    if (MessageBoxW(g_gameHwnd,
            enabled
                ? L(L"Enable network/timing diagnostics and CLOSE THE CLIENT NOW?\n\nLaunch it again to start recording. The multiplayer connection will end; unsaved progress is NOT saved automatically.\n\nCSV logs are written beside the EXE: up to 32 MiB per process, stopping below 64 MiB of free space. Recording affects timing. Enable it separately in both clients when investigating a network issue. See NETWORK_TRACE.md.",
                    L"Включить диагностику сети/задержек и СЕЙЧАС ЗАКРЫТЬ КЛИЕНТ?\n\nЗапустите его снова для начала записи. Сетевое соединение завершится; несохранённый прогресс автоматически НЕ сохраняется.\n\nCSV создаются рядом с EXE: до 32 МиБ на процесс, остановка при остатке менее 64 МиБ. Запись влияет на тайминги. Для сетевого исследования включите её отдельно в обоих клиентах. Подробности — NETWORK_TRACE.md.")
                : L(L"Disable diagnostics and CLOSE THE CLIENT NOW?\n\nThe next launch will run without recording. The multiplayer connection will end; unsaved progress is NOT saved automatically. Existing logs will not be deleted.",
                    L"Отключить диагностику и СЕЙЧАС ЗАКРЫТЬ КЛИЕНТ?\n\nСледующий запуск будет без записи. Сетевое соединение завершится; несохранённый прогресс автоматически НЕ сохраняется. Существующие логи не удаляются."),
            L(L"Diagnostics: restart required", L"Диагностика: нужен перезапуск"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    const std::vector<c4_ini_reset::Entry> entries = {
        {iniFile(), "menu", "netTrace", enabled ? "1" : "0"}};
    const auto saved = c4_ini_reset::Apply(entries);
    if (!saved.success) {
        MessageBoxW(g_gameHwnd,
                    saved.rollbackComplete
                        ? L(L"Could not save diagnostics. The previous setting remains; the client stays open.",
                            L"Не удалось сохранить диагностику. Прежняя настройка сохранена; клиент остаётся открытым.")
                        : L(L"Saving and rollback failed. Check [menu] netTrace in C4menu.ini. The client stays open.",
                            L"Сохранение и откат завершились ошибкой. Проверьте [menu] netTrace в C4menu.ini. Клиент остаётся открытым."),
                    L(L"Diagnostics", L"Диагностика"), MB_OK | MB_ICONERROR);
        return;
    }
    DDExitClientAfterSettingsChange(0);
}

bool menuCommandReloadsCurrentRenderer(UINT id)
{
    return id == kIdMaintas || id == kIdVsync || id == kIdBoxing ||
           id == kIdOutputSizeCustom ||
           (g_ver == VerEditor && id >= kIdResBase &&
            id < kIdResBase + static_cast<UINT>(kResCount)) ||
           (id >= kIdFpsBase && id < kIdFpsBase + static_cast<UINT>(kFpsCount)) ||
           (id >= kIdModeWindowed && id <= kIdModeExclusive);
}

void onMenuCommand(UINT id)
{
    if (id == kIdNetTrace) {
        toggleNetworkTrace();
        return;
    }
    if (id == kIdResetWrapper) {
        resetWrapperSettings();
        return;
    }
    if (g_ver != VerRussobit &&
        (id == kIdAlwaysActive ||
         (id >= kIdAnimOff && id <= kIdAnim6) ||
         (id >= kIdBattle1 && id <= kIdBattle4) ||
         (id >= kIdMap1 && id <= kIdMap3) ||
         (id >= kIdAnimMapOff && id <= kIdAnimMap6) ||
         (id >= kIdDragScroll && id <= kIdDialogVoInfo) ||
         id == kIdAutoConfirmUnitHire || id == kIdClouds || id == kIdFastAi)) {
        // Disabled menu items normally cannot generate WM_COMMAND, but never
        // let a synthetic command reach an exact-address MNS/SMNS path.
        return;
    }

    // Preserve the already compiled OpenGL program if its file was removed after startup. Every
    // command below would otherwise destroy it and silently compile cnc-ddraw's fallback. Reject
    // before changing INI or menu state; restoring the complete folder makes the command live again.
    if (menuCommandReloadsCurrentRenderer(id) && DDGetActiveRenderer() == 0 &&
        g_shaderIdx >= 0) {
        scanShaderAssets();
        if (!g_shaderAvailable[g_shaderIdx]) {
            refreshChecks();
            showShaderAssetsWarningOnce(g_gameHwnd);
            return;
        }
    }

    // These choices re-create only the presentation backend; the game process keeps running.
    bool reloadRenderer = false;
    bool outputSizeChanged = false;
    bool displayModeChanged = false;
    bool rendererChanged = false;
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
        const bool outputWasCustom =
            g_requestedOutputW != 0 || g_requestedOutputH != 0;
        const bool changed =
            !oldValid || oldMode != 0 || g_displaySizePending != value ||
            outputWasCustom;
        const bool saved = horplus_set_native_requested(value) != 0;
        if (saved) {
            g_gameCanvasExplicitlySelected = true;
            resetOutputToFollowGame();
        }
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
        const bool outputWasCustom =
            g_requestedOutputW != 0 || g_requestedOutputH != 0;
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
        if (saved) {
            g_gameCanvasExplicitlySelected = true;
            resetOutputToFollowGame();
        } else {
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
        } else if (!oldValid || oldMode != 2 || outputWasCustom) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Automatic resolution is enabled. The current game canvas already matches the automatic choice, and the normal window will follow it. Display-mode or monitor changes are recalculated on the next full game start.",
                  L"Автоматическое разрешение включено. Текущий игровой кадр уже совпадает с выбором автоматики, а обычное окно будет следовать ему. Смена режима экрана или монитора пересчитывается при следующем полном запуске игры."),
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
        const bool outputWasCustom =
            g_requestedOutputW != 0 || g_requestedOutputH != 0;
        const bool changed =
            !oldValid || oldMode != 1 ||
            oldW != kHorplusSizes[value].w ||
            oldH != kHorplusSizes[value].h || outputWasCustom;
        const bool saved =
            horplus_set_requested(1, kHorplusSizes[value].w,
                                  kHorplusSizes[value].h) != 0;
        if (saved) {
            g_gameCanvasExplicitlySelected = true;
            resetOutputToFollowGame();
        } else
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
    } else if (id == kIdAlwaysActive) {
        g_alwaysActive = !g_alwaysActive;
        applyAlwaysActive(g_alwaysActive);
    } else if (id == kIdDragScroll) {
        g_dragScroll = !g_dragScroll; // live: the detour reads this flag (persist() saves it)
        if (!g_dragScroll)
            cancelDragScroll();
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
        g_autoConfirmUnitHire = !g_autoConfirmUnitHire; // live pass-through hook; MNS default on
    } else if (id == kIdFastAi && fastai_is_available()) {
        const bool requested = fastai_get_enabled() == 0;
        if (requested &&
            MessageBoxW(
                g_gameHwnd,
                L(L"Fast AI compresses the hidden local server's AI timer/message pump. It does not change the AI algorithm, but the original wrapper marks it experimental because tighter message ordering can expose native game races or crashes. It works only in the process that hosts the game and has no effect on joiners or human turns.\n\nEnable Fast AI?",
                  L"Fast AI уплотняет обработку таймеров и сообщений ИИ скрытого локального сервера. Алгоритм решений ИИ не меняется, но оригинальный враппер помечает режим экспериментальным: более плотный порядок сообщений может проявить штатные гонки или вылеты игры. Режим работает только в процессе-хосте и не влияет на джойнеров или ходы людей.\n\nВключить Fast AI?"),
                L"Fast AI", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        fastai_set_enabled(requested ? 1 : 0);
    } else if (id >= kIdWindowStretchBase &&
               id <= kIdWindowStretchBase + 10) {
        g_windowStretchPercent =
            static_cast<int>(id - kIdWindowStretchBase) * 10;
        horplus_set_window_stretch_percent(g_windowStretchPercent);
        DDSetWindowStretchPercent(g_windowStretchPercent);
        persist();
        refreshChecks();
        return;
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
        applyAnimFactor(0, false, g_battleBaseFactor);
        showAnimationSpeedStatus(true, 10);
    } else if (id >= kIdAnim1 && id <= kIdAnim6) {
        g_battleAnimEnabled = true;
        g_battleAnimSpeed = static_cast<int>(id - kIdAnim1) + 1;
        applyAnimSpeed(0, true, g_battleAnimSpeed);
        showAnimationSpeedStatus(true, g_battleBaseFactor);
    } else if (id == kIdAnimMapOff) {
        g_mapAnimEnabled = false;
        applyAnimFactor(1, false, g_mapBaseFactor);
        showAnimationSpeedStatus(false, 10);
    } else if (id >= kIdAnimMap1 && id <= kIdAnimMap6) {
        g_mapAnimEnabled = true;
        g_mapAnimSpeed = static_cast<int>(id - kIdAnimMap1) + 1;
        applyAnimSpeed(1, true, g_mapAnimSpeed);
        showAnimationSpeedStatus(false, g_mapBaseFactor);
    } else if (id == kIdAtkOff) {
        g_battleAttackEnabled = false;
        g_attackVisualActive = 0;
        g_attackExpiryTick = 0;
        g_attackWatchdogTick = 0;
        applyAnimFactor(0, g_battleAnimEnabled, g_battleBaseFactor); // hand the clock back to the exact base
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
        const int requestedRenderer = static_cast<int>(id - kIdRendOpenGL);
        if (requestedRenderer == 0 && g_shaderIdx >= 0) {
            // A portable backend can legitimately use Lanczos/Bicubic/Bilinear/None without the
            // GLSL files. Do not carry such a known-missing menu preset into OpenGL, where cnc-ddraw
            // would silently compile its fallback instead.
            scanShaderAssets();
            if (!g_shaderAvailable[g_shaderIdx]) {
                refreshChecks();
                showShaderAssetsWarningOnce(g_gameHwnd);
                return;
            }
        }
        g_rendererIdx = requestedRenderer;
        if (!writeDdrawStr("renderer", kRenderers[g_rendererIdx].value)) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save the renderer to the active ddraw.ini section. The renderer was not changed.",
                  L"Не удалось сохранить рендерер в активную секцию ddraw.ini. Рендерер не изменён."),
                L(L"Renderer", L"Рендерер"),
                MB_OK | MB_ICONERROR);
            readDdrawState();
            refreshChecks();
            return;
        }
        rendererChanged = true;
        reloadRenderer = true;
    } else if (id >= kIdShaderBase && id < kIdShaderBase + static_cast<UINT>(kShaderCount)) {
        const int index = static_cast<int>(id - kIdShaderBase);
        const int portableFilter = portableFilterForShader(index);
        const int activeRenderer = DDGetActiveRenderer();
        // Re-probe before any INI write: a file can disappear after WM_INITMENUPOPUP, and disabled
        // menu state alone does not protect against a synthetic WM_COMMAND.
        scanShaderAssets();
        if (!shaderSupportedByActiveRenderer(index, activeRenderer)) {
            refreshChecks();
            return;
        }
        if (!writeDdrawStr("shader", kShaders[index].value)) {
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not save the filter to ddraw.ini. The filter was not changed.",
                  L"Не удалось сохранить фильтр в ddraw.ini. Фильтр не изменён."),
                L(L"Filter", L"Фильтр"), MB_OK | MB_ICONERROR);
            return;
        }
        g_shaderIdx = index;
        if (portableFilter >= 0) {
            char value[8] = {};
            wsprintfA(value, "%d", portableFilter);
            if (writeDdrawStr("d3d9_filter", value))
                g_d3dFilter = portableFilter;
        }
        reloadRenderer = true;
    } else if (id == kIdMaintas) {
        // Fit: native game aspect, fractional scale and letter/pillar-boxing as needed.
        g_maintas = true;
        g_boxing = false;
        g_aspectRatio[0] = 0;
        writeDdrawStr("aspect_ratio", "");
        writeDdrawBool("maintas", g_maintas);
        writeDdrawBool("boxing", g_boxing);
        reloadRenderer = true;
    } else if (id == kIdVsync) {
        g_vsync = !g_vsync;
        writeDdrawBool("vsync", g_vsync);
        reloadRenderer = true;
    } else if (id == kIdBoxing) {
        // Integer fit: boxing owns the viewport and therefore maintas must not pretend to be active.
        g_maintas = false;
        g_boxing = true;
        g_aspectRatio[0] = 0;
        writeDdrawStr("aspect_ratio", "");
        writeDdrawBool("maintas", g_maintas);
        writeDdrawBool("boxing", g_boxing);
        reloadRenderer = true;
    } else if ((id >= kIdTicks0 && id <= kIdTicks100) || id == kIdTicks180) {
        const int oldTicks = readDdrawInt("maxgameticks", 180);
        const int oldTicksIdx = g_ticksIdx;
        int selected = -1;
        for (int i = 0; i < kTicksCount; ++i) {
            if (kTicksCommandIds[i] == id) {
                selected = i;
                break;
            }
        }
        if (selected < 0)
            return;
        char b[8];
        wsprintfA(b, "%d", kTicksValues[selected]);
        if (!writeDdrawStr("maxgameticks", b) ||
            !DDSetMaxGameTicksLive(kTicksValues[selected])) {
            char old[8];
            wsprintfA(old, "%d", oldTicks);
            writeDdrawStr("maxgameticks", old);
            g_ticksIdx = oldTicksIdx;
            refreshChecks();
            MessageBoxW(
                g_gameHwnd,
                L(L"Could not apply the game speed cap live. The previous limiter remains active.",
                  L"Не удалось применить кап скорости игры на лету. Прежний лимитер остался активен."),
                L(L"Game speed cap", L"Кап скорости игры"),
                MB_OK | MB_ICONERROR);
            return;
        }
        g_ticksIdx = selected;
        refreshChecks();
        return;
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
                    ? L(L"Could not save width and height to the active ddraw.ini section. The window/output size was not changed.",
                        L"Не удалось сохранить width и height в активную секцию ddraw.ini. Размер окна/вывода не изменён.")
                    : L(L"Saving width and height failed, and the previous pair could not be fully restored. Check ddraw.ini before restarting the game.",
                        L"Сохранение width и height завершилось ошибкой, и прежнюю пару не удалось полностью восстановить. Проверьте ddraw.ini перед перезапуском игры."),
                L(L"Window/output size", L"Размер окна/вывода"),
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
    } else if (g_ver == VerEditor && id >= kIdResBase &&
               id < kIdResBase + static_cast<UINT>(kResCount)) {
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
        reloadRenderer = true;
    } else if (id >= kIdFpsBase && id < kIdFpsBase + static_cast<UINT>(kFpsCount)) {
        g_fpsIdx = static_cast<int>(id - kIdFpsBase);
        char b[12];
        wsprintfA(b, "%d", kFpsValues[g_fpsIdx]);
        writeDdrawStr("maxfps", b);
        reloadRenderer = true;
    } else if (id >= kIdModeWindowed && id <= kIdModeExclusive) {
        g_modeIdx = static_cast<int>(id - kIdModeWindowed);
        writeDdrawStr("windowed", kModes[g_modeIdx].windowed);
        writeDdrawStr("fullscreen", kModes[g_modeIdx].fullscreen);
        if (g_modeIdx == 1)
            writeDdrawBool("toggle_borderless", true);
        else if (g_modeIdx == 2)
            writeDdrawBool("toggle_borderless", false);
        displayModeChanged = true;
        reloadRenderer = true;
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
    if (reloadRenderer) {
        // We are the renderer: re-apply live without losing a manual resize or a hotkey-selected
        // mode. Explicit Output size and Display mode choices each replace only their own state.
        // A display-mode reload must see its final menu state on its first dd_SetDisplayMode pass;
        // otherwise syncChrome has to tear the freshly created renderer down a second time.
        if (displayModeChanged && g_gameHwnd)
            stageChromeForTargetMode(g_gameHwnd, g_modeIdx);
        const int liveResult =
            applyDdrawLive(outputSizeChanged, displayModeChanged, rendererChanged);
        if (g_gameHwnd)
            syncChrome(g_gameHwnd);
        if (rendererChanged) {
            g_pendingRendererVerifyTick = 0;
            // ogl_render_main deliberately sleeps and runs its texture/shader self-test after
            // DDReloadConfig returns. Verify the actual backend later instead of reporting the
            // context creation alone as final success.
            if (liveResult && g_rendererIdx == 0) {
                g_pendingRendererVerifyTick = GetTickCount() + 3500;
                if (g_gameHwnd)
                    SetTimer(g_gameHwnd, kPressTimerId, 32, nullptr);
            }
        }
        if (rendererChanged && !liveResult) {
            const int active = DDGetActiveRenderer();
            const char* activeName =
                active == 0 ? "OpenGL" : active == 1 ? "GDI"
                : active == 2 ? "Direct3D 9" : active == 3 ? "null"
                                                           : "unknown";
            mlog("[menu] live renderer switch failed: code=%d active=%s requested=%s",
                 DDGetRendererSwitchError(), activeName,
                 kRenderers[g_rendererIdx].value);
            MessageBoxW(
                g_gameHwnd,
                L(L"The selected renderer could not be activated live. A safe working backend is active; the selection is saved for the next game launch. Open Renderer again to see the actual active backend.",
                  L"Выбранный рендерер не удалось включить без перезапуска. Сейчас работает безопасный резервный backend; выбор сохранён для следующего запуска. Фактически активный backend показан в меню «Рендерер»."),
                L(L"Renderer", L"Рендерер"),
                MB_OK | MB_ICONWARNING);
        }
    } else {
        persist(); // live mod toggles -> C4menu.ini
    }
}

// The entire renderer viewport, including the decorative frame, belongs to D2's native software
// cursor. cursorcapture replays the exact dynamic CursorHandle fragments which the post-compositor
// would otherwise cover outside the fixed screen. Only real Windows menu/non-client UI uses an
// HCURSOR. Never switch visibility with ShowCursor's per-thread counter.
void setWrapperCursorVisible(bool visible, bool systemArrow = false)
{
    if (InterlockedExchangeAdd(&g_cursorCaptureInstalled, 0) == 0)
        return;
    const bool windowsArrow = visible && systemArrow;
    const LONG nextMode = windowsArrow ? 2 : 0;

    // Suppress only CCursorImpl's DrawTexture operations. CCursorImpl itself always runs, including
    // its current-handle selection and SmartPtr cleanup. Entering HTCLIENT restores those exact
    // native operations and hides the hardware layer with SetCursor(NULL).
    cursorcapture_set_suppressed(windowsArrow ? 1 : 0);
    DDSetPhysicalCursor(windowsArrow ? LoadCursorA(nullptr, IDC_ARROW) : nullptr);
    const LONG previousMode = InterlockedExchange(&g_ncCursorMode, nextMode);
    if (nextMode != previousMode)
        mlog("[cursor] owner -> %s",
             windowsArrow ? "Windows arrow" : "dynamic D2 software cursor");
}

void suppressGameCursorWithoutOwningPointer()
{
    if (InterlockedExchangeAdd(&g_cursorCaptureInstalled, 0) == 0)
        return;
    cursorcapture_set_suppressed(1);
    cursorcapture_clear();
    const LONG previousMode = InterlockedExchange(&g_ncCursorMode, 2);
    if (previousMode != 2)
        mlog("[cursor] owner -> another window (D2 cursor suppressed)");
}

enum class PhysicalPointerRegion
{
    External,
    NativeViewport,
    WindowsUi,
};

PhysicalPointerRegion classifyPhysicalPointer(HWND hwnd)
{
    static int lastProbe = -1;
    auto traceProbe = [&](int probe, const char* detail) {
        if (probe != lastProbe) {
            mlog("[cursor] area probe=%s", detail);
            lastProbe = probe;
        }
    };

    POINT screen = {};
    if (!hwnd || !DDGetPhysicalCursorPos(&screen)) {
        traceProbe(0, "external (window/pointer unavailable)");
        return PhysicalPointerRegion::External;
    }

    HWND hit = DDPhysicalWindowFromPoint(screen);
    if (!hit) {
        traceProbe(0, "external (no window at pointer)");
        return PhysicalPointerRegion::External;
    }

    if (hit != hwnd) {
        // The timer overlay is deliberately transparent/no-activate and remains part of the game
        // viewport for cursor purposes. Every other child/owned top-level window is real Windows
        // UI (#32770 dialog, popup, tooltip, control) and must never receive a hidden HCURSOR.
        char className[64] = {};
        const bool transparentOverlay =
            GetClassNameA(hit, className, static_cast<int>(sizeof(className))) > 0 &&
            lstrcmpA(className, "C4dllROverlay") == 0 &&
            GetAncestor(hit, GA_ROOTOWNER) == hwnd;
        if (!transparentOverlay) {
            if (IsChild(hwnd, hit) != FALSE || GetAncestor(hit, GA_ROOTOWNER) == hwnd) {
                traceProbe(2, "Windows UI (owned popup/child)");
                return PhysicalPointerRegion::WindowsUi;
            }
            traceProbe(0, "external (another window)");
            return PhysicalPointerRegion::External;
        }
    }

    POINT point = screen;
    RECT client = {};
    if (!DDPhysicalScreenToClient(hwnd, &point) ||
        !DDGetPhysicalClientRect(hwnd, &client) || !PtInRect(&client, point) ||
        g_menuLoopActive) {
        traceProbe(2, "Windows UI (menu/caption/non-client)");
        return PhysicalPointerRegion::WindowsUi;
    }

    int gameWidth = 0;
    int gameHeight = 0;
    int viewportX = 0;
    int viewportY = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    if (!DDGetScaleMetrics(&gameWidth, &gameHeight, nullptr, nullptr,
                           &viewportX, &viewportY,
                           &viewportWidth, &viewportHeight) ||
        gameWidth <= 0 || gameHeight <= 0 ||
        viewportWidth <= 0 || viewportHeight <= 0) {
        traceProbe(2, "Windows UI (renderer metrics unavailable)");
        return PhysicalPointerRegion::WindowsUi;
    }

    if (point.x < viewportX || point.y < viewportY ||
        point.x >= viewportX + viewportWidth ||
        point.y >= viewportY + viewportHeight) {
        traceProbe(3, "Windows arrow (outside game viewport)");
        return PhysicalPointerRegion::WindowsUi;
    }
    traceProbe(1, "dynamic D2 cursor across the whole game viewport");
    return PhysicalPointerRegion::NativeViewport;
}

void applyPhysicalPointerRegion(PhysicalPointerRegion region)
{
    if (region == PhysicalPointerRegion::External)
        suppressGameCursorWithoutOwningPointer();
    else if (region == PhysicalPointerRegion::WindowsUi)
        setWrapperCursorVisible(true, true);
    else
        setWrapperCursorVisible(false, false);
}

void reevaluateWrapperCursor(HWND hwnd)
{
    if (!hwnd || InterlockedExchangeAdd(&g_cursorCaptureInstalled, 0) == 0)
        return;
    applyPhysicalPointerRegion(classifyPhysicalPointer(hwnd));
}

void handleCursorLifecycle(HWND hwnd, UINT msg, WPARAM wParam)
{
    if (InterlockedExchangeAdd(&g_cursorCaptureInstalled, 0) == 0)
        return;

    switch (msg) {
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_MOUSELEAVE:
        // The physical hit-test is authoritative: it preserves D2 hover over an inactive viewport,
        // selects Windows UI for an owned dialog, and never changes another process's HCURSOR.
        reevaluateWrapperCursor(hwnd);
        break;
    case WM_DESTROY:
    case WM_NCDESTROY:
    case WM_SHOWWINDOW:
        if (msg != WM_SHOWWINDOW || wParam == FALSE) {
            suppressGameCursorWithoutOwningPointer();
            if (msg == WM_NCDESTROY && g_gameHwnd == hwnd)
                g_gameHwnd = nullptr;
        }
        break;
    case WM_QUERYENDSESSION:
    case WM_ENDSESSION:
        suppressGameCursorWithoutOwningPointer();
        break;
    default:
        break;
    }
}

bool handleDecorativeCursor(HWND hwnd, UINT msg, LPARAM lParam)
{
    if (InterlockedExchangeAdd(&g_cursorCaptureInstalled, 0) == 0)
        return false;

    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        applyPhysicalPointerRegion(classifyPhysicalPointer(hwnd));
        // Own every HTCLIENT decision, exactly like DisciplesGL. Forwarding the message in active
        // content would let devmode's native SetCursor call restore a second hardware cursor after
        // we selected NULL for the game's software-sword region.
        return true;
    }
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking = {};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd;
        TrackMouseEvent(&tracking);
        const PhysicalPointerRegion region = classifyPhysicalPointer(hwnd);
        applyPhysicalPointerRegion(region);
        // A native popup menu consumes its own hover. Game content, including an inactive window,
        // must receive WM_MOUSEMOVE so D2 updates its software cursor and button highlight. Exact
        // edge-scroll suppression is independent and prevents background map movement.
        if (region == PhysicalPointerRegion::WindowsUi && GetCapture() != hwnd)
            return true;
    }
    return false;
}

extern "C" void timerhost_pump(void); // perform any queued on-elapse press on the game thread
extern "C" int timerhost_filter_input(UINT msg, WPARAM wParam, LPARAM lParam);
extern "C" void timerhost_install(void); // exact EXE timer hooks; deferred past loader lock
extern "C" int timerhost_battle_kind(void); // dialog-lifecycle signal, available before first action

void showAnimationSpeedStatus(bool battleVisible, int factor)
{
    if (InterlockedExchangeAdd(&g_statusTextInstallState, 0) != 2)
        return;
    void* const strat = reinterpret_cast<void*>(
        InterlockedExchangeAdd(&g_statusStratInterf, 0));
    if (!strat)
        return;

    char text[64] = {};
    sprintf_s(text, sizeof(text), "%s animation: %d.%dx",
              battleVisible ? "Battle" : "Map", factor / 10, factor % 10);
    __try {
        reinterpret_cast<void(__thiscall*)(void*, char*)>(0x4900C8)(strat, text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedCompareExchange(
            &g_statusStratInterf, 0,
            static_cast<LONG>(reinterpret_cast<uintptr_t>(strat)));
        mlog("[menu] native status-text object became unavailable; notice skipped");
    }
}

// Keep live animation-speed adjustment available without stealing ordinary '+'/'-' input from the
// game (chat, numeric fields and native hotkeys all use the same WM_KEYDOWN stream). Ctrl+Plus and
// Ctrl+Minus adjust the visible map/battle context; unmodified symbols always reach Disciples II.
bool handleAnimSpeedHotkey(WPARAM key)
{
    if (g_ver != VerRussobit || GetKeyState(VK_MENU) < 0 || GetKeyState(VK_CONTROL) >= 0)
        return false;
    if (key != VK_OEM_PLUS && key != VK_OEM_MINUS &&
        key != VK_ADD && key != VK_SUBTRACT)
        return false;

    // g_inBattle is action-driven and can still be false on a joiner's first visible battle. The
    // timer host publishes DLG_BATTLE construction earlier, so use both signals for the UI context.
    const bool battleVisible = g_inBattle || timerhost_battle_kind() != 0;
    bool* enabled = battleVisible ? &g_battleAnimEnabled : &g_mapAnimEnabled;
    int* speed = battleVisible ? &g_battleAnimSpeed : &g_mapAnimSpeed;
    int* exactFactor = battleVisible ? &g_battleBaseFactor : &g_mapBaseFactor;
    const int oldFactor = *enabled ? *exactFactor : 10;
    int factor = oldFactor;
    if (key == VK_OEM_PLUS || key == VK_ADD) {
        if (factor < 150)
            ++factor;
    } else if (factor > 10) {
        --factor;
    }
    if (factor == oldFactor)
        return false; // do not steal a chord at the x1.0/x15.0 boundaries

    *enabled = factor > 10;
    *exactFactor = factor;
    const int preset = animPresetForFactor(factor);
    if (preset)
        *speed = preset; // retain the nearest exact coarse preset for older C4dll-R builds
    if (battleVisible) {
        applyAnimFactor(0, *enabled, factor);
        updateBattleBurst();
    } else {
        applyAnimFactor(1, *enabled, factor);
    }
    // Two multiplayer processes share one INI but keep independent live state. Persist only the
    // pair changed by this chord so one instance cannot overwrite the other's unrelated settings.
    const char* f = iniFile();
    char value[8] = {};
    WritePrivateProfileStringA("menu",
        battleVisible ? "battleAnimEnabled" : "mapAnimEnabled", *enabled ? "1" : "0", f);
    wsprintfA(value, "%d", *speed);
    WritePrivateProfileStringA("menu",
        battleVisible ? "battleAnimSpeed" : "mapAnimSpeed", value, f);
    wsprintfA(value, "%d", factor);
    WritePrivateProfileStringA("menu",
        battleVisible ? "battleAnimFactor" : "mapAnimFactor", value, f);
    mlog("[menu] Ctrl+%s -> %s animation %.1fx (factor=%d)",
         (key == VK_OEM_PLUS || key == VK_ADD) ? "+" : "-",
         battleVisible ? "battle" : "map", factor / 10.0, factor);
    showAnimationSpeedStatus(battleVisible, factor);
    refreshChecks();
    return true;
}

LRESULT dispatchGameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Russobit's native WM_MOUSEMOVE helper rejects the event when its WM_ACTIVATEAPP byte is 0.
    // Background hover is nevertheless a presentation-only operation we deliberately support: it
    // must move the D2 cursor and highlight native buttons without activating the window or making
    // gameplay input active. Raise that one byte only for the synchronous native hover dispatch and
    // restore the exact previous value even if a third-party game hook faults or unwinds.
    if (msg != WM_MOUSEMOVE ||
        classifyPhysicalPointer(hwnd) != PhysicalPointerRegion::NativeViewport) {
        eventtrace_message(C4TRACE_NATIVE_WND_ENTER, hwnd, msg, wParam, lParam);
        const LRESULT result = g_origWndProc(hwnd, msg, wParam, lParam);
        eventtrace_message(C4TRACE_NATIVE_WND_RETURN, hwnd, msg, wParam, lParam);
        // A no-move press is deliberately forwarded so the native iso handler can replay it as a
        // click. If the release landed on another panel/outside the iso view, that handler is not
        // called; repair the still-live wrapper capture after native dispatch instead of carrying
        // it until the view is recreated.
        if (msg == WM_LBUTTONUP && g_dragScrollActive)
            cancelDragScroll();
        return result;
    }

    volatile BYTE* activeFlag = nullptr;
    BYTE previous = 0;
    bool changed = false;
    __try {
        // Version detection and the WndProc detour already gate this to the exact Russobit layout,
        // but the window can be torn down re-entrantly. Keep the state probe independently
        // fail-closed; a stale GWLP_USERDATA must never turn a hover fix into a process crash.
        __try {
            auto* windowObject = reinterpret_cast<BYTE*>(
                GetWindowLongPtrA(hwnd, GWLP_USERDATA));
            if (isUserPtr(windowObject)) {
                auto* state = *reinterpret_cast<BYTE**>(windowObject + 4);
                if (isUserPtr(state) && state[0x19] == 0) {
                    activeFlag = state + 0x18;
                    previous = *activeFlag;
                    if (previous == 0) {
                        *activeFlag = 1;
                        changed = true;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            activeFlag = nullptr;
            changed = false;
        }

        if (changed) {
            static LONG announced = 0;
            if (InterlockedCompareExchange(&announced, 1, 0) == 0)
                mlog("[cursor] native inactive-hover focus gate bypassed per WM_MOUSEMOVE");
        }

        return g_origWndProc(hwnd, msg, wParam, lParam);
    } __finally {
        if (changed && activeFlag)
            *activeFlag = previous;
    }
}

LRESULT CALLBACK wndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    eventtrace_install();
    messagebatch_install(hwnd, iniFile());
    messagebatch_window_event(hwnd, msg, wParam);
    eventtrace_message(C4TRACE_FEATURE_WND, hwnd, msg, wParam, lParam);
    if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_NCDESTROY ||
        msg == WM_QUERYENDSESSION || msg == WM_ENDSESSION ||
        (msg == WM_SYSCOMMAND && (wParam & 0xFFF0u) == SC_CLOSE) ||
        (msg == WM_SHOWWINDOW && wParam == FALSE)) {
        mlog("[window] lifecycle msg=%04X hwnd=%p wParam=%p lParam=%p caller=%p",
             msg, hwnd, reinterpret_cast<void*>(wParam), reinterpret_cast<void*>(lParam),
             _ReturnAddress());
    }
    // The first GUI dispatch is after every imported DLL has completed DllMain. Install shared EXE
    // vtable hooks here and tail-chain the target present at runtime, independent of module order.
    ensureBattleDiscriminator();
    ensureBattleChooseActionHook();
    timerhost_install();
    if (msg == WM_NCDESTROY) {
        if (g_gameHwnd == hwnd) {
            releasePressTimer(hwnd);
            g_gameHwnd = nullptr;
        }
    } else {
        // renderer_message runs before this detour and may already have assigned g_gameHwnd. Timer
        // ownership therefore has its own HWND state and is retried until SetTimer succeeds.
        if (!g_gameHwnd)
            g_gameHwnd = hwnd; // remember the canonical game window
        if (g_gameHwnd == hwnd)
            ensurePressTimer(g_gameHwnd);
    }
    if (pluginhost_battle_state_message(msg))
        return 0;
    if (timerhost_auto_battle_message(msg))
        return 0;
    // Our private 32ms timer (idle WM_TIMER): refresh the cached scenario day on the GAME thread, then
    // run the queued auto-end-turn/retreat press. Gate on OUR id and CONSUME it (return 0) so the game's
    // own timers don't drive the pump and our id never leaks into the game's WndProc timer dispatch.
    if (msg == WM_TIMER && wParam == kPressTimerId) {
        // WM_MOUSELEAVE is not guaranteed when another top-level game window appears under a
        // stationary pointer. Reconcile visual ownership on the existing idle timer: an inactive
        // hovered client keeps receiving native D2 hover, every other process clears its stale
        // software cursor without touching the real foreground application's HCURSOR.
        reevaluateWrapperCursor(hwnd);
        featuremenu_refresh_day();
        updateBattleBurst(); // idle/attack split: drive g_battleFactor from exact visual events
        dvoPoll();           // auto-close a voiced event popup once its VO has finished
        verifyPendingRenderer();
        timerhost_pump();
        fastai_pump(); // discovery only; accelerated work runs on host ThreadWindowClass itself
        return 0;
    }

    // cnc-ddraw's renderer bridge also observes these messages because some MNS paths consume an
    // activation notification before the native game WndProc sees it. Calling this here as well is
    // intentional and idempotent; it covers direct/native dispatch and older renderer builds.
    handleCursorLifecycle(hwnd, msg, wParam);

    // Do this before the game handles WM_SETCURSOR: its software cursor is clipped away by the
    // presentation-only decorative compositor outside the centered fixed screen.
    // Use the physical pointer for cursor ownership. cnc-ddraw has already clamped the lParam it
    // forwards to D2, so mapped coordinates cannot distinguish renderer letter/pillar bars.
    if (handleDecorativeCursor(hwnd, msg, lParam))
        return TRUE;

    if (msg == WM_KEYDOWN && handleAnimSpeedHotkey(wParam))
        return 0;

    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        pluginhost_key(msg, wParam, lParam))
        return 0;

    // Legacy C4dll-R used F4 for a one-key normal-window/fullscreen toggle. Keep it wrapper-owned:
    // old ddraw.ini files have no keytogglefullscreen2, and Alt+F4 remains a WM_SYSKEYDOWN.
    if (msg == WM_KEYDOWN && wParam == VK_F4 && !(lParam & 0x40000000)) {
        toggleWindowModeWithChrome(hwnd);
        syncChrome(hwnd);
        return 0;
    }

    // Retained as a safety seam for timer-host compatibility, but deliberately fail-open: forced
    // Auto Battle must never own mouse, keyboard or WM_CHAR input. Its native toggle command is
    // generation-scoped and runs independently on the GUI thread.
    if (timerhost_filter_input(msg, wParam, lParam))
        return 0;

    // Win32 capture can be broken by a focus change, modal loop or another window taking capture.
    // Clear our route before any plugin/game handler sees the lifecycle event; otherwise a missing
    // button-up can leave every later move consumed until the strategic view is recreated.
    if (g_dragScrollActive &&
        (msg == WM_CANCELMODE || msg == WM_KILLFOCUS || msg == WM_NCDESTROY ||
         (msg == WM_ACTIVATEAPP && !wParam) ||
         (msg == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lParam) != hwnd))) {
        cancelDragScroll();
    }

    // The overlay is deliberately WS_EX_TRANSPARENT. Give native plugins first refusal only for
    // mouse/capture messages; timer.c4p uses this for its explicit Ctrl+Alt drag gesture. This must
    // precede map drag so a clock grab never reaches the strategic-map handler.
    if ((msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE) &&
        pluginhost_mouse(msg, wParam)) {
        return 0;
    }

    // SetCapture routes the release to the main HWND even when the pointer has crossed from the
    // iso field onto a unit panel or outside the window. In that case the game's interface router
    // may never call isoMouseHook with WM_LBUTTONUP. A real drag consumed its original DOWN, so it
    // is both necessary and safe to finish it here without forwarding an unmatched native UP.
    if (g_dragScrollActive && g_dragMoved && msg == WM_LBUTTONUP) {
        cancelDragScroll();
        return 0;
    }

    // fake_WndProc has already transformed lParam into game coordinates before it calls the game's
    // WndProc. A second cnc-ddraw transform here shifts the drag anchor under scaling or letterboxing.
    if (g_dragScrollActive && msg == WM_MOUSEMOVE) {
        if ((wParam & MK_LBUTTON) == 0) {
            // Self-heal exactly when the physical gesture has ended even if its WM_LBUTTONUP was
            // lost during a modal/capture transition. Do not swallow this ordinary hover sample.
            cancelDragScroll();
        } else {
            dragScrollWndMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                              static_cast<int>(static_cast<short>(HIWORD(lParam))));
            return 0;
        }
    }
    // Keep the OS pointer visible over the caption + menu bar (non-client), invisible in the client.
    switch (msg) {
    case WM_NCMOUSEMOVE:
        setWrapperCursorVisible(true, true);
        break;
    case WM_ENTERMENULOOP:
        g_menuLoopActive = true;
        setWrapperCursorVisible(true, true);
        pluginhost_menu_loop(1);
        break;
    case WM_EXITMENULOOP:
        g_menuLoopActive = false;
        pluginhost_menu_loop(0);
        reevaluateWrapperCursor(hwnd);
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
            // Re-evaluate the single cursor owner immediately even when the pointer is stationary.
            reevaluateWrapperCursor(hwnd);
            return 0;
        }
        // Plugin config-menu commands (0xB000+ block): route to the owning plugin's c4p_command.
        if (id >= 0xB000 && id < 0xC000 && pluginhost_command(id)) {
            reevaluateWrapperCursor(hwnd);
            return 0;
        }
    } else if (msg == WM_INITMENUPOPUP) {
        if (reinterpret_cast<HMENU>(wParam) == g_shaderMenu ||
            reinterpret_cast<HMENU>(wParam) == g_rendMenu)
            scanShaderAssets();
        pluginhost_refresh_menus();
        refreshChecks();
    }
    return dispatchGameWndProc(hwnd, msg, wParam, lParam);
}

void buildMenu()
{
    if (g_bar)
        return;
    // Plugins load on the host worker thread (not loader-lock-serialized DllMain), so wait for that
    // before reading the list. Loading takes milliseconds vs seconds to the game window, so this
    // effectively never blocks; the timeout just means "build without plugins" if loading hangs.
    pluginhost_wait_ready(5000);
    // This worker runs after DLL_PROCESS_ATTACH, so filesystem probing is outside loader lock. Keep
    // the scan beside menu construction; the GUI thread only consumes the resulting status.
    scanShaderAssets();
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
                kIdClouds, L(L"(MNS/SMNS) Show map clouds (after restart)",
                             L"(MNS/SMNS) Показывать облака на карте (после перезапуска)"));
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
                L(L"Ctrl +/- adjusts the current battle value by 0.1x.",
                  L"Ctrl +/- меняет текущую скорость боя с шагом 0,1x."));
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
                L(L"Ctrl +/- adjusts the current map value by 0.1x.",
                  L"Ctrl +/- меняет текущую скорость карты с шагом 0,1x."));
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

    AppendMenuW(g_gameMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_gameMenu, MF_STRING, kIdResetWrapper,
                L(L"Reset wrapper settings...", L"Сбросить настройки враппера..."));

    // ===== "Video" - presentation choices are live; logical game resolution is restart-only =====
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
                    L(L"Automatic resolution...",
                      L"Автоматическое разрешение..."));
        AppendMenuW(g_displaySizeMenu, MF_SEPARATOR, 0, nullptr);
        // One list, one concept: these all change the logical game canvas. The star marks the
        // three unmodified Disciples II DisplaySize modes; the other reviewed entries use Hor+.
        AppendMenuW(g_displaySizeMenu, nativeFlags, kIdDisplaySize0,
                    g_ru ? kDisplaySizeLabelsRu[0] : kDisplaySizeLabelsEn[0]);
        AppendMenuW(g_displaySizeMenu, nativeFlags, kIdDisplaySize1,
                    g_ru ? kDisplaySizeLabelsRu[1] : kDisplaySizeLabelsEn[1]);
        for (int i = 0; i <= 2; ++i)
            AppendMenuW(g_displaySizeMenu, wideFlags,
                        kIdHorplusBase + static_cast<UINT>(i),
                        g_ru ? kHorplusSizes[i].ru : kHorplusSizes[i].en);
        AppendMenuW(g_displaySizeMenu, nativeFlags, kIdDisplaySize2,
                    g_ru ? kDisplaySizeLabelsRu[2] : kDisplaySizeLabelsEn[2]);
        for (int i = 3; i < kHorplusSizeCount; ++i)
            AppendMenuW(g_displaySizeMenu, wideFlags,
                        kIdHorplusBase + static_cast<UINT>(i),
                        g_ru ? kHorplusSizes[i].ru : kHorplusSizes[i].en);
        AppendMenuW(g_displaySizeMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_displaySizeMenu, MF_STRING | MF_GRAYED, kIdDisplaySizeState, L"...");
        AppendMenuW(g_displaySizeMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_displaySizeMenu, MF_STRING, kIdOutputSizeCustom,
                    L(L"Window/output size (live)...",
                      L"Размер окна/вывода (сразу)..."));
    } else {
        // The editor has no selectable game canvas; keep only its direct window-size control.
        AppendMenuW(g_displaySizeMenu, MF_STRING, kIdOutputSizeCustom,
                    L(L"Window size...", L"Размер окна..."));
    }
    g_resolutionMenuPosition = GetMenuItemCount(g_videoMenu);
    AppendMenuW(g_videoMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(g_displaySizeMenu),
                g_ver == VerEditor
                    ? L(L"Resolution / window size...",
                        L"Разрешение / размер окна...")
                    : L(L"Resolution...", L"Разрешение..."));
    // Keep one compact output dialog rather than a second preset list. It scales only the finished
    // canvas, applies live, and every explicit game-resolution choice can still re-link it to Auto.
    g_resMenu = nullptr;
    g_scaleMenu = CreatePopupMenu();
    AppendMenuW(g_scaleMenu, MF_STRING, kIdMaintas,
                L(L"Fit (live) - preserve the selected game aspect",
                  L"Вписать (сразу) - сохранить выбранные пропорции игры"));
    AppendMenuW(g_scaleMenu, MF_STRING, kIdBoxing,
                L(L"Integer pixel blocks (live) - 2x means 2x2 = 4 output pixels",
                  L"Целые блоки пикселей (сразу) - 2x значит 2x2 = 4 пикселя вывода"));
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleCustom, L"...");
    AppendMenuW(g_scaleMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleGeometry, L"...");
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleResult, L"...");
    AppendMenuW(g_scaleMenu, MF_STRING | MF_GRAYED, kIdScaleFilterInfo, L"...");
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_scaleMenu),
                L(L"Output scaling (live)", L"Масштаб вывода (сразу)"));

    // DisciplesGL 1.90's "Stretch windows" control preserves geometry: a centred part of the
    // finished game canvas is enlarged to the viewport. The rarely useful independent-X/Y output
    // stretch remains compatible with hand-edited ddraw.ini files, but is intentionally not in GUI.
    if (horplus_is_available()) {
        g_windowStretchMenu = CreatePopupMenu();
        AppendMenuW(g_windowStretchMenu, MF_STRING, kIdWindowStretchBase,
                    L(L"100% - no enlargement", L"100% - без увеличения"));
        AppendMenuW(g_windowStretchMenu, MF_SEPARATOR, 0, nullptr);
        for (int i = 1; i <= 10; ++i) {
            wchar_t label[64] = {};
            if (i == 10)
                lstrcpynW(label, L(L"Fill height (default)",
                                    L"На всю высоту (по умолчанию)"),
                          static_cast<int>(sizeof(label) / sizeof(label[0])));
            else
                lstrcpynW(label, L"...",
                          static_cast<int>(sizeof(label) / sizeof(label[0])));
            AppendMenuW(g_windowStretchMenu, MF_STRING,
                        kIdWindowStretchBase + static_cast<UINT>(i), label);
        }
        AppendMenuW(g_windowStretchMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_windowStretchMenu, MF_STRING | MF_GRAYED,
                    kIdWindowStretchInfo, L"...");
        AppendMenuW(g_windowStretchMenu, MF_STRING | MF_GRAYED,
                    kIdWindowStretchHelp, L"...");
        g_windowStretchMenuPosition = GetMenuItemCount(g_videoMenu);
        AppendMenuW(g_videoMenu, MF_POPUP,
                    reinterpret_cast<UINT_PTR>(g_windowStretchMenu),
                    L(L"Fixed window size (live)",
                      L"Размер центрального окна (сразу)"));
    }

    g_shaderMenu = CreatePopupMenu();
    for (int i = 0; i < kShaderCount; ++i) {
        const UINT flags = MF_STRING |
                           (g_shaderAvailable[i] ? MF_ENABLED : MF_GRAYED);
        AppendMenuW(g_shaderMenu, flags, kIdShaderBase + i,
                    g_ru ? kShaders[i].ru : kShaders[i].en);
    }
    AppendMenuW(g_shaderMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_shaderMenu, MF_STRING | MF_GRAYED, kIdShaderStatus, L"...");
    AppendMenuW(g_shaderMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Lanczos/Bicubic/Bilinear/None also work on D3D9; GDI uses smooth/nearest fallback",
                  L"Lanczos/Bicubic/Bilinear/Нет работают и в D3D9; GDI использует сглаживание/nearest"));
    refreshShaderAssetStatus();
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_shaderMenu),
                L(L"Filter", L"Фильтр"));
    g_rendMenu = CreatePopupMenu();
    for (int i = 0; i < kRendererCount; ++i)
        AppendMenuW(g_rendMenu, MF_STRING, kIdRendOpenGL + i,
                    g_ru ? kRenderers[i].ru : kRenderers[i].en);
    AppendMenuW(g_rendMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_rendMenu, MF_STRING | MF_GRAYED, kIdRendererActive,
                L(L"Active now: not initialized",
                  L"Сейчас активен: не инициализирован"));
    AppendMenuW(g_rendMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Switches live; Filter is a separate setting",
                  L"Меняется сразу; фильтр настраивается отдельно"));
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_rendMenu),
                L(L"Renderer", L"Рендерер"));
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
                L(L"Frame cap (live)", L"Кап FPS (сразу)"));
    g_ticksMenu = CreatePopupMenu();
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks0,
                L(L"Uncapped (high CPU)", L"Без капа (грузит CPU)"));
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks30,
                L(L"30 (cool CPU, sluggish)", L"30 (холодный CPU, задумчиво)"));
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks60, L"60");
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks100,
                L"100");
    AppendMenuW(g_ticksMenu, MF_STRING, kIdTicks180,
                L(L"180 (default)", L"180 (по умолчанию)"));
    AppendMenuW(g_ticksMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_ticksMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Game core speed: low values make the game think before every action",
                  L"Скорость ядра игры: низкие значения = пауза перед каждым действием"));
    AppendMenuW(g_perfMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_ticksMenu),
                L(L"Game speed cap (live)", L"Кап скорости игры (на лету)"));
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
    AppendMenuW(g_perfMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_perfMenu, MF_STRING, kIdFastAi,
                L(L"Fast AI (experimental, host only)",
                  L"Fast AI (эксперимент, только хост)"));
    AppendMenuW(g_perfMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Accelerates AI message processing; off by default because it may expose game crashes.",
                  L"Ускоряет обработку сообщений ИИ; по умолчанию выкл., так как может проявить вылеты игры."));
    AppendMenuW(g_perfMenu, MF_SEPARATOR, 0, nullptr);
    g_technicalMenu = CreatePopupMenu();
    AppendMenuW(g_technicalMenu, MF_STRING, kIdNetTrace,
                L(L"Network/timing diagnostics (restart)...",
                  L"Диагностика сети и задержек (рестарт)..."));
    AppendMenuW(g_technicalMenu, MF_STRING | MF_GRAYED, kIdNetTraceInfo,
                L(L"Changing this option closes the client. Logs are limited; see NETWORK_TRACE.md.",
                  L"Смена настройки закрывает клиент. Логи ограничены; см. NETWORK_TRACE.md."));
    AppendMenuW(g_perfMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_technicalMenu),
                L(L"Technical settings", L"Технические настройки"));

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
    int after = before;
    LRESULT result = 0;

    // In exclusive mode F4 can reach the game WndProc yet still toggle too late: chrome briefly
    // follows the attempted transition while the fullscreen-sized renderer wins the nested relayout.
    // Handle it at cnc-ddraw's WH_KEYBOARD point, just like Alt+Enter, and consume the initial press
    // so exactly one normalized transition owns both mode and normal-window geometry.
    if (DDIsWindowModeToggleHotkey(code, wParam, lParam)) {
        after = toggleWindowModeWithChrome(g_keyboardObserverHwnd);
        result = 1;
    } else {
        result = g_origKeyboardHook(code, wParam, lParam);
        after = DDGetDisplayMode();
    }

    // The WH_KEYBOARD path consumes Alt+Enter/F4 (and any configured secondary hotkey), so no
    // dependable WndProc key message follows. Marshal one private message after its synchronous
    // mode change.
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

/*
 * cnc-ddraw's util_toggle_fullscreen() already performs the complete renderer/display rebuild via
 * dd_SetDisplayMode().  Stage the menu for the destination mode before that rebuild so its client
 * and viewport calculations see the final non-client geometry.  syncChrome() then only verifies
 * the result; DDRelayoutCurrentMode remains the recovery path when SetMenu failed or an external
 * transition left unexpected chrome behind.
 *
 * Do not call DrawMenuBar here.  Drawing the destination chrome while the source mode is still
 * visible is precisely the intermediate frame which used to flash.  The completed mode-set sends
 * SWP_FRAMECHANGED, and syncChrome() requests the final non-client repaint afterwards.
 */
bool stageChromeForTargetMode(HWND hwnd, int targetMode)
{
    if (targetMode < 0 || !hwnd || !g_bar)
        return false;

    const bool targetWantsMenu = targetMode == 0;
    const bool hasOurMenu = GetMenu(hwnd) == g_bar;
    if (targetWantsMenu == hasOurMenu)
        return false;

    if (!targetWantsMenu)
        setWrapperCursorVisible(false);
    return SetMenu(hwnd, targetWantsMenu ? g_bar : nullptr) != FALSE;
}

int toggleWindowModeWithChrome(HWND hwnd)
{
    const int before = DDGetDisplayMode();
    if (DDGetActiveRenderer() == 0 && g_shaderIdx >= 0) {
        scanShaderAssets();
        if (!g_shaderAvailable[g_shaderIdx]) {
            refreshChecks();
            showShaderAssetsWarningOnce(hwnd);
            return before;
        }
    }
    if (before < 0) {
        DDToggleWindowedMode();
        return DDGetDisplayMode();
    }

    // Both fullscreen variants have no menu; either one toggles back to a normal window.
    const int targetMode = before == 0 ? 1 : 0;
    const bool staged = stageChromeForTargetMode(hwnd, targetMode);

    DDToggleWindowedMode();
    const int after = DDGetDisplayMode();

    // A guarded/failed toggle must not leave the pre-staged destination chrome on the source mode.
    if (staged && after == before) {
        stageChromeForTargetMode(hwnd, before);
        DrawMenuBar(hwnd);
        reevaluateWrapperCursor(hwnd);
    }

    return after;
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
    const int previousLiveMode = g_liveModeIdx;
    const bool liveModeChanged =
        previousLiveMode >= 0 && previousLiveMode != liveMode;

    const bool wantMenu = liveMode == 0;
    const bool hasOurMenu = GetMenu(hwnd) == g_bar;
    bool changed = false;

    if (wantMenu && !hasOurMenu) {
        changed = SetMenu(hwnd, g_bar) != FALSE;
    } else if (!wantMenu && hasOurMenu) {
        setWrapperCursorVisible(false);
        changed = SetMenu(hwnd, nullptr) != FALSE;
    }

    g_liveModeIdx = liveMode;
    // Reconcile files periodically as part of the existing 1.5 s chrome health check. This also
    // catches external mode changes and files removed/restored without opening the Filter menu.
    scanShaderAssets();
    refreshChecks();
    showShaderAssetsWarningOnce(hwnd);

    if (changed || liveModeChanged)
        DrawMenuBar(hwnd);
    if (changed) {
        // Recompute the client area and mouse viewport without cfg_load(): re-reading ddraw.ini here
        // would immediately undo the hotkey transition. A wrapper-owned hotkey normally pre-stages
        // chrome and skips this fallback; external/unexpected menu state still needs a real relayout.
        DDRelayoutCurrentMode();
        mlog("[menu] chrome %s (mode=%d)", wantMenu ? "attached" : "detached", liveMode);
    }

    // Fullscreen -> normal: the renderer and menu now agree on client chrome, so restore the exact
    // pre-fullscreen outer placement saved by DDToggleWindowedMode (DisciplesGL semantics).
    DDCompleteWindowedModeToggle();

    // F4 and cnc-ddraw's Alt+Enter change only live g_config state. Persist the successfully
    // observed result so the next process starts in the mode the user actually left selected.
    // Do not write on the first sync: that is startup observation, not a user transition.
    if (liveModeChanged) {
        if (saveLiveDisplayModeForNextStart(liveMode)) {
            mlog("[menu] live display mode %d saved for next start", liveMode);
            if (liveMode != 0)
                showFirstFullscreenPersistenceNotice(hwnd);
        } else {
            MessageBoxW(
                hwnd,
                L(L"The display mode changed for this session, but it could not be saved to ddraw.ini. The next game start may use the previous mode.",
                  L"Режим экрана изменён для текущего сеанса, но сохранить его в ddraw.ini не удалось. При следующем запуске игра может вернуться к прежнему режиму."),
                L(L"Display mode", L"Режим экрана"),
                MB_OK | MB_ICONWARNING);
        }
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
        // an area comparison also remains robust for unusual validated canvas sizes.
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
    eventtrace_install();
    messagebatch_install(hwnd, iniFile());
    messagebatch_window_event(hwnd, msg, wParam);
    eventtrace_message(C4TRACE_RENDER_WND, hwnd, msg, wParam, lParam);
    if (!result)
        return 0;

    if (!g_gameHwnd)
        g_gameHwnd = hwnd;

    if (pluginhost_battle_state_message(msg)) {
        *result = 0;
        return 1;
    }
    if (timerhost_auto_battle_message(msg)) {
        *result = 0;
        return 1;
    }

    // The renderer observes focus changes before the validated MNS game-WndProc detour, and may
    // consume some of them. Keep this before the Russobit early return so client/menu cursor mode is
    // reconciled immediately. The exact hook repeats it safely when delivery continues.
    handleCursorLifecycle(hwnd, msg, wParam);

    if (g_ver != VerRussobit && msg == WM_TIMER && wParam == kPressTimerId) {
        verifyPendingRenderer();
        if (!g_pendingRendererVerifyTick)
            KillTimer(hwnd, kPressTimerId);
        *result = 0;
        return 1;
    }

    // Wrapper-owned keys must be handled at the renderer boundary before the Russobit early return.
    // Some renderer/compatibility paths consume a message before the native game WndProc detour;
    // handling it here also prevents double delivery because fake_WndProc stops when we return 1.
    if (msg == WM_KEYDOWN && handleAnimSpeedHotkey(wParam)) {
        *result = 0;
        return 1;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        pluginhost_key(msg, wParam, lParam)) {
        *result = 0;
        return 1;
    }
    if (msg == WM_KEYDOWN && wParam == VK_F4 && !(lParam & 0x40000000)) {
        toggleWindowModeWithChrome(hwnd);
        syncChrome(hwnd);
        *result = 0;
        return 1;
    }
    if ((msg == WM_LBUTTONUP || msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED) &&
        pluginhost_mouse(msg, wParam)) {
        *result = 0;
        return 1;
    }
    if (timerhost_filter_input(msg, wParam, lParam)) {
        *result = 0;
        return 1;
    }

    // Exact Russobit delivery continues through wndProcHook, where DOWN/MOVE are routed once after
    // the same fail-open timer safety seam. Other executable layouts have no such detour, so complete the
    // plugin pointer route here; release/cancel lifecycle was already delivered above.
    if (g_ver != VerRussobit &&
        (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE) &&
        pluginhost_mouse(msg, wParam)) {
        *result = 0;
        return 1;
    }

    if (g_ver == VerRussobit)
        return 0;

    // Generic/Akella/GOG/Steam path: fake_WndProc calls this bridge before it transforms mouse
    // coordinates or forwards WM_SETCURSOR to the game, so use the OS cursor position + renderer
    // viewport just like the exact MNS/SMNS path above.
    if (handleDecorativeCursor(hwnd, msg, lParam)) {
        *result = TRUE;
        return 1;
    }

    switch (msg) {
    case WM_NCMOUSEMOVE:
        setWrapperCursorVisible(true, true);
        break;
    case WM_ENTERMENULOOP:
        g_menuLoopActive = true;
        setWrapperCursorVisible(true, true);
        pluginhost_menu_loop(1);
        break;
    case WM_EXITMENULOOP:
        g_menuLoopActive = false;
        pluginhost_menu_loop(0);
        reevaluateWrapperCursor(hwnd);
        break;
    default:
        break;
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
            reevaluateWrapperCursor(hwnd);
            *result = 0;
            return 1;
        }
        if (id >= 0xB000 && id < 0xC000 && pluginhost_command(id)) {
            reevaluateWrapperCursor(hwnd);
            *result = 0;
            return 1;
        }
    } else if (msg == WM_INITMENUPOPUP) {
        if (reinterpret_cast<HMENU>(wParam) == g_shaderMenu ||
            reinterpret_cast<HMENU>(wParam) == g_rendMenu)
            scanShaderAssets();
        pluginhost_refresh_menus();
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
DWORD g_edgeScrollRealTick = 0;  // unscaled wall-clock time of the last successful native step
bool g_edgeScrollRealTickValid = false;
volatile LONG g_edgeScrollNativeDelay = 0; // Disciple.ini ScrollSpeed: 0/50/100
volatile LONG g_edgeScrollStepX = 32;
volatile LONG g_edgeScrollStepY = 16;
volatile LONG g_edgeScrollDglPatchesInstalled = 0;
constexpr DWORD kEdgeScrollCadenceHz = 60;
constexpr DWORD kEdgeScrollCadenceMs = 1000 / kEdgeScrollCadenceHz; // DGL integer formula: 16ms
constexpr DWORD kEdgeScrollFallbackIntervalMs = 33; // fail-safe if the exact call site is unknown
bool g_dragScrollActive = false;
bool g_dragMoved = false;
PointI g_dragMapCenter{};     // exact center tile returned by the game at button-down
PointI g_dragPointerAnchor{}; // button-down cursor plus the center's sub-tile screen offset
PointI g_dragStart{};         // cursor at button-down (click-vs-drag detection)
constexpr int kDragStartThreshold = 1; // first changed game pixel starts the drag

void cancelDragScroll()
{
    g_dragScrollActive = false;
    g_dragMoved = false;
    if (g_gameHwnd && GetCapture() == g_gameHwnd)
        ReleaseCapture();
}

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

// Exact replacement for the call to MapGraphics::getScrollSpeed at 0x54241E. The native values
// 0/50/100 are retained as DGL's 100%/75%/50% distance factors, while the returned deadline is the
// wrapper's canonical 60Hz cadence for every setting. This keeps the step independent of monitor
// refresh/maxfps; an intentionally sub-60 maxgameticks cap can still reduce input polling, like the
// rest of the game. Map-animation time scaling is neutralized by scrollDirHook's matching unscaled
// wall-clock gate below.
DWORD __fastcall edgeScrollIntervalHook(void* mapHolder, void* /*edx*/)
{
    LONG nativeDelay = 0;
    __try {
        if (isUserPtr(mapHolder)) {
            char* map = *reinterpret_cast<char**>(mapHolder);
            if (isUserPtr(map))
                nativeDelay = *reinterpret_cast<int*>(map + 0x40);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nativeDelay = 0;
    }
    if (nativeDelay < 0)
        nativeDelay = 0;
    if (nativeDelay > 100)
        nativeDelay = 100;
    InterlockedExchange(&g_edgeScrollNativeDelay, nativeDelay);
    return kEdgeScrollCadenceMs;
}

// The DGL v3.01a offset hook replaces `mov edi,[eax]; push 16; push 32` at 0x541C2D. Its stack
// contract is important: after the thunk returns, the generated x/y values occupy exactly the two
// argument slots that the original pushes created; EAX is restored for the following mov ebx,[eax+4].
void __fastcall fillEdgeScrollOffset(PointI* offset)
{
    offset->x = InterlockedExchangeAdd(&g_edgeScrollStepX, 0);
    offset->y = InterlockedExchangeAdd(&g_edgeScrollStepY, 0);
}

__declspec(naked) void edgeScrollOffsetThunk()
{
    __asm {
        pop ecx
        push eax
        push eax
        push ecx
        push eax
        lea ecx, [esp+0x8]
        call fillEdgeScrollOffset
        pop eax
        mov edi, [eax]
        ret
    }
}

void getDglEdgeScrollStep(int* stepX, int* stepY)
{
    int width = 0, height = 0;
    if (!DDGetScaleMetrics(&width, &height, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr) ||
        width <= 0 || height <= 0) {
        width = 800;
        height = 600;
    }

    const double diagonal = std::sqrt(static_cast<double>(width) * width +
                                      static_cast<double>(height) * height);
    DWORD multi = static_cast<DWORD>(diagonal * 600.0 /
                                     (static_cast<double>(kEdgeScrollCadenceHz) * 800.0));
    if (!multi)
        multi = 1;

    LONG nativeDelay = InterlockedExchangeAdd(&g_edgeScrollNativeDelay, 0);
    if (nativeDelay < 0)
        nativeDelay = 0;
    if (nativeDelay > 100)
        nativeDelay = 100;
    const double factor = 1.0 - 0.005 * nativeDelay;
    int y = static_cast<int>(std::floor(multi * factor + 0.5));
    int x = static_cast<int>(std::floor(multi * factor * 2.0 + 0.5));
    if (y < 1)
        y = 1;
    if (x < 1)
        x = 1;
    *stepX = x;
    *stepY = y;
}

bool installEdgeScrollDglPatches()
{
    constexpr uintptr_t offsetSiteVA = 0x541C2D;
    constexpr uintptr_t timingSiteVA = 0x54241E;
    static const std::uint8_t offsetExpected[6] = {
        0x8B, 0x38, 0x6A, 0x10, 0x6A, 0x20 // mov edi,[eax]; push 16; push 32
    };
    static const std::uint8_t timingExpected[5] = {
        0xE8, 0x93, 0xF0, 0xFF, 0xFF // call MapGraphics::getScrollSpeed (0x5414B6)
    };
    if (memcmp(reinterpret_cast<const void*>(offsetSiteVA), offsetExpected,
               sizeof(offsetExpected)) != 0 ||
        memcmp(reinterpret_cast<const void*>(timingSiteVA), timingExpected,
               sizeof(timingExpected)) != 0) {
        mlog("[edge] DGL signatures mismatch at %#x/%#x; keeping 33ms fail-safe",
             static_cast<unsigned>(offsetSiteVA), static_cast<unsigned>(timingSiteVA));
        return false;
    }

    std::uint8_t offsetCall[6] = {0xE8, 0, 0, 0, 0, 0x90};
    std::int32_t rel = static_cast<std::int32_t>(
        reinterpret_cast<uintptr_t>(&edgeScrollOffsetThunk) -
        (offsetSiteVA + 5));
    memcpy(offsetCall + 1, &rel, sizeof(rel));
    std::uint8_t timingCall[5] = {0xE8, 0, 0, 0, 0};
    rel = static_cast<std::int32_t>(
        reinterpret_cast<uintptr_t>(&edgeScrollIntervalHook) -
        (timingSiteVA + sizeof(timingCall)));
    memcpy(timingCall + 1, &rel, sizeof(rel));

    if (!writeBytes(offsetSiteVA, offsetCall, sizeof(offsetCall))) {
        mlog("[edge] DGL offset patch FAILED at %#x", static_cast<unsigned>(offsetSiteVA));
        return false;
    }
    if (!writeBytes(timingSiteVA, timingCall, sizeof(timingCall))) {
        // Keep the two call sites all-or-nothing: a dynamic step with the native 0/50/100ms delay
        // would multiply the menu speed factor twice. The original bytes are already validated.
        writeBytes(offsetSiteVA, offsetExpected, sizeof(offsetExpected));
        mlog("[edge] DGL timing patch FAILED at %#x; offset patch rolled back",
             static_cast<unsigned>(timingSiteVA));
        return false;
    }
    InterlockedExchange(&g_edgeScrollDglPatchesInstalled, 1);
    mlog("[edge] DGL cadence+offset patches installed (60Hz, live native 0/50/100)");
    return true;
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

// GetMapCenter may legitimately report an out-of-range center when the viewport presses against a
// map edge. Feeding that value back into SetMapCenter makes the native routine fail its first bounds
// check forever, so the held drag cannot recover by reversing direction. Reproduce DisciplesGL's
// boundary repair exactly: inspect the offset in the two isometric map axes, zero only the component
// that points beyond an edge, then convert the remaining offset back to screen coordinates and
// re-anchor the held gesture. Interior moves deliberately keep their original down-time invariant.
void normalizeDragBoundary(void* mg, int pointerX, int pointerY)
{
    PointI mapCenter{}, screenOffset{};
    reinterpret_cast<GetMapCenterFn>(0x5414BC)(
        mg, &mapCenter, &screenOffset.x, &screenOffset.y);

    auto* mapGeometry = reinterpret_cast<int*>(*reinterpret_cast<void**>(mg));
    if (!isUserPtr(mapGeometry))
        return;
    const int size = mapGeometry[5];
    if (size <= 0)
        return;

    // screen -> isometric map axes (the exact integer transform used by DGL).
    int mapOffsetX = screenOffset.y * 2 + screenOffset.x;
    int mapOffsetY = screenOffset.y * 2 - screenOffset.x;
    bool reset = false;

    if (mapCenter.x < 0 || (mapCenter.x == 0 && mapOffsetX < 0)) {
        mapCenter.x = 0;
        mapOffsetX = 0;
        reset = true;
    } else if (mapCenter.x >= size ||
               (mapCenter.x == size - 1 && mapOffsetX > 0)) {
        mapCenter.x = size - 1;
        mapOffsetX = 0;
        reset = true;
    }

    if (mapCenter.y < 0 || (mapCenter.y == 0 && mapOffsetY < 0)) {
        mapCenter.y = 0;
        mapOffsetY = 0;
        reset = true;
    } else if (mapCenter.y >= size ||
               (mapCenter.y == size - 1 && mapOffsetY > 0)) {
        mapCenter.y = size - 1;
        mapOffsetY = 0;
        reset = true;
    }

    if (!reset)
        return;

    // isometric map axes -> screen offset. Keep the unclamped axis so diagonal dragging remains
    // continuous along the edge, while reversing the clamped axis works on the very next sample.
    screenOffset.x = (mapOffsetX - mapOffsetY) / 2;
    screenOffset.y = (mapOffsetX + mapOffsetY) / 4;
    g_dragMapCenter = mapCenter;
    g_dragPointerAnchor = {
        pointerX + screenOffset.x,
        pointerY + screenOffset.y,
    };
    panMapCenterSmooth(mg, &g_dragMapCenter, screenOffset.x, screenOffset.y);

    static int logged = 0;
    if (logged++ < 8) {
        mlog("[drag] boundary normalized center=%d,%d offset=%d,%d pointer=%d,%d",
             mapCenter.x, mapCenter.y, screenOffset.x, screenOffset.y, pointerX, pointerY);
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
                // Win32 capture keeps moves reaching our main WndProc when D2 routes the held
                // pointer away from the iso view. Unlike DGL's internal dialog capture, this must
                // be paired with explicit focus/capture/panel-release cleanup in wndProcHook.
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
            if (mg) {
                panMapCenterSmooth(mg, &g_dragMapCenter,
                                   g_dragPointerAnchor.x - pt->x,
                                   g_dragPointerAnchor.y - pt->y);
                normalizeDragBoundary(mg, pt->x, pt->y);
            }
            return 1; // consume moves while panning
        } else if (g_dragScrollActive && msgId == WM_LBUTTONUP) {
            const bool moved = g_dragMoved;
            cancelDragScroll();
            if (!moved) {
                // Plain click: replay the down so it selects/opens. Do NOT then forward the up - the
                // replayed down can open a dialog (e.g. a city) and tear down the iso view, so forwarding
                // the up to that stale view derefs a freed child (game sub_5CA3F1, this[3]==NULL) -> crash.
                callOrigIsoMouse(view, WM_LBUTTONDOWN, pt);
                return 1;
            }
            return callOrigIsoMouse(view, msgId, pt); // a real drag: view intact, forward the up to end it
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cancelDragScroll();
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
        normalizeDragBoundary(mg, gameX, gameY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cancelDragScroll();
    }
}

// The game's iso DIRECTIONAL scroll: sub_54249C -> sub_541BC1 -> sub_54301B. Single choke point for
// window-EDGE scroll (sub_541BC1 reached only through here). Programmatic centering uses sub_541588, so
// gating this kills edge-scroll without touching click-to-center or our drag-pan. The native caller
// reads the game's virtual clock; map animation x15 can therefore make its deadline pass too early.
// The exact DGL call sites above provide a 60Hz cadence and a resolution-scaled step. Mirror that
// cadence on the unscaled clock here. If either exact patch is unavailable, retain v1.8's 33ms
// fail-safe instead of combining half of the new timing model with the native one. The fixed step
// deliberately does not replay time spent away from an edge as one large move on re-entry.
// Suppress scrolling during an actual held-button drag and while this game window is inactive. The
// latter replaces the removed synthetic-center GetCursorPos guard: cursor coordinates always stay
// real, while background maps still cannot scroll.
// __thiscall(self, dir) via __fastcall(ecx=self, edx, dir); installed unconditionally.
char __fastcall scrollDirHook(void* self, void* /*edx*/, int dir)
{
    const bool inactive =
        g_gameHwnd && DDGetPhysicalForegroundWindow() != g_gameHwnd;
    if (g_scrollDirDiag < 12) {
        ++g_scrollDirDiag;
        mlog("[edge] scrollDir dir=%d dragging=%d inactive=%d", dir,
             g_dragScrollActive ? 1 : 0, inactive ? 1 : 0);
    }
    if (g_dragScrollActive || inactive) {
        g_edgeScrollRealTickValid = false;
        return 0; // avoid fighting grab-pan and never move a background client's map
    }

    // g_realTimeGetTime is the original WINMM import saved before the game's IAT slot is redirected
    // to timeGetTimeHook. DWORD subtraction deliberately keeps the 49-day wraparound semantics safe.
    const DWORD now = g_realTimeGetTime ? g_realTimeGetTime() : GetTickCount();
    const bool dglPatched =
        InterlockedExchangeAdd(&g_edgeScrollDglPatchesInstalled, 0) == 1;
    const DWORD interval = dglPatched
        ? kEdgeScrollCadenceMs
        : kEdgeScrollFallbackIntervalMs;
    const DWORD elapsed = g_edgeScrollRealTickValid
        ? now - g_edgeScrollRealTick
        : interval;
    // The native deadline is shared by every direction; changing edge/corner must not bypass it.
    if (g_edgeScrollRealTickValid && elapsed < interval)
        return 0;

    if (dglPatched) {
        int stepX = 0, stepY = 0;
        getDglEdgeScrollStep(&stepX, &stepY);
        InterlockedExchange(&g_edgeScrollStepX, stepX);
        InterlockedExchange(&g_edgeScrollStepY, stepY);
    }

    const char moved =
        reinterpret_cast<char(__fastcall*)(void*, void*, int)>(g_origScrollDir)(self, nullptr, dir);
    // Match the native caller: a blocked boundary does not advance its last-success deadline.
    if (moved) {
        g_edgeScrollRealTick = now;
        g_edgeScrollRealTickValid = true;
    }
    return moved;
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
        const bool dglScroll = installEdgeScrollDglPatches();
        mlog("[menu] drag-scroll + edge-scroll detours installed (0x48E8A0 iso, 0x54249C edge; drag default %s; edge %s)",
             g_dragScroll ? "on" : "off",
             dglScroll ? "DGL cadence/step" : "v1.8 fail-safe");
    }
}

// Entry point called from cnc-ddraw's DllMain (DLL_PROCESS_ATTACH), after hook_init + the embed.
extern "C" void featuremenu_install(void)
{
    detectVersion();
    // The unique CStratInterf construction happens before the first WndProc dispatch, so capture
    // its lifecycle now, while DLL_PROCESS_ATTACH still precedes the EXE entry point.
    installStatusTextHooks();

    const int cursorCapture = g_ver == VerRussobit ? cursorcapture_install() : 0;
    mlog("[cursor] native dynamic cursor capture %s",
         cursorCapture ? "ABI validated" : "unavailable; native cursor ownership unchanged");
    if (cursorCapture) {
        InterlockedExchange(&g_cursorCaptureInstalled, 1);
        DDEnableD2CursorOwnership();
    }

    // Exact-address gameplay features are limited to the validated MNS/SMNS
    // layout. Other builds still receive the address-free renderer menu; its
    // MNS/SMNS-labelled entries are visible but disabled.
    if (g_ver != VerRussobit && g_ver != VerEditor)
        mlog("[menu] game exe %lu bytes: generic menu; MNS/SMNS patches disabled", g_exeSize);

    if (horplus_is_available() || g_ver == VerRussobit) {
        migrateLegacyDisplaySize();
        readDisplaySize();
    }

    // The logical canvas has already been selected by horplus_install(). Remove any stale outer
    // size which would make the finished game image smaller than its own native pixel grid.
    normalizeUndersizedOutput();

    // Migrate the old adapter's EnableZoom/ZoomFactor only when this build has no menu-owned value
    // yet. Copy both paths because exeDirFile() intentionally reuses one scratch buffer.
    char stretchMenuPath[MAX_PATH] = {};
    lstrcpynA(stretchMenuPath, iniFile(), sizeof(stretchMenuPath));
    char stretchValue[16] = {};
    const bool stretchStored =
        GetPrivateProfileStringA("menu", "stretchWindows", "", stretchValue,
                                 sizeof(stretchValue), stretchMenuPath) > 0;
    int requestedWindowStretch = 100;
    if (stretchStored) {
        requestedWindowStretch = atoi(stretchValue);
    } else {
        char gameSettingsPath[MAX_PATH] = {};
        lstrcpynA(gameSettingsPath, discipleIni(), sizeof(gameSettingsPath));
        const bool enabled =
            GetPrivateProfileIntA("Disciple", "EnableZoom", 1, gameSettingsPath) != 0;
        int legacyFactor =
            GetPrivateProfileIntA("Wrapper", "ZoomFactor", 100, gameSettingsPath);
        if (legacyFactor <= 0 || legacyFactor > 100)
            legacyFactor = 100;
        requestedWindowStretch = enabled ? legacyFactor : 0;
    }
    if (requestedWindowStretch < 0)
        requestedWindowStretch = 0;
    if (requestedWindowStretch > 100)
        requestedWindowStretch = 100;
    requestedWindowStretch = ((requestedWindowStretch + 5) / 10) * 10;

    // First run: generate a commented C4menu.ini (converting any old mss32menu.ini). This seed step
    // itself does not touch the game's own Disciple.ini / settings.lua; the explicit native-canvas
    // migration above is the sole startup exception.
    seedConfigFirstRun();
    if (!stretchStored) {
        char value[8] = {};
        wsprintfA(value, "%d", requestedWindowStretch);
        WritePrivateProfileStringA("menu", "stretchWindows", value, stretchMenuPath);
    }

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

    g_windowStretchPercent = requestedWindowStretch;
    // The bridge stores the percentage atomically and derives the exact old centred crop lazily
    // once DirectDraw has created the logical canvas. This is presentation-only and needs no restart.
    horplus_set_window_stretch_percent(g_windowStretchPercent);
    DDSetWindowStretchPercent(g_windowStretchPercent);

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
    char battleFactorRaw[16] = {};
    if (GetPrivateProfileStringA("menu", "battleAnimFactor", "", battleFactorRaw,
                                 sizeof(battleFactorRaw), f) > 0) {
        g_battleBaseFactor = atoi(battleFactorRaw);
    } else {
        g_battleBaseFactor = g_battleAnimEnabled
            ? kAnimFactor[g_battleAnimSpeed - 1]
            : 10;
    }
    if (g_battleBaseFactor < 10)
        g_battleBaseFactor = 10;
    if (g_battleBaseFactor > 150)
        g_battleBaseFactor = 150;
    if (g_battleBaseFactor == 10)
        g_battleAnimEnabled = false;
    g_mapAnimEnabled = GetPrivateProfileIntA("menu", "mapAnimEnabled", 0, f) != 0;
    g_mapAnimSpeed = GetPrivateProfileIntA("menu", "mapAnimSpeed", 5, f);
    if (g_mapAnimSpeed < 1)
        g_mapAnimSpeed = 1;
    if (g_mapAnimSpeed > 6)
        g_mapAnimSpeed = 6;
    char mapFactorRaw[16] = {};
    if (GetPrivateProfileStringA("menu", "mapAnimFactor", "", mapFactorRaw,
                                 sizeof(mapFactorRaw), f) > 0) {
        g_mapBaseFactor = atoi(mapFactorRaw);
    } else {
        g_mapBaseFactor = g_mapAnimEnabled
            ? kAnimFactor[g_mapAnimSpeed - 1]
            : 10;
    }
    if (g_mapBaseFactor < 10)
        g_mapBaseFactor = 10;
    if (g_mapBaseFactor > 150)
        g_mapBaseFactor = 150;
    if (g_mapBaseFactor == 10)
        g_mapAnimEnabled = false;
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
        GetPrivateProfileIntA("menu", "autoConfirmUnitHire",
                              g_ver == VerRussobit ? 1 : 0, f) != 0;
    char fastAiRaw[16] = {};
    const bool fastAiStored =
        GetPrivateProfileStringA("menu", "fastAI", "", fastAiRaw,
                                 sizeof(fastAiRaw), f) > 0;
    const int fastAiRequested = fastAiStored
        ? (atoi(fastAiRaw) != 0 ? 1 : 0)
        : (GetPrivateProfileIntA("Wrapper", "FastAI", 0, discipleIni()) != 0 ? 1 : 0);
    if (g_ver == VerRussobit) {
        // Apply only loader-safe state here. Shared EXE vtable hooks are deferred to the first GUI
        // dispatch, after all imported modules have completed process attach.
        applyAlwaysActive(g_alwaysActive);
        installTimeScaleHook();
        installDoubleClickTimeFix(); // keep D2's native 250ms input window under virtual time
        installDragScrollDetour(); // map grab+drag panning; pass-through when off
        dvoInstall(); // voiced-dialog auto-skip + logger; pass-through when off
        installUnitHireConfirmHook(); // X005TA0285; pass-through unless enabled
        fastai_install(); // exact two-callsite gate; shared predicate is deliberately not detoured
        fastai_set_enabled(fastAiRequested);
        applyAnimFactor(0, g_battleAnimEnabled, g_battleBaseFactor);
        applyAnimFactor(1, g_mapAnimEnabled, g_mapBaseFactor);
        installWndProcDetour();
    }

    HANDLE thread = CreateThread(nullptr, 0, &menuWorker, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    mlog("[menu] feature menu scheduled "
         "(alwaysActive=%d, battleAnim=%d/%.1fx, mapAnim=%d/%.1fx)",
         g_alwaysActive ? 1 : 0, g_battleAnimEnabled ? 1 : 0,
         g_battleBaseFactor / 10.0, g_mapAnimEnabled ? 1 : 0,
         g_mapBaseFactor / 10.0);
}
