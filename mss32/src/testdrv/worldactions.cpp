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
#include "globaldata.h"
#include "groundcat.h"
#include "mempool.h"
#include "midgard.h"
#include "midgardid.h"
#include "midgardmap.h"
#include "midgardobjectmap.h"
#include "midstack.h"
#include "midunit.h"
#include "midunitgroup.h"
#include "mqpoint.h"
#include "phasegame.h"
#include "phasegamehooks.h"
#include "ussoldier.h"
#include "usstackleader.h"
#include "usunit.h"
#include "utils.h"
#include <algorithm>
#include <spdlog/spdlog.h>
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

    CPhaseGame* phaseGame = hooks::getStashedPhaseGame();
    if (!phaseGame || !phaseGame->data || !phaseGame->data->clientTakesTurn)
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    CMidgardID campId{}, stackId{}, unitId{};
    CMidgardIDApi::get().fromString(&campId, campIdStr);
    CMidgardIDApi::get().fromString(&stackId, stackIdStr);
    CMidgardIDApi::get().fromString(&unitId, unitIdStr);
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
    CPhaseGame* phaseGame = hooks::getStashedPhaseGame();
    if (!phaseGame || !phaseGame->data || !phaseGame->data->clientTakesTurn)
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    if (sourcePos < 0 || sourcePos > 5 || targetPos < 0 || targetPos > 5 || sourcePos == targetPos)
        return false;

    CMidgardID stackId{};
    CMidgardIDApi::get().fromString(&stackId, stackIdStr);
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
    CPhaseGame* phaseGame = hooks::getStashedPhaseGame();
    if (!phaseGame || !phaseGame->data || !phaseGame->data->clientTakesTurn)
        return false;

    const IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return false;

    CMidgardID stackId{}, unitId{};
    CMidgardIDApi::get().fromString(&stackId, stackIdStr);
    CMidgardIDApi::get().fromString(&unitId, unitIdStr);
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
