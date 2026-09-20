#include "../gametext.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
unsigned passed = 0;
void expect(const char* name, const std::string& input, const std::string& output)
{
    const std::string actual = twitchstat::stripGameMarkup(input.c_str());
    if (actual != output) {
        std::cerr << "FAIL " << name << "\nExpected: " << output << "\nActual: " << actual << '\n';
        std::exit(1);
    }
    ++passed;
}
} // namespace

int main()
{
    if (!twitchstat::stripGameMarkup(nullptr).empty())
        return 1;
    // Actual templates from interfaceutils.cpp and movepathhooks.cpp, with concrete values.
    expect("positive bonus RGB", R"(40 \c025;090;000;+ 15\c000;000;000;)", "40 + 15");
    expect("negative bonus RGB", R"(\c100;000;000;- 25%\c000;000;000;)", "- 25%");
    expect("outline RGB and alignment", R"(\fmedium;\hC;\vT;\c255;255;255;\o000;000;000;27)", "27");
    expect("font changes do not insert separators", R"(\fMedBold;Damage:\fNormal; 75)", "Damage: 75");
    expect("alignment and tab stops", R"(\hL;\s60;Type:\tFire\n)", "Type:\tFire\n");
    // X005TA0423 stores a layout reset, not a literal \n, between these two fields.
    expect("immunity and ward rows", R"(\fMedbold;Иммунитет:\t\fNormal;\p97;Нет\mL0;\fMedbold;Стойкость:\t\fNormal;\p97;Огонь)",
           "Иммунитет:\tНет\nСтойкость:\tОгонь");
    expect("initial margin has no leading blank row", R"(\mL3;\s40;Уровень: \t5\n)", "Уровень: \t5\n");
    expect("margin reset after existing newline", R"(Нет\n\mL0;Стойкость)", "Нет\nСтойкость");
    expect("paragraph offsets preserve real newline", R"(\p99;30\p0;\n45)", "30\n45");
    expect("literal semicolon separated numbers", "000;000; 090;000; 100/100 -25% +15", "000;000; 090;000; 100/100 -25% +15");
    expect("truncated RGB is untouched", R"(\c025;090;)", R"(\c025;090;)");
    expect("out of range RGB is untouched", R"(\c256;000;000;Text)", R"(\c256;000;000;Text)");
    expect("malformed RGB is untouched", R"(\c025;word;000;37)", R"(\c025;word;000;37)");
    expect("unknown command preserves numbers and text", R"(\qKeep 15;090;000; End)", R"(\qKeep 15;090;000; End)");
    expect("incomplete font command preserves text", R"(\fNormal Text; 75)", R"(\fNormal Text; 75)");
    expect("unverified layout command is untouched", R"(\mQ0;Word)", R"(\mQ0;Word)");
    expect("invalid integer field is untouched", R"(\p-2;42)", R"(\p-2;42)");
    expect("escaped backslash is literal", R"(A\\n B\\c025;090;000;)", R"(A\n B\c025;090;000;)");
    expect("trailing backslash", "Text\\", "Text\\");
    expect("existing breaks and whitespace survive", "  A\r\n\n\nB\rC\t  ", "  A\n\n\nB\nC\t  ");
    expect("non-ASCII bytes survive before codepage conversion", std::string("\x80\xff", 2) + R"(\c025;090;000;45)", std::string("\x80\xff", 2) + "45");
    // Every prefix is a valid bounded input. A partial directive remains literal until complete.
    const std::string color = R"(\c025;090;000;)";
    for (std::size_t length = 0; length < color.size(); ++length)
        expect("every partial RGB prefix", color.substr(0, length), color.substr(0, length));
    expect("complete RGB directive", color, "");
    std::cout << passed << " game-text parser checks passed\n";
    return 0;
}
