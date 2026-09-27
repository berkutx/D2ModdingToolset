#ifndef TESTS_SIMTURNS_LOBBY_STARTUP_CONTROLLER_H
#define TESTS_SIMTURNS_LOBBY_STARTUP_CONTROLLER_H

#include "simturns/session_types.h"
#include <cstdint>

namespace hooks::simturns {

bool available();
bool strategicQueueIdle();
std::uint64_t pregameNativeNotificationGeneration();
bool beginSession(Role role);

} // namespace hooks::simturns

#endif
