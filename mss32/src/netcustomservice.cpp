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

#include "netcustomservice.h"
#include "lobbysaveexchange.h"
#include "mempool.h"
#include "midgard.h"
#include "midgardhooks.h"
#include "midgardmsgbox.h"
#include "midmsgboxbuttonhandler.h"
#include "mqnetservice.h"
#include "mquikernel.h"
#include "netcustompeer.h"
#include "netcustomsession.h"
#include "netmsg.h"
#include "settings.h"
#include "textids.h"
#include "utils.h"
#include "version.h"
#include <MessageIdentifiers.h>
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <string_view>
#include <usersettings.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {

namespace {

static constexpr std::uint32_t lobbyMaintenanceIntervalMs{250};
static constexpr std::size_t rememberedNoticesMax{256};
static constexpr UINT gameTextCodePage{1251};
/** Main-thread guard shared by packet draining and local lobby maintenance. */
bool peerProcessActive{};
/** Prevents native UI transitions from recursively processing deferred lobby state. */
bool lobbyStateProcessActive{};

struct MainThreadCallbackGuard
{
    explicit MainThreadCallbackGuard(bool& active)
        : active{active}
    {
        active = true;
    }

    ~MainThreadCallbackGuard()
    {
        active = false;
    }

    bool& active;
};

struct SystemNoticeMsgBoxButtonHandler : game::CMidMsgBoxButtonHandler
{ };

void __fastcall systemNoticeMsgBoxButtonHandlerDestructor(
    SystemNoticeMsgBoxButtonHandler* thisptr,
    int /*%edx*/,
    char flags)
{
    if (flags & 1) {
        game::Memory::get().freeNonZero(thisptr);
    }
}

void __fastcall systemNoticeMsgBoxButtonHandler(
    SystemNoticeMsgBoxButtonHandler* /*thisptr*/,
    int /*%edx*/,
    game::CMidgardMsgBox* msgBox,
    bool /*okPressed*/)
{
    // The handler itself deliberately owns no service or UI pointers. Both are resolved on the
    // main thread at dismissal time, so service/session teardown cannot leave a dangling capture.
    if (msgBox) {
        hideInterface(msgBox);
        msgBox->vftable->destructor(msgBox, 1);
    }
    if (auto service = CNetCustomService::get()) {
        service->notifySystemNoticeModalClosed();
    }
}

game::CMidMsgBoxButtonHandlerVftable systemNoticeMsgBoxButtonHandlerVftable{
    (game::CMidMsgBoxButtonHandlerVftable::Destructor)
        systemNoticeMsgBoxButtonHandlerDestructor,
    (game::CMidMsgBoxButtonHandlerVftable::Handler)systemNoticeMsgBoxButtonHandler,
};

std::optional<std::string> utf8ToGameEncoding(std::string_view text)
{
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    const auto wideLength{MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                               static_cast<int>(text.size()), nullptr, 0)};
    if (wideLength <= 0) {
        return std::nullopt;
    }

    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), wide.data(), wideLength)) {
        return std::nullopt;
    }

    BOOL usedDefaultChar{};
    const auto encodedLength{WideCharToMultiByte(
        gameTextCodePage, WC_NO_BEST_FIT_CHARS, wide.data(), wideLength, nullptr, 0, nullptr,
        &usedDefaultChar)};
    if (encodedLength <= 0 || usedDefaultChar) {
        return std::nullopt;
    }

    std::string encoded(static_cast<std::size_t>(encodedLength), '\0');
    usedDefaultChar = FALSE;
    if (!WideCharToMultiByte(gameTextCodePage, WC_NO_BEST_FIT_CHARS, wide.data(), wideLength,
                             encoded.data(), encodedLength, nullptr, &usedDefaultChar)
        || usedDefaultChar) {
        return std::nullopt;
    }

    return encoded;
}

bool isAuthenticatedLobbyPacket(const CNetCustomService* service, const SLNet::Packet* packet)
{
    return service && packet && service->loggedIn()
           && packet->guid != SLNet::UNASSIGNED_RAKNET_GUID
           && packet->guid == service->getLobbyGuid();
}

} // namespace

game::IMqNetServiceVftable CNetCustomService::g_vftable = {
    (game::IMqNetServiceVftable::Destructor)destructor,
    (game::IMqNetServiceVftable::HasSessions)hasSessions,
    (game::IMqNetServiceVftable::GetSessions)getSessions,
    (game::IMqNetServiceVftable::CreateSession)createSession,
    (game::IMqNetServiceVftable::JoinSession)joinSession,
};

CNetCustomService* CNetCustomService::get()
{
    auto midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data) {
        return nullptr;
    }
    auto service = midgard->data->netService;

    if (service && service->vftable == &g_vftable) {
        return static_cast<CNetCustomService*>(service);
    }

    return nullptr;
}

CNetCustomService::CNetCustomService()
    : m_peer(nullptr)
    , m_connected(false)
    , m_session(nullptr)
    , m_peerCallback(this)
    , m_lobbyCallback(this)
{
    spdlog::debug(__FUNCTION__);

    vftable = &g_vftable;

    // TODO: separate peers for lobby client, server and client players.
    // TODO: process server peer packets directly in server thread.
    // TODO: process players peer packets right inside IMqNetPlayer::ReceiveMessage.
    // Replacing peer queue constant polling (WM_TIMER) with notification message for better
    // efficiency
    // createTimerEvent(&m_peerProcessEvent, this, peerProcessEventCallback, peerProcessInterval);
    addPeerCallback(&m_peerCallback);
    createMessageEvent(&m_peerProcessEvent, this, peerProcessEventCallback, peerProcessMessageName);
    // Expires save transfers, waits for a safe pending-match UI transition and recovers a peer
    // notification that was nested inside a native UI callback. This never resends a packet.
    createTimerEvent(&m_lobbyMaintenanceTimerEvent, this,
                     lobbyMaintenanceTimerEventCallback, lobbyMaintenanceIntervalMs);

    m_peer = new CNetCustomPeer(peerProcessMessageName);
    // auto peer = SLNet::RakPeerInterface::GetInstance();
    m_peer->SetTimeoutTime(peerConnectionTimeout, SLNet::UNASSIGNED_SYSTEM_ADDRESS);

    m_lobbyClient.SetMessageFactory(&m_lobbyMsgFactory);
    m_lobbyClient.SetCallbackInterface(&m_lobbyCallback);

    m_roomsClient.SetRoomsCallback(&m_roomsCallback);
}

CNetCustomService::~CNetCustomService()
{
    spdlog::debug(__FUNCTION__);

    clearLobbyMatchState();
    m_peer->Shutdown(peerShutdownTimeout);
    SLNet::RakPeerInterface::DestroyInstance(m_peer);

    const auto& eventApi{game::UiEventApi::get()};
    eventApi.destructor(&m_lobbyMaintenanceTimerEvent);
    eventApi.destructor(&m_peerProcessEvent);
}

bool CNetCustomService::connect()
{
    if (!startPeer()) {
        return false;
    }

    const auto& serverIp = userSettings().lobby.server.ip;
    const auto& serverPort = userSettings().lobby.server.port;
    spdlog::debug(__FUNCTION__ ": connecting to lobby server with ip '{:s}', port {:d}", serverIp,
                  serverPort);
    switch (auto result = m_peer->Connect(serverIp.c_str(), serverPort, nullptr, 0)) {
    case SLNet::CONNECTION_ATTEMPT_STARTED:
        spdlog::debug(__FUNCTION__ ": lobby connection attempt started");
        break;
    case SLNet::ALREADY_CONNECTED_TO_ENDPOINT:
        spdlog::debug(__FUNCTION__ ": lobby is already connected");
        break;
    default:
        spdlog::error(__FUNCTION__ ": lobby connection attempt failed");
        return false;
    }

    return true;
}

bool CNetCustomService::startPeer()
{
    const auto& clientPort = userSettings().lobby.client.port;
    spdlog::debug(__FUNCTION__ ": starting lobby peer on port {:d}", clientPort);
    SLNet::SocketDescriptor socket{clientPort, nullptr};
    switch (auto result = m_peer->Startup(1, &socket, 1)) {
    case SLNet::RAKNET_STARTED:
        spdlog::debug(__FUNCTION__ ": lobby peer is started");
        break;
    case SLNet::RAKNET_ALREADY_STARTED:
        spdlog::debug(__FUNCTION__ ": lobby peer is already started");
        break;
    default:
        spdlog::error(__FUNCTION__ ": lobby peer has failed to start, result = {:d}", (int)result);
        return false;
    }

    // Some examples do this after peer creation, some - after startup. Does not seems to really
    // matter except that some plugins do some stuff on attaching if the peer is already started.
    m_peer->AttachPlugin(&m_lobbyClient);
    m_peer->AttachPlugin(&m_roomsClient);

    return true;
}

CNetCustomSession* CNetCustomService::getSession() const
{
    return m_session;
}

const std::string& CNetCustomService::getUserName() const
{
    return m_userName;
}

bool CNetCustomService::connected() const
{
    // TODO: try m_peer->GetConnectionState(getLobbyGuid()) == SLNet::ConnectionState::IS_CONNECTED
    return m_connected;
}

bool CNetCustomService::loggedIn() const
{
    // TODO: try using Lobby2Presence instead
    return m_connected && !m_userName.empty();
}

const SLNet::RakNetGUID CNetCustomService::getPeerGuid() const
{
    return m_peer->GetMyGUID();
}

const SLNet::RakNetGUID CNetCustomService::getLobbyGuid() const
{
    return m_peer->GetGuidFromSystemAddress(m_lobbyClient.GetServerAddress());
}

bool CNetCustomService::send(const SLNet::BitStream& stream,
                             const SLNet::RakNetGUID& to,
                             PacketPriority priority) const
{
    if (!m_peer->Send(&stream, priority, PacketReliability::RELIABLE_ORDERED, 0, to, false)) {
        spdlog::debug(__FUNCTION__ ": failed on bad input");
        return false;
    }

    return true;
}

bool CNetCustomService::registerAccount(const char* userName, const char* password)
{
    spdlog::debug(__FUNCTION__);

    if (!userName) {
        spdlog::debug(__FUNCTION__ ": empty account name");
        return false;
    }

    if (!password) {
        spdlog::debug(__FUNCTION__ ": empty password");
        return false;
    }

    auto msg{m_lobbyMsgFactory.Alloc(SLNet::L2MID_Client_RegisterAccount)};
    auto account{static_cast<SLNet::Client_RegisterAccount*>(msg)};
    if (!account) {
        spdlog::debug(__FUNCTION__ ": failed to allocate message");
        return false;
    }

    account->userName = userName;
    account->titleName = titleName;

    auto& params = account->createAccountParameters;
    params.password = password;

    const auto result{account->PrevalidateInput()};
    if (!result) {
        spdlog::debug(__FUNCTION__ ": input validation failed");
    } else {
        m_lobbyClient.SendMsg(account);
        spdlog::debug(__FUNCTION__ ": register message sent");
    }

    m_lobbyMsgFactory.Dealloc(account);
    return result;
}

bool CNetCustomService::login(const char* userName, const char* password)
{
    spdlog::debug(__FUNCTION__);

    if (!userName) {
        return false;
    }

    if (!password) {
        return false;
    }

    auto msg{m_lobbyMsgFactory.Alloc(SLNet::L2MID_Client_Login)};
    auto login{static_cast<SLNet::Client_Login*>(msg)};
    if (!login) {
        spdlog::debug(__FUNCTION__ ": failed to allocate message");
        return false;
    }

    login->userName = userName;
    login->userPassword = password;

    login->titleName = titleName;
    login->titleSecretKey = titleSecretKey;

    const auto result{login->PrevalidateInput()};
    if (!result) {
        spdlog::debug(__FUNCTION__ ": input validation failed");
    } else {
        m_lobbyClient.SendMsg(login);
        spdlog::debug(__FUNCTION__ ": login message sent");
    }

    m_lobbyMsgFactory.Dealloc(login);
    return result;
}

void CNetCustomService::logoff()
{
    spdlog::debug(__FUNCTION__);

    auto msg{m_lobbyMsgFactory.Alloc(SLNet::L2MID_Client_Logoff)};
    auto logoff{static_cast<SLNet::Client_Logoff*>(msg)};
    if (!logoff) {
        spdlog::debug(__FUNCTION__ ": failed to allocate message");
        return;
    }

    m_lobbyClient.SendMsg(logoff);
    m_lobbyMsgFactory.Dealloc(logoff);
    spdlog::debug(__FUNCTION__ ": logoff message sent");
}

void CNetCustomService::sendChatMessage(const char* text)
{
    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_CHAT_MESSAGE));
    stream.Write(SLNet::RakString(m_userName.c_str()));
    stream.Write(SLNet::RakString(text));
    send(stream, getLobbyGuid(), LOW_PRIORITY);
}

CNetCustomService::ChatMessage CNetCustomService::readChatMessage(const SLNet::Packet* packet)
{
    spdlog::debug(__FUNCTION__);

    using namespace SLNet;

    BitStream stream{packet->data, packet->length, false};
    stream.IgnoreBytes(sizeof(MessageID));

    RakString sender;
    if (!stream.Read(sender)) {
        spdlog::debug(__FUNCTION__ ": failed to read chat message sender");
        return {};
    }

    RakString text;
    if (!stream.Read(text)) {
        spdlog::debug(__FUNCTION__ ": failed to read chat message text");
        return {};
    }

    return {sender, text};
}

void CNetCustomService::queryOnlineUsers()
{
    spdlog::debug(__FUNCTION__);

    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_GET_ONLINE_USERS_REQUEST));
    send(stream, getLobbyGuid(), LOW_PRIORITY);
}

std::vector<CNetCustomService::UserInfo> CNetCustomService::readOnlineUsers(
    const SLNet::Packet* packet)
{
    spdlog::debug(__FUNCTION__);

    using namespace SLNet;

    BitStream stream{packet->data, packet->length, false};
    stream.IgnoreBytes(sizeof(MessageID));

    unsigned int count;
    if (!stream.Read(count)) {
        spdlog::debug(__FUNCTION__ ": failed to read online users count");
        return {};
    }

    std::vector<UserInfo> result;
    result.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        RakNetGUID guid;
        if (!stream.Read(guid)) {
            spdlog::debug(__FUNCTION__ ": failed to read online user guid");
            return {};
        }

        RakString name;
        if (!stream.Read(name)) {
            spdlog::debug(__FUNCTION__ ": failed to read online user name");
            return {};
        }

        result.push_back({guid, name});
    }

    return result;
}

void CNetCustomService::queryChatMessages()
{
    spdlog::debug(__FUNCTION__);

    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_GET_CHAT_MESSAGES_REQUEST));
    send(stream, getLobbyGuid(), LOW_PRIORITY);
}

std::vector<CNetCustomService::ChatMessage> CNetCustomService::readChatMessages(
    const SLNet::Packet* packet)
{
    spdlog::debug(__FUNCTION__);

    using namespace SLNet;

    BitStream stream{packet->data, packet->length, false};
    stream.IgnoreBytes(sizeof(MessageID));

    unsigned int count;
    if (!stream.Read(count)) {
        spdlog::debug(__FUNCTION__ ": failed to read chat message count");
        return {};
    }

    std::vector<ChatMessage> result;
    result.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        RakString sender;
        if (!stream.Read(sender)) {
            spdlog::debug(__FUNCTION__ ": failed to read chat message sender");
            return {};
        }

        RakString text;
        if (!stream.Read(text)) {
            spdlog::debug(__FUNCTION__ ": failed to read chat message text");
            return {};
        }

        result.push_back({sender, text});
    }

    return result;
}

void CNetCustomService::setTemplateInfo(const std::string& name)
{
    m_templateName = name;
    m_templateHash.clear();

    if (name.empty()) {
        return;
    }

    const auto templatePath = templatesFolder() / name;

    if (std::filesystem::exists(templatePath)) {
        m_templateHash = computeHash({templatePath});
    }
}

const std::string& CNetCustomService::getTemplateName() const
{
    return m_templateName;
}

const std::string& CNetCustomService::getTemplateHash()
{
    if (m_templateHash.empty() && !m_templateName.empty()) {
        m_templateHash = computeTemplateHash(m_templateName);
    }

    return m_templateHash;
}

std::string CNetCustomService::computeTemplateHash(const std::string& templateName) const
{
    auto file = templatesFolder() / templateName;

    if (!std::filesystem::exists(file))
        return {};

    return computeHash({file});
}

bool CNetCustomService::createRoom(const char* gameName,
                                   const char* scenarioName,
                                   const char* scenarioDescription,
                                   const char* password)
{
    spdlog::debug(__FUNCTION__ ": game name = '{:s}', password = '{:s}'", gameName, password);

    const auto& filesHash = getGameFilesHash();
    if (filesHash.empty()) {
        spdlog::debug(__FUNCTION__ ": failed because the game files hash is empty");
        return false;
    }

    auto gameVersion{getInterfaceText(textIds().lobby.gameVersion.c_str())};
    if (gameVersion.empty()) {
        // v.3.01
        gameVersion = getInterfaceText("X150TA0026");
    }

    SLNet::CreateRoom_Func room{};
    room.userName = m_userName.c_str();
    room.gameIdentifier = titleName;

    auto& params = room.networkedRoomCreationParameters;
    params.destroyOnModeratorLeave = true;
    // SLNet demands room name to be unique, so a game name is inconvenient to use for this
    // purpose (because it defaults to scenario name, and multiple players can create rooms using
    // the same scenario receiving the error REC_ROOM_CREATION_PARAMETERS_ROOM_NAME_IN_USE).
    params.roomName = room.userName;
    params.slots.publicSlots = 1;
    params.slots.reservedSlots = 0;
    params.slots.spectatorSlots = 0;

    auto& properties = room.initialRoomProperties;
    auto hashColumn{properties.AddColumn(gameFilesHashColumnName, DataStructures::Table::STRING)};
    auto versionColumn{properties.AddColumn(gameVersionColumnName, DataStructures::Table::STRING)};
    auto gameNameColumn{properties.AddColumn(gameNameColumnName, DataStructures::Table::STRING)};
    auto passwordColumn{properties.AddColumn(passwordColumnName, DataStructures::Table::STRING)};
    auto scenNameColumn{
        properties.AddColumn(scenarioNameColumnName, DataStructures::Table::STRING)};
    auto scenDescColumn{
        properties.AddColumn(scenarioDescriptionColumnName, DataStructures::Table::STRING)};

    auto templateNameColumn{
        properties.AddColumn(templateNameColumnName, DataStructures::Table::STRING)};

    auto templateHashColumn{
        properties.AddColumn(templateHashColumnName, DataStructures::Table::STRING)};

    auto rankedColumn{properties.AddColumn(rankedColumnName, DataStructures::Table::STRING)};
    auto simTurnsDaysColumn{
        properties.AddColumn(simultaneousTurnsDaysColumnName, DataStructures::Table::STRING)};
    auto unlockGuiColumn{properties.AddColumn(unlockGuiColumnName, DataStructures::Table::STRING)};

    const auto& templateName = getTemplateName();
    const auto& templateHash = getTemplateHash();
    const auto effectiveSimTurnsDays{m_roomOptions.effectiveSimultaneousTurnsDays()};
    const auto simTurnsDays{std::to_string(effectiveSimTurnsDays)};

    auto row = properties.AddRow(0);
    row->UpdateCell(hashColumn, filesHash.c_str());
    row->UpdateCell(templateNameColumn, templateName.c_str());
    row->UpdateCell(templateHashColumn, templateHash.c_str());
    row->UpdateCell(versionColumn, gameVersion.c_str());
    row->UpdateCell(gameNameColumn, gameName);
    row->UpdateCell(passwordColumn, password);
    row->UpdateCell(scenNameColumn, scenarioName);
    row->UpdateCell(scenDescColumn, scenarioDescription);
    const bool ranked{hooks::gameVersion() == GameVersion::Russobit && m_roomOptions.ranked};
    row->UpdateCell(rankedColumn, ranked ? "1" : "0");
    row->UpdateCell(simTurnsDaysColumn, simTurnsDays.c_str());
    row->UpdateCell(unlockGuiColumn, m_roomOptions.unlockGui ? "1" : "0");

    m_roomsClient.ExecuteFunc(&room);
    return true;
}

void CNetCustomService::leaveRoom()
{
    spdlog::debug(__FUNCTION__);

    SLNet::LeaveRoom_Func func{};
    func.userName = m_userName.c_str();
    m_roomsClient.ExecuteFunc(&func);
}

void CNetCustomService::searchRooms()
{
    spdlog::debug(__FUNCTION__);

    SLNet::SearchByFilter_Func func{};
    func.gameIdentifier = titleName;
    func.userName = m_userName.c_str();
    func.onlyJoinable = true;
    m_roomsClient.ExecuteFunc(&func);
}

void CNetCustomService::joinRoom(SLNet::RoomID id)
{
    spdlog::debug(__FUNCTION__ ": room id = {:d}", id);

    SLNet::JoinByFilter_Func func{};
    func.gameIdentifier = titleName;
    func.userName = m_userName.c_str();
    func.query.AddQuery_NUMERIC(DefaultRoomColumns::GetColumnName(DefaultRoomColumns::TC_ROOM_ID),
                                id);
    func.roomMemberMode = RMM_PUBLIC;
    m_roomsClient.ExecuteFunc(&func);
}

bool CNetCustomService::changeRoomPublicSlots(unsigned int publicSlots)
{
    spdlog::debug(__FUNCTION__ ": public slots = {:d}", publicSlots);

    if (publicSlots < 1) {
        spdlog::debug(__FUNCTION__ ": could not set number of room public slots lesser than 1");
        return false;
    }

    SLNet::ChangeSlotCounts_Func func{};
    func.userName = m_userName.c_str();
    func.slots.publicSlots = publicSlots;
    m_roomsClient.ExecuteFunc(&func);
    return true;
}

void CNetCustomService::addPeerCallback(NetPeerCallback* callback)
{
    spdlog::debug(__FUNCTION__);

    std::lock_guard lock(m_peerCallbacksMutex);
    if (std::find(m_peerCallbacks.begin(), m_peerCallbacks.end(), callback)
        == m_peerCallbacks.end()) {
        m_peerCallbacks.push_back(callback);
    }
}

void CNetCustomService::removePeerCallback(NetPeerCallback* callback)
{
    spdlog::debug(__FUNCTION__);

    std::lock_guard lock(m_peerCallbacksMutex);
    m_peerCallbacks.erase(std::remove(m_peerCallbacks.begin(), m_peerCallbacks.end(), callback),
                          m_peerCallbacks.end());
}

void CNetCustomService::addLobbyCallback(SLNet::Lobby2Callbacks* callback)
{
    spdlog::debug(__FUNCTION__);

    m_lobbyClient.AddCallbackInterface(callback);
}

void CNetCustomService::removeLobbyCallback(SLNet::Lobby2Callbacks* callback)
{
    spdlog::debug(__FUNCTION__);

    m_lobbyClient.RemoveCallbackInterface(callback);
}

void CNetCustomService::addRoomsCallback(SLNet::RoomsCallback* callback)
{
    spdlog::debug(__FUNCTION__);

    m_roomsClient.AddRoomsCallback(callback);
}

void CNetCustomService::removeRoomsCallback(SLNet::RoomsCallback* callback)
{
    spdlog::debug(__FUNCTION__);

    m_roomsClient.RemoveRoomsCallback(callback);
}

void __fastcall CNetCustomService::destructor(CNetCustomService* thisptr, int /*%edx*/, char flags)
{
    thisptr->~CNetCustomService();

    if (flags & 1) {
        spdlog::debug(__FUNCTION__ ": freeing memory");
        game::Memory::get().freeNonZero(thisptr);
    }
}

bool __fastcall CNetCustomService::hasSessions(CNetCustomService* thisptr, int /*%edx*/)
{
    spdlog::debug(__FUNCTION__);

    return false;
}

void __fastcall CNetCustomService::getSessions(CNetCustomService* thisptr,
                                               int /*%edx*/,
                                               game::List<game::IMqNetSessEnum*>* sessions,
                                               const GUID* appGuid,
                                               const char* ipAddress,
                                               bool allSessions,
                                               bool requirePassword)
{
    // This method used by vanilla interface.
    // Since we have our custom one, we can ignore it and there is no need to implement.
    spdlog::debug(__FUNCTION__);
}

void __fastcall CNetCustomService::createSession(CNetCustomService* thisptr,
                                                 int /*%edx*/,
                                                 game::IMqNetSession** netSession,
                                                 const GUID* /* appGuid */,
                                                 const char* sessionName,
                                                 const char* /* password */)
{
    spdlog::debug(__FUNCTION__ ": session name = {:s}", sessionName);

    thisptr->clearLobbyMatchState();

    auto session = (CNetCustomSession*)game::Memory::get().allocate(sizeof(CNetCustomSession));
    new (session) CNetCustomSession(thisptr, sessionName, thisptr->getPeerGuid());
    thisptr->m_session = session;
    *netSession = session;
}

void __fastcall CNetCustomService::joinSession(CNetCustomService* thisptr,
                                               int /*%edx*/,
                                               game::IMqNetSession** netSession,
                                               CNetCustomSessEnum* netSessionEnum,
                                               const char* /*password*/)
{
    spdlog::debug(__FUNCTION__ ": session name = {:s}", netSessionEnum->sessionName);

    thisptr->clearLobbyMatchState();

    auto session = (CNetCustomSession*)game::Memory::get().allocate(sizeof(CNetCustomSession));
    new (session)
        CNetCustomSession(thisptr, netSessionEnum->sessionName.c_str(), netSessionEnum->serverGuid);
    thisptr->m_session = session;
    *netSession = session;
}

void __fastcall CNetCustomService::peerProcessEventCallback(const CNetCustomService* /*thisptr*/,
                                                            int /*%edx*/,
                                                            unsigned int,
                                                            long)
{
    spdlog::debug(__FUNCTION__);

    auto service = get();
    if (!service) {
        // MSDN: "The KillTimer function does not remove WM_TIMER messages already posted to the
        // message queue." Thus we can end up crashing if WM_TIMER is processed after service
        // destruction.
        spdlog::debug(__FUNCTION__ ": preventing processing callback after service destruction");
        return;
    }

    if (peerProcessActive || lobbyStateProcessActive) {
        // Native callbacks can pump messages and timers.  Local maintenance will drain deferred
        // UI state after the outer callback returns; no packet is resent here.
        return;
    }

    auto peer = service->m_peer;
    {
        MainThreadCallbackGuard callbackGuard{peerProcessActive};
        for (auto packet = peer->Receive(); packet != nullptr;
             peer->DeallocatePacket(packet), packet = peer->Receive()) {

            auto type = static_cast<DefaultMessageIDTypes>(packet->data[0]);
            auto callbacks = service->getPeerCallbacks();
            for (auto& callback : callbacks) {
                callback->onPacketReceived(type, peer, packet);
            }
        }

        // MATCH_ENDED itself is deferred below. Other native observers can still replace the
        // service, so acknowledge only through the peer that is still installed.
        auto currentService = get();
        if (currentService != service || currentService->m_peer != peer) {
            return;
        }
        peer->CompletePacketProcessing();
    }

    if (get() == service) {
        service->processDeferredLobbyState();
    }
}

std::vector<NetPeerCallback*> CNetCustomService::getPeerCallbacks() const
{
    std::lock_guard lock(m_peerCallbacksMutex);
    return m_peerCallbacks;
}

const std::string& CNetCustomService::getGameFilesHash()
{
    if (m_gameFilesHash.empty()) {
        m_gameFilesHash = computeHash(getGameFilesToHash());
        if (m_gameFilesHash.empty()) {
            spdlog::debug(__FUNCTION__ ": failed to compute hash of game files");
        }
    }

    return m_gameFilesHash;
}

std::vector<std::filesystem::path> CNetCustomService::getGameFilesToHash() const
{
    std::vector<std::filesystem::path> result{{gameFolder() / "mss32.dll"}};

    for (const auto& entry : std::filesystem::recursive_directory_iterator(globalsFolder())) {
        if (!entry.is_regular_file()) {
            continue;
        }

        static const std::array<std::filesystem::path, 4> excludeGlobals{
            {globalsFolder() / "Tglobal.dbf", globalsFolder() / "TAiMsg.dbf",
             globalsFolder() / "Tleader.dbf", globalsFolder() / "Tplayer.dbf"}};
        if (std::find_if(excludeGlobals.begin(), excludeGlobals.end(),
                         [&entry](const std::filesystem::path& excluded) {
                             return std::filesystem::equivalent(excluded, entry);
                         })
            != excludeGlobals.end()) {
            continue;
        }

        result.push_back(entry.path());
    }

    // Scripts might be absent in case of pure vanilla (even without settings.lua)
    auto scripts = scriptsFolder();
    if (std::filesystem::is_directory(scripts)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(scripts)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            if (entry.path().filename() == "userSettings.lua") {
                continue;
            }

            result.push_back(entry.path());
        }
    }

    return result;
}

CNetCustomService::UserInfo CNetCustomService::getUserInfo() const
{
    return {getPeerGuid(), getUserName().c_str()};
}

CNetCustomService::RoomOptions& CNetCustomService::getRoomOptions()
{
    return m_roomOptions;
}

void CNetCustomService::processPeerMessages() const
{
    peerProcessEventCallback(this, 0, 0, 0);
}

bool CNetCustomService::readSaveRequest(const SLNet::Packet* packet,
                                        LobbyProtocol::SaveRequestV2& request) const
{
    using namespace LobbyProtocol;

    if (!packet || !packet->data || packet->length < sizeof(SLNet::MessageID)) {
        spdlog::warn(__FUNCTION__ ": malformed save request packet");
        return false;
    }
    if (!isAuthenticatedLobbyPacket(this, packet)) {
        spdlog::warn(__FUNCTION__ ": refusing save request not authenticated by lobby session");
        return false;
    }

    SLNet::BitStream stream{packet->data, packet->length, false};
    stream.IgnoreBytes(sizeof(SLNet::MessageID));

    std::uint8_t version{};
    std::uint8_t role{};
    if (!stream.Read(version) || !stream.Read(request.saveId) || !stream.Read(role)
        || !stream.Read(request.maxBytes) || !stream.Read(request.timeoutMs)) {
        spdlog::warn(__FUNCTION__ ": malformed save request");
        if (request.saveId) {
            sendLobbySaveFailure(request.saveId, SaveFailureV2::MalformedRequest);
        }
        return false;
    }

    if (stream.GetNumberOfUnreadBits() != 0) {
        spdlog::warn(__FUNCTION__ ": save request has trailing data");
        if (request.saveId) {
            sendLobbySaveFailure(request.saveId, SaveFailureV2::MalformedRequest);
        }
        return false;
    }

    if (version != saveTransferVersion) {
        spdlog::warn(__FUNCTION__ ": unsupported save protocol version {:d}",
                     static_cast<int>(version));
        sendLobbySaveFailure(request.saveId, SaveFailureV2::UnsupportedVersion);
        return false;
    }
    if (role > static_cast<std::uint8_t>(SaveRoleV2::Joiner)) {
        sendLobbySaveFailure(request.saveId, SaveFailureV2::MalformedRequest);
        return false;
    }

    request.role = static_cast<SaveRoleV2>(role);
    return true;
}

bool CNetCustomService::readSaveStoredAck(const SLNet::Packet* packet,
                                          std::uint64_t& saveId) const
{
    using namespace LobbyProtocol;

    constexpr std::size_t packetSize{sizeof(SLNet::MessageID) + sizeof(std::uint8_t)
                                     + sizeof(std::uint64_t)};
    if (!packet || !packet->data || packet->length != packetSize
        || !isAuthenticatedLobbyPacket(this, packet)) {
        spdlog::warn(__FUNCTION__ ": refusing malformed or unauthenticated stored ACK");
        return false;
    }

    SLNet::BitStream stream{packet->data, packet->length, false};
    stream.IgnoreBytes(sizeof(SLNet::MessageID));
    std::uint8_t version{};
    if (!stream.Read(version) || !stream.Read(saveId) || version != saveTransferVersion
        || saveId == 0 || stream.GetNumberOfUnreadBits() != 0) {
        spdlog::warn(__FUNCTION__ ": invalid stored ACK payload");
        return false;
    }
    return true;
}

bool CNetCustomService::readSystemNotice(const SLNet::Packet* packet,
                                         LobbyProtocol::SystemNoticeV1& notice) const
{
    using namespace LobbyProtocol;

    if (!packet || !packet->data || packet->length < sizeof(SLNet::MessageID)) {
        spdlog::warn(__FUNCTION__ ": malformed system notice packet");
        return false;
    }
    if (!isAuthenticatedLobbyPacket(this, packet)) {
        spdlog::warn(__FUNCTION__ ": refusing system notice not authenticated by lobby session");
        return false;
    }

    SLNet::BitStream stream{packet->data, packet->length, false};
    stream.IgnoreBytes(sizeof(SLNet::MessageID));

    std::uint8_t version{};
    std::uint8_t presentation{};
    std::uint16_t textLength{};
    if (!stream.Read(version) || !stream.Read(notice.noticeId) || !stream.Read(presentation)
        || !stream.Read(textLength)) {
        spdlog::warn(__FUNCTION__ ": malformed system notice header");
        return false;
    }

    if (version != systemNoticeVersion || notice.noticeId == 0
        || presentation > static_cast<std::uint8_t>(SystemNoticePresentation::Modal)
        || textLength == 0 || textLength > systemNoticeTextMax
        || stream.GetNumberOfUnreadBits() < static_cast<SLNet::BitSize_t>(textLength) * 8) {
        spdlog::warn(__FUNCTION__ ": invalid system notice metadata");
        return false;
    }

    notice.presentation = static_cast<SystemNoticePresentation>(presentation);
    notice.text.resize(textLength);
    if (!stream.ReadAlignedBytes(reinterpret_cast<unsigned char*>(notice.text.data()),
                                 textLength)
        || notice.text.find('\0') != std::string::npos
        || stream.GetNumberOfUnreadBits() != 0) {
        spdlog::warn(__FUNCTION__ ": invalid system notice text");
        return false;
    }

    // Validate UTF-8 before retaining the notice for a later modal.
    return utf8ToGameEncoding(notice.text).has_value();
}

void CNetCustomService::enqueueSystemNotice(LobbyProtocol::SystemNoticeV1 notice)
{
    if (!m_seenSystemNotices.insert(notice.noticeId).second) {
        return;
    }

    m_seenSystemNoticeOrder.push_back(notice.noticeId);
    if (m_seenSystemNoticeOrder.size() > rememberedNoticesMax) {
        m_seenSystemNotices.erase(m_seenSystemNoticeOrder.front());
        m_seenSystemNoticeOrder.pop_front();
    }

    m_pendingSystemNotices.push_back(std::move(notice));
}

void CNetCustomService::processDeferredLobbyState()
{
    if (peerProcessActive || lobbyStateProcessActive || m_systemNoticeModalActive) {
        return;
    }

    MainThreadCallbackGuard callbackGuard{lobbyStateProcessActive};
    if (m_matchEndPending) {
        processPendingMatchEnd();
        return;
    }

    processPendingSystemNotices();
}

void CNetCustomService::processPendingMatchEnd()
{
    if (!m_matchEndPending) {
        return;
    }

    if (!loggedIn() || !getSession()) {
        m_matchEndPending = false;
        return;
    }

    auto midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data) {
        return;
    }
    if (!midgard->data->multiplayerGame || !midgard->data->gameIsRunning
        || !midgard->data->client) {
        m_matchEndPending = false;
        return;
    }

    m_matchEndPending = false;
    terminateLobbySaveTransfers();

    // Active gameplay normally has menuPhase == nullptr. MID_STARTMENU performs the native
    // game/network teardown and creates a fresh menu phase.
    midgardStartMenuMessageCallbackHooked(midgard, 0, 0, 0);
}

void CNetCustomService::processPendingSystemNotices()
{
    while (!m_systemNoticeModalActive && !m_pendingSystemNotices.empty()) {
        auto notice{std::move(m_pendingSystemNotices.front())};
        m_pendingSystemNotices.pop_front();
        if (displaySystemNotice(notice)) {
            return;
        }
    }
}

void CNetCustomService::notifySystemNoticeModalClosed()
{
    if (!m_systemNoticeModalActive) {
        return;
    }

    m_systemNoticeModalActive = false;
    processDeferredLobbyState();
}

void CNetCustomService::notifySessionDestroyed(CNetCustomSession* session)
{
    if (m_session != session) {
        return;
    }

    m_session = nullptr;
    clearLobbyMatchState();
}

void CNetCustomService::clearLobbyMatchState()
{
    resetLobbySaveTransferState();
    m_matchEndPending = false;
}

bool CNetCustomService::showSystemNoticeModal(const std::string& text)
{
    if (m_systemNoticeModalActive) {
        return false;
    }
    m_systemNoticeModalActive = true;

    auto handler = static_cast<SystemNoticeMsgBoxButtonHandler*>(
        game::Memory::get().allocate(sizeof(SystemNoticeMsgBoxButtonHandler)));
    handler->vftable = &systemNoticeMsgBoxButtonHandlerVftable;
    showMessageBox(text, handler, false);
    return true;
}

bool CNetCustomService::displaySystemNotice(const LobbyProtocol::SystemNoticeV1& notice)
{
    const auto encoded{utf8ToGameEncoding(notice.text)};
    if (!encoded) {
        spdlog::warn(__FUNCTION__
                     ": notice {:016x} is invalid UTF-8 or cannot be represented in Windows-1251",
                     notice.noticeId);
        return false;
    }

    if (notice.presentation != LobbyProtocol::SystemNoticePresentation::Modal) {
        spdlog::warn(__FUNCTION__ ": refusing non-modal system-notice packet");
        return false;
    }
    return showSystemNoticeModal(*encoded);
}

void __fastcall CNetCustomService::lobbyMaintenanceTimerEventCallback(
    CNetCustomService* /*thisptr*/,
    int /*%edx*/)
{
    auto service = get();
    if (!service || peerProcessActive || lobbyStateProcessActive) {
        // A timer notification may already be queued when native service teardown removes the
        // event. Resolve the current instance instead of trusting the callback's raw userdata.
        return;
    }

    {
        MainThreadCallbackGuard callbackGuard{lobbyStateProcessActive};
        expireLobbySaveTransfers();
    }

    service = get();
    if (!service) {
        return;
    }

    if (service->m_peer->IsPacketNotificationSent()) {
        peerProcessEventCallback(service, 0, 0, 0);
    } else {
        service->processDeferredLobbyState();
    }
}

void CNetCustomService::PeerCallback::onPacketReceived(DefaultMessageIDTypes type,
                                                       SLNet::RakPeerInterface* peer,
                                                       const SLNet::Packet* packet)
{
    switch (type) {
    case ID_CONNECTION_REQUEST_ACCEPTED: {
        spdlog::debug(__FUNCTION__ ": connection request accepted, set server address");
        // Make sure plugins know about the server
        m_service->m_lobbyClient.SetServerAddress(packet->systemAddress);
        m_service->m_roomsClient.SetServerAddress(packet->systemAddress);
        m_service->m_connected = true;
        break;
    }
    case ID_CONNECTION_ATTEMPT_FAILED:
        spdlog::debug(__FUNCTION__ ": connection attempt failed");
        break;
    case ID_ALREADY_CONNECTED:
        spdlog::debug(__FUNCTION__ ": already connected (should never happen)");
        break;
    case ID_NO_FREE_INCOMING_CONNECTIONS:
        spdlog::debug(__FUNCTION__ ": server is full");
        break;
    case ID_DISCONNECTION_NOTIFICATION:
        spdlog::debug(__FUNCTION__ ": server was shut down");
        m_service->m_connected = false;
        m_service->m_userName.clear();
        m_service->clearLobbyMatchState();
        break;
    case ID_CONNECTION_LOST:
        spdlog::debug(__FUNCTION__ ": connection with server is lost");
        m_service->m_connected = false;
        m_service->m_userName.clear();
        m_service->clearLobbyMatchState();
        break;
    case ID_LOBBY2_SERVER_ERROR:
        spdlog::debug(__FUNCTION__ ": lobby server error");
        break;
    case ID_ROOMS_EXECUTE_FUNC:
        spdlog::debug(__FUNCTION__ ": room function executed");
        break;
    case ID_LOBBY_SAVE_REQUEST: {
        LobbyProtocol::SaveRequestV2 request{};
        if (m_service->readSaveRequest(packet, request)) {
            handleLobbySaveRequest(request);
        }
        break;
    }
    case ID_LOBBY_SAVE_STORED_ACK: {
        std::uint64_t saveId{};
        if (m_service->readSaveStoredAck(packet, saveId)) {
            // Peer packets are drained only by the UI-event callback (or the same main-thread
            // teardown/watchdog path). Apply the ACK synchronously before a later RELIABLE_ORDERED
            // MATCH_ENDED packet can reset the transfer state.
            handleLobbySaveStoredAck(saveId);
        }
        break;
    }
    case ID_LOBBY_SYSTEM_NOTICE: {
        LobbyProtocol::SystemNoticeV1 notice{};
        if (m_service->readSystemNotice(packet, notice)) {
            m_service->enqueueSystemNotice(std::move(notice));
        }
        break;
    }
    case ID_LOBBY_MATCH_ENDED:
        if (!isAuthenticatedLobbyPacket(m_service, packet)) {
            spdlog::warn(__FUNCTION__ ": refusing MATCH_ENDED not authenticated by lobby session");
        } else if (packet->data && packet->length == sizeof(SLNet::MessageID)) {
            spdlog::info(__FUNCTION__ ": received lobby MATCH_ENDED");
            m_service->m_matchEndPending = true;
        } else {
            spdlog::warn(__FUNCTION__ ": malformed match-ended packet");
        }
        break;
    default:
        // Log user messages explicitly to avoid cluttering the log
        if (type < ID_USER_PACKET_ENUM) {
            spdlog::debug(__FUNCTION__ ": packet type {:d}", static_cast<int>(type));
        }
        break;
    }
}

void CNetCustomService::LobbyCallback::MessageResult(SLNet::Client_Login* message)
{
    if (message->resultCode == SLNet::L2RC_SUCCESS) {
        m_service->m_userName = message->userName.C_String();
    }

    ExecuteDefaultResult(message);
}

void CNetCustomService::LobbyCallback::MessageResult(SLNet::Client_Logoff* message)
{
    if (message->resultCode == SLNet::L2RC_SUCCESS) {
        m_service->m_userName.clear();
        m_service->clearLobbyMatchState();
    }

    ExecuteDefaultResult(message);
}

void CNetCustomService::LobbyCallback::MessageResult(
    SLNet::Notification_Client_RemoteLogin* message)
{
    if (message->resultCode == SLNet::L2RC_SUCCESS) {
        if (m_service->m_userName == message->handle.C_String()) {
            // The same account is remotely logged-in, means that we are now logged out
            m_service->m_userName.clear();
            m_service->clearLobbyMatchState();
        }
    }

    ExecuteDefaultResult(message);
}

void CNetCustomService::LobbyCallback::ExecuteDefaultResult(SLNet::Lobby2Message* msg)
{
    // To optimize out DebugMsg call
    if (!gameSettings().debugMode) {
        return;
    }

    SLNet::RakString str;
    msg->DebugMsg(str);

    spdlog::debug(str.C_String());
}

void CNetCustomService::RoomsCallback::CreateRoom_Callback(
    const SLNet::SystemAddress& senderAddress,
    SLNet::CreateRoom_Func* callResult)
{
    ExecuteDefaultResult("CreateRoom", callResult->resultCode, callResult->roomId,
                         &callResult->roomDescriptor);
}

void CNetCustomService::RoomsCallback::EnterRoom_Callback(const SLNet::SystemAddress& senderAddress,
                                                          SLNet::EnterRoom_Func* callResult)
{
    ExecuteDefaultResult("EnterRoom", callResult->resultCode, callResult->roomId,
                         &callResult->joinedRoomResult.roomDescriptor);
}

void CNetCustomService::RoomsCallback::LeaveRoom_Callback(const SLNet::SystemAddress& senderAddress,
                                                          SLNet::LeaveRoom_Func* callResult)
{
    auto roomId = callResult->removeUserResult.roomId;
    ExecuteDefaultResult("LeaveRoom", callResult->resultCode, roomId);
}

void CNetCustomService::RoomsCallback::RoomMemberLeftRoom_Callback(
    const SLNet::SystemAddress& senderAddress,
    SLNet::RoomMemberLeftRoom_Notification* notification)
{
    ExecuteDefaultResult("RoomMemberLeftRoom", SLNet::REC_SUCCESS, notification->roomId);
}

void CNetCustomService::RoomsCallback::RoomMemberJoinedRoom_Callback(
    const SLNet::SystemAddress& senderAddress,
    SLNet::RoomMemberJoinedRoom_Notification* notification)
{
    ExecuteDefaultResult("RoomMemberJoinedRoom", SLNet::REC_SUCCESS, notification->roomId);
}

void CNetCustomService::RoomsCallback::ExecuteDefaultResult(
    const char* callbackName,
    SLNet::RoomsErrorCode resultCode,
    SLNet::RoomID roomId,
    SLNet::RoomDescriptor* roomDescriptor) const
{
    switch (resultCode) {
    case SLNet::REC_SUCCESS: {
        // Descriptor is only filled on success
        auto roomName = roomDescriptor
                            ? roomDescriptor->GetProperty(DefaultRoomColumns::TC_ROOM_NAME)->c
                            : "";
        spdlog::debug("{:s} roomId: {:d}, roomName: {:s}", callbackName, roomId, roomName);
        break;
    }

    default: {
        auto resultText = SLNet::RoomsErrorCodeDescription::ToEnglish(resultCode);
        spdlog::debug("{:s} failed, error: {:s}", callbackName, resultText);
        break;
    }
    }
}

} // namespace hooks
