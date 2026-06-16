#ifndef SPELLVIEW_H
#define SPELLVIEW_H

#include "currencyview.h"
#include "idview.h"
#include "modifierview.h"


#include <optional>
#include <sol/forward.hpp>

namespace sol {
class state;
}

namespace game {
struct TStrategicSpell;
}

namespace bindings {

class UnitImplView;

class SpellView
{
public:
    SpellView(const game::TStrategicSpell* spell);

    static void bind(sol::state& lua);

    IdView getId() const;

    int getCategory() const;

    int getLevel() const;

    CurrencyView getCastingCost() const;

    CurrencyView getBuyCost() const;

    std::optional<UnitImplView> getUnitImpl() const;

    int getRestoreMove() const;

    int getArea() const;

    int getDamage() const;

    int getHeal() const;

    int getGround() const;

    bool getChangeTerrain() const;

    int getAiCategory() const;

    std::optional<ModifierView> getModifier() const;

    int getDamageSource() const;

    std::vector<ModifierView> getWards() const;

private:
    const game::TStrategicSpell* spell;
};

} // namespace bindings

#endif