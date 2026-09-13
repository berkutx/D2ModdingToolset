// Replays the shipped timer against host events; no game, hooks, renderer or sound device.
#include <windows.h>
#include <cstdio>
static DWORD replayNow;
static DWORD WINAPI replayGetTickCount() { return replayNow; }
#define GetTickCount replayGetTickCount
#include "../plugins/timer/timer.cpp"
#undef GetTickCount

static C4P_Host replayHost;
static C4P_BattleTimerState replayBattle;
static uint32_t replaySerial, replayAck;
static int replayActive, replayRole, replayDay, replayPostBattle;
static int endCalls, postBattleReads, failures, checks;
static int __cdecl one() { return 1; }
static void __cdecl invalidate() {}
static const char* __cdecl configPath() { return nullptr; }
static uint32_t __cdecl serial() { return replaySerial; }
static uint32_t __cdecl ack() { return replayAck; }
static int __cdecl active() { return replayActive; }
static int __cdecl role() { return replayRole; }
static int __cdecl day() { return replayDay; }
static int __cdecl kind() { return replayBattle.battle_kind; }
static int __cdecl battle(C4P_BattleTimerState* out) { *out = replayBattle; return 1; }
static int __cdecl cancel() { return 1; }
static int __cdecl end() { ++endCalls; return 1; }
static int __cdecl postBattle() { ++postBattleReads; return replayPostBattle; }
static void check(bool value, const char* name) {
    ++checks;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
static void tick(DWORD now) { replayNow = now; c4p_tick(0xFFFFFFFF); }
static int64_t remaining() {
    return static_cast<int64_t>(budgetMsForDay(g_curDayBudget)) + g_extra -
        static_cast<uint32_t>((g_paused ? g_pausedAt : replayNow) - g_baseline);
}
static void reset(int serverRole = 0, bool legacyHost = false) {
    replayHost.struct_size = legacyHost ? offsetof(C4P_Host, post_battle_pending) : sizeof(replayHost);
    g_host = &replayHost;
    replayActive = 1; replayRole = serverRole; replaySerial = 3; replayAck = 1;
    replayDay = 3; replayPostBattle = 0; replayNow = 0; endCalls = postBattleReads = 0;
    replayBattle = {}; replayBattle.struct_size = sizeof(replayBattle);
    replayBattle.battle_instance = 1; replayBattle.local_active = 1;
    replayBattle.playback_local = -1;
    g_state = 2; g_pauseOn = 0; g_pauseAnim = 1; g_turnDay = 1; g_autoBattle = 0;
    g_durBase = 90; g_alwaysVisible = 1; g_running = 0;
    g_warningSound = 0; g_warningFired = false;
    g_paused = g_manualPaused = g_userPaused = g_policyPaused = 0;
    g_baseline = g_pausedAt = 0; g_extra = 0; g_resetExtra = 0;
    g_lastPlayer = -1; g_wasActive = g_turnAccepted = g_manualSetPending = 0;
    g_elapseFired = g_expired = g_pvpClampPending = g_offTurnPvpExhausted = 0;
    g_pvpClampInstance = g_pvpHandledInstance = g_pvpHandledGeneration = UINT32_MAX;
    g_offTurnPvpPlayer = g_offTurnPvpDay = -1;
    g_postBattleBilling = g_observedBattleKind = 0; g_observedBattleInstance = 0;
    g_curDayBudget = 0; g_lastSerial = 0; g_lastBeginTurnAck = 0;
    bankClear();
    tick(0);
}
static void finishBattle(DWORD now) {
    replayBattle.battle_kind = 0;
    ++replayBattle.battle_instance;
    replayPostBattle = 1;
    tick(now);
}

int main() {
    InitializeCriticalSection(&g_lock);
    replayHost.invalidate = invalidate; replayHost.config_path = configPath;
    replayHost.get_turn_serial = serial; replayHost.get_turn_player = one;
    replayHost.is_in_game = one; replayHost.get_day = day;
    replayHost.turn_active = active; replayHost.turn_player_id = one;
    replayHost.begin_turn_ack_serial = ack; replayHost.server_role = role;
    replayHost.battle_kind = kind; replayHost.get_battle_timer_state = battle;
    replayHost.cancel_elapse = cancel; replayHost.end_day = end;
    replayHost.post_battle_pending = postBattle;

    // Log 14036: off6 serial stays 3, PvE ends, host retires transition, player ends the turn.
    reset();
    replayBattle.battle_kind = 2; tick(1000);
    replayActive = 0; finishBattle(2000); tick(4000);
    check(g_turnAccepted && !g_paused && remaining() == 86000,
          "reward interval still bills the accepted turn while strategic flag is temporarily zero");
    replayActive = 1; tick(5000);
    replayPostBattle = 0; tick(5100);
    check(!g_postBattleBilling, "native stable-strategy event retires plugin post-battle billing");
    replayActive = 0; tick(6000); tick(120000);
    check(g_paused && !g_turnAccepted && remaining() == 84000,
          "opponent time is not billed when the turn serial does not change");
    check(endCalls == 0, "no timeout is consumed during the opponent turn");
    replayActive = 1; replayDay = 4; tick(121000);
    check(g_turnAccepted && !g_paused && g_curDayBudget == 4 && remaining() == 174000,
          "joiner opens next day once, adding 90 seconds to its 84-second bank");
    tick(121001);
    check(remaining() == 173999, "stable own-turn samples do not grant duplicate budgets");
    tick(295001); tick(296000);
    check(endCalls == 1 && remaining() == -1000,
          "next own-turn timeout queues End Day once and retains signed overtime");

    reset();
    replayBattle.battle_kind = 2; tick(1000);
    replayActive = 0; finishBattle(2000); tick(95000);
    check(endCalls == 1 && remaining() == -5000 && !g_paused,
          "long PvE reward UI still accrues signed debt and queues its timeout");
    replayActive = 1; replayPostBattle = 0; tick(96000);
    replayActive = 0; tick(97000); tick(110000);
    check(remaining() == -7000 && g_paused, "post-battle debt freezes at actual turn end");
    replayActive = 1; replayDay = 4; tick(111000);
    check(remaining() == 83000 && !g_elapseFired,
          "next day carries debt and rearms its timeout");

    reset(1);
    replayBattle.battle_kind = 2; tick(1000); finishBattle(2000);
    replayPostBattle = 0; tick(3000);
    replayActive = 0; tick(4000); tick(100000);
    check(remaining() == 86000 && g_paused, "host also freezes after post-PvE turn end");
    replayActive = 1; replayDay = 4; ++replayAck; tick(101000);
    check(remaining() == 176000 && g_curDayBudget == 4,
          "host acknowledgement still opens the next bank");

    reset();
    replayBattle.battle_kind = 2; tick(1000);
    // Both teardown and native retirement happened between worker polls.
    replayBattle.battle_kind = 0; ++replayBattle.battle_instance;
    replayPostBattle = 0; tick(3000);
    check(!g_postBattleBilling, "already retired transition cannot recreate a stale billing latch");

    reset(0, true);
    replayBattle.battle_kind = 2; tick(1000); finishBattle(2000);
    replayActive = 0; ++replaySerial; tick(3000);
    check(postBattleReads == 0 && !g_postBattleBilling && g_paused,
          "1.9.2 host ABI is not read past its size; legacy serial boundary remains supported");

    for (int serverRole = 0; serverRole <= 1; ++serverRole) {
        reset(serverRole);
        tick(300000); // 90-second grant is overdrawn by 210 seconds while a modal is open.
        check(endCalls == 1 && remaining() == -210000,
              "modal overtime queues one timeout and retains the entire signed debt");
        replayActive = 0; tick(300000); tick(500000);
        replayActive = 1; replayDay = 4; ++replayAck; tick(500001);
        check(endCalls == 2 && remaining() == -120000 && g_elapseFired,
              "host/joiner queues skip on the very first tick of an already negative new day");
        tick(500001);
        check(endCalls == 2, "negative new day cannot enqueue duplicate timeouts");
        replayActive = 0; tick(500001); tick(700000);
        replayActive = 1; replayDay = 5; ++replayAck; tick(700001);
        check(endCalls == 3 && remaining() == -30000,
              "debt spanning another full day rearms and immediately queues the next skip");
        replayActive = 0; tick(700001); tick(900000);
        replayActive = 1; replayDay = 6; ++replayAck; tick(900001);
        check(endCalls == 3 && remaining() == 60000 && !g_elapseFired,
              "first positive bank after repaying debt starts normally without a stale skip");

        reset(serverRole); tick(180000);
        replayActive = 0; tick(180000); tick(200000);
        replayActive = 1; replayDay = 4; ++replayAck; tick(200001);
        check(endCalls == 2 && remaining() == 0,
              "new day with exactly zero remaining also queues its timeout immediately");
    }

    std::printf("Timer post-battle replay: %d checks, %d failures\n", checks, failures);
    DeleteCriticalSection(&g_lock);
    return failures ? 1 : 0;
}
