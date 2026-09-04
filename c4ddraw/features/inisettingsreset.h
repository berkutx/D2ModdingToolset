#pragma once

#include <windows.h>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// A bounded, best-effort transaction over explicitly listed INI keys. This is
// not a file-byte transaction or a cross-process lock. Callers must serialize
// their own configuration writers, and apply live settings only after success.
// Missing files may be created by Win32; rollback removes their new keys but
// deliberately does not delete files, sections, comments, or unknown settings.
namespace c4_ini_reset {

struct Entry {
    std::string file;
    std::string section;
    std::string key;
    std::string value;
};

enum Phase { Preflight, Write, Rollback, Done };

struct Result {
    bool success;
    bool rollbackComplete;
    size_t failedIndex;
    DWORD error;
    Phase phase;
};

// The callback must not throw. A null value deletes precisely the named key.
// It may return FALSE after changing a key: that key is also rolled back.
typedef BOOL (*WriteCallback)(void* context, const char* file,
                              const char* section, const char* key,
                              const char* value);

namespace detail {

struct LastErrorGuard {
    DWORD saved;
    LastErrorGuard() : saved(GetLastError()) {}
    ~LastErrorGuard() { SetLastError(saved); }
};

struct Snapshot {
    bool present;
    std::string value;
    Snapshot() : present(false) {}
};

inline BOOL NativeWrite(void*, const char* file, const char* section,
                        const char* key, const char* value)
{
    return WritePrivateProfileStringA(section, key, value, file);
}

inline bool HasNul(const std::string& text)
{
    return text.find('\0') != std::string::npos;
}

inline bool ValidEntry(const Entry& entry)
{
    // Avoid the Win32 relative-name fallback to the Windows directory.
    const bool absolute = entry.file.size() >= 3 &&
        ((entry.file[1] == ':' && (entry.file[2] == '\\' || entry.file[2] == '/')) ||
         (entry.file[0] == '\\' && entry.file[1] == '\\'));
    return absolute && !HasNul(entry.file) && !entry.section.empty() &&
        !HasNul(entry.section) && entry.section.find_first_of("\r\n[]") == std::string::npos &&
        !entry.key.empty() && !HasNul(entry.key) &&
        entry.key.find_first_of("\r\n=") == std::string::npos &&
        !HasNul(entry.value) && entry.value.find_first_of("\r\n") == std::string::npos;
}

inline bool ReadSnapshot(const Entry& entry, Snapshot& snapshot,
                         size_t maxSnapshotChars, DWORD& error)
{
    const DWORD attributes = GetFileAttributesA(entry.file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD attributeError = GetLastError();
        if (attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND) {
            error = attributeError;
            return false;
        }
    } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        error = ERROR_DIRECTORY;
        return false;
    }

    size_t capacity = maxSnapshotChars < 256 ? maxSnapshotChars : 256;
    for (;;) {
        std::vector<char> section(capacity, '\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetPrivateProfileSectionA(entry.section.c_str(),
            &section[0], static_cast<DWORD>(capacity), entry.file.c_str());
        const DWORD readError = GetLastError();
        if (!length && readError != ERROR_SUCCESS && readError != ERROR_FILE_NOT_FOUND &&
            readError != ERROR_PATH_NOT_FOUND) {
            error = readError;
            return false;
        }
        // Win32 signals truncation with nSize - 2. Even an exact-fit ambiguity
        // is rejected at the limit; never restore a possibly truncated value.
        if (length >= capacity - 2) {
            if (capacity == maxSnapshotChars) {
                error = ERROR_MORE_DATA;
                return false;
            }
            capacity = capacity > maxSnapshotChars / 2 ? maxSnapshotChars : capacity * 2;
            continue;
        }

        // GetPrivateProfileSection preserves raw RHS spelling (including quotes),
        // unlike GetPrivateProfileString, and distinguishes missing from empty.
        for (const char* row = &section[0]; *row; row += std::strlen(row) + 1) {
            const char* equals = std::strchr(row, '=');
            if (equals && static_cast<size_t>(equals - row) == entry.key.size() &&
                _strnicmp(row, entry.key.c_str(), entry.key.size()) == 0) {
                snapshot.present = true;
                snapshot.value.assign(equals + 1);
                return true;
            }
        }
        return true;
    }
}

} // namespace detail

inline Result Apply(const std::vector<Entry>& entries,
                    WriteCallback write = nullptr, void* context = nullptr,
                    size_t maxSnapshotChars = 65536)
{
    detail::LastErrorGuard preserve;
    Result result = { false, true, static_cast<size_t>(-1), ERROR_SUCCESS, Preflight };
    if (maxSnapshotChars < 4 || maxSnapshotChars > MAXDWORD) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    if (!write) write = &detail::NativeWrite;
    std::vector<detail::Snapshot> snapshots;
    try {
        snapshots.resize(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            result.failedIndex = i;
            if (!detail::ValidEntry(entries[i])) {
                result.error = ERROR_INVALID_PARAMETER;
                return result;
            }
            if (!detail::ReadSnapshot(entries[i], snapshots[i], maxSnapshotChars, result.error))
                return result;
        }
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        SetLastError(ERROR_SUCCESS);
        if (write(context, entry.file.c_str(), entry.section.c_str(), entry.key.c_str(),
                  entry.value.c_str())) continue;
        result.failedIndex = i;
        result.phase = Write;
        result.error = GetLastError();
        if (!result.error) result.error = ERROR_WRITE_FAULT;
        // Include the failed attempt: an injected writer (or a partial failure)
        // is allowed to have changed it before reporting failure.
        for (size_t remaining = i + 1; remaining; --remaining) {
            const size_t j = remaining - 1;
            const Entry& original = entries[j];
            const detail::Snapshot& snapshot = snapshots[j];
            if (!write(context, original.file.c_str(), original.section.c_str(),
                       original.key.c_str(), snapshot.present ? snapshot.value.c_str() : nullptr)) {
                result.rollbackComplete = false;
                result.phase = Rollback;
            }
        }
        return result;
    }
    result.success = true;
    result.failedIndex = static_cast<size_t>(-1);
    result.phase = Done;
    return result;
}

} // namespace c4_ini_reset
