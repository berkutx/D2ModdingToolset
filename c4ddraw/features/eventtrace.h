#pragma once
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Observers only. No message is generated, removed, retried or modified by these helpers. */
void eventtrace_install(void);
void eventtrace_message(unsigned stage, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void eventtrace_pulled(unsigned stage, const MSG* msg, int result, unsigned remove);
void eventtrace_clock(DWORD realTick, DWORD virtualTick, DWORD factor, uintptr_t caller);
unsigned long long eventtrace_wait_begin(void);
void eventtrace_wait_end(unsigned long long start, unsigned tickLength);
void eventtrace_frame(unsigned stage, uintptr_t surface);
void eventtrace_surface(unsigned stage, uintptr_t surface, unsigned caps, unsigned flags,
                        DWORD lastFlip, DWORD lastBlt);
void eventtrace_mark_input(void);
#ifdef __cplusplus
}
#endif

/* CSV event IDs; a/b/c/d interpretation is documented in NETWORK_TRACE.md. */
enum {
    C4TRACE_HOOK = 1, C4TRACE_REGISTER = 2,
    C4TRACE_POST_ENTER = 3, C4TRACE_POST_RETURN = 4,
    C4TRACE_GET_RAW = 10, C4TRACE_GET_MAPPED = 11,
    C4TRACE_PEEK_RAW = 12, C4TRACE_PEEK_MAPPED = 13,
    C4TRACE_PULLED_PAYLOAD = 14,
    C4TRACE_RENDER_WND = 20, C4TRACE_FEATURE_WND = 21,
    C4TRACE_NATIVE_WND_ENTER = 22, C4TRACE_NATIVE_WND_RETURN = 23,
    C4TRACE_CLOCK = 30, C4TRACE_WAIT = 31, C4TRACE_WAIT_SUMMARY = 32,
    C4TRACE_MANAGER = 40,
    C4TRACE_SURFACE = 50, C4TRACE_UPLOAD = 51,
    C4TRACE_SWAP_ENTER = 52, C4TRACE_SWAP_RETURN = 53, C4TRACE_RENDER_START = 54,
    C4TRACE_SURFACE_WAKE = 55,
    C4TRACE_BATTLE_SUBMIT = 60, C4TRACE_BATTLE_SUBMIT_RETURN = 61,
    C4TRACE_BATTLE_SELECT = 62, C4TRACE_BATTLE_RESULT = 63,
    C4TRACE_BATTLE_OPEN = 64, C4TRACE_AUTO_BLOCK = 65,
    C4TRACE_BATTLE_IDS = 66,
    C4TRACE_FASTAI_DISPATCH_ENTER = 70, C4TRACE_FASTAI_DISPATCH_RETURN = 71
};
