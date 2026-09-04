/* Diagnostic pass-through seams for the exact Russobit Discipl2.exe.
 * ABI sources: mss32/include/{netmessages,netmsgmapentry,midcommandqueue2,
 * commandmsg,middatacache}.h. Signatures were read from the preserved alpha
 * full dump (inventory-trace-{hook-signatures,callback-signature}.txt).
 *
 * Deliberately hook CNMMap::queueMessageCallback, NOT Push: installed mss32
 * already detours Push. Calling the unchanged native callback preserves that
 * mod hook, its rejection policy and all command ordering. No code/data in a
 * mod DLL is patched, and no mod-relative address is used.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <detours.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>
#include "c4trace.h"
#include "inventorytrace.h"

#pragma intrinsic(_ReturnAddress)

namespace {

constexpr uintptr_t kBase = 0x00400000;
constexpr DWORD kExeSize = 4187648;
// SHA256 of the immutable client EXE, not of any mss32 build.
const unsigned char kExeSha256[32] = {
    0x13,0x75,0xCD,0xEF,0x09,0xEC,0x47,0x0E,0xE6,0x4F,0xE5,0x69,0x3F,0xB7,0x34,0xD7,
    0xC6,0x9F,0xB2,0x15,0x21,0x23,0x11,0xD9,0x97,0xF7,0x92,0xB2,0x58,0xA6,0x42,0xEB
};

// Identity states: untested, checking, exact, unavailable. Never wait on a
// concurrent checker; these diagnostics must not create another wait chain.
volatile LONG g_identity = 0;
volatile LONG g_installAttempted = 0;

enum : unsigned {
    Ready = 100, Unavailable = 101, Identity = 102,
    SendEnter = 110, SendLeave = 111, SendCaller = 112,
    IngressEnter = 114, IngressLeave = 115, IngressSender = 116,
    ApplyEnter = 118, ApplyLeave = 119,
    NotifyEnter = 120, NotifyLeave = 121, QueueBefore = 122
};
enum : uintptr_t {
    UnsupportedExe = 1, ModifiedSite = 2, TransactionFailed = 3,
    UnsupportedArchitecture = 4
};

using SendFn = void(__thiscall*)(void*, const std::uint32_t*, const std::uint32_t*,
                                 const std::uint32_t*, char);
using IngressFn = bool(__thiscall*)(void*, void*, std::uint32_t);
using ApplyFn = void(__thiscall*)(void*);
using NotifyFn = void(__thiscall*)(void*, void*);
SendFn g_send = nullptr;
IngressFn g_ingress = nullptr;
ApplyFn g_apply = nullptr;
NotifyFn g_notify = nullptr;

bool exactFileHash()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!pathLength || pathLength >= MAX_PATH)
        return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != kExeSize) {
        CloseHandle(file);
        return false;
    }

    // Dynamic imports avoid adding a link dependency to the renderer build.
    HMODULE bcrypt = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bcrypt) {
        CloseHandle(file);
        return false;
    }
    auto open = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(
        GetProcAddress(bcrypt, "BCryptOpenAlgorithmProvider"));
    auto property = reinterpret_cast<decltype(&BCryptGetProperty)>(
        GetProcAddress(bcrypt, "BCryptGetProperty"));
    auto create = reinterpret_cast<decltype(&BCryptCreateHash)>(
        GetProcAddress(bcrypt, "BCryptCreateHash"));
    auto update = reinterpret_cast<decltype(&BCryptHashData)>(
        GetProcAddress(bcrypt, "BCryptHashData"));
    auto finish = reinterpret_cast<decltype(&BCryptFinishHash)>(
        GetProcAddress(bcrypt, "BCryptFinishHash"));
    auto destroy = reinterpret_cast<decltype(&BCryptDestroyHash)>(
        GetProcAddress(bcrypt, "BCryptDestroyHash"));
    auto close = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(
        GetProcAddress(bcrypt, "BCryptCloseAlgorithmProvider"));
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    PUCHAR object = nullptr;
    bool matched = false;
    do {
        if (!open || !property || !create || !update || !finish || !destroy || !close)
            break;
        if (open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            break;
        ULONG objectBytes = 0, returned = 0;
        if (property(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                     sizeof(objectBytes), &returned, 0) < 0 || returned != sizeof(objectBytes) ||
            !objectBytes || objectBytes > 1024 * 1024)
            break;
        object = static_cast<PUCHAR>(HeapAlloc(GetProcessHeap(), 0, objectBytes));
        if (!object || create(algorithm, &hash, object, objectBytes, nullptr, 0, 0) < 0)
            break;
        unsigned char buffer[16384];
        DWORD total = 0;
        bool complete = true;
        for (;;) {
            DWORD bytes = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr)) {
                complete = false;
                break;
            }
            if (!bytes)
                break;
            if (bytes > kExeSize - total || update(hash, buffer, bytes, 0) < 0) {
                complete = false;
                break;
            }
            total += bytes;
        }
        unsigned char digest[32] = {};
        if (complete && total == kExeSize && finish(hash, digest, sizeof(digest), 0) >= 0)
            matched = memcmp(digest, kExeSha256, sizeof(digest)) == 0;
    } while (false);
    if (hash && destroy) destroy(hash);
    if (object) HeapFree(GetProcessHeap(), 0, object);
    if (algorithm && close) close(algorithm, 0);
    FreeLibrary(bcrypt);
    CloseHandle(file);
    return matched;
}

bool exactLoadedImage()
{
    __try {
        const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        if (reinterpret_cast<uintptr_t>(base) != kBase)
            return false; // absolute operands in this exact image are not wildcarded
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE &&
               nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
               nt->FileHeader.TimeDateStamp == 0x3FD9DBC2 &&
               nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
               nt->OptionalHeader.ImageBase == kBase &&
               nt->OptionalHeader.SizeOfImage > 0x3B7A34;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t read32(uintptr_t address)
{
    if (!address) return 0;
    __try { return *reinterpret_cast<const std::uint32_t*>(address); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

std::uint32_t field32(uintptr_t object, uintptr_t offset)
{
    // A missing/empty queue front is ordinary, not a reason to raise a caught
    // access violation in every trace sample.
    return object ? read32(object + offset) : 0;
}

std::uint32_t queueFlags(uintptr_t queue)
{
    if (!queue) return 0x80000000u;
    __try {
        const auto* bytes = reinterpret_cast<const unsigned char*>(queue);
        return bytes[0x1C] | (std::uint32_t(bytes[0x1D]) << 8) |
               (std::uint32_t(bytes[0x3C]) << 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0x80000000u; }
}

uintptr_t frontMessage(uintptr_t queue)
{
    if (!queue || !field32(queue, 0x0C)) return 0;
    const uintptr_t sentinel = field32(queue, 0x10);
    const uintptr_t first = read32(sentinel);
    return first && first != sentinel ? read32(first + 8) : 0;
}

void queueBefore(uintptr_t queue, uintptr_t message)
{
    c4trace_event(QueueBefore, queue, message, field32(queue, 0x0C),
                   queueFlags(queue), frontMessage(queue));
}

void __fastcall sendHook(void* self, void*, const std::uint32_t* from,
                         const std::uint32_t* to, const std::uint32_t* item, char flags)
{
    if (!c4trace_enabled()) { g_send(self, from, to, item, flags); return; }
    const DWORD beforeError = GetLastError();
    const uintptr_t object = reinterpret_cast<uintptr_t>(self);
    const std::uint32_t fromId = read32(reinterpret_cast<uintptr_t>(from));
    const std::uint32_t toId = read32(reinterpret_cast<uintptr_t>(to));
    const std::uint32_t itemId = read32(reinterpret_cast<uintptr_t>(item));
    c4trace_event(SendEnter, object, fromId, toId, itemId, static_cast<unsigned char>(flags));
    c4trace_event(SendCaller, object, reinterpret_cast<uintptr_t>(_ReturnAddress()),
                   reinterpret_cast<uintptr_t>(from), reinterpret_cast<uintptr_t>(to),
                   reinterpret_cast<uintptr_t>(item));
    SetLastError(beforeError);
    g_send(self, from, to, item, flags);
    const DWORD afterError = GetLastError();
    c4trace_event(SendLeave, object, fromId, toId, itemId, static_cast<unsigned char>(flags));
    SetLastError(afterError);
}

bool __fastcall ingressHook(void* map, void*, void* message, std::uint32_t sender)
{
    if (!c4trace_enabled()) return g_ingress(map, message, sender);
    const DWORD beforeError = GetLastError();
    const uintptr_t queue = read32(reinterpret_cast<uintptr_t>(map));
    const uintptr_t msg = reinterpret_cast<uintptr_t>(message);
    c4trace_event(IngressEnter, queue, msg, read32(msg), field32(msg, 4), field32(msg, 8));
    c4trace_event(IngressSender, queue, sender, reinterpret_cast<uintptr_t>(map),
                   reinterpret_cast<uintptr_t>(_ReturnAddress()), 0);
    queueBefore(queue, msg);
    SetLastError(beforeError);
    const bool result = g_ingress(map, message, sender);
    const DWORD afterError = GetLastError();
    c4trace_event(IngressLeave, queue, msg, result, field32(queue, 0x0C), queueFlags(queue));
    SetLastError(afterError);
    return result;
}

void __fastcall applyHook(void* self, void*)
{
    if (!c4trace_enabled()) { g_apply(self); return; }
    const DWORD beforeError = GetLastError();
    const uintptr_t queue = reinterpret_cast<uintptr_t>(self);
    const uintptr_t msg = frontMessage(queue);
    c4trace_event(ApplyEnter, queue, msg, read32(msg), field32(msg, 4), field32(msg, 8));
    queueBefore(queue, msg);
    SetLastError(beforeError);
    g_apply(self);
    const DWORD afterError = GetLastError();
    c4trace_event(ApplyLeave, queue, msg, field32(queue, 0x0C), queueFlags(queue), 0);
    SetLastError(afterError);
}

void __fastcall notifyHook(void* self, void*, void* changedObject)
{
    if (!c4trace_enabled()) { g_notify(self, changedObject); return; }
    const DWORD beforeError = GetLastError();
    const uintptr_t cache = reinterpret_cast<uintptr_t>(self);
    const uintptr_t object = reinterpret_cast<uintptr_t>(changedObject);
    // IMidScenarioObject {vftable, id}; no mod-private fields are read.
    const std::uint32_t id = field32(object, 4);
    const uintptr_t vtable = read32(object);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    c4trace_event(NotifyEnter, cache, object, id, vtable, caller);
    SetLastError(beforeError);
    g_notify(self, changedObject);
    const DWORD afterError = GetLastError();
    // Subscribers may replace/delete their input; use the entry-time scalar snapshot.
    c4trace_event(NotifyLeave, cache, object, id, vtable, caller);
    SetLastError(afterError);
}

const unsigned char kSendSignature[32] = {
    0xB8,0xC0,0x74,0x68,0x00,0xE8,0xD7,0x63,0x26,0x00,0x83,0xEC,0x14,0x56,0x8B,0xF1,
    0xE8,0xA8,0x08,0x00,0x00,0xFF,0x75,0x14,0x8D,0x4D,0xE0,0xFF,0x75,0x10,0xFF,0x75
};
const unsigned char kIngressSignature[16] = {
    0xFF,0x74,0x24,0x04,0x8B,0x09,0xE8,0x9C,0x04,0x00,0x00,0xB0,0x01,0xC2,0x08,0x00
};
const unsigned char kApplySignature[32] = {
    0xB8,0xAC,0x8B,0x68,0x00,0xE8,0x5F,0xCF,0x25,0x00,0x83,0xEC,0x34,0x57,0x8B,0xF9,
    0x8D,0x4D,0xDC,0x8B,0x47,0x10,0xFF,0x30,0xE8,0x01,0x02,0x15,0x00,0x8D,0x4D,0xDC
};
const unsigned char kNotifySignature[32] = {
    0x53,0x8B,0xD9,0x55,0x56,0x8B,0x43,0x0C,0x8D,0x73,0x18,0x57,0x8B,0x00,0x83,0x66,
    0x08,0x00,0x89,0x46,0x04,0x8B,0x43,0x0C,0x33,0xC9,0x39,0x4E,0x08,0x75,0x06,0x39
};

struct HookSite {
    uintptr_t address;
    const unsigned char* signature;
    size_t size;
    PVOID* original;
    PVOID replacement;
    unsigned mask;
};

bool siteMatches(const HookSite& site)
{
    __try {
        return memcmp(reinterpret_cast<const void*>(site.address), site.signature, site.size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void installHooks()
{
#if !defined(_M_IX86)
    c4trace_event(Unavailable, 0, UnsupportedArchitecture, 0, 0, 0);
#else
    HookSite sites[] = {
        {0x406FEF, kSendSignature, sizeof(kSendSignature), reinterpret_cast<PVOID*>(&g_send),
         reinterpret_cast<PVOID>(&sendHook), 1},
        {0x4102B0, kIngressSignature, sizeof(kIngressSignature), reinterpret_cast<PVOID*>(&g_ingress),
         reinterpret_cast<PVOID>(&ingressHook), 2},
        {0x410467, kApplySignature, sizeof(kApplySignature), reinterpret_cast<PVOID*>(&g_apply),
         reinterpret_cast<PVOID>(&applyHook), 4},
        {0x4179E1, kNotifySignature, sizeof(kNotifySignature), reinterpret_cast<PVOID*>(&g_notify),
         reinterpret_cast<PVOID>(&notifyHook), 8}
    };
    unsigned mask = 0;
    for (const auto& site : sites) {
        if (siteMatches(site)) {
            *site.original = reinterpret_cast<PVOID>(site.address);
            mask |= site.mask;
        } else {
            // In particular, never follow/overwrite an unknown pre-existing detour.
            c4trace_event(Unavailable, site.address, ModifiedSite, site.mask, 0, 0);
        }
    }
    if (!mask) return;
    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR) {
        c4trace_event(Unavailable, kBase, TransactionFailed, error, mask, 0);
        return;
    }
    error = DetourUpdateThread(GetCurrentThread());
    for (const auto& site : sites) {
        if (error == NO_ERROR && (mask & site.mask)) {
            // Recheck immediately before Attach; Detours must see the verified EXE
            // prologue, not chase a mod's JMP to a different module.
            if (!siteMatches(site)) error = ERROR_INVALID_DATA;
            else error = DetourAttach(site.original, site.replacement);
        }
    }
    if (error == NO_ERROR) error = DetourTransactionCommit();
    else DetourTransactionAbort();
    if (error != NO_ERROR) {
        c4trace_event(Unavailable, kBase, TransactionFailed, error, mask, 0);
        return;
    }
    c4trace_event(Ready, kBase, mask, 0x4102B0, 0x410757, 0);
#endif
}

} // namespace

extern "C" int c4_exact_game_exe(void)
{
    const DWORD savedError = GetLastError();
    LONG state = InterlockedCompareExchange(&g_identity, 1, 0);
    if (state == 0) {
        const bool exact = exactLoadedImage() && exactFileHash();
        state = exact ? 2 : 3;
        InterlockedExchange(&g_identity, state);
        c4trace_event(Identity, reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
                       exact ? 1 : 0, kExeSize, 0, 0);
    }
    SetLastError(savedError);
    return state == 2;
}

extern "C" int inventorytrace_exact_exe(void)
{
    return c4trace_enabled() ? c4_exact_game_exe() : 0;
}

extern "C" void inventorytrace_install(void)
{
    const DWORD savedError = GetLastError();
    if (!c4trace_enabled()) { SetLastError(savedError); return; }
    const bool exact = inventorytrace_exact_exe() != 0;
    if (InterlockedCompareExchange(&g_identity, 0, 0) == 1) {
        SetLastError(savedError);
        return; // a subsequent GUI dispatch may retry after the identity check
    }
    if (InterlockedCompareExchange(&g_installAttempted, 1, 0) == 0) {
        if (exact) installHooks();
        else c4trace_event(Unavailable, kBase, UnsupportedExe, 0, 0, 0);
    }
    SetLastError(savedError);
}
