// The runner extracts the production State, result enum and complete Auto dispatcher.
// Native UI/input preparation is replaced by fakes that can cancel, re-enter or replace a battle.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include "timerhost-auto-state.generated.h"

static SRWLOCK g_battleStateLock = SRWLOCK_INIT;
static char firstViewer, secondViewer;
static int checks, failures, prepareCalls, nativeCalls, latchCalls, cancelCalls;
static int nativeResult, nestedResult, nativeDepth, cursorDepth;
static bool reenterNative, guardsHeld;
static void* invokedViewer;
enum PrepareBehavior {
    ready, cancelAndDefer, replaceInstance, replaceViewer, advanceGeneration,
    clearRequest, closeSelection, reenterAndDefer
};
static PrepareBehavior prepareBehavior;
int dispatchForcedAutoBattle();
static bool isUserPtr(void* pointer) { return pointer != nullptr; }
static int featuremenu_native_dispatch_active() { return nativeDepth; }
static int cursorcapture_draw_active() { return cursorDepth; }
static void* battleInterf(void* viewer) { return viewer; }
static void tlog(const char*, ...) {}
static bool prepareTimeoutInput(void*)
{
    ++prepareCalls;
    guardsHeld = guardsHeld && g.inAction == 1 && g.forceAutoDispatching == 1;
    switch (prepareBehavior) {
    case cancelAndDefer:
        ++cancelCalls;
        ++g.dragCancelSerial;
        prepareBehavior = ready;
        return false;
    case replaceInstance:
        ++g.battleInstance;
        break;
    case replaceViewer:
        g.battleViewer = &secondViewer;
        break;
    case advanceGeneration:
        g.battleStateSeq += 2;
        break;
    case clearRequest:
        g.forceAutoRequested = 0;
        break;
    case closeSelection:
        g.battleSelectionOpen = 0;
        break;
    case reenterAndDefer:
        g.forceAutoMessagePosted = 1;
        nestedResult = dispatchForcedAutoBattle();
        guardsHeld = guardsHeld && g.inAction == 1 && g.forceAutoDispatching == 1;
        prepareBehavior = ready;
        return false;
    default:
        break;
    }
    return true;
}
static int invokeNativeAutoBattle(void* viewer, int* side)
{
    ++nativeCalls;
    invokedViewer = viewer;
    guardsHeld = guardsHeld && g.inAction == 1 && g.forceAutoDispatching == 1;
    *side = 0x100;
    if (reenterNative) {
        g.forceAutoMessagePosted = 1;
        nestedResult = dispatchForcedAutoBattle();
        guardsHeld = guardsHeld && g.inAction == 1 && g.forceAutoDispatching == 1;
    }
    return nativeResult;
}
static int nativeAutoBattleEnabledForSide(void*, int) { return 1; }
static bool latchForcedAutoBattle(void* viewer, LONG instance, int side)
{
    ++latchCalls;
    if (viewer != g.battleViewer || instance != g.battleInstance)
        return false;
    g.forceAutoLatched = 1;
    g.forceAutoLatchedViewer = viewer;
    g.forceAutoLatchedInstance = instance;
    g.forceAutoLatchedSideOffset = side;
    return true;
}
static int autoBattleSideOffset(void*) { return 0x100; }
static bool enforceForcedAutoBattlePresentation(void*, int) { return true; }

#include "timerhost-auto-dispatch.generated.h"

static void check(bool condition, const char* message)
{
    ++checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}
static void reset()
{
    g = {};
    g.battleViewer = &firstViewer;
    g.battleInstance = 7;
    g.battleStateSeq = 10;
    g.battleKindPublished = g.battleTurnActive = g.battleSelectionOpen = 1;
    g.battlePlaybackLocal = -1;
    g.forceAutoRequested = g.forceAutoUiAvailable = g.forceAutoMessagePosted = 1;
    g.forceAutoLastInstance = g.forceAutoLastGeneration = -1;
    prepareCalls = nativeCalls = latchCalls = cancelCalls = 0;
    nativeDepth = cursorDepth = 0;
    nestedResult = -1;
    invokedViewer = nullptr;
    guardsHeld = true;
    reenterNative = false;
    prepareBehavior = ready;
    nativeResult = kNativeAutoApplied;
}
static bool generationUnconsumed()
{
    return g.forceAutoLastInstance == -1 && g.forceAutoLastGeneration == -1;
}
static bool guardsReleased() { return !g.inAction && !g.forceAutoDispatching; }

int main()
{
    reset(); prepareBehavior = cancelAndDefer;
    check(dispatchForcedAutoBattle() == 0 && cancelCalls == 1 && nativeCalls == 0 &&
          g.forceAutoRequested && generationUnconsumed() && !g.forceAutoMessagePosted,
          "drag cancellation preserves timeout and generation for a later dispatch");
    check(guardsReleased(), "deferred cancellation releases both dispatch guards");
    check(dispatchForcedAutoBattle() == 1 && nativeCalls == 1 && latchCalls == 1 &&
          !g.forceAutoRequested && g.forceAutoLastInstance == 7 && g.forceAutoLastGeneration == 10,
          "next ready dispatch applies the same timeout once");
    dispatchForcedAutoBattle(); dispatchForcedAutoBattle();
    check(nativeCalls == 1 && cancelCalls == 1 && guardsHeld && guardsReleased(),
          "completed Auto is neither repeated nor cancelled again");

    reset(); nativeDepth = 1;
    check(dispatchForcedAutoBattle() == 0 && !prepareCalls && !nativeCalls &&
          !g.forceAutoMessagePosted && g.forceAutoRequested && generationUnconsumed(),
          "native window callback defers private Auto message without losing retry");
    nativeDepth = 0;
    check(dispatchForcedAutoBattle() == 1 && nativeCalls == 1,
          "Auto resumes after native window callback unwinds");

    reset(); cursorDepth = 1;
    check(dispatchForcedAutoBattle() == 0 && !prepareCalls && !nativeCalls &&
          !g.forceAutoMessagePosted && g.forceAutoRequested && generationUnconsumed(),
          "cursor callback defers Auto without consuming its generation");
    cursorDepth = 0;
    check(dispatchForcedAutoBattle() == 1 && nativeCalls == 1,
          "Auto resumes after cursor callback unwinds");

    reset(); g.inAction = 1;
    check(dispatchForcedAutoBattle() == 0 && !prepareCalls && !nativeCalls &&
          g.inAction == 1 && !g.forceAutoDispatching && !g.forceAutoMessagePosted &&
          g.forceAutoRequested && generationUnconsumed(),
          "End Day reentry preserves the outer inAction owner and queued Auto");
    g.inAction = 0;
    check(dispatchForcedAutoBattle() == 1, "Auto retries after End Day callback returns");

    reset(); g.forceAutoDispatching = 1;
    check(dispatchForcedAutoBattle() == 0 && !prepareCalls && !nativeCalls &&
          !g.inAction && g.forceAutoDispatching == 1 && g.forceAutoRequested,
          "occupied Auto guard releases only the newly acquired action guard");

    reset(); prepareBehavior = replaceInstance;
    check(dispatchForcedAutoBattle() == 0 && !nativeCalls && !latchCalls &&
          g.forceAutoRequested && generationUnconsumed(),
          "preparation that replaces battle instance cannot invoke stale Auto");
    prepareBehavior = ready;
    check(dispatchForcedAutoBattle() == 1 && g.forceAutoLatchedInstance == 8,
          "replacement battle requires and receives a fresh dispatch snapshot");

    reset(); prepareBehavior = replaceViewer;
    check(dispatchForcedAutoBattle() == 0 && !nativeCalls && !latchCalls && generationUnconsumed(),
          "preparation that replaces viewer cannot call the previously captured viewer");
    prepareBehavior = ready;
    check(dispatchForcedAutoBattle() == 1 && invokedViewer == &secondViewer,
          "retry targets only the newly captured live viewer");

    reset(); prepareBehavior = advanceGeneration;
    check(dispatchForcedAutoBattle() == 0 && !nativeCalls && generationUnconsumed(),
          "selection generation change during preparation defers Auto");
    prepareBehavior = ready;
    check(dispatchForcedAutoBattle() == 1 && g.forceAutoLastGeneration == 12,
          "fresh selection generation can consume the retained request");

    reset(); prepareBehavior = clearRequest;
    check(dispatchForcedAutoBattle() == 0 && !nativeCalls && !latchCalls &&
          !g.forceAutoRequested && generationUnconsumed(),
          "request cleared by cancellation callback cannot invoke Auto afterwards");

    reset(); prepareBehavior = closeSelection;
    check(dispatchForcedAutoBattle() == 0 && !nativeCalls && g.forceAutoRequested &&
          generationUnconsumed(),
          "selection closed by preparation is checked again before native invocation");

    reset(); prepareBehavior = reenterAndDefer;
    check(dispatchForcedAutoBattle() == 0 && nestedResult == 0 && prepareCalls == 1 &&
          !nativeCalls && g.forceAutoRequested && generationUnconsumed() &&
          guardsHeld && guardsReleased(),
          "nested dispatcher from cancellation cannot prepare or apply Auto recursively");
    check(dispatchForcedAutoBattle() == 1 && nativeCalls == 1,
          "outer cancellation leaves exactly one successful retry");

    reset(); reenterNative = true;
    check(dispatchForcedAutoBattle() == 1 && nativeCalls == 1 && nestedResult == 0 &&
          prepareCalls == 1 && latchCalls == 1 && guardsHeld && guardsReleased(),
          "nested dispatcher from native Auto callback cannot submit a second action");

    reset(); nativeResult = kNativeAutoDeferred;
    check(dispatchForcedAutoBattle() == 0 && nativeCalls == 1 && !latchCalls &&
          g.forceAutoRequested && generationUnconsumed(),
          "temporarily covered native toggle keeps its generation retryable");
    nativeResult = kNativeAutoApplied;
    check(dispatchForcedAutoBattle() == 1 && nativeCalls == 2 && latchCalls == 1,
          "covered toggle later applies once when the native UI becomes ready");

    reset(); nativeResult = kNativeAutoRejected;
    check(dispatchForcedAutoBattle() == 0 && nativeCalls == 1 && !latchCalls &&
          !g.forceAutoRequested && g.forceAutoLastGeneration == 10 && guardsReleased(),
          "actual native rejection consumes generation and restores manual control");

    reset(); g.forceAutoUiAvailable = 0;
    check(dispatchForcedAutoBattle() == 0 && !prepareCalls && !nativeCalls &&
          !g.forceAutoMessagePosted && g.forceAutoRequested && guardsReleased(),
          "unavailable native Auto UI cannot enter cancellation or native callbacks");

    std::printf("Timer Auto drag dispatch replay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
