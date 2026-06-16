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
// File: stealitemhooks.h
// ============================================================

#ifndef STEALITEMHOOKS_H
#define STEALITEMHOOKS_H

#include "midgardid.h"
#include "stealmerchantiteminterf.h"

namespace game {

struct IMidgardObjectMap;

}

namespace hooks {

struct StealItemBuildContext
{
    bool active{false};

    int insertedCount{0};

    bool lastBuildHadVisibleItems{true};

    game::CMidgardID merchantId;
    game::CMidgardID playerId;

    const game::IMidgardObjectMap* objectMap{};
};

extern thread_local StealItemBuildContext g_stealItemCtx;

bool shouldDisplayStealItem(const game::CMidgardID* merchantId, game::StealItemEntry* entry);

void* getStealItemCtorHooked();

void** getStealItemCtorOrig();

void* getAddStealItemHooked();

void** getAddStealItemOrig();

} // namespace hooks

#endif