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

#ifndef MENUCUSTOMBASE_H
#define MENUCUSTOMBASE_H

#include "menubase.h"
#include "midmsgboxbuttonhandler.h"
#include "netcustomservice.h"
#include <Lobby2Message.h>
#include <string>

namespace game {
struct CPopupDialogInterf;
struct CMenuFlashWait;
struct TypeDescriptor;

enum class EditFilter : int;
} // namespace game

namespace hooks {

class CMenuCustomBase
{
public:
    CMenuCustomBase(game::CMenuBase* menu);
    ~CMenuCustomBase();

protected:
    bool hasWaitDialog() const { return m_menuWait != nullptr; }
    game::CMenuBase* getMenu() const;
    game::CInterface* findOptionalControl(const char* controlName,
                                          const game::TypeDescriptor* controlType) const;
    void showWaitDialog();
    void hideWaitDialog();
    void onConnectionLost();
    void setUserNameToEditName();
    bool createRoom(const char* gameName,
                    const char* scenarioName,
                    const char* scenarioDescription,
                    const char* password);

    class CPopupDialogCustomBase
    {
    public:
        CPopupDialogCustomBase(game::CPopupDialogInterf* dialog, const char* dialogName);

    private:
        game::CPopupDialogInterf* m_dialog;
        std::string m_dialogName;
    };

    class CMidMsgBoxBackToMainButtonHandler : public game::CMidMsgBoxButtonHandler
    {
    public:
        CMidMsgBoxBackToMainButtonHandler(game::CMenuBase* menu);

    protected:
        static void __fastcall destructor(CMidMsgBoxBackToMainButtonHandler* thisptr,
                                          int /*%edx*/,
                                          char flags);
        static void __fastcall handler(CMidMsgBoxBackToMainButtonHandler* thisptr,
                                       int /*%edx*/,
                                       game::CMidgardMsgBox* msgBox,
                                       bool okPressed);

    private:
        game::CMenuBase* m_menu;
    };
    assert_offset(CMidMsgBoxBackToMainButtonHandler, vftable, 0);

private:
    class PeerCallback : public NetPeerCallback
    {
    public:
        PeerCallback(CMenuCustomBase* menu)
            : m_menu{menu}
        { }

        ~PeerCallback() override = default;

        void onPacketReceived(DefaultMessageIDTypes type,
                              SLNet::RakPeerInterface* peer,
                              const SLNet::Packet* packet) override;

    private:
        CMenuCustomBase* m_menu;
    };

    class LobbyCallback : public SLNet::Lobby2Callbacks
    {
    public:
        LobbyCallback(CMenuCustomBase* menu)
            : m_menu{menu}
        { }

        ~LobbyCallback() override = default;

        void MessageResult(SLNet::Notification_Client_RemoteLogin* message) override;

    private:
        CMenuCustomBase* m_menu;
    };

    game::CMenuBase* m_menu;
    game::CMenuFlashWait* m_menuWait;
    PeerCallback m_peerCallback;
    LobbyCallback m_lobbyCallback;
};

} // namespace hooks

#endif // MENUCUSTOMBASE_H
