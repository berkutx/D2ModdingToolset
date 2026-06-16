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

#include "globalspellsview.h"

#include "idview.h"
#include "spellutils.h"
#include "spellview.h"

#include <sol/sol.hpp>

namespace bindings {

void GlobalSpellsView::bind(sol::state& lua)
{
    auto view = lua.new_usertype<GlobalSpellsView>("GlobalSpellsView");

    view["get"] = &GlobalSpellsView::get;
}

std::optional<SpellView> GlobalSpellsView::get(const std::string& idStr) const
{
    using namespace game;

    IdView idView(idStr);

    auto spell = SpellUtilsApi::get().findSpellById(&idView.id);

    if (!spell) {
        return std::nullopt;
    }

    return SpellView(spell);
}

} // namespace bindings