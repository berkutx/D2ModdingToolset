/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * World-state reporter. See testdrv/worldreporter.h.
 *
 * Compile-gated by D2_TESTDRV: absent from ordinary Debug and Release builds.
 *
 * Reuses the mod's own state-surfacing layer (bindings::ScenarioView + the *View
 * wrappers), so the reporter mirrors how the mod already reads players and stacks.
 * Live unit combat getters can execute Lua modifiers; their expensive slot profiles
 * are opt-in telemetry (D2TESTDRV_WORLD_PROFILES), not part of basic world observation.
 * Mod style for these reads is null-checks, not SEH; crash-safety comes from the thin __try
 * wrapper in autonav (these calls allocate, so __try cannot live here: MSVC C2712).
 */

#ifdef D2_TESTDRV

#include "testdrv/worldreporter.h"
#include "testdrv/json.h"
#include "testdrv/testdrv.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "phasegame.h"
#include "bindings/currencyview.h"
#include "bindings/fortview.h"
#include "bindings/groupview.h"
#include "bindings/idview.h"
#include "bindings/mercsview.h" // camps: forEachMercenary -> MercsView (+ SiteView, UnitImplView)
#include "bindings/playerview.h"
#include "bindings/point.h"
#include "bindings/scenarioview.h"
#include "bindings/stackview.h"
#include "bindings/attackview.h" // unit combat profile: getReach()/getAttackClass()/isMelee()/maxTargets()
#include "bindings/unitimplview.h" // UnitImplView: getXpKilled()/getHitPoint()/getArmor()/getAttack()/...
#include "bindings/unitslotview.h" // getGroup().getSlots() yields std::vector<UnitSlotView> (slot occupancy)
#include "bindings/unitview.h" // getGroup().getUnits() yields std::vector<UnitView>; .size() needs it complete
#include "gameutils.h"
#include "midbag.h" // chests: raw CMidBag via forEachScenarioObject(IdType::Bag)
#include "midclient.h"
#include "midgard.h"
#include "midgardid.h"
#include "utils.h"
#include "version.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace worldreporter {

namespace {

// Built on the UI thread (rebuildSnapshot), read by the bridge thread (copyWorldSnapshot), both
// under g_snapMutex.
std::mutex g_snapMutex;
std::string g_snapJson;
std::uint32_t g_snapEpoch = 0;
bool g_enabled = false;
bool g_includeUnitProfiles = false;
bool g_mapSeen = false;    // latched only for one currently validated live CPhaseGame
game::CPhaseGame* g_mapPhase = nullptr; // identity token only; never dereferenced after validation
DWORD g_lastBuildTick = 0; // GetTickCount when the last rebuild completed
constexpr DWORD kThrottleMs = 500;

using json::appendEscaped;
using json::kvInt;

void kvStr(std::string& out, const char* key, const char* val)
{
    out += '"';
    out += key;
    out += "\":";
    appendEscaped(out, val);
}

void kvBool(std::string& out, const char* key, bool v)
{
    out += '"';
    out += key;
    out += "\":";
    out += v ? "true" : "false";
}

// Emit a unit's combat profile (no leading/trailing comma; the caller frames it). Drives the formation
// + camp-selection logic: the PRIMARY attack's `reach` classifies the line (AttackReachId Adjacent=3 =
// hits only the nearest enemy = a FRONT defender; Any=2 / All=1 = can hit any target = a BACK unit),
// `atkClass` flags casters/support (AttackClassId Heal=6 / Cure=14 = healer, Paralyze=3, etc.), and
// `xp` (xpKilled) is the value proxy. `small` = single-slot (a big unit takes a whole column).
void emitUnitProfile(std::string& out, const bindings::UnitImplView& impl)
{
    kvInt(out, "level", impl.getLevel());
    out += ',';
    kvInt(out, "xp", impl.getXpKilled());
    out += ',';
    kvInt(out, "hp", impl.getHitPoint());
    out += ',';
    kvInt(out, "armor", impl.getArmor());
    out += ',';
    kvInt(out, "dmg", impl.getDamageMax());
    out += ',';
    kvBool(out, "small", impl.isSmall());
    if (auto atk = impl.getAttack()) {
        out += ',';
        kvInt(out, "reach", atk->getReach());
        out += ',';
        kvInt(out, "atkClass", atk->getAttackClass());
        out += ',';
        kvBool(out, "melee", atk->isMelee());
        out += ',';
        kvInt(out, "maxTargets", atk->maxTargets());
    }
}

// One transport-stable numeric spelling is shared by snapshots and strict command identities.
std::string wireId(const game::CMidgardID& id)
{
    char value[11]{};
    wsprintfA(value, "0x%08X", static_cast<unsigned>(id.value));
    return value;
}

std::string wireId(const game::CMidgardID* id)
{
    return wireId(*id);
}

const char* relationOf(const game::CMidgardID& owner, const game::CMidgardID& local,
                       const game::CMidgardID& neutral)
{
    if (owner == local)
        return "self";
    if (owner == neutral)
        return "neutral";
    return "enemy";
}

// UI-thread only: reads live game objects through ScenarioView and the *View wrappers.
void buildJson(std::string& json, const game::IMidgardObjectMap* objectMap)
{
    using namespace bindings;

    // The local client's player + the neutral player, to tag each player/stack relation.
    game::CMidgardID localId = game::emptyId;
    auto* midgard = game::CMidgardApi::get().instance();
    if (midgard && midgard->data && midgard->data->netPlayerClientPtr)
        localId = midgard->data->netPlayerClientPtr->second;
    // The active player is a distinct proof field in network games: localId
    // identifies this process, while currentPlayerId proves who owns the stock
    // turn after merge. Resolve it from a freshly back-pointer-validated phase;
    // there is deliberately no process-lifetime phase pointer fallback.
    game::CMidgardID activePlayerId = game::emptyId;
    auto* phaseGame = testdrv::livePhaseGame();
    if (phaseGame && phaseGame->data)
        activePlayerId = phaseGame->data->currentPlayerId;
    // Single-instance games (skirmish/hotseat) have no network client, so use
    // that same live active identity as the local relation anchor.
    if (localId == game::emptyId) {
        localId = activePlayerId;
    }

    game::CMidgardID neutralId = game::emptyId;
    if (auto* neutral = hooks::getNeutralPlayer(objectMap))
        neutralId = PlayerView{neutral, objectMap}.getId().id;

    ScenarioView scenario{objectMap};

    json += '{';
    kvInt(json, "day", scenario.getCurrentDay());
    json += ',';
    kvBool(json, "strategicActionReady", testdrv::strategicActionReady());
    json += ',';
    kvStr(json, "activePlayerId", wireId(activePlayerId).c_str());

    json += ",\"players\":[";
    bool firstPlayer = true;
    scenario.forEachPlayer([&](const PlayerView& p) {
        const auto id = p.getId();
        const auto bank = p.getBank();
        if (!firstPlayer)
            json += ',';
        firstPlayer = false;
        json += '{';
        kvStr(json, "id", wireId(&id.id).c_str());
        json += ',';
        kvStr(json, "relation", relationOf(id.id, localId, neutralId));
        json += ',';
        kvBool(json, "human", p.isHuman());
        json += ',';
        kvInt(json, "race", p.getRaceCategoryId());
        json += ',';
        kvInt(json, "gold", bank.getGold());
        json += ',';
        kvInt(json, "lifeMana", bank.getLifeMana());
        json += ',';
        kvInt(json, "deathMana", bank.getDeathMana());
        json += ',';
        kvInt(json, "infernalMana", bank.getInfernalMana());
        json += ',';
        kvInt(json, "runicMana", bank.getRunicMana());
        json += ',';
        kvInt(json, "groveMana", bank.getGroveMana());
        json += '}';
    });
    json += ']';

    json += ",\"stacks\":[";
    bool firstStack = true;
    scenario.forEachStack([&](const StackView& s) {
        const auto id = s.getId();
        const auto pos = s.getPosition();
        game::CMidgardID ownerId = game::emptyId;
        if (auto owner = s.getOwner())
            ownerId = owner->getId().id;
        const auto unitViews = s.getGroup().getUnits();
        const int units = (int)unitViews.size();
        int hp = 0; // total current HP of the group; a battle drops it (damage) even with no unit killed
        for (const auto& u : unitViews)
            hp += u.getHp();
        if (!firstStack)
            json += ',';
        firstStack = false;
        json += '{';
        kvStr(json, "id", wireId(&id.id).c_str());
        json += ',';
        kvInt(json, "x", pos.x);
        json += ',';
        kvInt(json, "y", pos.y);
        json += ',';
        kvStr(json, "owner", wireId(&ownerId).c_str());
        json += ',';
        kvStr(json, "relation", relationOf(ownerId, localId, neutralId));
        json += ',';
        kvInt(json, "movement", s.getMovement());
        json += ',';
        kvInt(json, "units", units);
        json += ',';
        json += "\"unitIds\":[";
        bool firstUnit = true;
        for (const auto& unit : unitViews) {
            if (!firstUnit)
                json += ',';
            firstUnit = false;
            json += '\"';
            json += wireId(unit.getId().id);
            json += '\"';
        }
        json += "],\"unitStates\":[";
        firstUnit = true;
        for (const auto& unit : unitViews) {
            if (!firstUnit)
                json += ',';
            firstUnit = false;
            json += '{';
            kvStr(json, "id", wireId(unit.getId().id).c_str());
            json += ',';
            kvInt(json, "hp", unit.getHp());
            json += '}';
        }
        json += "],";
        kvInt(json, "hp", hp);
        json += ',';
        kvInt(json, "subrace", s.getSubrace());
        json += ',';
        // A stack INSIDE a fort/city/village (getInside() set) is a garrison: its reported position is
        // the fort CENTRE (offset, like the player's own capital), and it cannot be attacked as a free
        // monster (that is a siege). The move/attack test must skip these and target free stacks only.
        kvBool(json, "inside", s.getInside().has_value());
        json += ',';
        // Group formation slots (0..5; front line = position%2==0, column = position/2). A big unit
        // occupies a whole column pair (isBig). leaderId = the group's leader (NEVER dismiss it; that
        // disbands the stack). Drives the slot/hire/formation management commands.
        game::CMidgardID leaderId = game::emptyId;
        if (auto leader = s.getLeader())
            leaderId = leader->getId().id;
        kvStr(json, "leaderId", wireId(&leaderId).c_str());
        json += ",\"slots\":[";
        bool firstSlot = true;
        for (const auto& slot : s.getGroup().getSlots()) {
            const auto uid = slot.getUnitId();
            if (uid == game::emptyId)
                continue;
            std::optional<bindings::UnitImplView> impl;
            if (auto uv = slot.getUnitView())
                impl = uv->getImpl();
            const bool big = impl && !impl->isSmall();
            if (!firstSlot)
                json += ',';
            firstSlot = false;
            json += '{';
            kvInt(json, "position", slot.getPosition());
            json += ',';
            kvStr(json, "unitId", wireId(&uid).c_str());
            json += ',';
            kvInt(json, "line", slot.getLine());
            json += ',';
            kvBool(json, "isBig", big);
            if (impl && g_includeUnitProfiles) {
                json += ',';
                emitUnitProfile(json, *impl); // reach/class/melee etc. for placement decisions
            }
            json += '}';
        }
        json += ']';
        json += '}';
    });
    json += ']';

    // Neutral mercenary camps: each is a CMidSiteMercs site exposing a fixed roster the visitor can
    // hire. The test enters the first camp and hires the one hero in it, so report id/pos + the roster
    // (impl id, level, unique). forEachMercenary already filters Site objects down to the mercs category.
    json += ",\"camps\":[";
    bool firstCamp = true;
    scenario.forEachMercenary([&](const MercsView& m) {
        const auto id = m.getId();
        const auto pos = m.getPosition();
        const auto roster = m.getUnits();
        if (!firstCamp)
            json += ',';
        firstCamp = false;
        json += '{';
        kvStr(json, "id", wireId(&id.id).c_str());
        json += ',';
        kvInt(json, "x", pos.x);
        json += ',';
        kvInt(json, "y", pos.y);
        json += ',';
        json += "\"units\":[";
        bool firstUnit = true;
        for (const auto& u : roster) {
            const auto impl = u.getImpl();
            const auto implId = impl.getId();
            if (!firstUnit)
                json += ',';
            firstUnit = false;
            json += '{';
            kvStr(json, "impl", wireId(&implId.id).c_str());
            json += ',';
            emitUnitProfile(json, impl); // level/xp/hp/armor/dmg/small + primary attack reach/class/melee
            json += ',';
            kvBool(json, "unique", u.isUnique());
            json += '}';
        }
        json += ']';
        json += '}';
    });
    json += ']';

    // Treasure chests / bags lying on the map. No *View wrapper exists for these, so read the raw
    // CMidBag straight from the object map (id at offset 4 via IMidObjectT, position, inventory items).
    // The test walks the 100-move hero onto each bag to collect it, so report id/pos + the item ids.
    json += ",\"bags\":[";
    bool firstBag = true;
    hooks::forEachScenarioObject(
        objectMap, game::IdType::Bag, [&](const game::IMidScenarioObject* obj) {
            const auto* bag = static_cast<const game::CMidBag*>(obj);
            const auto& pos = bag->mapElement.position;
            if (!firstBag)
                json += ',';
            firstBag = false;
            json += '{';
            kvStr(json, "id", wireId(&bag->id).c_str());
            json += ',';
            kvInt(json, "x", pos.x);
            json += ',';
            kvInt(json, "y", pos.y);
            json += ',';
            json += "\"items\":[";
            bool firstItem = true;
            for (const game::CMidgardID* it = bag->inventory.items.bgn;
                 it != bag->inventory.items.end; ++it) {
                if (!firstItem)
                    json += ',';
                firstItem = false;
                appendEscaped(json, wireId(it).c_str());
            }
            json += ']';
            json += '}';
        });
    json += ']';
    json += '}';
}

} // namespace

void rebuildSnapshot()
{
    if (!g_enabled)
        return;

    // Every rebuild first proves the currently live phase. The pointer retained below is only an
    // identity/generation token: it is compared, never dereferenced. A phase loss/change revokes the
    // prior map-ready latch before any object-map read.
    auto* phaseGame = testdrv::livePhaseGame();
    if (!phaseGame) {
        g_mapSeen = false;
        g_mapPhase = nullptr;
        return;
    }
    if (g_mapPhase != phaseGame) {
        g_mapSeen = false;
        g_mapPhase = phaseGame;
    }

    // Only report once the strategic map has come up. On a single-instance game (skirmish/hotseat)
    // the client object map is built IN PLACE during the initial scenario load, and reading it from
    // the tick mid-build crashed the load in a way the outer SEH guard cannot catch. The UI reporter
    // already tracks the live dialog; latch on the map so we never touch the object map during that
    // initial-load window. (In MP the client map is populated atomically per net message, so this
    // only changes single-instance behaviour and the pre-map load, which no test reads.)
    const char* dlg = uistatereporter::currentDialogName();
    if (dlg && (std::strcmp(dlg, "DLG_ISO_PAL") == 0 || std::strcmp(dlg, "DLG_STRATEGIC") == 0))
        g_mapSeen = true;
    if (!g_mapSeen)
        return;

    // Leave 500ms after each completed walk for ordinary game frames. Starting this interval before
    // expensive opt-in profile getters would make an over-budget walk run again on the next frame.
    const DWORD now = GetTickCount();
    if (g_lastBuildTick != 0 && (now - g_lastBuildTick) < kThrottleMs)
        return;

    const game::IMidgardObjectMap* objectMap =
        game::CMidClientCoreApi::get().getObjectMap(&phaseGame->data->midClient->core);
    if (!objectMap) {
        // No scenario loaded (menus, or BETWEEN scenarios) -> nothing to report. Clear the load-window
        // latch so a SECOND scenario load in the same process re-gates: g_mapSeen is a one-shot, and
        // without this reset a return-to-menu-then-new-skirmish would let the next in-place client-map
        // build be read mid-build again (the very crash the latch prevents on the first load).
        g_mapSeen = false;
        return;
    }

    std::string json;
    json.reserve(2048);
    buildJson(json, objectMap);

    std::lock_guard<std::mutex> lk(g_snapMutex);
    if (json != g_snapJson) {
        g_snapJson.swap(json);
        ++g_snapEpoch;
    }
    g_lastBuildTick = GetTickCount();
}

bool copyWorldSnapshot(std::string& outJson, std::uint32_t& outEpoch)
{
    std::lock_guard<std::mutex> lk(g_snapMutex);
    if (g_snapJson.empty())
        return false;
    outJson = g_snapJson;
    outEpoch = g_snapEpoch;
    return true;
}

bool install()
{
    if (!testenv::on("D2TESTDRV_WORLD"))
        return true;
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return false;
    g_includeUnitProfiles = testenv::on("D2TESTDRV_WORLD_PROFILES");
    g_enabled = true;
    spdlog::info("[testdrv] world-state reporter enabled (D2TESTDRV_WORLD)");
    return true;
}

} // namespace worldreporter
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV

