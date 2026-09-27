/*
 * Process-local engine state shared by the production simultaneous-turn
 * modules. Scheduling and player-day authority remain in the relay/lobby.
 * The current game integration supports exactly one host and one joiner.
 */

#ifndef SIMTURNS_STATE_H
#define SIMTURNS_STATE_H

#include "simturns/session_types.h"
#include <cstdint>
#include <string>

namespace hooks::simturns {

using FaultObserver = void (*)(const char* reason);

enum class Phase : std::uint32_t
{
    Disabled,
    Prepared,
    WaitingForSession,
    /** The lobby explicitly selected ordinary stock turn ownership. */
    Stock,
    Ready,
    Independent,
    Held,
    Merging,
    AwaitingStockTurn,
    Merged,
    /** Native map teardown is in progress; no gameplay work may start. */
    Closing,
    Faulted,
};

void initializeState(Role role);
void markEngineInstalled();
/** Quietly closes native admission before deliberate map destruction. */
void closeStateForTeardown();
/** Only after worker join, queue cancellation and complete patch restoration. */
void resetState();

/** Registers the process-lifetime one-shot terminal-fault sink. Registration
 * is idempotent for the same function and must precede the first pipe start. */
bool setFaultObserver(FaultObserver observer);

/** Records the local CMidgardID before it is published to the coordinator. */
bool publishLocalHandle(std::uint32_t handle);

/** Applies the lobby's explicit Stock policy without activating the overlay. */
bool authorizeStock();

/** Accepts the coordinator's explicit simultaneous SessionPlan snapshot. */
bool establishSession(std::uint32_t hostHandle,
                      std::uint32_t joinHandle,
                      std::uint32_t mergeDay);

bool activateIndependent();
bool enterHeld();
bool beginMerge();
/** Host-only completion of Merging -> Merged. A concurrent terminal fault
 * wins and is never overwritten by a late merge callback. */
bool finishMerge(std::uint32_t mergeDay);
/** Join-only handoff that keeps all action gates closed through natural RX
 * dispatch, queue drain, and global release. */
bool awaitStockTurn(std::uint32_t mergeDay);
/** Completes only AwaitingStockTurn -> Merged. The controller calls this only
 * for lobby ReleaseStock after both clients reported the exact natural
 * BeginTurn queue-drain boundary. */
bool finishStockTurn();

Phase phase();
Role role();
bool isHost();
/** True while independent-day semantics still own engine effects, including
 * the first-arriver hold where peer traffic can remain in flight. */
bool activePremerge();
bool ownsVirtualTurn();
bool isAwaitingStockTurn();
bool isMerged();
bool isFaulted();

std::uint32_t configuredMergeDay();
std::uint32_t hostHandle();
std::uint32_t joinHandle();
std::uint32_t localHandle();
std::uint32_t otherHandle();

/** Terminal for the complete coordinated process lifetime. First reason wins
 * and is propagated once to the registered control-pipe sink. Safe from any
 * thread. */
void fault(const char* reason);
std::string faultReason();

} // namespace hooks::simturns

#endif // SIMTURNS_STATE_H
