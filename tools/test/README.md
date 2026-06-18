# Disciples 2 test harness

A UI test harness for Disciples 2. A PowerShell test script launches one or two game instances,
reads the state of their interface, and sends commands to it (button clicks, list selections, spin
and text input) through a local relay process. Tests assert against interface state, not against
screenshots or log output.

English first; the Russian version follows under `## RU`.

## Architecture

The harness has three layers.

| Layer | Responsibility | Location |
|---|---|---|
| Game client | The `mss32.dll` DebugTest build, loaded by `Discipl2.exe`. Its reporter (`uistatereporter`) tracks the current dialog and, every frame, enumerates all of its controls into a JSON snapshot. Its executor (`autonav`) runs each incoming command on the game's UI thread. It contains no test logic. | `mss32/src/testdrv/` |
| Relay | A dependency-free Node.js server. It keeps the latest snapshot of each client, keyed by role, and forwards the dispatcher's commands to the client. It contains no test logic. | `tools/relay/relay.js` |
| Dispatcher | The PowerShell test script. It launches the clients, reads their interface state, sends commands, checks the result, and coordinates the two clients when a test needs both. | `tools/test/*.ps1` |

The relay exists because the dispatcher and the game run in separate processes. The dispatcher
cannot read the game's in-memory interface or call into it directly. The game client reports its
interface to the relay and executes commands the relay delivers; the relay is the shared point both
the dispatcher and every client connect to. It also lets one dispatcher drive two clients (a host
and a joiner) at once, each addressed by its role.

Data flow:

```
dispatcher  ->  relay  ->  game client      commands  (POST /api/ui/invoke, /select, /spin, /edit)
dispatcher  <-  relay  <-  game client      state     (GET  /api/ui)
```

The dispatcher reads `GET /api/ui` to get the snapshot the client last sent (over the `UiSnapshot`
bridge opcode), and posts to `/api/ui/*` to forward a command to the client (over the matching
command opcode). The client runs the command on its UI thread and sends an updated snapshot on the
next frame.

## The UI snapshot

`GET /api/ui?role=<role>` returns the current dialog and every control on it:

```json
{ "role": "host", "dialog": "DLG_PROTOCOL", "widgets": [
  { "name": "BTN_CONTINUE",   "type": "button",  "state": { "enabled": true } },
  { "name": "TLBOX_PROTOCOL", "type": "listbox", "state": { "selected": 2, "total": 3 } },
  { "name": "EDIT_NAME",      "type": "edit",    "state": { "text": "AutoTest" } } ] }
```

| `type` | Control | `state` fields |
|---|---|---|
| `button` | push button | `enabled` (bool) |
| `listbox` | list | `selected`, `total` (int) |
| `spin` | spin button | `index` (int), `text` (current option) |
| `edit` | text input | `text` |
| `text` | static text | `text` |
| `picture`, `toggle`, `radio`, `scrollbar` | other controls | none |

A message box is an ordinary dialog (`DLG_MESSAGE_BOX`); its body is the `text` of a text widget, so
it appears in the snapshot like any other dialog.

## Enabling the harness

The harness is compiled into `mss32.dll` only in the **DebugTest** configuration (the `D2_TESTDRV`
define). The Debug and Release builds are byte-identical to the unmodified DLL.

Within a DebugTest build, each feature is switched on at runtime by a `D2TESTDRV_*` environment
variable. `Start-GameClient` sets these on the launched game process: `D2TESTDRV_UI_REPORTER` (the
snapshot), `D2TESTDRV_RELAY_BRIDGE` (the command bridge), `D2TESTDRV_SKIP_INTRO` and
`D2TESTDRV_BLACKSCREEN_FIX` (boot fixes), and `D2TESTDRV_ROLE` (the relay key). To run a feature
by hand, set the same variables before launching `Discipl2.exe`.

## Setup

1. Configuration. Copy `test.config.sample.psd1` to `test.config.psd1` (which is gitignored) and set
   `GameDir` to the Disciples 2 installation. The scripts read it whenever `-GameDir` is not given on
   the command line, and report the path to correct if it is wrong.
2. DLL. Build the DebugTest `mss32.dll` and place it in `GameDir`, alongside `Discipl2.exe` and the
   renamed `Mss23.dll`.
3. Node.js on `PATH`. The dispatcher starts `relay.js`.

## Running a test

```powershell
.\scenario-generation.ps1             # single instance
.\multiplayer-two-instance.ps1 -Kill  # host and joiner reach the strategic map
```

## Example: scenario-generation.ps1

[`scenario-generation.ps1`](scenario-generation.ps1) drives the random-scenario generator's form
with a single client. It navigates the multiplayer menu to the generator
(`DLG_RANDOM_SCENARIO_MULTI`), reads the snapshot to confirm the form is present, and exercises every
command type:

```powershell
. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir       # from -GameDir, otherwise test.config.psd1
$relay   = Start-TestRelay
$client  = Start-GameClient -GameDir $GameDir -Role host

Wait-Dialog   host DLG_MAIN_MENU 90
Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL
Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2   # TCP/IP
# ... on to DLG_RANDOM_SCENARIO_MULTI ...

$names = (Get-GameUi host).widgets.name
Set-ListSelection host $D TLBOX_TEMPLATES 3
Set-EditText      host $D EDIT_NAME "AutoTest"
Set-SpinOption    host $D SPIN_SIZE 1
Invoke-Button     host $D BTN_GENERATE
```

The test passes when the generator dialog opens, each navigation step produces the expected dialog,
the expected widgets are present, and the client is still on the generator dialog after the form is
driven.

## Commands

From [`_relay.ps1`](_relay.ps1); dot-source it. `<role>` is `host`, `join`, and so on, and matches
the client's `D2TESTDRV_ROLE`.

| Command | Action | Endpoint |
|---|---|---|
| `Resolve-GameDir [$GameDir]` | the game folder, from `-GameDir` or the config, validated | none |
| `Start-TestRelay` | start `relay.js`, return its process | none |
| `Start-GameClient -GameDir <d> -Role <r>` | launch a DebugTest client | none |
| `Get-Dialog <role>` | the current dialog name | `GET /api/state` |
| `Get-GameUi <role>` | `{ role, dialog, widgets }`, every widget with its state | `GET /api/ui` |
| `Get-RoleState <role>` | one role's `{ dialog, widgets, connected, ... }` and latched flags | `GET /api/state` |
| `Wait-Dialog <role> <dlg> [sec]` | wait until the client is on `<dlg>` | none |
| `Invoke-Button <role> <dlg> <btn>` | click a button | `POST /api/ui/invoke` |
| `Set-ListSelection <role> <dlg> <listbox> <i>` | set a list selection | `POST /api/ui/select` |
| `Set-SpinOption <role> <dlg> <spin> <i>` | set a spin-button option | `POST /api/ui/spin` |
| `Set-EditText <role> <dlg> <edit> <text>` | set an edit-box's text | `POST /api/ui/edit` |
| `Step-ToDialog <role> <dlg> <btn> <toDlg> [sec]` | click `<btn>` until the client reaches `<toDlg>` | none |

For interactive use, dot-source [`../relay/drive-game-relay.ps1`](../relay/drive-game-relay.ps1): it
provides the same commands plus the read-only inspectors `Get-GameStatus`, `Get-GameLog`,
`Get-GameChat`, and `Get-GameEvents`.

## Finding dialog and widget names

Launch one client and read the snapshot with `Get-GameUi` as you navigate. It lists every control of
the current dialog with its name, type, and state, including list boxes, spin buttons, and edit boxes:

```powershell
$relay = Start-TestRelay; $c = Start-GameClient -GameDir (Resolve-GameDir) -Role host
Get-GameUi host | ForEach-Object widgets | Format-Table name, type
Invoke-Button host DLG_MAIN_MENU BTN_MULTI
Get-Dialog host    # read the next dialog, then its widgets
```

## Addressing widgets by name

Commands address a widget by its dialog name, not by the topmost dialog. The game keeps several
dialogs open at once (for example `DLG_CHOOSE_SKIRMISH` inside `DLG_HOST`), so the button a test wants
may belong to a dialog that is not on top. The client resolves the name across all open dialogs.

Each command reports whether it resolved its target. `Invoke-Button`, `Set-ListSelection`,
`Set-SpinOption`, and `Set-EditText` return `$true` when the dialog and widget were found and the
action ran, and `$false` otherwise (a wrong name, or the dialog is not open). The relay holds the
request open until the client answers, so a command to an absent target is reported, not silently
dropped. A test therefore waits for a dialog and then acts on it: `Step-ToDialog` retries the click
while the source dialog is not yet present and stops once the target dialog appears or the timeout
elapses.

## Driving through a sequence of dialogs

Several game flows present a sequence of modal dialogs, each closed by a specific button before the
flow continues. The first turn is the main example: the scenario briefing, the new-day income dialog,
the lord-naming dialog, and message boxes appear in turn for each player.

Detect the current dialog with `Get-Dialog` (or `Get-RoleState` for the full state). Map each expected
dialog to the button that closes it, and click that button whenever the dialog is current:

```powershell
$Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'
    'DLG_BEGIN_TURN'        = 'BTN_OK'
    'DLG_GETINFO_BOX'       = 'BTN_CLOSE'
    'DLG_MESSAGE_BOX'       = 'BTN_OK'
}
while (-not (Get-RoleState $role).reachedStrategic) {
    $d = Get-Dialog $role
    if ($Dismiss.ContainsKey($d)) { Invoke-Button $role $d $Dismiss[$d] }
    Start-Sleep -Milliseconds 2000
}
```

Three points govern this loop.

1. Pace the clicks. Custom game menus advance one frame at a time, far more slowly than the native
   menus. Clicking the same dialog again before it has processed the previous click queues a duplicate
   command. A pause of about two seconds is enough; track the last dialog clicked so the same one is
   not clicked twice in a row.
2. A dialog that only flashes by can be missed by polling. The relay records a sticky per-role flag
   the first time it sees such a dialog: `sawBeginTurn` becomes true on the first `DLG_BEGIN_TURN` and
   stays true, and `reachedStrategic` latches the same way on the map view. Read these flags through
   `Get-RoleState` instead of trying to catch the dialog in a poll.
3. Closing a dialog updates the snapshot on its own. The reporter polls the engine's real topmost
   interface each frame, so when a modal closes and reveals the dialog beneath it, the snapshot reports
   the revealed dialog with no further action from the script.

The lord-naming dialog `DLG_GETINFO_BOX` is closed by a single `BTN_CLOSE`. It already holds the
lord's default name, so do not call `Set-EditText` on it: setting the text resets the input and
prevents the close from committing. `DriveToStrategic` in
[`multiplayer-two-instance.ps1`](multiplayer-two-instance.ps1) is the complete implementation.

## Adding a test

Write `tools/test/<name>.ps1`. Take `param([string]$GameDir, [switch]$Kill)`, dot-source `_relay.ps1`,
call `Resolve-GameDir`, `Start-TestRelay`, and `Start-GameClient`, drive the client, check the result,
and stop the relay and clients in a `finally` block. An orphaned relay holds its named pipe and blocks
the next run. Exit with code 1 on failure.

A two-instance or otherwise coordinated test runs entirely through the relay: send a command, wait,
read the resulting state, act. A minimal single-instance test may instead use the built-in self-nav
(add `SELFNAV` to the client flags; see [`walk-menu.ps1`](walk-menu.ps1)).

## Adding a CI job

CI lives in [`../../.github/workflows`](../../.github/workflows). The reusable suite is `tests.yml`.
Two entrypoints call it on disjoint path filters: `mss32.yml` (on `mss32/**`) rebuilds the DLL and
runs the suite against it; `tests-only.yml` (on `tools/**`) reuses the last built DLL without
recompiling.

To add a test to CI, copy one of the test jobs in `tests.yml` (for example
`multiplayer-strategic`) and change its final step to run your script with
`-GameDir "$env:GAME_DIR" -Kill`. Each test job downloads the `mss32-debugtest` artifact, restores the
cached game, deploys `mss32.dll`, and runs the script. CI has no config file, so `-GameDir` is passed
explicitly and `-Kill` makes the run clean up after itself.

## Files

| File | Role |
|---|---|
| `_relay.ps1` | the toolkit: config, relay, clients, and the commands above |
| `test.config.sample.psd1` | per-machine config template; copy to `test.config.psd1` |
| `scenario-generation.ps1` | single-instance form-driving example |
| `multiplayer-two-instance.ps1` | two clients into a started multiplayer game (host and joiner) |
| `reliability_test.ps1` | boot N times to the main menu (the CI boot test) |
| `walk-menu.ps1` | one self-nav client, left running for manual inspection |
| `_show-window.ps1`, `_capture.ps1` | bring a window forward, capture a PNG (diagnostics only) |
| `../relay/relay.js` | the relay |
| `../relay/drive-game-relay.ps1` | interactive console over the relay |

---

## RU

Каркас для тестирования интерфейса Disciples 2. Тестовый скрипт на PowerShell запускает один или два
экземпляра игры, читает состояние их интерфейса и подаёт ему команды (нажатие кнопок, выбор в списках,
переключение спиннеров, ввод текста) через локальный процесс-посредник (рилей). Проверки опираются на
состояние интерфейса, а не на скриншоты или содержимое логов.

## Архитектура

Каркас состоит из трёх слоёв.

| Слой | Назначение | Расположение |
|---|---|---|
| Клиент игры | Сборка `mss32.dll` в конфигурации DebugTest, загружаемая `Discipl2.exe`. Репортёр (`uistatereporter`) отслеживает текущий диалог и каждый кадр перечисляет все его контролы в JSON-снапшот. Исполнитель (`autonav`) выполняет каждую входящую команду на UI-потоке игры. Тестовой логики не содержит. | `mss32/src/testdrv/` |
| Рилей | Node.js-сервер без зависимостей. Хранит последний снапшот каждого клиента по ключу-роли и пересылает команды диспетчера клиенту. Тестовой логики не содержит. | `tools/relay/relay.js` |
| Диспетчер | Тестовый скрипт на PowerShell. Запускает клиентов, читает состояние их интерфейса, подаёт команды, проверяет результат и координирует двух клиентов, когда тесту нужны оба. | `tools/test/*.ps1` |

Рилей нужен потому, что диспетчер и игра работают в разных процессах. Диспетчер не может читать
интерфейс игры из её памяти или вызывать её напрямую. Клиент игры сообщает свой интерфейс рилею и
выполняет команды, которые рилей доставляет; рилей служит общей точкой, к которой подключаются и диспетчер, и
каждый клиент. Он же позволяет одному диспетчеру вести два клиента сразу (хост и присоединяющегося),
обращаясь к каждому по его роли.

Поток данных:

```
диспетчер  ->  рилей  ->  клиент игры      команды    (POST /api/ui/invoke, /select, /spin, /edit)
диспетчер  <-  рилей  <-  клиент игры      состояние  (GET  /api/ui)
```

Диспетчер читает `GET /api/ui`, получая снапшот, который клиент прислал последним (опкодом
`UiSnapshot`), и шлёт `POST /api/ui/*`, чтобы передать команду клиенту (соответствующим командным
опкодом). Клиент выполняет команду на UI-потоке и присылает обновлённый снапшот на следующем кадре.

## Снапшот интерфейса

`GET /api/ui?role=<role>` возвращает текущий диалог и все его контролы:

```json
{ "role": "host", "dialog": "DLG_PROTOCOL", "widgets": [
  { "name": "BTN_CONTINUE",   "type": "button",  "state": { "enabled": true } },
  { "name": "TLBOX_PROTOCOL", "type": "listbox", "state": { "selected": 2, "total": 3 } },
  { "name": "EDIT_NAME",      "type": "edit",    "state": { "text": "AutoTest" } } ] }
```

| `type` | Контрол | Поля `state` |
|---|---|---|
| `button` | кнопка | `enabled` (bool) |
| `listbox` | список | `selected`, `total` (int) |
| `spin` | спин-кнопка | `index` (int), `text` (текущая опция) |
| `edit` | поле ввода | `text` |
| `text` | статический текст | `text` |
| `picture`, `toggle`, `radio`, `scrollbar` | прочие контролы | нет |

Месседж-бокс является обычным диалогом (`DLG_MESSAGE_BOX`); его текст хранится в поле `text` текстового виджета,
поэтому в снапшоте он виден так же, как любой другой диалог.

## Включение каркаса

Каркас попадает в `mss32.dll` только в конфигурации **DebugTest** (дефайн `D2_TESTDRV`). Сборки Debug и
Release побайтно совпадают с неизменённой DLL.

Внутри сборки DebugTest каждая возможность включается во время выполнения переменной среды `D2TESTDRV_*`.
`Start-GameClient` задаёт их запускаемому процессу игры: `D2TESTDRV_UI_REPORTER` (снапшот),
`D2TESTDRV_RELAY_BRIDGE` (командный мост), `D2TESTDRV_SKIP_INTRO` и `D2TESTDRV_BLACKSCREEN_FIX`
(boot-фиксы), `D2TESTDRV_ROLE` (ключ-роль у рилея). Чтобы запустить возможность вручную, задайте
те же переменные перед запуском `Discipl2.exe`.

## Настройка

1. Конфигурация. Скопируйте `test.config.sample.psd1` в `test.config.psd1` (он в gitignore) и задайте
   `GameDir` как путь к установке Disciples 2. Скрипты читают его, когда `-GameDir` не передан в командной
   строке, и при неверном пути сообщают, что исправить.
2. DLL. Соберите `mss32.dll` в конфигурации DebugTest и положите её в `GameDir`, рядом с `Discipl2.exe`
   и переименованной `Mss23.dll`.
3. Node.js в `PATH`. Диспетчер запускает `relay.js`.

## Запуск теста

```powershell
.\scenario-generation.ps1             # один экземпляр
.\multiplayer-two-instance.ps1 -Kill  # хост и присоединяющийся доходят до стратегической карты
```

## Пример: scenario-generation.ps1

[`scenario-generation.ps1`](scenario-generation.ps1) ведёт форму генератора случайных карт одним
клиентом. Он проходит мультиплеерное меню до генератора (`DLG_RANDOM_SCENARIO_MULTI`), читает снапшот,
чтобы убедиться в наличии формы, и задействует каждый тип команды:

```powershell
. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir       # из -GameDir, иначе из test.config.psd1
$relay   = Start-TestRelay
$client  = Start-GameClient -GameDir $GameDir -Role host

Wait-Dialog   host DLG_MAIN_MENU 90
Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL
Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2   # TCP/IP
# ... далее до DLG_RANDOM_SCENARIO_MULTI ...

$names = (Get-GameUi host).widgets.name
Set-ListSelection host $D TLBOX_TEMPLATES 3
Set-EditText      host $D EDIT_NAME "AutoTest"
Set-SpinOption    host $D SPIN_SIZE 1
Invoke-Button     host $D BTN_GENERATE
```

Тест проходит, когда диалог генератора открылся, каждый шаг навигации привёл к ожидаемому диалогу,
нужные виджеты присутствуют, и клиент остаётся на диалоге генератора после прохода по форме.

## Команды

Из [`_relay.ps1`](_relay.ps1); подключите его через `.`. `<role>` это `host`, `join` и т. п., и
совпадает с `D2TESTDRV_ROLE` клиента.

| Команда | Действие | Endpoint |
|---|---|---|
| `Resolve-GameDir [$GameDir]` | папка игры, из `-GameDir` или конфига, с проверкой | нет |
| `Start-TestRelay` | запустить `relay.js`, вернуть процесс | нет |
| `Start-GameClient -GameDir <d> -Role <r>` | запустить клиента DebugTest | нет |
| `Get-Dialog <role>` | имя текущего диалога | `GET /api/state` |
| `Get-GameUi <role>` | `{ role, dialog, widgets }`, все виджеты с их состоянием | `GET /api/ui` |
| `Get-RoleState <role>` | состояние роли `{ dialog, widgets, connected, ... }` и липкие флаги | `GET /api/state` |
| `Wait-Dialog <role> <dlg> [sec]` | ждать, пока клиент окажется на `<dlg>` | нет |
| `Invoke-Button <role> <dlg> <btn>` | нажать кнопку | `POST /api/ui/invoke` |
| `Set-ListSelection <role> <dlg> <listbox> <i>` | выбрать элемент списка | `POST /api/ui/select` |
| `Set-SpinOption <role> <dlg> <spin> <i>` | задать опцию спин-кнопки | `POST /api/ui/spin` |
| `Set-EditText <role> <dlg> <edit> <text>` | задать текст поля ввода | `POST /api/ui/edit` |
| `Step-ToDialog <role> <dlg> <btn> <toDlg> [sec]` | нажимать `<btn>`, пока клиент не дойдёт до `<toDlg>` | нет |

Для интерактивной работы подключите [`../relay/drive-game-relay.ps1`](../relay/drive-game-relay.ps1):
он даёт те же команды плюс инспекторы только для чтения `Get-GameStatus`, `Get-GameLog`,
`Get-GameChat`, `Get-GameEvents`.

## Поиск имён диалогов и виджетов

Запустите один клиент и читайте снапшот через `Get-GameUi` по ходу навигации. Он перечисляет каждый
контрол текущего диалога с именем, типом и состоянием, включая списки, спин-кнопки и поля ввода:

```powershell
$relay = Start-TestRelay; $c = Start-GameClient -GameDir (Resolve-GameDir) -Role host
Get-GameUi host | ForEach-Object widgets | Format-Table name, type
Invoke-Button host DLG_MAIN_MENU BTN_MULTI
Get-Dialog host    # прочитать следующий диалог, затем его виджеты
```

## Адресация виджетов по имени

Команда адресует виджет по имени диалога, а не по верхнему диалогу. Игра держит несколько диалогов
открытыми одновременно (например, `DLG_CHOOSE_SKIRMISH` внутри `DLG_HOST`), поэтому нужная тесту кнопка
может принадлежать диалогу, который не находится сверху. Клиент разрешает имя по всем открытым диалогам.

Каждая команда сообщает, разрешила ли она свою цель. `Invoke-Button`, `Set-ListSelection`,
`Set-SpinOption` и `Set-EditText` возвращают `$true`, когда диалог и виджет найдены и действие
выполнено, и `$false` иначе (неверное имя или диалог не открыт). Рилей держит запрос открытым, пока
клиент не ответит, поэтому команда на отсутствующую цель сообщается, а не теряется молча. Поэтому тест
ждёт диалог и затем действует на нём: `Step-ToDialog` повторяет нажатие, пока исходный диалог ещё не
появился, и останавливается, когда появляется целевой диалог или истекает тайм-аут.

## Прохождение последовательности диалогов

Ряд игровых сценариев показывает последовательность модальных диалогов, каждый из которых закрывается
определённой кнопкой, прежде чем сценарий продолжится. Главный пример, первый ход: брифинг сценария,
диалог дохода нового дня, диалог имени лорда и месседж-боксы появляются по очереди у каждого игрока.

Определяйте текущий диалог через `Get-Dialog` (или `Get-RoleState` для полного состояния).
Сопоставьте каждому ожидаемому диалогу закрывающую его кнопку и нажимайте её, когда диалог текущий:

```powershell
$Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'
    'DLG_BEGIN_TURN'        = 'BTN_OK'
    'DLG_GETINFO_BOX'       = 'BTN_CLOSE'
    'DLG_MESSAGE_BOX'       = 'BTN_OK'
}
while (-not (Get-RoleState $role).reachedStrategic) {
    $d = Get-Dialog $role
    if ($Dismiss.ContainsKey($d)) { Invoke-Button $role $d $Dismiss[$d] }
    Start-Sleep -Milliseconds 2000
}
```

Этим циклом управляют три обстоятельства.

1. Соблюдайте паузы между нажатиями. Кастомные игровые меню продвигаются по одному кадру и заметно
   медленнее нативных. Повторное нажатие того же диалога до обработки предыдущего нажатия ставит в
   очередь дублирующую команду. Паузы около двух секунд достаточно; запоминайте последний нажатый
   диалог, чтобы не нажимать один и тот же дважды подряд.
2. Диалог, который лишь мелькает, опрос может пропустить. Рилей записывает липкий флаг роли при первом
   появлении такого диалога: `sawBeginTurn` становится истинным на первом `DLG_BEGIN_TURN` и остаётся
   таким, а `reachedStrategic` так же фиксируется на виде карты. Читайте эти флаги через `Get-RoleState`,
   а не пытайтесь поймать диалог опросом.
3. Закрытие диалога обновляет снапшот само. Репортёр каждый кадр опрашивает реальный верхний интерфейс
   движка, поэтому при закрытии модального окна и появлении диалога под ним снапшот сообщает открывшийся
   диалог без действий со стороны скрипта.

Диалог имени лорда `DLG_GETINFO_BOX` закрывается одним `BTN_CLOSE`. В нём уже находится имя лорда по
умолчанию, поэтому не вызывайте на нём `Set-EditText`: запись текста сбрасывает ввод и мешает закрытию
зафиксироваться. Полную реализацию содержит `DriveToStrategic` в
[`multiplayer-two-instance.ps1`](multiplayer-two-instance.ps1).

## Добавление теста

Создайте `tools/test/<имя>.ps1`. Возьмите `param([string]$GameDir, [switch]$Kill)`, подключите
`_relay.ps1`, вызовите `Resolve-GameDir`, `Start-TestRelay` и `Start-GameClient`, ведите клиента,
проверьте результат и остановите рилей и клиентов в блоке `finally`. Осиротевший рилей удерживает свой
именованный канал и блокирует следующий запуск. Завершайтесь кодом 1 при провале.

Двух-инстансный или иначе координированный тест работает целиком через рилей: отправить команду,
подождать, прочитать итоговое состояние, действовать. Минимальный одиночный тест может вместо этого
использовать встроенный self-nav (добавьте `SELFNAV` к флагам клиента; см. [`walk-menu.ps1`](walk-menu.ps1)).

## Добавление джоба в CI

CI находится в [`../../.github/workflows`](../../.github/workflows). Переиспользуемый набор называется `tests.yml`.
Его вызывают два входа по непересекающимся фильтрам путей: `mss32.yml` (на `mss32/**`) пересобирает DLL
и прогоняет по ней набор; `tests-only.yml` (на `tools/**`) переиспользует последнюю собранную DLL без
перекомпиляции.

Чтобы добавить тест в CI, скопируйте один из тест-джобов в `tests.yml` (например,
`multiplayer-strategic`) и замените его последний шаг на запуск вашего скрипта с
`-GameDir "$env:GAME_DIR" -Kill`. Каждый тест-джоб скачивает артефакт `mss32-debugtest`, восстанавливает
игру из кэша, разворачивает `mss32.dll` и запускает скрипт. Конфига в CI нет, поэтому `-GameDir`
передаётся явно, а `-Kill` обеспечивает уборку после прогона.

## Файлы

| Файл | Роль |
|---|---|
| `_relay.ps1` | тулкит: конфиг, рилей, клиенты и команды выше |
| `test.config.sample.psd1` | шаблон конфига машины; копируется в `test.config.psd1` |
| `scenario-generation.ps1` | пример драйва формы одним экземпляром |
| `multiplayer-two-instance.ps1` | два клиента в начатую мультиплеерную игру (хост и присоединяющийся) |
| `reliability_test.ps1` | загрузка N раз до главного меню (бут-тест CI) |
| `walk-menu.ps1` | один self-nav клиент, оставленный запущенным для ручного осмотра |
| `_show-window.ps1`, `_capture.ps1` | поднять окно, снять PNG (только диагностика) |
| `../relay/relay.js` | рилей |
| `../relay/drive-game-relay.ps1` | интерактивная консоль над рилеем |
