/*
 * DebugTest compatibility adapter over hooks::netintercept.
 */

#ifdef D2_TESTDRV

#include "testdrv/nettracehooks.h"
#include "executablefingerprint.h"
#include "midgard.h"
#include "midserver.h"
#include "midserverlogic.h"
#include "mqnetplayer.h"
#include "netplayerinfo.h"
#include "netmsg.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string_view>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace nettracehooks {

namespace {

// Test-only DirectPlay session observation. Shared RX/TX ownership lives
// exclusively in hooks::netintercept.
constexpr std::uintptr_t kEnumSessionSlotVA = 0x006E682C;
constexpr std::uintptr_t kCreateSessionSlotVA = 0x006E6830;
constexpr std::uintptr_t kJoinSessionSlotVA = 0x006E6834;
constexpr std::uintptr_t kEnumExpectedVA = 0x0055D17D;
constexpr std::uintptr_t kCreateExpectedVA = 0x0055D42C;
constexpr std::uintptr_t kJoinExpectedVA = 0x0055D5C5;

using FnEnumSessions = void(__fastcall*)(void* self, void* edx, void* sessions,
                                         const GUID* appGuid, const char* ipAddress,
                                         char allSessions, char requirePassword);
using FnCreateSession = void(__fastcall*)(void* self, void* edx, void** netSession,
                                          const GUID* appGuid, const char* sessionName,
                                          const char* password);
using FnJoinSession = void(__fastcall*)(void* self, void* edx, void** netSession,
                                        void* netSessionEnum, const char* password);

std::atomic<FnEnumSessions> g_originalEnum{nullptr};
std::atomic<FnCreateSession> g_originalCreate{nullptr};
std::atomic<FnJoinSession> g_originalJoin{nullptr};
std::atomic<bool> g_sessionHooksInstalled{false};
std::mutex g_sessionInstallMutex;
std::atomic<bool> g_runtimeObserversRegistered{false};
std::atomic<bool> g_packetLoggingEnabled{false};
std::atomic<std::uint32_t> g_enumCompletedGeneration{0};
std::atomic<std::uint32_t> g_enumReadyGeneration{0};
std::atomic<std::uint32_t> g_firstConnectDpid{0};
std::atomic<std::uint32_t> g_peerConnectDpid{0};
std::array<char, 64> g_directPlayHost{};

[[noreturn]] void failFastSessionRuntime(const char* reason, unsigned exitCode)
{
    spdlog::critical("[nettrace] {}; terminating fail-closed", reason);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

constexpr std::size_t traceTextLength(std::size_t available)
{
    return available < 31 ? available : 31;
}

std::string_view traceText(const std::uint8_t* bytes, std::size_t available)
{
    if (!bytes)
        return {};
    return {reinterpret_cast<const char*>(bytes), traceTextLength(available)};
}

void traceRx(void*, std::uint32_t senderDpid, const std::uint8_t* payload,
             std::uint32_t payloadSize)
{
    if (g_packetLoggingEnabled.load(std::memory_order_acquire)) {
        spdlog::debug("[nettrace] RX '{}' from {:d} ({:d} B)",
                      traceText(payload, payloadSize), senderDpid, payloadSize);
    }

    // Literal kHostScript::WaitPeer witness. The source driver cached the DPID
    // at payload+36 from its first CConnectMsg (the host self-announcement),
    // then released exactly once when the same RX path exposed a different
    // dynamic DPID greater than the server id. Keep this in the removable DebugTest observer: it neither
    // suppresses nor re-dispatches the packet and therefore cannot perturb the
    // stock DirectPlay session flow.
    constexpr char connectRtti[] = ".?AVCConnectMsg@@";
    constexpr std::size_t connectDpidOffset = 36;
    // The legacy hook compared hdr[1] >= 41, where hdr[1] is the complete
    // NetMessageHeader::length. It then named that value payload_size even
    // though its payload pointer was already advanced by eight bytes. The
    // shared MSS observer exposes the bounded body length (frame length - 8),
    // so the equivalent admission is the last byte actually read: body[39].
    constexpr std::size_t connectMinimumPayloadSize =
        connectDpidOffset + sizeof(std::uint32_t);
    static_assert(connectMinimumPayloadSize == 40);
    if (payload && payloadSize >= connectMinimumPayloadSize
        && std::memcmp(payload, connectRtti, sizeof(connectRtti) - 1) == 0) {
        std::uint32_t dpid = 0;
        std::memcpy(&dpid, payload + connectDpidOffset, sizeof(dpid));
        if (dpid <= 1)
            return;

        std::uint32_t self = g_firstConnectDpid.load(std::memory_order_acquire);
        if (!self) {
            std::uint32_t expected = 0;
            if (g_firstConnectDpid.compare_exchange_strong(
                    expected, dpid, std::memory_order_acq_rel)) {
                spdlog::info("[nettrace] self CConnectMsg observed dpid={}", dpid);
                return;
            }
            self = expected;
        }
        if (dpid == self)
            return;

        std::uint32_t peer = g_peerConnectDpid.load(std::memory_order_acquire);
        if (!peer) {
            std::uint32_t expected = 0;
            if (g_peerConnectDpid.compare_exchange_strong(
                    expected, dpid, std::memory_order_acq_rel)) {
                spdlog::info(
                    "[nettrace] peer CConnectMsg observed self={} peer={}", self, dpid);
                return;
            }
            peer = expected;
        }
        // Generic observation does not impose a two-player scenario policy.
        if (peer != dpid)
            spdlog::info("[nettrace] additional CConnectMsg peer observed dpid={}", dpid);
    }
}

void traceTx(void*, std::uint32_t idTo, const std::uint8_t* rawMessage,
             std::uint32_t messageLength)
{
    constexpr std::size_t classOffset = offsetof(game::NetMessageHeader, messageClassName);
    const std::size_t classBytes = messageLength > classOffset
                                       ? std::min<std::size_t>(
                                           messageLength - classOffset,
                                           sizeof(game::NetMessageHeader::messageClassName))
                                       : 0;
    const auto* classText = rawMessage ? rawMessage + classOffset : nullptr;
    spdlog::debug("[nettrace] TX '{}' to {:d} ({:d} B)",
                  traceText(classText, classBytes), idTo, messageLength);
}

netintercept::ObserverBundle runtimeObserverBundle(bool enablePacketLogging)
{
    netintercept::ObserverBundle bundle;
    // WaitPeer is a required causal observer even when verbose packet logging
    // is disabled. TX tracing is diagnostic-only and remains opt-in.
    bundle.rx = &traceRx;
    if (enablePacketLogging)
        bundle.tx = &traceTx;
    return bundle;
}

bool registerRuntimeObservers(bool enablePacketLogging)
{
    if (g_runtimeObserversRegistered.load(std::memory_order_acquire)) {
        return g_packetLoggingEnabled.load(std::memory_order_acquire)
               == enablePacketLogging;
    }

    const auto bundle = runtimeObserverBundle(enablePacketLogging);
    if (!netintercept::addObservers(bundle)) {
        spdlog::error("[nettrace] observer table full; runtime observer bundle not registered");
        return false;
    }
    g_packetLoggingEnabled.store(enablePacketLogging, std::memory_order_release);
    g_runtimeObserversRegistered.store(true, std::memory_order_release);
    return true;
}

void __fastcall enumSessionsHook(void* self, void* edx, void* sessions, const GUID* appGuid,
                                 const char* ipAddress, char allSessions, char requirePassword)
{
    const char* effectiveIp = (ipAddress && ipAddress[0]) ? ipAddress : g_directPlayHost.data();
    if (!effectiveIp[0])
        failFastSessionRuntime("EnumSessions has neither a UI address nor D2TESTDRV_DIRECTPLAY_HOST",
                               0xD2E77216u);
    spdlog::info("[nettrace] EnumSessions host='{}' source={}", effectiveIp,
                 (ipAddress && ipAddress[0]) ? "ui" : "explicit-env");
    const FnEnumSessions original = g_originalEnum.load(std::memory_order_acquire);
    if (!original)
        failFastSessionRuntime("EnumSessions original target is missing", 0xD2E77210u);
    original(self, edx, sessions, appGuid, effectiveIp, allSessions, requirePassword);
    const std::uint32_t previous =
        g_enumCompletedGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (previous == UINT32_MAX)
        failFastSessionRuntime("EnumSessions generation wrapped", 0xD2E77211u);
    spdlog::info("[nettrace] EnumSessions original returned generation={}", previous + 1);
}

void __fastcall createSessionHook(void* self, void* edx, void** netSession, const GUID* appGuid,
                                  const char* sessionName, const char* password)
{
    spdlog::info("[nettrace] CreateSession name='{}'", sessionName ? sessionName : "(null)");
    const FnCreateSession original = g_originalCreate.load(std::memory_order_acquire);
    if (!original)
        failFastSessionRuntime("CreateSession original target is missing", 0xD2E77212u);
    original(self, edx, netSession, appGuid, sessionName, password);
}

void __fastcall joinSessionHook(void* self, void* edx, void** netSession, void* netSessionEnum,
                                const char* password)
{
    spdlog::info("[nettrace] JoinSession");
    const FnJoinSession original = g_originalJoin.load(std::memory_order_acquire);
    if (!original)
        failFastSessionRuntime("JoinSession original target is missing", 0xD2E77213u);
    original(self, edx, netSession, netSessionEnum, password);
}

[[noreturn]] void failFastPatchedSession(const char* operation, std::uintptr_t address,
                                         unsigned exitCode)
{
    spdlog::critical("[nettrace] {} failed after memory changed at {:#x}; terminating",
                     operation, address);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

bool slotMatches(std::uintptr_t address, std::uintptr_t expected)
{
    const std::uintptr_t actual = *reinterpret_cast<const std::uintptr_t*>(address);
    if (actual == expected)
        return true;
    spdlog::error("[nettrace] session slot {:#x} is {:#x}, expected {:#x}", address, actual,
                  expected);
    return false;
}

bool writeSlot(std::uintptr_t address, std::uintptr_t value)
{
    auto* slot = reinterpret_cast<std::uintptr_t*>(address);
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    *slot = value;
    DWORD ignored = 0;
    const BOOL protectionRestored =
        VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    const BOOL cacheFlushed = FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    if (!protectionRestored || !cacheFlushed)
        failFastPatchedSession("session vtable patch finalization", address, 0xD2E77201u);
    return true;
}

bool sessionHooksPreflightUnlocked()
{
    if (g_sessionHooksInstalled.load(std::memory_order_relaxed)) {
        return slotMatches(kEnumSessionSlotVA,
                           reinterpret_cast<std::uintptr_t>(&enumSessionsHook))
               && slotMatches(kCreateSessionSlotVA,
                              reinterpret_cast<std::uintptr_t>(&createSessionHook))
               && slotMatches(kJoinSessionSlotVA,
                              reinterpret_cast<std::uintptr_t>(&joinSessionHook));
    }
    return slotMatches(kEnumSessionSlotVA, kEnumExpectedVA)
           && slotMatches(kCreateSessionSlotVA, kCreateExpectedVA)
           && slotMatches(kJoinSessionSlotVA, kJoinExpectedVA);
}

bool sessionHooksPreflight()
{
    std::lock_guard<std::mutex> lock(g_sessionInstallMutex);
    return sessionHooksPreflightUnlocked();
}

bool installSessionHooks()
{
    std::lock_guard<std::mutex> lock(g_sessionInstallMutex);
    if (!sessionHooksPreflightUnlocked()) {
        spdlog::error("[nettrace] DirectPlay session preflight failed; no session hooks installed");
        return false;
    }
    if (g_sessionHooksInstalled.load(std::memory_order_relaxed))
        return true;

    g_originalEnum.store(reinterpret_cast<FnEnumSessions>(kEnumExpectedVA),
                         std::memory_order_release);
    g_originalCreate.store(reinterpret_cast<FnCreateSession>(kCreateExpectedVA),
                           std::memory_order_release);
    g_originalJoin.store(reinterpret_cast<FnJoinSession>(kJoinExpectedVA),
                         std::memory_order_release);

    const bool enumerate =
        writeSlot(kEnumSessionSlotVA, reinterpret_cast<std::uintptr_t>(&enumSessionsHook));
    const bool create = enumerate
                        && writeSlot(kCreateSessionSlotVA,
                                     reinterpret_cast<std::uintptr_t>(&createSessionHook));
    const bool join = create
                      && writeSlot(kJoinSessionSlotVA,
                                   reinterpret_cast<std::uintptr_t>(&joinSessionHook));
    if (!join) {
        bool rollbackOk = true;
        if (create)
            rollbackOk &= writeSlot(kCreateSessionSlotVA, kCreateExpectedVA);
        if (enumerate)
            rollbackOk &= writeSlot(kEnumSessionSlotVA, kEnumExpectedVA);
        spdlog::error("[nettrace] session apply failed (Enum={}, Create={}, Join={}); rollback={}",
                      enumerate ? "ok" : "FAIL", create ? "ok" : "FAIL",
                      join ? "ok" : "FAIL", rollbackOk ? "ok" : "FAIL");
        if (!rollbackOk)
            failFastPatchedSession("partial session-hook rollback", kEnumSessionSlotVA,
                                   0xD2E77202u);
        return false;
    }

    g_sessionHooksInstalled.store(true, std::memory_order_release);
    return true;
}

bool loadExplicitDirectPlayHost()
{
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA("D2TESTDRV_DIRECTPLAY_HOST",
                                                  g_directPlayHost.data(),
                                                  static_cast<DWORD>(g_directPlayHost.size()));
    // Preserve the generic harness's localhost default. An explicit empty or
    // oversized endpoint remains an error; scenario runners may pin an address.
    if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        std::strcpy(g_directPlayHost.data(), "127.0.0.1");
        return true;
    }
    if (length == 0 || length >= g_directPlayHost.size()) {
        spdlog::error(
            "[nettrace] D2TESTDRV_DIRECTPLAY_HOST must name one explicit DirectPlay endpoint");
        return false;
    }
    return true;
}

} // namespace

bool addRxObserver(RxTraceCallback callback)
{
    if (!netintercept::addRxObserver(callback)) {
        spdlog::warn("[nettrace] RX observer registration rejected");
        return false;
    }
    return true;
}

bool addTxObserver(TxTraceCallback callback)
{
    if (!netintercept::addTxObserver(callback)) {
        spdlog::warn("[nettrace] TX observer registration rejected");
        return false;
    }
    return true;
}

bool addObservers(RxTraceCallback rx, TxTraceCallback tx)
{
    netintercept::ObserverBundle bundle;
    bundle.rx = rx;
    bundle.tx = tx;
    if (!netintercept::addObservers(bundle)) {
        spdlog::warn("[nettrace] RX/TX observer-pair registration rejected");
        return false;
    }
    return true;
}

bool addRxPostDispatchObserver(RxPostDispatchObserver callback)
{
    return netintercept::addRxPostDispatchObserver(callback);
}

bool addTxPostSendObserver(TxPostSendObserver callback)
{
    return netintercept::addTxPostSendObserver(callback);
}

void setDispatchCallback(RxDispatchCallback callback)
{
    netintercept::setSecondaryRxDispatchCallback(callback);
}

void setTxCallback(TxGateCallback callback)
{
    netintercept::setSecondaryTxCallback(callback);
}

int recvDispatchDepth()
{
    return netintercept::recvDispatchDepth();
}

unsigned long mainThreadId()
{
    return netintercept::mainThreadId();
}

void onUiFrame()
{
    const std::uint32_t completed =
        g_enumCompletedGeneration.load(std::memory_order_acquire);
    std::uint32_t ready = g_enumReadyGeneration.load(std::memory_order_acquire);
    if (completed == ready)
        return;
    // Publish latest completed discovery; repeated enumeration is normal UI.
    if (!g_enumReadyGeneration.compare_exchange_strong(
            ready, completed, std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (ready == completed)
            return;
        failFastSessionRuntime("EnumSessions UI-frame generation publication raced",
                               0xD2E77215u);
    }
    spdlog::info("[nettrace] EnumSessions ready on next natural UI frame generation={}", completed);
}

bool preflight(bool enablePacketLogging)
{
    if (!executablefingerprint::isExactRussobit()) {
        spdlog::error("[nettrace] exact Russobit fingerprint mismatch");
        return false;
    }
    if (!loadExplicitDirectPlayHost())
        return false;
    if (!netintercept::preflight())
        return false;
    if (!sessionHooksPreflight()) {
        spdlog::error("[nettrace] DirectPlay session preflight failed");
        return false;
    }
    if (!netintercept::canAddObservers(runtimeObserverBundle(enablePacketLogging))) {
        spdlog::error("[nettrace] observer table has no capacity for runtime observer bundle");
        return false;
    }
    return true;
}

bool commit(bool enablePacketLogging)
{
    if (!netintercept::install())
        return false;
    if (!installSessionHooks())
        return false;
    if (!registerRuntimeObservers(enablePacketLogging))
        return false;

    spdlog::info(
        "[nettrace] installed (shared RX/TX; sessions Enum/Create/Join; packet-log={})",
        enablePacketLogging ? "on" : "off");
    return true;
}

bool install(bool enablePacketLogging)
{
    return preflight(enablePacketLogging) && commit(enablePacketLogging);
}

} // namespace nettracehooks
} // namespace testdrv
} // namespace hooks

namespace hooks::netintercept {
namespace {
constexpr int kMaxObservers = 4;
std::array<RxTraceCallback, kMaxObservers> g_rxObservers{};
std::array<TxTraceCallback, kMaxObservers> g_txObservers{};
std::atomic<int> g_rxObserverCount{0};
std::atomic<int> g_txObserverCount{0};
std::array<RxPostDispatchObserver, kMaxObservers> g_rxPostDispatchObservers{};
std::array<TxPostSendObserver, kMaxObservers> g_txPostSendObservers{};
std::atomic<int> g_rxPostDispatchObserverCount{0};
std::atomic<int> g_txPostSendObserverCount{0};
std::mutex g_observerMutex;

std::atomic<RxDispatchCallback> g_secondaryRxDispatchCallback{nullptr};
std::atomic<TxGateCallback> g_secondaryTxCallback{nullptr};

template <typename Callback, std::size_t Size>
bool canAppendObserver(Callback callback, const std::array<Callback, Size>& observers,
                       int count)
{
    if (!callback)
        return true;
    for (int i = 0; i < count; ++i)
        if (observers[static_cast<std::size_t>(i)] == callback)
            return true;
    return count < static_cast<int>(Size);
}

template <typename Callback, std::size_t Size>
int appendObserver(Callback callback, std::array<Callback, Size>& observers, int count)
{
    if (!callback)
        return count;
    for (int i = 0; i < count; ++i)
        if (observers[static_cast<std::size_t>(i)] == callback)
            return count;
    observers[static_cast<std::size_t>(count)] = callback;
    return count + 1;
}

bool hasAnyObserver(const ObserverBundle& bundle)
{
    return bundle.rx || bundle.tx || bundle.rxPostDispatch || bundle.txPostSend;
}

bool canAddObserversUnlocked(const ObserverBundle& bundle)
{
    if (!hasAnyObserver(bundle))
        return false;

    const int rxCount = g_rxObserverCount.load(std::memory_order_relaxed);
    const int txCount = g_txObserverCount.load(std::memory_order_relaxed);
    if (!canAppendObserver(bundle.rx, g_rxObservers, rxCount)
        || !canAppendObserver(bundle.tx, g_txObservers, txCount)) {
        return false;
    }
    const int rxPostCount =
        g_rxPostDispatchObserverCount.load(std::memory_order_relaxed);
    const int txPostCount =
        g_txPostSendObserverCount.load(std::memory_order_relaxed);
    if (!canAppendObserver(bundle.rxPostDispatch, g_rxPostDispatchObservers, rxPostCount)
        || !canAppendObserver(bundle.txPostSend, g_txPostSendObservers, txPostCount)) {
        return false;
    }
    return true;
}

[[noreturn]] void failFastRuntime(const char* operation, unsigned exitCode)
{
    spdlog::critical("[netintercept] {}; terminating fail-closed", operation);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}
} // namespace

bool addRxObserver(RxTraceCallback callback)
{
    ObserverBundle bundle;
    bundle.rx = callback;
    return addObservers(bundle);
}

bool addTxObserver(TxTraceCallback callback)
{
    ObserverBundle bundle;
    bundle.tx = callback;
    return addObservers(bundle);
}

bool canAddObservers(const ObserverBundle& bundle)
{
    std::lock_guard<std::mutex> lock(g_observerMutex);
    return canAddObserversUnlocked(bundle);
}

bool addObservers(const ObserverBundle& bundle)
{
    std::lock_guard<std::mutex> lock(g_observerMutex);
    if (!canAddObserversUnlocked(bundle))
        return false;

    const int rxCount = g_rxObserverCount.load(std::memory_order_relaxed);
    const int txCount = g_txObserverCount.load(std::memory_order_relaxed);
    const int rxPostCount =
        g_rxPostDispatchObserverCount.load(std::memory_order_relaxed);
    const int txPostCount =
        g_txPostSendObserverCount.load(std::memory_order_relaxed);

    const int newRxCount = appendObserver(bundle.rx, g_rxObservers, rxCount);
    const int newTxCount = appendObserver(bundle.tx, g_txObservers, txCount);
    const int newRxPostCount = appendObserver(
        bundle.rxPostDispatch, g_rxPostDispatchObservers, rxPostCount);
    const int newTxPostCount =
        appendObserver(bundle.txPostSend, g_txPostSendObservers, txPostCount);

    // Every array write is complete before any reader-visible count advances.
    g_rxObserverCount.store(newRxCount, std::memory_order_release);
    g_txObserverCount.store(newTxCount, std::memory_order_release);
    g_rxPostDispatchObserverCount.store(newRxPostCount, std::memory_order_release);
    g_txPostSendObserverCount.store(newTxPostCount, std::memory_order_release);
    return true;
}

bool addRxPostDispatchObserver(RxPostDispatchObserver callback)
{
    ObserverBundle bundle;
    bundle.rxPostDispatch = callback;
    return addObservers(bundle);
}

bool addTxPostSendObserver(TxPostSendObserver callback)
{
    ObserverBundle bundle;
    bundle.txPostSend = callback;
    return addObservers(bundle);
}

void setSecondaryRxDispatchCallback(RxDispatchCallback callback)
{
    g_secondaryRxDispatchCallback.store(callback, std::memory_order_release);
}

void setSecondaryTxCallback(TxGateCallback callback)
{
    g_secondaryTxCallback.store(callback, std::memory_order_release);
}

bool canClaimSecondaryTxCallback(TxGateCallback callback)
{
    if (!callback)
        return false;
    const TxGateCallback current =
        g_secondaryTxCallback.load(std::memory_order_acquire);
    return !current || current == callback;
}

bool claimSecondaryTxCallback(TxGateCallback callback)
{
    if (!callback)
        return false;
    TxGateCallback current = g_secondaryTxCallback.load(std::memory_order_acquire);
    for (;;) {
        if (current == callback)
            return true;
        if (current)
            return false;
        if (g_secondaryTxCallback.compare_exchange_weak(
                current, callback, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

int dispatchLocalServerFrameNow(std::uint32_t senderDpid,
                                const game::NetMessageHeader* message)
{
    if (!executablefingerprint::isExactRussobit() || !installed() || !message
        || message->messageType != game::netMessageNormalType
        || message->length < sizeof(game::NetMessageHeader)
        || message->length > game::netMessageMaxLength) {
        return 0;
    }
    const DWORD uiThread = mainThreadId();
    if (!uiThread || GetCurrentThreadId() != uiThread || recvDispatchDepth() != 0)
        return 0;

    auto* midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data || !midgard->data->multiplayerGame
        || midgard->data->hotseatGame || !midgard->data->host
        || !midgard->data->server
        || !midgard->data->server->data) {
        return 0;
    }
    if (senderDpid <= game::serverNetPlayerId
        || senderDpid == game::singleNetPlayerId)
        return 0;

    auto* serverData = midgard->data->server->data;
    auto* serverPlayer = reinterpret_cast<game::IMqNetPlayer*>(
        serverData->netPlayerServer);
    if (!serverPlayer || !serverPlayer->vftable
        || !serverPlayer->vftable->getNetId || !serverData->serverLogic
        || !serverData->netCallbacks || !serverData->netMsgEntryData) {
        return 0;
    }
    const int receiverNetId = serverPlayer->vftable->getNetId(serverPlayer);
    if (receiverNetId != static_cast<int>(game::serverNetPlayerId))
        return 0;

    auto* playerInfo = game::CMidServerLogicApi::get().getPlayerInfo(
        serverData->serverLogic, senderDpid);
    if (!playerInfo || !playerInfo->controlledByHuman
        || playerInfo->playerNetId != senderDpid)
        return 0;

    // Russobit 0x4338BE supplies CMidServerData+8, i.e. the address of
    // netCallbacks, as sub_55B948's receiver-map self. Preserve the same typed
    // server route without a cached DirectPlay receive object. The serialized
    // frame is borrowed only for this synchronous call.
    void* const serverReceiveSelf = static_cast<void*>(&serverData->netCallbacks);
    return testdetail::dispatchLocalServerFrame(
        serverReceiveSelf, senderDpid, receiverNetId, message);
}

namespace testdetail {
void observeRx(void* self, std::uint32_t senderDpid, const std::uint8_t* payload,
               std::uint32_t payloadSize)
{
    const int observerCount = g_rxObserverCount.load(std::memory_order_acquire);
    for (int i = 0; i < observerCount; ++i) {
        g_rxObservers[static_cast<std::size_t>(i)](
            self, senderDpid, payload, payloadSize);
    }
}

void observeTx(void* self, std::uint32_t idTo, const game::NetMessageHeader* message)
{
    const int observerCount = g_txObserverCount.load(std::memory_order_acquire);
    for (int i = 0; i < observerCount; ++i) {
        g_txObservers[static_cast<std::size_t>(i)](
            self, idTo, reinterpret_cast<const std::uint8_t*>(message), message->length);
    }
}

RxDecision applyRxPolicy(void* self, void* edx, int packet, std::uint32_t frameLength,
                         std::uint32_t senderDpid, std::uint32_t receiverDpid)
{
    if (const RxDispatchCallback callback =
            g_secondaryRxDispatchCallback.load(std::memory_order_acquire))
        return callback(self, edx, packet, frameLength, senderDpid, receiverDpid);
    return RxDecision::Pass;
}

TxDecision applyTxPolicy(void* self, std::uint32_t idTo,
                         const game::NetMessageHeader* message)
{
    if (const TxGateCallback callback =
            g_secondaryTxCallback.load(std::memory_order_acquire))
        return callback(self, idTo, message);
    return TxDecision::Pass;
}

void observeRxDispatched(void* self, std::uint32_t postDispatchSenderDpid,
                         std::uint32_t postDispatchReceiverDpid,
                         const std::uint8_t* postDispatchPayload,
                         std::uint32_t postDispatchPayloadSize, int result)
{
    const int observerCount =
        g_rxPostDispatchObserverCount.load(std::memory_order_acquire);
    for (int i = 0; i < observerCount; ++i) {
        try {
            g_rxPostDispatchObservers[static_cast<std::size_t>(i)](
                self, postDispatchSenderDpid, postDispatchReceiverDpid,
                postDispatchPayload, postDispatchPayloadSize, result);
        } catch (...) {
            // Debug instrumentation must not unwind through the exact
            // native receive ABI after the engine has mutated state.
            failFastRuntime("post-dispatch RX observer threw", 0xD2E77112u);
        }
    }
}

void observeTxSent(void* self, std::uint32_t idTo, const std::uint8_t* postSendMessage,
                   std::uint32_t postSendSize, int result)
{
    const int observerCount =
        g_txPostSendObserverCount.load(std::memory_order_acquire);
    for (int i = 0; i < observerCount; ++i) {
        try {
            g_txPostSendObservers[static_cast<std::size_t>(i)](
                self, idTo, postSendMessage, postSendSize, result);
        } catch (...) {
            // Debug instrumentation must not unwind through the exact
            // native Send ABI after the selected transport has returned.
            failFastRuntime("post-Send TX observer threw", 0xD2E77113u);
        }
    }
}
} // namespace testdetail
} // namespace hooks::netintercept

#endif // D2_TESTDRV
