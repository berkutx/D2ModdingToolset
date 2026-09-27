import {
  EngineActionKind,
  MAX_ENGINE_DAY,
  Op,
  PROTOCOL_VERSION,
  ProtocolError,
  RoleHint,
  SessionMode,
  assertEmptyPayload,
  decodeActionResult,
  decodeBootstrapProgress,
  decodeEndTurnApplied,
  decodeEndTurnObserved,
  decodeHello,
  decodeLocalPlayerHandle,
  decodeMergeApplied,
  encodeBootstrapProgress,
  encodeBootstrapReleased,
  encodeEngineAction,
  encodeError,
  encodeHelloAck,
  encodeSessionPlan,
  opName,
} from './protocol.js';

export const SessionPhase = Object.freeze({
  Waiting: 'Waiting',
  Activating: 'Activating',
  Bootstrapping: 'Bootstrapping',
  Ready: 'Ready',
  Independent: 'Independent',
  PreparingMerge: 'PreparingMerge',
  ExecutingMerge: 'ExecutingMerge',
  Merged: 'Merged',
  Faulted: 'Faulted',
  Closed: 'Closed',
});

export const DEFAULT_CASCADE_TIMEOUT_MS = 30_000;
export const DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS = 12_000;
export const DEFAULT_BOOTSTRAP_TIMEOUT_MS = 15_000;
export const DEFAULT_MERGE_APPLY_TIMEOUT_MS = 30_000;
export const MAX_ONE_SHOT_DELAY_MS = 0x7fffffff;
export const MAX_MERGE_DAY = MAX_ENGINE_DAY;

export function roleName(role) {
  if (role === RoleHint.Host) return 'host';
  if (role === RoleHint.Join) return 'join';
  return `role-${role}`;
}

function validatePositiveInteger(value, label) {
  if (!Number.isInteger(value) || value <= 0) {
    throw new RangeError(`${label} must be a positive integer`);
  }
  return value;
}

function validateU32(value, label, {nonZero = false} = {}) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff ||
      (nonZero && value === 0)) {
    throw new RangeError(
      `${label} must be ${nonZero ? 'a non-zero' : 'an'} unsigned 32-bit integer`,
    );
  }
  return value;
}

function validateMergeDay(value) {
  validateU32(value, 'mergeDay');
  if (value === 1 || value > MAX_ENGINE_DAY) {
    throw new RangeError(
      `mergeDay must be 0 (disabled) or an integer from 2 to ${MAX_ENGINE_DAY}`,
    );
  }
  return value;
}

function validateNonNegativeDelay(value, label) {
  if (!Number.isSafeInteger(value) || value < 0 || value > MAX_ONE_SHOT_DELAY_MS) {
    throw new RangeError(
      `${label} must be a non-negative integer no greater than ` +
      `${MAX_ONE_SHOT_DELAY_MS}`,
    );
  }
  return value;
}

/**
 * Server-authoritative simultaneous-turn state machine.
 *
 * A connection must provide {id, send(op, payload), close()}. The relay owns
 * the epoch, merge day, subjective days, action ids and one-shot turn leases.
 * Clients report only facts tied to the current epoch/lease and execute typed
 * engine actions. Every engine mutation that gates later work is acknowledged.
 */
export class SimTurnsCoordinator {
  constructor({
    instanceId = 'unit',
    epoch = 1,
    mergeDay = 0,
    cascadeTimeoutMs = DEFAULT_CASCADE_TIMEOUT_MS,
    endTurnSignalTimeoutMs = DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS,
    bootstrapTimeoutMs = DEFAULT_BOOTSTRAP_TIMEOUT_MS,
    bootstrapReleaseGated = false,
    bootstrapCascadeDelayMs = 0,
    mergeApplyTimeoutMs = DEFAULT_MERGE_APPLY_TIMEOUT_MS,
    logger = () => {},
  } = {}) {
    this.instanceId = String(instanceId);
    this.epoch = validateU32(epoch, 'epoch', {nonZero: true});
    this.mergeDay = validateMergeDay(mergeDay);
    this.cascadeTimeoutMs = validatePositiveInteger(
      cascadeTimeoutMs,
      'cascadeTimeoutMs',
    );
    this.endTurnSignalTimeoutMs = validatePositiveInteger(
      endTurnSignalTimeoutMs,
      'endTurnSignalTimeoutMs',
    );
    this.bootstrapTimeoutMs = validatePositiveInteger(
      bootstrapTimeoutMs,
      'bootstrapTimeoutMs',
    );
    if (typeof bootstrapReleaseGated !== 'boolean') {
      throw new TypeError('bootstrapReleaseGated must be a boolean');
    }
    this.bootstrapReleaseGated = bootstrapReleaseGated;
    this.bootstrapReleaseAttempted = false;
    this.bootstrapReleaseGranted = !bootstrapReleaseGated;
    this.bootstrapCascadeDelayMs = validateNonNegativeDelay(
      bootstrapCascadeDelayMs,
      'bootstrapCascadeDelayMs',
    );
    this.mergeApplyTimeoutMs = validatePositiveInteger(
      mergeApplyTimeoutMs,
      'mergeApplyTimeoutMs',
    );
    this.logger = logger;

    this.phase = SessionPhase.Waiting;
    this.faultReason = null;
    this.sessionPlanSent = false;
    this.sessionReady = false;
    this.nextActionId = 1;
    this.nextLease = 1;

    this.connections = new Map();
    this.peersByRole = new Map();
    this.queue = [];
    this.activeJob = null;
    this.pendingAction = null;
    this.pendingMerge = null;
    this.bootstrap = null;
    this.bootstrapTimer = null;
    this.bootstrapTimerGeneration = 0;
    this.bootstrapCascadeTimer = null;
  }

  releaseBootstrap() {
    if (!this.bootstrapReleaseGated) {
      throw new Error('bootstrap release gate is not configured');
    }
    if (this.bootstrapReleaseAttempted) {
      throw new Error('bootstrap release was already attempted');
    }
    this.bootstrapReleaseAttempted = true;

    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    const valid = this.phase === SessionPhase.Waiting &&
      !this.sessionPlanSent &&
      host?.handle > 0 &&
      join?.handle > 0 &&
      host.handle !== join.handle;
    if (!valid) {
      const reason =
        'bootstrap release requires one waiting host, one waiting join, and both exact handles';
      this._fault(reason);
      throw new Error(reason);
    }

    this.bootstrapReleaseGranted = true;
    this._log('bootstrap-release-granted', {
      hostHandle: host.handle,
      joinHandle: join.handle,
    });
    this._refreshSessionPlan();
    if (!this.sessionPlanSent) {
      const reason = 'bootstrap release did not produce the host SessionPlan delivery';
      this._fault(reason);
      throw new Error(reason);
    }
  }

  registerConnection(connection) {
    if (!connection || typeof connection.send !== 'function' ||
        typeof connection.close !== 'function') {
      throw new TypeError('connection must provide send() and close()');
    }
    if (this.phase === SessionPhase.Closed) {
      connection.close();
      return;
    }
    if (this.connections.has(connection)) return;
    this.connections.set(connection, {peer: null});
    this._log('connection-open', {connectionId: connection.id ?? 'unknown'});
  }

  handleFrame(connection, frame) {
    if (!this.connections.has(connection)) this.registerConnection(connection);
    const state = this.connections.get(connection);
    if (!state || this.phase === SessionPhase.Closed) return;

    try {
      if (!frame || !Number.isInteger(frame.op) || !Buffer.isBuffer(frame.payload)) {
        throw new ProtocolError('invalid decoded frame object');
      }
      if (frame.flags !== 0) {
        throw new ProtocolError(`flags must be 0, got ${frame.flags}`);
      }

      if (!state.peer) {
        if (frame.op !== Op.Hello) {
          throw new ProtocolError(
            `first message must be Hello, got ${opName(frame.op)}`,
          );
        }
        this._handleHello(connection, state, frame.payload);
        return;
      }

      const peer = state.peer;
      if (frame.op === Op.Goodbye) {
        assertEmptyPayload(frame.payload, 'Goodbye');
        this.disconnect(connection, {graceful: true});
        connection.close();
        return;
      }
      if (this.phase === SessionPhase.Faulted) {
        this._sendError(connection, `session faulted: ${this.faultReason}`);
        return;
      }
      if (this.phase === SessionPhase.Merged) {
        this._sendError(connection, 'session already merged; stock turn flow owns progression');
        return;
      }

      switch (frame.op) {
      case Op.LocalPlayerHandle:
        this._handleLocalPlayerHandle(peer, frame.payload);
        return;
      case Op.SessionActivated:
        assertEmptyPayload(frame.payload, 'SessionActivated');
        this._handleSessionActivated(peer);
        return;
      case Op.BootstrapBeginTurnApplied:
        this._handleBootstrapBeginTurnApplied(peer, frame.payload);
        return;
      case Op.BootstrapComplete:
        this._handleBootstrapComplete(peer, frame.payload);
        return;
      case Op.BootstrapCommitApplied:
        this._handleBootstrapCommitApplied(peer, frame.payload);
        return;
      case Op.BootstrapOperationalApplied:
        this._handleBootstrapOperationalApplied(peer, frame.payload);
        return;
      case Op.EndTurnObserved:
        this._handleEndTurnObserved(peer, frame.payload);
        return;
      case Op.EndTurnApplied:
        this._handleEndTurnApplied(peer, frame.payload);
        return;
      case Op.ActionResult:
        this._handleActionResult(peer, frame.payload);
        return;
      case Op.MergeApplied:
        this._handleMergeApplied(peer, frame.payload);
        return;
      default:
        throw new ProtocolError(`unexpected client opcode ${opName(frame.op)}`);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.protocolViolation(connection, message);
    }
  }

  protocolViolation(connection, message, {terminal = false} = {}) {
    const state = this.connections.get(connection);
    const peer = state?.peer ?? null;
    this._log('protocol-error', {
      connectionId: connection?.id ?? 'unknown',
      role: peer ? roleName(peer.role) : null,
      message,
    });

    if (peer && this.phase === SessionPhase.Merged) {
      this._sendError(connection, `merged session protocol violation: ${message}`);
      return;
    }
    if (peer && (terminal || this._sessionHasStarted())) {
      this._fault(`protocol violation from ${roleName(peer.role)}: ${message}`, {
        allowMerged: true,
      });
      return;
    }

    this._sendError(connection, message);
    if (peer) {
      this._removePeer(peer);
      this._recomputeWaitingState();
    }
    this.connections.delete(connection);
    try { connection.close(); } catch {}
  }

  disconnect(connection, {graceful = false} = {}) {
    const state = this.connections.get(connection);
    if (!state) return;
    this.connections.delete(connection);
    const peer = state.peer;
    if (!peer) return;

    const started = this._sessionHasStarted();
    this._removePeer(peer);
    this._log('peer-disconnected', {
      role: roleName(peer.role),
      pid: peer.pid,
      graceful,
      started,
    });

    if (this.phase === SessionPhase.Faulted || this.phase === SessionPhase.Closed) return;
    if (started || this.phase === SessionPhase.Merged) {
      this._fault(`${roleName(peer.role)} peer disconnected during active session`, {
        allowMerged: true,
      });
    } else {
      this._recomputeWaitingState();
    }
  }

  shutdown() {
    if (this.phase === SessionPhase.Closed) return;
    this._clearPendingAction();
    this._clearAllEndTurnSignals();
    this._clearBootstrap();
    this._clearPendingMerge();
    this.queue.length = 0;
    this.activeJob = null;
    this.phase = SessionPhase.Closed;
    this.sessionReady = false;
    this._log('coordinator-closed', {});
  }

  snapshot() {
    const peers = {};
    for (const [role, peer] of this.peersByRole) {
      peers[roleName(role)] = {
        pid: peer.pid,
        handle: peer.handle,
        currentDay: peer.currentDay,
        lease: peer.lease,
        inFlight: peer.inFlight,
        atBarrier: peer.atBarrier,
        held: peer.held,
        pendingEndTurnLease: peer.pendingEndTurn?.lease ?? null,
        endTurnObserved: peer.pendingEndTurn?.observed ?? false,
        endTurnApplied: peer.pendingEndTurn?.applied ?? false,
        sessionPlanDeliveryAttempted: peer.sessionPlanDeliveryAttempted,
        sessionPlanDelivered: peer.sessionPlanDelivered,
        activated: peer.activated,
      };
    }
    return {
      instanceId: this.instanceId,
      epoch: this.epoch,
      mergeDay: this.mergeDay,
      phase: this.phase,
      faultReason: this.faultReason,
      ready: this.sessionReady,
      sessionPlanSent: this.sessionPlanSent,
      bootstrapReleaseGate: {
        configured: this.bootstrapReleaseGated,
        attempted: this.bootstrapReleaseAttempted,
        granted: this.bootstrapReleaseGranted,
      },
      queueLength: this.queue.length,
      activeJob: this.activeJob ? {
        role: roleName(this.activeJob.peer.role),
        completedDay: this.activeJob.completedDay,
        nextDay: this.activeJob.nextDay ?? null,
        stage: this.activeJob.stage ?? null,
      } : null,
      pendingAction: this.pendingAction ? {
        actionId: this.pendingAction.action.actionId,
        kind: this.pendingAction.action.kind,
        stage: this.pendingAction.stage,
        expectedRole: roleName(this.pendingAction.expectedPeer.role),
      } : null,
      pendingMerge: this.pendingMerge ? {
        actionId: this.pendingMerge.actionId,
        hostPrepared: this.pendingMerge.hostPrepared,
        joinPrepared: this.pendingMerge.joinPrepared,
        executeDispatched: this.pendingMerge.executeDispatched,
        executeResult: this.pendingMerge.executeResult,
        hostApplied: this.pendingMerge.hostApplied,
        joinApplied: this.pendingMerge.joinApplied,
        releaseDispatched: this.pendingMerge.releaseDispatched,
      } : null,
      bootstrap: this.bootstrap ? {
        cascadeClaimed: this.bootstrap.cascadeClaimed,
        beginTurnApplied: this.bootstrap.beginTurnApplied,
        cascadeDispatched: this.bootstrap.cascadeDispatched,
        cascadeDelayPending: this.bootstrapCascadeTimer !== null,
        cascadeComplete: this.bootstrap.cascadeComplete,
        joinComplete: this.bootstrap.joinComplete,
        commitDispatched: this.bootstrap.commitDispatched,
        hostCommitApplied: this.bootstrap.hostCommitApplied,
        joinCommitApplied: this.bootstrap.joinCommitApplied,
        operationalDispatched: this.bootstrap.operationalDispatched,
        hostOperationalApplied: this.bootstrap.hostOperationalApplied,
        joinOperationalApplied: this.bootstrap.joinOperationalApplied,
        releaseDispatched: this.bootstrap.releaseDispatched,
      } : null,
      peers,
    };
  }

  _handleHello(connection, state, payload) {
    let hello;
    try {
      hello = decodeHello(payload);
    } catch (error) {
      this._rejectHello(connection, error.message);
      return;
    }

    let role = hello.roleHint;
    if (role === RoleHint.Unknown) {
      if (!this.peersByRole.has(RoleHint.Host)) role = RoleHint.Host;
      else if (!this.peersByRole.has(RoleHint.Join)) role = RoleHint.Join;
    }

    let rejection = null;
    if (this.phase !== SessionPhase.Waiting) {
      rejection = `coordinator session is ${this.phase}`;
    } else if (hello.version !== PROTOCOL_VERSION) {
      rejection =
        `protocol version ${hello.version} is unsupported; expected ${PROTOCOL_VERSION}`;
    } else if (hello.pid === 0) {
      rejection = 'pid must be non-zero';
    } else if (role !== RoleHint.Host && role !== RoleHint.Join) {
      rejection = 'no host or join role is available';
    } else if (this.peersByRole.has(role)) {
      rejection = `${roleName(role)} role is already connected`;
    } else if ([...this.peersByRole.values()].some((peer) => peer.pid === hello.pid)) {
      rejection = `pid ${hello.pid} is already registered under another role`;
    }

    if (rejection) {
      this._rejectHello(connection, rejection);
      return;
    }

    const peer = {
      connection,
      pid: hello.pid,
      role,
      roleHint: hello.roleHint,
      handle: 0,
      currentDay: 1,
      lease: this._allocateLease(),
      inFlight: false,
      atBarrier: false,
      held: false,
      pendingEndTurn: null,
      sessionPlanDeliveryAttempted: false,
      sessionPlanDelivered: false,
      activated: false,
    };
    state.peer = peer;
    this.peersByRole.set(role, peer);
    this._send(connection, Op.HelloAck, encodeHelloAck(1));
    this._log('hello-accepted', {
      role: roleName(role),
      roleHint: hello.roleHint,
      pid: peer.pid,
      lease: peer.lease,
    });
  }

  _rejectHello(connection, reason) {
    this._log('hello-rejected', {
      connectionId: connection.id ?? 'unknown',
      reason,
    });
    try { this._send(connection, Op.HelloAck, encodeHelloAck(0)); } catch {}
    this._sendError(connection, reason);
    this.connections.delete(connection);
    try { connection.close(); } catch {}
  }

  _handleLocalPlayerHandle(peer, payload) {
    const {handle} = decodeLocalPlayerHandle(payload);
    if (handle === 0) throw new ProtocolError('local player handle must be non-zero');
    if (peer.handle !== 0 && peer.handle !== handle) {
      throw new ProtocolError(
        `local player handle changed from 0x${peer.handle.toString(16)} ` +
        `to 0x${handle.toString(16)}`,
      );
    }
    const other = this._otherPeer(peer);
    if (other?.handle === handle) {
      throw new ProtocolError(
        `local player handle 0x${handle.toString(16)} is already owned by ` +
        roleName(other.role),
      );
    }
    if (peer.handle === handle) return;
    if (this.sessionPlanSent) {
      throw new ProtocolError('local player handle arrived after SessionPlan');
    }

    peer.handle = handle;
    this._log('handle-ready', {role: roleName(peer.role), handle});
    this._refreshSessionPlan();
  }

  _refreshSessionPlan() {
    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    if (!host || !join || host.handle === 0 || join.handle === 0) return;
    if (host.handle === join.handle) {
      this._fault('host and join reported the same local player handle');
      return;
    }
    if (!this.bootstrapReleaseGranted) {
      this._log('bootstrap-release-pending', {
        hostHandle: host.handle,
        joinHandle: join.handle,
      });
      return;
    }
    if (this.sessionPlanSent || this.bootstrap) return;

    const planPayload = encodeSessionPlan({
      epoch: this.epoch,
      mode: SessionMode.Simultaneous,
      hostHandle: host.handle,
      joinHandle: join.handle,
      mergeDay: this.mergeDay,
      hostLease: host.lease,
      joinLease: join.lease,
    });
    this.bootstrap = {
      planPayload,
      cascadeClaimed: false,
      beginTurnApplied: false,
      cascadeDispatched: false,
      cascadeComplete: false,
      joinComplete: false,
      commitDispatched: false,
      commitFanoutComplete: false,
      hostCommitApplied: false,
      joinCommitApplied: false,
      operationalDispatched: false,
      operationalFanoutComplete: false,
      hostOperationalApplied: false,
      joinOperationalApplied: false,
      releaseDispatched: false,
      releaseFanoutComplete: false,
    };
    this.phase = SessionPhase.Activating;
    this.sessionPlanSent = true;
    if (!this._deliverSessionPlan(host)) return;
    this._log('session-plan-created', {
      epoch: this.epoch,
      mergeDay: this.mergeDay,
      hostHandle: host.handle,
      joinHandle: join.handle,
      hostLease: host.lease,
      joinLease: join.lease,
    });
  }

  _deliverSessionPlan(peer) {
    const bootstrap = this.bootstrap;
    const role = roleName(peer.role);
    if (!bootstrap || this.phase !== SessionPhase.Activating) {
      this._fault(`cannot deliver SessionPlan to ${role} outside activation`);
      return false;
    }
    if (peer.sessionPlanDeliveryAttempted || peer.sessionPlanDelivered) {
      this._fault(`duplicate SessionPlan delivery attempt for ${role}`);
      return false;
    }
    if (peer.role === RoleHint.Join) {
      const host = this.peersByRole.get(RoleHint.Host);
      if (!host?.activated) {
        this._fault('join SessionPlan delivery preceded host SessionActivated');
        return false;
      }
    }

    peer.sessionPlanDeliveryAttempted = true;
    peer.sessionPlanDelivered = true;
    try {
      this._send(peer.connection, Op.SessionPlan, bootstrap.planPayload);
    } catch (error) {
      this._fault(`failed to deliver SessionPlan to ${role}: ${error.message}`);
      return false;
    }
    this._log('session-plan-delivered', {role, epoch: this.epoch});
    return true;
  }

  _handleSessionActivated(peer) {
    if (!peer.sessionPlanDelivered) {
      throw new ProtocolError(
        `${roleName(peer.role)} SessionActivated preceded its SessionPlan delivery`,
      );
    }
    if (!this.bootstrap ||
        (this.phase !== SessionPhase.Activating &&
         this.phase !== SessionPhase.Bootstrapping)) {
      throw new ProtocolError(`unexpected SessionActivated in phase ${this.phase}`);
    }
    if (peer.activated) {
      throw new ProtocolError(`duplicate SessionActivated from ${roleName(peer.role)}`);
    }
    peer.activated = true;
    this._log('session-activated', {role: roleName(peer.role)});
    if (peer.role === RoleHint.Host) {
      const join = this.peersByRole.get(RoleHint.Join);
      if (!join || !this._deliverSessionPlan(join)) return;
    }
    this._maybeDispatchBootstrap();
  }

  _handleBootstrapBeginTurnApplied(peer, payload) {
    const progress = decodeBootstrapProgress(payload, 'BootstrapBeginTurnApplied');
    if (peer.role !== RoleHint.Join) {
      throw new ProtocolError('BootstrapBeginTurnApplied is accepted only from join');
    }
    if (!this.bootstrap ||
        (this.phase !== SessionPhase.Activating &&
         this.phase !== SessionPhase.Bootstrapping)) {
      throw new ProtocolError(
        `unexpected BootstrapBeginTurnApplied in phase ${this.phase}`,
      );
    }
    if (!peer.sessionPlanDelivered || !peer.activated) {
      throw new ProtocolError(
        'BootstrapBeginTurnApplied preceded join SessionPlan activation',
      );
    }
    if (progress.handle !== peer.handle || progress.day !== 1) {
      throw new ProtocolError('BootstrapBeginTurnApplied does not match join day 1');
    }
    if (this.bootstrap.beginTurnApplied) {
      throw new ProtocolError('duplicate BootstrapBeginTurnApplied');
    }

    this.bootstrap.beginTurnApplied = true;
    this._log('bootstrap-begin-turn-applied', {
      role: 'join',
      handle: peer.handle,
      day: 1,
    });
    this._armBootstrapCascadeDelay(this.bootstrap);
    this._maybeDispatchBootstrap();
  }

  _maybeDispatchBootstrap() {
    const bootstrap = this.bootstrap;
    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    if (!bootstrap || bootstrap.cascadeClaimed || !host?.activated ||
        !join?.activated || !bootstrap.beginTurnApplied) return;

    bootstrap.cascadeClaimed = true;
    this.phase = SessionPhase.Bootstrapping;
    const job = {
      peer: join,
      completedDay: 0,
      nextDay: 1,
      bootstrap: true,
      stage: 'bootstrap-apply',
    };
    this.activeJob = job;
    if (this.bootstrapCascadeDelayMs === 0) {
      this._dispatchClaimedBootstrap(bootstrap, job);
    } else if (!this.bootstrapCascadeTimer) {
      this._fault(
        'bootstrap prerequisites became ready without the sole delay timer',
      );
    }
  }

  _armBootstrapCascadeDelay(bootstrap) {
    if (this.bootstrapCascadeDelayMs === 0) return;
    if (this.bootstrap !== bootstrap || this.bootstrapCascadeTimer) {
      this._fault('bootstrap cascade delay could not be armed exactly once');
      return;
    }

    const timer = setTimeout(() => {
      if (this.bootstrapCascadeTimer !== timer) return;
      this.bootstrapCascadeTimer = null;
      if (this.bootstrap !== bootstrap ||
          this.phase === SessionPhase.Faulted ||
          this.phase === SessionPhase.Closed) return;

      const host = this.peersByRole.get(RoleHint.Host);
      const join = this.peersByRole.get(RoleHint.Join);
      const missing = [];
      if (!host?.activated) missing.push('host SessionActivated');
      if (!join?.activated) missing.push('join SessionActivated');
      if (!bootstrap.beginTurnApplied) missing.push('join BootstrapBeginTurnApplied');
      if (missing.length !== 0) {
        this._fault(
          `bootstrap prerequisites missing at the ` +
          `${this.bootstrapCascadeDelayMs} ms deadline: ${missing.join(', ')}`,
        );
        return;
      }

      const job = this.activeJob;
      if (!bootstrap.cascadeClaimed || this.phase !== SessionPhase.Bootstrapping ||
          !job?.bootstrap || job.peer !== join || job.nextDay !== 1) {
        this._fault('bootstrap cascade lost its exact one-shot claim');
        return;
      }
      this._dispatchClaimedBootstrap(bootstrap, job);
    }, this.bootstrapCascadeDelayMs);
    timer.unref?.();
    this.bootstrapCascadeTimer = timer;
    this._log('bootstrap-cascade-delay-armed', {
      delayMs: this.bootstrapCascadeDelayMs,
      anchor: 'bootstrap-begin-turn-applied',
    });
  }

  _dispatchClaimedBootstrap(bootstrap, job) {
    if (this.bootstrap !== bootstrap || this.phase !== SessionPhase.Bootstrapping ||
        this.activeJob !== job || !bootstrap.cascadeClaimed ||
        bootstrap.cascadeDispatched) {
      this._fault('bootstrap cascade claim lost its exact coordinator state');
      return;
    }
    bootstrap.cascadeDispatched = true;
    const host = this.peersByRole.get(RoleHint.Host);
    this._dispatchSingleAction({
      expectedPeer: host,
      kind: EngineActionKind.ApplyTurnStart,
      playerHandle: job.peer.handle,
      day: 1,
      lease: job.peer.lease,
      stage: 'bootstrap-apply',
      job,
      timeoutMs: this.cascadeTimeoutMs,
    });
  }

  _handleBootstrapComplete(peer, payload) {
    const progress = decodeBootstrapProgress(payload, 'BootstrapComplete');
    if (peer.role !== RoleHint.Join) {
      throw new ProtocolError('BootstrapComplete is accepted only from join');
    }
    if (!this.bootstrap || this.phase !== SessionPhase.Bootstrapping ||
        !this.bootstrap.cascadeDispatched) {
      throw new ProtocolError(`unexpected BootstrapComplete in phase ${this.phase}`);
    }
    if (progress.handle !== peer.handle || progress.day !== 1) {
      throw new ProtocolError('BootstrapComplete does not match join day 1');
    }
    if (this.bootstrap.joinComplete) {
      throw new ProtocolError('duplicate BootstrapComplete');
    }
    this.bootstrap.joinComplete = true;
    this._log('bootstrap-turn-info-applied', {
      role: 'join',
      handle: peer.handle,
      day: 1,
    });
    this._maybeCommitBootstrap();
  }

  _maybeCommitBootstrap() {
    const bootstrap = this.bootstrap;
    if (!bootstrap || bootstrap.commitDispatched || !bootstrap.cascadeComplete ||
        !bootstrap.joinComplete) return;
    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    if (!host?.activated || !join?.activated) {
      this._fault('peer activation state disappeared before bootstrap commit');
      return;
    }

    this._clearBootstrapTimeout();
    bootstrap.commitDispatched = true;
    const committed = encodeBootstrapProgress(join.handle, 1);
    try {
      this._send(join.connection, Op.BootstrapCommitted, committed);
      this._send(host.connection, Op.BootstrapCommitted, committed);
    } catch (error) {
      this._fault(`failed to deliver BootstrapCommitted: ${error.message}`);
      return;
    }
    if (this.bootstrap !== bootstrap || this.phase === SessionPhase.Faulted) return;
    bootstrap.commitFanoutComplete = true;
    this._log('bootstrap-commit-dispatched', {joinHandle: join.handle, day: 1});
    this._maybeDispatchBootstrapOperational();
    if (this.bootstrap === bootstrap && !bootstrap.operationalDispatched) {
      this._armBootstrapTimeout('BootstrapCommitApplied', [
        ['host', 'hostCommitApplied'],
        ['join', 'joinCommitApplied'],
      ]);
    }
  }

  _handleBootstrapCommitApplied(peer, payload) {
    const progress = decodeBootstrapProgress(payload, 'BootstrapCommitApplied');
    const bootstrap = this.bootstrap;
    const join = this.peersByRole.get(RoleHint.Join);
    if (!bootstrap || this.phase !== SessionPhase.Bootstrapping ||
        !bootstrap.commitDispatched || bootstrap.operationalDispatched) {
      throw new ProtocolError(
        `unexpected BootstrapCommitApplied in phase ${this.phase}`,
      );
    }
    if (!join || !peer.activated || progress.handle !== join.handle ||
        progress.day !== 1) {
      throw new ProtocolError(
        'BootstrapCommitApplied does not match the activated join day 1',
      );
    }
    const field = peer.role === RoleHint.Host
      ? 'hostCommitApplied'
      : 'joinCommitApplied';
    if (bootstrap[field]) {
      throw new ProtocolError(
        `duplicate BootstrapCommitApplied from ${roleName(peer.role)}`,
      );
    }
    bootstrap[field] = true;
    this._log('bootstrap-commit-applied', {role: roleName(peer.role)});
    this._maybeDispatchBootstrapOperational();
  }

  _maybeDispatchBootstrapOperational() {
    const bootstrap = this.bootstrap;
    if (!bootstrap || bootstrap.operationalDispatched ||
        !bootstrap.commitFanoutComplete || !bootstrap.hostCommitApplied ||
        !bootstrap.joinCommitApplied) return;
    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    if (!host?.activated || !join?.activated) {
      this._fault('peer activation state disappeared before bootstrap operational release');
      return;
    }

    this._clearBootstrapTimeout();
    bootstrap.operationalDispatched = true;
    const operational = encodeBootstrapProgress(join.handle, 1);
    try {
      // Host-first preserves ordering before the first later ApplyTurnStart.
      this._send(host.connection, Op.BootstrapOperational, operational);
      this._send(join.connection, Op.BootstrapOperational, operational);
    } catch (error) {
      this._fault(`failed to deliver BootstrapOperational: ${error.message}`);
      return;
    }
    if (this.bootstrap !== bootstrap || this.phase === SessionPhase.Faulted) return;
    bootstrap.operationalFanoutComplete = true;
    this._log('bootstrap-operational-dispatched', {joinHandle: join.handle, day: 1});
    this._maybeCompleteBootstrap();
    if (this.bootstrap === bootstrap && !bootstrap.releaseDispatched) {
      this._armBootstrapTimeout('BootstrapOperationalApplied', [
        ['host', 'hostOperationalApplied'],
        ['join', 'joinOperationalApplied'],
      ]);
    }
  }

  _handleBootstrapOperationalApplied(peer, payload) {
    const progress = decodeBootstrapProgress(payload, 'BootstrapOperationalApplied');
    const bootstrap = this.bootstrap;
    const join = this.peersByRole.get(RoleHint.Join);
    if (!bootstrap || this.phase !== SessionPhase.Bootstrapping ||
        !bootstrap.operationalDispatched || !bootstrap.hostCommitApplied ||
        !bootstrap.joinCommitApplied) {
      throw new ProtocolError(
        `unexpected BootstrapOperationalApplied in phase ${this.phase}`,
      );
    }
    if (!join || !peer.activated || progress.handle !== join.handle ||
        progress.day !== 1) {
      throw new ProtocolError(
        'BootstrapOperationalApplied does not match the activated join day 1',
      );
    }
    const field = peer.role === RoleHint.Host
      ? 'hostOperationalApplied'
      : 'joinOperationalApplied';
    if (bootstrap[field]) {
      throw new ProtocolError(
        `duplicate BootstrapOperationalApplied from ${roleName(peer.role)}`,
      );
    }
    bootstrap[field] = true;
    this._log('bootstrap-operational-applied', {role: roleName(peer.role)});
    this._maybeCompleteBootstrap();
  }

  _maybeCompleteBootstrap() {
    const bootstrap = this.bootstrap;
    if (!bootstrap || this.phase !== SessionPhase.Bootstrapping ||
        !bootstrap.operationalDispatched || !bootstrap.operationalFanoutComplete ||
        !bootstrap.hostOperationalApplied || !bootstrap.joinOperationalApplied ||
        bootstrap.releaseDispatched) return;
    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    if (!host?.activated || !join?.activated) {
      this._fault('peer activation state disappeared before final bootstrap release');
      return;
    }

    this._clearBootstrapTimeout();
    this.activeJob = null;
    this.phase = SessionPhase.Ready;
    this.sessionReady = true;
    bootstrap.releaseDispatched = true;
    const released = encodeBootstrapReleased(join.handle, 1);
    try {
      this._send(host.connection, Op.BootstrapReleased, released);
      if (this.phase !== SessionPhase.Ready || this.bootstrap !== bootstrap) return;
      this._send(join.connection, Op.BootstrapReleased, released);
    } catch (error) {
      this._fault(`failed to deliver BootstrapReleased: ${error.message}`);
      return;
    }
    if (this.phase !== SessionPhase.Ready || this.bootstrap !== bootstrap) return;
    bootstrap.releaseFanoutComplete = true;
    this._log('bootstrap-released', {joinHandle: join.handle, day: 1});
    this._log('session-operational', {epoch: this.epoch, mergeDay: this.mergeDay});
  }

  _endTurnSignalsAreAllowed(reporter, label) {
    if (!this.sessionReady ||
        (this.phase !== SessionPhase.Ready &&
         this.phase !== SessionPhase.Independent)) {
      if (this.sessionPlanSent) {
        throw new ProtocolError(
          `${label} is not allowed in phase ${this.phase}`,
        );
      }
      this._sendError(reporter.connection, 'session is not ready');
      return false;
    }
    return true;
  }

  _handleEndTurnObserved(peer, payload) {
    const signal = decodeEndTurnObserved(payload);
    if (!this._endTurnSignalsAreAllowed(peer, 'EndTurnObserved')) return;
    this._requireEpoch(signal.epoch, 'EndTurnObserved');
    if (signal.lease !== peer.lease) {
      throw new ProtocolError(
        `EndTurnObserved lease mismatch for ${roleName(peer.role)}: ` +
        `expected ${peer.lease}, got ${signal.lease}`,
      );
    }
    this._acceptEndTurnSignal(peer, 'observed', signal.lease);
  }

  _handleEndTurnApplied(reporter, payload) {
    const signal = decodeEndTurnApplied(payload);
    if (!this._endTurnSignalsAreAllowed(reporter, 'EndTurnApplied')) return;
    this._requireEpoch(signal.epoch, 'EndTurnApplied');
    if (reporter.role !== RoleHint.Host) {
      throw new ProtocolError('EndTurnApplied is accepted only from host');
    }
    const origin = this._peerForLease(signal.lease);
    if (!origin) {
      throw new ProtocolError(
        `EndTurnApplied lease ${signal.lease} does not identify a current player turn`,
      );
    }
    this._acceptEndTurnSignal(origin, 'applied', signal.lease);
  }

  _acceptEndTurnSignal(origin, kind, lease) {
    if (origin.atBarrier) {
      throw new ProtocolError(
        `${roleName(origin.role)} is already waiting at the merge barrier`,
      );
    }

    let pending = origin.pendingEndTurn;
    if (!pending) {
      if (origin.inFlight) {
        throw new ProtocolError(
          `a turn is already in flight for ${roleName(origin.role)}`,
        );
      }
      pending = {
        lease,
        completedDay: origin.currentDay,
        observed: false,
        applied: false,
        timer: null,
      };
      origin.pendingEndTurn = pending;
      origin.inFlight = true;
      this._armEndTurnSignalTimeout(origin, pending);
    } else if (pending.lease !== lease) {
      throw new ProtocolError(
        `second outstanding end-turn lease for ${roleName(origin.role)}: ` +
        `expected ${pending.lease}, got ${lease}`,
      );
    }

    if (pending[kind]) {
      throw new ProtocolError(
        `duplicate EndTurn${kind === 'observed' ? 'Observed' : 'Applied'} ` +
        `for ${roleName(origin.role)} lease=${lease}`,
      );
    }
    pending[kind] = true;
    this._log(`end-turn-${kind}`, {
      role: roleName(origin.role),
      lease,
      completedDay: pending.completedDay,
    });
    if (!pending.observed || !pending.applied) return;

    const completedDay = pending.completedDay;
    this._clearEndTurnSignal(origin);
    this.queue.push({peer: origin, lease, completedDay, stage: 'queued'});
    if (this.phase === SessionPhase.Ready) this.phase = SessionPhase.Independent;
    this._log('end-turn-accepted', {
      role: roleName(origin.role),
      lease,
      completedDay,
      queued: this.queue.length,
    });
    this._drainQueue();
  }

  _drainQueue() {
    if (!this.sessionReady ||
        this.phase === SessionPhase.Faulted ||
        this.phase === SessionPhase.PreparingMerge ||
        this.phase === SessionPhase.ExecutingMerge ||
        this.phase === SessionPhase.Merged ||
        this.phase === SessionPhase.Closed ||
        this.activeJob || this.pendingAction || this.pendingMerge) return;

    const job = this.queue.shift();
    if (!job) return;
    this.activeJob = job;
    const {peer} = job;
    if (!peer.inFlight || peer.atBarrier || job.lease !== peer.lease ||
        job.completedDay !== peer.currentDay) {
      this._fault(
        `accepted end-turn lease lost coordinator state for ${roleName(peer.role)}`,
      );
      return;
    }
    if (peer.currentDay >= MAX_ENGINE_DAY) {
      this._fault(`engine day overflow for ${roleName(peer.role)}`);
      return;
    }

    job.nextDay = peer.currentDay + 1;
    if (this.mergeDay > 0 && job.nextDay > this.mergeDay) {
      this._fault(
        `turn progression crossed mergeDay=${this.mergeDay} for ${roleName(peer.role)}`,
      );
      return;
    }
    if (this.mergeDay > 0 && job.nextDay === this.mergeDay) {
      this._enterBarrier(job);
      return;
    }
    this._dispatchApplyTurnStart(job);
  }

  _dispatchApplyTurnStart(job) {
    const host = this.peersByRole.get(RoleHint.Host);
    if (!host) {
      this._fault('host is unavailable for ApplyTurnStart');
      return;
    }
    job.actionId = this._allocateActionId();
    job.nextLease = this._allocateLease();
    job.stage = 'apply-turn-start';
    this._dispatchSingleAction({
      actionId: job.actionId,
      expectedPeer: host,
      kind: EngineActionKind.ApplyTurnStart,
      playerHandle: job.peer.handle,
      day: job.nextDay,
      lease: job.nextLease,
      stage: 'ordinary-apply',
      job,
      timeoutMs: this.cascadeTimeoutMs,
    });
  }

  _dispatchActivateTurn(job) {
    job.stage = 'activate-turn';
    this._dispatchSingleAction({
      actionId: job.actionId,
      expectedPeer: job.peer,
      kind: EngineActionKind.ActivateTurn,
      playerHandle: job.peer.handle,
      day: job.nextDay,
      lease: job.nextLease,
      stage: 'ordinary-activate',
      job,
      timeoutMs: this.cascadeTimeoutMs,
    });
  }

  _enterBarrier(job) {
    const peer = job.peer;
    // The end-turn lease is consumed, but currentDay intentionally remains
    // mergeDay - 1 until the server-authoritative merge transaction finishes.
    peer.atBarrier = true;
    peer.held = false;
    job.stage = 'hold-input';
    this._dispatchSingleAction({
      expectedPeer: peer,
      kind: EngineActionKind.HoldInput,
      playerHandle: peer.handle,
      day: peer.currentDay,
      lease: 0,
      stage: 'hold-input',
      job,
      timeoutMs: this.mergeApplyTimeoutMs,
    });
  }

  _maybeBeginMerge() {
    if (this.pendingMerge || this.pendingAction || this.activeJob ||
        this.queue.length !== 0) return;
    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    if (!host?.atBarrier || !join?.atBarrier || !host.held || !join.held) {
      this._drainQueue();
      return;
    }
    if (host.currentDay + 1 !== this.mergeDay ||
        join.currentDay + 1 !== this.mergeDay) {
      this._fault('merge barrier days do not exactly precede the configured merge day');
      return;
    }

    this.sessionReady = false;
    this.phase = SessionPhase.PreparingMerge;
    const actionId = this._allocateActionId();
    const pending = {
      actionId,
      hostPrepared: false,
      joinPrepared: false,
      executeDispatched: false,
      executeResult: false,
      hostApplied: false,
      joinApplied: false,
      releaseDispatched: false,
      timer: null,
    };
    this.pendingMerge = pending;
    this._armMergeTimeout(pending, 'PrepareMerge ActionResult');

    const action = encodeEngineAction({
      epoch: this.epoch,
      actionId,
      kind: EngineActionKind.PrepareMerge,
      playerHandle: host.handle,
      day: this.mergeDay,
      lease: 0,
    });
    try {
      this._send(join.connection, Op.EngineAction, action);
      this._send(host.connection, Op.EngineAction, action);
    } catch (error) {
      this._fault(`failed to deliver PrepareMerge: ${error.message}`);
      return;
    }
    this._log('merge-prepare-dispatched', {
      actionId,
      mergeDay: this.mergeDay,
    });
  }

  _dispatchExecuteMerge(pending) {
    if (this.pendingMerge !== pending || this.phase !== SessionPhase.PreparingMerge ||
        !pending.hostPrepared || !pending.joinPrepared ||
        pending.executeDispatched) return;
    const host = this.peersByRole.get(RoleHint.Host);
    if (!host) {
      this._fault('host disappeared before ExecuteMerge');
      return;
    }
    this._clearMergeTimeout();
    this.phase = SessionPhase.ExecutingMerge;
    pending.executeDispatched = true;
    this._armMergeTimeout(pending, 'ExecuteMerge evidence');
    const action = encodeEngineAction({
      epoch: this.epoch,
      actionId: pending.actionId,
      kind: EngineActionKind.ExecuteMerge,
      playerHandle: host.handle,
      day: this.mergeDay,
      lease: 0,
    });
    try {
      this._send(host.connection, Op.EngineAction, action);
    } catch (error) {
      this._fault(`failed to deliver ExecuteMerge: ${error.message}`);
      return;
    }
    this._log('merge-execute-dispatched', {
      actionId: pending.actionId,
      mergeDay: this.mergeDay,
    });
  }

  _handleActionResult(peer, payload) {
    const result = decodeActionResult(payload);
    this._requireEpoch(result.epoch, 'ActionResult');

    if (this.pendingMerge &&
        result.actionId === this.pendingMerge.actionId &&
        (result.kind === EngineActionKind.PrepareMerge ||
         result.kind === EngineActionKind.ExecuteMerge)) {
      this._handleMergeActionResult(peer, result);
      return;
    }

    const pending = this.pendingAction;
    if (!pending) {
      throw new ProtocolError(
        `unexpected ActionResult actionId=${result.actionId}`,
      );
    }
    if (peer !== pending.expectedPeer) {
      throw new ProtocolError(
        `ActionResult actionId=${result.actionId} came from ` +
        `${roleName(peer.role)}, expected ${roleName(pending.expectedPeer.role)}`,
      );
    }
    if (result.actionId !== pending.action.actionId ||
        result.kind !== pending.action.kind) {
      throw new ProtocolError(
        `ActionResult mismatch: expected actionId=${pending.action.actionId} ` +
        `kind=${pending.action.kind}, got actionId=${result.actionId} ` +
        `kind=${result.kind}`,
      );
    }

    this._clearPendingAction();
    if (!result.success) {
      this._fault(
        `${roleName(peer.role)} failed ${pending.stage} ` +
        `actionId=${result.actionId}`,
      );
      return;
    }

    const {job} = pending;
    switch (pending.stage) {
    case 'bootstrap-apply':
      if (!this.bootstrap || this.phase !== SessionPhase.Bootstrapping ||
          !this.bootstrap.cascadeDispatched || this.bootstrap.cascadeComplete) {
        this._fault('bootstrap ActionResult lost its exact coordinator state');
        return;
      }
      this.bootstrap.cascadeComplete = true;
      this._log('bootstrap-cascade-complete', {
        actionId: result.actionId,
        role: 'join',
        day: 1,
      });
      this._maybeCommitBootstrap();
      return;
    case 'ordinary-apply':
      if (this.activeJob !== job || job.peer.lease !== job.lease ||
          job.peer.currentDay !== job.completedDay) {
        this._fault('ApplyTurnStart result lost its exact turn state');
        return;
      }
      this._dispatchActivateTurn(job);
      return;
    case 'ordinary-activate':
      if (this.activeJob !== job || job.peer.lease !== job.lease ||
          job.peer.currentDay !== job.completedDay) {
        this._fault('ActivateTurn result lost its exact turn state');
        return;
      }
      job.peer.currentDay = job.nextDay;
      job.peer.lease = job.nextLease;
      job.peer.inFlight = false;
      this.activeJob = null;
      this._log('turn-start-complete', {
        actionId: result.actionId,
        role: roleName(job.peer.role),
        day: job.nextDay,
        lease: job.nextLease,
      });
      this._drainQueue();
      return;
    case 'hold-input':
      if (this.activeJob !== job || !job.peer.atBarrier ||
          job.peer.lease !== job.lease || job.peer.currentDay !== job.completedDay) {
        this._fault('HoldInput result lost its exact barrier state');
        return;
      }
      job.peer.held = true;
      job.peer.inFlight = false;
      this.activeJob = null;
      this._log('barrier-held', {
        actionId: result.actionId,
        role: roleName(job.peer.role),
        day: job.peer.currentDay,
      });
      // Another EndTurn may have queued while this HoldInput was pending.
      this._drainQueue();
      this._maybeBeginMerge();
      return;
    default:
      this._fault(`unknown pending action stage ${pending.stage}`);
    }
  }

  _handleMergeActionResult(peer, result) {
    const pending = this.pendingMerge;
    if (!pending) throw new ProtocolError('merge ActionResult has no transaction');

    if (result.kind === EngineActionKind.PrepareMerge) {
      if (this.phase !== SessionPhase.PreparingMerge ||
          result.actionId !== pending.actionId) {
        throw new ProtocolError(
          `unexpected PrepareMerge ActionResult in phase ${this.phase}`,
        );
      }
      const field = peer.role === RoleHint.Host ? 'hostPrepared' : 'joinPrepared';
      if (pending[field]) {
        throw new ProtocolError(
          `duplicate PrepareMerge ActionResult from ${roleName(peer.role)}`,
        );
      }
      pending[field] = true;
      if (!result.success) {
        this._fault(`${roleName(peer.role)} failed PrepareMerge`);
        return;
      }
      this._log('merge-prepare-applied', {
        actionId: result.actionId,
        role: roleName(peer.role),
      });
      this._dispatchExecuteMerge(pending);
      return;
    }

    if (result.kind === EngineActionKind.ExecuteMerge) {
      if (this.phase !== SessionPhase.ExecutingMerge ||
          result.actionId !== pending.actionId || !pending.executeDispatched) {
        throw new ProtocolError(
          `unexpected ExecuteMerge ActionResult in phase ${this.phase}`,
        );
      }
      if (peer.role !== RoleHint.Host) {
        throw new ProtocolError('ExecuteMerge ActionResult is accepted only from host');
      }
      if (pending.executeResult) {
        throw new ProtocolError('duplicate ExecuteMerge ActionResult');
      }
      pending.executeResult = true;
      if (!result.success) {
        this._fault('host failed ExecuteMerge');
        return;
      }
      this._log('merge-execute-applied', {actionId: result.actionId});
      this._maybeReleaseStock(pending);
      return;
    }

    throw new ProtocolError(`unexpected merge ActionResult actionId=${result.actionId}`);
  }

  _handleMergeApplied(peer, payload) {
    const applied = decodeMergeApplied(payload);
    this._requireEpoch(applied.epoch, 'MergeApplied');
    const pending = this.pendingMerge;
    if (!pending || this.phase !== SessionPhase.ExecutingMerge ||
        applied.actionId !== pending.actionId || !pending.executeDispatched) {
      throw new ProtocolError(
        `unexpected MergeApplied actionId=${applied.actionId} in phase ${this.phase}`,
      );
    }
    const field = peer.role === RoleHint.Host ? 'hostApplied' : 'joinApplied';
    if (pending[field]) {
      throw new ProtocolError(`duplicate MergeApplied from ${roleName(peer.role)}`);
    }
    pending[field] = true;
    this._log('merge-applied', {
      actionId: applied.actionId,
      role: roleName(peer.role),
    });
    this._maybeReleaseStock(pending);
  }

  _maybeReleaseStock(pending) {
    if (this.pendingMerge !== pending || this.phase !== SessionPhase.ExecutingMerge ||
        !pending.executeResult || !pending.hostApplied || !pending.joinApplied ||
        pending.releaseDispatched) return;
    const host = this.peersByRole.get(RoleHint.Host);
    const join = this.peersByRole.get(RoleHint.Join);
    if (!host || !join) {
      this._fault('peer disappeared before ReleaseStock');
      return;
    }

    this._clearMergeTimeout();
    pending.releaseDispatched = true;
    const action = encodeEngineAction({
      epoch: this.epoch,
      actionId: pending.actionId,
      kind: EngineActionKind.ReleaseStock,
      playerHandle: host.handle,
      day: this.mergeDay,
      lease: 0,
    });

    // Claim Merged before the sole join-before-host fan-out. A partial send is
    // terminal and cannot be retried without risking asymmetric stock gates.
    this.phase = SessionPhase.Merged;
    host.currentDay = this.mergeDay;
    join.currentDay = this.mergeDay;
    try {
      this._send(join.connection, Op.EngineAction, action);
      if (this.phase !== SessionPhase.Merged || this.pendingMerge !== pending) return;
      this._send(host.connection, Op.EngineAction, action);
    } catch (error) {
      this._fault(`failed to deliver ReleaseStock: ${error.message}`, {
        allowMerged: true,
      });
      return;
    }
    if (this.phase !== SessionPhase.Merged || this.pendingMerge !== pending) return;
    this.pendingMerge = null;
    this._log('merge-released', {
      actionId: pending.actionId,
      mergeDay: this.mergeDay,
    });
  }

  _dispatchSingleAction({
    actionId = null,
    expectedPeer,
    kind,
    playerHandle,
    day,
    lease,
    stage,
    job,
    timeoutMs,
  }) {
    if (!expectedPeer) {
      this._fault(`cannot dispatch ${stage}: expected peer is unavailable`);
      return;
    }
    if (this.pendingAction) {
      this._fault(`cannot dispatch ${stage}: another engine action is pending`);
      return;
    }
    const action = {
      epoch: this.epoch,
      actionId: actionId ?? this._allocateActionId(),
      kind,
      playerHandle,
      day,
      lease,
    };
    const pending = {
      expectedPeer,
      action,
      stage,
      job,
      timer: null,
    };
    const timer = setTimeout(() => {
      if (this.pendingAction !== pending) return;
      this._fault(
        `${stage} actionId=${action.actionId} timed out after ${timeoutMs} ms`,
      );
    }, timeoutMs);
    timer.unref?.();
    pending.timer = timer;
    this.pendingAction = pending;

    try {
      this._send(expectedPeer.connection, Op.EngineAction, encodeEngineAction(action));
    } catch (error) {
      this._fault(`failed to deliver ${stage}: ${error.message}`);
      return;
    }
    this._log('engine-action-dispatched', {
      actionId: action.actionId,
      kind,
      stage,
      recipient: roleName(expectedPeer.role),
      playerHandle,
      day,
      lease,
    });
  }

  _requireEpoch(actual, label) {
    if (actual !== this.epoch) {
      throw new ProtocolError(
        `${label} epoch mismatch: expected ${this.epoch}, got ${actual}`,
      );
    }
  }

  _peerForLease(lease) {
    for (const peer of this.peersByRole.values()) {
      if (peer.lease === lease) return peer;
    }
    return null;
  }

  _otherPeer(peer) {
    return this.peersByRole.get(
      peer.role === RoleHint.Host ? RoleHint.Join : RoleHint.Host,
    ) ?? null;
  }

  _allocateActionId() {
    const value = this.nextActionId;
    if (value === 0) {
      const reason = 'engine action id space exhausted';
      this._fault(reason);
      throw new RangeError(reason);
    }
    this.nextActionId = value === 0xffffffff ? 0 : value + 1;
    return value;
  }

  _allocateLease() {
    const value = this.nextLease;
    if (value === 0) {
      const reason = 'turn lease space exhausted';
      this._fault(reason);
      throw new RangeError(reason);
    }
    this.nextLease = value === 0xffffffff ? 0 : value + 1;
    return value;
  }

  _sessionHasStarted() {
    return this.sessionPlanSent ||
      this.phase !== SessionPhase.Waiting ||
      this.activeJob !== null ||
      this.pendingAction !== null ||
      this.pendingMerge !== null;
  }

  _removePeer(peer) {
    this._clearEndTurnSignal(peer);
    if (this.peersByRole.get(peer.role) === peer) this.peersByRole.delete(peer.role);
    this.queue = this.queue.filter((job) => job.peer !== peer);
    peer.inFlight = false;
    peer.activated = false;
    this.sessionReady = false;
  }

  _recomputeWaitingState() {
    if (this._sessionHasStarted() ||
        this.phase === SessionPhase.Faulted ||
        this.phase === SessionPhase.Closed ||
        this.phase === SessionPhase.Merged) return;
    this.phase = SessionPhase.Waiting;
    this._clearAllEndTurnSignals();
    this._clearBootstrap();
    this.sessionPlanSent = false;
    this.sessionReady = false;
  }

  _clearPendingAction() {
    if (this.pendingAction?.timer) clearTimeout(this.pendingAction.timer);
    this.pendingAction = null;
  }

  _clearEndTurnSignal(peer) {
    const pending = peer?.pendingEndTurn;
    if (pending?.timer) clearTimeout(pending.timer);
    if (peer) peer.pendingEndTurn = null;
  }

  _clearAllEndTurnSignals() {
    for (const peer of this.peersByRole.values()) this._clearEndTurnSignal(peer);
  }

  _armEndTurnSignalTimeout(origin, pending) {
    const timer = setTimeout(() => {
      if (origin.pendingEndTurn !== pending ||
          this.phase === SessionPhase.Faulted ||
          this.phase === SessionPhase.Closed) return;
      const missing = pending.observed ? 'EndTurnApplied' : 'EndTurnObserved';
      this._fault(
        `end-turn evidence timed out for ${roleName(origin.role)} ` +
        `lease=${pending.lease}; missing ${missing} after ` +
        `${this.endTurnSignalTimeoutMs} ms`,
      );
    }, this.endTurnSignalTimeoutMs);
    timer.unref?.();
    pending.timer = timer;
  }

  _clearBootstrapTimeout() {
    this.bootstrapTimerGeneration += 1;
    if (this.bootstrapTimer) {
      clearTimeout(this.bootstrapTimer);
      this.bootstrapTimer = null;
    }
  }

  _clearBootstrap() {
    if (this.bootstrapCascadeTimer) {
      clearTimeout(this.bootstrapCascadeTimer);
      this.bootstrapCascadeTimer = null;
    }
    this._clearBootstrapTimeout();
    this.bootstrap = null;
  }

  _armBootstrapTimeout(stage, acknowledgements) {
    this._clearBootstrapTimeout();
    const bootstrap = this.bootstrap;
    const generation = this.bootstrapTimerGeneration;
    const timer = setTimeout(() => {
      if (this.bootstrap !== bootstrap || this.sessionReady ||
          this.bootstrapTimerGeneration !== generation) return;
      const missing = acknowledgements
        .filter(([, field]) => !bootstrap[field])
        .map(([role]) => role);
      if (missing.length === 0) return;
      this._fault(
        `${stage} missing: ${missing.join(', ')} after ` +
        `${this.bootstrapTimeoutMs} ms`,
      );
    }, this.bootstrapTimeoutMs);
    timer.unref?.();
    this.bootstrapTimer = timer;
  }

  _clearMergeTimeout() {
    if (this.pendingMerge?.timer) {
      clearTimeout(this.pendingMerge.timer);
      this.pendingMerge.timer = null;
    }
  }

  _clearPendingMerge() {
    this._clearMergeTimeout();
    this.pendingMerge = null;
  }

  _armMergeTimeout(pending, label) {
    this._clearMergeTimeout();
    const timer = setTimeout(() => {
      if (this.pendingMerge !== pending ||
          this.phase === SessionPhase.Faulted ||
          this.phase === SessionPhase.Closed ||
          this.phase === SessionPhase.Merged) return;
      this._fault(
        `${label} timed out for mergeDay=${this.mergeDay} after ` +
        `${this.mergeApplyTimeoutMs} ms`,
      );
    }, this.mergeApplyTimeoutMs);
    timer.unref?.();
    pending.timer = timer;
  }

  _fault(reason, {allowMerged = false} = {}) {
    if ((!allowMerged && this.phase === SessionPhase.Merged) ||
        this.phase === SessionPhase.Faulted ||
        this.phase === SessionPhase.Closed) return;
    this._clearPendingAction();
    this._clearAllEndTurnSignals();
    this._clearBootstrap();
    this._clearPendingMerge();
    this.queue.length = 0;
    this.activeJob = null;
    for (const peer of this.peersByRole.values()) peer.inFlight = false;
    this.phase = SessionPhase.Faulted;
    this.sessionPlanSent = false;
    this.sessionReady = false;
    this.faultReason = String(reason);
    this._log('session-faulted', {reason: this.faultReason});
    for (const peer of this.peersByRole.values()) {
      this._sendError(peer.connection, this.faultReason);
    }
  }

  _send(connection, op, payload) {
    connection.send(op, payload);
  }

  _sendError(connection, message) {
    try { this._send(connection, Op.Error, encodeError(message)); } catch {}
  }

  _log(event, fields) {
    try { this.logger(event, fields); } catch {}
  }
}
