/*
 * DebugTest environment/fingerprint helpers.
 */

#ifdef D2_TESTDRV

#include "testdrv/testenv.h"
#include "executablefingerprint.h"

namespace hooks {
namespace testdrv {
namespace testenv {

namespace {
unsigned char g_moduleAnchor = 0;
}

bool supportedGameBuild()
{
    return executablefingerprint::isExactRussobit();
}

bool pinHarnessModule()
{
    static const bool pinned = []() {
        HMODULE module = nullptr;
        return GetModuleHandleExW(
                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                   reinterpret_cast<LPCWSTR>(&g_moduleAnchor), &module)
               != FALSE;
    }();
    return pinned;
}

} // namespace testenv
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV

