/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2024 Stanislav Egorov.
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

#ifndef NETCUSTOMPEER_H
#define NETCUSTOMPEER_H

#include <RakPeer.h>
#include <atomic>
#include <cstdint>

namespace hooks {

/**
 * Sends notification message to a window when there are network packets to receive.
 * Keeps notification flag to be reset outside by the receiver to avoid cluttering the window's
 * message queue.
 */
class CNetCustomPeer : public SLNet::RakPeer
{
public:
    CNetCustomPeer(const char* packetNotificationMessageName);
    ~CNetCustomPeer();

    bool IsPacketNotificationSent() const;
    void ResetPacketNotification();
    /**
     * Acknowledges the notification consumed by the UI thread and immediately rearms it when a
     * packet arrived during the final drain/reset window.
     */
    void CompletePacketProcessing();

protected:
    static void UpdateThreadCallback(RakPeerInterface* peer, void* data);

    bool HasPacketsToReceive();
    void SendPacketNotification();

private:
    std::uint32_t m_packetNotificationMessageId;
    std::atomic<bool> m_packetNotificationSent;
};

} // namespace hooks

#endif // NETCUSTOMPEER_H
