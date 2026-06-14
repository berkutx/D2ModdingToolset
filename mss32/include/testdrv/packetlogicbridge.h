/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Named-pipe / TCP client connecting mss32 to the node.js relay
 * (d2lobby.packetlogic). Protocol:
 *   uint32 length (= 4 + payload) | uint16 opcode | uint16 flags | byte[] payload
 * Pipe name: \\.\pipe\d2lobby.packetlogic. All integers little-endian.
 *
 * Pure observability/control transport: it forwards RX/TX packet traces + log
 * lines to the relay and hands incoming control opcodes it does not itself own to
 * a registered command callback (so a consumer can extend the protocol without
 * this file knowing about it). Compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_PACKETLOGICBRIDGE_H
#define TESTDRV_PACKETLOGICBRIDGE_H

#include <cstdint>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace bridge {

/** Handler for a control opcode the bridge itself does not own. Runs on the
 * bridge thread; must not block. */
using CommandCallback = void (*)(std::uint16_t op, const std::uint8_t* payload,
                                 std::uint32_t size);

/** Start the bridge background thread (idempotent). Connects to the relay, does
 * the Hello handshake, and registers RX+TX trace sinks on nettracehooks. */
bool start(HMODULE selfModule);

/** Signal the bridge thread to disconnect. */
void stop();

/** Send a Log line to the relay. Safe from any thread; dropped if not connected. */
void send_log(const char* utf8_message);

/** Register a handler for control opcodes the bridge does not own (null clears). */
void setCommandCallback(CommandCallback cb);

} // namespace bridge
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_PACKETLOGICBRIDGE_H
