# D2MSS simultaneous-turn relay (protocol v8)

In this branch this is the **local reference coordinator and regression suite**.
The production DLL uses the authenticated lobby adapter documented in
[the lobby contract](../../docs/SIMULTANEOUS_TURNS_LOBBY_PROTOCOL.md), not this pipe.
The commands below run the reference process; they do not attach this branch's
DLL to it. The universal live harness and its local/lobby adapters are a separate
follow-up PR. Historical live test results do not verify the new lobby transport.

This directory contains the dependency-free local coordinator for the D2MSS
simultaneous-turn proof of concept. It coordinates exactly one host process and
one join process. It does **not** proxy DirectPlay traffic, replace the game
session, or integrate with the public lobby server.

The relay is the authority for the session epoch, merge day, each player's
subjective day, turn leases, and engine-action transactions. Native clients
report observable engine events and execute explicitly typed actions; they do
not tell the relay what day they have reached.

The coordinator is deliberately independent from `tools/relay`, `testdrv`, and
the old lobby dashboard. Removing `tools/simturns-relay` removes this process
and its tests without entangling those components. The removable DebugTest
UI-command bridge also keeps its own transport, configuration, and wire
contract.

## Preparing a future lobby adapter

The local relay remains the runnable reference and the target of its
unit and pipe-integration tests (and the donor branch's live OH tests). This separation does not move any
code into the lobby or change the v8 protocol:

| Module | Owns | Must not acquire |
|---|---|---|
| [coordinator.js](src/coordinator.js) | One session's days, leases, actions, bootstrap and merge state | Socket or filesystem lifecycle, test UI commands |
| [protocol.js](src/protocol.js) | v8 messages, validation and byte codecs | Session state or transport ownership |
| [framed-connection.js](src/framed-connection.js) | One socket's decoder, writes and close notifications | Turn decisions, retries or another action queue |
| [server.js](src/server.js) | Listener, sockets, coordinator instance, local release-file control | A second implementation of the state machine |

A future lobby transport can provide the coordinator's existing
`{id, send(op, payload), close()}` connection contract instead of the local
byte-stream adapter. Keep sends and inbound dispatch synchronous: accepting a
write is not an engine acknowledgement, `socket.write(false)` is backpressure
and must not trigger a resend, and a closed connection must stop the rest of
its decoded batch. Preserve the existing fan-out order and terminal errors.

The local `Hello` PID/role hint is not network authentication. Session and role
authorization belong to that future lobby adapter, not to test-harness UI
commands. The release-file watcher remains local test control; it is not a
lobby readiness protocol. No lobby server adapter is implemented in this directory.

## Failure rule

Protocol v8 has no recovery, retry, reset, reconnect, resend, or fallback to
stock turns. Any failed or timed-out causal stage is terminal:

1. the coordinator enters `Faulted`;
2. it sends `Error` to every reachable peer;
3. it clears pending and queued work;
4. it sends no later gameplay, bootstrap, or merge action;
5. later end-turn evidence cannot advance the session.

A client that sees `Error`, EOF, or a broken pipe must close the simultaneous-
turn mutation gates and surface the failure. Coordinator loss is never
permission to resume stock progression.

## Requirements and launch

- Node.js 20 or newer;
- no `npm install` and no third-party packages;
- on Windows, the default endpoint is `\\.\pipe\d2mss.simturns.v8`;
- the default merge day is `0`, which disables the merge barrier.

From this directory:

```powershell
.\start.ps1
```

Select a server-owned merge day with either launcher. An enabled merge day
must be in the native engine-day range 2..2147483647:

```powershell
.\start.ps1 -MergeDay 12

node .\src\cli.js --merge-day 12
```

`-MergeDay 0` and `--merge-day 0` disable merging. Clients do not announce or
negotiate this value; they receive it in `SessionPlan`.

Override the pipe for an isolated run:

```powershell
$env:D2MSS_SIMTURNS_PIPE = '\\.\pipe\d2mss.simturns.dev1'
.\start.ps1 -MergeDay 12
```

`-PipeName` takes precedence over the environment. `-LogDirectory` changes the
file-log directory. The launcher validates Node and the pipe name, runs in the
foreground, and tees newline-delimited JSON logs to a unique file under
`logs`. It never kills another relay or removes a stale endpoint, so a second
instance on the same pipe fails visibly.

Direct launch is also supported and logs JSON to stdout:

```powershell
node .\src\cli.js `
  --pipe '\\.\pipe\d2mss.simturns.dev1' `
  --merge-day 12
```

Run all unit and named-pipe integration tests with:

```powershell
node --test
```

## Removable bootstrap-harness controls

The test harness can add two cold-start ordering controls without changing the
production protocol:

```powershell
node .\src\cli.js `
  --pipe '\\.\pipe\d2mss.simturns.dev1' `
  --merge-day 12 `
  --bootstrap-release-file 'C:\absolute\run\bootstrap.release' `
  --bootstrap-cascade-delay-ms 500
```

The PowerShell launcher exposes the same options as
`-BootstrapReleaseFile` and `-BootstrapCascadeDelayMs`.

`--bootstrap-release-file` must be an absolute path that does not exist when
the relay starts. The relay installs one directory watcher and validates that
initial absence before listening. Creating a regular file at that path after
both exact player handles have arrived irreversibly calls the coordinator's
one `releaseBootstrap()` operation. Until then, neither client receives a
`SessionPlan`. Creating the file too early faults the session; a pre-existing
path prevents startup. The watcher is consumed after its first event and is
closed during shutdown.

For `--bootstrap-cascade-delay-ms N`, where `N > 0`, one timer is armed exactly
once by the join's exact `BootstrapBeginTurnApplied`. At its fixed deadline,
both clients must already have acknowledged `SessionActivated`; otherwise the
session faults. The timer is cleared on fault or close and is never restarted,
retried, or caught up. The production default is `0`, which dispatches the same
bootstrap action immediately after all prerequisites are present.

## Frame format

Every integer is unsigned and little-endian. A frame is:

| Offset | Size | Field |
|---:|---:|---|
| `0` | 4 | `length`: bytes after this field (`4 + payloadSize`) |
| `4` | 2 | `op` |
| `6` | 2 | `flags`, must be zero |
| `8` | variable | payload |

The maximum accepted `length` is 64 KiB. A value smaller than 4, a value above
the limit, non-zero flags, the wrong payload size, an unknown enum, or an
opcode invalid for the sender/state is a protocol error.

All structured payload fields below are consecutive `u32` values. `Error` is
the only exception and carries at most 4096 bytes of UTF-8 text.

| Op | Name | Direction | Payload |
|---:|---|---|---|
| `0x0001` | `Hello` | client -> relay | `{version, pid, roleHint}` |
| `0x0002` | `HelloAck` | relay -> client | `{accepted, version}` |
| `0x0003` | `Goodbye` | client -> relay | empty |
| `0x0007` | `LocalPlayerHandle` | client -> relay | `{handle}` |
| `0x0008` | `SessionPlan` | relay -> host, then join | `{epoch, mode, hostHandle, joinHandle, mergeDay, hostLease, joinLease}` |
| `0x000b` | `SessionActivated` | each client -> relay | empty |
| `0x000c` | `BootstrapBeginTurnApplied` | join -> relay | `{joinHandle, day=1}` |
| `0x000d` | `BootstrapComplete` | join -> relay | `{joinHandle, day=1}` |
| `0x000e` | `BootstrapCommitted` | relay -> join, then host | `{joinHandle, day=1}` |
| `0x000f` | `BootstrapCommitApplied` | each client -> relay | `{joinHandle, day=1}` |
| `0x0010` | `BootstrapOperational` | relay -> host, then join | `{joinHandle, day=1}` |
| `0x0011` | `BootstrapOperationalApplied` | each client -> relay | `{joinHandle, day=1}` |
| `0x0015` | `BootstrapReleased` | relay -> host, then join | `{joinHandle, day=1}` |
| `0x1000` | `EndTurnObserved` | origin client -> relay | `{epoch, lease}` |
| `0x1001` | `EngineAction` | relay -> executing client(s) | `{epoch, actionId, kind, playerHandle, day, lease}` |
| `0x1002` | `ActionResult` | executing client -> relay | `{epoch, actionId, kind, success}` |
| `0x1007` | `EndTurnApplied` | host -> relay | `{epoch, lease}` |
| `0x1008` | `MergeApplied` | each client -> relay | `{epoch, actionId}` |
| `0x10ff` | `Error` | relay -> client | UTF-8 bytes |

`Hello.version` and `HelloAck.version` are 8. `roleHint=0` lets the relay assign
the first available role; `roleHint=1` requests host and `roleHint=2` requests
join. A role hint is not authority over session settings. In particular,
`Hello` contains no merge day.

`SessionPlan.mode=0` explicitly selects stock turns and requires `mergeDay=0`;
`mode=1` selects simultaneous turns. The current coordinator emits mode 1. Its
`epoch` is a non-zero value owned by this relay process. Player handles are
non-zero and distinct. Simultaneous plans carry two non-zero distinct leases;
stock plans carry two zero leases. `mergeDay` is either 0 or in the inclusive
range 2..2147483647. Although day fields are encoded as `u32`, every
engine-facing day is capped at `INT32_MAX` because the native game stores it
as a signed 32-bit integer.
`HelloAck.accepted=1` means only that the version, PID, and role slot were
accepted; it does not authorize gameplay.

Engine action kinds are:

| Kind | Name | Meaning |
|---:|---|---|
| `1` | `ApplyTurnStart` | run the host-side native day-start cascade |
| `2` | `ActivateTurn` | apply the new subjective turn and lease at the origin client |
| `3` | `HoldInput` | close the arriving client's local input at the merge barrier |
| `4` | `PrepareMerge` | prepare each client for the authoritative merge transaction |
| `5` | `ExecuteMerge` | execute the merge once on the host |
| `6` | `ReleaseStock` | reopen stock progression after all merge evidence exists |

An action result is identified by the tuple `(kind, actionId)`, scoped by
`epoch`. The same `actionId` may deliberately appear with different kinds in
one transaction. A result with the wrong epoch, action ID, kind, sender, or
success encoding is terminal once the session has started.

The `lease` field is semantic, not optional padding: `ApplyTurnStart` and
`ActivateTurn` require the same non-zero newly allocated turn grant. `HoldInput`,
`PrepareMerge`, `ExecuteMerge`, and `ReleaseStock` require `lease=0`.

## Bootstrap sequence

After one host, one join, and two distinct non-zero local player handles are
present, the relay creates one immutable `SessionPlan`. Its staged bootstrap is
a causal one-shot chain:

1. the relay sends `SessionPlan` to host (immediately in production or after
   the optional harness release file);
2. host applies the plan and sends `SessionActivated`;
3. only that acknowledgement releases the same `SessionPlan` to join;
4. join applies the plan, sends `SessionActivated`, injects its required local
   day-1 activation, and reports `BootstrapBeginTurnApplied(join, 1)`;
5. after both activations and that join edge, the relay sends the host one
   `EngineAction(ApplyTurnStart, join, day=1, joinLease)`; the optional harness
   delay is anchored here;
6. host returns the exact successful `ActionResult(ApplyTurnStart, actionId)`;
7. join observes the resulting natural turn state and sends
   `BootstrapComplete(join, 1)`; steps 6 and 7 may arrive in either order;
8. relay sends `BootstrapCommitted(join, 1)` to join and then host, and waits
   for both `BootstrapCommitApplied` acknowledgements;
9. relay sends `BootstrapOperational(join, 1)` to host and then join, and waits
   for both `BootstrapOperationalApplied` acknowledgements;
10. relay first enters `Ready`, then sends the unacknowledged
    `BootstrapReleased(join, 1)` to host and then join.

The relay does not use a production wall-clock deadline while waiting for
human/modal-dependent bootstrap observations. The host engine action has its
30-second request timeout. Each dispatched engine action owns a fresh one-shot
30-second deadline; expiry faults the session and the action is never retried.
Once the committed or operational machine-acknowledgement
fan-out starts, each stage has its own bootstrap deadline. No step is reissued.

## Server-owned turns and leases

Each peer starts at relay-owned `currentDay=1` with the lease published in
`SessionPlan`. The relay keeps the two subjective days independently; one
player may advance while the other is still on an earlier day. A lease is a
one-shot capability for exactly one end-turn transaction.

Admission requires two facts with the same `{epoch, lease}`:

1. the origin sends `EndTurnObserved` only after its original DirectPlay send
   succeeds;
2. the host sends `EndTurnApplied` only after the original host receive
   dispatcher succeeds;
3. either fact may arrive first, but the first one starts a terminal watchdog;
4. only the exact pair consumes the lease and enters the relay's global turn
   queue.

By default, the first fact also starts an independent one-shot 12-second
evidence watchdog. Its expiry is terminal: later evidence cannot revive the
session and no fact or engine action is resent. This evidence deadline does not
inherit the longer engine-action timeout.

`EndTurnApplied` does not carry a player or day: the globally unique current
lease identifies the origin. Clients likewise never send a completed or next
day. The relay reads the origin's `currentDay`, computes `nextDay=currentDay+1`,
and serializes all accepted work so two simultaneous clicks cannot create two
concurrent native cascades. Attempting to advance beyond day 2147483647 is a
terminal session fault; the relay never emits an out-of-domain `EngineAction`.

For an ordinary turn, one `actionId` spans the complete transaction:

1. relay allocates the origin's next lease and sends host
   `EngineAction(ApplyTurnStart, origin, nextDay, nextLease)`;
2. relay waits for host `ActionResult(ApplyTurnStart, actionId, success=1)`;
3. relay sends origin `EngineAction(ActivateTurn, origin, nextDay, nextLease)`
   with the same
   `actionId`;
4. only after origin returns
   `ActionResult(ActivateTurn, actionId, success=1)` does the relay commit
   `currentDay=nextDay`, publish the new current lease internally, clear the
   player's in-flight state, and start later queued work.

Thus a successful host cascade alone never advances authoritative relay state
or permits another end turn. Any duplicate evidence, stale lease, failed
action, mismatched action identity, or timeout faults the session.

## Merge barrier and transaction

For enabled `mergeDay=N`, only the strict condition `nextDay == N` enters the
barrier. `nextDay > N` is a terminal consistency error. Arrival does **not**
advance the player's authoritative day: `currentDay` remains `N-1`.

Each arriver gets its own fresh-action transaction:

1. relay sends that same local client
   `EngineAction(HoldInput, arrivingHandle, day=N-1, lease=0)`;
2. `playerHandle` is deliberately the arriving/local player, not the other
   player;
3. only the exact successful `ActionResult(HoldInput, actionId)` marks that
   peer held and lets the relay process the other peer's queued arrival.

The merge begins only when both players are at exactly `N-1`, both barrier
leases have been consumed, both `HoldInput` actions have succeeded, and no
ordinary action remains. One shared `actionId` then spans the entire merge
transaction:

1. relay sends identical `EngineAction(PrepareMerge, hostHandle, N)` to join
   and then host;
2. relay waits for successful `ActionResult(PrepareMerge, actionId)` from both;
3. relay sends host `EngineAction(ExecuteMerge, hostHandle, N)` with the same
   `actionId`;
4. host returns successful `ActionResult(ExecuteMerge, actionId)` after its
   native merge call completes;
5. host and join each send `MergeApplied(epoch, actionId)` after observing the
   exact natural merged turn at their UI/engine seam; the execute result and
   the two applied notifications may arrive in any order;
6. only after all three facts exist does the relay first enter `Merged` and
   send `EngineAction(ReleaseStock, hostHandle, N)` with the same `actionId` to
   join and then host.

`ReleaseStock` has no `ActionResult` and is never retried. The join-before-host
order is intentional. Inter-process final fan-out cannot be atomic: if one
write succeeds and the other fails, the coordinator faults the already-claimed
merged state and sends `Error` to every reachable peer. Clients must close
their local gates on that terminal error; the protocol promises no distributed
rollback of engine work already applied.

## Native-client contract

The D2MSS native module is responsible for:

- enabling this path only behind the simultaneous-turn feature toggle;
- connecting with `Hello`, publishing `LocalPlayerHandle`, validating the
  immutable `SessionPlan`, and completing bootstrap before accepting end turns;
- treating the relay's epoch, merge day, days, leases, and actions as
  authoritative for this session;
- emitting the two end-turn facts only at their exact post-original DirectPlay
  boundaries;
- executing every `EngineAction` on the correct game/UI thread and returning
  the exact `ActionResult` where required;
- reporting `MergeApplied` only after the shared execute action's natural
  merged turn has actually reached the corresponding local engine seam;
- keeping gameplay gates closed until `BootstrapReleased` or `ReleaseStock` as
  applicable;
- failing closed on `Error`, EOF, socket failure, write failure, bad payload,
  unexpected opcode, stale epoch/lease, or impossible local state;
- never interpreting silence, disconnect, or timeout as permission to advance.

The relay owns ordering and authority; the client owns safe application of
engine mutations. Neither side substitutes timing assumptions for explicit
causal evidence.

## Logging

Every relay process creates a UUID `instanceId` and a random non-zero `epoch`.
Every JSON record includes the instance ID, UTC timestamp, and event name. The
startup record also includes the selected `mergeDay`. Useful bootstrap markers
include `session-plan-created`, ordered `session-plan-delivered` records, and
`session-operational`; turn and merge actions include their typed `actionId`.
