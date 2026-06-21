# C4dll-R backlog — deferred / not yet implemented

Living tracker of what is NOT done yet. Add new items here instead of letting them get lost in chat.
Status legend: `[ ]` todo · `[~]` partial · `[!]` known bug. Most timer items hang off the **keystone**
(the legacy `off[]` dialog/turn hook layer) — see `repo/mss32/.../timer-host-event-addrs` (memory) and
the fidelity audit. Approach agreed with the user: **transcribe the working legacy `timer.mod` 1:1**
(addresses already extracted), not redesign.

## Known bugs (renderer / menu)
- `[!]` **Resolution not applied live** (cnc-ddraw `width`/`height` via the Video menu): the window
  grows but the render is not rescaled (black expansion border) and the game's edge-scroll / mouse
  mapping stays on the OLD boundaries.
  - Cause (from `dd.c` `dd_SetDisplayMode`): `DDReloadConfig` calls `dd_SetDisplayMode(0,0,0,0)` which
    keeps the game's internal res and sets `g_ddraw.render.width/height` to the new window size, then
    restarts the render thread — but the windowed upscale viewport + mouse coord scale are not fully
    recomputed live, AND cnc-ddraw subtracts the menu-bar height (`SM_CYMENU`) from the render area
    ONLY in fullscreen-windowed (dd.c:748), not in normal windowed. Our added menu bar therefore
    throws off the windowed render/input geometry when the window is resized.
  - Fix (needs in-game testing): recompute the windowed upscale viewport + mouse scale in
    `DDReloadConfig`, and account for the menu bar in the windowed render area; OR, as an honest
    interim, relabel Resolution "(restart)" like Renderer / Frame cap. Verify edge-scroll boundaries
    and that the render fills the client area after a live change.

## Timer — host-event layer (the keystone)
Ported into the HOST module `features/timerhost.cpp` (installed from `featuremenu_install`, Russobit).
Full implementation spec from the keystone-RE workflow (in the task output + memory). Build order:
capture/observe (done) -> turn/day -> gated game-CALL actions.
- `[x]` **Dialog-capture keystone (Phase 1a)** — `DetourAttach` off[9]=`0x5C93D6` (verified __stdcall
  `f(iface, btnName, dlgName, ...)`) captures DLG_STRATEGIC/BTN_END_TURN + DLG_BATTLE_*/BTN_* + capital/
  diplomacy/briefing buttons; off[8]=`0x6E3294` CButtonInterf vtable[0] nulls them on destroy; off[5]=
  `0x6CEB5C` CMidClient vtable[0] resets on scenario change. Boot-verified clean.
- `[x]` **Combat Pause PvP/PvAny** — DONE: `battle_kind()` (0/PvP/any) from the off[9] BTN_DEFEND PvP
  walk (`[[[ [iface+4]+8]+0x1C]+0x14F8]`); plugin pauses per PauseOn 1=PvP / 2=any.
- `[x]` **Animation Pause** — DONE: `is_animating()` = the BTN_DEFEND hidden byte (attack animation
  playing); plugin freezes the Force clock per PauseAnimation, mirroring `sub_100034A0`.
- `[ ]` **Precise turn-start + player id (Phase 1b)** — off[6]=`0x48A69A` (verify callee sig) → set
  `turn_active`/`turn_player_id`; off[7]=`0x48FDFE` per-frame clear. Refines the `currentPlayerIndex` poll.
- `[ ]` **On Day Start/End + extra-time accumulation (Phase 1b)** — off[7] DayTurn bits + off[16]=
  `0x489AB3` day-reset; the per-player extra-time bank keyed on `turn_player_id` (plugin owns durations).
- `[ ]` **inCombat refinement (Phase 1b)** — off[14]=`0x48D162` / off[15]=`0x540B48` for the precise
  in-combat-for-our-player flag (currently approximated by is_in_battle).
- `[ ]` **On Elapse — End Day / Retreat (Phase 2, RISKY game vtable CALLS, gated)** — `timerhost_end_day`
  presses the first enabled captured button via CButtonInterf vtable+0xB0; `timerhost_retreat` via
  AUTOBATTLE +0x8C / RESOLVE +0xA0. Guards: `[obj+8]+4` enabled byte, re-entry guard, SEH, game-thread
  marshal (pending flag consumed in off[7]), `actionsEnabled` gate default OFF. Stubs return 0 today.

## Timer — dialogs (resource ports)
- `[x]` **Timetable dialog** (legacy **res 5** / sub_100044E0) — DONE: Day/Duration grid (timer.rc
  IDD_TIMETABLE + timetableDlgProc), wired to Force Turn Mode > Timetable... (cmd +0x13). Edits
  (no spinners yet — polish). Menu IDs corrected to the authoritative map (decompiled sub_10004D40):
  +0x13 Timetable, +0x15 Help, +0x16 About; there is NO "Position" dialog (the review was wrong).
- `[ ]` **Set dialog** (legacy **res 6** / DialogFunc, cmd +0x04) — one "minutes" field; sets the
  current turn's remaining time (baseline = now - minutes, + day budget + clears extra in Force mode).
- `[ ]` **About dialog** (legacy res 4 / sub_10004A90, cmd +0x16) — version + author SysLinks;
  currently a plain MessageBox (good enough, low priority).
- `[ ]` **Drag-to-move** the on-screen timer (legacy WndProc mouse: dword_10008048 Alt-held +
  WM_LBUTTONDOWN -> fractional anchor flt_10008120/8124). Not a dialog. Replaces the removed "Position".

NOTE: idalib decompile RESTORED for timer.mod (fresh full idat .i64); all legacy functions now
decompile, so the rest is direct transcription.

## Timer — minor fidelity (from the audit)
- `[ ]` **Extra-time carry** — `dword_10008068` per-day budget table; count-down = `budget + extra -
  elapsed` (we use a flat budget). Tied to the Set / Timetable dialogs.
- `[ ]` **Colour ramp 3-tier** — legacy has blink-red / over-budget / in-budget (+0x80 paused alpha);
  our `pickColors` is 2-tier. Not pixel-identical.
- `[ ]` **FontSize / Offset format** — `FontSize` is our addition (legacy has none); `OffsetX/Y` are
  legacy float 0.0..1.0 anchors, our `AnchorX/Y` are int 0..100. Cosmetic / config-format only.

## DGL features
- `[ ]` **Map drag-scroll** (faithful) — DGL `MouseScroll` = grab + drag pan (`sub_10017020` hooks the
  iso-view mouse handler). After the timer. See memory `dgl-map-drag-scroll`.

## Release / CI
- `[ ]` **Push C4dll-R** — the whole `c4ddraw/` + workflows are uncommitted (user chose local for now).
  When ready: first commit + push to origin, then tag `c4dll-r-v0.1` → `c4dll-r-release.yml` publishes
  the drop-in zip. CI itself is clean (the mss32 track never shipped the menu — `skip-worktree`).

## Done recently (for context, not a TODO)
- Animation speed: live + separate battle/map multipliers up to 5x (timeGetTime virtual clock).
- Combat Pause (any battle); fidelity fixes (DayTurn clamp, menu tail IDs, Force-mode ACTIVE gate).
- Timetable per-day duration + the day source (`C4P_Host.get_day` → `CScenarioInfo.currentTurn`).
- First-run `C4menu.ini` converter; mss32 featuremenu removed (the crashing const-patch); adapter cleanup.
