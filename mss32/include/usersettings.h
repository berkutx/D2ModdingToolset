/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2020 Vladimir Makeev.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef USERSETTINGS_H
#define USERSETTINGS_H

#include <cstdint>
#include <string>
#include <vector>
#include <sol/sol.hpp>

namespace hooks {

struct Color
{
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
};

struct Hotkey
{
    std::uint32_t key{'I'};

    bool ctrl{false};
    bool shift{false};
    bool alt{false};
};

struct Hotkeys
{
    Hotkey openSelectedObject;
    Hotkey quickSave;
    Hotkey customPathLayer;
};

struct UnitEncyclopedia
{
    bool detailedUnitDescription;
    bool detailedAttackDescription;
    bool displayDynamicUpgradeValues;
    bool displayBonusHp;
    bool displayBonusXp;
    bool displayInfiniteAttackIndicator;
    bool displayCriticalHitTextInAttackName;
    bool updateOnShiftKeyPress;
    bool updateOnCtrlKeyPress;
    bool updateOnAltKeyPress;
};

/**
 * Controls movement cost display on the strategic map.
 */
struct MovementDisplay
{
    Color textColor{};
    Color outlineColor{};

    bool show{};
    bool showMovementAfterAction{};
};

struct Lobby
{
    struct Server
    {
        std::string ip{"104.248.139.25"};
        std::uint16_t port{61111};
    } server;

    struct Client
    {
        // 0 means auto-assign by OS
        std::uint16_t port{0};
    } client;

    struct Controls
    {
        bool ranked{true};
        bool simultaneousTurns{false};
        bool unlockGui{false};
    } controls;

    struct Defaults
    {
        bool ranked{false};
        bool simultaneousTurns{false};
        bool unlockGui{false};
        int simultaneousTurnsDays{7};
    } defaults;

    // Stores login information while the game is running,
    // not loaded from userSettings.lua.
    std::string password;
};

struct UserSettings
{
    bool showBanners;
    bool showResources;
    bool showLandConverted;

    int carryOverItemsMax;

    std::vector<std::string> customSortOrder;

    Hotkeys hotkeys;

    UnitEncyclopedia unitEncyclopedia;

    MovementDisplay movementDisplay;

    Lobby lobby;
};

const UserSettings& baseUserSettings();
const UserSettings& defaultUserSettings();

const UserSettings& userSettings();

const sol::environment* userSettingsEnvironment();

Lobby& lobbySettings();

} // namespace hooks

#endif // USERSETTINGS_H
