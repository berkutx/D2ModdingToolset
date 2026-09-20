#ifndef C4_NETBOUNDARYTRACE_H
#define C4_NETBOUNDARYTRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Native EXE call boundaries only. No MSS offsets, reads of its private queues,
 * extra network calls, or replacement of its virtual-method implementation.
 * Install at initial GUI dispatch, before a network session is created. */
void netboundarytrace_install(void);
enum C4NetBoundaryEvent {
    C4NET_BOUNDARY_READY = 200, C4NET_BOUNDARY_UNAVAILABLE = 201,
    C4NET_SEND_ENTER = 202, C4NET_SEND_RESULT = 203,
    C4NET_RECEIVE = 204, C4NET_FRAME = 205,
    C4NET_EXCEPTION = 208,
    C4NET_TOTALS = 210, C4NET_RECEIVE_TOTALS = 211, C4NET_DIAGNOSTIC_TOTALS = 212
};
enum C4NetBoundarySite {
    C4NET_CLIENT_SEND = 1, C4NET_SERVER_SEND = 2,
    C4NET_CLIENT_RECEIVE = 4, C4NET_SERVER_RECEIVE = 8,
    C4NET_CLIENT_COUNT = 16, C4NET_SERVER_COUNT = 32
};
typedef struct C4NetBoundaryRoleCounters {
    uint32_t sends, sendFailures, receives, receiveSuccess, receiveEmpty, receiveFailures;
    uint32_t countCalls, lastCount, activeCalls, exceptions, selected, malformed;
} C4NetBoundaryRoleCounters;
typedef struct C4NetBoundaryCounters {
    uint32_t installedMask;
    C4NetBoundaryRoleCounters client, server;
} C4NetBoundaryCounters;
/* Atomic per-field samples, not a transactionally consistent whole snapshot.
 * lastCount is the last REAL game count-call result, never a fresh queue probe.
 * Counters wrap at 2^32. No game/MSS calls; preserves LastError. */
void netboundarytrace_sampleCounters(C4NetBoundaryCounters* out);
/* Emit the cumulative snapshots above; caller supplies the sampling cadence. */
void netboundarytrace_sample(void);
#ifdef __cplusplus
}
#endif
#endif
