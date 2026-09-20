#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <detours.h>
#include "c4trace.h"
#include "eventtrace.h"
#include "inventorytrace.h"
#include "netturntrace.h"

namespace {

// CMidClientCore : IMqNetSystem, virtual slot 2. This is an EXE method, not MSS code.
// The saved native receiver confirms void __thiscall(this, unsigned netPlayerId), ret 4.
using Disconnect = void(__thiscall*)(void*, unsigned);
Disconnect g_originalDisconnect = reinterpret_cast<Disconnect>(0x40C2BE);
volatile LONG g_disconnectStatus = 0;
volatile LONG g_disconnectAttempted = 0;
volatile LONG g_disconnectSerial = 0;

void __fastcall traceDisconnect(void* self, void*, unsigned netPlayerId)
{
    const DWORD incomingError = GetLastError();
    const LONG serial = c4trace_enabled() ? InterlockedIncrement(&g_disconnectSerial) : 0;
    if (serial)
        c4trace_event(C4TRACE_DISCONNECT_ENTER, reinterpret_cast<uintptr_t>(self),
                      netPlayerId, 0, 0, static_cast<uintptr_t>(serial));
    SetLastError(incomingError);
    bool completed = false;
    __try {
        g_originalDisconnect(self, netPlayerId);
        completed = true;
    } __finally {
        const DWORD nativeError = GetLastError();
        if (serial)
            c4trace_event(C4TRACE_DISCONNECT_RETURN, reinterpret_cast<uintptr_t>(self),
                          netPlayerId, 0, completed ? 1u : 0u, static_cast<uintptr_t>(serial));
        SetLastError(nativeError);
    }
}

bool disconnectEntryMatches()
{
    // Do not bypass or overwrite an unknown existing detour. Loss of this optional coverage is
    // explicit; other network observers continue to work.
    static const unsigned char entry[] = {
        0xB8, 0x45, 0x81, 0x68, 0x00, 0xE8, 0x08, 0x11, 0x26, 0x00,
        0x83, 0xEC, 0x10, 0x53
    };
    __try {
        return *reinterpret_cast<const uintptr_t*>(0x6CEB54) == 0x40C2BE &&
            std::memcmp(reinterpret_cast<const void*>(0x40C2BE), entry, sizeof(entry)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

extern "C" int netturntrace_disconnect_available(void)
{
    return static_cast<int>(InterlockedCompareExchange(&g_disconnectStatus, 0, 0));
}

extern "C" void netturntrace_install(void)
{
    const DWORD incomingError = GetLastError();
    __try {
        if (!c4trace_enabled() || InterlockedCompareExchange(&g_disconnectAttempted, 1, 0))
            return;
        LONG status = -1;
        if (c4_exact_game_exe()) {
            status = -2;
            if (disconnectEntryMatches()) {
                status = -3;
                if (DetourTransactionBegin() == NO_ERROR) {
                    if (DetourUpdateThread(GetCurrentThread()) == NO_ERROR &&
                        DetourAttach(reinterpret_cast<PVOID*>(&g_originalDisconnect),
                                     reinterpret_cast<PVOID>(traceDisconnect)) == NO_ERROR) {
                        if (DetourTransactionCommit() == NO_ERROR)
                            status = 1;
                    } else {
                        DetourTransactionAbort();
                    }
                }
            }
        }
        InterlockedExchange(&g_disconnectStatus, status);
        c4trace_event(C4TRACE_DISCONNECT_COVERAGE, 0x40C2BE,
                      static_cast<uintptr_t>(status), 0x6CEB54, 0, 0);
    } __finally {
        SetLastError(incomingError);
    }
}
