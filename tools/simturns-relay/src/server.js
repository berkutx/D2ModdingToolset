import {randomInt, randomUUID} from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import {
  DEFAULT_PIPE_NAME,
  Op,
  PROTOCOL_VERSION,
} from './protocol.js';
import {attachFramedConnection} from './framed-connection.js';
import {
  DEFAULT_BOOTSTRAP_TIMEOUT_MS,
  DEFAULT_CASCADE_TIMEOUT_MS,
  DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS,
  DEFAULT_MERGE_APPLY_TIMEOUT_MS,
  SimTurnsCoordinator,
} from './coordinator.js';

export function resolvePipeName(value = process.env.D2MSS_SIMTURNS_PIPE) {
  const pipeName = value?.trim() || DEFAULT_PIPE_NAME;
  if (process.platform === 'win32') {
    if (!/^\\\\\.\\pipe\\[A-Za-z0-9._-]+$/.test(pipeName)) {
      throw new Error(
        'pipe name must match \\\\.\\pipe\\[A-Za-z0-9._-]+ on Windows',
      );
    }
  } else if (!path.isAbsolute(pipeName)) {
    throw new Error('pipe path must be absolute on non-Windows platforms');
  }
  return pipeName;
}

export function resolveBootstrapReleaseFile(value) {
  if (value === undefined || value === null) return null;
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error('bootstrap release file must be a non-empty absolute path');
  }
  if (!path.isAbsolute(value)) {
    throw new Error('bootstrap release file must be an absolute path');
  }
  return path.normalize(value);
}

export function createJsonLogger(instanceId, output = console.log) {
  return (event, fields = {}, level = 'info') => {
    output(JSON.stringify({
      timestamp: new Date().toISOString(),
      level,
      instanceId,
      event,
      ...fields,
    }));
  };
}

export class SimTurnsRelayServer {
  constructor({
    pipeName = process.env.D2MSS_SIMTURNS_PIPE,
    instanceId = randomUUID(),
    epoch = randomInt(1, 0x100000000),
    mergeDay = 0,
    cascadeTimeoutMs = DEFAULT_CASCADE_TIMEOUT_MS,
    endTurnSignalTimeoutMs = DEFAULT_END_TURN_SIGNAL_TIMEOUT_MS,
    bootstrapTimeoutMs = DEFAULT_BOOTSTRAP_TIMEOUT_MS,
    bootstrapReleaseFile = null,
    bootstrapCascadeDelayMs = 0,
    mergeApplyTimeoutMs = DEFAULT_MERGE_APPLY_TIMEOUT_MS,
    logger = null,
  } = {}) {
    this.pipeName = resolvePipeName(pipeName);
    this.bootstrapReleaseFile = resolveBootstrapReleaseFile(bootstrapReleaseFile);
    this.instanceId = instanceId;
    this.log = logger ?? createJsonLogger(instanceId);
    this.sockets = new Set();
    this.connectionCounter = 0;
    this.closed = false;
    this.bootstrapReleaseWatcher = null;
    this.bootstrapReleaseObserved = false;

    this.coordinator = new SimTurnsCoordinator({
      instanceId,
      epoch,
      mergeDay,
      cascadeTimeoutMs,
      endTurnSignalTimeoutMs,
      bootstrapTimeoutMs,
      bootstrapReleaseGated: this.bootstrapReleaseFile !== null,
      bootstrapCascadeDelayMs,
      mergeApplyTimeoutMs,
      logger: (event, fields) => this.log(event, fields),
    });
    this.server = net.createServer((socket) => this._accept(socket));
  }

  async listen() {
    if (this.closed) throw new Error('server is already closed');
    this._startBootstrapReleaseWatcher();
    try {
      await new Promise((resolve, reject) => {
        const onError = (error) => {
          this.server.off('listening', onListening);
          reject(error);
        };
        const onListening = () => {
          this.server.off('error', onError);
          resolve();
        };
        this.server.once('error', onError);
        this.server.once('listening', onListening);
        this.server.listen(this.pipeName);
      });
    } catch (error) {
      this._closeBootstrapReleaseWatcher();
      throw error;
    }
    this.server.on('error', (error) => {
      this.log('server-error', {message: error.message}, 'error');
    });
    this.log('listening', {
      pipeName: this.pipeName,
      protocolVersion: PROTOCOL_VERSION,
      pid: process.pid,
      epoch: this.coordinator.epoch,
      mergeDay: this.coordinator.mergeDay,
      bootstrapReleaseGated: this.bootstrapReleaseFile !== null,
      bootstrapCascadeDelayMs: this.coordinator.bootstrapCascadeDelayMs,
      cascadeTimeoutMs: this.coordinator.cascadeTimeoutMs,
      endTurnSignalTimeoutMs: this.coordinator.endTurnSignalTimeoutMs,
      mergeApplyTimeoutMs: this.coordinator.mergeApplyTimeoutMs,
    });
    return this;
  }

  async close() {
    if (this.closed) return;
    this.closed = true;
    this._closeBootstrapReleaseWatcher();
    this.coordinator.shutdown();
    for (const socket of this.sockets) socket.destroy();
    this.sockets.clear();
    if (this.server.listening) {
      await new Promise((resolve) => this.server.close(resolve));
    }
    this.log('server-closed', {});
  }

  _startBootstrapReleaseWatcher() {
    if (!this.bootstrapReleaseFile) return;
    if (this.bootstrapReleaseWatcher) {
      throw new Error('bootstrap release watcher is already active');
    }

    const releaseFile = this.bootstrapReleaseFile;
    const directory = path.dirname(releaseFile);
    const targetName = path.basename(releaseFile);
    const namesEqual = process.platform === 'win32'
      ? (left, right) => left.toLowerCase() === right.toLowerCase()
      : (left, right) => left === right;

    let watcher;
    try {
      watcher = fs.watch(directory, {persistent: false}, (_eventType, filename) => {
        if (this.bootstrapReleaseObserved || this.closed) return;
        const changedName = filename === null ? null : filename.toString();
        if (changedName !== null && !namesEqual(changedName, targetName)) return;

        let releaseStat;
        try {
          releaseStat = fs.lstatSync(releaseFile);
        } catch (error) {
          if (error?.code === 'ENOENT' && changedName === null) return;
          this._rejectBootstrapReleaseEvent(
            `could not inspect bootstrap release file: ${error.message}`,
          );
          return;
        }
        if (!releaseStat.isFile()) {
          this._rejectBootstrapReleaseEvent(
            'bootstrap release path was created but is not a regular file',
          );
          return;
        }

        this.bootstrapReleaseObserved = true;
        this._closeBootstrapReleaseWatcher();
        this.log('bootstrap-release-file-observed', {path: releaseFile});
        try {
          this.coordinator.releaseBootstrap();
        } catch (error) {
          this.log(
            'bootstrap-release-file-rejected',
            {path: releaseFile, message: error.message},
            'error',
          );
        }
      });
      this.bootstrapReleaseWatcher = watcher;
      watcher.once('error', (error) => {
        if (this.bootstrapReleaseWatcher !== watcher) return;
        this._rejectBootstrapReleaseEvent(
          `bootstrap release watcher failed: ${error.message}`,
        );
      });

      // Install the watcher first, then validate initial absence synchronously.
      // A file created during validation is therefore either rejected as
      // pre-existing or delivered by the already-active event source.
      try {
        fs.lstatSync(releaseFile);
        throw new Error(`bootstrap release file must not exist at startup: ${releaseFile}`);
      } catch (error) {
        if (error?.code !== 'ENOENT') throw error;
      }
    } catch (error) {
      try { watcher?.close(); } catch {}
      if (this.bootstrapReleaseWatcher === watcher) {
        this.bootstrapReleaseWatcher = null;
      }
      throw error;
    }
    this.log('bootstrap-release-file-watching', {path: releaseFile});
  }

  _rejectBootstrapReleaseEvent(message) {
    if (this.bootstrapReleaseObserved) return;
    this.bootstrapReleaseObserved = true;
    this._closeBootstrapReleaseWatcher();
    this.log(
      'bootstrap-release-file-rejected',
      {path: this.bootstrapReleaseFile, message},
      'error',
    );
    // A bad one-shot filesystem event is terminal for this server instance.
    // Closing also clears every coordinator timer and cannot release gameplay.
    void this.close();
  }

  _closeBootstrapReleaseWatcher() {
    const watcher = this.bootstrapReleaseWatcher;
    this.bootstrapReleaseWatcher = null;
    if (!watcher) return;
    try { watcher.close(); } catch {}
  }

  _accept(socket) {
    socket.setNoDelay(true);
    this.sockets.add(socket);
    const connectionId = `${this.instanceId}:${++this.connectionCounter}`;
    attachFramedConnection(socket, {
      connectionId,
      coordinator: this.coordinator,
      logger: (...args) => this.log(...args),
      onClose: (closedSocket) => this.sockets.delete(closedSocket),
    });
  }
}

export async function startRelayServer(options = {}) {
  const server = new SimTurnsRelayServer(options);
  await server.listen();
  return server;
}

export {Op};
