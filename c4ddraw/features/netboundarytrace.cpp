/* Exact Russobit EXE seams, verified against saved native-net-consumer.txt,
 * native-net-server-consumer.txt, native-net-pass4.txt, native-net-consumer2.txt.
 * Every shim retains the original indirect target, argument/cleanup contract,
 * and displaced instruction. Never follows or patches an MSS detour. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <detours.h>
#include <cstdint>
#include <cstring>
#include "c4trace.h"
#include "inventorytrace.h"
#include "netboundarytrace.h"
#include "nettraceframe.h"

namespace {
using SendFn = bool(__thiscall*)(void*, uint32_t, const void*);
using ReceiveFn = int(__thiscall*)(void*, uint32_t*, void*);
using CountFn = int(__thiscall*)(void*);
struct AtomicCounters {
    volatile LONG sends, sendFailures, receives, receiveSuccess, receiveEmpty, receiveFailures;
    volatile LONG countCalls, lastCount, activeCalls, exceptions, selected, malformed;
};
AtomicCounters g_counters[2] = {};
volatile LONG g_installedMask = 0, g_attempted = 0, g_ordinal = 0;
uintptr_t g_clientSendReturn = 0x403CD1, g_serverSendReturn = 0x433741;
uintptr_t g_clientReceiveReturn = 0x402C8F, g_serverReceiveReturn = 0x433927;
uintptr_t g_clientCountReturn = 0x402CB4, g_serverCountReturn = 0x4339A4;

bool safeCopy(void* dest, const void* src, size_t count)
{
    if (!src) return false;
    __try { std::memcpy(dest, src, count); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

c4nettraceframe::Result frame(const void* source)
{
    using namespace c4nettraceframe;
    unsigned char copy[CopyLimit];
    if (!safeCopy(copy, source, HeaderBytes)) return inspect(nullptr, HeaderBytes);
    Result result = inspect(copy, HeaderBytes);
    if (result.status == Status::SelectedIncomplete && result.length <= CopyLimit &&
        safeCopy(copy, source, result.length)) result = inspect(copy, result.length);
    return result;
}

void recordFrame(uintptr_t self, uint32_t ordinal, const c4nettraceframe::Result& value,
                 unsigned senderStatus = 0)
{
    const uint32_t classification = uint32_t(value.kind) | (uint32_t(value.status) << 8);
    // senderStatus: 0 send/not-applicable, 1 receive sender copied, 2 unavailable.
    c4trace_event(C4NET_FRAME, self, ordinal, classification, value.length, senderStatus);
    // Separate scalar record keeps all 64 digest bits on this 32-bit process.
    c4trace_event(C4NET_FRAME, self, ordinal, 0x80000000u,
                  uint32_t(value.fingerprint), uint32_t(value.fingerprint >> 32));
}

void classify(AtomicCounters& counts, const c4nettraceframe::Result& value)
{
    using namespace c4nettraceframe;
    if (value.kind != Kind::None) InterlockedIncrement(&counts.selected);
    if (value.status == Status::InvalidHeader || value.status == Status::IncompleteHeader ||
        value.status == Status::SelectedIncomplete) InterlockedIncrement(&counts.malformed);
}

bool observeSend(unsigned role, void* self, uintptr_t target, uint32_t to, const void* message)
{
    const auto original = reinterpret_cast<SendFn>(target);
    if (!c4trace_enabled()) return original(self, to, message);
    const DWORD entryError = GetLastError();
    AtomicCounters& counts = g_counters[role];
    InterlockedIncrement(&counts.sends);
    InterlockedIncrement(&counts.activeCalls);
    const auto value = frame(message);
    classify(counts, value);
    const bool selected = value.kind != c4nettraceframe::Kind::None;
    const uint32_t ordinal = selected ? uint32_t(InterlockedIncrement(&g_ordinal)) : 0;
    if (selected) {
        c4trace_event(C4NET_SEND_ENTER, reinterpret_cast<uintptr_t>(self), ordinal, role, to, target);
        recordFrame(reinterpret_cast<uintptr_t>(self), ordinal, value);
    }
    bool completed = false, result = false;
    SetLastError(entryError);
    __try {
        result = original(self, to, message);
        completed = true;
    } __finally {
        const DWORD resultError = GetLastError();
        InterlockedDecrement(&counts.activeCalls);
        if (!completed) {
            InterlockedIncrement(&counts.exceptions);
            c4trace_event(C4NET_EXCEPTION, reinterpret_cast<uintptr_t>(self), ordinal, role, 0, target);
        } else {
            if (!result) InterlockedIncrement(&counts.sendFailures);
            if (selected) c4trace_event(C4NET_SEND_RESULT, reinterpret_cast<uintptr_t>(self),
                                        ordinal, role, result, resultError);
        }
        SetLastError(resultError);
    }
    return result;
}

int observeReceive(unsigned role, void* self, uintptr_t target, uint32_t* from, void* message)
{
    const auto original = reinterpret_cast<ReceiveFn>(target);
    if (!c4trace_enabled()) return original(self, from, message);
    const DWORD entryError = GetLastError();
    AtomicCounters& counts = g_counters[role];
    InterlockedIncrement(&counts.receives);
    InterlockedIncrement(&counts.activeCalls);
    int result = 0;
    bool completed = false;
    SetLastError(entryError);
    __try {
        result = original(self, from, message);
        completed = true;
    } __finally {
        const DWORD resultError = GetLastError();
        InterlockedDecrement(&counts.activeCalls);
        if (!completed) {
            InterlockedIncrement(&counts.exceptions);
            c4trace_event(C4NET_EXCEPTION, reinterpret_cast<uintptr_t>(self), 0, role, 1, target);
        } else if (result == 2) {
            InterlockedIncrement(&counts.receiveSuccess);
            const auto value = frame(message);
            classify(counts, value);
            if (value.kind != c4nettraceframe::Kind::None) {
                uint32_t sender = 0;
                const bool senderValid = safeCopy(&sender, from, sizeof(sender));
                const uint32_t ordinal = uint32_t(InterlockedIncrement(&g_ordinal));
                c4trace_event(C4NET_RECEIVE, reinterpret_cast<uintptr_t>(self), ordinal, role, sender, target);
                recordFrame(reinterpret_cast<uintptr_t>(self), ordinal, value, senderValid ? 1 : 2);
            }
        } else if (result == 0) InterlockedIncrement(&counts.receiveEmpty);
        else InterlockedIncrement(&counts.receiveFailures);
        SetLastError(resultError);
    }
    return result;
}

int observeCount(unsigned role, void* self, uintptr_t target)
{
    const auto original = reinterpret_cast<CountFn>(target);
    if (!c4trace_enabled()) return original(self);
    const DWORD entryError = GetLastError();
    AtomicCounters& counts = g_counters[role];
    InterlockedIncrement(&counts.countCalls);
    InterlockedIncrement(&counts.activeCalls);
    int result = 0;
    bool completed = false;
    SetLastError(entryError);
    __try { result = original(self); completed = true; }
    __finally {
        const DWORD resultError = GetLastError();
        InterlockedDecrement(&counts.activeCalls);
        if (completed) InterlockedExchange(&counts.lastCount, result);
        else {
            InterlockedIncrement(&counts.exceptions);
            c4trace_event(C4NET_EXCEPTION, reinterpret_cast<uintptr_t>(self), 0, role, 2, target);
        }
        SetLastError(resultError);
    }
    return result;
}

bool __fastcall sendClient(void* self, uintptr_t target, uint32_t to, const void* message)
{ return observeSend(0, self, target, to, message); }
bool __fastcall sendServer(void* self, uintptr_t target, uint32_t to, const void* message)
{ return observeSend(1, self, target, to, message); }
int __fastcall receiveClient(void* self, uintptr_t target, uint32_t* from, void* message)
{ return observeReceive(0, self, target, from, message); }
int __fastcall receiveServer(void* self, uintptr_t target, uint32_t* from, void* message)
{ return observeReceive(1, self, target, from, message); }
int __fastcall countClient(void* self, uintptr_t target)
{ return observeCount(0, self, target); }
int __fastcall countServer(void* self, uintptr_t target)
{ return observeCount(1, self, target); }

#if defined(_M_IX86)
__declspec(naked) void clientSendShim()
{
    __asm {
        mov edx, [ecx]
        mov edx, [edx+14h]
        call sendClient
        jmp dword ptr [g_clientSendReturn]
    }
}
__declspec(naked) void serverSendShim()
{
    __asm {
        mov edx, [eax+14h]
        call sendServer
        movzx eax, al
        jmp dword ptr [g_serverSendReturn]
    }
}
__declspec(naked) void clientReceiveShim()
{
    __asm {
        mov edx, [edx+18h]
        call receiveClient
        cmp eax, 3
        jmp dword ptr [g_clientReceiveReturn]
    }
}
__declspec(naked) void serverReceiveShim()
{
    __asm {
        mov edx, [eax+18h]
        call receiveServer
        mov [ebp-4], eax
        jmp dword ptr [g_serverReceiveReturn]
    }
}
__declspec(naked) void clientCountShim()
{
    __asm {
        mov eax, [ecx]
        mov edx, [eax+10h]
        call countClient
        jmp dword ptr [g_clientCountReturn]
    }
}
__declspec(naked) void serverCountShim()
{
    __asm {
        mov edx, [eax+10h]
        call countServer
        test eax, eax
        jmp dword ptr [g_serverCountReturn]
    }
}
#endif

const unsigned char kClientSend[] = {0x8B,0x11,0xFF,0x52,0x14};
const unsigned char kServerSend[] = {0xFF,0x50,0x14,0x0F,0xB6,0xC0};
const unsigned char kClientReceive[] = {0xFF,0x52,0x18,0x83,0xF8,0x03};
const unsigned char kServerReceive[] = {0xFF,0x50,0x18,0x89,0x45,0xFC};
const unsigned char kClientCount[] = {0x8B,0x01,0xFF,0x50,0x10};
const unsigned char kServerCount[] = {0xFF,0x50,0x10,0x85,0xC0};
struct Site { uintptr_t address; const unsigned char* signature; size_t bytes; PVOID replacement; unsigned mask; PVOID original; };

bool matches(const Site& site)
{
    __try { return !std::memcmp(reinterpret_cast<void*>(site.address), site.signature, site.bytes); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void installSites(Site* sites, size_t count)
{
    unsigned mask = 0;
    for (size_t i = 0; i < count; ++i) {
        if (matches(sites[i])) { sites[i].original = reinterpret_cast<PVOID>(sites[i].address); mask |= sites[i].mask; }
        else c4trace_event(C4NET_BOUNDARY_UNAVAILABLE, sites[i].address, 2, sites[i].mask, 0, 0);
    }
    if (!mask) return;
    LONG result = DetourTransactionBegin();
    if (result == NO_ERROR) {
        result = DetourUpdateThread(GetCurrentThread());
        for (size_t i = 0; i < count && result == NO_ERROR; ++i) {
            if (!(mask & sites[i].mask)) continue;
            if (!matches(sites[i])) result = ERROR_INVALID_DATA;
            else result = DetourAttach(&sites[i].original, sites[i].replacement);
        }
        if (result == NO_ERROR) result = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    if (result == NO_ERROR) {
        InterlockedExchange(&g_installedMask, LONG(mask));
        c4trace_event(C4NET_BOUNDARY_READY, 0, mask, 0, 0, 0);
    } else c4trace_event(C4NET_BOUNDARY_UNAVAILABLE, 0, 3, mask, result, 0);
}
} // namespace

extern "C" void netboundarytrace_install(void)
{
    const DWORD error = GetLastError();
    if (c4trace_enabled() && InterlockedCompareExchange(&g_attempted, 1, 0) == 0) {
        if (!inventorytrace_exact_exe()) c4trace_event(C4NET_BOUNDARY_UNAVAILABLE, 0, 1, 0, 0, 0);
#if defined(_M_IX86)
        else {
            Site sites[] = {
                {0x403CCC,kClientSend,sizeof(kClientSend),clientSendShim,C4NET_CLIENT_SEND,nullptr},
                {0x43373B,kServerSend,sizeof(kServerSend),serverSendShim,C4NET_SERVER_SEND,nullptr},
                {0x402C89,kClientReceive,sizeof(kClientReceive),clientReceiveShim,C4NET_CLIENT_RECEIVE,nullptr},
                {0x433921,kServerReceive,sizeof(kServerReceive),serverReceiveShim,C4NET_SERVER_RECEIVE,nullptr},
                {0x402CAF,kClientCount,sizeof(kClientCount),clientCountShim,C4NET_CLIENT_COUNT,nullptr},
                {0x43399F,kServerCount,sizeof(kServerCount),serverCountShim,C4NET_SERVER_COUNT,nullptr}
            };
            installSites(sites, sizeof(sites) / sizeof(sites[0]));
        }
#else
        else c4trace_event(C4NET_BOUNDARY_UNAVAILABLE, 0, 4, 0, 0, 0);
#endif
    }
    SetLastError(error);
}

extern "C" void netboundarytrace_sampleCounters(C4NetBoundaryCounters* out)
{
    const DWORD error = GetLastError();
    if (out) {
        out->installedMask = uint32_t(InterlockedCompareExchange(&g_installedMask, 0, 0));
        C4NetBoundaryRoleCounters* result[] = {&out->client, &out->server};
        for (unsigned role = 0; role < 2; ++role) {
#define SAMPLE(name) result[role]->name = uint32_t(InterlockedCompareExchange(&g_counters[role].name, 0, 0))
            SAMPLE(sends); SAMPLE(sendFailures); SAMPLE(receives); SAMPLE(receiveSuccess);
            SAMPLE(receiveEmpty); SAMPLE(receiveFailures); SAMPLE(countCalls); SAMPLE(lastCount);
            SAMPLE(activeCalls); SAMPLE(exceptions); SAMPLE(selected); SAMPLE(malformed);
#undef SAMPLE
        }
    }
    SetLastError(error);
}

extern "C" void netboundarytrace_sample(void)
{
    const DWORD error = GetLastError();
    if (c4trace_enabled()) {
        C4NetBoundaryCounters snapshot = {};
        netboundarytrace_sampleCounters(&snapshot);
        const C4NetBoundaryRoleCounters* roles[] = {&snapshot.client, &snapshot.server};
        for (unsigned role = 0; role < 2; ++role) {
            const auto& value = *roles[role];
            c4trace_event(C4NET_TOTALS, role, value.sends, value.sendFailures, value.receives, value.receiveSuccess);
            c4trace_event(C4NET_RECEIVE_TOTALS, role, value.receiveEmpty, value.receiveFailures, value.countCalls, value.lastCount);
            c4trace_event(C4NET_DIAGNOSTIC_TOTALS, role, value.activeCalls, value.exceptions, value.selected, value.malformed);
        }
    }
    SetLastError(error);
}
