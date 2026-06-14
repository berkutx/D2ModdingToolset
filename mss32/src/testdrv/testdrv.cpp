/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Installer / entry point — see testdrv/testdrv.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#include "testdrv/testdrv.h"
#include "testdrv/bootfixes.h"
#include "testdrv/nettracehooks.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/windowtag.h"
#include "version.h"
#include <spdlog/spdlog.h>

namespace hooks {
namespace testdrv {

void installEarly()
{
    bootfixes::installEarly();
}

void install(HMODULE self)
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return;
    spdlog::info("[testdrv] install: UI/nav/net/bridge");

    uistatereporter::install(); // D2TESTDRV_UI (+ arms auto-nav if D2TESTDRV_ROLE;
                                // host/join roles also install nettracehooks for RX gates)
    windowtag::start();         // [HOST]/[CLIENT] caption tag for host/join roles
    bootfixes::startActivation(); // D2TESTDRV_ACTIVATE: fake foreground so the pump runs headless

    if (testenv::on("D2TESTDRV_NET")) {
        nettracehooks::install();
        bridge::start(self); // connects only if a relay is listening; otherwise inert
    }
}

} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
