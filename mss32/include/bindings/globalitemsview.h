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

#ifndef GLOBALITEMSVIEW_H
#define GLOBALITEMSVIEW_H

#include <optional>
#include <string>

namespace sol {
class state;
}

namespace bindings {

class ItemBaseView;

class GlobalItemsView
{
public:
    static void bind(sol::state& lua);

    std::optional<ItemBaseView> getBase(const std::string& idStr) const;
};

} // namespace bindings

#endif