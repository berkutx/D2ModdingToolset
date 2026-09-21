#ifndef PREPAREDMATCHLIFECYCLE_H
#define PREPAREDMATCHLIFECYCLE_H

namespace hooks::prepared {
enum class Stage { Waiting, Confirming, Accepted, Generating, Creating, Setup, Terminal, Returning };
enum class CancelAction { Acknowledge, AwaitSafePoint, PreserveRoom };

inline CancelAction cancelAction(Stage stage, bool roomCreated)
{
    if (stage == Stage::Generating || stage == Stage::Creating)
        return CancelAction::AwaitSafePoint;
    if (stage == Stage::Setup || roomCreated) return CancelAction::PreserveRoom;
    return CancelAction::Acknowledge;
}

inline CancelAction requestCancellation(Stage stage, bool roomCreated, bool& canceled)
{
    const auto action = cancelAction(stage, roomCreated);
    // A created room keeps its agreed native setup. Only a pending creation can
    // still be canceled; do not latch a new cancellation during Setup or play.
    if (action != CancelAction::PreserveRoom) canceled = true;
    return action;
}

inline Stage stageAfterRoomCreationResult(bool success, bool& canceled)
{
    if (!success) return Stage::Returning;
    // CreateRoom won the crossing with Cancel. Its successful result settles the
    // creation barrier, so the earlier request must not suppress the host's lord.
    canceled = false;
    return Stage::Setup;
}

inline Stage stageAfterCanceledAck(Stage stage)
{
    // A late/duplicate cancel must not discard the pending native menu cleanup.
    return stage == Stage::Returning ? Stage::Returning : Stage::Terminal;
}
} // namespace hooks::prepared
#endif
