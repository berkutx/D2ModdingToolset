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

#include "spellview.h"

#include "attacksourcecat.h"
#include "categoryids.h"
#include "spellcat.h"
#include "strategicspell.h"
#include "unitimplview.h"
#include "unitutils.h"
#include "usunitimpl.h"
#include "modifierview.h"
#include "modifierutils.h"
#include "unitmodifier.h"

#include <sol/sol.hpp>
#include <vector>

namespace bindings {

SpellView::SpellView(const game::TStrategicSpell* spell)
    : spell(spell)
{ }

void SpellView::bind(sol::state& lua)
{
    auto view = lua.new_usertype<SpellView>("SpellView");

    view["id"] = sol::property(&getId);

    view["type"] = sol::property(&getCategory);

    view["level"] = sol::property(&getLevel);

    view["castingCost"] = sol::property(&getCastingCost);

    view["buyCost"] = sol::property(&getBuyCost);

    view["unit"] = sol::property(&getUnitImpl);

    view["restoreMove"] = sol::property(&getRestoreMove);

    view["area"] = sol::property(&getArea);

    view["damage"] = sol::property(&getDamage);

    view["heal"] = sol::property(&getHeal);

    view["ground"] = sol::property(&getGround);

    view["changeTerrain"] = sol::property(&getChangeTerrain);

    view["aiType"] = sol::property(&getAiCategory);

    view["modifier"] = sol::property(&getModifier);

    view["damageSource"] = sol::property(&getDamageSource);

    view["wards"] = sol::property(&SpellView::getWards);
}

IdView SpellView::getId() const
{
    return spell->id;
}

int SpellView::getCategory() const
{
    return (int)spell->data->spellCategory.id;
}

int SpellView::getLevel() const
{
    return spell->data->level;
}

CurrencyView SpellView::getCastingCost() const
{
    return CurrencyView(spell->data->castingCost);
}

CurrencyView SpellView::getBuyCost() const
{
    return CurrencyView(spell->data->buyCost);
}

std::optional<UnitImplView> SpellView::getUnitImpl() const
{
    using namespace game;

    auto unitId = spell->data->unitId;

    if (unitId == emptyId) {
        return std::nullopt;
    }

    auto unit = hooks::getUnitImpl(&unitId);

    if (!unit) {
        return std::nullopt;
    }

    return UnitImplView(unit);
}

int SpellView::getRestoreMove() const
{
    return spell->data->restoreMovePoints;
}

int SpellView::getArea() const
{
    return spell->data->area;
}

int SpellView::getDamage() const
{
    return spell->data->damageQty;
}

int SpellView::getHeal() const
{
    return spell->data->healQty;
}

int SpellView::getGround() const
{
    return (int)spell->data->groundCategory.id;
}

bool SpellView::getChangeTerrain() const
{
    return spell->data->changeGround;
}

int SpellView::getAiCategory() const
{
    return (int)spell->data->aiSpellCategory.id;
}

std::optional<ModifierView> SpellView::getModifier() const
{
    using namespace game;

    auto modifierId = spell->data->modifierId;

    if (modifierId == emptyId) {
        return std::nullopt;
    }

    auto modifier = hooks::getUnitModifier(&modifierId);

    if (!modifier || !modifier->data || !modifier->data->modifier) {
        return std::nullopt;
    }

    return ModifierView(modifier->data->modifier);
}

int SpellView::getDamageSource() const
{
    auto source = spell->data->damageSource;

    if (!source) {
        return game::emptyCategoryId;
    }

    return (int)source->id;
}


std::vector<ModifierView> SpellView::getWards() const
{
    std::vector<ModifierView> result;

    const auto& wards = spell->data->wards;

    for (const game::CMidgardID* modifierId = wards.bgn; modifierId != wards.end; ++modifierId) {
        auto modifier = hooks::getUnitModifier(modifierId);

        if (!modifier || !modifier->data || !modifier->data->modifier) {
            continue;
        }

        result.push_back(ModifierView(modifier->data->modifier));
    }

    return result;
}

} // namespace bindings