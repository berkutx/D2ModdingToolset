// Private process only: ExitProcess is replaced by a recorder. The source
// contract suite compares this function body with rendererbridge.c to prevent
// a stale copied implementation from passing. No Win32/game/UI access here.
#include <cstdio>

struct FakeConfig {
    int save_settings;
    int terminate_process;
    int width;
    int maxgameticks;
    int singlecpu;
};
FakeConfig g_config = {};
int exitCalls = 0;
unsigned exitCode = 99;

void recordExit(unsigned code)
{
    ++exitCalls;
    exitCode = code;
}
#define ExitProcess recordExit

void DDExitClientAfterSettingsChange(int discard_old_window_state)
{
    if (discard_old_window_state)
        g_config.save_settings = 0;
    if (g_config.terminate_process)
        g_config.terminate_process = 2;
    ExitProcess(0);
}

#undef ExitProcess

int main()
{
    int passed = 0;
    int failed = 0;
    const int settings[] = { 0, 1, 2, -1 };
    for (int discard = 0; discard <= 1; ++discard) {
        for (int save : settings) {
            for (int termination : settings) {
                g_config = { save, termination, 1337, 180, 1 };
                exitCalls = 0;
                exitCode = 99;
                DDExitClientAfterSettingsChange(discard);
                const bool correctSave = g_config.save_settings == (discard ? 0 : save);
                const bool correctTermination = g_config.terminate_process == (termination ? 2 : 0);
                const bool correctExit = exitCalls == 1 && exitCode == 0;
                const bool untouched = g_config.width == 1337 && g_config.maxgameticks == 180 && g_config.singlecpu == 1;
                const bool checks[] = { correctSave, correctTermination, correctExit, untouched };
                for (bool check : checks) {
                    if (check) ++passed; else ++failed;
                }
                std::printf("%s: discard=%d savesettings=%d terminate=%d; save_gate=%d upstream_exit=%d exit_once=%d unrelated_unchanged=%d\n",
                    correctSave && correctTermination && correctExit && untouched ? "PASS" : "FAIL",
                    discard, save, termination, correctSave, correctTermination, correctExit, untouched);
            }
        }
    }
    std::printf("RESULT: %d passed, %d failed; fake ExitProcess only, no real process termination.\n", passed, failed);
    return failed ? 1 : 0;
}
