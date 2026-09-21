#ifndef CLIENTCOMPATIBILITY_H
#define CLIENTCOMPATIBILITY_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hooks::compatibility {

/** Payload excludes ID_USER_PACKET_ENUM + 17: version, then the existing FilesHash. */
using Payload = std::array<std::uint8_t, 33>;

inline bool validHash(std::string_view hash) noexcept
{
    return hash.size() == 32 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline std::optional<Payload> encode(std::string_view hash) noexcept
{
    if (!validHash(hash)) return std::nullopt;
    Payload payload{};
    payload[0] = 1;
    std::copy(hash.begin(), hash.end(), payload.begin() + 1);
    return payload;
}

/** All methods run on the main thread. Only the supplied, owning computation runs asynchronously.
 * Destruction joins the future; no detached worker may outlive the service / DLL. */
class FilesHashCache
{
public:
    bool started() const noexcept { return started_; }
    bool pending() const noexcept { return future_.valid(); }
    void unavailable() noexcept { started_ = true; }

    template<class Compute>
    void start(Compute&& compute) noexcept
    {
        if (started_) return;
        started_ = true;
        try {
            future_ = std::async(std::launch::async, std::forward<Compute>(compute));
        } catch (...) { /* Unknown hash must not break login or start a retry loop. */ }
    }

    const std::string& value(bool wait = false) noexcept
    {
        try {
            if (future_.valid() && (wait || future_.wait_for(std::chrono::seconds(0))
                                              == std::future_status::ready)) {
                hash_ = future_.get();
                if (!validHash(hash_)) hash_.clear();
            }
        } catch (...) { hash_.clear(); }
        return hash_;
    }

private:
    bool started_{};
    std::string hash_;
    std::future<std::string> future_;
};

/** Only failed local enqueue attempts are retried, never a successfully queued reliable packet. */
class Publication
{
public:
    void begin() noexcept { pending_ = true; attempts_ = 0; nextAttemptMs_ = 0; }
    void stop() noexcept { pending_ = false; }
    bool due(std::uint64_t nowMs) const noexcept { return pending_ && nowMs >= nextAttemptMs_; }
    void attempted(bool sent, std::uint64_t nowMs) noexcept
    {
        pending_ = !sent && ++attempts_ < 3;
        nextAttemptMs_ = nowMs + 1000;
    }

private:
    bool pending_{};
    unsigned attempts_{};
    std::uint64_t nextAttemptMs_{};
};

} // namespace hooks::compatibility
#endif
