# Disciples 2 test harness

Drive the game from a script: launch one or two clients, read the live UI, click buttons, fill
forms, and assert — all over a local relay, no screenshots and no log scraping. A test is a
PowerShell file in this folder.

> English first; **Русская версия ниже** (`## RU`).

## The worked example — read this first

[`scenario-generation.ps1`](scenario-generation.ps1) drives the random-scenario generator's form.
This is the whole shape of a test:

```powershell
. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir       # from -GameDir, else test.config.psd1
$relay   = Start-TestRelay                # node relay.js
$client  = Start-GameClient -GameDir $GameDir -Role host

Wait-Dialog host DLG_MAIN_MENU 90                                   # wait for a dialog
Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL            # click until the next dialog
Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2               # pick TCP/IP
Step-ToDialog host DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI
# ... on to the generator (DLG_RANDOM_SCENARIO_MULTI) ...

$names = (Get-GameUi host).widgets.name                            # read every widget on the dialog
Set-ListSelection host $D TLBOX_TEMPLATES 3                        # a specific template
Set-EditText      host $D EDIT_NAME "AutoTest"                     # the player name
Set-SpinOption    host $D SPIN_SIZE 1                              # a spinner
Invoke-Button     host $D BTN_GENERATE
```

Verification is relay-only: the generator opened, each `Step-ToDialog` required a real click, the
expected widgets are present (`Get-GameUi`), and the client stayed alive on the dialog.

## How it works — three layers

```
   DISPATCHER  (your .ps1)            RELAY  (node)              GAME CLIENT  (mss32 DebugTest DLL)
   the brain                          dumb mirror + relay        thin: no test logic
   ───────────────────────           ─────────────────          ──────────────────────────────────
   Get-Dialog / Get-GameUi  ─GET /api/ui──▶  ┌─────────┐ ◀─UiSnapshot (JSON)──  uistatereporter
                                             │  state  │                        enumerates the current
   Invoke-Button            ─POST──────────▶ │   per   │ ─InvokeButton (opcode)▶ dialog's widgets every
   Set-ListSelection         /api/ui/invoke  │  role   │                         frame; runs the click on
   Set-SpinOption / -EditText  select/...    └─────────┘                         the game's UI thread
```

- **Game client** — the `mss32.dll` **DebugTest** build, loaded by `Discipl2.exe`. Its
  `uistatereporter` ([../../mss32/src/testdrv/uistatereporter.cpp](../../mss32/src/testdrv/uistatereporter.cpp))
  hooks the dialog button-bind, tracks the current dialog, and each frame enumerates **all** of its
  controls into a JSON snapshot. The `autonav` executor runs invoke/select/spin/edit on the UI
  thread. No test logic; gated by `D2TESTDRV_*` env vars. (`mss32/src/testdrv/`)
- **Relay** — a dependency-free Node server ([../relay/relay.js](../relay/relay.js)). It mirrors each
  client's latest snapshot (keyed by role) and forwards the dispatcher's commands. No test logic.
- **Dispatcher** — your test (PowerShell, this folder). The brain: it reads each client's UI, drives
  it, verifies, and coordinates two clients — over the relay, no files.

The snapshot the reporter ships and the relay serves at `GET /api/ui` is:

```json
{ "role": "host", "dialog": "DLG_PROTOCOL", "widgets": [
  { "name": "BTN_CONTINUE",   "type": "button",  "state": { "enabled": true } },
  { "name": "TLBOX_PROTOCOL", "type": "listbox", "state": { "selected": 2, "total": 3 } },
  { "name": "EDIT_NAME",      "type": "edit",    "state": { "text": "AutoTest" } } ] }
```

`type` is `button` / `listbox` / `spin` / `edit` / `text` / `picture` / `toggle` / `radio`. `state`
carries `enabled` (button), `selected` + `total` (listbox), `index` + `text` (spin), or `text`
(edit/text). A message box is an ordinary dialog (`DLG_MESSAGE_BOX`) — its body is the `text` of a
text widget, so it shows up in the snapshot like anything else.

## Setup

1. **Config.** Copy [`test.config.sample.psd1`](test.config.sample.psd1) to `test.config.psd1`
   (gitignored) and set `GameDir` to your Disciples 2 install. Scripts read it when you don't pass
   `-GameDir`; if the path is wrong they say exactly what to fix.
2. **DLL.** Build the **DebugTest** `mss32.dll` and deploy it over your `GameDir` (the folder must
   have `Discipl2.exe` and the renamed `Mss23.dll`).
3. **Node.js** on `PATH` (the dispatcher starts `relay.js`).

Then run a test:

```powershell
.\scenario-generation.ps1            # single instance
.\multiplayer-two-instance.ps1 -Kill # host + joiner reach the strategic map
```

## The commands

From [`_relay.ps1`](_relay.ps1) — dot-source it. `<role>` is `host` / `join` / etc. (matches the
client's `D2TESTDRV_ROLE`).

| Command | Does | Endpoint |
|---|---|---|
| `Resolve-GameDir [$GameDir]` | the game folder, from `-GameDir` or the config (validated) | — |
| `Start-TestRelay` | start `relay.js`, return its process | — |
| `Start-GameClient -GameDir <d> -Role <r>` | launch a DebugTest client | — |
| `Get-Dialog <role>` | the current dialog name | `GET /api/state` |
| `Get-GameUi <role>` | `{role, dialog, widgets[]}` — every widget + state | `GET /api/ui` |
| `Get-RoleState <role>` | one role's `{dialog, widgets, connected, …}` | `GET /api/state` |
| `Wait-Dialog <role> <dlg> [sec]` | wait until the client is on `<dlg>` | — |
| `Invoke-Button <role> <dlg> <btn>` | click a button (run its functor) | `POST /api/ui/invoke` |
| `Set-ListSelection <role> <dlg> <listbox> <i>` | set a listbox selection | `POST /api/ui/select` |
| `Set-SpinOption <role> <dlg> <spin> <i>` | set a spin-button option | `POST /api/ui/spin` |
| `Set-EditText <role> <dlg> <edit> <text>` | set an edit-box's text | `POST /api/ui/edit` |
| `Step-ToDialog <role> <dlg> <btn> <toDlg> [sec]` | click `<btn>` until the client reaches `<toDlg>` (re-fires) | — |

The client resolves a widget **by dialog name**, so a click on a co-present (non-current) or
just-closed dialog is safe.

For interactive poking, dot-source [`../relay/drive-game-relay.ps1`](../relay/drive-game-relay.ps1):
the same commands plus `Get-GameStatus` / `Get-GameLog` / `Get-GameChat` / `Get-GameEvents`.

## Finding dialog and widget names

Launch one client (`UI_REPORTER` + `RELAY_BRIDGE`, the `Start-GameClient` default), then read the
live UI as you navigate — every control is in the snapshot, with its name, type and state:

```powershell
$relay = Start-TestRelay; $c = Start-GameClient -GameDir (Resolve-GameDir) -Role host
Get-GameUi host | % widgets | Format-Table name, type, @{n='state';e={$_.state | ConvertTo-Json -Compress}}
Invoke-Button host DLG_MAIN_MENU BTN_MULTI
Get-Dialog host      # -> DLG_PROTOCOL, then read its widgets again
```

No need to grep the game's `.dlg` files — list boxes, spin buttons and edit boxes are all in the
snapshot, unlike the old buttons-only view.

## Verifying — over the relay, never logs or files

- **Reaching a dialog proves the clicks ran.** Assert with `Get-Dialog` / `Wait-Dialog`; a
  successful `Step-ToDialog` already means the client executed each click.
- **Read widget state with `Get-GameUi`** — assert a button is enabled, a listbox landed on the
  right index, a spinner shows the right option, an edit holds the right text.
- **Flickering dialogs**: a poll can miss a dialog that only flashes by. `relay.js` latches a sticky
  per-role flag on first sight (e.g. `sawBeginTurn` flips `true` on the first `DLG_BEGIN_TURN`); read
  `(Get-RoleState <role>).sawBeginTurn`. Add a latch for any dialog you must not miss.
- **A close is reported.** The reporter polls the engine's real topmost interface each frame, so when
  a modal closes and reveals the dialog underneath, the snapshot switches to it on its own — no stale
  value to work around. (Co-present `DLG_ISO_PAL` + `DLG_STRATEGIC` share a screen, so either of that
  pair may be reported; both resolve by name regardless.)
- Logs are for humans. Don't scrape them for state or ordering.

### First-turn popups

When a turn begins, popups stack for **both** players — scenario briefing, the new-day income
`DLG_BEGIN_TURN`, the "name your lord" `DLG_GETINFO_BOX`, message boxes — and must be clicked
through. Map each to the button that closes it, pace the clicks (~2 s), and log each:

```powershell
$Dismiss = @{ 'DLG_SCENARIO_BRIEFING'='BTN_CONTINUE'; 'DLG_BEGIN_TURN'='BTN_OK'
              'DLG_GETINFO_BOX'='BTN_CLOSE'; 'DLG_MESSAGE_BOX'='BTN_OK' }
$d = Get-Dialog $role
if ($Dismiss.ContainsKey($d)) { Invoke-Button $role $d $Dismiss[$d] }
```

`DLG_GETINFO_BOX` closes on a single `BTN_CLOSE` — it already holds the lord's default name, so do
**not** `Set-EditText` it (that corrupts the accept). The reporter shows the map underneath as soon
as it closes, so the next action just works. See `DriveToStrategic` in
[`multiplayer-two-instance.ps1`](multiplayer-two-instance.ps1).

## Adding a test

1. Write `tools/test/<name>.ps1`. Start from the [worked example](#the-worked-example--read-this-first):
   `param([string]$GameDir, [switch]$Kill)`, dot-source `_relay.ps1`, `Resolve-GameDir`,
   `Start-TestRelay`, `Start-GameClient`, drive, assert, and clean up the relay + clients in a
   `finally` (an orphaned relay's pipe blocks the next run). `exit 1` on failure.
2. A two-instance / coordinated test runs **entirely over the relay** (request → wait → response →
   action). A minimal single-instance test can use the built-in self-nav instead (`-Flags …,SELFNAV`,
   see [`walk-menu.ps1`](walk-menu.ps1)).

## Adding it to CI

CI lives in [`../../.github/workflows`](../../.github/workflows). The reusable suite is
**`tests.yml`**; two entrypoints call it on disjoint path filters:

- **`mss32.yml`** (on `mss32/**`) builds Debug + Release + **DebugTest**, then runs `tests.yml`
  against the DLL it just built.
- **`tests-only.yml`** (on `tools/**`) skips the build and **reuses** the last DebugTest DLL — a
  script-only change runs the tests in minutes, no recompile.

`tests.yml` jobs: `prep-game` (caches the ~2 GB minimal game once per run), `headless-boot-smoke`
(boot 1×), `headless-boot-reliability` (boot 5×), and `multiplayer-two-instance-strategic` (the
two-client MP test). Each test job: download the `mss32-debugtest` artifact → restore the game cache
→ deploy `mss32.dll` → run the `.ps1`.

To add your test, copy one of those jobs and change the last step:

```yaml
  my-test:
    name: My test
    needs: prep-game
    runs-on: windows-2025-vs2026
    defaults: { run: { shell: pwsh } }
    steps:
      - uses: actions/checkout@v6
        with: { submodules: false, fetch-depth: 1 }
      - uses: actions/download-artifact@v8
        with: { name: mss32-debugtest, path: dll, run-id: '${{ inputs.dll_run_id || github.run_id }}', github-token: '${{ github.token }}' }
      - uses: actions/cache/restore@v5
        with: { path: game.tar.gz, key: slasher-min-v1, enableCrossOsArchive: true, fail-on-cache-miss: true }
      - name: Unpack game + deploy DLL
        run: |
          New-Item -ItemType Directory -Force -Path game | Out-Null
          tar -xzf game.tar.gz -C game
          $dir = (Get-ChildItem game -Directory | Select-Object -First 1).FullName
          Copy-Item dll\mss32.dll "$dir\mss32.dll" -Force
          "GAME_DIR=$dir" | Out-File -FilePath $env:GITHUB_ENV -Append
      - name: Run my test
        run: ./tools/test/my-test.ps1 -GameDir "$env:GAME_DIR" -Kill
```

Pass `-GameDir "$env:GAME_DIR"` (CI has no config file) and `-Kill` so the run cleans up.

## The files

| File | Role |
|---|---|
| `_relay.ps1` | the toolkit: config, relay, clients, the commands above |
| `test.config.sample.psd1` | per-machine config template (copy to `test.config.psd1`) |
| `scenario-generation.ps1` | single-instance form-driving example |
| `multiplayer-two-instance.ps1` | two clients into a started MP game (host + joiner) |
| `reliability_test.ps1` | boot N× to the main menu (the CI boot test) |
| `walk-menu.ps1` | one self-nav client, leave it running to poke at |
| `_show-window.ps1`, `_capture.ps1` | bring a kept window forward / grab a PNG (diagnostics only) |
| `../relay/relay.js` | the node relay |
| `../relay/drive-game-relay.ps1` | interactive console over the relay |

---

## RU

Управляй игрой из скрипта: запусти один-два клиента, читай живой UI, жми кнопки, заполняй формы и
проверяй — всё через локальный рилей, без скриншотов и без скрёбки логов. Тест — это PowerShell-файл
в этой папке.

### Рабочий пример — читать первым

[`scenario-generation.ps1`](scenario-generation.ps1) рулит формой генератора случайных карт. Это вся
форма теста:

```powershell
. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir       # из -GameDir, иначе из test.config.psd1
$relay   = Start-TestRelay                # node relay.js
$client  = Start-GameClient -GameDir $GameDir -Role host

Wait-Dialog host DLG_MAIN_MENU 90                                   # ждать диалог
Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL            # жать, пока не следующий диалог
Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2               # выбрать TCP/IP
$names = (Get-GameUi host).widgets.name                            # прочитать все виджеты диалога
Set-EditText host $D EDIT_NAME "AutoTest"
Invoke-Button host $D BTN_GENERATE
```

Проверка только через рилей: генератор открылся, каждый `Step-ToDialog` потребовал реального клика,
нужные виджеты на месте (`Get-GameUi`), клиент жив на диалоге.

### Как устроено — три слоя

```
   ДИСПЕТЧЕР  (твой .ps1)             РИЛЕЙ  (node)              КЛИЕНТ ИГРЫ  (mss32 DebugTest DLL)
   мозг                              тупое зеркало + ретранслятор  тонкий: без тест-логики
   ───────────────────────          ─────────────────          ──────────────────────────────────
   Get-Dialog / Get-GameUi  ─GET /api/ui──▶  ┌─────────┐ ◀─UiSnapshot (JSON)──  uistatereporter
                                             │ состояние│                        каждый кадр перечисляет
   Invoke-Button            ─POST──────────▶ │  по роли │ ─InvokeButton (опкод)▶ все виджеты текущего
   Set-ListSelection         /api/ui/invoke  └─────────┘                         диалога; клик — на
   Set-SpinOption / -EditText  select/...                                        UI-потоке игры
```

- **Клиент игры** — сборка `mss32.dll` **DebugTest**, которую грузит `Discipl2.exe`. Его
  `uistatereporter` хукает бинд кнопок диалога, отслеживает текущий диалог и каждый кадр перечисляет
  **все** его контролы в JSON-снапшот. Исполнитель `autonav` выполняет invoke/select/spin/edit на
  UI-потоке. Тест-логики нет; гейтится `D2TESTDRV_*`. (`mss32/src/testdrv/`)
- **Рилей** — Node-сервер без зависимостей ([../relay/relay.js](../relay/relay.js)). Зеркалит
  последний снапшот каждого клиента (по роли) и пробрасывает команды. Тест-логики нет.
- **Диспетчер** — твой тест (PowerShell, эта папка). Мозг: читает UI, рулит, проверяет, координирует
  два клиента — через рилей, без файлов.

Снапшот, который отдаёт `GET /api/ui`:

```json
{ "role": "host", "dialog": "DLG_PROTOCOL", "widgets": [
  { "name": "TLBOX_PROTOCOL", "type": "listbox", "state": { "selected": 2, "total": 3 } },
  { "name": "EDIT_NAME",      "type": "edit",    "state": { "text": "AutoTest" } } ] }
```

`type` — `button` / `listbox` / `spin` / `edit` / `text` / `picture` / `toggle` / `radio`. `state` —
`enabled` (button), `selected`+`total` (listbox), `index`+`text` (spin), `text` (edit/text).
Месседж-бокс — обычный диалог (`DLG_MESSAGE_BOX`), его текст — это `text` текстового виджета, так что
он виден в снапшоте как всё остальное.

### Настройка

1. **Конфиг.** Скопируй [`test.config.sample.psd1`](test.config.sample.psd1) в `test.config.psd1`
   (в gitignore) и впиши `GameDir` — путь к установке Disciples 2. Скрипты читают его, когда не
   передан `-GameDir`; при неверном пути скажут, что поправить.
2. **DLL.** Собери **DebugTest** `mss32.dll` и положи её в `GameDir` (там должны быть `Discipl2.exe`
   и переименованная `Mss23.dll`).
3. **Node.js** в `PATH` (диспетчер запускает `relay.js`).

Запуск:

```powershell
.\scenario-generation.ps1
.\multiplayer-two-instance.ps1 -Kill
```

### Команды

Из [`_relay.ps1`](_relay.ps1) — подключи через `.`. `<role>` — `host` / `join` и т.п. (совпадает с
`D2TESTDRV_ROLE` клиента).

| Команда | Что | Endpoint |
|---|---|---|
| `Resolve-GameDir [$GameDir]` | папка игры, из `-GameDir` или конфига (с проверкой) | — |
| `Start-TestRelay` | запустить `relay.js` | — |
| `Start-GameClient -GameDir <d> -Role <r>` | запустить DebugTest-клиента | — |
| `Get-Dialog <role>` | имя текущего диалога | `GET /api/state` |
| `Get-GameUi <role>` | `{role, dialog, widgets[]}` — все виджеты + состояние | `GET /api/ui` |
| `Get-RoleState <role>` | состояние роли `{dialog, widgets, connected, …}` | `GET /api/state` |
| `Wait-Dialog <role> <dlg> [sec]` | ждать диалог | — |
| `Invoke-Button <role> <dlg> <btn>` | нажать кнопку | `POST /api/ui/invoke` |
| `Set-ListSelection <role> <dlg> <listbox> <i>` | выбор в листбоксе | `POST /api/ui/select` |
| `Set-SpinOption <role> <dlg> <spin> <i>` | опция спин-кнопки | `POST /api/ui/spin` |
| `Set-EditText <role> <dlg> <edit> <text>` | текст поля ввода | `POST /api/ui/edit` |
| `Step-ToDialog <role> <dlg> <btn> <toDlg> [sec]` | жать `<btn>`, пока не дойдёт до `<toDlg>` | — |

Клиент резолвит виджет **по имени диалога**, поэтому клик по co-present/только что закрытому диалогу
безопасен.

Для ручного ковыряния — [`../relay/drive-game-relay.ps1`](../relay/drive-game-relay.ps1): те же
команды плюс `Get-GameStatus` / `Get-GameLog` / `Get-GameChat` / `Get-GameEvents`.

### Как узнать имена диалогов и виджетов

Запусти один клиент (`UI_REPORTER` + `RELAY_BRIDGE` — дефолт `Start-GameClient`) и читай живой UI по
ходу навигации — каждый контрол в снапшоте, с именем, типом и состоянием:

```powershell
$relay = Start-TestRelay; $c = Start-GameClient -GameDir (Resolve-GameDir) -Role host
Get-GameUi host | % widgets | Format-Table name, type
Invoke-Button host DLG_MAIN_MENU BTN_MULTI; Get-Dialog host
```

Grep `.dlg`-файлов больше не нужен — листбоксы, спины и поля ввода теперь все в снапшоте (в отличие
от старого вида «только кнопки»).

### Проверка — через рилей, не через логи/файлы

- **Дойти до диалога = клики прошли.** Проверяй `Get-Dialog` / `Wait-Dialog`; успешный
  `Step-ToDialog` уже значит, что клиент исполнил клики.
- **Состояние виджетов через `Get-GameUi`** — что кнопка активна, листбокс на нужном индексе, спин
  показывает нужную опцию, в поле нужный текст.
- **Мелькающие диалоги**: опрос может пропустить вспыхнувший диалог. `relay.js` ставит липкий флаг на
  роль при первом появлении (например, `sawBeginTurn` на первом `DLG_BEGIN_TURN`); читай
  `(Get-RoleState <role>).sawBeginTurn`. Любой важный диалог защёлкивай так же.
- **Закрытие репортится.** Репортер каждый кадр опрашивает реальный верхний интерфейс движка, так что
  при закрытии модалки снапшот сам переключается на диалог под ней — устаревшего значения нет.
  (Co-present `DLG_ISO_PAL` + `DLG_STRATEGIC` делят экран — может прийти любой; оба резолвятся по имени.)
- Логи — для человека. Не скрёб их ради состояния/очерёдности.

#### Диалоги начала хода

В начале хода у **обоих** игроков всплывают диалоги — брифинг, доход нового дня `DLG_BEGIN_TURN`,
имя лорда `DLG_GETINFO_BOX`, месседж-боксы — их надо прокликать. Сопоставь каждому закрывающую кнопку,
пейси клики (~2 с) и логируй:

```powershell
$Dismiss = @{ 'DLG_SCENARIO_BRIEFING'='BTN_CONTINUE'; 'DLG_BEGIN_TURN'='BTN_OK'
              'DLG_GETINFO_BOX'='BTN_CLOSE'; 'DLG_MESSAGE_BOX'='BTN_OK' }
$d = Get-Dialog $role
if ($Dismiss.ContainsKey($d)) { Invoke-Button $role $d $Dismiss[$d] }
```

`DLG_GETINFO_BOX` закрывается одним `BTN_CLOSE` — в нём уже дефолтное имя лорда, поэтому **не** делай
`Set-EditText` (ломает accept). Репортер сразу показывает карту под ним. Пример — `DriveToStrategic`
в [`multiplayer-two-instance.ps1`](multiplayer-two-instance.ps1).

### Добавить тест

1. Создай `tools/test/<имя>.ps1`. Бери [рабочий пример](#рабочий-пример--читать-первым):
   `param([string]$GameDir, [switch]$Kill)`, подключи `_relay.ps1`, `Resolve-GameDir`,
   `Start-TestRelay`, `Start-GameClient`, рули, проверяй, и чисти рилей + клиентов в `finally`
   (осиротевший рилей блокирует пайп для следующего запуска). `exit 1` при провале.
2. Двух-инстансный / координированный тест — **только через рилей** (запрос → ожидание → ответ →
   действие). Минимальный одиночный может использовать встроенный self-nav (`-Flags …,SELFNAV`, см.
   [`walk-menu.ps1`](walk-menu.ps1)).

### Подключить к CI

CI — в [`../../.github/workflows`](../../.github/workflows). Переиспользуемый набор — **`tests.yml`**;
его зовут два входа по непересекающимся путям:

- **`mss32.yml`** (на `mss32/**`) собирает Debug + Release + **DebugTest**, затем гоняет `tests.yml`
  по только что собранной DLL.
- **`tests-only.yml`** (на `tools/**`) пропускает сборку и **переиспользует** прошлую DebugTest DLL —
  правка только скриптов проходит тесты за минуты, без перекомпиляции.

Джобы `tests.yml`: `prep-game` (кэширует ~2 ГБ минимальной игры), `headless-boot-smoke` (бут 1×),
`headless-boot-reliability` (бут 5×), `multiplayer-two-instance-strategic` (двух-клиентный MP). Каждый
тест-джоб: скачать артефакт `mss32-debugtest` → восстановить игру из кэша → положить `mss32.dll` →
запустить `.ps1`.

Чтобы добавить свой — скопируй джоб и поменяй последний шаг на свой `.ps1` с
`-GameDir "$env:GAME_DIR" -Kill` (в CI конфига нет, `-Kill` чистит за собой). Шаблон — в английской
части выше.

### Файлы

| Файл | Роль |
|---|---|
| `_relay.ps1` | тулкит: конфиг, рилей, клиенты, команды выше |
| `test.config.sample.psd1` | шаблон конфига машины (копируй в `test.config.psd1`) |
| `scenario-generation.ps1` | пример драйва формы (один инстанс) |
| `multiplayer-two-instance.ps1` | два клиента в начатую MP-игру (host + joiner) |
| `reliability_test.ps1` | бут N× до главного меню (бут-тест CI) |
| `walk-menu.ps1` | один self-nav клиент, оставить запущенным |
| `_show-window.ps1`, `_capture.ps1` | поднять окно / снять PNG (только диагностика) |
| `../relay/relay.js` | node-рилей |
| `../relay/drive-game-relay.ps1` | интерактивная консоль над рилеем |
