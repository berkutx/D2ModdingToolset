/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * World-state reporter: walks the strategic-map object map through the mod's own
 * bindings::ScenarioView and emits a JSON snapshot of every player's resources and
 * every stack (id, position, owner, movement, leader, unit count, relation). The
 * game-state counterpart of the UI snapshot: it covers resource monitoring, the
 * nearby-creature scan (stack positions + relation), and hero movement points in
 * one payload. Built on the UI thread (the auto-nav tick, throttled), read by the
 * bridge thread. Gated at runtime by D2TESTDRV_WORLD; compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_WORLDREPORTER_H
#define TESTDRV_WORLDREPORTER_H

#include <cstdint>
#include <string>

namespace hooks {
namespace testdrv {
namespace worldreporter {

/** Enable the reporter if D2TESTDRV_WORLD. No-op on a non-Russobit image or if the gate is off. */
void install();

/** Rebuild the world snapshot from the live object map. UI-thread ONLY (reads game objects via
 * ScenarioView); throttled (~500ms) and a no-op before a scenario is loaded. Call through the thin
 * SEH wrapper in autonav, never from the bridge thread (getObjectMap is thread-keyed). */
void rebuildSnapshot();

/** Copy the latest world snapshot (JSON: {"day":..,"players":[..],"stacks":[..]}) and its change
 * epoch. Thread-safe; the bridge thread calls this. Returns false before the first snapshot exists. */
bool copyWorldSnapshot(std::string& outJson, std::uint32_t& outEpoch);

} // namespace worldreporter
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_WORLDREPORTER_H
