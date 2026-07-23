# C4dll-R monolith (cnc-ddraw renderer + in-game menu, one assembly)

This folder builds a single **`C4dll-R.dll`** that replaces the game's original third-party
DirectDraw renderer for Disciples II, in the same drop-in, swappable way. It is one self-contained
assembly: the [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) renderer is **embedded** (no
separate `ddraw.dll` to ship), the original CodeBase exports are **forwarded** to `CB63.dll`, and an
in-game **menu** is included. It does **not** depend on, modify, or require the
`mss32` mod: `mss32.dll` keeps calling `Mss23.dll` and is never touched.

## Layout

| Path | What it is | Committed |
| --- | --- | --- |
| `upstream/cnc-ddraw/` | The cnc-ddraw renderer as a **git submodule**, pinned at upstream `a0b81b11` (a 7.1.0.1-dev snapshot, 80 commits past the v7.1.0.0 tag). Never edited in place. | submodule pointer |
| `patches/cnc-ddraw-c4dll-r.patch` | Minimal integration diff over upstream: redirect the exe's `DirectDrawCreate(Ex)` imports and call the single `c4features_install()` bootstrap (`dllmain.c` only) | yes |
| `patches/cnc-ddraw-render-null.patch` | The `render_null` headless backend: `dd.c` renderer branch + vcxproj entries + new `inc/render_null.h`, `src/render_null.c` | yes |
| `patches/cnc-ddraw-default-ini.patch` | The Disciples II tuned `ddraw.ini` template written by `cfg_create_ini` (`config.c`) when no ini exists on first run | yes |
| `patches/cnc-ddraw-simple-zoom.patch` | Narrow WndProc + OpenGL/D3D9/GDI integration for wrapper-owned Ctrl+Wheel zoom and address-free editor menu routing | yes |
| `features/featuremenu.cpp` | The in-game menu + feature hooks, self-contained (no mss32 deps) | yes |
| `features/rendererbridge.c` | Wrapper-owned adapters to cnc-ddraw internals: live reload, screenshot, coordinate mapping and simple-zoom state/formula | yes |
| `features/localization.cpp` | Locale/encoding bridge modelled after the legacy wrapper; no hard-coded Russian code pages | yes |
| `features/savelogic.cpp`, `cursorfix.cpp` | Version-independent save/archive hooks and the Disciples II edge-scroll guard | yes |
| `docs/hook-points.md` | Every game address/structure C4dll-R attaches to (Russobit) | yes |
| `forwarder/C4dll-R.cb63.def` | The 483 CB63 export forwards (`Name=CB63.Name @ord`) | yes |
| `build.ps1` | Reproducible build + deploy/restore | yes |
| `build/`, `out/` | Build working copy + artifact (regenerated) | no (gitignored) |

## How it works

1. The game statically imports `DDRAW.dll`, `C4dll-R.dll` (CodeBase), `mss32.dll` and others.
2. `C4dll-R.dll` forwards all 483 CodeBase exports to `CB63.dll`, so it satisfies the game's
   `C4dll-R` import exactly like the original CodeBase copy did.
3. From cnc-ddraw's `DllMain` we patch the **game exe's IAT** for `DirectDrawCreate`/`DirectDrawCreateEx`
   to the embedded cnc-ddraw implementation. The game's static `DDRAW.dll` import still loads the
   system `ddraw.dll`, but its create entry points are bypassed, so the embedded renderer is used.
   No separate `ddraw.dll` is shipped. (Everything stays inside the single `C4dll-R.dll`.)
4. The one `c4features_install()` bootstrap starts the wrapper-owned modules. `featuremenu_install()`
   then adds the in-game menu: it detours the game window procedure by
   address to receive `WM_COMMAND` and attaches a real menu bar (Game / Video / Performance / Plugins)
   under cnc-ddraw's title bar. Renderer settings are written to `ddraw.ini` and re-applied live via the
   renderer's own `DDReloadConfig`; screenshots use `DDTakeScreenshot`.
5. The **Game** menu also carries gameplay features that hook the exe by address (Russobit only, all
   SEH-guarded, most default-off): live battle/map animation-speed multipliers and per-hit attack burst,
   map drag-scroll and **Skip voiced event dialogs** (auto-close a `DLG_EVENT_POPUP` after its
   voiceover finishes and append its text to `dialog-vo-log.txt`). Every game address and structure
   these touch is listed in [`docs/hook-points.md`](docs/hook-points.md).

## Build

```powershell
./build.ps1                 # build only  -> build/cnc-ddraw/bin/Release/C4dll-R.dll
./build.ps1 -Deploy         # build, back up the game's baseline once, then swap in the monolith
./build.ps1 -Restore        # put the baseline C4dll-R.dll + standalone ddraw.dll back
```

`build.ps1` copies the pinned `upstream/cnc-ddraw` submodule to `build/`, applies the four patches
(`cnc-ddraw-c4dll-r` integration + `render-null` + `default-ini` + `simple-zoom`), copies in the
wrapper-owned `features/*.c*` sources (renderer bridge, menu, plugins, locale, saves, cursor guard
and headless mode),
generates `C4dll-R.def` (the CB63 forwards plus the two exports), retargets the vcxproj
(`TargetName` + `.def` + the extra source), stamps the version identity (`-Version <ver>`, default
`dev-<repo sha>`: writes `inc/git.h` from the outer repo, edits `res.rc` to identify as C4dll-R,
strips the upstream PreBuildEvent that regenerated `git.h` as UNKNOWN), and runs MSBuild (Release,
Win32, v143, static CRT).
MSBuild is located via `vswhere`, so it works both on a dev box and on CI. The CI workflow
`.github/workflows/c4ddraw.yml` runs the same `build.ps1` and uploads `C4dll-R.dll` + `timer.c4p`.

## Releases

C4dll-R is published to GitHub Releases on its **own tag namespace** (`c4dll-r-v*`), separate from the
mss32 mod, so the two are versioned and released independently (their version numbers may differ). To
cut a release, push a tag:

```sh
git tag c4dll-r-v1.0
git push origin c4dll-r-v1.0
```

`.github/workflows/c4dll-r-release.yml` then builds `C4dll-R.dll` + `Mods/timer.c4p`, packages them
with `INSTALL.txt`, a sample `C4plugins.ini` and the recommended `ddraw.ini` into `C4dll-R-v1.0.zip`, and publishes a GitHub
Release with that zip, a `-symbols.zip` (the matching PDBs for crash triage) and the loose
`C4dll-R.dll` + `timer.c4p` attached. The release version is stamped into the DLL version
resource (`build.ps1 -Version`), so a build is identifiable from file properties. Running the
workflow manually (workflow_dispatch) publishes a **prerelease** tagged `c4dll-r-dev-<sha>` (or
your label); versions containing `rc` / `alpha` / `beta` / `dev` are always marked prerelease. The
package sources live in `c4ddraw/release/` (`INSTALL.txt`, `C4plugins.ini`, `ddraw.ini`,
`RELEASE_NOTES.md`).

## Deploy by hand

Put `C4dll-R.dll` next to `Discipl2.exe` (replacing the CodeBase copy), keep `CB63.dll` and
`ddraw.ini` there, and remove any standalone `ddraw.dll`. To A/B test our-vs-stock, swap
`C4dll-R.dll` only.

## Updating cnc-ddraw

`upstream/cnc-ddraw/` is a git submodule pinned at an exact commit. To move to a newer upstream:
bump the submodule (`git -C upstream/cnc-ddraw fetch && git -C upstream/cnc-ddraw checkout <sha>`, then
`git add upstream/cnc-ddraw`), re-apply the patches against the new tree (`git apply`), resolve any
reject, and regenerate. `build.ps1` always builds from the pinned submodule + the patches, so the
upstream tree is never edited in place. The patches touch disjoint files, so order does not matter.

## What exactly is linked into C4dll-R.dll

One binary, three layers:

| Layer | Sources | Purpose |
| --- | --- | --- |
| cnc-ddraw core | all upstream `src/*.c`: `dd`, `ddsurface`, `blt`, `config`, renderers `render_ogl` / `render_d3d9` / `render_gdi`, `winapi_hooks`, `wndproc`, `hook`, `fps_limiter`, `utils`, `lodepng` (screenshots), the `IDirectDraw*` / `IDirect3D*` COM shims | the DirectDraw replacement itself |
| render_null | added by `patches/cnc-ddraw-render-null.patch` | headless backend (`renderer=null`) for test harnesses, no visible output |
| Microsoft Detours | upstream `src/detours/` | function/IAT hooking used by cnc-ddraw and the feature layer |
| C4dll-R layer | `features/rendererbridge.c`, `c4features.cpp`, `featuremenu.cpp`, `pluginhost.cpp`, `timerhost.cpp`, `localization.cpp`, `savelogic.cpp`, `cursorfix.cpp`, `headless.cpp` | wrapper integration, menu, plugins, locale conversion, save/archive logic, D2 cursor guard and headless windowing |

Exports: the 483 CodeBase forwards (`name=CB63.name`) plus `DDReloadConfig` (live settings
reload) and `DDTakeScreenshot`. `Mods\timer.c4p` is built separately from `plugins/timer/` and is
NOT inside the DLL.

Why one DLL: the game already imports a library named `C4dll-R` (the CodeBase copy), so a single
file swap delivers the renderer, the menu and the plugin host, with no separate `ddraw.dll` that
could shadow or be shadowed.

## First run and settings files

Three files, three owners:

| File | Who creates it | What lives there |
| --- | --- | --- |
| `ddraw.ini` | shipped in the release zip (recommended defaults); if absent, C4dll-R generates a Disciples II tuned one on first launch (`patches/cnc-ddraw-default-ini.patch`) | renderer, window mode, resolution, shader, performance caps |
| `C4menu.ini` | generated by the menu on first launch | gameplay toggles: always active, animation speed, attack burst, drag-scroll and unit-hire auto-confirm; plus `language` (auto/en/ru) and `debugLog` (0 = no C4menu-<pid>.log / C4plugins.log files, default; 1 or the `C4DLL_DEBUG` env var enables diagnostics) |
| `Disciple.ini` | the game's own/wrapper-compatible file | native speed presets, editor `ScenEditDatabase`, `[Wrapper] Locale`, `Archive` and `IncludeSubdirectories` |

Old settings conversion: on first launch (no `C4menu.ini` yet) the menu reads the legacy
`mss32menu.ini` `[menu]` section (the old mss32-mod menu config) and converts it: `alwaysActive`
carries over as is; old `animationSpeedEnabled=1` maps to battle speed 1.5x, otherwise the default
2x is used. Nothing else is read, and the conversion never touches the game's `Disciple.ini` or
`Scripts\settings.lua`. Once `C4menu.ini` exists it is never regenerated: the user owns it.

If `ddraw.ini` is missing entirely, the generated default is no longer the upstream stock one:
`cfg_create_ini` writes a Disciples II tuned config (`patches/cnc-ddraw-default-ini.patch`) -
`fake_mode=1024x768x16`, `renderer=opengl`, windowed with a real title bar (`windowed=true`,
`border=true`, `resizable=false`), `width=800`/`height=600`, `maintas=true`, the xBRZ freescale
shader, `devmode=true`, `singlecpu=true`, `nonexclusive=true`, `noactivateapp=true`, `maxfps=144`,
`maxgameticks=100`, `vsync=true`, the usual hotkeys, and `savesettings=0` so cnc-ddraw never
rewrites the file and strips its comments. The comments are carried in the file and explain every
choice; the ini parser takes everything after `=` as the value, so all comments sit on their own
lines. The zip still ships the recommended `ddraw.ini` (native resolution, resizable window,
Lanczos shader, `savesettings=1`) - delete it to compare against the generated one.

## Settings reference: ddraw.ini (shipped defaults)

"live" = the menu applies it instantly through `DDReloadConfig`; "restart" = takes effect on the
next game start.

| Key | Shipped | Effect | Applies |
| --- | --- | --- | --- |
| `fake_mode` | `1024x768x16` | fakes a 16-bit desktop for the game's color-depth check | restart |
| `renderer` | `opengl` | shaders + best upscaling; `auto` picks D3D9 first (no shader filters); `gdi` = software; if OpenGL fails, cnc-ddraw falls back to GDI on its own | restart |
| `windowed` + `fullscreen` | `true` + `false` | windowed with a title bar and the menu; `true`+`true` = borderless fullscreen; `false`+any = exclusive fullscreen | live |
| `border` | `true` | real title bar, draggable window; the menu bar sits under it | live |
| `resizable` | `true` | window edges resize; aspect is kept by `maintas` | live |
| `width`, `height` | `0`, `0` | output size; 0 = native game size (1024x768) | live |
| `maintas` | `true` | keep 4:3, no stretching on widescreen | live |
| `boxing` | `false` | integer scaling (sharp pixels + borders); off fills the window | live |
| `shader` | `lanczos2-sharp` | upscale filter, OpenGL renderer only; the menu offers 8 presets | live |
| `savesettings` | `1` | cnc-ddraw writes window size/pos/state back on exit | - |
| `maxgameticks` | `100` | game loop cap in ticks/s; see "The game speed cap" below | restart |
| `maxfps` | `-1` | render FPS cap, -1 = screen refresh; paces the render thread only, never slows the game | restart |
| `vsync` | `false` | vertical sync; needed only against tearing in exclusive fullscreen (windowed and borderless never tear thanks to DWM composition), costs a little display lag | live |
| `singlecpu` | `true` | pin the process to one core (cnc-ddraw old-game default; candidate for `false`, see Experimental; menu: Performance > Single CPU core) | restart |
| `noactivateapp` | `true` | keep rendering when unfocused (the game LOGIC half of this is the menu item "Always active") | restart |
| `nonexclusive` | `true` | never take exclusive DirectDraw; reliable menus/videos | restart |
| `adjmouse` | `true` | scale the cursor to the window size | live |
| `devmode` | `true` | cursor not clipped to the window (original windowed feel); Ctrl+Tab or RAlt+RCtrl release it if anything clips | live |
| `keytogglefullscreen` ... | see file | hotkeys as VK codes, 0x00 disables | - |
| `resolutions`, `fixchilds` | `0`, `2` | mode-list and child-window handling; fine as is for D2 | - |

Parser warning: comments only on their own lines. Everything after `=` including trailing spaces
is the value, so an inline `; comment` silently breaks the setting.

## The game speed cap (maxgameticks)

The one setting that matters more than FPS. The engine advances every game command (a move, a
battle action, a dialog transition) through an internal chain that is pumped one step per
main-loop iteration, and `maxgameticks` caps that loop. Measured on real sessions: one command
takes about 10 pump steps, so the reaction floor per command is roughly `10 / maxgameticks`
seconds:

| maxgameticks | Reaction floor per command | CPU |
| --- | --- | --- |
| 30 | ~330 ms: the game visibly "thinks" before every action | coolest |
| 60 | ~170 ms | low |
| 100 (shipped) | ~100 ms | moderate |
| -1 (uncapped) | as fast as the CPU allows | one core busy |

The old DisciplesGL "ColdCPU" feel corresponds to 30. The shipped default is 100: near-instant
reactions at a bounded CPU cost. Note that `maxfps` has no such effect: it paces only the render
thread.

## In-game menu

The full gameplay menu installs on the Russobit exe. The validated Scenario Editor exe gets the
address-free **File / Video / Performance / Plugins** menu; unsupported executables keep the
renderer only. In windowed mode the bar appears under the title bar.

The menu is bilingual: `C4menu.ini` `[menu] language` = `auto` (default) / `en` / `ru`. With
`auto` the menu is Russian when the Windows UI language is Russian or the system codepage is 1251,
English otherwise. Submenus carry grayed one-line hints explaining each option.

### Simple zoom

`Ctrl+Mouse Wheel` reproduces the DisciplesGL 2.0.2 simple zoom in both the game and Scenario
Editor: wheel up adds 0.1x, wheel down subtracts 0.4x, range 1.0x..8.0x, and the image stays anchored
at the cursor. It is process-local, not persisted, and works in OpenGL, D3D9/Auto and GDI. Ordinary
wheel input retains the original wrapper behavior (an Up/Down key press). It does not inspect or call
`mss32.dll`.

### Scenario Editor

**File > Editor mode > Scenarios / Campaigns** writes the editor's native
`Disciple.ini` `[Disciple] ScenEditDatabase=0/1` setting and asks for an editor restart, exactly like
the legacy wrapper. The switch uses no executable or `mss32.dll` addresses; the exact supported
`ScenEdit.exe` is still validated by the existing PE-size check.

### Game

| Item | What it does | Saved to | Applies |
| --- | --- | --- | --- |
| Always active | the game keeps running (no pause) when the window loses focus; an in-place patch of the activation check, verified against the original bytes before writing | `C4menu.ini` `alwaysActive` | live |
| Map drag-scroll (left button) | hold LMB on the map and drag to pan; a plain click still selects (delivered on release); window-edge scroll is off while enabled | `C4menu.ini` `dragScroll` | live |
| Auto-confirm unit hire | skips only “Do you want to hire this unit?” by invoking its normal `BTN_YES` callback during the local player's turn; off by default | `C4menu.ini` `autoConfirmUnitHire` | live |
| Battle speed (whole battle): Off / 1.5x / 2x (default) / 3x / 4x / 5x / 15x | multiplies all battle animation timing via a virtual clock (a `timeGetTime` redirect); no game memory is patched | `C4menu.ini` `battleAnimEnabled` + `battleAnimSpeed` | live |
| Attack speed-up (burst on each hit): Off / 1.5x .. 5x (default) / 15x | an extra multiplier only while a hit/effect plays (about 1.2 s, easing back over 0.7 s), on top of the battle speed | `C4menu.ini` `battleAttackEnabled` + `battleAttackSpeed` | live |
| Map animation speed: Off (default) / 1.5x .. 15x | the same virtual clock on the strategic map (water, flags, effects) | `C4menu.ini` `mapAnimEnabled` + `mapAnimSpeed` | live |
| Battle speed (game option): Slow / Normal / Fast / Instant | the game's OWN option, same as its settings screen | `Disciple.ini` `BattleSpeed` | next battle |
| Map movement speed (game option): Normal / Fast / Very fast | the game's OWN option: walk speed of player and AI stacks, read when a move starts | `Disciple.ini` `PlayerSpeed` + `OpponentSpeed` | next move |

The 15x entries are test presets, exaggerated on purpose.

### Video

| Item | What it does | Saved to | Applies |
| --- | --- | --- | --- |
| Display mode: Windowed / Fullscreen borderless / Fullscreen exclusive | window style; exclusive does a real display-mode change and falls back to borderless where impossible (RDP); Alt+Enter toggles | `ddraw.ini` `windowed` + `fullscreen` | live |
| Resolution: Native .. 3840x2160 | output window size; Native = the 1024x768 game size | `ddraw.ini` `width` + `height` | live |
| Filter / upscale: Lanczos (best for D2 art) / xBRZ / Bicubic / AMD FSR / xBR lv2 / Bilinear / None / CRT | the upscale shader | `ddraw.ini` `shader` | live, OpenGL only |
| Renderer (restart): OpenGL (recommended) / GDI / Auto | rendering backend; Auto picks D3D9 first, which has no shader filters | `ddraw.ini` `renderer` | restart |
| Keep 4:3 aspect | letterbox instead of stretch on widescreen | `ddraw.ini` `maintas` | live |
| VSync | fixes tearing in exclusive fullscreen at the cost of a little display lag; windowed and borderless never tear (DWM composition), so keep it off there | `ddraw.ini` `vsync` | live |
| Integer scaling | pixel-perfect zoom with borders; keep OFF to fill the window | `ddraw.ini` `boxing` | live |
| Take screenshot (PrintScreen) | saves a screenshot via the renderer | - | - |

### Performance

| Item | What it does | Saved to | Applies |
| --- | --- | --- | --- |
| Frame cap (restart): Monitor refresh rate / 30 / 60 / 144 | render FPS only, never slows game logic | `ddraw.ini` `maxfps` | restart |
| Game speed cap (restart): Uncapped / 30 / 60 / 100 (default) | the game loop cap, see the section above | `ddraw.ini` `maxgameticks` | restart |
| Single CPU core (restart) | pins the whole process to CPU core 0 (cnc-ddraw's old-game safety net, ON by default); OFF is the experimental performance option, see Experimental | `ddraw.ini` `singlecpu` | restart |

### Plugins

Appears when `Mods\` contains plugins: `Native (.c4p)` and `Legacy (.mod)` submenus, each grafting
the plugin's own menu. The bundled Timer plugin is configured via `C4plugins.ini` and is
documented separately.

## Save handling (all game versions)

`features/savelogic.cpp` ports the legacy wrapper's file-level save conveniences without using a
single `Discipl2.exe` address. It detours Win32 file enumeration/open/close APIs, so the same code
applies to the Akella, Russobit, GOG and editor executables:

- hold **Ctrl** while confirming a normal save to write the next `QuickSaveNNN.sg` beside it;
- `[Wrapper] Archive=1` in `Disciple.ini` (default when the key is absent) copies each closed save
  to `Archive\YYYYMMDD\~name-YYYYMMDD-HHMMSS-marker.sg`;
- hold **Shift** while saving to force that one archive copy even when `Archive=0`;
- `[Wrapper] IncludeSubdirectories=1` (default `0`) recursively exposes `.sg` files under
  `Archive\` in the game's ordinary save/load list.

These settings are read when the operation happens, so editing `Disciple.ini` needs no rebuild and
no game restart. The game's `[Settings] AutoSave` key is unrelated and is not implemented here.
With `[menu] debugLog=1`, save events also go to `C4saves-<pid>.log`.

## Game text locale (all game versions)

`features/localization.cpp` implements the legacy wrapper's `[Wrapper] Locale=<LCID>` behavior
without hard-coding Russian 866/1251. The selected Windows locale supplies its OEM and ANSI code
pages and msvcrt `LC_CTYPE`; the `OemToCharA`/`CharToOemA` bridge can be switched live. The default
is `GetUserDefaultLCID`; `Locale=0` disables wrapper recoding. On the Russobit build it is exposed as
**Game > Game text locale**, enumerating the locales installed in Windows and writing the selected
LCID to `Disciple.ini`. Other game builds use the same address-free bridge and can set the key by
hand even though their address-specific in-game menu is not installed.

## Experimental

- Attack burst 15x steps: test-only exaggeration.
- `C4menu.ini` `perUnitBurst`: per-unit burst scoping; no menu item, off by default, unfinished.
- Single CPU core (`singlecpu`): cnc-ddraw's old-game safety net. With `true` (the default) the
  whole process is pinned to CPU core 0 on Windows 10 (`SetProcessAffinityMask(1)`; Windows 11
  24H2+ uses a softer per-thread mechanism instead). The pin protects ancient games from timer
  drift and core-migration bugs, but it also serializes everything on one core: the render thread
  runs at ABOVE_NORMAL priority and preempts the game thread on every presented frame, and any
  background-thread burst becomes a direct game stall. D2 has no known core-migration problems,
  so `false` is the performance candidate; it is NOT yet A/B tested, which is why the shipped
  default stays `true`. Toggle: Performance > Single CPU core (restart), or `singlecpu=` in
  ddraw.ini; it is a plain config value, no rebuild involved. How to test: switch OFF, restart the
  game, play a battle and a few map turns; watch for sound stutter or timing oddities (test with
  sound ON - stutter is the known failure mode), then keep whichever feels better.
- `renderer=null`: no-render backend for test harnesses; nothing is drawn, the game window stays a
  NORMAL window. Where the environment cannot create real top-level windows at all (Wine null
  driver, no X server) the same run falls back to message-only windows automatically: the fallback
  arms only after a real window creation fails AND the message-only retry succeeds, so on a desktop
  it never engages.

## No OpenGL on the system (VM, RDP): Mesa and dxil.dll

Not part of the release. Where no working OpenGL driver exists, the Mesa3D Windows distribution
can be dropped next to the exe (`opengl32.dll` + `libgallium_wgl.dll` and companions): cnc-ddraw
then renders GL through Mesa, usually via its D3D12 backend. In that setup `dxil.dll` from the
same Mesa kit is REQUIRED: without it the D3D12 driver cannot sign its shaders and the game
silently exits at startup. Outside the Mesa setup `dxil.dll` does nothing and is not shipped with
C4dll-R.

---

# Монолит C4dll-R (рендерер cnc-ddraw + внутриигровое меню, одна сборка)

Эта папка собирает единый **`C4dll-R.dll`**, заменяющий оригинальный сторонний рендерер DirectDraw
для Disciples II, тем же подключаемым/заменяемым способом. Это одна
самодостаточная сборка: рендерер [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) **встроен**
(отдельный `ddraw.dll` не нужен), оригинальные экспорты CodeBase **форвардятся** в `CB63.dll`, и
включено внутриигровое **меню**. Сборка **не** зависит от мода `mss32`, не меняет
его и не требует: `mss32.dll` так же зовёт `Mss23.dll` и не трогается.

## Структура

| Путь | Что это | В репозитории |
| --- | --- | --- |
| `upstream/cnc-ddraw/` | Рендерер cnc-ddraw как **git submodule**, запинен на апстрим `a0b81b11` (dev-снапшот линии 7.1.0.1, 80 коммитов после тега v7.1.0.0). На месте не редактируется. | указатель submodule |
| `patches/cnc-ddraw-c4dll-r.patch` | Минимальный интеграционный дифф: перенаправление импортов `DirectDrawCreate(Ex)` exe и один вызов `c4features_install()` (только `dllmain.c`) | да |
| `patches/cnc-ddraw-render-null.patch` | Headless-бэкенд `render_null`: ветка рендерера в `dd.c` + записи vcxproj + новые `inc/render_null.h`, `src/render_null.c` | да |
| `patches/cnc-ddraw-default-ini.patch` | Настроенный под Disciples II шаблон `ddraw.ini`, который `cfg_create_ini` (`config.c`) пишет при первом запуске, если файла нет | да |
| `patches/cnc-ddraw-simple-zoom.patch` | Узкая интеграция WndProc и OpenGL/D3D9/GDI для `Ctrl+колесо` и адресно-независимой маршрутизации меню редактора | да |
| `features/featuremenu.cpp` | Внутриигровое меню + хуки фич, самодостаточное (без зависимостей mss32) | да |
| `features/rendererbridge.c` | Собственные адаптеры врапера к внутренностям cnc-ddraw: live reload, скриншот, перевод координат, состояние и формула simple zoom | да |
| `features/localization.cpp` | Мост локали/кодировок по образцу старого врапера, без жёстких русских кодовых страниц | да |
| `features/savelogic.cpp`, `cursorfix.cpp` | Независимые от версии хуки сейвов/архива и защита edge-scroll для Disciples II | да |
| `docs/hook-points.md` | Все адреса/структуры игры, к которым цепляется C4dll-R (Russobit) | да |
| `forwarder/C4dll-R.cb63.def` | 483 форварда экспортов CB63 (`Name=CB63.Name @ord`) | да |
| `build.ps1` | Воспроизводимая сборка + deploy/restore | да |
| `build/`, `out/` | Рабочая копия сборки + артефакт (генерируются) | нет (gitignore) |

## Как это работает

1. Игра статически импортирует `DDRAW.dll`, `C4dll-R.dll` (CodeBase), `mss32.dll` и другие.
2. `C4dll-R.dll` форвардит все 483 экспорта CodeBase в `CB63.dll`, удовлетворяя импорт `C4dll-R`
   точно как оригинальная копия CodeBase.
3. Из `DllMain` cnc-ddraw мы патчим **IAT exe игры** для `DirectDrawCreate`/`DirectDrawCreateEx` на
   встроенную реализацию cnc-ddraw. Статический импорт `DDRAW.dll` всё ещё грузит системный
   `ddraw.dll`, но его точки создания обходятся, поэтому используется встроенный рендерер. Отдельный
   `ddraw.dll` не поставляется. (Всё остаётся внутри единого `C4dll-R.dll`.)
4. Единственный bootstrap `c4features_install()` запускает собственные модули врапера. Затем
   `featuremenu_install()` добавляет меню: детурит оконную процедуру игры по адресу, чтобы
   получать `WM_COMMAND`, и крепит настоящий меню-бар (Game / Video / Performance / Plugins) под
   заголовком cnc-ddraw. Настройки рендерера пишутся в `ddraw.ini` и применяются вживую через собственный
   `DDReloadConfig`; скриншоты делает `DDTakeScreenshot`.
5. Меню **Game** также несёт геймплейные фичи, которые цепляются к exe по адресам (только Russobit, всё
   под SEH, большинство по умолчанию выключены): живые множители скорости анимации боя/карты и бонус на
   удар, перетаскивание карты и **пропуск озвученных диалогов** (авто-закрытие `DLG_EVENT_POPUP`
   после озвучки + запись текста в `dialog-vo-log.txt`). Все адреса и структуры игры, к которым
   они цепляются, перечислены в [`docs/hook-points.md`](docs/hook-points.md).

## Сборка

```powershell
./build.ps1                 # только сборка -> build/cnc-ddraw/bin/Release/C4dll-R.dll
./build.ps1 -Deploy         # сборка, разовый бэкап baseline игры, затем подмена на монолит
./build.ps1 -Restore        # вернуть baseline C4dll-R.dll + отдельный ddraw.dll
```

`build.ps1` копирует запиненный submodule `upstream/cnc-ddraw` в `build/`, накладывает четыре патча
(`cnc-ddraw-c4dll-r` + `render-null` + `default-ini` + `simple-zoom`) и копирует собственные
`features/*.c*` (renderer bridge, меню, плагины, локаль, сейвы, cursor guard и headless),
генерирует `C4dll-R.def` (форварды CB63 плюс два экспорта), перенацеливает vcxproj (`TargetName` +
`.def` + доп. исходник), зашивает версию (`-Version <ver>`, по умолчанию `dev-<sha репо>`: пишет
`inc/git.h` из внешнего репо, правит `res.rc` на идентичность C4dll-R, вырезает апстримный
PreBuildEvent, который перегенерировал `git.h` в UNKNOWN) и запускает MSBuild (Release, Win32,
v143, статический CRT). MSBuild ищется
через `vswhere`, поэтому работает и на машине разработчика, и на CI. Workflow
`.github/workflows/c4ddraw.yml` запускает тот же `build.ps1` и выгружает `C4dll-R.dll` + `timer.c4p`.

## Релизы

C4dll-R публикуется в GitHub Releases в **собственном тег-неймспейсе** (`c4dll-r-v*`), отдельно от
мода mss32, поэтому они версионируются и релизятся независимо (номера версий могут различаться). Чтобы
выпустить релиз, запушьте тег:

```sh
git tag c4dll-r-v1.0
git push origin c4dll-r-v1.0
```

`.github/workflows/c4dll-r-release.yml` соберёт `C4dll-R.dll` + `Mods/timer.c4p`, упакует их с
`INSTALL.txt`, примером `C4plugins.ini` и рекомендованным `ddraw.ini` в `C4dll-R-v1.0.zip` и опубликует GitHub Release с этим
архивом, `-symbols.zip` (соответствующие PDB для разбора крашей) и отдельными файлами
`C4dll-R.dll` + `timer.c4p`. Версия релиза зашивается в ресурс версии DLL (`build.ps1 -Version`),
так что сборка опознаётся по свойствам файла. Ручной запуск workflow (workflow_dispatch) публикует
**пре-релиз** с тегом `c4dll-r-dev-<sha>` (или вашей меткой); версии, содержащие `rc` / `alpha` /
`beta` / `dev`, всегда помечаются пре-релизом. Исходники пакета — в `c4ddraw/release/`
(`INSTALL.txt`, `C4plugins.ini`, `ddraw.ini`, `RELEASE_NOTES.md`).

## Ручная установка

Положите `C4dll-R.dll` рядом с `Discipl2.exe` (заменив копию CodeBase), оставьте `CB63.dll` и
`ddraw.ini`, удалите любой отдельный `ddraw.dll`. Для сравнения наш/сток меняйте только `C4dll-R.dll`.

## Обновление cnc-ddraw

`upstream/cnc-ddraw/` это git submodule, запиненный на точный коммит. Чтобы перейти на новый апстрим:
сдвиньте submodule (`git -C upstream/cnc-ddraw fetch && git -C upstream/cnc-ddraw checkout <sha>`, затем
`git add upstream/cnc-ddraw`), наложите патчи на новое дерево (`git apply`), разрешите конфликты и
пересоберите. `build.ps1` всегда собирает из запиненного submodule + патчи, поэтому дерево апстрима не
редактируется на месте. Патчи трогают непересекающиеся файлы, поэтому порядок не важен.

## Что именно слинковано внутри C4dll-R.dll

Один бинарь, три слоя:

| Слой | Исходники | Назначение |
| --- | --- | --- |
| Ядро cnc-ddraw | все апстримные `src/*.c`: `dd`, `ddsurface`, `blt`, `config`, рендереры `render_ogl` / `render_d3d9` / `render_gdi`, `winapi_hooks`, `wndproc`, `hook`, `fps_limiter`, `utils`, `lodepng` (скриншоты), COM-прослойки `IDirectDraw*` / `IDirect3D*` | сама замена DirectDraw |
| render_null | добавляется `patches/cnc-ddraw-render-null.patch` | headless-бэкенд (`renderer=null`) для тест-харнессов, без видимого вывода |
| Microsoft Detours | апстримный `src/detours/` | function/IAT-хуки для cnc-ddraw и слоя фич |
| Слой C4dll-R | `features/rendererbridge.c`, `c4features.cpp`, `featuremenu.cpp`, `pluginhost.cpp`, `timerhost.cpp`, `localization.cpp`, `savelogic.cpp`, `cursorfix.cpp`, `headless.cpp` | интеграция врапера, меню, плагины, локаль, сейвы/архив, защита курсора D2 и headless-окна |

Экспорты: 483 форварда CodeBase (`name=CB63.name`) плюс `DDReloadConfig` (живое перечтение
настроек) и `DDTakeScreenshot`. `Mods\timer.c4p` собирается отдельно из `plugins/timer/` и внутрь
DLL НЕ входит.

Почему одна DLL: игра уже импортирует библиотеку с именем `C4dll-R` (копию CodeBase), поэтому
замена одного файла даёт рендерер, меню и хост плагинов сразу, без отдельного `ddraw.dll`,
который мог бы кого-то перекрыть или быть перекрытым.

## Первый запуск и файлы настроек

Три файла, три владельца:

| Файл | Кто создаёт | Что хранит |
| --- | --- | --- |
| `ddraw.ini` | лежит в релизном zip (рекомендованные значения); если отсутствует, C4dll-R при первом запуске создаст настроенный под Disciples II (`patches/cnc-ddraw-default-ini.patch`) | рендерер, режим окна, разрешение, шейдер, капы производительности |
| `C4menu.ini` | создаётся меню при первом запуске | игровые тумблеры: always active, скорости анимаций, attack burst, drag-scroll и автоподтверждение найма; плюс `language` (auto/en/ru) и `debugLog` (0 = не писать файлы C4menu-<pid>.log / C4plugins.log, по умолчанию; 1 или env `C4DLL_DEBUG` включает диагностику) |
| `Disciple.ini` | собственный/совместимый со старым врапером файл игры | родные пресеты скорости, редакторский `ScenEditDatabase`, `[Wrapper] Locale`, `Archive` и `IncludeSubdirectories` |

Конвертация старых настроек: при первом запуске (когда `C4menu.ini` ещё нет) меню читает
легаси-файл `mss32menu.ini`, секцию `[menu]` (старый конфиг меню mss32-мода), и конвертирует:
`alwaysActive` переносится как есть; старый `animationSpeedEnabled=1` превращается в скорость боя
1.5x, иначе берётся дефолт 2x. Больше ничего не читается, а `Disciple.ini` и
`Scripts\settings.lua` игры конвертация не трогает никогда. Существующий `C4menu.ini` повторно не
генерируется: файл принадлежит пользователю.

Если `ddraw.ini` отсутствует совсем, автосозданный файл - это больше не апстрим-сток:
`cfg_create_ini` пишет настроенный под Disciples II конфиг (`patches/cnc-ddraw-default-ini.patch`) -
`fake_mode=1024x768x16`, `renderer=opengl`, окно с настоящим заголовком (`windowed=true`,
`border=true`, `resizable=false`), `width=800`/`height=600`, `maintas=true`, шейдер xBRZ freescale,
`devmode=true`, `singlecpu=true`, `nonexclusive=true`, `noactivateapp=true`, `maxfps=144`,
`maxgameticks=100`, `vsync=true`, привычные горячие клавиши и `savesettings=0`, чтобы cnc-ddraw
не переписывал файл и не срезал комментарии. Комментарии лежат прямо в файле и объясняют каждый
выбор; парсер ini берёт значением всё после `=`, поэтому все комментарии - отдельными строками.
В zip по-прежнему лежит рекомендованный `ddraw.ini` (родное разрешение, тянущееся окно, шейдер
Lanczos, `savesettings=1`) - удалите его, если хотите сравнить с генерируемым.

## Справочник настроек: ddraw.ini (значения из комплекта)

«сразу» = меню применяет сразу через `DDReloadConfig`; «рестарт» = вступает в силу со следующего
запуска игры.

| Ключ | В комплекте | Эффект | Применение |
| --- | --- | --- | --- |
| `fake_mode` | `1024x768x16` | фейкает 16-битный десктоп для проверки глубины цвета в игре | рестарт |
| `renderer` | `opengl` | шейдеры + лучший апскейл; `auto` сначала берёт D3D9 (без шейдерных фильтров); `gdi` = софтверный; если OpenGL не поднялся, cnc-ddraw сам откатится на GDI | рестарт |
| `windowed` + `fullscreen` | `true` + `false` | окно с заголовком и меню; `true`+`true` = borderless на весь экран; `false`+любое = эксклюзивный фулскрин | сразу |
| `border` | `true` | настоящий заголовок окна (окно можно таскать); меню-бар под ним | сразу |
| `resizable` | `true` | окно тянется за края; пропорции держит `maintas` | сразу |
| `width`, `height` | `0`, `0` | размер вывода; 0 = родной размер игры (1024x768) | сразу |
| `maintas` | `true` | держать 4:3, без растягивания на широких экранах | сразу |
| `boxing` | `false` | целочисленный масштаб (чёткие пиксели + рамки); выкл = заполнять окно | сразу |
| `shader` | `lanczos2-sharp` | фильтр апскейла, только для OpenGL; в меню 8 пресетов | сразу |
| `savesettings` | `1` | cnc-ddraw сам сохраняет размер/позицию/состояние окна при выходе | - |
| `maxgameticks` | `100` | кап игрового цикла, тиков/с; см. раздел «Кап скорости игры» | рестарт |
| `maxfps` | `-1` | кап FPS рендера, -1 = частота монитора; крутит только поток рендера, игру не замедляет | рестарт |
| `vsync` | `false` | вертикальная синхронизация; нужна только от разрывов в эксклюзивном фулскрине (в окне и безрамочном режиме разрывов не бывает благодаря композиции DWM), стоит немного задержки вывода | сразу |
| `singlecpu` | `true` | прижать процесс к одному ядру (дефолт cnc-ddraw для старых игр; кандидат на `false`, см. «Экспериментальное»; меню: Производительность > Одно ядро CPU) | рестарт |
| `noactivateapp` | `true` | продолжать рендер без фокуса (логическую половину даёт пункт меню "Always active") | рестарт |
| `nonexclusive` | `true` | не брать эксклюзивный DirectDraw; надёжные меню/видео | рестарт |
| `adjmouse` | `true` | масштабировать курсор под окно | сразу |
| `devmode` | `true` | курсор не запирается в окне (родное оконное поведение); если что-то заперло: Ctrl+Tab или RAlt+RCtrl | сразу |
| `keytogglefullscreen` ... | см. файл | горячие клавиши VK-кодами, 0x00 отключает | - |
| `resolutions`, `fixchilds` | `0`, `2` | список видеорежимов и обработка дочерних окон; для D2 менять не нужно | - |

Предупреждение о парсере: комментарии только отдельной строкой. Всё после `=`, включая хвостовые
пробелы, считается значением, поэтому инлайновый `; комментарий` молча ломает настройку.

## Кап скорости игры (maxgameticks)

Настройка важнее FPS. Движок проводит каждую игровую команду (ход, действие боя, переход
диалога) через внутреннюю цепочку, которая прокачивается по одному шагу за итерацию главного
цикла, а `maxgameticks` этот цикл ограничивает. Замерено на реальных сессиях: одна команда
занимает около 10 шагов цикла, поэтому пол реакции на команду примерно `10 / maxgameticks`
секунд:

| maxgameticks | Пол реакции на команду | CPU |
| --- | --- | --- |
| 30 | ~330 мс: игра заметно «думает» перед каждым действием | самый холодный |
| 60 | ~170 мс | низкий |
| 100 (в комплекте) | ~100 мс | умеренный |
| -1 (без капа) | насколько хватит CPU | одно ядро занято |

Старому «ColdCPU» из DisciplesGL соответствует 30. В комплекте 100: почти мгновенные реакции при
ограниченной цене по CPU. Заметьте: `maxfps` такого эффекта не имеет, он крутит только поток
рендера.

## Внутриигровое меню

Полное игровое меню устанавливается для exe Русобита. Проверенный exe редактора сценариев получает
адресно-независимое меню **Файл / Видео / Производительность / Плагины**; для неподдерживаемых
exe остаётся только рендерер. В оконном режиме бар появляется под заголовком окна.

Меню двуязычное: `C4menu.ini` `[menu] language` = `auto` (по умолчанию) / `en` / `ru`. При `auto`
меню русское, когда язык интерфейса Windows русский, системная кодовая страница 1251 или выбрана
русская локаль текста игры; иначе английское. Выбор доступен в **Игра > Язык меню** и применяется
после перезапуска игры. В подменю есть серые строки-подсказки, объясняющие каждую опцию.

### Простое увеличение

`Ctrl+колесо мыши` повторяет simple zoom из DisciplesGL 2.0.2 и в игре, и в редакторе сценариев:
колесо вверх добавляет 0,1x, вниз убавляет 0,4x, диапазон 1,0x..8,0x, изображение остаётся
привязанным к позиции курсора. Масштаб действует до завершения процесса, не сохраняется и работает
в OpenGL, D3D9/Auto и GDI. Обычное колесо сохраняет поведение оригинального врапера — передаёт
стрелку вверх/вниз. `mss32.dll` не проверяется и не вызывается.

### Редактор сценариев

**Файл > Режим редактора > Сценарии / Кампании** пишет штатный ключ
`Disciple.ini` `[Disciple] ScenEditDatabase=0/1` и просит перезапустить редактор — как старый
врапер. Для переключателя не используются адреса ни `ScenEdit.exe`, ни `mss32.dll`; при этом сам
поддерживаемый `ScenEdit.exe` по-прежнему проверяется существующим условием по размеру PE.

### Игра

| Пункт | Что делает | Куда пишет | Применение |
| --- | --- | --- | --- |
| Язык меню | `Авто` / `English` / `Русский`; автоматический режим учитывает язык Windows, CP1251 и выбранную локаль текста игры | `C4menu.ini` `language` | после перезапуска |
| Всегда активна | игра продолжает работать (без паузы), когда окно теряет фокус; точечный патч проверки активации, сверяется с оригинальными байтами перед записью | `C4menu.ini` `alwaysActive` | сразу |
| Перетаскивание карты | зажать ЛКМ на карте и тянуть = панорамирование; обычный клик по-прежнему выбирает (доставляется при отпускании); скролл от края окна при включённом пункте отключён | `C4menu.ini` `dragScroll` | сразу |
| Автоподтверждение найма воинов | пропускает только вопрос «Хотите нанять этого воина?» штатным callback `BTN_YES` в активный ход локального игрока; по умолчанию выключено | `C4menu.ini` `autoConfirmUnitHire` | сразу |
| Скорость боя (весь бой): Выкл / 1.5x / 2x (по умолчанию) / 3x / 4x / 5x / 15x | умножает тайминг всех боевых анимаций через виртуальные часы (редирект `timeGetTime`); память игры не патчится | `C4menu.ini` `battleAnimEnabled` + `battleAnimSpeed` | сразу |
| Ускорение атак (рывок на каждый удар): Выкл / 1.5x .. 5x (по умолчанию) / 15x | дополнительный множитель только пока проигрывается удар/эффект (около 1.2 с, плавный спад за 0.7 с), поверх скорости боя | `C4menu.ini` `battleAttackEnabled` + `battleAttackSpeed` | сразу |
| Скорость анимаций карты: Выкл (по умолчанию) / 1.5x .. 15x | те же виртуальные часы на стратегической карте (вода, флаги, эффекты) | `C4menu.ini` `mapAnimEnabled` + `mapAnimSpeed` | сразу |
| Скорость боя (опция игры): Медленно / Нормально / Быстро / Мгновенно | СОБСТВЕННАЯ опция игры, та же, что в её настройках | `Disciple.ini` `BattleSpeed` | со следующего боя |
| Скорость передвижения на карте (опция игры): Нормально / Быстро / Очень быстро | СОБСТВЕННАЯ опция игры: скорость шага ваших и вражеских отрядов, читается при старте движения | `Disciple.ini` `PlayerSpeed` + `OpponentSpeed` | со следующего движения |

Пункты 15x - тестовые пресеты, преувеличены намеренно.

### Видео

| Пункт | Что делает | Куда пишет | Применение |
| --- | --- | --- | --- |
| Режим экрана: Оконный / Полный экран без рамки / Полный экран эксклюзивный | стиль окна; эксклюзивный делает настоящую смену видеорежима и откатывается в безрамочный там, где она невозможна (RDP); Alt+Enter переключает | `ddraw.ini` `windowed` + `fullscreen` | сразу |
| Разрешение: Родное .. 3840x2160 | размер окна вывода; Родное = игровые 1024x768 | `ddraw.ini` `width` + `height` | сразу |
| Фильтр / масштабирование: Lanczos (лучший для графики D2) / xBRZ / Bicubic / AMD FSR / xBR lv2 / Bilinear / Без фильтра / CRT | шейдер апскейла | `ddraw.ini` `shader` | сразу, только OpenGL |
| Рендерер (рестарт): OpenGL (рекомендуется) / GDI / Auto | бэкенд рендера; Auto сначала берёт D3D9, у которого нет шейдерных фильтров | `ddraw.ini` `renderer` | рестарт |
| Держать 4:3 | леттербокс вместо растягивания на широких экранах | `ddraw.ini` `maintas` | сразу |
| VSync | лечит разрывы в эксклюзивном фулскрине ценой небольшой задержки вывода; в окне и безрамочном режиме разрывов нет и так (композиция DWM), там держите выключенным | `ddraw.ini` `vsync` | сразу |
| Целочисленный масштаб | пиксель-в-пиксель с рамками; держите OFF, чтобы заполнять окно | `ddraw.ini` `boxing` | сразу |
| Сделать скриншот (PrintScreen) | скриншот средствами рендерера | - | - |

### Производительность

| Пункт | Что делает | Куда пишет | Применение |
| --- | --- | --- | --- |
| Кап FPS (рестарт): Частота монитора / 30 / 60 / 144 | только FPS рендера, логику игры не замедляет | `ddraw.ini` `maxfps` | рестарт |
| Кап скорости игры (рестарт): Без капа / 30 / 60 / 100 (по умолчанию) | кап игрового цикла, см. раздел выше | `ddraw.ini` `maxgameticks` | рестарт |
| Одно ядро CPU (рестарт) | прижимает весь процесс к ядру 0 (страховка cnc-ddraw для старых игр, ON по умолчанию); OFF - экспериментальный вариант производительности, см. «Экспериментальное» | `ddraw.ini` `singlecpu` | рестарт |

### Плагины

Появляется, когда в `Mods\` есть плагины: подменю `Нативные (.c4p)` и `Старые (.mod)`, каждое
подцепляет собственное меню плагина. Комплектный плагин Timer настраивается через
`C4plugins.ini` и документируется отдельно.

## Работа с сейвами (все версии игры)

`features/savelogic.cpp` переносит файловую логику старого врапера без единого адреса
`Discipl2.exe`. Перехватываются WinAPI-функции открытия, закрытия и перечисления файлов, поэтому
одна реализация работает с exe Акеллы, Русобита, GOG и редактора:

- зажать **Ctrl** при подтверждении обычного сохранения — записать следующий `QuickSaveNNN.sg`
  рядом с выбранным сейвом;
- `[Wrapper] Archive=1` в `Disciple.ini` (дефолт, если ключа нет) — после закрытия сейва скопировать
  его в `Archive\YYYYMMDD\~имя-YYYYMMDD-HHMMSS-маркер.sg`;
- зажать **Shift** при сохранении — принудительно архивировать именно этот сейв даже при
  `Archive=0`;
- `[Wrapper] IncludeSubdirectories=1` (дефолт `0`) — рекурсивно показывать `.sg` из `Archive\` в
  обычном игровом списке загрузки/сохранения.

Параметры перечитываются в момент операции: после правки `Disciple.ini` не нужны ни пересборка,
ни перезапуск игры. Игровой ключ `[Settings] AutoSave` к этому коду не относится. При
`[menu] debugLog=1` события сейвов дополнительно пишутся в `C4saves-<pid>.log`.

## Локализация текста игры (все версии игры)

`features/localization.cpp` реализует совместимый со старым врапером ключ
`[Wrapper] Locale=<LCID>` без жёсткой пары 866/1251. OEM- и ANSI-кодовые страницы и `LC_CTYPE`
msvcrt берутся из выбранной локали Windows; мост `OemToCharA`/`CharToOemA` переключается вживую.
По умолчанию используется `GetUserDefaultLCID`, `Locale=0` отключает перекодировку врапером.
В сборке Русобита это вынесено в **Игра > Локализация текста игры**: список собирается из локалей,
установленных в Windows, выбранный LCID пишется в `Disciple.ini`. На остальных версиях тот же
безадресный мост работает, но ключ пока задаётся вручную, поскольку их адресное меню не ставится.

## Экспериментальное

- Шаги 15x у burst/скоростей: тестовое преувеличение.
- `C4menu.ini` `perUnitBurst`: burst только для действующих юнитов; пункта меню нет, по умолчанию
  выключено, не доделано.
- Одно ядро CPU (`singlecpu`): страховка cnc-ddraw для старых игр. При `true` (по умолчанию) весь
  процесс прижимается к ядру 0 на Windows 10 (`SetProcessAffinityMask(1)`; на Windows 11 24H2+
  вместо этого мягкий пер-поточный механизм). Прижим защищает древние игры от дрейфа таймеров и
  багов миграции между ядрами, но и сериализует всё на одном ядре: поток рендера с приоритетом
  ABOVE_NORMAL вытесняет игровой поток на каждом показанном кадре, а любой всплеск фонового
  потока превращается в прямую задержку игры. У D2 известных проблем с миграцией ядер нет,
  поэтому `false` - кандидат по производительности; A/B-прогонов ещё НЕ было, поэтому дефолт
  остаётся `true`. Переключатель: Производительность > Одно ядро CPU (рестарт), либо `singlecpu=`
  в ddraw.ini; это обычное значение конфига, пересборка не нужна. Как тестировать: выключить,
  перезапустить игру, сыграть бой и несколько ходов по карте; следить за заиканием звука и
  странностями таймингов (тестировать СО ЗВУКОМ - заикание и есть известный симптом), затем
  оставить то, что ощущается лучше.
- `renderer=null`: норендер-бэкенд для тест-харнессов; ничего не рисуется, окно игры остаётся
  ОБЫЧНЫМ окном. Там, где среда вообще не может создать настоящие top-level окна (null-драйвер
  Wine, без X-сервера), тот же запуск сам откатывается на message-only окна: фолбэк взводится
  только после того, как создание настоящего окна ПРОВАЛИЛОСЬ, а message-only повтор удался,
  поэтому на десктопе он не срабатывает никогда.

## В системе нет OpenGL (VM, RDP): Mesa и dxil.dll

Не входит в релиз. Там, где рабочего драйвера OpenGL нет, рядом с exe можно положить
Windows-дистрибутив Mesa3D (`opengl32.dll` + `libgallium_wgl.dll` и сопутствующие): cnc-ddraw
будет рендерить GL через Mesa, обычно через её D3D12-бэкенд. В такой конфигурации `dxil.dll` из
того же комплекта Mesa ОБЯЗАТЕЛЕН: без него D3D12-драйвер не может подписывать шейдеры и игра
молча завершается на старте. Вне Mesa-конфигурации `dxil.dll` не делает ничего и с C4dll-R не
поставляется.
