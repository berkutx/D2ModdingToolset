#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace game {
struct CMidgardID { int value{}; };
bool operator==(CMidgardID a, CMidgardID b) { return a.value == b.value; }
bool operator!=(CMidgardID a, CMidgardID b) { return !(a == b); }
constexpr CMidgardID emptyId{};
struct CMqPoint { int x{}, y{}; };
struct IMidgardObjectMap;
struct ObjectVftable {};
struct IMidScenarioObject { const ObjectVftable* vftable{}; CMidgardID id{}; };
struct MapElement { CMqPoint position{}; int sizeX{}, sizeY{}; };
struct CFortification : IMidScenarioObject {
    MapElement mapElement{}; CMidgardID ownerId{}, stackId{};
};
struct CFortificationVftable : ObjectVftable {
    int (*getTier)(const CFortification*, const IMidgardObjectMap*){};
};
struct CMidStack {
    CMidgardID id{}, ownerId{}, insideId{}, leaderId{};
    bool leaderAlive{}; CMqPoint position{};
};
struct CMidUnit { CMidgardID id{}; void* unitImpl{}; int currentHp{}; };
struct ObjectMapVftable {
    const IMidScenarioObject* (*findScenarioObjectById)(const IMidgardObjectMap*, const CMidgardID*){};
};
struct IMidgardObjectMap { const ObjectMapVftable* vftable{}; };
struct CMidgardMap { int mapSize{}; };
struct CMidgardPlan {};
struct Functions {
    const CMidUnit* (*findUnitById)(const IMidgardObjectMap*, const CMidgardID*){};
    const CMidgardPlan* (*getMidgardPlan)(const IMidgardObjectMap*){};
    bool (*stackCanMoveToPosition)(const IMidgardObjectMap*, const CMqPoint*, const CMidStack*, const CMidgardPlan*){};
};
const Functions& gameFunctions();
struct Rtti { const void* IMidScenarioObjectType{}; const void* CFortificationType{}; };
namespace RttiApi {
const Rtti& rtti();
struct Api { void* (*dynamicCast)(const void*, int, const void*, const void*, int){}; };
const Api& get();
}
}

namespace fixture {
game::CFortification fort;
game::CMidStack stack;
game::CMidUnit leader;
game::CMidgardMap map;
game::CMidgardPlan plan;
game::IMidgardObjectMap objectMap;
bool haveObject, castOk, haveLeader, haveMap, havePlan, passable, fixtureRule;
int tier, passabilityCalls, fixtureX, fixtureY;
game::CMidgardID localOwner;
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
int getTier(const game::CFortification*, const game::IMidgardObjectMap*) { return tier; }
game::CFortificationVftable fortVftable;
const game::IMidScenarioObject* findObject(const game::IMidgardObjectMap*, const game::CMidgardID*) {
    return haveObject ? &fort : nullptr;
}
game::ObjectMapVftable objectMapVftable{&findObject};
const game::CMidUnit* findLeader(const game::IMidgardObjectMap*, const game::CMidgardID*) {
    return haveLeader ? &leader : nullptr;
}
const game::CMidgardPlan* getPlan(const game::IMidgardObjectMap*) { return havePlan ? &plan : nullptr; }
bool canMove(const game::IMidgardObjectMap* objects, const game::CMqPoint* point,
             const game::CMidStack* actualStack, const game::CMidgardPlan* actualPlan) {
    ++passabilityCalls;
    require(objects == &objectMap && actualStack == &stack && actualPlan == &plan,
            "native admission did not receive the actual inside stack/map/plan");
    require(point->x == fort.mapElement.position.x + 5 && point->y == fort.mapElement.position.y + 5,
            "native admission received the wrong outer gate");
    return passable;
}
void* cast(const void* object, int, const void*, const void*, int) {
    require(object == static_cast<const game::IMidScenarioObject*>(&fort), "wrong RTTI source object");
    return castOk ? &fort : nullptr;
}
void reset() {
    haveObject = castOk = haveLeader = haveMap = havePlan = passable = true;
    fixtureRule = false; fixtureX = 15; fixtureY = 25; passabilityCalls = 0; tier = 6;
    localOwner = {1};
    fortVftable.getTier = &getTier;
    fort = {}; fort.vftable = &fortVftable; fort.id = {10};
    fort.ownerId = {1}; fort.stackId = {20}; fort.mapElement = {{10, 20}, 5, 5};
    stack = {{20}, {1}, {10}, {30}, true, {10, 20}};
    leader = {{30}, &stack, 50}; map = {64}; objectMap.vftable = &objectMapVftable;
}
}
namespace game {
const Functions& gameFunctions() {
    static Functions value{&fixture::findLeader, &fixture::getPlan, &fixture::canMove}; return value;
}
namespace RttiApi {
const Rtti& rtti() { static Rtti value{}; return value; }
const Api& get() { static Api value{&fixture::cast}; return value; }
}
}
namespace hooks {
const game::CMidgardMap* getMidgardMap(const game::IMidgardObjectMap*) {
    return fixture::haveMap ? &fixture::map : nullptr;
}
game::CMqPoint getObjectEntrance(const game::CMqPoint& position, int sizeX, int sizeY) {
    return {position.x + sizeX - 1, position.y + sizeY - 1};
}
namespace testdrv { namespace worldactions {
game::CMidgardID localPlayerId() { return fixture::localOwner; }
namespace fixtureplan {
bool garrisonExit(std::uint32_t, int, int, int& x, int& y) {
    x = fixture::fixtureX; y = fixture::fixtureY; return fixture::fixtureRule;
}
}
#include "testdrv_capital_exit_production.inc"
}}}

int main() {
    using namespace fixture;
    using namespace hooks::testdrv::worldactions;
    int checks = 0;
    auto check = [&](const std::function<void()>& change, bool expected) {
        reset(); change();
        game::CMqPoint inner{-99, -98}, outer{-97, -96}; game::CMidgardID fortId{-95};
        const bool actual = querySupportedCapitalExit(&objectMap, &stack, localOwner, inner, outer, fortId);
        require(actual == expected, "capital geometry admission mismatch");
        if (expected) {
            require(inner.x == 14 && inner.y == 24 && outer.x == 15 && outer.y == 25 && fortId.value == 10,
                    "wrong admitted geometry");
            require(passabilityCalls == 1, "native admission not checked exactly once");
        } else {
            require(inner.x == -99 && inner.y == -98 && outer.x == -97 && outer.y == -96 && fortId.value == -95,
                    "failed admission modified outputs");
        }
        ++checks;
    };
    check([]{}, true);
    check([]{ localOwner = {}; }, false);
    check([]{ objectMap.vftable = nullptr; }, false);
    check([]{ stack.ownerId = {2}; }, false);
    check([]{ stack.insideId = {}; }, false);
    check([]{ stack.leaderAlive = false; }, false);
    check([]{ stack.leaderId = {}; }, false);
    check([]{ haveLeader = false; }, false);
    check([]{ leader.id = {31}; }, false);
    check([]{ leader.unitImpl = nullptr; }, false);
    check([]{ leader.currentHp = 0; }, false);
    check([]{ haveObject = false; }, false);
    check([]{ castOk = false; }, false);
    check([]{ fort.id = {11}; }, false);
    check([]{ fort.ownerId = {2}; }, false);
    check([]{ fort.stackId = {21}; }, false);
    check([]{ fort.vftable = nullptr; }, false);
    check([]{ fortVftable.getTier = nullptr; }, false);
    check([]{ tier = 5; }, false);
    check([]{ fort.mapElement.sizeX = 4; }, false);
    check([]{ fort.mapElement.sizeY = 6; }, false);
    check([]{ ++stack.position.x; }, false);
    check([]{ ++stack.position.y; }, false);
    check([]{ haveMap = false; }, false);
    check([]{ havePlan = false; }, false);
    check([]{ map.mapSize = 5; }, false);
    check([]{ stack.position.x = fort.mapElement.position.x = -1; }, false);
    check([]{ stack.position.y = fort.mapElement.position.y = -1; }, false);
    check([]{ stack.position.x = fort.mapElement.position.x = 59; }, false);
    check([]{ stack.position.y = fort.mapElement.position.y = 0x7fffffff; }, false);
    check([]{ passable = false; }, false);
    reset();
    game::CMqPoint inner{}, outer{};
    require(resolveGarrisonExit(&objectMap, &stack, 15, 25, inner, outer), "exact gate rejected"); ++checks;
    require(!resolveGarrisonExit(&objectMap, &stack, 16, 25, inner, outer), "caller gate drift accepted"); ++checks;
    fixtureRule = true;
    require(resolveGarrisonExit(&objectMap, &stack, 15, 25, inner, outer), "matching fixture rejected"); ++checks;
    fixtureX = 16;
    require(!resolveGarrisonExit(&objectMap, &stack, 16, 25, inner, outer), "fixture overrode actual geometry"); ++checks;
    std::cout << "capital-exit actual-function checks PASS: " << checks << '\n';
}
