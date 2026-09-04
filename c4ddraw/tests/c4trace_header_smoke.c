#include "../features/c4trace.h"

int c4trace_header_smoke(void)
{
    c4trace_event(0, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0, (uintptr_t)0);
    (void)c4trace_configured((const char*)0);
    (void)c4trace_environment_forced();
    return c4trace_enabled();
}
