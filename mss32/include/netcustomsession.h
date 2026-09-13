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

#ifndef NETCUSTOMSESSION_H
#define NETCUSTOMSESSION_H

#include "d2list.h"
#include "mqnetsession.h"
#include <slikenet/types.h>
#include <string>

namespace game {
struct String;
struct IMqNetSystem;
struct IMqNetReception;
struct IMqNetPlayer;
struct IMqNetPlayerEnum;
struct IMqNetPlayerServer;
struct IMqNetPlayerClient;
} // namespace game

namespace hooks {

class CNetCustomService;
class CNetCustomPlayerClient;
class CNetCustomPlayerServer;

class CNetCustomSession : public game::IMqNetSession
{
public:
    CNetCustomSession(CNetCustomService* service,
                      const char* name,
                      const SLNet::RakNetGUID& serverGuid);
    ~CNetCustomSession();

    CNetCustomService* getService() const;
    const std::string& getName() const;
    bool isHost() const;
    const SLNet::RakNetGUID& getServerGuid() const { return m_serverGuid; }
    int getMaxPlayers() const { return m_maxPlayers; }
    bool setMaxPlayers(int maxPlayers);

protected:
    // IMqNetSession
    using GetName = game::String*(__fastcall*)(CNetCustomSession* thisptr,
                                               int /*%edx*/,
                                               game::String* sessionName);
    static void __fastcall destructor(CNetCustomSession* thisptr, int /*%edx*/, char flags);
    static game::String* __fastcall getName(CNetCustomSession* thisptr,
                                            int /*%edx*/,
                                            game::String* sessionName);
    static int __fastcall getClientCount(CNetCustomSession* thisptr, int /*%edx*/);
    static int __fastcall getMaxClients(CNetCustomSession* thisptr, int /*%edx*/);
    static void __fastcall getPlayers(CNetCustomSession* thisptr,
                                      int /*%edx*/,
                                      game::List<game::IMqNetPlayerEnum*>* players);
    static void __fastcall createClient(CNetCustomSession* thisptr,
                                        int /*%edx*/,
                                        game::IMqNetPlayerClient** client,
                                        game::IMqNetSystem* netSystem,
                                        game::IMqNetReception* reception,
                                        const char* clientName);
    static void __fastcall createServer(CNetCustomSession* thisptr,
                                        int /*%edx*/,
                                        game::IMqNetPlayerServer** server,
                                        game::IMqNetSystem* netSystem,
                                        game::IMqNetReception* reception);

private:
    CNetCustomService* m_service;
    std::string m_name;
    int m_clientCount;
    int m_maxPlayers;
    bool m_isHost;
    SLNet::RakNetGUID m_serverGuid;
};

assert_offset(CNetCustomSession, vftable, 0);

} // namespace hooks

#endif // NETCUSTOMSESSION_H
