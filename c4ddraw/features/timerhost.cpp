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
extern "C" void pluginhost_queue_battle_state(int active);
#include "c4trace.h"
#include "eventtrace.h"

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
extern "C" int timerhost_get_battle_timer_state(C4P_BattleTimerState* out);
extern "C" HWND pluginhost_game_hwnd(void);
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
constexpr int kBattleActionAuto = 5;
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
    LONG volatile pendingEndDayLocalOrigin; // 1 attacker/current turn, 0 defender, -1 unknown
    LONG volatile battleClosePressed;  // BTN_CLOSE is one-shot for the resolved battle
    LONG volatile strategicReadyTicks; // require a stable restored strategic topmost UI
    LONG volatile endDayLockWaitLogged; // one diagnostic edge while native CPhaseGame gate is busy
    LONG volatile postBattleTransition; // battle UI ended; reward/result UI may still cover strategy
    LONG volatile postBattleReadyTicks; // stable strategic proof used to retire that transition
    LONG volatile inAction;       // local re-entry guard around the game-thread press
    // Original timer.mod's separate one-shot off[13] flag: consumed only by the END_TURN
    // confirmation-query callsite, so an automatic timeout never opens X005TA0000.
    LONG volatile suppressEndTurnConfirm;
    // DLG_BEGIN_TURN's real BTN_OK callback increments this only after the game has accepted the
    // acknowledgement. The plugin uses the edge, rather than dialog visibility, as its clock start.
    LONG volatile beginTurnAckSerial;
    // Load Game restores an already-usable turn without constructing DLG_BEGIN_TURN. The network
    // turn-info edge arms a second, UI-proven acknowledgement path; it is consumed only after the
    // real strategic END_TURN control is enabled and topmost for two idle GUI ticks.
    LONG volatile beginTurnReadyPending;
    LONG volatile beginTurnReadyTicks;
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
    // Forced Auto Battle is a deferred native UI command. The plugin worker only sets requested;
    // the game thread consumes it once for a specific battle-instance/state generation.
    LONG volatile forceAutoRequested;
    LONG volatile forceAutoMessagePosted;
    LONG volatile forceAutoDispatching;
    LONG volatile forceAutoLastInstance;
    LONG volatile forceAutoLastGeneration;
    LONG volatile forceAutoUiAvailable;
    // Once the native toggle accepted a timeout, protect that exact battle side until teardown.
    // This guard is deliberately narrower than the released input filter: it owns only the native
    // TOG_AUTOBATTLE callback, so chat and every unrelated mouse/key command remain available.
    LONG volatile forceAutoLatched;
    LONG volatile forceAutoLatchedInstance;
    LONG volatile forceAutoLatchedSideOffset;
    void* volatile forceAutoLatchedViewer;
    void* g_orig_dlgCreate;
    void* g_orig_btnDtor;
    void* g_orig_midClientDtor;
    void* g_orig_turnInfo;
    void* g_origConfirmQuery;
    void* g_origBeginTurnOk;
    void* g_origAutoBattleToggle;
    void* g_origBattleSubmit;
    int lastTurnPlayer;     // full off[6] owner id (debounce: ignore same-owner message bursts)
    int confirmHookInstalled;
    int beginTurnAckHookInstalled;
    int autoBattleToggleGuardInstalled;
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
    InterlockedExchange(&g.pendingEndDayLocalOrigin, -1);
    InterlockedExchange(&g.battleClosePressed, 0);
    InterlockedExchange(&g.strategicReadyTicks, 0);
    InterlockedExchange(&g.endDayLockWaitLogged, 0);
    InterlockedExchange(&g.suppressEndTurnConfirm, 0);
}

void clearPostBattleTransition()
{
    InterlockedExchange(&g.postBattleTransition, 0);
    InterlockedExchange(&g.postBattleReadyTicks, 0);
}

void clearForcedAutoLatch()
{
    // Publish inactive first so a concurrent read can never match partially-cleared identity data.
    InterlockedExchange(&g.forceAutoLatched, 0);
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&g.forceAutoLatchedViewer), nullptr);
    InterlockedExchange(&g.forceAutoLatchedInstance, -1);
    InterlockedExchange(&g.forceAutoLatchedSideOffset, -1);
}

void clearBattleState()
{
    beginBattleStateWrite();
    InterlockedIncrement(&g.battleInstance);
    InterlockedExchange(&g.battleContinuation, 0);
    InterlockedExchange(&g.battleSelectionOpen, 0);
    InterlockedExchange(&g.battleSelectionPending, 0);
    InterlockedExchange(&g.battlePlaybackLocal, -1);
    InterlockedExchange(&g.battleTurnActive, -1);
    InterlockedExchange(&g.battleKindPublished, 0);
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(&g.battleViewer), nullptr);
    InterlockedExchange(&g.forceAutoRequested, 0);
    InterlockedExchange(&g.forceAutoMessagePosted, 0);
    InterlockedExchange(&g.forceAutoLastInstance, -1);
    InterlockedExchange(&g.forceAutoLastGeneration, -1);
    clearForcedAutoLatch();
    endBattleStateWrite();
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
            pluginhost_queue_battle_state(1);
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
        pluginhost_queue_battle_state(0);
    } else if (self == g.btnClose)
        g.btnClose = nullptr;
    else if (self == g.btnResolve)
        g.btnResolve = nullptr;
    return reinterpret_cast<int(__thiscall*)(void*, int)>(g.g_orig_btnDtor)(self, a2);
}

// off[5] CMidClient::vftable[0] destructor: drop all captures (scenario teardown / between games).
int __fastcall hook_midClientDestroy(void* self, void* /*edx*/, int a2)
{
    g.endTurn = g.capBack = g.diploBack = g.briefCont = nullptr;
    g.btnRetreat = g.btnDefend = g.btnClose = g.btnResolve = nullptr;
    g.animFlag = g.pvpFlag = g.anyCombat = 0;
    clearPendingActions();
    clearPostBattleTransition();
    clearBattleState();
    InterlockedExchange(&g.beginTurnReadyPending, 0);
    InterlockedExchange(&g.beginTurnReadyTicks, 0);
    pluginhost_queue_battle_state(0);
    pluginhost_turn_reset();   // game/scenario change -> clear off[6] in-game flag
    g.lastTurnPlayer = -1;     // re-arm player debounce for the new game's first turn
    return reinterpret_cast<int(__thiscall*)(void*, int)>(g.g_orig_midClientDtor)(self, a2);
}

// off[6] turn-info handler (sub_48A680 @0x48A680 = CCmdTurnInfoMsg processor). A NETWORK message,
// dispatched on EVERY client at turn-start -> the reliable cross-client turn signal. Bump serial +
// mark in-game, then chain to original. __thiscall(this, a2) -> __fastcall trick.
int __fastcall hook_turnInfo(void* self, void* /*edx*/, int a2)
{
    // a2 is the broadcast CCmdTurnInfoMsg. Its serialized owner at +0x18 is authoritative on both
    // host and joiner; the local-player accessor is constant for the lifetime of a client and was
    // therefore incapable of detecting later turn transfers.
    int rawOwner = -1;
    __try {
        const char* message = reinterpret_cast<const char*>(a2);
        if (isUserPtr(message) &&
            *reinterpret_cast<void* const*>(message) == reinterpret_cast<void*>(0x6D4B14))
            rawOwner = *reinterpret_cast<const int*>(message + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rawOwner = -1;
    }
    const bool validOwner = rawOwner != 0 && rawOwner != -1 &&
                            rawOwner != kInvalidMidgardId;
    // Debounce: sub_48A680 can fire several times per turn-info (a burst). Treat only a real PLAYER
    // CHANGE as a new turn; otherwise the plugin re-banks+reloads the budget and the turn starts at
    // DOUBLE time (joiner's first turn showed ~90s instead of 45s).
    if (validOwner && rawOwner != g.lastTurnPlayer) {
        g.lastTurnPlayer = rawOwner;
        // Publish this arm before the turn serial: the plugin worker must never observe a fresh
        // turn boundary before its GUI-side readiness fallback is prepared. Normal turns consume
        // it in hookBeginTurnOk; loaded saves have no such callback and consume it in the pump.
        InterlockedExchange(&g.beginTurnReadyTicks, 0);
        InterlockedExchange(&g.beginTurnReadyPending, 1);
        // A queued press belongs only to the turn in which the timer elapsed. Never let it cross a
        // network turn boundary and fire against the next player's fresh, positive clock.
        clearPendingActions();
        clearPostBattleTransition();
        const int player = rawOwner & 0xFFFF;
        pluginhost_bump_turn(player);
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 60)
            tlog("[timer] turn-info off6 owner=%08X player=%d (serial bump)",
                 static_cast<unsigned>(rawOwner), player);
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

struct PhaseGameLockSnapshot
{
    bool available;
    bool locked;
    uint32_t pendingLocalUpdates;
    uint32_t pendingNetworkUpdates;
    unsigned char specialState;
};

PhaseGameLockSnapshot phaseGameLockSnapshot()
{
    PhaseGameLockSnapshot result = {};
    __try {
        // Same validated Russobit ownership chain as featuremenu_my_turn(). `phase` points at the
        // embedded CPhase base at CPhaseGame+8; CPhaseGame::data is therefore phase+8. The native
        // END_TURN callback uses this exact CPhaseGame and calls CheckObjectLock(0x4078B7) before it
        // is allowed to construct/send a command. Waiting on the same runtime gate avoids guessing
        // a fixed delay after combat and also honours a mod's legitimate detour of that EXE entry.
        char* mid = reinterpret_cast<char*(__cdecl*)()>(0x401D35)();
        if (!isUserPtr(mid))
            return result;
        char* midData = *reinterpret_cast<char**>(mid + 8);
        if (!isUserPtr(midData))
            return result;
        char* client = *reinterpret_cast<char**>(midData + 40);
        if (!isUserPtr(client))
            return result;
        char* clientData = *reinterpret_cast<char**>(client + 12);
        if (!isUserPtr(clientData))
            return result;
        char* phase = *reinterpret_cast<char**>(clientData);
        if (!isUserPtr(phase))
            return result;
        char* phaseGame = phase - 8;
        char* phaseGameData = *reinterpret_cast<char**>(phase + 8);
        if (!isUserPtr(phaseGame) || !isUserPtr(phaseGameData) ||
            *reinterpret_cast<char**>(phaseGame + 16) != phaseGameData ||
            *reinterpret_cast<char**>(phaseGameData + 36) != client)
            return result;
        char* objectLock = *reinterpret_cast<char**>(phaseGameData + 64);
        if (!isUserPtr(objectLock))
            return result;

        MEMORY_BASIC_INFORMATION info = {};
        void* const checkObjectLock = reinterpret_cast<void*>(0x4078B7);
        if (!VirtualQuery(checkObjectLock, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return result;
        const DWORD protection = info.Protect & 0xFF;
        if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
            return result;

        result.pendingLocalUpdates = *reinterpret_cast<uint32_t*>(objectLock + 20);
        result.pendingNetworkUpdates = *reinterpret_cast<uint32_t*>(objectLock + 24);
        result.specialState = *reinterpret_cast<unsigned char*>(objectLock + 28);
        result.locked = reinterpret_cast<bool(__thiscall*)(void*)>(checkObjectLock)(phaseGame);
        result.available = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
    }
    return result;
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

// Native Auto Battle UI path for the verified common Discipl2.exe. It is intentionally independent
// of the replaceable mss32.dll mod core: every address and layout below belongs to the game EXE.
// The helpers are signature-gated during install; no fixed UiEvent callback is invoked directly.
// In particular, 0x6355EF is a UiEvent callback which is valid only after the game has created and
// stored its five-millisecond timer event.
constexpr uintptr_t kGetBattleDialog = 0x56CEA4;
constexpr uintptr_t kFindToggleButton = 0x50BB1F;
constexpr uintptr_t kSetToggleChecked = 0x5355B3;
constexpr uintptr_t kAutoBattleToggleCallback = 0x635509;
constexpr unsigned kToggleSetEnabledVtableOffset = 0x8C;
constexpr unsigned kToggleCallOnClickedVtableOffset = 0x94;
constexpr unsigned kAutoBattleSideAOffset = 56;
constexpr unsigned kAutoBattleSideBOffset = 57;
constexpr unsigned kAutoBattleCurrentSideOffset = 58;

const unsigned char kGetBattleDialogSignature[] = {
    0x8B, 0x41, 0x14, 0x8B, 0x40, 0x3C, 0xC3};
const unsigned char kFindToggleButtonSignature[] = {
    0xFF, 0x74, 0x24, 0x08, 0xFF, 0x74, 0x24, 0x08, 0xE8, 0xA2, 0x04, 0x00, 0x00,
    0xC2, 0x08, 0x00};
const unsigned char kSetToggleCheckedSignature[] = {
    0x8B, 0x41, 0x08, 0x8A, 0x54, 0x24, 0x04, 0x88, 0x50, 0x04, 0xE8, 0x95, 0xFF,
    0xFF, 0xFF, 0xC2, 0x04, 0x00};
const unsigned char kAutoBattleToggleCallbackSignature[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x89, 0x4D, 0xF4, 0x8B,
    0x45, 0xF4, 0x8B, 0x48, 0x20, 0x33, 0xD2, 0x8A, 0x51, 0x3A};

bool validateBytes(uintptr_t address, const unsigned char* signature, size_t size)
{
    __try {
        return memcmp(reinterpret_cast<const void*>(address), signature, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool validateNativeAutoBattleUi()
{
    return validateBytes(kGetBattleDialog, kGetBattleDialogSignature,
                         sizeof(kGetBattleDialogSignature)) &&
           validateBytes(kFindToggleButton, kFindToggleButtonSignature,
                         sizeof(kFindToggleButtonSignature)) &&
           validateBytes(kSetToggleChecked, kSetToggleCheckedSignature,
                         sizeof(kSetToggleCheckedSignature));
}

bool executableAddress(const void* address)
{
    if (!isUserPtr(address))
        return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD protection = info.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

void* autoBattleToggle(void* batViewer)
{
    if (!InterlockedExchangeAdd(&g.forceAutoUiAvailable, 0))
        return nullptr;
    void* const outer = battleInterf(batViewer);
    if (!isUserPtr(outer))
        return nullptr;
    __try {
        void* const dialog = reinterpret_cast<void*(__thiscall*)(void*)>(
            kGetBattleDialog)(outer);
        return isUserPtr(dialog)
            ? reinterpret_cast<void*(__stdcall*)(void*, const char*)>(
                  kFindToggleButton)(dialog, "TOG_AUTOBATTLE")
            : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int autoBattleSideOffset(void* batViewer)
{
    char* data = nullptr;
    char* data2 = nullptr;
    if (!battleData(batViewer, &data, &data2))
        return -1;
    __try {
        return data2[kAutoBattleCurrentSideOffset]
            ? kAutoBattleSideAOffset
            : kAutoBattleSideBOffset;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int nativeAutoBattleEnabledForSide(void* batViewer, int sideOffset)
{
    if (sideOffset != static_cast<int>(kAutoBattleSideAOffset) &&
        sideOffset != static_cast<int>(kAutoBattleSideBOffset))
        return -1;
    char* data = nullptr;
    char* data2 = nullptr;
    if (!battleData(batViewer, &data, &data2))
        return -1;
    __try {
        return data2[sideOffset] ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool setNativeAutoBattleForSide(void* batViewer, int sideOffset, bool enabled)
{
    if (sideOffset != static_cast<int>(kAutoBattleSideAOffset) &&
        sideOffset != static_cast<int>(kAutoBattleSideBOffset))
        return false;
    char* data = nullptr;
    char* data2 = nullptr;
    if (!battleData(batViewer, &data, &data2))
        return false;
    __try {
        data2[sideOffset] = enabled ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool setAutoBattleToggleEnabled(void* toggle, bool enabled)
{
    if (!isUserPtr(toggle))
        return false;
    __try {
        void* const vtable = *reinterpret_cast<void**>(toggle);
        void* const method = isUserPtr(vtable)
            ? *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) +
                                        kToggleSetEnabledVtableOffset)
            : nullptr;
        if (!executableAddress(method))
            return false;
        reinterpret_cast<void(__thiscall*)(void*, bool)>(method)(toggle, enabled);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool forcedAutoLatchForViewer(void* batViewer, int* protectedSideOffset)
{
    bool matched = false;
    int sideOffset = -1;
    AcquireSRWLockShared(&g_battleStateLock);
    if (g.forceAutoLatched && g.autoBattleToggleGuardInstalled &&
        batViewer == g.forceAutoLatchedViewer && batViewer == g.battleViewer &&
        g.forceAutoLatchedInstance == g.battleInstance) {
        sideOffset = g.forceAutoLatchedSideOffset;
        matched = sideOffset == static_cast<int>(kAutoBattleSideAOffset) ||
                  sideOffset == static_cast<int>(kAutoBattleSideBOffset);
    }
    ReleaseSRWLockShared(&g_battleStateLock);
    if (protectedSideOffset)
        *protectedSideOffset = matched ? sideOffset : -1;
    return matched;
}

bool latchForcedAutoBattle(void* batViewer, LONG battleInstance, int sideOffset)
{
    if (sideOffset != static_cast<int>(kAutoBattleSideAOffset) &&
        sideOffset != static_cast<int>(kAutoBattleSideBOffset))
        return false;

    bool latched = false;
    AcquireSRWLockExclusive(&g_battleStateLock);
    if (g.autoBattleToggleGuardInstalled && g.battleViewer == batViewer &&
        g.battleInstance == battleInstance && g.battleKindPublished == 1) {
        // Identity is complete before the final interlocked publication of the active bit.
        InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&g.forceAutoLatchedViewer), batViewer);
        InterlockedExchange(&g.forceAutoLatchedInstance, battleInstance);
        InterlockedExchange(&g.forceAutoLatchedSideOffset, sideOffset);
        InterlockedExchange(&g.forceAutoLatched, 1);
        latched = true;
    }
    ReleaseSRWLockExclusive(&g_battleStateLock);
    return latched;
}

bool enforceForcedAutoBattlePresentation(void* batViewer, int protectedSideOffset)
{
    // Preserve the native per-side authority first. The checked/disabled control is presentation
    // and first-line input rejection; the callback detour below is the final cancellation guard.
    const bool flagRestored =
        setNativeAutoBattleForSide(batViewer, protectedSideOffset, true);
    void* const toggle = autoBattleToggle(batViewer);
    if (!isUserPtr(toggle))
        return false;
    bool checked = false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, bool)>(
            kSetToggleChecked)(toggle, true);
        checked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        checked = false;
    }
    const bool disabled = setAutoBattleToggleEnabled(toggle, false);
    return flagRestored && checked && disabled;
}

// TOG_AUTOBATTLE's mouse click and dialog hotkey A both reach this one stored functor. Protect only
// an already-latched side: no window messages are swallowed and no unrelated control is affected.
int __fastcall hookAutoBattleToggle(void* self, void* /*edx*/, unsigned char checked, int a3)
{
    void* const batViewer = isUserPtr(self)
        ? reinterpret_cast<char*>(self) + kBatViewerOffset
        : nullptr;
    int protectedSideOffset = -1;
    if (isUserPtr(batViewer) &&
        forcedAutoLatchForViewer(batViewer, &protectedSideOffset)) {
        const int currentSideOffset = autoBattleSideOffset(batViewer);
        // A failed selector read is fail-closed for this exact callback. It must not become an easy
        // cancellation race during teardown; the stored protected side can still be restored safely.
        if (currentSideOffset < 0 || currentSideOffset == protectedSideOffset) {
            const bool restored =
                enforceForcedAutoBattlePresentation(batViewer, protectedSideOffset);
            if (!checked || !restored) {
                tlog("[timer] forced Auto Battle cancellation blocked "
                     "(viewer=%p side=%d source=%s restored=%d)",
                     batViewer, protectedSideOffset, checked ? "repeat" : "uncheck",
                     restored ? 1 : 0);
            }
            return 1;
        }
    }
    return reinterpret_cast<int(__thiscall*)(void*, unsigned char, int)>(
        g.g_origAutoBattleToggle)(self, checked, a3);
}

bool installAutoBattleToggleGuard()
{
    if (!validateBytes(kAutoBattleToggleCallback,
                       kAutoBattleToggleCallbackSignature,
                       sizeof(kAutoBattleToggleCallbackSignature))) {
        tlog("[timer] TOG_AUTOBATTLE callback signature mismatch; forced Auto disabled");
        return false;
    }

    g.g_origAutoBattleToggle = reinterpret_cast<void*>(kAutoBattleToggleCallback);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g.g_origAutoBattleToggle,
                 reinterpret_cast<void*>(hookAutoBattleToggle));
    if (DetourTransactionCommit() != NO_ERROR) {
        g.g_origAutoBattleToggle = reinterpret_cast<void*>(kAutoBattleToggleCallback);
        tlog("[timer] TOG_AUTOBATTLE cancellation guard detour FAILED");
        return false;
    }
    g.autoBattleToggleGuardInstalled = 1;
    tlog("[timer] forced Auto Battle click/hotkey guard installed "
         "(TOG_AUTOBATTLE 0x635509, hotkey A)");
    return true;
}

enum NativeAutoBattleResult
{
    kNativeAutoDeferred = -1, // the battle control is temporarily covered/not available; retry
    kNativeAutoRejected = 0,  // the native callback was attempted; wait for a new generation
    kNativeAutoApplied = 1,
};

// Runs only on the game GUI thread. This follows the same public UI route as a player click:
// update the checked state, then call CToggleButton::callOnClicked so the stored native functor
// reaches CBattleViewerInterf_OnAutoBattleToggle with its normal argument and object lifetime.
int invokeNativeAutoBattle(void* batViewer, int* appliedSideOffset)
{
    if (appliedSideOffset)
        *appliedSideOffset = -1;
    void* const toggle = autoBattleToggle(batViewer);
    if (!isUserPtr(toggle) || !interfaceOnTop(toggle))
        return kNativeAutoDeferred;

    const int sideOffset = autoBattleSideOffset(batViewer);
    if (sideOffset < 0)
        return kNativeAutoRejected;

    __try {
        void* const vtable = *reinterpret_cast<void**>(toggle);
        void* const onClicked = isUserPtr(vtable)
            ? *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) +
                                        kToggleCallOnClickedVtableOffset)
            : nullptr;
        if (!executableAddress(onClicked))
            return kNativeAutoRejected;

        reinterpret_cast<void(__thiscall*)(void*, bool)>(kSetToggleChecked)(toggle, true);
        int enabled = nativeAutoBattleEnabledForSide(batViewer, sideOffset);
        if (enabled != 1) {
            reinterpret_cast<void(__thiscall*)(void*)>(onClicked)(toggle);
            enabled = nativeAutoBattleEnabledForSide(batViewer, sideOffset);
        }
        if (enabled == 1) {
            // Tournament timeout is final for this battle side. Disable only this native toggle;
            // mouse, keyboard and WM_CHAR remain untouched, so chat and the rest of the UI work.
            if (!setAutoBattleToggleEnabled(toggle, false))
                tlog("[timer] forced Auto Battle toggle disable failed; callback guard remains active");
            if (appliedSideOffset)
                *appliedSideOffset = sideOffset;
            return kNativeAutoApplied;
        }

        // Do not leave a checked-looking control behind when its native callback was rejected.
        reinterpret_cast<void(__thiscall*)(void*, bool)>(kSetToggleChecked)(toggle, false);
        return kNativeAutoRejected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // An exception after entering the native path is not a transient topmost condition. Do not
        // hammer the same control every 32ms; the next accepted ChooseAction gets a new generation.
        return kNativeAutoRejected;
    }
}

LONG volatile g_autoBattleMessage = 0;

UINT autoBattleMessageId()
{
    LONG message = InterlockedExchangeAdd(&g_autoBattleMessage, 0);
    if (!message) {
        const UINT registered = RegisterWindowMessageA("C4dllR.TimerAutoBattle.v2");
        if (registered)
            InterlockedCompareExchange(&g_autoBattleMessage,
                                       static_cast<LONG>(registered), 0);
        message = InterlockedExchangeAdd(&g_autoBattleMessage, 0);
    }
    return static_cast<UINT>(message);
}

bool autoBattleSelectionEligible(const C4P_BattleTimerState& state)
{
    return state.battle_kind == 1 && state.local_active == 1 &&
           state.selection_open == 1 && state.playback_local < 0;
}

bool postForcedAutoBattleIfReady()
{
    if (!InterlockedExchangeAdd(&g.forceAutoRequested, 0) ||
        !InterlockedExchangeAdd(&g.forceAutoUiAvailable, 0))
        return false;

    C4P_BattleTimerState state = {};
    state.struct_size = sizeof(state);
    if (!timerhost_get_battle_timer_state(&state) || !autoBattleSelectionEligible(state))
        return false;
    if (InterlockedExchangeAdd(&g.forceAutoLastInstance, 0) ==
            static_cast<LONG>(state.battle_instance) &&
        InterlockedExchangeAdd(&g.forceAutoLastGeneration, 0) ==
            static_cast<LONG>(state.generation))
        return false;
    if (InterlockedCompareExchange(&g.forceAutoMessagePosted, 1, 0) != 0)
        return true;

    const UINT message = autoBattleMessageId();
    const HWND hwnd = pluginhost_game_hwnd();
    if (!message || !hwnd || !PostMessageA(hwnd, message, 0, 0)) {
        InterlockedExchange(&g.forceAutoMessagePosted, 0);
        return false;
    }
    return true;
}

int dispatchForcedAutoBattle()
{
    if (!InterlockedExchangeAdd(&g.forceAutoRequested, 0) ||
        !InterlockedExchangeAdd(&g.forceAutoUiAvailable, 0) ||
        InterlockedCompareExchange(&g.forceAutoDispatching, 1, 0) != 0)
        return 0;

    InterlockedExchange(&g.forceAutoMessagePosted, 0);
    void* viewer = nullptr;
    LONG instance = -1;
    LONG generation = -1;
    bool eligible = false;

    // Capture the exact open selection before entering game code. The callback can synchronously
    // submit an action and mutate battle state; no SRW lock may be held across that call. The
    // dispatching guard prevents re-entry while this selection has not yet been classified.
    AcquireSRWLockShared(&g_battleStateLock);
    generation = g.battleStateSeq;
    instance = g.battleInstance;
    viewer = g.battleViewer;
    eligible = !(generation & 1) && g.battleKindPublished == 1 &&
               g.battleTurnActive == 1 && g.battleSelectionOpen == 1 &&
               g.battlePlaybackLocal < 0 && isUserPtr(viewer) &&
               !(g.forceAutoLastInstance == instance &&
                 g.forceAutoLastGeneration == generation);
    ReleaseSRWLockShared(&g_battleStateLock);

    int result = kNativeAutoDeferred;
    int appliedSideOffset = -1;
    if (eligible)
        result = invokeNativeAutoBattle(viewer, &appliedSideOffset);
    if (eligible && result != kNativeAutoDeferred) {
        // Only an actual callback attempt consumes this generation. A covered battle control is a
        // normal transient state (for example chat/modal UI) and must retry after it becomes topmost.
        InterlockedExchange(&g.forceAutoLastInstance, instance);
        InterlockedExchange(&g.forceAutoLastGeneration, generation);
    }
    if (result == kNativeAutoApplied) {
        // The UI functor can submit synchronously. Publish the persistent guard only if this is
        // still the same battle instance and the exact side flag accepted by 0x635509 remains set.
        const bool latched =
            nativeAutoBattleEnabledForSide(viewer, appliedSideOffset) == 1 &&
            latchForcedAutoBattle(viewer, instance, appliedSideOffset);
        if (latched) {
            const int currentSideOffset = autoBattleSideOffset(viewer);
            if (currentSideOffset == appliedSideOffset) {
                const bool presentation =
                    enforceForcedAutoBattlePresentation(viewer, appliedSideOffset);
                if (!presentation)
                    tlog("[timer] forced Auto Battle latched; "
                         "checked/disabled presentation will retry");
            } else {
                tlog("[timer] forced Auto Battle latched after side advanced; "
                     "presentation deferred (protected=%d current=%d)",
                     appliedSideOffset, currentSideOffset);
            }
            InterlockedExchange(&g.forceAutoRequested, 0);
            tlog("[timer] forced Auto Battle locked for battle side "
                 "(viewer=%p instance=%ld side=%d)",
                 viewer, instance, appliedSideOffset);
        } else {
            // Teardown/viewer replacement raced the callback. Never publish a stale lock into the
            // next battle; its own timeout request will carry a different instance token.
            result = kNativeAutoRejected;
        }
    }
    if (eligible && result == kNativeAutoRejected) {
        // A semantic submit gate is active while requested. If the native toggle rejected the
        // activation, release that gate immediately rather than trapping the battle on one choice.
        InterlockedExchange(&g.forceAutoRequested, 0);
        tlog("[timer] forced Auto Battle request released after native rejection; manual submit restored");
    }
    if (eligible && result != kNativeAutoDeferred)
        tlog("[timer] forced Auto Battle native toggle %s (viewer=%p instance=%ld generation=%ld)",
             result == kNativeAutoApplied ? "APPLIED" : "REJECTED",
             viewer, instance, generation);
    InterlockedExchange(&g.forceAutoDispatching, 0);
    return result == kNativeAutoApplied ? 1 : 0;
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

bool forcedAutoBlocksManualSubmit(void** batViewerOut, int* protectedSideOffsetOut,
                                  bool* pendingOut)
{
    void* batViewer = nullptr;
    int protectedSideOffset = -1;
    bool pending = false;
    bool latched = false;

    AcquireSRWLockShared(&g_battleStateLock);
    batViewer = g.battleViewer;
    pending = g.forceAutoRequested && g.battleKindPublished == 1 &&
              g.battleTurnActive == 1 && g.battleSelectionOpen == 1 &&
              g.battlePlaybackLocal < 0 && isUserPtr(batViewer);
    if (g.forceAutoLatched && g.autoBattleToggleGuardInstalled &&
        batViewer == g.forceAutoLatchedViewer &&
        g.forceAutoLatchedInstance == g.battleInstance) {
        protectedSideOffset = g.forceAutoLatchedSideOffset;
        latched = protectedSideOffset == static_cast<int>(kAutoBattleSideAOffset) ||
                  protectedSideOffset == static_cast<int>(kAutoBattleSideBOffset);
    }
    ReleaseSRWLockShared(&g_battleStateLock);

    if (!pending && latched) {
        const int currentSideOffset = autoBattleSideOffset(batViewer);
        // The exact protected viewer/instance is known. A transient selector read failure must not
        // turn into a one-click bypass while the native Auto UiEvent is pending.
        latched = currentSideOffset < 0 || currentSideOffset == protectedSideOffset;
    }
    if (batViewerOut)
        *batViewerOut = batViewer;
    if (protectedSideOffsetOut)
        *protectedSideOffsetOut = protectedSideOffset;
    if (pendingOut)
        *pendingOut = pending;
    return pending || latched;
}

// CTaskBattle's two IBatNotify entry points (ordinary BattleAction and UseItem) converge on this
// exact sender. It is the authoritative end of the current local decision window. ChooseAction is
// delivered only to the client that must choose, so it cannot be used as a broadcast owner stream:
// after this submit the timer must remain stopped until another local ChooseAction opens a choice.
// A mandatory second-hit target arrives as its own ChooseAction and is therefore charged normally.
void __fastcall hookBattleSubmit(void* self, void* /*edx*/, const void* battleMsgData,
                                 int action, const void* targetId, const void* attackerId)
{
    // Raw ABI arguments, plus guarded ID snapshots only while diagnostics are enabled.
    c4trace_event(C4TRACE_BATTLE_SUBMIT, reinterpret_cast<uintptr_t>(self), action,
                  reinterpret_cast<uintptr_t>(battleMsgData), reinterpret_cast<uintptr_t>(targetId),
                  reinterpret_cast<uintptr_t>(attackerId));
    if (c4trace_enabled()) {
        const DWORD saved = GetLastError();
        unsigned target = 0xffffffffu, actor = 0xffffffffu;
        __try { if (targetId) target = *reinterpret_cast<const unsigned*>(targetId); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { if (attackerId) actor = *reinterpret_cast<const unsigned*>(attackerId); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        c4trace_event(C4TRACE_BATTLE_IDS, reinterpret_cast<uintptr_t>(self), target, actor,
                      action, reinterpret_cast<uintptr_t>(battleMsgData));
        SetLastError(saved);
    }
    if (action != kBattleActionAuto) {
        void* forcedViewer = nullptr;
        int protectedSideOffset = -1;
        bool pending = false;
        if (forcedAutoBlocksManualSubmit(&forcedViewer, &protectedSideOffset, &pending)) {
            c4trace_event(C4TRACE_AUTO_BLOCK, reinterpret_cast<uintptr_t>(forcedViewer),
                          action, protectedSideOffset, pending, 0);
            // Do not close selection_open and do not touch window input. The queued/normal native
            // Auto callback remains free to submit action 5; chat and every non-battle command work.
            if (!pending && protectedSideOffset >= 0)
                enforceForcedAutoBattlePresentation(forcedViewer, protectedSideOffset);
            tlog("[timer] manual battle action blocked by forced Auto "
                 "(action=%d viewer=%p side=%d phase=%s)",
                 action, forcedViewer, protectedSideOffset,
                 pending ? "requested" : "latched");
            return;
        }
    }

    if (isUserPtr(g.battleViewer) &&
        InterlockedExchangeAdd(&g.battleTurnActive, 0) == 1) {
        // Close only this choice. Keep the parsed local-side and double-attack continuation fields;
        // selection_open is the timer authority until the next ChooseAction event.
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
    c4trace_event(C4TRACE_BATTLE_SUBMIT_RETURN, reinterpret_cast<uintptr_t>(self), action, 0, 0, 0);
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
    // Clear the load-resume arm before entering game code. If the native callback pumps a nested
    // WM_TIMER while closing its modal, that tick must not publish an early synthetic acceptance.
    InterlockedExchange(&g.beginTurnReadyPending, 0);
    InterlockedExchange(&g.beginTurnReadyTicks, 0);
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
    // Idempotent fallback if a non-standard viewer path skipped the normal DLG_BATTLE_B create hook.
    pluginhost_queue_battle_state(1);
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
        InterlockedExchange(&g.forceAutoMessagePosted, 0);
        InterlockedExchange(&g.forceAutoLastInstance, -1);
        InterlockedExchange(&g.forceAutoLastGeneration, -1);
        clearForcedAutoLatch();
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
    c4trace_event(C4TRACE_BATTLE_SELECT, reinterpret_cast<uintptr_t>(batViewer),
                  receive, diag.activeId, localActive, (valid ? 1u : 0u) | (continuation ? 2u : 0u));
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
    // Result playback is still part of the visible battle UI lifetime.
    pluginhost_queue_battle_state(1);
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
        InterlockedExchange(&g.forceAutoMessagePosted, 0);
        InterlockedExchange(&g.forceAutoLastInstance, -1);
        InterlockedExchange(&g.forceAutoLastGeneration, -1);
        clearForcedAutoLatch();
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
    c4trace_event(C4TRACE_BATTLE_RESULT, reinterpret_cast<uintptr_t>(batViewer),
                  serial, diag.activeId, localPlayback, valid);
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
        if (opened) {
            c4trace_event(C4TRACE_BATTLE_OPEN, reinterpret_cast<uintptr_t>(batViewer),
                          g.battleInstance, g.battleStateSeq, 0, 0);
            tlog("[timer] local battle selection opened (viewer=%p)", batViewer);
            postForcedAutoBattleIfReady();
        }
    }

    // Native battle refresh mirrors the side flag back into the control. Reassert checked+disabled
    // whenever the protected side is current, covering UI recreation without owning general input.
    int protectedSideOffset = -1;
    if (forcedAutoLatchForViewer(batViewer, &protectedSideOffset)) {
        const int currentSideOffset = autoBattleSideOffset(batViewer);
        if (currentSideOffset == protectedSideOffset) {
            if (!enforceForcedAutoBattlePresentation(batViewer, protectedSideOffset))
                tlog("[timer] forced Auto Battle presentation retry deferred "
                     "(viewer=%p side=%d)", batViewer, protectedSideOffset);
        }
    }
}

extern "C" void timerhost_on_battle_end(void)
{
    // IBatViewer teardown happens before reward/artifact dialogs have necessarily returned to the
    // strategic interface. Preserve that provenance so a timeout in the transition takes the same
    // strict path as a timeout which happened while BTN_CLOSE was still alive.
    InterlockedExchange(&g.postBattleTransition, 1);
    InterlockedExchange(&g.postBattleReadyTicks, 0);
    clearBattleState();
}

extern "C" int timerhost_filter_input(UINT msg, WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    // Fail open. The released forced-Auto path used this hook to swallow all mouse, keyboard and
    // WM_CHAR input while waiting for a local submit. A lost/rejected multiplayer action then
    // locked both gameplay and chat indefinitely. Manual native Auto Battle needs no input filter.
    (void)msg;
    return 0;
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
int queueForcedAutoBattle(uint32_t expectedBattleInstance, bool requireExpectedInstance)
{
    if (!g.installed || !InterlockedExchangeAdd(&g.forceAutoUiAvailable, 0))
        return 0;

    // The plugin reached 00:00 from a coherent billable-local snapshot, but the game thread may
    // advance to Result/opponent selection before this worker-thread call samples host state. V2
    // accepts the intent only for that same live PvP instance and defers to its next local choice;
    // legacy compatibility can target only the safe current instance because its ABI has no token.
    LONG instance = -1;
    LONG generation = -1;
    bool livePvp = false;
    AcquireSRWLockShared(&g_battleStateLock);
    instance = g.battleInstance;
    generation = g.battleStateSeq;
    livePvp = !(generation & 1) && g.battleKindPublished == 1 &&
              isUserPtr(g.battleViewer) &&
              (!requireExpectedInstance ||
               static_cast<uint32_t>(instance) == expectedBattleInstance);
    if (livePvp)
        InterlockedExchange(&g.forceAutoRequested, 1);
    ReleaseSRWLockShared(&g_battleStateLock);
    if (!livePvp)
        return 0;

    // The caller may be the timer worker. It only publishes a request under the battle-state lock;
    // teardown/viewer replacement must therefore either precede validation or clear it afterward.
    // Native UI code is reached later through the window message (or GUI WM_TIMER fallback).
    postForcedAutoBattleIfReady();
    tlog("[timer] forced Auto Battle queued (instance=%ld generation=%ld)",
         instance, generation);
    return 1;
}

extern "C" int timerhost_force_auto_battle(void)
{
    // Compatibility for the 1.8 plugin, which has no instance argument. It still uses the safe
    // native-UI enqueue and never takes input ownership; current plugins use the strict v2 entry.
    return queueForcedAutoBattle(0, false);
}

extern "C" int timerhost_force_auto_battle_v2(uint32_t expectedBattleInstance)
{
    return queueForcedAutoBattle(expectedBattleInstance, true);
}

extern "C" int timerhost_auto_battle_message(UINT msg)
{
    const UINT expected = autoBattleMessageId();
    if (!expected || msg != expected)
        return 0;
    dispatchForcedAutoBattle();
    return 1;
}
// Phase 2 on-elapse: the worker queues work and timerhost_pump() waits for a safe idle WM_TIMER.
// Local intermediate dialogs may be revisited; the final networked END_TURN click is one-shot.
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
                            isUserPtr(g.btnDefend) || isUserPtr(g.btnClose) ||
                            InterlockedExchangeAdd(&g.postBattleTransition, 0) != 0;
    const LONG localOrigin = featuremenu_my_turn();
    InterlockedExchange(&g.pendingEndDayBattle, fromBattle ? 1 : 0);
    InterlockedExchange(&g.pendingEndDayLocalOrigin, localOrigin);
    InterlockedExchange(&g.battleClosePressed, 0);
    InterlockedExchange(&g.strategicReadyTicks, 0);
    InterlockedExchange(&g.endDayLockWaitLogged, 0);
    InterlockedExchange(&g.pendingEndDay, 1);
    tlog("[timer] end_day QUEUED by plugin "
         "(fromBattle=%d localOrigin=%ld battleKind=%ld instance=%ld)",
         fromBattle ? 1 : 0, localOrigin,
         InterlockedExchangeAdd(&g.battleKindPublished, 0),
         InterlockedExchangeAdd(&g.battleInstance, 0));
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
    // Load Game restores clientTakesTurn and the strategic interface but never executes the normal
    // DLG_BEGIN_TURN/BTN_OK callback. Publish the same readiness serial after two 32-ms GUI ticks
    // prove that the ordinary END_TURN control is enabled and genuinely topmost. During a normal
    // turn-start modal interfaceOnTop(endTurn) stays false, so BTN_OK remains the only fast path.
    if (InterlockedExchangeAdd(&g.beginTurnReadyPending, 0)) {
        const bool ready = featuremenu_my_turn() == 1 && isUserPtr(g.endTurn) &&
                           btnEnabled(g.endTurn) && interfaceOnTop(g.endTurn);
        if (!ready) {
            InterlockedExchange(&g.beginTurnReadyTicks, 0);
        } else if (InterlockedIncrement(&g.beginTurnReadyTicks) >= 2 &&
                   InterlockedCompareExchange(&g.beginTurnReadyPending, 0, 1) == 1) {
            InterlockedExchange(&g.beginTurnReadyTicks, 0);
            const LONG serial = InterlockedIncrement(&g.beginTurnAckSerial);
            tlog("[timer] loaded/resumed strategic turn accepted on stable topmost UI "
                 "(owner=%08X ack=%ld)",
                 static_cast<unsigned>(g.lastTurnPlayer), serial);
        }
    } else {
        InterlockedExchange(&g.beginTurnReadyTicks, 0);
    }

    // GUI-thread fallback for the private message path. It also covers the brief startup interval
    // before pluginhost can resolve the canonical game HWND.
    if (InterlockedExchangeAdd(&g.forceAutoRequested, 0) &&
        !InterlockedExchangeAdd(&g.forceAutoMessagePosted, 0))
        dispatchForcedAutoBattle();

    // Retire post-battle provenance only after the ordinary strategic END_TURN control is topmost
    // for two idle ticks. Until then a reward/artifact/modal interface must keep End Day on the
    // strict post-battle path even though IBatViewer has already been destroyed.
    if (InterlockedExchangeAdd(&g.postBattleTransition, 0)) {
        const int myTurn = featuremenu_my_turn();
        const bool stableStrategic = myTurn == 1 && isUserPtr(g.endTurn) &&
            btnEnabled(g.endTurn) && interfaceOnTop(g.endTurn);
        if (!stableStrategic) {
            InterlockedExchange(&g.postBattleReadyTicks, 0);
        } else if (InterlockedIncrement(&g.postBattleReadyTicks) >= 2) {
            clearPostBattleTransition();
            tlog("[timer] post-battle transition retired on stable strategic UI");
        }
    }

    // Called ONLY on WM_TIMER (featuremenu's 32ms timer), outside the live mouse/key callback stack.
    // That is necessary for UI lifetime safety but does not prove command readiness: after combat the
    // strategic interface can already be visible while CPhaseGame's object lock is still held below.
    if (!g.pendingEndDay)
        return;
    // The button press tears down strategic UI and is unsafe in the middle of a drag. Keep this
    // request queued until the mouse is released.
    const bool endDayBlockedByDrag =
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (endDayBlockedByDrag)
        return;
    if (InterlockedCompareExchange(&g.inAction, 1, 0) != 0)
        return; // re-entry guard: the press pumps messages; never recurse into a second press
    __try {
        if (g.pendingEndDay && !endDayBlockedByDrag) {
            const bool battleUi = isUserPtr(g.btnRetreat) || isUserPtr(g.btnDefend) ||
                                  isUserPtr(g.btnClose);
            if (battleUi) {
                // Never retreat. Wait for the resolved state, then use the ordinary BTN_CLOSE once.
                if (!g.battleClosePressed && isUserPtr(g.btnClose) && btnEnabled(g.btnClose)) {
                    InterlockedExchange(&g.battleClosePressed, 1);
                    InterlockedExchange(&g.strategicReadyTicks, 0);
                    InterlockedExchange(&g.postBattleTransition, 1);
                    InterlockedExchange(&g.postBattleReadyTicks, 0);
                    tlog("[timer] battle resolved; pressing BTN_CLOSE before End Day");
                    pressBtn(g.btnClose);
                }
            } else {
                const int myTurn = featuremenu_my_turn();
                const LONG localOrigin =
                    InterlockedExchangeAdd(&g.pendingEndDayLocalOrigin, 0);
                if (g.pendingEndDayBattle && localOrigin != 1) {
                    // End Day belongs only to the client which owned the strategic turn when its
                    // clock expired. A defender's local battle clock can also reach zero and force
                    // Auto Battle, but must never consume the attacker's strategic turn. Unknown
                    // provenance fails closed for the same reason.
                    tlog("[timer] post-battle End Day discarded: timeout did not originate from "
                         "the local strategic owner (current=%d origin=%ld); attacker's turn preserved",
                         myTurn, localOrigin);
                    clearPendingActions();
                } else if (myTurn == 0 && !g.pendingEndDayBattle) {
                    tlog("[timer] End Day discarded: local strategic turn is not active "
                         "(current=%d origin=%ld postBattle=0)", myTurn, localOrigin);
                    clearPendingActions();
                } else if (g.pendingEndDayBattle) {
                    // Post-battle path is intentionally strict. Victory/defeat screens do not expose
                    // an enabled strategic END_TURN button, and we never press their Continue control.
                    const bool strategicReady =
                        myTurn == 1 && isUserPtr(g.endTurn) && btnEnabled(g.endTurn) &&
                        interfaceOnTop(g.endTurn);
                    if (!strategicReady) {
                        InterlockedExchange(&g.strategicReadyTicks, 0);
                        InterlockedExchange(&g.endDayLockWaitLogged, 0);
                    } else {
                        const PhaseGameLockSnapshot objectLock = phaseGameLockSnapshot();
                        if (!objectLock.available || objectLock.locked) {
                            InterlockedExchange(&g.strategicReadyTicks, 0);
                            if (InterlockedCompareExchange(&g.endDayLockWaitLogged, 1, 0) == 0) {
                                if (objectLock.available) {
                                    tlog("[timer] post-battle END_TURN deferred by native object lock "
                                         "(local=%lu network=%lu special=%u)",
                                         static_cast<unsigned long>(objectLock.pendingLocalUpdates),
                                         static_cast<unsigned long>(objectLock.pendingNetworkUpdates),
                                         static_cast<unsigned>(objectLock.specialState));
                                } else {
                                    tlog("[timer] post-battle END_TURN deferred: native object-lock "
                                         "state is not yet available");
                                }
                            }
                        } else if (InterlockedIncrement(&g.strategicReadyTicks) >= 2) {
                            if (InterlockedExchange(&g.endDayLockWaitLogged, 0)) {
                                tlog("[timer] native object lock released "
                                     "(local=%lu network=%lu special=%u)",
                                     static_cast<unsigned long>(objectLock.pendingLocalUpdates),
                                     static_cast<unsigned long>(objectLock.pendingNetworkUpdates),
                                     static_cast<unsigned>(objectLock.specialState));
                            }
                            tlog("[timer] strategic UI and native command gate stably ready; "
                                 "pressing END_TURN after battle");
                            // The native click is a network submission, not an idempotent poll. Consume
                            // the request before entering game code so WM_TIMER cannot send it again
                            // while the turn-info broadcast is in flight.
                            InterlockedExchange(&g.pendingEndDay, 0);
                            InterlockedExchange(&g.pendingEndDayBattle, 0);
                            InterlockedExchange(&g.suppressEndTurnConfirm, 1);
                            pressBtn(g.endTurn);
                            InterlockedExchange(&g.suppressEndTurnConfirm, 0);
                        }
                    }
                } else if (myTurn != 0) {
                    // Preserve the legacy non-battle End Day priority chain for ordinary timeouts.
                    void* order[4] = {g.briefCont, g.capBack, g.diploBack, g.endTurn};
                    for (int i = 0; i < 4; ++i) {
                        if (!isUserPtr(order[i]) || !btnEnabled(order[i]))
                            continue;
                        const bool endTurn = order[i] == g.endTurn;
                        if (endTurn && !interfaceOnTop(g.endTurn))
                            continue;
                        tlog("[timer] pressing btn[%d]=%p (WM_TIMER idle)", i, order[i]);
                        if (endTurn) {
                            InterlockedExchange(&g.pendingEndDay, 0);
                            InterlockedExchange(&g.pendingEndDayBattle, 0);
                            InterlockedExchange(&g.suppressEndTurnConfirm, 1);
                        }
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
    InterlockedExchange(&g.beginTurnReadyPending, 0);
    InterlockedExchange(&g.beginTurnReadyTicks, 0);
    InterlockedExchange(&g.battleTurnActive, -1);
    InterlockedExchange(&g.battlePlaybackLocal, -1);
    InterlockedExchange(&g.forceAutoLastInstance, -1);
    InterlockedExchange(&g.forceAutoLastGeneration, -1);
    InterlockedExchange(&g.forceAutoLatchedInstance, -1);
    InterlockedExchange(&g.forceAutoLatchedSideOffset, -1);

    const bool localPlayerAccessorValid = validateLocalNetworkPlayerIdAccessor();
    InterlockedExchange(&g_localNetworkPlayerIdAccessorAvailable,
                        localPlayerAccessorValid ? 1 : 0);
    if (localPlayerAccessorValid)
        tlog("[timer] local-player accessor validated (0x403384, netPlayerClientPtr +32)");
    else
        tlog("[timer] local-player accessor signature mismatch; PvP decision timing unavailable");

    const bool autoBattleUiHelpersValid = validateNativeAutoBattleUi();
    const bool autoBattleGuardValid =
        autoBattleUiHelpersValid && installAutoBattleToggleGuard();
    const bool autoBattleUiValid = autoBattleUiHelpersValid && autoBattleGuardValid;
    InterlockedExchange(&g.forceAutoUiAvailable, autoBattleUiValid ? 1 : 0);
    if (autoBattleUiValid)
        tlog("[timer] native Auto Battle UI path + click/hotkey guard validated");
    else
        tlog("[timer] native Auto Battle signature/guard mismatch; forced Auto remains fail-closed");

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
    // off[5] CMidClient::vftable[0] destructor -> drop all captures at scenario teardown.
    g.g_orig_midClientDtor =
        patchVtableSlot(0x6CEB5C, 0, reinterpret_cast<void*>(hook_midClientDestroy));

    // Exact Russobit/MNS 3.01a callsite used by the original timer.mod off[13] table. Signature-gated:
    // on another executable this remains a clean no-op and automatic End Day is not queued.
    installEndTurnConfirmHook();
    // Exact Russobit begin-turn confirmation callback. Other executable families deliberately keep
    // the existing active-turn edge because this address is not assumed to be portable.
    installBeginTurnAckHook();
    // Forced Auto enters through the toggle's ordinary callOnClicked path. The callback detour above
    // does not trigger Auto and never calls 0x6355EF: it only rejects later cancellation of the exact
    // latched side. General mouse/key/WM_CHAR input is never captured.
    // Exact common IBatNotify sender: closes the local selection interval for every action kind.
    installBattleSubmitHook();

    tlog("[timer] keystone capture installed (off9 dlg-create 0x5C93D6, off8 btn-dtor 0x6E3294, "
         "off5 CMidClient-dtor 0x6CEB5C)");
}
