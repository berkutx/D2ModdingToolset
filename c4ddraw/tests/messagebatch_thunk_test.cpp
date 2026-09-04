// Execute the actual production x86 thunk against our own synthetic stack/site.
// No game module is loaded; Detours modifies only syntheticSite in this process.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include "../upstream/cnc-ddraw/src/detours/detours.h"
#define C4_MESSAGEBATCH_THUNK_TESTING
#include "../features/messagebatch.cpp"

static unsigned identityCalls = 0, admissionRejected = 0, admissionReady = 0;
extern "C" void c4trace_event(unsigned event, uintptr_t, uintptr_t a, uintptr_t,
                              uintptr_t, uintptr_t)
{
    if (event == BatchUnavailable && a == 1) ++admissionRejected;
    if (event == BatchReady) ++admissionReady;
}
// Actual install config parsing is exercised, but ALWAYS rejects before
// exactSites, module PIN, Detours, or any fixed native EXE address access.
extern "C" int c4_exact_game_exe(void) { ++identityCalls; return 0; }
static unsigned mappingCalls = 0;
extern "C" BOOL WINAPI DDMessageBatchPeekRaw(LPMSG msg, HWND hwnd, UINT lo, UINT hi, UINT flags)
{
    return PeekMessageA(msg, hwnd, lo, hi, flags);
}
extern "C" void DDMessageBatchMapRemoved(LPMSG)
{
    ++mappingCalls;
    SetLastError(0x8811);
}
static_assert(sizeof(void*) == 4 && sizeof(MSG) == 28, "Win32 native stack fixture only");

namespace fixture {
struct Pair { HWND hwnd; bool child; unsigned char pad[3]; };
struct Data { uint32_t unknown; Pair* pair; unsigned char unused[80]; void* controller; };
struct Kernel { void* vft; Data* data; };
static_assert(sizeof(Data) == 92, "Native kernel data");
Pair pair = {};
uintptr_t controller = 0x17171717;
Data data = {};
Kernel kernel = {};
MSG input = {}, copied = {}, firstSeen = {};
uintptr_t entryTarget = 0, expectedEsp = 0, observedEsp = 0, observedMsg = 0;
uintptr_t observedEsi = 0, observedEbp = 0, observedEbx = 0, observedEdi = 0;
uint32_t guardBefore = 0, guardAfter = 0, quitValue = 0;
unsigned path = 99, calls = 0, removed = 0, mode = 0, failures = 0, cases = 0;
const char* caseName = "setup";
constexpr DWORD kFirstError = 0x5A11;
constexpr DWORD kExtraError = 0x5A22;

void check(bool value, const char* expression, int line)
{
    if (!value) {
        ++failures;
        std::printf("FAIL %s:%d: %s\n", caseName, line, expression);
    }
}
#define CHECK(expression) fixture::check(!!(expression), #expression, __LINE__)

LRESULT WINAPI originalDispatch(const MSG* message)
{
    if (!calls) {
        observedMsg = reinterpret_cast<uintptr_t>(message);
        firstSeen = *message;
    }
    ++calls;
    SetLastError(calls == 1 ? kFirstError : kExtraError);
    return 0x12345678;
}

BOOL WINAPI fakeCounter(LARGE_INTEGER* value)
{
    value->QuadPart = 1000000;
    SetLastError(0x7711);
    return mode != 3;
}

BOOL WINAPI fakePeek(MSG* message, HWND hwnd, UINT minimum, UINT maximum, UINT flags)
{
    CHECK(!hwnd && !minimum && !maximum);
    if (mode != 1 && mode != 2) return FALSE;
    if (mode == 1 && removed == 3) return FALSE;
    *message = input;
    message->message = g_netMessage;
    message->wParam = 10 + removed;
    if (flags & PM_REMOVE) {
        ++removed;
        if (mode == 2) {
            message->hwnd = nullptr;
            message->message = WM_QUIT;
            message->wParam = 0xCAFEBEEF;
        }
    }
    SetLastError(0x7722);
    return TRUE;
}

__declspec(naked) void finish()
{
    __asm {
        mov observedEsp, esp
        mov observedEsi, esi
        mov observedEbp, ebp
        mov observedEbx, ebx
        mov observedEdi, edi
        mov eax, [esp+10h]
        mov guardBefore, eax
        mov eax, [esp+30h]
        mov guardAfter, eax
        lea esi, [esp+14h]
        mov edi, offset copied
        mov ecx, 7
        rep movsd
        add esp, 40h
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

__declspec(naked) void resumeLanding()
{
    __asm {
        mov path, 0
        // First original instruction at native resume 562979.
        mov eax, [esi+4]
        jmp finish
    }
}

__declspec(naked) void quitLanding()
{
    __asm {
        mov path, 1
        // Native quit epilogue reads original MSG.wParam at ESP+1Ch.
        mov eax, [esp+1Ch]
        mov quitValue, eax
        jmp finish
    }
}

__declspec(naked) void syntheticSite()
{
    __asm {
        lea eax, [esp+14h]
        push eax
        call ebp
        jmp resumeLanding
    }
}

__declspec(naked) void enter()
{
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        sub esp, 40h
        mov expectedEsp, esp
        mov dword ptr [esp+10h], 0ABCDEF01h
        mov dword ptr [esp+30h], 010FEDCBAh
        lea edi, [esp+14h]
        mov esi, offset input
        mov ecx, 7
        rep movsd
        mov esi, offset kernel
        mov ebp, offset originalDispatch
        mov ebx, 0EB123400h
        mov edi, 0ED876543h
        jmp dword ptr [entryTarget]
    }
}

void run(const char* name, unsigned selectedMode, bool enabled, uintptr_t target,
         unsigned expectedCalls, bool expectedQuit)
{
    caseName = name;
    ++cases;
    mode = selectedMode;
    path = 99; calls = removed = quitValue = 0;
    mappingCalls = 0;
    expectedEsp = observedEsp = observedMsg = 0;
    std::memset(&copied, 0, sizeof(copied));
    std::memset(&firstSeen, 0, sizeof(firstSeen));
    g_enabled = enabled ? 1 : 0;
    g_depth = g_epoch = g_modalMask = 0;
    entryTarget = target;
    SetLastError(0x1234);
    enter();
    const DWORD error = GetLastError();
    CHECK(observedEsp == expectedEsp);
    CHECK(observedMsg == expectedEsp + 0x14);
    CHECK(observedEsi == reinterpret_cast<uintptr_t>(&kernel));
    CHECK(observedEbp == reinterpret_cast<uintptr_t>(&originalDispatch));
    CHECK(observedEbx == 0xEB123400 && observedEdi == 0xED876543);
    CHECK(guardBefore == 0xABCDEF01 && guardAfter == 0x10FEDCBA);
    CHECK(!std::memcmp(&firstSeen, &input, sizeof(input)));
    CHECK(calls == expectedCalls);
    CHECK(mappingCalls == expectedCalls - 1);
    CHECK(path == (expectedQuit ? 1u : 0u));
    CHECK(g_depth == 0);
    CHECK(error == kFirstError);
    if (expectedQuit) {
        CHECK(quitValue == 0xCAFEBEEF);
        CHECK(copied.message == WM_QUIT && copied.wParam == 0xCAFEBEEF);
    } else {
        CHECK(!std::memcmp(&copied, &input, sizeof(input)));
    }
}

void admissionConfigTests(HWND hwnd)
{
    caseName = "private admission config setup";
    char directory[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, directory, MAX_PATH);
    if (!length || length >= MAX_PATH ||
        !std::strstr(directory, "\\.diagnostics\\messagebatch-thunk-tests\\")) {
        CHECK(false); // Never write test INIs outside the dedicated test case.
        return;
    }
    char* slash = std::strrchr(directory, '\\');
    if (!slash) { CHECK(false); return; }
    *slash = 0;
    struct ConfigCase { const char* name; const char* contents; bool attemptEnabled; };
    const ConfigCase configs[] = {
        {"missing INI defaults to enabled admission", nullptr, true},
        {"missing key defaults to enabled admission", "[unrelated]\r\nvalue=1\r\n", true},
        {"explicit zero bypasses admission", "[menu]\r\nmessageBatching=0\r\n", false},
        {"explicit one attempts enabled admission", "[menu]\r\nmessageBatching=1\r\n", true},
        {"invalid value fails closed", "[menu]\r\nmessageBatching=garbage\r\n", false},
        {"empty value fails closed", "[menu]\r\nmessageBatching=\r\n", false}
    };
    unsigned index = 0;
    for (const auto& config : configs) {
        caseName = config.name;
        ++cases;
        char ini[MAX_PATH] = {};
        const int count = sprintf_s(ini, "%s\\admission-%lu-%u.ini", directory,
                                    GetCurrentProcessId(), index++);
        if (count <= 0) { CHECK(false); continue; }
        if (config.contents) {
            const HANDLE file = CreateFileA(ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) { CHECK(false); continue; }
            DWORD written = 0;
            const DWORD bytes = static_cast<DWORD>(std::strlen(config.contents));
            const BOOL ok = WriteFile(file, config.contents, bytes, &written, nullptr);
            CloseHandle(file);
            if (!ok || written != bytes) { CHECK(false); continue; }
        } else {
            CHECK(GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES);
        }
        g_installState = 0;
        g_enabled = 0;
        identityCalls = admissionRejected = admissionReady = 0;
        const PVOID priorTarget = g_unusedTrampoline;
        SetLastError(0x6A11);
        messagebatch_install(hwnd, ini);
        const DWORD error = GetLastError();
        const unsigned expected = config.attemptEnabled ? 1 : 0;
        CHECK(identityCalls == expected);
        CHECK(admissionRejected == expected);
        CHECK(admissionReady == 0);
        CHECK(g_installState == 2 && g_enabled == 0);
        CHECK(g_unusedTrampoline == priorTarget);
        CHECK(error == 0x6A11);
        // A second dispatch cannot silently retry admission after the latch.
        messagebatch_install(hwnd, ini);
        CHECK(identityCalls == expected && admissionRejected == expected);
        std::printf("ADMISSION: %s; identity=%u rejected=%u ready=%u enabled=%ld\n",
                    config.name, identityCalls, admissionRejected, admissionReady, g_enabled);
    }
}
} // namespace fixture

int main()
{
    using namespace fixture;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "C4MessageBatchPrivateThunkTest";
    if (!RegisterClassA(&wc)) return 10;
    const HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "private hidden thunk fixture", 0,
                                     0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 11;
    pair.hwnd = hwnd;
    data.pair = &pair;
    data.controller = &controller;
    kernel.data = &data;
    input.hwnd = hwnd;
    input.message = RegisterWindowMessageA("C4PrivateThunkOriginal");
    input.wParam = 0x11223344; input.lParam = 0x55667788;
    input.time = 12345; input.pt = {13, 17};
    g_uiThread = GetCurrentThreadId();
    g_mainHwnd = hwnd;
    g_netMessage = RegisterWindowMessageA("MIDGARD NETMSG");
    g_queueMessage = RegisterWindowMessageA("MQ_COMMANDQUEUE2");
    g_frequency.QuadPart = 1000000;
    g_ops.peek = fakePeek;
    g_ops.counter = fakeCounter;
    g_resume = reinterpret_cast<uintptr_t>(&resumeLanding);
    g_quit = reinterpret_cast<uintptr_t>(&quitLanding);
    const uintptr_t thunk = reinterpret_cast<uintptr_t>(&batchThunk);
    const uintptr_t site = reinterpret_cast<uintptr_t>(&syntheticSite);
    run("unpatched synthetic original baseline", 0, false, site, 1, false);
    run("actual thunk disabled", 0, false, thunk, 1, false);
    run("actual thunk first plus three extras", 1, true, thunk, 4, false);
    run("actual thunk removed quit native branch", 2, true, thunk, 1, true);
    run("actual thunk clock fail closed", 3, true, thunk, 1, false);

    // Exercise the real bundled Detours engine on our own exact 5-byte seam.
    const unsigned char expected[] = {0x8D, 0x44, 0x24, 0x14, 0x50, 0xFF, 0xD5};
    CHECK(!std::memcmp(reinterpret_cast<void*>(site), expected, sizeof(expected)));
    PVOID trampoline = reinterpret_cast<PVOID>(site);
    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR) status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR) status = DetourAttach(&trampoline, batchThunk);
    if (status == NO_ERROR) status = DetourTransactionCommit();
    else DetourTransactionAbort();
    CHECK(status == NO_ERROR);
    if (status == NO_ERROR) {
        CHECK(!std::memcmp(reinterpret_cast<void*>(site + 5), expected + 5, 2));
        run("private Detours site disabled", 0, false, site, 1, false);
        run("private Detours site extra batch", 1, true, site, 4, false);
        run("private Detours site quit branch", 2, true, site, 1, true);
        status = DetourTransactionBegin();
        if (status == NO_ERROR) status = DetourUpdateThread(GetCurrentThread());
        if (status == NO_ERROR) status = DetourDetach(&trampoline, batchThunk);
        if (status == NO_ERROR) status = DetourTransactionCommit();
        else DetourTransactionAbort();
        CHECK(status == NO_ERROR);
        CHECK(!std::memcmp(reinterpret_cast<void*>(site), expected, sizeof(expected)));
        run("private site restored after detach", 0, false, site, 1, false);
    }
    admissionConfigTests(hwnd);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    std::printf("RESULT=%s cases=%u failures=%u; actual x86 thunk + private Detours site + rejected install admission\n",
                failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
