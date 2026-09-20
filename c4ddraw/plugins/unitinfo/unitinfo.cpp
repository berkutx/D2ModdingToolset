/*
 * twitchstat.c4p - self-contained live battle-roster snapshot source for Twitch.
 *
 * The plugin owns no artwork and no unit database. A paced UI-thread timer captures at most one
 * Wide Battle card per step and publishes only complete validated rosters for the Twitch bridge. The
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
#include "snapshotprofile.h"
#include "slicedcapture.h"
#include "capturepace.h"
#include "localbridge.h"

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
volatile LONG g_profileBatchesRemaining = 0;
volatile LONG g_profileStopAfterSamples = 0;
volatile LONG g_profileAutoStopped = 0; // process latch; battle/menu transitions never reset it
twitchstat::CapturePace g_capturePace; // UI thread only; also rejects queued duplicate timer messages
bool g_framePublished = false;
DWORD g_publishedFirstTick = 0;
char g_lastSliceFailure[320] = {};
constexpr UINT kCapturePauseMs = 50; // pause begins after native work returns, not before it

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
HWND g_pollWindow = nullptr;
HANDLE g_fileEvent = nullptr;
HANDLE g_fileWorker = nullptr;
SRWLOCK g_fileLock = SRWLOCK_INIT;
std::string g_pendingFrame;
std::wstring g_livePath;
volatile LONG g_stopFileWorker = 0;
unsigned long g_battleSerial = 0;
unsigned long long g_sessionTime = 0;
constexpr wchar_t kPollWindowClass[] = L"C4dllR_TwitchStatPoll";
constexpr UINT kPollMessage = WM_APP + 37;

constexpr wchar_t kWindowClass[] = L"C4dllR_TwitchStatJsonWindow";
constexpr int kEditId = 1001;
constexpr int kCopyId = 1002;
constexpr int kCloseId = 1003;

enum CommandOffset
{
    kEnabled = 1,
    kBridgeStatus = 2
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
    if (InterlockedCompareExchange(&g_debugLog, 0, 0) == 0 &&
        InterlockedCompareExchange(&g_profileBatchesRemaining, 0, 0) == 0)
        return;
    char message[2304] = {};
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
    const char* status = "Local connection: off";
    char details[160] = {};
    if (enabled) {
        switch (twitchstat::localBridgeState()) {
        case twitchstat::LocalBridgeState::Starting:
            status = "Local connection: starting...";
            break;
        case twitchstat::LocalBridgeState::Listening:
            sprintf_s(details, "Local connection: ready (game PID %lu)", GetCurrentProcessId());
            status = details;
            break;
        case twitchstat::LocalBridgeState::PortBusy:
            status = "Port 8765 busy: disable Twitch Stat in the other game, then re-enable here";
            break;
        case twitchstat::LocalBridgeState::Failed:
            sprintf_s(details, "Local connection failed (%d): disable and re-enable to retry",
                      twitchstat::localBridgeError());
            status = details;
            break;
        default:
            break;
        }
    }
    ModifyMenuA(g_menu, g_base + kBridgeStatus, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                g_base + kBridgeStatus, status);
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

unsigned long long unixMilliseconds()
{
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER ticks;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return (ticks.QuadPart - 116444736000000000ULL) / 10000ULL;
}

// Disk I/O is kept off the game thread. Coalescing bounds the queue to the newest full snapshot.
DWORD WINAPI writeFrames(LPVOID)
{
    const std::wstring temporary = g_livePath + L".tmp";
    for (;;) {
        WaitForSingleObject(g_fileEvent, INFINITE);
        std::string frame;
        AcquireSRWLockExclusive(&g_fileLock);
        frame.swap(g_pendingFrame);
        ReleaseSRWLockExclusive(&g_fileLock);
        if (!frame.empty()) {
            HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const bool ok = WriteFile(file, frame.data(), static_cast<DWORD>(frame.size()),
                                          &written, nullptr) && written == frame.size();
                CloseHandle(file);
                if (ok)
                    MoveFileExW(temporary.c_str(), g_livePath.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        if (InterlockedCompareExchange(&g_stopFileWorker, 0, 0))
            return 0;
    }
}

void publishFrame(const char* snapshot, int width = 0, int height = 0, uint32_t oldestAgeMs = 0)
{
    int32_t left = 0, top = 0, visibleWidth = width, visibleHeight = height, zoom = 1000;
    const size_t required = offsetof(C4P_Host, get_visible_game_rect) +
                            sizeof(g_host->get_visible_game_rect);
    if (snapshot && g_host->struct_size >= required && g_host->get_visible_game_rect)
        g_host->get_visible_game_rect(&left, &top, &visibleWidth, &visibleHeight, &zoom);
    const unsigned long long now = unixMilliseconds();
    // Delivery order and capture freshness are different clocks: a sliced job may start
    // before an intervening cache-invalidation frame, but finish after it. Keep ts at
    // publication time so the viewer accepts it; the bridge checks captured_at for TTL.
    const unsigned long long capturedAt = snapshot && now >= oldestAgeMs ? now - oldestAgeMs : now;
    char metadata[640];
    sprintf_s(metadata,
        "{\"schema\":\"c4dll.twitch-frame\",\"version\":1,\"pid\":%lu,"
        "\"battle_id\":\"%lu-%llu-%lu\",\"ts\":%llu,\"captured_at\":%llu,\"active\":%s,"
        "\"viewport\":{\"left\":%d,\"top\":%d,\"width\":%d,\"height\":%d},"
        "\"snapshot\":",
        GetCurrentProcessId(), GetCurrentProcessId(), g_sessionTime, g_battleSerial,
        now, capturedAt, snapshot ? "true" : "false", left, top, visibleWidth, visibleHeight);
    std::string frame(metadata);
    frame += snapshot ? snapshot : "null";
    frame += "}";
    // Networking consumes an immutable complete frame on its own worker. It never reads game
    // objects or waits for the browser on this UI thread. Disk output is diagnostic/legacy only.
    twitchstat::localBridgePublish(frame, capturedAt, snapshot != nullptr);
    if (!g_fileEvent)
        return;
    frame += "\n";
    AcquireSRWLockExclusive(&g_fileLock);
    g_pendingFrame.swap(frame);
    ReleaseSRWLockExclusive(&g_fileLock);
    SetEvent(g_fileEvent);
}

void invalidatePublishedFrame()
{
    if (g_framePublished) {
        g_framePublished = false;
        publishFrame(nullptr);
    }
}

bool readSlicedBattleState(SlicedBattleState* out)
{
    const size_t required = offsetof(C4P_Host, get_battle_timer_state) +
                            sizeof(g_host->get_battle_timer_state);
    if (!out || !g_host || g_host->struct_size < required || !g_host->get_battle_timer_state ||
        !InterlockedCompareExchange(&g_enabled, 0, 0) ||
        InterlockedCompareExchange(&g_battleActive, 0, 0) != 1)
        return false;
    C4P_BattleTimerState state = {};
    state.struct_size = sizeof(state);
    if (!g_host->get_battle_timer_state(&state))
        return false;
    *out = {state.battle_instance, state.generation, state.battle_kind, state.local_active,
            state.selection_open, state.continuation, state.animation_active, state.playback_local};
    return true;
}

bool startFileWriter()
{
    wchar_t executable[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH))
        return false;
    wchar_t* slash = wcsrchr(executable, L'\\');
    if (!slash)
        return false;
    slash[1] = 0;
    wchar_t leaf[64];
    swprintf_s(leaf, L"TwitchStat-live-%lu.json", GetCurrentProcessId());
    g_livePath = executable;
    g_livePath += leaf;
    g_fileEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fileEvent)
        return false;
    g_fileWorker = CreateThread(nullptr, 0, writeFrames, nullptr, 0, nullptr);
    if (!g_fileWorker) {
        CloseHandle(g_fileEvent);
        g_fileEvent = nullptr;
        return false;
    }
    publishFrame(nullptr);
    return true;
}

bool currentThreadCpuTicks(unsigned long long* ticks)
{
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return false;
    ULARGE_INTEGER kernelTicks, userTicks;
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    *ticks = kernelTicks.QuadPart + userTicks.QuadPart;
    return true;
}

void logSnapshotProfile(const NativeSnapshotProfile& profile, int result, LONGLONG wall,
                        LONGLONG publish, bool cpuAvailable, unsigned long long cpuTicks)
{
    LARGE_INTEGER frequency = {};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return;
    const double msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    UnitSnapshotTiming total = {};
    char units[1400] = {};
    size_t used = 0;
    for (int i = 0; i < profile.units; ++i) {
        const auto& unit = profile.unit[i];
        total.prep += unit.prep;
        total.constructor += unit.constructor;
        total.controls += unit.controls;
        total.destructor += unit.destructor;
        total.text += unit.text;
        const int count = _snprintf_s(units + used, sizeof(units) - used, _TRUNCATE,
            "%s%d:%.3f/%.3f/%.3f/%.3f/%.3f", i ? ";" : "", i,
            unit.prep * msPerTick, unit.constructor * msPerTick,
            unit.controls * msPerTick, unit.destructor * msPerTick, unit.text * msPerTick);
        if (count < 0)
            break;
        used += static_cast<size_t>(count);
    }
    // One aggregated write after all native objects have been destroyed. Unit tuple order:
    // parameter preparation / wrapper constructor / controls+effects / destructor / UTF-8 text.
    unitLog("profile result=%d units=%d wall_ms=%.3f cpu_ms=%.3f preflight_ms=%.3f "
            "prep_ms=%.3f ctor_ms=%.3f controls_ms=%.3f dtor_ms=%.3f text_ms=%.3f "
            "json_ms=%.3f publish_ms=%.3f unit_ms(prep/ctor/controls/dtor/text)=[%s]", result,
            profile.units, wall * msPerTick,
            cpuAvailable ? static_cast<double>(cpuTicks) / 10000.0 : -1.0,
            profile.preflight * msPerTick, total.prep * msPerTick,
            total.constructor * msPerTick, total.controls * msPerTick,
            total.destructor * msPerTick, total.text * msPerTick,
            profile.json * msPerTick, publish * msPerTick, units);
}

void pollBattle()
{
    if (InterlockedCompareExchange(&g_profileAutoStopped, 0, 0))
        return;
    if (!InterlockedCompareExchange(&g_enabled, 0, 0) ||
        InterlockedCompareExchange(&g_battleActive, 0, 0) != 1 || !hostReportsBattleActive()) {
        battleunitinfo_cancel_sliced();
        invalidatePublishedFrame();
        return;
    }
    ExtractionLatch extraction;
    if (!extraction.acquired)
        return;
    const bool profiling = InterlockedCompareExchange(&g_profileBatchesRemaining, 0, 0) > 0;
    SlicedCaptureInfo info = {};
    info.capturedUnit = -1;
    unsigned long long cpuStarted = 0, cpuFinished = 0;
    const bool cpuStartValid = profiling && currentThreadCpuTicks(&cpuStarted);
    const LONGLONG profileStarted = profiling ? snapshotProfileCounter() : 0;
    int width, height;
    getLogicalGameSize(&width, &height);
    uint32_t required = 0;
    const DWORD started = GetTickCount();
    const int result = battleunitinfo_step_json(width, height, g_json.data(),
        static_cast<uint32_t>(g_json.size()), &required, &readSlicedBattleState, &info);
    if (result == SliceComplete) {
        g_lastSliceFailure[0] = 0;
        g_framePublished = true;
        g_publishedFirstTick = GetTickCount() - info.oldestAgeMs;
        publishFrame(g_json.data(), width, height, info.oldestAgeMs);
    } else if (result == SliceUnavailable) {
        invalidatePublishedFrame();
        const char* reason = battleunitinfo_last_diagnostic();
        if (strcmp(g_lastSliceFailure, reason) != 0) {
            unitLog("slice unavailable reason=%s", reason);
            strncpy_s(g_lastSliceFailure, reason, _TRUNCATE);
        }
    }
    if (g_framePublished && GetTickCount() - g_publishedFirstTick >= 2500)
        invalidatePublishedFrame();
    // Pending/Cached never re-stamp old text. The bridge retains its original six-second TTL.
    const LONGLONG profileFinished = profiling ? snapshotProfileCounter() : 0;
    const bool cpuAvailable = cpuStartValid && currentThreadCpuTicks(&cpuFinished) &&
                              cpuFinished >= cpuStarted;
    if (info.capturedUnit >= 0) {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        const double wallMs = profiling && frequency.QuadPart > 0
            ? (profileFinished - profileStarted) * 1000.0 / frequency.QuadPart
            : static_cast<double>(GetTickCount() - started);
        unitLog("slice unit=%d/%d result=%d wall_ms=%.3f cpu_ms=%.3f age_ms=%lu reason=%s",
                info.capturedUnit + 1, info.totalUnits, result, wallMs,
                cpuAvailable ? (cpuFinished - cpuStarted) / 10000.0 : -1.0,
                static_cast<unsigned long>(info.oldestAgeMs), battleunitinfo_last_diagnostic());
    }
    if (result == SliceComplete) {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        LONGLONG activeTicks = info.profile.preflight + info.profile.json;
        for (int i = 0; i < info.profile.units; ++i) {
            const auto& timing = info.profile.unit[i];
            activeTicks += timing.prep + timing.constructor + timing.controls + timing.destructor + timing.text;
        }
        unitLog("sliced snapshot complete units=%d bytes=%lu age_ms=%lu active_ms=%.3f",
                info.totalUnits, static_cast<unsigned long>(required),
                static_cast<unsigned long>(info.oldestAgeMs), frequency.QuadPart > 0
                    ? activeTicks * 1000.0 / frequency.QuadPart : -1.0);
    }
    if (profiling && result == SliceComplete) {
        const bool stopAfterSamples =
            InterlockedCompareExchange(&g_profileStopAfterSamples, 0, 0) != 0;
        // Both modes count completed rosters, never partial slices or cancelled work.
        {
            if (stopAfterSamples &&
                InterlockedCompareExchange(&g_profileBatchesRemaining, 0, 0) == 1) {
                InterlockedExchange(&g_profileAutoStopped, 1);
                if (g_pollWindow)
                    KillTimer(g_pollWindow, 1);
                battleunitinfo_cancel_sliced();
                invalidatePublishedFrame();
                // Log before consuming the final budget entry so Profile alone enables this line.
                unitLog("profile=2 completed: 20 successful snapshots; automatic polling stopped until restart");
            }
            InterlockedDecrement(&g_profileBatchesRemaining);
        }
    }
}

LRESULT CALLBACK pollWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if ((message == WM_TIMER && wParam == 1) || message == kPollMessage) {
        if (!g_capturePace.ready(GetTickCount()))
            return 0;
        KillTimer(window, 1);
        // Nested message dispatch during native construction must not start another slice.
        g_capturePace.defer(GetTickCount(), kCapturePauseMs);
        pollBattle();
        g_capturePace.defer(GetTickCount(), kCapturePauseMs);
        if (!InterlockedCompareExchange(&g_profileAutoStopped, 0, 0) &&
            InterlockedCompareExchange(&g_enabled, 0, 0) &&
            InterlockedCompareExchange(&g_battleActive, 0, 0) == 1)
            SetTimer(window, 1, kCapturePauseMs, nullptr);
        return 0;
    }
    if (message == WM_CLOSE) {
        KillTimer(window, 1);
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_NCDESTROY)
        g_pollWindow = nullptr;
    return DefWindowProcW(window, message, wParam, lParam);
}

void updatePolling()
{
    // The final diagnostic sample already killed the timer and published inactive exactly once.
    // Do not restart on a later battle edge or Enabled menu toggle in this process.
    if (InterlockedCompareExchange(&g_profileAutoStopped, 0, 0))
        return;
    const bool active = InterlockedCompareExchange(&g_enabled, 0, 0) &&
                        InterlockedCompareExchange(&g_battleActive, 0, 0) == 1;
    if (!active) {
        if (g_pollWindow)
            KillTimer(g_pollWindow, 1);
        battleunitinfo_cancel_sliced();
        invalidatePublishedFrame();
        return;
    }
    if (!g_pollWindow) {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&pollWndProc), &module);
        WNDCLASSEXW cls = {};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = pollWndProc;
        cls.hInstance = module;
        cls.lpszClassName = kPollWindowClass;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;
        g_pollWindow = CreateWindowExW(0, kPollWindowClass, L"", 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, module, nullptr);
    }
    if (g_pollWindow) {
        g_capturePace.reset();
        SetTimer(g_pollWindow, 1, kCapturePauseMs, nullptr);
        // Lifecycle callbacks occur outside game constructors, but defer the expensive native
        // read to a separate UI message as well. Never extract from c4p_tick's worker thread.
        PostMessageW(g_pollWindow, kPollMessage, 0, 0);
    }
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
    // Profile=1 measures twenty completed sliced rosters. Profile=2 also stops after them.
    const int profileMode = host->get_config_int("TwitchStat", "Profile", 0);
    InterlockedExchange(&g_profileBatchesRemaining, profileMode ? 20 : 0);
    InterlockedExchange(&g_profileStopAfterSamples, profileMode == 2 ? 1 : 0);
    InterlockedExchange(&g_profileAutoStopped, 0);
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
    g_framePublished = false;
    g_sessionTime = unixMilliseconds();
    g_capturePace.reset();
    g_lastSliceFailure[0] = 0;
    // Both background workers and UI callbacks need their code for the process lifetime.
    // Pin independently of diagnostic file I/O: a read-only game folder must not break streaming.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&c4p_init), &pinned))
        return 0;
    twitchstat::localBridgeSetEnabled(enabled != 0);
    if (!startFileWriter()) {
        unitLog("local transport unavailable winerr=%lu", GetLastError());
        publishFrame(nullptr);
    }
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
        invalidatePublishedFrame();
        ++g_battleSerial;
        battleunitinfo_reset_battle();
    } else {
        InterlockedExchange(&g_swallowRelease, 0);
        if (g_window)
            PostMessageW(g_window, WM_CLOSE, 0, 0);
    }
    updatePolling();
}

extern "C" HMENU __cdecl c4p_menu(int base_cmd_id)
{
    g_base = base_cmd_id;
    g_menu = CreatePopupMenu();
    if (!g_menu)
        return nullptr;
    AppendMenuA(g_menu, MF_STRING, g_base + kEnabled, "&Enabled");
    AppendMenuA(g_menu, MF_STRING | MF_GRAYED, g_base + kBridgeStatus, "Local connection: off");
    refreshMenu();
    return g_menu;
}

extern "C" void __cdecl c4p_command(int cmd)
{
    if (!g_host || cmd != g_base + kEnabled)
        return;
    const LONG enabled = InterlockedCompareExchange(&g_enabled, 0, 0) ? 0 : 1;
    InterlockedExchange(&g_enabled, enabled);
    twitchstat::localBridgeSetEnabled(enabled != 0);
    if (enabled)
        publishFrame(nullptr);
    if (!enabled) {
        InterlockedExchange(&g_swallowRelease, 0);
        if (g_window)
            ShowWindow(g_window, SW_HIDE);
    }
    g_host->set_config_int("TwitchStat", "Enabled", enabled ? 1 : 0);
    updatePolling();
    refreshMenu();
}

extern "C" int __cdecl c4p_mouse(UINT msg, WPARAM, int x, int y)
{
    if (!g_host)
        return 0;
    // Normal broadcasting is automatic and never consumes game input. LMB remains an opt-in
    // diagnostic gesture only when the user explicitly enables the JSON preview.
    if (!InterlockedCompareExchange(&g_preview, 0, 0))
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

        // Preview reads the latest complete cache. It never bypasses pacing with a full native
        // roster extraction, and never attempts to show a partial/inactive batch.
        ExtractionLatch extraction;
        if (!extraction.acquired) {
            unitLog("LMB ignored: extraction already in progress");
            return 0;
        }
        if (!g_framePublished || GetTickCount() - g_publishedFirstTick >= 2500 ||
            !battleunitinfo_preview_hit(x, y)) {
            unitLog("preview unavailable: no recent complete sliced snapshot");
            return 0;
        }
        if (!showPayload(g_json.data())) {
            unitLog("cached preview failed winerr=%lu", static_cast<unsigned long>(GetLastError()));
        }
        // The preview gesture was handled; do not forward its release to a battle command.
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

extern "C" void __cdecl c4p_shutdown(void)
{
    InterlockedExchange(&g_enabled, 0);
    twitchstat::localBridgeShutdown();
    if (g_pollWindow)
        PostMessageW(g_pollWindow, WM_CLOSE, 0, 0);
    // No joins under a possible loader lock. The plugin stays pinned; each worker exits itself.
    InterlockedExchange(&g_stopFileWorker, 1);
    if (g_fileEvent)
        SetEvent(g_fileEvent);
}
