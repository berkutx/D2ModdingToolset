/*
 * Optional exact-instance transfer from a fort to its visiting stack, followed
 * by explicitly requested native HP changes.
 * All data comes from the immutable fixture plan; preparation runs only at
 * the first authoritative server turn-zero, never on a UI frame or retry.
 */
#ifdef D2_TESTDRV
#include "testdrv/fixture_reinforcement.h"
#include "testdrv/fixtureplan.h"
#include "testdrv/testenv.h"
#include "fortification.h"
#include "game.h"
#include "gameutils.h"
#include "idset.h"
#include "midgardscenariomap.h"
#include "midmsgsender.h"
#include "midserverlogic.h"
#include "midstack.h"
#include "midunit.h"
#include "usunit.h"
#include "visitors.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks::testdrv::fixture_reinforcement {
namespace {
using fixtureplan::Operation;
struct LiveOperation {
    game::CMidStack* stack{};
    game::CFortification* fort{};
    game::CMidUnit* leader{};
    std::array<game::CMidUnit*, 6> units{};
};
struct LiveHealth {
    game::CMidStack* stack{};
    game::CMidUnit* unit{};
};
enum class CommitState { Armed, Claimed, Committed };
std::atomic<CommitState> g_state{CommitState::Armed};

[[noreturn]] void failFast(const char* reason)
{
    spdlog::critical("[testdrv][fixture-reinforcement] FAILED: {}; terminating", reason);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), 0xD2E77331u);
    std::abort();
}
void require(bool condition, const char* reason)
{
    if (!condition) failFast(reason);
}
bool exactHostRole()
{
    char role[16]{};
    const DWORD size = GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
    return size == 4 && std::strcmp(role, "host") == 0;
}

bool validVector(const game::IdVector& vector)
{
    const auto begin = reinterpret_cast<std::uintptr_t>(vector.bgn);
    const auto end = reinterpret_cast<std::uintptr_t>(vector.end);
    const auto allocatedEnd = reinterpret_cast<std::uintptr_t>(vector.allocatedMemEnd);
    if (!begin && !end && !allocatedEnd)
        return true;
    return begin && end && allocatedEnd && begin <= end && end <= allocatedEnd
           && (end - begin) % sizeof(game::CMidgardID) == 0
           && (allocatedEnd - begin) % sizeof(game::CMidgardID) == 0;
}

std::size_t vectorSize(const game::IdVector& vector)
{
    if (!vector.bgn)
        return 0;
    return static_cast<std::size_t>(vector.end - vector.bgn);
}

int countId(const game::IdVector& vector, const game::CMidgardID& id)
{
    int count = 0;
    for (auto it = vector.bgn; it != vector.end; ++it) {
        if (*it == id)
            ++count;
    }
    return count;
}

int countPosition(const game::CMidUnitGroup& group, const game::CMidgardID& id)
{
    int count = 0;
    for (const auto& position : group.positions) {
        if (position == id)
            ++count;
    }
    return count;
}

bool exactPositionCells(const game::CMidUnitGroup& group,
                        const std::array<game::CMidgardID, 6>& expected)
{
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (group.positions[i] != expected[i])
            return false;
    }
    return true;
}

bool exactUnitSet(const game::CMidUnitGroup& group,
                  const std::array<game::CMidgardID, 6>& expected,
                  std::size_t expectedCount)
{
    if (!validVector(group.units) || vectorSize(group.units) != expectedCount)
        return false;
    for (std::size_t i = 0; i < expectedCount; ++i) {
        if (countId(group.units, expected[i]) != 1)
            return false;
    }
    return true;
}

bool exactDestinationGroup(const game::CMidUnitGroup& group,
                           const fixtureplan::Group& expected)
{
    if (!validVector(group.units) || vectorSize(group.units) != expected.count)
        return false;
    for (std::size_t i = 0; i < expected.count; ++i) {
        if (group.units.bgn[i] != expected.units[i])
            return false;
    }
    return exactPositionCells(group, expected.cells);
}

game::CMidUnit* exactUnit(game::CMidgardScenarioMap* objectMap,
                         const game::CMidgardID& id)
{
    return game::gameFunctions().findUnitById(objectMap, &id);
}

LiveOperation preflightOperation(game::CMidgardScenarioMap* objectMap, const Operation& op)
{
    LiveOperation live;
    live.stack = getStack(objectMap, &op.destinationStack);
    live.fort = getFort(objectMap, &op.sourceFort);
    require(live.stack != nullptr, "exact destination stack is missing");
    require(live.fort != nullptr, "exact source fort is missing");
    require(live.stack->id == op.destinationStack, "stack object id mismatch");
    require(live.fort->id == op.sourceFort, "fort object id mismatch");
    require(live.stack->ownerId == op.owner, "stack owner mismatch");
    require(live.fort->ownerId == op.owner, "fort owner mismatch");
    require(live.stack->insideId == op.sourceFort, "stack is not inside the exact fort");
    require(live.fort->stackId == op.destinationStack, "fort visiting stack id mismatch");
    require(live.stack->leaderId == op.leader && live.stack->leaderAlive,
            "stack leader identity/alive state mismatch");
    require(live.stack->position.x == op.anchorX && live.stack->position.y == op.anchorY,
            "stack anchor mismatch");
    // processZero precedes the first CEffectPlayerTurn refresh. Movement still
    // has its scenario pre-turn value; normal turn/deploy evidence is separate.
    require(live.stack->group.maxUnitsAllowed == -1
                || live.stack->group.maxUnitsAllowed >= static_cast<int>(op.destinationAfter.count),
            "destination semantic group limit is too small");
    require(exactDestinationGroup(live.stack->group, op.destinationBefore),
            "destination group prestate mismatch");
    require(exactUnitSet(live.fort->group, op.sourceBefore.units, op.sourceBefore.count),
            "source unit set mismatch");
    require(exactPositionCells(live.fort->group, op.sourceBefore.cells),
            "source position cells mismatch");
    live.leader = exactUnit(objectMap, op.leader);
    require(live.leader && live.leader->currentHp == op.leaderHp,
            "leader unit/HP mismatch");
    for (std::size_t i = 0; i < op.unitCount; ++i) {
        const auto& unit = op.units[i];
        live.units[i] = exactUnit(objectMap, unit.id);
        require(live.units[i] != nullptr, "exact source unit instance is missing");
        require(live.units[i]->unitImpl != nullptr
                    && live.units[i]->unitImpl->id == unit.implementation,
                "source unit implementation mismatch");
        require(live.units[i]->currentHp == unit.hp, "source unit HP mismatch");
        require(countId(live.fort->group.units, unit.id) == 1
                    && countPosition(live.fort->group, unit.id) == 1
                    && countId(live.stack->group.units, unit.id) == 0,
                "source unit group membership mismatch");
        require(getFortByUnitId(objectMap, &unit.id) == live.fort,
                "source unit does not resolve to the exact fort");
    }
    return live;
}

int postHealthHp(const fixtureplan::Plan* plan, const game::CMidgardID& unit, int unchangedHp)
{
    if (plan) for (std::size_t i = 0; i < plan->healthCount; ++i) {
        if (plan->health[i].unit == unit)
            return plan->health[i].newHp;
    }
    return unchangedHp;
}

void verifyPostOperation(game::CMidgardScenarioMap* objectMap, const Operation& op,
                         const LiveOperation& live, const fixtureplan::Plan* healthPlan = nullptr)
{
    require(exactDestinationGroup(live.stack->group, op.destinationAfter),
            "destination group postcondition failed");
    require(exactUnitSet(live.fort->group, op.sourceAfter.units, op.sourceAfter.count),
            "retained source unit set postcondition failed");
    require(exactPositionCells(live.fort->group, op.sourceAfter.cells),
            "retained source position postcondition failed");
    require(live.stack->leaderId == op.leader
                && live.stack->leaderAlive == (postHealthHp(healthPlan, op.leader, op.leaderHp) > 0),
            "leader changed during transfer");
    for (std::size_t i = 0; i < op.unitCount; ++i) {
        const auto& expected = op.units[i];
        require(countId(live.stack->group.units, expected.id) == 1
                    && countPosition(live.stack->group, expected.id) == 1
                    && countId(live.fort->group.units, expected.id) == 0
                    && countPosition(live.fort->group, expected.id) == 0,
                "unit transfer uniqueness postcondition failed");
        auto* unit = exactUnit(objectMap, expected.id);
        require(unit == live.units[i] && unit->unitImpl
                    && unit->unitImpl->id == expected.implementation
                    && unit->currentHp == postHealthHp(healthPlan, expected.id, expected.hp),
                "unit instance/type/HP changed during transfer");
    }
}

using ReinsertUnitIntoGroup = bool(__stdcall*)(const game::CMidgardID* unitId,
                                                const game::CMidgardID* groupId,
                                                int position,
                                                game::IMidgardObjectMap* objectMap,
                                                int apply);
// Exact-build-gated CVisitorReinsertUnitIntoGroup preserves the existing unit
// instance. apply=0 is native CanApply; apply=1 performs the one requested move.
const auto reinsertUnitIntoGroup =
    reinterpret_cast<ReinsertUnitIntoGroup>(0x005E8DC0u);

void preflightVisitors(game::CMidgardScenarioMap* objectMap, const Operation& op)
{
    const auto& visitors = game::VisitorApi::get();
    for (std::size_t i = 0; i < op.unitCount; ++i) {
        const auto& unit = op.units[i];
        require(visitors.extractUnitFromGroup(&unit.id, &op.sourceFort, objectMap, 0),
                "extract visitor preflight failed");
        require(reinsertUnitIntoGroup(&unit.id, &op.destinationStack, unit.destinationSlot,
                                      objectMap, 0), "reinsert visitor preflight failed");
    }
}

void transferOperation(game::CMidgardScenarioMap* objectMap, const Operation& op,
                       const LiveOperation& live)
{
    const auto& visitors = game::VisitorApi::get();
    for (std::size_t i = 0; i < op.unitCount; ++i) {
        const auto& unit = op.units[i];
        require(visitors.extractUnitFromGroup(&unit.id, &op.sourceFort, objectMap, 1),
                "extract visitor apply failed");
        require(exactUnit(objectMap, unit.id) == live.units[i],
                "extract visitor did not preserve the exact unit instance");
        require(countId(live.fort->group.units, unit.id) == 0
                    && countPosition(live.fort->group, unit.id) == 0,
                "extract visitor left the unit in the source");
        require(reinsertUnitIntoGroup(&unit.id, &op.destinationStack, unit.destinationSlot,
                                      objectMap, 1), "reinsert visitor apply failed");
        require(countId(live.stack->group.units, unit.id) == 1
                    && live.stack->group.positions[unit.destinationSlot] == unit.id,
                "reinsert visitor did not place the exact unit once");
    }
}

LiveHealth preflightHealth(game::CMidgardScenarioMap* objectMap,
                           const fixtureplan::HealthPostcondition& health)
{
    LiveHealth live{getStack(objectMap, &health.stack), exactUnit(objectMap, health.unit)};
    require(live.stack && live.stack->id == health.stack && live.unit
                && live.unit->id == health.unit && live.unit->unitImpl
                && live.unit->unitImpl->id == health.implementation,
            "health unit/stack/type prestate mismatch");
    require(live.unit->currentHp == health.expectedHp, "health unit HP prestate mismatch");
    require(health.newHp <= game::CMidUnitApi::get().getHpMax(live.unit),
            "health poststate exceeds native maximum HP");
    // The plan already binds this unit to an exact destinationAfter group.
    // A transferred unit is still in its source fort at this read-only boundary.
    require(game::VisitorApi::get().changeUnitHp(&health.unit,
                health.newHp - health.expectedHp, objectMap, 0),
            "health visitor preflight failed");
    return live;
}

void verifyHealth(game::CMidgardScenarioMap* objectMap,
                  const fixtureplan::HealthPostcondition& health,
                  const LiveHealth& live, bool applied)
{
    require(getStack(objectMap, &health.stack) == live.stack
                && exactUnit(objectMap, health.unit) == live.unit && live.unit->unitImpl
                && live.unit->unitImpl->id == health.implementation
                && live.unit->currentHp == (applied ? health.newHp : health.expectedHp),
            "health unit instance/type/HP postcondition failed");
    require(countId(live.stack->group.units, health.unit) == 1
                && countPosition(live.stack->group, health.unit) > 0,
            "health unit is not in its exact destination stack");
    if (live.stack->leaderId == health.unit)
        require(live.stack->leaderAlive == (live.unit->currentHp > 0),
                "native health visitor did not preserve leader alive parity");
}

void markChanged(game::CMidgardScenarioMap* objectMap, const game::CMidgardID& id)
{
    game::Pair<game::IdSetIterator, bool> result;
    game::IdSetApi::get().insert(&objectMap->changedObjects, &result, &id);
}
} // namespace

void onFirstTurnZero(game::CMidServerLogic* serverLogic)
{
    if (!testenv::on("D2TESTDRV_APPLY_FIXTURE") || !exactHostRole())
        return;
    require(fixtureplan::preflight(), "invalid fixture plan");
    const auto* plan = fixtureplan::get();
    require(plan && plan->operationCount, "fixture apply requires a unit-transfer plan");
    CommitState expected = CommitState::Armed;
    if (!g_state.compare_exchange_strong(expected, CommitState::Claimed,
                                         std::memory_order_acq_rel)) {
        if (expected == CommitState::Committed) return;
        failFast("turn-zero fixture callback re-entered before commit");
    }
    require(testenv::supportedGameBuild(), "fixture requires the exact supported executable");
    require(testenv::on("D2TESTDRV_RELAY_BRIDGE")
                && testenv::on("D2TESTDRV_UI_REPORTER") && testenv::on("D2TESTDRV_WORLD"),
            "fixture requires relay, UI and world proof channels");
    require(serverLogic && serverLogic->coreData, "server logic/core data is missing");
    auto* objectMap = game::CMidServerLogicApi::get().getObjectMap(serverLogic);
    require(objectMap != nullptr, "authoritative scenario map is missing");
    std::array<LiveOperation, fixtureplan::MaxOperations> live{};
    std::size_t transferred = 0;
    for (std::size_t i = 0; i < plan->operationCount; ++i)
        live[i] = preflightOperation(objectMap, plan->operations[i]);
    std::array<LiveHealth, fixtureplan::MaxOperations> liveHealth{};
    for (std::size_t i = 0; i < plan->healthCount; ++i)
        liveHealth[i] = preflightHealth(objectMap, plan->health[i]);
    if (plan->healthCount) for (std::size_t i = 0; i < plan->operationCount; ++i) {
        bool survivor = false;
        const auto& group = plan->operations[i].destinationAfter;
        for (std::size_t u = 0; u < group.count; ++u) {
            const auto* unit = exactUnit(objectMap, group.units[u]);
            require(unit != nullptr, "health group survivor preflight lost a unit");
            survivor |= postHealthHp(plan, unit->id, unit->currentHp) > 0;
        }
        require(survivor, "health poststate would leave an entirely dead group");
    }
    for (std::size_t i = 0; i < plan->operationCount; ++i) {
        const auto& op = plan->operations[i];
        for (std::size_t j = 0; j < plan->operationCount; ++j) {
            if (i == j) continue;
            for (std::size_t u = 0; u < op.unitCount; ++u)
                require(countId(live[j].stack->group.units, op.units[u].id) == 0
                            && countId(live[j].fort->group.units, op.units[u].id) == 0,
                        "source unit leaked into another operation's group");
        }
    }
    // Every group and every visitor pair is checked before the first write.
    for (std::size_t i = 0; i < plan->operationCount; ++i)
        preflightVisitors(objectMap, plan->operations[i]);
    for (std::size_t i = 0; i < plan->operationCount; ++i) {
        transferOperation(objectMap, plan->operations[i], live[i]);
        transferred += plan->operations[i].unitCount;
    }
    for (std::size_t i = 0; i < plan->operationCount; ++i)
        verifyPostOperation(objectMap, plan->operations[i], live[i]);
    for (std::size_t i = 0; i < plan->healthCount; ++i) {
        const auto& health = plan->health[i];
        verifyHealth(objectMap, health, liveHealth[i], false);
        // CVisitorChangeUnitHP owns clamping and leaderAlive, not the fixture.
        require(game::VisitorApi::get().changeUnitHp(&health.unit,
                    health.newHp - health.expectedHp, objectMap, 1),
                "health visitor apply failed");
        verifyHealth(objectMap, health, liveHealth[i], true);
    }
    if (plan->healthCount) for (std::size_t i = 0; i < plan->operationCount; ++i)
        verifyPostOperation(objectMap, plan->operations[i], live[i], plan);
    // Publish the changed groups and explicitly changed units exactly once.
    for (std::size_t i = 0; i < plan->operationCount; ++i) {
        markChanged(objectMap, plan->operations[i].destinationStack);
        markChanged(objectMap, plan->operations[i].sourceFort);
    }
    for (std::size_t i = 0; i < plan->healthCount; ++i) {
        markChanged(objectMap, plan->health[i].unit);
        markChanged(objectMap, plan->health[i].stack);
    }
    auto* sender = static_cast<game::IMidMsgSender*>(serverLogic);
    require(sender->vftable->sendObjectsChanges(sender),
            "authoritative unit-transfer publication failed");
    for (std::size_t i = 0; i < plan->operationCount; ++i)
        verifyPostOperation(objectMap, plan->operations[i], live[i], plan);
    for (std::size_t i = 0; i < plan->healthCount; ++i)
        verifyHealth(objectMap, plan->health[i], liveHealth[i], true);
    g_state.store(CommitState::Committed, std::memory_order_release);
    spdlog::info("[testdrv][fixture-reinforcement] COMMITTED unit-transfer plan: operations={} units={}",
                 plan->operationCount, transferred);
    for (std::size_t i = 0; i < plan->healthCount; ++i) {
        const auto& health = plan->health[i];
        spdlog::info("[testdrv][fixture-health] COMMITTED stack=0x{:08X} unit=0x{:08X} "
                     "implementation=0x{:08X} hp={}->{} leaderAlive={}",
                     static_cast<std::uint32_t>(health.stack.value),
                     static_cast<std::uint32_t>(health.unit.value),
                     static_cast<std::uint32_t>(health.implementation.value),
                     health.expectedHp, health.newHp, liveHealth[i].stack->leaderAlive);
    }
    spdlog::default_logger()->flush();
}
} // namespace hooks::testdrv::fixture_reinforcement
#endif // D2_TESTDRV

