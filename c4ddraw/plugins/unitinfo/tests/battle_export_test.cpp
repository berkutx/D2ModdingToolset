// Exercises copied-text conversion and JSON export only. No native game entry point is called.
#include "../battleunitinfo.cpp"

#include <cstdlib>
#include <iostream>

namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    std::unique_ptr<NativeEncyclopedia> source(new NativeEncyclopedia());
    strcpy_s(source->name, "\\fMedBold;\xC3\xE5\xF0\xEE\xE9"); // CP1251: Герой
    strcpy_s(source->stats,
        "\\fMedBold;\xC7\xE4\xEE\xF0\xEE\xE2\xFC\xE5" // CP1251: Здоровье
        ":\\fNormal;\t\\c025;090;000;+ 15\\c000;000;000;\n");
    strcpy_s(source->statsExtra, "\\p97;Immunity\\mL0;Ward");
    strcpy_s(source->description, "Quoted \"text\"\r\nC:\\unknown;");
    strcpy_s(source->leader, "\\fMedBold;Leader:\\fNormal; + 2");
    strcpy_s(source->attack, "\\c100;000;000;- 25%\\c000;000;000;");
    source->effectCount = 2;
    strcpy_s(source->effects[0], "\\c025;090;000;"); // Formatting-only effect stays omitted.
    strcpy_s(source->effects[1], "\\c025;090;000;\xDF\xE4" ": 2"); // CP1251: Яд

    const UnitPublic unit = makePublicUnit(0x12345678, *source, 1251);
    check(unit.name == u8"Герой", "plain name is decoded CP1251 and has no font tag");
    check(unit.stats == u8"Здоровье:\t+ 15\n", "plain stats keep prior readable values");
    check(unit.formatted.stats ==
        u8"\\fMedBold;Здоровье:\\fNormal;\t\\c025;090;000;+ 15\\c000;000;000;\n",
        "formatted stats retain RGB, fonts, tab and newline after UTF-8 conversion");
    check(unit.statsExtra == "Immunity\nWard", "plain layout reset becomes a row boundary");
    check(unit.formatted.statsExtra == "\\p97;Immunity\\mL0;Ward",
        "formatted tab stop and row reset remain untouched");
    check(unit.formatted.description == "Quoted \"text\"\r\nC:\\unknown;",
        "raw quotes, CRLF and unknown markup remain untouched");
    check(unit.effects.size() == 1 && unit.formatted.effects.size() == 1,
        "plain and formatted effects use the same filtered indexes");
    check(unit.effects[0] == u8"Яд: 2" && unit.formatted.effects[0] ==
        u8"\\c025;090;000;Яд: 2", "effects preserve CP1251 text and real color tag");
    check(unit.upgrade.empty() && unit.formatted.upgrade.empty(), "missing controls stay empty");

    BattleContext context = {};
    context.area = {0, 0, 1024, 768};
    SlotSnapshot slots[12] = {};
    for (auto& slot : slots)
        slot.unitIndex = -1;
    slots[0].unitIndex = 0;
    const std::string json = buildJson(context, 1024, 768, -1, slots, {unit});
    check(json.find("305419896") == std::string::npos, "internal native unit id is not exported");
    std::cout << json;
    return 0;
}
