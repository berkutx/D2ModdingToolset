/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Window-caption role tag — see testdrv/windowtag.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#include "testdrv/windowtag.h"
#include <cstring>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace windowtag {

namespace {

struct FindCtx
{
    DWORD pid;
    HWND found;
};

BOOL CALLBACK findHwnd(HWND h, LPARAM lp)
{
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid == c->pid) {
        char t[64]{};
        GetWindowTextA(h, t, sizeof(t));
        if (t[0] && IsWindowVisible(h)) {
            c->found = h;
            return FALSE;
        }
    }
    return TRUE;
}

const char* roleTag()
{
    char role[16]{};
    GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
    if (lstrcmpiA(role, "host") == 0)
        return "HOST";
    if (lstrcmpiA(role, "join") == 0 || lstrcmpiA(role, "joiner") == 0)
        return "CLIENT";
    return nullptr;
}

// Re-applies "<base>  [ROLE]" every ~2s — the engine rewrites its own caption, so
// a one-shot SetWindowText would be lost. Snapshots the base title once (skipping
// an already-tagged title so a relaunch doesn't double-tag).
DWORD WINAPI tagThread(LPVOID)
{
    const char* tag = roleTag();
    if (!tag)
        return 0;
    char base[256]{};
    bool haveBase = false;
    for (;;) {
        FindCtx ctx{GetCurrentProcessId(), nullptr};
        EnumWindows(&findHwnd, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.found) {
            if (!haveBase) {
                char cur[256]{};
                int n = GetWindowTextA(ctx.found, cur, sizeof(cur) - 1);
                if (n > 0 && !strstr(cur, "[HOST]") && !strstr(cur, "[CLIENT]")) {
                    lstrcpynA(base, cur, sizeof(base));
                    haveBase = true;
                }
            }
            if (haveBase) {
                char neu[300];
                wsprintfA(neu, "%s  [%s]", base, tag);
                SetWindowTextA(ctx.found, neu);
            }
        }
        Sleep(2000);
    }
}

} // namespace

void start()
{
    if (!roleTag())
        return;
    HANDLE th = CreateThread(nullptr, 0, &tagThread, nullptr, 0, nullptr);
    if (th)
        CloseHandle(th);
    spdlog::info("[testdrv] window-tag thread started");
}

} // namespace windowtag
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
