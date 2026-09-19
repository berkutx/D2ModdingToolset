// Production timeout cancellation and pointer probes on synthetic x86 native-layout objects.
// Only native RTTI, virtual callbacks and physical button observations are fakes. The executable
// does not load a game, install hooks, synthesize input or modify a running process.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

static_assert(sizeof(void*) == 4, "native drag ABI replay requires x86");
static int checks, failures;
static void check(bool value, const char* message)
{
    ++checks;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures;
}

using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProcFn g_origWndProc = nullptr;
volatile LONG g_nativeWndProcDepth = 0;
volatile LONG g_cursorDrawDepth = 0;
struct Renderer {};
void cursorDrawBody(void*, Renderer*);
#include "native-ui-depth.generated.h"

struct State { volatile LONG dragCancelAvailable, dragCancelSerial; } g;
struct FakeNode { FakeNode* next; FakeNode* prev; void* value; };
struct FakeList { unsigned count; FakeNode* head; unsigned unknown; void* allocator; };
struct FakeInterfaceData { void* counter; void* interfaceManager; };
struct FakeDragData {
    unsigned flag;
    void* dragSource;
    unsigned unknown;
    void* counter;
    void* interfaceManager;
    FakeList sources;
    FakeList targets;
};
struct FakeInterface {
    void** vtable;
    FakeInterfaceData* data;
    unsigned padding[2];
    void** dropVtable;
    FakeDragData* dragData;
};
struct FakeManager { void** vtable; };
struct FakeSource { void** vtable; };
static_assert(offsetof(FakeInterface, dropVtable) == 16, "drop manager offset");
static_assert(offsetof(FakeInterface, dragData) == 20, "drag data offset");
static_assert(offsetof(FakeDragData, sources) == 20, "registered source list offset");
static_assert(offsetof(FakeDragData, targets) == 36, "registered target list offset");
static_assert(offsetof(FakeNode, value) == 8, "native list node payload offset");

static FakeInterface inputAnchor, dragOwner, plainTop;
static FakeInterfaceData interfaceData;
static FakeDragData dragData;
static FakeManager interfaceManager;
static FakeSource dragSource, otherSource;
static FakeNode sentinel, registered;
static void* managerVtable[23];
static void* dropVtable[14];
static void* dragSourceVtable[10];
static void* topInterface;
static void* rememberedInterface;
static uintptr_t rejectedSignature;
static int signatureReads, resetCalls, cleanupCalls, castCalls, getCalls;
static int nestedInputAttempts;
static bool leftDown, alternateGetter, nullGetter, mutateOnReset, nestedGuardOnReset;
static bool nestedDirectReset, throwOnReset;
static int nestedCancelResult;
static bool serialPublished, nestedInputAllowed;

static bool validateBytes(uintptr_t address, const unsigned char*, size_t)
{
    ++signatureReads;
    return address != rejectedSignature;
}
static void tlog(const char*, ...) {}
static SHORT testAsyncKeyState(int key)
{
    return key == VK_LBUTTON && leftDown ? SHORT(-32768) : 0;
}
static void* testDynamicCast(void* interf)
{
    ++castCalls;
    return interf == &dragOwner ? &dragOwner : nullptr;
}
#define C4_TIMER_DRAG_DYNAMIC_CAST(interf) testDynamicCast(interf)
#define GetAsyncKeyState testAsyncKeyState
#include "../features/timerdrag.h"
#undef GetAsyncKeyState
#undef C4_TIMER_DRAG_DYNAMIC_CAST

static void* __fastcall getTop(void*, void*) { return topInterface; }
static void* __fastcall getRemembered(void*, void*) { return rememberedInterface; }
static void* __fastcall getSource(void*, void*)
{
    ++getCalls;
    return nullGetter ? nullptr : alternateGetter ? &otherSource : dragData.dragSource;
}
static int __fastcall cleanupSource(void*, void*)
{
    ++cleanupCalls;
    return 0;
}
static void __fastcall resetSource(void*, void*)
{
    ++resetCalls;
    serialPublished = serialPublished && g.dragCancelSerial == resetCalls;
    if (nestedDirectReset)
        nestedCancelResult = cancelTimeoutDrag(&inputAnchor);
    if (throwOnReset)
        RaiseException(0xE0424242, 0, 0, nullptr);
    if (nestedGuardOnReset) {
        // A native reset callback can enter a nested window dispatch before clearing its dragSource.
        InterlockedIncrement(&g_nativeWndProcDepth);
        ++nestedInputAttempts;
        nestedInputAllowed = prepareTimeoutInput(&inputAnchor);
        InterlockedDecrement(&g_nativeWndProcDepth);
    }
    void* oldSource = dragData.dragSource;
    dragData.flag = 0;
    dragData.dragSource = nullptr;
    if (oldSource) cleanupSource(oldSource, nullptr);
    if (mutateOnReset) {
        // The callback can replace the entire UI. The cancellation helper must not reread its
        // captured dragOwner/dragSource afterwards; the next timer tick will reacquire the new window.
        dragOwner.dragData = reinterpret_cast<FakeDragData*>(1);
        dragSource.vtable = nullptr;
        topInterface = &plainTop;
    }
}
static void reset()
{
    g = {};
    g.dragCancelAvailable = 1;
    g_nativeWndProcDepth = g_cursorDrawDepth = 0;
    inputAnchor = dragOwner = plainTop = {};
    dragData = {};
    dragSource = otherSource = {};
    for (void*& entry : managerVtable) entry = nullptr;
    for (void*& entry : dropVtable) entry = nullptr;
    for (void*& entry : dragSourceVtable) entry = nullptr;
    interfaceManager.vtable = managerVtable;
    managerVtable[2] = reinterpret_cast<void*>(getTop);
    managerVtable[11] = reinterpret_cast<void*>(getRemembered);
    dropVtable[1] = reinterpret_cast<void*>(getSource);
    dropVtable[4] = reinterpret_cast<void*>(resetSource);
    dragSourceVtable[7] = reinterpret_cast<void*>(cleanupSource);
    dragSource.vtable = otherSource.vtable = dragSourceVtable;
    interfaceData = {nullptr, &interfaceManager};
    inputAnchor.data = dragOwner.data = plainTop.data = &interfaceData;
    dragOwner.dropVtable = dropVtable;
    dragOwner.dragData = &dragData;
    dragData.interfaceManager = &interfaceManager;
    sentinel = {&registered, &registered, nullptr};
    registered = {&sentinel, &sentinel, &dragSource};
    dragData.sources = {1, &sentinel, 0, nullptr};
    dragData.dragSource = &dragSource;
    dragData.flag = 1;
    topInterface = &dragOwner;
    rememberedInterface = nullptr;
    rejectedSignature = 0;
    signatureReads = resetCalls = cleanupCalls = castCalls = getCalls = nestedInputAttempts = 0;
    leftDown = alternateGetter = nullGetter = mutateOnReset = nestedGuardOnReset = false;
    nestedDirectReset = throwOnReset = false;
    nestedCancelResult = TimeoutDragIdle;
    nestedInputAllowed = false;
    serialPublished = true;
}

static LRESULT CALLBACK nativeWnd(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    check(featuremenu_native_dispatch_active() == 1, "native window callback is marked active");
    if (message == 1) {
        check(callNativeGameWndProc(hwnd, 2, wParam, lParam) == 23, "nested WndProc return preserved");
        check(g_nativeWndProcDepth == 1, "outer WndProc remains active after nested return");
    }
    if (message == 2) check(g_nativeWndProcDepth == 2, "nested WndProc depth retained");
    if (message == 3) RaiseException(0xE0424242, 0, 0, nullptr);
    return 23;
}
void cursorDrawBody(void* state, Renderer* renderer)
{
    check(cursorcapture_draw_active() == 1, "native cursor callback is marked active");
    const ULONG_PTR mode = reinterpret_cast<ULONG_PTR>(state);
    if (mode == 1) {
        cursorDrawThunk(reinterpret_cast<void*>(2), nullptr, renderer);
        check(g_cursorDrawDepth == 1, "outer cursor draw remains active after nested return");
    }
    if (mode == 2) check(g_cursorDrawDepth == 2, "nested cursor draw depth retained");
    if (mode == 3) RaiseException(0xE0424242, 0, 0, nullptr);
}
static void testGuardExceptions()
{
    bool caught = false;
    __try { callNativeGameWndProc(nullptr, 3, 0, 0); }
    __except (GetExceptionCode() == 0xE0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        caught = true;
    }
    check(caught && !featuremenu_native_dispatch_active(),
          "WndProc exception propagates while finally restores depth");
    caught = false;
    __try { cursorDrawThunk(reinterpret_cast<void*>(3), nullptr, nullptr); }
    __except (GetExceptionCode() == 0xE0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        caught = true;
    }
    check(caught && !cursorcapture_draw_active(),
          "cursor exception propagates while finally restores depth");
}

int main()
{
    reset();
    check(validateNativeDragLayout() && signatureReads == 7, "all native code and RTTI signatures are required");
    const uintptr_t signatureAddresses[] = {0x66D466, 0x5A9EA2, 0x56CF5C, 0x53D357, 0x53D6A3, 0x78E8D8, 0x79B290};
    for (uintptr_t address : signatureAddresses) {
        rejectedSignature = address;
        check(!validateNativeDragLayout(), "a mismatched native signature disables cancellation");
    }

    reset(); leftDown = true;
    check(!prepareTimeoutInput(&inputAnchor) && resetCalls == 1 && cleanupCalls == 1 &&
          !dragData.dragSource && g.dragCancelSerial == 1 && serialPublished,
          "held drag is cancelled once before any timeout action and invalidation is published first");
    check(!prepareTimeoutInput(&inputAnchor) && resetCalls == 1,
          "holding the cancelled gesture defers the action without cancelling again");
    leftDown = false;
    check(prepareTimeoutInput(&inputAnchor) && resetCalls == 1,
          "releasing the cancelled gesture allows the queued action on a later check");

    reset();
    check(!prepareTimeoutInput(&inputAnchor) && resetCalls == 1,
          "released physical button with active native source still requires cancellation");
    check(prepareTimeoutInput(&inputAnchor), "cancelled source is reacquired as idle on the next check");
    reset(); dragData.dragSource = nullptr;
    check(prepareTimeoutInput(&inputAnchor) && resetCalls == 0, "idle drag manager needs no cancellation");
    leftDown = true;
    check(!prepareTimeoutInput(&inputAnchor), "physical held press still delays transition without a native source");

    reset(); topInterface = &plainTop;
    check(prepareTimeoutInput(&inputAnchor) && resetCalls == 0, "non-drag top interface remains eligible");
    rememberedInterface = &dragOwner;
    check(!prepareTimeoutInput(&inputAnchor) && resetCalls == 1,
          "remembered drag interface is cancelled even when a different window is topmost");
    reset(); rememberedInterface = &dragOwner;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragCancelled && resetCalls == 1,
          "same remembered and top interface is cancelled only once");
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragIdle && getCalls == 2,
          "same idle remembered and top interface is queried only once per call");

    reset(); registered.value = &otherSource;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "unregistered current source is never dereferenced for cleanup");
    reset(); alternateGetter = true;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "getter and raw source disagreement blocks cancellation");
    reset(); nullGetter = true;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "null getter cannot hide a nonnull native source");
    reset(); dragData.sources.count = 2;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "list length exceeding nodes blocks cancellation");
    reset(); registered.next = &registered;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "list not closing at its sentinel blocks cancellation");
    reset(); dragData.sources.count = 4097;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "unbounded malformed list is rejected");
    reset(); sentinel.next = reinterpret_cast<FakeNode*>(1);
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "invalid list node cannot reach a native reset callback");

    reset(); dragSourceVtable[7] = &dragSource;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "nonexecutable source cleanup pointer blocks native reset");
    reset(); dropVtable[4] = &dragSource;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 0,
          "nonexecutable reset pointer is never invoked");
    reset(); g.dragCancelAvailable = 0;
    check(!prepareTimeoutInput(&inputAnchor) && castCalls == 0 && resetCalls == 0,
          "unsupported native layout defers without probing game objects");
    reset(); dragOwner.data = nullptr;
    check(!prepareTimeoutInput(&inputAnchor) && resetCalls == 0, "missing native interface manager blocks cancellation");

    reset(); g_nativeWndProcDepth = 1;
    check(!prepareTimeoutInput(&inputAnchor) && getCalls == 0 && resetCalls == 0,
          "nested native input dispatch blocks cancellation before object access");
    g_nativeWndProcDepth = 0; g_cursorDrawDepth = 1;
    check(!prepareTimeoutInput(&inputAnchor) && getCalls == 0 && resetCalls == 0,
          "active native cursor draw blocks cancellation before object access");
    reset(); nestedGuardOnReset = true;
    check(!prepareTimeoutInput(&inputAnchor) && resetCalls == 1 && nestedInputAttempts == 1 && !nestedInputAllowed,
          "nested native dispatch during reset cannot recursively cancel the live source");
    reset(); nestedDirectReset = true;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragCancelled && resetCalls == 1 &&
          nestedCancelResult == TimeoutDragUnavailable && g.dragCancelSerial == 1,
          "direct reset callback reentry is blocked without duplicating native cancellation");
    reset(); throwOnReset = true;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragUnavailable && resetCalls == 1,
          "native cancellation exception defers the destructive timeout action");
    throwOnReset = false;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragCancelled && resetCalls == 2,
          "cancellation busy guard is released after an exception");
    reset(); mutateOnReset = true;
    check(cancelTimeoutDrag(&inputAnchor) == TimeoutDragCancelled && resetCalls == 1,
          "reset may invalidate captured owner and source without a subsequent stale read");
    check(prepareTimeoutInput(&inputAnchor), "next check resolves the replacement interface afresh");

    reset(); g_origWndProc = nativeWnd;
    check(callNativeGameWndProc(nullptr, 1, 0, 0) == 23 && !featuremenu_native_dispatch_active(),
          "WndProc guard preserves normal return and clears active depth");
    cursorDrawThunk(reinterpret_cast<void*>(1), nullptr, nullptr);
    check(!cursorcapture_draw_active(), "cursor guard clears active depth after normal draw");
    testGuardExceptions();

    std::printf("Timer drag replay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
