#include "c4trace.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace {

const unsigned kBufferRecords = 8192; // Two buffers: exactly 16,384 records total.
const ULONGLONG kFileCap = 32ULL * 1024 * 1024;
const ULONGLONG kMinimumFreeBytes = 64ULL * 1024 * 1024;
const unsigned kFooterReserve = 1024;

struct Record {
    LONGLONG qpc;
    LONGLONG sequence;
    DWORD tick;
    DWORD thread;
    unsigned event;
    uintptr_t object, a, b, c, d;
};

Record g_records[2][kBufferRecords];
unsigned g_counts[2] = {};
unsigned g_active = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_initialized = 0;
volatile LONG g_enabled = 0;
__declspec(align(8)) volatile LONG64 g_sequence = 0;
__declspec(align(8)) volatile LONG64 g_lockDrops = 0;
__declspec(align(8)) volatile LONG64 g_bufferDrops = 0;
__declspec(align(8)) volatile LONG64 g_accepted = 0;
wchar_t g_directory[MAX_PATH] = {};
LARGE_INTEGER g_frequency = {};
LARGE_INTEGER g_startedQpc = {};
SYSTEMTIME g_startedUtc = {};
DWORD g_processId = 0;

#ifdef C4TRACE_TESTING
ULONGLONG g_testCap = kFileCap;
volatile LONG g_testLowDisk = 0;
volatile LONG g_testStop = 0;
volatile LONG g_testWorkers = 0;
volatile LONG g_testFinished = 0;
volatile LONG g_testWriteFailure = 0;
volatile LONG g_testLastError = 0;
#endif

struct LastErrorGuard {
    DWORD value;
    LastErrorGuard() : value(GetLastError()) {}
    ~LastErrorGuard() { SetLastError(value); }
};

LONG64 readCounter(volatile LONG64* value)
{
    return InterlockedCompareExchange64(value, 0, 0);
}

ULONGLONG fileCap()
{
#ifdef C4TRACE_TESTING
    return g_testCap;
#else
    return kFileCap;
#endif
}

bool enoughDisk(DWORD* error)
{
#ifdef C4TRACE_TESTING
    if (InterlockedCompareExchange(&g_testLowDisk, 0, 0)) {
        *error = ERROR_DISK_FULL;
        return false;
    }
#endif
    ULARGE_INTEGER available = {};
    if (!GetDiskFreeSpaceExW(g_directory, &available, NULL, NULL)) {
        *error = GetLastError();
        return false;
    }
    if (available.QuadPart < kMinimumFreeBytes) {
        *error = ERROR_DISK_FULL;
        return false;
    }
    return true;
}

template<class Character> bool literalOne(const Character* value)
{
    // INI whitespace is harmless; values such as true, 01, 1junk remain disabled.
    while (*value == ' ' || *value == '\t') ++value;
    if (*value++ != '1') return false;
    while (*value == ' ' || *value == '\t') ++value;
    return *value == 0;
}

bool requested()
{
    wchar_t value[32] = {};
    const bool environmentEnabled = c4trace_environment_forced() != 0;
    wchar_t ini[MAX_PATH] = {};
    if (_snwprintf_s(ini, MAX_PATH, _TRUNCATE, L"%lsC4menu.ini", g_directory) < 0)
        return environmentEnabled;
    const DWORD copied = GetPrivateProfileStringW(L"menu", L"netTrace", L"0", value, 32, ini);
    return environmentEnabled || (copied < 31 && literalOne(value));
}

// All formatting, filesystem operations, and waits below run on the writer.
bool writeBytes(HANDLE file, const char* data, DWORD length, ULONGLONG* bytes,
                DWORD* error)
{
#ifdef C4TRACE_TESTING
    if (*bytes != 0 && InterlockedCompareExchange(&g_testWriteFailure, 0, 0)) {
        *error = ERROR_WRITE_FAULT;
        return false;
    }
#endif
    DWORD written = 0;
    const BOOL result = WriteFile(file, data, length, &written, NULL);
    const DWORD failure = result ? ERROR_WRITE_FAULT : GetLastError();
    *bytes += written;
    if (!result || written != length) {
        *error = failure;
        return false;
    }
    return true;
}

void debugStop(const char* reason, DWORD error)
{
#ifdef C4TRACE_TESTING
    InterlockedExchange(&g_testLastError, error);
#endif
    char text[192] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE,
                "[C4trace] stopped: %s; win32_error=%lu; lock_drops=%lld; buffer_drops=%lld\n",
                reason, error, readCounter(&g_lockDrops), readCounter(&g_bufferDrops));
    OutputDebugStringA(text);
}

HANDLE openOutput(DWORD* error)
{
    wchar_t name[MAX_PATH] = {};
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        if (_snwprintf_s(name, MAX_PATH, _TRUNCATE,
                        L"%lsC4trace-%lu-%04u%02u%02uT%02u%02u%02u%03u-%llX-%u.csv",
                        g_directory, g_processId, g_startedUtc.wYear, g_startedUtc.wMonth,
                        g_startedUtc.wDay, g_startedUtc.wHour, g_startedUtc.wMinute,
                        g_startedUtc.wSecond, g_startedUtc.wMilliseconds,
                        static_cast<unsigned long long>(g_startedQpc.QuadPart), attempt) < 0) {
            *error = ERROR_FILENAME_EXCED_RANGE;
            return INVALID_HANDLE_VALUE;
        }
        HANDLE file = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (file != INVALID_HANDLE_VALUE) return file;
        *error = GetLastError();
        if (*error != ERROR_FILE_EXISTS && *error != ERROR_ALREADY_EXISTS) break;
    }
    return INVALID_HANDLE_VALUE;
}

DWORD WINAPI writerMain(void*)
{
#ifdef C4TRACE_TESTING
    InterlockedIncrement(&g_testWorkers);
#endif
    DWORD error = 0;
    const char* reason = "io_failure";
    ULONGLONG bytes = 0;
    ULONGLONG writtenRecords = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool healthy = enoughDisk(&error);
    if (!healthy) reason = "low_disk_or_disk_query_failure";
    if (healthy) {
        file = openOutput(&error);
        healthy = file != INVALID_HANDLE_VALUE;
    }
    char chunk[65536];
    if (healthy) {
        const int size = _snprintf_s(chunk, sizeof(chunk), _TRUNCATE,
            "#schema,C4trace,1\n#pid,%lu\n#qpc_frequency,%lld\n"
            "#start_utc,%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\n"
            "#start_qpc,%lld\n#record_capacity,16384\n#file_cap_bytes,%llu\n"
            "#warning,diagnostics_perturb_scheduling_and_may_drop_records\n"
            "seq,qpc,tick,tid,event,object,a,b,c,d\n",
            g_processId, g_frequency.QuadPart, g_startedUtc.wYear, g_startedUtc.wMonth,
            g_startedUtc.wDay, g_startedUtc.wHour, g_startedUtc.wMinute,
            g_startedUtc.wSecond, g_startedUtc.wMilliseconds, g_startedQpc.QuadPart,
            fileCap());
        healthy = size > 0 && writeBytes(file, chunk, static_cast<DWORD>(size), &bytes, &error);
    }
    LONG64 lastLockDrops = 0, lastBufferDrops = 0;
    unsigned flushTicks = 0;
    while (healthy) {
        Sleep(250);
#ifdef C4TRACE_TESTING
        if (InterlockedCompareExchange(&g_testStop, 0, 0)) {
            reason = "test_stop";
            break;
        }
#endif
        if (!enoughDisk(&error)) {
            reason = "low_disk_or_disk_query_failure";
            break;
        }
        // Producer critical sections only copy one fixed record. No waiting producer.
        if (!TryAcquireSRWLockExclusive(&g_lock)) continue;
        const unsigned drain = g_active;
        const unsigned count = g_counts[drain];
        g_active ^= 1;
        g_counts[g_active] = 0;
        ReleaseSRWLockExclusive(&g_lock);

        unsigned used = 0;
        unsigned chunkRecords = 0;
        for (unsigned i = 0; healthy && i != count; ++i) {
            const Record& record = g_records[drain][i];
            char row[256];
            const int size = _snprintf_s(row, sizeof(row), _TRUNCATE,
                "%lld,%lld,%lu,%lu,%u,0x%llX,0x%llX,0x%llX,0x%llX,0x%llX\n",
                record.sequence, record.qpc, record.tick, record.thread, record.event,
                static_cast<unsigned long long>(record.object),
                static_cast<unsigned long long>(record.a),
                static_cast<unsigned long long>(record.b),
                static_cast<unsigned long long>(record.c),
                static_cast<unsigned long long>(record.d));
            if (size <= 0) { error = ERROR_INVALID_DATA; healthy = false; break; }
            if (bytes + used + size + kFooterReserve > fileCap()) {
                reason = "cap";
                healthy = false;
                break;
            }
            if (used + size > sizeof(chunk)) {
                if (!writeBytes(file, chunk, used, &bytes, &error)) {
                    healthy = false;
                    break;
                }
                writtenRecords += chunkRecords;
                used = 0;
                chunkRecords = 0;
            }
            memcpy(chunk + used, row, size);
            used += size;
            ++chunkRecords;
        }
        // Preserve already formatted rows even when the next one reached the cap.
        if (used && (healthy || reason[0] == 'c')) {
            if (writeBytes(file, chunk, used, &bytes, &error)) writtenRecords += chunkRecords;
            else { healthy = false; reason = "io_failure"; }
        }
        if (!healthy) break;
        const LONG64 lockDrops = readCounter(&g_lockDrops);
        const LONG64 bufferDrops = readCounter(&g_bufferDrops);
        if (lockDrops != lastLockDrops || bufferDrops != lastBufferDrops) {
            const int size = _snprintf_s(chunk, sizeof(chunk), _TRUNCATE,
                "#status,lock_drops=%lld,buffer_drops=%lld,written=%llu\n",
                lockDrops, bufferDrops, writtenRecords);
            if (bytes + size + kFooterReserve > fileCap()) { reason = "cap"; break; }
            if (!writeBytes(file, chunk, static_cast<DWORD>(size), &bytes, &error)) break;
            lastLockDrops = lockDrops;
            lastBufferDrops = bufferDrops;
        }
        if (++flushTicks == 4) {
            flushTicks = 0;
            if (!FlushFileBuffers(file)) { error = GetLastError(); break; }
        }
    }
    InterlockedExchange(&g_enabled, 0);
    // Writer-only shutdown barrier: complete any already-entered fixed-copy section.
    AcquireSRWLockExclusive(&g_lock);
    const LONG64 accepted = readCounter(&g_accepted);
    ReleaseSRWLockExclusive(&g_lock);
    // Diagnostics are fail-closed: no restart, no extra files, no mutation of gameplay.
    if (file != INVALID_HANDLE_VALUE) {
        const int size = _snprintf_s(chunk, sizeof(chunk), _TRUNCATE,
            "#stop,reason=%s,win32_error=%lu,lock_drops=%lld,buffer_drops=%lld,"
            "accepted=%lld,written=%llu,unwritten=%llu\n",
            reason, error, readCounter(&g_lockDrops), readCounter(&g_bufferDrops),
            accepted, writtenRecords,
            static_cast<ULONGLONG>(accepted) - writtenRecords);
        if (size > 0 && bytes + size <= fileCap()) {
            DWORD footerError = 0;
            if (!writeBytes(file, chunk, static_cast<DWORD>(size), &bytes, &footerError))
                error = footerError;
        }
        if (!FlushFileBuffers(file)) error = GetLastError();
        CloseHandle(file);
    }
    debugStop(reason, error); // Also reports IO errors when the file cannot be written.
#ifdef C4TRACE_TESTING
    InterlockedExchange(&g_testFinished, 1);
#endif
    return 0;
}

} // namespace

extern "C" void c4trace_init(void)
{
    LastErrorGuard preserve;
    if (InterlockedCompareExchange(&g_initialized, 1, 0)) return;
    const DWORD length = GetModuleFileNameW(NULL, g_directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(g_directory, L'\\');
    if (!slash) return;
    slash[1] = 0;
    if (!requested()) return;
    DWORD error = 0;
    if (!enoughDisk(&error) || !QueryPerformanceFrequency(&g_frequency) ||
        g_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&g_startedQpc)) return;
    GetSystemTime(&g_startedUtc);
    g_processId = GetCurrentProcessId();
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&c4trace_init), &module)) return;
    InterlockedExchange(&g_enabled, 1);
    HANDLE thread = CreateThread(NULL, 0, writerMain, NULL, 0, NULL);
    if (!thread) {
        InterlockedExchange(&g_enabled, 0);
        return;
    }
    CloseHandle(thread);
}

extern "C" int c4trace_enabled(void)
{
    LastErrorGuard preserve;
    return InterlockedCompareExchange(&g_enabled, 0, 0) != 0;
}

extern "C" int c4trace_configured(const char* iniPath)
{
    LastErrorGuard preserve;
    if (!iniPath || !*iniPath) return 0;
    char value[32] = {};
    const DWORD copied = GetPrivateProfileStringA("menu", "netTrace", "0", value, 32, iniPath);
    return copied < 31 && literalOne(value) ? 1 : 0;
}

extern "C" int c4trace_environment_forced(void)
{
    LastErrorGuard preserve;
    wchar_t value[32] = {};
    const DWORD length = GetEnvironmentVariableW(L"C4DLL_NETTRACE", value, 32);
    return length == 1 && value[0] == L'1' ? 1 : 0;
}

extern "C" void c4trace_event(unsigned event, uintptr_t object, uintptr_t a, uintptr_t b,
                               uintptr_t c, uintptr_t d)
{
    LastErrorGuard preserve;
    if (!InterlockedCompareExchange(&g_enabled, 0, 0)) return;
    Record record = {};
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) return;
    record.qpc = counter.QuadPart;
    record.tick = GetTickCount();
    record.thread = GetCurrentThreadId();
    record.sequence = InterlockedIncrement64(&g_sequence);
    record.event = event;
    record.object = object;
    record.a = a; record.b = b; record.c = c; record.d = d;
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        InterlockedIncrement64(&g_lockDrops);
        return;
    }
    if (InterlockedCompareExchange(&g_enabled, 0, 0)) {
        if (g_counts[g_active] < kBufferRecords) {
            g_records[g_active][g_counts[g_active]++] = record;
            InterlockedIncrement64(&g_accepted);
        } else {
            InterlockedIncrement64(&g_bufferDrops);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

#ifdef C4TRACE_TESTING
extern "C" void c4trace_test_limits(unsigned long long cap, int lowDisk)
{
    g_testCap = cap;
    InterlockedExchange(&g_testLowDisk, lowDisk);
}
extern "C" void c4trace_test_stop(void) { InterlockedExchange(&g_testStop, 1); }
extern "C" int c4trace_test_workers(void) { return g_testWorkers; }
extern "C" int c4trace_test_finished(void) { return g_testFinished; }
extern "C" void c4trace_test_write_failure(void) { InterlockedExchange(&g_testWriteFailure, 1); }
extern "C" unsigned c4trace_test_last_error(void) { return static_cast<unsigned>(g_testLastError); }
#endif
