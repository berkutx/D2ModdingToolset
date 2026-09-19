/*
 * This file is part of the modding toolset for Disciples 2.
 * https://github.com/bartonsun/D2ModdingToolset
 * Licensed under GNU GPL version 3 or later; see COPYING.
 */

#ifndef SCENARIOTEMPLATERECIPE_H
#define SCENARIOTEMPLATERECIPE_H

#include "maptemplate.h"
#include <ctime>
#include <string>

namespace hooks {

/** Inputs of the accepted generation, before Lua contents can override settings.
 * The template source is retained in memory, not reread from disk during a restart.
 */
struct ScenarioTemplateRecipe
{
    rsg::MapTemplateSettings settings;
    std::string source;
};

/** Executes the recipe in a fresh Lua VM and builds fresh contents for this preview. */
rsg::MapTemplate instantiateScenarioTemplate(const ScenarioTemplateRecipe& recipe, std::time_t seed);

} // namespace hooks

#endif // SCENARIOTEMPLATERECIPE_H
