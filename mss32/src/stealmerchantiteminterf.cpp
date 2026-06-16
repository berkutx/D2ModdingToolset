/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/bartonsun/D2ModdingToolset)
 * Copyright (C) 2026 Max Vynogradov.
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
// ============================================================
// File: stealmerchantiteminterf.cpp
// ============================================================

#include "stealmerchantiteminterf.h"
#include "version.h"

#include <array>
#include <cstdint>

namespace game::StealMerchantItemInterfApi {

//
// helper wrappers
//

static game::CMidgardID* __fastcall getMerchantIdImpl(void* thisptr, void*)
{
    //
    // asm:
    //
    // mov edi, [esi+20h]
    //

    auto merchantId = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(thisptr) + 0x20);

    return reinterpret_cast<game::CMidgardID*>(merchantId);
}

static void* __fastcall getItemVectorImpl(void* thisptr, void*)
{
    //
    // asm:
    //
    // mov ecx, [esi+20h]
    // add ecx, 0Ch
    //

    auto interfaceData = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(thisptr) + 0x20);

    return reinterpret_cast<void*>(reinterpret_cast<std::uint8_t*>(interfaceData) + 0x0C);
}

// clang-format off

static std::array<Api, 4> functions = {{

    // Akella
    Api{
        (Api::Constructor)0x4A519E,
        (Api::GetListBox)0x4A56CE,
        (Api::AddStealItem)0x4A5BCA,

        (Api::GetMerchantId)getMerchantIdImpl,
        (Api::GetItemVector)getItemVectorImpl,
    },

    // Russobit
    Api{
        (Api::Constructor)0x4A519E,
        (Api::GetListBox)0x4A56CE,
        (Api::AddStealItem)0x4A5BCA,

        (Api::GetMerchantId)getMerchantIdImpl,
        (Api::GetItemVector)getItemVectorImpl,
    },

    // Gog
    Api{
        (Api::Constructor)0x4A4A1F,
        (Api::GetListBox)0x4A4F2F,
        (Api::AddStealItem)0x4A542B,

        (Api::GetMerchantId)getMerchantIdImpl,
        (Api::GetItemVector)getItemVectorImpl,
    },

    // Scenario Editor
    Api{
        (Api::Constructor)nullptr,
        (Api::GetListBox)nullptr,
        (Api::AddStealItem)nullptr,

        (Api::GetMerchantId)nullptr,
        (Api::GetItemVector)nullptr,
    }

}};

// clang-format on

Api& get()
{
    return functions[static_cast<int>(hooks::gameVersion())];
}

} // namespace game::StealMerchantItemInterfApi