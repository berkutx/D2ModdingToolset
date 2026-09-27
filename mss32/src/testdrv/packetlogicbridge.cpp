/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Relay client. See testdrv/packetlogicbridge.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro no test code is compiled.
 */

#ifdef D2_TESTDRV

#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <winsock2.h>

#include "netmsg.h"
#include "testdrv/packetlogicbridge.h"
#include "testdrv/lobbychatreporter.h"
#include "testdrv/nettracehooks.h"
#include "testdrv/testenv.h"
#include "testdrv/uistatereporter.h"
#include "testdrv/worldreporter.h"
#include <array>
#include <atomic>
#include <cstddef>
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
constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\d2lobby.packetlogic";
constexpr uint32_t kProtocolVersion = 7;
constexpr size_t kPipeNameMaxLength = 256;

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
    BeginApplied = 0x0204,   // post-original natural broadcast CCmdBeginTurnMsg
    EndSendReturned = 0x0205, // post-original natural CCmdEndTurnMsg Send
    StartupJoinObserved = 0x0206, // pre-dispatch join-lobby CJoinGameMsg gate
    StartupBeginObserved = 0x0207, // pre-dispatch join-lobby BeginTurn gate
    StartupDirectedBeginObserved = 0x0208, // directed join-player BeginTurn stage
    StartupCompleteObserved = 0x0209, // join-player CJoinGame completes stock startup
    BeginSendReturned = 0x020A, // post-original natural CCmdBeginTurnMsg Send
    InvokeButton = 0x0300,   // <- dispatcher: seq | appearance | owner | button target
    SetSelection = 0x0301,   // <- dispatcher: seq | appearance | owner | listbox target
    SetSpin = 0x0302,        // <- dispatcher: seq | appearance | owner | spin target
    SetEditText = 0x0303,    // <- dispatcher: seq | appearance | owner | edit target
    CommandResult = 0x0304,  // -> relay: outcome of a dispatcher command (u32 seq | u8 found)
    MoveStack = 0x0305,      // <- v4 causal map identity + exact from/to (autonav -> worldactions)
    InvokeToggle = 0x0306,   // <- dispatcher: seq | appearance | owner | toggle target
    SelectScenarioPath = 0x030A, // <- dispatcher: exact registered scenario path
    EnableToggle = 0x030B,   // <- dispatcher: require unchecked+enabled, then set true once
    EnableAutoBattle = 0x030C, // <- dispatcher: exact Russobit viewer kick
    AutoBattleKickResult = 0x030D, // -> relay: post-callback kick invariant
    CommandStarted = 0x030E, // -> relay: u32 seq, immediately before one native MoveStack
    InvokePairedEndTurn = 0x030F, // <- relay: exact EndTurn arm; waits on its UI thread
    ReleasePairedEndTurn = 0x0310, // <- relay: u32 armed seq; bridge-thread release edge

    UiSnapshot = 0x0410,     // -> relay: current dialog + all its widgets with state (JSON)
    WorldSnapshot = 0x0411,  // -> relay: players' resources + all map stacks (JSON, world reporter)
    LegacyStacksSnapshot = 0x0412, // -> relay: host live stacks (packed legacy binary census)
    LobbyChat = 0x0413, // -> relay: recent UTF-8 custom-lobby chat JSON
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
std::atomic<bool> g_telemetryObserversRegistered{false};
std::atomic<bool> g_turnEventsRequested{false};
std::atomic<bool> g_turnEventsEnabled{false};
std::atomic<bool> g_joinStartupRole{false};
enum class JoinStartupPhase : std::uint8_t
{
    AwaitBroadcastBegin,
    AwaitHostJoin,
    AwaitDirectedJoinBegin,
    AwaitJoinPlayerJoin,
    Complete,
};
struct JoinStartupWitness
{
    JoinStartupPhase phase = JoinStartupPhase::AwaitBroadcastBegin;
    std::uint32_t senderDpid = 0;
    std::uint32_t receiverDpid = 0;
    std::uint32_t hostHandle = 0;
    std::uint32_t joinHandle = 0;
};
std::mutex g_joinStartupMutex;
JoinStartupWitness g_joinStartupWitness;
bool g_telemetryPreflighted = false;
bool g_telemetryPlanRequested = false;
bool g_transportUsesTcp = false;
std::wstring g_preflightedPipeName{kPipeName};
std::string g_preflightedTcpHost;
std::uint16_t g_preflightedTcpPort = 0;

[[noreturn]] void bridgeFatal(const char* reason, unsigned exitCode)
{
    spdlog::critical("[testdrv] bridge {}; terminating fail-closed", reason);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

[[noreturn]] void bridgeFatalWin32(const char* reason,
                                   DWORD win32Error,
                                   unsigned exitCode)
{
    spdlog::critical(
        "[testdrv] bridge {}; win32-error={}; terminating fail-closed",
        reason, static_cast<unsigned>(win32Error));
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

bool isExactPipeName(const std::wstring& value)
{
    constexpr wchar_t prefix[] = L"\\\\.\\pipe\\";
    constexpr size_t prefixLength = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    if (value.size() <= prefixLength || value.compare(0, prefixLength, prefix) != 0)
        return false;
    for (size_t i = prefixLength; i < value.size(); ++i) {
        const wchar_t ch = value[i];
        const bool allowed = (ch >= L'a' && ch <= L'z')
                          || (ch >= L'A' && ch <= L'Z')
                          || (ch >= L'0' && ch <= L'9')
                          || ch == L'.' || ch == L'_' || ch == L'-';
        if (!allowed)
            return false;
    }
    return true;
}

enum class EnvironmentValueState
{
    Missing,
    Present,
    Invalid,
};

template <std::size_t Capacity>
EnvironmentValueState readExactEnvironmentA(const char* name,
                                            char (&value)[Capacity],
                                            DWORD& length)
{
    static_assert(Capacity > 1 && Capacity <= MAXDWORD,
                  "environment buffer must have one exact DWORD-sized bound");
    SetLastError(ERROR_SUCCESS);
    length = GetEnvironmentVariableA(
        name, value, static_cast<DWORD>(Capacity));
    const DWORD error = GetLastError();
    if (length >= Capacity) {
        spdlog::error("[testdrv] {} exceeds its exact bound", name);
        return EnvironmentValueState::Invalid;
    }
    if (length != 0)
        return EnvironmentValueState::Present;
    if (error == ERROR_ENVVAR_NOT_FOUND)
        return EnvironmentValueState::Missing;
    if (error == ERROR_SUCCESS)
        spdlog::error("[testdrv] {} is explicitly empty", name);
    else
        spdlog::error("[testdrv] could not read {}; win32-error={}",
                      name, static_cast<unsigned>(error));
    return EnvironmentValueState::Invalid;
}

template <std::size_t Capacity>
EnvironmentValueState readExactEnvironmentW(const wchar_t* name,
                                            wchar_t (&value)[Capacity],
                                            DWORD& length)
{
    static_assert(Capacity > 1 && Capacity <= MAXDWORD,
                  "environment buffer must have one exact DWORD-sized bound");
    SetLastError(ERROR_SUCCESS);
    length = GetEnvironmentVariableW(
        name, value, static_cast<DWORD>(Capacity));
    const DWORD error = GetLastError();
    if (length >= Capacity) {
        spdlog::error("[testdrv] D2TESTDRV_PIPE_NAME exceeds its exact bound");
        return EnvironmentValueState::Invalid;
    }
    if (length != 0)
        return EnvironmentValueState::Present;
    if (error == ERROR_ENVVAR_NOT_FOUND)
        return EnvironmentValueState::Missing;
    if (error == ERROR_SUCCESS)
        spdlog::error("[testdrv] D2TESTDRV_PIPE_NAME is explicitly empty");
    else
        spdlog::error(
            "[testdrv] could not read D2TESTDRV_PIPE_NAME; win32-error={}",
            static_cast<unsigned>(error));
    return EnvironmentValueState::Invalid;
}

bool preflightTransport()
{
    char tcpHost[256]{};
    DWORD hostLength = 0;
    const EnvironmentValueState hostState = readExactEnvironmentA(
        "D2TESTDRV_BRIDGE_TCP_HOST", tcpHost, hostLength);
    if (hostState == EnvironmentValueState::Invalid)
        return false;

    char tcpPort[16]{};
    DWORD portLength = 0;
    const EnvironmentValueState portState = readExactEnvironmentA(
        "D2TESTDRV_BRIDGE_TCP_PORT", tcpPort, portLength);
    if (portState == EnvironmentValueState::Invalid)
        return false;
    if ((hostState == EnvironmentValueState::Present)
        != (portState == EnvironmentValueState::Present)) {
        spdlog::error(
            "[testdrv] bridge TCP host and port must be configured as one exact pair");
        return false;
    }

    if (hostState == EnvironmentValueState::Present) {
        unsigned long parsedPort = 0;
        for (DWORD i = 0; i < portLength; ++i) {
            if (tcpPort[i] < '0' || tcpPort[i] > '9') {
                spdlog::error("[testdrv] bridge TCP port is not exact decimal");
                return false;
            }
            parsedPort = parsedPort * 10 + static_cast<unsigned>(tcpPort[i] - '0');
            if (parsedPort > 65535)
                break;
        }
        if (parsedPort == 0 || parsedPort > 65535) {
            spdlog::error("[testdrv] bridge TCP port is outside 1..65535");
            return false;
        }
        const unsigned long parsedAddress = inet_addr(tcpHost);
        if (parsedAddress == INADDR_NONE) {
            spdlog::error("[testdrv] bridge TCP host must be one exact IPv4 address");
            return false;
        }
        g_transportUsesTcp = true;
        g_preflightedTcpHost.assign(tcpHost, hostLength);
        g_preflightedTcpPort = static_cast<std::uint16_t>(parsedPort);
        return true;
    }

    wchar_t pipeName[kPipeNameMaxLength + 1]{};
    DWORD pipeLength = 0;
    const EnvironmentValueState pipeState = readExactEnvironmentW(
        L"D2TESTDRV_PIPE_NAME", pipeName, pipeLength);
    if (pipeState == EnvironmentValueState::Invalid)
        return false;

    const std::wstring resolved = pipeState == EnvironmentValueState::Missing
        ? std::wstring{kPipeName}
        : std::wstring{pipeName, pipeLength};
    if (!isExactPipeName(resolved)) {
        spdlog::error(
            "[testdrv] D2TESTDRV_PIPE_NAME must match \\\\.\\pipe\\[A-Za-z0-9._-]+");
        return false;
    }
    g_transportUsesTcp = false;
    g_preflightedPipeName = resolved;
    return true;
}

struct SendItem
{
    Op op;
    std::vector<uint8_t> payload;
};
std::mutex g_send_mutex;
std::deque<SendItem> g_send_queue;
constexpr size_t kSendQueueMax = 256;

constexpr char kBeginTurnRtti[] = ".?AVCCmdBeginTurnMsg@@";
constexpr char kJoinGameRtti[] = ".?AVCJoinGameMsg@@";
constexpr char kCommandEndTurnRtti[] = ".?AVCCmdEndTurnMsg@@";
constexpr std::uint32_t kNetMessagePayloadOffset =
    offsetof(game::NetMessageHeader, messageClassName);
constexpr std::uint32_t kBeginTurnPayloadSize = 48;
constexpr std::uint32_t kBeginTurnFrameLength =
    kNetMessagePayloadOffset + kBeginTurnPayloadSize;
constexpr std::uint32_t kCommandEndTurnFrameLength =
    sizeof(game::NetMessageHeader) + 1 + sizeof(std::uint32_t);
constexpr std::uint32_t kBeginTurnAddresseeOffset = 36;
constexpr std::uint32_t kBeginTurnSequenceOffset = 40;
constexpr std::uint32_t kBeginTurnActiveHandleOffset = 44;

// Russobit CJoinGameMsg: fixed prefix through encoded name length, one
// variable NUL-terminated player name, then one LRaceCategory id.
constexpr std::uint32_t kJoinGamePlayerHandleOffset = 36;
constexpr std::uint32_t kJoinGameNameLengthOffset = 40;
constexpr std::uint32_t kJoinGameNameOffset = 44;
constexpr std::uint32_t kJoinGameRaceCategorySize = 4;
constexpr std::uint32_t kJoinGameFixedPayloadSize =
    kJoinGameNameOffset + kJoinGameRaceCategorySize;

// Exact 49-byte CCmdEndTurnMsg frame: 44-byte NetMessageHeader, one
// finalOfRound byte, then the four-byte player handle.
constexpr std::uint32_t kCommandEndTurnRoundFlagOffset =
    sizeof(game::NetMessageHeader);
constexpr std::uint32_t kCommandEndTurnPlayerHandleOffset =
    kCommandEndTurnRoundFlagOffset + 1;

static_assert(kBeginTurnFrameLength == 56,
              "exact Russobit BeginTurn frame length changed");
static_assert(kJoinGameFixedPayloadSize == 48,
              "exact Russobit JoinGame fixed payload layout changed");
static_assert(kNetMessagePayloadOffset == 8,
              "exact Russobit NetMessageHeader prefix changed");
static_assert(kCommandEndTurnRoundFlagOffset == 44,
              "exact Russobit EndTurn round flag offset changed");
static_assert(kCommandEndTurnPlayerHandleOffset == 45,
              "exact Russobit EndTurn player-handle offset changed");
static_assert(kCommandEndTurnFrameLength == 49,
              "exact Russobit EndTurn frame length changed");

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
                if (n == SOCKET_ERROR)
                    return false;
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
            if (n == SOCKET_ERROR)
                return false;
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
            if (n == SOCKET_ERROR)
                return false;
            if (n == 0)
                return false;
            read += n;
        }
        out_op = static_cast<Op>(*(uint16_t*)(buf.data() + 0));
        if (*(uint16_t*)(buf.data() + 2) != 0)
            return false;
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
    if (*(uint16_t*)(buf.data() + 2) != 0)
        return false;
    out_payload.assign(buf.begin() + 4, buf.end());
    return true;
}

bool enqueue(Op opcode, const void* payload, uint32_t size)
{
    if (!g_running.load(std::memory_order_acquire))
        return false;
    try {
        SendItem item;
        item.op = opcode;
        if (size > 0 && payload)
            item.payload.assign((const uint8_t*)payload, (const uint8_t*)payload + size);
        std::lock_guard<std::mutex> lk(g_send_mutex);
        if (g_send_queue.size() >= kSendQueueMax)
            bridgeFatal("outbound event queue overflowed", 0xD2E7740Au);
        g_send_queue.push_back(std::move(item));
        return true;
    } catch (...) {
        bridgeFatal("could not allocate one outbound event", 0xD2E7740Bu);
    }
}

template <std::size_t Size>
bool hasExactRtti(const std::uint8_t* payload, std::uint32_t payloadSize,
                  const char (&expected)[Size])
{
    static_assert(Size > 1, "RTTI literal must include text and its terminator");
    return payload && payloadSize >= Size
           && std::memcmp(payload, expected, Size) == 0;
}

std::uint32_t readU32(const std::uint8_t* bytes)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void writeU32(std::uint8_t* bytes, std::uint32_t value)
{
    std::memcpy(bytes, &value, sizeof(value));
}

bool isDynamicPlayerDpid(std::uint32_t dpid)
{
    return dpid > game::serverNetPlayerId
           && dpid != game::singleNetPlayerId && dpid != UINT32_MAX;
}

// Startup telemetry observes the validated natural frame in the independent
// DebugTest secondary RX policy slot and always returns Pass. Production's
// primary simultaneous-turn policy remains authoritative and runs first.
netintercept::RxDecision onJoinStartupRxObserved(
    void*, void*, int packet, std::uint32_t frameLength,
    std::uint32_t senderDpid, std::uint32_t receiverDpid)
{
    if (!g_turnEventsEnabled.load(std::memory_order_acquire)
        || !g_joinStartupRole.load(std::memory_order_acquire)
        || packet == 0 || frameLength < kNetMessagePayloadOffset
        || !isDynamicPlayerDpid(receiverDpid)) {
        return netintercept::RxDecision::Pass;
    }

    const auto* frame = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(static_cast<std::uint32_t>(packet)));
    if (readU32(frame) != game::netMessageNormalType
        || readU32(frame + 4) != frameLength) {
        return netintercept::RxDecision::Pass;
    }
    const std::uint8_t* payload = frame + kNetMessagePayloadOffset;
    const std::uint32_t payloadSize = frameLength - kNetMessagePayloadOffset;

    if (senderDpid == game::serverNetPlayerId
        && payloadSize == kBeginTurnPayloadSize
        && hasExactRtti(payload, payloadSize, kBeginTurnRtti)) {
        const std::uint32_t addressee =
            readU32(payload + kBeginTurnAddresseeOffset);
        const std::uint32_t commandSequence =
            readU32(payload + kBeginTurnSequenceOffset);
        const std::uint32_t activeHandle =
            readU32(payload + kBeginTurnActiveHandleOffset);
        const bool broadcastHostBegin =
            addressee == 0 && commandSequence == 1 && activeHandle != 0;
        const bool directedJoinBegin =
            addressee != 0 && commandSequence == UINT32_MAX && activeHandle != 0;
        if (!broadcastHostBegin && !directedJoinBegin)
            return netintercept::RxDecision::Pass;

        std::lock_guard<std::mutex> startupLock(g_joinStartupMutex);
        if (broadcastHostBegin) {
            if (g_joinStartupWitness.phase != JoinStartupPhase::AwaitBroadcastBegin) {
                if (g_joinStartupWitness.senderDpid == senderDpid
                    && g_joinStartupWitness.receiverDpid == receiverDpid
                    && g_joinStartupWitness.hostHandle == activeHandle) {
                    bridgeFatal("duplicate startup broadcast BeginTurn transition",
                                0xD2E77420u);
                }
                bridgeFatal("conflicting startup broadcast BeginTurn transition",
                            0xD2E77426u);
            }
            g_joinStartupWitness.senderDpid = senderDpid;
            g_joinStartupWitness.receiverDpid = receiverDpid;
            g_joinStartupWitness.hostHandle = activeHandle;
            std::array<std::uint8_t, 24> evidence{};
            writeU32(evidence.data() + 0, senderDpid);
            writeU32(evidence.data() + 4, receiverDpid);
            writeU32(evidence.data() + 8, frameLength);
            writeU32(evidence.data() + 12, addressee);
            writeU32(evidence.data() + 16, commandSequence);
            writeU32(evidence.data() + 20, activeHandle);
            if (!enqueue(Op::StartupBeginObserved, evidence.data(),
                         static_cast<std::uint32_t>(evidence.size()))) {
                bridgeFatal("startup BeginTurn observation could not be queued",
                            0xD2E77421u);
            }
            // Publish the causal phase only after its evidence is queued. The
            // stock receive stream is ordered, so a qualifying host CJoinGame
            // before this edge is a protocol violation rather than a reason to
            // resubmit or poll.
            g_joinStartupWitness.phase = JoinStartupPhase::AwaitHostJoin;
            return netintercept::RxDecision::Pass;
        }

        if (g_joinStartupWitness.phase != JoinStartupPhase::AwaitDirectedJoinBegin) {
            bridgeFatal("directed startup BeginTurn arrived out of order",
                        0xD2E77429u);
        }
        if (senderDpid != g_joinStartupWitness.senderDpid
            || receiverDpid != g_joinStartupWitness.receiverDpid
            || activeHandle != g_joinStartupWitness.hostHandle
            || addressee == g_joinStartupWitness.hostHandle) {
            bridgeFatal("directed startup BeginTurn identity mismatch",
                        0xD2E7742Au);
        }
        g_joinStartupWitness.joinHandle = addressee;
        std::array<std::uint8_t, 24> evidence{};
        writeU32(evidence.data() + 0, senderDpid);
        writeU32(evidence.data() + 4, receiverDpid);
        writeU32(evidence.data() + 8, frameLength);
        writeU32(evidence.data() + 12, addressee);
        writeU32(evidence.data() + 16, commandSequence);
        writeU32(evidence.data() + 20, activeHandle);
        if (!enqueue(Op::StartupDirectedBeginObserved, evidence.data(),
                     static_cast<std::uint32_t>(evidence.size()))) {
            bridgeFatal("directed startup BeginTurn observation could not be queued",
                        0xD2E7742Bu);
        }
        g_joinStartupWitness.phase = JoinStartupPhase::AwaitJoinPlayerJoin;
        return netintercept::RxDecision::Pass;
    }

    // CJoinGame is an authoritative server broadcast. Its local receiver is
    // the exact dynamic playerNetId supplied by the native Russobit RX ABI.
    if (senderDpid != game::serverNetPlayerId
        || payloadSize < kJoinGameFixedPayloadSize + 1
        || !hasExactRtti(payload, payloadSize, kJoinGameRtti)) {
        return netintercept::RxDecision::Pass;
    }
    const std::uint32_t joinedHandle =
        readU32(payload + kJoinGamePlayerHandleOffset);
    const std::uint32_t encodedNameLength =
        readU32(payload + kJoinGameNameLengthOffset);
    if (joinedHandle == 0 || encodedNameLength == 0
        || encodedNameLength > payloadSize - kJoinGameFixedPayloadSize
        || payloadSize != kJoinGameFixedPayloadSize + encodedNameLength) {
        return netintercept::RxDecision::Pass;
    }
    const std::uint8_t* encodedName = payload + kJoinGameNameOffset;
    if (encodedName[encodedNameLength - 1] != 0
        || std::memchr(encodedName, 0, encodedNameLength - 1) != nullptr) {
        return netintercept::RxDecision::Pass;
    }
    const std::uint32_t raceCategoryId =
        readU32(encodedName + encodedNameLength);

    std::lock_guard<std::mutex> startupLock(g_joinStartupMutex);
    if (g_joinStartupWitness.phase == JoinStartupPhase::AwaitBroadcastBegin)
        bridgeFatal("startup CJoinGame preceded startup BeginTurn", 0xD2E77427u);
    if (senderDpid != g_joinStartupWitness.senderDpid
        || receiverDpid != g_joinStartupWitness.receiverDpid) {
        bridgeFatal("startup CJoinGame transport identity mismatch", 0xD2E7742Cu);
    }

    std::array<std::uint8_t, 24> evidence{};
    writeU32(evidence.data() + 0, senderDpid);
    writeU32(evidence.data() + 4, receiverDpid);
    writeU32(evidence.data() + 8, frameLength);
    writeU32(evidence.data() + 12, joinedHandle);
    writeU32(evidence.data() + 16, encodedNameLength);
    writeU32(evidence.data() + 20, raceCategoryId);

    if (g_joinStartupWitness.phase == JoinStartupPhase::AwaitHostJoin) {
        if (joinedHandle != g_joinStartupWitness.hostHandle) {
            bridgeFatal("non-host CJoinGame preceded startup host transition",
                        0xD2E77428u);
        }
        if (!enqueue(Op::StartupJoinObserved, evidence.data(),
                     static_cast<std::uint32_t>(evidence.size()))) {
            bridgeFatal("startup host CJoinGame observation could not be queued",
                        0xD2E77423u);
        }
        g_joinStartupWitness.phase = JoinStartupPhase::AwaitDirectedJoinBegin;
        return netintercept::RxDecision::Pass;
    }

    if (g_joinStartupWitness.phase == JoinStartupPhase::AwaitDirectedJoinBegin)
        bridgeFatal("CJoinGame preceded directed startup BeginTurn", 0xD2E7742Du);

    if (g_joinStartupWitness.phase == JoinStartupPhase::AwaitJoinPlayerJoin) {
        if (joinedHandle != g_joinStartupWitness.joinHandle) {
            bridgeFatal("startup join-player CJoinGame handle mismatch",
                        0xD2E7742Eu);
        }
        if (!enqueue(Op::StartupCompleteObserved, evidence.data(),
                     static_cast<std::uint32_t>(evidence.size()))) {
            bridgeFatal("startup completion CJoinGame could not be queued",
                        0xD2E7742Fu);
        }
        g_joinStartupWitness.phase = JoinStartupPhase::Complete;
        spdlog::info(
            "[testdrv] exact stock two-player startup completed "
            "(host=0x{:08x}, join=0x{:08x}, receiver={})",
            g_joinStartupWitness.hostHandle, g_joinStartupWitness.joinHandle,
            receiverDpid);
        return netintercept::RxDecision::Pass;
    }

    bridgeFatal("CJoinGame repeated after exact stock startup completion",
                0xD2E77430u);
    return netintercept::RxDecision::Pass;
}

// Post-original natural BeginTurn evidence. Deferred/dropped packets never
// reach this observer and therefore cannot be mistaken for applied state.
void onStockRxApplied(void*, std::uint32_t senderDpid,
                      std::uint32_t receiverDpid,
                      const std::uint8_t* payload,
                      std::uint32_t payloadSize, int dispatchResult)
{
    if (!g_turnEventsEnabled.load(std::memory_order_acquire))
        return;
    if (senderDpid != game::serverNetPlayerId
        || !isDynamicPlayerDpid(receiverDpid) || dispatchResult <= 0)
        return;

    if (payloadSize != kBeginTurnPayloadSize
        || !hasExactRtti(payload, payloadSize, kBeginTurnRtti))
        return;

    const std::uint32_t addressee =
        readU32(payload + kBeginTurnAddresseeOffset);
    const std::uint32_t commandSequence =
        readU32(payload + kBeginTurnSequenceOffset);
    const std::uint32_t activeHandle =
        readU32(payload + kBeginTurnActiveHandleOffset);
    if (addressee != 0 || commandSequence == UINT32_MAX || activeHandle == 0)
        return;

    std::array<std::uint8_t, 28> evidence{};
    writeU32(evidence.data() + 0, senderDpid);
    writeU32(evidence.data() + 4, receiverDpid);
    writeU32(evidence.data() + 8, kBeginTurnFrameLength);
    writeU32(evidence.data() + 12,
             static_cast<std::uint32_t>(dispatchResult));
    writeU32(evidence.data() + 16, addressee);
    writeU32(evidence.data() + 20, commandSequence);
    writeU32(evidence.data() + 24, activeHandle);
    if (!enqueue(Op::BeginApplied, evidence.data(),
                 static_cast<std::uint32_t>(evidence.size()))) {
        bridgeFatal("post-dispatch BeginTurn telemetry could not be queued",
                    0xD2E77424u);
    }
}

// Post-original exact DirectPlay Send evidence for CCmdEndTurnMsg.
void onEndTurnSendReturned(void*, std::uint32_t idTo,
                           const std::uint8_t* message,
                           std::uint32_t size, int sendResult)
{
    if (!g_turnEventsEnabled.load(std::memory_order_acquire))
        return;
    if (!message || !isDynamicPlayerDpid(idTo)
        || size != kCommandEndTurnFrameLength)
        return;

    const std::uint32_t messageType = readU32(message + 0);
    const std::uint32_t storedLength = readU32(message + 4);
    const std::uint8_t* payload = message + kNetMessagePayloadOffset;
    const std::uint32_t payloadSize = size - kNetMessagePayloadOffset;
    if (messageType != game::netMessageNormalType || storedLength != size
        || !hasExactRtti(payload, payloadSize, kCommandEndTurnRtti))
        return;

    const std::uint8_t finalOfRound = message[kCommandEndTurnRoundFlagOffset];
    const std::uint32_t playerHandle =
        readU32(message + kCommandEndTurnPlayerHandleOffset);
    if (finalOfRound > 1 || playerHandle == 0)
        return;

    std::array<std::uint8_t, 12> evidence{};
    writeU32(evidence.data() + 0, idTo);
    writeU32(evidence.data() + 4, size);
    writeU32(evidence.data() + 8, static_cast<std::uint32_t>(sendResult));
    if (!enqueue(Op::EndSendReturned, evidence.data(),
                 static_cast<std::uint32_t>(evidence.size()))) {
        bridgeFatal("post-Send EndTurn telemetry could not be queued",
                    0xD2E77425u);
    }
}

// Post-original exact DirectPlay Send evidence for CCmdBeginTurnMsg. This
// observer is passive: it runs only after the selected natural transport has
// returned and neither changes nor resubmits the borrowed frame.
void onTxSendReturned(void* self, std::uint32_t idTo,
                      const std::uint8_t* message,
                      std::uint32_t size, int sendResult)
{
    if (!g_turnEventsEnabled.load(std::memory_order_acquire))
        return;

    if (message && size == kBeginTurnFrameLength) {
        const std::uint32_t messageType = readU32(message + 0);
        const std::uint32_t storedLength = readU32(message + 4);
        const std::uint8_t* payload = message + kNetMessagePayloadOffset;
        const std::uint32_t payloadSize = size - kNetMessagePayloadOffset;
        if (messageType == game::netMessageNormalType && storedLength == size
            && hasExactRtti(payload, payloadSize, kBeginTurnRtti)) {
            const std::uint32_t addressee =
                readU32(payload + kBeginTurnAddresseeOffset);
            const std::uint32_t commandSequence =
                readU32(payload + kBeginTurnSequenceOffset);
            const std::uint32_t activeHandle =
                readU32(payload + kBeginTurnActiveHandleOffset);
            const bool broadcast = idTo == 0 && addressee == 0
                && commandSequence != 0 && commandSequence != UINT32_MAX
                && activeHandle != 0;
            const bool directed = isDynamicPlayerDpid(idTo) && addressee != 0
                && commandSequence == UINT32_MAX && activeHandle != 0;
            if (broadcast || directed) {
                std::array<std::uint8_t, 24> evidence{};
                writeU32(evidence.data() + 0, idTo);
                writeU32(evidence.data() + 4, size);
                writeU32(evidence.data() + 8,
                         static_cast<std::uint32_t>(sendResult));
                writeU32(evidence.data() + 12, addressee);
                writeU32(evidence.data() + 16, commandSequence);
                writeU32(evidence.data() + 20, activeHandle);
                if (!enqueue(Op::BeginSendReturned, evidence.data(),
                             static_cast<std::uint32_t>(evidence.size()))) {
                    bridgeFatal(
                        "post-Send BeginTurn telemetry could not be queued",
                        0xD2E77431u);
                }
            }
        }
    }

    // Preserve the existing EndTurn observer and its exact acceptance rules.
    onEndTurnSendReturned(self, idTo, message, size, sendResult);
}

netintercept::ObserverBundle telemetryObserverBundle()
{
    netintercept::ObserverBundle bundle;
    bundle.rxPostDispatch = &onStockRxApplied;
    bundle.txPostSend = &onTxSendReturned;
    return bundle;
}

// nettracehooks RX-trace sink. Frame: u32 self, u32 sender, u32 size, byte[size].
// The process-lifetime observer is inert outside the running bridge and never performs transport I/O.
void on_rx_trace(void* self, std::uint32_t sender, const uint8_t* payload, uint32_t size)
{
    if (!g_running.load(std::memory_order_acquire))
        return;
    if (size > 0x10000)
        bridgeFatal("RX trace exceeded the protocol bound", 0xD2E7740Cu);
    std::vector<uint8_t> frame(12 + size);
    *(uint32_t*)(frame.data() + 0) = (uint32_t)(uintptr_t)self;
    *(uint32_t*)(frame.data() + 4) = (uint32_t)sender;
    *(uint32_t*)(frame.data() + 8) = size;
    if (size)
        memcpy(frame.data() + 12, payload, size);
    if (!enqueue(Op::PacketTraceRx, frame.data(), (uint32_t)frame.size())) {
        if (!g_running.load(std::memory_order_acquire))
            return;
        bridgeFatal("could not publish one RX trace", 0xD2E7740Du);
    }
}

// nettracehooks TX-trace sink. Frame: u32 self, u32 idTo, u32 size, byte[size].
// It has the same process-lifetime registration and running-state gate as the RX observer.
void on_tx_trace(void* self, uint32_t idTo, const uint8_t* message, uint32_t size)
{
    if (!g_running.load(std::memory_order_acquire))
        return;
    if (size > 0x10000)
        bridgeFatal("TX trace exceeded the protocol bound", 0xD2E7740Eu);
    std::vector<uint8_t> frame(12 + size);
    *(uint32_t*)(frame.data() + 0) = (uint32_t)(uintptr_t)self;
    *(uint32_t*)(frame.data() + 4) = idTo;
    *(uint32_t*)(frame.data() + 8) = size;
    if (size)
        memcpy(frame.data() + 12, message, size);
    if (!enqueue(Op::PacketTrace, frame.data(), (uint32_t)frame.size())) {
        if (!g_running.load(std::memory_order_acquire))
            return;
        bridgeFatal("could not publish one TX trace", 0xD2E7740Fu);
    }
}

std::vector<uint8_t> build_hello_payload()
{
    char module_path[MAX_PATH];
    GetModuleFileNameA(g_self, module_path, sizeof(module_path));
    size_t mod_len = strlen(module_path);
    // Role (host/join/...) is a required protocol-v7 field used by the paired relay to tag
    // each instance; the exact HelloAck below rejects a relay with a mismatched protocol.
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
    // The game may pin all of its threads to one CPU. Keep brief transport
    // operations responsive while the normal-priority UI thread is busy.
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL))
        bridgeFatalWin32("could not set transport worker priority", GetLastError(), 0xD2E7741Du);

    if (g_transportUsesTcp) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            bridgeFatal("WSAStartup failed", 0xD2E77403u);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            bridgeFatal("could not create its one TCP socket", 0xD2E77404u);
        DWORD timeout_ms = 2000;
        if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout_ms,
                       sizeof(timeout_ms)) == SOCKET_ERROR
            || setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms,
                          sizeof(timeout_ms)) == SOCKET_ERROR) {
            bridgeFatal("could not set finite TCP I/O timeouts", 0xD2E77411u);
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(g_preflightedTcpPort);
        addr.sin_addr.s_addr = inet_addr(g_preflightedTcpHost.c_str());
        if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
            bridgeFatal("one TCP connection attempt failed", 0xD2E77406u);
        g_sock = s;
        spdlog::info("[testdrv] bridge connected via TCP (socket={:d})", (int)s);
    } else {
        HANDLE h = CreateFileW(g_preflightedPipeName.c_str(),
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            bridgeFatalWin32(
                "one named-pipe connection attempt failed", error, 0xD2E77407u);
        }
        g_pipe = h;
        spdlog::info("[testdrv] bridge connected to relay pipe");
    }

    auto hello = build_hello_payload();
    if (!write_message(Op::Hello, hello.data(), (uint32_t)hello.size()))
        bridgeFatal("Hello write failed", 0xD2E77408u);
    Op op;
    std::vector<uint8_t> payload;
    if (!read_message(op, payload) || op != Op::HelloAck || payload.size() != 8
        || *reinterpret_cast<const std::uint32_t*>(payload.data()) != 1
        || *reinterpret_cast<const std::uint32_t*>(payload.data() + 4) != kProtocolVersion) {
        bridgeFatal("did not receive the exact HelloAck", 0xD2E77409u);
    }
    // Telemetry remains inert until this exact flags=0 v3 identity has been
    // accepted. Native observations before the handshake are not relay evidence.
    g_turnEventsEnabled.store(
        g_turnEventsRequested.load(std::memory_order_acquire),
        std::memory_order_release);
    spdlog::info("[testdrv] bridge HelloAck ok ({:d} bytes)", (unsigned)payload.size());
    const char* msg = "mss32 testdrv bridge alive";
    if (!write_message(Op::Log, msg, (uint32_t)strlen(msg)))
        bridgeFatal("could not publish the post-handshake alive event", 0xD2E77412u);

    uint32_t last_ui_epoch = 0;
    uint32_t last_world_epoch = 0;
    uint32_t last_chat_epoch = 0;
    while (g_running.load(std::memory_order_acquire)) {
        // 0. Forward the live UI snapshot (current dialog + every widget with its state) to the
        //    relay whenever it changes, so the dispatcher can scan/verify the UI without scraping
        //    logs. The reporter builds it on the UI thread under a lock and bumps the epoch on each
        //    change; we ship only on a new epoch.
        {
            std::string snap;
            uint32_t epoch = 0;
            if (uistatereporter::copyUiSnapshot(snap, epoch) && epoch != last_ui_epoch) {
                last_ui_epoch = epoch;
                if (!write_message(Op::UiSnapshot, snap.data(), (uint32_t)snap.size())) {
                    if (!g_running.load(std::memory_order_acquire))
                        break;
                    bridgeFatal("could not publish one UI snapshot", 0xD2E77413u);
                }
            }
        }

        // 0b. Forward the live WORLD snapshot (players' resources + every map stack) the same way:
        //     the reporter rebuilds it on the UI thread (throttled) and bumps its epoch on change.
        {
            std::string snap;
            uint32_t epoch = 0;
            if (worldreporter::copyWorldSnapshot(snap, epoch) && epoch != last_world_epoch) {
                last_world_epoch = epoch;
                // Make the diagnostic stream causally older than the world
                // evidence. The harness can observe this epoch only after all
                // complete log records which preceded its UI-thread rebuild
                // have reached the file; no per-record hot-path flush is needed.
                spdlog::default_logger()->flush();
                if (!write_message(Op::WorldSnapshot, snap.data(), (uint32_t)snap.size())) {
                    if (!g_running.load(std::memory_order_acquire))
                        break;
                    bridgeFatal("could not publish one world snapshot", 0xD2E77414u);
                }
            }
        }

        // Chat is a distinct UTF-8 stream; never reuse the packed-stack opcode.
        {
            std::string snap;
            uint32_t epoch = 0;
            if (lobbychatreporter::copyChatLog(snap, epoch) && epoch != last_chat_epoch) {
                last_chat_epoch = epoch;
                if (!write_message(Op::LobbyChat, snap.data(), (uint32_t)snap.size())) {
                    if (!g_running.load(std::memory_order_acquire))
                        break;
                    bridgeFatal("could not publish one lobby-chat snapshot", 0xD2E77419u);
                }
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
            if (!write_message(item.op, item.payload.data(), (uint32_t)item.payload.size(), false)) {
                if (!g_running.load(std::memory_order_acquire))
                    break;
                bridgeFatal("could not publish one queued event", 0xD2E77415u);
            }
        }
        if (!g_running.load(std::memory_order_acquire))
            break;

        // 2. Poll for an incoming message.
        bool has_data = false;
        SOCKET s = g_sock.load();
        if (s != INVALID_SOCKET) {
            u_long avail = 0;
            if (ioctlsocket(s, FIONREAD, &avail) == SOCKET_ERROR) {
                if (!g_running.load(std::memory_order_acquire))
                    break;
                bridgeFatal("lost the TCP connection", 0xD2E77416u);
            }
            has_data = (avail >= 4);
        } else {
            DWORD avail = 0;
            if (!PeekNamedPipe(g_pipe.load(), nullptr, 0, nullptr, &avail, nullptr)) {
                if (!g_running.load(std::memory_order_acquire))
                    break;
                bridgeFatal("lost the named-pipe connection", 0xD2E77417u);
            }
            has_data = (avail >= 4);
        }
        if (has_data) {
            if (!read_message(op, payload)) {
                if (!g_running.load(std::memory_order_acquire))
                    break;
                bridgeFatal("received an invalid or incomplete relay frame", 0xD2E77418u);
            }
            handle_incoming(op, payload);
            continue;
        }
        Sleep(5);
    }

    HANDLE old = g_pipe.exchange(INVALID_HANDLE_VALUE);
    if (old != INVALID_HANDLE_VALUE)
        CloseHandle(old);
    SOCKET sold = g_sock.exchange(INVALID_SOCKET);
    if (sold != INVALID_SOCKET) {
        closesocket(sold);
        WSACleanup();
    }
    g_turnEventsEnabled.store(false, std::memory_order_release);
    spdlog::info("[testdrv] bridge thread exiting");
}

} // namespace

bool preflightTurnEvents(bool requested)
{
    if (g_telemetryPreflighted)
        return g_telemetryPlanRequested == requested;

    if (!preflightTransport())
        return false;

    bool joinRole = false;
    if (requested) {
        char role[32]{};
        const DWORD roleLength = GetEnvironmentVariableA(
            "D2TESTDRV_ROLE", role, static_cast<DWORD>(sizeof(role)));
        if (roleLength >= sizeof(role)) {
            spdlog::error("[testdrv] telemetry role exceeds its exact bound");
            return false;
        }
        joinRole = std::strcmp(role, "join") == 0;
        if (!netintercept::canAddObservers(telemetryObserverBundle())) {
            spdlog::error(
                "[testdrv] no all-or-none capacity for telemetry post-observer pair");
            return false;
        }
    }

    g_telemetryPlanRequested = requested;
    g_joinStartupRole.store(joinRole, std::memory_order_release);
    g_turnEventsRequested.store(requested, std::memory_order_release);
    g_telemetryPreflighted = true;
    spdlog::info("[testdrv] simultaneous-turn telemetry preflight (requested={}, join-role={})",
                 requested, joinRole);
    return true;
}

bool commitTurnEvents()
{
    if (!g_telemetryPreflighted)
        return false;
    if (!g_telemetryPlanRequested)
        return true;
    if (g_telemetryObserversRegistered.load(std::memory_order_acquire))
        return true;

    // The shared registry validates and publishes both post observers under
    // one mutex. Only after that non-partial step do we publish the infallible
    // independent secondary RX callback; production's primary slot is untouched.
    if (!netintercept::addObservers(telemetryObserverBundle()))
        return false;
    nettracehooks::setDispatchCallback(&onJoinStartupRxObserved);
    g_telemetryObserversRegistered.store(true, std::memory_order_release);
    spdlog::info("[testdrv] simultaneous-turn telemetry observers committed all-or-none");
    return true;
}

void setCommandCallback(CommandCallback cb)
{
    g_command_cb = cb;
}

bool start(HMODULE selfModule)
{
    if (g_running.exchange(true, std::memory_order_acq_rel))
        return false; // already started
    if (!g_telemetryPreflighted) {
        g_running.store(false, std::memory_order_release);
        return false;
    }
    g_turnEventsEnabled.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> startupLock(g_joinStartupMutex);
        g_joinStartupWitness = JoinStartupWitness{};
    }
    g_self = selfModule;
    // Packet-trace forwarding is opt-in (D2TESTDRV_NET_INTERCEPT): on_rx_trace runs on the
    // UI/dispatch thread for EVERY received packet, which during a begin-turn replication
    // burst piles work onto the thread the game is mid-loading on. The dispatcher-driven MP
    // test drives off UI state, not packets, so it leaves this off; packet-observability builds
    // turn it on. The process-lifetime RX/TX pair is published all-or-none;
    // running-state gates make it inert again if thread creation fails.
    if (testenv::on("D2TESTDRV_NET_INTERCEPT")) {
        if (!nettracehooks::addObservers(&on_rx_trace, &on_tx_trace)) {
            g_running.store(false, std::memory_order_release);
            return false;
        }
    }
    try {
        g_thread = std::thread(bridge_thread_main);
        g_thread.detach();
    } catch (...) {
        g_running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void stop()
{
    // Publish stop before closing the transport: expected cancellation/close failures are then
    // graceful, while an unsolicited loss with g_running still true remains terminal.
    g_turnEventsEnabled.store(false, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
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
    if (utf8_message && !enqueue(Op::Log, utf8_message, (uint32_t)strlen(utf8_message)))
        bridgeFatal("could not enqueue one log event", 0xD2E77419u);
}

void send_command_result(std::uint32_t seq, bool found)
{
    uint8_t p[5];
    *(uint32_t*)(p + 0) = seq;
    p[4] = found ? 1 : 0;
    if (!enqueue(Op::CommandResult, p, sizeof(p)))
        bridgeFatal("could not publish one command result", 0xD2E77410u);
}

void send_command_started(std::uint32_t seq)
{
    std::uint8_t p[4];
    std::memcpy(p, &seq, sizeof(seq));
    if (!enqueue(Op::CommandStarted, p, sizeof(p)))
        bridgeFatal("could not publish one command-started edge", 0xD2E7741Bu);
}

void send_auto_battle_kick_result(std::uint32_t seq,
                                  const AutoBattleKickResult& result)
{
    // Wire layout is explicit and packed independently of the compiler:
    // u32 seq | 9*u8 invariant fields | u32 bound member-function address.
    std::uint8_t p[17];
    *reinterpret_cast<std::uint32_t*>(p + 0) = seq;
    p[4] = result.succeeded ? 1 : 0;
    p[5] = result.controllerGateBefore;
    p[6] = result.kickStateBefore;
    p[7] = result.kickStateAfter;
    p[8] = result.sideSelector;
    p[9] = result.flag38Before;
    p[10] = result.flag38After;
    p[11] = result.flag39Before;
    p[12] = result.flag39After;
    std::memcpy(p + 13, &result.memberFunction, sizeof(result.memberFunction));
    if (!enqueue(Op::AutoBattleKickResult, p, sizeof(p)))
        bridgeFatal("could not publish one auto-battle kick result", 0xD2E7741Au);
}

void send_legacy_stacks_snapshot(const void* payload, std::uint32_t size)
{
    // Keep the legacy reporter removable: bridge owns only the public wire
    // shape and opcode, while pointer capture/sampling stays in its own module.
    // Exact validation prevents a malformed native census from being accepted
    // as test evidence by the relay.
    constexpr std::uint32_t headerSize = sizeof(std::uint32_t);
    constexpr std::uint32_t recordSize = 20;
    constexpr std::uint32_t maxRecords = 256;
    if (!payload || size < headerSize)
        bridgeFatal("received an invalid legacy stack snapshot", 0xD2E7741Bu);

    std::uint32_t count = 0;
    std::memcpy(&count, payload, sizeof(count));
    if (count == 0 || count > maxRecords
        || size != headerSize + count * recordSize)
        bridgeFatal("received an invalid legacy stack snapshot", 0xD2E7741Bu);
    if (!enqueue(Op::LegacyStacksSnapshot, payload, size))
        bridgeFatal("could not publish one legacy stack snapshot", 0xD2E7741Cu);
}


} // namespace bridge
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV

