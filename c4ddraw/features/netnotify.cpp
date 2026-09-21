/* Recover the lower custom-lobby notification, never a game command.
 * No MSS imports, private fields, callback addresses or code patches.
 * Native contracts: docs/network-notification-analysis.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <cstring>
#include "netnotify.h"
#include "netnotifystate.h"
#include "c4trace.h"
#ifndef C4_NETNOTIFY_TESTING
#include <detours.h>
#include "inventorytrace.h"
#include "messagebatch.h"
#endif
extern "C" int featuremenu_native_dispatch_active(void);

namespace {
using LoopFn = WPARAM(__thiscall*)(void*);
using AddFn = int(__thiscall*)(void*, UINT, void*);
using RemoveFn = void(__thiscall*)(void*, UINT, int);
using RunFn = bool(__thiscall*)(void*, DWORD*, unsigned short, WPARAM, LPARAM);
using DestroyFn = void(__thiscall*)(void*);
using DispatchFn = LRESULT(WINAPI*)(const MSG*);
using Ticket = c4net::WakeState::Ticket;
LoopFn originalLoop = reinterpret_cast<LoopFn>(0x56288A);
AddFn originalAdd = reinterpret_cast<AddFn>(0x56418D);
RemoveFn originalRemove = reinterpret_cast<RemoveFn>(0x564206);
RunFn originalRun = reinterpret_cast<RunFn>(0x5646EA);
DestroyFn originalDestroy = reinterpret_cast<DestroyFn>(0x56387D);
PVOID originalIteration = reinterpret_cast<PVOID>(0x562909);
SRWLOCK stateLock = SRWLOCK_INIT;
c4net::WakeState state;
volatile LONG earlyInstalled = 0, lateAttempted = 0, enabled = 0;
HWND mainWindow = nullptr;
DWORD uiThread = 0;
UINT customMessage = 0, privateMessage = 0;
HANDLE stopEvent = nullptr; // protected by stateLock, worker closes it on exit
__declspec(thread) unsigned loopDepth = 0, callbackDepth = 0, syntheticDepth = 0;
__declspec(thread) unsigned customCallbackDepth = 0;
__declspec(thread) void* outerKernel = nullptr;
__declspec(thread) unsigned modalMask = 0;
uint32_t posts = 0, deliveries = 0, deferrals = 0, failures = 0;
constexpr DWORD kWakePeriodMs = 1000;
enum : unsigned { Ready = 240, Unavailable = 241, Registration = 242,
                  Armed = 243, Totals = 244 };

uintptr_t controllerData(void* object)
{
    if (!object) return 0;
    __try { return *reinterpret_cast<uintptr_t*>(static_cast<char*>(object) + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool currentKernel(void* kernel, uintptr_t* controller, uintptr_t* data)
{
    __try {
        if (!kernel) return false;
        auto* kernelData = *reinterpret_cast<unsigned char**>(static_cast<char*>(kernel) + 4);
        if (!kernelData) return false;
        void* pair = *reinterpret_cast<void**>(kernelData + 4);
        if (!pair || *static_cast<HWND*>(pair) != mainWindow) return false;
        *controller = *reinterpret_cast<uintptr_t*>(kernelData + 0x58);
        *data = controllerData(reinterpret_cast<void*>(*controller));
        return *controller && *data;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safeBoundary(void* kernel)
{
    if (!enabled || GetCurrentThreadId() != uiThread || loopDepth != 1 ||
        kernel != outerKernel || callbackDepth || syntheticDepth || modalMask ||
        featuremenu_native_dispatch_active()) return false;
    DWORD process = 0;
    GUITHREADINFO gui = {sizeof(gui)};
    // Inactive/minimized clients still need network processing. Disabled or
    // system-modal windows must first return to the normal outer loop.
    return IsWindow(mainWindow) && IsWindowEnabled(mainWindow) &&
           GetWindowThreadProcessId(mainWindow, &process) == uiThread &&
           process == GetCurrentProcessId() && GetGUIThreadInfo(uiThread, &gui) &&
           !(gui.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE));
}

void cancelController(uintptr_t object)
{
    AcquireSRWLockExclusive(&stateLock);
    if (state.observed && (!object || state.controller == object)) state.cancel();
    ReleaseSRWLockExclusive(&stateLock);
}

void stopRecovery()
{
    InterlockedExchange(&enabled, 0);
    AcquireSRWLockExclusive(&stateLock);
    state.cancel();
    if (stopEvent) SetEvent(stopEvent);
    ReleaseSRWLockExclusive(&stateLock);
}

void postTicket(Ticket ticket)
{
    const BOOL posted = PostMessageA(mainWindow, privateMessage, ticket.sequence, ticket.generation);
    AcquireSRWLockExclusive(&stateLock);
    if (posted) ++posts;
    else { ++failures; state.postFailed(ticket); }
    ReleaseSRWLockExclusive(&stateLock);
}

DWORD WINAPI wakeWorker(void* argument)
{
    HANDLE stop = static_cast<HANDLE>(argument);
    unsigned samples = 0;
    for (;;) {
        const DWORD wait = WaitForSingleObject(stop, kWakePeriodMs);
        if (wait != WAIT_TIMEOUT) break;
        Ticket ticket = {};
        AcquireSRWLockExclusive(&stateLock);
        const bool post = enabled && state.reserve(&ticket);
        const bool report = enabled && state.observed && (++samples % 5 == 0);
        const uint32_t p = posts, d = deliveries, m = deferrals, f = failures;
        ReleaseSRWLockExclusive(&stateLock);
        if (post) postTicket(ticket);
        // Cumulative wrapper activity only; successful dispatch is NOT proof
        // that packets existed or that a particular match was recovered.
        if (report) c4trace_event(Totals, 0, p, d, m, f);
    }
    AcquireSRWLockExclusive(&stateLock);
    if (stopEvent == stop) stopEvent = nullptr;
    ReleaseSRWLockExclusive(&stateLock);
    CloseHandle(stop);
    return 0;
}

WPARAM __fastcall loopHook(void* kernel, void*)
{
    const DWORD entryError = GetLastError();
    const void* previousKernel = outerKernel;
    if (!loopDepth) outerKernel = kernel;
    ++loopDepth;
    WPARAM result = 0;
    DWORD resultError = entryError;
    __try {
        SetLastError(entryError);
        result = originalLoop(kernel);
        resultError = GetLastError();
    } __finally {
        --loopDepth;
        outerKernel = const_cast<void*>(previousKernel);
        uintptr_t controller = 0, data = 0;
        if (!loopDepth && GetCurrentThreadId() == uiThread && currentKernel(kernel, &controller, &data))
            stopRecovery();
        SetLastError(resultError);
    }
    return result;
}

int __fastcall addHook(void* object, void*, UINT message, void* functor)
{
    const int id = originalAdd(object, message, functor);
    const DWORD saved = GetLastError();
    if (enabled && message == customMessage) {
        const uintptr_t data = controllerData(object);
        AcquireSRWLockExclusive(&stateLock);
        const bool accepted = GetCurrentThreadId() == uiThread &&
            state.registered(reinterpret_cast<uintptr_t>(object), data, static_cast<uint32_t>(id));
        if (!accepted) state.reject();
        const uint32_t generation = state.generation;
        ReleaseSRWLockExclusive(&stateLock);
        c4trace_event(Registration, reinterpret_cast<uintptr_t>(object), message,
                      static_cast<uint32_t>(id), generation, accepted ? 1 : 0);
    }
    SetLastError(saved);
    return id; // event ID zero is valid
}

void __fastcall removeHook(void* object, void*, UINT message, int id)
{
    const DWORD saved = GetLastError();
    if (message == customMessage) {
        AcquireSRWLockExclusive(&stateLock);
        const bool removed = state.removed(reinterpret_cast<uintptr_t>(object), static_cast<uint32_t>(id));
        ReleaseSRWLockExclusive(&stateLock);
        if (removed) c4trace_event(Registration, reinterpret_cast<uintptr_t>(object), message,
                                  static_cast<uint32_t>(id), 0, 2);
    }
    SetLastError(saved);
    originalRemove(object, message, id);
}

void __fastcall destroyHook(void* object, void*)
{
    const DWORD saved = GetLastError();
    cancelController(reinterpret_cast<uintptr_t>(object));
    SetLastError(saved);
    originalDestroy(object);
}

bool __fastcall runHook(void* object, void*, DWORD* out, unsigned short message, WPARAM wp, LPARAM lp)
{
    const DWORD entryError = GetLastError();
    const bool custom = enabled && GetCurrentThreadId() == uiThread && message == customMessage;
    const bool observe = custom && !syntheticDepth && !customCallbackDepth && !wp && !lp;
    uint32_t generation = 0;
    const uintptr_t data = observe ? controllerData(object) : 0;
    if (observe) {
        AcquireSRWLockShared(&stateLock);
        if (state.matches(reinterpret_cast<uintptr_t>(object), data)) generation = state.generation;
        ReleaseSRWLockShared(&stateLock);
    }
    ++callbackDepth;
    if (custom) ++customCallbackDepth;
    bool result = true;
    DWORD resultError = entryError;
    __try {
        SetLastError(entryError);
        result = originalRun(object, out, message, wp, lp);
        resultError = GetLastError();
    } __finally {
        --callbackDepth;
        if (custom) --customCallbackDepth;
        if (AbnormalTermination()) cancelController(reinterpret_cast<uintptr_t>(object));
        else if (generation && !result && data == controllerData(object)) {
            // AL=0 alone is insufficient (lower_bound may find the next key).
            // Our exact live, unique registration must survive the whole call.
            AcquireSRWLockExclusive(&stateLock);
            const bool newlyArmed = !state.armed &&
                state.completed(reinterpret_cast<uintptr_t>(object), data, generation);
            ReleaseSRWLockExclusive(&stateLock);
            if (newlyArmed) c4trace_event(Armed, reinterpret_cast<uintptr_t>(object), message, generation, 0, 0);
        }
        SetLastError(resultError);
    }
    return result;
}
} // namespace

void __stdcall netnotify_iteration(void* kernel)
{
    const DWORD saved = GetLastError();
    if (safeBoundary(kernel)) {
        uintptr_t controller = 0, data = 0;
        const bool valid = currentKernel(kernel, &controller, &data);
        Ticket ticket = {};
        AcquireSRWLockExclusive(&stateLock);
        if (state.observed && (!valid || !state.matches(controller, data))) state.cancel();
        const bool post = state.requeue(&ticket);
        ReleaseSRWLockExclusive(&stateLock);
        if (post) postTicket(ticket);
    }
    SetLastError(saved);
}

#if defined(_M_IX86)
__declspec(naked) void netnotifyIterationThunk()
{
    __asm {
        pushfd
        pushad
        push esi
        call netnotify_iteration
        popad
        popfd
        jmp dword ptr [originalIteration]
    }
}
#endif

extern "C" LRESULT netnotify_dispatch(const MSG* message, DispatchFn original, void* kernel)
{
    const DWORD entryError = GetLastError();
    if (message->hwnd != mainWindow || !privateMessage || message->message != privateMessage)
        return original(message);
    MSG translated = *message;
    bool deliver = false;
    if (safeBoundary(kernel)) {
        uintptr_t controller = 0, data = 0;
        const bool valid = currentKernel(kernel, &controller, &data);
        AcquireSRWLockExclusive(&stateLock);
        if (state.observed && (!valid || !state.matches(controller, data))) state.cancel();
        deliver = valid && state.matches(controller, data) &&
            state.consume({static_cast<uint32_t>(message->lParam), static_cast<uint32_t>(message->wParam)});
        if (deliver) ++deliveries;
        ReleaseSRWLockExclusive(&stateLock);
    }
    if (deliver) { translated.message = customMessage; translated.wParam = 0; translated.lParam = 0; }
    DWORD resultError = entryError;
    LRESULT result = 0;
    if (deliver) ++syntheticDepth;
    __try {
        SetLastError(entryError);
        // Unsupported/modal/stale private messages are consumed/deferred by
        // our WndProc, never forwarded as native net notifications.
        result = original(deliver ? &translated : message);
        resultError = GetLastError();
    } __finally {
        if (deliver) --syntheticDepth;
        if (AbnormalTermination() && deliver) cancelController(0);
        SetLastError(resultError);
    }
    return result;
}

extern "C" int netnotify_window_event(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    const DWORD saved = GetLastError();
    const bool mine = privateMessage && message == privateMessage;
    if (hwnd == mainWindow && GetCurrentThreadId() == uiThread) {
        if (mine) {
            AcquireSRWLockExclusive(&stateLock);
            Ticket ticket = {static_cast<uint32_t>(lp), static_cast<uint32_t>(wp)};
            if (state.owns(ticket)) { state.defer(ticket); ++deferrals; }
            ReleaseSRWLockExclusive(&stateLock);
        }
        switch (message) {
        case WM_ENTERMENULOOP: modalMask |= 1; break;
        case WM_EXITMENULOOP: modalMask &= ~1u; break;
        case WM_ENTERSIZEMOVE: modalMask |= 2; break;
        case WM_EXITSIZEMOVE: modalMask &= ~2u; break;
        case WM_DESTROY: case WM_NCDESTROY: stopRecovery(); break;
        }
    }
    SetLastError(saved);
    return mine ? 1 : 0;
}

extern "C" int netnotify_requested(const char* iniPath)
{
    const DWORD saved = GetLastError();
    char setting[16] = {};
    GetPrivateProfileStringA("menu", "networkWakeRecovery", "1", setting, sizeof(setting), iniPath);
    const bool requested = !strcmp(setting, "1");
    SetLastError(saved);
    return requested ? 1 : 0;
}

#ifndef C4_NETNOTIFY_TESTING
namespace {
struct Site { uintptr_t address; const char* bytes; size_t size; PVOID* original; PVOID replacement; };
bool matchesSite(uintptr_t address, const char* bytes, size_t length)
{
    __try { return !memcmp(reinterpret_cast<void*>(address), bytes, length); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool applySites(Site* sites, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (!matchesSite(sites[i].address, sites[i].bytes, sites[i].size)) return false;
    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR) return false;
    error = DetourUpdateThread(GetCurrentThread());
    for (size_t i = 0; error == NO_ERROR && i < count; ++i) {
        if (!matchesSite(sites[i].address, sites[i].bytes, sites[i].size)) error = ERROR_INVALID_DATA;
        else error = DetourAttach(sites[i].original, sites[i].replacement);
    }
    if (error != NO_ERROR) { DetourTransactionAbort(); return false; }
    return DetourTransactionCommit() == NO_ERROR;
}
bool loadedImage()
{
    __try {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (reinterpret_cast<uintptr_t>(base) != 0x400000) return false;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
               nt->FileHeader.TimeDateStamp == 0x3FD9DBC2 && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

extern "C" void netnotify_bootstrap(void)
{
    const DWORD saved = GetLastError();
#if defined(_M_IX86)
    if (loadedImage()) {
        // Pure depth observer before EXE entry. Full file identity is checked
        // later, outside loader lock, before any active feature is enabled.
        Site site = {0x56288A, "\x83\xEC\x20\x56\x8B\xF1\x8B\x46\x04\x8B\x48\x04", 12,
                     reinterpret_cast<PVOID*>(&originalLoop), reinterpret_cast<PVOID>(&loopHook)};
        HMODULE pinned = nullptr;
        if (matchesSite(site.address, site.bytes, site.size) &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCSTR>(&loopHook), &pinned) && applySites(&site, 1))
            InterlockedExchange(&earlyInstalled, 1);
    }
#endif
    SetLastError(saved);
}

extern "C" void netnotify_install(HWND hwnd, const char* iniPath)
{
    const DWORD saved = GetLastError();
    if (InterlockedCompareExchange(&lateAttempted, 1, 0)) { SetLastError(saved); return; }
    if (!netnotify_requested(iniPath)) { SetLastError(saved); return; }
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(hwnd, &process);
    unsigned reason = 1;
#if defined(_M_IX86)
    if (earlyInstalled && hwnd && thread == GetCurrentThreadId() && process == GetCurrentProcessId() &&
        c4_exact_game_exe() && messagebatch_dispatch_ready()) {
        mainWindow = hwnd; uiThread = thread;
        customMessage = RegisterWindowMessageA("MIDGARD CUSTOM LOBBY NETMSG");
        privateMessage = RegisterWindowMessageA("C4dllR.NetworkWake.v1");
        reason = 2;
        if (customMessage && privateMessage && customMessage != privateMessage) {
            HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (stop) {
                Site sites[] = {
                    {0x56418D, "\xB8\xF8\xE1\x6A\x00\xE8\x39\x92\x10\x00\x83\xEC\x1C", 13,
                     reinterpret_cast<PVOID*>(&originalAdd), reinterpret_cast<PVOID>(&addHook)},
                    {0x564206, "\xB8\x0C\xE2\x6A\x00\xE8\xC0\x91\x10\x00\x83\xEC\x24", 13,
                     reinterpret_cast<PVOID*>(&originalRemove), reinterpret_cast<PVOID>(&removeHook)},
                    {0x5646EA, "\x55\x8B\xEC\x83\xEC\x0C\x8B\x45\x08\x53\x56\x8B\xF1", 13,
                     reinterpret_cast<PVOID*>(&originalRun), reinterpret_cast<PVOID>(&runHook)},
                    {0x56387D, "\xB8\xCF\xE1\x6A\x00", 5,
                     reinterpret_cast<PVOID*>(&originalDestroy), reinterpret_cast<PVOID>(&destroyHook)},
                    {0x562909, "\x8B\x06\x8B\xCE\xFF\x50\x54\x84\xC0\x75\x05", 11,
                     &originalIteration, reinterpret_cast<PVOID>(&netnotifyIterationThunk)}
                };
                reason = 3;
                if (applySites(sites, sizeof(sites) / sizeof(sites[0]))) {
                    AcquireSRWLockExclusive(&stateLock);
                    stopEvent = stop;
                    ReleaseSRWLockExclusive(&stateLock);
                    InterlockedExchange(&enabled, 1);
                    HANDLE worker = CreateThread(nullptr, 0, wakeWorker, stop, 0, nullptr);
                    if (worker) {
                        CloseHandle(worker);
                        c4trace_event(Ready, reinterpret_cast<uintptr_t>(hwnd), customMessage, privateMessage, kWakePeriodMs, 0);
                        SetLastError(saved);
                        return;
                    }
                    InterlockedExchange(&enabled, 0);
                    stopEvent = nullptr; // worker was never created
                    reason = 4;
                }
                CloseHandle(stop);
            }
        }
    }
#endif
    c4trace_event(Unavailable, reinterpret_cast<uintptr_t>(hwnd), reason, thread, process, 0);
    SetLastError(saved);
}
#endif
