/*
 * Predicate wrappers are installed once. They fail closed while lobby policy
 * is unresolved, delegate in explicit stock mode, and expose overlay ownership
 * only after a simultaneous SessionPlan has been applied.
 */

#include "simturns/engine_hooks.h"
#include "midgardid.h"
#include "midserverlogic.h"
#include "mqpresentationmanager.h"
#include "netintercept.h"
#include "phasegame.h"
#include "rendererimpl.h"
#include "simturns/controller.h"
#include "simturns/state.h"
#include "task.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks::simturns::engine_hooks {

namespace {

constexpr std::uintptr_t forceActiveAddress = 0x406394;
constexpr std::uintptr_t isCurrentPlayerAddress = 0x41E77F;
constexpr std::uintptr_t getCurrentTaskAddress = 0x5C9FF8;
constexpr std::uintptr_t setCurrentTaskAddress = 0x5C9FC8;
constexpr std::uintptr_t strategicTaskHolderVftable = 0x6D6974;
constexpr std::uintptr_t waitTaskVftable = 0x6DCBEC;

constexpr std::array<std::uint8_t, 7> forceActiveBytes{{
    0x8B, 0x41, 0x10, 0x8A, 0x40, 0x28, 0xC3}};
constexpr std::array<std::uint8_t, 34> isCurrentPlayerBytes{{
    0x8B, 0x4C, 0x24, 0x04, 0xE8, 0x9E, 0x20, 0x00, 0x00, 0x85, 0xC0,
    0x75, 0x04, 0x32, 0xC0, 0xEB, 0x0E, 0x8B, 0x4C, 0x24, 0x08, 0x8B,
    0x40, 0x04, 0x2B, 0x01, 0xF7, 0xD8, 0x1B, 0xC0, 0x40, 0xC2, 0x08,
    0x00}};
constexpr std::array<std::uint8_t, 16> getCurrentTaskBytes{{
    0x56, 0x57, 0x8B, 0xF9, 0x33, 0xC0, 0x8B, 0x77,
    0x04, 0x39, 0x46, 0x04, 0x0F, 0x94, 0xC0, 0x84}};
constexpr std::array<std::uint8_t, 16> setCurrentTaskBytes{{
    0x83, 0x7C, 0x24, 0x04, 0x00, 0x74, 0x11, 0x8B,
    0x49, 0x04, 0xFF, 0x74, 0x24, 0x04, 0x83, 0xC1}};

// Exact-Russobit views, verified in 5C9FC8/5C9FF8, 48D933 and 4D5901.
// The shared CTaskManagerData declaration does not describe these fields.
struct StrategicTaskHolder
{
    const void* vftable;
    std::uint8_t unused[0xEC];
    int turnMode;
};

struct NativeTaskManager
{
    struct Data
    {
        StrategicTaskHolder* holder;
        game::ITask* currentTask;
    };
    const void* vftable;
    Data* data;
};

struct NativeWaitTask
{
    struct Data
    {
        void* unused[2];
        void* pendingCommand;
        game::CPhaseGame* phaseGame;
    };
    game::ITask task;
    std::uint8_t unused[0x1C];
    Data* data;
};

static_assert(offsetof(StrategicTaskHolder, turnMode) == 0xF0);
static_assert(offsetof(NativeTaskManager, data) == 4);
static_assert(offsetof(NativeTaskManager::Data, currentTask) == 4);
static_assert(offsetof(NativeWaitTask, data) == 0x20);
static_assert(offsetof(NativeWaitTask::Data, pendingCommand) == 8);
static_assert(offsetof(NativeWaitTask::Data, phaseGame) == 0xC);
static_assert(offsetof(game::CMqPresentationManagerData, renderingFrame) == 0x44);
static_assert(offsetof(game::CRendererImpl, renderingInProcess) == 0x68);

using ForceActive = bool(__thiscall*)(void* thisptr);
using IsCurrentPlayer = bool(__stdcall*)(game::CMidServerLogic* logic,
                                         const game::CMidgardID* playerId);
using GetCurrentTask = game::ITask*(__thiscall*)(NativeTaskManager* manager);
using SetCurrentTask = void(__thiscall*)(NativeTaskManager* manager, game::ITask* task);

ForceActive g_forceActiveOriginal = nullptr;
IsCurrentPlayer g_isCurrentPlayerOriginal = nullptr;
GetCurrentTask g_getCurrentTaskOriginal = nullptr;
thread_local bool g_refreshingTask = false;

game::ITask* __fastcall getCurrentTaskHooked(NativeTaskManager* manager, void* /*edx*/)
{
    auto* task = g_getCurrentTaskOriginal(manager);
    if (g_refreshingTask || !task || phase() != Phase::Independent
        || reinterpret_cast<std::uintptr_t>(task->vftable) != waitTaskVftable
        || GetCurrentThreadId() != netintercept::mainThreadId()
        || netintercept::recvDispatchDepth() != 0
        || !localActionAdmission(localHandle())) {
        return task;
    }

    auto* holder = manager->data->holder;
    auto* wait = reinterpret_cast<NativeWaitTask*>(task);
    if (!holder || reinterpret_cast<std::uintptr_t>(holder->vftable)
                       != strategicTaskHolderVftable
        || holder->turnMode != 1 || !wait->data || wait->data->pendingCommand
        || !wait->data->phaseGame
        || !wait->data->phaseGame->data
        || !wait->data->phaseGame->data->midObjectLock
        || game::CPhaseGameApi::get().checkObjectLock(wait->data->phaseGame)) {
        return task;
    }

    // Cursor rendering also calls this getter. Destroying Wait there frees its
    // images while the renderer refuses to unregister their animations.
    const auto& presentationApi = game::CMqPresentationManagerApi::get();
    game::PresentationMgrPtr presentation{};
    presentationApi.getPresentationManager(&presentation);
    const auto* data = presentation.data ? presentation.data->data : nullptr;
    const bool canRefresh = data && data->renderer && !data->renderingFrame
                            && !data->renderer->renderingInProcess;
    presentationApi.presentationMgrPtrSetData(&presentation, nullptr);
    if (!canRefresh) {
        return task;
    }

    // TurnInfo can cache a non-notifying CTaskWait before admission opens.
    // A later input getter outside presentation/render replaces it and delivers
    // that same input to the new task; no queued pointer or synthetic input.
    struct RefreshScope
    {
        RefreshScope() { g_refreshingTask = true; }
        ~RefreshScope() { g_refreshingTask = false; }
    } refreshScope;
    reinterpret_cast<SetCurrentTask>(setCurrentTaskAddress)(manager, nullptr);
    auto* refreshed = g_getCurrentTaskOriginal(manager);
    spdlog::info("[simturns] refreshed strategic turn-wait task after local admission opened");
    return refreshed;
}

enum class PredicatePolicy : std::uint8_t
{
    Stock,
    Overlay,
    Block,
};

constexpr PredicatePolicy localUiPolicy(Phase current)
{
    switch (current) {
    case Phase::Disabled:
    case Phase::Stock:
    case Phase::Merged:
        return PredicatePolicy::Stock;
    case Phase::Independent:
        return PredicatePolicy::Overlay;
    case Phase::Prepared:
    case Phase::WaitingForSession:
    case Phase::Ready:
    case Phase::Held:
    case Phase::Merging:
    case Phase::AwaitingStockTurn:
    case Phase::Closing:
    case Phase::Faulted:
        return PredicatePolicy::Block;
    }
    return PredicatePolicy::Block;
}

constexpr PredicatePolicy serverMutationPolicy(Phase current)
{
    switch (current) {
    case Phase::Disabled:
    case Phase::Stock:
    case Phase::Merged:
        return PredicatePolicy::Stock;
    case Phase::Independent:
    case Phase::Held:
    case Phase::Merging:
        return PredicatePolicy::Overlay;
    case Phase::Prepared:
    case Phase::WaitingForSession:
    case Phase::Ready:
    case Phase::AwaitingStockTurn:
    case Phase::Closing:
    case Phase::Faulted:
        return PredicatePolicy::Block;
    }
    return PredicatePolicy::Block;
}

static_assert(localUiPolicy(Phase::Disabled) == PredicatePolicy::Stock);
static_assert(localUiPolicy(Phase::Stock) == PredicatePolicy::Stock);
static_assert(localUiPolicy(Phase::Merged) == PredicatePolicy::Stock);
static_assert(localUiPolicy(Phase::Independent) == PredicatePolicy::Overlay);
static_assert(localUiPolicy(Phase::Prepared) == PredicatePolicy::Block);
static_assert(localUiPolicy(Phase::WaitingForSession) == PredicatePolicy::Block);
static_assert(localUiPolicy(Phase::Ready) == PredicatePolicy::Block);
static_assert(localUiPolicy(Phase::Held) == PredicatePolicy::Block);
static_assert(localUiPolicy(Phase::Merging) == PredicatePolicy::Block);
static_assert(localUiPolicy(Phase::AwaitingStockTurn) == PredicatePolicy::Block);
static_assert(localUiPolicy(Phase::Faulted) == PredicatePolicy::Block);
static_assert(localUiPolicy(Phase::Closing) == PredicatePolicy::Block);
static_assert(serverMutationPolicy(Phase::Disabled) == PredicatePolicy::Stock);
static_assert(serverMutationPolicy(Phase::Stock) == PredicatePolicy::Stock);
static_assert(serverMutationPolicy(Phase::Merged) == PredicatePolicy::Stock);
static_assert(serverMutationPolicy(Phase::Independent) == PredicatePolicy::Overlay);
static_assert(serverMutationPolicy(Phase::Held) == PredicatePolicy::Overlay);
static_assert(serverMutationPolicy(Phase::Merging) == PredicatePolicy::Overlay);
static_assert(serverMutationPolicy(Phase::Prepared) == PredicatePolicy::Block);
static_assert(serverMutationPolicy(Phase::WaitingForSession) == PredicatePolicy::Block);
static_assert(serverMutationPolicy(Phase::Ready) == PredicatePolicy::Block);
static_assert(serverMutationPolicy(Phase::AwaitingStockTurn) == PredicatePolicy::Block);
static_assert(serverMutationPolicy(Phase::Faulted) == PredicatePolicy::Block);
static_assert(serverMutationPolicy(Phase::Closing) == PredicatePolicy::Block);

bool __fastcall forceActiveHooked(void* thisptr, void* /*edx*/)
{
    switch (localUiPolicy(phase())) {
    case PredicatePolicy::Overlay:
        // Independent is also used while the day-1 bootstrap is still being
        // prepared. The controller admission seam opens only after the
        // relay's global BootstrapReleased and closes again after EndTurn.
        return localActionAdmission(localHandle());
    case PredicatePolicy::Block:
        return false;
    case PredicatePolicy::Stock:
        return g_forceActiveOriginal ? g_forceActiveOriginal(thisptr) : false;
    }
    return false;
}

bool __stdcall isCurrentPlayerHooked(game::CMidServerLogic* logic,
                                     const game::CMidgardID* playerId)
{
    switch (serverMutationPolicy(phase())) {
    case PredicatePolicy::Overlay: {
        if (!isHost())
            return g_isCurrentPlayerOriginal(logic, playerId);
        const std::uint32_t candidate = playerId
                                            ? static_cast<std::uint32_t>(playerId->value)
                                            : 0;
        if (candidate != 0
            && (candidate == hostHandle() || candidate == joinHandle())) {
            return true;
        }
        // AI and any unexpected third-party id retain the stock predicate.
        // Session activation separately proves that exactly two humans match
        // the negotiated pair before these overlay phases can be entered.
        return g_isCurrentPlayerOriginal
                   ? g_isCurrentPlayerOriginal(logic, playerId)
                   : false;
    }
    case PredicatePolicy::Block:
        return false;
    case PredicatePolicy::Stock:
        return g_isCurrentPlayerOriginal ? g_isCurrentPlayerOriginal(logic, playerId) : false;
    }
    return false;
}

template <std::size_t N>
bool matches(std::uintptr_t address, const std::array<std::uint8_t, N>& expected,
             const char* name)
{
    if (std::memcmp(reinterpret_cast<const void*>(address), expected.data(), expected.size()) == 0)
        return true;
    spdlog::error("[simturns] detour preflight mismatch for {} at {:#x}", name, address);
    return false;
}

} // namespace

bool preflight()
{
    bool result = matches(forceActiveAddress, forceActiveBytes,
                          "force-active predicate");
    result = matches(getCurrentTaskAddress, getCurrentTaskBytes, "get-current-task")
             && result;
    result = matches(setCurrentTaskAddress, setCurrentTaskBytes, "set-current-task")
             && result;
    result = matches(isCurrentPlayerAddress, isCurrentPlayerBytes,
                     "isCurrentPlayer")
             && result;
    return result;
}

void append(Hooks& hooks)
{
    hooks.push_back(HookInfo{reinterpret_cast<void*>(forceActiveAddress),
                             reinterpret_cast<void*>(&forceActiveHooked),
                             reinterpret_cast<void**>(&g_forceActiveOriginal)});
    hooks.push_back(HookInfo{reinterpret_cast<void*>(getCurrentTaskAddress),
                             reinterpret_cast<void*>(&getCurrentTaskHooked),
                             reinterpret_cast<void**>(&g_getCurrentTaskOriginal)});
    hooks.push_back(HookInfo{reinterpret_cast<void*>(isCurrentPlayerAddress),
                             reinterpret_cast<void*>(&isCurrentPlayerHooked),
                             reinterpret_cast<void**>(&g_isCurrentPlayerOriginal)});
}

} // namespace hooks::simturns::engine_hooks
