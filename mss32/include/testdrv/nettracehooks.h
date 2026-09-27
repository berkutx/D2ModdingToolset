/*
 * DebugTest compatibility adapter over the shared production-safe
 * hooks::netintercept mechanism. RX/TX sites are owned only by netintercept;
 * this module adds test logging and DirectPlay session observability.
 */

#ifndef TESTDRV_NETTRACEHOOKS_H
#define TESTDRV_NETTRACEHOOKS_H

#include "testdrv/networkobservers.h"

namespace hooks {
namespace testdrv {
namespace nettracehooks {

using RxDecision = netintercept::RxDecision;
using TxDecision = netintercept::TxDecision;
using RxTraceCallback = netintercept::RxTraceCallback;
using TxTraceCallback = netintercept::TxTraceCallback;
using RxDispatchCallback = netintercept::RxDispatchCallback;
using TxGateCallback = netintercept::TxGateCallback;
#ifdef D2_TESTDRV
using RxPostDispatchObserver = netintercept::RxPostDispatchObserver;
using TxPostSendObserver = netintercept::TxPostSendObserver;
#endif

/** Process-lifetime observer registrations forwarded to shared netintercept. */
bool addRxObserver(RxTraceCallback callback);
bool addTxObserver(TxTraceCallback callback);
bool addObservers(RxTraceCallback rx, TxTraceCallback tx);
#ifdef D2_TESTDRV
bool addRxPostDispatchObserver(RxPostDispatchObserver callback);
bool addTxPostSendObserver(TxPostSendObserver callback);
#endif

/** Historical testdrv names forwarded only to the independent test policy slots. */
void setDispatchCallback(RxDispatchCallback callback);
void setTxCallback(TxGateCallback callback);

int recvDispatchDepth();
unsigned long mainThreadId();

/** Publish a causal readiness marker on the first natural UI frame after each
 * completed synchronous EnumSessions call. */
void onUiFrame();

/** Read-only validation for the shared RX/TX interception, test-only
 * DirectPlay Enum/Create/Join slots, explicit endpoint, and observer capacity. */
bool preflight(bool enablePacketLogging);

/** Commit the already preflighted native hooks, then publish packet-log
 * observers as one all-or-none bundle. */
bool commit(bool enablePacketLogging);

/** Compatibility one-shot wrapper. New D2_TESTDRV startup uses the explicit
 * preflight/commit phases. */
bool install(bool enablePacketLogging);

} // namespace nettracehooks
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_NETTRACEHOOKS_H

