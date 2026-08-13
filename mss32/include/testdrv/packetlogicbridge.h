/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Named-pipe / TCP client connecting mss32 to the node.js relay
 * (d2lobby.packetlogic). Protocol:
 *   uint32 length (= 4 + payload) | uint16 opcode | uint16 flags | byte[] payload
 * Pipe name: \\.\pipe\d2lobby.packetlogic. All integers little-endian.
 *
 * It forwards the live UI/world snapshots and hands incoming commands to the
 * UI-thread executor. Compile-gated by D2_TESTDRV.
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
 * the Hello handshake, and streams snapshots/command results. */
bool start(HMODULE selfModule);

/** Report the outcome of a dispatcher command back to the relay. The relay holds the
 * matching POST open until this arrives, so the test learns whether the addressed dialog
 * and widget were actually found. Sent from the UI thread before the action runs. */
void send_command_result(std::uint32_t seq, bool found);

/** Register a handler for control opcodes the bridge does not own, e.g. the
 * dispatcher's InvokeButton / SetSelection commands, which the auto-nav executor
 * queues for the UI thread. Runs on the bridge thread; must not block. Null clears. */
void setCommandCallback(CommandCallback cb);

} // namespace bridge
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_PACKETLOGICBRIDGE_H
