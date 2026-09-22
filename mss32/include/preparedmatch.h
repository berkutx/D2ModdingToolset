#ifndef PREPAREDMATCH_H
#define PREPAREDMATCH_H
#include "preparedmatchprotocol.h"
namespace game { struct NetMessageHeader; struct CMenuPhase; }
namespace hooks {
struct CMenuRandomScenario;
enum class RestartScenarioGenerationResult : int;
void resetPreparedMatch();
void receivePreparedMatch(const unsigned char* bytes, std::size_t size);
/** Main-thread safe point after peer callback fanout. True means a modal/transition is owned. */
bool processPreparedMatch();
void preparedMatchGenerationEnded(RestartScenarioGenerationResult result);
bool preparePreparedMatchRoom(CMenuRandomScenario* menu);
bool canAcceptPreparedMatch(CMenuRandomScenario* menu);
const prepared::Identity* preparedMatchRoomIdentity();
void preparedMatchRoomCreated(bool success);
void preparedMatchMenuDestroyed(CMenuRandomScenario* menu);
void observePreparedMatchSetup(const game::NetMessageHeader* message);
} // namespace hooks
#endif
