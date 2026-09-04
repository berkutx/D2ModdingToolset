#ifndef C4_INVENTORYTRACE_H
#define C4_INVENTORYTRACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in only. Call after c4trace_init on the first GUI dispatch, not DllMain.
 * No mod-DLL offsets, gameplay mutations, timer/affinity/priority changes.
 * Unknown executables/modified hook sites remain untouched and emit event 101. */
void inventorytrace_install(void);

/* Cached, fail-closed identity gate for other read-only EXE-layout probes.
 * Returns 0 while disabled, unsupported, or another caller is checking identity.
 * Preserves GetLastError. The first enabled call reads/hashes the EXE file. */
int inventorytrace_exact_exe(void);

/* Shared read-only admission check. Unlike the diagnostic wrapper above this
 * works when netTrace is off; caller must gate its own opt-in feature first.
 * Returns 0 while another thread hashes; no waiting and no mod identity. */
int c4_exact_game_exe(void);

#ifdef __cplusplus
}
#endif

#endif
