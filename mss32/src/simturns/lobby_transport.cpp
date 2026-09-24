#include "simturns/lobby_transport.h"
#include "simturns/lobby_wire.h"
#include "simturns/controller.h"
#include "simturns/coordinator_port.h"
#include "simturns/native_apply_fence.h"
#include "simturns/native_notification_policy.h"
#include "netcustomservice.h"
#include "netcustomsession.h"
#include "netintercept.h"
#include "netmsg.h"
#include <BitStream.h>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace hooks::simturns {
namespace {
// Room/epoch selection is UI-thread-owned. Native server callbacks can send
// confirmations: the shared binding serializes those sends with invalidation,
// so no callback retains a service pointer beyond its lifetime.
struct Binding {
    struct PendingDelivery {
        std::uint64_t barrier{};
        protocol::Bytes bytes;
        std::function<void()> native;
    };
    std::mutex mutex;
    CNetCustomService* service{};
    std::uint32_t room{}, epoch{};
    Role role{};
    bool sending{true};
    bool nativeReady{};
    bool progressQueued{};
    bool noticePending{};
    PregameJoinSnapshotPolicy pregameSnapshot;
    NativeApplyFence nativeApplied;
    std::vector<std::uint64_t> awaitingClientDrain;
    std::deque<PendingDelivery> deliveries;
    std::size_t pendingFrames{};
};
CNetCustomService* owner{};
std::optional<std::uint32_t> room;
std::uint32_t highWaterEpoch{};
std::shared_ptr<Binding> binding;
// Selection changes are UI-owned; the native server's start-message guard takes
// a shared binding snapshot without racing room replacement or native teardown.
std::mutex selectionMutex;
const char* roleName(Role role) { return role == Role::Host ? "host" : "join"; }
const char* policyName(netintercept::RxDecision policy) {
    switch (policy) {
    case netintercept::RxDecision::Pass: return "pass";
    case netintercept::RxDecision::Drop: return "drop";
    case netintercept::RxDecision::Consume: return "consume";
    case netintercept::RxDecision::Defer: return "defer";
    }
    return "unknown";
}
// Diagnostic I/O must not suppress an Abort, alter admission or escape a native
// completion callback. No raw payload, chat, account name or credentials here.
template<class... Args>
void diagnostic(spdlog::level::level_enum level, const char* format, const Args&... args) noexcept {
    try {
        spdlog::log(level, format, args...);
        spdlog::default_logger()->flush();
    } catch (...) { }
}
void scheduleProgress(const std::shared_ptr<Binding>& selected);
void drainProgress(const std::shared_ptr<Binding>& selected);
void reportFailure(const std::shared_ptr<Binding>& selected) {
    {
        std::lock_guard lock(selected->mutex);
        selected->noticePending = true;
    }
    scheduleProgress(selected);
}

bool send(CNetCustomService* service, const lobby::Envelope& envelope) {
    const auto bytes = lobby::encode(envelope);
    if (!service || bytes.empty()) return false;
    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_SIMULTANEOUS_TURNS));
    stream.WriteAlignedBytes(bytes.data(), static_cast<unsigned>(bytes.size()));
    return service->send(stream, service->getLobbyGuid(), HIGH_PRIORITY);
}
bool stopSending(const std::shared_ptr<Binding>& selected) {
    if (!selected) return false;
    std::lock_guard lock(selected->mutex);
    const bool wasSending = selected->sending;
    selected->sending = false;
    return wasSending;
}
void rejectActive(lobby::AbortReason reason, const char* detail) {
    if (!binding) return;
    // Explicit abort paths suppress the port's generic LocalFailure notice.
    {
        std::lock_guard lock(binding->mutex);
        if (!binding->sending) return;
        diagnostic(spdlog::level::err,
                   "[simturns-diag] local_abort pid={} room={} epoch={} role={} reason={} detail={}",
                   GetCurrentProcessId(), binding->room, binding->epoch, roleName(binding->role),
                   static_cast<unsigned>(reason), detail);
        lobby::Envelope abort;
        abort.operation = lobby::Operation::Abort; abort.room = binding->room;
        abort.epoch = binding->epoch; abort.reason = reason;
        send(binding->service, abort);
        binding->sending = false;
    }
    reportFailure(binding);
    CoordinatorPort::processInstance().fail(CoordinatorFailureOrigin::LocalInvariant, detail);
}
void acknowledge(CNetCustomService* service, const lobby::Envelope& arm, bool accepted) {
    lobby::Envelope ack;
    ack.operation = lobby::Operation::ArmAck; ack.room = arm.room; ack.epoch = arm.epoch;
    ack.status = accepted ? 0 : 1;
    if (!send(service, ack) && accepted)
        rejectActive(lobby::AbortReason::LocalFailure, "cannot acknowledge lobby session arm");
}

void drainProgress(const std::shared_ptr<Binding>& selected) {
    {
        std::lock_guard lock(selected->mutex);
        if (!selected->sending) return;
    }
    if (strategicQueueIdle()) {
        for (const auto sequence : selected->awaitingClientDrain) {
            if (!selected->nativeApplied.complete(sequence)) {
                rejectActive(lobby::AbortReason::Protocol, "duplicate native command completion"); return;
            }
        }
        selected->awaitingClientDrain.clear();
    }
    while (!selected->deliveries.empty()) {
        auto& next = selected->deliveries.front();
        if (!next.native && !selected->nativeApplied.reached(next.barrier)) return;
        auto delivery = std::move(next);
        selected->deliveries.pop_front();
        if (delivery.native) {
            delivery.native();
        } else {
            --selected->pendingFrames;
            CoordinatorPort::processInstance().receive(delivery.bytes.data(), delivery.bytes.size());
        }
        std::lock_guard lock(selected->mutex);
        if (!selected->sending) return;
    }
}
void discardProgress(void* context) { delete static_cast<std::shared_ptr<Binding>*>(context); }
void runProgress(void* context) {
    std::unique_ptr<std::shared_ptr<Binding>> selected(static_cast<std::shared_ptr<Binding>*>(context));
    CNetCustomService* noticeService{};
    {
        std::lock_guard lock((*selected)->mutex);
        (*selected)->progressQueued = false;
        if ((*selected)->noticePending) {
            (*selected)->noticePending = false;
            noticeService = (*selected)->service;
        }
    }
    if (binding == *selected && noticeService && noticeService == owner
        && noticeService == CNetCustomService::get())
        noticeService->enqueueSystemNotice("Simultaneous turns stopped: the lobby/native protocol failed. "
                                          "No further game commands will be accepted for this map.");
    drainProgress(*selected);
}
void scheduleProgress(const std::shared_ptr<Binding>& selected) {
    {
        std::lock_guard lock(selected->mutex);
        if (selected->progressQueued || (!selected->sending && !selected->noticePending)) return;
        selected->progressQueued = true;
    }
    auto context = std::make_unique<std::shared_ptr<Binding>>(selected);
    if (netintercept::queueOnNextUiFrame(runProgress, context.get(), discardProgress)) {
        context.release(); return;
    }
    std::lock_guard lock(selected->mutex);
    if (selected->sending) {
        spdlog::critical("Cannot enqueue the native/control ordering barrier");
        std::terminate(); // Losing this edge would turn a native apply into a false acknowledgement.
    }
}
} // namespace

struct LobbyNativeTicket {
    std::weak_ptr<Binding> owner;
    std::uint64_t sequence{};
    bool clientReceiver{};
};

namespace {
struct NativeCompletion {
    std::shared_ptr<LobbyNativeTicket> ticket;
    netintercept::NativeReceiveResult result{};
    netintercept::NativeReceiveDiagnostic diagnostic;
    bool allowedNotification{};
    bool pregameJoinNotification{};
    std::uint64_t pregameGeneration{};
};
void discardNativeCompletion(void* context) { delete static_cast<NativeCompletion*>(context); }
void completeNativeOnUi(void* context) {
    std::unique_ptr<NativeCompletion> completion(static_cast<NativeCompletion*>(context));
    auto selected = completion->ticket->owner.lock();
    if (!selected) return;
    {
        std::lock_guard lock(selected->mutex);
        if (!selected->sending) return;
    }
    if (completion->result == netintercept::NativeReceiveResult::Failed
        || completion->result == netintercept::NativeReceiveResult::Unhandled) {
        const auto& d = completion->diagnostic;
        diagnostic(spdlog::level::err,
                   "[simturns-diag] native_failed pid={} room={} epoch={} role={} ticket={} endpoint={} "
                   "class={} bytes={} type={:#x} sender={:#x} receiver={:#x} rx_tid={} "
                   "site={} replay={} policy={} dispatched={} handler_count={} pending_controls={} awaiting_drain={}",
                   GetCurrentProcessId(), selected->room, selected->epoch, roleName(selected->role),
                   completion->ticket->sequence, completion->ticket->clientReceiver ? "client" : "server",
                   d.messageClass, d.frameLength, d.messageType, d.sender, d.receiver, d.threadId,
                   d.replay ? "replay" : (d.captureDPlaySelf ? "0x402ca7" : "0x43396e"), d.replay,
                   policyName(d.policy), d.dispatched, d.dispatchResult,
                   selected->pendingFrames, selected->awaitingClientDrain.size());
        rejectActive(lobby::AbortReason::Protocol, "native packet did not complete its handler"); return;
    }
    if (completion->ticket->clientReceiver
        && completion->result == netintercept::NativeReceiveResult::Applied
        && !strategicQueueIdle()) {
        selected->awaitingClientDrain.push_back(completion->ticket->sequence);
    } else if (!selected->nativeApplied.complete(completion->ticket->sequence)) {
        rejectActive(lobby::AbortReason::Protocol, "duplicate native packet completion"); return;
    }
    drainProgress(selected);
}
void nativeReceiveCompleted(void* context, netintercept::NativeReceiveResult result,
                            const netintercept::NativeReceiveDiagnostic& diagnostic) {
    std::unique_ptr<NativeCompletion> completion(static_cast<NativeCompletion*>(context));
    completion->result = result;
    completion->diagnostic = diagnostic;
    const auto selected = completion->ticket->owner.lock();
    if (!selected) return;
    {
        std::lock_guard lock(selected->mutex);
        if (!selected->sending) return;
        // A nested native receive may already have crossed the snapshot boundary.
        if (completion->pregameJoinNotification && selected->pregameSnapshot.scenarioStarted())
            completion->allowedNotification = false;
    }
    // Classify synchronously, before a queued UI task can enter another phase.
    // The stage snapshot and current generation must describe the same pregame
    // lifetime; the weak binding above also rejects a retired lobby map.
    if (result == netintercept::NativeReceiveResult::Unhandled) {
        completion->result = resolveLobbyNativeReceiveResult(
            result, completion->allowedNotification, completion->pregameGeneration,
            completion->allowedNotification ? pregameNativeNotificationGeneration() : 0);
    }
    // The native dispatcher may run on the server worker. Never inspect the
    // strategic command queue or run a coordinator action on that thread.
    if (netintercept::queueOnNextUiFrame(completeNativeOnUi, completion.get(), discardNativeCompletion)) {
        completion.release(); return;
    }
    std::lock_guard lock(selected->mutex);
    if (selected->sending) {
        spdlog::critical("Cannot enqueue native receive completion");
        std::terminate();
    }
}
} // namespace

bool lobbySupported() { return available(); }

bool lobbyMapArmed(const CNetCustomService* service) {
    std::shared_ptr<Binding> selected;
    {
        std::lock_guard selectionLock(selectionMutex);
        if (!binding || owner != service || !room || binding->room != *room) return false;
        selected = binding;
    }
    std::lock_guard lock(selected->mutex);
    return selected->sending && selected->nativeReady;
}

std::shared_ptr<LobbyNativeTicket> lobbyTrackNativePacket(bool clientReceiver) {
    if (!binding) return {};
    std::lock_guard lock(binding->mutex);
    if (!binding->sending) return {};
    auto ticket = std::make_shared<LobbyNativeTicket>();
    ticket->owner = binding; ticket->sequence = binding->nativeApplied.issue();
    ticket->clientReceiver = clientReceiver;
    return ticket;
}

void lobbyDeliverNativePacket(std::shared_ptr<LobbyNativeTicket> ticket,
                              std::function<void()> delivery) {
    if (!ticket) { delivery(); return; }
    const auto selected = ticket->owner.lock();
    if (!selected) return;
    {
        std::lock_guard lock(selected->mutex);
        if (!selected->sending) return;
    }
    // Do not let packets after a control barrier enter the strategic queue.
    // Release them when the control is delivered, NOT when its engine ACK arrives:
    // bootstrap/turn actions require subsequent native packets to make progress.
    if (selected->deliveries.empty()) { delivery(); return; }
    selected->deliveries.push_back({0, {}, std::move(delivery)});
    scheduleProgress(selected);
}

void lobbyDiscardNativePacket(std::shared_ptr<LobbyNativeTicket> ticket) {
    if (!ticket) return;
    auto completion = std::make_unique<NativeCompletion>();
    completion->ticket = std::move(ticket);
    nativeReceiveCompleted(completion.release(), netintercept::NativeReceiveResult::Filtered, {});
}

bool lobbyStageNativeReceive(const game::NetMessageHeader* buffer,
                            std::shared_ptr<LobbyNativeTicket> ticket,
                            std::uint32_t sender) {
    if (!ticket) return true;
    const auto selected = ticket->owner.lock();
    const auto retired = [&selected]() {
        if (!selected) return true;
        std::lock_guard lock(selected->mutex);
        return !selected->sending;
    };
    // Teardown retires confirmations before joining the native worker. Its
    // already-received packets still follow native dispatch, without an OH ACK.
    if (retired()) return true;
    auto context = std::make_unique<NativeCompletion>();
    context->ticket = std::move(ticket);
    if (buffer) {
        {
            std::lock_guard lock(selected->mutex);
            // Latch before dispatch, never from a delayed UI completion. A new
            // map gets a new Binding; leaving/re-entering a menu cannot reopen it.
            selected->pregameSnapshot.observe(buffer->messageType, buffer->length,
                buffer->messageClassName, context->ticket->clientReceiver, sender);
            std::uint32_t startupWords[3]{};
            if (buffer->length == sizeof(game::NetMessageHeader) + sizeof(startupWords))
                std::memcpy(startupWords, buffer + 1, sizeof(startupWords));
            context->pregameJoinNotification = selected->pregameSnapshot.allowsUnhandledRefresh(
                buffer->messageType, buffer->length, buffer->messageClassName,
                selected->role == Role::Join, context->ticket->clientReceiver, sender)
                || selected->pregameSnapshot.allowsUnhandledStartupBeginTurn(
                    buffer->messageType, buffer->length, buffer->messageClassName, startupWords,
                    selected->role == Role::Join, context->ticket->clientReceiver, sender)
                || selected->pregameSnapshot.allowsUnhandledJoinGame(
                    buffer->messageType, buffer->length, buffer->messageClassName,
                    reinterpret_cast<const std::uint8_t*>(buffer + 1),
                    buffer->length >= sizeof(*buffer) ? buffer->length - sizeof(*buffer) : 0,
                    selected->role == Role::Join, context->ticket->clientReceiver, sender);
            context->allowedNotification = context->pregameJoinNotification || isPregameConnectNotification(
                buffer->messageType, buffer->length, buffer->messageClassName,
                context->ticket->clientReceiver, sender);
        }
        if (context->allowedNotification)
            context->pregameGeneration = pregameNativeNotificationGeneration();
    }
    if (!netintercept::stageNativeReceive(buffer, context.get(), nativeReceiveCompleted,
                                         discardNativeCompletion)) {
        if (retired()) return true; // Teardown may have won between check and registration.
        CoordinatorPort::processInstance().fail(CoordinatorFailureOrigin::LocalInvariant,
                                                "cannot stage native receive completion");
        return false;
    }
    context.release(); return true;
}

void lobbyRoomJoined(CNetCustomService* service, std::uint32_t roomId) {
    if (owner == service && room && *room == roomId) return;
    if (binding) rejectActive(lobby::AbortReason::RoomLeft, "room replaced before native teardown");
    if (owner != service) highWaterEpoch = 0;
    std::lock_guard selectionLock(selectionMutex);
    owner = service; room = roomId;
}

void lobbyRoomLeft(CNetCustomService* service) {
    if (owner != service) return;
    rejectActive(lobby::AbortReason::RoomLeft, "simultaneous-turn room left");
    std::lock_guard selectionLock(selectionMutex);
    room.reset(); // Epoch high-water survives room changes on this authenticated connection.
}

void lobbyDisconnected(CNetCustomService* service) {
    if (owner != service) return;
    const bool wasActive = stopSending(binding);
    if (binding) {
        std::lock_guard lock(binding->mutex);
        binding->service = nullptr;
    }
    {
        std::lock_guard selectionLock(selectionMutex);
        owner = nullptr; room.reset(); highWaterEpoch = 0;
    }
    if (wasActive)
        CoordinatorPort::processInstance().fail(CoordinatorFailureOrigin::LocalInvariant,
                                                "authenticated lobby connection ended");
}

void receiveLobbyControl(CNetCustomService* service, const std::uint8_t* bytes, std::size_t size) {
    // The caller has already authenticated the packet's RakNet GUID, not just its payload.
    if (owner != service || !room) return;
    lobby::Envelope envelope;
    if (!lobby::decode(bytes, size, envelope)) {
        rejectActive(lobby::AbortReason::Protocol, "malformed lobby control envelope"); return;
    }
    if (envelope.room != *room) return;
    if (envelope.operation == lobby::Operation::Arm) {
        if (binding || envelope.epoch <= highWaterEpoch || !available()
            || !service->roomRequiresSimultaneousTurns()) {
            acknowledge(service, envelope, false); return;
        }
        highWaterEpoch = envelope.epoch;
        const auto role = static_cast<Role>(envelope.role);
        const auto* session = service->getSession();
        if (!session || session->isHost() != (role == Role::Host)) {
            acknowledge(service, envelope, false); return;
        }
        auto selected = std::make_shared<Binding>();
        selected->service = service; selected->room = envelope.room;
        selected->epoch = envelope.epoch; selected->role = role;
        {
            std::lock_guard selectionLock(selectionMutex);
            binding = selected;
        }
        const std::weak_ptr<Binding> weak = selected;
        auto sender = [weak](const protocol::Bytes& frame) {
            const auto current = weak.lock();
            if (!current) return false;
            std::lock_guard lock(current->mutex);
            if (!current->sending || !current->service) return false;
            lobby::Envelope value;
            value.operation = lobby::Operation::Frame; value.room = current->room;
            value.epoch = current->epoch; value.frame = frame;
            return send(current->service, value);
        };
        auto terminal = [weak]() {
            const auto current = weak.lock();
            if (!current) return;
            {
                std::lock_guard lock(current->mutex);
                if (!current->sending || !current->service) return;
                lobby::Envelope value;
                value.operation = lobby::Operation::Abort; value.room = current->room;
                value.epoch = current->epoch; value.reason = lobby::AbortReason::LocalFailure;
                send(current->service, value);
                current->sending = false;
            }
            reportFailure(current);
        };
        const SimTurnsSessionOptions options{true, role};
        auto progress = [weak]() {
            // Both progress sites are UI-thread observations. An idle map creates
            // no dispatcher work; only a pending causal barrier needs a wakeup.
            if (const auto current = weak.lock(); current
                && (!current->deliveries.empty() || !current->awaitingClientDrain.empty()))
                scheduleProgress(current);
        };
        auto& port = CoordinatorPort::processInstance();
        auto faultDiagnostic = [weak](const CoordinatorFaultDiagnostic& fault) {
            const auto current = weak.lock();
            if (!current) return;
            diagnostic(spdlog::level::err,
                       "[simturns-diag] port_fault pid={} room={} epoch={} role={} generation={} started={} detail={}",
                       GetCurrentProcessId(), current->room, fault.epoch, roleName(fault.role),
                       fault.generation, fault.started, fault.message);
        };
        const bool portArmed = port.arm(options, envelope.epoch, envelope.mergeDay,
                                       std::move(sender), std::move(terminal), std::move(progress),
                                       std::move(faultDiagnostic));
        const bool armed = portArmed && beginSession(role);
        diagnostic(spdlog::level::info,
                   "[simturns-diag] arm pid={} room={} epoch={} role={} merge_day={} port_armed={} native_armed={}",
                   GetCurrentProcessId(), selected->room, selected->epoch, roleName(role),
                   envelope.mergeDay, portArmed, armed);
        if (armed) {
            std::lock_guard lock(selected->mutex);
            selected->nativeReady = true;
        }
        if (!armed) {
            // beginSession(false) has acquired no native session. Retire only the
            // just-created, unstarted arm; never stop an existing active epoch.
            stopSending(selected);
            if (portArmed) port.stop();
            {
                std::lock_guard selectionLock(selectionMutex);
                binding.reset();
            }
            service->enqueueSystemNotice("Cannot arm simultaneous turns for this map.");
        }
        acknowledge(service, envelope, armed);
        return;
    }
    if (!binding || envelope.epoch < binding->epoch) return; // Old map's tail cannot touch the new map.
    if (envelope.epoch != binding->epoch) {
        rejectActive(lobby::AbortReason::Protocol, "unexpected lobby map epoch"); return;
    }
    {
        std::lock_guard lock(binding->mutex);
        if (!binding->sending) return;
    }
    if (envelope.operation == lobby::Operation::Frame) {
        if (binding->pendingFrames >= 64) {
            rejectActive(lobby::AbortReason::Protocol, "native/control barrier queue exhausted"); return;
        }
        binding->deliveries.push_back({binding->nativeApplied.watermark(), std::move(envelope.frame), {}});
        ++binding->pendingFrames;
        scheduleProgress(binding);
    } else if (envelope.operation == lobby::Operation::Abort) {
        if (stopSending(binding)) {
            diagnostic(spdlog::level::err,
                       "[simturns-diag] remote_abort pid={} room={} epoch={} role={} reason={}",
                       GetCurrentProcessId(), binding->room, binding->epoch, roleName(binding->role),
                       static_cast<unsigned>(envelope.reason));
            reportFailure(binding);
            CoordinatorPort::processInstance().fail(CoordinatorFailureOrigin::LocalInvariant,
                                                    "lobby coordinator aborted the map");
        }
    } else {
        rejectActive(lobby::AbortReason::Protocol, "client-only lobby control operation received");
    }
}

void lobbySessionCreated(CNetCustomService* service, bool host) {
    if (binding && owner == service && host != (binding->role == Role::Host))
        rejectActive(lobby::AbortReason::RoleMismatch, "native session role disagrees with lobby");
}

void lobbyMapTeardownBegun() { stopSending(binding); }

void lobbyMapDestroyed() {
    // Existing native clear hooks call this only after the server worker has joined.
    // Retain room and the high-water epoch: 111 replaces the map, not the lobby room.
    std::lock_guard selectionLock(selectionMutex);
    binding.reset();
}

} // namespace hooks::simturns
