/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Relay client. See testdrv/packetlogicbridge.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "testdrv/lobbychatreporter.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/worldreporter.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

namespace hooks {
namespace testdrv {
namespace bridge {

namespace {

// Pipe name + protocol version (mirror the node relay).
constexpr const wchar_t* kDefaultPipeName = L"\\\\.\\pipe\\d2lobby.packetlogic";
constexpr uint32_t kProtocolVersion = 1;

// Public protocol opcodes only. Control opcodes the bridge does not own are
// handed to the registered command callback by number, so this file carries no
// knowledge of any consumer's private commands.
enum class Op : uint16_t
{
    Hello = 0x0001,
    HelloAck = 0x0002,
    InvokeButton = 0x0300,   // <- dispatcher: click a button (handled by the autonav executor)
    SetSelection = 0x0301,   // <- dispatcher: set a listbox selection (autonav executor)
    SetSpin = 0x0302,        // <- dispatcher: set a spin-button option (autonav executor)
    SetEditText = 0x0303,    // <- dispatcher: set an edit-box's text (autonav executor)
    CommandResult = 0x0304,  // -> relay: outcome of a dispatcher command (u32 seq | u8 found)
    MoveStack = 0x0305,      // <- dispatcher: move a stack to a tile (autonav -> worldactions); forwarded by number
    InvokeToggle = 0x0306,   // <- dispatcher: flip a toggle button (e.g. TOG_AUTOBATTLE); forwarded by number
    HireMerc = 0x0307,       // <- dispatcher: buy a merc from a camp into a stack (CSiteBuyUnitMsg, autonav -> worldactions); forwarded by number
    MoveGroupUnit = 0x0308,  // <- dispatcher: move/swap a unit between formation slots (CStackSwapUnitMsg); forwarded by number
    DismissUnit = 0x0309,    // <- dispatcher: dismiss a non-leader unit from a stack (CStackDismissUnitMsg); forwarded by number

    UiSnapshot = 0x0410,     // -> relay: current dialog + all its widgets with state (JSON)
    WorldSnapshot = 0x0411,  // -> relay: players' resources + all map stacks (JSON, world reporter)
    LobbyChat = 0x0412,      // -> relay: recent custom-lobby chat (JSON, lobby chat reporter)
};

std::atomic<HANDLE> g_pipe{INVALID_HANDLE_VALUE};
std::atomic<SOCKET> g_sock{INVALID_SOCKET};
std::atomic<bool> g_running{false};
std::thread g_thread;
HMODULE g_self = nullptr;
CommandCallback g_command_cb = nullptr;

struct SendItem
{
    Op op;
    std::vector<uint8_t> payload;
};
std::mutex g_send_mutex;
std::deque<SendItem> g_send_queue;
constexpr size_t kSendQueueMax = 256;

bool has_connection()
{
    return g_pipe.load() != INVALID_HANDLE_VALUE || g_sock.load() != INVALID_SOCKET;
}

void clear_send_queue()
{
    std::lock_guard<std::mutex> lock(g_send_mutex);
    g_send_queue.clear();
}

void close_connection()
{
    HANDLE pipe = g_pipe.exchange(INVALID_HANDLE_VALUE);
    if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, nullptr);
        CloseHandle(pipe);
    }

    SOCKET socket = g_sock.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }

    // Command results produced for a relay which disappeared are no longer
    // correlated after the next Hello. Current snapshots are resent instead.
    clear_send_queue();
}

bool write_message(Op op, const void* payload, uint32_t payload_size)
{
    uint32_t length = 4 + payload_size; // opcode + flags + payload
    uint8_t header[8];
    *(uint32_t*)(header + 0) = length;
    *(uint16_t*)(header + 4) = static_cast<uint16_t>(op);
    *(uint16_t*)(header + 6) = 0; // flags

    SOCKET s = g_sock.load();
    if (s != INVALID_SOCKET) {
        auto sendAll = [s](const char* data, int size) {
            int sent = 0;
            while (sent < size) {
                const int count = ::send(s, data + sent, size - sent, 0);
                if (count == SOCKET_ERROR) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) {
                        Sleep(1);
                        continue;
                    }
                    return false;
                }
                if (count == 0)
                    return false;
                sent += count;
            }
            return true;
        };
        return sendAll(reinterpret_cast<const char*>(header), sizeof(header))
               && (payload_size == 0
                   || sendAll(reinterpret_cast<const char*>(payload), payload_size));
    }

    HANDLE h = g_pipe.load();
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    if (!WriteFile(h, header, sizeof(header), &written, nullptr) || written != sizeof(header))
        return false;
    if (payload_size > 0) {
        if (!WriteFile(h, payload, payload_size, &written, nullptr) || written != payload_size)
            return false;
    }
    return true;
}

bool read_message(Op& out_op, std::vector<uint8_t>& out_payload)
{
    uint8_t header[4];
    SOCKET s = g_sock.load();
    if (s != INVALID_SOCKET) {
        auto recvAll = [s](char* data, int size) {
            int received = 0;
            while (received < size) {
                const int count = ::recv(s, data + received, size - received, 0);
                if (count == SOCKET_ERROR) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) {
                        Sleep(1);
                        continue;
                    }
                    return false;
                }
                if (count == 0)
                    return false;
                received += count;
            }
            return true;
        };
        if (!recvAll(reinterpret_cast<char*>(header), sizeof(header)))
            return false;
        const uint32_t length = *reinterpret_cast<uint32_t*>(header);
        if (length < 4 || length > 16 * 1024 * 1024)
            return false;
        std::vector<uint8_t> buffer(length);
        if (!recvAll(reinterpret_cast<char*>(buffer.data()), length))
            return false;
        out_op = static_cast<Op>(*reinterpret_cast<uint16_t*>(buffer.data()));
        out_payload.assign(buffer.begin() + 4, buffer.end());
        return true;
    }

    HANDLE h = g_pipe.load();
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD got = 0;
    if (!ReadFile(h, header, 4, &got, nullptr) || got != 4)
        return false;
    uint32_t length = *(uint32_t*)header;
    if (length < 4 || length > 16 * 1024 * 1024)
        return false;
    std::vector<uint8_t> buf(length);
    DWORD read = 0;
    while (read < length) {
        DWORD n = 0;
        if (!ReadFile(h, buf.data() + read, length - read, &n, nullptr) || n == 0)
            return false;
        read += n;
    }
    out_op = static_cast<Op>(*(uint16_t*)(buf.data() + 0));
    out_payload.assign(buf.begin() + 4, buf.end());
    return true;
}

bool enqueue(Op opcode, const void* payload, uint32_t size)
{
    if (!g_running.load())
        return false;
    if (g_pipe.load() == INVALID_HANDLE_VALUE && g_sock.load() == INVALID_SOCKET)
        return false;
    SendItem item;
    item.op = opcode;
    if (size > 0 && payload)
        item.payload.assign((const uint8_t*)payload, (const uint8_t*)payload + size);
    std::lock_guard<std::mutex> lk(g_send_mutex);
    if (g_send_queue.size() >= kSendQueueMax)
        g_send_queue.pop_front(); // drop oldest, preserve liveness
    g_send_queue.push_back(std::move(item));
    return true;
}

std::vector<uint8_t> build_hello_payload()
{
    char module_path[MAX_PATH];
    GetModuleFileNameA(g_self, module_path, sizeof(module_path));
    size_t mod_len = strlen(module_path);
    // Role (host/join/...) lets the relay tag each of the two instances. Appended
    // after the module path so an older relay that ignores it still parses Hello.
    char role[32]{};
    GetEnvironmentVariableA("D2TESTDRV_ROLE", role, sizeof(role));
    size_t role_len = strlen(role);
    std::vector<uint8_t> p(12 + mod_len + 4 + role_len);
    *(uint32_t*)(p.data() + 0) = kProtocolVersion;
    *(uint32_t*)(p.data() + 4) = GetCurrentProcessId();
    *(uint32_t*)(p.data() + 8) = static_cast<uint32_t>(mod_len);
    memcpy(p.data() + 12, module_path, mod_len);
    *(uint32_t*)(p.data() + 12 + mod_len) = static_cast<uint32_t>(role_len);
    if (role_len)
        memcpy(p.data() + 12 + mod_len + 4, role, role_len);
    return p;
}

void handle_incoming(Op op, const std::vector<uint8_t>& payload)
{
    // Commands are interpreted only by the UI-thread executor. The bridge owns
    // transport and framing, not a second command-dispatch layer.
    if (g_command_cb) {
        g_command_cb(static_cast<uint16_t>(op), payload.data(), (uint32_t)payload.size());
    } else {
        spdlog::info("[testdrv] bridge op 0x{:04X} ({:d} bytes), no command handler",
                     (unsigned)op, (unsigned)payload.size());
    }
}

void bridge_thread_main()
{
    bool wsaStarted = false;
    struct ThreadExit
    {
        bool& wsaStarted;
        ~ThreadExit()
        {
            close_connection();
            if (wsaStarted)
                WSACleanup();
            g_running.store(false);
            spdlog::info("[testdrv] bridge thread exiting");
        }
    } threadExit{wsaStarted};

    Sleep(500); // let the loader settle before chatting on a pipe

    char tcpHost[256]{};
    const DWORD hostLength =
        GetEnvironmentVariableA("D2TESTDRV_BRIDGE_TCP_HOST", tcpHost, sizeof(tcpHost));
    if (hostLength >= sizeof(tcpHost)) {
        spdlog::warn("[testdrv] D2TESTDRV_BRIDGE_TCP_HOST is too long, bridge disabled");
        return;
    }
    const bool useTcp = hostLength > 0;
    sockaddr_in tcpAddress{};
    wchar_t pipeName[256]{};

    if (useTcp) {
        char portText[16]{};
        const DWORD portLength =
            GetEnvironmentVariableA("D2TESTDRV_BRIDGE_TCP_PORT", portText, sizeof(portText));
        if (portLength >= sizeof(portText)) {
            spdlog::warn("[testdrv] D2TESTDRV_BRIDGE_TCP_PORT is too long, bridge disabled");
            return;
        }

        char* portEnd = nullptr;
        const long port = portText[0] ? strtol(portText, &portEnd, 10) : 47626;
        if (port < 1 || port > 65535 || (portText[0] && (!portEnd || *portEnd != '\0'))) {
            spdlog::warn("[testdrv] D2TESTDRV_BRIDGE_TCP_PORT is invalid, bridge disabled");
            return;
        }

        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            spdlog::warn("[testdrv] bridge WSAStartup failed");
            return;
        }
        wsaStarted = true;

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* resolved = nullptr;
        const std::string service = std::to_string(port);
        const int resolveResult = getaddrinfo(tcpHost, service.c_str(), &hints, &resolved);
        if (resolveResult != 0 || !resolved || !resolved->ai_addr) {
            spdlog::warn("[testdrv] bridge cannot resolve TCP host '{}' (error={:d})", tcpHost,
                         resolveResult);
            if (resolved)
                freeaddrinfo(resolved);
            return;
        }
        tcpAddress = *reinterpret_cast<const sockaddr_in*>(resolved->ai_addr);
        freeaddrinfo(resolved);
    } else {
        const DWORD pipeLength = GetEnvironmentVariableW(
            L"D2TESTDRV_PIPE_NAME", pipeName, static_cast<DWORD>(_countof(pipeName)));
        if (pipeLength >= _countof(pipeName)) {
            spdlog::warn("[testdrv] D2TESTDRV_PIPE_NAME is too long, bridge disabled");
            return;
        }
        if (!pipeLength)
            lstrcpynW(pipeName, kDefaultPipeName, static_cast<int>(_countof(pipeName)));
    }

    auto connectOnce = [&]() -> bool {
        if (useTcp) {
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_SOCKET)
                return false;
            DWORD timeoutMs = 2000;
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
            if (::connect(socket, reinterpret_cast<sockaddr*>(&tcpAddress), sizeof(tcpAddress))
                == SOCKET_ERROR) {
                closesocket(socket);
                return false;
            }
            if (!g_running.load()) {
                closesocket(socket);
                return false;
            }
            u_long nonblocking = 1;
            if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                closesocket(socket);
                return false;
            }
            g_sock.store(socket);
            spdlog::info("[testdrv] bridge connected via TCP (socket={:d})", (int)socket);
            return true;
        }

        HANDLE pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            return false;
        if (!g_running.load()) {
            CloseHandle(pipe);
            return false;
        }
        g_pipe.store(pipe);
        spdlog::info("[testdrv] bridge connected to relay pipe");
        return true;
    };

    unsigned connectAttempts = 0;
    unsigned handshakeFailures = 0;
    while (g_running.load()) {
        if (!connectOnce()) {
            ++connectAttempts;
            if (connectAttempts == 20
                || (connectAttempts > 20 && connectAttempts % 120 == 0)) {
                spdlog::info("[testdrv] bridge waiting for relay (attempt {:d})", connectAttempts);
            }
            Sleep(500);
            continue;
        }
        connectAttempts = 0;

        auto hello = build_hello_payload();
        Op op{};
        std::vector<uint8_t> payload;
        const bool helloOk = write_message(Op::Hello, hello.data(), (uint32_t)hello.size())
                             && read_message(op, payload) && op == Op::HelloAck;
        if (!helloOk) {
            ++handshakeFailures;
            if (handshakeFailures == 1 || handshakeFailures % 20 == 0) {
                spdlog::warn("[testdrv] bridge handshake failed; reconnecting (failure {:d})",
                             handshakeFailures);
            }
            close_connection();
            Sleep(500);
            continue;
        }
        handshakeFailures = 0;
        spdlog::info("[testdrv] bridge HelloAck ok ({:d} bytes)", (unsigned)payload.size());

        uint32_t lastUiEpoch = 0;
        uint32_t lastWorldEpoch = 0;
        uint32_t lastChatEpoch = 0;
        bool connectionOk = true;
        while (g_running.load() && has_connection() && connectionOk) {
            // Epochs restart at zero after each Hello so a replacement relay receives
            // the complete current UI/world/chat state without replaying commands.
            {
                std::string snap;
                uint32_t epoch = 0;
                if (uistatereporter::copyUiSnapshot(snap, epoch) && epoch != lastUiEpoch) {
                    lastUiEpoch = epoch;
                    connectionOk =
                        write_message(Op::UiSnapshot, snap.data(), (uint32_t)snap.size());
                }
            }
            {
                std::string snap;
                uint32_t epoch = 0;
                if (connectionOk && worldreporter::copyWorldSnapshot(snap, epoch)
                    && epoch != lastWorldEpoch) {
                    lastWorldEpoch = epoch;
                    connectionOk =
                        write_message(Op::WorldSnapshot, snap.data(), (uint32_t)snap.size());
                }
            }
            {
                std::string snap;
                uint32_t epoch = 0;
                if (connectionOk && lobbychatreporter::copyChatLog(snap, epoch)
                    && epoch != lastChatEpoch) {
                    lastChatEpoch = epoch;
                    connectionOk =
                        write_message(Op::LobbyChat, snap.data(), (uint32_t)snap.size());
                }
            }
            if (!connectionOk)
                break;

            for (;;) {
                SendItem item;
                bool has = false;
                {
                    std::lock_guard<std::mutex> lock(g_send_mutex);
                    if (!g_send_queue.empty()) {
                        item = std::move(g_send_queue.front());
                        g_send_queue.pop_front();
                        has = true;
                    }
                }
                if (!has)
                    break;
                if (!write_message(item.op, item.payload.data(), (uint32_t)item.payload.size())) {
                    connectionOk = false;
                    break;
                }
            }
            if (!connectionOk)
                break;

            bool hasData = false;
            SOCKET socket = g_sock.load();
            if (socket != INVALID_SOCKET) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(socket, &readable);
                timeval noWait{};
                const int ready = select(0, &readable, nullptr, nullptr, &noWait);
                if (ready == SOCKET_ERROR) {
                    connectionOk = false;
                    break;
                }
                if (ready > 0) {
                    char probe = 0;
                    const int peeked = recv(socket, &probe, 1, MSG_PEEK);
                    if (peeked == 0
                        || (peeked == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
                        connectionOk = false;
                        break;
                    }
                    u_long available = 0;
                    if (ioctlsocket(socket, FIONREAD, &available) == SOCKET_ERROR) {
                        connectionOk = false;
                        break;
                    }
                    hasData = available >= 4;
                }
            } else {
                DWORD available = 0;
                HANDLE pipe = g_pipe.load();
                if (pipe == INVALID_HANDLE_VALUE
                    || !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                    connectionOk = false;
                    break;
                }
                hasData = available >= 4;
            }

            if (hasData) {
                if (!read_message(op, payload)) {
                    connectionOk = false;
                    break;
                }
                handle_incoming(op, payload);
                continue;
            }
            Sleep(5);
        }

        if (g_running.load())
            spdlog::warn("[testdrv] relay connection lost; retrying");
        close_connection();
        if (g_running.load())
            Sleep(500);
    }
}

} // namespace

void setCommandCallback(CommandCallback cb)
{
    g_command_cb = cb;
}

bool start(HMODULE selfModule)
{
    if (g_running.exchange(true))
        return false; // already started
    g_self = selfModule;
    g_thread = std::thread(bridge_thread_main);
    g_thread.detach();
    return true;
}

void send_command_result(std::uint32_t seq, bool found)
{
    uint8_t p[5];
    *(uint32_t*)(p + 0) = seq;
    p[4] = found ? 1 : 0;
    enqueue(Op::CommandResult, p, sizeof(p)); // bridge thread writes it; non-blocking on the UI thread
}


} // namespace bridge
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
