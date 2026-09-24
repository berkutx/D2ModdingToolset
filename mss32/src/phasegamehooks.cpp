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

#include "phasegamehooks.h"
#include "midclient.h"
#include "midgard.h"
#include "midobjectlock.h"
#include "phasegame.h"
#ifdef D2_SIMTURNS
#include "simturns/controller.h"
#include "simturns/state.h"
#endif
#include "stackmovemsg.h"
#include <spdlog/spdlog.h>

namespace hooks {

bool __fastcall phaseGameCheckObjectLockHooked(game::CPhaseGame* thisptr, int /*%edx*/)
{
#ifdef D2_SIMTURNS
    simturns::onPhaseGame(thisptr);
#endif
    const auto* lock = thisptr->data->midObjectLock;
    if (lock->patched.exportingLeader) {
        spdlog::debug(__FUNCTION__ ": unlocked due to exportingLeader");
        return false;
    }

    if (lock->patched.movingStack) {
        return true;
    }

    return lock->pendingLocalUpdates || lock->pendingNetworkUpdates;
}

void __fastcall phaseGameSendStackMoveMsgHooked(
    game::CPhaseGame* thisptr,
    int /*%edx*/,
    const game::CMidgardID* stackId,
    const game::List<game::Pair<game::CMqPoint, int>>* movementPath,
    const game::CMqPoint* startPosition,
    const game::CMqPoint* endPosition)
{
    using namespace game;

#ifdef D2_SIMTURNS
    simturns::onPhaseGame(thisptr);
    switch (simturns::phase()) {
    case simturns::Phase::Disabled:
    case simturns::Phase::Stock:
    case simturns::Phase::Independent:
    case simturns::Phase::Merged:
        break;
    case simturns::Phase::Prepared:
    case simturns::Phase::WaitingForSession:
    case simturns::Phase::Ready:
    case simturns::Phase::Held:
    case simturns::Phase::Merging:
    case simturns::Phase::AwaitingStockTurn:
    case simturns::Phase::Closing:
    case simturns::Phase::Faulted:
        return;
    }
#endif
    const auto& stackMoveMsgApi = CStackMoveMsgApi::get();

    auto* data = thisptr->data;
    if (!data->clientTakesTurn) {
        return;
    }

#ifdef D2_SIMTURNS
    const auto previousPendingNetworkUpdates = data->midObjectLock->pendingNetworkUpdates;
    const bool previousMovingStack = data->midObjectLock->patched.movingStack;
#endif
    ++data->midObjectLock->pendingNetworkUpdates;
    data->midObjectLock->patched.movingStack = true;
    spdlog::debug(
        __FUNCTION__ ": CMidObjectLock::movingStack set to true, pendingNetworkUpdates incremented to {:d}",
        data->midObjectLock->pendingNetworkUpdates);

    CStackMoveMsg message;
    stackMoveMsgApi.constructor2(&message, stackId, movementPath, startPosition, endPosition);

    CMidClient* client = data->midClient;
    CMidgard* midgard = client->core.data->midgard;
#ifdef D2_SIMTURNS
    const bool sent = CMidgardApi::get().sendNetMsgToServer(midgard, &message);
#else
    CMidgardApi::get().sendNetMsgToServer(midgard, &message);
#endif

    stackMoveMsgApi.destructor(&message);
#ifdef D2_SIMTURNS
    if (!sent && simturns::phase() != simturns::Phase::Disabled) {
        // This attempt was rejected before delivery. Restore only its lock
        // contribution; do not retry or dispatch through a second transport.
        data->midObjectLock->pendingNetworkUpdates = previousPendingNetworkUpdates;
        data->midObjectLock->patched.movingStack = previousMovingStack;
        spdlog::error(__FUNCTION__ ": native CStackMoveMsg send rejected");
    }
#endif
}

} // namespace hooks
