/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * World actions. See testdrv/worldactions.h.
 *
 * Compile-gated by D2_TESTDRV: no test code is compiled without the macro.
 *
 * moveStack submits one CStackMoveMsg through the same native CMidgard client transport used by the
 * game. For
 * ordinary movement outside the simultaneous-turn acceptance plan the path is searched over the
 * game's own per-tile enter-cost and passability. Source-reproduction actions deliberately preserve
 * the old lobby harness' exact CStackMoveMsg intent: a straight Chebyshev path origin..target with
 * cumulative cost i*3. The separate clean-long acceptance fixture uses the existing native-cost
 * path search, because the historical straight targets are now proved to cross occupied map geometry.
 * Ordinary path search snapshots every live stack from the authoritative object map before Dijkstra;
 * the per-player plan is fog-limited and therefore cannot by itself prove that an intermediate tile is
 * unoccupied.
 * For an occupied enemy target the authoritative server trims the occupied final tile and starts the
 * battle, yielding the same approach tile and movement charge as the legacy tests.
 * Mod style is null-checks, not SEH (the search allocates -> __try would be C2712); crash-safety
 * comes from the thin __try wrapper in autonav (safeMoveStack).
 */

#ifdef D2_TESTDRV

#include "testdrv/worldactions.h"
#include "testdrv/testdrv.h"
#include "testdrv/fixtureplan.h"
#include "d2list.h"
#include "d2pair.h"
#include "executablefingerprint.h"
#include "game.h"
#include "gameutils.h"
#include "usunit.h"
#include "midunitgroup.h"
#include "globaldata.h"
#include "groundcat.h"
#include "mempool.h"
#include "midgard.h"
#include "midgardid.h"
#include "midgardmap.h"
#include "midgardplan.h"
#include "midgardobjectmap.h"
#include "midserver.h"
#include "midserverlogic.h"
#include "midstack.h"
#include "midunit.h"
#include "mqnetplayer.h"
#include "mqpoint.h"
#include "testdrv/networkobservers.h"
#include "netmsg.h"
#include "netplayerinfo.h"
#include "phasegame.h"
#include "phasegamehooks.h"
#include "ussoldier.h"
#include "usstackleader.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <queue>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

namespace hooks {
namespace testdrv {
namespace worldactions {

namespace {

constexpr std::size_t kStackMoveFixedWireSize = 68;
constexpr std::size_t kStackMovePathNodeWireSize = 12;
constexpr char kStackMoveWireClass[] = ".?AVCStackMoveMsg@@";

struct HostMoveArm
{
    std::uint32_t senderDpid = 0;
    std::uint32_t stackId = 0;
    int startX = 0;
    int startY = 0;
    int targetX = 0;
    int targetY = 0;
    std::uint32_t pathLength = 0;
    bool consumed = false;
    bool dispatched = false;
    bool violation = false;
};

std::atomic<bool> g_hostMoveRouteCommitted{false};
std::atomic<bool> g_exactLegacyIntentCommitted{false};
std::atomic<bool> g_cleanLongIntentCommitted{false};
thread_local HostMoveArm* g_currentHostMoveArm = nullptr;

bool dynamicDpid(std::uint32_t dpid)
{
    return dpid > game::serverNetPlayerId && dpid != game::singleNetPlayerId;
}

bool netPlayerDpid(void* player, std::uint32_t& dpid)
{
    auto* netPlayer = reinterpret_cast<game::IMqNetPlayer*>(player);
    if (!netPlayer || !netPlayer->vftable || !netPlayer->vftable->getNetId)
        return false;
    const int value = netPlayer->vftable->getNetId(netPlayer);
    if (value < 0)
        return false;
    dpid = static_cast<std::uint32_t>(value);
    return true;
}

std::uint32_t wireU32(const game::NetMessageHeader* message, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value,
                reinterpret_cast<const std::uint8_t*>(message) + offset,
                sizeof(value));
    return value;
}

bool isExactArmedStackMove(const HostMoveArm& arm,
                           const game::NetMessageHeader* message)
{
    if (!message || message->messageType != game::netMessageNormalType
        || message->length < kStackMoveFixedWireSize
        || message->length > game::netMessageMaxLength
        || std::memcmp(message->messageClassName, kStackMoveWireClass,
                       sizeof(kStackMoveWireClass)) != 0) {
        return false;
    }

    const std::uint32_t pathLength = wireU32(message, 64);
    if (!pathLength || pathLength != arm.pathLength)
        return false;
    const std::uint64_t required =
        static_cast<std::uint64_t>(kStackMoveFixedWireSize)
        + static_cast<std::uint64_t>(pathLength) * kStackMovePathNodeWireSize;
    if (required > message->length || required > game::netMessageMaxLength)
        return false;

    return wireU32(message, 44) == arm.stackId
           && static_cast<int>(wireU32(message, 48)) == arm.startX
           && static_cast<int>(wireU32(message, 52)) == arm.startY
           && static_cast<int>(wireU32(message, 56)) == arm.targetX
           && static_cast<int>(wireU32(message, 60)) == arm.targetY;
}

netintercept::TxDecision legacyHostMoveTxGate(
    void* self, std::uint32_t idTo, const game::NetMessageHeader* message)
{
    HostMoveArm* arm = g_currentHostMoveArm;
    if (!arm)
        return netintercept::TxDecision::Pass;

    std::uint32_t actualSender = 0;
    if (!netPlayerDpid(self, actualSender)) {
        arm->violation = true;
        spdlog::critical("[worldactions] armed host move has no typed TX sender DPID");
        return netintercept::TxDecision::Reject;
    }

    if (arm->consumed) {
        // The synchronous local server handler may publish authoritative state
        // while this stack-local arm is still in scope. That is ordinary server
        // fan-out and must retain its natural transport. A second client->server
        // command under the same arm would be an unrequested second action.
        if (actualSender == game::serverNetPlayerId
            && (idTo == game::broadcastNetPlayerId || dynamicDpid(idTo))) {
            return netintercept::TxDecision::Pass;
        }
        arm->violation = true;
        spdlog::critical(
            "[worldactions] second/unexpected TX while one host move arm is consumed "
            "(sender={}, target={})",
            actualSender, idTo);
        return netintercept::TxDecision::Reject;
    }

    // `self` is the active TX interface pointer and is adjusted relative to the
    // client pointer stored by CMidgard.  The live Russobit trace proves that
    // both expose the same dynamic DPID while their raw addresses differ.
    // Sender identity is therefore the typed DPID plus the server-side
    // player/owner proof established while arming, never pointer equality.
    if (actualSender != arm->senderDpid || idTo != game::serverNetPlayerId
        || !isExactArmedStackMove(*arm, message)) {
        arm->violation = true;
        const bool readable = message && message->length >= kStackMoveFixedWireSize
                              && message->length <= game::netMessageMaxLength;
        const bool exactClass = message
                                && std::memcmp(message->messageClassName,
                                               kStackMoveWireClass,
                                               sizeof(kStackMoveWireClass)) == 0;
        spdlog::critical(
            "[worldactions] armed host move TX mismatch "
            "(sender={}/{}, target={}/1, normal={}, class={}, len={}, "
            "stack={:#010x}/{:#010x}, start=({},{})/({},{}), end=({},{})/({},{}), "
            "path={}/{})",
            actualSender, arm->senderDpid, idTo,
            message && message->messageType == game::netMessageNormalType,
            exactClass, message ? message->length : 0,
            readable ? wireU32(message, 44) : 0, arm->stackId,
            readable ? static_cast<int>(wireU32(message, 48)) : 0,
            readable ? static_cast<int>(wireU32(message, 52)) : 0,
            arm->startX, arm->startY,
            readable ? static_cast<int>(wireU32(message, 56)) : 0,
            readable ? static_cast<int>(wireU32(message, 60)) : 0,
            arm->targetX, arm->targetY,
            readable ? wireU32(message, 64) : 0, arm->pathLength);
        return netintercept::TxDecision::Reject;
    }

    // Consume before entering the server: reentrant authoritative broadcasts
    // are distinguishable from a forbidden second client command.
    arm->consumed = true;
    arm->dispatched =
        netintercept::dispatchLocalServerFrameNow(arm->senderDpid, message) > 0;
    return arm->dispatched ? netintercept::TxDecision::Redirect
                           : netintercept::TxDecision::Reject;
}

bool prepareHostMoveArm(const game::CMidgardID& ownerId,
                        const game::CMidgardID& stackId,
                        const game::CMqPoint& start,
                        const game::CMqPoint& target,
                        std::uint32_t pathLength,
                        HostMoveArm& arm)
{
    if (!g_hostMoveRouteCommitted.load(std::memory_order_acquire) || !pathLength
        || g_currentHostMoveArm) {
        return false;
    }
    auto* midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data || !midgard->data->multiplayerGame
        || midgard->data->hotseatGame || !midgard->data->host
        || !midgard->data->netPlayerClientPtr || !midgard->data->server
        || !midgard->data->server->data) {
        return false;
    }

    auto* client = midgard->data->netPlayerClientPtr->first.data;
    std::uint32_t senderDpid = 0;
    if (!client || !netPlayerDpid(client, senderDpid) || !dynamicDpid(senderDpid)
        || midgard->data->netPlayerClientPtr->second != ownerId) {
        return false;
    }
    auto* serverData = midgard->data->server->data;
    if (!serverData->serverLogic)
        return false;
    const game::NetPlayerInfo* playerInfo =
        game::CMidServerLogicApi::get().getPlayerInfo(serverData->serverLogic, senderDpid);
    if (!playerInfo || !playerInfo->controlledByHuman
        || playerInfo->playerNetId != senderDpid || playerInfo->playerId != ownerId) {
        return false;
    }

    arm.senderDpid = senderDpid;
    arm.stackId = static_cast<std::uint32_t>(stackId.value);
    arm.startX = start.x;
    arm.startY = start.y;
    arm.targetX = target.x;
    arm.targetY = target.y;
    arm.pathLength = pathLength;
    return true;
}

class HostMoveArmScope
{
public:
    explicit HostMoveArmScope(HostMoveArm& arm)
        : previous(g_currentHostMoveArm)
    {
        if (!previous)
            g_currentHostMoveArm = &arm;
    }

    ~HostMoveArmScope()
    {
        g_currentHostMoveArm = previous;
    }

    bool entered() const { return previous == nullptr; }

private:
    HostMoveArm* previous;
};

bool submitStackMoveOnce(game::CPhaseGame* phaseGame,
                         const game::CMidgardID& ownerId,
                         const game::CMidgardID& stackId,
                         const game::List<game::Pair<game::CMqPoint, int>>& path,
                         const game::CMqPoint& start,
                         const game::CMqPoint& target)
{
    auto* midgard = game::CMidgardApi::get().instance();
    const bool exactNetworkHost =
        midgard && midgard->data && midgard->data->multiplayerGame
        && !midgard->data->hotseatGame && midgard->data->host;
    if (!exactNetworkHost || !g_hostMoveRouteCommitted.load(std::memory_order_acquire)) {
        return hooks::trySendStackMoveMsgThroughNativeTransport(
            phaseGame, &stackId, &path, &start, &target);
    }

    HostMoveArm arm;
    if (!prepareHostMoveArm(ownerId, stackId, start, target, path.length, arm)) {
        spdlog::error("[worldactions] exact host move route could not be armed");
        return false;
    }
    HostMoveArmScope scope(arm);
    if (!scope.entered())
        return false;

    const bool sendResult = hooks::trySendStackMoveMsgThroughNativeTransport(
        phaseGame, &stackId, &path, &start, &target);
    const bool complete = sendResult && arm.consumed && arm.dispatched && !arm.violation;
    if (!complete) {
        spdlog::error(
            "[worldactions] one host move ended incomplete (send={}, consumed={}, "
            "dispatched={}, violation={}); no fallback/retry",
            sendResult, arm.consumed, arm.dispatched, arm.violation);
    }
    return complete;
}

// --- bare game-List node helpers ---------------------------------------------------------------
// The game exposes a typed constructor/pushBack only for IdList (List<CMidgardID>), not for
// List<CMqPoint> / List<Pair<CMqPoint,int>>, so we build the circular-sentinel list by hand against
// the game allocator. The game only ITERATES these lists (populateFromPath reads the raw path;
// CStackMoveMsg deep-copies the wire path), never frees them through our `allocator`, so
// allocator=nullptr is safe and we free our own nodes.
template <typename T>
void listInit(game::List<T>& list)
{
    auto* head = static_cast<game::ListNode<T>*>(
        game::Memory::get().allocate(sizeof(game::ListNode<T>)));
    head->next = head;
    head->prev = head;
    list.length = 0;
    list.head = head;
    list.unknown = 0;
    list.allocator = nullptr;
}

template <typename T>
void listPushBack(game::List<T>& list, const T& value)
{
    auto* node = static_cast<game::ListNode<T>*>(
        game::Memory::get().allocate(sizeof(game::ListNode<T>)));
    node->data = value;
    node->prev = list.head->prev;
    node->next = list.head;
    list.head->prev->next = node;
    list.head->prev = node;
    ++list.length;
}

template <typename T>
void listFree(game::List<T>& list)
{
    if (!list.head)
        return;
    auto* node = list.head->next;
    while (node != list.head) {
        auto* next = node->next;
        game::Memory::get().freeNonZero(node);
        node = next;
    }
    game::Memory::get().freeNonZero(list.head);
    list.head = nullptr;
    list.length = 0;
}

game::CMidgardID localPlayerId()
{
    auto* midgard = game::CMidgardApi::get().instance();
    if (midgard && midgard->data && midgard->data->netPlayerClientPtr)
        return midgard->data->netPlayerClientPtr->second;
    // Single-instance (skirmish/hotseat): no network client, so netPlayerClientPtr is null; the player
    // whose turn it is is "self". Without this the ownership check below rejects every move.
    if (auto* phaseGame = testdrv::livePhaseGame())
        if (phaseGame->data)
            return phaseGame->data->currentPlayerId;
    return game::emptyId;
}

bool parseWireId(const char* text, game::CMidgardID& id)
{
    if (!text)
        return false;

    const std::size_t length = std::strlen(text);
    // The bridge publishes and accepts one canonical wire spelling only.  Apart
    // from tightening the test-only API, this keeps the process-lifetime
    // semantic-intent ledger numeric: changing hex case cannot disguise an
    // already queued or consumed move as a second command.
    if (length == 10 && text[0] == '0' && text[1] == 'x') {
        std::uint32_t value = 0;
        for (std::size_t i = 2; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            else
                return false;
            value = (value << 4) | digit;
        }
        id.value = static_cast<int>(value);
        return id != game::emptyId;
    }
    return false;
}

bool parseGenericId(const char* text, game::CMidgardID& id)
{
    if (!text || !*text)
        return false;
    if (parseWireId(text, id))
        return true;
    game::CMidgardIDApi::get().fromString(&id, text);
    return id != game::emptyId;
}

bool resolveGarrisonExit(const game::CMidgardID& stackId,
                         const game::CMqPoint& anchor,
                         int requestedX,
                         int requestedY,
                         game::CMqPoint& exitStart,
                         game::CMqPoint& exitDest)
{
    exitStart.x = anchor.x + 4;
    exitStart.y = anchor.y + 4;
    if (!fixtureplan::garrisonExit(static_cast<std::uint32_t>(stackId.value),
                                    anchor.x, anchor.y, exitDest.x, exitDest.y)) {
        // Retained generated-map fixture geometry only, not a universal capital-exit algorithm.
        exitDest.x = anchor.x + 5;
        exitDest.y = anchor.y + 5;
    }
    return requestedX == exitDest.x && requestedY == exitDest.y;
}

bool isAllowedExactRoute(const game::CMidgardID& stackId,
                         const game::CMqPoint& start,
                         const game::CMqPoint& target)
{
    return fixtureplan::allowsExactRoute(static_cast<std::uint32_t>(stackId.value),
                                          start.x, start.y, target.x, target.y);
}

bool isExcludedMoveTile(const game::CMqPoint& point)
{
    return fixtureplan::isExcludedTile(point.x, point.y);
}

} // namespace

bool preflightHostMoveRoute(bool requested)
{
    if (!requested)
        return true;
    return executablefingerprint::isExactRussobit()
           && netintercept::canClaimSecondaryTxCallback(&legacyHostMoveTxGate);
}

bool commitHostMoveRoute(bool requested,
                         bool exactLegacyIntent,
                         bool cleanLongIntent)
{
    g_exactLegacyIntentCommitted.store(exactLegacyIntent, std::memory_order_release);
    g_cleanLongIntentCommitted.store(cleanLongIntent, std::memory_order_release);
    if (!requested)
        return true;
    if (exactLegacyIntent && cleanLongIntent)
        return false;
    if (!netintercept::installed()
        || !netintercept::claimSecondaryTxCallback(&legacyHostMoveTxGate)) {
        return false;
    }
    g_hostMoveRouteCommitted.store(true, std::memory_order_release);
    g_exactLegacyIntentCommitted.store(exactLegacyIntent, std::memory_order_release);
    g_cleanLongIntentCommitted.store(cleanLongIntent, std::memory_order_release);
    spdlog::info(
        "[worldactions] exact legacy host move route committed "
        "(join remains native, exact-old-intent={}, clean-long-intent={})",
        exactLegacyIntent,
        cleanLongIntent);
    return true;
}

bool submitExactLegacyIntent(game::CPhaseGame* phaseGame,
                             const game::CMidgardID& ownerId,
                             const game::CMidgardID& stackId,
                             const game::CMqPoint& start,
                             const game::CMqPoint& target)
{
    // Exact port of lobby commit d77c9e9
    // MessageWireFormat.BuildCStackMoveMsg.  This is intentionally not a
    // pathfinder: the green source supplied this one semantic intent and let
    // the authoritative game validate/apply it.
    int steps = std::max(std::abs(target.x - start.x), std::abs(target.y - start.y));
    if (steps < 1)
        steps = 1;
    if (steps > 60)
        steps = 60; // legacy builder's exact safety cap

    game::List<game::Pair<game::CMqPoint, int>> path;
    listInit(path);
    int x = start.x;
    int y = start.y;
    for (int i = 0; i <= steps; ++i) {
        game::Pair<game::CMqPoint, int> waypoint;
        waypoint.first.x = x;
        waypoint.first.y = y;
        waypoint.second = i * 3;
        listPushBack(path, waypoint);
        if (x < target.x)
            ++x;
        else if (x > target.x)
            --x;
        if (y < target.y)
            ++y;
        else if (y > target.y)
            --y;
    }

    const bool sent = submitStackMoveOnce(
        phaseGame, ownerId, stackId, path, start, target);
    listFree(path);
    return sent;
}

bool moveStack(const char* stackIdStr,
               int expectedFromX,
               int expectedFromY,
               int expectedMovement,
               int targetX,
               int targetY)
{
    using namespace game;

    // Resolve the exact live phase. The common native sender performs the
    // final stock/overlay admission check immediately before its one send.
    CPhaseGame* phaseGame = testdrv::livePhaseGame();
    if (!phaseGame || !phaseGame->data)
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    CMidgardID stackId{};
    if (!parseWireId(stackIdStr, stackId))
        return false;

    CMidStack* stack = hooks::getStack(objectMap, &stackId);
    if (!stack)
        return false;
    if (stack->ownerId != localPlayerId())
        return false; // only move our own stacks
    // Validate the optional causal source MP before either the garrison or map
    // branch can submit. CMidStack stores movement in one byte.
    if (expectedMovement < -1 || expectedMovement > 255
        || (expectedMovement >= 0
            && static_cast<int>(stack->movement) != expectedMovement))
        return false;

    const auto& fn = gameFunctions();
    auto* plan = fn.getMidgardPlan(objectMap);
    const auto* midgardMap = hooks::getMidgardMap(objectMap);
    if (!plan || !midgardMap)
        return false;
    const int mapSize = midgardMap->mapSize;
    if (mapSize <= 0)
        return false;
    // Reject an off-map target before route construction or native submission.
    if (targetX < 0 || targetY < 0 || targetX >= mapSize || targetY >= mapSize)
        return false;

    // Stack-leader movement properties, derived the same way the engine's pathfinder/preview does
    // (movepathhooks.cpp showMovementPathHooked): waterOnly + per-ground movement bonuses + leaderAlive.
    auto* leaderObj = objectMap->vftable->findScenarioObjectById(objectMap, &stack->leaderId);
    auto* leader = static_cast<const CMidUnit*>(leaderObj);
    if (!leader || !leader->unitImpl)
        return false;
    auto* unitImpl = leader->unitImpl;
    auto* soldier = fn.castUnitImplToSoldier(unitImpl);
    const bool waterOnly = soldier && soldier->vftable->getWaterOnly(soldier);
    auto* leaderExt = fn.castUnitImplToStackLeader(unitImpl);
    const auto& ground = GroundCategories::get();
    const bool plainsBonus = leaderExt && leaderExt->vftable->hasMovementBonus(leaderExt, ground.plain);
    const bool forestBonus = leaderExt && leaderExt->vftable->hasMovementBonus(leaderExt, ground.forest);
    const bool waterBonus = leaderExt && leaderExt->vftable->hasMovementBonus(leaderExt, ground.water);
    const bool leaderAlive = stack->leaderAlive;

    const CMqPoint start = stack->position;

    // Garrison exit: a hero INSIDE its capital (insideId set) sits at the fort ANCHOR, which is not a
    // walkable tile, so the Dijkstra below finds nothing. Exiting is a free move the game applies
    // specially; resolve one pinned test-fixture gate and send its direct garrison-cell -> gate path.
    if (stack->insideId != emptyId) {
        // Garrison exit, replicated EXACTLY from a real mouse-click exit captured in the send hook:
        // the reported stack->position is the fort ANCHOR, but the game moves the hero from its real
        // garrison cell (anchor + (4,4)) to the gate (anchor + (5,5)) as a SINGLE 0-cost diagonal step
        // (real capture for anchor (33,9): path (37,13:0) (38,14:0)). Starting from the anchor - as a
        // normal path would - is why the server silently dropped the earlier attempts.
        // The resolver below contains only D2_TESTDRV fixture geometry; it must never leak into a
        // non-test code path.
        CMqPoint exitStart;
        CMqPoint exitDest;
        if (!resolveGarrisonExit(stackId, start, targetX, targetY, exitStart, exitDest))
            return false;
        if (exitStart.x != expectedFromX || exitStart.y != expectedFromY)
            return false; // stale/wrong garrison-cell precondition: never send
        List<Pair<CMqPoint, int>> exitPath;
        listInit(exitPath);
        Pair<CMqPoint, int> a;
        a.first = exitStart;
        a.second = 0;
        listPushBack(exitPath, a);
        Pair<CMqPoint, int> b;
        b.first = exitDest;
        b.second = 0;
        listPushBack(exitPath, b);
        const bool sent = submitStackMoveOnce(
            phaseGame, stack->ownerId, stackId, exitPath, exitStart, exitDest);
        listFree(exitPath);
        return sent;
    }

    // The old green harness supplied fromx/fromy for every injected move. Keep
    // that causal source exact: a stale command is rejected before pathfinding
    // or network publication and is never repaired from the live position.
    if (start.x != expectedFromX || start.y != expectedFromY)
        return false;
    // A nonnegative movement token makes a later traversal of the same edge a
    // new causal intent only after the previous move was really applied. The
    // sentinel -1 preserves the old target-only no-refire identity for legacy
    // source actions and attacks.
    if (start.x == targetX && start.y == targetY)
        return false; // already there

    CMqPoint requestedTarget;
    requestedTarget.x = targetX;
    requestedTarget.y = targetY;
    const IdType stackType = IdType::Stack;
    const CMidgardID* occupiedStackId =
        CMidgardPlanApi::get().getObjectId(plan, &requestedTarget, &stackType);
    CMidStack* occupiedStack =
        occupiedStackId ? hooks::getStack(objectMap, occupiedStackId) : nullptr;
    const bool exactAttack = occupiedStack && occupiedStack->id != stackId
                             && occupiedStack->ownerId != stack->ownerId;

    const bool exactLegacyIntent =
        g_exactLegacyIntentCommitted.load(std::memory_order_acquire);
    const bool cleanLongIntent =
        g_cleanLongIntentCommitted.load(std::memory_order_acquire);
    const bool pinnedCleanLongMove =
        cleanLongIntent && isAllowedExactRoute(stackId, start, requestedTarget);
    if (cleanLongIntent && !pinnedCleanLongMove) {
        spdlog::error(
            "[worldactions] clean-long plan rejected non-fixture move {} ({},{})->({},{})",
            stackIdStr,
            start.x,
            start.y,
            requestedTarget.x,
            requestedTarget.y);
        return false;
    }
    if (exactAttack || exactLegacyIntent) {
        // The old source never used its ordinary pathfinder for these HTTP
        // actions. Keep every historical/canonical intent exact and let the
        // server own occupied-target truncation. Only an explicitly selected,
        // separately pinned clean-long plan enters native-cost Dijkstra.
        if (occupiedStack && !exactAttack)
            return false;
        return submitExactLegacyIntent(
            phaseGame, stack->ownerId, stackId, start, requestedTarget);
    }

    // --- Dijkstra over 8-connected tiles, weighted by the GAME's own per-tile enter-cost
    // (computeMovementCost) and gated by its passability (stackCanMoveToPosition). Only the visit
    // order is ours; every cost/passability decision is a native game function, so the route matches
    // what the engine's planner would pick (cheapest legal, obstacle-avoiding path).
    const int cells = mapSize * mapSize;
    constexpr int kInf = 0x7fffffff;
    std::vector<int> dist(cells, kInf);
    std::vector<int> parent(cells, -1);
    auto index = [mapSize](int x, int y) { return y * mapSize + x; };

    if (start.x < 0 || start.y < 0 || start.x >= mapSize || start.y >= mapSize)
        return false;
    const int startIdx = index(start.x, start.y);
    dist[startIdx] = 0;

    // CMidgardPlan is the local player's fog-limited spatial view. In r1/r2 it
    // omitted the unrevealed neutral stack at (30,13), so
    // stackCanMoveToPosition legitimately admitted its adjacent guard tile
    // (29,14); entering that tile started combat and abandoned the requested
    // free destination.
    // Snapshot every live stack from the authoritative object map once, before
    // path search. This changes only graph admission before the sole native
    // submission; there is no alternate send, retry, or recovery path.
    std::vector<bool> stackGuardCells;
    if (pinnedCleanLongMove) {
        stackGuardCells.assign(cells, false);
        hooks::forEachScenarioObject(
            objectMap,
            IdType::Stack,
            [&](const IMidScenarioObject* object) {
                if (!object || object->id == stackId)
                    return;
                const CMidStack* other = hooks::getStack(objectMap, &object->id);
                if (!other)
                    return;
                // The pinned clean proof is deliberately conservative and
                // excludes every 3x3 stack guard zone from its sole route.
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int x = other->position.x + dx;
                        const int y = other->position.y + dy;
                        if (x >= 0 && y >= 0 && x < mapSize && y < mapSize)
                            stackGuardCells[index(x, y)] = true;
                    }
                }
            });
    }

    using PqNode = std::pair<int, int>; // (cost, idx)
    std::priority_queue<PqNode, std::vector<PqNode>, std::greater<PqNode>> pq;
    pq.push({0, startIdx});

    while (!pq.empty()) {
        const int d = pq.top().first;
        const int cur = pq.top().second;
        pq.pop();
        if (d != dist[cur])
            continue;
        const int cx = cur % mapSize;
        const int cy = cur / mapSize;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                const int nx = cx + dx;
                const int ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= mapSize || ny >= mapSize)
                    continue;
                CMqPoint np;
                np.x = nx;
                np.y = ny;
                // stackCanMoveToPosition deliberately treats an enemy-occupied
                // tile as a legal attack destination.  That is correct for the
                // exactAttack branch above, but never for an intermediate node
                // of an ordinary move to a different, free target: replaying
                // such a path starts that battle and abandons the requested
                // destination.  Keep every occupied stack cell out of this
                // ordinary-movement graph; this selects one legal route before
                // the sole submission and is not a runtime fallback/retry.
                if (pinnedCleanLongMove && stackGuardCells[index(nx, ny)])
                    continue;
                if (!fn.stackCanMoveToPosition(objectMap, &np, stack, plan))
                    continue;
                const int cost = fn.computeMovementCost(&np, objectMap, midgardMap, plan, &stackId,
                                                        nullptr, nullptr, leaderAlive, plainsBonus,
                                                        forestBonus, waterBonus, waterOnly, true);
                if (cost <= 0)
                    continue; // forbidden tile
                const int nIdx = index(nx, ny);
                const int nd = d + cost;
                if (nd < dist[nIdx]) {
                    dist[nIdx] = nd;
                    parent[nIdx] = cur;
                    pq.push({nd, nIdx});
                }
            }
        }
    }

    // Occupied non-enemy tiles are never a movement fallback. Enemy occupancy was handled above
    // by the exact legacy attack intent; an ordinary move must reach the requested tile itself.
    if (occupiedStack || dist[index(targetX, targetY)] == kInf)
        return false;
    const int destIdx = index(targetX, targetY);
    if (destIdx < 0 || destIdx == startIdx)
        return false; // nowhere to go
    if (pinnedCleanLongMove && dist[destIdx] > static_cast<int>(stack->movement)) {
        spdlog::error(
            "[worldactions] pinned clean-long route costs {} but stack has {} MP",
            dist[destIdx],
            static_cast<int>(stack->movement));
        return false;
    }
    // Reconstruct the route including its start tile; the native wire path includes that tile.
    std::vector<CMqPoint> route;
    for (int at = destIdx; at != -1; at = parent[at]) {
        CMqPoint p;
        p.x = at % mapSize;
        p.y = at / mapSize;
        route.push_back(p);
    }
    std::reverse(route.begin(), route.end()); // was dest..start, now start..dest

    if (pinnedCleanLongMove) {
        for (const auto& tile : route) {
            if (isExcludedMoveTile(tile)) {
                spdlog::error(
                    "[worldactions] pinned clean-long route intersects popup zone at ({},{})",
                    tile.x,
                    tile.y);
                return false;
            }
        }
        spdlog::info(
            "[worldactions] pinned clean-long route validated (cost={}, destination=({},{}))",
            dist[destIdx],
            targetX,
            targetY);
    }

    // Build the wire path List<Pair<CMqPoint,int>> {tile, cumulative move points} (the 12B element the
    // lobby capture confirmed). The path INCLUDES the start tile as element 0 (cumMp 0): the server
    // replays it as path[i] -> path[i+1], so a start-excluded path of length 1 made it read path[1]
    // out of bounds and the apply AV'd. The cumulative cost per tile is the Dijkstra distance, summed
    // from the game's own per-tile computeMovementCost.
    List<Pair<CMqPoint, int>> wirePath;
    listInit(wirePath);
    for (const auto& tile : route) {
        Pair<CMqPoint, int> wp;
        wp.first = tile;
        wp.second = dist[index(tile.x, tile.y)];
        listPushBack(wirePath, wp);
    }

    // Ordinary movement has one destination: the requested reachable unoccupied tile.
    const bool sent = submitStackMoveOnce(
        phaseGame, stack->ownerId, stackId, wirePath, start, requestedTarget);
    listFree(wirePath);
    return sent;
}

// Generic client gestures: repeatable, guarded by current UI identity and native admission.
bool moveStack(const char* stackIdStr, int targetX, int targetY)
{
    using namespace game;

    // Match the stock UI admission before doing any path work or sending a net-message. The raw turn
    // bit is insufficient while the existing phase object-lock predicate is still busy applying a
    // command; bypassing that lock can make a direct test action disappear or overlap a prior move.
    CPhaseGame* phaseGame = testdrv::livePhaseGame();
    if (!phaseGame || !testdrv::strategicActionReady())
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    CMidgardID stackId{};
    if (!parseGenericId(stackIdStr, stackId))
        return false;
    if (stackId == emptyId)
        return false;

    CMidStack* stack = hooks::getStack(objectMap, &stackId);
    if (!stack)
        return false;
    if (stack->ownerId != localPlayerId())
        return false; // only move our own stacks

    const auto& fn = gameFunctions();
    auto* plan = fn.getMidgardPlan(objectMap);
    const auto* midgardMap = hooks::getMidgardMap(objectMap);
    if (!plan || !midgardMap)
        return false;
    const int mapSize = midgardMap->mapSize;
    if (mapSize <= 0)
        return false;
    // Reject an off-map target up front: otherwise the nearest-reachable fallback below would still
    // send the off-map tile as the message `end`, and moveStack would report success for a move the
    // host then rejects. (The garrison-exit sub-step passes anchor+5, which is on-map.)
    if (targetX < 0 || targetY < 0 || targetX >= mapSize || targetY >= mapSize)
        return false;

    // Stack-leader movement properties, derived the same way the engine's pathfinder/preview does
    // (movepathhooks.cpp showMovementPathHooked): waterOnly + per-ground movement bonuses + leaderAlive.
    auto* leaderObj = objectMap->vftable->findScenarioObjectById(objectMap, &stack->leaderId);
    auto* leader = static_cast<const CMidUnit*>(leaderObj);
    if (!leader || !leader->unitImpl)
        return false;
    auto* unitImpl = leader->unitImpl;
    auto* soldier = fn.castUnitImplToSoldier(unitImpl);
    const bool waterOnly = soldier && soldier->vftable->getWaterOnly(soldier);
    auto* leaderExt = fn.castUnitImplToStackLeader(unitImpl);
    const auto& ground = GroundCategories::get();
    const bool plainsBonus = leaderExt && leaderExt->vftable->hasMovementBonus(leaderExt, ground.plain);
    const bool forestBonus = leaderExt && leaderExt->vftable->hasMovementBonus(leaderExt, ground.forest);
    const bool waterBonus = leaderExt && leaderExt->vftable->hasMovementBonus(leaderExt, ground.water);
    const bool leaderAlive = stack->leaderAlive;

    const CMqPoint start = stack->position;
    if (start.x == targetX && start.y == targetY)
        return false; // already there

    // Garrison exit: a hero INSIDE its capital (insideId set) sits at the fort ANCHOR, which is not a
    // walkable tile, so the Dijkstra below finds nothing. Exiting is a free move the game applies
    // specially; send a direct anchor->exit-tile path (the test's +5 sub-step) and let the server exit
    // the hero, then subsequent moves run as a normal free stack.
    if (stack->insideId != emptyId) {
        // This command surface supports the single observed capital-exit gesture only. Do not
        // silently replace an arbitrary caller target with the fixture gate.
        if (targetX != start.x + 5 || targetY != start.y + 5)
            return false;
        // Garrison exit, replicated EXACTLY from a real mouse-click exit captured in the send hook:
        // the reported stack->position is the fort ANCHOR, but the game moves the hero from its real
        // garrison cell (anchor + (4,4)) to the gate (anchor + (5,5)) as a SINGLE 0-cost diagonal step
        // (real capture for anchor (33,9): path (37,13:0) (38,14:0)). Starting from the anchor - as a
        // normal path would - is why the server silently dropped the earlier attempts.
        // NOTE: the +4/+5 offsets are this capital's geometry, a test fixture (D2_TESTDRV only); they
        // are not general and must never leak into a non-test code path.
        CMqPoint exitStart;
        exitStart.x = start.x + 4;
        exitStart.y = start.y + 4;
        CMqPoint exitDest;
        exitDest.x = start.x + 5;
        exitDest.y = start.y + 5;
        List<Pair<CMqPoint, int>> exitPath;
        listInit(exitPath);
        Pair<CMqPoint, int> a;
        a.first = exitStart;
        a.second = 0;
        listPushBack(exitPath, a);
        Pair<CMqPoint, int> b;
        b.first = exitDest;
        b.second = 0;
        listPushBack(exitPath, b);
        CPhaseGameApi::get().sendStackMoveMsg(phaseGame, &stackId, &exitPath, &exitStart, &exitDest);
        listFree(exitPath);
        return true;
    }

    // --- Dijkstra over 8-connected tiles, weighted by the GAME's own per-tile enter-cost
    // (computeMovementCost) and gated by its passability (stackCanMoveToPosition). Only the visit
    // order is ours; every cost/passability decision is a native game function, so the route matches
    // what the engine's planner would pick (cheapest legal, obstacle-avoiding path).
    const int cells = mapSize * mapSize;
    constexpr int kInf = 0x7fffffff;
    std::vector<int> dist(cells, kInf);
    std::vector<int> parent(cells, -1);
    auto index = [mapSize](int x, int y) { return y * mapSize + x; };

    if (start.x < 0 || start.y < 0 || start.x >= mapSize || start.y >= mapSize)
        return false;
    const int startIdx = index(start.x, start.y);
    dist[startIdx] = 0;
    using PqNode = std::pair<int, int>; // (cost, idx)
    std::priority_queue<PqNode, std::vector<PqNode>, std::greater<PqNode>> pq;
    pq.push({0, startIdx});

    while (!pq.empty()) {
        const int d = pq.top().first;
        const int cur = pq.top().second;
        pq.pop();
        if (d != dist[cur])
            continue;
        const int cx = cur % mapSize;
        const int cy = cur / mapSize;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                const int nx = cx + dx;
                const int ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= mapSize || ny >= mapSize)
                    continue;
                CMqPoint np;
                np.x = nx;
                np.y = ny;
                if (!fn.stackCanMoveToPosition(objectMap, &np, stack, plan))
                    continue;
                const int cost = fn.computeMovementCost(&np, objectMap, midgardMap, plan, &stackId,
                                                        nullptr, nullptr, leaderAlive, plainsBonus,
                                                        forestBonus, waterBonus, waterOnly, true);
                if (cost <= 0)
                    continue; // forbidden tile
                const int nIdx = index(nx, ny);
                const int nd = d + cost;
                if (nd < dist[nIdx]) {
                    dist[nIdx] = nd;
                    parent[nIdx] = cur;
                    pq.push({nd, nIdx});
                }
            }
        }
    }

    // Destination: the requested tile if reachable, else the reachable tile closest to it (mirrors a
    // player clicking a far/blocked tile -> the stack moves toward it as far as it can).
    int destIdx = -1;
    if (targetX >= 0 && targetY >= 0 && targetX < mapSize && targetY < mapSize
        && dist[index(targetX, targetY)] != kInf) {
        destIdx = index(targetX, targetY);
    } else {
        int best = kInf;
        int bestCost = kInf;
        for (int i = 0; i < cells; ++i) {
            if (dist[i] == kInf || i == startIdx)
                continue;
            const int ax = std::abs((i % mapSize) - targetX);
            const int ay = std::abs((i / mapSize) - targetY);
            const int cheb = (ax > ay) ? ax : ay;
            // Smallest Chebyshev ring around the target; WITHIN a ring, the CHEAPEST tile to reach
            // (least path cost). So the stack stops on the side it approached from and enters a site
            // (camp/city) from there, instead of pathing around to a fixed lowest-index cell on the far
            // side. A hero coming from the south thus enters a building from below, as a player expects.
            if (cheb < best || (cheb == best && dist[i] < bestCost)) {
                best = cheb;
                bestCost = dist[i];
                destIdx = i;
            }
        }
    }
    if (destIdx < 0 || destIdx == startIdx)
        return false; // nowhere to go

    // Reconstruct the route including the start tile, required by the wire format below.
    std::vector<CMqPoint> route;
    for (int at = destIdx; at != -1; at = parent[at]) {
        CMqPoint p;
        p.x = at % mapSize;
        p.y = at / mapSize;
        route.push_back(p);
    }
    std::reverse(route.begin(), route.end()); // was dest..start, now start..dest

    // Build the wire path List<Pair<CMqPoint,int>> {tile, cumulative move points} (the 12B element the
    // lobby capture confirmed). The path INCLUDES the start tile as element 0 (cumMp 0): the server
    // replays it as path[i] -> path[i+1], so a start-excluded path of length 1 made it read path[1]
    // out of bounds and the apply AV'd. The cumulative cost per tile is the Dijkstra distance, summed
    // from the game's own per-tile computeMovementCost.
    List<Pair<CMqPoint, int>> wirePath;
    listInit(wirePath);
    for (const auto& tile : route) {
        Pair<CMqPoint, int> wp;
        wp.first = tile;
        wp.second = dist[index(tile.x, tile.y)];
        listPushBack(wirePath, wp);
    }

    // The message `end` is the REQUESTED target, not the tile the hero stops on. For a normal move to a
    // reachable tile they are the same; for an ATTACK the target is an occupied enemy tile (the Dijkstra
    // stops adjacent), and end=enemy is what makes the server start a battle - exactly the captured
    // mouse-click attack format (path stops adjacent at the last reachable, end is the enemy tile).
    CMqPoint reqTarget;
    reqTarget.x = targetX;
    reqTarget.y = targetY;
    CPhaseGameApi::get().sendStackMoveMsg(phaseGame, &stackId, &wirePath, &start, &reqTarget);
    listFree(wirePath);
    return true;
}

// Hire the mercenary <unitId> (a camp roster entry the world reporter lists) from camp <campId> into
// the hero stack <stackId>, at the first fitting free slot. Sends the engine's OWN CSiteBuyUnitMsg via
// CPhaseGame::sendSiteBuyUnitMsg (Russobit 0x4067a2) - the exact call the merc-camp drag-drop makes on
// a drop. The acting client (the joiner that walked into the camp) is NOT the server: it SENDS the
// message; the host validates gold, removes the merc from the camp roster, adds it to the group, and
// broadcasts the result, so the hire replicates to every player. (An earlier client-side
// CVisitorAddUnitToGroup was the wrong layer: it is the server's apply step, returns false on a client,
// and would not replicate.) MUST run on the UI thread, own turn. Returns true if the message was sent.
bool hireMerc(const char* campIdStr, const char* stackIdStr, const char* unitIdStr)
{
    using namespace game;

    CPhaseGame* phaseGame = testdrv::livePhaseGame();
    if (!phaseGame || !testdrv::strategicActionReady())
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    CMidgardID campId{}, stackId{}, unitId{};
    if (!parseGenericId(campIdStr, campId))
        return false;
    if (!parseGenericId(stackIdStr, stackId))
        return false;
    if (!parseGenericId(unitIdStr, unitId))
        return false;
    if (campId == emptyId || stackId == emptyId || unitId == emptyId)
        return false;

    CMidStack* stack = hooks::getStack(objectMap, &stackId);
    if (!stack || stack->ownerId != localPlayerId())
        return false; // only hire into our own stack, on our own turn

    // First fitting free slot, BIG-AWARE: a big occupant on a front cell (even position) also blocks the
    // back cell (pos+1) of its column. A big merc needs a whole free column {2c, 2c+1}; a small merc
    // takes the first non-blocked cell. The camp drag-drop snaps a big unit to its front cell, so the
    // front-cell position is what we send. The merc's size comes from its global unit impl.
    const auto& fn = gameFunctions();
    const auto& global = GlobalDataApi::get();
    const auto globalData = *global.getGlobalData();
    const auto* mercImpl = globalData
                               ? static_cast<const IUsUnit*>(global.findById(globalData->units, &unitId))
                               : nullptr;
    auto* mercSoldier = mercImpl ? fn.castUnitImplToSoldier(mercImpl) : nullptr;
    const bool mercIsBig = mercSoldier && !mercSoldier->vftable->getSizeSmall(mercSoldier);

    auto* group = &stack->group;
    bool blocked[6] = {false, false, false, false, false, false};
    for (int p = 0; p < 6; ++p) {
        const CMidgardID* uid = CMidUnitGroupApi::get().getUnitIdByPosition(group, p);
        if (!uid || *uid == emptyId)
            continue;
        blocked[p] = true;
        if (p % 2 == 0) { // front cell: a big occupant also blocks the back cell of this column
            auto* uObj = objectMap->vftable->findScenarioObjectById(objectMap, uid);
            auto* u = static_cast<const CMidUnit*>(uObj);
            auto* s = (u && u->unitImpl) ? fn.castUnitImplToSoldier(u->unitImpl) : nullptr;
            if (s && !s->vftable->getSizeSmall(s))
                blocked[p + 1] = true;
        }
    }
    int freePos = -1;
    if (mercIsBig) {
        for (int c = 0; c < 3; ++c)
            if (!blocked[2 * c] && !blocked[2 * c + 1]) {
                freePos = 2 * c;
                break;
            }
    } else {
        for (int p = 0; p < 6; ++p)
            if (!blocked[p]) {
                freePos = p;
                break;
            }
    }
    if (freePos < 0)
        return false; // no fitting free slot

    spdlog::info("[testdrv] hireMerc: camp={} stack={} unit={} pos={} big={}", campIdStr, stackIdStr,
                 unitIdStr, freePos, mercIsBig);

    // CPhaseGame::sendSiteBuyUnitMsg(phaseGame, &siteId, &stackId, &unitId, position): __thiscall, pushes
    // the three ids + position into a CSiteBuyUnitMsg and sends it to the server via data->midClient,
    // identical to the merc-camp drop handler. The server apply (CSiteBuyUnitMsg handler -> 0x5d8d93)
    // casts the site to CMidSiteMercs, charges gold, drops the merc from the roster, adds it, broadcasts.
    using SendSiteBuyUnitMsgFn = void(__thiscall*)(CPhaseGame*, const CMidgardID*, const CMidgardID*,
                                                   const CMidgardID*, int);
    auto sendSiteBuyUnitMsg = reinterpret_cast<SendSiteBuyUnitMsgFn>(0x4067a2);
    sendSiteBuyUnitMsg(phaseGame, &campId, &stackId, &unitId, freePos);
    return true;
}

// Move the unit at <sourcePos> to <targetPos> within stack <stackId>'s 6-cell formation. Sends the
// engine's OWN CStackSwapUnitMsg via CPhaseGame::sendStackSwapUnitMsg (Russobit 0x406cc7), the exact
// call the formation drag-drop makes. The host applies it (CVisitorSwapUnitPosition) and broadcasts a
// CCmdUpdateObjMsg, so the rearrange REPLICATES to every player. If <targetPos> is EMPTY this is a plain
// MOVE (the source cell empties); if it is OCCUPIED it is a SWAP (the two cells exchange). Use it to
// drop a just-hired unit into a free slot, or to set the battle line (ranged/casters back, melee front).
// MUST be called on the UI thread. Returns true if the message was sent.
//
// Key detail (verified by RE + live): the engine's swap visitor validates the MOVED unit's position (it
// must hold a unit) and ALLOWS the other to be empty. That validated position is the message's FIRST
// position field, so <sourcePos> (the unit being moved) goes there and <targetPos> second. We gate:
//  - positions 0..5 and distinct;
//  - the SOURCE cell holds a unit (the engine rejects an empty source);
//  - the stack is ours, on our turn (the send has no clientTakesTurn gate, so we add one).
// A leader may be moved/swapped to any cell (it is never dismissed by this message). A big unit is
// anchored to its FRONT (even) cell of a column; address it by that cell.
bool moveGroupUnit(const char* stackIdStr, int sourcePos, int targetPos)
{
    using namespace game;

    // The swap send (0x406cc7) has NO clientTakesTurn gate of its own (unlike the hire), so we gate it.
    CPhaseGame* phaseGame = testdrv::livePhaseGame();
    if (!phaseGame || !testdrv::strategicActionReady())
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    if (sourcePos < 0 || sourcePos > 5 || targetPos < 0 || targetPos > 5 || sourcePos == targetPos)
        return false;

    CMidgardID stackId{};
    if (!parseGenericId(stackIdStr, stackId))
        return false;
    if (stackId == emptyId)
        return false;

    CMidStack* stack = hooks::getStack(objectMap, &stackId);
    if (!stack || stack->ownerId != localPlayerId())
        return false; // only rearrange our own stack, on our own turn

    // The SOURCE cell (the moved unit) must hold a unit; the engine rejects an empty source. The TARGET
    // may be empty (plain move, source empties) or occupied (swap). Gate only the source.
    const auto& groups = CMidUnitGroupApi::get();
    const CMidgardID* srcUnit = groups.getUnitIdByPosition(&stack->group, sourcePos);
    if (!srcUnit || *srcUnit == emptyId)
        return false;
    const CMidgardID* tgtUnit = groups.getUnitIdByPosition(&stack->group, targetPos);
    const bool targetEmpty = (!tgtUnit || *tgtUnit == emptyId);

    spdlog::info("[testdrv] moveGroupUnit: stack={} {} {}->{}", stackIdStr, targetEmpty ? "move" : "swap",
                 sourcePos, targetPos);

    // CPhaseGame::sendStackSwapUnitMsg(phaseGame, posA, &stackIdA, posB, &stackIdB): in-group move uses
    // the hero stack as BOTH ids. posA is the FIRST position field, which the engine requires occupied,
    // so it is the MOVED unit (sourcePos); posB (targetPos) is the destination and may be empty. The host
    // runs CVisitorSwapUnitPosition and broadcasts the result.
    using SendStackSwapUnitMsgFn = void(__thiscall*)(CPhaseGame*, int, const CMidgardID*, int,
                                                     const CMidgardID*);
    auto sendStackSwapUnitMsg = reinterpret_cast<SendStackSwapUnitMsgFn>(0x406cc7);
    sendStackSwapUnitMsg(phaseGame, sourcePos, &stackId, targetPos, &stackId);
    return true;
}

// Dismiss the unit <unitId> from stack <stackId> (remove it, freeing its slot). Sends the engine's OWN
// CStackDismissUnitMsg via CPhaseGame::sendStackDismissUnitMsg (Russobit 0x406f47), the exact call the
// manage-stack dismiss makes. The host applies it and broadcasts, so the removal REPLICATES to every
// player. Use it to drop a low-value unit so a more valuable (or 2-slot) one fits. NEVER dismisses the
// leader: that is a different, stack-disbanding message (CStackDismissLeaderMsg), and we reject a leader
// id outright (the user's hard rule). MUST be called on the UI thread. Returns true if the message was
// sent (own stack, our turn, the unit is a non-leader member of the group).
bool dismissUnit(const char* stackIdStr, const char* unitIdStr)
{
    using namespace game;

    // The dismiss send (0x406f47) has no clientTakesTurn gate of its own (like the swap), so we gate it.
    CPhaseGame* phaseGame = testdrv::livePhaseGame();
    if (!phaseGame || !testdrv::strategicActionReady())
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    CMidgardID stackId{}, unitId{};
    if (!parseGenericId(stackIdStr, stackId))
        return false;
    if (!parseGenericId(unitIdStr, unitId))
        return false;
    if (stackId == emptyId || unitId == emptyId)
        return false;

    CMidStack* stack = hooks::getStack(objectMap, &stackId);
    if (!stack || stack->ownerId != localPlayerId())
        return false; // only dismiss from our own stack, on our own turn

    // NEVER dismiss the leader (hard rule; the engine disbands the stack via a different message).
    if (unitId == stack->leaderId)
        return false;

    // The unit must actually be a member of this stack's group.
    const auto& groups = CMidUnitGroupApi::get();
    bool inGroup = false;
    for (int p = 0; p < 6 && !inGroup; ++p) {
        const CMidgardID* uid = groups.getUnitIdByPosition(&stack->group, p);
        if (uid && *uid == unitId)
            inGroup = true;
    }
    if (!inGroup)
        return false;

    spdlog::info("[testdrv] dismissUnit: stack={} unit={}", stackIdStr, unitIdStr);

    // CPhaseGame::sendStackDismissUnitMsg(phaseGame, &unitId, &stackId): the message carries the unit id
    // first, the stack id second (the exact call the manage-stack dismiss makes). The host removes the
    // unit and broadcasts the result.
    using SendStackDismissUnitMsgFn = void(__thiscall*)(CPhaseGame*, const CMidgardID*,
                                                        const CMidgardID*);
    auto sendStackDismissUnitMsg = reinterpret_cast<SendStackDismissUnitMsgFn>(0x406f47);
    sendStackDismissUnitMsg(phaseGame, &unitId, &stackId);
    return true;
}


} // namespace worldactions
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
