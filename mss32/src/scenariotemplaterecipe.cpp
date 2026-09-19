/*
 * This file is part of the modding toolset for Disciples 2.
 * https://github.com/bartonsun/D2ModdingToolset
 * Licensed under GNU GPL version 3 or later; see COPYING.
 */

#include "scenariotemplaterecipe.h"
#include "exceptions.h"
#include "maptemplatereader.h"
#include <sol/sol.hpp>

namespace hooks {

rsg::MapTemplate instantiateScenarioTemplate(const ScenarioTemplateRecipe& recipe, std::time_t seed)
{
    if (recipe.source.empty()) {
        throw rsg::TemplateException("Scenario template source is empty or unreadable");
    }

    sol::state lua;
    rsg::bindLuaApi(lua);
    // Seed before executing the chunk: templates also make random choices at file scope.
    // Explicitly seeding avoids repeating the VM's time/address seed on a rapid Retry.
    sol::protected_function seedRandom = lua["math"]["randomseed"];
    auto seeded = seedRandom(seed);
    if (!seeded.valid()) {
        const sol::error error = seeded;
        throw rsg::TemplateException(error.what());
    }
    auto loaded = lua.safe_script(recipe.source, [](lua_State*, sol::protected_function_result result) {
        return result;
    });
    if (!loaded.valid()) {
        const sol::error error = loaded;
        throw rsg::TemplateException(error.what());
    }

    // readTemplateContents appends collections and can override settings. Neither its
    // output nor Lua globals may leak into the next preview or mutate the saved recipe.
    rsg::MapTemplate scenario;
    scenario.settings = recipe.settings;
    rsg::readTemplateContents(scenario, lua);
    return scenario;
}

} // namespace hooks
