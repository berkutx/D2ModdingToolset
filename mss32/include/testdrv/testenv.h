/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 *
 * Tiny shared helper: read a boolean runtime gate from an environment variable.
 * The test/logging system is gated two ways — compile-time by D2_TESTDRV (is the
 * code in the DLL at all) and runtime by D2TESTDRV_* env vars (is this feature on
 * in this launch). This is the runtime half. Compile-gated by D2_TESTDRV.
 */

#ifndef TESTDRV_TESTENV_H
#define TESTDRV_TESTENV_H

#ifdef D2_TESTDRV

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace testenv {

/** True if env var `name` is set to 1/t/T/y/Y. */
inline bool on(const char* name)
{
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0
           && (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y');
}

} // namespace testenv
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV

#endif // TESTDRV_TESTENV_H
