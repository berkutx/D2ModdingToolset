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

#ifndef NETCUSTOMSERVICE_H
#define NETCUSTOMSERVICE_H

#include "mqnetservice.h"
#include "netmsg.h"
#include "uievent.h"
#include <Lobby2Client.h>
#include <Lobby2Message.h>
#include <MessageIdentifiers.h>
#include <PacketPriority.h>
#include <RakPeerInterface.h>
#include <RoomsPlugin.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace game {
struct NetMessageHeader;
} // namespace game

namespace hooks {

/** Shared lifetime-safe count of native game messages queued by custom players. */
class NativeGameMessageTracker
{
public:
    void queued() noexcept
    {
        m_pending.fetch_add(1, std::memory_order_relaxed);
    }

    void consumed(std::size_t count = 1) noexcept
    {
        auto pending = m_pending.load(std::memory_order_relaxed);
        while (pending != 0) {
            const auto remaining = count < pending ? pending - count : 0;
            if (m_pending.compare_exchange_weak(pending, remaining, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    bool empty() const noexcept
    {
        return m_pending.load(std::memory_order_relaxed) == 0;
    }

private:
    std::atomic_size_t m_pending{};
};

// Keep the numeric values in sync with the lobby server.  Values +1 through +7 are the
// established custom-lobby protocol; ranked-match messages are append-only so older clients keep
// interpreting every legacy byte exactly as before.  Every value must fit in SLNet::MessageID and
// remain below the stock game-message relay byte (255).
enum LobbyMessageId
{
    ID_LOBBY_CHAT_MESSAGE = ID_USER_PACKET_ENUM + 1,
    ID_LOBBY_GET_ONLINE_USERS_REQUEST = ID_USER_PACKET_ENUM + 2,
    ID_LOBBY_GET_ONLINE_USERS_RESPONSE = ID_USER_PACKET_ENUM + 3,
    ID_LOBBY_GET_CHAT_MESSAGES_REQUEST = ID_USER_PACKET_ENUM + 4,
    ID_LOBBY_GET_CHAT_MESSAGES_RESPONSE = ID_USER_PACKET_ENUM + 5,
    ID_GAME_MESSAGE_TO_HOST_SERVER = ID_USER_PACKET_ENUM + 6,
    ID_GAME_MESSAGE_TO_HOST_CLIENT = ID_USER_PACKET_ENUM + 7,
    /** Core -> host: start one bounded native host save. */
    ID_LOBBY_SAVE_REQUEST = ID_USER_PACKET_ENUM + 8,
    /** Host -> core: one native-save BEGIN, CHUNK, COMMIT, or FAIL operation. */
    ID_LOBBY_SAVE_UPLOAD = ID_USER_PACKET_ENUM + 9,
    /** Core -> participant: the ranked match is finalized; leave the game UI for the lobby. */
    ID_LOBBY_MATCH_ENDED = ID_USER_PACKET_ENUM + 10,
    /** Participant -> core: protocol capability or authenticated host setup extension. */
    ID_LOBBY_PLAYER_SETUP = ID_USER_PACKET_ENUM + 11,
    /** Core -> participant: modal notice. System chat uses legacy chat packets. */
    ID_LOBBY_SYSTEM_NOTICE = ID_USER_PACKET_ENUM + 12,
    /** Core -> host: durable-storage confirmation, not a RakNet delivery ACK. */
    ID_LOBBY_SAVE_STORED_ACK = ID_USER_PACKET_ENUM + 13,
    /** Host -> core: native-save result and actual collision-safe basename. */
    ID_LOBBY_SAVE_NATIVE_RESULT = ID_USER_PACKET_ENUM + 14,
    /** Coordinated same-room random-map restart; negotiated by ClientCapabilities. */
    ID_LOBBY_RESTART = ID_USER_PACKET_ENUM + 15,
    ID_GAME_MESSAGE = game::netMessageNormalType & 0xff,
};

static_assert(ID_GAME_MESSAGE == 255);
static_assert(ID_LOBBY_RESTART < ID_GAME_MESSAGE);

/** Lobby-specific wire protocol. Values are serialized field-by-field with SLNet::BitStream;
 * these structures are logical payloads, not packed wire images. Keep in sync with the lobby
 * server. */
namespace LobbyProtocol {

static constexpr std::size_t systemNoticeTextMax{1024};
static constexpr std::uint32_t saveFileHardLimit{32u * 1024u * 1024u};
static constexpr std::uint32_t saveChunkSizeMax{16u * 1024u};
static constexpr std::size_t saveStemMax{180};
static constexpr std::size_t saveResultFileNameMax{220};

enum class SaveMode : std::uint8_t
{
    Upload = 0,
    LocalOnly = 1,
};

/** Validated logical request from a capability-negotiated lobby server. */
struct SaveRequest
{
    std::uint64_t saveId{};
    SaveMode mode{SaveMode::Upload};
    std::string saveStem;
};

enum class SaveDataOperation : std::uint8_t
{
    Begin = 0,
    Chunk = 1,
    Commit = 2,
    Fail = 3,
};

/** Result shared by SAVE_UPLOAD/FAIL and SAVE_NATIVE_RESULT. */
enum class SaveResult : std::uint8_t
{
    Success = 0,
    Failed = 1,
    TimedOut = 2,
};

enum class PlayerSetupKind : std::uint8_t
{
    ClientCapabilities = 0,
    HostLord = 1,
};

/** Exact capability schema announced after a successful Lobby2 login. */
static constexpr std::uint32_t rankedLifecycleCapabilityVersion{1};
static constexpr std::size_t clientInstallIdSize{16};

} // namespace LobbyProtocol

class CNetCustomPeer;
class CNetCustomSession;

class NetPeerCallback
{
public:
    virtual ~NetPeerCallback() = default;
    virtual void onPacketReceived(DefaultMessageIDTypes type,
                                  SLNet::RakPeerInterface* peer,
                                  const SLNet::Packet* packet) = 0;
};

// Used in CNetCustomService::joinSession instead of IMqNetSessEnum
struct CNetCustomSessEnum
{
    SLNet::RakNetGUID serverGuid;
    std::string sessionName;
};

class CNetCustomService : public game::IMqNetService
{
public:
    struct UserInfo
    {
        SLNet::RakNetGUID guid;
        SLNet::RakString name;
    };

    struct ChatMessage
    {
        SLNet::RakString sender;
        SLNet::RakString text;
    };

    /** Room options set in the custom lobby menu, sent as room property columns on creation. */
    struct RoomOptions
    {
        bool ranked{false};
        /** Whether the simultaneous-turns room option is enabled. */
        bool simultaneousTurnsEnabled{false};
        /** Spinner selection retained while simultaneous turns are disabled. */
        int simultaneousTurnsDays{7};
        bool unlockGui{false};
    };

    // !!! Keep in sync with lobby server
    static constexpr std::uint32_t peerConnectionTimeout{30000};
    static constexpr std::uint32_t peerShutdownTimeout{100};
    // static constexpr std::uint32_t peerProcessInterval{10};
    static constexpr char peerProcessMessageName[] = "MIDGARD CUSTOM LOBBY NETMSG";
    static constexpr char titleName[] = "Disciples II: Rise of the Elves";
    static constexpr char titleSecretKey[] = "TheVerySecretKey";
    static constexpr char gameFilesHashColumnName[] = "FilesHash";
    static constexpr char gameVersionColumnName[] = "GameVersion";
    static constexpr char gameNameColumnName[] = "GameName";
    static constexpr char passwordColumnName[] = "Password";
    static constexpr char scenarioNameColumnName[] = "ScenarioName";
    static constexpr char scenarioDescriptionColumnName[] = "ScenarioDescription";
    static constexpr char templateNameColumnName[] = "TemplateName";
    static constexpr char templateHashColumnName[] = "TemplateHash";
    static constexpr char rankedColumnName[] = "Ranked";
    static constexpr char simultaneousTurnsDaysColumnName[] = "SimultaneousTurnsDays";
    static constexpr char unlockGuiColumnName[] = "UnlockGui";
    // See SLNet::Lobby2Message::ValidatePassword
    static constexpr std::uint32_t passwordMaxLength{50};

    /** Returns the service instance if it is set in CMidgard, otherwise returns nullptr. */
    static CNetCustomService* get();

    CNetCustomService();
    ~CNetCustomService();

    bool connect();
    CNetCustomSession* getSession() const;
    const std::string& getUserName() const;
    bool connected() const;
    bool loggedIn() const;
    const SLNet::RakNetGUID getPeerGuid() const;
    const SLNet::RakNetGUID getLobbyGuid() const;
    bool send(const SLNet::BitStream& stream,
              const SLNet::RakNetGUID& to,
              PacketPriority priority) const;
    const std::string& getGameFilesHash();
    std::vector<std::filesystem::path> getGameFilesToHash() const;
    void setTemplateInfo(const std::string& name);

    const std::string& getTemplateName() const;
    const std::string& getTemplateHash();

    std::string computeTemplateHash(const std::string& templateName) const;
    UserInfo getUserInfo() const;
    void processPeerMessages() const;

    /** Shows the next queued global notice after the active native dialog is dismissed. */
    void notifySystemNoticeModalClosed();

    /** Invalidates the service's non-owning session pointer during native session teardown. */
    void notifySessionDestroyed(CNetCustomSession* session);

    std::shared_ptr<NativeGameMessageTracker> getNativeGameMessageTracker() const;

    RoomOptions& getRoomOptions();

    bool registerAccount(const char* userName, const char* password);
    bool login(const char* userName, const char* password);
    void logoff();

    void sendChatMessage(const char* text);
    ChatMessage readChatMessage(const SLNet::Packet* packet);

    /** Requests online user list. Handle ID_LOBBY_GET_ONLINE_USERS_RESPONSE in peer callback. */
    void queryOnlineUsers();
    std::vector<UserInfo> readOnlineUsers(const SLNet::Packet* packet);

    /** Requests saved chat messages. Handle ID_LOBBY_GET_CHAT_MESSAGES_RESPONSE in peer callback.
     */
    void queryChatMessages();
    std::vector<ChatMessage> readChatMessages(const SLNet::Packet* packet);

    /** Tries to create and enter a new room. */
    bool createRoom(const char* gameName,
                    const char* scenarioName,
                    const char* scenarioDescription,
                    const char* password = nullptr);

    /** Requests to leave the previously entered room. */
    void leaveRoom();

    /** Requests a list of rooms. */
    void searchRooms();

    /** Requests joining a room. */
    void joinRoom(SLNet::RoomID id);

    /** Tries to change number of public slots in current room. */
    bool changeRoomPublicSlots(unsigned int publicSlots);

    /**
     * The service is always first to receive peer notifications.
     * So other listeners will be dealing with already updated service state.
     */
    void addPeerCallback(NetPeerCallback* callback);
    void removePeerCallback(NetPeerCallback* callback);

    /**
     * The service is always first to receive lobby notifications.
     * So other listeners will be dealing with already updated service state.
     */
    void addLobbyCallback(SLNet::Lobby2Callbacks* callback);
    void removeLobbyCallback(SLNet::Lobby2Callbacks* callback);

    /**
     * The service is always first to receive room notifications.
     * So other listeners will be dealing with already updated service state.
     */
    void addRoomsCallback(SLNet::RoomsCallback* callback);
    void removeRoomsCallback(SLNet::RoomsCallback* callback);

protected:
    bool startPeer();

    // IMqNetService
    static void __fastcall destructor(CNetCustomService* thisptr, int /*%edx*/, char flags);
    static bool __fastcall hasSessions(CNetCustomService* thisptr, int /*%edx*/);
    static void __fastcall getSessions(CNetCustomService* thisptr,
                                       int /*%edx*/,
                                       game::List<game::IMqNetSessEnum*>* sessions,
                                       const GUID* appGuid,
                                       const char* ipAddress,
                                       bool allSessions,
                                       bool requirePassword);
    static void __fastcall createSession(CNetCustomService* thisptr,
                                         int /*%edx*/,
                                         game::IMqNetSession** netSession,
                                         const GUID* /* appGuid */,
                                         const char* sessionName,
                                         const char* password);
    static void __fastcall joinSession(CNetCustomService* thisptr,
                                       int /*%edx*/,
                                       game::IMqNetSession** netSession,
                                       CNetCustomSessEnum* netSessionEnum,
                                       const char* password);

private:
    static game::IMqNetServiceVftable g_vftable;

    class PeerCallback : public NetPeerCallback
    {
    public:
        PeerCallback(CNetCustomService* service)
            : m_service(service)
        { }

        void onPacketReceived(DefaultMessageIDTypes type,
                              SLNet::RakPeerInterface* peer,
                              const SLNet::Packet* packet);

    private:
        CNetCustomService* m_service;
    };

    class LobbyCallback : public SLNet::Lobby2Callbacks
    {
    public:
        LobbyCallback(CNetCustomService* service)
            : m_service(service)
        { }

        ~LobbyCallback() override = default;

        void MessageResult(SLNet::Client_Login* message) override;
        void MessageResult(SLNet::Client_Logoff* message) override;
        void MessageResult(SLNet::Notification_Client_RemoteLogin* message) override;
        void ExecuteDefaultResult(SLNet::Lobby2Message* msg) override;

    private:
        CNetCustomService* m_service;
    };

    class RoomsCallback : public SLNet::RoomsCallback
    {
    public:
        RoomsCallback() = default;
        ~RoomsCallback() override = default;

        void CreateRoom_Callback(const SLNet::SystemAddress& senderAddress,
                                 SLNet::CreateRoom_Func* callResult) override;

        void EnterRoom_Callback(const SLNet::SystemAddress& senderAddress,
                                SLNet::EnterRoom_Func* callResult) override;

        void LeaveRoom_Callback(const SLNet::SystemAddress& senderAddress,
                                SLNet::LeaveRoom_Func* callResult) override;

        void RoomMemberLeftRoom_Callback(
            const SLNet::SystemAddress& senderAddress,
            SLNet::RoomMemberLeftRoom_Notification* notification) override;

        void RoomMemberJoinedRoom_Callback(
            const SLNet::SystemAddress& senderAddress,
            SLNet::RoomMemberJoinedRoom_Notification* notification) override;

    protected:
        void ExecuteDefaultResult(const char* callbackName,
                                  SLNet::RoomsErrorCode resultCode,
                                  SLNet::RoomID roomId,
                                  SLNet::RoomDescriptor* roomDescriptor = nullptr) const;
    };

    static void __fastcall peerProcessEventCallback(const CNetCustomService* thisptr,
                                                     int /*%edx*/,
                                                     unsigned int,
                                                     long);
    /** Expires local save deadlines and drains deferred UI state; it never resends network data. */
    static void __fastcall lobbyMaintenanceTimerEventCallback(CNetCustomService* thisptr,
                                                               int /*%edx*/);
    std::vector<NetPeerCallback*> getPeerCallbacks() const;

    bool readSaveRequest(const SLNet::Packet* packet,
                         LobbyProtocol::SaveRequest& request) const;
    bool readSaveStoredAck(const SLNet::Packet* packet, std::uint64_t& saveId) const;
    bool readSystemNotice(const SLNet::Packet* packet, std::string& notice) const;
    void enqueueSystemNotice(std::string notice);
    void processDeferredLobbyState();
    void processPendingMatchEnd();
    void processPendingSystemNotices();
    /** Drops the current match transfer/terminal state while retaining global system notices. */
    void clearLobbyMatchState();

    bool m_connected;
    PeerCallback m_peerCallback;
    CNetCustomSession* m_session;
    std::string m_userName;
    /** Interacts with lobby server. */
    SLNet::Lobby2Client m_lobbyClient;
    /** Creates network messages. */
    SLNet::Lobby2MessageFactory m_lobbyMsgFactory;
    LobbyCallback m_lobbyCallback;
    /** Interacts with lobby server rooms. */
    SLNet::RoomsPlugin m_roomsClient;
    RoomsCallback m_roomsCallback;
    /** Connection with lobby server. */
    CNetCustomPeer* m_peer;
    game::UiEvent m_peerProcessEvent;
    game::UiEvent m_lobbyMaintenanceTimerEvent;
    std::vector<NetPeerCallback*> m_peerCallbacks;
    mutable std::mutex m_peerCallbacksMutex;
    std::deque<std::string> m_pendingSystemNotices;
    bool m_matchEndPending{};
    bool m_systemNoticeModalActive{};
    std::shared_ptr<NativeGameMessageTracker> m_nativeGameMessageTracker{
        std::make_shared<NativeGameMessageTracker>()};
    std::string m_gameFilesHash;
    std::string m_templateName;
    std::string m_templateHash;
    RoomOptions m_roomOptions;
};

assert_offset(CNetCustomService, vftable, 0);

} // namespace hooks

#endif // NETCUSTOMSERVICE_H
