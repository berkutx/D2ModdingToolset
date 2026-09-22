/* Exact-Russobit inline patches owned by production simultaneous turns. */

#ifndef SIMTURNS_PATCHES_H
#define SIMTURNS_PATCHES_H

namespace hooks::simturns::patches {

/** Read-only verification of both roles' byte sites before process hook install. */
bool preflight();

/** Safe-point overlay activation. Suspends and IP-checks every peer thread,
 * applies the complete set, and rolls back before resuming on failure. */
bool activate();

/** Restore independent-only UI gates before the merge-day begin turn. */
bool restoreUiGates();

/** Restore synthetic-cascade repairs after the merge-day begin turn completes. */
bool restoreCascadeRepairs();

/** Restore every owned byte patch at the quiescent native-map teardown boundary. */
bool rollbackAll();

} // namespace hooks::simturns::patches

#endif // SIMTURNS_PATCHES_H
