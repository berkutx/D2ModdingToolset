#ifndef LOBBYRESTART_H
#define LOBBYRESTART_H

#include <cstdint>
#include <vector>

namespace game {
struct CMidgard;
struct CMenuPhase;
struct NetMessageHeader;
}

namespace hooks {

namespace LobbyProtocol {
// Keep in sync with lobby/server. One operation byte and one nonzero attempt token.
enum class RestartOperation : std::uint8_t
{
    Prepare = 0,
    ClientReady = 1,
    HostReady = 2,
    Start = 3,
    Abort = 4,
    HostCreated = 5,
    Join = 6,
    SetupReady = 7,
    Launch = 8,
    Loaded = 9,
    Complete = 10,
};
static constexpr std::uint32_t coordinatedRestartFeature{1};
}

void handleLobbyRestart(LobbyProtocol::RestartOperation operation, std::uint64_t token);
void handleLobbyRestartLoopbackFence(std::uint64_t token);
/** Runs deferred native/UI transitions, never from inside a peer callback. */
bool processLobbyRestart();
bool isLobbyRestartActive();
bool lobbyRestartBlocksGameMessages();
/** True only around the native teardown which must retain the RoomsPlugin membership. */
bool retainLobbyRoomForRestart();
bool isLobbyRestartMenuTransition();
void enterLobbyRestartMenu(game::CMenuPhase* phase);
void finishLobbyRestartMenuReturn();
void resetLobbyRestart();

/** Retains the selected race/lord and replaces native default requests on the next attempt. */
void prepareLobbyRestartSetupMessage(const game::NetMessageHeader* message,
                                     std::vector<unsigned char>& replacement);
void observeLobbyRestartSetupInfo(const game::NetMessageHeader* message);
/** Called on the client UI thread immediately before native message dispatch. */
bool allowLobbyRestartClientMessage(const game::NetMessageHeader* message);

} // namespace hooks

#endif
