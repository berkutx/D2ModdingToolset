#define WIN32_LEAN_AND_MEAN
#include "../features/ownedwindowtimer.h"
#include <cstdio>
#include <cstdlib>

namespace {
OwnedWindowTimer timer;
int ticks = 0, nativeTicks = 0, checks = 0;
HWND lastTarget = nullptr;
DWORD callbackThread = 0;
bool stopInCallback = false;
HINSTANCE module;

void check(bool result, const char* description)
{
    ++checks;
    std::printf("%s %02d %s\n", result ? "PASS" : "FAIL", checks, description);
    if (!result)
        std::exit(1);
}

void tick(HWND target)
{
    ++ticks;
    lastTarget = target;
    callbackThread = GetCurrentThreadId();
    if (stopInCallback)
        timer.release(target);
}

LRESULT CALLBACK targetProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_TIMER) {
        ++nativeTicks;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

HWND createTarget()
{
    return CreateWindowExW(0, L"C4TimerTestTarget", L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, module, nullptr);
}

void pump(DWORD duration)
{
    const DWORD start = GetTickCount();
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        Sleep(1);
    } while (GetTickCount() - start < duration);
}

// Uses actual DispatchMessage, including a stale message already copied out of
// the native queue. No direct calls to the helper's private WndProc.
void dispatchTimer(HWND window, UINT_PTR id)
{
    MSG message = {};
    message.hwnd = window;
    message.message = WM_TIMER;
    message.wParam = id;
    DispatchMessageW(&message);
}

DWORD WINAPI foreignThread(void*)
{
    HWND foreignTarget = createTarget();
    const bool accepted = timer.ensure(foreignTarget, module, tick, 15);
    timer.stop();
    timer.release(foreignTarget);
    DestroyWindow(foreignTarget);
    return accepted ? 1 : 0;
}
}

int main()
{
    module = GetModuleHandleW(nullptr);
    WNDCLASSW cls = {};
    cls.lpfnWndProc = targetProc;
    cls.hInstance = module;
    cls.lpszClassName = L"C4TimerTestTarget";
    check(RegisterClassW(&cls) != 0, "register private target windows");
    HWND first = createTarget();
    HWND second = createTarget();
    check(first && second, "create private targets without touching a game");
    check(!timer.ensure(nullptr, module, tick) &&
          !timer.ensure(first, nullptr, tick) && !timer.ensure(first, module, nullptr),
          "invalid bindings rejected");
    check(timer.ensure(first, module, tick, 15), "arm timer on wrapper-owned HWND");
    const HWND initialWindow = timer.window();
    const UINT_PTR initialId = timer.timerId();
    check(initialWindow != first &&
          FindWindowExW(HWND_MESSAGE, nullptr, L"C4dll-R.OwnedIdleTimer.1", nullptr) == initialWindow &&
          GetWindowThreadProcessId(initialWindow, nullptr) == GetCurrentThreadId(),
          "timer namespace is separate and stays on target UI thread");
    check(timer.ensure(first, module, tick, 15) && timer.window() == initialWindow &&
          timer.timerId() == initialId, "repeated ensure does not reset or duplicate timer");
    pump(80);
    check(ticks > 0 && lastTarget == first && callbackThread == GetCurrentThreadId(),
          "real WM_TIMER dispatch invokes callback for correct target on UI thread");

    check(SetTimer(first, initialId, 15, nullptr) &&
          SetTimer(first, 0xC4D7, 15, nullptr), "arm native same-ID and former C4-ID timers");
    const int beforeTicks = ticks;
    pump(80);
    check(nativeTicks > 0 && ticks > beforeTicks,
          "native and wrapper timers both run despite matching numeric IDs");
    KillTimer(first, initialId);
    KillTimer(first, 0xC4D7);
    const int beforeKill = ticks;
    pump(60);
    check(ticks > beforeKill, "native KillTimer cannot remove wrapper timer");

    const int beforeWrong = ticks;
    dispatchTimer(timer.window(), timer.timerId() + 1);
    timer.release(second);
    check(ticks == beforeWrong && timer.window() == initialWindow,
          "wrong timer ID and wrong-target release do not alter active binding");

    HANDLE worker = CreateThread(nullptr, 0, foreignThread, nullptr, 0, nullptr);
    check(worker != nullptr, "create foreign UI thread test");
    check(WaitForSingleObject(worker, 2000) == WAIT_OBJECT_0, "foreign UI thread returns");
    DWORD workerResult = 1;
    GetExitCodeThread(worker, &workerResult);
    CloseHandle(worker);
    check(workerResult == 0 && timer.window() == initialWindow && timer.target() == first,
          "foreign-thread ensure/stop/release cannot steal timer ownership");

    check(timer.ensure(second, module, tick, 15) && timer.timerId() != initialId,
          "rebind destroys old timer and gives next target a fresh generation");
    const int beforeStale = ticks;
    dispatchTimer(timer.window(), initialId);
    check(ticks == beforeStale, "copied stale generation cannot tick a rearmed or recycled HWND");
    pump(50);
    check(lastTarget == second && ticks > beforeStale, "rebound timer services only the new target");

    MSG copied = {};
    copied.hwnd = timer.window();
    copied.message = WM_TIMER;
    copied.wParam = timer.timerId();
    timer.release(second);
    const int beforeStop = ticks;
    DispatchMessageW(&copied);
    pump(35);
    check(!timer.window() && ticks == beforeStop, "queued/copied timer after release cannot invoke callback");

    SetTimer(first, 0xC4D7, 15, nullptr);
    check(timer.ensure(first, module, tick, 15), "restart wrapper alongside native timer");
    timer.stop();
    const int beforeNative = nativeTicks;
    pump(50);
    check(nativeTicks > beforeNative, "wrapper teardown leaves native same-namespace timer intact");
    KillTimer(first, 0xC4D7);

    check(timer.ensure(first, module, tick, 15), "restart before target destruction");
    const HWND orphanWindow = timer.window();
    const UINT_PTR orphanId = timer.timerId();
    DestroyWindow(first); // Deliberately bypass featuremenu's normal release seam.
    first = nullptr;
    const int beforeDestroy = ticks;
    dispatchTimer(orphanWindow, orphanId);
    check(!timer.window() && ticks == beforeDestroy,
          "target property lifetime rejects destroyed target and cleans orphan timer");

    check(timer.ensure(second, module, tick, 15), "restart before timer-window destruction");
    const UINT_PTR beforeExternalDestroy = timer.timerId();
    DestroyWindow(timer.window());
    check(!timer.window() && timer.ensure(second, module, tick, 15) &&
          timer.timerId() != beforeExternalDestroy,
          "external helper destruction clears binding and supports clean restart");

    // The native main pump uses HWND=NULL. A target-filtered drain must not
    // accidentally consume the private timer; the later unfiltered pump does.
    const int beforeFilter = ticks;
    PostMessageW(timer.window(), WM_TIMER, timer.timerId(), 0);
    MSG filtered;
    while (PeekMessageW(&filtered, second, 0, 0, PM_REMOVE))
        DispatchMessageW(&filtered);
    check(ticks == beforeFilter, "target-filtered queue cleanup cannot consume helper messages");
    pump(30);
    check(ticks > beforeFilter, "normal HWND=NULL pump includes wrapper message-only window");

    stopInCallback = true;
    const int beforeSelfStop = ticks;
    dispatchTimer(timer.window(), timer.timerId());
    pump(35);
    check(!timer.window() && ticks == beforeSelfStop + 1,
          "callback can release its own timer without duplicate later invocation");
    DestroyWindow(second);
    std::printf("PASS: %d real Win32 checks; private test windows only\n", checks);
    return 0;
}
