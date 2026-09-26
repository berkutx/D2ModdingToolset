// Exercise the actual potion-field reader without loading the game. Unused
// constructor hooks are discarded by /Gy + /OPT:REF; no source copy is patched.
#include "../mss32/src/itempotionhooks.cpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace game {
struct CDBTable
{
    const char* modifier;
    bool missingField;
    bool invalidId;
};
const CMidgardID invalidId{0x003f0000};
} // namespace game

namespace {
static_assert(sizeof(void*) == 4, "Use the MSVC x86 developer environment");
struct NativeError { enum Kind { None, InvalidId, MissingField } kind; };
struct Observations
{
    int integerReads = 0;
    int stringReads = 0;
    int idReads = 0;
    int frees = 0;
} observed;
constexpr game::CMidgardID validModifier{0x00080001};

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void __stdcall readInt(int* value, const game::CDBTable*, const char* field,
                       int minimum, int maximum)
{
    ++observed.integerReads;
    if (std::strcmp(field, "ITEM_CAT") == 0) {
        require(minimum == 0 && maximum == 14, "ITEM_CAT bounds changed");
        *value = 7;
    } else {
        require(std::strcmp(field, "HP_POTION") == 0, "unexpected integer field");
        require(minimum == std::numeric_limits<int>::min()
                    && maximum == std::numeric_limits<int>::max(),
                "HP_POTION bounds changed");
        *value = 125;
    }
}

void __stdcall readString(game::String* value, const game::CDBTable* table,
                          const char* field)
{
    ++observed.stringReads;
    require(std::strcmp(field, "MOD_POTION") == 0, "unexpected string field");
    if (table->missingField)
        throw NativeError{NativeError::MissingField};
    if (!table->modifier)
        return; // A native empty String may have no allocation.
    value->length = static_cast<std::uint32_t>(std::strlen(table->modifier));
    value->lengthAllocated = value->length + 1;
    value->string = new char[value->lengthAllocated];
    std::memcpy(value->string, table->modifier, value->lengthAllocated);
}

// On x86 this fastcall shim consumes the native thiscall ECX argument; the
// unused EDX argument needs no stack slot and the native Free has no arguments.
int __fastcall freeString(game::String* value, int)
{
    ++observed.frees;
    delete[] value->string;
    *value = {};
    return 0;
}

void __stdcall readId(game::CMidgardID* value, const game::CDBTable* table,
                      const char* field)
{
    ++observed.idReads;
    require(std::strcmp(field, "MOD_POTION") == 0, "unexpected ID field");
    require(observed.frees == 1, "native String must be freed before strict readId");
    if (table->invalidId)
        throw NativeError{NativeError::InvalidId};
    *value = validModifier;
}
} // namespace

namespace game::CDBTableApi {
Api& get()
{
    static Api api{};
    api.readIntWithBoundsCheck = &readInt;
    api.readString = &readString;
    api.readId = &readId;
    return api;
}
} // namespace game::CDBTableApi

namespace game::StringApi {
Api& get()
{
    static Api api{};
    api.free = reinterpret_cast<Api::Free>(&freeString);
    return api;
}
} // namespace game::StringApi

// Resolve API references from the unused constructor hooks; calling either is
// a test failure, never a substitute game allocator or constructor.
namespace game::Memory {
Api& get() { throw std::runtime_error("potion constructor entered game allocator"); }
} // namespace game::Memory
namespace hooks {
OriginalFunctions& getOriginalFunctions()
{
    throw std::runtime_error("potion constructor entered native trampoline");
}
} // namespace hooks

int main()
{
    struct Case
    {
        const char* name;
        game::CDBTable table;
        bool expectIdRead;
        NativeError::Kind error;
    };
    const Case cases[] = {
        {"null", {nullptr, false, false}, false, NativeError::None},
        {"empty", {"", false, false}, false, NativeError::None},
        {"DBF space padding", {"          ", false, false}, false, NativeError::None},
        {"valid ID", {"G000UM0001", false, false}, true, NativeError::None},
        {"padded nonblank uses strict reader", {"  G000UM0001 ", false, false}, true,
         NativeError::None},
        {"invalid nonblank propagates", {"INVALID", false, true}, true, NativeError::InvalidId},
        {"tab is not DBF blank", {" \t ", false, true}, true, NativeError::InvalidId},
        {"missing field propagates", {nullptr, true, false}, false, NativeError::MissingField},
    };
    const char* current = "initialization";
    try {
        for (const auto& test : cases) {
            current = test.name;
            observed = {};
            auto table = test.table;
            int itemCategory = -9;
            int hitPoints = -9;
            game::CMidgardID modifier{-9};
            auto error = NativeError::None;
            try {
                hooks::readPotionExtraFields(&itemCategory, &hitPoints, &modifier, &table);
            } catch (const NativeError& e) {
                error = e.kind;
            }
            require(error == test.error, "native error was swallowed or replaced");
            require(observed.integerReads == 2, "required fields were not read once");
            require(observed.stringReads == 1, "MOD_POTION must be inspected once");
            require(observed.idReads == (test.expectIdRead ? 1 : 0), "wrong strict-reader count");
            require(observed.frees == (table.missingField ? 0 : 1), "wrong String free count");
            if (error == NativeError::None) {
                require(itemCategory == 7 && hitPoints == 125, "required values changed");
                require(modifier == (test.expectIdRead ? validModifier : game::invalidId),
                        "wrong optional modifier value");
            } else {
                require(itemCategory == -9 && hitPoints == -9 && modifier.value == -9,
                        "failed read published partial outputs");
            }
            std::printf("PASS %s\n", current);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s: %s\n", current, e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "FAIL %s: unexpected exception\n", current);
        return 1;
    }
#ifdef _DEBUG
    std::puts("Potion field regressions: 8/8 (Debug)");
#else
    std::puts("Potion field regressions: 8/8 (Release)");
#endif
    return 0;
}
