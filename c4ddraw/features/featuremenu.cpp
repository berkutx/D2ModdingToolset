/*
 * C4dll-R monolith: DisciplesGL-style in-game menu + feature toggles.
 *
 * Ported from the D2ModdingToolset mss32 module (featuremenu.cpp) into the embedded cnc-ddraw
 * renderer so the final C4dll-R.dll is a SELF-CONTAINED, swappable assembly (renderer + menu),
 * exactly like DisciplesGL was. It does NOT depend on mss32: the few bits it needs from the game
 * (version detection, the GameSettings pointer chain) are inlined here as raw addresses/offsets,
 * the renderer's own DDReloadConfig / DDTakeScreenshot exports are called directly (same module),
 * and logging goes to a local logger instead of spdlog.
 *
 * Original file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset). GPLv3+. See the repo LICENSE.
 *
 * cnc-ddraw renders into the game's own window but has no in-game settings menu, so we add a real
 * menu BAR below the title bar with DGL-style options.
 *   - The real title bar comes from cnc-ddraw itself (ddraw.ini border=true); we only attach the
 *     menu BAR (SetMenu) + drive it. We DETOUR the game's window procedure by address (not a
 *     SetWindowLong subclass, which cnc-ddraw clobbers) to receive WM_COMMAND.
 *   - Live toggles (always-active, animation speed) patch the game in-process and persist to
 *     C4menu.ini next to the exe.
 *   - Renderer / shader / mode / resolution / vsync / boxing settings are cnc-ddraw settings: the
 *     menu writes them to ddraw.ini and re-applies them LIVE via the renderer's DDReloadConfig
 *     (a few init-once ones still say "(restart)").
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

// The renderer's own exports (defined in dllmain.c, same module). Called directly: no GetProcAddress,
// no separate ddraw.dll. DDReloadConfig re-reads ddraw.ini + dd_SetDisplayMode; DDTakeScreenshot
// saves a timestamped PNG. Both no-op safely if the renderer is not yet initialized.
extern "C" void DDReloadConfig(void);
extern "C" void DDTakeScreenshot(void);

// Plugin host (pluginhost.cpp) - used to build the "Plugins" menu (split legacy .mod / native .c4p).
extern "C" void pluginhost_wait_ready(unsigned ms);
extern "C" int pluginhost_count(void);
extern "C" const char* pluginhost_name(int i);
extern "C" int pluginhost_is_new(int i);
extern "C" void* pluginhost_menu(int i);
extern "C" int pluginhost_command(unsigned id); // route a plugin-block WM_COMMAND to its c4p_command

namespace {

// --- minimal logger (replaces spdlog) -> OutputDebugStringA + C4menu.log next to the exe -------
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

void mlog(const char* fmt, ...)
{
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
    HANDLE h = CreateFileA(exeDirFile("C4menu.log"), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &w, nullptr);
        CloseHandle(h);
    }
}

// --- game version detection (inlined from D2ModdingToolset version.cpp: by exe file size) -------
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

// --- always-active: force the foreground-flag store to 1 (no pause on focus loss) -----
// Russobit only for now (same site/bytes as testdrv/bootfixes.cpp 0x5628BE). Toggles live.
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

// --- animation speed: DGL-style virtual clock over timeGetTime --------------------------
// The game drives sprite animation off timeGetTime(): it advances a frame when
// timeGetTime() >= nextUpdate, then nextUpdate += interval (66ms slow / 33ms fast lists, plus the
// per-unit battle intervals). DisciplesGL sped this up NOT by patching game memory, but by hooking
// the game's imported timeGetTime and returning a VIRTUAL clock that advances factor/10 times
// faster (this is exactly DGL's sub_10012DE0 in the .disciplesgl backup; our exe goes through DGL's
// table B / else branch, so this -- not the table-A-only per-object +0x98 hook -- is the mechanism
// that always worked for us). Every interval the game compares against then shrinks proportionally
// -> faster animation, with ZERO game-state writes. Version-independent, race-free (no init-timing
// crash like the old 66/33 const-patch), adjustable LIVE. We patch only the game exe's
// WINMM!timeGetTime IAT slot, so cnc-ddraw's own fps-limiter timeGetTime is left untouched.
//
// factor: 10 = x1.0 (identity, vanilla) ... 50 = x5.0. The clock re-anchors whenever the factor
// changes so virtual time stays continuous (no jump) across a live speed change.
using TimeGetTimeFn = DWORD(WINAPI*)(void);
TimeGetTimeFn g_realTimeGetTime = nullptr;

// Two independent live multipliers (x10 fixed-point: 10 = x1.0 ... 50 = x5.0). The hook picks the
// battle one vs the map one by g_inBattle (set by the battle-viewer discriminator below). 10 is
// identity, so an "off" knob leaves that context at vanilla speed.
volatile LONG g_battleFactor = 10;
volatile LONG g_mapFactor = 10;
volatile LONG g_inBattle = 0;

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
    if (g_vcLastFactor != factor) { // factor changed (incl. a battle<->map switch) -> re-anchor
        g_vcLastFactor = factor;
        g_vcRealAnchor = g_vcRealNow;
        g_vcVirtAnchor = g_vcVirtNow;
    }
    g_vcRealNow = g_realTimeGetTime();
    DWORD result = factor * (g_vcRealNow - g_vcRealAnchor) / 10u + g_vcVirtAnchor;
    g_vcVirtNow = result;
    return result;
}

// Patch the game exe's WINMM!timeGetTime import thunk -> our virtual clock. Russobit IAT slot
// 0x6CE420 (verified: Discipl2.exe import, module WINMM). At factor 10 the clock is identity, so we
// install it unconditionally and harmlessly; the menu just moves the factor live.
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

// --- battle vs map discriminator: g_inBattle --------------------------------------------
// To split the multipliers we must know whether the LOCAL client is currently showing a battle.
// The battle viewer (IBatViewer) has a fixed vftable @0x6F4294 on Russobit (from the D2 modding
// toolset); its method slots are [0] destructor, [1] update, [2] showAttackEffect, [3] battleEnd
// (verified in our exe: 0x645900 / 0x630DE3 / 0x63203B / 0x631FFC). update + showAttackEffect fire
// throughout every battle; battleEnd + destructor fire when it closes. We overwrite the 4 vftable
// slots with naked thunks that just latch g_inBattle and tail-jump the saved original -> can't miss
// a battle, and it is calling-convention agnostic (ECX/this and the stack pass through untouched;
// only a memory store + an indirect jmp, no registers clobbered). No Detours, no prologue patching;
// the vftable is patched once at install when no battle is live. Battles are never nested, so a
// plain flag suffices.
void* g_batDtorOrig = nullptr;
void* g_batUpdateOrig = nullptr;
void* g_batShowOrig = nullptr;
void* g_batEndOrig = nullptr;

__declspec(naked) void batUpdateThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 1
        jmp dword ptr [g_batUpdateOrig]
    }
}
__declspec(naked) void batShowThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 1
        jmp dword ptr [g_batShowOrig]
    }
}
__declspec(naked) void batEndThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 0
        jmp dword ptr [g_batEndOrig]
    }
}
__declspec(naked) void batDtorThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 0
        jmp dword ptr [g_batDtorOrig]
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
}

// Map menu "Speed N" (1..5) to a live virtual-clock factor for battle (which=0) or map (which=1).
// Off = x1.0 (vanilla). Range is 1.5x..5x (the user asked to raise the old 1.5x cap to 5x). Runs on
// the game UI thread (menu WM_COMMAND).
void applyAnimSpeed(int which, bool enabled, int speed)
{
    static const int kFactor[5] = {15, 20, 30, 40, 50}; // 1.5x, 2x, 3x, 4x, 5x
    if (speed < 1)
        speed = 1;
    if (speed > 5)
        speed = 5;
    LONG f = enabled ? kFactor[speed - 1] : 10;
    if (which == 0)
        g_battleFactor = f;
    else
        g_mapFactor = f;
}

// --- persistence (a small ini next to the exe) ------------------------------------------
const char* iniFile()
{
    return exeDirFile("C4menu.ini");
}

// --- menu state + command IDs (WM_COMMAND from the menu bar) ---------------------------
enum : UINT
{
    kIdAlwaysActive = 0xA100,
    kIdAnimOff = 0xA110, // BATTLE animation multiplier (live virtual clock): off / 1.5x..5x
    kIdAnim1 = 0xA111,
    kIdAnim2 = 0xA112,
    kIdAnim3 = 0xA113,
    kIdAnim4 = 0xA114,
    kIdAnim5 = 0xA115,
    // cnc-ddraw (ddraw.ini) settings
    kIdRendOpenGL = 0xA130,
    kIdRendGdi = 0xA131,
    kIdRendAuto = 0xA132,
    kIdShaderBase = 0xA140, // kIdShaderBase + index into kShaders[]
    kIdMaintas = 0xA150,
    kIdVsync = 0xA151,
    kIdBoxing = 0xA152,
    kIdTicks0 = 0xA160,
    kIdTicks30 = 0xA161,
    kIdTicks60 = 0xA162,
    kIdTicks100 = 0xA163,
    kIdResBase = 0xA170, // + index into kRes[]  (window output size: width/height)
    kIdFpsBase = 0xA180, // + index into kFpsValues[] (maxfps frame cap)
    // native game speeds (GameSettings + Disciple.ini); apply on the next battle / turn
    kIdBattle1 = 0xA190, // battleSpeed 1..4 (slow/normal/fast/instant)
    kIdBattle2 = 0xA191,
    kIdBattle3 = 0xA192,
    kIdBattle4 = 0xA193,
    kIdMap1 = 0xA1A0, // playerSpeed/opponentSpeed 1..3 (normal/fast/very fast)
    kIdMap2 = 0xA1A1,
    kIdMap3 = 0xA1A2,
    kIdModeWindowed = 0xA1B0, // ddraw.ini windowed/fullscreen pair (live via DDReloadConfig)
    kIdModeBorderless = 0xA1B1,
    kIdModeExclusive = 0xA1B2,
    kIdScreenshot = 0xA1C0, // action: take a screenshot (cnc-ddraw DDTakeScreenshot)
    kIdAnimMapOff = 0xA1D0, // MAP animation multiplier (live virtual clock): off / 1.5x..5x
    kIdAnimMap1 = 0xA1D1,
    kIdAnimMap2 = 0xA1D2,
    kIdAnimMap3 = 0xA1D3,
    kIdAnimMap4 = 0xA1D4,
    kIdAnimMap5 = 0xA1D5,
    kIdLast = 0xA1FF, // upper bound of our WM_COMMAND id block
};

// cnc-ddraw renderer + shader/filter tables (menu label + the exact ddraw.ini value).
struct NameVal
{
    const char* label;
    const char* value;
};
const NameVal kRenderers[] = {
    {"OpenGL - shaders + best upscaling (recommended)", "opengl"},
    {"GDI - software, max compatibility (slower)", "gdi"},
    {"Auto - pick D3D9/OpenGL automatically", "auto"}};
const int kRendererCount = 3;
// Image filters, ranked best->basic for D2's hand-painted art (see hints).
const NameVal kShaders[] = {
    {"Lanczos - sharp, detailed (best for D2 art)", "Shaders\\interpolation\\lanczos2-sharp.glsl"},
    {"xBRZ - pixel-art scaler, clean sprite edges", "Shaders\\xbrz\\xbrz-freescale-multipass.glsl"},
    {"Bicubic - smooth, balanced (cnc default)", "Shaders\\interpolation\\catmull-rom-bilinear.glsl"},
    {"AMD FSR - modern edge sharpening, crisp", "Shaders\\interpolation\\fsr.glsl"},
    {"xBR lv2 - pixel-art, lighter than xBRZ", "Shaders\\xbr\\xbr-lv2-noblend.glsl"},
    {"Bilinear - simple smoothing, a bit soft", "Shaders\\interpolation\\bilinear.glsl"},
    {"None - sharpest pixels, blocky on zoom", "Shaders\\nearest-neighbor.glsl"},
    {"CRT - retro scanlines (style, not sharper)", "Shaders\\crt\\crt-lottes-fast-no-warp-bilinear.glsl"}};
const int kShaderCount = 8;
const int kTicksValues[] = {0, 30, 60, 100};
const int kTicksCount = 4;
// Output window size (ddraw.ini width/height). 0,0 = native game size (1024x768); larger
// values upscale the render via the selected shader. cnc-ddraw centres the window.
struct ResOpt
{
    const char* label;
    int w, h;
};
const ResOpt kRes[] = {
    {"Native (game size)", 0, 0},
    {"640 x 480", 640, 480},
    {"800 x 600", 800, 600},
    {"1152 x 864", 1152, 864},
    {"1280 x 960", 1280, 960},
    {"1280 x 720 (16:9)", 1280, 720},
    {"1366 x 768 (16:9)", 1366, 768},
    {"1600 x 1200", 1600, 1200},
    {"1600 x 900 (16:9)", 1600, 900},
    {"1920 x 1440", 1920, 1440},
    {"1920 x 1080 (16:9)", 1920, 1080},
    {"2560 x 1440 (16:9)", 2560, 1440},
    {"3840 x 2160 (16:9)", 3840, 2160}};
const int kResCount = 13; // 0xA170..0xA17C (room up to 0xA17F before kIdFpsBase)
const int kFpsValues[] = {-1, 30, 60, 144};
const char* kFpsLabels[] = {"VSync / refresh", "30", "60", "144"};
const int kFpsCount = 4;
// Display mode = the ddraw.ini windowed/fullscreen pair. Windowed keeps the caption + menu;
// the fullscreen modes are borderless (caption goes away, the menu bar stays). Exclusive does a
// real mode change locally and falls back to borderless where that's disallowed (e.g. RDP).
struct ModeOpt
{
    const char* label;
    const char* windowed;
    const char* fullscreen;
};
const ModeOpt kModes[] = {
    {"Windowed - title bar + menu, draggable", "true", "false"},
    {"Fullscreen borderless - fills screen, alt-tab friendly", "true", "true"},
    {"Fullscreen exclusive - real mode change (local only)", "false", "true"}};
const int kModeCount = 3;

bool g_alwaysActive = false;
bool g_battleAnimEnabled = false; // BATTLE live anim multiplier (virtual clock)
int g_battleAnimSpeed = 5;
bool g_mapAnimEnabled = false;    // MAP live anim multiplier (virtual clock)
int g_mapAnimSpeed = 5;
// cnc-ddraw (ddraw.ini) state, read at startup so the menu shows the current values
int g_rendererIdx = 0;  // index into kRenderers (-1 = unknown/custom)
int g_shaderIdx = -1;   // index into kShaders   (-1 = unknown/custom)
bool g_maintas = false, g_vsync = false, g_boxing = false;
int g_ticksIdx = -1;    // index into kTicksValues (-1 = custom)
int g_resIdx = -1;      // index into kRes (-1 = custom)
int g_fpsIdx = -1;      // index into kFpsValues (-1 = custom)
int g_battleSpeed = 2;  // native GameSettings.battleSpeed (1..4)
int g_mapSpeed = 1;     // native GameSettings.playerSpeed/opponentSpeed (1..3)
int g_modeIdx = 0;      // index into kModes (windowed/borderless/exclusive)
HMENU g_bar = nullptr;
int g_grownTo = 0; // window height we last grew to (so the menu bar does not cut the game; no re-fight)
HMENU g_gameMenu = nullptr, g_videoMenu = nullptr, g_perfMenu = nullptr; // top-level bar menus
HMENU g_battleAnimMenu = nullptr, g_mapAnimMenu = nullptr;
HMENU g_rendMenu = nullptr, g_shaderMenu = nullptr;
HMENU g_ticksMenu = nullptr, g_resMenu = nullptr, g_fpsMenu = nullptr;
HMENU g_battleMenu = nullptr, g_mapMenu = nullptr, g_modeMenu = nullptr;
int g_ncCursorShown = 0;  // 1 while we've bumped the OS cursor visible for the non-client area
int g_ncCursorAdded = 0;  // how many ShowCursor(TRUE) we added (to remove exactly that many)

using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProcFn g_origWndProc = nullptr;

uintptr_t gameWndProcVA()
{
    return g_ver == VerRussobit ? 0x562e0f : 0;
}

void persist()
{
    const char* f = iniFile();
    char buf[8];
    WritePrivateProfileStringA("menu", "alwaysActive", g_alwaysActive ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "battleAnimEnabled", g_battleAnimEnabled ? "1" : "0", f);
    wsprintfA(buf, "%d", g_battleAnimSpeed);
    WritePrivateProfileStringA("menu", "battleAnimSpeed", buf, f);
    WritePrivateProfileStringA("menu", "mapAnimEnabled", g_mapAnimEnabled ? "1" : "0", f);
    wsprintfA(buf, "%d", g_mapAnimSpeed);
    WritePrivateProfileStringA("menu", "mapAnimSpeed", buf, f);
}

// First-run config: if C4menu.ini does not exist yet, generate a complete, COMMENTED one and convert
// any settings carried over from the old mss32-track mss32menu.ini. This never touches the game's own
// Disciple.ini / Scripts\settings.lua (those stay the player's; the native Battle/Map speed menu
// writes Disciple.ini only when the user actually changes it). We write the file directly (the
// WritePrivateProfileStringA API cannot emit comments), so the user gets a self-documenting config.
void seedConfigFirstRun()
{
    const char* f = iniFile();
    if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES)
        return; // already present -> the user owns it; leave it untouched

    // Convert from the old single-global anim config, if an mss32menu.ini is present next to the exe.
    const char* old = exeDirFile("mss32menu.ini");
    const int aa = GetPrivateProfileIntA("menu", "alwaysActive", 0, old) ? 1 : 0;
    const int oldAnimOn = GetPrivateProfileIntA("menu", "animationSpeedEnabled", 0, old);
    // The old animationSpeed (1..5) topped out near 1.5x; the new per-context scale STARTS at 1.5x
    // (= speed 1), so an old "on" maps to speed 1 on BOTH contexts (the old global affected all anim).
    const int en = oldAnimOn ? 1 : 0;
    const int bSp = oldAnimOn ? 1 : 2; // sensible default when later enabled: 2x
    const int mSp = oldAnimOn ? 1 : 2;

    char buf[1024];
    const int n = wsprintfA(buf,
        "; C4dll-R menu settings (auto-generated on first run).\r\n"
        "; Edit by hand, or use the in-game \"Game\" menu - changes are saved back here.\r\n"
        "; SEPARATE from the game's own Disciple.ini / Scripts\\settings.lua, which C4dll-R\r\n"
        "; never modifies on its own.\r\n"
        "\r\n"
        "[menu]\r\n"
        "; Keep the game running (no pause) when the window loses focus.  0 = off, 1 = on.\r\n"
        "alwaysActive=%d\r\n"
        "\r\n"
        "; Live animation-speed multiplier, separate for battle and the strategic map\r\n"
        "; (DisciplesGL-style timeGetTime virtual clock; safe, no game-memory patch).\r\n"
        ";   *Enabled : 0 = vanilla, 1 = on.   *Speed : 1..5  ->  1.5x / 2x / 3x / 4x / 5x.\r\n"
        "battleAnimEnabled=%d\r\n"
        "battleAnimSpeed=%d\r\n"
        "mapAnimEnabled=%d\r\n"
        "mapAnimSpeed=%d\r\n",
        aa, en, bSp, en, mSp);

    HANDLE h = CreateFileA(f, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, buf, static_cast<DWORD>(n), &wr, nullptr);
        CloseHandle(h);
        mlog("[menu] first-run C4menu.ini generated (alwaysActive=%d, anim on=%d, b=%d m=%d)", aa, en,
             bSp, mSp);
    }
}

// --- cnc-ddraw settings live in ddraw.ini next to the exe (same dir as C4menu.ini) ----
const char* ddrawIni()
{
    return exeDirFile("ddraw.ini");
}

bool readDdrawBool(const char* key, bool def)
{
    char v[16] = {};
    GetPrivateProfileStringA("ddraw", key, def ? "true" : "false", v, sizeof(v), ddrawIni());
    return lstrcmpiA(v, "true") == 0 || lstrcmpiA(v, "1") == 0 || lstrcmpiA(v, "yes") == 0;
}

void writeDdrawStr(const char* key, const char* value)
{
    // WritePrivateProfileString preserves the file's comment lines and writes a clean
    // key=value (no inline comment) -- exactly what cnc-ddraw's strict parser needs.
    WritePrivateProfileStringA("ddraw", key, value, ddrawIni());
}

void writeDdrawBool(const char* key, bool on)
{
    writeDdrawStr(key, on ? "true" : "false");
}

// Re-apply cnc-ddraw settings live: we ARE the renderer, so call DDReloadConfig directly (it
// re-reads ddraw.ini + dd_SetDisplayMode). Runs on the UI thread (the menu WM_COMMAND thread).
void applyDdrawLive()
{
    DDReloadConfig();
}

// Take a screenshot via the renderer's DDTakeScreenshot (timestamped PNG in .\Screenshots\,
// same as the PrintScreen hotkey).
void takeScreenshot()
{
    DDTakeScreenshot();
}

// Read the current ddraw.ini values into menu state so the checks reflect reality.
void readDdrawState()
{
    char r[32] = {};
    GetPrivateProfileStringA("ddraw", "renderer", "opengl", r, sizeof(r), ddrawIni());
    g_rendererIdx = -1;
    for (int i = 0; i < kRendererCount; ++i)
        if (lstrcmpiA(r, kRenderers[i].value) == 0) {
            g_rendererIdx = i;
            break;
        }

    char sh[MAX_PATH] = {};
    GetPrivateProfileStringA("ddraw", "shader", "", sh, sizeof(sh), ddrawIni());
    g_shaderIdx = -1;
    for (int i = 0; i < kShaderCount; ++i)
        if (lstrcmpiA(sh, kShaders[i].value) == 0) {
            g_shaderIdx = i;
            break;
        }

    g_maintas = readDdrawBool("maintas", false);
    g_vsync = readDdrawBool("vsync", false);
    g_boxing = readDdrawBool("boxing", false);

    const int ticks = GetPrivateProfileIntA("ddraw", "maxgameticks", 0, ddrawIni());
    g_ticksIdx = -1;
    for (int i = 0; i < kTicksCount; ++i)
        if (kTicksValues[i] == ticks) {
            g_ticksIdx = i;
            break;
        }

    const int w = GetPrivateProfileIntA("ddraw", "width", 0, ddrawIni());
    const int h = GetPrivateProfileIntA("ddraw", "height", 0, ddrawIni());
    g_resIdx = -1;
    for (int i = 0; i < kResCount; ++i)
        if (kRes[i].w == w && kRes[i].h == h) {
            g_resIdx = i;
            break;
        }

    const int fps = GetPrivateProfileIntA("ddraw", "maxfps", -1, ddrawIni());
    g_fpsIdx = -1;
    for (int i = 0; i < kFpsCount; ++i)
        if (kFpsValues[i] == fps) {
            g_fpsIdx = i;
            break;
        }

    const bool wnd = readDdrawBool("windowed", false);
    const bool fs = readDdrawBool("fullscreen", false);
    g_modeIdx = !wnd ? 2 : (fs ? 1 : 0); // !windowed=exclusive; windowed+fullscreen=borderless
}

// --- native game speeds: battle vs map (the engine's own split) -------------------------
// These live in the GameSettings struct (read at battle/turn start) and Disciple.ini [Settings].
// Battle = battleSpeed (1 slow .. 4 instant); map turns = playerSpeed/opponentSpeed (1..3).
const char* discipleIni()
{
    return exeDirFile("Disciple.ini");
}

// In-memory GameSettings access, inlined from D2ModdingToolset (midgard.h / gamesettings.h).
// Russobit only. CMidgardApi::instance() @ 0x401d35 (__cdecl, returns CMidgard*); CMidgard.data
// @ +8; CMidgardData.settings @ +60 is a GameSettings**; GameSettings fields: playerSpeed @ +360,
// opponentSpeed @ +364, battleSpeed @ +372 (String=16B layout, struct size 468).
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

// Current turn player, for the plugin host's host-driven turn detection (pluginhost.cpp calls this).
// Reads the game's in-process server logic: CMidgard (instance @0x401d35) -> data @+8 ->
// CMidServer @+44 -> CMidServerData @+12 -> CMidServerLogic @+28 -> currentPlayerIndex @+244. The
// index advances on every turn change, including a SKIPPED turn (the server moves it itself), which
// is exactly the signal the timer needs. Present for host/hotseat games (server runs in-process); a
// pure network client has no server here and returns -1. Struct offsets are from the D2 modding
// toolset and are stable across the localized builds; only the singleton anchor is version-specific
// (Russobit). SEH-guarded and runs off the game thread (polled by the overlay worker): a transient
// or torn pointer chain just yields -1, never a crash. *outInGame is set to 1 when a player is read.
// Battle state for the timer plugin's Combat Pause (pluginhost exposes it via C4P_Host.is_in_battle).
// g_inBattle is the battle-viewer-vftable (0x6F4294) discriminator set in the anim section above.
extern "C" int featuremenu_in_battle(void)
{
    return g_inBattle ? 1 : 0;
}

// Current scenario day (= CScenarioInfo.currentTurn) for the timer's Timetable (per-day duration).
// Reads the in-process server's object map and looks up the ScenarioInfo object. Russobit-only,
// SEH-guarded, READ-ONLY. Chain: CMidgard(instance)->data@+8 -> server@+44 -> CMidServer.data@+12 ->
// CMidServerLogic@+28 -> CMidServerLogicCore::coreData@+4 -> CMidServerLogicCoreData::objectMap@+20.
// Then objectMap->vftable[1] getId() -> scenario id; build the ScenarioInfo id from its parts
// (CMidgardID bits: category=v>>30, categoryIndex=(v>>22)&0xff, type=(v>>16)&0x3f; IdType
// ScenarioInfo=27); objectMap->vftable[5] findScenarioObjectById() -> CScenarioInfo;
// currentTurn@+32. Returns -1 when unavailable (e.g. a pure network client with no in-process server).
extern "C" int featuremenu_current_day(void)
{
    if (g_ver != VerRussobit)
        return -1;
    int day = -1;
    __try {
        auto instance = reinterpret_cast<char*(__cdecl*)()>(0x401d35);
        char* mid = instance();
        if (!isUserPtr(mid)) return -1;
        char* data = *reinterpret_cast<char**>(mid + 8);
        if (!isUserPtr(data)) return -1;
        char* server = *reinterpret_cast<char**>(data + 44);
        if (!isUserPtr(server)) return -1;
        char* sdata = *reinterpret_cast<char**>(server + 12);
        if (!isUserPtr(sdata)) return -1;
        char* logic = *reinterpret_cast<char**>(sdata + 28); // CMidServerLogic
        if (!isUserPtr(logic)) return -1;
        char* coreData = *reinterpret_cast<char**>(logic + 4); // CMidServerLogicCore::coreData
        if (!isUserPtr(coreData)) return -1;
        void** objectMap = *reinterpret_cast<void***>(coreData + 20); // CMidServerLogicCoreData::objectMap
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
        day = *reinterpret_cast<int*>(obj + 32); // CScenarioInfo.currentTurn
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (day < 0 || day > 100000)
        return -1;
    return day;
}

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

// Apply a native speed live to the in-memory GameSettings (takes effect next battle/turn) and
// persist it to Disciple.ini. which: 0 = battle, 1 = map (player + opponent). Runs on the game
// UI thread (from the menu WM_COMMAND), so touching the game struct here is safe.
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

void refreshChecks()
{
    if (!g_gameMenu)
        return;
    // Game
    CheckMenuItem(g_gameMenu, kIdAlwaysActive,
                  MF_BYCOMMAND | (g_alwaysActive ? MF_CHECKED : MF_UNCHECKED));
    const UINT bSel = g_battleAnimEnabled ? (kIdAnim1 + static_cast<UINT>(g_battleAnimSpeed - 1)) : kIdAnimOff;
    if (g_battleAnimMenu)
        CheckMenuRadioItem(g_battleAnimMenu, kIdAnimOff, kIdAnim5, bSel, MF_BYCOMMAND);
    const UINT mSel = g_mapAnimEnabled ? (kIdAnimMap1 + static_cast<UINT>(g_mapAnimSpeed - 1)) : kIdAnimMapOff;
    if (g_mapAnimMenu)
        CheckMenuRadioItem(g_mapAnimMenu, kIdAnimMapOff, kIdAnimMap5, mSel, MF_BYCOMMAND);
    if (g_battleMenu)
        CheckMenuRadioItem(g_battleMenu, kIdBattle1, kIdBattle4,
                           kIdBattle1 + static_cast<UINT>(g_battleSpeed - 1), MF_BYCOMMAND);
    if (g_mapMenu)
        CheckMenuRadioItem(g_mapMenu, kIdMap1, kIdMap3,
                           kIdMap1 + static_cast<UINT>(g_mapSpeed - 1), MF_BYCOMMAND);
    // Video
    if (g_modeMenu && g_modeIdx >= 0)
        CheckMenuRadioItem(g_modeMenu, kIdModeWindowed, kIdModeExclusive,
                           kIdModeWindowed + static_cast<UINT>(g_modeIdx), MF_BYCOMMAND);
    if (g_resMenu && g_resIdx >= 0)
        CheckMenuRadioItem(g_resMenu, kIdResBase, kIdResBase + kResCount - 1,
                           kIdResBase + static_cast<UINT>(g_resIdx), MF_BYCOMMAND);
    if (g_shaderMenu && g_shaderIdx >= 0)
        CheckMenuRadioItem(g_shaderMenu, kIdShaderBase, kIdShaderBase + kShaderCount - 1,
                           kIdShaderBase + static_cast<UINT>(g_shaderIdx), MF_BYCOMMAND);
    if (g_rendMenu && g_rendererIdx >= 0)
        CheckMenuRadioItem(g_rendMenu, kIdRendOpenGL, kIdRendAuto,
                           kIdRendOpenGL + static_cast<UINT>(g_rendererIdx), MF_BYCOMMAND);
    if (g_videoMenu) {
        CheckMenuItem(g_videoMenu, kIdMaintas, MF_BYCOMMAND | (g_maintas ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(g_videoMenu, kIdVsync, MF_BYCOMMAND | (g_vsync ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(g_videoMenu, kIdBoxing, MF_BYCOMMAND | (g_boxing ? MF_CHECKED : MF_UNCHECKED));
    }
    // Performance
    if (g_fpsMenu && g_fpsIdx >= 0)
        CheckMenuRadioItem(g_fpsMenu, kIdFpsBase, kIdFpsBase + kFpsCount - 1,
                           kIdFpsBase + static_cast<UINT>(g_fpsIdx), MF_BYCOMMAND);
    if (g_ticksMenu && g_ticksIdx >= 0)
        CheckMenuRadioItem(g_ticksMenu, kIdTicks0, kIdTicks100,
                           kIdTicks0 + static_cast<UINT>(g_ticksIdx), MF_BYCOMMAND);
}

void onMenuCommand(UINT id)
{
    bool restartItem = false;
    if (id == kIdAlwaysActive) {
        g_alwaysActive = !g_alwaysActive;
        applyAlwaysActive(g_alwaysActive);
    } else if (id == kIdAnimOff) {
        g_battleAnimEnabled = false;
        applyAnimSpeed(0, false, g_battleAnimSpeed);
    } else if (id >= kIdAnim1 && id <= kIdAnim5) {
        g_battleAnimEnabled = true;
        g_battleAnimSpeed = static_cast<int>(id - kIdAnim1) + 1;
        applyAnimSpeed(0, true, g_battleAnimSpeed);
    } else if (id == kIdAnimMapOff) {
        g_mapAnimEnabled = false;
        applyAnimSpeed(1, false, g_mapAnimSpeed);
    } else if (id >= kIdAnimMap1 && id <= kIdAnimMap5) {
        g_mapAnimEnabled = true;
        g_mapAnimSpeed = static_cast<int>(id - kIdAnimMap1) + 1;
        applyAnimSpeed(1, true, g_mapAnimSpeed);
    } else if (id >= kIdRendOpenGL && id <= kIdRendAuto) {
        g_rendererIdx = static_cast<int>(id - kIdRendOpenGL);
        writeDdrawStr("renderer", kRenderers[g_rendererIdx].value);
        restartItem = true;
    } else if (id >= kIdShaderBase && id < kIdShaderBase + static_cast<UINT>(kShaderCount)) {
        g_shaderIdx = static_cast<int>(id - kIdShaderBase);
        writeDdrawStr("shader", kShaders[g_shaderIdx].value);
        restartItem = true;
    } else if (id == kIdMaintas) {
        g_maintas = !g_maintas;
        writeDdrawBool("maintas", g_maintas);
        restartItem = true;
    } else if (id == kIdVsync) {
        g_vsync = !g_vsync;
        writeDdrawBool("vsync", g_vsync);
        restartItem = true;
    } else if (id == kIdBoxing) {
        g_boxing = !g_boxing;
        writeDdrawBool("boxing", g_boxing);
        restartItem = true;
    } else if (id >= kIdTicks0 && id <= kIdTicks100) {
        g_ticksIdx = static_cast<int>(id - kIdTicks0);
        char b[8];
        wsprintfA(b, "%d", kTicksValues[g_ticksIdx]);
        writeDdrawStr("maxgameticks", b);
        restartItem = true;
    } else if (id >= kIdResBase && id < kIdResBase + static_cast<UINT>(kResCount)) {
        g_resIdx = static_cast<int>(id - kIdResBase);
        char b[12];
        wsprintfA(b, "%d", kRes[g_resIdx].w);
        writeDdrawStr("width", b);
        wsprintfA(b, "%d", kRes[g_resIdx].h);
        writeDdrawStr("height", b);
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
        restartItem = true;
    } else if (id == kIdScreenshot) {
        takeScreenshot(); // action: save a PNG now; nothing to persist or re-check
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
    if (restartItem)
        applyDdrawLive(); // we are the renderer: re-apply live via DDReloadConfig
    else
        persist(); // live mod toggles -> C4menu.ini
}

// With devmode=true the game hides the OS cursor (it draws its own inside the client), so the
// pointer is invisible over our caption + menu bar (the non-client area). Bump the OS cursor
// visible while the mouse is over the non-client area and restore it (let the game draw its own)
// back in the client. ShowCursor is a global counter, so keep our +1/-1 balanced.
void setNonClientCursor(bool overNonClient)
{
    if (overNonClient) {
        if (!g_ncCursorShown) {
            // Force the global show-count non-negative (the game may have hidden it well
            // below -1), tracking how many we added so we can remove exactly that many.
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

LRESULT CALLBACK wndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Keep the OS pointer visible over the caption + menu bar (non-client), invisible in the
    // client where the game draws its own. WM_NCMOUSEMOVE reaches us via cnc-ddraw's default
    // message forward (wndproc.c); WM_MOUSEMOVE comes through its own forward.
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

    // Menu-bar command: WM_COMMAND with HIWORD==0 AND lParam==0. The lParam check is
    // essential — a control's WM_COMMAND (e.g. BN_CLICKED, notify code 0) also has
    // HIWORD==0 but passes the control HWND in lParam, and would otherwise collide with
    // our IDs and fire spuriously. Real menu commands always have lParam==0.
    if (msg == WM_COMMAND && lParam == 0 && HIWORD(wParam) == 0) {
        const UINT id = LOWORD(wParam);
        if (id >= kIdAlwaysActive && id <= kIdLast) {
            onMenuCommand(id);
            return 0;
        }
        // Plugin config-menu commands (0xB000+ block): route to the owning NEW plugin's c4p_command.
        // Legacy plugin ids fall through to the original WndProc -> the legacy plugin's own subclass.
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
    // Plugins now load on the host's worker thread (not the loader-lock-serialized DllMain), so wait
    // until that finishes before reading the plugin list for the "Plugins" submenu. Plugins load in
    // milliseconds (the worker starts at DllMain return); the game window we need is seconds away, so
    // this effectively never blocks. The timeout just means "build without plugins" if loading hangs.
    pluginhost_wait_ready(5000);
    readDdrawState();   // reflect the current ddraw.ini in the checks/radios
    readNativeSpeeds(); // reflect the current Disciple.ini battle/map speeds

    // ===== "Game" — gameplay / animation (live; Battle/Map speed presets apply next battle/turn) =====
    g_gameMenu = CreatePopupMenu();
    AppendMenuA(g_gameMenu, MF_STRING, kIdAlwaysActive, "Always active");
    g_battleAnimMenu = CreatePopupMenu();
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnimOff, "Off (vanilla)");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim1, "1.5x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim2, "2x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim3, "3x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim4, "4x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim5, "5x");
    AppendMenuA(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_battleAnimMenu),
                "Battle animation (live x1.5..x5)");
    g_mapAnimMenu = CreatePopupMenu();
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMapOff, "Off (vanilla)");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap1, "1.5x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap2, "2x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap3, "3x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap4, "4x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap5, "5x");
    AppendMenuA(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_mapAnimMenu),
                "Map animation (live x1.5..x5)");
    g_battleMenu = CreatePopupMenu();
    AppendMenuA(g_battleMenu, MF_STRING, kIdBattle1, "Slow");
    AppendMenuA(g_battleMenu, MF_STRING, kIdBattle2, "Normal");
    AppendMenuA(g_battleMenu, MF_STRING, kIdBattle3, "Fast");
    AppendMenuA(g_battleMenu, MF_STRING, kIdBattle4, "Instant");
    AppendMenuA(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_battleMenu),
                "Battle speed (game option, next battle)");
    g_mapMenu = CreatePopupMenu();
    AppendMenuA(g_mapMenu, MF_STRING, kIdMap1, "Normal");
    AppendMenuA(g_mapMenu, MF_STRING, kIdMap2, "Fast");
    AppendMenuA(g_mapMenu, MF_STRING, kIdMap3, "Very fast");
    AppendMenuA(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_mapMenu),
                "Map turn speed (game option, next turn)");

    // ===== "Video" — look (all live except Renderer) =====
    g_videoMenu = CreatePopupMenu();
    g_modeMenu = CreatePopupMenu();
    for (int i = 0; i < kModeCount; ++i)
        AppendMenuA(g_modeMenu, MF_STRING, kIdModeWindowed + i, kModes[i].label);
    AppendMenuA(g_modeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(g_modeMenu, MF_STRING | MF_GRAYED, 0, "Hotkey: Alt+Enter toggles fullscreen");
    AppendMenuA(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_modeMenu), "Display mode");
    g_resMenu = CreatePopupMenu();
    for (int i = 0; i < kResCount; ++i)
        AppendMenuA(g_resMenu, MF_STRING, kIdResBase + i, kRes[i].label);
    AppendMenuA(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_resMenu), "Resolution");
    g_shaderMenu = CreatePopupMenu();
    for (int i = 0; i < kShaderCount; ++i)
        AppendMenuA(g_shaderMenu, MF_STRING, kIdShaderBase + i, kShaders[i].label);
    AppendMenuA(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_shaderMenu), "Filter / upscale");
    g_rendMenu = CreatePopupMenu();
    for (int i = 0; i < kRendererCount; ++i)
        AppendMenuA(g_rendMenu, MF_STRING, kIdRendOpenGL + i, kRenderers[i].label);
    AppendMenuA(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_rendMenu), "Renderer (restart)");
    AppendMenuA(g_videoMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(g_videoMenu, MF_STRING, kIdMaintas, "Keep 4:3 aspect - no stretch on widescreen");
    AppendMenuA(g_videoMenu, MF_STRING, kIdVsync, "VSync - no tearing (caps to refresh)");
    AppendMenuA(g_videoMenu, MF_STRING, kIdBoxing, "Integer scaling - crisp whole-pixel zoom");
    AppendMenuA(g_videoMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(g_videoMenu, MF_STRING, kIdScreenshot, "Take screenshot (PrintScreen)");

    // ===== "Performance" — frame/CPU caps (apply on restart) =====
    g_perfMenu = CreatePopupMenu();
    g_fpsMenu = CreatePopupMenu();
    for (int i = 0; i < kFpsCount; ++i)
        AppendMenuA(g_fpsMenu, MF_STRING, kIdFpsBase + i, kFpsLabels[i]);
    AppendMenuA(g_perfMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_fpsMenu), "Frame cap (restart)");
    g_ticksMenu = CreatePopupMenu();
    AppendMenuA(g_ticksMenu, MF_STRING, kIdTicks0, "Uncapped (high CPU)");
    AppendMenuA(g_ticksMenu, MF_STRING, kIdTicks30, "30 (coolest)");
    AppendMenuA(g_ticksMenu, MF_STRING, kIdTicks60, "60 (default)");
    AppendMenuA(g_ticksMenu, MF_STRING, kIdTicks100, "100 (smoothest)");
    AppendMenuA(g_perfMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_ticksMenu),
                "Game speed cap (restart)");

    g_bar = CreateMenu();
    AppendMenuA(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_gameMenu), "Game");
    AppendMenuA(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_videoMenu), "Video");
    AppendMenuA(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_perfMenu), "Performance");

    // ===== "Plugins" — hosted Mods\ plugins, split into native (.c4p) and legacy (.mod) =====
    // (The plugin host loads them at startup; here we just surface them + graft each legacy
    // plugin's own config submenu. Its menu commands route to the plugin via the WndProc chain.)
    const int pc = pluginhost_count();
    if (pc > 0) {
        HMENU plugins = CreatePopupMenu();
        HMENU nativeSub = CreatePopupMenu();
        HMENU legacySub = CreatePopupMenu();
        int nNew = 0, nLegacy = 0;
        for (int i = 0; i < pc; ++i) {
            const char* nm = pluginhost_name(i);
            if (pluginhost_is_new(i)) {
                HMENU pm = reinterpret_cast<HMENU>(pluginhost_menu(i));
                if (pm) // the plugin's own config submenu (c4p_menu); commands route via c4p_command
                    AppendMenuA(nativeSub, MF_POPUP, reinterpret_cast<UINT_PTR>(pm), nm);
                else
                    AppendMenuA(nativeSub, MF_STRING | MF_GRAYED, 0, nm);
                ++nNew;
            } else {
                HMENU pm = reinterpret_cast<HMENU>(pluginhost_menu(i));
                if (pm)
                    AppendMenuA(legacySub, MF_POPUP, reinterpret_cast<UINT_PTR>(pm), nm);
                else
                    AppendMenuA(legacySub, MF_STRING | MF_GRAYED, 0, nm);
                ++nLegacy;
            }
        }
        AppendMenuA(plugins, MF_POPUP | (nNew ? 0u : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(nativeSub), "Native (.c4p)");
        AppendMenuA(plugins, MF_POPUP | (nLegacy ? 0u : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(legacySub), "Legacy (.mod)");
        AppendMenuA(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(plugins), "Plugins");
    }
}

// Attach our menu bar to the game window. The real caption comes from cnc-ddraw (ddraw.ini
// border=true); SetMenu is not blocked, so the menu bar attaches + renders along the window top.
void ensureChrome(HWND hwnd)
{
    if (GetMenu(hwnd) != g_bar) {
        SetMenu(hwnd, g_bar);
        DrawMenuBar(hwnd);
    }
    // Keep the window tall enough that the menu bar sits ABOVE the client (game) area instead of
    // eating into it. cnc-ddraw sizes the window without knowing about our menu, so after the menu is
    // attached - and again after any cnc-ddraw resize (e.g. a Video-menu mode/resolution change) - the
    // menu takes one row from the client and the top looks cut. Grow the window back by SM_CYMENU.
    // g_grownTo guards against re-growing a size we already produced, so we never fight cnc-ddraw
    // (verified: the window stays put after our grow, no oscillation).
    {
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        const int wh = wr.bottom - wr.top;
        const int dh = GetSystemMetrics(SM_CYMENU);
        if (wh != g_grownTo && dh > 0 && dh < 200) {
            g_grownTo = wh + dh;
            SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, g_grownTo,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            mlog("[menu] grew window to %dpx so the menu bar does not cut the game render area", g_grownTo);
        }
    }
    refreshChecks();
}

struct FindCtx
{
    DWORD pid;
    HWND found;
};

BOOL CALLBACK findGameWindow(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == ctx->pid && IsWindowVisible(hwnd)) {
        // Match the game's MAIN window by class (the title can be empty); confirmed
        // 'MQ_UIManager' for Disciples II. The game has more than one MQ_UIManager
        // window (incl. a zero-size one) — require a real size so we pick the real one.
        char cls[64] = {};
        GetClassNameA(hwnd, cls, sizeof(cls));
        if (lstrcmpA(cls, "MQ_UIManager") == 0) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            if ((rc.right - rc.left) >= 640 && (rc.bottom - rc.top) >= 400) {
                ctx->found = hwnd;
                return FALSE;
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
        FindCtx ctx{GetCurrentProcessId(), nullptr};
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
        ensureChrome(hwnd);
        if (!announced) {
            mlog("[menu] menu bar installed on hwnd %p", reinterpret_cast<void*>(hwnd));
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

// Entry point called from cnc-ddraw's DllMain (DLL_PROCESS_ATTACH), after hook_init + the embed.
extern "C" void timerhost_install(void); // timer keystone (features/timerhost.cpp)

extern "C" void featuremenu_install(void)
{
    detectVersion();

    // The menu + in-memory byte patches use Russobit-only addresses (WndProc to detour, always-active,
    // the timeGetTime IAT hook + battle-viewer vftable discriminator, the CMidgard chain). On any
    // other build we simply don't install (the embedded renderer works regardless). We test on the
    // Russobit build, so keep this dead simple.
    if (g_ver != VerRussobit) {
        mlog("[menu] unsupported game version (exe %lu bytes): menu/patches disabled", g_exeSize);
        return;
    }

    // ---- Russobit: full menu + feature install ----
    // First run: generate a commented C4menu.ini (converting any old mss32menu.ini values). Never
    // touches the game's own Disciple.ini / settings.lua.
    seedConfigFirstRun();

    const char* f = iniFile();
    g_alwaysActive = GetPrivateProfileIntA("menu", "alwaysActive", 0, f) != 0;
    // Split battle/map live-multiplier keys, default OFF (each context stays at vanilla speed until
    // the user opts in per context via the Game menu).
    g_battleAnimEnabled = GetPrivateProfileIntA("menu", "battleAnimEnabled", 0, f) != 0;
    g_battleAnimSpeed = GetPrivateProfileIntA("menu", "battleAnimSpeed", 5, f);
    g_mapAnimEnabled = GetPrivateProfileIntA("menu", "mapAnimEnabled", 0, f) != 0;
    g_mapAnimSpeed = GetPrivateProfileIntA("menu", "mapAnimSpeed", 5, f);

    // Apply now (DllMain context — before the game's main loop and anim init run). The IAT is
    // already populated by the loader at this point, so the time-scale hook installs cleanly here.
    // The battle discriminator patches the (idle) battle-viewer vftable; no battle is live yet.
    applyAlwaysActive(g_alwaysActive);
    installTimeScaleHook();
    installBattleDiscriminator();
    timerhost_install(); // timer keystone: capture dialog/battle buttons + combat/animation state
    applyAnimSpeed(0, g_battleAnimEnabled, g_battleAnimSpeed);
    applyAnimSpeed(1, g_mapAnimEnabled, g_mapAnimSpeed);

    installWndProcDetour();

    HANDLE thread = CreateThread(nullptr, 0, &menuWorker, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    mlog("[menu] feature menu scheduled (alwaysActive=%d, battleAnim=%d/%d, mapAnim=%d/%d)",
         g_alwaysActive ? 1 : 0, g_battleAnimEnabled ? 1 : 0, g_battleAnimSpeed,
         g_mapAnimEnabled ? 1 : 0, g_mapAnimSpeed);
}
