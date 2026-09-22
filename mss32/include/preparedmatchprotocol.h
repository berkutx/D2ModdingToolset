#ifndef PREPAREDMATCHPROTOCOL_H
#define PREPAREDMATCHPROTOCOL_H
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hooks::prepared {
struct Identity {
    std::string preparationId, gameId, attemptId;
    std::uint32_t revision{};
    bool operator==(const Identity& b) const {
        return preparationId == b.preparationId && gameId == b.gameId
            && attemptId == b.attemptId && revision == b.revision;
    }
};
struct Participant {
    std::string name;
    std::int8_t race{-1}, lord{-1};
    std::uint8_t team{1};
};
struct Offer {
    Identity identity;
    std::string host, filename, md5, title, firstTurn, summary;
    std::map<std::string, std::int32_t> parameters;
    std::vector<std::string> explicitParameters;
    std::vector<Participant> participants;
    bool ranked{}, unlockGui{}, simultaneous{};
    std::uint16_t simultaneousUntil{};
};
enum class Operation : std::uint8_t { Offer = 0, Status = 1, Cancel = 2 };
enum class State : std::uint8_t {
    Received, Busy, Confirmation, Accepted, Generating, RoomCreated, Deferred, Error, Canceled
};
// Bytes exclude the append-only RakNet packet ID. All integers are big-endian.
bool decodeOffer(const std::uint8_t* bytes, std::size_t length, Offer& offer) noexcept;
bool decodeCancel(const std::uint8_t* bytes, std::size_t length, Identity& identity) noexcept;
std::vector<std::uint8_t> encodeStatus(const Identity& identity, State state,
                                      const std::string& detail);
bool validParameterKey(const std::string& key);
} // namespace hooks::prepared
#endif
