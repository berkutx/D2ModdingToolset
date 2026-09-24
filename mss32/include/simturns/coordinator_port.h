/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * Semantic boundary between the game-engine controller and the external
 * simultaneous-turn coordinator. Transport configuration, framing, threads,
 * and reconnect policy stay behind this port.
 */

#ifndef SIMTURNS_COORDINATOR_PORT_H
#define SIMTURNS_COORDINATOR_PORT_H

#include "simturns/protocol.h"
#include "simturns/session_types.h"
#include <cstdint>
#include <functional>
#include <string>

namespace hooks::simturns {

enum class CoordinatorEventKind : std::uint8_t
{
    SessionPlan,
    BootstrapCommitted,
    BootstrapOperational,
    BootstrapReleased,
    EngineAction,
};

/** One already decoded, ordered, and control-core-validated command. */
struct CoordinatorEvent
{
    CoordinatorEventKind kind{};
    protocol::SessionPlan sessionPlan;
    protocol::BootstrapProgress bootstrapCommitted;
    protocol::BootstrapProgress bootstrapOperational;
    protocol::BootstrapProgress bootstrapReleased;
    protocol::EngineAction engineAction;
};

struct CoordinatorTerminalFault
{
    std::string message;
};

struct CoordinatorCallbacks
{
    /**
     * May run on a transport worker. The receiver must only enqueue ordered UI
     * work and return promptly; it must never call the game engine directly.
     */
    std::function<void(CoordinatorEvent)> postToUi;

    /**
     * Delivered exactly once for a terminal transport, protocol, or local-port
     * fault. The receiver must close native gates before queuing diagnostics.
     */
    std::function<void(CoordinatorTerminalFault)> terminalFault;
};

enum class BootstrapCheckpoint : std::uint8_t
{
    BeginTurnApplied,
    Complete,
    CommitApplied,
    OperationalApplied,
};

enum class CoordinatorFailureOrigin : std::uint8_t
{
    UiApply,
    LocalInvariant,
};

/**
 * Session-scoped lobby adapter. Engine reports retain the v8 control-core
 * checks; the authenticated lobby owns room identity and delivery order.
 * No environment opt-in, pipe worker, reconnect, or stock fallback.
 */
class CoordinatorPort final
{
public:
    using Sender = std::function<bool(const protocol::Bytes&)>;
    static CoordinatorPort& processInstance();

    CoordinatorPort(const CoordinatorPort&) = delete;
    CoordinatorPort& operator=(const CoordinatorPort&) = delete;

    /** Arm from an authenticated lobby envelope before native map startup.
     * Sender synchronously enqueues reliable ordered data without reentering
     * this port. Terminal reports the epoch failure to the lobby once. */
    bool arm(const SimTurnsSessionOptions& session, std::uint32_t epoch,
             std::uint32_t mergeDay, Sender sender,
             std::function<void()> terminal = {},
             std::function<void()> progressWake = {});
    /** Natural native progress edge; adapters may wake an ordered-input barrier. */
    void notifyNativeProgress();
    /** Each envelope contains exactly one complete v8 frame. */
    void receive(const std::uint8_t* bytes, std::size_t size);
    /** Fence callbacks/sends before native teardown, without issuing a fault. */
    void quiesce();
    /** Native-worker-joined teardown only; never a recovery operation. */
    void stop();

    /** Validate the already armed runtime session, without transport I/O. */
    bool preflight(const SimTurnsSessionOptions& session, std::string& error);

    /** Bind this map's callbacks once, before publishing its local player. */
    bool start(const SimTurnsSessionOptions& session, CoordinatorCallbacks callbacks);
    bool bindLocalPlayer(std::uint32_t handle);

    /** True only while the validated control core is in its active turn phase. */
    bool operational() const;
    bool endTurnPending() const;
    bool claimEndTurn(std::uint32_t& lease);

    bool reportSessionActivated();
    bool reportBootstrap(BootstrapCheckpoint checkpoint,
                         std::uint32_t handle,
                         std::uint32_t day);
    bool reportEndTurnObserved(std::uint32_t lease);
    bool reportEndTurnApplied(std::uint32_t originHandle);
    bool reportActionResult(const protocol::EngineAction& action, bool success);
    bool reportMergeApplied(std::uint32_t actionId);

    /** Local validation/commit of coordinator releases which have no reply frame. */
    bool acceptBootstrapRelease(const protocol::BootstrapProgress& released);
    bool acceptStockRelease(const protocol::EngineAction& action);

    /** Thread-safe, terminal, and exact-once from the controller's perspective. */
    void fail(CoordinatorFailureOrigin origin, const char* reason);

private:
    CoordinatorPort();
    ~CoordinatorPort() = delete;

    struct Impl;
    Impl* impl;
};

} // namespace hooks::simturns

#endif // SIMTURNS_COORDINATOR_PORT_H
