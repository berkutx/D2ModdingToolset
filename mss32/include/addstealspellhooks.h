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
// File: addstealspellhooks.h
// ============================================================

#ifndef ADDSTEALSPELLHOOKS_H
#define ADDSTEALSPELLHOOKS_H

#include "midgardid.h"

namespace game {

struct IMidgardObjectMap;

}

namespace hooks {

struct StealSpellBuildContext
{
    bool active{false};

    int insertedCount{0};

    bool lastBuildHadVisibleSpells{true};

    game::CMidgardID mageId;
    game::CMidgardID playerId;

    const game::IMidgardObjectMap* objectMap{};
};

extern thread_local StealSpellBuildContext g_stealSpellCtx;

bool shouldDisplayStealSpell(const game::CMidgardID* spellId);

void* getBuildSpellListHooked();

void** getBuildSpellListOrig();

void* getSortedIdListInsertHooked();

void** getSortedIdListInsertOrig();

void* getStealSpellInterfCtorHooked();

void** getStealSpellInterfCtorOrig();

} // namespace hooks

#endif