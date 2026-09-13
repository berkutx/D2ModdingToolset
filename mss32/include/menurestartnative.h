/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
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
 */

#ifndef MENURESTARTNATIVE_H
#define MENURESTARTNATIVE_H

namespace game {
struct CMenuPhase;
}

namespace hooks {

bool restartNativeSupported();

/** Reuses the native start action. Call once, after the restart setup barrier. */
bool pressRestartNativeStartButton(game::CMenuPhase* phase);

/** Query after ReqLord; AnsStartInfo reports the selections accepted by the host. */
bool requestRestartSetupInfo();

} // namespace hooks

#endif // MENURESTARTNATIVE_H
