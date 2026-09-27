/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 *
 * The coordinator is a control plane only. Ordinary game messages retain their
 * native send/receive path through the lobby transport; this module observes
 * End Turn and applies only authorized turn-start/barrier operations.
 */

#include "simturns/controller.h"

#include "executablefingerprint.h"
#include "game.h"
#include "gameutils.h"
#include "midclient.h"
#include "midclientcore.h"
#include "midgard.h"
#include "midobjectlock.h"
#include "midserver.h"
#include "midserverlogic.h"
#include "midstack.h"
#include "mqnetplayerclient.h"
#include "netintercept.h"
#include "netmsg.h"
#include "netplayerinfo.h"
#include "phasegame.h"
#include "scenarioinfo.h"
#include "simturns/coordinator_port.h"
#include "simturns/day_scope.h"
#include "simturns/engine_hooks.h"
#include "simturns/patches.h"
#include "simturns/state.h"
#include "simturns/turn_context.h"
#include "uiframedispatcher.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks::simturns {

namespace {

constexpr std::uintptr_t mergeBeginTurnAddress = 0x00420FFA;
constexpr std::array<std::uint8_t, 16> mergeBeginTurnPrefix{{
    0xB8, 0xB4, 0xB2, 0x68, 0x00, 0xE8, 0xCC, 0xC3,
    0x24, 0x00, 0x83, 0xEC, 0x14, 0x53, 0x56, 0x57,
}};

constexpr char requestEndTurnRtti[] = ".?AVCReqEndTurnMsg@@";
constexpr char beginTurnRtti[] = ".?AVCCmdBeginTurnMsg@@";
constexpr char turnInfoRtti[] = ".?AVCCmdTurnInfoMsg@@";
constexpr char startupLeaderNameRtti[] = ".?AVCStackChangeLeaderNameMsg@@";
constexpr std::size_t netMessagePayloadOffset =
    offsetof(game::NetMessageHeader, messageClassName);
constexpr std::size_t syntheticBeginTurnPayloadSize = 48;
constexpr std::size_t syntheticBeginTurnFrameLength =
    netMessagePayloadOffset + syntheticBeginTurnPayloadSize;
constexpr std::size_t requestEndTurnFrameLength = sizeof(game::NetMessageHeader);
static_assert(netMessagePayloadOffset == 8);
static_assert(syntheticBeginTurnFrameLength == 56);
static_assert(requestEndTurnFrameLength == 44);
constexpr std::size_t syntheticAddresseeOffset = 36;
constexpr std::size_t syntheticSequenceOffset = 40;
constexpr std::size_t syntheticActiveHandleOffset = 44;
constexpr std::uint32_t directedBeginTurnSequence = 0xffffffffu;
constexpr std::uint32_t startupBeginTurnSequence = 1;
constexpr std::size_t maximumPlayers = 8;
constexpr std::size_t netMessageClassNameSize =
    sizeof(static_cast<game::NetMessageHeader*>(nullptr)->messageClassName);
constexpr std::size_t startupLeaderNameStackOffset = netMessageClassNameSize;
constexpr std::size_t startupLeaderNameByteCountOffset =
    startupLeaderNameStackOffset + sizeof(game::CMidgardID);
constexpr std::size_t startupLeaderNameBytesOffset =
    startupLeaderNameByteCountOffset + sizeof(std::uint32_t);
constexpr std::uint32_t startupLeaderNameMaximumBytes = 500;
static_assert(sizeof(startupLeaderNameRtti) == 32);
static_assert(netMessageClassNameSize == 36);
static_assert(netMessagePayloadOffset + startupLeaderNameBytesOffset == 52);

struct ExactRtti
{
    const char* text;
    std::size_t storageSize;
};

enum class TurnAnnouncementLayout
{
    Invalid,
    Broadcast,
    Directed,
};

struct TurnAnnouncement
{
    TurnAnnouncementLayout layout{TurnAnnouncementLayout::Invalid};
    std::uint32_t addressee{};
    std::uint32_t activeHandle{};
    std::uint32_t sequence{};
};

enum class JoinBootstrap : std::uint8_t
{
    Dormant,
    AwaitingStartupDrain,
    BeginTurnQueued,
    AwaitingOwnTurnInfo,
    OwnTurnInfoQueued,
    AwaitingCommit,
    CommitApplied,
    OperationalApplied,
    Complete,
    Failed,
};

enum class HostBootstrap : std::uint8_t
{
    Dormant,
    AwaitingCascade,
    CascadeRunning,
    AwaitingCommit,
    CommitApplied,
    OperationalApplied,
    Complete,
    Failed,
};

enum class StartupLeaderNameTx : std::uint8_t
{
    Dormant,
    Expected,
    InFlight,
    Sent,
};

enum class NetworkIdentityBindResult : std::uint8_t
{
    Bound,
    WaitingForNaturalJoinTurnProof,
    Invalid,
};

enum class StartupTurnProofState : std::uint8_t
{
    Empty,
    BroadcastLatched,
    DirectedLatched,
    Conflicted,
};

#define D2_EXACT_RTTI(text) {text, sizeof(text)}

// Exact Russobit CMidServerLogic input map recovered from the contiguous
// dispatcher at lobby/RE/Discipl2.exe.c:70927-73480. Chat, paperdoll queries,
// EndQueueCommands and connection lifecycle traffic are deliberately absent.
// The final four entries are proven target=1 envelopes handled outside that
// member map (including the MSS resource-exchange extension).
constexpr ExactRtti strategicIntentRtti[] = {
    D2_EXACT_RTTI(".?AVCNextScenarioMsg@@"),
    D2_EXACT_RTTI(".?AVCReqEndTurnMsg@@"),
    D2_EXACT_RTTI(".?AVCStackMoveMsg@@"),
    D2_EXACT_RTTI(".?AVCStackBattleActionMsg@@"),
    D2_EXACT_RTTI(".?AVCSiteBuyItemMsg@@"),
    D2_EXACT_RTTI(".?AVCSiteSellItemMsg@@"),
    D2_EXACT_RTTI(".?AVCSiteBuySpellMsg@@"),
    D2_EXACT_RTTI(".?AVCSiteBuyUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCSiteTrainUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCNobleActionMsg@@"),
    D2_EXACT_RTTI(".?AVCNobleActionCancelMsg@@"),
    D2_EXACT_RTTI(".?AVCSpellCastMsg@@"),
    D2_EXACT_RTTI(".?AVCSpellResearchMsg@@"),
    D2_EXACT_RTTI(".?AVCCastWandScrollMsg@@"),
    D2_EXACT_RTTI(".?AVCCityGrowMsg@@"),
    D2_EXACT_RTTI(".?AVCCityBuildMsg@@"),
    D2_EXACT_RTTI(".?AVCGiveResourceMsg@@"),
    D2_EXACT_RTTI(".?AVCProposeTradeMsg@@"),
    D2_EXACT_RTTI(".?AVCAcceptTradeMsg@@"),
    D2_EXACT_RTTI(".?AVCProposeAllianceMsg@@"),
    D2_EXACT_RTTI(".?AVCAcceptAllianceMsg@@"),
    D2_EXACT_RTTI(".?AVCBreakAllianceMsg@@"),
    D2_EXACT_RTTI(".?AVCStackSwapUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCStackEnrollLeaderMsg@@"),
    D2_EXACT_RTTI(".?AVCStackChangeLeaderNameMsg@@"),
    D2_EXACT_RTTI(".?AVCStackDismissLeaderMsg@@"),
    D2_EXACT_RTTI(".?AVCStackEnrollUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCStackDismissUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCForceDynLevelUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCStackExchangeItemMsg@@"),
    D2_EXACT_RTTI(".?AVCStackEquipItemMsg@@"),
    D2_EXACT_RTTI(".?AVCStackUpgLeadersMsg@@"),
    D2_EXACT_RTTI(".?AVCStackUsePotionMsg@@"),
    D2_EXACT_RTTI(".?AVCStackHealUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCStackReviveUnitMsg@@"),
    D2_EXACT_RTTI(".?AVCDropBagMsg@@"),
    D2_EXACT_RTTI(".?AVCCloseBagMsg@@"),
    D2_EXACT_RTTI(".?AVCDropRodMsg@@"),
    D2_EXACT_RTTI(".?AVCStartQueueCommandsMsg@@"),
    D2_EXACT_RTTI(".?AVCUnpauseAIMsg@@"),
    D2_EXACT_RTTI(".?AVCDoneExportLeaderMsg@@"),
    D2_EXACT_RTTI(".?AVCProposeExchangeMapMsg@@"),
    D2_EXACT_RTTI(".?AVCAcceptExchangeMapMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBattleStartMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdQueueCommandsMsg@@"),
    D2_EXACT_RTTI(".?AVCSaveGameMsg@@"),
    D2_EXACT_RTTI(".?AVCExchangeResourcesMsg@@"),
};

// Exact Russobit CMidClient receive map at lobby/RE/Discipl2.exe.c:54221-58403.
// Presentation-only chat, popup and GameSaved notifications are excluded.
// This broad set is used only for terminal fail-closed handling, never for
// generic worker-to-UI replay (which must remain narrowly proven).
constexpr ExactRtti authoritativeStateMutationRtti[] = {
    D2_EXACT_RTTI(".?AVCRefreshInfo@@"),
    D2_EXACT_RTTI(".?AVCCmdEraseObjMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdUpdateObjMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdMoveStackMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdCastSpellMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBattleStartMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBattleChooseActionMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBattleResultMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBattleEndMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdStackDestroyedMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdStackAppearMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdStackIllusionMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdNobleActionMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdNobleResultMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdPickupBagMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdOpenBagMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdCloseBagMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdEndTurnMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBeginTurnMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdTurnInfoMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdTurnSummaryMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdExportLeaderMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdScenarioEndMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdProposeTradeMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdTradeAcceptMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdProposeAllianceMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdAcceptAllianceMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBreakAllianceMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdGiveGoldMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdDiscoverSpyMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdBreakRodMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdDropRodMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdTerrainChangeMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdRemoveLMarkMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdMapChangeMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdOccupyCityMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdCityGrowMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdLootRuinMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdStackVisitMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdCreateStackMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdChangeOwnerMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdGiveSpellMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdGiveItemMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdChangeObjectiveMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdChangeFogMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdStackActionMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdRmvRiotMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdUpgradeLeadersMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdCaptureCapitalMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdCaptureResourcesMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdQueueCommandsMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdChangeOrderMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdChangeLandmarkMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdProposeExchangeMapMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdAcceptExchangeMapMsg@@"),
};

// Canonical proxy proved the first three worker-RX paths. The additional four
// enter MSS day-scoped execution and therefore fault if allowed off the typed
// UI seam. Do not broaden this list without a live trace/RE proof: native
// handshake and synchronization traffic is ordering-sensitive.
constexpr ExactRtti requiresUiReplayRtti[] = {
    D2_EXACT_RTTI(".?AVCStackMoveMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdMoveStackMsg@@"),
    D2_EXACT_RTTI(".?AVCReqEndTurnMsg@@"),
    D2_EXACT_RTTI(".?AVCCityBuildMsg@@"),
    D2_EXACT_RTTI(".?AVCSpellResearchMsg@@"),
    D2_EXACT_RTTI(".?AVCSpellCastMsg@@"),
    D2_EXACT_RTTI(".?AVCCmdCastSpellMsg@@"),
};

#undef D2_EXACT_RTTI

std::atomic<bool> g_prepareAttempted{false};
std::atomic<bool> g_prepared{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_sessionActive{false};
std::atomic<bool> g_tearingDown{false};
std::atomic<std::uint64_t> g_sessionGeneration{1};
// A transport callback must not pass its generation check in one map and
// publish a fault in the next. Reentry is possible through fault -> port ->
// terminalFault on the same thread, but never while holding a native lock.
std::recursive_mutex g_sessionCallbackMutex;
unsigned g_teardownDepth{}; // only the validated native UI thread
std::atomic<bool> g_coordinatorStartAttempted{false};
std::atomic<game::CPhaseGame*> g_phaseGame{nullptr};
std::atomic<game::CMidServerLogic*> g_serverLogic{nullptr};
// Published once immediately before patch activation. The authoritative sender
// is DirectPlay's reserved DPID_SERVERPLAYER (1), proven from the natural
// startup BeginTurn received by this exact process. A nonzero sender value is
// the release/acquire commit marker for the pair.
std::atomic<std::uint32_t> g_authoritativeSenderDpid{0};
std::atomic<std::uint32_t> g_localReceiverDpid{0};
std::mutex g_startupIdentityMutex;
std::uint32_t g_startupBeginTurnServerDpid = 0;
std::uint32_t g_startupBeginTurnReceiverDpid = 0;
std::uint32_t g_startupBeginTurnActiveHandle = 0;
std::uint32_t g_startupDirectedAddressee = 0;
StartupTurnProofState g_startupTurnProofState = StartupTurnProofState::Empty;
struct PendingJoinActivation
{
    bool present = false;
    bool continuationQueued = false;
    bool continuationInProgress = false;
    protocol::SessionPlan plan{};
};
PendingJoinActivation g_pendingJoinActivation;

enum class PendingUiApplyKind : std::uint8_t
{
    None,
    ActivateTurn,
    NaturalMerge,
};

// A successful native dispatcher return proves that the exact handler accepted
// the command, while CMidCommandQueue2's empty boundary proves that every
// command it queued has finished. Either event can occur first (the queue drain
// can be reentrant), so relay-visible completion requires both independent
// latches for one immutable relay action.
struct PendingUiApply
{
    PendingUiApplyKind kind{PendingUiApplyKind::None};
    std::uint32_t actionId{};
    std::uint32_t handle{};
    std::uint32_t day{};
    protocol::EngineAction action{};
    bool dispatchSucceeded{};
    bool queueDrained{};
    bool completionClaimed{};
};

enum class PendingUiLatchResult : std::uint8_t
{
    Invalid,
    Waiting,
    CompletionClaimed,
};

constexpr bool currentPlayerIsNegotiated(std::uint32_t currentHandle,
                                         std::uint32_t localPlayerHandle,
                                         std::uint32_t otherPlayerHandle)
{
    // Russobit's BeginTurn paths run independently: sub_417BD3 changes
    // the CMidGame active-player node while sub_4078E4 synchronizes the
    // CPhaseGame currentPlayerId. Their notification order differs by the
    // native route, so a queue-drained turn message can leave
    // either negotiated human in this diagnostic field.  No zero, duplicate,
    // neutral, or third-party handle belongs to the exact two-player session.
    return currentHandle != 0 && localPlayerHandle != 0 && otherPlayerHandle != 0
           && localPlayerHandle != otherPlayerHandle
           && (currentHandle == localPlayerHandle
               || currentHandle == otherPlayerHandle);
}

static_assert(currentPlayerIsNegotiated(0xa3de0001u, 0xa3de0001u,
                                        0xa3de0002u));
static_assert(currentPlayerIsNegotiated(0xa3de0002u, 0xa3de0001u,
                                        0xa3de0002u));
static_assert(!currentPlayerIsNegotiated(0, 0xa3de0001u, 0xa3de0002u));
static_assert(!currentPlayerIsNegotiated(0xa3de0003u, 0xa3de0001u,
                                         0xa3de0002u));
static_assert(!currentPlayerIsNegotiated(0xa3de0001u, 0xa3de0001u,
                                         0xa3de0001u));

constexpr bool naturalMergeCurrentMatchesRole(bool hostRole,
                                              std::uint32_t currentHandle,
                                              std::uint32_t localPlayerHandle,
                                              std::uint32_t hostPlayerHandle)
{
    // The host's local CPhase task (sub_4078E4) mirrors the active host into
    // currentPlayerId.  On the join, the natural netplay handler sub_418806
    // updates CMidClientCore and publishes its notification without calling
    // sub_40620F, so this role-local diagnostic field can retain either member
    // of the negotiated pair.  The exact stock packet, its host handle, the
    // successful original dispatch, and the queue drain remain the causal
    // ownership proof; this predicate only rejects an impossible CPhase view.
    if (hostRole) {
        return localPlayerHandle != 0 && localPlayerHandle == hostPlayerHandle
               && currentHandle == hostPlayerHandle;
    }
    return localPlayerHandle != hostPlayerHandle
           && currentPlayerIsNegotiated(currentHandle, localPlayerHandle,
                                        hostPlayerHandle);
}

static_assert(naturalMergeCurrentMatchesRole(true, 0xa3de0001u,
                                             0xa3de0001u, 0xa3de0001u));
static_assert(!naturalMergeCurrentMatchesRole(true, 0xa3de0002u,
                                              0xa3de0001u, 0xa3de0001u));
static_assert(!naturalMergeCurrentMatchesRole(true, 0xa3de0001u,
                                              0xa3de0002u, 0xa3de0001u));
static_assert(naturalMergeCurrentMatchesRole(false, 0xa3de0002u,
                                             0xa3de0002u, 0xa3de0001u));
static_assert(naturalMergeCurrentMatchesRole(false, 0xa3de0001u,
                                             0xa3de0002u, 0xa3de0001u));
static_assert(!naturalMergeCurrentMatchesRole(false, 0,
                                              0xa3de0002u, 0xa3de0001u));
static_assert(!naturalMergeCurrentMatchesRole(false, 0xa3de0003u,
                                              0xa3de0002u, 0xa3de0001u));
static_assert(!naturalMergeCurrentMatchesRole(false, 0xa3de0001u,
                                              0xa3de0001u, 0xa3de0001u));

std::mutex g_pendingUiApplyMutex;
PendingUiApply g_pendingUiApply;
std::atomic<std::uint32_t> g_mergeActionId{0};
std::atomic<std::uint32_t> g_mergeDay{0};
std::atomic<bool> g_executeStarted{false};
std::atomic<bool> g_executeResultQueued{false};
std::atomic<bool> g_naturalMergeReady{false};
std::atomic<bool> g_mergeAppliedQueued{false};
protocol::EngineAction g_executeMergeAction;
std::atomic<bool> g_hostEndTurnRxClaimed{false};
std::atomic<bool> g_joinEndTurnRxClaimed{false};
std::atomic<JoinBootstrap> g_joinBootstrap{JoinBootstrap::Dormant};
std::atomic<HostBootstrap> g_hostBootstrap{HostBootstrap::Dormant};
// BootstrapOperational proves that this native client can receive/process the
// peer's first released actions. BootstrapReleased separately opens only this
// client's local strategic TX, after the relay has both prepare ACKs.
std::atomic<bool> g_bootstrapPrepared{false};
std::atomic<bool> g_bootstrapReleased{false};
// The first-leader naming dialog is part of stock TurnInfo completion. Its
// BTN_CLOSE sends one CStackChangeLeaderNameMsg synchronously before that
// command queue can drain, so BootstrapReleased cannot be its prerequisite.
// An exact own-player TurnInfo post-dispatch event arms this single-use token.
// On the host that RX precedes strategic UI/local-handle publication, so the
// already-latched authoritative BeginTurn tuple is its pre-bind identity proof.
std::atomic<StartupLeaderNameTx> g_startupLeaderNameTx{
    StartupLeaderNameTx::Dormant};
std::atomic<std::uint32_t> g_startupLeaderNameExpectedHandle{0};
std::atomic<std::uint32_t> g_startupLeaderNameStackId{0};
std::atomic<std::uint32_t> g_startupLeaderNameFrameLength{0};
std::atomic<std::uint32_t> g_startupLeaderNameByteCount{0};
DetourTargets g_productionTargets;

void resumePendingJoinActivationOnUi(void*);
void completeClaimedPendingUiApply(const PendingUiApply& pending);
void completeStartupLeaderNameAfterSend(std::uint32_t stackId, int sendResult);

struct CoordinatorEventContext
{
    std::uint64_t generation;
    CoordinatorEvent event;
};

struct CoordinatorFaultContext
{
    std::uint64_t generation;
    CoordinatorTerminalFault fault;
};

bool currentSession(std::uint64_t generation)
{
    return g_sessionActive.load(std::memory_order_acquire)
           && !g_tearingDown.load(std::memory_order_acquire)
           && generation == g_sessionGeneration.load(std::memory_order_acquire);
}

template <typename Context>
void discardUiContext(void* context)
{
    delete static_cast<Context*>(context);
}

bool queueJoinActivation()
{
    auto* generation = new (std::nothrow) std::uint64_t(
        g_sessionGeneration.load(std::memory_order_acquire));
    if (!generation)
        return false;
    if (netintercept::invokeOnUiThread(&resumePendingJoinActivationOnUi, generation,
                                      &discardUiContext<std::uint64_t>)) {
        return true;
    }
    delete generation;
    return false;
}

CoordinatorPort& coordinator()
{
    return CoordinatorPort::processInstance();
}

bool exactStrategicQueueEmpty(game::CPhaseGame* phaseGame,
                              game::CMidObjectLock* objectLock)
{
    if (!phaseGame || !phaseGame->data || !objectLock
        || phaseGame->data->midObjectLock != objectLock)
        return false;
    auto* queue = game::CPhaseApi::get().getCommandQueue(&phaseGame->phase);
    return queue && objectLock->commandQueue == queue && queue->started
           && !queue->processingCommand && queue->commandsList.length == 0
           && objectLock->pendingLocalUpdates == 0;
}

struct SetDayContext
{
    std::uint32_t day;
};

struct CascadeCallContext
{
    game::CMidServerLogic* logic;
    game::NetPlayerInfo* player;
};

template <std::size_t Size>
bool hasExactRtti(const std::uint8_t* payload,
                  std::uint32_t payloadSize,
                  const char (&expected)[Size])
{
    static_assert(Size > 1, "RTTI literal must contain text and a terminator");
    constexpr std::size_t textSize = Size - 1;
    return payload && payloadSize >= Size
           && std::memcmp(payload, expected, textSize) == 0
           && payload[textSize] == 0;
}

bool hasExactRtti(const std::uint8_t* payload,
                  std::uint32_t payloadSize,
                  const ExactRtti& expected)
{
    return payload && expected.storageSize > 1
           && payloadSize >= expected.storageSize
           && std::memcmp(payload, expected.text, expected.storageSize) == 0;
}

bool decodeStartupLeaderNameIntent(const std::uint8_t* payload,
                                   std::uint32_t payloadSize,
                                   game::CMidgardID& stackId,
                                   std::uint32_t& nameByteCount)
{
    if (!hasExactRtti(payload, payloadSize, startupLeaderNameRtti)
        || payloadSize < startupLeaderNameBytesOffset + 1) {
        return false;
    }

    // sub_55CC9F zeroes the complete 44-byte NetMessageHeader before copying
    // typeid(...).raw_name(). The 32-byte RTTI (including NUL) therefore leaves
    // four exact zero padding bytes in messageClassName[36].
    for (std::size_t offset = sizeof(startupLeaderNameRtti);
         offset < netMessageClassNameSize; ++offset) {
        if (payload[offset] != 0)
            return false;
    }

    std::memcpy(&stackId.value, payload + startupLeaderNameStackOffset,
                sizeof(stackId.value));
    std::memcpy(&nameByteCount, payload + startupLeaderNameByteCountOffset,
                sizeof(nameByteCount));
    if (!stackId.value || nameByteCount == 0
        || nameByteCount > startupLeaderNameMaximumBytes
        || payloadSize != startupLeaderNameBytesOffset + nameByteCount) {
        return false;
    }

    const auto* const name = payload + startupLeaderNameBytesOffset;
    if (name[nameByteCount - 1] != 0
        || (nameByteCount > 1
            && std::memchr(name, 0, nameByteCount - 1) != nullptr)) {
        return false;
    }
    return true;
}

constexpr TurnAnnouncementLayout classifyTurnAnnouncement(std::uint32_t word36,
                                                           std::uint32_t word40,
                                                           std::uint32_t word44)
{
    if (word40 == 0xffffffffu)
        return word36 && word44 ? TurnAnnouncementLayout::Directed
                                : TurnAnnouncementLayout::Invalid;
    return !word36 && word44 ? TurnAnnouncementLayout::Broadcast
                             : TurnAnnouncementLayout::Invalid;
}

bool decodeTurnAnnouncement(const std::uint8_t* payload,
                            std::uint32_t payloadSize,
                            TurnAnnouncement& result)
{
    if (!payload || payloadSize < 48)
        return false;

    std::uint32_t word36 = 0;
    std::uint32_t word40 = 0;
    std::uint32_t word44 = 0;
    std::memcpy(&word36, payload + 36, sizeof(word36));
    std::memcpy(&word40, payload + 40, sizeof(word40));
    std::memcpy(&word44, payload + 44, sizeof(word44));
    const TurnAnnouncementLayout layout =
        classifyTurnAnnouncement(word36, word40, word44);
    if (layout == TurnAnnouncementLayout::Invalid)
        return false;

    result.layout = layout;
    result.addressee = word36;
    result.activeHandle = word44;
    result.sequence = word40;
    return true;
}

bool decodeTurnInfoActive(const std::uint8_t* payload,
                          std::uint32_t payloadSize,
                          std::uint32_t& activeHandle)
{
    // TurnInfo is not a BeginTurn layout. Exact Russobit captures place the
    // active handle at the unaligned +50 field, with the directed sentinel at
    // +40 and DPID_SERVERPLAYER at +44.
    if (!payload || payloadSize < 54)
        return false;
    std::uint32_t sentinel = 0;
    std::uint32_t serverDpid = 0;
    std::memcpy(&sentinel, payload + 40, sizeof(sentinel));
    std::memcpy(&serverDpid, payload + 44, sizeof(serverDpid));
    std::memcpy(&activeHandle, payload + 50, sizeof(activeHandle));
    return sentinel == 0xffffffffu && serverDpid == game::serverNetPlayerId
           && activeHandle != 0;
}

static_assert(classifyTurnAnnouncement(0, 1, 0xa3de0001u)
              == TurnAnnouncementLayout::Broadcast);
static_assert(classifyTurnAnnouncement(0xa3de0002u, 0xffffffffu, 0xa3de0001u)
              == TurnAnnouncementLayout::Directed);
static_assert(classifyTurnAnnouncement(0, 0xffffffffu, 0xa3de0001u)
              == TurnAnnouncementLayout::Invalid);
static_assert(classifyTurnAnnouncement(0xa3de0002u, 1, 0xa3de0001u)
              == TurnAnnouncementLayout::Invalid);

constexpr bool isExactStockHandoff(TurnAnnouncementLayout layout,
                                   std::uint32_t activeHandle,
                                   std::uint32_t sequence,
                                   std::uint32_t frameLength,
                                   std::uint32_t expectedHost)
{
    // IDA: CCmdBeginTurnMsg receives only the current player's CMidgardID.
    // CCommandMsg owns +40 as its process-global broadcast sequence; the
    // logical day is committed independently by PrepareMerge/setScenarioDay.
    // Sequence 1 is the already-consumed startup broadcast and UINT32_MAX is
    // the directed-layout sentinel, so neither can release the stock handoff.
    return layout == TurnAnnouncementLayout::Broadcast
           && activeHandle == expectedHost
           && sequence > startupBeginTurnSequence
           && sequence != directedBeginTurnSequence
           && frameLength == syntheticBeginTurnFrameLength;
}

static_assert(isExactStockHandoff(TurnAnnouncementLayout::Broadcast,
                                  0xa3de0001u, 17, 56, 0xa3de0001u));
static_assert(!isExactStockHandoff(TurnAnnouncementLayout::Broadcast,
                                   0xa3de0001u, 1, 56, 0xa3de0001u));
static_assert(!isExactStockHandoff(TurnAnnouncementLayout::Directed,
                                   0xa3de0001u, 17, 56, 0xa3de0001u));
static_assert(!isExactStockHandoff(TurnAnnouncementLayout::Broadcast,
                                   0xa3de0002u, 17, 56, 0xa3de0001u));
static_assert(!isExactStockHandoff(TurnAnnouncementLayout::Broadcast,
                                   0xa3de0001u, 17, 55, 0xa3de0001u));
static_assert(!isExactStockHandoff(TurnAnnouncementLayout::Broadcast,
                                   0xa3de0001u, directedBeginTurnSequence, 56,
                                   0xa3de0001u));

template <std::size_t Size>
bool matchesAnyExactRtti(const std::uint8_t* payload,
                         std::uint32_t payloadSize,
                         const ExactRtti (&expected)[Size])
{
    for (const auto& rtti : expected) {
        if (hasExactRtti(payload, payloadSize, rtti))
            return true;
    }
    return false;
}

bool isStrategicIntent(const std::uint8_t* payload, std::uint32_t payloadSize)
{
    return matchesAnyExactRtti(payload, payloadSize, strategicIntentRtti);
}

bool isStateMutation(const std::uint8_t* payload, std::uint32_t payloadSize)
{
    return isStrategicIntent(payload, payloadSize)
           || matchesAnyExactRtti(payload, payloadSize, authoritativeStateMutationRtti);
}

bool requiresUiReplay(const std::uint8_t* payload, std::uint32_t payloadSize)
{
    return matchesAnyExactRtti(payload, payloadSize, requiresUiReplayRtti);
}

constexpr bool overlayOwnsMutationGate(Phase current)
{
    return current == Phase::Independent || current == Phase::Held
           || current == Phase::Merging || current == Phase::AwaitingStockTurn;
}

static_assert(overlayOwnsMutationGate(Phase::Independent));
static_assert(overlayOwnsMutationGate(Phase::AwaitingStockTurn));
static_assert(!overlayOwnsMutationGate(Phase::WaitingForSession));
static_assert(!overlayOwnsMutationGate(Phase::Ready));
static_assert(!overlayOwnsMutationGate(Phase::Stock));
static_assert(!overlayOwnsMutationGate(Phase::Merged));

bool mergeEntryMatches()
{
    return std::memcmp(reinterpret_cast<const void*>(mergeBeginTurnAddress),
                       mergeBeginTurnPrefix.data(), mergeBeginTurnPrefix.size()) == 0;
}

bool validPlayers(const game::CMidServerLogic* logic)
{
    if (!logic || !logic->coreData || !logic->coreData->players)
        return false;
    const auto* players = logic->coreData->players;
    return players->bgn && players->end && players->end >= players->bgn
           && static_cast<std::size_t>(players->end - players->bgn) <= maximumPlayers;
}

bool hasExactNegotiatedHumanPair(const game::CMidServerLogic* logic,
                                 std::uint32_t expectedHost,
                                 std::uint32_t expectedJoin)
{
    if (!expectedHost || !expectedJoin || expectedHost == expectedJoin
        || !validPlayers(logic)) {
        return false;
    }

    std::size_t humanCount = 0;
    bool foundHost = false;
    bool foundJoin = false;
    const auto* const players = logic->coreData->players;
    for (const auto* player = players->bgn; player != players->end; ++player) {
        if (!player->controlledByHuman)
            continue;

        ++humanCount;
        const std::uint32_t handle =
            static_cast<std::uint32_t>(player->playerId.value);
        if (handle == expectedHost) {
            if (foundHost)
                return false;
            foundHost = true;
        } else if (handle == expectedJoin) {
            if (foundJoin)
                return false;
            foundJoin = true;
        } else {
            // Protocol v2 models exactly one host and one joiner. Never let an
            // unnegotiated human inherit the virtual-current-player bypass.
            return false;
        }
    }
    return humanCount == 2 && foundHost && foundJoin;
}

game::NetPlayerInfo* findPlayer(game::CMidServerLogic* logic,
                                std::uint32_t handle,
                                int* index = nullptr)
{
    if (!handle || !validPlayers(logic))
        return nullptr;

    auto* const players = logic->coreData->players;
    for (auto* player = players->bgn; player != players->end; ++player) {
        if (static_cast<std::uint32_t>(player->playerId.value) == handle) {
            if (index)
                *index = static_cast<int>(player - players->bgn);
            return player;
        }
    }
    return nullptr;
}

game::NetPlayerInfo* findPlayerByNetId(game::CMidServerLogic* logic,
                                       std::uint32_t playerNetId)
{
    if (!playerNetId || !validPlayers(logic))
        return nullptr;

    game::NetPlayerInfo* match = nullptr;
    auto* const players = logic->coreData->players;
    for (auto* player = players->bgn; player != players->end; ++player) {
        if (player->playerNetId != playerNetId)
            continue;
        if (match)
            return nullptr;
        match = player;
    }
    return match;
}

constexpr bool isDynamicPlayerDpid(std::uint32_t dpid) noexcept
{
    return dpid > game::serverNetPlayerId && dpid != game::singleNetPlayerId
           && dpid != UINT32_MAX;
}

constexpr bool isExactAuthoritativeTurnPath(std::uint32_t idFrom,
                                            std::uint32_t playerNetId) noexcept
{
    return idFrom == game::serverNetPlayerId && isDynamicPlayerDpid(playerNetId);
}

static_assert(isExactAuthoritativeTurnPath(game::serverNetPlayerId, 2));
static_assert(!isExactAuthoritativeTurnPath(2, 2));
static_assert(!isExactAuthoritativeTurnPath(game::serverNetPlayerId,
                                            game::serverNetPlayerId));
static_assert(!isExactAuthoritativeTurnPath(game::serverNetPlayerId,
                                            game::singleNetPlayerId));

bool isCurrentNetworkServerLogic(game::CMidServerLogic* logic);

bool resolveExactLocalReceiverDpid(std::uint32_t& result)
{
    auto* midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data || !midgard->data->multiplayerGame
        || midgard->data->hotseatGame || !midgard->data->netPlayerClientPtr) {
        return false;
    }

    auto* client = midgard->data->netPlayerClientPtr->first.data;
    if (!client || !client->vftable || !client->vftable->getNetId)
        return false;

    const int rawDpid = client->vftable->getNetId(
        reinterpret_cast<game::IMqNetPlayer*>(client));
    const std::uint32_t dpid = static_cast<std::uint32_t>(rawDpid);
    if (!isDynamicPlayerDpid(dpid))
        return false;
    result = dpid;
    return true;
}

bool recordExactStartupBeginTurnProof(std::uint32_t idFrom,
                                      std::uint32_t playerNetId,
                                      const TurnAnnouncement& announcement)
{
    if (!isExactAuthoritativeTurnPath(idFrom, playerNetId)
        || announcement.layout != TurnAnnouncementLayout::Broadcast
        || announcement.sequence != startupBeginTurnSequence
        || !announcement.activeHandle) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
    if (g_startupTurnProofState != StartupTurnProofState::Empty
        || g_startupBeginTurnServerDpid || g_startupBeginTurnReceiverDpid
        || g_startupBeginTurnActiveHandle || g_startupDirectedAddressee) {
        g_startupTurnProofState = StartupTurnProofState::Conflicted;
        return false;
    }

    g_startupBeginTurnServerDpid = idFrom;
    g_startupBeginTurnReceiverDpid = playerNetId;
    g_startupBeginTurnActiveHandle = announcement.activeHandle;
    g_startupTurnProofState = StartupTurnProofState::BroadcastLatched;
    spdlog::info(
        "[simturns] latched exact natural startup BeginTurn broadcast (serverSender={}, localReceiver={}, active={:#x})",
        idFrom, playerNetId, announcement.activeHandle);
    return true;
}

bool recordExactPreBindDirectedBeginTurn(
    std::uint32_t idFrom,
    std::uint32_t playerNetId,
    const TurnAnnouncement& announcement,
    bool& queueJoinContinuation)
{
    queueJoinContinuation = false;
    if (isHost() || !isExactAuthoritativeTurnPath(idFrom, playerNetId)
        || announcement.layout != TurnAnnouncementLayout::Directed
        || announcement.sequence != UINT32_MAX) {
        return false;
    }

    const std::uint32_t publishedHandle = localHandle();
    if (publishedHandle && announcement.addressee != publishedHandle)
        return false;

    // Every retained Russobit capture has exactly this ordering: one startup
    // broadcast {0, 1, hostHandle}, followed by the stock directed activation
    // {joinHandle, UINT32_MAX, hostHandle}. The directed addressee is latched
    // here and is checked against SessionPlan/localHandle before binding. It
    // passes to the original handler; the only side effect here is completing
    // the exact two-event identity proof and, if needed, owning one continuation.
    std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
    if (g_startupTurnProofState != StartupTurnProofState::BroadcastLatched
        || g_startupDirectedAddressee
        || g_startupBeginTurnServerDpid != idFrom
        || g_startupBeginTurnReceiverDpid != playerNetId
        || g_startupBeginTurnActiveHandle != announcement.activeHandle) {
        g_startupTurnProofState = StartupTurnProofState::Conflicted;
        return false;
    }

    g_startupDirectedAddressee = announcement.addressee;
    g_startupTurnProofState = StartupTurnProofState::DirectedLatched;
    spdlog::info(
        "[simturns] latched exact natural directed BeginTurn (serverSender={}, localReceiver={}, addressee={:#x}, active={:#x})",
        idFrom, playerNetId, announcement.addressee, announcement.activeHandle);
    if (g_pendingJoinActivation.present
        && !g_pendingJoinActivation.continuationQueued
        && !g_pendingJoinActivation.continuationInProgress) {
        g_pendingJoinActivation.continuationQueued = true;
        queueJoinContinuation = true;
    }
    return true;
}

NetworkIdentityBindResult bindExactNetworkIdentities(
    const protocol::SessionPlan& plan)
{
    spdlog::info("[simturns] entering exact network identity bind (role={})",
                 isHost() ? "host" : "join");
    if (g_authoritativeSenderDpid.load(std::memory_order_acquire)
        || g_localReceiverDpid.load(std::memory_order_acquire)) {
        return NetworkIdentityBindResult::Invalid;
    }

    std::uint32_t localDpid = 0;
    if (!resolveExactLocalReceiverDpid(localDpid))
        return NetworkIdentityBindResult::Invalid;
    spdlog::info("[simturns] resolved typed local receiver DPID {}", localDpid);

    if (isHost()) {
        auto* logic = g_serverLogic.load(std::memory_order_acquire);
        auto* host = findPlayer(logic, plan.hostHandle);
        auto* join = findPlayer(logic, plan.joinHandle);
        if (!isCurrentNetworkServerLogic(logic)
            || !hasExactNegotiatedHumanPair(logic, plan.hostHandle,
                                            plan.joinHandle)
            || !host || !join || !host->controlledByHuman
            || !join->controlledByHuman
            || !isDynamicPlayerDpid(host->playerNetId)
            || !isDynamicPlayerDpid(join->playerNetId)
            || host->playerNetId == join->playerNetId
            || localDpid != host->playerNetId) {
            return NetworkIdentityBindResult::Invalid;
        }
        spdlog::info("[simturns] host negotiated-human DPID pair validated");
    }

    std::uint32_t authoritativeDpid = 0;
    {
        // DPID_SERVERPLAYER is the authoritative sender of CCmd* traffic, not
        // the host player's dynamic receive DPID. Run 75dc5eff observed the
        // natural startup CCmdBeginTurnMsg from 1 on both host and join. We do
        // not substitute that constant: the exact native event must have
        // proved the same sender, this process's receiver, and the negotiated
        // host handle before any production patch is activated.
        std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
        if (g_startupTurnProofState == StartupTurnProofState::Conflicted)
            return NetworkIdentityBindResult::Invalid;
        const StartupTurnProofState requiredState =
            isHost() ? StartupTurnProofState::BroadcastLatched
                     : StartupTurnProofState::DirectedLatched;
        if (g_startupTurnProofState != requiredState) {
            if (!isHost()
                && (g_startupTurnProofState == StartupTurnProofState::Empty
                    || g_startupTurnProofState
                           == StartupTurnProofState::BroadcastLatched)) {
                return NetworkIdentityBindResult::WaitingForNaturalJoinTurnProof;
            }
            return NetworkIdentityBindResult::Invalid;
        }
        if (g_startupBeginTurnServerDpid != game::serverNetPlayerId
            || g_startupBeginTurnReceiverDpid != localDpid
            || g_startupBeginTurnActiveHandle != plan.hostHandle
            || (isHost() && g_startupDirectedAddressee)
            || (!isHost()
                && (g_startupDirectedAddressee != plan.joinHandle
                    || g_startupDirectedAddressee != localHandle()))) {
            return NetworkIdentityBindResult::Invalid;
        }
        authoritativeDpid = g_startupBeginTurnServerDpid;
    }

    // Publish the local receiver first; the authoritative server sender is the
    // release commit marker consumed by RX validation and synthetic injection.
    g_localReceiverDpid.store(localDpid, std::memory_order_relaxed);
    g_authoritativeSenderDpid.store(authoritativeDpid, std::memory_order_release);
    spdlog::info(
        "[simturns] exact network identities bound (serverSender={}, localReceiver={})",
        authoritativeDpid, localDpid);
    return NetworkIdentityBindResult::Bound;
}

std::atomic<bool>* endTurnRxClaimFor(std::uint32_t handle)
{
    if (handle == hostHandle())
        return &g_hostEndTurnRxClaimed;
    if (handle == joinHandle())
        return &g_joinEndTurnRxClaimed;
    return nullptr;
}

bool isCurrentNetworkServerLogic(game::CMidServerLogic* logic)
{
    if (!validPlayers(logic) || !logic->coreData->objectMap
        || !logic->coreData->multiplayerGame || logic->coreData->hotseatGame) {
        return false;
    }

    auto* midgard = game::CMidgardApi::get().instance();
    return midgard && midgard->data && midgard->data->server
           && midgard->data->server->data
           && midgard->data->server->data->serverLogic == logic;
}

game::IMidgardObjectMap* clientObjectMap()
{
    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    if (!phaseGame || !phaseGame->data || !phaseGame->data->midClient)
        return nullptr;
    return game::CMidClientCoreApi::get().getObjectMap(&phaseGame->data->midClient->core);
}

const char* startupBootstrapSubstate()
{
    if (isHost()) {
        switch (g_hostBootstrap.load(std::memory_order_acquire)) {
        case HostBootstrap::Dormant:
            return "Dormant";
        case HostBootstrap::AwaitingCascade:
            return "AwaitingCascade";
        case HostBootstrap::CascadeRunning:
            return "CascadeRunning";
        case HostBootstrap::AwaitingCommit:
            return "AwaitingCommit";
        case HostBootstrap::CommitApplied:
            return "CommitApplied";
        case HostBootstrap::OperationalApplied:
            return "OperationalApplied";
        case HostBootstrap::Complete:
            return "Complete";
        case HostBootstrap::Failed:
            return "Failed";
        }
        return "InvalidHostBootstrap";
    }

    switch (g_joinBootstrap.load(std::memory_order_acquire)) {
    case JoinBootstrap::Dormant:
        return "Dormant";
    case JoinBootstrap::AwaitingStartupDrain:
        return "AwaitingStartupDrain";
    case JoinBootstrap::BeginTurnQueued:
        return "BeginTurnQueued";
    case JoinBootstrap::AwaitingOwnTurnInfo:
        return "AwaitingOwnTurnInfo";
    case JoinBootstrap::OwnTurnInfoQueued:
        return "OwnTurnInfoQueued";
    case JoinBootstrap::AwaitingCommit:
        return "AwaitingCommit";
    case JoinBootstrap::CommitApplied:
        return "CommitApplied";
    case JoinBootstrap::OperationalApplied:
        return "OperationalApplied";
    case JoinBootstrap::Complete:
        return "Complete";
    case JoinBootstrap::Failed:
        return "Failed";
    }
    return "InvalidJoinBootstrap";
}

bool startupLeaderNameBootstrapStateIsEligible(Phase current)
{
    if (isHost()) {
        const HostBootstrap bootstrap =
            g_hostBootstrap.load(std::memory_order_acquire);
        if (current == Phase::Prepared || current == Phase::WaitingForSession
            || current == Phase::Ready) {
            return bootstrap == HostBootstrap::Dormant;
        }
        if (current != Phase::Independent)
            return false;
        return bootstrap == HostBootstrap::Dormant
               || bootstrap == HostBootstrap::AwaitingCascade
               || bootstrap == HostBootstrap::CascadeRunning
               || bootstrap == HostBootstrap::AwaitingCommit
               || bootstrap == HostBootstrap::CommitApplied
               || bootstrap == HostBootstrap::OperationalApplied
               || bootstrap == HostBootstrap::Complete;
    }

    if (current != Phase::Independent)
        return false;
    const JoinBootstrap bootstrap =
        g_joinBootstrap.load(std::memory_order_acquire);
    return bootstrap == JoinBootstrap::OwnTurnInfoQueued
           || bootstrap == JoinBootstrap::AwaitingCommit
           || bootstrap == JoinBootstrap::CommitApplied
           || bootstrap == JoinBootstrap::OperationalApplied
           || bootstrap == JoinBootstrap::Complete;
}

bool isExactOwnTurnInfoRoute(std::uint32_t idFrom,
                             std::uint32_t playerNetId,
                             std::uint32_t activeHandle)
{
    const std::uint32_t publishedLocalHandle = localHandle();
    if (!activeHandle
        || (publishedLocalHandle && activeHandle != publishedLocalHandle))
        return false;

    const std::uint32_t authoritativeDpid =
        g_authoritativeSenderDpid.load(std::memory_order_acquire);
    if (authoritativeDpid) {
        return publishedLocalHandle
               && authoritativeDpid == game::serverNetPlayerId
               && idFrom == authoritativeDpid
               && playerNetId
                      == g_localReceiverDpid.load(std::memory_order_relaxed);
    }

    // The host can receive and finish its natural TurnInfo while SessionPlan
    // is still travelling through the local coordinator. Its already-latched
    // startup BeginTurn is the exact pre-bind proof for the same sender,
    // receiver and active player. A joiner never uses this shortcut.
    if (!isHost())
        return false;
    std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
    return g_startupTurnProofState == StartupTurnProofState::BroadcastLatched
           && !g_startupDirectedAddressee
           && g_startupBeginTurnServerDpid == game::serverNetPlayerId
           && idFrom == g_startupBeginTurnServerDpid
           && playerNetId == g_startupBeginTurnReceiverDpid
           && activeHandle == g_startupBeginTurnActiveHandle;
}

bool isExactPreBindHostTurnInfoActive(std::uint32_t activeHandle)
{
    if (!isHost() || !activeHandle || localHandle()
        || g_authoritativeSenderDpid.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
    return g_startupTurnProofState == StartupTurnProofState::BroadcastLatched
           && !g_startupDirectedAddressee
           && g_startupBeginTurnServerDpid == game::serverNetPlayerId
           && isDynamicPlayerDpid(g_startupBeginTurnReceiverDpid)
           && activeHandle == g_startupBeginTurnActiveHandle;
}

bool armStartupLeaderNameFromOwnTurnInfo(std::uint32_t activeHandle)
{
    const Phase current = phase();
    if (g_bootstrapReleased.load(std::memory_order_acquire))
        return true;
    std::uint32_t publishedLocalHandle = localHandle();
    bool exactActiveHandle = publishedLocalHandle
                                 ? activeHandle == publishedLocalHandle
                                 : isExactPreBindHostTurnInfoActive(activeHandle);
    // onPhaseGame can publish the typed handle between the first load and the
    // pre-bind proof's own zero-handle check. Resolve that ordering by reading
    // the one-shot typed value again; this observes state, not the TurnInfo a
    // second time.
    if (!exactActiveHandle && !publishedLocalHandle) {
        publishedLocalHandle = localHandle();
        exactActiveHandle = publishedLocalHandle
                            && activeHandle == publishedLocalHandle;
    }
    if (!exactActiveHandle
        || !startupLeaderNameBootstrapStateIsEligible(current)) {
        fault("own TurnInfo could not arm the exact startup leader-name TX");
        return false;
    }

    std::uint32_t emptyExpectedHandle = 0;
    if (!g_startupLeaderNameExpectedHandle.compare_exchange_strong(
            emptyExpectedHandle, activeHandle, std::memory_order_acq_rel)) {
        fault("own TurnInfo attempted to claim a second startup leader handle");
        return false;
    }
    // The reverse ordering is also legal: onPhaseGame may publish immediately
    // after the pre-bind proof but before this claim. Its mirrored check sees
    // our claim if it wins; this acquire load sees its publication otherwise.
    const std::uint32_t committedLocalHandle = localHandle();
    if (committedLocalHandle && committedLocalHandle != activeHandle) {
        fault("pre-bind own TurnInfo claim disagrees with the published local player");
        return false;
    }
    StartupLeaderNameTx expected = StartupLeaderNameTx::Dormant;
    if (!g_startupLeaderNameTx.compare_exchange_strong(
            expected, StartupLeaderNameTx::Expected,
            std::memory_order_acq_rel)) {
        fault("own TurnInfo attempted to arm startup leader-name TX more than once");
        return false;
    }
    spdlog::info(
        "[simturns] own TurnInfo armed one startup leader-name TX (role={}, active={:#x}, bootstrap={})",
        isHost() ? "host" : "join", activeHandle,
        startupBootstrapSubstate());
    return true;
}

void completeHostStartupTurnInfoAfterDispatch(std::uint32_t activeHandle)
{
    if (!isHost()
        || !armStartupLeaderNameFromOwnTurnInfo(activeHandle)) {
        if (!isFaulted())
            fault("post-dispatch startup TurnInfo reached an invalid host session");
    }
}

bool consumeStartupLeaderNameIntent(const game::NetMessageHeader* message,
                                    const std::uint8_t* payload,
                                    std::uint32_t payloadSize)
{
    game::CMidgardID stackId{};
    std::uint32_t nameByteCount = 0;
    if (!message
        || !decodeStartupLeaderNameIntent(payload, payloadSize, stackId,
                                          nameByteCount)) {
        fault("startup CStackChangeLeaderNameMsg violated its exact wire layout");
        return false;
    }

    const DWORD uiThread = netintercept::mainThreadId();
    if (!uiThread || GetCurrentThreadId() != uiThread
        || netintercept::recvDispatchDepth() != 0) {
        fault("startup leader-name TX did not originate at the exact UI boundary");
        return false;
    }

    const Phase current = phase();
    const std::uint32_t publishedLocalHandle = localHandle();
    const std::uint32_t expectedActiveHandle =
        g_startupLeaderNameExpectedHandle.load(std::memory_order_acquire);
    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    auto* objectMap = clientObjectMap();
    const auto* scenarioInfo = objectMap ? getScenarioInfo(objectMap) : nullptr;
    auto* stack = objectMap ? getStack(objectMap, &stackId) : nullptr;
    if (!startupLeaderNameBootstrapStateIsEligible(current)
        || !publishedLocalHandle
        || expectedActiveHandle != publishedLocalHandle
        || !phaseGame || !phaseGame->data
        || static_cast<std::uint32_t>(phaseGame->data->currentPlayerId.value)
               != publishedLocalHandle
        || !scenarioInfo || scenarioInfo->currentTurn != 1 || !stack
        || static_cast<std::uint32_t>(stack->ownerId.value)
               != publishedLocalHandle
        || !stack->leaderAlive || !stack->leaderId.value) {
        fault("startup leader-name TX did not resolve to the live local day-1 leader");
        return false;
    }

    if (current != Phase::Prepared && current != Phase::WaitingForSession) {
        TurnGrant grant;
        if (!resolveTurnGrant(publishedLocalHandle, grant)
            || grant.day != 1) {
            fault("startup leader-name TX escaped its relay-issued day-1 grant");
            return false;
        }
    }
    if (coordinator().endTurnPending()) {
        fault("startup leader-name TX arrived after a subjective EndTurn claim");
        return false;
    }

    StartupLeaderNameTx expected = StartupLeaderNameTx::Expected;
    if (!g_startupLeaderNameTx.compare_exchange_strong(
            expected, StartupLeaderNameTx::InFlight,
            std::memory_order_acq_rel)) {
        fault("startup leader-name TX was absent, duplicated, or out of order");
        return false;
    }

    g_startupLeaderNameStackId.store(
        static_cast<std::uint32_t>(stackId.value), std::memory_order_relaxed);
    g_startupLeaderNameFrameLength.store(message->length,
                                         std::memory_order_relaxed);
    g_startupLeaderNameByteCount.store(nameByteCount,
                                       std::memory_order_release);
    if (!netintercept::armCurrentTxCompletion(
            &completeStartupLeaderNameAfterSend,
            static_cast<std::uint32_t>(stackId.value))) {
        fault("could not arm exact post-Send startup leader-name completion");
        return false;
    }

    spdlog::info(
        "[simturns] validated one in-flight startup leader-name TX (role={}, frame={}, nameBytes={}, stack={:#x}, bootstrap={})",
        isHost() ? "host" : "join", message->length, nameByteCount,
        static_cast<std::uint32_t>(stackId.value),
        startupBootstrapSubstate());
    return true;
}

std::uintptr_t setScenarioDayCallback(void* context, game::CScenarioInfo* scenarioInfo)
{
    auto* setDay = static_cast<SetDayContext*>(context);
    if (!setDay || !scenarioInfo || !setDay->day)
        return 0;
    scenarioInfo->currentTurn = static_cast<int>(setDay->day);
    return 1;
}

bool setScenarioDay(game::IMidgardObjectMap* objectMap, std::uint32_t day)
{
    SetDayContext context{day};
    return runSerializedCurrentTurn(objectMap, nullptr, &setScenarioDayCallback, &context) != 0;
}

bool injectBeginTurn(std::uint32_t activeHandle, std::uint32_t day)
{
    const std::uint32_t addressee = localHandle();
    const std::uint32_t authoritativeSenderDpid =
        g_authoritativeSenderDpid.load(std::memory_order_acquire);
    const std::uint32_t localReceiverDpid =
        g_localReceiverDpid.load(std::memory_order_acquire);
    if (!addressee || !activeHandle || !day
        || authoritativeSenderDpid != game::serverNetPlayerId
        || !isDynamicPlayerDpid(localReceiverDpid)) {
        return false;
    }

    // Exact Russobit captures distinguish ordinary broadcasts from the
    // directed activation/resync used when one client finishes loading:
    //   broadcast: {zero,+36; global sequence,+40; active player,+44}
    //   directed:  {addressee,+36; 0xffffffff,+40; active player,+44}
    // A synthetic broadcast carrying logical day 1 is stale after startup's
    // global CCommandMsg sequence has already reached 2. Directed messages
    // have a non-empty CCommandMsg::playerId and therefore follow the stock
    // per-client path without poisoning or racing that process-global counter.
    // The logical day itself is committed through setScenarioDay immediately
    // before every injection; it is deliberately not encoded as broadcast seq.
    std::array<std::uint8_t, syntheticBeginTurnPayloadSize> payload{};
    static_assert(sizeof(beginTurnRtti) <= syntheticAddresseeOffset,
                  "BeginTurn RTTI must fit before its directed fields");
    std::memcpy(payload.data(), beginTurnRtti, sizeof(beginTurnRtti) - 1);
    std::memcpy(payload.data() + syntheticAddresseeOffset, &addressee,
                sizeof(addressee));
    std::memcpy(payload.data() + syntheticSequenceOffset,
                &directedBeginTurnSequence, sizeof(directedBeginTurnSequence));
    std::memcpy(payload.data() + syntheticActiveHandleOffset, &activeHandle,
                sizeof(activeHandle));
    return netintercept::injectAuthoritativePayloadNow(
        localReceiverDpid, payload.data(),
        static_cast<std::uint32_t>(payload.size()));
}

std::uintptr_t invokeCascade(void* context, game::CScenarioInfo*)
{
    auto* call = static_cast<CascadeCallContext*>(context);
    auto beginTurn = game::gameFunctions().midServerLogicDataBeginTurn;
    if (!call || !call->logic || !call->player || !beginTurn)
        return 0;

    // The real vector-owned CMidgardID is required here. Passing a temporary
    // integer-shaped id was one of the old proxy's unstable experiments.
    beginTurn(&call->logic->data, &call->player->playerId);
    // Effect hooks execute synchronously inside the native cascade and may
    // terminal-fault the overlay. Never acknowledge a partially applied
    // cascade as successful and never retry it.
    return isFaulted() ? 0 : 1;
}

void faultEngineAction(const protocol::EngineAction& action, const char* reason)
{
    if (action.actionId
        && !coordinator().reportActionResult(action, false)) {
        spdlog::error("[simturns] could not enqueue failed {} result {}",
                      protocol::actionName(action.kind), action.actionId);
    }
    fault(reason);
}

void failPendingCoordinatorEvent(const char* reason)
{
    coordinator().fail(CoordinatorFailureOrigin::UiApply, reason);
    fault(reason);
}

bool claimPendingUiApply(PendingUiApplyKind kind,
                         const protocol::EngineAction& action,
                         std::uint32_t handle,
                         std::uint32_t& actionId)
{
    if (kind == PendingUiApplyKind::None || !action.actionId || !handle
        || !action.day)
        return false;

    std::lock_guard<std::mutex> lock(g_pendingUiApplyMutex);
    if (g_pendingUiApply.kind != PendingUiApplyKind::None) {
        return false;
    }

    actionId = action.actionId;
    g_pendingUiApply.kind = kind;
    g_pendingUiApply.actionId = action.actionId;
    g_pendingUiApply.handle = handle;
    g_pendingUiApply.day = action.day;
    g_pendingUiApply.action = action;
    return true;
}

bool pendingUiApplyActive()
{
    std::lock_guard<std::mutex> lock(g_pendingUiApplyMutex);
    return g_pendingUiApply.kind != PendingUiApplyKind::None;
}

bool cancelPendingUiApply(std::uint32_t actionId)
{
    std::lock_guard<std::mutex> lock(g_pendingUiApplyMutex);
    if (!actionId || g_pendingUiApply.kind == PendingUiApplyKind::None
        || g_pendingUiApply.actionId != actionId) {
        return false;
    }
    g_pendingUiApply = {};
    return true;
}

bool retirePendingUiApply(std::uint32_t actionId)
{
    std::lock_guard<std::mutex> lock(g_pendingUiApplyMutex);
    if (!actionId || g_pendingUiApply.kind == PendingUiApplyKind::None
        || g_pendingUiApply.actionId != actionId
        || !g_pendingUiApply.dispatchSucceeded || !g_pendingUiApply.queueDrained
        || !g_pendingUiApply.completionClaimed) {
        return false;
    }
    g_pendingUiApply = {};
    return true;
}

PendingUiLatchResult latchPendingUiDispatch(std::uint32_t actionId,
                                            PendingUiApply& completion)
{
    std::lock_guard<std::mutex> lock(g_pendingUiApplyMutex);
    if (!actionId || g_pendingUiApply.kind == PendingUiApplyKind::None
        || g_pendingUiApply.actionId != actionId
        || g_pendingUiApply.dispatchSucceeded
        || g_pendingUiApply.completionClaimed) {
        return PendingUiLatchResult::Invalid;
    }

    g_pendingUiApply.dispatchSucceeded = true;
    if (!g_pendingUiApply.queueDrained)
        return PendingUiLatchResult::Waiting;

    g_pendingUiApply.completionClaimed = true;
    completion = g_pendingUiApply;
    return PendingUiLatchResult::CompletionClaimed;
}

PendingUiLatchResult latchPendingUiQueueDrain(PendingUiApply& completion)
{
    std::lock_guard<std::mutex> lock(g_pendingUiApplyMutex);
    if (g_pendingUiApply.kind == PendingUiApplyKind::None)
        return PendingUiLatchResult::Invalid;
    if (g_pendingUiApply.completionClaimed || g_pendingUiApply.queueDrained)
        return PendingUiLatchResult::Waiting;

    g_pendingUiApply.queueDrained = true;
    if (!g_pendingUiApply.dispatchSucceeded)
        return PendingUiLatchResult::Waiting;

    g_pendingUiApply.completionClaimed = true;
    completion = g_pendingUiApply;
    return PendingUiLatchResult::CompletionClaimed;
}

void completePendingUiDispatch(std::uint32_t actionId)
{
    PendingUiApply completion;
    const PendingUiLatchResult result =
        latchPendingUiDispatch(actionId, completion);
    if (result == PendingUiLatchResult::Invalid) {
        cancelPendingUiApply(actionId);
        failPendingCoordinatorEvent("native UI dispatch lost its exact relay action");
        return;
    }
    if (result == PendingUiLatchResult::CompletionClaimed)
        completeClaimedPendingUiApply(completion);
}

void failJoinBootstrap(const char* reason)
{
    g_joinBootstrap.store(JoinBootstrap::Failed, std::memory_order_release);
    fault(reason);
}

using MergeBeginTurn = char(__thiscall*)(game::CMidServerLogic* logic);

bool mergeHost(std::uint32_t mergeDay)
{
    auto* logic = g_serverLogic.load(std::memory_order_acquire);
    if (!isCurrentNetworkServerLogic(logic) || !mergeEntryMatches())
        return false;

    int hostIndex = -1;
    if (!findPlayer(logic, hostHandle(), &hostIndex) || hostIndex < 0)
        return false;

    logic->currentPlayerIndex = hostIndex;
    const char dispatched = reinterpret_cast<MergeBeginTurn>(mergeBeginTurnAddress)(logic);

    // The independent cascade repairs deliberately remain active through the
    // merge-day 0x420FFA call and are restored immediately afterward.
    if (!patches::restoreCascadeRepairs())
        return false;
    if (!dispatched) {
        spdlog::error("[simturns] merge-day 0x420FFA returned failure for day {}", mergeDay);
        return false;
    }
    return true;
}


bool publishMergeAppliedIfReady()
{
    if (!g_naturalMergeReady.load(std::memory_order_acquire)
        || !g_executeResultQueued.load(std::memory_order_acquire)) {
        return true;
    }

    bool expected = false;
    if (!g_mergeAppliedQueued.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return expected;
    }

    const std::uint32_t actionId =
        g_mergeActionId.load(std::memory_order_acquire);
    if (!actionId || !coordinator().reportMergeApplied(actionId)) {
        return false;
    }
    spdlog::info(
        "[simturns] natural merge BeginTurn applied and drained (actionId={}, day={})",
        actionId, g_mergeDay.load(std::memory_order_acquire));
    return true;
}

void handleApplyTurnStart(const protocol::EngineAction& action)
{
    const Phase current = phase();
    const bool bootstrap =
        !g_bootstrapPrepared.load(std::memory_order_acquire)
        && g_hostBootstrap.load(std::memory_order_acquire)
               == HostBootstrap::AwaitingCascade;
    const bool peerCatchUp = current == Phase::Held
                             && action.playerHandle == otherHandle();
    if (!isHost()
        || (!bootstrap && current != Phase::Independent && !peerCatchUp)
        || (bootstrap && (action.playerHandle != joinHandle() || action.day != 1))) {
        faultEngineAction(action,
                          "ApplyTurnStart arrived outside its host execution window");
        return;
    }
    if (!bootstrap
        && !g_bootstrapReleased.load(std::memory_order_acquire)) {
        faultEngineAction(action,
                          "ordinary ApplyTurnStart arrived before bootstrap release");
        return;
    }

    auto* logic = g_serverLogic.load(std::memory_order_acquire);
    auto* player = isCurrentNetworkServerLogic(logic)
                       ? findPlayer(logic, action.playerHandle)
                       : nullptr;
    std::atomic<bool>* const endTurnClaim =
        bootstrap ? nullptr : endTurnRxClaimFor(action.playerHandle);
    if (!player || (!bootstrap
                    && (!endTurnClaim
                        || !endTurnClaim->load(std::memory_order_acquire)))) {
        faultEngineAction(action,
                          "ApplyTurnStart lacks its exact player/EndTurn proof");
        return;
    }
    if (bootstrap) {
        HostBootstrap expected = HostBootstrap::AwaitingCascade;
        if (!g_hostBootstrap.compare_exchange_strong(
                expected, HostBootstrap::CascadeRunning,
                std::memory_order_acq_rel)) {
            faultEngineAction(action, "bootstrap ApplyTurnStart was not exact-once");
            return;
        }
    }

    const TurnGrant grant{action.playerHandle, action.day, action.lease};
    if (!installTurnGrant(grant)) {
        faultEngineAction(action, "could not install relay-issued turn grant");
        return;
    }
    EngineActionContextScope scope{
        ScopedEngineActionContext{action.playerHandle, action.day, action.lease}};
    CascadeCallContext call{logic, player};
    if (!scope.valid()
        || runSerializedCurrentTurn(logic->coreData->objectMap, &action.day,
                                    &invokeCascade, &call) == 0) {
        faultEngineAction(action, "host turn-start cascade failed");
        return;
    }

    if (!bootstrap) {
        bool claimed = true;
        if (!endTurnClaim->compare_exchange_strong(
                claimed, false, std::memory_order_acq_rel)) {
            faultEngineAction(action,
                              "host EndTurn proof changed during ApplyTurnStart");
            return;
        }
    }
    if (!coordinator().reportActionResult(action, true)) {
        fault("could not enqueue successful ApplyTurnStart result");
        return;
    }
    if (bootstrap) {
        HostBootstrap expected = HostBootstrap::CascadeRunning;
        if (!g_hostBootstrap.compare_exchange_strong(
                expected, HostBootstrap::AwaitingCommit,
                std::memory_order_acq_rel)) {
            fault("bootstrap ApplyTurnStart lost its local state");
            return;
        }
    }
    spdlog::info(
        "[simturns] ApplyTurnStart {} completed for handle={:#x}, day={}, lease={}",
        action.actionId, action.playerHandle, action.day, action.lease);
}

void handleActivateTurn(const protocol::EngineAction& action)
{
    if (phase() != Phase::Independent
        || !g_bootstrapReleased.load(std::memory_order_acquire)
        || action.playerHandle != localHandle()) {
        faultEngineAction(action,
                          "ActivateTurn arrived outside the local active session");
        return;
    }
    const TurnGrant grant{action.playerHandle, action.day, action.lease};
    if (!installTurnGrant(grant)
        || !setScenarioDay(clientObjectMap(), action.day)) {
        faultEngineAction(action,
                          "could not install/apply the directed local turn grant");
        return;
    }

    std::uint32_t actionId = 0;
    if (!claimPendingUiApply(PendingUiApplyKind::ActivateTurn, action,
                             action.playerHandle, actionId)) {
        faultEngineAction(action, "ActivateTurn overlapped another UI action");
        return;
    }
    EngineActionContextScope scope{
        ScopedEngineActionContext{action.playerHandle, action.day, action.lease}};
    if (!scope.valid() || !injectBeginTurn(action.playerHandle, action.day)) {
        cancelPendingUiApply(actionId);
        faultEngineAction(action, "could not inject directed ActivateTurn");
        return;
    }
    completePendingUiDispatch(actionId);
    if (isFaulted())
        return;
    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    onCommandQueueDrained(phaseGame && phaseGame->data
                              ? phaseGame->data->midObjectLock
                              : nullptr);
}

void handleHoldInput(const protocol::EngineAction& action)
{
    if (phase() != Phase::Independent
        || action.playerHandle != localHandle()
        || !enterHeld()
        || !setScenarioDay(clientObjectMap(), action.day)) {
        faultEngineAction(action, "invalid HoldInput transition");
        return;
    }
    // Hold is local admission policy only. The relay deliberately does not
    // project the peer's day into this client, so no synthetic BeginTurn is
    // injected and no peer clock is inferred here.
    if (!coordinator().reportActionResult(action, true)) {
        fault("could not enqueue successful HoldInput result");
        return;
    }
    spdlog::info("[simturns] local input held at day {} (actionId={})",
                 action.day, action.actionId);
}

void handlePrepareMerge(const protocol::EngineAction& action)
{
    if (action.playerHandle != hostHandle() || action.day < 2
        || action.day != configuredMergeDay() || pendingUiApplyActive()
        || !beginMerge() || !patches::restoreUiGates()
        || !setScenarioDay(clientObjectMap(), action.day)) {
        faultEngineAction(action, "invalid PrepareMerge transition");
        return;
    }

    std::uint32_t empty = 0;
    if (!g_mergeActionId.compare_exchange_strong(
            empty, action.actionId, std::memory_order_acq_rel)) {
        faultEngineAction(action, "a merge transaction is already installed");
        return;
    }
    g_mergeDay.store(action.day, std::memory_order_release);
    g_executeStarted.store(false, std::memory_order_release);
    g_executeResultQueued.store(!isHost(), std::memory_order_release);
    g_naturalMergeReady.store(false, std::memory_order_release);
    g_mergeAppliedQueued.store(false, std::memory_order_release);
    g_executeMergeAction = action;
    g_executeMergeAction.kind = protocol::EngineActionKind::ExecuteMerge;

    if (!isHost() && !awaitStockTurn(action.day)) {
        faultEngineAction(action, "join could not enter the stock-handoff wait");
        return;
    }
    if (!coordinator().reportActionResult(action, true)) {
        fault("could not enqueue successful PrepareMerge result");
        return;
    }
    spdlog::info("[simturns] prepared merge transaction {} at stock day {}",
                 action.actionId, action.day);
}

void handleExecuteMerge(const protocol::EngineAction& action)
{
    if (!isHost() || phase() != Phase::Merging
        || action.actionId != g_mergeActionId.load(std::memory_order_acquire)
        || action.day != g_mergeDay.load(std::memory_order_acquire)
        || action.playerHandle != hostHandle()
        || g_executeResultQueued.load(std::memory_order_acquire)) {
        faultEngineAction(action, "invalid ExecuteMerge transition");
        return;
    }
    g_executeMergeAction = action;

    auto* logic = g_serverLogic.load(std::memory_order_acquire);
    if (!isCurrentNetworkServerLogic(logic)
        || !setScenarioDay(logic->coreData->objectMap, action.day)) {
        faultEngineAction(action, "could not set the host stock merge day");
        return;
    }
    EngineActionContextScope scope{
        ScopedEngineActionContext{action.playerHandle, action.day, 0}};
    if (!scope.valid()) {
        faultEngineAction(action, "invalid ExecuteMerge engine context");
        return;
    }
    g_executeStarted.store(true, std::memory_order_release);
    if (!mergeHost(action.day)) {
        faultEngineAction(action, "host merge-day 0x420FFA failed");
        return;
    }

    // The local natural BeginTurn can dispatch reentrantly from 0x420FFA.
    // Queue ExecuteMerge's result first, then release a latched MergeApplied.
    if (!coordinator().reportActionResult(action, true)) {
        fault("could not enqueue successful ExecuteMerge result");
        return;
    }
    g_executeResultQueued.store(true, std::memory_order_release);
    if (!publishMergeAppliedIfReady()) {
        fault("could not enqueue the latched natural MergeApplied proof");
        return;
    }
    spdlog::info("[simturns] host executed merge transaction {} via 0x420FFA",
                 action.actionId);
}

void handleReleaseStock(const protocol::EngineAction& action)
{
    const Phase expected = isHost() ? Phase::Merging : Phase::AwaitingStockTurn;
    if (phase() != expected
        || action.actionId != g_mergeActionId.load(std::memory_order_acquire)
        || action.day != g_mergeDay.load(std::memory_order_acquire)
        || !g_mergeAppliedQueued.load(std::memory_order_acquire)
        || !coordinator().acceptStockRelease(action)) {
        failPendingCoordinatorEvent("invalid ReleaseStock transition");
        return;
    }

    resetTurnContext();
    const bool released = isHost() ? finishMerge(action.day) : finishStockTurn();
    if (!released) {
        fault("could not restore the stock turn state");
        return;
    }
    spdlog::info("[simturns] relay released stock turns (actionId={}, day={})",
                 action.actionId, action.day);
}

void handleEngineAction(const protocol::EngineAction& action)
{
    switch (action.kind) {
    case protocol::EngineActionKind::ApplyTurnStart:
        handleApplyTurnStart(action);
        return;
    case protocol::EngineActionKind::ActivateTurn:
        handleActivateTurn(action);
        return;
    case protocol::EngineActionKind::HoldInput:
        handleHoldInput(action);
        return;
    case protocol::EngineActionKind::PrepareMerge:
        handlePrepareMerge(action);
        return;
    case protocol::EngineActionKind::ExecuteMerge:
        handleExecuteMerge(action);
        return;
    case protocol::EngineActionKind::ReleaseStock:
        handleReleaseStock(action);
        return;
    }
    faultEngineAction(action, "unknown relay engine action");
}

void completeClaimedPendingUiApply(const PendingUiApply& pending)
{
    if (pending.kind == PendingUiApplyKind::None || !pending.actionId
        || !pending.handle || !pending.day || !pending.dispatchSucceeded
        || !pending.queueDrained || !pending.completionClaimed) {
        cancelPendingUiApply(pending.actionId);
        failPendingCoordinatorEvent("invalid completed UI action snapshot");
        return;
    }

    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    if (!phaseGame || !phaseGame->data) {
        cancelPendingUiApply(pending.actionId);
        failPendingCoordinatorEvent("queue-drained UI action lost CPhaseGame data");
        return;
    }
    const std::uint32_t currentHandle =
        static_cast<std::uint32_t>(phaseGame->data->currentPlayerId.value);

    if (pending.kind == PendingUiApplyKind::ActivateTurn) {
        if (pending.action.kind != protocol::EngineActionKind::ActivateTurn
            || phase() != Phase::Independent
            || currentHandle != localHandle()) {
            cancelPendingUiApply(pending.actionId);
            faultEngineAction(pending.action,
                              "queue-drained engine action left an invalid UI state");
            return;
        }
    } else if (pending.kind == PendingUiApplyKind::NaturalMerge) {
        const Phase expected = isHost() ? Phase::Merging
                                        : Phase::AwaitingStockTurn;
        if (phase() != expected || pending.handle != hostHandle()
            || pending.day != configuredMergeDay()
            || !naturalMergeCurrentMatchesRole(
                isHost(), currentHandle, localHandle(), hostHandle())) {
            cancelPendingUiApply(pending.actionId);
            failPendingCoordinatorEvent(
                "queue-drained natural BeginTurn left its merge transaction");
            return;
        }
    }

    if (!retirePendingUiApply(pending.actionId)) {
        failPendingCoordinatorEvent("completed UI action could not retire exactly once");
        return;
    }

    if (pending.kind == PendingUiApplyKind::ActivateTurn) {
        if (!coordinator().reportActionResult(pending.action, true)) {
            fault("could not enqueue queue-drained engine ActionResult");
            return;
        }
        spdlog::info("[simturns] {} UI queue drained (actionId={}, day={})",
                     protocol::actionName(pending.action.kind),
                     pending.actionId, pending.day);
        return;
    }

    bool expectedNatural = false;
    if (!g_naturalMergeReady.compare_exchange_strong(
            expectedNatural, true, std::memory_order_acq_rel)
        || !publishMergeAppliedIfReady()) {
        failPendingCoordinatorEvent(
            "natural merge proof was duplicated or could not publish");
    }
}

void completeNaturalStockHandoffDispatch(std::uint32_t actionId)
{
    completePendingUiDispatch(actionId);
    if (isFaulted())
        return;
    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    onCommandQueueDrained(phaseGame && phaseGame->data
                              ? phaseGame->data->midObjectLock
                              : nullptr);
}

void activateSessionAfterIdentityBinding(const protocol::SessionPlan& plan)
{
    if (!patches::activate()) {
        failPendingCoordinatorEvent("simultaneous-turn inline-patch activation failed");
        return;
    }
    if (!activateIndependent()) {
        const bool rolledBack = patches::rollbackAll();
        if (!rolledBack)
            spdlog::critical("[simturns] activation transition rollback was incomplete");
        failPendingCoordinatorEvent("could not enter simultaneous-turn independent phase");
        return;
    }

    if (g_bootstrapPrepared.load(std::memory_order_acquire)
        || g_bootstrapReleased.load(std::memory_order_acquire)) {
        failPendingCoordinatorEvent(
            "SessionPlan attempted to restart an operational bootstrap");
        return;
    }

    if (isHost()) {
        HostBootstrap expected = HostBootstrap::Dormant;
        if (!g_hostBootstrap.compare_exchange_strong(
                expected, HostBootstrap::AwaitingCascade,
                std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent("host bootstrap was armed more than once");
            return;
        }
        if (!coordinator().reportSessionActivated()) {
            g_hostBootstrap.store(HostBootstrap::Failed,
                                  std::memory_order_release);
            fault("could not publish host SessionActivated");
            return;
        }
        spdlog::info(
            "[simturns] host engine session activated; awaiting exact join day-1 cascade event");
        return;
    }

    {
        // SessionPlan can overtake stock startup commands already waiting in
        // CMidCommandQueue2 behind a modal dialog. Do not inject ahead of them:
        // arm one activation and let the existing queue-completion event issue
        // it exactly once at the first proven empty boundary.
        JoinBootstrap expected = JoinBootstrap::Dormant;
        if (!g_joinBootstrap.compare_exchange_strong(
                expected, JoinBootstrap::AwaitingStartupDrain,
                std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent("join bootstrap was armed more than once");
            return;
        }
        if (!coordinator().reportSessionActivated()) {
            g_joinBootstrap.store(JoinBootstrap::Failed,
                                  std::memory_order_release);
            fault("could not publish join SessionActivated");
            return;
        }
        auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
        onCommandQueueDrained(phaseGame && phaseGame->data
                                  ? phaseGame->data->midObjectLock
                                  : nullptr);
        return;
    }
}

void resumePendingJoinActivationOnUi(void* rawGeneration)
{
    std::unique_ptr<std::uint64_t> generation(
        static_cast<std::uint64_t*>(rawGeneration));
    if (!generation || !currentSession(*generation))
        return;
    protocol::SessionPlan plan;
    {
        std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
        if (isHost() || !g_pendingJoinActivation.present
            || g_pendingJoinActivation.continuationInProgress) {
            failPendingCoordinatorEvent(
                "join SessionPlan continuation reached an invalid exact-event state");
            return;
        }
        g_pendingJoinActivation.continuationQueued = false;
        g_pendingJoinActivation.continuationInProgress = true;
        plan = g_pendingJoinActivation.plan;
    }

    const NetworkIdentityBindResult result = bindExactNetworkIdentities(plan);
    if (result == NetworkIdentityBindResult::WaitingForNaturalJoinTurnProof) {
        bool queueContinuation = false;
        {
            std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
            if (!g_pendingJoinActivation.present
                || !g_pendingJoinActivation.continuationInProgress) {
                failPendingCoordinatorEvent(
                    "join SessionPlan wait lost its exact continuation state");
                return;
            }
            g_pendingJoinActivation.continuationInProgress = false;
            // If the natural packet arrived between the bind check and this
            // state publication, schedule its sole continuation now. Otherwise
            // the RX subscriber will schedule it when that event arrives.
            if (g_startupTurnProofState == StartupTurnProofState::DirectedLatched
                && !g_pendingJoinActivation.continuationQueued) {
                g_pendingJoinActivation.continuationQueued = true;
                queueContinuation = true;
            }
        }
        if (queueContinuation && !queueJoinActivation()) {
            failPendingCoordinatorEvent(
                "could not queue the natural BeginTurn SessionPlan continuation");
            return;
        }
        spdlog::info(
            "[simturns] join SessionPlan latched; awaiting the exact natural Broadcast->Directed proof");
        return;
    }
    if (result != NetworkIdentityBindResult::Bound) {
        failPendingCoordinatorEvent(
            "SessionPlan and natural BeginTurn do not prove one exact host/local DPID pair");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
        if (!g_pendingJoinActivation.present
            || !g_pendingJoinActivation.continuationInProgress
            || g_pendingJoinActivation.plan.epoch != plan.epoch
            || g_pendingJoinActivation.plan.hostHandle != plan.hostHandle
            || g_pendingJoinActivation.plan.joinHandle != plan.joinHandle
            || g_pendingJoinActivation.plan.mergeDay != plan.mergeDay) {
            failPendingCoordinatorEvent(
                "join identity bind did not consume its exact SessionPlan event");
            return;
        }
        g_pendingJoinActivation = {};
    }
    activateSessionAfterIdentityBinding(plan);
}

void activateSession(const protocol::SessionPlan& plan)
{
    spdlog::info("[simturns] applying SessionPlan on the UI thread (role={}, mode={})",
                 isHost() ? "host" : "join", static_cast<unsigned>(plan.mode));

    if (plan.mode == TurnMode::Stock) {
        const std::uint32_t expectedLocal = isHost() ? plan.hostHandle
                                                     : plan.joinHandle;
        if (!expectedLocal || expectedLocal != localHandle()) {
            failPendingCoordinatorEvent(
                "stock SessionPlan does not match the local player");
            return;
        }
        if (!authorizeStock()) {
            failPendingCoordinatorEvent(
                "could not apply the relay's stock-turn policy");
            return;
        }
        spdlog::info("[simturns] relay selected stock turns; overlay gates remain inactive");
        return;
    }

    spdlog::info("[simturns] applying simultaneous SessionPlan (role={})",
                 isHost() ? "host" : "join");
    if (!establishSession(plan.hostHandle, plan.joinHandle, plan.mergeDay)
        || !initializeTurnContextFromSessionPlan(
            TurnGrant{plan.hostHandle, 1, plan.hostLease},
            TurnGrant{plan.joinHandle, 1, plan.joinLease})) {
        failPendingCoordinatorEvent(
            "SessionPlan does not match the local engine session");
        return;
    }

    if (isHost()) {
        if (bindExactNetworkIdentities(plan)
            != NetworkIdentityBindResult::Bound) {
            failPendingCoordinatorEvent(
                "host SessionPlan lacks one exact typed host/local DPID binding");
            return;
        }
        activateSessionAfterIdentityBinding(plan);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
        if (g_pendingJoinActivation.present
            || g_pendingJoinActivation.continuationQueued
            || g_pendingJoinActivation.continuationInProgress) {
            failPendingCoordinatorEvent(
                "join SessionPlan was published more than once");
            return;
        }
        g_pendingJoinActivation.present = true;
        g_pendingJoinActivation.continuationQueued = true;
        g_pendingJoinActivation.plan = plan;
    }
    // Queue the SessionPlan half exactly once. Publishing the queued claim in
    // the same critical section prevents a concurrent natural BeginTurn from
    // creating a second continuation. If the packet is still absent when this
    // task runs, its later RX event becomes the sole next owner.
    if (!queueJoinActivation()) {
        failPendingCoordinatorEvent(
            "could not queue the exact join SessionPlan continuation");
    }
}

void handleBootstrapCommitted(const protocol::BootstrapProgress& committed)
{
    if (phase() != Phase::Independent
        || g_bootstrapPrepared.load(std::memory_order_acquire)
        || g_bootstrapReleased.load(std::memory_order_acquire)
        || committed.handle != joinHandle() || committed.day != 1) {
        failPendingCoordinatorEvent("invalid BootstrapCommitted transition");
        return;
    }

    if (isHost()) {
        HostBootstrap expected = HostBootstrap::AwaitingCommit;
        if (!g_hostBootstrap.compare_exchange_strong(
                expected, HostBootstrap::CommitApplied, std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent(
                "host BootstrapCommitted preceded its exact day-1 cascade");
            return;
        }
    } else {
        JoinBootstrap expected = JoinBootstrap::AwaitingCommit;
        if (!g_joinBootstrap.compare_exchange_strong(
                expected, JoinBootstrap::CommitApplied, std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent(
                "join BootstrapCommitted preceded its exact TurnInfo completion");
            return;
        }
    }

    if (!coordinator().reportBootstrap(BootstrapCheckpoint::CommitApplied,
                                       committed.handle, committed.day)) {
        if (isHost())
            g_hostBootstrap.store(HostBootstrap::Failed, std::memory_order_release);
        else
            g_joinBootstrap.store(JoinBootstrap::Failed, std::memory_order_release);
        fault("could not publish UI-applied BootstrapCommitApplied");
        return;
    }
    spdlog::info(
        "[simturns] bootstrap commit applied locally; mutation gates remain closed");
}

void handleBootstrapOperational(const protocol::BootstrapProgress& operational)
{
    if (phase() != Phase::Independent
        || g_bootstrapPrepared.load(std::memory_order_acquire)
        || g_bootstrapReleased.load(std::memory_order_acquire)
        || operational.handle != joinHandle() || operational.day != 1) {
        failPendingCoordinatorEvent("invalid BootstrapOperational transition");
        return;
    }

    if (isHost()) {
        HostBootstrap expected = HostBootstrap::CommitApplied;
        if (!g_hostBootstrap.compare_exchange_strong(
                expected, HostBootstrap::OperationalApplied, std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent(
                "host BootstrapOperational preceded its applied commit acknowledgement");
            return;
        }
    } else {
        JoinBootstrap expected = JoinBootstrap::CommitApplied;
        if (!g_joinBootstrap.compare_exchange_strong(
                expected, JoinBootstrap::OperationalApplied, std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent(
                "join BootstrapOperational preceded its applied commit acknowledgement");
            return;
        }
    }

    // This UI-thread store proves receive/cascade readiness only. Local
    // strategic TX remains closed until the relay has both prepare ACKs and
    // publishes BootstrapReleased.
    g_bootstrapPrepared.store(true, std::memory_order_release);
    if (!coordinator().reportBootstrap(BootstrapCheckpoint::OperationalApplied,
                                       operational.handle, operational.day)) {
        if (isHost())
            g_hostBootstrap.store(HostBootstrap::Failed, std::memory_order_release);
        else
            g_joinBootstrap.store(JoinBootstrap::Failed, std::memory_order_release);
        fault("could not publish UI-applied BootstrapOperationalApplied");
        return;
    }
    spdlog::info(
        "[simturns] bootstrap operational prepare applied; awaiting global release");
}

void handleBootstrapReleased(const protocol::BootstrapProgress& released)
{
    if (phase() != Phase::Independent
        || !g_bootstrapPrepared.load(std::memory_order_acquire)
        || g_bootstrapReleased.load(std::memory_order_acquire)
        || released.handle != joinHandle() || released.day != 1) {
        failPendingCoordinatorEvent("invalid BootstrapReleased transition");
        return;
    }

    if (isHost()) {
        HostBootstrap expected = HostBootstrap::OperationalApplied;
        if (!g_hostBootstrap.compare_exchange_strong(
                expected, HostBootstrap::Complete, std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent(
                "host BootstrapReleased lost its exact prepare state");
            return;
        }
    } else {
        JoinBootstrap expected = JoinBootstrap::OperationalApplied;
        if (!g_joinBootstrap.compare_exchange_strong(
                expected, JoinBootstrap::Complete, std::memory_order_acq_rel)) {
            failPendingCoordinatorEvent(
                "join BootstrapReleased lost its exact prepare state");
            return;
        }
    }

    if (!coordinator().acceptBootstrapRelease(released)) {
        fault("coordinator rejected the final BootstrapReleased acknowledgement");
        return;
    }
    bool closed = false;
    if (!g_bootstrapReleased.compare_exchange_strong(
            closed, true, std::memory_order_acq_rel)) {
        failPendingCoordinatorEvent(
            "BootstrapReleased attempted to open local TX twice");
        return;
    }
    spdlog::info(
        "[simturns] bootstrap operational release applied; strict independent turns are operational");
}

void handleCoordinatorEventOnUi(void* rawContext)
{
    std::unique_ptr<CoordinatorEventContext> context(
        static_cast<CoordinatorEventContext*>(rawContext));
    if (!context || !currentSession(context->generation))
        return;

    switch (context->event.kind) {
    case CoordinatorEventKind::SessionPlan:
        activateSession(context->event.sessionPlan);
        return;
    case CoordinatorEventKind::BootstrapCommitted:
        handleBootstrapCommitted(context->event.bootstrapCommitted);
        return;
    case CoordinatorEventKind::BootstrapOperational:
        handleBootstrapOperational(context->event.bootstrapOperational);
        return;
    case CoordinatorEventKind::BootstrapReleased:
        handleBootstrapReleased(context->event.bootstrapReleased);
        return;
    case CoordinatorEventKind::EngineAction:
        handleEngineAction(context->event.engineAction);
        return;
    }
    failPendingCoordinatorEvent(
        "unknown coordinator event reached the UI dispatcher");
}

void handleCoordinatorFaultOnUi(void* rawContext)
{
    std::unique_ptr<CoordinatorFaultContext> context(
        static_cast<CoordinatorFaultContext*>(rawContext));
    if (!context || !currentSession(context->generation)) {
        return;
    }
    const char* reason = context->fault.message.empty()
                             ? "simultaneous-turn coordinator terminated"
                             : context->fault.message.c_str();
    fault(reason);
}

void postCoordinatorEvent(std::uint64_t generation, CoordinatorEvent event)
{
    std::unique_lock<std::recursive_mutex> lock(g_sessionCallbackMutex);
    if (!currentSession(generation))
        return;
    spdlog::info("[simturns] coordinator queued event kind={} for ordered UI application",
                 static_cast<unsigned>(event.kind));
    auto* context = new (std::nothrow) CoordinatorEventContext{generation, std::move(event)};
    if (!context) {
        if (currentSession(generation))
            failPendingCoordinatorEvent(
                "could not allocate a coordinator event UI context");
        return;
    }
    // invokeOnUiThread may apply the command synchronously on the UI thread.
    // Do not hold the lifetime lock across native engine work: terminal faults
    // from another thread must still close admission immediately. A queued
    // context checks its immutable generation again before touching the engine.
    lock.unlock();
    if (!netintercept::invokeOnUiThread(&handleCoordinatorEventOnUi, context,
                                      &discardUiContext<CoordinatorEventContext>)) {
        delete context;
        lock.lock();
        if (currentSession(generation))
            failPendingCoordinatorEvent(
                "simultaneous-turn UI task queue rejected a coordinator event");
    }
}

void postCoordinatorFault(std::uint64_t generation, CoordinatorTerminalFault terminalFault)
{
    std::lock_guard<std::recursive_mutex> lock(g_sessionCallbackMutex);
    if (!currentSession(generation))
        return;
    const char* reason = terminalFault.message.empty()
                             ? "simultaneous-turn coordinator terminated"
                             : terminalFault.message.c_str();
    // fault() is explicitly thread-safe. Close native TX/RX/engine gates on
    // the coordinator callback before a queued UI diagnostic can be delayed behind
    // ordinary gameplay work after a disconnect or protocol violation.
    fault(reason);

    auto* context = new (std::nothrow) CoordinatorFaultContext{
        generation, std::move(terminalFault)};
    if (!context) {
        if (currentSession(generation))
            fault("could not allocate a coordinator-fault UI context");
        return;
    }
    if (!netintercept::invokeOnUiThread(&handleCoordinatorFaultOnUi, context,
                                      &discardUiContext<CoordinatorFaultContext>)) {
        delete context;
        if (currentSession(generation))
            fault("simultaneous-turn UI task queue rejected a coordinator fault");
    }
}

void onDeferredOverflow(std::uint32_t, const std::uint8_t*, std::uint32_t)
{
    // Best-effort terminal notification. The shared layer immediately follows
    // this callback with an unconditional process fail-fast: an ordered native
    // RX packet must never be lost while the game is allowed to continue.
    fault("deferred simultaneous-turn RX queue overflowed");
}

void propagateTerminalStateFaultToCoordinator(const char* reason)
{
    coordinator().fail(CoordinatorFailureOrigin::LocalInvariant, reason);
}

bool readRxPayload(int packet,
                   std::uint32_t frameLength,
                   const std::uint8_t** payload,
                   std::uint32_t* payloadSize)
{
    if (!packet || frameLength < sizeof(game::NetMessageHeader)
        || frameLength > game::netMessageMaxLength || !payload || !payloadSize) {
        return false;
    }
    const auto* frame = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(static_cast<std::uint32_t>(packet)));
    std::uint32_t type = 0;
    std::uint32_t storedLength = 0;
    std::memcpy(&type, frame, sizeof(type));
    std::memcpy(&storedLength, frame + sizeof(type), sizeof(storedLength));
    if (type != game::netMessageNormalType || storedLength != frameLength) {
        return false;
    }
    *payload = frame + netMessagePayloadOffset;
    *payloadSize = frameLength - netMessagePayloadOffset;
    return true;
}

bool rxMustReplayOnUiThread()
{
    const DWORD uiThread = netintercept::mainThreadId();
    return netintercept::recvDispatchDepth() != 0 || !uiThread
           || GetCurrentThreadId() != uiThread;
}

void completeJoinBootstrapTurnInfoAfterDispatch(std::uint32_t activeHandle)
{
    if (isHost() || phase() != Phase::Independent
        || activeHandle != localHandle()) {
        failJoinBootstrap(
            "post-dispatch bootstrap TurnInfo reached an invalid join session");
        return;
    }

    JoinBootstrap expected = JoinBootstrap::AwaitingOwnTurnInfo;
    if (!g_joinBootstrap.compare_exchange_strong(
            expected, JoinBootstrap::OwnTurnInfoQueued,
            std::memory_order_acq_rel)) {
        if (expected != JoinBootstrap::Failed) {
            failJoinBootstrap(
                "bootstrap join TurnInfo was duplicated or arrived out of order");
        }
        return;
    }

    if (!armStartupLeaderNameFromOwnTurnInfo(activeHandle)) {
        if (!isFaulted()) {
            failJoinBootstrap(
                "post-dispatch join TurnInfo could not arm startup leader-name TX");
        }
        return;
    }

    // TurnInfo may have completed synchronously without leaving another queue
    // item, or its resulting command may still be pending. This is one event
    // continuation, not a second attempt: the same queue-drain subscriber
    // proves the stock handler's final state in either scheduling shape.
    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    onCommandQueueDrained(phaseGame && phaseGame->data
                              ? phaseGame->data->midObjectLock
                              : nullptr);
}

void completeStartupLeaderNameAfterSend(std::uint32_t stackId, int sendResult)
{
    const std::uint32_t expectedStack =
        g_startupLeaderNameStackId.load(std::memory_order_acquire);
    const std::uint32_t frameLength =
        g_startupLeaderNameFrameLength.load(std::memory_order_acquire);
    const std::uint32_t nameByteCount =
        g_startupLeaderNameByteCount.load(std::memory_order_acquire);
    if (!stackId || stackId != expectedStack || !frameLength || !nameByteCount
        || g_startupLeaderNameTx.load(std::memory_order_acquire)
               != StartupLeaderNameTx::InFlight) {
        fault("startup leader-name Send completion lost its exact TX claim");
        return;
    }
    if (sendResult == 0) {
        fault("natural startup leader-name transport Send failed");
        return;
    }

    StartupLeaderNameTx expected = StartupLeaderNameTx::InFlight;
    if (!g_startupLeaderNameTx.compare_exchange_strong(
            expected, StartupLeaderNameTx::Sent,
            std::memory_order_acq_rel)) {
        fault("startup leader-name Send completion was duplicated or out of order");
        return;
    }
    spdlog::info(
        "[simturns] bootstrap first-leader-name TX sent (role={}, frame={}, nameBytes={}, stack={:#x}, bootstrap={})",
        isHost() ? "host" : "join", frameLength, nameByteCount, stackId,
        startupBootstrapSubstate());
}

void completeEndTurnAfterSend(std::uint32_t lease, int sendResult)
{
    // Both DirectPlay and SLikeNet continuations normalize their native bool
    // result to 1/0. The control-plane half is published only after the one natural
    // transport send returns successfully; no retry/requeue exists.
    if (sendResult == 0) {
        fault("natural EndTurn transport send failed; no issued signal published");
        return;
    }
    if (phase() != Phase::Independent
        || !g_bootstrapReleased.load(std::memory_order_acquire)) {
        fault("EndTurn Send completed after the independent mutation gate closed");
        return;
    }
    if (!coordinator().reportEndTurnObserved(lease)) {
        fault("could not publish post-original EndTurnObserved");
        return;
    }
    spdlog::info(
        "[simturns] natural EndTurn Send completed; lease={} published once",
        lease);
}

void completeHostEndTurnAfterDispatch(std::uint32_t originHandle)
{
    const Phase current = phase();
    const bool acceptsNaturalEndTurn =
        current == Phase::Independent
        || (current == Phase::Held && originHandle == otherHandle());
    if (!isHost() || !acceptsNaturalEndTurn
        || !g_bootstrapPrepared.load(std::memory_order_acquire)) {
        fault("post-dispatch EndTurn RX completion reached an invalid host phase");
        return;
    }
    std::atomic<bool>* const claim = endTurnRxClaimFor(originHandle);
    if (!claim || !claim->load(std::memory_order_acquire)) {
        fault("post-dispatch EndTurn RX completion lost its exact origin claim");
        return;
    }
    if (!coordinator().reportEndTurnApplied(originHandle)) {
        fault("could not publish post-original EndTurnApplied");
        return;
    }
    spdlog::info(
        "[simturns] host original EndTurn RX completed for handle={:#x}; applied signal published once",
        originHandle);
}

netintercept::RxDecision rxGate(void*, void*, int packet,
                                std::uint32_t frameLength,
                                std::uint32_t idFrom,
                                std::uint32_t playerNetId)
{
    if (!g_sessionActive.load(std::memory_order_acquire)
        || g_tearingDown.load(std::memory_order_acquire))
        return netintercept::RxDecision::Pass;
    const std::uint8_t* payload = nullptr;
    std::uint32_t payloadSize = 0;
    if (!readRxPayload(packet, frameLength, &payload, &payloadSize))
        return netintercept::RxDecision::Pass;

    const Phase current = phase();
    const bool stateMutation = isStateMutation(payload, payloadSize);
    if ((current == Phase::Faulted || current == Phase::Closing) && stateMutation)
        return netintercept::RxDecision::Drop;

    const bool beginTurn = hasExactRtti(payload, payloadSize, beginTurnRtti);
    const bool turnInfo = hasExactRtti(payload, payloadSize, turnInfoRtti);
    const bool endTurn = hasExactRtti(payload, payloadSize, requestEndTurnRtti);

    if (current != Phase::Stock
        && !g_authoritativeSenderDpid.load(std::memory_order_acquire)
        && beginTurn) {
        TurnAnnouncement announcement;
        bool queueJoinContinuation = false;
        if (frameLength != syntheticBeginTurnFrameLength
            || !decodeTurnAnnouncement(payload, payloadSize, announcement)) {
            fault("natural pre-bind BeginTurn violated the exact Russobit frame layout");
            return netintercept::RxDecision::Drop;
        }
        const bool exact = announcement.layout == TurnAnnouncementLayout::Broadcast
                               ? recordExactStartupBeginTurnProof(
                                     idFrom, playerNetId, announcement)
                               : recordExactPreBindDirectedBeginTurn(
                                     idFrom, playerNetId, announcement,
                                     queueJoinContinuation);
        if (!exact) {
            fault("natural pre-bind BeginTurn violated the authoritative DPID proof");
            return netintercept::RxDecision::Drop;
        }
        if (queueJoinContinuation && !queueJoinActivation()) {
            failPendingCoordinatorEvent(
                "could not queue the exact natural BeginTurn SessionPlan continuation");
            return netintercept::RxDecision::Drop;
        }
    }

    const std::uint32_t boundAuthoritativeDpid =
        g_authoritativeSenderDpid.load(std::memory_order_acquire);
    if (boundAuthoritativeDpid && (beginTurn || turnInfo)) {
        const std::uint32_t boundLocalDpid =
            g_localReceiverDpid.load(std::memory_order_relaxed);
        if (playerNetId != boundLocalDpid) {
            fault("turn-announcement receiver changed after exact SessionPlan binding");
            return netintercept::RxDecision::Drop;
        }
        if (idFrom != boundAuthoritativeDpid) {
            fault("authoritative turn announcement did not come from bound DPID_SERVERPLAYER");
            return netintercept::RxDecision::Drop;
        }
    }

    const bool hostStartupTurnInfoWindow =
        isHost() && !g_bootstrapReleased.load(std::memory_order_acquire)
        && (current == Phase::Prepared || current == Phase::WaitingForSession
            || current == Phase::Ready || current == Phase::Independent);
    if (hostStartupTurnInfoWindow && turnInfo) {
        std::uint32_t activeHandle = 0;
        if (!decodeTurnInfoActive(payload, payloadSize, activeHandle)
            || !activeHandle) {
            fault("natural host startup TurnInfo violated its exact layout");
            return netintercept::RxDecision::Drop;
        }
        const std::uint32_t publishedLocalHandle = localHandle();
        const bool exactOwnRoute =
            isExactOwnTurnInfoRoute(idFrom, playerNetId, activeHandle);
        const bool ownTurnInfo = exactOwnRoute
                                 || (publishedLocalHandle
                                     && activeHandle == publishedLocalHandle);
        if (ownTurnInfo) {
            if (!exactOwnRoute) {
                fault("natural own-player TurnInfo violated its exact authoritative route");
                return netintercept::RxDecision::Drop;
            }
            if (g_startupLeaderNameTx.load(std::memory_order_acquire)
                != StartupLeaderNameTx::Dormant) {
                fault("host received more than one startup own-player TurnInfo");
                return netintercept::RxDecision::Drop;
            }
            if (!netintercept::armCurrentRxCompletion(
                    &completeHostStartupTurnInfoAfterDispatch, activeHandle)) {
                fault("could not arm post-dispatch host startup TurnInfo completion");
                return netintercept::RxDecision::Drop;
            }
            return netintercept::RxDecision::Pass;
        }
    }

    if (isHost()
        && (current == Phase::Independent || current == Phase::Held)
        && endTurn) {
        if (!g_bootstrapPrepared.load(std::memory_order_acquire)) {
            fault("natural EndTurn RX escaped before bootstrap operational release");
            return netintercept::RxDecision::Drop;
        }
        if (frameLength != requestEndTurnFrameLength
            || !isDynamicPlayerDpid(idFrom)
            || playerNetId != game::serverNetPlayerId) {
            fault("host received a malformed natural CReqEndTurnMsg");
            return netintercept::RxDecision::Drop;
        }
        if (rxMustReplayOnUiThread())
            return netintercept::RxDecision::Defer;

        auto* logic = g_serverLogic.load(std::memory_order_acquire);
        auto* player = findPlayerByNetId(logic, idFrom);
        if (!isCurrentNetworkServerLogic(logic) || !player
            || !player->controlledByHuman) {
            fault("EndTurn RX sender DPID did not resolve to one exact live human player");
            return netintercept::RxDecision::Drop;
        }
        const std::uint32_t originHandle =
            static_cast<std::uint32_t>(player->playerId.value);
        // Once the host has finished first, the only remaining legal natural
        // EndTurn belongs to the still-active peer. Local TX stays closed in
        // Held; this exception exists solely on the host RX path.
        if (current == Phase::Held && originHandle != otherHandle()) {
            fault("held host received EndTurn from an already-finished origin");
            return netintercept::RxDecision::Drop;
        }
        std::atomic<bool>* const claim = endTurnRxClaimFor(originHandle);
        bool expected = false;
        if (!claim || !claim->compare_exchange_strong(
                          expected, true, std::memory_order_acq_rel)) {
            fault("duplicate or out-of-order host EndTurn RX claim");
            return netintercept::RxDecision::Drop;
        }
        if (!netintercept::armCurrentRxCompletion(
                &completeHostEndTurnAfterDispatch, originHandle)) {
            fault("could not arm exact post-dispatch host EndTurn RX completion");
            return netintercept::RxDecision::Drop;
        }
        return netintercept::RxDecision::Pass;
    }

    // Once both exact pre-merge EndTurn packets have been applied, another
    // request is stale by construction. Never let it fall through into stock
    // mutation while the convergence overlay still owns the receive gate.
    if (isHost() && endTurn && overlayOwnsMutationGate(current)) {
        fault("natural EndTurn RX arrived outside the active pre-merge phase");
        return netintercept::RxDecision::Drop;
    }

    if (!isHost() && current == Phase::Independent && turnInfo
        && !g_bootstrapPrepared.load(std::memory_order_acquire)) {
        std::uint32_t activeHandle = 0;
        if (!decodeTurnInfoActive(payload, payloadSize, activeHandle)) {
            failPendingCoordinatorEvent("invalid TurnInfo during join bootstrap");
            return netintercept::RxDecision::Drop;
        }

        const JoinBootstrap bootstrap =
            g_joinBootstrap.load(std::memory_order_acquire);
        if (activeHandle == localHandle()) {
            if (bootstrap != JoinBootstrap::AwaitingOwnTurnInfo) {
                failPendingCoordinatorEvent(
                    "own TurnInfo arrived outside its exact bootstrap event state");
                return netintercept::RxDecision::Drop;
            }
            if (rxMustReplayOnUiThread())
                return netintercept::RxDecision::Defer;
            if (!netintercept::armCurrentRxCompletion(
                    &completeJoinBootstrapTurnInfoAfterDispatch, activeHandle)) {
                failPendingCoordinatorEvent(
                    "could not arm post-dispatch bootstrap TurnInfo completion");
                return netintercept::RxDecision::Drop;
            }
            return netintercept::RxDecision::Pass;
        }

        // Host-active startup announcements remain suppressed by the normal
        // independent-turn filter below. They are not substitutes for the one
        // exact join TurnInfo and do not trigger any bootstrap action.
    }

    if (isHost() && current == Phase::Merging && beginTurn) {
        if (!g_executeStarted.load(std::memory_order_acquire)) {
            failPendingCoordinatorEvent(
                "natural host BeginTurn preceded ExecuteMerge authorization");
            return netintercept::RxDecision::Drop;
        }
        TurnAnnouncement announcement;
        if (!decodeTurnAnnouncement(payload, payloadSize, announcement)) {
            failPendingCoordinatorEvent(
                "invalid natural BeginTurn while host awaits stock handoff");
            return netintercept::RxDecision::Drop;
        }
        if (isExactStockHandoff(announcement.layout, announcement.activeHandle,
                                announcement.sequence, frameLength, hostHandle())) {
            if (rxMustReplayOnUiThread())
                return netintercept::RxDecision::Defer;

            spdlog::info(
                "[simturns] accepted host natural stock BeginTurn "
                "(frame={}, addressee={:#x}, sequence={}, active={:#x}, mergeDay={})",
                frameLength, announcement.addressee, announcement.sequence,
                announcement.activeHandle, configuredMergeDay());

            const protocol::EngineAction mergeAction = g_executeMergeAction;
            std::uint32_t actionId = 0;
            if (!claimPendingUiApply(PendingUiApplyKind::NaturalMerge,
                                     mergeAction, hostHandle(), actionId)) {
                failPendingCoordinatorEvent(
                    "host natural BeginTurn could not claim its merge action");
                return netintercept::RxDecision::Drop;
            }
            if (!netintercept::armCurrentRxCompletion(
                    &completeNaturalStockHandoffDispatch, actionId)) {
                cancelPendingUiApply(actionId);
                failPendingCoordinatorEvent(
                    "could not arm post-dispatch host BeginTurn completion");
                return netintercept::RxDecision::Drop;
            }
            return netintercept::RxDecision::Pass;
        }
        spdlog::error(
            "[simturns] rejected host natural stock BeginTurn "
            "(layout={}, frame={}, addressee={:#x}, sequence={}, active={:#x}, "
            "expectedHost={:#x}, mergeDay={})",
            static_cast<unsigned>(announcement.layout), frameLength,
            announcement.addressee, announcement.sequence,
            announcement.activeHandle, hostHandle(), configuredMergeDay());
        failPendingCoordinatorEvent(
            "unexpected BeginTurn layout/player while host awaits stock handoff");
        return netintercept::RxDecision::Drop;
    }

    if (!isHost() && current == Phase::AwaitingStockTurn && beginTurn) {
        TurnAnnouncement announcement;
        if (!decodeTurnAnnouncement(payload, payloadSize, announcement)) {
            failPendingCoordinatorEvent(
                "invalid natural BeginTurn while awaiting stock handoff");
            return netintercept::RxDecision::Drop;
        }
        // Exact Russobit captures and IDA prove that sub_420FFA broadcasts
        // carry {zero,+36; global CCommandMsg sequence,+40; active,+44}.
        // Merge day is causal relay/scenario state, not a field in this packet.
        // Directed activation carries the UINT32_MAX sentinel and cannot
        // release this boundary.
        if (isExactStockHandoff(announcement.layout, announcement.activeHandle,
                                announcement.sequence, frameLength, hostHandle())) {
            if (rxMustReplayOnUiThread())
                return netintercept::RxDecision::Defer;

            spdlog::info(
                "[simturns] accepted join natural stock BeginTurn "
                "(frame={}, addressee={:#x}, sequence={}, active={:#x}, mergeDay={})",
                frameLength, announcement.addressee, announcement.sequence,
                announcement.activeHandle, configuredMergeDay());

            const protocol::EngineAction mergeAction = g_executeMergeAction;
            std::uint32_t actionId = 0;
            if (!claimPendingUiApply(PendingUiApplyKind::NaturalMerge,
                                     mergeAction, hostHandle(), actionId)) {
                failPendingCoordinatorEvent(
                    "join natural BeginTurn could not claim its merge action");
                return netintercept::RxDecision::Drop;
            }
            if (!netintercept::armCurrentRxCompletion(
                    &completeNaturalStockHandoffDispatch, actionId)) {
                cancelPendingUiApply(actionId);
                failPendingCoordinatorEvent(
                    "could not arm post-dispatch join BeginTurn completion");
                return netintercept::RxDecision::Drop;
            }
            return netintercept::RxDecision::Pass;
        }
        spdlog::error(
            "[simturns] rejected join natural stock BeginTurn "
            "(layout={}, frame={}, addressee={:#x}, sequence={}, active={:#x}, "
            "expectedHost={:#x}, mergeDay={})",
            static_cast<unsigned>(announcement.layout), frameLength,
            announcement.addressee, announcement.sequence,
            announcement.activeHandle, hostHandle(), configuredMergeDay());
        failPendingCoordinatorEvent(
            "unexpected BeginTurn layout/player while awaiting stock handoff");
        return netintercept::RxDecision::Drop;
    }

    const bool joinFilterActive = !isHost()
                                  && (current == Phase::Independent
                                      || current == Phase::Held
                                      || current == Phase::Merging
                                      || current == Phase::AwaitingStockTurn
                                      || current == Phase::Faulted);
    const bool turnAnnouncement = beginTurn || turnInfo;
    if (joinFilterActive && turnAnnouncement) {
        std::uint32_t activeHandle = 0;
        if (beginTurn) {
            TurnAnnouncement announcement;
            if (decodeTurnAnnouncement(payload, payloadSize, announcement))
                activeHandle = announcement.activeHandle;
        } else {
            decodeTurnInfoActive(payload, payloadSize, activeHandle);
        }
        if (!activeHandle) {
            failPendingCoordinatorEvent(
                "invalid natural turn announcement on join RX");
            return netintercept::RxDecision::Drop;
        }
        // Keep a directed activation/TurnInfo for the joiner's own handle;
        // suppress only when the correctly decoded active player is the host
        // and would steal independent/held UI ownership.
        if (current == Phase::Faulted)
            return netintercept::RxDecision::Drop;
        if (activeHandle == hostHandle())
            return netintercept::RxDecision::Consume;
    }

    if (!requiresUiReplay(payload, payloadSize))
        return netintercept::RxDecision::Pass;

    // Before SessionPlan and after the natural merge handoff this layer is
    // observational: preserve stock DirectPlay threading and ordering.
    if (overlayOwnsMutationGate(current) && rxMustReplayOnUiThread()) {
        return netintercept::RxDecision::Defer;
    }
    return netintercept::RxDecision::Pass;
}

netintercept::TxDecision txGate(void*,
                                std::uint32_t idTo,
                                const game::NetMessageHeader* message)
{
    if (!g_sessionActive.load(std::memory_order_acquire)
        || g_tearingDown.load(std::memory_order_acquire))
        return netintercept::TxDecision::Pass;
    if (!message || message->messageType != game::netMessageNormalType
        || message->length < offsetof(game::NetMessageHeader, messageClassName)
        || message->length > game::netMessageMaxLength) {
        return netintercept::TxDecision::Pass;
    }

    const auto* payload = reinterpret_cast<const std::uint8_t*>(message) + 8;
    const std::uint32_t payloadSize =
        message->length - offsetof(game::NetMessageHeader, messageClassName);
    const bool endTurnRtti = hasExactRtti(payload, payloadSize, requestEndTurnRtti);
    const bool endTurn = endTurnRtti
                         && message->length == requestEndTurnFrameLength;
    const bool startupLeaderName =
        hasExactRtti(payload, payloadSize, startupLeaderNameRtti);
    const StartupLeaderNameTx startupLeaderNameState =
        g_startupLeaderNameTx.load(std::memory_order_acquire);
    const Phase current = phase();
    const bool closedBootstrapTxWindow =
        !g_bootstrapReleased.load(std::memory_order_acquire)
        && (current == Phase::Prepared || current == Phase::WaitingForSession
            || current == Phase::Ready || current == Phase::Independent);
    const bool causalStartupNameOutstanding =
        startupLeaderNameState == StartupLeaderNameTx::Expected
        || startupLeaderNameState == StartupLeaderNameTx::InFlight;
    if (startupLeaderName
        && (closedBootstrapTxWindow || causalStartupNameOutstanding)) {
        if (idTo != game::serverNetPlayerId) {
            fault("startup leader-name TX did not target DPID_SERVERPLAYER");
            return netintercept::TxDecision::Reject;
        }
        return consumeStartupLeaderNameIntent(message, payload, payloadSize)
                   ? netintercept::TxDecision::Pass
                   : netintercept::TxDecision::Reject;
    }
    // Russobit sends local player intents to DPID_SERVERPLAYER (1). Host
    // authoritative broadcasts use 0/dynamic recipients and must continue to
    // reach a joiner that is still playing while the host waits at its barrier.
    if (idTo != game::serverNetPlayerId
        || !isStrategicIntent(payload, payloadSize)) {
        return netintercept::TxDecision::Pass;
    }

    switch (current) {
    case Phase::Independent:
        if (!g_bootstrapReleased.load(std::memory_order_acquire)) {
            fault("strategic intent escaped before UI-applied bootstrap operational release");
            return netintercept::TxDecision::Reject;
        }
        if (coordinator().endTurnPending()) {
            fault("strategic intent attempted after the local EndTurn lease claim");
            return netintercept::TxDecision::Reject;
        }
        if (endTurnRtti && !endTurn) {
            fault("local CReqEndTurnMsg did not have the exact Russobit wire size");
            return netintercept::TxDecision::Reject;
        }
        if (!endTurn)
            return netintercept::TxDecision::Pass;
        {
            std::uint32_t lease = 0;
            if (!coordinator().claimEndTurn(lease)) {
                fault("could not claim the relay-issued local EndTurn lease");
                return netintercept::TxDecision::Reject;
            }
            if (!netintercept::armCurrentTxCompletion(
                    &completeEndTurnAfterSend, lease)) {
                fault("could not arm exact post-original EndTurn Send completion");
                return netintercept::TxDecision::Reject;
            }
        }
        return netintercept::TxDecision::Pass;
    case Phase::Prepared:
    case Phase::WaitingForSession:
    case Phase::Ready:
    case Phase::Held:
    case Phase::Merging:
    case Phase::AwaitingStockTurn:
    case Phase::Closing:
    case Phase::Faulted:
        // This is a mutation backstop, not packet suppression. Returning a
        // synthetic success would let stock producers retain pending UI/object
        // state for a message that never entered the transport.
        return netintercept::TxDecision::Reject;
    case Phase::Disabled:
    case Phase::Stock:
    case Phase::Merged:
        return netintercept::TxDecision::Pass;
    }
    return netintercept::TxDecision::Reject;
}

// Only called at a quiescent native-map boundary. Process-owned Detours and
// their originals are deliberately retained; every borrowed map identity is not.
void resetMapState()
{
    g_coordinatorStartAttempted.store(false, std::memory_order_release);
    g_phaseGame.store(nullptr, std::memory_order_release);
    g_serverLogic.store(nullptr, std::memory_order_release);
    g_mergeActionId.store(0, std::memory_order_release);
    g_mergeDay.store(0, std::memory_order_release);
    g_executeStarted.store(false, std::memory_order_release);
    g_executeResultQueued.store(false, std::memory_order_release);
    g_naturalMergeReady.store(false, std::memory_order_release);
    g_mergeAppliedQueued.store(false, std::memory_order_release);
    g_executeMergeAction = {};
    g_hostEndTurnRxClaimed.store(false, std::memory_order_release);
    g_joinEndTurnRxClaimed.store(false, std::memory_order_release);
    g_authoritativeSenderDpid.store(0, std::memory_order_release);
    g_localReceiverDpid.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_startupIdentityMutex);
        g_startupBeginTurnServerDpid = 0;
        g_startupBeginTurnReceiverDpid = 0;
        g_startupBeginTurnActiveHandle = 0;
        g_startupDirectedAddressee = 0;
        g_startupTurnProofState = StartupTurnProofState::Empty;
        g_pendingJoinActivation = {};
    }
    {
        std::lock_guard<std::mutex> lock(g_pendingUiApplyMutex);
        g_pendingUiApply = {};
    }
    g_joinBootstrap.store(JoinBootstrap::Dormant, std::memory_order_release);
    g_hostBootstrap.store(HostBootstrap::Dormant, std::memory_order_release);
    g_bootstrapPrepared.store(false, std::memory_order_release);
    g_bootstrapReleased.store(false, std::memory_order_release);
    g_startupLeaderNameTx.store(StartupLeaderNameTx::Dormant,
                                std::memory_order_release);
    g_startupLeaderNameExpectedHandle.store(0, std::memory_order_release);
    g_startupLeaderNameStackId.store(0, std::memory_order_release);
    g_startupLeaderNameFrameLength.store(0, std::memory_order_release);
    g_startupLeaderNameByteCount.store(0, std::memory_order_release);
    resetTurnContext();
}

} // namespace

bool prepare()
{
    if (g_prepared.load(std::memory_order_acquire))
        return true;

    bool expected = false;
    if (!g_prepareAttempted.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel)) {
        return g_prepared.load(std::memory_order_acquire);
    }
    if (!executablefingerprint::isExactRussobit()) {
        spdlog::info("[simturns] unsupported executable; lobby capability unavailable");
        return false;
    }

    std::string error;
    DetourTargets productionTargets;
    bool ok = netintercept::preflight();
    ok = patches::preflight() && ok;
    ok = engine_hooks::preflight() && ok;
    // A process can host one room and join the next. Install the superset once;
    // wrappers delegate unchanged while Disabled or when the role does not own
    // the host-side hook. Room admission, not DLL-load environment, selects role.
    ok = appendProductionHookBundle(true, productionTargets, error) && ok;
    if (!mergeEntryMatches()) {
        error = "simultaneous-turn Russobit preflight failed at merge 0x420FFA";
        ok = false;
    }
    if (!ok) {
        spdlog::error("[simturns] complete read-only preflight failed{}{}; no hooks appended",
                      error.empty() ? "" : ": ", error);
        return false;
    }
    if (!uiframedispatcher::request())
        return false;

    g_productionTargets = std::move(productionTargets);
    g_prepared.store(true, std::memory_order_release);
    spdlog::info("[simturns] both-role production bundle prepared; no room activated");
    return true;
}

void appendHooks(Hooks& hooks)
{
    if (!g_prepared.load(std::memory_order_acquire))
        return;

    engine_hooks::append(hooks);
    hooks.reserve(hooks.size() + g_productionTargets.size());
    for (const auto& target : g_productionTargets)
        hooks.push_back(HookInfo{target.target, target.hook, target.original});
}

bool install()
{
    if (!g_prepared.load(std::memory_order_acquire))
        return true;
    if (g_installed.load(std::memory_order_acquire))
        return true;

    if (!netintercept::install()) {
        spdlog::error("[simturns] common RX/TX installation failed; wrappers remain stock");
        return false;
    }

    netintercept::setRxDispatchCallback(&rxGate);
    netintercept::setTxCallback(&txGate);
    netintercept::setDeferredOverflowCallback(&onDeferredOverflow);
    if (!setFaultObserver(&propagateTerminalStateFaultToCoordinator)) {
        spdlog::error(
            "[simturns] could not register the process-lifetime coordinator sink");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    spdlog::info(
        "[simturns] engine interception installed; coordinator start deferred to strategic phase");
    return true;
}

bool available()
{
    return g_prepared.load(std::memory_order_acquire)
           && g_installed.load(std::memory_order_acquire);
}

bool strategicQueueIdle()
{
    if (!g_sessionActive.load(std::memory_order_acquire)
        || g_tearingDown.load(std::memory_order_acquire) || isFaulted()
        || GetCurrentThreadId() != netintercept::mainThreadId()
        || netintercept::recvDispatchDepth() != 0)
        return false;
    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    return phaseGame && phaseGame->data
           && exactStrategicQueueEmpty(phaseGame, phaseGame->data->midObjectLock);
}

std::uint64_t pregameNativeNotificationGeneration()
{
    if (GetCurrentThreadId() != netintercept::mainThreadId()
        || !g_sessionActive.load(std::memory_order_acquire)
        || g_tearingDown.load(std::memory_order_acquire)
        || phase() != Phase::WaitingForSession
        || g_phaseGame.load(std::memory_order_acquire)
        || g_coordinatorStartAttempted.load(std::memory_order_acquire)) {
        return 0;
    }
    // CMidClient owns the strategic notification handlers. Its creation closes
    // this exception before the first onPhaseGame callback can publish itself.
    const auto* midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data || midgard->data->client) return 0;
    return g_sessionGeneration.load(std::memory_order_acquire);
}

bool beginSession(Role selectedRole)
{
    std::lock_guard<std::recursive_mutex> lock(g_sessionCallbackMutex);
    if (!available() || g_sessionActive.load(std::memory_order_acquire)
        || g_tearingDown.load(std::memory_order_acquire)
        || phase() != Phase::Disabled
        || (selectedRole != Role::Host && selectedRole != Role::Join)
        || GetCurrentThreadId() != netintercept::mainThreadId()) {
        return false;
    }
    const SimTurnsSessionOptions selected{true, selectedRole};
    std::string error;
    if (!coordinator().preflight(selected, error)) {
        spdlog::error("[simturns] authenticated-room preflight failed: {}", error);
        return false;
    }
    resetMapState();
    initializeState(selectedRole);
    markEngineInstalled();
    if (isFaulted()) {
        resetState();
        return false;
    }
    // Last fallible step: a rejected Arm must leave no active capture behind.
    // No native work is admitted before the active publication below.
    if (!netintercept::beginSession()) {
        resetState();
        spdlog::error("[simturns] native receive identity could not be armed");
        return false;
    }
    g_sessionActive.store(true, std::memory_order_release);
    spdlog::info("[simturns] room armed before native startup (role={})",
                 selectedRole == Role::Host ? "host" : "join");
    return true;
}

bool beginSessionTeardown()
{
    std::unique_lock<std::recursive_mutex> lock(g_sessionCallbackMutex);
    if (!available() || !g_sessionActive.load(std::memory_order_acquire))
        return true;
    const auto uiThread = netintercept::mainThreadId();
    if (uiThread != 0 && GetCurrentThreadId() != uiThread)
        return false;
    // clearNetworkStateAndService may call clearNetworkState internally.
    // Only the outer return proves destruction of the complete native owner.
    if (g_teardownDepth == std::numeric_limits<unsigned>::max())
        return false;
    if (g_teardownDepth++ != 0)
        return true;
    g_tearingDown.store(true, std::memory_order_release);
    // Generation is local lifetime identity, not a server epoch or a retry.
    // Fence callbacks before stopping transport; stop cannot revive this map.
    const auto generation = g_sessionGeneration.load(std::memory_order_relaxed);
    if (generation == std::numeric_limits<std::uint64_t>::max())
        return false;
    g_sessionGeneration.store(generation + 1, std::memory_order_release);
    closeStateForTeardown();
    // No old callback can enter after the generation/Closing publication.
    // Do not hold this lock while quiesce waits for an existing native report.
    lock.unlock();
    coordinator().quiesce();
    return netintercept::beginSessionTeardown();
}

bool endSession()
{
    std::lock_guard<std::recursive_mutex> lock(g_sessionCallbackMutex);
    if (!available() || !g_sessionActive.load(std::memory_order_acquire))
        return true;
    if (!g_tearingDown.load(std::memory_order_acquire)
        || !g_teardownDepth
        || (netintercept::mainThreadId() != 0
            && GetCurrentThreadId() != netintercept::mainThreadId())) {
        return false;
    }
    if (g_teardownDepth > 1) {
        --g_teardownDepth;
        return true;
    }
    // Native clear has now joined its worker and destroyed CMidClient/Server.
    // In particular the battle compatibility flags must not leak to another
    // ordinary game, or to the same room's 111-generated replacement map.
    if (!patches::rollbackAll())
        return false;
    coordinator().stop();
    resetMapState();
    if (!netintercept::endSessionTeardown())
        return false;
    resetState();
    g_sessionActive.store(false, std::memory_order_release);
    g_tearingDown.store(false, std::memory_order_release);
    g_teardownDepth = 0;
    return true;
}

bool localActionAdmission(std::uint32_t currentPlayerHandle)
{
    switch (phase()) {
    case Phase::Disabled:
    case Phase::Stock:
    case Phase::Merged:
        return true;
    case Phase::Independent:
        return currentPlayerHandle != 0
               && currentPlayerHandle == localHandle()
               && g_bootstrapReleased.load(std::memory_order_acquire)
               && coordinator().operational()
               && !coordinator().endTurnPending();
    case Phase::Prepared:
    case Phase::WaitingForSession:
    case Phase::Ready:
    case Phase::Held:
    case Phase::Merging:
    case Phase::AwaitingStockTurn:
    case Phase::Closing:
    case Phase::Faulted:
        return false;
    }
    return false;
}

void onPhaseGame(game::CPhaseGame* phaseGame)
{
    if (!g_sessionActive.load(std::memory_order_acquire)
        || g_tearingDown.load(std::memory_order_acquire) || !phaseGame || !phaseGame->data
        || !phaseGame->data->midClient) {
        return;
    }

    game::CPhaseGame* observed = g_phaseGame.load(std::memory_order_acquire);
    if (!observed) {
        g_phaseGame.compare_exchange_strong(observed, phaseGame, std::memory_order_acq_rel);
        observed = g_phaseGame.load(std::memory_order_acquire);
    }
    if (observed != phaseGame) {
        fault("strategic phase object changed during simultaneous-turn session");
        return;
    }
    // The transport schedules a later UI edge; it does not apply control frames
    // reentrantly from this native phase/command callback.
    coordinator().notifyNativeProgress();

    if (g_coordinatorStartAttempted.load(std::memory_order_acquire) || isFaulted()
        || isAwaitingStockTurn() || isMerged()) {
        return;
    }

    auto* midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data || midgard->data->client != phaseGame->data->midClient
        || !midgard->data->multiplayerGame || midgard->data->hotseatGame
        || !midgard->data->netPlayerClientPtr
        || !midgard->data->netPlayerClientPtr->first.data) {
        return;
    }

    const SimTurnsSessionOptions selected{true, role()};
    const bool engineIsHost = midgard->data->host;
    const bool configuredHost = selected.role == Role::Host;
    if (engineIsHost != configuredHost) {
        bool expected = false;
        if (g_coordinatorStartAttempted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            fault("configured simultaneous-turn role disagrees with the real network role");
        }
        return;
    }

    const std::uint32_t handle = static_cast<std::uint32_t>(
        midgard->data->netPlayerClientPtr->second.value);
    if (!handle)
        return;
    if (configuredHost) {
        // Rooms creates the native server before the lobby can Arm this map.
        // Bind its existing typed owner at strategic startup, not in a ctor
        // whose timing would couple room negotiation to native construction.
        auto* server = midgard->data->server;
        auto* logic = server && server->data ? server->data->serverLogic : nullptr;
        if (!isCurrentNetworkServerLogic(logic))
            return;
        game::CMidServerLogic* previous = nullptr;
        if (!g_serverLogic.compare_exchange_strong(previous, logic, std::memory_order_acq_rel)
            && previous != logic) {
            fault("network server logic changed during simultaneous-turn startup");
            return;
        }
    }
    if (!publishLocalHandle(handle)) {
        fault("local player handle changed before coordinator startup");
        return;
    }
    const std::uint32_t expectedStartupLeaderHandle =
        g_startupLeaderNameExpectedHandle.load(std::memory_order_acquire);
    if (expectedStartupLeaderHandle
        && expectedStartupLeaderHandle != handle) {
        fault("pre-bind own TurnInfo handle disagrees with the strategic local player");
        return;
    }

    bool expected = false;
    if (!g_coordinatorStartAttempted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    CoordinatorCallbacks callbacks;
    const auto generation = g_sessionGeneration.load(std::memory_order_acquire);
    callbacks.postToUi = [generation](CoordinatorEvent event) {
        postCoordinatorEvent(generation, std::move(event));
    };
    callbacks.terminalFault = [generation](CoordinatorTerminalFault terminalFault) {
        postCoordinatorFault(generation, std::move(terminalFault));
    };
    if (!coordinator().start(selected, std::move(callbacks))) {
        fault("could not start simultaneous-turn coordinator");
        return;
    }
    if (!coordinator().bindLocalPlayer(handle)) {
        fault("could not publish local player handle to coordinator");
        return;
    }
    spdlog::info(
        "[simturns] coordinator startup requested from strategic UI (handle={:#x})",
        handle);
}

void onCommandQueueDrained(game::CMidObjectLock* objectLock)
{
    if (!g_sessionActive.load(std::memory_order_acquire)
        || g_tearingDown.load(std::memory_order_acquire))
        return;
    if (exactStrategicQueueEmpty(g_phaseGame.load(std::memory_order_acquire), objectLock))
        coordinator().notifyNativeProgress();
    const JoinBootstrap bootstrap =
        g_joinBootstrap.load(std::memory_order_acquire);
    const bool bootstrapDrainPending =
        bootstrap == JoinBootstrap::AwaitingStartupDrain
        || bootstrap == JoinBootstrap::BeginTurnQueued
        || bootstrap == JoinBootstrap::OwnTurnInfoQueued;
    const bool uiApplyPending = pendingUiApplyActive();
    if (!bootstrapDrainPending && !uiApplyPending)
        return;

    if (bootstrapDrainPending && uiApplyPending) {
        failPendingCoordinatorEvent(
            "join bootstrap and an ordinary relay action overlapped one queue drain");
        return;
    }

    if (!g_installed.load(std::memory_order_acquire)) {
        if (uiApplyPending)
            failPendingCoordinatorEvent(
                "PendingUiApply drain reached an uninstalled controller");
        else
            failJoinBootstrap("join bootstrap drain reached an uninstalled controller");
        return;
    }

    if (bootstrapDrainPending) {
        if (isHost()) {
            failJoinBootstrap(
                "join bootstrap drain event reached the host controller");
            return;
        }
        const Phase current = phase();
        if (current != Phase::Independent) {
            g_joinBootstrap.store(JoinBootstrap::Failed,
                                  std::memory_order_release);
            if (current != Phase::Faulted)
                fault("join bootstrap left Independent before activation completed");
            return;
        }
    }

    auto* phaseGame = g_phaseGame.load(std::memory_order_acquire);
    if (!phaseGame || !phaseGame->data || !objectLock) {
        if (uiApplyPending)
            failPendingCoordinatorEvent(
                "PendingUiApply has no strategic object lock");
        else
            failJoinBootstrap("join bootstrap has no strategic object lock");
        return;
    }
    if (phaseGame->data->midObjectLock != objectLock) {
        // Object-lock callbacks are process-wide. An unrelated queue is not
        // evidence for or against this exact strategic relay action.
        if (bootstrapDrainPending)
            failJoinBootstrap(
                "join bootstrap observed a different strategic object lock");
        return;
    }

    auto* commandQueue = game::CPhaseApi::get().getCommandQueue(&phaseGame->phase);
    if (!commandQueue || objectLock->commandQueue != commandQueue) {
        if (uiApplyPending)
            failPendingCoordinatorEvent(
                "PendingUiApply lost the exact strategic command queue");
        else
            failJoinBootstrap("join bootstrap lost the exact strategic command queue");
        return;
    }
    if (objectLock->pendingLocalUpdates != 0 || !commandQueue->started
        || commandQueue->processingCommand
        || commandQueue->commandsList.length != 0) {
        return;
    }

    if (uiApplyPending) {
        PendingUiApply completion;
        const PendingUiLatchResult result =
            latchPendingUiQueueDrain(completion);
        if (result == PendingUiLatchResult::Invalid) {
            failPendingCoordinatorEvent(
                "queue drain lost its exact PendingUiApply action");
            return;
        }
        if (result == PendingUiLatchResult::CompletionClaimed)
            completeClaimedPendingUiApply(completion);
        return;
    }

    if (bootstrap == JoinBootstrap::BeginTurnQueued) {
        if (static_cast<std::uint32_t>(phaseGame->data->currentPlayerId.value)
            != localHandle()) {
            failJoinBootstrap(
                "directed bootstrap BeginTurn drained without selecting the join player");
            return;
        }
        if (phaseGame->data->clientTakesTurn) {
            failJoinBootstrap(
                "directed bootstrap BeginTurn unexpectedly set clientTakesTurn before TurnInfo");
            return;
        }

        JoinBootstrap expected = JoinBootstrap::BeginTurnQueued;
        if (!g_joinBootstrap.compare_exchange_strong(
                expected, JoinBootstrap::AwaitingOwnTurnInfo,
                std::memory_order_acq_rel)) {
            failJoinBootstrap(
                "directed bootstrap BeginTurn lost its exact queue-drain state");
            return;
        }
        if (!coordinator().reportBootstrap(BootstrapCheckpoint::BeginTurnApplied,
                                           localHandle(), 1)) {
            failJoinBootstrap("could not publish BootstrapBeginTurnApplied");
            return;
        }
        spdlog::info(
            "[simturns] directed day-1 BeginTurn applied; awaiting one natural join TurnInfo");
        return;
    }

    if (bootstrap == JoinBootstrap::OwnTurnInfoQueued) {
        if (static_cast<std::uint32_t>(phaseGame->data->currentPlayerId.value)
                != localHandle()
            || !phaseGame->data->clientTakesTurn) {
            failJoinBootstrap(
                "own bootstrap TurnInfo drained without establishing clientTakesTurn");
            return;
        }

        JoinBootstrap expected = JoinBootstrap::OwnTurnInfoQueued;
        if (!g_joinBootstrap.compare_exchange_strong(
                expected, JoinBootstrap::AwaitingCommit,
                std::memory_order_acq_rel)) {
            failJoinBootstrap(
                "own bootstrap TurnInfo lost its exact queue-drain state");
            return;
        }
        if (!coordinator().reportBootstrap(BootstrapCheckpoint::Complete,
                                           localHandle(), 1)) {
            failJoinBootstrap("could not publish BootstrapComplete");
            return;
        }
        spdlog::info(
            "[simturns] natural join TurnInfo established clientTakesTurn; awaiting bootstrap commit");
        return;
    }

    if (bootstrap != JoinBootstrap::AwaitingStartupDrain) {
        failJoinBootstrap("unexpected join bootstrap state at a queue-drain event");
        return;
    }
    if (phaseGame->data->clientTakesTurn) {
        failJoinBootstrap(
            "join already owned the turn before the exact day-1 bootstrap chain");
        return;
    }

    JoinBootstrap expected = JoinBootstrap::AwaitingStartupDrain;
    if (!g_joinBootstrap.compare_exchange_strong(
            expected, JoinBootstrap::BeginTurnQueued,
            std::memory_order_acq_rel)) {
        failJoinBootstrap("join startup drain attempted more than one activation");
        return;
    }

    TurnGrant grant;
    if (!resolveTurnGrant(localHandle(), grant) || grant.day != 1
        || !setScenarioDay(clientObjectMap(), grant.day)
        || !injectBeginTurn(localHandle(), grant.day)) {
        failJoinBootstrap("join bootstrap drain-boundary activation failed");
        return;
    }

    const JoinBootstrap afterInjection =
        g_joinBootstrap.load(std::memory_order_acquire);
    if (afterInjection == JoinBootstrap::BeginTurnQueued
        && !commandQueue->processingCommand
        && commandQueue->commandsList.length == 0) {
        failJoinBootstrap("join bootstrap activation did not enter CMidCommandQueue2");
        return;
    }
    if (afterInjection != JoinBootstrap::BeginTurnQueued
        && afterInjection != JoinBootstrap::AwaitingOwnTurnInfo
        && afterInjection != JoinBootstrap::Failed) {
        failJoinBootstrap("join bootstrap activation produced an invalid event state");
        return;
    }
    spdlog::info(
        "[simturns] join startup queue drained; injected one directed day-1 BeginTurn");
}

} // namespace hooks::simturns
