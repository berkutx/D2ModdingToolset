/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * Relay-issued turn grants and short-lived engine action context.
 */

#ifndef SIMTURNS_TURN_CONTEXT_H
#define SIMTURNS_TURN_CONTEXT_H

#include "simturns/session_types.h"
#include <cstdint>

namespace hooks::simturns {

using TurnLease = std::uint32_t;

/**
 * An immutable authority snapshot issued by the lobby/relay. MSS never derives
 * the next day or lease from a previous value.
 */
struct TurnGrant
{
    std::uint32_t handle{};
    std::uint32_t day{};
    TurnLease lease{};
};

/**
 * Installs the two day-1 grants carried by the future SessionPlan message.
 * Repeating the exact same plan is harmless; replacing an installed plan is
 * rejected until resetTurnContext() starts a new process-local session.
 */
bool initializeTurnContextFromSessionPlan(const TurnGrant& host,
                                          const TurnGrant& join);

/** Clears both negotiated slots. This does not change the engine phase. */
void resetTurnContext();

/**
 * Publishes the relay's current grant for one negotiated handle. The supplied
 * value replaces the slot verbatim; this module performs no +1, merge-day, or
 * lease-order calculation.
 */
bool installTurnGrant(const TurnGrant& grant);

/**
 * Resolves the day used by a typed engine hook. An active engine-action scope
 * has strict precedence and must belong to the requested owner. With no scope,
 * the current relay-issued grant is used as a temporary compatibility path for
 * native DirectPlay packets that are not yet transported in a lobby envelope.
 */
bool resolveTurnGrant(std::uint32_t ownerHandle, TurnGrant& result);

/** Relay metadata attached to one concrete engine mutation. Turn-grant
 * actions carry their lease; merge actions deliberately use lease zero because
 * the lobby has closed both subjective turns before stock convergence. */
struct ScopedEngineActionContext
{
    std::uint32_t handle{};
    std::uint32_t day{};
    TurnLease lease{};
};

/**
 * Makes one action context visible to nested typed engine hooks on this thread.
 * Nested scopes are supported; the innermost context always wins.
 */
class EngineActionContextScope final
{
public:
    explicit EngineActionContextScope(const ScopedEngineActionContext& context) noexcept;
    ~EngineActionContextScope();

    EngineActionContextScope(const EngineActionContextScope&) = delete;
    EngineActionContextScope& operator=(const EngineActionContextScope&) = delete;
    EngineActionContextScope(EngineActionContextScope&&) = delete;
    EngineActionContextScope& operator=(EngineActionContextScope&&) = delete;

    bool valid() const noexcept;

private:
    ScopedEngineActionContext context;
    const ScopedEngineActionContext* previous{};
};

} // namespace hooks::simturns

#endif // SIMTURNS_TURN_CONTEXT_H
