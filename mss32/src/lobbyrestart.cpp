/*
 * This file is part of the modding toolset for Disciples 2.
 * https://github.com/bartonsun/D2ModdingToolset
 * Licensed under GNU GPL version 3 or later; see COPYING.
 */

#include "lobbyrestart.h"
#include "dialoginterf.h"
#include "interfaceutils.h"
#include "lobbysaveexchange.h"
#include "mempool.h"
#include "menucustomlobby.h"
#include "menucustomrandomscenariomulti.h"
#include "menunewskirmishmulti.h"
#include "menuphase.h"
#include "menurestartnative.h"
#include "midclient.h"
#include "midgard.h"
#include "midserver.h"
#include "mqnetplayerclient.h"
#include "netcustomplayerserver.h"
#include "netcustomservice.h"
#include "netcustomsession.h"
#include "popupdialoginterf.h"
#include "textboxinterf.h"
#include "uimanager.h"
#include "utils.h"
#include <BitStream.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <spdlog/spdlog.h>

namespace hooks {
namespace {

using Operation = LobbyProtocol::RestartOperation;
using Clock = std::chrono::steady_clock;

enum class Stage
{
    Idle,
    Prepare,
    Clearing,
    Draining,
    Waiting,
    BeginGeneration,
    Generating,
    CreateHost,
    JoinHost,
    SettingUp,
    Launching,
    Failed,
    Returning,
};

struct Restart
{
    Stage stage{Stage::Idle};
    std::uint64_t token{};
    bool host{};
    int maxPlayers{};
    SLNet::RakNetGUID hostGuid;
    std::string roomName;
    std::map<SLNet::RakNetGUID, SLNet::RakString> clients;
    game::CPopupDialogInterf* wait{};
    Clock::time_point deadline{};
    bool querySent{};
    bool setupReadySent{};
    bool launchAllowed{};
    bool loadedSent{};
    bool refreshSeen{};
};

Restart restart;
// A native server worker can still send while the UI is clearing the previous world.
std::atomic_bool blockGameMessages{};
std::atomic_bool replaySetup{};
std::atomic_bool restartActive{};
std::mutex setupMutex;
std::vector<unsigned char> selectedRace;
std::vector<unsigned char> selectedLord;
const std::array<const char*, 3> roomOptionMessages{
    ".?AVCMenusDiffLevelMsg@@", ".?AVCMenusStartCashMsg@@", ".?AVCMenusMaxPlayerMsg@@"};
std::array<std::vector<unsigned char>, 3> selectedRoomOptions;
bool lordRequestSent{};
bool setupMatches{};

bool isMessage(const game::NetMessageHeader* message, const char* name)
{
    return std::strncmp(message->messageClassName, name, sizeof(message->messageClassName)) == 0;
}

std::int32_t readInt(const unsigned char* data)
{
    std::int32_t value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool send(Operation operation)
{
    auto service = CNetCustomService::get();
    if (!service || !service->loggedIn()) {
        return false;
    }
    SLNet::BitStream stream;
    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_RESTART));
    stream.Write(static_cast<std::uint8_t>(operation));
    stream.Write(restart.token);
    return service->send(stream, service->getLobbyGuid(), HIGH_PRIORITY);
}

void closeWait()
{
    auto wait = restart.wait;
    restart.wait = nullptr;
    if (wait) {
        hideInterface(wait);
        wait->vftable->destructor(wait, 1);
    }
}

std::string gameText(const wchar_t* text)
{
    const int length = WideCharToMultiByte(1251, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(length > 0 ? length : 1, '\0');
    WideCharToMultiByte(1251, 0, text, -1, result.data(), static_cast<int>(result.size()),
                         nullptr, nullptr);
    result.pop_back();
    return result;
}

void showWait(const wchar_t* text)
{
    using namespace game;
    closeWait();
    restart.wait = static_cast<CPopupDialogInterf*>(Memory::get().allocate(sizeof(CPopupDialogInterf)));
    CPopupDialogInterfApi::get().constructor(restart.wait, "DLG_WAIT_GENERATION", nullptr);
    auto dialog = *restart.wait->dialog;
    const auto& dialogApi = CDialogInterfApi::get();
    dialogApi.hideControl(dialog, "BTN_CANCEL");
    if (auto info = dialogApi.findTextBox(dialog, "TXT_INFO")) {
        CTextBoxInterfApi::get().setString(info, gameText(text).c_str());
    }
    showInterface(restart.wait);
}

void fail(const char* reason, bool notifyServer = true)
{
    if (restart.stage == Stage::Returning) {
        return;
    }
    spdlog::warn("Lobby restart {} failed: {}", restart.token, reason);
    if (notifyServer) {
        send(Operation::Abort);
    }
    if (restart.stage == Stage::Generating) {
        auto midgard = game::CMidgardApi::get().instance();
        auto phase = midgard && midgard->data ? midgard->data->menuPhase : nullptr;
        if (phase && phase->data->currentMenu) {
            reinterpret_cast<CMenuRandomScenario*>(phase->data->currentMenu)->cancelGeneration = true;
        }
    }
    blockGameMessages = true;
    restart.stage = Stage::Failed;
}

bool postStartMenu()
{
    auto midgard = game::CMidgardApi::get().instance();
    auto data = midgard ? midgard->data : nullptr;
    return data && data->uiManager.data && data->startMenuMessageId
           && game::CUIManagerApi::get().postMessage(data->uiManager.data,
                                                     data->startMenuMessageId, 0, 0);
}

void showRestartMenu(game::CMenuPhase* phase, game::CMenuPhaseApi::Api::CreateMenuCallback factory)
{
    auto data = phase->data;
    auto callback = &factory;
    game::CMenuPhaseApi::get().showMenu(phase, &data->currentPhase, &data->interfManager,
                                       &data->currentMenu, &data->transitionAnimation,
                                       game::MenuPhase::RandomScenarioMulti, nullptr, &callback);
    // showMenu owns the object and its callbacks; only the progress/wait popup is visible.
    if (data->currentMenu) {
        hideInterface(data->currentMenu);
    }
}

void generationCompleted(CMenuRandomScenario*, RestartScenarioGenerationResult result)
{
    if (restart.stage != Stage::Generating) {
        return;
    }
    if (result == RestartScenarioGenerationResult::Success) {
        restart.deadline = Clock::now() + std::chrono::seconds(180);
        restart.stage = Stage::CreateHost;
    } else {
        fail("generation failed or was canceled");
    }
}

void joinCompleted(bool success)
{
    if (restart.stage != Stage::JoinHost) {
        return;
    }
    if (!success) {
        fail("native join handshake failed");
        return;
    }
    restart.stage = Stage::SettingUp;
    auto phase = game::CMidgardApi::get().instance()->data->menuPhase;
    if (phase && phase->data->currentMenu) {
        hideInterface(phase->data->currentMenu);
    }
}

bool captureSession()
{
    auto service = CNetCustomService::get();
    auto session = service ? service->getSession() : nullptr;
    auto midgard = game::CMidgardApi::get().instance();
    auto data = midgard ? midgard->data : nullptr;
    if (!session || !data || !data->client || !data->client->data->scenarioStarted
        || hasActiveLobbyHostSaveTransfer()) {
        return false;
    }
    {
        std::lock_guard lock(setupMutex);
        if (selectedRace.empty() || selectedLord.empty()) {
            return false;
        }
        lordRequestSent = setupMatches = false;
    }
    restart.host = session->isHost();
    restart.hostGuid = session->getServerGuid();
    restart.roomName = session->getName();
    restart.maxPlayers = session->getMaxPlayers();
    if (restart.host) {
        if (!hasRestartScenario() || !data->server || !data->server->data->netPlayerServer) {
            return false;
        }
        auto server = reinterpret_cast<CNetCustomPlayerServer*>(data->server->data->netPlayerServer);
        restart.clients = server->getRemoteClients();
    }
    return true;
}

bool restoreRoomOptions(game::CMidgard* midgard)
{
    auto pair = midgard->data->netPlayerClientPtr;
    auto client = pair ? pair->first.data : nullptr;
    if (!client) {
        return false;
    }
    std::array<std::vector<unsigned char>, 3> options;
    {
        std::lock_guard lock(setupMutex);
        options = selectedRoomOptions;
    }
    for (auto& option : options) {
        if (!option.empty()
            && !client->vftable->sendMessage(reinterpret_cast<game::IMqNetPlayer*>(client),
                                               game::serverNetPlayerId,
                                               reinterpret_cast<game::NetMessageHeader*>(option.data()))) {
            return false;
        }
    }
    return true;
}

} // namespace

void handleLobbyRestart(Operation operation, std::uint64_t token)
{
    if (operation == Operation::Prepare && restart.stage == Stage::Idle) {
        restart.token = token;
        restart.stage = Stage::Prepare;
        restartActive = true;
        restart.deadline = Clock::now() + std::chrono::seconds(180);
        blockGameMessages = true;
        return;
    }
    if (restart.stage == Stage::Idle || restart.token != token) {
        return;
    }
    if (operation == Operation::Abort) {
        fail("lobby canceled restart", false);
    } else if (operation == Operation::Start && restart.stage == Stage::Waiting) {
        // The host can inspect, copy and retry the preview without a decision timeout.
        restart.deadline = Clock::time_point::max();
        if (restart.host) {
            restart.stage = Stage::BeginGeneration;
        }
    } else if (operation == Operation::Join && restart.stage == Stage::Waiting && !restart.host) {
        restart.deadline = Clock::now() + std::chrono::seconds(180);
        restart.stage = Stage::JoinHost;
    } else if (operation == Operation::Launch && restart.host
               && restart.stage == Stage::SettingUp) {
        restart.launchAllowed = true;
    } else if (operation == Operation::Complete && restart.stage == Stage::Launching
               && restart.loadedSent) {
        spdlog::info("Lobby restart {} completed: all participants loaded", restart.token);
        closeWait();
        restart = {};
        restartActive = false;
        replaySetup = false;
        blockGameMessages = false;
    }
}

bool isLobbyRestartActive()
{
    return restartActive.load();
}

bool lobbyRestartBlocksGameMessages()
{
    return blockGameMessages.load();
}

bool retainLobbyRoomForRestart()
{
    return restart.stage == Stage::Clearing;
}

bool isLobbyRestartMenuTransition()
{
    return restart.stage == Stage::Clearing;
}

void enterLobbyRestartMenu(game::CMenuPhase* phase)
{
    phase->data->networkGame = true;
    phase->data->host = restart.host;
    phase->data->maxPlayers = restart.maxPlayers;
    phase->data->midgard->data->host = restart.host;
    restart.stage = Stage::Draining;
    replaySetup = true;
    showWait(restart.host ? L"\u0420\u0435\u0441\u0442\u0430\u0440\u0442: \u043e\u0436\u0438\u0434\u0430\u0435\u043c \u0433\u043e\u0442\u043e\u0432\u043d\u043e\u0441\u0442\u0438 \u0443\u0447\u0430\u0441\u0442\u043d\u0438\u043a\u043e\u0432..."
                          : L"\u0425\u043e\u0441\u0442 \u043f\u0435\u0440\u0435\u0433\u0435\u043d\u0435\u0440\u0438\u0440\u0443\u0435\u0442 \u043a\u0430\u0440\u0442\u0443.\n\u041e\u0436\u0438\u0434\u0430\u0439\u0442\u0435, \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u0441\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u043e.");
    // Native clear joins the old server worker. A self-packet queued after it is the fence
    // for any old loopback messages still inside RakNet, beyond the native receive queues.
    auto service = CNetCustomService::get();
    SLNet::BitStream fence;
    fence.Write(static_cast<SLNet::MessageID>(ID_LOBBY_RESTART));
    fence.Write(static_cast<std::uint8_t>(Operation::HostReady));
    fence.Write(restart.token);
    if (!service || !service->send(fence, service->getPeerGuid(), HIGH_PRIORITY)) {
        fail("cannot fence old loopback messages");
    }
}

void handleLobbyRestartLoopbackFence(std::uint64_t token)
{
    if (restart.stage != Stage::Draining || restart.token != token) {
        return;
    }
    restart.stage = Stage::Waiting;
    if (!send(restart.host ? Operation::HostReady : Operation::ClientReady)) {
        fail("cannot send readiness");
    }
}

bool processLobbyRestart()
{
    if (!isLobbyRestartActive()) {
        return false;
    }
    if (restart.stage == Stage::Returning) {
        return true;
    }
    auto service = CNetCustomService::get();
    if (restart.stage != Stage::Failed
        && (!service || !service->loggedIn() || Clock::now() >= restart.deadline)) {
        fail("connection lost or restart deadline expired");
    }
    if (restart.stage == Stage::Failed) {
        // The old world may already be destroyed on another participant: do not resume half a match.
        // Return through native teardown, which now performs the real LeaveRoom operation.
        closeWait();
        restart.stage = Stage::Returning;
        if (postStartMenu()) {
            spdlog::info("Lobby restart {}: returning to lobby after failure", restart.token);
        } else {
            restart.stage = Stage::Failed;
        }
        return true;
    }
    if (restart.stage == Stage::Prepare) {
        if (!captureSession()) {
            fail("no active generated match/setup snapshot, or a save is in progress");
            return true;
        }
        restart.stage = Stage::Clearing;
        if (!postStartMenu()) {
            fail("cannot post native teardown");
        }
        return true;
    }

    auto midgard = game::CMidgardApi::get().instance();
    auto phase = midgard && midgard->data ? midgard->data->menuPhase : nullptr;
    if (restart.stage == Stage::BeginGeneration && phase) {
        closeWait();
        if (!prepareRestartScenarioGeneration(generationCompleted)) {
            fail("generator snapshot unavailable");
            return true;
        }
        restart.stage = Stage::Generating;
        showRestartMenu(phase, createRestartScenarioMenu);
    } else if (restart.stage == Stage::CreateHost && phase) {
        auto menu = reinterpret_cast<game::CMenuBase*>(phase->data->currentMenu);
        auto dialog = game::CMenuBaseApi::get().getDialogInterface(menu);
        setEditBoxText(dialog, "EDIT_GAME", restart.roomName.c_str(), false);
        phase->data->maxPlayers = restart.maxPlayers;
        blockGameMessages = false;
        if (!game::CMenuNewSkirmishMultiApi::get().createServer(menu)) {
            fail("native server creation failed");
            return true;
        }
        // Publish the new receive endpoint before adding remote players can make the
        // native worker send setup packets. Both paths use the same ordered lobby channel.
        if (!send(Operation::HostCreated)) {
            fail("cannot notify lobby about new native server");
            return true;
        }
        auto server = reinterpret_cast<CNetCustomPlayerServer*>(midgard->data->server->data->netPlayerServer);
        for (const auto& client : restart.clients) {
            server->addClient(client.first, client.second);
        }
        phase->data->currentPhase = game::MenuPhase::NewSkirmish2LobbyHost;
        game::CMenuPhaseApi::get().switchToLobbyHostJoin(phase);
        hideInterface(phase->data->currentMenu);
        if (!restoreRoomOptions(midgard)) {
            fail("cannot restore native room options");
            return true;
        }
        restart.stage = Stage::SettingUp;
        showWait(L"\u041a\u0430\u0440\u0442\u0430 \u0433\u043e\u0442\u043e\u0432\u0430.\n\u0412\u043e\u0441\u0441\u0442\u0430\u043d\u0430\u0432\u043b\u0438\u0432\u0430\u0435\u043c \u0443\u0447\u0430\u0441\u0442\u043d\u0438\u043a\u043e\u0432 \u0438 \u0438\u0445 \u0432\u044b\u0431\u043e\u0440...");
    } else if (restart.stage == Stage::JoinHost && phase && !service->getSession()) {
        showRestartMenu(phase, createRestartJoinMenu);
        blockGameMessages = false;
        if (!beginRestartJoin(reinterpret_cast<CMenuCustomLobby*>(phase->data->currentMenu),
                               restart.hostGuid, restart.roomName.c_str(), restart.maxPlayers,
                               joinCompleted)) {
            fail("cannot start native join handshake");
        }
    } else if (restart.stage == Stage::SettingUp && phase) {
        bool lordSent{}, matches{};
        {
            std::lock_guard lock(setupMutex);
            lordSent = lordRequestSent;
            matches = setupMatches;
        }
        if (lordSent && !restart.querySent) {
            restart.querySent = true;
            if (!requestRestartSetupInfo()) {
                fail("cannot query restored native setup");
            }
        }
        if (matches && !restart.setupReadySent) {
            restart.setupReadySent = send(Operation::SetupReady);
        }
        if (restart.setupReadySent && (!restart.host || restart.launchAllowed)
            && pressRestartNativeStartButton(phase)) {
            restart.stage = Stage::Launching;
            closeWait();
        }
    } else if (restart.stage == Stage::Launching && midgard && midgard->data->client
               && midgard->data->client->data->scenarioStarted && !restart.loadedSent) {
        restart.loadedSent = send(Operation::Loaded);
        if (restart.loadedSent) {
            spdlog::info("Lobby restart {}: local world loaded ({})", restart.token,
                         restart.host ? "host" : "joiner");
            showWait(L"\u041a\u0430\u0440\u0442\u0430 \u0437\u0430\u0433\u0440\u0443\u0436\u0435\u043d\u0430.\n\u041e\u0436\u0438\u0434\u0430\u0435\u043c \u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0438 \u0432\u0441\u0435\u0445 \u0443\u0447\u0430\u0441\u0442\u043d\u0438\u043a\u043e\u0432...");
        }
    }
    return true;
}

void finishLobbyRestartMenuReturn()
{
    // During generation there is no native session whose destructor could send LeaveRoom.
    if (restart.stage == Stage::Returning) {
        if (auto service = CNetCustomService::get()) {
            service->leaveRoom();
        } else {
            resetLobbyRestart();
        }
    }
}

void resetLobbyRestart()
{
    closeWait();
    restart = {};
    restartActive = false;
    blockGameMessages = false;
    replaySetup = false;
    {
        std::lock_guard lock(setupMutex);
        selectedRace.clear();
        selectedLord.clear();
        for (auto& option : selectedRoomOptions) {
            option.clear();
        }
        lordRequestSent = setupMatches = false;
    }
    clearRestartScenario();
}

void prepareLobbyRestartSetupMessage(const game::NetMessageHeader* message,
                                     std::vector<unsigned char>& replacement)
{
    if (!message || message->length < sizeof(*message) || message->length >= game::netMessageMaxLength
        || blockGameMessages) {
        return;
    }
    for (std::size_t i = 0; i < roomOptionMessages.size(); ++i) {
        if (!isMessage(message, roomOptionMessages[i]) || message->length < sizeof(*message) + 4) {
            continue;
        }
        std::lock_guard lock(setupMutex);
        if (replaySetup) {
            replacement = selectedRoomOptions[i];
        } else {
            auto bytes = reinterpret_cast<const unsigned char*>(message);
            selectedRoomOptions[i].assign(bytes, bytes + message->length);
        }
        return;
    }
    const bool race = isMessage(message, ".?AVCMenusReqRaceMsg@@");
    const bool lord = isMessage(message, ".?AVCMenusReqLordMsg@@");
    if ((!race && !lord) || message->length < sizeof(*message) + (race ? 4u : 8u)) {
        return;
    }
    std::lock_guard lock(setupMutex);
    auto& snapshot = race ? selectedRace : selectedLord;
    if (replaySetup) {
        replacement = snapshot;
        if (lord) {
            lordRequestSent = true;
        }
    } else {
        auto bytes = reinterpret_cast<const unsigned char*>(message);
        snapshot.assign(bytes, bytes + message->length);
    }
}

void observeLobbyRestartSetupInfo(const game::NetMessageHeader* message)
{
    if (!replaySetup || !restart.querySent || !message || message->length < sizeof(*message) + 12
        || !isMessage(message, ".?AVCMenusAnsStartInfoMsg@@")) {
        return;
    }
    std::lock_guard lock(setupMutex);
    if (!lordRequestSent || selectedRace.empty() || selectedLord.empty()) {
        return;
    }
    auto info = reinterpret_cast<const unsigned char*>(message) + sizeof(*message);
    auto race = selectedRace.data() + sizeof(*message);
    auto lord = selectedLord.data() + sizeof(*message);
    // Native AnsStartInfo serializes face, race category, lord category, in that order.
    setupMatches = readInt(info) == readInt(lord + 4) && readInt(info + 4) == readInt(race)
                   && readInt(info + 8) == readInt(lord);
}

bool allowLobbyRestartClientMessage(const game::NetMessageHeader* message)
{
    if (!isLobbyRestartActive()) {
        return true;
    }
    const bool refresh = isMessage(message, ".?AVCRefreshInfo@@");
    const bool newScenario = isMessage(message, ".?AVCNewScenarioMsg@@");
    const bool startScenario = isMessage(message, ".?AVCStartScenarioMsg@@");
    if (!refresh && !newScenario && !startScenario) {
        return true;
    }
    auto midgard = game::CMidgardApi::get().instance();
    auto client = midgard && midgard->data ? midgard->data->client : nullptr;
    auto cache = client && client->core.data ? client->core.data->dataCache : nullptr;
    if (newScenario || startScenario || !restart.refreshSeen || !cache) {
        spdlog::info("Lobby restart {} pid {}: dispatch {} stage {} cache {:p}",
                     restart.token, GetCurrentProcessId(), message->messageClassName,
                     static_cast<int>(restart.stage), static_cast<void*>(cache));
    }
    if (refresh) {
        restart.refreshSeen = true;
        // Russobit CRefreshInfo applies objects directly to dataCache (0x41799e).
        // CNewScenarioMsg must have created it first; dropping a refresh would lose map state.
        if (!cache) {
            fail("received CRefreshInfo without a native map; aborting instead of losing objects");
            return false;
        }
    }
    return true;
}

} // namespace hooks
