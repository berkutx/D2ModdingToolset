/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Unified, transport-agnostic network interception + logging. ONE recv-dispatch
 * hook (sub_55B948) sees both native DirectPlay and the mod's SLikeNet RX (it
 * sits at the IMqNetReception level, above the transport split); the DirectPlay
 * Send vtable hook covers native TX (custom TX is already spdlog-logged by the
 * mod). This module is pure mechanism + logging: it owns NO gameplay policy.
 *
 * Two kinds of seam are exposed:
 *   - trace sinks (setRx/TxTraceCallback)  — observability, used by the relay bridge;
 *   - gating callbacks (setDispatch/TxCallback) — a single hook point where a
 *     consumer (e.g. a relay-driven test, or another module) may pass / drop /
 *     defer (RX) or pass / drop / redirect (TX) a packet. Null in a public build,
 *     so the hook then merely logs and passes.
 *
 * Compile-gated by D2_TESTDRV; without it the whole module compiles to nothing.
 */

#ifndef TESTDRV_NETTRACEHOOKS_H
#define TESTDRV_NETTRACEHOOKS_H

#include <cstdint>

namespace game {
struct NetMessageHeader;
}

namespace hooks {
namespace testdrv {
namespace nettracehooks {

/** Inbound (RX) gate decision. */
enum class RxDecision
{
    Pass,  // call the original recv-dispatch now
    Drop,  // swallow the packet (never delivered)
    Defer, // copy + replay it later on the UI thread at depth 0
};

/** Outbound (TX) gate decision. */
enum class TxDecision
{
    Pass,     // call the original send now
    Drop,     // swallow the send (never put on the wire)
    Redirect, // the gate delivered it itself; swallow the native send
};

/** Observability sink for a received CNetMsg envelope. `payload` points at the
 * NetMessageHeader's class name; runs on the game/worker thread (enqueue only). */
using RxTraceCallback = void (*)(void* self, int sender, const std::uint8_t* payload,
                                 std::uint32_t size);
/** Observability sink for an outgoing CNetMsg. `message` points at the raw
 * NetMessageHeader bytes. */
using TxTraceCallback = void (*)(void* self, std::uint32_t idTo, const std::uint8_t* message,
                                 std::uint32_t size);

/** RX gate: decide pass/drop/defer for an inbound packet. May consult
 * recvDispatchDepth()/mainThreadId(). */
using RxDispatchCallback = RxDecision (*)(void* self, void* edx, int packet, int size, int sender);
/** TX gate: decide pass/drop/redirect for an outbound message. */
using TxGateCallback = TxDecision (*)(void* self, std::uint32_t idTo,
                                      const game::NetMessageHeader* message);

/** Register an observer of received envelopes. Multiple may be registered (e.g.
 * the relay bridge AND the MP-gate detector); each is called per inbound packet.
 * Register at install time (process-lifetime; there is no removal). */
void addRxObserver(RxTraceCallback cb);
/** Register an observer of outgoing messages (multiple allowed). */
void addTxObserver(TxTraceCallback cb);

void setDispatchCallback(RxDispatchCallback cb);
void setTxCallback(TxGateCallback cb);

/** Recursion depth around the original recv-dispatch (0 == outermost). */
int recvDispatchDepth();
/** The game UI thread id (captured once the game window is subclassed); 0 until then. */
unsigned long mainThreadId();

/** Install the RX recv-dispatch hook + the DirectPlay TX Send hook (Russobit-
 * pinned, idempotent — safe to call from more than one installer). Returns true
 * if the RX call-site patches applied. */
bool install();

} // namespace nettracehooks
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_NETTRACEHOOKS_H
