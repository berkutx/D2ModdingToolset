// The runner extracts State, queue/cancel and the complete GUI pump from timerhost.cpp.
// Only game/OS observations and the native button callback are replaced by deterministic fakes.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include "timerhost-state.generated.h"

struct Button { bool enabled, top; } endButton, closeButton, backButton, continueButton;
static PhaseGameLockSnapshot nativeLock;
static int replayMyTurn, attempts, submissions, intermediatePresses, checks, failures;
static bool dragging, nestedPump, consumedBeforePress;
static bool nativeSource, inputAvailable, nativeDispatchBusy, cursorBusy, cancelClearsRequest;
static int dragCancels;
extern "C" void timerhost_pump(void);
static bool isUserPtr(void* p) { return p != nullptr; }
static bool btnEnabled(void* p) { return p && static_cast<Button*>(p)->enabled; }
static bool interfaceOnTop(void* p) { return p && static_cast<Button*>(p)->top; }
static int featuremenu_my_turn() { return replayMyTurn; }
static PhaseGameLockSnapshot phaseGameLockSnapshot() { return nativeLock; }
static void dispatchForcedAutoBattle() {}
static void tlog(const char*, ...) {}
static int featuremenu_native_dispatch_active() { return nativeDispatchBusy; }
static int cursorcapture_draw_active() { return cursorBusy; }
static bool prepareTimeoutInput(void* anchor)
{
    if (!anchor || !inputAvailable || nativeDispatchBusy || cursorBusy) return false;
    if (nativeSource) {
        nativeSource = false;
        ++dragCancels;
        ++g.dragCancelSerial;
        if (cancelClearsRequest) g.pendingEndDay = 0;
        if (nestedPump) timerhost_pump();
        return false;
    }
    return !dragging;
}
static SHORT testAsyncKeyState(int key) { return key == VK_LBUTTON && dragging ? SHORT(0x8000) : 0; }
static void pressBtn(void* button)
{
    if (button != &endButton) {
        ++intermediatePresses;
        if (button == &backButton) g.capBack = nullptr;
        return;
    }
    ++attempts;
    consumedBeforePress = consumedBeforePress && !g.pendingEndDay && g.suppressEndTurnConfirm;
    // Actual END_TURN @48FDD7 returns at 48FE05 when CheckObjectLock is true. It does not retry.
    if (replayMyTurn == 1 && nativeLock.available && !nativeLock.locked && endButton.top)
        ++submissions;
    if (nestedPump) timerhost_pump(); // native callbacks can pump nested GUI messages
}
#define GetAsyncKeyState testAsyncKeyState
#include "timerhost-pump.generated.h"
#undef GetAsyncKeyState

static void check(bool value, const char* message)
{
    ++checks;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures;
}
static void reset()
{
    g = {};
    endButton = closeButton = backButton = continueButton = {true, true};
    g.endTurn = &endButton;
    g.installed = g.confirmHookInstalled = 1;
    nativeLock = {true, false, 0, 0, 0};
    replayMyTurn = 1;
    attempts = submissions = intermediatePresses = 0;
    dragging = nestedPump = false;
    nativeSource = nativeDispatchBusy = cursorBusy = cancelClearsRequest = false;
    inputAvailable = true;
    dragCancels = 0;
    consumedBeforePress = true;
}
static void queue() { timerhost_end_day(); }
static void idle(int count = 1) { while (count--) timerhost_pump(); }

int main()
{
    reset();
    g.endTurn = nullptr; queue(); idle(4);
    check(g.pendingEndDay && attempts == 0, "garrison timeout waits while strategic button is absent");
    g.endTurn = &endButton;
    nativeLock.locked = true; nativeLock.pendingNetworkUpdates = 1; idle(4);
    check(g.pendingEndDay && attempts == 0,
          "closing garrison with busy native gate preserves timeout without a rejected press");
    nativeLock.locked = false; nativeLock.pendingNetworkUpdates = 0; idle();
    check(g.pendingEndDay && attempts == 0, "first unlocked sample alone cannot consume timeout");
    idle();
    check(submissions == 1 && !g.pendingEndDay, "released gate submits deferred garrison timeout once");
    idle(20);
    check(submissions == 1 && attempts == 1, "network acknowledgement delay does not duplicate End Day");

    reset(); // plugin has already queued the first tick of an overdrawn new day
    nativeLock.locked = true; nativeLock.pendingLocalUpdates = 2; queue(); idle(5);
    check(g.pendingEndDay && attempts == 0, "negative-bank start waits through native day initialization");
    nativeLock = {true, false, 0, 0, 0}; idle(2);
    check(submissions == 1, "negative-bank start skips as soon as the command can be accepted");

    reset(); nativeLock.available = false; queue(); idle(4);
    check(g.pendingEndDay && attempts == 0, "unavailable command state does not lose timeout");
    nativeLock.available = true; idle(2);
    check(submissions == 1, "timeout proceeds once native command state becomes available");

    reset(); queue(); idle(); nativeLock.locked = true; idle();
    nativeLock.locked = false; idle();
    check(g.pendingEndDay && attempts == 0, "busy sample resets consecutive readiness observations");
    idle(); check(submissions == 1, "two fresh ready samples permit one command");

    reset(); endButton.top = false; queue(); idle(4);
    check(g.pendingEndDay && attempts == 0, "modal covering an enabled button keeps request queued");
    endButton.top = true; nestedPump = true; idle(2);
    check(submissions == 1 && consumedBeforePress,
          "request is consumed before native callback and nested WM_TIMER cannot resend it");

    reset(); dragging = true; queue(); idle(3);
    check(g.pendingEndDay && attempts == 0, "dragging defers destructive UI transition");
    dragging = false; idle(2); check(submissions == 1, "mouse release allows queued timeout");

    reset(); nativeSource = dragging = nestedPump = true; queue(); idle();
    check(dragCancels == 1 && g.pendingEndDay && attempts == 0,
          "timeout cancels native drag while mouse is held and survives nested pump");
    idle(3);
    check(dragCancels == 1 && attempts == 0, "held mouse defers transition without repeating cancellation");
    dragging = false; idle(2);
    check(submissions == 1 && !g.pendingEndDay, "cancelled drag submits once after release");

    reset(); nativeSource = true; queue(); idle();
    check(dragCancels == 1 && attempts == 0 && g.pendingEndDay,
          "native drag left after physical release is cancelled before End Day");
    idle(2); check(submissions == 1, "fresh captures after cancellation permit End Day");

    reset(); nativeSource = nativeDispatchBusy = true; queue(); idle(3);
    check(dragCancels == 0 && attempts == 0 && g.pendingEndDay,
          "nested native input processing cannot cancel or consume timeout");
    nativeDispatchBusy = false; cursorBusy = true; idle(3);
    check(dragCancels == 0 && attempts == 0, "active cursor draw cannot be invalidated by timeout");
    cursorBusy = false; idle(3); check(submissions == 1, "timeout resumes after native callbacks finish");

    reset(); nativeSource = cancelClearsRequest = true; queue(); idle(5);
    check(dragCancels == 1 && attempts == 0 && !g.pendingEndDay,
          "cancellation callback which retires the turn cannot press a stale button");

    reset(); inputAvailable = false; queue(); idle(3);
    check(g.pendingEndDay && attempts == 0, "unknown native drag state preserves timeout safely");
    inputAvailable = true; idle(2); check(submissions == 1, "available input state resumes timeout");

    reset(); queue(); replayMyTurn = 0; idle(4);
    check(!g.pendingEndDay && attempts == 0, "ended local turn discards stale timeout");
    reset(); queue(); timerhost_cancel_elapse(); idle(4);
    check(!g.pendingEndDay && attempts == 0, "explicit cancellation prevents later skip");

    reset(); g.capBack = &backButton; nativeLock.locked = true; queue(); idle();
    check(intermediatePresses == 1 && g.pendingEndDay && attempts == 0,
          "capital Back can release its UI while final End Day still waits on native lock");
    nativeLock.locked = false; idle(2);
    check(submissions == 1, "return from capital uses the shared strategic command gate");

    reset(); g.capBack = &backButton; backButton.top = endButton.top = false; queue(); idle(4);
    check(intermediatePresses == 0 && g.pendingEndDay, "covered capital Back is never pressed");
    backButton.top = true; idle(); endButton.top = true; idle(2);
    check(intermediatePresses == 1 && submissions == 1, "uncovered capital resumes pending timeout");

    reset(); g.btnClose = &closeButton; closeButton.top = false; queue(); idle(3);
    check(intermediatePresses == 0 && !g.battleClosePressed && g.pendingEndDay,
          "covered battle Close is not consumed");
    closeButton.top = true; idle(3);
    check(intermediatePresses == 1 && g.battleClosePressed, "topmost battle Close remains one-shot");

    reset(); g.postBattleTransition = 1; endButton.top = false;
    g.briefCont = &continueButton; queue(); idle(4);
    check(g.pendingEndDay && attempts == 0 && intermediatePresses == 0,
          "post-battle reward or victory modal is never auto-continued");
    endButton.top = true; nativeLock.locked = true; idle(3);
    check(g.pendingEndDay && attempts == 0, "post-battle command still waits on native lock");
    nativeLock.locked = false; idle(2); check(submissions == 1, "post-battle command remains supported");

    reset(); replayMyTurn = 0; g.postBattleTransition = 1; queue(); replayMyTurn = 1; idle(4);
    check(!g.pendingEndDay && attempts == 0, "defender timeout cannot consume attacker strategic turn");
    std::printf("Timer End Day replay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
