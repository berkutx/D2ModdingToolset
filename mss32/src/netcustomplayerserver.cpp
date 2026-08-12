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

#include "netcustomplayerserver.h"
#include "mempool.h"
#include "mqnetreception.h"
#include "mqnetsystem.h"
#include "netcustomplayer.h"
#include "netcustomsession.h"
#include "netmsg.h"
#include "utils.h"
#include <BitStream.h>
#include <MessageIdentifiers.h>
#include <algorithm>
#include <mutex>
#include <spdlog/spdlog.h>

namespace hooks {

CNetCustomPlayerServer::CNetCustomPlayerServer(CNetCustomSession* session,
                                               game::IMqNetSystem* system,
                                               game::IMqNetReception* reception)
    : CNetCustomPlayer(session,
                       system,
                       reception,
                       "SERVER",
                       game::serverNetPlayerId,
                       spdlog::default_logger_raw()->clone("plserver"))
    , m_peerCallback(this)
    , m_roomsCallback(this)
{
    getLogger()->debug(__FUNCTION__);

    static game::IMqNetPlayerServerVftable vftable = {
        (game::IMqNetPlayerServerVftable::Destructor)destructor,
        (game::IMqNetPlayerServerVftable::GetName)(GetName)getName,
        (game::IMqNetPlayerServerVftable::GetNetId)getNetId,
        (game::IMqNetPlayerServerVftable::GetSession)(GetSession)getSession,
        (game::IMqNetPlayerServerVftable::GetMessageCount)getMessageCount,
        (game::IMqNetPlayerServerVftable::SendNetMessage)sendMessage,
        (game::IMqNetPlayerServerVftable::ReceiveMessage)receiveMessage,
        (game::IMqNetPlayerServerVftable::SetNetSystem)setNetSystem,
        (game::IMqNetPlayerServerVftable::Method8)method8,
        (game::IMqNetPlayerServerVftable::DestroyPlayer)destroyPlayer,
        (game::IMqNetPlayerServerVftable::SetMaxPlayers)setMaxPlayers,
        (game::IMqNetPlayerServerVftable::SetAllowJoin)setAllowJoin,
    };

    this->vftable = &vftable;
    getService()->addPeerCallback(&m_peerCallback);
    getService()->addRoomsCallback(&m_roomsCallback);
}

CNetCustomPlayerServer::~CNetCustomPlayerServer()
{
    getLogger()->debug(__FUNCTION__);
    getService()->removePeerCallback(&m_peerCallback);
    getService()->removeRoomsCallback(&m_roomsCallback);
}

bool CNetCustomPlayerServer::addClient(const SLNet::RakNetGUID& guid, const SLNet::RakString& name)
{
    getLogger()->debug(__FUNCTION__ ": id = 0x{:x}, name = {:s}", getClientId(guid),
                       name.C_String());

    {
        std::lock_guard lock(m_remoteClientsMutex);
        auto result = m_remoteClients.insert({guid, name});
        if (!result.second) {
            getLogger()
                ->debug(__FUNCTION__ ": failed because the id 0x{:x} already exists, name = {:s}",
                        getClientId(guid), name.C_String());
            return false;
        }
    }

    // TODO: this should be called from the server's thread (from ReceiveMessage method)
    auto system = getSystem();
    system->vftable->onPlayerConnected(system, getClientId(guid));
    return true;
}

bool CNetCustomPlayerServer::removeClient(const SLNet::RakNetGUID& guid)
{
    {
        std::lock_guard lock(m_remoteClientsMutex);
        size_t result = m_remoteClients.erase(guid);
        if (!result) {
            getLogger()->debug(__FUNCTION__ ": failed because the id 0x{:x} does not exist",
                               getClientId(guid));
            return false;
        }
    }

    // TODO: this should be called from the server's thread (from ReceiveMessage method)
    auto system = getSystem();
    system->vftable->onPlayerDisconnected(system, getClientId(guid));
    return true;
}

bool CNetCustomPlayerServer::removeClient(const SLNet::RakString& name)
{
    SLNet::RakNetGUID guid = SLNet::UNASSIGNED_RAKNET_GUID;
    {
        std::lock_guard lock(m_remoteClientsMutex);
        for (auto it = m_remoteClients.begin(); it != m_remoteClients.end(); ++it) {
            if (it->second == name) {
                guid = it->first;
                m_remoteClients.erase(guid);
                break;
            }
        }
    }

    if (guid == SLNet::UNASSIGNED_RAKNET_GUID) {
        getLogger()->debug(__FUNCTION__ ": failed because there is no client with name '{:s}'",
                           name.C_String());
        return false;
    }

    // TODO: this should be called from the server's thread (from ReceiveMessage method)
    auto system = getSystem();
    system->vftable->onPlayerDisconnected(system, getClientId(guid));
    return true;
}

void __fastcall CNetCustomPlayerServer::destructor(CNetCustomPlayerServer* thisptr,
                                                   int /*%edx*/,
                                                   char flags)
{
    thisptr->~CNetCustomPlayerServer();

    if (flags & 1) {
        spdlog::debug(__FUNCTION__ ": freeing memory");
        game::Memory::get().freeNonZero(thisptr);
    }
}

bool __fastcall CNetCustomPlayerServer::sendMessage(CNetCustomPlayerServer* thisptr,
                                                    int /*%edx*/,
                                                    std::uint32_t idTo,
                                                    const game::NetMessageHeader* message)
{
    if (idTo == getClientId(thisptr->getService()->getPeerGuid())) {
        return thisptr->sendHostMessage(message);
    } else if (idTo == game::broadcastNetPlayerId) {
        if (!thisptr->sendHostMessage(message)) {
            return false;
        }

        auto clients = thisptr->getRemoteClients();
        if (clients.empty()) {
            return true;
        }
        return thisptr->sendRemoteMessage(message, clients);
    } else {
        return thisptr->sendRemoteMessage(message, thisptr->getRemoteClientGuid(idTo));
    }
}

bool __fastcall CNetCustomPlayerServer::destroyPlayer(CNetCustomPlayerServer* thisptr,
                                                      int /*%edx*/,
                                                      int playerId)
{
    thisptr->getLogger()->debug(__FUNCTION__);
    return false;
}

bool __fastcall CNetCustomPlayerServer::setMaxPlayers(CNetCustomPlayerServer* thisptr,
                                                      int /*%edx*/,
                                                      int maxPlayers)
{
    thisptr->getLogger()->debug(__FUNCTION__ ": max players = {:d}", maxPlayers);
    return thisptr->getSession()->setMaxPlayers(maxPlayers);
}

bool __fastcall CNetCustomPlayerServer::setAllowJoin(CNetCustomPlayerServer* thisptr,
                                                     int /*%edx*/,
                                                     bool allowJoin)
{
    // Ignore this since its only called during server creation and eventually being allowed
    thisptr->getLogger()->debug(__FUNCTION__ ": allow join = {:d}", (int)allowJoin);

    if (allowJoin) {
        // This means that the server finished initialization and ready to accept clients
        // TODO: this should be called from ReceiveMessage method
        auto system = thisptr->getSystem();
        system->vftable->onPlayerConnected(system,
                                           getClientId(thisptr->getService()->getPeerGuid()));
    }

    return true;
}

CNetCustomPlayerServer::RemoteClients CNetCustomPlayerServer::getRemoteClients() const
{
    std::lock_guard lock(m_remoteClientsMutex);
    return m_remoteClients;
}

SLNet::RakNetGUID CNetCustomPlayerServer::getRemoteClientGuid(std::uint32_t id) const
{
    std::lock_guard lock(m_remoteClientsMutex);
    auto it = std::find_if(m_remoteClients.begin(), m_remoteClients.end(),
                           [id](const auto& client) { return getClientId(client.first) == id; });
    if (it == m_remoteClients.end()) {
        getLogger()->debug(__FUNCTION__ ": there is no client with id 0x{:x}", id);
        return {};
    }
    return it->first;
}

void CNetCustomPlayerServer::PeerCallback::onPacketReceived(DefaultMessageIDTypes type,
                                                            SLNet::RakPeerInterface* peer,
                                                            const SLNet::Packet* packet)
{
    switch (type) {
    case ID_GAME_MESSAGE: {
        SLNet::RakNetGUID sender;
        std::size_t availableBytes{};
        auto message = getMessageAndSender(packet, &sender, &availableBytes);
        if (!message) {
            m_player->getLogger()->warn(__FUNCTION__ ": refusing malformed relayed game message");
            break;
        }
        m_player->postMessageToReceive(message, availableBytes, getClientId(sender));
        break;
    }

    case ID_GAME_MESSAGE_TO_HOST_SERVER: {
        const auto availableBytes{packet ? static_cast<std::size_t>(packet->length) : 0};
        auto message = packet ? reinterpret_cast<game::NetMessageHeader*>(packet->data) : nullptr;
        if (!isValidMessage(message, availableBytes, ID_GAME_MESSAGE_TO_HOST_SERVER)) {
            m_player->getLogger()->warn(__FUNCTION__ ": refusing malformed direct game message");
            break;
        }
        message->messageType = game::netMessageNormalType; // TODO: any better way to do this?
        m_player->postMessageToReceive(message, availableBytes, getClientId(packet->guid));
        break;
    }

    case ID_DISCONNECTION_NOTIFICATION: {
        m_player->getLogger()->debug(__FUNCTION__ ": server was shut down");
        m_player->removeClient(packet->guid);
        break;
    }

    case ID_CONNECTION_LOST: {
        m_player->getLogger()->debug(__FUNCTION__ ": connection with server is lost");
        m_player->removeClient(packet->guid);
        break;
    }
    }
}

void CNetCustomPlayerServer::RoomsCallback::RoomMemberLeftRoom_Callback(
    const SLNet::SystemAddress& /*senderAddress is the lobby*/,
    SLNet::RoomMemberLeftRoom_Notification* notification)
{
    // TODO: make sure that the notification only arrives for our room, otherwise check roomId
    m_player->getLogger()->debug(__FUNCTION__ ": member name = '{:s}'",
                                 notification->roomMember.C_String());
    m_player->removeClient(notification->roomMember);
}

void CNetCustomPlayerServer::RoomsCallback::RoomMemberJoinedRoom_Callback(
    const SLNet::SystemAddress& /*senderAddress is the lobby*/,
    SLNet::RoomMemberJoinedRoom_Notification* notification)
{
    // TODO: make sure that the notification only arrives for our room, otherwise check roomId
    const auto result = notification->joinedRoomResult;
    m_player->getLogger()->debug(__FUNCTION__ ": member name = '{:s}'",
                                 result->joiningMemberName.C_String());
    m_player->addClient(result->joiningMemberGuid, result->joiningMemberName);
}

} // namespace hooks
