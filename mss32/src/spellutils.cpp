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
// File: spellutils.cpp
// ============================================================

#include "spellutils.h"
#include "version.h"

#include <array>

namespace game::SpellUtilsApi {

// clang-format off

static std::array<Api, 4> functions = {{

    // Akella
    Api{
        (Api::FindSpellById)0x005dbef3
    },

    // Russobit
    Api{
        (Api::FindSpellById)0x005dbef3
    },

    // Gog
    Api{
        (Api::FindSpellById)0x005DAC28
    },

    // Scenario Editor
    Api{
        (Api::FindSpellById)0x005dbef3
    }
}};

// clang-format on

Api& get()
{
    return functions[static_cast<int>(hooks::gameVersion())];
}

} // namespace game::SpellUtilsApi