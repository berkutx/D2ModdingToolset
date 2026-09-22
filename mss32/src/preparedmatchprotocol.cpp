#include "preparedmatchprotocol.h"
#include <algorithm>
#include <set>

namespace hooks::prepared {
namespace {
bool text(const std::string& value, std::size_t max, bool empty = false) {
    if ((!empty && value.empty()) || value.size() > max) return false;
    for (unsigned char c : value) if (c < 32 || c == 127) return false;
    return true;
}
bool validId(const std::string& value) {
    return text(value, 64) && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool validIdentity(const Identity& v) {
    return validId(v.preparationId) && validId(v.gameId) && validId(v.attemptId) && v.revision;
}
struct Reader {
    const std::uint8_t* at; const std::uint8_t* end;
    bool number(std::uint32_t& n, unsigned width) {
        if (static_cast<std::size_t>(end - at) < width) return false;
        n = 0; while (width--) n = (n << 8) | *at++; return true;
    }
    bool string(std::string& s, std::size_t max, bool empty = false) {
        std::uint32_t n;
        if (!number(n, 2) || n > max || static_cast<std::size_t>(end - at) < n) return false;
        s.assign(reinterpret_cast<const char*>(at), n); at += n; return text(s, max, empty);
    }
    bool identity(Identity& v) {
        return string(v.preparationId, 64) && string(v.gameId, 64)
            && string(v.attemptId, 64) && number(v.revision, 4) && validIdentity(v);
    }
};
void number(std::vector<std::uint8_t>& out, std::uint32_t n, unsigned width) {
    for (unsigned shift = width * 8; shift; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(n >> (shift - 8)));
}
void string(std::vector<std::uint8_t>& out, const std::string& s) {
    number(out, static_cast<std::uint32_t>(s.size()), 2); out.insert(out.end(), s.begin(), s.end());
}
}
bool validParameterKey(const std::string& key) {
    static const char* standard[] = {"size", "roads", "forest", "startingGold", "startingNativeMana",
        "water", "maxUnit", "maxSpell", "maxLeader", "maxCity", "startingLevel", "iterations"};
    for (auto name : standard) if (key == name) return true;
    if (key.size() < 6 || key.size() > 7 || key.compare(0, 5, "spin:") != 0) return false;
    if (key.size() == 7 && key[5] == '0') return false;
    unsigned n = 0;
    for (std::size_t i = 5; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return false;
        n = n * 10 + key[i] - '0';
    }
    return n < 32;
}
bool decodeOffer(const std::uint8_t* bytes, std::size_t length, Offer& offer) noexcept {
    if (!bytes || length < 2 || length > 8192 || bytes[0] != 1 || bytes[1] != 0) return false;
    try {
        Reader r{bytes + 2, bytes + length}; Offer v;
        if (!r.identity(v.identity) || !r.string(v.host, 192) || !r.string(v.filename, 128)
            || v.filename.find_first_of("/\\:") != std::string::npos
            || v.filename == "." || v.filename == ".." || !r.string(v.md5, 32)
            || v.md5.size() != 32 || !std::all_of(v.md5.begin(), v.md5.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }) || !r.string(v.title, 256, true)) return false;
        std::uint32_t count, n;
        if (!r.number(count, 1) || count > 44) return false;
        while (count--) {
            std::string key;
            if (!r.string(key, 32) || !validParameterKey(key) || !r.number(n, 4)
                || !v.parameters.emplace(key, static_cast<std::int32_t>(n)).second) return false;
        }
        if (!r.number(count, 1) || count > 44) return false;
        std::set<std::string> explicitKeys;
        while (count--) {
            std::string key;
            if (!r.string(key, 32) || !v.parameters.count(key) || !explicitKeys.insert(key).second)
                return false;
            v.explicitParameters.push_back(std::move(key));
        }
        if (!r.number(n, 1) || n > 7) return false;
        v.ranked = n & 1; v.unlockGui = n & 2; v.simultaneous = n & 4;
        if (!r.number(n, 2)) return false;
        v.simultaneousUntil = static_cast<std::uint16_t>(n);
        if (!r.number(count, 1) || count < 2 || count > 4) return false;
        std::set<std::string> names;
        while (count--) {
            Participant p;
            if (!r.string(p.name, 192) || !names.insert(p.name).second || !r.number(n, 1)) return false;
            p.race = static_cast<std::int8_t>(n);
            if (!(p.race == -1 || p.race == 0 || p.race == 1 || p.race == 2 || p.race == 3 || p.race == 5)
                || !r.number(n, 1)) return false;
            p.lord = static_cast<std::int8_t>(n);
            if (p.lord < -1 || p.lord > 2 || !r.number(n, 1) || n < 1 || n > 4) return false;
            p.team = static_cast<std::uint8_t>(n); v.participants.push_back(std::move(p));
        }
        if (!r.string(v.firstTurn, 192, true) || !r.string(v.summary, 2048, true) || r.at != r.end
            || !names.count(v.host) || (!v.firstTurn.empty() && !names.count(v.firstTurn))) return false;
        offer = std::move(v); return true;
    } catch (...) { return false; }
}
bool decodeCancel(const std::uint8_t* bytes, std::size_t length, Identity& identity) noexcept {
    if (!bytes || length < 2 || length > 204 || bytes[0] != 1 || bytes[1] != 2) return false;
    try {
        Reader r{bytes + 2, bytes + length}; Identity v;
        if (!r.identity(v) || r.at != r.end) return false;
        identity = std::move(v); return true;
    } catch (...) { return false; }
}
std::vector<std::uint8_t> encodeStatus(const Identity& v, State state, const std::string& detail) {
    if (!validIdentity(v) || !text(detail, 128, true) || static_cast<unsigned>(state) > 8) return {};
    std::vector<std::uint8_t> out{1, 1};
    string(out, v.preparationId); string(out, v.gameId); string(out, v.attemptId);
    number(out, v.revision, 4); out.push_back(static_cast<std::uint8_t>(state)); string(out, detail);
    return out;
}
} // namespace hooks::prepared
