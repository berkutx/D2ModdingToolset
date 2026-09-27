/*
 * Optional network instrumentation, implemented in testdrv/nettracehooks.cpp.
 * Existing netintercept API names are retained for harness compatibility.
 * Production consumers include only netintercept.h.
 */
#ifndef TESTDRV_NETWORKOBSERVERS_H
#define TESTDRV_NETWORKOBSERVERS_H

#ifdef D2_TESTDRV
#include "netintercept.h"

namespace hooks::netintercept {

/** DebugTest RX observer receives the serialized message body after the first eight
 * bytes of NetMessageHeader. `payloadSize` is header.length - 8. It runs on
 * the natural game/worker RX thread. */
using RxTraceCallback = void (*)(void* self, std::uint32_t idFrom,
                                 const std::uint8_t* payload,
                                 std::uint32_t payloadSize);

/** DebugTest TX observer preserves the legacy tracing contract: `message` points at
 * the outer NetMessageHeader and `size` is message->length. */
using TxTraceCallback = void (*)(void* self, std::uint32_t idTo, const std::uint8_t* message,
                                 std::uint32_t size);

/** DebugTest-only observer for a natural RX frame that the exact Russobit
 * dispatcher applied (`dispatchResult > 0`). It runs synchronously after the
 * production causal completion. The payload is borrowed for this call only. */
using RxPostDispatchObserver = void (*)(void* self, std::uint32_t idFrom,
                                        std::uint32_t playerNetId,
                                        const std::uint8_t* payload,
                                        std::uint32_t payloadSize,
                                        int dispatchResult);

/** DebugTest-only observer for a natural TX frame after the selected transport
 * send returns. It never runs for synthetic Drop/Redirect/Reject results. The
 * message is borrowed for this call only. */
using TxPostSendObserver = void (*)(void* self, std::uint32_t idTo,
                                    const std::uint8_t* message,
                                    std::uint32_t size, int sendResult);

/**
 * DebugTest-only equivalent of the legacy host PacketIn route. The complete
 * borrowed native frame produced by CMidgard is synchronously submitted to the
 * live host CMidServer receive dispatcher for the exact dynamic sender DPID.
 * Production and secondary RX gates still observe the frame. Caller must be
 * the known UI thread at receive depth zero. Returns the native dispatcher
 * result (positive means a message handler was found), never a gameplay-state
 * assertion.
 */
int dispatchLocalServerFrameNow(std::uint32_t senderDpid,
                                const game::NetMessageHeader* message);

/** One DebugTest all-or-none observer registration. Null fields are ignored. The
 * process-lifetime registry validates capacity for every supplied callback
 * before publishing any of them. */
struct ObserverBundle
{
    RxTraceCallback rx = nullptr;
    TxTraceCallback tx = nullptr;
    RxPostDispatchObserver rxPostDispatch = nullptr;
    TxPostSendObserver txPostSend = nullptr;
};

/** DebugTest process-lifetime registrations. Return false only when the bounded observer
 * table is full (or cb is null). Duplicate callbacks are treated as success. */
bool addRxObserver(RxTraceCallback cb);
bool addTxObserver(TxTraceCallback cb);

/** Read-only capacity/duplicate preflight and all-or-none registration for a
 * related observer set. addObservers repeats the complete validation while
 * holding the same registry mutex, so a stale preflight cannot cause a partial
 * registration. */
bool canAddObservers(const ObserverBundle& bundle);
bool addObservers(const ObserverBundle& bundle);

/** Independent process-lifetime DebugTest registrations. They do not consume
 * or collide with the single production armCurrent* completion slots. */
bool addRxPostDispatchObserver(RxPostDispatchObserver cb);
bool addTxPostSendObserver(TxPostSendObserver cb);

/** Independent compatibility-policy slots. DebugTest's historical adapter uses
 * these, so enabling test instrumentation cannot replace production policy. */
void setSecondaryRxDispatchCallback(RxDispatchCallback cb);
void setSecondaryTxCallback(TxGateCallback cb);

/** Non-overwriting ownership for the single DebugTest TX policy slot. The
 * preflight is advisory; claim repeats the atomic null-or-same check. */
bool canClaimSecondaryTxCallback(TxGateCallback cb);
bool claimSecondaryTxCallback(TxGateCallback cb);

/** One fail-closed observation of every ordered-work source that can race a
 * DebugTest strategic action. `receiveHookDepth` spans the complete RX hook,
 * including policy, deferred-frame snapshotting, native dispatch completions,
 * and post-dispatch observers. The epoch changes on RX entry/exit and every
 * accepted deferred/UI queue mutation. Queue sizes are captured while both
 * queue mutexes are held; `naturalFrameHadOrderedWork` prevents the last item
 * drained on this outer frame from becoming an immediate same-frame action. */
struct OrderedWorkSnapshot
{
    std::uint64_t epoch{};
    std::uint32_t receiveHookDepth{};
    std::uint32_t originalDispatchDepth{};
    std::uint32_t deferredPacketCount{};
    std::uint32_t uiTaskCount{};
    bool naturalFrameHadOrderedWork{};
};

/** Captures one non-retried snapshot. False means an RX transition overlapped
 * the observation; callers must treat that as busy, never resample inside the
 * same admission attempt. This function only observes state and never drains,
 * reposts, waits, or retries work. */
bool captureOrderedWorkSnapshotForTestdrv(OrderedWorkSnapshot& snapshot);

// Synchronous internal seams: borrowed buffers and callback stages are unchanged.
namespace testdetail {
void observeRx(void* self, std::uint32_t sender, const std::uint8_t* payload,
               std::uint32_t size);
void observeTx(void* self, std::uint32_t target, const game::NetMessageHeader* message);
RxDecision applyRxPolicy(void* self, void* edx, int packet, std::uint32_t length,
                         std::uint32_t sender, std::uint32_t receiver);
TxDecision applyTxPolicy(void* self, std::uint32_t target,
                         const game::NetMessageHeader* message);
void observeRxDispatched(void* self, std::uint32_t sender, std::uint32_t receiver,
                         const std::uint8_t* payload, std::uint32_t size, int result);
void observeTxSent(void* self, std::uint32_t target, const std::uint8_t* message,
                   std::uint32_t size, int result);

// Core owns the dispatcher. Called only after the existing test-only
// host/frame/thread validation succeeds; no queue or new dispatch path.
int dispatchLocalServerFrame(void* receiverSelf, std::uint32_t sender,
                             int receiver, const game::NetMessageHeader* message);
} // namespace testdetail
} // namespace hooks::netintercept
#endif // D2_TESTDRV
#endif // TESTDRV_NETWORKOBSERVERS_H

