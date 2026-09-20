#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace twitchstat {
namespace gametext_detail {

inline bool digit(char value) { return value >= '0' && value <= '9'; }
inline bool letter(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

// A field must be complete before it can be consumed. In particular, a truncated RGB
// directive must not eat its first component and leave apparently valid game numbers.
inline bool unsignedField(const char* text, std::size_t length, std::size_t& at,
                          std::size_t maxDigits, bool color)
{
    const std::size_t begin = at;
    unsigned value = 0;
    while (at < length && digit(text[at]) && at - begin < maxDigits) {
        if (color)
            value = value * 10 + static_cast<unsigned>(text[at] - '0');
        ++at;
    }
    if (at == begin || at >= length || text[at] != ';' || (color && value > 255))
        return false;
    ++at;
    return true;
}

// These are the directives present in the game's interface resources and emitted by
// this repository. Unknown/malformed syntax is copied verbatim instead of searching
// for an arbitrary next semicolon and discarding potentially meaningful text.
inline std::size_t formattingEnd(const char* text, std::size_t length, std::size_t begin,
                                 bool& rowBoundary)
{
    rowBoundary = false;
    if (begin + 1 >= length)
        return begin;
    const char command = text[begin + 1];
    std::size_t at = begin + 2;
    if (command == 'c' || command == 'o') {
        // interfaceutils.cpp: RGB text color; movepathhooks.cpp: RGB outline color.
        for (unsigned component = 0; component < 3; ++component)
            if (!unsignedField(text, length, at, 3, true))
                return begin;
        return at;
    }
    if (command == 'f') {
        if (at >= length || !letter(text[at]))
            return begin;
        while (at < length && (letter(text[at]) || digit(text[at]) ||
                               text[at] == '_' || text[at] == '-'))
            ++at;
        return at < length && text[at] == ';' ? at + 1 : begin;
    }
    if (command == 'h' || command == 'v') {
        if (at + 1 >= length || text[at + 1] != ';')
            return begin;
        const char value = text[at];
        const bool valid = command == 'h' ? (value == 'L' || value == 'C' || value == 'R')
                                          : (value == 'T' || value == 'C' || value == 'B');
        return valid ? at + 2 : begin;
    }
    if (command == 's' || command == 'p')
        return unsignedField(text, length, at, 10, false) ? at : begin;
    if (command == 'm' && at < length && text[at] == 'L') {
        ++at;
        if (!unsignedField(text, length, at, 10, false))
            return begin;
        // TApp.dbf X005TA0423 (also enclayoutunithooks.cpp getTxtStatsText):
        // ...\p97;%IMMU%\mL0;\fMedbold;...%WARD%. This layout reset separates
        // the immunity and ward rows. Preserve that boundary in the plain-text view.
        rowBoundary = true;
        return at;
    }
    return begin;
}

} // namespace gametext_detail

// Input/output retain the game's original byte encoding. Conversion to UTF-8 belongs
// to the caller. Formatting numbers are consumed only inside a validated directive;
// literal numbers, punctuation, unknown directives and existing line breaks survive.
inline std::string stripGameMarkup(const char* source)
{
    if (!source)
        return {};
    const std::size_t length = std::strlen(source);
    std::string result;
    result.reserve(length);
    for (std::size_t at = 0; at < length;) {
        const char value = source[at];
        if (value == '\r') {
            result.push_back('\n');
            at += at + 1 < length && source[at + 1] == '\n' ? 2 : 1;
            continue;
        }
        if (value != '\\' || at + 1 >= length) {
            result.push_back(value);
            ++at;
            continue;
        }
        const char command = source[at + 1];
        if (command == 'n' || command == 't' || command == '\\') {
            result.push_back(command == 'n' ? '\n' : command == 't' ? '\t' : '\\');
            at += 2;
            continue;
        }
        bool rowBoundary = false;
        const std::size_t end = gametext_detail::formattingEnd(source, length, at, rowBoundary);
        if (end != at) {
            if (rowBoundary && !result.empty() && result.back() != '\n')
                result.push_back('\n');
            at = end;
            continue;
        }
        result.push_back(value);
        ++at;
    }
    return result;
}

} // namespace twitchstat
