// Compile the actual generated HandleMessage/fake_PeekMessage and bridge bodies.
// Dependency stubs isolate routing/coordinate stages; no game DLL or HWND is used.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <cmath>
#include <cstdio>
#include <cstring>

struct {
    LONG ref;
    HWND hwnd;
    int width, height;
    DWORD last_msg_pull_tick;
    POINT cursor;
    struct { int x_adjust, y_adjust; float unscale_x, unscale_y; } mouse;
    struct { struct { int x, y, width, height; } viewport; LONG screen_updated; } render;
    struct { int tick_length; } ticks_limiter;
} g_ddraw;
struct {
    BOOL windowed, adjmouse, hook_peekmessage, devmode, vhack, darkcolony_hack;
    int limiter_type;
} g_config;
LONG g_c4_d2_cursor_ownership;
BOOL g_mouse_locked;
enum { LIMIT_PEEKMESSAGE = 1234, C4TRACE_PEEK_RAW = 12, C4TRACE_PEEK_MAPPED = 13 };
static MSG queued, events[8];
static unsigned eventIds[8], eventCount, zoomCalls, screenCalls, cursorCalls;
static unsigned lockCalls, timeCalls, peekCalls, limiterCalls, failures, cases;
static BOOL screenSucceeds, zoomEnabled;
static const char* caseName;

static BOOL real_ScreenToClient(HWND, LPPOINT point)
{
    ++screenCalls;
    if (!screenSucceeds) return FALSE;
    point->x -= 100;
    point->y -= 200;
    return TRUE;
}
static BOOL fake_GetCursorPos(LPPOINT point)
{
    ++cursorCalls;
    *point = {11, 22};
    return TRUE;
}
static void DDApplySimpleZoomMouse(int* x, int* y, int, int)
{
    ++zoomCalls;
    if (zoomEnabled) { *x += 50; *y += 70; }
}
static void mouse_lock() { ++lockCalls; g_mouse_locked = TRUE; }
static DWORD testTimeGetTime() { ++timeCalls; return 0x12345678; }
#define timeGetTime testTimeGetTime
static void util_limit_game_ticks() { ++limiterCalls; }
static BOOL real_PeekMessageA(LPMSG out, HWND, UINT, UINT, UINT)
{
    ++peekCalls;
    *out = queued;
    return TRUE;
}
static void eventtrace_pulled(unsigned id, const MSG* msg, BOOL ok, unsigned flags)
{
    if (eventCount >= 8 || !ok || flags != PM_REMOVE) { ++failures; return; }
    eventIds[eventCount] = id;
    events[eventCount++] = *msg;
}

#include "message-mapping-extracted.h"

static void check(bool ok, const char* expression, int line)
{
    if (!ok) { ++failures; std::printf("FAIL %s:%d %s\n", caseName, line, expression); }
}
#define CHECK(x) check(!!(x), #x, __LINE__)

static void reset(const char* name)
{
    ++cases;
    caseName = name;
    std::memset(&g_ddraw, 0, sizeof(g_ddraw));
    std::memset(&g_config, 0, sizeof(g_config));
    g_ddraw.ref = 1;
    g_ddraw.hwnd = reinterpret_cast<HWND>(0x1234);
    g_ddraw.width = 800;
    g_ddraw.height = 600;
    g_ddraw.mouse = {10, 20, 0.5f, 0.5f};
    g_ddraw.render.viewport = {10, 20, 1600, 1200};
    g_ddraw.cursor = {7, 9};
    g_config.windowed = g_config.adjmouse = TRUE;
    g_c4_d2_cursor_ownership = 1;
    g_mouse_locked = FALSE;
    screenSucceeds = zoomEnabled = TRUE;
    eventCount = zoomCalls = screenCalls = cursorCalls = lockCalls = timeCalls = peekCalls = limiterCalls = 0;
    std::memset(events, 0, sizeof(events));
}
static MSG make(UINT id)
{
    MSG msg = {};
    msg.hwnd = g_ddraw.hwnd;
    msg.message = id;
    msg.wParam = 42;
    msg.lParam = MAKELPARAM(210, 420);
    msg.pt = {310, 620};
    return msg;
}
static void normalBridgeContract(const MSG& before, const MSG& after)
{
    CHECK(peekCalls == 0 && limiterCalls == 0);
    CHECK(eventCount == 2 && eventIds[0] == C4TRACE_PEEK_RAW && eventIds[1] == C4TRACE_PEEK_MAPPED);
    CHECK(std::memcmp(&before, &events[0], sizeof(MSG)) == 0);
    CHECK(std::memcmp(&after, &events[1], sizeof(MSG)) == 0);
}

int main()
{
    reset("registered net notification maps MSG.pt exactly once, payload unchanged");
    MSG msg = make(0xC010), before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(msg.pt.x == 150 && msg.pt.y == 270);
    CHECK(msg.lParam == before.lParam && msg.wParam == before.wParam);
    CHECK(zoomCalls == 1 && screenCalls == 1 && timeCalls == 1);
    CHECK(g_ddraw.last_msg_pull_tick == 0x12345678);
    normalBridgeContract(before, msg);

    reset("current D2 hook_peekmessage=0 mouse lParam left for WndProc");
    msg = make(WM_MOUSEMOVE); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(msg.message == WM_MOUSEMOVE && msg.pt.x == 150 && msg.pt.y == 270);
    CHECK(msg.lParam == before.lParam && zoomCalls == 1);
    normalBridgeContract(before, msg);

    reset("mouse peek mapping and existing fake_Peek produce identical MSG");
    g_config.hook_peekmessage = TRUE;
    msg = make(WM_MOUSEMOVE); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(GET_X_LPARAM(msg.lParam) == 100 && GET_Y_LPARAM(msg.lParam) == 200);
    CHECK(msg.pt.x == 100 && msg.pt.y == 200 && zoomCalls == 1);
    normalBridgeContract(before, msg);
    const MSG mappedByBridge = msg;
    reset("existing fake_Peek reference path");
    g_config.hook_peekmessage = TRUE;
    queued = before;
    MSG mappedByPeek = {};
    CHECK(fake_PeekMessageA(&mappedByPeek, nullptr, 0, 0, PM_REMOVE));
    CHECK(std::memcmp(&mappedByPeek, &mappedByBridge, sizeof(MSG)) == 0);
    CHECK(peekCalls == 1 && zoomCalls == 1);

    reset("unlocked mouse down preserves native WM_NULL suppression");
    g_config.hook_peekmessage = TRUE;
    g_c4_d2_cursor_ownership = 0;
    msg = make(WM_LBUTTONDOWN); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(msg.message == WM_NULL && lockCalls == 0 && zoomCalls == 1);
    normalBridgeContract(before, msg);

    reset("unlocked mouse up locks once and preserves WM_NULL suppression");
    g_config.hook_peekmessage = TRUE;
    g_c4_d2_cursor_ownership = 0;
    msg = make(WM_LBUTTONUP); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(msg.message == WM_NULL && lockCalls == 1 && g_mouse_locked);
    normalBridgeContract(before, msg);

    reset("D2 cursor ownership prevents legacy unlocked suppression");
    g_config.hook_peekmessage = TRUE;
    msg = make(WM_LBUTTONDOWN); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(msg.message == WM_LBUTTONDOWN && lockCalls == 0);
    normalBridgeContract(before, msg);

    reset("key payload unchanged but independent MSG.pt maps once");
    g_config.hook_peekmessage = TRUE;
    msg = make(WM_KEYDOWN); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(msg.wParam == before.wParam && msg.lParam == before.lParam && msg.message == before.message);
    CHECK(msg.pt.x == 150 && msg.pt.y == 270 && zoomCalls == 1);
    normalBridgeContract(before, msg);

    reset("failed ScreenToClient uses cached cursor without zoom");
    screenSucceeds = FALSE;
    msg = make(0xC010); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(msg.pt.x == 7 && msg.pt.y == 9 && zoomCalls == 0);
    normalBridgeContract(before, msg);

    reset("inactive ddraw leaves message unchanged");
    g_ddraw.ref = 0;
    msg = make(0xC010); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(std::memcmp(&msg, &before, sizeof(MSG)) == 0);
    CHECK(timeCalls == 0 && zoomCalls == 0);
    normalBridgeContract(before, msg);

    reset("mapping does not run fake_Peek limiter even if configured");
    g_config.limiter_type = LIMIT_PEEKMESSAGE;
    g_ddraw.ticks_limiter.tick_length = 6;
    g_ddraw.render.screen_updated = TRUE;
    msg = make(0xC010); before = msg;
    DDMessageBatchMapRemoved(&msg);
    CHECK(g_ddraw.render.screen_updated == TRUE);
    normalBridgeContract(before, msg);

    reset("raw bridge delegates exactly one dequeue without mapping or limiter");
    queued = make(WM_MOUSEMOVE);
    msg = {};
    CHECK(DDMessageBatchPeekRaw(&msg, nullptr, 0, 0, PM_REMOVE));
    CHECK(std::memcmp(&msg, &queued, sizeof(MSG)) == 0);
    CHECK(peekCalls == 1 && zoomCalls == 0 && eventCount == 0 && limiterCalls == 0);

    std::printf("RESULT=%s cases=%u failures=%u (actual extracted mapping bodies; dependency stubs)\n",
                failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
