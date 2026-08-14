/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Installer / entry point. See testdrv/testdrv.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#include "testdrv/testdrv.h"
#include "testdrv/autonav.h"
#include "testdrv/bootfixes.h"
#include "testdrv/directplaysessionhooks.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/worldreporter.h"
#include "midclient.h"
#include "midgard.h"
#include "phasegame.h"
#include "version.h"
#include <cstddef>
#include <spdlog/spdlog.h>
#include <crtdbg.h>
#include <iostream>
#include <streambuf>
#include <string>

namespace hooks {
namespace testdrv {

namespace {

HMODULE g_self{};
bool g_wantRelay{};
bool g_runtimeStarted{};
bool g_runtimeStarting{};

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
    // std::cerr may flush during CRT teardown, after function-local static destruction. Keep the
    // streambuf alive for the process lifetime instead of leaving cerr with a dangling rdbuf.
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

bool strategicActionReady()
{
    auto* phaseGame = livePhaseGame();
    auto* data = phaseGame ? phaseGame->data : nullptr;
    return data && data->clientTakesTurn && data->midObjectLock
           && !game::CPhaseGameApi::get().checkObjectLock(phaseGame);
}

void installEarly()
{
    bootfixes::installEarly();
}

bool install(HMODULE self)
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return true;
    spdlog::info("[testdrv] install");

    // Only a dispatcher-driven run (the relay bridge) has no human to click a native CRT/OS box, so
    // an assert there hangs the run: route reports to the log and fail fast. A plain manual DebugTest
    // launch (no bridge) is LEFT with the default visible assert dialog so the tester still sees it.
    g_self = self;
    g_wantRelay = testenv::on("D2TESTDRV_RELAY_BRIDGE");
    const bool wantSelfNav = testenv::on("D2TESTDRV_SELFNAV");
    if (g_wantRelay)
        installReportCapture();

    // UI-state reporter (D2TESTDRV_UI_REPORTER), the foundation: exposes the live dialog
    // + buttons and arms the auto-nav executor (tiny exit smoke or dispatcher-driven).
    const bool uiInstalled = uistatereporter::install();
    worldreporter::install(); // world-state snapshot (D2TESTDRV_WORLD): players' resources + map stacks
    // Do not start threads or install the frame hook under DllMain's loader lock.
    if ((g_wantRelay || wantSelfNav) && !uiInstalled) {
        spdlog::critical("[testdrv] UI runtime requested but the UI reporter could not be installed");
        return false;
    }
    return true;
}

void startRuntimeFromUi()
{
    if (g_runtimeStarted || g_runtimeStarting)
        return;
    g_runtimeStarting = true;

    bool ok = true;
    if (g_wantRelay) {
        char role[16]{};
        GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
        const bool join = lstrcmpiA(role, "join") == 0 || lstrcmpiA(role, "joiner") == 0;
        if (join)
            ok = directplaysessionhooks::install();
    }
    ok = autonav::onUiReady() && ok;
    if (g_wantRelay)
        ok = bridge::start(g_self) && ok;
    if (!ok) {
        spdlog::critical("[testdrv] UI runtime initialization failed; terminating test process");
        spdlog::default_logger()->flush();
        TerminateProcess(GetCurrentProcess(), 0xD2E77304u);
        std::abort();
    }
    g_runtimeStarted = true;
    g_runtimeStarting = false;
    spdlog::info("[testdrv] UI runtime started (relay={})", g_wantRelay);
}

} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
