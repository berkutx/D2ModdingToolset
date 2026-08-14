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
| Game client | A test-harness-enabled `mss32.dll` (DebugTest or Release with `D2TestDrv=true`), loaded by `Discipl2.exe`. Its reporter (`uistatereporter`) tracks the current dialog and, every frame, enumerates all of its controls into a JSON snapshot. Its executor (`autonav`) runs each incoming command on the game's UI thread. It contains no test logic. | `mss32/src/testdrv/` |
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

Windows clients use the named pipe by default. Headless Wine/Docker runners and parallel Windows
headless hosts use the same framed
protocol over TCP: set `D2TESTDRV_BRIDGE_TCP_HOST`/`D2TESTDRV_BRIDGE_TCP_PORT` for the game process
and `D2_RELAY_TCP_HOST`/`D2_RELAY_TCP_PORT` for the relay. TCP is a supported transport for the
headless launcher, not a packet-inspection or gameplay hook. `D2_RELAY_HTTP_HOST`/`D2_RELAY_HTTP_PORT`
select each relay's HTTP control endpoint. The native TCP client resolves DNS/container service names,
reconnects after a relay restart, performs a fresh `Hello`, and republishes current snapshots.

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

Custom-lobby chat is a separate opt-in snapshot. Start a client with `-LobbyChat` (or set
`D2TESTDRV_LOBBY_CHAT=1`) and call `Get-LobbyChat <role>` (`GET /api/lobby/chat`). The reporter observes
the game's natural `CMenuCustomLobby::addChatMessage` call, converts CP1251 text to UTF-8, and retains at
most 80 messages. Chat stays out of frequently polled `/api/state`; no packet interception is involved.

## The world snapshot

Beyond the interface, any harness-enabled client also reports live game state once a scenario is loaded. With
the `D2TESTDRV_WORLD` flag (set by `Start-GameClient`), the client walks the scenario on the
strategic-map tick (throttled to at most about twice a second, not every frame) and publishes a second
snapshot the dispatcher reads with `Get-World <role>` (`GET /api/world`):

```json
{ "day": 1,
  "strategicActionReady": true,
  "players": [
    { "id": "...", "relation": "self", "human": true, "race": 1, "gold": 200, "lifeMana": 0 } ],
  "stacks": [
    { "id": "S143KC0001", "x": 38, "y": 38, "owner": "...", "relation": "self",
      "movement": 33, "units": 1, "hp": 95, "subrace": 1, "inside": true } ] }
```

`strategicActionReady` is the world reporter's UI-thread observation of the same stock admission used
by `Move-Stack`: `CPhaseGameData::clientTakesTurn` is true and the existing phase object-lock predicate
is clear. It does not add queue policy or simultaneous-turn semantics. The relay exposes the same
boolean in `/api/state` and `/api/world`, while the native action checks it again authoritatively before
sending. Before a one-shot strategic action, require a connected role, the current strategic-map
dialog, `sawBeginTurn`, and this bit; never probe or retry the action to discover readiness.

`relation` tags each player and stack `self`, `neutral`, or `enemy`. In a single-instance game `self`
is the player whose turn it is (the reporter has no network client to name the local human), so read
the world only on your own turn; in multiplayer it is the local client's own player. `Get-Resources
<role>` returns the local player's row; `Get-Stacks <role>` returns the stack list. The stack fields a
movement or battle test relies on:

| Field | Meaning |
|---|---|
| `x`, `y` | the stack's tile. For a garrisoned stack (`inside` true) this is the fort ANCHOR (its top-left reference cell), not the hero's real cell. |
| `movement` | movement points left this turn; a move decrements them. |
| `units` | number of units in the group. |
| `hp` | total current HP of the group. A battle drops it (damage) even when no unit is killed, so it is the finest signal that a fight took place. |
| `inside` | true when the stack is INSIDE a fort, city, or village (a garrison). Such a stack reports the fort anchor and is attacked as a siege, so the monster test skips it. |

The snapshot only appears once the strategic map is up; in the menus and during the initial scenario
load it is empty, so a test POLLS `Get-Stacks` after reaching the map until the stacks are present.

## Enabling the harness

The harness capability is the **`D2_TESTDRV`** compile gate; Debug/Release is the independent build
profile. The internal **DebugTest** configuration is Debug (`/Od`, `_DEBUG`, debug CRT/dependencies)
with that gate enabled by default, and CI publishes it as `mss32-test-harness-debug`. Passing
`/p:D2TestDrv=true` to the ordinary Release profile produces the optimized
`mss32-test-harness-release`; it is not a separate artificial “ReleaseTest” configuration. Plain
Release keeps the gate off and is published as `mss32-production-release`. Every PR builds all three:
full gameplay remains on DebugTest, while the strict template test runs on both harness profiles.

Within a test-harness build, each feature is switched on at runtime by a `D2TESTDRV_*` environment
variable. `Start-GameClient` sets these on the launched game process: `D2TESTDRV_UI_REPORTER` (the
snapshot), `D2TESTDRV_RELAY_BRIDGE` (the command bridge), `D2TESTDRV_SKIP_INTRO` and
`D2TESTDRV_BLACKSCREEN_FIX` (boot fixes), and `D2TESTDRV_ROLE` (the relay key). To run a feature
by hand, set the same variables before launching `Discipl2.exe`.

## Setup

1. Configuration. Copy `test.config.sample.psd1` to `test.config.psd1` (which is gitignored) and set
   `GameDir` to the Disciples 2 installation. The scripts read it whenever `-GameDir` is not given on
   the command line, and report the path to correct if it is wrong.
2. DLL. Build the Debug-profile test-harness `mss32.dll` (`DebugTest` configuration) and place it in
   `GameDir`, alongside `Discipl2.exe` and the renamed `Mss23.dll`. For the optimized A/B profile,
   build ordinary `Release` with `/p:D2TestDrv=true` and deploy that DLL instead.
3. Node.js on `PATH`. The dispatcher starts `relay.js`.

## Running a test

```powershell
.\scenario-generation.ps1                         # single instance: drive the generator form
.\scenario-generation.ps1 -Template 7 -ToMap      # single instance: generate, then play into the strategic map
.\multiplayer-two-instance.ps1 -Kill              # host and joiner reach the strategic map
.\multiplayer-two-instance.ps1 -Kill -RandomMap -EndHostTurn -Scenario 7   # host GENERATES the map, then an honest turn-pass
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
Set-SpinOption    host $D SPIN_SIZE 0   # only an index exposed by this template is valid
Invoke-Button     host $D BTN_GENERATE
```

The test passes when the generator dialog opens, each navigation step produces the expected dialog,
the expected widgets are present, and the client is still on the generator dialog after the form is
driven.

With `-WaitGenerationSec` the test also waits for the map to generate and asserts it reaches
`DLG_GENERATION_RESULT`; with `-ToMap` it goes on to accept the map, start the game, and reach the
strategic map. The generator fails several ways, each reported separately: a sol3 panic that
leaves the client on the form, a debug-CRT assert (a generator heap fault) that the DebugTest build
routes to the log and fails the run on instead of a blocking dialog, an error box (`DLG_MESSAGE_BOX`)
raised after the generator gives up, or a timeout still on `DLG_WAIT_GENERATION`. The two-instance
[`multiplayer-two-instance.ps1`](multiplayer-two-instance.ps1) `-RandomMap` builds the session from a
generated map instead of a skirmish, so with `-EndHostTurn` it runs the full honest flow: generate,
both clients reach the map, the host ends its turn, and the joiner clicks through its own new day.

## Commands

From [`_relay.ps1`](_relay.ps1); dot-source it. `<role>` is `host`, `join`, and so on, and matches
the client's `D2TESTDRV_ROLE`.

| Command | Action | Endpoint |
|---|---|---|
| `Resolve-GameDir [$GameDir]` | the game folder, from `-GameDir` or the config, validated | none |
| `Start-TestRelay` | start `relay.js`, return its process | none |
| `Start-GameClient -GameDir <d> -Role <r>` | launch a client with either harness DLL profile | none |
| `Get-Dialog <role>` | the current dialog name | `GET /api/state` |
| `Get-GameUi <role>` | `{ role, dialog, widgets }`, every widget with its state | `GET /api/ui` |
| `Get-RoleState <role>` | one role's `{ dialog, widgets, connected, ... }` and latched flags | `GET /api/state` |
| `Wait-Dialog <role> <dlg> [sec]` | wait until the client is on `<dlg>` | none |
| `Invoke-Button <role> <dlg> <btn>` | click a button | `POST /api/ui/invoke` |
| `Set-ListSelection <role> <dlg> <listbox> <i>` | set a list selection | `POST /api/ui/select` |
| `Set-SpinOption <role> <dlg> <spin> <i>` | set a spin-button option | `POST /api/ui/spin` |
| `Set-EditText <role> <dlg> <edit> <text>` | set an edit-box's text | `POST /api/ui/edit` |
| `Invoke-Toggle <role> <dlg> <toggle>` | flip a toggle button (e.g. auto-battle) | `POST /api/ui/toggle` |
| `Step-ToDialog <role> <dlg> <btn> <toDlg> [sec]` | click `<btn>` until the client reaches `<toDlg>` | none |
| `Get-World <role>` | the world snapshot `{ day, strategicActionReady, players, stacks }` | `GET /api/world` |
| `Get-Resources <role>` | the local player's resource row | `GET /api/world` |
| `Get-Stacks <role>` | every stack on the map | `GET /api/world` |
| `Move-Stack <role> <id> <x> <y>` | move a stack to a tile, or onto a monster to attack | `POST /api/ui/move` |
| `Resolve-TemplateIndex <gameDir> <name>` | the 0-based generator-template index for a template name | none |

For interactive use, dot-source [`../relay/drive-game-relay.ps1`](../relay/drive-game-relay.ps1): it
provides the same commands plus the read-only inspectors `Get-GameStatus`, `Get-GameUi`,
`Get-Dialog`, and `Get-World`.

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
action was accepted for execution, and `$false` otherwise (a wrong name, or the dialog is not open).
This result is not proof that a synchronous callback completed successfully; the test must observe
the expected state transition. The relay holds the request open until the client answers, so a command
to an absent target is reported, not silently dropped. A test therefore waits for a dialog and then
acts on it: `Step-ToDialog` uses a bounded retry policy only for coalescible navigation and stops once
the target dialog appears or the timeout elapses. Semantic actions such as accepting a generated map
are dispatched once and verified by their effect.

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

## Battle testing

A battle test drives the whole "approach a monster and fight it" flow through the same native paths a
real player uses, with no input emulation: it reads the world snapshot, issues one move command per
step, and asserts against the snapshot. [`attack-monster.ps1`](attack-monster.ps1) is the canonical
template. Its steps, in order:

1. Reach the map. Generate a random skirmish (Single Player -> New Skirmish -> Random Map) and drive
   the first-turn popups to `DLG_STRATEGIC`/`DLG_ISO_PAL`, the way `scenario-generation.ps1 -ToMap`
   does. Skirmish is the single-instance, sequential-turn mode, so the local player is the one whose
   turn it is.
2. Exit the garrison. The starting hero sits INSIDE its capital, so the snapshot reports the fort
   anchor, not the hero's tile, and the first action is always a free exit step. Issue it with
   `Move-Stack <role> <heroId> (cx+5) (cy+5)`, where `(cx,cy)` is the reported anchor position: the
   client detects the garrisoned stack and replicates the game's exact 0-cost exit move. Do not probe or
   retry it: the first-day `DLG_BEGIN_TURN` can appear after the iso view while the stock turn/object-lock
   admission is still closed. Acknowledge each known startup dialog at most once,
   then wait for a connected current map with `sawBeginTurn` and `strategicActionReady` both true,
   refresh the hero precondition, and issue this sole exit intent. Finally wait for the reported position
   to change to a real tile and treat that as the hero's starting tile.
3. Pick the target. From `Get-Stacks`, take the nearest stack whose `relation` is `neutral` and whose
   `inside` is false: a free neutral monster. Filter on `neutral` specifically, not just non-self, so
   an enemy AI player's roaming stack is never chosen as the "monster"; skip `inside` stacks, which are
   garrisons (a city, fort, or village) reported at the fort anchor and fought as a siege.
4. Snapshot BEFORE. Record the monster's and the hero's `x`/`y`, `units`, and `hp`, and whether the
   monster is already adjacent to the hero (Chebyshev distance of 1 or less).
5. Attack. `Move-Stack <role> <heroId> <monX> <monY>` onto the monster's tile. The client routes the
   hero adjacent and sets the move message's `end` to the monster tile, so the server starts the
   battle exactly as clicking the enemy stack would: the exact `DLG_BATTLE_A` or `DLG_BATTLE_B` viewer
   opens, and the UI reporter sees it.
6. Auto-battle. `Invoke-Toggle` on that exact A/B viewer's `TOG_AUTOBATTLE` hands the fight to the game's AI,
   which plays every round (this is the in-game auto-battle, not the instant resolve), and the battle
   ends on its own. Auto-battle is a toggle, not a button, so it needs `Invoke-Toggle`, not
   `Invoke-Button`.
7. Dismiss the post-battle dialogs. A battle is followed by a result screen and, often, one or more
   reward or dropped-item dialogs. Click the forward button (any of `BTN_CLOSE`, `BTN_OK`,
   `BTN_TAKEALL`, `BTN_TAKE`, `BTN_CONTINUE`, `BTN_RIGHTSIDE`) on each, the same way the first-turn
   sequence is driven, until `DLG_STRATEGIC`/`DLG_ISO_PAL` returns. Do not blind-click a generic
   `BTN_YES` here, and do not treat the battle viewer (`DLG_BATTLE_A/B`) as stuck while it is still up
   (a long auto-battle keeps it open); bound the rest with a no-progress guard so an unrecognized
   reward dialog fails fast instead of spinning the whole timeout.
8. Verify by reading one clean post-battle snapshot, then comparing. Poll `Get-World` until the GET
   succeeds, so an absent stack means a real removal and not a dropped request; then look up the hero
   and the monster by id. The run passes when both of these hold:
   - The fight resolved: the monster is gone from the census, OR its `hp` or `units` dropped, OR the
     hero's `hp` or `units` dropped, OR the hero itself is gone (a lost battle that destroyed the
     party). Someone usually dies, but at the least a fought battle deals damage, so the group `hp` is
     the key signal: a lone leader against a two-unit monster may kill nobody yet still show HP loss on
     both sides.
   - The hero approached: if the hero still exists, its position differs from its post-exit tile. The
     one exception is a monster already adjacent at step 4, where no approach step is expected; if the
     hero was destroyed, the approach check is moot.

The moves in a real fight always differ from the starting position (the approach check); the only case
where they do not is a monster directly adjacent to the garrison exit, which almost never happens.
Pace the post-battle dialog loop like any other dialog sequence: about one click every 0.7 to 2
seconds, tracking the last dialog so the same one is not clicked twice before it advances.

This battle flow is factored into [`_battle.ps1`](_battle.ps1) (`Invoke-HeroAttack`) and reused by the
multiplayer test [`mp-attack-monsters.ps1`](mp-attack-monsters.ps1). CI copies the repository-pinned
`assets/mp-battle-regen.sg` into the disposable game's `Exports` as its sole slot; no map generator or
external loader participates. The small deterministic scenario gives each player one free fighter, a
distinct nearby short-fight neutral, and reserve targets. Each player attacks once and ends its turn. After the day rolls
over the test logs daily income and the regeneration of every damaged survivor (a winning hero that
took hits, or a surviving monster). Regeneration is unit and timing-dependent, so it is printed for
observation rather than hard-gated; pass `-MinRegenPct 5` for a strict floor. The two clients exchange
the real game messages over DirectPlay (begin-turn, stack-move, battle), so the run exercises turn-pass,
movement points, income, and combat HP end to end. Random generation remains independently covered by
the generated-map turn-pass job and the full template matrix.

## Adding a test

Write `tools/test/<name>.ps1`. Take `param([string]$GameDir, [switch]$Kill)`, dot-source `_relay.ps1`,
call `Resolve-GameDir`, `Start-TestRelay`, and `Start-GameClient`, drive the client, check the result,
and stop the relay and clients in a `finally` block. An orphaned relay holds its named pipe and blocks
the next run. Exit with code 1 on failure.

A two-instance or otherwise coordinated test runs entirely through the relay: send a command, wait,
read the resulting state, act. A minimal single-instance test may instead use the built-in self-nav
(add `SELFNAV` to the client flags; see [`walk-menu.ps1`](walk-menu.ps1)).

## Adding a CI job

CI lives in [`../../.github/workflows`](../../.github/workflows). `test-harness.yml` is the single
automated entrypoint for every PR and relevant `dev`/`master` push. It validates every PowerShell/
JavaScript tool and both relay transports, builds DebugTest and Release+D2_TESTDRV harness DLLs, calls
the complete `tests.yml` gameplay suite on DebugTest, and independently builds exact-checkout plain
production Release through the reusable `mss32-production-build.yml`. `mss32.yml` is the manual
production Debug/Release entrypoint, so it does not duplicate an automated PR run.

The strict template matrix checks out `bartonsun/D2-Templates` (`master` by default), resolves it once
to a commit, flattens `sMNS/**/*.lua` into one immutable snapshot, and records source paths, sizes and
SHA-256 hashes. Each file gets one job under DebugTest and one under optimized Release+D2_TESTDRV,
using the same source snapshot, valid template defaults and digest-verified Mesa asset. Each job makes
five sequential launches of the real game client with the game's natural randomness. Attempts 1-4
must reach the generator result; attempt 5 also accepts the result, starts the scenario, clicks each
known enabled startup dialog once, observes `DLG_BEGIN_TURN`, and requires a populated self world on
the actionable bare strategic map. The ready map remains visible for three seconds in the proof video.

A CRT assert, sol3 panic, generator error, timeout, rejected/unplayable map, parser omission, missing
outcome, or required-video failure is red. `fail-fast:false` still runs every source file in both
profiles. All five outcome rows and per-attempt MSS/relay/OBS/console evidence sets are retained for
90 days. If all five attempts pass, only attempt 5's single-window MKV is retained; if any fail, only
the failed-attempt MKVs are retained. Because DebugTest and Release use independent natural-random
runs, their status comparison is diagnostic rather than a reproducibility gate; missing/malformed rows
or a source-manifest mismatch still fail it.

To add a test to CI, copy one of the test jobs in `tests.yml` (for example
`multiplayer-strategic`) and change its final step to run your script with
`-GameDir "$env:GAME_DIR" -Kill`. Each test job downloads the `mss32-test-harness-debug` artifact, restores the
cached game, deploys `mss32.dll`, and runs the script. CI has no config file, so `-GameDir` is passed
explicitly and `-Kill` makes the run clean up after itself.

## Files

| File | Role |
|---|---|
| `_relay.ps1` | the toolkit: config, relay, clients, and the commands above |
| `relay-transport-smoke.ps1` | framed-protocol smoke for named pipe and TCP: Hello/state/chat plus delayed command correlation without duplicate dispatch |
| `_battle.ps1` | the shared battle flow (`Invoke-HeroAttack`): use a required free hero or exit a garrisoned one, attack the nearest free neutral, auto-battle, dismiss dialogs, report before/after |
| `_obs.ps1` | owned portable-OBS setup/record/stop helper: two-pane multiplayer/static-arena or single-host template proof; required CI recordings start before the game and fail if no MKV is produced |
| `test.config.sample.psd1` | per-machine config template; copy to `test.config.psd1` |
| `scenario-generation.ps1` | single-instance generator example: drive the form, and with `-ToMap` play the generated map into the strategic screen |
| `list-templates.js` | enumerate valid generator templates in the same compact numeric order as the game; drives the automatic all-template matrix |
| `world-snapshot.ps1` | single-instance world-snapshot example: reach the map and read the live world state (player resources + map stacks) |
| `move-hero.ps1` | single-instance move example: exit the garrison and move the hero with the game's own pathfinding cost, verified through the world snapshot |
| `attack-monster.ps1` | single-instance battle template: exit, approach a free monster, attack, auto-battle, dismiss post-battle dialogs, and verify by HP / units / position |
| `mp-attack-monsters.ps1` | multiplayer battle test on the repository-pinned `assets/mp-battle-regen.sg`: both players attack their own nearby neutral and pass turns, then a damaged survivor's regeneration is verified after the day rolls over |
| `multiplayer-two-instance.ps1` | two clients into a started game (a skirmish, or with `-RandomMap` a generated map); `-EndHostTurn` adds the honest turn-pass |
| `reliability_test.ps1` | boot N times to the main menu (the CI boot test) |
| `walk-menu.ps1` | one self-nav client, left running for manual inspection |
| `lobby-create.ps1` | manual live-lobby integration test; not run in CI and requires explicit credentials |
| `luckytest-arena.ps1` | static authored-arena test exercising chests, camps, hire and squad layout for both roles; CI copies the repository-pinned `assets/luckytest-arena.sg`, never the LuckyTest loader/reroller |
| `assets/*.sg` | ordinary frozen scenario fixtures copied once into each disposable CI game; no runtime map loader, reroller or generator dependency |
| `HIRE-MERC.md`, `SLOT-MANAGEMENT.md` | contracts and RE notes for the optional LuckyTest action surface |
| `_show-window.ps1`, `_capture.ps1` | bring a window forward and capture a diagnostic PNG |
| `../relay/relay.js` | the relay |
| `../relay/drive-game-relay.ps1` | interactive console over the relay |

---

## RU

Тестовая система интерфейса Disciples 2. Тестовый скрипт на PowerShell запускает один или два
экземпляра игры, читает состояние их интерфейса и подаёт ему команды (нажатие кнопок, выбор в списках,
переключение спиннеров, ввод текста) через локальный процесс-посредник (рилей). Проверки опираются на
состояние интерфейса, а не на скриншоты или содержимое логов.

## Архитектура

Тестовая система состоит из трёх слоёв.

| Слой | Назначение | Расположение |
|---|---|---|
| Клиент игры | `mss32.dll` с включённой test-harness capability (DebugTest или Release с `D2TestDrv=true`), загружаемая `Discipl2.exe`. Репортёр (`uistatereporter`) отслеживает текущий диалог и каждый кадр перечисляет все его контролы в JSON-снапшот. Исполнитель (`autonav`) выполняет каждую входящую команду на UI-потоке игры. Тестовой логики не содержит. | `mss32/src/testdrv/` |
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

В Windows клиенты по умолчанию используют именованный канал. Headless-запуски под Wine/Docker и
параллельные Windows headless-хосты
используют тот же framed-протокол через TCP: процессу игры задаются
`D2TESTDRV_BRIDGE_TCP_HOST`/`D2TESTDRV_BRIDGE_TCP_PORT`, а рилею —
`D2_RELAY_TCP_HOST`/`D2_RELAY_TCP_PORT`. TCP — поддерживаемый транспорт headless-launcher-а,
а не перехват пакетов или gameplay-hook. `D2_RELAY_HTTP_HOST`/`D2_RELAY_HTTP_PORT` выбирают HTTP
control endpoint каждого рилея. Native TCP-клиент разрешает DNS/имена Docker services, после рестарта
рилея переподключается, повторяет `Hello` и заново публикует текущие снапшоты.

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

Чат custom lobby — отдельный opt-in снапшот. Запустите клиента с `-LobbyChat` (либо задайте
`D2TESTDRV_LOBBY_CHAT=1`) и вызовите `Get-LobbyChat <role>` (`GET /api/lobby/chat`). Репортёр наблюдает
естественный вызов игры `CMenuCustomLobby::addChatMessage`, преобразует CP1251 в UTF-8 и хранит не
больше 80 сообщений. Чат не попадает в часто опрашиваемый `/api/state`; перехвата пакетов здесь нет.

## Снапшот мира

Помимо интерфейса, любой клиент с включённым harness сообщает и живое состояние игры, как только загружен сценарий. С
флагом `D2TESTDRV_WORLD` (его задаёт `Start-GameClient`) клиент на тике стратегической карты (с
троттлингом не чаще примерно двух раз в секунду, не каждый кадр) обходит сценарий и публикует второй
снапшот, который диспетчер читает через `Get-World <role>` (`GET /api/world`):

```json
{ "day": 1,
  "strategicActionReady": true,
  "players": [
    { "id": "...", "relation": "self", "human": true, "race": 1, "gold": 200, "lifeMana": 0 } ],
  "stacks": [
    { "id": "S143KC0001", "x": 38, "y": 38, "owner": "...", "relation": "self",
      "movement": 33, "units": 1, "hp": 95, "subrace": 1, "inside": true } ] }
```

`strategicActionReady` — наблюдение UI-потока за тем же штатным admission, который использует
`Move-Stack`: `CPhaseGameData::clientTakesTurn` истинно и существующий object-lock фазы свободен. Этот
флаг не добавляет собственной политики очередей или семантики одновременных ходов. Рилей отдаёт тот
же bool и в `/api/state`, и в `/api/world`, а native action авторитетно проверяет его ещё раз перед
отправкой. Перед однократным стратегическим действием требуйте подключённую роль, текущий диалог
стратегической карты, `sawBeginTurn` и этот флаг; не пробуйте действие и не повторяйте его для
определения готовности.

`relation` помечает каждого игрока и каждый стек как `self`, `neutral` или `enemy`. В одиночной игре
`self` это игрок, чей сейчас ход (у репортёра нет сетевого клиента, чтобы назвать локального человека),
поэтому читайте мир только на своём ходу; в мультиплеере это собственный игрок локального клиента.
`Get-Resources <role>` возвращает строку локального игрока; `Get-Stacks <role>` возвращает список
стеков. Поля стека, на которые опирается тест перемещения или боя:

| Поле | Значение |
|---|---|
| `x`, `y` | клетка стека. Для стека в гарнизоне (`inside` истинно) это ЯКОРЬ форта (его верхняя-левая опорная клетка), а не реальная клетка героя. |
| `movement` | оставшиеся очки хода на этом ходу; перемещение их уменьшает. |
| `units` | число юнитов в отряде. |
| `hp` | суммарные текущие ХП отряда. Бой их снижает (урон), даже если никто не убит, поэтому это самый тонкий признак того, что бой состоялся. |
| `inside` | истинно, когда стек находится ВНУТРИ форта, города или деревни (гарнизон). Такой стек сообщает якорь форта и атакуется как осада, поэтому тест на монстра его пропускает. |

Снапшот появляется только после выхода стратегической карты; в меню и во время начальной загрузки
сценария он пуст, поэтому тест ОПРАШИВАЕТ `Get-Stacks` после выхода на карту, пока стеки не появятся.

## Включение тестовой системы

Test-harness capability задаётся compile gate **`D2_TESTDRV`**, а Debug/Release — независимый профиль
сборки. Внутренняя конфигурация **DebugTest** — это Debug (`/Od`, `_DEBUG`, debug CRT/dependencies`) с
включённым gate; CI публикует её как `mss32-test-harness-debug`. Обычный Release с параметром
`/p:D2TestDrv=true` даёт оптимизированный `mss32-test-harness-release`, без искусственной конфигурации
«ReleaseTest». Plain Release оставляет gate выключенным и публикуется как
`mss32-production-release`. Каждый PR собирает все три варианта: полный gameplay идёт на DebugTest,
а строгая проверка шаблонов — на обеих harness-сборках.

Внутри test-harness сборки каждая возможность включается во время выполнения переменной среды `D2TESTDRV_*`.
`Start-GameClient` задаёт их запускаемому процессу игры: `D2TESTDRV_UI_REPORTER` (снапшот),
`D2TESTDRV_RELAY_BRIDGE` (командный мост), `D2TESTDRV_SKIP_INTRO` и `D2TESTDRV_BLACKSCREEN_FIX`
(boot-фиксы), `D2TESTDRV_ROLE` (ключ-роль у рилея). Чтобы запустить возможность вручную, задайте
те же переменные перед запуском `Discipl2.exe`.

## Настройка

1. Конфигурация. Скопируйте `test.config.sample.psd1` в `test.config.psd1` (он в gitignore) и задайте
   `GameDir` как путь к установке Disciples 2. Скрипты читают его, когда `-GameDir` не передан в командной
   строке, и при неверном пути сообщают, что исправить.
2. DLL. Соберите Debug-профиль test-harness `mss32.dll` (конфигурация `DebugTest`) и положите её в
   `GameDir`, рядом с `Discipl2.exe` и переименованной `Mss23.dll`. Для оптимизированного A/B-профиля
   соберите обычный `Release` с `/p:D2TestDrv=true` и разверните полученную DLL вместо DebugTest.
3. Node.js в `PATH`. Диспетчер запускает `relay.js`.

## Запуск теста

```powershell
.\scenario-generation.ps1                         # один экземпляр: проход по форме генератора
.\scenario-generation.ps1 -Template 7 -ToMap      # один экземпляр: сгенерировать и доиграть до стратегической карты
.\multiplayer-two-instance.ps1 -Kill              # хост и присоединяющийся доходят до стратегической карты
.\multiplayer-two-instance.ps1 -Kill -RandomMap -EndHostTurn -Scenario 7   # хост ГЕНЕРИРУЕТ карту, затем честный пропуск хода
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
Set-SpinOption    host $D SPIN_SIZE 0   # допустим только индекс, реально предоставленный шаблоном
Invoke-Button     host $D BTN_GENERATE
```

Тест проходит, когда диалог генератора открылся, каждый шаг навигации привёл к ожидаемому диалогу,
нужные виджеты присутствуют, и клиент остаётся на диалоге генератора после прохода по форме.

С `-WaitGenerationSec` тест также ждёт генерацию карты и проверяет, что дошло до
`DLG_GENERATION_RESULT`; с `-ToMap` он идёт дальше: принимает карту, запускает игру и доходит до
стратегической карты. Генератор падает по-разному, каждый случай сообщается отдельно: sol3-паника,
оставляющая клиента на форме, ассерт отладочного CRT (порча кучи в генераторе), который сборка
DebugTest пишет в лог и роняет на нём прогон вместо блокирующего диалога, месседж-бокс
(`DLG_MESSAGE_BOX`) после того как генератор сдался, или тайм-аут всё ещё на `DLG_WAIT_GENERATION`. Двух-инстансный
[`multiplayer-two-instance.ps1`](multiplayer-two-instance.ps1) с `-RandomMap` создаёт сессию из
сгенерированной карты вместо скирмиша, поэтому с `-EndHostTurn` он проходит полный честный сценарий:
сгенерировать, оба клиента доходят до карты, хост завершает ход, а присоединяющийся прокликивает свой
новый день.

## Команды

Из [`_relay.ps1`](_relay.ps1); подключите его через `.`. `<role>` это `host`, `join` и т. п., и
совпадает с `D2TESTDRV_ROLE` клиента.

| Команда | Действие | Endpoint |
|---|---|---|
| `Resolve-GameDir [$GameDir]` | папка игры, из `-GameDir` или конфига, с проверкой | нет |
| `Start-TestRelay` | запустить `relay.js`, вернуть процесс | нет |
| `Start-GameClient -GameDir <d> -Role <r>` | запустить клиент с любой harness DLL-сборкой | нет |
| `Get-Dialog <role>` | имя текущего диалога | `GET /api/state` |
| `Get-GameUi <role>` | `{ role, dialog, widgets }`, все виджеты с их состоянием | `GET /api/ui` |
| `Get-RoleState <role>` | состояние роли `{ dialog, widgets, connected, ... }` и липкие флаги | `GET /api/state` |
| `Wait-Dialog <role> <dlg> [sec]` | ждать, пока клиент окажется на `<dlg>` | нет |
| `Invoke-Button <role> <dlg> <btn>` | нажать кнопку | `POST /api/ui/invoke` |
| `Set-ListSelection <role> <dlg> <listbox> <i>` | выбрать элемент списка | `POST /api/ui/select` |
| `Set-SpinOption <role> <dlg> <spin> <i>` | задать опцию спин-кнопки | `POST /api/ui/spin` |
| `Set-EditText <role> <dlg> <edit> <text>` | задать текст поля ввода | `POST /api/ui/edit` |
| `Invoke-Toggle <role> <dlg> <toggle>` | переключить toggle-кнопку (например, автобой) | `POST /api/ui/toggle` |
| `Step-ToDialog <role> <dlg> <btn> <toDlg> [sec]` | нажимать `<btn>`, пока клиент не дойдёт до `<toDlg>` | нет |
| `Get-World <role>` | снапшот мира `{ day, strategicActionReady, players, stacks }` | `GET /api/world` |
| `Get-Resources <role>` | строка ресурсов локального игрока | `GET /api/world` |
| `Get-Stacks <role>` | все стеки на карте | `GET /api/world` |
| `Move-Stack <role> <id> <x> <y>` | переместить стек на клетку или на монстра для атаки | `POST /api/ui/move` |
| `Resolve-TemplateIndex <gameDir> <name>` | 0-базовый индекс шаблона генератора по его имени | нет |

Для интерактивной работы подключите [`../relay/drive-game-relay.ps1`](../relay/drive-game-relay.ps1):
он даёт те же команды плюс инспекторы только для чтения `Get-GameStatus`, `Get-GameUi`,
`Get-Dialog`, `Get-World`.

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
принято к выполнению, и `$false` иначе (неверное имя или диалог не открыт). Этот результат не доказывает,
что синхронный callback успешно завершился: тест должен наблюдать ожидаемый переход состояния. Рилей
держит запрос открытым, пока клиент не ответит, поэтому команда на отсутствующую цель сообщается, а не
теряется молча. `Step-ToDialog` применяет ограниченные повторы только к коалесцируемой навигации и
останавливается при появлении целевого диалога или по тайм-ауту. Семантические действия вроде принятия
сгенерированной карты отправляются один раз и проверяются по эффекту.

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

## Тестирование боя

Тест боя проводит весь сценарий «подойти к монстру и сразиться» теми же нативными путями, что и живой
игрок, без эмуляции ввода: он читает снапшот мира, шлёт по одной команде перемещения на шаг и проверяет
снапшот. [`attack-monster.ps1`](attack-monster.ps1) - канонический шаблон. Его шаги по порядку:

1. Выйти на карту. Сгенерировать случайный скирмиш (Single Player -> New Skirmish -> Random Map) и
   провести popup-ы первого хода до `DLG_STRATEGIC`/`DLG_ISO_PAL`, как делает `scenario-generation.ps1
   -ToMap`. Скирмиш - одиночный режим с последовательными ходами, поэтому локальный игрок и есть тот,
   чей ход.
2. Выйти из гарнизона. Стартовый герой сидит ВНУТРИ столицы, поэтому снапшот сообщает якорь форта, а
   не клетку героя, и первое действие - всегда бесплатный шаг выхода. Подайте его через
   `Move-Stack <role> <heroId> (cx+5) (cy+5)`, где `(cx,cy)` - сообщённая позиция якоря: клиент
   распознаёт стек в гарнизоне и воспроизводит точный 0-стоимостный выход игры. Не пробуйте и не
   повторяйте его: `DLG_BEGIN_TURN` первого дня может появиться после изо-вида, пока штатный admission
   хода/object-lock ещё закрыт. Подтвердите каждый известный стартовый диалог не более одного
   раза, затем дождитесь подключённой роли на текущей карте с истинными `sawBeginTurn` и
   `strategicActionReady`, заново проверьте героя и подайте единственную команду выхода. После этого
   дождитесь смены сообщённой позиции на реальную клетку и считайте её стартовой клеткой героя.
3. Выбрать цель. Из `Get-Stacks` возьмите ближайший стек, у которого `relation` равно `neutral`, а
   `inside` ложно: свободного нейтрального монстра. Фильтруйте именно по `neutral`, а не просто по
   не-`self`, чтобы бродячий стек вражеского ИИ-игрока никогда не был выбран как «монстр»; стеки с
   `inside` пропускайте - это гарнизоны (город, форт, деревня), сообщаемые в якоре форта и атакуемые
   как осада.
4. Снимок ДО. Запишите `x`/`y`, `units` и `hp` монстра и героя, а также находится ли монстр уже рядом
   с героем (расстояние Чебышёва 1 или меньше).
5. Атаковать. `Move-Stack <role> <heroId> <monX> <monY>` на клетку монстра. Клиент проводит героя
   вплотную и ставит `end` сообщения о перемещении в клетку монстра, поэтому сервер начинает бой
   ровно так же, как клик по вражескому стеку: открывается точный viewer `DLG_BATTLE_A` или
   `DLG_BATTLE_B`, и репортёр интерфейса его видит.
6. Автобой. `Invoke-Toggle` на `TOG_AUTOBATTLE` этого точного A/B viewer передаёт бой ИИ игры, который отыгрывает
   каждый раунд (это внутриигровой автобой, а не мгновенное разрешение), и бой завершается сам. Автобой
   - переключатель, а не кнопка, поэтому нужен `Invoke-Toggle`, а не `Invoke-Button`.
7. Закрыть послебоевые диалоги. За боем следует экран результата и нередко один или несколько диалогов
   награды или выпавших предметов. Нажимайте кнопку-вперёд (любую из `BTN_CLOSE`, `BTN_OK`,
   `BTN_TAKEALL`, `BTN_TAKE`, `BTN_CONTINUE`, `BTN_RIGHTSIDE`) на каждом так же, как ведут
   последовательность первого хода, пока не вернётся `DLG_STRATEGIC`/`DLG_ISO_PAL`. Не прожимайте вслепую
   общий `BTN_YES` здесь и не считайте окно боя (`DLG_BATTLE_A/B`) зависшим, пока оно ещё открыто (долгий
   автобой держит его открытым); остальное ограничьте сторожем «нет прогресса», чтобы нераспознанный
   диалог награды быстро падал, а не крутил весь тайм-аут.
8. Проверить, прочитав один чистый послебоевой снапшот, затем сравнив. Опрашивайте `Get-World`, пока
   GET не удастся, чтобы отсутствие стека означало реальное удаление, а не потерянный запрос; затем
   найдите героя и монстра по id. Прогон проходит, когда выполнены обе проверки:
   - Бой разрешился: монстра нет в переписи, ЛИБО его `hp` или `units` упали, ЛИБО `hp` или `units`
     героя упали, ЛИБО самого героя нет (проигранный бой уничтожил отряд). Кто-то обычно гибнет, но как
     минимум состоявшийся бой наносит урон, поэтому ключевой признак - `hp` отряда: одинокий лидер
     против монстра из двух юнитов может никого не убить, но всё равно покажет потерю ХП с обеих сторон.
   - Герой подошёл: если герой ещё существует, его позиция отличается от клетки после выхода.
     Единственное исключение - монстр, уже стоявший рядом на шаге 4, где шаг подхода не ожидается; если
     герой уничтожен, проверка подхода неприменима.

Ходы в настоящем бою всегда отличаются от стартовой позиции (проверка подхода); единственный случай,
когда нет, - монстр прямо вплотную к выходу из гарнизона, что почти не случается. Выдерживайте темп
цикла послебоевых диалогов как у любой последовательности диалогов: примерно одно нажатие в 0.7-2
секунды, запоминая последний диалог, чтобы не нажать один и тот же дважды до того, как он продвинется.

Этот сценарий боя вынесен в [`_battle.ps1`](_battle.ps1) (`Invoke-HeroAttack`) и переиспользуется
мультиплеерным тестом [`mp-attack-monsters.ps1`](mp-attack-monsters.ps1). CI копирует закреплённый в
репозитории `assets/mp-battle-regen.sg` в `Exports` одноразовой игры как единственный слот; генератор
карт и внешний loader не участвуют. Маленький детерминированный сценарий даёт каждому игроку свободного
бойца, отдельного ближнего нейтрала для короткого боя и запасные цели. Каждый игрок один раз атакует и завершает ход. После
смены дня тест логирует дневной инком и регенерацию каждого повреждённого выжившего. Реген зависит от
юнита и тайминга, поэтому печатается для наблюдения, а не жёстко гейтится; для строгого порога передайте
`-MinRegenPct 5`. Два клиента обмениваются реальными игровыми сообщениями по DirectPlay (начало хода,
перемещение стека, бой), так что прогон сквозь проверяет пас хода, очки движения, инком и боевое ХП.
Случайная генерация отдельно покрыта generated-map turn-pass job и полной матрицей шаблонов.

## Добавление теста

Создайте `tools/test/<имя>.ps1`. Возьмите `param([string]$GameDir, [switch]$Kill)`, подключите
`_relay.ps1`, вызовите `Resolve-GameDir`, `Start-TestRelay` и `Start-GameClient`, ведите клиента,
проверьте результат и остановите рилей и клиентов в блоке `finally`. Осиротевший рилей удерживает свой
именованный канал и блокирует следующий запуск. Завершайтесь кодом 1 при провале.

Двух-инстансный или иначе координированный тест работает целиком через рилей: отправить команду,
подождать, прочитать итоговое состояние, действовать. Минимальный одиночный тест может вместо этого
использовать встроенный self-nav (добавьте `SELFNAV` к флагам клиента; см. [`walk-menu.ps1`](walk-menu.ps1)).

## Добавление джоба в CI

CI находится в [`../../.github/workflows`](../../.github/workflows). `test-harness.yml` — единый
автоматический вход для каждого PR и релевантного push в `dev`/`master`. Он проверяет PowerShell/JS и
оба транспорта, собирает DebugTest и Release+D2_TESTDRV DLL, запускает полный gameplay-набор
`tests.yml` на DebugTest и независимо собирает plain production Release из точного checkout через
`mss32-production-build.yml`. `mss32.yml` оставлен ручным входом production Debug/Release, поэтому
второй автоматический PR-run не создаётся.

Строгая матрица checkout-ит `bartonsun/D2-Templates` (`master` по умолчанию), один раз фиксирует commit,
flatten-ит `sMNS/**/*.lua` в неизменяемый snapshot и записывает исходные пути, размеры и SHA-256. Каждый
файл получает отдельный job на DebugTest и оптимизированном Release+D2_TESTDRV с одним source snapshot,
валидными defaults шаблона и проверенным по digest Mesa asset. Каждый job пять раз последовательно
запускает реальный клиент игры с естественной случайностью самой игры. Попытки 1-4 должны дойти до
результата генератора; попытка 5 ещё принимает результат, запускает сценарий, по одному разу нажимает
точную enabled-кнопку каждого известного стартового диалога, наблюдает `DLG_BEGIN_TURN` и требует
заполненный self-world на готовой к действиям чистой стратегической карте. Готовая карта ещё три
секунды остаётся в кадре доказательного видео.

CRT assert, sol3 panic, error box, timeout, непринятая/неиграбельная карта, пропуск parser-ом,
отсутствующий outcome или обязательное видео краснят CI. `fail-fast:false` всё равно запускает каждый
файл в обоих профилях. Все пять outcome-строк и отдельные MSS/relay/OBS/console evidence-наборы хранятся
90 дней. Если прошли все пять попыток, остаётся только single-window MKV попытки 5; если есть падения —
только MKV упавших попыток. Поскольку DebugTest и Release выполняют независимые естественно-случайные
прогоны, различие их статусов информационное; отсутствующие/повреждённые строки или несовпадение source
manifest по-прежнему дают hard failure.

Чтобы добавить тест в CI, скопируйте один из тест-джобов в `tests.yml` (например,
`multiplayer-strategic`) и замените его последний шаг на запуск вашего скрипта с
`-GameDir "$env:GAME_DIR" -Kill`. Каждый тест-джоб скачивает артефакт `mss32-test-harness-debug`, восстанавливает
игру из кэша, разворачивает `mss32.dll` и запускает скрипт. Конфига в CI нет, поэтому `-GameDir`
передаётся явно, а `-Kill` обеспечивает уборку после прогона.

## Файлы

| Файл | Роль |
|---|---|
| `_relay.ps1` | тулкит: конфиг, рилей, клиенты и команды выше |
| `relay-transport-smoke.ps1` | smoke framed-протокола для named pipe и TCP: Hello/state/chat и задержанный command-result без повторной отправки |
| `_battle.ps1` | общий сценарий боя (`Invoke-HeroAttack`): взять требуемого свободного героя или вывести гарнизонного, атаковать ближайшего свободного нейтрала, автобой, закрытие диалогов, отчёт до/после |
| `_obs.ps1` | helper собственного portable OBS: две панели для multiplayer/static-arena или один host для template proof; обязательная CI-запись стартует до игры и краснит job при отсутствии MKV |
| `test.config.sample.psd1` | шаблон конфига машины; копируется в `test.config.psd1` |
| `scenario-generation.ps1` | одиночный пример генератора: прогон по форме, а с `-ToMap` доиграть сгенерированную карту до стратегического экрана |
| `list-templates.js` | перечисляет валидные шаблоны генератора в том же компактном числовом порядке, что игра; питает автоматическую матрицу всех шаблонов |
| `world-snapshot.ps1` | одиночный пример снапшота мира: выйти на карту и прочитать живое состояние мира (ресурсы игрока + стеки карты) |
| `move-hero.ps1` | одиночный пример перемещения: выйти из гарнизона и переместить героя с родной стоимостью пути игры, проверка через снапшот мира |
| `attack-monster.ps1` | одиночный шаблон боя: выход, подход к свободному монстру, атака, автобой, закрытие послебоевых диалогов и проверка по ХП / юнитам / позиции |
| `mp-attack-monsters.ps1` | мультиплеерный боевой тест на закреплённом `assets/mp-battle-regen.sg`: оба игрока атакуют своего ближнего нейтрала и пропускают ход, затем после смены дня проверяется регенерация повреждённого выжившего |
| `multiplayer-two-instance.ps1` | два клиента в начатую игру (скирмиш или с `-RandomMap` сгенерированная карта); `-EndHostTurn` добавляет честный пропуск хода |
| `reliability_test.ps1` | загрузка N раз до главного меню (бут-тест CI) |
| `walk-menu.ps1` | один self-nav клиент, оставленный запущенным для ручного осмотра |
| `lobby-create.ps1` | ручной интеграционный тест живого lobby; не запускается в CI и требует явных credentials |
| `luckytest-arena.ps1` | тест статической авторской арены: сундуки, лагеря, найм и раскладка отряда у обеих ролей; CI копирует закреплённый в репозитории `assets/luckytest-arena.sg`, без LuckyTest loader/reroller |
| `assets/*.sg` | обычные замороженные сценарии, один раз копируемые в одноразовую CI-игру; без runtime loader, reroller или генератора |
| `HIRE-MERC.md`, `SLOT-MANAGEMENT.md` | контракты и RE-заметки опционального LuckyTest action surface |
| `_show-window.ps1`, `_capture.ps1` | поднять окно и снять диагностический PNG |
| `../relay/relay.js` | рилей |
| `../relay/drive-game-relay.ps1` | интерактивная консоль над рилеем |
