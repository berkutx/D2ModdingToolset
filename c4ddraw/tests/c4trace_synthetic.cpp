#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <algorithm>
#include "../features/c4trace.h"

extern "C" void c4trace_test_limits(unsigned long long cap, int lowDisk);
extern "C" void c4trace_test_stop(void);
extern "C" int c4trace_test_workers(void);
extern "C" int c4trace_test_finished(void);
extern "C" void c4trace_test_write_failure(void);
extern "C" unsigned c4trace_test_last_error(void);
extern "C" int c4trace_header_smoke(void);

static volatile LONG g_failures = 0;
static bool g_staggerEmitters = false;
static void check(bool condition, const char* description)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", description); InterlockedIncrement(&g_failures); }
}

static DWORD WINAPI emit(void* context)
{
    const unsigned count = static_cast<unsigned>(reinterpret_cast<uintptr_t>(context));
    for (unsigned i = 0; i != count; ++i) {
        SetLastError(0xC4DE1234);
        c4trace_event(101, 0x12345678, i, GetCurrentThreadId(), 0xAABBCCDD, 0xFFFFFFFF);
        if (GetLastError() != 0xC4DE1234) InterlockedIncrement(&g_failures);
        // Harness only: keep emitting across several writer swaps (not a recorder wait).
        if (g_staggerEmitters && (i % 256) == 0) Sleep(1);
    }
    return 0;
}

static std::vector<std::string> traceFiles(const std::string& directory)
{
    std::vector<std::string> result;
    WIN32_FIND_DATAA data = {};
    HANDLE search = FindFirstFileA((directory + "C4trace-*.csv").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return result;
    do { result.push_back(directory + data.cFileName); } while (FindNextFileA(search, &data));
    FindClose(search);
    return result;
}

static void configuredQueries(const std::string& ini, const std::string& directory)
{
    struct ValueCase { const char* value; int expected; };
    const ValueCase iniCases[] = {
        {NULL, 0}, {"", 0}, {"0", 0}, {"1", 1}, {" 1 ", 1}, {"\t1\t", 1},
        {"true", 0}, {"yes", 0}, {"01", 0}, {"1junk", 0}, {"+1", 0},
        {"1 0", 0}, {"1111111111111111111111111111111111111111", 0}
    };
    for (const auto& test : iniCases) {
        check(WritePrivateProfileStringA("menu", "netTrace", test.value, ini.c_str()) != FALSE,
              "write isolated INI case");
        SetEnvironmentVariableA("C4DLL_NETTRACE", "1");
        SetLastError(0xC4DE0003);
        check(c4trace_configured(ini.c_str()) == test.expected, "configured literal INI semantics, not environment");
        check(GetLastError() == 0xC4DE0003, "configured preserves last error");
        check(c4trace_enabled() == 0 && c4trace_test_workers() == 0, "query does not start recording");
    }
    SetLastError(0xC4DE0004);
    check(c4trace_configured(NULL) == 0 && c4trace_configured("") == 0 &&
          c4trace_configured((directory + "does-not-exist.ini").c_str()) == 0,
          "missing INI/path disabled");
    check(GetLastError() == 0xC4DE0004, "invalid path query preserves last error");
    const ValueCase envCases[] = {
        {NULL, 0}, {"", 0}, {"0", 0}, {"1", 1}, {" 1", 0}, {"1 ", 0},
        {"\t1", 0}, {"true", 0}, {"yes", 0}, {"01", 0}, {"1junk", 0},
        {"1111111111111111111111111111111111111111", 0}
    };
    for (const auto& test : envCases) {
        check(SetEnvironmentVariableA("C4DLL_NETTRACE", test.value) != FALSE, "set isolated process environment case");
        SetLastError(0xC4DE0005);
        check(c4trace_environment_forced() == test.expected, "environment exact one semantics");
        check(GetLastError() == 0xC4DE0005, "environment query preserves last error");
    }
    WritePrivateProfileStringA("menu", "netTrace", "0", ini.c_str());
    SetEnvironmentVariableA("C4DLL_NETTRACE", NULL);
    c4trace_init();
    check(c4trace_enabled() == 0, "initial off is latched");
    WritePrivateProfileStringA("menu", "netTrace", "1", ini.c_str());
    SetEnvironmentVariableA("C4DLL_NETTRACE", "1");
    check(c4trace_configured(ini.c_str()) == 1 && c4trace_environment_forced() == 1,
          "queries see requested changes after initialization");
    c4trace_init();
    check(c4trace_enabled() == 0 && c4trace_test_workers() == 0,
          "requested changes do not reinitialize or start a latched-off recorder");
    check(traceFiles(directory).empty(), "queries and latched-off init create no trace");
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    char executable[MAX_PATH] = {};
    GetModuleFileNameA(NULL, executable, MAX_PATH);
    const std::string directory = std::string(executable).substr(0, std::string(executable).find_last_of('\\') + 1);
    const std::string ini = directory + "C4menu.ini";
    if (mode == "config") {
        configuredQueries(ini, directory);
        printf("config: %s\n", g_failures ? "FAIL" : "PASS");
        return g_failures ? 1 : 0;
    }
    const std::vector<std::string> previousFiles = traceFiles(directory);
    const size_t before = previousFiles.size();
    SetEnvironmentVariableA("C4DLL_NETTRACE", NULL);
    if (mode == "ini") WritePrivateProfileStringA("menu", "netTrace", "1", ini.c_str());
    else if (mode == "invalid") {
        WritePrivateProfileStringA("menu", "netTrace", "true", ini.c_str());
        SetEnvironmentVariableA("C4DLL_NETTRACE", "true");
    } else if (mode != "off") SetEnvironmentVariableA("C4DLL_NETTRACE", "1");
    const bool off = mode == "off" || mode == "invalid" || mode == "lowdisk";
    c4trace_test_limits(mode == "cap" ? 8192 : 32ULL * 1024 * 1024, mode == "lowdisk");
    if (mode == "iofailure") c4trace_test_write_failure();
    check(c4trace_header_smoke() == 0, "C-compatible header and initial disabled state");
    SetLastError(0xC4DE0001);
    c4trace_init();
    check(GetLastError() == 0xC4DE0001, "init preserves last error");
    SetLastError(0xC4DE0002);
    check((c4trace_enabled() != 0) == !off, "expected initial enabled state");
    check(GetLastError() == 0xC4DE0002, "enabled preserves last error");
    c4trace_init(); // Idempotent, never a second writer.
    if (mode == "on") {
        SetEnvironmentVariableA("C4DLL_NETTRACE", NULL);
        WritePrivateProfileStringA("menu", "netTrace", "0", ini.c_str());
        check(!c4trace_configured(ini.c_str()) && !c4trace_environment_forced(),
              "requested off is separately visible while recording");
        c4trace_init();
        check(c4trace_enabled() != 0, "requested off does not detach/reinitialize a running recorder");
    }
    if (mode == "concurrent") {
        g_staggerEmitters = true;
        HANDLE workers[8] = {};
        for (unsigned i = 0; i != 8; ++i)
            workers[i] = CreateThread(NULL, 0, emit, reinterpret_cast<void*>(15000), 0, NULL);
        check(WaitForMultipleObjects(8, workers, TRUE, 10000) == WAIT_OBJECT_0, "emitters complete without blocking");
        for (unsigned i = 0; i != 8; ++i) CloseHandle(workers[i]);
    } else {
        emit(reinterpret_cast<void*>(mode == "cap" ? 15000 : 64));
    }
    if (mode == "cap" || mode == "iofailure") {
        for (unsigned i = 0; c4trace_enabled() && i != 40; ++i) Sleep(100);
        check(!c4trace_enabled(), "cap or IO failure disables further recording");
    } else Sleep(off ? 350 : 750);
    if (!off) {
        c4trace_test_stop();
        for (unsigned i = 0; !c4trace_test_finished() && i != 40; ++i) Sleep(100);
        check(c4trace_test_finished() != 0, "writer finishes");
        check(c4trace_test_workers() == 1, "exactly one writer");
    } else check(c4trace_test_workers() == 0, "disabled creates no writer");
    const std::vector<std::string> files = traceFiles(directory);
    check(files.size() == before + (off ? 0 : 1), "new-file-only output count");
    if (!off && !files.empty()) {
        std::string path;
        for (size_t i = 0; i != files.size(); ++i)
            if (std::find(previousFiles.begin(), previousFiles.end(), files[i]) == previousFiles.end())
                path = files[i];
        check(!path.empty(), "identify newly created trace");
        std::ifstream input(path.c_str(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        check(text.find("#schema,C4trace,1\n") == 0, "schema header");
        check(text.find("#qpc_frequency,") != std::string::npos, "frequency metadata");
        check(text.find("#start_utc,") != std::string::npos, "UTC metadata");
        check(text.find("seq,qpc,tick,tid,event,object,a,b,c,d\n") != std::string::npos, "CSV columns");
        if (mode != "iofailure") {
            check(text.find(",101,0x12345678,") != std::string::npos, "event payload present");
            check(text.find("#stop,") != std::string::npos, "final status recorded");
        } else check(c4trace_test_last_error() == ERROR_WRITE_FAULT,
                     "IO failure reported out-of-band when disk writes fail");
        if (mode == "cap") {
            check(text.size() <= 8192, "cap never exceeded");
            check(text.find("reason=cap") != std::string::npos, "cap reason reported");
        }
        if (mode == "concurrent") {
            check(text.find("#status,lock_drops=") != std::string::npos, "overload reports drops");
            check(text.find("lock_drops=0,buffer_drops=0,") == std::string::npos,
                  "concurrent overload produces counted drops");
        }
        printf("%s: %s (%llu bytes)\n", mode.c_str(), path.c_str(), static_cast<unsigned long long>(text.size()));
    }
    printf("%s: %s\n", mode.c_str(), g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
