#ifndef PREPAREDMATCHSETTINGS_H
#define PREPAREDMATCHSETTINGS_H
#include "preparedmatchprotocol.h"
#include "maptemplate.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace hooks::prepared {
/** Pure validation, before executing cached Lua or mutating native UI. No disk access. */
inline rsg::MapTemplateSettings applySettings(const Offer& offer, rsg::MapTemplateSettings settings)
{
    if (offer.participants.size() != static_cast<std::size_t>(settings.maxPlayers))
        throw std::runtime_error("template-player-count");
    auto value = [&](const char* key, int fallback, int minimum, int maximum) {
        auto found = offer.parameters.find(key);
        const auto n = found == offer.parameters.end() ? fallback : found->second;
        if (n < minimum || n > maximum) throw std::runtime_error(std::string("parameter-range:") + key);
        return n;
    };
    settings.size = value("size", settings.sizeMin, settings.sizeMin, settings.sizeMax);
    if ((settings.size - settings.sizeMin) % 24) throw std::runtime_error("parameter-step:size");
    settings.roads = value("roads", settings.roads, 0, 100);
    settings.forest = value("forest", settings.forest, 0, 100);
    settings.startingGold = value("startingGold", settings.startingGold, 0, 9999);
    settings.startingNativeMana = value("startingNativeMana", settings.startingNativeMana, 0, 9999);
    settings.water = value("water", settings.water, 0, 100);
    settings.maxUnit = static_cast<std::uint8_t>(value("maxUnit", settings.maxUnit, 2, 10));
    settings.maxSpell = static_cast<std::uint8_t>(value("maxSpell", settings.maxSpell, 0, 5));
    settings.maxLeader = static_cast<std::uint8_t>(value("maxLeader", settings.maxLeader, 1, 99));
    settings.maxCity = static_cast<std::uint8_t>(value("maxCity", settings.maxCity, 1, 5));
    settings.startingLevel = static_cast<std::uint8_t>(value("startingLevel", settings.startingLevel, 1, 99));
    settings.iterations = value("iterations", settings.iterations, 0, 1000000);
    settings.parametersValues.clear();
    for (std::size_t i = 0; i < settings.parameters.size(); ++i) {
        const auto& p = settings.parameters[i]; const auto key = "spin:" + std::to_string(i);
        const int n = value(key.c_str(), p.valueDefault, p.valueMin, p.valueMax);
        if (p.valueStep <= 0 || (n - p.valueMin) % p.valueStep) throw std::runtime_error("parameter-step:" + key);
        settings.parametersValues.push_back(n);
    }
    for (const auto& p : offer.parameters) {
        if (!validParameterKey(p.first)) throw std::runtime_error("unknown-parameter");
        if (p.first.compare(0, 5, "spin:") == 0 && std::stoul(p.first.substr(5)) >= settings.parameters.size())
            throw std::runtime_error("unknown-template-spin");
    }
    const auto host = std::find_if(offer.participants.begin(), offer.participants.end(),
                                  [&](const Participant& p) { return p.name == offer.host; });
    if (host == offer.participants.end()) throw std::runtime_error("missing-host");
    settings.races.clear(); std::set<int> fixed;
    auto append = [&](const Participant& p) {
        if (!(p.race == -1 || p.race == 0 || p.race == 1 || p.race == 2 || p.race == 3 || p.race == 5))
            throw std::runtime_error("invalid-race");
        if (p.race >= 0 && !fixed.insert(p.race).second) throw std::runtime_error("duplicate-race");
        settings.races.push_back(p.race == -1 ? rsg::RaceType::Random : static_cast<rsg::RaceType>(p.race));
    };
    append(*host);
    for (const auto& p : offer.participants) if (p.name != offer.host) append(p);
    return settings;
}
} // namespace hooks::prepared
#endif
