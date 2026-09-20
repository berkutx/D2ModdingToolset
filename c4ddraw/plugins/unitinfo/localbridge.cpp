#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>

#include "localbridge.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace twitchstat {
namespace {

const char* const kHost = "127.0.0.1:8765";
const char* const kOrigin = "http://127.0.0.1:8765";
const char* const kExtensionOrigin = "https://pvffxvvhlpi5o8qe3ybjjpwb5hh7n7.ext-twitch.tv";
const size_t kMaxFrame = 1024 * 1024;
const size_t kMaxHeaders = 8192;
const size_t kMaxClients = 12;
const ULONGLONG kStaleMs = 6000;
const ULONGLONG kClientTimeoutMs = 10000;
const ULONGLONG kSendTimeoutMs = 5000;

// Command's low bit is enabled, upper bits identify a particular enable/disable
// cycle. A failed bind is retried only after another explicit off/on cycle.
volatile LONG g_command = 0;
volatile LONG g_shutdown = 0;
volatile LONG g_state = static_cast<LONG>(LocalBridgeState::Stopped);
volatile LONG g_error = 0;
volatile LONG g_invalidation = 0;
volatile LONG g_idleCommand = -1;
INIT_ONCE g_startOnce = INIT_ONCE_STATIC_INIT;
PVOID volatile g_wake = nullptr;
SRWLOCK g_frameLock = SRWLOCK_INIT;

struct Frame {
    std::string json;
    unsigned long long capturedAt;
    LONG command;
    LONG invalidation;
    bool active;
};
std::shared_ptr<const Frame> g_frame;

LONG atomicRead(volatile LONG* value) { return InterlockedCompareExchange(value, 0, 0); }
HANDLE wakeEvent() { return static_cast<HANDLE>(InterlockedCompareExchangePointer(&g_wake, nullptr, nullptr)); }
void wakeWorker() { const HANDLE event = wakeEvent(); if (event) SetEvent(event); }

unsigned long long unixMs()
{
    FILETIME value;
    GetSystemTimeAsFileTime(&value);
    ULARGE_INTEGER ticks;
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return (ticks.QuadPart - 116444736000000000ULL) / 10000ULL;
}

void setState(LONG command, LocalBridgeState state, int error = 0)
{
    if (atomicRead(&g_command) != command) return;
    InterlockedExchange(&g_error, error);
    InterlockedExchange(&g_state, static_cast<LONG>(state));
}

struct Client {
    SOCKET socket = INVALID_SOCKET;
    std::string request;
    std::string output;
    size_t sent = 0;
    ULONGLONG acceptedAt = 0;
    ULONGLONG queuedAt = 0;
    ULONGLONG nextEvent = 0;
    bool headersDone = false;
    bool events = false;
    bool readClosed = false;

    void close()
    {
        if (socket != INVALID_SOCKET) closesocket(socket);
        socket = INVALID_SOCKET;
        // Release large frames rather than retaining a 1 MiB allocation per slot.
        std::string().swap(request);
        std::string().swap(output);
        sent = 0;
        headersDone = events = readClosed = false;
    }
};

struct Network {
    SOCKET listener = INVALID_SOCKET;
    std::array<Client, kMaxClients> clients;
    ~Network() { close(); }
    void close()
    {
        if (listener != INVALID_SOCKET) closesocket(listener);
        listener = INVALID_SOCKET;
        for (auto& client : clients) client.close();
    }
};

std::string responseHeaders(int code, const char* type, size_t length, bool events)
{
    const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" :
        code == 403 ? "Forbidden" : code == 404 ? "Not Found" :
        code == 405 ? "Method Not Allowed" : code == 431 ? "Request Header Fields Too Large" : "Internal Server Error";
    char first[160];
    std::snprintf(first, sizeof(first), "HTTP/1.1 %d %s\r\n", code, reason);
    std::string result(first);
    result += "Content-Type: "; result += type;
    result += "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Cross-Origin-Resource-Policy: same-origin\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'self' https://extension-files.twitch.tv; "
        "style-src 'self'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; object-src 'none'; frame-ancestors 'none'\r\n"
        "Referrer-Policy: no-referrer\r\nConnection: close\r\n";
    if (!events) {
        char size[64];
        std::snprintf(size, sizeof(size), "Content-Length: %llu\r\n", static_cast<unsigned long long>(length));
        result += size;
    }
    result += "\r\n";
    return result;
}

void reply(Client& client, int code, const char* text, size_t size, const char* type = "text/plain; charset=utf-8")
{
    client.output = responseHeaders(code, type, size, false);
    client.output.append(text, size);
    client.sent = 0;
    client.queuedAt = GetTickCount64();
    client.headersDone = true;
}

void reply(Client& client, int code, const std::string& text, const char* type = "text/plain; charset=utf-8")
{
    reply(client, code, text.data(), text.size(), type);
}

std::string inactive(const char* code, const char* message, unsigned long long now)
{
    char prefix[256];
    const DWORD pid = GetCurrentProcessId();
    std::snprintf(prefix, sizeof(prefix),
        "{\"frame\":{\"schema\":\"c4dll.twitch-frame\",\"version\":1,\"pid\":%lu,"
        "\"battle_id\":\"bridge:%s\",\"ts\":%llu,\"active\":false,\"snapshot\":null},"
        "\"status\":{\"pid\":%lu,\"code\":\"%s\",\"message\":\"", pid, code, now, pid, code);
    return std::string(prefix) + message + "\"}}";
}

std::string envelope(LONG command)
{
    std::shared_ptr<const Frame> frame;
    AcquireSRWLockShared(&g_frameLock);
    frame = g_frame;
    ReleaseSRWLockShared(&g_frameLock);
    const unsigned long long now = unixMs();
    if (!frame || frame->command != command) {
        if (atomicRead(&g_idleCommand) == command)
            return inactive("idle", "Игра подключена. Ожидаем поддерживаемый бой.", now);
        return inactive("missing", "Ожидаем данные плагина TwitchStat.", now);
    }
    // Keep idle frames fresh even when no battle is open and no capture runs.
    if (!frame->active || frame->invalidation != atomicRead(&g_invalidation))
        return inactive("idle", "Игра подключена. Ожидаем поддерживаемый бой.", now);
    if (frame->json.empty())
        return inactive("invalid", "Снимок игры превышает допустимый размер или имеет неверный формат.", now);
    if (frame->capturedAt > now + 5000 || (now >= frame->capturedAt && now - frame->capturedAt >= kStaleMs))
        return inactive("stale", "Данные игры устарели. Ожидаем следующий снимок боя.", now);
    char suffix[192];
    std::snprintf(suffix, sizeof(suffix),
        ",\"status\":{\"code\":\"battle\",\"pid\":%lu,\"message\":\"Бой подключён. Данные обновляются.\"}}", GetCurrentProcessId());
    return "{\"frame\":" + frame->json + suffix;
}

std::string eventPayload(const std::string& json)
{
    // The native serializer emits pretty JSON. SSE requires one data: prefix
    // per physical line; EventSource joins them with newlines before JSON.parse.
    std::string result;
    result.reserve(json.size() + 32);
    result = "data: ";
    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < json.size() && json[i + 1] == '\n') ++i;
            result += "\ndata: ";
        } else result += c;
    }
    result += "\n\n";
    return result;
}

std::string lower(const std::string& text)
{
    std::string result(text);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

std::string trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

bool token(const std::string& text)
{
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (std::string("!#$%&'*+-.^_`|~").find(c) == std::string::npos) return false;
    }
    return true;
}

struct Request {
    std::string path, query, host, origin, site, mode, dest;
};

int parseRequest(const std::string& raw, Request& result)
{
    const size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos || raw.size() != end + 4) return 400;
    const size_t firstEnd = raw.find("\r\n");
    const std::string line = raw.substr(0, firstEnd);
    const size_t space = line.find(' '), lastSpace = line.rfind(' ');
    if (space == std::string::npos || space == lastSpace ||
        (line.substr(lastSpace + 1) != "HTTP/1.1" && line.substr(lastSpace + 1) != "HTTP/1.0")) return 400;
    if (line.substr(0, space) != "GET") return 405;
    const std::string target = line.substr(space + 1, lastSpace - space - 1);
    if (target.empty() || target[0] != '/' || target.find('#') != std::string::npos) return 400;
    for (unsigned char c : target) if (c <= 32 || c >= 127) return 400;
    const size_t question = target.find('?');
    result.path = target.substr(0, question);
    if (question != std::string::npos) result.query = target.substr(question + 1);
    std::array<std::string, 64> names;
    size_t count = 0;
    for (size_t start = firstEnd + 2; start < end; ) {
        const size_t next = raw.find("\r\n", start);
        if (next == std::string::npos || next > end || count == names.size()) return 400;
        const std::string header = raw.substr(start, next - start);
        const size_t colon = header.find(':');
        if (colon == std::string::npos || !token(header.substr(0, colon))) return 400;
        const std::string name = lower(header.substr(0, colon));
        for (size_t i = 0; i < count; ++i) if (name == names[i]) return 400;
        names[count++] = name;
        const std::string value = trim(header.substr(colon + 1));
        for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return 400;
        if (name == "host") result.host = value;
        else if (name == "origin") result.origin = value;
        else if (name == "sec-fetch-site") result.site = value;
        else if (name == "sec-fetch-mode") result.mode = value;
        else if (name == "sec-fetch-dest") result.dest = value;
        else if (name == "transfer-encoding" || (name == "content-length" && value != "0")) return 400;
        start = next + 2;
    }
    return result.host == kHost ? 200 : 403;
}

int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decode(const std::string& encoded, std::string& result)
{
    for (size_t i = 0; i < encoded.size(); ++i) {
        unsigned char c = encoded[i];
        if (c == '%') {
            if (i + 2 >= encoded.size() || hex(encoded[i + 1]) < 0 || hex(encoded[i + 2]) < 0) return false;
            c = static_cast<unsigned char>(hex(encoded[i + 1]) * 16 + hex(encoded[i + 2]));
            i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c >= 127) return false;
        result += static_cast<char>(c);
    }
    return true;
}

bool validUuid(const std::string& value)
{
    if (value.size() != 36 || value[14] != '4') return false;
    const char variant = value[19];
    if (variant != '8' && variant != '9' && variant != 'a' && variant != 'A' && variant != 'b' && variant != 'B') return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (hex(value[i]) < 0) return false;
    }
    return true;
}

bool validRelay(const std::string& query)
{
    std::string origin, nonce;
    bool hasOrigin = false, hasNonce = false;
    for (size_t start = 0; start < query.size(); ) {
        const size_t end = query.find('&', start);
        const std::string pair = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t equal = pair.find('=');
        if (equal == std::string::npos) return false;
        std::string name, value;
        if (!decode(pair.substr(0, equal), name) || !decode(pair.substr(equal + 1), value)) return false;
        if (name == "origin") { if (hasOrigin) return false; hasOrigin = true; origin = value; }
        else if (name == "nonce") { if (hasNonce) return false; hasNonce = true; nonce = value; }
        else return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return hasOrigin && hasNonce && (origin == kOrigin || origin == kExtensionOrigin) && validUuid(nonce);
}

void handleRequest(Client& client, LONG command)
{
    Request request;
    const int code = parseRequest(client.request, request);
    std::string().swap(client.request);
    if (code != 200) { reply(client, code, "Invalid local request"); return; }
    const bool crossSite = (!request.site.empty() && request.site != "same-origin" && request.site != "none") ||
        (!request.origin.empty() && request.origin != kOrigin);
    const bool navigation = request.mode == "navigate" && request.dest == "document";
    if (request.path == "/relay.html") {
        if (!validRelay(request.query)) { reply(client, 400, "Invalid relay destination"); return; }
        if (crossSite && !navigation) { reply(client, 403, "Relay requires a top-level navigation"); return; }
    } else if (request.path == "/video_overlay.html") {
        if (crossSite && !navigation) { reply(client, 403, "Viewer requires a top-level navigation"); return; }
    } else if (crossSite) { reply(client, 403, "Cross-site access is not supported"); return; }
    if (request.path == "/events") {
        client.output = responseHeaders(200, "text/event-stream; charset=utf-8", 0, true) +
            eventPayload(envelope(command));
        client.sent = 0;
        client.queuedAt = GetTickCount64();
        client.nextEvent = client.queuedAt + 1000;
        client.headersDone = client.events = true;
    } else if (request.path == "/snapshot") {
        reply(client, 200, envelope(command), "application/json; charset=utf-8");
    } else {
        const char* data = nullptr;
        const char* mime = nullptr;
        size_t size = 0;
        if (!localBridgeAsset(request.path, &data, &size, &mime) || !data || !mime || size > kMaxFrame) {
            reply(client, 404, "Not found");
        } else reply(client, 200, data, size, mime);
    }
}

int beginListening(Network& network)
{
    network.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (network.listener == INVALID_SOCKET) return WSAGetLastError();
    const BOOL exclusive = TRUE;
    if (setsockopt(network.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) return WSAGetLastError();
    u_long nonblocking = 1;
    if (ioctlsocket(network.listener, FIONBIO, &nonblocking) == SOCKET_ERROR) return WSAGetLastError();
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(8765);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(network.listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        return WSAGetLastError();
    if (listen(network.listener, static_cast<int>(kMaxClients)) == SOCKET_ERROR) return WSAGetLastError();
    return 0;
}

void acceptClients(Network& network)
{
    // Bound each iteration even when another local process floods the listener.
    for (size_t i = 0; i < kMaxClients; ++i) {
        sockaddr_in peer = {};
        int size = sizeof(peer);
        const SOCKET socket = accept(network.listener, reinterpret_cast<sockaddr*>(&peer), &size);
        if (socket == INVALID_SOCKET) return;
        Client* available = nullptr;
        for (auto& client : network.clients) if (client.socket == INVALID_SOCKET) { available = &client; break; }
        if (!available || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { closesocket(socket); continue; }
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) { closesocket(socket); continue; }
        const BOOL noDelay = TRUE;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        available->socket = socket;
        available->acceptedAt = GetTickCount64();
    }
}

void readClient(Client& client, LONG command)
{
    char buffer[4096];
    const int count = recv(client.socket, buffer, sizeof(buffer), 0);
    if (count == 0) {
        // A client may finish its write half after sending a complete GET while
        // continuing to read the response. Do not truncate that response.
        if (client.headersDone) client.readClosed = true;
        else client.close();
        return;
    }
    if (count == SOCKET_ERROR) { if (WSAGetLastError() != WSAEWOULDBLOCK) client.close(); return; }
    // One HTTP request per connection; no bodies, upgrades or pipelined requests.
    if (client.headersDone) { client.close(); return; }
    if (client.request.size() + count > kMaxHeaders) { reply(client, 431, "Request headers too large"); return; }
    client.request.append(buffer, count);
    if (client.request.find("\r\n\r\n") != std::string::npos) handleRequest(client, command);
}

void writeClient(Client& client)
{
    for (unsigned chunk = 0; chunk < 4 && client.sent < client.output.size(); ++chunk) {
        const size_t remaining = (std::min)(size_t(65536), client.output.size() - client.sent);
        const int count = send(client.socket, client.output.data() + client.sent, static_cast<int>(remaining), 0);
        if (count == SOCKET_ERROR) { if (WSAGetLastError() != WSAEWOULDBLOCK) client.close(); return; }
        if (count <= 0) { client.close(); return; }
        client.sent += count;
    }
    if (client.sent == client.output.size()) {
        std::string().swap(client.output);
        client.sent = 0;
        if (!client.events) client.close();
    }
}

int poll(Network& network, LONG command)
{
    fd_set readable, writable, exceptional;
    FD_ZERO(&readable); FD_ZERO(&writable); FD_ZERO(&exceptional);
    FD_SET(network.listener, &readable);
    FD_SET(network.listener, &exceptional);
    const ULONGLONG now = GetTickCount64();
    for (auto& client : network.clients) {
        if (client.socket == INVALID_SOCKET) continue;
        if ((!client.headersDone && now - client.acceptedAt >= kClientTimeoutMs) ||
            (!client.output.empty() && now - client.queuedAt >= kSendTimeoutMs)) { client.close(); continue; }
        if (client.events && client.output.empty() && now >= client.nextEvent) {
            client.output = eventPayload(envelope(command));
            client.queuedAt = now;
            client.nextEvent = now + 1000;
        }
        if (!client.readClosed) FD_SET(client.socket, &readable);
        FD_SET(client.socket, &exceptional);
        if (!client.output.empty()) FD_SET(client.socket, &writable);
    }
    timeval timeout = {};
    const int selected = select(0, &readable, &writable, &exceptional, &timeout);
    if (selected == SOCKET_ERROR) return WSAGetLastError();
    if (FD_ISSET(network.listener, &exceptional)) return WSAECONNABORTED;
    if (FD_ISSET(network.listener, &readable)) acceptClients(network);
    for (auto& client : network.clients) {
        if (client.socket == INVALID_SOCKET) continue;
        if (FD_ISSET(client.socket, &exceptional)) { client.close(); continue; }
        if (FD_ISSET(client.socket, &readable)) readClient(client, command);
        if (client.socket != INVALID_SOCKET && FD_ISSET(client.socket, &writable)) writeClient(client);
    }
    return 0;
}

DWORD WINAPI worker(void*)
{
    Network network;
    bool winsockActive = false;
    LONG attempted = -1;
    while (!atomicRead(&g_shutdown)) {
        const LONG command = atomicRead(&g_command);
        try {
            if (command != attempted) {
                network.close();
                attempted = command;
                if (!(command & 1)) setState(command, LocalBridgeState::Stopped);
                else {
                    setState(command, LocalBridgeState::Starting);
                    int error = 0;
                    if (!winsockActive) {
                        WSADATA winsock;
                        error = WSAStartup(MAKEWORD(2, 2), &winsock);
                        winsockActive = error == 0;
                    }
                    if (!error) error = beginListening(network);
                    if (error) {
                        network.close();
                        setState(command, error == WSAEADDRINUSE || error == WSAEACCES ? LocalBridgeState::PortBusy : LocalBridgeState::Failed, error);
                    } else setState(command, LocalBridgeState::Listening);
                }
            }
            if (network.listener != INVALID_SOCKET) {
                const int error = poll(network, command);
                if (error) { network.close(); setState(command, LocalBridgeState::Failed, error); }
            }
        } catch (...) {
            // Fail closed, retaining the worker so a later explicit off/on can
            // recover. No implicit retries compete with another game instance.
            network.close();
            setState(command, LocalBridgeState::Failed, ERROR_UNHANDLED_EXCEPTION);
        }
        // Sockets are nonblocking. This wait runs only on our background
        // thread; enable/disable/publish wake it without awaiting any I/O.
        WaitForSingleObject(wakeEvent(), network.listener == INVALID_SOCKET ? INFINITE : 20);
    }
    network.close();
    setState(atomicRead(&g_command), LocalBridgeState::Stopped);
    if (winsockActive) WSACleanup();
    return 0;
}

BOOL CALLBACK startWorker(PINIT_ONCE, PVOID, PVOID*)
{
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) { InterlockedExchange(&g_error, GetLastError()); return FALSE; }
    InterlockedExchangePointer(&g_wake, event);
    const HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (!thread) {
        const DWORD error = GetLastError();
        InterlockedExchangePointer(&g_wake, nullptr);
        CloseHandle(event);
        InterlockedExchange(&g_error, error);
        return FALSE;
    }
    CloseHandle(thread);
    // The event is deliberately process-lifetime storage. Shutdown never closes
    // a handle that an in-flight game-thread publish could still be signalling.
    return TRUE;
}

}

void localBridgeSetEnabled(bool enabled)
{
    if (atomicRead(&g_shutdown)) return;
    LONG current = atomicRead(&g_command), next;
    do {
        if (bool(current & 1) == enabled) return;
        next = ((current + 2) & ~1L) | (enabled ? 1 : 0);
        const LONG actual = InterlockedCompareExchange(&g_command, next, current);
        if (actual == current) break;
        current = actual;
    } while (true);
    if (enabled) {
        setState(next, LocalBridgeState::Starting);
        if (!InitOnceExecuteOnce(&g_startOnce, startWorker, nullptr, nullptr))
            setState(next, LocalBridgeState::Failed, atomicRead(&g_error));
    }
    wakeWorker();
}

void localBridgePublish(const std::string& json, unsigned long long capturedAt, bool active)
{
    const LONG command = atomicRead(&g_command);
    if (!(command & 1) || atomicRead(&g_shutdown)) return;
    // End-of-battle invalidation must survive both allocation failure and a
    // contended mailbox. An older active publication cannot resurrect afterward.
    if (!active) {
        InterlockedExchange(&g_idleCommand, command);
        InterlockedIncrement(&g_invalidation);
        wakeWorker();
    }
    const LONG invalidation = atomicRead(&g_invalidation);
    try {
        std::shared_ptr<Frame> next = std::make_shared<Frame>();
        // Frames originate from the plugin's own serializer, never a file or
        // network input. Whitespace (including pretty-print newlines) is valid.
        const size_t first = json.find_first_not_of(" \t\r\n"), last = json.find_last_not_of(" \t\r\n");
        if (json.size() <= kMaxFrame && first != std::string::npos && json[first] == '{' && json[last] == '}') next->json = json;
        next->capturedAt = capturedAt;
        next->active = active;
        next->command = command;
        next->invalidation = invalidation;
        // A slow reader must never suspend the UI. Coalescing may skip one
        // publication if the worker is currently copying its shared pointer.
        if (!TryAcquireSRWLockExclusive(&g_frameLock)) return;
        std::shared_ptr<const Frame> previous = std::move(g_frame);
        g_frame = std::move(next);
        ReleaseSRWLockExclusive(&g_frameLock);
        // Destruction/copying of JSON happens outside the shared lock.
        wakeWorker();
    } catch (...) {
        // Do not allow allocation failures to escape through a game callback.
    }
}

LocalBridgeState localBridgeState() { return static_cast<LocalBridgeState>(atomicRead(&g_state)); }
int localBridgeError() { return static_cast<int>(atomicRead(&g_error)); }

void localBridgeShutdown()
{
    localBridgeSetEnabled(false);
    InterlockedExchange(&g_shutdown, 1);
    wakeWorker();
}

}
