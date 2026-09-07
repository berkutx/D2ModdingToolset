#ifndef TESTDRV_JSON_H
#define TESTDRV_JSON_H

#include <string>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace json {

// UI/world snapshots encode control and non-ASCII bytes as Latin-1 codepoints.
// Lobby chat uses UTF-8 and deliberately has a different escaping contract.
inline void appendEscaped(std::string& out, const char* s)
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

inline void kvInt(std::string& out, const char* key, int v)
{
    out += '"';
    out += key;
    out += "\":";
    char buf[16];
    wsprintfA(buf, "%d", v);
    out += buf;
}

} // namespace json
} // namespace testdrv
} // namespace hooks

#endif // TESTDRV_JSON_H
