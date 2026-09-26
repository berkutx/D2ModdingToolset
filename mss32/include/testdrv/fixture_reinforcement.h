/*
 * Generic fixture preparation, separate from gameplay/turn-mode logic.
 */
#ifndef TESTDRV_FIXTURE_REINFORCEMENT_H
#define TESTDRV_FIXTURE_REINFORCEMENT_H
#ifdef D2_TESTDRV
namespace game { struct CMidServerLogic; }
namespace hooks::testdrv::fixture_reinforcement {
// Claims the host's first authoritative turn-zero once when D2TESTDRV_APPLY_FIXTURE
// enables D2TESTDRV_FIXTURE_PLAN transfer operations. Plan-only clients do not mutate.
// Any precondition/apply failure is terminal.
void onFirstTurnZero(game::CMidServerLogic* serverLogic);
}
#endif
#endif

