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
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

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

const char* iniFile(); // defined below (C4menu.ini next to the exe)

// Diagnostics are OFF by default (no C4menu-<pid>.log noise in the game folder).
// Enable with [menu] debugLog=1 in C4menu.ini or the C4DLL_DEBUG env var.
void mlog(const char* fmt, ...)
{
    static int enabled = -1;
    if (enabled < 0) {
        char env[8] = {};
        enabled = (GetPrivateProfileIntA("menu", "debugLog", 0, iniFile()) != 0 ||
                   GetEnvironmentVariableA("C4DLL_DEBUG", env, sizeof(env)) > 0)
                      ? 1
                      : 0;
    }
    if (!enabled)
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
volatile LONG g_batViewer = 0;   // IBatViewer instance, captured by batUpdateThunk; cleared on battle end

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
        mov dword ptr [g_batViewer], ecx   // thiscall: ecx = IBatViewer instance (for per-unit burst)
        jmp dword ptr [g_batUpdateOrig]
    }
}
__declspec(naked) void batShowThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 1
        mov dword ptr [g_attackPulse], 1
        mov dword ptr [g_batViewer], ecx   // slot[2] DOES fire (slot[1] doesn't); same IBatViewer this
        jmp dword ptr [g_batShowOrig]
    }
}
__declspec(naked) void batEndThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 0
        mov dword ptr [g_batViewer], 0
        jmp dword ptr [g_batEndOrig]
    }
}
__declspec(naked) void batDtorThunk()
{
    __asm {
        mov dword ptr [g_inBattle], 0
        mov dword ptr [g_batViewer], 0
        jmp dword ptr [g_batDtorOrig]
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
    kIdSingleCpu = 0xA164, // ddraw.ini singlecpu toggle (experimental, restart)
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
    kIdAtk6 = 0xA1E7, // 15x (test)
    kIdPerUnit = 0xA1E8, // toggle: attack burst speeds ONLY acting units (experimental)
    kIdDialogVo = 0xA1E9, // toggle: auto-close voiced event popups after VO (logs dialog-vo-log.txt)
    kIdDialogVoInfo = 0xA1EA, // disabled info line: where the voiced-dialog log is written
    kIdLast = 0xA1FF, // upper bound of our WM_COMMAND id block
};

// --- menu language (EN/RU). [menu] language = auto|en|ru; auto = Russian when the Windows UI
// language is Russian or the system codepage is 1251 (the Russobit audience). Wide strings +
// AppendMenuW keep the Cyrillic correct on any system codepage.
bool g_ru = false;
static const wchar_t* L(const wchar_t* en, const wchar_t* ru)
{
    return g_ru ? ru : en;
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
// Output window size (ddraw.ini width/height). 0,0 = native game size (1024x768); larger upscales.
struct ResOpt
{
    const wchar_t* en;
    const wchar_t* ru;
    int w, h;
};
const ResOpt kRes[] = {
    {L"Native (game size)", L"Родное (размер игры)", 0, 0},
    {L"640 x 480", L"640 x 480", 640, 480},
    {L"800 x 600", L"800 x 600", 800, 600},
    {L"1152 x 864", L"1152 x 864", 1152, 864},
    {L"1280 x 960", L"1280 x 960", 1280, 960},
    {L"1280 x 720 (16:9)", L"1280 x 720 (16:9)", 1280, 720},
    {L"1366 x 768 (16:9)", L"1366 x 768 (16:9)", 1366, 768},
    {L"1600 x 1200", L"1600 x 1200", 1600, 1200},
    {L"1600 x 900 (16:9)", L"1600 x 900 (16:9)", 1600, 900},
    {L"1920 x 1440", L"1920 x 1440", 1920, 1440},
    {L"1920 x 1080 (16:9)", L"1920 x 1080 (16:9)", 1920, 1080},
    {L"2560 x 1440 (16:9)", L"2560 x 1440 (16:9)", 2560, 1440},
    {L"3840 x 2160 (16:9)", L"3840 x 2160 (16:9)", 3840, 2160}};
const int kResCount = 13; // 0xA170..0xA17C (room before kIdFpsBase)
const int kFpsValues[] = {-1, 30, 60, 144};
const wchar_t* kFpsLabelsEn[] = {L"Monitor refresh rate", L"30", L"60", L"144"};
const wchar_t* kFpsLabelsRu[] = {L"Частота монитора", L"30", L"60", L"144"};
const int kFpsCount = 4;
// Display mode = ddraw.ini windowed/fullscreen pair. Windowed keeps caption + menu; fullscreen modes
// are borderless (menu bar stays). Exclusive does a real mode change, falls back to borderless (RDP).
struct ModeOpt
{
    const wchar_t* en;
    const wchar_t* ru;
    const char* windowed;
    const char* fullscreen;
};
const ModeOpt kModes[] = {
    {L"Windowed - title bar + menu, draggable",
     L"Оконный - заголовок + меню, окно можно таскать", "true", "false"},
    {L"Fullscreen borderless - fills screen, alt-tab friendly",
     L"Полный экран без рамки - весь экран, дружит с alt-tab", "true", "true"},
    {L"Fullscreen exclusive - real mode change (local only)",
     L"Полный экран эксклюзивный - реальная смена режима (не для RDP)", "false", "true"}};
const int kModeCount = 3;

bool g_alwaysActive = false;
bool g_battleAnimEnabled = false; // BATTLE live anim multiplier
int g_battleAnimSpeed = 5;
bool g_mapAnimEnabled = false;    // MAP live anim multiplier
int g_mapAnimSpeed = 5;
bool g_battleAttackEnabled = false; // BATTLE attack burst: speed up only while a hit/effect plays
int g_battleAttackSpeed = 3;        // burst factor (1..5); idle stays at the battle base (vanilla unless set)
DWORD g_attackExpiryTick = 0;       // burst window end (GetTickCount ms); 0 = no burst pending
bool g_perUnitBurst = false;        // EXPERIMENTAL: scale only the acting animators' interval (not the global clock)
// cnc-ddraw (ddraw.ini) state, read at startup so the menu shows current values (-1 = unknown/custom)
int g_rendererIdx = 0;  // index into kRenderers
int g_shaderIdx = -1;   // index into kShaders
bool g_maintas = false, g_vsync = false, g_boxing = false;
bool g_singlecpu = true; // ddraw.ini singlecpu (cnc-ddraw default true); OFF = experimental
bool g_dragScroll = false; // grab+drag map panning (ini [menu] dragScroll, default off)
bool g_dialogVoSkip = false; // auto-close voiced event popups after VO + log their text (default off)
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
    WritePrivateProfileStringA("menu", "perUnitBurst", g_perUnitBurst ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "dragScroll", g_dragScroll ? "1" : "0", f);
    WritePrivateProfileStringA("menu", "dialogVoSkip", g_dialogVoSkip ? "1" : "0", f);
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
    // Defaults: battle animation ON at 2x, map animation OFF. (Old single-global "on" -> battle speed 1.)
    const int battleEn = 1;
    const int mapEn = 0;
    const int bSp = oldAnimOn ? 1 : 2; // battle default 2x
    const int mSp = 2;

    char buf[2048];
    const int n = wsprintfA(buf,
        "; C4dll-R menu settings (auto-generated on first run).\r\n"
        "; Edit by hand, or use the in-game \"Game\" menu - changes are saved back here.\r\n"
        "; SEPARATE from the game's own Disciple.ini / Scripts\\settings.lua, which C4dll-R\r\n"
        "; never modifies on its own.\r\n"
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
        "; Auto-close event dialogs that have a voiceover, once the VO finishes (streamer aid).\r\n"
        "; Their text is appended to dialog-vo-log.txt in the game folder.  0 = off (default), 1 = on.\r\n"
        "dialogVoSkip=0\r\n"
        "\r\n"
        "; Write C4menu-<pid>.log diagnostics next to the exe.  0 = off (default), 1 = on.\r\n"
        "debugLog=0\r\n",
        aa, battleEn, bSp, mapEn, mSp);

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
    g_singlecpu = readDdrawBool("singlecpu", true);

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

// Eased burst factor (x10): from the given idle base, jumps to the attack factor during the hold window,
// then eases linearly back over kAttackRampMs. Used by both the global-clock burst and the per-unit burst.
int easedBurstFactor(int idle)
{
    if (!g_inBattle || !g_attackExpiryTick)
        return idle;
    const int atk = kAnimFactor[g_battleAttackSpeed - 1];
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

// True while a hit/effect is playing (attack-pulse window: hold + ramp). Used to keep attacks at full speed
// while idle is slowed between hits. Reliable signal (sub_639743 slot[2] -> g_attackExpiryTick).
static bool inAttackWindow()
{
    return g_attackExpiryTick && static_cast<int>(GetTickCount() - g_attackExpiryTick) < static_cast<int>(kAttackRampMs);
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
    // Each hit/effect (vftable slot[2] -> g_attackPulse) opens a window during which the battle clock runs at
    // the attack-burst factor, then eases back to the "Battle animation" base. Run from the 32ms WM_TIMER pump.
    if (g_attackPulse) {
        g_attackPulse = 0;
        g_attackExpiryTick = GetTickCount() + kAttackHoldMs; // hold end; ramp runs for kAttackRampMs after it
    }
    if (!g_battleAttackEnabled)
        return; // global g_battleFactor left to applyAnimSpeed (the live battle multiplier)
    g_battleFactor = easedBurstFactor(g_battleAnimEnabled ? kAnimFactor[g_battleAnimSpeed - 1] : 10);
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

void refreshChecks()
{
    if (!g_gameMenu)
        return;
    // Game
    CheckMenuItem(g_gameMenu, kIdAlwaysActive,
                  MF_BYCOMMAND | (g_alwaysActive ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_gameMenu, kIdDragScroll,
                  MF_BYCOMMAND | (g_dragScroll ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_gameMenu, kIdDialogVo,
                  MF_BYCOMMAND | (g_dialogVoSkip ? MF_CHECKED : MF_UNCHECKED));
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
    if (g_perfMenu)
        CheckMenuItem(g_perfMenu, kIdSingleCpu,
                      MF_BYCOMMAND | (g_singlecpu ? MF_CHECKED : MF_UNCHECKED));
}

void onMenuCommand(UINT id)
{
    bool restartItem = false;
    if (id == kIdAlwaysActive) {
        g_alwaysActive = !g_alwaysActive;
        applyAlwaysActive(g_alwaysActive);
    } else if (id == kIdDragScroll) {
        g_dragScroll = !g_dragScroll; // live: the detour reads this flag (persist() saves it)
    } else if (id == kIdDialogVo) {
        g_dialogVoSkip = !g_dialogVoSkip; // live: the detours read this flag (persist() saves it)
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
    } else if (id == kIdSingleCpu) {
        // Experimental: affinity is applied once in dd_CreateEx, so a game restart is required.
        g_singlecpu = !g_singlecpu;
        writeDdrawBool("singlecpu", g_singlecpu);
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
        dvoPoll();           // auto-close a voiced event popup once its VO has finished
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

    // ===== "Game" - gameplay / animation =====
    g_gameMenu = CreatePopupMenu();
    AppendMenuW(g_gameMenu, MF_STRING, kIdAlwaysActive,
                L(L"Always active - keep playing when the window loses focus",
                  L"Всегда активна - игра не встаёт на паузу без фокуса"));
    AppendMenuW(g_gameMenu, MF_STRING, kIdDragScroll,
                L(L"Map drag-scroll - hold left button to pan the map",
                  L"Перетаскивание карты - зажать левую кнопку и тянуть"));
    AppendMenuW(g_gameMenu, MF_STRING, kIdDialogVo,
                L(L"Skip voiced event dialogs - auto-close after the voiceover",
                  L"Пропускать озвученные диалоги - авто-закрытие после озвучки"));
    AppendMenuW(g_gameMenu, MF_STRING | MF_GRAYED, kIdDialogVoInfo,
                L(L"    (their text is saved to dialog-vo-log.txt in the game folder)",
                  L"    (их текст пишется в dialog-vo-log.txt в папке игры)"));
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
    AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_battleAnimMenu),
                L(L"Battle speed (whole battle)", L"Скорость боя (весь бой)"));
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
    AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_battleAtkMenu),
                L(L"Attack speed-up (burst on each hit)", L"Ускорение атак (рывок на каждый удар)"));
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
    AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_mapAnimMenu),
                L(L"Map animation speed", L"Скорость анимаций карты"));
    g_battleMenu = CreatePopupMenu();
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle1, L(L"Slow", L"Медленно"));
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle2, L(L"Normal", L"Нормально"));
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle3, L(L"Fast", L"Быстро"));
    AppendMenuW(g_battleMenu, MF_STRING, kIdBattle4, L(L"Instant", L"Мгновенно"));
    AppendMenuW(g_battleMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_battleMenu, MF_STRING | MF_GRAYED, 0,
                L(L"The game's own option; applies from the next battle.",
                  L"Родная опция игры; действует со следующего боя."));
    AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_battleMenu),
                L(L"Battle speed (game option)", L"Скорость боя (опция игры)"));
    g_mapMenu = CreatePopupMenu();
    AppendMenuW(g_mapMenu, MF_STRING, kIdMap1, L(L"Normal", L"Нормально"));
    AppendMenuW(g_mapMenu, MF_STRING, kIdMap2, L(L"Fast", L"Быстро"));
    AppendMenuW(g_mapMenu, MF_STRING, kIdMap3, L(L"Very fast", L"Очень быстро"));
    AppendMenuW(g_mapMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_mapMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Walk speed of your and enemy stacks (the game's own option).",
                  L"Скорость шага ваших и вражеских отрядов (родная опция игры)."));
    AppendMenuW(g_gameMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_mapMenu),
                L(L"Map movement speed (game option)",
                  L"Скорость передвижения на карте (опция игры)"));

    // ===== "Video" - look (all live except Renderer) =====
    g_videoMenu = CreatePopupMenu();
    g_modeMenu = CreatePopupMenu();
    for (int i = 0; i < kModeCount; ++i)
        AppendMenuW(g_modeMenu, MF_STRING, kIdModeWindowed + i, g_ru ? kModes[i].ru : kModes[i].en);
    AppendMenuW(g_modeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_modeMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Hotkey: Alt+Enter toggles fullscreen",
                  L"Горячая клавиша: Alt+Enter переключает полный экран"));
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_modeMenu),
                L(L"Display mode", L"Режим экрана"));
    g_resMenu = CreatePopupMenu();
    for (int i = 0; i < kResCount; ++i)
        AppendMenuW(g_resMenu, MF_STRING, kIdResBase + i, g_ru ? kRes[i].ru : kRes[i].en);
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_resMenu),
                L(L"Resolution", L"Разрешение"));
    g_shaderMenu = CreatePopupMenu();
    for (int i = 0; i < kShaderCount; ++i)
        AppendMenuW(g_shaderMenu, MF_STRING, kIdShaderBase + i,
                    g_ru ? kShaders[i].ru : kShaders[i].en);
    AppendMenuW(g_shaderMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_shaderMenu, MF_STRING | MF_GRAYED, 0,
                L(L"OpenGL renderer only", L"Только для рендерера OpenGL"));
    AppendMenuW(g_videoMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_shaderMenu),
                L(L"Filter / upscale", L"Фильтр / масштабирование"));
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
    AppendMenuW(g_videoMenu, MF_STRING, kIdMaintas,
                L(L"Keep 4:3 aspect - no stretch on widescreen",
                  L"Держать 4:3 - без растягивания на широких экранах"));
    AppendMenuW(g_videoMenu, MF_STRING, kIdVsync,
                L(L"VSync - fixes tearing in exclusive fullscreen (a bit more lag)",
                  L"VSync - лечит разрывы в эксклюзивном фулскрине (чуть больше задержка)"));
    AppendMenuW(g_videoMenu, MF_STRING, kIdBoxing,
                L(L"Integer scaling - pixel-perfect with borders (OFF = fill window)",
                  L"Целочисленный масштаб - пиксель-в-пиксель с рамками (OFF = заполнять окно)"));
    AppendMenuW(g_videoMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_videoMenu, MF_STRING, kIdScreenshot,
                L(L"Take screenshot (PrintScreen)", L"Сделать скриншот (PrintScreen)"));

    // ===== "Performance" - frame/CPU caps (apply on restart) =====
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
                L(L"Single CPU core (restart) - old-game safety net",
                  L"Одно ядро CPU (рестарт) - страховка для старых игр"));
    AppendMenuW(g_perfMenu, MF_STRING | MF_GRAYED, 0,
                L(L"Experimental: OFF frees the game from core 0; back ON if sound stutters",
                  L"Экспериментально: OFF снимает привязку к ядру 0; верните ON при заикании звука"));

    g_bar = CreateMenu();
    AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_gameMenu), L(L"Game", L"Игра"));
    AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_videoMenu), L(L"Video", L"Видео"));
    AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_perfMenu),
                L(L"Performance", L"Производительность"));

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
        AppendMenuW(plugins, MF_POPUP | (nNew ? 0u : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(nativeSub), L(L"Native (.c4p)", L"Нативные (.c4p)"));
        AppendMenuW(plugins, MF_POPUP | (nLegacy ? 0u : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(legacySub), L(L"Legacy (.mod)", L"Старые (.mod)"));
        AppendMenuW(g_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(plugins),
                    L(L"Plugins", L"Плагины"));
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
    // Menu language: [menu] language = auto|en|ru. auto = Russian when the Windows UI language is
    // Russian or the system codepage is 1251 (Russobit audience runs both kinds of systems).
    char lang[8] = {};
    GetPrivateProfileStringA("menu", "language", "auto", lang, sizeof(lang), f);
    if (lstrcmpiA(lang, "ru") == 0)
        g_ru = true;
    else if (lstrcmpiA(lang, "en") == 0)
        g_ru = false;
    else
        g_ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN || GetACP() == 1251;
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
    g_dragScroll = GetPrivateProfileIntA("menu", "dragScroll", 0, f) != 0;
    g_dialogVoSkip = GetPrivateProfileIntA("menu", "dialogVoSkip", 0, f) != 0;

    // Apply now (DllMain, before the game's main loop / anim init). The IAT is already populated by the
    // loader, so the time-scale hook installs cleanly; the battle discriminator patches the idle vftable.
    applyAlwaysActive(g_alwaysActive);
    installTimeScaleHook();
    installBattleDiscriminator();
    timerhost_install(); // timer keystone: capture dialog/battle buttons + combat/animation state
    installDragScrollDetour(); // map grab+drag panning (gated by g_dragScroll; pass-through when off)
    dvoInstall(); // voiced-dialog auto-skip + logger (gated by g_dialogVoSkip; pass-through when off)
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
