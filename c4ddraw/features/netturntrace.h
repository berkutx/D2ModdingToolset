#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Passive native-client disconnect observer. Call on the normal GUI thread, after DllMain.
 * Never calls the disconnect callback itself or reads private MSS structures. */
void netturntrace_install(void);
/* 0 = not attempted; 1 = installed; -1 = unsupported EXE; -2 = changed native entry;
 * -3 = detour failed. */
int netturntrace_disconnect_available(void);
int timerhost_turn_trace_available(void);

#ifdef __cplusplus
}
#endif
