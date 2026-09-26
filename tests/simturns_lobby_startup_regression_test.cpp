// Portable tests of the production lobby adapter, CoordinatorPort and core.
// Only the native/UI/service boundary is stubbed; no game, local coordinator,
// D2_TESTDRV, network or binary fixture is used.
#include "simturns/lobby_transport.h"
#include "simturns/lobby_wire.h"
#include "simturns/coordinator_port.h"
#include "simturns/controller.h"
#include "netcustomservice.h"
#include "netcustomsession.h"
#include "netintercept.h"
#include "netmsg.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace hooks;
using namespace hooks::simturns;

namespace {

struct UiTask
{
    netintercept::UiTaskCallback callback{};
    void* context{};
    netintercept::UiTaskDiscardCallback discard{};
};

struct StagedReceive
{
    void* context{};
    netintercept::NativeReceiveCallback complete{};
    netintercept::UiTaskDiscardCallback discard{};
    netintercept::NativeReceiveDiagnostic diagnostic;
};

std::deque<UiTask> uiTasks;
std::deque<StagedReceive> stagedReceives;
bool strategicIdle{};
std::uint64_t pregameGeneration{1};

void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

void drainOneUiTask()
{
    check(!uiTasks.empty(), "expected one queued UI completion");
    const auto task = uiTasks.front();
    uiTasks.pop_front();
    task.callback(task.context);
}

void drainAllUiTasks()
{
    while (!uiTasks.empty()) drainOneUiTask();
}

struct NativeFrame
{
    explicit NativeFrame(const char* messageClass, std::uint32_t length,
                         std::uint32_t word0 = 0, std::uint32_t word1 = 0,
                         std::uint32_t word2 = 0)
        : bytes(std::max<std::uint32_t>(length, sizeof(game::NetMessageHeader)))
    {
        auto* value = header();
        value->messageType = game::netMessageNormalType;
        value->length = length;
        const auto nameLength = std::min<std::size_t>(std::strlen(messageClass),
                                                       sizeof(value->messageClassName) - 1);
        std::memcpy(value->messageClassName, messageClass, nameLength);
        if (length == sizeof(game::NetMessageHeader) + 3 * sizeof(std::uint32_t)) {
            const std::uint32_t words[]{word0, word1, word2};
            std::memcpy(value + 1, words, sizeof(words));
        }
    }

    game::NetMessageHeader* header()
    {
        return reinterpret_cast<game::NetMessageHeader*>(bytes.data());
    }

    std::vector<std::uint8_t> bytes;
};

NativeFrame syntheticJoinGame62()
{
    NativeFrame result(".?AVCJoinGameMsg@@", 62);
    // The failing diagnostic deliberately copied no remote payload. Build only
    // the proven Russobit shape from the checked-in stock CJoin fixture: handle,
    // encoded NUL-terminated name length/name, then one lord-category id. The
    // values below are synthetic and are not claimed to be the failed packet.
    const std::uint32_t joinedHandle = 0xa3de0001;
    const std::uint32_t nameLength = 6;
    const char name[nameLength] = {'t', 'e', 's', 't', '1', '\0'};
    const std::uint32_t lordCategory = 2;
    std::memcpy(result.bytes.data() + 44, &joinedHandle, sizeof(joinedHandle));
    std::memcpy(result.bytes.data() + 48, &nameLength, sizeof(nameLength));
    std::memcpy(result.bytes.data() + 52, name, sizeof(name));
    std::memcpy(result.bytes.data() + 58, &lordCategory, sizeof(lordCategory));
    return result;
}

NativeFrame syntheticUpdateObj52()
{
    NativeFrame result(".?AVCCmdUpdateObjMsg@@", 52);
    // Reconstructed, not captured run004 payload: factory case 1 at 0x47C0BB,
    // constructor 0x47B902/vtable 0x6D4F34, serializer 0x47F663 uses only the
    // 44-byte CNetMsg header followed by CCommandMsg player ID and sequence.
    // Neither word is an object ID, and this test imposes no new value rule.
    const std::uint32_t words[]{0, 2};
    std::memcpy(result.bytes.data() + sizeof(game::NetMessageHeader), words, sizeof(words));
    return result;
}

netintercept::NativeReceiveDiagnostic diagnosticFor(const game::NetMessageHeader* buffer,
                                                     int handlerCount)
{
    netintercept::NativeReceiveDiagnostic result;
    result.captureHeader(buffer->messageType, buffer->length, buffer->messageClassName);
    result.sender = game::serverNetPlayerId;
    result.receiver = 0x6d85df0a;
    result.threadId = 15308;
    result.policy = netintercept::RxDecision::Pass;
    result.dispatchResult = handlerCount;
    result.dispatched = true;
    result.captureDPlaySelf = true;
    return result;
}

void stageNative(NativeFrame& frame, bool clientReceiver = true,
                 std::uint32_t sender = game::serverNetPlayerId)
{
    auto ticket = lobbyTrackNativePacket(clientReceiver);
    check(static_cast<bool>(ticket), "armed lobby did not issue a native ticket");
    bool delivered{};
    lobbyDeliverNativePacket(ticket, [&] { delivered = true; });
    check(delivered, "startup packet was unexpectedly held behind a control frame");
    check(lobbyStageNativeReceive(frame.header(), std::move(ticket), sender),
          "production lobby transport rejected native staging");
    check(stagedReceives.size() == 1, "native stage seam did not retain exactly one completion");
}

void completeNextNative(netintercept::NativeReceiveResult result, int handlerCount,
                        std::uint32_t sender = game::serverNetPlayerId,
                        netintercept::RxDecision policy = netintercept::RxDecision::Pass)
{
    check(!stagedReceives.empty(), "expected one staged native completion");
    auto staged = std::move(stagedReceives.front());
    stagedReceives.pop_front();
    // Keep the production callback policy-free. A normally returned native
    // dispatch must carry its real handler-count classification; policy Drop
    // is independently a failure and cannot be rescued by the adapter.
    const auto actual = policy == netintercept::RxDecision::Drop
        ? netintercept::NativeReceiveResult::Failed
        : netintercept::nativeDispatchResult(handlerCount);
    check(actual == result, "test input contradicts native dispatch completion semantics");
    staged.diagnostic.sender = sender;
    staged.diagnostic.dispatchResult = handlerCount;
    staged.diagnostic.policy = policy;
    staged.diagnostic.dispatched = policy == netintercept::RxDecision::Pass;
    staged.complete(staged.context, actual, staged.diagnostic);
}

void dispatchNative(NativeFrame& frame, netintercept::NativeReceiveResult result,
                    int handlerCount, std::uint32_t sender = game::serverNetPlayerId,
                    bool clientReceiver = true,
                    netintercept::RxDecision policy = netintercept::RxDecision::Pass)
{
    stageNative(frame, clientReceiver, sender);
    completeNextNative(result, handlerCount, sender, policy);
}

std::vector<simturns::lobby::Envelope> sentEnvelopes(const CNetCustomService& service)
{
    std::vector<simturns::lobby::Envelope> result;
    for (const auto& packet : service.sent) {
        check(packet.size() > 1 && packet.front() == ID_LOBBY_SIMULTANEOUS_TURNS,
              "test service captured a malformed lobby packet");
        simturns::lobby::Envelope envelope;
        check(simturns::lobby::decode(packet.data() + 1, packet.size() - 1, envelope),
              "test service could not decode production lobby output");
        result.push_back(std::move(envelope));
    }
    return result;
}

unsigned abortCount(const CNetCustomService& service)
{
    const auto envelopes = sentEnvelopes(service);
    return static_cast<unsigned>(std::count_if(envelopes.begin(), envelopes.end(), [](const auto& value) {
        return value.operation == simturns::lobby::Operation::Abort;
    }));
}

void armMap(CNetCustomService& service, CNetCustomSession& session,
            std::uint32_t epoch, Role role = Role::Join)
{
    CoordinatorPort::processInstance().stop();
    service.sent.clear();
    service.notices.clear();
    strategicIdle = false;
    ++pregameGeneration;
    session.setHost(role == Role::Host);
    lobbyRoomJoined(&service, 6);
    simturns::lobby::Envelope arm;
    arm.operation = simturns::lobby::Operation::Arm;
    arm.room = 6;
    arm.epoch = epoch;
    arm.role = static_cast<std::uint8_t>(role);
    arm.mergeDay = 3;
    const auto bytes = simturns::lobby::encode(arm);
    check(!bytes.empty(), "could not encode test Arm");
    receiveLobbyControl(&service, bytes.data(), bytes.size());
    check(lobbyMapArmed(&service), "production lobby transport did not arm the map");
    const auto envelopes = sentEnvelopes(service);
    check(envelopes.size() == 1 && envelopes.front().operation == simturns::lobby::Operation::ArmAck
              && envelopes.front().status == 0,
          "map Arm was not acknowledged");
}

void retireMap()
{
    drainAllUiTasks();
    check(stagedReceives.empty(), "native completion leaked across a map");
    lobbyMapTeardownBegun();
    lobbyMapDestroyed();
    CoordinatorPort::processInstance().stop();
}

void wrongSenderJoinGameRemainsFailClosed(CNetCustomService& service,
                                          CNetCustomSession& session)
{
    armMap(service, session, 5);
    auto join = syntheticJoinGame62();
    dispatchNative(join, netintercept::NativeReceiveResult::Unhandled, 0, 2);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "wrong-sender unhandled JoinGame did not fail closed");
    retireMap();
}

void directedBeginTurnRemainsFailClosed(CNetCustomService& service,
                                        CNetCustomSession& session)
{
    armMap(service, session, 6);
    // Directed activation is not the natural startup broadcast. The production
    // RX gate latches/validates this tuple before native dispatch; a broad
    // zero-handler BeginTurn exception would hide a rejected causal command.
    NativeFrame directed(".?AVCCmdBeginTurnMsg@@", 56,
                         0xa3de0002, UINT32_MAX, 0xa3de0001);
    dispatchNative(directed, netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "directed zero-handler BeginTurn was incorrectly retired as a startup broadcast");
    retireMap();
}

void scenarioBoundaryClosesZeroHandlerExceptions(CNetCustomService& service,
                                                 CNetCustomSession& session,
                                                 std::uint32_t epoch = 7,
                                                 const char* boundary = ".?AVCNewScenarioMsg@@")
{
    armMap(service, session, epoch);
    strategicIdle = true;
    NativeFrame scenario(boundary, 48);
    dispatchNative(scenario, netintercept::NativeReceiveResult::Applied, 1);
    drainAllUiTasks();

    auto lateJoin = syntheticJoinGame62();
    dispatchNative(lateJoin, netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "scenario boundary did not permanently close the zero-handler JoinGame exception");
    retireMap();
}

void unrelatedPregameCommandRemainsFailClosed(CNetCustomService& service,
                                             CNetCustomSession& session)
{
    armMap(service, session, 23);
    NativeFrame move(".?AVCCmdMoveStackMsg@@", 56);
    dispatchNative(move, netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "unrelated pregame strategic command was silently retired");
    retireMap();
}

void changedGenerationCannotRescueUnhandled(CNetCustomService& service,
                                            CNetCustomSession& session,
                                            std::uint32_t epoch, bool zero)
{
    armMap(service, session, epoch);
    auto join = syntheticJoinGame62();
    stageNative(join);
    if (zero) pregameGeneration = 0;
    else ++pregameGeneration;
    completeNextNative(netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          zero ? "zero completion generation rescued an unhandled JoinGame"
               : "changed completion generation rescued an unhandled JoinGame");
    retireMap();
}

void lateOldBindingCompletionCannotFaultNewMap(CNetCustomService& service,
                                               CNetCustomSession& session)
{
    armMap(service, session, 10);
    NativeFrame oldRefresh(".?AVCRefreshInfo@@", 105);
    stageNative(oldRefresh);

    // Teardown retires the old binding before the intentionally late callback.
    // The test stub retains only the callback-owned context, never a Binding.
    lobbyMapTeardownBegun();
    lobbyMapDestroyed();
    CoordinatorPort::processInstance().stop();

    armMap(service, session, 11);
    completeNextNative(netintercept::NativeReceiveResult::Failed, -1);
    drainAllUiTasks();
    check(lobbyMapArmed(&service) && abortCount(service) == 0,
          "late old-map native completion faulted the replacement binding");
    retireMap();
}

void failedNativeResultsAreNeverRescued(CNetCustomService& service,
                                        CNetCustomSession& session,
                                        std::uint32_t epoch,
                                        netintercept::RxDecision policy)
{
    armMap(service, session, epoch);
    auto join = syntheticJoinGame62();
    dispatchNative(join, netintercept::NativeReceiveResult::Failed, -1,
                   game::serverNetPlayerId, true, policy);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          policy == netintercept::RxDecision::Drop
              ? "policy Drop was rescued by the unhandled-notification exception"
              : "failed native dispatch was rescued by the unhandled-notification exception");
    retireMap();
}

void mandatoryMenuResponseRemainsFailClosed(CNetCustomService& service,
                                            CNetCustomSession& session)
{
    armMap(service, session, 14);
    // Payload is intentionally synthetic/zero: a mandatory menu response which
    // native code cannot handle is not an optional pregame notification.
    NativeFrame menu(".?AVCMenusAnsInfoMsg@@", 199);
    dispatchNative(menu, netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "zero-handler mandatory MenusAnsInfo was silently retired");
    retireMap();
}

void mandatoryPlayerListRemainsFailClosed(CNetCustomService& service,
                                          CNetCustomSession& session)
{
    armMap(service, session, 19);
    NativeFrame players(".?AVCPlayerListMsg@@", 88);
    dispatchNative(players, netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "zero-handler mandatory PlayerList was silently retired");
    retireMap();
}

void hostAndServerEndpointRemainFailClosed(CNetCustomService& service,
                                           CNetCustomSession& session)
{
    armMap(service, session, 15, Role::Host);
    auto hostJoin = syntheticJoinGame62();
    dispatchNative(hostJoin, netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "host received the join-only zero-handler JoinGame exception");
    retireMap();

    armMap(service, session, 16);
    auto serverJoin = syntheticJoinGame62();
    dispatchNative(serverJoin, netintercept::NativeReceiveResult::Unhandled, 0,
                   game::serverNetPlayerId, false);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "server endpoint received the client-only zero-handler JoinGame exception");
    retireMap();
}

protocol::Bytes controlPacket(protocol::Op op, std::initializer_list<std::uint32_t> words)
{
    protocol::Bytes payload;
    for (const auto word : words)
        for (unsigned i = 0; i != 4; ++i)
            payload.push_back(static_cast<std::uint8_t>(word >> (8 * i)));
    return protocol::encodeFrame(op, payload);
}

void appliedClientTicketBlocksControlUntilStrategicDrain(CNetCustomService& service,
                                                         CNetCustomSession& session,
                                                         bool withNotification = false)
{
    const std::uint32_t epoch = withNotification ? 21 : 17;
    armMap(service, session, epoch);
    unsigned events{}, faults{};
    CoordinatorCallbacks callbacks;
    callbacks.postToUi = [&](CoordinatorEvent) { ++events; };
    callbacks.terminalFault = [&](CoordinatorTerminalFault) { ++faults; };
    auto& port = CoordinatorPort::processInstance();
    check(port.start({true, Role::Join}, std::move(callbacks)),
          "could not start the real CoordinatorPort for barrier coverage");
    check(port.bindLocalPlayer(2), "could not bind the join player for barrier coverage");

    NativeFrame menu(".?AVCMenusAnsInfoMsg@@", 199);
    dispatchNative(menu, netintercept::NativeReceiveResult::Applied, 1);

    // The optional zero-handler notification must retire its own ticket while
    // leaving the preceding Applied ticket waiting for strategic drain. The
    // real control frame below is fenced behind both, so neither a leaked
    // JoinGame ticket nor a blanket fence bypass can pass this test.
    if (withNotification) {
        auto join = syntheticJoinGame62();
        dispatchNative(join, netintercept::NativeReceiveResult::Unhandled, 0);
    }

    lobby::Envelope frame;
    frame.operation = lobby::Operation::Frame;
    frame.room = 6;
    frame.epoch = epoch;
    frame.frame = controlPacket(protocol::Op::SessionPlan,
                                {epoch, 1, 1, 2, 3, 11, 12});
    const auto bytes = lobby::encode(frame);
    check(!bytes.empty(), "could not encode the real control frame");
    receiveLobbyControl(&service, bytes.data(), bytes.size());

    drainAllUiTasks();
    check(events == 0 && faults == 0,
          "control frame crossed an Applied client ticket before strategic drain");
    strategicIdle = true;
    port.notifyNativeProgress();
    drainAllUiTasks();
    check(events == 1 && faults == 0 && abortCount(service) == 0,
          "strategic drain did not release the fenced real control frame exactly once");
    retireMap();
}

void stagedJoinCannotCrossNestedScenarioBoundary(CNetCustomService& service,
                                                 CNetCustomSession& session)
{
    armMap(service, session, 18);
    auto join = syntheticJoinGame62();
    stageNative(join);

    // A nested native receive may close the pregame window after JoinGame was
    // classified but before its original dispatch returns and completes.
    NativeFrame scenario(".?AVCNewScenarioMsg@@", 48);
    auto boundaryTicket = lobbyTrackNativePacket(true);
    check(static_cast<bool>(boundaryTicket), "nested boundary did not receive a native ticket");
    bool delivered{};
    lobbyDeliverNativePacket(boundaryTicket, [&] { delivered = true; });
    check(delivered && lobbyStageNativeReceive(scenario.header(), std::move(boundaryTicket),
                                               game::serverNetPlayerId),
          "nested NewScenario could not stage");
    check(stagedReceives.size() == 2,
          "nested scenario boundary did not preserve both native completions");

    completeNextNative(netintercept::NativeReceiveResult::Unhandled, 0);
    completeNextNative(netintercept::NativeReceiveResult::Applied, 1);
    drainAllUiTasks();
    check(abortCount(service) == 1,
          "staged JoinGame crossed a nested NewScenario boundary");
    retireMap();
}

void capturedFailingJoinBatchMustNotAbort(CNetCustomService& service,
                                          CNetCustomSession& session)
{
    armMap(service, session, 20);

    struct Entry
    {
        const char* messageClass;
        std::uint32_t length;
        netintercept::NativeReceiveResult result;
        int handlerCount;
        std::uint32_t word0{}, word1{}, word2{};
    };

    // Exact order and lengths from artifacts/oh-repro-20260924-065703/mss32.log.
    // Only JoinGame's zero handler count is directly observed. The other result
    // assignments are test inputs: thirteen Applied packets reproduce the
    // diagnostic's aggregate awaiting_drain=13, while two Refresh packets and
    // the BeginTurn separately exercise the existing narrow exceptions.
    const std::vector<Entry> burst{
        {".?AVCRefreshInfo@@", 23491, netintercept::NativeReceiveResult::Unhandled, 0},
        {".?AVCRefreshInfo@@", 105, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 105, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 105, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 105, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 105, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 535, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 56, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCMenusAnsInfoMsg@@", 199, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCCmdBeginTurnMsg@@", 56, netintercept::NativeReceiveResult::Unhandled, 0,
         0, 1, 0xa3de0001},
        {".?AVCRefreshInfo@@", 56, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 91, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 616, netintercept::NativeReceiveResult::Unhandled, 0},
        {".?AVCRefreshInfo@@", 218, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCRefreshInfo@@", 464, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCMenusAnsInfoMsg@@", 199, netintercept::NativeReceiveResult::Applied, 1},
        {".?AVCJoinGameMsg@@", 62, netintercept::NativeReceiveResult::Unhandled, 0},
        {".?AVCPlayerListMsg@@", 88, netintercept::NativeReceiveResult::Applied, 1},
    };

    for (std::size_t i = 0; i < burst.size(); ++i) {
        const auto& entry = burst[i];
        NativeFrame frame = i == 16 ? syntheticJoinGame62()
            : NativeFrame(entry.messageClass, entry.length,
                          entry.word0, entry.word1, entry.word2);
        dispatchNative(frame, entry.result, entry.handlerCount);

        // Match the natural-frame interleaving in the captured batch: one
        // completion ran after packet 1, packet 9, and packet 16; the remaining
        // completions stayed queued through JoinGame and PlayerList.
        if (i == 0 || i == 8 || i == 15) drainOneUiTask();
    }

    drainAllUiTasks();
    strategicIdle = true;
    CoordinatorPort::processInstance().notifyNativeProgress();
    drainAllUiTasks();

    const auto aborts = abortCount(service);
    retireMap();
    check(aborts == 0,
          "observed join startup burst emitted protocol Abort at zero-handler CJoinGameMsg(62)");
}

void updateObjNegativeControls(CNetCustomService& service, CNetCustomSession& session)
{
    std::uint32_t epoch = 24;
    const auto mustAbort = [&](NativeFrame frame, Role role = Role::Join,
                              bool clientReceiver = true,
                              std::uint32_t sender = game::serverNetPlayerId) {
        armMap(service, session, epoch++, role);
        dispatchNative(frame, netintercept::NativeReceiveResult::Unhandled, 0,
                       sender, clientReceiver);
        drainAllUiTasks();
        check(abortCount(service) == 1, "invalid UpdateObj notification escaped its boundary");
        retireMap();
    };
    for (const auto length : {44u, 48u, 51u, 53u, 56u})
        mustAbort(NativeFrame(".?AVCCmdUpdateObjMsg@@", length));
    auto wrongType = syntheticUpdateObj52();
    wrongType.header()->messageType = 0xfffe;
    mustAbort(std::move(wrongType));
    auto longerName = syntheticUpdateObj52();
    longerName.header()->messageClassName[sizeof(".?AVCCmdUpdateObjMsg@@") - 1] = 'X';
    mustAbort(std::move(longerName));
    mustAbort(syntheticUpdateObj52(), Role::Host);
    mustAbort(syntheticUpdateObj52(), Role::Join, false);
    mustAbort(syntheticUpdateObj52(), Role::Join, true, 2);

    for (const bool zero : {false, true}) {
        armMap(service, session, epoch++);
        auto update = syntheticUpdateObj52();
        stageNative(update);
        if (zero) pregameGeneration = 0;
        else ++pregameGeneration;
        completeNextNative(netintercept::NativeReceiveResult::Unhandled, 0);
        drainAllUiTasks();
        check(abortCount(service) == 1, "UpdateObj crossed its no-CMidClient generation");
        retireMap();
    }
    for (const auto policy : {netintercept::RxDecision::Pass, netintercept::RxDecision::Drop}) {
        armMap(service, session, epoch++);
        auto update = syntheticUpdateObj52();
        dispatchNative(update, netintercept::NativeReceiveResult::Failed, -1,
                       game::serverNetPlayerId, true, policy);
        drainAllUiTasks();
        check(abortCount(service) == 1, "failed UpdateObj dispatch was rescued");
        retireMap();
    }
    for (const auto* boundary : {".?AVCNewScenarioMsg@@", ".?AVCStartScenarioMsg@@"}) {
        armMap(service, session, epoch++);
        NativeFrame scenario(boundary, 48);
        dispatchNative(scenario, netintercept::NativeReceiveResult::Applied, 1);
        drainAllUiTasks();
        // Keep the stub generation nonzero to independently prove that the
        // per-binding latch cannot reopen even if a lifetime probe is delayed.
        auto update = syntheticUpdateObj52();
        dispatchNative(update, netintercept::NativeReceiveResult::Unhandled, 0);
        drainAllUiTasks();
        check(abortCount(service) == 1, "post-snapshot UpdateObj was silently retired");
        retireMap();
    }

    armMap(service, session, epoch++);
    auto update = syntheticUpdateObj52();
    stageNative(update);
    NativeFrame scenario(".?AVCNewScenarioMsg@@", 48);
    auto ticket = lobbyTrackNativePacket(true);
    check(static_cast<bool>(ticket), "nested scenario did not receive a ticket");
    bool delivered{};
    lobbyDeliverNativePacket(ticket, [&] { delivered = true; });
    check(delivered && lobbyStageNativeReceive(scenario.header(), std::move(ticket),
                                               game::serverNetPlayerId),
          "could not stage nested scenario after UpdateObj");
    completeNextNative(netintercept::NativeReceiveResult::Unhandled, 0);
    completeNextNative(netintercept::NativeReceiveResult::Applied, 1);
    drainAllUiTasks();
    check(abortCount(service) == 1, "UpdateObj completion crossed a nested snapshot boundary");
    retireMap();
}

void run004UpdateObjThenOwnSnapshot(CNetCustomService& service, CNetCustomSession& session)
{
    constexpr std::uint32_t epoch = 50;
    armMap(service, session, epoch);
    struct Entry {
        const char* name;
        std::uint32_t length;
        int handlers;
        std::uint32_t word0{}, word1{}, word2{};
    };
    // Order/lengths are from oh-real-lobby-20260926/run-004/join.mss32.log.
    // Apart from UpdateObj's observed zero, handler counts below are controlled
    // test inputs, not recovered packet-by-packet evidence. Fourteen Applied
    // inputs reproduce the observed awaiting_drain=14 without inventing a drain.
    const Entry burst[]{
        {".?AVCRefreshInfo@@", 70700, 0},
        {".?AVCRefreshInfo@@", 20326, 0},
        {".?AVCRefreshInfo@@", 105, 1},
        {".?AVCRefreshInfo@@", 105, 1},
        {".?AVCRefreshInfo@@", 105, 1},
        {".?AVCRefreshInfo@@", 105, 1},
        {".?AVCRefreshInfo@@", 105, 1},
        {".?AVCRefreshInfo@@", 545, 1},
        {".?AVCRefreshInfo@@", 56, 1},
        {".?AVCMenusAnsInfoMsg@@", 204, 1},
        {".?AVCCmdBeginTurnMsg@@", 56, 0, 0, 1, 0xa3de0001},
        {".?AVCRefreshInfo@@", 56, 1},
        {".?AVCRefreshInfo@@", 91, 1},
        {".?AVCRefreshInfo@@", 218, 1},
        {".?AVCRefreshInfo@@", 316, 1},
        {".?AVCMenusAnsInfoMsg@@", 204, 1},
        {".?AVCJoinGameMsg@@", 62, 0},
        {".?AVCPlayerListMsg@@", 88, 1},
    };
    for (const auto& entry : burst) {
        auto frame = std::strcmp(entry.name, ".?AVCJoinGameMsg@@") == 0
            ? syntheticJoinGame62()
            : NativeFrame(entry.name, entry.length, entry.word0, entry.word1, entry.word2);
        dispatchNative(frame, netintercept::nativeDispatchResult(entry.handlers), entry.handlers);
    }
    drainAllUiTasks();
    check(abortCount(service) == 0 && service.sent.size() == 1,
          "startup burst aborted or fabricated an engine acknowledgement");

    auto update = syntheticUpdateObj52();
    NativeFrame renameRefresh(".?AVCRefreshInfo@@", 118);
    dispatchNative(update, netintercept::NativeReceiveResult::Unhandled, 0);
    // This companion result is a test input: the run aborted before its queued
    // UI completion. It remains a pre-snapshot native notification, not an ACK.
    dispatchNative(renameRefresh, netintercept::NativeReceiveResult::Unhandled, 0);
    drainAllUiTasks();
    check(lobbyMapArmed(&service) && abortCount(service) == 0 && service.sent.size() == 1,
          "run004 pre-snapshot UpdateObj(52) aborted or fabricated an engine acknowledgement");

    // The following own snapshot is a required continuation, not an event seen
    // in the aborted run. Creating CMidClient closes the generation before the
    // strategic phase exists; its NewScenario and full Refresh must still run.
    pregameGeneration = 0;
    NativeFrame scenario(".?AVCNewScenarioMsg@@", 48);
    NativeFrame fullRefresh(".?AVCRefreshInfo@@", 70700);
    dispatchNative(scenario, netintercept::NativeReceiveResult::Applied, 1);
    dispatchNative(fullRefresh, netintercept::NativeReceiveResult::Applied, 1);
    drainAllUiTasks();

    unsigned events{}, faults{};
    CoordinatorCallbacks callbacks;
    callbacks.postToUi = [&](CoordinatorEvent event) {
        check(event.kind == CoordinatorEventKind::SessionPlan, "unexpected control event");
        ++events;
    };
    callbacks.terminalFault = [&](CoordinatorTerminalFault) { ++faults; };
    auto& port = CoordinatorPort::processInstance();
    check(port.start({true, Role::Join}, std::move(callbacks)) && port.bindLocalPlayer(2),
          "own snapshot could not progress to the actual coordinator port");
    const auto beforeControl = service.sent.size();
    lobby::Envelope control;
    control.operation = lobby::Operation::Frame;
    control.room = 6;
    control.epoch = epoch;
    control.frame = controlPacket(protocol::Op::SessionPlan, {epoch, 1, 1, 2, 3, 11, 12});
    const auto bytes = lobby::encode(control);
    check(!bytes.empty(), "could not encode continuation SessionPlan");
    receiveLobbyControl(&service, bytes.data(), bytes.size());
    drainAllUiTasks();
    check(events == 0 && faults == 0 && service.sent.size() == beforeControl,
          "UpdateObj retirement bypassed an Applied snapshot/startup ticket");
    strategicIdle = true;
    port.notifyNativeProgress();
    drainAllUiTasks();
    check(events == 1 && faults == 0 && abortCount(service) == 0
              && service.sent.size() == beforeControl,
          "own snapshot drain did not release SessionPlan once without fabricated engine ACKs");
    retireMap();
}

} // namespace

namespace hooks::simturns {

bool available() { return true; }
bool strategicQueueIdle() { return strategicIdle; }
std::uint64_t pregameNativeNotificationGeneration() { return pregameGeneration; }
bool beginSession(Role) { return true; }

} // namespace hooks::simturns

namespace hooks::netintercept {

bool stageNativeReceive(const game::NetMessageHeader* buffer, void* context,
                        NativeReceiveCallback complete, UiTaskDiscardCallback discard)
{
    if (!buffer || !context || !complete || !discard) return false;
    StagedReceive staged;
    staged.context = context;
    staged.complete = complete;
    staged.discard = discard;
    staged.diagnostic = diagnosticFor(buffer, 1);
    stagedReceives.push_back(std::move(staged));
    return true;
}

bool queueOnNextUiFrame(UiTaskCallback callback, void* context, UiTaskDiscardCallback discard)
{
    if (!callback || !context) return false;
    uiTasks.push_back({callback, context, discard});
    return true;
}

} // namespace hooks::netintercept

int main()
{
    try {
        CNetCustomSession session(false);
        CNetCustomService service(&session);
        const auto run = [](const char* name, auto&& test) {
            test();
            std::cout << "PASS " << name << '\n';
        };
        run("wrong-sender JoinGame stays fail-closed",
            [&] { wrongSenderJoinGameRemainsFailClosed(service, session); });
        run("directed BeginTurn stays fail-closed",
            [&] { directedBeginTurnRemainsFailClosed(service, session); });
        run("NewScenario closes pregame zero-handler exceptions",
            [&] { scenarioBoundaryClosesZeroHandlerExceptions(service, session); });
        run("zero generation cannot rescue Unhandled",
            [&] { changedGenerationCannotRescueUnhandled(service, session, 8, true); });
        run("changed generation cannot rescue Unhandled",
            [&] { changedGenerationCannotRescueUnhandled(service, session, 9, false); });
        run("late old-binding completion cannot fault replacement map",
            [&] { lateOldBindingCompletionCannotFaultNewMap(service, session); });
        run("RX Drop cannot be rescued",
            [&] { failedNativeResultsAreNeverRescued(service, session, 12,
                                                      netintercept::RxDecision::Drop); });
        run("failed native dispatch cannot be rescued",
            [&] { failedNativeResultsAreNeverRescued(service, session, 13,
                                                      netintercept::RxDecision::Pass); });
        run("mandatory MenusAnsInfo zero-handler stays fail-closed",
            [&] { mandatoryMenuResponseRemainsFailClosed(service, session); });
        run("host and server endpoint stay fail-closed",
            [&] { hostAndServerEndpointRemainFailClosed(service, session); });
        run("Applied native ticket fences a real control frame",
            [&] { appliedClientTicketBlocksControlUntilStrategicDrain(service, session); });
        run("staged JoinGame cannot cross nested NewScenario",
            [&] { stagedJoinCannotCrossNestedScenarioBoundary(service, session); });
        run("mandatory PlayerList zero-handler stays fail-closed",
            [&] { mandatoryPlayerListRemainsFailClosed(service, session); });
        run("captured 18-packet join batch does not Abort",
            [&] { capturedFailingJoinBatchMustNotAbort(service, session); });
        run("filtered JoinGame retires only its ticket before the control frame",
            [&] { appliedClientTicketBlocksControlUntilStrategicDrain(service, session, true); });
        run("StartScenario also closes pregame zero-handler exceptions",
            [&] { scenarioBoundaryClosesZeroHandlerExceptions(service, session, 22,
                                                               ".?AVCStartScenarioMsg@@"); });
        run("unrelated pregame strategic command stays fail-closed",
            [&] { unrelatedPregameCommandRemainsFailClosed(service, session); });
        run("UpdateObj shape, sender, role, failure and snapshot boundaries stay closed",
            [&] { updateObjNegativeControls(service, session); });
        run("run004 UpdateObj retires before own snapshot without bypassing its apply fence",
            [&] { run004UpdateObjThenOwnSnapshot(service, session); });
        std::cout << "lobby startup: pre-snapshot notifications retired; own snapshot and control fence preserved\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "lobby startup regression: " << e.what() << '\n';
        return 1;
    }
}
