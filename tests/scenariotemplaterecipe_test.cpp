/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/bartonsun/D2ModdingToolset)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "scenariotemplaterecipe.h"
#include "maptemplatereader.h"
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int variable(const rsg::MapTemplate& map, const char* name)
{
    for (const auto& entry : map.contents.scenarioVariables.scenarioVariables) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    throw std::runtime_error(std::string("Missing scenario variable: ") + name);
}

void checkResult(const rsg::MapTemplate& map, const rsg::MapTemplateSettings& settings)
{
    const bool overridden = variable(map, "OPTIONAL") != 0;
    require(variable(map, "LOADS") == 1 && variable(map, "CALLS") == 1,
            "Lua globals leaked from another generation");
    require(map.contents.zones.empty() && map.contents.connections.empty(),
            "Unexpected fixture contents");
    require(map.contents.scenarioVariables.scenarioVariables.size() == (overridden ? 6u : 5u),
            "Previous scenario variables accumulated in the new contents");
    require(map.settings.startingGold == (overridden ? 999 : settings.startingGold)
                && map.settings.roads == (overridden ? 77 : settings.roads),
            "Optional content settings leaked into another generation");
    require(map.settings.name == settings.name && map.settings.description == settings.description
                && map.settings.races == settings.races && map.settings.size == settings.size
                && map.settings.maxPlayers == settings.maxPlayers
                && map.settings.startingNativeMana == settings.startingNativeMana,
            "Accepted player settings were replaced by Lua metadata");
    require(map.settings.parametersValues == settings.parametersValues
                && map.settings.parameters.size() == 2
                && map.settings.parameters.front().name == "Strength"
                && map.settings.parameters.front().value == 7
                && map.settings.parameters.back().name == "Density"
                && map.settings.parameters.back().value == 3,
            "Accepted custom spins were not preserved");
}

void checkSame(const rsg::MapTemplate& left, const rsg::MapTemplate& right)
{
    const auto& a = left.contents.scenarioVariables.scenarioVariables;
    const auto& b = right.contents.scenarioVariables.scenarioVariables;
    require(a.size() == b.size(), "Same seed produced different contents");
    for (std::size_t i = 0; i != a.size(); ++i) {
        require(a[i].name == b[i].name && a[i].value == b[i].value,
                "Same seed did not replay the Lua contents");
    }
    require(left.settings.startingGold == right.settings.startingGold
                && left.settings.roads == right.settings.roads,
            "Same seed produced different optional settings");
}

void testRecipe()
{
    // Parser-only fixture: no game installation, external Lua files or map generation.
    std::string source = R"lua(
loads = (loads or 0) + 1
local topRoll = math.random(1, 1000000)
local optional = math.random(0, 1)
template = {
    name = 'Do not reload metadata', startingGold = 1234,
    getContents = function(races, size, spins)
        calls = (calls or 0) + 1
        assert(races[1] == Race.Human and races[2] == Race.Elf)
        assert(size == 72 and spins[1] == 7 and spins[2] == 3)
        local contents = {
            zones = {}, connections = {},
            scenarioVariables = {
                {name = 'TOP', value = topRoll},
                {name = 'CONTENTS', value = math.random(1, 1000000)},
                {name = 'OPTIONAL', value = optional},
                {name = 'LOADS', value = loads},
                {name = 'CALLS', value = calls},
            },
        }
        if optional == 1 then
            contents.startingGold = 999
            contents.roads = 77
            table.insert(contents.scenarioVariables, {name = 'EXTRA', value = 1})
        end
        return contents
    end,
}
)lua";
    hooks::ScenarioTemplateRecipe recipe;
    recipe.source = source;
    source = "error('The recipe must own its accepted source')";
    recipe.settings.name = "Accepted template";
    recipe.settings.description = "Accepted description";
    recipe.settings.races = {rsg::RaceType::Human, rsg::RaceType::Elf};
    recipe.settings.maxPlayers = 2;
    recipe.settings.size = 72;
    recipe.settings.startingGold = 250;
    recipe.settings.startingNativeMana = 100;
    recipe.settings.roads = 25;
    rsg::MapTemplateSettings::TemplateCustomParameter parameter;
    parameter.name = "Strength";
    parameter.value = 7;
    recipe.settings.parameters.push_back(parameter);
    parameter.name = "Density";
    parameter.value = 3;
    recipe.settings.parameters.push_back(parameter);
    recipe.settings.parametersValues = {7, 3};
    const auto settings = recipe.settings;
    const auto pinnedSource = recipe.source;

    const auto first = hooks::instantiateScenarioTemplate(recipe, std::time_t{1});
    bool differentTop = false, differentContents = false;
    bool sawOptional = false, sawOmitted = false;
    for (std::time_t seed = 1; seed <= 64; ++seed) {
        const auto generated = hooks::instantiateScenarioTemplate(recipe, seed);
        checkResult(generated, settings);
        checkSame(generated, hooks::instantiateScenarioTemplate(recipe, seed));
        differentTop |= variable(generated, "TOP") != variable(first, "TOP");
        differentContents |= variable(generated, "CONTENTS") != variable(first, "CONTENTS");
        sawOptional |= variable(generated, "OPTIONAL") == 1;
        sawOmitted |= variable(generated, "OPTIONAL") == 0;
    }
    require(differentTop, "New seed did not reroll top-level Lua");
    require(differentContents, "New seed did not reroll getContents");
    require(sawOptional && sawOmitted, "Fixture did not cover both optional branches");
    checkSame(first, hooks::instantiateScenarioTemplate(recipe, std::time_t{1}));
    require(recipe.source == pinnedSource && recipe.settings.startingGold == settings.startingGold
                && recipe.settings.roads == settings.roads && recipe.settings.races == settings.races
                && recipe.settings.parametersValues == settings.parametersValues,
            "Generation mutated the accepted recipe");

    for (const char* badSource : {"", "invalid Lua !", "error('load failure')",
                                 "template = {}",
                                 "template = {getContents = function() error('contents failure') end}"}) {
        auto broken = recipe;
        broken.source = badSource;
        bool failed = false;
        try {
            hooks::instantiateScenarioTemplate(broken, std::time_t{1});
        } catch (const std::exception&) {
            failed = true;
        }
        require(failed, "Invalid recipe did not report its preparation error");
    }
    checkSame(first, hooks::instantiateScenarioTemplate(recipe, std::time_t{1}));
}

void testTemplateFile(const char* fileName)
{
    sol::state lua;
    rsg::bindLuaApi(lua);
    hooks::ScenarioTemplateRecipe recipe;
    recipe.settings = rsg::readTemplateSettings(fileName, lua);
    recipe.settings.races = {rsg::RaceType::Human, rsg::RaceType::Undead};
    recipe.settings.size = recipe.settings.sizeMin;
    for (auto& parameter : recipe.settings.parameters) {
        parameter.value = parameter.valueDefault;
        recipe.settings.parametersValues.push_back(parameter.value);
    }
    std::ifstream source(fileName, std::ios::binary);
    require(source.good(), "Could not read the template fixture");
    recipe.source.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
    const auto first = hooks::instantiateScenarioTemplate(recipe, std::time_t{31001});
    const auto second = hooks::instantiateScenarioTemplate(recipe, std::time_t{31002});
    const auto replay = hooks::instantiateScenarioTemplate(recipe, std::time_t{31001});
    require(!first.contents.zones.empty() && !second.contents.zones.empty(),
            "Real template did not produce zones");
    const auto counts = [](const rsg::MapTemplateContents& contents) {
        return std::make_tuple(contents.zones.size(), contents.connections.size(),
                               contents.scenarioVariables.scenarioVariables.size(),
                               contents.diplomacy.relations.size(),
                               contents.customSubraces.customSubraces.size());
    };
    require(counts(first.contents) == counts(replay.contents),
            "Repeated real template accumulated contents or did not replay the seed");
    std::cout << "Parsed template: " << fileName << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        testRecipe();
        // Optional local fixtures are only parsed; this does not generate a playable map.
        for (int i = 1; i < argc; ++i) {
            testTemplateFile(argv[i]);
        }
        std::cout << "Scenario template recipe tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scenario template recipe test failed: " << error.what() << '\n';
        return 1;
    }
}
