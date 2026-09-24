/* Conditional Detours wrappers for the exact Russobit turn predicates. */

#ifndef SIMTURNS_ENGINE_HOOKS_H
#define SIMTURNS_ENGINE_HOOKS_H

#include "hooks.h"

namespace hooks::simturns::engine_hooks {

bool preflight();
void append(Hooks& hooks);

} // namespace hooks::simturns::engine_hooks

#endif // SIMTURNS_ENGINE_HOOKS_H
