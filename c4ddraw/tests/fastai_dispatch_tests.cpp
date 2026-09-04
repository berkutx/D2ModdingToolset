// Executes the actual fastai.cpp without installing any EXE hooks. All HWNDs belong to this test.
// QPC/Sleep are deterministic test seams, not replacements in the production build.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <vector>

static LONGLONG testCounter;
static LONGLONG sleepAdvance, peekAdvance;
static unsigned peekCalls;
static bool failCounter, raiseForward, raiseQueued, forceEmptyPeek;
static HWND destroyDuringPeek;
static BOOL WINAPI testQpc(LARGE_INTEGER* value) {
    value->QuadPart = (testCounter += 10);
    return !failCounter;
}
static BOOL WINAPI testFrequency(LARGE_INTEGER* value) { value->QuadPart = 1000000; return TRUE; }
static void WINAPI testSleep(DWORD) { testCounter += sleepAdvance; }
static LRESULT WINAPI testCallWindowProc(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (raiseForward) RaiseException(0xE0424242, 0, 0, nullptr);
    return CallWindowProcA(proc, hwnd, msg, wp, lp);
}
static LRESULT WINAPI testDispatchMessage(const MSG* msg) {
    if (raiseQueued) RaiseException(0xE0424243, 0, 0, nullptr);
    return DispatchMessageA(msg);
}
static BOOL WINAPI testPeekMessage(MSG* msg, HWND hwnd, UINT low, UINT high, UINT flags) {
    ++peekCalls;
    testCounter += peekAdvance; // Models time spent in callbacks dispatched inside Peek.
    if (destroyDuringPeek) {
        HWND victim = destroyDuringPeek;
        destroyDuringPeek = nullptr;
        DestroyWindow(victim); // Models a sent callback dispatched from inside Peek.
        return FALSE;
    }
    if (forceEmptyPeek) return FALSE; // Deterministic empty-branch/budget test, no OS broadcasts.
    return PeekMessageA(msg, hwnd, low, high, flags);
}

#define QueryPerformanceCounter testQpc
#define QueryPerformanceFrequency testFrequency
#define Sleep testSleep
#define CallWindowProcA testCallWindowProc
#define DispatchMessageA testDispatchMessage
#define PeekMessageA testPeekMessage
#include "../features/fastai.cpp"
#undef QueryPerformanceCounter
#undef QueryPerformanceFrequency
#undef Sleep
#undef CallWindowProcA
#undef DispatchMessageA
#undef PeekMessageA

struct TraceRecord { unsigned event; uintptr_t object, a, b, c, d; };
static std::vector<TraceRecord> records;
static bool traceEnabled = true, haveServer = true;
extern "C" int featuremenu_debug_enabled(void) { return 0; }
extern "C" int pluginhost_has_server(void) { return haveServer; }
extern "C" void c4trace_init(void) {}
extern "C" int c4trace_enabled(void) { return traceEnabled; }
extern "C" void c4trace_event(unsigned ev, uintptr_t obj, uintptr_t a, uintptr_t b,
                              uintptr_t c, uintptr_t d) {
    records.push_back({ev,obj,a,b,c,d});
}

static const UINT kStop = WM_APP+10, kChild = WM_APP+11, kOther = WM_APP+12;
static const UINT kNested = WM_APP+13, kInner = WM_APP+14;
static int failures, tests, childCalls, otherCalls, stopCalls, nestedCalls, innerCalls;
static int timerProcCalls, destroyedCalls;
static UINT_PTR callbackId;
static bool destroyOnTimer;
static std::vector<WPARAM> nativeTimers;
static std::vector<LPARAM> nativeTimerParameters;

static void check(bool ok, const char* message) {
    ++tests;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
static LRESULT CALLBACK nativeServer(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        nativeTimers.push_back(wp);
        nativeTimerParameters.push_back(lp);
        if (destroyOnTimer) { destroyOnTimer = false; DestroyWindow(hwnd); }
        SetLastError(0x13572468);
        return 77;
    }
    if (msg == kStop) { ++stopCalls; InterlockedExchange(&g_enabled, 0); return 0; }
    if (msg == kNested) {
        ++nestedCalls;
        runTimerSlice(hwnd, 999, 0);
        SendMessageA(hwnd, kInner, 17, 23);
        return 0;
    }
    if (msg == kInner) { ++innerCalls; return 0; }
    if (msg == WM_NCDESTROY) ++destroyedCalls;
    return DefWindowProcA(hwnd, msg, wp, lp);
}
static LRESULT CALLBACK childProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kChild) { ++childCalls; check(wp == 29 && lp == 31, "child parameters preserved"); return 0; }
    if (msg == kOther) { ++otherCalls; return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
static void CALLBACK timerProc(HWND, UINT, UINT_PTR id, DWORD) {
    ++timerProcCalls;
    callbackId = id;
    InterlockedExchange(&g_enabled, 0);
}

static HWND makeServer() {
    MSG msg = {};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message != WM_QUIT) DispatchMessageA(&msg);
    }
    HWND hwnd = CreateWindowExA(0, "ThreadWindowClass", "C4 private FastAI test", 0,
        0,0,1,1,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
    attachServerWindow(hwnd); // Exact private HWND only; no discovery or EXE patching.
    testCounter = 0;
    sleepAdvance = peekAdvance = 0;
    peekCalls = 0;
    failCounter = raiseForward = raiseQueued = destroyOnTimer = forceEmptyPeek = false;
    destroyDuringPeek = nullptr;
    g_enabled = g_available = g_fastai_turn = 1;
    g_timeoutUntil = 0;
    childCalls = otherCalls = stopCalls = nestedCalls = innerCalls = 0;
    timerProcCalls = destroyedCalls = 0;
    callbackId = 0;
    nativeTimers.clear(); nativeTimerParameters.clear(); records.clear();
    check(serverState().hwnd == hwnd && serverState().proc == nativeServer, "private attachment ready");
    return hwnd;
}
static void finish(HWND hwnd) {
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    check(!serverState().hwnd && !serverState().proc, "attachment detached on NCDESTROY");
    check(g_pumping == 0 && g_traceDispatchDepth == 0 && g_queuedDispatch == nullptr, "all scope guards clean");
}
static bool catchesSynthetic(HWND hwnd) {
    __try { runTimerSlice(hwnd, 404, 0); }
    __except (GetExceptionCode() == 0xE0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
static bool catchesQueued(HWND hwnd) {
    __try { runTimerSlice(hwnd, 405, 0); }
    __except (GetExceptionCode() == 0xE0424243 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}

int main() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = nativeServer;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ThreadWindowClass";
    if (!RegisterClassA(&wc)) return 10;
    wc.lpfnWndProc = childProc;
    wc.lpszClassName = "C4PrivateFastAiChild";
    if (!RegisterClassA(&wc)) return 11;
    g_aiMessage = RegisterWindowMessageA("C4 PRIVATE FASTAI TEST AI MESSAGE");

    HWND hwnd = makeServer();
    g_enabled = 0;
    SetLastError(0x24681357);
    runTimerSlice(hwnd, 101, 0);
    check(nativeTimers.empty() && GetLastError() == 0x24681357, "disabled path does no work, preserves LastError");
    finish(hwnd);

    hwnd = makeServer();
    forceEmptyPeek = true;
    SetLastError(0x24681357);
    runTimerSlice(hwnd, 101, 0x12345);
    check(nativeTimers.size() == kSliceDispatchLimit, "empty slice obeys dispatch limit");
    check(GetLastError() == 0x24681357, "slice preserves outer native LastError");
    for (auto id : nativeTimers) check(id == 101, "synthetic timer preserves outer timer id");
    for (auto parameter : nativeTimerParameters) check(parameter == 0x12345, "synthetic direct native timer preserves outer parameter");
    for (auto r : records) if (r.event == 70) check((r.d & 0x7f) == kTraceSyntheticEmpty && (r.d >> 8) == 1, "synthetic trace source/depth preserved");
    forceEmptyPeek = false;
    finish(hwnd);

    hwnd = makeServer();
    sleepAdvance = 4000;
    PostMessageA(hwnd, WM_TIMER, 202, 0);
    runTimerSlice(hwnd, 101, 0);
    check(peekCalls == 0 && nativeTimers.empty(), "expired yield does not Peek or synthesize");
    MSG budgetMessage = {};
    check(PeekMessageA(&budgetMessage, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE) &&
          budgetMessage.wParam == 202, "expired yield leaves real queued timer untouched");
    finish(hwnd);

    hwnd = makeServer();
    forceEmptyPeek = true;
    peekAdvance = 4000;
    runTimerSlice(hwnd, 101, 0);
    check(peekCalls == 1 && nativeTimers.empty(), "expired empty Peek does not synthesize");
    forceEmptyPeek = false;
    finish(hwnd);

    hwnd = makeServer();
    peekAdvance = 4000;
    PostMessageA(hwnd, WM_TIMER, 202, 0);
    runTimerSlice(hwnd, 101, 0);
    check(peekCalls == 1 && nativeTimers.size() == 1 && nativeTimers[0] == 202,
          "message already removed by expired Peek is still delivered exactly once");
    check(!PeekMessageA(&budgetMessage, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE),
          "delivered overbudget timer is not reposted or duplicated");
    finish(hwnd);

    hwnd = makeServer();
    sleepAdvance = -1000;
    PostMessageA(hwnd, WM_TIMER, 202, 0);
    runTimerSlice(hwnd, 101, 0);
    check(peekCalls == 0 && nativeTimers.empty(), "backward QPC after yield stops before Peek");
    check(PeekMessageA(&budgetMessage, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE) &&
          budgetMessage.wParam == 202, "backward QPC leaves queued timer untouched");
    finish(hwnd);

    hwnd = makeServer();
    forceEmptyPeek = true;
    peekAdvance = -1000;
    runTimerSlice(hwnd, 101, 0);
    check(peekCalls == 1 && nativeTimers.empty(), "backward QPC after empty Peek stops synthetic work");
    forceEmptyPeek = false;
    finish(hwnd);

    hwnd = makeServer();
    PostMessageA(hwnd, WM_TIMER, 202, 0);
    PostMessageA(hwnd, kStop, 0, 0);
    runTimerSlice(hwnd, 101, 0);
    check(nativeTimers.size() == 1 && nativeTimers[0] == 202, "queued timer NOT replaced by outer timer");
    check(stopCalls == 1, "queued stop callback delivered once");
    for (auto r : records) if (r.event == 70) check((r.d & 0x7f) == kTraceQueued, "queued server forward uses source2");
    finish(hwnd);

    hwnd = makeServer();
    HWND child = CreateWindowExA(0,"C4PrivateFastAiChild","private child",WS_CHILD,0,0,1,1,hwnd,nullptr,wc.hInstance,nullptr);
    HWND other = CreateWindowExA(0,"C4PrivateFastAiChild","private peer",0,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
    PostMessageA(other,kOther,0,0);
    PostMessageA(child,kChild,29,31);
    PostMessageA(hwnd,kStop,0,0);
    runTimerSlice(hwnd,101,0);
    check(childCalls == 1 && otherCalls == 0 && stopCalls == 1, "child uses own WndProc, unrelated HWND not drained");
    MSG otherMsg = {};
    check(PeekMessageA(&otherMsg,other,kOther,kOther,PM_REMOVE) && otherMsg.hwnd == other, "unrelated posted message still pending");
    DestroyWindow(other);
    finish(hwnd);

    hwnd = makeServer();
    const UINT_PTR id = SetTimer(hwnd,303,10,timerProc);
    MSG pending = {};
    const ULONGLONG began = GetTickCount64();
    while (!PeekMessageA(&pending,hwnd,WM_TIMER,WM_TIMER,PM_NOREMOVE) && GetTickCount64()-began < 1000) Sleep(1);
    check(id && pending.message == WM_TIMER && pending.lParam, "real TIMERPROC timer available");
    runTimerSlice(hwnd,101,0);
    check(timerProcCalls == 1 && callbackId == id && nativeTimers.empty(), "real TIMERPROC called once with its real id");
    KillTimer(hwnd,id);
    finish(hwnd);

    hwnd = makeServer();
    PostQuitMessage(37);
    runTimerSlice(hwnd,101,0);
    MSG quit = {};
    check(nativeTimers.empty() && PeekMessageA(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE) && quit.wParam == 37, "QUIT preserved for outer pump with exit code");
    check(!PeekMessageA(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE), "QUIT not duplicated");
    finish(hwnd);

    hwnd = makeServer();
    PostMessageA(hwnd,WM_CLOSE,0,0);
    runTimerSlice(hwnd,101,0);
    check(!IsWindow(hwnd) && destroyedCalls == 1 && nativeTimers.empty(), "queued close runs teardown, no post-destruction synthetic tick");
    finish(hwnd);

    hwnd = makeServer();
    destroyDuringPeek = hwnd;
    runTimerSlice(hwnd,101,0);
    check(!IsWindow(hwnd) && nativeTimers.empty(), "sent teardown inside empty Peek suppresses synthetic tick");
    finish(hwnd);

    hwnd = makeServer();
    PostMessageA(hwnd,kNested,0,0);
    PostMessageA(hwnd,kStop,0,0);
    runTimerSlice(hwnd,101,0);
    check(nestedCalls == 1 && innerCalls == 1 && nativeTimers.empty(), "nested runTimerSlice rejected without dropping native sent callback");
    bool sawNested = false;
    for (auto r : records) if (r.event == 70 && r.a == kInner) {
        sawNested = true;
        check((r.d & 0x7f) == kTraceExternal && (r.d >> 8) == 2, "nested sent callback not mislabeled queued");
    }
    check(sawNested, "nested trace retained");
    finish(hwnd);

    hwnd = makeServer();
    raiseForward = true;
    check(catchesSynthetic(hwnd), "synthetic SEH propagates unchanged");
    raiseForward = false;
    check(g_pumping == 0 && g_traceDispatchDepth == 0, "synthetic SEH clears pump and trace depth");
    bool sawAbnormal = false;
    for (auto r : records) if (r.event == 71 && (r.d & 0x80)) sawAbnormal = true;
    check(sawAbnormal, "abnormal trace boundary retained");
    finish(hwnd);

    hwnd = makeServer();
    PostMessageA(hwnd,kStop,0,0);
    raiseQueued = true;
    check(catchesQueued(hwnd), "queued dispatch SEH propagates unchanged");
    raiseQueued = false;
    check(g_pumping == 0 && g_queuedDispatch == nullptr, "queued SEH clears pump and provenance context");
    finish(hwnd);

    hwnd = makeServer();
    failCounter = true;
    runTimerSlice(hwnd,101,0);
    check(nativeTimers.empty() && g_pumping == 0, "QPC failure leaves no acquired guard");
    failCounter = false;
    finish(hwnd);

    hwnd = makeServer();
    destroyOnTimer = true;
    runTimerSlice(hwnd,101,0);
    check(nativeTimers.size() == 1 && !IsWindow(hwnd), "synthetic callback teardown stops the slice immediately");
    finish(hwnd);

    hwnd = makeServer();
    const ServerState attachment = serverState();
    check(!clearServerWindow(hwnd, attachment.generation-1) && serverState().hwnd == hwnd, "stale generation cannot detach current window");
    g_enabled = 0;
    SetLastError(0x24681357);
    const LRESULT result = fastAiServerWndProc(hwnd,WM_TIMER,101,0);
    check(result == 77 && GetLastError() == 0x13572468, "outer WndProc result and native LastError retained");
    finish(hwnd);

    traceEnabled = false;
    hwnd = makeServer();
    PostMessageA(hwnd,WM_TIMER,202,0);
    PostMessageA(hwnd,kStop,0,0);
    runTimerSlice(hwnd,101,0);
    check(records.empty() && nativeTimers.size() == 1 && nativeTimers[0] == 202, "trace off retains queued timer behavior");
    finish(hwnd);
    UnregisterClassA("C4PrivateFastAiChild",wc.hInstance);
    UnregisterClassA("ThreadWindowClass",wc.hInstance);
    std::printf("FastAI tests: %d checks, %d failures; private HWNDs only; no install hooks\n",tests,failures);
    return failures ? 1 : 0;
}
