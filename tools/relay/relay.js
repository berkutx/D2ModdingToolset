/*
 * Disciples 2 DebugTest relay.
 *
 * This process is deliberately only a broker: native MSS code reports typed UI/world/turn
 * evidence and consumes commands on the game's natural UI frame.  The relay binds one HTTP
 * mutation to one published dialog appearance plus one native dialog owner and forwards it once.
 * A role has one socket for the finite run, and each accepted command is written exactly once.
 *
 * Default transport: \\.\pipe\d2lobby.packetlogic, HTTP http://127.0.0.1:8077.
 * Tests/Wine may select explicit endpoints through D2TESTDRV_PIPE_NAME,
 * D2TESTDRV_HTTP_HOST/_PORT and D2TESTDRV_BRIDGE_TCP_HOST/_PORT.
 */

'use strict';

const net = require('net');
const http = require('http');
const { TextDecoder } = require('util');

function envValue(name, alias, fallback = '') {
    const primary = process.env[name] || '';
    const alternate = process.env[alias] || '';
    if (primary && alternate && primary !== alternate)
        throw new Error(`${name} and ${alias} conflict`);
    return primary || alternate || fallback;
}

function envPort(name, defaultValue = null, alias = '') {
    const raw = alias ? envValue(name, alias) : process.env[name];
    if (raw === undefined || raw === '') {
        if (defaultValue !== null) return defaultValue;
        throw new Error(`${name} must be set explicitly`);
    }
    if (!/^[1-9][0-9]*$/.test(raw))
        throw new Error(`${name} must be one decimal integer from 1 to 65535`);
    const port = Number(raw);
    if (!Number.isSafeInteger(port) || port > 65535)
        throw new Error(`${name} must be one decimal integer from 1 to 65535`);
    return port;
}

const PIPE_NAME = process.env.D2TESTDRV_PIPE_NAME || '\\\\.\\pipe\\d2lobby.packetlogic';
const HTTP_HOST = envValue('D2TESTDRV_HTTP_HOST', 'D2_RELAY_HTTP_HOST', '127.0.0.1');
const HTTP_PORT = envPort('D2TESTDRV_HTTP_PORT', 8077, 'D2_RELAY_HTTP_PORT');
const BRIDGE_TCP_HOST_TEXT = envValue('D2TESTDRV_BRIDGE_TCP_HOST', 'D2_RELAY_TCP_HOST',
    process.env.D2_RELAY_TCP_PORT ? '127.0.0.1' : '');
const BRIDGE_TCP_PORT_TEXT = envValue('D2TESTDRV_BRIDGE_TCP_PORT', 'D2_RELAY_TCP_PORT');
if (!!BRIDGE_TCP_HOST_TEXT !== !!BRIDGE_TCP_PORT_TEXT) {
    throw new Error('D2TESTDRV_BRIDGE_TCP_HOST and D2TESTDRV_BRIDGE_TCP_PORT must be set together');
}
const BRIDGE_TCP_ENABLED = BRIDGE_TCP_HOST_TEXT !== '';
const BRIDGE_TCP_HOST = BRIDGE_TCP_HOST_TEXT;
const BRIDGE_TCP_PORT = BRIDGE_TCP_ENABLED ? envPort('D2TESTDRV_BRIDGE_TCP_PORT', null, 'D2_RELAY_TCP_PORT') : null;
const INSTANCE_ID = envValue('D2TESTDRV_RUN_ID', 'D2_RELAY_INSTANCE_ID', `${process.pid}-${Date.now()}`);
const PROTOCOL_VERSION = 7;

// Public DebugTest wire protocol. Production simultaneous-turn traffic uses a separate relay.
const Op = Object.freeze({
    Hello: 0x0001,
    HelloAck: 0x0002,
    Goodbye: 0x0003,
    BeginApplied: 0x0204,
    EndSendReturned: 0x0205,
    StartupJoinObserved: 0x0206,
    StartupBeginObserved: 0x0207,
    StartupDirectedBeginObserved: 0x0208,
    StartupCompleteObserved: 0x0209,
    BeginSendReturned: 0x020a,
    InvokeButton: 0x0300,
    SetSelection: 0x0301,
    SetSpin: 0x0302,
    SetEditText: 0x0303,
    CommandResult: 0x0304,
    MoveStack: 0x0305,
    InvokeToggle: 0x0306,
    HireMerc: 0x0307,
    MoveGroupUnit: 0x0308,
    DismissUnit: 0x0309,
    SelectScenarioPath: 0x030a,
    EnableToggle: 0x030b,
    EnableAutoBattle: 0x030c,
    AutoBattleKickResult: 0x030d,
    CommandStarted: 0x030e,
    InvokePairedEndTurn: 0x030f,
    ReleasePairedEndTurn: 0x0310,
    MoveStackToward: 0x0311,
    ReleaseStartupActions: 0x0312,
    UiSnapshot: 0x0410,
    WorldSnapshot: 0x0411,
    LegacyStacksSnapshot: 0x0412,
    LobbyChat: 0x0413,
    Log: 0xff00,
});

const MAX_FRAME_BYTES = 16 * 1024 * 1024;
const MAX_EVIDENCE = 4096;
const MAX_PENDING_COMMANDS = 64;
const DEFAULT_COMMAND_TIMEOUT_MS = 5000;
const AUTO_BATTLE_COMMAND_TIMEOUT_MS = 25000;
const MIN_COMMAND_TIMEOUT_MS = 1000;
const MAX_COMMAND_TIMEOUT_MS = 120000;
const MAX_TURN_WAITERS = 16;
const MAX_TURN_WAIT_MS = 120000;
const MAX_UI_READY_WAITERS = 16;
const MAX_UI_READY_WAIT_MS = 600000;
const MAX_WORLD_PAIR_WAITERS = 16;
const MAX_WORLD_PAIR_WAIT_MS = 120000;
const MAX_UI_INVOKE_INTENTS = 16;
const MAX_UI_INVOKE_STABLE_MS = 5000;
const MAX_LEGACY_STACKS = 256;
const utf8 = new TextDecoder('utf-8', { fatal: true });

const emptyLegacyStacksSnapshot = Object.freeze({
    sourceRole: 'host', sequence: 0, stacks: Object.freeze([]),
});

const state = {
    clients: new Map(),
    byRole: Object.create(null),
    socketByRole: Object.create(null),
    chatByRole: Object.create(null),
    agentListening: false,
    terminalFault: null,
    shuttingDown: false,
    uiSequence: 0,
    worldSequence: 0,
    turnSequence: 0,
    legacyStacksSequence: 0,
    legacyStacksSnapshot: emptyLegacyStacksSnapshot,
    uiHistory: [],
    worldHistory: [],
    turnHistory: [],
    startupActionsRelease: null,
};

let commandSequence = 0;
const pendingCommands = new Map();
const turnWaiters = new Set();
const uiReadyWaiters = new Set();
const worldPairWaiters = new Set();
const uiInvokeIntents = new Map();
let endTurnPairIntent = null;

function settleCommandStarted(pending, value) {
    if (!pending || pending.startedSettled) return;
    pending.startedSettled = true;
    pending.resolveStarted(value);
}

function nowIso() {
    return new Date().toISOString();
}

function roleOf(socket) {
    return state.clients.get(socket)?.role || '?';
}

function evidenceForFilter(history, filter) {
    return history.filter((entry) => entry.seq > filter.after
        && (!filter.role || entry.role === filter.role));
}

function turnHistoryResponse(filter, timedOut = false) {
    return {
        latestSeq: state.turnSequence,
        events: evidenceForFilter(state.turnHistory, filter),
        terminalFault: state.terminalFault,
        timedOut,
    };
}

function removeTurnWaiter(waiter) {
    if (!turnWaiters.delete(waiter)) return false;
    clearTimeout(waiter.timer);
    waiter.req.off('aborted', waiter.onAbort);
    waiter.res.off('close', waiter.onAbort);
    return true;
}

function finishTurnWaiter(waiter, timedOut = false) {
    if (!removeTurnWaiter(waiter)) return;
    if (!waiter.res.destroyed && !waiter.res.writableEnded)
        sendJson(waiter.res, 200, turnHistoryResponse(waiter.filter, timedOut));
}

function notifyTurnWaiters() {
    for (const waiter of [...turnWaiters]) {
        if (state.terminalFault
            || evidenceForFilter(state.turnHistory, waiter.filter).length !== 0) {
            finishTurnWaiter(waiter, false);
        }
    }
}

function waitForTurnEvidence(req, res, filter, waitMs) {
    if (turnWaiters.size >= MAX_TURN_WAITERS) {
        return sendJson(res, 503, {
            error: `turn-history waiter capacity ${MAX_TURN_WAITERS} exhausted`,
        });
    }
    const waiter = { req, res, filter, timer: null, onAbort: null };
    waiter.onAbort = () => { removeTurnWaiter(waiter); };
    waiter.timer = setTimeout(() => finishTurnWaiter(waiter, true), waitMs);
    turnWaiters.add(waiter);
    req.once('aborted', waiter.onAbort);
    res.once('close', waiter.onAbort);
}

function exactReadyUiObservation(filter, observation) {
    if (!observation || observation.role !== filter.role
        || observation.connected !== true || !Number.isInteger(observation.pid)
        || observation.pid < 1 || typeof observation.modulePath !== 'string'
        || observation.modulePath.length === 0
        || observation.uiSeq <= filter.after || observation.dialog !== filter.dialog
        || observation.dialogReady !== true
        || observation.dialogInstance !== observation.dialogAppearance
        || !Array.isArray(observation.targets)) return null;
    const roots = observation.targets.filter((target) => target.dialog === filter.dialog);
    if (roots.length !== 1 || !Array.isArray(roots[0].widgets)) return null;
    const buttons = roots[0].widgets.filter((widget) => widget.name === filter.button
        && widget.type === 'button');
    if (buttons.length !== 1 || buttons[0].state.enabled !== true) return null;
    return observation;
}

function uiReadyWaitResponse(observation = null, timedOut = false) {
    return {
        latestSeq: state.uiSequence,
        observation,
        terminalFault: state.terminalFault,
        timedOut,
    };
}

function removeUiReadyWaiter(waiter) {
    if (!uiReadyWaiters.delete(waiter)) return false;
    clearTimeout(waiter.timer);
    waiter.req.off('aborted', waiter.onAbort);
    waiter.res.off('close', waiter.onAbort);
    return true;
}

function finishUiReadyWaiter(waiter, observation = null, timedOut = false) {
    if (!removeUiReadyWaiter(waiter)) return;
    if (!waiter.res.destroyed && !waiter.res.writableEnded)
        sendJson(waiter.res, 200, uiReadyWaitResponse(observation, timedOut));
}

function notifyUiReadyWaiters(observation = null) {
    for (const waiter of [...uiReadyWaiters]) {
        if (state.terminalFault) {
            finishUiReadyWaiter(waiter);
            continue;
        }
        const exact = exactReadyUiObservation(waiter.filter, observation);
        if (exact) finishUiReadyWaiter(waiter, exact, false);
    }
}

function waitForUiReady(req, res, filter) {
    if (uiReadyWaiters.size >= MAX_UI_READY_WAITERS) {
        return sendJson(res, 503, {
            error: `UI-ready waiter capacity ${MAX_UI_READY_WAITERS} exhausted`,
        });
    }
    const waiter = { req, res, filter, timer: null, onAbort: null };
    waiter.onAbort = () => { removeUiReadyWaiter(waiter); };
    waiter.timer = setTimeout(
        () => finishUiReadyWaiter(waiter, null, true), filter.waitMs);
    uiReadyWaiters.add(waiter);
    req.once('aborted', waiter.onAbort);
    res.once('close', waiter.onAbort);
    console.log(`[ui-ready-wait] armed role=${filter.role}`
        + ` ${filter.dialog}::${filter.button} after=${filter.after}`);
}

function currentUiObservation(role) {
    const current = state.byRole[role];
    if (!current) return null;
    return {
        role, connected: current.connected, pid: current.pid, modulePath: current.modulePath,
        dialog: current.dialog,
        dialogInstance: current.dialogInstance,
        dialogAppearance: current.dialogAppearance,
        dialogReady: current.dialogReady,
        strategicIdle: current.strategicIdle,
        mapLoaded: current.mapLoaded,
        startupActionsHeld: current.startupActionsHeld,
        uiSeq: current.uiSeq,
        widgets: current.widgets,
        targets: current.targets,
    };
}

function exactUiInvokeOwner(intent, observation) {
    const strategicEndTurn = intent.dialog === 'DLG_STRATEGIC'
        && intent.button === 'BTN_END_TURN';
    if (!observation || observation.role !== intent.role
        || observation.uiSeq <= intent.after || observation.dialog !== intent.dialog
        || observation.dialogReady !== true
        || (strategicEndTurn && observation.strategicIdle !== true)
        || observation.dialogInstance !== observation.dialogAppearance
        || !Array.isArray(observation.targets)) return null;
    const roots = observation.targets.filter((target) => target.dialog === intent.dialog);
    if (roots.length !== 1 || !Array.isArray(roots[0].widgets)) return null;
    const buttons = roots[0].widgets.filter((widget) => widget.name === intent.button
        && widget.type === 'button');
    if (buttons.length !== 1 || buttons[0].state.enabled !== true) return null;
    return roots[0].instance;
}

function removeUiInvokeIntent(intent) {
    if (uiInvokeIntents.get(intent.role) !== intent) return false;
    uiInvokeIntents.delete(intent.role);
    clearTimeout(intent.timer);
    clearTimeout(intent.stableTimer);
    intent.stableTimer = null;
    intent.candidate = null;
    intent.req.off('aborted', intent.onAbort);
    intent.res.off('close', intent.onAbort);
    return true;
}

function finishUiInvokeIntentsOnFault() {
    for (const intent of [...uiInvokeIntents.values()]) {
        if (!removeUiInvokeIntent(intent)) continue;
        intent.phase = 'terminal';
        sendJson(intent.res, 500, {
            error: 'relay is terminal', terminalFault: state.terminalFault,
        });
    }
}

function dispatchUiInvokeIntent(intent, observation, owner) {
    if (!removeUiInvokeIntent(intent)) return false;
    intent.phase = 'issued';
    const captured = {
        role: observation.role,
        dialog: observation.dialog,
        dialogInstance: observation.dialogInstance,
        dialogAppearance: observation.dialogAppearance,
        dialogReady: observation.dialogReady,
        strategicIdle: observation.strategicIdle,
        uiSeq: observation.uiSeq,
        widgets: observation.widgets,
        targets: observation.targets,
    };
    console.log(`[ui-intent] issued role=${intent.role} ${intent.dialog}::${intent.button}`
        + ` appearance=${captured.dialogAppearance} owner=${owner} uiSeq=${captured.uiSeq}`);
    sendCommand(intent.socket, Op.InvokeButton, Buffer.concat([
        u32(captured.dialogAppearance), u32(owner),
        encodeStr(intent.dialog), encodeStr(intent.button),
    ]), intent.commandTimeoutMs).then((found) => {
        intent.phase = 'completed';
        sendCommandJson(intent.res, {
            found,
            role: intent.role,
            invoke: {
                dlg: intent.dialog, btn: intent.button,
                appearance: captured.dialogAppearance, instance: owner,
            },
            observation: captured,
        });
    }).catch((error) => {
        faultRelay(`UI invoke intent completion threw: ${error.message}`, intent.socket);
        sendCommandJson(intent.res, { found: null });
    });
    return true;
}

function issueUiInvokeIntent(intent, observation) {
    const owner = exactUiInvokeOwner(intent, observation);
    if (owner === null) {
        // Any transient/unready publication breaks the optional stability
        // interval. The logical intent remains armed; no command has fired.
        clearTimeout(intent.stableTimer);
        intent.stableTimer = null;
        intent.candidate = null;
        return false;
    }
    if (intent.stableMs === 0)
        return dispatchUiInvokeIntent(intent, observation, owner);

    const appearance = observation.dialogAppearance;
    if (intent.candidate
        && intent.candidate.appearance === appearance
        && intent.candidate.owner === owner) return false;

    clearTimeout(intent.stableTimer);
    const candidate = { appearance, owner };
    intent.candidate = candidate;
    console.log(`[ui-intent] candidate role=${intent.role}`
        + ` ${intent.dialog}::${intent.button} appearance=${appearance}`
        + ` owner=${owner} stableMs=${intent.stableMs}`);
    intent.stableTimer = setTimeout(() => {
        intent.stableTimer = null;
        if (uiInvokeIntents.get(intent.role) !== intent
            || intent.candidate !== candidate) return;
        const current = currentUiObservation(intent.role);
        const currentOwner = exactUiInvokeOwner(intent, current);
        if (currentOwner === null
            || current.dialogAppearance !== candidate.appearance
            || currentOwner !== candidate.owner) {
            intent.candidate = null;
            return;
        }
        dispatchUiInvokeIntent(intent, current, currentOwner);
    }, intent.stableMs);
    return false;
}

function notifyUiInvokeIntent(observation) {
    const intent = uiInvokeIntents.get(observation.role);
    if (intent) issueUiInvokeIntent(intent, observation);
}

function armUiInvokeIntent(req, res, value) {
    if (uiInvokeIntents.has(value.role)) {
        return sendJson(res, 409, {
            error: `role ${value.role} already owns one pending UI invoke intent`,
        });
    }
    if (uiInvokeIntents.size >= MAX_UI_INVOKE_INTENTS) {
        return sendJson(res, 503, {
            error: `UI invoke intent capacity ${MAX_UI_INVOKE_INTENTS} exhausted`,
        });
    }
    const intent = {
        ...value, req, res, phase: 'armed', timer: null,
        stableTimer: null, candidate: null, onAbort: null,
    };
    intent.onAbort = () => {
        if (!removeUiInvokeIntent(intent)) return;
        intent.phase = 'terminal';
        faultRelay(`UI invoke intent for role ${intent.role} lost its HTTP owner`, intent.socket);
    };
    intent.timer = setTimeout(() => {
        if (!removeUiInvokeIntent(intent)) return;
        intent.phase = 'terminal';
        faultRelay(
            `UI invoke intent for role ${intent.role} timed out after ${intent.waitMs}ms`,
            intent.socket);
        sendJson(intent.res, 500, {
            error: 'relay is terminal', terminalFault: state.terminalFault,
        });
    }, value.waitMs);
    uiInvokeIntents.set(value.role, intent);
    req.once('aborted', intent.onAbort);
    res.once('close', intent.onAbort);
    console.log(`[ui-intent] armed role=${value.role} ${value.dialog}::${value.button}`
        + ` after=${value.after}`);
    issueUiInvokeIntent(intent, currentUiObservation(value.role));
}

function inspectEndTurnPairObservation(spec, observation) {
    if (!observation || observation.role !== spec.role
        || observation.connected !== true || !Number.isInteger(observation.pid)
        || observation.pid < 1 || typeof observation.modulePath !== 'string'
        || observation.modulePath.length === 0) {
        return { state: 'invalid', reason: `${spec.role} has no exact live UI owner` };
    }
    if (!Number.isInteger(observation.uiSeq) || observation.uiSeq < spec.uiSeq) {
        return {
            state: 'invalid',
            reason: `${spec.role} UI sequence ${observation.uiSeq} precedes saved ${spec.uiSeq}`,
        };
    }
    if (observation.dialogReady !== true
        || (observation.dialog !== 'DLG_STRATEGIC'
            && observation.dialog !== 'DLG_ISO_PAL')
        || observation.dialogInstance !== observation.dialogAppearance
        || observation.dialogAppearance !== spec.appearance
        || !Array.isArray(observation.targets)) {
        return {
            state: 'invalid',
            reason: `${spec.role} root strategic appearance drifted from ${spec.appearance}`,
        };
    }
    const roots = observation.targets.filter(
        (target) => target.dialog === 'DLG_STRATEGIC');
    if (roots.length !== 1 || roots[0].instance !== spec.instance
        || !Array.isArray(roots[0].widgets)) {
        return {
            state: 'invalid',
            reason: `${spec.role} DLG_STRATEGIC owner drifted from ${spec.instance}`,
        };
    }
    const buttons = roots[0].widgets.filter((widget) => widget.name === 'BTN_END_TURN'
        && widget.type === 'button');
    if (buttons.length !== 1 || buttons[0].state.enabled !== true) {
        return {
            state: 'invalid',
            reason: `${spec.role} exact BTN_END_TURN is absent or disabled`,
        };
    }
    return {
        state: observation.strategicIdle === true ? 'idle' : 'busy',
        observation,
    };
}

function removeEndTurnPairIntent(intent) {
    if (endTurnPairIntent !== intent) return false;
    endTurnPairIntent = null;
    clearTimeout(intent.timer);
    intent.req.off('aborted', intent.onAbort);
    intent.res.off('close', intent.onAbort);
    return true;
}

function finishEndTurnPairIntentOnFault() {
    const intent = endTurnPairIntent;
    if (!intent || !removeEndTurnPairIntent(intent)) return;
    intent.phase = 'terminal';
    if (!intent.res.destroyed && !intent.res.writableEnded) {
        sendJson(intent.res, 500, {
            error: 'relay is terminal', terminalFault: state.terminalFault,
        });
    }
}

function failArmedEndTurnPair(intent, reason) {
    if (!removeEndTurnPairIntent(intent)) return false;
    intent.phase = 'terminal';
    faultRelay(`paired strategic End Turn ${reason}`);
    if (!intent.res.destroyed && !intent.res.writableEnded) {
        sendJson(intent.res, 500, {
            error: 'relay is terminal', terminalFault: state.terminalFault,
        });
    }
    return true;
}

function capturedEndTurnObservation(spec, observation) {
    return {
        role: spec.role,
        connected: observation.connected,
        pid: observation.pid,
        modulePath: observation.modulePath,
        dialog: observation.dialog,
        dialogAppearance: observation.dialogAppearance,
        dialogInstance: observation.dialogInstance,
        strategicIdle: observation.strategicIdle,
        uiSeq: observation.uiSeq,
        invoke: {
            dlg: 'DLG_STRATEGIC', btn: 'BTN_END_TURN',
            appearance: spec.appearance, instance: spec.instance,
        },
    };
}

function tryIssueEndTurnPairIntent() {
    const intent = endTurnPairIntent;
    if (!intent || intent.phase !== 'armed' || state.terminalFault) return false;

    const hostCheck = inspectEndTurnPairObservation(
        intent.host, currentUiObservation('host'));
    const joinCheck = inspectEndTurnPairObservation(
        intent.join, currentUiObservation('join'));
    if (hostCheck.state === 'invalid')
        return failArmedEndTurnPair(intent, hostCheck.reason);
    if (joinCheck.state === 'invalid')
        return failArmedEndTurnPair(intent, joinCheck.reason);
    if (hostCheck.state !== 'idle' || joinCheck.state !== 'idle') return false;
    if (state.socketByRole.host !== intent.host.socket
        || state.socketByRole.join !== intent.join.socket) {
        return failArmedEndTurnPair(intent, 'lost one exact role socket before issue');
    }

    // Irreversibly consume the pair before the first socket write. Both arm
    // frames are written exactly once before either UI-thread edge is awaited.
    // Each game UI thread then blocks behind its leased exact EndTurn button;
    // only after both CommandStarted edges does one release frame per client
    // allow the sole callbacks to run. Peer DirectPlay RX therefore cannot be
    // dispatched by one UI thread before the other callback begins.
    if (!removeEndTurnPairIntent(intent)) return false;
    intent.phase = 'issued';
    const host = capturedEndTurnObservation(intent.host, hostCheck.observation);
    const join = capturedEndTurnObservation(intent.join, joinCheck.observation);
    const bodyFor = (captured) => Buffer.concat([
        u32(captured.invoke.appearance), u32(captured.invoke.instance),
        encodeStr(captured.invoke.dlg), encodeStr(captured.invoke.btn),
        u32(intent.commandTimeoutMs),
    ]);
    const hostIssuedAt = process.hrtime.bigint();
    const hostCommand = issueCommand(
        intent.host.socket, Op.InvokePairedEndTurn, bodyFor(host),
        intent.commandTimeoutMs);
    const joinIssuedAt = process.hrtime.bigint();
    const joinCommand = issueCommand(
        intent.join.socket, Op.InvokePairedEndTurn, bodyFor(join),
        intent.commandTimeoutMs);
    const armWriteSkewMs = Number(joinIssuedAt - hostIssuedAt) / 1e6;
    console.log(`[end-turn-pair] arms issued host ui=${host.uiSeq} join ui=${join.uiSeq}`
        + ` skew=${armWriteSkewMs.toFixed(3)}ms`);

    Promise.all([hostCommand.started, joinCommand.started]).then(
        ([hostStarted, joinStarted]) => {
            if (!hostStarted || !joinStarted)
                throw new Error('both paired EndTurn UI threads did not arm');
            const hostReleasedAt = process.hrtime.bigint();
            const hostReleased = send(
                intent.host.socket, Op.ReleasePairedEndTurn, u32(hostCommand.seq));
            const joinReleasedAt = process.hrtime.bigint();
            const joinReleased = send(
                intent.join.socket, Op.ReleasePairedEndTurn, u32(joinCommand.seq));
            if (!hostReleased || !joinReleased)
                throw new Error('both paired EndTurn release frames were not written');
            const dispatchSkewMs = Number(joinReleasedAt - hostReleasedAt) / 1e6;
            console.log(`[end-turn-pair] released host seq=${hostCommand.seq}`
                + ` join seq=${joinCommand.seq} skew=${dispatchSkewMs.toFixed(3)}ms`);
            return Promise.all([hostCommand.result, joinCommand.result]).then(
                ([hostFound, joinFound]) => ({
                    hostFound, joinFound, hostStarted, joinStarted, dispatchSkewMs,
                }));
        }).then(({ hostFound, joinFound, hostStarted, joinStarted, dispatchSkewMs }) => {
        intent.phase = 'completed';
        if (hostFound !== true || joinFound !== true) {
            faultRelay('paired strategic End Turn did not resolve both exact native owners');
            if (!intent.res.destroyed && !intent.res.writableEnded) {
                sendJson(intent.res, 500, {
                    error: 'relay is terminal', terminalFault: state.terminalFault,
                    found: false, dispatchSkewMs, armWriteSkewMs,
                    host: { ...host, found: hostFound, armedAtMs: hostStarted.startedMs },
                    join: { ...join, found: joinFound, armedAtMs: joinStarted.startedMs },
                });
            }
            return;
        }
        if (!intent.res.destroyed && !intent.res.writableEnded) {
            sendJson(intent.res, 200, {
                found: true, dispatchSkewMs, armWriteSkewMs,
                host: { ...host, found: true, armedAtMs: hostStarted.startedMs },
                join: { ...join, found: true, armedAtMs: joinStarted.startedMs },
            });
        }
    }).catch((error) => {
        intent.phase = 'terminal';
        faultRelay(`paired strategic End Turn completion threw: ${error.message}`);
        if (!intent.res.destroyed && !intent.res.writableEnded) {
            sendJson(intent.res, 500, {
                error: 'relay is terminal', terminalFault: state.terminalFault,
            });
        }
    });
    return true;
}

function armEndTurnPairIntent(req, res, value) {
    if (endTurnPairIntent) {
        return sendJson(res, 409, {
            error: 'one paired strategic End Turn intent is already armed',
        });
    }
    const hostSocket = clientByRole('host');
    const joinSocket = clientByRole('join');
    if (!hostSocket || !joinSocket || hostSocket === joinSocket) {
        return sendJson(res, 409, {
            error: 'paired strategic End Turn requires two exact live role sockets',
        });
    }
    if ([...pendingCommands.values()].some(
        (pending) => pending.socket === hostSocket || pending.socket === joinSocket)) {
        return sendJson(res, 409, {
            error: 'paired strategic End Turn found an earlier role command in flight',
        });
    }
    const host = { role: 'host', socket: hostSocket, ...value.host };
    const join = { role: 'join', socket: joinSocket, ...value.join };
    const hostInitial = inspectEndTurnPairObservation(host, currentUiObservation('host'));
    const joinInitial = inspectEndTurnPairObservation(join, currentUiObservation('join'));
    if (hostInitial.state === 'invalid' || joinInitial.state === 'invalid') {
        return sendJson(res, 409, {
            error: hostInitial.state === 'invalid' ? hostInitial.reason : joinInitial.reason,
        });
    }

    const intent = {
        host, join, waitMs: value.waitMs, commandTimeoutMs: value.commandTimeoutMs,
        req, res, phase: 'armed', timer: null, onAbort: null,
    };
    intent.onAbort = () => {
        if (!removeEndTurnPairIntent(intent)) return;
        intent.phase = 'terminal';
        faultRelay('paired strategic End Turn lost its HTTP owner while armed');
    };
    intent.timer = setTimeout(() => {
        failArmedEndTurnPair(intent, `timed out after ${intent.waitMs}ms`);
    }, intent.waitMs);
    endTurnPairIntent = intent;
    req.once('aborted', intent.onAbort);
    res.once('close', intent.onAbort);
    console.log(`[end-turn-pair] armed host=${host.appearance}/${host.instance}`
        + ` ui>=${host.uiSeq} join=${join.appearance}/${join.instance} ui>=${join.uiSeq}`);
    tryIssueEndTurnPairIntent();
}

function appendEvidence(kind, item, socket) {
    const sequenceKey = kind === 'ui' ? 'uiSequence' : 'worldSequence';
    const history = kind === 'ui' ? state.uiHistory : state.worldHistory;
    if (history.length >= MAX_EVIDENCE) {
        faultRelay(`${kind} evidence capacity ${MAX_EVIDENCE} exhausted`, socket);
        return null;
    }
    if (state[sequenceKey] >= 0xffffffff) {
        faultRelay(`${kind} evidence sequence wrapped`, socket);
        return null;
    }
    const entry = { seq: ++state[sequenceKey], t: nowIso(), ...item };
    history.push(entry);
    return entry;
}

function appendTurnEvidence(item, socket) {
    if (state.turnHistory.length >= MAX_EVIDENCE) {
        faultRelay(`turn evidence capacity ${MAX_EVIDENCE} exhausted`, socket);
        return null;
    }
    if (state.turnSequence >= 0xffffffff) {
        faultRelay('turn evidence sequence wrapped', socket);
        return null;
    }
    const entry = { seq: ++state.turnSequence, t: nowIso(), ...item };
    state.turnHistory.push(entry);
    notifyTurnWaiters();
    return entry;
}

function exactWorldRoleObservation(filter, role) {
    const current = state.byRole[role];
    const after = role === 'host' ? filter.hostAfter : filter.joinAfter;
    if (!current || current.connected !== true || !Number.isInteger(current.pid)
        || current.pid < 1 || typeof current.modulePath !== 'string'
        || current.modulePath.length === 0 || !Number.isInteger(current.worldSeq)
        || current.worldSeq <= after || !Array.isArray(current.stacks)) return null;
    const stacks = current.stacks.filter((stack) => stack
        && stack.id === filter.id);
    if (stacks.length !== 1 || stacks[0].x !== filter.x || stacks[0].y !== filter.y
        || stacks[0].movement !== filter.movement) return null;
    return {
        role, connected: true, pid: current.pid, modulePath: current.modulePath,
        worldSeq: current.worldSeq, day: current.day,
        activePlayerId: current.activePlayerId,
        stack: {
            id: stacks[0].id, x: stacks[0].x, y: stacks[0].y,
            movement: stacks[0].movement,
        },
    };
}

function exactWorldPairObservation(filter) {
    const join = exactWorldRoleObservation(filter, 'join');
    const host = exactWorldRoleObservation(filter, 'host');
    if (!join || !host) return null;
    return {
        id: filter.id, x: filter.x, y: filter.y, movement: filter.movement,
        host, join,
    };
}

function worldPairWaitResponse(observation = null, timedOut = false) {
    return {
        latestSeq: state.worldSequence,
        observation,
        terminalFault: state.terminalFault,
        timedOut,
    };
}

function removeWorldPairWaiter(waiter) {
    if (!worldPairWaiters.delete(waiter)) return false;
    clearTimeout(waiter.timer);
    waiter.req.off('aborted', waiter.onAbort);
    waiter.res.off('close', waiter.onAbort);
    return true;
}

function finishWorldPairWaiter(waiter, observation = null, timedOut = false) {
    if (!removeWorldPairWaiter(waiter)) return;
    if (!waiter.res.destroyed && !waiter.res.writableEnded)
        sendJson(waiter.res, 200, worldPairWaitResponse(observation, timedOut));
}

function notifyWorldPairWaiters() {
    for (const waiter of [...worldPairWaiters]) {
        if (state.terminalFault) {
            finishWorldPairWaiter(waiter);
            continue;
        }
        const exact = exactWorldPairObservation(waiter.filter);
        if (exact) finishWorldPairWaiter(waiter, exact, false);
    }
}

function waitForExactWorldPair(req, res, filter) {
    if (worldPairWaiters.size >= MAX_WORLD_PAIR_WAITERS) {
        return sendJson(res, 503, {
            error: `world-pair waiter capacity ${MAX_WORLD_PAIR_WAITERS} exhausted`,
        });
    }
    const waiter = { req, res, filter, timer: null, onAbort: null };
    waiter.onAbort = () => { removeWorldPairWaiter(waiter); };
    waiter.timer = setTimeout(
        () => finishWorldPairWaiter(waiter, null, true), filter.waitMs);
    worldPairWaiters.add(waiter);
    req.once('aborted', waiter.onAbort);
    res.once('close', waiter.onAbort);
    console.log(`[world-pair-wait] armed id=${filter.id}`
        + ` @(${filter.x},${filter.y})/MP${filter.movement}`
        + ` hostAfter=${filter.hostAfter} joinAfter=${filter.joinAfter}`);
}

function faultRelay(reason, socket = null) {
    if (!state.terminalFault) {
        state.terminalFault = { t: nowIso(), reason };
        console.error(`[terminal] ${reason}`);
    }
    notifyTurnWaiters();
    notifyUiReadyWaiters();
    notifyWorldPairWaiters();
    finishUiInvokeIntentsOnFault();
    finishEndTurnPairIntentOnFault();
    for (const [seq, pending] of pendingCommands) {
        clearTimeout(pending.timer);
        pendingCommands.delete(seq);
        settleCommandStarted(pending, null);
        pending.resolve(null);
    }
    if (socket && !socket.destroyed) socket.destroy();
}

// Wire frame: u32 length(=4+payloadLen) | u16 opcode | u16 flags(=0) | payload.
function encodeFrame(op, payload = Buffer.alloc(0)) {
    const out = Buffer.alloc(8 + payload.length);
    out.writeUInt32LE(4 + payload.length, 0);
    out.writeUInt16LE(op, 4);
    out.writeUInt16LE(0, 6);
    payload.copy(out, 8);
    return out;
}

function send(socket, op, payload) {
    if (state.terminalFault || !socket || socket.destroyed || !socket.writable) return false;
    try {
        socket.write(encodeFrame(op, payload));
        return true;
    } catch (error) {
        faultRelay(`socket write failed: ${error.message}`, socket);
        return false;
    }
}

function u32(value) {
    const out = Buffer.alloc(4);
    out.writeUInt32LE(value >>> 0, 0);
    return out;
}

function i32(value) {
    const out = Buffer.alloc(4);
    out.writeInt32LE(value, 0);
    return out;
}

function encodeStr(value) {
    const bytes = Buffer.from(value, 'utf8');
    if (bytes.length > 0xffff) throw new Error('wire string exceeds uint16 length');
    const out = Buffer.alloc(2 + bytes.length);
    out.writeUInt16LE(bytes.length, 0);
    bytes.copy(out, 2);
    return out;
}

function commandTimedOut(seq) {
    const pending = pendingCommands.get(seq);
    if (!pending) return;
    pendingCommands.delete(seq);
    settleCommandStarted(pending, null);
    pending.resolve(null);
    const commandOp = `0x${pending.commandOp.toString(16).padStart(4, '0')}`;
    faultRelay(`command seq=${seq} timed out without a result`
        + ` (role=${roleOf(pending.socket)} op=${commandOp}`
        + ` started=${pending.startedAtMs !== null} timeoutMs=${pending.timeoutMs})`,
        pending.socket);
}

function awaitResult(seq, socket, timeoutMs, expectedResultOp, commandOp) {
    let resolveResult;
    let resolveStarted;
    const result = new Promise((resolve) => { resolveResult = resolve; });
    const started = new Promise((resolve) => { resolveStarted = resolve; });
    const pending = {
        socket,
        resolve: resolveResult,
        resolveStarted,
        startedSettled: false,
        startedAtMs: null,
        timer: null,
        timeoutMs,
        expectedResultOp,
        commandOp,
    };
    if (commandOp !== Op.MoveStack && commandOp !== Op.InvokePairedEndTurn)
        settleCommandStarted(pending, null);
    pending.timer = setTimeout(() => commandTimedOut(seq), timeoutMs);
    pendingCommands.set(seq, pending);
    return { seq, result, started };
}

function issueCommand(socket, op, body, timeoutMs = DEFAULT_COMMAND_TIMEOUT_MS,
    expectedResultOp = Op.CommandResult) {
    const failed = () => ({
        seq: 0, result: Promise.resolve(null), started: Promise.resolve(null),
    });
    if (state.terminalFault) return failed();
    if ([...pendingCommands.values()].some((pending) => pending.socket === socket)) {
        faultRelay(`agent ${roleOf(socket)} already owns one pending command`, socket);
        return failed();
    }
    if (pendingCommands.size >= MAX_PENDING_COMMANDS) {
        faultRelay(`pending command capacity ${MAX_PENDING_COMMANDS} exhausted`, socket);
        return failed();
    }
    commandSequence = (commandSequence + 1) >>> 0;
    if (commandSequence === 0) {
        faultRelay('command sequence wrapped to zero', socket);
        return failed();
    }
    const command = awaitResult(commandSequence, socket, timeoutMs, expectedResultOp, op);
    if (!send(socket, op, Buffer.concat([u32(commandSequence), body])))
        faultRelay(`command seq=${commandSequence} could not be written`, socket);
    return command;
}

function sendCommand(socket, op, body, timeoutMs = DEFAULT_COMMAND_TIMEOUT_MS,
    expectedResultOp = Op.CommandResult) {
    return issueCommand(socket, op, body, timeoutMs, expectedResultOp).result;
}

function decodeUtf8(bytes, field) {
    try {
        return utf8.decode(bytes);
    } catch (error) {
        throw new Error(`${field} is not valid UTF-8`);
    }
}

function parseHello(payload) {
    if (payload.length < 16) throw new Error('Hello is too short');
    const version = payload.readUInt32LE(0);
    const pid = payload.readUInt32LE(4);
    const moduleLength = payload.readUInt32LE(8);
    if (moduleLength === 0 || moduleLength > 32768 || moduleLength > payload.length - 16)
        throw new Error('Hello module path length is invalid');
    const roleOffset = 12 + moduleLength;
    if (payload.length < roleOffset + 4) throw new Error('Hello role length is missing');
    const roleLength = payload.readUInt32LE(roleOffset);
    if (roleLength === 0 || roleLength > 31 || payload.length !== roleOffset + 4 + roleLength)
        throw new Error('Hello role length or trailing bytes are invalid');
    const modulePath = decodeUtf8(payload.subarray(12, roleOffset), 'Hello module path');
    const role = decodeUtf8(payload.subarray(roleOffset + 4), 'Hello role');
    if (version !== PROTOCOL_VERSION)
        throw new Error(`Hello protocol version ${version} does not equal ${PROTOCOL_VERSION}`);
    if (pid === 0) throw new Error('Hello pid must be non-zero');
    if (!/^[A-Za-z0-9._-]+$/.test(role)) throw new Error('Hello role is invalid');
    return { version, pid, modulePath, role };
}

function isDynamicPlayerDpid(value) {
    return value > 1 && value !== 0x00ffffff && value !== 0xffffffff;
}

function parseBeginApplied(payload) {
    if (payload.length !== 28)
        throw new Error(`BeginApplied payload must be exactly 28 bytes, got ${payload.length}`);
    const value = {
        senderDpid: payload.readUInt32LE(0), receiverDpid: payload.readUInt32LE(4),
        frameLength: payload.readUInt32LE(8), dispatchResult: payload.readInt32LE(12),
        addressee: payload.readUInt32LE(16), commandSequence: payload.readUInt32LE(20),
        activeHandle: payload.readUInt32LE(24),
    };
    if (value.senderDpid !== 1) throw new Error('BeginApplied senderDpid must be 1');
    if (!isDynamicPlayerDpid(value.receiverDpid))
        throw new Error('BeginApplied receiverDpid must be one dynamic local player');
    if (value.frameLength !== 56)
        throw new Error('BeginApplied frameLength must be the exact Russobit size 56');
    if (value.dispatchResult <= 0)
        throw new Error('BeginApplied dispatchResult must prove an applied native dispatch');
    if (value.addressee !== 0 || value.commandSequence === 0xffffffff
        || value.activeHandle === 0) {
        throw new Error('BeginApplied must carry the exact natural broadcast layout');
    }
    return value;
}

function parseStartupBeginObserved(payload) {
    if (payload.length !== 24)
        throw new Error(`StartupBeginObserved payload must be exactly 24 bytes, got ${payload.length}`);
    const value = {
        senderDpid: payload.readUInt32LE(0), receiverDpid: payload.readUInt32LE(4),
        frameLength: payload.readUInt32LE(8), addressee: payload.readUInt32LE(12),
        commandSequence: payload.readUInt32LE(16), activeHandle: payload.readUInt32LE(20),
    };
    if (value.senderDpid !== 1) throw new Error('StartupBeginObserved senderDpid must be 1');
    if (!isDynamicPlayerDpid(value.receiverDpid))
        throw new Error('StartupBeginObserved receiverDpid must be one dynamic local player');
    if (value.frameLength !== 56)
        throw new Error('StartupBeginObserved frameLength must be the exact Russobit size 56');
    if (value.addressee !== 0 || value.commandSequence !== 1 || value.activeHandle === 0)
        throw new Error('StartupBeginObserved must carry the exact day-1 layout');
    return value;
}

function parseStartupDirectedBeginObserved(payload) {
    if (payload.length !== 24)
        throw new Error(`StartupDirectedBeginObserved payload must be exactly 24 bytes, got ${payload.length}`);
    const value = {
        senderDpid: payload.readUInt32LE(0), receiverDpid: payload.readUInt32LE(4),
        frameLength: payload.readUInt32LE(8), addressee: payload.readUInt32LE(12),
        commandSequence: payload.readUInt32LE(16), activeHandle: payload.readUInt32LE(20),
    };
    if (value.senderDpid !== 1)
        throw new Error('StartupDirectedBeginObserved senderDpid must be 1');
    if (!isDynamicPlayerDpid(value.receiverDpid))
        throw new Error('StartupDirectedBeginObserved receiverDpid must be one dynamic local player');
    if (value.frameLength !== 56)
        throw new Error('StartupDirectedBeginObserved frameLength must be the exact Russobit size 56');
    if (value.addressee === 0 || value.commandSequence !== 0xffffffff
        || value.activeHandle === 0 || value.addressee === value.activeHandle) {
        throw new Error('StartupDirectedBeginObserved must carry the exact join-player layout');
    }
    return value;
}

function parseStartupJoinObserved(payload) {
    if (payload.length !== 24)
        throw new Error(`StartupJoinObserved payload must be exactly 24 bytes, got ${payload.length}`);
    const value = {
        senderDpid: payload.readUInt32LE(0), receiverDpid: payload.readUInt32LE(4),
        frameLength: payload.readUInt32LE(8), joinedHandle: payload.readUInt32LE(12),
        nameLength: payload.readUInt32LE(16), raceCategoryId: payload.readUInt32LE(20),
    };
    if (value.senderDpid !== 1) throw new Error('StartupJoinObserved senderDpid must be 1');
    if (!isDynamicPlayerDpid(value.receiverDpid))
        throw new Error('StartupJoinObserved receiverDpid must be one dynamic local player');
    if (value.joinedHandle === 0) throw new Error('StartupJoinObserved joinedHandle must be non-zero');
    if (value.nameLength === 0 || value.frameLength !== 56 + value.nameLength)
        throw new Error('StartupJoinObserved frameLength must match its Russobit player name');
    return value;
}

function parseStartupCompleteObserved(payload) {
    if (payload.length !== 24)
        throw new Error(`StartupCompleteObserved payload must be exactly 24 bytes, got ${payload.length}`);
    const value = {
        senderDpid: payload.readUInt32LE(0), receiverDpid: payload.readUInt32LE(4),
        frameLength: payload.readUInt32LE(8), joinedHandle: payload.readUInt32LE(12),
        nameLength: payload.readUInt32LE(16), raceCategoryId: payload.readUInt32LE(20),
    };
    if (value.senderDpid !== 1)
        throw new Error('StartupCompleteObserved senderDpid must be 1');
    if (!isDynamicPlayerDpid(value.receiverDpid))
        throw new Error('StartupCompleteObserved receiverDpid must be one dynamic local player');
    if (value.joinedHandle === 0)
        throw new Error('StartupCompleteObserved joinedHandle must be non-zero');
    if (value.nameLength === 0 || value.frameLength !== 56 + value.nameLength)
        throw new Error('StartupCompleteObserved frameLength must match its Russobit player name');
    return value;
}

function parseEndSendReturned(payload) {
    if (payload.length !== 12)
        throw new Error(`EndSendReturned payload must be exactly 12 bytes, got ${payload.length}`);
    const value = {
        idTo: payload.readUInt32LE(0), frameLength: payload.readUInt32LE(4),
        sendResult: payload.readInt32LE(8),
    };
    if (!isDynamicPlayerDpid(value.idTo))
        throw new Error('EndSendReturned idTo must be one dynamic remote player');
    if (value.frameLength !== 49)
        throw new Error('EndSendReturned frameLength must be the exact Russobit size 49');
    return value;
}

function parseBeginSendReturned(payload) {
    if (payload.length !== 24) {
        throw new Error(
            `BeginSendReturned payload must be exactly 24 bytes, got ${payload.length}`);
    }
    const value = {
        idTo: payload.readUInt32LE(0), frameLength: payload.readUInt32LE(4),
        sendResult: payload.readInt32LE(8), addressee: payload.readUInt32LE(12),
        commandSequence: payload.readUInt32LE(16), activeHandle: payload.readUInt32LE(20),
    };
    if (value.frameLength !== 56) {
        throw new Error(
            'BeginSendReturned frameLength must be the exact Russobit size 56');
    }
    const broadcast = value.idTo === 0 && value.addressee === 0
        && value.commandSequence !== 0 && value.commandSequence !== 0xffffffff
        && value.activeHandle !== 0;
    const directed = isDynamicPlayerDpid(value.idTo) && value.addressee !== 0
        && value.commandSequence === 0xffffffff && value.activeHandle !== 0;
    if (!broadcast && !directed) {
        throw new Error(
            'BeginSendReturned must carry one exact broadcast or directed Russobit layout');
    }
    return value;
}

function validWidget(widget) {
    return widget && typeof widget === 'object' && typeof widget.name === 'string'
        && widget.name.length > 0 && typeof widget.type === 'string' && widget.type.length > 0
        && widget.state && typeof widget.state === 'object' && !Array.isArray(widget.state);
}

// The finite paired run opts in through both native popup subscribers. A loaded
// map is independent of topmost modal, active turn and OH coordinator readiness.
// Keep the current observations, not a process-lifetime "ever saw map" latch.
function tryReleaseStartupActions() {
    if (state.terminalFault || state.startupActionsRelease) return;
    const roles = ['host', 'join'];
    const pair = roles.map((role) => ({
        role, socket: clientByRole(role), observation: state.byRole[role],
    }));
    if (pair.some(({ socket, observation }) => !socket || !socket.writable
        || socket.destroyed || observation?.connected !== true
        || observation.mapLoaded !== true || observation.startupActionsHeld !== true)) return;
    if (pair[0].socket === pair[1].socket
        || pair[0].observation.pid === pair[1].observation.pid) {
        return faultRelay('startup actions require two distinct client identities');
    }
    // Consume once before either write. A failed write faults this run; it never
    // retries or grants admission to a replacement process.
    state.startupActionsRelease = {
        t: nowIso(),
        ...Object.fromEntries(pair.map(({ role, observation }) => [role, {
            pid: observation.pid, uiSeq: observation.uiSeq,
        }])),
    };
    console.log(`[startup-actions] release ${JSON.stringify(state.startupActionsRelease)}`);
    for (const { role, socket } of pair) {
        if (!send(socket, Op.ReleaseStartupActions, Buffer.alloc(0)))
            return faultRelay(`startup actions release could not be written to ${role}`, socket);
    }
}

function handleUiSnapshot(socket, identity, payload) {
    let snapshot;
    try { snapshot = JSON.parse(decodeUtf8(payload, 'UI snapshot')); }
    catch (error) { return faultRelay(`bad UI snapshot: ${error.message}`, socket); }
    if (!snapshot || typeof snapshot !== 'object' || Array.isArray(snapshot)
        || typeof snapshot.dialog !== 'string' || snapshot.dialog.length === 0
        || !Number.isInteger(snapshot.instance) || snapshot.instance < 1
        || snapshot.instance > 0xffffffff || typeof snapshot.ready !== 'boolean'
        || typeof snapshot.strategicIdle !== 'boolean'
        || typeof snapshot.mapLoaded !== 'boolean'
        || typeof snapshot.startupActionsHeld !== 'boolean'
        || !Array.isArray(snapshot.widgets) || !snapshot.widgets.every(validWidget)
        || !Array.isArray(snapshot.targets)) {
        return faultRelay('UI snapshot shape is invalid', socket);
    }
    const targetKeys = new Set();
    for (const target of snapshot.targets) {
        if (!target || typeof target !== 'object' || typeof target.dialog !== 'string'
            || target.dialog.length === 0 || !Number.isInteger(target.instance)
            || target.instance < 1 || target.instance > 0xffffffff
            || !Array.isArray(target.widgets) || !target.widgets.every(validWidget)) {
            return faultRelay('UI action-target shape is invalid', socket);
        }
        const key = `${target.dialog}\u0000${target.instance}`;
        if (targetKeys.has(key)) return faultRelay('UI action target is duplicated', socket);
        targetKeys.add(key);
    }
    if (snapshot.targets.filter((target) => target.dialog === snapshot.dialog).length !== 1)
        return faultRelay('UI snapshot does not identify exactly one current dialog owner', socket);
    if (identity.dialogInstance !== 0 && snapshot.instance < identity.dialogInstance)
        return faultRelay(`dialog appearance regressed for role ${identity.role}`, socket);
    if (snapshot.instance === identity.dialogInstance && identity.dialog !== null
        && snapshot.dialog !== identity.dialog) {
        return faultRelay(`dialog name changed without a new appearance for role ${identity.role}`, socket);
    }
    const evidence = appendEvidence('ui', {
        role: identity.role, dialog: snapshot.dialog,
        dialogInstance: snapshot.instance, dialogAppearance: snapshot.instance,
        dialogReady: snapshot.ready, strategicIdle: snapshot.strategicIdle,
        mapLoaded: snapshot.mapLoaded, startupActionsHeld: snapshot.startupActionsHeld,
        widgets: snapshot.widgets, targets: snapshot.targets,
    }, socket);
    if (!evidence) return;
    const buttons = snapshot.widgets.filter((widget) => widget.type === 'button')
        .map((widget) => widget.name);
    Object.assign(identity, {
        dialog: snapshot.dialog, dialogInstance: snapshot.instance,
        dialogAppearance: snapshot.instance, dialogReady: snapshot.ready,
        strategicIdle: snapshot.strategicIdle, uiSeq: evidence.seq,
        mapLoaded: snapshot.mapLoaded, startupActionsHeld: snapshot.startupActionsHeld,
        widgets: snapshot.widgets, buttons, targets: snapshot.targets,
    });
    const roleState = state.byRole[identity.role];
    Object.assign(roleState, {
        dialog: snapshot.dialog, dialogInstance: snapshot.instance,
        dialogAppearance: snapshot.instance, dialogReady: snapshot.ready,
        strategicIdle: snapshot.strategicIdle, uiSeq: evidence.seq,
        mapLoaded: snapshot.mapLoaded, startupActionsHeld: snapshot.startupActionsHeld,
        widgets: snapshot.widgets, buttons, targets: snapshot.targets,
    });
    if (snapshot.dialog === 'DLG_STRATEGIC' || snapshot.dialog === 'DLG_ISO_PAL')
        roleState.reachedStrategic = true;
    if (snapshot.dialog === 'DLG_BEGIN_TURN') roleState.sawBeginTurn = true;
    console.log(`[ui] ${identity.role} -> ${snapshot.dialog} appearance=${snapshot.instance}`
        + ` ready=${snapshot.ready} strategicIdle=${snapshot.strategicIdle}`);
    const observation = {
        role: evidence.role,
        connected: roleState.connected,
        pid: roleState.pid,
        modulePath: roleState.modulePath,
        dialog: evidence.dialog,
        dialogInstance: evidence.dialogInstance,
        dialogAppearance: evidence.dialogAppearance,
        dialogReady: evidence.dialogReady,
        strategicIdle: evidence.strategicIdle,
        mapLoaded: evidence.mapLoaded,
        startupActionsHeld: evidence.startupActionsHeld,
        uiSeq: evidence.seq,
        widgets: evidence.widgets,
        targets: evidence.targets,
    };
    tryReleaseStartupActions();
    notifyUiReadyWaiters(observation);
    notifyUiInvokeIntent(observation);
    tryIssueEndTurnPairIntent();
}

function handleAgentLog(socket, identity, payload) {
    if (payload.length === 0 || payload.length > MAX_EVIDENCE)
        return faultRelay(`agent log length ${payload.length} is outside 1..${MAX_EVIDENCE}`, socket);
    let message;
    try { message = decodeUtf8(payload, 'agent log'); }
    catch (error) { return faultRelay(`bad agent log: ${error.message}`, socket); }
    // Log is a one-way native diagnostic event retained by the strict bridge
    // contract. Keep it out of relay state and HTTP APIs; JSON quoting prevents
    // an embedded newline/control byte from forging another console record.
    console.log(`[dll:${identity.role}] ${JSON.stringify(message)}`);
}

function handleWorldSnapshot(socket, identity, payload) {
    let snapshot;
    try { snapshot = JSON.parse(decodeUtf8(payload, 'world snapshot')); }
    catch (error) { return faultRelay(`bad world snapshot: ${error.message}`, socket); }
    if (!snapshot || typeof snapshot !== 'object' || Array.isArray(snapshot)
        || !Number.isInteger(snapshot.day) || snapshot.day < 0 || snapshot.day > 0xffffffff
        || typeof snapshot.activePlayerId !== 'string'
        || !/^0x[0-9a-fA-F]{8}$/.test(snapshot.activePlayerId)
        || !Array.isArray(snapshot.players) || !Array.isArray(snapshot.stacks)) {
        return faultRelay('world snapshot shape is invalid', socket);
    }
    if (!snapshot.players.every((item) => item && typeof item === 'object')
        || !snapshot.stacks.every((item) => item && typeof item === 'object')) {
        return faultRelay('world snapshot entries are invalid', socket);
    }
    if (('strategicActionReady' in snapshot && typeof snapshot.strategicActionReady !== 'boolean')
        || ('camps' in snapshot && !Array.isArray(snapshot.camps))
        || ('bags' in snapshot && !Array.isArray(snapshot.bags))) {
        return faultRelay('world snapshot generic fields are invalid', socket);
    }
    const generic = {
        strategicActionReady: snapshot.strategicActionReady === true,
        camps: snapshot.camps || [], bags: snapshot.bags || [],
    };
    const evidence = appendEvidence('world', {
        role: identity.role, day: snapshot.day, activePlayerId: snapshot.activePlayerId,
        players: snapshot.players, stacks: snapshot.stacks,
    }, socket);
    if (!evidence) return;
    Object.assign(identity, {
        worldSeq: evidence.seq, day: snapshot.day, activePlayerId: snapshot.activePlayerId,
        players: snapshot.players, stacks: snapshot.stacks,
    });
    Object.assign(state.byRole[identity.role], {
        worldSeq: evidence.seq, day: snapshot.day, activePlayerId: snapshot.activePlayerId,
        players: snapshot.players, stacks: snapshot.stacks,
    });
    Object.assign(identity, generic);
    Object.assign(state.byRole[identity.role], generic);
    notifyWorldPairWaiters();
    console.log(`[world] ${identity.role} -> day ${snapshot.day}, ${snapshot.stacks.length} stacks`);
}

function canonicalHex32(value) {
    return `0x${value.toString(16).toUpperCase().padStart(8, '0')}`;
}

function parseLegacyStacksSnapshot(payload) {
    if (payload.length < 4)
        throw new Error(`payload must contain its u32 count, got ${payload.length} bytes`);
    const count = payload.readUInt32LE(0);
    if (count > MAX_LEGACY_STACKS)
        throw new Error(`count ${count} exceeds maximum ${MAX_LEGACY_STACKS}`);
    const expectedLength = 4 + count * 20;
    if (payload.length !== expectedLength) {
        throw new Error(
            `payload for count ${count} must be exactly ${expectedLength} bytes, got ${payload.length}`);
    }
    const stacks = [];
    for (let index = 0, offset = 4; index < count; index++, offset += 20) {
        stacks.push(Object.freeze({
            id: canonicalHex32(payload.readUInt32LE(offset)),
            owner: canonicalHex32(payload.readUInt32LE(offset + 4)),
            x: payload.readInt32LE(offset + 8),
            y: payload.readInt32LE(offset + 12),
            movement: payload.readUInt32LE(offset + 16),
        }));
    }
    return Object.freeze(stacks);
}

function handleLegacyStacksSnapshot(socket, identity, payload) {
    if (identity.role !== 'host') {
        return faultRelay(
            `LegacyStacksSnapshot is host-authoritative; role ${identity.role} cannot publish it`,
            socket);
    }
    let stacks;
    try { stacks = parseLegacyStacksSnapshot(payload); }
    catch (error) {
        return faultRelay(`malformed LegacyStacksSnapshot: ${error.message}`, socket);
    }
    if (state.legacyStacksSequence >= 0xffffffff)
        return faultRelay('LegacyStacksSnapshot sequence wrapped', socket);
    const snapshot = Object.freeze({
        sourceRole: 'host', sequence: ++state.legacyStacksSequence, stacks,
    });
    state.legacyStacksSnapshot = snapshot;
    console.log(`[legacy-stacks] host -> sequence ${snapshot.sequence}, ${stacks.length} stacks`);
}

function handleMessage(socket, op, flags, payload) {
    if (state.terminalFault) return;
    const identity = state.clients.get(socket);
    if (!identity) return faultRelay('message arrived from an unknown socket', socket);
    if (flags !== 0) return faultRelay(`frame flags must be zero, got ${flags}`, socket);
    if (!identity.registered && op !== Op.Hello)
        return faultRelay(`first agent message must be Hello, got 0x${op.toString(16)}`, socket);
    if (identity.registered && op === Op.Hello)
        return faultRelay(`role ${identity.role} sent a second Hello`, socket);

    switch (op) {
    case Op.Hello: {
        let hello;
        try { hello = parseHello(payload); }
        catch (error) { return faultRelay(`malformed Hello: ${error.message}`, socket); }
        if (Object.prototype.hasOwnProperty.call(state.byRole, hello.role))
            return faultRelay(`role ${hello.role} attempted to register more than once`, socket);
        if (Object.values(state.byRole).some((peer) => peer.pid === hello.pid))
            return faultRelay(`pid ${hello.pid} attempted to register under two roles`, socket);
        Object.assign(identity, {
            registered: true, role: hello.role, pid: hello.pid, modulePath: hello.modulePath,
            dialog: null, dialogInstance: 0, dialogAppearance: 0, dialogReady: false,
            strategicIdle: false, uiSeq: 0, worldSeq: 0,
            mapLoaded: false, startupActionsHeld: false,
            widgets: [], buttons: [], targets: [],
            activePlayerId: null, players: [], stacks: [],
        });
        state.byRole[hello.role] = {
            connected: true, pid: hello.pid, modulePath: hello.modulePath,
            dialog: null, dialogInstance: 0, dialogAppearance: 0, dialogReady: false,
            strategicIdle: false, uiSeq: 0, worldSeq: 0,
            mapLoaded: false, startupActionsHeld: false,
            widgets: [], buttons: [], targets: [],
            activePlayerId: null, players: [], stacks: [], reachedStrategic: false,
            sawBeginTurn: false,
        };
        state.socketByRole[hello.role] = socket;
        const ack = Buffer.alloc(8);
        ack.writeUInt32LE(1, 0);
        ack.writeUInt32LE(PROTOCOL_VERSION, 4);
        if (!send(socket, Op.HelloAck, ack))
            return faultRelay(`HelloAck for role ${hello.role} could not be written`, socket);
        console.log(`[hello] role=${hello.role} pid=${hello.pid} v${hello.version}`);
        break;
    }
    case Op.UiSnapshot:
        handleUiSnapshot(socket, identity, payload);
        break;
    case Op.WorldSnapshot:
        handleWorldSnapshot(socket, identity, payload);
        break;
    case Op.LobbyChat: {
        let snapshot;
        try { snapshot = JSON.parse(decodeUtf8(payload, 'lobby chat')); }
        catch (error) { return faultRelay(`bad lobby chat: ${error.message}`, socket); }
        if (!snapshot || !Array.isArray(snapshot.messages)
            || !snapshot.messages.every((message) => message
                && typeof message.sender === 'string' && typeof message.text === 'string'
                && typeof message.t === 'string'))
            return faultRelay('lobby chat snapshot shape is invalid', socket);
        state.chatByRole[identity.role] = snapshot.messages;
        break;
    }
    case Op.LegacyStacksSnapshot:
        handleLegacyStacksSnapshot(socket, identity, payload);
        break;
    case Op.Log:
        handleAgentLog(socket, identity, payload);
        break;
    case Op.CommandResult: {
        if (payload.length !== 5 || payload.readUInt8(4) > 1)
            return faultRelay('CommandResult payload is invalid', socket);
        const seq = payload.readUInt32LE(0);
        const pending = pendingCommands.get(seq);
        if (!pending || pending.socket !== socket)
            return faultRelay(`CommandResult seq=${seq} has no exact pending owner`, socket);
        if (pending.expectedResultOp !== Op.CommandResult) {
            return faultRelay(`CommandResult seq=${seq} does not match its expected result opcode`,
                socket);
        }
        if ((pending.commandOp === Op.MoveStack
            || pending.commandOp === Op.InvokePairedEndTurn)
            && pending.startedAtMs === null) {
            return faultRelay(`CommandResult seq=${seq} arrived before CommandStarted`, socket);
        }
        clearTimeout(pending.timer);
        pendingCommands.delete(seq);
        pending.resolve(payload.readUInt8(4) === 1);
        break;
    }
    case Op.CommandStarted: {
        if (payload.length !== 4)
            return faultRelay('CommandStarted payload is invalid', socket);
        const seq = payload.readUInt32LE(0);
        const pending = pendingCommands.get(seq);
        if (!pending || pending.socket !== socket)
            return faultRelay(`CommandStarted seq=${seq} has no exact pending owner`, socket);
        if ((pending.commandOp !== Op.MoveStack
            && pending.commandOp !== Op.InvokePairedEndTurn)
            || pending.expectedResultOp !== Op.CommandResult) {
            return faultRelay(
                `CommandStarted seq=${seq} does not match one started command`, socket);
        }
        if (pending.startedAtMs !== null)
            return faultRelay(`CommandStarted seq=${seq} was published more than once`, socket);

        pending.startedAtMs = Date.now();
        clearTimeout(pending.timer);
        pending.timer = setTimeout(() => commandTimedOut(seq), pending.timeoutMs);
        settleCommandStarted(pending, {
            seq,
            startedMs: pending.startedAtMs,
            // One relay-owned order domain shared with both roles' UI evidence.
            // This is an observation watermark, not a source-process timestamp.
            startedUiSeq: state.uiSequence,
        });
        break;
    }
    case Op.AutoBattleKickResult: {
        if (payload.length !== 17 || payload.readUInt8(4) > 1)
            return faultRelay('AutoBattleKickResult payload is invalid', socket);
        const seq = payload.readUInt32LE(0);
        const pending = pendingCommands.get(seq);
        if (!pending || pending.socket !== socket) {
            return faultRelay(`AutoBattleKickResult seq=${seq} has no exact pending owner`,
                socket);
        }
        if (pending.expectedResultOp !== Op.AutoBattleKickResult) {
            return faultRelay(
                `AutoBattleKickResult seq=${seq} does not match its expected result opcode`, socket);
        }
        const result = {
            succeeded: payload.readUInt8(4) === 1,
            controllerGateBefore: payload.readUInt8(5),
            kickStateBefore: payload.readUInt8(6),
            kickStateAfter: payload.readUInt8(7),
            sideSelector: payload.readUInt8(8),
            flag38Before: payload.readUInt8(9),
            flag38After: payload.readUInt8(10),
            flag39Before: payload.readUInt8(11),
            flag39After: payload.readUInt8(12),
            memberFunction: payload.readUInt32LE(13),
        };
        const selectedTransition = result.sideSelector !== 0
            ? result.flag38Before === 0 && result.flag38After === 1
                && result.flag39After === result.flag39Before
            : result.flag39Before === 0 && result.flag39After === 1
                && result.flag38After === result.flag38Before;
        const proved = result.controllerGateBefore === 0
            && result.kickStateBefore === 0 && result.kickStateAfter === 1
            && selectedTransition && result.memberFunction === 0x00635509;
        if (result.succeeded !== proved) {
            return faultRelay(
                `AutoBattleKickResult seq=${seq} contradicts its engine-state proof`, socket);
        }
        clearTimeout(pending.timer);
        pendingCommands.delete(seq);
        pending.resolve(result);
        break;
    }
    case Op.BeginApplied:
    case Op.StartupBeginObserved:
    case Op.StartupJoinObserved:
    case Op.StartupDirectedBeginObserved:
    case Op.StartupCompleteObserved:
    case Op.EndSendReturned:
    case Op.BeginSendReturned: {
        const definitions = {
            [Op.BeginApplied]: ['stock-begin-turn-applied', 'BeginApplied', parseBeginApplied],
            [Op.StartupBeginObserved]: [
                'stock-startup-begin-turn-observed', 'StartupBeginObserved', parseStartupBeginObserved,
            ],
            [Op.StartupJoinObserved]: [
                'stock-startup-join-game-observed', 'StartupJoinObserved', parseStartupJoinObserved,
            ],
            [Op.StartupDirectedBeginObserved]: [
                'stock-startup-directed-begin-turn-observed',
                'StartupDirectedBeginObserved', parseStartupDirectedBeginObserved,
            ],
            [Op.StartupCompleteObserved]: [
                'stock-startup-complete-observed',
                'StartupCompleteObserved', parseStartupCompleteObserved,
            ],
            [Op.EndSendReturned]: [
                'stock-end-turn-send-returned', 'EndSendReturned', parseEndSendReturned,
            ],
            [Op.BeginSendReturned]: [
                'stock-begin-turn-send-returned', 'BeginSendReturned', parseBeginSendReturned,
            ],
        };
        const [kind, name, parser] = definitions[op];
        let value;
        try { value = parser(payload); }
        catch (error) { return faultRelay(`malformed ${name}: ${error.message}`, socket); }
        const evidence = appendTurnEvidence({ kind, role: identity.role, ...value }, socket);
        if (evidence) console.log(`[turn] #${evidence.seq} ${identity.role} ${kind}`);
        break;
    }
    case Op.Goodbye:
        if (payload.length !== 0) return faultRelay('Goodbye payload must be empty', socket);
        return faultRelay(`agent ${identity.role} ended the finite run with Goodbye`, socket);
    default:
        return faultRelay(`unhandled agent op 0x${op.toString(16)} (${payload.length}B)`, socket);
    }
}

function attachParser(socket) {
    let buffered = Buffer.alloc(0);
    socket.on('data', (chunk) => {
        if (state.terminalFault) return;
        buffered = Buffer.concat([buffered, chunk]);
        for (;;) {
            if (buffered.length < 4) break;
            const length = buffered.readUInt32LE(0);
            if (length < 4 || length > MAX_FRAME_BYTES) {
                faultRelay(`bad agent frame length ${length}`, socket);
                return;
            }
            if (buffered.length < 4 + length) break;
            const frame = buffered.subarray(4, 4 + length);
            buffered = buffered.subarray(4 + length);
            try {
                handleMessage(socket, frame.readUInt16LE(0), frame.readUInt16LE(2), frame.subarray(4));
            } catch (error) {
                faultRelay(`agent handler threw: ${error.message}`, socket);
            }
            if (state.terminalFault) return;
        }
    });
    socket.on('end', () => {
        if (!state.shuttingDown && buffered.length !== 0)
            faultRelay(`agent ended with ${buffered.length} bytes of a partial frame`, socket);
    });
}

function onAgentConnection(socket) {
    if (state.terminalFault) {
        socket.destroy();
        return;
    }
    socket.setNoDelay(true);
    state.clients.set(socket, {
        registered: false, role: '?', pid: 0, modulePath: '',
        dialog: null, dialogInstance: 0, dialogAppearance: 0, dialogReady: false,
        strategicIdle: false, uiSeq: 0, worldSeq: 0,
        widgets: [], buttons: [], targets: [],
        activePlayerId: null, players: [], stacks: [],
    });
    console.log('[conn] agent connected');
    attachParser(socket);
    socket.on('close', () => {
        const identity = state.clients.get(socket);
        state.clients.delete(socket);
        if (identity?.registered && state.socketByRole[identity.role] === socket) {
            state.byRole[identity.role].connected = false;
            delete state.socketByRole[identity.role];
        }
        console.log(`[conn] ${identity?.role || '?'} disconnected`);
        if (!state.shuttingDown && identity)
            faultRelay(`agent ${identity.registered ? `${identity.role} pid=${identity.pid}` : '<unregistered>'} disconnected`);
    });
    socket.on('error', (error) => {
        if (!state.shuttingDown) faultRelay(`agent socket error: ${error.message}`, socket);
    });
}

function listenFailure(kind, error) {
    console.error(`[${kind}] listen failed: ${error.message}`);
    state.shuttingDown = true;
    process.exit(1);
}

if (!BRIDGE_TCP_ENABLED && process.platform !== 'win32')
    throw new Error('non-Windows relay requires explicit D2TESTDRV_BRIDGE_TCP_HOST/_PORT');

if (BRIDGE_TCP_ENABLED) {
    const server = net.createServer(onAgentConnection);
    server.on('error', (error) => listenFailure('tcp', error));
    server.listen(BRIDGE_TCP_PORT, BRIDGE_TCP_HOST, () => {
        state.agentListening = true;
        console.log(`[tcp] agent server on ${BRIDGE_TCP_HOST}:${BRIDGE_TCP_PORT}`);
    });
} else {
    const server = net.createServer(onAgentConnection);
    server.on('error', (error) => listenFailure('pipe', error));
    server.listen(PIPE_NAME, () => {
        state.agentListening = true;
        console.log(`[pipe] listening on ${PIPE_NAME}`);
    });
}

function clientByRole(role) {
    if (state.terminalFault) return null;
    const socket = state.socketByRole[role];
    return socket && state.clients.has(socket) ? socket : null;
}

function sendJson(res, status, value) {
    if (res.destroyed || res.writableEnded) return;
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify(value, null, 2));
}

function validateQuery(res, query, required, optional = []) {
    const allowed = new Set([...required, ...optional]);
    for (const key of query.keys()) {
        if (!allowed.has(key)) {
            sendJson(res, 400, { error: `unexpected query parameter ${key}` });
            return false;
        }
    }
    for (const key of allowed) {
        if (query.getAll(key).length > 1) {
            sendJson(res, 400, { error: `${key} may be specified at most once` });
            return false;
        }
    }
    for (const key of required) {
        if (query.getAll(key).length !== 1) {
            sendJson(res, 400, { error: `${key} is required exactly once` });
            return false;
        }
    }
    return true;
}

function parseRole(res, text) {
    if (typeof text !== 'string' || !/^[A-Za-z0-9._-]+$/.test(text)) {
        sendJson(res, 400, { error: 'role must use the DebugTest Hello role syntax' });
        return null;
    }
    return text;
}

function parseString(res, text, name, minBytes = 1, maxBytes = 255) {
    const bytes = typeof text === 'string' ? Buffer.byteLength(text, 'utf8') : -1;
    if (bytes < minBytes || bytes > maxBytes) {
        sendJson(res, 400, { error: `${name} must contain ${minBytes}..${maxBytes} UTF-8 bytes` });
        return null;
    }
    return text;
}

function parseUint32Token(res, text, name) {
    if (typeof text !== 'string' || !/^[1-9][0-9]*$/.test(text)) {
        sendJson(res, 400, { error: `${name} must be a decimal uint32 token` });
        return null;
    }
    const value = Number(text);
    if (!Number.isSafeInteger(value) || value > 0xffffffff) {
        sendJson(res, 400, { error: `${name} must be a decimal uint32 token` });
        return null;
    }
    return value;
}

function parseInt32(res, text, name) {
    if (typeof text !== 'string' || !/^(0|-?[1-9][0-9]*)$/.test(text)) {
        sendJson(res, 400, { error: `${name} must be a canonical decimal int32` });
        return null;
    }
    const value = Number(text);
    if (!Number.isSafeInteger(value) || value < -0x80000000 || value > 0x7fffffff) {
        sendJson(res, 400, { error: `${name} must be a canonical decimal int32` });
        return null;
    }
    return value;
}

function parseUint32Watermark(res, text, name) {
    if (typeof text !== 'string' || !/^(0|[1-9][0-9]*)$/.test(text)) {
        sendJson(res, 400, { error: `${name} must be a decimal uint32 watermark` });
        return null;
    }
    const value = Number(text);
    if (!Number.isSafeInteger(value) || value > 0xffffffff) {
        sendJson(res, 400, { error: `${name} must be a decimal uint32 watermark` });
        return null;
    }
    return value;
}

function parseCommandTimeout(res, query) {
    const values = query.getAll('timeoutMs');
    if (values.length === 0) return DEFAULT_COMMAND_TIMEOUT_MS;
    const text = values[0];
    if (!/^[1-9][0-9]*$/.test(text)) {
        sendJson(res, 400, {
            error: `timeoutMs must be one decimal integer from ${MIN_COMMAND_TIMEOUT_MS} to ${MAX_COMMAND_TIMEOUT_MS}`,
        });
        return null;
    }
    const value = Number(text);
    if (!Number.isSafeInteger(value) || value < MIN_COMMAND_TIMEOUT_MS
        || value > MAX_COMMAND_TIMEOUT_MS) {
        sendJson(res, 400, {
            error: `timeoutMs must be one decimal integer from ${MIN_COMMAND_TIMEOUT_MS} to ${MAX_COMMAND_TIMEOUT_MS}`,
        });
        return null;
    }
    return value;
}

function parseEvidenceFilter(res, query, allowWait = false) {
    if (!validateQuery(res, query, [], allowWait ? ['after', 'role', 'waitMs'] : ['after', 'role']))
        return null;
    const afterText = query.get('after') || '0';
    if (!/^(0|[1-9][0-9]*)$/.test(afterText)) {
        sendJson(res, 400, { error: 'after must be a decimal uint32 watermark' });
        return null;
    }
    const after = Number(afterText);
    if (!Number.isSafeInteger(after) || after > 0xffffffff) {
        sendJson(res, 400, { error: 'after must be a decimal uint32 watermark' });
        return null;
    }
    const roleText = query.get('role');
    let role = null;
    if (roleText !== null) {
        role = parseRole(res, roleText);
        if (role === null) return null;
    }
    return { after, role };
}

function parseTurnWait(res, query) {
    const text = query.get('waitMs');
    if (text === null) return 0;
    if (!/^[1-9][0-9]*$/.test(text)) {
        sendJson(res, 400, {
            error: `waitMs must be one decimal integer from 1 to ${MAX_TURN_WAIT_MS}`,
        });
        return null;
    }
    const value = Number(text);
    if (!Number.isSafeInteger(value) || value > MAX_TURN_WAIT_MS) {
        sendJson(res, 400, {
            error: `waitMs must be one decimal integer from 1 to ${MAX_TURN_WAIT_MS}`,
        });
        return null;
    }
    return value;
}

function parseUiReadyWait(res, query) {
    if (!validateQuery(res, query, ['role', 'dlg', 'btn', 'after', 'waitMs'])) return null;
    const role = parseRole(res, query.get('role'));
    const dialog = parseString(res, query.get('dlg'), 'dlg');
    const button = parseString(res, query.get('btn'), 'btn');
    const afterText = query.get('after');
    if (!/^(0|[1-9][0-9]*)$/.test(afterText)) {
        sendJson(res, 400, { error: 'after must be a decimal uint32 watermark' });
        return null;
    }
    const after = Number(afterText);
    if (!Number.isSafeInteger(after) || after > 0xffffffff) {
        sendJson(res, 400, { error: 'after must be a decimal uint32 watermark' });
        return null;
    }
    const waitText = query.get('waitMs');
    if (!/^[1-9][0-9]*$/.test(waitText)) {
        sendJson(res, 400, {
            error: `waitMs must be one decimal integer from 1 to ${MAX_UI_READY_WAIT_MS}`,
        });
        return null;
    }
    const waitMs = Number(waitText);
    if (!Number.isSafeInteger(waitMs) || waitMs > MAX_UI_READY_WAIT_MS) {
        sendJson(res, 400, {
            error: `waitMs must be one decimal integer from 1 to ${MAX_UI_READY_WAIT_MS}`,
        });
        return null;
    }
    if (role === null || dialog === null || button === null) return null;
    return { role, dialog, button, after, waitMs };
}

function parseExactWorldPairWait(res, query) {
    if (!validateQuery(res, query,
        ['hostAfter', 'joinAfter', 'id', 'x', 'y', 'mp', 'waitMs'])) return null;
    const hostAfter = parseUint32Watermark(
        res, query.get('hostAfter'), 'hostAfter');
    if (hostAfter === null) return null;
    const joinAfter = parseUint32Watermark(
        res, query.get('joinAfter'), 'joinAfter');
    if (joinAfter === null) return null;
    const id = query.get('id');
    if (typeof id !== 'string' || !/^0x[0-9A-F]{8}$/.test(id)) {
        sendJson(res, 400, {
            error: 'id must be one canonical uppercase 0x-prefixed 32-bit handle',
        });
        return null;
    }
    const x = parseInt32(res, query.get('x'), 'x');
    if (x === null) return null;
    const y = parseInt32(res, query.get('y'), 'y');
    if (y === null) return null;
    const movement = parseInt32(res, query.get('mp'), 'mp');
    if (movement === null) return null;
    if (movement < 0 || movement > 255) {
        sendJson(res, 400, { error: 'mp must be within 0..255' });
        return null;
    }
    const waitText = query.get('waitMs');
    if (!/^[1-9][0-9]*$/.test(waitText)) {
        sendJson(res, 400, {
            error: `waitMs must be one decimal integer from 1 to ${MAX_WORLD_PAIR_WAIT_MS}`,
        });
        return null;
    }
    const waitMs = Number(waitText);
    if (!Number.isSafeInteger(waitMs) || waitMs > MAX_WORLD_PAIR_WAIT_MS) {
        sendJson(res, 400, {
            error: `waitMs must be one decimal integer from 1 to ${MAX_WORLD_PAIR_WAIT_MS}`,
        });
        return null;
    }
    return { hostAfter, joinAfter, id, x, y, movement, waitMs };
}

function parseUiInvokeIntent(res, query) {
    if (!validateQuery(res, query, ['role', 'dlg', 'btn', 'after', 'waitMs'],
        ['timeoutMs', 'stableMs']))
        return null;
    const role = parseRole(res, query.get('role'));
    const dialog = parseString(res, query.get('dlg'), 'dlg');
    const button = parseString(res, query.get('btn'), 'btn');
    const afterText = query.get('after');
    if (!/^(0|[1-9][0-9]*)$/.test(afterText)) {
        sendJson(res, 400, { error: 'after must be a decimal uint32 watermark' });
        return null;
    }
    const after = Number(afterText);
    if (!Number.isSafeInteger(after) || after > 0xffffffff) {
        sendJson(res, 400, { error: 'after must be a decimal uint32 watermark' });
        return null;
    }
    const waitMs = parseTurnWait(res, query);
    const commandTimeoutMs = parseCommandTimeout(res, query);
    const stableText = query.get('stableMs');
    let stableMs = 0;
    if (stableText !== null) {
        if (!/^[1-9][0-9]*$/.test(stableText)) {
            sendJson(res, 400, {
                error: `stableMs must be one decimal integer from 1 to ${MAX_UI_INVOKE_STABLE_MS}`,
            });
            return null;
        }
        stableMs = Number(stableText);
        if (!Number.isSafeInteger(stableMs)
            || stableMs > MAX_UI_INVOKE_STABLE_MS) {
            sendJson(res, 400, {
                error: `stableMs must be one decimal integer from 1 to ${MAX_UI_INVOKE_STABLE_MS}`,
            });
            return null;
        }
    }
    if (role === null || dialog === null || button === null
        || waitMs === null || commandTimeoutMs === null) return null;
    if (stableMs >= waitMs) {
        sendJson(res, 400, { error: 'stableMs must be shorter than waitMs' });
        return null;
    }
    const socket = clientByRole(role);
    if (!socket) {
        sendJson(res, 503, { error: `no client for role ${role}` });
        return null;
    }
    return {
        role, dialog, button, after, waitMs, stableMs, commandTimeoutMs, socket,
    };
}

function parseEndTurnPairIntent(res, query) {
    const required = [
        'hostappearance', 'hostinstance', 'hostui',
        'joinappearance', 'joininstance', 'joinui',
        'waitMs', 'timeoutMs',
    ];
    if (!validateQuery(res, query, required)) return null;
    const hostAppearance = parseUint32Token(
        res, query.get('hostappearance'), 'hostappearance');
    if (hostAppearance === null) return null;
    const hostInstance = parseUint32Token(
        res, query.get('hostinstance'), 'hostinstance');
    if (hostInstance === null) return null;
    const hostUi = parseUint32Token(res, query.get('hostui'), 'hostui');
    if (hostUi === null) return null;
    const joinAppearance = parseUint32Token(
        res, query.get('joinappearance'), 'joinappearance');
    if (joinAppearance === null) return null;
    const joinInstance = parseUint32Token(
        res, query.get('joininstance'), 'joininstance');
    if (joinInstance === null) return null;
    const joinUi = parseUint32Token(res, query.get('joinui'), 'joinui');
    if (joinUi === null) return null;
    const waitMs = parseTurnWait(res, query);
    if (waitMs === null) return null;
    const commandTimeoutMs = parseCommandTimeout(res, query);
    if (commandTimeoutMs === null) return null;
    return {
        host: { appearance: hostAppearance, instance: hostInstance, uiSeq: hostUi },
        join: { appearance: joinAppearance, instance: joinInstance, uiSeq: joinUi },
        waitMs, commandTimeoutMs,
    };
}

function readyDialogClient(res, role, dialog, appearance, owner, control, type) {
    const socket = clientByRole(role);
    if (!socket) {
        sendJson(res, 503, { error: `no client for role ${role}` });
        return null;
    }
    const current = state.byRole[role];
    if (!current || current.dialogReady !== true || !Array.isArray(current.targets)
        || current.dialogInstance !== current.dialogAppearance) {
        sendJson(res, 409, { error: `role ${role} has no ready native dialog owner` });
        return null;
    }
    if (current.dialogAppearance !== appearance) {
        sendJson(res, 409, {
            error: `dialog ${dialog} appearance ${appearance} is no longer current`,
            currentAppearance: current.dialogAppearance,
        });
        return null;
    }
    const targets = current.targets.filter((target) => target.dialog === dialog
        && target.instance === owner);
    if (targets.length !== 1) {
        sendJson(res, 409, {
            error: `dialog ${dialog} owner ${owner} is not the exact ready action target`,
        });
        return null;
    }
    const controls = targets[0].widgets.filter((widget) => widget.name === control
        && widget.type === type);
    if (controls.length !== 1) {
        sendJson(res, 409, {
            error: `${type} ${dialog}::${control} is not unique on owner ${owner}`,
        });
        return null;
    }
    if (type === 'button' && controls[0].state.enabled !== true) {
        sendJson(res, 409, {
            error: `button ${dialog}::${control} is not explicitly enabled on owner ${owner}`,
        });
        return null;
    }
    return { socket, controlState: controls[0].state };
}

function sendCommandJson(res, value) {
    if (state.terminalFault) {
        return sendJson(res, 500, {
            error: 'relay is terminal', terminalFault: state.terminalFault,
        });
    }
    return sendJson(res, 200, value);
}

function parseUiTarget(res, query, controlParam, controlType, extraRequired = [], optional = []) {
    const required = ['role', 'dlg', controlParam, 'appearance', 'instance', ...extraRequired];
    if (!validateQuery(res, query, required, optional)) return null;
    const role = parseRole(res, query.get('role'));
    const dialog = parseString(res, query.get('dlg'), 'dlg');
    const control = parseString(res, query.get(controlParam), controlParam);
    const appearance = parseUint32Token(res, query.get('appearance'), 'appearance');
    const owner = parseUint32Token(res, query.get('instance'), 'instance');
    if (role === null || dialog === null || control === null
        || appearance === null || owner === null) return null;
    const target = readyDialogClient(res, role, dialog, appearance, owner, control, controlType);
    return target ? { ...target, role, dialog, control, appearance, owner } : null;
}

function parseMapActionTarget(res, query) {
    if (!validateQuery(res, query,
        ['role', 'id', 'fromx', 'fromy', 'frommp', 'x', 'y',
            'appearance', 'instance'], ['timeoutMs'])) return null;
    const role = parseRole(res, query.get('role'));
    const id = query.get('id');
    const fromX = parseInt32(res, query.get('fromx'), 'fromx');
    const fromY = parseInt32(res, query.get('fromy'), 'fromy');
    const fromMovement = parseInt32(res, query.get('frommp'), 'frommp');
    const x = parseInt32(res, query.get('x'), 'x');
    const y = parseInt32(res, query.get('y'), 'y');
    const appearance = parseUint32Token(res, query.get('appearance'), 'appearance');
    const owner = parseUint32Token(res, query.get('instance'), 'instance');
    const commandTimeoutMs = parseCommandTimeout(res, query);
    if (role === null || fromX === null || fromY === null || fromMovement === null
        || x === null || y === null
        || appearance === null || owner === null || commandTimeoutMs === null) return null;
    if (fromMovement < -1 || fromMovement > 255) {
        sendJson(res, 400, {
            error: 'frommp must be -1 (legacy/attack) or within 0..255',
        });
        return null;
    }
    if (typeof id !== 'string' || !/^0x[0-9A-F]{8}$/.test(id)) {
        sendJson(res, 400, {
            error: 'id must be one canonical uppercase 0x-prefixed 32-bit handle',
        });
        return null;
    }
    const socket = clientByRole(role);
    if (!socket) {
        sendJson(res, 503, { error: `no client for role ${role}` });
        return null;
    }
    const current = state.byRole[role];
    const bareMap = current?.dialog === 'DLG_STRATEGIC' || current?.dialog === 'DLG_ISO_PAL';
    if (!current || current.dialogReady !== true || !bareMap
        || current.dialogInstance !== current.dialogAppearance
        || current.dialogAppearance !== appearance || !Array.isArray(current.targets)) {
        sendJson(res, 409, {
            error: `role ${role} is not on the requested ready strategic-map appearance`,
            currentAppearance: current?.dialogAppearance ?? 0,
        });
        return null;
    }
    const roots = current.targets.filter((target) => target.dialog === current.dialog
        && target.instance === owner);
    if (roots.length !== 1) {
        sendJson(res, 409, {
            error: `strategic-map root owner ${owner} is not the exact ready action target`,
        });
        return null;
    }
    return { socket, role, id, fromX, fromY, fromMovement, x, y,
        appearance, owner, commandTimeoutMs };
}

function parseGenericMapTarget(res, query, fields, bareMap) {
    if (!validateQuery(res, query, ['role', 'appearance', 'instance', ...fields], ['timeoutMs']))
        return null;
    const role = parseRole(res, query.get('role'));
    const appearance = parseUint32Token(res, query.get('appearance'), 'appearance');
    const owner = parseUint32Token(res, query.get('instance'), 'instance');
    const commandTimeoutMs = parseCommandTimeout(res, query);
    if (role === null || appearance === null || owner === null || commandTimeoutMs === null)
        return null;
    const socket = clientByRole(role);
    if (!socket) { sendJson(res, 503, { error: `no client for role ${role}` }); return null; }
    const current = state.byRole[role];
    if (!current || (bareMap && !['DLG_STRATEGIC', 'DLG_ISO_PAL'].includes(current.dialog))
        || current.dialogReady !== true || current.dialogInstance !== current.dialogAppearance
        || current.dialogAppearance !== appearance || !Array.isArray(current.targets)
        || current.targets.filter((target) => target.dialog === current.dialog
            && target.instance === owner).length !== 1) {
        sendJson(res, 409, { error: 'map action requires the exact ready strategic-map owner' });
        return null;
    }
    return { socket, role, appearance, owner, commandTimeoutMs };
}

function parseGameId(res, query, name) {
    const value = query.get(name);
    if (typeof value !== 'string'
        || !/^(?:0x[0-9A-Fa-f]{8}|[A-Za-z][A-Za-z0-9]{9})$/.test(value)) {
        sendJson(res, 400, { error: `${name} must be a game ID or 0x-prefixed handle` });
        return null;
    }
    return value;
}

function parsePairedMapActionTarget(res, query, prefix, role, commandTimeoutMs) {
    const projected = new URLSearchParams();
    projected.set('role', role);
    for (const field of [
        'id', 'fromx', 'fromy', 'frommp', 'x', 'y', 'appearance', 'instance',
    ])
        projected.set(field, query.get(`${prefix}${field}`));
    const target = parseMapActionTarget(res, projected);
    return target ? { ...target, commandTimeoutMs } : null;
}

function encodeMoveStackCommand(target) {
    return Buffer.concat([
        u32(target.appearance), u32(target.owner), encodeStr(target.id),
        i32(target.fromX), i32(target.fromY), i32(target.fromMovement),
        i32(target.x), i32(target.y),
    ]);
}

async function handleHttp(req, res) {
    const url = new URL(req.url, `http://${HTTP_HOST}:${HTTP_PORT}`);
    const path = url.pathname;
    const query = url.searchParams;

    if (req.method === 'POST' && state.terminalFault) {
        return sendJson(res, 500, {
            error: 'relay is terminal', terminalFault: state.terminalFault,
        });
    }
    if (req.method === 'GET' && path === '/api/status') {
        if (!validateQuery(res, query, [])) return;
        return sendJson(res, 200, {
            instanceId: INSTANCE_ID, agentListening: state.agentListening,
            terminalFault: state.terminalFault, roles: state.byRole,
            startupActionsRelease: state.startupActionsRelease,
        });
    }
    if (req.method === 'GET' && path === '/api/state') {
        if (!validateQuery(res, query, [])) return;
        return sendJson(res, 200, { terminalFault: state.terminalFault, roles: state.byRole });
    }
    if (req.method === 'GET' && path === '/api/ui') {
        if (!validateQuery(res, query, [], ['role'])) return;
        const roleText = query.get('role');
        if (roleText !== null) {
            const role = parseRole(res, roleText);
            if (role === null) return;
            const value = state.byRole[role];
            return sendJson(res, 200, {
                role, dialog: value?.dialog ?? null,
                dialogInstance: value?.dialogInstance ?? 0,
                dialogAppearance: value?.dialogAppearance ?? 0,
                dialogReady: value?.dialogReady ?? false,
                strategicIdle: value?.strategicIdle ?? false,
                mapLoaded: value?.mapLoaded ?? false,
                startupActionsHeld: value?.startupActionsHeld ?? false,
                uiSeq: value?.uiSeq ?? 0, widgets: value?.widgets || [],
                worldSeq: value?.worldSeq ?? 0,
                targets: value?.targets || [],
            });
        }
        const roles = Object.create(null);
        for (const [role, value] of Object.entries(state.byRole)) {
            roles[role] = {
                dialog: value.dialog, dialogInstance: value.dialogInstance,
                dialogAppearance: value.dialogAppearance, dialogReady: value.dialogReady,
                strategicIdle: value.strategicIdle, uiSeq: value.uiSeq,
                mapLoaded: value.mapLoaded, startupActionsHeld: value.startupActionsHeld,
                worldSeq: value.worldSeq ?? 0,
                widgets: value.widgets, targets: value.targets,
            };
        }
        return sendJson(res, 200, { roles });
    }
    if (req.method === 'GET' && path === '/api/world') {
        if (!validateQuery(res, query, [], ['role'])) return;
        const roleText = query.get('role');
        if (roleText !== null) {
            const role = parseRole(res, roleText);
            if (role === null) return;
            const value = state.byRole[role];
            return sendJson(res, 200, {
                role, worldSeq: value?.worldSeq ?? 0, day: value?.day ?? null,
                activePlayerId: value?.activePlayerId ?? null,
                players: value?.players || [], stacks: value?.stacks || [],
                strategicActionReady: value?.strategicActionReady === true,
                camps: value?.camps || [], bags: value?.bags || [],
            });
        }
        const roles = Object.create(null);
        for (const [role, value] of Object.entries(state.byRole)) {
            roles[role] = {
                worldSeq: value.worldSeq, day: value.day,
                activePlayerId: value.activePlayerId,
                players: value.players, stacks: value.stacks,
                strategicActionReady: value.strategicActionReady === true,
                camps: value.camps || [], bags: value.bags || [],
            };
        }
        return sendJson(res, 200, { roles });
    }
    if (req.method === 'GET' && path === '/api/lobby/chat') {
        if (!validateQuery(res, query, [], ['role'])) return;
        if (query.has('role')) {
            const role = parseRole(res, query.get('role'));
            if (role === null) return;
            return sendJson(res, 200, { role, messages: state.chatByRole[role] || [] });
        }
        const roles = Object.create(null);
        for (const role of Object.keys(state.byRole))
            roles[role] = { messages: state.chatByRole[role] || [] };
        return sendJson(res, 200, { roles });
    }
    if (req.method === 'GET' && path === '/api/legacy-stacks') {
        if (!validateQuery(res, query, [])) return;
        return sendJson(res, 200, state.legacyStacksSnapshot);
    }
    if (req.method === 'GET' && path === '/api/ui/history') {
        const filter = parseEvidenceFilter(res, query);
        if (!filter) return;
        return sendJson(res, 200, {
            latestSeq: state.uiSequence, events: evidenceForFilter(state.uiHistory, filter),
        });
    }
    if (req.method === 'GET' && path === '/api/ui/wait-ready') {
        const filter = parseUiReadyWait(res, query);
        if (!filter) return;
        const exact = exactReadyUiObservation(
            filter, currentUiObservation(filter.role));
        if (exact || state.terminalFault)
            return sendJson(res, 200, uiReadyWaitResponse(exact, false));
        return waitForUiReady(req, res, filter);
    }
    if (req.method === 'GET' && path === '/api/world/history') {
        const filter = parseEvidenceFilter(res, query);
        if (!filter) return;
        return sendJson(res, 200, {
            latestSeq: state.worldSequence,
            events: evidenceForFilter(state.worldHistory, filter),
        });
    }
    if (req.method === 'GET' && path === '/api/world/wait-exact-pair') {
        const filter = parseExactWorldPairWait(res, query);
        if (!filter) return;
        const exact = exactWorldPairObservation(filter);
        if (exact || state.terminalFault)
            return sendJson(res, 200, worldPairWaitResponse(exact, false));
        return waitForExactWorldPair(req, res, filter);
    }
    if (req.method === 'GET' && path === '/api/turn/history') {
        const filter = parseEvidenceFilter(res, query, true);
        if (!filter) return;
        const waitMs = parseTurnWait(res, query);
        if (waitMs === null) return;
        const response = turnHistoryResponse(filter, false);
        if (waitMs === 0 || response.events.length !== 0 || response.terminalFault)
            return sendJson(res, 200, response);
        return waitForTurnEvidence(req, res, filter, waitMs);
    }

    if (req.method === 'POST'
        && path === '/api/ui/end-turn-pair-when-strategic-idle') {
        const intent = parseEndTurnPairIntent(res, query);
        if (!intent) return;
        return armEndTurnPairIntent(req, res, intent);
    }
    if (req.method === 'POST' && path === '/api/ui/invoke') {
        const target = parseUiTarget(res, query, 'btn', 'button', [], ['timeoutMs']);
        if (!target) return;
        const timeoutMs = parseCommandTimeout(res, query);
        if (timeoutMs === null) return;
        const found = await sendCommand(target.socket, Op.InvokeButton, Buffer.concat([
            u32(target.appearance), u32(target.owner),
            encodeStr(target.dialog), encodeStr(target.control),
        ]), timeoutMs);
        return sendCommandJson(res, {
            role: target.role,
            invoke: { dlg: target.dialog, btn: target.control,
                appearance: target.appearance, instance: target.owner },
            found,
        });
    }
    if (req.method === 'POST' && path === '/api/ui/invoke-when-ready') {
        const intent = parseUiInvokeIntent(res, query);
        if (!intent) return;
        return armUiInvokeIntent(req, res, intent);
    }
    if (req.method === 'POST' && path === '/api/ui/select') {
        const target = parseUiTarget(res, query, 'lb', 'listbox', ['index'], ['timeoutMs']);
        if (!target) return;
        const timeoutMs = parseCommandTimeout(res, query);
        if (timeoutMs === null) return;
        const index = parseInt32(res, query.get('index'), 'index');
        if (index === null) return;
        const total = target.controlState.total;
        if (!Number.isInteger(total) || index < 0 || index >= total) {
            return sendJson(res, 409, {
                error: `listbox ${target.dialog}::${target.control} cannot select index ${index}`,
            });
        }
        const found = await sendCommand(target.socket, Op.SetSelection, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control), i32(index),
        ]), timeoutMs);
        return sendCommandJson(res, {
            role: target.role,
            select: { dlg: target.dialog, lb: target.control, index,
                appearance: target.appearance, instance: target.owner },
            found,
        });
    }
    if (req.method === 'POST' && path === '/api/ui/select-scenario') {
        const target = parseUiTarget(res, query, 'lb', 'listbox', ['path']);
        if (!target) return;
        const exactPath = parseString(res, query.get('path'), 'path', 1, 259);
        if (exactPath === null) return;
        const found = await sendCommand(target.socket, Op.SelectScenarioPath, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control), encodeStr(exactPath),
        ]));
        return sendCommandJson(res, {
            role: target.role,
            scenario: { dlg: target.dialog, lb: target.control, path: exactPath,
                appearance: target.appearance, instance: target.owner },
            found,
        });
    }
    if (req.method === 'POST' && path === '/api/ui/spin') {
        const target = parseUiTarget(res, query, 'spin', 'spin', ['index'], ['timeoutMs']);
        if (!target) return;
        const timeoutMs = parseCommandTimeout(res, query);
        if (timeoutMs === null) return;
        const index = parseInt32(res, query.get('index'), 'index');
        if (index === null) return;
        const found = await sendCommand(target.socket, Op.SetSpin, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control), i32(index),
        ]), timeoutMs);
        return sendCommandJson(res, {
            role: target.role,
            spin: { dlg: target.dialog, spin: target.control, index,
                appearance: target.appearance, instance: target.owner },
            found,
        });
    }
    if (req.method === 'POST' && path === '/api/ui/edit-secret') {
        const target = parseUiTarget(res, query, 'edit', 'edit');
        if (!target) return;
        const chunks = [];
        let size = 0;
        for await (const chunk of req) {
            size += chunk.length;
            if (size > 4096) return sendJson(res, 413, { error: 'secret input exceeds limit' });
            chunks.push(chunk);
        }
        const text = decodeUtf8(Buffer.concat(chunks), 'secret input');
        const found = await sendCommand(target.socket, Op.SetEditText, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control), encodeStr(text),
        ]));
        return sendCommandJson(res, { role: target.role, found });
    }
    if (req.method === 'POST' && path === '/api/ui/edit') {
        const target = parseUiTarget(res, query, 'edit', 'edit', ['text'], ['timeoutMs']);
        if (!target) return;
        const timeoutMs = parseCommandTimeout(res, query);
        if (timeoutMs === null) return;
        const text = parseString(res, query.get('text'), 'text', 0, 4096);
        if (text === null) return;
        const found = await sendCommand(target.socket, Op.SetEditText, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control), encodeStr(text),
        ]), timeoutMs);
        return sendCommandJson(res, {
            role: target.role,
            edit: { dlg: target.dialog, edit: target.control,
                text: /password/i.test(target.control) ? '[redacted]' : text,
                appearance: target.appearance, instance: target.owner },
            found,
        });
    }
    if (req.method === 'POST' && path === '/api/ui/toggle') {
        const target = parseUiTarget(res, query, 'tog', 'toggle', [], ['timeoutMs']);
        if (!target) return;
        const timeoutMs = parseCommandTimeout(res, query);
        if (timeoutMs === null) return;
        const found = await sendCommand(target.socket, Op.InvokeToggle, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control),
        ]), timeoutMs);
        return sendCommandJson(res, {
            role: target.role,
            toggle: { dlg: target.dialog, tog: target.control,
                appearance: target.appearance, instance: target.owner },
            found,
        });
    }
    if (req.method === 'POST' && path === '/api/ui/enable-toggle') {
        const target = parseUiTarget(res, query, 'tog', 'toggle');
        if (!target) return;
        if (target.controlState.checked !== false) {
            return sendJson(res, 409, {
                error: `toggle ${target.dialog}::${target.control} is not explicitly unchecked`,
            });
        }
        const found = await sendCommand(target.socket, Op.EnableToggle, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control),
        ]));
        return sendCommandJson(res, {
            role: target.role,
            enableToggle: { dlg: target.dialog, tog: target.control,
                appearance: target.appearance, instance: target.owner },
            found,
        });
    }
    if (req.method === 'POST' && path === '/api/ui/enable-auto-battle') {
        const target = parseUiTarget(res, query, 'tog', 'toggle');
        if (!target) return;
        if (target.dialog !== 'DLG_BATTLE_A' || target.control !== 'TOG_AUTOBATTLE') {
            return sendJson(res, 400, {
                error: 'enable-auto-battle accepts only DLG_BATTLE_A::TOG_AUTOBATTLE',
            });
        }
        const kick = await sendCommand(target.socket, Op.EnableAutoBattle, Buffer.concat([
            u32(target.appearance), u32(target.owner), encodeStr(target.dialog),
            encodeStr(target.control),
        ]), AUTO_BATTLE_COMMAND_TIMEOUT_MS, Op.AutoBattleKickResult);
        return sendCommandJson(res, {
            role: target.role,
            autoBattle: { dlg: target.dialog, tog: target.control,
                appearance: target.appearance, instance: target.owner },
            found: kick?.succeeded === true,
            kick,
        });
    }
    if (req.method === 'POST' && [
        '/api/ui/move-toward', '/api/ui/hire', '/api/ui/move-unit', '/api/ui/dismiss',
    ].includes(path)) {
        const definitions = {
            '/api/ui/move-toward': [Op.MoveStackToward, 'move', ['id', 'x', 'y'], ['id'], ['x', 'y']],
            '/api/ui/hire': [Op.HireMerc, 'hire', ['camp', 'stack', 'unit'], ['camp', 'stack', 'unit'], []],
            '/api/ui/move-unit': [Op.MoveGroupUnit, 'moveUnit', ['stack', 'src', 'dst'], ['stack'], ['src', 'dst']],
            '/api/ui/dismiss': [Op.DismissUnit, 'dismiss', ['stack', 'unit'], ['stack', 'unit'], []],
        };
        const [opcode, resultName, fields, idFields, intFields] = definitions[path];
        const target = parseGenericMapTarget(res, query, fields, opcode === Op.MoveStackToward);
        if (!target) return;
        const values = {};
        for (const field of idFields) {
            values[field] = parseGameId(res, query, field);
            if (values[field] === null) return;
        }
        for (const field of intFields) {
            values[field] = parseInt32(res, query.get(field), field);
            if (values[field] === null) return;
            if (['src', 'dst'].includes(field) && (values[field] < 0 || values[field] > 5))
                return sendJson(res, 400, { error: 'formation slots must be within 0..5' });
        }
        const body = [u32(target.appearance), u32(target.owner),
            ...idFields.map((field) => encodeStr(values[field])),
            ...intFields.map((field) => i32(values[field]))];
        const found = await sendCommand(target.socket, opcode, Buffer.concat(body),
            target.commandTimeoutMs);
        return sendCommandJson(res, { role: target.role,
            [resultName]: { ...values, appearance: target.appearance, instance: target.owner }, found });
    }
    if (req.method === 'POST' && path === '/api/ui/move') {
        const target = parseMapActionTarget(res, query);
        if (!target) return;
        const found = await sendCommand(target.socket, Op.MoveStack,
            encodeMoveStackCommand(target), target.commandTimeoutMs);
        return sendCommandJson(res, {
            role: target.role,
            move: {
                id: target.id, fromx: target.fromX, fromy: target.fromY,
                frommp: target.fromMovement,
                x: target.x, y: target.y,
                appearance: target.appearance, instance: target.owner,
            },
            found,
        });
    }
    if (req.method === 'POST'
        && (path === '/api/ui/move-pair' || path === '/api/ui/long-move-pair')) {
        const fields = [
            'id', 'fromx', 'fromy', 'frommp', 'x', 'y', 'appearance', 'instance',
        ];
        const required = fields.flatMap((field) => [`host${field}`, `join${field}`]);
        if (!validateQuery(res, query, required, ['timeoutMs'])) return;
        const commandTimeoutMs = parseCommandTimeout(res, query);
        if (commandTimeoutMs === null) return;

        // Validate both immutable intents before either can mutate the game.
        const host = parsePairedMapActionTarget(
            res, query, 'host', 'host', commandTimeoutMs);
        if (!host) return;
        const join = parsePairedMapActionTarget(
            res, query, 'join', 'join', commandTimeoutMs);
        if (!join) return;
        if (host.socket === join.socket) {
            faultRelay('parallel move pair resolved host and join to one agent socket');
            return sendCommandJson(res, { found: false });
        }

        if (path === '/api/ui/move-pair') {
            // The attack adapter preserves the proved host-authority ordering:
            // admit host before join can occupy the host dispatcher, but never
            // wait for host completion. This is intentionally not the long-move
            // concurrency oracle below.
            const hostCommand = issueCommand(host.socket, Op.MoveStack,
                encodeMoveStackCommand(host), host.commandTimeoutMs);
            const hostStarted = await hostCommand.started;
            if (!hostStarted) {
                return sendCommandJson(res, {
                    found: false,
                    host: { found: null, startedMs: null },
                    join: { found: null, startedMs: null },
                });
            }
            const joinCommand = issueCommand(join.socket, Op.MoveStack,
                encodeMoveStackCommand(join), join.commandTimeoutMs);
            const [hostFound, joinStarted, joinFound] = await Promise.all([
                hostCommand.result, joinCommand.started, joinCommand.result,
            ]);
            return sendCommandJson(res, {
                found: hostFound === true && joinFound === true,
                dispatchSkewMs: joinStarted
                    && joinStarted.startedMs - hostStarted.startedMs,
                dispatchSkewKind: 'relay-command-started-receipt',
                host: {
                    found: hostFound,
                    startedMs: hostStarted.startedMs,
                    startedUiSeq: hostStarted.startedUiSeq,
                },
                join: {
                    found: joinFound,
                    startedMs: joinStarted?.startedMs ?? null,
                    startedUiSeq: joinStarted?.startedUiSeq ?? null,
                },
            });
        }

        // Preserve long_move_sim.ps1's one-shot parallel fire: write both exact
        // role commands back-to-back before awaiting any acknowledgement. The
        // host socket is written first only to make relay issue order
        // deterministic; either UI thread may publish CommandStarted first.
        // Waiting on one role before writing the other would hide precisely the
        // cross-player blocking/starvation this endpoint exists to expose.
        const hostCommand = issueCommand(host.socket, Op.MoveStack,
            encodeMoveStackCommand(host), host.commandTimeoutMs);
        const joinCommand = issueCommand(join.socket, Op.MoveStack,
            encodeMoveStackCommand(join), join.commandTimeoutMs);
        const [hostStarted, joinStarted] = await Promise.all([
            hostCommand.started, joinCommand.started,
        ]);
        if (!hostStarted || !joinStarted) {
            return sendCommandJson(res, {
                found: false,
                host: { found: null, startedMs: null },
                join: { found: null, startedMs: null },
            });
        }
        const [hostFound, joinFound] = await Promise.all([
            hostCommand.result, joinCommand.result,
        ]);
        const startedReceiptDeltaMs = joinStarted.startedMs - hostStarted.startedMs;
        return sendCommandJson(res, {
            found: hostFound === true && joinFound === true,
            dispatchSkewMs: Math.abs(startedReceiptDeltaMs),
            dispatchSkewKind: 'absolute-relay-command-started-receipt',
            dispatchOrder: startedReceiptDeltaMs < 0 ? 'join-first'
                : (startedReceiptDeltaMs > 0 ? 'host-first' : 'same-millisecond'),
            host: {
                found: hostFound,
                startedMs: hostStarted.startedMs,
                startedUiSeq: hostStarted.startedUiSeq,
            },
            join: {
                found: joinFound,
                startedMs: joinStarted?.startedMs ?? null,
                startedUiSeq: joinStarted?.startedUiSeq ?? null,
            },
        });
    }

    return sendJson(res, 404, { error: 'not found' });
}

const httpServer = http.createServer((req, res) => {
    handleHttp(req, res).catch((error) => {
        faultRelay(`HTTP handler threw: ${error.message}`);
        sendJson(res, 500, { error: 'relay is terminal', terminalFault: state.terminalFault });
    });
});
httpServer.on('error', (error) => listenFailure('http', error));
httpServer.listen(HTTP_PORT, HTTP_HOST, () => {
    console.log(`[http] api on http://${HTTP_HOST}:${HTTP_PORT}`);
});

function shutdown() {
    state.shuttingDown = true;
    process.exit(0);
}
process.once('SIGINT', shutdown);
process.once('SIGTERM', shutdown);
