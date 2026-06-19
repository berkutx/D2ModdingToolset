/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * World-state reporter. See testdrv/worldreporter.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 *
 * Reuses the mod's own state-surfacing layer (bindings::ScenarioView + the *View
 * wrappers), which are plain pointer wrappers usable from C++ with no Lua VM, so
 * the reporter mirrors how the mod already reads players and stacks. Mod style for
 * these reads is null-checks, not SEH; crash-safety comes from the thin __try
 * wrapper in autonav (these calls allocate, so __try cannot live here: MSVC C2712).
 */

#ifdef D2_TESTDRV

#include "testdrv/worldreporter.h"
#include "testdrv/testenv.h"
#include "bindings/currencyview.h"
#include "bindings/groupview.h"
#include "bindings/idview.h"
#include "bindings/playerview.h"
#include "bindings/point.h"
#include "bindings/scenarioview.h"
#include "bindings/stackview.h"
#include "bindings/unitview.h" // getGroup().getUnits() yields std::vector<UnitView>; .size() needs it complete
#include "gameutils.h"
#include "midgard.h"
#include "midgardid.h"
#include "utils.h"
#include "version.h"
#include <cstdint>
#include <mutex>
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
DWORD g_lastBuildTick = 0; // GetTickCount of the last rebuild (the walk is heavier than the UI one)
constexpr DWORD kThrottleMs = 500;

// JSON helpers (mirror uistatereporter): ASCII keys; \u-escape control + non-ASCII bytes as Latin-1
// codepoints so the payload is always valid JSON without a cp1251 table.
void appendEscaped(std::string& out, const char* s)
{
    out += '"';
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
            const unsigned char c = *p;
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    char buf[8];
                    wsprintfA(buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
            }
        }
    }
    out += '"';
}

void kvStr(std::string& out, const char* key, const char* val)
{
    out += '"';
    out += key;
    out += "\":";
    appendEscaped(out, val);
}

void kvInt(std::string& out, const char* key, int v)
{
    out += '"';
    out += key;
    out += "\":";
    char buf[16];
    wsprintfA(buf, "%d", v);
    out += buf;
}

void kvBool(std::string& out, const char* key, bool v)
{
    out += '"';
    out += key;
    out += "\":";
    out += v ? "true" : "false";
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

    game::CMidgardID neutralId = game::emptyId;
    if (auto* neutral = hooks::getNeutralPlayer(objectMap))
        neutralId = PlayerView{neutral, objectMap}.getId().id;

    ScenarioView scenario{objectMap};

    json += '{';
    kvInt(json, "day", scenario.getCurrentDay());

    json += ",\"players\":[";
    bool firstPlayer = true;
    scenario.forEachPlayer([&](const PlayerView& p) {
        const auto id = p.getId();
        const auto bank = p.getBank();
        if (!firstPlayer)
            json += ',';
        firstPlayer = false;
        json += '{';
        kvStr(json, "id", hooks::idToString(&id.id).c_str());
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
        const int units = (int)s.getGroup().getUnits().size();
        if (!firstStack)
            json += ',';
        firstStack = false;
        json += '{';
        kvStr(json, "id", hooks::idToString(&id.id).c_str());
        json += ',';
        kvInt(json, "x", pos.x);
        json += ',';
        kvInt(json, "y", pos.y);
        json += ',';
        kvStr(json, "owner", hooks::idToString(&ownerId).c_str());
        json += ',';
        kvStr(json, "relation", relationOf(ownerId, localId, neutralId));
        json += ',';
        kvInt(json, "movement", s.getMovement());
        json += ',';
        kvInt(json, "units", units);
        json += ',';
        kvInt(json, "subrace", s.getSubrace());
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

    // Throttle: the object-map walk is heavier than the per-dialog UI snapshot, so rebuild at most
    // ~every 500ms instead of every frame.
    const DWORD now = GetTickCount();
    if (g_lastBuildTick != 0 && (now - g_lastBuildTick) < kThrottleMs)
        return;
    g_lastBuildTick = now;

    const game::IMidgardObjectMap* objectMap = hooks::getObjectMap();
    if (!objectMap)
        return; // no scenario loaded yet (menus) -> nothing to report

    std::string json;
    json.reserve(2048);
    buildJson(json, objectMap);

    std::lock_guard<std::mutex> lk(g_snapMutex);
    if (json != g_snapJson) {
        g_snapJson.swap(json);
        ++g_snapEpoch;
    }
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

void install()
{
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit)
        return;
    if (!testenv::on("D2TESTDRV_WORLD"))
        return;
    g_enabled = true;
    spdlog::info("[testdrv] world-state reporter enabled (D2TESTDRV_WORLD)");
}

} // namespace worldreporter
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
