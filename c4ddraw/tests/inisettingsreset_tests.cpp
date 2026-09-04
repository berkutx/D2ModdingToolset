#include "../features/inisettingsreset.h"
#include "../features/wrapperdefaults.h"
#include <cstdio>

namespace {
int passed = 0;
int failed = 0;

void check(bool condition, const char* name)
{
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (condition) ++passed; else ++failed;
}

std::string fixture(const char* name)
{
    char path[MAX_PATH] = {};
    const DWORD length = GetFullPathNameA(name, MAX_PATH, path, nullptr);
    if (!length || length >= MAX_PATH) {
        std::printf("Invalid private fixture path\n");
        ExitProcess(2);
    }
    return path;
}

void put(const std::string& file, const char* section, const char* key, const char* value)
{
    if (!WritePrivateProfileStringA(section, key, value, file.c_str())) {
        std::printf("Fixture write failed, error=%lu\n", GetLastError());
        ExitProcess(2);
    }
}

c4_ini_reset::detail::Snapshot read(const std::string& file, const char* section, const char* key)
{
    c4_ini_reset::Entry entry = { file, section, key, "" };
    c4_ini_reset::detail::Snapshot snapshot;
    DWORD error = 0;
    if (!c4_ini_reset::detail::ReadSnapshot(entry, snapshot, 65536, error)) {
        std::printf("Fixture read failed, error=%lu\n", error);
        ExitProcess(2);
    }
    return snapshot;
}

struct Failure {
    unsigned calls;
    unsigned failAt;
    unsigned failAgainAt;
    bool mutateBeforeFailure;
    std::vector<std::string> keys;
    Failure(unsigned first, unsigned again = 0, bool mutate = false)
        : calls(0), failAt(first), failAgainAt(again), mutateBeforeFailure(mutate) {}
};

BOOL injected(void* context, const char* file, const char* section,
              const char* key, const char* value)
{
    Failure& failure = *static_cast<Failure*>(context);
    ++failure.calls;
    failure.keys.push_back(key);
    if (failure.calls == failure.failAt || failure.calls == failure.failAgainAt) {
        if (failure.mutateBeforeFailure)
            WritePrivateProfileStringA(section, key, value, file);
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    return WritePrivateProfileStringA(section, key, value, file);
}

void successfulReset()
{
    using namespace c4_ini_reset;
    const std::string file = fixture("success.ini");
    put(file, "ddraw", "renderer", "gdi");
    put(file, "ddraw", "aspect_ratio", "5:4");
    put(file, "Discipl2", "aspect_ratio", "16:9");
    put(file, "ddraw", "unknown", "preserve");
    put(file, "another-profile", "renderer", "d3d9");
    const std::vector<Entry> entries = {
        { file, "ddraw", "renderer", "opengl" },
        { file, "Discipl2", "renderer", "opengl" },
        { file, "ddraw", "aspect_ratio", "" },
        { file, "Discipl2", "aspect_ratio", "" }
    };
    SetLastError(0x12345678);
    const Result result = Apply(entries);
    check(GetLastError() == 0x12345678, "success preserves LastError");
    check(result.success && result.phase == Done && result.rollbackComplete, "successful multi-section reset");
    check(result.failedIndex == static_cast<size_t>(-1) && result.error == 0, "success has no failure index/error");
    check(read(file, "ddraw", "renderer").value == "opengl", "global default updated");
    check(read(file, "Discipl2", "renderer").value == "opengl", "active profile default updated");
    check(read(file, "ddraw", "unknown").value == "preserve", "unknown key preserved");
    check(read(file, "another-profile", "renderer").value == "d3d9", "other profile preserved");
    check(read(file, "ddraw", "aspect_ratio").present && read(file, "ddraw", "aspect_ratio").value.empty(), "empty global override is present");
    check(read(file, "Discipl2", "aspect_ratio").present && read(file, "Discipl2", "aspect_ratio").value.empty(), "empty active override is present");
    char effective[80] = {};
    if (!GetPrivateProfileStringA("Discipl2", "aspect_ratio", "", effective, sizeof(effective), file.c_str()))
        GetPrivateProfileStringA("ddraw", "aspect_ratio", "", effective, sizeof(effective), file.c_str());
    check(!effective[0], "clearing both sections defeats empty-profile inheritance");
}

void exactRollback()
{
    using namespace c4_ini_reset;
    const std::string first = fixture("rollback-first.ini");
    const std::string second = fixture("rollback-second.ini");
    put(first, "menu", "empty", "");
    put(first, "menu", "quoted", "\"a value\"");
    put(first, "menu", "sentinel", "\x01");
    put(second, "Wrapper", "locale", "1049");
    const auto sentinelBefore = read(first, "menu", "sentinel");
    const std::vector<Entry> entries = {
        { first, "menu", "absent", "new" },
        { first, "menu", "empty", "new" },
        { first, "menu", "quoted", "new" },
        { first, "menu", "sentinel", "new" },
        { second, "Wrapper", "locale", "1033" }
    };
    Failure failure(5, 0, true);
    SetLastError(0xABCDEF);
    const Result result = Apply(entries, &injected, &failure);
    check(GetLastError() == 0xABCDEF, "failure preserves caller LastError");
    check(!result.success && result.rollbackComplete && result.phase == Write, "failed write reports complete rollback");
    check(result.failedIndex == 4 && result.error == ERROR_DISK_FULL, "failed write preserves index and original error");
    check(failure.calls == 10, "all attempted keys rolled back including failed write");
    check(failure.keys[5] == "locale" && failure.keys[9] == "absent", "rollback is reverse order");
    check(!read(first, "menu", "absent").present, "missing key restored by deletion");
    check(read(first, "menu", "empty").present && read(first, "menu", "empty").value.empty(), "empty key not confused with absence");
    check(read(first, "menu", "quoted").value == "\"a value\"", "raw quote spelling restored");
    const auto sentinelAfter = read(first, "menu", "sentinel");
    check(sentinelAfter.present == sentinelBefore.present && sentinelAfter.value == sentinelBefore.value,
          "sentinel-like value restores original Win32-visible state");
    check(read(second, "Wrapper", "locale").value == "1049", "partial failing write in second file restored");
}

void incompleteRollback()
{
    using namespace c4_ini_reset;
    const std::string file = fixture("incomplete.ini");
    put(file, "menu", "one", "old1");
    put(file, "menu", "two", "old2");
    const std::vector<Entry> entries = { { file, "menu", "one", "new1" }, { file, "menu", "two", "new2" } };
    Failure failure(2, 3);
    const Result result = Apply(entries, &injected, &failure);
    check(!result.success && !result.rollbackComplete && result.phase == Rollback, "rollback write failure explicitly reported");
    check(result.failedIndex == 1 && result.error == ERROR_DISK_FULL, "rollback failure retains first failed reset key");
    check(failure.calls == 4 && read(file, "menu", "one").value == "old1", "rollback continues after restore error");
}

void preflight()
{
    using namespace c4_ini_reset;
    const std::string file = fixture("preflight.ini");
    put(file, "menu", "one", "old");
    const std::string longValue(1200, 'x');
    put(file, "large", "long", longValue.c_str());
    const std::vector<Entry> entries = { { file, "menu", "one", "new" }, { file, "large", "long", "short" } };
    Failure writes(0);
    const Result result = Apply(entries, &injected, &writes, 512);
    check(!result.success && result.phase == Preflight && result.error == ERROR_MORE_DATA, "truncated section rejected before writing");
    check(result.failedIndex == 1 && writes.calls == 0, "all snapshots must succeed before first write");
    check(read(file, "menu", "one").value == "old", "preflight failure leaves previous settings untouched");
    check(read(file, "large", "long").value == longValue, "bounded section reader grows without truncating raw value");
    const Result grown = Apply(entries);
    check(grown.success && read(file, "large", "long").value == "short", "default dynamic capacity handles long original value");
    check(Apply(std::vector<Entry>()).success, "empty transaction succeeds");
    check(Apply(entries, nullptr, nullptr, 3).error == ERROR_INVALID_PARAMETER, "invalid snapshot capacity rejected");
    std::vector<Entry> invalid = { { "relative.ini", "menu", "key", "x" } };
    check(Apply(invalid).error == ERROR_INVALID_PARAMETER, "relative filename cannot write Windows directory");
    invalid[0] = Entry{ file, "menu", "bad=key", "x" };
    check(Apply(invalid).error == ERROR_INVALID_PARAMETER, "invalid INI key rejected");
    invalid[0] = Entry{ file, "menu", "key", "x\ny" };
    check(Apply(invalid).error == ERROR_INVALID_PARAMETER, "multi-line value rejected");
    invalid[0] = Entry{ file, "menu", "key", std::string("x\0y", 3) };
    check(Apply(invalid).error == ERROR_INVALID_PARAMETER, "embedded NUL rejected");
}

void caseAndRepeatedKeys()
{
    using namespace c4_ini_reset;
    const std::string file = fixture("case.ini");
    put(file, "menu", "MixedKey", "original");
    const std::vector<Entry> entries = {
        { file, "MENU", "mixedKEY", "one" },
        { file, "menu", "MixedKey", "two" },
        { file, "menu", "fail", "x" }
    };
    Failure failure(3);
    const Result result = Apply(entries, &injected, &failure);
    check(!result.success && result.rollbackComplete, "case-insensitive/repeated target rollback completes");
    check(read(file, "menu", "mixedkey").value == "original", "all duplicate snapshots capture pre-transaction original");
    check(!read(file, "menu", "fail").present, "failed new key remains absent");
}

void productionDefaults()
{
    using namespace c4_ini_reset;
    const std::string ddraw = fixture("defaults-ddraw.ini");
    const std::string menu = fixture("defaults-C4menu.ini");
    const std::string game = fixture("defaults-Disciple.ini");
    const std::string plugins = fixture("untouched-C4plugins.ini");
    const std::string timer = fixture("untouched-timer.ini");
    put(plugins, "plugins", "timer", "timer.c4p");
    put(plugins, "timer", "AutoBattle", "1");
    put(timer, "timer", "Bank", "420");
    put(timer, "timer", "Paused", "1");
    put(menu, "timer", "Bank", "315");
    put(menu, "plugins", "UserPreference", "keep");
    put(menu, "menu", "UserUnknown", "keep");
    put(ddraw, "ddraw", "UserUnknown", "keep");
    put(ddraw, "Discipl2", "UserUnknown", "keep-profile");
    put(ddraw, "other-game", "renderer", "d3d9");
    put(ddraw, "other-game", "aspect_ratio", "5:4");
    put(game, "Settings", "BattleSpeed", "4");
    put(game, "Settings", "PlayerSpeed", "3");
    put(game, "Settings", "OpponentSpeed", "2");
    put(game, "Settings", "IsoBirds", "0");
    put(game, "Disciple", "ScenEditDatabase", "1");
    put(game, "Wrapper", "LegacyUnknown", "preserve");
    std::vector<Entry> entries;
    for (const auto& setting : c4defaults::renderer) {
        put(ddraw, "ddraw", setting.key, "non-default");
        put(ddraw, "Discipl2", setting.key, "non-default-profile");
        entries.push_back({ddraw, "ddraw", setting.key, setting.value});
        entries.push_back({ddraw, "Discipl2", setting.key, setting.value});
    }
    for (const auto& setting : c4defaults::menu) {
        put(menu, "menu", setting.key, "non-default");
        entries.push_back({menu, "menu", setting.key, setting.value});
    }
    entries.push_back({menu, "menu", "autoConfirmUnitHire", "1"});
    for (const auto& setting : c4defaults::wrapper) {
        put(game, "Wrapper", setting.key, "non-default");
        entries.push_back({game, "Wrapper", setting.key, setting.value});
    }
    // Mirror the caller's explicit dynamic keys, without monitor/game calls.
    char locale[24] = {};
    sprintf_s(locale, "%lu", GetUserDefaultLCID());
    entries.push_back({game, "Wrapper", "Locale", locale});
    entries.push_back({game, "Disciple", "DisplaySize", "0"});
    entries.push_back({game, "Wrapper", "GameCanvasMode", "2"});
    entries.push_back({game, "Wrapper", "GameCanvasWidth", "0"});
    entries.push_back({game, "Wrapper", "GameCanvasHeight", "0"});
    entries.push_back({game, "Wrapper", "LegacyDisplaySizeMigrated", "1"});
    std::vector<detail::Snapshot> previous;
    for (const auto& entry : entries)
        previous.push_back(read(entry.file, entry.section.c_str(), entry.key.c_str()));
    Failure lastWrite(static_cast<unsigned>(entries.size()), 0, true);
    const Result reverted = Apply(entries, &injected, &lastWrite);
    check(!reverted.success && reverted.rollbackComplete, "production-table final-write failure rolls back all files");
    bool allRestored = true;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto restored = read(entries[i].file, entries[i].section.c_str(), entries[i].key.c_str());
        if (restored.present != previous[i].present || restored.value != previous[i].value)
            allRestored = false;
    }
    check(allRestored, "every production-table old/missing value restored after injected partial failure");
    check(Apply(entries).success, "production defaults tables apply across three private INIs");
    bool defaultsMatch = true;
    for (const auto& entry : entries) {
        const auto snapshot = read(entry.file, entry.section.c_str(), entry.key.c_str());
        if (!snapshot.present || snapshot.value != entry.value) defaultsMatch = false;
    }
    check(defaultsMatch, "every production renderer/menu/wrapper and dynamic default matches");
    check(Apply(entries).success, "repeated reset succeeds");
    bool idempotent = true;
    for (const auto& entry : entries) {
        const auto snapshot = read(entry.file, entry.section.c_str(), entry.key.c_str());
        if (!snapshot.present || snapshot.value != entry.value) idempotent = false;
    }
    check(idempotent, "repeated reset is key-value idempotent");
    check(read(plugins, "plugins", "timer").value == "timer.c4p" &&
          read(plugins, "timer", "AutoBattle").value == "1", "C4plugins fixture and AutoBattle setting preserved");
    check(read(timer, "timer", "Bank").value == "420" && read(timer, "timer", "Paused").value == "1",
          "separate timer fixture bank/pause preserved");
    check(read(menu, "timer", "Bank").value == "315" && read(menu, "plugins", "UserPreference").value == "keep",
          "timer/plugin sections sharing menu INI preserved");
    check(read(menu, "menu", "UserUnknown").value == "keep" && read(ddraw, "ddraw", "UserUnknown").value == "keep" &&
          read(ddraw, "Discipl2", "UserUnknown").value == "keep-profile", "unknown keys in touched sections preserved");
    check(read(ddraw, "other-game", "renderer").value == "d3d9" && read(ddraw, "other-game", "aspect_ratio").value == "5:4",
          "non-active custom renderer profile preserved");
    check(read(game, "Settings", "BattleSpeed").value == "4" && read(game, "Settings", "PlayerSpeed").value == "3" &&
          read(game, "Settings", "OpponentSpeed").value == "2", "native gameplay speed preferences preserved");
    check(read(game, "Settings", "IsoBirds").value == "0" && read(game, "Disciple", "ScenEditDatabase").value == "1",
          "native clouds/editor preferences preserved");
    check(read(game, "Wrapper", "LegacyUnknown").value == "preserve", "unknown legacy Wrapper key preserved");
    std::printf("Production defaults exercised: %u explicit keys.\n", static_cast<unsigned>(entries.size()));
}
} // namespace

int main()
{
    successfulReset();
    exactRollback();
    incompleteRollback();
    preflight();
    caseAndRepeatedKeys();
    productionDefaults();
    std::printf("RESULT: %d passed, %d failed. Private fixture INIs only; no game/config access.\n", passed, failed);
    return failed ? 1 : 0;
}
