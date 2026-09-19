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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "usersettings.h"

#include "scripts.h"
#include "utils.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace hooks {

template <typename T>
static T readSetting(const sol::table& table,
                     const char* name,
                     T def,
                     T min = std::numeric_limits<T>::min(),
                     T max = std::numeric_limits<T>::max())
{
    return std::clamp<T>(table.get_or(name, def), min, max);
}

static std::string readSetting(const sol::table& table, const char* name, const std::string& def)
{
    return table.get_or(name, def);
}

static Color readColor(const sol::table& table, const Color& def)
{
    Color color{};
    color.r = readSetting(table, "red", def.r);
    color.g = readSetting(table, "green", def.g);
    color.b = readSetting(table, "blue", def.b);

    return color;
}

static void readCustomSortSettings(const sol::table& table, UserSettings& settings)
{
    settings.customSortOrder.clear();

    auto arr = table.get<sol::optional<sol::table>>("customSortOrder");
    if (!arr.has_value())
        return;

    for (auto& kv : arr.value())
        settings.customSortOrder.push_back(kv.second.as<std::string>());
}

static std::uint32_t parseKeyCode(std::string key)
{
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (key.empty())
        return 0;

    if (key.size() == 1)
        return key[0];

    static const std::unordered_map<std::string, std::uint32_t> keys = {
        {"TAB", VK_TAB},           {"SPACE", VK_SPACE},     {"ENTER", VK_RETURN},
        {"RETURN", VK_RETURN},     {"ESC", VK_ESCAPE},      {"ESCAPE", VK_ESCAPE},
        {"BACKSPACE", VK_BACK},    {"DELETE", VK_DELETE},   {"INSERT", VK_INSERT},
        {"HOME", VK_HOME},         {"END", VK_END},         {"PAGEUP", VK_PRIOR},
        {"PAGEDOWN", VK_NEXT},

        {"LEFT", VK_LEFT},         {"RIGHT", VK_RIGHT},     {"UP", VK_UP},
        {"DOWN", VK_DOWN},

        {"CTRL", VK_CONTROL},      {"SHIFT", VK_SHIFT},     {"ALT", VK_MENU},

        {"NUM0", VK_NUMPAD0},      {"NUM1", VK_NUMPAD1},    {"NUM2", VK_NUMPAD2},
        {"NUM3", VK_NUMPAD3},      {"NUM4", VK_NUMPAD4},    {"NUM5", VK_NUMPAD5},
        {"NUM6", VK_NUMPAD6},      {"NUM7", VK_NUMPAD7},    {"NUM8", VK_NUMPAD8},
        {"NUM9", VK_NUMPAD9},

        {"PLUS", VK_OEM_PLUS},     {"MINUS", VK_OEM_MINUS}, {"COMMA", VK_OEM_COMMA},
        {"PERIOD", VK_OEM_PERIOD},
    };

    auto it = keys.find(key);
    if (it != keys.end())
        return it->second;

    if (key[0] == 'F') {
        int number = std::atoi(key.c_str() + 1);
        if (number >= 1 && number <= 24)
            return VK_F1 + number - 1;
    }

    return 0;
}

static std::string keyCodeToString(std::uint32_t key)
{
    if (key >= 'A' && key <= 'Z')
        return std::string(1, static_cast<char>(key));

    if (key >= '0' && key <= '9')
        return std::string(1, static_cast<char>(key));

    if (key >= VK_F1 && key <= VK_F24)
        return "F" + std::to_string(key - VK_F1 + 1);

    switch (key) {
    case VK_TAB:
        return "TAB";
    case VK_SPACE:
        return "SPACE";
    case VK_RETURN:
        return "ENTER";
    case VK_ESCAPE:
        return "ESC";
    case VK_BACK:
        return "BACKSPACE";
    case VK_DELETE:
        return "DELETE";
    case VK_INSERT:
        return "INSERT";
    case VK_HOME:
        return "HOME";
    case VK_END:
        return "END";
    case VK_PRIOR:
        return "PAGEUP";
    case VK_NEXT:
        return "PAGEDOWN";

    case VK_LEFT:
        return "LEFT";
    case VK_RIGHT:
        return "RIGHT";
    case VK_UP:
        return "UP";
    case VK_DOWN:
        return "DOWN";

    case VK_NUMPAD0:
        return "NUM0";
    case VK_NUMPAD1:
        return "NUM1";
    case VK_NUMPAD2:
        return "NUM2";
    case VK_NUMPAD3:
        return "NUM3";
    case VK_NUMPAD4:
        return "NUM4";
    case VK_NUMPAD5:
        return "NUM5";
    case VK_NUMPAD6:
        return "NUM6";
    case VK_NUMPAD7:
        return "NUM7";
    case VK_NUMPAD8:
        return "NUM8";
    case VK_NUMPAD9:
        return "NUM9";

    case VK_OEM_PLUS:
        return "PLUS";
    case VK_OEM_MINUS:
        return "MINUS";
    case VK_OEM_COMMA:
        return "COMMA";
    case VK_OEM_PERIOD:
        return "PERIOD";
    }

    return "";
}
static void readHotkey(const sol::table& table, const char* name, Hotkey& value, const Hotkey& def)
{
    auto hotkey = table.get<sol::optional<sol::table>>(name);
    if (!hotkey.has_value()) {
        value = def;
        return;
    }

    value.key = parseKeyCode(readSetting(hotkey.value(), "key", keyCodeToString(def.key)));

    value.ctrl = readSetting(hotkey.value(), "ctrl", def.ctrl);
    value.shift = readSetting(hotkey.value(), "shift", def.shift);
    value.alt = readSetting(hotkey.value(), "alt", def.alt);
}

static void readHotkeySettings(const sol::table& table, Hotkeys& value)
{
    const auto& def = defaultUserSettings().hotkeys;

    value = def;

    auto category = table.get<sol::optional<sol::table>>("hotkeys");
    if (!category.has_value())
        return;

    readHotkey(category.value(), "openSelectedObject", value.openSelectedObject,
               def.openSelectedObject);

    readHotkey(category.value(), "quickSave", value.quickSave, def.quickSave);

    readHotkey(category.value(), "customPathLayer", value.customPathLayer, def.customPathLayer);
}

static void readUnitEncyclopediaSettings(const sol::table& table, UnitEncyclopedia& value)
{
    const auto& def = defaultUserSettings().unitEncyclopedia;

    auto category = table.get<sol::optional<sol::table>>("unitEncyclopedia");
    if (!category.has_value()) {
        value = def;

        // Backward compatibility
        value.detailedAttackDescription = readSetting(table, "detailedAttackDescription",
                                                      def.detailedAttackDescription);
        return;
    }

    value.detailedUnitDescription = readSetting(category.value(), "detailedUnitDescription",
                                                def.detailedUnitDescription);

    value.detailedAttackDescription = readSetting(category.value(), "detailedAttackDescription",
                                                  def.detailedAttackDescription);

    value.displayDynamicUpgradeValues = readSetting(category.value(), "displayDynamicUpgradeValues",
                                                    def.displayDynamicUpgradeValues);

    value.displayBonusHp = readSetting(category.value(), "displayBonusHp", def.displayBonusHp);

    value.displayBonusXp = readSetting(category.value(), "displayBonusXp", def.displayBonusXp);

    value.displayInfiniteAttackIndicator = readSetting(category.value(),
                                                       "displayInfiniteAttackIndicator",
                                                       def.displayInfiniteAttackIndicator);

    value.displayCriticalHitTextInAttackName = readSetting(category.value(),
                                                           "displayCriticalHitTextInAttackName",
                                                           def.displayCriticalHitTextInAttackName);

    value.updateOnShiftKeyPress = readSetting(category.value(), "updateOnShiftKeyPress",
                                              def.updateOnShiftKeyPress);

    value.updateOnCtrlKeyPress = readSetting(category.value(), "updateOnCtrlKeyPress",
                                             def.updateOnCtrlKeyPress);

    value.updateOnAltKeyPress = readSetting(category.value(), "updateOnAltKeyPress",
                                            def.updateOnAltKeyPress);

}
static void readMovementDisplaySettings(const sol::table& table, MovementDisplay& value)
{
    const auto& def = defaultUserSettings().movementDisplay;

    value = def;

    auto movement = table.get<sol::optional<sol::table>>("movementCost");
    if (!movement.has_value())
        return;

    value.show = readSetting(movement.value(), "show", def.show);

    value.showMovementAfterAction = readSetting(movement.value(), "showMovementAfterAction",
                                                def.showMovementAfterAction);

    auto textColor = movement.value().get<sol::optional<sol::table>>("textColor");

    if (textColor.has_value())
        value.textColor = readColor(textColor.value(), def.textColor);

    auto outlineColor = movement.value().get<sol::optional<sol::table>>("outlineColor");

    if (outlineColor.has_value())
        value.outlineColor = readColor(outlineColor.value(), def.outlineColor);
}

static void readLobbySettings(const sol::table& table, Lobby& value)
{
    const auto& def = defaultUserSettings().lobby;

    value.server.ip = def.server.ip;
    value.server.port = def.server.port;
    value.client.port = def.client.port;
    value.controls = def.controls;
    value.defaults = def.defaults;

    auto lobby = table.get<sol::optional<sol::table>>("lobby");
    if (!lobby.has_value())
        return;

    auto server = lobby.value().get<sol::optional<sol::table>>("server");

    if (server.has_value()) {
        value.server.ip = readSetting(server.value(), "ip", def.server.ip);

        value.server.port = readSetting(server.value(), "port", def.server.port);
    }

    auto client = lobby.value().get<sol::optional<sol::table>>("client");

    if (client.has_value()) {
        value.client.port = readSetting(client.value(), "port", def.client.port);
    }

    auto controls = lobby.value().get<sol::optional<sol::table>>("controls");

    if (controls.has_value()) {
        value.controls.ranked = readSetting(controls.value(), "ranked", def.controls.ranked);
        value.controls.simultaneousTurns = readSetting(controls.value(), "simultaneousTurns",
                                                       def.controls.simultaneousTurns);
        value.controls.unlockGui = readSetting(controls.value(), "unlockGui", def.controls.unlockGui);
    }

    auto defaults = lobby.value().get<sol::optional<sol::table>>("defaults");
    if (defaults.has_value()) {
        value.defaults.ranked = readSetting(defaults.value(), "ranked", def.defaults.ranked);
        value.defaults.simultaneousTurns = readSetting(defaults.value(), "simultaneousTurns",
                                                       def.defaults.simultaneousTurns);
        value.defaults.unlockGui = readSetting(defaults.value(), "unlockGui", def.defaults.unlockGui);
        value.defaults.simultaneousTurnsDays = std::clamp(
            readSetting(defaults.value(), "simultaneousTurnsDays", def.defaults.simultaneousTurnsDays),
            0, 30);
    }
}
static void readUserSettings(const sol::table& table, UserSettings& settings)
{
    settings.showBanners = readSetting(table, "showBanners", defaultUserSettings().showBanners);

    settings.showResources = readSetting(table, "showResources",
                                         defaultUserSettings().showResources);

    settings.showLandConverted = readSetting(table, "showLandConverted",
                                             defaultUserSettings().showLandConverted);
    settings.carryOverItemsMax = readSetting(table, "carryOverItemsMax",
                                             defaultUserSettings().carryOverItemsMax, 0);

    readCustomSortSettings(table, settings);
    readHotkeySettings(table, settings.hotkeys);
    readUnitEncyclopediaSettings(table, settings.unitEncyclopedia);
    readMovementDisplaySettings(table, settings.movementDisplay);
    readLobbySettings(table, settings.lobby);
}

const UserSettings& baseUserSettings()
{
    static UserSettings settings;
    static bool initialized = false;

    if (!initialized) {

        settings.showBanners = false;
        settings.showResources = false;
        settings.showLandConverted = false;

        settings.carryOverItemsMax = 5;

        settings.unitEncyclopedia.detailedUnitDescription = false;
        settings.unitEncyclopedia.detailedAttackDescription = false;
        settings.unitEncyclopedia.displayDynamicUpgradeValues = false;
        settings.unitEncyclopedia.displayBonusHp = false;
        settings.unitEncyclopedia.displayBonusXp = false;
        settings.unitEncyclopedia.displayInfiniteAttackIndicator = false;
        settings.unitEncyclopedia.displayCriticalHitTextInAttackName = false;
        settings.unitEncyclopedia.updateOnShiftKeyPress = false;
        settings.unitEncyclopedia.updateOnCtrlKeyPress = false;
        settings.unitEncyclopedia.updateOnAltKeyPress = false;

        settings.movementDisplay.show = false;
        settings.movementDisplay.showMovementAfterAction = false;
        settings.movementDisplay.textColor = {200, 200, 200};
        settings.movementDisplay.outlineColor = {0, 0, 0};

        settings.hotkeys.openSelectedObject.key = 'I';
        settings.hotkeys.openSelectedObject.ctrl = false;
        settings.hotkeys.openSelectedObject.shift = false;
        settings.hotkeys.openSelectedObject.alt = false;

        settings.hotkeys.quickSave.key = 'Q';
        settings.hotkeys.quickSave.ctrl = true;
        settings.hotkeys.quickSave.shift = false;
        settings.hotkeys.quickSave.alt = false;

        settings.hotkeys.customPathLayer.key = VK_SHIFT;
        settings.hotkeys.customPathLayer.ctrl = false;
        settings.hotkeys.customPathLayer.shift = false;
        settings.hotkeys.customPathLayer.alt = false;

        initialized = true;
    }

    return settings;
}

const UserSettings& defaultUserSettings()
{
    static UserSettings settings;
    static bool initialized = false;

    if (!initialized) {

        
        const auto& base = baseUserSettings();
        settings = base;

        settings.showBanners = true;
        settings.showResources = true;

        settings.unitEncyclopedia.detailedUnitDescription = true;
        settings.unitEncyclopedia.detailedAttackDescription = true;

        settings.movementDisplay.show = true;

        initialized = true;
    }

    return settings;
}



static std::optional<sol::environment> settingsEnvironment;

const sol::environment* userSettingsEnvironment()
{
    return settingsEnvironment ? &*settingsEnvironment : nullptr;
}

static void initializeUserSettings(UserSettings& value)
{
    value = defaultUserSettings();

    const auto path{scriptsFolder() / "userSettings.lua"};

    try {

        settingsEnvironment = executeUserSettingsFile(path);

        if (settingsEnvironment) {
            const sol::table& table = (*settingsEnvironment)["settings"];
            readUserSettings(table, value);
        }

    } catch (const std::exception& e) {

        showErrorMessageBox(fmt::format("Failed to read script '{:s}'.\n"
                                        "Reason: '{:s}'",
                                        path.string(), e.what()));
    }
}

static UserSettings& getUserSettings()
{
    static UserSettings settings;
    static bool initialized = false;

    if (!initialized) {
        initializeUserSettings(settings);
        initialized = true;
    }

    return settings;
}

const UserSettings& userSettings()
{
    return getUserSettings();
}

Lobby& lobbySettings()
{
    return getUserSettings().lobby;
}



} // namespace hooks
