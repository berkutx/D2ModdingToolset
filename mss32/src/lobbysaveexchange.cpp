/*
 * This file is part of the modding toolset for Disciples 2.
 * (https://github.com/bartonsun/D2ModdingToolset)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "lobbysaveexchange.h"
#include "game.h"
#include "gamesettings.h"
#include "gameutils.h"
#include "midclient.h"
#include "midclientcore.h"
#include "middatacache.h"
#include "midgard.h"
#include "midgardid.h"
#include "midgardscenariomap.h"
#ifdef small
#undef small
#endif
#include "midplayer.h"
#include "midstreamenvfile.h"
#include "netcustomsession.h"
#include "phase.h"
#include "phasegame.h"
#include "racetype.h"
#include "scenarioheader.h"
#include "scenarioinfo.h"
#include "utils.h"
#include "version.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <slikenet/BitStream.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

extern std::thread::id mainThreadId;

namespace hooks {
namespace {

using namespace LobbyProtocol;

static constexpr std::size_t saveRolesTotal{2};
static constexpr auto storedAckGrace{std::chrono::seconds(10)};

struct SaveTransferSession
{
    SaveRequestV2 request;
    std::string saveName;
    std::filesystem::path savePath;
    std::chrono::steady_clock::time_point deadline;
};

struct PendingStoredAck
{
    std::uint64_t saveId{};
    std::filesystem::path savePath;
    std::chrono::steady_clock::time_point deadline;
};

std::array<std::optional<SaveTransferSession>, saveRolesTotal> activeTransfers;
std::array<std::optional<PendingStoredAck>, saveRolesTotal> pendingStoredAcks;
// Both the network callback and save handling run on the game/UI thread.
int clientExpansionContent{-1};
int clientExpansionScenarioId{std::numeric_limits<int>::min()};

std::size_t roleIndex(SaveRoleV2 role)
{
    return static_cast<std::size_t>(role);
}

bool isKnownRole(SaveRoleV2 role)
{
    return role == SaveRoleV2::Host || role == SaveRoleV2::Joiner;
}

bool isMainThread()
{
    return std::this_thread::get_id() == mainThreadId;
}

bool isDeadlineExpired(const SaveTransferSession& transfer)
{
    return std::chrono::steady_clock::now() >= transfer.deadline;
}

void finishTransfer(SaveRoleV2 role)
{
    activeTransfers[roleIndex(role)].reset();
}

void awaitStoredAck(SaveRoleV2 role)
{
    auto& active{activeTransfers[roleIndex(role)]};
    if (!active) {
        return;
    }

    PendingStoredAck pending{};
    pending.saveId = active->request.saveId;
    pending.savePath = std::move(active->savePath);
    // The authoritative request deadline governs capture/upload. Preserve a bounded grace beyond
    // it for the server to drain a large reliable transfer and return its Stored ACK.
    pending.deadline = active->deadline + storedAckGrace;
    pendingStoredAcks[roleIndex(role)] = std::move(pending);
    active.reset();
}

std::filesystem::path getSaveFolder(const game::CMidgard* midgard)
{
    std::filesystem::path folder{gameFolder()};

    auto settings = midgard->data->settings;
    if (settings && *settings && (*settings)->saveGameFolder.string) {
        folder /= (*settings)->saveGameFolder.string;
    } else {
        folder /= "SaveGame";
    }

    return folder;
}

std::string makeSaveName(SaveRoleV2 role, std::uint64_t saveId)
{
    const char* origin{role == SaveRoleV2::Host ? "Host" : "Joiner"};
    std::ostringstream result;
    result << "LobbyMatch" << origin << '_' << std::hex << std::setw(16) << std::setfill('0')
           << saveId;
    return result.str();
}

std::filesystem::path makeSavePath(const game::CMidgard* midgard,
                                   const std::string& saveName)
{
    return getSaveFolder(midgard) / (saveName + ".sg");
}

bool sendSaveDataPrefix(SLNet::BitStream& stream,
                        std::uint64_t saveId,
                        SaveDataOperationV2 operation)
{
    auto service{CNetCustomService::get()};
    if (!service) {
        return false;
    }

    stream.Write(static_cast<SLNet::MessageID>(ID_LOBBY_SAVE_UPLOAD));
    stream.Write(saveTransferVersion);
    stream.Write(saveId);
    stream.Write(static_cast<std::uint8_t>(operation));
    return true;
}

bool sendSaveData(SLNet::BitStream& stream)
{
    auto service{CNetCustomService::get()};
    return service
        && service->send(stream, service->getLobbyGuid(), PacketPriority::MEDIUM_PRIORITY);
}

bool validateSaveSignature(const std::filesystem::path& path);

bool uploadSave(SaveTransferSession& transfer, SaveFailureV2& failure)
{
    std::error_code error;
    const auto fileSize64{std::filesystem::file_size(transfer.savePath, error)};
    if (error || fileSize64 == 0) {
        failure = SaveFailureV2::FileIo;
        return false;
    }
    if (fileSize64 > transfer.request.maxBytes || fileSize64 > saveFileHardLimit
        || fileSize64 > std::numeric_limits<std::uint32_t>::max()) {
        failure = SaveFailureV2::TooLarge;
        return false;
    }
    if (isDeadlineExpired(transfer)) {
        failure = SaveFailureV2::TimedOut;
        return false;
    }
    if (!validateSaveSignature(transfer.savePath)) {
        failure = SaveFailureV2::CaptureFailed;
        return false;
    }

    const auto totalSize{static_cast<std::uint32_t>(fileSize64)};
    SLNet::BitStream begin;
    if (!sendSaveDataPrefix(begin, transfer.request.saveId, SaveDataOperationV2::Begin)) {
        failure = SaveFailureV2::SendFailed;
        return false;
    }
    begin.Write(totalSize);
    if (!sendSaveData(begin)) {
        failure = SaveFailureV2::SendFailed;
        return false;
    }

    std::ifstream file{transfer.savePath, std::ios::binary};
    if (!file) {
        failure = SaveFailureV2::FileIo;
        return false;
    }

    std::array<unsigned char, saveChunkSizeMax> buffer{};
    for (std::uint32_t offset = 0; offset < totalSize;) {
        if (isDeadlineExpired(transfer)) {
            failure = SaveFailureV2::TimedOut;
            return false;
        }

        const auto chunkSize{static_cast<std::uint16_t>(
            std::min<std::uint32_t>(saveChunkSizeMax, totalSize - offset))};
        file.read(reinterpret_cast<char*>(buffer.data()), chunkSize);
        if (!file) {
            failure = SaveFailureV2::FileIo;
            return false;
        }

        SLNet::BitStream chunk;
        if (!sendSaveDataPrefix(chunk, transfer.request.saveId, SaveDataOperationV2::Chunk)) {
            failure = SaveFailureV2::SendFailed;
            return false;
        }
        chunk.Write(offset);
        chunk.Write(chunkSize);
        chunk.WriteAlignedBytes(buffer.data(), chunkSize);
        if (!sendSaveData(chunk)) {
            failure = SaveFailureV2::SendFailed;
            return false;
        }
        offset += chunkSize;
    }

    if (isDeadlineExpired(transfer)) {
        failure = SaveFailureV2::TimedOut;
        return false;
    }

    SLNet::BitStream commit;
    if (!sendSaveDataPrefix(commit, transfer.request.saveId, SaveDataOperationV2::Commit)) {
        failure = SaveFailureV2::SendFailed;
        return false;
    }
    if (!sendSaveData(commit)) {
        failure = SaveFailureV2::SendFailed;
        return false;
    }

    spdlog::info(__FUNCTION__
                 ": sent local save id {:016x}, role {:d}, {:d} bytes; awaiting stored ACK",
                 transfer.request.saveId, static_cast<int>(transfer.request.role), totalSize);
    return true;
}

template <std::size_t size>
void copyHeaderString(char (&destination)[size], const char* source)
{
    if (!source || !*source) {
        return;
    }

    const auto length{std::min<std::size_t>(std::strlen(source), size - 1)};
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

void buildJoinerHeader(game::ScenarioFileHeader& header,
                       const game::IMidgardObjectMap* objectMap,
                       const game::CMidClient* client,
                       const game::CMidgard* midgard)
{
    const auto scenarioInfo{getScenarioInfo(objectMap)};
    copyHeaderString(header.description, scenarioInfo->description);
    copyHeaderString(header.author, scenarioInfo->creator);
    copyHeaderString(header.name, scenarioInfo->name);
    header.official = scenarioInfo->official != 0;
    header.mapSize = scenarioInfo->mapSize;
    header.difficulty = scenarioInfo->gameDifficulty.id;
    header.turnNumber = scenarioInfo->currentTurn;
    header.unknown2 = scenarioInfo->suggestedLevel;

    if (scenarioInfo->campaignId != game::invalidId) {
        const auto campaignId{idToString(&scenarioInfo->campaignId)};
        copyHeaderString(header.campaignId, campaignId.c_str());
    }

    auto settings{midgard->data->settings};
    if (settings && *settings && (*settings)->defaultPlayerName.string) {
        copyHeaderString(header.defaultPlayerName, (*settings)->defaultPlayerName.string);
    }

    if (client->data && client->data->phase) {
        const auto playerId{game::CPhaseApi::get().getCurrentPlayerId(client->data->phase)};
        const auto player{playerId ? getPlayer(objectMap, playerId) : nullptr};
        if (player && player->raceType && player->raceType->data) {
            header.race = player->raceType->data->raceType.id;
        }
    }

    const auto racesTotal{std::min<std::uint32_t>(scenarioInfo->races.length,
                                                  static_cast<std::uint32_t>(
                                                      std::size(header.races)))};
    header.racesTotal = static_cast<int>(racesTotal);
    if (scenarioInfo->races.head) {
        auto race{scenarioInfo->races.begin()};
        const auto end{scenarioInfo->races.end()};
        for (std::uint32_t index = 0; index < racesTotal && race != end; ++index, ++race) {
            header.races[index].race = race->id;
        }
    }

    // The joiner has no CMidServerLogic::aiLogic payload. Unused player records, PRNG state and
    // optional-data size remain zero, matching the captured joiner header.
    header.paddingSize = 0;
}

game::CPhaseGame* getHostPhaseGame(const game::CMidgard* midgard)
{
    if (!isMainThread() || !midgard || !midgard->data || !midgard->data->multiplayerGame
        || !midgard->data->host || !midgard->data->gameIsRunning || !midgard->data->client) {
        return nullptr;
    }

    const auto client{midgard->data->client};
    if (!client->data || !client->data->scenarioStarted || !client->data->phase) {
        return nullptr;
    }

    const auto phase{client->data->phase};
    const auto phaseGame{reinterpret_cast<game::CPhaseGame*>(
        reinterpret_cast<std::uint8_t*>(phase) - offsetof(game::CPhaseGame, phase))};
    return phaseGame->data && phaseGame->data->midClient == client ? phaseGame : nullptr;
}

bool sameWindowsPath(const std::filesystem::path& first,
                     const std::filesystem::path& second)
{
    const auto firstNative{first.native()};
    const auto secondNative{second.native()};
    if (firstNative.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || secondNative.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    return CompareStringOrdinal(firstNative.data(), static_cast<int>(firstNative.size()),
                                secondNative.data(), static_cast<int>(secondNative.size()), TRUE)
        == CSTR_EQUAL;
}

bool isExpectedHostSaveResultPath(const std::string& observed,
                                  const std::filesystem::path& expected)
{
    if (observed.empty()) {
        return false;
    }

    try {
        auto expectedPath{expected};
        if (expectedPath.is_relative()) {
            expectedPath = std::filesystem::path{gameFolder()} / expectedPath;
        }
        expectedPath = expectedPath.lexically_normal();

        std::filesystem::path observedPath{observed};
        if (observedPath.is_relative()) {
            observedPath = observedPath.has_parent_path()
                ? std::filesystem::path{gameFolder()} / observedPath
                : expectedPath.parent_path() / observedPath;
        }
        observedPath = observedPath.lexically_normal();
        return sameWindowsPath(observedPath, expectedPath);
    } catch (...) {
        return false;
    }
}

const game::IMidgardObjectMapVftable* expectedDataCacheVftable()
{
    switch (gameVersion()) {
    case GameVersion::Akella:
    case GameVersion::Russobit:
        return reinterpret_cast<const game::IMidgardObjectMapVftable*>(0x6cf8cc);
    default:
        return nullptr;
    }
}

const game::IMidgardStreamEnvVftable* expectedWriteStreamEnvVftable()
{
    switch (gameVersion()) {
    case GameVersion::Akella:
    case GameVersion::Russobit:
        return reinterpret_cast<const game::IMidgardStreamEnvVftable*>(0x6f010c);
    default:
        return nullptr;
    }
}

game::CMidgardScenarioMap* getVerifiedClientScenarioMap(game::CMidDataCache2* cache)
{
    const auto expectedCacheVftable{expectedDataCacheVftable()};
    if (!cache || !expectedCacheVftable || cache->vftable != expectedCacheVftable) {
        spdlog::warn(__FUNCTION__ ": unexpected CMidDataCache2/vtable");
        return nullptr;
    }

    auto scenarioMap{cache->scenarioMap};
    if (!scenarioMap || scenarioMap->vftable != game::CMidgardScenarioMapApi::vftable()) {
        spdlog::warn(__FUNCTION__ ": client-cache backing map pointer/vtable mismatch");
        return nullptr;
    }

    const auto objectsTotal{scenarioMap->vftable->getObjectsTotal(scenarioMap)};
    if (objectsTotal <= 0 || objectsTotal > 1000000
        || !getScenarioInfo(scenarioMap)) {
        spdlog::warn(__FUNCTION__ ": invalid local scenario map, objects = {:d}", objectsTotal);
        return nullptr;
    }

    return scenarioMap;
}

bool tryGetClientExpansionContent(const game::CMidClient*,
                                  const game::CMidgardID& scenarioId,
                                  bool& result)
{
    const auto observed{clientExpansionContent};
    const auto observedScenarioId{clientExpansionScenarioId};
    if (observed < 0 || observedScenarioId != scenarioId.value) {
        spdlog::warn(__FUNCTION__
                     ": expansion stream is not bound to scenario {:08x} (observed {:08x})",
                     static_cast<unsigned int>(scenarioId.value),
                     static_cast<unsigned int>(observedScenarioId));
        return false;
    }
    result = observed != 0;
    return true;
}

bool validateSaveSignature(const std::filesystem::path& path)
{
    static constexpr char signature[]{"D2EESFISIG"};
    std::array<char, sizeof(signature) - 1> actual{};
    std::ifstream file{path, std::ios::binary};
    return file && file.read(actual.data(), actual.size())
        && std::equal(actual.begin(), actual.end(), std::begin(signature));
}

bool validateNativeSaveHeader(const std::filesystem::path& path,
                              const game::CMidgardID& expectedScenarioId)
{
    game::CMidgardID parsedScenarioId{game::invalidId};
    game::ScenarioFileHeader parsedHeader{};
    bool valid{false};
    try {
        valid = game::ScenarioFileHeaderApi::readAndValidateFileHeader(
            path.string().c_str(), &parsedScenarioId, &parsedHeader);
    } catch (...) {
        spdlog::warn(__FUNCTION__ ": native header reader raised a C++ exception");
        return false;
    }

    if (!valid || parsedScenarioId != expectedScenarioId || parsedHeader.racesTotal <= 0
        || parsedHeader.racesTotal > static_cast<int>(std::size(parsedHeader.races))) {
        spdlog::warn(__FUNCTION__
                     ": invalid native header, ok = {:d}, scenario = {:08x}/{:08x}, races = {:d}",
                     static_cast<int>(valid),
                     static_cast<unsigned int>(parsedScenarioId.value),
                     static_cast<unsigned int>(expectedScenarioId.value),
                     parsedHeader.racesTotal);
        return false;
    }

    return true;
}

bool captureJoinerSave(SaveTransferSession& transfer, SaveFailureV2& failure)
{
    if (!isMainThread()) {
        failure = SaveFailureV2::UnsafePhase;
        return false;
    }

    auto midgard{game::CMidgardApi::get().instance()};
    if (!midgard || !midgard->data || !midgard->data->client) {
        failure = SaveFailureV2::NoActiveGame;
        return false;
    }

    auto client{midgard->data->client};
    if (!midgard->data->gameIsRunning || !client->data || !client->data->scenarioStarted
        || !client->data->phase) {
        failure = SaveFailureV2::UnsafePhase;
        return false;
    }

    const auto& envApi{game::CMidStreamEnvFileApi::get()};
    if (!envApi.writeConstructor) {
        failure = SaveFailureV2::UnsupportedGameBuild;
        return false;
    }

    auto cache{game::CMidClientCoreApi::get().getObjectMap(&client->core)};
    auto scenarioMap{getVerifiedClientScenarioMap(cache)};
    if (!scenarioMap) {
        failure = SaveFailureV2::CaptureFailed;
        return false;
    }

    const auto getIdType{game::CMidgardIDApi::get().getType};
    if (!getIdType || getIdType(&scenarioMap->scenarioFileId) != game::IdType::ScenarioFile) {
        spdlog::warn(__FUNCTION__ ": invalid client scenario id {:08x}",
                     static_cast<unsigned int>(scenarioMap->scenarioFileId.value));
        failure = SaveFailureV2::CaptureFailed;
        return false;
    }

    game::ScenarioFileHeader header{};
    buildJoinerHeader(header, scenarioMap, client, midgard);
    if (header.racesTotal <= 0
        || header.racesTotal > static_cast<int>(std::size(header.races))) {
        spdlog::warn(__FUNCTION__ ": invalid header race count {:d}", header.racesTotal);
        failure = SaveFailureV2::CaptureFailed;
        return false;
    }

    bool isExpansionContent{};
    if (!tryGetClientExpansionContent(client, scenarioMap->scenarioFileId,
                                      isExpansionContent)) {
        failure = SaveFailureV2::SerializerUnvalidated;
        return false;
    }

    const auto expectedEnvVftable{expectedWriteStreamEnvVftable()};
    if (!expectedEnvVftable) {
        failure = SaveFailureV2::UnsupportedGameBuild;
        return false;
    }

    spdlog::info(__FUNCTION__
                 ": begin lobby-requested joiner capture id {:016x}, scenario {:08x}, "
                 "objects {:d}, turn {:d}, races {:d}, expansion {:d}, path '{:s}'",
                 transfer.request.saveId,
                 static_cast<unsigned int>(scenarioMap->scenarioFileId.value),
                 scenarioMap->vftable->getObjectsTotal(scenarioMap), header.turnNumber,
                 header.racesTotal, static_cast<int>(isExpansionContent),
                 transfer.savePath.string());

    std::error_code error;
    std::filesystem::create_directories(transfer.savePath.parent_path(), error);
    if (error) {
        failure = SaveFailureV2::FileIo;
        return false;
    }

    auto partPath{transfer.savePath};
    partPath += ".part";
    std::filesystem::remove(partPath, error);
    error.clear();

    game::CMidStreamEnvFile streamEnv{};
    bool constructed{false};
    bool streamed{false};
    bool destroyed{false};
    std::string streamError;
    try {
        auto result{envApi.writeConstructor(&streamEnv, partPath.string().c_str(),
                                             &scenarioMap->scenarioFileId, isExpansionContent,
                                             &header, nullptr)};
        constructed = result == &streamEnv && streamEnv.vftable == expectedEnvVftable;
        if (constructed) {
            if (streamEnv.vftable->getError) {
                const auto text{streamEnv.vftable->getError(&streamEnv)};
                if (text) {
                    streamError = text;
                }
            }
            const bool modesValid{streamEnv.vftable->writeMode
                                  && streamEnv.vftable->writeMode(&streamEnv)
                                  && streamEnv.vftable->readMode
                                  && !streamEnv.vftable->readMode(&streamEnv)
                                  && streamEnv.vftable->isExpansionContent
                                  && streamEnv.vftable->isExpansionContent(&streamEnv)
                                      == isExpansionContent};
            if (streamError.empty() && modesValid) {
                streamed = game::CMidgardScenarioMapApi::get().stream(scenarioMap, &streamEnv);
                if (streamEnv.vftable->getError) {
                    const auto text{streamEnv.vftable->getError(&streamEnv)};
                    if (text) {
                        streamError = text;
                    }
                }
            } else if (!modesValid) {
                streamError = "unexpected native stream mode";
            }
        }
    } catch (...) {
        spdlog::warn(__FUNCTION__ ": native constructor/stream raised a C++ exception");
        streamed = false;
    }

    if (constructed) {
        // flags == 0 destroys the stack object and flushes/closes files without freeing `this`.
        try {
            streamEnv.vftable->destructor(&streamEnv, 0);
            destroyed = true;
        } catch (...) {
            spdlog::warn(__FUNCTION__ ": native stream destructor raised a C++ exception");
        }
    }

    if (!constructed || !streamed || !destroyed || !streamError.empty()
        || !validateSaveSignature(partPath)
        || !validateNativeSaveHeader(partPath, scenarioMap->scenarioFileId)) {
        spdlog::warn(__FUNCTION__
                     ": local map stream failed, constructed = {:d}, streamed = {:d}, "
                     "destroyed = {:d}, error = '{:s}'",
                     static_cast<int>(constructed), static_cast<int>(streamed),
                     static_cast<int>(destroyed), streamError);
        std::filesystem::remove(partPath, error);
        failure = SaveFailureV2::CaptureFailed;
        return false;
    }

    const auto fileSize{std::filesystem::file_size(partPath, error)};
    const bool sizeReadFailed{static_cast<bool>(error)};
    if (sizeReadFailed || fileSize == 0 || fileSize > transfer.request.maxBytes
        || fileSize > saveFileHardLimit) {
        std::error_code cleanupError;
        std::filesystem::remove(partPath, cleanupError);
        failure = sizeReadFailed ? SaveFailureV2::FileIo : SaveFailureV2::TooLarge;
        return false;
    }

    std::filesystem::remove(transfer.savePath, error);
    error.clear();
    std::filesystem::rename(partPath, transfer.savePath, error);
    if (error) {
        std::filesystem::remove(partPath, error);
        failure = SaveFailureV2::FileIo;
        return false;
    }

    return true;
}

bool roleMatchesClient(SaveRoleV2 role,
                       const CNetCustomSession* session,
                       const game::CMidgard* midgard)
{
    const bool sessionHost{session->isHost()};
    const bool midgardHost{midgard->data->host};
    if (sessionHost != midgardHost) {
        spdlog::warn(__FUNCTION__ ": refusing save request while host state is inconsistent");
        return false;
    }
    return role == (sessionHost ? SaveRoleV2::Host : SaveRoleV2::Joiner);
}

} // namespace

void observeLobbySaveGameMessage(const game::NetMessageHeader* message,
                                 std::size_t availableBytes)
{
    static constexpr char refreshInfoClass[]{".?AVCRefreshInfo@@"};
    if (!message || availableBytes < sizeof(game::NetMessageHeader) + sizeof(std::uint32_t)
        || availableBytes >= game::netMessageMaxLength
        || message->length < sizeof(game::NetMessageHeader) + sizeof(std::uint32_t)
        || message->length > availableBytes || message->length >= game::netMessageMaxLength
        || std::memchr(message->messageClassName, '\0', sizeof(message->messageClassName)) == nullptr
        || std::strcmp(message->messageClassName, refreshInfoClass) != 0) {
        return;
    }

    game::CMidgardID expansionMarker{};
    const auto fromString{game::CMidgardIDApi::get().fromString};
    if (!fromString) {
        return;
    }
    fromString(&expansionMarker, "G25500FFFF");

    std::uint32_t firstId{};
    std::memcpy(&firstId,
                reinterpret_cast<const std::uint8_t*>(message)
                    + sizeof(game::NetMessageHeader),
                sizeof(firstId));
    const bool observed{firstId == static_cast<std::uint32_t>(expansionMarker.value)};
    const auto scenarioOffset{sizeof(game::NetMessageHeader)
                              + (observed ? 2u : 1u) * sizeof(std::uint32_t)};
    if (availableBytes < scenarioOffset || message->length < scenarioOffset) {
        return;
    }

    std::uint32_t scenarioId{firstId};
    if (observed) {
        std::memcpy(&scenarioId,
                    reinterpret_cast<const std::uint8_t*>(message)
                        + sizeof(game::NetMessageHeader) + sizeof(std::uint32_t),
                    sizeof(scenarioId));
    }
    game::CMidgardID typedScenarioId{static_cast<int>(scenarioId)};
    const auto getIdType{game::CMidgardIDApi::get().getType};
    if (!getIdType || getIdType(&typedScenarioId) != game::IdType::ScenarioFile) {
        return;
    }

    clientExpansionScenarioId = typedScenarioId.value;
    clientExpansionContent = observed ? 1 : 0;
}

void sendLobbySaveFailure(std::uint64_t saveId, LobbyProtocol::SaveFailureV2 failure)
{
    SLNet::BitStream stream;
    if (!sendSaveDataPrefix(stream, saveId, LobbyProtocol::SaveDataOperationV2::Fail)) {
        return;
    }
    stream.Write(static_cast<std::uint8_t>(failure));
    sendSaveData(stream);
}

void handleLobbySaveStoredAck(std::uint64_t saveId)
{
    if (!isMainThread() || saveId == 0) {
        spdlog::warn(__FUNCTION__ ": refusing stored ACK outside the main/UI thread or with zero id");
        return;
    }

    for (std::size_t index = 0; index < pendingStoredAcks.size(); ++index) {
        auto& pending{pendingStoredAcks[index]};
        if (!pending || pending->saveId != saveId) {
            continue;
        }

        std::error_code error;
        const bool removed{std::filesystem::remove(pending->savePath, error)};
        if (error) {
            spdlog::warn(__FUNCTION__
                         ": could not remove acknowledged local save '{:s}', error {:d}; "
                         "retaining file",
                         pending->savePath.string(), error.value());
        } else if (removed) {
            spdlog::info(__FUNCTION__
                         ": removed acknowledged local save '{:s}', id {:016x}",
                         pending->savePath.string(), saveId);
        }

        pending.reset();
        return;
    }

}

void handleLobbySaveRequest(const LobbyProtocol::SaveRequestV2& request)
{
    using namespace LobbyProtocol;

    if (!isMainThread()) {
        spdlog::warn(__FUNCTION__ ": refusing capture outside the main/UI thread");
        sendLobbySaveFailure(request.saveId, SaveFailureV2::UnsafePhase);
        return;
    }
    if (gameVersion() != GameVersion::Russobit) {
        sendLobbySaveFailure(request.saveId, SaveFailureV2::UnsupportedGameBuild);
        return;
    }
    expireLobbySaveTransfers();
    if (!isKnownRole(request.role) || request.saveId == 0 || request.maxBytes == 0
        || request.maxBytes > saveFileHardLimit || request.timeoutMs == 0) {
        sendLobbySaveFailure(request.saveId, SaveFailureV2::MalformedRequest);
        return;
    }
    auto& pendingAck{pendingStoredAcks[roleIndex(request.role)]};
    if (pendingAck) {
        if (pendingAck->saveId == request.saveId) {
            return;
        }
        sendLobbySaveFailure(request.saveId, SaveFailureV2::Busy);
        return;
    }

    auto& active{activeTransfers[roleIndex(request.role)]};
    if (active) {
        if (active->request.saveId == request.saveId) {
            return;
        }
        sendLobbySaveFailure(request.saveId, SaveFailureV2::Busy);
        return;
    }

    auto service{CNetCustomService::get()};
    auto midgard{game::CMidgardApi::get().instance()};
    auto session{service ? service->getSession() : nullptr};
    if (!service || !session || !midgard || !midgard->data || !midgard->data->multiplayerGame
        || !midgard->data->client) {
        sendLobbySaveFailure(request.saveId, SaveFailureV2::NoActiveGame);
        return;
    }
    if (!roleMatchesClient(request.role, session, midgard)) {
        sendLobbySaveFailure(request.saveId, SaveFailureV2::WrongRole);
        return;
    }

    SaveTransferSession transfer{};
    transfer.request = request;
    transfer.saveName = makeSaveName(request.role, request.saveId);
    transfer.savePath = makeSavePath(midgard, transfer.saveName);
    transfer.deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(request.timeoutMs);

    if (request.role == SaveRoleV2::Host) {
        const auto sendSaveGameMsg{game::CPhaseGameApi::get().sendSaveGameMsg};
        if (!sendSaveGameMsg) {
            sendLobbySaveFailure(request.saveId, SaveFailureV2::UnsupportedGameBuild);
            return;
        }
        const auto phaseGame{getHostPhaseGame(midgard)};
        if (!phaseGame) {
            sendLobbySaveFailure(request.saveId, SaveFailureV2::UnsafePhase);
            return;
        }

        active.emplace(std::move(transfer));
        spdlog::info(__FUNCTION__ ": requesting independent host save '{:s}', id {:016x}",
                     active->saveName, request.saveId);
        try {
            // false is the native non-UI/autosave form. The wrapper's true form owns an
            // additional UI-lock increment which a direct CPhaseGame call must not request.
            sendSaveGameMsg(phaseGame, active->saveName.c_str(), false);
        } catch (...) {
            spdlog::warn(__FUNCTION__ ": native host save builder raised a C++ exception");
            const auto saveId{request.saveId};
            finishTransfer(request.role);
            sendLobbySaveFailure(saveId, SaveFailureV2::CaptureFailed);
        }
        return;
    }

    active.emplace(std::move(transfer));
    SaveFailureV2 failure{SaveFailureV2::CaptureFailed};
    const bool captured{captureJoinerSave(*active, failure)};
    const bool uploaded{captured && uploadSave(*active, failure)};
    if (uploaded) {
        awaitStoredAck(request.role);
    } else {
        finishTransfer(request.role);
        sendLobbySaveFailure(request.saveId, failure);
    }
}

bool hasActiveLobbyHostSaveTransfer()
{
    return isMainThread() && activeTransfers[roleIndex(SaveRoleV2::Host)].has_value();
}

void handleGameSavedForLobby(bool success, const std::string& savePath)
{
    using namespace LobbyProtocol;

    auto& active{activeTransfers[roleIndex(SaveRoleV2::Host)]};
    if (!active) {
        return;
    }
    if (!isExpectedHostSaveResultPath(savePath, active->savePath)) {
        return;
    }

    auto service{CNetCustomService::get()};
    auto midgard{game::CMidgardApi::get().instance()};
    auto session{service ? service->getSession() : nullptr};
    SaveFailureV2 failure{SaveFailureV2::CaptureFailed};
    if (!service || !session || !midgard || !midgard->data || !session->isHost()
        || !midgard->data->host) {
        failure = SaveFailureV2::WrongRole;
    } else if (isDeadlineExpired(*active)) {
        failure = SaveFailureV2::TimedOut;
    } else if (!success) {
        failure = SaveFailureV2::CaptureFailed;
    } else if (uploadSave(*active, failure)) {
        awaitStoredAck(SaveRoleV2::Host);
        return;
    }

    const auto saveId{active->request.saveId};
    finishTransfer(SaveRoleV2::Host);
    sendLobbySaveFailure(saveId, failure);
}

void expireLobbySaveTransfers()
{
    if (!isMainThread()) {
        return;
    }

    for (std::size_t index = 0; index < activeTransfers.size(); ++index) {
        auto& active{activeTransfers[index]};
        if (!active || !isDeadlineExpired(*active)) {
            continue;
        }

        const auto role{static_cast<SaveRoleV2>(index)};
        const auto saveId{active->request.saveId};
        spdlog::warn(__FUNCTION__ ": lobby save {:016x}, role {:d}, timed out",
                     saveId, static_cast<int>(role));
        finishTransfer(role);
        sendLobbySaveFailure(saveId, SaveFailureV2::TimedOut);
    }

    for (std::size_t index = 0; index < pendingStoredAcks.size(); ++index) {
        auto& pending{pendingStoredAcks[index]};
        if (!pending || std::chrono::steady_clock::now() < pending->deadline) {
            continue;
        }

        const auto role{static_cast<SaveRoleV2>(index)};
        spdlog::warn(__FUNCTION__
                     ": stored ACK for local save {:016x}, role {:d}, timed out; retaining file",
                     pending->saveId, static_cast<int>(role));
        pending.reset();
    }
}

void terminateLobbySaveTransfers()
{
    for (std::size_t index = 0; index < activeTransfers.size(); ++index) {
        auto& active{activeTransfers[index]};
        if (!active) {
            continue;
        }

        const auto role{static_cast<SaveRoleV2>(index)};
        const auto saveId{active->request.saveId};
        // MATCH_ENDED is sent after the server's authoritative 30-second window. The client's
        // local deadline starts later (on packet receipt), so comparing it here could defer the
        // transition forever. Terminally fail any remaining transfer and trust the server.
        finishTransfer(role);
        sendLobbySaveFailure(saveId, SaveFailureV2::TimedOut);
    }

    for (std::size_t index = 0; index < pendingStoredAcks.size(); ++index) {
        auto& pending{pendingStoredAcks[index]};
        if (!pending) {
            continue;
        }

        const auto role{static_cast<SaveRoleV2>(index)};
        spdlog::info(__FUNCTION__
                     ": forgetting stored ACK wait for {:016x}, role {:d}; retaining local file",
                     pending->saveId, static_cast<int>(role));
        pending.reset();
    }
}

void resetLobbySaveTransferState()
{
    for (auto& active : activeTransfers) {
        active.reset();
    }
    for (auto& pending : pendingStoredAcks) {
        pending.reset();
    }
    clientExpansionContent = -1;
    clientExpansionScenarioId = std::numeric_limits<int>::min();
}

} // namespace hooks
