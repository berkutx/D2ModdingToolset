#pragma once

#include <stdint.h>
#include "snapshotprofile.h"

// Value-only lifecycle token. Read on the game UI thread before/after native capture.
// This is a conservative invalidation signal, not a full revision of arbitrary Lua state.
struct SlicedBattleState
{
    uint32_t instance;
    uint32_t generation;
    int32_t kind;
    int32_t localActive;
    int32_t selectionOpen;
    int32_t continuation;
    int32_t animationActive;
    int32_t playbackLocal;
};

inline bool slicedStateEligible(const SlicedBattleState& state)
{
    if (state.kind <= 0)
        return false;
    if (state.animationActive == 0)
        return true;
    // The legacy UI-ready byte can still be zero at the first manual choice. The host opens
    // selection only AFTER the accepted ChooseAction handler returns, and clears it before
    // Submit/Result. That exact local decision (with no Result playback) is safe to inspect.
    // Unknown animation state is still rejected; this is not a timeout or an animation bypass.
    return state.animationActive == 1 && state.localActive == 1 &&
           state.selectionOpen == 1 && state.playbackLocal == -1;
}

typedef bool (__cdecl* ReadSlicedBattleState)(SlicedBattleState*);

enum SlicedCaptureResult
{
    SliceUnavailable = 0, // known change/error/not eligible; invalidate the published frame
    SliceComplete = 1,    // complete newly captured JSON, never a partial roster
    SlicePending = 2,     // one working batch, no new publication
    SliceCached = 3       // probe matches, bounded cached frame remains; do not refresh its timestamp
};

struct SlicedCaptureInfo
{
    int capturedUnit; // -1 means no native capture in this call; otherwise zero-based index
    int totalUnits;
    uint32_t oldestAgeMs; // age of first card of a completed batch; preserve this in frame timestamp
    uint32_t stepCount;
    NativeSnapshotProfile profile;
};

extern "C" int battleunitinfo_step_json(
    int width, int height, char* json, uint32_t capacity, uint32_t* required,
    ReadSlicedBattleState readState, SlicedCaptureInfo* info);
extern "C" void battleunitinfo_cancel_sliced(void);
extern "C" bool battleunitinfo_preview_hit(int x, int y);
