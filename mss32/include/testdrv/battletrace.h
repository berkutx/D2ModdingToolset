#ifndef TESTDRV_BATTLETRACE_H
#define TESTDRV_BATTLETRACE_H

#ifdef D2_TESTDRV
#include <cstdint>

namespace game {
struct BattleMsgData;
struct IMidgardObjectMap;
struct CMidgardID;
enum class BattleAction : int;
}

namespace hooks::testdrv::battletrace {

// Hook boundaries, not a promise that each scope represents one resolved attack.
// Destruction observes every normal return; unwinding is explicitly incomplete.
class Scope {
public:
    Scope(const char* kind, const game::BattleMsgData* battle,
          const game::IMidgardObjectMap* map = nullptr,
          const game::CMidgardID* actor = nullptr,
          const game::BattleAction* chosenAction = nullptr,
          const game::CMidgardID* chosenTarget = nullptr,
          const game::CMidgardID* chosenAttacker = nullptr) noexcept;
    ~Scope() noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    // Uses an already-resolved native map; never selects a map by thread identity.
    void setMap(const game::IMidgardObjectMap* value) noexcept { map = value; }

private:
    friend void damageHit(const game::BattleMsgData*, const game::IMidgardObjectMap*,
                          const game::CMidgardID*, const game::CMidgardID*,
                          int, int, int, int, int) noexcept;
    const char* kind;
    const game::BattleMsgData* battle;
    const game::IMidgardObjectMap* map;
    const game::CMidgardID* actor;
    const game::BattleAction* chosenAction;
    const game::CMidgardID* chosenTarget;
    const game::CMidgardID* chosenAttacker;
    Scope* previous{};
    std::uint32_t id{};
    int exceptions{};
};

// Only the normal-damage onHit hook's observed result, not misses/other attack classes.
void damageHit(const game::BattleMsgData* battle, const game::IMidgardObjectMap* map,
               const game::CMidgardID* actor, const game::CMidgardID* target,
               int hpBefore, int hpAfter, int normal, int critical, int total) noexcept;

} // namespace hooks::testdrv::battletrace
#endif
#endif

