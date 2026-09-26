// The runner extracts the real hookAssignFunctor/preflight/commit functions. These stubs isolate
// their routing/ownership contract, not native memory patching, dialog readiness or actual C4.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

namespace game {
struct CDialogInterf {};
struct CButtonInterf {};
struct SmartPointer {};
namespace CButtonInterfApi {
using AssignFunctor = CButtonInterf*(__stdcall*)(CDialogInterf*, const char*, const char*,
                                                SmartPointer*, int);
struct Api { AssignFunctor assignFunctor; };
Api buttonApi{};
Api& get() { return buttonApi; }
}
}

namespace spdlog {
template<class... Args> void error(const char*, Args...) {}
template<class... Args> void info(const char*, Args...) {}
}

namespace hooks { namespace testdrv {
std::vector<int> events;
void startRuntimeFromUi(game::CDialogInterf*) { events.push_back(2); }
namespace uistatereporter {
using FnAssignFunctor = game::CButtonInterfApi::AssignFunctor;
bool supported = true;
bool patchAllowed = true;
bool patchIntact = true;
bool returnButton = true;
bool ready = true;
int installCalls = 0;
game::CButtonInterf button;
game::CButtonInterf* __stdcall canonical(game::CDialogInterf*, const char*, const char*,
                                         game::SmartPointer*, int)
{
    events.push_back(3);
    return returnButton ? &button : nullptr;
}
const uintptr_t kAssignFunctorVA = reinterpret_cast<uintptr_t>(&canonical);
constexpr std::size_t kBindCallSiteCount = 178;
FnAssignFunctor g_origAssignFunctor = nullptr;
bool g_bindCallSitesInstalled = false;
std::uint32_t g_dialogInstance = 41;
std::uint32_t g_curOwnerInstance = 73;
struct BindCallPatch {};
namespace testenv { bool supportedGameBuild() { return supported; } }
bool prepareBindCallPatches(BindCallPatch (&)[kBindCallSiteCount]) { return patchAllowed; }
bool allBindCallSitesPointToHook() { return patchIntact; }
bool installBindCallPatches()
{
    ++installCalls;
    if (!patchAllowed || (g_bindCallSitesInstalled && !patchIntact)) return false;
    g_bindCallSitesInstalled = true;
    return true;
}
void beginBind(game::CDialogInterf*, const char*) { ready = false; events.push_back(1); }
void recordBind(game::CDialogInterf*, const char*, const char*)
{
    CHECK(!ready);
    CHECK(events.back() == 3);
    events.push_back(4);
}
namespace autonav {
void onDialogBound(const char*, const char*, std::uint32_t appearance, std::uint32_t owner,
                   game::CButtonInterf* exactButton)
{
    CHECK(!ready);
    CHECK(appearance == 41 && owner == 73 && exactButton == &button);
    events.push_back(5);
}
}

#include "testdrv_ui_bind_seam_production.inc"

void reset()
{
    events.clear();
    supported = patchAllowed = patchIntact = returnButton = ready = true;
    installCalls = 0;
    g_origAssignFunctor = nullptr;
    g_bindCallSitesInstalled = false;
    game::CButtonInterfApi::get().assignFunctor = &canonical;
}

void tests()
{
    game::CDialogInterf dialog;
    game::SmartPointer functor;
    auto& api = game::CButtonInterfApi::get();
    reset();
    api.assignFunctor(&dialog, "BTN_OK", "DLG_LOGIN_ACCOUNT", &functor, 0);
    CHECK(events == std::vector<int>{3}); // Runtime-disabled table is untouched.
    events.clear();
    CHECK(preflight());
    CHECK(api.assignFunctor == &canonical); // Preflight performs no API write.
    CHECK(commit());
    CHECK(api.assignFunctor == &hookAssignFunctor && g_origAssignFunctor == &canonical);
    CHECK(api.assignFunctor(&dialog, "BTN_OK", "DLG_LOGIN_ACCOUNT", &functor, 0) == &button);
    CHECK((events == std::vector<int>{1, 2, 3, 4, 5}));
    CHECK(!ready); // No reentrant action/readiness publication from bind.
    events.clear();
    hookAssignFunctor(&dialog, "BTN_BACK", "DLG_MAIN_MENU", &functor, 0);
    CHECK((events == std::vector<int>{1, 2, 3, 4, 5})); // Native and MSS same path, once.
    events.clear();
    returnButton = false;
    CHECK(api.assignFunctor(&dialog, "MISSING", "DLG_LOGIN_ACCOUNT", &functor, 0) == nullptr);
    CHECK((events == std::vector<int>{1, 2, 3})); // Missing button never records success.
    CHECK(preflight() && commit()); // Idempotence requires both still-owned seams.
    CHECK(g_origAssignFunctor == &canonical);
    api.assignFunctor = &canonical;
    const int priorCalls = installCalls;
    CHECK(!preflight() && !commit()); // Stolen/reset API table is not silently repaired.
    CHECK(installCalls == priorCalls);

    reset();
    supported = false;
    CHECK(!preflight() && !commit() && installCalls == 0);
    CHECK(api.assignFunctor == &canonical);
    reset();
    CHECK(preflight());
    patchAllowed = false;
    CHECK(!commit());
    CHECK(api.assignFunctor == &canonical && !g_bindCallSitesInstalled);
    reset();
    api.assignFunctor = &hookAssignFunctor;
    CHECK(!preflight() && !commit() && installCalls == 0);
    reset();
    CHECK(preflight() && commit());
    patchIntact = false;
    CHECK(!preflight() && !commit());
}
}}}

int main()
{
    try {
        hooks::testdrv::uistatereporter::tests();
        std::cout << "UI bind seam: PASS (actual functions, native/UI dependencies stubbed)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UI bind seam: FAIL: " << error.what() << '\n';
        return 1;
    }
}
