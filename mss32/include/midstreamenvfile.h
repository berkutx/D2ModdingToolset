/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/VladimirMakeev/D2ModdingToolset)
 * Copyright (C) 2022 Vladimir Makeev.
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

#ifndef MIDSTREAMENVFILE_H
#define MIDSTREAMENVFILE_H

#include "d2pair.h"
#include "d2set.h"
#include "d2string.h"
#include "midgardid.h"
#include "midgardstreamenv.h"

namespace game {

struct IStdFileImpl;
struct CMidStreamCountFile;
struct CMidStreamFile;
struct ScenarioFileHeader;
struct CStreamBits;

struct StreamEnvFileInfo
{
    Pair<const char* /* dbf file name */, const char* /* Assumption: dbf SQL query */> pair;
    CMidStreamFile* stream;
};

/** Implementation of IMidgardStreamEnv that works with files. */
struct CMidStreamEnvFile : public IMidgardStreamEnv
{
    CMidgardID scenarioId;
    bool readMode;
    bool unknown2;
    char padding[2];
    IStdFileImpl* stdFileImpl;
    String fileName;
    CMidgardID unknownId;
    String errorMessage;
    CMidStreamCountFile* streamCountFile;
    Set<StreamEnvFileInfo> streams;
};

assert_size(CMidStreamEnvFile, 84);

namespace CMidStreamEnvFileApi {

struct Api
{
    /** Constructs a file-backed write environment and writes the scenario header.
     * The optional stream contains host-only AI state and may be null. */
    using WriteConstructor = CMidStreamEnvFile*(__thiscall*)(CMidStreamEnvFile* thisptr,
                                                             const char* filePath,
                                                             const CMidgardID* scenarioId,
                                                             bool isExpansionContent,
                                                             ScenarioFileHeader* header,
                                                             CStreamBits* optionalData);
    WriteConstructor writeConstructor;
};

Api& get();

} // namespace CMidStreamEnvFileApi

} // namespace game

#endif // MIDSTREAMENVFILE_H
