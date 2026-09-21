// Standalone Win32 contract tests. The production implementation is included
// unchanged except its install path is excluded by C4_MESSAGEBATCH_TESTING.
// Every HWND/message below belongs to this test process, never to the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <vector>
#include <functional>
#include <thread>

#define C4_MESSAGEBATCH_TESTING
#include "../features/messagebatch.cpp"

struct CapturedTrace { unsigned id; uintptr_t object, a, b, c, d; };
static std::vector<CapturedTrace> traceEvents;
extern "C" void c4trace_event(unsigned id, uintptr_t object, uintptr_t a, uintptr_t b,
                              uintptr_t c, uintptr_t d)
{
    const DWORD savedError = GetLastError();
    traceEvents.push_back({id, object, a, b, c, d});
    SetLastError(savedError);
}

static_assert(sizeof(void*) == 4, "Native fixture must be compiled for Win32");

namespace test {
struct HandlePair { HWND hwnd; bool child; unsigned char padding[3]; };
struct KernelData {
    uint32_t unknown;
    HandlePair* pair;
    unsigned char unused[80];
    void* controller;
};
struct Kernel { void* vtable; KernelData* data; };
static_assert(sizeof(HandlePair) == 8, "HWND/bool native pair");
static_assert(sizeof(KernelData) == 92, "Native kernel data layout");
static_assert(offsetof(KernelData, controller) == 0x58, "Native controller offset");
static_assert(sizeof(Kernel) == 8, "Native kernel object layout");

static HWND mainWindow, otherWindow, childWindow;
static HandlePair pair, replacementPair;
static KernelData data, replacementData;
static Kernel kernel;
static uintptr_t controllerA = 0x11111111u, controllerB = 0x22222222u;
static UINT netMessage, queueMessage;
static std::vector<MSG> dispatched;
static std::vector<MSG> mappedRaw;
static std::function<void(const MSG&)> onDispatch;
static std::function<void(UINT)> onPeek;
static std::function<void(UINT)> onWindow;
static LONGLONG fakeTime;
static BOOL counterWorks = TRUE;
static unsigned peekCount, wrongPeekFilter, testCount, failures;
static unsigned throwDispatchNumber;
static LONG timerProcCalls, timerWindowCalls;
static DWORD errorAfterFirst = 0x4511, errorAfterExtra = 0x8822;
static const char* currentCase;

static void check(bool condition, const char* expression, int line)
{
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s line %d: %s (last_error=%lu)\n",
                currentCase, line, expression, GetLastError());
}
#define CHECK(expression) test::check(!!(expression), #expression, __LINE__)

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    messagebatch_window_event(hwnd, message, wp);
    if (message == WM_TIMER) ++timerWindowCalls;
    if (onWindow) onWindow(message);
    return DefWindowProcA(hwnd, message, wp, lp);
}

static BOOL WINAPI counter(LARGE_INTEGER* value)
{
    if (!counterWorks) return FALSE;
    value->QuadPart = fakeTime;
    SetLastError(0xDEAD);
    return TRUE;
}

static BOOL WINAPI peek(LPMSG message, HWND hwnd, UINT minimum, UINT maximum, UINT remove)
{
    ++peekCount;
    if (hwnd || minimum || maximum) ++wrongPeekFilter;
    if (onPeek) onPeek(remove);
    const BOOL found = PeekMessageA(message, hwnd, minimum, maximum, remove);
    SetLastError(0xBEEF);
    return found;
}

// A deliberately visible injected model of the existing HandleMessage seam.
// Tests validate routing/count/raw-vs-mapped data, not a second implementation
// of the renderer's coordinate transform.
static void mapRemoved(LPMSG message)
{
    mappedRaw.push_back(*message);
    message->pt.x += 1000;
    message->pt.y -= 1000;
    if (message->message == WM_MOUSEMOVE) message->lParam = MAKELPARAM(321, 654);
    if (message->message == WM_KEYDOWN) message->wParam = VK_F12;
    SetLastError(0xCAFE);
}

static LRESULT WINAPI dispatch(const MSG* message)
{
    const MSG copy = *message;
    const bool first = dispatched.empty();
    dispatched.push_back(copy);
    if (throwDispatchNumber == dispatched.size()) RaiseException(0xE1420042u, 0, 0, nullptr);
    if (onDispatch) onDispatch(copy);
    const LRESULT result = DispatchMessageA(&copy);
    SetLastError(first ? errorAfterFirst : errorAfterExtra);
    return result;
}

static void clearQueue()
{
    MSG message = {};
    unsigned count = 0;
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (++count > 4096) { CHECK(false); break; }
        if (message.message != WM_QUIT) DispatchMessageA(&message);
    }
}

static MSG makeMessage(UINT id, WPARAM wp = 1, LPARAM lp = 2, HWND hwnd = nullptr)
{
    MSG message = {};
    message.hwnd = hwnd ? hwnd : mainWindow;
    message.message = id;
    message.wParam = wp;
    message.lParam = lp;
    message.time = 0x12345678;
    message.pt.x = 17;
    message.pt.y = 23;
    return message;
}

static void post(UINT id, WPARAM wp = 1, LPARAM lp = 2, HWND hwnd = nullptr)
{
    CHECK(PostMessageA(hwnd ? hwnd : mainWindow, id, wp, lp) != FALSE);
}

static bool take(MSG& message)
{
    return PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE) != FALSE;
}

static bool isHead(UINT id, HWND hwnd)
{
    MSG message = {};
    return PeekMessageA(&message, nullptr, 0, 0, PM_NOREMOVE) &&
           message.message == id && message.hwnd == hwnd;
}

static void reset(const char* name)
{
    currentCase = name;
    ++testCount;
    onDispatch = nullptr;
    onPeek = nullptr;
    onWindow = nullptr;
    clearQueue();
    dispatched.clear();
    mappedRaw.clear();
    traceEvents.clear();
    fakeTime = 1000000;
    counterWorks = TRUE;
    peekCount = wrongPeekFilter = 0;
    throwDispatchNumber = 0;
    timerProcCalls = timerWindowCalls = 0;
    pair = {mainWindow, false, {0, 0, 0}};
    replacementPair = pair;
    std::memset(&data, 0, sizeof(data));
    data.pair = &pair;
    data.controller = &controllerA;
    replacementData = data;
    kernel.vtable = nullptr;
    kernel.data = &data;
    g_enabled = 1;
    g_uiThread = GetCurrentThreadId();
    g_mainHwnd = mainWindow;
    g_netMessage = netMessage;
    g_queueMessage = queueMessage;
    g_frequency.QuadPart = 1000000;
    g_depth = 0;
    g_epoch = 0;
    g_modalMask = 0;
    g_ops.peek = peek;
    g_ops.counter = counter;
    g_ops.mapRemoved = nullptr;
    SetLastError(0x1122);
}

static int run(MSG& message)
{
    return messagebatch_dispatch(&message, dispatch, &kernel);
}

static void finish()
{
    CHECK(wrongPeekFilter == 0);
    CHECK(g_depth == 0);
}

static void baseline()
{
    reset("first always dispatched once and error preserved");
    MSG first = makeMessage(netMessage, 11, 22);
    CHECK(run(first) == 0);
    const DWORD after = GetLastError();
    CHECK(dispatched.size() == 1);
    CHECK(dispatched[0].wParam == 11 && dispatched[0].lParam == 22);
    CHECK(after == errorAfterFirst);
    finish();

    reset("FIFO permitted messages preserve original HWND wp lp");
    post(netMessage, 101, 201);
    post(queueMessage, 102, 202);
    post(netMessage, 103, 203);
    first = makeMessage(queueMessage, 100, 200);
    CHECK(run(first) == 0);
    const DWORD afterBurst = GetLastError();
    CHECK(dispatched.size() == 4);
    for (size_t i = 0; i < dispatched.size(); ++i) {
        CHECK(dispatched[i].hwnd == mainWindow);
        CHECK(dispatched[i].wParam == 100 + i);
        CHECK(dispatched[i].lParam == static_cast<LPARAM>(200 + i));
    }
    CHECK(afterBurst == errorAfterFirst);
    MSG remaining = {};
    // The OS may post unrelated messages to a top-level hidden window while
    // the test runs. Assert our notifications drained, not that USER32 is idle.
    CHECK(!PeekMessageA(&remaining, mainWindow, netMessage, netMessage, PM_NOREMOVE));
    CHECK(!PeekMessageA(&remaining, mainWindow, queueMessage, queueMessage, PM_NOREMOVE));
    finish();
}

static void barriers()
{
    const UINT ids[] = {WM_KEYDOWN, WM_MOUSEMOVE, WM_TIMER, WM_APP + 99,
                        WM_PAINT, WM_CLOSE, WM_ACTIVATEAPP};
    for (UINT id : ids) {
        reset("non-network head is a FIFO barrier");
        post(id, 17, 0);
        post(netMessage, 77, 88);
        MSG first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        CHECK(dispatched.size() == 1);
        CHECK(isHead(id, mainWindow));
        MSG message = {};
        CHECK(take(message) && message.message == id);
        CHECK(take(message) && message.message == netMessage && message.wParam == 77);
        finish();
    }

    for (HWND hwnd : {otherWindow, childWindow}) {
        reset("other or child HWND is a FIFO barrier");
        post(netMessage, 17, 23, hwnd);
        post(queueMessage, 77, 88);
        MSG first = makeMessage(netMessage);
        CHECK(run(first) == 0);
        CHECK(dispatched.size() == 1);
        CHECK(isHead(netMessage, hwnd));
        finish();
    }

    reset("thread message is not dispatched by batch");
    CHECK(PostThreadMessageA(GetCurrentThreadId(), netMessage, 17, 23));
    post(queueMessage);
    MSG first = makeMessage(netMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, nullptr));
    finish();

    reset("WM_QUIT stays for native outer loop");
    PostQuitMessage(31);
    first = makeMessage(netMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    MSG quit = {};
    CHECK(take(quit) && quit.message == WM_QUIT && quit.wParam == 31);
    finish();
}

static void caps()
{
    reset("32 extra callbacks per outer dispatch");
    for (unsigned i = 1; i <= 40; ++i) post(netMessage, i, 0);
    MSG first = makeMessage(queueMessage, 0, 0);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 33);
    MSG next = {};
    CHECK(take(next) && next.wParam == 33);
    finish();

    reset("one millisecond checked between callbacks");
    for (unsigned i = 1; i <= 5; ++i) post(queueMessage, i, 0);
    onDispatch = [](const MSG& message) { if (message.wParam) fakeTime += 1000; };
    first = makeMessage(netMessage, 0, 0);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 2);
    CHECK(take(next) && next.wParam == 2);
    finish();

    reset("QPC failure fails closed after first");
    counterWorks = FALSE;
    post(netMessage);
    first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    finish();

    for (LONGLONG peekDelay : {1000LL, 1001LL}) {
        reset("nonremoving Peek consumes full budget: no removal or mapping");
        post(netMessage, 77, 0);
        g_ops.mapRemoved = mapRemoved;
        unsigned removeCalls = 0;
        onPeek = [&](UINT flags) {
            if (flags & PM_REMOVE) ++removeCalls;
            else fakeTime += peekDelay;
        };
        first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        const DWORD after = GetLastError();
        CHECK(dispatched.size() == 1);
        CHECK(removeCalls == 0 && mappedRaw.empty());
        CHECK(isHead(netMessage, mainWindow));
        CHECK(after == errorAfterFirst);
        finish();
    }

    reset("nonremoving Peek below budget permits one callback before next deadline");
    post(netMessage, 77, 0);
    post(queueMessage, 88, 0);
    onPeek = [](UINT flags) { if (!(flags & PM_REMOVE)) fakeTime += 999; };
    first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 2 && dispatched[1].wParam == 77);
    CHECK(isHead(queueMessage, mainWindow));
    finish();

    reset("QPC failure after nonremoving Peek leaves message queued");
    post(netMessage, 77, 0);
    onPeek = [](UINT flags) { if (!(flags & PM_REMOVE)) counterWorks = FALSE; };
    first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    finish();
}

static void mapping()
{
    reset("normal eligible extras mapped exactly once, original first unchanged");
    g_ops.mapRemoved = mapRemoved;
    post(netMessage, 77, 177);
    post(queueMessage, 88, 188);
    MSG first = makeMessage(queueMessage, 11, 22);
    const MSG originalFirst = first;
    CHECK(run(first) == 0);
    const DWORD after = GetLastError();
    CHECK(mappedRaw.size() == 2 && dispatched.size() == 3);
    CHECK(std::memcmp(&first, &originalFirst, sizeof(MSG)) == 0);
    CHECK(std::memcmp(&dispatched[0], &originalFirst, sizeof(MSG)) == 0);
    for (size_t i = 0; i < mappedRaw.size() && i + 1 < dispatched.size(); ++i) {
        CHECK(dispatched[i + 1].message == mappedRaw[i].message);
        CHECK(dispatched[i + 1].hwnd == mappedRaw[i].hwnd);
        CHECK(dispatched[i + 1].wParam == mappedRaw[i].wParam);
        CHECK(dispatched[i + 1].lParam == mappedRaw[i].lParam);
        CHECK(dispatched[i + 1].pt.x == mappedRaw[i].pt.x + 1000);
        CHECK(dispatched[i + 1].pt.y == mappedRaw[i].pt.y - 1000);
    }
    CHECK(after == errorAfterFirst);
    finish();

    for (UINT changedId : {WM_MOUSEMOVE, WM_KEYDOWN}) {
        reset("changed head mouse/key mapped once before true dispatch, trace redacted");
        g_ops.mapRemoved = mapRemoved;
        post(netMessage, 77, 177);
        post(changedId, 88, 188);
        post(queueMessage, 99, 199);
        bool raced = false;
        onPeek = [&](UINT flags) {
            if (!raced && (flags & PM_REMOVE)) {
                raced = true;
                MSG consumed = {};
                CHECK(PeekMessageA(&consumed, nullptr, 0, 0, PM_REMOVE));
                CHECK(consumed.message == netMessage && consumed.wParam == 77);
            }
        };
        first = makeMessage(queueMessage, 11, 22);
        const MSG unchanged = first;
        CHECK(run(first) == 0);
        const DWORD afterRace = GetLastError();
        CHECK(raced);
        CHECK(mappedRaw.size() == 1 && dispatched.size() == 2);
        if (mappedRaw.size() != 1 || dispatched.size() != 2) { finish(); continue; }
        CHECK(std::memcmp(&first, &unchanged, sizeof(MSG)) == 0);
        CHECK(std::memcmp(&dispatched[0], &unchanged, sizeof(MSG)) == 0);
        CHECK(mappedRaw[0].message == changedId && mappedRaw[0].wParam == 88 && mappedRaw[0].lParam == 188);
        CHECK(dispatched[1].message == changedId);
        CHECK(dispatched[1].pt.x == mappedRaw[0].pt.x + 1000);
        CHECK(dispatched[1].pt.y == mappedRaw[0].pt.y - 1000);
        CHECK(dispatched[1].wParam == (changedId == WM_KEYDOWN ? VK_F12 : 88));
        CHECK(dispatched[1].lParam == (changedId == WM_MOUSEMOVE ? MAKELPARAM(321, 654) : 188));
        unsigned dispatchRecords = 0;
        for (const auto& event : traceEvents) {
            if (event.id == 133 && event.a == changedId) {
                ++dispatchRecords;
                CHECK(event.b == 0 && event.c == 0);
            }
        }
        CHECK(dispatchRecords == 1);
        CHECK(isHead(queueMessage, mainWindow));
        CHECK(afterRace == errorAfterFirst);
        finish();
    }

    for (UINT id : {WM_MOUSEMOVE, WM_KEYDOWN, WM_TIMER}) {
        reset("untouched FIFO barrier is not mapped");
        g_ops.mapRemoved = mapRemoved;
        post(id, 88, 0);
        post(netMessage, 99, 0);
        first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        CHECK(mappedRaw.empty() && dispatched.size() == 1);
        CHECK(isHead(id, mainWindow));
        finish();
    }

    reset("unexpected removed WM_QUIT is not mapped");
    g_ops.mapRemoved = mapRemoved;
    post(netMessage, 77, 0);
    bool raced = false;
    onPeek = [&](UINT flags) {
        if (!raced && (flags & PM_REMOVE)) {
            raced = true;
            MSG consumed = {};
            CHECK(PeekMessageA(&consumed, nullptr, 0, 0, PM_REMOVE));
            PostQuitMessage(47);
        }
    };
    first = makeMessage(queueMessage);
    CHECK(run(first) == 1);
    CHECK(first.message == WM_QUIT && first.wParam == 47);
    CHECK(mappedRaw.empty() && dispatched.size() == 1);
    finish();
}

static void contextChanges()
{
    const std::function<void()> changes[] = {
        [] { kernel.data = &replacementData; },
        [] { data.controller = &controllerB; },
        [] { data.pair = &replacementPair; },
        [] { pair.hwnd = otherWindow; },
        [] { pair.child = !pair.child; }
    };
    for (const auto& change : changes) {
        reset("context changed by first stops before any peek/remove");
        post(netMessage, 77, 0);
        onDispatch = [&](const MSG&) { change(); };
        MSG first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        CHECK(dispatched.size() == 1);
        CHECK(isHead(netMessage, mainWindow));
        finish();

        reset("context changed by extra stops further batch");
        post(netMessage, 77, 0);
        post(netMessage, 88, 0);
        onDispatch = [&](const MSG& message) { if (message.wParam == 77) change(); };
        first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        CHECK(dispatched.size() == 2);
        MSG next = {};
        CHECK(take(next) && next.wParam == 88);
        finish();

        reset("sent callback during nonremoving peek invalidates context");
        post(netMessage, 77, 0);
        bool changed = false;
        onPeek = [&](UINT flags) {
            if (!changed && !(flags & PM_REMOVE)) { changed = true; change(); }
        };
        first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        CHECK(changed);
        CHECK(dispatched.size() == 1);
        CHECK(isHead(netMessage, mainWindow));
        finish();
    }
}

static void nested()
{
    reset("nested first dispatch cannot recursively batch");
    post(netMessage, 77, 0);
    bool nestedRan = false;
    onDispatch = [&](const MSG& message) {
        if (message.wParam == 11 && !nestedRan) {
            nestedRan = true;
            MSG inner = makeMessage(queueMessage, 22, 0);
            CHECK(run(inner) == 0);
            CHECK(dispatched.size() == 2);
            CHECK(isHead(netMessage, mainWindow));
        }
    };
    MSG first = makeMessage(netMessage, 11, 0);
    CHECK(run(first) == 0);
    CHECK(nestedRan);
    CHECK(dispatched.size() == 2); // epoch rejects outer continuation after nested loop
    CHECK(dispatched[0].wParam == 11 && dispatched[1].wParam == 22);
    CHECK(isHead(netMessage, mainWindow));
    finish();

    reset("nested during extra cancels remaining outer batch");
    post(netMessage, 77, 0);
    post(queueMessage, 88, 0);
    nestedRan = false;
    onDispatch = [&](const MSG& message) {
        if (message.wParam == 77 && !nestedRan) {
            nestedRan = true;
            MSG inner = makeMessage(queueMessage, 22, 0);
            CHECK(run(inner) == 0);
        }
    };
    first = makeMessage(netMessage, 11, 0);
    CHECK(run(first) == 0);
    CHECK(nestedRan && dispatched.size() == 3);
    CHECK(dispatched[1].wParam == 77 && dispatched[2].wParam == 22);
    MSG remaining = {};
    CHECK(take(remaining) && remaining.wParam == 88);
    finish();
}

static void failClosed()
{
    const std::function<void()> invalid[] = {
        [] { g_enabled = 0; },
        [] { g_uiThread = GetCurrentThreadId() + 1; },
        [] { g_mainHwnd = otherWindow; },
        [] { g_netMessage = 0; },
        [] { g_queueMessage = 0; },
        [] { g_frequency.QuadPart = 0; },
        [] { kernel.data = nullptr; },
        [] { data.pair = nullptr; },
        [] { data.controller = nullptr; }
    };
    for (const auto& invalidate : invalid) {
        reset("invalid configuration or context still dispatches first once");
        post(netMessage, 77, 0);
        invalidate();
        MSG first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        CHECK(dispatched.size() == 1);
        CHECK(isHead(netMessage, mainWindow));
        finish();
    }

    reset("unreadable kernel pointer fails closed without crash");
    post(netMessage, 77, 0);
    MSG first = makeMessage(queueMessage);
    CHECK(messagebatch_dispatch(&first, dispatch, reinterpret_cast<void*>(1)) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    finish();

    reset("disabled private HWND fails closed");
    EnableWindow(mainWindow, FALSE);
    post(netMessage, 77, 0);
    first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    EnableWindow(mainWindow, TRUE);
    finish();
}

static void removeRaces()
{
    reset("unexpected removed nonquit is dispatched once then batch stops");
    post(netMessage, 77, 0);
    post(WM_APP + 99, 88, 0);
    post(queueMessage, 99, 0);
    bool raced = false;
    onPeek = [&](UINT flags) {
        if (!raced && (flags & PM_REMOVE)) {
            raced = true;
            MSG consumed = {};
            CHECK(PeekMessageA(&consumed, nullptr, 0, 0, PM_REMOVE));
            CHECK(consumed.message == netMessage && consumed.wParam == 77);
        }
    };
    MSG first = makeMessage(queueMessage, 1, 0);
    CHECK(run(first) == 0);
    CHECK(raced);
    CHECK(dispatched.size() == 2);
    CHECK(dispatched[1].message == WM_APP + 99 && dispatched[1].wParam == 88);
    bool sawRedacted = false;
    for (const auto& event : traceEvents) {
        if (event.id == 133 && event.a == WM_APP + 99) {
            sawRedacted = true;
            CHECK(event.b == 0 && event.c == 0);
        }
    }
    CHECK(sawRedacted);
    CHECK(isHead(queueMessage, mainWindow));
    finish();

    reset("unexpected removed quit returned to native quit branch");
    post(netMessage, 77, 0);
    raced = false;
    onPeek = [&](UINT flags) {
        if (!raced && (flags & PM_REMOVE)) {
            raced = true;
            MSG consumed = {};
            CHECK(PeekMessageA(&consumed, nullptr, 0, 0, PM_REMOVE));
            PostQuitMessage(47);
        }
    };
    first = makeMessage(queueMessage, 1, 0);
    CHECK(run(first) == 1);
    const DWORD after = GetLastError();
    CHECK(raced);
    CHECK(dispatched.size() == 1);
    CHECK(first.message == WM_QUIT && first.wParam == 47);
    CHECK(after == errorAfterFirst);
    finish();

    reset("changed same-class head delivered once then stop");
    post(netMessage, 77, 0);
    post(netMessage, 88, 0);
    post(queueMessage, 99, 0);
    raced = false;
    onPeek = [&](UINT flags) {
        if (!raced && (flags & PM_REMOVE)) {
            raced = true;
            MSG consumed = {};
            CHECK(PeekMessageA(&consumed, nullptr, 0, 0, PM_REMOVE));
        }
    };
    first = makeMessage(queueMessage, 1, 0);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 2 && dispatched[1].wParam == 88);
    CHECK(isHead(queueMessage, mainWindow));
    finish();

    reset("context changed inside removing peek never discards removed message");
    post(netMessage, 77, 0);
    post(queueMessage, 88, 0);
    raced = false;
    onPeek = [&](UINT flags) {
        if (!raced && (flags & PM_REMOVE)) {
            raced = true;
            data.controller = &controllerB;
        }
    };
    first = makeMessage(queueMessage, 1, 0);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 2 && dispatched[1].wParam == 77);
    CHECK(isHead(queueMessage, mainWindow));
    finish();
}

static void lifecycle()
{
    const UINT boundary[] = {WM_ENTERMENULOOP, WM_EXITMENULOOP, WM_ENTERSIZEMOVE,
                             WM_EXITSIZEMOVE, WM_ENABLE, WM_ACTIVATE, WM_ACTIVATEAPP,
                             WM_SHOWWINDOW, WM_CLOSE, WM_DESTROY, WM_QUERYENDSESSION,
                             WM_ENDSESSION};
    for (UINT id : boundary) {
        reset("lifecycle observation cancels batch after first dispatch");
        post(netMessage, 77, 0);
        onDispatch = [id](const MSG&) { messagebatch_window_event(mainWindow, id, 0); };
        MSG first = makeMessage(queueMessage);
        CHECK(run(first) == 0);
        CHECK(dispatched.size() == 1);
        CHECK(isHead(netMessage, mainWindow));
        finish();
    }

    reset("modal entry plus exit within first remains epoch barrier");
    post(netMessage, 77, 0);
    onDispatch = [](const MSG&) {
        messagebatch_window_event(mainWindow, WM_ENTERMENULOOP, 0);
        messagebatch_window_event(mainWindow, WM_EXITMENULOOP, 0);
    };
    MSG first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(g_modalMask == 0 && dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    finish();

    reset("already modal fails closed");
    messagebatch_window_event(mainWindow, WM_ENTERMENULOOP, 0);
    post(netMessage, 77, 0);
    first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    finish();

    reset("controller vtable identity change is a lifetime boundary");
    post(netMessage, 77, 0);
    const auto saved = controllerA;
    onDispatch = [](const MSG&) { controllerA = 0x33333333; };
    first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    controllerA = saved;
    finish();

    reset("unrelated HWND lifecycle does not cancel main batch");
    post(netMessage, 77, 0);
    onDispatch = [](const MSG&) { messagebatch_window_event(otherWindow, WM_ACTIVATE, 0); };
    first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 2);
    finish();
}

static void realSentCallback()
{
    reset("real cross-thread sent callback during Peek invalidates context");
    post(netMessage, 77, 0);
    bool sentSeen = false, startedSender = false;
    volatile LONG sendSucceeded = 0;
    std::thread sender;
    onWindow = [&](UINT id) {
        if (id == WM_APP + 42) {
            sentSeen = true;
            data.controller = &controllerB;
        }
    };
    onPeek = [&](UINT flags) {
        if (!startedSender && !(flags & PM_REMOVE)) {
            startedSender = true;
            sender = std::thread([&]() {
                DWORD_PTR result = 0;
                const BOOL ok = SendMessageTimeoutA(otherWindow, WM_APP + 42, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0;
                InterlockedExchange(&sendSucceeded, ok);
            });
            const ULONGLONG began = GetTickCount64();
            while (!(HIWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) &&
                   GetTickCount64() - began < 1000) Sleep(1);
            CHECK((HIWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) != 0);
        }
    };
    MSG first = makeMessage(queueMessage);
    CHECK(run(first) == 0);
    if (sender.joinable()) sender.join();
    CHECK(startedSender && sentSeen && sendSucceeded);
    CHECK(dispatched.size() == 1);
    CHECK(isHead(netMessage, mainWindow));
    finish();
}

static void CALLBACK timerProc(HWND, UINT, UINT_PTR, DWORD)
{
    ++timerProcCalls;
}

static void realTimer()
{
    reset("real TIMERPROC message remains a barrier then uses normal DispatchMessage");
    const UINT_PTR timer = SetTimer(mainWindow, 0x451, 10, timerProc);
    CHECK(timer != 0);
    MSG timerMessage = {};
    BOOL ready = FALSE;
    const ULONGLONG began = GetTickCount64();
    while (!ready && GetTickCount64() - began < 1000) {
        ready = PeekMessageA(&timerMessage, mainWindow, WM_TIMER, WM_TIMER, PM_NOREMOVE);
        if (!ready) Sleep(1);
    }
    CHECK(ready && timerMessage.lParam != 0);
    MSG first = makeMessage(netMessage);
    CHECK(run(first) == 0);
    CHECK(dispatched.size() == 1);
    CHECK(timerProcCalls == 0 && timerWindowCalls == 0);
    MSG remaining = {};
    CHECK(PeekMessageA(&remaining, mainWindow, WM_TIMER, WM_TIMER, PM_REMOVE));
    CHECK(remaining.message == WM_TIMER && remaining.lParam == timerMessage.lParam);
    // Feed the already removed timer as the next first message, as the outer
    // native loop does. A direct WndProc call would hit timerWindowCalls instead.
    CHECK(run(remaining) == 0);
    CHECK(timerProcCalls == 1 && timerWindowCalls == 0);
    KillTimer(mainWindow, timer);
    finish();
}

static bool catchDispatchException(MSG* message)
{
    __try { messagebatch_dispatch(message, dispatch, &kernel); }
    __except (GetExceptionCode() == 0xE1420042u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}

static void exceptional()
{
    reset("first dispatch SEH propagates and restores TLS depth");
    post(netMessage, 77, 0);
    throwDispatchNumber = 1;
    MSG first = makeMessage(queueMessage);
    SetLastError(0x1122);
    CHECK(catchDispatchException(&first));
    const DWORD firstError = GetLastError();
    CHECK(dispatched.size() == 1);
    CHECK(firstError == 0x1122);
    CHECK(isHead(netMessage, mainWindow));
    finish();

    reset("extra dispatch SEH propagates and preserves first LastError");
    post(netMessage, 77, 0);
    post(queueMessage, 88, 0);
    throwDispatchNumber = 2;
    first = makeMessage(queueMessage);
    CHECK(catchDispatchException(&first));
    const DWORD extraError = GetLastError();
    CHECK(dispatched.size() == 2);
    CHECK(extraError == errorAfterFirst);
    CHECK(isHead(queueMessage, mainWindow));
    CHECK(!traceEvents.empty() && traceEvents.back().id == 134 && traceEvents.back().d == 1);
    finish();
}
} // namespace test

int main()
{
    using namespace test;
    currentCase = "private fixture setup";
    WNDCLASSA wc = {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "C4MessageBatchIsolatedContractTest";
    if (!RegisterClassA(&wc)) return 10;
    mainWindow = CreateWindowExA(0, wc.lpszClassName, "private main", 0,
                                 0, 0, 1, 1, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    otherWindow = CreateWindowExA(0, wc.lpszClassName, "private other", 0,
                                  0, 0, 1, 1, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    childWindow = CreateWindowExA(0, wc.lpszClassName, "private child", WS_CHILD,
                                  0, 0, 1, 1, mainWindow, nullptr, wc.hInstance, nullptr);
    if (!mainWindow || !otherWindow || !childWindow) return 11;
    netMessage = RegisterWindowMessageA("MIDGARD NETMSG");
    queueMessage = RegisterWindowMessageA("MQ_COMMANDQUEUE2");
    if (!netMessage || !queueMessage || netMessage == queueMessage) return 12;
    baseline();
    barriers();
    caps();
    mapping();
    contextChanges();
    nested();
    failClosed();
    removeRaces();
    lifecycle();
    realSentCallback();
    realTimer();
    exceptional();
    onDispatch = nullptr;
    onPeek = nullptr;
    onWindow = nullptr;
    clearQueue();
    DestroyWindow(childWindow);
    DestroyWindow(otherWindow);
    DestroyWindow(mainWindow);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    std::printf("RESULT=%s cases=%u failures=%u (private hidden windows only)\n",
                failures ? "FAIL" : "PASS", testCount, failures);
    return failures ? 1 : 0;
}
