# C4dll-R backlog — deferred / not yet implemented

Living tracker of what is NOT done yet. Add new items here instead of letting them get lost in chat.
Status legend: `[ ]` todo · `[~]` partial · `[!]` known bug. Most timer items hang off the **keystone**
(the legacy `off[]` dialog/turn hook layer) — see `repo/mss32/.../timer-host-event-addrs` (memory) and
the fidelity audit. Approach agreed with the user: **transcribe the working legacy `timer.mod` 1:1**
(addresses already extracted), not redesign.

## Known bugs (renderer / menu)
- `[x]` **Resolution not applied live** (black expansion border + edge-scroll on old bounds) - FIXED
  (pending the user's visual confirm). The BACKLOG's original guess (menu-height/viewport not
  recomputed) was WRONG: a multi-agent source audit proved `dd_SetDisplayMode` IS coherent and DOES
  rescale render+viewport+mouse+swapchain in one pass. The real cause was OUR OWN `featuremenu.cpp`
  `menuWorker`/`ensureChrome`: it grew the window by `SM_CYMENU` via a raw `SetWindowPos` on a 1500ms
  poll, OUT-OF-BAND and ~1.5s AFTER `dd_SetDisplayMode`. That async grow fires a Win32 `WM_SIZE` which
  cnc-ddraw treats as a geometric no-op (wndproc.c recompute is IsLinux-gated), so render/viewport/
  mouse.rc were never recomputed for the grown client and the added strip stayed black; the game's
  edge-scroll kept the old `g_ddraw.width/height`-derived bounds. A restart worked because everything
  settled in one pass.
  - Fix (implemented): (1) DELETE the async grow; `ensureChrome` now attaches the menu then posts a
    registered message (`C4dllR_MenuRelayout`) so the menu-aware re-lay runs THROUGH the renderer on
    the GUI thread (`DDReloadConfig` -> `dd_SetDisplayMode` grows by one menu row via
    `AdjustWindowRectEx(GetMenu!=NULL)` AND recomputes viewport+mouse in the same pass). (2) Hardened
    `DDReloadConfig` (patch): re-clip the cursor (`mouse_unlock/lock` when locked) + force a
    renderer-owned `clear_screen`+`RedrawWindow` so no region is left black by the game's DefWindowProc.
  - NOTE: with `maintas=true`+`boxing=true` (our ddraw.ini) the image is integer-boxed/centered by
    design; "fills the window" then means "boxed to the largest integer multiple, centered". If the
    user wants true stretch-to-fill, that is a separate scaling-mode choice (turn boxing/maintas off).

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
- `[x]` **On Day Start/End + extra-time accumulation** — DONE in the plugin (exact formula from
  sub_10001B90/sub_10001DC0, keyed on get_turn_player): Force mode = Fischer time-bank (each turn
  +budget, unused carries per player; ResetExtraTime drops it); Simple mode = count-up + On-Day
  pause/unpause/reset bits. No new game hook needed (uses get_turn_player + get_day).
- `[ ]` **inCombat refinement (Phase 1b)** — off[14]=`0x48D162` / off[15]=`0x540B48` for the precise
  in-combat-for-our-player flag (currently approximated by is_in_battle).
- `[x]` **On Elapse — End Day / Retreat (Phase 2)** — IMPLEMENTED (pending in-game test). Verified the
  legacy press in sub_10004D40 (the WndProc): End-Day presses the first ENABLED captured button in
  priority close->briefCont->capBack->diploBack->endTurn, gated on btnRetreat NOT present (not in
  combat); Retreat presses btnRetreat; press = `(*(*(btn)+0xB0))(btn)` __thiscall, enabled guard
  `*([btn+8]+4)!=0`. Plugin latches v9<0 once per turn (re-armed when v9>=0) and calls host end_day/
  retreat; timerhost queues a pending flag; timerhost_pump() (called from the featuremenu WndProc detour,
  game thread) does the guarded press with a re-entry latch + SEH. The +0x8C/+0xA0 note was wrong - all
  presses use +0xB0.

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
- `[x]` **Map drag-scroll** (faithful) — IMPLEMENTED (pending in-game test). Detours the Russobit iso-view
  mouse handler sub_48E8A0; left-drag on open terrain pans via sub_541588 (screen->map sub_5418BA,
  MapGraphics singleton 0x837DA0). Menu: Game -> "Map drag-scroll (left button)", ini dragScroll default
  off. v1 may need pan-direction/feel tuning. See memory `dgl-map-drag-scroll`.

## Release / CI
- `[ ]` **Push C4dll-R** — the whole `c4ddraw/` + workflows are uncommitted (user chose local for now).
  When ready: first commit + push to origin, then tag `c4dll-r-v0.1` → `c4dll-r-release.yml` publishes
  the drop-in zip. CI itself is clean (the mss32 track never shipped the menu — `skip-worktree`).

## Done recently (for context, not a TODO)
- Animation speed: live + separate battle/map multipliers up to 5x (timeGetTime virtual clock).
- Combat Pause (any battle); fidelity fixes (DayTurn clamp, menu tail IDs, Force-mode ACTIVE gate).
- Timetable per-day duration + the day source (`C4P_Host.get_day` → `CScenarioInfo.currentTurn`).
- First-run `C4menu.ini` converter; mss32 featuremenu removed (the crashing const-patch); adapter cleanup.
