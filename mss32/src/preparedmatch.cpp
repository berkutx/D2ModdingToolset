#include "preparedmatch.h"
#include "preparedmatchlifecycle.h"
#include "preparedmatchtemplates.h"
#include "preparedmatchtext.h"
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
#include "simturns/lobby_transport.h"
#include "simturns/lobby_wire.h"
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
    std::string localFilename;
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
struct ActiveJoin {
    JoinOffer offer;
    JoinStage stage{JoinStage::Waiting};
    JoinState state{JoinState::Received};
    std::uint64_t roomsRevision{};
    bool canceled{};
    std::vector<std::string> pages;
    std::size_t page{};
    Clock::time_point refreshDeadline{};
};
std::optional<ActiveJoin> activeJoin;
std::deque<JoinOffer> waitingJoins;
JoinReceipts joinReceipts; // Terminal answers survive logout/login within this process.
game::CMidgardMsgBox* joinBox{};
std::uint64_t joinPromptEpoch{};

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
void sendJoinStatus(const JoinIdentity& target, JoinState state, const std::string& detail = {}) {
    auto* service = CNetCustomService::get();
    if (!service || !service->loggedIn()) return;
    auto bytes = encodeJoinStatus(target, state, detail);
    if (bytes.empty()) return;
    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_PREPARED_MATCH));
    stream.WriteAlignedBytes(bytes.data(), static_cast<unsigned>(bytes.size()));
    service->send(stream, service->getLobbyGuid(), HIGH_PRIORITY);
}
void finishJoin(JoinState state, const std::string& detail = {}) {
    if (!activeJoin) return;
    activeJoin->state = state; activeJoin->stage = JoinStage::Terminal;
    joinReceipts.remember(activeJoin->offer.target, activeJoin->offer.recipient, state);
    sendJoinStatus(activeJoin->offer.target, state, detail);
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
    auto selected = selectLocalTemplate(offer, getScenarioTemplates(), gameText(offer.title),
                                        gameText(offer.filename));
    rsg::RandomGenerator random; random.setSeed(static_cast<std::size_t>(std::time(nullptr)));
    selected.settings.replaceRandomRaces(random);
    active->localFilename = std::move(selected.filename);
    active->recipe = {std::move(selected.settings), selected.cached->source};
}
std::vector<std::string> paginateText(game::CTextBoxInterf* textBox, std::string text,
                                    const std::string& question, const std::string& next) {
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
    auto fits = [&](const std::string& body) {
        const auto candidate = format + body;
        return metrics.ptr.data->vftable->getTextHeight(metrics.ptr.data, candidate.c_str(), width) <= height;
    };
    return promptPages(std::move(text), gameText(question), gameText(next), fits);
}
void buildPages(game::CTextBoxInterf* textBox) {
    active->pages = paginateText(textBox, hostPromptText(active->offer),
        "Сгенерировать карту?", "Да — далее, Нет — отложить.");
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

struct JoinConfirmationHandler : game::CMidMsgBoxButtonHandler {
    std::uint64_t epoch, promptEpoch;
};
void __fastcall destroyJoinHandler(JoinConfirmationHandler* p, int, char flags) {
    if (flags & 1) game::Memory::get().freeNonZero(p);
}
void __fastcall answerJoin(JoinConfirmationHandler* handler, int, game::CMidgardMsgBox* box, bool yes) {
    const auto expected = handler->epoch, expectedPrompt = handler->promptEpoch;
    if (joinBox == box) joinBox = nullptr;
    if (box) { hideInterface(box); box->vftable->destructor(box, 1); }
    if (expected != epoch || expectedPrompt != joinPromptEpoch || !activeJoin
        || activeJoin->stage != JoinStage::Prompt || activeJoin->canceled) return;
    if (!yes) { finishJoin(JoinState::Declined, "player-declined"); return; }
    if (++activeJoin->page == activeJoin->pages.size()) activeJoin->stage = JoinStage::Accepted;
}
game::CMidMsgBoxButtonHandlerVftable joinHandlerVftable{
    reinterpret_cast<game::CMidMsgBoxButtonHandlerVftable::Destructor>(destroyJoinHandler),
    reinterpret_cast<game::CMidMsgBoxButtonHandlerVftable::Handler>(answerJoin)};

void receiveJoinOffer(JoinOffer offer) {
    auto* service = CNetCustomService::get();
    try {
        if (!service || !service->loggedIn() || gameText(offer.recipient) != service->getUserName()
            || gameText(offer.host) == service->getUserName()) return;
    } catch (...) { return; }
    if (const auto old = joinReceipts.find(offer.target, offer.recipient)) {
        sendJoinStatus(offer.target, *old); return;
    }
    if (activeJoin && activeJoin->offer.target == offer.target) {
        sendJoinStatus(offer.target, activeJoin->state); return;
    }
    if (std::any_of(waitingJoins.begin(), waitingJoins.end(), [&](const JoinOffer& v) { return v.target == offer.target; })) return;
    if (waitingJoins.size() >= 4) {
        joinReceipts.remember(offer.target, offer.recipient, JoinState::Unavailable);
        sendJoinStatus(offer.target, JoinState::Unavailable, "join-queue-full"); return;
    }
    sendJoinStatus(offer.target, idleLobby() ? JoinState::Received : JoinState::Busy);
    waitingJoins.push_back(std::move(offer));
}
void cancelJoinOffer(const JoinIdentity& target) {
    for (auto it = waitingJoins.begin(); it != waitingJoins.end();) {
        if (!(it->target == target)) { ++it; continue; }
        joinReceipts.remember(it->target, it->recipient, JoinState::Unavailable);
        sendJoinStatus(it->target, JoinState::Unavailable, "join-withdrawn");
        it = waitingJoins.erase(it);
    }
    if (activeJoin && activeJoin->offer.target == target && activeJoin->stage != JoinStage::Terminal)
        activeJoin->canceled = true; // Destroy only our own modal at the post-callback safe point.
}
bool processJoinCancellation() {
    if (!activeJoin || !activeJoin->canceled) return false;
    ++joinPromptEpoch;
    auto* box = joinBox; joinBox = nullptr;
    if (box) { hideInterface(box); box->vftable->destructor(box, 1); }
    finishJoin(JoinState::Unavailable, "join-withdrawn"); activeJoin.reset();
    return box != nullptr;
}
bool processPreparedJoin() {
    if (joinBox) return true;
    if (activeJoin && activeJoin->stage == JoinStage::Terminal) activeJoin.reset();
    if (!activeJoin && !waitingJoins.empty()) {
        activeJoin.emplace(); activeJoin->offer = std::move(waitingJoins.front()); waitingJoins.pop_front();
    }
    if (!activeJoin) return false;
    if (!idleLobby()) {
        // A game or an unrelated dialog can last indefinitely. Refresh again on
        // return; neither its elapsed time nor a replaced lobby menu expires the invite.
        activeJoin->stage = joinStageAfterBusy(activeJoin->stage);
        return false;
    }
    auto* menu = reinterpret_cast<CMenuCustomLobby*>(phase()->data->currentMenu);
    auto& join = *activeJoin;
    try {
        const auto hostName = gameText(join.offer.host);
        const auto action = joinAction(join.stage, true, menu->preparedRoomsRevision() > join.roomsRevision,
                                       menu->hasPreparedJoinRoom(join.offer.target.roomId, hostName));
        if (action == JoinAction::RefreshRooms) {
            join.roomsRevision = menu->preparedRoomsRevision();
            join.refreshDeadline = Clock::now() + std::chrono::seconds(15);
            join.stage = join.stage == JoinStage::Accepted ? JoinStage::CheckingJoinRoom : JoinStage::CheckingRoom;
            CNetCustomService::get()->searchRooms(); return false;
        }
        if (action == JoinAction::Wait
            && (join.stage == JoinStage::CheckingRoom || join.stage == JoinStage::CheckingJoinRoom)
            && Clock::now() > join.refreshDeadline) {
            // No room-search response is not evidence that the room disappeared.
            // Keep consent/queue state and retry; a fresh result or JoinCancel settles it.
            join.stage = joinStageAfterBusy(join.stage); return false;
        }
        if (action == JoinAction::Unavailable) {
            finishJoin(JoinState::Unavailable, "join-room-unavailable"); return false;
        }
        if (action == JoinAction::Join) {
            // No race/lord/portrait overrides: ordinary native joining owns every choice.
            const auto started = menu->joinPreparedRoom(join.offer.target.roomId, hostName);
            finishJoin(started ? JoinState::Accepted : JoinState::Unavailable,
                       started ? "join-requested" : "join-validation-failed");
            return true;
        }
        if (action != JoinAction::ShowPrompt) return false;
        join.stage = JoinStage::Prompt; join.state = JoinState::Prompt;
        auto* handler = static_cast<JoinConfirmationHandler*>(game::Memory::get().allocate(sizeof(JoinConfirmationHandler)));
        handler->vftable = &joinHandlerVftable; handler->epoch = epoch; handler->promptEpoch = ++joinPromptEpoch;
        auto* box = static_cast<game::CMidgardMsgBox*>(game::Memory::get().allocate(sizeof(game::CMidgardMsgBox)));
        game::CMidgardMsgBoxApi::get().constructor(box, "", true, handler, nullptr, nullptr);
        try {
            auto* dialog = box->data->dialogInterf;
            if (!game::CDialogInterfApi::get().findControl(dialog, "TXT_INFO")) throw std::runtime_error("message-box-text-missing");
            auto* textBox = game::CDialogInterfApi::get().findTextBox(dialog, "TXT_INFO");
            if (!textBox || !textBox->data) throw std::runtime_error("message-box-text-missing");
            if (join.pages.empty()) join.pages = paginateText(textBox,
                join.offer.title + "\nХост: " + join.offer.host + "\nКарта готова.",
                "Войти в комнату?", "Да — далее, Нет — отказаться.");
            const auto footer = promptFooter(join.page, join.pages.size(),
                "Войти в комнату?", "Да — далее, Нет — отказаться.");
            const auto text = join.pages[join.page] + gameText(footer);
            game::CTextBoxInterfApi::get().setString(textBox, text.c_str());
            joinBox = box; showInterface(box); sendJoinStatus(join.offer.target, JoinState::Prompt);
        } catch (...) {
            if (joinBox == box) { joinBox = nullptr; hideInterface(box); }
            box->vftable->destructor(box, 1);
            finishJoin(JoinState::Unavailable, "join-confirmation-layout");
            showMessageBox(gameText("Не удалось показать приглашение. Откройте подготовку матча на сайте."));
        }
        return true;
    } catch (...) {
        finishJoin(JoinState::Unavailable, "join-client-error");
        showMessageBox(gameText("Не удалось присоединиться к подготовленному матчу. Проверьте файлы игры и комнаты в лобби."));
        return true;
    }
}
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
void resetPreparedMatch() {
    ++epoch; ++joinPromptEpoch; active.reset(); waiting.clear(); seen.clear(); modal = false;
    // Native menu teardown owns its interfaces. Late handlers cannot affect a new login.
    activeJoin.reset(); waitingJoins.clear(); joinBox = nullptr;
}
void receivePreparedMatch(const unsigned char* bytes, std::size_t size) {
    JoinOffer joinOffer;
    if (decodeJoinOffer(bytes, size, joinOffer)) { receiveJoinOffer(std::move(joinOffer)); return; }
    JoinIdentity joinCancel;
    if (decodeJoinCancel(bytes, size, joinCancel)) { cancelJoinOffer(joinCancel); return; }
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
    if (processJoinCancellation()) return true;
    if (joinBox) return true;
    if (modal) return true;
    if (active && active->stage == Stage::Terminal && idleLobby()) {
        seen.emplace_back(active->offer.identity, active->status); if (seen.size() > 32) seen.pop_front(); active.reset();
    }
    if (!active && !waiting.empty()) { active.emplace(); active->offer = std::move(waiting.front()); waiting.pop_front(); }
    if (!active) return processPreparedJoin();
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
            const auto footer = promptFooter(active->page, active->pages.size(),
                "Сгенерировать карту?", "Да — далее, Нет — отложить.");
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
            if (active->offer.simultaneous && (!simturns::lobbySupported()
                || active->offer.participants.size() != 2
                || !simturns::lobby::validMergeDay(active->offer.simultaneousUntil)))
                throw std::runtime_error("unsupported-simultaneous-turns");
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
            if (!startPreparedMatchScenarioGeneration(menu, active->recipe, active->localFilename))
                preparedMatchGenerationEnded(RestartScenarioGenerationResult::Error);
            return true;
        } catch (const std::exception& e) {
            const std::string reason = e.what();
            std::string message = "Не удалось подготовить матч: " + reason;
            if (reason == "missing-template") message = "Шаблон: " + active->offer.filename
                + "\nне найден шаблон для матча, перезапустите клиент игры после добавления."
                  " Добавьте подходящий локальный шаблон.";
            else if (reason == "ambiguous-template") message = "Найдено несколько подходящих локальных шаблонов одной версии: "
                + active->offer.filename + ". Оставьте один нужный вариант и перезапустите клиент игры.";
            else if (reason == "template-player-count" || reason == "unknown-template-spin"
                     || reason.compare(0, 10, "parameter-") == 0)
                message = "Выбранная локальная версия шаблона не поддерживает параметры матча: " + reason
                    + ". Проверьте условия на странице подготовки.";
            else if (reason == "unsupported-native-client") message = "Автоподготовка матча пока поддерживает только проверенный клиент Russobit.";
            else if (reason == "unsupported-simultaneous-turns") message = "Одновременные ходы требуют сборку MSS с поддержкой ОХ, двух игроков и день объединения 0 или 2–30.";
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
    return processPreparedJoin();
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
