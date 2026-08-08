# C4dll-R (self-contained DirectDraw wrapper for Disciples II)

This folder builds a single drop-in **`C4dll-R.dll`** DirectDraw wrapper for Disciples II. It is one
self-contained assembly: the [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) renderer is
**embedded** (no
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
| `patches/cnc-ddraw-output-downscale.patch` | Allows filtered output below the logical game canvas only with mouse-coordinate remapping, including manual window resize | yes |
| `patches/cnc-ddraw-decorative-background.patch`, `features/decorative.cpp`, `features/decor/` | Presentation-only DisciplesGL background and Alternative frame around classic 4:3 screens | yes |
| `features/featuremenu.cpp` | The in-game menu + feature hooks, self-contained (no mss32 deps) | yes |
| `features/widebattle.cpp`, `DLG_BATTLE_B.dlg` | Signature-gated Widescreen Battle hooks for the original D2 2.00-3.01 layout table + embedded 990-wide dialog, derived from DisciplesGL under MIT | yes |
| `features/horplus.cpp` | Signature-gated true Hor+ game-canvas presets reconstructed from the legacy wrapper | yes |
| `features/clouds.cpp` | Signature-gated loader, archive lookup and update pipeline for an external `Imgs\IsoClouds.ff` | yes |
| `release/Shaders/` | The eight OpenGL presets exposed by the menu, including their multipass files and retained license headers | yes |
| `features/rendererbridge.c` | Wrapper-owned adapters to cnc-ddraw internals: live reload, screenshot, coordinate mapping and simple-zoom state/formula | yes |
| `features/localization.cpp` | Locale/encoding bridge modelled after the legacy wrapper; no hard-coded Russian code pages | yes |
| `features/savelogic.cpp`, `cursorfix.cpp` | Version-independent save/archive hooks and the Disciples II edge-scroll guard | yes |
| `docs/hook-points.md` | Every MNS/SMNS game address/structure C4dll-R attaches to | yes |
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
   then attaches a real menu bar (Game / Video / Performance / Plugins) through the renderer's
   window-procedure integration. The validated MNS/SMNS layouts can additionally use their native
   address detour. Renderer settings are written to `ddraw.ini` and re-applied live via the renderer's
   own `DDReloadConfig`; screenshots use `DDTakeScreenshot`.
5. The **Game** menu also carries gameplay features that hook the exe by address (validated MNS/SMNS
   layouts only, all SEH/signature-guarded, with defaults documented below): live battle/map
   animation-speed multipliers and per-hit attack burst,
   map drag-scroll and **Skip voiced event dialogs** (auto-close a `DLG_EVENT_POPUP` after its
   voiceover finishes and append its text to `dialog-vo-log.txt`). Every game address and structure
   these touch is listed in [`docs/hook-points.md`](docs/hook-points.md).

## Build

```powershell
./build.ps1                 # build only  -> build/cnc-ddraw/bin/Release/C4dll-R.dll
./build.ps1 -Deploy         # build, back up the game's baseline once, then swap in the monolith
./build.ps1 -Restore        # put the baseline C4dll-R.dll + standalone ddraw.dll back
```

`build.ps1` copies the pinned `upstream/cnc-ddraw` submodule to `build/`, applies six focused patches
(`cnc-ddraw-c4dll-r` integration + `render-null` + `default-ini` + `simple-zoom` +
`output-downscale` + `decorative-background`), copies in the
wrapper-owned feature sources (renderer bridge, menu, Hor+, Widescreen Battle, clouds, plugins,
locale, saves, cursor guard and headless mode), embeds the reviewed wide-battle dialog and decorative
resources as RCDATA,
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
with `Shaders`, `INSTALL.txt`, a sample `C4plugins.ini`, `ddraw.ini`, `LICENSE` and third-party notices into `C4dll-R-v1.0.zip`, and publishes a GitHub
Release with that zip, a `-symbols.zip` (the matching PDBs for crash triage) and the loose
`C4dll-R.dll` + `timer.c4p` attached. The release version is stamped into the DLL version
resource (`build.ps1 -Version`), so a build is identifiable from file properties. Running the
workflow manually (workflow_dispatch) publishes a **prerelease** tagged `c4dll-r-dev-<sha>` (or
your label); versions containing `rc` / `alpha` / `beta` / `dev` are always marked prerelease. The
package sources live in `c4ddraw/release/` (`INSTALL.txt`, `C4plugins.ini`, `ddraw.ini`,
`Shaders/`, `THIRD_PARTY_NOTICES.txt`, `RELEASE_NOTES.md`); the repository-root GPL-3.0 `LICENSE`
is copied into the archive by the workflow.

## Deploy by hand

Put `C4dll-R.dll` next to `Discipl2.exe` (replacing the CodeBase copy), keep `CB63.dll` and
`ddraw.ini` there. If the folder also contains a standalone `ddraw.dll` from another wrapper,
rename or remove that file; a clean installation normally has none, so otherwise do nothing.
To A/B test our-vs-stock, swap `C4dll-R.dll` only.

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
| C4dll-R layer | `features/rendererbridge.c`, `c4features.cpp`, `featuremenu.cpp`, `decorative.cpp`, `horplus.cpp`, `widebattle.cpp`, `clouds.cpp`, `pluginhost.cpp`, `timerhost.cpp`, `localization.cpp`, `savelogic.cpp`, `cursorfix.cpp`, `headless.cpp` | wrapper integration, menu, presentation-only decorative background, true Hor+ game canvas, wide battle, external cloud archive pipeline, plugins, locale conversion, save/archive logic, D2 cursor guard and headless windowing |

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
| `C4menu.ini` | generated by the menu on first launch | gameplay toggles: animation speed, attack burst, drag-scroll and unit-hire auto-confirm; the presentation-only `decorativeBackground` toggle (on by default); the retained internal `wideBattle` default/config flag has no menu item in 1.5; plus `language` (auto/en/ru) and `debugLog` (0 = no C4menu-<pid>.log / C4plugins.log files, default; 1 or the `C4DLL_DEBUG` env var enables diagnostics) |
| `Disciple.ini` | the game's own/wrapper-compatible file | native `[Disciple] DisplaySize`, `[Settings] IsoBirds` (map-cloud visibility), Hor+ `[Wrapper] GameCanvasMode/Width/Height`, native speed presets, editor `ScenEditDatabase`, `[Wrapper] Locale`, `Archive` and `IncludeSubdirectories` |

Old settings conversion: on first launch (no `C4menu.ini` yet) the menu reads the legacy
`mss32menu.ini` `[menu]` section (the old mss32-mod menu config) and converts it: `alwaysActive`
carries over as is; old `animationSpeedEnabled=1` maps to battle speed 1.5x, otherwise the default
2x is used. Nothing else is read, and the conversion never touches the game's `Disciple.ini` or
`Scripts\settings.lua`. Once `C4menu.ini` exists it is never regenerated: the user owns it.

If `ddraw.ini` is missing entirely, the generated default is no longer the upstream stock one:
`cfg_create_ini` writes a Disciples II tuned config (`patches/cnc-ddraw-default-ini.patch`) -
`fake_mode=1024x768x16`, `renderer=opengl`, windowed with a real title bar (`windowed=true`,
`border=true`, `resizable=true`), `width=0`/`height=0` (follow the active game resolution), `maintas=true`, the Lanczos
shader, `devmode=true`, `singlecpu=true`, `nonexclusive=true`, `noactivateapp=true`, `maxfps=144`,
`maxgameticks=100`, `vsync=true`, the usual renderer hotkeys, and `savesettings=0` so cnc-ddraw never
rewrites the file and strips its comments. The comments are carried in the file and explain every
choice; the ini parser takes everything after `=` as the value, so all comments sit on their own
lines. The zip still ships the recommended `ddraw.ini` (native resolution, resizable window,
Lanczos shader, `savesettings=1`) - delete it to compare against the generated one.

`F4` is handled by C4dll-R itself: it switches a normal window to the last selected fullscreen kind
(borderless on the first use), and either fullscreen kind back to a normal window. `Alt+Enter`
retains cnc-ddraw's configured window/fullscreen toggle; `Alt+F4` still closes the game.

## 1.6: как выбирается режим экрана

Основной путь теперь снова похож на один пункт Resolution старого DisciplesGL: **Видео ->
Разрешение -> Автоматическое разрешение** одновременно выбирает логический кадр игры и возвращает
обычное окно в режим `width=0`, `height=0`, то есть окно следует кадру без скрытого второго размера.
Для обычного окна автоматика вычитает из рабочей области рамку, заголовок, строку меню и панель
задач; для borderless/exclusive использует весь монитор. Из подходящего семейства она берёт самый
крупный проверенный кадр, который помещается: штатный `DisplaySize` на 4:3/5:4 или настоящий Hor+
на широком экране. Целый коэффициент больше не важнее обзора карты — дробное увеличение аккуратно
делает шейдер. Пример: на 1920x1080 полноэкранный режим выбирает Hor+ 1920x1080, а обычное окно на
том же мониторе обычно 1600x900, чтобы системные элементы оставались на экране; на 3840x2160
выбирается 2560x1440, а не уменьшенный 1920x1080 ради 2x.

Ручные штатные и широкие размеры находятся в двух компактных подменю и тоже привязывают обычное
окно к выбранному кадру; нужен полный перезапуск игры. Только пункт **Дополнительно: изменить только
окно/вывод для стрима** снова разъединяет размеры: он меняет внешний вывод сразу, но не добавляет
обзор карты. **Вписать / Целые блоки / Растянуть** определяют лишь геометрию уже выбранной пары.

## 1.6: how the screen mode is selected

The main path is again close to the original DisciplesGL's single Resolution command: **Video ->
Resolution -> Automatic resolution** selects the logical game canvas and resets the normal-window
output to `width=0`, `height=0`, so the window follows that canvas instead of retaining a hidden
second size. For a normal window Auto subtracts the frame, title, menu row and taskbar from the work
area; borderless/exclusive uses the full monitor. It then picks the largest fitting validated canvas
from the appropriate family: stock `DisplaySize` on 4:3/5:4, true Hor+ on a wide display. Integer
scaling no longer wins at the cost of map area; the selected shader handles a fractional scale. For
example, a 1920x1080 monitor selects Hor+ 1920x1080 fullscreen but normally 1600x900 in a decorated
window, while 3840x2160 selects 2560x1440 instead of dropping to 1920x1080 merely for 2x.

Manual stock and widescreen sizes live in two compact submenus and also re-link the normal window;
they require a full game restart. Only **Advanced: change window/stream output only** separates the
sizes again: it changes the outer output live without adding map view. **Fit / Integer pixel blocks /
Stretch** only decide how the already selected pair is mapped.

## Resolution pipeline: game resolution -> window/screen -> scaling

“Resolution” still has four internal stages. The regular Resolution commands keep the first two
linked; only the explicitly Advanced output command separates them:

| Stage | Setting | What it changes | Applies |
| --- | --- | --- | --- |
| Game resolution | stock `[Disciple] DisplaySize`, or the widescreen `[Wrapper] GameCanvasMode/Width/Height` override | how many logical pixels the game creates and lays out: strategic view, UI and battle room | full game restart |
| Window / screen size | **Video -> Resolution -> Advanced output**, `ddraw.ini` `width`/`height`, or manual resize | render target around the selected game resolution; it never adds game content | live or resize |
| Output geometry | `maintas`, `boxing`, `aspect_ratio` | how the game resolution is mapped into the output viewport | live |
| Sampling | `shader` | how source pixels are reconstructed after the geometry is known | live, OpenGL only |

The nine recognized D2 2.00-3.01 executable layouts create their DirectDraw surfaces and dependent
map buffers once during startup. In the unified **Video -> Resolution** popup, three entries marked
`★` are the game's stock modes and a separately labelled submenu shows all validated widescreen
modes; the right column gives each aspect ratio. **Automatic resolution** chooses between both
families: 4:3/5:4 targets use one of the three stock `DisplaySize` modes, while 3:2 and wider targets
use a validated Hor+ canvas. It selects the largest canvas that fits the current normal-window work
area or the fullscreen monitor. Every game-view entry changes the logical game size, re-links normal
output to it and therefore requires a full restart. The live Advanced output dialog remains at the
bottom of this popup.

| Stock game modes | Setting |
| ---: | --- |
| 800x600 (4:3) | `GameCanvasMode=0`, `[Disciple] DisplaySize=0` |
| 1024x768 (4:3) | `GameCanvasMode=0`, `[Disciple] DisplaySize=1` |
| 1280x1024 (5:4) | `GameCanvasMode=0`, `[Disciple] DisplaySize=2` |

| Widescreen logical canvases | Setting |
| ---: | --- |
| 1066x600 | `GameCanvasMode=1`, `GameCanvasWidth=1066`, `GameCanvasHeight=600` |
| 1152x648 | `GameCanvasMode=1`, `GameCanvasWidth=1152`, `GameCanvasHeight=648` |
| 1280x720 | `GameCanvasMode=1`, `GameCanvasWidth=1280`, `GameCanvasHeight=720` |
| 1366x768 | `GameCanvasMode=1`, `GameCanvasWidth=1366`, `GameCanvasHeight=768` |
| 1440x810 | `GameCanvasMode=1`, `GameCanvasWidth=1440`, `GameCanvasHeight=810` |
| 1536x864 | `GameCanvasMode=1`, `GameCanvasWidth=1536`, `GameCanvasHeight=864` |
| 1600x900 | `GameCanvasMode=1`, `GameCanvasWidth=1600`, `GameCanvasHeight=900` |
| 1820x1024 | `GameCanvasMode=1`, `GameCanvasWidth=1820`, `GameCanvasHeight=1024` |
| 1920x1080 | `GameCanvasMode=1`, `GameCanvasWidth=1920`, `GameCanvasHeight=1080` |
| 2560x1440 | `GameCanvasMode=1`, `GameCanvasWidth=2560`, `GameCanvasHeight=1440` |

A widescreen canvas is not calculated from `DisplaySize`: at startup it replaces the stock mode.
The equal-height pairs make the relationship easy to compare: 800x600 -> 1066x600,
1024x768 -> 1366x768, and 1280x1024 -> 1820x1024. Widescreen expands the logical map view rather
than stretching the output. Explicit `GameCanvasMode=0` selects the stock `DisplaySize`;
`GameCanvasMode=1` keeps an explicitly selected Hor+ canvas; `GameCanvasMode=2` is monitor-adaptive.
While either wide mode is selected, the wrapper stores `DisplaySize=0` only as the internal
compatibility selector used by the original Hor+ patch. It does **not** reduce the actual game
resolution to 800x600: the Hor+ width and height are the DirectDraw canvas. Stock `DisplaySize` and
Hor+ are alternative layout families, so placing 1280x1024 underneath Hor+ would not add pixels or
remove bars; it would only mix an unpatched stock layout with the wide hooks.
When the key is absent on a recognized layout, adaptive mode is the default. For example, 1920x1080
fullscreen selects Hor+ 1920x1080 at 1x, 3840x2160 selects Hor+ 2560x1440 at 1.5x, and 1280x1024
fullscreen selects the game's stock 1280x1024 mode. A normal decorated window may deliberately use
the next smaller fitting canvas. Manual stock and Hor+ selections are never replaced by automation.

The signature-gated game hooks change the DirectDraw mode, strategic-map layout and
dimension-dependent allocations. If the executable is not one of the nine validated game layouts or
the required bytes do not match, no partial patch is left behind: the menu item remains visible but
disabled and the game keeps its native resolution.

The menu shows `current -> after restart` and previews the future output viewport without pretending
the running game changed. After a different game size is successfully saved, an information dialog explicitly
asks you to close the whole game and start it again; selecting the already-requested canvas does not
show a false restart prompt. `fake_mode=1024x768x16` is only cnc-ddraw's internal virtual 16-bit
compatibility bootstrap; it is not the game resolution, window/output size, or scaling mode. With a
validated widescreen canvas, C4dll-R corrects that process-local bootstrap geometry and advertises
the exact Hor+ mode during DirectDraw enumeration without writing the adjusted dimensions back to
`ddraw.ini`.

The legacy wrapper could override `DisplaySize=0` with `[Wrapper] DisplayWidth` /
`DisplayHeight`. That is why an old config containing `DisplaySize=0`, `DisplayWidth=1024` and
`DisplayHeight=768` still produced a 1024x768 game canvas and allowed WideBattle. cnc-ddraw does
not read those legacy keys. On a recognized game layout, C4dll-R imports the three exact stock pairs below once,
only while `DisplaySize` is absent or zero, preserving the corresponding stock mode.
It never treats an arbitrary legacy size as a widescreen patch dimension. It keeps the old keys for
rollback, writes
`[Wrapper] LegacyDisplaySizeMigrated=1`, and never guesses an equivalent for an arbitrary size or
overrides an explicit nonzero `DisplaySize`:

| Legacy DisplayWidth x DisplayHeight | New setting |
| ---: | --- |
| 800x600 | `[Disciple] DisplaySize=0` |
| 1024x768 | `[Disciple] DisplaySize=1` |
| 1280x1024 | `[Disciple] DisplaySize=2` |

`ddraw.ini width=0` and `height=0` makes a normal window/exclusive mode follow the active game
resolution, while borderless automatically uses the desktop. A fixed output size is an absolute
number of output pixels and is not rewritten when the game resolution changes. With `adjmouse=true`
(the shipped value), it may be smaller than the logical canvas: cnc-ddraw filters the finished image
to the window and maps mouse coordinates back to game pixels. Exclusive fullscreen can additionally
fall back to a supported display mode, so its requested size is not guaranteed.

The menu preserves the boundary without exposing two equal-weight choices: selecting a game
resolution writes restart-latched `Disciple.ini` values, sets next-start output to `0,0`, and does
not reload the running game's canvas. The scaling lines preview the selected next canvas while still
identifying live game resolution and output. One **Video -> Resolution** popup contains Automatic,
compact manual stock/widescreen submenus, and one explicitly **Advanced output** dialog. A custom
image area from 320x240 through 8192x8192 writes `width`/`height` to the effective
`ddraw.ini` section and applies it live. The full downscale range uses the shipped `adjmouse=true`;
if mouse remapping was disabled by hand, the dialog raises its minimum to the running game canvas
so clicks cannot become desynchronized. Existing hand-edited values remain supported.

The scale is a result, not another resolution setting. For game size `G` and output size `O`:

- **Fit** uses one coefficient `k = min(Ow/Gw, Oh/Gh)` and centers approximately
  `Gw*k x Gh*k`; geometry stays correct and the remainder becomes bars.
- **Integer** (`boxing`) searches for the largest exact multiplier from 19 down to 1 and uses
  `G*N`; one game pixel then occupies an `N x N` block, i.e. `N^2` output pixels. If the output is
  smaller than `G`, no positive integer factor exists and the renderer performs filtered reduction.
- **Stretch** uses independent `kx=Ow/Gw` and `ky=Oh/Gh`; unequal values distort geometry.
- A custom `aspect_ratio` changes the target geometry; the shader never changes any of these
  coefficients. `Ctrl+Wheel` zoom is applied after this base viewport and is not part of the base
  scale shown by the menu.

The scaling result line reports `1:1` only when the final viewport has exactly the same width and
height as the logical game canvas: one game pixel equals one output pixel. It is a dynamic result,
not a resolution or scaling mode, and no resolution entry is decorated with it. The value applies
equally to stock and widescreen canvases; a pending exclusive-mode result remains an estimate because
the display driver may select a fallback mode.

For example, these are different choices even though both may mention 1920x1080:

- **Manual output size 1920x1080** around native 800x600 gives a 1440x1080 fitted 4:3 viewport at 1.8x,
  or a centered exact 800x600 viewport in Integer mode. It adds no strategic-map content.
- **Game resolution 1920x1080** makes the game itself lay out a wider 1920x1080 strategic view;
  Automatic output then follows it, while a fixed output is independently fitted, boxed or stretched.

At Integer 2x, one 800x600 game pixel becomes a 2x2 block and therefore needs at least 1600x1200
output; 1024x768 needs 2048x1536, and 1280x1024 needs 2560x2048. Game resolution and output must never
be treated as interchangeable just because their numeric dimensions happen to match.

WideBattle in the legacy wrapper was reverse-checked, including its embedded dialog resource and
hook sites. It selects a fixed 990x600 battle layout instead of the stock 800x600 one, keeps both
unit panels visible, moves the units/controls/background, and fixes side selection and item hit
areas. It does not choose the game resolution, output size, scaling or strategic-map view. The
current port intentionally gates it on the **actual game-resolution width >= 990**, never on the
output/window width. It is enabled by default and latched when the next battle opens; 1.5 exposes
no user-facing WideBattle switch.

Map clouds use the real legacy pipeline rather than a renderer effect: when the executable and
archive are supported, signature-gated hooks enlarge the owning game object, load and index
`Imgs\IsoClouds.ff`, redirect the `CLOUD*` resources and run their initialization/update path.
The archive is an external asset from an existing DisciplesGL installation and is not
redistributed. Its reviewed SHA-256 is
`962F334E1CFA3226AF27B953AF0F6EBA6C1F82EF708A948C0D4C2A76FF804EE6`.
**Game -> Map clouds** is a restart-only alias for the game's native `[Settings] IsoBirds` option,
which is the single source of truth. The short-lived development key `[menu] clouds` is migrated
once and removed. If the archive is absent or its exact hash is different, or the executable
signatures do not match, the item is unavailable and no cloud patch is applied.

## Settings reference: ddraw.ini (shipped defaults)

"live" = the menu applies it instantly through `DDReloadConfig`; "restart" = takes effect on the
next game start.

| Key | Shipped | Effect | Applies |
| --- | --- | --- | --- |
| `fake_mode` | `1024x768x16` | internal virtual 16-bit compatibility bootstrap, not a game/window resolution or scale; a validated widescreen canvas corrects only the process-local bootstrap geometry without rewriting this value | restart |
| `renderer` | `opengl` | shaders + best upscaling; `auto` picks D3D9 first (no shader filters); `gdi` = software; if OpenGL fails, cnc-ddraw falls back to GDI on its own | restart |
| `windowed` + `fullscreen` | `true` + `false` | windowed with a title bar and the menu; `true`+`true` = borderless fullscreen; `false`+`false` = exclusive fullscreen (legacy `false`+`true` is migrated) | live |
| `border` | `true` | real title bar, draggable window; the menu bar sits under it | live |
| `resizable` | `true` | window edges resize; aspect is kept by `maintas` | live |
| `width`, `height` | `0`, `0` | output target selected by Video -> Resolution -> Window/output; `0,0` follows the active game resolution, while borderless always uses the desktop. With `adjmouse=true`, a smaller window is allowed and the finished canvas is filtered down without changing the logical game view | live, start or manual resize |
| `maintas` | `true` | fit while preserving the selected game resolution's aspect, with letter/pillar-boxing as needed | live |
| `boxing` | `false` | largest exact integer fit from 19x down to 1x; takes priority over `maintas`. At 2x one game pixel occupies a 2x2 block = 4 output pixels. Below 1x no integer factor exists, so the renderer uses filtered reduction | live |
| `aspect_ratio` | empty | custom aspect override; the menu shows it as Custom and clears it when Fit / Integer / Stretch is selected | live |
| `shader` | `lanczos2-sharp` | upscale filter, OpenGL renderer only; the menu offers 8 presets | live |
| `savesettings` | `1` | cnc-ddraw writes window size/pos/state back on exit | - |
| `maxgameticks` | `100` | game loop cap in ticks/s; see "The game speed cap" below | restart |
| `maxfps` | `-1` | render FPS cap, -1 = screen refresh; paces the render thread only, never slows the game | restart |
| `vsync` | `false` | vertical sync; needed only against tearing in exclusive fullscreen (windowed and borderless never tear thanks to DWM composition), costs a little display lag | live |
| `singlecpu` | `true` | 1 CPU stability mode; enable it if the game randomly crashes/freezes on maps. cnc-ddraw applies it once at startup; Windows 11 24H2+ uses its game-thread policy instead of pinning external audio helpers | restart |
| `noactivateapp` | `true` | keep renderer output updating while the window is unfocused | restart |
| `nonexclusive` | `true` | never take exclusive DirectDraw; reliable menus/videos | restart |
| `adjmouse` | `true` | scale the cursor to the window size | live |
| `devmode` | `true` | cursor not clipped to the window (original windowed feel); Ctrl+Tab or RAlt+RCtrl release it if anything clips | live |
| `keytogglefullscreen` ... | see file | cnc-ddraw hotkeys as VK codes, 0x00 disables; C4dll-R's `F4` toggle is wrapper-owned and independent of these keys | - |
| `resolutions`, `fixchilds` | `0`, `2` | mode-list and child-window handling; fine as is for D2 | - |

Parser warning: comments only on their own lines. Everything after `=` including trailing spaces
is the value, so an inline `; comment` silently breaks the setting.

cnc-ddraw may select a process-specific section such as `[Discipl2]` or `[Discipl2/2]` before
falling back to `[ddraw]`. Explicit menu choices use that same effective section, so a per-game
override cannot silently replace a setting just chosen in the menu. cnc-ddraw's own automatic
window-state save is narrower: `savesettings=1` writes `[ddraw]`, while other nonzero values write
the base process section. The menu treats a live resize/hotkey mode as a next-start value only when
that destination cannot be shadowed by the active section.

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

The menu is available on MNS/SMNS, Akella, GOG and unrecognized game executables. The universal
renderer, **Video**, **Performance**, menu-language and game-text-locale controls remain active on
all of them. Game resolution and Widescreen Battle additionally recognize nine validated D2
2.00-3.01 game layouts by PE `ProductVersion` plus a code probe. The remaining exact-address
gameplay controls are prefixed **`(MNS/SMNS)`**; they remain visible but disabled on every other
executable, so no unvalidated address is patched. Here MNS/SMNS means the two validated
`Discipl2.exe` layouts identified by file size: **4,187,648** and **4,214,272** bytes.
The validated Scenario Editor exe gets its address-free **File / Video / Performance / Plugins**
menu. In windowed mode the bar appears under the title bar.

The menu is bilingual: `C4menu.ini` `[menu] language` = `auto` (default) / `en` / `ru`. With
`auto` the menu is Russian when the Windows UI language is Russian or the system codepage is 1251,
English otherwise. Submenus carry grayed one-line hints explaining each option.

### Simple zoom

`Ctrl+Mouse Wheel` reproduces the DisciplesGL 2.0.2 simple zoom in both the game and Scenario
Editor: wheel up adds 0.1x, wheel down subtracts 0.4x, range 1.0x..8.0x, and the image stays anchored
at the cursor. It is process-local, not persisted, and works in OpenGL, D3D9/Auto and GDI. Ctrl+Wheel
is consumed after changing zoom; an ordinary wheel remains a normal `WM_MOUSEWHEEL` and no longer
generates artificial Up/Down map movement. Mouse hit coordinates follow the zoomed image. It does
not inspect or call `mss32.dll`.

### Scenario Editor

**File > Editor mode > Scenarios / Campaigns** writes the editor's native
`Disciple.ini` `[Disciple] ScenEditDatabase=0/1` setting and asks for an editor restart, exactly like
the legacy wrapper. The switch uses no executable or `mss32.dll` addresses; the exact supported
`ScenEdit.exe` is still validated by the existing PE-size check.

### Game

| Item | What it does | Saved to | Applies |
| --- | --- | --- | --- |
| Menu language | Auto / English / Russian; Auto considers the Windows language/codepage and selected game-text locale | `C4menu.ini` `language` | after restart |
| Game text locale | selects the Windows locale used for the wrapper's OEM/ANSI text conversion, or disables wrapper recoding | `Disciple.ini` `[Wrapper] Locale` | live |
| (MNS/SMNS) Map drag-scroll (left button) | enabled by default; hold LMB on the map and drag to pan. The exact button-down point is retained and the first changed game pixel moves the map immediately, without re-anchoring to a tile centre. A down/up without movement still selects (delivered on release). Native window-edge scroll remains available and is suppressed only during an active drag | `C4menu.ini` `dragScroll` | live |
| (MNS/SMNS) Auto-confirm unit hire | skips only “Do you want to hire this unit?” by invoking its normal `BTN_YES` callback during the local player's turn; off by default | `C4menu.ini` `autoConfirmUnitHire` | live |
| (MNS/SMNS) Battle speed (whole battle): Off / 1.5x / 2x (default) / 3x / 4x / 5x / 15x | multiplies all battle animation timing via a virtual clock (a `timeGetTime` redirect); no game memory is patched | `C4menu.ini` `battleAnimEnabled` + `battleAnimSpeed` | live |
| (MNS/SMNS) Attack speed-up (burst on each hit): Off / 1.5x .. 5x (default) / 15x | switches the battle clock to the selected factor from the effect-start callback until the engine reports that the final visual component ended, then returns to idle linearly over 300 ms. A signature-gated hook supplies the exact end, with the timed fallback retained if that hook is unavailable | `C4menu.ini` `battleAttackEnabled` + `battleAttackSpeed` | live |
| (MNS/SMNS) Map animation speed: Off (default) / 1.5x .. 15x | the same virtual clock on the strategic map (water, flags, effects). `+/-` changes this preset on the map and the battle-animation preset while a battle is visible; main and numpad keys are supported | `C4menu.ini` `mapAnimEnabled` + `mapAnimSpeed` | live |
| (MNS/SMNS) Battle speed (game option): Slow / Normal / Fast / Instant | the game's OWN option, same as its settings screen | `Disciple.ini` `BattleSpeed` | next battle |
| (MNS/SMNS) Map movement speed (game option): Normal / Fast / Very fast | the game's OWN option: walk speed of player and AI stacks, read when a move starts | `Disciple.ini` `PlayerSpeed` + `OpponentSpeed` | next move |
| (MNS/SMNS) Map clouds | loads and animates the real external `Imgs\IsoClouds.ff` through the legacy allocation/archive/resource pipeline; the menu aliases the game's native visibility option and is unavailable without the validated archive or matching executable signatures | `Disciple.ini` `[Settings] IsoBirds` | full restart |

The 15x entries are test presets, exaggerated on purpose.
Widescreen Battle is intentionally absent from the 1.5 menu. On a recognized original layout it
remains enabled by default and selects the fixed 990x600 two-panel dialog when the next battle opens.

### Video

| Item | What it does | Saved to | Applies |
| --- | --- | --- | --- |
| Display mode: Windowed / Fullscreen (adaptive borderless) / Fullscreen exclusive (advanced) | borderless automatically uses the desktop size; exclusive performs a real mode change and can fall back to borderless where impossible (RDP). The menu bar is shown only in a normal window. `F4` and `Alt+Enter` reliably return even from real exclusive fullscreen, restore a normal-window client fitted to the monitor work area instead of reusing fullscreen geometry, and save the resulting mode for the next start. On the first fullscreen transition, an automatic canvas shows its next-start result; a manually fixed canvas offers to enable Automatic resolution with the exact reviewed size it would select | `ddraw.ini` `windowed` + `fullscreen`; optional Auto choice also writes `Disciple.ini` | live; canvas choice applies after a full restart |
| Decorative background around classic screens | on an active widescreen game canvas, fills the area outside a centered fixed-size screen with the DisciplesGL background and Alternative frame. It is enabled by default and included in wrapper screenshots. A stock native canvas has no internal free area, so the item is disabled there; a separately enlarged Window/stream output can still have renderer-owned black bars | `C4menu.ini` `decorativeBackground` | live after a widescreen game-resolution restart |
| Resolution -> Automatic / Manual stock / Manual widescreen | Automatic selects the largest fitting validated game canvas for the persisted display mode; manual sizes live in two compact submenus. All regular choices re-link normal output to the game canvas. Widescreen shows more map rather than stretching output; unsupported executable layouts gray the unavailable game-view rows | stock `[Disciple] DisplaySize`; widescreen `[Wrapper] GameCanvasMode/Width/Height`; output `width=0`, `height=0` | full restart |
| Resolution -> Advanced output | opens a numeric width/height dialog in the same popup and deliberately separates output from game view. Automatic follows the selected game resolution; with mouse remapping enabled, a custom 320x240..8192x8192 value is the image area inside the window, excluding frame/title/menu. Borderless still uses the desktop | effective `ddraw.ini` `width` + `height` | live and next start |
| Scaling: Fit / Integer pixel blocks / Stretch / Custom | maps the logical game canvas into the actual window/desktop. The result line shows the coefficient, viewport and bars; `1:1` means one game pixel equals one output pixel. With mouse remapping active, the window may be smaller than the canvas and the selected shader filters it down | `ddraw.ini` `maintas` + `boxing` + `aspect_ratio` | live |
| Filter: Lanczos / xBRZ / Bicubic / AMD FSR / xBR lv2 / Bilinear / None / CRT | OpenGL sampling filter for enlargement or reduction; all eight presets and required multipass files ship in `Shaders/`. Lanczos, Bicubic and Bilinear suit fractional downscaling | `ddraw.ini` `shader` | live, OpenGL only |
| Renderer (restart): OpenGL (recommended) / GDI / Auto | rendering backend; Auto picks D3D9 first, which has no shader filters | `ddraw.ini` `renderer` | restart |
| VSync | fixes tearing in exclusive fullscreen at the cost of a little display lag; windowed and borderless never tear (DWM composition), so keep it off there | `ddraw.ini` `vsync` | live |
| Take screenshot (PrintScreen) | saves a screenshot via the renderer | - | - |

The widescreen game resolution and Widescreen Battle remain independent. The former expands the
strategic view and requires a full restart; the latter selects a fixed 990-wide battle dialog for
the next battle. Changing only the window size or scaling mode affects neither. The long
output-preset submenu is intentionally absent; the Window/output dialog at the bottom of the same
Resolution popup handles Automatic and arbitrary persisted dimensions, while borderless continues
to follow the desktop.

### Performance

| Item | What it does | Saved to | Applies |
| --- | --- | --- | --- |
| Frame cap (restart): Monitor refresh rate / 30 / 60 / 144 | render FPS only, never slows game logic | `ddraw.ini` `maxfps` | restart |
| Game speed cap (restart): Uncapped / 30 / 60 / 100 (default) | the game loop cap, see the section above | `ddraw.ini` `maxgameticks` | restart |
| 1 CPU stability (restart) | helps prevent random crashes/freezes on maps; ON by default. On Windows 10/Wine it pins the process to logical CPU 0; Windows 11 24H2+ pins only game-owned threads and leaves external audio helpers alone | `ddraw.ini` `singlecpu` | full restart |

### Plugins

Loads only native `Mods\*.c4p` plugins and grafts each plugin directly under **Plugins**. The bundled
Timer is configured via `C4plugins.ini`; its countdown uses `TableDuration_0`. Hold **Ctrl+Alt** and
drag the clock with LMB to reposition it. On the exact Russobit/MNS layout, Force mode starts only
after the player accepts the native beginning-of-turn summary with **OK**; the visible dialog does
not consume time, and a started Force clock cannot be manually paused. Other builds keep the prior
active-turn edge because no unverified callback address is used. The plugin overlay stays below an
open native menu and same-process dialogs, so the timer cannot cover menu commands.

The default sword shown over a decorative frame uses the same live X/Y viewport scale as the
game-rendered cursor. It therefore keeps the same size and proportions when crossing between the
game screen and an extended frame, including a deliberately stretched output.

## Save handling (all game versions)

`features/savelogic.cpp` ports the legacy wrapper's file-level save conveniences without using a
single `Discipl2.exe` address. It detours Win32 file enumeration/open/close APIs, so the same code
applies to the Akella, MNS/SMNS, GOG and editor executables:

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
is `GetUserDefaultLCID`; `Locale=0` disables wrapper recoding. On MNS/SMNS, Akella, GOG and
unrecognized game executables it is exposed as **Game > Game text locale**, enumerating the locales
installed in Windows and writing the selected LCID to `Disciple.ini`.

## 1 CPU stability mode

`singlecpu=true` is the shipped and generated default. If the game randomly crashes or freezes on
otherwise unrelated maps, enable **Performance > 1 CPU stability (restart)** and fully restart the
game. The menu only saves the next-start value to `ddraw.ini`; it deliberately does not reload the
current process into a mixed affinity state. At startup, Windows 10 and Wine use cnc-ddraw's
process-wide logical-CPU-0 policy. Native Windows 11 24H2+ follows cnc-ddraw's newer policy: the
process remains unrestricted, game-owned current and future threads are pinned to logical CPU 0,
and external audio/helper threads are deliberately left alone. Disable it only to diagnose a
specific performance or sound issue, then restart again.

## Experimental

- Attack burst 15x steps: test-only exaggeration.
- `C4menu.ini` `perUnitBurst`: per-unit burst scoping; no menu item, off by default, unfinished.
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

# C4dll-R (самодостаточный DirectDraw-врапер для Disciples II)

Эта папка собирает единый подключаемый **`C4dll-R.dll`** — DirectDraw-врапер для Disciples II.
Это одна самодостаточная сборка: рендерер
[cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) **встроен**
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
| `patches/cnc-ddraw-output-downscale.patch` | Разрешает фильтрованный вывод меньше логического кадра только при пересчёте координат мыши, включая ручной resize окна | да |
| `patches/cnc-ddraw-decorative-background.patch`, `features/decorative.cpp`, `features/decor/` | Фон DisciplesGL и рамка Alternative вокруг классических экранов 4:3, только на этапе вывода | да |
| `features/featuremenu.cpp` | Внутриигровое меню + хуки фич, самодостаточное (без зависимостей mss32) | да |
| `features/widebattle.cpp`, `DLG_BATTLE_B.dlg` | Защищённые сигнатурами хуки широкого боя для исходной таблицы D2 2.00-3.01 + встроенная раскладка диалога шириной 990, перенесённые из DisciplesGL по MIT | да |
| `features/horplus.cpp` | Защищённые сигнатурами пресеты настоящего Hor+ кадра игры, восстановленные по старому враперу | да |
| `features/clouds.cpp` | Защищённые сигнатурами загрузка, поиск ресурсов и обновление внешнего `Imgs\IsoClouds.ff` | да |
| `release/Shaders/` | Восемь OpenGL-пресетов из меню, включая multipass-файлы и сохранённые заголовки лицензий | да |
| `features/rendererbridge.c` | Собственные адаптеры врапера к внутренностям cnc-ddraw: live reload, скриншот, перевод координат, состояние и формула simple zoom | да |
| `features/localization.cpp` | Мост локали/кодировок по образцу старого врапера, без жёстких русских кодовых страниц | да |
| `features/savelogic.cpp`, `cursorfix.cpp` | Независимые от версии хуки сейвов/архива и защита edge-scroll для Disciples II | да |
| `docs/hook-points.md` | Все адреса/структуры MNS/SMNS, к которым цепляется C4dll-R | да |
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
   `featuremenu_install()` крепит настоящий меню-бар (Game / Video / Performance / Plugins) через
   интеграцию оконной процедуры рендерера. Проверенные раскладки MNS/SMNS дополнительно могут
   использовать свой адресный детур. Настройки рендерера пишутся в `ddraw.ini` и применяются вживую
   через `DDReloadConfig`; скриншоты делает `DDTakeScreenshot`.
5. Меню **Game** также несёт геймплейные фичи, которые цепляются к exe по адресам (только проверенные
   раскладки MNS/SMNS; всё защищено SEH/сигнатурами, дефолты описаны ниже): живые множители скорости анимации боя/карты и бонус на
   удар, перетаскивание карты и **пропуск озвученных диалогов** (авто-закрытие `DLG_EVENT_POPUP`
   после озвучки + запись текста в `dialog-vo-log.txt`). Все адреса и структуры игры, к которым
   они цепляются, перечислены в [`docs/hook-points.md`](docs/hook-points.md).

## Сборка

```powershell
./build.ps1                 # только сборка -> build/cnc-ddraw/bin/Release/C4dll-R.dll
./build.ps1 -Deploy         # сборка, разовый бэкап baseline игры, затем подмена на монолит
./build.ps1 -Restore        # вернуть baseline C4dll-R.dll + отдельный ddraw.dll
```

`build.ps1` копирует запиненный submodule `upstream/cnc-ddraw` в `build/`, накладывает шесть
тематических патчей (`cnc-ddraw-c4dll-r` + `render-null` + `default-ini` + `simple-zoom` +
`output-downscale` + `decorative-background`) и копирует
собственные исходники features (renderer bridge, меню, Hor+, широкий бой, облака, плагины,
локаль, сейвы, cursor guard и headless), встраивает проверенный диалог широкого боя и декоративные
ресурсы как RCDATA,
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
`Shaders`, `INSTALL.txt`, примером `C4plugins.ini`, `ddraw.ini`, `LICENSE` и notices в `C4dll-R-v1.0.zip` и опубликует GitHub Release с этим
архивом, `-symbols.zip` (соответствующие PDB для разбора крашей) и отдельными файлами
`C4dll-R.dll` + `timer.c4p`. Версия релиза зашивается в ресурс версии DLL (`build.ps1 -Version`),
так что сборка опознаётся по свойствам файла. Ручной запуск workflow (workflow_dispatch) публикует
**пре-релиз** с тегом `c4dll-r-dev-<sha>` (или вашей меткой); версии, содержащие `rc` / `alpha` /
`beta` / `dev`, всегда помечаются пре-релизом. Исходники пакета — в `c4ddraw/release/`
(`INSTALL.txt`, `C4plugins.ini`, `ddraw.ini`, `Shaders/`, `THIRD_PARTY_NOTICES.txt`,
`RELEASE_NOTES.md`); корневой GPL-3.0 `LICENSE` workflow копирует в архив.

## Ручная установка

Положите `C4dll-R.dll` рядом с `Discipl2.exe` (заменив копию CodeBase), оставьте `CB63.dll` и
`ddraw.ini`. Если в папке есть отдельный `ddraw.dll` от другого врапера, переименуйте или удалите
его; в чистой установке такого файла обычно нет, и тогда ничего делать не нужно. Для сравнения
наш/сток меняйте только `C4dll-R.dll`.

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
| Слой C4dll-R | `features/rendererbridge.c`, `c4features.cpp`, `featuremenu.cpp`, `decorative.cpp`, `horplus.cpp`, `widebattle.cpp`, `clouds.cpp`, `pluginhost.cpp`, `timerhost.cpp`, `localization.cpp`, `savelogic.cpp`, `cursorfix.cpp`, `headless.cpp` | интеграция врапера, меню, декоративный фон только на этапе вывода, настоящий Hor+ кадр, широкий бой, pipeline внешнего архива облаков, плагины, локаль, сейвы/архив, защита курсора D2 и headless-окна |

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
| `C4menu.ini` | создаётся меню при первом запуске | игровые тумблеры: скорости анимаций, attack burst, drag-scroll, облака карты и автоподтверждение найма; тумблер оформления `decorativeBackground` (включён по умолчанию); сохранённый внутренний флаг `wideBattle` не имеет пункта меню в 1.5; плюс `language` (auto/en/ru) и `debugLog` (0 = не писать файлы C4menu-<pid>.log / C4plugins.log, по умолчанию; 1 или env `C4DLL_DEBUG` включает диагностику) |
| `Disciple.ini` | собственный/совместимый со старым врапером файл игры | штатный `[Disciple] DisplaySize`, Hor+ `[Wrapper] GameCanvasMode/Width/Height`, родные пресеты скорости, редакторский `ScenEditDatabase`, `[Wrapper] Locale`, `Archive` и `IncludeSubdirectories` |

Конвертация старых настроек: при первом запуске (когда `C4menu.ini` ещё нет) меню читает
легаси-файл `mss32menu.ini`, секцию `[menu]` (старый конфиг меню mss32-мода), и конвертирует:
`alwaysActive` переносится как есть; старый `animationSpeedEnabled=1` превращается в скорость боя
1.5x, иначе берётся дефолт 2x. Больше ничего не читается, а `Disciple.ini` и
`Scripts\settings.lua` игры конвертация не трогает никогда. Существующий `C4menu.ini` повторно не
генерируется: файл принадлежит пользователю.

Если `ddraw.ini` отсутствует совсем, автосозданный файл - это больше не апстрим-сток:
`cfg_create_ini` пишет настроенный под Disciples II конфиг (`patches/cnc-ddraw-default-ini.patch`) -
`fake_mode=1024x768x16`, `renderer=opengl`, окно с настоящим заголовком (`windowed=true`,
`border=true`, `resizable=true`), `width=0`/`height=0` (следовать активному разрешению игры), `maintas=true`, шейдер Lanczos,
`devmode=true`, `singlecpu=true`, `nonexclusive=true`, `noactivateapp=true`, `maxfps=144`,
`maxgameticks=100`, `vsync=true`, привычные горячие клавиши рендерера и `savesettings=0`, чтобы cnc-ddraw
не переписывал файл и не срезал комментарии. Комментарии лежат прямо в файле и объясняют каждый
выбор; парсер ini берёт значением всё после `=`, поэтому все комментарии - отдельными строками.
В zip по-прежнему лежит рекомендованный `ddraw.ini` (родное разрешение, тянущееся окно, шейдер
Lanczos, `savesettings=1`) - удалите его, если хотите сравнить с генерируемым.

`F4` обрабатывается самим C4dll-R: из обычного окна он включает последний выбранный вид полного
экрана (при первом нажатии — безрамочный), а из любого полного экрана возвращает обычное окно.
`Alt+Enter` остаётся настроенным переключателем окно/полный экран cnc-ddraw; `Alt+F4` по-прежнему
закрывает игру.

## Цепочка разрешения: разрешение игры -> окно/экран -> масштабирование

Внутри остаются четыре этапа. Обычный выбор «Разрешение» связывает первые два; разъединяет их
только явно дополнительная настройка вывода:

| Этап | Настройка | Что меняет | Применение |
| --- | --- | --- | --- |
| Разрешение игры | штатный `[Disciple] DisplaySize` или широкая подмена `[Wrapper] GameCanvasMode/Width/Height` | сколько логических пикселей создаёт и раскладывает сама игра: стратегический обзор, UI и поле боя | полный перезапуск игры |
| Размер окна / экрана | **Видео -> Разрешение -> Доп. вывод**, `ddraw.ini` `width`/`height` или ручной resize | цель вывода вокруг выбранного разрешения игры; нового игрового содержимого не добавляет | сразу или resize |
| Геометрия вывода | `maintas`, `boxing`, `aspect_ratio` | как разрешение игры вписывается во viewport вывода | сразу |
| Фильтрация | `shader` | как пересчитываются исходные пиксели после выбора геометрии | сразу, только OpenGL |

Девять распознаваемых раскладок exe D2 2.00–3.01 создают поверхности DirectDraw и зависимые буферы
карты один раз при старте. В едином меню **Видео -> Разрешение** три пункта со знаком `★` являются
штатными режимами игры, а отдельное подменю показывает все проверенные широкие варианты; справа
указаны пропорции. «Автоматическое разрешение» выбирает между семействами: для цели 4:3/5:4 — один
из трёх штатных `DisplaySize`, для 3:2 и шире — проверенный кадр Hor+. Берётся самый крупный кадр,
который помещается в рабочую область обычного окна либо в полный монитор. Каждый игровой пункт
привязывает к кадру обычное окно и требует полного перезапуска.

| Штатные режимы игры | Настройка |
| ---: | --- |
| 800x600 (4:3) | `GameCanvasMode=0`, `[Disciple] DisplaySize=0` |
| 1024x768 (4:3) | `GameCanvasMode=0`, `[Disciple] DisplaySize=1` |
| 1280x1024 (5:4) | `GameCanvasMode=0`, `[Disciple] DisplaySize=2` |

| Широкие логические кадры | Настройка |
| ---: | --- |
| 1066x600 | `GameCanvasMode=1`, `GameCanvasWidth=1066`, `GameCanvasHeight=600` |
| 1152x648 | `GameCanvasMode=1`, `GameCanvasWidth=1152`, `GameCanvasHeight=648` |
| 1280x720 | `GameCanvasMode=1`, `GameCanvasWidth=1280`, `GameCanvasHeight=720` |
| 1366x768 | `GameCanvasMode=1`, `GameCanvasWidth=1366`, `GameCanvasHeight=768` |
| 1440x810 | `GameCanvasMode=1`, `GameCanvasWidth=1440`, `GameCanvasHeight=810` |
| 1536x864 | `GameCanvasMode=1`, `GameCanvasWidth=1536`, `GameCanvasHeight=864` |
| 1600x900 | `GameCanvasMode=1`, `GameCanvasWidth=1600`, `GameCanvasHeight=900` |
| 1820x1024 | `GameCanvasMode=1`, `GameCanvasWidth=1820`, `GameCanvasHeight=1024` |
| 1920x1080 | `GameCanvasMode=1`, `GameCanvasWidth=1920`, `GameCanvasHeight=1080` |
| 2560x1440 | `GameCanvasMode=1`, `GameCanvasWidth=2560`, `GameCanvasHeight=1440` |

Широкий кадр не вычисляется из `DisplaySize`, а заменяет штатный режим при старте. Для наглядности
пары одинаковой высоты: 800x600 -> 1066x600, 1024x768 -> 1366x768 и
1280x1024 -> 1820x1024. Широкий кадр расширяет логический обзор карты, а не растягивает вывод.
Явный `GameCanvasMode=0` выбирает штатный `DisplaySize`, `1` сохраняет вручную выбранный Hor+,
`2` включает автоподбор. Если ключ отсутствует на распознанной раскладке, по умолчанию включается
авто. При любом широком режиме враппер хранит `DisplaySize=0` только как внутренний переключатель
совместимости исходного Hor+-патча. Это **не** уменьшает игру до 800x600: настоящей DirectDraw-
поверхностью становятся выбранные ширина и высота Hor+. Штатный `DisplaySize` и Hor+ — два
альтернативных семейства раскладки; подложенный под Hor+ режим 1280x1024 не добавит пикселей и не
уберёт поля, а лишь смешает непатченую штатную раскладку с широкими хуками.
Например, полный экран 1920x1080 выбирает Hor+ 1920x1080 при 1x, 3840x2160 — Hor+ 2560x1440
при 1.5x, а 1280x1024 — штатный режим игры 1280x1024. Обычное окно с рамкой может намеренно
получить следующий меньший помещающийся кадр. Явный ручной выбор автоматика не заменяет.

Защищённые сигнатурами хуки меняют режим DirectDraw, раскладку стратегической карты и зависящие от
размеров аллокации. Если exe не относится к девяти проверенным игровым раскладкам или обязательные
байты не совпали, частичного патча не остаётся: пункт меню виден, но выключен, а игра сохраняет штатное
разрешение.

Меню показывает `сейчас -> после перезапуска` и рассчитывает будущий viewport, не выдавая его за
уже изменившееся разрешение. После успешного сохранения действительно другого размера игры
информационная модалка прямо
просит полностью закрыть игру и запустить заново; повторный выбор уже запрошенного кадра не показывает
ложное требование перезапуска. `fake_mode=1024x768x16` — только внутренний виртуальный 16-битный
bootstrap совместимости cnc-ddraw, а не разрешение игры/окна или режим масштаба. При активном
проверенном широком кадре C4dll-R исправляет process-local геометрию bootstrap и добавляет точный
Hor+-режим в перечисление DirectDraw, не записывая подставленные размеры в `ddraw.ini`.

Старый враппер умел подменить `DisplaySize=0` через `[Wrapper] DisplayWidth` /
`DisplayHeight`. Поэтому старый конфиг с `DisplaySize=0`, `DisplayWidth=1024` и
`DisplayHeight=768` на деле создавал игровой кадр 1024x768 и разрешал WideBattle. cnc-ddraw
эти legacy-ключи не читает. На распознанной игровой раскладке C4dll-R один раз переносит только три точные штатные пары
ниже и только если `DisplaySize` отсутствует или равен нулю, сохраняя совместимость штатного
fallback. Произвольный старый размер не используется как размер широкого патча. Старые ключи
сохраняются для отката,
ставится `[Wrapper] LegacyDisplaySizeMigrated=1`; произвольный размер не угадывается, а явно
заданный ненулевой `DisplaySize` не переопределяется:

| Старые DisplayWidth x DisplayHeight | Новая настройка |
| ---: | --- |
| 800x600 | `[Disciple] DisplaySize=0` |
| 1024x768 | `[Disciple] DisplaySize=1` |
| 1280x1024 | `[Disciple] DisplaySize=2` |

`ddraw.ini width=0` и `height=0` заставляет обычное окно/эксклюзивный режим следовать активному
разрешению игры, а borderless автоматически берёт рабочий стол. Фиксированный размер вывода —
абсолютное число выходных пикселей, которое не переписывается при смене разрешения игры. При
`adjmouse=true` (значение в комплекте) он может быть меньше логического кадра: cnc-ddraw фильтрует
готовое изображение до окна и пересчитывает мышь обратно в игровые координаты. Эксклюзивный режим
может подобрать другой поддерживаемый видеорежим, поэтому точный запрошенный размер не гарантирован.

Меню сохраняет границу, но не показывает два равноправных «разрешения»: игровой выбор пишет
restart-настройки `Disciple.ini`, ставит выводу следующего запуска `0,0` и не перезагружает живой
игровой кадр. Один popup **Видео -> Разрешение** содержит Авто, компактные ручные подменю штатных и
широких режимов и один явно дополнительный диалог вывода. Произвольная область изображения от
320x240 до 8192x8192 пишет `width`/`height` в эффективную секцию `ddraw.ini` и применяется сразу.
Полный диапазон уменьшения использует комплектное `adjmouse=true`; если пересчёт мыши был вручную
выключен, диалог поднимает минимум до текущего игрового кадра, чтобы клики не рассинхронизировались.
Старые значения, заданные вручную, продолжают работать.

Коэффициент — результат расчёта, а не ещё одно разрешение. Для размера игры `G` и вывода `O`:

- **Вписать** использует один коэффициент `k = min(Ow/Gw, Oh/Gh)` и центрирует примерно
  `Gw*k x Gh*k`; геометрия не искажается, остаток становится полями.
- **Целые блоки** (`boxing`) ищет наибольший точный множитель от 19 до 1 и берёт `G*N`; один
  пиксель игры занимает блок `N x N`, то есть `N^2` пикселей вывода. Если вывод меньше `G`,
  положительного целого множителя нет и рендерер выполняет фильтрованное уменьшение.
- **Растянуть** использует независимые `kx=Ow/Gw` и `ky=Oh/Gh`; при разных значениях геометрия
  искажена.
- Свой `aspect_ratio` меняет целевую геометрию; шейдер ни один из этих коэффициентов не меняет.
  Увеличение `Ctrl+колесо` применяется уже после базового viewport и не входит в показываемый
  меню базовый коэффициент.

Строка результата масштабирования показывает `1:1` только когда итоговый viewport точно совпадает
с логическим кадром: один пиксель игры равен одному пикселю вывода. Это динамический результат, а
не разрешение или режим масштаба; рядом с пунктами разрешения он не рисуется. Правило одинаково для
штатных и широких кадров. Для ожидающего перезапуска exclusive результат остаётся оценкой, поскольку
драйвер может выбрать запасной видеорежим.

Например, это разные выборы, хотя в обоих может встретиться 1920x1080:

- **Ручной вывод 1920x1080** вокруг штатного 800x600 даёт при «Вписать» viewport 1440x1080 с
  коэффициентом 1.8x, а при «Целых» — точный 800x600 по центру. Нового обзора карты нет.
- **Разрешение игры 1920x1080** заставляет саму игру разложить более широкий стратегический обзор
  1920x1080; вывод «Авто» следует ему, а фиксированный вывод независимо вписывает, боксит или
  растягивает готовый кадр.

Для целого 2x один пиксель кадра 800x600 становится блоком 2x2, поэтому нужен вывод минимум
1600x1200; для 1024x768 — 2048x1536, для 1280x1024 — 2560x2048. Разрешение игры и вывод нельзя
подменять друг другом только потому, что их числа случайно совпали.

WideBattle старого враппера сверен по его встроенному ресурсу диалога и местам хуков. Он выбирает
фиксированную боевую раскладку 990x600 вместо штатной 800x600, одновременно показывает обе панели
отрядов, двигает юнитов/кнопки/фон и исправляет выбор стороны мышью и hit-area предметов. Он не
выбирает разрешение игры, размер вывода, масштабирование или обзор стратегической карты. Текущий порт
намеренно проверяет **фактическую ширину разрешения игры >= 990**, а не размер окна/вывода. Режим
включён по умолчанию и фиксируется при открытии следующего боя; пользовательского переключателя в
меню 1.5 нет.

Облака карты используют настоящий legacy-pipeline, а не эффект рендерера: при поддерживаемых exe и
архиве защищённые сигнатурами хуки расширяют объект-владелец, загружают и индексируют
`Imgs\IsoClouds.ff`, перенаправляют ресурсы `CLOUD*` и запускают их инициализацию/обновление.
Архив — внешний файл из существующей установки DisciplesGL и в комплект не входит.
SHA-256 проверенного файла:
`962F334E1CFA3226AF27B953AF0F6EBA6C1F82EF708A948C0D4C2A76FF804EE6`.
**Game -> Облака на карте** — требующий перезапуска алиас штатной опции игры
`[Settings] IsoBirds`, которая служит единственным источником правды. Временный ключ ранней
разработки `[menu] clouds` однократно переносится и удаляется. Если архива нет, его точный хеш
отличается или сигнатуры exe не совпали, пункт недоступен и ни один облачный патч не применяется.

## Справочник настроек: ddraw.ini (значения из комплекта)

«сразу» = меню применяет сразу через `DDReloadConfig`; «рестарт» = вступает в силу со следующего
запуска игры.

| Ключ | В комплекте | Эффект | Применение |
| --- | --- | --- | --- |
| `fake_mode` | `1024x768x16` | внутренний виртуальный 16-битный bootstrap совместимости, а не разрешение игры/окна или масштаб; проверенный широкий кадр исправляет только process-local геометрию bootstrap без перезаписи значения | рестарт |
| `renderer` | `opengl` | шейдеры + лучший апскейл; `auto` сначала берёт D3D9 (без шейдерных фильтров); `gdi` = софтверный; если OpenGL не поднялся, cnc-ddraw сам откатится на GDI | рестарт |
| `windowed` + `fullscreen` | `true` + `false` | окно с заголовком и меню; `true`+`true` = borderless на весь экран; `false`+`false` = эксклюзивный фулскрин (старое `false`+`true` мигрирует автоматически) | сразу |
| `border` | `true` | настоящий заголовок окна (окно можно таскать); меню-бар под ним | сразу |
| `resizable` | `true` | окно тянется за края; пропорции держит `maintas` | сразу |
| `width`, `height` | `0`, `0` | цель вывода из Видео -> Разрешение -> Окно/вывод; `0,0` следует активному разрешению игры, а borderless всегда использует рабочий стол. При `adjmouse=true` окно может быть меньше кадра игры: готовая картинка фильтруется вниз, логический обзор не меняется | сразу, запуск или ручной resize |
| `maintas` | `true` | вписать с сохранением пропорций выбранного разрешения игры и полями при необходимости | сразу |
| `boxing` | `false` | наибольшее точное целочисленное вписывание от 19x до 1x; имеет приоритет над `maintas`. При 2x один пиксель игры занимает блок 2x2 = 4 пикселя вывода. Ниже 1x целого множителя нет, поэтому рендерер использует фильтрованное уменьшение | сразу |
| `aspect_ratio` | пусто | свой override пропорций; меню показывает его как Custom и очищает при выборе «Вписать / Целые / Растянуть» | сразу |
| `shader` | `lanczos2-sharp` | фильтр апскейла, только для OpenGL; в меню 8 пресетов | сразу |
| `savesettings` | `1` | cnc-ddraw сам сохраняет размер/позицию/состояние окна при выходе | - |
| `maxgameticks` | `100` | кап игрового цикла, тиков/с; см. раздел «Кап скорости игры» | рестарт |
| `maxfps` | `-1` | кап FPS рендера, -1 = частота монитора; крутит только поток рендера, игру не замедляет | рестарт |
| `vsync` | `false` | вертикальная синхронизация; нужна только от разрывов в эксклюзивном фулскрине (в окне и безрамочном режиме разрывов не бывает благодаря композиции DWM), стоит немного задержки вывода | сразу |
| `singlecpu` | `true` | режим стабильности 1 CPU; включите, если игра случайно вылетает/зависает на любых картах. cnc-ddraw применяет его один раз при запуске; Windows 11 24H2+ использует свою политику игровых потоков и не прижимает внешние аудиопотоки | рестарт |
| `noactivateapp` | `true` | продолжать обновлять изображение без фокуса окна | рестарт |
| `nonexclusive` | `true` | не брать эксклюзивный DirectDraw; надёжные меню/видео | рестарт |
| `adjmouse` | `true` | масштабировать курсор под окно | сразу |
| `devmode` | `true` | курсор не запирается в окне (родное оконное поведение); если что-то заперло: Ctrl+Tab или RAlt+RCtrl | сразу |
| `keytogglefullscreen` ... | см. файл | горячие клавиши cnc-ddraw VK-кодами, 0x00 отключает; переключатель C4dll-R по `F4` от этих ключей не зависит | - |
| `resolutions`, `fixchilds` | `0`, `2` | список видеорежимов и обработка дочерних окон; для D2 менять не нужно | - |

Предупреждение о парсере: комментарии только отдельной строкой. Всё после `=`, включая хвостовые
пробелы, считается значением, поэтому инлайновый `; комментарий` молча ломает настройку.

cnc-ddraw может выбрать секцию конкретного процесса — например `[Discipl2]` или `[Discipl2/2]` —
раньше общей `[ddraw]`. Явный выбор в меню пишется в ту же эффективную секцию, поэтому per-game
override его не отменит. Автосохранение состояния окна у cnc-ddraw уже: `savesettings=1` пишет в
`[ddraw]`, остальные ненулевые значения — в базовую секцию процесса. Меню считает живой resize
или hotkey-режим будущим значением только когда активная секция точно не перекроет это место.

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

Меню доступно на MNS/SMNS, Akella, GOG и неизвестных игровых exe. Универсальные пункты рендерера,
**Видео**, **Производительность**, язык меню и локаль текста работают на всех. Разрешение игры и
«Широкий бой» дополнительно распознают девять проверенных игровых раскладок D2 2.00–3.01
по PE `ProductVersion` и контрольной инструкции. Остальные точные адресные игровые функции имеют префикс
**`(MNS/SMNS)`** и остаются видимыми, но серыми на остальных exe, поэтому непроверенные адреса не
патчатся. MNS/SMNS здесь — две проверенные раскладки
`Discipl2.exe` размером **4 187 648** и **4 214 272** байта. Проверенный редактор сценариев получает
безадресное меню **Файл / Видео / Производительность / Плагины**. В оконном режиме бар появляется
под заголовком окна.

Меню двуязычное: `C4menu.ini` `[menu] language` = `auto` (по умолчанию) / `en` / `ru`. При `auto`
меню русское, когда язык интерфейса Windows русский, системная кодовая страница 1251 или выбрана
русская локаль текста игры; иначе английское. Выбор доступен в **Игра > Язык меню** и применяется
после перезапуска игры. В подменю есть серые строки-подсказки, объясняющие каждую опцию.

### Простое увеличение

`Ctrl+колесо мыши` повторяет simple zoom из DisciplesGL 2.0.2 и в игре, и в редакторе сценариев:
колесо вверх добавляет 0,1x, вниз убавляет 0,4x, диапазон 1,0x..8,0x, изображение остаётся
привязанным к позиции курсора. Масштаб действует до завершения процесса, не сохраняется и работает
в OpenGL, D3D9/Auto и GDI. `Ctrl+колесо` поглощается после изменения масштаба; обычное колесо
остаётся обычным `WM_MOUSEWHEEL` и больше не создаёт искусственное движение карты вверх/вниз.
Координаты клика следуют увеличенному изображению. `mss32.dll` не проверяется и не вызывается.

### Редактор сценариев

**Файл > Режим редактора > Сценарии / Кампании** пишет штатный ключ
`Disciple.ini` `[Disciple] ScenEditDatabase=0/1` и просит перезапустить редактор — как старый
врапер. Для переключателя не используются адреса ни `ScenEdit.exe`, ни `mss32.dll`; при этом сам
поддерживаемый `ScenEdit.exe` по-прежнему проверяется существующим условием по размеру PE.

### Игра

| Пункт | Что делает | Куда пишет | Применение |
| --- | --- | --- | --- |
| Язык меню | `Авто` / `English` / `Русский`; автоматический режим учитывает язык Windows, CP1251 и выбранную локаль текста игры | `C4menu.ini` `language` | после перезапуска |
| (MNS/SMNS) Перетаскивание карты | включено по умолчанию; запоминается точная точка нажатия, и уже первый изменившийся игровой пиксель сразу двигает карту без перепривязки к центру тайла. Нажатие/отпускание без движения по-прежнему выбирает объект (клик доставляется при отпускании). Штатный скролл у края окна подавляется только на время активного перетаскивания | `C4menu.ini` `dragScroll` | сразу |
| (MNS/SMNS) Автоподтверждение найма воинов | пропускает только вопрос «Хотите нанять этого воина?» штатным callback `BTN_YES` в активный ход локального игрока; по умолчанию выключено | `C4menu.ini` `autoConfirmUnitHire` | сразу |
| (MNS/SMNS) Скорость боя (весь бой): Выкл / 1.5x / 2x (по умолчанию) / 3x / 4x / 5x / 15x | умножает тайминг всех боевых анимаций через виртуальные часы (редирект `timeGetTime`); память игры не патчится | `C4menu.ini` `battleAnimEnabled` + `battleAnimSpeed` | сразу |
| (MNS/SMNS) Ускорение атак (рывок на каждый удар): Выкл / 1.5x .. 5x (по умолчанию) / 15x | включает выбранный фактор по callback начала эффекта и держит его до сообщения движка об окончании последней визуальной части, затем линейно возвращает idle за 300 мс. Точное окончание даёт hook с проверкой сигнатуры; для неизвестной раскладки exe сохраняется старый временной fallback | `C4menu.ini` `battleAttackEnabled` + `battleAttackSpeed` | сразу |
| (MNS/SMNS) Скорость анимаций карты: Выкл (по умолчанию) / 1.5x .. 15x | те же виртуальные часы на стратегической карте (вода, флаги, эффекты). `+/-` меняет этот пресет на карте и пресет всей боевой анимации во время боя; работают основные клавиши и numpad | `C4menu.ini` `mapAnimEnabled` + `mapAnimSpeed` | сразу |
| (MNS/SMNS) Скорость боя (опция игры): Медленно / Нормально / Быстро / Мгновенно | СОБСТВЕННАЯ опция игры, та же, что в её настройках | `Disciple.ini` `BattleSpeed` | со следующего боя |
| (MNS/SMNS) Скорость передвижения на карте (опция игры): Нормально / Быстро / Очень быстро | СОБСТВЕННАЯ опция игры: скорость шага ваших и вражеских отрядов, читается при старте движения | `Disciple.ini` `PlayerSpeed` + `OpponentSpeed` | со следующего движения |
| (MNS/SMNS) Облака на карте | загружает и анимирует настоящий внешний `Imgs\IsoClouds.ff` через legacy-pipeline аллокации, архива и ресурсов; пункт меню управляет штатной видимостью и недоступен без валидированного архива или совпавших сигнатур exe | `Disciple.ini` `[Settings] IsoBirds` | полный перезапуск |

Пункты 15x - тестовые пресеты, преувеличены намеренно.
«Широкий бой» намеренно отсутствует в меню 1.5. На распознанной исходной раскладке он остаётся
включённым по умолчанию и выбирает фиксированный двухпанельный диалог 990x600 при открытии боя.

### Видео

| Пункт | Что делает | Куда пишет | Применение |
| --- | --- | --- | --- |
| Режим экрана: Оконный / Полный экран (адаптивный без рамки) / Эксклюзивный полный экран (дополнительно) | безрамочный режим автоматически берёт размер рабочего стола; эксклюзивный делает настоящую смену видеорежима и может откатиться в безрамочный (например, в RDP). Меню-бар показывается только в обычном окне; `F4` и `Alt+Enter` надёжно возвращают даже из настоящего эксклюзивного режима, восстанавливают вписанную в рабочую область клиентскую часть обычного окна вместо повторного использования полноэкранной геометрии и сохраняют итоговый режим для следующего запуска. При первом переходе автоматический кадр показывает результат следующего старта, а для закреплённого вручную кадра предлагается включить Auto с точным рассчитанным размером | `ddraw.ini` `windowed` + `fullscreen`; при согласии на Auto также `Disciple.ini` | сразу; выбор кадра применяется после полного перезапуска |
| Декоративный фон вокруг классических экранов | при активном широком разрешении игры заполняет место вокруг центрированного фиксированного экрана фоном DisciplesGL и рамкой Alternative. Включён по умолчанию и попадает во встроенные скриншоты. В штатном кадре внутреннего свободного места нет, поэтому пункт серый; отдельное увеличение «окна/стрима» всё ещё может оставить чёрные поля самого рендерера | `C4menu.ini` `decorativeBackground` | сразу после перезапуска в широком разрешении игры |
| Разрешение -> Авто / Вручную штатное / Вручную широкое | Авто выбирает самый крупный помещающийся проверенный игровой кадр для сохранённого режима экрана; ручные размеры находятся в двух компактных подменю. Любой обычный выбор снова привязывает вывод окна к игровому кадру. Широкий режим добавляет обзор карты, а не растягивает вывод | штатный `[Disciple] DisplaySize`; широкий `[Wrapper] GameCanvasMode/Width/Height`; вывод `width=0`, `height=0` | полный перезапуск |
| Разрешение -> Доп. вывод | открывает числовой диалог ширины/высоты и намеренно отделяет вывод от игрового обзора. «Автоматически» следует выбранному разрешению игры; при включённом пересчёте мыши произвольное значение 320x240..8192x8192 задаёт область изображения внутри окна без рамки, заголовка и меню. Borderless всё равно использует рабочий стол | эффективная секция `ddraw.ini`, `width` + `height` | сразу и при следующем запуске |
| Масштаб: Вписать / Целые блоки пикселей / Растянуть / Custom | укладывает логический кадр игры в фактическое окно/рабочий стол. Строка результата показывает коэффициент, viewport и поля; `1:1` означает один игровой пиксель на один выходной. При пересчёте мыши окно может быть меньше кадра, тогда выбранный шейдер фильтрует изображение вниз | `ddraw.ini` `maintas` + `boxing` + `aspect_ratio` | сразу |
| Фильтр: Lanczos / xBRZ / Bicubic / AMD FSR / xBR lv2 / Bilinear / Без фильтра / CRT | OpenGL-фильтр увеличения или уменьшения; все восемь пресетов и необходимые multipass-файлы входят в `Shaders/`. Для дробного downscale подходят Lanczos, Bicubic и Bilinear | `ddraw.ini` `shader` | сразу, только OpenGL |
| Рендерер (рестарт): OpenGL (рекомендуется) / GDI / Auto | бэкенд рендера; Auto сначала берёт D3D9, у которого нет шейдерных фильтров | `ddraw.ini` `renderer` | рестарт |
| VSync | лечит разрывы в эксклюзивном фулскрине ценой небольшой задержки вывода; в окне и безрамочном режиме разрывов нет и так (композиция DWM), там держите выключенным | `ddraw.ini` `vsync` | сразу |
| Сделать скриншот (PrintScreen) | скриншот средствами рендерера | - | - |

Широкое разрешение игры и «Широкий экран боя» независимы. Первое расширяет стратегический обзор и
требует полного перезапуска; второе выбирает фиксированный боевой диалог шириной 990 со следующего
боя. Смена только размера окна или масштабирования не включает ни одну из них. Длинное подменю
пресетов вывода намеренно убрано: диалог «Окно/вывод» внизу единого меню «Разрешение» задаёт
автоматический или произвольный сохраняемый размер, а безрамочный режим следует рабочему столу.

### Производительность

| Пункт | Что делает | Куда пишет | Применение |
| --- | --- | --- | --- |
| Кап FPS (рестарт): Частота монитора / 30 / 60 / 144 | только FPS рендера, логику игры не замедляет | `ddraw.ini` `maxfps` | рестарт |
| Кап скорости игры (рестарт): Без капа / 30 / 60 / 100 (по умолчанию) | кап игрового цикла, см. раздел выше | `ddraw.ini` `maxgameticks` | рестарт |
| 1 CPU: стабильность (рестарт) | помогает от случайных вылетов/зависаний на картах; включено по умолчанию. На Windows 10/Wine весь процесс прижимается к логическому CPU 0; Windows 11 24H2+ прижимает только игровые потоки, не затрагивая внешние аудиопотоки | `ddraw.ini` `singlecpu` | полный рестарт |

### Плагины

Загружаются только нативные плагины `Mods\*.c4p`; меню каждого сразу появляется в разделе
**Плагины**. Комплектный Timer настраивается через `C4plugins.ini`, длина отсчёта —
`TableDuration_0`. Для перемещения часов зажмите **Ctrl+Alt** и перетащите их ЛКМ. На точной
раскладке Russobit/MNS режим Force начинает отсчёт только после штатной кнопки **OK** в сводке
начала хода: пока диалог открыт, время не уходит, а уже запущенный Force-таймер вручную не ставится
на паузу. На других exe остаётся прежний сигнал активного хода — непроверенные адреса не применяются.
При открытом штатном меню слой плагинов остаётся под ним, а диалоги того же процесса — выше:
таймер больше не перекрывает пункты меню.

Стандартный меч над декоративной рамкой использует тот же текущий масштаб viewport по X/Y, что и
рисуемый игрой курсор. Поэтому на границе игрового экрана и расширенной рамки сохраняются одинаковые
размер и пропорции, в том числе при намеренно растянутом выводе.

## Работа с сейвами (все версии игры)

`features/savelogic.cpp` переносит файловую логику старого врапера без единого адреса
`Discipl2.exe`. Перехватываются WinAPI-функции открытия, закрытия и перечисления файлов, поэтому
одна реализация работает с exe Акеллы, MNS/SMNS, GOG и редактора:

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
На MNS/SMNS, Akella, GOG и неизвестных игровых exe это вынесено в
**Игра > Локализация текста игры**: список собирается из локалей, установленных в Windows, выбранный
LCID пишется в `Disciple.ini`.

## Режим стабильности 1 CPU

`singlecpu=true` включён по умолчанию и в комплектном, и в автосозданном конфиге. Если игра
случайно вылетает или зависает на любых, не связанных между собой картах, включите
**Производительность > 1 CPU: стабильность (рестарт)** и полностью перезапустите игру. Меню
только сохраняет настройку следующего запуска в `ddraw.ini` и намеренно не создаёт смешанное
affinity-состояние в текущем процессе. При запуске Windows 10 и Wine используют процессную
политику cnc-ddraw: весь процесс работает на логическом CPU 0. На нативной Windows 11 24H2+
процесс остаётся свободным, текущие и будущие игровые потоки прижимаются к CPU 0, а внешние
аудио- и служебные потоки намеренно не затрагиваются. Отключайте режим только для диагностики
конкретной проблемы со звуком или производительностью, после чего снова перезапускайте игру.

## Экспериментальное

- Шаги 15x у burst/скоростей: тестовое преувеличение.
- `C4menu.ini` `perUnitBurst`: burst только для действующих юнитов; пункта меню нет, по умолчанию
  выключено, не доделано.
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
