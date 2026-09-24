import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import test from 'node:test';
import {attachFramedConnection} from '../src/framed-connection.js';
import {Op, encodeFrame} from '../src/protocol.js';

class FakeSocket extends EventEmitter {
  constructor() {
    super();
    this.destroyed = false;
    this.writable = true;
    this.writes = [];
    this.endCalls = 0;
    this.writeResult = true;
    this.onWrite = null;
  }

  write(frame) {
    this.writes.push(Buffer.from(frame));
    this.onWrite?.();
    return this.writeResult;
  }

  end() {
    this.endCalls += 1;
    this.writable = false;
  }
}

function attach(socket = new FakeSocket(), connectionId = 'adapter:1') {
  const events = [];
  const received = [];
  const violations = [];
  const logs = [];
  const coordinator = {
    registerConnection(connection) {
      events.push(['register', connection]);
    },
    handleFrame(connection, frame) {
      received.push({connection, frame});
    },
    protocolViolation(connection, message) {
      violations.push({connection, message});
    },
    disconnect(connection) {
      events.push(['disconnect', connection]);
    },
  };
  const connection = attachFramedConnection(socket, {
    connectionId,
    coordinator,
    logger: (event, fields, level) => logs.push({event, fields, level}),
    onClose: (closedSocket) => events.push(['close', closedSocket]),
  });
  return {socket, connection, coordinator, events, received, violations, logs};
}

test('framed connection preserves fragmented and coalesced frame order', () => {
  const state = attach();
  const firstPayload = Buffer.from([1, 2, 3]);
  const secondPayload = Buffer.from([4, 5]);
  const first = encodeFrame(Op.Hello, firstPayload);
  const second = encodeFrame(Op.LocalPlayerHandle, secondPayload);

  assert.deepEqual(state.events, [['register', state.connection]]);
  state.socket.emit('data', first.subarray(0, 3));
  assert.equal(state.received.length, 0);
  state.socket.emit('data', Buffer.concat([first.subarray(3), second]));

  assert.deepEqual(state.received, [
    {connection: state.connection, frame: {op: Op.Hello, flags: 0, payload: firstPayload}},
    {connection: state.connection, frame: {op: Op.LocalPlayerHandle, flags: 0, payload: secondPayload}},
  ]);
  assert.equal(state.logs.filter(({event}) => event === 'frame-received').length, 2);
  assert.equal(state.violations.length, 0);
});

test('each framed connection owns its unfinished decoder input', () => {
  const first = attach(new FakeSocket(), 'adapter:first');
  const second = attach(new FakeSocket(), 'adapter:second');
  const frame = encodeFrame(Op.Hello, Buffer.from([9]));

  first.socket.emit('data', frame.subarray(0, 5));
  second.socket.emit('data', frame);
  assert.equal(first.received.length, 0);
  assert.equal(second.received.length, 1);
  first.socket.emit('data', frame.subarray(5));
  assert.equal(first.received.length, 1);
  assert.equal(second.received.length, 1);
});

test('coordinator close ends the current batch and leaves disconnect to socket close', () => {
  const state = attach();
  state.coordinator.handleFrame = (connection, frame) => {
    state.received.push({connection, frame});
    connection.close();
  };
  state.socket.emit('data', Buffer.concat([
    encodeFrame(Op.Hello),
    encodeFrame(Op.LocalPlayerHandle),
  ]));
  state.socket.emit('data', encodeFrame(Op.Goodbye));
  state.connection.close();

  assert.equal(state.received.length, 1);
  assert.equal(state.socket.endCalls, 1);
  assert.deepEqual(state.events, [['register', state.connection]]);
  state.socket.emit('close');
  assert.deepEqual(state.events, [
    ['register', state.connection],
    ['close', state.socket],
    ['disconnect', state.connection],
  ]);
});

test('send stays synchronous and write(false) neither faults nor retries', () => {
  const state = attach();
  const order = [];
  const payload = Buffer.from([7, 8]);
  state.socket.writeResult = false;
  state.socket.onWrite = () => {
    order.push('write');
    state.socket.emit('data', encodeFrame(Op.SessionActivated));
  };
  state.coordinator.handleFrame = (connection, frame) => {
    order.push('receive');
    state.received.push({connection, frame});
  };

  order.push('before-send');
  const result = state.connection.send(Op.SessionPlan, payload);
  order.push('after-send');

  assert.equal(result, undefined);
  assert.deepEqual(order, ['before-send', 'write', 'receive', 'after-send']);
  assert.deepEqual(state.socket.writes, [encodeFrame(Op.SessionPlan, payload)]);
  assert.equal(state.socket.endCalls, 0);
  assert.equal(state.violations.length, 0);
  assert.equal(state.logs.filter(({event}) => event === 'frame-sent').length, 1);
});

test('send rejects an unwritable or destroyed socket without writing', () => {
  for (const field of ['writable', 'destroyed']) {
    const state = attach();
    state.socket[field] = field === 'destroyed';
    assert.throws(
      () => state.connection.send(Op.HelloAck, Buffer.alloc(0)),
      /connection adapter:1 is not writable/,
    );
    assert.equal(state.socket.writes.length, 0);
    assert.equal(state.logs.filter(({event}) => event === 'frame-sent').length, 0);
  }
});

test('malformed input reaches coordinator before close and never dispatches its batch', () => {
  const state = attach();
  state.coordinator.protocolViolation = (connection, message) => {
    state.violations.push({connection, message});
    assert.equal(state.socket.endCalls, 0, 'coordinator owns the terminal close decision');
    connection.close();
  };
  const malformed = Buffer.alloc(4);
  malformed.writeUInt32LE(3);
  state.socket.emit('data', Buffer.concat([encodeFrame(Op.Hello), malformed]));

  assert.equal(state.received.length, 0, 'decoder validates the batch before dispatching');
  assert.equal(state.violations.length, 1);
  assert.equal(state.violations[0].connection, state.connection);
  assert.match(state.violations[0].message, /frame length must be >= 4, got 3/);
  assert.equal(state.socket.endCalls, 1);
  assert.deepEqual(state.events, [['register', state.connection]]);
  state.socket.emit('data', encodeFrame(Op.Hello));
  assert.equal(state.received.length, 0);
  state.socket.emit('close');
  assert.deepEqual(state.events.slice(1), [
    ['close', state.socket],
    ['disconnect', state.connection],
  ]);
});

test('socket errors are logged and socket close remains the disconnect edge', () => {
  const state = attach();
  state.socket.emit('error', new Error('test socket failure'));
  assert.deepEqual(state.logs, [{
    event: 'socket-error',
    fields: {connectionId: 'adapter:1', message: 'test socket failure'},
    level: 'warn',
  }]);
  assert.deepEqual(state.events, [['register', state.connection]]);
  state.socket.emit('close');
  assert.deepEqual(state.events.slice(1), [
    ['close', state.socket],
    ['disconnect', state.connection],
  ]);
});
