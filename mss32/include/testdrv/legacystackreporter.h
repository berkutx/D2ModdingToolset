/*
 * Removable DebugTest-only live-stack reporter used by the legacy
 * simultaneous-turn acceptance oracle.
 *
 * This module deliberately does not walk the object map from a UI callback.
 * It captures the adjusted IMidScenarioObject pointer seen by the original
 * Russobit CMidStack::Stream and samples the corresponding live CMidStack at
 * a fixed 500 ms cadence on a low-priority observer thread.
 */

#ifndef TESTDRV_LEGACYSTACKREPORTER_H
#define TESTDRV_LEGACYSTACKREPORTER_H

namespace hooks {
namespace testdrv {
namespace legacystackreporter {

/** Parse D2TESTDRV_LEGACY_STACKS exactly. Missing means disabled; the only
 * accepted enabled spelling is "1". Invalid or empty values fail preflight. */
bool readRequestedGate(bool& requested);

/** Validate the immutable DebugTest plan without patching executable code.
 * An enabled plan requires the exact Russobit image and D2TESTDRV_ROLE=host. */
bool preflight(bool requested);

/** Install the one CMidStack::Stream entry trampoline after ordinary hooks.
 * The target must still contain the exact bytes proven during preflight. */
bool commit();

/** Start the process-lifetime, low-priority 500 ms sampler exactly once.
 * Must be called only after the relay bridge has been started. */
bool start();

} // namespace legacystackreporter
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_LEGACYSTACKREPORTER_H

