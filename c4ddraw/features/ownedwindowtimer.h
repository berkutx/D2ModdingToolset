#pragma once

#include <Windows.h>

// A timer namespace owned by C4, never by the application's HWND. All lifetime
// operations and callbacks run on the target's UI thread. No extra message pump,
// SendMessage or TIMERPROC is involved: ordinary DispatchMessage delivers ticks.
class OwnedWindowTimer {
public:
    using Callback = void (*)(HWND);
    OwnedWindowTimer() = default;
    OwnedWindowTimer(const OwnedWindowTimer&) = delete;
    OwnedWindowTimer& operator=(const OwnedWindowTimer&) = delete;

    bool ensure(HWND target, HINSTANCE module, Callback callback, UINT period = 32)
    {
        DWORD process = 0;
        const DWORD thread = GetCurrentThreadId();
        if (!target || !module || !callback ||
            GetWindowThreadProcessId(target, &process) != thread ||
            process != GetCurrentProcessId())
            return false;

        const LONG previous = InterlockedCompareExchange(
            &ownerThread_, static_cast<LONG>(thread), 0);
        if (previous && static_cast<DWORD>(previous) != thread)
            return false;

        if (target_ == target && window_ && timerId_ &&
            GetPropW(target, propertyName()) == this)
            return true;
        stop();

        // Do not wrap/reuse a generation: a removed WM_TIMER can already have
        // been copied out of the queue, including across a destroyed HWND.
        if (nextTimerId_ == static_cast<UINT_PTR>(-1))
            return false;
        const UINT_PTR generation = ++nextTimerId_;

        WNDCLASSW cls = {};
        cls.lpfnWndProc = windowProc;
        cls.hInstance = module;
        cls.lpszClassName = className();
        if (!RegisterClassW(&cls)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||
                !GetClassInfoW(module, className(), &cls) ||
                cls.lpfnWndProc != windowProc)
                return false;
        }
        // A property belongs to the window's lifetime, unlike its recyclable
        // integer handle. Refuse an existing binding instead of replacing it.
        if (GetPropW(target, propertyName()) ||
            !SetPropW(target, propertyName(), this))
            return false;
        target_ = target;
        callback_ = callback;
        window_ = CreateWindowExW(0, className(), L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, module, this);
        if (!window_) {
            stop();
            return false;
        }
        timerId_ = generation;
        if (!SetTimer(window_, timerId_, period, nullptr)) {
            stop();
            return false;
        }
        return true;
    }

    void release(HWND target)
    {
        if (GetCurrentThreadId() == static_cast<DWORD>(ownerThread_) && target == target_)
            stop();
    }

    void stop()
    {
        if (GetCurrentThreadId() != static_cast<DWORD>(ownerThread_))
            return;
        const HWND oldWindow = window_;
        const HWND oldTarget = target_;
        const UINT_PTR oldTimer = timerId_;
        // Invalidate before Win32 teardown, which itself calls the WndProc.
        window_ = nullptr;
        target_ = nullptr;
        callback_ = nullptr;
        timerId_ = 0;
        if (oldTarget && GetPropW(oldTarget, propertyName()) == this)
            RemovePropW(oldTarget, propertyName());
        if (oldWindow) {
            if (oldTimer)
                KillTimer(oldWindow, oldTimer);
            DestroyWindow(oldWindow);
        }
    }

    HWND window() const { return window_; }
    HWND target() const { return target_; }
    UINT_PTR timerId() const { return timerId_; }

private:
    static const wchar_t* className() { return L"C4dll-R.OwnedIdleTimer.1"; }
    static const wchar_t* propertyName() { return L"C4dll-R.OwnedIdleTimer.Target.1"; }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<OwnedWindowTimer*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<OwnedWindowTimer*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && msg == WM_TIMER) {
            if (self->window_ == hwnd && self->timerId_ && wp == self->timerId_ &&
                lp == 0 && GetCurrentThreadId() == static_cast<DWORD>(self->ownerThread_)) {
                DWORD process = 0;
                const HWND target = self->target_;
                if (target && GetPropW(target, propertyName()) == self &&
                    GetWindowThreadProcessId(target, &process) == GetCurrentThreadId() &&
                    process == GetCurrentProcessId() && self->callback_)
                    self->callback_(target);
                else
                    self->stop();
            }
            return 0;
        }
        if (self && msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (self->window_ == hwnd) {
                if (self->target_ && GetPropW(self->target_, propertyName()) == self)
                    RemovePropW(self->target_, propertyName());
                self->window_ = nullptr;
                self->target_ = nullptr;
                self->callback_ = nullptr;
                self->timerId_ = 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    volatile LONG ownerThread_ = 0;
    HWND window_ = nullptr;
    HWND target_ = nullptr;
    Callback callback_ = nullptr;
    UINT_PTR timerId_ = 0;
    UINT_PTR nextTimerId_ = 0;
};
