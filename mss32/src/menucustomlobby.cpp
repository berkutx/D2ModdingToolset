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

#include "menucustomlobby.h"
#ifdef D2_TESTDRV
#include "testdrv/lobbychatreporter.h"
#endif
#include "autodialog.h"
#include "button.h"
#include "dialoginterf.h"
#include "dynamiccast.h"
#include "formattedtext.h"
#include "globaldata.h"
#include "image2fill.h"
#include "image2outline.h"
#include "image2text.h"
#include "imageptrvector.h"
#include "interfaceutils.h"
#include "interfmanager.h"
#include "listbox.h"
#include "mempool.h"
#include "menuphase.h"
#include "midgard.h"
#include "midgardmsgbox.h"
#include "netcustomplayerclient.h"
#include "netcustomsession.h"
#include "netmessages.h"
#include "netmsgcallbacks.h"
#include "netmsgmapentry.h"
#include "originalfunctions.h"
#include "pictureinterf.h"
#include "racelist.h"
#include "restrictions.h"
#include "textboxinterf.h"
#include "textids.h"
#include "uimanager.h"
#include "utils.h"
#include <unitutils.h>
#include <spdlog/spdlog.h>

namespace hooks {

    static DataStructures::Table::Cell* getPropertySafe(SLNet::RoomDescriptor* room, const char* name)
{
    auto row = room->roomProperties.GetRowByIndex(0, nullptr);
    if (!row)
        return nullptr;

    auto index = room->roomProperties.ColumnIndex(name);

    if (index == (unsigned)-1 || index >= row->cells.Size())
        return nullptr;

    return room->GetProperty((int)index);
}


CMenuCustomLobby::CMenuCustomLobby(game::CMenuPhase* menuPhase)
    : CMenuCustomBase(this)
    , m_peerCallback(this)
    , m_roomsCallback(this)
    , m_netMsgEntryData(nullptr)
    , m_roomPasswordDialog(nullptr)
    , m_chatMessageStock(chatMessageMaxStock)
    , m_userIcons{}
    , m_usersListBoxName{nullptr}
    , m_roomsUpdateEvent{}
    , m_usersUpdateEvent{}
    , m_chatMessageRegenEvent{}
{
    using namespace game;

    const auto& menuBaseApi = CMenuBaseApi::get();

    menuBaseApi.constructor(this, menuPhase);

    static RttiInfo<CMenuBaseVftable> rttiInfo = {};
    if (rttiInfo.locator == nullptr) {
        // Reuse object locator for our custom this.
        // We only need it for dynamic_cast<CAnimInterf>() to work properly without exceptions
        // when the game exits main loop and checks for current CInterface.
        // See DestroyAnimInterface() in IDA.
        replaceRttiInfo(rttiInfo, this->vftable);
        rttiInfo.vftable.destructor = (CInterfaceVftable::Destructor)&destructor;
        // BTN_SEND is enough, no need for explicit handling
        // rttiInfo.vftable.handleKeyboard = (CInterfaceVftable::HandleKeyboard)&handleKeyboard;
    }
    this->vftable = &rttiInfo.vftable;

    menuBaseApi.createMenu(this, dialogName);

    auto service = CNetCustomService::get();
    initializeNetMsgEntries();
    service->addPeerCallback(&m_peerCallback);
    service->addRoomsCallback(&m_roomsCallback);

    initializeChatControls();
    initializeUserControls();
    initializeUsersControls();
    initializeRoomsControls();
}

CMenuCustomLobby ::~CMenuCustomLobby()
{
    using namespace game;

    const auto& uiEventApi = UiEventApi::get();

    hideRoomPasswordDialog();

    uiEventApi.destructor(&m_chatMessageRegenEvent);
    uiEventApi.destructor(&m_usersUpdateEvent);
    uiEventApi.destructor(&m_roomsUpdateEvent);

    ImagePtrVectorApi::get().destructor(&m_userIcons);

    auto service = CNetCustomService::get();
    if (service) {
        service->removeRoomsCallback(&m_roomsCallback);
        service->removePeerCallback(&m_peerCallback);
    }

    if (m_netMsgEntryData) {
        spdlog::debug("Delete custom lobby menu netMsgEntryData");
        NetMsgApi::get().freeEntryData(m_netMsgEntryData);
    }

    CMenuBaseApi::get().destructor(this);
}

void __fastcall CMenuCustomLobby::destructor(CMenuCustomLobby* thisptr, int /*%edx*/, char flags)
{
    thisptr->~CMenuCustomLobby();

    if (flags & 1) {
        spdlog::debug("Free CMenuCustomLobby memory");
        game::Memory::get().freeNonZero(thisptr);
    }
}

// See CMenuLobbyHandleKeyboard (Akella 0x4e3a98)
int __fastcall CMenuCustomLobby::handleKeyboard(CMenuCustomLobby* thisptr,
                                                int /*%edx*/,
                                                int key,
                                                int a3)
{
    using namespace game;

    auto result = CMenuBaseApi::vftable()->handleKeyboard((CInterface*)thisptr, key, a3);
    if (key == VK_RETURN) {
        thisptr->sendChatMessage();
    }
    return result;
}

void CMenuCustomLobby::initializeNetMsgEntries()
{
    using namespace game;

    const auto& netMsgApi = NetMsgApi::get();
    auto& netMsgMapEntryApi = CNetMsgMapEntryApi::get();

    netMsgApi.allocateEntryData(this->menuBaseData->menuPhase, &this->m_netMsgEntryData);

    auto infoCallback = (CNetMsgMapEntryApi::Api::MenusAnsInfoCallback)ansInfoMsgHandler;
    auto entry = netMsgMapEntryApi.allocateMenusAnsInfoEntry(this, infoCallback);
    netMsgApi.addEntry(this->m_netMsgEntryData, entry);

    auto versionCallback = (CNetMsgMapEntryApi::Api::GameVersionCallback)gameVersionMsgHandler;
    entry = netMsgMapEntryApi.allocateGameVersionEntry(this, versionCallback);
    netMsgApi.addEntry(this->m_netMsgEntryData, entry);
}

void CMenuCustomLobby::initializeChatControls()
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& smartPtrApi = SmartPointerApi::get();
    const auto& menuBaseApi = CMenuBaseApi::get();
    const auto& listBoxApi = CListBoxInterfApi::get();
    const auto& editBoxApi = CEditBoxInterfApi::get();

    auto dialog = menuBaseApi.getDialogInterface(this);

    auto listBoxChat = (CListBoxInterf*)findOptionalControl("LBOX_CHAT", rtti.CListBoxInterfType);
    if (listBoxChat) {
        using TextCallback = CMenuBaseApi::Api::ListBoxDisplayTextCallback;
        TextCallback callback{(TextCallback::Callback)listBoxChatDisplayHandler};

        SmartPointer functor;
        menuBaseApi.createListBoxDisplayTextFunctor(&functor, 0, this, &callback);
        listBoxApi.assignDisplayTextFunctor(dialog, "LBOX_CHAT", dialogName, &functor, true);
        smartPtrApi.createOrFreeNoDtor(&functor, nullptr);

        // Request saved chat messages
        CNetCustomService::get()->queryChatMessages();
    }

    auto editBoxChat = (CEditBoxInterf*)findOptionalControl("EDIT_CHAT", rtti.CEditBoxInterfType);
    if (editBoxChat) {
        editBoxApi.setInputLength(editBoxChat, chatMessageMaxLength);

        createTimerEvent(&m_chatMessageRegenEvent, this, chatMessageRegenEventCallback,
                         chatMessageRegenEventInterval);
    }

    auto btnSend = (CButtonInterf*)findOptionalControl("BTN_SEND", rtti.CButtonInterfType);
    if (btnSend) {
        setButtonCallback(dialog, "BTN_SEND", sendBtnHandler, this);
    }
}

void CMenuCustomLobby::initializeUserControls()
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& textBoxApi = CTextBoxInterfApi::get();
    const auto& pictureApi = CPictureInterfApi::get();

    auto service = CNetCustomService::get();
    auto userInfo = service->getUserInfo();

    auto txtNickname = (CTextBoxInterf*)findOptionalControl("TXT_NICKNAME",
                                                            rtti.CTextBoxInterfType);
    if (txtNickname) {
        textBoxApi.setString(txtNickname, userInfo.name.C_String());
    }

    auto imgPlayerLFace = (CPictureInterf*)findOptionalControl("IMG_PLAYER_LFACE",
                                                               rtti.CPictureInterfType);
    if (imgPlayerLFace) {
        CMqPoint offset{};
        pictureApi.setImage(imgPlayerLFace, getUserImage(userInfo, true, false), &offset);
    }

    auto imgPlayerRFace = (CPictureInterf*)findOptionalControl("IMG_PLAYER_RFACE",
                                                               rtti.CPictureInterfType);
    if (imgPlayerRFace) {
        CMqPoint offset{};
        pictureApi.setImage(imgPlayerRFace, getUserImage(userInfo, false, false), &offset);
    }

    auto imgPlayerLBigFace = (CPictureInterf*)findOptionalControl("IMG_PLAYER_LBIGFACE",
                                                                  rtti.CPictureInterfType);
    if (imgPlayerLBigFace) {
        CMqPoint offset{};
        pictureApi.setImage(imgPlayerLBigFace, getUserImage(userInfo, true, true), &offset);
    }

    auto imgPlayerRBigFace = (CPictureInterf*)findOptionalControl("IMG_PLAYER_RBIGFACE",
                                                                  rtti.CPictureInterfType);
    if (imgPlayerRBigFace) {
        CMqPoint offset{};
        pictureApi.setImage(imgPlayerLBigFace, getUserImage(userInfo, false, true), &offset);
    }
}

void CMenuCustomLobby::initializeUsersControls()
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& smartPtrApi = SmartPointerApi::get();
    const auto& menuBaseApi = CMenuBaseApi::get();
    const auto& listBoxApi = CListBoxInterfApi::get();

    auto dialog = menuBaseApi.getDialogInterface(this);

    const char* usersListBoxName = "LBOX_LPLAYERS";
    auto listBoxUsers = (CListBoxInterf*)findOptionalControl(usersListBoxName,
                                                             rtti.CListBoxInterfType);
    if (!listBoxUsers) {
        usersListBoxName = "LBOX_RPLAYERS";
        listBoxUsers = (CListBoxInterf*)findOptionalControl(usersListBoxName,
                                                            rtti.CListBoxInterfType);
    }

    if (listBoxUsers) {
        auto callback = (CMenuBaseApi::Api::ListBoxDisplayCallback)listBoxUsersDisplayHandler;

        SmartPointer functor;
        menuBaseApi.createListBoxDisplayFunctor(&functor, 0, this, &callback);
        listBoxApi.assignDisplaySurfaceFunctor(dialog, usersListBoxName, dialogName, &functor);
        smartPtrApi.createOrFreeNoDtor(&functor, nullptr);
    } else {
        usersListBoxName = "TLBOX_PLAYERS";
        listBoxUsers = (CListBoxInterf*)findOptionalControl(usersListBoxName,
                                                            rtti.CListBoxInterfType);
        if (listBoxUsers) {
            using TextCallback = CMenuBaseApi::Api::ListBoxDisplayTextCallback;
            TextCallback callback{(TextCallback::Callback)listBoxUsersDisplayTextHandler};

            SmartPointer functor;
            menuBaseApi.createListBoxDisplayTextFunctor(&functor, 0, this, &callback);
            listBoxApi.assignDisplayTextFunctor(dialog, usersListBoxName, dialogName, &functor,
                                                false);
            smartPtrApi.createOrFreeNoDtor(&functor, nullptr);
        }
    }

    auto txtPlayersTotal = (CTextBoxInterf*)findOptionalControl("TXT_PLAYERS_TOTAL",
                                                                rtti.CTextBoxInterfType);
    if (listBoxUsers || txtPlayersTotal) {
        ImagePtrVectorApi::get().reserve(&m_userIcons, 1);
        m_usersListBoxName = usersListBoxName;

        // Request users list as soon as possible, no need to wait for event
        CNetCustomService::get()->queryOnlineUsers();
        createTimerEvent(&m_usersUpdateEvent, this, usersUpdateEventCallback,
                         usersUpdateEventInterval);
    }
}

void CMenuCustomLobby::initializeRoomsControls()
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& smartPtrApi = SmartPointerApi::get();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& menuBaseApi = CMenuBaseApi::get();
    const auto& listBoxApi = CListBoxInterfApi::get();

    auto btnBack = (CButtonInterf*)findOptionalControl("BTN_BACK", rtti.CButtonInterfType);
    if (btnBack) {
        setButtonCallback(btnBack, backBtnHandler, this);
    }

    auto btnCreate = (CButtonInterf*)findOptionalControl("BTN_CREATE", rtti.CButtonInterfType);
    if (btnCreate) {
        setButtonCallback(btnCreate, createBtnHandler, this);
    }

    auto btnHelp = (CButtonInterf*)findOptionalControl("BTN_HELP", rtti.CButtonInterfType);

    if (btnHelp) {
        setButtonCallback(btnHelp, helpBtnHandler, this);
    }


    auto btnLoad = (CButtonInterf*)findOptionalControl("BTN_LOAD", rtti.CButtonInterfType);
    if (btnLoad) {
        setButtonCallback(btnLoad, loadBtnHandler, this);
    }

    auto btnJoin = (CButtonInterf*)findOptionalControl("BTN_JOIN", rtti.CButtonInterfType);
    if (btnJoin) {
        setButtonCallback(btnJoin, joinBtnHandler, this);
        btnJoin->vftable->setEnabled(btnJoin, false);
    }

    auto dialog = menuBaseApi.getDialogInterface(this);
    auto listBoxRooms = (CListBoxInterf*)findOptionalControl("LBOX_ROOMS", rtti.CListBoxInterfType);
    if (listBoxRooms) {
        auto callback = (CMenuBaseApi::Api::ListBoxDisplayCallback)listBoxRoomsDisplayHandler;

        SmartPointer functor;
        menuBaseApi.createListBoxDisplayFunctor(&functor, 0, this, &callback);
        listBoxApi.assignDisplaySurfaceFunctor(dialog, "LBOX_ROOMS", dialogName, &functor);
        smartPtrApi.createOrFreeNoDtor(&functor, nullptr);

        listBoxApi.setElementsTotal(listBoxRooms, 0);

        // Request rooms list as soon as possible, no need to wait for event
        CNetCustomService::get()->searchRooms();
        createTimerEvent(&m_roomsUpdateEvent, this, roomsUpdateEventCallback,
                         roomsUpdateEventInterval);
    }

    auto txtRoomInfo = (CTextBoxInterf*)findOptionalControl("TXT_ROOM_INFO",
                                                            rtti.CTextBoxInterfType);
    if (txtRoomInfo) {
        auto callback = (CMenuBaseApi::Api::ListBoxCallback)listBoxRoomsUpdateHandler;

        SmartPointer functor;
        menuBaseApi.createListBoxFunctor(&functor, 0, this, &callback);
        listBoxApi.assignFunctor(dialog, "LBOX_ROOMS", dialogName, &functor);
        smartPtrApi.createOrFreeNoDtor(&functor, nullptr);
    }
}

void CMenuCustomLobby::showRoomPasswordDialog()
{
    using namespace game;

    if (m_roomPasswordDialog) {
        spdlog::debug("Error showing room-password dialog that is already shown");
        return;
    }

    m_roomPasswordDialog = (CRoomPasswordInterf*)Memory::get().allocate(
        sizeof(CRoomPasswordInterf));
    showInterface(new (m_roomPasswordDialog) CRoomPasswordInterf(this));
}

void CMenuCustomLobby::hideRoomPasswordDialog()
{
    if (m_roomPasswordDialog) {
        hideInterface(m_roomPasswordDialog);
        m_roomPasswordDialog->vftable->destructor(m_roomPasswordDialog, 1);
        m_roomPasswordDialog = nullptr;
    }
}

void __fastcall CMenuCustomLobby::helpBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/)
{
    using namespace game;

    spdlog::debug("HELP Button pressed");

    if (thisptr->m_helpDialog) {
        spdlog::debug("HELP Dialog already opened, ignoring");
        return;
    }

    spdlog::debug("HELP Allocating help dialog");

    thisptr->m_helpDialog = (CHelpInterf*)Memory::get().allocate(sizeof(CHelpInterf));

    spdlog::debug("HELP Showing help dialog");

    showInterface(new (thisptr->m_helpDialog) CHelpInterf(thisptr));
}

void __fastcall CMenuCustomLobby::CHelpInterf::closeBtnHandler(CHelpInterf* thisptr, int /*%edx*/)
{
    spdlog::debug("HELP Close button pressed");

    auto menu = thisptr->m_menu;

    spdlog::debug("HELP Hiding help interface");

    hideInterface(thisptr);

    spdlog::debug("HELP Calling destructor");

    thisptr->vftable->destructor(thisptr, 1);

    menu->m_helpDialog = nullptr;

    spdlog::debug("HELP Help dialog destroyed and pointer cleared");
}

void __fastcall CMenuCustomLobby::createBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/)
{
    using namespace game;

    auto menuPhase = thisptr->menuBaseData->menuPhase;
    menuPhase->data->host = true;
    CMenuPhaseApi::get().switchPhase(menuPhase, MenuTransition::CustomLobby2NewSkirmish);
}

void __fastcall CMenuCustomLobby::loadBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/)
{
    using namespace game;

    auto menuPhase = thisptr->menuBaseData->menuPhase;
    menuPhase->data->host = true;
    CMenuPhaseApi::get().switchPhase(menuPhase, MenuTransition::CustomLobby2LoadSkirmish);
}
void __fastcall CMenuCustomLobby::CHelpInterf::listBoxClickHandler(CHelpInterf* thisptr,
                                                                   int,
                                                                   int index)
{
    const auto& helpEntries = hooks::textIds().lobby.help;

    if (index < 0 || index >= (int)helpEntries.size())
        return;

    const auto& entry = helpEntries[index];

    auto link = resolveTextSmart(entry.url, true);

    if (link.empty())
        return;

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000);

    if (ctrl) {
        copyToClipboard(link);
        spdlog::debug("HELP Copied URL: {}", link);
    } else if (shift) {

        HWND hwnd = GetForegroundWindow();
        if (hwnd && IsWindow(hwnd)) {
            ShowWindow(hwnd, SW_MINIMIZE);
        }

        openInBrowser(link);
        spdlog::debug("HELP Opened URL: {}", link);
    }
}

void __fastcall CMenuCustomLobby::CHelpInterf::listBoxDisplayHandler(CHelpInterf* thisptr,
                                                                     int,
                                                                     game::ImagePointList* contents,
                                                                     const game::CMqRect* lineArea,
                                                                     int index,
                                                                     bool selected)
{
    using namespace game;

    const auto& helpEntries = hooks::textIds().lobby.help;

    if (index < 0 || index >= (int)helpEntries.size())
        return;

    const auto& entry = helpEntries[index];

    auto title = resolveTextSmart(entry.title, false);

    const int rowHeight = 24;
    CMqPoint size{lineArea->right - lineArea->left, rowHeight};

    auto image2Text = (CImage2Text*)Memory::get().allocate(sizeof(CImage2Text));

    CImage2TextApi::get().constructor(image2Text, size.x, size.y);

    CImage2TextApi::get().setText(image2Text, fmt::format("\\fSmall;{}", title).c_str());

    ImagePtrPointPair pair{};
    SmartPointerApi::get().createOrFree((SmartPointer*)&pair.first, image2Text);

    pair.second.x = 0;
    pair.second.y = 0;

    ImagePointListApi::get().add(contents, &pair);

    SmartPointerApi::get().createOrFree((SmartPointer*)&pair.first, nullptr);

    if (selected) {
        game::Color color{};
        CMqPoint outlineSize{lineArea->right - lineArea->left, rowHeight};

        auto outline = (CImage2Outline*)Memory::get().allocate(sizeof(CImage2Outline));

        CImage2OutlineApi::get().constructor(outline, &outlineSize, &color, 0xff);

        ImagePtrPointPair outlinePair{};
        SmartPointerApi::get().createOrFree((SmartPointer*)&outlinePair.first, outline);

        outlinePair.second.x = 0;
        outlinePair.second.y = 0;

        ImagePointListApi::get().add(contents, &outlinePair);

        SmartPointerApi::get().createOrFree((SmartPointer*)&outlinePair.first, nullptr);
    }
}

CMenuCustomLobby::CHelpInterf::CHelpInterf(CMenuCustomLobby* menu)
    : CPopupDialogCustomBase(this, helpDialogName)
    , m_menu(menu)
{
    using namespace game;

    spdlog::debug("HELP Constructing HelpInterf");

    CPopupDialogInterfApi::get().constructor(this, helpDialogName, nullptr);

    setButtonCallback(*dialog, "BTN_CLOSE", closeBtnHandler, this);

    const auto& dialogApi = CDialogInterfApi::get();
    const auto& pictureApi = CPictureInterfApi::get();
    const auto& listBoxApi = CListBoxInterfApi::get();
    const auto& menuBaseApi = CMenuBaseApi::get();

    // ===== Portrait
    auto img = dialogApi.findPicture(*dialog, "IMG_PLAYER_RBIGFACE");
    if (img) {
        const auto& fn = gameFunctions();
        const auto& idApi = CMidgardIDApi::get();

        CMidgardID unitId{};
        idApi.fromString(&unitId, "G001UU7582");

        if (unitId.value == 0x3f0000) {
            spdlog::warn("Invalid id string, using fallback");
            idApi.fromString(&unitId, "G000UU0152");
        }

        auto* impl = hooks::getGlobalUnitImpl(&unitId);

        if (!impl) {
            spdlog::warn("Unit impl not found, fallback");

            idApi.fromString(&unitId, "G000UU0152");
            impl = hooks::getGlobalUnitImpl(&unitId);

            if (!impl) {
                spdlog::error("Fallback impl also not found");
                return;
            }
        }

        auto faceImage = fn.createUnitFaceImage(&unitId, true);

        if (!faceImage) {
            spdlog::error("createUnitFaceImage returned nullptr");
            return;
        }

        faceImage->vftable->setPercentHp(faceImage, 100);
        faceImage->vftable->setUnknown68(faceImage, 0);
        faceImage->vftable->setLeftSide(faceImage, false);

        CMqPoint offset{-4, 0};
        pictureApi.setImage(img, (IMqImage2*)faceImage, &offset);
    }

    // ===== Description from Lua
    auto txtHelp = dialogApi.findTextBox(*dialog, "TXT_HELP_INFO");
    if (txtHelp) {
        auto text = resolveTextSmart(hooks::textIds().lobby.helpDescription, false);
        CTextBoxInterfApi::get().setString(txtHelp, text.c_str());
    }

    // ===== ListBox
    auto listBox = dialogApi.findListBox(*dialog, "LBOX_TEXT_AREA");
    if (!listBox) {
        spdlog::error("HELP LBOX_TEXT_AREA not found!");
        return;
    }

    // Display callback
    SmartPointer displayFunctor;
    auto displayCallback = (CMenuBaseApi::Api::ListBoxDisplayCallback)listBoxDisplayHandler;

    menuBaseApi.createListBoxDisplayFunctor(&displayFunctor, 0, m_menu, &displayCallback);

    listBoxApi.assignDisplaySurfaceFunctor(*dialog, "LBOX_TEXT_AREA", helpDialogName,
                                           &displayFunctor);

    SmartPointerApi::get().createOrFreeNoDtor(&displayFunctor, nullptr);

    // Click callback
    SmartPointer clickFunctor;
    auto clickCallback = (CMenuBaseApi::Api::ListBoxCallback)listBoxClickHandler;

    menuBaseApi.createListBoxFunctor(&clickFunctor, 0, m_menu, &clickCallback);

    listBoxApi.assignFunctor(*dialog, "LBOX_TEXT_AREA", helpDialogName, &clickFunctor);

    SmartPointerApi::get().createOrFreeNoDtor(&clickFunctor, nullptr);

    // set count
    listBoxApi.setElementsTotal(listBox, (int)hooks::textIds().lobby.help.size());
}



void __fastcall CMenuCustomLobby::joinBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/)
{
    auto room = thisptr->getSelectedRoom();
    if (!room) {
        return;
    }

    if (room->usedSlots >= room->totalSlots) {
        showMessageBox(getInterfaceText("X005TA0886"));
        return;
    }

    // Show wait dialog right away because game files hashing can be lengthy
    thisptr->showWaitDialog();

    const auto& gameFilesHash = CNetCustomService::get()->getGameFilesHash();
    if (room->gameFilesHash != gameFilesHash) {
        auto message = getInterfaceText(textIds().lobby.checkFilesFailed.c_str());
        if (message.empty()) {
            message =
                "Unable to join the room because the owner's game version or files are different.";
        }

        thisptr->hideWaitDialog();
        showMessageBox(message);
        return;
    }

    if (!room->templateHash.empty()) {
        auto templatePath = templatesFolder() / room->templateName;

        if (!std::filesystem::exists(templatePath)) {
            thisptr->hideWaitDialog();

            showMessageBox(
                fmt::format("Required map template was not found:\n{}", room->templateName));

            return;
        }

        auto localHash = computeHash({templatePath});

        if (localHash != room->templateHash) {
            thisptr->hideWaitDialog();

            showMessageBox(
                fmt::format("Map template differs from the host version:\n{}", room->templateName));

            return;
        }
    }

    if (!room->password.empty()) {
        // Store selected room info because the room list can be updated while the dialog is shown
        thisptr->m_joiningRoomId = room->id;
        thisptr->m_joiningRoomPassword = room->password;
        thisptr->hideWaitDialog();
        thisptr->showRoomPasswordDialog();
        return;
    }

    CNetCustomService::get()->joinRoom(room->id);
}

void __fastcall CMenuCustomLobby::backBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/)
{
    using namespace game;

    auto message = getInterfaceText(textIds().lobby.confirmBack.c_str());
    if (message.empty()) {
        message = "Do you really want to exit the lobby?";
    }

    auto handler = (CConfirmBackMsgBoxButtonHandler*)Memory::get().allocate(
        sizeof(CConfirmBackMsgBoxButtonHandler));
    new (handler) CConfirmBackMsgBoxButtonHandler(thisptr);

    showMessageBox(message, handler, true);
}

void __fastcall CMenuCustomLobby::sendBtnHandler(CMenuCustomLobby* thisptr, int /*%edx*/)
{
    thisptr->sendChatMessage();
}

void __fastcall CMenuCustomLobby::roomsUpdateEventCallback(CMenuCustomLobby* /*thisptr*/,
                                                           int /*%edx*/)
{
    CNetCustomService::get()->searchRooms();
}

void __fastcall CMenuCustomLobby::usersUpdateEventCallback(CMenuCustomLobby* /*thisptr*/,
                                                           int /*%edx*/)
{
    CNetCustomService::get()->queryOnlineUsers();
}

void __fastcall CMenuCustomLobby::chatMessageRegenEventCallback(CMenuCustomLobby* thisptr,
                                                                int /*%edx*/)
{
    if (thisptr->m_chatMessageStock < chatMessageMaxStock) {
        ++thisptr->m_chatMessageStock;
    }
}

void __fastcall CMenuCustomLobby::listBoxRoomsUpdateHandler(CMenuCustomLobby* thisptr,
                                                            int /*%edx*/,
                                                            int selectedIndex)
{
    thisptr->updateTxtRoomInfo(selectedIndex);
}

void __fastcall CMenuCustomLobby::listBoxRoomsDisplayHandler(CMenuCustomLobby* thisptr,
                                                             int /*%edx*/,
                                                             game::ImagePointList* contents,
                                                             const game::CMqRect* lineArea,
                                                             int index,
                                                             bool selected)
{
    thisptr->updateListBoxRoomsRow(index, selected, lineArea, contents);
}

void __fastcall CMenuCustomLobby::listBoxUsersDisplayHandler(CMenuCustomLobby* thisptr,
                                                             int /*%edx*/,
                                                             game::ImagePointList* contents,
                                                             const game::CMqRect* lineArea,
                                                             int index,
                                                             bool selected)
{
    thisptr->updateListBoxUsersRow(index, selected, lineArea, contents);
}

void __fastcall CMenuCustomLobby::listBoxUsersDisplayTextHandler(CMenuCustomLobby* thisptr,
                                                                 int /*%edx*/,
                                                                 game::String* string,
                                                                 bool,
                                                                 int index)
{
    using namespace game;

    if (index < 0 || index >= (int)thisptr->m_users.size()) {
        return;
    }
    const auto& user = thisptr->m_users[index];

    game::StringApi::get().initFromString(string, user.name.C_String());
}

void __fastcall CMenuCustomLobby::listBoxChatDisplayHandler(CMenuCustomLobby* thisptr,
                                                            int /*%edx*/,
                                                            game::String* string,
                                                            bool,
                                                            int index)
{
    const auto& messages = thisptr->m_chatMessages;
    if (index < 0 || index >= (int)messages.size()) {
        return;
    }

    auto messageText{getInterfaceText(textIds().lobby.chatMessage.c_str())};
    if (messageText.empty()) {
        messageText = "\\fMedBold;%SENDER%:\\fNormal; %MSG%";
    }

    const auto& message = messages[index];
    replace(messageText, "%SENDER%", message.sender.C_String());
    replace(messageText, "%MSG%", message.text.C_String());
    game::StringApi::get().initFromString(string, messageText.c_str());
}

bool __fastcall CMenuCustomLobby::gameVersionMsgHandler(CMenuCustomLobby* menu,
                                                        int /*%edx*/,
                                                        const game::CGameVersionMsg* message,
                                                        std::uint32_t idFrom)
{
    using namespace game;

    const auto& midgardApi = CMidgardApi::get();

    spdlog::debug("CGameVersionMsg received from 0x{:x}", idFrom);

    if (message->gameVersion != GameVersion::RiseOfTheElves) {
        // You are trying to join a game with a newer or an older version of the game.
        midgardApi.clearNetworkState(midgardApi.instance());
        showMessageBox(getInterfaceText("X006TA0008"));
        return true;
    }

    CMenusReqInfoMsg reqInfoMsg;
    reqInfoMsg.vftable = NetMessagesApi::getMenusReqInfoVftable();
    return midgardApi.sendNetMsgToServer(midgardApi.instance(), &reqInfoMsg);
}

bool __fastcall CMenuCustomLobby::ansInfoMsgHandler(CMenuCustomLobby* menu,
                                                    int /*%edx*/,
                                                    const game::CMenusAnsInfoMsg* message,
                                                    std::uint32_t idFrom)
{
    using namespace game;

    const auto& midgardApi = CMidgardApi::get();
    const auto& listApi = RaceCategoryListApi::get();

    spdlog::debug("CMenusAnsInfoMsg received from 0x{:x}", idFrom);

    menu->hideWaitDialog();

    if (message->raceIds.length == 0) {
        // Could not join game.  Host did not report any available race.
        midgardApi.clearNetworkState(midgardApi.instance());
        showMessageBox(getInterfaceText("X005TA0886"));
        return true;
    }

    auto globalData = *GlobalDataApi::get().getGlobalData();
    auto racesTable = globalData->raceCategories;
    auto& findRaceById = LRaceCategoryTableApi::get().findCategoryById;

    auto menuPhase = menu->menuBaseData->menuPhase;
    auto phaseData = menuPhase->data;

    auto races = &phaseData->races;
    listApi.freeNodes(races);
    for (auto node = message->raceIds.head->next; node != message->raceIds.head;
         node = node->next) {
        const int raceId = static_cast<const int>(node->data);

        LRaceCategory category{};
        findRaceById(racesTable, &category, &raceId);

        listApi.add(races, &category);
    }

    phaseData->host = false;
    phaseData->unknown8 = false;
    phaseData->suggestedLevel = message->suggestedLevel;
    allocateString(&phaseData->scenarioName, message->scenarioName);
    allocateString(&phaseData->scenarioDescription, message->scenarioDescription);

    game::CMenuPhaseApi::get().switchPhase(menuPhase, MenuTransition::CustomLobby2LobbyJoin);
    return true;
}

CMenuCustomLobby::RoomInfo CMenuCustomLobby::getRoomInfo(SLNet::RoomDescriptor* roomDescriptor)
{
    std::vector<std::string> clientNames;

    const auto& roomMembers = roomDescriptor->roomMemberList;

    for (unsigned int i = 0; i < roomMembers.Size(); ++i) {
        const auto& member = roomMembers[i];

        if (member.roomMemberMode != RMM_MODERATOR) {
            clientNames.push_back(member.name.C_String());
        }
    }

    auto gameNameProperty = roomDescriptor->GetProperty(CNetCustomService::gameNameColumnName);

    auto passwordProperty = roomDescriptor->GetProperty(CNetCustomService::passwordColumnName);

    auto filesHashProperty = roomDescriptor->GetProperty(CNetCustomService::gameFilesHashColumnName);

    auto templateNameProperty = getPropertySafe(roomDescriptor,
                                                CNetCustomService::templateNameColumnName);

    auto templateHashProperty = getPropertySafe(roomDescriptor,
                                                CNetCustomService::templateHashColumnName);

    auto gameVersionProperty = roomDescriptor->GetProperty(
        CNetCustomService::gameVersionColumnName);

    auto scenarioNameProperty = roomDescriptor->GetProperty(
        CNetCustomService::scenarioNameColumnName);

    auto scenarioDescriptionProperty = roomDescriptor->GetProperty(
        CNetCustomService::scenarioDescriptionColumnName);

    auto usedSlotsProperty = roomDescriptor->GetProperty(DefaultRoomColumns::TC_USED_PUBLIC_SLOTS);

    auto totalSlotsProperty = roomDescriptor->GetProperty(
        DefaultRoomColumns::TC_TOTAL_PUBLIC_SLOTS);

    auto* moderator = getRoomModerator(roomDescriptor->roomMemberList);

    return {roomDescriptor->lobbyRoomId, moderator ? moderator->name.C_String() : "",

            (gameNameProperty && gameNameProperty->c) ? gameNameProperty->c : "",
            (passwordProperty && passwordProperty->c) ? passwordProperty->c : "",
            (filesHashProperty && filesHashProperty->c) ? filesHashProperty->c : "",

            (templateNameProperty && templateNameProperty->c) ? templateNameProperty->c : "",
            (templateHashProperty && templateHashProperty->c) ? templateHashProperty->c : "",

            (gameVersionProperty && gameVersionProperty->c) ? gameVersionProperty->c : "",
            (scenarioNameProperty && scenarioNameProperty->c) ? scenarioNameProperty->c : "",
            (scenarioDescriptionProperty && scenarioDescriptionProperty->c)
                ? scenarioDescriptionProperty->c
                : "",

            std::move(clientNames),

            // Add 1 to used and total slots because they are not counting room moderator
            usedSlotsProperty ? (int)usedSlotsProperty->i + 1 : 0,
            totalSlotsProperty ? (int)totalSlotsProperty->i + 1 : 0};
}

SLNet::RoomMemberDescriptor* CMenuCustomLobby::getRoomModerator(
    DataStructures::List<SLNet::RoomMemberDescriptor>& roomMembers)
{
    for (unsigned int i = 0; i < roomMembers.Size(); ++i) {
        auto& member = roomMembers[i];
        if (member.roomMemberMode == RMM_MODERATOR) {
            return &member;
        }
    }

    return nullptr;
}

void CMenuCustomLobby::updateRooms(DataStructures::List<SLNet::RoomDescriptor*>& roomDescriptors)
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& textBoxApi = CTextBoxInterfApi::get();
    const auto& listBoxApi = CListBoxInterfApi::get();

    auto dialog = CMenuBaseApi::get().getDialogInterface(this);
    auto listBox = dialogApi.findListBox(dialog, "LBOX_ROOMS");
    if (!listBox->vftable->isOnTop(listBox)) {
        // If the listBox is not on top then the game will only remove its childs without rendering
        // the new ones. This happens when any popup dialog is displayed.
        return;
    }

    SLNet::RoomID selectedRoomId = (unsigned)-1;
    auto selectedRoom = getSelectedRoom();
    if (selectedRoom) {
        selectedRoomId = selectedRoom->id;
    }

    m_rooms.clear();
    m_rooms.reserve(roomDescriptors.Size());
    auto selectedIndex = roomDescriptors.Size() ? 0 : (unsigned)-1;
    for (unsigned int i = 0; i < roomDescriptors.Size(); ++i) {
        m_rooms.push_back(getRoomInfo(roomDescriptors[i]));
        if (m_rooms.back().id == selectedRoomId) {
            selectedIndex = i;
        }
    }

    listBoxApi.setElementsTotal(listBox, (int)m_rooms.size());
    if (selectedIndex != (unsigned)-1) {
        listBoxApi.setSelectedIndex(listBox, selectedIndex);
    }

    auto btnJoin = (CButtonInterf*)findOptionalControl("BTN_JOIN", rtti.CButtonInterfType);
    if (btnJoin) {
        btnJoin->vftable->setEnabled(btnJoin, !m_rooms.empty());
    }

    auto txtRoomsTotal = (CTextBoxInterf*)findOptionalControl("TXT_ROOMS_TOTAL",
                                                              rtti.CTextBoxInterfType);
    if (txtRoomsTotal) {
        auto text{getInterfaceText(textIds().lobby.roomsTotal.c_str())};
        if (text.empty()) {
            text = "Games available: %ROOMS_NUM%";
        }
        replace(text, "%ROOMS_NUM%", fmt::format("{:d}", m_rooms.size()));

        textBoxApi.setString(txtRoomsTotal, text.c_str());
    }

    updateTxtRoomInfo(selectedIndex);
}

const CMenuCustomLobby::RoomInfo* CMenuCustomLobby::getSelectedRoom()
{
    using namespace game;

    auto dialog = CMenuBaseApi::get().getDialogInterface(this);
    auto listBox = CDialogInterfApi::get().findListBox(dialog, "LBOX_ROOMS");
    auto index = (size_t)CListBoxInterfApi::get().selectedIndex(listBox);
    return index < m_rooms.size() ? &m_rooms[index] : nullptr;
}

void CMenuCustomLobby::updateTxtRoomInfo(int roomIndex)
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& textBoxApi = CTextBoxInterfApi::get();

    auto txtRoomInfo = (CTextBoxInterf*)findOptionalControl("TXT_ROOM_INFO",
                                                            rtti.CTextBoxInterfType);
    if (!txtRoomInfo) {
        return;
    }

    if ((size_t)roomIndex >= m_rooms.size()) {
        textBoxApi.setString(txtRoomInfo, "");
        return;
    }
    const auto& room = m_rooms[(size_t)roomIndex];

    auto text{getInterfaceText(textIds().lobby.roomInfo.c_str())};
    if (text.empty()) {
        text =
            "\\hC;Version: %VERSION%\n"
            "\\fNormal;Players (%PLAYERS_NUM%/%PLAYERS_MAX%): \\fMedBold;%HOST%\\fNormal;%CLIENTS_SEPARATOR%%CLIENTS%\n"
            "\\fMedBold;%SCEN_NAME%\n"
            "\\fNormal;%SCEN_DESC%";
    }

    std::string clients;
    for (const auto& clientName : room.clientNames) {
        if (clients.length()) {
            clients += ", ";
        }
        clients += clientName;
    }

    replace(text, "%VERSION%", room.gameVersion);
    replace(text, "%PLAYERS_NUM%", fmt::format("{:d}", room.usedSlots));
    replace(text, "%PLAYERS_MAX%", fmt::format("{:d}", room.totalSlots));
    replace(text, "%HOST%", room.hostName);
    replace(text, "%CLIENTS_SEPARATOR%", clients.length() ? ", " : "");
    replace(text, "%CLIENTS%", clients);
    replace(text, "%SCEN_NAME%", room.scenarioName);
    replace(text, "%SCEN_DESC%", room.scenarioDescription);

    textBoxApi.setString(txtRoomInfo, text.c_str());
}

void CMenuCustomLobby::updateListBoxRoomsRow(int rowIndex,
                                             bool selected,
                                             const game::CMqRect* lineArea,
                                             game::ImagePointList* contents)
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();

    // If TXT_DESCRIPTION is present then we assume that a legacy (Motlin's) table layout is used
    if (findOptionalControl("TXT_DESCRIPTION", rtti.CTextBoxInterfType)) {
        updateListBoxRoomsTableRow(rowIndex, selected, lineArea, contents);
    } else {
        updateListBoxRoomsListRow(rowIndex, selected, lineArea, contents);
    }
}

void CMenuCustomLobby::updateListBoxRoomsListRow(int rowIndex,
                                                 bool selected,
                                                 const game::CMqRect* lineArea,
                                                 game::ImagePointList* contents)
{
    if ((size_t)rowIndex >= m_rooms.size()) {
        return;
    }
    const auto& room = m_rooms[(size_t)rowIndex];

    auto text{getInterfaceText(textIds().lobby.roomInfoInList.c_str())};
    if (text.empty()) {
        text =
            "\\fMedBold;%NAME%\n"
            "\\fNormal;Version: %VERSION%\n"
            "\\fNormal;Players (%PLAYERS_NUM%/%PLAYERS_MAX%): \\fMedBold;%HOST%\\fNormal;%CLIENTS_SEPARATOR%%CLIENTS%";
    }

    std::string clients;
    for (const auto& clientName : room.clientNames) {
        if (clients.length()) {
            clients += ", ";
        }
        clients += clientName;
    }

    replace(text, "%NAME%", room.gameName);
    replace(text, "%VERSION%", room.gameVersion);
    replace(text, "%PLAYERS_NUM%", fmt::format("{:d}", room.usedSlots));
    replace(text, "%PLAYERS_MAX%", fmt::format("{:d}", room.totalSlots));
    replace(text, "%HOST%", room.hostName);
    replace(text, "%CLIENTS_SEPARATOR%", clients.length() ? ", " : "");
    replace(text, "%CLIENTS%", clients);

    addListBoxRoomsItemContent(text.c_str(), "DLG_CUSTOM_LOBBY_PROTECTED_ROOM",
                               !room.password.empty(), lineArea, contents);

    if (selected) {
        addListBoxRoomsSelectionOutline(lineArea, contents);
    }
}

void CMenuCustomLobby::updateListBoxRoomsTableRow(int rowIndex,
                                                  bool selected,
                                                  const game::CMqRect* lineArea,
                                                  game::ImagePointList* contents)
{
    if ((size_t)rowIndex >= m_rooms.size()) {
        return;
    }
    const auto& room = m_rooms[(size_t)rowIndex];

    addListBoxRoomsCellText("TXT_HOST", fmt::format("\\vC;{:s}", room.hostName).c_str(), lineArea,
                            contents);
    addListBoxRoomsCellText("TXT_DESCRIPTION", fmt::format("\\vC;{:s}", room.gameName).c_str(),
                            lineArea, contents);
    addListBoxRoomsCellText("TXT_VERSION", fmt::format("\\vC;{:s}", room.gameVersion).c_str(),
                            lineArea, contents);
    addListBoxRoomsCellText(
        "TXT_PLAYERS", fmt::format("\\vC;\\hC;{:d}/{:d}", room.usedSlots, room.totalSlots).c_str(),
        lineArea, contents);

    if (!room.password.empty()) {
        addListBoxRoomsCellImage("IMG_PROTECTED", "DLG_CUSTOM_LOBBY_PROTECTED_ROOM", lineArea,
                                 contents);
    }

    if (selected) {
        addListBoxRoomsSelectionOutline(lineArea, contents);
    }
}

void CMenuCustomLobby::addListBoxRoomsItemContent(const char* text,
                                                  const char* imageName,
                                                  bool showImage,
                                                  const game::CMqRect* lineArea,
                                                  game::ImagePointList* contents)
{
    using namespace game;

    const auto& createFreePtr = SmartPointerApi::get().createOrFree;
    const auto& image2TextApi = CImage2TextApi::get();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& autoDialogApi = AutoDialogApi::get();
    const auto& menuBaseApi = CMenuBaseApi::get();

    CMqPoint textSize{lineArea->right - lineArea->left, lineArea->bottom - lineArea->top};

    const int IMAGE_PADDING_X = 8;
    CMqPoint imageSize{};
    auto image = autoDialogApi.loadImage(imageName);
    if (image) {
        image->vftable->getSize(image, &imageSize);
        textSize.x -= imageSize.x + IMAGE_PADDING_X * 2;
    }

    auto image2Text = (CImage2Text*)Memory::get().allocate(sizeof(CImage2Text));
    image2TextApi.constructor(image2Text, textSize.x, textSize.y);
    image2TextApi.setText(image2Text, text);

    auto dialog = menuBaseApi.getDialogInterface(this);
    auto listBox = dialogApi.findListBox(dialog, "LBOX_ROOMS");
    auto listBoxRect = listBox->vftable->getArea(listBox);

    ImagePtrPointPair pair{};
    createFreePtr((SmartPointer*)&pair.first, image2Text);
    ImagePointListApi::get().add(contents, &pair);
    createFreePtr((SmartPointer*)&pair.first, nullptr);

    if (image) {
        if (showImage) {
            ImagePtrPointPair pair{};
            createFreePtr((SmartPointer*)&pair.first, image);
            pair.second.x = textSize.x + IMAGE_PADDING_X;
            pair.second.y = (lineArea->bottom - lineArea->top - imageSize.y) / 2;
            ImagePointListApi::get().add(contents, &pair);
            createFreePtr((SmartPointer*)&pair.first, nullptr);
        } else {
            image->vftable->destructor(image, 1);
        }
    }
}

void CMenuCustomLobby::addListBoxRoomsCellText(const char* columnName,
                                               const char* value,
                                               const game::CMqRect* lineArea,
                                               game::ImagePointList* contents)
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& createFreePtr = SmartPointerApi::get().createOrFree;
    const auto& image2TextApi = CImage2TextApi::get();

    auto columnHeader = (CTextBoxInterf*)findOptionalControl(columnName, rtti.CTextBoxInterfType);
    if (!columnHeader) {
        return;
    }

    auto columnHeaderRect = columnHeader->vftable->getArea(columnHeader);
    auto cellWidth = columnHeaderRect->right - columnHeaderRect->left;
    auto cellHeight = lineArea->bottom - lineArea->top;

    auto image2Text = (CImage2Text*)Memory::get().allocate(sizeof(CImage2Text));
    image2TextApi.constructor(image2Text, cellWidth, cellHeight);
    image2TextApi.setText(image2Text, value);

    auto dialog = CMenuBaseApi::get().getDialogInterface(this);
    auto listBox = dialogApi.findListBox(dialog, "LBOX_ROOMS");
    auto listBoxRect = listBox->vftable->getArea(listBox);

    ImagePtrPointPair pair{};
    createFreePtr((SmartPointer*)&pair.first, image2Text);
    pair.second.x = columnHeaderRect->left - listBoxRect->left;
    pair.second.y = 0;
    ImagePointListApi::get().add(contents, &pair);
    createFreePtr((SmartPointer*)&pair.first, nullptr);
}

void CMenuCustomLobby::addListBoxRoomsCellImage(const char* columnName,
                                                const char* imageName,
                                                const game::CMqRect* lineArea,
                                                game::ImagePointList* contents)
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& createFreePtr = SmartPointerApi::get().createOrFree;

    auto columnHeader = (CPictureInterf*)findOptionalControl(columnName, rtti.CPictureInterfType);
    if (!columnHeader) {
        return;
    }

    auto columnHeaderRect = columnHeader->vftable->getArea(columnHeader);
    auto image = AutoDialogApi::get().loadImage(imageName);

    // Calculate offset to center the image
    CMqPoint imageSize{};
    image->vftable->getSize(image, &imageSize);
    CMqPoint imageOffset{(columnHeaderRect->right - columnHeaderRect->left - imageSize.x) / 2,
                         (lineArea->bottom - lineArea->top - imageSize.y) / 2};

    auto dialog = CMenuBaseApi::get().getDialogInterface(this);
    auto listBox = dialogApi.findListBox(dialog, "LBOX_ROOMS");
    auto listBoxRect = listBox->vftable->getArea(listBox);

    ImagePtrPointPair pair{};
    createFreePtr((SmartPointer*)&pair.first, image);
    pair.second.x = imageOffset.x + columnHeaderRect->left - listBoxRect->left;
    pair.second.y = imageOffset.y;
    ImagePointListApi::get().add(contents, &pair);
    createFreePtr((SmartPointer*)&pair.first, nullptr);
}

void CMenuCustomLobby::addListBoxRoomsSelectionOutline(const game::CMqRect* lineArea,
                                                       game::ImagePointList* contents)
{
    using namespace game;

    const auto& createFreePtr = SmartPointerApi::get().createOrFree;

    // 0x0 - transparent, 0xff - opaque
    game::Color color{};
    CMqPoint size{lineArea->right - lineArea->left, lineArea->bottom - lineArea->top};
    auto outline = (CImage2Outline*)Memory::get().allocate(sizeof(CImage2Outline));
    CImage2OutlineApi::get().constructor(outline, &size, &color, 0xff);

    ImagePtrPointPair pair{};
    createFreePtr((SmartPointer*)&pair.first, outline);
    pair.second.x = 0;
    pair.second.y = 0;
    ImagePointListApi::get().add(contents, &pair);
    createFreePtr((SmartPointer*)&pair.first, nullptr);
}

void CMenuCustomLobby::updateListBoxUsersRow(int rowIndex,
                                             bool /*selected*/,
                                             const game::CMqRect* lineArea,
                                             game::ImagePointList* contents)
{
    using namespace game;

    if (rowIndex < 0 || rowIndex >= (int)m_users.size()) {
        return;
    }
    const auto& user = m_users[rowIndex];

    if (rowIndex < 0 || rowIndex >= (int)m_userIcons.size()) {
        spdlog::debug(
            "Failed to update users listbox because the icon at index {:d} is out of list bounds",
            rowIndex);
        return;
    }
    const auto& icon = m_userIcons.bgn[rowIndex];

    CMqPoint iconSize;
    icon.data->vftable->getSize(icon.data, &iconSize);

    // Center image horizontally
    CMqPoint iconClientPos{(lineArea->right - lineArea->left - iconSize.x) / 2, 0};

    // Place text area under the icon
    CMqRect textClientArea = {0, iconSize.y, lineArea->right - lineArea->left,
                              lineArea->bottom - lineArea->top};

    auto playerName = getShortenedUserNameInList(user.name.C_String(), "...",
                                                 textClientArea.right - textClientArea.left);

    ImagePointListApi::get().addImageWithText(contents, lineArea, &icon, &iconClientPos,
                                              playerName.c_str(), &textClientArea, false, 0);
}

game::IMqImage2* CMenuCustomLobby::getUserImage(const CNetCustomService::UserInfo& user,
                                                bool left,
                                                bool big)
{
    using namespace game;

    const auto& fn = gameFunctions();
    const auto& idApi = CMidgardIDApi::get();

    CMidgardID unitImplId{};
    auto token = user.guid.g;
    // A little bit of easter eggs is never wrong
    if (user.name == "UnveN") {
        idApi.fromString(&unitImplId, "G000UU0101"); /* Skeleton */
    } else if (token % 23 == 0) {
        idApi.fromString(&unitImplId, "G000UU5031"); /* Master Thug */
    } else if (token % 69 == 0) {
        idApi.fromString(&unitImplId, "G000UU6121"); /* Drega Zul */
    } else if (token % 81 == 0) {
        idApi.fromString(&unitImplId, "G000UU8046"); /* Shamaness */
    } else {
        const size_t ID_COUNT = 18;
        const char* unitImplIds[ID_COUNT] =
            {"G000UU0001" /* Squire */,     "G000UU0006" /* Archer */,
             "G000UU0008" /* Apprentice */, "G000UU0011" /* Acolyte */,
             "G000UU0036" /* Dwarf */,      "G000UU0026" /* Axe Thrower */,
             "G000UU0033" /* Tenderfoot */, "G000UU0052" /* Possessed */,
             "G000UU0062" /* Cultist */,    "G000UU0086" /* Fighter */,
             "G000UU0080" /* Initiate */,   "G000UU0078" /* Ghost */,
             "G000UU0092" /* Werewolf */,   "G000UU8014" /* Centaur Lancer */,
             "G000UU8018" /* Scout */,      "G000UU8025" /* Adept */,
             "G000UU8031" /* Spiritess */,  "G000UU8036" /* Ent Minor */};
        idApi.fromString(&unitImplId, unitImplIds[token % ID_COUNT]);
    }

    auto faceImage = fn.createUnitFaceImage(&unitImplId, big);
    faceImage->vftable->setPercentHp(faceImage, 100);
    faceImage->vftable->setUnknown68(faceImage, 0);
    faceImage->vftable->setLeftSide(faceImage, left);

    return (IMqImage2*)faceImage;
}

std::string CMenuCustomLobby::getShortenedUserNameInList(const char* name,
                                                         const char* shortenedMark,
                                                         int textAreaWidth)
{
    using namespace game;

    auto format{getInterfaceText(textIds().lobby.playerNameInList.c_str())};
    if (format.empty()) {
        format = "\\vC;\\hC;\\fSmall;%NAME%";
    }

    std::string result = format;
    replace(result, "%NAME%", name);

    std::string shortenedName = name;
    auto markLength = shortenedMark ? strlen(shortenedMark) : 0;

    FormattedTextPtr ptr;
    IFormattedTextApi::get().getFormattedText(&ptr);
    while (ptr.data->vftable->getTextWidth(ptr.data, result.c_str()) >= textAreaWidth) {
        if (shortenedName.length() <= markLength) {
            break;
        }
        shortenedName.pop_back();

        result = format;
        if (shortenedMark) {
            replace(result, "%NAME%", std::string(shortenedName).append(shortenedMark));
        } else {
            replace(result, "%NAME%", shortenedName);
        }
    }
    SmartPointerApi::get().createOrFree((SmartPointer*)&ptr, nullptr);

    return result;
}

// See CMenuSession::JoinGameBtnCallback (Akella 0x4f0136)
void CMenuCustomLobby::joinServer(SLNet::RoomDescriptor* roomDescriptor)
{
    using namespace game;

    const auto& midgardApi = CMidgardApi::get();

    auto service = CNetCustomService::get();
    CNetCustomSession* session = nullptr;
    CNetCustomSessEnum sessEnum{getRoomModerator(roomDescriptor->roomMemberList)->guid,
                                roomDescriptor->GetProperty(DefaultRoomColumns::TC_ROOM_NAME)->c};
    service->vftable->joinSession(service, (IMqNetSession**)&session, (IMqNetSessEnum*)&sessEnum,
                                  nullptr);

    // Add 1 to total slots because they are not counting room moderator
    auto maxPlayers = (int)roomDescriptor->GetProperty(DefaultRoomColumns::TC_TOTAL_PUBLIC_SLOTS)->i
                      + 1;
    session->setMaxPlayers(maxPlayers);

    auto menuPhase = menuBaseData->menuPhase;
    menuPhase->data->maxPlayers = maxPlayers;

    auto midgard = midgardApi.instance();
    auto& currentSession = midgard->data->netSession;
    if (currentSession) {
        currentSession->vftable->destructor(currentSession, 1);
    }
    currentSession = session;
    midgard->data->host = false;
    midgardApi.createNetClient(midgard, service->getUserName().c_str(), true);
    midgardApi.setClientsNetProxy(midgard, menuPhase);

    CMenusReqVersionMsg reqVersionMsg;
    reqVersionMsg.vftable = NetMessagesApi::getMenusReqVersionVftable();
    if (!midgardApi.sendNetMsgToServer(midgard, &reqVersionMsg)) {
        // Cannot connect to game session
        midgardApi.clearNetworkState(midgard);
        showMessageBox(getInterfaceText("X003TA0001"));
    }
}

void CMenuCustomLobby::addChatMessage(CNetCustomService::ChatMessage message)
{
    if (m_chatMessages.size() >= chatMessageMaxCount) {
        m_chatMessages.pop_front();
    }
#ifdef D2_TESTDRV
    // Read before the move below: the lobby server echoes both remote and locally sent lines here.
    hooks::testdrv::lobbychatreporter::onChatReceived(message.sender.C_String(),
                                                      message.text.C_String());
#endif
    m_chatMessages.push_back(std::move(message));

    updateListBoxChat();
}

void CMenuCustomLobby::sendChatMessage()
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& editBoxApi = CEditBoxInterfApi::get();

    auto editBoxChat = (CEditBoxInterf*)findOptionalControl("EDIT_CHAT", rtti.CEditBoxInterfType);
    if (!editBoxChat) {
        return;
    }

    auto message = editBoxChat->data->editBoxData.inputString.string;
    if (!strlen(message)) {
        return;
    }

    if (m_chatMessageStock == 0) {
        auto msg{getInterfaceText(textIds().lobby.tooManyChatMessages.c_str())};
        if (msg.empty()) {
            msg = "Too many chat messages.\nWait a couple of seconds.";
        }
        showMessageBox(msg);
        return;
    }

    CNetCustomService::get()->sendChatMessage(message);
    editBoxApi.setString(editBoxChat, "");
    --m_chatMessageStock;
}

void CMenuCustomLobby::updateUsers(std::vector<CNetCustomService::UserInfo> users)
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& textBoxApi = CTextBoxInterfApi::get();
    const auto& listBoxApi = CListBoxInterfApi::get();
    const auto& smartPtrApi = SmartPointerApi::get();
    const auto& imagePtrVectorApi = ImagePtrVectorApi::get();

    auto dialog = CMenuBaseApi::get().getDialogInterface(this);
    auto txtPlayersTotal = (CTextBoxInterf*)findOptionalControl("TXT_PLAYERS_TOTAL",
                                                                rtti.CTextBoxInterfType);
    if (txtPlayersTotal) {
        auto text{getInterfaceText(textIds().lobby.playersTotal.c_str())};
        if (text.empty()) {
            text = "Players online: %PLAYERS_NUM%";
        }
        replace(text, "%PLAYERS_NUM%", fmt::format("{:d}", users.size()));

        textBoxApi.setString(txtPlayersTotal, text.c_str());
    }

    auto listBox = (CListBoxInterf*)findOptionalControl(m_usersListBoxName,
                                                        rtti.CListBoxInterfType);
    if (!listBox || !listBox->vftable->isOnTop(listBox)) {
        // If the listBox is not on top then the game will only remove its childs without rendering
        // the new ones. This happens when any popup dialog is displayed.
        return;
    }

    bool isLeftIcons = m_usersListBoxName == "LBOX_LPLAYERS";
    bool isRightIcons = m_usersListBoxName == "LBOX_RPLAYERS";
    if (isLeftIcons || isRightIcons) {
        // See CManageStkInterfSetLeaderAbilities (Akella 0x497E5A)
        imagePtrVectorApi.destructor(&m_userIcons);
        imagePtrVectorApi.reserve(&m_userIcons, 1);
        for (const auto& user : users) {
            auto* icon = getUserImage(user, isLeftIcons, false);
            ImagePtr iconPtr{};
            smartPtrApi.createOrFree((SmartPointer*)&iconPtr, icon);
            imagePtrVectorApi.pushBack(&m_userIcons, &iconPtr);
            smartPtrApi.createOrFree((SmartPointer*)&iconPtr, nullptr);
        }
    }

    auto selectedIndex = listBoxApi.selectedIndex(listBox);
    m_users = std::move(users);
    listBoxApi.setElementsTotal(listBox, (int)m_users.size());
    listBoxApi.setSelectedIndex(listBox, std::min(selectedIndex, (int)m_users.size() - 1));
}

void CMenuCustomLobby::updateChat(std::vector<CNetCustomService::ChatMessage> messages)
{
    m_chatMessages.clear();
    while (messages.size()) {
        if (m_chatMessages.size() >= chatMessageMaxCount) {
            break;
        }
        m_chatMessages.push_front(std::move(messages.back()));
        messages.pop_back();
    }

    updateListBoxChat();
}

void CMenuCustomLobby::updateListBoxChat()
{
    using namespace game;

    const auto& rtti = RttiApi::rtti();
    const auto& dialogApi = CDialogInterfApi::get();
    const auto& listBoxApi = CListBoxInterfApi::get();

    auto listBoxChat = (CListBoxInterf*)findOptionalControl("LBOX_CHAT", rtti.CListBoxInterfType);
    if (!listBoxChat || !listBoxChat->vftable->isOnTop(listBoxChat)) {
        // If the listBox is not on top then the game will only remove its childs without rendering
        // the new ones. This happens when any popup dialog is displayed.
        return;
    }

    auto count = m_chatMessages.size();
    listBoxApi.setElementsTotal(listBoxChat, (int)count);
    listBoxApi.setSelectedIndex(listBoxChat, (int)count - 1);
}

void CMenuCustomLobby::PeerCallback::onPacketReceived(DefaultMessageIDTypes type,
                                                      SLNet::RakPeerInterface* peer,
                                                      const SLNet::Packet* packet)
{
    switch (type) {
    case ID_LOBBY_CHAT_MESSAGE: {
        m_menu->addChatMessage(CNetCustomService::get()->readChatMessage(packet));
        break;
    }

    case ID_LOBBY_GET_ONLINE_USERS_RESPONSE: {
        m_menu->updateUsers(CNetCustomService::get()->readOnlineUsers(packet));
        break;
    }

    case ID_LOBBY_GET_CHAT_MESSAGES_RESPONSE: {
        m_menu->updateChat(CNetCustomService::get()->readChatMessages(packet));
        break;
    }
    }
}

void CMenuCustomLobby::RoomsCallback::JoinByFilter_Callback(const SLNet::SystemAddress&,
                                                            SLNet::JoinByFilter_Func* callResult)
{
    m_menu->hideWaitDialog();

    switch (auto resultCode = callResult->resultCode) {
    case SLNet::REC_SUCCESS: {
        m_menu->joinServer(&callResult->joinedRoomResult.roomDescriptor);
        break;
    }

    case SLNet::REC_JOIN_BY_FILTER_NO_ROOMS: {
        auto msg{getInterfaceText(textIds().lobby.selectedRoomNoLongerExists.c_str())};
        if (msg.empty()) {
            msg = "The selected room no longer exists.";
        }
        showMessageBox(msg);
        break;
    }

    case SLNet::REC_JOIN_BY_FILTER_NO_SLOTS: {
        // Could not join game. Host did not report any available race.
        showMessageBox(getInterfaceText("X005TA0886"));
        break;
    }

    default: {
        auto msg{getInterfaceText(textIds().lobby.joinRoomFailed.c_str())};
        if (msg.empty()) {
            msg = "Could not join a room.\n%ERROR%";
        }
        replace(msg, "%ERROR%", SLNet::RoomsErrorCodeDescription::ToEnglish(resultCode));
        showMessageBox(msg);
        break;
    }
    }
}

void CMenuCustomLobby::RoomsCallback::SearchByFilter_Callback(
    const SLNet::SystemAddress&,
    SLNet::SearchByFilter_Func* callResult)
{
    switch (auto resultCode = callResult->resultCode) {
    case SLNet::REC_SUCCESS: {
        m_menu->updateRooms(callResult->roomsOutput);
        break;
    }

    default: {
        if (!CNetCustomService::get()->loggedIn()) {
            // The error is expected, just silently remove the search event
            game::UiEventApi::get().destructor(&m_menu->m_roomsUpdateEvent);
            break;
        }

        auto msg{getInterfaceText(textIds().lobby.searchRoomsFailed.c_str())};
        if (msg.empty()) {
            msg = "Could not search for rooms.\n%ERROR%";
        }
        replace(msg, "%ERROR%", SLNet::RoomsErrorCodeDescription::ToEnglish(resultCode));
        showMessageBox(msg);
        break;
    }
    }
}

CMenuCustomLobby::CConfirmBackMsgBoxButtonHandler::CConfirmBackMsgBoxButtonHandler(
    CMenuCustomLobby* menu)
    : m_menu{menu}
{
    static game::CMidMsgBoxButtonHandlerVftable vftable = {
        (game::CMidMsgBoxButtonHandlerVftable::Destructor)destructor,
        (game::CMidMsgBoxButtonHandlerVftable::Handler)handler,
    };

    this->vftable = &vftable;
}

void __fastcall CMenuCustomLobby::CConfirmBackMsgBoxButtonHandler::destructor(
    CConfirmBackMsgBoxButtonHandler* thisptr,
    int /*%edx*/,
    char flags)
{ }

void __fastcall CMenuCustomLobby::CConfirmBackMsgBoxButtonHandler::handler(
    CConfirmBackMsgBoxButtonHandler* thisptr,
    int /*%edx*/,
    game::CMidgardMsgBox* msgBox,
    bool okPressed)
{
    using namespace game;

    auto interfManager = thisptr->m_menu->interfaceData->interfManager.data;
    interfManager->CInterfManagerImpl::CInterfManager::vftable->hideInterface(interfManager,
                                                                              msgBox);
    if (msgBox) {
        msgBox->vftable->destructor(msgBox, 1);
    }

    if (okPressed) {
        auto menuPhase = thisptr->m_menu->menuBaseData->menuPhase;
        getOriginalFunctions().menuPhaseTransitionToMainOrCloseGame(menuPhase, true);
    }
}

CMenuCustomLobby::CRoomPasswordInterf::CRoomPasswordInterf(CMenuCustomLobby* menu)
    : CPopupDialogCustomBase{this, roomPasswordDialogName}
    , m_menu{menu}
{
    using namespace game;

    CPopupDialogInterfApi::get().constructor(this, roomPasswordDialogName, nullptr);

    setButtonCallback(*dialog, "BTN_CANCEL", cancelBtnHandler, this);
    setButtonCallback(*dialog, "BTN_OK", okBtnHandler, this);

    // Using EditFilter::Names for consistency with other game menus like CMenuNewSkirmishMulti
    setEditBoxData(*dialog, "EDIT_PASSWORD", EditFilter::Names,
                   gameRestrictions().passwordMaxLength, false);
}

void __fastcall CMenuCustomLobby::CRoomPasswordInterf::okBtnHandler(CRoomPasswordInterf* thisptr,
                                                                    int /*%edx*/)
{
    auto menu = thisptr->m_menu;
    if (menu->m_joiningRoomPassword == getEditBoxText(*thisptr->dialog, "EDIT_PASSWORD")) {
        CNetCustomService::get()->joinRoom(menu->m_joiningRoomId);
        menu->hideRoomPasswordDialog();
        menu->showWaitDialog();
    } else {
        auto msg{getInterfaceText(textIds().lobby.wrongRoomPassword.c_str())};
        if (msg.empty()) {
            msg = "Wrong room password";
        }
        showMessageBox(msg);
    }
}

void __fastcall CMenuCustomLobby::CRoomPasswordInterf::cancelBtnHandler(
    CRoomPasswordInterf* thisptr,
    int /*%edx*/)
{
    spdlog::debug("User canceled register account");
    thisptr->m_menu->hideRoomPasswordDialog();
}

} // namespace hooks
