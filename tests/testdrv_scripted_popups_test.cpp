// Actual subscriber code; platform/UI/SEH and common-ledger enqueue are stubs.
// No game process, native hooks, relay, credentials or process termination.
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include "../mss32/include/testdrv/battlelayout.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
#define __try try
#define __except(x) catch (...)
struct NativeTermination { unsigned code; };
int lstrcmpA(const char* a, const char* b) { return std::strcmp(a, b); }
void lstrcpynA(char* out, const char* text, int size) { std::strncpy(out, text, size - 1); out[size - 1] = 0; }
unsigned fixtureTick = 500;
unsigned GetTickCount() { return fixtureTick; }
unsigned long long GetTickCount64() { return fixtureTick; }
unsigned GetCurrentProcessId() { return 4242; }
void* GetCurrentProcess() { return nullptr; }
[[noreturn]] void TerminateProcess(void*, unsigned code) { throw NativeTermination{code}; }
namespace spdlog {
struct Logger { void flush() {} } logger;
Logger* default_logger() { return &logger; }
template<class... A> void info(const char*, A...) {}
template<class... A> void error(const char*, A...) {}
template<class... A> void critical(const char*, A...) {}
}
namespace fixture {
bool loaded = false, phase = false, ready = true;
std::string dialog, message;
std::uint32_t appearance = 7, owner = 77, age = 300;
std::vector<std::string> claims;
}
namespace game {
struct CDialogInterf {} dialog;
struct CallbackVft { void* runCallback; };
struct CBFunctorDispatch0 { CallbackVft* vftable; };
struct ButtonData { struct { CBFunctorDispatch0* data; } onClickedFunctor; };
struct CButtonInterf { ButtonData* buttonData; };
struct TextData { struct { const char* string; } text; } textData;
struct CTextBoxInterf { TextData* data; } textBox;
namespace CDialogInterfApi {
struct Api {
    CTextBoxInterf* findTextBox(CDialogInterf*, const char* name) {
        CHECK(std::strcmp(name, "TXT_INFO") == 0);
        textData.text.string = fixture::message.c_str(); textBox.data = &textData; return &textBox;
    }
} api;
Api& get() { return api; }
}
}
namespace hooks::testdrv {
bool mapLoaded() { return fixture::loaded; }
void* livePhaseGame() { return fixture::phase ? &game::dialog : nullptr; }
namespace uistatereporter {
const char* currentDialogName() { return fixture::dialog.c_str(); }
game::CDialogInterf* findDialog(const char*) { return &game::dialog; }
bool isReadyDialogInstance(const char* name, std::uint32_t appearance, std::uint32_t owner) {
    return fixture::ready && fixture::dialog == name && appearance == fixture::appearance && owner == fixture::owner;
}
bool getReadyDialogInstanceAge(const char* name, std::uint32_t appearance, std::uint32_t owner, std::uint32_t& age) {
    age = fixture::age; return isReadyDialogInstance(name, appearance, owner);
}
bool getReadyCurrentDialogInstanceAge(const char* name, std::uint32_t& appearance, std::uint32_t& owner, std::uint32_t& age) {
    appearance = fixture::appearance; owner = fixture::owner; age = fixture::age;
    return isReadyDialogInstance(name, appearance, owner);
}
bool isReadyBattleResultCloseInstance(std::uint32_t, std::uint32_t, game::CButtonInterf*, game::CBFunctorDispatch0*) { return true; }
}
namespace autonav {
void claimAndEnqueueScriptedPopupAction(const char* dialog, const char* button, std::uint32_t,
    std::uint32_t, std::uint32_t, game::CButtonInterf*, game::CBFunctorDispatch0*) {
    fixture::claims.emplace_back(std::string(dialog) + "::" + button);
}
}
}
#include "testdrv_scripted_popups_production.inc"
using namespace hooks::testdrv::scriptedpopups;

void reset(bool lobby = true, const char* role = "host") {
    g_prepared = g_enabled = g_confirmations = g_lobbyScope = g_active = false;
    g_battleActive = g_battleResultPublished = g_battleResultClaimed = g_postBattleCaptureOpen = false;
    g_startupAdmission.store(StartupAdmission::Disabled);
    g_releaseAppearance = g_releaseOwner = g_lastClaimedAppearance = g_lastClaimedOwner = 0;
    g_pending = PendingPopup{}; std::memset(g_role, 0, sizeof(g_role));
    fixture::loaded = fixture::phase = false; fixture::ready = true; fixture::age = 300;
    fixture::appearance = 7; fixture::owner = 77; fixture::claims.clear(); fixture::message.clear();
    fixture::dialog.clear(); fixtureTick = 500;
    CHECK(preflight(true, true, role, lobby)); activateAfterHooks();
}
void bind(const char* dialog, const char* button) {
    fixture::dialog = dialog;
    onDialogBound(dialog, button, fixture::appearance, fixture::owner, nullptr);
}
template<class F> void terminal(unsigned code, F action) {
    bool caught = false;
    try { action(); } catch (const NativeTermination& e) { CHECK(e.code == code); caught = true; }
    CHECK(caught); CHECK(fixture::claims.empty());
}
void hostRelease(unsigned pid = 4242) {
    const std::uint32_t words[]{pid, fixture::appearance, fixture::owner};
    receiveStartupRelease(reinterpret_cast<const std::uint8_t*>(words), sizeof(words));
}
int main() {
    try {
        const std::string dayOne = "\xCD\xE0\xF7\xE0\xEB\xEE \xE7\xE0\xE4\xE0\xED\xE8\xFF, \xE4\xE5\xED\xFC 1";
        reset(); CHECK(lobbyStartupPopups()); CHECK(preflight(true, true, "host", true));
        CHECK(!preflight(true, true, "host", false));
        bind("DLG_MESSAGE_BOX", "BTN_YES"); bind("DLG_MESSAGE_BOX", "BTN_NO");
        CHECK(!g_pending.valid); fixture::owner++; tick(); CHECK(fixture::claims.empty());
        reset(); bind("DLG_SCENARIO_BRIEFING", "BTN_CONTINUE"); tick();
        CHECK(fixture::claims.size() == 1 && startupActionsHeld());
        reset(); fixture::phase = true; bind("DLG_BEGIN_TURN", "BTN_OK"); tick();
        CHECK(g_pending.valid && fixture::claims.empty()); // construction-time capture
        fixture::loaded = true; hostRelease(); tick(); tick();
        CHECK(fixture::claims.size() == 1 && !startupActionsHeld());
        reset(true, "join"); fixture::loaded = true; fixture::message = dayOne;
        bind("DLG_MESSAGE_BOX", "BTN_OK"); fixture::age = 299; tick(); CHECK(fixture::claims.empty());
        fixture::age = 300; tick(); CHECK(fixture::claims.empty());
        receiveStartupRelease(nullptr, 0); tick(); tick(); CHECK(fixture::claims.size() == 1);
        for (const auto& text : {std::string("Disconnected"), std::string("Start of quest, day 1"),
                 dayOne + " error", dayOne.substr(0, dayOne.size() - 1), std::string(128, 'x')}) {
            reset(); fixture::loaded = true; fixture::message = text;
            bind("DLG_MESSAGE_BOX", "BTN_OK"); terminal(0xD2E77365u, [] { tick(); });
        }
        reset(); fixture::loaded = true; fixture::message = dayOne;
        bind("DLG_MESSAGE_BOX", "BTN_OK"); bind("DLG_MESSAGE_BOX", "BTN_YES");
        terminal(0xD2E77365u, [] { tick(); });
        reset(false); CHECK(!lobbyStartupPopups()); fixture::message = "legacy message";
        bind("DLG_MESSAGE_BOX", "BTN_OK"); CHECK(g_pending.valid);
        fixture::loaded = true; receiveStartupRelease(nullptr, 0); tick(); CHECK(fixture::claims.size() == 1);
        reset(false); terminal(0xD2E77362u, [] { hostRelease(); });
        reset(true, "join"); terminal(0xD2E77362u, [] { hostRelease(); });
        reset(); terminal(0xD2E77363u, [] { hostRelease(999); });
        reset(); fixture::loaded = true; bind("DLG_BEGIN_TURN", "BTN_OK"); hostRelease();
        fixture::owner++; terminal(0xD2E77364u, [] { tick(); });
        reset(); bind("DLG_SCENARIO_BRIEFING", "BTN_CONTINUE"); hostRelease();
        terminal(0xD2E77361u, [] { tick(); });
        reset(); hostRelease(); terminal(0xD2E77360u, [] { hostRelease(); });
        reset(); terminal(0xD2E77362u, [] { receiveStartupRelease(nullptr, 4); });
        reset(); fixture::loaded = true; bind("DLG_BEGIN_TURN", "BTN_OK"); hostRelease(); tick();
        fixture::claims.clear(); fixture::appearance++; fixture::owner++;
        bind("DLG_TURNSUMMARY", "BTN_CANCEL"); CHECK(!g_pending.valid);
        bind("DLG_TURNSUMMARY", "BTN_OK"); fixture::age = 299; tick(); CHECK(fixture::claims.empty());
        fixture::age = 300; tick(); tick();
        CHECK(fixture::claims.size() == 1 && fixture::claims.front() == "DLG_TURNSUMMARY::BTN_OK");
        reset(false); bind("DLG_TURNSUMMARY", "BTN_OK"); CHECK(!g_pending.valid);
        CHECK(!hooks::testdrv::isBattleDialog(nullptr));
        CHECK(!hooks::testdrv::isBattleDialog("DLG_BATTLE_C"));
        CHECK(!hooks::testdrv::isBattleDialog("dlg_battle_b"));
        for (const char* layout : {"DLG_BATTLE_A", "DLG_BATTLE_B"}) {
            reset(false); fixture::loaded = true; receiveStartupRelease(nullptr, 0); tick();
            bind(layout, "BTN_DEFEND"); tick();
            CHECK(g_battleActive && !g_pending.valid && fixture::claims.empty());
            game::CallbackVft vft{reinterpret_cast<void*>(1)};
            game::CBFunctorDispatch0 functor{&vft};
            game::ButtonData data{{&functor}};
            game::CButtonInterf button{&data};
            onDialogBound(layout, "BTN_CLOSE", fixture::appearance, fixture::owner, &button);
            CHECK(g_pending.battleResultClose && g_pending.exactButton == &button
                  && g_pending.exactFunctor == &functor);
            fixtureTick += 299; tick(); CHECK(fixture::claims.empty());
            fixtureTick++; tick(); tick();
            CHECK(fixture::claims.size() == 1
                  && fixture::claims.front() == std::string(layout) + "::BTN_CLOSE");
            reset(false); bind(layout, "BTN_DEFEND");
            terminal(0xD2E7734Bu, [&] { bind(layout, "BTN_CLOSE"); });
        }
        std::cout << "scripted popup actual state machine: PASS\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
      catch (const NativeTermination& e) { std::cerr << "unexpected native terminal " << e.code << '\n'; return 1; }
}
