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

inline Stage stageAfterCanceledAck(Stage stage)
{
    // A late/duplicate cancel must not discard the pending native menu cleanup.
    return stage == Stage::Returning ? Stage::Returning : Stage::Terminal;
}
} // namespace hooks::prepared
#endif
