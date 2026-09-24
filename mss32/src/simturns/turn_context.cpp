/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "simturns/turn_context.h"

#include <array>
#include <mutex>

namespace hooks::simturns {

namespace {

std::mutex g_grantsMutex;
std::array<TurnGrant, 2> g_grants{};
std::array<TurnGrant, 2> g_initialPlan{};
bool g_planInstalled{};
thread_local const ScopedEngineActionContext* g_engineActionContext = nullptr;

constexpr bool validGrant(const TurnGrant& grant) noexcept
{
    return grant.handle != 0 && grant.day != 0 && grant.day <= maxEngineDay
           && grant.lease != 0;
}

constexpr bool validContext(const ScopedEngineActionContext& context) noexcept
{
    // Merge convergence is an explicit engine action after all turn leases
    // have been retired. Its handle/day remain authoritative with lease zero.
    return context.handle != 0 && context.day != 0
           && context.day <= maxEngineDay;
}

constexpr bool sameGrant(const TurnGrant& left, const TurnGrant& right) noexcept
{
    return left.handle == right.handle && left.day == right.day
           && left.lease == right.lease;
}

TurnGrant* findGrant(std::uint32_t handle)
{
    for (auto& grant : g_grants) {
        if (grant.handle == handle)
            return &grant;
    }
    return nullptr;
}

} // namespace

bool initializeTurnContextFromSessionPlan(const TurnGrant& host,
                                          const TurnGrant& join)
{
    if (!validGrant(host) || !validGrant(join) || host.day != 1 || join.day != 1
        || host.handle == join.handle || host.lease == join.lease) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_grantsMutex);
    if (g_planInstalled) {
        return sameGrant(g_initialPlan[0], host)
               && sameGrant(g_initialPlan[1], join);
    }

    g_grants = {host, join};
    g_initialPlan = {host, join};
    g_planInstalled = true;
    return true;
}

void resetTurnContext()
{
    std::lock_guard<std::mutex> lock(g_grantsMutex);
    g_grants = {};
    g_initialPlan = {};
    g_planInstalled = false;
}

bool installTurnGrant(const TurnGrant& grant)
{
    if (!validGrant(grant))
        return false;

    std::lock_guard<std::mutex> lock(g_grantsMutex);
    if (!g_planInstalled)
        return false;
    TurnGrant* const current = findGrant(grant.handle);
    if (!current)
        return false;

    *current = grant;
    return true;
}

bool resolveTurnGrant(std::uint32_t ownerHandle, TurnGrant& result)
{
    result = {};
    if (!ownerHandle)
        return false;

    // A scoped packet/cascade context is the exact execution authority. Never
    // fall back to another player's grant when a nested hook exposes a
    // different owner: that would silently apply the action on the wrong day.
    if (g_engineActionContext) {
        if (!validContext(*g_engineActionContext)
            || g_engineActionContext->handle != ownerHandle) {
            return false;
        }
        result = TurnGrant{g_engineActionContext->handle,
                           g_engineActionContext->day,
                           g_engineActionContext->lease};
        return true;
    }

    std::lock_guard<std::mutex> lock(g_grantsMutex);
    if (!g_planInstalled)
        return false;
    TurnGrant* const grant = findGrant(ownerHandle);
    if (!grant || !validGrant(*grant))
        return false;
    result = *grant;
    return true;
}

EngineActionContextScope::EngineActionContextScope(
    const ScopedEngineActionContext& selected) noexcept
    : context(selected)
    , previous(g_engineActionContext)
{
    // Invalid metadata deliberately shadows the compatibility grant. A caller
    // must not turn malformed action metadata into an unrelated cached grant.
    g_engineActionContext = &context;
}

EngineActionContextScope::~EngineActionContextScope()
{
    g_engineActionContext = previous;
}

bool EngineActionContextScope::valid() const noexcept
{
    return validContext(context);
}

} // namespace hooks::simturns
