/* Opt-in, wrapper-side event boundaries. No MSS layout, code patch or notification retry. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <cstring>
#include "c4trace.h"
#include "eventtrace.h"
#pragma intrinsic(_ReturnAddress)

extern "C" void inventorytrace_install(void);
extern "C" int inventorytrace_exact_exe(void);

namespace {
using PostFn = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using RegisterFn = UINT(WINAPI*)(LPCSTR);
PostFn originalPost;
RegisterFn originalRegister;
volatile LONG installed;
volatile LONG inputTick;
bool exactExe;
LARGE_INTEGER frequency;
__declspec(thread) DWORD lastClock;
__declspec(thread) DWORD lastFactor;
__declspec(thread) DWORD lastManager;
__declspec(thread) DWORD lastWait;
__declspec(thread) unsigned waitCount;
__declspec(thread) unsigned long long waitTotal;
__declspec(thread) unsigned long long waitMaximum;

bool interesting(UINT msg)
{
    // Never capture chat text or ordinary keyboard input. Numeric message parameters only.
    return msg >= 0xC000 || msg == WM_TIMER || msg == WM_LBUTTONDOWN ||
           msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK || msg == WM_ACTIVATEAPP ||
           (msg == WM_KEYDOWN); // caller restricts keydown to A/F8
}

bool hot()
{
    const DWORD tick = static_cast<DWORD>(InterlockedCompareExchange(&inputTick, 0, 0));
    return tick && GetTickCount() - tick < 2000;
}

void sampleManager(unsigned stage)
{
    if (!exactExe) return;
    const DWORD now = GetTickCount();
    if (now - lastManager < 250) return;
    lastManager = now;
    __try {
        // Exact EXE singleton only; no call to get()/AddRef, and no mod-relative offsets.
        const uintptr_t ref = *reinterpret_cast<volatile uintptr_t*>(0x839048);
        const uintptr_t object = *reinterpret_cast<volatile uintptr_t*>(0x83904C);
        const DWORD count = ref >= 0x10000 && ref < 0x7fff0000
            ? *reinterpret_cast<volatile DWORD*>(ref) : 0xffffffff;
        c4trace_event(C4TRACE_MANAGER, object, ref, count, stage, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c4trace_event(C4TRACE_MANAGER, 0, 0, 0xffffffff, stage, GetExceptionCode());
    }
}

BOOL WINAPI tracedPost(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const bool record = c4trace_enabled() && msg >= 0xC000;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (record) c4trace_event(C4TRACE_POST_ENTER, reinterpret_cast<uintptr_t>(hwnd), msg, wp, lp, caller);
    const BOOL result = originalPost(hwnd, msg, wp, lp);
    const DWORD error = GetLastError();
    // No file output/formatting/wait here: this point is inside the suspected post/store window.
    if (record) c4trace_event(C4TRACE_POST_RETURN, reinterpret_cast<uintptr_t>(hwnd), msg, result, error, caller);
    SetLastError(error);
    return result;
}

UINT WINAPI tracedRegister(LPCSTR name)
{
    const UINT result = originalRegister(name);
    const DWORD error = GetLastError();
    unsigned peerName = 0;
    __try {
        if (name && !strcmp(name, "MIDGARD CUSTOM LOBBY NETMSG")) peerName = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    c4trace_event(C4TRACE_REGISTER, 0, result, peerName, error,
                  reinterpret_cast<uintptr_t>(_ReturnAddress()));
    SetLastError(error);
    return result;
}

bool patchImport(const char* name, void* replacement, void** previous)
{
    // Walk named EXE imports, not a fixed IAT RVA and never a DLL's code/exports.
    __try {
        BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew > 0x100000) return false;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
        const DWORD size = nt->OptionalHeader.SizeOfImage;
        const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || dir.VirtualAddress >= size || dir.Size > size - dir.VirtualAddress)
            return false;
        auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (DWORD i = 0; (i + 1) * sizeof(*desc) <= dir.Size && desc[i].Name; ++i) {
            if (desc[i].Name >= size || _stricmp(reinterpret_cast<char*>(base + desc[i].Name), "USER32.dll"))
                continue;
            if (!desc[i].OriginalFirstThunk || !desc[i].FirstThunk) continue;
            for (DWORD n = 0;; ++n) {
                const size_t offset = static_cast<size_t>(n) * sizeof(IMAGE_THUNK_DATA32);
                if (offset + sizeof(IMAGE_THUNK_DATA32) > size ||
                    desc[i].OriginalFirstThunk > size - offset - sizeof(IMAGE_THUNK_DATA32) ||
                    desc[i].FirstThunk > size - offset - sizeof(IMAGE_THUNK_DATA32)) break;
                auto symbol = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc[i].OriginalFirstThunk + offset);
                if (!symbol->u1.AddressOfData) break;
                if (IMAGE_SNAP_BY_ORDINAL32(symbol->u1.Ordinal) || symbol->u1.AddressOfData >= size) continue;
                auto importName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + symbol->u1.AddressOfData);
                if (strcmp(reinterpret_cast<char*>(importName->Name), name)) continue;
                auto slot = reinterpret_cast<void**>(base + desc[i].FirstThunk + offset);
                void* old = *slot;
                if (!old || old == replacement) return false;
                DWORD protect = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
                *previous = old; // publish original before installing the observer
                void* observed = InterlockedCompareExchangePointer(slot, replacement, old);
                DWORD unused = 0;
                VirtualProtect(slot, sizeof(void*), protect, &unused);
                c4trace_event(C4TRACE_HOOK, reinterpret_cast<uintptr_t>(slot),
                              reinterpret_cast<uintptr_t>(old), reinterpret_cast<uintptr_t>(replacement),
                              observed == old, GetLastError());
                return observed == old;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
}

extern "C" void eventtrace_install(void)
{
    const DWORD saved = GetLastError();
    if (InterlockedCompareExchange(&installed, 1, 0) == 0) {
        c4trace_init(); // first GUI dispatch, not DllMain
        if (c4trace_enabled()) {
            QueryPerformanceFrequency(&frequency);
            inventorytrace_install();
            exactExe = inventorytrace_exact_exe() != 0;
            const bool post = patchImport("PostMessageA", reinterpret_cast<void*>(tracedPost),
                                         reinterpret_cast<void**>(&originalPost));
            const bool reg = patchImport("RegisterWindowMessageA", reinterpret_cast<void*>(tracedRegister),
                                        reinterpret_cast<void**>(&originalRegister));
            c4trace_event(C4TRACE_HOOK, 0, post, reg, exactExe, 0);
        }
    }
    SetLastError(saved);
}

extern "C" void eventtrace_mark_input(void)
{
    if (!c4trace_enabled()) return;
    const DWORD saved = GetLastError();
    InterlockedExchange(&inputTick, static_cast<LONG>(GetTickCount()));
    SetLastError(saved);
}

extern "C" void eventtrace_message(unsigned stage, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!c4trace_enabled() || !interesting(msg) ||
        (msg == WM_KEYDOWN && wp != 'A' && wp != VK_F8)) return;
    const DWORD saved = GetLastError();
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
        (msg == WM_KEYDOWN && wp == VK_F8)) eventtrace_mark_input();
    c4trace_event(stage, reinterpret_cast<uintptr_t>(hwnd), msg, wp, lp, 0);
    if (stage == C4TRACE_FEATURE_WND) sampleManager(stage);
    SetLastError(saved);
}

extern "C" void eventtrace_pulled(unsigned stage, const MSG* msg, int result, unsigned remove)
{
    if (!c4trace_enabled()) return;
    if (result < 0) { c4trace_event(stage, 0, 0, result, remove, GetLastError()); return; }
    if (!result || !msg || !interesting(msg->message) ||
        (msg->message == WM_KEYDOWN && msg->wParam != 'A' && msg->wParam != VK_F8)) return;
    // MSG.time is raw OS time, independent from the game's virtual clock.
    c4trace_event(stage, reinterpret_cast<uintptr_t>(msg->hwnd), msg->message, msg->time,
                  remove, msg->lParam);
    c4trace_event(C4TRACE_PULLED_PAYLOAD, reinterpret_cast<uintptr_t>(msg->hwnd),
                  msg->message, msg->wParam, msg->lParam, stage);
}

extern "C" void eventtrace_clock(DWORD realTick, DWORD virtualTick, DWORD factor, uintptr_t caller)
{
    if (!c4trace_enabled()) return;
    const DWORD saved = GetLastError();
    if (factor != lastFactor || realTick - lastClock >= 250 || hot()) {
        lastFactor = factor; lastClock = realTick;
        c4trace_event(C4TRACE_CLOCK, caller, realTick, virtualTick, factor, 0);
    }
    SetLastError(saved);
}

extern "C" unsigned long long eventtrace_wait_begin(void)
{
    if (!c4trace_enabled()) return 0;
    const DWORD saved = GetLastError();
    LARGE_INTEGER stamp; QueryPerformanceCounter(&stamp);
    SetLastError(saved);
    return stamp.QuadPart;
}

extern "C" void eventtrace_wait_end(unsigned long long start, unsigned tickLength)
{
    if (!start || !c4trace_enabled()) return;
    const DWORD saved = GetLastError();
    LARGE_INTEGER stamp; QueryPerformanceCounter(&stamp);
    const unsigned long long duration = stamp.QuadPart - start;
    const DWORD now = GetTickCount();
    ++waitCount; waitTotal += duration;
    if (duration > waitMaximum) waitMaximum = duration;
    if (hot() || (frequency.QuadPart && duration >= static_cast<unsigned long long>(frequency.QuadPart / 20)))
        c4trace_event(C4TRACE_WAIT, 0, static_cast<uintptr_t>(duration), tickLength, 0, 0);
    if (now - lastWait >= 250) {
        c4trace_event(C4TRACE_WAIT_SUMMARY, 0, waitCount, static_cast<uintptr_t>(waitTotal),
                      static_cast<uintptr_t>(waitMaximum), tickLength);
        lastWait = now; waitCount = 0; waitTotal = waitMaximum = 0;
    }
    SetLastError(saved);
}

extern "C" void eventtrace_frame(unsigned stage, uintptr_t surface)
{
    if (!c4trace_enabled()) return;
    const DWORD saved = GetLastError();
    if (stage == C4TRACE_RENDER_START || hot()) c4trace_event(stage, surface, 0, 0, 0, 0);
    SetLastError(saved);
}

extern "C" void eventtrace_surface(unsigned stage, uintptr_t surface, unsigned caps,
                                    unsigned flags, DWORD lastFlip, DWORD lastBlt)
{
    if (!c4trace_enabled()) return;
    const DWORD saved = GetLastError();
    if (hot()) c4trace_event(stage, surface, caps, flags, lastFlip, lastBlt);
    SetLastError(saved);
}
