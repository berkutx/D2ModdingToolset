/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Headless-boot fixes, pinned for the Russobit Discipl2.exe
 * (4,187,648 bytes, base 0x400000):
 *   - skip-intro  (NOP the fullscreen BinkOpen at 0x67D5B2);
 *   - black-screen fg-flag (force the foreground flag at 0x5628BE so the boot
 *     pump advances when launched without foreground).
 *
 * These are the two patches a 20x20 controlled headless test proved necessary and
 * sufficient (fg-flag: 0/20 without -> 20/20 with). Forced OS window activation
 * (SetForegroundWindow / WM_ACTIVATE*) was deliberately NOT ported: it only helps
 * the window PAINT (GOG GL-wrapper render-pause, RE/08) and we read game STATE,
 * not pixels — so it was never exercised in a passing run. Compile-gated by
 * D2_TESTDRV; each fix is independently gated by a runtime env var.
 */

#ifndef TESTDRV_BOOTFIXES_H
#define TESTDRV_BOOTFIXES_H

namespace hooks {
namespace testdrv {
namespace bootfixes {

/** Apply the static byte patches from DllMain (earliest): skip-intro if
 * D2TESTDRV_SKIP_INTRO, fg-flag if D2TESTDRV_BOOT. No-op on a non-Russobit image. */
void installEarly();

/** Spawn the forced-activation thread (D2TESTDRV_ACTIVATE) that fakes window
 * activation/foreground so message-gated paths run headless. Call post-DllMain. */
void startActivation();

} // namespace bootfixes
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_BOOTFIXES_H
