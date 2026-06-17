/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Tags the game window caption with the role, "<base>  [HOST]" / "[CLIENT]", so
 * the two Discipl2 windows of a two-instance test are distinguishable at a glance
 * (and so orchestration scripts can target / count test-launched instances by the
 * tag). Role from D2TESTDRV_ROLE; compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_WINDOWTAG_H
#define TESTDRV_WINDOWTAG_H

namespace hooks {
namespace testdrv {
namespace windowtag {

/** Start the caption-tagging thread if D2TESTDRV_ROLE is host/join. No-op
 * otherwise (probe/exit/unset get no tag). */
void start();

} // namespace windowtag
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_WINDOWTAG_H
