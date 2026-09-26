/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2024 Stanislav Egorov.
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

#include "phasegame.h"
#include "utils.h"
#include "version.h"
#include <array>
#include <cstdint>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace game::CPhaseGameApi {

// clang-format off
static std::array<Api, 4> functions = {{
    // Akella -- ranked host capture is intentionally unsupported for this build.
    Api{
        (Api::CheckObjectLock)0x4078b7,
        (Api::SendStackMoveMsg)0x40650f,
        (Api::SendSaveGameMsg)nullptr,
#ifdef D2_TESTDRV
        nullptr,
#endif
    },
    // Russobit
    Api{
        (Api::CheckObjectLock)0x4078b7,
        (Api::SendStackMoveMsg)0x40650f,
        (Api::SendSaveGameMsg)0x40639b,
#ifdef D2_TESTDRV
        (Api::ClientTakesTurn)0x406394,
#endif
    },
    // GOG -- ranked host capture is intentionally unsupported for this build.
    Api{
        (Api::CheckObjectLock)0x40753e,
        (Api::SendStackMoveMsg)0x40619b,
        (Api::SendSaveGameMsg)nullptr,
#ifdef D2_TESTDRV
        nullptr,
#endif
    },
    // Scenario Editor
    Api{
        (Api::CheckObjectLock)nullptr,
        (Api::SendStackMoveMsg)nullptr,
        (Api::SendSaveGameMsg)nullptr,
#ifdef D2_TESTDRV
        nullptr,
#endif
    },
}};
// clang-format on

Api& get()
{
    return functions[static_cast<int>(hooks::gameVersion())];
}

bool nativeSaveSupported()
{
    // Reference executable: Discipl2.exe, 4,187,648 bytes,
    // SHA-256 1375CDEF09EC470EE64FE5693FB734D7C69FB215212311D997F792B258A642EB.
    // At 0x40639b the audited disassembly consumes ECX as this and returns with retn 8. Checking
    // all recorded entry bytes below deliberately rejects size-only Russobit lookalikes.
    static constexpr std::array<std::uint8_t, 16> expectedPrologue{
        0xb8, 0xbc, 0x71, 0x68, 0x00, 0xe8, 0x2b, 0x70,
        0x26, 0x00, 0x83, 0xec, 0x14, 0x56, 0x57, 0x8b,
    };

    if (hooks::gameVersion() != hooks::GameVersion::Russobit) {
        return false;
    }

    std::error_code fileError;
    if (std::filesystem::file_size(hooks::exePath(), fileError) != 4187648 || fileError) {
        return false;
    }

    const auto entryPoint{get().sendSaveGameMsg};
    if (!entryPoint) {
        return false;
    }

    const auto code{reinterpret_cast<const std::uint8_t*>(entryPoint)};
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(code, &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protection{memory.Protect & 0xff};
    if (protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE
        && protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }

    const auto codeAddress{reinterpret_cast<std::uintptr_t>(code)};
    const auto regionAddress{reinterpret_cast<std::uintptr_t>(memory.BaseAddress)};
    // These are runtime bounds returned by VirtualQuery, not version-specific API addresses.
    if (codeAddress < regionAddress || expectedPrologue.size() > memory.RegionSize
        || codeAddress - regionAddress > memory.RegionSize - expectedPrologue.size()) {
        return false;
    }

    return std::memcmp(code, expectedPrologue.data(), expectedPrologue.size()) == 0;
}

} // namespace game::CPhaseGameApi
