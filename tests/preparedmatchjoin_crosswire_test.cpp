#include "preparedmatch.h" // CoreRoot/lobby/server must precede the MSS include path.
#include "preparedmatchprotocol.h"
#include <cstdio>

namespace core = conclave::prepared;
namespace client = hooks::prepared;
namespace { unsigned checks{}, failures{}; }
#define CHECK(v) do { ++checks; if (!(v)) { ++failures; std::fprintf(stderr, "FAIL join-crosswire:%d %s\n", __LINE__, #v); } } while (false)

void roundTrip(const core::JoinOffer& offer) {
    const auto bytes = core::encodeJoinOffer(offer);
    CHECK(!bytes.empty());
    client::JoinOffer decoded;
    CHECK(client::decodeJoinOffer(bytes.data(), bytes.size(), decoded));
    CHECK(decoded.target.identity.preparationId == offer.identity.preparationId);
    CHECK(decoded.target.identity.gameId == offer.identity.gameId);
    CHECK(decoded.target.identity.attemptId == offer.identity.attemptId);
    CHECK(decoded.target.identity.revision == offer.identity.revision);
    CHECK(decoded.target.roomId == offer.roomId && decoded.host == offer.host);
    CHECK(decoded.title == offer.title && decoded.recipient == offer.recipient);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        CHECK(!client::decodeJoinOffer(bytes.data(), n, decoded));
    auto trailing = bytes; trailing.push_back(0);
    CHECK(!client::decodeJoinOffer(trailing.data(), trailing.size(), decoded));
    const auto cancel = core::encodeJoinCancel(offer.identity, offer.roomId);
    client::JoinIdentity target;
    CHECK(client::decodeJoinCancel(cancel.data(), cancel.size(), target));
    CHECK(target == decoded.target);
    for (std::size_t n = 0; n < cancel.size(); ++n)
        CHECK(!client::decodeJoinCancel(cancel.data(), n, target));
    for (unsigned state = 0; state <= 5; ++state) {
        const auto status = client::encodeJoinStatus(target, static_cast<client::JoinState>(state), std::string(128, 'd'));
        core::JoinStatus received;
        CHECK(core::decodeJoinStatus(status.data(), status.size(), received));
        CHECK(received.identity == offer.identity && received.roomId == offer.roomId);
        CHECK(static_cast<unsigned>(received.state) == state && received.detail == std::string(128, 'd'));
        for (std::size_t n = 0; n < status.size(); ++n)
            CHECK(!core::decodeJoinStatus(status.data(), n, received));
        auto extra = status; extra.push_back(0);
        CHECK(!core::decodeJoinStatus(extra.data(), extra.size(), received));
    }
    for (unsigned version = 0; version <= 255; ++version) {
        if (version == 1) continue;
        auto wrong = bytes; wrong[0] = static_cast<std::uint8_t>(version);
        CHECK(!client::decodeJoinOffer(wrong.data(), wrong.size(), decoded));
    }
}

int main() {
    core::JoinOffer offer{{"prep-1", "game-1", "attempt-1", 1}, 0, "Alice", "Fight", "Bob"};
    roundTrip(offer);
    offer.roomId = UINT32_MAX - 1; offer.title.clear(); roundTrip(offer);
    offer.identity = {std::string(64, 'p'), std::string(64, 'g'), std::string(64, 'a'), UINT32_MAX};
    offer.host.assign(192, 'h'); offer.title.assign(256, 't'); offer.recipient.assign(192, 'r');
    CHECK(core::encodeJoinOffer(offer).size() == 854); roundTrip(offer);
    offer.roomId = UINT32_MAX; CHECK(core::encodeJoinOffer(offer).empty());
    std::printf("prepared join crosswire: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
