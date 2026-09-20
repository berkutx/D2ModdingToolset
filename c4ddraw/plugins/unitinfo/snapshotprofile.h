#pragma once

#include <windows.h>

// Diagnostic PODs only: no owned game objects, allocations, or destruction inside native SEH.
struct UnitSnapshotTiming
{
    LONGLONG prep;
    LONGLONG constructor;
    LONGLONG controls;
    LONGLONG destructor;
    LONGLONG text;
};

struct NativeSnapshotProfile
{
    int units;
    LONGLONG preflight;
    LONGLONG json;
    UnitSnapshotTiming unit[12];
};

inline LONGLONG snapshotProfileCounter()
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

inline void snapshotProfileFinish(LONGLONG* elapsed, LONGLONG* started)
{
    if (elapsed) {
        const LONGLONG now = snapshotProfileCounter();
        *elapsed += now - *started;
        *started = now;
    }
}
