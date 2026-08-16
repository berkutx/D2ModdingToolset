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
  `menuWorker`/old chrome sync: it grew the window by `SM_CYMENU` via a raw `SetWindowPos` on a 1500ms
  poll, OUT-OF-BAND and ~1.5s AFTER `dd_SetDisplayMode`. That async grow fires a Win32 `WM_SIZE` which
  cnc-ddraw treats as a geometric no-op (wndproc.c recompute is IsLinux-gated), so render/viewport/
  mouse.rc were never recomputed for the grown client and the added strip stayed black; the game's
  edge-scroll kept the old `g_ddraw.width/height`-derived bounds. A restart worked because everything
  settled in one pass.
  - Fix (implemented): the worker only posts `C4dllR_MenuRelayout`; GUI-thread `syncChrome` reads the
    renderer's live mode, attaches the menu only in a normal window, and detaches it in both borderless
    and exclusive fullscreen. `DDRelayoutCurrentMode` then re-runs `dd_SetDisplayMode` without reloading
    the ini, so a menu-row change coherently recomputes window geometry, viewport, renderer and mouse
    without undoing a live F4 transition. The old out-of-band `SetWindowPos` path is gone.
  - NOTE: with `maintas=true`+`boxing=true` (our ddraw.ini) the image is integer-boxed/centered by
    design; "fills the window" then means "boxed to the largest integer multiple, centered". If the
    user wants true stretch-to-fill, that is a separate scaling-mode choice (turn boxing/maintas off).
- `[x]` **Fullscreen menu flicker / no way back to a window** — FIXED (pending visual confirmation).
  The Win32 menu is intentionally absent from both fullscreen modes. Wrapper-owned F4 switches either
  fullscreen kind back to a normal window and remembers the last fullscreen kind for the return trip;
  Alt+Enter remains cnc-ddraw's configured toggle and Alt+F4 remains close.
- `[x]` **Window-edge scroll stalls when the pointer crosses the window edge** — FIXED (pending
  in-game confirmation). The cursor guard keeps cnc-ddraw's transformed/clamped game coordinates while
  the game owns the foreground, even when the physical pointer is outside the client. Enabling map
  drag-scroll no longer globally disables native edge-scroll; it suppresses it only during an active
  LMB drag.

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
  presses use +0xB0. The original off[13] callsite bypass is now mirrored for forced END_TURN only,
  and pending actions are cancelled on positive time, Reset/Set, scenario/turn changes and Force-off.

## Timer — dialogs (resource ports)
- `[x]` **Timetable dialog** (legacy **res 5** / sub_100044E0) — DONE: Day/Duration grid (timer.rc
  IDD_TIMETABLE + timetableDlgProc), wired to Force Turn Mode > Timetable... (cmd +0x13). Edits
  (no spinners yet — polish). Menu IDs corrected to the authoritative map (decompiled sub_10004D40):
  +0x13 Timetable, +0x15 Help, +0x16 About; there is NO "Position" dialog (the review was wrong).
- `[ ]` **Set dialog** (legacy **res 6** / DialogFunc, cmd +0x04) — one "minutes" field; sets the
  current turn's remaining time (baseline = now - minutes, + day budget + clears extra in Force mode).
- `[ ]` **About dialog** (legacy res 4 / sub_10004A90, cmd +0x16) — version + author SysLinks;
  currently a plain MessageBox (good enough, low priority).
- `[x]` **Drag-to-move** the on-screen timer — Ctrl+Alt+LMB inside the measured clock rect retains the
  exact grab offset, uses high-resolution normalized anchors and persists on drop without a first-move jump.

NOTE: idalib decompile RESTORED for timer.mod (fresh full idat .i64); all legacy functions now
decompile, so the rest is direct transcription.

## Timer — minor fidelity (from the audit)
- `[ ]` **Extra-time carry** — `dword_10008068` per-day budget table; count-down = `budget + extra -
  elapsed` (we use a flat budget). Tied to the Set / Timetable dialogs.
- `[ ]` **Colour ramp 3-tier** — legacy has blink-red / over-budget / in-budget (+0x80 paused alpha);
  our `pickColors` is 2-tier. Not pixel-identical.
- `[ ]` **FontSize / Offset format** — `FontSize` is our addition (legacy has none); `OffsetX/Y` are
  legacy float 0.0..1.0 anchors, our `AnchorX/Y` are int 0..100. Cosmetic / config-format only.

## Animation speed (see `docs/anim-speed.md`)
- `[~]` **Battle idle/attack split = global burst** — DONE as a global-clock burst: vftable slot[2]
  (`showAttackEffect` `0x63203B`) publishes a start event; the signature-gated final-zero branch of
  the visual `CAnimCounter` (`call 0x638AD9`) publishes the exact end. One last-writer-wins event
  preserves order even when an instant effect starts and ends inside one 32ms pump tick. The factor stays high only between
  those events, then returns linearly over 300ms. A 5s watchdog is emergency-only; signature mismatch
  retains the previous speed-aware timed fallback. Idle is calm BETWEEN attacks, but because this
  drives the global clock it also speeds idle units DURING the actual attack. Not true per-unit isolation.
- `[~]` **True per-unit isolation (EXPERIMENTAL, default off)** — speed up ONLY the acting animators,
  leave idle + global clock vanilla. Mechanism (RE this session): each anim object is clock-gated with a
  per-object interval at `+0x34` (66ms idle / 33ms fast) + deadline `+0x38` (set by ctor `0x51E210`); so
  the lever is shrinking a specific object's `+0x34` (the DGL per-object method), NOT extra-calls/freeze
  (both fail under clock-gating). Impl in `applyPerUnitBurst` (`featuremenu.cpp`): `batUpdateThunk`
  captures the IBatViewer instance (`g_batViewer`); chain `viewer -> *(+4) data -> *(+4996/5000/5016/5020)
  = action animator -> +0x34`. Menu "per-unit (experimental)" toggle. SAFE: `isUserPtr` + SEH +
  value-sanity (only writes if `+0x34` reads 33/66, else no-op so a wrong offset can't corrupt) + restores
  every frame; logs `[burst] off=.. iv@34=..` (throttled) so an in-game test confirms the right objects.
  - `[ ]` PENDING in-game confirm: verify the `+4996..+5020` objects ARE the acting animators and `+0x34`
    is their interval (read the `[burst]` log from a battle). If the chain/offset is off, fix from the log.
  - `[ ]` If confirmed: consider easing the interval (already passes the eased factor) and widening the
    candidate set if attacker/target use more than 4 animators.

## DGL features
- `[~]` **Restart-only widescreen game-resolution selector** — BUILT AND DEPLOYED;
  in-game confirmation pending. The unified Resolution menu offers 1066x600, 1152x648, 1280x720,
  1366x768, 1440x810, 1536x864, 1600x900, 1820x1024, 1920x1080 and 2560x1440.
  Every choice creates a real logical canvas and expands the strategic view horizontally
  without stretching. The smaller choices support compact windows and streaming; the reviewed
  minimum height remains the base UI's 600 pixels.
  Native 800x600 / 1024x768 / 1280x1024 are visible in the same menu and marked with `★`.
  The original nine-layout D2 2.00-3.01 table changes the logical DirectDraw mode, strategic layout
  and dimension-dependent allocations only after `ProductVersion`, the row probe and every required
  signature match; native fallback
  writes no widescreen game bytes. The menu previews `current -> after restart`, future
  viewport/scale and output clamping without reloading the live renderer. After a different canvas
  is successfully saved, a modal explicitly requests a full game restart. Output/window size remains
  a separate renderer setting, with Auto following the active game canvas. Legacy
  `DisplayWidth/DisplayHeight` is imported only for the unsupported-executable native fallback;
  an absent `GameCanvasMode` defaults a supported executable to 1280x720, while explicit mode 0
  selects the corresponding stock `DisplaySize`.
- `[x]` **Map drag-scroll** (faithful) — IMPLEMENTED (pending in-game feel test). Detours the Russobit
  iso-view mouse handler sub_48E8A0; screen->map sub_5418BA gates capture to real map terrain,
  sub_5414BC snapshots the exact center/sub-tile offset on button-down, and sub_541588 pans from that
  invariant on the first changed game pixel. MapGraphics singleton: 0x837DA0. Menu: Game -> "Map
  drag-scroll (left button)", ini dragScroll default on. A no-movement click is preserved, and native
  edge-scroll remains active except during the drag itself. See memory `dgl-map-drag-scroll`.
- `[x]` **Widescreen Battle** — ported across the original nine D2 2.00-3.01 address layouts with
  `ProductVersion`, row-probe and original-byte validation before any patch. Default on, latched when
  the next battle opens, and unavailable when the selected executable signatures do not match. It
  activates only when both the logical canvas and original fixed-screen zoom view are at least 990
  pixels wide; default 1024x768/1280x1024 therefore remain stock, while reviewed Hor+ canvases are
  wide. The user-facing toggle is intentionally hidden in 1.7 while the existing default/config
  activation path remains. This widens only the battle layout and shows both unit panels; it does not
  select or widen the strategic-map resolution. The shared non-wide background correction preserves
  the original mirror-dependent 0/-150 geometry on native 800x600.
- `[~]` **Optional map clouds** — BUILT AND DEPLOYED; in-game confirmation pending.
  This is the real signature-gated allocation/factory/archive-lookup/init/update pipeline, not a
  renderer boolean or an inert menu item. It uses an existing external `Imgs\IsoClouds.ff`; that
  asset is validated at startup and is not redistributed. The restart-only menu item aliases the
  game's native `[Settings] IsoBirds` visibility setting, remains unavailable when the asset or
  exact executable is unsupported, and applies no cloud patch in that case.

## Release / CI
- `[ ]` **Push C4dll-R** — the whole `c4ddraw/` + workflows are uncommitted (user chose local for now).
  When ready: first commit + push to origin, then tag `c4dll-r-v0.1` → `c4dll-r-release.yml` publishes
  the drop-in zip. CI itself is clean (the mss32 track never shipped the menu — `skip-worktree`).

## Done recently (for context, not a TODO)
- Animation speed: live battle/map multipliers up to 15x; native hero/AI map speed (`playerSpeed`)
  + battle speed (`GameSettings`); exact-end battle attack burst (fast hits, calm idle) with a
  300ms ease-down after the last visual component completes.
- Combat Pause (any battle); fidelity fixes (DayTurn clamp, menu tail IDs, Force-mode ACTIVE gate).
- Timetable per-day duration + the day source (`C4P_Host.get_day` → `CScenarioInfo.currentTurn`).
- First-run `C4menu.ini` converter; mss32 featuremenu removed (the crashing const-patch); adapter cleanup.
