#ifndef PREPAREDMATCHTEXT_H
#define PREPAREDMATCHTEXT_H
#include "preparedmatchprotocol.h"
#include <algorithm>
#include <stdexcept>

namespace hooks::prepared {
inline const char* promptRace(int race, bool full = false) {
    switch (race) {
    case 0: return "Империя";
    case 1: return full ? "Орды нежити" : "Нежить";
    case 2: return full ? "Легионы проклятых" : "Легионы";
    case 3: return full ? "Горные кланы" : "Кланы";
    case 5: return full ? "Эльфийский союз" : "Эльфы";
    default: return full ? "случайная" : "случайная раса";
    }
}
inline const char* promptLord(int lord, bool full = false) {
    switch (lord) {
    case 0: return "Маг";
    case 1: return full ? "Военачальник" : "Воин";
    case 2: return full ? "Повелитель воров" : "Вор";
    default: return "";
    }
}
inline std::string promptParameter(const std::string& key) {
    static const std::pair<const char*, const char*> names[] = {
        {"size", "Размер"}, {"roads", "Дороги, %"}, {"forest", "Лес, %"},
        {"startingGold", "Золото"}, {"startingNativeMana", "Мана"},
        {"water", "Вода, %"}, {"maxUnit", "Макс. уровень юнитов"},
        {"maxSpell", "Макс. круг магии"}, {"maxLeader", "Макс. уровень героя"},
        {"maxCity", "Макс. уровень города"}, {"startingLevel", "Нач. уровень"},
        {"iterations", "Попытки генерации"}
    };
    for (const auto& name : names) if (key == name.first) return name.second;
    if (key.compare(0, 5, "spin:") == 0)
        return "Параметр " + std::to_string(std::stoul(key.substr(5)) + 1);
    return key;
}
inline std::string promptSimultaneous(const Offer& offer) {
    return offer.simultaneousUntil == 0 ? "ОХ: без объединения"
        : "ОХ: объединение на день " + std::to_string(offer.simultaneousUntil);
}
inline std::string promptExtraTerms(const Offer& offer) {
    // Remove only the exact generated duplicate, never arbitrary text beginning
    // with a familiar label. Unknown/older summaries and all bets remain intact.
    std::string duplicate = "Расы: ";
    for (std::size_t i = 0; i < offer.participants.size(); ++i) {
        const auto& p = offer.participants[i];
        duplicate += (i ? ", " : "") + p.name + " — " + promptRace(p.race, true);
        if (p.lord >= 0) duplicate += " (" + std::string(promptLord(p.lord, true)) + ")";
    }
    duplicate += " | Хост: " + offer.host;
    if (!offer.firstTurn.empty()) duplicate += "; 1-й ход: " + offer.firstTurn;
    std::string extra = offer.summary;
    if (extra == duplicate) return {};
    if (extra.compare(0, duplicate.size() + 3, duplicate + " | ") != 0) return extra;
    extra.erase(0, duplicate.size() + 3);
    if (offer.simultaneous) {
        const auto simultaneous = promptSimultaneous(offer);
        if (extra == simultaneous) return {};
        const auto suffix = " | " + simultaneous;
        if (extra.size() >= suffix.size()
            && extra.compare(extra.size() - suffix.size(), suffix.size(), suffix) == 0)
            extra.erase(extra.size() - suffix.size());
    }
    return extra;
}
inline std::string hostPromptText(const Offer& offer) {
    std::string text = (offer.title.empty() ? offer.filename : offer.title)
        + (offer.ranked ? " · рейтинг" : " · без рейтинга");
    for (const auto& p : offer.participants) {
        text += "\n" + p.name + " — " + promptRace(p.race);
        if (p.lord >= 0) text += ", " + std::string(promptLord(p.lord));
        if (offer.participants.size() > 2) text += " · ком. " + std::to_string(p.team);
        if (p.name == offer.host) text += " (хост)";
    }
    if (!offer.firstTurn.empty()) text += "\n1-й ход: " + offer.firstTurn;
    if (offer.simultaneous) text += "\n" + promptSimultaneous(offer);
    if (offer.unlockGui) text += "\nUnlock GUI включён";
    for (std::size_t i = 0; i < offer.explicitParameters.size(); ++i) {
        const auto& key = offer.explicitParameters[i];
        text += (i ? "; " : "\n") + promptParameter(key) + ": " + std::to_string(offer.parameters.at(key));
    }
    const auto extra = promptExtraTerms(offer);
    if (!extra.empty()) text += "\n" + extra;
    return text;
}
inline std::string promptFooter(std::size_t page, std::size_t count,
    const std::string& question, const std::string& next) {
    if (count == 1) return "\n" + question;
    return "\n(" + std::to_string(page + 1) + "/" + std::to_string(count) + ") "
        + (page + 1 == count ? question : next);
}
// Text and footers use the renderer's single-byte encoding. The native caller
// supplies actual textbox/font measurement; tests inject only the fit policy.
template<class Fits>
std::vector<std::string> promptPages(std::string text, const std::string& question,
    const std::string& next, Fits fits) {
    if (fits(text + promptFooter(0, 1, question, next))) return {text};
    const auto reserve = "\n(9999/9999) ";
    auto fitsPage = [&](const std::string& body) {
        return fits(body + reserve + question) && fits(body + reserve + next);
    };
    std::vector<std::string> pages;
    while (!text.empty()) {
        std::size_t n{}, upper = text.size();
        while (n < upper) {
            const auto middle = n + (upper - n + 1) / 2;
            if (fitsPage(text.substr(0, middle))) n = middle;
            else upper = middle - 1;
        }
        if (!n) throw std::runtime_error("message-box-too-small");
        if (n < text.size()) {
            const auto split = text.find_last_of(" \n", n - 1);
            if (split != std::string::npos && split > n / 2) n = split + 1;
        }
        pages.push_back(text.substr(0, n)); text.erase(0, n);
        if (pages.size() > 9999) throw std::runtime_error("message-box-too-many-pages");
    }
    if (pages.empty()) throw std::runtime_error("message-box-too-small");
    return pages;
}
} // namespace hooks::prepared
#endif
