/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2024 Stanislav Egorov.
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

#ifndef MENUCUSTOMNEWSKIRMISHMULTI_H
#define MENUCUSTOMNEWSKIRMISHMULTI_H

#include "menucustombase.h"
#include "menunewskirmishmulti.h"
#include <RoomsPlugin.h>

namespace game {
struct ScenarioData;
} // namespace game

namespace hooks {

class CMenuCustomNewSkirmishMulti
    : public game::CMenuNewSkirmishMulti
    , public CMenuCustomBase
{
public:
    CMenuCustomNewSkirmishMulti(game::CMenuPhase* menuPhase);
    ~CMenuCustomNewSkirmishMulti();

protected:
    // CInterface
    static void __fastcall destructor(CMenuCustomNewSkirmishMulti* thisptr,
                                      int /*%edx*/,
                                      char flags);

    static void __fastcall loadBtnHandler(CMenuCustomNewSkirmishMulti* thisptr, int /*%edx*/);

    /** Sets room options controls (TOG_RANKED/TOG_SIM_DAYS_LABEL/SPIN_SIM_DAYS/
     * TOG_UNLOCK_GUI) from CNetCustomService. */
    void initializeRoomOptionsControls();
    /** Reads room options controls state back into CNetCustomService. */
    void readRoomOptionsControls();

    const game::ScenarioData* getSelectedScenario();
    bool isGameAndPlayerNamesValid();

    class RoomsCallback : public SLNet::RoomsCallback
    {
    public:
        RoomsCallback(CMenuCustomNewSkirmishMulti* menu)
            : m_menu{menu}
        { }

        void CreateRoom_Callback(const SLNet::SystemAddress& senderAddress,
                                 SLNet::CreateRoom_Func* callResult) override;

    private:
        CMenuCustomNewSkirmishMulti* m_menu;
    };

private:
    RoomsCallback m_roomsCallback;
};

assert_offset(CMenuCustomNewSkirmishMulti, vftable, 0);

} // namespace hooks

#endif // MENUCUSTOMNEWSKIRMISHMULTI_H
