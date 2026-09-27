#ifndef TESTDRV_LOCAL_COORDINATOR_ADAPTER_H
#define TESTDRV_LOCAL_COORDINATOR_ADAPTER_H

#include <vector>
namespace hooks { struct HookInfo; }
namespace hooks::testdrv::local_coordinator_adapter {

// Explicit local test transport only. No env opt-in exists in production DLLs.
bool requested();
bool preflight();
void appendHooks(std::vector<HookInfo>& hooks);
// Starts only the pipe worker on a normal UI callback, never in DllMain.
// Native arming waits for the typed CreateNetClient boundary.
bool start();
// Signal before native clear; join after native worker teardown, before rearming.
void beginTeardown();
void stop();

} // namespace hooks::testdrv::local_coordinator_adapter
#endif
