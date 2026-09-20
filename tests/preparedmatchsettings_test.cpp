#include "preparedmatchsettings.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* message) {
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
}
int main() {
    try {
        using namespace hooks::prepared;
        rsg::MapTemplateSettings defaults;
        defaults.maxPlayers = 2; defaults.sizeMin = 48; defaults.sizeMax = 144;
        defaults.startingGold = 1250; defaults.waterType = rsg::WaterType::Lakes;
        rsg::MapTemplateSettings::TemplateCustomParameter spin;
        spin.valueMin = -5; spin.valueMax = 5; spin.valueStep = 2; spin.valueDefault = -1;
        defaults.parameters.push_back(spin);
        Offer offer; offer.host = "host";
        offer.participants = {{"joiner", 1, -1, 2}, {"host", 3, 1, 1}};
        auto settings = applySettings(offer, defaults);
        require(settings.races[0] == rsg::RaceType::Dwarf && settings.races[1] == rsg::RaceType::Undead,
                "native race mapping or host order changed");
        require(settings.startingGold == 1250 && settings.waterType == defaults.waterType
            && settings.parametersValues[0] == -1, "cached defaults were lost");
        require(defaults.races.empty() && defaults.parametersValues.empty(), "startup defaults mutated");
        offer.parameters = {{"size", 96}, {"spin:0", 3}, {"startingGold", 9999}, {"iterations", 1000000}};
        settings = applySettings(offer, defaults);
        require(settings.size == 96 && settings.parametersValues[0] == 3 && settings.iterations == 1000000,
                "explicit parameters were ignored");
        offer.participants[1].race = -1;
        require(applySettings(offer, defaults).races[0] == rsg::RaceType::Random, "wire random not converted");
        offer.parameters["size"] = 50;
        rejects([&] { applySettings(offer, defaults); }, "invalid map-size step accepted");
        offer.parameters["size"] = 96; offer.parameters["spin:0"] = 0;
        rejects([&] { applySettings(offer, defaults); }, "invalid custom step accepted");
        offer.parameters["spin:0"] = 3; offer.parameters["spin:1"] = 0;
        rejects([&] { applySettings(offer, defaults); }, "unknown template spin accepted");
        offer.parameters.erase("spin:1"); offer.parameters["startingGold"] = 10000;
        rejects([&] { applySettings(offer, defaults); }, "gold range accepted");
        offer.parameters["startingGold"] = 9999; offer.participants[1].race = 1;
        rejects([&] { applySettings(offer, defaults); }, "duplicate fixed race accepted");
        offer.participants[1].race = 4;
        rejects([&] { applySettings(offer, defaults); }, "neutral playable race accepted");
        offer.participants.pop_back();
        rejects([&] { applySettings(offer, defaults); }, "player count mismatch accepted");
        std::cout << "prepared settings: defaults, immutable snapshot, exact races, ranges and spins passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
