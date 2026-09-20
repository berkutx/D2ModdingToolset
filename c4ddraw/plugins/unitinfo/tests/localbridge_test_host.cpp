// Standalone transport driver: no game, wrapper, Lua, Node runtime or external web files.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../localbridge.h"
#include "../../../features/c4plugin.h"
#include <iostream>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <cstring>
int runSlicedStateTests();

namespace {
HMODULE plugin = nullptr;
HMENU menu = nullptr;
int configEnabled = 0;
void (__cdecl* pluginCommand)(int) = nullptr;
void (__cdecl* pluginRefresh)() = nullptr;
void (__cdecl* pluginShutdown)() = nullptr;
HWND __cdecl fakeHwnd() { return nullptr; }
int __cdecl configGet(const char* section, const char* key, int fallback) {
    if (!std::strcmp(section,"TwitchStat") && !std::strcmp(key,"Enabled")) return configEnabled;
    return fallback;
}
void __cdecl configSet(const char* section, const char* key, int value) {
    if (!std::strcmp(section,"TwitchStat") && !std::strcmp(key,"Enabled")) configEnabled = value;
}
C4P_Host fakeHost = {};
void setEnabled(bool enabled) {
    if (plugin) { if (bool(configEnabled) != enabled) pluginCommand(1001); }
    else twitchstat::localBridgeSetEnabled(enabled);
}
void shutdownBridge() {
    if (plugin) pluginShutdown();
    else twitchstat::localBridgeShutdown();
}
}

static unsigned long long nowMs()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return (value.QuadPart - 116444736000000000ULL) / 10000;
}

static const char* stateName()
{
    if (plugin) {
        pluginRefresh();
        char status[256] = {};
        GetMenuStringA(menu, 1002, status, sizeof(status), MF_BYCOMMAND);
        if (std::strstr(status,"starting")) return "Starting";
        if (std::strstr(status,"ready")) return "Listening";
        if (std::strstr(status,"busy")) return "PortBusy";
        if (std::strstr(status,"failed")) return "Failed";
        return "Stopped";
    }
    using twitchstat::LocalBridgeState;
    switch (twitchstat::localBridgeState()) {
    case LocalBridgeState::Stopped: return "Stopped";
    case LocalBridgeState::Starting: return "Starting";
    case LocalBridgeState::Listening: return "Listening";
    case LocalBridgeState::PortBusy: return "PortBusy";
    default: return "Failed";
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--check-sliced-state"))
        return runSlicedStateTests();
    if (argc > 1) {
        plugin = LoadLibraryA(argv[1]);
        if (!plugin) return 2;
        const auto init = reinterpret_cast<int (__cdecl*)(const C4P_Host*)>(GetProcAddress(plugin,"c4p_init"));
        const auto createMenu = reinterpret_cast<HMENU (__cdecl*)(int)>(GetProcAddress(plugin,"c4p_menu"));
        pluginCommand = reinterpret_cast<void (__cdecl*)(int)>(GetProcAddress(plugin,"c4p_command"));
        pluginRefresh = reinterpret_cast<void (__cdecl*)()>(GetProcAddress(plugin,"c4p_refresh_menu"));
        pluginShutdown = reinterpret_cast<void (__cdecl*)()>(GetProcAddress(plugin,"c4p_shutdown"));
        if (!init || !createMenu || !pluginCommand || !pluginRefresh || !pluginShutdown) return 3;
        fakeHost.struct_size = sizeof(fakeHost);
        fakeHost.get_hwnd = fakeHwnd;
        fakeHost.get_config_int = configGet;
        fakeHost.set_config_int = configSet;
        if (!init(&fakeHost)) return 4;
        menu = createMenu(1000);
        if (!menu) return 5;
    }
    std::string command;
    while (std::getline(std::cin, command)) {
        LARGE_INTEGER before, after, frequency;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        if (command == "enable") setEnabled(true);
        else if (command == "disable") setEnabled(false);
        else if (command == "cycle") {
            setEnabled(false);
            setEnabled(true);
        } else if (command == "idle") {
            twitchstat::localBridgePublish("", nowMs(), false);
        } else if (command == "oversize") {
            twitchstat::localBridgePublish(std::string(1024 * 1024 + 1, 'x'), nowMs(), true);
        } else if (command.compare(0, 6, "frame ") == 0) {
            const size_t split = command.find(' ', 6);
            const unsigned long long age = std::stoull(command.substr(6, split - 6));
            twitchstat::localBridgePublish(command.substr(split + 1), nowMs() - age, true);
        } else if (command.compare(0, 10, "framefile ") == 0) {
            const size_t split = command.find(' ', 10);
            const unsigned long long age = std::stoull(command.substr(10, split - 10));
            std::ifstream file(command.substr(split + 1), std::ios::binary);
            const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            twitchstat::localBridgePublish(json, nowMs() - age, true);
        } else if (command == "quit") {
            shutdownBridge();
            for (int i = 0; i < 100 && twitchstat::localBridgeState() != twitchstat::LocalBridgeState::Stopped; ++i)
                Sleep(10);
        }
        QueryPerformanceCounter(&after);
        std::cout << "{\"state\":\"" << stateName() << "\",\"error\":" << twitchstat::localBridgeError()
                  << ",\"pid\":" << GetCurrentProcessId() << ",\"us\":"
                  << (after.QuadPart - before.QuadPart) * 1000000 / frequency.QuadPart << "}" << std::endl;
        if (command == "quit") break;
    }
    shutdownBridge();
    return 0;
}
