/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 */

#include "simturns/spell_timing.h"

#include "gameutils.h"
#include "midgardid.h"
#include "netmsgmapentry.h"
#include "scenarioinfo.h"
#include "simturns/day_scope.h"
#include "simturns/state.h"
#include "simturns/turn_context.h"

#include <cstddef>
#include <cstdint>

namespace hooks::simturns::spell_timing {

namespace {

constexpr auto correctedTurnStopProof = rebaseTurnStop(8, 7, 3);
static_assert(correctedTurnStopProof.valid && correctedTurnStopProof.changed
                  && correctedTurnStopProof.value == 4,
              "TURN_STOP must preserve duration while changing its day base");
constexpr auto unchangedTurnStopProof = rebaseTurnStop(5, 3, 3);
static_assert(unchangedTurnStopProof.valid && !unchangedTurnStopProof.changed
                  && unchangedTurnStopProof.value == 5,
              "TURN_STOP already on the owner's day base must stay unchanged");
static_assert(!rebaseTurnStop(2, 3, 1).valid,
              "TURN_STOP before its stamped day is not a valid duration");
static_assert(!rebaseTurnStop(66, 1, 1).valid,
              "TURN_STOP duration beyond the proven bound must be rejected");
static_assert(!rebaseTurnStop(2, 1, std::numeric_limits<std::uint32_t>::max()).valid,
              "TURN_STOP owner-day overflow must be rejected");

using RunCallback = game::CNetMsgMapEntry_memberVftable::RunCallback;

struct CmdCastSpellLayout
{
    std::uint8_t unknown[0x10];
    game::CMidgardID casterId;
};

static_assert(offsetof(CmdCastSpellLayout, casterId) == 0x10,
              "CCmdCastSpellMsg caster id must be at +0x10");

RunCallback g_cmdCastSpellRunCallbackOriginal{};

enum class CasterTimingPolicy : std::uint8_t
{
    Invalid,
    Natural,
    Granted,
};

constexpr CasterTimingPolicy casterTimingPolicy(std::uint32_t caster,
                                                std::uint32_t host,
                                                std::uint32_t join) noexcept
{
    if (caster && (caster == host || caster == join))
        return CasterTimingPolicy::Granted;
    if (!caster)
        return CasterTimingPolicy::Natural;

    constexpr auto typeMask = 0x3fu;
    constexpr auto typeShift = 16u;
    const auto type = (caster >> typeShift) & typeMask;
    return type == static_cast<std::uint32_t>(game::IdType::Player)
               ? CasterTimingPolicy::Natural
               : CasterTimingPolicy::Invalid;
}

constexpr std::uint32_t policyHost = 0x805e0001u;
constexpr std::uint32_t policyJoin = 0x805e0002u;
static_assert(casterTimingPolicy(policyHost, policyHost, policyJoin)
                  == CasterTimingPolicy::Granted,
              "negotiated host spells require relay-granted timing");
static_assert(casterTimingPolicy(0, policyHost, policyJoin) == CasterTimingPolicy::Natural,
              "the engine no-owner sentinel must retain natural timing");
static_assert(casterTimingPolicy(0x805e0003u, policyHost, policyJoin)
                  == CasterTimingPolicy::Natural,
              "an untracked AI player must retain natural timing");
static_assert(casterTimingPolicy(0x805f0003u, policyHost, policyJoin)
                  == CasterTimingPolicy::Invalid,
              "a non-player caster must not be mistaken for an AI player");

struct RunCallbackCall
{
    RunCallback original;
    game::CNetMsgMapEntry_member* mapEntry;
    game::CNetMsg* message;
    std::uint32_t idFrom;
    std::uint32_t playerNetId;
};

std::uintptr_t invokeRunCallback(void* context, game::CScenarioInfo*)
{
    auto* call = static_cast<RunCallbackCall*>(context);
    return call->original(call->mapEntry, call->message, call->idFrom, call->playerNetId) ? 1u : 0u;
}

bool runForCasterGrant(RunCallbackCall& call,
                       std::uint32_t casterHandle,
                       const char* missingCasterFault)
{
    if (isFaulted() || phase() == Phase::Closing)
        return false;
    if (!isHost() || !activePremerge())
        return invokeRunCallback(&call, nullptr) != 0;

    // The grant is relay authority; the temporary engine day below is only
    // the execution context required by the stock spell callback.
    TurnGrant grant;
    if (!resolveTurnGrant(casterHandle, grant)) {
        fault(missingCasterFault);
        return false;
    }

    auto* objectMap = const_cast<game::IMidgardObjectMap*>(hooks::getServerObjectMap());
    return runSerializedCurrentTurn(objectMap, &grant.day, invokeRunCallback, &call) != 0;
}

bool __fastcall cmdCastSpellRunCallbackHooked(game::CNetMsgMapEntry_member* mapEntry,
                                              int,
                                              game::CNetMsg* message,
                                              std::uint32_t idFrom,
                                              std::uint32_t playerNetId)
{
    RunCallbackCall call{g_cmdCastSpellRunCallbackOriginal, mapEntry, message, idFrom, playerNetId};
    if (isFaulted() || phase() == Phase::Closing)
        return false;
    if (!isHost() || !activePremerge())
        return invokeRunCallback(&call, nullptr) != 0;
    const auto* layout = reinterpret_cast<const CmdCastSpellLayout*>(message);
    const bool expectedEntry = mapEntry
                               && reinterpret_cast<std::uintptr_t>(mapEntry->vftable)
                                      == russobit::cmdCastSpellMapEntryVftable;
    if (activePremerge() && (!expectedEntry || !layout)) {
        fault("invalid CCmdCastSpellMsg layout during simultaneous turns");
        return false;
    }

    const auto casterHandle = layout ? static_cast<std::uint32_t>(layout->casterId.value) : 0;
    switch (casterTimingPolicy(casterHandle, hostHandle(), joinHandle())) {
    case CasterTimingPolicy::Granted:
        return runForCasterGrant(
            call, casterHandle,
            "could not resolve relay-issued CCmdCastSpellMsg turn context");
    case CasterTimingPolicy::Natural:
        return invokeRunCallback(&call, nullptr) != 0;
    case CasterTimingPolicy::Invalid:
        fault("invalid CCmdCastSpellMsg caster during simultaneous turns");
        return false;
    }
    return false;
}

using AddSpellEffect = bool(__thiscall*)(void* thisptr,
                                         int opaque,
                                         const game::CMidgardID* unitId,
                                         const game::CMidgardID* originId,
                                         const game::CMidgardID* modifierId,
                                         const game::CMidgardID* casterId,
                                         const std::uint32_t* turnStop);

AddSpellEffect g_addSpellEffectOriginal{};

struct AddSpellEffectCall
{
    AddSpellEffect original;
    void* thisptr;
    int opaque;
    const game::CMidgardID* unitId;
    const game::CMidgardID* originId;
    const game::CMidgardID* modifierId;
    const game::CMidgardID* casterId;
    const std::uint32_t* turnStop;
    std::uint32_t ownerDay;
};

std::uintptr_t invokeAddSpellEffect(void* context, game::CScenarioInfo* scenarioInfo)
{
    auto* call = static_cast<AddSpellEffectCall*>(context);
    if (isFaulted() || phase() == Phase::Closing)
        return 0;

    const std::uint32_t* turnStop = call->turnStop;
    std::uint32_t corrected = turnStop ? *turnStop : 0;

    if (activePremerge() && scenarioInfo && turnStop) {
        const auto result = rebaseTurnStop(*turnStop,
                                           static_cast<std::uint32_t>(scenarioInfo->currentTurn),
                                           call->ownerDay);
        if (result.valid) {
            corrected = result.value;
            turnStop = &corrected;
        } else {
            fault("invalid TURN_STOP base or duration during simultaneous turns");
            return 0;
        }
    }

    return call->original(call->thisptr, call->opaque, call->unitId, call->originId,
                          call->modifierId, call->casterId, turnStop)
               ? 1u
               : 0u;
}

bool __fastcall addSpellEffectHooked(void* thisptr,
                                     int,
                                     int opaque,
                                     const game::CMidgardID* unitId,
                                     const game::CMidgardID* originId,
                                     const game::CMidgardID* modifierId,
                                     const game::CMidgardID* casterId,
                                     const std::uint32_t* turnStop)
{
    AddSpellEffectCall call{g_addSpellEffectOriginal,
                            thisptr,
                            opaque,
                            unitId,
                            originId,
                            modifierId,
                            casterId,
                            turnStop,
                            0};
    if (isFaulted() || phase() == Phase::Closing)
        return false;
    if (!isHost() || !activePremerge())
        return invokeAddSpellEffect(&call, nullptr) != 0;
    if (!casterId || !turnStop) {
        fault("missing spell-effect owner or TURN_STOP during simultaneous turns");
        return false;
    }

    const auto casterHandle = static_cast<std::uint32_t>(casterId->value);
    switch (casterTimingPolicy(casterHandle, hostHandle(), joinHandle())) {
    case CasterTimingPolicy::Natural:
        return invokeAddSpellEffect(&call, nullptr) != 0;
    case CasterTimingPolicy::Invalid:
        fault("invalid spell-effect caster during simultaneous turns");
        return false;
    case CasterTimingPolicy::Granted:
        break;
    }

    // TURN_STOP is rebased against the server-issued owner's day. MSS does not
    // infer that day from the engine calendar or a preceding local action.
    TurnGrant grant;
    if (!resolveTurnGrant(casterHandle, grant)) {
        fault("missing relay-issued spell-effect owner turn context");
        return false;
    }
    call.ownerDay = grant.day;

    auto* objectMap = const_cast<game::IMidgardObjectMap*>(hooks::getServerObjectMap());
    return runSerializedCurrentTurn(objectMap, nullptr, invokeAddSpellEffect, &call) != 0;
}

} // namespace

bool preflight(std::string& error)
{
    error.clear();
    if (!russobit::expectBytes(russobit::cmdCastSpellRunCallback,
                                  russobit::cmdCastSpellRunCallbackBytes,
                                  "CCmdCastSpellMsg runCallback", error)
        || !russobit::expectBytes(russobit::cmdCastSpellMapEntryVftable,
                                  russobit::cmdCastSpellMapEntryVftableBytes,
                                  "CCmdCastSpellMsg map-entry vftable", error)
        || !russobit::expectBytes(russobit::addSpellEffect, russobit::addSpellEffectPrefix,
                                  "CMidSpellEffects add", error)
        || !russobit::expectBytes(russobit::turnStopCopyCall, russobit::turnStopCopyCallBytes,
                                  "TURN_STOP copy call", error)
        || !russobit::expectBytes(russobit::copyDword, russobit::copyDwordBytes,
                                  "TURN_STOP copy target", error)) {
        return false;
    }

    if (russobit::decodeRelativeCallTarget(russobit::turnStopCopyCall) != russobit::copyDword) {
        error = "simultaneous-turn Russobit preflight decoded the wrong TURN_STOP copy target";
        return false;
    }
    return true;
}

void appendDetours(DetourTargets& targets)
{
    targets.push_back({reinterpret_cast<void*>(russobit::cmdCastSpellRunCallback),
                       reinterpret_cast<void*>(&cmdCastSpellRunCallbackHooked),
                       reinterpret_cast<void**>(&g_cmdCastSpellRunCallbackOriginal)});
    targets.push_back({reinterpret_cast<void*>(russobit::addSpellEffect),
                       reinterpret_cast<void*>(&addSpellEffectHooked),
                       reinterpret_cast<void**>(&g_addSpellEffectOriginal)});
}

} // namespace hooks::simturns::spell_timing
