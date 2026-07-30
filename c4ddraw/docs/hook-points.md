# C4dll-R hook points (game join points)

Every place C4dll-R attaches to `Discipl2.exe`, plus its version-independent Win32 API hooks.
Most gameplay hooks still target the validated MNS/SMNS layout (reference exe size
**4187648**; the **4214272** custom-icon variant is treated as the same code layout).
The two exceptions are **game resolution (Hor+)** and **Widescreen Battle**: they select one of the
nine original Disciples II 2.00-3.01 address layouts by PE `ProductVersion` plus that row's code
probe. An unknown version or a mismatched signature leaves those menu items disabled and writes no
game bytes. All other address-based features retain the `detectVersion()` MNS/SMNS gate.
Structure offsets are version-independent (from the D2ModdingToolset headers) unless noted.
Verified against `.idare/Discipl2.exe.i64`.

Hook kinds: **IAT** = import-table swap, **Detour** = MS-Detours trampoline, **vftable** = vtable-slot
overwrite, **patch** = raw byte patch, **call** = we call a game function by address.

## Renderer embed (`patches/cnc-ddraw-c4dll-r.patch`, `dllmain.c`)

| Kind | Target | Purpose |
| --- | --- | --- |
| IAT | game import `DDRAW.dll!DirectDrawCreate` / `DirectDrawCreateEx` | route to the embedded cnc-ddraw (`dd_CreateEx`); no standalone `ddraw.dll` |

## Save handling (`features/savelogic.cpp`, all executable variants)

No game addresses or structures are used. The hooks filter only `.sg` operations; other wrapper,
plugin and game file I/O passes through unchanged.

| Kind | Target | Purpose |
| --- | --- | --- |
| Detour | `kernel32!CreateFileA` | Ctrl `QuickSaveNNN.sg`, remember completed save writes, capture Shift force-archive |
| Detour | `kernel32!CloseHandle` | after the tracked save is closed, copy it to the dated archive when enabled |
| Detour | `kernel32!FindFirstFileA` / `FindNextFileA` / `FindClose` | optional recursive `.sg` enumeration for `[Wrapper] IncludeSubdirectories=1`; child search handles are closed explicitly |

## Menu + WndProc (`features/featuremenu.cpp`)

| Kind | Address | Purpose |
| --- | --- | --- |
| Detour | `0x562E0F` | game WndProc — receive `WM_COMMAND` (menu), our 32 ms `WM_TIMER` pump, drag-scroll |
| Detour | cnc-ddraw `keyboard_hook_proc` | observe a completed Alt+Enter/custom-hotkey mode change and post GUI-thread menu sync; original hotkey semantics and return value are unchanged |
| patch | `0x5628BE` | always-active: skip the lose-focus pause |
| call | `0x401D35` | `CMidgard::instance()` → `data(+8) → settings(+60)` = `GameSettings**` (battle/player/opponent speed) |
| bridge | `DDReloadConfig` / `DDTakeScreenshot` / `DDGetScaleMetrics` | live re-apply `ddraw.ini`, screenshot, and actual game/output/viewport geometry for menu diagnostics |

## Simple zoom + Scenario Editor menu (address-free)

No `Discipl2.exe`, `ScenEdit.exe`, or `mss32.dll` address/API is used. The supported editor is still
identified by the validated PE size (2895872); all zoom/editor message routing is then wrapper-owned.

| Kind | Target | Purpose |
| --- | --- | --- |
| call from patched cnc-ddraw WndProc | `DDHandleSimpleZoom` | reproduce DisciplesGL 2.0.2 Ctrl+Wheel steps (+0.1/-0.4, clamp 1..8, cursor anchor), then forward Up/Down |
| call from OpenGL/D3D9/GDI final output | `DDApplySimpleZoomViewport` | apply the process-local zoom to the final destination rectangle |
| call from patched cnc-ddraw WndProc | `featuremenu_renderer_message` | route ScenEdit menu commands without detouring an editor function |
| Win32 profile write | `Disciple.ini` `[Disciple] ScenEditDatabase` | switch Scenarios/Campaigns; restart required |

## Animation speed (`features/featuremenu.cpp`)

| Kind | Address | Purpose |
| --- | --- | --- |
| IAT | `0x6CE420` (`winmm!timeGetTime`) | virtual clock: scale battle/map animation (factor/10), gated by `g_inBattle` |
| vftable | `0x6F4294` (IBatViewer) | battle discriminator. slots: [0] dtor `0x645900`, [1] choose-action handler `0x630DE3`, **[2] showAttackEffect `0x63203B`** (attack-start pulse), [3] battleEnd `0x631FFC` |
| patched call | `0x638AD9` (original `0x639743`) | exact attack-end pulse after `CBatViewerUtils::CAnimCounter` reaches zero; 14-byte context signature at `0x638AD0` is required |
| vftable | `0x6F48CC` slot[1] (patch at `0x6F48D0`) | per-unit frame-speed; orig `CBatUnitAnim::update` = `0x65615E` (experimental) |
| native | `GameSettings.battleSpeed` / `playerSpeed`(+0x168) / `opponentSpeed` | map/battle speed (no hook; written via the CMidgard chain) |

## Map drag-scroll (`features/featuremenu.cpp`)

| Kind | Address | Purpose |
| --- | --- | --- |
| Detour | `0x48E8A0` | iso-view mouse handler — grab+drag map pan |
| Detour | `0x54249C` | edge-scroll — suppress while dragging |
| call | `0x5414BC` | capture the exact map center and its sub-tile screen offset on LMB down |
| call | `0x541588` | pan from that saved center using `button-down anchor - current cursor` |
| call | `0x5418BA` | native screen-to-map hit test; prevents drag capture over non-map UI |

## Game resolution / Hor+ (`features/horplus.cpp`, original D2 2.00-3.01 layouts)

The selector uses the same five PE `ProductVersion` values and nine per-build probes as the original
`AddressSpaceV2` table. Each row supplies the resolution switch, strategic-map allocation/grid and
drawing sites, limit operands, battle surface class and background-centering site. All derived
instructions, absolute operands and call targets are validated before the transaction is written.
The addresses below are the 3.01/R7 reference row; the other eight rows are carried beside it in
`horplus.cpp` and follow the same derived `+N` relationships.

| Kind | 3.01/R7 reference | Purpose |
| --- | --- | --- |
| signature | `0x5676DA` | `push 0x00CA0000` row probe, paired with `ProductVersion 2003.12.11.1` |
| jump | `0x61A6C5` -> `0x61A72A` | replace the game's logical DirectDraw width/height |
| patch | `0x402444`, `0x40245A` | accept the expanded image path |
| patch | sites derived from `0x5191B2`, `0x51D6E0`, `0x5188A2` | resize canvas-dependent allocations |
| patch | sites derived from `0x5CCA34`, `0x5CC9F5`, `0x48BF35`, `0x488B9E` | resize/enable the strategic grid and minimap path |
| patch | sites derived from `0x5195BF`, `0x5196CE`, `0x672E84` | redirect width/height limits for canvases above 1152 |
| call | `0x53DAA9`, `0x48BF35`, `0x5335EC`, `0x487AB9`, `0x487ADB` | surface state and horizontal/vertical drawing offsets |
| shared call | `0x6482A8`, `0x6482C3` | battle-background centering; normally owned by WideBattle, otherwise installed by Hor+ alone |

## Widescreen Battle (`features/widebattle.cpp`, original D2 2.00-3.01 layouts)

All original bytes and call targets are validated before any write; unsupported executables keep
the stock battle path. The selector mirrors the original table: five `ProductVersion` values and
nine layouts (including duplicate-version variants distinguished by their exact code probe). The
user choice is latched when a battle opens and becomes active only when the game's logical
DirectDraw width is at least 990. RCDATA `10` contains the reviewed `DLG_BATTLE_B` layout and is
prepended once through the selected layout's CRT reader while the startup interface database is
parsed. The addresses below are the 3.01/R7 reference row; `widebattle.cpp` carries and validates
the corresponding address in every selected row, plus the distinct V2/V3 object offsets and ABI.

| Kind | 3.01/R7 reference | Purpose |
| --- | --- | --- |
| signature | `0x62E345` | exact V3 battle-class marker; capability gate only |
| patch + call | `0x6482A8`, `0x6482C3` | center the battle background for stock/wide layouts |
| call | `0x62ED86`, `0x62ED9B` (original `0x62F4CE`) | disable stock unit centering in wide battles |
| call | `0x63289E` (original `0x633FA8`) | remove the left-group mouse-side restriction in wide battles |
| jump / call | `0x639858`, `0x62F068` | keep both unit groups visible and active |
| call | `0x62ED48` | select the V3 reversed-group field (`+0x14F6` / `+0x14F7`) |
| call | `0x62FE08`, `0x62FE36`, `0x6302BC`, `0x6302F0` | initialize both V3 group objects with the correct `+0x1384` / `+0x1398` offsets |
| call | `0x62F1D2` (original `0x62B9C0`) | install the 990x200/11-sprite battle interface indices |
| call | `0x63A8A7` (stock branch `0x63AC33`) | correct item-use rectangles for the visible side |
| call | `0x62E27A`, `0x62E585` | choose `DLG_BATTLE_A` or embedded `DLG_BATTLE_B` |
| reader | `0x6CE274` (`msvcrt!fgets` IAT) | prepend the embedded wide-dialog definition once, then pass through forever; rows R2/R6 patch their original local reader call instead |

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
  hooks are SEH-guarded; the feature is default-OFF and MNS/SMNS-gated, so a bad read fails safe.

## Diagnostics

`[menu] debugLog=1` in `C4menu.ini` (or the `C4DLL_DEBUG` env var) writes an `OutputDebugString` +
`C4menu-<pid>.log` trace. The same gate drives the timer host (`timerhost.cpp`, same log file) and
the plugin host (`pluginhost.cpp` -> `C4plugins.log`), so a default release build writes no log
files at all. Dialog-VO lines are tagged `[dvo]` (`armed` / `eos` / `poll` / `VO done` / SEH).
The voiced-dialog text log is `dialog-vo-log.txt` (append-only, UTF-8), independent of `debugLog`.
