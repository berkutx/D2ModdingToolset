/*
 * Exact-Russobit natural screen-loop event used as the sole ordered UI seam.
 */

#include "uiframedispatcher.h"
#include "executablefingerprint.h"
#include "hooks.h"
#include "netintercept.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace uiframedispatcher {

namespace {

// sub_5629CA is called by exact-Russobit sub_56288A once per natural screen-loop
// iteration, before that loop attempts PeekMessage/GetMessage dispatch.
constexpr std::uintptr_t kUiFrameVA = 0x005629CA;
constexpr std::uint8_t kUiFrameExpected[16] = {
    0x55, 0x8B, 0xEC, 0x51, 0x51, 0x56, 0x8B, 0x75,
    0x0C, 0x56, 0xFF, 0x15, 0x84, 0xE3, 0x6C, 0x00,
};

using UiFrameFn = LONG(__stdcall*)(HWND, LPPOINT, LONG*);
UiFrameFn g_originalUiFrame = reinterpret_cast<UiFrameFn>(kUiFrameVA);

std::mutex g_requestMutex;
std::atomic<bool> g_requested{false};
std::atomic<bool> g_installed{false};
thread_local bool g_dispatchActive = false;

#ifdef D2_TESTDRV
std::atomic<DebugFrameCallback> g_debugFrameCallback{nullptr};
#endif

class DispatchScope
{
public:
    DispatchScope()
    {
        g_dispatchActive = true;
    }

    ~DispatchScope()
    {
        g_dispatchActive = false;
    }

    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;
};

LONG __stdcall uiFrameHook(HWND window, LPPOINT point, LONG* state)
{
    // A nested screen loop is not a second ordered-work edge: it calls stock
    // directly and leaves the next item for the next outer natural frame.
    if (g_dispatchActive)
        return g_originalUiFrame(window, point, state);

    DispatchScope dispatchScope;

    netintercept::drainOneOnUiThread();

#ifdef D2_TESTDRV
    const DebugFrameCallback callback =
        g_debugFrameCallback.load(std::memory_order_acquire);
    if (callback)
        callback(window);
#endif

    return g_originalUiFrame(window, point, state);
}

} // namespace

bool request()
{
    if (g_requested.load(std::memory_order_acquire))
        return true;

    std::lock_guard<std::mutex> lock(g_requestMutex);
    if (g_requested.load(std::memory_order_relaxed))
        return true;
    if (!executablefingerprint::isExactRussobit()) {
        spdlog::error("[uiframe] exact Russobit executable fingerprint mismatch");
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kUiFrameVA), kUiFrameExpected,
                    sizeof(kUiFrameExpected)) != 0) {
        spdlog::error("[uiframe] bytes at {:#x} do not match exact sub_5629CA", kUiFrameVA);
        return false;
    }

    g_requested.store(true, std::memory_order_release);
    spdlog::info("[uiframe] exact sub_5629CA hook requested after read-only preflight");
    return true;
}

bool requested()
{
    return g_requested.load(std::memory_order_acquire);
}

HookInfo hookInfo()
{
    return HookInfo{reinterpret_cast<void*>(kUiFrameVA),
                    reinterpret_cast<void*>(&uiFrameHook),
                    reinterpret_cast<void**>(&g_originalUiFrame)};
}

void markInstalled()
{
    if (!requested())
        return;
    g_installed.store(true, std::memory_order_release);
    spdlog::info("[uiframe] exact sub_5629CA natural-frame dispatcher installed");
}

bool installed()
{
    return g_installed.load(std::memory_order_acquire);
}

#ifdef D2_TESTDRV
bool setDebugFrameCallback(DebugFrameCallback callback)
{
    if (!callback || !requested())
        return false;

    std::lock_guard<std::mutex> lock(g_requestMutex);
    const DebugFrameCallback currentCallback =
        g_debugFrameCallback.load(std::memory_order_relaxed);
    if (currentCallback && currentCallback != callback) {
        return false;
    }
    g_debugFrameCallback.store(callback, std::memory_order_release);
    return true;
}
#endif

} // namespace uiframedispatcher
} // namespace hooks
