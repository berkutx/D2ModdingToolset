/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2026 Alexey Voskresensky.
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

#include "fortification.h"
#include "game.h"
#include "gameutils.h"
#include "globaldata.h"
#include "globalvariables.h"
#include "idlist.h"
#include "leadersforhire.h"
#include "lordcat.h"
#include "lordtype.h"
#include "midgardid.h"
#include "midgardobjectmap.h"
#include "midplayer.h"
#include "midstack.h"
#include "midunit.h"
#include "netplayerinfo.h"
#include "originalfunctions.h"
#include "playerview.h"
#include "racetype.h"
#include "scenarioinfo.h"
#include "scripts.h"
#include "utils.h"
#include "unitgenerator.h"
#include "bindings/idview.h"
#include "bindings/playerview.h"
#include "bindings/unitview.h"
#include <filesystem>
#include <sol/sol.hpp>
#include <spdlog/spdlog.h>

namespace hooks {


static sol::table idListToTable(lua_State* L, const game::IdList* list)
{
    sol::state_view lua(L);
    sol::table result = lua.create_table();
    for (const auto& id : *list) {
        result.add(bindings::IdView(&id));
    }
    return result;
}

static void tableToIdList(const sol::table& tbl, game::IdList* list)
{
    using namespace game;

    const auto& listApi = IdListApi::get();
    const auto& fn = gameFunctions();
    const auto& global = GlobalDataApi::get();
    const auto globalData = *global.getGlobalData();

    listApi.clear(list);

    for (auto& kv : tbl) {
        sol::object obj = kv.second;
        if (!obj.is<bindings::IdView>()) {
            spdlog::warn("Skipping non-IdView value in Lua table");
            continue;
        }
        auto idView = obj.as<bindings::IdView>();
        const CMidgardID& id = idView;

        void* unitImpl = global.findById(globalData->units, &id);
        if (!unitImpl) {
            spdlog::warn("Skipping non-existent unit ID: {}", hooks::idToString(&id));
            continue;
        }

        const IUsUnit* unit = static_cast<const IUsUnit*>(unitImpl);
        if (!fn.castUnitImplToLeader(unit) && (!fn.castUnitImplToNoble(unit))) {
            spdlog::debug("Skipping non-leader unit ID: {}", hooks::idToString(&id));
            continue;
        }

        listApi.pushBack(list, &id);
    }
}

bool __stdcall getLeadersHireListHooked(const game::IMidgardObjectMap* objectMap,
                                        const game::CMidgardID* playerId,
                                        game::IdList* hireList)
{
    using namespace game;

    const auto& listApi = IdListApi::get();
    const auto& global = GlobalDataApi::get();
    const auto globalData = *global.getGlobalData();

    listApi.clear(hireList);

    auto player = getPlayer(objectMap, playerId);

    game::RacesMap** races = globalData->races;
    TRaceType* race = (TRaceType*)global.findById(races, &player->raceId);

    auto leaders = race->data->leaders;
    for (CMidgardID* it = leaders.bgn; it != leaders.end; ++it) {
        listApi.pushBack(hireList, it);
    }

    CMidgardID nobleId = race->data->nobleUnitId;
    listApi.pushBack(hireList, &nobleId);


    static std::optional<sol::environment> env;
    static std::optional<sol::function> processFunction;
    const std::filesystem::path path = scriptsFolder() / "hire.lua";

    if (!env && !processFunction) {
        processFunction = getScriptFunction(path, "getLeadersHireList", env, false, true);
    }

    if (!processFunction) {
        return true;
    }

    try {
        lua_State* L = env->lua_state();
        sol::table inputList = idListToTable(L, hireList);
        bindings::PlayerView playerView(player, objectMap);
        sol::table outputList = (*processFunction)(inputList, playerView);

        if (!outputList.valid() || outputList.get_type() != sol::type::table) {
            spdlog::warn("Lua function getLeadersHireList did not return a table.");
            return true;
        }

        tableToIdList(outputList, hireList);

    } catch (const std::exception& e) {
        showErrorMessageBox(fmt::format("Failed to run '{:s}' script.\n"
                                        "Reason: '{:s}'",
                                        path.string(), e.what()));
        spdlog::error("Lua error in {}: {}", path.string(), e.what());
    }

    return true;
}


void __stdcall addNobleLeaderToUIHooked(game::IdList* unitIds,
                                        game::CMidPlayer* player,
                                        game::CPhaseGame* phaseGame)
{ 
    return;
}

bool __stdcall changeStackLeaderInCapitalHooked(game::IMidgardObjectMap* objectMap,
                                                game::NetPlayerInfo* playerInfo,
                                                int turn)
{
    using namespace game;

    const int MODE_SKIP = 1;
    const int MODE_CLEAR_IMPORT = 2;

    const auto& fn = gameFunctions();
    const auto& idApi{CMidgardIDApi::get()};
    const auto& visitor = VisitorApi::get();
    const auto& global = GlobalDataApi::get();
    const auto globalData = *global.getGlobalData();
    const auto& groupApi = CMidUnitGroupApi::get();
    const auto& smartPtrApi = SmartPointerApi::get();
    auto variables{getScenarioVariables(objectMap)};

    int mode{0};
    if (variables && variables->variables.length) {
        spdlog::debug("Loop through {:d} scenario variables", variables->variables.length);
        for (const auto& variable : variables->variables) {
            const auto& name = variable.second.name;
            const auto expectedName{"START_LEADER_CHANGE_MODE"};
            if (!strncmp(name, expectedName, sizeof(name))) {
                mode = variable.second.value;
                break;
            }
        }
    }

    if (mode == -1) {
        CMidgardID playerId = playerInfo->playerId;
        auto capital = fn.findCapitalByPlayerId(&playerId, objectMap);
        visitor.eraseStack(&capital->stackId, objectMap, 1);        
        return false;
    }

    if (mode & MODE_CLEAR_IMPORT) {
        smartPtrApi.createOrFree(&playerInfo->importLeaderStore, 0);
    }

    if (mode & MODE_SKIP) {
        return false;
    }

    bool wasReplaced = getOriginalFunctions().changeStackLeaderInCapital(objectMap, playerInfo, turn);

    if (!wasReplaced) {
        return wasReplaced;
    }

    auto scenarioInfo = getScenarioInfo(objectMap);
    if (scenarioInfo->currentTurn != turn) {
        return wasReplaced;
    }

    CMidgardID playerId = playerInfo->playerId;
    auto capital = fn.findCapitalByPlayerId(&playerId, objectMap);
    if (!capital) {
        return wasReplaced;
    }

    auto stack = getStack(objectMap, &capital->stackId);
    if (!stack) {
        return wasReplaced;
    }

    auto group = fn.getStackFortRuinGroup(nullptr, objectMap, &capital->stackId);
    if (groupApi.getUnitsCount(group) != 1) {
        return wasReplaced;
    }

    auto currentLeaderId = stack->leaderId;
    CMidUnit* unit = fn.findUnitById(objectMap, &currentLeaderId);
    if (!unit) {
        return wasReplaced;
    }

    auto player = getPlayer(objectMap, &playerId);

    static std::optional<sol::environment> env;
    static std::optional<sol::function> modifyFunc;
    const std::filesystem::path path = scriptsFolder() / "hire.lua";

    if (!env && !modifyFunc) {
        modifyFunc = getScriptFunction(path, "getStartingLeader", env, false, true);
    }

    CMidgardID newLeaderId = emptyId;

    if (modifyFunc) {
        try {
            lua_State* L = env->lua_state();
            bindings::PlayerView playerView(player, objectMap);
            bindings::UnitView unitView(unit);
            sol::object resultObj = (*modifyFunc)(playerView, unitView);

            if (resultObj.is<bindings::IdView>()) {
                auto idView = resultObj.as<bindings::IdView>();
                const CMidgardID& id = idView;

                void* unitImpl = global.findById(globalData->units, &id);
                if (!unitImpl) {
                    spdlog::warn("changeStackLeader: ID {} not found", hooks::idToString(&id));
                    return wasReplaced;
                }
                const IUsUnit* unit = static_cast<const IUsUnit*>(unitImpl);
                bool isLeader = fn.castUnitImplToLeader(unit) != nullptr;
                bool isNoble = fn.castUnitImplToNoble(unit) != nullptr;
                if (!isLeader && !isNoble) {
                    spdlog::warn("changeStackLeader: ID {} is not a leader or noble",
                                 hooks::idToString(&id));
                    return wasReplaced;
                }
                newLeaderId = id;
            }
        } catch (const std::exception& e) {
            showErrorMessageBox(fmt::format("Failed to run getStartingLeader: {}", e.what()));
        }
    }

    if (newLeaderId != emptyId) {
        int suggestedLevel = scenarioInfo->suggestedLevel;

        void* unitImpl = global.findById(globalData->units, &newLeaderId);
        const IUsUnit* unit = static_cast<const IUsUnit*>(unitImpl);
        bool isNoble = fn.castUnitImplToNoble(unit) != nullptr;
        int effectiveLevel = isNoble ? 1 : suggestedLevel;

        CUnitGenerator* unitGenerator = globalData->unitGenerator;

        CMidgardID leveledImplId;
        unitGenerator->vftable->generateUnitImplId(unitGenerator, &leveledImplId, &newLeaderId,
                                                   effectiveLevel);
        if (leveledImplId == emptyId) {
            spdlog::error("changeStackLeader: failed to generate impl ID for level {}",
                          effectiveLevel);
            return wasReplaced;
        }
        unitGenerator->vftable->generateUnitImpl(unitGenerator, &leveledImplId);

        if (!visitor.extractUnitFromGroup(&currentLeaderId, &capital->stackId, objectMap, 1)) {
            spdlog::error("changeStackLeader: failed to extract current leader");
            return wasReplaced;
        }

        int creationTurn = scenarioInfo->currentTurn;
        if (!visitor.addUnitToGroup(&leveledImplId, &capital->stackId, 2, &creationTurn, 1,
                                    objectMap, 1)) {
            spdlog::error("changeStackLeader: failed to add new leader");
            return wasReplaced;
        }

        auto newStack = getStack(objectMap, &capital->stackId);
        if (!newStack) {
            spdlog::error("changeStackLeader: stack not found after addition");
            return wasReplaced;
        }
        CMidgardID newUnitId = newStack->leaderId;
        spdlog::debug("Leader replaced: new unit ID {}", hooks::idToString(&newUnitId));
    }

    return wasReplaced;
}

} // namespace hooks