import assert from 'node:assert/strict';
import {randomUUID} from 'node:crypto';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {
  EngineActionKind,
  FrameDecoder,
  Op,
  PROTOCOL_VERSION,
  RoleHint,
  SessionMode,
  decodeBootstrapProgress,
  decodeEngineAction,
  decodeError,
  decodeHelloAck,
  decodeSessionPlan,
  encodeActionResult,
  encodeBootstrapProgress,
  encodeEndTurnApplied,
  encodeEndTurnObserved,
  encodeFrame,
  encodeHello,
  encodeLocalPlayerHandle,
  encodeMergeApplied,
} from '../src/protocol.js';
import {
  DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS,
  SessionPhase,
} from '../src/coordinator.js';
import {
  SimTurnsRelayServer,
  resolveBootstrapReleaseFile,
  resolvePipeName,
  startRelayServer,
} from '../src/server.js';

const EPOCH = 0x55667788;
const HOST_HANDLE = 0xa3de0001;
const JOIN_HANDLE = 0xa3de0002;

function uniquePipeName() {
  const suffix = `${process.pid}.${randomUUID()}`;
  return process.platform === 'win32'
    ? String.raw`\\.\pipe\d2mss.simturns.v8.test.${suffix}`
    : path.join(os.tmpdir(), `d2mss.simturns.v8.test.${suffix}.sock`);
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function waitUntil(predicate, label, timeoutMs = 1500) {
  const deadline = performance.now() + timeoutMs;
  while (!predicate()) {
    if (performance.now() >= deadline) throw new Error(`timed out waiting for ${label}`);
    await delay(5);
  }
}

class TestClient {
  constructor(socket) {
    this.socket = socket;
    this.decoder = new FrameDecoder();
    this.queued = new Map();
    this.waiters = new Map();
    this.received = [];
    socket.on('data', (chunk) => {
      for (const frame of this.decoder.push(chunk)) this._receive(frame);
    });
    socket.on('error', () => {});
  }

  static async connect(pipeName) {
    const socket = net.createConnection(pipeName);
    await new Promise((resolve, reject) => {
      socket.once('connect', resolve);
      socket.once('error', reject);
    });
    return new TestClient(socket);
  }

  send(op, payload = Buffer.alloc(0)) {
    this.socket.write(encodeFrame(op, payload));
  }

  count(op) {
    return this.received.filter((frame) => frame.op === op).length;
  }

  next(op, timeoutMs = 1500) {
    const queued = this.queued.get(op);
    if (queued?.length) return Promise.resolve(queued.shift());
    return new Promise((resolve, reject) => {
      let waiter;
      const timeout = setTimeout(() => {
        const waiters = this.waiters.get(op) ?? [];
        this.waiters.set(op, waiters.filter((candidate) => candidate !== waiter));
        reject(new Error(`timed out waiting for opcode 0x${op.toString(16)}`));
      }, timeoutMs);
      waiter = {
        resolve: (frame) => {
          clearTimeout(timeout);
          resolve(frame);
        },
      };
      const waiters = this.waiters.get(op) ?? [];
      waiters.push(waiter);
      this.waiters.set(op, waiters);
    });
  }

  destroy() {
    this.socket.destroy();
  }

  _receive(frame) {
    this.received.push(frame);
    const waiters = this.waiters.get(frame.op);
    if (waiters?.length) {
      waiters.shift().resolve(frame);
      return;
    }
    const queued = this.queued.get(frame.op) ?? [];
    queued.push(frame);
    this.queued.set(frame.op, queued);
  }
}

function sendResult(client, action, success = true) {
  client.send(
    Op.ActionResult,
    encodeActionResult({
      epoch: action.epoch,
      actionId: action.actionId,
      kind: action.kind,
      success,
    }),
  );
}

async function bootstrapClients(
  server,
  host,
  join,
  {mergeDay, bootstrapActionTimeoutMs = 1500} = {},
) {
  host.send(Op.Hello, encodeHello({pid: 101, roleHint: RoleHint.Host}));
  join.send(Op.Hello, encodeHello({pid: 202, roleHint: RoleHint.Join}));
  assert.deepEqual(decodeHelloAck((await host.next(Op.HelloAck)).payload), {
    accepted: 1,
    version: PROTOCOL_VERSION,
  });
  assert.deepEqual(decodeHelloAck((await join.next(Op.HelloAck)).payload), {
    accepted: 1,
    version: PROTOCOL_VERSION,
  });

  host.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  join.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
  const plan = decodeSessionPlan((await host.next(Op.SessionPlan)).payload);
  assert.deepEqual(plan, {
    epoch: EPOCH,
    mode: SessionMode.Simultaneous,
    hostHandle: HOST_HANDLE,
    joinHandle: JOIN_HANDLE,
    mergeDay,
    hostLease: 1,
    joinLease: 2,
  });
  assert.equal(join.count(Op.SessionPlan), 0);
  host.send(Op.SessionActivated);
  assert.deepEqual(
    decodeSessionPlan((await join.next(Op.SessionPlan)).payload),
    plan,
  );
  join.send(Op.SessionActivated);
  join.send(
    Op.BootstrapBeginTurnApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  const bootstrapAction = decodeEngineAction(
    (await host.next(Op.EngineAction, bootstrapActionTimeoutMs)).payload,
  );
  assert.equal(bootstrapAction.kind, EngineActionKind.ApplyTurnStart);
  assert.equal(bootstrapAction.playerHandle, JOIN_HANDLE);
  assert.equal(bootstrapAction.day, 1);
  assert.equal(bootstrapAction.lease, plan.joinLease);

  join.send(Op.BootstrapComplete, encodeBootstrapProgress(JOIN_HANDLE, 1));
  sendResult(host, bootstrapAction);
  assert.deepEqual(
    decodeBootstrapProgress((await join.next(Op.BootstrapCommitted)).payload),
    {handle: JOIN_HANDLE, day: 1},
  );
  assert.deepEqual(
    decodeBootstrapProgress((await host.next(Op.BootstrapCommitted)).payload),
    {handle: JOIN_HANDLE, day: 1},
  );
  join.send(Op.BootstrapCommitApplied, encodeBootstrapProgress(JOIN_HANDLE, 1));
  host.send(Op.BootstrapCommitApplied, encodeBootstrapProgress(JOIN_HANDLE, 1));
  assert.deepEqual(
    decodeBootstrapProgress((await host.next(Op.BootstrapOperational)).payload),
    {handle: JOIN_HANDLE, day: 1},
  );
  assert.deepEqual(
    decodeBootstrapProgress((await join.next(Op.BootstrapOperational)).payload),
    {handle: JOIN_HANDLE, day: 1},
  );
  host.send(Op.BootstrapOperationalApplied, encodeBootstrapProgress(JOIN_HANDLE, 1));
  join.send(Op.BootstrapOperationalApplied, encodeBootstrapProgress(JOIN_HANDLE, 1));
  assert.deepEqual(
    decodeBootstrapProgress((await host.next(Op.BootstrapReleased)).payload),
    {handle: JOIN_HANDLE, day: 1},
  );
  assert.deepEqual(
    decodeBootstrapProgress((await join.next(Op.BootstrapReleased)).payload),
    {handle: JOIN_HANDLE, day: 1},
  );
  await waitUntil(
    () => server.coordinator.snapshot().phase === SessionPhase.Ready,
    'ready coordinator',
  );
  return plan;
}

async function openReadyServer(t, {mergeDay = 0, ...serverOptions} = {}) {
  const server = await startRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    mergeDay,
    logger: () => {},
    ...serverOptions,
  });
  const host = await TestClient.connect(server.pipeName);
  const join = await TestClient.connect(server.pipeName);
  t.after(async () => {
    host.destroy();
    join.destroy();
    await server.close();
  });
  const plan = await bootstrapClients(server, host, join, {mergeDay});
  return {server, host, join, plan};
}

function sendEndTurnPair(session, role, lease, order = ['observed', 'applied']) {
  const origin = role === 'host' ? session.host : session.join;
  for (const kind of order) {
    if (kind === 'observed') {
      origin.send(
        Op.EndTurnObserved,
        encodeEndTurnObserved({epoch: EPOCH, lease}),
      );
    } else {
      session.host.send(
        Op.EndTurnApplied,
        encodeEndTurnApplied({epoch: EPOCH, lease}),
      );
    }
  }
}

async function reachPreparingMerge(session) {
  sendEndTurnPair(session, 'join', session.plan.joinLease);
  const joinHold = decodeEngineAction((await session.join.next(Op.EngineAction)).payload);
  assert.equal(joinHold.kind, EngineActionKind.HoldInput);
  sendResult(session.join, joinHold);
  sendEndTurnPair(session, 'host', session.plan.hostLease, ['applied', 'observed']);
  const hostHold = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(hostHold.kind, EngineActionKind.HoldInput);
  sendResult(session.host, hostHold);
  const joinPrepare = decodeEngineAction((await session.join.next(Op.EngineAction)).payload);
  const hostPrepare = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(joinPrepare.kind, EngineActionKind.PrepareMerge);
  assert.equal(hostPrepare.kind, EngineActionKind.PrepareMerge);
  return {joinPrepare, hostPrepare};
}

async function reachExecutingMerge(session) {
  const prepared = await reachPreparingMerge(session);
  sendResult(session.join, prepared.joinPrepare);
  sendResult(session.host, prepared.hostPrepare);
  const execute = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(execute.kind, EngineActionKind.ExecuteMerge);
  return {...prepared, execute};
}

test('server keeps action and end-turn evidence watchdogs independent', async (t) => {
  const server = new SimTurnsRelayServer({
    pipeName: uniquePipeName(),
    cascadeTimeoutMs: 1234,
    logger: () => {},
  });
  t.after(async () => { await server.close(); });
  assert.equal(server.coordinator.cascadeTimeoutMs, 1234);
  assert.equal(
    server.coordinator.endTurnSignalTimeoutMs,
    DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS,
  );
});

test('named pipe v8 bootstraps and waits for ActivateTurn acknowledgement', async (t) => {
  const session = await openReadyServer(t);
  sendEndTurnPair(session, 'join', session.plan.joinLease, ['applied', 'observed']);
  const apply = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.deepEqual(
    {
      kind: apply.kind,
      playerHandle: apply.playerHandle,
      day: apply.day,
      lease: apply.lease,
    },
    {
      kind: EngineActionKind.ApplyTurnStart,
      playerHandle: JOIN_HANDLE,
      day: 2,
      lease: apply.lease,
    },
  );
  assert.notEqual(apply.lease, session.plan.joinLease);
  sendResult(session.host, apply);
  const activate = decodeEngineAction((await session.join.next(Op.EngineAction)).payload);
  assert.equal(activate.kind, EngineActionKind.ActivateTurn);
  assert.equal(activate.actionId, apply.actionId);
  assert.equal(activate.lease, apply.lease);
  assert.equal(session.server.coordinator.snapshot().peers.join.currentDay, 1);
  assert.equal(session.server.coordinator.snapshot().peers.join.inFlight, true);
  sendResult(session.join, activate);
  await waitUntil(
    () => session.server.coordinator.snapshot().peers.join.currentDay === 2,
    'join day 2 activation',
  );
  assert.equal(session.server.coordinator.snapshot().peers.join.lease, activate.lease);
  assert.equal(session.server.coordinator.snapshot().peers.join.inFlight, false);
});

test('named pipe v8 holds both arrivals and executes one server-owned merge', async (t) => {
  const session = await openReadyServer(t, {mergeDay: 2});
  sendEndTurnPair(session, 'join', session.plan.joinLease);
  const joinHold = decodeEngineAction((await session.join.next(Op.EngineAction)).payload);
  assert.equal(joinHold.kind, EngineActionKind.HoldInput);
  assert.equal(joinHold.playerHandle, JOIN_HANDLE);
  assert.equal(joinHold.day, 1);
  sendResult(session.join, joinHold);

  sendEndTurnPair(session, 'host', session.plan.hostLease, ['applied', 'observed']);
  const hostHold = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(hostHold.kind, EngineActionKind.HoldInput);
  assert.equal(hostHold.playerHandle, HOST_HANDLE);
  sendResult(session.host, hostHold);

  const joinPrepare = decodeEngineAction((await session.join.next(Op.EngineAction)).payload);
  const hostPrepare = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(joinPrepare.kind, EngineActionKind.PrepareMerge);
  assert.equal(hostPrepare.kind, EngineActionKind.PrepareMerge);
  assert.equal(joinPrepare.actionId, hostPrepare.actionId);
  assert.equal(joinPrepare.playerHandle, HOST_HANDLE);
  assert.equal(session.server.coordinator.snapshot().peers.host.currentDay, 1);
  assert.equal(session.server.coordinator.snapshot().peers.join.currentDay, 1);

  sendResult(session.host, hostPrepare);
  sendResult(session.join, joinPrepare);
  const execute = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(execute.kind, EngineActionKind.ExecuteMerge);
  assert.equal(execute.actionId, hostPrepare.actionId);
  session.join.send(
    Op.MergeApplied,
    encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
  );
  session.host.send(
    Op.MergeApplied,
    encodeMergeApplied({epoch: EPOCH, actionId: execute.actionId}),
  );
  await delay(10);
  assert.equal(session.server.coordinator.snapshot().phase, SessionPhase.ExecutingMerge);
  sendResult(session.host, execute);

  const joinRelease = decodeEngineAction((await session.join.next(Op.EngineAction)).payload);
  const hostRelease = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(joinRelease.kind, EngineActionKind.ReleaseStock);
  assert.equal(hostRelease.kind, EngineActionKind.ReleaseStock);
  assert.equal(joinRelease.actionId, execute.actionId);
  assert.equal(hostRelease.actionId, execute.actionId);
  await waitUntil(
    () => session.server.coordinator.snapshot().phase === SessionPhase.Merged,
    'merged coordinator',
  );
});

test('server configuration, not Hello, controls mergeDay and epoch', async (t) => {
  const server = await startRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    mergeDay: 9,
    logger: () => {},
  });
  const host = await TestClient.connect(server.pipeName);
  const join = await TestClient.connect(server.pipeName);
  t.after(async () => {
    host.destroy();
    join.destroy();
    await server.close();
  });
  host.send(Op.Hello, encodeHello({pid: 1, roleHint: RoleHint.Host}));
  join.send(Op.Hello, encodeHello({pid: 2, roleHint: RoleHint.Join}));
  await host.next(Op.HelloAck);
  await join.next(Op.HelloAck);
  host.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  join.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
  const plan = decodeSessionPlan((await host.next(Op.SessionPlan)).payload);
  assert.equal(plan.epoch, EPOCH);
  assert.equal(plan.mergeDay, 9);
});

test('release file gates SessionPlan and delay anchors the bootstrap action', async (t) => {
  const temporary = await mkdtemp(path.join(os.tmpdir(), 'd2mss-v8-gate-'));
  const releaseFile = path.join(temporary, 'bootstrap.release');
  t.after(async () => rm(temporary, {recursive: true, force: true}));
  const server = await startRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    mergeDay: 0,
    bootstrapReleaseFile: releaseFile,
    bootstrapCascadeDelayMs: 45,
    logger: () => {},
  });
  const host = await TestClient.connect(server.pipeName);
  const join = await TestClient.connect(server.pipeName);
  t.after(async () => {
    host.destroy();
    join.destroy();
    await server.close();
  });

  host.send(Op.Hello, encodeHello({pid: 1, roleHint: RoleHint.Host}));
  join.send(Op.Hello, encodeHello({pid: 2, roleHint: RoleHint.Join}));
  await host.next(Op.HelloAck);
  await join.next(Op.HelloAck);
  host.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  join.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
  await delay(30);
  assert.equal(host.count(Op.SessionPlan), 0);
  await writeFile(releaseFile, 'release');
  const plan = decodeSessionPlan((await host.next(Op.SessionPlan)).payload);
  assert.equal(plan.mergeDay, 0);
  host.send(Op.SessionActivated);
  await join.next(Op.SessionPlan);
  join.send(Op.SessionActivated);
  const startedAt = performance.now();
  join.send(
    Op.BootstrapBeginTurnApplied,
    encodeBootstrapProgress(JOIN_HANDLE, 1),
  );
  await delay(20);
  assert.equal(host.count(Op.EngineAction), 0);
  const action = decodeEngineAction((await host.next(Op.EngineAction)).payload);
  assert.equal(action.kind, EngineActionKind.ApplyTurnStart);
  assert.ok(performance.now() - startedAt >= 35);
});

test('pre-existing bootstrap release file is rejected before listen', async (t) => {
  const temporary = await mkdtemp(path.join(os.tmpdir(), 'd2mss-v8-preexisting-'));
  const releaseFile = path.join(temporary, 'bootstrap.release');
  await writeFile(releaseFile, 'too early');
  t.after(async () => rm(temporary, {recursive: true, force: true}));
  const server = new SimTurnsRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    bootstrapReleaseFile: releaseFile,
    logger: () => {},
  });
  await assert.rejects(() => server.listen(), /must not exist at startup/);
  await server.close();
});

test('invalid client evidence faults both live named-pipe peers', async (t) => {
  const session = await openReadyServer(t);
  session.join.send(
    Op.EndTurnObserved,
    encodeEndTurnObserved({epoch: EPOCH + 1, lease: session.plan.joinLease}),
  );
  assert.match(decodeError((await session.host.next(Op.Error)).payload), /epoch mismatch/);
  assert.match(decodeError((await session.join.next(Op.Error)).payload), /epoch mismatch/);
  assert.equal(session.server.coordinator.snapshot().phase, SessionPhase.Faulted);
});

test('pipe and bootstrap path validation remain platform-exact', () => {
  if (process.platform === 'win32') {
    assert.equal(
      resolvePipeName(String.raw`\\.\pipe\d2mss.simturns.v8.custom`),
      String.raw`\\.\pipe\d2mss.simturns.v8.custom`,
    );
    assert.throws(() => resolvePipeName('relative'), /pipe name must match/);
  } else {
    assert.equal(resolvePipeName('/tmp/d2mss.sock'), '/tmp/d2mss.sock');
    assert.throws(() => resolvePipeName('relative'), /must be absolute/);
  }
  assert.throws(
    () => resolveBootstrapReleaseFile('relative.release'),
    /must be an absolute path/,
  );
});

test('server generates a non-zero epoch when none is supplied', () => {
  const server = new SimTurnsRelayServer({
    pipeName: uniquePipeName(),
    logger: () => {},
  });
  assert.ok(server.coordinator.epoch > 0);
  assert.ok(server.coordinator.epoch <= 0xffffffff);
  assert.equal(server.coordinator.mergeDay, 0);
  void server.close();
});

test('premature release-file creation faults once and never emits SessionPlan', async (t) => {
  const temporary = await mkdtemp(path.join(os.tmpdir(), 'd2mss-v8-premature-'));
  const releaseFile = path.join(temporary, 'bootstrap.release');
  t.after(async () => rm(temporary, {recursive: true, force: true}));
  const server = await startRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    bootstrapReleaseFile: releaseFile,
    logger: () => {},
  });
  const host = await TestClient.connect(server.pipeName);
  const join = await TestClient.connect(server.pipeName);
  t.after(async () => {
    host.destroy();
    join.destroy();
    await server.close();
  });
  host.send(Op.Hello, encodeHello({pid: 1, roleHint: RoleHint.Host}));
  join.send(Op.Hello, encodeHello({pid: 2, roleHint: RoleHint.Join}));
  await host.next(Op.HelloAck);
  await join.next(Op.HelloAck);
  await writeFile(releaseFile, 'too early');
  assert.match(decodeError((await host.next(Op.Error)).payload), /both exact handles/);
  assert.match(decodeError((await join.next(Op.Error)).payload), /both exact handles/);
  assert.equal(server.coordinator.snapshot().phase, SessionPhase.Faulted);
  host.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  join.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
  await delay(20);
  assert.equal(host.count(Op.SessionPlan), 0);
  assert.equal(join.count(Op.SessionPlan), 0);
});

test('server close removes an unconsumed release-file watcher', async (t) => {
  const temporary = await mkdtemp(path.join(os.tmpdir(), 'd2mss-v8-close-gate-'));
  const releaseFile = path.join(temporary, 'bootstrap.release');
  t.after(async () => rm(temporary, {recursive: true, force: true}));
  const server = await startRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    bootstrapReleaseFile: releaseFile,
    logger: () => {},
  });
  assert.notEqual(server.bootstrapReleaseWatcher, null);
  await server.close();
  assert.equal(server.bootstrapReleaseWatcher, null);
  await writeFile(releaseFile, 'after close');
  await delay(20);
  assert.equal(server.coordinator.snapshot().phase, SessionPhase.Closed);
});

test('named-pipe end-turn barrier times out for either missing fact', async (t) => {
  for (const missing of ['observed', 'applied']) {
    await t.test(`missing ${missing}`, async (st) => {
      const session = await openReadyServer(st, {endTurnSignalTimeoutMs: 15});
      if (missing === 'applied') {
        session.join.send(
          Op.EndTurnObserved,
          encodeEndTurnObserved({epoch: EPOCH, lease: session.plan.joinLease}),
        );
      } else {
        session.host.send(
          Op.EndTurnApplied,
          encodeEndTurnApplied({epoch: EPOCH, lease: session.plan.joinLease}),
        );
      }
      const error = decodeError((await session.join.next(Op.Error)).payload);
      assert.match(error, new RegExp(`missing EndTurn${missing === 'applied' ? 'Applied' : 'Observed'}`));
      assert.equal(session.server.coordinator.snapshot().phase, SessionPhase.Faulted);
    });
  }
});

test('named-pipe end-turn evidence rejects duplicate, reporter, and lease violations', async (t) => {
  await t.test('duplicate observed', async (st) => {
    const session = await openReadyServer(st);
    const payload = encodeEndTurnObserved({epoch: EPOCH, lease: session.plan.joinLease});
    session.join.send(Op.EndTurnObserved, payload);
    session.join.send(Op.EndTurnObserved, payload);
    assert.match(decodeError((await session.host.next(Op.Error)).payload), /duplicate/);
  });
  await t.test('applied from join', async (st) => {
    const session = await openReadyServer(st);
    session.join.send(
      Op.EndTurnApplied,
      encodeEndTurnApplied({epoch: EPOCH, lease: session.plan.joinLease}),
    );
    assert.match(decodeError((await session.host.next(Op.Error)).payload), /only from host/);
  });
  await t.test('observed with other player lease', async (st) => {
    const session = await openReadyServer(st);
    session.join.send(
      Op.EndTurnObserved,
      encodeEndTurnObserved({epoch: EPOCH, lease: session.plan.hostLease}),
    );
    assert.match(decodeError((await session.host.next(Op.Error)).payload), /lease mismatch/);
  });
});

test('named-pipe simultaneous peer pairs serialize through host actions', async (t) => {
  const session = await openReadyServer(t);
  session.host.send(
    Op.EndTurnObserved,
    encodeEndTurnObserved({epoch: EPOCH, lease: session.plan.hostLease}),
  );
  session.join.send(
    Op.EndTurnObserved,
    encodeEndTurnObserved({epoch: EPOCH, lease: session.plan.joinLease}),
  );
  session.host.send(
    Op.EndTurnApplied,
    encodeEndTurnApplied({epoch: EPOCH, lease: session.plan.joinLease}),
  );
  session.host.send(
    Op.EndTurnApplied,
    encodeEndTurnApplied({epoch: EPOCH, lease: session.plan.hostLease}),
  );
  const firstApply = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(firstApply.kind, EngineActionKind.ApplyTurnStart);
  assert.equal(firstApply.playerHandle, JOIN_HANDLE);
  await delay(10);
  assert.equal(session.server.coordinator.snapshot().queueLength, 1);
  sendResult(session.host, firstApply);
  const firstActivate = decodeEngineAction((await session.join.next(Op.EngineAction)).payload);
  assert.equal(firstActivate.actionId, firstApply.actionId);
  sendResult(session.join, firstActivate);
  const secondApply = decodeEngineAction((await session.host.next(Op.EngineAction)).payload);
  assert.equal(secondApply.kind, EngineActionKind.ApplyTurnStart);
  assert.equal(secondApply.playerHandle, HOST_HANDLE);
  assert.notEqual(secondApply.actionId, firstApply.actionId);
});

test('named-pipe merge rejects wrong tuple, duplicate prepare, and native failure', async (t) => {
  await t.test('wrong prepare kind', async (st) => {
    const session = await openReadyServer(st, {mergeDay: 2});
    const {joinPrepare} = await reachPreparingMerge(session);
    session.join.send(
      Op.ActionResult,
      encodeActionResult({
        epoch: EPOCH,
        actionId: joinPrepare.actionId,
        kind: EngineActionKind.ExecuteMerge,
        success: true,
      }),
    );
    assert.match(decodeError((await session.host.next(Op.Error)).payload), /unexpected ExecuteMerge/);
  });
  await t.test('duplicate prepare result', async (st) => {
    const session = await openReadyServer(st, {mergeDay: 2});
    const {joinPrepare} = await reachPreparingMerge(session);
    sendResult(session.join, joinPrepare);
    sendResult(session.join, joinPrepare);
    assert.match(decodeError((await session.host.next(Op.Error)).payload), /duplicate PrepareMerge/);
  });
  await t.test('failed execute result', async (st) => {
    const session = await openReadyServer(st, {mergeDay: 2});
    const {execute} = await reachExecutingMerge(session);
    sendResult(session.host, execute, false);
    assert.match(decodeError((await session.join.next(Op.Error)).payload), /failed ExecuteMerge/);
  });
});

test('named-pipe non-zero flags fault an active session', async (t) => {
  const session = await openReadyServer(t);
  session.join.socket.write(encodeFrame(Op.EndTurnObserved, Buffer.alloc(8), 1));
  assert.match(decodeError((await session.host.next(Op.Error)).payload), /flags must be 0/);
  assert.equal(session.server.coordinator.snapshot().phase, SessionPhase.Faulted);
});

test('rejected duplicate role does not poison a waiting valid pair', async (t) => {
  const server = await startRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    mergeDay: 4,
    logger: () => {},
  });
  const host = await TestClient.connect(server.pipeName);
  const duplicate = await TestClient.connect(server.pipeName);
  const join = await TestClient.connect(server.pipeName);
  t.after(async () => {
    host.destroy();
    duplicate.destroy();
    join.destroy();
    await server.close();
  });
  host.send(Op.Hello, encodeHello({pid: 1, roleHint: RoleHint.Host}));
  await host.next(Op.HelloAck);
  duplicate.send(Op.Hello, encodeHello({pid: 2, roleHint: RoleHint.Host}));
  assert.equal(decodeHelloAck((await duplicate.next(Op.HelloAck)).payload).accepted, 0);
  assert.match(decodeError((await duplicate.next(Op.Error)).payload), /already connected/);
  join.send(Op.Hello, encodeHello({pid: 3, roleHint: RoleHint.Join}));
  assert.equal(decodeHelloAck((await join.next(Op.HelloAck)).payload).accepted, 1);
  host.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  join.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE));
  assert.equal(decodeSessionPlan((await host.next(Op.SessionPlan)).payload).mergeDay, 4);
});

test('named-pipe disconnect faults the survivor without stock fallback', async (t) => {
  const session = await openReadyServer(t);
  session.join.destroy();
  assert.match(
    decodeError((await session.host.next(Op.Error)).payload),
    /disconnected during active session/,
  );
  assert.equal(session.server.coordinator.snapshot().phase, SessionPhase.Faulted);
  assert.equal(session.host.received
    .filter(({op}) => op === Op.EngineAction)
    .map(({payload}) => decodeEngineAction(payload))
    .filter(({kind}) => kind === EngineActionKind.ReleaseStock).length, 0);
});

test('named-pipe decoder accepts fragmented and coalesced client frames', async (t) => {
  const server = await startRelayServer({
    pipeName: uniquePipeName(),
    epoch: EPOCH,
    mergeDay: 3,
    logger: () => {},
  });
  const host = await TestClient.connect(server.pipeName);
  const join = await TestClient.connect(server.pipeName);
  t.after(async () => {
    host.destroy();
    join.destroy();
    await server.close();
  });

  const hostHello = encodeFrame(
    Op.Hello,
    encodeHello({pid: 1, roleHint: RoleHint.Host}),
  );
  host.socket.write(hostHello.subarray(0, 3));
  await delay(5);
  host.socket.write(hostHello.subarray(3));
  assert.equal(decodeHelloAck((await host.next(Op.HelloAck)).payload).accepted, 1);

  const joinFrames = Buffer.concat([
    encodeFrame(Op.Hello, encodeHello({pid: 2, roleHint: RoleHint.Join})),
    encodeFrame(Op.LocalPlayerHandle, encodeLocalPlayerHandle(JOIN_HANDLE)),
  ]);
  join.socket.write(joinFrames);
  assert.equal(decodeHelloAck((await join.next(Op.HelloAck)).payload).accepted, 1);
  host.send(Op.LocalPlayerHandle, encodeLocalPlayerHandle(HOST_HANDLE));
  const plan = decodeSessionPlan((await host.next(Op.SessionPlan)).payload);
  assert.equal(plan.mergeDay, 3);
  assert.equal(plan.hostHandle, HOST_HANDLE);
  assert.equal(plan.joinHandle, JOIN_HANDLE);
});
