// Actual production adapter + batching seam; fixed game install sites excluded.
// Native callees are fixture functions; all windows and Detours sites are private.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include "../upstream/cnc-ddraw/src/detours/detours.h"
#define C4_NETNOTIFY_TESTING
#include "../features/netnotify.cpp"
#define C4_MESSAGEBATCH_TESTING
#pragma warning(push)
#pragma warning(disable: 4459) // Single test TU merges otherwise separate anonymous namespaces.
#include "../features/messagebatch.cpp"
#pragma warning(pop)

static int fixtureNativeDepth = 0;
extern "C" int featuremenu_native_dispatch_active(void) { return fixtureNativeDepth; }
extern "C" void c4trace_event(unsigned, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{ SetLastError(0xBADC0DE); } // Deliberately noisy instrumentation must remain transparent.

namespace fixture {
struct Controller { void* vtable; void* data; };
struct Pair { HWND window; bool child; unsigned char padding[3]; };
struct KernelData { uint32_t unused; Pair* pair; unsigned char padding[80]; Controller* controller; };
struct Kernel { void** vtable; KernelData* data; };
static_assert(sizeof(void*) == 4 && offsetof(KernelData, controller) == 0x58, "Native x86 layout");
uintptr_t controllerStorage = 17, changedStorage = 18;
Controller controller = {reinterpret_cast<void*>(0x1111), &controllerStorage};
Pair pair = {};
KernelData kernelData = {};
void* kernelVtable[22] = {};
Kernel kernel = {kernelVtable, &kernelData};
Kernel nestedKernel = {kernelVtable, &kernelData};
HWND window = nullptr;
unsigned checks = 0, assertionFailures = 0, cases = 0;
const char* caseName = "setup";
unsigned nativeCalls = 0, dispatchCalls = 0, removeCalls = 0, destroyCalls = 0, addCalls = 0;
unsigned observedLoopDepth = 0, observedCallbackDepth = 0;
WPARAM seenWp = 1;
LPARAM seenLp = 1;
void* seenObject = nullptr;
UINT seenMessage = 0;
int nextId = 0;
enum Mode { Normal, NoListener, ForeignNested, CustomNested, Throw, RemoveDuringRun, ChangeData, NestedLoop };
Mode runMode = Normal, loopMode = Normal;
constexpr DWORD kEntryError = 0xAB10;
constexpr DWORD kNativeError = 0xAB11;
constexpr DWORD kDispatchError = 0xAB12;
constexpr DWORD kException = 0xE0424242;
constexpr WPARAM kLoopResult = 0xFEDCBA98;
UINT foreignMessage;

void check(bool good, const char* expression, int line)
{
    ++checks;
    if (!good) { ++assertionFailures; std::printf("FAIL %s:%d %s\n", caseName, line, expression); }
}
#define CHECK(x) fixture::check(!!(x), #x, __LINE__)
void begin(const char* name) { caseName = name; ++cases; std::printf("CASE %02u %s\n", cases, name); }

int __fastcall nativeAdd(void* object, void*, UINT msg, void*)
{
    ++addCalls; seenObject = object; seenMessage = msg;
    SetLastError(kNativeError);
    return nextId;
}
void __fastcall nativeRemove(void* object, void*, UINT msg, int id)
{
    ++removeCalls; seenObject = object; seenMessage = msg;
    CHECK(id == nextId);
    CHECK(GetLastError() == kEntryError);
    SetLastError(kNativeError);
}
void __fastcall nativeDestroy(void* object, void*)
{
    ++destroyCalls; seenObject = object;
    CHECK(!state.observed);
    CHECK(GetLastError() == kEntryError);
    SetLastError(kNativeError);
}
bool __fastcall nativeRun(void* object, void*, DWORD* out, unsigned short msg, WPARAM wp, LPARAM lp)
{
    ++nativeCalls;
    seenObject = object; seenMessage = msg; seenWp = wp; seenLp = lp;
    observedCallbackDepth = callbackDepth;
    CHECK(out != nullptr);
    *out = 0x789ABC;
    if (runMode == Throw) RaiseException(kException, 0, 0, nullptr);
    if (runMode == ForeignNested && msg == foreignMessage) {
        DWORD nested = 0;
        CHECK(callbackDepth == 1 && customCallbackDepth == 0);
        CHECK(!runHook(object, nullptr, &nested, static_cast<unsigned short>(customMessage), 0, 0));
        CHECK(state.armed && callbackDepth == 1 && customCallbackDepth == 0);
    }
    if (runMode == CustomNested && customCallbackDepth == 1) {
        DWORD nested = 0;
        runHook(object, nullptr, &nested, msg, 0, 0);
        CHECK(!state.armed && customCallbackDepth == 1);
    }
    if (runMode == RemoveDuringRun) {
        SetLastError(kEntryError);
        removeHook(object, nullptr, customMessage, nextId);
    }
    if (runMode == ChangeData) controller.data = &changedStorage;
    SetLastError(kNativeError);
    return runMode == NoListener;
}
WPARAM __fastcall nativeLoop(void* object, void*)
{
    CHECK(GetLastError() == kEntryError);
    observedLoopDepth = loopDepth;
    CHECK(outerKernel == &kernel);
    if (loopMode == Throw) RaiseException(kException, 0, 0, nullptr);
    if (loopMode == NestedLoop && object == &kernel) {
        loopMode = Normal;
        CHECK(reinterpret_cast<LoopFn>(&loopHook)(&nestedKernel) == kLoopResult);
        CHECK(observedLoopDepth == 2 && loopDepth == 1 && outerKernel == &kernel);
    }
    SetLastError(kNativeError);
    return kLoopResult;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (netnotify_window_event(hwnd, msg, wp, lp)) return 0;
    if (msg == customMessage) {
        DWORD result = 0;
        runHook(&controller, nullptr, &result, static_cast<unsigned short>(msg), wp, lp);
        return 0x1234;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
LRESULT WINAPI dispatch(const MSG* msg)
{
    ++dispatchCalls;
    const LRESULT result = DispatchMessageA(msg);
    SetLastError(kDispatchError);
    return result;
}

void drainPrivate()
{
    MSG msg;
    while (PeekMessageA(&msg, window, privateMessage, privateMessage, PM_REMOVE)) {}
}
void reset()
{
    drainPrivate();
    state = c4net::WakeState{};
    enabled = 1; mainWindow = window; uiThread = GetCurrentThreadId();
    loopDepth = 1; outerKernel = &kernel;
    callbackDepth = customCallbackDepth = syntheticDepth = modalMask = 0;
    fixtureNativeDepth = 0;
    posts = deliveries = deferrals = failures = 0;
    nativeCalls = dispatchCalls = removeCalls = destroyCalls = addCalls = 0;
    nextId = 0; runMode = loopMode = Normal;
    controller.data = &controllerStorage;
    kernelData.controller = &controller;
    g_enabled = 0; g_depth = g_epoch = g_modalMask = 0;
    g_uiThread = uiThread; g_mainHwnd = window;
    EnableWindow(window, TRUE);
}
void registration(bool arm = true)
{
    SetLastError(kEntryError);
    CHECK(addHook(&controller, nullptr, customMessage, reinterpret_cast<void*>(1)) == nextId);
    CHECK(GetLastError() == kNativeError && state.observed && !state.armed);
    if (arm) {
        DWORD out = 0;
        SetLastError(kEntryError);
        CHECK(!runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 0, 0));
        CHECK(GetLastError() == kNativeError && out == 0x789ABC && state.armed);
    }
}
MSG postWake()
{
    Ticket ticket = {};
    CHECK(state.reserve(&ticket));
    postTicket(ticket);
    MSG msg = {};
    CHECK(PeekMessageA(&msg, window, privateMessage, privateMessage, PM_REMOVE));
    return msg;
}
bool catchesRun()
{
    __try {
        DWORD out = 0;
        runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 0, 0);
    } __except (GetExceptionCode() == kException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
bool catchesLoop()
{
    __try { reinterpret_cast<LoopFn>(&loopHook)(&kernel); }
    __except (GetExceptionCode() == kException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
bool catchesDispatch(MSG* msg)
{
    __try { netnotify_dispatch(msg, dispatch, &kernel); }
    __except (GetExceptionCode() == kException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
DWORD WINAPI foreignSafeBoundary(void*)
{
    loopDepth = 1; outerKernel = &kernel;
    return safeBoundary(&kernel) ? 1 : 0;
}

void adapterCases()
{
    begin("inert early loop preserves thiscall result, LastError and nested depth");
    reset(); enabled = 0; loopDepth = 0; outerKernel = nullptr;
    loopMode = NestedLoop;
    SetLastError(kEntryError);
    CHECK(reinterpret_cast<LoopFn>(&loopHook)(&kernel) == kLoopResult);
    CHECK(GetLastError() == kNativeError && !loopDepth && !outerKernel);

    begin("loop SEH unwinds depth and cancels before leaving native loop");
    reset(); registration(); loopDepth = 0; outerKernel = nullptr; loopMode = Throw;
    SetLastError(kEntryError);
    CHECK(catchesLoop());
    CHECK(!loopDepth && !outerKernel && !enabled && !state.observed);

    begin("registration ID zero is valid but initially unarmed");
    reset(); registration(false);
    Ticket ticket = {};
    CHECK(!state.reserve(&ticket) && state.eventId == 0 && addCalls == 1);

    begin("actual callback completion inside foreign callback arms without outer loop");
    reset(); registration(false); loopDepth = 0; outerKernel = nullptr;
    runMode = ForeignNested;
    DWORD out = 0;
    SetLastError(kEntryError);
    CHECK(!runHook(&controller, nullptr, &out, static_cast<unsigned short>(foreignMessage), 0, 0));
    CHECK(GetLastError() == kNativeError && state.armed && nativeCalls == 2 && !callbackDepth);

    begin("synthetic, unmatched and nonzero-payload calls cannot arm");
    reset(); registration(false);
    syntheticDepth = 1;
    runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 0, 0);
    syntheticDepth = 0;
    CHECK(!state.armed);

    runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 7, 8);
    CHECK(!state.armed && seenWp == 7 && seenLp == 8);
    runMode = NoListener;
    CHECK(runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 0, 0));
    CHECK(!state.armed);

    begin("recursive custom dispatcher cannot arm until the outer matching callback returns");
    reset(); registration(false); runMode = CustomNested;
    runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 0, 0);
    CHECK(state.armed && nativeCalls == 2 && !callbackDepth && !customCallbackDepth);

    begin("removal during callback prevents arming stale registration");
    reset(); registration(false); runMode = RemoveDuringRun;
    runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 0, 0);
    CHECK(!state.observed && !state.armed && removeCalls == 1);

    begin("controller-data replacement during callback prevents arming");
    reset(); registration(false); runMode = ChangeData;
    runHook(&controller, nullptr, &out, static_cast<unsigned short>(customMessage), 0, 0);
    CHECK(!state.armed);

    begin("callback exception cancels generation and unwinds both nesting counters");
    reset(); registration(); runMode = Throw;
    CHECK(catchesRun());
    CHECK(!state.observed && !callbackDepth && !customCallbackDepth);

    begin("safe boundary rejects nested/native/modal/disabled-window/wrong-kernel contexts");
    reset(); registration();
    CHECK(safeBoundary(&kernel));
    loopDepth = 2; CHECK(!safeBoundary(&kernel)); loopDepth = 1;
    callbackDepth = 1; CHECK(!safeBoundary(&kernel)); callbackDepth = 0;
    syntheticDepth = 1; CHECK(!safeBoundary(&kernel)); syntheticDepth = 0;
    fixtureNativeDepth = 1; CHECK(!safeBoundary(&kernel)); fixtureNativeDepth = 0;
    CHECK(!safeBoundary(&nestedKernel));
    netnotify_window_event(window, WM_ENTERMENULOOP, 0, 0);
    CHECK(!safeBoundary(&kernel));
    netnotify_window_event(window, WM_EXITMENULOOP, 0, 0);
    EnableWindow(window, FALSE); CHECK(!safeBoundary(&kernel)); EnableWindow(window, TRUE);
    HANDLE worker = CreateThread(nullptr, 0, foreignSafeBoundary, nullptr, 0, nullptr);
    CHECK(worker && WaitForSingleObject(worker, 2000) == WAIT_OBJECT_0);
    DWORD workerResult = 1;
    GetExitCodeThread(worker, &workerResult); CloseHandle(worker);
    CHECK(!workerResult);

    begin("modal private token defers, iteration requeues, outer dispatch translates exactly once");
    reset(); registration();
    MSG msg = postWake();
    const unsigned before = nativeCalls;
    netnotify_window_event(window, WM_ENTERSIZEMOVE, 0, 0);
    SetLastError(kEntryError);
    CHECK(netnotify_dispatch(&msg, dispatch, &kernel) == 0);
    CHECK(GetLastError() == kDispatchError && nativeCalls == before && dispatchCalls == 1);
    CHECK(state.phase == c4net::WakeState::Deferred);
    netnotify_iteration(&kernel);
    CHECK(state.phase == c4net::WakeState::Deferred);
    netnotify_window_event(window, WM_EXITSIZEMOVE, 0, 0);
    SetLastError(kEntryError);
    netnotify_iteration(&kernel);
    CHECK(GetLastError() == kEntryError && state.phase == c4net::WakeState::Queued);
    MSG renewed = {};
    CHECK(PeekMessageA(&renewed, window, privateMessage, privateMessage, PM_REMOVE));
    CHECK(renewed.wParam != msg.wParam);
    CHECK(netnotify_dispatch(&renewed, dispatch, &kernel) == 0x1234);
    CHECK(nativeCalls == before + 1 && dispatchCalls == 2 && seenWp == 0 && seenLp == 0);
    CHECK(state.phase == c4net::WakeState::Idle && !syntheticDepth && deliveries == 1);
    netnotify_dispatch(&renewed, dispatch, &kernel);
    CHECK(nativeCalls == before + 1); // duplicate cannot consume twice

    begin("stale token does not cancel or steal newer pending request");
    reset(); registration();
    msg = postWake();
    SetLastError(kEntryError);
    removeHook(&controller, nullptr, customMessage, nextId);
    CHECK(GetLastError() == kNativeError && !state.observed);
    registration();
    renewed = postWake();
    const uint32_t pending = state.pending;
    const unsigned callsBeforeStale = nativeCalls;
    netnotify_dispatch(&msg, dispatch, &kernel);
    CHECK(state.pending == pending && nativeCalls == callsBeforeStale);
    netnotify_dispatch(&renewed, dispatch, &kernel);
    CHECK(nativeCalls == callsBeforeStale + 1);

    begin("controller destruction cancels request before native destructor");
    reset(); registration(); msg = postWake();
    SetLastError(kEntryError);
    destroyHook(&controller, nullptr);
    CHECK(GetLastError() == kNativeError && destroyCalls == 1 && !state.observed);
    const unsigned beforeDestructorToken = nativeCalls;
    netnotify_dispatch(&msg, dispatch, &kernel);
    CHECK(nativeCalls == beforeDestructorToken);

    begin("window teardown and mismatched controller suppress pending native delivery");
    reset(); registration(); msg = postWake();
    netnotify_window_event(window, WM_DESTROY, 0, 0);
    const unsigned beforeTeardown = nativeCalls;
    netnotify_dispatch(&msg, dispatch, &kernel);
    CHECK(!enabled && !state.observed && nativeCalls == beforeTeardown);
    reset(); registration(); msg = postWake();
    kernelData.controller = nullptr;
    netnotify_dispatch(&msg, dispatch, &kernel);
    CHECK(!state.observed && nativeCalls == 1);

    begin("actual message-batch seam still runs recovery with batching disabled");
    reset(); registration(); msg = postWake();
    g_enabled = 0;
    SetLastError(kEntryError);
    CHECK(!messagebatch_dispatch(&msg, dispatch, &kernel));
    CHECK(GetLastError() == kDispatchError && nativeCalls == 2 && dispatchCalls == 1);
    CHECK(!g_depth && !syntheticDepth && seenWp == 0 && seenLp == 0);

    begin("synthetic callback SEH unwinds synthetic depth and invalidates pending registration");
    reset(); registration(); msg = postWake(); runMode = Throw;
    CHECK(catchesDispatch(&msg));
    CHECK(!syntheticDepth && !callbackDepth && !customCallbackDepth && !state.observed);

    begin("actual periodic worker posts bounded request and stops cleanly on teardown");
    reset(); registration();
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(stop != nullptr);
    stopEvent = stop;
    HANDLE periodicWorker = CreateThread(nullptr, 0, wakeWorker, stop, 0, nullptr);
    CHECK(periodicWorker != nullptr);
    bool found = false;
    const DWORD started = GetTickCount();
    while (!found && GetTickCount() - started < 1800) {
        found = PeekMessageA(&msg, window, privateMessage, privateMessage, PM_REMOVE) != FALSE;
        if (!found) Sleep(5);
    }
    CHECK(found && state.phase == c4net::WakeState::Queued && posts == 1);
    if (found) netnotify_dispatch(&msg, dispatch, &kernel);
    CHECK(deliveries == 1 && nativeCalls == 2);
    stopRecovery();
    CHECK(WaitForSingleObject(periodicWorker, 2000) == WAIT_OBJECT_0);
    CloseHandle(periodicWorker);
    CHECK(stopEvent == nullptr && !enabled && !state.observed);
}

uintptr_t iterationEntry = 0, expectedEsp = 0, seenEsp = 0;
uintptr_t seenEbx = 0, seenEdi = 0, seenEbp = 0, seenEsi = 0, seenEdx = 0, seenEcx = 0;
uint32_t seenFlags = 0, nativeReturn = 0, seenReturn = 0;
unsigned virtualCalls = 0, branch = 0;
__declspec(naked) void nativeVirtual()
{
    __asm {
        pushfd
        pop seenFlags
        inc virtualCalls
        mov seenEsp, esp
        mov seenEbx, ebx
        mov seenEdi, edi
        mov seenEbp, ebp
        mov seenEsi, esi
        mov seenEdx, edx
        mov seenEcx, ecx
        mov eax, nativeReturn
        ret
    }
}
__declspec(naked) void iterationFinish()
{
    __asm {
        mov seenReturn, eax
        popad
        ret
    }
}
__declspec(naked) void iterationSite()
{
    __asm {
        mov eax, [esi]
        mov ecx, esi
        call dword ptr [eax+54h]
        test al, al
        jnz nonzero
        mov branch, 0
        jmp iterationFinish
    nonzero:
        mov branch, 1
        jmp iterationFinish
    }
}
__declspec(naked) void iterationEnter()
{
    __asm {
        pushad
        mov expectedEsp, esp
        mov ebx, 0EB123456h
        mov edi, 0ED123456h
        mov ebp, 0EB987654h
        mov edx, 0ED987654h
        mov esi, offset kernel
        push 247h
        popfd
        jmp dword ptr [iterationEntry]
    }
}
void runIteration(uint32_t result)
{
    nativeReturn = result; virtualCalls = 0;
    SetLastError(kEntryError);
    iterationEnter();
    CHECK(GetLastError() == kEntryError);
    CHECK(virtualCalls == 1 && seenReturn == result && branch == ((result & 0xFF) ? 1u : 0u));
    CHECK(seenEsp + 4 == expectedEsp && seenEcx == reinterpret_cast<uintptr_t>(&kernel));
    CHECK(seenEsi == reinterpret_cast<uintptr_t>(&kernel) && seenEbx == 0xEB123456 &&
          seenEdi == 0xED123456 && seenEbp == 0xEB987654 && seenEdx == 0xED987654);
}
void thunkCases()
{
    begin("actual iteration thunk through private Detours trampoline preserves virtual call and ABI");
    reset(); registration();
    kernelVtable[0x54 / sizeof(void*)] = reinterpret_cast<void*>(&nativeVirtual);
    iterationEntry = reinterpret_cast<uintptr_t>(&iterationSite);
    runIteration(0xAABBCC00);
    const uint32_t baselineFlags = seenFlags;
    const unsigned char bytes[] = {0x8B, 0x06, 0x8B, 0xCE, 0xFF, 0x50, 0x54};
    CHECK(!std::memcmp(reinterpret_cast<void*>(iterationEntry), bytes, sizeof(bytes)));
    originalIteration = reinterpret_cast<PVOID>(iterationEntry);
    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR) status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR) status = DetourAttach(&originalIteration, netnotifyIterationThunk);
    if (status == NO_ERROR) status = DetourTransactionCommit(); else DetourTransactionAbort();
    CHECK(status == NO_ERROR);
    if (status == NO_ERROR) {
        runIteration(0xAABBCC00);
        CHECK((seenFlags & 0xCD5) == (baselineFlags & 0xCD5));
        runIteration(0xAABBCC01);
        CHECK((seenFlags & 0xCD5) == (baselineFlags & 0xCD5));
        MSG pending = postWake();
        netnotify_window_event(window, pending.message, pending.wParam, pending.lParam);
        CHECK(state.phase == c4net::WakeState::Deferred);
        runIteration(0xAABBCC01);
        CHECK(state.phase == c4net::WakeState::Queued && nativeCalls == 1);
        status = DetourTransactionBegin();
        if (status == NO_ERROR) status = DetourUpdateThread(GetCurrentThread());
        if (status == NO_ERROR) status = DetourDetach(&originalIteration, netnotifyIterationThunk);
        if (status == NO_ERROR) status = DetourTransactionCommit(); else DetourTransactionAbort();
        CHECK(status == NO_ERROR);
        CHECK(!std::memcmp(reinterpret_cast<void*>(iterationEntry), bytes, sizeof(bytes)));
        runIteration(0xAABBCC00);
    }
    drainPrivate();
}
} // namespace fixture

int main()
{
    using namespace fixture;
    originalLoop = reinterpret_cast<LoopFn>(&nativeLoop);
    originalAdd = reinterpret_cast<AddFn>(&nativeAdd);
    originalRemove = reinterpret_cast<RemoveFn>(&nativeRemove);
    originalRun = reinterpret_cast<RunFn>(&nativeRun);
    originalDestroy = reinterpret_cast<DestroyFn>(&nativeDestroy);
    customMessage = RegisterWindowMessageA("C4.AdapterFixture.Custom");
    privateMessage = RegisterWindowMessageA("C4.AdapterFixture.Private");
    foreignMessage = RegisterWindowMessageA("C4.AdapterFixture.Foreign");
    WNDCLASSA cls = {};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = GetModuleHandleA(nullptr);
    cls.lpszClassName = "C4NetnotifyAdapterPrivateTest";
    if (!RegisterClassA(&cls)) return 20;
    window = CreateWindowExA(0, cls.lpszClassName, "private hidden fixture", 0,
                            0, 0, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return 21;
    pair.window = window;
    kernelData.pair = &pair;
    kernelData.controller = &controller;
    adapterCases();
    thunkCases();
    enabled = 0;
    DestroyWindow(window);
    std::printf("RESULT=%s cases=%u checks=%u failures=%u; production hooks + Win32 messages + private x86 Detours site\n",
                assertionFailures ? "FAIL" : "PASS", cases, checks, assertionFailures);
    return assertionFailures ? 1 : 0;
}
