import assert from 'node:assert/strict';
import test from 'node:test';
import {
  EngineActionKind,
  MAX_ENGINE_DAY,
  Op,
  PROTOCOL_VERSION,
  RoleHint,
  SessionMode,
  decodeActionResult,
  decodeBootstrapProgress,
  decodeEngineAction,
  decodeError,
  decodeHelloAck,
  decodeSessionPlan,
  encodeActionResult,
  encodeBootstrapProgress,
  encodeEndTurnApplied,
  encodeEndTurnObserved,
  encodeHello,
  encodeLocalPlayerHandle,
  encodeMergeApplied,
} from '../src/protocol.js';
import {
  DEFAULT_CASCADE_TIMEOUT_MS,
  DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS,
  DEFAULT_MERGE_APPLY_TIMEOUT_MS,
  MAX_ONE_SHOT_DELAY_MS,
  SessionPhase,
  SimTurnsCoordinator,
} from '../src/coordinator.js';

const EPOCH = 0x10203040;
const HOST_HANDLE = 0xa3de0001;
const JOIN_HANDLE = 0xa3de0002;

class FakeConnection {
  constructor(id, events = []) {
    this.id = id;
    this.events = events;
    this.sent = [];
    this.closed = false;
    this.failOnOp = null;
    this.onSend = null;
  }

  send(op, payload) {
    if (this.failOnOp === op) throw new Error(`injected ${this.id} send failure`);
    const message = {op, payload: Buffer.from(payload)};
    this.sent.push(message);
    this.events.push({connection: this.id, ...message});
    this.onSend?.(message);
  }

  close() {
    this.closed = true;
  }

  messages(op) {
    return this.sent.filter((message) => message.op === op);
  }

  last(op) {
    return this.messages(op).at(-1);
  }
}

function deliver(coordinator, connection, op, payload = Buffer.alloc(0), flags = 0) {
  coordinator.handleFrame(connection, {op, flags, payload});
}

function connectPeer(coordinator, id, pid, roleHint, events = []) {
  const connection = new FakeConnection(id, events);
  coordinator.registerConnection(connection);
  deliver(coordinator, connection, Op.Hello, encodeHello({pid, roleHint}));
  assert.deepEqual(decodeHelloAck(connection.last(Op.HelloAck).payload), {
    accepted: 1,
    version: PROTOCOL_VERSION,
  });
  return connection;
}

function actionResult(coordinator, connection, action, success = true) {
  deliver(
    coordinator,
    connection,
    Op.ActionResult,
    encodeActionResult({
      epoch: action.epoch,
      actionId: action.actionId,
      kind: action.kind,
      success,
    }),
  );
}

function latestAction(connection) {
  return decodeEngineAction(connection.last(Op.EngineAction).payload);
}

function rawProgress(handle, day) {
  const payload = Buffer.alloc(8);
  payload.writeUInt32LE(handle, 0);
  payload.writeUInt32LE(day, 4);
  return payload;
}

function dispatchBootstrap(session) {
  deliver(session.coordinator, session.host, Op.SessionActivated);
  deliver(session.coordinator, session.join, Op.SessionActivated);
  deliver(
    session.coordinator,
    session.join,
    Op.BootstrapBeginTurnApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  const action = latestAction(session.host);
  assert.equal(action.kind, EngineActionKind.ApplyTurnStart);
  return action;
}

function reachBootstrapCommit(session, {completeFirst = false} = {}) {
  const action = dispatchBootstrap(session);
  if (completeFirst) {
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapComplete,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    actionResult(session.coordinator, session.host, action);
  } else {
    actionResult(session.coordinator, session.host, action);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapComplete,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
  }
  return action;
}

function reachBootstrapOperational(session) {
  reachBootstrapCommit(session);
  deliver(
    session.coordinator,
    session.host,
    Op.BootstrapCommitApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  deliver(
    session.coordinator,
    session.join,
    Op.BootstrapCommitApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
}

function completeBootstrap(
  coordinator,
  host,
  join,
  {completeFirst = false, commitOrder = [host, join], operationalOrder = [join, host]} = {},
) {
  deliver(coordinator, host, Op.SessionActivated);
  deliver(coordinator, join, Op.SessionActivated);
  deliver(
    coordinator,
    join,
    Op.BootstrapBeginTurnApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );

  const bootstrapAction = latestAction(host);
  assert.deepEqual(
    {
      epoch: bootstrapAction.epoch,
      kind: bootstrapAction.kind,
      playerHandle: bootstrapAction.playerHandle,
      day: bootstrapAction.day,
    },
    {
      epoch: EPOCH,
      kind: EngineActionKind.ApplyTurnStart,
      playerHandle: JOIN_HANDLE,
      day: 1,
    },
  );
  if (completeFirst) {
    deliver(
      coordinator,
      join,
      Op.BootstrapComplete,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    actionResult(coordinator, host, bootstrapAction);
  } else {
    actionResult(coordinator, host, bootstrapAction);
    deliver(
      coordinator,
      join,
      Op.BootstrapComplete,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
  }

  for (const peer of commitOrder) {
    deliver(
      coordinator,
      peer,
      Op.BootstrapCommitApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
  }
  for (const peer of operationalOrder) {
    deliver(
      coordinator,
      peer,
      Op.BootstrapOperationalApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
  }
  assert.equal(coordinator.snapshot().phase, SessionPhase.Ready);
  assert.equal(coordinator.snapshot().ready, true);
  return bootstrapAction;
}

function plannedSession({mergeDay = 0, coordinatorOptions = {}} = {}) {
  const events = [];
  const coordinator = new SimTurnsCoordinator({
    epoch: EPOCH,
    mergeDay,
    ...coordinatorOptions,
  });
  const host = connectPeer(
    coordinator,
    'host',
    101,
    RoleHint.Host,
    events,
  );
  const join = connectPeer(
    coordinator,
    'join',
    202,
    RoleHint.Join,
    events,
  );
  deliver(
    coordinator,
    host,
    Op.LocalPlayerHandle,
    encodeLocalPlayerHandle(HOST_HANDLE),
  );
  deliver(
    coordinator,
    join,
    Op.LocalPlayerHandle,
    encodeLocalPlayerHandle(JOIN_HANDLE),
  );

  assert.equal(host.messages(Op.SessionPlan).length, 1);
  assert.equal(join.messages(Op.SessionPlan).length, 0);
  const plan = decodeSessionPlan(host.last(Op.SessionPlan).payload);
  assert.deepEqual(plan, {
    epoch: EPOCH,
    mode: SessionMode.Simultaneous,
    hostHandle: HOST_HANDLE,
    joinHandle: JOIN_HANDLE,
    mergeDay,
    hostLease: 1,
    joinLease: 2,
  });
  return {coordinator, host, join, events, plan};
}

function readySession(options = {}) {
  const session = plannedSession(options);
  completeBootstrap(session.coordinator, session.host, session.join);
  return session;
}

function endTurnEvidence(session, role, order = ['observed', 'applied']) {
  const origin = role === 'host' ? session.host : session.join;
  const lease = session.coordinator.snapshot().peers[role].lease;
  for (const kind of order) {
    if (kind === 'observed') {
      deliver(
        session.coordinator,
        origin,
        Op.EndTurnObserved,
        encodeEndTurnObserved({epoch: EPOCH, lease}),
      );
    } else {
      deliver(
        session.coordinator,
        session.host,
        Op.EndTurnApplied,
        encodeEndTurnApplied({epoch: EPOCH, lease}),
      );
    }
  }
  return lease;
}

function finishOrdinaryTurn(session, role, order) {
  const origin = role === 'host' ? session.host : session.join;
  const oldLease = endTurnEvidence(session, role, order);
  const apply = latestAction(session.host);
  assert.equal(apply.kind, EngineActionKind.ApplyTurnStart);
  assert.equal(apply.playerHandle, role === 'host' ? HOST_HANDLE : JOIN_HANDLE);
  assert.notEqual(apply.lease, oldLease);
  actionResult(session.coordinator, session.host, apply);
  const activate = latestAction(origin);
  assert.equal(activate.kind, EngineActionKind.ActivateTurn);
  assert.equal(activate.actionId, apply.actionId);
  assert.equal(activate.lease, apply.lease);
  actionResult(session.coordinator, origin, activate);
  return {apply, activate, oldLease};
}

function arriveAtBarrier(session, role, order) {
  const origin = role === 'host' ? session.host : session.join;
  const lease = endTurnEvidence(session, role, order);
  const hold = latestAction(origin);
  assert.deepEqual(
    {
      kind: hold.kind,
      playerHandle: hold.playerHandle,
      day: hold.day,
      lease: hold.lease,
    },
    {
      kind: EngineActionKind.HoldInput,
      playerHandle: role === 'host' ? HOST_HANDLE : JOIN_HANDLE,
      day: 1,
      lease: 0,
    },
  );
  actionResult(session.coordinator, origin, hold);
  return hold;
}

function preparingMergeSession(coordinatorOptions = {}) {
  const session = readySession({mergeDay: 2, coordinatorOptions});
  arriveAtBarrier(session, 'host');
  arriveAtBarrier(session, 'join');
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.PreparingMerge);
  return session;
}

function executingMergeSession(coordinatorOptions = {}) {
  const session = preparingMergeSession(coordinatorOptions);
  const hostPrepare = latestAction(session.host);
  const joinPrepare = latestAction(session.join);
  actionResult(session.coordinator, session.host, hostPrepare);
  actionResult(session.coordinator, session.join, joinPrepare);
  const execute = latestAction(session.host);
  assert.equal(execute.kind, EngineActionKind.ExecuteMerge);
  return {...session, execute};
}

test('constructor validates server-owned epoch, merge day, and timers', () => {
  const defaultCoordinator = new SimTurnsCoordinator();
  assert.equal(DEFAULT_CASCADE_TIMEOUT_MS, 30_000);
  assert.equal(DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS, 12_000);
  assert.equal(defaultCoordinator.cascadeTimeoutMs, 30_000);
  assert.equal(defaultCoordinator.endTurnSignalTimeoutMs, 12_000);
  const customActionDeadline = new SimTurnsCoordinator({cascadeTimeoutMs: 1234});
  assert.equal(customActionDeadline.cascadeTimeoutMs, 1234);
  assert.equal(customActionDeadline.endTurnSignalTimeoutMs, 12_000);
  for (const epoch of [0, -1, 0x100000000, 1.5]) {
    assert.throws(() => new SimTurnsCoordinator({epoch}), /epoch/);
  }
  for (const mergeDay of [-1, 1, MAX_ENGINE_DAY + 1, 0x100000000, 2.5]) {
    assert.throws(() => new SimTurnsCoordinator({mergeDay}), /mergeDay/);
  }
  assert.equal(new SimTurnsCoordinator({mergeDay: 0}).mergeDay, 0);
  assert.equal(new SimTurnsCoordinator({mergeDay: 2}).mergeDay, 2);
  assert.equal(
    new SimTurnsCoordinator({mergeDay: MAX_ENGINE_DAY}).mergeDay,
    MAX_ENGINE_DAY,
  );
  assert.throws(
    () => new SimTurnsCoordinator({cascadeTimeoutMs: 0}),
    /positive integer/,
  );
});

test('roleHint assigns exact roles and Unknown fills the remaining role', () => {
  const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
  const host = connectPeer(coordinator, 'auto-host', 1, RoleHint.Unknown);
  const join = connectPeer(coordinator, 'join', 2, RoleHint.Join);
  assert.equal(coordinator.connections.get(host).peer.role, RoleHint.Host);
  assert.equal(coordinator.connections.get(join).peer.role, RoleHint.Join);

  const duplicate = new FakeConnection('duplicate');
  coordinator.registerConnection(duplicate);
  deliver(
    coordinator,
    duplicate,
    Op.Hello,
    encodeHello({pid: 3, roleHint: RoleHint.Host}),
  );
  assert.equal(decodeHelloAck(duplicate.last(Op.HelloAck).payload).accepted, 0);
  assert.match(decodeError(duplicate.last(Op.Error).payload), /already connected/);
  assert.equal(duplicate.closed, true);
});

test('SessionPlan is server-owned, host-first, and bootstrap uses EngineAction', () => {
  const session = readySession({mergeDay: 7});
  assert.equal(session.host.messages(Op.SessionPlan).length, 1);
  assert.equal(session.join.messages(Op.SessionPlan).length, 1);
  assert.deepEqual(
    decodeSessionPlan(session.join.last(Op.SessionPlan).payload),
    session.plan,
  );
  assert.equal(session.host.messages(Op.EngineAction).length, 1);
  assert.equal(session.join.messages(Op.EngineAction).length, 0);
  assert.equal(session.host.messages(Op.BootstrapReleased).length, 1);
  assert.equal(session.join.messages(Op.BootstrapReleased).length, 1);
  assert.deepEqual(
    decodeBootstrapProgress(session.host.last(Op.BootstrapReleased).payload),
    {handle: JOIN_HANDLE, day: 1},
  );
});

test('bootstrap accepts completion and acknowledgement evidence in either order', () => {
  const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
  const host = connectPeer(coordinator, 'host', 1, RoleHint.Host);
  const join = connectPeer(coordinator, 'join', 2, RoleHint.Join);
  deliver(coordinator, host, Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  deliver(coordinator, join, Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
  completeBootstrap(coordinator, host, join, {
    completeFirst: true,
    commitOrder: [join, host],
    operationalOrder: [host, join],
  });
});

test('harness bootstrap gate remains exact and one-shot', () => {
  const coordinator = new SimTurnsCoordinator({
    epoch: EPOCH,
    bootstrapReleaseGated: true,
  });
  const host = connectPeer(coordinator, 'host', 1, RoleHint.Host);
  const join = connectPeer(coordinator, 'join', 2, RoleHint.Join);
  deliver(coordinator, host, Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  deliver(coordinator, join, Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
  assert.equal(host.messages(Op.SessionPlan).length, 0);
  coordinator.releaseBootstrap();
  assert.equal(host.messages(Op.SessionPlan).length, 1);
  assert.throws(() => coordinator.releaseBootstrap(), /already attempted/);
});

test('ordinary flow accepts both evidence orders and commits only after ActivateTurn result', () => {
  for (const order of [
    ['observed', 'applied'],
    ['applied', 'observed'],
  ]) {
    const session = readySession();
    const oldLease = endTurnEvidence(session, 'join', order);
    const apply = latestAction(session.host);
    assert.equal(apply.kind, EngineActionKind.ApplyTurnStart);
    assert.equal(session.coordinator.snapshot().peers.join.currentDay, 1);
    assert.equal(session.coordinator.snapshot().peers.join.inFlight, true);

    actionResult(session.coordinator, session.host, apply);
    const activate = latestAction(session.join);
    assert.equal(activate.kind, EngineActionKind.ActivateTurn);
    assert.equal(activate.actionId, apply.actionId);
    assert.notEqual(activate.lease, oldLease);
    assert.equal(session.coordinator.snapshot().peers.join.currentDay, 1);
    assert.equal(session.coordinator.snapshot().peers.join.inFlight, true);

    actionResult(session.coordinator, session.join, activate);
    const snapshot = session.coordinator.snapshot();
    assert.equal(snapshot.peers.join.currentDay, 2);
    assert.equal(snapshot.peers.join.lease, activate.lease);
    assert.equal(snapshot.peers.join.inFlight, false);
    assert.equal(snapshot.phase, SessionPhase.Independent);
  }
});

test('players advance independently and serialized jobs retain their own leases', () => {
  const session = readySession();
  const hostLease = endTurnEvidence(session, 'host');
  const hostApply = latestAction(session.host);
  const joinLease = endTurnEvidence(session, 'join', ['applied', 'observed']);
  assert.equal(session.coordinator.snapshot().queueLength, 1);

  actionResult(session.coordinator, session.host, hostApply);
  const hostActivate = latestAction(session.host);
  actionResult(session.coordinator, session.host, hostActivate);
  const joinApply = latestAction(session.host);
  assert.equal(joinApply.kind, EngineActionKind.ApplyTurnStart);
  assert.equal(joinApply.playerHandle, JOIN_HANDLE);
  assert.notEqual(joinApply.lease, joinLease);
  assert.notEqual(joinApply.actionId, hostApply.actionId);
  assert.notEqual(hostApply.lease, hostLease);
  actionResult(session.coordinator, session.host, joinApply);
  const joinActivate = latestAction(session.join);
  actionResult(session.coordinator, session.join, joinActivate);

  const snapshot = session.coordinator.snapshot();
  assert.equal(snapshot.peers.host.currentDay, 2);
  assert.equal(snapshot.peers.join.currentDay, 2);
  assert.equal(snapshot.queueLength, 0);
});

test('mergeDay 0 never creates a barrier', () => {
  const session = readySession({mergeDay: 0});
  for (let day = 2; day <= 4; day += 1) {
    const {apply} = finishOrdinaryTurn(session, 'host');
    assert.equal(apply.day, day);
    assert.equal(session.coordinator.snapshot().peers.host.atBarrier, false);
  }
});

test('barrier keeps currentDay at N-1 and waits for each HoldInput result', () => {
  const session = readySession({mergeDay: 2});
  endTurnEvidence(session, 'join');
  const hold = latestAction(session.join);
  assert.equal(hold.kind, EngineActionKind.HoldInput);
  let snapshot = session.coordinator.snapshot();
  assert.equal(snapshot.peers.join.currentDay, 1);
  assert.equal(snapshot.peers.join.atBarrier, true);
  assert.equal(snapshot.peers.join.held, false);
  assert.equal(snapshot.peers.join.inFlight, true);

  actionResult(session.coordinator, session.join, hold);
  snapshot = session.coordinator.snapshot();
  assert.equal(snapshot.peers.join.currentDay, 1);
  assert.equal(snapshot.peers.join.held, true);
  assert.equal(snapshot.peers.join.inFlight, false);
  assert.equal(snapshot.phase, SessionPhase.Independent);
  assert.equal(session.host.messages(Op.EngineAction).length, 1);
});

test('barrier drains a second arrival queued before the first HoldInput acknowledgement', async (t) => {
  for (const firstRole of ['host', 'join']) {
    await t.test(`${firstRole} arrives first`, (subtest) => {
      const session = readySession({mergeDay: 2});
      subtest.after(() => session.coordinator.shutdown());
      const secondRole = firstRole === 'host' ? 'join' : 'host';
      const first = session[firstRole];
      const second = session[secondRole];
      const actionsOfKind = (connection, kind) => connection.messages(Op.EngineAction)
        .map(({payload}) => decodeEngineAction(payload))
        .filter((action) => action.kind === kind);

      endTurnEvidence(session, firstRole);
      const firstHold = latestAction(first);
      assert.equal(firstHold.kind, EngineActionKind.HoldInput);
      const secondActionCount = second.messages(Op.EngineAction).length;

      // Both facts for the second peer arrive while the first HoldInput is pending.
      endTurnEvidence(session, secondRole);
      let snapshot = session.coordinator.snapshot();
      assert.equal(snapshot.queueLength, 1);
      assert.equal(snapshot.peers[firstRole].held, false);
      assert.equal(snapshot.peers[secondRole].atBarrier, false);
      assert.equal(second.messages(Op.EngineAction).length, secondActionCount);

      actionResult(session.coordinator, first, firstHold);
      assert.equal(second.messages(Op.EngineAction).length, secondActionCount + 1,
        'first HoldInput acknowledgement must dispatch the queued peer HoldInput');
      const secondHold = latestAction(second);
      assert.equal(secondHold.kind, EngineActionKind.HoldInput);
      assert.equal(secondHold.playerHandle, secondRole === 'host' ? HOST_HANDLE : JOIN_HANDLE);
      assert.equal(secondHold.day, 1);
      assert.equal(secondHold.lease, 0);
      snapshot = session.coordinator.snapshot();
      assert.equal(snapshot.queueLength, 0);
      assert.equal(snapshot.peers[firstRole].held, true);
      assert.equal(snapshot.peers[secondRole].held, false);
      assert.equal(snapshot.phase, SessionPhase.Independent);
      for (const peer of [session.host, session.join]) {
        assert.equal(actionsOfKind(peer, EngineActionKind.HoldInput).length, 1);
        assert.equal(actionsOfKind(peer, EngineActionKind.PrepareMerge).length, 0);
      }

      actionResult(session.coordinator, second, secondHold);
      snapshot = session.coordinator.snapshot();
      assert.equal(snapshot.phase, SessionPhase.PreparingMerge);
      assert.equal(snapshot.peers.host.currentDay, 1);
      assert.equal(snapshot.peers.join.currentDay, 1);
      const hostPrepare = latestAction(session.host);
      const joinPrepare = latestAction(session.join);
      assert.equal(hostPrepare.kind, EngineActionKind.PrepareMerge);
      assert.equal(joinPrepare.kind, EngineActionKind.PrepareMerge);
      assert.equal(hostPrepare.actionId, joinPrepare.actionId);
      for (const peer of [session.host, session.join]) {
        assert.equal(actionsOfKind(peer, EngineActionKind.PrepareMerge).length, 1);
      }

      actionResult(session.coordinator, session.join, joinPrepare);
      actionResult(session.coordinator, session.host, hostPrepare);
      const execute = latestAction(session.host);
      assert.equal(execute.kind, EngineActionKind.ExecuteMerge);
      assert.equal(execute.actionId, hostPrepare.actionId);
      actionResult(session.coordinator, session.host, execute);
      for (const peer of [session.join, session.host]) {
        deliver(session.coordinator, peer, Op.MergeApplied,
          encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}));
      }
      assert.equal(session.coordinator.snapshot().phase, SessionPhase.Merged);
      assert.equal(actionsOfKind(session.host, EngineActionKind.ExecuteMerge).length, 1);
      for (const peer of [session.host, session.join]) {
        assert.equal(actionsOfKind(peer, EngineActionKind.ReleaseStock).length, 1);
      }
    });
  }
});

test('merge transaction reuses one id and releases join before host', () => {
  for (const firstRole of ['host', 'join']) {
    const session = readySession({mergeDay: 2});
    const secondRole = firstRole === 'host' ? 'join' : 'host';
    arriveAtBarrier(session, firstRole);
    arriveAtBarrier(session, secondRole, ['applied', 'observed']);

    let snapshot = session.coordinator.snapshot();
    assert.equal(snapshot.phase, SessionPhase.PreparingMerge);
    assert.equal(snapshot.peers.host.currentDay, 1);
    assert.equal(snapshot.peers.join.currentDay, 1);
    const hostPrepare = latestAction(session.host);
    const joinPrepare = latestAction(session.join);
    assert.equal(hostPrepare.kind, EngineActionKind.PrepareMerge);
    assert.equal(joinPrepare.kind, EngineActionKind.PrepareMerge);
    assert.equal(hostPrepare.actionId, joinPrepare.actionId);
    assert.equal(hostPrepare.playerHandle, HOST_HANDLE);

    actionResult(session.coordinator, session.join, joinPrepare);
    assert.equal(latestAction(session.host).kind, EngineActionKind.PrepareMerge);
    actionResult(session.coordinator, session.host, hostPrepare);
    const execute = latestAction(session.host);
    assert.equal(execute.kind, EngineActionKind.ExecuteMerge);
    assert.equal(execute.actionId, hostPrepare.actionId);
    snapshot = session.coordinator.snapshot();
    assert.equal(snapshot.phase, SessionPhase.ExecutingMerge);
    assert.equal(snapshot.peers.host.currentDay, 1);
    assert.equal(snapshot.peers.join.currentDay, 1);

    deliver(
      session.coordinator,
      session.join,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
    );
    actionResult(session.coordinator, session.host, execute);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.ExecutingMerge);
    deliver(
      session.coordinator,
      session.host,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
    );

    snapshot = session.coordinator.snapshot();
    assert.equal(snapshot.phase, SessionPhase.Merged);
    assert.equal(snapshot.peers.host.currentDay, 2);
    assert.equal(snapshot.peers.join.currentDay, 2);
    const hostRelease = latestAction(session.host);
    const joinRelease = latestAction(session.join);
    assert.equal(hostRelease.kind, EngineActionKind.ReleaseStock);
    assert.equal(joinRelease.kind, EngineActionKind.ReleaseStock);
    assert.equal(hostRelease.actionId, execute.actionId);
    assert.equal(joinRelease.actionId, execute.actionId);
    const releaseEvents = session.events.filter(({op, payload}) =>
      op === Op.EngineAction &&
      decodeEngineAction(payload).kind === EngineActionKind.ReleaseStock,
    );
    assert.deepEqual(
      releaseEvents.map(({connection}) => connection),
      ['join', 'host'],
    );
  }
});

test('ExecuteMerge waits for result and both MergeApplied in any order', () => {
  const orders = [
    ['result', 'host', 'join'],
    ['host', 'join', 'result'],
    ['join', 'result', 'host'],
  ];
  for (const order of orders) {
    const session = readySession({mergeDay: 2});
    arriveAtBarrier(session, 'host');
    arriveAtBarrier(session, 'join');
    const prepare = latestAction(session.host);
    actionResult(session.coordinator, session.host, prepare);
    actionResult(session.coordinator, session.join, latestAction(session.join));
    const execute = latestAction(session.host);
    for (const item of order) {
      if (item === 'result') actionResult(session.coordinator, session.host, execute);
      else {
        deliver(
          session.coordinator,
          item === 'host' ? session.host : session.join,
          Op.MergeApplied,
          encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
        );
      }
    }
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Merged);
  }
});

test('wrong epoch and stale leases fault after activation', () => {
  {
    const session = readySession();
    const lease = session.coordinator.snapshot().peers.join.lease;
    deliver(
      session.coordinator,
      session.join,
      Op.EndTurnObserved,
      encodeEndTurnObserved({epoch: EPOCH + 1, lease}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /epoch mismatch/);
  }
  {
    const session = readySession();
    const {oldLease} = finishOrdinaryTurn(session, 'join');
    deliver(
      session.coordinator,
      session.join,
      Op.EndTurnObserved,
      encodeEndTurnObserved({epoch: EPOCH, lease: oldLease}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /lease mismatch/);
  }
});

test('ActionResult enforces expected peer, tuple identity, success, and exact-once', async (t) => {
  await t.test('wrong peer', () => {
    const session = readySession();
    endTurnEvidence(session, 'join');
    const action = latestAction(session.host);
    actionResult(session.coordinator, session.join, action);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('wrong kind', () => {
    const session = readySession();
    endTurnEvidence(session, 'join');
    const action = latestAction(session.host);
    deliver(
      session.coordinator,
      session.host,
      Op.ActionResult,
      encodeActionResult({
        epoch: EPOCH,
        actionId: action.actionId,
        kind: EngineActionKind.ActivateTurn,
        success: true,
      }),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('failure', () => {
    const session = readySession();
    endTurnEvidence(session, 'join');
    actionResult(session.coordinator, session.host, latestAction(session.host), false);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('duplicate', () => {
    const session = readySession();
    endTurnEvidence(session, 'join');
    const apply = latestAction(session.host);
    actionResult(session.coordinator, session.host, apply);
    actionResult(session.coordinator, session.host, apply);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
});

test('end-turn and action watchdogs fail closed', async (t) => {
  await t.test('missing applied evidence', async () => {
    const session = readySession({
      coordinatorOptions: {endTurnSignalTimeoutMs: 15},
    });
    const lease = session.coordinator.snapshot().peers.join.lease;
    deliver(
      session.coordinator,
      session.join,
      Op.EndTurnObserved,
      encodeEndTurnObserved({epoch: EPOCH, lease}),
    );
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /missing EndTurnApplied/);
  });
  await t.test('missing action result', async () => {
    const session = readySession({
      coordinatorOptions: {cascadeTimeoutMs: 15},
    });
    endTurnEvidence(session, 'join');
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.join.last(Op.Error).payload), /timed out/);
  });
  await t.test('late ActivateTurn result cannot retry or revive the session', async () => {
    const session = readySession({
      coordinatorOptions: {cascadeTimeoutMs: 15},
    });
    const oldLease = endTurnEvidence(session, 'join');
    const apply = latestAction(session.host);
    actionResult(session.coordinator, session.host, apply);
    const activate = latestAction(session.join);
    assert.equal(activate.kind, EngineActionKind.ActivateTurn);
    const hostActionCount = session.host.messages(Op.EngineAction).length;
    const joinActionCount = session.join.messages(Op.EngineAction).length;

    await new Promise((resolve) => setTimeout(resolve, 35));
    let snapshot = session.coordinator.snapshot();
    assert.equal(snapshot.phase, SessionPhase.Faulted);
    assert.match(
      snapshot.faultReason,
      new RegExp(`ordinary-activate actionId=${activate.actionId} timed out`),
    );
    assert.equal(snapshot.peers.join.currentDay, 1);
    assert.equal(snapshot.peers.join.lease, oldLease);
    assert.equal(snapshot.peers.join.inFlight, false);
    assert.equal(session.host.messages(Op.EngineAction).length, hostActionCount);
    assert.equal(session.join.messages(Op.EngineAction).length, joinActionCount);

    actionResult(session.coordinator, session.join, activate);
    snapshot = session.coordinator.snapshot();
    assert.equal(snapshot.phase, SessionPhase.Faulted);
    assert.equal(snapshot.peers.join.currentDay, 1);
    assert.equal(snapshot.peers.join.lease, oldLease);
    assert.equal(session.host.messages(Op.EngineAction).length, hostActionCount);
    assert.equal(session.join.messages(Op.EngineAction).length, joinActionCount);
  });
});

test('merge acknowledgements are role-scoped and exact-once', async (t) => {
  await t.test('join cannot acknowledge ExecuteMerge', () => {
    const session = readySession({mergeDay: 2});
    arriveAtBarrier(session, 'host');
    arriveAtBarrier(session, 'join');
    actionResult(session.coordinator, session.host, latestAction(session.host));
    actionResult(session.coordinator, session.join, latestAction(session.join));
    actionResult(session.coordinator, session.join, latestAction(session.host));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('duplicate MergeApplied', () => {
    const session = readySession({mergeDay: 2});
    arriveAtBarrier(session, 'host');
    arriveAtBarrier(session, 'join');
    actionResult(session.coordinator, session.host, latestAction(session.host));
    actionResult(session.coordinator, session.join, latestAction(session.join));
    const execute = latestAction(session.host);
    const payload = encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId});
    deliver(session.coordinator, session.join, Op.MergeApplied, payload);
    deliver(session.coordinator, session.join, Op.MergeApplied, payload);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
});

test('partial ReleaseStock delivery is terminal and never retried', () => {
  const session = readySession({mergeDay: 2});
  arriveAtBarrier(session, 'host');
  arriveAtBarrier(session, 'join');
  actionResult(session.coordinator, session.host, latestAction(session.host));
  actionResult(session.coordinator, session.join, latestAction(session.join));
  const execute = latestAction(session.host);
  actionResult(session.coordinator, session.host, execute);
  deliver(
    session.coordinator,
    session.host,
    Op.MergeApplied,
    encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
  );
  session.host.failOnOp = Op.EngineAction;
  deliver(
    session.coordinator,
    session.join,
    Op.MergeApplied,
    encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
  );
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  const joinReleases = session.join.messages(Op.EngineAction)
    .map(({payload}) => decodeEngineAction(payload))
    .filter(({kind}) => kind === EngineActionKind.ReleaseStock);
  assert.equal(joinReleases.length, 1);
  session.host.failOnOp = null;
  deliver(
    session.coordinator,
    session.join,
    Op.MergeApplied,
    encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
  );
  assert.equal(joinReleases.length, 1);
});

test('disconnect before SessionPlan is replaceable; disconnect afterward faults', () => {
  const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
  const first = connectPeer(coordinator, 'first', 1, RoleHint.Join);
  coordinator.disconnect(first);
  const replacement = connectPeer(coordinator, 'replacement', 2, RoleHint.Join);
  assert.equal(replacement.closed, false);

  const session = readySession();
  session.coordinator.disconnect(session.join);
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
});

test('unexpected client engine actions are terminal after SessionPlan', () => {
  const session = readySession();
  deliver(session.coordinator, session.join, Op.EngineAction, Buffer.alloc(24));
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  assert.match(decodeError(session.host.last(Op.Error).payload), /unexpected client opcode/);
});

test('ActionResult codec used by tests remains tuple-exact', () => {
  const payload = encodeActionResult({
    epoch: EPOCH,
    actionId: 77,
    kind: EngineActionKind.HoldInput,
    success: true,
  });
  assert.deepEqual(decodeActionResult(payload), {
    epoch: EPOCH,
    actionId: 77,
    kind: EngineActionKind.HoldInput,
    success: true,
  });
});

test('bootstrap release gate rejects disabled, premature, and repeated release', async (t) => {
  await t.test('disabled gate', () => {
    const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
    assert.throws(() => coordinator.releaseBootstrap(), /not configured/);
    assert.equal(coordinator.snapshot().phase, SessionPhase.Waiting);
  });
  await t.test('premature one-shot release', () => {
    const coordinator = new SimTurnsCoordinator({
      epoch: EPOCH,
      bootstrapReleaseGated: true,
    });
    const host = connectPeer(coordinator, 'host', 1, RoleHint.Host);
    connectPeer(coordinator, 'join', 2, RoleHint.Join);
    deliver(
      coordinator,
      host,
      Op.LocalPlayerHandle,
      encodeLocalPlayerHandle(HOST_HANDLE),
    );
    assert.throws(() => coordinator.releaseBootstrap(), /both exact handles/);
    assert.equal(coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.throws(() => coordinator.releaseBootstrap(), /already attempted/);
    assert.equal(host.messages(Op.SessionPlan).length, 0);
  });
});

test('SessionPlan staging is host-first, exact-once, and send-fail closed', async (t) => {
  await t.test('join activation before its plan', () => {
    const session = plannedSession();
    deliver(session.coordinator, session.join, Op.SessionActivated);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.join.messages(Op.SessionPlan).length, 0);
  });
  await t.test('duplicate host activation', () => {
    const session = plannedSession();
    deliver(session.coordinator, session.host, Op.SessionActivated);
    assert.equal(session.join.messages(Op.SessionPlan).length, 1);
    deliver(session.coordinator, session.host, Op.SessionActivated);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.join.messages(Op.SessionPlan).length, 1);
  });
  await t.test('initial host plan write failure', () => {
    const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
    const host = connectPeer(coordinator, 'host', 1, RoleHint.Host);
    const join = connectPeer(coordinator, 'join', 2, RoleHint.Join);
    deliver(coordinator, host, Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
    host.failOnOp = Op.SessionPlan;
    deliver(coordinator, join, Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
    assert.equal(coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(host.messages(Op.SessionPlan).length, 0);
    assert.equal(join.messages(Op.SessionPlan).length, 0);
  });
  await t.test('staged join plan write failure', () => {
    const session = plannedSession();
    session.join.failOnOp = Op.SessionPlan;
    deliver(session.coordinator, session.host, Op.SessionActivated);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.host.messages(Op.SessionPlan).length, 1);
    assert.equal(session.join.messages(Op.SessionPlan).length, 0);
  });
});

test('bootstrap delay is one-shot and is cleared on shutdown', async (t) => {
  await t.test('single anchored dispatch', async () => {
    const session = plannedSession({
      coordinatorOptions: {bootstrapCascadeDelayMs: 15},
    });
    deliver(session.coordinator, session.host, Op.SessionActivated);
    deliver(session.coordinator, session.join, Op.SessionActivated);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapBeginTurnApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.host.messages(Op.EngineAction).length, 0);
    await new Promise((resolve) => setTimeout(resolve, 30));
    assert.equal(session.host.messages(Op.EngineAction).length, 1);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapBeginTurnApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.host.messages(Op.EngineAction).length, 1);
  });
  await t.test('shutdown cancels pending delay', async () => {
    const session = plannedSession({
      coordinatorOptions: {bootstrapCascadeDelayMs: 20},
    });
    deliver(session.coordinator, session.host, Op.SessionActivated);
    deliver(session.coordinator, session.join, Op.SessionActivated);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapBeginTurnApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    session.coordinator.shutdown();
    await new Promise((resolve) => setTimeout(resolve, 40));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Closed);
    assert.equal(session.host.messages(Op.EngineAction).length, 0);
  });
  await t.test('delay range is exact', () => {
    assert.equal(
      new SimTurnsCoordinator({bootstrapCascadeDelayMs: MAX_ONE_SHOT_DELAY_MS})
        .bootstrapCascadeDelayMs,
      MAX_ONE_SHOT_DELAY_MS,
    );
    for (const value of [-1, 1.5, MAX_ONE_SHOT_DELAY_MS + 1]) {
      assert.throws(
        () => new SimTurnsCoordinator({bootstrapCascadeDelayMs: value}),
        /bootstrapCascadeDelayMs/,
      );
    }
  });
});

test('BootstrapReleased claims Ready before host-first exact fan-out', () => {
  const session = plannedSession();
  reachBootstrapOperational(session);
  const observed = [];
  session.host.onSend = ({op}) => {
    if (op === Op.BootstrapReleased) {
      observed.push('host');
      assert.equal(session.coordinator.snapshot().phase, SessionPhase.Ready);
      assert.equal(session.coordinator.snapshot().ready, true);
    }
  };
  session.join.onSend = ({op}) => {
    if (op === Op.BootstrapReleased) observed.push('join');
  };
  deliver(
    session.coordinator,
    session.join,
    Op.BootstrapOperationalApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  assert.deepEqual(observed, []);
  deliver(
    session.coordinator,
    session.host,
    Op.BootstrapOperationalApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  assert.deepEqual(observed, ['host', 'join']);
  assert.equal(session.host.messages(Op.BootstrapReleased).length, 1);
  assert.equal(session.join.messages(Op.BootstrapReleased).length, 1);
});

test('partial BootstrapReleased fan-out is terminal and never retried', () => {
  const session = plannedSession();
  reachBootstrapOperational(session);
  session.join.failOnOp = Op.BootstrapReleased;
  deliver(
    session.coordinator,
    session.host,
    Op.BootstrapOperationalApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  deliver(
    session.coordinator,
    session.join,
    Op.BootstrapOperationalApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  assert.equal(session.host.messages(Op.BootstrapReleased).length, 1);
  assert.equal(session.join.messages(Op.BootstrapReleased).length, 0);
  session.join.failOnOp = null;
  deliver(
    session.coordinator,
    session.host,
    Op.BootstrapOperationalApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  assert.equal(session.host.messages(Op.BootstrapReleased).length, 1);
  assert.equal(session.join.messages(Op.BootstrapReleased).length, 0);
});

test('human-dependent bootstrap observations have no wall-clock deadline', async (t) => {
  await t.test('SessionActivated wait', async () => {
    const session = plannedSession({
      coordinatorOptions: {bootstrapTimeoutMs: 10},
    });
    await new Promise((resolve) => setTimeout(resolve, 30));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Activating);
    assert.equal(session.host.messages(Op.Error).length, 0);
  });
  await t.test('BootstrapComplete wait', async () => {
    const session = plannedSession({
      coordinatorOptions: {bootstrapTimeoutMs: 10},
    });
    const action = dispatchBootstrap(session);
    actionResult(session.coordinator, session.host, action);
    await new Promise((resolve) => setTimeout(resolve, 30));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Bootstrapping);
    assert.equal(session.host.messages(Op.EngineAction).length, 1);
    assert.equal(session.host.messages(Op.Error).length, 0);
  });
});

test('machine bootstrap acknowledgement stages have terminal one-shot timeouts', async (t) => {
  await t.test('commit acknowledgement', async () => {
    const session = plannedSession({
      coordinatorOptions: {bootstrapTimeoutMs: 15},
    });
    reachBootstrapCommit(session);
    deliver(
      session.coordinator,
      session.host,
      Op.BootstrapCommitApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.join.last(Op.Error).payload), /BootstrapCommitApplied/);
    assert.equal(session.host.messages(Op.BootstrapOperational).length, 0);
  });
  await t.test('operational acknowledgement', async () => {
    const session = plannedSession({
      coordinatorOptions: {bootstrapTimeoutMs: 15},
    });
    reachBootstrapOperational(session);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapOperationalApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(
      decodeError(session.host.last(Op.Error).payload),
      /BootstrapOperationalApplied/,
    );
    assert.equal(session.host.messages(Op.BootstrapReleased).length, 0);
  });
  await t.test('completed bootstrap clears its timer', async () => {
    const session = readySession({
      coordinatorOptions: {bootstrapTimeoutMs: 10},
    });
    await new Promise((resolve) => setTimeout(resolve, 30));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Ready);
    assert.equal(session.host.messages(Op.Error).length, 0);
  });
});

test('bootstrap rejects out-of-order, wrong-role, malformed, and duplicate events', async (t) => {
  await t.test('join begin-turn evidence before staged plan', () => {
    const session = plannedSession();
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapBeginTurnApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('host reports join-only begin-turn evidence', () => {
    const session = plannedSession();
    deliver(session.coordinator, session.host, Op.SessionActivated);
    deliver(session.coordinator, session.join, Op.SessionActivated);
    deliver(
      session.coordinator,
      session.host,
      Op.BootstrapBeginTurnApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('BootstrapComplete before cascade dispatch', () => {
    const session = plannedSession();
    deliver(session.coordinator, session.host, Op.SessionActivated);
    deliver(session.coordinator, session.join, Op.SessionActivated);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapComplete,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('host reports join-only BootstrapComplete', () => {
    const session = plannedSession();
    dispatchBootstrap(session);
    deliver(
      session.coordinator,
      session.host,
      Op.BootstrapComplete,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('duplicate BootstrapComplete', () => {
    const session = plannedSession();
    dispatchBootstrap(session);
    const payload = encodeBootstrapProgress(JOIN_HANDLE, 1);
    deliver(session.coordinator, session.join, Op.BootstrapComplete, payload);
    deliver(session.coordinator, session.join, Op.BootstrapComplete, payload);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('commit acknowledgement before commit fan-out', () => {
    const session = plannedSession();
    dispatchBootstrap(session);
    deliver(
      session.coordinator,
      session.host,
      Op.BootstrapCommitApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('wrong-handle commit acknowledgement', () => {
    const session = plannedSession();
    reachBootstrapCommit(session);
    deliver(
      session.coordinator,
      session.host,
      Op.BootstrapCommitApplied,
      encodeBootstrapProgress(HOST_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('malformed operational acknowledgement', () => {
    const session = plannedSession();
    reachBootstrapOperational(session);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapOperationalApplied,
      rawProgress(JOIN_HANDLE, 2),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('duplicate operational acknowledgement', () => {
    const session = plannedSession();
    reachBootstrapOperational(session);
    const payload = encodeBootstrapProgress(JOIN_HANDLE, 1);
    deliver(session.coordinator, session.join, Op.BootstrapOperationalApplied, payload);
    deliver(session.coordinator, session.join, Op.BootstrapOperationalApplied, payload);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.host.messages(Op.BootstrapReleased).length, 0);
  });
});

test('end-turn evidence is an exact two-party barrier', async (t) => {
  await t.test('first evidence cannot dispatch', () => {
    const session = readySession();
    const baseline = session.host.messages(Op.EngineAction).length;
    const lease = session.coordinator.snapshot().peers.join.lease;
    deliver(
      session.coordinator,
      session.join,
      Op.EndTurnObserved,
      encodeEndTurnObserved({epoch: EPOCH, lease}),
    );
    assert.equal(session.host.messages(Op.EngineAction).length, baseline);
    assert.equal(session.coordinator.snapshot().peers.join.inFlight, true);
    deliver(
      session.coordinator,
      session.host,
      Op.EndTurnApplied,
      encodeEndTurnApplied({epoch: EPOCH, lease}),
    );
    assert.equal(session.host.messages(Op.EngineAction).length, baseline + 1);
  });
  await t.test('only host may report applied evidence', () => {
    const session = readySession();
    const lease = session.coordinator.snapshot().peers.join.lease;
    deliver(
      session.coordinator,
      session.join,
      Op.EndTurnApplied,
      encodeEndTurnApplied({epoch: EPOCH, lease}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('duplicate first evidence', () => {
    const session = readySession();
    const lease = session.coordinator.snapshot().peers.join.lease;
    const payload = encodeEndTurnObserved({epoch: EPOCH, lease});
    deliver(session.coordinator, session.join, Op.EndTurnObserved, payload);
    deliver(session.coordinator, session.join, Op.EndTurnObserved, payload);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('wrong current lease', () => {
    const session = readySession();
    const hostLease = session.coordinator.snapshot().peers.host.lease;
    deliver(
      session.coordinator,
      session.join,
      Op.EndTurnObserved,
      encodeEndTurnObserved({epoch: EPOCH, lease: hostLease}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('evidence before bootstrap release', () => {
    const session = plannedSession();
    deliver(
      session.coordinator,
      session.join,
      Op.EndTurnObserved,
      encodeEndTurnObserved({epoch: EPOCH, lease: session.plan.joinLease}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.host.messages(Op.EngineAction).length, 0);
  });
  await t.test('missing observed evidence times out', async () => {
    const session = readySession({
      coordinatorOptions: {endTurnSignalTimeoutMs: 15},
    });
    const lease = session.coordinator.snapshot().peers.join.lease;
    deliver(
      session.coordinator,
      session.host,
      Op.EndTurnApplied,
      encodeEndTurnApplied({epoch: EPOCH, lease}),
    );
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.join.last(Op.Error).payload), /missing EndTurnObserved/);
  });
});

test('two partial peer barriers remain closed and later serialize exactly once', () => {
  const session = readySession();
  const hostLease = session.coordinator.snapshot().peers.host.lease;
  const joinLease = session.coordinator.snapshot().peers.join.lease;
  const baseline = session.host.messages(Op.EngineAction).length;
  deliver(
    session.coordinator,
    session.host,
    Op.EndTurnObserved,
    encodeEndTurnObserved({epoch: EPOCH, lease: hostLease}),
  );
  deliver(
    session.coordinator,
    session.join,
    Op.EndTurnObserved,
    encodeEndTurnObserved({epoch: EPOCH, lease: joinLease}),
  );
  assert.equal(session.host.messages(Op.EngineAction).length, baseline);
  deliver(
    session.coordinator,
    session.host,
    Op.EndTurnApplied,
    encodeEndTurnApplied({epoch: EPOCH, lease: joinLease}),
  );
  deliver(
    session.coordinator,
    session.host,
    Op.EndTurnApplied,
    encodeEndTurnApplied({epoch: EPOCH, lease: hostLease}),
  );
  assert.equal(session.host.messages(Op.EngineAction).length, baseline + 1);
  assert.equal(session.coordinator.snapshot().queueLength, 1);
  const first = latestAction(session.host);
  actionResult(session.coordinator, session.host, first);
  const firstOrigin = first.playerHandle === HOST_HANDLE ? session.host : session.join;
  actionResult(session.coordinator, firstOrigin, latestAction(firstOrigin));
  assert.equal(session.host.messages(Op.EngineAction).length, baseline + 2);
});

test('Hello and unauthenticated input are isolated before session activation', async (t) => {
  await t.test('malformed Hello', () => {
    const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
    const connection = new FakeConnection('bad');
    coordinator.registerConnection(connection);
    deliver(coordinator, connection, Op.Hello, Buffer.alloc(11));
    assert.equal(decodeHelloAck(connection.last(Op.HelloAck).payload).accepted, 0);
    assert.equal(connection.closed, true);
    assert.equal(coordinator.snapshot().phase, SessionPhase.Waiting);
  });
  await t.test('version mismatch and zero pid', () => {
    for (const hello of [
      encodeHello({version: PROTOCOL_VERSION - 1, pid: 1, roleHint: RoleHint.Host}),
      encodeHello({pid: 0, roleHint: RoleHint.Host}),
    ]) {
      const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
      const connection = new FakeConnection('bad');
      coordinator.registerConnection(connection);
      deliver(coordinator, connection, Op.Hello, hello);
      assert.equal(decodeHelloAck(connection.last(Op.HelloAck).payload).accepted, 0);
      assert.equal(coordinator.snapshot().phase, SessionPhase.Waiting);
    }
  });
  await t.test('duplicate pid does not poison replacement role', () => {
    const coordinator = new SimTurnsCoordinator({epoch: EPOCH});
    connectPeer(coordinator, 'host', 1, RoleHint.Host);
    const duplicate = new FakeConnection('duplicate');
    coordinator.registerConnection(duplicate);
    deliver(
      coordinator,
      duplicate,
      Op.Hello,
      encodeHello({pid: 1, roleHint: RoleHint.Join}),
    );
    assert.equal(decodeHelloAck(duplicate.last(Op.HelloAck).payload).accepted, 0);
    const join = connectPeer(coordinator, 'join', 2, RoleHint.Join);
    assert.equal(join.closed, false);
  });
  await t.test('unauthenticated bad connection cannot reset ready session', () => {
    const session = readySession();
    const outsider = new FakeConnection('outsider');
    session.coordinator.registerConnection(outsider);
    deliver(
      session.coordinator,
      outsider,
      Op.LocalPlayerHandle,
      encodeLocalPlayerHandle(0x1234),
    );
    assert.equal(outsider.closed, true);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Ready);
    assert.equal(session.host.messages(Op.Error).length, 0);
  });
});

test('engine-action transport failures are terminal at every mutable stage', async (t) => {
  await t.test('ApplyTurnStart write', () => {
    const session = readySession();
    session.host.failOnOp = Op.EngineAction;
    endTurnEvidence(session, 'join');
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('ActivateTurn write', () => {
    const session = readySession();
    endTurnEvidence(session, 'join');
    const apply = latestAction(session.host);
    session.join.failOnOp = Op.EngineAction;
    actionResult(session.coordinator, session.host, apply);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('HoldInput write', () => {
    const session = readySession({mergeDay: 2});
    session.join.failOnOp = Op.EngineAction;
    endTurnEvidence(session, 'join');
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('PrepareMerge partial fan-out', () => {
    const session = readySession({mergeDay: 2});
    arriveAtBarrier(session, 'host');
    session.host.failOnOp = Op.EngineAction;
    arriveAtBarrier(session, 'join');
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    const joinPrepare = session.join.messages(Op.EngineAction)
      .map(({payload}) => decodeEngineAction(payload))
      .filter(({kind}) => kind === EngineActionKind.PrepareMerge);
    assert.equal(joinPrepare.length, 1);
  });
  await t.test('ExecuteMerge write', () => {
    const session = preparingMergeSession();
    const hostPrepare = latestAction(session.host);
    const joinPrepare = latestAction(session.join);
    actionResult(session.coordinator, session.host, hostPrepare);
    session.host.failOnOp = Op.EngineAction;
    actionResult(session.coordinator, session.join, joinPrepare);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
});

test('merge transaction rejects premature, mismatched, duplicate, and failed evidence', async (t) => {
  await t.test('MergeApplied before ExecuteMerge', () => {
    const session = preparingMergeSession();
    const actionId = session.coordinator.snapshot().pendingMerge.actionId;
    deliver(
      session.coordinator,
      session.join,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH, actionId}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('duplicate PrepareMerge result', () => {
    const session = preparingMergeSession();
    const prepare = latestAction(session.join);
    actionResult(session.coordinator, session.join, prepare);
    actionResult(session.coordinator, session.join, prepare);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('failed PrepareMerge result', () => {
    const session = preparingMergeSession();
    actionResult(session.coordinator, session.join, latestAction(session.join), false);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('failed ExecuteMerge result', () => {
    const session = executingMergeSession();
    actionResult(session.coordinator, session.host, session.execute, false);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('wrong MergeApplied action id', () => {
    const session = executingMergeSession();
    deliver(
      session.coordinator,
      session.join,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH, actionId: session.execute.actionId + 1}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
  await t.test('wrong MergeApplied epoch', () => {
    const session = executingMergeSession();
    deliver(
      session.coordinator,
      session.join,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH + 1, actionId: session.execute.actionId}),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  });
});

test('merge action watchdog covers Hold, Prepare, and Execute evidence', async (t) => {
  assert.equal(DEFAULT_MERGE_APPLY_TIMEOUT_MS, 30_000);
  await t.test('HoldInput result timeout', async () => {
    const session = readySession({
      mergeDay: 2,
      coordinatorOptions: {mergeApplyTimeoutMs: 15},
    });
    endTurnEvidence(session, 'join');
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /hold-input/);
  });
  await t.test('partial PrepareMerge result does not extend timeout', async () => {
    const session = preparingMergeSession({mergeApplyTimeoutMs: 20});
    await new Promise((resolve) => setTimeout(resolve, 10));
    actionResult(session.coordinator, session.join, latestAction(session.join));
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /PrepareMerge/);
  });
  await t.test('partial ExecuteMerge evidence times out', async () => {
    const session = executingMergeSession({mergeApplyTimeoutMs: 15});
    actionResult(session.coordinator, session.host, session.execute);
    deliver(
      session.coordinator,
      session.host,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH, actionId: session.execute.actionId}),
    );
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.join.last(Op.Error).payload), /ExecuteMerge evidence/);
  });
  await t.test('completed merge leaves no stale timeout', async () => {
    const session = executingMergeSession({mergeApplyTimeoutMs: 15});
    actionResult(session.coordinator, session.host, session.execute);
    for (const peer of [session.join, session.host]) {
      deliver(
        session.coordinator,
        peer,
        Op.MergeApplied,
        encodeMergeApplied({epoch: EPOCH, actionId: session.execute.actionId}),
      );
    }
    await new Promise((resolve) => setTimeout(resolve, 35));
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Merged);
  });
});

test('disconnect during merge preparation or execution faults the survivor', async (t) => {
  await t.test('preparation', () => {
    const session = preparingMergeSession();
    session.coordinator.disconnect(session.join);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.ok(session.host.messages(Op.Error).length > 0);
  });
  await t.test('execution', () => {
    const session = executingMergeSession();
    session.coordinator.disconnect(session.host);
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.ok(session.join.messages(Op.Error).length > 0);
  });
});

test('Merged rejects follow-up evidence without reopening coordinator flow', () => {
  const session = executingMergeSession();
  actionResult(session.coordinator, session.host, session.execute);
  for (const peer of [session.host, session.join]) {
    deliver(
      session.coordinator,
      peer,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH, actionId: session.execute.actionId}),
    );
  }
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Merged);
  const releasesBefore = session.host.messages(Op.EngineAction).length;
  deliver(
    session.coordinator,
    session.host,
    Op.EndTurnApplied,
    encodeEndTurnApplied({epoch: EPOCH, lease: 1}),
  );
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Merged);
  assert.equal(session.host.messages(Op.EngineAction).length, releasesBefore);
  assert.match(decodeError(session.host.last(Op.Error).payload), /already merged/);
});

test('opaque action and lease identifiers exhaust fail-closed instead of wrapping', async (t) => {
  await t.test('action id UINT32_MAX is last usable transaction', () => {
    const session = readySession();
    session.coordinator.nextActionId = 0xffffffff;
    const first = finishOrdinaryTurn(session, 'join');
    assert.equal(first.apply.actionId, 0xffffffff);
    assert.equal(first.activate.actionId, 0xffffffff);
    const actionCount = session.host.messages(Op.EngineAction).length;
    endTurnEvidence(session, 'join');
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /action id space exhausted/);
    assert.equal(session.host.messages(Op.EngineAction).length, actionCount);
  });
  await t.test('lease UINT32_MAX is last usable grant', () => {
    const session = readySession();
    session.coordinator.nextLease = 0xffffffff;
    const first = finishOrdinaryTurn(session, 'join');
    assert.equal(first.apply.lease, 0xffffffff);
    assert.equal(first.activate.lease, 0xffffffff);
    const actionCount = session.host.messages(Op.EngineAction).length;
    endTurnEvidence(session, 'join');
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /lease space exhausted/);
    assert.equal(session.host.messages(Op.EngineAction).length, actionCount);
  });
});

test('partial bootstrap intermediate fan-outs are terminal and never retried', async (t) => {
  await t.test('BootstrapCommitted join delivered, host write fails', () => {
    const session = plannedSession();
    const action = dispatchBootstrap(session);
    session.host.failOnOp = Op.BootstrapCommitted;
    actionResult(session.coordinator, session.host, action);
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapComplete,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.join.messages(Op.BootstrapCommitted).length, 1);
    assert.equal(session.host.messages(Op.BootstrapCommitted).length, 0);
  });
  await t.test('BootstrapOperational host delivered, join write fails', () => {
    const session = plannedSession();
    reachBootstrapCommit(session);
    session.join.failOnOp = Op.BootstrapOperational;
    deliver(
      session.coordinator,
      session.host,
      Op.BootstrapCommitApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    deliver(
      session.coordinator,
      session.join,
      Op.BootstrapCommitApplied,
      encodeBootstrapProgress(JOIN_HANDLE, 1),
    );
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.equal(session.host.messages(Op.BootstrapOperational).length, 1);
    assert.equal(session.join.messages(Op.BootstrapOperational).length, 0);
  });
});

test('engine-action telemetry identifies transaction stage and executing role', () => {
  const events = [];
  const session = readySession({
    coordinatorOptions: {
      logger: (event, fields) => events.push({event, fields}),
    },
  });
  endTurnEvidence(session, 'join');
  const dispatch = events.filter(({event}) => event === 'engine-action-dispatched').at(-1);
  assert.equal(dispatch.fields.stage, 'ordinary-apply');
  assert.equal(dispatch.fields.recipient, 'host');
  assert.equal(dispatch.fields.playerHandle, JOIN_HANDLE);
  assert.equal(dispatch.fields.lease, latestAction(session.host).lease);
});

test('malformed ActionResult payload faults without a later action', () => {
  const session = readySession();
  endTurnEvidence(session, 'join');
  const hostActions = session.host.messages(Op.EngineAction).length;
  deliver(session.coordinator, session.host, Op.ActionResult, Buffer.alloc(15));
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  assert.equal(session.host.messages(Op.EngineAction).length, hostActions);
  assert.match(decodeError(session.join.last(Op.Error).payload), /must be 16 bytes/);
});

test('disconnect after completed merge faults the survivor', () => {
  const session = executingMergeSession();
  actionResult(session.coordinator, session.host, session.execute);
  for (const peer of [session.join, session.host]) {
    deliver(
      session.coordinator,
      peer,
      Op.MergeApplied,
      encodeMergeApplied({epoch: EPOCH, actionId: session.execute.actionId}),
    );
  }
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Merged);
  session.coordinator.disconnect(session.join);
  assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
  assert.match(decodeError(session.host.last(Op.Error).payload), /disconnected/);
});

test('server-owned day arithmetic faults on barrier crossing and engine overflow', async (t) => {
  await t.test('INT32_MAX remains a valid final engine day', () => {
    const session = readySession();
    session.coordinator.connections.get(session.join).peer.currentDay =
      MAX_ENGINE_DAY - 1;
    const {apply, activate} = finishOrdinaryTurn(session, 'join');
    assert.equal(apply.day, MAX_ENGINE_DAY);
    assert.equal(activate.day, MAX_ENGINE_DAY);
    assert.equal(
      session.coordinator.snapshot().peers.join.currentDay,
      MAX_ENGINE_DAY,
    );
  });
  await t.test('nextDay beyond mergeDay', () => {
    const session = readySession({mergeDay: 2});
    session.coordinator.connections.get(session.join).peer.currentDay = 2;
    const baseline = session.host.messages(Op.EngineAction).length;
    endTurnEvidence(session, 'join');
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(decodeError(session.host.last(Op.Error).payload), /crossed mergeDay/);
    assert.equal(session.host.messages(Op.EngineAction).length, baseline);
  });
  await t.test('INT32_MAX is terminal for ordinary next-day progression', () => {
    const session = readySession();
    session.coordinator.connections.get(session.join).peer.currentDay = MAX_ENGINE_DAY;
    const baseline = session.host.messages(Op.EngineAction).length;
    endTurnEvidence(session, 'join');
    assert.equal(session.coordinator.snapshot().phase, SessionPhase.Faulted);
    assert.match(
      decodeError(session.host.last(Op.Error).payload),
      /engine day overflow/,
    );
    assert.equal(session.host.messages(Op.EngineAction).length, baseline);
  });
});
