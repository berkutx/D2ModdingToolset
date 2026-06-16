/*
 * d2lobby.packetlogic relay — part of the publishable test/logging system.
 *
 * A dependency-free Node.js server the mss32 bridge connects to over a Windows named pipe.
 * MULTI-CLIENT (host + joiner, each tagged by role from its Hello), a dumb mirror + command
 * relay with no test logic: it mirrors each agent's live UI (dialog + buttons) and packets, and
 * relays the dispatcher's InvokeButton / SetSelection commands to a role. The dispatcher reads
 * /api/state and POSTs /api/invoke|/api/select, so two instances coordinate with no files.
 *
 * Run: node relay.js  (pipe \\.\pipe\d2lobby.packetlogic, http :8077). Built-ins net + http only.
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
    PacketTrace: 0x0202,    // TX
    PacketTraceRx: 0x0203,  // RX
    InvokeButton: 0x0300,   // -> agent: click a button
    SetSelection: 0x0301,   // -> agent: set a listbox selection
    Dialog: 0x0410,         // <- agent: live UI state ("dialogName\nbtn1,btn2,...")
    Log: 0xff00,
};

const MAX_RING = 500;
const state = {
    clients: new Map(),  // socket -> { role, pid, modulePath, dialog, buttons }
    byRole: {},          // role -> { connected, pid, dialog, buttons }  (serialized to /api/state)
    socketByRole: {},    // role -> current socket (NOT serialized); the authoritative owner, so a
                         // relaunch supersedes it and a stale socket's late 'close' can't flip it offline.
    logs: [],
    packets: [],
    chat: [],
    events: [],
};

function ring(arr, item) {
    arr.push(item);
    if (arr.length > MAX_RING) arr.shift();
}

function nowIso() {
    return new Date().toISOString();
}

function roleOf(socket) {
    const c = state.clients.get(socket);
    return (c && c.role) || '?';
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

function send(socket, op, payload) {
    try { socket.write(encodeFrame(op, payload)); } catch (e) { /* socket gone */ }
}

// u16 strLen | str ...  (matches the autonav remote-command parser)
function encodeStr(s) {
    const b = Buffer.from(s || '', 'utf8');
    const out = Buffer.alloc(2 + b.length);
    out.writeUInt16LE(b.length, 0);
    b.copy(out, 2);
    return out;
}

// ---- net-message decoding --------------------------------------------------
function decodeClassName(bytes) {
    const s = bytes.slice(0, Math.min(bytes.length, 64)).toString('latin1');
    const m = s.match(/\.\?A[UV]([A-Za-z0-9_]+)@@/);
    return m ? m[1] : null;
}

function extractText(bytes) {
    let best = '', cur = '';
    for (const b of bytes) {
        if (b >= 0x20 && b < 0x7f) cur += String.fromCharCode(b);
        else { if (cur.length > best.length) best = cur; cur = ''; }
    }
    if (cur.length > best.length) best = cur;
    return best.length >= 2 ? best.trim() : '';
}

const CHAT_CLASSES = new Set(['CCmdChatMsg', 'CChatMsg']);
const EVENT_CLASSES = new Set([
    'CConnectMsg', 'CJoinGameMsg', 'CDisconnectMsg', 'CCmdBeginTurnMsg', 'CCmdEndTurnMsg',
]);

function onTrace(socket, dir, payload) {
    if (payload.length < 12) return;
    const self = payload.readUInt32LE(0);
    const peer = payload.readUInt32LE(4);
    const size = payload.readUInt32LE(8);
    const body = payload.slice(12, 12 + Math.min(size, payload.length - 12));
    const cls = decodeClassName(body);
    const role = roleOf(socket);
    ring(state.packets, {
        t: nowIso(), role, dir, self: self.toString(16), peer, size,
        cls: cls || '?', hex: body.slice(0, 32).toString('hex'),
    });
    if (cls && CHAT_CLASSES.has(cls)) {
        const text = extractText(body);
        ring(state.chat, { t: nowIso(), role, dir, peer, cls, text });
        console.log(`[chat] (${role} ${dir} from ${peer}) ${text}`);
    } else if (cls && EVENT_CLASSES.has(cls)) {
        ring(state.events, { t: nowIso(), role, dir, peer, cls });
        console.log(`[event] ${role} ${cls} (${dir} peer=${peer})`);
    }
}

// ---- named-pipe server (agent connections) ---------------------------------
function parseHello(payload) {
    if (payload.length < 12) return null; // too short for version|pid|modLen — malformed
    const version = payload.readUInt32LE(0);
    const pid = payload.readUInt32LE(4);
    let modLen = payload.readUInt32LE(8);
    if (modLen > payload.length - 12) modLen = payload.length - 12; // clamp a corrupt length
    const modulePath = payload.slice(12, 12 + modLen).toString('utf8');
    let role = '';
    const roleOff = 12 + modLen;
    if (payload.length >= roleOff + 4) {
        const roleLen = payload.readUInt32LE(roleOff);
        if (payload.length >= roleOff + 4 + roleLen)
            role = payload.slice(roleOff + 4, roleOff + 4 + roleLen).toString('utf8');
    }
    return { version, pid, modulePath, role };
}

function handleMessage(socket, op, flags, payload) {
    switch (op) {
    case Op.Hello: {
        const h = parseHello(payload);
        if (!h) { console.error('[pipe] malformed Hello dropped'); break; }
        const role = h.role || `pid${h.pid}`;
        // A re-registering role (relaunch) supersedes the prior socket: evict it so clientByRole
        // never returns a dead duplicate, and its late 'close' can't touch the live entry.
        const prev = state.socketByRole[role];
        if (prev && prev !== socket) { state.clients.delete(prev); try { prev.destroy(); } catch (e) { /* gone */ } }
        state.clients.set(socket, { role, pid: h.pid, modulePath: h.modulePath, dialog: null, buttons: [] });
        state.byRole[role] = { connected: true, pid: h.pid, dialog: null, buttons: [], reachedStrategic: false };
        state.socketByRole[role] = socket;
        console.log(`[hello] role=${role} pid=${h.pid} v${h.version}`);
        const ack = Buffer.alloc(8);
        ack.writeUInt32LE(1, 0);
        ack.writeUInt32LE(PROTOCOL_VERSION, 4);
        send(socket, Op.HelloAck, ack);
        break;
    }
    case Op.Dialog: {
        const s = payload.toString('utf8');
        const nl = s.indexOf('\n');
        const dialog = nl >= 0 ? s.slice(0, nl) : s;
        const buttons = nl >= 0 && s.length > nl + 1 ? s.slice(nl + 1).split(',').filter(Boolean) : [];
        const c = state.clients.get(socket);
        if (c) {
            c.dialog = dialog; c.buttons = buttons;
            const r = state.byRole[c.role];
            if (r) {
                r.dialog = dialog; r.buttons = buttons;
                // Sticky "reached the map": DLG_ISO_PAL (the isometric map view) appears BEFORE
                // the first-turn popups; DLG_STRATEGIC is the same map. Latch on either — the
                // dialog only flickers through, so a poll can miss it.
                if (dialog === 'DLG_STRATEGIC' || dialog === 'DLG_ISO_PAL') r.reachedStrategic = true;
            }
        }
        console.log(`[ui] ${roleOf(socket)} -> ${dialog} [${buttons.join(',')}]`);
        break;
    }
    case Op.Log: {
        const line = payload.toString('utf8');
        ring(state.logs, { t: nowIso(), role: roleOf(socket), line });
        console.log(`[log] (${roleOf(socket)}) ${line}`);
        break;
    }
    case Op.PacketTraceRx: onTrace(socket, 'RX', payload); break;
    case Op.PacketTrace: onTrace(socket, 'TX', payload); break;
    case Op.Goodbye: console.log(`[goodbye] ${roleOf(socket)} disconnecting`); break;
    default:
        ring(state.logs, { t: nowIso(), role: roleOf(socket), line: `unhandled op 0x${op.toString(16)} (${payload.length}B)` });
        break;
    }
}

function attachParser(socket) {
    let buf = Buffer.alloc(0);
    socket.on('data', (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        for (;;) {
            if (buf.length < 4) break;
            const len = buf.readUInt32LE(0);
            if (len < 4 || len > 16 * 1024 * 1024) {
                console.error(`[pipe] bad frame length ${len}; dropping`);
                socket.destroy();
                return;
            }
            if (buf.length < 4 + len) break;
            const frame = buf.slice(4, 4 + len);
            buf = buf.slice(4 + len);
            const op = frame.readUInt16LE(0);
            const flags = frame.readUInt16LE(2);
            const payload = frame.slice(4);
            try { handleMessage(socket, op, flags, payload); }
            catch (e) { console.error('[pipe] handler error', e); }
        }
    });
}

const pipeServer = net.createServer((socket) => {
    console.log('[pipe] client connected');
    state.clients.set(socket, { role: '?', pid: 0, modulePath: '', dialog: null, buttons: [] });
    attachParser(socket);
    const drop = () => {
        const c = state.clients.get(socket);
        state.clients.delete(socket);
        // Only flip the role offline if THIS socket is still its current owner — a late 'close'
        // from a socket already superseded by a relaunch must not knock the live one offline.
        if (c && state.socketByRole[c.role] === socket) {
            if (state.byRole[c.role]) state.byRole[c.role].connected = false;
            delete state.socketByRole[c.role];
        }
        console.log(`[pipe] ${c ? c.role : '?'} disconnected`);
    };
    socket.on('close', drop);
    socket.on('error', (e) => console.error('[pipe] socket error', e.message));
});

pipeServer.on('error', (e) => console.error('[pipe] server error', e.message));
pipeServer.listen(PIPE_NAME, () => console.log(`[pipe] listening on ${PIPE_NAME}`));

// ---- HTTP API --------------------------------------------------------------
function clientByRole(role) {
    // the role's current socket, or null once dropped — never a stale duplicate (else 503)
    const sock = state.socketByRole[role];
    return (sock && state.clients.has(sock)) ? sock : null;
}

function sendJson(res, code, obj) {
    res.writeHead(code, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(obj, null, 2));
}

const httpServer = http.createServer((req, res) => {
    const url = new URL(req.url, `http://${HTTP_HOST}:${HTTP_PORT}`);
    const path = url.pathname;
    const q = url.searchParams;

    if (req.method === 'GET' && path === '/api/status') return sendJson(res, 200, { roles: state.byRole });
    // Live per-role UI state for the dispatcher: { roles: { host:{connected,dialog,buttons}, ... } }.
    if (req.method === 'GET' && path === '/api/state') return sendJson(res, 200, { roles: state.byRole });
    if (req.method === 'GET' && path === '/api/log') return sendJson(res, 200, { logs: state.logs.slice(-100), packets: state.packets.slice(-100) });
    if (req.method === 'GET' && path === '/api/chat') return sendJson(res, 200, { chat: state.chat.slice(-100) });
    if (req.method === 'GET' && path === '/api/events') return sendJson(res, 200, { events: state.events.slice(-100) });
    if (req.method === 'GET' && path === '/api/packets') return sendJson(res, 200, { packets: state.packets.slice(-200) });

    if (req.method === 'POST' && path === '/api/invoke') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no agent for role ' + q.get('role') });
        const dlg = q.get('dlg') || '', btn = q.get('btn') || '';
        send(sock, Op.InvokeButton, Buffer.concat([encodeStr(dlg), encodeStr(btn)]));
        return sendJson(res, 200, { sent: { role: roleOf(sock), invoke: { dlg, btn } } });
    }
    if (req.method === 'POST' && path === '/api/select') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no agent for role ' + q.get('role') });
        const dlg = q.get('dlg') || '', lb = q.get('lb') || '', index = parseInt(q.get('index') || '0', 10);
        const idx = Buffer.alloc(4); idx.writeInt32LE(index, 0);
        send(sock, Op.SetSelection, Buffer.concat([encodeStr(dlg), encodeStr(lb), idx]));
        return sendJson(res, 200, { sent: { role: roleOf(sock), select: { dlg, lb, index } } });
    }
    sendJson(res, 404, { error: 'not found' });
});

httpServer.listen(HTTP_PORT, HTTP_HOST, () => console.log(`[http] api on http://${HTTP_HOST}:${HTTP_PORT}`));

process.on('SIGINT', () => { console.log('\nshutting down'); process.exit(0); });
