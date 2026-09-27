#ifndef SIMTURNS_LOBBY_TRANSPORT_H
#define SIMTURNS_LOBBY_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace hooks { class CNetCustomService; }
namespace game { struct NetMessageHeader; }
namespace hooks::simturns {

struct LobbyNativeTicket;
/** Always compiled: disabled builds may not start an authenticated OH room. */
bool lobbyAllowsMapStart(CNetCustomService* service);

#ifdef D2_SIMTURNS
bool lobbySupported();
bool lobbyMapArmed(const CNetCustomService* service);
std::shared_ptr<LobbyNativeTicket> lobbyTrackNativePacket(bool clientReceiver);
// Called on the lobby/UI thread; delivery owns a packet copy and may be deferred
// behind an earlier control packet until its native predecessors are applied.
void lobbyDeliverNativePacket(std::shared_ptr<LobbyNativeTicket> ticket,
                              std::function<void()> delivery);
// The existing restart policy deliberately consumes pre-NewScenario snapshots.
void lobbyDiscardNativePacket(std::shared_ptr<LobbyNativeTicket> ticket);
bool lobbyStageNativeReceive(const game::NetMessageHeader* buffer,
                            std::shared_ptr<LobbyNativeTicket> ticket,
                            std::uint32_t sender);
void lobbyRoomJoined(CNetCustomService* service, std::uint32_t room);
void lobbyRoomLeft(CNetCustomService* service);
void lobbyDisconnected(CNetCustomService* service);
void receiveLobbyControl(CNetCustomService* service, const std::uint8_t* bytes, std::size_t size);
void lobbySessionCreated(CNetCustomService* service, bool host);
void lobbyMapTeardownBegun();
/** Called only after native network teardown joined its server worker. */
void lobbyMapDestroyed();
#else
inline bool lobbySupported() { return false; }
inline bool lobbyMapArmed(const CNetCustomService*) { return false; }
inline void lobbyRoomJoined(CNetCustomService*, std::uint32_t) {}
inline void lobbyRoomLeft(CNetCustomService*) {}
inline void lobbyDisconnected(CNetCustomService*) {}
inline void receiveLobbyControl(CNetCustomService*, const std::uint8_t*, std::size_t) {}
inline void lobbySessionCreated(CNetCustomService*, bool) {}
inline void lobbyMapTeardownBegun() {}
inline void lobbyMapDestroyed() {}
#endif

} // namespace hooks::simturns
#endif
