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

#ifndef NETCUSTOMPLAYERCLIENT_H
#define NETCUSTOMPLAYERCLIENT_H

#include "mqnetplayerclient.h"
#include "netcustomplayer.h"
#include "netcustomservice.h"

namespace hooks {

class CNetCustomPlayerClient : public CNetCustomPlayer
{
public:
    CNetCustomPlayerClient(CNetCustomSession* session,
                           game::IMqNetSystem* system,
                           game::IMqNetReception* reception,
                           const char* name,
                           const SLNet::RakNetGUID& serverGuid);
    ~CNetCustomPlayerClient();

protected:
    using CNetCustomPlayer::setName;

    // IMqNetPlayerClient
    using SetName = bool(__fastcall*)(CNetCustomPlayerClient* thisptr,
                                      int /*%edx*/,
                                      const char* name);
    static void __fastcall destructor(CNetCustomPlayerClient* thisptr, int /*%edx*/, char flags);
    static bool __fastcall sendMessage(CNetCustomPlayerClient* thisptr,
                                       int /*%edx*/,
                                       std::uint32_t idTo,
                                       const game::NetMessageHeader* message);
    static bool __fastcall setName(CNetCustomPlayerClient* thisptr, int /*%edx*/, const char* name);
    static bool __fastcall isHost(CNetCustomPlayerClient* thisptr, int /*%edx*/);

private:
    /** Forwards the host's own lord pick to the lobby server (ID_LOBBY_PLAYER_SETUP)
     * since it never leaves the local loopback otherwise. */
    void forwardPlayerSetupToLobby(const game::NetMessageHeader* message) const;

    class PeerCallback : public NetPeerCallback
    {
    public:
        PeerCallback(CNetCustomPlayerClient* player)
            : m_player{player}
        { }
        virtual ~PeerCallback() = default;

        void onPacketReceived(DefaultMessageIDTypes type,
                              SLNet::RakPeerInterface* peer,
                              const SLNet::Packet* packet) override;

    private:
        CNetCustomPlayerClient* m_player;
    };

    class RoomsCallback : public SLNet::RoomsCallback
    {
    public:
        RoomsCallback(CNetCustomPlayerClient* player)
            : m_player{player}
        { }

        void RoomDestroyedOnModeratorLeft_Callback(
            const SLNet::SystemAddress& senderAddress,
            SLNet::RoomDestroyedOnModeratorLeft_Notification* notification) override;

    private:
        CNetCustomPlayerClient* m_player;
    };

    PeerCallback m_peerCallback;
    RoomsCallback m_roomsCallback;
    SLNet::RakNetGUID m_serverGuid;
};

assert_offset(CNetCustomPlayerClient, vftable, 0);

} // namespace hooks

#endif // NETCUSTOMPLAYERCLIENT_H
