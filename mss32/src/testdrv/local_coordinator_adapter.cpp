#ifdef D2_TESTDRV
#include "testdrv/local_coordinator_adapter.h"
#include "testdrv/testenv.h"
#include <spdlog/spdlog.h>

#ifdef D2_SIMTURNS
#include "hooks.h"
#include "midgard.h"
#include "netcustomservice.h"
#include "simturns/controller.h"
#include "simturns/coordinator_port.h"
#include "simturns/protocol.h"
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <process.h>
#include <stdexcept>
#include <string>

namespace hooks::testdrv::local_coordinator_adapter {
namespace {
namespace p = simturns::protocol;
constexpr DWORD ioTimeout = 5000;
constexpr std::size_t maxQueuedFrames = 32;
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Adapter {
    std::mutex mutex;
    std::wstring pipe;
    simturns::Role role{};
    bool parsed{}, selected{}, valid{}, started{}, failed{};
    std::atomic<bool> mapArmed{};
    Handle stopEvent, wakeEvent, worker;
    std::deque<p::Bytes> outgoing;
};
Adapter& state() { static auto* value = new Adapter; return *value; }
game::CMidgardApi::Api::CreateNetClient originalCreateNetClient{};

std::wstring environment(const wchar_t* name)
{
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const auto length = GetEnvironmentVariableW(name, value.data(), size);
    if (!length || length >= size) return {};
    value.resize(length);
    return value;
}
bool stopping() { return WaitForSingleObject(state().stopEvent.value, 0) == WAIT_OBJECT_0; }
[[noreturn]] void ioFailure(const char* operation)
{
    throw std::runtime_error(std::string(operation) + " failed (Windows "
                             + std::to_string(GetLastError()) + ")");
}
void failed(const char* reason)
{
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (s.failed || stopping()) return;
        s.failed = true;
    }
    spdlog::error("[testdrv][local-coordinator] {}", reason);
    simturns::CoordinatorPort::processInstance().fail(
        simturns::CoordinatorFailureOrigin::LocalInvariant, reason);
    SetEvent(s.stopEvent.value);
}

// Every pending operation is canceled and reaped before its stack OVERLAPPED
// and byte buffer disappear, including exceptions from a concurrent write.
struct Io {
    HANDLE pipe;
    OVERLAPPED operation{};
    bool pending{};
    Io(HANDLE pipe, HANDLE event) : pipe(pipe) { operation.hEvent = event; ResetEvent(event); }
    ~Io() {
        if (pending) {
            CancelIoEx(pipe, &operation);
            DWORD ignored{};
            GetOverlappedResult(pipe, &operation, &ignored, TRUE);
        }
    }
    bool finish(DWORD& count) {
        if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) ioFailure("pipe I/O");
        pending = false;
        return count != 0;
    }
};
bool writeFrame(HANDLE pipe, HANDLE event, const p::Bytes& bytes)
{
    const auto deadline = GetTickCount64() + ioTimeout;
    std::size_t offset{};
    while (offset < bytes.size() && !stopping()) {
        Io io(pipe, event);
        DWORD written{};
        if (!WriteFile(pipe, bytes.data() + offset,
                       static_cast<DWORD>(bytes.size() - offset), &written, &io.operation)) {
            if (GetLastError() != ERROR_IO_PENDING) ioFailure("pipe write");
            io.pending = true;
            const auto now = GetTickCount64();
            if (now >= deadline) throw std::runtime_error("pipe write timeout");
            HANDLE events[]{state().stopEvent.value, event};
            const auto result = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(deadline - now));
            if (result == WAIT_OBJECT_0) return false;
            if (result == WAIT_TIMEOUT) throw std::runtime_error("pipe write timeout");
            if (result != WAIT_OBJECT_0 + 1) ioFailure("pipe write wait");
            io.finish(written);
        }
        if (!written) throw std::runtime_error("pipe closed during write");
        offset += written;
    }
    return !stopping();
}
bool enqueue(const p::Bytes& bytes)
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.failed || stopping() || s.outgoing.size() == maxQueuedFrames
        || bytes.size() < 8 || bytes.size() > p::maxFrameLength + 4u) return false;
    s.outgoing.push_back(bytes);
    SetEvent(s.wakeEvent.value);
    return true;
}
bool drain(HANDLE pipe, HANDLE event)
{
    for (;;) {
        p::Bytes next;
        {
            auto& s = state();
            std::lock_guard lock(s.mutex);
            if (s.failed || stopping()) return false;
            if (s.outgoing.empty()) return true;
            next = std::move(s.outgoing.front());
            s.outgoing.pop_front();
        }
        if (!writeFrame(pipe, event, next)) return false;
    }
}
void run()
{
    auto& s = state();
    HANDLE opened = CreateFileW(s.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (opened == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
        if (!WaitNamedPipeW(s.pipe.c_str(), ioTimeout)) ioFailure("pipe connect");
        if (stopping()) return;
        opened = CreateFileW(s.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    if (opened == INVALID_HANDLE_VALUE) ioFailure("pipe connect");
    Handle pipe(opened), readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!readEvent.value || !writeEvent.value) ioFailure("pipe event creation");
    if (!writeFrame(pipe.value, writeEvent.value, p::encodeHello(GetCurrentProcessId(), s.role))) return;
    const auto handshakeDeadline = GetTickCount64() + ioTimeout;
    p::FrameDecoder decoder;
    bool accepted{};
    std::array<std::uint8_t, 4096> bytes{};
    while (!stopping()) {
        if (accepted && !drain(pipe.value, writeEvent.value)) return;
        Io io(pipe.value, readEvent.value);
        DWORD count{};
        if (!ReadFile(pipe.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, &io.operation)) {
            if (GetLastError() != ERROR_IO_PENDING) ioFailure("pipe read");
            io.pending = true;
            for (;;) {
                const auto now = GetTickCount64();
                if (!accepted && now >= handshakeDeadline) throw std::runtime_error("HelloAck timeout");
                HANDLE events[]{s.stopEvent.value, s.wakeEvent.value, readEvent.value};
                const auto result = WaitForMultipleObjects(3, events, FALSE,
                    accepted ? INFINITE : static_cast<DWORD>(handshakeDeadline - now));
                if (result == WAIT_OBJECT_0) return;
                if (result == WAIT_OBJECT_0 + 1) {
                    if (accepted && !drain(pipe.value, writeEvent.value)) return;
                    continue;
                }
                if (result == WAIT_TIMEOUT) throw std::runtime_error("HelloAck timeout");
                if (result != WAIT_OBJECT_0 + 2) ioFailure("pipe read wait");
                io.finish(count);
                break;
            }
        }
        if (!count) throw std::runtime_error("local coordinator disconnected");
        std::vector<p::Frame> frames;
        std::string error;
        if (!decoder.push(bytes.data(), count, frames, error)) throw std::runtime_error(error);
        for (const auto& frame : frames) {
            if (stopping()) return;
            if (!accepted) {
                p::HelloAck ack;
                if (frame.op != p::Op::HelloAck || !p::decodeHelloAck(frame.payload, ack, error)
                    || !ack.accepted || ack.version != p::version)
                    throw std::runtime_error("local coordinator rejected v8 Hello");
                accepted = true;
                spdlog::info("[testdrv][local-coordinator] v8 Hello accepted");
                continue;
            }
            if (!s.mapArmed.load())
                throw std::runtime_error("local control arrived before native session arming");
            const auto complete = p::encodeFrame(frame.op, frame.payload);
            simturns::CoordinatorPort::processInstance().receive(complete.data(), complete.size());
        }
    }
}
unsigned __stdcall threadEntry(void*)
{
    try { run(); }
    catch (const std::exception& e) { failed(e.what()); }
    catch (...) { failed("unhandled local coordinator transport failure"); }
    return 0;
}

std::uint32_t __fastcall createNetClient(game::CMidgard* midgard, int,
                                        const char* name, bool setDefault)
{
    auto& s = state();
    auto* data = midgard ? midgard->data : nullptr;
    if (!data || !data->multiplayerGame || data->hotseatGame)
        return originalCreateNetClient(midgard, name, setDefault);
    // The local adapter cannot replace authenticated lobby admission. Explicit
    // local tests select DirectPlay; lobby tests do not set D2MSS_SIMTURNS.
    if (CNetCustomService::get() || !data->netSession
        || data->host != (s.role == simturns::Role::Host) || s.mapArmed.load()) {
        spdlog::error("[testdrv][local-coordinator] unexpected native session/role");
        return 0;
    }
    if (!start()) return 0;
    {
        std::lock_guard lock(s.mutex);
        if (s.failed || stopping()) return 0;
    }
    auto& port = simturns::CoordinatorPort::processInstance();
    // Sender success is bounded FIFO ownership, like RakPeer::Send, NOT a
    // claim that the pipe physically wrote or the peer acknowledged the frame.
    // The sole worker preserves order; any partial/failed write is terminal.
    const bool armed = port.armLocal({true, s.role}, enqueue, [] {
        SetEvent(state().stopEvent.value);
    });
    if (!armed) return 0;
    s.mapArmed.store(true);
    if (!simturns::beginSession(s.role)) {
        stop();
        port.stop();
        return 0;
    }
    bool transportFailed{};
    {
        std::lock_guard lock(s.mutex);
        transportFailed = s.failed || stopping();
    }
    if (transportFailed) {
        // A connect failure can precede armLocal. Its latched failure must
        // also terminate this newly armed lifetime, never revive the pipe.
        port.fail(simturns::CoordinatorFailureOrigin::LocalInvariant,
                  "local coordinator failed during native session arming");
        return 0;
    }
    return originalCreateNetClient(midgard, name, setDefault);
}
} // namespace

bool requested()
{
    return state().parsed ? state().selected : environment(L"D2MSS_SIMTURNS") == L"1";
}
bool preflight()
{
    auto& s = state();
    if (s.parsed) return !s.selected || s.valid;
    s.parsed = true;
    s.selected = environment(L"D2MSS_SIMTURNS") == L"1";
    if (!s.selected) return true;
    const auto role = environment(L"D2MSS_SIMTURNS_ROLE");
    if (role == L"host") s.role = simturns::Role::Host;
    else if (role == L"join") s.role = simturns::Role::Join;
    else { spdlog::error("D2MSS_SIMTURNS_ROLE must be host or join"); return false; }
    s.pipe = environment(L"D2MSS_SIMTURNS_PIPE");
    if (s.pipe.empty()) s.pipe = L"\\\\.\\pipe\\d2mss.simturns.v8";
    constexpr wchar_t prefix[] = L"\\\\.\\pipe\\";
    constexpr auto prefixLength = std::size(prefix) - 1;
    if (s.pipe.size() <= prefixLength || s.pipe.size() >= 240
        || s.pipe.compare(0, prefixLength, prefix) != 0) return false;
    for (std::size_t i = prefixLength; i < s.pipe.size(); ++i) {
        const auto c = s.pipe[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-')) return false;
    }
    if (!testenv::supportedGameBuild()) return false;
    // Exact CreateNetClient detour entry is validated below before descriptors
    // enter the ordinary all-or-nothing MSS hook transaction.
    originalCreateNetClient = game::CMidgardApi::get().createNetClient;
    // Russobit 0x403200: mov eax,0x686aa4; call 0x66d3d0. Both complete
    // instructions, the ECX/stack ABI and ret 8 were checked in the exact EXE.
    constexpr std::uint8_t prologue[]{0xb8, 0xa4, 0x6a, 0x68, 0x00,
                                      0xe8, 0xc6, 0xa1, 0x26, 0x00};
    s.valid = originalCreateNetClient
              && std::memcmp(reinterpret_cast<const void*>(originalCreateNetClient),
                             prologue, sizeof(prologue)) == 0;
    if (!s.valid) spdlog::error("[testdrv][local-coordinator] CreateNetClient prologue mismatch");
    return s.valid;
}
void appendHooks(std::vector<HookInfo>& hooks)
{
    if (state().selected && state().valid)
        hooks.push_back({reinterpret_cast<void*>(game::CMidgardApi::get().createNetClient),
                         reinterpret_cast<void*>(&createNetClient),
                         reinterpret_cast<void**>(&originalCreateNetClient)});
}
bool start()
{
    auto& s = state();
    if (!s.selected) return true;
    std::lock_guard lock(s.mutex);
    if (!s.valid || s.failed) return false;
    if (s.started) return !stopping();
    if (!simturns::available() || !testenv::pinHarnessModule()) return false;
    s.stopEvent.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s.wakeEvent.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.stopEvent.value || !s.wakeEvent.value) return false;
    s.started = true;
    s.worker.value = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, threadEntry, nullptr, 0, nullptr));
    if (!s.worker.value) { s.failed = true; return false; }
    return true;
}
void beginTeardown()
{
    if (state().mapArmed.load()) SetEvent(state().stopEvent.value);
}
void stop()
{
    auto& s = state();
    if (!s.mapArmed.exchange(false)) return;
    SetEvent(s.stopEvent.value);
    if (s.worker.value && s.worker.value != INVALID_HANDLE_VALUE)
        WaitForSingleObject(s.worker.value, INFINITE);
    std::lock_guard lock(s.mutex);
    s.outgoing.clear();
    // One local connection belongs to one acceptance run. No reconnect or
    // automatic new epoch; normal lobby restarts never enter this adapter.
}
} // namespace hooks::testdrv::local_coordinator_adapter
#else
namespace hooks::testdrv::local_coordinator_adapter {
bool requested() { return testenv::on("D2MSS_SIMTURNS"); }
bool preflight() {
    if (!requested()) return true;
    spdlog::error("Local simultaneous-turn tests require EnableSimultaneousTurns=true");
    return false;
}
void appendHooks(std::vector<HookInfo>&) {}
bool start() { return !requested(); }
void beginTeardown() {}
void stop() {}
}
#endif
#endif
