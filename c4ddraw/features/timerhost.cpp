/*
 * C4dll-R timer host keystone. Ports the legacy timer.mod off[] game-hook layer (dialog/button capture,
 * combat + animation classification, and - later - the on-elapse actions) into the in-process renderer
 * DLL, and exposes it to the timer.c4p plugin via C4P_Host. Russobit "- Copy" exe only: addresses are
 * hardcoded (image base 0x400000, reloc delta 0), same convention as featuremenu.cpp.
 *
 * Detours fire on the game UI thread; the plugin polls from the overlay worker -> shared fields are
 * volatile and every game-pointer deref is SEH-guarded. Built incrementally (keystone plan):
 *   Phase 1a (THIS): capture - off[9] dialog-create (DetourAttach 0x5C93D6, verified __stdcall 5-arg),
 *                    off[8] CButtonInterf::vftable[0] destructor (null-on-destroy), off[5] CMidClient
 *                    vftable[0] scenario-init reset. Gives is_animating + battle_kind + the captured
 *                    buttons. Observe-only (reads + null-writes); no game CALLs.
 *   Phase 1b (next): off[10]/off[11] toggle capture, off[6]/off[7] turn/ACTIVE/day.
 *   Phase 2 (next):  the gated on-elapse vtable CALLS (end_day / retreat).
 */
#include <windows.h>
#include <cstdint>
#include <cstdarg>
#include <detours.h>

namespace {

// --- minimal self-contained helpers (mirror featuremenu.cpp's) ---
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
    HANDLE h = CreateFileA("C4menu.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
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

// Save the existing vtable method pointer, write our detour into the slot. Returns the original.
void* patchVtableSlot(uintptr_t vtableVA, unsigned byteOff, void* detour)
{
    void* orig = *reinterpret_cast<void**>(vtableVA + byteOff);
    writeBytes(vtableVA + byteOff, &detour, sizeof(detour));
    return orig;
}

// --- captured state (volatile: written by game-thread detours, read by the worker via the accessors) ---
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
    LONG volatile actionsEnabled; // gate for the on-elapse vtable CALLs (Phase 2; default 0)
    LONG volatile pendingEndDay;  // queued by the plugin on elapse; consumed on the game thread (pump)
    LONG volatile pendingRetreat;
    LONG volatile inAction;       // re-entry guard around the game-thread press (legacy dword_10008388)
    // saved originals / trampolines
    void* g_orig_dlgCreate;
    void* g_orig_btnDtor;
    void* g_orig_scenInit;
    int installed;
} g;

// off[9] dialog/button-create capture. Verified: int __stdcall sub_5C93D6(iface, btnName, dlgName, a4, a5).
int __stdcall hook_dlgCreate(int ifaceObj, char* btnName, const char* dlgName, int cb1, int cb2)
{
    int obj = reinterpret_cast<int(__stdcall*)(int, char*, const char*, int, int)>(g.g_orig_dlgCreate)(
        ifaceObj, btnName, dlgName, cb1, cb2);
    if (!obj || !btnName || !dlgName)
        return obj;
    void* o = reinterpret_cast<void*>(obj);

    if (!lstrcmpA(dlgName, "DLG_STRATEGIC") && !lstrcmpA(btnName, "BTN_END_TURN"))
        g.endTurn = o;
    else if (!lstrcmpA(dlgName, "DLG_CAPITAL") && !lstrcmpA(btnName, "BTN_BACK"))
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
                // anim flag = the DEFEND button's hidden/disabled byte == 0 (an attack is animating)
                void* f8 = *reinterpret_cast<void**>(reinterpret_cast<char*>(o) + 8);
                g.animFlag = (*(reinterpret_cast<unsigned char*>(f8) + 4) == 0) ? 1 : 0;
                // PvP byte = *(BYTE*)[ [[ [iface+4]+8]+0x1C]+0x14F8 ]   (off[3]=0x14F8 player-struct byte)
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
    if (self == g.endTurn)
        g.endTurn = nullptr;
    else if (self == g.capBack)
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
    return reinterpret_cast<int(__thiscall*)(void*, int)>(g.g_orig_scenInit)(self, a2);
}

// CButtonInterf enabled flag (legacy *([btn+8]+4) != 0) + the press action (vtable+0xB0), SEH-guarded.
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
    __try {
        void* vtable = *reinterpret_cast<void**>(btn);
        void* method = *reinterpret_cast<void**>(reinterpret_cast<char*>(vtable) + 0xB0);
        reinterpret_cast<void(__thiscall*)(void*)>(method)(btn); // CButtonInterf "press" (legacy +0xB0)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

// --- exported read accessors (lock-free single-word volatile reads; called by pluginhost thunks) ---
extern "C" int timerhost_is_animating(void) { return g.animFlag ? 1 : 0; }
extern "C" int timerhost_battle_kind(void) { return g.pvpFlag ? 1 : (g.anyCombat ? 2 : 0); }
extern "C" int timerhost_turn_active(void) { return 0; }    // set by off[6] (Phase 1b)
extern "C" int timerhost_turn_player_id(void) { return -1; } // set by off[6] (Phase 1b)
// Phase 2 on-elapse actions: the plugin (overlay worker thread) QUEUES the press; timerhost_pump()
// performs it on the game UI thread (from the featuremenu WndProc detour), exactly like the legacy
// posted a message to its WndProc which pressed the captured button (sub_10004D40 press = vtable+0xB0).
extern "C" int timerhost_retreat(void) { InterlockedExchange(&g.pendingRetreat, 1); return 1; }
extern "C" int timerhost_end_day(void) { InterlockedExchange(&g.pendingEndDay, 1); tlog("[timer] end_day QUEUED by plugin"); return 1; }

extern "C" void timerhost_pump(void)
{
    if (!g.pendingEndDay && !g.pendingRetreat)
        return;
    // SAFETY GATE (crash fix). Only fire the press when the user is NOT mid-interaction. Pressing
    // END_TURN is a heavy turn-transition dispatch; doing it while the left button is physically held,
    // or while the mouse is captured (our map drag-scroll OR a game modal), raced the game's deferred
    // UI teardown and crashed with a use-after-free - Discipl2 sub_4D9A73+0x49 does a virtual call
    // (this->vtbl[5]) on a dialog object that the press had already destroyed. Defer until idle: the
    // pending flag persists and the next message retries. Mirrors the legacy timer, which pressed from
    // a POSTED message (dispatched at a clean, non-input point), never inline on a live mouse message.
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || GetCapture() != nullptr)
        return;
    if (InterlockedCompareExchange(&g.inAction, 1, 0) != 0)
        return; // re-entry guard: the press pumps messages; never recurse into a second press
    __try {
        if (InterlockedExchange(&g.pendingRetreat, 0)) {
            void* b = g.btnRetreat; // in-combat: press the captured RETREAT button
            if (isUserPtr(b) && btnEnabled(b))
                pressBtn(b);
        }
        if (InterlockedExchange(&g.pendingEndDay, 0)) {
            // End-Day only OUTSIDE combat (no RETREAT button up); press the first enabled advance button
            // in the legacy priority: close -> briefing-continue -> capital/diplomacy-back -> end-turn.
            tlog("[timer] pump end_day: retreat=%p close=%p brief=%p cap=%p diplo=%p endTurn=%p en=%d",
                 g.btnRetreat, g.btnClose, g.briefCont, g.capBack, g.diploBack, g.endTurn,
                 isUserPtr(g.endTurn) ? (btnEnabled(g.endTurn) ? 1 : 0) : -1);
            if (!isUserPtr(g.btnRetreat)) {
                void* order[5] = { g.btnClose, g.briefCont, g.capBack, g.diploBack, g.endTurn };
                for (int i = 0; i < 5; ++i) {
                    if (isUserPtr(order[i]) && btnEnabled(order[i])) {
                        tlog("[timer] pressing btn[%d]=%p", i, order[i]);
                        pressBtn(order[i]);
                        break;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    InterlockedExchange(&g.inAction, 0);
}

// --- install (called from featuremenu_install on Russobit, after installBattleDiscriminator) ---
extern "C" void timerhost_install(void)
{
    if (g.installed)
        return;
    g.installed = 1;

    // off[9] dialog/button-create capture: DetourAttach the resolved target (verified __stdcall 5-arg).
    g.g_orig_dlgCreate = reinterpret_cast<void*>(0x5C93D6);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&g.g_orig_dlgCreate, reinterpret_cast<void*>(hook_dlgCreate));
    if (DetourTransactionCommit() != NO_ERROR) {
        g.g_orig_dlgCreate = reinterpret_cast<void*>(0x5C93D6);
        tlog("[timer] keystone off[9] dlg-create detour FAILED");
    }

    // off[8] CButtonInterf::vftable[0] destructor -> null captured buttons on destroy.
    g.g_orig_btnDtor = patchVtableSlot(0x6E3294, 0, reinterpret_cast<void*>(hook_btnDestroy));
    // off[5] CMidClient::vftable[0] scenario-init -> drop all captures.
    g.g_orig_scenInit = patchVtableSlot(0x6CEB5C, 0, reinterpret_cast<void*>(hook_scenarioInit));

    tlog("[timer] keystone capture installed (off9 dlg-create 0x5C93D6, off8 btn-dtor 0x6E3294, "
         "off5 scen-init 0x6CEB5C)");
}
