/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2021 Stanislav Egorov.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "battlemsgdatahooks.h"
#ifdef D2_TESTDRV
#include "testdrv/battletrace.h"
#endif
#include "attackview.h"
#include "batattack.h"
#include "bindings/battlemsgdataviewmutable.h"
#include "bindings/groupview.h"
#include "bindings/idview.h"
#include "bindings/unitview.h"
#include "customaibattle.h"
#include "customattacks.h"
#include "fortification.h"
#include "gameutils.h"
#include "intset.h"
#include "midplayer.h"
#include "midstack.h"
#include "modifierutils.h"
#include "originalfunctions.h"
#include "racetype.h"
#include "scripts.h"
#include "settings.h"
#include "unitimplview.h"
#include "unitutils.h"
#include "usunitimpl.h"
#include "utils.h"
#include "hooks.h"
#include "midgardid.h"
#include <atomic>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <batattackutils.h>

namespace {

thread_local int g_PreTurnHookDepth{};
std::unordered_set<std::uint64_t> g_FiredBattleKeys;

static std::uint64_t makeBattlePairKey(const game::BattleMsgData* msg)
{
    const auto attacker = static_cast<std::uint32_t>(msg->attackerGroupId.value);
    const auto defender = static_cast<std::uint32_t>(msg->defenderGroupId.value);
    return (static_cast<std::uint64_t>(attacker) << 32) | defender;
}


} // namespace

namespace hooks {

void tryFirePreTurnHookOnce(game::IMidgardObjectMap* objectMap,
                            game::BattleMsgData* battleMsgData,
                            const game::CMidgardID* unitId)
{
    using namespace game;

    if (g_PreTurnHookDepth || !objectMap || !battleMsgData || !unitId)
        return;
    if (*unitId == emptyId || *unitId == invalidId)
        return;

    const CMidgardID* firstId = nullptr;
    for (const auto& turn : battleMsgData->turnsOrder) {
        if (turn.unitId != emptyId && turn.unitId != invalidId) {
            firstId = &turn.unitId;
            break;
        }
    }
    if (!firstId || *firstId != *unitId)
        return;
    if (!g_FiredBattleKeys.insert(makeBattlePairKey(battleMsgData)).second)
        return;

    ++g_PreTurnHookDepth;
    beforeBattleTurnHooked(battleMsgData, objectMap, unitId);
    --g_PreTurnHookDepth;
}

void resetPreTurnHookState()
{
    g_FiredBattleKeys.clear();
}

bool getFiredBattleKeys()
{
    return g_FiredBattleKeys.empty() ? true : false;
}


class ModifiedUnitsPatchedFactory
{
public:
    ModifiedUnitsPatchedFactory()
        : count(0)
    { }

    ~ModifiedUnitsPatchedFactory()
    {
        if (count != 0) {
            spdlog::error("{:d} instances of ModifiedUnitsPatched remained on finalization", count.load());
        }
    }

    game::ModifiedUnitInfo* create()
    {
        count++;
        return new game::ModifiedUnitInfo[game::ModifiedUnitCountPatched];
    }

    void destroy(game::ModifiedUnitInfo* value)
    {
        count--;
        delete[] value;
    }

private:
    std::atomic<int> count;
} modifiedUnitsPatchedFactory;

void resetUnitInfo(game::UnitInfo* unitInfo)
{
    using namespace game;

    auto modifiedUnits = unitInfo->modifiedUnits;
    memset(unitInfo, 0, sizeof(UnitInfo));

    unitInfo->unitId1 = invalidId;

    unitInfo->modifiedUnits = modifiedUnits;
    resetModifiedUnitsInfo(unitInfo);

    for (auto& modifierId : unitInfo->modifierIds) {
        modifierId = invalidId;
    }
}

game::BattleMsgData* __fastcall battleMsgDataCtorHooked(game::BattleMsgData* thisptr, int /*%edx*/)
{
    using namespace game;

    getOriginalFunctions().battleMsgDataCtor(thisptr);

    for (auto& unitInfo : thisptr->unitsInfo) {
        memset(&unitInfo.modifiedUnits, 0, sizeof(ModifiedUnitsPatched));
        unitInfo.modifiedUnits.patched = modifiedUnitsPatchedFactory.create();
        resetModifiedUnitsInfo(&unitInfo);
    }

    return thisptr;
}

game::BattleMsgData* __fastcall battleMsgDataCopyCtorHooked(game::BattleMsgData* thisptr,
                                                            int /*%edx*/,
                                                            const game::BattleMsgData* src)
{
    using namespace game;

    getOriginalFunctions().battleMsgDataCtor(thisptr);

    for (auto& unitInfo : thisptr->unitsInfo) {
        memset(&unitInfo.modifiedUnits, 0, sizeof(ModifiedUnitsPatched));
    }

    return battleMsgDataCopyHooked(thisptr, 0, src);
}

game::BattleMsgData* __fastcall battleMsgDataCopyAssignHooked(game::BattleMsgData* thisptr,
                                                              int /*%edx*/,
                                                              const game::BattleMsgData* src)
{
    using namespace game;

    if (thisptr == src)
        return thisptr;

    const size_t count = std::size(thisptr->unitsInfo);
    std::vector<ModifiedUnitInfo*> prev(count);
    for (size_t i = 0; i < count; i++) {
        prev[i] = thisptr->unitsInfo[i].modifiedUnits.patched;
    }

    *thisptr = *src;

    for (size_t i = 0; i < count; i++) {
        memcpy(prev[i], src->unitsInfo[i].modifiedUnits.patched,
               sizeof(ModifiedUnitInfo) * ModifiedUnitCountPatched);
        thisptr->unitsInfo[i].modifiedUnits.patched = prev[i];
    }

    return thisptr;
}

game::BattleMsgData* __fastcall battleMsgDataCopyHooked(game::BattleMsgData* thisptr,
                                                        int /*%edx*/,
                                                        const game::BattleMsgData* src)
{
    using namespace game;

    *thisptr = *src;

    const size_t count = std::size(thisptr->unitsInfo);
    for (size_t i = 0; i < count; i++) {
        auto modifiedUnits = modifiedUnitsPatchedFactory.create();
        memcpy(modifiedUnits, src->unitsInfo[i].modifiedUnits.patched,
               sizeof(ModifiedUnitInfo) * ModifiedUnitCountPatched);
        thisptr->unitsInfo[i].modifiedUnits.patched = modifiedUnits;
    }

    return thisptr;
}

void __fastcall battleMsgDataDtorHooked(game::BattleMsgData* thisptr, int /*%edx*/)
{
    using namespace game;

    for (auto& unitInfo : thisptr->unitsInfo) {
        modifiedUnitsPatchedFactory.destroy(unitInfo.modifiedUnits.patched);
        unitInfo.modifiedUnits.patched = nullptr;
    }
}

void __fastcall removeUnitInfoHooked(game::BattleMsgData* thisptr,
                                     int /*%edx*/,
                                     const game::CMidgardID* unitId)
{
    using namespace game;

    const auto& battle = BattleMsgDataApi::get();

    size_t index;
    ModifiedUnitsPatched modifiedUnits{};
    const size_t count = std::size(thisptr->unitsInfo);
    for (index = 0; index < count; ++index) {
        if (thisptr->unitsInfo[index].unitId1 == *unitId) {
            modifiedUnits = thisptr->unitsInfo[index].modifiedUnits;
            break;
        }
    }

    for (size_t i = index; i + 1 < count; ++i) {
        memcpy(&thisptr->unitsInfo[i], &thisptr->unitsInfo[i + 1], sizeof(UnitInfo));
    }

    if (index < count) {
        auto lastInfo = &thisptr->unitsInfo[count - 1];
        lastInfo->modifiedUnits = modifiedUnits;
        resetUnitInfo(lastInfo);
    }

    while (battle.decreaseUnitAttacks(thisptr, unitId))
        ;
}

void updateDefendBattleAction(const game::UnitInfo* unitInfo,
                              game::Set<game::BattleAction>* actions)
{
    using namespace game;

    const auto& intSetApi = IntSetApi::get();

    if (unitInfo->unitFlags.parts.attackedOnceOfTwice && actions->length)
        return;

    BattleAction defend = BattleAction::Defend;
    Pair<IntSetIterator, bool> tmp{};
    intSetApi.insert((IntSet*)actions, &tmp, (int*)&defend);
}

void updateWaitBattleAction(const game::BattleMsgData* battleMsgData,
                            const game::UnitInfo* unitInfo,
                            game::Set<game::BattleAction>* actions)
{
    using namespace game;

    const auto& intSetApi = IntSetApi::get();

    if (battleMsgData->currentRound < 1)
        return;

    if (unitInfo->unitFlags.parts.waited || unitInfo->unitFlags.parts.attackedOnceOfTwice)
        return;

    BattleAction wait = BattleAction::Wait;
    Pair<IntSetIterator, bool> tmp{};
    intSetApi.insert((IntSet*)actions, &tmp, (int*)&wait);
}

void updateUseItemBattleAction(const game::IMidgardObjectMap* objectMap,
                               const game::BattleMsgData* battleMsgData,
                               const game::UnitInfo* unitInfo,
                               game::Set<game::BattleAction>* actions,
                               game::GroupIdTargetsPair* item1Targets,
                               game::GroupIdTargetsPair* item2Targets)
{
    using namespace game;

    const auto& idApi = CMidgardIDApi::get();
    const auto& battleApi = BattleMsgDataApi::get();
    const auto& intSetApi = IntSetApi::get();

    if (battleMsgData->currentRound < 1)
        return;

    auto groupId = unitInfo->unitFlags.parts.attacker ? &battleMsgData->attackerGroupId
                                                      : &battleMsgData->defenderGroupId;
    if (idApi.getType(groupId) != IdType::Stack)
        return;

    if (!isStackLeaderAndAllowedToUseBattleItems(objectMap, &unitInfo->unitId1, battleMsgData))
        return;

    auto stack = getStack(objectMap, groupId);

    GroupIdTargetsPair* itemTargets[] = {item1Targets, item2Targets};
    EquippedItemIdx itemIndices[] = {EquippedItemIdx::Battle1, EquippedItemIdx::Battle2};
    for (int i = 0; i < 2; ++i) {
        auto itemId = stack->leaderEquippedItems.bgn[itemIndices[i]];
        if (itemId == emptyId)
            continue;

        auto& inventory = stack->inventory;
        if (inventory.vftable->getItemIndex(&inventory, &itemId) == -1)
            continue;

        bool used = false;
        for (const auto& usedItemId : battleMsgData->usedItemIds) {
            if (usedItemId == itemId) {
                used = true;
                break;
            }
        }
        if (used)
            continue;

        battleApi.getItemAttackTargets(objectMap, battleMsgData, &unitInfo->unitId1, &itemId,
                                       itemTargets[i]);
    }

    for (const auto targets : itemTargets) {
        if (targets->second.length > 0) {
            BattleAction useItem = BattleAction::UseItem;
            Pair<IntSetIterator, bool> tmp{};
            intSetApi.insert((IntSet*)actions, &tmp, (int*)&useItem);
            break;
        }
    }
}

void updateRetreatBattleAction(const game::IMidgardObjectMap* objectMap,
                               const game::BattleMsgData* battleMsgData,
                               const game::UnitInfo* unitInfo,
                               game::Set<game::BattleAction>* actions)
{
    using namespace game;

    const auto& idApi = CMidgardIDApi::get();
    const auto& battleApi = BattleMsgDataApi::get();
    const auto& intSetApi = IntSetApi::get();

    if (battleMsgData->currentRound < 1)
        return;

    if (unitInfo->unitFlags.parts.attackedOnceOfTwice)
        return;

    auto groupId = unitInfo->unitFlags.parts.attacker ? &battleMsgData->attackerGroupId
                                                      : &battleMsgData->defenderGroupId;
    if (idApi.getType(groupId) != IdType::Stack)
        return;

    if (!unitInfo->unitFlags.parts.attacker) {
        auto stack = getStack(objectMap, groupId);
        if (stack && stack->insideId != emptyId)
            return;
    }

    BattleAction retreat = BattleAction::Retreat;
    Pair<IntSetIterator, bool> tmp{};
    intSetApi.insert((IntSet*)actions, &tmp, (int*)&retreat);
}

void updateAttackBattleAction(const game::IMidgardObjectMap* objectMap,
                              const game::BattleMsgData* battleMsgData,
                              const game::UnitInfo* unitInfo,
                              game::Set<game::BattleAction>* actions,
                              game::GroupIdTargetsPair* attackTargets)
{
    using namespace game;

    const auto& fn = gameFunctions();
    const auto& intSetApi = IntSetApi::get();

    const auto attack = fn.getAttackById(objectMap, &unitInfo->unitId1, 1, false);
    const auto attackClass = attack->vftable->getAttackClass(attack);
    const auto batAttack = fn.createBatAttack(objectMap, battleMsgData, &unitInfo->unitId1,
                                              &unitInfo->unitId1, 1, attackClass, false);
    batAttack->vftable->getTargetGroupId(batAttack, &attackTargets->first, battleMsgData);
    batAttack->vftable->fillTargetsList(batAttack, objectMap, battleMsgData,
                                        &attackTargets->second);
    batAttack->vftable->destructor(batAttack, true);

    if (attackTargets->second.length > 0) {
        BattleAction attack = BattleAction::Attack;
        Pair<IntSetIterator, bool> tmp{};
        intSetApi.insert((IntSet*)actions, &tmp, (int*)&attack);
    }
}

void __stdcall updateBattleActionsHooked(const game::IMidgardObjectMap* objectMap,
                                         const game::BattleMsgData* battleMsgData,
                                         const game::CMidgardID* unitId,
                                         game::Set<game::BattleAction>* actions,
                                         game::GroupIdTargetsPair* attackTargets,
                                         game::GroupIdTargetsPair* item1Targets,
                                         game::GroupIdTargetsPair* item2Targets)
{
    using namespace game;

    const auto& battleApi = BattleMsgDataApi::get();
    const auto& intSetApi = IntSetApi::get();

    intSetApi.clear((IntSet*)actions);

    auto unitInfo = battleApi.getUnitInfoById(battleMsgData, unitId);

    updateWaitBattleAction(battleMsgData, unitInfo, actions);
    updateUseItemBattleAction(objectMap, battleMsgData, unitInfo, actions, item1Targets,
                              item2Targets);
    updateRetreatBattleAction(objectMap, battleMsgData, unitInfo, actions);
    updateAttackBattleAction(objectMap, battleMsgData, unitInfo, actions, attackTargets);
    updateDefendBattleAction(unitInfo, actions);
}

void __fastcall beforeBattleRoundHooked(game::BattleMsgData* thisptr, int /*%edx*/)
{
    using namespace game;
#ifdef D2_TESTDRV
    testdrv::battletrace::Scope trace("round-before", thisptr);
#endif

    const auto& battleApi = BattleMsgDataApi::get();
    const auto& unitInfoApi = UnitInfoListApi::get();
    IMidgardObjectMap* objectMap = const_cast<IMidgardObjectMap*>(hooks::getObjectMap());
#ifdef D2_TESTDRV
    trace.setMap(objectMap);
#endif

    thisptr->currentRound++;

    UnitInfoList unitList;
    unitInfoApi.constructor(&unitList);
    battleApi.getUnitInfos(thisptr, &unitList, true);

    for (const auto& unitInfo : unitList) {
        UnitInfo* info = battleApi.getUnitInfoById(thisptr, &unitInfo.unitId1);
        if (info) {
            if (battleApi.getUnitStatus(thisptr, &info->unitId1, BattleStatus::Dead))
            {
                auto modifiedIds = getModifiedUnitIds(info);
                if (!modifiedIds.empty())
                {
                    for (auto& targetId : modifiedIds)
                    {
                        removeModifiers(thisptr, objectMap, info, &targetId);
                    }
                    resetModifiedUnitsInfo(info);
                }
            }
            info->unitFlags.parts.waited = false;
            info->unitFlags.parts.attackedOnceOfTwice = false;
        }
    }
    unitInfoApi.destructor(&unitList);

    auto& ftSelf = getCustomAttacks().freeTransformSelf;
    ftSelf.unitId = emptyId;
    ftSelf.turnCount = 0;
    ftSelf.used = false;

    std::optional<sol::environment> env;
    static auto onBeforeRound = getScriptFunction(scriptsFolder() / "hooks/hooks.lua",
                                                  "OnBeforeBattleRound", env, false, true);
    if (onBeforeRound && objectMap) {
        try {
            (*onBeforeRound)(bindings::BattleMsgDataView{thisptr, objectMap});
        } catch (const std::exception& e) {
            showErrorMessageBox(fmt::format("Lua Error (OnBeforeBattleRound): {}", e.what()));
        }
    }
}

void __stdcall aiChooseBattleActionHooked(const game::IMidgardObjectMap* objectMap,
                                          game::BattleMsgData* battleMsgData,
                                          const game::CMidgardID* unitId,
                                          const game::Set<game::BattleAction>* possibleActions,
                                          const game::PossibleTargets* possibleTargets,
                                          game::BattleAction* battleAction,
                                          game::CMidgardID* targetUnitId,
                                          game::CMidgardID* attackerUnitId)
{
    using namespace game;
#ifdef D2_TESTDRV
    testdrv::battletrace::Scope trace("ai-choice", battleMsgData, objectMap, unitId,
                                     battleAction, targetUnitId, attackerUnitId);
#endif

    const auto& chooseBattleAction{getOriginalFunctions().aiChooseBattleAction};

    const auto& customBattleLogic{getCustomAiBattleLogic()};
    if (!customBattleLogic.customBattleLogicEnabled) {
        // Custom battle action logic disabled, use original
        chooseBattleAction(objectMap, battleMsgData, unitId, possibleActions, possibleTargets,
                           battleAction, targetUnitId, attackerUnitId);
        return;
    }

    CMidgardID allyGroupId{};
    gameFunctions().getAllyOrEnemyGroupId(&allyGroupId, battleMsgData, unitId, true);

    const CMidPlayer* allyPlayer{getGroupOwner(objectMap, &allyGroupId)};
    if (!allyPlayer) {
        const auto message{fmt::format("Could not find player that owns unit {:s} in group {:s}",
                                       idToString(unitId), idToString(&allyGroupId))};

        if (gameSettings().battle.debugAi) {
            showErrorMessageBox(message);
        } else {
            spdlog::error(message);
        }

        chooseBattleAction(objectMap, battleMsgData, unitId, possibleActions, possibleTargets,
                           battleAction, targetUnitId, attackerUnitId);
        return;
    }

    const auto& battleLogicMap{customBattleLogic.attitudeBattleLogic};
    const auto it{battleLogicMap.find(allyPlayer->attitude.id)};
    if (it == battleLogicMap.cend()) {
        const auto message{fmt::format("Could not find battle action script by attitude {:d}",
                                       static_cast<int>(allyPlayer->attitude.id))};

        if (gameSettings().battle.debugAi) {
            showErrorMessageBox(message);
        } else {
            spdlog::error(message);
        }

        chooseBattleAction(objectMap, battleMsgData, unitId, possibleActions, possibleTargets,
                           battleAction, targetUnitId, attackerUnitId);
        return;
    }

    const auto& actionScript{it->second};

    std::optional<sol::environment> env;
    const auto path{scriptsFolder() / actionScript};
    auto chooseAction = getScriptFunction(path, "chooseAction", env, true, true);
    if (!chooseAction) {
        const auto message{
            fmt::format("Could not find 'chooseAction' function. Script '{:s}'", path.string())};

        if (gameSettings().battle.debugAi) {
            showErrorMessageBox(message);
        } else {
            spdlog::error(message);
        }

        chooseBattleAction(objectMap, battleMsgData, unitId, possibleActions, possibleTargets,
                           battleAction, targetUnitId, attackerUnitId);
        return;
    }

    try {
        const bindings::BattleMsgDataViewMutable battleView{battleMsgData, objectMap};

        const CMidUnit* unit = static_cast<const CMidUnit*>(
            objectMap->vftable->findScenarioObjectById(objectMap, unitId));
        if (!unit) {
            const auto message = fmt::format("Could not find unit {:s} in objectMap",
                                             idToString(unitId));
            if (gameSettings().battle.debugAi) {
                showErrorMessageBox(message);
            } else {
                spdlog::error(message);
            }
            chooseBattleAction(objectMap, battleMsgData, unitId, possibleActions, possibleTargets,
                               battleAction, targetUnitId, attackerUnitId);
            return;
        }
        const bindings::UnitView activeUnitView{unit};

        std::vector<BattleAction> actions;
        actions.reserve(possibleActions->length);
        for (auto& v : *possibleActions) {
            actions.push_back(v);
        }

        const CMidgardID* attTargGroupId{possibleTargets->attackTargetGroupId};
        std::optional<bindings::GroupView> attackTargetGroupView;
        if (const auto* group = getGroup(objectMap, attTargGroupId)) {
            attackTargetGroupView = bindings::GroupView{group, objectMap, attTargGroupId};
        }

        std::vector<int> attackTargets;
        attackTargets.reserve(possibleTargets->attackTargets->length);
        for (auto& v : *possibleTargets->attackTargets) {
            attackTargets.push_back(v);
        }

        const CMidgardID* item1TargGroupId{possibleTargets->item1TargetGroupId};
        std::optional<bindings::GroupView> item1TargetGroupView;
        if (const auto* group = getGroup(objectMap, item1TargGroupId)) {
            item1TargetGroupView = bindings::GroupView{group, objectMap, item1TargGroupId};
        }

        std::vector<int> item1Targets;
        item1Targets.reserve(possibleTargets->item1Targets->length);
        for (auto& v : *possibleTargets->item1Targets) {
            item1Targets.push_back(v);
        }

        const CMidgardID* item2TargGroupId{possibleTargets->item2TargetGroupId};
        std::optional<bindings::GroupView> item2TargetGroupView;
        if (const auto* group = getGroup(objectMap, item2TargGroupId)) {
            item2TargetGroupView = bindings::GroupView{group, objectMap, item2TargGroupId};
        }

        std::vector<int> item2Targets;
        item2Targets.reserve(possibleTargets->item2Targets->length);
        for (auto& v : *possibleTargets->item2Targets) {
            item2Targets.push_back(v);
        }

        int chosenAction = 0;
        bindings::IdView chosenTargetId{(CMidgardID*)nullptr};
        bindings::IdView chosenAttackerId{(CMidgardID*)nullptr};
        sol::tie(chosenAction, chosenTargetId,
                 chosenAttackerId) = (*chooseAction)(battleView, activeUnitView, actions,
                                                     attackTargetGroupView, attackTargets,
                                                     item1TargetGroupView, item1Targets,
                                                     item2TargetGroupView, item2Targets);

        *targetUnitId = chosenTargetId.id;
        *attackerUnitId = chosenAttackerId.id;
        *battleAction = static_cast<BattleAction>(chosenAction);
    } catch (const std::exception& e) {
        const auto message{
            fmt::format("Failed to run '{:s}' script. Reason: {:s}", path.string(), e.what())};

        if (gameSettings().battle.debugAi) {
            showErrorMessageBox(message);
        } else {
            spdlog::error(message);
        }

        *targetUnitId = *unitId;
        *attackerUnitId = *unitId;
        *battleAction = gameSettings().battle.fallbackAction;
    }
}

void __fastcall setUnitFlag5Hooked(game::BattleMsgData* battleMsgData,
                                   int /*%edx*/,
                                   game::CMidgardID* unitId,
                                   bool enabled)
{
    using namespace game;

    static const auto& battleApi = BattleMsgDataApi::get();

    if (!enabled) {
        for (int i = 0; i < 13; i++) {
            if (battleMsgData->turnsOrder[i].unitId == *unitId) {
                if (battleMsgData->turnsOrder[i].attackCount > 1)
                    return;
                break;
            }
        }
    }

    UnitInfo* info = battleApi.getUnitInfoById(battleMsgData, unitId);
    if (info)
        info->unitFlags.parts.attackedOnceOfTwice = enabled;
}

game::UpdateDisableResult* __stdcall updateParalyzePetrifyEffectsHooked(
    game::UpdateDisableResult* value,
    game::BattleMsgData* battleMsgData,
    game::UnitInfo* unitInfo,
    game::CMidgardID* unitId)
{
    using namespace game;

    static const auto& battleApi = BattleMsgDataApi::get();

    CMidgardID id = unitInfo->unitId1;

    bool isBroken = false;
    bool isLong = battleApi.getUnitStatus(battleMsgData, &id, BattleStatus::DisableLong);

    if (!battleApi.getUnitStatus(battleMsgData, &id, BattleStatus::Paralyze)
        && !battleApi.getUnitStatus(battleMsgData, &id, BattleStatus::Petrify)) {
        value->isBroken = isBroken;
        value->isLong = isLong;
        return value;
    }

    if (battleApi.getUnitStatus(battleMsgData, &id, BattleStatus::Dead)) {
        value->isBroken = true;
        value->isLong = isLong;
        return value;
    }

    bool isUnit1 = unitInfo->unitId1 == *unitId;
    bool isUnit2 = unitInfo->unitId2 == *unitId;

    if (isUnit1 || isUnit2) {
        if (isLong) {
            if (isUnit2) {
                isBroken = false;
            } else {
                if (gameSettings().extendedBattle.longEffectsUsesPower
                    != baseGameSettings().extendedBattle.longEffectsUsesPower) {
                    UnitInfo* info = battleApi.getUnitInfoById(battleMsgData, unitId);
                    int remainingDuration = unitInfo->disableAppliedRound;
                    remainingDuration -= 1;

                    if (remainingDuration <= 0) {
                        isBroken = true;
                        info->disableAppliedRound = 0;
                    } else {
                        isBroken = false;
                        info->disableAppliedRound = remainingDuration;
                    }
                } else {
                    int curRound = battleMsgData->currentRound;
                    int disableRound = unitInfo->disableAppliedRound;
                    isBroken = battleApi.checkLongEffectDuration(curRound - disableRound);
                }
            }
        } else {
            isBroken = true;
        }
    }

    value->isBroken = isBroken;
    value->isLong = isLong;
    return value;
}

bool __stdcall shouldDisablePoisonHooked(game::BattleMsgData* battleMsgData, game::CMidgardID* unitId)
{
    using namespace game;

    const auto& battleApi = BattleMsgDataApi::get();
    const auto& gSettings = gameSettings();
    const auto& bSettings = baseGameSettings();

    if (!battleApi.getUnitStatus(battleMsgData, unitId, BattleStatus::Poison))
        return false;
    if (!battleApi.getUnitStatus(battleMsgData, unitId, BattleStatus::PoisonLong))
        return true;

    UnitInfo* unitInfo = battleApi.getUnitInfoById(battleMsgData, unitId);

    int curRound = battleMsgData->currentRound;
    int disableRound = unitInfo->poisonAppliedRound;

    bool isDisabled = battleApi.checkLongEffectDuration(curRound - disableRound);

    if (gSettings.extendedBattle.longEffectsUsesPower != bSettings.extendedBattle.longEffectsUsesPower)
    {
        isDisabled = disableRound - 1 <= 0;
        if (isDisabled)
            unitInfo->poisonAppliedRound = 0;
        else
            unitInfo->poisonAppliedRound -= 1;
    }

    return isDisabled;
}

bool __stdcall shouldDisableFrostbiteHooked(game::BattleMsgData* battleMsgData, game::CMidgardID* unitId)
{
    using namespace game;

    auto& battleApi = BattleMsgDataApi::get();
    const auto& gSettings = gameSettings();
    const auto& bSettings = baseGameSettings();

    if (!battleApi.getUnitStatus(battleMsgData, unitId, BattleStatus::Frostbite))
        return false;
    if (!battleApi.getUnitStatus(battleMsgData, unitId, BattleStatus::FrostbiteLong))
        return true;

    UnitInfo* unitInfo = battleApi.getUnitInfoById(battleMsgData, unitId);

    int curRound = battleMsgData->currentRound;
    int disableRound = unitInfo->frostbiteAppliedRound;

    bool isDisabled = battleApi.checkLongEffectDuration(curRound - disableRound);

    if (gSettings.extendedBattle.longEffectsUsesPower
        != bSettings.extendedBattle.longEffectsUsesPower) {
        isDisabled = disableRound - 1 <= 0;
        if (isDisabled)
            unitInfo->frostbiteAppliedRound = 0;
        else
            unitInfo->frostbiteAppliedRound -= 1;
    }

    return isDisabled;
}

bool __stdcall shouldDisableBlisterHooked(game::BattleMsgData* battleMsgData,
                                          game::CMidgardID* unitId)
{
    using namespace game;

    auto& battleApi = BattleMsgDataApi::get();
    const auto& gSettings = gameSettings();
    const auto& bSettings = baseGameSettings();

    if (!battleApi.getUnitStatus(battleMsgData, unitId, BattleStatus::Blister))
        return false;
    if (!battleApi.getUnitStatus(battleMsgData, unitId, BattleStatus::BlisterLong))
        return true;

    UnitInfo* unitInfo = battleApi.getUnitInfoById(battleMsgData, unitId);

    int curRound = battleMsgData->currentRound;
    int disableRound = unitInfo->blisterAppliedRound;

    bool isDisabled = battleApi.checkLongEffectDuration(curRound - disableRound);

    if (gSettings.extendedBattle.longEffectsUsesPower
        != bSettings.extendedBattle.longEffectsUsesPower) {
        isDisabled = disableRound - 1 <= 0;
        if (isDisabled)
            unitInfo->blisterAppliedRound = 0;
        else
            unitInfo->blisterAppliedRound -= 1;
    }

    return isDisabled;
}

bool __stdcall shouldDisableTransformOtherHooked(game::BattleMsgData* battleMsgData,
                                                game::CMidgardID* unitId)
{
    using namespace game;

    auto& battleApi = BattleMsgDataApi::get();
    const auto& gSettings = gameSettings();
    const auto& bSettings = baseGameSettings();

    if (!battleApi.getUnitStatus(battleMsgData, unitId, BattleStatus::TransformLong))
        return false;

    UnitInfo* unitInfo = battleApi.getUnitInfoById(battleMsgData, unitId);

    int curRound = battleMsgData->currentRound;
    int disableRound = unitInfo->transformAppliedRound;

    bool isDisabled = battleApi.checkLongEffectDuration(curRound - disableRound);

    if (gSettings.extendedBattle.longEffectsUsesPower
        != bSettings.extendedBattle.longEffectsUsesPower) {
        isDisabled = disableRound - 1 <= 0;
        if (isDisabled)
            unitInfo->transformAppliedRound = 0;
        else
            unitInfo->transformAppliedRound -= 1;
    }

    return isDisabled;
}

} // namespace hooks
