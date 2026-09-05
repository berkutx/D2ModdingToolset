/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2023 Vladimir Makeev.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MENURANDOMSCENARIO_H
#define MENURANDOMSCENARIO_H

#include "map.h"
#include "mapgenerator.h"
#include "maptemplate.h"
#include "menubase.h"
#include <array>
#include <ctime>
#include <string>
#include <thread>
#include <utility>

namespace game {
struct CButtonInterf;
struct CPopupDialogInterf;
} // namespace game

namespace hooks {

enum class GenerationStatus : int
{
    NotStarted,    /**< Random scenario generation has not started yet. */
    InProcess,     /**< Generation is in process, generator thread is running. */
    Canceled,      /**< Generation was canceled by player. */
    Done,          /**< Generation successfully done, scenario can be serialized. */
    LimitExceeded, /**< Generation could not succeed in specified number of attempts. */
    Error,         /**< Generation was aborted with an error. */
};

struct CMenuRandomScenario;

enum class RestartScenarioGenerationResult : int
{
    Success,
    Canceled,
    LimitExceeded,
    Error,
};

using RestartScenarioCompletion = void (*)(CMenuRandomScenario* menu,
                                           RestartScenarioGenerationResult result);

/** Base menu for random scenario generation. */
struct CMenuRandomScenario : public game::CMenuBase
{
    using StartScenario = void (*)(CMenuRandomScenario* menu);

    CMenuRandomScenario(game::CMenuPhase* menuPhase,
                        StartScenario startScenario,
                        const char* dialogName);
    ~CMenuRandomScenario();

    game::UiEvent uiEvent{};
    std::thread generatorThread;
    rsg::MapTemplate scenarioTemplate;
    std::string scenarioTemplateName;
    rsg::MapPtr scenario;
    std::unique_ptr<rsg::MapGenerator> generator;

    // Tracks which button shows which race image
    using RaceIndices = std::array<std::pair<game::CButtonInterf*, int /* image index */>, 4>;
    RaceIndices raceIndices;

    game::CPopupDialogInterf* popup{};
    GenerationStatus generationStatus{GenerationStatus::NotStarted};
    StartScenario startScenario{};
    RestartScenarioCompletion restartCompletion{};
    std::time_t generatedSeed{};
    bool cancelGeneration{false};
    bool restartGeneration{false};
};

void prepareToStartRandomScenario(CMenuRandomScenario* menu, bool networkGame = false);

/** Returns true when an accepted custom-lobby random scenario can be regenerated. */
bool hasRestartScenario();

/** Drops the retained scenario template after the real lobby room is left. */
void clearRestartScenario();

/** Arms the next restart-menu factory call with its completion callback. */
bool prepareRestartScenarioGeneration(RestartScenarioCompletion completion);

/** Starts regeneration in a freshly constructed random-scenario menu. */
bool startPreparedRestartScenarioGeneration(CMenuRandomScenario* menu);

} // namespace hooks

#endif // MENURANDOMSCENARIO_H
