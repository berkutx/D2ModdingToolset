#ifndef C4TRACE_H
#define C4TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from normal initialization, not DllMain. Disabled by default.
 * Opt in with [menu] netTrace=1 in the EXE's C4menu.ini, or C4DLL_NETTRACE=1.
 * Only the literal value 1 enables tracing. Settings are read once per process.
 * Enabled tracing pins its containing module until process exit.
 *
 * Diagnostics perturb scheduling: QPC, atomics, a try-lock, and a background
 * writer have a cost. A trace is not a timing-neutral observation. Events never
 * wait for the writer; overload loses records and is reported in the CSV.
 * The most recent buffered events can be lost on abrupt process termination.
 * All entry points preserve the calling thread's GetLastError value.
 */
void c4trace_init(void);
int c4trace_enabled(void);
/* Read-only requested-state queries; neither initializes nor changes recording.
 * configured reads only the given INI (NULL/empty -> off), not the environment.
 * environment_forced is true only for the exact environment string "1".
 * Neither result promises that the writer is running; use enabled for that.
 */
int c4trace_configured(const char* iniPath);
int c4trace_environment_forced(void);
void c4trace_event(unsigned event, uintptr_t object, uintptr_t a, uintptr_t b,
                   uintptr_t c, uintptr_t d);

#ifdef __cplusplus
}
#endif

#endif
