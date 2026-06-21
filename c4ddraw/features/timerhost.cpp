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

} // namespace

// --- exported read accessors (lock-free single-word volatile reads; called by pluginhost thunks) ---
extern "C" int timerhost_is_animating(void) { return g.animFlag ? 1 : 0; }
extern "C" int timerhost_battle_kind(void) { return g.pvpFlag ? 1 : (g.anyCombat ? 2 : 0); }
extern "C" int timerhost_turn_active(void) { return 0; }    // set by off[6] (Phase 1b)
extern "C" int timerhost_turn_player_id(void) { return -1; } // set by off[6] (Phase 1b)
extern "C" int timerhost_retreat(void) { return 0; }         // gated action (Phase 2)
extern "C" int timerhost_end_day(void) { return 0; }         // gated action (Phase 2)

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
