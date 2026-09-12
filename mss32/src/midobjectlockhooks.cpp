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

#include "midobjectlockhooks.h"
#include "lobbysaveresume.h"
#include "midcommandqueue2.h"
#include "originalfunctions.h"
#include <spdlog/spdlog.h>

namespace hooks {

game::CMidObjectLock* __fastcall midObjectLockCtorHooked(game::CMidObjectLock* thisptr,
                                                         int /*%edx*/,
                                                         game::CMidCommandQueue2* commandQueue,
                                                         game::CMidDataCache2* dataCache)
{
    auto result = getOriginalFunctions().midObjectLockCtor(thisptr, commandQueue, dataCache);
    result->patched.movingStack = false;
    return result;
}

void __fastcall midObjectLockNotify1CallbackHooked(game::CMidObjectLock* thisptr, int /*%edx*/)
{
    prepareLobbySaveResumeUi(thisptr);
    ++thisptr->pendingLocalUpdates;
    spdlog::debug(
        __FUNCTION__ ": pendingLocalUpdates incremented to {:d}, processingCommand = {:d}",
        thisptr->pendingLocalUpdates, thisptr->commandQueue->processingCommand);

    game::CMidCommandQueue2Api::get().processCommands(thisptr->commandQueue);
}

void __fastcall midObjectLockNotify2CallbackHooked(game::CMidObjectLock* thisptr, int /*%edx*/)
{
    --thisptr->pendingLocalUpdates;
    spdlog::debug(
        __FUNCTION__ ": pendingLocalUpdates decremented to {:d}, processingCommand = {:d}",
        thisptr->pendingLocalUpdates, thisptr->commandQueue->processingCommand);

    game::CMidCommandQueue2Api::get().processCommands(thisptr->commandQueue);
}

void __fastcall midObjectLockOnObjectChangedHooked(game::CMidObjectLock* thisptr,
                                                   int /*%edx*/,
                                                   const game::IMidScenarioObject* /*obj*/)
{
    spdlog::debug(__FUNCTION__ ": reseting pendingNetworkUpdates to 0");
    thisptr->pendingNetworkUpdates = 0;
}

} // namespace hooks
