#include <windows.h>
#include <stdint.h>
#include <cstdio>
#include <vector>
#include "eventtrace.h"

struct Record { unsigned event; uintptr_t object, a, b, c, d; };
static std::vector<Record> records;
static bool enabled = true;
static bool throwNative = false;
static int nativeCalls = 0;
static DWORD nativeIncomingError = 0;
static void* nativeSelf = nullptr;
static int nativeMessage = 0;
static unsigned nativePlayer = 0;
static int failures = 0;
static const int kInvalidMidgardId = 0x3F0000;
static volatile LONG g_turnInfoTraceAvailable = 1;
static volatile LONG g_turnInfoTraceSerial = 0;
struct {
    int lastTurnPlayer = -1;
    volatile LONG beginTurnReadyTicks = 0, beginTurnReadyPending = 0;
    void* g_orig_turnInfo = nullptr;
} g;
static bool isUserPtr(const void* p) { return p != nullptr; }
static void clearPendingActions() {}
static void clearPostBattleTransition() {}
static void pluginhost_bump_turn(int) {}
static void tlog(const char*, ...) {}
extern "C" int c4trace_enabled() { return enabled ? 1 : 0; }
extern "C" void c4trace_event(unsigned e, uintptr_t o, uintptr_t a, uintptr_t b,
                              uintptr_t c, uintptr_t d)
{
    records.push_back({e, o, a, b, c, d});
    SetLastError(0xDEADBEEF); // Deliberately hostile sink: hook must preserve the native value.
}

#include "turninfo-trace.generated.h"
#include "disconnect-trace.generated.h"

static int __fastcall originalTurn(void* self, void*, int message)
{
    ++nativeCalls;
    nativeIncomingError = GetLastError();
    nativeSelf = self;
    nativeMessage = message;
    SetLastError(0x13572468);
    if (throwNative) RaiseException(0xE0424321, 0, 0, nullptr);
    return static_cast<int>(0x87654321u);
}
static void __fastcall originalDisconnect(void* self, void*, unsigned player)
{
    ++nativeCalls;
    nativeIncomingError = GetLastError();
    nativeSelf = self;
    nativePlayer = player;
    SetLastError(0x13572468);
    if (throwNative) RaiseException(0xE0424321, 0, 0, nullptr);
}
static bool turnThrows(void* self, int message)
{
    __try { hook_turnInfo(self, nullptr, message); }
    __except (GetExceptionCode() == 0xE0424321 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
static bool disconnectThrows(void* self, unsigned player)
{
    __try { traceDisconnect(self, nullptr, player); }
    __except (GetExceptionCode() == 0xE0424321 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
static void check(bool condition, const char* name)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
static void reset()
{
    records.clear(); enabled = true; throwNative = false; nativeCalls = 0;
    nativeIncomingError = 0; nativeSelf = nullptr; nativeMessage = 0; nativePlayer = 0;
    g.lastTurnPlayer = -1;
    g.g_orig_turnInfo = reinterpret_cast<void*>(originalTurn);
    g_originalDisconnect = reinterpret_cast<Disconnect>(originalDisconnect);
    g_turnInfoTraceAvailable = 1;
    SetLastError(0x24681357);
}
int main()
{
    uintptr_t message[7] = {0x6D4B14, 0, 0, 0, 0, 0, 0x12340001};
    void* const self = reinterpret_cast<void*>(0x12345678);
    const int msg = reinterpret_cast<int>(message);
    reset();
    int result = hook_turnInfo(self, nullptr, msg);
    DWORD error = GetLastError();
    check(nativeCalls == 1 && nativeSelf == self && nativeMessage == msg &&
          result == static_cast<int>(0x87654321u), "TurnInfo original called once, arguments/full return preserved");
    check(nativeIncomingError == 0x24681357 && error == 0x13572468,
          "TurnInfo incoming and native LastError preserved");
    check(records.size() == 2 && records[0].event == C4TRACE_TURN_INFO_ENTER &&
          records[0].b == message[6] && records[0].c == 0x6D4B14 &&
          records[1].event == C4TRACE_TURN_INFO_RETURN && records[1].c == 1 &&
          records[0].d == records[1].d && records[1].b == 0x87654321u,
          "TurnInfo native fields, return and call correlation recorded");
    hook_turnInfo(self, nullptr, msg);
    check(records.size() == 4 && records[2].d != records[0].d,
          "Repeated owner is still recorded independently of timer debounce");
    reset(); throwNative = true;
    const bool turnException = turnThrows(self, msg);
    error = GetLastError();
    check(turnException && nativeCalls == 1 && records.size() == 2 && records[1].c == 0 &&
          error == 0x13572468, "TurnInfo SEH propagates with abnormal exit and native LastError");
    reset(); g_turnInfoTraceAvailable = 0;
    hook_turnInfo(self, nullptr, msg);
    check(records.empty() && nativeCalls == 1,
          "Unavailable exact-EXE coverage leaves TurnInfo behavior intact");
    reset(); enabled = false;
    hook_turnInfo(self, nullptr, msg);
    check(records.empty() && nativeCalls == 1 && GetLastError() == 0x13572468,
          "Disabled recording leaves TurnInfo behavior intact");
    reset();
    traceDisconnect(self, nullptr, 0xABCDEF01);
    error = GetLastError();
    check(nativeCalls == 1 && nativeSelf == self && nativePlayer == 0xABCDEF01 &&
          nativeIncomingError == 0x24681357 && error == 0x13572468,
          "Disconnect original once, arguments and LastError preserved");
    check(records.size() == 2 && records[0].event == C4TRACE_DISCONNECT_ENTER &&
          records[1].event == C4TRACE_DISCONNECT_RETURN && records[0].d == records[1].d &&
          records[0].a == 0xABCDEF01 && records[1].c == 1,
          "Disconnect net player id and completion recorded");
    reset(); throwNative = true;
    const bool disconnectException = disconnectThrows(self, 1);
    error = GetLastError();
    check(disconnectException && nativeCalls == 1 && records.size() == 2 && records[1].c == 0 &&
          error == 0x13572468, "Disconnect SEH propagates with abnormal exit and native LastError");
    reset(); enabled = false;
    traceDisconnect(self, nullptr, 1);
    check(records.empty() && nativeCalls == 1 && GetLastError() == 0x13572468,
          "Disabled recording leaves disconnect behavior intact");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
