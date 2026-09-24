import {FrameDecoder, ProtocolError, encodeFrame, opName} from './protocol.js';

/**
 * Local byte-stream adapter for one coordinator connection. The server owns
 * the socket lifetime/set; the coordinator sees only {id, send, close}.
 * Keep delivery synchronous: a write queues bytes once, and false is normal
 * stream backpressure, not a failed action or permission to resend.
 */
export function attachFramedConnection(socket, {
  connectionId,
  coordinator,
  logger,
  onClose,
}) {
  const decoder = new FrameDecoder();
  let ended = false;

  const connection = {
    id: connectionId,
    send: (op, payload) => {
      if (socket.destroyed || !socket.writable) {
        throw new Error(`connection ${connectionId} is not writable`);
      }
      socket.write(encodeFrame(op, payload));
      logger('frame-sent', {
        connectionId,
        op: opName(op),
        payloadBytes: payload.length,
      });
    },
    close: () => {
      if (ended || socket.destroyed) return;
      ended = true;
      socket.end();
    },
  };

  coordinator.registerConnection(connection);
  socket.on('data', (chunk) => {
    if (ended) return;
    try {
      const frames = decoder.push(chunk);
      for (const frame of frames) {
        logger('frame-received', {
          connectionId,
          op: opName(frame.op),
          payloadBytes: frame.payload.length,
        });
        coordinator.handleFrame(connection, frame);
        if (ended) break;
      }
    } catch (error) {
      const message = error instanceof ProtocolError
        ? error.message
        : `frame decode failed: ${error.message}`;
      coordinator.protocolViolation(connection, message);
    }
  });
  socket.on('error', (error) => {
    logger('socket-error', {connectionId, message: error.message}, 'warn');
  });
  socket.on('close', () => {
    ended = true;
    onClose(socket);
    coordinator.disconnect(connection);
  });
  return connection;
}
