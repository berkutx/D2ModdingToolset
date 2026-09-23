/* Lobby transport adapter for the unchanged v8 control-client state machine. */
#include "simturns/coordinator_port.h"
#include "simturns/control_client_core.h"
#include <mutex>
#include <utility>

namespace hooks::simturns {

struct CoordinatorPort::Impl
{
    std::mutex mutex;
    ControlClientCore core;
    SimTurnsSessionOptions session;
    Sender sender;
    std::function<void()> terminal;
    std::function<void()> progressWake;
    CoordinatorCallbacks callbacks;
    std::uint32_t epoch{}, mergeDay{};
    std::uint64_t generation{};
    bool armed{}, started{}, failed{};
#ifdef D2_TESTDRV
    bool localPlanIdentity{};
#endif

    // Sender is the non-reentrant RakPeerInterface::Send enqueue operation,
    // not a callback into the coordinator. Serialize its acceptance and the
    // core's Written edge against inbound replies and other native reporters.
    template<class Prepare> bool send(Prepare prepare)
    {
        std::string error;
        std::uint64_t selectedGeneration{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!started || failed) return false;
            selectedGeneration = generation;
            ControlOutboundFrame frame;
            if (prepare(core, frame, error)) {
                core.commitQueued(frame);
                try {
                    if (sender(frame.frame)) {
                        core.completeWritten(frame);
                        return true;
                    }
                } catch (...) { }
                error = "lobby rejected simultaneous-turn control send";
            }
        }
        fault(error.empty() ? "invalid simultaneous-turn report" : error.c_str(), selectedGeneration);
        return false;
    }

    void fault(const char* reason, std::uint64_t expectedGeneration = 0)
    {
        std::function<void()> notify;
        std::function<void(CoordinatorTerminalFault)> sink;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!armed || failed || (expectedGeneration && expectedGeneration != generation)) return;
            failed = true;
            notify = terminal;
            sink = callbacks.terminalFault;
        }
        // Both callbacks may call fail() again. First failure wins; neither
        // runs while holding the core mutex, and no failed send is retried.
        try { if (notify) notify(); } catch (...) { }
        if (sink) sink({reason ? reason : "simultaneous-turn terminal failure"});
    }
};

CoordinatorPort::CoordinatorPort() : impl(new Impl) {}
CoordinatorPort& CoordinatorPort::processInstance()
{
    static auto* const port = new CoordinatorPort;
    return *port;
}

bool CoordinatorPort::arm(const SimTurnsSessionOptions& session,
                           std::uint32_t epoch, std::uint32_t mergeDay,
                           Sender sender, std::function<void()> terminal,
                           std::function<void()> progressWake)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->armed || impl->generation == UINT64_MAX || !session.requested || !epoch || !sender
        || (session.role != Role::Host && session.role != Role::Join)
        || (mergeDay && (mergeDay < 2 || mergeDay > maxEngineDay))) return false;
    impl->session = session;
    ++impl->generation;
    impl->epoch = epoch;
    impl->mergeDay = mergeDay;
    impl->sender = std::move(sender);
    impl->terminal = std::move(terminal);
    impl->progressWake = std::move(progressWake);
    impl->core = {};
    impl->core.configure(session);
    impl->armed = true;
    impl->failed = false;
#ifdef D2_TESTDRV
    impl->localPlanIdentity = false;
#endif
    return true;
}

#ifdef D2_TESTDRV
bool CoordinatorPort::armLocal(const SimTurnsSessionOptions& session, Sender sender,
                                std::function<void()> terminal)
{
    // The adapter arms before publishing its first LocalPlayerHandle, the
    // causal prerequisite for SessionPlan. Reuse normal lifetime validation.
    if (!arm(session, 1, 0, std::move(sender), std::move(terminal))) return false;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->epoch = 0;
    impl->localPlanIdentity = true;
    return true;
}
#endif

bool CoordinatorPort::preflight(const SimTurnsSessionOptions& session, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->armed && !impl->failed && session.requested
        && session.role == impl->session.role) return true;
    error = "lobby has not armed this simultaneous-turn session";
    return false;
}

bool CoordinatorPort::start(const SimTurnsSessionOptions& session, CoordinatorCallbacks callbacks)
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->armed || impl->started || impl->failed || !session.requested
        || session.role != impl->session.role || !callbacks.postToUi
        || !callbacks.terminalFault) return false;
    std::string error;
    if (!impl->core.activate(error)) return false;
    impl->callbacks = std::move(callbacks);
    impl->started = true;
    return true;
}

void CoordinatorPort::quiesce()
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->started = false;
    impl->failed = true;
    impl->callbacks = {};
    impl->sender = {};
    impl->terminal = {};
    impl->progressWake = {};
}

void CoordinatorPort::notifyNativeProgress()
{
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->armed && !impl->failed) wake = impl->progressWake;
    }
    if (wake) wake();
}

void CoordinatorPort::stop()
{
    // Only the old native worker's joined teardown boundary may retire these
    // borrowed service callbacks. Disconnect/Abort merely calls fail().
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->armed = impl->started = impl->failed = false;
    impl->epoch = impl->mergeDay = 0;
    impl->callbacks = {};
    impl->sender = {};
    impl->terminal = {};
    impl->progressWake = {};
    impl->core = {};
}

void CoordinatorPort::receive(const std::uint8_t* bytes, std::size_t size)
{
    std::string error;
    CoordinatorEvent event;
    std::uint64_t selectedGeneration{};
    std::function<void(CoordinatorEvent)> sink;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->armed || impl->failed) return;
        selectedGeneration = impl->generation;
        if (!impl->started || !bytes || size < 8
            || size > protocol::maxFrameLength + 4u) {
            error = "control frame arrived before map binding or has invalid size";
        } else {
            const std::uint32_t length = std::uint32_t(bytes[0])
                | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16)
                | (std::uint32_t(bytes[3]) << 24);
            protocol::FrameDecoder decoder;
            std::vector<protocol::Frame> frames;
            if (length != size - 4 || !decoder.push(bytes, size, frames, error)
                || frames.size() != 1) {
                if (error.empty()) error = "lobby envelope must contain exactly one v8 frame";
            } else {
                const auto& frame = frames.front();
                // Arm and SessionPlan must agree. In particular Stock is not
                // a downgrade path after this room explicitly armed OH.
                if (frame.op == protocol::Op::SessionPlan) {
                    protocol::SessionPlan plan;
                    if (protocol::decodeSessionPlan(frame.payload, plan, error)) {
#ifdef D2_TESTDRV
                        if (impl->localPlanIdentity && plan.mode == TurnMode::Simultaneous) {
                            impl->epoch = plan.epoch;
                            impl->mergeDay = plan.mergeDay;
                            impl->localPlanIdentity = false;
                        }
#endif
                        if (plan.epoch != impl->epoch || plan.mergeDay != impl->mergeDay
                            || plan.mode != TurnMode::Simultaneous)
                            error = "SessionPlan disagrees with armed simultaneous-turn session";
                    }
                }
                ControlInboundEvent decoded;
                ControlFailure failure;
                if (error.empty() && !impl->core.acceptInbound(frame, decoded, failure))
                    error = std::move(failure.message);
                if (error.empty()) {
                    switch (decoded.kind) {
                    case ControlInboundKind::SessionPlan: event.kind = CoordinatorEventKind::SessionPlan; break;
                    case ControlInboundKind::BootstrapCommitted: event.kind = CoordinatorEventKind::BootstrapCommitted; break;
                    case ControlInboundKind::BootstrapOperational: event.kind = CoordinatorEventKind::BootstrapOperational; break;
                    case ControlInboundKind::BootstrapReleased: event.kind = CoordinatorEventKind::BootstrapReleased; break;
                    case ControlInboundKind::EngineAction: event.kind = CoordinatorEventKind::EngineAction; break;
                    default: error = "control core returned no event"; break;
                    }
                    event.sessionPlan = decoded.sessionPlan;
                    event.bootstrapCommitted = decoded.bootstrapCommitted;
                    event.bootstrapOperational = decoded.bootstrapOperational;
                    event.bootstrapReleased = decoded.bootstrapReleased;
                    event.engineAction = decoded.engineAction;
                    sink = impl->callbacks.postToUi;
                }
            }
        }
    }
    if (!error.empty()) { impl->fault(error.c_str(), selectedGeneration); return; }
    try { sink(std::move(event)); }
    catch (...) { impl->fault("could not enqueue lobby control event on UI", selectedGeneration); }
}

bool CoordinatorPort::bindLocalPlayer(std::uint32_t handle)
{
    return impl->send([=](ControlClientCore& core, ControlOutboundFrame& frame, std::string& error) {
        if (core.bindLocalHandle(handle) == LocalHandleBinding::Conflict
            || !core.prepareStoredLocalPlayerHandle(frame)) {
            error = "local player binding is invalid or duplicated"; return false;
        }
        return true;
    });
}

bool CoordinatorPort::operational() const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->started && !impl->failed && impl->core.phase() == ControlPhase::Active;
}
bool CoordinatorPort::endTurnPending() const
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->started && !impl->failed && impl->core.endTurnPending();
}
bool CoordinatorPort::claimEndTurn(std::uint32_t& lease)
{
    std::string error;
    std::uint64_t generation{};
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        lease = 0;
        if (!impl->started || impl->failed) return false;
        generation = impl->generation;
        if (impl->core.claimEndTurn(lease, error)) return true;
    }
    impl->fault(error.c_str(), generation); return false;
}
bool CoordinatorPort::reportSessionActivated()
{
    return impl->send([](auto& core, auto& frame, auto& error) { return core.prepareSessionActivated(frame, error); });
}
bool CoordinatorPort::reportBootstrap(BootstrapCheckpoint checkpoint, std::uint32_t handle, std::uint32_t day)
{
    return impl->send([=](auto& core, auto& frame, auto& error) {
        switch (checkpoint) {
        case BootstrapCheckpoint::BeginTurnApplied: return core.prepareBootstrapBeginTurnApplied(handle, day, frame, error);
        case BootstrapCheckpoint::Complete: return core.prepareBootstrapComplete(handle, day, frame, error);
        case BootstrapCheckpoint::CommitApplied: return core.prepareBootstrapCommitApplied(handle, day, frame, error);
        case BootstrapCheckpoint::OperationalApplied: return core.prepareBootstrapOperationalApplied(handle, day, frame, error);
        }
        error = "unknown bootstrap checkpoint"; return false;
    });
}
bool CoordinatorPort::reportEndTurnObserved(std::uint32_t lease)
{
    return impl->send([=](auto& core, auto& frame, auto& error) { return core.prepareEndTurnObserved(lease, frame, error); });
}
bool CoordinatorPort::reportEndTurnApplied(std::uint32_t originHandle)
{
    return impl->send([=](auto& core, auto& frame, auto& error) { return core.prepareEndTurnApplied(originHandle, frame, error); });
}
bool CoordinatorPort::reportActionResult(const protocol::EngineAction& action, bool success)
{
    return impl->send([&](auto& core, auto& frame, auto& error) { return core.prepareActionResult(action, success, frame, error); });
}
bool CoordinatorPort::reportMergeApplied(std::uint32_t actionId)
{
    return impl->send([=](auto& core, auto& frame, auto& error) { return core.prepareMergeApplied(actionId, frame, error); });
}
bool CoordinatorPort::acceptBootstrapRelease(const protocol::BootstrapProgress& released)
{
    std::string error;
    std::uint64_t generation{};
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->started || impl->failed) return false;
        generation = impl->generation;
        if (impl->core.ackBootstrapReleased(released, error)) return true;
    }
    impl->fault(error.c_str(), generation); return false;
}
bool CoordinatorPort::acceptStockRelease(const protocol::EngineAction& action)
{
    std::string error;
    std::uint64_t generation{};
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->started || impl->failed) return false;
        generation = impl->generation;
        if (impl->core.ackReleaseStock(action, error)) return true;
    }
    impl->fault(error.c_str(), generation); return false;
}
void CoordinatorPort::fail(CoordinatorFailureOrigin, const char* reason) { impl->fault(reason); }

} // namespace hooks::simturns
