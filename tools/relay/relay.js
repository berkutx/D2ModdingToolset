/*
 * d2lobby.packetlogic relay — part of the publishable test/logging system.
 *
 * A dependency-free Node.js server that the mss32 test build's bridge connects to
 * over a Windows named pipe. It accepts the Hello handshake, ingests RX/TX packet
 * traces + log lines, decodes net-message class names (and best-effort chat text /
 * system notifications), and exposes a small HTTP API for tests / tooling.
 *
 * Run:  node relay.js            (pipe \\.\pipe\d2lobby.packetlogic, http :8077)
 * Then launch the game with the DebugTest mss32 build and D2TESTDRV_NET=1.
 *
 * No external dependencies — only the built-in `net` and `http` modules.
 */

'use strict';

const net = require('net');
const http = require('http');

const PIPE_NAME = '\\\\.\\pipe\\d2lobby.packetlogic';
const HTTP_HOST = '127.0.0.1';
const HTTP_PORT = 8077;
const PROTOCOL_VERSION = 1;

// Opcodes (mirror testdrv/packetlogicbridge.cpp — public protocol only).
const Op = {
    Hello: 0x0001,
    HelloAck: 0x0002,
    Goodbye: 0x0003,
    ConfigurePatches: 0x0004,
    LocalPlayerHandle: 0x0007,
    PacketTrace: 0x0202,   // TX
    PacketTraceRx: 0x0203, // RX
    InvokeButton: 0x0300,
    Log: 0xff00,
};

const MAX_RING = 500;
const state = {
    client: null,    // the connected DLL socket
    hello: null,     // { version, pid, modulePath }
    logs: [],        // recent log lines
    packets: [],     // recent decoded RX/TX traces
    chat: [],        // extracted chat lines
    events: [],      // system notifications (join/leave/turn/connect)
    ui: null,        // current dialog snapshot (populated once the DLL forwards UiSnapshot)
};

function ring(arr, item) {
    arr.push(item);
    if (arr.length > MAX_RING) arr.shift();
}

function nowIso() {
    // Date is fine here (Node host, not a workflow script).
    return new Date().toISOString();
}

// ---- frame codec -----------------------------------------------------------
// Wire frame: u32 length(=4+payloadLen) | u16 opcode | u16 flags | payload
function encodeFrame(op, payload) {
    payload = payload || Buffer.alloc(0);
    const buf = Buffer.alloc(8 + payload.length);
    buf.writeUInt32LE(4 + payload.length, 0);
    buf.writeUInt16LE(op, 4);
    buf.writeUInt16LE(0, 6);
    payload.copy(buf, 8);
    return buf;
}

// ---- net-message decoding --------------------------------------------------
// Mangled MSVC RTTI raw name looks like ".?AVCStackMoveMsg@@" — pull the class.
function decodeClassName(bytes) {
    const s = bytes.slice(0, Math.min(bytes.length, 64)).toString('latin1');
    const m = s.match(/\.\?A[UV]([A-Za-z0-9_]+)@@/);
    return m ? m[1] : null;
}

// Longest run of printable ASCII (len >= 2) — best-effort chat text extraction.
function extractText(bytes) {
    let best = '';
    let cur = '';
    for (const b of bytes) {
        if (b >= 0x20 && b < 0x7f) {
            cur += String.fromCharCode(b);
        } else {
            if (cur.length > best.length) best = cur;
            cur = '';
        }
    }
    if (cur.length > best.length) best = cur;
    return best.length >= 2 ? best.trim() : '';
}

const CHAT_CLASSES = new Set(['CCmdChatMsg', 'CChatMsg']);
const EVENT_CLASSES = new Set([
    'CConnectMsg', 'CJoinGameMsg', 'CDisconnectMsg', 'CCmdBeginTurnMsg', 'CCmdEndTurnMsg',
]);

function onTrace(dir, payload) {
    // payload: u32 self, u32 peer, u32 size, byte[size]
    if (payload.length < 12) return;
    const self = payload.readUInt32LE(0);
    const peer = payload.readUInt32LE(4);
    const size = payload.readUInt32LE(8);
    const body = payload.slice(12, 12 + Math.min(size, payload.length - 12));
    const cls = decodeClassName(body);
    const rec = {
        t: nowIso(), dir, self: self.toString(16), peer, size,
        cls: cls || '?', hex: body.slice(0, 32).toString('hex'),
    };
    ring(state.packets, rec);

    if (cls && CHAT_CLASSES.has(cls)) {
        const text = extractText(body);
        const line = { t: rec.t, dir, peer, cls, text };
        ring(state.chat, line);
        console.log(`[chat] (${dir} from ${peer}) ${text}`);
    } else if (cls && EVENT_CLASSES.has(cls)) {
        const ev = { t: rec.t, dir, peer, cls };
        ring(state.events, ev);
        console.log(`[event] ${cls} (${dir} peer=${peer})`);
    }
}

// ---- named-pipe server (DLL connection) ------------------------------------
function handleMessage(socket, op, flags, payload) {
    switch (op) {
    case Op.Hello: {
        const version = payload.readUInt32LE(0);
        const pid = payload.readUInt32LE(4);
        const modLen = payload.readUInt32LE(8);
        const modulePath = payload.slice(12, 12 + modLen).toString('utf8');
        state.hello = { version, pid, modulePath };
        console.log(`[hello] pid=${pid} v${version} ${modulePath}`);
        const ack = Buffer.alloc(8);
        ack.writeUInt32LE(1, 0);                 // accepted
        ack.writeUInt32LE(PROTOCOL_VERSION, 4);  // relay protocol version
        socket.write(encodeFrame(Op.HelloAck, ack));
        break;
    }
    case Op.Log: {
        const line = payload.toString('utf8');
        ring(state.logs, { t: nowIso(), line });
        console.log(`[log] ${line}`);
        break;
    }
    case Op.PacketTraceRx:
        onTrace('RX', payload);
        break;
    case Op.PacketTrace:
        onTrace('TX', payload);
        break;
    case Op.Goodbye:
        console.log('[goodbye] DLL disconnecting');
        break;
    default:
        ring(state.logs, { t: nowIso(), line: `unhandled op 0x${op.toString(16)} (${payload.length}B)` });
        break;
    }
}

function attachParser(socket) {
    let buf = Buffer.alloc(0);
    socket.on('data', (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        for (;;) {
            if (buf.length < 4) break;
            const len = buf.readUInt32LE(0); // = 4 + payloadLen (opcode+flags+payload)
            if (len < 4 || len > 16 * 1024 * 1024) {
                console.error(`[pipe] bad frame length ${len}; dropping connection`);
                socket.destroy();
                return;
            }
            if (buf.length < 4 + len) break;
            const frame = buf.slice(4, 4 + len);
            buf = buf.slice(4 + len);
            const op = frame.readUInt16LE(0);
            const flags = frame.readUInt16LE(2);
            const payload = frame.slice(4);
            try {
                handleMessage(socket, op, flags, payload);
            } catch (e) {
                console.error('[pipe] handler error', e);
            }
        }
    });
}

const pipeServer = net.createServer((socket) => {
    console.log('[pipe] DLL connected');
    state.client = socket;
    attachParser(socket);
    socket.on('close', () => {
        console.log('[pipe] DLL disconnected');
        if (state.client === socket) state.client = null;
    });
    socket.on('error', (e) => console.error('[pipe] socket error', e.message));
});

pipeServer.on('error', (e) => console.error('[pipe] server error', e.message));
pipeServer.listen(PIPE_NAME, () => console.log(`[pipe] listening on ${PIPE_NAME}`));

// ---- HTTP API --------------------------------------------------------------
// Encode an InvokeButton command: u16 dlgLen | dlg | u16 btnLen | btn.
function encodeInvoke(dlg, btn) {
    const d = Buffer.from(dlg || '', 'utf8');
    const b = Buffer.from(btn || '', 'utf8');
    const out = Buffer.alloc(2 + d.length + 2 + b.length);
    let o = 0;
    out.writeUInt16LE(d.length, o); o += 2; d.copy(out, o); o += d.length;
    out.writeUInt16LE(b.length, o); o += 2; b.copy(out, o);
    return out;
}

function sendJson(res, code, obj) {
    const body = JSON.stringify(obj, null, 2);
    res.writeHead(code, { 'Content-Type': 'application/json' });
    res.end(body);
}

const httpServer = http.createServer((req, res) => {
    const url = new URL(req.url, `http://${HTTP_HOST}:${HTTP_PORT}`);
    const path = url.pathname;

    if (req.method === 'GET' && path === '/api/status') {
        return sendJson(res, 200, { connected: !!state.client, hello: state.hello });
    }
    if (req.method === 'GET' && path === '/api/ui') {
        return sendJson(res, 200, state.ui || { note: 'no UiSnapshot received yet' });
    }
    if (req.method === 'GET' && path === '/api/log') {
        return sendJson(res, 200, { logs: state.logs.slice(-100), packets: state.packets.slice(-100) });
    }
    if (req.method === 'GET' && path === '/api/chat') {
        return sendJson(res, 200, { chat: state.chat.slice(-100) });
    }
    if (req.method === 'GET' && path === '/api/events') {
        return sendJson(res, 200, { events: state.events.slice(-100) });
    }
    if (req.method === 'GET' && path === '/api/packets') {
        return sendJson(res, 200, { packets: state.packets.slice(-200) });
    }
    if (req.method === 'POST' && path === '/api/invoke') {
        if (!state.client) return sendJson(res, 503, { error: 'no DLL connected' });
        const dlg = url.searchParams.get('dlg') || '';
        const btn = url.searchParams.get('btn') || '';
        state.client.write(encodeFrame(Op.InvokeButton, encodeInvoke(dlg, btn)));
        return sendJson(res, 200, { sent: { dlg, btn } });
    }
    sendJson(res, 404, { error: 'not found' });
});

httpServer.listen(HTTP_PORT, HTTP_HOST, () =>
    console.log(`[http] api on http://${HTTP_HOST}:${HTTP_PORT}`));

process.on('SIGINT', () => { console.log('\nshutting down'); process.exit(0); });
