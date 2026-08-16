/*
 * C4dll-R bounded Fast AI for the exact Russobit/MNS 3.01a executable.
 *
 * The open-source DisciplesGL implementation marks the interval between two calls to
 * CMidServer's GetQueueCommands predicate as an AI turn and gives the hidden server window more
 * WM_TIMER work while that interval is active.  This port deliberately patches the two CALL
 * sites, not their shared target: detouring 0x61BF74 globally would mark unrelated callers as AI.
 *
 * Unlike the legacy unbounded while loop, one real ThreadWindowClass WM_TIMER may synthesize at
 * most 32 dispatches and at most 3 ms of work.  The normal message loop always regains control.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" int featuremenu_debug_enabled(void);
extern "C" int pluginhost_has_server(void);

// C linkage gives the x86 inline assembler stable names.  They are internal to the DLL (there is
// no __declspec(dllexport)); the public API is the fastai_* group at the end of this file.
extern "C" {
uintptr_t g_fastai_queue_commands = 0;
volatile LONG g_fastai_turn = 0;
}

namespace {

constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t kStartCallPreferred = 0x0042C330;
constexpr uintptr_t kEndCallPreferred = 0x0042C3B8;
constexpr uintptr_t kQueueCommandsPreferred = 0x0061BF74;
constexpr DWORD kRussobitExeSize = 4187648;
constexpr DWORD kRussobitCustomIconExeSize = 4214272;
constexpr DWORD kAiMessageGraceMs = 2000;
constexpr DWORD kSliceBudgetMs = 3;
constexpr unsigned kSliceDispatchLimit = 32;

volatile LONG g_installAttempted = 0;
volatile LONG g_available = 0;
volatile LONG g_enabled = 0; // default OFF; the menu/config layer opts in explicitly
volatile LONG g_timeoutUntil = 0;
volatile LONG g_pumping = 0;
volatile LONG g_scanBusy = 0;
volatile LONG g_lastLoggedTurn = -1;
volatile LONG g_lastSliceLog = 0;
volatile PVOID g_serverHwnd = nullptr;
volatile PVOID g_serverWndProc = nullptr;
UINT g_aiMessage = 0;
DWORD g_nextWindowScan = 0;

const char* logPath()
{
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, sizeof(path));
        char* slash = strrchr(path, '\\');
        if (slash)
            slash[1] = 0;
        else
            path[0] = 0;
        char leaf[40] = {};
        wsprintfA(leaf, "C4menu-%lu.log", GetCurrentProcessId());
        lstrcatA(path, leaf);
    }
    return path;
}

void flog(const char* fmt, ...)
{
    if (!featuremenu_debug_enabled())
        return;

    char line[640] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 3, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = 0;
    size_t len = strlen(line);
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = 0;
    OutputDebugStringA(line);

    HANDLE file = CreateFileA(logPath(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(len), &written, nullptr);
        CloseHandle(file);
    }
}

DWORD currentExeSize()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, sizeof(path));
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data) || data.nFileSizeHigh != 0)
        return 0;
    return data.nFileSizeLow;
}

bool readImage(std::uint8_t*& base, IMAGE_NT_HEADERS32*& nt)
{
    base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
    if (!base)
        return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
           nt->OptionalHeader.SizeOfImage > (kQueueCommandsPreferred - kPreferredImageBase);
}

uintptr_t relocated(std::uint8_t* base, uintptr_t preferred)
{
    return reinterpret_cast<uintptr_t>(base) + preferred - kPreferredImageBase;
}

uintptr_t callTarget(uintptr_t site)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
    if (bytes[0] != 0xE8)
        return 0;
    std::int32_t displacement = 0;
    memcpy(&displacement, bytes + 1, sizeof(displacement));
    return site + 5 + displacement;
}

bool bytesEqual(uintptr_t site, const std::uint8_t* expected, size_t count)
{
    return memcmp(reinterpret_cast<const void*>(site), expected, count) == 0;
}

#if defined(_M_IX86)
extern "C" __declspec(naked) void fastaiStartTurnThunk()
{
    __asm {
        call dword ptr [g_fastai_queue_commands]
        mov dword ptr [g_fastai_turn], 1
        ret
    }
}

extern "C" __declspec(naked) void fastaiEndTurnThunk()
{
    __asm {
        call dword ptr [g_fastai_queue_commands]
        mov dword ptr [g_fastai_turn], 0
        ret
    }
}
#endif

bool encodeCall(std::uint8_t (&out)[5], uintptr_t site, const void* target)
{
    out[0] = 0xE8;
    const intptr_t distance = reinterpret_cast<uintptr_t>(target) - (site + sizeof(out));
#if INTPTR_MAX > INT32_MAX
    if (distance < INT32_MIN || distance > INT32_MAX)
        return false;
#endif
    const std::int32_t relative = static_cast<std::int32_t>(distance);
    memcpy(out + 1, &relative, sizeof(relative));
    return true;
}

bool installCallsiteHooks()
{
#if !defined(_M_IX86)
    flog("[fast-ai] disabled: Fast AI hooks require a 32-bit x86 build");
    return false;
#else
    const DWORD fileSize = currentExeSize();
    if (fileSize != kRussobitExeSize && fileSize != kRussobitCustomIconExeSize) {
        flog("[fast-ai] disabled: unsupported executable size %lu", fileSize);
        return false;
    }

    std::uint8_t* base = nullptr;
    IMAGE_NT_HEADERS32* nt = nullptr;
    if (!readImage(base, nt)) {
        flog("[fast-ai] disabled: invalid or unsupported PE32 image");
        return false;
    }

    // Context signatures include both preceding object-access calls and the predicate result test.
    // They distinguish these two semantic call sites from every other caller of 0x61BF74.
    static const std::uint8_t kStartSignature[] = {
        0xE8, 0x0E, 0x5A, 0xFD, 0xFF, 0x8B, 0xC8, 0xE8, 0xD3, 0x70, 0xFD, 0xFF,
        0x8B, 0xC8, 0xE8, 0x3F, 0xFC, 0x1E, 0x00, 0x84, 0xC0, 0x74, 0x51
    };
    static const std::uint8_t kEndSignature[] = {
        0xE8, 0x86, 0x59, 0xFD, 0xFF, 0x8B, 0xC8, 0xE8, 0x4B, 0x70, 0xFD, 0xFF,
        0x8B, 0xC8, 0xE8, 0xB7, 0xFB, 0x1E, 0x00, 0x84, 0xC0, 0x74, 0x60
    };

    const uintptr_t startCall = relocated(base, kStartCallPreferred);
    const uintptr_t endCall = relocated(base, kEndCallPreferred);
    const uintptr_t queueCommands = relocated(base, kQueueCommandsPreferred);
    const uintptr_t startSignature = startCall - 14;
    const uintptr_t endSignature = endCall - 14;

    if (!bytesEqual(startSignature, kStartSignature, sizeof(kStartSignature)) ||
        !bytesEqual(endSignature, kEndSignature, sizeof(kEndSignature)) ||
        callTarget(startCall) != queueCommands || callTarget(endCall) != queueCommands) {
        flog("[fast-ai] disabled: Russobit call-site signature mismatch "
             "(start=%#lx end=%#lx)",
             static_cast<unsigned long>(callTarget(startCall)),
             static_cast<unsigned long>(callTarget(endCall)));
        return false;
    }

    std::uint8_t startPatch[5] = {};
    std::uint8_t endPatch[5] = {};
    if (!encodeCall(startPatch, startCall, reinterpret_cast<const void*>(&fastaiStartTurnThunk)) ||
        !encodeCall(endPatch, endCall, reinterpret_cast<const void*>(&fastaiEndTurnThunk))) {
        flog("[fast-ai] disabled: thunk is outside rel32 range");
        return false;
    }

    std::uint8_t originalStart[5] = {};
    std::uint8_t originalEnd[5] = {};
    memcpy(originalStart, reinterpret_cast<const void*>(startCall), sizeof(originalStart));
    memcpy(originalEnd, reinterpret_cast<const void*>(endCall), sizeof(originalEnd));

    const uintptr_t regionStart = startCall;
    const SIZE_T regionSize = endCall + sizeof(endPatch) - regionStart;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(regionStart), regionSize,
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        flog("[fast-ai] disabled: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }

    g_fastai_queue_commands = queueCommands;
    memcpy(reinterpret_cast<void*>(startCall), startPatch, sizeof(startPatch));
    memcpy(reinterpret_cast<void*>(endCall), endPatch, sizeof(endPatch));
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(regionStart), regionSize);

    const bool verified = bytesEqual(startCall, startPatch, sizeof(startPatch)) &&
                          bytesEqual(endCall, endPatch, sizeof(endPatch));
    if (!verified) {
        memcpy(reinterpret_cast<void*>(startCall), originalStart, sizeof(originalStart));
        memcpy(reinterpret_cast<void*>(endCall), originalEnd, sizeof(originalEnd));
        g_fastai_queue_commands = 0;
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(regionStart), regionSize);
    }

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(regionStart), regionSize, oldProtect, &ignored);
    if (!verified) {
        flog("[fast-ai] disabled: transactional call-site verification failed; bytes restored");
        return false;
    }

    flog("[fast-ai] exact hooks installed (start=%#lx end=%#lx target=%#lx; default OFF)",
         static_cast<unsigned long>(startCall), static_cast<unsigned long>(endCall),
         static_cast<unsigned long>(queueCommands));
    return true;
#endif
}

bool tickBefore(DWORD now, DWORD deadline)
{
    return static_cast<LONG>(deadline - now) > 0;
}

bool aiThinking(DWORD now)
{
    const DWORD until = static_cast<DWORD>(InterlockedCompareExchange(&g_timeoutUntil, 0, 0));
    return InterlockedCompareExchange(&g_fastai_turn, 0, 0) != 0 || tickBefore(now, until);
}

WNDPROC serverWndProc()
{
    return reinterpret_cast<WNDPROC>(
        InterlockedCompareExchangePointer(const_cast<PVOID volatile*>(&g_serverWndProc), nullptr,
                                          nullptr));
}

void beginAiGrace()
{
    const DWORD deadline = GetTickCount() + kAiMessageGraceMs;
    InterlockedExchange(&g_timeoutUntil, static_cast<LONG>(deadline));
}

LRESULT dispatchOriginal(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WNDPROC original = serverWndProc();
    return original ? CallWindowProcA(original, hwnd, message, wParam, lParam)
                    : DefWindowProcA(hwnd, message, wParam, lParam);
}

void runTimerSlice(HWND hwnd, WPARAM timerId, LPARAM timerProc)
{
    if (InterlockedCompareExchange(&g_enabled, 0, 0) == 0 ||
        InterlockedCompareExchange(&g_available, 0, 0) == 0 || !pluginhost_has_server())
        return;
    if (InterlockedCompareExchange(&g_pumping, 1, 0) != 0)
        return;

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER started = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    // QPC is available on every Windows version supported by the wrapper.  Round the deadline up
    // so the slice receives the requested budget without relying on GetTickCount's coarse tick.
    const LONGLONG budgetTicks =
        (frequency.QuadPart * kSliceBudgetMs + 999) / 1000;
    const LONGLONG deadline = started.QuadPart + budgetTicks;
    unsigned dispatched = 0;
    bool stop = false;

    for (;;) {
        LARGE_INTEGER nowCounter = {};
        QueryPerformanceCounter(&nowCounter);
        if (dispatched >= kSliceDispatchLimit || nowCounter.QuadPart >= deadline ||
            InterlockedCompareExchange(&g_enabled, 0, 0) == 0 || !aiThinking(GetTickCount()))
            break;
        Sleep(0);
        MSG queued = {};
        const BOOL haveMessage = PeekMessageA(&queued, hwnd, 0, 0, PM_REMOVE);
        if (haveMessage && queued.message != WM_TIMER) {
            if (queued.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(queued.wParam));
                stop = true;
            } else {
                dispatchOriginal(hwnd, queued.message, queued.wParam, queued.lParam);
                if (queued.message == g_aiMessage)
                    beginAiGrace();
                if (queued.message == WM_CLOSE || queued.message == WM_DESTROY ||
                    queued.message == WM_NCDESTROY)
                    stop = true;
            }
        } else {
            // No work was queued (or a redundant queued timer was consumed): give the native server
            // WndProc one synthetic tick using the real timer's id/callback pair.
            dispatchOriginal(hwnd, WM_TIMER, timerId, timerProc);
        }
        ++dispatched;
        if (stop)
            break;
    }

    InterlockedExchange(&g_pumping, 0);
    if (dispatched != 0) {
        const LONG turn = InterlockedCompareExchange(&g_fastai_turn, 0, 0);
        const LONG previous = InterlockedExchange(&g_lastLoggedTurn, turn);
        LARGE_INTEGER ended = {};
        QueryPerformanceCounter(&ended);
        const DWORD elapsedUs = frequency.QuadPart > 0
                                    ? static_cast<DWORD>(
                                          (ended.QuadPart - started.QuadPart) * 1000000 /
                                          frequency.QuadPart)
                                    : 0;
        const DWORD nowMs = GetTickCount();
        const DWORD lastLog =
            static_cast<DWORD>(InterlockedCompareExchange(&g_lastSliceLog, 0, 0));
        if (previous != turn || nowMs - lastLog >= 1000) {
            InterlockedExchange(&g_lastSliceLog, static_cast<LONG>(nowMs));
            flog("[fast-ai] slice: %u dispatch(es), %lu us, turn=%ld, enabled=%ld",
                 dispatched, elapsedUs, turn,
                 InterlockedCompareExchange(&g_enabled, 0, 0));
        }
    }
}

LRESULT CALLBACK fastAiServerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // This subclass is installed only after an exact class-name and process check.  Keep the
    // runtime guard too: a stale/reused HWND must never turn an unrelated window into an AI pump.
    char className[64] = {};
    if (!GetClassNameA(hwnd, className, sizeof(className)) ||
        lstrcmpA(className, "ThreadWindowClass") != 0)
        return dispatchOriginal(hwnd, message, wParam, lParam);

    const LRESULT result = dispatchOriginal(hwnd, message, wParam, lParam);
    if (message == g_aiMessage)
        beginAiGrace();
    else if (message == WM_TIMER)
        runTimerSlice(hwnd, wParam, lParam);

    if (message == WM_NCDESTROY) {
        InterlockedCompareExchangePointer(const_cast<PVOID volatile*>(&g_serverHwnd), nullptr, hwnd);
        InterlockedExchangePointer(const_cast<PVOID volatile*>(&g_serverWndProc), nullptr);
        InterlockedExchange(&g_timeoutUntil, 0);
        InterlockedExchange(&g_fastai_turn, 0);
        flog("[fast-ai] ThreadWindowClass destroyed; pump detached");
    }
    return result;
}

struct WindowSearch
{
    DWORD processId;
    HWND found;
};

BOOL CALLBACK findServerWindow(HWND hwnd, LPARAM parameter)
{
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != search->processId)
        return TRUE;
    char className[64] = {};
    if (GetClassNameA(hwnd, className, sizeof(className)) &&
        lstrcmpA(className, "ThreadWindowClass") == 0) {
        search->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND findServerWindow()
{
    WindowSearch search = {GetCurrentProcessId(), nullptr};
    EnumWindows(&findServerWindow, reinterpret_cast<LPARAM>(&search));
    if (search.found)
        return search.found;

    // Message-only windows are not included in EnumWindows.  The stock game uses a hidden top-level
    // window, but accepting the message-only form keeps headless/server builds on the same safe path.
    for (HWND hwnd = FindWindowExA(HWND_MESSAGE, nullptr, "ThreadWindowClass", nullptr); hwnd;
         hwnd = FindWindowExA(HWND_MESSAGE, hwnd, "ThreadWindowClass", nullptr)) {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId == search.processId)
            return hwnd;
    }
    return nullptr;
}

bool isOwnedServerWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    char className[64] = {};
    return processId == GetCurrentProcessId() &&
           GetClassNameA(hwnd, className, sizeof(className)) &&
           lstrcmpA(className, "ThreadWindowClass") == 0;
}

void attachServerWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    if (InterlockedCompareExchangePointer(const_cast<PVOID volatile*>(&g_serverHwnd), nullptr,
                                          nullptr))
        return;

    char className[64] = {};
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId() ||
        !GetClassNameA(hwnd, className, sizeof(className)) ||
        lstrcmpA(className, "ThreadWindowClass") != 0)
        return;

    WNDPROC original = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(hwnd, GWLP_WNDPROC));
    if (!original || original == &fastAiServerWndProc)
        return;

    // Publish the original before installing our procedure so an immediately delivered message
    // can always forward safely.  Only the one scanner admitted by g_scanBusy reaches this block.
    InterlockedExchangePointer(const_cast<PVOID volatile*>(&g_serverWndProc),
                               reinterpret_cast<PVOID>(original));
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrA(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&fastAiServerWndProc));
    if (!previous && GetLastError() != ERROR_SUCCESS) {
        InterlockedExchangePointer(const_cast<PVOID volatile*>(&g_serverWndProc), nullptr);
        flog("[fast-ai] ThreadWindowClass subclass failed (%lu)", GetLastError());
        return;
    }
    if (previous && reinterpret_cast<WNDPROC>(previous) != original) {
        // A competing subclass appeared between Get/Set.  Chain to the actual procedure returned by
        // SetWindowLongPtr rather than bypassing it.
        InterlockedExchangePointer(const_cast<PVOID volatile*>(&g_serverWndProc),
                                   reinterpret_cast<PVOID>(previous));
    }
    InterlockedExchangePointer(const_cast<PVOID volatile*>(&g_serverHwnd), hwnd);
    flog("[fast-ai] attached to hidden ThreadWindowClass hwnd=%p thread=%lu",
         hwnd, GetWindowThreadProcessId(hwnd, nullptr));
}

} // namespace

extern "C" void fastai_install(void)
{
    if (InterlockedCompareExchange(&g_installAttempted, 1, 0) != 0)
        return;
    if (installCallsiteHooks())
        InterlockedExchange(&g_available, 1);
}

extern "C" int fastai_set_enabled(int enabled)
{
    const LONG value = (enabled && InterlockedCompareExchange(&g_available, 0, 0) != 0) ? 1 : 0;
    const LONG previous = InterlockedExchange(&g_enabled, value);
    if (!value)
        InterlockedExchange(&g_timeoutUntil, 0);
    if (previous != value)
        flog("[fast-ai] live state -> %s", value ? "ON" : "OFF");
    return static_cast<int>(value);
}

extern "C" int fastai_get_enabled(void)
{
    return InterlockedCompareExchange(&g_enabled, 0, 0) ? 1 : 0;
}

extern "C" int fastai_is_available(void)
{
    return InterlockedCompareExchange(&g_available, 0, 0) ? 1 : 0;
}

// Called cheaply from the existing game-thread housekeeping timer.  It performs no AI work: it
// only discovers/subclasses the hidden server window.  All accelerated dispatch remains on that
// window's owning thread and is therefore host-only and game-thread-correct.
extern "C" void fastai_pump(void)
{
    if (InterlockedCompareExchange(&g_available, 0, 0) == 0)
        return;
    if (!g_aiMessage) {
        // RegisterWindowMessage returns the process/session-wide id already used by the game when
        // both sides pass the same string; no import detour is needed.
        g_aiMessage = RegisterWindowMessageA("AIMESSAGE");
        if (!g_aiMessage) {
            flog("[fast-ai] AIMESSAGE registration failed (%lu)", GetLastError());
            return;
        }
    }

    HWND attached = reinterpret_cast<HWND>(
        InterlockedCompareExchangePointer(const_cast<PVOID volatile*>(&g_serverHwnd), nullptr,
                                          nullptr));
    if (isOwnedServerWindow(attached))
        return;
    if (attached) {
        InterlockedCompareExchangePointer(const_cast<PVOID volatile*>(&g_serverHwnd), nullptr,
                                          attached);
        InterlockedExchangePointer(const_cast<PVOID volatile*>(&g_serverWndProc), nullptr);
    }

    const DWORD now = GetTickCount();
    if (tickBefore(now, g_nextWindowScan))
        return;
    g_nextWindowScan = now + 500;
    if (!pluginhost_has_server())
        return;
    if (InterlockedCompareExchange(&g_scanBusy, 1, 0) != 0)
        return;
    attachServerWindow(findServerWindow());
    InterlockedExchange(&g_scanBusy, 0);
}
