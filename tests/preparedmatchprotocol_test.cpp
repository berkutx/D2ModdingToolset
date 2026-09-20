#include "preparedmatchprotocol.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void num(Bytes& b, std::uint32_t n, unsigned width) {
    for (unsigned shift = width * 8; shift; shift -= 8) b.push_back(static_cast<std::uint8_t>(n >> (shift - 8)));
}
void str(Bytes& b, const std::string& s) { num(b, static_cast<unsigned>(s.size()), 2); b.insert(b.end(), s.begin(), s.end()); }
Bytes header(unsigned char operation) {
    Bytes b{1, operation}; str(b, "prep-1"); str(b, "game_2"); str(b, "attempt-3"); num(b, 42, 4); return b;
}
Bytes offer() {
    auto b = header(0); str(b, "host"); str(b, "Fight.lua"); str(b, "0123456789abcdef0123456789abcdef"); str(b, "Fight");
    b.push_back(2); str(b, "size"); num(b, 72, 4); str(b, "spin:0"); num(b, 0xfffffffd, 4);
    b.push_back(1); str(b, "spin:0"); b.push_back(7); num(b, 10, 2);
    b.push_back(2); str(b, "host"); b.push_back(5); b.push_back(1); b.push_back(1);
    str(b, "joiner"); b.push_back(255); b.push_back(255); b.push_back(2);
    str(b, "host"); str(b, "Agreed conditions | no resource deductions"); return b;
}
}
int main() {
    try {
        using namespace hooks::prepared;
        auto bytes = offer(); Offer parsed;
        require(decodeOffer(bytes.data(), bytes.size(), parsed), "valid offer rejected");
        require(parsed.identity.revision == 42 && parsed.parameters.at("spin:0") == -3, "numeric encoding mismatch");
        require(parsed.participants[0].race == 5 && parsed.participants[0].lord == 1
            && parsed.participants[1].race == -1 && parsed.participants[1].lord == -1, "category encoding mismatch");
        require(parsed.ranked && parsed.unlockGui && parsed.simultaneous && parsed.simultaneousUntil == 10, "flags mismatch");
        for (std::size_t i = 0; i < bytes.size(); ++i)
            require(!decodeOffer(bytes.data(), i, parsed), "truncated offer accepted");
        auto invalid = bytes; invalid.push_back(0);
        require(!decodeOffer(invalid.data(), invalid.size(), parsed), "trailing byte accepted");
        invalid = bytes; invalid[0] = 2;
        require(!decodeOffer(invalid.data(), invalid.size(), parsed), "unknown version accepted");
        invalid = bytes; invalid[2] = 255; invalid[3] = 255;
        require(!decodeOffer(invalid.data(), invalid.size(), parsed), "oversize string accepted");
        auto cancel = header(2); Identity id;
        require(decodeCancel(cancel.data(), cancel.size(), id), "cancel rejected");
        for (std::size_t i = 0; i < cancel.size(); ++i)
            require(!decodeCancel(cancel.data(), i, id), "truncated cancel accepted");
        require(id == parsed.identity, "cancel identity mismatch");
        auto expected = header(1); expected.push_back(6); str(expected, "host-deferred");
        require(encodeStatus(id, State::Deferred, "host-deferred") == expected, "status differs from wire golden vector");
        require(encodeStatus(id, State::Error, std::string(129, 'x')).empty(), "oversize status accepted");
        require(validParameterKey("spin:0") && validParameterKey("spin:31") && validParameterKey("iterations"), "known key rejected");
        require(!validParameterKey("spin:32") && !validParameterKey("spin:01") && !validParameterKey("spin:-1")
            && !validParameterKey("race") && !validParameterKey("__proto__"), "unknown parameter accepted");
        require(!decodeOffer(nullptr, 1, parsed), "null accepted");
        std::cout << "prepared protocol: golden vectors, categories, every truncation, bounds and parameter keys passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
