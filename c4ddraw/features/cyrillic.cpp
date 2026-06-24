/*
 * Russian (cp-1251) text fix for Disciples II, ported into C4dll-R from the mss32 mod.
 * Restores cp866<->cp1251 decoding that the original renderer masked, with NO system-locale change.
 * NOTE: do not also run this in mss32 (double-convert); pair C4dll-R with the clean/minimal mss32.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>
#include <clocale>
#include <cstring>

namespace {

BOOL(WINAPI* realOemToCharA)(LPCSTR, LPSTR) = OemToCharA;
BOOL(WINAPI* realCharToOemA)(LPCSTR, LPSTR) = CharToOemA;

void dbg(const char* s)
{
    OutputDebugStringA(s);
}

// Recode a NUL-terminated string `from`->`to`. Single-byte both ways, so in-place (src == dst,
// as the game does) is safe: src is fully read into wide before dst is written.
BOOL recodeString(LPCSTR src, LPSTR dst, UINT from, UINT to, BOOL(WINAPI* fallback)(LPCSTR, LPSTR))
{
    if (!src || !dst)
        return fallback(src, dst);
    wchar_t wide[1024];
    const int n = MultiByteToWideChar(from, 0, src, -1, wide, 1024);
    if (n <= 0)
        return fallback(src, dst);
    char out[1024];
    const int m = WideCharToMultiByte(to, 0, wide, -1, out, sizeof(out), nullptr, nullptr);
    if (m <= 0)
        return fallback(src, dst);
    memcpy(dst, out, m); // m counts the terminator; output length <= input length (1:1)
    return TRUE;
}

BOOL WINAPI hookedOemToCharA(LPCSTR src, LPSTR dst) // cp866 -> cp1251 (display)
{
    return recodeString(src, dst, 866, 1251, realOemToCharA);
}

BOOL WINAPI hookedCharToOemA(LPCSTR src, LPSTR dst) // cp1251 -> cp866 (save)
{
    return recodeString(src, dst, 1251, 866, realCharToOemA);
}

} // namespace

// Called from cnc-ddraw's DllMain (after the embed).
extern "C" void cyrillic_install(void)
{
    // (1) Force msvcrt's OWN LC_CTYPE = cp-1251 (fixes _strupr/toupper); the game's ctype resolves
    // to msvcrt.dll, not this module's static CRT.
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (crt) {
        using SetLocaleFn = char*(__cdecl*)(int, const char*);
        if (auto setLocale = reinterpret_cast<SetLocaleFn>(GetProcAddress(crt, "setlocale"))) {
            setLocale(LC_CTYPE, ".1251");
            dbg("[cyrillic] msvcrt LC_CTYPE := cp-1251\n");
        }
    } else {
        dbg("[cyrillic] msvcrt.dll not present; cannot set cp-1251 ctype\n");
    }

    // (2) Detour OemToCharA/CharToOemA to explicit cp866<->cp1251 (the display fix).
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(realOemToCharA), hookedOemToCharA);
    DetourAttach(&reinterpret_cast<PVOID&>(realCharToOemA), hookedCharToOemA);
    if (DetourTransactionCommit() == NO_ERROR)
        dbg("[cyrillic] OemToCharA/CharToOemA forced to cp866<->cp1251\n");
    else
        dbg("[cyrillic] failed to detour OemToCharA/CharToOemA\n");
}
