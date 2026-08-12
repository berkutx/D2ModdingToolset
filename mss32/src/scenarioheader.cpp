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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "scenarioheader.h"
#include "version.h"
#include <array>

namespace game::ScenarioFileHeaderApi {

namespace {

/** Native game ABI.  The Russobit entry point was verified to take exactly six stack arguments
 * and return with `ret 0x18`. This wrapper deliberately does not request the optional data,
 * format-version, or expansion outputs. Keep the raw ABI private so feature code cannot call it
 * with a wrong signature. */
using NativeReadAndValidateFileHeader = bool(__stdcall*)(const char* filePath,
                                                         CMidgardID* scenarioFileId,
                                                         ScenarioFileHeader* header,
                                                         void* optionalDataOut,
                                                         int* formatVersion,
                                                         bool* expansionContent);

// clang-format off
static std::array<NativeReadAndValidateFileHeader, 4> functions = {{
    // Akella -- ranked capture is intentionally unsupported.
    nullptr,
    // Russobit
    (NativeReadAndValidateFileHeader)0x5e3db7,
    // GOG -- intentionally disabled until its complete ranked-capture path is verified.
    nullptr,
    // Scenario Editor
    nullptr,
}};
// clang-format on

} // namespace

bool readAndValidateFileHeader(const char* filePath,
                               CMidgardID* scenarioFileId,
                               ScenarioFileHeader* header)
{
    const auto version{static_cast<int>(hooks::gameVersion())};
    if (!filePath || !scenarioFileId || !header || version < 0
        || version >= static_cast<int>(functions.size()) || !functions[version]) {
        return false;
    }
    return functions[version](filePath, scenarioFileId, header, nullptr, nullptr, nullptr);
}

} // namespace game::ScenarioFileHeaderApi
