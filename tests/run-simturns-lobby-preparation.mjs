// Offline only: imports the actual helper, mocks all server I/O, uses an owned temp directory.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { EventEmitter } from 'node:events';
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { CONTRACT, PreparationError, configuration, validateRecordPath, selectTemplate,
  validateReceipt, assertOwned, assertOwnedAttempt, preparationSummary, receiptStore, runAction, connectApi } from '../tools/test/simturns-lobby-preparation.mjs';

const clone = value => structuredClone(value);
const ID = '11111111-1111-1111-1111-111111111111';
const GAME = '22222222-2222-2222-2222-222222222222';
const ATTEMPT = '33333333-3333-3333-3333-333333333333';
const config = { origin: 'http://fixture.invalid:8200', user: 'test2', pass: 'TEST-SECRET-NOT-FOR-OUTPUT',
  recordPath: 'mock-run/owned-preparation.json', sitePackage: 'mock-package.json' };
const template = { id: CONTRACT.templateVersionId, name: 'Diligence 1.2.3', filename: CONTRACT.filename,
  maxPlayers: 2, published: true, isExample: true, allowedRaces: ['elves', 'clans'], defaults: { size: 48, roads: 20 } };
function ownedReceipt() {
  return { schema: 1, purpose: CONTRACT.purpose, id: ID, gameId: GAME, creator: 'test2',
    origin: config.origin, recordPath: config.recordPath, createdAt: 1000,
    template: { id: template.id, name: template.name, filename: template.filename }, parameters: { ...template.defaults } };
}
function preparation() {
  return { id: ID, gameId: GAME, creator: 'test2', createdAt: 1000, revision: 1, status: 'preparing',
    host: 'test2', firstTurn: 'test2', ranked: false, simultaneous: true, simultaneousUntil: 2,
    participants: CONTRACT.participants.map(v => ({ ...v, accepted: true, ready: false,
      autoAccepted: v.name === 'test1', autoReady: false, invitationDeclined: false, confirmationPaused: false })),
    templateVersionId: template.id, template: clone(template), parameters: { ...template.defaults },
    explicitParameters: [], bids: [], auctions: [], acceptedBidId: null, assignmentsFinalizedAt: null,
    launch: null, attempts: [], rights: { manage: true, launch: true, closeReview: false } };
}
function ready() {
  const p = preparation();
  p.revision = 2; p.status = 'ready'; p.assignmentsFinalizedAt = 2000; p.agreedAt = 2000;
  p.participants.forEach(v => { v.ready = true; v.autoReady = true; });
  return p;
}
function launched(status = 'confirmation') {
  const p = ready(); p.status = status; p.revision = 4;
  p.launch = { attemptId: ATTEMPT, revision: 2, status, createdAt: 3000,
    confirmations: p.participants.map(({ name }) => ({ name, mode: 'automatic' })) };
  if (status === 'room_created') Object.assign(p.launch, { roomId: 8, roomInstanceId: '8-12345', matchId: '1-12345' });
  return p;
}
function harness(initial = preparation(), receipt = null) {
  let p = clone(initial), saved = clone(receipt), createClaimed = false;
  let startClaimed = initial.launch ? { id: initial.id, revision: initial.launch.revision } : null;
  const calls = [];
  const store = {
    assertNew() { assert.equal(saved, null); assert.equal(createClaimed, false); },
    claimCreate() { assert.equal(createClaimed, false); createClaimed = true; },
    save(value) { assert.equal(saved, null); saved = clone(value); },
    read() { return validateReceipt(saved, config); },
    claimStart(value) { if (startClaimed) throw new PreparationError('receipt_write_refused'); startClaimed = { id: value.id, revision: value.revision }; },
    startIntent() { return clone(startClaimed); },
  };
  const emit = async (event, value) => {
    calls.push({ event, value: clone(value) });
    if (event === 'preparation:templates') return [clone(template)];
    if (event === 'preparation:create' || event === 'preparation:watch') return clone(p);
    assert.equal(event, 'preparation:command');
    assert.equal(value.id, p.id); assert.equal(value.revision, p.revision);
    if (value.action === 'assign') { p = ready(); }
    else if (value.action === 'start') { p = launched(); }
    else if (['cancel', 'review_close'].includes(value.action)) {
      p.status = 'cancelled'; p.revision++;
      if (value.action === 'review_close') p.reviewClose = { by: 'test2', at: 4000 };
    } else assert.fail(`Unexpected fixture command ${value.action}`);
    return clone(p);
  };
  return { emit, store, calls, receipt: () => clone(saved), set: value => { p = clone(value); } };
}
const code = expected => error => error instanceof PreparationError && error.code === expected;

test('import is offline; create uses exact casual defaults then a narrow assignment', async () => {
  const h = harness();
  const result = await runAction('create', config, h);
  assert.equal(result.preparation.status, 'ready');
  assert.equal(result.preparation.title, 'Diligence 1.2.3');
  assert.deepEqual(h.calls.map(v => [v.event, v.value.action]), [
    ['preparation:templates', undefined], ['preparation:create', undefined], ['preparation:command', 'assign']]);
  assert.deepEqual(h.calls[1].value.participants, CONTRACT.participants);
  assert.equal(h.calls[1].value.simultaneousUntil, 2);
  assert.equal(h.calls[1].value.ranked, false);
  assert.deepEqual(h.calls[2].value.data, { host: 'test2', firstTurn: 'test2', participants: [
    { name: 'test2', race: 'elves', lord: null }, { name: 'test1', race: 'clans', lord: null }] });
  assert.equal(h.receipt().id, ID);
  assert.ok(!JSON.stringify(h.receipt()).includes(config.pass));
});

test('already finalized fresh create does not assign twice', async () => {
  const h = harness(ready());
  await runAction('create', config, h);
  assert.equal(h.calls.length, 2);
});

test('failed assignment preserves owned receipt; no retry/reset commands exist', async () => {
  const h = harness(), baseEmit = h.emit;
  h.emit = async (event, value) => {
    if (value?.action === 'assign') throw new PreparationError('api_command_refused');
    return baseEmit(event, value);
  };
  await assert.rejects(runAction('create', config, h), code('api_command_refused'));
  assert.equal(h.receipt().id, ID);
  for (const action of ['retry', 'reset', 'launch', 'review_result', ''])
    await assert.rejects(runAction(action, config, h), code('unknown_action'));
});

test('start issues one command for ready own prep; repeated invocation never reoffers', async () => {
  const h = harness(ready(), ownedReceipt());
  const result = await runAction('start', config, h);
  assert.equal(result.preparation.launch.attemptId, ATTEMPT);
  await assert.rejects(runAction('start', config, h), code('preparation_not_fresh'));
  assert.equal(h.calls.filter(v => v.value.action === 'start').length, 1);
});

test('ambiguous start ACK remains claimed even if watch still appears fresh', async () => {
  const h = harness(ready(), ownedReceipt()), baseEmit = h.emit;
  h.emit = async (event, value) => {
    if (value?.action === 'start') throw new PreparationError('api_ack_failed');
    return baseEmit(event, value);
  };
  await assert.rejects(runAction('start', config, h), code('api_ack_failed'));
  await assert.rejects(runAction('start', config, h), code('receipt_write_refused'));
});

test('detail and cleanup refuse a launch not belonging to the recorded start revision', async () => {
  const p = launched();
  assert.throws(() => assertOwnedAttempt(p, null), code('foreign_launch_attempt'));
  assert.throws(() => assertOwnedAttempt(p, { id: ID, revision: 1 }), code('foreign_launch_attempt'));
  assert.equal(assertOwnedAttempt(p, { id: ID, revision: 2 }), p);
  for (const action of ['detail', 'close']) {
    const h = harness(ready(), ownedReceipt()); h.set(p);
    await assert.rejects(runAction(action, config, h), code('foreign_launch_attempt'));
    assert.equal(h.calls.length, 1);
  }
});

test('paused/declined/not-ready peer is never confirmed on their behalf', async () => {
  for (const field of ['ready', 'accepted', 'confirmationPaused', 'invitationDeclined']) {
    const p = ready(); p.participants[1][field] = !['ready', 'accepted'].includes(field);
    const h = harness(p, ownedReceipt());
    await assert.rejects(runAction('start', config, h), code('start_not_ready'));
    assert.equal(h.calls.length, 1);
  }
});

test('authenticated host start may supply only their own readiness', async () => {
  const p = ready(); p.status = 'preparing'; p.participants[0].ready = false; p.participants[0].autoReady = false;
  const h = harness(p, ownedReceipt());
  const result = await runAction('start', config, h);
  assert.equal(result.preparation.launch.attemptId, ATTEMPT);
  assert.deepEqual(h.calls[1].value.data, {});
});

test('detail exposes only bounded preparation metadata and consent, not private data', async () => {
  const p = launched('room_created');
  Object.assign(p, { chat: [{ text: 'PRIVATE-TEXT' }], token: 'PRIVATE-TOKEN', reviewReason: 'PRIVATE-REASON' });
  Object.assign(p.launch, { error: 'PRIVATE-ERROR', secret: 'PRIVATE-SECRET' });
  p.rights.secret = 'PRIVATE-RIGHTS';
  p.participants[0].profile = 'PRIVATE-PROFILE';
  const h = harness(p, ownedReceipt());
  const result = await runAction('detail', config, h), json = JSON.stringify(result);
  assert.equal(h.calls.length, 1);
  assert.ok(!json.includes('PRIVATE-'));
  assert.equal(result.preparation.launch.roomId, 8);
  assert.equal(result.preparation.launch.hasError, true);
  assert.equal(result.preparation.launch.errorCode, null);
  assert.equal(result.preparation.participants[1].ready, true);
});

test('cleanup uses cancel before room creation; review_close after creation/review; terminal no-op', async () => {
  for (const status of ['ready', 'confirmation', 'room_created', 'review', 'cancelled']) {
    const p = status === 'ready' ? ready() : launched(status === 'review' || status === 'cancelled' ? 'failed' : status);
    p.status = status; p.rights.closeReview = ['review', 'room_created'].includes(status);
    const h = harness(p, ownedReceipt());
    const result = await runAction('close', config, h);
    assert.equal(result.preparation.closedWithoutResult, true);
    const commands = h.calls.filter(v => v.event === 'preparation:command');
    assert.equal(commands.length, status === 'cancelled' ? 0 : 1);
    if (commands.length) assert.equal(commands[0].value.action, p.rights.closeReview ? 'review_close' : 'cancel');
  }
});

test('cleanup refuses any result, permissions loss, or changed own conditions', async () => {
  for (const mutation of [p => { p.status = 'completed'; }, p => { p.reviewResult = {}; },
    p => { p.result = {}; }, p => { p.rights.manage = false; }]) {
    const p = ready(); mutation(p); const h = harness(p, ownedReceipt());
    await assert.rejects(runAction('close', config, h), PreparationError);
    assert.equal(h.calls.length, 1);
  }
});

test('foreign identity, tournament, terms, duplicate roster, and launch changes fail closed', () => {
  const mutations = [p => { p.id = GAME; }, p => { p.gameId = ID; }, p => { p.creator = 'test1'; },
    p => { p.ranked = true; }, p => { p.simultaneous = false; }, p => { p.simultaneousUntil = 7; },
    p => { p.firstTurn = 'test1'; }, p => { p.host = 'test1'; }, p => { p.championshipId = ID; },
    p => { p.seriesId = ID; }, p => { p.matchId = 3; }, p => { p.participants[1].name = 'test2'; },
    p => { p.participants.reverse(); }, p => { p.participants[0].race = 'clans'; },
    p => { p.participants[0].lord = 'mage'; }, p => { p.participants[0].team = 2; },
    p => { p.parameters.size++; }, p => { p.explicitParameters.push('size'); },
    p => { p.bids.push({}); }, p => { p.auctions.push({}); }, p => { p.template.name = 'Changed'; },
    p => { p.templateVersionId = GAME; }, p => { p.createdAt++; }, p => { p.revision = 0; },
    p => { p.status = 'unexpected'; }, p => { p.launch.confirmations[1].name = 'stranger'; },
    p => { p.launch.revision = 100; }, p => { p.launch.attemptId = 'bad'; },
    p => { p.attempts.push({}); }, p => { p.retryReason = 'A retry'; }];
  for (const mutate of mutations) {
    const p = launched(); mutate(p);
    assert.throws(() => assertOwned(p, ownedReceipt()), PreparationError);
  }
});

test('catalog pins exact published example, capacity/races and numeric defaults', () => {
  assert.equal(selectTemplate([clone(template)]).name, template.name);
  for (const mutate of [t => { t.published = false; }, t => { t.isExample = false; },
    t => { t.id = ID; }, t => { t.filename = 'Other.lua'; }, t => { t.maxPlayers = 4; },
    t => { t.allowedRaces = ['elves']; }, t => { t.defaults.size = '48'; }]) {
    const t = clone(template); mutate(t);
    assert.throws(() => selectTemplate([t]), PreparationError);
  }
  assert.throws(() => selectTemplate([template, template]), PreparationError);
});

test('receipt path and wx guards preserve prior records and ambiguous create intents', () => {
  const root = mkdtempSync(join(tmpdir(), 'simturns-preparation-test-'));
  try {
    const artifacts = join(root, 'artifacts'), run = join(artifacts, 'fresh-run');
    mkdirSync(run, { recursive: true });
    const path = join(run, 'owned-preparation.json'), pkg = join(root, 'package.json');
    writeFileSync(pkg, '{}', { flag: 'wx' });
    assert.equal(validateRecordPath(path, artifacts), path);
    assert.throws(() => validateRecordPath(join(artifacts, 'owned-preparation.json'), artifacts), PreparationError);
    assert.throws(() => validateRecordPath(join(root, 'owned-preparation.json'), artifacts), PreparationError);
    assert.throws(() => validateRecordPath(join(run, 'other.json'), artifacts), PreparationError);
    assert.throws(() => validateRecordPath('relative/owned-preparation.json', artifacts), PreparationError);
    const env = { OH_SITE_ORIGIN: config.origin, OH_SITE_PACKAGE: pkg, OH_PREPARATION_RECORD_PATH: path,
      D2_LOBBY_HOST_ACCOUNT: 'test2', D2_LOBBY_JOIN_ACCOUNT: 'test1', D2_LOBBY_HOST_PASSWORD: config.pass };
    const c = configuration(env, artifacts), s = receiptStore(c);
    for (const origin of ['https://user:secret@fixture.invalid', 'http://fixture.invalid/path', 'file:///tmp/', 'http://fixture.invalid/?token=secret'])
      assert.throws(() => configuration({ ...env, OH_SITE_ORIGIN: origin }, artifacts), PreparationError);
    assert.throws(() => configuration({ ...env, D2_LOBBY_JOIN_ACCOUNT: 'stranger' }, artifacts), PreparationError);
    s.assertNew(); s.claimCreate();
    assert.throws(() => s.assertNew(), code('create_already_claimed'));
    const r = { ...ownedReceipt(), recordPath: path }; s.save(r);
    const before = readFileSync(path, 'utf8');
    assert.throws(() => s.save({ ...r, id: GAME }), code('receipt_write_refused'));
    assert.equal(readFileSync(path, 'utf8'), before);
    assert.equal(s.read().id, ID);
    s.claimStart(ready()); assert.throws(() => s.claimStart(ready()), code('receipt_write_refused'));
    assert.deepEqual(s.startIntent(), { id: ID, revision: 2 });
    assert.throws(() => validateReceipt({ ...r, origin: 'http://foreign.invalid' }, c), PreparationError);
    assert.ok(!before.includes(config.pass));
  } finally {
    // Only the exact directory allocated by mkdtemp above; no workspace/user paths.
    rmSync(root, { recursive: true, force: true });
  }
});

test('API adapter bounds calls, disables reconnect/redirect, and sanitizes remote errors', async () => {
  const fetches = [], sockets = [];
  let response = { ok: true, data: { example: true } };
  const fetchImpl = async (url, options) => {
    fetches.push({ url, options });
    return { ok: true, json: async () => ({ ok: true }), headers: { getSetCookie: () => ['session=PRIVATE-COOKIE; HttpOnly'] } };
  };
  const ioFactory = (url, options) => {
    const socket = new EventEmitter();
    socket.connect = () => queueMicrotask(() => socket.emit('connect'));
    socket.disconnect = () => { socket.disconnected = true; };
    socket.timeout = ms => { assert.equal(ms, 15000); return { emitWithAck: async () => response }; };
    sockets.push({ url, options, socket }); return socket;
  };
  const api = await connectApi(config, { fetchImpl, ioFactory });
  assert.equal(fetches[0].options.redirect, 'error');
  assert.equal(sockets[0].options.reconnection, false);
  assert.equal(sockets[0].options.autoConnect, false);
  assert.deepEqual(await api.emit('preparation:watch', { id: ID }), { example: true });
  response = { ok: false, error: 'PRIVATE-COOKIE PRIVATE-TOKEN PRIVATE-PASSWORD' };
  await assert.rejects(api.emit('preparation:command', {}), code('api_command_refused'));
  api.disconnect(); assert.equal(sockets[0].socket.disconnected, true);
  await assert.rejects(connectApi(config, { ioFactory, fetchImpl: async () => { throw new Error('PRIVATE-ERROR'); } }), code('login_failed'));
});

test('projection rejects dangerous metadata shape by omission and never emits arbitrary server errors', () => {
  const p = launched(); p.launch.roomId = { token: 'private' }; p.launch.matchId = 'private?token=secret';
  const result = preparationSummary(p);
  assert.ok(!Object.hasOwn(result.launch, 'roomId'));
  assert.ok(!Object.hasOwn(result.launch, 'matchId'));
});
