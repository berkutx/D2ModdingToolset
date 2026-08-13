/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Lobby-chat reporter. See testdrv/lobbychatreporter.h.
 */

#ifdef D2_TESTDRV

#include "testdrv/lobbychatreporter.h"
#include <cstdint>
#include <deque>
#include <mutex>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace lobbychatreporter {

namespace {

std::mutex g_mutex;
struct Entry
{
    std::string t;
    std::string sender;
    std::string text;
};
std::deque<Entry> g_messages;
std::string g_json;
std::uint32_t g_epoch = 0;
constexpr std::size_t kMaxMessages = 80;

bool enabled()
{
    static const bool value =
        GetEnvironmentVariableA("D2TESTDRV_LOBBY_CHAT", nullptr, 0) > 0;
    return value;
}

std::string nowLocal()
{
    SYSTEMTIME time;
    GetLocalTime(&time);
    char buffer[24];
    wsprintfA(buffer, "%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth,
              time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

// The Russobit game build stores lobby strings as Windows-1251 bytes. Convert at the capture
// boundary so the framed payload and HTTP response are ordinary UTF-8 JSON (ASCII commands such
// as !luckytest remain byte-identical, while Cyrillic chat no longer turns into mojibake).
std::string gameTextToUtf8(const char* value)
{
    if (!value || !*value)
        return {};
    const int wideLength = MultiByteToWideChar(1251, 0, value, -1, nullptr, 0);
    if (wideLength <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    if (!MultiByteToWideChar(1251, 0, value, -1, wide.data(), wideLength))
        return {};
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, nullptr, 0, nullptr,
                                               nullptr);
    if (utf8Length <= 0)
        return {};
    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, utf8.data(), utf8Length, nullptr,
                             nullptr))
        return {};
    utf8.pop_back(); // WideCharToMultiByte counted the terminating NUL.
    return utf8;
}

void appendEscaped(std::string& out, const std::string& value)
{
    out += '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buffer[8];
                wsprintfA(buffer, "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void rebuild()
{
    g_json = "{\"messages\":[";
    bool first = true;
    for (const auto& message : g_messages) {
        if (!first)
            g_json += ',';
        first = false;
        g_json += "{\"t\":";
        appendEscaped(g_json, message.t);
        g_json += ",\"sender\":";
        appendEscaped(g_json, message.sender);
        g_json += ",\"text\":";
        appendEscaped(g_json, message.text);
        g_json += '}';
    }
    g_json += "]}";
}

} // namespace

void onChatReceived(const char* sender, const char* text)
{
    if (!enabled())
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_messages.size() >= kMaxMessages)
        g_messages.pop_front();
    g_messages.push_back({nowLocal(), gameTextToUtf8(sender), gameTextToUtf8(text)});
    rebuild();
    ++g_epoch;
}

bool copyChatLog(std::string& outJson, std::uint32_t& outEpoch)
{
    if (!enabled())
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_epoch == 0)
        return false;
    outJson = g_json;
    outEpoch = g_epoch;
    return true;
}

} // namespace lobbychatreporter
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
