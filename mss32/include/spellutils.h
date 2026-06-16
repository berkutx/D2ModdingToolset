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
// File: spellutils.h
// ============================================================

#ifndef SPELLUTILS_H
#define SPELLUTILS_H

#include "midgardid.h"

namespace game {

struct TStrategicSpell;

namespace SpellUtilsApi {

struct Api
{
    using FindSpellById = TStrategicSpell*(__stdcall*)(CMidgardID* spellId);

    FindSpellById findSpellById;
};

Api& get();

} // namespace SpellUtilsApi

} // namespace game

#endif