#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace twitchstat {

// Canonical scalar values only: comparison never includes native structure padding. Identities
// are tokens, never retained pointers to dereference. This is a change detector, not a revision of
// arbitrary Lua globals or the live scenario's modifier chain.
struct SlicedGuard
{
    std::array<std::uint32_t, 8> lifecycle;
    std::array<std::uintptr_t, 6> identity;
    std::array<std::int32_t, 6> geometry;
    std::array<std::int32_t, 12> roster;
    std::array<std::array<std::uint64_t, 49>, 22> units;
    std::array<std::array<std::uint64_t, 2>, 13> turns;
    std::array<std::uint64_t, 26> battle;

    bool operator==(const SlicedGuard& other) const
    {
        return lifecycle == other.lifecycle && identity == other.identity &&
               geometry == other.geometry && roster == other.roster && units == other.units &&
               turns == other.turns && battle == other.battle;
    }
    bool operator!=(const SlicedGuard& other) const { return !(*this == other); }
};

inline std::uint64_t readLittleEndian(const unsigned char* data, unsigned width)
{
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i)
        result |= static_cast<std::uint64_t>(data[i]) << (i * 8);
    return result;
}

// Verified Russobit/MNS public BattleMsgData: 3920 bytes, 22 UnitInfo records of 168 bytes.
// The caller must protect native reads. No virtual getters or Lua run here.
inline void copyBattleFields(const unsigned char* data, SlicedGuard* out)
{
    for (std::size_t i = 0; i < out->units.size(); ++i) {
        const auto* unit = data + i * 168;
        auto& values = out->units[i];
        std::size_t n = 0;
        values[n++] = readLittleEndian(unit, 4);       // IDs
        values[n++] = readLittleEndian(unit + 4, 4);
        values[n++] = readLittleEndian(unit + 8, 8);  // statuses
        for (unsigned at = 16; at < 24; at += 2)
            values[n++] = readLittleEndian(unit + at, 2); // DOT + extra attack count
        for (unsigned at = 24; at < 44; at += 4)
            values[n++] = readLittleEndian(unit + at, 4); // applied damage, immunity masks
        values[n++] = readLittleEndian(unit + 44, 2); // HP
        values[n++] = readLittleEndian(unit + 46, 2); // XP
        values[n++] = unit[48];                      // flags
        values[n++] = unit[49] & 1;                  // known hidden bit only
        for (unsigned at = 50; at < 55; ++at)
            values[n++] = unit[at];                  // applied rounds; +55 unknown omitted
        values[n++] = readLittleEndian(unit + 56, 4); // summon owner
        // Original ABI stores 8 pairs inline. Patched MSS stores a pointer in this fixed union.
        // These 16 words are conservative identity/content tokens only. The pointed-to 48-pair
        // allocation is NOT dereferenced without a verified runtime ABI discriminator; changes
        // exclusively inside that allocation are covered only by the bounded forced refresh.
        for (unsigned at = 60; at < 124; at += 4)
            values[n++] = readLittleEndian(unit + at, 4);
        for (unsigned at = 124; at < 168; at += 4)
            values[n++] = readLittleEndian(unit + at, 4); // modifiers[8], armor, attack reduction
    }
    for (std::size_t i = 0; i < out->turns.size(); ++i) {
        const auto* turn = data + 3696 + i * 8;
        out->turns[i][0] = readLittleEndian(turn, 4);
        out->turns[i][1] = turn[4]; // skip the three padding bytes
    }
    std::size_t n = 0;
    for (unsigned at = 3800; at < 3864; at += 4)
        out->battle[n++] = readLittleEndian(data + at, 4); // groups, players, stack unit IDs
    for (unsigned at = 3872; at < 3888; at += 4)
        out->battle[n++] = readLittleEndian(data + at, 4); // used items; omit unknown ids3
    out->battle[n++] = data[3888];                        // current round
    out->battle[n++] = readLittleEndian(data + 3896, 8);  // exact double value bits
    out->battle[n++] = readLittleEndian(data + 3904, 8);
    out->battle[n++] = data[3912];                        // battle flags
    out->battle[n++] = data[3913];
    out->battle[n++] = data[3914];                        // duel; omit trailing unknown/padding
}

inline std::uint32_t slicedAge(std::uint32_t now, std::uint32_t started)
{
    return now - started; // wrap-safe for these bounded intervals
}
inline bool slicedRefreshDue(std::uint32_t now, std::uint32_t started)
{
    return slicedAge(now, started) >= 1000;
}
inline bool slicedBatchExpired(std::uint32_t now, std::uint32_t started)
{
    return slicedAge(now, started) >= 2500;
}

} // namespace twitchstat
