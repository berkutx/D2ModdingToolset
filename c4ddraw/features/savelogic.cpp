/*
 * Save conveniences ported from the legacy closed-source C4dll-R wrapper.
 *
 * This feature deliberately hooks Win32 file APIs instead of Discipl2.exe addresses, so the
 * Akella, Russobit, GOG and editor executables all use the same implementation:
 *   - hold Ctrl while saving -> QuickSaveNNN.sg (next free sequence number);
 *   - [Wrapper] Archive=1 -> copy a closed save to Archive\YYYYMMDD;
 *   - hold Shift while saving -> archive once even when Archive=0;
 *   - [Wrapper] IncludeSubdirectories=1 -> expose archived .sg files in the normal save list.
 *
 * The INI keys and archive naming scheme match the old wrapper. The recursive enumerator keeps
 * real Win32 search handles and also hooks FindClose, fixing the child-handle leak in the legacy
 * implementation.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" int featuremenu_debug_enabled(void);

namespace {

constexpr size_t kLongPath = 2048;
constexpr int kMaxTrackedSaves = 16;
constexpr int kMaxFindSessions = 16;
constexpr int kMaxFindDepth = 16;

HANDLE(WINAPI* g_realCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) =
    CreateFileA;
BOOL(WINAPI* g_realCloseHandle)(HANDLE) = CloseHandle;
HANDLE(WINAPI* g_realFindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA) = FindFirstFileA;
BOOL(WINAPI* g_realFindNextFileA)(HANDLE, LPWIN32_FIND_DATAA) = FindNextFileA;
BOOL(WINAPI* g_realFindClose)(HANDLE) = FindClose;

CRITICAL_SECTION g_lock{};
bool g_lockReady = false;
char g_iniPath[MAX_PATH] = {};

struct ScopedLock
{
    ScopedLock()
    {
        if (g_lockReady)
            EnterCriticalSection(&g_lock);
    }
    ~ScopedLock()
    {
        if (g_lockReady)
            LeaveCriticalSection(&g_lock);
    }
};

struct SaveHandle
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool forceArchive = false;
    char path[kLongPath] = {};
};

struct FindFrame
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    char relative[kLongPath] = {};
};

struct FindSession
{
    bool used = false;
    bool exhausted = false;
    HANDLE root = INVALID_HANDLE_VALUE;
    int depth = 0;
    char base[kLongPath] = {};
    FindFrame frames[kMaxFindDepth]{};
};

SaveHandle g_saves[kMaxTrackedSaves]{};
FindSession g_finds[kMaxFindSessions]{};

bool copyText(char* dst, size_t cap, const char* src)
{
    if (!dst || !cap || !src)
        return false;
    const size_t n = strlen(src);
    if (n >= cap) {
        dst[0] = 0;
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

bool formatText(char* dst, size_t cap, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(dst, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
    return n >= 0 && static_cast<size_t>(n) < cap;
}

void initIniPath()
{
    GetModuleFileNameA(nullptr, g_iniPath, static_cast<DWORD>(sizeof(g_iniPath)));
    char* slash = strrchr(g_iniPath, '\\');
    if (!slash)
        slash = strrchr(g_iniPath, '/');
    if (slash)
        copyText(slash + 1, sizeof(g_iniPath) - static_cast<size_t>(slash + 1 - g_iniPath), "Disciple.ini");
    else
        copyText(g_iniPath, sizeof(g_iniPath), "Disciple.ini");
}

void saveLog(const char* fmt, ...)
{
    if (!featuremenu_debug_enabled())
        return;

    char line[700];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line) - 3, _TRUNCATE, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    line[n++] = '\r';
    line[n++] = '\n';
    line[n] = 0;
    OutputDebugStringA(line);

    char logPath[MAX_PATH];
    copyText(logPath, sizeof(logPath), g_iniPath);
    char* slash = strrchr(logPath, '\\');
    if (slash) {
        char leaf[48];
        formatText(leaf, sizeof(leaf), "C4saves-%lu.log", GetCurrentProcessId());
        copyText(slash + 1, sizeof(logPath) - static_cast<size_t>(slash + 1 - logPath), leaf);
        HANDLE h = g_realCreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
            g_realCloseHandle(h);
        }
    }
}

bool endsWithI(const char* text, const char* suffix)
{
    if (!text || !suffix)
        return false;
    const size_t n = strlen(text);
    const size_t m = strlen(suffix);
    return n >= m && _stricmp(text + n - m, suffix) == 0;
}

const char* filePart(const char* path)
{
    if (!path)
        return nullptr;
    const char* a = strrchr(path, '\\');
    const char* b = strrchr(path, '/');
    const char* slash = !a ? b : (!b ? a : (a > b ? a : b));
    return slash ? slash + 1 : path;
}

bool isSavePath(const char* path)
{
    return endsWithI(path, ".sg");
}

bool isSaveFindPattern(const char* pattern)
{
    const char* name = filePart(pattern);
    return name && _stricmp(name, "*.sg") == 0;
}

bool archiveEnabled()
{
    return GetPrivateProfileIntA("Wrapper", "Archive", 1, g_iniPath) != 0;
}

bool includeSubdirectoriesEnabled()
{
    return GetPrivateProfileIntA("Wrapper", "IncludeSubdirectories", 0, g_iniPath) != 0;
}

bool joinPath(char* out, size_t cap, const char* left, const char* right)
{
    if (!left || !left[0])
        return copyText(out, cap, right);
    const size_t n = strlen(left);
    if (left[n - 1] == '\\' || left[n - 1] == '/')
        return formatText(out, cap, "%s%s", left, right);
    return formatText(out, cap, "%s\\%s", left, right);
}

bool parseQuickIndex(const char* name, unsigned* value)
{
    constexpr char prefix[] = "QuickSave";
    if (!name || _strnicmp(name, prefix, sizeof(prefix) - 1) != 0)
        return false;
    const char* p = name + sizeof(prefix) - 1;
    if (*p < '0' || *p > '9')
        return false;
    unsigned v = 0;
    do {
        if (v > 9999999U)
            return false;
        v = v * 10U + static_cast<unsigned>(*p - '0');
        ++p;
    } while (*p >= '0' && *p <= '9');
    if (_stricmp(p, ".sg") != 0)
        return false;
    *value = v;
    return true;
}

bool makeQuickSavePath(const char* original, char* out, size_t cap)
{
    char folder[kLongPath] = {};
    const char* name = filePart(original);
    if (!name)
        return false;
    if (name != original) {
        const size_t folderLen = static_cast<size_t>(name - original - 1);
        if (folderLen >= sizeof(folder))
            return false;
        memcpy(folder, original, folderLen);
        folder[folderLen] = 0;
    }

    char pattern[kLongPath];
    if (!joinPath(pattern, sizeof(pattern), folder, "QuickSave*.sg"))
        return false;

    unsigned highest = 0;
    WIN32_FIND_DATAA data{};
    HANDLE find = g_realFindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            unsigned value = 0;
            if (parseQuickIndex(data.cFileName, &value) && value > highest)
                highest = value;
        } while (g_realFindNextFileA(find, &data));
        g_realFindClose(find);
    }
    if (highest == 0xFFFFFFFFU)
        return false;

    char leaf[64];
    if (!formatText(leaf, sizeof(leaf), "QuickSave%03u.sg", highest + 1U))
        return false;
    return joinPath(out, cap, folder, leaf);
}

void fullPath(const char* path, char* out, size_t cap)
{
    const DWORD n = GetFullPathNameA(path, static_cast<DWORD>(cap), out, nullptr);
    if (!n || n >= cap)
        copyText(out, cap, path);
}

void rememberSave(HANDLE handle, const char* path, bool forceArchive)
{
    ScopedLock lock;
    int slot = -1;
    for (int i = 0; i < kMaxTrackedSaves; ++i) {
        if (g_saves[i].handle == handle) {
            slot = i;
            break;
        }
        if (slot < 0 && g_saves[i].handle == INVALID_HANDLE_VALUE)
            slot = i;
    }
    if (slot < 0)
        slot = 0;
    g_saves[slot].handle = handle;
    g_saves[slot].forceArchive = forceArchive;
    fullPath(path, g_saves[slot].path, sizeof(g_saves[slot].path));
}

bool takeSave(HANDLE handle, SaveHandle* save)
{
    ScopedLock lock;
    for (auto& item : g_saves) {
        if (item.handle == handle) {
            *save = item;
            item = SaveHandle{};
            return true;
        }
    }
    return false;
}

bool createDirectoryIfNeeded(const char* path)
{
    if (CreateDirectoryA(path, nullptr))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

unsigned readSaveMarker(const char* source)
{
    unsigned char header[0x24a] = {};
    HANDLE h = g_realCreateFileA(source, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    DWORD read = 0;
    const BOOL ok = ReadFile(h, header, sizeof(header), &read, nullptr);
    g_realCloseHandle(h);
    return ok && read == sizeof(header) ? header[sizeof(header) - 1] : 0;
}

void archiveSave(const char* source)
{
    char root[kLongPath];
    if (!copyText(root, sizeof(root), source))
        return;
    char* slashA = strrchr(root, '\\');
    char* slashB = strrchr(root, '/');
    char* slash = !slashA ? slashB : (!slashB ? slashA : (slashA > slashB ? slashA : slashB));
    if (!slash)
        return;

    char basename[MAX_PATH];
    if (!copyText(basename, sizeof(basename), slash + 1))
        return;
    *slash = 0;
    char* dot = strrchr(basename, '.');
    if (dot)
        *dot = 0;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char date[16];
    char stamp[32];
    formatText(date, sizeof(date), "%u%02u%02u", now.wYear, now.wMonth, now.wDay);
    formatText(stamp, sizeof(stamp), "%u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond);

    char archiveDir[kLongPath];
    char dayDir[kLongPath];
    if (!joinPath(archiveDir, sizeof(archiveDir), root, "Archive") ||
        !joinPath(dayDir, sizeof(dayDir), archiveDir, date) ||
        !createDirectoryIfNeeded(archiveDir) || !createDirectoryIfNeeded(dayDir)) {
        saveLog("[saves] cannot create archive directory for %s (error=%lu)", source, GetLastError());
        return;
    }

    const unsigned marker = readSaveMarker(source);
    char leaf[MAX_PATH];
    char target[kLongPath];
    formatText(leaf, sizeof(leaf), "~%s-%s-%u.sg", basename, stamp, marker);
    if (!joinPath(target, sizeof(target), dayDir, leaf))
        return;

    BOOL copied = CopyFileA(source, target, TRUE);
    if (!copied && (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)) {
        // The legacy name only has one-second precision. Keep it for the first copy, then avoid
        // dropping a second save made in the same second.
        for (unsigned collision = 1; collision < 100 && !copied; ++collision) {
            formatText(leaf, sizeof(leaf), "~%s-%s-%u-%u.sg", basename, stamp, marker, collision);
            if (!joinPath(target, sizeof(target), dayDir, leaf))
                break;
            copied = CopyFileA(source, target, TRUE);
            if (!copied && GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                break;
        }
    }

    if (copied)
        saveLog("[saves] archived %s -> %s", source, target);
    else
        saveLog("[saves] archive copy failed %s -> %s (error=%lu)", source, target, GetLastError());
}

HANDLE WINAPI hookedCreateFileA(LPCSTR fileName, DWORD access, DWORD share,
                                LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags,
                                HANDLE templateFile)
{
    const bool saveWrite = fileName && creation == CREATE_ALWAYS && (access & GENERIC_WRITE) != 0 &&
                           isSavePath(fileName);
    const bool forceArchive = saveWrite && GetKeyState(VK_SHIFT) < 0;

    char quickPath[kLongPath];
    const char* actualPath = fileName;
    if (saveWrite && GetKeyState(VK_CONTROL) < 0 &&
        makeQuickSavePath(fileName, quickPath, sizeof(quickPath))) {
        actualPath = quickPath;
        saveLog("[saves] Ctrl quick-save path: %s -> %s", fileName, actualPath);
    }

    HANDLE handle = g_realCreateFileA(actualPath, access, share, security, creation, flags, templateFile);
    if (saveWrite && handle != INVALID_HANDLE_VALUE)
        rememberSave(handle, actualPath, forceArchive);
    return handle;
}

BOOL WINAPI hookedCloseHandle(HANDLE handle)
{
    SaveHandle save;
    const bool tracked = takeSave(handle, &save);
    const BOOL result = g_realCloseHandle(handle);
    const DWORD closeError = GetLastError();

    if (tracked && result && (save.forceArchive || archiveEnabled()))
        archiveSave(save.path);

    SetLastError(closeError);
    return result;
}

FindSession* findSession(HANDLE root)
{
    for (auto& session : g_finds) {
        if (session.used && session.root == root)
            return &session;
    }
    return nullptr;
}

FindSession* allocateFindSession()
{
    for (auto& session : g_finds) {
        if (!session.used) {
            session = FindSession{};
            session.used = true;
            return &session;
        }
    }
    return nullptr;
}

void closeFindChildren(FindSession& session)
{
    for (int i = session.depth; i > 0; --i) {
        if (session.frames[i].handle != INVALID_HANDLE_VALUE)
            g_realFindClose(session.frames[i].handle);
        session.frames[i].handle = INVALID_HANDLE_VALUE;
    }
    session.depth = 0;
}

bool makeRelativeName(WIN32_FIND_DATAA* data, const char* relative)
{
    if (!relative[0])
        return true;
    char name[MAX_PATH];
    if (!joinPath(name, sizeof(name), relative, data->cFileName))
        return false;
    return copyText(data->cFileName, sizeof(data->cFileName), name);
}

bool nextSaveFile(FindSession& session, WIN32_FIND_DATAA* data, bool currentIsReady)
{
    bool ready = currentIsReady;
    for (;;) {
        FindFrame& frame = session.frames[session.depth];
        if (!ready) {
            if (!g_realFindNextFileA(frame.handle, data)) {
                if (session.depth == 0) {
                    session.exhausted = true;
                    SetLastError(ERROR_NO_MORE_FILES);
                    return false;
                }
                g_realFindClose(frame.handle);
                frame.handle = INVALID_HANDLE_VALUE;
                --session.depth;
                continue;
            }
        }
        ready = false;

        if ((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (strcmp(data->cFileName, ".") == 0 || strcmp(data->cFileName, "..") == 0 ||
                session.depth + 1 >= kMaxFindDepth)
                continue;

            char childRelative[kLongPath];
            char childDir[kLongPath];
            char childPattern[kLongPath];
            if (!joinPath(childRelative, sizeof(childRelative), frame.relative, data->cFileName) ||
                !joinPath(childDir, sizeof(childDir), session.base, childRelative) ||
                !joinPath(childPattern, sizeof(childPattern), childDir, "*.*"))
                continue;

            WIN32_FIND_DATAA childData{};
            HANDLE child = g_realFindFirstFileA(childPattern, &childData);
            if (child == INVALID_HANDLE_VALUE)
                continue;
            ++session.depth;
            FindFrame& childFrame = session.frames[session.depth];
            childFrame.handle = child;
            copyText(childFrame.relative, sizeof(childFrame.relative), childRelative);
            *data = childData;
            ready = true;
            continue;
        }

        if (endsWithI(data->cFileName, ".sg") && makeRelativeName(data, frame.relative))
            return true;
    }
}

HANDLE WINAPI hookedFindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
    // Test the pattern first so GetPrivateProfileIntA's own file activity cannot recurse here.
    if (!pattern || !data || !isSaveFindPattern(pattern) || !includeSubdirectoriesEnabled())
        return g_realFindFirstFileA(pattern, data);

    const size_t n = strlen(pattern);
    char base[kLongPath];
    if (n < 4 || n - 4 >= sizeof(base))
        return g_realFindFirstFileA(pattern, data);
    memcpy(base, pattern, n - 4); // strip "*.sg", retaining the directory separator
    base[n - 4] = 0;

    char rootPattern[kLongPath];
    if (!joinPath(rootPattern, sizeof(rootPattern), base, "*.*"))
        return g_realFindFirstFileA(pattern, data);

    WIN32_FIND_DATAA first{};
    HANDLE root = g_realFindFirstFileA(rootPattern, &first);
    if (root == INVALID_HANDLE_VALUE)
        return root;

    ScopedLock lock;
    FindSession* session = allocateFindSession();
    if (!session) {
        g_realFindClose(root);
        return g_realFindFirstFileA(pattern, data);
    }
    session->root = root;
    session->frames[0].handle = root;
    copyText(session->base, sizeof(session->base), base);

    *data = first;
    if (nextSaveFile(*session, data, true)) {
        saveLog("[saves] recursive save enumeration: %s", pattern);
        return root;
    }

    closeFindChildren(*session);
    g_realFindClose(root);
    *session = FindSession{};
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI hookedFindNextFileA(HANDLE root, LPWIN32_FIND_DATAA data)
{
    ScopedLock lock;
    FindSession* session = findSession(root);
    if (!session)
        return g_realFindNextFileA(root, data);
    if (!data || session->exhausted) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    return nextSaveFile(*session, data, false) ? TRUE : FALSE;
}

BOOL WINAPI hookedFindClose(HANDLE root)
{
    {
        ScopedLock lock;
        FindSession* session = findSession(root);
        if (session) {
            closeFindChildren(*session);
            *session = FindSession{};
        }
    }
    return g_realFindClose(root);
}

} // namespace

// Called from cnc-ddraw's DllMain. No Discipl2.exe addresses: safe for every known executable.
extern "C" void savelogic_install(void)
{
    initIniPath();
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;

    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
        error = DetourUpdateThread(GetCurrentThread());
    if (error == NO_ERROR)
        error = DetourAttach(&reinterpret_cast<PVOID&>(g_realCreateFileA), hookedCreateFileA);
    if (error == NO_ERROR)
        error = DetourAttach(&reinterpret_cast<PVOID&>(g_realCloseHandle), hookedCloseHandle);
    if (error == NO_ERROR)
        error = DetourAttach(&reinterpret_cast<PVOID&>(g_realFindFirstFileA), hookedFindFirstFileA);
    if (error == NO_ERROR)
        error = DetourAttach(&reinterpret_cast<PVOID&>(g_realFindNextFileA), hookedFindNextFileA);
    if (error == NO_ERROR)
        error = DetourAttach(&reinterpret_cast<PVOID&>(g_realFindClose), hookedFindClose);

    if (error == NO_ERROR)
        error = DetourTransactionCommit();
    else
        DetourTransactionAbort();

    if (error == NO_ERROR)
        OutputDebugStringA("[saves] quick-save/archive hooks installed (version-independent)\n");
    else
        OutputDebugStringA("[saves] failed to install quick-save/archive hooks\n");
}
