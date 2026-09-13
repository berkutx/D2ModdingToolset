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

#include "menucustombase.h"
#include "button.h"
#include "dialoginterf.h"
#include "dynamiccast.h"
#include "editboxinterf.h"
#include "interfmanager.h"
#include "lobbyrestart.h"
#include "mempool.h"
#include "menuflashwait.h"
#include "menuphase.h"
#include "midgardmsgbox.h"
#include "netcustomservice.h"
#include "popupdialoginterf.h"
#include "textids.h"
#include "utils.h"
#include <spdlog/spdlog.h>

namespace hooks {

CMenuCustomBase::CMenuCustomBase(game::CMenuBase* menu)
    : m_menu{menu}
    , m_menuWait{nullptr}
    , m_peerCallback{this}
    , m_lobbyCallback{this}
{
    auto service = CNetCustomService::get();
    if (service) {
        service->addPeerCallback(&m_peerCallback);
        service->addLobbyCallback(&m_lobbyCallback);
    }
}

CMenuCustomBase::~CMenuCustomBase()
{
    hideWaitDialog();

    auto service = CNetCustomService::get();
    if (service) {
        service->removeLobbyCallback(&m_lobbyCallback);
        service->removePeerCallback(&m_peerCallback);
    }
}

game::CMenuBase* CMenuCustomBase::getMenu() const
{
    return m_menu;
}

game::CInterface* CMenuCustomBase::findOptionalControl(
    const char* controlName,
    const game::TypeDescriptor* controlType) const
{
    using namespace game;

    // Using generic findControl for optional controls to prevent error messages and dynamic_cast to
    // ensure correct control type
    auto dialog = CMenuBaseApi::get().getDialogInterface(getMenu());
    auto control = CDialogInterfApi::get().findControl(dialog, controlName);
    return (CInterface*)RttiApi::get().dynamicCast(control, 0, RttiApi::rtti().CInterfaceType,
                                                   controlType, 0);
}

void CMenuCustomBase::showWaitDialog()
{
    using namespace game;

    if (m_menuWait) {
        spdlog::debug("Error showing wait dialog that is already shown");
        return;
    }

    m_menuWait = (CMenuFlashWait*)Memory::get().allocate(sizeof(CMenuFlashWait));
    CMenuFlashWaitApi::get().constructor(m_menuWait);
    showInterface(m_menuWait);
}

void CMenuCustomBase::hideWaitDialog()
{
    if (m_menuWait) {
        hideInterface(m_menuWait);
        m_menuWait->vftable->destructor(m_menuWait, 1);
        m_menuWait = nullptr;
    }
}

void CMenuCustomBase::onConnectionLost()
{
    using namespace game;

    // The restart coordinator returns after callback dispatch. Its temporary menu can be
    // destroyed before a normal back-to-main popup's button handler uses that menu again.
    if (isLobbyRestartActive()) {
        return;
    }

    hideWaitDialog();

    auto handler = (CMidMsgBoxBackToMainButtonHandler*)Memory::get().allocate(
        sizeof(CMidMsgBoxBackToMainButtonHandler));
    new (handler) CMidMsgBoxBackToMainButtonHandler(m_menu);

    // Connection to the server is lost.
    showMessageBox(getInterfaceText("X005TA0403"), handler, false);
}

void CMenuCustomBase::setUserNameToEditName()
{
    using namespace game;

    const auto& editBoxApi = CEditBoxInterfApi::get();

    auto dialog = CMenuBaseApi::get().getDialogInterface(getMenu());
    auto editName = CDialogInterfApi::get().findEditBox(dialog, "EDIT_NAME");
    editBoxApi.setString(editName, CNetCustomService::get()->getUserName().c_str());
    // editBoxApi.setEditable is bugged - it switches input focus to a next control even if the
    // current control is not focused
    editName->data->editable = false;
}

bool CMenuCustomBase::createRoom(const char* gameName,
                                 const char* scenarioName,
                                 const char* scenarioDescription,
                                 const char* password)
{
    // Showing wait dialog right away because requesting room creation might be lengthy because of
    // game files hashing
    showWaitDialog();

    if (!CNetCustomService::get()->createRoom(gameName, scenarioName, scenarioDescription,
                                              password)) {
        spdlog::debug("Failed to request room creation");
        auto msg{getInterfaceText(textIds().lobby.createRoomRequestFailed.c_str())};
        if (msg.empty()) {
            msg = "Could not request to create a room from the lobby server.";
        }
        hideWaitDialog();
        showMessageBox(msg);
        return false;
    }

    return true;
}

CMenuCustomBase::CPopupDialogCustomBase::CPopupDialogCustomBase(game::CPopupDialogInterf* dialog,
                                                                const char* dialogName)
    : m_dialog{dialog}
    , m_dialogName{dialogName}
{ }

CMenuCustomBase::CMidMsgBoxBackToMainButtonHandler::CMidMsgBoxBackToMainButtonHandler(
    game::CMenuBase* menu)
    : m_menu{menu}
{
    static game::CMidMsgBoxButtonHandlerVftable vftable = {
        (game::CMidMsgBoxButtonHandlerVftable::Destructor)destructor,
        (game::CMidMsgBoxButtonHandlerVftable::Handler)handler,
    };

    this->vftable = &vftable;
}

void __fastcall CMenuCustomBase::CMidMsgBoxBackToMainButtonHandler::destructor(
    CMidMsgBoxBackToMainButtonHandler* thisptr,
    int /*%edx*/,
    char flags)
{ }

void __fastcall CMenuCustomBase::CMidMsgBoxBackToMainButtonHandler::handler(
    CMidMsgBoxBackToMainButtonHandler* thisptr,
    int /*%edx*/,
    game::CMidgardMsgBox* msgBox,
    bool okPressed)
{
    using namespace game;

    hideInterface(msgBox);
    if (msgBox) {
        msgBox->vftable->destructor(msgBox, 1);
    }

    if (okPressed) {
        auto menuPhase = thisptr->m_menu->menuBaseData->menuPhase;
        CMenuPhaseApi::get().transitionToMainOrCloseGame(menuPhase, true);
    }
}

void CMenuCustomBase::PeerCallback::onPacketReceived(DefaultMessageIDTypes type,
                                                     SLNet::RakPeerInterface* peer,
                                                     const SLNet::Packet* packet)
{
    switch (type) {
    case ID_DISCONNECTION_NOTIFICATION:
    case ID_CONNECTION_LOST:
        m_menu->onConnectionLost();
        break;
    }
}

void CMenuCustomBase::LobbyCallback::MessageResult(SLNet::Notification_Client_RemoteLogin* message)
{
    if (message->resultCode == SLNet::L2RC_SUCCESS) {
        // We can possibly become logged out by a remote login of the same account.
        // TODO: find a better way (there seems to be no special notification message).
        if (!CNetCustomService::get()->loggedIn()) {
            m_menu->onConnectionLost();
        }
    }
}

} // namespace hooks
