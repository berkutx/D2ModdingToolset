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
#include "stackmovemsg.h"
#include <cstddef>
#include <spdlog/spdlog.h>

namespace hooks {

#ifdef D2_TESTDRV
// Latched on the UI thread by the object-lock check (called while the iso/strategic phase is up), so
// a testdrv move can reach sendStackMoveMsg with the live receiver. Gated so vanilla stays identical.
static game::CPhaseGame* g_stashedPhaseGame = nullptr;

// The live game phase, resolved from the client the SAME reliable way getObjectMap() does:
// CMidgard -> client -> client->data->phase. CMidClientData::phase is a CPhase* that points at the
// CPhase MEMBER embedded inside the active CPhaseGame (CPhaseGame first inherits INotifyCQ, so its
// CPhase lives at offset 8), so step back by that offset to recover the CPhaseGame. Validate by the
// back-pointer pg->data->midClient == client: a real game phase points back to this client, which both
// confirms the pointer arithmetic and rejects a non-game CPhase (a menu/other phase). NULL off-game.
static game::CPhaseGame* resolvePhaseGameFromClient()
{
    auto* midgard = game::CMidgardApi::get().instance();
    if (!midgard || !midgard->data || !midgard->data->client)
        return nullptr;
    game::CMidClient* client = midgard->data->client;
    if (!client->data || !client->data->phase)
        return nullptr;
    auto* pg = reinterpret_cast<game::CPhaseGame*>(
        reinterpret_cast<char*>(client->data->phase) - offsetof(game::CPhaseGame, phase));
    if (!pg->data || pg->data->midClient != client)
        return nullptr; // not the game phase (menu/other), or stale
    return pg;
}

game::CPhaseGame* getStashedPhaseGame()
{
    // Prefer resolving from the client: it VALIDATES via the midClient back-pointer, so it never hands
    // back a phase from a different or torn-down scenario. The object-lock-latched pointer is only a
    // fallback for the rare case the resolve fails - it is unvalidated and is never reset on teardown, so
    // returning it first could dangle across a scenario change. (The resolve also covers the idle
    // strategic map where the object-lock hook may not have fired - the case that made a just-loaded
    // move find null.)
    if (auto* pg = resolvePhaseGameFromClient())
        return pg;
    return g_stashedPhaseGame;
}
#endif

bool __fastcall phaseGameCheckObjectLockHooked(game::CPhaseGame* thisptr, int /*%edx*/)
{
#ifdef D2_TESTDRV
    g_stashedPhaseGame = thisptr;
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

    const auto& stackMoveMsgApi = CStackMoveMsgApi::get();

    auto* data = thisptr->data;
    if (!data->clientTakesTurn) {
        return;
    }

    ++data->midObjectLock->pendingNetworkUpdates;
    data->midObjectLock->patched.movingStack = true;
    spdlog::debug(
        __FUNCTION__ ": CMidObjectLock::movingStack set to true, pendingNetworkUpdates incremented to {:d}",
        data->midObjectLock->pendingNetworkUpdates);

    CStackMoveMsg message;
    stackMoveMsgApi.constructor2(&message, stackId, movementPath, startPosition, endPosition);

    CMidClient* client = data->midClient;
    CMidgard* midgard = client->core.data->midgard;
    CMidgardApi::get().sendNetMsgToServer(midgard, &message);

    stackMoveMsgApi.destructor(&message);
}

} // namespace hooks
