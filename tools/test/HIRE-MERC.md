# Hiring a mercenary from a camp (test-harness agent guide)

How the harness hires a unit from a mercenary camp programmatically, with the change
replicating correctly in multiplayer. This documents the reverse-engineered call chain
behind `worldactions::hireMerc` and the method used to find it, which is the same method
for any other "drag in a stack/site UI" action (e.g. moving a unit between formation slots).

## TL;DR contract

| Operation | Agent call | Result |
| --- | --- | --- |
| Buy a merc into a stack | `POST /api/ui/hire?role=<r>&camp=<CAMP_ID>&stack=<STACK_ID>&unit=<UNIT_ID>` | `{found:bool}` (the hire message was sent) |

`camp` and `unit` come straight from the world snapshot: a `camps[]` entry's `.id` and one
of its `units[].impl`. `found:true` means the client message was sent; the host validates
(gold, slot) and is the final authority. Verify by re-reading `GET /api/world` on EITHER
role once the round trip settles: the stack's `units` goes up and the new unit appears in
`slots[]` on BOTH roles.

## The key insight (why the first approach was wrong)

The acting player (e.g. the joiner) is a network CLIENT; the host is the authoritative
SERVER. A merc hire is a CLIENT-to-SERVER request message; the server applies it and
broadcasts the result to everyone.

An earlier attempt called `VisitorApi::addUnitToGroup` directly from the client. That is the
SERVER's apply step, not the client's request: from a client it returns false, and even if
it edited the local object map it would NOT replicate. A stack-trace hook on
`addUnitToGroup` stayed silent during a real manual hire on the joiner, which is the proof:
the joiner SENDS, it does not APPLY. The fix is to send the same message the camp UI sends.

## The message: CSiteBuyUnitMsg

A merc-camp drag-drop (drop a merc onto a hero formation cell) builds and sends
`CSiteBuyUnitMsg` (Russobit vtable `0x6d516c`; extends `CSiteBuyItemMsg` `0x6d513c`). Layout
(size `0x18`):

```
+0x00  vftable (0x6d516c)
+0x04  CMidgardID siteId     // the mercenary camp (a CMidSiteMercs)
+0x08  CMidgardID stackId    // the hero group receiving the unit
+0x0C  CMidgardID unitId     // the merc roster entry being bought (a global unit impl id)
+0x10  int        = -1
+0x14  int        position   // target slot 0..5 (front cell for a big unit)
```

## The clean entry point

`CPhaseGame::sendSiteBuyUnitMsg` at Russobit `0x4067a2`:

```cpp
using SendSiteBuyUnitMsgFn = void(__thiscall*)(game::CPhaseGame*, const game::CMidgardID* siteId,
                                               const game::CMidgardID* stackId,
                                               const game::CMidgardID* unitId, int position);
reinterpret_cast<SendSiteBuyUnitMsgFn>(0x4067a2)(phaseGame, &campId, &stackId, &unitId, position);
```

It is gated by `clientTakesTurn` (only fires on the local player's own turn), builds the
message, and sends it via `data->midClient`. This is the exact call the drop handler makes,
so the harness reproduces a real drag without any UI emulation. `phaseGame` is the live
`CPhaseGame*` from `hooks::testdrv::livePhaseGame()` (the same one `moveStack` uses).

## The server apply (for reference, runs on the host)

`CSiteBuyUnitMsg` handler -> `sub_41EB68` (refs the string `"SiteBuyUnit"`) -> `sub_42F3A9`
-> `sub_5D8D93(objectMap, siteId, stackId, unitId, position)`:

1. casts `siteId` to `CMidSiteMercs` (the camp),
2. charges gold (cost derived from the unit's HP, `sub_5878BC`),
3. removes the merc from the camp roster (`sub_5E8FF1`),
4. `addUnitToGroup(unitId, stackId, position)` (`0x5e8bf8`) - generates the scenario unit
   from the global impl and places it,
5. broadcasts the result, so every client (the buyer and the host) converges.

So `unitId` is the camp roster's GLOBAL impl id (what `camps[].units[].impl` reports, e.g.
`G000UU8031`); the server generates the per-scenario instance (`S143UN....`). The harness
does NOT generate the unit or pick a leveled impl: it sends the roster id verbatim.

## The harness implementation

`worldactions::hireMerc(campId, stackId, unitId)`:

1. gate on `livePhaseGame()` + `clientTakesTurn` + the stack is ours (`ownerId ==
   localPlayerId()`),
2. pick the first fitting free slot, big-aware (a big merc needs a whole free column
   `{0,1}`/`{2,3}`/`{4,5}`; merc size read from its global impl), and send the FRONT cell
   for a big unit (the camp UI snaps a big drop to the front cell),
3. call `CPhaseGame::sendSiteBuyUnitMsg` with `(campId, stackId, unitId, freePos)`.

Wired exactly like `moveStack`: opcode `HireMerc = 0x0307` (`packetlogicbridge.cpp`), parsed
in `autonav.cpp` `onRemoteCommand` (three id strings), dispatched as `safeHireMerc`, exposed
as `POST /api/ui/hire` (`relay.js`) and `Hire-Merc` (`_relay.ps1`).

## Verification (done, live, MP)

Joiner walks onto a camp -> `DLG_MERCENARIES` opens -> `Hire-Merc` -> the host generates the
unit, places it, and broadcasts. Both roles then report `units=2` for the hero with the new
unit in slot 1 (identical `slots[]` on join and host). Confirmed against a manual GUI hire,
which produces the same result.

## The method (reuse this for the next site/stack UI action)

To find the client send behind any drag/drop action (e.g. moving a unit between formation
slots), in IDA on the Russobit `Discipl2.exe`:

1. Find the interface by its dialog string (e.g. `DLG_MERCENARIES`) or the visitor it ends in
   (`addUnitToGroup` callers map the SERVER apply side).
2. From the server apply, follow callers up to the message handler; the handler references a
   short ASCII tag (here `"SiteBuyUnit"`) that names the message class; confirm via the
   message vtable's RTTI (`CSiteBuyUnitMsg`).
3. Find the message constructor's caller on the CLIENT side: a `CPhaseGame` method that
   builds the message and sends it. Confirm `this` is a `CPhaseGame` by the `clientTakesTurn`
   gate `*(*(this+0x10)+0x28)` (`CPhaseGame::data` at `+0x10`, `clientTakesTurn` at
   `CPhaseGameData+0x28`).
4. Read the disasm to fix the argument order (push order into the message), then call the
   send directly with the live `CPhaseGame*`.
