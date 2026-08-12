/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/bartonsun/D2ModdingToolset)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "midstreamenvfile.h"
#include "version.h"
#include <array>

namespace game::CMidStreamEnvFileApi {

// The write constructor is verified only for the byte-identical Akella/Russobit builds.  GOG
// ranked capture is intentionally unsupported: host save, stream-layout, and vtable addresses all
// require a complete per-build verification, not a guessed constructor address.  A null entry
// makes the trusted V2 request fail closed with UnsupportedGameBuild.
static std::array<Api, 4> functions = {{
    // Akella
    Api{(Api::WriteConstructor)0x5e28fe},
    // Russobit
    Api{(Api::WriteConstructor)0x5e28fe},
    // GOG -- intentionally unsupported until the complete capture path is verified.
    Api{nullptr},
    // Scenario Editor
    Api{nullptr},
}};

Api& get()
{
    return functions[static_cast<int>(hooks::gameVersion())];
}

} // namespace game::CMidStreamEnvFileApi
