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

#include "globalitemsview.h"

#include "globaldata.h"
#include "idview.h"
#include "itembase.h"
#include "itembaseview.h"

#include <sol/sol.hpp>

namespace bindings {

void GlobalItemsView::bind(sol::state& lua)
{
    auto view = lua.new_usertype<GlobalItemsView>("GlobalItemsView");

    view["getBase"] = &GlobalItemsView::getBase;
}

std::optional<ItemBaseView> GlobalItemsView::getBase(const std::string& idStr) const
{
    using namespace game;

    IdView idView(idStr);

    auto globalData = *GlobalDataApi::get().getGlobalData();

    auto item = GlobalDataApi::get().findItemById(globalData->itemTypes, &idView.id);

    if (!item) {
        return std::nullopt;
    }

    return ItemBaseView(item);
}

} // namespace bindings