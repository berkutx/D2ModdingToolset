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

#include "netcustomplayer.h"
#include "d2string.h"
#include "lobbyrestart.h"
#include "mempool.h"
#include "mqnetplayer.h"
#include "mqnetreception.h"
#include "mqnetsystem.h"
#include "netcustomservice.h"
#include "netcustomsession.h"
#include "netmsg.h"
#include <cstring>
#include <mutex>
#include <slikenet/types.h>
#include <spdlog/spdlog.h>
#include <utility>

namespace hooks {

CNetCustomPlayer::CNetCustomPlayer(CNetCustomSession* session,
                                   game::IMqNetSystem* system,
                                   game::IMqNetReception* reception,
                                   const char* name,
                                   std::uint32_t id,
                                   std::shared_ptr<spdlog::logger> logger)
    : m_session{session}
    , m_system{system}
    , m_reception{reception}
    , m_name{name}
    , m_id{id}
    , m_messageTracker{session && session->getService()
                           ? session->getService()->getNativeGameMessageTracker()
                           : nullptr}
    , m_logger{std::move(logger)}
{
    vftable = nullptr;
}

CNetCustomPlayer::~CNetCustomPlayer()
{
    getLogger()->debug(__FUNCTION__);

    if (m_system) {
        getLogger()->debug(__FUNCTION__ ": destroying net system");
        m_system->vftable->destructor(m_system, 1);
    }

    if (m_reception) {
        getLogger()->debug(__FUNCTION__ ": destroying net reception");
        m_reception->vftable->destructor(m_reception, 1);
    }

    std::lock_guard<std::mutex> lock(m_messagesMutex);
    if (m_messageTracker) {
        m_messageTracker->consumed(m_messages.size());
    }
    while (!m_messages.empty()) {
        m_messages.pop();
    }
}

uint32_t CNetCustomPlayer::getClientId(const SLNet::RakNetGUID& guid)
{
    return guid.ToUint32(guid);
}

bool CNetCustomPlayer::isValidMessage(const game::NetMessageHeader* message,
                                      std::size_t availableBytes,
                                      std::uint32_t expectedMessageType)
{
    if (!message || availableBytes < sizeof(game::NetMessageHeader)
        || availableBytes >= game::netMessageMaxLength) {
        return false;
    }

    if (message->length < sizeof(game::NetMessageHeader) || message->length > availableBytes
        || message->length >= game::netMessageMaxLength
        || message->messageType != expectedMessageType) {
        return false;
    }

    return std::memchr(message->messageClassName, '\0', sizeof(message->messageClassName)) != nullptr;
}

const game::NetMessageHeader* CNetCustomPlayer::getMessageAndSender(const SLNet::Packet* packet,
                                                                    SLNet::RakNetGUID* sender,
                                                                    std::size_t* availableBytes)
{
    if (availableBytes) {
        *availableBytes = 0;
    }
    if (!packet || !packet->data || !sender || !availableBytes
        || packet->length < sizeof(SLNet::MessageID)) {
        return nullptr;
    }

    SLNet::BitStream input{packet->data, packet->length, false};
    input.IgnoreBytes(sizeof(SLNet::MessageID));
    if (!input.Read(*sender)) {
        spdlog::debug(__FUNCTION__ ": failed to read sender guid");
        return nullptr;
    }

    const auto readBits{input.GetReadOffset()};
    if ((readBits & 7u) != 0) {
        return nullptr;
    }

    const auto readBytes{static_cast<std::size_t>(readBits / 8)};
    if (readBytes > packet->length) {
        return nullptr;
    }

    *availableBytes = static_cast<std::size_t>(packet->length) - readBytes;
    const auto messageData{input.GetData() + readBytes};
    const auto message{reinterpret_cast<const game::NetMessageHeader*>(messageData)};
    if (!isValidMessage(message, *availableBytes, game::netMessageNormalType)) {
        *availableBytes = 0;
        return nullptr;
    }
    return message;
}

CNetCustomService* CNetCustomPlayer::getService() const
{
    return m_session->getService();
}

CNetCustomSession* CNetCustomPlayer::getSession() const
{
    return m_session;
}

game::IMqNetSystem* CNetCustomPlayer::getSystem() const
{
    return m_system;
}

game::IMqNetReception* CNetCustomPlayer::getReception() const
{
    return m_reception;
}

const std::string& CNetCustomPlayer::getName() const
{
    return m_name;
}

void CNetCustomPlayer::setName(const char* value)
{
    m_name = value;
}

uint32_t CNetCustomPlayer::getId() const
{
    return m_id;
}

void CNetCustomPlayer::postMessageToReceive(const game::NetMessageHeader* message,
                                            std::size_t availableBytes,
                                            std::uint32_t idFrom)
{
    if (lobbyRestartBlocksGameMessages()) {
        return;
    }
    if (!isValidMessage(message, availableBytes, game::netMessageNormalType)) {
        getLogger()->warn(__FUNCTION__ ": refusing invalid game message from 0x{:x}", idFrom);
        return;
    }

    getLogger()->debug(__FUNCTION__ ": '{:s}' from 0x{:x}", message->messageClassName, idFrom);

    auto msg = std::make_unique<unsigned char[]>(message->length);
    std::memcpy(msg.get(), message, message->length);
    {
        std::lock_guard<std::mutex> lock(m_messagesMutex);
        m_messages.push(IdMessagePair{idFrom, std::move(msg)});
        if (m_messageTracker) {
            m_messageTracker->queued();
        }
    }

    m_reception->vftable->notify(m_reception);
}

bool CNetCustomPlayer::sendRemoteMessage(const game::NetMessageHeader* message,
                                         const SLNet::RakNetGUID& to) const
{
    if (lobbyRestartBlocksGameMessages()) {
        return true;
    }
    getLogger()->debug(__FUNCTION__ ": '{:s}' to 0x{:x}", message->messageClassName,
                       getClientId(to));

    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_GAME_MESSAGE));
    stream.Write(static_cast<uint32_t>(1)); // Recipient count
    stream.Write(to);
    stream.Write((const char*)message, message->length);

    auto service = getService();
    return service->send(stream, service->getLobbyGuid(), HIGH_PRIORITY);
}

bool CNetCustomPlayer::sendRemoteMessage(const game::NetMessageHeader* message,
                                         const RemoteClients& to) const
{
    if (lobbyRestartBlocksGameMessages()) {
        return true;
    }
    getLogger()->debug(__FUNCTION__ ": '{:s}' to {:d} recipient(s)", message->messageClassName,
                       to.size());

    if (to.empty()) {
        return false;
    }

    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_GAME_MESSAGE));
    stream.Write(static_cast<uint32_t>(to.size()));
    for (const auto& recipient : to) {
        stream.Write(recipient.first);
    }
    stream.Write((const char*)message, message->length);

    auto service = getService();
    return service->send(stream, service->getLobbyGuid(), HIGH_PRIORITY);
}

bool CNetCustomPlayer::sendHostMessage(const game::NetMessageHeader* message) const
{
    if (lobbyRestartBlocksGameMessages()) {
        return true;
    }
    getLogger()->debug(__FUNCTION__ ": '{:s}' to 0x{:x}", message->messageClassName, m_id);

    auto msg = const_cast<game::NetMessageHeader*>(message);
    auto originalType = msg->messageType;
    msg->messageType = m_id == game::serverNetPlayerId ? ID_GAME_MESSAGE_TO_HOST_CLIENT
                                                       : ID_GAME_MESSAGE_TO_HOST_SERVER;

    auto service = getService();
    SLNet::BitStream stream((unsigned char*)message, message->length, false);
    bool result = service->send(stream, service->getPeerGuid(), HIGH_PRIORITY);
    msg->messageType = originalType;
    return result;
}

const std::shared_ptr<spdlog::logger>& CNetCustomPlayer::getLogger() const
{
    return m_logger;
}

void __fastcall CNetCustomPlayer::destructor(CNetCustomPlayer* thisptr, int /*%edx*/, char flags)
{
    thisptr->~CNetCustomPlayer();

    if (flags & 1) {
        spdlog::debug(__FUNCTION__ ": freeing memory");
        game::Memory::get().freeNonZero(thisptr);
    }
}

game::String* __fastcall CNetCustomPlayer::getName(CNetCustomPlayer* thisptr,
                                                   int /*%edx*/,
                                                   game::String* string)
{
    thisptr->getLogger()->debug(__FUNCTION__ ": name = '{:s}'", thisptr->m_name);
    game::StringApi::get().initFromString(string, thisptr->m_name.c_str());
    return string;
}

int __fastcall CNetCustomPlayer::getNetId(CNetCustomPlayer* thisptr, int /*%edx*/)
{
    thisptr->getLogger()->debug(__FUNCTION__ ": id = 0x{:x}", thisptr->m_id);
    return static_cast<int>(thisptr->m_id);
}

game::IMqNetSession* __fastcall CNetCustomPlayer::getSession(CNetCustomPlayer* thisptr,
                                                             int /*%edx*/)
{
    thisptr->getLogger()->debug(__FUNCTION__);
    return thisptr->m_session;
}

int __fastcall CNetCustomPlayer::getMessageCount(CNetCustomPlayer* thisptr, int /*%edx*/)
{
    std::lock_guard<std::mutex> messageGuard(thisptr->m_messagesMutex);
    return static_cast<int>(thisptr->m_messages.size());
}

game::ReceiveMessageResult __fastcall CNetCustomPlayer::receiveMessage(
    CNetCustomPlayer* thisptr,
    int /*%edx*/,
    std::uint32_t* idFrom,
    game::NetMessageHeader* buffer)
{
    std::lock_guard<std::mutex> messageGuard(thisptr->m_messagesMutex);

    if (thisptr->m_messages.empty()) {
        return game::ReceiveMessageResult::NoMessages;
    }

    const auto& pair = thisptr->m_messages.front();
    auto message = reinterpret_cast<const game::NetMessageHeader*>(pair.second.get());
    const auto consumeFront = [thisptr]() {
        thisptr->m_messages.pop();
        if (thisptr->m_messageTracker) {
            thisptr->m_messageTracker->consumed();
        }
    };

    if (message->messageType != game::netMessageNormalType) {
        thisptr->getLogger()
            ->debug(__FUNCTION__ ": message from 0x{:x} with unexpected type 0x{:x}", pair.first,
                     message->messageType);
        consumeFront();
        return game::ReceiveMessageResult::Failure;
    }

    if (message->length >= game::netMessageMaxLength) {
        thisptr->getLogger()->debug(
            __FUNCTION__ ": message from 0x{:x} with length {:d} that exeeds maximum of {:d}",
            pair.first, message->length, game::netMessageMaxLength);
        consumeFront();
        return game::ReceiveMessageResult::Failure;
    }

    *idFrom = pair.first;
    std::memcpy(buffer, message, message->length);
    consumeFront();

    thisptr->m_logger
        ->debug(__FUNCTION__ ": '{:s}' from 0x{:x} with length {:d}, messages remain = {:d}",
                buffer->messageClassName, *idFrom, buffer->length, thisptr->m_messages.size());
    return game::ReceiveMessageResult::Success;
}

void __fastcall CNetCustomPlayer::setNetSystem(CNetCustomPlayer* thisptr,
                                               int /*%edx*/,
                                               game::IMqNetSystem* netSystem)
{
    thisptr->getLogger()->debug(__FUNCTION__ ": old system = {:p}, new system = {:p}",
                                (void*)thisptr->m_system, (void*)netSystem);
    if (thisptr->m_system != netSystem) {
        if (thisptr->m_system) {
            thisptr->m_system->vftable->destructor(thisptr->m_system, 1);
        }
        thisptr->m_system = netSystem;
    }
}

int __fastcall CNetCustomPlayer::method8(CNetCustomPlayer* thisptr, int /*%edx*/, int a2)
{
    thisptr->getLogger()->debug(__FUNCTION__);
    return a2;
}

} // namespace hooks
