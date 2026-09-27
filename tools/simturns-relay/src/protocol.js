export const PROTOCOL_VERSION = 8;
export const DEFAULT_PIPE_NAME = String.raw`\\.\pipe\d2mss.simturns.v8`;
export const DEFAULT_MAX_FRAME_LENGTH = 64 * 1024;
// Native CScenarioInfo stores a day in a signed 32-bit integer even though
// the wire field is u32. Keep every engine-facing day inside that domain.
export const MAX_ENGINE_DAY = 0x7fffffff;

export const Op = Object.freeze({
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

export const RoleHint = Object.freeze({Unknown: 0, Host: 1, Join: 2});
export const SessionMode = Object.freeze({Stock: 0, Simultaneous: 1});
export const EngineActionKind = Object.freeze({
  ApplyTurnStart: 1,
  ActivateTurn: 2,
  HoldInput: 3,
  PrepareMerge: 4,
  ExecuteMerge: 5,
  ReleaseStock: 6,
});

const opNames = new Map(Object.entries(Op).map(([name, value]) => [value, name]));
const roleHints = new Set(Object.values(RoleHint));
const sessionModes = new Set(Object.values(SessionMode));
const engineActionKinds = new Set(Object.values(EngineActionKind));

export class ProtocolError extends Error {
  constructor(message) {
    super(message);
    this.name = 'ProtocolError';
  }
}

export function opName(op) {
  return opNames.get(op) ?? `Unknown(0x${Number(op).toString(16).padStart(4, '0')})`;
}

function requireBuffer(payload, label = 'payload') {
  if (!Buffer.isBuffer(payload)) throw new TypeError(`${label} must be a Buffer`);
  return payload;
}

function requireLength(payload, expected, label) {
  requireBuffer(payload, label);
  if (payload.length !== expected) {
    throw new ProtocolError(`${label} must be ${expected} bytes, got ${payload.length}`);
  }
}

function requireU32(value, label) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError(`${label} must be an unsigned 32-bit integer`);
  }
  return value;
}

function requireNonZeroU32(value, label) {
  requireU32(value, label);
  if (value === 0) throw new RangeError(`${label} must be non-zero`);
  return value;
}

function requireEngineDay(value, label) {
  requireNonZeroU32(value, label);
  if (value > MAX_ENGINE_DAY) {
    throw new RangeError(`${label} must not exceed ${MAX_ENGINE_DAY}`);
  }
  return value;
}

function requireMergeDay(value, label) {
  requireU32(value, label);
  if (value === 1 || value > MAX_ENGINE_DAY) {
    throw new RangeError(
      `${label} must be 0 (disabled) or an integer from 2 to ${MAX_ENGINE_DAY}`,
    );
  }
  return value;
}

function requireKnown(value, allowed, label) {
  requireU32(value, label);
  if (!allowed.has(value)) throw new RangeError(`${label} is not recognized: ${value}`);
  return value;
}

function decodeKnown(value, allowed, label) {
  if (!allowed.has(value)) throw new ProtocolError(`${label} is not recognized: ${value}`);
  return value;
}

function normalizeSuccess(success) {
  if (success === true || success === 1) return 1;
  if (success === false || success === 0) return 0;
  throw new RangeError('success must be boolean, 0, or 1');
}

function decodeSuccess(value, label) {
  if (value !== 0 && value !== 1) {
    throw new ProtocolError(`${label} success must be 0 or 1, got ${value}`);
  }
  return value === 1;
}

export function encodeFrame(op, payload = Buffer.alloc(0), flags = 0) {
  requireU32(op, 'op');
  requireU32(flags, 'flags');
  if (op > 0xffff || flags > 0xffff) {
    throw new RangeError('op and flags must fit in unsigned 16-bit fields');
  }
  requireBuffer(payload);

  const length = 4 + payload.length;
  const frame = Buffer.allocUnsafe(4 + length);
  frame.writeUInt32LE(length, 0);
  frame.writeUInt16LE(op, 4);
  frame.writeUInt16LE(flags, 6);
  payload.copy(frame, 8);
  return frame;
}

export class FrameDecoder {
  constructor({maxFrameLength = DEFAULT_MAX_FRAME_LENGTH} = {}) {
    if (!Number.isInteger(maxFrameLength) || maxFrameLength < 4) {
      throw new RangeError('maxFrameLength must be an integer >= 4');
    }
    this.maxFrameLength = maxFrameLength;
    this.buffer = Buffer.alloc(0);
  }

  push(chunk) {
    requireBuffer(chunk, 'chunk');
    if (chunk.length === 0) return [];
    this.buffer = this.buffer.length === 0
      ? Buffer.from(chunk)
      : Buffer.concat([this.buffer, chunk]);

    const frames = [];
    while (this.buffer.length >= 4) {
      const length = this.buffer.readUInt32LE(0);
      if (length < 4) {
        this.buffer = Buffer.alloc(0);
        throw new ProtocolError(`frame length must be >= 4, got ${length}`);
      }
      if (length > this.maxFrameLength) {
        this.buffer = Buffer.alloc(0);
        throw new ProtocolError(
          `frame length ${length} exceeds limit ${this.maxFrameLength}`,
        );
      }
      const total = 4 + length;
      if (this.buffer.length < total) break;
      frames.push({
        op: this.buffer.readUInt16LE(4),
        flags: this.buffer.readUInt16LE(6),
        payload: Buffer.from(this.buffer.subarray(8, total)),
      });
      this.buffer = this.buffer.subarray(total);
    }
    return frames;
  }
}

export function encodeHello({
  version = PROTOCOL_VERSION,
  pid,
  roleHint = RoleHint.Unknown,
}) {
  const payload = Buffer.allocUnsafe(12);
  payload.writeUInt32LE(requireU32(version, 'version'), 0);
  payload.writeUInt32LE(requireU32(pid, 'pid'), 4);
  payload.writeUInt32LE(requireKnown(roleHint, roleHints, 'roleHint'), 8);
  return payload;
}

export function decodeHello(payload) {
  requireLength(payload, 12, 'Hello payload');
  return {
    version: payload.readUInt32LE(0),
    pid: payload.readUInt32LE(4),
    roleHint: decodeKnown(payload.readUInt32LE(8), roleHints, 'Hello roleHint'),
  };
}

export function encodeHelloAck(accepted, version = PROTOCOL_VERSION) {
  if (accepted !== 0 && accepted !== 1 && accepted !== false && accepted !== true) {
    throw new RangeError('accepted must be boolean, 0, or 1');
  }
  const payload = Buffer.allocUnsafe(8);
  payload.writeUInt32LE(accepted ? 1 : 0, 0);
  payload.writeUInt32LE(requireU32(version, 'version'), 4);
  return payload;
}

export function decodeHelloAck(payload) {
  requireLength(payload, 8, 'HelloAck payload');
  const accepted = payload.readUInt32LE(0);
  if (accepted > 1) {
    throw new ProtocolError(`HelloAck accepted must be 0 or 1, got ${accepted}`);
  }
  return {accepted, version: payload.readUInt32LE(4)};
}

export function encodeLocalPlayerHandle(handle) {
  const payload = Buffer.allocUnsafe(4);
  payload.writeUInt32LE(requireU32(handle, 'handle'), 0);
  return payload;
}

export function decodeLocalPlayerHandle(payload) {
  requireLength(payload, 4, 'LocalPlayerHandle payload');
  return {handle: payload.readUInt32LE(0)};
}

export function encodeSessionPlan({
  epoch,
  mode = SessionMode.Simultaneous,
  hostHandle,
  joinHandle,
  mergeDay,
  hostLease,
  joinLease,
}) {
  requireNonZeroU32(epoch, 'epoch');
  requireKnown(mode, sessionModes, 'mode');
  requireNonZeroU32(hostHandle, 'hostHandle');
  requireNonZeroU32(joinHandle, 'joinHandle');
  requireMergeDay(mergeDay, 'mergeDay');
  requireU32(hostLease, 'hostLease');
  requireU32(joinLease, 'joinLease');
  if (hostHandle === joinHandle) throw new RangeError('player handles must be distinct');
  if (mode === SessionMode.Stock) {
    if (mergeDay !== 0 || hostLease !== 0 || joinLease !== 0) {
      throw new RangeError(
        'Stock SessionPlan requires mergeDay, hostLease, and joinLease to be 0',
      );
    }
  } else if (hostLease === 0 || joinLease === 0 || hostLease === joinLease) {
    throw new RangeError(
      'Simultaneous SessionPlan requires non-zero distinct player leases',
    );
  }

  const payload = Buffer.allocUnsafe(28);
  payload.writeUInt32LE(epoch, 0);
  payload.writeUInt32LE(mode, 4);
  payload.writeUInt32LE(hostHandle, 8);
  payload.writeUInt32LE(joinHandle, 12);
  payload.writeUInt32LE(mergeDay, 16);
  payload.writeUInt32LE(hostLease, 20);
  payload.writeUInt32LE(joinLease, 24);
  return payload;
}

export function decodeSessionPlan(payload) {
  requireLength(payload, 28, 'SessionPlan payload');
  const plan = {
    epoch: payload.readUInt32LE(0),
    mode: decodeKnown(payload.readUInt32LE(4), sessionModes, 'SessionPlan mode'),
    hostHandle: payload.readUInt32LE(8),
    joinHandle: payload.readUInt32LE(12),
    mergeDay: payload.readUInt32LE(16),
    hostLease: payload.readUInt32LE(20),
    joinLease: payload.readUInt32LE(24),
  };
  if (plan.epoch === 0) throw new ProtocolError('SessionPlan epoch must be non-zero');
  if (plan.hostHandle === 0 || plan.joinHandle === 0) {
    throw new ProtocolError('SessionPlan player handles must be non-zero');
  }
  if (plan.hostHandle === plan.joinHandle) {
    throw new ProtocolError('SessionPlan player handles must be distinct');
  }
  if (plan.mergeDay === 1 || plan.mergeDay > MAX_ENGINE_DAY) {
    throw new ProtocolError(
      `SessionPlan mergeDay must be 0 (disabled) or an integer from 2 to ` +
      `${MAX_ENGINE_DAY}`,
    );
  }
  if (plan.mode === SessionMode.Stock) {
    if (plan.mergeDay !== 0 || plan.hostLease !== 0 || plan.joinLease !== 0) {
      throw new ProtocolError(
        'Stock SessionPlan requires mergeDay, hostLease, and joinLease to be 0',
      );
    }
  } else if (plan.hostLease === 0 || plan.joinLease === 0 ||
             plan.hostLease === plan.joinLease) {
    throw new ProtocolError(
      'Simultaneous SessionPlan requires non-zero distinct player leases',
    );
  }
  return plan;
}

export function encodeBootstrapProgress(handle, day) {
  if (handle === 0) throw new RangeError('bootstrap handle must be non-zero');
  if (day !== 1) throw new RangeError('bootstrap day must be exactly 1');
  const payload = Buffer.allocUnsafe(8);
  payload.writeUInt32LE(requireU32(handle, 'handle'), 0);
  payload.writeUInt32LE(day, 4);
  return payload;
}

export function decodeBootstrapProgress(payload, label = 'bootstrap progress') {
  requireLength(payload, 8, `${label} payload`);
  const handle = payload.readUInt32LE(0);
  const day = payload.readUInt32LE(4);
  if (handle === 0 || day !== 1) {
    throw new ProtocolError(`${label} requires a non-zero join handle at day 1`);
  }
  return {handle, day};
}

export function encodeBootstrapReleased(handle, day) {
  return encodeBootstrapProgress(handle, day);
}

export function decodeBootstrapReleased(payload) {
  return decodeBootstrapProgress(payload, 'BootstrapReleased');
}

function encodeEndTurn({epoch, lease}, label) {
  const payload = Buffer.allocUnsafe(8);
  payload.writeUInt32LE(requireNonZeroU32(epoch, `${label} epoch`), 0);
  payload.writeUInt32LE(requireNonZeroU32(lease, `${label} lease`), 4);
  return payload;
}

function decodeEndTurn(payload, label) {
  requireLength(payload, 8, `${label} payload`);
  const epoch = payload.readUInt32LE(0);
  const lease = payload.readUInt32LE(4);
  if (epoch === 0 || lease === 0) {
    throw new ProtocolError(`${label} requires a non-zero epoch and lease`);
  }
  return {epoch, lease};
}

export function encodeEndTurnObserved(message) {
  return encodeEndTurn(message, 'EndTurnObserved');
}

export function decodeEndTurnObserved(payload) {
  return decodeEndTurn(payload, 'EndTurnObserved');
}

export function encodeEndTurnApplied(message) {
  return encodeEndTurn(message, 'EndTurnApplied');
}

export function decodeEndTurnApplied(payload) {
  return decodeEndTurn(payload, 'EndTurnApplied');
}

export function encodeEngineAction({
  epoch,
  actionId,
  kind,
  playerHandle,
  day,
  lease = 0,
}) {
  requireKnown(kind, engineActionKinds, 'EngineAction kind');
  requireU32(lease, 'EngineAction lease');
  const carriesLease = kind === EngineActionKind.ApplyTurnStart ||
    kind === EngineActionKind.ActivateTurn;
  if (carriesLease ? lease === 0 : lease !== 0) {
    throw new RangeError(
      carriesLease
        ? 'ApplyTurnStart and ActivateTurn require a non-zero lease'
        : 'HoldInput and merge actions require lease 0',
    );
  }
  const payload = Buffer.allocUnsafe(24);
  payload.writeUInt32LE(requireNonZeroU32(epoch, 'EngineAction epoch'), 0);
  payload.writeUInt32LE(requireNonZeroU32(actionId, 'EngineAction actionId'), 4);
  payload.writeUInt32LE(kind, 8);
  payload.writeUInt32LE(
    requireNonZeroU32(playerHandle, 'EngineAction playerHandle'),
    12,
  );
  payload.writeUInt32LE(requireEngineDay(day, 'EngineAction day'), 16);
  payload.writeUInt32LE(lease, 20);
  return payload;
}

export function decodeEngineAction(payload) {
  requireLength(payload, 24, 'EngineAction payload');
  const action = {
    epoch: payload.readUInt32LE(0),
    actionId: payload.readUInt32LE(4),
    kind: decodeKnown(
      payload.readUInt32LE(8),
      engineActionKinds,
      'EngineAction kind',
    ),
    playerHandle: payload.readUInt32LE(12),
    day: payload.readUInt32LE(16),
    lease: payload.readUInt32LE(20),
  };
  if (action.epoch === 0 || action.actionId === 0 ||
      action.playerHandle === 0 || action.day === 0) {
    throw new ProtocolError(
      'EngineAction requires a non-zero epoch, actionId, playerHandle, and day',
    );
  }
  if (action.day > MAX_ENGINE_DAY) {
    throw new ProtocolError(
      `EngineAction day must not exceed ${MAX_ENGINE_DAY}`,
    );
  }
  const carriesLease = action.kind === EngineActionKind.ApplyTurnStart ||
    action.kind === EngineActionKind.ActivateTurn;
  if (carriesLease ? action.lease === 0 : action.lease !== 0) {
    throw new ProtocolError(
      carriesLease
        ? 'ApplyTurnStart and ActivateTurn require a non-zero lease'
        : 'HoldInput and merge actions require lease 0',
    );
  }
  return action;
}

export function encodeActionResult({epoch, actionId, kind, success}) {
  const payload = Buffer.allocUnsafe(16);
  payload.writeUInt32LE(requireNonZeroU32(epoch, 'ActionResult epoch'), 0);
  payload.writeUInt32LE(requireNonZeroU32(actionId, 'ActionResult actionId'), 4);
  payload.writeUInt32LE(requireKnown(kind, engineActionKinds, 'ActionResult kind'), 8);
  payload.writeUInt32LE(normalizeSuccess(success), 12);
  return payload;
}

export function decodeActionResult(payload) {
  requireLength(payload, 16, 'ActionResult payload');
  const epoch = payload.readUInt32LE(0);
  const actionId = payload.readUInt32LE(4);
  if (epoch === 0 || actionId === 0) {
    throw new ProtocolError('ActionResult requires a non-zero epoch and actionId');
  }
  return {
    epoch,
    actionId,
    kind: decodeKnown(
      payload.readUInt32LE(8),
      engineActionKinds,
      'ActionResult kind',
    ),
    success: decodeSuccess(payload.readUInt32LE(12), 'ActionResult'),
  };
}

export function encodeMergeApplied({epoch, actionId}) {
  const payload = Buffer.allocUnsafe(8);
  payload.writeUInt32LE(requireNonZeroU32(epoch, 'MergeApplied epoch'), 0);
  payload.writeUInt32LE(requireNonZeroU32(actionId, 'MergeApplied actionId'), 4);
  return payload;
}

export function decodeMergeApplied(payload) {
  requireLength(payload, 8, 'MergeApplied payload');
  const epoch = payload.readUInt32LE(0);
  const actionId = payload.readUInt32LE(4);
  if (epoch === 0 || actionId === 0) {
    throw new ProtocolError('MergeApplied requires a non-zero epoch and actionId');
  }
  return {epoch, actionId};
}

export function encodeError(message) {
  const payload = Buffer.from(String(message), 'utf8');
  if (payload.length <= 4096) return payload;

  // Keep the size bound without cutting a UTF-8 code point in half.
  let end = 4096;
  while (end > 0 && (payload[end] & 0xc0) === 0x80) end -= 1;
  return payload.subarray(0, end);
}

export function decodeError(payload) {
  requireBuffer(payload, 'Error payload');
  return payload.toString('utf8');
}

export function assertEmptyPayload(payload, label) {
  requireLength(payload, 0, `${label} payload`);
}
