/*
 * Game text localization bridge for Disciples II, modelled after the legacy C4dll-R wrapper.
 *
 * Disciple.ini [Wrapper] Locale is an LCID (1049 = Russian). The selected locale supplies the
 * OEM and ANSI code pages used by OemToCharA/CharToOemA and msvcrt's LC_CTYPE. Locale=0 disables
 * the conversion. Unlike the old binary, the detours stay installed and the code pages can be
 * switched live from the C4dll-R menu.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

#include <clocale>
#include <cstdio>
#include <cstring>

namespace {

BOOL(WINAPI* g_realOemToCharA)(LPCSTR, LPSTR) = OemToCharA;
BOOL(WINAPI* g_realCharToOemA)(LPCSTR, LPSTR) = CharToOemA;

volatile LONG g_locale = 0;
volatile LONG g_oemCodePage = 0;
volatile LONG g_ansiCodePage = 0;
char g_iniPath[MAX_PATH] = {};

void initIniPath()
{
    GetModuleFileNameA(nullptr, g_iniPath, sizeof(g_iniPath));
    char* slash = strrchr(g_iniPath, '\\');
    if (!slash)
        slash = strrchr(g_iniPath, '/');
    lstrcpynA(slash ? slash + 1 : g_iniPath, "Disciple.ini",
              slash ? static_cast<int>(sizeof(g_iniPath) - (slash + 1 - g_iniPath))
                    : static_cast<int>(sizeof(g_iniPath)));
}

UINT localeCodePage(LCID locale, LCTYPE type)
{
    char text[16] = {};
    if (!GetLocaleInfoA(locale, type, text, sizeof(text)))
        return 0;
    unsigned value = 0;
    return sscanf_s(text, "%u", &value) == 1 ? value : 0;
}

void setMsvcrtCtype(UINT ansiCodePage)
{
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (!crt)
        return;
    using SetLocaleFn = char*(__cdecl*)(int, const char*);
    auto setLocale = reinterpret_cast<SetLocaleFn>(GetProcAddress(crt, "setlocale"));
    if (!setLocale)
        return;

    if (!ansiCodePage) {
        setLocale(LC_CTYPE, "C");
        return;
    }
    char localeName[24];
    sprintf_s(localeName, ".%u", ansiCodePage);
    setLocale(LC_CTYPE, localeName);
}

bool applyLocale(LCID locale)
{
    UINT oem = 0;
    UINT ansi = 0;
    if (locale) {
        oem = localeCodePage(locale, LOCALE_IDEFAULTCODEPAGE);
        ansi = localeCodePage(locale, LOCALE_IDEFAULTANSICODEPAGE);
        if (!oem || !ansi)
            return false;
    }

    InterlockedExchange(&g_oemCodePage, static_cast<LONG>(oem));
    InterlockedExchange(&g_ansiCodePage, static_cast<LONG>(ansi));
    InterlockedExchange(&g_locale, static_cast<LONG>(locale));
    setMsvcrtCtype(ansi);

    char msg[160];
    sprintf_s(msg, "[localization] LCID=%lu OEM=%u ANSI=%u\n", static_cast<unsigned long>(locale),
              oem, ansi);
    OutputDebugStringA(msg);
    return true;
}

BOOL recodeString(LPCSTR src, LPSTR dst, UINT from, UINT to,
                  BOOL(WINAPI* fallback)(LPCSTR, LPSTR))
{
    if (!src || !dst || !from || !to || from == to)
        return fallback(src, dst);

    wchar_t wide[1024];
    const int n = MultiByteToWideChar(from, 0, src, -1, wide,
                                      static_cast<int>(sizeof(wide) / sizeof(wide[0])));
    if (n <= 0)
        return fallback(src, dst);
    char out[1024];
    const int m = WideCharToMultiByte(to, 0, wide, -1, out, static_cast<int>(sizeof(out)), nullptr,
                                      nullptr);
    if (m <= 0)
        return fallback(src, dst);
    memcpy(dst, out, static_cast<size_t>(m));
    return TRUE;
}

BOOL WINAPI hookedOemToCharA(LPCSTR src, LPSTR dst)
{
    const UINT from = static_cast<UINT>(InterlockedCompareExchange(&g_oemCodePage, 0, 0));
    const UINT to = static_cast<UINT>(InterlockedCompareExchange(&g_ansiCodePage, 0, 0));
    return recodeString(src, dst, from, to, g_realOemToCharA);
}

BOOL WINAPI hookedCharToOemA(LPCSTR src, LPSTR dst)
{
    const UINT from = static_cast<UINT>(InterlockedCompareExchange(&g_ansiCodePage, 0, 0));
    const UINT to = static_cast<UINT>(InterlockedCompareExchange(&g_oemCodePage, 0, 0));
    return recodeString(src, dst, from, to, g_realCharToOemA);
}

} // namespace

extern "C" unsigned localization_get_locale(void)
{
    return static_cast<unsigned>(InterlockedCompareExchange(&g_locale, 0, 0));
}

extern "C" unsigned localization_get_oem_code_page(void)
{
    return static_cast<unsigned>(InterlockedCompareExchange(&g_oemCodePage, 0, 0));
}

extern "C" unsigned localization_get_ansi_code_page(void)
{
    return static_cast<unsigned>(InterlockedCompareExchange(&g_ansiCodePage, 0, 0));
}

extern "C" int localization_set_locale(unsigned locale)
{
    if (!applyLocale(static_cast<LCID>(locale)))
        return 0;
    char text[16];
    sprintf_s(text, "%u", locale);
    return WritePrivateProfileStringA("Wrapper", "Locale", text, g_iniPath) ? 1 : 0;
}

extern "C" void localization_install(void)
{
    initIniPath();
    const LCID systemLocale = GetUserDefaultLCID();
    const LCID configured = static_cast<LCID>(
        GetPrivateProfileIntA("Wrapper", "Locale", static_cast<int>(systemLocale), g_iniPath));
    if (!applyLocale(configured))
        applyLocale(systemLocale);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(g_realOemToCharA), hookedOemToCharA);
    DetourAttach(&reinterpret_cast<PVOID&>(g_realCharToOemA), hookedCharToOemA);
    if (DetourTransactionCommit() == NO_ERROR)
        OutputDebugStringA("[localization] OemToCharA/CharToOemA locale bridge installed\n");
    else
        OutputDebugStringA("[localization] failed to install OemToCharA/CharToOemA bridge\n");
}
