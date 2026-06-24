/*
 * C4dll-R timer host: ports timer.mod's off[] game-hook layer (dialog/button capture, combat/anim
 * classification, on-elapse actions) into the renderer DLL; exposed to timer.c4p via C4P_Host.
 * Russobit "- Copy" exe ONLY: addresses hardcoded (base 0x400000, reloc 0), like featuremenu.cpp.
 * Detours run on the game UI thread, plugin polls from the worker -> shared fields volatile, derefs SEH-guarded.
 */
#include <windows.h>
#include <cstdint>
#include <cstdarg>
#include <detours.h>

// In pluginhost.cpp; bumped by the off[6] turn-info hook (a NETWORK msg firing on EVERY client),
// so turn-starts are detected even on a pure client (MP joiner) with no server.
extern "C" void pluginhost_bump_turn(int player);
extern "C" void pluginhost_turn_reset(void);
// Client-valid scenario-day source (featuremenu.cpp); logged per turn edge to verify the per-day budget.
extern "C" int featuremenu_current_day(void);
// "Is it my turn" = CPhaseGameData.clientTakesTurn (sub-dialog-immune); -1 if unavailable -> fallback.
extern "C" int featuremenu_my_turn(void);

namespace {

void tlog(const char* fmt, ...)
{
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
    HANDLE h = CreateFileA(logName, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
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
    LONG volatile pendingEndDay;  // queued by plugin on elapse; consumed on game thread (pump)
    LONG volatile pendingRetreat;
    LONG volatile inAction;       // re-entry guard around the game-thread press (legacy dword_10008388)
    void* g_orig_dlgCreate;
    void* g_orig_btnDtor;
    void* g_orig_scenInit;
    void* g_orig_turnInfo;
    int lastTurnPlayer;     // last off[6] player byte (debounce: ignore same-player bursts)
    int installed;
} g;

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
    pluginhost_turn_reset();   // game/scenario change -> clear off[6] in-game flag
    g.lastTurnPlayer = -1;     // re-arm player debounce for the new game's first turn
    return reinterpret_cast<int(__thiscall*)(void*, int)>(g.g_orig_scenInit)(self, a2);
}

// off[6] turn-info handler (sub_48A680 @0x48A680 = CCmdTurnInfoMsg processor). A NETWORK message,
// dispatched on EVERY client at turn-start -> the reliable cross-client turn signal. Bump serial +
// mark in-game, then chain to original. __thiscall(this, a2) -> __fastcall trick.
int __fastcall hook_turnInfo(void* self, void* /*edx*/, int a2)
{
    // Current turn player byte, legacy way (sub_10001B90 did v3 = *sub_404E71()). Pure client-side
    // getter, valid on BOTH MP clients: CMidgard @0x401D35 -> data @+8 -> obj @+32 -> player id @ obj+8.
    int player = -1;
    __try {
        char* mid = reinterpret_cast<char*(__cdecl*)()>(0x401D35)();
        if (isUserPtr(mid)) {
            char* data = *reinterpret_cast<char**>(mid + 8);
            if (isUserPtr(data)) {
                char* obj = *reinterpret_cast<char**>(data + 32);
                if (isUserPtr(obj))
                    player = *reinterpret_cast<unsigned char*>(obj + 8);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        player = -1;
    }
    // Debounce: sub_48A680 can fire several times per turn-info (a burst). Treat only a real PLAYER
    // CHANGE as a new turn; otherwise the plugin re-banks+reloads the budget and the turn starts at
    // DOUBLE time (joiner's first turn showed ~90s instead of 45s).
    if (player >= 0 && player != g.lastTurnPlayer) {
        g.lastTurnPlayer = player;
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

} // namespace

// Exported read accessors (lock-free volatile reads; called by pluginhost thunks).
extern "C" int timerhost_is_animating(void) { return g.animFlag ? 1 : 0; }
extern "C" int timerhost_battle_kind(void) { return g.pvpFlag ? 1 : (g.anyCombat ? 2 : 0); }
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
extern "C" int timerhost_turn_player_id(void) { return -1; } // set by off[6] (Phase 1b)
// Phase 2 on-elapse: plugin (worker thread) QUEUES the press; timerhost_pump() performs it on each
// idle WM_TIMER (featuremenu's 32ms timer), retrying until the turn ends - like the legacy 0x113 case.
extern "C" int timerhost_retreat(void) { InterlockedExchange(&g.pendingRetreat, 1); tlog("[timer] retreat QUEUED by plugin"); return 1; }
extern "C" int timerhost_end_day(void) { InterlockedExchange(&g.pendingEndDay, 1); tlog("[timer] end_day QUEUED by plugin"); return 1; }

extern "C" void timerhost_pump(void)
{
    // Called ONLY on WM_TIMER (featuremenu's 32ms timer) = an idle point, after the hero finished
    // moving. Pressing END_TURN mid-move fires onClick but the game rejects the turn-end.
    if (!g.pendingEndDay && !g.pendingRetreat)
        return;
    // Never press while the left button is physically held (END_TURN tears down UI -> use-after-free
    // mid-drag). Pending flag persists -> retried next WM_TIMER.
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
        return;
    if (InterlockedCompareExchange(&g.inAction, 1, 0) != 0)
        return; // re-entry guard: the press pumps messages; never recurse into a second press
    __try {
        // A captured RETREAT button = a real battle dialog is up. In combat RETREAT is a one-shot flee;
        // PRESERVE pendingEndDay so an out-of-time turn still auto-ends once combat is over.
        const bool inBattle = isUserPtr(g.btnRetreat);
        if (inBattle) {
            if (InterlockedExchange(&g.pendingRetreat, 0)) { // one-shot: never re-press a flee
                if (btnEnabled(g.btnRetreat))
                    pressBtn(g.btnRetreat);
            }
        } else {
            InterlockedExchange(&g.pendingRetreat, 0); // retreat does not apply outside combat
            if (g.pendingEndDay) {
                // Retry each idle WM_TIMER until the turn ends (no give-up cap: dropping it would leave
                // the turn un-skipped). Press the first enabled advance button.
                if (isUserPtr(g.endTurn) && btnEnabled(g.endTurn)) {
                    void* order[5] = { g.btnClose, g.briefCont, g.capBack, g.diploBack, g.endTurn };
                    for (int i = 0; i < 5; ++i) {
                        if (isUserPtr(order[i]) && btnEnabled(order[i])) {
                            tlog("[timer] pressing btn[%d]=%p (WM_TIMER idle)", i, order[i]);
                            pressBtn(order[i]);
                            break;
                        }
                    }
                } else {
                    InterlockedExchange(&g.pendingEndDay, 0); // turn ended (or no longer ours) -> done
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

    tlog("[timer] keystone capture installed (off9 dlg-create 0x5C93D6, off8 btn-dtor 0x6E3294, "
         "off5 scen-init 0x6CEB5C)");
}
