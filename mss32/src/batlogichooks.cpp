/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2025 Alexey Voskresensky.
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

#include "batlogichooks.h"
#include "batlogic.h"
#include "battlemsgdata.h"
#include "battlemsgdatahooks.h"
#include "game.h"
#include "midunit.h"
#include "originalfunctions.h"
#include "unitinfolist.h"
#include "battlemsgdataviewmutable.h"
#include "gameutils.h"
#include "groupview.h"
#include "scripts.h"
#include "resultsender.h"
#ifdef D2_TESTDRV
#include "testdrv/battletrace.h"
#endif
#include <spdlog/spdlog.h>

static std::atomic<game::CBatLogic*> g_batLogic{nullptr};

namespace hooks {

void __fastcall battleTurnHooked(game::CBatLogic* thisptr,
                                 int /*%edx*/,
                                 game::CResultSender* resultSender,
                                 int groupBattleCondition,
                                 game::CMidgardID* a4,
                                 game::CMidgardID* a5)
{
    using namespace game;
#ifdef D2_TESTDRV
    testdrv::battletrace::Scope trace("resolution", thisptr ? thisptr->battleMsgData : nullptr,
                                     thisptr ? thisptr->objectMap : nullptr);
#endif

    const CBatLogicApi::Api& batLogicApi = CBatLogicApi::get();

    g_batLogic.store(thisptr, std::memory_order_release);

    batLogicApi.updateUnitsBattleXp(thisptr->objectMap, thisptr->battleMsgData);

    getOriginalFunctions().battleTurn(thisptr, resultSender, groupBattleCondition, a4, a5);
}

void __fastcall updateGroupsIfBattleIsOverHooked(game::CBatLogic* thisptr,
                                                 int /*%edx*/,
                                                 game::CResultSender* resultSender)
{
    using namespace game;
#ifdef D2_TESTDRV
    testdrv::battletrace::Scope trace("finalize-check", thisptr ? thisptr->battleMsgData : nullptr,
                                     thisptr ? thisptr->objectMap : nullptr);
#endif
    auto& batLogicApi = CBatLogicApi::get();

    // Available whenever battle logic runs — earlier and more often than battleTurn alone
    // (e.g. first human turn may not enter battleTurn until an action is resolved).
    if (thisptr)
        g_batLogic.store(thisptr, std::memory_order_release);

    if (getFiredBattleKeys()) 
    {
        for (const auto& turn : thisptr->battleMsgData->turnsOrder) 
        {
            if (turn.unitId != emptyId && turn.unitId != invalidId) 
            {
                tryFirePreTurnHookOnce(thisptr->objectMap, thisptr->battleMsgData, &turn.unitId);
                break;
            }
        }
    }

    if (!batLogicApi.isBattleOver(thisptr)) {
        return;
    }

    const BattleMsgDataApi::Api& battleApi = BattleMsgDataApi::get();
    const Functions& fn = game::gameFunctions();
    const UnitInfoListApi::Api& listApi = UnitInfoListApi::get();
    const CMidgardIDApi::Api& idApi = CMidgardIDApi::get();
    BattleMsgData* msgData = thisptr->battleMsgData;
    IMidgardObjectMap* objMap = thisptr->objectMap;

    batLogicApi.checkAndDestroyEquippedBattleItems(objMap, msgData);

    UnitInfoList unitInfos{};
    listApi.constructor(&unitInfos);
    battleApi.getUnitInfos(msgData, &unitInfos, false);
    listApi.sortBy(&unitInfos, listApi.compareByUnsummonOrder);

    CMidgardID tempId;

    for (const UnitInfo& unitInfo : unitInfos) {
        idApi.validateId(&tempId, unitInfo.unitId1);

        if (CMidUnit* unit = fn.findUnitById(objMap, &tempId)) {
            if (battleApi.getUnitStatus(msgData, &tempId, BattleStatus::Summon)) {
                batLogicApi.applyCBatAttackUnsummonEffect(objMap, &tempId, msgData, resultSender);
            }
            else if (unit->transformed) {
                bool retreated = battleApi.getUnitStatus(msgData, &tempId, BattleStatus::Retreated);
                batLogicApi.applyCBatAttackUntransformEffect(objMap, &tempId, msgData, resultSender,
                                                             !retreated);
            }
        }
    }

    CMidgardID winnerGroupId;
    batLogicApi.getBattleWinnerGroupId(thisptr, &winnerGroupId);

    static const std::filesystem::path scriptPath = scriptsFolder() / "hooks/hooks.lua";
    std::optional<sol::environment> env;

    if (std::optional OnBattleEnd = getScriptFunction(scriptPath, "OnBattleEnd", env, false, true)) {
        try {
            const IMidgardObjectMap* globalObjMap = hooks::getObjectMap();
            if (const CMidUnitGroup* winnerGroup = hooks::getGroup(globalObjMap, &winnerGroupId)) {
                const bindings::GroupView win{winnerGroup, globalObjMap, &winnerGroupId};
                const bindings::BattleMsgDataView battleView{thisptr->battleMsgData, globalObjMap};
                (*OnBattleEnd)(win, battleView);
            }
        } catch (const std::exception& e) {
            showErrorMessageBox(fmt::format("Lua Error (OnBattleEnd): {:s}", e.what()));
        }
    }

    batLogicApi.applyCBatAttackGroupUpgrade(objMap, &winnerGroupId, msgData, resultSender);
    batLogicApi.applyCBatAttackGroupBattleCount(objMap, &winnerGroupId, msgData, resultSender);
    batLogicApi.restoreLeaderPositionsAfterDuel(objMap, msgData);

    listApi.destructor(&unitInfos);

    g_batLogic.store(nullptr, std::memory_order_release);
}

game::CBatLogic* hooks::getBatLogic()
{
    auto* batLogic = g_batLogic.load();
    return batLogic ? batLogic : nullptr;
}

} // namespace hooks
