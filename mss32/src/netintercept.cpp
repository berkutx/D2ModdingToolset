/*
 * Shared, policy-free RX/TX interception for the exact Russobit executable.
 */

#include "netintercept.h"
#include "executablefingerprint.h"
#include "netmsg.h"
#include "uiframedispatcher.h"
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <spdlog/spdlog.h>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace netintercept {

namespace {

// Exact-Russobit sites retained from b7637a03; see docs/2026-09-22_simturns-port-report.md.
constexpr std::uintptr_t kRecvDispatchVA = 0x0055B948;
constexpr std::uintptr_t kRecvCallSite1VA = 0x00402CA7;
constexpr std::uintptr_t kRecvCallSite2VA = 0x0043396E;
constexpr std::uintptr_t kDPlaySendSlotVA = 0x006E69B0;
constexpr std::uintptr_t kDPlaySendExpectedVA = 0x0055E3C7;
constexpr std::uint32_t kRxPayloadOffset =
    offsetof(game::NetMessageHeader, messageClassName);
static_assert(kRxPayloadOffset == 8);

// Exact stock ABI: sub_55B948(self, packet, idFrom, playerNetId). The packet's
// +4 word is the complete frame length, including its eight-byte prefix; no
// size is passed on the stack. Both stock callers obtain playerNetId from the
// local receive player, while idFrom is written by DirectPlay Receive.
using FnRecvDispatch =
    int(__fastcall*)(void* self, void* edx, int packet, int idFrom, int playerNetId);
// IMqNetPlayer::Send returns a one-byte bool.  Keep that exact native return
// type at the DirectPlay ABI boundary; upper EAX bits are not a success value.
using FnSend = bool(__fastcall*)(void* self, void* edx, std::uint32_t idTo,
                                  const game::NetMessageHeader* message);

std::atomic<RxDispatchCallback> g_rxDispatchCallback{nullptr};
std::atomic<TxGateCallback> g_txCallback{nullptr};
std::atomic<DeferredOverflowCallback> g_deferredOverflowCallback{nullptr};

std::atomic<FnSend> g_originalSend{nullptr};
std::atomic<bool> g_installed{false};
std::mutex g_installMutex;

std::atomic<int> g_recvDispatchDepth{0};
// Unlike g_recvDispatchDepth, this spans receiveHookCore from its first policy
// observation through its causal completion, including any nested screen loop.
std::atomic<std::uint32_t> g_receiveHookDepth{0};
std::atomic<std::uint32_t> g_sendHookDepth{0};
thread_local std::uint32_t g_localReceiveHookDepth = 0;
thread_local std::uint32_t g_localSendHookDepth = 0;
std::atomic<bool> g_sessionTeardown{false};
std::atomic<bool> g_sessionActive{false};
DWORD g_teardownThreadId = 0; // owner of the paired native teardown boundary
bool g_orderedTaskExecuting = false; // UI thread only
std::atomic<void*> g_capturedRxSelf{nullptr};
std::atomic<bool> g_capturedRxSelfMismatch{false};

struct NativeReceiveTicket
{
    void* context = nullptr;
    NativeReceiveCallback complete = nullptr;
    UiTaskDiscardCallback discard = nullptr;
};

std::mutex g_nativeTicketMutex;
std::unordered_map<const game::NetMessageHeader*, NativeReceiveTicket> g_nativeTickets;
constexpr std::size_t kMaxNativeTickets = 64;

struct DeferredPacket
{
    void* self = nullptr;
    void* edx = nullptr;
    std::uint32_t idFrom = 0;
    std::uint32_t playerNetId = 0;
    std::vector<std::uint8_t> frame;
    NativeReceiveTicket ticket;
};

struct UiTask
{
    UiTaskCallback callback = nullptr;
    void* context = nullptr;
    UiTaskDiscardCallback discard = nullptr;
};

class UiTaskExecutionScope
{
public:
    UiTaskExecutionScope() : previous(g_orderedTaskExecuting)
    {
        g_orderedTaskExecuting = true;
    }
    ~UiTaskExecutionScope() { g_orderedTaskExecuting = previous; }

private:
    bool previous;
};

struct RxCompletionTask
{
    RxCompletionCallback callback = nullptr;
    std::uint32_t tag = 0;
};

struct TxCompletionTask
{
    TxCompletionCallback callback = nullptr;
    std::uint32_t tag = 0;
};

thread_local RxCompletionTask* g_currentRxCompletionTask = nullptr;
thread_local TxCompletionTask* g_currentTxCompletionTask = nullptr;

class CurrentRxCompletionScope
{
public:
    explicit CurrentRxCompletionScope(RxCompletionTask* current)
        : previous(g_currentRxCompletionTask)
    {
        g_currentRxCompletionTask = current;
    }

    ~CurrentRxCompletionScope()
    {
        g_currentRxCompletionTask = previous;
    }

    CurrentRxCompletionScope(const CurrentRxCompletionScope&) = delete;
    CurrentRxCompletionScope& operator=(const CurrentRxCompletionScope&) = delete;

private:
    RxCompletionTask* previous;
};

class CurrentTxCompletionScope
{
public:
    explicit CurrentTxCompletionScope(TxCompletionTask* current)
        : previous(g_currentTxCompletionTask)
    {
        g_currentTxCompletionTask = current;
    }

    ~CurrentTxCompletionScope()
    {
        g_currentTxCompletionTask = previous;
    }

    CurrentTxCompletionScope(const CurrentTxCompletionScope&) = delete;
    CurrentTxCompletionScope& operator=(const CurrentTxCompletionScope&) = delete;

private:
    TxCompletionTask* previous;
};

std::mutex g_deferredMutex;
std::deque<DeferredPacket> g_deferredPackets;
constexpr std::size_t kMaxDeferredPackets = 64;
std::mutex g_uiTaskMutex;
std::deque<UiTask> g_uiTasks;
constexpr std::size_t kMaxUiTasks = 64;
bool g_preferUiTask = false; // accessed only by the validated UI-thread drain

std::atomic<unsigned long> g_mainThreadId{0};

int receiveHookCore(void* self, void* edx, int packet, int idFrom, int playerNetId,
                    bool captureDPlaySelf, bool allowDefer,
                    NativeReceiveTicket* replayTicket = nullptr);
int receiveHookCoreImpl(void* self, void* edx, int packet, int idFrom,
                        int playerNetId, bool captureDPlaySelf,
                        bool allowDefer, NativeReceiveTicket& ticket,
                        NativeReceiveResult& disposition);
int __fastcall receiveHookSite1(void* self, void* edx, int packet, int idFrom,
                                int playerNetId);
int __fastcall receiveHookSite2(void* self, void* edx, int packet, int idFrom,
                                int playerNetId);

[[noreturn]] void failFastPatchedProcess(const char* operation, std::uintptr_t address,
                                         unsigned exitCode)
{
    spdlog::critical("[netintercept] {} failed after memory changed at {:#x}; terminating",
                     operation, address);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

[[noreturn]] void failFastRuntime(const char* operation, unsigned exitCode)
{
    spdlog::critical("[netintercept] {}; terminating fail-closed", operation);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

NativeReceiveTicket takeNativeTicket(int packet)
{
    const auto* buffer = reinterpret_cast<const game::NetMessageHeader*>(
        static_cast<std::uintptr_t>(static_cast<std::uint32_t>(packet)));
    std::lock_guard<std::mutex> lock(g_nativeTicketMutex);
    const auto it = g_nativeTickets.find(buffer);
    if (it == g_nativeTickets.end())
        return {};
    const auto ticket = it->second;
    g_nativeTickets.erase(it);
    return ticket;
}

void discardNativeTicket(NativeReceiveTicket& ticket) noexcept
{
    const auto retired = std::exchange(ticket, {});
    if (!retired.discard)
        return;
    try {
        retired.discard(retired.context);
    } catch (...) {
        failFastRuntime("native receive ticket destruction threw", 0xD2E77118u);
    }
}

void completeNativeTicket(NativeReceiveTicket& ticket, NativeReceiveResult result)
{
    const auto retired = std::exchange(ticket, {});
    if (!retired.complete)
        return;
    try {
        retired.complete(retired.context, result);
    } catch (...) {
        failFastRuntime("native receive ticket completion threw", 0xD2E77119u);
    }
}

bool callTargetMatches(std::uintptr_t callSite, std::uintptr_t expectedTarget)
{
    const auto* code = reinterpret_cast<const std::uint8_t*>(callSite);
    if (code[0] != 0xE8) {
        spdlog::error("[netintercept] {:#x} is not CALL rel32 (byte={:#04x})", callSite,
                      code[0]);
        return false;
    }
    const std::int32_t relative = *reinterpret_cast<const std::int32_t*>(code + 1);
    const auto actualTarget = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(callSite + 5) + static_cast<std::intptr_t>(relative));
    if (actualTarget != expectedTarget) {
        spdlog::error("[netintercept] {:#x} targets {:#x}, expected {:#x}", callSite,
                      actualTarget, expectedTarget);
        return false;
    }
    return true;
}

bool pointerMatches(std::uintptr_t slotAddress, std::uintptr_t expected)
{
    const std::uintptr_t actual = *reinterpret_cast<const std::uintptr_t*>(slotAddress);
    if (actual == expected)
        return true;
    spdlog::error("[netintercept] pointer at {:#x} is {:#x}, expected {:#x}", slotAddress,
                  actual, expected);
    return false;
}

bool writeCallTarget(std::uintptr_t callSite, std::uintptr_t target)
{
    auto* operand = reinterpret_cast<std::int32_t*>(callSite + 1);
    const auto nextInstruction = callSite + 5;
    const auto relative = static_cast<std::int32_t>(target - nextInstruction);
    DWORD oldProtection = 0;
    if (!VirtualProtect(operand, sizeof(*operand), PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;

    *operand = relative;
    DWORD ignored = 0;
    const BOOL protectionRestored =
        VirtualProtect(operand, sizeof(*operand), oldProtection, &ignored);
    const BOOL cacheFlushed =
        FlushInstructionCache(GetCurrentProcess(), operand, sizeof(*operand));
    if (!protectionRestored || !cacheFlushed)
        failFastPatchedProcess("CALL patch finalization", callSite, 0xD2E77101u);
    return true;
}

bool writePointer(std::uintptr_t slotAddress, std::uintptr_t value)
{
    auto* slot = reinterpret_cast<std::uintptr_t*>(slotAddress);
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;

    *slot = value;
    DWORD ignored = 0;
    const BOOL protectionRestored =
        VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    const BOOL cacheFlushed = FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    if (!protectionRestored || !cacheFlushed)
        failFastPatchedProcess("vtable patch finalization", slotAddress, 0xD2E77102u);
    return true;
}

int callOriginalReceive(void* self, void* edx, int packet, int idFrom, int playerNetId)
{
    g_recvDispatchDepth.fetch_add(1, std::memory_order_acq_rel);
    int result = 0;
    __try {
        result = reinterpret_cast<FnRecvDispatch>(kRecvDispatchVA)(
            self, edx, packet, idFrom, playerNetId);
    } __finally {
        // Balance depth, but deliberately do not consume an AV from the game's
        // dispatcher. Continuing after one would turn a deterministic failure
        // into delayed state corruption.
        g_recvDispatchDepth.fetch_sub(1, std::memory_order_acq_rel);
    }
    return result;
}

void notifyDeferredOverflowBestEffort(std::uint32_t idFrom,
                                      const std::uint8_t* payload,
                                      std::uint32_t payloadSize) noexcept
{
    const DeferredOverflowCallback callback =
        g_deferredOverflowCallback.load(std::memory_order_acquire);
    if (!callback)
        return;
    try {
        callback(idFrom, payload, payloadSize);
    } catch (...) {
        // A diagnostic callback must never defeat the unconditional fail-fast
        // boundary that follows allocation loss/queue exhaustion.
    }
}

void enqueueDeferredPacket(void* self, void* edx, std::uint32_t idFrom,
                           std::uint32_t playerNetId, const std::uint8_t* frame,
                           std::uint32_t frameLength, std::uint32_t payloadSize,
                           NativeReceiveTicket& ticket)
{
    if (g_sessionTeardown.load(std::memory_order_acquire)) {
        discardNativeTicket(ticket);
        return;
    }
    if (!uiframedispatcher::installed())
        failFastRuntime("deferred RX requested without the natural-frame dispatcher",
                        0xD2E77109u);

    DeferredPacket packet;
    packet.self = self;
    packet.edx = edx;
    packet.idFrom = idFrom;
    packet.playerNetId = playerNetId;
    try {
        // NetMessageHeader::length is the exact byte extent stock dispatch
        // consumes. Snapshot exactly that frame and preserve both DPID inputs.
        packet.frame.assign(frame, frame + static_cast<std::size_t>(frameLength));
    } catch (...) {
        spdlog::critical("[netintercept] could not snapshot deferred RX (sender={}, frame={} B)",
                         idFrom, frameLength);
        notifyDeferredOverflowBestEffort(idFrom, frame + kRxPayloadOffset, payloadSize);
        failFastRuntime("deferred RX allocation failed", 0xD2E77106u);
    }
    bool queued = false;
    bool allocationFailed = false;
    bool retired = false;
    {
        std::lock_guard<std::mutex> lock(g_deferredMutex);
        if (g_sessionTeardown.load(std::memory_order_acquire)) {
            // A worker can decide Defer just before the UI closes this map.
            // Retirement is cancellation of that map, never queue overflow.
            retired = true;
        } else if (g_deferredPackets.size() >= kMaxDeferredPackets) {
            spdlog::critical(
                "[netintercept] deferred RX queue overflow (capacity={}, sender={}, frame={} B)",
                kMaxDeferredPackets, idFrom, frameLength);
        } else {
            try {
                g_deferredPackets.push_back(std::move(packet));
                g_deferredPackets.back().ticket = std::exchange(ticket, {});
                queued = true;
            } catch (...) {
                // Never unwind an allocation failure through the native receive
                // hook/C ABI. The still-owned packet identifies the loss below.
                allocationFailed = true;
            }
        }
    }
    if (retired) {
        discardNativeTicket(ticket);
        return;
    }
    if (queued)
        return;

    // Neither an old nor the current ordered packet is silently discarded. Give
    // the controller one non-blocking notification, then stop before gameplay can
    // continue with an unknowably incomplete RX stream.
    notifyDeferredOverflowBestEffort(idFrom, frame + kRxPayloadOffset, payloadSize);
    failFastRuntime(allocationFailed ? "deferred RX queue allocation failed"
                                     : "deferred RX queue exhausted",
                    allocationFailed ? 0xD2E77108u : 0xD2E77105u);
}

int receiveHookCore(void* self, void* edx, int packet, int idFrom, int playerNetId,
                    bool captureDPlaySelf, bool allowDefer,
                    NativeReceiveTicket* replayTicket)
{
    // Keep this wrapper free of C++ objects requiring unwinding: __finally must
    // retire the full-hook depth even when the native dispatcher raises SEH,
    // while the exception itself must continue outward unchanged.
    g_receiveHookDepth.fetch_add(1, std::memory_order_acq_rel);
    ++g_localReceiveHookDepth;
    NativeReceiveTicket ticket = replayTicket ? std::exchange(*replayTicket, {})
                                              : takeNativeTicket(packet);
    NativeReceiveResult disposition = NativeReceiveResult::Failed;
    int result = 0;
    __try {
        if (g_sessionTeardown.load(std::memory_order_acquire)) {
            // Native clear owns stopping/joining its worker. Final worker calls
            // retain the original path while that owner is still alive.
            result = callOriginalReceive(self, edx, packet, idFrom, playerNetId);
        } else {
            result = receiveHookCoreImpl(self, edx, packet, idFrom, playerNetId,
                                         captureDPlaySelf, allowDefer, ticket, disposition);
        }
    } __finally {
        --g_localReceiveHookDepth;
        g_receiveHookDepth.fetch_sub(1, std::memory_order_acq_rel);
        if (AbnormalTermination() || g_sessionTeardown.load(std::memory_order_acquire))
            discardNativeTicket(ticket);
    }
    completeNativeTicket(ticket, disposition);
    return result;
}

int receiveHookCoreImpl(void* self, void* edx, int packet, int idFrom,
                        int playerNetId, bool captureDPlaySelf,
                        bool allowDefer, NativeReceiveTicket& ticket,
                        NativeReceiveResult& disposition)
{
    if (captureDPlaySelf && self && g_sessionActive.load(std::memory_order_acquire)
        && !g_sessionTeardown.load(std::memory_order_acquire)) {
        void* expected = nullptr;
        if (!g_capturedRxSelf.compare_exchange_strong(expected, self,
                                                      std::memory_order_acq_rel)
            && expected != self) {
            g_capturedRxSelfMismatch.store(true, std::memory_order_release);
        }
    }

    RxCompletionTask completion;
    if (packet) {
        const auto* frame = reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(static_cast<std::uint32_t>(packet)));
        std::uint32_t messageType = 0;
        std::uint32_t frameLength = 0;
        std::memcpy(&messageType, frame, sizeof(messageType));
        std::memcpy(&frameLength, frame + sizeof(messageType), sizeof(frameLength));
        if (messageType == game::netMessageNormalType
            && frameLength >= sizeof(game::NetMessageHeader)
            && frameLength <= game::netMessageMaxLength) {
            const std::uint32_t payloadSize = frameLength - kRxPayloadOffset;
            const std::uint8_t* payload = frame + kRxPayloadOffset;
            const std::uint32_t senderDpid = static_cast<std::uint32_t>(idFrom);
            const std::uint32_t receiverDpid =
                static_cast<std::uint32_t>(playerNetId);

            RxDecision decision = RxDecision::Pass;
            {
                // A policy may arm one causal completion only while this exact
                // invocation is being classified. Restoring the TLS pointer
                // before original dispatch prevents nested engine code from
                // accidentally attaching work to the outer packet.
                CurrentRxCompletionScope completionScope(&completion);
                if (const RxDispatchCallback callback =
                        g_rxDispatchCallback.load(std::memory_order_acquire))
                    decision = callback(self, edx, packet, frameLength,
                                        senderDpid, receiverDpid);
            }
            if (decision != RxDecision::Pass) {
                if (decision == RxDecision::Consume) {
                    disposition = NativeReceiveResult::Filtered;
                    return 0;
                }
                if (decision == RxDecision::Drop)
                    return 0;
                if (decision == RxDecision::Defer) {
                    if (!allowDefer) {
                        failFastRuntime(
                            "RX policy attempted to defer a synchronous replay/direct dispatch",
                            0xD2E77115u);
                    }
                    enqueueDeferredPacket(self, edx, senderDpid, receiverDpid,
                                          frame, frameLength, payloadSize, ticket);
                    return 0;
                }
            }
        }
    }
    const int result = callOriginalReceive(self, edx, packet, idFrom, playerNetId);
    disposition = result > 0 ? NativeReceiveResult::Applied : NativeReceiveResult::Failed;
    if (completion.callback && !g_sessionTeardown.load(std::memory_order_acquire)) {
        if (result <= 0) {
            failFastRuntime("armed post-dispatch RX transition had no matching engine handler",
                            0xD2E7710Fu);
        }
        try {
            completion.callback(completion.tag);
        } catch (...) {
            failFastRuntime("post-dispatch RX completion threw", 0xD2E7710Du);
        }
    }
    return result;
}

int __fastcall receiveHookSite1(void* self, void* edx, int packet, int idFrom,
                                int playerNetId)
{
    // 0x402CA7 passes sub_40336F(v13) = *(outer+8)+36: the proven DPlay
    // receive inner-self required for later local packet injection.
    return receiveHookCore(self, edx, packet, idFrom, playerNetId, true, true);
}

int __fastcall receiveHookSite2(void* self, void* edx, int packet, int idFrom,
                                int playerNetId)
{
    // 0x43396E passes a different (a2+8) receiver container. It participates
    // in observation/policy but must never replace the injection owner.
    return receiveHookCore(self, edx, packet, idFrom, playerNetId, false, true);
}

int directPlaySendContinuation(void* self, void* transportContext,
                               std::uint32_t idTo,
                               const game::NetMessageHeader* message)
{
    const FnSend original = g_originalSend.load(std::memory_order_acquire);
    if (!original)
        failFastPatchedProcess("missing original Send target", kDPlaySendSlotVA, 0xD2E77103u);
    return original(self, transportContext, idTo, message) ? 1 : 0;
}

int dispatchTxCore(void* self, void* transportContext, std::uint32_t idTo,
                   const game::NetMessageHeader* message,
                   TxSendContinuation continuation)
{
    if (!continuation)
        failFastRuntime("TX dispatcher received no natural-send continuation", 0xD2E77114u);
    if (g_sessionTeardown.load(std::memory_order_acquire))
        return continuation(self, transportContext, idTo, message);

    TxCompletionTask completion;
    if (message) {
        TxDecision decision = TxDecision::Pass;
        {
            // The completion belongs only to this exact Send invocation. A
            // nested engine Send gets its own TLS scope and cannot steal it.
            CurrentTxCompletionScope completionScope(&completion);
            if (const TxGateCallback callback =
                    g_txCallback.load(std::memory_order_acquire)) {
                decision = callback(self, idTo, message);
            }
        }
        if (decision != TxDecision::Pass) {
            if (completion.callback) {
                failFastRuntime(
                    "TX policy armed a post-Send completion but suppressed the original Send",
                    0xD2E77110u);
            }
            if (decision == TxDecision::Reject)
                return 0;
            if (decision == TxDecision::Drop || decision == TxDecision::Redirect)
                return 1;
        }
    }

    const int result = continuation(self, transportContext, idTo, message);
    if (completion.callback && !g_sessionTeardown.load(std::memory_order_acquire)) {
        try {
            completion.callback(completion.tag, result);
        } catch (...) {
            failFastRuntime("post-Send TX completion threw", 0xD2E77111u);
        }
    }
    return result;
}

bool __fastcall sendHook(void* self, void* edx, std::uint32_t idTo,
                         const game::NetMessageHeader* message)
{
    return dispatchTx(self, edx, idTo, message, &directPlaySendContinuation) != 0;
}

bool preflightUnlocked()
{
    if (!executablefingerprint::isExactRussobit()) {
        spdlog::warn("[netintercept] exact Russobit fingerprint mismatch; hooks disabled");
        return false;
    }
    if (g_installed.load(std::memory_order_relaxed)) {
        return callTargetMatches(kRecvCallSite1VA,
                                  reinterpret_cast<std::uintptr_t>(&receiveHookSite1))
               && callTargetMatches(kRecvCallSite2VA,
                                     reinterpret_cast<std::uintptr_t>(&receiveHookSite2))
               && pointerMatches(kDPlaySendSlotVA,
                                 reinterpret_cast<std::uintptr_t>(&sendHook));
    }
    if (!callTargetMatches(kRecvCallSite1VA, kRecvDispatchVA)
        || !callTargetMatches(kRecvCallSite2VA, kRecvDispatchVA)
        || !pointerMatches(kDPlaySendSlotVA, kDPlaySendExpectedVA)) {
        spdlog::error("[netintercept] full RX/TX preflight failed; no memory changed");
        return false;
    }
    return true;
}

} // namespace

int dispatchTx(void* self, void* transportContext, std::uint32_t idTo,
               const game::NetMessageHeader* message,
               TxSendContinuation continuation)
{
    g_sendHookDepth.fetch_add(1, std::memory_order_acq_rel);
    ++g_localSendHookDepth;
    int result = 0;
    __try {
        result = dispatchTxCore(self, transportContext, idTo, message, continuation);
    } __finally {
        --g_localSendHookDepth;
        g_sendHookDepth.fetch_sub(1, std::memory_order_acq_rel);
    }
    return result;
}

void setRxDispatchCallback(RxDispatchCallback callback)
{
    g_rxDispatchCallback.store(callback, std::memory_order_release);
}

void setTxCallback(TxGateCallback callback)
{
    g_txCallback.store(callback, std::memory_order_release);
}

bool armCurrentRxCompletion(RxCompletionCallback callback, std::uint32_t tag)
{
    RxCompletionTask* task = g_currentRxCompletionTask;
    if (!callback || !task || task->callback)
        return false;
    task->callback = callback;
    task->tag = tag;
    return true;
}

bool armCurrentTxCompletion(TxCompletionCallback callback, std::uint32_t tag)
{
    TxCompletionTask* task = g_currentTxCompletionTask;
    if (!callback || !task || task->callback)
        return false;
    task->callback = callback;
    task->tag = tag;
    return true;
}

void setDeferredOverflowCallback(DeferredOverflowCallback callback)
{
    g_deferredOverflowCallback.store(callback, std::memory_order_release);
}

bool preflight()
{
    std::lock_guard<std::mutex> lock(g_installMutex);
    return preflightUnlocked();
}

bool install()
{
    if (g_installed.load(std::memory_order_acquire))
        return true;
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_installed.load(std::memory_order_relaxed))
        return true;
    if (!preflightUnlocked())
        return false;

    // Publish the original before exposing sendHook through the vtable slot.
    g_originalSend.store(reinterpret_cast<FnSend>(kDPlaySendExpectedVA),
                         std::memory_order_release);

    const bool firstRx =
        writeCallTarget(kRecvCallSite1VA, reinterpret_cast<std::uintptr_t>(&receiveHookSite1));
    const bool secondRx = firstRx
                           && writeCallTarget(kRecvCallSite2VA,
                                              reinterpret_cast<std::uintptr_t>(&receiveHookSite2));
    const bool send = secondRx
                      && writePointer(kDPlaySendSlotVA,
                                      reinterpret_cast<std::uintptr_t>(&sendHook));
    if (!send) {
        bool rollbackOk = true;
        if (secondRx)
            rollbackOk &= writeCallTarget(kRecvCallSite2VA, kRecvDispatchVA);
        if (firstRx)
            rollbackOk &= writeCallTarget(kRecvCallSite1VA, kRecvDispatchVA);
        spdlog::error("[netintercept] apply failed (RX={}/{}, TX={}); rollback={}",
                      firstRx ? "ok" : "FAIL", secondRx ? "ok" : "FAIL",
                      send ? "ok" : "FAIL", rollbackOk ? "ok" : "FAIL");
        if (!rollbackOk)
            failFastPatchedProcess("partial RX/TX rollback", kRecvCallSite1VA, 0xD2E77104u);
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    spdlog::info("[netintercept] installed atomically (RX=ok/ok, DirectPlay TX=ok)");
    return true;
}

bool installed()
{
    return g_installed.load(std::memory_order_acquire);
}

int recvDispatchDepth()
{
    return g_recvDispatchDepth.load(std::memory_order_acquire);
}

unsigned long mainThreadId()
{
    return g_mainThreadId.load(std::memory_order_acquire);
}

void* capturedRxSelf()
{
    if (g_capturedRxSelfMismatch.load(std::memory_order_acquire))
        return nullptr;
    return g_capturedRxSelf.load(std::memory_order_acquire);
}

bool stageNativeReceive(const game::NetMessageHeader* buffer, void* context,
                        NativeReceiveCallback complete, UiTaskDiscardCallback discard)
{
    if (!buffer || !complete || !discard || !installed())
        return false;
    std::lock_guard<std::mutex> lock(g_nativeTicketMutex);
    if (!g_sessionActive.load(std::memory_order_acquire)
        || g_sessionTeardown.load(std::memory_order_acquire)
        || g_nativeTickets.size() >= kMaxNativeTickets)
        return false;
    try {
        return g_nativeTickets.emplace(buffer, NativeReceiveTicket{context, complete, discard})
            .second;
    } catch (...) {
        return false;
    }
}

bool beginSession()
{
    if (!installed() || !mainThreadId() || GetCurrentThreadId() != mainThreadId()
        || g_sessionActive.load(std::memory_order_acquire)
        || g_sessionTeardown.load(std::memory_order_acquire) || g_orderedTaskExecuting)
        return false;
    std::scoped_lock lock(g_deferredMutex, g_uiTaskMutex, g_nativeTicketMutex);
    if (g_localReceiveHookDepth || g_localSendHookDepth
        || !g_deferredPackets.empty() || !g_uiTasks.empty() || !g_nativeTickets.empty())
        return false;
    g_capturedRxSelf.store(nullptr, std::memory_order_release);
    g_capturedRxSelfMismatch.store(false, std::memory_order_release);
    g_sessionActive.store(true, std::memory_order_release);
    return true;
}

bool beginSessionTeardown()
{
    if (!installed())
        return true;
    const DWORD uiThread = mainThreadId();
    if ((uiThread && GetCurrentThreadId() != uiThread) || g_orderedTaskExecuting
        || g_localReceiveHookDepth || g_localSendHookDepth)
        return false;
    if (g_sessionTeardown.exchange(true, std::memory_order_acq_rel))
        return false;

    std::deque<UiTask> abandoned;
    std::deque<DeferredPacket> abandonedPackets;
    decltype(g_nativeTickets) abandonedTickets;
    {
        std::scoped_lock lock(g_deferredMutex, g_uiTaskMutex, g_nativeTicketMutex);
        // Native startup can clear an empty session before the first screen
        // loop. With no proven UI thread, only this pristine state is legal.
        const bool pristine = !g_capturedRxSelf.load(std::memory_order_acquire)
                              && !g_capturedRxSelfMismatch.load(std::memory_order_acquire)
                              && g_deferredPackets.empty() && g_uiTasks.empty()
                              && g_nativeTickets.empty();
        // Other-thread dispatch is still owned by the native server until the
        // original clear stops and joins it. Validate process-wide zero only
        // at endSessionTeardown, after that native ownership boundary.
        if (!uiThread && !pristine) {
            g_sessionTeardown.store(false, std::memory_order_release);
            return false;
        }
        for (const auto& task : g_uiTasks) {
            if (!task.discard) {
                g_sessionTeardown.store(false, std::memory_order_release);
                return false;
            }
        }
        // These packets belong only to the session being destroyed. Their
        // borrowed self/edx values must never be replayed in a later map.
        abandonedPackets.swap(g_deferredPackets);
        abandonedTickets.swap(g_nativeTickets);
        abandoned.swap(g_uiTasks);
        g_teardownThreadId = GetCurrentThreadId();
    }
    for (auto& packet : abandonedPackets)
        discardNativeTicket(packet.ticket);
    for (auto& entry : abandonedTickets)
        discardNativeTicket(entry.second);
    for (const auto& task : abandoned) {
        try {
            task.discard(task.context);
        } catch (...) {
            failFastRuntime("UI task destruction threw", 0xD2E77117u);
        }
    }
    return true;
}

bool endSessionTeardown()
{
    if (!installed())
        return true;
    if (GetCurrentThreadId() != g_teardownThreadId || g_orderedTaskExecuting
        || !g_sessionTeardown.load(std::memory_order_acquire)
        || g_receiveHookDepth.load(std::memory_order_acquire)
        || g_sendHookDepth.load(std::memory_order_acquire)
        || recvDispatchDepth() != 0)
        return false;
    g_capturedRxSelf.store(nullptr, std::memory_order_release);
    g_capturedRxSelfMismatch.store(false, std::memory_order_release);
    g_sessionActive.store(false, std::memory_order_release);
    g_preferUiTask = false;
    g_teardownThreadId = 0;
    g_sessionTeardown.store(false, std::memory_order_release);
    return true;
}

bool queueOnNextUiFrame(UiTaskCallback callback, void* context,
                        UiTaskDiscardCallback discard)
{
    if (!callback || !installed() || !uiframedispatcher::installed())
        return false;
    {
        std::lock_guard<std::mutex> lock(g_uiTaskMutex);
        if (g_sessionTeardown.load(std::memory_order_acquire)
            || g_uiTasks.size() >= kMaxUiTasks)
            return false;
        try {
            g_uiTasks.push_back(UiTask{callback, context, discard});
        } catch (...) {
            // The task was not accepted. Ownership of context therefore stays
            // with the caller, which will free it and enter its fail-closed path.
            return false;
        }
    }
    spdlog::info("[netintercept] accepted ordered UI task");
    return true;
}

bool invokeOnUiThread(UiTaskCallback callback, void* context,
                      UiTaskDiscardCallback discard)
{
    if (!callback || !installed() || !uiframedispatcher::installed()
        || g_sessionTeardown.load(std::memory_order_acquire))
        return false;

    const DWORD currentThread = GetCurrentThreadId();
    const DWORD uiThread = g_mainThreadId.load(std::memory_order_acquire);
    if (uiThread && uiThread == currentThread && recvDispatchDepth() == 0) {
        UiTaskExecutionScope executionScope;
        callback(context);
        return true;
    }
    return queueOnNextUiFrame(callback, context, discard);
}

bool drainOneOnUiThread()
{
    const DWORD currentThread = GetCurrentThreadId();
    DWORD expected = 0;
    g_mainThreadId.compare_exchange_strong(expected, currentThread, std::memory_order_acq_rel);
    if (g_mainThreadId.load(std::memory_order_acquire) != currentThread) {
        spdlog::error("[netintercept] drain rejected off UI thread (current={}, UI={})",
                      currentThread, g_mainThreadId.load(std::memory_order_relaxed));
        return false;
    }
    if (g_sessionTeardown.load(std::memory_order_acquire) || recvDispatchDepth() != 0) {
        // Stock RX can reenter a screen loop while constructing a dialog. Leave
        // both queues untouched; the next outer natural frame is the sole next
        // ordered-work edge. There is no repost, retry, timer, or fallback seam.
        return false;
    }

    DeferredPacket deferred;
    UiTask task;
    bool haveDeferred = false;
    bool haveTask = false;

    const auto takeDeferred = [&]() {
        std::lock_guard<std::mutex> lock(g_deferredMutex);
        if (g_deferredPackets.empty())
            return false;
        deferred = std::move(g_deferredPackets.front());
        g_deferredPackets.pop_front();
        return true;
    };
    const auto takeUiTask = [&]() {
        std::lock_guard<std::mutex> lock(g_uiTaskMutex);
        if (g_uiTasks.empty())
            return false;
        task = g_uiTasks.front();
        g_uiTasks.pop_front();
        return true;
    };

    // Alternate priority whenever both queues remain busy. The preference flag
    // is touched only by this validated UI-thread path.
    if (g_preferUiTask) {
        haveTask = takeUiTask();
        if (!haveTask)
            haveDeferred = takeDeferred();
    } else {
        haveDeferred = takeDeferred();
        if (!haveDeferred)
            haveTask = takeUiTask();
    }

    if (haveDeferred) {
        g_preferUiTask = true;
        std::uint32_t storedType = 0;
        std::uint32_t storedLength = 0;
        if (deferred.frame.size() >= sizeof(game::NetMessageHeader)) {
            std::memcpy(&storedType, deferred.frame.data(), sizeof(storedType));
            std::memcpy(&storedLength,
                        deferred.frame.data() + sizeof(storedType),
                        sizeof(storedLength));
        }
        if (storedType != game::netMessageNormalType
            || storedLength != deferred.frame.size()
            || storedLength > game::netMessageMaxLength) {
            failFastRuntime("deferred RX snapshot/header length mismatch", 0xD2E77107u);
        }
        spdlog::debug("[netintercept] replay deferred RX sender={} frame={} B",
                      deferred.idFrom, deferred.frame.size());
        receiveHookCore(deferred.self, deferred.edx,
                        static_cast<int>(reinterpret_cast<std::uintptr_t>(deferred.frame.data())),
                        static_cast<int>(deferred.idFrom),
                        static_cast<int>(deferred.playerNetId), false, false, &deferred.ticket);
    } else if (haveTask) {
        g_preferUiTask = false;
        spdlog::info("[netintercept] executing one ordered UI task");
        UiTaskExecutionScope executionScope;
        task.callback(task.context);
    }

    const bool hadOrderedWork = haveDeferred || haveTask;
    return hadOrderedWork;
}

bool injectAuthoritativePayloadNow(std::uint32_t playerNetId,
                                   const std::uint8_t* payload,
                                   std::uint32_t payloadSize)
{
    constexpr std::uint32_t minimumPayloadSize =
        sizeof(game::NetMessageHeader) - kRxPayloadOffset;
    if (!installed() || g_sessionTeardown.load(std::memory_order_acquire)
        || playerNetId <= game::serverNetPlayerId
        || playerNetId == game::singleNetPlayerId || playerNetId == UINT32_MAX
        || !payload
        || payloadSize < minimumPayloadSize
        || payloadSize > game::netMessageMaxLength - kRxPayloadOffset) {
        return false;
    }
    const DWORD uiThread = g_mainThreadId.load(std::memory_order_acquire);
    if (!uiThread || GetCurrentThreadId() != uiThread || recvDispatchDepth() != 0)
        return false;
    void* self = capturedRxSelf();
    if (!self)
        return false;

    const std::uint32_t frameLength = kRxPayloadOffset + payloadSize;
    auto* frame = static_cast<std::uint8_t*>(
        HeapAlloc(GetProcessHeap(), 0, frameLength));
    if (!frame)
        return false;
    *reinterpret_cast<std::uint32_t*>(frame + 0) = game::netMessageNormalType;
    *reinterpret_cast<std::uint32_t*>(frame + 4) = frameLength;
    std::memcpy(frame + kRxPayloadOffset, payload, payloadSize);

    const int handled = callOriginalReceive(
        self, nullptr, static_cast<int>(reinterpret_cast<std::uintptr_t>(frame)),
        static_cast<int>(game::serverNetPlayerId), static_cast<int>(playerNetId));
    HeapFree(GetProcessHeap(), 0, frame);
    // Exact Russobit sub_55B948 returns the number of matching callbacks it
    // dispatched. A well-formed frame with no registered handler is not an
    // applied engine transition and must not be acknowledged to the relay.
    return handled > 0;
}

} // namespace netintercept
} // namespace hooks
