#include "preparedmatchtemplates.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* message) {
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
template<class F> void rejectsWith(F action, const char* expected) {
    try { action(); }
    catch (const std::runtime_error& error) { require(error.what() == std::string(expected), error.what()); return; }
    throw std::runtime_error(std::string("Expected rejection: ") + expected);
}
void testLocalTemplates() {
    using namespace hooks::prepared;
    struct FamilyFixture { const char* name; const char* family; bool version; };
    for (const auto& fixture : {
        FamilyFixture{"Fight! 1.4", "fight!", true},
        FamilyFixture{" \tFight! 1.4\r\n", "fight!", true},
        FamilyFixture{"Fight_-v1.10", "fight", true},
        FamilyFixture{"Bladerunner[Duo] 3.4.16b", "bladerunner[duo]", true},
        FamilyFixture{"Bladerunner[Trinity] 2.0", "bladerunner[trinity]", true},
        FamilyFixture{"Ultimate open-water 1.2b", "ultimate open-water", true},
        FamilyFixture{"Outrunner 3.3 beta", "outrunner", true},
        FamilyFixture{"Outrunner 3.3K(2)", "outrunner 3.3k(2)", false},
        FamilyFixture{"Outrunner_kotovasiya2", "outrunner_kotovasiya2", false},
        FamilyFixture{"Fight1.4", "fight1.4", false},
        FamilyFixture{"Fight 1.2.3.4.5.6", "fight", true},
        FamilyFixture{"Fight 1.2.3.4.5.6.7", "fight 1.2.3.4.5.6.7", false},
        FamilyFixture{"Fight 1000000000.0", "fight 1000000000.0", false},
        FamilyFixture{"Fight  1.2_rc3", "fight", true},
        FamilyFixture{"Fight 1.2 Beta", "fight 1.2 beta", false},
        FamilyFixture{"Fight 1.2 beta-2", "fight 1.2 beta", true},
        FamilyFixture{"Fight   mode", "fight   mode", false}}) {
        const auto parsed = templateFamily(fixture.name);
        require(parsed.name == fixture.family && parsed.version.has_value() == fixture.version, fixture.name);
    }
    auto older = [&](const char* a, const char* b) {
        const auto first = parseTemplateVersion(a), second = parseTemplateVersion(b);
        require(first && second && compareTemplateVersions(*first, *second) < 0, "version ordering changed");
    };
    older("1.9", "1.10"); older("1.2", "1.2a"); older("1.2a", "1.2b");
    older("1.2 alpha2", "1.2 beta"); older("1.2 beta9", "1.2 rc1"); older("1.2 rc9", "1.2");
    older("1.4a beta", "1.4"); older("1.4b beta1", "1.4a beta2");
    require(compareTemplateVersions(*parseTemplateVersion("1.2"), *parseTemplateVersion("1.2.0")) == 0,
            "zero-padded versions differ");
    require(!parseTemplateVersion("3.3K(2)") && !parseTemplateVersion("1.2 beta-2"), "unknown version grammar accepted");

    rsg::MapTemplateSettings defaults;
    defaults.maxPlayers = 2; defaults.sizeMin = 48; defaults.sizeMax = 144;
    rsg::MapTemplateSettings::TemplateCustomParameter spin;
    spin.name = "Strength"; spin.unit = "%"; spin.valueMin = -5; spin.valueMax = 5;
    spin.valueStep = 2; spin.valueDefault = -1;
    defaults.parameters.push_back(spin);
    hooks::ScenarioTemplates catalog;
    auto add = [&](std::string filename, const char* title, rsg::MapTemplateSettings settings) {
        settings.name = title;
        catalog.emplace_back(std::move(filename), std::move(settings));
        catalog.back().source = "cached-source:" + catalog.back().filename;
        catalog.back().md5 = std::string(32, 'a');
    };
    add("Templates/Fight_1.9.lua", "Fight! 1.9", defaults);
    add("Templates/Fight-1.10.lua", "Fight! 1.10", defaults);
    add("Templates/Fight_fork_99.0.lua", "Fight! 99.0", defaults);
    Offer offer; offer.title = "Fight! 1.4"; offer.filename = "Fight.lua";
    offer.md5 = std::string(32, 'b'); offer.host = "host";
    offer.participants = {{"joiner", 1, -1, 2}, {"host", 3, 1, 1}};
    offer.parameters = {{"size", 72}, {"spin:0", 3}, {"startingGold", 1700}};
    auto select = [&] { return selectLocalTemplate(offer, catalog, offer.title, offer.filename); };
    auto selected = select();
    require(selected.cached == &catalog[1] && selected.filename == "Fight-1.10.lua", "latest local file not selected");
    require(selected.cached->source == "cached-source:Templates/Fight-1.10.lua"
            && selected.cached->md5 == std::string(32, 'a') && selected.cached->md5 != offer.md5,
            "site MD5 replaced cached local identity/source");
    require(selected.settings.parametersValues[0] == 3 && selected.settings.startingGold == 1700
            && selected.settings.size == 72 && selected.settings.races[0] == rsg::RaceType::Dwarf,
            "agreed values or host race order changed");
    require(catalog[1].settings.parametersValues.empty() && catalog[1].settings.races.empty(), "startup settings mutated");
    offer.md5 = std::string(32, 'c');
    require(select().cached == &catalog[1], "new site artifact digest changed local selection");
    auto changed = defaults; changed.parameters[0].name = "Renamed parameter";
    add("Templates/Fight_2.0.lua", "Fight! 2.0", changed);
    require(select().cached == &catalog.back(), "parameter label became a schema compatibility gate");
    catalog.pop_back();
    changed = defaults; changed.maxPlayers = 3;
    add("Templates/Fight_9.0.lua", "Fight! 9.0", changed);
    rejectsWith(select, "template-player-count"); catalog.pop_back();
    changed = defaults; changed.sizeMax = 48;
    add("Templates/Fight_9.1.lua", "Fight! 9.1", changed);
    rejectsWith(select, "parameter-range:size"); catalog.pop_back();
    changed = defaults; changed.parameters[0].valueMax = 1;
    add("Templates/Fight_9.2.lua", "Fight! 9.2", changed);
    rejectsWith(select, "parameter-range:spin:0"); catalog.pop_back();
    add("Templates/Fight_v1.10.0.lua", "Fight! 1.10.0", defaults);
    rejectsWith(select, "ambiguous-template"); catalog.pop_back();
    offer.parameters["spin:0"] = 0;
    rejects(select, "invalid local spin step accepted");
    offer.parameters["spin:0"] = 3; offer.parameters["spin:1"] = 1;
    rejects(select, "unknown local spin accepted"); offer.parameters.erase("spin:1");
    offer.filename = "Missing.lua"; rejectsWith(select, "missing-template");
    offer.filename = "Fight_1.9.lua"; offer.title = "Nonconventional title without version";
    require(select().cached == &catalog[0], "nonversioned title lost exact-filename fallback");
    offer.md5 = std::string(32, 'f');
    require(select().cached == &catalog[0], "exact fallback acquired a Lua MD5 gate");
    offer.filename = "Fight.lua"; rejectsWith(select, "missing-template");

    catalog.clear();
    add("Templates/Bladerunner_duo_2.0.lua", "Bladerunner[Duo] 2.0", defaults);
    add("Templates/Bladerunner_trinity_99.0.lua", "Bladerunner[Trinity] 99.0", defaults);
    add("Templates/Bladerunner_duo_fork_99.0.lua", "Bladerunner[Duo] 99.0", defaults);
    offer.title = "Bladerunner[Duo] 1.0"; offer.filename = "Bladerunner_duo.lua";
    require(select().cached == &catalog[0], "Duo/Trinity or filename fork families merged");
    catalog.clear();
    add("Templates/Outrunner_3.3.lua", "Outrunner 3.3 beta", defaults);
    add("Templates/Outrunner_kotovasiya2.lua", "Outrunner 3.3K(2)", defaults);
    offer.title = "Outrunner 3.2"; offer.filename = "Outrunner.lua";
    require(select().cached == &catalog[0], "Kotovasiya version suffix merged into ordinary family");
    offer.title = "Outrunner 3.3K(2)"; offer.filename = "Outrunner_kotovasiya2.lua";
    require(select().cached == &catalog[1], "unknown variant version lost exact-filename fallback");
}
}
int main() {
    try {
        testLocalTemplates();
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
        std::cout << "prepared settings: local latest selection, fork isolation, no MD5/schema gate or silent downgrade, immutable snapshot, ranges and spins passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
