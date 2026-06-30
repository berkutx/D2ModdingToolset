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
| `upstream/cnc-ddraw/` | The cnc-ddraw renderer as a **git submodule**, pinned at upstream `a0b81b11` (v7.1.0.1). Never edited in place. | submodule pointer |
| `patches/cnc-ddraw-mss32.patch` | Our diff over upstream: DirectDraw embed + `DDReloadConfig`/`DDTakeScreenshot` exports + the `featuremenu_install()` call (`dllmain.c`, `winapi_hooks.c`, `exports.def`) | yes |
| `patches/cnc-ddraw-render-null.patch` | The `render_null` headless backend: `dd.c` renderer branch + vcxproj entries + new `inc/render_null.h`, `src/render_null.c` | yes |
| `features/featuremenu.cpp` | The in-game menu, self-contained (no mss32 deps) | yes |
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
4. `featuremenu_install()` then adds the in-game menu: it detours the game window procedure by
   address to receive `WM_COMMAND` and attaches a real menu bar (Game / Video / Performance / Plugins)
   under cnc-ddraw's title bar. Renderer settings are written to `ddraw.ini` and re-applied live via the
   renderer's own `DDReloadConfig`; screenshots use `DDTakeScreenshot`.

## Build

```powershell
./build.ps1                 # build only  -> build/cnc-ddraw/bin/Release/C4dll-R.dll
./build.ps1 -Deploy         # build, back up the game's baseline once, then swap in the monolith
./build.ps1 -Restore        # put the baseline C4dll-R.dll + standalone ddraw.dll back
```

`build.ps1` copies the pinned `upstream/cnc-ddraw` submodule to `build/`, applies both patches
(`cnc-ddraw-mss32` + `cnc-ddraw-render-null`), copies in `features/featuremenu.cpp`,
generates `C4dll-R.def` (the CB63 forwards plus the two exports), retargets the vcxproj
(`TargetName` + `.def` + the extra source), and runs MSBuild (Release, Win32, v143, static CRT).
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
with `INSTALL.txt` and a sample `C4plugins.ini` into `C4dll-R-v1.0.zip`, and publishes a GitHub
Release with that zip plus the loose `C4dll-R.dll` and `timer.c4p` attached. Running the workflow
manually (workflow_dispatch) publishes a **prerelease** tagged `c4dll-r-dev-<sha>` for testing. The
package sources live in `c4ddraw/release/` (`INSTALL.txt`, `C4plugins.ini`, `RELEASE_NOTES.md`).

## Deploy by hand

Put `C4dll-R.dll` next to `Discipl2.exe` (replacing the CodeBase copy), keep `CB63.dll` and
`ddraw.ini` there, and remove any standalone `ddraw.dll`. To A/B test our-vs-stock, swap
`C4dll-R.dll` only.

## Updating cnc-ddraw

`upstream/cnc-ddraw/` is a git submodule pinned at an exact commit. To move to a newer upstream:
bump the submodule (`git -C upstream/cnc-ddraw fetch && git -C upstream/cnc-ddraw checkout <sha>`, then
`git add upstream/cnc-ddraw`), re-apply both patches against the new tree (`git apply`), resolve any
reject, and regenerate. `build.ps1` always builds from the pinned submodule + the patches, so the
upstream tree is never edited in place. Both patches touch disjoint files, so order does not matter.

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
| `upstream/cnc-ddraw/` | Рендерер cnc-ddraw как **git submodule**, запинен на апстрим `a0b81b11` (v7.1.0.1). На месте не редактируется. | указатель submodule |
| `patches/cnc-ddraw-mss32.patch` | Наш дифф: встраивание DirectDraw + экспорты `DDReloadConfig`/`DDTakeScreenshot` + вызов `featuremenu_install()` (`dllmain.c`, `winapi_hooks.c`, `exports.def`) | да |
| `patches/cnc-ddraw-render-null.patch` | Headless-бэкенд `render_null`: ветка рендерера в `dd.c` + записи vcxproj + новые `inc/render_null.h`, `src/render_null.c` | да |
| `features/featuremenu.cpp` | Внутриигровое меню, самодостаточное (без зависимостей mss32) | да |
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
4. Далее `featuremenu_install()` добавляет меню: детурит оконную процедуру игры по адресу, чтобы
   получать `WM_COMMAND`, и крепит настоящий меню-бар (Game / Video / Performance / Plugins) под
   заголовком cnc-ddraw. Настройки рендерера пишутся в `ddraw.ini` и применяются вживую через собственный
   `DDReloadConfig`; скриншоты делает `DDTakeScreenshot`.

## Сборка

```powershell
./build.ps1                 # только сборка -> build/cnc-ddraw/bin/Release/C4dll-R.dll
./build.ps1 -Deploy         # сборка, разовый бэкап baseline игры, затем подмена на монолит
./build.ps1 -Restore        # вернуть baseline C4dll-R.dll + отдельный ddraw.dll
```

`build.ps1` копирует запиненный submodule `upstream/cnc-ddraw` в `build/`, накладывает оба патча
(`cnc-ddraw-mss32` + `cnc-ddraw-render-null`), копирует `features/featuremenu.cpp`,
генерирует `C4dll-R.def` (форварды CB63 плюс два экспорта), перенацеливает vcxproj (`TargetName` +
`.def` + доп. исходник) и запускает MSBuild (Release, Win32, v143, статический CRT). MSBuild ищется
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
`INSTALL.txt` и примером `C4plugins.ini` в `C4dll-R-v1.0.zip` и опубликует GitHub Release с этим
архивом плюс отдельными файлами `C4dll-R.dll` и `timer.c4p`. Ручной запуск workflow
(workflow_dispatch) публикует **пре-релиз** с тегом `c4dll-r-dev-<sha>` для теста. Исходники пакета —
в `c4ddraw/release/` (`INSTALL.txt`, `C4plugins.ini`, `RELEASE_NOTES.md`).

## Ручная установка

Положите `C4dll-R.dll` рядом с `Discipl2.exe` (заменив копию CodeBase), оставьте `CB63.dll` и
`ddraw.ini`, удалите любой отдельный `ddraw.dll`. Для сравнения наш/сток меняйте только `C4dll-R.dll`.

## Обновление cnc-ddraw

`upstream/cnc-ddraw/` это git submodule, запиненный на точный коммит. Чтобы перейти на новый апстрим:
сдвиньте submodule (`git -C upstream/cnc-ddraw fetch && git -C upstream/cnc-ddraw checkout <sha>`, затем
`git add upstream/cnc-ddraw`), наложите оба патча на новое дерево (`git apply`), разрешите конфликты и
пересоберите. `build.ps1` всегда собирает из запиненного submodule + патчи, поэтому дерево апстрима не
редактируется на месте. Оба патча трогают непересекающиеся файлы, поэтому порядок не важен.
