/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Lobby-chat reporter: captures custom-lobby chat messages as they arrive in
 * CMenuCustomLobby::addChatMessage and exposes a JSON snapshot to the test relay.
 * Runtime-gated by D2TESTDRV_LOBBY_CHAT; compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_LOBBYCHATREPORTER_H
#define TESTDRV_LOBBYCHATREPORTER_H

#include <cstdint>
#include <string>

namespace hooks {
namespace testdrv {
namespace lobbychatreporter {

/** Record a received (or server-echoed) lobby message on the UI thread. */
void onChatReceived(const char* sender, const char* text);

/** Copy {"messages":[{"t":"...","sender":"...","text":"..."},...]} and its epoch.
 * Thread-safe; returns false before the first captured message. */
bool copyChatLog(std::string& outJson, std::uint32_t& outEpoch);

} // namespace lobbychatreporter
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_LOBBYCHATREPORTER_H
