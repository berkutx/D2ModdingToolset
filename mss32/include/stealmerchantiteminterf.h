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
// File: stealmerchantiteminterf.h
// ============================================================


#ifndef STEALMERCHANTITEMINTERF_H
#define STEALMERCHANTITEMINTERF_H

#include "currency.h"
#include "middragdropinterf.h"
#include "midgardid.h"

namespace game {

struct CListBoxInterf;

struct StealItemEntry
{
    CMidgardID itemId;
    Bank value;
    int unknown;
};

namespace StealMerchantItemInterfApi {

struct Api
{
    using Constructor =
        CMidDragDropInterf*(__thiscall*)(void* thisptr, int a2, int functor, int a4, void* a5);

    Constructor constructor;

    using GetListBox = CListBoxInterf*(__thiscall*)(void* thisptr);

    GetListBox getListBox;

    using AddStealItem = char*(__thiscall*)(void* thisptr, StealItemEntry* entry);

    AddStealItem addStealItem;

    //
    // helper wrappers (not work)
    //

    using GetMerchantId = game::CMidgardID*(__thiscall*)(void* thisptr);

    GetMerchantId getMerchantId;

    using GetItemVector = void*(__thiscall*)(void* thisptr);

    GetItemVector getItemVector;
};

Api& get();

} // namespace StealMerchantItemInterfApi

} // namespace game

#endif