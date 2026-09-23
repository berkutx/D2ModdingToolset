/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Installer / entry point. See testdrv/testdrv.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro no test code is compiled.
 */

#ifdef D2_TESTDRV

#include "testdrv/testdrv.h"
#include "testdrv/fixtureplan.h"
#include "midclient.h"
#include "midgard.h"
#include "phasegame.h"
#include <cstddef>
#include "testdrv/autonav.h"
#include "testdrv/bootfixes.h"
#include "testdrv/legacystackreporter.h"
#include "testdrv/nettracehooks.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/worldactions.h"
#include "testdrv/worldreporter.h"
#include "uiframedispatcher.h"
#include "testdrv/local_coordinator_adapter.h"
#include <atomic>
#include <cstdlib>
#include <spdlog/spdlog.h>
#include <crtdbg.h>
#include <iostream>
#include <streambuf>
#include <string>

namespace hooks {
namespace testdrv {

namespace {

std::atomic<bool> g_runtimeArmed{false};
std::atomic<bool> g_bridgeStartClaimed{false};
HMODULE g_selfModule = nullptr;
bool g_wantRelay = false;

struct HarnessPlan
{
    bool requested = false;
    bool wantRelay = false;
    bool wantUi = false;
    bool wantWorld = false;
    bool wantLegacyStacks = false;
    bool legacyStacksGateValid = true;
    bool wantNet = false;
    bool wantTurnEvents = false;
    bool wantExactLegacyMoves = false;
    bool wantCleanLongMoves = false;
    bool wantLegacyHostLoopback = false;
    bool moveModeGatesValid = true;
    bool wantSelfNav = false;
    bool wantAutoDismiss = false;
    bool wantAutoBattlePrearm = false;
    bool wantScriptedPopups = false;
    bool wantScriptedPopupConfirmations = false;
    bool skipIntro = false;
    bool blackScreen = false;
    bool needsUiFrame = false;
    bool needsNet = false;
};

HarnessPlan g_plan;
bool g_planParsed = false;
bool g_preflightComplete = false;
bool g_commitComplete = false;

bool readExactOneGate(const char* name, bool& requested)
{
    requested = false;
    char value[2]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0)
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
    if (length == 1 && value[0] == '1') {
        requested = true;
        return true;
    }
    return false;
}

HarnessPlan parseHarnessPlan()
{
    HarnessPlan plan;
    plan.wantRelay = testenv::on("D2TESTDRV_RELAY_BRIDGE");
    plan.wantUi = testenv::on("D2TESTDRV_UI_REPORTER");
    plan.wantWorld = testenv::on("D2TESTDRV_WORLD");
    plan.legacyStacksGateValid =
        legacystackreporter::readRequestedGate(plan.wantLegacyStacks);
    plan.wantNet = testenv::on("D2TESTDRV_NET_INTERCEPT");
    plan.wantTurnEvents =
        testenv::on("D2TESTDRV_TURN_EVENTS");
    plan.moveModeGatesValid =
        readExactOneGate("D2TESTDRV_EXACT_LEGACY_MOVES", plan.wantExactLegacyMoves)
        && readExactOneGate("D2TESTDRV_CLEAN_LONG_MOVES", plan.wantCleanLongMoves)
        && readExactOneGate("D2TESTDRV_LEGACY_HOST_LOOPBACK", plan.wantLegacyHostLoopback);
    plan.wantSelfNav = testenv::on("D2TESTDRV_SELFNAV");
    plan.wantAutoDismiss = testenv::on("D2TESTDRV_AUTODISMISS");
    plan.wantAutoBattlePrearm =
        testenv::on("D2TESTDRV_AUTO_BATTLE_PREARM");
    plan.wantScriptedPopups =
        testenv::on("D2TESTDRV_SCRIPTED_POPUPS");
    plan.wantScriptedPopupConfirmations =
        testenv::on("D2TESTDRV_SCRIPTED_POPUPS_CONFIRMATIONS");
    plan.skipIntro = testenv::on("D2TESTDRV_SKIP_INTRO");
    plan.blackScreen = testenv::on("D2TESTDRV_BLACKSCREEN_FIX");
    plan.needsUiFrame = plan.wantRelay || plan.wantSelfNav || plan.wantAutoDismiss
                        || plan.wantAutoBattlePrearm || plan.wantScriptedPopups;
    // Movement geometry and transport are independent: exact legacy commands
    // still use natural delivery. Direct PacketIn needs its own diagnostic gate.
    plan.needsNet = plan.wantNet || plan.wantRelay || plan.wantTurnEvents
                    || plan.wantWorld;
    plan.requested = plan.wantRelay || plan.wantUi || plan.wantWorld
                     || plan.wantLegacyStacks || !plan.legacyStacksGateValid || plan.wantNet
                     || plan.wantTurnEvents || plan.wantExactLegacyMoves
                     || plan.wantCleanLongMoves || plan.wantLegacyHostLoopback
                     || !plan.moveModeGatesValid || plan.wantSelfNav
                      || plan.wantAutoDismiss || plan.wantAutoBattlePrearm
                      || plan.wantScriptedPopups
                      || plan.wantScriptedPopupConfirmations
                     || plan.skipIntro || plan.blackScreen;
    return plan;
}

[[noreturn]] void failFastInstall(const char* seam, unsigned exitCode)
{
    spdlog::critical("[testdrv] post-hook commit failed at {}; terminating", seam);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

[[noreturn]] void failFastRuntimeStart(const char* seam)
{
    spdlog::critical("[testdrv] relay bridge runtime start failed at {}; terminating", seam);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), 0xD2E77320u);
    std::abort();
}

void startRuntimeOnce(const char* seam)
{
    if (!local_coordinator_adapter::start())
        failFastRuntimeStart("local coordinator adapter");
    if (!g_runtimeArmed.load(std::memory_order_acquire) || !g_wantRelay)
        return;
    bool expected = false;
    if (!g_bridgeStartClaimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;
    if (!bridge::start(g_selfModule))
        failFastRuntimeStart(seam);
    if (!legacystackreporter::start())
        failFastRuntimeStart("legacy stack reporter sampler");
    spdlog::info("[testdrv] relay helper started exactly once after DllMain ({})", seam);
}

#ifdef _DEBUG
// Route CRT debug reports (the debug-heap "Debug Assertion Failed" box and friends) away from the
// modal MessageBox that the dispatcher cannot click (it drives game dialogs, not native boxes, so an
// unhandled assert hangs the run). An assert or error is a real fault, e.g. the generator corrupting
// the heap: the point is to CATCH it and fail, not to Ignore-and-continue, which would hide the bug.
// So record the reason, flush, and kill the process; the dispatcher sees the game die and fails the
// run fast, reading the logged reason from the DLL log file (no relay needed). Warnings are
// non-fatal. Only the debug CRT (the DebugTest build) emits these reports at all.
int __cdecl crtReportHook(int reportType, char* message, int* returnValue)
{
    const char* kind = reportType == _CRT_ASSERT ? "assert" : (reportType == _CRT_ERROR ? "error" : "warn");
    std::string msg = message ? message : "(null)";
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    spdlog::error("[testdrv][crt-{}] {}", kind, msg);
    if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR) {
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0xA55E27u); // fail fast on a real fault, do not continue
    }
    if (returnValue)
        *returnValue = 0; // warning only: do not break into a debugger, just carry on
    return TRUE;          // handled: no dialog
}

// The sol Lua binding prints its panic ("expected table, received string", ...) to std::cerr;
// forward that into the log so a generator panic is captured with everything else.
class CerrToLog : public std::streambuf {
    std::string line;
protected:
    int overflow(int ch) override
    {
        if (ch == '\n') {
            if (!line.empty()) {
                spdlog::error("[testdrv][stderr] {}", line);
                line.clear();
            }
        } else if (ch != EOF && ch != '\r') {
            line.push_back(static_cast<char>(ch));
        }
        return ch;
    }
};
#endif

// Keep the harness unattended: suppress OS crash dialogs always, and under the debug CRT route its
// asserts + std::cerr into the log instead of blocking boxes that the dispatcher cannot dismiss.
void installReportCapture()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
    for (int t : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT }) {
        _CrtSetReportMode(t, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
    }
    _CrtSetReportHook(crtReportHook);
    static CerrToLog* s_cerrSink = new CerrToLog;
    std::cerr.rdbuf(s_cerrSink);
    spdlog::info("[testdrv] CRT report capture installed (asserts -> log + fail fast; stderr -> log; no dialogs)");
#endif
}

} // namespace

game::CPhaseGame* livePhaseGame()
{
    auto* midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data || !midgard->data->client)
        return nullptr;
    auto* client = midgard->data->client;
    if (!client->data || !client->data->phase)
        return nullptr;
    auto* phaseGame = reinterpret_cast<game::CPhaseGame*>(
        reinterpret_cast<char*>(client->data->phase) - offsetof(game::CPhaseGame, phase));
    return phaseGame->data && phaseGame->data->midClient == client ? phaseGame : nullptr;
}

bool mapLoaded()
{
    const auto uiThread = nettracehooks::mainThreadId();
    if (!uiThread || GetCurrentThreadId() != uiThread)
        return false;
    auto* midgard = game::CMidgardApi::get().instance();
    auto* client = midgard && midgard->data ? midgard->data->client : nullptr;
    // Initial paired-scenario admission only: stock sets scenarioStarted after
    // constructing/assigning CPhaseGame. This is not a reconnect-generation token.
    if (!client || !client->data || !client->data->scenarioStarted)
        return false;
    auto* core = client->core.data;
    return core && core->scenarioInitialized && core->dataCache && livePhaseGame();
}

bool strategicActionReady()
{
    auto* phaseGame = livePhaseGame();
    auto* data = phaseGame ? phaseGame->data : nullptr;
    const auto& api = game::CPhaseGameApi::get();
    return data && data->midObjectLock && api.clientTakesTurn
           && api.clientTakesTurn(phaseGame) && !api.checkObjectLock(phaseGame);
}

bool preflight()
{
    if (g_planParsed)
        return g_preflightComplete;

    g_planParsed = true;
    g_plan = parseHarnessPlan();
    if (!local_coordinator_adapter::preflight())
        return false;
    if (local_coordinator_adapter::requested()) {
        g_plan.requested = true;
        g_plan.needsUiFrame = true;
        g_plan.needsNet = true;
    }

    if (!fixtureplan::preflight())
        return false;
    const bool wantFixtureApply = testenv::on("D2TESTDRV_APPLY_FIXTURE");
    const auto* fixture = fixtureplan::get();
    if (wantFixtureApply && (!fixture || fixture->operationCount == 0)) {
        spdlog::error("[testdrv] D2TESTDRV_APPLY_FIXTURE requires nonempty fixture operations");
        return false;
    }
    g_plan.requested = g_plan.requested || fixtureplan::hasPlan() || wantFixtureApply;
    if ((g_plan.wantExactLegacyMoves || g_plan.wantCleanLongMoves)
        && !fixtureplan::hasPlan()) {
        spdlog::error("[testdrv] exact movement plans require D2TESTDRV_FIXTURE_PLAN");
        return false;
    }
    if (g_plan.wantLegacyHostLoopback
        && (!g_plan.wantExactLegacyMoves || !fixtureplan::hasPlan())) {
        spdlog::error(
            "[testdrv] diagnostic LEGACY_HOST_LOOPBACK requires EXACT_LEGACY_MOVES "
            "and D2TESTDRV_FIXTURE_PLAN");
        return false;
    }

    if (!g_plan.legacyStacksGateValid || !g_plan.moveModeGatesValid)
        return false;

    if (!bootfixes::preflight(g_plan.skipIntro, g_plan.blackScreen))
        return false;
    if (!g_plan.requested) {
        g_preflightComplete = true;
        return true;
    }
    if (g_plan.wantScriptedPopups && g_plan.wantAutoDismiss) {
        spdlog::error(
            "[testdrv] D2TESTDRV_SCRIPTED_POPUPS is mutually exclusive with "
            "D2TESTDRV_AUTODISMISS");
        return false;
    }
    if (g_plan.wantScriptedPopups && !g_plan.wantRelay) {
        spdlog::error(
            "[testdrv] D2TESTDRV_SCRIPTED_POPUPS requires RELAY_BRIDGE for paired startup admission");
        return false;
    }
    if (g_plan.wantScriptedPopupConfirmations && !g_plan.wantScriptedPopups) {
        spdlog::error(
            "[testdrv] D2TESTDRV_SCRIPTED_POPUPS_CONFIRMATIONS requires "
            "D2TESTDRV_SCRIPTED_POPUPS");
        return false;
    }
    if (g_plan.wantAutoBattlePrearm && !g_plan.wantRelay) {
        spdlog::error(
            "[testdrv] D2TESTDRV_AUTO_BATTLE_PREARM requires RELAY_BRIDGE in the "
            "immutable canonical harness plan");
        return false;
    }
    if (g_plan.wantTurnEvents && !g_plan.wantRelay) {
        spdlog::error(
            "[testdrv] D2TESTDRV_TURN_EVENTS requires RELAY_BRIDGE as its exact sink");
        return false;
    }
    if (g_plan.wantExactLegacyMoves && g_plan.wantCleanLongMoves) {
        spdlog::error(
            "[testdrv] EXACT_LEGACY_MOVES and CLEAN_LONG_MOVES are mutually exclusive");
        return false;
    }
    if ((g_plan.wantExactLegacyMoves || g_plan.wantCleanLongMoves)
        && !g_plan.wantWorld) {
        spdlog::error(
            "[testdrv] explicit move command mode requires D2TESTDRV_WORLD");
        return false;
    }
    if (g_plan.wantLegacyStacks && !g_plan.wantRelay) {
        spdlog::error(
            "[testdrv] D2TESTDRV_LEGACY_STACKS requires RELAY_BRIDGE as its exact sink");
        return false;
    }
    if (g_plan.needsUiFrame && !g_plan.wantUi) {
        spdlog::error(
            "[testdrv] relay/self-nav/auto-dismiss/auto-battle-prearm/scripted-popups requires "
            "D2TESTDRV_UI_REPORTER");
        return false;
    }
    if (g_plan.wantWorld && (!g_plan.wantUi || !g_plan.needsUiFrame)) {
        spdlog::error(
            "[testdrv] D2TESTDRV_WORLD requires UI_REPORTER and a natural-frame consumer");
        return false;
    }
    if (!testenv::supportedGameBuild()) {
        spdlog::error("[testdrv] requested harness requires the exact Russobit image");
        return false;
    }

    if (!legacystackreporter::preflight(g_plan.wantLegacyStacks))
        return false;

    if (g_plan.wantUi && !uistatereporter::preflight())
        return false;
    if (g_plan.needsNet && !nettracehooks::preflight(g_plan.wantNet))
        return false;
    if (!worldactions::preflightHostMoveRoute(g_plan.wantLegacyHostLoopback)) {
        spdlog::error("[testdrv] diagnostic legacy host loopback preflight failed");
        return false;
    }
    if (g_plan.wantRelay
        && !bridge::preflightTurnEvents(g_plan.wantTurnEvents))
        return false;
    if (g_plan.needsUiFrame && !uiframedispatcher::request()) {
        spdlog::error("[testdrv] exact natural UI-frame seam preflight failed");
        return false;
    }
    if (g_plan.wantUi
        && !autonav::preflight(g_plan.wantSelfNav, g_plan.wantRelay,
                               g_plan.wantAutoDismiss,
                               g_plan.wantAutoBattlePrearm,
                               g_plan.wantScriptedPopups,
                               g_plan.wantScriptedPopupConfirmations))
        return false;

    g_preflightComplete = true;
    spdlog::info(
        "[testdrv] immutable plan preflight passed (relay={}, ui={}, world={}, legacy-stacks={}, net={}, "
        "telemetry={}, exact-legacy-moves={}, clean-long-moves={}, legacy-host-loopback={}, selfnav={}, "
        "autodismiss={}, auto-battle-prearm={}, scripted-popups={}, "
        "popup-confirmations={}, "
        "skip-intro={}, black-screen={})",
        g_plan.wantRelay, g_plan.wantUi, g_plan.wantWorld,
        g_plan.wantLegacyStacks, g_plan.wantNet,
        g_plan.wantTurnEvents, g_plan.wantExactLegacyMoves,
        g_plan.wantCleanLongMoves, g_plan.wantLegacyHostLoopback, g_plan.wantSelfNav,
        g_plan.wantAutoDismiss, g_plan.wantAutoBattlePrearm,
        g_plan.wantScriptedPopups, g_plan.wantScriptedPopupConfirmations,
        g_plan.skipIntro, g_plan.blackScreen);
    return true;
}

bool install(HMODULE self)
{
    if (g_commitComplete)
        return true;
    if (!g_planParsed || !g_preflightComplete)
        failFastInstall("commit without successful preflight", 0xD2E77321u);
    if (!g_plan.requested) {
        g_commitComplete = true;
        return true;
    }
    if (!testenv::pinHarnessModule())
        failFastInstall("process-lifetime module pin", 0xD2E77322u);

    if (g_plan.wantRelay)
        installReportCapture();

    bootfixes::commit();

    // The 178 direct CALL operands are the proven C4-compatible UI observation
    // seam. The canonical assignFunctor entry remains owned by C4/timerhost.
    if (g_plan.wantUi && !uistatereporter::commit())
        failFastInstall("UI reporter CALL-site bundle", 0xD2E77323u);
    if (g_plan.wantWorld && !worldreporter::install())
        failFastInstall("world reporter activation", 0xD2E77324u);

    if (g_plan.needsNet && !nettracehooks::commit(g_plan.wantNet))
        failFastInstall("network/session hook bundle", 0xD2E77325u);
    if (!worldactions::commitHostMoveRoute(
            g_plan.wantLegacyHostLoopback,
            g_plan.wantExactLegacyMoves,
            g_plan.wantCleanLongMoves))
        failFastInstall("diagnostic legacy host loopback", 0xD2E77327u);
    if (g_plan.wantRelay && !bridge::commitTurnEvents())
        failFastInstall("turn-event telemetry observer bundle", 0xD2E77326u);
    if (!legacystackreporter::commit())
        failFastInstall("legacy CMidStack::Stream reporter", 0xD2E77328u);

    if (g_plan.wantUi)
        autonav::activateAfterHooks();

    g_selfModule = self;
    g_wantRelay = g_plan.wantRelay;
    if (g_wantRelay) {
        g_runtimeArmed.store(true, std::memory_order_release);
        spdlog::info("[testdrv] relay helper armed; waiting for the first typed UI callback");
    }
    g_commitComplete = true;
    spdlog::info("[testdrv] immutable plan committed after ordinary hooks");
    return true;
}

void startRuntimeFromUi(game::CDialogInterf* dialog)
{
    if (dialog)
        startRuntimeOnce("CButtonInterf::assignFunctor");
}

} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
