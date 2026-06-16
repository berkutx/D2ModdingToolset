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
// File: stealspellinterf.h
// ============================================================


#include "MIDDRAGDROPINTERF.H"
#include "MIDGARDOBJECTMAP.H"

#ifndef STEALSPELLINTERF_H
#define STEALSPELLINTERF_H

namespace game {

struct CListBoxInterf;

namespace StealSpellInterfApi {

struct Api
{
    using Constructor =
        CMidDragDropInterf*(__thiscall*)(void* thisptr, int a2, int functor, int a4, void* a5);

    Constructor constructor;

    using GetListBox = CListBoxInterf*(__thiscall*)(void* thisptr);

    GetListBox getListBox;

   using BuildSpellList = bool(__stdcall*)(const game::IMidgardObjectMap*,
                                            game::CMidgardID,
                                            game::CMidgardID,
                                            void*);
    BuildSpellList buildSpellList;

};

Api& get();

} // namespace StealSpellInterfApi

} // namespace game

#endif