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

#ifndef AUTODIALOGHOOKS_H
#define AUTODIALOGHOOKS_H

namespace game {
struct CAutoDialog;
struct DialogDescriptor;
} // namespace game

namespace hooks {

/** Temporarily exposes the custom lobby host dialog to the native DLG_HOST constructor. */
class CustomHostDialogGuard
{
public:
    CustomHostDialogGuard();
    ~CustomHostDialogGuard();

    CustomHostDialogGuard(const CustomHostDialogGuard&) = delete;
    CustomHostDialogGuard& operator=(const CustomHostDialogGuard&) = delete;

private:
    game::DialogDescriptor** m_slot{};
    game::DialogDescriptor* m_original{};
};

bool __fastcall autoDialogLoadAndParseScriptFileHooked(game::CAutoDialog* thisptr,
                                                       int /*%edx*/,
                                                       const char* filePath);

} // namespace hooks

#endif // AUTODIALOGHOOKS_H
