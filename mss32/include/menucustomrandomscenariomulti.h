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

#ifndef MENUCUSTOMRANDOMSCENARIOMULTI_H
#define MENUCUSTOMRANDOMSCENARIOMULTI_H

#include "dynamiccast.h"
#include "menucustombase.h"
#include "menurandomscenariomulti.h"
#include <RoomsPlugin.h>

namespace hooks {

class CMenuCustomRandomScenarioMulti
    : public CMenuRandomScenarioMulti
    , public CMenuCustomBase
{
public:
    CMenuCustomRandomScenarioMulti(game::CMenuPhase* menuPhase);
    CMenuCustomRandomScenarioMulti(game::CMenuPhase* menuPhase, bool restartGeneration);
    ~CMenuCustomRandomScenarioMulti();

protected:
    static game::RttiInfo<game::CMenuBaseVftable> rttiInfo;

    static void createRoomAndServer(CMenuCustomRandomScenarioMulti* menu);

    // CInterface
    static void __fastcall destructor(CMenuCustomRandomScenarioMulti* thisptr,
                                      int /*%edx*/,
                                      char flags);

    class RoomsCallback : public SLNet::RoomsCallback
    {
    public:
        RoomsCallback(CMenuCustomRandomScenarioMulti* menu)
            : m_menu{menu}
        { }

        void CreateRoom_Callback(const SLNet::SystemAddress& senderAddress,
                                 SLNet::CreateRoom_Func* callResult) override;

    private:
        CMenuCustomRandomScenarioMulti* m_menu;
    };

private:
    RoomsCallback m_roomsCallback;
    bool m_roomsCallbackRegistered{};
};

assert_offset(CMenuCustomRandomScenarioMulti, vftable, 0);

/** Menu factory used by the automatic random-scenario restart transition. */
game::CMenuBase* __stdcall createRestartScenarioMenu(game::CMenuPhase* menuPhase);

} // namespace hooks

#endif // MENUCUSTOMRANDOMSCENARIOMULTI_H
