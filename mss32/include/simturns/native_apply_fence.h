#ifndef SIMTURNS_NATIVE_APPLY_FENCE_H
#define SIMTURNS_NATIVE_APPLY_FENCE_H

#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

namespace hooks::simturns {
/** UI-owned causal watermark; later packets never prevent an earlier barrier.
 * Completion means native dispatch AND, for client commands, command-queue drain. */
class NativeApplyFence {
public:
    std::uint64_t issue() {
        if (issued == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("native receive sequence exhausted");
        return ++issued;
    }
    bool complete(std::uint64_t ticket) {
        if (!ticket || ticket > issued || ticket <= applied || !completed.insert(ticket).second)
            return false;
        while (completed.erase(applied + 1)) ++applied;
        return true;
    }
    std::uint64_t watermark() const { return issued; }
    bool reached(std::uint64_t barrier) const { return barrier <= applied; }
private:
    std::uint64_t issued{}, applied{};
    std::set<std::uint64_t> completed;
};
} // namespace hooks::simturns
#endif
