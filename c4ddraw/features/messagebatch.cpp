/* Bounded, default-on servicing of already queued EXE notifications, not faster
 * simulation. See docs/2026-09-04_reverse-wrapper-only-plan-report.md.
 * No MSS import, offset, data write, code patch, notification retry or CQ call.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <cstring>
#include "messagebatch.h"
#include "c4trace.h"
#ifndef C4_MESSAGEBATCH_TESTING
#include <detours.h>
#include "inventorytrace.h"
extern "C" BOOL WINAPI DDMessageBatchPeekRaw(LPMSG, HWND, UINT, UINT, UINT);
extern "C" void DDMessageBatchMapRemoved(LPMSG);
#endif

namespace {
using DispatchFn = LRESULT(WINAPI*)(const MSG*);
struct BatchOps {
    BOOL(WINAPI* peek)(LPMSG, HWND, UINT, UINT, UINT);
    BOOL(WINAPI* counter)(LARGE_INTEGER*);
    void (*mapRemoved)(LPMSG);
};
#ifdef C4_MESSAGEBATCH_TESTING
BatchOps g_ops = {&PeekMessageA, &QueryPerformanceCounter, nullptr};
#else
// Use the wrapper's original USER32 entry, including hook=2 configurations.
// fake_PeekMessageA would introduce its own limiter and map the head twice.
BatchOps g_ops = {&DDMessageBatchPeekRaw, &QueryPerformanceCounter, &DDMessageBatchMapRemoved};
#endif
volatile LONG g_enabled = 0;
DWORD g_uiThread = 0;
HWND g_mainHwnd = nullptr;
UINT g_netMessage = 0, g_queueMessage = 0;
LARGE_INTEGER g_frequency = {};
__declspec(thread) unsigned g_depth = 0;
__declspec(thread) unsigned g_epoch = 0;
__declspec(thread) unsigned g_modalMask = 0;
constexpr unsigned kExtraLimit = 32;
constexpr unsigned kBudgetUs = 1000;

enum : unsigned {
    BatchReady = 130, BatchUnavailable = 131, BatchEnter = 132,
    BatchDispatch = 133, BatchLeave = 134, BatchNativeFirst = 135
};
enum Stop : unsigned {
    Empty = 0, CountLimit = 1, TimeLimit = 2, Barrier = 3, ContextChange = 4,
    Reentry = 5, ClockFailure = 6, RemovedBarrier = 7, RemovedQuit = 8
};

struct KernelSnapshot {
    void* kernel;
    void* data;
    void* pair;
    void* controller;
    void* controllerVft;
    HWND hwnd;
    unsigned char pairFlag;
};

bool snapshot(void* kernel, KernelSnapshot* out)
{
    if (!kernel) return false;
    __try {
        // CMqUIKernelSimpleData from the stable EXE, not a mod-private struct.
        auto* data = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(kernel) + 4);
        if (!data) return false;
        void* pair = *reinterpret_cast<void**>(data + 4);
        void* controller = *reinterpret_cast<void**>(data + 0x58);
        if (!pair || !controller) return false;
        HWND hwnd = *static_cast<HWND*>(pair);
        void* vft = *static_cast<void**>(controller);
        if (!hwnd || !vft || hwnd != g_mainHwnd) return false;
        const unsigned char pairFlag = *(static_cast<unsigned char*>(pair) + sizeof(HWND));
        *out = {kernel, data, pair, controller, vft, hwnd, pairFlag};
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    DWORD pid = 0;
    return IsWindow(out->hwnd) && IsWindowEnabled(out->hwnd) && !IsIconic(out->hwnd) &&
           GetWindowThreadProcessId(out->hwnd, &pid) == g_uiThread && pid == GetCurrentProcessId();
}

bool stable(const KernelSnapshot& before, unsigned epoch)
{
    if (g_epoch != epoch || g_modalMask || g_depth != 1) return false;
    KernelSnapshot now = {};
    if (!snapshot(before.kernel, &now) ||
        now.data != before.data || now.pair != before.pair ||
        now.controller != before.controller || now.controllerVft != before.controllerVft ||
        now.hwnd != before.hwnd || now.pairFlag != before.pairFlag)
        return false;
    GUITHREADINFO gui = {sizeof(gui)};
    return GetGUIThreadInfo(g_uiThread, &gui) &&
           !(gui.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE));
}

bool eligible(const MSG& msg, HWND hwnd)
{
    return msg.hwnd == hwnd && msg.message >= 0xC000 &&
           (msg.message == g_netMessage || msg.message == g_queueMessage);
}

bool sameMessage(const MSG& a, const MSG& b)
{
    return a.hwnd == b.hwnd && a.message == b.message && a.wParam == b.wParam &&
           a.lParam == b.lParam && a.time == b.time && a.pt.x == b.pt.x && a.pt.y == b.pt.y;
}

} // namespace

/* Return 1 only if an unexpected WM_QUIT was actually removed. The thunk then
 * uses the native quit epilogue with its original stack MSG/wParam. Normally
 * quit is a PM_NOREMOVE barrier and remains entirely owned by the native loop.
 * Real Peek may execute sent callbacks between our two calls; if they change
 * the head, NEVER discard/repost an already removed message or filter past it.
 */
int __stdcall messagebatch_dispatch(MSG* first, DispatchFn original, void* kernel)
{
    const DWORD entryError = GetLastError();
    const bool enabled = InterlockedCompareExchange(&g_enabled, 0, 0) != 0 &&
                         GetCurrentThreadId() == g_uiThread;
    KernelSnapshot before = {};
    const unsigned epoch = g_epoch;
    const bool candidate = enabled && !g_depth && !g_modalMask &&
                           g_netMessage >= 0xC000 && g_queueMessage >= 0xC000 &&
                           g_netMessage != g_queueMessage && snapshot(kernel, &before);
    if (g_depth) ++g_epoch; // invalidates any outer batch, even after this nested loop exits
    ++g_depth;             // includes the FIRST dispatch, not just the extra messages
    DWORD firstError = entryError;
    unsigned extra = 0;
    Stop stop = Empty;
    int quit = 0;
    LARGE_INTEGER started = {}, ended = {};
    bool measured = false;
    __try {
        SetLastError(entryError);
        original(first); // exactly once, unchanged MSG, including nested/unsupported contexts
        firstError = GetLastError();
        if (enabled)
            c4trace_event(BatchNativeFirst, reinterpret_cast<uintptr_t>(kernel),
                          first->message, g_depth, epoch, g_epoch);
        if (!candidate || !stable(before, epoch)) {
            stop = ContextChange;
            __leave;
        }
        if (g_frequency.QuadPart <= 0 || !g_ops.counter(&started)) {
            stop = ClockFailure;
            __leave;
        }
        measured = true;
        const LONGLONG budget = (g_frequency.QuadPart * kBudgetUs + 999999) / 1000000;
        c4trace_event(BatchEnter, reinterpret_cast<uintptr_t>(kernel),
                      first->message, kExtraLimit, kBudgetUs, epoch);
        for (;;) {
            LARGE_INTEGER now = {};
            if (extra >= kExtraLimit) { stop = CountLimit; break; }
            if (!g_ops.counter(&now) || now.QuadPart < started.QuadPart) { stop = ClockFailure; break; }
            if (now.QuadPart - started.QuadPart >= budget) { stop = TimeLimit; break; }
            if (!stable(before, epoch)) { stop = ContextChange; break; }
            MSG peeked = {};
            if (!g_ops.peek(&peeked, nullptr, 0, 0, PM_NOREMOVE)) { stop = Empty; break; }
            // Peek may have dispatched sent messages. Recheck before removal.
            if (!stable(before, epoch)) { stop = ContextChange; break; }
            if (!eligible(peeked, before.hwnd)) { stop = Barrier; break; }
            // PM_NOREMOVE may itself execute a long sent callback. Do not begin
            // another removal after that callback has exhausted our budget.
            if (!g_ops.counter(&now) || now.QuadPart < started.QuadPart) { stop = ClockFailure; break; }
            if (now.QuadPart - started.QuadPart >= budget) { stop = TimeLimit; break; }
            MSG removed = {};
            if (!g_ops.peek(&removed, nullptr, 0, 0, PM_REMOVE)) { stop = Empty; break; }
            const bool stillStable = stable(before, epoch);
            const bool unchangedHead = sameMessage(peeked, removed);
            if (removed.message == WM_QUIT) {
                *first = removed;
                stop = RemovedQuit;
                quit = 1;
                break;
            }
            const bool allowed = eligible(removed, before.hwnd);
            // Do not log ordinary input payloads in the exceptional changed-head path.
            c4trace_event(BatchDispatch, reinterpret_cast<uintptr_t>(removed.hwnd),
                          removed.message, allowed ? removed.wParam : 0,
                          allowed ? static_cast<uintptr_t>(removed.lParam) : 0, extra + 1);
            // Keep the normal wrapper mapping exactly once, including an input
            // message unexpectedly removed after a sent callback changed the head.
            if (g_ops.mapRemoved) g_ops.mapRemoved(&removed);
            original(&removed); // true DispatchMessage, never a direct WndProc shortcut
            ++extra;
            if (!stillStable || !stable(before, epoch)) { stop = ContextChange; break; }
            if (!unchangedHead || !allowed) { stop = RemovedBarrier; break; }
        }
    } __finally {
        --g_depth;
        if (enabled) {
            unsigned long long ticks = 0;
            if (measured && g_ops.counter(&ended) && ended.QuadPart >= started.QuadPart)
                ticks = static_cast<unsigned long long>(ended.QuadPart - started.QuadPart);
            c4trace_event(BatchLeave, reinterpret_cast<uintptr_t>(kernel), extra, stop,
                          static_cast<uintptr_t>(ticks), AbnormalTermination() ? 1 : 0);
        }
        // Preserve the first native call's observed LastError; instrumentation,
        // context guards and extra API calls must not leak theirs into the loop.
        SetLastError(firstError);
    }
    return quit;
}

extern "C" void messagebatch_window_event(HWND hwnd, UINT msg, WPARAM wp)
{
    if (!g_enabled || GetCurrentThreadId() != g_uiThread || hwnd != g_mainHwnd) return;
    switch (msg) {
    case WM_ENTERMENULOOP: g_modalMask |= 1; ++g_epoch; break;
    case WM_EXITMENULOOP: g_modalMask &= ~1u; ++g_epoch; break;
    case WM_ENTERSIZEMOVE: g_modalMask |= 2; ++g_epoch; break;
    case WM_EXITSIZEMOVE: g_modalMask &= ~2u; ++g_epoch; break;
    case WM_ENABLE: case WM_ACTIVATE: case WM_ACTIVATEAPP: case WM_SHOWWINDOW:
    case WM_CLOSE: case WM_DESTROY: case WM_NCDESTROY:
    case WM_QUERYENDSESSION: case WM_ENDSESSION: ++g_epoch; break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0u) == SC_CLOSE || (wp & 0xFFF0u) == SC_MINIMIZE) ++g_epoch;
        break;
    }
    if (msg == WM_NCDESTROY) InterlockedExchange(&g_enabled, 0);
}

#ifndef C4_MESSAGEBATCH_TESTING
namespace {
volatile LONG g_installState = 0; // 0 untested; 1 in progress; 2 installed/off/rejected
PVOID g_unusedTrampoline = reinterpret_cast<PVOID>(0x562972);
uintptr_t g_resume = 0x562979;
uintptr_t g_quit = 0x5629B8;

#if defined(_M_IX86)
__declspec(naked) void batchThunk()
{
    __asm {
        lea eax, [esp+14h] // before ANY pushes: native local MSG
        push esi          // kernel
        push ebp          // original DispatchMessage loaded by this native loop
        push eax          // original MSG
        call messagebatch_dispatch
        test eax, eax
        jnz sawQuit
        jmp dword ptr [g_resume]
    sawQuit:
        jmp dword ptr [g_quit]
    }
}
#endif

bool exactSites()
{
    // Include BOTH neighbours: the zero-Peek branch targets 562979. It must
    // stay intact; hooking FF D5 at562977 with five bytes would corrupt it.
    const unsigned char dispatch[] = {
        0x85,0xC0,0x74,0x0E,
        0x83,0x7C,0x24,0x18,0x12,0x74,0x46,0x8D,0x44,0x24,0x14,0x50,
        0xFF,0xD5,0x8B,0x46,0x04,0x33,0xC9,0x39,0x58,0x04
    };
    const unsigned char update[] = {0x8B,0x10,0x8B,0xC8,0xFF,0x52,0x4C,0x84,0xC0};
    __try {
        return !memcmp(reinterpret_cast<void*>(0x562967), dispatch, sizeof(dispatch)) &&
               !memcmp(reinterpret_cast<void*>(0x56299F), update, sizeof(update));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

extern "C" void messagebatch_install(HWND hwnd, const char* iniPath)
{
    const DWORD savedError = GetLastError();
    if (InterlockedCompareExchange(&g_installState, 1, 0) != 0) { SetLastError(savedError); return; }
    bool on = false;
    char value[16] = {};
    GetPrivateProfileStringA("menu", "messageBatching", "1", value, sizeof(value), iniPath);
    on = !strcmp(value, "1"); // default ON; explicit 0/invalid OFF; restart-latched
    if (!on) { InterlockedExchange(&g_installState, 2); SetLastError(savedError); return; }
#if defined(_M_IX86)
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!hwnd || pid != GetCurrentProcessId() || tid != GetCurrentThreadId() || !c4_exact_game_exe() || !exactSites()) {
        c4trace_event(BatchUnavailable, 0x562972, 1, tid, pid, 0);
        InterlockedExchange(&g_installState, 2);
        SetLastError(savedError);
        return;
    }
    g_netMessage = RegisterWindowMessageA("MIDGARD NETMSG");
    g_queueMessage = RegisterWindowMessageA("MQ_COMMANDQUEUE2");
    if (!g_netMessage || !g_queueMessage || !QueryPerformanceFrequency(&g_frequency) || g_frequency.QuadPart <= 0) {
        c4trace_event(BatchUnavailable, 0x562972, 2, 0, 0, 0);
        InterlockedExchange(&g_installState, 2);
        SetLastError(savedError);
        return;
    }
    g_mainHwnd = hwnd;
    g_uiThread = tid;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCSTR>(&batchThunk), &pinned)) {
        c4trace_event(BatchUnavailable, 0x562972, 5, GetLastError(), 0, 0);
        InterlockedExchange(&g_installState, 2);
        SetLastError(savedError);
        return;
    }
    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR) {
        error = DetourUpdateThread(GetCurrentThread());
        if (error == NO_ERROR && !exactSites()) error = ERROR_INVALID_DATA;
        if (error == NO_ERROR) error = DetourAttach(&g_unusedTrampoline, batchThunk);
        if (error == NO_ERROR) error = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    if (error == NO_ERROR) {
        InterlockedExchange(&g_enabled, 1);
        c4trace_event(BatchReady, 0x562972, g_netMessage, g_queueMessage, kExtraLimit, kBudgetUs);
    } else {
        c4trace_event(BatchUnavailable, 0x562972, 3, error, 0, 0);
    }
#else
    c4trace_event(BatchUnavailable, 0, 4, 0, 0, 0);
#endif
    InterlockedExchange(&g_installState, 2);
    SetLastError(savedError);
}
#endif
