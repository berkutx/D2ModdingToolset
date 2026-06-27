# Slot management and unit dismissal (test-harness agent guide)

How a harness agent inspects a hero's group slots and dismisses a unit to free a slot,
reusing existing mss32 facilities. Everything below maps to code that already exists in
the toolset; the only new pieces are one relay opcode end-to-end and a few extra JSON
fields on the world snapshot. Both mirror facilities that are already in the harness.

## TL;DR contract

| Operation | Agent call | Result |
| --- | --- | --- |
| Read slot occupancy | `GET /api/world` -> `stacks[].slots[]` + `leaderId` | per-slot `{position, unitId, isBig}` |
| Move/swap a unit (formation) | `POST /api/ui/move-unit?role=&stack=<ID>&src=<0..5>&dst=<0..5>` (`Move-GroupUnit`) | `{found:bool}` |
| Dismiss a unit (free a slot) | `POST /api/ui/dismiss?role=host&stackId=<ID>&unitId=<ID>` | `{found:bool}` |

The move/swap is DONE and live-verified (replicates): `worldactions::moveGroupUnit` sends the engine's
`CStackSwapUnitMsg` via `CPhaseGame::sendStackSwapUnitMsg` (Russobit `0x406cc7`), the host applies it
(`CVisitorSwapUnitPosition`) and broadcasts `CCmdUpdateObjMsg`. `src` (the moved unit) must hold a unit;
`dst` may be EMPTY (plain move, e.g. slide a just-hired unit down into a free slot) or OCCUPIED (swap,
e.g. put a ranged/caster on the back line and a melee up front). The moved unit's position goes in the
message's FIRST field (the one the engine requires occupied); the destination second (may be empty).
(RE chain + the method to find such client messages: `HIRE-MERC.md`.)

Backed entirely by mss32: read via `GroupView::getSlots()` / `UnitSlotView`; write via
`VisitorApi::extractUnitFromGroup(..., apply=1)` (the same call mss32 already uses in
`leadersforhirehooks.cpp:309`).

## The slot model

- A hero army is a `CMidStack` owning a `CMidUnitGroup` with `CMidgardID positions[6]`
  (6 slots, index 0..5). A city/fort garrison is the same: `CFortification` owns a
  `CMidUnitGroup` with the same 6-slot layout.
- Grid: `line = position % 2` (0 = front, 1 = back); `column = position / 2` (0..2).
  `UnitSlotView` gives `getLine()` / `getColumn()` / `isFrontline()` directly.
- A big (2-slot) unit occupies the two positions of one column, e.g. `{0,1}` or `{2,3}`
  or `{4,5}`. Detect with `UnitView.getImpl().isSmall() == false`.
- Empty slot: `unitId == emptyId`, unit pointer is null.
- The leader is one of the slot units. Never dismiss it (see Guardrails).

## READ: inspect slot occupancy

`GET /api/world` currently emits, per stack: `id, x, y, owner, relation, movement,
units (count), hp (sum), subrace, inside`. It loses per-slot detail because the reporter
calls `getUnits()` not `getSlots()`.

Add per-slot detail by extending the existing `forEachStack` lambda in
`mss32/src/testdrv/worldreporter.cpp` (~line 188, where it already holds a `StackView s`):

```cpp
// after the current stack fields, reuse the existing GroupView:
json += ",\"leaderId\":";  emit s.getLeader() ? its getId() : empty
json += ",\"slots\":[";
for (const auto& slot : s.getGroup().getSlots()) {        // GroupView::getSlots()
    if (slot.getUnitId() == game::emptyId) continue;       // skip empty slots
    auto unit = slot.getUnitView();                        // optional<UnitView>
    // emit { "position": slot.getPosition(),
    //        "unitId":  idToString(slot.getUnitId()),
    //        "isBig":   unit && !unit->getImpl().isSmall() }
}
json += ']';
```

Resulting per-stack JSON the agent reads:

```json
{ "id":"STACK_...", "owner":"PLAYER_...", "units":4, "leaderId":"UNIT_...",
  "slots":[ {"position":0,"unitId":"UNIT_A","isBig":false},
            {"position":2,"unitId":"UNIT_B","isBig":true} ] }
```

Free-slot count = `6 - sum(isBig ? 2 : 1 for occupied slots)`. A big hire fits only if a
whole column (`{0,1}`, `{2,3}` or `{4,5}`) is empty.

Scoring "the weakest unit" needs per-unit stats. The minimal `slots[]` above carries only
id/position/isBig; if the agent must rank occupants, also emit per-slot impl fields
(`UnitView` / `UnitImplView`: name, level, tier, xp) the same way, or keep the ranking
logic in C++/Lua where the bindings are richer.

## WRITE: dismiss a unit

Agent call (programmatic, no UI emulation):

```
POST http://127.0.0.1:8077/api/ui/dismiss?role=host&stackId=<STACK_ID>&unitId=<UNIT_ID>
-> { "role":"host", "dismiss":{"stackId":...,"unitId":...}, "found":true }
```

`found:true` means the dismiss was issued. Verify by re-reading `GET /api/world`: the slot
must now be empty and `units` decremented.

What backs it (reused from mss32):

```cpp
// host side, in the relay command handler:
const auto& v = game::VisitorApi::get();
// apply=0 first as a precheck, then apply=1 to commit:
if (v.extractUnitFromGroup(&unitId, &groupId, objectMap, /*apply*/1)) { ... }
```

- `groupId` = the id of the stack/group that owns the unit (for a city garrison, the
  fort's group id).
- `apply=0` = can-apply check only; `apply=1` = commit. Canonical usage already in the
  tree: `leadersforhirehooks.cpp:309`, `summonhooks.cpp`.

## Wiring the dismiss command (reuse-based, one opcode end to end)

Mirror the existing `MoveStack` / `InvokeToggle` path exactly:

1. `tools/relay/relay.js` - add `Op.DismissUnit` (next free opcode, e.g. `0x0307`) and a
   `POST /api/ui/dismiss` handler copied from the `/api/ui/move` block (~line 400):
   `sendCommand(sock, Op.DismissUnit, Buffer.concat([encodeStr(stackId), encodeStr(unitId)]))`.
2. `mss32/src/testdrv/packetlogicbridge.cpp` - add `Op::DismissUnit` to the enum
   (lines 43-59). Unknown opcodes are already forwarded to the command callback.
3. `mss32/src/testdrv/autonav.cpp` - in `onRemoteCommand` (line 276) parse the two strings
   with the existing `readStr` lambda into a `RemoteCmd{ type = 6, stackId, unitId }`; in
   `drainRemoteCommands` (line 364) add `else if (cmd.type == 6) safeDismissUnit(cmd);`.
4. `mss32/src/testdrv/worldactions.cpp` - add `dismissUnit(stackId, unitId)` modeled on
   `moveStack` (lines 114-319): resolve ids from the object map, validate ownership, call
   `VisitorApi::extractUnitFromGroup(apply=1)`. Wrap in `safeDismissUnit` with the same
   `__try/__except` + `reportFound(seq, ok)` as `safeMoveStack` (line 353).

## MP correctness

- Issue the change through the host's server-logic / visitor with `apply=1`, never a raw
  `objectMap` edit. The visitor is what mss32 itself uses for this mutation.
- VERIFY REPLICATION before trusting it in multiplayer: after a dismiss, read the JOINER's
  `/api/world` too and confirm the slot is empty there. The in-game Dismiss button drives
  the `StackDismissUnit` command (`CStackDismissUnitMsg`), which the server processes and
  broadcasts. If a bare `extractUnitFromGroup(apply=1)` from the relay handler does NOT
  reach the joiner, route it as the real `StackDismissUnit` command instead (the
  server-command path the UI uses) so the server replicates it. Do not assume replication;
  test it.
- Ownership: only the owner may dismiss. Validate `stack->ownerId == localPlayerId()`
  before issuing. The harness runs the host, so dismiss the host's own units only.
- Timing: a turn-phase action. Do it during the host's turn (after BeginTurn, before
  EndTurn), like the other slot/hire operations.

## Recipes

Free a slot for a 2-slot (big) hire:

1. `GET /api/world`, find the target stack `slots[]`.
2. A big unit needs a fully empty column (`{0,1}`, `{2,3}` or `{4,5}`).
3. If none is free, pick the weakest non-leader occupant, `POST /api/ui/dismiss` it,
   re-read, repeat until a column opens.
4. Proceed with the hire (hire-list hooks / camp interface).

Replace a weak unit with a better hire:

1. `/api/world` slots -> choose the weakest non-leader `unitId`.
2. `/api/ui/dismiss` it.
3. Hire/add the better unit into the freed slot.

## Guardrails

- Never dismiss the LEADER. Compare each candidate against `leaderId`; dismissing a leader
  is a different command (`StackDismissLeader`) and disbands the whole stack.
- Re-read `/api/world` after every dismiss to confirm the slot is free before the next
  action. Fail loud if `found:false` or the slot did not clear; do not silently continue.
- Prefer the programmatic `/api/ui/dismiss` over driving the real `TOG_DISMISS` UI button:
  the UI path needs the unit panel open and pops a confirmation dialog (the project rule is
  programmatic state, not input emulation).
- Host dismisses only its own units; never act across players.

## Reuse map (mss32 symbol behind each operation)

| Operation | Agent surface | mss32 reuse | Location |
| --- | --- | --- | --- |
| read per-slot occupancy | `stacks[].slots[]` | `GroupView::getSlots()` -> `UnitSlotView` | `include/bindings/groupview.h:53`, `unitslotview.h:46-55` |
| big-unit flag | `slots[].isBig` | `UnitView.getImpl().isSmall()` (inverse) | `include/bindings/unitimplview.h` |
| leader id | `leaderId` | `StackView::getLeader()` | `include/bindings/stackview.h` |
| reporter emit | (extend lambda) | `forEachStack` already holds `StackView` | `src/testdrv/worldreporter.cpp:188-226` |
| dismiss (execute) | `POST /api/ui/dismiss` | `VisitorApi::extractUnitFromGroup(apply=1)` | `include/visitors.h:191`; example `src/leadersforhirehooks.cpp:309` |
| relay endpoint | `POST /api/ui/dismiss` | mirror `/api/ui/move` | `tools/relay/relay.js:400` |
| opcode | `Op.DismissUnit` | mirror `MoveStack`/`InvokeToggle` | `src/testdrv/packetlogicbridge.cpp:43-59` |
| command drain | `cmd.type==6 -> safeDismissUnit` | mirror `safeMoveStack` | `src/testdrv/autonav.cpp:353,364-392` |
| ownership + issue | `dismissUnit()` | mirror `worldactions::moveStack` | `src/testdrv/worldactions.cpp:114-319` |

Related (already present, used alongside slot management): item transfer between stacks
`CMidServerLogicApi::stackExchangeItem`; native move `CPhaseGameApi::sendStackMoveMsg`
(via `/api/ui/move`); hire-list customization hooks (`unitsforhirehooks.cpp`,
`leadersforhirehooks.cpp`).
