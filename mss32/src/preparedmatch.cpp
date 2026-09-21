#include "preparedmatch.h"
#include "preparedmatchlifecycle.h"
#include "preparedmatchsettings.h"
#include "button.h"
#include "categorylist.h"
#include "dialoginterf.h"
#include "dynamiccast.h"
#include "editboxinterf.h"
#include "globaldata.h"
#include "formattedtext.h"
#include "interfaceutils.h"
#include "interfmanager.h"
#include "menucustomlobby.h"
#include "menucustomrandomscenariomulti.h"
#include "menuphase.h"
#include "menurestartnative.h"
#include "midgard.h"
#include "midgardmsgbox.h"
#include "mempool.h"
#include "netcustomservice.h"
#include "netmessages.h"
#include "racecategory.h"
#include "racelist.h"
#include "randomgenerator.h"
#include "scenariotemplates.h"
#include "textboxinterf.h"
#include "utils.h"
#include <BitStream.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>
#include <windows.h>

namespace hooks {
namespace {
using namespace prepared;
using Clock = std::chrono::steady_clock;
struct Active {
    Offer offer;
    Stage stage{Stage::Waiting};
    State status{State::Received};
    ScenarioTemplateRecipe recipe;
    std::vector<std::string> pages;
    std::size_t page{};
    bool canceled{}, setupReceived{}, querySent{};
    int observedRace{-2}, observedLord{-2}, lordClicks{};
    Clock::time_point setupDeadline{}, nextQuery{};
};
std::optional<Active> active;
std::deque<Offer> waiting;
std::deque<std::pair<Identity, State>> seen;
std::uint64_t epoch{};
bool modal{};

std::string gameText(const std::string& input) {
    if (input.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                     static_cast<int>(input.size()), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8 in prepared match");
    std::wstring wide(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), wide.data(), n);
    BOOL replaced{};
    const int length = WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, wide.data(), n, nullptr, 0, nullptr, &replaced);
    if (!length || replaced) throw std::runtime_error("Prepared text cannot be displayed by this game client");
    std::string result(length, 0);
    WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, wide.data(), n, result.data(), length, nullptr, &replaced);
    if (replaced) throw std::runtime_error("Prepared text cannot be displayed by this game client");
    return result;
}
void sendStatus(const Identity& identity, State state, const std::string& detail = {}) {
    auto* service = CNetCustomService::get();
    if (!service || !service->loggedIn()) return;
    auto bytes = encodeStatus(identity, state, detail);
    if (bytes.empty()) return;
    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_PREPARED_MATCH));
    stream.WriteAlignedBytes(bytes.data(), static_cast<unsigned>(bytes.size()));
    service->send(stream, service->getLobbyGuid(), HIGH_PRIORITY);
}
void status(State value, const std::string& detail = {}) {
    if (!active) return;
    active->status = value; sendStatus(active->offer.identity, value, detail);
}
game::CMenuPhase* phase() {
    auto* midgard = game::CMidgardApi::get().instance();
    return midgard && midgard->data ? midgard->data->menuPhase : nullptr;
}
bool topIsMenu(game::CMenuPhase* p) {
    if (!p || !p->data || !p->data->currentMenu || !p->data->interfManager.data) return false;
    auto* manager = reinterpret_cast<game::CInterfManager*>(p->data->interfManager.data);
    return manager->vftable->isInputAllowed(manager)
        && manager->vftable->getTopmostInterface(manager) == p->data->currentMenu;
}
bool idleLobby() {
    auto* service = CNetCustomService::get(); auto* p = phase();
    return service && service->loggedIn() && !service->getSession() && p && p->data
        && p->data->currentPhase == game::MenuPhase::CustomLobby && topIsMenu(p)
        && reinterpret_cast<CMenuCustomLobby*>(p->data->currentMenu)->isPreparedMatchIdle();
}
void terminal(State state, const std::string& detail, const std::string& message = {}) {
    if (!active) return;
    status(state, detail); active->stage = Stage::Terminal;
    if (!message.empty()) showMessageBox(gameText(message));
}
const Participant& host() {
    const auto& v = active->offer;
    return *std::find_if(v.participants.begin(), v.participants.end(), [&](const Participant& p) { return p.name == v.host; });
}
void makeRecipe() {
    const auto& offer = active->offer;
    const ScenarioTemplate* cached = nullptr;
    for (const auto& item : getScenarioTemplates()) {
        if (std::filesystem::path(item.filename).filename().string() == gameText(offer.filename)) {
            cached = &item; break;
        }
    }
    if (!cached) throw std::runtime_error("missing-template");
    if (_stricmp(cached->md5.c_str(), offer.md5.c_str()) != 0) throw std::runtime_error("template-hash");
    auto settings = applySettings(offer, cached->settings);
    rsg::RandomGenerator random; random.setSeed(static_cast<std::size_t>(std::time(nullptr)));
    settings.replaceRandomRaces(random);
    active->recipe = {std::move(settings), cached->source};
}
std::string parameterLabel(const std::string& key) {
    static const std::pair<const char*, const char*> names[] = {
        {"size", "Размер карты"}, {"roads", "Дороги, %"}, {"forest", "Лес, %"},
        {"startingGold", "Стартовое золото"}, {"startingNativeMana", "Стартовая родная мана"},
        {"water", "Вода, %"}, {"maxUnit", "Макс. уровень юнитов"},
        {"maxSpell", "Макс. круг заклинаний"}, {"maxLeader", "Макс. уровень героя"},
        {"maxCity", "Макс. уровень города"}, {"startingLevel", "Стартовый уровень"},
        {"iterations", "Итерации генерации"}
    };
    for (const auto& name : names) if (key == name.first) return name.second;
    if (key.compare(0, 5, "spin:") == 0)
        return "Параметр шаблона " + std::to_string(std::stoul(key.substr(5)) + 1);
    return key;
}
void buildPages(game::CTextBoxInterf* textBox) {
    const auto& v = active->offer;
    std::string text = "Подготовленный матч: " + v.title + "\nШаблон: " + v.filename + "\nХост: " + v.host + "\nИгроки: ";
    for (std::size_t i = 0; i < v.participants.size(); ++i) text += (i ? ", " : "") + v.participants[i].name;
    text += v.ranked ? "\nРейтинговая игра." : "\nНерейтинговая игра.";
    for (const auto& key : v.explicitParameters)
        text += "\n" + parameterLabel(key) + ": " + std::to_string(v.parameters.at(key));
    if (v.unlockGui) text += "\nUnlock GUI включён.";
    if (v.simultaneous) text += "\nОдновременные ходы: " + std::to_string(v.simultaneousUntil);
    if (!v.summary.empty()) text += "\n" + v.summary;
    text = gameText(text);
    const auto* rect = textBox->vftable->getArea(textBox);
    const auto width = rect->right - rect->left;
    const auto height = rect->bottom - rect->top;
    const std::string format = textBox->data->format.string ? textBox->data->format.string : "";
    struct TextMetrics {
        game::FormattedTextPtr ptr{};
        TextMetrics() { game::IFormattedTextApi::get().getFormattedText(&ptr); }
        ~TextMetrics() { game::SmartPointerApi::get().createOrFree(reinterpret_cast<game::SmartPointer*>(&ptr), nullptr); }
    } metrics;
    if (!metrics.ptr.data || width <= 0 || height <= 0) throw std::runtime_error("message-box-size");
    const auto reserve = gameText("\n(9999/9999) Создать матч? Да — генерация, Нет — отложить.");
    auto fits = [&](const std::string& body) {
        const auto candidate = format + body + reserve;
        return metrics.ptr.data->vftable->getTextHeight(metrics.ptr.data, candidate.c_str(), width) <= height;
    };
    // Measure with the game's renderer and the actual mod's textbox. Pages retain
    // every byte; the reserved footer is longer than any displayed page counter.
    while (!text.empty()) {
        std::size_t n{}, upper = std::min<std::size_t>(text.size(), 1024);
        while (n < upper) {
            const auto middle = n + (upper - n + 1) / 2;
            if (fits(text.substr(0, middle))) n = middle;
            else upper = middle - 1;
        }
        if (!n) throw std::runtime_error("message-box-too-small");
        if (n < text.size()) {
            const auto split = text.find_last_of(" \n", n - 1);
            if (split != std::string::npos && split > n / 2) n = split + 1;
        }
        active->pages.push_back(text.substr(0, n)); text.erase(0, n);
    }
}
struct ConfirmationHandler : game::CMidMsgBoxButtonHandler { std::uint64_t epoch; };
void __fastcall destroyHandler(ConfirmationHandler* p, int, char flags) { if (flags & 1) game::Memory::get().freeNonZero(p); }
void __fastcall answer(ConfirmationHandler* handler, int, game::CMidgardMsgBox* box, bool yes) {
    const auto expected = handler->epoch; // The box owns and destroys its handler.
    if (box) { hideInterface(box); box->vftable->destructor(box, 1); }
    if (expected != epoch) return;
    modal = false;
    if (!active || active->stage != Stage::Confirming || active->canceled) return;
    if (!yes) { terminal(State::Deferred, "host-deferred"); return; }
    if (++active->page == active->pages.size()) active->stage = Stage::Accepted;
}
game::CMidMsgBoxButtonHandlerVftable handlerVftable{
    reinterpret_cast<game::CMidMsgBoxButtonHandlerVftable::Destructor>(destroyHandler),
    reinterpret_cast<game::CMidMsgBoxButtonHandlerVftable::Handler>(answer)};
game::CMenuBase* __stdcall createPreparedMenu(game::CMenuPhase* p) {
    auto* memory = game::Memory::get().allocate(sizeof(CMenuCustomRandomScenarioMulti));
    auto* menu = new (memory) CMenuCustomRandomScenarioMulti(p);
    setEditBoxText(game::CMenuBaseApi::get().getDialogInterface(menu), "EDIT_GAME", "Prepared match", false);
    menu->preparedMatchGeneration = true;
    return menu;
}
game::CMenuBase* __stdcall createLobby(game::CMenuPhase* p) {
    return new (game::Memory::get().allocate(sizeof(CMenuCustomLobby))) CMenuCustomLobby(p);
}
void showMenu(game::CMenuPhase* p, game::MenuPhase target, game::CMenuPhaseApi::Api::CreateMenuCallback factory) {
    auto* d = p->data; auto* callback = &factory;
    game::CMenuPhaseApi::get().showMenu(p, &d->currentPhase, &d->interfManager, &d->currentMenu,
                                      &d->transitionAnimation, target, nullptr, &callback);
}
bool pressLord(game::CMenuPhase* p) {
    if (!topIsMenu(p) || p->data->currentPhase != game::MenuPhase::LobbyHost) return false;
    const auto* type = (*game::RttiApi::get().typeIdOperator)(p->data->currentMenu);
    if (!type || std::strcmp(type->name, ".?AVCMenuLobbyHost@@")) return false;
    auto* dialog = game::CMenuBaseApi::get().getDialogInterface(reinterpret_cast<game::CMenuBase*>(p->data->currentMenu));
    if (!dialog || !game::CDialogInterfApi::get().findControl(dialog, "BTN_LORD")) return false;
    auto* button = game::CDialogInterfApi::get().findButton(dialog, "BTN_LORD");
    if (!button || !button->vftable->isEnabled(button) || !button->buttonData || !button->buttonData->onClickedFunctor.data) return false;
    auto* action = button->buttonData->onClickedFunctor.data;
    action->vftable->runCallback(action); // Native ReqLord keeps the current, valid portrait.
    return true;
}
}
void resetPreparedMatch() { ++epoch; active.reset(); waiting.clear(); seen.clear(); modal = false; }
void receivePreparedMatch(const unsigned char* bytes, std::size_t size) {
    Offer offer;
    if (decodeOffer(bytes, size, offer)) {
        auto* service = CNetCustomService::get();
        try { if (!service || gameText(offer.host) != service->getUserName()) return; } catch (...) { return; }
        if (active && active->offer.identity == offer.identity) { sendStatus(offer.identity, active->status); return; }
        const auto prior = std::find_if(seen.begin(), seen.end(), [&](const auto& item) { return item.first == offer.identity; });
        if (prior != seen.end()) { sendStatus(offer.identity, prior->second); return; }
        if (std::any_of(waiting.begin(), waiting.end(), [&](const Offer& v) { return v.identity == offer.identity; })) return;
        if (waiting.size() >= 4) { sendStatus(offer.identity, State::Error, "prepared-queue-full"); return; }
        sendStatus(offer.identity, idleLobby() ? State::Received : State::Busy);
        waiting.push_back(std::move(offer)); return;
    }
    Identity cancel;
    if (!decodeCancel(bytes, size, cancel)) return;
    const auto before = waiting.size();
    waiting.erase(std::remove_if(waiting.begin(), waiting.end(), [&](const Offer& v) { return v.identity == cancel; }), waiting.end());
    if (waiting.size() != before) {
        seen.emplace_back(cancel, State::Canceled); if (seen.size() > 32) seen.pop_front();
        sendStatus(cancel, State::Canceled, "canceled-before-prompt");
    }
    if (!active || !(active->offer.identity == cancel)) {
        const auto prior = std::find_if(seen.begin(), seen.end(), [&](const auto& item) { return item.first == cancel; });
        if (prior != seen.end()) sendStatus(cancel, prior->second == State::RoomCreated ? State::RoomCreated : State::Canceled);
        return;
    }
    // The outgoing RoomsPlugin request and Status use RELIABLE_ORDERED channel 0.
    // Once CreateRoom was sent, wait for its actual result; do not falsely ACK
    // cancellation or unbind a room that already exists on the server.
    const auto action = requestCancellation(active->stage, active->status == State::RoomCreated,
                                             active->canceled);
    if (action == CancelAction::AwaitSafePoint) return;
    if (action == CancelAction::PreserveRoom) {
        status(State::RoomCreated); return;
    }
    status(State::Canceled, "canceled-before-create");
    active->stage = stageAfterCanceledAck(active->stage);
}
bool processPreparedMatch() {
    if (modal) return true;
    if (active && active->stage == Stage::Terminal && idleLobby()) {
        seen.emplace_back(active->offer.identity, active->status); if (seen.size() > 32) seen.pop_front(); active.reset();
    }
    if (!active && !waiting.empty()) { active.emplace(); active->offer = std::move(waiting.front()); waiting.pop_front(); }
    if (!active) return false;
    auto* p = phase();
    if (active->canceled && active->stage == Stage::Generating && p && p->data
        && p->data->currentPhase == game::MenuPhase::RandomScenarioMulti && p->data->currentMenu) {
        cancelPreparedMatchScenarioGeneration(reinterpret_cast<CMenuRandomScenario*>(p->data->currentMenu));
        // Cancellation is already a local creation barrier: neither preview Accept
        // nor the queued generator completion can issue CreateRoom now. Acknowledge
        // without waiting for the expensive worker to finish its current geometry.
        if (active->status != State::Canceled) status(State::Canceled, "generation-cancel-latched");
        return true;
    }
    if (active->stage == Stage::Waiting && idleLobby()) {
        active->stage = Stage::Confirming; status(State::Confirmation);
    }
    if (active->stage == Stage::Confirming && idleLobby()) {
        auto* handler = static_cast<ConfirmationHandler*>(game::Memory::get().allocate(sizeof(ConfirmationHandler)));
        handler->vftable = &handlerVftable; handler->epoch = epoch;
        auto* box = static_cast<game::CMidgardMsgBox*>(game::Memory::get().allocate(sizeof(game::CMidgardMsgBox)));
        game::CMidgardMsgBoxApi::get().constructor(box, "", true, handler, nullptr, nullptr);
        try {
            auto* dialog = box->data->dialogInterf;
            if (!game::CDialogInterfApi::get().findControl(dialog, "TXT_INFO")) throw std::runtime_error("message-box-text-missing");
            auto* textBox = game::CDialogInterfApi::get().findTextBox(dialog, "TXT_INFO");
            if (!textBox || !textBox->data) throw std::runtime_error("message-box-text-missing");
            if (active->pages.empty()) buildPages(textBox);
            const bool last = active->page + 1 == active->pages.size();
            const auto footer = "\n(" + std::to_string(active->page + 1) + "/" + std::to_string(active->pages.size()) + ") "
                + (last ? "Создать матч? Да — генерация, Нет — отложить." : "Да — далее, Нет — отложить.");
            const auto message = active->pages[active->page] + gameText(footer);
            game::CTextBoxInterfApi::get().setString(textBox, message.c_str());
            modal = true; showInterface(box);
        } catch (...) {
            box->vftable->destructor(box, 1);
            terminal(State::Error, "confirmation-layout", "Условия нельзя полностью показать в игре. Проверьте подготовку на сайте.");
        }
        return true;
    }
    if (active->stage == Stage::Accepted && idleLobby()) {
        try {
            if (!restartNativeSupported()) throw std::runtime_error("unsupported-native-client");
            makeRecipe();
            auto* service = CNetCustomService::get(); auto& options = service->getRoomOptions();
            options.ranked = active->offer.ranked; options.unlockGui = active->offer.unlockGui;
            options.simultaneousTurnsEnabled = active->offer.simultaneous;
            options.simultaneousTurnsDays = active->offer.simultaneousUntil;
            p->data->host = true;
            status(State::Accepted); active->stage = Stage::Generating; status(State::Generating);
            showMenu(p, game::MenuPhase::RandomScenarioMulti, createPreparedMenu);
            // showMenu installs its own interface after invoking the factory. Show
            // the wait/preview above that menu, never from inside its constructor.
            auto* menu = reinterpret_cast<CMenuRandomScenario*>(p->data->currentMenu);
            if (!startPreparedMatchScenarioGeneration(menu, active->recipe, gameText(active->offer.filename)))
                preparedMatchGenerationEnded(RestartScenarioGenerationResult::Error);
            return true;
        } catch (const std::exception& e) {
            const std::string reason = e.what();
            std::string message = "Не удалось подготовить матч: " + reason;
            if (reason == "missing-template") message = "Шаблон: " + active->offer.filename
                + "\nне найден шаблон для матча, перезапустите клиент игры после добавления."
                  " Скачайте нужную версию со страницы подготовки.";
            else if (reason == "template-hash") message = "Другая версия шаблона " + active->offer.filename
                + ". Скачайте нужную версию со страницы подготовки и перезапустите клиент игры.";
            else if (reason == "unsupported-native-client") message = "Автоподготовка матча пока поддерживает только проверенный клиент Russobit.";
            terminal(State::Error, reason.substr(0, 128), message); return true;
        }
    }
    if (active->stage == Stage::Returning && p && p->data->currentPhase == game::MenuPhase::RandomScenarioMulti && topIsMenu(p)) {
        active->stage = Stage::Terminal; showMenu(p, game::MenuPhase::CustomLobby, createLobby); return true;
    }
    if (active->stage == Stage::Setup) {
        if (active->canceled || host().lord < 0 || Clock::now() > active->setupDeadline) {
            active->stage = Stage::Terminal; return false;
        }
        if (!p || !p->data || p->data->currentPhase != game::MenuPhase::LobbyHost) return false;
        auto* service = CNetCustomService::get();
        if (!service || !service->getNativeGameMessageTracker()->empty()) return false;
        if (active->setupReceived && topIsMenu(p)) {
            if (active->observedRace != static_cast<int>(active->recipe.settings.races.front())) {
                // The player changed their choice: do not fight manual input.
                active->stage = Stage::Terminal; return false;
            }
            if (active->observedLord == host().lord || active->lordClicks >= 3) { active->stage = Stage::Terminal; return false; }
            if (pressLord(p)) {
                active->setupReceived = false; ++active->lordClicks; active->querySent = false;
                active->nextQuery = Clock::now() + std::chrono::milliseconds(250);
            }
        }
        if (!active->querySent && Clock::now() >= active->nextQuery && topIsMenu(p)) active->querySent = requestRestartSetupInfo();
    }
    return false;
}
void preparedMatchGenerationEnded(RestartScenarioGenerationResult result) {
    if (!active || active->stage != Stage::Generating) return;
    status(active->canceled ? State::Canceled : result == RestartScenarioGenerationResult::Canceled ? State::Deferred : State::Error,
           result == RestartScenarioGenerationResult::Canceled ? "generation-canceled" : "generation-failed");
    active->stage = Stage::Returning;
}
bool canAcceptPreparedMatch(CMenuRandomScenario* menu) {
    if (!menu->preparedMatchGeneration) return true;
    if (!active || active->stage != Stage::Generating) return false;
    if (active->canceled) { preparedMatchGenerationEnded(RestartScenarioGenerationResult::Canceled); return false; }
    return true;
}
bool preparePreparedMatchRoom(CMenuRandomScenario* menu) {
    if (!menu->preparedMatchGeneration) return true;
    if (!active || active->stage != Stage::Generating) return false;
    if (active->canceled) { preparedMatchGenerationEnded(RestartScenarioGenerationResult::Canceled); return false; }
    // Fresh Russobit CMenuLobby takes the first menu-phase race and emits native ReqRace.
    // getContents may reorder races: move the resolved host race first only in native setup.
    auto& races = menu->scenarioTemplate.settings.races;
    const auto expected = active->recipe.settings.races.front();
    if (std::find(races.begin(), races.end(), expected) == races.end()) {
        preparedMatchGenerationEnded(RestartScenarioGenerationResult::Error); return false;
    }
    auto* list = &menu->menuBaseData->menuPhase->data->races;
    game::RaceCategoryListApi::get().freeNodes(list);
    auto append = [&](rsg::RaceType race) {
        game::LRaceCategory category{}; const int id = static_cast<int>(race);
        const auto* global = *game::GlobalDataApi::get().getGlobalData();
        game::LRaceCategoryTableApi::get().findCategoryById(global->raceCategories, &category, &id);
        game::RaceCategoryListApi::get().add(list, &category);
    };
    append(expected); for (auto race : races) if (race != expected) append(race);
    active->stage = Stage::Creating; return true;
}
const Identity* preparedMatchRoomIdentity() { return active && active->stage == Stage::Creating && !active->canceled ? &active->offer.identity : nullptr; }
void preparedMatchRoomCreated(bool success) {
    if (!active || active->stage != Stage::Creating) return;
    if (!success) {
        status(State::Error, "room-creation-failed");
        active->stage = stageAfterRoomCreationResult(false, active->canceled); return;
    }
    status(State::RoomCreated);
    active->stage = stageAfterRoomCreationResult(true, active->canceled);
    active->setupDeadline = Clock::now() + std::chrono::seconds(15); active->nextQuery = Clock::now() + std::chrono::milliseconds(500);
}
void preparedMatchMenuDestroyed(CMenuRandomScenario* menu) {
    if (menu->preparedMatchGeneration && active && active->stage == Stage::Generating)
        terminal(State::Deferred, "generator-left");
}
void observePreparedMatchSetup(const game::NetMessageHeader* message) {
    if (!active || active->stage != Stage::Setup || !active->querySent || !message
        || message->length < sizeof(*message) + 12
        || std::strcmp(message->messageClassName, ".?AVCMenusAnsStartInfoMsg@@")) return;
    const auto* data = reinterpret_cast<const unsigned char*>(message) + sizeof(*message);
    std::memcpy(&active->observedRace, data + 4, 4); std::memcpy(&active->observedLord, data + 8, 4);
    active->setupReceived = true;
}
} // namespace hooks
