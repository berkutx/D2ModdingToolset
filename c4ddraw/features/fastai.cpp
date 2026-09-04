/*
 * C4dll-R bounded Fast AI for the exact Russobit/MNS 3.01a executable.
 *
 * The open-source DisciplesGL implementation marks the interval between two calls to
 * CMidServer's GetQueueCommands predicate as an AI turn and gives the hidden server window more
 * WM_TIMER work while that interval is active.  This port deliberately patches the two CALL
 * sites, not their shared target: detouring 0x61BF74 globally would mark unrelated callers as AI.
 *
 * Unlike the legacy unbounded while loop, one real ThreadWindowClass WM_TIMER may synthesize at
 * most 32 dispatches, checking a 3 ms budget between callbacks. A native callback itself is not
 * preempted. Real queued messages retain normal Windows dispatch semantics.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "c4trace.h"
#include "eventtrace.h"

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
SRWLOCK g_serverStateLock = SRWLOCK_INIT; // Protect the pair/generation, never a window API call.
LONG g_serverGeneration = 0;
UINT g_aiMessage = 0;
DWORD g_nextWindowScan = 0;

// Trace provenance is local to this forwarding seam, not a new game policy.
enum : unsigned {
    kTraceExternal = 0,
    kTraceClassFallback = 1,
    kTraceQueued = 2,
    kTraceSyntheticEmpty = 3,
    kTraceSyntheticAfterTimer = 4 // Reserved for old traces; real queued timers are no longer replaced.
};
__declspec(thread) unsigned g_traceDispatchDepth = 0;

struct QueuedDispatchContext {
    const MSG* message;
    bool consumed;
};
__declspec(thread) QueuedDispatchContext* g_queuedDispatch = nullptr;

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

struct ServerState {
    HWND hwnd;
    WNDPROC proc;
    LONG generation;
};

ServerState serverState()
{
    AcquireSRWLockShared(&g_serverStateLock);
    const ServerState state = {reinterpret_cast<HWND>(g_serverHwnd),
                              reinterpret_cast<WNDPROC>(g_serverWndProc), g_serverGeneration};
    ReleaseSRWLockShared(&g_serverStateLock);
    return state;
}

WNDPROC serverWndProc()
{
    return serverState().proc;
}

bool currentSliceWindow(const ServerState& expected)
{
    const ServerState current = serverState();
    return expected.hwnd && expected.proc && current.hwnd == expected.hwnd &&
           current.generation == expected.generation && current.proc == expected.proc &&
           GetWindowThreadProcessId(expected.hwnd, nullptr) == GetCurrentThreadId();
}

bool clearServerWindow(HWND hwnd, LONG generation)
{
    bool cleared = false;
    AcquireSRWLockExclusive(&g_serverStateLock);
    if (g_serverHwnd == hwnd && g_serverGeneration == generation) {
        g_serverHwnd = nullptr;
        g_serverWndProc = nullptr;
        ++g_serverGeneration;
        InterlockedExchange(&g_timeoutUntil, 0);
        InterlockedExchange(&g_fastai_turn, 0);
        cleared = true;
    }
    ReleaseSRWLockExclusive(&g_serverStateLock);
    return cleared;
}

void beginAiGrace()
{
    const DWORD deadline = GetTickCount() + kAiMessageGraceMs;
    InterlockedExchange(&g_timeoutUntil, static_cast<LONG>(deadline));
}

LRESULT dispatchOriginal(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                         unsigned traceSource = kTraceExternal)
{
    WNDPROC original = serverWndProc();
    if (!c4trace_enabled())
        return original ? CallWindowProcA(original, hwnd, message, wParam, lParam)
                        : DefWindowProcA(hwnd, message, wParam, lParam);

    // Record every enabled server forward, including periods without player input. QPC lives in
    // c4trace_event: two bounded records, no extra message API, wait, allocation or file IO here.
    // Context d: low 7 bits=source above; bit7=abnormal return (RETURN only); upper24=TLS depth.
    // ENTER: object=HWND, a=message, b=wParam, c=lParam. RETURN: b=LRESULT, c=LastError.
    // Per-TID ENTER/RETURN QPC + context delimit slow calls and nested native dispatch. This is
    // a callback return, not proof that a network packet/command was processed successfully.
    const DWORD beforeError = GetLastError();
    const unsigned depth = ++g_traceDispatchDepth;
    const uintptr_t context = ((depth & 0xFFFFFFu) << 8) | traceSource;
    c4trace_event(C4TRACE_FASTAI_DISPATCH_ENTER, reinterpret_cast<uintptr_t>(hwnd),
                  message, wParam, static_cast<uintptr_t>(lParam), context);
    SetLastError(beforeError);
    LRESULT result = 0;
    __try {
        result = original ? CallWindowProcA(original, hwnd, message, wParam, lParam)
                          : DefWindowProcA(hwnd, message, wParam, lParam);
    } __finally {
        // Do not catch or convert a native exception. Restore diagnostic depth even on unwind.
        const DWORD afterError = GetLastError();
        c4trace_event(C4TRACE_FASTAI_DISPATCH_RETURN, reinterpret_cast<uintptr_t>(hwnd),
                      message, static_cast<uintptr_t>(result), afterError,
                      context | (AbnormalTermination() ? 0x80u : 0u));
        --g_traceDispatchDepth;
        SetLastError(afterError);
    }
    return result;
}

void dispatchQueued(const MSG& queued)
{
    // Mark exactly the expected server forward, not arbitrary nested/sent callbacks. TIMERPROC
    // and child-window dispatch need not reach our server WndProc and therefore have no 70/71 pair.
    QueuedDispatchContext context = {&queued, false};
    QueuedDispatchContext* previous = g_queuedDispatch;
    // A TIMERPROC can itself send the same WM_TIMER to the WndProc. That is a nested callback,
    // not the queued DispatchMessage's direct server forward, even if all four arguments match.
    g_queuedDispatch = queued.message == WM_TIMER && queued.lParam ? nullptr : &context;
    __try {
        DispatchMessageA(&queued);
    } __finally {
        g_queuedDispatch = previous;
    }
}

bool sliceBudgetRemaining(LONGLONG deadline, LONGLONG& previousCounter)
{
    LARGE_INTEGER now = {};
    if (!QueryPerformanceCounter(&now) || now.QuadPart < previousCounter)
        return false;
    previousCounter = now.QuadPart;
    return now.QuadPart < deadline;
}

void runTimerSliceBody(const ServerState& window, WPARAM timerId, LPARAM timerProc)
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER started = {};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&started))
        return;
    // QPC is available on every Windows version supported by the wrapper.  Round the deadline up
    // so the slice receives the requested budget without relying on GetTickCount's coarse tick.
    const LONGLONG budgetTicks =
        (frequency.QuadPart * kSliceBudgetMs + 999) / 1000;
    const LONGLONG deadline = started.QuadPart + budgetTicks;
    LONGLONG previousCounter = started.QuadPart;
    unsigned dispatched = 0;
    bool stop = false;

    for (;;) {
        if (dispatched >= kSliceDispatchLimit || !currentSliceWindow(window) ||
            InterlockedCompareExchange(&g_enabled, 0, 0) == 0 || !aiThinking(GetTickCount()) ||
            !sliceBudgetRemaining(deadline, previousCounter))
            break;
        Sleep(0);
        // Yielding may consume the remaining budget. Do not remove another queued message then.
        if (!currentSliceWindow(window) ||
            InterlockedCompareExchange(&g_enabled, 0, 0) == 0 || !aiThinking(GetTickCount()) ||
            !sliceBudgetRemaining(deadline, previousCounter))
            break;
        MSG queued = {};
        const BOOL haveMessage = PeekMessageA(&queued, window.hwnd, 0, 0, PM_REMOVE);
        if (haveMessage) {
            if (queued.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(queued.wParam));
                stop = true;
            } else {
                // A real queued timer keeps its own id/TIMERPROC; a child retains its own HWND.
                // Once removed, deliver it even if Peek's sent callbacks exhausted the budget.
                // Re-entering our server subclass is intentional: g_pumping prevents a nested
                // slice, while normal subclass/teardown handling still runs exactly once.
                dispatchQueued(queued);
                if (queued.hwnd == window.hwnd &&
                    (queued.message == WM_CLOSE || queued.message == WM_DESTROY ||
                     queued.message == WM_NCDESTROY))
                    stop = true;
            }
        } else {
            // Peek may execute sent callbacks even when it returns FALSE. Recheck the attachment
            // and the budget after that possible teardown/reentry before any synthetic work.
            if (!currentSliceWindow(window) ||
                InterlockedCompareExchange(&g_enabled, 0, 0) == 0 || !aiThinking(GetTickCount()) ||
                !sliceBudgetRemaining(deadline, previousCounter))
                break;
            // This is deliberately not DispatchMessage: preserve the existing native AI-tick
            // contract, using the outer real server timer's id/parameter only when no work queued.
            dispatchOriginal(window.hwnd, WM_TIMER, timerId, timerProc, kTraceSyntheticEmpty);
        }
        ++dispatched;
        if (stop || !currentSliceWindow(window))
            break;
    }

    if (dispatched != 0) {
        const LONG turn = InterlockedCompareExchange(&g_fastai_turn, 0, 0);
        const LONG previous = InterlockedExchange(&g_lastLoggedTurn, turn);
        LARGE_INTEGER ended = {};
        const BOOL haveEnded = QueryPerformanceCounter(&ended);
        const DWORD elapsedUs = haveEnded && ended.QuadPart >= started.QuadPart
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

void runTimerSlice(HWND hwnd, WPARAM timerId, LPARAM timerProc)
{
    // Preserve the outer native callback's LastError. Never convert an exception to success.
    const DWORD savedError = GetLastError();
    bool acquired = false;
    __try {
        const ServerState window = serverState();
        if (window.hwnd != hwnd || !currentSliceWindow(window) ||
            InterlockedCompareExchange(&g_enabled, 0, 0) == 0 ||
            InterlockedCompareExchange(&g_available, 0, 0) == 0 || !pluginhost_has_server())
            __leave;
        if (InterlockedCompareExchange(&g_pumping, 1, 0) != 0)
            __leave;
        acquired = true;
        runTimerSliceBody(window, timerId, timerProc);
    } __finally {
        if (acquired)
            InterlockedExchange(&g_pumping, 0);
        SetLastError(savedError);
    }
}

LRESULT CALLBACK fastAiServerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // This subclass is installed only after an exact class-name and process check.  Keep the
    // runtime guard too: a stale/reused HWND must never turn an unrelated window into an AI pump.
    const DWORD beforeError = GetLastError();
    const ServerState window = serverState();
    char className[64] = {};
    if (!GetClassNameA(hwnd, className, sizeof(className)) ||
        lstrcmpA(className, "ThreadWindowClass") != 0) {
        SetLastError(beforeError);
        return dispatchOriginal(hwnd, message, wParam, lParam, kTraceClassFallback);
    }

    unsigned source = kTraceExternal;
    if (g_queuedDispatch && !g_queuedDispatch->consumed) {
        const MSG& expected = *g_queuedDispatch->message;
        if (expected.hwnd == hwnd && expected.message == message &&
            expected.wParam == wParam && expected.lParam == lParam) {
            g_queuedDispatch->consumed = true;
            source = kTraceQueued;
        }
    }
    SetLastError(beforeError);
    LRESULT result = 0;
    __try {
        result = dispatchOriginal(hwnd, message, wParam, lParam, source);
        const DWORD nativeError = GetLastError();
        if (message == g_aiMessage && currentSliceWindow(window))
            beginAiGrace();
        else if (message == WM_TIMER)
            runTimerSlice(hwnd, wParam, lParam);
        SetLastError(nativeError);
    } __finally {
        const DWORD afterError = GetLastError();
        if (message == WM_NCDESTROY && clearServerWindow(hwnd, window.generation))
            flog("[fast-ai] ThreadWindowClass destroyed; pump detached");
        SetLastError(afterError);
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

    // Publish the complete attachment before installing the procedure. An immediate callback can
    // forward and teardown safely. No window API runs while this small metadata lock is held.
    AcquireSRWLockExclusive(&g_serverStateLock);
    if (g_serverHwnd) {
        ReleaseSRWLockExclusive(&g_serverStateLock);
        return;
    }
    g_serverWndProc = reinterpret_cast<PVOID>(original);
    g_serverHwnd = hwnd;
    const LONG generation = ++g_serverGeneration;
    ReleaseSRWLockExclusive(&g_serverStateLock);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrA(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&fastAiServerWndProc));
    if (!previous && GetLastError() != ERROR_SUCCESS) {
        clearServerWindow(hwnd, generation);
        flog("[fast-ai] ThreadWindowClass subclass failed (%lu)", GetLastError());
        return;
    }
    if (previous && reinterpret_cast<WNDPROC>(previous) != original) {
        // A competing subclass appeared between Get/Set.  Chain to the actual procedure returned by
        // SetWindowLongPtr rather than bypassing it.
        AcquireSRWLockExclusive(&g_serverStateLock);
        if (g_serverHwnd == hwnd && g_serverGeneration == generation)
            g_serverWndProc = reinterpret_cast<PVOID>(previous);
        ReleaseSRWLockExclusive(&g_serverStateLock);
    }
    const ServerState attached = serverState();
    if (attached.hwnd != hwnd || attached.generation != generation)
        return; // The window was destroyed during installation; do not resurrect the attachment.
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

    const ServerState attached = serverState();
    if (isOwnedServerWindow(attached.hwnd))
        return;
    if (attached.hwnd)
        clearServerWindow(attached.hwnd, attached.generation);

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
