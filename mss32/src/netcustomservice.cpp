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
#include "lobbyrestart.h"
#include "lobbysaveexchange.h"
#include "mempool.h"
#include "menurestartnative.h"
#include "midgard.h"
#include "midgardmsgbox.h"
#include "midmsgboxbuttonhandler.h"
#include "mqnetservice.h"
#include "mquikernel.h"
#include "netcustompeer.h"
#include "netcustomsession.h"
#include "netmsg.h"
#include "phasegame.h"
#include "settings.h"
#include "textids.h"
#include "uimanager.h"
#include "utils.h"
#include <MessageIdentifiers.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <string_view>
#include <utility>
#include <usersettings.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wincrypt.h>

namespace hooks {

namespace {

static constexpr std::uint32_t lobbyMaintenanceIntervalMs{250};
static constexpr UINT gameTextCodePage{1251};
static constexpr std::size_t clientHelloWireSize{
    sizeof(SLNet::MessageID) + sizeof(std::uint8_t) + sizeof(std::uint32_t)
    + LobbyProtocol::clientInstallIdSize + sizeof(std::uint16_t) * 2
    + sizeof(std::uint32_t) * 2};
static_assert(clientHelloWireSize == 34);
/** Prevents recursive packet, maintenance, and deferred-UI processing on the main thread. */
bool mainThreadCallbackActive{};

struct ClientEnvironment
{
    std::array<std::uint8_t, LobbyProtocol::clientInstallIdSize> installId{};
    std::uint16_t windowsMajor{};
    std::uint16_t windowsMinor{};
    std::uint32_t windowsBuild{};
};

class RegistryKey
{
public:
    ~RegistryKey()
    {
        if (key) {
            RegCloseKey(key);
        }
    }

    HKEY key{};
};

class WinHandle
{
public:
    ~WinHandle()
    {
        if (handle) {
            CloseHandle(handle);
        }
    }

    HANDLE handle{};
};

bool validInstallId(
    const std::array<std::uint8_t, LobbyProtocol::clientInstallIdSize>& installId)
{
    return std::any_of(installId.begin(), installId.end(),
                       [](std::uint8_t value) { return value != 0; });
}

bool readInstallId(
    HKEY key, std::array<std::uint8_t, LobbyProtocol::clientInstallIdSize>& installId)
{
    DWORD type{};
    DWORD size{static_cast<DWORD>(installId.size())};
    return RegQueryValueExW(key, L"InstallId", nullptr, &type, installId.data(), &size)
               == ERROR_SUCCESS
           && type == REG_BINARY && size == installId.size() && validInstallId(installId);
}

bool generateInstallId(
    std::array<std::uint8_t, LobbyProtocol::clientInstallIdSize>& installId)
{
    HCRYPTPROV provider{};
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        return false;
    }
    const bool generated{
        CryptGenRandom(provider, static_cast<DWORD>(installId.size()), installId.data()) != FALSE};
    CryptReleaseContext(provider, 0);
    return generated && validInstallId(installId);
}

std::optional<std::array<std::uint8_t, LobbyProtocol::clientInstallIdSize>> installId()
{
    // The named mutex keeps two local game clients from creating different first-run ids.
    WinHandle mutex{CreateMutexW(nullptr, FALSE, L"Local\\Conclave.InstallId")};
    if (!mutex.handle) {
        return std::nullopt;
    }
    const DWORD waitResult{WaitForSingleObject(mutex.handle, 250)};
    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        return std::nullopt;
    }

    RegistryKey registry;
    const LONG opened{RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Conclave", 0, nullptr,
                                      REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE,
                                      nullptr, &registry.key, nullptr)};
    if (opened != ERROR_SUCCESS) {
        ReleaseMutex(mutex.handle);
        return std::nullopt;
    }

    std::array<std::uint8_t, LobbyProtocol::clientInstallIdSize> value{};
    if (!readInstallId(registry.key, value)) {
        if (!generateInstallId(value)
            || RegSetValueExW(registry.key, L"InstallId", 0, REG_BINARY, value.data(),
                              static_cast<DWORD>(value.size())) != ERROR_SUCCESS) {
            ReleaseMutex(mutex.handle);
            return std::nullopt;
        }
    }

    ReleaseMutex(mutex.handle);
    return value;
}

std::optional<ClientEnvironment> clientEnvironment()
{
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll{GetModuleHandleW(L"ntdll.dll")};
    const auto rtlGetVersion{ntdll ? reinterpret_cast<RtlGetVersion>(
                                         GetProcAddress(ntdll, "RtlGetVersion"))
                                   : nullptr};
    if (!rtlGetVersion) {
        return std::nullopt;
    }

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0 || version.dwMajorVersion > 0xffffu
        || version.dwMinorVersion > 0xffffu) {
        return std::nullopt;
    }

    const auto id{installId()};
    if (!id) {
        return std::nullopt;
    }

    return ClientEnvironment{*id, static_cast<std::uint16_t>(version.dwMajorVersion),
                             static_cast<std::uint16_t>(version.dwMinorVersion),
                             static_cast<std::uint32_t>(version.dwBuildNumber)};
}

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
    if (text.empty()) {
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

bool isSafeSaveStem(std::string_view stem)
{
    if (stem.empty() || stem.size() > LobbyProtocol::saveStemMax) {
        return false;
    }

    return std::all_of(stem.begin(), stem.end(), [](unsigned char character) {
        return (character >= 'A' && character <= 'Z')
               || (character >= 'a' && character <= 'z')
               || (character >= '0' && character <= '9') || character == '_'
               || character == '-';
    });
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
    resetLobbyRestart();
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
    const auto effectiveSimTurnsDays{
        m_roomOptions.simultaneousTurnsEnabled ? m_roomOptions.simultaneousTurnsDays : 0};
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
    const bool ranked{game::CPhaseGameApi::nativeSaveSupported() && m_roomOptions.ranked};
    row->UpdateCell(rankedColumn, ranked ? "1" : "0");
    row->UpdateCell(simTurnsDaysColumn, simTurnsDays.c_str());
    row->UpdateCell(unlockGuiColumn, m_roomOptions.unlockGui ? "1" : "0");

    m_roomsClient.ExecuteFunc(&room);
    return true;
}

void CNetCustomService::leaveRoom()
{
    resetLobbyRestart();
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

    if (mainThreadCallbackActive) {
        // Native callbacks can pump messages and timers.  Local maintenance will drain deferred
        // UI state after the outer callback returns; no packet is resent here.
        return;
    }

    auto peer = service->m_peer;
    {
        MainThreadCallbackGuard callbackGuard{mainThreadCallbackActive};
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

std::shared_ptr<NativeGameMessageTracker> CNetCustomService::getNativeGameMessageTracker() const
{
    return m_nativeGameMessageTracker;
}

void CNetCustomService::processPeerMessages() const
{
    peerProcessEventCallback(this, 0, 0, 0);
}

bool CNetCustomService::readSaveRequest(const SLNet::Packet* packet,
                                        LobbyProtocol::SaveRequest& request) const
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

    std::uint8_t mode{};
    if (!stream.Read(request.saveId) || !stream.Read(mode)) {
        spdlog::warn(__FUNCTION__ ": malformed save request header");
        if (request.saveId) {
            sendLobbySaveFailure(request.saveId, SaveResult::Failed);
        }
        return false;
    }

    const auto stemBits{stream.GetNumberOfUnreadBits()};
    const auto stemBytes{static_cast<std::size_t>(stemBits / 8)};
    request.mode = static_cast<SaveMode>(mode);
    if (mode > static_cast<std::uint8_t>(SaveMode::LocalOnly) || request.saveId == 0
        || stemBits % 8 != 0 || stemBytes == 0 || stemBytes > saveStemMax) {
        spdlog::warn(__FUNCTION__ ": invalid save request metadata");
        sendLobbySaveFailure(request, SaveResult::Failed);
        return false;
    }

    request.saveStem.resize(stemBytes);
    if (!stream.ReadAlignedBytes(
            reinterpret_cast<unsigned char*>(request.saveStem.data()),
            static_cast<unsigned int>(stemBytes))
        || stream.GetNumberOfUnreadBits() != 0 || !isSafeSaveStem(request.saveStem)) {
        spdlog::warn(__FUNCTION__ ": invalid save request stem");
        sendLobbySaveFailure(request, SaveResult::Failed);
        return false;
    }

    return true;
}

bool CNetCustomService::readSaveStoredAck(const SLNet::Packet* packet,
                                          std::uint64_t& saveId) const
{
    using namespace LobbyProtocol;

    constexpr std::size_t packetSize{sizeof(SLNet::MessageID) + sizeof(std::uint64_t)};
    if (!packet || !packet->data || packet->length != packetSize
        || !isAuthenticatedLobbyPacket(this, packet)) {
        spdlog::warn(__FUNCTION__ ": refusing malformed or unauthenticated stored ACK");
        return false;
    }

    SLNet::BitStream stream{packet->data, packet->length, false};
    stream.IgnoreBytes(sizeof(SLNet::MessageID));
    if (!stream.Read(saveId) || saveId == 0 || stream.GetNumberOfUnreadBits() != 0) {
        spdlog::warn(__FUNCTION__ ": invalid stored ACK payload");
        return false;
    }
    return true;
}

bool CNetCustomService::readSystemNotice(const SLNet::Packet* packet, std::string& notice) const
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

    const auto textBits{stream.GetNumberOfUnreadBits()};
    const auto textBytes{static_cast<std::size_t>(textBits / 8)};
    if (textBits % 8 != 0 || textBytes == 0 || textBytes > systemNoticeTextMax) {
        spdlog::warn(__FUNCTION__ ": invalid system notice metadata");
        return false;
    }

    std::string utf8Text(textBytes, '\0');
    if (!stream.ReadAlignedBytes(reinterpret_cast<unsigned char*>(utf8Text.data()),
                                 static_cast<unsigned int>(textBytes))
        || utf8Text.find('\0') != std::string::npos
        || stream.GetNumberOfUnreadBits() != 0) {
        spdlog::warn(__FUNCTION__ ": invalid system notice text");
        return false;
    }

    const auto encoded{utf8ToGameEncoding(utf8Text)};
    if (!encoded) {
        spdlog::warn(__FUNCTION__
                     ": notice is invalid UTF-8 or cannot be represented in Windows-1251");
        return false;
    }
    notice = *encoded;
    return true;
}

void CNetCustomService::enqueueSystemNotice(std::string notice)
{
    m_pendingSystemNotices.push_back(std::move(notice));
}

void CNetCustomService::processDeferredLobbyState()
{
    if (mainThreadCallbackActive || m_systemNoticeModalActive) {
        return;
    }

    MainThreadCallbackGuard callbackGuard{mainThreadCallbackActive};
    if (processLobbyRestart()) {
        return;
    }
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

    auto midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data) {
        return;
    }

    auto data = midgard->data;
    if (!loggedIn() || !getSession() || !data->multiplayerGame || !data->client) {
        // A detached terminal packet can arrive after the old native session has already gone.
        // It must not block later global notices while waiting for state that will never return.
        m_matchEndPending = false;
        return;
    }
    if (!m_nativeGameMessageTracker->empty()) {
        // Client and host-server receptions use different windows; wait until both queues drain.
        return;
    }

    auto uiManager = data->uiManager.data;
    if (!uiManager || data->startMenuMessageId == 0) {
        return;
    }

    // Native player reception also uses posted window messages. The tracker keeps this transition
    // pending until both client and server queues are dequeued; posting MID_STARTMENU then lets an
    // already-running client notification return before native network teardown starts.
    if (!game::CUIManagerApi::get().postMessage(uiManager, data->startMenuMessageId, 0, 0)) {
        spdlog::warn(__FUNCTION__ ": failed to post MID_STARTMENU, error = {:d}", GetLastError());
        return;
    }

    m_matchEndPending = false;
    terminateLobbySaveTransfers();
}

void CNetCustomService::processPendingSystemNotices()
{
    if (m_systemNoticeModalActive || m_pendingSystemNotices.empty()) {
        return;
    }

    auto notice{std::move(m_pendingSystemNotices.front())};
    m_pendingSystemNotices.pop_front();
    m_systemNoticeModalActive = true;

    auto handler = static_cast<SystemNoticeMsgBoxButtonHandler*>(
        game::Memory::get().allocate(sizeof(SystemNoticeMsgBoxButtonHandler)));
    handler->vftable = &systemNoticeMsgBoxButtonHandlerVftable;
    showMessageBox(notice, handler, false);
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

void __fastcall CNetCustomService::lobbyMaintenanceTimerEventCallback(
    CNetCustomService* /*thisptr*/,
    int /*%edx*/)
{
    auto service = get();
    if (!service || mainThreadCallbackActive) {
        // A timer notification may already be queued when native service teardown removes the
        // event. Resolve the current instance instead of trusting the callback's raw userdata.
        return;
    }

    {
        MainThreadCallbackGuard callbackGuard{mainThreadCallbackActive};
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
        LobbyProtocol::SaveRequest request{};
        if (m_service->readSaveRequest(packet, request)) {
            if (isLobbyRestartActive()) {
                sendLobbySaveFailure(request, LobbyProtocol::SaveResult::Failed);
            } else {
                handleLobbySaveRequest(request);
            }
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
        std::string notice;
        if (m_service->readSystemNotice(packet, notice)) {
            m_service->enqueueSystemNotice(std::move(notice));
        }
        break;
    }
    case ID_LOBBY_RESTART: {
        if (!packet || !packet->data || packet->length != 10) {
            break;
        }
        SLNet::BitStream stream(packet->data, packet->length, false);
        stream.IgnoreBytes(1);
        std::uint8_t operation{};
        std::uint64_t token{};
        if (stream.Read(operation) && stream.Read(token) && token != 0) {
            if (packet->guid == m_service->getPeerGuid()
                && operation == static_cast<std::uint8_t>(LobbyProtocol::RestartOperation::HostReady)) {
                handleLobbyRestartLoopbackFence(token);
            } else if (isAuthenticatedLobbyPacket(m_service, packet)) {
                handleLobbyRestart(static_cast<LobbyProtocol::RestartOperation>(operation), token);
            }
        }
        break;
    }
    case ID_LOBBY_MATCH_ENDED:
        if (isLobbyRestartActive()) {
            break;
        }
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

        // Pre-ranked lobby servers ignore this authenticated extension. Current servers use its
        // fixed schema both for the ranked capability gate and optional anti-abuse signals.
        const auto environment{clientEnvironment().value_or(ClientEnvironment{})};
        SLNet::BitStream stream;
        stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_PLAYER_SETUP));
        stream.Write(static_cast<std::uint8_t>(
            LobbyProtocol::PlayerSetupKind::ClientCapabilities));
        stream.Write(LobbyProtocol::rankedLifecycleCapabilityVersion);
        stream.WriteAlignedBytes(environment.installId.data(),
                                 static_cast<unsigned int>(environment.installId.size()));
        stream.Write(environment.windowsMajor);
        stream.Write(environment.windowsMinor);
        stream.Write(environment.windowsBuild);
        stream.Write(restartNativeSupported() ? LobbyProtocol::coordinatedRestartFeature : 0u);
        const auto lobbyGuid{m_service->getLobbyGuid()};
        if (!m_service->send(stream, lobbyGuid, LOW_PRIORITY)) {
            spdlog::warn(__FUNCTION__ ": failed to advertise ranked-lifecycle capability");
        }
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
