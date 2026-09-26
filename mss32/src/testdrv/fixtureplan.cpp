/*
 * Windows INI fixture data, cached before any test operation can mutate the game.
 */
#ifdef D2_TESTDRV
#include "testdrv/fixtureplan.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks::testdrv::fixtureplan {
namespace {

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

struct Reader {
    std::string path;

    std::string text(const std::string& section, const std::string& key,
                     const char* defaultValue = "\1") const
    {
        char buffer[1024]{};
        const DWORD size = GetPrivateProfileStringA(section.c_str(), key.c_str(),
                                                    defaultValue, buffer, sizeof(buffer), path.c_str());
        require(size < sizeof(buffer) - 1, "fixture value is too long");
        require(!(size == 1 && buffer[0] == '\1'), "required fixture key is missing");
        return std::string(buffer, size);
    }

    std::size_t numbers(const std::string& section, const std::string& key,
                        std::uint32_t* values, std::size_t capacity,
                        const char* defaultValue = "\1") const
    {
        const auto value = text(section, key, defaultValue);
        const char* cursor = value.c_str();
        std::size_t count = 0;
        while (*cursor) {
            while (std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
            require(*cursor >= '0' && *cursor <= '9', "fixture requires unsigned numbers");
            errno = 0;
            char* end{};
            const auto number = std::strtoull(cursor, &end, 0);
            require(end != cursor && errno != ERANGE
                        && number <= std::numeric_limits<std::uint32_t>::max(),
                    "fixture number is outside uint32");
            require(count < capacity, "too many values in fixture key");
            values[count++] = static_cast<std::uint32_t>(number);
            while (std::isspace(static_cast<unsigned char>(*end))) ++end;
            if (!*end) break;
            require(*end == ',' && end[1], "fixture list requires comma-separated numbers");
            cursor = end + 1;
        }
        return count;
    }

    std::uint32_t number(const std::string& section, const std::string& key,
                         std::uint32_t minimum, std::uint32_t maximum,
                         const char* defaultValue = "\1") const
    {
        std::uint32_t value{};
        require(numbers(section, key, &value, 1, defaultValue) == 1
                    && value >= minimum && value <= maximum, "fixture number outside bounds");
        return value;
    }

    game::CMidgardID id(const std::string& section, const std::string& key) const
    {
        return game::CMidgardID{static_cast<int>(number(section, key, 1, UINT32_MAX))};
    }

    int coordinate(const std::string& section, const std::string& key) const
    {
        return static_cast<int>(number(section, key, 0, 4095));
    }

    Group group(const std::string& section, const std::string& prefix) const
    {
        Group result;
        std::uint32_t values[6]{};
        result.count = numbers(section, prefix + "Units", values, 6);
        for (std::size_t i = 0; i < result.count; ++i) {
            require(values[i] != 0, "unit list contains an empty id");
            result.units[i].value = static_cast<int>(values[i]);
            for (std::size_t j = 0; j < i; ++j)
                require(result.units[i] != result.units[j], "unit list contains a duplicate");
        }
        require(numbers(section, prefix + "Cells", values, 6) == 6,
                "group must describe all six cells");
        for (std::size_t i = 0; i < 6; ++i) {
            result.cells[i].value = static_cast<int>(values[i]);
            require(!values[i] || std::find(result.units.begin(),
                        result.units.begin() + result.count, result.cells[i])
                        != result.units.begin() + result.count,
                    "group cell refers to an unlisted unit");
        }
        for (std::size_t i = 0; i < result.count; ++i)
            require(std::find(result.cells.begin(), result.cells.end(), result.units[i])
                        != result.cells.end(), "listed unit has no group cell");
        return result;
    }
};

bool contains(const Group& group, game::CMidgardID id)
{
    return std::find(group.units.begin(), group.units.begin() + group.count, id)
           != group.units.begin() + group.count;
}

void validateOperation(const Operation& op)
{
    require(op.sourceFort != op.destinationStack && op.destinationBefore.count > 0
                && op.destinationBefore.units[0] == op.leader,
            "invalid source/destination or leader prestate");
    require(op.sourceBefore.count >= op.unitCount
                && op.sourceAfter.count == op.sourceBefore.count - op.unitCount
                && op.destinationAfter.count == op.destinationBefore.count + op.unitCount,
            "unit-transfer group sizes are inconsistent");
    auto sourceCells = op.sourceBefore.cells;
    auto destinationCells = op.destinationBefore.cells;
    for (std::size_t i = 0; i < op.unitCount; ++i) {
        const auto& unit = op.units[i];
        require(unit.id != op.leader && contains(op.sourceBefore, unit.id)
                    && !contains(op.sourceAfter, unit.id)
                    && !contains(op.destinationBefore, unit.id)
                    && op.destinationAfter.units[op.destinationBefore.count + i] == unit.id,
                "transfer does not match expected group membership/order");
        require(std::count(sourceCells.begin(), sourceCells.end(), unit.id) == 1,
                "transferred unit must occupy exactly one source cell");
        require(destinationCells[unit.destinationSlot].value == 0,
                "destination slot is occupied or repeated");
        *std::find(sourceCells.begin(), sourceCells.end(), unit.id) = game::CMidgardID{0};
        destinationCells[unit.destinationSlot] = unit.id;
    }
    require(sourceCells == op.sourceAfter.cells && destinationCells == op.destinationAfter.cells,
            "transfer cell postconditions are inconsistent");
    for (std::size_t i = 0; i < op.sourceAfter.count; ++i)
        require(contains(op.sourceBefore, op.sourceAfter.units[i]),
                "source postcondition adds a unit");
    for (std::size_t i = 0; i < op.destinationBefore.count; ++i)
        require(op.destinationBefore.units[i] == op.destinationAfter.units[i],
                "destination postcondition changes its existing units");
}

struct CachedPlan {
    Plan plan;
    bool configured{};
    bool valid{true};
};

CachedPlan load()
{
    CachedPlan result;
    char path[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("D2TESTDRV_FIXTURE_PLAN", path, sizeof(path));
    if (!length)
        return result;
    result.configured = true;
    try {
        require(length < sizeof(path), "fixture path exceeds MAX_PATH");
        const std::string value(path, length);
        require((value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0]))
                    && value[1] == ':' && (value[2] == '\\' || value[2] == '/'))
                    || (value.size() > 2 && value[0] == '\\' && value[1] == '\\'),
                "fixture path must be absolute");
        const DWORD attributes = GetFileAttributesA(path);
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY),
                "fixture file is missing or inaccessible");
        const Reader reader{value};
        auto& plan = result.plan;
        require(reader.number("plan", "version", 1, 1) == 1, "unsupported fixture version");
        plan.operationCount = reader.number("plan", "operationCount", 0, MaxOperations);
        plan.healthCount = reader.number("plan", "healthCount", 0, MaxOperations, "0");
        plan.exitCount = reader.number("plan", "exitCount", 0, MaxMovementRules);
        plan.routeCount = reader.number("plan", "routeCount", 0, MaxMovementRules);
        plan.excludedTileCount = reader.number("plan", "excludedTileCount", 0, MaxMovementRules);
        for (std::size_t i = 0; i < plan.operationCount; ++i) {
            const auto section = "operation" + std::to_string(i + 1);
            auto& op = plan.operations[i];
            op.owner = reader.id(section, "owner");
            op.sourceFort = reader.id(section, "sourceFort");
            op.destinationStack = reader.id(section, "destinationStack");
            op.leader = reader.id(section, "leader");
            op.leaderHp = static_cast<int>(reader.number(section, "leaderHp", 1, INT_MAX));
            op.anchorX = reader.coordinate(section, "anchorX");
            op.anchorY = reader.coordinate(section, "anchorY");
            op.unitCount = reader.number(section, "unitCount", 1, 6);
            op.sourceBefore = reader.group(section, "sourceBefore");
            op.sourceAfter = reader.group(section, "sourceAfter");
            op.destinationBefore = reader.group(section, "destinationBefore");
            op.destinationAfter = reader.group(section, "destinationAfter");
            for (std::size_t j = 0; j < op.unitCount; ++j) {
                const auto prefix = "unit" + std::to_string(j + 1);
                auto& unit = op.units[j];
                unit.id = reader.id(section, prefix + "Id");
                unit.implementation = reader.id(section, prefix + "Implementation");
                unit.hp = static_cast<int>(reader.number(section, prefix + "Hp", 1, INT_MAX));
                unit.destinationSlot = static_cast<int>(reader.number(section, prefix + "Slot", 0, 5));
            }
            validateOperation(op);
            for (std::size_t j = 0; j < i; ++j) {
                const auto& other = plan.operations[j];
                require(op.sourceFort != other.sourceFort
                            && op.sourceFort != other.destinationStack
                            && op.destinationStack != other.sourceFort
                            && op.destinationStack != other.destinationStack
                            && op.leader != other.leader,
                        "fixture operations overlap groups or leaders");
                for (std::size_t u = 0; u < op.unitCount; ++u)
                    for (std::size_t v = 0; v < other.unitCount; ++v)
                        require(op.units[u].id != other.units[v].id,
                                "fixture operations repeat a transferred unit");
            }
        }
        for (std::size_t i = 0; i < plan.healthCount; ++i) {
            const auto section = "health" + std::to_string(i + 1);
            auto& health = plan.health[i];
            health.stack = reader.id(section, "stack");
            health.unit = reader.id(section, "unit");
            health.implementation = reader.id(section, "implementation");
            health.expectedHp = static_cast<int>(reader.number(section, "expectedHp", 1, INT_MAX));
            health.newHp = static_cast<int>(reader.number(section, "newHp", 0, INT_MAX));
            require(health.expectedHp != health.newHp, "fixture health change is a no-op");
            const auto end = plan.operations.begin() + plan.operationCount;
            const auto operation = std::find_if(plan.operations.begin(), end,
                [&](const Operation& op) { return op.destinationStack == health.stack; });
            require(operation != end && contains(operation->destinationAfter, health.unit),
                    "fixture health unit is outside its destination group poststate");
            for (std::size_t j = 0; j < i; ++j)
                require(health.unit != plan.health[j].unit, "fixture repeats a health unit");
        }
        for (std::size_t i = 0; i < plan.exitCount; ++i) {
            const auto section = "exit" + std::to_string(i + 1);
            auto& rule = plan.exits[i];
            rule.stack = reader.number(section, "stack", 1, UINT32_MAX);
            rule.anchorX = reader.coordinate(section, "anchorX");
            rule.anchorY = reader.coordinate(section, "anchorY");
            rule.x = reader.coordinate(section, "x");
            rule.y = reader.coordinate(section, "y");
            for (std::size_t j = 0; j < i; ++j)
                require(rule.stack != plan.exits[j].stack
                            || rule.anchorX != plan.exits[j].anchorX
                            || rule.anchorY != plan.exits[j].anchorY,
                        "fixture repeats a garrison exit key");
        }
        for (std::size_t i = 0; i < plan.routeCount; ++i) {
            const auto section = "route" + std::to_string(i + 1);
            auto& rule = plan.routes[i];
            rule.stack = reader.number(section, "stack", 1, UINT32_MAX);
            rule.fromX = reader.coordinate(section, "fromX");
            rule.fromY = reader.coordinate(section, "fromY");
            rule.toX = reader.coordinate(section, "toX");
            rule.toY = reader.coordinate(section, "toY");
        }
        for (std::size_t i = 0; i < plan.excludedTileCount; ++i) {
            const auto section = "excludedTile" + std::to_string(i + 1);
            auto& rule = plan.excludedTiles[i];
            rule.left = reader.coordinate(section, "left");
            rule.top = reader.coordinate(section, "top");
            rule.right = reader.coordinate(section, "right");
            rule.bottom = reader.coordinate(section, "bottom");
            require(rule.left <= rule.right && rule.top <= rule.bottom,
                    "fixture rectangle has inverted bounds");
        }
    } catch (const std::exception& error) {
        spdlog::critical("[testdrv][fixture-plan] INVALID: {}", error.what());
        result.valid = false;
    }
    return result;
}

const CachedPlan& cached()
{
    static const CachedPlan value = load();
    return value;
}
} // namespace

bool preflight() { return cached().valid; }
bool hasPlan() { return cached().configured && cached().valid; }
const Plan* get() { return hasPlan() ? &cached().plan : nullptr; }

bool garrisonExit(std::uint32_t stackId, int anchorX, int anchorY, int& x, int& y)
{
    const auto* plan = get();
    if (plan) for (std::size_t i = 0; i < plan->exitCount; ++i) {
        const auto& rule = plan->exits[i];
        if (rule.stack == stackId && rule.anchorX == anchorX && rule.anchorY == anchorY) {
            x = rule.x; y = rule.y;
            return true;
        }
    }
    return false;
}

bool allowsExactRoute(std::uint32_t stackId, int fromX, int fromY, int toX, int toY)
{
    const auto* plan = get();
    if (plan) for (std::size_t i = 0; i < plan->routeCount; ++i) {
        const auto& rule = plan->routes[i];
        if (rule.stack == stackId && rule.fromX == fromX && rule.fromY == fromY
                && rule.toX == toX && rule.toY == toY)
            return true;
    }
    return false;
}

bool isExcludedTile(int x, int y)
{
    const auto* plan = get();
    if (plan) for (std::size_t i = 0; i < plan->excludedTileCount; ++i) {
        const auto& rule = plan->excludedTiles[i];
        if (x >= rule.left && x <= rule.right && y >= rule.top && y <= rule.bottom)
            return true;
    }
    return false;
}
} // namespace hooks::testdrv::fixtureplan
#endif

