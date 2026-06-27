/*
 * d2lobby.packetlogic relay, part of the publishable test/logging system.
 *
 * A dependency-free Node.js server the mss32 bridge connects to over a Windows named pipe.
 * MULTI-CLIENT (host + joiner, each tagged by role from its Hello), a dumb mirror + command
 * relay with no test logic: it mirrors each agent's live UI (current dialog + every widget with
 * its state) and packets, and relays the dispatcher's invoke/select/spin/edit commands to a role.
 * The dispatcher reads GET /api/ui and POSTs /api/ui/{invoke,select,spin,edit}, so two instances
 * coordinate with no files.
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

// Opcodes (mirror testdrv/packetlogicbridge.cpp, public protocol only).
const Op = {
    Hello: 0x0001,
    HelloAck: 0x0002,
    Goodbye: 0x0003,
    ConfigurePatches: 0x0004,
    LocalPlayerHandle: 0x0007,
    PacketTrace: 0x0202,    // TX
    PacketTraceRx: 0x0203,  // RX
    InvokeButton: 0x0300,   // -> client: click a button
    SetSelection: 0x0301,   // -> client: set a listbox selection
    SetSpin: 0x0302,        // -> client: set a spin-button option
    SetEditText: 0x0303,    // -> client: set an edit-box's text
    CommandResult: 0x0304,  // <- client: outcome of a command (u32 seq | u8 found)
    MoveStack: 0x0305,      // -> client: move a stack to a tile (u16 stackId | i32 x | i32 y)
    InvokeToggle: 0x0306,   // -> client: flip a toggle button (u16 dlg | u16 toggle)
    HireMerc: 0x0307,       // -> client: buy a merc from a camp into a stack (u16 campId | u16 stackId | u16 unitId)
    MoveGroupUnit: 0x0308,  // -> client: move/swap a unit between formation slots (u16 stackId | i32 src | i32 dst)
    UiSnapshot: 0x0410,     // <- client: current dialog + all its widgets with state (JSON)
    WorldSnapshot: 0x0411,  // <- client: players' resources + all map stacks (JSON)
    Log: 0xff00,
};

const MAX_RING = 500;
const state = {
    clients: new Map(),  // socket -> { role, pid, modulePath, dialog, widgets, buttons }
    byRole: {},          // role -> { connected, pid, dialog, widgets, buttons }  (serialized to /api/state)
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

function u32(n) {
    const b = Buffer.alloc(4);
    b.writeUInt32LE(n >>> 0, 0);
    return b;
}

// ---- command-result correlation --------------------------------------------
// Each command carries a sequence id; the client answers with a CommandResult so the POST can
// report whether the addressed dialog and widget were found. seq -> resolver, cleared on answer
// or timeout.
let g_seq = 0;
const g_pending = new Map();

function awaitResult(seq, ms) {
    return new Promise((resolve) => {
        const timer = setTimeout(() => { g_pending.delete(seq); resolve(null); }, ms);
        g_pending.set(seq, { resolve, timer });
    });
}

// Send a command to a client and resolve to its found flag (true/false), or null if no answer.
function sendCommand(sock, op, body) {
    const seq = (g_seq = (g_seq + 1) >>> 0);
    send(sock, op, Buffer.concat([u32(seq), body]));
    return awaitResult(seq, 5000);
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
    if (payload.length < 12) return null; // too short for version|pid|modLen, malformed
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
        state.clients.set(socket, { role, pid: h.pid, modulePath: h.modulePath, dialog: null, widgets: [], buttons: [], players: [], stacks: [], camps: [], bags: [] });
        state.byRole[role] = { connected: true, pid: h.pid, dialog: null, widgets: [], buttons: [], players: [], stacks: [], camps: [], bags: [], reachedStrategic: false, sawBeginTurn: false };
        state.socketByRole[role] = socket;
        console.log(`[hello] role=${role} pid=${h.pid} v${h.version}`);
        const ack = Buffer.alloc(8);
        ack.writeUInt32LE(1, 0);
        ack.writeUInt32LE(PROTOCOL_VERSION, 4);
        send(socket, Op.HelloAck, ack);
        break;
    }
    case Op.UiSnapshot: {
        // JSON: { dialog: "DLG_X", widgets: [ {name, type, state}, ... ] }. The DLL escapes all
        // strings, so a parse failure means a torn frame, log and skip, never crash the relay.
        let snap;
        try { snap = JSON.parse(payload.toString('utf8')); }
        catch (e) { console.error(`[ui] ${roleOf(socket)} bad snapshot JSON: ${e.message}`); break; }
        const dialog = snap.dialog || '';
        const widgets = Array.isArray(snap.widgets) ? snap.widgets : [];
        const buttons = widgets.filter((w) => w.type === 'button').map((w) => w.name); // back-compat view
        const c = state.clients.get(socket);
        if (c) {
            c.dialog = dialog; c.widgets = widgets; c.buttons = buttons;
            const r = state.byRole[c.role];
            if (r) {
                r.dialog = dialog; r.widgets = widgets; r.buttons = buttons;
                // Sticky "reached the map": DLG_ISO_PAL (the isometric map view) appears BEFORE
                // the first-turn popups; DLG_STRATEGIC is the same map. Latch on either, the
                // dialog only flickers through, so a poll can miss it.
                if (dialog === 'DLG_STRATEGIC' || dialog === 'DLG_ISO_PAL') r.reachedStrategic = true;
                if (dialog === 'DLG_BEGIN_TURN') r.sawBeginTurn = true; // a new day / turn began for this role
            }
        }
        console.log(`[ui] ${roleOf(socket)} -> ${dialog} (${widgets.length} widgets)`);
        break;
    }
    case Op.WorldSnapshot: {
        // JSON: { day, players: [...], stacks: [...], camps: [...], bags: [...] }. Same DLL escaping
        // guarantees as UiSnapshot, so a parse failure means a torn frame: log and skip, never crash.
        let snap;
        try { snap = JSON.parse(payload.toString('utf8')); }
        catch (e) { console.error(`[world] ${roleOf(socket)} bad snapshot JSON: ${e.message}`); break; }
        const players = Array.isArray(snap.players) ? snap.players : [];
        const stacks = Array.isArray(snap.stacks) ? snap.stacks : [];
        const camps = Array.isArray(snap.camps) ? snap.camps : [];
        const bags = Array.isArray(snap.bags) ? snap.bags : [];
        const c = state.clients.get(socket);
        if (c) {
            c.day = snap.day; c.players = players; c.stacks = stacks; c.camps = camps; c.bags = bags;
            const r = state.byRole[c.role];
            if (r) { r.day = snap.day; r.players = players; r.stacks = stacks; r.camps = camps; r.bags = bags; }
        }
        console.log(`[world] ${roleOf(socket)} -> day ${snap.day}, ${players.length} players, ${stacks.length} stacks, ${camps.length} camps, ${bags.length} bags`);
        break;
    }
    case Op.Log: {
        const line = payload.toString('utf8');
        ring(state.logs, { t: nowIso(), role: roleOf(socket), line });
        console.log(`[log] (${roleOf(socket)}) ${line}`);
        break;
    }
    case Op.CommandResult: {
        if (payload.length < 5) break;
        const seq = payload.readUInt32LE(0);
        const found = payload.readUInt8(4) !== 0;
        const p = g_pending.get(seq);
        if (p) { clearTimeout(p.timer); g_pending.delete(seq); p.resolve(found); }
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
        // Only flip the role offline if THIS socket is still its current owner, a late 'close'
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
    // the role's current socket, or null once dropped, never a stale duplicate (else 503)
    const sock = state.socketByRole[role];
    return (sock && state.clients.has(sock)) ? sock : null;
}

function sendJson(res, code, obj) {
    res.writeHead(code, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(obj, null, 2));
}

const httpServer = http.createServer(async (req, res) => {
    const url = new URL(req.url, `http://${HTTP_HOST}:${HTTP_PORT}`);
    const path = url.pathname;
    const q = url.searchParams;

    if (req.method === 'GET' && path === '/api/status') return sendJson(res, 200, { roles: state.byRole });
    // Per-role status + latches for the dispatcher: { roles: { host:{connected,dialog,widgets,...}, ... } }.
    if (req.method === 'GET' && path === '/api/state') return sendJson(res, 200, { roles: state.byRole });
    // The live UI snapshot. With ?role=, one role's { role, dialog, widgets }; without, every role.
    if (req.method === 'GET' && path === '/api/ui') {
        const role = q.get('role');
        if (role) {
            const r = state.byRole[role];
            return sendJson(res, 200, { role, dialog: r ? r.dialog : null, widgets: r ? r.widgets : [] });
        }
        const roles = {};
        for (const [name, r] of Object.entries(state.byRole)) roles[name] = { dialog: r.dialog, widgets: r.widgets };
        return sendJson(res, 200, { roles });
    }
    // The live world snapshot. With ?role=, one role's { role, day, players, stacks, camps, bags };
    // without, every role.
    if (req.method === 'GET' && path === '/api/world') {
        const role = q.get('role');
        if (role) {
            const r = state.byRole[role];
            return sendJson(res, 200, { role, day: r ? r.day : null, players: r ? (r.players || []) : [], stacks: r ? (r.stacks || []) : [], camps: r ? (r.camps || []) : [], bags: r ? (r.bags || []) : [] });
        }
        const roles = {};
        for (const [name, r] of Object.entries(state.byRole)) roles[name] = { day: r.day, players: r.players || [], stacks: r.stacks || [], camps: r.camps || [], bags: r.bags || [] };
        return sendJson(res, 200, { roles });
    }
    if (req.method === 'GET' && path === '/api/log') return sendJson(res, 200, { logs: state.logs.slice(-100), packets: state.packets.slice(-100) });
    if (req.method === 'GET' && path === '/api/chat') return sendJson(res, 200, { chat: state.chat.slice(-100) });
    if (req.method === 'GET' && path === '/api/events') return sendJson(res, 200, { events: state.events.slice(-100) });
    if (req.method === 'GET' && path === '/api/packets') return sendJson(res, 200, { packets: state.packets.slice(-200) });

    // UI actions, grouped under /api/ui/*. Each resolves the role's agent socket then forwards one
    // bridge command; 503 if that role has no connected agent.
    if (req.method === 'POST' && path === '/api/ui/invoke') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const dlg = q.get('dlg') || '', btn = q.get('btn') || '';
        const found = await sendCommand(sock, Op.InvokeButton, Buffer.concat([encodeStr(dlg), encodeStr(btn)]));
        return sendJson(res, 200, { role: roleOf(sock), invoke: { dlg, btn }, found });
    }
    if (req.method === 'POST' && path === '/api/ui/select') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const dlg = q.get('dlg') || '', lb = q.get('lb') || '', index = parseInt(q.get('index') || '0', 10);
        const idx = Buffer.alloc(4); idx.writeInt32LE(index, 0);
        const found = await sendCommand(sock, Op.SetSelection, Buffer.concat([encodeStr(dlg), encodeStr(lb), idx]));
        return sendJson(res, 200, { role: roleOf(sock), select: { dlg, lb, index }, found });
    }
    if (req.method === 'POST' && path === '/api/ui/spin') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const dlg = q.get('dlg') || '', spin = q.get('spin') || '', index = parseInt(q.get('index') || '0', 10);
        const idx = Buffer.alloc(4); idx.writeInt32LE(index, 0);
        const found = await sendCommand(sock, Op.SetSpin, Buffer.concat([encodeStr(dlg), encodeStr(spin), idx]));
        return sendJson(res, 200, { role: roleOf(sock), spin: { dlg, spin, index }, found });
    }
    if (req.method === 'POST' && path === '/api/ui/edit') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const dlg = q.get('dlg') || '', edit = q.get('edit') || '', text = q.get('text') || '';
        const found = await sendCommand(sock, Op.SetEditText, Buffer.concat([encodeStr(dlg), encodeStr(edit), encodeStr(text)]));
        return sendJson(res, 200, { role: roleOf(sock), edit: { dlg, edit, text }, found });
    }
    // Move a stack: resolve the role's agent, forward { stackId, x, y }; the agent builds the path
    // with the game's own cost/passability and issues sendStackMoveMsg. `found` = the move was issued.
    if (req.method === 'POST' && path === '/api/ui/move') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const id = q.get('id') || '', x = parseInt(q.get('x') || '0', 10), y = parseInt(q.get('y') || '0', 10);
        const bx = Buffer.alloc(4); bx.writeInt32LE(x, 0);
        const by = Buffer.alloc(4); by.writeInt32LE(y, 0);
        const found = await sendCommand(sock, Op.MoveStack, Buffer.concat([encodeStr(id), bx, by]));
        return sendJson(res, 200, { role: roleOf(sock), move: { id, x, y }, found });
    }
    if (req.method === 'POST' && path === '/api/ui/toggle') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const dlg = q.get('dlg') || '', tog = q.get('tog') || '';
        const found = await sendCommand(sock, Op.InvokeToggle, Buffer.concat([encodeStr(dlg), encodeStr(tog)]));
        return sendJson(res, 200, { role: roleOf(sock), toggle: { dlg, tog }, found });
    }
    // Buy a mercenary from a camp into a stack's first fitting free slot (testdrv worldactions::hireMerc,
    // which sends the engine's CSiteBuyUnitMsg from the acting client; the host applies + replicates).
    // `found` = the hire message was sent (own stack, our turn, a free slot).
    if (req.method === 'POST' && path === '/api/ui/hire') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const camp = q.get('camp') || '', stack = q.get('stack') || '', unit = q.get('unit') || '';
        const found = await sendCommand(sock, Op.HireMerc,
            Buffer.concat([encodeStr(camp), encodeStr(stack), encodeStr(unit)]));
        return sendJson(res, 200, { role: roleOf(sock), hire: { camp, stack, unit }, found });
    }
    // Move/swap a unit between formation slots of a stack (testdrv worldactions::moveGroupUnit, which
    // sends the engine's CStackSwapUnitMsg; the host applies + replicates). Empty dst = move (src
    // empties), occupied dst = swap. `found` = the message was sent (own stack, our turn, src occupied).
    if (req.method === 'POST' && path === '/api/ui/move-unit') {
        const sock = clientByRole(q.get('role'));
        if (!sock) return sendJson(res, 503, { error: 'no client for role ' + q.get('role') });
        const stack = q.get('stack') || '', src = parseInt(q.get('src'), 10), dst = parseInt(q.get('dst'), 10);
        const b = Buffer.alloc(8); b.writeInt32LE(src | 0, 0); b.writeInt32LE(dst | 0, 4);
        const found = await sendCommand(sock, Op.MoveGroupUnit, Buffer.concat([encodeStr(stack), b]));
        return sendJson(res, 200, { role: roleOf(sock), moveUnit: { stack, src, dst }, found });
    }
    sendJson(res, 404, { error: 'not found' });
});

httpServer.listen(HTTP_PORT, HTTP_HOST, () => console.log(`[http] api on http://${HTTP_HOST}:${HTTP_PORT}`));

process.on('SIGINT', () => { console.log('\nshutting down'); process.exit(0); });
