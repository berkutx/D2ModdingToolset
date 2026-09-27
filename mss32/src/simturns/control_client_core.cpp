/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "simturns/control_client_core.h"
#include <utility>

namespace hooks::simturns {

namespace {

bool sameAction(const protocol::EngineAction& left,
                const protocol::EngineAction& right)
{
    return left.epoch == right.epoch && left.actionId == right.actionId
           && left.kind == right.kind && left.playerHandle == right.playerHandle
           && left.day == right.day && left.lease == right.lease;
}

bool isNegotiatedHandle(std::uint32_t handle,
                        std::uint32_t hostHandle,
                        std::uint32_t joinHandle)
{
    return handle && (handle == hostHandle || handle == joinHandle);
}

bool validSharedActionProgression(protocol::EngineActionKind previous,
                                  protocol::EngineActionKind next)
{
    using Kind = protocol::EngineActionKind;
    return (previous == Kind::ApplyTurnStart && next == Kind::ActivateTurn)
           || (previous == Kind::PrepareMerge
               && (next == Kind::ExecuteMerge || next == Kind::ReleaseStock))
           || (previous == Kind::ExecuteMerge && next == Kind::ReleaseStock);
}

} // namespace

void ControlClientCore::configure(const SimTurnsSessionOptions& session)
{
    sessionConfig = session;
}

bool ControlClientCore::activate(std::string& error)
{
    if (currentPhase != ControlPhase::Dormant) {
        error = "control client may be activated exactly once";
        return false;
    }
    if (!sessionConfig.requested) {
        error = "control client activation requires an explicit local opt-in";
        return false;
    }
    if (sessionConfig.role != Role::Host && sessionConfig.role != Role::Join) {
        error = "control client activation requires host or join role";
        return false;
    }
    currentPhase = ControlPhase::AwaitingSessionPlan;
    return true;
}

Role ControlClientCore::role() const
{
    return sessionConfig.role;
}

TurnMode ControlClientCore::mode() const
{
    return sessionMode;
}

std::uint32_t ControlClientCore::epoch() const
{
    return plan.epoch;
}

std::uint32_t ControlClientCore::mergeDay() const
{
    return plan.mergeDay;
}

std::uint32_t ControlClientCore::hostHandle() const
{
    return plan.hostHandle;
}

std::uint32_t ControlClientCore::joinHandle() const
{
    return plan.joinHandle;
}

ControlPhase ControlClientCore::phase() const
{
    return currentPhase;
}

bool ControlClientCore::localHandleSent() const
{
    return localHandleDelivery == DeliveryState::Written;
}

bool ControlClientCore::acceptInbound(const protocol::Frame& frame,
                                      ControlInboundEvent& event,
                                      ControlFailure& failure)
{
    event = {};
    failure = {};

    auto reject = [&failure](ControlFailureKind kind, std::string message) {
        failure.kind = kind;
        failure.message = std::move(message);
        return false;
    };

    std::string error;
    if (frame.flags != 0)
        return reject(ControlFailureKind::Protocol,
                      "protocol v8 frame flags must be zero");
    if (frame.op == protocol::Op::Error) {
        std::string remoteMessage;
        if (!protocol::decodeError(frame.payload, remoteMessage, error))
            return reject(ControlFailureKind::Protocol, std::move(error));
        return reject(ControlFailureKind::RemoteError,
                      "relay Error: "
                          + (remoteMessage.empty() ? std::string("unspecified")
                                                   : remoteMessage));
    }
    if (!protocol::isKnownOp(frame.op))
        return reject(ControlFailureKind::Protocol, "relay sent an unknown opcode");

    if (currentPhase == ControlPhase::AwaitingSessionPlan) {
        if (frame.op != protocol::Op::SessionPlan) {
            return reject(ControlFailureKind::Protocol,
                          std::string("expected SessionPlan, got ")
                              + protocol::opName(frame.op));
        }
        protocol::SessionPlan received;
        if (!protocol::decodeSessionPlan(frame.payload, received, error)
            || !acceptSessionPlan(received, error)) {
            return reject(ControlFailureKind::Protocol, std::move(error));
        }
        event.kind = ControlInboundKind::SessionPlan;
        event.sessionPlan = received;
        return true;
    }

    if (frame.op == protocol::Op::BootstrapCommitted) {
        protocol::BootstrapProgress committed;
        if (!protocol::decodeBootstrapCommitted(frame.payload, committed, error)
            || !acceptBootstrapCommitted(committed, error)) {
            return reject(ControlFailureKind::Protocol, std::move(error));
        }
        event.kind = ControlInboundKind::BootstrapCommitted;
        event.bootstrapCommitted = committed;
        return true;
    }
    if (frame.op == protocol::Op::BootstrapOperational) {
        protocol::BootstrapProgress operational;
        if (!protocol::decodeBootstrapOperational(frame.payload, operational, error)
            || !acceptBootstrapOperational(operational, error)) {
            return reject(ControlFailureKind::Protocol, std::move(error));
        }
        event.kind = ControlInboundKind::BootstrapOperational;
        event.bootstrapOperational = operational;
        return true;
    }
    if (frame.op == protocol::Op::BootstrapReleased) {
        protocol::BootstrapProgress released;
        if (!protocol::decodeBootstrapReleased(frame.payload, released, error)
            || !acceptBootstrapReleased(released, error)) {
            return reject(ControlFailureKind::Protocol, std::move(error));
        }
        event.kind = ControlInboundKind::BootstrapReleased;
        event.bootstrapReleased = released;
        return true;
    }
    if (frame.op == protocol::Op::EngineAction) {
        protocol::EngineAction action;
        if (!protocol::decodeEngineAction(frame.payload, action, error)
            || !acceptEngineAction(action, error)) {
            return reject(ControlFailureKind::Protocol, std::move(error));
        }
        event.kind = ControlInboundKind::EngineAction;
        event.engineAction = action;
        return true;
    }

    return reject(ControlFailureKind::Protocol,
                  std::string("unexpected ") + protocol::opName(frame.op)
                      + " in control phase "
                      + std::to_string(static_cast<unsigned>(currentPhase)));
}

LocalHandleBinding ControlClientCore::bindLocalHandle(std::uint32_t handle)
{
    if (!handle)
        return LocalHandleBinding::Conflict;
    if (!storedLocalHandle) {
        storedLocalHandle = handle;
        return LocalHandleBinding::Stored;
    }
    return storedLocalHandle == handle ? LocalHandleBinding::AlreadySame
                                       : LocalHandleBinding::Conflict;
}

bool ControlClientCore::prepareStoredLocalPlayerHandle(ControlOutboundFrame& frame) const
{
    if (!storedLocalHandle || localHandleDelivery != DeliveryState::Idle
        || currentPhase != ControlPhase::AwaitingSessionPlan) {
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::LocalPlayerHandle;
    frame.handle = storedLocalHandle;
    frame.frame = protocol::encodeLocalPlayerHandle(storedLocalHandle);
    return true;
}

bool ControlClientCore::prepareSessionActivated(ControlOutboundFrame& frame,
                                                std::string& error) const
{
    if (currentPhase != ControlPhase::Bootstrapping
        || sessionActivatedDelivery != DeliveryState::Idle) {
        error = "SessionActivated is not available in the current phase";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::SessionActivated;
    frame.frame = protocol::encodeSessionActivated();
    return true;
}

bool ControlClientCore::prepareBootstrapBeginTurnApplied(
    std::uint32_t handle,
    std::uint32_t day,
    ControlOutboundFrame& frame,
    std::string& error) const
{
    if (sessionConfig.role != Role::Join || currentPhase != ControlPhase::Bootstrapping
        || sessionActivatedDelivery != DeliveryState::Written
        || bootstrapBeginTurnDelivery != DeliveryState::Idle
        || handle != localHandle() || day != 1) {
        error = "BootstrapBeginTurnApplied requires the activated join at day 1";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::BootstrapBeginTurnApplied;
    frame.handle = handle;
    frame.day = day;
    frame.frame = protocol::encodeBootstrapBeginTurnApplied(handle, day);
    return true;
}

bool ControlClientCore::prepareBootstrapComplete(std::uint32_t handle,
                                                 std::uint32_t day,
                                                 ControlOutboundFrame& frame,
                                                 std::string& error) const
{
    if (sessionConfig.role != Role::Join || currentPhase != ControlPhase::Bootstrapping
        || bootstrapBeginTurnDelivery != DeliveryState::Written
        || bootstrapCompleteDelivery != DeliveryState::Idle
        || handle != localHandle() || day != 1) {
        error = "BootstrapComplete requires the applied join day-1 activation";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::BootstrapComplete;
    frame.handle = handle;
    frame.day = day;
    frame.frame = protocol::encodeBootstrapComplete(handle, day);
    return true;
}

bool ControlClientCore::prepareBootstrapCommitApplied(
    std::uint32_t handle,
    std::uint32_t day,
    ControlOutboundFrame& frame,
    std::string& error) const
{
    if (currentPhase != ControlPhase::Bootstrapping || !bootstrapCommittedReceived
        || bootstrapCommitDelivery != DeliveryState::Idle
        || handle != joinHandle() || day != 1) {
        error = "BootstrapCommitApplied does not match the pending commit";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::BootstrapCommitApplied;
    frame.handle = handle;
    frame.day = day;
    frame.frame = protocol::encodeBootstrapCommitApplied(handle, day);
    return true;
}

bool ControlClientCore::prepareBootstrapOperationalApplied(
    std::uint32_t handle,
    std::uint32_t day,
    ControlOutboundFrame& frame,
    std::string& error) const
{
    if (currentPhase != ControlPhase::Bootstrapping || !bootstrapOperationalReceived
        || bootstrapCommitDelivery != DeliveryState::Written
        || bootstrapOperationalDelivery != DeliveryState::Idle
        || handle != joinHandle() || day != 1) {
        error = "BootstrapOperationalApplied does not match the pending prepare";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::BootstrapOperationalApplied;
    frame.handle = handle;
    frame.day = day;
    frame.frame = protocol::encodeBootstrapOperationalApplied(handle, day);
    return true;
}

bool ControlClientCore::claimEndTurn(std::uint32_t& lease, std::string& error)
{
    lease = 0;
    if (currentPhase != ControlPhase::Active || localEndTurnPending) {
        error = "EndTurn is not available in the current local-access state";
        return false;
    }
    const std::uint32_t currentLease = leaseFor(localHandle());
    if (!currentLease) {
        error = "EndTurn has no live server-issued lease";
        return false;
    }
    localEndTurnPending = true;
    claimedLocalLease = currentLease;
    endTurnObservedDelivery = DeliveryState::Idle;
    lease = currentLease;
    return true;
}

bool ControlClientCore::endTurnPending() const
{
    return localEndTurnPending;
}

bool ControlClientCore::prepareEndTurnObserved(std::uint32_t lease,
                                               ControlOutboundFrame& frame,
                                               std::string& error) const
{
    if (currentPhase != ControlPhase::Active || !localEndTurnPending
        || !lease || lease != claimedLocalLease
        || endTurnObservedDelivery != DeliveryState::Idle || !plan.epoch) {
        error = "EndTurnObserved does not match the claimed local turn lease";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::EndTurnObserved;
    frame.lease = lease;
    frame.frame = protocol::encodeEndTurnObserved(plan.epoch, lease);
    return true;
}

bool ControlClientCore::prepareEndTurnApplied(std::uint32_t originHandle,
                                              ControlOutboundFrame& frame,
                                              std::string& error) const
{
    if (sessionConfig.role != Role::Host
        || (currentPhase != ControlPhase::Active
            && currentPhase != ControlPhase::Holding)
        || !isNegotiatedHandle(originHandle, hostHandle(), joinHandle())) {
        error = "EndTurnApplied requires a negotiated origin on the host";
        return false;
    }
    const std::uint32_t lease = leaseFor(originHandle);
    const bool hostOrigin = originHandle == hostHandle();
    const std::uint32_t reported = hostOrigin ? reportedHostLease : reportedJoinLease;
    const std::uint32_t queued = hostOrigin ? queuedHostLease : queuedJoinLease;
    if (!lease || lease == reported || lease == queued) {
        error = "EndTurnApplied is duplicate or has no live origin lease";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::EndTurnApplied;
    frame.handle = originHandle;
    frame.lease = lease;
    frame.frame = protocol::encodeEndTurnApplied(plan.epoch, lease);
    return true;
}

bool ControlClientCore::prepareActionResult(const protocol::EngineAction& action,
                                            bool success,
                                            ControlOutboundFrame& frame,
                                            std::string& error) const
{
    if (!hasPendingAction || !sameAction(action, pendingAction)
        || action.kind == protocol::EngineActionKind::ReleaseStock
        || actionResultDelivery != DeliveryState::Idle) {
        error = "ActionResult does not match the sole pending engine action";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::ActionResult;
    frame.action = action;
    frame.success = success;
    frame.frame = protocol::encodeActionResult(action, success);
    return true;
}

bool ControlClientCore::prepareMergeApplied(std::uint32_t actionId,
                                            ControlOutboundFrame& frame,
                                            std::string& error) const
{
    const bool executeResultAlreadyQueued =
        hasPendingAction
        && pendingAction.kind == protocol::EngineActionKind::ExecuteMerge
        && pendingAction.actionId == actionId
        && actionResultDelivery == DeliveryState::Queued;
    const bool hostExecutionProved =
        sessionConfig.role != Role::Host
        || (executeMergeAccepted
            && (executeMergeSuccessQueued || executeMergeSucceeded));
    if (currentPhase != ControlPhase::MergePrepared || !mergeActionId
        || actionId != mergeActionId
        || !hostExecutionProved
        || (hasPendingAction && !executeResultAlreadyQueued)
        || mergeAppliedDelivery != DeliveryState::Idle) {
        error = "MergeApplied does not match the prepared merge transaction";
        return false;
    }
    frame = {};
    frame.kind = ControlOutboundKind::MergeApplied;
    frame.action.actionId = actionId;
    frame.frame = protocol::encodeMergeApplied(plan.epoch, actionId);
    return true;
}

void ControlClientCore::commitQueued(const ControlOutboundFrame& frame)
{
    switch (frame.kind) {
    case ControlOutboundKind::LocalPlayerHandle:
        localHandleDelivery = DeliveryState::Queued;
        break;
    case ControlOutboundKind::SessionActivated:
        sessionActivatedDelivery = DeliveryState::Queued;
        break;
    case ControlOutboundKind::BootstrapBeginTurnApplied:
        bootstrapBeginTurnDelivery = DeliveryState::Queued;
        break;
    case ControlOutboundKind::BootstrapComplete:
        bootstrapCompleteDelivery = DeliveryState::Queued;
        break;
    case ControlOutboundKind::BootstrapCommitApplied:
        bootstrapCommitDelivery = DeliveryState::Queued;
        break;
    case ControlOutboundKind::BootstrapOperationalApplied:
        bootstrapOperationalDelivery = DeliveryState::Queued;
        break;
    case ControlOutboundKind::EndTurnObserved:
        endTurnObservedDelivery = DeliveryState::Queued;
        break;
    case ControlOutboundKind::EndTurnApplied:
        if (frame.handle == hostHandle())
            queuedHostLease = frame.lease;
        else if (frame.handle == joinHandle())
            queuedJoinLease = frame.lease;
        break;
    case ControlOutboundKind::ActionResult:
        actionResultDelivery = DeliveryState::Queued;
        if (frame.action.kind == protocol::EngineActionKind::ExecuteMerge
            && frame.success) {
            executeMergeSuccessQueued = true;
        }
        break;
    case ControlOutboundKind::MergeApplied:
        mergeAppliedDelivery = DeliveryState::Queued;
        break;
    }
}

void ControlClientCore::completeWritten(const ControlOutboundFrame& frame)
{
    switch (frame.kind) {
    case ControlOutboundKind::LocalPlayerHandle:
        localHandleDelivery = DeliveryState::Written;
        return;
    case ControlOutboundKind::SessionActivated:
        sessionActivatedDelivery = DeliveryState::Written;
        return;
    case ControlOutboundKind::BootstrapBeginTurnApplied:
        bootstrapBeginTurnDelivery = DeliveryState::Written;
        return;
    case ControlOutboundKind::BootstrapComplete:
        bootstrapCompleteDelivery = DeliveryState::Written;
        return;
    case ControlOutboundKind::BootstrapCommitApplied:
        bootstrapCommitDelivery = DeliveryState::Written;
        return;
    case ControlOutboundKind::BootstrapOperationalApplied:
        bootstrapOperationalDelivery = DeliveryState::Written;
        currentPhase = ControlPhase::BootstrapPrepared;
        return;
    case ControlOutboundKind::EndTurnObserved:
        endTurnObservedDelivery = DeliveryState::Written;
        return;
    case ControlOutboundKind::EndTurnApplied:
        if (frame.handle == hostHandle()) {
            queuedHostLease = 0;
            reportedHostLease = frame.lease;
        } else if (frame.handle == joinHandle()) {
            queuedJoinLease = 0;
            reportedJoinLease = frame.lease;
        }
        return;
    case ControlOutboundKind::MergeApplied:
        mergeAppliedDelivery = DeliveryState::Written;
        currentPhase = ControlPhase::MergeApplied;
        return;
    case ControlOutboundKind::ActionResult:
        break;
    }

    if (!hasPendingAction || !sameAction(frame.action, pendingAction)
        || actionResultDelivery != DeliveryState::Queued) {
        return;
    }

    const protocol::EngineAction completed = pendingAction;
    const bool success = frame.success;
    clearPendingAction();
    if (!success) {
        currentPhase = ControlPhase::Failed;
        return;
    }

    switch (completed.kind) {
    case protocol::EngineActionKind::ApplyTurnStart:
        if (completed.playerHandle == hostHandle())
            hostLease = completed.lease;
        else
            joinLease = completed.lease;
        if (currentPhase == ControlPhase::Bootstrapping)
            bootstrapCascadeCompleted = true;
        else {
            hasCompletedApplyTurnStart = true;
            completedApplyTurnStart = completed;
        }
        break;
    case protocol::EngineActionKind::ActivateTurn:
        if (completed.playerHandle == hostHandle())
            hostLease = completed.lease;
        else
            joinLease = completed.lease;
        clearLocalEndTurn();
        break;
    case protocol::EngineActionKind::HoldInput:
        clearLocalEndTurn();
        currentPhase = ControlPhase::Holding;
        break;
    case protocol::EngineActionKind::PrepareMerge:
        currentPhase = ControlPhase::MergePrepared;
        break;
    case protocol::EngineActionKind::ExecuteMerge:
        executeMergeSucceeded = true;
        currentPhase = ControlPhase::MergePrepared;
        break;
    case protocol::EngineActionKind::ReleaseStock:
        break;
    }
}

bool ControlClientCore::ackBootstrapReleased(
    const protocol::BootstrapProgress& released,
    std::string& error)
{
    if (currentPhase != ControlPhase::BootstrapPrepared
        || !bootstrapReleasedReceived || bootstrapReleasedApplied
        || released.handle != joinHandle() || released.day != 1) {
        error = "BootstrapReleased does not match the prepared day-1 session";
        return false;
    }
    bootstrapReleasedApplied = true;
    currentPhase = ControlPhase::Active;
    return true;
}

bool ControlClientCore::ackReleaseStock(const protocol::EngineAction& action,
                                        std::string& error)
{
    if (!hasPendingAction || !sameAction(action, pendingAction)
        || action.kind != protocol::EngineActionKind::ReleaseStock
        || currentPhase != ControlPhase::MergeApplied
        || action.actionId != mergeActionId) {
        error = "ReleaseStock does not match the applied merge transaction";
        return false;
    }
    clearPendingAction();
    currentPhase = ControlPhase::Merged;
    return true;
}

bool ControlClientCore::acceptSessionPlan(const protocol::SessionPlan& received,
                                          std::string& error)
{
    if (currentPhase != ControlPhase::AwaitingSessionPlan
        || localHandleDelivery != DeliveryState::Written || !storedLocalHandle) {
        error = "SessionPlan arrived before the exact local handle write";
        return false;
    }
    const std::uint32_t expectedLocal = sessionConfig.role == Role::Host
                                            ? received.hostHandle
                                            : received.joinHandle;
    if (storedLocalHandle != expectedLocal) {
        error = "SessionPlan role does not match the published local handle";
        return false;
    }
    if (received.mode == TurnMode::Simultaneous
        && received.hostLease == received.joinLease) {
        error = "simultaneous SessionPlan requires distinct turn leases";
        return false;
    }

    plan = received;
    sessionMode = received.mode;
    hostLease = received.hostLease;
    joinLease = received.joinLease;
    currentPhase = received.mode == TurnMode::Simultaneous
                       ? ControlPhase::Bootstrapping
                       : ControlPhase::Stock;
    return true;
}

bool ControlClientCore::acceptBootstrapCommitted(
    const protocol::BootstrapProgress& committed,
    std::string& error)
{
    const bool localPrerequisite = sessionConfig.role == Role::Host
                                       ? bootstrapCascadeCompleted
                                       : bootstrapCompleteDelivery == DeliveryState::Written;
    if (currentPhase != ControlPhase::Bootstrapping || bootstrapCommittedReceived
        || sessionActivatedDelivery != DeliveryState::Written || !localPrerequisite
        || committed.handle != joinHandle() || committed.day != 1) {
        error = "BootstrapCommitted preceded the completed local day-1 bootstrap";
        return false;
    }
    bootstrapCommittedReceived = true;
    return true;
}

bool ControlClientCore::acceptBootstrapOperational(
    const protocol::BootstrapProgress& operational,
    std::string& error)
{
    if (currentPhase != ControlPhase::Bootstrapping
        || !bootstrapCommittedReceived || bootstrapOperationalReceived
        || bootstrapCommitDelivery != DeliveryState::Written
        || operational.handle != joinHandle() || operational.day != 1) {
        error = "BootstrapOperational preceded the local commit acknowledgement";
        return false;
    }
    bootstrapOperationalReceived = true;
    return true;
}

bool ControlClientCore::acceptBootstrapReleased(
    const protocol::BootstrapProgress& released,
    std::string& error)
{
    if (currentPhase != ControlPhase::BootstrapPrepared
        || !bootstrapOperationalReceived || bootstrapReleasedReceived
        || bootstrapOperationalDelivery != DeliveryState::Written
        || released.handle != joinHandle() || released.day != 1) {
        error = "BootstrapReleased preceded the distributed prepare barrier";
        return false;
    }
    bootstrapReleasedReceived = true;
    return true;
}

bool ControlClientCore::validateActionIdentity(
    const protocol::EngineAction& action,
    std::string& error) const
{
    if (action.epoch != plan.epoch) {
        error = "EngineAction epoch does not match SessionPlan";
        return false;
    }
    if (hasPendingAction) {
        error = "EngineAction overtook the sole pending engine action";
        return false;
    }
    if (lastActionId && action.actionId < lastActionId) {
        error = "EngineAction actionId moved backwards";
        return false;
    }
    if (action.actionId == lastActionId) {
        if (action.kind == lastActionKind) {
            error = "duplicate EngineAction identity";
            return false;
        }
        if (!validSharedActionProgression(lastActionKind, action.kind)) {
            error = "EngineAction reused actionId outside its transaction";
            return false;
        }
    }
    return true;
}

bool ControlClientCore::acceptEngineAction(const protocol::EngineAction& action,
                                           std::string& error)
{
    if (sessionMode != TurnMode::Simultaneous
        || !validateActionIdentity(action, error)) {
        if (error.empty())
            error = "EngineAction requires a simultaneous SessionPlan";
        return false;
    }

    const auto belowMerge = [this](std::uint32_t day) {
        return day && (!plan.mergeDay || day < plan.mergeDay);
    };

    switch (action.kind) {
    case protocol::EngineActionKind::ApplyTurnStart: {
        if (sessionConfig.role != Role::Host
            || !isNegotiatedHandle(action.playerHandle, hostHandle(), joinHandle())
            || !belowMerge(action.day)) {
            error = "ApplyTurnStart requires a pre-merge negotiated player on host";
            return false;
        }
        if (currentPhase == ControlPhase::Bootstrapping) {
            if (sessionActivatedDelivery != DeliveryState::Written
                || bootstrapCascadeCompleted || action.playerHandle != joinHandle()
                || action.day != 1 || action.lease != joinLease) {
                error = "bootstrap ApplyTurnStart must target the initial join grant";
                return false;
            }
        } else if (currentPhase != ControlPhase::Active
                   && currentPhase != ControlPhase::Holding) {
            error = "ApplyTurnStart arrived outside an active host engine phase";
            return false;
        } else if ((currentPhase == ControlPhase::Holding
                    && action.playerHandle == localHandle())
                   || action.lease == leaseFor(action.playerHandle)
                   || action.lease == leaseFor(action.playerHandle == hostHandle()
                                                   ? joinHandle()
                                                   : hostHandle())) {
            error = "ordinary ApplyTurnStart has an invalid target or reused lease";
            return false;
        } else {
            const std::uint32_t currentLease = leaseFor(action.playerHandle);
            const std::uint32_t reportedLease = action.playerHandle == hostHandle()
                                                    ? reportedHostLease
                                                    : reportedJoinLease;
            if (!currentLease || reportedLease != currentLease) {
                error = "ApplyTurnStart preceded the host EndTurnApplied write";
                return false;
            }
            if (action.playerHandle == localHandle()
                && (!localEndTurnPending || claimedLocalLease != currentLease
                    || endTurnObservedDelivery != DeliveryState::Written)) {
                error = "local ApplyTurnStart preceded the EndTurnObserved write";
                return false;
            }
        }
        break;
    }
    case protocol::EngineActionKind::ActivateTurn:
        if (currentPhase != ControlPhase::Active || !localEndTurnPending
            || endTurnObservedDelivery != DeliveryState::Written
            || action.playerHandle != localHandle() || !belowMerge(action.day)
            || action.lease == claimedLocalLease
            || action.lease == leaseFor(action.playerHandle == hostHandle()
                                            ? joinHandle()
                                            : hostHandle())) {
            error = "ActivateTurn does not match the pending local turn";
            return false;
        }
        if (sessionConfig.role == Role::Host
            && (!hasCompletedApplyTurnStart
                || action.actionId != completedApplyTurnStart.actionId
                || action.playerHandle != completedApplyTurnStart.playerHandle
                || action.day != completedApplyTurnStart.day
                || action.lease != completedApplyTurnStart.lease)) {
            error = "host ActivateTurn does not match its completed ApplyTurnStart";
            return false;
        }
        break;
    case protocol::EngineActionKind::HoldInput:
        if (currentPhase != ControlPhase::Active || !localEndTurnPending
            || endTurnObservedDelivery != DeliveryState::Written
            || !plan.mergeDay || action.playerHandle != localHandle()
            || action.day != plan.mergeDay - 1 || action.lease != 0) {
            error = "HoldInput does not match the local pre-merge barrier arrival";
            return false;
        }
        break;
    case protocol::EngineActionKind::PrepareMerge:
        if (currentPhase != ControlPhase::Holding
            || action.playerHandle != hostHandle() || !plan.mergeDay
            || action.day != plan.mergeDay || action.lease != 0) {
            error = "PrepareMerge requires the held lobby-owned merge target";
            return false;
        }
        mergeActionId = action.actionId;
        executeMergeAccepted = false;
        executeMergeSuccessQueued = false;
        executeMergeSucceeded = false;
        break;
    case protocol::EngineActionKind::ExecuteMerge:
        if (sessionConfig.role != Role::Host
            || currentPhase != ControlPhase::MergePrepared || !mergeActionId
            || action.actionId != mergeActionId
            || action.playerHandle != hostHandle() || action.day != plan.mergeDay
            || action.lease != 0) {
            error = "ExecuteMerge does not match the prepared host merge";
            return false;
        }
        executeMergeAccepted = true;
        break;
    case protocol::EngineActionKind::ReleaseStock:
        if (currentPhase != ControlPhase::MergeApplied || !mergeActionId
            || action.actionId != mergeActionId
            || action.playerHandle != hostHandle() || action.day != plan.mergeDay
            || action.lease != 0) {
            error = "ReleaseStock does not match the applied merge";
            return false;
        }
        break;
    }

    hasPendingAction = true;
    pendingAction = action;
    actionResultDelivery = DeliveryState::Idle;
    lastActionId = action.actionId;
    lastActionKind = action.kind;
    return true;
}

std::uint32_t ControlClientCore::localHandle() const
{
    if (plan.epoch) {
        return sessionConfig.role == Role::Host ? plan.hostHandle : plan.joinHandle;
    }
    return storedLocalHandle;
}

std::uint32_t ControlClientCore::leaseFor(std::uint32_t handle) const
{
    if (handle == hostHandle())
        return hostLease;
    if (handle == joinHandle())
        return joinLease;
    return 0;
}

void ControlClientCore::clearLocalEndTurn()
{
    localEndTurnPending = false;
    claimedLocalLease = 0;
    endTurnObservedDelivery = DeliveryState::Idle;
}

void ControlClientCore::clearPendingAction()
{
    hasPendingAction = false;
    pendingAction = {};
    actionResultDelivery = DeliveryState::Idle;
}

} // namespace hooks::simturns
