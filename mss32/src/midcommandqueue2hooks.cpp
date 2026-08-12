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

#include "midcommandqueue2hooks.h"
#include "commandmsg.h"
#include "lobbysaveexchange.h"
#include "mempool.h"
#include "netmsgcallbacks.h"
#include "netmsgmapentrycmdmovestackendmsg.h"
#include "originalfunctions.h"
#include "version.h"
#include <cstddef>
#include <cstring>
#include <new>
#include <spdlog/spdlog.h>
#include <string>

namespace hooks {
namespace {

/** Russobit CCmdGameSavedMsg layout recovered from the native constructor/serializer. Keep this
 * view local to the only version for which the ABI was verified. */
struct CCmdGameSavedMsgView
{
    game::CCommandMsg command;
    bool success;
    char padding1[3];
    const char* savePath;
    bool uiLockRequest;
    char padding2[3];
};

static_assert(sizeof(CCmdGameSavedMsgView) == 0x1c);
static_assert(offsetof(CCmdGameSavedMsgView, success) == 0x10);
static_assert(offsetof(CCmdGameSavedMsgView, savePath) == 0x14);
static_assert(offsetof(CCmdGameSavedMsgView, uiLockRequest) == 0x18);

/** Copies the native owned path before the original queue push can release/copy the message. */
bool copyNativeSavePath(const char* source, std::string& result)
{
    static constexpr std::size_t maxNativeSavePath{4096};
    if (!source) {
        return false;
    }

    const auto length{strnlen(source, maxNativeSavePath)};
    if (length == maxNativeSavePath) {
        return false;
    }
    result.assign(source, length);
    return true;
}

} // namespace

game::CMidCommandQueue2::CNMMap* __fastcall netMsgMapConstructorHooked(
    game::CMidCommandQueue2::CNMMap* thisptr,
    int /*%edx*/,
    game::NetMsgCallbacks** netCallbacks,
    game::CMidCommandQueue2* commandQueue)
{
    using namespace game;

    const auto& commandQueueApi = CMidCommandQueue2Api::get();

    auto result = getOriginalFunctions().netMsgMapConstructor(thisptr, netCallbacks, commandQueue);

    auto entry = (CNetMsgMapEntryCmdMoveStackEndMsg*)Memory::get().allocate(
        sizeof(CNetMsgMapEntryCmdMoveStackEndMsg));
    new (entry)
        CNetMsgMapEntryCmdMoveStackEndMsg(thisptr, commandQueueApi.netMsgMapQueueMessageCallback);

    NetMsgApi::get().addEntry(thisptr->netMsgEntryData, (CNetMsgMapEntry*)entry);

    return result;
}

void __fastcall midCommandQueue2PushHooked(game::CMidCommandQueue2* thisptr,
                                           int /*%edx*/,
                                           const game::CCommandMsg* commandMsg)
{
    using namespace game;

    const auto& commandQueueApi = CMidCommandQueue2Api::get();

    if (commandMsg->playerId == emptyId) {
        std::uint32_t sequenceNumber = commandMsg->sequenceNumber;
        std::uint32_t lastCommandSequenceNumber = *commandQueueApi.lastCommandSequenceNumber;
        if (sequenceNumber <= lastCommandSequenceNumber) {
            spdlog::error(
                __FUNCTION__ ": message with id {:d} is rejected due to outdated sequence number {:d} vs last {:d}",
                (int)commandMsg->vftable->getId(commandMsg), sequenceNumber,
                lastCommandSequenceNumber);
        }
    }

    bool gameSavedResultAvailable{};
    bool gameSavedSuccessfully{};
    std::string gameSavedPath;
    if (hasActiveLobbyHostSaveTransfer()
        && commandMsg->vftable->getId(commandMsg) == CommandMsgId::GameSaved
        && gameVersion() == GameVersion::Russobit) {
        const auto result{reinterpret_cast<const CCmdGameSavedMsgView*>(commandMsg)};
        gameSavedSuccessfully = result->success;
        gameSavedResultAvailable = copyNativeSavePath(result->savePath, gameSavedPath);
        if (!gameSavedResultAvailable) {
            spdlog::warn(__FUNCTION__ ": ignored GameSaved with an invalid native path");
        }
    }

    getOriginalFunctions().midCommandQueue2Push(thisptr, commandMsg);

    if (gameSavedResultAvailable) {
        handleGameSavedForLobby(gameSavedSuccessfully, gameSavedPath);
    }
}

} // namespace hooks
