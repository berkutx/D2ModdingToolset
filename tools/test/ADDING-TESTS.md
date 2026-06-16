# Adding a test

> English first; **Русская версия ниже** (`## RU`).

## How it works — three layers

| Layer | What | Where |
|---|---|---|
| **Agent** | The mss32 **DebugTest** DLL. Thin: reports the live dialog + its buttons, and executes invoke/select/spin/edit commands on the game's UI thread. No test logic. Gated by `D2TESTDRV_*` env vars. | `mss32/src/testdrv/` |
| **Relay** | A dependency-free Node server. A dumb live **mirror + command relay** — no test logic. | `tools/relay/relay.js` |
| **Dispatcher** | **Your test** (PowerShell). The brain: it scans each agent's UI (`/api/state`), drives it (`/api/invoke\|select\|spin\|edit`), verifies and coordinates. | `tools/test/*.ps1` |

A test is just a PowerShell script in **`tools/test/`** (not the repo root). Dot-source `_relay.ps1` for the toolkit.

## Quick start

```powershell
. "$PSScriptRoot\_relay.ps1"
$relay = Start-TestRelay
$h = Launch-Agent -Game $Game -Role "host"     # boots with UI_REPORTER + RELAY_BRIDGE
WaitDlg "host" "DLG_MAIN_MENU" 90
StepTo  "host" "DLG_MAIN_MENU" "BTN_MULTI" "DLG_PROTOCOL" 45
# ... drive, then assert with Dlg/WaitDlg ...
Stop-Process -Id $h.Id,$relay.Id -Force
```

## The commands (from `_relay.ps1`)

| Helper | Does | Relay endpoint |
|---|---|---|
| `Dlg <role>` / `State` | read the live dialog / all roles | `GET /api/state` |
| `InvokeBtn <role> <dlg> <btn>` | click a button (run its functor) | `POST /api/invoke` |
| `SetSel <role> <dlg> <listbox> <index>` | set a listbox selection | `POST /api/select` |
| `SetSpin <role> <dlg> <spin> <index>` | set a spin-button option | `POST /api/spin` |
| `SetEdit <role> <dlg> <edit> <text>` | set an edit-box's text | `POST /api/edit` |
| `WaitDlg <role> <dlg> <sec>` | wait until the agent is on `<dlg>` | — |
| `StepTo <role> <src> <btn> <expect> <sec>` | click `<btn>` until the agent reaches `<expect>` (re-fires) | — |

The agent resolves a widget **by dialog name**, so a click on a co-present (non-current) or already-closed dialog is safe.

## Discovering dialog / widget names

- **Buttons + the current dialog name are live in `/api/state`** — the agent reports every button it binds. Launch one instance (`UI_REPORTER` + `RELAY_BRIDGE`, no `SELFNAV`), navigate by hand with `InvokeBtn`, and `GET /api/state` (or `Dlg`/`Buttons`) at each step to read the real names.
- **Listboxes, spin-buttons and edit-boxes are NOT button binds**, so they do *not* appear in `/api/state`. Get their names from the game's `Interf/Interf.dlg` (grep the `DIALOG <NAME>` block) or the mod source.

## Verify over the relay — not logs or files

- Assert the dialog you expect with `Dlg` / `WaitDlg`. Reaching a dialog via `StepTo` already **proves the agent executed the clicks**.
- Form *values* (selection / spin / text) are not exposed in `/api/state`; assert instead that the agent **stayed responsive** (still on the dialog, or a clean follow-up transition). Do **not** grep logs for state or ordering — logs are for human debugging only.

## Add it to CI (optional)

Add a job to `.github/workflows/tests.yml` mirroring an existing one (download the `mss32-debugtest` DLL, restore the game cache, deploy, run your `.ps1`). A change to **only** `tools/**` reuses the last prebuilt DLL via `tests-only.yml` — no rebuild.

## Worked example — `scenario-generation.ps1`

A single instance navigates the **multiplayer** setup to the random-scenario generator
(`DLG_RANDOM_SCENARIO_MULTI`), picks a **specific** template (`SetSel TLBOX_TEMPLATES`), fills
the player name (`SetEdit EDIT_NAME`), toggles spinners (`SetSpin SPIN_SIZE/SPIN_GOAL`) and
clicks `BTN_GENERATE`. Read it as a template for a form-driving test.

## Notes / gotchas

- **Custom menus tick slower than native menus.** The generator is a mod menu whose per-frame
  tick is much slower than the main menus, so give each command a settle (~3 s) instead of
  firing back-to-back; `StepTo` already re-fires on a timer.
- **`CMessageBox` popups are not reported** (the UI reporter only hooks the dialog button-bind),
  so a flow that ends in a message box is not relay-visible.
- Two-instance / coordinated tests must run **entirely over the relay** (request → wait →
  response → action), no files on disk. A minimal single-instance test may rely on the in-DLL
  agent alone.

---

## RU (русская версия)

### Как это устроено — три слоя

| Слой | Что | Где |
|---|---|---|
| **Агент** | DLL mss32 **DebugTest**. Тонкий: репортит текущий диалог + кнопки и исполняет invoke/select/spin/edit на UI-потоке игры. Без тест-логики. Гейтится `D2TESTDRV_*`. | `mss32/src/testdrv/` |
| **Рилей** | Node-сервер без зависимостей. Тупое живое **зеркало + ретранслятор команд**. | `tools/relay/relay.js` |
| **Диспетчер** | **Твой тест** (PowerShell). Мозг: сканит UI (`/api/state`), рулит (`/api/invoke\|select\|spin\|edit`), проверяет, координирует. | `tools/test/*.ps1` |

Тест — это PowerShell-скрипт в **`tools/test/`** (не в корне). Подключи `_relay.ps1` (`. "$PSScriptRoot\_relay.ps1"`) — там тулкит.

### Быстрый старт

```powershell
. "$PSScriptRoot\_relay.ps1"
$relay = Start-TestRelay
$h = Launch-Agent -Game $Game -Role "host"     # бут с UI_REPORTER + RELAY_BRIDGE
WaitDlg "host" "DLG_MAIN_MENU" 90
StepTo  "host" "DLG_MAIN_MENU" "BTN_MULTI" "DLG_PROTOCOL" 45
# ... рулим, потом проверяем через Dlg/WaitDlg ...
Stop-Process -Id $h.Id,$relay.Id -Force
```

### Команды (из `_relay.ps1`)

- `Dlg <role>` / `State` — прочитать текущий диалог / все роли (`GET /api/state`).
- `InvokeBtn <role> <dlg> <btn>` — нажать кнопку (`POST /api/invoke`).
- `SetSel <role> <dlg> <listbox> <index>` — выбор в листбоксе (`POST /api/select`).
- `SetSpin <role> <dlg> <spin> <index>` — опция спин-кнопки (`POST /api/spin`).
- `SetEdit <role> <dlg> <edit> <text>` — текст в поле ввода (`POST /api/edit`).
- `WaitDlg <role> <dlg> <sec>` — ждать диалог.
- `StepTo <role> <src> <btn> <expect> <sec>` — жать `<btn>`, пока агент не дойдёт до `<expect>` (с ре-файром).

Агент резолвит виджет **по имени диалога**, так что клик по co-present/закрытому диалогу безопасен (no-op).

### Как узнать имена диалогов / виджетов

- **Кнопки и имя текущего диалога видны в `/api/state`** — агент репортит каждую забинженную кнопку. Запусти один инстанс (`UI_REPORTER` + `RELAY_BRIDGE`, без `SELFNAV`), навигируй вручную через `InvokeBtn`, и читай `/api/state` на каждом шаге.
- **Листбоксы, спины и поля ввода — НЕ бинды кнопок**, поэтому в `/api/state` их нет. Имена бери из `Interf/Interf.dlg` игры (grep блока `DIALOG <ИМЯ>`) или из исходников мода.

### Проверка через рилей — не через логи/файлы

- Проверяй ожидаемый диалог через `Dlg`/`WaitDlg`. Дойти до диалога через `StepTo` уже **доказывает, что агент исполнил клики**.
- *Значения* формы (выбор/спин/текст) в `/api/state` не отдаются; вместо этого проверяй, что агент **остался жив** (тот же диалог / чистый следующий переход). Логи для состояний/очерёдности **не** скрести — логи только для человека.

### Подключить к CI (опционально)

Добавь джоб в `.github/workflows/tests.yml` по образцу существующего (скачать DLL `mss32-debugtest`, восстановить игру из кэша, задеплоить, запустить `.ps1`). Правка только в `tools/**` переиспользует готовую DLL через `tests-only.yml` — без пересборки.

### Рабочий пример — `scenario-generation.ps1`

Один инстанс навигируется по **мультиплеерному** сетапу к генератору сценариев
(`DLG_RANDOM_SCENARIO_MULTI`), выбирает **конкретный** шаблон (`SetSel TLBOX_TEMPLATES`),
заполняет имя (`SetEdit EDIT_NAME`), переключает спины (`SetSpin SPIN_SIZE/SPIN_GOAL`) и жмёт
`BTN_GENERATE`. Бери как шаблон теста на драйв формы.

### Заметки / грабли

- **Кастомные меню тикают медленнее нативных.** У генератора per-frame тик сильно медленнее
  главных меню, поэтому давай каждой команде осесть (~3 с), а не сыпь подряд; `StepTo` и так
  ре-файрит по таймеру.
- **`CMessageBox` не репортится** (репортер хукает только бинд кнопок диалога), так что поток,
  заканчивающийся месседж-боксом, через рилей не виден.
- Двух-инстансные / координированные тесты — **только через рилей** (запрос → ожидание →
  ответ → действие), без файлов на диске. Минимальный одиночный тест может опираться на агента.
