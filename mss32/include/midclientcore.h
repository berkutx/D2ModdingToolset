/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2021 Vladimir Makeev.
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

#ifndef MIDCLIENTCORE_H
#define MIDCLIENTCORE_H

#include "midcommandqueue2.h"
#include "mqnetsystem.h"

namespace game {

struct CMidgard;
struct CMidDataCache2;
struct CoreCommandUpdate;
struct CCommandCanIgnore;
struct CMidHotseatManager;

template <typename T>
struct CNetMsgT;
struct CNetMsgVftable;
using CNetMsg = CNetMsgT<CNetMsgVftable>;

struct CMidClientCoreData
{
    CMidgard* midgard;
    int unknown;
    CMidDataCache2* dataCache;
    bool scenarioInitialized;
    bool objectMapAccessed;
    char padding[2];
    CMidCommandQueue2* commandQueue;
    CoreCommandUpdate* coreCommandUpdate;
    CCommandCanIgnore* commandCanIgnore;
    CMidHotseatManager* hotseatManager;
};

assert_size(CMidClientCoreData, 32);
assert_offset(CMidClientCoreData, scenarioInitialized, 12);
assert_offset(CMidClientCoreData, objectMapAccessed, 13);

struct CMidClientCore : public IMqNetSystem
{
    CMidClientCoreData* data;
};

assert_size(CMidClientCore, 8);

struct CoreCommandUpdate : public CMidCommandQueue2::IUpdate
{
    CMidClientCore* midClientCore;
};

assert_size(CoreCommandUpdate, 8);

struct CCommandCanIgnore : public CMidCommandQueue2::ICanIgnore
{
    CMidClientCore* midClientCore;
};

assert_size(CCommandCanIgnore, 8);

namespace CMidClientCoreApi {

struct Api
{
    using GetObjectMap = CMidDataCache2*(__thiscall*)(CMidClientCore* thisptr);
    GetObjectMap getObjectMap;
};

Api& get();

} // namespace CMidClientCoreApi

} // namespace game

#endif // MIDCLIENTCORE_H
