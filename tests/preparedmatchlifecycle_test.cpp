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

        for (auto joinStage : {JoinStage::Waiting, JoinStage::CheckingRoom, JoinStage::Prompt,
                              JoinStage::Accepted, JoinStage::CheckingJoinRoom, JoinStage::Terminal})
            require(joinAction(joinStage, false, true, true) == JoinAction::Wait,
                    "join prompt/transition interrupted a busy game, modal or menu transition");
        require(joinAction(JoinStage::Waiting, true, false, false) == JoinAction::RefreshRooms,
                "first idle lobby did not request its fresh room list");
        require(joinAction(JoinStage::CheckingRoom, true, false, false) == JoinAction::Wait,
                "not-yet-loaded room list rejected a valid queued invitation");
        require(joinAction(JoinStage::CheckingRoom, true, true, true) == JoinAction::ShowPrompt,
                "fresh available room did not request explicit Yes/No");
        require(joinAction(JoinStage::Accepted, true, true, true) == JoinAction::RefreshRooms,
                "Yes joined using the stale pre-modal room list");
        require(joinAction(JoinStage::CheckingJoinRoom, true, false, true) == JoinAction::Wait,
                "Yes joined before refreshing the current room");
        require(joinAction(JoinStage::CheckingJoinRoom, true, true, true) == JoinAction::Join,
                "explicit Yes and fresh room did not use ordinary joining");
        for (auto joinStage : {JoinStage::CheckingRoom, JoinStage::CheckingJoinRoom})
            require(joinAction(joinStage, true, true, false) == JoinAction::Unavailable,
                    "missing/replaced/full room was not rejected");
        require(joinAction(JoinStage::Terminal, true, true, true) == JoinAction::Wait,
                "terminal answer retriggered the join");
        require(joinStageAfterBusy(JoinStage::CheckingRoom) == JoinStage::Waiting
            && joinStageAfterBusy(JoinStage::CheckingJoinRoom) == JoinStage::Accepted,
                "busy modal/game time expired an invitation instead of requesting fresh rooms on return");
        JoinReceipts receipts;
        const JoinIdentity target{{"prep", "game", "attempt", 7}, 17};
        receipts.remember(target, "Alice", JoinState::Busy);
        require(!receipts.find(target, "Alice"), "temporary busy state became a terminal receipt");
        receipts.remember(target, "Alice", JoinState::Declined);
        require(receipts.find(target, "Alice") == JoinState::Declined,
                "decline was not remembered independently of transient queue/login state");
        receipts.remember(target, "Alice", JoinState::Unavailable);
        require(receipts.find(target, "Alice") == JoinState::Declined, "withdrawal overwrote explicit decline");
        require(!receipts.find(target, "Bob"), "one account's receipt suppressed another account");
        auto next = target; ++next.roomId;
        require(!receipts.find(next, "Alice"), "a different room inherited old consent");
        next = target; ++next.identity.revision;
        require(!receipts.find(next, "Alice"), "a different attempt revision inherited old consent");

        std::cout << "prepared lifecycle: pre-create ACK, generation barrier, Cancel/CreateRoom crossing, "
                     "late Setup cancel, created game and return cleanup passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
