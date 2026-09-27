/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "simturns/day_scope.h"

#include "eventeffect.h"
#include "executablefingerprint.h"
#include "gameutils.h"
#include "idvector.h"
#include "midgardid.h"
#include "midmsgsender.h"
#include "netintercept.h"
#include "scenarioinfo.h"
#include "simturns/battle_compat.h"
#include "simturns/spell_timing.h"
#include "simturns/state.h"
#include "simturns/turn_context.h"

#include <cassert>
#include <cstddef>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks::simturns {

namespace {

INIT_ONCE g_currentTurnLockOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_currentTurnLock;

BOOL CALLBACK initializeCurrentTurnLock(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_currentTurnLock);
    return TRUE;
}

bool ensureCurrentTurnLock()
{
    return InitOnceExecuteOnce(&g_currentTurnLockOnce, initializeCurrentTurnLock, nullptr, nullptr)
           != FALSE;
}

struct PlayerDayEffectLayout
{
    game::IEventEffectVftable* vftable;
    game::CMidgardID playerId;
    game::CMidgardID targetId;
    bool ignoreTurnAndCost;
    char padding[3];
};

static_assert(sizeof(PlayerDayEffectLayout) == 16,
              "player-scoped event effect layout must be 16 bytes");
static_assert(offsetof(PlayerDayEffectLayout, playerId) == 4,
              "player-scoped event effect player id must be at +4");
static_assert(offsetof(PlayerDayEffectLayout, targetId) == 8,
              "player-scoped event effect target id must be at +8");
static_assert(offsetof(PlayerDayEffectLayout, ignoreTurnAndCost) == 12,
              "player-scoped event effect flag must be at +12");

using EffectApply = game::IEventEffectVftable::Apply;

EffectApply g_capitalBuildApplyOriginal{};
EffectApply g_spellResearchApplyOriginal{};

enum class PlayerDayPolicy : std::uint8_t
{
    Invalid,
    Natural,
    Granted,
};

constexpr PlayerDayPolicy playerDayPolicy(std::uint32_t player,
                                          std::uint32_t host,
                                          std::uint32_t join) noexcept
{
    if (player && (player == host || player == join))
        return PlayerDayPolicy::Granted;
    constexpr auto typeMask = 0x3fu;
    constexpr auto typeShift = 16u;
    const auto type = (player >> typeShift) & typeMask;
    return player && type == static_cast<std::uint32_t>(game::IdType::Player)
               ? PlayerDayPolicy::Natural
               : PlayerDayPolicy::Invalid;
}

constexpr std::uint32_t policyHost = 0x805e0001u;
constexpr std::uint32_t policyJoin = 0x805e0002u;
static_assert(playerDayPolicy(policyHost, policyHost, policyJoin)
                  == PlayerDayPolicy::Granted,
              "negotiated humans require relay-granted build/research timing");
static_assert(playerDayPolicy(0x805e0003u, policyHost, policyJoin)
                  == PlayerDayPolicy::Natural,
              "an untracked AI player must retain natural build/research timing");
static_assert(playerDayPolicy(0, policyHost, policyJoin) == PlayerDayPolicy::Invalid,
              "an empty effect owner is not a valid AI player");

struct EffectApplyCall
{
    EffectApply original;
    const game::IEventEffect* effect;
    game::IMidgardObjectMap* objectMap;
    game::IMidMsgSender* messageSender;
    game::IdVector* triggerers;
};

std::uintptr_t invokeEffectApply(void* context, game::CScenarioInfo*)
{
    auto* call = static_cast<EffectApplyCall*>(context);
    return call->original(call->effect, call->objectMap, call->messageSender, call->triggerers)
               ? 1u
               : 0u;
}

bool applyForGrantedDay(EffectApply original,
                        const game::IEventEffect* effect,
                        game::IMidgardObjectMap* objectMap,
                        game::IMidMsgSender* messageSender,
                        game::IdVector* triggerers,
                        std::uintptr_t expectedVftable,
                        const char* missingGrantFault)
{
    EffectApplyCall call{original, effect, objectMap, messageSender, triggerers};
    if (isFaulted() || phase() == Phase::Closing)
        return false;
    if (!isHost() || !activePremerge())
        return invokeEffectApply(&call, nullptr) != 0;

    const auto* layout = reinterpret_cast<const PlayerDayEffectLayout*>(effect);
    if (!layout || reinterpret_cast<std::uintptr_t>(layout->vftable) != expectedVftable) {
        fault("simultaneous-turn player-day effect layout mismatch");
        return false;
    }

    const auto playerHandle = static_cast<std::uint32_t>(layout->playerId.value);
    switch (playerDayPolicy(playerHandle, hostHandle(), joinHandle())) {
    case PlayerDayPolicy::Natural:
        return invokeEffectApply(&call, nullptr) != 0;
    case PlayerDayPolicy::Invalid:
        fault("invalid build/research effect owner during simultaneous turns");
        return false;
    case PlayerDayPolicy::Granted:
        break;
    }

    // The relay chooses the grant. This hook only selects the exact execution
    // context in which the stock effect observes CScenarioInfo::currentTurn.
    TurnGrant grant;
    if (!resolveTurnGrant(playerHandle, grant)) {
        fault(missingGrantFault);
        return false;
    }

    return runSerializedCurrentTurn(objectMap, &grant.day, invokeEffectApply, &call) != 0;
}

bool __fastcall capitalBuildApplyHooked(const game::IEventEffect* effect,
                                        int,
                                        game::IMidgardObjectMap* objectMap,
                                        game::IMidMsgSender* messageSender,
                                        game::IdVector* triggerers)
{
    return applyForGrantedDay(g_capitalBuildApplyOriginal, effect, objectMap, messageSender,
                              triggerers, russobit::capitalBuildVftable,
                              "missing relay-issued turn context for capital build");
}

bool __fastcall spellResearchApplyHooked(const game::IEventEffect* effect,
                                         int,
                                         game::IMidgardObjectMap* objectMap,
                                         game::IMidMsgSender* messageSender,
                                         game::IdVector* triggerers)
{
    return applyForGrantedDay(g_spellResearchApplyOriginal, effect, objectMap, messageSender,
                              triggerers, russobit::spellResearchVftable,
                              "missing relay-issued turn context for spell research");
}

} // namespace

std::uintptr_t runSerializedCurrentTurn(game::IMidgardObjectMap* objectMap,
                                        const std::uint32_t* temporaryDay,
                                        SerializedCurrentTurnCallback callback,
                                        void* context)
{
    if (!callback)
        return 0;

    if (isFaulted() || phase() == Phase::Closing)
        return 0;

    // This lock only serializes our own temporary writes; it cannot stop
    // unrelated engine code from reading CScenarioInfo::currentTurn. Every
    // pre-merge scoped callback must therefore execute on the proven UI thread.
    if (activePremerge()) {
        const DWORD expectedUiThread = netintercept::mainThreadId();
        const DWORD currentThread = GetCurrentThreadId();
        const bool onUiThread = expectedUiThread != 0 && currentThread == expectedUiThread;
        assert(onUiThread && "simultaneous-turn day scope escaped the UI thread");
        if (!onUiThread) {
            spdlog::critical("[simturns] day scope rejected off UI thread (current={}, UI={})",
                             currentThread, expectedUiThread);
            fault("simultaneous-turn day scope executed off the proven UI thread");
            return 0;
        }
    }

    if (!objectMap) {
        fault("missing object map for simultaneous-turn day scope");
        return 0;
    }

    auto* scenarioInfo = const_cast<game::CScenarioInfo*>(hooks::getScenarioInfo(objectMap));
    if (!scenarioInfo) {
        fault("could not resolve CScenarioInfo for simultaneous-turn day scope");
        return 0;
    }
    if (!ensureCurrentTurnLock()) {
        fault("could not initialize simultaneous-turn currentTurn lock");
        return 0;
    }

    EnterCriticalSection(&g_currentTurnLock);
    if (isFaulted() || phase() == Phase::Closing) {
        LeaveCriticalSection(&g_currentTurnLock);
        return 0;
    }

    // temporaryDay is an execution-context projection only. This seam never
    // advances, validates ordering for, or otherwise owns the game calendar.
    const int savedDay = scenarioInfo->currentTurn;
    const bool overrideDay = temporaryDay && *temporaryDay && activePremerge();
    std::uintptr_t result = 0;
    __try {
        if (overrideDay)
            scenarioInfo->currentTurn = static_cast<int>(*temporaryDay);
        result = callback(context, scenarioInfo);
    } __finally {
        if (overrideDay)
            scenarioInfo->currentTurn = savedDay;
        LeaveCriticalSection(&g_currentTurnLock);
    }
    return result;
}

namespace day_scope {

bool preflight(std::string& error)
{
    error.clear();
    return russobit::expectBytes(russobit::capitalBuildApply, russobit::capitalBuildApplyBytes,
                                 "CEffectCapitalBuild::apply", error)
           && russobit::expectBytes(russobit::capitalBuildVftable,
                                    russobit::capitalBuildVftableBytes,
                                    "CEffectCapitalBuild vftable", error)
           && russobit::expectBytes(russobit::spellResearchApply, russobit::spellResearchApplyBytes,
                                    "CEffectSpellResearch::apply", error)
           && russobit::expectBytes(russobit::spellResearchVftable,
                                    russobit::spellResearchVftableBytes,
                                    "CEffectSpellResearch vftable", error);
}

void appendDetours(DetourTargets& targets)
{
    targets.push_back({reinterpret_cast<void*>(russobit::capitalBuildApply),
                       reinterpret_cast<void*>(&capitalBuildApplyHooked),
                       reinterpret_cast<void**>(&g_capitalBuildApplyOriginal)});
    targets.push_back({reinterpret_cast<void*>(russobit::spellResearchApply),
                       reinterpret_cast<void*>(&spellResearchApplyHooked),
                       reinterpret_cast<void**>(&g_spellResearchApplyOriginal)});
}

} // namespace day_scope

bool preflightProductionHookBundle(bool host, std::string& error)
{
    error.clear();
    if constexpr (sizeof(void*) != 4) {
        error = "simultaneous-turn engine hooks require a 32-bit process";
        return false;
    }
    if (!executablefingerprint::isExactRussobit()) {
        error = "simultaneous-turn engine hooks require the exact supported Russobit executable";
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) != russobit::imageBase) {
        error = "Russobit executable is not loaded at the proven image base 0x00400000";
        return false;
    }
    if (host && (!day_scope::preflight(error) || !spell_timing::preflight(error)))
        return false;
    return battle_compat::preflight(error);
}

bool appendProductionHookBundle(bool host, DetourTargets& targets, std::string& error)
{
    if (!preflightProductionHookBundle(host, error))
        return false;

    DetourTargets additions;
    additions.reserve(host ? 5u : 1u);
    if (host) {
        day_scope::appendDetours(additions);
        spell_timing::appendDetours(additions);
    }
    battle_compat::appendDetours(additions);
    targets.insert(targets.end(), additions.begin(), additions.end());
    return true;
}

} // namespace hooks::simturns
