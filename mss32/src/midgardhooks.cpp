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

#include "midgardhooks.h"
#include "interface.h"
#include "interfmanager.h"
#include "lobbyrestart.h"
#include "mempool.h"
#include "menucustomlobby.h"
#include "menuphase.h"
#include "midgard.h"
#include "midstart.h"
#include "netcustomservice.h"
#include "originalfunctions.h"
#include <spdlog/spdlog.h>

namespace hooks {

void __fastcall midgardStartMenuMessageCallbackHooked(game::CMidgard* thisptr,
                                                      int /*%edx*/,
                                                      unsigned int wParam,
                                                      long lParam)
{
    using namespace game;

    const auto& midgardApi = CMidgardApi::get();
    const auto& menuPhaseApi = CMenuPhaseApi::get();

    auto service = CNetCustomService::get();
    bool isLoggedInCustomLobby = service && service->loggedIn();
    if (isLoggedInCustomLobby) {
        midgardApi.clearNetworkState(thisptr);
    } else {
        midgardApi.clearNetworkStateAndService(thisptr);
    }
    finishLobbyRestartMenuReturn();

    auto* data = thisptr->data;
    auto& midStart = data->midStart;
    if (midStart) {
        CMidStartApi::get().destructor(midStart);
        Memory::get().freeNonZero(midStart);
        midStart = nullptr;
    }

    auto& waitInterface = data->waitInterface;
    if (waitInterface) {
        auto interfManager = data->interfManager.data;
        interfManager->CInterfManagerImpl::CInterfManager::vftable->hideInterface(interfManager,
                                                                                  waitInterface);
        waitInterface->vftable->destructor(waitInterface, 1);
        waitInterface = nullptr;
    }

    auto menuPhase = (CMenuPhase*)Memory::get().allocate(sizeof(CMenuPhase));
    menuPhaseApi.constructor(menuPhase, wParam != 0, lParam == 0);

    if (data->menuPhase) {
        data->menuPhase->IMqNetSystem::vftable->destructor(data->menuPhase, 1);
    }
    data->menuPhase = menuPhase;

    if (isLoggedInCustomLobby && isLobbyRestartMenuTransition()) {
        enterLobbyRestartMenu(menuPhase);
    } else if (lParam) {
        menuPhaseApi.switchToQuickLoad(menuPhase, (const char*)lParam);
        Memory::get().freeNonZero((void*)lParam);
    } else if (isLoggedInCustomLobby) {
        auto phaseData = menuPhase->data;
        menuPhaseApi.showFullScreenAnimation(menuPhase, &phaseData->currentPhase,
                                             &phaseData->interfManager, &phaseData->currentMenu,
                                             MenuPhase::Back2CustomLobby,
                                             CMenuCustomLobby::transitionFromBlackName);
    } else if (data->directPlayLobbySession) {
        menuPhaseApi.switchToQuickLobby(menuPhase);
    } else if (data->lobbySessionExists) {
        menuPhaseApi.switchToGameSpy(menuPhase);
    } else {
        menuPhaseApi.transitionToMain(menuPhase, true);
    }
}

void resetCommandSequenceGlobalCounters()
{
    using namespace game;

    // Fixes leader "teleportation" issue
    uint32_t& lastCommandSequenceNumber = *CMidCommandQueue2Api::get().lastCommandSequenceNumber;
    uint32_t& nextCommandSequenceNumber = *CCommandMsgApi::get().nextCommandSequenceNumber;
    spdlog::debug(
        __FUNCTION__ ": lastCommandSequenceNumber = {:d}, nextCommandSequenceNumber = {:d}",
        lastCommandSequenceNumber, nextCommandSequenceNumber);
    lastCommandSequenceNumber = 0;
    nextCommandSequenceNumber = 1;
}

void __fastcall midgardClearNetworkStateHooked(game::CMidgard* thisptr, int /*%edx*/)
{
    spdlog::debug(__FUNCTION__);

    getOriginalFunctions().midgardClearNetworkState(thisptr);

    // Make sure that there are no peer messages remain to process.
    // Though it does not guarantee that a new ones will not arrive shortly after.
    auto service = CNetCustomService::get();
    if (service) {
        service->processPeerMessages();
    }

    resetCommandSequenceGlobalCounters();
}

void __fastcall midgardClearNetworkStateAndServiceHooked(game::CMidgard* thisptr, int /*%edx*/)
{
    spdlog::debug(__FUNCTION__);

    getOriginalFunctions().midgardClearNetworkStateAndService(thisptr);

    resetCommandSequenceGlobalCounters();
}

} // namespace hooks
