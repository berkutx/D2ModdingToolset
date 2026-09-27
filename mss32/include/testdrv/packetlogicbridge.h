/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Named-pipe / TCP protocol-v7 client connecting mss32 to the node.js relay
 * (d2lobby.packetlogic). Protocol:
 *   uint32 length (= 4 + payload) | uint16 opcode | uint16 flags | byte[] payload
 * Pipe name: \\.\pipe\d2lobby.packetlogic. All integers little-endian.
 *
 * Pure observability/control transport: it forwards the live UI snapshot, RX/TX
 * packet traces + log lines to the relay and hands incoming control opcodes it does
 * not itself own to a registered command callback (so a consumer can extend the
 * protocol without this file knowing about it). Compile-gated by D2_TESTDRV.
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

/** Stage the immutable D2TESTDRV_TURN_EVENTS plan and preflight the
 * shared post-dispatch/post-send observer bundle before ordinary hooks. */
bool preflightTurnEvents(bool requested);

/** Atomically publish the preflighted post-observer pair, then occupy only the
 * independent DebugTest secondary RX policy slot. Never replaces production. */
bool commitTurnEvents();

/** Start the single process-lifetime bridge background thread. It makes one
 * connection attempt, requires the exact v7 HelloAck and never reconnects. */
bool start(HMODULE selfModule);

/** Signal the bridge thread to disconnect. */
void stop();

/** Send a Log line to the relay. Safe from any thread; dropped if not connected. */
void send_log(const char* utf8_message);

/** Report the outcome of a dispatcher command back to the relay. The relay holds the
 * matching POST open until this arrives, so the test learns whether the addressed dialog
 * and widget were actually found. Sent from the UI thread before the action runs. */
void send_command_result(std::uint32_t seq, bool found);

/** Publish the exact UI-thread edge immediately before one MoveStack enters
 * worldactions::moveStack, or after one paired EndTurn has armed its exact
 * target for release on a later natural frame. This is not an acknowledgement:
 * the matching CommandResult remains mandatory. */
void send_command_started(std::uint32_t seq);

/** Result of the one exact Russobit TOG_AUTOBATTLE callback. Unlike the
 * generic CommandResult this is published only after the callback returns and
 * its viewer-side kick invariant has been read back on the UI thread. */
struct AutoBattleKickResult
{
    bool succeeded;
    std::uint8_t controllerGateBefore;
    std::uint8_t kickStateBefore;
    std::uint8_t kickStateAfter;
    std::uint8_t sideSelector;
    std::uint8_t flag38Before;
    std::uint8_t flag38After;
    std::uint8_t flag39Before;
    std::uint8_t flag39After;
    std::uint32_t memberFunction;
};

void send_auto_battle_kick_result(std::uint32_t seq,
                                  const AutoBattleKickResult& result);

/** Publish one host-side legacy live-stack census as opcode 0x0412.
 * Payload is exactly LE u32 count followed by count packed 20-byte records:
 * {u32 id, u32 owner, i32 x, i32 y, u32 movement}. The bridge validates the
 * shape and fails closed; it does not retry or reconnect. */
void send_legacy_stacks_snapshot(const void* payload, std::uint32_t size);

/** Register a handler for control opcodes the bridge does not own, e.g. the
 * dispatcher's InvokeButton / SetSelection commands, which the auto-nav executor
 * queues for the UI thread. Runs on the bridge thread; must not block. Null clears. */
void setCommandCallback(CommandCallback cb);

} // namespace bridge
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_PACKETLOGICBRIDGE_H
