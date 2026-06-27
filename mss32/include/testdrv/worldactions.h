/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * World actions: state-mutating test commands (move a stack, ...). See worldactions.cpp.
 *
 * Compile-gated by D2_TESTDRV. The reporter side (worldreporter) only observes; this side issues
 * the move the same way the game's own click handler does, so a test drives a hero exactly like a
 * real player without emulating input.
 */

#ifndef TESTDRV_WORLDACTIONS_H
#define TESTDRV_WORLDACTIONS_H

namespace hooks {
namespace testdrv {
namespace worldactions {

/**
 * Move the stack <stackId> toward tile (x, y).
 *
 * The path is built with the GAME'S OWN per-tile functions (computeMovementCost as the edge weight,
 * stackCanMoveToPosition as passability) and annotated by the native PathInfoListApi::populateFromPath,
 * then issued via CPhaseGameApi::sendStackMoveMsg, the exact call the strategic-map click handler makes.
 * Only the visit order of the search is ours; every cost/passability decision is a native game function.
 *
 * If (x, y) is not directly reachable, the stack moves toward it as far as it can (mirrors a player
 * clicking a far/blocked tile). MUST be called on the UI thread (from the autonav tick). Returns true
 * if a move message was issued (own stack, our turn, a reachable destination).
 */
bool moveStack(const char* stackId, int x, int y);

/**
 * Hire the mercenary <unitId> (a camp roster entry the world reporter lists) from merc camp <campId>
 * into the stack <stackId>'s group at its first fitting free slot. Sends the engine's OWN
 * CSiteBuyUnitMsg via CPhaseGame::sendSiteBuyUnitMsg, the exact call the camp's drag-drop makes on a
 * drop, so the host validates + applies + broadcasts it: the hire REPLICATES to every player (acting
 * client and host alike). MUST be called on the UI thread. Returns true if the message was sent (own
 * stack, our turn, a free slot); the host has final say on gold/validity.
 */
bool hireMerc(const char* campId, const char* stackId, const char* unitId);

/**
 * Move the unit at <sourcePos> to <targetPos> within stack <stackId>'s 6-cell formation. Sends the
 * engine's OWN CStackSwapUnitMsg via CPhaseGame::sendStackSwapUnitMsg, the exact call the formation
 * drag-drop makes, so the host applies it and broadcasts: the rearrange REPLICATES to every player. If
 * <targetPos> is EMPTY this is a plain MOVE (the source cell empties); if OCCUPIED it is a SWAP. Use it
 * to drop a just-hired unit into a free slot, or to set the battle line (ranged/casters back, melee
 * front). MUST be called on the UI thread. Returns true if the message was sent (own stack, our turn,
 * SOURCE cell occupied, positions 0..5 and distinct).
 */
bool moveGroupUnit(const char* stackId, int sourcePos, int targetPos);

} // namespace worldactions
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_WORLDACTIONS_H
