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

#include "netcustomplayerclient.h"
#include "lobbyrestart.h"
#include "mempool.h"
#include "mqnetreception.h"
#include "mqnetsystem.h"
#include "netcustomplayer.h"
#include "netcustomservice.h"
#include "netcustomsession.h"
#include "netmsg.h"
#include <BitStream.h>
#include <cstring>
#include <spdlog/spdlog.h>

namespace hooks {

CNetCustomPlayerClient::CNetCustomPlayerClient(CNetCustomSession* session,
                                               game::IMqNetSystem* system,
                                               game::IMqNetReception* reception,
                                               const char* name,
                                               const SLNet::RakNetGUID& serverGuid)
    : CNetCustomPlayer{session,
                       system,
                       reception,
                       name,
                       getClientId(session->getService()->getPeerGuid()),
                       spdlog::default_logger_raw()->clone("plclient")}
    , m_peerCallback(this)
    , m_roomsCallback(this)
    , m_serverGuid{serverGuid}
{
    getLogger()->debug(__FUNCTION__);

    static game::IMqNetPlayerClientVftable vftable = {
        (game::IMqNetPlayerClientVftable::Destructor)destructor,
        (game::IMqNetPlayerClientVftable::GetName)(GetName)getName,
        (game::IMqNetPlayerClientVftable::GetNetId)getNetId,
        (game::IMqNetPlayerClientVftable::GetSession)(GetSession)getSession,
        (game::IMqNetPlayerClientVftable::GetMessageCount)getMessageCount,
        (game::IMqNetPlayerClientVftable::SendNetMessage)sendMessage,
        (game::IMqNetPlayerClientVftable::ReceiveMessage)receiveMessage,
        (game::IMqNetPlayerClientVftable::SetNetSystem)setNetSystem,
        (game::IMqNetPlayerClientVftable::Method8)method8,
        (game::IMqNetPlayerClientVftable::SetName)(SetName)setName,
        (game::IMqNetPlayerClientVftable::IsHost)isHost,
    };

    this->vftable = &vftable;
    auto service = getService();
    service->addPeerCallback(&m_peerCallback);
    service->addRoomsCallback(&m_roomsCallback);
}

CNetCustomPlayerClient ::~CNetCustomPlayerClient()
{
    getLogger()->debug(__FUNCTION__);
    auto service = getService();
    service->removeRoomsCallback(&m_roomsCallback);
    service->removePeerCallback(&m_peerCallback);
}

void __fastcall CNetCustomPlayerClient::destructor(CNetCustomPlayerClient* thisptr,
                                                   int /*%edx*/,
                                                   char flags)
{
    thisptr->~CNetCustomPlayerClient();

    if (flags & 1) {
        spdlog::debug(__FUNCTION__ ": freeing memory");
        game::Memory::get().freeNonZero(thisptr);
    }
}

bool __fastcall CNetCustomPlayerClient::sendMessage(CNetCustomPlayerClient* thisptr,
                                                    int /*%edx*/,
                                                    std::uint32_t idTo,
                                                    const game::NetMessageHeader* message)
{
    if (idTo != game::serverNetPlayerId) {
        thisptr->getLogger()->debug(
            __FUNCTION__ ": denying sending message to a player other than the server");
        return false;
    }

    std::vector<unsigned char> replacement;
    prepareLobbyRestartSetupMessage(message, replacement);
    if (!replacement.empty()) {
        message = reinterpret_cast<const game::NetMessageHeader*>(replacement.data());
    }

    if (thisptr->getSession()->isHost()) {
        const bool sentToGame{thisptr->sendHostMessage(message)};
        if (sentToGame) {
            thisptr->forwardPlayerSetupToLobby(message);
        }
        return sentToGame;
    } else {
        return thisptr->sendRemoteMessage(message, thisptr->m_serverGuid);
    }
}

void CNetCustomPlayerClient::forwardPlayerSetupToLobby(const game::NetMessageHeader* message) const
{
    // The stock player-list message already exposes the host race to the relay, but the host's
    // lord request stays in the local loopback. Its first field is the lord category; the second
    // one is only the selected portrait.
    static constexpr char lordMessageClass[]{".?AVCMenusReqLordMsg@@"};
    static constexpr auto lordCategoryOffset{sizeof(game::NetMessageHeader)};
    if (!message || message->length < lordCategoryOffset + sizeof(std::int32_t)
        || std::memcmp(message->messageClassName, lordMessageClass,
                       sizeof(lordMessageClass)) != 0) {
        return;
    }
    std::int32_t lordCategory{};
    std::memcpy(&lordCategory,
                reinterpret_cast<const char*>(message) + lordCategoryOffset,
                sizeof(lordCategory));

    auto service = getService();
    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_PLAYER_SETUP));
    stream.Write(static_cast<std::uint8_t>(LobbyProtocol::PlayerSetupKind::HostLord));
    stream.Write(lordCategory);
    if (!service->send(stream, service->getLobbyGuid(), LOW_PRIORITY)) {
        getLogger()->warn(__FUNCTION__ ": failed to forward accepted host lord to lobby");
    }
}

bool __fastcall CNetCustomPlayerClient::setName(CNetCustomPlayerClient* thisptr,
                                                int /*%edx*/,
                                                const char* name)
{
    thisptr->getLogger()->debug(__FUNCTION__ ": name = '{:s}'", name);
    thisptr->setName(name);
    return true;
}

bool __fastcall CNetCustomPlayerClient::isHost(CNetCustomPlayerClient* thisptr, int /*%edx*/)
{
    bool result = thisptr->getSession()->isHost();
    thisptr->getLogger()->debug(__FUNCTION__ ": is host = {:d}", (int)result);
    return result;
}

void CNetCustomPlayerClient::PeerCallback::onPacketReceived(DefaultMessageIDTypes type,
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
        if (sender != m_player->m_serverGuid) {
            // Should only be a message to the server if we are hosting
            // (since both server and client players share the same peer)
            m_player->getLogger()
                ->debug(__FUNCTION__ ": skipping '{:s}' from 0x{:x} (not a server)",
                        message->messageClassName, getClientId(sender));
            break;
        }
        observeLobbyRestartSetupInfo(message);
        m_player->postMessageToReceive(message, availableBytes, game::serverNetPlayerId);
        break;
    }

    case ID_GAME_MESSAGE_TO_HOST_CLIENT: {
        const auto availableBytes{packet ? static_cast<std::size_t>(packet->length) : 0};
        auto message = packet ? reinterpret_cast<game::NetMessageHeader*>(packet->data) : nullptr;
        if (!isValidMessage(message, availableBytes, ID_GAME_MESSAGE_TO_HOST_CLIENT)) {
            m_player->getLogger()->warn(__FUNCTION__ ": refusing malformed direct game message");
            break;
        }
        message->messageType = game::netMessageNormalType; // TODO: any better way to do this?
        observeLobbyRestartSetupInfo(message);
        m_player->postMessageToReceive(message, availableBytes, game::serverNetPlayerId);
        break;
    }

    case ID_DISCONNECTION_NOTIFICATION: {
        m_player->getLogger()->debug(__FUNCTION__ ": server was shut down");
        auto system = m_player->getSystem();
        if (system) {
            system->vftable->onPlayerDisconnected(system, game::serverNetPlayerId);
        }
        break;
    }

    case ID_CONNECTION_LOST: {
        m_player->getLogger()->debug(__FUNCTION__ ": connection with server is lost");
        auto system = m_player->getSystem();
        if (system) {
            system->vftable->onPlayerDisconnected(system, game::serverNetPlayerId);
        }
        break;
    }
    }
}

void CNetCustomPlayerClient::RoomsCallback::RoomDestroyedOnModeratorLeft_Callback(
    const SLNet::SystemAddress& senderAddress,
    SLNet::RoomDestroyedOnModeratorLeft_Notification* notification)
{
    // TODO: make sure that the notification only arrives for our room, otherwise check roomId
    m_player->getLogger()->debug(__FUNCTION__);
    auto system = m_player->getSystem();
    if (system) {
        system->vftable->onPlayerDisconnected(system, game::serverNetPlayerId);
    }
}

} // namespace hooks
