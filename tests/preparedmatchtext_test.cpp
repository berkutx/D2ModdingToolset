#include "preparedmatchtext.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace hooks::prepared;
unsigned checks{};
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class Action>
void requireError(Action action, const char* expected) {
    try { action(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()) == expected, "unexpected pagination error");
        return;
    }
    require(false, "missing expected pagination error");
}
Offer diligence() {
    Offer offer;
    offer.title = "Diligence";
    offer.filename = "Diligence.lua";
    offer.host = "test1";
    offer.firstTurn = "test1";
    offer.participants = {{"test1", 0, -1, 1}, {"test2", 5, -1, 2}};
    offer.summary = "Расы: test1 — Империя, test2 — Эльфийский союз | Хост: test1; 1-й ход: test1";
    return offer;
}

// This is an injected single-byte wrapping policy, NOT the game's font metrics.
// Runtime font/rectangle fit remains the responsibility of IFormattedText.
bool wrappedFits(const std::string& text, std::size_t columns, std::size_t lines) {
    std::size_t usedLines = 1, usedColumns = 0;
    for (unsigned char value : text) {
        if (value == '\n') { ++usedLines; usedColumns = 0; }
        else {
            if (usedColumns == columns) { ++usedLines; usedColumns = 0; }
            ++usedColumns;
        }
        if (usedLines > lines) return false;
    }
    return usedLines <= lines;
}
void checkFallback(const std::string& body, const std::string& question, const std::string& next) {
    const auto fits = [](const std::string& text) { return wrappedFits(text, 37, 5); };
    const auto pages = promptPages(body, question, next, fits);
    require(pages.size() > 1, "long single-byte fixture did not paginate");
    std::string reconstructed;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        require(!pages[i].empty(), "pagination made an empty body page");
        const auto footer = promptFooter(i, pages.size(), question, next);
        require(fits(pages[i] + footer), "page with its actual displayed footer does not fit injected policy");
        const auto expectedFooter = "\n(" + std::to_string(i + 1) + "/" + std::to_string(pages.size())
            + ") " + (i + 1 == pages.size() ? question : next);
        require(footer == expectedFooter, "wrong continuation/final footer");
        reconstructed += pages[i];
    }
    require(reconstructed == body, "fallback discarded, duplicated, or reordered a body byte");
}

void formatterChecks() {
    auto offer = diligence();
    require(promptExtraTerms(offer).empty(), "canonical Diligence summary was not deduplicated");
    require(hostPromptText(offer) == "Diligence · без рейтинга\ntest1 — Империя (хост)\ntest2 — Эльфы\n1-й ход: test1",
            "ordinary two-player Diligence prompt changed or repeats conditions");
    offer.ranked = true;
    require(hostPromptText(offer).find("Diligence · рейтинг\n") == 0, "ranked flag absent");
    offer.title.clear();
    require(hostPromptText(offer).find("Diligence.lua · рейтинг\n") == 0, "empty title did not fall back to filename");

    offer = diligence();
    offer.participants = {{"test1", 0, 0, 1}, {"test2", 1, 1, 2}, {"third", 2, 2, 1}, {"fourth", 5, -1, 2}};
    offer.summary = "Расы: test1 — Империя (Маг), test2 — Орды нежити (Военачальник), third — Легионы проклятых (Повелитель воров), fourth — Эльфийский союз | Хост: test1; 1-й ход: test1";
    require(promptExtraTerms(offer).empty(), "four-player canonical race/lord summary was not recognized");
    const auto teams = hostPromptText(offer);
    require(teams.find("test1 — Империя, Маг · ком. 1 (хост)") != std::string::npos, "native lord 0 must be mage");
    require(teams.find("test2 — Нежить, Воин · ком. 2") != std::string::npos, "native lord 1 must be warrior");
    require(teams.find("third — Легионы, Вор · ком. 1") != std::string::npos, "native lord 2 must be thief/diplomat");
    require(teams.find("fourth — Эльфы · ком. 2") != std::string::npos, "fourth player/team or unspecified lord changed");
    require(std::string(promptLord(-1)).empty(), "unspecified lord was invented");
    require(std::string(promptRace(3)) == "Кланы" && std::string(promptRace(3, true)) == "Горные кланы", "clans label mismatch");
    require(std::string(promptRace(-1)) == "случайная раса" && std::string(promptRace(-1, true)) == "случайная", "random race label mismatch");

    offer = diligence();
    offer.parameters = {{"startingGold",650},{"spin:0",-3},{"spin:31",7},{"forest",13},{"startingNativeMana",450}};
    offer.explicitParameters = {"startingGold","spin:0","spin:31"};
    const auto explicitText = hostPromptText(offer);
    require(explicitText.find("\nЗолото: 650; Параметр 1: -3; Параметр 32: 7") != std::string::npos,
            "explicit author default, signed spin, order or one-based label changed");
    require(explicitText.find("Лес") == std::string::npos && explicitText.find("Мана") == std::string::npos,
            "non-explicit defaults leaked into the compact changes list");
    offer.unlockGui = true;
    require(hostPromptText(offer).find("Unlock GUI включён") != std::string::npos, "unlock GUI condition disappeared");

    for (unsigned day : {0u,7u}) {
        offer = diligence(); offer.simultaneous = true; offer.simultaneousUntil = static_cast<std::uint16_t>(day);
        const std::string expected = day ? "ОХ: объединение на день 7" : "ОХ: без объединения";
        offer.summary += " | " + expected;
        require(promptExtraTerms(offer).empty(), "canonical OH suffix was not deduplicated");
        const auto text = hostPromptText(offer);
        const auto first = text.find(expected);
        require(first != std::string::npos && text.find(expected, first + expected.size()) == std::string::npos,
                "OH rule missing or displayed twice");
    }
    offer = diligence();
    require(hostPromptText(offer).find("ОХ:") == std::string::npos, "disabled OH was invented");

    const std::string extras = "Ставки: 100 первый ход | Торги: Выбор расы: test2 — 150; Условие: third — 200 | Особое условие: не трогать нейтралов";
    offer.summary += " | " + extras;
    require(promptExtraTerms(offer) == extras, "bets, auctions or custom agreement was discarded");
    require(hostPromptText(offer).find(extras) != std::string::npos, "retained extra terms missing from final prompt");
    offer.simultaneous = true; offer.simultaneousUntil = 7;
    offer.summary += " | ОХ: объединение на день 7";
    require(promptExtraTerms(offer) == extras, "OH suffix removal damaged the preceding agreements");
    for (const std::string& unknown : std::vector<std::string>{
        "Расы: неизвестный старый формат | Хост: test1 | Торги: сохранить всё",
        diligence().summary + " изменённый хвост без разделителя",
        diligence().summary + " | ОХ: другое неизвестное значение"}) {
        offer.summary = unknown;
        if (unknown.find(diligence().summary + " | ") == 0)
            require(promptExtraTerms(offer) == "ОХ: другое неизвестное значение", "unknown OH suffix was silently dropped");
        else require(promptExtraTerms(offer) == unknown, "noncanonical summary was heuristically removed");
    }
    offer = diligence();
    const std::string unrecognized(2048, 'Z'); offer.summary = unrecognized;
    require(promptExtraTerms(offer) == unrecognized, "maximum-length unknown summary was truncated");
    require(hostPromptText(offer).find(unrecognized) != std::string::npos, "long agreement absent from rendered body");
}

void paginationChecks() {
    const std::string question = "Generate map?", next = "Yes: next; No: defer.";
    require(promptFooter(0,1,question,next) == "\nGenerate map?", "single-page footer contains a needless counter");
    require(promptFooter(0,2,question,next) == "\n(1/2) Yes: next; No: defer.", "intermediate page asks for generation");
    require(promptFooter(1,2,question,next) == "\n(2/2) Generate map?", "last page does not ask for generation");

    const std::string large(1500,'X');
    unsigned calls = 0;
    const auto one = promptPages(large,question,next,[&](const std::string& candidate){
        ++calls;
        require(candidate == large + promptFooter(0,1,question,next), "full-text single-page candidate was not tried first");
        return true;
    });
    require(calls == 1 && one.size() == 1 && one.front() == large, "1024-byte legacy cap split an otherwise fitting body");

    const std::string body = "short body", finalFooter = promptFooter(0,1,question,next);
    const auto exactLimit = body.size() + finalFooter.size();
    const auto fitsExactly = [exactLimit](const std::string& candidate){return candidate.size() <= exactLimit;};
    require(!fitsExactly(body + "\n(9999/9999) " + question), "test fixture does not distinguish actual footer from reserve");
    const auto exact = promptPages(body,question,next,fitsExactly);
    require(exact.size() == 1 && exact.front() == body, "oversized reserve forced a false second page");

    std::string encoded;
    for (unsigned i=0;i<90;++i) {
        encoded += std::string(7,static_cast<char>(0xc0 + i % 32));
        encoded += i % 4 ? ' ' : '\n';
    }
    encoded += std::string(220,static_cast<char>(0xff)); // A long unbroken single-byte word.
    encoded += " trailing spaces  \n";
    checkFallback(encoded,question,next);
    checkFallback(encoded,"A deliberately longer final confirmation question?","Next?");
    requireError([&]{promptPages("body",question,next,[](const std::string&){return false;});}, "message-box-too-small");
    requireError([&]{promptPages("body",question,next,[](const std::string& candidate){return candidate.size() <= 3;});}, "message-box-too-small");
}
}

int main() {
    try {
        formatterChecks(); paginationChecks();
        std::cout << "prepared text: " << checks << " formatting/pagination policy checks passed (not native font-fit evidence)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
