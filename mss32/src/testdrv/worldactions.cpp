/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * World actions. See testdrv/worldactions.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to nothing and the build is
 * byte-identical to vanilla.
 *
 * moveStack reuses the mod's typed game layer end to end: the path is searched over the game's OWN
 * per-tile enter-cost (computeMovementCost) and passability (stackCanMoveToPosition), annotated with
 * the native PathInfoListApi::populateFromPath, and submitted with CPhaseGameApi::sendStackMoveMsg
 * (the exact call the click handler issues at 0x4ce842) -> the host re-validates and applies it (move
 * points deducted, battle on contact), identical to a real player's click. Only the Dijkstra visit
 * order is ours. Mod style is null-checks, not SEH (the search allocates -> __try would be C2712);
 * crash-safety comes from the thin __try wrapper in autonav (safeMoveStack).
 */

#ifdef D2_TESTDRV

#include "testdrv/worldactions.h"
#include "d2list.h"
#include "d2pair.h"
#include "game.h"
#include "gameutils.h"
#include "groundcat.h"
#include "mempool.h"
#include "midgard.h"
#include "midgardid.h"
#include "midgardmap.h"
#include "midgardobjectmap.h"
#include "midstack.h"
#include "midunit.h"
#include "mqpoint.h"
#include "phasegame.h"
#include "phasegamehooks.h"
#include "ussoldier.h"
#include "usstackleader.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace hooks {
namespace testdrv {
namespace worldactions {

namespace {

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
    if (auto* phaseGame = hooks::getStashedPhaseGame())
        if (phaseGame->data)
            return phaseGame->data->currentPlayerId;
    return game::emptyId;
}

} // namespace

bool moveStack(const char* stackIdStr, int targetX, int targetY)
{
    using namespace game;

    // The move issues a client net-message; only do it on the local player's own turn.
    CPhaseGame* phaseGame = hooks::getStashedPhaseGame();
    if (!phaseGame || !phaseGame->data || !phaseGame->data->clientTakesTurn)
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    CMidgardID stackId{};
    CMidgardIDApi::get().fromString(&stackId, stackIdStr);
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
        for (int i = 0; i < cells; ++i) {
            if (dist[i] == kInf || i == startIdx)
                continue;
            const int ax = std::abs((i % mapSize) - targetX);
            const int ay = std::abs((i / mapSize) - targetY);
            const int cheb = (ax > ay) ? ax : ay;
            if (cheb < best) {
                best = cheb;
                destIdx = i;
            }
        }
    }
    if (destIdx < 0 || destIdx == startIdx)
        return false; // nowhere to go

    // Reconstruct the route start..dest. The wire path is the sequence of DESTINATION tiles only: the
    // engine never sends a move node on the stack's own tile (the current position is carried
    // separately as `start`), so the start tile is dropped below.
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

} // namespace worldactions
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
