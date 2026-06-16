/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/bartonsun/D2ModdingToolset)
 * Copyright (C) 2026 Max Vynogradov.
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

// ============================================================
// File: addstealspellhooks.cpp
// ============================================================

#include "addstealspellhooks.h"

#include "DYNAMICCAST.h"
#include "MIDGARDOBJECTMAP.h"
#include "MIDSITEMAGE.h"
#include "button.h"
#include "game.h"
#include "gameutils.h"
#include "globaldata.h"
#include "globalvariables.h"
#include "idset.h"
#include "listbox.h"
#include "midgardid.h"
#include "midpopupinterf.h"
#include "scripts.h"
#include "spellutils.h"
#include "spellview.h"
#include "stealspellinterf.h"
#include "strategicspell.h"
#include "utils.h"

#include <algorithm>
#include <optional>
#include <string>

#include <dialoginterf.h>
#include <sol/sol.hpp>
#include <spdlog/spdlog.h>

namespace hooks {

thread_local StealSpellBuildContext g_stealSpellCtx;

//
// lua
//

static std::optional<sol::environment> env;

static std::optional<sol::function> theftFilterMageTowerLua;

//
// scenario variable
//

static const char scenSpellCanStealLessCostSum[]{"SPELL_CAN_STEAL_LESS_COST_SUM"};

static constexpr int noSpellCostLimit{9999 * 6};

//
// build hook
//

using BuildSpellList = bool(__stdcall*)(const game::IMidgardObjectMap*,
                                        const game::CMidgardID*,
                                        const game::CMidgardID*,
                                        void*);

static BuildSpellList buildSpellListOrig;

//
// insert hook
//

using SortedInsert = int(__thiscall*)(void*, int, game::CMidgardID*);

static SortedInsert sortedInsertOrig;

//
// ctor hook
//

using StealSpellCtor = game::CMidDragDropInterf*(__thiscall*)(void*, int, int, int, void*);

static StealSpellCtor stealSpellCtorOrig;

//
// debug
//

static std::string midgardIdToDebugString(const game::CMidgardID* id)
{
    if (!id) {
        return "null";
    }

    char buffer[32]{};

    game::CMidgardIDApi::get().toString(id, buffer);

    return std::string(buffer);
}

//
// helpers
//

static bool getScenarioVariable(const game::IMidgardObjectMap* objectMap,
                                const char* name,
                                int& value)
{
    const auto scenVars{getScenarioVariables(objectMap)};

    if (!scenVars) {
        return false;
    }

    for (const auto& pair : scenVars->variables) {

        if (_stricmp(pair.second.name, name) == 0) {

            value = pair.second.value;

            spdlog::info("[STEAL_SPELL] scenario variable found");
            spdlog::info(pair.second.name);
            spdlog::info(value);

            return true;
        }
    }

    return false;
}

static int getSpellCostLimit(const game::IMidgardObjectMap* objectMap)
{
    int value{};

    if (getScenarioVariable(objectMap, scenSpellCanStealLessCostSum, value)) {
        return std::max(0, value);
    }

    const game::GlobalData* global{*game::GlobalDataApi::get().getGlobalData()};

    return global->globalVariables->data->stealSpellValue;
}

static int getSpellTotalCost(const game::TStrategicSpell* spell)
{
    if (!spell || !spell->data) {
        return 0;
    }

    const auto& cost = spell->data->buyCost;

    return cost.gold + cost.infernalMana + cost.lifeMana + cost.runicMana + cost.deathMana
           + cost.groveMana;
}

//
// filter
//

bool shouldDisplayStealSpell(const game::CMidgardID* spellId)
{
    if (!spellId) {
        return false;
    }

    auto spell = game::SpellUtilsApi::get().findSpellById(const_cast<game::CMidgardID*>(spellId));

    if (!spell || !spell->data) {

        spdlog::info("[STEAL_SPELL] spell lookup failed");

        return false;
    }

    //
    // lua has priority
    //

    if (!theftFilterMageTowerLua) {

        const auto path{scriptsFolder() / "theft.lua"};

        theftFilterMageTowerLua = getScriptFunction(path, "theftFilterMageTower", env, false, true);

        if (theftFilterMageTowerLua) {

            spdlog::info("[STEAL_SPELL] theft.lua loaded");

        } else {

            spdlog::info("[STEAL_SPELL] failed to load theft.lua");
        }
    }

    //
    // lua
    //

    if (theftFilterMageTowerLua) {

        try {

            sol::state_view lua(theftFilterMageTowerLua->lua_state());

            sol::table stealSpellContext = lua.create_table();

            stealSpellContext["mageTowerId"] = &g_stealSpellCtx.mageId;

            stealSpellContext["playerId"] =&g_stealSpellCtx.playerId;

            stealSpellContext["spell"] = bindings::SpellView(spell);

            sol::object result = (*theftFilterMageTowerLua)(stealSpellContext);

            if (result.is<bool>()) {
                return result.as<bool>();
            }

        } catch (const std::exception& e) {

            spdlog::info("[STEAL_SPELL] lua exception");
            spdlog::info(e.what());

            const auto path{scriptsFolder() / "theft.lua"};

            showErrorMessageBox(fmt::format("Failed to run "
                                            "'{:s}' script.\n"
                                            "Reason: '{:s}'",
                                            path.string(), e.what()));
        }
    }

    //
    // cpp fallback
    //

    int limit = getSpellCostLimit(g_stealSpellCtx.objectMap);

    //
    // disabled
    //

    if (limit >= noSpellCostLimit) {
        return true;
    }

    int totalCost = getSpellTotalCost(spell);

    return totalCost < limit;
}

//
// build hook
//

bool __stdcall buildSpellListHooked(const game::IMidgardObjectMap* objectMap,
                                    const game::CMidgardID* mageId,
                                    const game::CMidgardID* playerId,
                                    void* list)
{
    spdlog::info("[STEAL_SPELL] build start");

    auto mageObj = objectMap->vftable->findScenarioObjectById(objectMap, mageId);

    auto playerObj = objectMap->vftable->findScenarioObjectById(objectMap, playerId);

    if (!mageObj || !playerObj) {

        spdlog::info("[STEAL_SPELL] failed to resolve objects");

        return buildSpellListOrig(objectMap, mageId, playerId, list);
    }

    //
    // activate context
    //

    g_stealSpellCtx.active = true;

    g_stealSpellCtx.insertedCount = 0;

    g_stealSpellCtx.objectMap = objectMap;

    g_stealSpellCtx.mageId = mageObj->id;

    g_stealSpellCtx.playerId = playerObj->id;

    try {

        buildSpellListOrig(objectMap, mageId, playerId, list);

    } catch (...) {

        g_stealSpellCtx.active = false;

        throw;
    }

    //
    // save ui state
    //

    g_stealSpellCtx.lastBuildHadVisibleSpells = g_stealSpellCtx.insertedCount > 0;

    bool result = g_stealSpellCtx.lastBuildHadVisibleSpells;

    //
    // cleanup
    //

    g_stealSpellCtx.active = false;

    spdlog::info("[STEAL_SPELL] visible spells");
    spdlog::info(g_stealSpellCtx.insertedCount);

    return result;
}

//
// insert hook
//

int __fastcall sortedIdListInsertHooked(void* thisptr,
                                        int /*edx*/,
                                        int a1,
                                        game::CMidgardID* spellId)
{
    //
    // not our context
    //

    if (!g_stealSpellCtx.active) {
        return sortedInsertOrig(thisptr, a1, spellId);
    }

    //
    // filter
    //

    if (!shouldDisplayStealSpell(spellId)) {
        return a1;
    }

    //
    // visible spell
    //

    ++g_stealSpellCtx.insertedCount;

    //
    // keep
    //

    return sortedInsertOrig(thisptr, a1, spellId);
}

//
// ctor hook
//

game::CMidDragDropInterf* __fastcall stealSpellInterfCtorHooked(void* thisptr,
                                                                int /*edx*/,
                                                                int a2,
                                                                int functor,
                                                                int a4,
                                                                void* a5)
{
    auto result = stealSpellCtorOrig(thisptr, a2, functor, a4, a5);

    //
    // no visible spells
    //

    if (!g_stealSpellCtx.lastBuildHadVisibleSpells) {

        auto popup = reinterpret_cast<game::CMidPopupInterf*>(result);

        if (!popup || !popup->popupData) {
            return result;
        }

        auto dialog = popup->popupData->dialog;

        if (dialog) {

            const auto& dlgApi = game::CDialogInterfApi::get();

            auto okBtn = dlgApi.findButton(dialog, "BTN_YES");

            if (okBtn) {
                okBtn->vftable->setEnabled(okBtn, false);
            }
        }
    }

    return result;
}

//
// exports
//

void* getBuildSpellListHooked()
{
    return (void*)buildSpellListHooked;
}

void** getBuildSpellListOrig()
{
    return (void**)&buildSpellListOrig;
}

void* getSortedIdListInsertHooked()
{
    return (void*)sortedIdListInsertHooked;
}

void** getSortedIdListInsertOrig()
{
    return (void**)&sortedInsertOrig;
}

void* getStealSpellInterfCtorHooked()
{
    return (void*)stealSpellInterfCtorHooked;
}

void** getStealSpellInterfCtorOrig()
{
    return (void**)&stealSpellCtorOrig;
}

} // namespace hooks