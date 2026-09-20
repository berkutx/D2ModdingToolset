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
        std::cout << "prepared lifecycle: pre-create ACK, generation barrier, created room and return cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
