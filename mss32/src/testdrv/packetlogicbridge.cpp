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

#include "testdrv/packetlogicbridge.h"
#include "testdrv/nettracehooks.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/worldreporter.h"
#include <atomic>
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
constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\d2lobby.packetlogic";
constexpr uint32_t kProtocolVersion = 1;

// Public protocol opcodes only. Control opcodes the bridge does not own are
// handed to the registered command callback by number, so this file carries no
// knowledge of any consumer's private commands.
enum class Op : uint16_t
{
    Hello = 0x0001,
    HelloAck = 0x0002,
    Goodbye = 0x0003,
    ConfigurePatches = 0x0004,
    LocalPlayerHandle = 0x0007,
    PacketTrace = 0x0202,    // TX
    PacketTraceRx = 0x0203,  // RX
    InvokeButton = 0x0300,   // <- dispatcher: click a button (handled by the autonav executor)
    SetSelection = 0x0301,   // <- dispatcher: set a listbox selection (autonav executor)
    SetSpin = 0x0302,        // <- dispatcher: set a spin-button option (autonav executor)
    SetEditText = 0x0303,    // <- dispatcher: set an edit-box's text (autonav executor)
    CommandResult = 0x0304,  // -> relay: outcome of a dispatcher command (u32 seq | u8 found)
    UiSnapshot = 0x0410,     // -> relay: current dialog + all its widgets with state (JSON)
    WorldSnapshot = 0x0411,  // -> relay: players' resources + all map stacks (JSON, world reporter)
    Log = 0xFF00,
};

std::atomic<HANDLE> g_pipe{INVALID_HANDLE_VALUE};
std::atomic<SOCKET> g_sock{INVALID_SOCKET};
std::atomic<bool> g_running{false};
std::mutex g_write_mutex;
std::mutex g_sock_write_mutex;
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

bool write_message(Op op, const void* payload, uint32_t payload_size, bool non_blocking = false)
{
    uint32_t length = 4 + payload_size; // opcode + flags + payload
    uint8_t header[8];
    *(uint32_t*)(header + 0) = length;
    *(uint16_t*)(header + 4) = static_cast<uint16_t>(op);
    *(uint16_t*)(header + 6) = 0; // flags

    SOCKET s = g_sock.load();
    if (s != INVALID_SOCKET) {
        std::unique_lock<std::mutex> lock(g_sock_write_mutex, std::defer_lock);
        if (non_blocking) {
            if (!lock.try_lock())
                return false;
        } else {
            lock.lock();
        }
        auto send_all = [&](const char* p, int total) {
            int sent = 0;
            while (sent < total) {
                int n = ::send(s, p + sent, total - sent, 0);
                if (n == SOCKET_ERROR) {
                    if (WSAGetLastError() == WSAEWOULDBLOCK) {
                        if (non_blocking)
                            return false;
                        Sleep(1);
                        continue;
                    }
                    return false;
                }
                if (n == 0)
                    return false;
                sent += n;
            }
            return true;
        };
        if (!send_all((char*)header, (int)sizeof(header)))
            return false;
        if (payload_size > 0 && !send_all((char*)payload, (int)payload_size))
            return false;
        return true;
    }

    HANDLE h = g_pipe.load();
    if (h == INVALID_HANDLE_VALUE)
        return false;

    std::unique_lock<std::mutex> lock(g_write_mutex, std::defer_lock);
    if (non_blocking) {
        if (!lock.try_lock())
            return false;
    } else {
        lock.lock();
    }
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
        int got = 0;
        while (got < 4) {
            int n = ::recv(s, (char*)header + got, 4 - got, 0);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    Sleep(1);
                    continue;
                }
                return false;
            }
            if (n == 0)
                return false;
            got += n;
        }
        uint32_t length = *(uint32_t*)header;
        if (length < 4 || length > 16 * 1024 * 1024)
            return false;
        std::vector<uint8_t> buf(length);
        int read = 0, need = (int)length;
        while (read < need) {
            int n = ::recv(s, (char*)buf.data() + read, need - read, 0);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    Sleep(1);
                    continue;
                }
                return false;
            }
            if (n == 0)
                return false;
            read += n;
        }
        out_op = static_cast<Op>(*(uint16_t*)(buf.data() + 0));
        out_payload.assign(buf.begin() + 4, buf.end());
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

// nettracehooks RX-trace sink. Frame: u32 self, u32 sender, u32 size, byte[size].
// Runs on the game/worker thread, must only enqueue (non-blocking).
void on_rx_trace(void* self, int sender, const uint8_t* payload, uint32_t size)
{
    if (size > 0x10000)
        return;
    std::vector<uint8_t> frame(12 + size);
    *(uint32_t*)(frame.data() + 0) = (uint32_t)(uintptr_t)self;
    *(uint32_t*)(frame.data() + 4) = (uint32_t)sender;
    *(uint32_t*)(frame.data() + 8) = size;
    if (size)
        memcpy(frame.data() + 12, payload, size);
    enqueue(Op::PacketTraceRx, frame.data(), (uint32_t)frame.size());
}

// nettracehooks TX-trace sink. Frame: u32 self, u32 idTo, u32 size, byte[size].
void on_tx_trace(void* self, uint32_t idTo, const uint8_t* message, uint32_t size)
{
    if (size > 0x10000)
        return;
    std::vector<uint8_t> frame(12 + size);
    *(uint32_t*)(frame.data() + 0) = (uint32_t)(uintptr_t)self;
    *(uint32_t*)(frame.data() + 4) = idTo;
    *(uint32_t*)(frame.data() + 8) = size;
    if (size)
        memcpy(frame.data() + 12, message, size);
    enqueue(Op::PacketTrace, frame.data(), (uint32_t)frame.size());
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
    switch (op) {
    case Op::ConfigurePatches:
        if (payload.size() >= 4) {
            uint32_t flags = *(uint32_t*)payload.data();
            spdlog::info("[testdrv] bridge ConfigurePatches flags=0x{:08X} (noted)", flags);
        }
        break;
    default:
        // InvokeButton and any consumer-private opcode are handed to the command
        // callback if one is registered; otherwise just logged.
        if (g_command_cb) {
            g_command_cb(static_cast<uint16_t>(op), payload.data(), (uint32_t)payload.size());
        } else {
            spdlog::info("[testdrv] bridge op 0x{:04X} ({:d} bytes), no command handler",
                         (unsigned)op, (unsigned)payload.size());
        }
        break;
    }
}

void bridge_thread_main()
{
    Sleep(500); // let the loader settle before chatting on a pipe

    char tcp_host[256]{};
    DWORD host_len = GetEnvironmentVariableA("D2TESTDRV_BRIDGE_TCP_HOST", tcp_host, sizeof(tcp_host));
    bool use_tcp = (host_len > 0);

    if (use_tcp) {
        char port_str[16]{};
        GetEnvironmentVariableA("D2TESTDRV_BRIDGE_TCP_PORT", port_str, sizeof(port_str));
        int port = port_str[0] ? atoi(port_str) : 47626;
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            spdlog::warn("[testdrv] bridge WSAStartup failed");
            return;
        }
        SOCKET s = INVALID_SOCKET;
        for (int attempt = 0; attempt < 20 && g_running.load(); attempt++) {
            s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                WSACleanup();
                return;
            }
            DWORD timeout_ms = 2000;
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((u_short)port);
            addr.sin_addr.s_addr = inet_addr(tcp_host);
            if (addr.sin_addr.s_addr == INADDR_NONE)
                addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            if (::connect(s, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR)
                break;
            closesocket(s);
            s = INVALID_SOCKET;
            Sleep(500);
        }
        if (s == INVALID_SOCKET) {
            spdlog::warn("[testdrv] bridge could not connect via TCP, giving up");
            WSACleanup();
            return;
        }
        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        g_sock = s;
        spdlog::info("[testdrv] bridge connected via TCP (socket={:d})", (int)s);
    } else {
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 20 && g_running.load(); attempt++) {
            h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                            nullptr);
            if (h != INVALID_HANDLE_VALUE)
                break;
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY) {
                spdlog::warn("[testdrv] bridge CreateFileW failed lastError={:d}", err);
                return;
            }
            Sleep(500);
        }
        if (h == INVALID_HANDLE_VALUE) {
            spdlog::info("[testdrv] bridge: no relay running, observability off");
            return;
        }
        g_pipe = h;
        spdlog::info("[testdrv] bridge connected to relay pipe");
    }

    auto hello = build_hello_payload();
    if (!write_message(Op::Hello, hello.data(), (uint32_t)hello.size())) {
        spdlog::warn("[testdrv] bridge Hello write failed");
        return;
    }
    Op op;
    std::vector<uint8_t> payload;
    if (!read_message(op, payload) || op != Op::HelloAck) {
        spdlog::warn("[testdrv] bridge HelloAck not received");
        return;
    }
    spdlog::info("[testdrv] bridge HelloAck ok ({:d} bytes)", (unsigned)payload.size());
    const char* msg = "mss32 testdrv bridge alive";
    write_message(Op::Log, msg, (uint32_t)strlen(msg));

    uint32_t last_ui_epoch = 0;
    uint32_t last_world_epoch = 0;
    while (g_running.load()) {
        // 0. Forward the live UI snapshot (current dialog + every widget with its state) to the
        //    relay whenever it changes, so the dispatcher can scan/verify the UI without scraping
        //    logs. The reporter builds it on the UI thread under a lock and bumps the epoch on each
        //    change; we ship only on a new epoch.
        {
            std::string snap;
            uint32_t epoch = 0;
            if (uistatereporter::copyUiSnapshot(snap, epoch) && epoch != last_ui_epoch) {
                last_ui_epoch = epoch;
                write_message(Op::UiSnapshot, snap.data(), (uint32_t)snap.size());
            }
        }

        // 0b. Forward the live WORLD snapshot (players' resources + every map stack) the same way:
        //     the reporter rebuilds it on the UI thread (throttled) and bumps its epoch on change.
        {
            std::string snap;
            uint32_t epoch = 0;
            if (worldreporter::copyWorldSnapshot(snap, epoch) && epoch != last_world_epoch) {
                last_world_epoch = epoch;
                write_message(Op::WorldSnapshot, snap.data(), (uint32_t)snap.size());
            }
        }

        // 1. Drain pending writes from the game-thread enqueue.
        for (;;) {
            SendItem item;
            bool has = false;
            {
                std::lock_guard<std::mutex> lk(g_send_mutex);
                if (!g_send_queue.empty()) {
                    item = std::move(g_send_queue.front());
                    g_send_queue.pop_front();
                    has = true;
                }
            }
            if (!has)
                break;
            write_message(item.op, item.payload.data(), (uint32_t)item.payload.size(), false);
        }

        // 2. Poll for an incoming message.
        bool has_data = false;
        SOCKET s = g_sock.load();
        if (s != INVALID_SOCKET) {
            u_long avail = 0;
            if (ioctlsocket(s, FIONREAD, &avail) == SOCKET_ERROR)
                break;
            has_data = (avail >= 4);
        } else {
            DWORD avail = 0;
            if (!PeekNamedPipe(g_pipe.load(), nullptr, 0, nullptr, &avail, nullptr))
                break;
            has_data = (avail >= 4);
        }
        if (has_data) {
            if (!read_message(op, payload))
                break;
            handle_incoming(op, payload);
            continue;
        }
        Sleep(5);
    }

    write_message(Op::Goodbye, nullptr, 0);
    HANDLE old = g_pipe.exchange(INVALID_HANDLE_VALUE);
    if (old != INVALID_HANDLE_VALUE)
        CloseHandle(old);
    SOCKET sold = g_sock.exchange(INVALID_SOCKET);
    if (sold != INVALID_SOCKET) {
        closesocket(sold);
        WSACleanup();
    }
    spdlog::info("[testdrv] bridge thread exiting");
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
    // Packet-trace forwarding is opt-in (D2TESTDRV_NET_INTERCEPT): on_rx_trace runs on the
    // UI/dispatch thread for EVERY received packet, which during a begin-turn replication
    // burst piles work onto the thread the game is mid-loading on. The dispatcher-driven MP
    // test drives off UI state, not packets, so it leaves this off; logging/secret builds
    // turn it on. on_rx/tx_trace self-gate on g_running, so they go quiet after stop().
    if (testenv::on("D2TESTDRV_NET_INTERCEPT")) {
        nettracehooks::addRxObserver(&on_rx_trace);
        nettracehooks::addTxObserver(&on_tx_trace);
    }
    g_thread = std::thread(bridge_thread_main);
    g_thread.detach();
    return true;
}

void stop()
{
    g_running.store(false); // observers go quiet (they self-gate on g_running)
    HANDLE old = g_pipe.exchange(INVALID_HANDLE_VALUE);
    if (old != INVALID_HANDLE_VALUE) {
        CancelIoEx(old, nullptr);
        CloseHandle(old);
    }
    SOCKET sold = g_sock.exchange(INVALID_SOCKET);
    if (sold != INVALID_SOCKET) {
        closesocket(sold);
        WSACleanup();
    }
}

void send_log(const char* utf8_message)
{
    if (utf8_message)
        write_message(Op::Log, utf8_message, (uint32_t)strlen(utf8_message));
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
