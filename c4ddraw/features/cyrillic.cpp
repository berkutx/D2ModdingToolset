/*
 * Russian (cp-1251) text fix for Disciples II, ported into C4dll-R from the mss32 mod
 * (D2ModdingToolset main.cpp setupCyrillicCodePage). It belongs here now that the renderer +
 * features live in C4dll-R: the monolith provides it, so it works with ANY (clean) mss32 instead
 * of being tied to a specific mss32 build.
 *
 * Disciples II stores its UI strings in OEM cp866 and decodes them for display with OemToCharA
 * (re-encodes on save with CharToOemA). Those USER32 calls use the SYSTEM ANSI codepage (CP_ACP);
 * on a non-Russian Windows (ACP=1252) they corrupt every string before it reaches the cp-1251
 * .mft bitmap fonts -> mojibake. The game also upper-cases via the locale-sensitive CRT
 * _strupr/toupper (from msvcrt.dll), which needs LC_CTYPE = cp-1251. DisciplesGL masked both; with
 * it replaced by the embedded cnc-ddraw we restore them here, with NO system-locale change:
 *   (1) force msvcrt's OWN LC_CTYPE = cp-1251 (the game's ctype resolves to msvcrt, not this DLL's CRT);
 *   (2) detour OemToCharA/CharToOemA to convert with EXPLICIT cp866<->cp1251 instead of CP_ACP.
 * cp-1251 is identical to ASCII for English text, so this is safe for non-RU builds.
 *
 * NOTE: do not also run this fix in mss32 (it would double-convert). The clean / "minimal" mss32
 * has no such fix - pair C4dll-R with that.
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

// Recode a NUL-terminated string from codepage `from` to `to`. Single-byte both ways, so length is
// preserved and an in-place call (src == dst, as the game makes) is safe: src is fully read into a
// wide buffer before dst is written.
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

BOOL WINAPI hookedOemToCharA(LPCSTR src, LPSTR dst) // OEM cp866 -> ANSI cp1251 (display)
{
    return recodeString(src, dst, 866, 1251, realOemToCharA);
}

BOOL WINAPI hookedCharToOemA(LPCSTR src, LPSTR dst) // ANSI cp1251 -> OEM cp866 (save)
{
    return recodeString(src, dst, 1251, 866, realCharToOemA);
}

} // namespace

// Called from cnc-ddraw's DllMain (after the embed). Installs the cp-1251 text fix.
extern "C" void cyrillic_install(void)
{
    // (1) msvcrt LC_CTYPE -> cp-1251 (fixes _strupr/toupper on the game's cp-1251 text). We resolve
    // setlocale from msvcrt.dll specifically, because the game's ctype uses msvcrt's CRT, not the
    // (different, static) CRT this module links.
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

    // (2) OemToCharA/CharToOemA -> explicit cp866<->cp1251 (the actual display fix).
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(realOemToCharA), hookedOemToCharA);
    DetourAttach(&reinterpret_cast<PVOID&>(realCharToOemA), hookedCharToOemA);
    if (DetourTransactionCommit() == NO_ERROR)
        dbg("[cyrillic] OemToCharA/CharToOemA forced to cp866<->cp1251\n");
    else
        dbg("[cyrillic] failed to detour OemToCharA/CharToOemA\n");
}
