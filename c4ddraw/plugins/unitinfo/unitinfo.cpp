/*
 * twitchstat.c4p - self-contained live battle-roster snapshot source for Twitch.
 *
 * The plugin owns no artwork and no unit database. LMB on a live Wide Battle portrait reads the
 * current battle roster from the game process and builds the exact UTF-8 transport document. The
 * preview window is an opt-in diagnostic only; normal operation retains the latest payload in memory
 * for the Twitch transport. No synthetic RMB is sent and the player's visible encyclopedia is not
 * modified.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../features/c4plugin.h"

extern "C" int battleunitinfo_get_json_at(int x, int y, int frameWidth, int frameHeight,
                                             char* utf8Json, uint32_t capacity,
                                             uint32_t* required);
extern "C" void battleunitinfo_reset_battle(void);
extern "C" const char* battleunitinfo_last_diagnostic(void);

namespace {

const C4P_Host* g_host = nullptr;
HMENU g_menu = nullptr;
int g_base = 0;
volatile LONG g_enabled = 1;
volatile LONG g_swallowRelease = 0;
volatile LONG g_extracting = 0;
volatile LONG g_battleActive = -1; // -1 = legacy host (fall back to is_in_battle)
volatile LONG g_frameWidth = 0;
volatile LONG g_frameHeight = 0;
volatile LONG g_debugLog = 0;
volatile LONG g_preview = 0;

HMODULE g_module = nullptr;
ATOM g_windowClass = 0;
HWND g_window = nullptr;
HWND g_edit = nullptr;
HWND g_copy = nullptr;
HWND g_close = nullptr;
HFONT g_font = nullptr;
// One allocation avoids constructing all hidden encyclopedia layouts twice merely to resize the
// output buffer. The transport payload is still bounded to one MiB below.
std::vector<char> g_json(1024 * 1024);
std::wstring g_clipboardText;
std::wstring g_displayText;

constexpr wchar_t kWindowClass[] = L"C4dllR_TwitchStatJsonWindow";
constexpr int kEditId = 1001;
constexpr int kCopyId = 1002;
constexpr int kCloseId = 1003;

enum CommandOffset
{
    kEnabled = 1
};

bool siblingPath(const char* leaf, char* output, size_t capacity)
{
    if (!leaf || !output || capacity == 0 ||
        !GetModuleFileNameA(nullptr, output, static_cast<DWORD>(capacity)))
        return false;
    char* slash = strrchr(output, '\\');
    if (!slash)
        return false;
    const size_t prefix = static_cast<size_t>(slash + 1 - output);
    const size_t leafLength = strlen(leaf);
    if (prefix + leafLength + 1 > capacity)
        return false;
    memcpy(output + prefix, leaf, leafLength + 1);
    return true;
}

bool diagnosticsEnabled()
{
    char environment[2] = {};
    if (GetEnvironmentVariableA("C4DLL_DEBUG", environment,
                                static_cast<DWORD>(sizeof(environment))) != 0)
        return true;
    char ini[MAX_PATH] = {};
    return siblingPath("C4menu.ini", ini, sizeof(ini)) &&
           GetPrivateProfileIntA("menu", "debugLog", 0, ini) != 0;
}

void unitLog(const char* format, ...)
{
    if (InterlockedCompareExchange(&g_debugLog, 0, 0) == 0)
        return;
    char message[700] = {};
    strcpy_s(message, "[twitchstat] ");
    const size_t prefix = strlen(message);
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(message + prefix, sizeof(message) - prefix, _TRUNCATE, format, arguments);
    va_end(arguments);
    message[sizeof(message) - 2] = 0;
    size_t length = strlen(message);
    message[length++] = '\n';
    message[length] = 0;
    OutputDebugStringA(message);

    char path[MAX_PATH] = {};
    if (!siblingPath("C4plugins.log", path, sizeof(path)))
        return;
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

bool hostHasMinimumApi(const C4P_Host* host)
{
    const size_t required = offsetof(C4P_Host, set_config_int) +
                            sizeof(host->set_config_int);
    return host && host->struct_size >= required && host->get_hwnd &&
           host->get_config_int && host->set_config_int;
}

bool hostReportsBattleActive()
{
    const size_t required = offsetof(C4P_Host, is_in_battle) +
                            sizeof(g_host->is_in_battle);
    // Very old ABI-compatible hosts without this optional field still get the extractor's own
    // strict DLG_BATTLE_B check. Current and v1.8 hosts take this cheap fast path outside battles.
    return !g_host || g_host->struct_size < required || !g_host->is_in_battle ||
           g_host->is_in_battle() != 0;
}

struct ExtractionLatch
{
    bool acquired;

    ExtractionLatch()
        : acquired(InterlockedCompareExchange(&g_extracting, 1, 0) == 0)
    {
    }

    ~ExtractionLatch()
    {
        if (acquired)
            InterlockedExchange(&g_extracting, 0);
    }

    ExtractionLatch(const ExtractionLatch&) = delete;
    ExtractionLatch& operator=(const ExtractionLatch&) = delete;
};

void getLogicalGameSize(int* width, int* height)
{
    *width = 0;
    *height = 0;
    const size_t required = offsetof(C4P_Host, get_game_size) +
                            sizeof(g_host->get_game_size);
    if (g_host && g_host->struct_size >= required && g_host->get_game_size) {
        int32_t w = 0;
        int32_t h = 0;
        if (g_host->get_game_size(&w, &h) && w > 0 && h > 0) {
            *width = w;
            *height = h;
            return;
        }
    }
    *width = static_cast<int>(InterlockedCompareExchange(&g_frameWidth, 0, 0));
    *height = static_cast<int>(InterlockedCompareExchange(&g_frameHeight, 0, 0));
}

void refreshMenu()
{
    if (!g_menu)
        return;
    const bool enabled = InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
    CheckMenuItem(g_menu, g_base + kEnabled,
                  MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
}

std::wstring utf8ToWide(const char* utf8)
{
    if (!utf8 || !*utf8)
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            utf8, -1, nullptr, 0);
    if (length <= 0)
        return L"Invalid UTF-8 payload";
    std::wstring value(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                             &value[0], length))
        return L"Invalid UTF-8 payload";
    value.resize(static_cast<size_t>(length - 1));
    return value;
}

std::wstring toEditNewlines(const std::wstring& source)
{
    std::wstring result;
    result.reserve(source.size() + source.size() / 20);
    wchar_t previous = 0;
    for (wchar_t ch : source) {
        if (ch == L'\n' && previous != L'\r')
            result.push_back(L'\r');
        result.push_back(ch);
        previous = ch;
    }
    return result;
}

void copyJsonToClipboard(HWND owner)
{
    if (g_clipboardText.empty() || !OpenClipboard(owner))
        return;
    EmptyClipboard();
    const SIZE_T bytes = (g_clipboardText.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* destination = GlobalLock(memory);
        if (destination) {
            memcpy(destination, g_clipboardText.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
            else
                memory = nullptr; // clipboard owns it
        }
        if (memory)
            GlobalFree(memory);
    }
    CloseClipboard();
}

void layoutControls(HWND window)
{
    RECT client = {};
    GetClientRect(window, &client);
    const int width = std::max(0L, client.right - client.left);
    const int height = std::max(0L, client.bottom - client.top);
    const int margin = 12;
    const int buttonHeight = 30;
    const int buttonWidth = 126;
    const int bottom = height - margin - buttonHeight;
    if (g_edit)
        MoveWindow(g_edit, margin, margin, std::max(0, width - margin * 2),
                   std::max(0, bottom - margin * 2), TRUE);
    if (g_close)
        MoveWindow(g_close, width - margin - buttonWidth, bottom,
                   buttonWidth, buttonHeight, TRUE);
    if (g_copy)
        MoveWindow(g_copy, width - margin * 2 - buttonWidth * 2, bottom,
                   buttonWidth, buttonHeight, TRUE);
}

LRESULT CALLBACK twitchStatWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY |
            WS_VSCROLL | WS_HSCROLL, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), g_module, nullptr);
        g_copy = CreateWindowExW(0, L"BUTTON", L"Copy JSON",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, 0, 0, window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyId)),
                                 g_module, nullptr);
        g_close = CreateWindowExW(0, L"BUTTON", L"Close",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, 0, 0, window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)),
                                  g_module, nullptr);
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (!g_font)
            g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND control : {g_edit, g_copy, g_close})
            if (control)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        layoutControls(window);
        return 0;

    case WM_SIZE:
        layoutControls(window);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 440;
        info->ptMinTrackSize.y = 320;
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == kCopyId) {
            copyJsonToClipboard(window);
            return 0;
        }
        if (LOWORD(wParam) == kCloseId) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (g_font && g_font != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(g_font);
        g_font = nullptr;
        g_edit = nullptr;
        g_copy = nullptr;
        g_close = nullptr;
        g_window = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool registerWindowClass()
{
    if (g_windowClass)
        return true;
    if (!g_module) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&twitchStatWndProc), &g_module);
    }
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = twitchStatWndProc;
    cls.hInstance = g_module;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    cls.hIconSm = cls.hIcon;
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.lpszClassName = kWindowClass;
    g_windowClass = RegisterClassExW(&cls);
    return g_windowClass != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ensureWindow()
{
    if (g_window && IsWindow(g_window))
        return true;
    if (!registerWindowClass() || !g_host)
        return false;

    HWND game = g_host->get_hwnd();
    if (!game || !IsWindow(game))
        return false;
    RECT gameRect = {};
    GetWindowRect(game, &gameRect);
    RECT work = {};
    const HMONITOR monitor = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {sizeof(monitorInfo)};
    if (GetMonitorInfoW(monitor, &monitorInfo))
        work = monitorInfo.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    const int width = std::min(680L, std::max(440L, work.right - work.left - 40));
    const int height = std::min(760L, std::max(320L, work.bottom - work.top - 40));
    int x = gameRect.right + 12;
    int y = std::max(work.top + 20, gameRect.top);
    if (x + width > work.right)
        x = std::max(work.left + 20, gameRect.left + 40);
    if (y + height > work.bottom)
        y = std::max(work.top + 20, work.bottom - height - 20);

    g_window = CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowClass, L"Twitch Stat — JSON preview",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, game,
        nullptr, g_module, nullptr);
    return g_window != nullptr;
}

bool showPayload(const char* json)
{
    if (!ensureWindow())
        return false;
    g_clipboardText = utf8ToWide(json);
    g_displayText = toEditNewlines(g_clipboardText);
    SetWindowTextW(g_edit, g_displayText.c_str());
    SendMessageW(g_edit, EM_SETSEL, 0, 0);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetWindowPos(g_window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_window);
    SetFocus(g_edit);
    return true;
}

} // namespace

extern "C" int __cdecl c4p_query(C4P_Info* out)
{
    if (!out || out->struct_size < sizeof(C4P_Info))
        return 0;
    out->abi_version = C4P_ABI_VERSION;
    // Keep the historical ID as the stable de-duplication identity. The host treats IDs as API,
    // while the visible product name and file are now Twitch Stat / twitchstat.c4p.
    out->id = "c4dll.unitinfo";
    out->name = "Twitch Stat";
    out->reserved_v2 = nullptr;
    return 1;
}

extern "C" int __cdecl c4p_init(const C4P_Host* host)
{
    if (!hostHasMinimumApi(host))
        return 0;
    g_host = host;
    InterlockedExchange(&g_debugLog, diagnosticsEnabled() ? 1 : 0);
    int enabled = host->get_config_int("TwitchStat", "Enabled", -1);
    if (enabled < 0)
        enabled = host->get_config_int("UnitInfo", "Enabled", 1); // one-way legacy fallback
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    InterlockedExchange(&g_preview,
                        host->get_config_int("TwitchStat", "Preview", 0) ? 1 : 0);
    InterlockedExchange(&g_swallowRelease, 0);
    InterlockedExchange(&g_extracting, 0);
    InterlockedExchange(&g_battleActive, -1);
    InterlockedExchange(&g_frameWidth, 0);
    InterlockedExchange(&g_frameHeight, 0);
    unitLog("init enabled=%ld preview=%ld host_size=%lu",
            InterlockedCompareExchange(&g_enabled, 0, 0),
            InterlockedCompareExchange(&g_preview, 0, 0),
            static_cast<unsigned long>(host->struct_size));
    // Window registration is intentionally lazy. A UI setup problem must never make the plugin
    // disappear from the host menu, and first-click creation happens on the game's UI thread.
    return 1;
}

extern "C" int __cdecl c4p_draw(C4P_Canvas* canvas)
{
    if (canvas && canvas->struct_size >= sizeof(C4P_Canvas) &&
        canvas->width > 0 && canvas->height > 0) {
        InterlockedExchange(&g_frameWidth, canvas->width);
        InterlockedExchange(&g_frameHeight, canvas->height);
    }
    return 0;
}

extern "C" uint32_t __cdecl c4p_scope(void)
{
    return C4P_SCOPE_BATTLE;
}

extern "C" void __cdecl c4p_battle_state(int active)
{
    InterlockedExchange(&g_battleActive, active ? 1 : 0);
    unitLog("battle state -> %d", active ? 1 : 0);
    if (active) {
        battleunitinfo_reset_battle();
    } else {
        InterlockedExchange(&g_swallowRelease, 0);
        if (g_window)
            PostMessageW(g_window, WM_CLOSE, 0, 0);
    }
}

extern "C" HMENU __cdecl c4p_menu(int base_cmd_id)
{
    g_base = base_cmd_id;
    g_menu = CreatePopupMenu();
    if (!g_menu)
        return nullptr;
    AppendMenuA(g_menu, MF_STRING, g_base + kEnabled, "&Enabled");
    refreshMenu();
    return g_menu;
}

extern "C" void __cdecl c4p_command(int cmd)
{
    if (!g_host || cmd != g_base + kEnabled)
        return;
    const LONG enabled = InterlockedCompareExchange(&g_enabled, 0, 0) ? 0 : 1;
    InterlockedExchange(&g_enabled, enabled);
    if (!enabled) {
        InterlockedExchange(&g_swallowRelease, 0);
        if (g_window)
            ShowWindow(g_window, SW_HIDE);
    }
    g_host->set_config_int("TwitchStat", "Enabled", enabled ? 1 : 0);
    refreshMenu();
}

extern "C" int __cdecl c4p_mouse(UINT msg, WPARAM, int x, int y)
{
    if (!g_host)
        return 0;

    const LONG battleActive = InterlockedCompareExchange(&g_battleActive, 0, 0);
    if (battleActive == 0 || (battleActive < 0 && !hostReportsBattleActive()))
        return 0;

    // A successful Twitch Stat trigger consumes both halves of the LMB gesture so the same click
    // cannot also activate a battle command. Reset the old latch on every new down so a missing
    // WM_LBUTTONUP never consumes an unrelated later click (Preview=1 can also redirect focus).
    if (msg == WM_LBUTTONDOWN) {
        InterlockedExchange(&g_swallowRelease, 0);
        const LONG enabled = InterlockedCompareExchange(&g_enabled, 0, 0);
        unitLog("LMB x=%d y=%d battle=%ld enabled=%ld", x, y, battleActive, enabled);
        if (enabled == 0)
            return 0;

        // Game input, battle updates and the native encyclopedia constructor all run on this UI
        // thread, so animation phase is not a safety boundary. Prevent only a nested callback from
        // starting a second detached encyclopedia batch before the first one has been destroyed.
        ExtractionLatch extraction;
        if (!extraction.acquired) {
            unitLog("LMB ignored: extraction already in progress");
            return 0;
        }
        int frameWidth = 0;
        int frameHeight = 0;
        getLogicalGameSize(&frameWidth, &frameHeight);
        unitLog("extract begin frame=%dx%d", frameWidth, frameHeight);
        uint32_t required = 0;
        int result = battleunitinfo_get_json_at(
            x, y, frameWidth, frameHeight, g_json.data(),
            static_cast<uint32_t>(g_json.size()), &required);
        if (result < 0 && required > g_json.size() && required <= 1024 * 1024) {
            g_json.resize(required);
            result = battleunitinfo_get_json_at(
                x, y, frameWidth, frameHeight, g_json.data(),
                static_cast<uint32_t>(g_json.size()), &required);
        }
        if (result != 1) {
            unitLog("extract failed result=%d required=%lu reason=%s", result,
                    static_cast<unsigned long>(required),
                    battleunitinfo_last_diagnostic());
            return 0;
        }

        const LONG preview = InterlockedCompareExchange(&g_preview, 0, 0);
        if (preview != 0 && !showPayload(g_json.data())) {
            unitLog("snapshot ready (%lu bytes), but preview failed winerr=%lu reason=%s",
                    static_cast<unsigned long>(required), static_cast<unsigned long>(GetLastError()),
                    battleunitinfo_last_diagnostic());
        } else {
            unitLog("snapshot ready payload=%lu bytes preview=%ld reason=%s",
                    static_cast<unsigned long>(required), preview,
                    battleunitinfo_last_diagnostic());
        }
        // Extraction is the LMB action even when Preview=0; do not forward that same gesture to the
        // game. Failed/out-of-scope extraction returned above and remains transparent.
        InterlockedExchange(&g_swallowRelease, 1);
        return 1;
    }

    // The original left-button down never reached the game, so its matching up must not either.
    if (msg == WM_LBUTTONUP)
        return InterlockedExchange(&g_swallowRelease, 0) != 0 ? 1 : 0;

    if (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED)
        InterlockedExchange(&g_swallowRelease, 0);
    return 0;
}

extern "C" void __cdecl c4p_refresh_menu(void)
{
    refreshMenu();
}
