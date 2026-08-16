/*
 * C4dll-R timer host: ports timer.mod's off[] game-hook layer (dialog/button capture, combat/anim
 * classification, on-elapse actions) into the renderer DLL; exposed to timer.c4p via C4P_Host.
 * Russobit "- Copy" exe ONLY: addresses hardcoded (base 0x400000, reloc 0), like featuremenu.cpp.
 * Detours run on the game UI thread, plugin polls from the worker -> shared fields volatile, derefs SEH-guarded.
 */
#include <windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <detours.h>
#include "c4plugin.h"

// In pluginhost.cpp; bumped by the off[6] turn-info hook (a NETWORK msg firing on EVERY client),
// so turn-starts are detected even on a pure client (MP joiner) with no server.
extern "C" void pluginhost_bump_turn(int player);
extern "C" void pluginhost_turn_reset(void);
// Client-valid scenario-day source (featuremenu.cpp); logged per turn edge to verify the per-day budget.
extern "C" int featuremenu_current_day(void);
// "Is it my turn" = CPhaseGameData.clientTakesTurn (sub-dialog-immune); -1 if unavailable -> fallback.
extern "C" int featuremenu_my_turn(void);
// Exact native showAttackEffect -> CAnimCounter completion interval from featuremenu's shared battle
// hook. Unlike BTN_DEFEND enabled state, it stays correct for special and continuation actions.
extern "C" int featuremenu_battle_animation_active(void);
// True only after the signature-gated Choose/Result controller lifecycle is installed.
extern "C" int featuremenu_battle_timer_lifecycle_available(void);
extern "C" int timerhost_force_auto_battle(void);
// Shared diagnostics gate ([menu] debugLog / C4DLL_DEBUG): OFF by default in release.
extern "C" int featuremenu_debug_enabled(void);

namespace {

// C4menu-<pid>.log next to the exe (featuremenu.cpp's mlog writes the same file).
const char* exeDirFile(const char* leaf)
{
    static char base[MAX_PATH] = {};
    if (!base[0]) {
        GetModuleFileNameA(nullptr, base, sizeof(base));
        char* slash = strrchr(base, '\\');
        if (slash)
            slash[1] = 0;
        else
            base[0] = 0;
    }
    static char path[MAX_PATH];
    lstrcpynA(path, base, sizeof(path));
    lstrcatA(path, leaf);
    return path;
}

void tlog(const char* fmt, ...)
{
    if (!featuremenu_debug_enabled()) // release stays silent: no C4menu-<pid>.log unless asked
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    char line[600];
    int n = wsprintfA(line, "%s\r\n", buf);
    static char logName[40] = {0};
    if (!logName[0]) wsprintfA(logName, "C4menu-%lu.log", GetCurrentProcessId()); // per-process (MP host/client split)
    HANDLE h = CreateFileA(exeDirFile(logName), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)n, &w, nullptr);
        CloseHandle(h);
    }
}

bool isUserPtr(const void* p)
{
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x7FFF0000;
}

// Verified Russobit CMidgard local-player accessor. IDA shows exactly:
//   mov eax,[ecx+8]       ; CMidgard.data
//   mov eax,[eax+20h]     ; CMidgardData.netPlayerClientPtr (+32, not +28)
//   add eax,8             ; &NetClientPtrIdPair.second
// Calling the native accessor avoids duplicating that layout in two C4 consumers. Its exact bytes
// are gated during install; unsupported executables remain fail-closed.
constexpr uintptr_t kLocalNetworkPlayerIdAccessor = 0x403384;
constexpr int kInvalidMidgardId = 0x003F0000;
const unsigned char kLocalNetworkPlayerIdSignature[10] = {
    0x8B, 0x41, 0x08, 0x8B, 0x40, 0x20, 0x83, 0xC0, 0x08, 0xC3};
LONG volatile g_localNetworkPlayerIdAccessorAvailable = 0;

bool validateLocalNetworkPlayerIdAccessor()
{
    __try {
        return memcmp(reinterpret_cast<const void*>(kLocalNetworkPlayerIdAccessor),
                      kLocalNetworkPlayerIdSignature,
                      sizeof(kLocalNetworkPlayerIdSignature)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readLocalNetworkPlayerId(int* rawId)
{
    if (!rawId)
        return false;
    *rawId = -1;
    if (!InterlockedExchangeAdd(&g_localNetworkPlayerIdAccessorAvailable, 0))
        return false;
    __try {
        char* mid = reinterpret_cast<char*(__cdecl*)()>(0x401D35)();
        int* idPtr = isUserPtr(mid)
            ? reinterpret_cast<int*(__thiscall*)(void*)>(kLocalNetworkPlayerIdAccessor)(mid)
            : nullptr;
        if (!isUserPtr(idPtr))
            return false;
        const int id = *idPtr;
        if (id == 0 || id == -1)
            return false;
        *rawId = id;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeBytes(uintptr_t va, const void* bytes, size_t len)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(va), len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(reinterpret_cast<void*>(va), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(va), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(va), len);
    return true;
}

// Write detour into a vtable slot, return the original method pointer.
void* patchVtableSlot(uintptr_t vtableVA, unsigned byteOff, void* detour)
{
    void* orig = *reinterpret_cast<void**>(vtableVA + byteOff);
    writeBytes(vtableVA + byteOff, &detour, sizeof(detour));
    return orig;
}

// Captured state: written by game-thread detours, read by the worker via accessors.
struct State
{
    // CButtonInterf* captured by (dialog, button) name in off[9]; nulled in off[8] on destroy.
    void* volatile endTurn;   // DLG_STRATEGIC / BTN_END_TURN  (legacy 10008330)
    void* volatile capBack;   // DLG_CAPITAL / BTN_BACK        (10008334)
    void* volatile diploBack; // DLG_DIPLOMACY / BTN_BACK      (10008338)
    void* volatile briefCont; // DLG_SCENARIO_BRIEFING / BTN_CONTINUE (1000833C)
    void* volatile btnRetreat; // DLG_BATTLE_* / BTN_RETREAT   (10008340)
    void* volatile btnDefend;  // DLG_BATTLE_* / BTN_DEFEND    (10008344) -> drives anim + PvP
    void* volatile btnClose;   // DLG_BATTLE_* / BTN_CLOSE     (10008348)
    void* volatile btnResolve; // DLG_BATTLE_* / BTN_RESOLVE   (1000834C)
    // combat / animation pause inputs (mirror 10008054 / 10008058 / 1000805C)
    LONG volatile animFlag;   // a battle attack animation is playing (BTN_DEFEND hidden)
    LONG volatile pvpFlag;    // human-vs-human battle
    LONG volatile anyCombat;  // any battle present
    LONG volatile pendingEndDay;       // queued by plugin on elapse; consumed on game thread (pump)
    LONG volatile pendingEndDayBattle; // End Day must wait for BTN_CLOSE, then strategic UI
    LONG volatile battleClosePressed;  // BTN_CLOSE is one-shot for the resolved battle
    LONG volatile strategicReadyTicks; // require a stable restored strategic topmost UI
    LONG volatile inAction;       // local re-entry guard around the game-thread press
    // Original timer.mod's separate one-shot off[13] flag: consumed only by the END_TURN
    // confirmation-query callsite, so an automatic timeout never opens X005TA0000.
    LONG volatile suppressEndTurnConfirm;
    // DLG_BEGIN_TURN's real BTN_OK callback increments this only after the game has accepted the
    // acknowledgement. The plugin uses the edge, rather than dialog visibility, as its clock start.
    LONG volatile beginTurnAckSerial;
    // Published at the accepted CTaskBattle ChooseAction controller boundary. -1 is deliberately
    // fail-closed: the timer freezes instead of charging the wrong client on an unknown layout.
    LONG volatile battleTurnActive;
    LONG volatile battleContinuation;
    LONG volatile battleSelectionOpen;
    LONG volatile battleSelectionPending; // accepted Choose; opens only after downstream UI returns
    LONG volatile battlePlaybackLocal; // current broadcast Result: local=1 / remote=0 / none=-1
    LONG volatile battleInstance;
    LONG volatile battleKindPublished;
    LONG volatile battleStateSeq; // even=stable, odd=game-thread publication in progress
    void* volatile battleViewer; // embedded IBatViewer* for the current battle instance
    // A timeout request can remain deferred through an in-flight animation / second attack. Once
    // forceAutoActive flips to 1 it is never cleared by Reset/config/cancel_elapse.
    LONG volatile forceAutoRequested;
    LONG volatile forceAutoActive;
    LONG volatile forceAutoKickPending;
    void* g_orig_dlgCreate;
    void* g_orig_btnDtor;
    void* g_orig_scenInit;
    void* g_orig_turnInfo;
    void* g_origConfirmQuery;
    void* g_origBeginTurnOk;
    void* g_origAutoBattleToggle;
    void* g_origBattleSubmit;
    int lastTurnPlayer;     // last off[6] player byte (debounce: ignore same-player bursts)
    int confirmHookInstalled;
    int beginTurnAckHookInstalled;
    int autoBattleToggleHookInstalled;
    int battleSubmitHookInstalled;
    int installed;
} g;

SRWLOCK g_battleStateLock = SRWLOCK_INIT;

void beginBattleStateWrite()
{
    AcquireSRWLockExclusive(&g_battleStateLock);
    InterlockedIncrement(&g.battleStateSeq); // stable even -> updating odd
    MemoryBarrier();
}

void endBattleStateWrite()
{
    MemoryBarrier();
    InterlockedIncrement(&g.battleStateSeq); // updating odd -> next stable even generation
    ReleaseSRWLockExclusive(&g_battleStateLock);
}

void clearPendingActions()
{
    InterlockedExchange(&g.pendingEndDay, 0);
    InterlockedExchange(&g.pendingEndDayBattle, 0);
    InterlockedExchange(&g.battleClosePressed, 0);
    InterlockedExchange(&g.strategicReadyTicks, 0);
    InterlockedExchange(&g.suppressEndTurnConfirm, 0);
}

void clearForcedBattle(const char* why)
{
    beginBattleStateWrite();
    InterlockedIncrement(&g.battleInstance);
    const LONG wasForced = InterlockedExchange(&g.forceAutoRequested, 0);
    InterlockedExchange(&g.forceAutoActive, 0);
    InterlockedExchange(&g.forceAutoKickPending, 0);
    InterlockedExchange(&g.battleContinuation, 0);
    InterlockedExchange(&g.battleSelectionOpen, 0);
    InterlockedExchange(&g.battleSelectionPending, 0);
    InterlockedExchange(&g.battlePlaybackLocal, -1);
    InterlockedExchange(&g.battleTurnActive, -1);
    InterlockedExchange(&g.battleKindPublished, 0);
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&g.battleViewer), nullptr);
    endBattleStateWrite();
    if (wasForced)
        tlog("[timer] forced Auto Battle released (%s)", why ? why : "battle reset");
}

// off[9] dialog/button-create capture. int __stdcall sub_5C93D6(iface, btnName, dlgName, a4, a5).
int __stdcall hook_dlgCreate(int ifaceObj, char* btnName, const char* dlgName, int cb1, int cb2)
{
    int obj = reinterpret_cast<int(__stdcall*)(int, char*, const char*, int, int)>(g.g_orig_dlgCreate)(
        ifaceObj, btnName, dlgName, cb1, cb2);
    if (!obj || !btnName || !dlgName)
        return obj;
    void* o = reinterpret_cast<void*>(obj);

    if (!lstrcmpA(dlgName, "DLG_STRATEGIC") && !lstrcmpA(btnName, "BTN_END_TURN")) {
        g.endTurn = o;
        tlog("[timer] END_TURN captured %p", o);
    } else if (!lstrcmpA(dlgName, "DLG_CAPITAL") && !lstrcmpA(btnName, "BTN_BACK"))
        g.capBack = o;
    else if (!lstrcmpA(dlgName, "DLG_DIPLOMACY") && !lstrcmpA(btnName, "BTN_BACK"))
        g.diploBack = o;
    else if (!lstrcmpA(dlgName, "DLG_SCENARIO_BRIEFING") && !lstrcmpA(btnName, "BTN_CONTINUE"))
        g.briefCont = o;
    else if (!lstrcmpA(dlgName, "DLG_BATTLE_A") || !lstrcmpA(dlgName, "DLG_BATTLE_B")) {
        if (!lstrcmpA(btnName, "BTN_RETREAT"))
            g.btnRetreat = o;
        else if (!lstrcmpA(btnName, "BTN_CLOSE"))
            g.btnClose = o;
        else if (!lstrcmpA(btnName, "BTN_RESOLVE"))
            g.btnResolve = o;
        else if (!lstrcmpA(btnName, "BTN_DEFEND")) {
            g.btnDefend = o;
            __try {
                // anim flag = DEFEND button's hidden/disabled byte == 0 (an attack is animating)
                void* f8 = *reinterpret_cast<void**>(reinterpret_cast<char*>(o) + 8);
                g.animFlag = (*(reinterpret_cast<unsigned char*>(f8) + 4) == 0) ? 1 : 0;
                // PvP byte = *(BYTE*)[[[ [iface+4]+8]+0x1C]+0x14F8 ] (off[3]=0x14F8 player-struct byte)
                char* p0 = *reinterpret_cast<char**>(reinterpret_cast<char*>(ifaceObj) + 4);
                char* p1 = *reinterpret_cast<char**>(p0 + 8);
                char* p2 = *reinterpret_cast<char**>(p1 + 0x1C);
                g.pvpFlag = *reinterpret_cast<unsigned char*>(p2 + 0x14F8) ? 1 : 0;
                g.anyCombat = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g.animFlag = 0;
                g.pvpFlag = 0;
                g.anyCombat = 1; // combat exists even if the struct walk faulted
            }
            // The dialog can exist briefly before the first accepted ChooseAction. Publish combat
            // with unknown owner so the timer fails closed during setup; never overwrite a newer
            // authoritative controller generation if one already arrived.
            if (InterlockedExchangeAdd(&g.battleKindPublished, 0) == 0) {
                beginBattleStateWrite();
                if (g.battleKindPublished == 0) {
                    InterlockedExchange(&g.battleKindPublished, g.pvpFlag ? 1 : 2);
                    InterlockedExchange(&g.battleTurnActive, -1);
                    InterlockedExchange(&g.battleSelectionOpen, 0);
                    InterlockedExchange(&g.battleSelectionPending, 0);
                    InterlockedExchange(&g.battlePlaybackLocal, -1);
                    InterlockedExchange(&g.battleContinuation, 0);
                }
                endBattleStateWrite();
            }
        }
    }
    return obj;
}

// off[8] CButtonInterf::vftable[0] destructor. __thiscall(this, a2) retn 4 == __fastcall(ecx,edx,a2).
int __fastcall hook_btnDestroy(void* self, void* /*edx*/, int a2)
{
    if (self == g.endTurn) {
        g.endTurn = nullptr;
        tlog("[timer] END_TURN destroyed %p", self);
    } else if (self == g.capBack)
        g.capBack = nullptr;
    else if (self == g.diploBack)
        g.diploBack = nullptr;
    else if (self == g.briefCont)
        g.briefCont = nullptr;
    else if (self == g.btnRetreat)
        g.btnRetreat = nullptr;
    else if (self == g.btnDefend) {
        g.btnDefend = nullptr;
        g.animFlag = 0;
        g.pvpFlag = 0;
        g.anyCombat = 0; // combat exit -> clear pause inputs
    } else if (self == g.btnClose)
        g.btnClose = nullptr;
    else if (self == g.btnResolve)
        g.btnResolve = nullptr;
    return reinterpret_cast<int(__thiscall*)(void*, int)>(g.g_orig_btnDtor)(self, a2);
}

// off[5] CMidClient::vftable[0] scenario-init: drop all captures (new scenario / between games).
int __fastcall hook_scenarioInit(void* self, void* /*edx*/, int a2)
{
    g.endTurn = g.capBack = g.diploBack = g.briefCont = nullptr;
    g.btnRetreat = g.btnDefend = g.btnClose = g.btnResolve = nullptr;
    g.animFlag = g.pvpFlag = g.anyCombat = 0;
    clearPendingActions();
    clearForcedBattle("scenario reset");
    pluginhost_turn_reset();   // game/scenario change -> clear off[6] in-game flag
    g.lastTurnPlayer = -1;     // re-arm player debounce for the new game's first turn
    return reinterpret_cast<int(__thiscall*)(void*, int)>(g.g_orig_scenInit)(self, a2);
}

// off[6] turn-info handler (sub_48A680 @0x48A680 = CCmdTurnInfoMsg processor). A NETWORK message,
// dispatched on EVERY client at turn-start -> the reliable cross-client turn signal. Bump serial +
// mark in-game, then chain to original. __thiscall(this, a2) -> __fastcall trick.
int __fastcall hook_turnInfo(void* self, void* /*edx*/, int a2)
{
    // Current turn player byte, legacy way (sub_10001B90 did v3 = *sub_404E71()). Use the same
    // verified local-network-player walk as PvP decision parsing so both consumers share one layout.
    int player = -1;
    int rawPlayerId = -1;
    if (readLocalNetworkPlayerId(&rawPlayerId))
        player = rawPlayerId & 0xFF;
    // Debounce: sub_48A680 can fire several times per turn-info (a burst). Treat only a real PLAYER
    // CHANGE as a new turn; otherwise the plugin re-banks+reloads the budget and the turn starts at
    // DOUBLE time (joiner's first turn showed ~90s instead of 45s).
    if (player >= 0 && player != g.lastTurnPlayer) {
        g.lastTurnPlayer = player;
        // A queued press belongs only to the turn in which the timer elapsed. Never let it cross a
        // network turn boundary and fire against the next player's fresh, positive clock.
        clearPendingActions();
        pluginhost_bump_turn(player);
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 60)
            tlog("[timer] turn-info off6 player=%d (serial bump)", player);
    }
    return reinterpret_cast<int(__fastcall*)(void*, void*, int)>(g.g_orig_turnInfo)(self, nullptr, a2);
}

// CButtonInterf enabled flag (legacy *([btn+8]+4) != 0), SEH-guarded.
bool btnEnabled(void* btn)
{
    __try {
        void* f8 = *reinterpret_cast<void**>(reinterpret_cast<char*>(btn) + 8);
        return f8 && (*(reinterpret_cast<unsigned char*>(f8) + 4) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool interfaceOnTop(void* interf)
{
    if (!isUserPtr(interf))
        return false;
    __try {
        void* vtable = *reinterpret_cast<void**>(interf);
        // CInterfaceVftable::isOnTop is slot 11 (+44): true only when this control or one of its
        // parents is the interface manager's current topmost UI.
        void* method = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 44);
        return reinterpret_cast<bool(__thiscall*)(void*)>(method)(interf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void pressBtn(void* btn)
{
    // Cold press like the legacy (sub_10004D40 -> vtable+0xB0 = CButtonInterf "press"). No arming - the
    // gate (sub_5323C9) passes on any laid-out button; the turn-end takes only when pressed at an idle
    // point, so the pump issues this only on a WM_TIMER (see wndProcHook), never mid hero-move.
    __try {
        void* vtable = *reinterpret_cast<void**>(btn);
        void* method = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0xB0);
        reinterpret_cast<void(__thiscall*)(void*)>(method)(btn);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Verified Russobit/MNS battle layout (Discipl2.exe SHA-256
// 1375cdef09ec470ee64fe5693fb734d7c69fb215212311d997f792b258a642eb):
// CBattleViewerInterf embeds IBatViewer at +24, then data/data2 at +28/+32. Keep these raw here so
// C4dll-R does not acquire a link-time dependency on the toolset's game-structure library.
constexpr unsigned kBatViewerOffset = 24;
constexpr unsigned kViewerDataOffset = 28;
constexpr unsigned kViewerData2Offset = 32;
constexpr unsigned kBothPlayersHumanOffset = 5368;
constexpr unsigned kAnimationReadyOffset = 5344;
constexpr unsigned kAutoAttackerOffset = 56;
constexpr unsigned kAutoDefenderOffset = 57;
constexpr unsigned kAutoSideSelectorOffset = 58;

void* battleInterf(void* batViewer)
{
    return isUserPtr(batViewer)
        ? reinterpret_cast<char*>(batViewer) - kBatViewerOffset
        : nullptr;
}

bool battleData(void* batViewer, char** data, char** data2)
{
    *data = nullptr;
    *data2 = nullptr;
    void* outer = battleInterf(batViewer);
    if (!isUserPtr(outer))
        return false;
    __try {
        *data = *reinterpret_cast<char**>(reinterpret_cast<char*>(outer) + kViewerDataOffset);
        *data2 = *reinterpret_cast<char**>(reinterpret_cast<char*>(outer) + kViewerData2Offset);
        return isUserPtr(*data) && isUserPtr(*data2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *data = nullptr;
        *data2 = nullptr;
        return false;
    }
}

int nativeBattleAnimationState(void* batViewer)
{
    if (!isUserPtr(batViewer))
        return -1;
    char* data = nullptr;
    char* data2 = nullptr;
    if (!battleData(batViewer, &data, &data2))
        return -1;
    __try {
        // showAttackEffect writes 0; CAnimCounter writes 1 only after aggregate playback ends.
        return data[kAnimationReadyOffset] ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool localPlayerId(int* rawId, int* typeIndex)
{
    *rawId = -1;
    *typeIndex = -1;
    int id = -1;
    if (!readLocalNetworkPlayerId(&id))
        return false;
    *rawId = id;
    *typeIndex = id & 0xFFFF;
    return true;
}

void setButtonEnabled(void* btn, bool enabled)
{
    if (!isUserPtr(btn))
        return;
    __try {
        void* vtable = *reinterpret_cast<void**>(btn);
        void* method = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 160);
        reinterpret_cast<void(__thiscall*)(void*, bool)>(method)(btn, enabled);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* autoBattleToggle(void* batViewer)
{
    void* outer = battleInterf(batViewer);
    if (!isUserPtr(outer))
        return nullptr;
    __try {
        void* dialog = reinterpret_cast<void*(__thiscall*)(void*)>(0x56CEA4)(outer);
        return isUserPtr(dialog)
            ? reinterpret_cast<void*(__stdcall*)(void*, const char*)>(0x50BB1F)(
                  dialog, "TOG_AUTOBATTLE")
            : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void setToggleForced(void* batViewer, bool checkCurrentSide)
{
    char* data = nullptr;
    char* data2 = nullptr;
    if (!battleData(batViewer, &data, &data2))
        return;
    __try {
        if (checkCurrentSide) {
            const unsigned flagOffset = data2[kAutoSideSelectorOffset]
                ? kAutoAttackerOffset
                : kAutoDefenderOffset;
            data2[flagOffset] = 1;
        }
        void* toggle = autoBattleToggle(batViewer);
        if (isUserPtr(toggle)) {
            if (checkCurrentSide)
                reinterpret_cast<void(__thiscall*)(void*, bool)>(0x5355B3)(toggle, true);
            void* vtable = *reinterpret_cast<void**>(toggle);
            void* setEnabled =
                *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 140);
            reinterpret_cast<void(__thiscall*)(void*, bool)>(setEnabled)(toggle, false);
        }
        if (checkCurrentSide) {
            // The window-message filter is authoritative; disabling visible action buttons as well
            // prevents one frame of misleading manual controls before the next message-loop turn.
            setButtonEnabled(g.btnRetreat, false);
            setButtonEnabled(g.btnDefend, false);
            setButtonEnabled(g.btnResolve, false);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool kickNativeAutoBattle(void* batViewer)
{
    void* outer = battleInterf(batViewer);
    if (!isUserPtr(outer))
        return false;
    __try {
        // CBattleViewerInterf::autoBattleCallback: clears its timer UiEvent, then sends the native
        // BattleAction::Auto through IBatNotify on the game thread.
        reinterpret_cast<void(__thiscall*)(void*)>(0x6355EF)(outer);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum BattleTurnParseReason
{
    kBattleParseOk = 0,
    kBattleParseBadEventPointers,
    kBattleParseNoLocalPlayer,
    kBattleParseNoViewerData,
    kBattleParseBadActionCount,
    kBattleParseLocalNotParticipant,
    kBattleParseBadActiveUnit,
    kBattleParseUnitNotFound,
    kBattleParseException,
};

const char* battleParseReasonName(int reason)
{
    switch (reason) {
    case kBattleParseOk: return "ok";
    case kBattleParseBadEventPointers: return "bad-event-pointers";
    case kBattleParseNoLocalPlayer: return "no-local-player";
    case kBattleParseNoViewerData: return "no-viewer-data";
    case kBattleParseBadActionCount: return "bad-action-count";
    case kBattleParseLocalNotParticipant: return "local-not-participant";
    case kBattleParseBadActiveUnit: return "bad-active-unit";
    case kBattleParseUnitNotFound: return "unit-not-found";
    case kBattleParseException: return "exception";
    default: return "unknown";
    }
}

struct BattleTurnDiag
{
    int reason;
    int localRaw;
    int localIndex;
    int attackerId;
    int defenderId;
    int activeId;
    std::uint32_t actionCount;
    unsigned char unitFlags;
};

bool parseBattleTurn(void* batViewer, const void* msg, const void* unitId,
                     const void* actions, int* localActive, int* continuation,
                     int* pvp, BattleTurnDiag* diag)
{
    *localActive = -1;
    *continuation = 0;
    *pvp = 0;
    *diag = {};
    diag->reason = kBattleParseBadEventPointers;
    diag->localRaw = -1;
    diag->localIndex = -1;
    diag->attackerId = -1;
    diag->defenderId = -1;
    diag->activeId = -1;
    if (!isUserPtr(msg) || !isUserPtr(unitId) || !isUserPtr(actions))
        return false;

    int localRaw = -1, localIndex = -1;
    if (!localPlayerId(&localRaw, &localIndex)) {
        diag->reason = kBattleParseNoLocalPlayer;
        return false;
    }
    diag->localRaw = localRaw;
    diag->localIndex = localIndex;

    char* data = nullptr;
    char* data2 = nullptr;
    if (!battleData(batViewer, &data, &data2)) {
        diag->reason = kBattleParseNoViewerData;
        return false;
    }

    __try {
        const char* battle = reinterpret_cast<const char*>(msg);
        // game::Set<BattleAction>::length is +4. A zero set is a valid transition with no open
        // local choice; the directed grant remains valid diagnostics. Only an impossible count
        // rejects the layout.
        const std::uint32_t actionCount =
            *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const char*>(actions) + 4);
        diag->actionCount = actionCount;
        if (actionCount > 8) {
            diag->reason = kBattleParseBadActionCount;
            return false;
        }

        const int attackerId = *reinterpret_cast<const int*>(battle + 3808);
        const int defenderId = *reinterpret_cast<const int*>(battle + 3812);
        diag->attackerId = attackerId;
        diag->defenderId = defenderId;
        const bool localAttacker = attackerId == localRaw;
        const bool localDefender = defenderId == localRaw;
        if (localAttacker == localDefender) { // neither local, or malformed same-player battle
            diag->reason = kBattleParseLocalNotParticipant;
            return false;
        }

        const int activeId = *reinterpret_cast<const int*>(unitId);
        diag->activeId = activeId;
        if (activeId == 0 || activeId == -1 || activeId == kInvalidMidgardId) {
            diag->reason = kBattleParseBadActiveUnit;
            return false;
        }
        const char* info = nullptr;
        for (int i = 0; i < 22; ++i) {
            const char* candidate = battle + i * 168;
            // Mirror the native UnitInfo lookup: its 22-entry scan compares only the primary
            // CMidgardID at +0. The secondary ID at +4 can belong to another record.
            if (*reinterpret_cast<const int*>(candidate) == activeId) {
                info = candidate;
                break;
            }
        }
        if (!info) {
            diag->reason = kBattleParseUnitNotFound;
            return false;
        }

        const unsigned char flags =
            *reinterpret_cast<const unsigned char*>(info + 48);
        diag->unitFlags = flags;
        const bool unitAttacker = (flags & 0x08) != 0;
        *localActive = (unitAttacker == localAttacker) ? 1 : 0;
        *continuation = (*localActive && (flags & 0x20)) ? 1 : 0;
        *pvp = data[kBothPlayersHumanOffset] ? 1 : 0;
        diag->reason = kBattleParseOk;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diag->reason = kBattleParseException;
        return false;
    }
}

// Every client receives CCmdBattleResultMsg. The server snapshots BattleMsgData before it updates
// the current turn entry (result sender around 0x626775; turn update at 0x62696F), so the first
// valid turnsOrder entry is the actor whose playback is about to be shown, including the first hit
// of a mandatory double attack. This is a playback-owner signal, not the next decision owner.
bool parseBattlePlayback(void* batViewer, const void* msg, int* localPlayback,
                         int* pvp, BattleTurnDiag* diag)
{
    *localPlayback = -1;
    *pvp = 0;
    *diag = {};
    diag->reason = kBattleParseBadEventPointers;
    diag->localRaw = -1;
    diag->localIndex = -1;
    diag->attackerId = -1;
    diag->defenderId = -1;
    diag->activeId = -1;
    if (!isUserPtr(msg))
        return false;

    int localRaw = -1, localIndex = -1;
    if (!localPlayerId(&localRaw, &localIndex)) {
        diag->reason = kBattleParseNoLocalPlayer;
        return false;
    }
    diag->localRaw = localRaw;
    diag->localIndex = localIndex;

    char* data = nullptr;
    char* data2 = nullptr;
    if (!battleData(batViewer, &data, &data2)) {
        diag->reason = kBattleParseNoViewerData;
        return false;
    }

    __try {
        const char* battle = reinterpret_cast<const char*>(msg);
        const int attackerId = *reinterpret_cast<const int*>(battle + 3808);
        const int defenderId = *reinterpret_cast<const int*>(battle + 3812);
        diag->attackerId = attackerId;
        diag->defenderId = defenderId;
        const bool localAttacker = attackerId == localRaw;
        const bool localDefender = defenderId == localRaw;
        if (localAttacker == localDefender) {
            diag->reason = kBattleParseLocalNotParticipant;
            return false;
        }

        // BattleTurn is 8 bytes; turnsOrder begins at BattleMsgData+3696. Native code first takes
        // the first entry before the invalid-id terminator and only then looks up that exact id in
        // UnitInfo. Never skip an unknown first actor and attribute the Result to a later unit.
        int activeId = -1;
        for (int turn = 0; turn < 13; ++turn) {
            const int candidateId =
                *reinterpret_cast<const int*>(battle + 3696 + turn * 8);
            if (candidateId == kInvalidMidgardId)
                break; // canonical terminator for the native turnsOrder array
            activeId = candidateId;
            break;
        }
        diag->activeId = activeId;
        if (activeId == 0 || activeId == -1 || activeId == kInvalidMidgardId) {
            diag->reason = kBattleParseBadActiveUnit;
            return false;
        }

        const char* info = nullptr;
        for (int i = 0; i < 22; ++i) {
            const char* candidate = battle + i * 168;
            if (*reinterpret_cast<const int*>(candidate) == activeId) {
                info = candidate;
                break;
            }
        }
        if (!info) {
            diag->reason = kBattleParseUnitNotFound;
            return false;
        }

        const unsigned char flags =
            *reinterpret_cast<const unsigned char*>(info + 48);
        diag->unitFlags = flags;
        const bool unitAttacker = (flags & 0x08) != 0;
        *localPlayback = (unitAttacker == localAttacker) ? 1 : 0;
        *pvp = data[kBothPlayersHumanOffset] ? 1 : 0;
        diag->reason = kBattleParseOk;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        diag->reason = kBattleParseException;
        return false;
    }
}

// TOG_AUTOBATTLE callback @0x635509: __thiscall(CBattleViewerInterf*, bool, int), retn 8.
// Once forced, an uncheck click/hotkey is acknowledged visually but never reaches the native clear.
int __fastcall hookAutoBattleToggle(void* self, void* /*edx*/, unsigned char checked, int a3)
{
    void* batViewer = reinterpret_cast<char*>(self) + kBatViewerOffset;
    if (g.forceAutoActive && batViewer == g.battleViewer) {
        if (!checked)
            tlog("[timer] forced Auto Battle cancellation blocked (viewer=%p)", batViewer);
        setToggleForced(batViewer, g.battleTurnActive == 1);
        return 1;
    }
    return reinterpret_cast<int(__thiscall*)(void*, unsigned char, int)>(
        g.g_origAutoBattleToggle)(self, checked, a3);
}

bool installAutoBattleToggleHook()
{
    constexpr uintptr_t kCallback = 0x635509;
    const unsigned char expected[12] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x89, 0x4D, 0xF4, 0x8B, 0x45, 0xF4};
    const unsigned char nativeAutoExpected[12] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x07, 0x5C, 0x6C, 0x00, 0x64, 0xA1};
    __try {
        if (memcmp(reinterpret_cast<const void*>(kCallback), expected, sizeof(expected)) != 0 ||
            memcmp(reinterpret_cast<const void*>(0x6355EF), nativeAutoExpected,
                   sizeof(nativeAutoExpected)) != 0) {
            tlog("[timer] native Auto Battle signature mismatch; forcing disabled");
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    g.g_origAutoBattleToggle = reinterpret_cast<void*>(kCallback);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g.g_origAutoBattleToggle,
                 reinterpret_cast<void*>(hookAutoBattleToggle));
    if (DetourTransactionCommit() != NO_ERROR) {
        g.g_origAutoBattleToggle = reinterpret_cast<void*>(kCallback);
        tlog("[timer] TOG_AUTOBATTLE detour FAILED");
        return false;
    }
    g.autoBattleToggleHookInstalled = 1;
    tlog("[timer] forced Auto Battle guard installed (TOG_AUTOBATTLE 0x635509)");
    return true;
}

// CTaskBattle's two IBatNotify entry points (ordinary BattleAction and UseItem) converge on this
// exact sender. It is the authoritative end of the current local decision window. ChooseAction is
// delivered only to the client that must choose, so it cannot be used as a broadcast owner stream:
// after this submit the timer must remain stopped until another local ChooseAction opens a choice.
// A mandatory second-hit target arrives as its own ChooseAction and is therefore charged normally.
void __fastcall hookBattleSubmit(void* self, void* /*edx*/, const void* battleMsgData,
                                 int action, const void* targetId, const void* attackerId)
{
    if (isUserPtr(g.battleViewer) &&
        InterlockedExchangeAdd(&g.battleTurnActive, 0) == 1) {
        // Close only this choice. Keep the parsed local-side and double-attack continuation fields
        // for diagnostics/force safety, but selection_open is the timer authority. Clearing the
        // continuation here could still let a pending timeout kick AI inside an in-flight action.
        beginBattleStateWrite();
        InterlockedExchange(&g.battleSelectionOpen, 0);
        InterlockedExchange(&g.battleSelectionPending, 0);
        endBattleStateWrite();
        tlog("[timer] local battle action submitted (action=%d); decision clock closed, "
             "continuation retained until next choose-action event", action);
    }
    reinterpret_cast<void(__thiscall*)(void*, const void*, int,
                                       const void*, const void*)>(
        g.g_origBattleSubmit)(self, battleMsgData, action, targetId, attackerId);
}

bool installBattleSubmitHook()
{
    constexpr uintptr_t kSubmit = 0x4065BA;
    const unsigned char expected[19] = {
        0xB8, 0x43, 0x72, 0x68, 0x00, 0xE8, 0x0C, 0x6E, 0x26, 0x00,
        0x81, 0xEC, 0x68, 0x0F, 0x00, 0x00, 0x56, 0x8B, 0xF1};
    const uintptr_t expectedNotifyVtable[6] = {
        0x4D91C1, 0x4D8EC4, 0x4D8EE1, 0x4D8EFC, 0x4D8F38, 0x4D8F8D};
    __try {
        if (memcmp(reinterpret_cast<const void*>(kSubmit), expected,
                   sizeof(expected)) != 0 ||
            memcmp(reinterpret_cast<const void*>(0x6DCF0C), expectedNotifyVtable,
                   sizeof(expectedNotifyVtable)) != 0) {
            tlog("[timer] battle-submit signature mismatch; PvP decision timing disabled");
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    g.g_origBattleSubmit = reinterpret_cast<void*>(kSubmit);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g.g_origBattleSubmit,
                 reinterpret_cast<void*>(hookBattleSubmit));
    if (DetourTransactionCommit() != NO_ERROR) {
        g.g_origBattleSubmit = reinterpret_cast<void*>(kSubmit);
        tlog("[timer] battle-submit detour FAILED; PvP decision timing disabled");
        return false;
    }
    g.battleSubmitHookInstalled = 1;
    tlog("[timer] battle-submit boundary installed (0x4065BA)");
    return true;
}

// off[13] in the legacy timer is deliberately a CALL-SITE hook, not a global detour of the shared
// query at 0x61B781. sub_48FC7F calls it at 0x48FCA3; a non-zero result opens X005TA0000 (the normal
// "end turn?" question), while zero proceeds directly. Suppress exactly one query made by our forced
// END_TURN press and leave every user click untouched.
char __fastcall hookEndTurnConfirm(void* self, void* /*edx*/)
{
    if (InterlockedExchange(&g.suppressEndTurnConfirm, 0)) {
        tlog("[timer] forced END_TURN confirmation suppressed");
        return 0;
    }
    return reinterpret_cast<char(__thiscall*)(void*)>(g.g_origConfirmQuery)(self);
}

bool installEndTurnConfirmHook()
{
    constexpr uintptr_t kCallSite = 0x48FCA3;
    const unsigned char expected[5] = {0xE8, 0xD9, 0xBA, 0x18, 0x00}; // call 0x61B781
    __try {
        if (memcmp(reinterpret_cast<const void*>(kCallSite), expected, sizeof(expected)) != 0) {
            tlog("[timer] off13 confirmation callsite signature mismatch; forced End Day disabled");
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    unsigned char patch[5] = {0xE8, 0, 0, 0, 0};
    const intptr_t delta = reinterpret_cast<intptr_t>(hookEndTurnConfirm) -
                           static_cast<intptr_t>(kCallSite + sizeof(patch));
    const int32_t rel = static_cast<int32_t>(delta);
    memcpy(patch + 1, &rel, sizeof(rel));
    g.g_origConfirmQuery = reinterpret_cast<void*>(0x61B781);
    if (!writeBytes(kCallSite, patch, sizeof(patch)))
        return false;

    g.confirmHookInstalled = 1;
    tlog("[timer] off13 forced-END_TURN confirmation hook installed (callsite 0x48FCA3)");
    return true;
}

// CBeginTurnInterf::BTN_OK callback. The dialog is the daily/turn summary (income + turn number),
// and 0x4BA8BB is the exact callback placed into BTN_OK's ordinary game functor by the constructor.
// Chain first: only a successfully completed native callback publishes the acknowledgement.
void* __fastcall hookBeginTurnOk(void* self, void* /*edx*/)
{
    void* result = reinterpret_cast<void*(__thiscall*)(void*)>(g.g_origBeginTurnOk)(self);
    const LONG serial = InterlockedIncrement(&g.beginTurnAckSerial);
    tlog("[timer] DLG_BEGIN_TURN BTN_OK accepted (ack=%ld)", serial);
    return result;
}

bool installBeginTurnAckHook()
{
    constexpr uintptr_t kCallback = 0x4BA8BB;
    // mov eax,[ecx+20h]; mov ecx,[eax]; jmp 0x4D97E3
    const unsigned char expected[10] = {
        0x8B, 0x41, 0x20, 0x8B, 0x08, 0xE9, 0x1E, 0xEF, 0x01, 0x00};
    __try {
        if (memcmp(reinterpret_cast<const void*>(kCallback), expected,
                   sizeof(expected)) != 0) {
            tlog("[timer] begin-turn BTN_OK signature mismatch; delayed start unavailable");
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    g.g_origBeginTurnOk = reinterpret_cast<void*>(kCallback);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g.g_origBeginTurnOk,
                 reinterpret_cast<void*>(hookBeginTurnOk));
    if (DetourTransactionCommit() != NO_ERROR) {
        g.g_origBeginTurnOk = reinterpret_cast<void*>(kCallback);
        tlog("[timer] begin-turn BTN_OK detour FAILED");
        return false;
    }

    g.beginTurnAckHookInstalled = 1;
    tlog("[timer] begin-turn BTN_OK acknowledgement hook installed (0x4BA8BB)");
    return true;
}

} // namespace

// Called by featuremenu's signature-gated accepted ChooseAction controller hook, before downstream
// UI handling. This remains authoritative even when the toolset fully replaces IBatViewer::update.
extern "C" void timerhost_on_battle_update(void* batViewer, const void* battleMsgData,
                                             const void* unitId, const void* actions)
{
    int localActive = -1, continuation = 0, pvp = 0;
    BattleTurnDiag diag = {};
    const bool valid = parseBattleTurn(batViewer, battleMsgData, unitId, actions,
                                       &localActive, &continuation, &pvp, &diag);

    // Publish the complete controller transition as one seqlock generation. The timer worker must
    // never combine a new battle viewer/PvP kind with the previous unit owner.
    void* const previousViewer = g.battleViewer;
    const LONG previous = InterlockedExchangeAdd(&g.battleTurnActive, 0);
    beginBattleStateWrite();
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&g.battleViewer), batViewer);
    if (previousViewer != batViewer) {
        InterlockedIncrement(&g.battleInstance);
        InterlockedExchange(&g.forceAutoRequested, 0);
        InterlockedExchange(&g.forceAutoActive, 0);
        InterlockedExchange(&g.forceAutoKickPending, 0);
    }
    InterlockedExchange(&g.anyCombat, 1);
    InterlockedExchange(&g.battleTurnActive, valid ? localActive : -1);
    InterlockedExchange(&g.battleContinuation, valid ? continuation : 0);
    InterlockedExchange(&g.battleSelectionOpen, 0);
    // A newly accepted local decision supersedes the prior Result playback even if the renderer's
    // final ready byte is published a few instructions later.
    InterlockedExchange(&g.battlePlaybackLocal, -1);
    InterlockedExchange(&g.battleSelectionPending,
                        valid && localActive && diag.actionCount ? 1 : 0);
    InterlockedExchange(&g.battleKindPublished, valid ? (pvp ? 1 : 2) : 2);
    if (valid)
        InterlockedExchange(&g.pvpFlag, pvp);
    endBattleStateWrite();

    static volatile LONG receiveSerial = 0;
    const LONG receive = InterlockedIncrement(&receiveSerial);
    if (valid) {
        tlog("[timer] battle-select #%ld viewer=%p local=%d pvp=%d selection=%d continuation=%d "
             "localId=%08X/%d attacker=%08X defender=%08X unit=%08X actions=%u flags=%02X",
             receive, batViewer, localActive, pvp,
             localActive && diag.actionCount ? 1 : 0, continuation,
             static_cast<unsigned>(diag.localRaw), diag.localIndex,
             static_cast<unsigned>(diag.attackerId), static_cast<unsigned>(diag.defenderId),
             static_cast<unsigned>(diag.activeId), diag.actionCount,
             static_cast<unsigned>(diag.unitFlags));
    } else {
        tlog("[timer] battle-select #%ld REJECTED reason=%s viewer=%p msg=%p unitPtr=%p "
             "actionsPtr=%p localId=%08X/%d attacker=%08X defender=%08X unit=%08X actions=%u",
             receive, battleParseReasonName(diag.reason), batViewer, battleMsgData, unitId,
             actions, static_cast<unsigned>(diag.localRaw), diag.localIndex,
             static_cast<unsigned>(diag.attackerId), static_cast<unsigned>(diag.defenderId),
             static_cast<unsigned>(diag.activeId), diag.actionCount);
    }
    if (previous != (valid ? localActive : -1)) {
        tlog("[timer] battle unit -> local=%d pvp=%d continuation=%d valid=%d",
             valid ? localActive : -1, valid ? pvp : static_cast<int>(g.pvpFlag),
             valid ? continuation : 0, valid ? 1 : 0);
    }

}

extern "C" void timerhost_on_battle_result(void* batViewer, const void* battleMsgData)
{
    int localPlayback = -1, pvp = 0;
    BattleTurnDiag diag = {};
    const bool valid = parseBattlePlayback(batViewer, battleMsgData,
                                           &localPlayback, &pvp, &diag);

    void* const previousViewer = g.battleViewer;
    beginBattleStateWrite();
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&g.battleViewer), batViewer);
    if (previousViewer != batViewer) {
        InterlockedIncrement(&g.battleInstance);
        InterlockedExchange(&g.forceAutoRequested, 0);
        InterlockedExchange(&g.forceAutoActive, 0);
        InterlockedExchange(&g.forceAutoKickPending, 0);
        InterlockedExchange(&g.battleTurnActive, -1);
        InterlockedExchange(&g.battleSelectionOpen, 0);
        InterlockedExchange(&g.battleSelectionPending, 0);
        InterlockedExchange(&g.battleContinuation, 0);
    }
    InterlockedExchange(&g.anyCombat, 1);
    // Result playback is never an open decision. The common submit normally closed these already,
    // but the broadcast boundary also covers native Auto Battle and zero-choice transitions.
    InterlockedExchange(&g.battleSelectionOpen, 0);
    InterlockedExchange(&g.battleSelectionPending, 0);
    InterlockedExchange(&g.battlePlaybackLocal, valid ? localPlayback : -1);
    if (valid) {
        InterlockedExchange(&g.battleKindPublished, pvp ? 1 : 2);
        InterlockedExchange(&g.pvpFlag, pvp);
    }
    endBattleStateWrite();

    static volatile LONG resultSerial = 0;
    const LONG serial = InterlockedIncrement(&resultSerial);
    if (valid) {
        tlog("[timer] battle-result #%ld playback local=%d pvp=%d viewer=%p "
             "localId=%08X/%d attacker=%08X defender=%08X unit=%08X flags=%02X",
             serial, localPlayback, pvp, batViewer,
             static_cast<unsigned>(diag.localRaw), diag.localIndex,
             static_cast<unsigned>(diag.attackerId),
             static_cast<unsigned>(diag.defenderId),
             static_cast<unsigned>(diag.activeId),
             static_cast<unsigned>(diag.unitFlags));
    } else {
        tlog("[timer] battle-result #%ld REJECTED reason=%s viewer=%p msg=%p "
             "localId=%08X/%d attacker=%08X defender=%08X unit=%08X",
             serial, battleParseReasonName(diag.reason), batViewer, battleMsgData,
             static_cast<unsigned>(diag.localRaw), diag.localIndex,
             static_cast<unsigned>(diag.attackerId),
             static_cast<unsigned>(diag.defenderId),
             static_cast<unsigned>(diag.activeId));
    }
}

extern "C" void timerhost_on_battle_result_end(void)
{
    const LONG previous = InterlockedExchangeAdd(&g.battlePlaybackLocal, 0);
    if (previous == -1)
        return;
    beginBattleStateWrite();
    InterlockedExchange(&g.battlePlaybackLocal, -1);
    endBattleStateWrite();
    tlog("[timer] aggregate battle playback ended (previous local=%ld)", previous);
}

extern "C" void timerhost_after_battle_update(void* batViewer)
{
    // The accepted choice becomes user-visible only after the native/toolset viewer handler has
    // returned. If Auto Battle submitted inside that handler, the submit hook already cleared the
    // pending bit, so we must not reopen manual input here.
    if (batViewer == g.battleViewer &&
        InterlockedExchangeAdd(&g.battleSelectionPending, 0)) {
        bool opened = false;
        beginBattleStateWrite();
        if (batViewer == g.battleViewer && g.battleTurnActive == 1 &&
            g.battleSelectionPending) {
            InterlockedExchange(&g.battleSelectionOpen, 1);
            InterlockedExchange(&g.battleSelectionPending, 0);
            opened = true;
        }
        endBattleStateWrite();
        if (opened)
            tlog("[timer] local battle selection opened (viewer=%p)", batViewer);
    }
    if (g.forceAutoRequested && !g.forceAutoActive)
        timerhost_force_auto_battle(); // re-evaluate the deferred latch on the new full choice
    if (g.forceAutoActive && batViewer == g.battleViewer)
        setToggleForced(batViewer, g.battleTurnActive == 1);
}

extern "C" void timerhost_on_battle_end(void)
{
    clearForcedBattle("battle end/destructor");
}

extern "C" int timerhost_filter_input(UINT msg, WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    if (!g.forceAutoActive)
        return 0;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
        return 1;
    default:
        return 0;
    }
}

// Exported read accessors (lock-free volatile reads; called by pluginhost thunks).
extern "C" int timerhost_get_battle_timer_state(C4P_BattleTimerState* out)
{
    const size_t legacySize = offsetof(C4P_BattleTimerState, playback_local);
    if (!out || out->struct_size < legacySize || !g.installed ||
        !g.battleSubmitHookInstalled || !featuremenu_battle_timer_lifecycle_available())
        return 0;
    const size_t callerSize = out->struct_size;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG seq1 = InterlockedExchangeAdd(&g.battleStateSeq, 0);
        if (seq1 & 1)
            continue;

        C4P_BattleTimerState state = {};
        state.struct_size = sizeof(state);
        state.generation = static_cast<uint32_t>(seq1);
        state.battle_instance = static_cast<uint32_t>(
            InterlockedExchangeAdd(&g.battleInstance, 0));
        state.battle_kind = static_cast<int32_t>(
            InterlockedExchangeAdd(&g.battleKindPublished, 0));
        state.local_active = static_cast<int32_t>(
            InterlockedExchangeAdd(&g.battleTurnActive, 0));
        state.selection_open = static_cast<int32_t>(
            InterlockedExchangeAdd(&g.battleSelectionOpen, 0));
        state.continuation = static_cast<int32_t>(
            InterlockedExchangeAdd(&g.battleContinuation, 0));
        state.playback_local = static_cast<int32_t>(
            InterlockedExchangeAdd(&g.battlePlaybackLocal, 0));
        void* const viewer = g.battleViewer;
        state.animation_active = state.battle_kind
            ? nativeBattleAnimationState(viewer)
            : 0;

        MemoryBarrier();
        const LONG seq2 = InterlockedExchangeAdd(&g.battleStateSeq, 0);
        if (seq1 == seq2 && !(seq2 & 1)) {
            const size_t copySize = callerSize < sizeof(state) ? callerSize : sizeof(state);
            memcpy(out, &state, copySize);
            return 1;
        }
    }
    return 0;
}

extern "C" int timerhost_is_animating(void)
{
    C4P_BattleTimerState state = {};
    state.struct_size = sizeof(state);
    if (timerhost_get_battle_timer_state(&state))
        return state.animation_active != 0 ? 1 : 0; // unknown (-1) is fail-closed
    return featuremenu_battle_animation_active();
}
extern "C" int timerhost_battle_kind(void)
{
    C4P_BattleTimerState state = {};
    state.struct_size = sizeof(state);
    if (timerhost_get_battle_timer_state(&state))
        return state.battle_kind;
    return g.pvpFlag ? 1 : (g.anyCombat ? 2 : 0);
}
extern "C" int timerhost_battle_turn_active(void)
{
    // Legacy ABI: this reports the side of the last directed local decision grant. It is not a
    // broadcast turn-owner stream; exact billing uses get_battle_timer_state().selection_open and
    // playback_local instead.
    if (!g.installed || !featuremenu_battle_timer_lifecycle_available())
        return -1;
    C4P_BattleTimerState state = {};
    state.struct_size = sizeof(state);
    return timerhost_get_battle_timer_state(&state) ? state.local_active : -1;
}
// Local player's turn is active when strategic END_TURN exists AND is enabled. Captured PER CLIENT,
// so valid on host AND joiner - unlike the server currentPlayerIndex (host-only). Transitions logged.
extern "C" int timerhost_turn_active(void)
{
    int a;
    const int mt = featuremenu_my_turn(); // game's clientTakesTurn: true for my WHOLE turn incl. every
    if (mt >= 0) {                        // sub-dialog (city/diplomacy/spellbook). The clean signal.
        a = mt;
    } else {
        // Fallback (menu/loading or a layout mismatch): END_TURN present+enabled, or a known my-turn
        // strategic sub-view. The pump stays END_TURN-only regardless.
        __try {
            a = (isUserPtr(g.endTurn) && btnEnabled(g.endTurn)) ? 1 : 0;
            if (!a && (isUserPtr(g.capBack) || isUserPtr(g.diploBack)))
                a = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            a = 0;
        }
    }
    static int last = -1;
    if (a != last) { last = a; tlog("[timer] turn_active -> %d (mt=%d endTurn=%p day=%d)", a, mt, g.endTurn, featuremenu_current_day()); }
    return a;
}
extern "C" int timerhost_turn_player_id(void)
{
    int raw = -1, index = -1;
    return localPlayerId(&raw, &index) ? index : -1;
}
extern "C" int timerhost_force_auto_battle(void)
{
    if (!g.autoBattleToggleHookInstalled || !g.battleSubmitHookInstalled)
        return 0;

    for (int attempt = 0; attempt < 4; ++attempt) {
        C4P_BattleTimerState state = {};
        state.struct_size = sizeof(state);
        if (!timerhost_get_battle_timer_state(&state) || state.battle_kind != 1 ||
            state.local_active < 0) {
            tlog("[timer] forced Auto Battle rejected (toggleHook=%d submitHook=%d "
                 "kind=%d active=%d)", g.autoBattleToggleHookInstalled,
                 g.battleSubmitHookInstalled, state.battle_kind, state.local_active);
            return 0;
        }

        // Serialize the request with battle begin/end publications. A stale worker snapshot can
        // never relatch force fields after the game thread has advanced to another instance.
        AcquireSRWLockShared(&g_battleStateLock);
        const bool sameState = state.generation == static_cast<uint32_t>(
                                   InterlockedExchangeAdd(&g.battleStateSeq, 0)) &&
                               state.battle_instance == static_cast<uint32_t>(
                                   InterlockedExchangeAdd(&g.battleInstance, 0));
        if (!sameState) {
            ReleaseSRWLockShared(&g_battleStateLock);
            continue;
        }

        const LONG wasRequested = InterlockedExchange(&g.forceAutoRequested, 1);
        // A continuation ChooseAction is a fresh, controllable decision boundary. selection_open
        // plus no attributed Result playback is sufficient to avoid kicking AI inside a submitted
        // action. The viewer's aggregate animation byte can still be busy during the first valid
        // choice, so it is not an authority for whether that choice is actionable.
        const bool canActivate = state.local_active == 1 && state.selection_open == 1 &&
                                 state.playback_local < 0;
        bool activatedNow = false;
        if (!g.forceAutoActive && canActivate &&
            InterlockedCompareExchange(&g.forceAutoActive, 1, 0) == 0) {
            InterlockedExchange(&g.forceAutoKickPending, 1);
            activatedNow = true;
        } else if (g.forceAutoActive && state.local_active == 1 &&
                   state.selection_open == 1 && state.playback_local < 0) {
            // Once latched, AI also owns any continuation actions it reaches itself.
            InterlockedExchange(&g.forceAutoKickPending, 1);
        }
        ReleaseSRWLockShared(&g_battleStateLock);

        if (activatedNow) {
            tlog("[timer] forced Auto Battle activated (instance=%u generation=%u)",
                 state.battle_instance, state.generation);
        }
        if (!wasRequested) {
            tlog("[timer] forced Auto Battle requested (defer=%d selection=%d anim=%d "
                 "playbackLocal=%d continuation=%d)", canActivate ? 0 : 1,
                 state.selection_open, state.animation_active, state.playback_local,
                 state.continuation);
        }
        return 1;
    }

    tlog("[timer] forced Auto Battle request dropped: battle state kept changing");
    return 0;
}
// Phase 2 on-elapse: plugin (worker thread) QUEUES the press; timerhost_pump() performs it on each
// idle WM_TIMER (featuremenu's 32ms timer), retrying until the turn ends - like the legacy 0x113 case.
extern "C" int timerhost_retreat(void)
{
    // ABI compatibility only. Retreat/Defend are intentionally disabled in the timer menu.
    tlog("[timer] legacy retreat request ignored");
    return 0;
}
extern "C" int timerhost_end_day(void)
{
    if (!g.confirmHookInstalled)
        return 0; // never replace a timeout with an unavoidable confirmation dialog
    const bool fromBattle = isUserPtr(g.battleViewer) || isUserPtr(g.btnRetreat) ||
                            isUserPtr(g.btnDefend) || isUserPtr(g.btnClose);
    InterlockedExchange(&g.pendingEndDayBattle, fromBattle ? 1 : 0);
    InterlockedExchange(&g.battleClosePressed, 0);
    InterlockedExchange(&g.strategicReadyTicks, 0);
    InterlockedExchange(&g.pendingEndDay, 1);
    tlog("[timer] end_day QUEUED by plugin (fromBattle=%d)", fromBattle ? 1 : 0);
    return 1;
}
extern "C" int timerhost_cancel_elapse(void)
{
    clearPendingActions();
    return 1;
}
extern "C" uint32_t timerhost_begin_turn_ack_serial(void)
{
    return g.beginTurnAckHookInstalled
        ? static_cast<uint32_t>(InterlockedExchangeAdd(
              &g.beginTurnAckSerial, 0))
        : UINT32_MAX;
}

extern "C" void timerhost_pump(void)
{
    // Called ONLY on WM_TIMER (featuremenu's 32ms timer) = an idle point, after the hero finished
    // moving. Pressing END_TURN mid-move fires onClick but the game rejects the turn-end.
    if (!g.pendingEndDay && !g.forceAutoKickPending)
        return;
    // Holding LMB must never stall a permanent forced-Auto latch. It blocks only End Day, whose
    // button press tears down strategic UI and is unsafe mid-drag; that request remains pending.
    const bool endDayBlockedByDrag = g.pendingEndDay &&
        ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    if (endDayBlockedByDrag && !g.forceAutoKickPending)
        return;
    if (InterlockedCompareExchange(&g.inAction, 1, 0) != 0)
        return; // re-entry guard: the press pumps messages; never recurse into a second press
    __try {
        // First activation needs one native AI callback because downstream UI handling has already
        // returned without seeing the newly-set side flag. Later unit updates schedule the
        // same callback themselves through the game's normal five-millisecond UiEvent.
        C4P_BattleTimerState battleState = {};
        battleState.struct_size = sizeof(battleState);
        const bool forceKickEligible = g.forceAutoKickPending && g.forceAutoActive &&
            timerhost_get_battle_timer_state(&battleState) &&
            battleState.battle_kind == 1 && battleState.local_active == 1 &&
            battleState.selection_open == 1 && battleState.playback_local < 0 &&
            isUserPtr(g.battleViewer);
        if (forceKickEligible) {
            void* const viewer = g.battleViewer;
            setToggleForced(viewer, true);
            if (kickNativeAutoBattle(viewer)) {
                InterlockedExchange(&g.forceAutoKickPending, 0);
                tlog("[timer] native Auto Battle callback dispatched (viewer=%p instance=%u)",
                     viewer, battleState.battle_instance);
            }
        }

        if (g.pendingEndDay && !endDayBlockedByDrag) {
            const bool battleUi = isUserPtr(g.btnRetreat) || isUserPtr(g.btnDefend) ||
                                  isUserPtr(g.btnClose);
            if (battleUi) {
                // Never retreat. Wait for the resolved state, then use the ordinary BTN_CLOSE once.
                if (!g.battleClosePressed && isUserPtr(g.btnClose) && btnEnabled(g.btnClose)) {
                    InterlockedExchange(&g.battleClosePressed, 1);
                    InterlockedExchange(&g.strategicReadyTicks, 0);
                    tlog("[timer] battle resolved; pressing BTN_CLOSE before End Day");
                    pressBtn(g.btnClose);
                }
            } else {
                const int myTurn = featuremenu_my_turn();
                if (myTurn == 0) {
                    // Includes the defender-timeout case: never end the attacker's strategic turn.
                    tlog("[timer] End Day discarded: local strategic turn is not active");
                    clearPendingActions();
                } else if (g.pendingEndDayBattle) {
                    // Post-battle path is intentionally strict. Victory/defeat screens do not expose
                    // an enabled strategic END_TURN button, and we never press their Continue control.
                    const bool strategicReady =
                        myTurn == 1 && isUserPtr(g.endTurn) && btnEnabled(g.endTurn) &&
                        interfaceOnTop(g.endTurn);
                    if (!strategicReady) {
                        InterlockedExchange(&g.strategicReadyTicks, 0);
                    } else if (InterlockedIncrement(&g.strategicReadyTicks) >= 2) {
                        tlog("[timer] strategic UI stably restored; pressing END_TURN after battle");
                        InterlockedExchange(&g.suppressEndTurnConfirm, 1);
                        pressBtn(g.endTurn);
                        InterlockedExchange(&g.suppressEndTurnConfirm, 0);
                    }
                } else if (myTurn != 0) {
                    // Preserve the legacy non-battle End Day priority chain for ordinary timeouts.
                    void* order[4] = {g.briefCont, g.capBack, g.diploBack, g.endTurn};
                    for (int i = 0; i < 4; ++i) {
                        if (!isUserPtr(order[i]) || !btnEnabled(order[i]))
                            continue;
                        tlog("[timer] pressing btn[%d]=%p (WM_TIMER idle)", i, order[i]);
                        const bool endTurn = order[i] == g.endTurn;
                        if (endTurn)
                            InterlockedExchange(&g.suppressEndTurnConfirm, 1);
                        pressBtn(order[i]);
                        if (endTurn)
                            InterlockedExchange(&g.suppressEndTurnConfirm, 0);
                        break;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    InterlockedExchange(&g.inAction, 0);
}

// Install (called from featuremenu_install on Russobit, after installBattleDiscriminator).
extern "C" void timerhost_install(void)
{
    if (g.installed)
        return;
    g.installed = 1;
    g.lastTurnPlayer = -1;
    InterlockedExchange(&g.battleTurnActive, -1);
    InterlockedExchange(&g.battlePlaybackLocal, -1);

    const bool localPlayerAccessorValid = validateLocalNetworkPlayerIdAccessor();
    InterlockedExchange(&g_localNetworkPlayerIdAccessorAvailable,
                        localPlayerAccessorValid ? 1 : 0);
    if (localPlayerAccessorValid)
        tlog("[timer] local-player accessor validated (0x403384, netPlayerClientPtr +32)");
    else
        tlog("[timer] local-player accessor signature mismatch; PvP decision timing unavailable");

    // off[9] dialog/button-create capture (__stdcall 5-arg); off[6] turn-info handler (cross-client).
    g.g_orig_dlgCreate = reinterpret_cast<void*>(0x5C93D6);
    g.g_orig_turnInfo = reinterpret_cast<void*>(0x48A680);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g.g_orig_dlgCreate, reinterpret_cast<void*>(hook_dlgCreate));
    DetourAttach(&g.g_orig_turnInfo, reinterpret_cast<void*>(hook_turnInfo));
    if (DetourTransactionCommit() != NO_ERROR) {
        g.g_orig_dlgCreate = reinterpret_cast<void*>(0x5C93D6);
        g.g_orig_turnInfo = reinterpret_cast<void*>(0x48A680);
        tlog("[timer] keystone off[9]/off[6] detour FAILED");
    }

    // off[8] CButtonInterf::vftable[0] destructor -> null captured buttons on destroy.
    g.g_orig_btnDtor = patchVtableSlot(0x6E3294, 0, reinterpret_cast<void*>(hook_btnDestroy));
    // off[5] CMidClient::vftable[0] scenario-init -> drop all captures.
    g.g_orig_scenInit = patchVtableSlot(0x6CEB5C, 0, reinterpret_cast<void*>(hook_scenarioInit));

    // Exact Russobit/MNS 3.01a callsite used by the original timer.mod off[13] table. Signature-gated:
    // on another executable this remains a clean no-op and automatic End Day is not queued.
    installEndTurnConfirmHook();
    // Exact Russobit begin-turn confirmation callback. Other executable families deliberately keep
    // the existing active-turn edge because this address is not assumed to be portable.
    installBeginTurnAckHook();
    // Native Auto Battle toggle/callback addresses are used only behind this exact prologue gate.
    installAutoBattleToggleHook();
    // Exact common IBatNotify sender: closes the local selection interval for every action kind.
    installBattleSubmitHook();

    tlog("[timer] keystone capture installed (off9 dlg-create 0x5C93D6, off8 btn-dtor 0x6E3294, "
         "off5 scen-init 0x6CEB5C)");
}
