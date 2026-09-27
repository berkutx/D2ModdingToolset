/*
 * Immutable, opt-in data for generic test-fixture operations.
 */
#ifndef TESTDRV_FIXTUREPLAN_H
#define TESTDRV_FIXTUREPLAN_H
#ifdef D2_TESTDRV
#include "midgardid.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace hooks::testdrv::fixtureplan {

constexpr std::size_t MaxOperations = 6;
constexpr std::size_t MaxMovementRules = 32;
struct Group {
    std::array<game::CMidgardID, 6> units{};
    std::size_t count{};
    std::array<game::CMidgardID, 6> cells{};
};
struct UnitTransfer {
    game::CMidgardID id{};
    game::CMidgardID implementation{};
    int hp{};
    int destinationSlot{};
};
struct Operation {
    game::CMidgardID owner{}, sourceFort{}, destinationStack{}, leader{};
    int leaderHp{}, anchorX{}, anchorY{};
    std::array<UnitTransfer, 6> units{};
    std::size_t unitCount{};
    Group sourceBefore, sourceAfter, destinationBefore, destinationAfter;
};
struct HealthPostcondition {
    game::CMidgardID stack{}, unit{}, implementation{};
    int expectedHp{}, newHp{};
};
struct Exit {
    std::uint32_t stack{};
    int anchorX{}, anchorY{}, x{}, y{};
};
struct Route {
    std::uint32_t stack{};
    int fromX{}, fromY{}, toX{}, toY{};
};
struct Rect {
    int left{}, top{}, right{}, bottom{};
};
struct Plan {
    std::array<Operation, MaxOperations> operations{};
    std::size_t operationCount{};
    // Optional HP changes after transfers; absent healthCount preserves the old plan.
    std::array<HealthPostcondition, MaxOperations> health{};
    std::size_t healthCount{};
    std::array<Exit, MaxMovementRules> exits{};
    std::size_t exitCount{};
    std::array<Route, MaxMovementRules> routes{};
    std::size_t routeCount{};
    std::array<Rect, MaxMovementRules> excludedTiles{};
    std::size_t excludedTileCount{};
};

// Reads D2TESTDRV_FIXTURE_PLAN once. An absent variable means disabled; an
// invalid nonempty file returns false and must fail the harness install.
bool preflight();
bool hasPlan();
const Plan* get();
bool garrisonExit(std::uint32_t stackId, int anchorX, int anchorY, int& x, int& y);
bool allowsExactRoute(std::uint32_t stackId, int fromX, int fromY, int toX, int toY);
bool isExcludedTile(int x, int y);

} // namespace hooks::testdrv::fixtureplan
#endif
#endif

