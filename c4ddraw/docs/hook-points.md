# C4dll-R hook points (game join points)

Every place C4dll-R attaches to `Discipl2.exe`. **All addresses are for the Russobit build**
(exe size **4187648**, "Rise of the Elves"); the menu/features refuse to install on any other
version (`detectVersion()` gate). Structure offsets are version-independent (from the D2ModdingToolset
headers) unless noted. Verified against `.idare/Discipl2.exe.i64`.

Hook kinds: **IAT** = import-table swap, **Detour** = MS-Detours trampoline, **vftable** = vtable-slot
overwrite, **patch** = raw byte patch, **call** = we call a game function by address.

## Renderer embed (`patches/cnc-ddraw-mss32.patch`, `dllmain.c`)

| Kind | Target | Purpose |
| --- | --- | --- |
| IAT | game import `DDRAW.dll!DirectDrawCreate` / `DirectDrawCreateEx` | route to the embedded cnc-ddraw (`dd_CreateEx`); no standalone `ddraw.dll` |

## Menu + WndProc (`features/featuremenu.cpp`)

| Kind | Address | Purpose |
| --- | --- | --- |
| Detour | `0x562E0F` | game WndProc — receive `WM_COMMAND` (menu), our 32 ms `WM_TIMER` pump, drag-scroll |
| patch | `0x5628BE` | always-active: skip the lose-focus pause |
| call | `0x401D35` | `CMidgard::instance()` → `data(+8) → settings(+60)` = `GameSettings**` (battle/player/opponent speed) |
| exports | `DDReloadConfig` / `DDTakeScreenshot` | own exports: live re-apply `ddraw.ini` / screenshot |

## Animation speed (`features/featuremenu.cpp`)

| Kind | Address | Purpose |
| --- | --- | --- |
| IAT | `0x6CE420` (`winmm!timeGetTime`) | virtual clock: scale battle/map animation (factor/10), gated by `g_inBattle` |
| vftable | `0x6F4294` (IBatViewer) | battle discriminator. slots: [0] dtor `0x645900`, [1] update `0x630DE3`, **[2] showAttackEffect `0x63203B`** (attack-burst pulse), [3] battleEnd `0x631FFC` |
| vftable | `0x6F48CC` slot[1] (patch at `0x6F48D0`) | per-unit frame-speed; orig `CBatUnitAnim::update` = `0x65615E` (experimental) |
| native | `GameSettings.battleSpeed` / `playerSpeed`(+0x168) / `opponentSpeed` | map/battle speed (no hook; written via the CMidgard chain) |

## Map drag-scroll (`features/featuremenu.cpp`)

| Kind | Address | Purpose |
| --- | --- | --- |
| Detour | `0x48E8A0` | iso-view mouse handler — grab+drag map pan |
| Detour | `0x54249C` | edge-scroll — suppress while dragging |

## Turn timer keystone (`features/timerhost.cpp`)

| Kind | Address | Purpose |
| --- | --- | --- |
| Detour | `0x5C93D6` | dialog-create — capture dialog/battle buttons |
| Detour | `0x48A680` | turn-info (player/serial) |
| ref | `0x6CEB5C` | scenario-init (day source) |
| ref | `0x6E3294` | button-dtor (button lifetime) |

## Dialog-VO auto-skip (`features/featuremenu.cpp`, `dvo*`)

Detour the event-popup VO-start, arm on a real VO, close after the sample finishes. Research:
`<game>/dialog-vo-autoclose-research.md`.

| Kind | Address | Purpose |
| --- | --- | --- |
| Detour | `0x4BE403` | `CEventPopup` try-start-VO — runs mid-ctor; ARM ONLY here (read soundId + popup ptr) |
| Detour | `0x521352` | UI sample-finished dispatcher — match the finished sample id to the armed one |
| call | `0x50BAAF` | `CDialogInterfApi::findButton` (`__stdcall(dialog, name)`) |
| call | `0x50BB0F` | `CDialogInterfApi::findTextBox` (`__stdcall(dialog, name)`) |
| call | `0x50C206` | `CDialogInterfApi::findControl` (`__thiscall`) — used by find\* |
| anchor | `0x6E1884` | `CDialogInterf` vftable — used to pick the real dialog off `popup+0xC` |

### Structures (event popup)

- `data = *(CEventPopup + 0x20)` — the VO/effect data object. `data+0x14` = VO gate (small int, **not** a
  string), `data+0x2C` = loaded mp3 buffer, `data+0x30` = **soundId** (EOS match key), `data+0x34` = show ts.
  The ctor (`0x4BDA84`) zero-inits `data+0x30`, so `soundId != 0` reliably means a real VO played.
- **dialog** = the `CDialogInterf`. `CPopupDialogInterf.dialog` lives at `popup+0xC` (single **or** double
  pointer depending on version) — resolve by probing both derefs and picking the one whose first dword is
  the `CDialogInterf` vftable (`0x6E1884`). `dvoPoll` does this.
- **EOS queue** (at `0x521352`): `base = *(this)`; head ptr `*(base+0x14C)`, tail ptr `*(base+0x150)`;
  finished id = `*head` (guarded by `head != tail`, mirroring the game's own empty-check).
- **close/text controls**: `BTN_RIGHTSIDE` (the single close button) and `TXT_RIGHTSIDE` (text) are bound
  **unconditionally** in the ctor. Only the picture switches by side: `IMG_LEFTSIDE` / `IMG_RIGHTSIDE`
  (`leftSide` flag via `sub_5E4CAC`). So the close button is side-independent (left / right / both).
- close = `findButton(dialog,"BTN_RIGHTSIDE") → buttonData(+8) → onClickedFunctor.data(+0xC) →
  vftable[0](runCallback)`; text = `findTextBox(dialog,"TXT_RIGHTSIDE") → data(+8) → text String(+0x14)`.

### Calling-convention notes

- `__thiscall` game functions are detoured via `__fastcall(self, dummyEdx, ...)`. `0x4BE403` is
  `__thiscall(this)`; `0x521352` is `__thiscall(this, a2, a3)`.
- `sub_4BE403` has an `_EH_prolog` frame; the Detours trampoline handles it. All game-memory reads in the
  hooks are SEH-guarded; the feature is default-OFF and Russobit-gated, so a bad read fails safe.

## Diagnostics

`[menu] debugLog=1` in `C4menu.ini` (or the `C4DLL_DEBUG` env var) writes an `OutputDebugString` +
`C4menu-<pid>.log` trace. Dialog-VO lines are tagged `[dvo]` (`armed` / `eos` / `poll` / `VO done` / SEH).
The voiced-dialog text log is `dialog-vo-log.txt` (append-only, UTF-8), independent of `debugLog`.
