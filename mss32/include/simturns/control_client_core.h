/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * Transport-lifecycle-independent simultaneous-turn control-client state
 * machine. The compact v8 wire codec is shared with its adapters, while pipe,
 * SLikeNet, queue, thread, and callback ownership stay outside this class.
 * The relay/lobby owns scheduling, player days, leases, and the merge barrier;
 * this core validates and acknowledges explicit engine actions only.
 */

#ifndef SIMTURNS_CONTROL_CLIENT_CORE_H
#define SIMTURNS_CONTROL_CLIENT_CORE_H

#include "simturns/protocol.h"
#include "simturns/session_types.h"
#include <cstdint>
#include <string>

namespace hooks::simturns {

enum class ControlOutboundKind : std::uint8_t
{
    LocalPlayerHandle,
    SessionActivated,
    BootstrapBeginTurnApplied,
    BootstrapComplete,
    BootstrapCommitApplied,
    BootstrapOperationalApplied,
    EndTurnObserved,
    EndTurnApplied,
    ActionResult,
    MergeApplied,
};

/** Canonical application phase. Transport lifecycle is deliberately separate. */
enum class ControlPhase : std::uint8_t
{
    Dormant,
    AwaitingSessionPlan,
    Stock,
    Bootstrapping,
    BootstrapPrepared,
    Active,
    Holding,
    MergePrepared,
    MergeApplied,
    Merged,
    Failed,
};

enum class ControlInboundKind : std::uint8_t
{
    None,
    SessionPlan,
    BootstrapCommitted,
    BootstrapOperational,
    BootstrapReleased,
    EngineAction,
};

struct ControlInboundEvent
{
    ControlInboundKind kind{};
    protocol::SessionPlan sessionPlan;
    protocol::BootstrapProgress bootstrapCommitted;
    protocol::BootstrapProgress bootstrapOperational;
    protocol::BootstrapProgress bootstrapReleased;
    protocol::EngineAction engineAction;
};

enum class ControlFailureKind : std::uint8_t
{
    None,
    Protocol,
    RemoteError,
};

struct ControlFailure
{
    ControlFailureKind kind{};
    std::string message;
};

struct ControlOutboundFrame
{
    ControlOutboundKind kind{};
    protocol::Bytes frame;
    protocol::EngineAction action;
    std::uint32_t handle{};
    std::uint32_t day{};
    std::uint32_t lease{};
    bool success{};
};

enum class LocalHandleBinding : std::uint8_t
{
    Stored,
    AlreadySame,
    Conflict,
};

/**
 * Protocol/session bookkeeping with no transport lifecycle. The owning adapter
 * serializes every mutating call with its existing lock and owns framing,
 * queues, callbacks, and terminal-fault delivery.
 */
class ControlClientCore final
{
public:
    void configure(const SimTurnsSessionOptions& session);
    bool activate(std::string& error);

    Role role() const;
    TurnMode mode() const;
    std::uint32_t epoch() const;
    std::uint32_t mergeDay() const;
    std::uint32_t hostHandle() const;
    std::uint32_t joinHandle() const;
    ControlPhase phase() const;
    bool localHandleSent() const;

    /** Decode, validate, and apply one ordered coordinator frame atomically. */
    bool acceptInbound(const protocol::Frame& frame,
                       ControlInboundEvent& event,
                       ControlFailure& failure);

    LocalHandleBinding bindLocalHandle(std::uint32_t handle);
    bool prepareStoredLocalPlayerHandle(ControlOutboundFrame& frame) const;

    bool prepareSessionActivated(ControlOutboundFrame& frame, std::string& error) const;
    bool prepareBootstrapBeginTurnApplied(std::uint32_t handle,
                                          std::uint32_t day,
                                          ControlOutboundFrame& frame,
                                          std::string& error) const;
    bool prepareBootstrapComplete(std::uint32_t handle,
                                  std::uint32_t day,
                                  ControlOutboundFrame& frame,
                                  std::string& error) const;
    bool prepareBootstrapCommitApplied(std::uint32_t handle,
                                       std::uint32_t day,
                                       ControlOutboundFrame& frame,
                                       std::string& error) const;
    bool prepareBootstrapOperationalApplied(std::uint32_t handle,
                                            std::uint32_t day,
                                            ControlOutboundFrame& frame,
                                            std::string& error) const;

    /** Claim and publish the one server-issued lease for this local turn. */
    bool claimEndTurn(std::uint32_t& lease, std::string& error);
    bool endTurnPending() const;
    bool prepareEndTurnObserved(std::uint32_t lease,
                                ControlOutboundFrame& frame,
                                std::string& error) const;
    /** Host-only post-original-RX acknowledgement; origin selects the lease. */
    bool prepareEndTurnApplied(std::uint32_t originHandle,
                               ControlOutboundFrame& frame,
                               std::string& error) const;

    bool prepareActionResult(const protocol::EngineAction& action,
                             bool success,
                             ControlOutboundFrame& frame,
                             std::string& error) const;
    bool prepareMergeApplied(std::uint32_t actionId,
                             ControlOutboundFrame& frame,
                             std::string& error) const;

    /** Called only after the adapter accepted the exact frame into its queue. */
    void commitQueued(const ControlOutboundFrame& frame);
    /** Called only after the entire frame was successfully written by transport. */
    void completeWritten(const ControlOutboundFrame& frame);

    bool ackBootstrapReleased(const protocol::BootstrapProgress& released,
                              std::string& error);
    /** ReleaseStock is a server command with no client response. */
    bool ackReleaseStock(const protocol::EngineAction& action, std::string& error);

private:
    enum class DeliveryState : std::uint8_t
    {
        Idle,
        Queued,
        Written,
    };

    bool acceptSessionPlan(const protocol::SessionPlan& plan, std::string& error);
    bool acceptBootstrapCommitted(const protocol::BootstrapProgress& committed,
                                  std::string& error);
    bool acceptBootstrapOperational(const protocol::BootstrapProgress& operational,
                                    std::string& error);
    bool acceptBootstrapReleased(const protocol::BootstrapProgress& released,
                                 std::string& error);
    bool acceptEngineAction(const protocol::EngineAction& action,
                            std::string& error);
    bool validateActionIdentity(const protocol::EngineAction& action,
                                std::string& error) const;
    std::uint32_t localHandle() const;
    std::uint32_t leaseFor(std::uint32_t handle) const;
    void clearLocalEndTurn();
    void clearPendingAction();

    SimTurnsSessionOptions sessionConfig;
    ControlPhase currentPhase{ControlPhase::Dormant};
    TurnMode sessionMode{TurnMode::Stock};
    protocol::SessionPlan plan;

    DeliveryState localHandleDelivery{};
    DeliveryState sessionActivatedDelivery{};
    DeliveryState bootstrapBeginTurnDelivery{};
    DeliveryState bootstrapCompleteDelivery{};
    DeliveryState bootstrapCommitDelivery{};
    DeliveryState bootstrapOperationalDelivery{};
    DeliveryState endTurnObservedDelivery{};
    DeliveryState actionResultDelivery{};
    DeliveryState mergeAppliedDelivery{};

    bool bootstrapCommittedReceived{};
    bool bootstrapOperationalReceived{};
    bool bootstrapReleasedReceived{};
    bool bootstrapReleasedApplied{};
    bool bootstrapCascadeCompleted{};

    std::uint32_t storedLocalHandle{};
    std::uint32_t hostLease{};
    std::uint32_t joinLease{};
    std::uint32_t claimedLocalLease{};
    std::uint32_t reportedHostLease{};
    std::uint32_t reportedJoinLease{};
    std::uint32_t queuedHostLease{};
    std::uint32_t queuedJoinLease{};
    bool localEndTurnPending{};

    bool hasPendingAction{};
    protocol::EngineAction pendingAction;
    std::uint32_t lastActionId{};
    protocol::EngineActionKind lastActionKind{};
    bool hasCompletedApplyTurnStart{};
    protocol::EngineAction completedApplyTurnStart;
    std::uint32_t mergeActionId{};
    bool executeMergeAccepted{};
    bool executeMergeSuccessQueued{};
    bool executeMergeSucceeded{};
};

} // namespace hooks::simturns

#endif // SIMTURNS_CONTROL_CLIENT_CORE_H
