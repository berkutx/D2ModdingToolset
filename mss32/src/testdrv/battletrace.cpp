// Optional, bounded raw observations. No decisions, property evaluation or RNG calls.
#ifdef D2_TESTDRV
#include "testdrv/battletrace.h"
#include "testdrv/testenv.h"
#include "battlemsgdata.h"
#include "midgardobjectmap.h"
#include "midunit.h"
#include <atomic>
#include <exception>
#include <string>
#include <spdlog/spdlog.h>

namespace hooks::testdrv::battletrace {
namespace {
constexpr std::uint32_t recordLimit = 8192;
std::atomic<std::uint32_t> g_sequence{0};
std::atomic<std::uint32_t> g_scopeId{0};
thread_local Scope* g_current = nullptr;

bool enabled() noexcept
{
    static const bool value = testenv::on("D2TESTDRV_BATTLE_TRACE");
    return value;
}

struct UnitSnapshot {
    std::uint32_t id;
    int battleHp;
    int mapHp;
    unsigned flags;
    int extraAttacks;
    std::uint64_t statuses;
    bool mapHpKnown;
};
struct QueueSnapshot { std::uint32_t id; int attacks; };
struct Damage { int hpBefore, hpAfter, normal, critical, total; };
struct Snapshot {
    std::uint32_t attacker, defender, actor, target, chosenAttacker;
    int round, action;
    bool ok, choiceKnown;
    QueueSnapshot queue[13];
    UnitSnapshot units[22];
};

// POD-only SEH boundary: an observation fault never consumes an engine exception.
void capture(Snapshot& out, const game::BattleMsgData* battle,
             const game::IMidgardObjectMap* map, const game::CMidgardID* actor,
             const game::BattleAction* action, const game::CMidgardID* target,
             const game::CMidgardID* chosenAttacker) noexcept
{
    __try {
        if (!battle)
            return;
        out.attacker = static_cast<std::uint32_t>(battle->attackerGroupId.value);
        out.defender = static_cast<std::uint32_t>(battle->defenderGroupId.value);
        out.round = battle->currentRound;
        out.actor = actor ? static_cast<std::uint32_t>(actor->value) : 0;
        out.target = target ? static_cast<std::uint32_t>(target->value) : 0;
        out.chosenAttacker = chosenAttacker
                                ? static_cast<std::uint32_t>(chosenAttacker->value) : 0;
        if (action) {
            out.action = static_cast<int>(*action);
            out.choiceKnown = true;
        }
        for (unsigned i = 0; i < 13; ++i) {
            out.queue[i].id = static_cast<std::uint32_t>(battle->turnsOrder[i].unitId.value);
            out.queue[i].attacks = battle->turnsOrder[i].attackCount;
        }
        for (unsigned i = 0; i < 22; ++i) {
            const auto& raw = battle->unitsInfo[i];
            auto& unit = out.units[i];
            unit.id = static_cast<std::uint32_t>(raw.unitId1.value);
            unit.battleHp = raw.unitHp;
            unit.flags = raw.unitFlags.value;
            unit.extraAttacks = raw.extraAttackCount;
            unit.statuses = raw.unitStatuses;
            if (map && ((unit.id >> 16) & 0x3fu) == static_cast<unsigned>(game::IdType::Unit)) {
                const auto* object = map->vftable->findScenarioObjectById(map, &raw.unitId1);
                if (object && object->id == raw.unitId1) {
                    unit.mapHp = static_cast<const game::CMidUnit*>(object)->currentHp;
                    unit.mapHpKnown = true;
                }
            }
        }
        out.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.ok = false;
    }
}

void emit(const char* kind, const char* edge, std::uint32_t scope, std::uint32_t parent,
          const game::BattleMsgData* battle, const game::IMidgardObjectMap* map,
          const game::CMidgardID* actor, const game::BattleAction* action = nullptr,
          const game::CMidgardID* target = nullptr,
          const game::CMidgardID* chosenAttacker = nullptr,
          const Damage* damage = nullptr) noexcept
{
    try {
        const auto seq = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (seq > recordLimit) {
            if (seq == recordLimit + 1)
                spdlog::debug("[battle-trace] {{\"v\":1,\"seq\":{},\"kind\":\"limit\",\"complete\":false}}", seq);
            return;
        }
        Snapshot s{};
        capture(s, battle, map, actor, action, target, chosenAttacker);
        // Reserve seq before formatting: an allocation failure leaves an auditable gap.
        const auto extra = damage ? fmt::format(
            ",\"hpBefore\":{},\"hpAfter\":{},\"normal\":{},\"critical\":{},\"total\":{}",
            damage->hpBefore, damage->hpAfter, damage->normal, damage->critical, damage->total)
                                  : std::string{};
        std::string queue, units;
        if (s.ok) {
            for (unsigned i = 0; i < 13; ++i) {
                if (i) queue += ',';
                queue += fmt::format("[{},{}]", s.queue[i].id, s.queue[i].attacks);
            }
            for (unsigned i = 0; i < 22; ++i) {
                const auto& u = s.units[i];
                if (((u.id >> 16) & 0x3fu) != static_cast<unsigned>(game::IdType::Unit))
                    continue;
                if (!units.empty()) units += ',';
                units += fmt::format("[{},{},{},{},{},{},{}]", i, u.id, u.battleHp,
                                     u.mapHpKnown ? std::to_string(u.mapHp) : "null",
                                     u.flags, u.extraAttacks, u.statuses);
            }
        }
        // Debug avoids the normal info-level flush on each hit. No private logger/thread.
        spdlog::debug("[battle-trace] {{\"v\":1,\"seq\":{},\"scope\":{},\"parent\":{},"
                      "\"kind\":\"{}\",\"edge\":\"{}\",\"pid\":{},\"tid\":{},\"tick\":{},"
                      "\"battle\":{},\"map\":{},\"attackerGroup\":{},\"defenderGroup\":{},"
                      "\"round\":{},\"actor\":{},\"snapshotOk\":{},\"choiceKnown\":{},"
                      "\"action\":{},\"target\":{},\"chosenAttacker\":{},\"queue\":[{}],\"units\":[{}]{}}}",
                      seq, scope, parent, kind, edge, GetCurrentProcessId(), GetCurrentThreadId(),
                      GetTickCount(), reinterpret_cast<std::uintptr_t>(battle),
                      reinterpret_cast<std::uintptr_t>(map), s.attacker, s.defender, s.round,
                      s.actor, s.ok, s.choiceKnown, s.action, s.target, s.chosenAttacker,
                      queue, units, extra);
    } catch (...) {
        // A missing sequence/edge is incomplete evidence, never a game failure or synthetic pass.
    }
}
} // namespace

Scope::Scope(const char* kind, const game::BattleMsgData* battle,
             const game::IMidgardObjectMap* map, const game::CMidgardID* actor,
             const game::BattleAction* chosenAction, const game::CMidgardID* chosenTarget,
             const game::CMidgardID* chosenAttacker) noexcept
    : kind(kind), battle(battle), map(map), actor(actor), chosenAction(chosenAction),
      chosenTarget(chosenTarget), chosenAttacker(chosenAttacker)
{
    if (!enabled())
        return;
    previous = g_current;
    if (!this->map && previous && previous->battle == battle)
        this->map = previous->map;
    id = g_scopeId.fetch_add(1, std::memory_order_relaxed) + 1;
    exceptions = std::uncaught_exceptions();
    g_current = this;
    // Output parameters are not initialized until the observed hook returns.
    emit(kind, "enter", id, previous ? previous->id : 0, battle, this->map, actor);
}

Scope::~Scope() noexcept
{
    if (!id)
        return;
    const bool unwinding = std::uncaught_exceptions() > exceptions;
    emit(kind, unwinding ? "unwind" : "leave", id, previous ? previous->id : 0,
         battle, map, actor, unwinding ? nullptr : chosenAction,
         unwinding ? nullptr : chosenTarget, unwinding ? nullptr : chosenAttacker);
    g_current = previous;
}

void damageHit(const game::BattleMsgData* battle, const game::IMidgardObjectMap* map,
               const game::CMidgardID* actor, const game::CMidgardID* target,
               int hpBefore, int hpAfter, int normal, int critical, int total) noexcept
{
    if (!enabled())
        return;
    const Damage damage{hpBefore, hpAfter, normal, critical, total};
    const auto scope = g_current && g_current->battle == battle ? g_current->id : 0;
    emit("damage-hit", "observed", scope, 0, battle, map, actor, nullptr, target,
         nullptr, &damage);
}
} // namespace hooks::testdrv::battletrace
#endif

