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

#ifndef PHASEGAMEHOOKS_H
#define PHASEGAMEHOOKS_H

#include "d2list.h"
#include "d2pair.h"

namespace game {
struct CMidgardID;
struct CMqPoint;
struct CPhaseGame;
} // namespace game

namespace hooks {

#ifdef D2_TESTDRV
/**
 * testdrv: the live CPhaseGame, so a programmatic test move can reach sendStackMoveMsg without walking
 * the iso-view task tree. Returns the pointer latched by phaseGameCheckObjectLockHooked when available,
 * otherwise resolves it fresh from CMidgard->client->data->phase (the same path getObjectMap() uses) and
 * validates it by the midClient back-pointer. NULL off-game. The fresh resolve is what makes a move work
 * on the idle strategic map, where the object-lock hook may not have fired (the latch alone was racy).
 * Call on the UI thread only (the client/phase are UI-thread-affine).
 */
game::CPhaseGame* getStashedPhaseGame();
#endif

bool __fastcall phaseGameCheckObjectLockHooked(game::CPhaseGame* thisptr, int /*%edx*/);

void __fastcall phaseGameSendStackMoveMsgHooked(
    game::CPhaseGame* thisptr,
    int /*%edx*/,
    const game::CMidgardID* stackId,
    const game::List<game::Pair<game::CMqPoint, int>>* movementPath,
    const game::CMqPoint* startPosition,
    const game::CMqPoint* endPosition);

} // namespace hooks

#endif // PHASEGAMEHOOKS_H
