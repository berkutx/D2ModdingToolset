/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "menurestartnative.h"
#include "button.h"
#include "dialoginterf.h"
#include "dynamiccast.h"
#include "menubase.h"
#include "menuphase.h"
#include "midgard.h"
#include "netmessages.h"
#include <cstring>

namespace hooks {

bool restartNativeSupported()
{
    return game::NetMessagesApi::getMenusReqStartInfoVftable() != nullptr;
}

bool pressRestartNativeStartButton(game::CMenuPhase* phase)
{
    using namespace game;

    if (!restartNativeSupported() || !phase || !phase->data
        || phase->data->currentPhase != MenuPhase::LobbyHost || !phase->data->currentMenu) {
        return false;
    }

    // LobbyHost, LobbyJoin and the subsequent wait screen share phase 16.
    // Check the concrete native menu before accessing CMenuBase or its controls.
    const auto* type = (*RttiApi::get().typeIdOperator)(phase->data->currentMenu);
    if (!type || (std::strcmp(type->name, ".?AVCMenuLobbyHost@@") != 0
                  && std::strcmp(type->name, ".?AVCMenuLobbyJoin@@") != 0)) {
        return false;
    }

    auto* menu = reinterpret_cast<CMenuBase*>(phase->data->currentMenu);
    auto* dialog = CMenuBaseApi::get().getDialogInterface(menu);
    if (!dialog) {
        return false;
    }
    auto* button = CDialogInterfApi::get().findButton(dialog, "BTN_OK");
    if (!button || !button->buttonData || !button->vftable->isEnabled(button)) {
        return false;
    }
    auto* action = button->buttonData->onClickedFunctor.data;
    if (!action) {
        return false;
    }

    // Joiners' native button becomes enabled only after the host starts.
    // Its existing callback sends ReqStartGame and enters the native wait screen.
    action->vftable->runCallback(action);
    return true;
}

bool requestRestartSetupInfo()
{
    using namespace game;

    auto* vftable = NetMessagesApi::getMenusReqStartInfoVftable();
    if (!vftable) {
        return false;
    }
    const auto& api = CMidgardApi::get();
    auto* midgard = api.instance();
    if (!midgard || !midgard->data || !midgard->data->netPlayerClientPtr) {
        return false;
    }

    CMenusReqStartInfoMsg message;
    message.vftable = vftable;
    return api.sendNetMsgToServer(midgard, &message);
}

} // namespace hooks
