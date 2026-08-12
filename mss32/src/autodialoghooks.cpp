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

#include "autodialoghooks.h"
#include "..\resource.h"
#include "autodialog.h"
#include "autodialogfile.h"
#include "dialogdescriptor.h"
#include "mempool.h"
#include "originalfunctions.h"
#include "utils.h"
#include <cstring>

namespace hooks {
namespace {

constexpr char customHostDialogKey[]{"DLG_CUSTOM_HOST"};
game::DialogDescriptor** hostDialogSlot{};
game::DialogDescriptor* customHostDialog{};

game::DialogDescriptor** findDialogSlot(game::DialogMap& dialogs, const char* name)
{
    for (auto iterator = dialogs.begin(); iterator != dialogs.end(); ++iterator) {
        if (std::strcmp(iterator->first, name) == 0) {
            return &iterator->second;
        }
    }
    return nullptr;
}

void destroyDialogDescriptor(game::DialogDescriptor* descriptor)
{
    game::DialogDescriptorApi::get().destructor(descriptor);
    game::Memory::get().freeNonZero(descriptor);
}

} // namespace

CustomHostDialogGuard::CustomHostDialogGuard()
{
    if (hostDialogSlot && customHostDialog) {
        m_slot = hostDialogSlot;
        m_original = *m_slot;
        *m_slot = customHostDialog;
    }
}

CustomHostDialogGuard::~CustomHostDialogGuard()
{
    if (m_slot) {
        *m_slot = m_original;
    }
}

bool __fastcall autoDialogLoadAndParseScriptFileHooked(game::CAutoDialog* thisptr,
                                                       int /*%edx*/,
                                                       const char* filePath)
{
    using namespace game;

    const auto& autoDialogApi = AutoDialogApi::get();
    const auto& autoDialogFileApi = AutoDialogFileApi::get();
    bool result = getOriginalFunctions().autoDialogLoadAndParseScriptFile(thisptr, filePath);
    if (!result) {
        return false;
    }

    const auto path{interfFolder() / "CustomLobby.dlg"};
    if (!writeResourceToFile(path, DLG_CUSTOM_LOBBY, false)) {
        return true;
    }

    CAutoDialogFile file{};
    autoDialogFileApi.constructor(&file, path.string().c_str(), 0);
    while (file.currentLineIdx < (int)file.lines.size()) {
        DialogDescriptor* descriptor = nullptr;
        if (!autoDialogApi.parseDialogFromScriptFile(&descriptor, thisptr, &file)) {
            break;
        }

        if (strcmp(descriptor->name, "DLG_HOST") == 0) {
            // Never replace the global DLG_HOST entry while loading resources. The custom lobby
            // exposes its descriptor only for the duration of its own native constructor.
            hostDialogSlot = findDialogSlot(thisptr->data->dialogs, "DLG_HOST");
            if (!hostDialogSlot) {
                destroyDialogDescriptor(descriptor);
                continue;
            }

            Pair<DialogMapIterator, bool> customResult{};
            DialogMapValue customEntry{};
            strncpy(customEntry.first, customHostDialogKey, sizeof(customEntry.first));
            customEntry.second = descriptor;
            if (autoDialogApi.dialogMapInsert(&thisptr->data->dialogs, &customResult, &customEntry)
                    ->second) {
                customHostDialog = descriptor;
            } else {
                customHostDialog = customResult.first->second;
                destroyDialogDescriptor(descriptor);
            }
            continue;
        }

        Pair<DialogMapIterator, bool> result{};
        DialogMapValue entry = {};
        strncpy(entry.first, descriptor->name, sizeof(entry.first));
        entry.second = descriptor;
        if (!autoDialogApi.dialogMapInsert(&thisptr->data->dialogs, &result, &entry)->second) {
            destroyDialogDescriptor(descriptor);
        }
    }

    autoDialogFileApi.destructor(&file);
    return true;
}

} // namespace hooks
