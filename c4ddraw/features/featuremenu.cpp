/*
 * C4dll-R monolith: in-game menu bar + feature toggles, embedded in the cnc-ddraw renderer.
 * Self-contained: no mss32 dependency; game version + GameSettings chain inlined as raw addresses.
 * Russobit-only patch sites. Original from D2ModdingToolset (GPLv3+, see repo LICENSE).
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

// Renderer exports (dllmain.c, same module): DDReloadConfig re-reads ddraw.ini + dd_SetDisplayMode;
// DDTakeScreenshot saves a PNG. Both no-op safely before the renderer is initialized.
extern "C" void DDReloadConfig(void);
extern "C" void DDTakeScreenshot(void);
extern "C" void DDMapClientToGame(long cx, long cy, long* gx, long* gy); // Win32 client -> game coords

// Plugin host (pluginhost.cpp): builds the "Plugins" menu (legacy .mod / native .c4p).
extern "C" void pluginhost_wait_ready(unsigned ms);
extern "C" int pluginhost_count(void);
extern "C" const char* pluginhost_name(int i);
extern "C" int pluginhost_is_new(int i);
extern "C" void* pluginhost_menu(int i);
extern "C" int pluginhost_command(unsigned id); // route a plugin-block WM_COMMAND to its c4p_command

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
    HANDLE h = CreateFileA(exeDirFile(logLeaf()), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &w, nullptr);
        CloseHandle(h);
    }
}

// --- game version detection (by exe file size) ---
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
volatile LONG g_attackPulse = 0; // set by batShowThunk (slot[2]) on each hit/effect; consumed by the pump

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
        mov dword ptr [g_attackPulse], 1
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
    // cnc-ddraw (ddraw.ini) settings
    kIdRendOpenGL = 0xA130,
    kIdRendGdi = 0xA131,
    kIdRendAuto = 0xA132,
    kIdShaderBase = 0xA140, // + index into kShaders[]
    kIdMaintas = 0xA150,
    kIdVsync = 0xA151,
    kIdBoxing = 0xA152,
    kIdTicks0 = 0xA160,
    kIdTicks30 = 0xA161,
    kIdTicks60 = 0xA162,
    kIdTicks100 = 0xA163,
    kIdResBase = 0xA170, // + index into kRes[]
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
    kIdLast = 0xA1FF, // upper bound of our WM_COMMAND id block
};

// cnc-ddraw renderer + shader tables (menu label + exact ddraw.ini value).
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
// Image filters, ranked best->basic for D2's hand-painted art.
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
// Output window size (ddraw.ini width/height). 0,0 = native game size (1024x768); larger upscales.
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
const int kResCount = 13; // 0xA170..0xA17C (room before kIdFpsBase)
const int kFpsValues[] = {-1, 30, 60, 144};
const char* kFpsLabels[] = {"VSync / refresh", "30", "60", "144"};
const int kFpsCount = 4;
// Display mode = ddraw.ini windowed/fullscreen pair. Windowed keeps caption + menu; fullscreen modes
// are borderless (menu bar stays). Exclusive does a real mode change, falls back to borderless (RDP).
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
bool g_battleAnimEnabled = false; // BATTLE live anim multiplier
int g_battleAnimSpeed = 5;
bool g_mapAnimEnabled = false;    // MAP live anim multiplier
int g_mapAnimSpeed = 5;
bool g_battleAttackEnabled = false; // BATTLE attack burst: speed up only while a hit/effect plays
int g_battleAttackSpeed = 3;        // burst factor (1..5); idle stays at the battle base (vanilla unless set)
DWORD g_attackExpiryTick = 0;       // burst window end (GetTickCount ms); 0 = no burst pending
// cnc-ddraw (ddraw.ini) state, read at startup so the menu shows current values (-1 = unknown/custom)
int g_rendererIdx = 0;  // index into kRenderers
int g_shaderIdx = -1;   // index into kShaders
bool g_maintas = false, g_vsync = false, g_boxing = false;
bool g_dragScroll = false; // grab+drag map panning (ini [menu] dragScroll, default off)
int g_ticksIdx = -1;    // index into kTicksValues
int g_resIdx = -1;      // index into kRes
int g_fpsIdx = -1;      // index into kFpsValues
int g_battleSpeed = 2;  // native GameSettings.battleSpeed (1..4)
int g_mapSpeed = 1;     // native GameSettings.playerSpeed/opponentSpeed (1..3)
int g_modeIdx = 0;      // index into kModes
HMENU g_bar = nullptr;
UINT g_relayoutMsg = 0; // registered msg: marshal the one-time menu-attach relayout onto the GUI thread
HMENU g_gameMenu = nullptr, g_videoMenu = nullptr, g_perfMenu = nullptr; // top-level bar menus
HMENU g_battleAnimMenu = nullptr, g_mapAnimMenu = nullptr, g_battleAtkMenu = nullptr;
HMENU g_rendMenu = nullptr, g_shaderMenu = nullptr;
HMENU g_ticksMenu = nullptr, g_resMenu = nullptr, g_fpsMenu = nullptr;
HMENU g_battleMenu = nullptr, g_mapMenu = nullptr, g_modeMenu = nullptr;
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
    WritePrivateProfileStringA("menu", "dragScroll", g_dragScroll ? "1" : "0", f);
}

// First run: if C4menu.ini is absent, generate a commented one (converting any old mss32menu.ini).
// Never touches the game's own Disciple.ini / settings.lua. Written directly since
// WritePrivateProfileStringA cannot emit comments.
void seedConfigFirstRun()
{
    const char* f = iniFile();
    if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES)
        return; // already present -> the user owns it

    // Convert from the old single-global anim config if mss32menu.ini is present.
    const char* old = exeDirFile("mss32menu.ini");
    const int aa = GetPrivateProfileIntA("menu", "alwaysActive", 0, old) ? 1 : 0;
    const int oldAnimOn = GetPrivateProfileIntA("menu", "animationSpeedEnabled", 0, old);
    // Old animationSpeed topped near 1.5x; the new scale STARTS at 1.5x (=speed 1), so old "on" -> speed 1.
    const int en = oldAnimOn ? 1 : 0;
    const int bSp = oldAnimOn ? 1 : 2; // default when later enabled: 2x
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
        "; (timeGetTime virtual clock; safe, no game-memory patch).\r\n"
        ";   *Enabled : 0 = vanilla, 1 = on.   *Speed : 1..6  ->  1.5x / 2x / 3x / 4x / 5x / 15x.\r\n"
        "battleAnimEnabled=%d\r\n"
        "battleAnimSpeed=%d\r\n"
        "mapAnimEnabled=%d\r\n"
        "mapAnimSpeed=%d\r\n"
        "\r\n"
        "; Battle attack burst: speed up ONLY while a hit/effect plays, so idle units stay calm.\r\n"
        ";   battleAttackEnabled : 0 = off, 1 = on.   battleAttackSpeed : 1..5  ->  1.5x..5x.\r\n"
        "battleAttackEnabled=0\r\n"
        "battleAttackSpeed=3\r\n",
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

// --- cnc-ddraw settings live in ddraw.ini (same dir as C4menu.ini) ---
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
    // Preserves comment lines and writes a clean key=value (no inline comment), as cnc-ddraw's
    // strict parser needs.
    WritePrivateProfileStringA("ddraw", key, value, ddrawIni());
}

void writeDdrawBool(const char* key, bool on)
{
    writeDdrawStr(key, on ? "true" : "false");
}

// Re-apply ddraw settings live: we ARE the renderer, so call DDReloadConfig directly. Runs on the UI thread.
void applyDdrawLive()
{
    DDReloadConfig();
}

// Take a screenshot via the renderer (timestamped PNG in .\Screenshots\).
void takeScreenshot()
{
    DDTakeScreenshot();
}

// Read current ddraw.ini values into menu state so the checks reflect reality.
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

// --- native game speeds: battle vs map (the engine's own split) ---
// In GameSettings (read at battle/turn start) and Disciple.ini [Settings].
const char* discipleIni()
{
    return exeDirFile("Disciple.ini");
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
// so waiting units don't twitch; each attack effect (vftable slot[2] -> g_attackPulse) opens a short
// window during which the virtual clock runs at the higher attack factor. All on the game UI thread
// (battle update sets the pulse, this runs from the WM_TIMER pump) - no game-internal pointers touched.
const DWORD kAttackHoldMs = 1200; // hold full attack factor while the hit plays (re-armed by each pulse)
const DWORD kAttackRampMs = 700;  // then ease back DOWN to the idle base over this long (no instant snap)
void updateBattleBurst(void)
{
    if (g_attackPulse) {
        g_attackPulse = 0;
        g_attackExpiryTick = GetTickCount() + kAttackHoldMs; // hold end; ramp runs for kAttackRampMs after it
    }
    if (!g_battleAttackEnabled)
        return; // split off: leave g_battleFactor to applyAnimSpeed (the live battle multiplier)
    const int idle = g_battleAnimEnabled ? kAnimFactor[g_battleAnimSpeed - 1] : 10; // idle base
    int f = idle;
    if (g_inBattle && g_attackExpiryTick) {
        const int atk = kAnimFactor[g_battleAttackSpeed - 1];
        const int sinceHoldEnd = static_cast<int>(GetTickCount() - g_attackExpiryTick);
        if (sinceHoldEnd < 0)
            f = atk; // still in the hold window: full attack speed
        else if (sinceHoldEnd < static_cast<int>(kAttackRampMs))
            f = atk + (idle - atk) * sinceHoldEnd / static_cast<int>(kAttackRampMs); // linear ease atk->idle
        if (f < idle)
            f = idle; // never dip below the idle base
    }
    g_battleFactor = f;
}

void refreshChecks()
{
    if (!g_gameMenu)
        return;
    // Game
    CheckMenuItem(g_gameMenu, kIdAlwaysActive,
                  MF_BYCOMMAND | (g_alwaysActive ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_gameMenu, kIdDragScroll,
                  MF_BYCOMMAND | (g_dragScroll ? MF_CHECKED : MF_UNCHECKED));
    const UINT bSel = g_battleAnimEnabled ? (kIdAnim1 + static_cast<UINT>(g_battleAnimSpeed - 1)) : kIdAnimOff;
    if (g_battleAnimMenu)
        CheckMenuRadioItem(g_battleAnimMenu, kIdAnimOff, kIdAnim6, bSel, MF_BYCOMMAND);
    const UINT mSel = g_mapAnimEnabled ? (kIdAnimMap1 + static_cast<UINT>(g_mapAnimSpeed - 1)) : kIdAnimMapOff;
    if (g_mapAnimMenu)
        CheckMenuRadioItem(g_mapAnimMenu, kIdAnimMapOff, kIdAnimMap6, mSel, MF_BYCOMMAND);
    const UINT aSel = g_battleAttackEnabled ? (kIdAtk1 + static_cast<UINT>(g_battleAttackSpeed - 1)) : kIdAtkOff;
    if (g_battleAtkMenu)
        CheckMenuRadioItem(g_battleAtkMenu, kIdAtkOff, kIdAtk5, aSel, MF_BYCOMMAND);
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
    } else if (id == kIdDragScroll) {
        g_dragScroll = !g_dragScroll; // live: the detour reads this flag (persist() saves it)
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
        applyAnimSpeed(0, g_battleAnimEnabled, g_battleAnimSpeed); // hand g_battleFactor back to the base
    } else if (id >= kIdAtk1 && id <= kIdAtk5) {
        g_battleAttackEnabled = true;
        g_battleAttackSpeed = static_cast<int>(id - kIdAtk1) + 1;
        updateBattleBurst(); // apply immediately (idle base now, burst on next hit)
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
    if (restartItem)
        applyDdrawLive(); // we are the renderer: re-apply live via DDReloadConfig
    else
        persist(); // live mod toggles -> C4menu.ini
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
        updateBattleBurst(); // idle/attack split: drive g_battleFactor from the attack pulse
        timerhost_pump();
        return 0;
    }

    // Drag-scroll pan: while LMB is held the game routes moves to the captured WndProc (SetCapture on
    // drag-start), not the iso vtable handler. Map client -> game coords and pan; consume.
    if (g_dragScrollActive && msg == WM_MOUSEMOVE) {
        long gx = 0, gy = 0;
        DDMapClientToGame((long)(short)LOWORD(lParam), (long)(short)HIWORD(lParam), &gx, &gy);
        dragScrollWndMove((int)gx, (int)gy);
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

    // GUI-thread menu-attach relayout (posted by menuWorker after SetMenu): re-apply renderer window
    // sizing WITH the menu attached so client == render area and viewport + mouse are recomputed in one
    // pass (replaces the old out-of-band SetWindowPos grow that raced the renderer).
    if (g_relayoutMsg && msg == g_relayoutMsg) {
        applyDdrawLive();
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
    // Plugins load on the host worker thread (not loader-lock-serialized DllMain), so wait for that
    // before reading the list. Loading takes milliseconds vs seconds to the game window, so this
    // effectively never blocks; the timeout just means "build without plugins" if loading hangs.
    pluginhost_wait_ready(5000);
    readDdrawState();   // reflect current ddraw.ini in the checks/radios
    readNativeSpeeds(); // reflect current Disciple.ini battle/map speeds

    // ===== "Game" - gameplay / animation (Battle/Map speed presets apply next battle/turn) =====
    g_gameMenu = CreatePopupMenu();
    AppendMenuA(g_gameMenu, MF_STRING, kIdAlwaysActive, "Always active");
    AppendMenuA(g_gameMenu, MF_STRING, kIdDragScroll, "Map drag-scroll (left button)");
    g_battleAnimMenu = CreatePopupMenu();
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnimOff, "Off (vanilla)");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim1, "1.5x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim2, "2x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim3, "3x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim4, "4x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim5, "5x");
    AppendMenuA(g_battleAnimMenu, MF_STRING, kIdAnim6, "15x (test)");
    AppendMenuA(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_battleAnimMenu),
                "Battle animation (live x1.5..x5)");
    g_battleAtkMenu = CreatePopupMenu();
    AppendMenuA(g_battleAtkMenu, MF_STRING, kIdAtkOff, "Off (calm idle)");
    AppendMenuA(g_battleAtkMenu, MF_STRING, kIdAtk1, "1.5x");
    AppendMenuA(g_battleAtkMenu, MF_STRING, kIdAtk2, "2x");
    AppendMenuA(g_battleAtkMenu, MF_STRING, kIdAtk3, "3x");
    AppendMenuA(g_battleAtkMenu, MF_STRING, kIdAtk4, "4x");
    AppendMenuA(g_battleAtkMenu, MF_STRING, kIdAtk5, "5x");
    AppendMenuA(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_battleAtkMenu),
                "Battle attack burst (fast hits, calm idle)");
    g_mapAnimMenu = CreatePopupMenu();
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMapOff, "Off (vanilla)");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap1, "1.5x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap2, "2x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap3, "3x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap4, "4x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap5, "5x");
    AppendMenuA(g_mapAnimMenu, MF_STRING, kIdAnimMap6, "15x (test)");
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

    // ===== "Video" - look (all live except Renderer) =====
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
    AppendMenuA(g_videoMenu, MF_STRING, kIdBoxing, "Integer scaling (pixel-art only - keep OFF to fill window)");
    AppendMenuA(g_videoMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(g_videoMenu, MF_STRING, kIdScreenshot, "Take screenshot (PrintScreen)");

    // ===== "Performance" - frame/CPU caps (apply on restart) =====
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

    // ===== "Plugins" - hosted Mods\ plugins, split native (.c4p) / legacy (.mod) =====
    // Surface each plugin + graft its config submenu; commands route to the plugin via the WndProc chain.
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

// Attach our menu bar to the game window (caption comes from cnc-ddraw, ddraw.ini border=true).
void ensureChrome(HWND hwnd)
{
    if (GetMenu(hwnd) != g_bar) {
        SetMenu(hwnd, g_bar);
        DrawMenuBar(hwnd);
        // The menu takes one row from the client. Do NOT correct with a raw SetWindowPos grow here: it
        // runs on this worker thread, lands AFTER cnc-ddraw's dd_SetDisplayMode, and fires a WM_SIZE
        // cnc-ddraw treats as a no-op - so render size, viewport and mouse.rc are never recomputed and
        // the added strip stays black (the live resolution-change bug). Instead re-lay THROUGH the
        // renderer: with the menu attached, DDReloadConfig -> dd_SetDisplayMode grows by one menu row
        // (AdjustWindowRectEx) AND recomputes viewport + mouse in one pass. dd_SetDisplayMode restarts
        // the render thread, so marshal it onto the GUI thread via PostMessage (g_relayoutMsg).
        if (g_relayoutMsg)
            PostMessageA(hwnd, g_relayoutMsg, 0, 0);
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
        // Match the game's MAIN window by class 'MQ_UIManager' (title may be empty). Several
        // MQ_UIManager windows exist (incl. a zero-size one) - require a real size to pick the real one.
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

// ===== Map drag-scroll (DGL-faithful grab+drag pan) =====
// Detours the in-game iso-view mouse handler (CStratInterf sub_48E8A0). While g_dragScroll is on, a
// left-press on open map terrain grabs the tile under the cursor; dragging pans so it stays put (like
// DGL MouseScroll). A press-release with no movement is replayed so a plain click still selects.
// Russobit addresses RE'd from the DGL backup + game exe. Game calls are __thiscall via typedefs
// (this file is /Gd); every game deref is SEH + isUserPtr guarded.
struct PointI { int x, y; };
struct RectI { int left, top, right, bottom; };

using IsoMouseFn    = int   (__fastcall*)(void* view, void* edx, int msgId, PointI* pt); // __thiscall target
using ScreenToMapFn = int   (__thiscall*)(void* mg, PointI* screen, PointI* outTile, PointI* outPx);
using ScrollTileFn  = void  (__thiscall*)(void* mg, PointI* tile, int extraDx, int extraDy);
using GetViewRectFn = RectI*(__thiscall*)(void* mg);

void* g_origIsoMouse = nullptr; // Detours trampoline to the original CStratInterf iso mouse handler
void* g_origScrollDir = nullptr; // trampoline to the game's directional map scroll (edge-scroll executor)
int g_scrollDirDiag = 0;         // first-N diagnostic counter for the edge-scroll hook
bool g_dragScrollActive = false;
extern "C" int g_c4dll_dragActive = 0; // winapi_hooks reads this: 1 = suppress game edge-scroll while dragging
bool g_dragMoved = false;
PointI g_dragAnchorTile{}; // map tile grabbed at drag start
PointI g_dragStart{};      // cursor at drag start (to detect movement)
bool g_dragAnchorSet = false; // anchor grabbed on the FIRST WndProc move (same transform as the pans)

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

// Pan the iso view so the grabbed tile lands at the cursor, pixel-precise. The game's center-on-tile
// (sub_541588) snaps scroll to the iso grid ctx[7]xctx[8], so a continuous drag steps by the grid (the
// jerk). Temporarily set the grid to 1 around the pan, then restore so click-to-center keeps its snap.
// Deref mg->engine2d->e0->ctx, grid @ctx+28/+32; isUserPtr-guarded.
void panTileSmooth(void* mg, PointI* tile, int dx, int dy)
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
            reinterpret_cast<ScrollTileFn>(0x541588)(mg, tile, dx, dy);
        } __finally {
            *gx = sx; *gy = sy;
        }
    } else {
        reinterpret_cast<ScrollTileFn>(0x541588)(mg, tile, dx, dy);
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
            PointI tile{};
            // screen->map returns false over the ~160px minimap/resource corner -> only grab real map
            if (mg && reinterpret_cast<ScreenToMapFn>(0x5418BA)(mg, pt, &tile, nullptr)) {
                // Over the map -> start a drag. Anchor tile is grabbed on the first WndProc move so it
                // uses the SAME client->game transform as the pans (no jump on the first move).
                g_dragStart = *pt;
                g_dragScrollActive = true;
                // NOTE: g_c4dll_dragActive (centering fake_GetCursorPos to kill edge-scroll) made the pan
                // jerk (game reacts to the centered cursor every frame). Left OFF; a surgical edge-scroll
                // disable needs the game's edge-scroll fn, not the cursor read.
                g_dragMoved = false;
                g_dragAnchorSet = false;
                // Capture the mouse so WM_MOUSEMOVE keeps reaching this handler while the button is held -
                // the game stops routing moves to the iso handler during a held button (DGL captured too).
                if (g_gameHwnd)
                    SetCapture(g_gameHwnd);
                return 1; // consume the down so the game does not select yet
            }
        } else if (g_dragScrollActive && msgId == WM_MOUSEMOVE) {
            if (pt->x != g_dragStart.x || pt->y != g_dragStart.y)
                g_dragMoved = true;
            void* mg = mapGraphicsPtr();
            if (mg) {
                RectI* vr = reinterpret_cast<GetViewRectFn>(0x56B8DF)(mg);
                if (isUserPtr(vr)) {
                    // place the grabbed tile under the cursor: tileScreen = viewLeft + halfW - 32 - extraDx
                    const int cx = vr->left + (vr->right - vr->left) / 2 - 32;
                    const int cy = vr->top + (vr->bottom - vr->top) / 2 - 16;
                    panTileSmooth(mg, &g_dragAnchorTile, cx - pt->x, cy - pt->y);
                }
            }
            return 1; // consume moves while panning
        } else if (g_dragScrollActive && msgId == WM_LBUTTONUP) {
            g_dragScrollActive = false;
            g_c4dll_dragActive = 0; // re-enable edge-scroll
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
        g_c4dll_dragActive = 0;
        return 1; // transient torn-down view mid dialog-transition: swallow, do NOT re-dispatch (re-crashes)
    }
    return callOrigIsoMouse(view, msgId, pt);
}

// WndProc-driven pan (called from wndProcHook with the cursor already mapped to GAME coords). The iso
// vtable handler stops receiving moves while the button is held; the captured WndProc does not.
void dragScrollWndMove(int gameX, int gameY)
{
    g_dragMoved = true; // a real drag (the button-up will not replay a select-click)
    __try {
        void* mg = mapGraphicsPtr();
        if (!mg)
            return;
        if (!g_dragAnchorSet) {
            // Grab the anchor from the first move's mapped cursor so anchor and pans use the identical
            // client->game transform -> the grabbed tile stays put (no first-move jump).
            PointI gp{ gameX, gameY }, tile{};
            if (reinterpret_cast<ScreenToMapFn>(0x5418BA)(mg, &gp, &tile, nullptr)) {
                g_dragAnchorTile = tile;
                g_dragAnchorSet = true;
            }
            return; // first move only establishes the anchor; do not pan yet
        }
        RectI* vr = reinterpret_cast<GetViewRectFn>(0x56B8DF)(mg);
        if (!isUserPtr(vr))
            return;
        // place the grabbed tile under the cursor: tileScreen = viewLeft + halfW - 32 - extraDx
        const int cx = vr->left + (vr->right - vr->left) / 2 - 32;
        const int cy = vr->top + (vr->bottom - vr->top) / 2 - 16;
        panTileSmooth(mg, &g_dragAnchorTile, cx - gameX, cy - gameY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The game's iso DIRECTIONAL scroll: sub_54249C -> sub_541BC1 -> sub_54301B. Single choke point for
// window-EDGE scroll (sub_541BC1 reached only through here). Programmatic centering uses sub_541588, so
// gating this kills edge-scroll without touching click-to-center or our drag-pan. While drag-scroll is
// ON, edge-scroll must be OFF. __thiscall(self, dir) via __fastcall(ecx=self, edx, dir); installed
// unconditionally, gate is live (toggle without restart).
char __fastcall scrollDirHook(void* self, void* /*edx*/, int dir)
{
    if (g_scrollDirDiag < 12) { ++g_scrollDirDiag; mlog("[edge] scrollDir dir=%d toggle=%d", dir, g_dragScroll ? 1 : 0); }
    if (g_dragScroll)
        return 0; // drag-scroll ON -> suppress the game's edge-scroll
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

    // The menu + byte patches use Russobit-only addresses (WndProc, always-active, timeGetTime IAT,
    // battle-viewer vftable, CMidgard chain). On any other build we don't install (renderer still works).
    if (g_ver != VerRussobit) {
        mlog("[menu] unsupported game version (exe %lu bytes): menu/patches disabled", g_exeSize);
        return;
    }

    // ---- Russobit: full menu + feature install ----
    // First run: generate a commented C4menu.ini (converting any old mss32menu.ini); never touches
    // the game's own Disciple.ini / settings.lua.
    seedConfigFirstRun();

    const char* f = iniFile();
    g_alwaysActive = GetPrivateProfileIntA("menu", "alwaysActive", 0, f) != 0;
    // Split battle/map live-multiplier keys, default OFF (each context stays vanilla until opted in).
    g_battleAnimEnabled = GetPrivateProfileIntA("menu", "battleAnimEnabled", 0, f) != 0;
    g_battleAnimSpeed = GetPrivateProfileIntA("menu", "battleAnimSpeed", 5, f);
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
    g_battleAttackEnabled = GetPrivateProfileIntA("menu", "battleAttackEnabled", 0, f) != 0;
    g_battleAttackSpeed = GetPrivateProfileIntA("menu", "battleAttackSpeed", 3, f);
    if (g_battleAttackSpeed < 1)
        g_battleAttackSpeed = 1;
    if (g_battleAttackSpeed > 5)
        g_battleAttackSpeed = 5;
    g_dragScroll = GetPrivateProfileIntA("menu", "dragScroll", 0, f) != 0;

    // Apply now (DllMain, before the game's main loop / anim init). The IAT is already populated by the
    // loader, so the time-scale hook installs cleanly; the battle discriminator patches the idle vftable.
    applyAlwaysActive(g_alwaysActive);
    installTimeScaleHook();
    installBattleDiscriminator();
    timerhost_install(); // timer keystone: capture dialog/battle buttons + combat/animation state
    installDragScrollDetour(); // map grab+drag panning (gated by g_dragScroll; pass-through when off)
    applyAnimSpeed(0, g_battleAnimEnabled, g_battleAnimSpeed);
    applyAnimSpeed(1, g_mapAnimEnabled, g_mapAnimSpeed);

    // Registered window message: menuWorker posts it so the one-time relayout (DDReloadConfig ->
    // dd_SetDisplayMode restarts the render thread) runs on the GUI thread, not the worker. Register
    // BEFORE the detour + worker so both observe the same non-zero id.
    g_relayoutMsg = RegisterWindowMessageA("C4dllR_MenuRelayout");

    installWndProcDetour();

    HANDLE thread = CreateThread(nullptr, 0, &menuWorker, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    mlog("[menu] feature menu scheduled (alwaysActive=%d, battleAnim=%d/%d, mapAnim=%d/%d)",
         g_alwaysActive ? 1 : 0, g_battleAnimEnabled ? 1 : 0, g_battleAnimSpeed,
         g_mapAnimEnabled ? 1 : 0, g_mapAnimSpeed);
}
