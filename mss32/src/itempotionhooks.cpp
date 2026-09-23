#include "itempotionhooks.h"
#include "dbtable.h"
#include "d2string.h"
#include "mempool.h"
#include "originalfunctions.h"
#include <limits>
#include <idview.h>

namespace hooks {

static void readPotionExtraFields(int* itemCat,
                                  int* hpPotion,
                                  game::CMidgardID* modPotion,
                                  game::CDBTable* dbTable)
{
    using namespace game;

    const auto& db = CDBTableApi::get();

    int item_Cat = 0;
    int hp_Potion = 0;
    CMidgardID mod_Potion = invalidId;

    db.readIntWithBoundsCheck(&item_Cat, dbTable, "ITEM_CAT", 0, 14);
    db.readIntWithBoundsCheck(&hp_Potion, dbTable, "HP_POTION", std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max());
    // Healing/revival potions may leave this optional DBF field blank. Do not
    // use a native exception for absence: debug mode reports it before catch.
    String modifierText{};
    db.readString(&modifierText, dbTable, "MOD_POTION");
    const char* value = modifierText.string;
    while (value && *value == ' ')
        ++value;
    const bool hasModifier = value && *value != '\0';
    StringApi::get().free(&modifierText);
    if (hasModifier) {
        db.readId(&mod_Potion, dbTable, "MOD_POTION");
    }

    *itemCat = item_Cat;
    *hpPotion = hp_Potion;
    *modPotion = mod_Potion;
}

game::CItemPotionHeal* __fastcall itemPotionHealCtorHooked(game::CItemPotionHeal* thisptr,
                                                           int /*%edx*/,
                                                           game::CDBTable* dbTable,
                                                           const game::GlobalData** globalData)
{
    using namespace game;

    const auto& db = CDBTableApi::get();

    auto* patched = (CItemPotionHealPatched*)game::Memory::get().allocate(
        sizeof(CItemPotionHealPatched));
    getOriginalFunctions().itemPotionHealCtor(patched, dbTable, globalData);
    readPotionExtraFields(&patched->itemCat, &patched->hpPotion, &patched->modPotion, dbTable);

    return patched;
}

game::CItemPotionBoostTemp* __fastcall itemPotionBoostTempCtorHooked(
    game::CItemPotionBoostTemp* thisptr,
    int /*%edx*/,
    game::CDBTable* dbTable,
    const game::GlobalData** globalData)
{
    auto* patched = (CItemPotionBoostTempPatched*)game::Memory::get().allocate(
        sizeof(CItemPotionBoostTempPatched));
    getOriginalFunctions().itemPotionBoostTempCtor(patched, dbTable, globalData);
    readPotionExtraFields(&patched->itemCat, &patched->hpPotion, &patched->modPotion, dbTable);

    return patched;
}

game::CItemPotionBoostPerm* __fastcall itemPotionBoostPermCtorHooked(
    game::CItemPotionBoostPerm* thisptr,
    int /*%edx*/,
    game::CDBTable* dbTable,
    const game::GlobalData** globalData)
{
    auto* patched = (CItemPotionBoostPermPatched*)game::Memory::get().allocate(
        sizeof(CItemPotionBoostPermPatched));
    getOriginalFunctions().itemPotionBoostPermCtor(patched, dbTable, globalData);
    readPotionExtraFields(&patched->itemCat, &patched->hpPotion, &patched->modPotion, dbTable);

    return patched;
}

game::CItemPotionRevive* __fastcall itemPotionReviveCtorHooked(game::CItemPotionRevive* thisptr,
                                                               int /*%edx*/,
                                                               game::CDBTable* dbTable,
                                                               const game::GlobalData** globalData)
{
    auto* patched = (CItemPotionRevivePatched*)game::Memory::get().allocate(
        sizeof(CItemPotionRevivePatched));
    getOriginalFunctions().itemPotionReviveCtor(patched, dbTable, globalData);
    readPotionExtraFields(&patched->itemCat, &patched->hpPotion, &patched->modPotion, dbTable);

    return patched;
}

} // namespace hooks
