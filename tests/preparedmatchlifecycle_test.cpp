#include "preparedmatchlifecycle.h"
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try {
        using namespace hooks::prepared;
        for (auto stage : {Stage::Waiting, Stage::Confirming, Stage::Accepted, Stage::Terminal}) {
            require(cancelAction(stage, false) == CancelAction::Acknowledge,
                    "cancel before creation did not ACK");
            require(stageAfterCanceledAck(stage) == Stage::Terminal,
                    "cancel before creation was not terminal");
        }
        for (auto stage : {Stage::Generating, Stage::Creating})
            require(cancelAction(stage, false) == CancelAction::AwaitSafePoint,
                    "cancel ACK crossed generation/create barrier");
        require(cancelAction(Stage::Setup, false) == CancelAction::PreserveRoom,
                "late cancel would interrupt native setup");
        require(cancelAction(Stage::Terminal, true) == CancelAction::PreserveRoom,
                "late cancel would forget a created room");
        require(cancelAction(Stage::Returning, false) == CancelAction::Acknowledge,
                "return cleanup prevented cancel ACK");
        auto returning = stageAfterCanceledAck(Stage::Returning);
        require(returning == Stage::Returning && stageAfterCanceledAck(returning) == Stage::Returning,
                "late or repeated cancel dropped deferred generator cleanup");

        // Actual coordinator decisions: Cancel crosses an already-sent CreateRoom,
        // then its successful response must still allow native race/lord setup.
        auto stage = Stage::Creating;
        bool canceled = false;
        require(requestCancellation(stage, false, canceled) == CancelAction::AwaitSafePoint && canceled,
                "in-flight CreateRoom cancellation did not wait for its real result");
        stage = stageAfterRoomCreationResult(true, canceled);
        require(stage == Stage::Setup && !canceled,
                "Cancel/CreateRoom crossing suppressed the agreed host setup");
        require(requestCancellation(stage, true, canceled) == CancelAction::PreserveRoom && !canceled,
                "Cancel during Setup suppressed the agreed host lord");
        require(stage == Stage::Setup, "Cancel during Setup changed the native transition");
        stage = Stage::Terminal;
        require(requestCancellation(stage, true, canceled) == CancelAction::PreserveRoom && !canceled,
                "Cancel after setup would interrupt the created game");

        for (auto early : {Stage::Waiting, Stage::Confirming, Stage::Accepted}) {
            canceled = false;
            require(requestCancellation(early, false, canceled) == CancelAction::Acknowledge && canceled,
                    "early cancellation no longer latched the creation barrier");
            require(stageAfterCanceledAck(early) == Stage::Terminal,
                    "early cancellation did not finish without creating a room");
        }
        canceled = false;
        require(requestCancellation(Stage::Generating, false, canceled) == CancelAction::AwaitSafePoint && canceled,
                "generation cancellation no longer blocks preview Accept");
        stage = Stage::Creating;
        require(requestCancellation(stage, false, canceled) == CancelAction::AwaitSafePoint,
                "failed creation crossing acknowledged cancellation before the result");
        stage = stageAfterRoomCreationResult(false, canceled);
        require(stage == Stage::Returning && canceled,
                "failed creation crossing started setup or discarded pending cancellation");

        std::cout << "prepared lifecycle: pre-create ACK, generation barrier, Cancel/CreateRoom crossing, "
                     "late Setup cancel, created game and return cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
