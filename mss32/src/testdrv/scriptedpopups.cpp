/*
 * Removable DebugTest-only exact-once popup subscriber.
 * Compile-gated by D2_TESTDRV and included only by the DebugTest project.
 */

#ifdef D2_TESTDRV

#include "testdrv/scriptedpopups.h"
#include "testdrv/autonav.h"
#include "testdrv/testdrv.h"
#include "testdrv/uistatereporter.h"
#include "button.h"
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {
namespace testdrv {
namespace scriptedpopups {

namespace {

struct PendingPopup
{
    bool valid = false;
    char dialog[48] = {};
    char button[48] = {};
    std::uint32_t appearance = 0;
    std::uint32_t owner = 0;
    unsigned messageButtonMask = 0;
    std::uint32_t observedTick = 0;
    bool observedLogged = false;
    bool battleResultClose = false;
    game::CButtonInterf* exactButton = nullptr;
    game::CBFunctorDispatch0* exactFunctor = nullptr;
};

bool g_prepared = false;
bool g_enabled = false;
bool g_confirmations = false;
bool g_active = false;
enum class StartupAdmission { Disabled, Held, ReleaseReceived, Released };
std::atomic<StartupAdmission> g_startupAdmission{StartupAdmission::Disabled};
bool g_battleActive = false;
bool g_battleResultPublished = false;
bool g_battleResultClaimed = false;
bool g_postBattleCaptureOpen = false;
char g_role[16] = {};
PendingPopup g_pending;
std::uint32_t g_lastClaimedAppearance = 0;
std::uint32_t g_lastClaimedOwner = 0;
constexpr std::uint32_t kMinimumReadyAgeMs = 300;
constexpr unsigned kMessageOk = 1u << 0;
constexpr unsigned kMessageYes = 1u << 1;
constexpr unsigned kMessageNo = 1u << 2;

[[noreturn]] void failFast(const char* reason, unsigned exitCode)
{
    spdlog::critical(
        "[testdrv][scripted-popup] terminal invariant failure: {}; terminating",
        reason);
    spdlog::default_logger()->flush();
    TerminateProcess(GetCurrentProcess(), exitCode);
    std::abort();
}

bool sameIdentity(std::uint32_t appearance, std::uint32_t owner,
                  const PendingPopup& pending)
{
    return pending.valid && pending.appearance == appearance
           && pending.owner == owner;
}

bool alreadyClaimed(std::uint32_t appearance, std::uint32_t owner)
{
    return appearance == g_lastClaimedAppearance && owner == g_lastClaimedOwner;
}

bool isOutsideBattleCandidate(const char* dialogName, const char* buttonName)
{
    return (lstrcmpA(dialogName, "DLG_SCENARIO_BRIEFING") == 0
            && lstrcmpA(buttonName, "BTN_CONTINUE") == 0)
           || (lstrcmpA(dialogName, "DLG_BEGIN_TURN") == 0
               && lstrcmpA(buttonName, "BTN_OK") == 0)
           || (lstrcmpA(dialogName, "DLG_GETINFO_BOX") == 0
               && lstrcmpA(buttonName, "BTN_CLOSE") == 0)
           || (lstrcmpA(dialogName, "DLG_EVENT_POPUP") == 0
               && lstrcmpA(buttonName, "BTN_RIGHTSIDE") == 0)
           || (lstrcmpA(dialogName, "DLG_MESSAGE_BOX") == 0
               && (lstrcmpA(buttonName, "BTN_OK") == 0
                   || (g_confirmations
                       && (lstrcmpA(buttonName, "BTN_YES") == 0
                           || lstrcmpA(buttonName, "BTN_NO") == 0))))
           // The stock battle-result close can reveal a ready strategic map
           // for one or more natural frames before its loot/stack-management
           // successor is constructed.  The green lobby DLL kept its
           // persistent dismiss subscriber alive across that gap.  Admit the
           // same safe successors outside g_battleActive so their first exact
           // appearance is still claimed once; no timer, retry, or fallback is
           // introduced here.
           || (lstrcmpA(dialogName, "DLG_MANAGE_STACK") == 0
               && lstrcmpA(buttonName, "BTN_CLOSE") == 0)
           || (lstrcmpA(dialogName, "DLG_ITEM") == 0
               && lstrcmpA(buttonName, "BTN_OK") == 0);
}

bool isBattleCandidate(const char* dialogName, const char* buttonName)
{
    if (lstrcmpA(dialogName, "DLG_BATTLE_A") == 0)
        return lstrcmpA(buttonName, "BTN_CLOSE") == 0;
    if (g_postBattleCaptureOpen
        && lstrcmpA(dialogName, "DLG_EVENT_POPUP") == 0)
        return lstrcmpA(buttonName, "BTN_RIGHTSIDE") == 0;
    if (lstrcmpA(dialogName, "DLG_MANAGE_STACK") == 0)
        return lstrcmpA(buttonName, "BTN_CLOSE") == 0;
    if (lstrcmpA(dialogName, "DLG_ITEM") == 0)
        return lstrcmpA(buttonName, "BTN_OK") == 0;
    if (lstrcmpA(dialogName, "DLG_MESSAGE_BOX") != 0)
        return false;
    // onDialogBound runs only after the stock assignFunctor helper returned,
    // therefore the first of these observed binds is the first real choice.
    return lstrcmpA(buttonName, "BTN_OK") == 0
           || lstrcmpA(buttonName, "BTN_YES") == 0
           || lstrcmpA(buttonName, "BTN_NO") == 0;
}

unsigned messageButtonBit(const char* buttonName)
{
    if (lstrcmpA(buttonName, "BTN_OK") == 0)
        return kMessageOk;
    if (lstrcmpA(buttonName, "BTN_YES") == 0)
        return kMessageYes;
    if (lstrcmpA(buttonName, "BTN_NO") == 0)
        return kMessageNo;
    return 0;
}

void logObserved(PendingPopup& pending)
{
    if (pending.observedLogged)
        return;
    pending.observedLogged = true;
    spdlog::info(
        "[testdrv][scripted-popup] OBSERVED role={} dialog={} appearance={} owner={} "
        "button={} tick={}",
        g_role, pending.dialog, pending.appearance, pending.owner,
        pending.button, static_cast<unsigned long long>(GetTickCount64()));
    spdlog::default_logger()->flush();
}

bool readyCurrentIdentity(const char* dialogName, std::uint32_t& appearance,
                          std::uint32_t& owner)
{
    std::uint32_t ignoredAge = 0;
    return uistatereporter::getReadyCurrentDialogInstanceAge(
        dialogName, appearance, owner, ignoredAge);
}

void releaseBattleGateOnReadyBareMap()
{
    if (!g_battleActive)
        return;
    const char* current = uistatereporter::currentDialogName();
    if (!current
        || (lstrcmpA(current, "DLG_STRATEGIC") != 0
            && lstrcmpA(current, "DLG_ISO_PAL") != 0))
        return;

    std::uint32_t appearance = 0;
    std::uint32_t owner = 0;
    if (!readyCurrentIdentity(current, appearance, owner))
        return;
    if (g_pending.valid)
        failFast("ready bare map retired a captured battle popup before its sole action",
                 0xD2E77346u);
    g_battleActive = false;
    g_battleResultPublished = false;
    g_battleResultClaimed = false;
    g_postBattleCaptureOpen = false;
    spdlog::info(
        "[testdrv][scripted-popup] battle gate released on ready bare map dialog={} "
        "appearance={} owner={}",
        current, appearance, owner);
}

} // namespace

bool preflight(bool enabled, bool confirmations, const char* role)
{
    if (g_prepared)
        return g_enabled == enabled && g_confirmations == confirmations
               && (!enabled || (role && lstrcmpA(g_role, role) == 0));

    g_enabled = enabled;
    g_confirmations = confirmations;
    if (confirmations && !enabled) {
        spdlog::error(
            "[testdrv][scripted-popup] confirmation ownership requires "
            "D2TESTDRV_SCRIPTED_POPUPS");
        return false;
    }
    if (!enabled) {
        g_prepared = true;
        return true;
    }
    if (!role
        || (lstrcmpA(role, "host") != 0 && lstrcmpA(role, "join") != 0)) {
        spdlog::error(
            "[testdrv][scripted-popup] D2TESTDRV_SCRIPTED_POPUPS requires exact "
            "D2TESTDRV_ROLE=host or join");
        return false;
    }
    lstrcpynA(g_role, role, sizeof(g_role));
    g_prepared = true;
    spdlog::info(
        "[testdrv][scripted-popup] preflight passed role={} minReadyAgeMs={} "
        "confirmations={}",
        g_role, kMinimumReadyAgeMs, g_confirmations);
    return true;
}

void activateAfterHooks()
{
    if (!g_enabled)
        return;
    if (!g_prepared)
        failFast("activation without successful preflight", 0xD2E77340u);
    g_active = true;
    g_startupAdmission.store(StartupAdmission::Held, std::memory_order_release);
    spdlog::info(
        "[testdrv][scripted-popup] active role={} exactOnce=true persistent=true",
        g_role);
}

bool startupActionsHeld()
{
    const auto state = g_startupAdmission.load(std::memory_order_acquire);
    return state == StartupAdmission::Held || state == StartupAdmission::ReleaseReceived;
}

void receiveStartupRelease(std::uint32_t payloadSize)
{
    StartupAdmission expected = StartupAdmission::Held;
    if (payloadSize != 0 || !g_startupAdmission.compare_exchange_strong(
            expected, StartupAdmission::ReleaseReceived, std::memory_order_acq_rel))
        failFast("startup release was malformed, duplicated, or outside the paired popup mode",
                 0xD2E77360u);
}

void onDialogBound(const char* dialogName, const char* buttonName,
                   std::uint32_t appearance, std::uint32_t ownerInstance,
                   game::CButtonInterf* exactButton)
{
    if (!g_active || !dialogName || !buttonName)
        return;

    if (lstrcmpA(dialogName, "DLG_BATTLE_A") == 0 && !g_battleActive) {
        g_battleActive = true;
        g_battleResultPublished = false;
        g_battleResultClaimed = false;
        g_postBattleCaptureOpen = false;
    }

    // A battle-result BTN_CLOSE callback can bind DLG_STRATEGIC and then bind
    // a loot/manage/message successor before returning to this outer frame.
    // Do not retire the battle/post-battle gate merely on that intermediate
    // bind: releaseBattleGateOnReadyBareMap() owns retirement only after a
    // natural frame proves the map itself is the ready topmost screen.

    // IDA proof for exact Russobit:
    // IBatViewer::battleEnd 0x632016 calls sub_638B41. That transition creates
    // BTN_CLOSE and, at 0x638CE4, successfully binds stock sub_6353CB through
    // the canonical assignFunctor seam. The live-battle hidden button emits no
    // successful bind, so this event is the result publication itself.
    const bool battleResultClose =
        lstrcmpA(dialogName, "DLG_BATTLE_A") == 0
        && lstrcmpA(buttonName, "BTN_CLOSE") == 0;
    const bool candidate = g_battleActive
        ? isBattleCandidate(dialogName, buttonName)
        : isOutsideBattleCandidate(dialogName, buttonName);
    if (!candidate)
        return;
    if (appearance == 0 || ownerInstance == 0)
        failFast("eligible bound button had no exact appearance/owner identity",
                 0xD2E77341u);
    game::CBFunctorDispatch0* exactFunctor = nullptr;
    if (battleResultClose) {
        bool callbackValid = false;
        __try {
            exactFunctor = exactButton && exactButton->buttonData
                ? exactButton->buttonData->onClickedFunctor.data
                : nullptr;
            callbackValid = exactFunctor && exactFunctor->vftable
                            && exactFunctor->vftable->runCallback;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            failFast("battle-result bind exact-button inspection fault",
                     0xD2E7734Au);
        }
        if (!callbackValid)
            failFast("battle-result bind had no exact stock callback",
                     0xD2E7734Bu);
    }
    if (battleResultClose && g_battleResultPublished)
        failFast("battle-result BTN_CLOSE was published more than once",
                 0xD2E7734Cu);
    if (alreadyClaimed(appearance, ownerInstance))
        return;
    const bool rankedMessage = lstrcmpA(dialogName, "DLG_MESSAGE_BOX") == 0
        && (g_battleActive || g_confirmations);
    if (g_pending.valid && !sameIdentity(appearance, ownerInstance, g_pending))
        failFast("captured popup retired before its sole action", 0xD2E77342u);
    if (g_pending.valid) {
        if (rankedMessage)
            g_pending.messageButtonMask |= messageButtonBit(buttonName);
        return;
    }

    g_pending.valid = true;
    lstrcpynA(g_pending.dialog, dialogName, sizeof(g_pending.dialog));
    g_pending.appearance = appearance;
    g_pending.owner = ownerInstance;
    if (battleResultClose) {
        g_battleResultPublished = true;
        g_battleResultClaimed = false;
        g_pending.observedTick = GetTickCount();
        g_pending.battleResultClose = true;
        g_pending.exactButton = exactButton;
        g_pending.exactFunctor = exactFunctor;
    }
    if (rankedMessage) {
        // Accumulate every actually bound choice during construction. Once the
        // dialog is ready, resolve the literal legacy priority OK -> YES -> NO.
        g_pending.messageButtonMask = messageButtonBit(buttonName);
        lstrcpynA(g_pending.button, buttonName, sizeof(g_pending.button));
        // OBSERVED is the first eligible real bind. CLAIMED may name a higher
        // priority button discovered later in this same construction batch.
        logObserved(g_pending);
    } else {
        lstrcpynA(g_pending.button, buttonName, sizeof(g_pending.button));
        logObserved(g_pending);
    }
}

void onBattleResultCloseClaimed(std::uint32_t appearance,
                                std::uint32_t ownerInstance)
{
    if (!g_active)
        return;
    if (!g_battleActive || appearance == 0 || ownerInstance == 0)
        failFast("battle-result close claim did not belong to an active exact battle",
                 0xD2E77348u);
    if (!g_battleResultPublished || g_battleResultClaimed
        || appearance != g_lastClaimedAppearance
        || ownerInstance != g_lastClaimedOwner)
        failFast("battle-result close claim did not match its sole publication",
                 0xD2E77350u);
    g_battleResultClaimed = true;
    g_postBattleCaptureOpen = true;
    spdlog::info(
        "[testdrv][scripted-popup] post-battle capture opened appearance={} owner={} tick={}",
        appearance, ownerInstance,
        static_cast<unsigned long long>(GetTickCount64()));
    spdlog::default_logger()->flush();
}

void tick()
{
    if (!g_active)
        return;

    if (g_startupAdmission.load(std::memory_order_acquire)
        == StartupAdmission::ReleaseReceived) {
        if (!testdrv::mapLoaded())
            failFast("paired startup release reached a client without its loaded scenario",
                     0xD2E77361u);
        g_startupAdmission.store(StartupAdmission::Released, std::memory_order_release);
        spdlog::info("[testdrv][scripted-popup] paired startup release applied role={}", g_role);
    }

    releaseBattleGateOnReadyBareMap();
    if (!g_pending.valid)
        return;

    std::uint32_t bindAgeMs = 0;
    if (!uistatereporter::getReadyDialogInstanceAge(
            g_pending.dialog, g_pending.appearance, g_pending.owner,
            bindAgeMs)) {
        // A construction frame is expected to be not-ready. Once a different
        // exact owner becomes ready, however, the captured action is retired
        // and cannot be issued against anything else.
        const char* current = uistatereporter::currentDialogName();
        std::uint32_t currentAppearance = 0;
        std::uint32_t currentOwner = 0;
        if (current && current[0]
            && readyCurrentIdentity(current, currentAppearance, currentOwner)
            && (currentAppearance != g_pending.appearance
                || currentOwner != g_pending.owner))
            failFast("captured popup identity retired before its sole action",
                     0xD2E77343u);
        return;
    }
    const std::uint32_t settleAgeMs = g_pending.battleResultClose
        ? GetTickCount() - g_pending.observedTick
        : bindAgeMs;
    if (settleAgeMs < kMinimumReadyAgeMs)
        return;
    // Briefing is navigation into the map. Every other automatic startup
    // action remains observed, not claimed, until both clients have loaded it.
    if (startupActionsHeld()
        && !(lstrcmpA(g_pending.dialog, "DLG_SCENARIO_BRIEFING") == 0
             && lstrcmpA(g_pending.button, "BTN_CONTINUE") == 0))
        return;
    if (g_pending.battleResultClose
        && !uistatereporter::isReadyBattleResultCloseInstance(
            g_pending.appearance, g_pending.owner, g_pending.exactButton,
            g_pending.exactFunctor))
        failFast("published battle-result BTN_CLOSE was not actionable after settle",
                 0xD2E77351u);

    if (g_pending.messageButtonMask != 0) {
        const char* selected = nullptr;
        if ((g_pending.messageButtonMask & kMessageOk) != 0)
            selected = "BTN_OK";
        else if ((g_pending.messageButtonMask & kMessageYes) != 0)
            selected = "BTN_YES";
        else if ((g_pending.messageButtonMask & kMessageNo) != 0)
            selected = "BTN_NO";
        if (!selected)
            failFast("ready battle message had no captured OK/YES/NO bind",
                     0xD2E77347u);
        lstrcpynA(g_pending.button, selected, sizeof(g_pending.button));
    }

    // Copy and clear before enqueue. A callback may synchronously bind the next
    // popup; that successor must remain independently captured and must never
    // be cleared by completion of this older generation.
    const PendingPopup action = g_pending;
    g_pending = PendingPopup{};
    g_lastClaimedAppearance = action.appearance;
    g_lastClaimedOwner = action.owner;
    autonav::claimAndEnqueueScriptedPopupAction(
        action.dialog, action.button, action.appearance, action.owner,
        settleAgeMs, action.exactButton, action.exactFunctor);
}

} // namespace scriptedpopups
} // namespace testdrv
} // namespace hooks

#endif // D2_TESTDRV

