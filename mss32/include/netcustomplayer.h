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

#ifndef NETCUSTOMPLAYER_H
#define NETCUSTOMPLAYER_H

#include "mqnetplayer.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace game {
struct IMqNetSystem;
struct IMqNetReception;
} // namespace game

namespace SLNet {
struct RakNetGUID;
struct Packet;
class RakString;
}; // namespace SLNet

namespace spdlog {
class logger;
};

namespace hooks {

class CNetCustomService;
class CNetCustomSession;

class CNetCustomPlayer : public game::IMqNetPlayer
{
public:
    CNetCustomPlayer(CNetCustomSession* session,
                     game::IMqNetSystem* system,
                     game::IMqNetReception* reception,
                     const char* name,
                     std::uint32_t id,
                     std::shared_ptr<spdlog::logger> logger);
    ~CNetCustomPlayer();

protected:
    using RemoteClients = std::map<SLNet::RakNetGUID, SLNet::RakString>;

    static uint32_t getClientId(const SLNet::RakNetGUID& guid);
    static bool isValidMessage(const game::NetMessageHeader* message,
                               std::size_t availableBytes,
                               std::uint32_t expectedMessageType);
    static const game::NetMessageHeader* getMessageAndSender(const SLNet::Packet* packet,
                                                             SLNet::RakNetGUID* sender,
                                                             std::size_t* availableBytes);

    CNetCustomService* getService() const;
    CNetCustomSession* getSession() const;
    game::IMqNetSystem* getSystem() const;
    game::IMqNetReception* getReception() const;
    const std::string& getName() const;
    void setName(const char* value);
    uint32_t getId() const;
    void postMessageToReceive(const game::NetMessageHeader* message,
                              std::size_t availableBytes,
                              std::uint32_t idFrom);
    bool sendRemoteMessage(const game::NetMessageHeader* message,
                           const SLNet::RakNetGUID& to) const;
    bool sendRemoteMessage(const game::NetMessageHeader* message, const RemoteClients& to) const;
    bool sendHostMessage(const game::NetMessageHeader* message) const;
    const std::shared_ptr<spdlog::logger>& getLogger() const;

    // IMqNetPlayer
    using GetName = game::String*(__fastcall*)(CNetCustomPlayer* thisptr,
                                               int /*%edx*/,
                                               game::String* string);
    using GetSession = game::IMqNetSession*(__fastcall*)(CNetCustomPlayer* thisptr, int /*%edx*/);
    using SendNetMessage = bool(__fastcall*)(CNetCustomPlayer* thisptr,
                                             int /*%edx*/,
                                             std::uint32_t idTo,
                                             const game::NetMessageHeader* message);
    static void __fastcall destructor(CNetCustomPlayer* thisptr, int /*%edx*/, char flags);
    static game::String* __fastcall getName(CNetCustomPlayer* thisptr,
                                            int /*%edx*/,
                                            game::String* string);
    static int __fastcall getNetId(CNetCustomPlayer* thisptr, int /*%edx*/);
    static game::IMqNetSession* __fastcall getSession(CNetCustomPlayer* thisptr, int /*%edx*/);
    static int __fastcall getMessageCount(CNetCustomPlayer* thisptr, int /*%edx*/);
    static game::ReceiveMessageResult __fastcall receiveMessage(CNetCustomPlayer* thisptr,
                                                                int /*%edx*/,
                                                                std::uint32_t* idFrom,
                                                                game::NetMessageHeader* buffer);
    static void __fastcall setNetSystem(CNetCustomPlayer* thisptr,
                                        int /*%edx*/,
                                        game::IMqNetSystem* netSystem);
    static int __fastcall method8(CNetCustomPlayer* thisptr, int /*%edx*/, int a2);

private:
    using NetMessagePtr = std::unique_ptr<unsigned char[]>;
    using IdMessagePair = std::pair<std::uint32_t, NetMessagePtr>;

    CNetCustomSession* m_session;
    game::IMqNetSystem* m_system;
    game::IMqNetReception* m_reception;
    std::string m_name;
    std::uint32_t m_id;
    std::queue<IdMessagePair> m_messages;
    mutable std::mutex m_messagesMutex;
    std::shared_ptr<spdlog::logger> m_logger;
};

assert_offset(CNetCustomPlayer, vftable, 0);

} // namespace hooks

#endif // NETCUSTOMPLAYER_H
