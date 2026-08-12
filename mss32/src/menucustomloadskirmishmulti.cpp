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

#include "menucustomloadskirmishmulti.h"
#include "mempool.h"
#include "originalfunctions.h"
#include "textids.h"
#include "utils.h"
#include <spdlog/spdlog.h>

namespace hooks {

game::RttiInfo<game::CMenuLoadSkirmishMultiVftable> CMenuCustomLoadSkirmishMulti::rttiInfo = {};

CMenuCustomLoadSkirmishMulti* CMenuCustomLoadSkirmishMulti::cast(CMenuBase* menu)
{
    return menu && menu->vftable == &rttiInfo.vftable ? (CMenuCustomLoadSkirmishMulti*)menu
                                                      : nullptr;
}

CMenuCustomLoadSkirmishMulti::CMenuCustomLoadSkirmishMulti(game::CMenuPhase* menuPhase)
    : CMenuCustomBase{this}
    , m_roomsCallback{this}
{
    using namespace game;

    CMenuLoadSkirmishMultiApi::get().constructor(this, menuPhase);

    if (rttiInfo.locator == nullptr) {
        replaceRttiInfo(rttiInfo, (CMenuLoadSkirmishMultiVftable*)this->CMenuBase::vftable);
        rttiInfo.vftable.destructor = (CInterfaceVftable::Destructor)&destructor;
    }
    this->CMenuBase::vftable = &rttiInfo.vftable;

    setUserNameToEditName();

    CNetCustomService::get()->addRoomsCallback(&m_roomsCallback);
}

CMenuCustomLoadSkirmishMulti::~CMenuCustomLoadSkirmishMulti()
{
    using namespace game;

    auto service = CNetCustomService::get();
    if (service) {
        service->removeRoomsCallback(&m_roomsCallback);
    }

    CMenuLoadSkirmishMultiApi::get().destructor(this);
}

void CMenuCustomLoadSkirmishMulti::createRoomAndServer()
{
    using namespace game;

    auto vftable = (CMenuLoadVftable*)((CMenuBase*)this)->vftable;
    auto phaseData = this->menuBaseData->menuPhase->data;
    CNetCustomService::get()->getRoomOptions().ranked = false;
    createRoom(vftable->getGameName(this), phaseData->scenarioName, phaseData->scenarioDescription,
               vftable->getPassword(this));
}

void __fastcall CMenuCustomLoadSkirmishMulti::destructor(CMenuCustomLoadSkirmishMulti* thisptr,
                                                         int /*%edx*/,
                                                         char flags)
{
    thisptr->~CMenuCustomLoadSkirmishMulti();

    if (flags & 1) {
        spdlog::debug("Free CMenuCustomLoadSkirmishMulti memory");
        game::Memory::get().freeNonZero(thisptr);
    }
}

void CMenuCustomLoadSkirmishMulti::RoomsCallback::CreateRoom_Callback(
    const SLNet::SystemAddress& senderAddress,
    SLNet::CreateRoom_Func* callResult)
{
    using namespace game;

    m_menu->hideWaitDialog();

    switch (auto resultCode = callResult->resultCode) {
    case SLNet::REC_SUCCESS: {
        getOriginalFunctions().menuLoadCreateServer(m_menu);
        break;
    }

    default: {
        auto msg{getInterfaceText(textIds().lobby.createRoomFailed.c_str())};
        if (msg.empty()) {
            msg = "Could not create a room.\n%ERROR%";
        }
        replace(msg, "%ERROR%", SLNet::RoomsErrorCodeDescription::ToEnglish(resultCode));
        showMessageBox(msg);
        break;
    }
    }
}

} // namespace hooks
