/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "simturns/state.h"
#include <atomic>
#include <mutex>
#include <spdlog/spdlog.h>

namespace hooks::simturns {

namespace {

std::atomic<Phase> g_phase{Phase::Disabled};
std::atomic<Role> g_role{Role::Host};
std::atomic<std::uint32_t> g_configuredMergeDay{0};
std::atomic<std::uint32_t> g_hostHandle{0};
std::atomic<std::uint32_t> g_joinHandle{0};
std::atomic<std::uint32_t> g_localHandle{0};
std::mutex g_faultMutex;
std::string g_faultReason;
std::atomic<FaultObserver> g_faultObserver{nullptr};
std::atomic<bool> g_faultPublished{false};
std::atomic<bool> g_faultObserverNotified{false};

bool transition(Phase expected, Phase desired)
{
    return g_phase.compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
}

void notifyFaultObserverOnce(const std::string& reason)
{
    const FaultObserver observer = g_faultObserver.load(std::memory_order_acquire);
    if (!observer)
        return;
    bool expected = false;
    if (g_faultObserverNotified.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        observer(reason.c_str());
    }
}

} // namespace

void initializeState(Role selectedRole)
{
    g_role.store(selectedRole, std::memory_order_release);
    g_configuredMergeDay.store(0, std::memory_order_release);
    g_hostHandle.store(0, std::memory_order_release);
    g_joinHandle.store(0, std::memory_order_release);
    g_localHandle.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_faultMutex);
        g_faultReason.clear();
    }
    g_faultPublished.store(false, std::memory_order_release);
    g_faultObserverNotified.store(false, std::memory_order_release);
    g_phase.store(Phase::Prepared, std::memory_order_release);
}

bool setFaultObserver(FaultObserver observer)
{
    if (!observer)
        return false;
    FaultObserver expected = nullptr;
    if (!g_faultObserver.compare_exchange_strong(
            expected, observer, std::memory_order_acq_rel)
        && expected != observer) {
        return false;
    }
    if (g_faultPublished.load(std::memory_order_acquire))
        notifyFaultObserverOnce(faultReason());
    return true;
}

void markEngineInstalled()
{
    if (!transition(Phase::Prepared, Phase::WaitingForSession))
        fault("invalid engine-install state transition");
}

void closeStateForTeardown()
{
    if (phase() != Phase::Disabled)
        g_phase.store(Phase::Closing, std::memory_order_release);
}

void resetState()
{
    initializeState(Role::Host);
    g_phase.store(Phase::Disabled, std::memory_order_release);
}

bool publishLocalHandle(std::uint32_t handle)
{
    if (handle == 0)
        return false;
    std::uint32_t expected = 0;
    return g_localHandle.compare_exchange_strong(expected, handle, std::memory_order_acq_rel)
        || expected == handle;
}

bool authorizeStock()
{
    return transition(Phase::WaitingForSession, Phase::Stock)
        || g_phase.load(std::memory_order_acquire) == Phase::Stock;
}

bool establishSession(std::uint32_t host, std::uint32_t join, std::uint32_t mergeDay)
{
    if (host == 0 || join == 0 || host == join
        || (mergeDay != 0 && (mergeDay < 2 || mergeDay > maxEngineDay))) {
        return false;
    }

    const std::uint32_t local = g_localHandle.load(std::memory_order_acquire);
    const std::uint32_t expectedLocal = isHost() ? host : join;
    if (local == 0 || local != expectedLocal)
        return false;

    g_configuredMergeDay.store(mergeDay, std::memory_order_release);
    g_hostHandle.store(host, std::memory_order_release);
    g_joinHandle.store(join, std::memory_order_release);
    Phase expected = Phase::WaitingForSession;
    if (!g_phase.compare_exchange_strong(expected, Phase::Ready, std::memory_order_acq_rel)) {
        g_hostHandle.store(0, std::memory_order_release);
        g_joinHandle.store(0, std::memory_order_release);
        g_configuredMergeDay.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

bool activateIndependent()
{
    return transition(Phase::Ready, Phase::Independent)
        || g_phase.load(std::memory_order_acquire) == Phase::Independent;
}

bool enterHeld()
{
    return transition(Phase::Independent, Phase::Held)
        || g_phase.load(std::memory_order_acquire) == Phase::Held;
}

bool beginMerge()
{
    Phase current = g_phase.load(std::memory_order_acquire);
    while (current == Phase::Independent || current == Phase::Held) {
        if (g_phase.compare_exchange_weak(current, Phase::Merging, std::memory_order_acq_rel))
            return true;
    }
    return current == Phase::Merging;
}

bool finishMerge(std::uint32_t mergeDay)
{
    if (!mergeDay || mergeDay != configuredMergeDay() || !isHost()
        || g_phase.load(std::memory_order_acquire) != Phase::Merging) {
        return g_phase.load(std::memory_order_acquire) == Phase::Merged;
    }

    Phase expected = Phase::Merging;
    return g_phase.compare_exchange_strong(expected, Phase::Merged,
                                           std::memory_order_acq_rel)
        || expected == Phase::Merged;
}

bool awaitStockTurn(std::uint32_t mergeDay)
{
    if (isHost())
        return false;
    if (!mergeDay || mergeDay != configuredMergeDay()
        || g_phase.load(std::memory_order_acquire) != Phase::Merging) {
        return g_phase.load(std::memory_order_acquire) == Phase::AwaitingStockTurn;
    }

    // The joiner has restored the stock UI/dispatch bytes, but it must remain
    // non-active until native DPlay delivers the host's merge-day BeginTurn.
    Phase expected = Phase::Merging;
    return g_phase.compare_exchange_strong(expected, Phase::AwaitingStockTurn,
                                           std::memory_order_acq_rel)
        || expected == Phase::AwaitingStockTurn;
}

bool finishStockTurn()
{
    if (isHost())
        return false;
    Phase expected = Phase::AwaitingStockTurn;
    return g_phase.compare_exchange_strong(expected, Phase::Merged,
                                           std::memory_order_acq_rel)
        || expected == Phase::Merged;
}

Phase phase()
{
    return g_phase.load(std::memory_order_acquire);
}

Role role()
{
    return g_role.load(std::memory_order_acquire);
}

bool isHost()
{
    return role() == Role::Host;
}

bool activePremerge()
{
    const Phase current = phase();
    return current == Phase::Independent || current == Phase::Held;
}

bool ownsVirtualTurn()
{
    const Phase current = phase();
    return current == Phase::Independent || current == Phase::Held
        || current == Phase::Merging || current == Phase::Closing
        || current == Phase::Faulted;
}

bool isAwaitingStockTurn()
{
    return phase() == Phase::AwaitingStockTurn;
}

bool isMerged()
{
    return phase() == Phase::Merged;
}

bool isFaulted()
{
    return phase() == Phase::Faulted;
}

std::uint32_t configuredMergeDay()
{
    return g_configuredMergeDay.load(std::memory_order_acquire);
}

std::uint32_t hostHandle()
{
    return g_hostHandle.load(std::memory_order_acquire);
}

std::uint32_t joinHandle()
{
    return g_joinHandle.load(std::memory_order_acquire);
}

std::uint32_t localHandle()
{
    return g_localHandle.load(std::memory_order_acquire);
}

std::uint32_t otherHandle()
{
    return isHost() ? joinHandle() : hostHandle();
}

void fault(const char* reason)
{
    Phase current = g_phase.load(std::memory_order_acquire);
    // Even after the final release, the relay remains the process-lifetime
    // safety authority. Any local invariant failure therefore closes both the
    // game gates and the control connection; there is no stock fallback.
    while (current != Phase::Disabled && current != Phase::Closing
           && current != Phase::Faulted) {
        if (g_phase.compare_exchange_weak(current, Phase::Faulted, std::memory_order_acq_rel)) {
            const char* message = reason && reason[0] ? reason : "unspecified simultaneous-turn fault";
            std::string published;
            {
                std::lock_guard<std::mutex> lock(g_faultMutex);
                g_faultReason = message;
                published = g_faultReason;
            }
            g_faultPublished.store(true, std::memory_order_release);
            spdlog::critical("[simturns] terminal fault: {}", message);
            notifyFaultObserverOnce(published);
            return;
        }
    }
}

std::string faultReason()
{
    std::lock_guard<std::mutex> lock(g_faultMutex);
    return g_faultReason;
}

} // namespace hooks::simturns
