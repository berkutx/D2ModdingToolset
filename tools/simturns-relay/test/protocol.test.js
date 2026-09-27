import assert from 'node:assert/strict';
import test from 'node:test';
import * as protocol from '../src/protocol.js';
import {
  DEFAULT_PIPE_NAME,
  EngineActionKind,
  FrameDecoder,
  MAX_ENGINE_DAY,
  Op,
  PROTOCOL_VERSION,
  ProtocolError,
  RoleHint,
  SessionMode,
  decodeActionResult,
  decodeBootstrapProgress,
  decodeBootstrapReleased,
  decodeEndTurnApplied,
  decodeEndTurnObserved,
  decodeEngineAction,
  decodeHello,
  decodeHelloAck,
  decodeLocalPlayerHandle,
  decodeMergeApplied,
  decodeSessionPlan,
  encodeActionResult,
  encodeBootstrapProgress,
  encodeBootstrapReleased,
  encodeEndTurnApplied,
  encodeEndTurnObserved,
  encodeEngineAction,
  encodeError,
  encodeFrame,
  encodeHello,
  encodeHelloAck,
  encodeLocalPlayerHandle,
  encodeMergeApplied,
  encodeSessionPlan,
  opName,
} from '../src/protocol.js';

const HOST_HANDLE = 0xa3de0001;
const JOIN_HANDLE = 0xa3de0002;

function writeU32s(values) {
  const payload = Buffer.alloc(values.length * 4);
  values.forEach((value, index) => payload.writeUInt32LE(value, index * 4));
  return payload;
}

test('protocol v8 pins the pipe, compact opcodes, and action kinds', () => {
  assert.equal(PROTOCOL_VERSION, 8);
  assert.equal(MAX_ENGINE_DAY, 0x7fffffff);
  assert.equal(DEFAULT_PIPE_NAME, String.raw`\\.\pipe\d2mss.simturns.v8`);
  assert.deepEqual(Op, {
    Hello: 0x0001,
    HelloAck: 0x0002,
    Goodbye: 0x0003,
    LocalPlayerHandle: 0x0007,
    SessionPlan: 0x0008,
    SessionActivated: 0x000b,
    BootstrapBeginTurnApplied: 0x000c,
    BootstrapComplete: 0x000d,
    BootstrapCommitted: 0x000e,
    BootstrapCommitApplied: 0x000f,
    BootstrapOperational: 0x0010,
    BootstrapOperationalApplied: 0x0011,
    BootstrapReleased: 0x0015,
    EndTurnObserved: 0x1000,
    EngineAction: 0x1001,
    ActionResult: 0x1002,
    EndTurnApplied: 0x1007,
    MergeApplied: 0x1008,
    Error: 0x10ff,
  });
  assert.deepEqual(RoleHint, {Unknown: 0, Host: 1, Join: 2});
  assert.deepEqual(SessionMode, {Stock: 0, Simultaneous: 1});
  assert.deepEqual(EngineActionKind, {
    ApplyTurnStart: 1,
    ActivateTurn: 2,
    HoldInput: 3,
    PrepareMerge: 4,
    ExecuteMerge: 5,
    ReleaseStock: 6,
  });
  assert.equal(opName(Op.EngineAction), 'EngineAction');
  assert.equal(opName(0x7ffe), 'Unknown(0x7ffe)');
});

test('v8 no longer exposes client-owned calendar and multi-phase merge messages', () => {
  for (const name of [
    'SessionReady',
    'SubjectiveEndTurnIssued',
    'SubjectiveEndTurnApplied',
    'DispatchCascade',
    'CascadeResult',
    'BeginTurn',
    'EnterObserver',
    'MergeNow',
    'MergeReady',
    'HostMergeApplied',
    'MergeCommitted',
    'MergeCommitApplied',
    'MergeOperational',
    'MergeOperationalApplied',
    'MergeReleased',
  ]) {
    assert.equal(Op[name], undefined, name);
  }

  for (const name of [
    'encodeSessionReady',
    'encodeSubjectiveEndTurnSignal',
    'encodeDispatchCascade',
    'encodeCascadeResult',
    'encodeBeginTurn',
    'encodeEnterObserver',
    'encodeMergeNow',
    'encodeMergeReady',
    'encodeHostMergeApplied',
    'encodeMergeCommitted',
    'encodeMergeCommitApplied',
    'encodeMergeOperational',
    'encodeMergeOperationalApplied',
    'encodeMergeReleased',
  ]) {
    assert.equal(protocol[name], undefined, name);
  }
});

test('frame is little-endian and length counts op, flags, and payload', () => {
  const frame = encodeFrame(0x1234, Buffer.from([0xaa, 0xbb]), 0x5678);
  assert.deepEqual(
    [...frame],
    [0x06, 0x00, 0x00, 0x00, 0x34, 0x12, 0x78, 0x56, 0xaa, 0xbb],
  );
});

test('decoder handles fragmented and coalesced v8 frames', () => {
  const first = encodeFrame(Op.HelloAck, encodeHelloAck(1));
  const second = encodeFrame(Op.EngineAction, encodeEngineAction({
    epoch: 4,
    actionId: 9,
    kind: EngineActionKind.ApplyTurnStart,
    playerHandle: JOIN_HANDLE,
    day: 3,
    lease: 11,
  }));
  const combined = Buffer.concat([first, second]);
  const decoder = new FrameDecoder();

  assert.deepEqual(decoder.push(combined.subarray(0, 3)), []);
  assert.deepEqual(decoder.push(combined.subarray(3, 11)), []);
  const frames = decoder.push(combined.subarray(11));
  assert.equal(frames.length, 2);
  assert.equal(frames[0].op, Op.HelloAck);
  assert.deepEqual(
    decodeHelloAck(frames[0].payload),
    {accepted: 1, version: PROTOCOL_VERSION},
  );
  assert.equal(frames[1].op, Op.EngineAction);
  assert.deepEqual(decodeEngineAction(frames[1].payload), {
    epoch: 4,
    actionId: 9,
    kind: EngineActionKind.ApplyTurnStart,
    playerHandle: JOIN_HANDLE,
    day: 3,
    lease: 11,
  });
});

test('decoder rejects malformed and oversized lengths', () => {
  const tooShort = Buffer.alloc(4);
  tooShort.writeUInt32LE(3, 0);
  assert.throws(() => new FrameDecoder().push(tooShort), ProtocolError);

  const tooLarge = Buffer.alloc(4);
  tooLarge.writeUInt32LE(65, 0);
  assert.throws(
    () => new FrameDecoder({maxFrameLength: 64}).push(tooLarge),
    /exceeds limit/,
  );
});

test('Hello v8 carries only version, pid, and non-authoritative roleHint', () => {
  const payload = encodeHello({pid: 0x12345678, roleHint: RoleHint.Join});
  assert.equal(payload.length, 12);
  assert.deepEqual(
    [...payload],
    [
      0x08, 0x00, 0x00, 0x00,
      0x78, 0x56, 0x34, 0x12,
      0x02, 0x00, 0x00, 0x00,
    ],
  );
  assert.deepEqual(decodeHello(payload), {
    version: PROTOCOL_VERSION,
    pid: 0x12345678,
    roleHint: RoleHint.Join,
  });
  assert.deepEqual(decodeHello(encodeHello({pid: 7})), {
    version: PROTOCOL_VERSION,
    pid: 7,
    roleHint: RoleHint.Unknown,
  });
});

test('HelloAck accepted is strictly binary', () => {
  assert.deepEqual(decodeHelloAck(encodeHelloAck(true)), {
    accepted: 1,
    version: PROTOCOL_VERSION,
  });
  assert.deepEqual(decodeHelloAck(encodeHelloAck(false)), {
    accepted: 0,
    version: PROTOCOL_VERSION,
  });
  assert.throws(() => encodeHelloAck(2), /boolean, 0, or 1/);
  assert.throws(
    () => decodeHelloAck(writeU32s([2, PROTOCOL_VERSION])),
    /must be 0 or 1/,
  );
});

test('SessionPlan is an exact little-endian 28-byte server-owned plan', () => {
  const plan = {
    epoch: 0x01020304,
    mode: SessionMode.Simultaneous,
    hostHandle: HOST_HANDLE,
    joinHandle: JOIN_HANDLE,
    mergeDay: 5,
    hostLease: 0x11223344,
    joinLease: 0x55667788,
  };
  const payload = encodeSessionPlan(plan);
  assert.equal(payload.length, 28);
  assert.deepEqual(
    [...payload],
    [
      0x04, 0x03, 0x02, 0x01,
      0x01, 0x00, 0x00, 0x00,
      0x01, 0x00, 0xde, 0xa3,
      0x02, 0x00, 0xde, 0xa3,
      0x05, 0x00, 0x00, 0x00,
      0x44, 0x33, 0x22, 0x11,
      0x88, 0x77, 0x66, 0x55,
    ],
  );
  assert.deepEqual(decodeSessionPlan(payload), plan);

  const mergeDisabled = {...plan, mergeDay: 0};
  assert.deepEqual(decodeSessionPlan(encodeSessionPlan(mergeDisabled)), mergeDisabled);

  const stock = {
    ...plan,
    mode: SessionMode.Stock,
    mergeDay: 0,
    hostLease: 0,
    joinLease: 0,
  };
  assert.deepEqual(decodeSessionPlan(encodeSessionPlan(stock)), stock);

  const lastEngineDay = {...plan, mergeDay: MAX_ENGINE_DAY};
  assert.deepEqual(
    decodeSessionPlan(encodeSessionPlan(lastEngineDay)),
    lastEngineDay,
  );
});

test('bootstrap progress and local handle codecs remain stable', () => {
  assert.deepEqual(
    decodeLocalPlayerHandle(encodeLocalPlayerHandle(JOIN_HANDLE)),
    {handle: JOIN_HANDLE},
  );
  assert.deepEqual(
    decodeBootstrapProgress(
      encodeBootstrapProgress(JOIN_HANDLE, 1),
      'BootstrapCommitted',
    ),
    {handle: JOIN_HANDLE, day: 1},
  );
  assert.deepEqual(
    decodeBootstrapReleased(encodeBootstrapReleased(JOIN_HANDLE, 1)),
    {handle: JOIN_HANDLE, day: 1},
  );
});

test('EndTurnObserved and EndTurnApplied share an exact epoch/lease layout', () => {
  const message = {epoch: 0x12345678, lease: 0x90abcdef};
  const observed = encodeEndTurnObserved(message);
  const applied = encodeEndTurnApplied(message);
  assert.equal(observed.length, 8);
  assert.deepEqual([...observed], [
    0x78, 0x56, 0x34, 0x12,
    0xef, 0xcd, 0xab, 0x90,
  ]);
  assert.deepEqual(applied, observed);
  assert.deepEqual(decodeEndTurnObserved(observed), message);
  assert.deepEqual(decodeEndTurnApplied(applied), message);
});

test('all six EngineAction kinds round-trip through one fixed payload', () => {
  for (const kind of Object.values(EngineActionKind)) {
    const action = {
      epoch: 2,
      actionId: 100 + kind,
      kind,
      playerHandle: kind % 2 === 0 ? HOST_HANDLE : JOIN_HANDLE,
      day: kind + 1,
      lease: kind <= EngineActionKind.ActivateTurn ? 0x5000 + kind : 0,
    };
    const payload = encodeEngineAction(action);
    assert.equal(payload.length, 24);
    assert.deepEqual(decodeEngineAction(payload), action);
  }
});

test('EngineAction payload field order is exact and little-endian', () => {
  const payload = encodeEngineAction({
    epoch: 0x01020304,
    actionId: 0x11223344,
    kind: EngineActionKind.ExecuteMerge,
    playerHandle: 0x55667788,
    day: 0x70abcdef,
    lease: 0,
  });
  assert.deepEqual([...payload], [
    0x04, 0x03, 0x02, 0x01,
    0x44, 0x33, 0x22, 0x11,
    0x05, 0x00, 0x00, 0x00,
    0x88, 0x77, 0x66, 0x55,
    0xef, 0xcd, 0xab, 0x70,
    0x00, 0x00, 0x00, 0x00,
  ]);
});

test('EngineAction lease policy matches the native v8 contract', () => {
  const base = {
    epoch: 1,
    actionId: 2,
    playerHandle: JOIN_HANDLE,
    day: 3,
  };
  for (const kind of [
    EngineActionKind.ApplyTurnStart,
    EngineActionKind.ActivateTurn,
  ]) {
    assert.throws(
      () => encodeEngineAction({...base, kind, lease: 0}),
      /require a non-zero lease/,
    );
    assert.throws(
      () => decodeEngineAction(writeU32s([1, 2, kind, JOIN_HANDLE, 3, 0])),
      /require a non-zero lease/,
    );
  }
  for (const kind of [
    EngineActionKind.HoldInput,
    EngineActionKind.PrepareMerge,
    EngineActionKind.ExecuteMerge,
    EngineActionKind.ReleaseStock,
  ]) {
    assert.throws(
      () => encodeEngineAction({...base, kind, lease: 9}),
      /require lease 0/,
    );
    assert.throws(
      () => decodeEngineAction(writeU32s([1, 2, kind, JOIN_HANDLE, 3, 9])),
      /require lease 0/,
    );
  }
});

test('ActionResult preserves action identity, kind, and strict success', () => {
  const message = {
    epoch: 0x01020304,
    actionId: 0x11223344,
    kind: EngineActionKind.PrepareMerge,
    success: true,
  };
  const payload = encodeActionResult(message);
  assert.equal(payload.length, 16);
  assert.deepEqual([...payload], [
    0x04, 0x03, 0x02, 0x01,
    0x44, 0x33, 0x22, 0x11,
    0x04, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
  ]);
  assert.deepEqual(decodeActionResult(payload), message);
  assert.deepEqual(
    decodeActionResult(encodeActionResult({...message, success: 0})),
    {...message, success: false},
  );
});

test('MergeApplied identifies the epoch and merge action only', () => {
  const message = {epoch: 0x12345678, actionId: 0x90abcdef};
  const payload = encodeMergeApplied(message);
  assert.equal(payload.length, 8);
  assert.deepEqual([...payload], [
    0x78, 0x56, 0x34, 0x12,
    0xef, 0xcd, 0xab, 0x90,
  ]);
  assert.deepEqual(decodeMergeApplied(payload), message);
});

test('fixed-size decoders reject every ambiguous payload length', () => {
  const decoders = [
    ['Hello', 12, decodeHello],
    ['HelloAck', 8, decodeHelloAck],
    ['LocalPlayerHandle', 4, decodeLocalPlayerHandle],
    ['SessionPlan', 28, decodeSessionPlan],
    ['BootstrapProgress', 8, decodeBootstrapProgress],
    ['EndTurnObserved', 8, decodeEndTurnObserved],
    ['EndTurnApplied', 8, decodeEndTurnApplied],
    ['EngineAction', 24, decodeEngineAction],
    ['ActionResult', 16, decodeActionResult],
    ['MergeApplied', 8, decodeMergeApplied],
  ];

  for (const [label, expected, decode] of decoders) {
    for (const size of [0, Math.max(0, expected - 1), expected + 1, expected + 8]) {
      assert.throws(
        () => decode(Buffer.alloc(size)),
        new RegExp(`must be ${expected} bytes`),
        `${label}/${size}`,
      );
    }
  }
});

test('enum decoders reject unknown role, mode, and action kind', () => {
  assert.throws(
    () => encodeHello({pid: 1, roleHint: 3}),
    /roleHint is not recognized/,
  );
  assert.throws(
    () => decodeHello(writeU32s([8, 1, 3])),
    /roleHint is not recognized/,
  );

  const plan = [1, 99, HOST_HANDLE, JOIN_HANDLE, 5, 7, 8];
  assert.throws(() => decodeSessionPlan(writeU32s(plan)), /mode is not recognized/);

  const action = [1, 2, 99, HOST_HANDLE, 3, 0];
  assert.throws(
    () => decodeEngineAction(writeU32s(action)),
    /kind is not recognized/,
  );
  assert.throws(
    () => decodeActionResult(writeU32s([1, 2, 99, 1])),
    /kind is not recognized/,
  );
});

test('server-owned identity fields reject zero and invalid plan relationships', () => {
  const validPlan = {
    epoch: 1,
    hostHandle: HOST_HANDLE,
    joinHandle: JOIN_HANDLE,
    mergeDay: 2,
    hostLease: 7,
    joinLease: 8,
  };
  for (const field of ['epoch', 'hostHandle', 'joinHandle']) {
    assert.throws(
      () => encodeSessionPlan({...validPlan, [field]: 0}),
      /must be non-zero/,
      field,
    );
  }
  assert.throws(
    () => encodeSessionPlan({...validPlan, joinHandle: HOST_HANDLE}),
    /handles must be distinct/,
  );
  assert.throws(
    () => encodeSessionPlan({...validPlan, joinLease: validPlan.hostLease}),
    /non-zero distinct player leases/,
  );
  assert.throws(
    () => encodeSessionPlan({...validPlan, hostLease: 0}),
    /non-zero distinct player leases/,
  );
  assert.throws(
    () => encodeSessionPlan({...validPlan, joinLease: 0}),
    /non-zero distinct player leases/,
  );
  assert.throws(
    () => encodeSessionPlan({...validPlan, mergeDay: 1}),
    /mergeDay must be 0 \(disabled\) or an integer from 2/,
  );
  assert.throws(
    () => encodeSessionPlan({...validPlan, mergeDay: MAX_ENGINE_DAY + 1}),
    /mergeDay.*must be 0.*2147483647/,
  );
  assert.throws(
    () => encodeSessionPlan({...validPlan, mode: SessionMode.Stock}),
    /Stock SessionPlan requires mergeDay, hostLease, and joinLease to be 0/,
  );

  for (const values of [
    [0, 1, HOST_HANDLE, JOIN_HANDLE, 2, 7, 8],
    [1, 1, 0, JOIN_HANDLE, 2, 7, 8],
    [1, 1, HOST_HANDLE, 0, 2, 7, 8],
    [1, 1, HOST_HANDLE, HOST_HANDLE, 2, 7, 8],
    [1, 1, HOST_HANDLE, JOIN_HANDLE, 1, 7, 8],
    [1, 1, HOST_HANDLE, JOIN_HANDLE, MAX_ENGINE_DAY + 1, 7, 8],
    [1, 1, HOST_HANDLE, JOIN_HANDLE, 2, 0, 8],
    [1, 1, HOST_HANDLE, JOIN_HANDLE, 2, 7, 0],
    [1, 1, HOST_HANDLE, JOIN_HANDLE, 2, 7, 7],
    [1, 0, HOST_HANDLE, JOIN_HANDLE, 2, 7, 8],
  ]) {
    assert.throws(() => decodeSessionPlan(writeU32s(values)), ProtocolError);
  }
});

test('end-turn, action, result, and merge acknowledgements reject zero identity', () => {
  for (const encode of [encodeEndTurnObserved, encodeEndTurnApplied]) {
    assert.throws(() => encode({epoch: 0, lease: 1}), /epoch must be non-zero/);
    assert.throws(() => encode({epoch: 1, lease: 0}), /lease must be non-zero/);
  }
  for (const decode of [decodeEndTurnObserved, decodeEndTurnApplied]) {
    assert.throws(() => decode(writeU32s([0, 1])), /non-zero epoch and lease/);
    assert.throws(() => decode(writeU32s([1, 0])), /non-zero epoch and lease/);
  }

  const validAction = {
    epoch: 1,
    actionId: 2,
    kind: EngineActionKind.ActivateTurn,
    playerHandle: JOIN_HANDLE,
    day: 3,
    lease: 4,
  };
  for (const field of ['epoch', 'actionId', 'playerHandle', 'day']) {
    assert.throws(
      () => encodeEngineAction({...validAction, [field]: 0}),
      /must be non-zero/,
      field,
    );
  }
  for (const index of [0, 1, 3, 4]) {
    const values = [1, 2, EngineActionKind.ActivateTurn, JOIN_HANDLE, 3, 4];
    values[index] = 0;
    assert.throws(() => decodeEngineAction(writeU32s(values)), /requires a non-zero/);
  }
  assert.deepEqual(
    decodeEngineAction(encodeEngineAction({...validAction, day: MAX_ENGINE_DAY})),
    {...validAction, day: MAX_ENGINE_DAY},
  );
  assert.throws(
    () => encodeEngineAction({...validAction, day: MAX_ENGINE_DAY + 1}),
    /EngineAction day must not exceed 2147483647/,
  );
  assert.throws(
    () => decodeEngineAction(writeU32s([
      1,
      2,
      EngineActionKind.ActivateTurn,
      JOIN_HANDLE,
      MAX_ENGINE_DAY + 1,
      4,
    ])),
    /EngineAction day must not exceed 2147483647/,
  );

  const validResult = {
    epoch: 1,
    actionId: 2,
    kind: EngineActionKind.HoldInput,
    success: true,
  };
  assert.throws(() => encodeActionResult({...validResult, epoch: 0}), /must be non-zero/);
  assert.throws(
    () => encodeActionResult({...validResult, actionId: 0}),
    /must be non-zero/,
  );
  assert.throws(
    () => decodeActionResult(writeU32s([0, 2, validResult.kind, 1])),
    /non-zero epoch and actionId/,
  );
  assert.throws(
    () => decodeActionResult(writeU32s([1, 0, validResult.kind, 1])),
    /non-zero epoch and actionId/,
  );

  assert.throws(() => encodeMergeApplied({epoch: 0, actionId: 2}), /must be non-zero/);
  assert.throws(() => encodeMergeApplied({epoch: 1, actionId: 0}), /must be non-zero/);
  assert.throws(() => decodeMergeApplied(writeU32s([0, 2])), /non-zero epoch/);
  assert.throws(() => decodeMergeApplied(writeU32s([1, 0])), /non-zero epoch/);
});

test('ActionResult success is binary on both encode and decode', () => {
  const base = {epoch: 1, actionId: 2, kind: EngineActionKind.ReleaseStock};
  for (const success of [true, false, 1, 0]) {
    assert.equal(
      decodeActionResult(encodeActionResult({...base, success})).success,
      success === true || success === 1,
    );
  }
  assert.throws(() => encodeActionResult({...base, success: 2}), /boolean, 0, or 1/);
  assert.throws(
    () => decodeActionResult(writeU32s([1, 2, base.kind, 2])),
    /success must be 0 or 1/,
  );
});

test('bootstrap progress validation remains fail-closed', () => {
  assert.throws(
    () => decodeBootstrapProgress(Buffer.alloc(8), 'BootstrapComplete'),
    /non-zero join handle at day 1/,
  );
  assert.throws(() => encodeBootstrapProgress(JOIN_HANDLE, 2), /exactly 1/);
});

test('Error truncation preserves valid UTF-8', () => {
  const payload = encodeError('x'.repeat(4095) + '\u{1f642}');
  assert.equal(payload.length, 4095);
  assert.equal(payload.toString('utf8'), 'x'.repeat(4095));
  assert.equal(payload.includes(Buffer.from([0xef, 0xbf, 0xbd])), false);
});
