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
    spdlog::info("[testdrv] install");

    // UI-state reporter (D2TESTDRV_UI_REPORTER) — the foundation: exposes the live dialog
    // + buttons and arms the auto-nav executor (onUiReady reads D2TESTDRV_SELFNAV /
    // D2TESTDRV_RELAY_BRIDGE to pick self-driven vs dispatcher-driven).
    uistatereporter::install();
    windowtag::start(); // [HOST]/[CLIENT] caption tag for host/join roles

    // Network interception layer: RX/TX hooks = logging AND the pass/drop/defer + TX-gate
    // seams the secret sim-turns branch registers its policy on. Independent of the relay.
    const bool wantRelay = testenv::on("D2TESTDRV_RELAY_BRIDGE");
    if (testenv::on("D2TESTDRV_NET_INTERCEPT") || wantRelay)
        nettracehooks::install();

    // Relay bridge: connect to the node relay (live UI-state forwarding + dispatcher
    // invoke/select commands + packet-trace forwarding). Inert if no relay is listening.
    if (wantRelay)
        bridge::start(self);
}

} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
