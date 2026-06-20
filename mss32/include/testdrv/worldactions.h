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

} // namespace worldactions
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_WORLDACTIONS_H
