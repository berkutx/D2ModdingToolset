/*
 * Publishable test/logging system for the Disciples 2 modding toolset.
 * Unified network interception + logging — see testdrv/nettracehooks.h.
 *
 * Compile-gated by D2_TESTDRV: without the macro the whole file compiles to
 * nothing and the build is byte-identical to vanilla.
 */

#ifdef D2_TESTDRV

#include "testdrv/nettracehooks.h"
#include "mqnetplayer.h"
#include "netmsg.h"
#include "version.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace nettracehooks {

namespace {

// --- Russobit (4,187,648-byte Discipl2.exe) pinned addresses -----------------
// The recv fan-out sub_55B948 sits ABOVE the transport (IMqNetReception level),
// so the single RX hook catches both native DirectPlay and the mod's SLikeNet
// traffic. The Send vtable is the native-DirectPlay player's IMqNetPlayer table;
// custom (SLikeNet) TX is logged by the mod itself.
constexpr uintptr_t kRecvDispatchVA = 0x55B948;  // sub_55B948 — reception fan-out
constexpr uintptr_t kRecvCallSite1VA = 0x402CA7; // `call sub_55B948` in sub_402BD3 (worker thread)
constexpr uintptr_t kRecvCallSite2VA = 0x43396E; // `call sub_55B948` in sub_4338BE (receive loop)
constexpr uintptr_t kDPlayPlayerVftVA = 0x6E699C;  // CNetDPlayPlayer IMqNetPlayer vtable
constexpr int kSendSlot = 5;                       // IMqNetPlayerVftable::sendMessage
constexpr uintptr_t kDPlayServiceVftVA = 0x6E6824; // CNetDPlayService IMqNetService vtable
constexpr int kEnumSlot = 2;                       // IMqNetServiceVftable::getSessions
constexpr int kCreateSlot = 3;                     // IMqNetServiceVftable::createSession
constexpr int kJoinSlot = 4;                       // IMqNetServiceVftable::joinSession

using FnRecvDispatch = int(__fastcall*)(void* self, void* edx, int packet, int size, int sender);
using FnSend = int(__fastcall*)(void* self, void* edx, std::uint32_t idTo,
                                const game::NetMessageHeader* message);
using FnEnumSessions = void(__fastcall*)(void* self, void* edx, void* sessions, const GUID* appGuid,
                                         const char* ipAddress, char allSessions,
                                         char requirePassword);
using FnCreateSession = void(__fastcall*)(void* self, void* edx, void** netSession,
                                          const GUID* appGuid, const char* sessionName,
                                          const char* password);
using FnJoinSession = void(__fastcall*)(void* self, void* edx, void** netSession,
                                        void* netSessionEnum, const char* password);

// Custom window message: replay one deferred packet on the UI thread.
constexpr UINT WM_D2_DEFERRED_PACKET = WM_USER + 0x46;

std::atomic<HWND> g_main_hwnd{nullptr};
std::atomic<unsigned long> g_main_thread_id{0}; // written on UI thread, read on RX threads
std::atomic<WNDPROC> g_orig_wndproc{nullptr};   // written on subclass thread, read on UI thread

// Recursion-DEPTH counter wrapped exactly around each orig sub_55B948 call.
std::atomic<int> g_recv_dispatch_depth{0};

// Observer slots are published via the atomic counts (release on add / acquire on
// read), so a reader that sees count==N also sees the N callback slots written.
// All of the cross-thread state below is atomic: registration runs on the loader/UI
// thread while the hooks fire on game/worker threads.
constexpr int kMaxObservers = 4;
RxTraceCallback g_rx_observers[kMaxObservers] = {};
std::atomic<int> g_rx_observer_count{0};
TxTraceCallback g_tx_observers[kMaxObservers] = {};
std::atomic<int> g_tx_observer_count{0};
std::atomic<RxDispatchCallback> g_rx_dispatch_cb{nullptr};
std::atomic<TxGateCallback> g_tx_gate_cb{nullptr};

std::atomic<FnSend> g_orig_send{nullptr};
std::atomic<FnEnumSessions> g_orig_enum{nullptr};
std::atomic<FnCreateSession> g_orig_create{nullptr};
std::atomic<FnJoinSession> g_orig_join{nullptr};
bool g_installed = false; // touched only on the single-threaded loader path

// A deferred packet, snapshotted for faithful replay through the SAME RX hook on
// the UI thread. This is generic hold-and-replay infrastructure — what to defer
// (and why) is the gate's policy, not ours.
struct DeferredPacket
{
    void* self;
    void* edx;
    int size;
    int sender;
    std::vector<std::uint8_t> frame; // copy of [u32 messageType][u32 length][body]
};
std::mutex g_deferred_mutex;
std::vector<DeferredPacket> g_deferred;
constexpr size_t kMaxDeferred = 64;

int __fastcall hook_recv_dispatch(void* self, void* edx, int packet, int size, int sender);

// Wrap the orig sub_55B948 with the depth counter + SEH so the counter is ALWAYS
// balanced even if orig faults. No C++ destructible objects here -> __try legal.
int call_orig_recv_guarded(void* self, void* edx, int packet, int size, int sender)
{
    g_recv_dispatch_depth.fetch_add(1);
    int r = 0;
    __try {
        r = ((FnRecvDispatch)kRecvDispatchVA)(self, edx, packet, size, sender);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::warn("[nettrace] orig recv-dispatch SEH-escaped — depth kept balanced");
    }
    g_recv_dispatch_depth.fetch_sub(1);
    return r;
}

// Copy a deferred packet into the queue + wake the UI thread. All std:: objects
// live HERE so the hooks that call it stay __try-clean (MSVC C2712).
void enqueue_deferred_packet(void* self, void* edx, int size, int sender,
                             const std::uint8_t* frame_ptr, std::uint32_t frame_size)
{
    DeferredPacket ent;
    ent.self = self;
    ent.edx = edx;
    ent.size = size;
    ent.sender = sender;
    ent.frame.assign(frame_ptr, frame_ptr + frame_size);
    {
        std::lock_guard<std::mutex> lk(g_deferred_mutex);
        if (g_deferred.size() >= kMaxDeferred) {
            spdlog::warn("[nettrace] deferred-packet queue full — dropping OLDEST");
            g_deferred.erase(g_deferred.begin());
        }
        g_deferred.push_back(std::move(ent));
    }
    HWND hwnd = g_main_hwnd.load();
    if (hwnd)
        PostMessageA(hwnd, WM_D2_DEFERRED_PACKET, 0, 0);
}

// Replay ONE deferred packet by RE-ENTERING the RX hook at depth==0. If an outer
// dispatch is still in flight, re-post and wait.
void drain_one_deferred_packet()
{
    if (g_recv_dispatch_depth.load() > 0) {
        HWND hwnd = g_main_hwnd.load();
        if (hwnd)
            PostMessageA(hwnd, WM_D2_DEFERRED_PACKET, 0, 0);
        return;
    }
    DeferredPacket ent;
    {
        std::lock_guard<std::mutex> lk(g_deferred_mutex);
        if (g_deferred.empty())
            return;
        ent = std::move(g_deferred.front());
        g_deferred.erase(g_deferred.begin());
    }
    spdlog::info("[nettrace] replay deferred packet (sender={:d}, frame={:d} B) at depth 0",
                 ent.sender, (unsigned)ent.frame.size());
    // `ent` outlives this synchronous call, so ent.frame.data() stays valid.
    hook_recv_dispatch(ent.self, ent.edx, (int)(uintptr_t)ent.frame.data(), ent.size, ent.sender);

    bool more = false;
    {
        std::lock_guard<std::mutex> lk(g_deferred_mutex);
        more = !g_deferred.empty();
    }
    if (more) {
        HWND hwnd = g_main_hwnd.load();
        if (hwnd)
            PostMessageA(hwnd, WM_D2_DEFERRED_PACKET, 0, 0);
    }
}

// Fan out an inbound envelope to the RX observers under SEH. `payload_size` is the
// message's self-declared length (hdr[1]); an observer that reads class-name bytes
// at fixed offsets trusts it, but an inconsistent/short replication snapshot (e.g.
// the host serving state before it has finished loading) can make that read run
// past the readable buffer. A malformed inbound message must never turn an
// observer's bounded read into a process-killing AV. __try-clean (no C++ locals
// whose lifetime spans the __try), mirroring call_orig_recv_guarded.
void dispatch_rx_observers_guarded(void* self, int sender, const std::uint8_t* payload,
                                   std::uint32_t payload_size)
{
    const int rxn = g_rx_observer_count.load(std::memory_order_acquire);
    __try {
        for (int i = 0; i < rxn; ++i)
            g_rx_observers[i](self, sender, payload, payload_size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::warn("[nettrace] RX observer SEH-escaped on a malformed inbound message");
    }
}

int __fastcall hook_recv_dispatch(void* self, void* edx, int packet, int size, int sender)
{
    std::uint32_t* hdr = (std::uint32_t*)(uintptr_t)packet;
    if (hdr && hdr[0] == game::netMessageNormalType && hdr[1] <= game::netMessageMaxLength) {
        const std::uint32_t payload_size = hdr[1];
        const std::uint8_t* payload = reinterpret_cast<const std::uint8_t*>(hdr + 2);

        // Observability: report the received CNetMsg envelope to every observer
        // (the relay bridge + the MP-gate detector). Catches chat and system-
        // notification messages too — the relay decodes class names.
        spdlog::debug("[nettrace] RX '{:.31s}' from {:d} ({:d} B)", (const char*)payload, sender,
                      payload_size);
        dispatch_rx_observers_guarded(self, sender, payload, payload_size);

        // Gate: a registered consumer (e.g. a relay-driven test, or another module)
        // may pass / drop / defer this inbound packet. None registered -> just pass.
        if (RxDispatchCallback rxcb = g_rx_dispatch_cb.load(std::memory_order_acquire)) {
            const RxDecision d = rxcb(self, edx, packet, size, sender);
            if (d == RxDecision::Drop)
                return 0;
            if (d == RxDecision::Defer) {
                enqueue_deferred_packet(self, edx, size, sender, payload - 8, 8 + payload_size);
                return 0;
            }
        }
    }

    return call_orig_recv_guarded(self, edx, packet, size, sender);
}

// --- TX (outgoing send) ------------------------------------------------------
int __fastcall hook_send(void* self, void* edx, std::uint32_t idTo,
                         const game::NetMessageHeader* message)
{
    if (message) {
        const int txn = g_tx_observer_count.load(std::memory_order_acquire);
        for (int i = 0; i < txn; ++i)
            g_tx_observers[i](self, idTo, reinterpret_cast<const std::uint8_t*>(message),
                              message->length);
        spdlog::debug("[nettrace] TX '{:.31s}' to {:d} ({:d} B)", message->messageClassName, idTo,
                      message->length);

        if (TxGateCallback txcb = g_tx_gate_cb.load(std::memory_order_acquire)) {
            const TxDecision d = txcb(self, idTo, message);
            if (d == TxDecision::Drop || d == TxDecision::Redirect)
                return 1; // swallow native send; report success so the game proceeds
        }
    }
    FnSend orig = g_orig_send.load(std::memory_order_acquire);
    return orig ? orig(self, edx, idTo, message) : 1; // guard: never call a null orig
}

// --- DirectPlay session vtable hooks (native TCP/IP multiplayer) --------------
// EnumSessions with a null/empty host pops DPlay's "Locate Session" common dialog
// asking the user to type an IP — which stalls headless auto-nav. Substitute
// localhost so a joiner finds a same-machine host automatically. Create/Join are
// logged only.
void __fastcall hook_enum_sessions(void* self, void* edx, void* sessions, const GUID* appGuid,
                                   const char* ipAddress, char allSessions, char requirePassword)
{
    const char* useIp = (ipAddress && ipAddress[0]) ? ipAddress : "127.0.0.1";
    spdlog::info("[nettrace] EnumSessions host='{}' (orig='{}')", useIp,
                 ipAddress ? ipAddress : "(null)");
    if (FnEnumSessions orig = g_orig_enum.load(std::memory_order_acquire))
        orig(self, edx, sessions, appGuid, useIp, allSessions, requirePassword);
}

void __fastcall hook_create_session(void* self, void* edx, void** netSession, const GUID* appGuid,
                                    const char* sessionName, const char* password)
{
    spdlog::info("[nettrace] CreateSession name='{}'", sessionName ? sessionName : "(null)");
    if (FnCreateSession orig = g_orig_create.load(std::memory_order_acquire))
        orig(self, edx, netSession, appGuid, sessionName, password);
}

void __fastcall hook_join_session(void* self, void* edx, void** netSession, void* netSessionEnum,
                                  const char* password)
{
    spdlog::info("[nettrace] JoinSession");
    if (FnJoinSession orig = g_orig_join.load(std::memory_order_acquire))
        orig(self, edx, netSession, netSessionEnum, password);
}

LRESULT CALLBACK d2_wndproc_hook(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    // Capture the game's UI thread id on first WndProc call (the loader thread
    // that ran DllMain has typically exited, so its id is stale).
    const unsigned long tid = GetCurrentThreadId();
    if (g_main_thread_id.load(std::memory_order_relaxed) != tid)
        g_main_thread_id.store(tid, std::memory_order_relaxed);

    if (msg == WM_D2_DEFERRED_PACKET) {
        __try {
            drain_one_deferred_packet();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            spdlog::warn("[nettrace] deferred-packet drain caught exception — continuing");
        }
        return 0;
    }

    // The subclass is installed on a background thread, so this hook can fire before
    // g_orig_wndproc is stored — fall back to DefWindowProcA rather than crash.
    WNDPROC orig = g_orig_wndproc.load(std::memory_order_acquire);
    return orig ? CallWindowProcA(orig, h, msg, w, l) : DefWindowProcA(h, msg, w, l);
}

struct FindHwndCtx
{
    DWORD pid;
    HWND found;
};

BOOL CALLBACK find_game_hwnd(HWND h, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindHwndCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid == ctx->pid) {
        char title[64]{};
        GetWindowTextA(h, title, sizeof(title));
        if (title[0] && IsWindowVisible(h)) {
            ctx->found = h;
            return FALSE;
        }
    }
    return TRUE;
}

// Patch the 4-byte operand of a `call rel32` at callSiteVA. Verifies the opcode
// is 0xE8 first so a wrong/updated image is never corrupted.
bool patch_call_rel(uintptr_t callSiteVA, void* newTarget)
{
    if (*(std::uint8_t*)callSiteVA != 0xE8) {
        spdlog::error("[nettrace] call-site {:#x} is not a CALL (0x{:02X}); refusing to patch",
                      callSiteVA, *(std::uint8_t*)callSiteVA);
        return false;
    }
    void* operandAddr = (void*)(callSiteVA + 1);
    uintptr_t nextIP = callSiteVA + 5;
    std::int32_t newRel = (std::int32_t)((uintptr_t)newTarget - nextIP);

    DWORD oldProtect = 0;
    if (!VirtualProtect(operandAddr, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    *(std::int32_t*)operandAddr = newRel;
    VirtualProtect(operandAddr, 4, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), operandAddr, 4);
    return true;
}

// Swap one function pointer in a vtable (in .rdata, normally read-only).
bool swap_vtable_slot(uintptr_t vtableVA, int slot, void* newFn, void** outOrig)
{
    uintptr_t* slotAddr = reinterpret_cast<uintptr_t*>(vtableVA) + slot;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slotAddr, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    if (outOrig)
        *outOrig = reinterpret_cast<void*>(*slotAddr);
    *slotAddr = reinterpret_cast<uintptr_t>(newFn);
    VirtualProtect(slotAddr, sizeof(uintptr_t), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), slotAddr, sizeof(uintptr_t));
    return true;
}

// Polls for the game window, then subclasses it (capture UI thread + drain
// deferred packets). The window does not exist at DllMain time.
DWORD WINAPI subclass_window_thread(LPVOID)
{
    for (int i = 0; i < 120; ++i) { // up to ~60s
        FindHwndCtx ctx{GetCurrentProcessId(), nullptr};
        EnumWindows(&find_game_hwnd, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.found) {
            WNDPROC prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                ctx.found, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&d2_wndproc_hook)));
            g_orig_wndproc.store(prev, std::memory_order_release);
            g_main_hwnd.store(ctx.found);
            spdlog::info("[nettrace] game window {:p} subclassed (orig WndProc {:p})",
                         (void*)ctx.found, (void*)g_orig_wndproc);
            return 0;
        }
        Sleep(500);
    }
    spdlog::warn("[nettrace] no game window found in 60s — deferred-packet replay disabled");
    return 0;
}

// The window subclass (and thus g_orig_wndproc) exists ONLY for the deferred-packet
// replay + UI-thread-id capture, which only a registered RX dispatch gate (e.g. a
// relay-driven delay/exclude) uses. Spawn it lazily on the first setDispatchCallback
// so a pure logging build never subclasses the game window. Idempotent.
std::atomic<bool> g_subclass_started{false};

void ensureSubclassThread()
{
    bool expected = false;
    if (!g_subclass_started.compare_exchange_strong(expected, true))
        return;
    HANDLE th = CreateThread(nullptr, 0, &subclass_window_thread, nullptr, 0, nullptr);
    if (th)
        CloseHandle(th);
}

} // namespace

void addRxObserver(RxTraceCallback cb)
{
    const int n = g_rx_observer_count.load(std::memory_order_relaxed);
    if (cb && n < kMaxObservers) {
        g_rx_observers[n] = cb;                                       // write slot...
        g_rx_observer_count.store(n + 1, std::memory_order_release);  // ...then publish
    }
}

void addTxObserver(TxTraceCallback cb)
{
    const int n = g_tx_observer_count.load(std::memory_order_relaxed);
    if (cb && n < kMaxObservers) {
        g_tx_observers[n] = cb;
        g_tx_observer_count.store(n + 1, std::memory_order_release);
    }
}

void setDispatchCallback(RxDispatchCallback cb)
{
    g_rx_dispatch_cb.store(cb, std::memory_order_release);
    // A gate that can Defer needs the UI-thread subclass for replay; spawn it now.
    // A pure logging build never registers one, so it never subclasses the window.
    if (cb)
        ensureSubclassThread();
}

void setTxCallback(TxGateCallback cb)
{
    g_tx_gate_cb.store(cb, std::memory_order_release);
}

int recvDispatchDepth()
{
    return g_recv_dispatch_depth.load();
}

unsigned long mainThreadId()
{
    return g_main_thread_id.load(std::memory_order_relaxed);
}

bool install()
{
    if (g_installed)
        return true;
    if (!executableIsGame() || gameVersion() != GameVersion::Russobit) {
        spdlog::warn("[nettrace] not the pinned Russobit image — network hooks disabled");
        return false;
    }
    g_installed = true;

    const bool p1 = patch_call_rel(kRecvCallSite1VA, reinterpret_cast<void*>(&hook_recv_dispatch));
    const bool p2 = patch_call_rel(kRecvCallSite2VA, reinterpret_cast<void*>(&hook_recv_dispatch));

    // Capture the original send BEFORE swapping so hook_send can never observe a
    // null g_orig_send through the just-installed slot (no networking happens this
    // early, but keep the window closed regardless).
    g_orig_send.store(reinterpret_cast<FnSend>(
                          *(reinterpret_cast<uintptr_t*>(kDPlayPlayerVftVA) + kSendSlot)),
                      std::memory_order_release);
    const bool tx = swap_vtable_slot(kDPlayPlayerVftVA, kSendSlot,
                                     reinterpret_cast<void*>(&hook_send), nullptr);

    // DirectPlay session vtable (native TCP/IP): EnumSessions 127.0.0.1 + log.
    void* o = nullptr;
    if (swap_vtable_slot(kDPlayServiceVftVA, kEnumSlot, reinterpret_cast<void*>(&hook_enum_sessions),
                         &o))
        g_orig_enum.store(reinterpret_cast<FnEnumSessions>(o), std::memory_order_release);
    if (swap_vtable_slot(kDPlayServiceVftVA, kCreateSlot,
                         reinterpret_cast<void*>(&hook_create_session), &o))
        g_orig_create.store(reinterpret_cast<FnCreateSession>(o), std::memory_order_release);
    if (swap_vtable_slot(kDPlayServiceVftVA, kJoinSlot, reinterpret_cast<void*>(&hook_join_session),
                         &o))
        g_orig_join.store(reinterpret_cast<FnJoinSession>(o), std::memory_order_release);

    // NOTE: the window subclass is spawned lazily by setDispatchCallback — a pure
    // logging/observer build never subclasses the game window.

    spdlog::info("[nettrace] installed (RX call-sites: {} / {}; DPlay TX: {}; session hooks set)",
                 p1 ? "ok" : "FAIL", p2 ? "ok" : "FAIL", tx ? "ok" : "FAIL");
    return p1 && p2;
}

} // namespace nettracehooks
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV
