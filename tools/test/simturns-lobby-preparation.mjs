// Owned casual preparation control for the native OH harness. Importing is offline.
// CLI: create | start | detail | close. Credentials/cookies never enter receipts/output.
import { createRequire } from 'node:module';
import { existsSync, lstatSync, readFileSync, realpathSync, statSync, writeFileSync } from 'node:fs';
import { basename, dirname, isAbsolute, relative, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const CONTRACT = Object.freeze({
  purpose: 'simturns-lobby-e2e-v1', creator: 'test2', host: 'test2', firstTurn: 'test2',
  templateVersionId: 'b056a02e-d2e6-43b0-9b0d-26ee3df57e5a', filename: 'Diligence.lua',
  ranked: false, simultaneous: true, simultaneousUntil: 2,
  participants: Object.freeze([
    Object.freeze({ name: 'test2', team: 1, race: 'elves', lord: '' }),
    Object.freeze({ name: 'test1', team: 2, race: 'clans', lord: '' }),
  ]),
});
const ACTIONS = new Set(['create', 'start', 'detail', 'close']);
const STATUSES = new Set(['preparing', 'ready', 'waiting_host', 'host_busy', 'confirmation',
  'generating', 'deferred', 'failed', 'cancelling', 'room_created', 'review', 'cancelled', 'completed']);
const LAUNCH_STATUSES = new Set([...STATUSES].filter(v => !['preparing', 'ready', 'review', 'completed'].includes(v)));
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/u;
const REASON = 'Техническая проверка ОХ; закрытие подготовки без результата и без рейтинга.';
export class PreparationError extends Error {
  constructor(code) { super(code); this.code = code; }
}
const check = (yes, code) => { if (!yes) throw new PreparationError(code); };
const object = value => value && typeof value === 'object' && !Array.isArray(value);
const timestamp = value => Number.isSafeInteger(value) && value > 0;
const safeText = value => typeof value === 'string' && value.length > 0 && value.length <= 256
  && !/[\x00-\x1f\x7f]/u.test(value);
const stable = value => JSON.stringify(Object.entries(value).sort(([a], [b]) => a.localeCompare(b)));
function parameters(value) {
  check(object(value) && Object.keys(value).length <= 64 && Object.entries(value).every(([k, v]) =>
    /^[a-zA-Z][a-zA-Z0-9_:]{0,63}$/u.test(k) && Number.isSafeInteger(v)), 'invalid_parameters');
  return { ...value };
}
function participants(value) {
  check(Array.isArray(value) && value.length === 2 && value.every((v, i) => object(v)
    && Object.entries(CONTRACT.participants[i]).every(([k, expected]) => v[k] === expected)), 'participants_changed');
}

export function validateRecordPath(requested, artifactsRoot = fileURLToPath(new URL('../../artifacts/', import.meta.url))) {
  check(typeof requested === 'string' && isAbsolute(requested), 'record_path_required');
  const path = resolve(requested), parent = realpathSync(dirname(path)), root = realpathSync(artifactsRoot);
  const within = relative(root, parent);
  check(basename(path) === 'owned-preparation.json' && statSync(parent).isDirectory()
    && within && within !== '..' && !within.startsWith(`..${sep}`) && !isAbsolute(within)
    && parent.toLowerCase() === resolve(dirname(path)).toLowerCase(), 'record_path_outside_run');
  if (existsSync(path)) check(lstatSync(path).isFile() && !lstatSync(path).isSymbolicLink(), 'invalid_record_file');
  return path;
}

export function configuration(env, artifactsRoot) {
  check(env.D2_LOBBY_HOST_ACCOUNT === CONTRACT.host && env.D2_LOBBY_JOIN_ACCOUNT === 'test1'
    && typeof env.D2_LOBBY_HOST_PASSWORD === 'string' && env.D2_LOBBY_HOST_PASSWORD.length > 0, 'test_credentials_required');
  let url;
  try { url = new URL(env.OH_SITE_ORIGIN); } catch { throw new PreparationError('origin_required'); }
  check(['http:', 'https:'].includes(url.protocol) && !url.username && !url.password
    && url.pathname === '/' && !url.search && !url.hash, 'invalid_origin');
  check(typeof env.OH_SITE_PACKAGE === 'string' && isAbsolute(env.OH_SITE_PACKAGE)
    && basename(env.OH_SITE_PACKAGE) === 'package.json' && statSync(env.OH_SITE_PACKAGE).isFile(), 'site_package_required');
  return { origin: url.origin, user: CONTRACT.host, pass: env.D2_LOBBY_HOST_PASSWORD,
    sitePackage: env.OH_SITE_PACKAGE, recordPath: validateRecordPath(env.OH_PREPARATION_RECORD_PATH, artifactsRoot) };
}

export function selectTemplate(catalog) {
  check(Array.isArray(catalog), 'invalid_template_catalog');
  const found = catalog.filter(t => t?.id === CONTRACT.templateVersionId);
  check(found.length === 1 && found[0].published === true && found[0].isExample === true
    && found[0].filename === CONTRACT.filename && safeText(found[0].name) && found[0].maxPlayers === 2
    && Array.isArray(found[0].allowedRaces) && ['elves', 'clans'].every(r => found[0].allowedRaces.includes(r)), 'template_mismatch');
  return { id: found[0].id, name: found[0].name, filename: found[0].filename, defaults: parameters(found[0].defaults) };
}

export function validateReceipt(receipt, config) {
  check(object(receipt) && receipt.schema === 1 && receipt.purpose === CONTRACT.purpose
    && receipt.creator === CONTRACT.creator && receipt.origin === config.origin
    && receipt.recordPath === config.recordPath && UUID.test(receipt.id) && UUID.test(receipt.gameId)
    && timestamp(receipt.createdAt), 'foreign_receipt');
  check(receipt.template?.id === CONTRACT.templateVersionId && receipt.template.filename === CONTRACT.filename
    && safeText(receipt.template.name), 'receipt_template_mismatch');
  parameters(receipt.parameters);
  return receipt;
}

export function assertOwned(p, receipt) {
  check(object(p) && p.id === receipt.id && p.gameId === receipt.gameId && p.creator === CONTRACT.creator
    && p.host === CONTRACT.host && p.firstTurn === CONTRACT.firstTurn && p.createdAt === receipt.createdAt
    && Number.isSafeInteger(p.revision) && p.revision >= 1 && STATUSES.has(p.status), 'preparation_identity_changed');
  check(p.ranked === false && p.simultaneous === true && p.simultaneousUntil === 2
    && !p.championshipId && !p.seriesId && !p.matchId, 'preparation_mode_changed');
  participants(p.participants);
  check(p.templateVersionId === receipt.template.id && p.template?.id === receipt.template.id
    && p.template.name === receipt.template.name && p.template.filename === receipt.template.filename, 'template_changed');
  check(stable(parameters(p.parameters)) === stable(receipt.parameters)
    && Array.isArray(p.explicitParameters) && p.explicitParameters.length === 0
    && Array.isArray(p.bids) && p.bids.length === 0 && Array.isArray(p.auctions) && p.auctions.length === 0
    && !p.acceptedBidId, 'preparation_terms_changed');
  check(Array.isArray(p.attempts) && p.attempts.length === 0 && !p.retryReason, 'preparation_retried');
  check(p.assignmentsFinalizedAt == null || timestamp(p.assignmentsFinalizedAt), 'invalid_assignments');
  if (p.launch != null) {
    check(object(p.launch) && UUID.test(p.launch.attemptId) && Number.isSafeInteger(p.launch.revision)
      && p.launch.revision >= 1 && p.launch.revision <= p.revision && LAUNCH_STATUSES.has(p.launch.status)
      && timestamp(p.launch.createdAt), 'invalid_launch');
    check(Array.isArray(p.launch.confirmations) && p.launch.confirmations.length === 2
      && p.launch.confirmations.every((v, i) => v.name === CONTRACT.participants[i].name
        && ['automatic', 'manual'].includes(v.mode)), 'invalid_launch_consents');
  }
  return p;
}

const noResult = p => check(p.status !== 'completed' && !p.reviewResult && !p.result, 'result_already_present');
export function assertFresh(p) {
  noResult(p);
  check(['preparing', 'ready'].includes(p.status) && p.launch == null && Array.isArray(p.attempts)
    && p.attempts.length === 0 && !p.reviewClose && !p.cancelledAt, 'preparation_not_fresh');
}

export function assertOwnedAttempt(p, intent) {
  // start() passes the current preparation revision into beginLaunch(). The
  // intent therefore pins the attempt even when the start ACK was lost.
  if (p.launch != null) check(intent?.id === p.id && intent.revision === p.launch.revision, 'foreign_launch_attempt');
  return p;
}

export function preparationSummary(p) {
  // Site readiness is not native consent or proof of game entry.
  const consent = ['accepted', 'ready', 'autoAccepted', 'autoReady', 'invitationDeclined', 'confirmationPaused'];
  const launch = p.launch == null ? null : {
    attemptId: p.launch.attemptId, revision: p.launch.revision, status: p.launch.status,
    createdAt: p.launch.createdAt, confirmations: p.launch.confirmations.map(({ name, mode }) => ({ name, mode })),
    hasError: !!p.launch.error,
    errorCode: ['room_no_longer_active', 'core_restarted_outcome_unknown'].includes(p.launch.error) ? p.launch.error : null,
  };
  if (launch) for (const k of ['roomId', 'roomInstanceId', 'matchId']) {
    const v = p.launch[k];
    if (v !== undefined && (Number.isSafeInteger(v) && v >= 0
      || typeof v === 'string' && /^[a-zA-Z0-9_-]{1,128}$/u.test(v))) launch[k] = v;
  }
  return { id: p.id, gameId: p.gameId, status: p.status, revision: p.revision, creator: p.creator,
    host: p.host, firstTurn: p.firstTurn, ranked: p.ranked, simultaneous: p.simultaneous,
    simultaneousUntil: p.simultaneousUntil, templateVersionId: p.templateVersionId, title: p.template.name,
    template: { id: p.template.id, name: p.template.name, filename: p.template.filename },
    parameters: { ...p.parameters }, explicitParameters: [],
    assignmentsFinalizedAt: p.assignmentsFinalizedAt ?? null, agreedAt: p.agreedAt ?? null,
    participants: p.participants.map(({ name, race, lord, team, ...other }) => ({ name, race, lord, team,
      ...Object.fromEntries(consent.filter(k => typeof other[k] === 'boolean').map(k => [k, other[k]])) })),
    launch, resultPresent: p.status === 'completed' || !!p.reviewResult || !!p.result,
    closedWithoutResult: p.status === 'cancelled' && !p.reviewResult && !p.result };
}

export function receiptStore(config) {
  const exclusive = (path, data) => {
    try { writeFileSync(path, `${JSON.stringify(data, null, 2)}\n`, { flag: 'wx', mode: 0o600 }); }
    catch { throw new PreparationError('receipt_write_refused'); }
  };
  return {
    assertNew() {
      check(!existsSync(config.recordPath), 'receipt_already_exists');
      check(!existsSync(`${config.recordPath}.create-intent`), 'create_already_claimed');
    },
    claimCreate() { exclusive(`${config.recordPath}.create-intent`, { purpose: CONTRACT.purpose, origin: config.origin }); },
    save(value) { exclusive(config.recordPath, value); },
    read() {
      try { return validateReceipt(JSON.parse(readFileSync(config.recordPath, 'utf8')), config); }
      catch (error) { throw error instanceof PreparationError ? error : new PreparationError('receipt_read_failed'); }
    },
    claimStart(value) { exclusive(`${config.recordPath}.start-intent`, { id: value.id, revision: value.revision }); },
    startIntent() {
      const path = `${config.recordPath}.start-intent`;
      if (!existsSync(path)) return null;
      try {
        const intent = JSON.parse(readFileSync(path, 'utf8'));
        check(UUID.test(intent?.id) && Number.isSafeInteger(intent.revision) && intent.revision >= 1, 'invalid_start_intent');
        return intent;
      } catch (error) { throw error instanceof PreparationError ? error : new PreparationError('start_intent_read_failed'); }
    },
  };
}

export async function runAction(action, config, { emit, store = receiptStore(config) }) {
  check(ACTIONS.has(action), 'unknown_action');
  let p, receipt;
  if (action === 'create') {
    store.assertNew();
    const template = selectTemplate(await emit('preparation:templates', {}));
    // Persist intent before create. An ambiguous ACK must never cause a duplicate create.
    store.claimCreate();
    p = await emit('preparation:create', { participants: CONTRACT.participants.map(v => ({ ...v })),
      host: CONTRACT.host, firstTurn: CONTRACT.firstTurn, templateVersionId: template.id,
      parameters: template.defaults, explicitParameters: [], ranked: false, simultaneous: true, simultaneousUntil: 2 });
    receipt = validateReceipt({ schema: 1, purpose: CONTRACT.purpose, id: p?.id, gameId: p?.gameId,
      createdAt: p?.createdAt, creator: CONTRACT.creator, origin: config.origin, recordPath: config.recordPath,
      template: { id: template.id, name: template.name, filename: template.filename }, parameters: template.defaults }, config);
    assertOwned(p, receipt);
    store.save(receipt); // Keep ownership even if the following assign fails.
    assertFresh(p);
    if (!p.assignmentsFinalizedAt) {
      check(p.rights?.manage === true, 'assign_not_authorized');
      p = assertOwned(await emit('preparation:command', { id: p.id, revision: p.revision, action: 'assign',
        data: { host: CONTRACT.host, firstTurn: CONTRACT.firstTurn,
          participants: CONTRACT.participants.map(({ name, race }) => ({ name, race, lord: null })) } }), receipt);
      assertFresh(p);
      check(timestamp(p.assignmentsFinalizedAt), 'assign_not_finalized');
    }
  } else {
    receipt = store.read();
    p = assertOwnedAttempt(assertOwned(await emit('preparation:watch', { id: receipt.id }), receipt), store.startIntent());
    if (action === 'start') {
      assertFresh(p);
      check(timestamp(p.assignmentsFinalizedAt) && p.rights?.launch === true
        && p.participants.every(v => !v.invitationDeclined && !v.confirmationPaused
          && (v.name === CONTRACT.host || v.accepted === true && v.ready === true)), 'start_not_ready');
      store.claimStart(p); // One command, even if its reply is lost.
      p = assertOwnedAttempt(assertOwned(await emit('preparation:command', { id: p.id, revision: p.revision,
        action: 'start', data: {} }), receipt), store.startIntent());
      noResult(p);
      check(p.launch != null, 'start_not_launched');
    } else if (action === 'close') {
      noResult(p);
      if (p.status !== 'cancelled') {
        const review = ['review', 'room_created'].includes(p.status);
        check(review ? p.rights?.closeReview === true : p.rights?.manage === true, 'close_not_authorized');
        p = assertOwnedAttempt(assertOwned(await emit('preparation:command', { id: p.id, revision: p.revision,
          action: review ? 'review_close' : 'cancel', data: { reason: REASON } }), receipt), store.startIntent());
        noResult(p);
        check(p.status === 'cancelled', 'close_not_confirmed');
      }
    }
  }
  return { ok: true, action, preparation: preparationSummary(p) };
}

export async function connectApi(config, { fetchImpl = globalThis.fetch, ioFactory } = {}) {
  let login;
  try {
    login = await fetchImpl(`${config.origin}/api/login`, { method: 'POST', redirect: 'error',
      headers: { 'content-type': 'application/json' }, body: JSON.stringify({ user: config.user, pass: config.pass }),
      signal: AbortSignal.timeout(10000) });
    check(login.ok && (await login.json())?.ok === true, 'login_failed');
  } catch { throw new PreparationError('login_failed'); }
  const cookie = login.headers.getSetCookie().map(v => v.split(';')[0]).join('; ');
  check(cookie, 'session_cookie_missing');
  const io = ioFactory || createRequire(config.sitePackage)('socket.io-client').io;
  const socket = io(`${config.origin}/lobby`, { autoConnect: false, transports: ['websocket'],
    reconnection: false, timeout: 10000, extraHeaders: { Cookie: cookie, Origin: config.origin } });
  try {
    await new Promise((accept, reject) => {
      const timer = setTimeout(() => reject(new PreparationError('socket_connect_failed')), 12000);
      socket.once('connect', () => { clearTimeout(timer); accept(); });
      socket.once('connect_error', () => { clearTimeout(timer); reject(new PreparationError('socket_connect_failed')); });
      socket.connect();
    });
  } catch { socket.disconnect(); throw new PreparationError('socket_connect_failed'); }
  return { disconnect: () => socket.disconnect(), async emit(event, value) {
    let result;
    try { result = await socket.timeout(15000).emitWithAck(event, value); }
    catch { throw new PreparationError('api_ack_failed'); }
    check(result?.ok === true, 'api_command_refused');
    return result.data;
  } };
}

export async function main(argv = process.argv.slice(2), env = process.env) {
  let api;
  const action = ACTIONS.has(argv[0]) ? argv[0] : 'invalid';
  try {
    check(argv.length === 1 && ACTIONS.has(action), 'unknown_action');
    const config = configuration(env), store = receiptStore(config);
    if (action === 'create') store.assertNew(); else store.read();
    api = await connectApi(config);
    process.stdout.write(`${JSON.stringify(await runAction(action, config, { emit: api.emit, store }))}\n`);
    return 0;
  } catch (error) {
    process.stdout.write(`${JSON.stringify({ ok: false, action,
      error: error instanceof PreparationError ? error.code : 'operation_failed' })}\n`);
    return 1;
  } finally { api?.disconnect(); }
}

if (process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url)
  process.exitCode = await main();
