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

#include "netcustomsession.h"
#include "d2string.h"
#include "lobbyrestart.h"
#include "mempool.h"
#include "mqnetsystem.h"
#include "netcustomplayerclient.h"
#include "netcustomplayerserver.h"
#include "netcustomservice.h"
#include "utils.h"
#include <atomic>
#include <chrono>
#include <spdlog/spdlog.h>
#include <thread>

namespace hooks {

CNetCustomSession::CNetCustomSession(CNetCustomService* service,
                                     const char* name,
                                     const SLNet::RakNetGUID& serverGuid)
    : m_service{service}
    , m_name{name}
    , m_clientCount{0}
    , m_maxPlayers{2}
    , m_isHost{serverGuid == service->getPeerGuid()}
    , m_serverGuid(serverGuid)
{
    spdlog::debug(__FUNCTION__ ": server = 0x{:x}, is host = {:d}",
                  m_serverGuid.ToUint32(m_serverGuid), (int)m_isHost);

    static game::IMqNetSessionVftable vftable = {
        (game::IMqNetSessionVftable::Destructor)destructor,
        (game::IMqNetSessionVftable::GetName)(GetName)getName,
        (game::IMqNetSessionVftable::GetClientCount)getClientCount,
        (game::IMqNetSessionVftable::GetMaxClients)getMaxClients,
        (game::IMqNetSessionVftable::GetPlayers)getPlayers,
        (game::IMqNetSessionVftable::CreateClient)createClient,
        (game::IMqNetSessionVftable::CreateServer)createServer,
    };

    this->vftable = &vftable;
}

CNetCustomSession ::~CNetCustomSession()
{
    spdlog::debug(__FUNCTION__);
    if (!retainLobbyRoomForRestart()) {
        m_service->leaveRoom();
    }
    m_service->notifySessionDestroyed(this);
}

CNetCustomService* CNetCustomSession::getService() const
{
    return m_service;
}

const std::string& CNetCustomSession::getName() const
{
    return m_name;
}

bool CNetCustomSession::isHost() const
{
    return m_isHost;
}

bool CNetCustomSession::setMaxPlayers(int maxPlayers)
{
    if (maxPlayers < 1 || maxPlayers > 4) {
        return false;
    }

    if (m_isHost && !isLobbyRestartActive()) {
        // -1 because room already have moderator
        const auto result{m_service->changeRoomPublicSlots(maxPlayers - 1)};
        if (result) {
            m_maxPlayers = maxPlayers;
        }

        return result;
    } else {
        m_maxPlayers = maxPlayers;
        return true;
    }
}

void __fastcall CNetCustomSession::destructor(CNetCustomSession* thisptr, int /*%edx*/, char flags)
{
    thisptr->~CNetCustomSession();

    if (flags & 1) {
        spdlog::debug(__FUNCTION__ ": freeing memory");
        game::Memory::get().freeNonZero(thisptr);
    }
}

game::String* __fastcall CNetCustomSession::getName(CNetCustomSession* thisptr,
                                                    int /*%edx*/,
                                                    game::String* sessionName)
{
    spdlog::debug(__FUNCTION__ ": name = {:s}", thisptr->m_name);
    game::StringApi::get().initFromString(sessionName, thisptr->m_name.c_str());
    return sessionName;
}

int __fastcall CNetCustomSession::getClientCount(CNetCustomSession* thisptr, int /*%edx*/)
{
    spdlog::debug(__FUNCTION__ ": client count = {:d}", thisptr->m_clientCount);
    return thisptr->m_clientCount;
}

int __fastcall CNetCustomSession::getMaxClients(CNetCustomSession* thisptr, int /*%edx*/)
{
    spdlog::debug(__FUNCTION__ ": max clients = {:d}", thisptr->m_maxPlayers);
    return thisptr->m_maxPlayers;
}

void __fastcall CNetCustomSession::getPlayers(CNetCustomSession* thisptr,
                                              int /*%edx*/,
                                              game::List<game::IMqNetPlayerEnum*>* players)
{
    spdlog::debug(__FUNCTION__ ": should not be called, returning nothing");
}

void __fastcall CNetCustomSession::createClient(CNetCustomSession* thisptr,
                                                int /*%edx*/,
                                                game::IMqNetPlayerClient** client,
                                                game::IMqNetSystem* netSystem,
                                                game::IMqNetReception* reception,
                                                const char* clientName)
{
    spdlog::debug(__FUNCTION__ ": client name = {:s}", clientName);

    auto result = (CNetCustomPlayerClient*)game::Memory::get().allocate(
        sizeof(CNetCustomPlayerClient));
    new (result)
        CNetCustomPlayerClient(thisptr, netSystem, reception, clientName, thisptr->m_serverGuid);
    *client = (game::IMqNetPlayerClient*)result;
    ++thisptr->m_clientCount;
}

void __fastcall CNetCustomSession::createServer(CNetCustomSession* thisptr,
                                                int /*%edx*/,
                                                game::IMqNetPlayerServer** server,
                                                game::IMqNetSystem* netSystem,
                                                game::IMqNetReception* reception)
{
    spdlog::debug(__FUNCTION__);

    auto result = (CNetCustomPlayerServer*)game::Memory::get().allocate(
        sizeof(CNetCustomPlayerServer));
    new (result) CNetCustomPlayerServer(thisptr, netSystem, reception);
    *server = (game::IMqNetPlayerServer*)result;
}

} // namespace hooks
