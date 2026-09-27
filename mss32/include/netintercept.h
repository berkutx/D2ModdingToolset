/*
 * Shared, policy-free network interception for the exact Russobit executable.
 * Production simultaneous-turn code and optional test tooling register policy
 * here instead of competing for the same RX call sites / DirectPlay Send slot.
 */

#ifndef NETINTERCEPT_H
#define NETINTERCEPT_H

#include <cstdint>

namespace game {
struct NetMessageHeader;
}

namespace hooks {
namespace netintercept {

enum class RxDecision
{
    Pass,
    Drop,
    /** Intentional successful protocol consumption without native dispatch. */
    Consume,
    Defer,
};

enum class TxDecision
{
    Pass,
    Drop,
    Redirect,
    /** Reject the send as failed. Unlike Drop/Redirect, the hook returns 0. */
    Reject,
};

/** Exact native sub_55B948 stack order is packet, sender DPID (`idFrom`),
 * local receiver DPID (`playerNetId`). Frame length is not an argument: the
 * common layer validates and reads it from NetMessageHeader::length, then
 * supplies all four named values to policy callbacks. */
using RxDispatchCallback = RxDecision (*)(void* self, void* edx, int packet,
                                           std::uint32_t frameLength,
                                           std::uint32_t idFrom,
                                           std::uint32_t playerNetId);
using TxGateCallback = TxDecision (*)(void* self, std::uint32_t idTo,
                                      const game::NetMessageHeader* message);

/** One transport-specific natural send. The common TX dispatcher calls this
 * continuation exactly once for Pass and never for Drop/Redirect/Reject.
 * `transportContext` is opaque and borrowed for the synchronous call. A zero
 * result is failure; every nonzero result is success. */
using TxSendContinuation = int (*)(void* self, void* transportContext,
                                   std::uint32_t idTo,
                                   const game::NetMessageHeader* message);

/** Small causal completion armed by an RX policy gate for its current packet.
 * It executes synchronously only after original sub_55B948 returns normally and
 * receive-dispatch depth is back at its prior value. */
using RxCompletionCallback = void (*)(std::uint32_t tag);

/** Small causal completion armed by a TX policy gate for its current packet.
 * It executes synchronously once, only after the natural transport send
 * returns normally. `sendResult` is the transport result (custom transports
 * normalize their bool result to 1/0). */
using TxCompletionCallback = void (*)(std::uint32_t tag, int sendResult);

/** Apply shared TX observation/policy around one logical IMqNetPlayer send.
 * This is also the compile-time seam used by SLikeNet client/server players,
 * above their transport-specific formatting and server fan-out. */
int dispatchTx(void* self, void* transportContext, std::uint32_t idTo,
               const game::NetMessageHeader* message,
               TxSendContinuation continuation);

/** Opaque UI task. If queued, `context` must remain valid until callback runs.
 * Callback executes at receive-dispatch depth zero and must not throw. */
using UiTaskCallback = void (*)(void* context);
/** Releases a queued task's context without touching native game objects. */
using UiTaskDiscardCallback = void (*)(void* context);

enum class NativeReceiveResult { Applied, Filtered, Failed, Unhandled };
/** Policy-free result of a normally returned original native dispatch. */
constexpr NativeReceiveResult nativeDispatchResult(int handlerCount) noexcept
{
    return handlerCount > 0 ? NativeReceiveResult::Applied
         : handlerCount == 0 ? NativeReceiveResult::Unhandled : NativeReceiveResult::Failed;
}
/** Owned, bounded evidence only; never used to decide packet admission. The
 * native result is a handler count, not a boolean handler success. Kept trivial
 * so the SEH receive wrapper needs no C++ unwinding. No payload/chat is copied. */
struct NativeReceiveDiagnostic
{
    char messageClass[37]{};
    std::uint32_t messageType{}, frameLength{}, sender{}, receiver{}, threadId{};
    RxDecision policy{RxDecision::Pass};
    int dispatchResult{};
    bool dispatched{}, captureDPlaySelf{}, replay{};

    void captureHeader(std::uint32_t type, std::uint32_t length,
                       const char (&name)[36]) noexcept
    {
        messageType = type;
        frameLength = length;
        for (auto& c : messageClass) c = 0;
        for (unsigned i = 0; i < sizeof(name); ++i) {
            const auto c = static_cast<unsigned char>(name[i]);
            if (!c) break;
            // No control bytes / injected log lines from a remote class name.
            messageClass[i] = c >= 32 && c < 127 ? static_cast<char>(c) : '?';
        }
    }
};
using NativeReceiveCallback = void (*)(void* context, NativeReceiveResult result,
                                       const NativeReceiveDiagnostic& diagnostic);

/** Attach an owned completion ticket to the exact native receive buffer.
 * Queuing/dequeuing a custom-player message is not proof of engine dispatch.
 * Success transfers context ownership: complete consumes it after RX exits,
 * or discard releases it at teardown/exception. Defer preserves the ticket
 * through the later replay; an intentional policy Consume reports Filtered.
 * Policy Drop reports Failed. Original native dispatch with no matching handler
 * reports Unhandled; its transport owner must explicitly resolve that result.
 * False retains caller ownership. Both callbacks must not throw. */
bool stageNativeReceive(const game::NetMessageHeader* buffer, void* context,
                        NativeReceiveCallback complete, UiTaskDiscardCallback discard);

/** Best-effort last-chance notification for a deferred-RX allocation/overflow
 * failure. The callback may publish a diagnostic terminal state, but the common
 * layer always fail-fasts immediately afterward: continuing after losing an
 * ordered game packet is unsafe. Runs on the RX thread and must not block or
 * throw. */
using DeferredOverflowCallback = void (*)(std::uint32_t idFrom,
                                          const std::uint8_t* payload,
                                          std::uint32_t payloadSize);

/** A single production policy gate per direction. Null clears it. */
void setRxDispatchCallback(RxDispatchCallback cb);
void setTxCallback(TxGateCallback cb);

/** Arm one completion for the RX invocation whose policy callbacks are currently
 * running. Returns false outside that narrow seam or if a completion is already
 * armed. Drop/Defer discards it; a deferred replay must arm it again. */
bool armCurrentRxCompletion(RxCompletionCallback cb, std::uint32_t tag);

/** Arm one completion for the TX invocation whose policy callbacks are
 * currently running. Returns false outside that seam or when already armed.
 * A non-Pass gate decision is a contract violation and terminates fail-closed;
 * completions are never replayed or retried. */
bool armCurrentTxCompletion(TxCompletionCallback cb, std::uint32_t tag);

/** Optional observable notification for the fatal deferred-queue overflow path. */
void setDeferredOverflowCallback(DeferredOverflowCallback cb);

/** Read-only exact-fingerprint + two RX call sites + DirectPlay Send slot
 * validation. If already installed, returns true for the owned installation. */
bool preflight();

/** Install the two RX call-site patches and DirectPlay Send vtable hook.
 * Exact-Russobit gated, idempotent, rechecks the full preflight immediately
 * before writing, and rolls back every completed write on failure. */
bool install();
bool installed();

/** Number of original sub_55B948 calls currently in flight process-wide. */
int recvDispatchDepth();

/** UI thread latched by the first exact sub_5629CA natural-frame edge, or 0
 * before that edge is observed. */
unsigned long mainThreadId();

/** Session-stable receive inner-self captured only at call-site 0x402CA7.
 * Returns null if the owner changes before the native teardown boundary. */
void* capturedRxSelf();

/** Begin capture of borrowed native receive identity for one armed map. Ordinary
 * rooms remain pass-through and do not participate in strict OH teardown. Must
 * run on the known UI thread, outside dispatch and with no old ordered work. */
bool beginSession();

/** Bracket native network/session destruction, on the known UI thread and
 * outside RX/TX dispatch or ordered task execution. Begin suspends draining,
 * discards old deferred byte snapshots and releases queued owned contexts;
 * End clears borrowed receive identity after native owners have been destroyed.
 * A queued task without a discard callback makes Begin fail without clearing it.
 * Callers must not continue native destruction after a failed Begin. */
bool beginSessionTeardown();
bool endSessionTeardown();

/** Marshal an opaque task to the game UI thread. The queue is bounded; false
 * means the callback was not accepted and the caller retains its context. */
bool invokeOnUiThread(UiTaskCallback callback, void* context,
                      UiTaskDiscardCallback discard = nullptr);

/** Always enqueue an opaque task for a later natural UI-frame drain, even when
 * the caller already is the UI thread. The queue is bounded and exact-once;
 * false means no ownership transfer and authorizes no retry. */
bool queueOnNextUiFrame(UiTaskCallback callback, void* context,
                        UiTaskDiscardCallback discard = nullptr);

/** Drain at most one deferred RX or queued UI task. Called only by the shared
 * exact sub_5629CA natural-frame hook. When both queues stay non-empty, work
 * alternates between them to prevent starvation. */
bool drainOneOnUiThread();

/** Build [0xFFFF, frameLength, payload] and call original sub_55B948 directly
 * as the authoritative DPID_SERVERPLAYER for one exact dynamic local receiver,
 * bypassing observers/gates. Caller must be on the known UI thread at receive
 * depth zero. */
bool injectAuthoritativePayloadNow(std::uint32_t playerNetId,
                                   const std::uint8_t* payload,
                                   std::uint32_t payloadSize);

} // namespace netintercept
} // namespace hooks

#endif // NETINTERCEPT_H
