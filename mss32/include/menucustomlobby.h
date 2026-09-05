/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2021 Vladimir Makeev.
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

#ifndef MENUCUSTOMLOBBY_H
#define MENUCUSTOMLOBBY_H

#include "d2vector.h"
#include "menubase.h"
#include "menucustombase.h"
#include "midmsgboxbuttonhandler.h"
#include "popupdialoginterf.h"
#include "uievent.h"
#include <DS_List.h>
#include <Lobby2Message.h>
#include <RoomsPlugin.h>
#include <deque>
#include <string>
#include <vector>

namespace game {
struct NetMsgEntryData;
struct CMenusAnsInfoMsg;
struct CGameVersionMsg;
} // namespace game

namespace hooks {

class CMenuCustomLobby;

using RestartJoinCompletion = void (*)(bool success);

/** Creates a message-only lobby menu used while reconnecting to a restarted match. */
game::CMenuBase* __stdcall createRestartJoinMenu(game::CMenuPhase* menuPhase);

/**
 * Starts the native session handshake without joining a RoomsPlugin room.
 * The completion callback is invoked exactly once after a started handshake succeeds or fails.
 */
bool beginRestartJoin(CMenuCustomLobby* menu,
                      const SLNet::RakNetGUID& hostGuid,
                      const char* roomName,
                      int maxPlayers,
                      RestartJoinCompletion completion);

class CMenuCustomLobby
    : public game::CMenuBase
    , public CMenuCustomBase
{
public:
    static constexpr char dialogName[] = "DLG_CUSTOM_LOBBY";
    static constexpr char roomPasswordDialogName[] = "DLG_ROOM_PASSWORD";
    static constexpr char transitionFromMainName[] = "TRANS_MAIN2CUSTOMLOBBY";
    static constexpr char transitionFromBlackName[] = "TRANS_BLACK2CUSTOMLOBBY";
    static constexpr char helpDialogName[] = "DLG_CUSTOM_HELP";
    static constexpr std::uint32_t roomsUpdateEventInterval{5000};
    static constexpr std::uint32_t usersUpdateEventInterval{5000};
    static constexpr std::uint32_t chatMessageMaxLength{40};
    static constexpr std::uint32_t chatMessageMaxCount{100};
    static constexpr std::uint32_t chatMessageMaxStock{3};
    static constexpr std::uint32_t chatMessageRegenEventInterval{3000};

    CMenuCustomLobby(game::CMenuPhase* menuPhase);
    ~CMenuCustomLobby();

protected:
    // CInterface
    static void __fastcall destructor(CMenuCustomLobby* thisptr, int /*%edx*/, char flags);
    static int __fastcall handleKeyboard(CMenuCustomLobby* thisptr, int /*%edx*/, int key, int a3);

    struct RoomInfo;

    void initializeNetMsgEntries();
    void initializeChatControls();
    void initializeUserControls();
    void initializeUsersControls();
    void initializeRoomsControls();
    void showRoomPasswordDialog();
    void hideRoomPasswordDialog();
    void updateRooms(DataStructures::List<SLNet::RoomDescriptor*>& roomDescriptors);
    const RoomInfo* getSelectedRoom();
    void updateTxtRoomInfo(int roomIndex);
    void updateListBoxRoomsRow(int rowIndex,
                               bool selected,
                               const game::CMqRect* lineArea,
                               game::ImagePointList* contents);
    void updateListBoxRoomsListRow(int rowIndex,
                                   bool selected,
                                   const game::CMqRect* lineArea,
                                   game::ImagePointList* contents);
    void updateListBoxRoomsTableRow(int rowIndex,
                                    bool selected,
                                    const game::CMqRect* lineArea,
                                    game::ImagePointList* contents);
    void addListBoxRoomsItemContent(const char* text,
                                    const char* imageName,
                                    bool showImage,
                                    const game::CMqRect* lineArea,
                                    game::ImagePointList* contents);
    void addListBoxRoomsCellText(const char* columnName,
                                 const char* value,
                                 const game::CMqRect* lineArea,
                                 game::ImagePointList* contents);
    void addListBoxRoomsCellImage(const char* columnName,
                                  const char* imageName,
                                  const game::CMqRect* lineArea,
                                  game::ImagePointList* contents);
    void addListBoxRoomsSelectionOutline(const game::CMqRect* lineArea,
                                         game::ImagePointList* contents);
    void updateListBoxUsersRow(int rowIndex,
                               bool selected,
                               const game::CMqRect* lineArea,
                               game::ImagePointList* contents);
    game::IMqImage2* getUserImage(const CNetCustomService::UserInfo& user, bool left, bool big);
    std::string getShortenedUserNameInList(const char* name,
                                           const char* shortenedMark,
                                           int textAreaWidth);
    void joinServer(SLNet::RoomDescriptor* roomDescriptor);
    bool joinServer(const SLNet::RakNetGUID& hostGuid, const char* roomName, int maxPlayers);
    RestartJoinCompletion takeRestartJoinCompletion();
    void completeRestartJoin(bool success);
    void addChatMessage(CNetCustomService::ChatMessage message);
    void sendChatMessage();
    void updateUsers(std::vector<CNetCustomService::UserInfo> users);
    void updateChat(std::vector<CNetCustomService::ChatMessage> messages);
    void updateListBoxChat();

    static RoomInfo getRoomInfo(SLNet::RoomDescriptor* roomDescriptor);
    static SLNet::RoomMemberDescriptor* getRoomModerator(
        DataStructures::List<SLNet::RoomMemberDescriptor>& roomMembers);

    static void __fastcall createBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall loadBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall joinBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall backBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall sendBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall helpBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall roomsUpdateEventCallback(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall usersUpdateEventCallback(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall chatMessageRegenEventCallback(CMenuCustomLobby* thisptr, int /*%edx*/);
    static void __fastcall listBoxRoomsUpdateHandler(CMenuCustomLobby* thisptr,
                                                     int /*%edx*/,
                                                     int selectedIndex);
    static void __fastcall listBoxRoomsDisplayHandler(CMenuCustomLobby* thisptr,
                                                      int /*%edx*/,
                                                      game::ImagePointList* contents,
                                                      const game::CMqRect* lineArea,
                                                      int index,
                                                      bool selected);
    static void __fastcall listBoxUsersDisplayHandler(CMenuCustomLobby* thisptr,
                                                      int /*%edx*/,
                                                      game::ImagePointList* contents,
                                                      const game::CMqRect* lineArea,
                                                      int index,
                                                      bool selected);
    static void __fastcall listBoxUsersDisplayTextHandler(CMenuCustomLobby* thisptr,
                                                          int /*%edx*/,
                                                          game::String* string,
                                                          bool,
                                                          int index);
    static void __fastcall listBoxChatDisplayHandler(CMenuCustomLobby* thisptr,
                                                     int /*%edx*/,
                                                     game::String* string,
                                                     bool,
                                                     int index);
    static bool __fastcall gameVersionMsgHandler(CMenuCustomLobby* menu,
                                                 int /*%edx*/,
                                                 const game::CGameVersionMsg* message,
                                                 std::uint32_t idFrom);
    static bool __fastcall ansInfoMsgHandler(CMenuCustomLobby* menu,
                                             int /*%edx*/,
                                             const game::CMenusAnsInfoMsg* message,
                                             std::uint32_t idFrom);

    struct RoomInfo
    {
        SLNet::RoomID id;
        std::string hostName;
        std::string gameName;
        std::string password;
        std::string gameFilesHash;
        std::string templateName;
        std::string templateHash;
        std::string gameVersion;
        std::string scenarioName;
        std::string scenarioDescription;
        std::vector<std::string> clientNames;
        int usedSlots;
        int totalSlots;
    };

    class PeerCallback : public NetPeerCallback
    {
    public:
        PeerCallback(CMenuCustomLobby* menu)
            : m_menu{menu}
        { }

        ~PeerCallback() override = default;

        void onPacketReceived(DefaultMessageIDTypes type,
                              SLNet::RakPeerInterface* peer,
                              const SLNet::Packet* packet) override;

    private:
        CMenuCustomLobby* m_menu;
    };

    class RoomsCallback : public SLNet::RoomsCallback
    {
    public:
        RoomsCallback(CMenuCustomLobby* menu)
            : m_menu{menu}
        { }

        ~RoomsCallback() override = default;

        void JoinByFilter_Callback(const SLNet::SystemAddress& senderAddress,
                                   SLNet::JoinByFilter_Func* callResult) override;

        void SearchByFilter_Callback(const SLNet::SystemAddress& senderAddress,
                                     SLNet::SearchByFilter_Func* callResult) override;

    private:
        CMenuCustomLobby* m_menu;
    };

    struct CConfirmBackMsgBoxButtonHandler : public game::CMidMsgBoxButtonHandler
    {
    public:
        CConfirmBackMsgBoxButtonHandler(CMenuCustomLobby* menu);

    protected:
        static void __fastcall destructor(CConfirmBackMsgBoxButtonHandler* thisptr,
                                          int /*%edx*/,
                                          char flags);
        static void __fastcall handler(CConfirmBackMsgBoxButtonHandler* thisptr,
                                       int /*%edx*/,
                                       game::CMidgardMsgBox* msgBox,
                                       bool okPressed);

    private:
        CMenuCustomLobby* m_menu;
    };
    assert_offset(CConfirmBackMsgBoxButtonHandler, vftable, 0);

    struct CRoomPasswordInterf
        : public game::CPopupDialogInterf
        , public CPopupDialogCustomBase
    {
        CRoomPasswordInterf(CMenuCustomLobby* menu);

    protected:
        static void __fastcall okBtnHandler(CRoomPasswordInterf* thisptr, int /*%edx*/);
        static void __fastcall cancelBtnHandler(CRoomPasswordInterf* thisptr, int /*%edx*/);

    private:
        CMenuCustomLobby* m_menu;
    };
    assert_offset(CRoomPasswordInterf, vftable, 0);

     struct CHelpInterf
        : public game::CPopupDialogInterf
        , public CPopupDialogCustomBase
    {
        CHelpInterf(CMenuCustomLobby * menu);

    protected:
        static void __fastcall closeBtnHandler(CHelpInterf * thisptr, int /*%edx*/);
        static void __fastcall listBoxDisplayHandler(CHelpInterf * thisptr, int /*%edx*/,
                                                     game::ImagePointList* contents,
                                                     const game::CMqRect* lineArea, int index,
                                                     bool selected);
        static void __fastcall listBoxClickHandler(CHelpInterf * thisptr, int /*edx*/,
                                                   int selectedIndex);

    private:
        CMenuCustomLobby* m_menu;
    };
    assert_offset(CHelpInterf, vftable, 0);

private:
    CMenuCustomLobby(game::CMenuPhase* menuPhase, bool restartJoin);

    friend game::CMenuBase* __stdcall createRestartJoinMenu(game::CMenuPhase* menuPhase);
    friend bool beginRestartJoin(CMenuCustomLobby* menu,
                                 const SLNet::RakNetGUID& hostGuid,
                                 const char* roomName,
                                 int maxPlayers,
                                 RestartJoinCompletion completion);

    CHelpInterf* m_helpDialog{};
    PeerCallback m_peerCallback;
    RoomsCallback m_roomsCallback;
    game::UiEvent m_roomsUpdateEvent;
    std::vector<RoomInfo> m_rooms;
    game::UiEvent m_usersUpdateEvent;
    std::vector<CNetCustomService::UserInfo> m_users;
    game::Vector<game::SmartPtr<game::IMqImage2>> m_userIcons;
    const char* m_usersListBoxName;
    game::NetMsgEntryData** m_netMsgEntryData;
    CRoomPasswordInterf* m_roomPasswordDialog;
    SLNet::RoomID m_joiningRoomId;
    std::string m_joiningRoomPassword;
    std::deque<CNetCustomService::ChatMessage> m_chatMessages;
    std::uint32_t m_chatMessageStock;
    game::UiEvent m_chatMessageRegenEvent;
    bool m_restartJoin;
    bool m_restartJoinPending;
    RestartJoinCompletion m_restartJoinCompletion;
};

assert_offset(CMenuCustomLobby, vftable, 0);

} // namespace hooks

#endif // MENUCUSTOMLOBBY_H
