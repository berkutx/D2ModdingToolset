import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, rm, writeFile, utimes} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import http from 'node:http';
import {EXTENSION_ORIGIN, STALE_AFTER_MS, readSnapshot, startBridge, parseArguments} from '../bridge/server.mjs';

async function fixture(t) {
  const gameDir = await mkdtemp(path.join(tmpdir(), 'd2-twitch-bridge-'));
  t.after(() => rm(gameDir, {recursive: true, force: true}));
  return gameDir;
}
function frame(pid, ts = Date.now()) {
  return {schema: 'c4dll.twitch-frame', version: 1, pid, battle_id: `${pid}-test-1`, ts, active: true,
    viewport: {left: 0, top: 0, width: 1024, height: 768}, snapshot: {schema: 'c4dll.battle-roster', schema_version: 1,
      units: [{name: 'Воин', stats: '100/100', effects: ['Защита']}], slots: Array.from({length: 12}, (_, i) => ({
        bounds: {left: i * 30, top: 10, right: i * 30 + 20, bottom: 30}, occupied: i === 0, unit_index: i === 0 ? 0 : null
      }))}};
}
async function put(gameDir, value) {
  const file = path.join(gameDir, `TwitchStat-live-${value.pid}.json`);
  await writeFile(file, JSON.stringify(value));
  return file;
}
function get(server, pathname, headers = {}, method = 'GET') {
  return new Promise((resolve, reject) => {
    const request = http.request({host: '127.0.0.1', port: server.address().port, path: pathname, method, headers}, response => {
      let body = '';
      response.setEncoding('utf8'); response.on('data', chunk => { body += chunk; });
      response.on('end', () => resolve({status: response.statusCode, headers: response.headers, body}));
    });
    request.on('error', reject); request.end();
  });
}

function readVirtualFrame(value, now, body = JSON.stringify(value)) {
  const io = {
    async readdir() { return [{name: `TwitchStat-live-${value.pid}.json`, isFile: () => true}]; },
    async stat() { return {mtimeMs: now, size: Buffer.byteLength(body)}; },
    async readFile() { return Buffer.from(body); }
  };
  return readSnapshot({gameDir: 'virtual', now}, io);
}

test('capture age expires at 6000 ms even when publication and file are recent', async () => {
  const now = 100000, current = {...frame(123, now), captured_at: now - 5999};
  const accepted = await readVirtualFrame(current, now);
  assert.equal(accepted.status.code, 'battle');
  assert.deepEqual(accepted.frame, current, 'publication and capture timestamps pass through unchanged');
  for (const age of [6000, 6001, 60000]) {
    const result = await readVirtualFrame({...current, captured_at: now - age}, now);
    assert.equal(result.status.code, 'stale', `capture age ${age}`);
    assert.equal(result.frame.active, false);
    assert.equal(result.frame.ts, now);
  }
});

test('legacy plugins without captured_at retain timestamp freshness and inactive compatibility', async () => {
  const now = 100000;
  for (const active of [true, false]) {
    const current = {...frame(123, now - 5999), active};
    if (!active) current.snapshot = null;
    const accepted = await readVirtualFrame(current, now);
    assert.deepEqual(accepted.frame, current);
    assert.equal(accepted.status.code, active ? 'battle' : 'idle');
    assert.equal((await readVirtualFrame({...current, ts: now - 6000}, now)).status.code, 'stale');
  }
});

test('capture timestamps must be finite nonnegative numbers no later than publication', async () => {
  const now = 100000, current = frame(123, now);
  for (const captured_at of [null, -1, now + 1, '100000', {}, [], false]) {
    const result = await readVirtualFrame({...current, captured_at}, now);
    assert.equal(result.status.code, 'invalid', JSON.stringify(captured_at));
    assert.equal(result.frame.active, false);
  }
  for (const nonfinite of ['1e999', '-1e999']) {
    const body = JSON.stringify({...current, captured_at: 'nonfinite'})
      .replace('"captured_at":"nonfinite"', `"captured_at":${nonfinite}`);
    assert.equal((await readVirtualFrame(current, now, body)).status.code, 'invalid');
  }
  assert.equal((await readVirtualFrame({...current, captured_at: now}, now)).status.code, 'battle');
  assert.equal((await readVirtualFrame({...frame(123, 1000), captured_at: 0}, 1000)).status.code, 'battle');
  // A valid capture time cannot bypass the existing bound on future publication timestamps.
  assert.equal((await readVirtualFrame({...current, ts: now + 5001, captured_at: now}, now)).status.code, 'stale');
});

test('bridge selects a fresh complete frame and clears stale files or stale payload timestamps', async t => {
  const gameDir = await fixture(t), current = frame(123);
  assert.equal((await readSnapshot({gameDir})).status.code, 'missing');
  const file = await put(gameDir, current);
  assert.deepEqual((await readSnapshot({gameDir})).frame, current);
  const old = new Date(Date.now() - STALE_AFTER_MS - 1000);
  await utimes(file, old, old);
  const staleFile = await readSnapshot({gameDir});
  assert.equal(staleFile.frame.active, false); assert.equal(staleFile.status.code, 'stale');
  await put(gameDir, frame(123, old.getTime()));
  assert.equal((await readSnapshot({gameDir})).status.code, 'stale');
  await put(gameDir, {...current, active: false, snapshot: null});
  assert.equal((await readSnapshot({gameDir})).status.code, 'idle');
});

test('fresh snapshots published during awaited I/O are compared with the clock after that I/O', async t => {
  let clock = 100000;
  t.mock.method(Date, 'now', () => clock);
  for (const delayedPhase of ['readdir', 'stat', 'readFile']) {
    clock = 100000;
    let current = frame(123, clock);
    async function phase(name) {
      await Promise.resolve();
      if (name === delayedPhase) {
        clock += 6001;
        // The plugin publishes a new complete file while the old read request waits.
        current = frame(123, clock);
      }
    }
    const io = {
      async readdir() {
        await phase('readdir');
        return [{name: 'TwitchStat-live-123.json', isFile: () => true}];
      },
      async stat() { await phase('stat'); return {mtimeMs: current.ts, size: 1000}; },
      async readFile() { await phase('readFile'); return Buffer.from(JSON.stringify(current)); }
    };
    // Exercise the production default clock, not just an explicitly supplied function.
    const result = await readSnapshot({gameDir: 'virtual'}, io);
    assert.equal(clock, 106001);
    assert.equal(result.status.code, 'battle', delayedPhase);
    assert.deepEqual(result.frame, current, delayedPhase);
  }
});

test('a snapshot that expires while readFile waits is still rejected at the unchanged 6 second TTL', async () => {
  let clock = 100000;
  const current = frame(123, clock);
  const io = {
    async readdir() { return [{name: 'TwitchStat-live-123.json', isFile: () => true}]; },
    async stat() { return {mtimeMs: current.ts, size: 1000}; },
    async readFile() {
      await Promise.resolve();
      clock += 6000;
      return Buffer.from(JSON.stringify(current));
    }
  };
  const result = await readSnapshot({gameDir: 'virtual', now: () => clock}, io);
  assert.equal(STALE_AFTER_MS, 6000);
  assert.equal(result.status.code, 'stale');
  assert.equal(result.frame.active, false);
  assert.equal(result.frame.ts, clock);
});

test('a numeric now remains supported for deterministic snapshot reads', async () => {
  const now = 100000, current = frame(123, now);
  const io = {
    async readdir() { return [{name: 'TwitchStat-live-123.json', isFile: () => true}]; },
    async stat() { return {mtimeMs: now, size: 1000}; },
    async readFile() { return Buffer.from(JSON.stringify(current)); }
  };
  assert.equal((await readSnapshot({gameDir: 'virtual', now}, io)).status.code, 'battle');
  assert.equal((await readSnapshot({gameDir: 'virtual', now: now + 6000}, io)).status.code, 'stale');
});

test('two fresh clients fail closed until an explicit PID selects one', async t => {
  const gameDir = await fixture(t);
  await put(gameDir, frame(123)); await put(gameDir, frame(456));
  const result = await readSnapshot({gameDir});
  assert.equal(result.status.code, 'multiple'); assert.equal(result.frame.active, false);
  assert.equal((await readSnapshot({gameDir, pid: 456})).frame.pid, 456);
  assert.equal((await readSnapshot({gameDir, pid: 789})).status.code, 'missing');
});

test('malformed, oversized and mismatched-PID snapshots never publish battle data', async t => {
  const gameDir = await fixture(t), file = path.join(gameDir, 'TwitchStat-live-123.json');
  await writeFile(file, '{');
  assert.equal((await readSnapshot({gameDir})).status.code, 'invalid');
  await writeFile(file, 'x'.repeat(1024 * 1024 + 1));
  assert.equal((await readSnapshot({gameDir})).frame.active, false);
  await writeFile(file, JSON.stringify(frame(456)));
  assert.equal((await readSnapshot({gameDir})).frame.active, false);
  await put(gameDir, {...frame(123), snapshot: {schema: 'unknown'}});
  assert.equal((await readSnapshot({gameDir})).status.code, 'invalid');
});

test('HTTP serves only explicit assets and refuses rebinding, cross-site snapshot reads and mutation', async t => {
  const gameDir = await fixture(t);
  await put(gameDir, frame(123));
  const server = await startBridge({gameDir, port: 0});
  t.after(() => new Promise(resolve => server.close(resolve)));
  assert.equal(server.address().address, '127.0.0.1');
  const response = await get(server, '/snapshot', {'Sec-Fetch-Site': 'same-origin'});
  assert.equal(response.status, 200); assert.equal(JSON.parse(response.body).frame.pid, 123);
  assert.equal(response.headers['access-control-allow-origin'], undefined);
  assert.equal(response.headers['cache-control'], 'no-store');
  for (const headers of [{Host: 'evil.test'}, {Origin: 'https://evil.test'}, {'Sec-Fetch-Site': 'cross-site'}, {'Sec-Fetch-Site': 'same-site'}]) {
    assert.equal((await get(server, '/snapshot', headers)).status, 403);
    assert.equal((await get(server, '/events', headers)).status, 403);
  }
  assert.equal((await get(server, '/snapshot', {}, 'POST')).status, 405);
  assert.equal((await get(server, '/snapshot', {}, 'OPTIONS')).status, 405);
  for (const requestPath of ['/server.mjs', '/package.json', '/TwitchStat-live-123.json', '/%2e%2e/server.mjs']) {
    assert.equal((await get(server, requestPath)).status, 404);
  }
  assert.equal((await get(server, '/video_overlay.html?local')).status, 200);
  assert.equal((await get(server, '/viewer.mjs')).status, 200);
});

test('local viewer allows cross-site document navigation but refuses embedding, fetches and cross-site assets', async t => {
  const gameDir = await fixture(t), server = await startBridge({gameDir, port: 0});
  t.after(() => new Promise(resolve => server.close(resolve)));
  const navigation = {'Sec-Fetch-Site': 'cross-site', 'Sec-Fetch-Mode': 'navigate', 'Sec-Fetch-Dest': 'document'};
  const response = await get(server, '/video_overlay.html?local', navigation);
  assert.equal(response.status, 200);
  assert.match(response.headers['content-type'], /^text\/html/);
  assert.match(response.headers['content-security-policy'], /frame-ancestors 'none'/);
  assert.equal(response.headers['access-control-allow-origin'], undefined);
  for (const headers of [
    {...navigation, 'Sec-Fetch-Dest': 'iframe'},
    {...navigation, 'Sec-Fetch-Dest': 'frame'},
    {...navigation, 'Sec-Fetch-Mode': 'cors', 'Sec-Fetch-Dest': 'empty'},
    {...navigation, 'Sec-Fetch-Mode': 'no-cors', 'Sec-Fetch-Dest': 'script'},
    {Origin: 'https://www.twitch.tv'}
  ]) assert.equal((await get(server, '/video_overlay.html?local', headers)).status, 403);
  for (const requestPath of ['/snapshot', '/events', '/viewer.mjs', '/protocol.mjs', '/overlay.css', '/config.html', '/live_config.html']) {
    assert.equal((await get(server, requestPath, navigation)).status, 403);
  }
  // After a document navigation the viewer itself fetches scripts and JSON on loopback.
  for (const requestPath of ['/viewer.mjs', '/protocol.mjs', '/overlay.css', '/snapshot']) {
    assert.equal((await get(server, requestPath, {'Sec-Fetch-Site': 'same-origin'})).status, 200);
  }
  assert.equal((await get(server, '/video_overlay.html?local', {...navigation, Host: 'evil.test'})).status, 403);
});

test('relay accepts only the exact extension origin and a UUID, with cross-site access restricted to document navigation', async t => {
  const gameDir = await fixture(t), server = await startBridge({gameDir, port: 0});
  t.after(() => new Promise(resolve => server.close(resolve)));
  const relayPath = (origin, nonce = '690f0a01-d68d-4459-963c-f9f65bed383d') => `/relay.html?origin=${encodeURIComponent(origin)}&nonce=${nonce}`;
  const navigation = {'Sec-Fetch-Site': 'cross-site', 'Sec-Fetch-Mode': 'navigate', 'Sec-Fetch-Dest': 'document'};
  assert.equal((await get(server, relayPath(EXTENSION_ORIGIN), navigation)).status, 200);
  assert.equal((await get(server, relayPath(EXTENSION_ORIGIN), {'Sec-Fetch-Site': 'cross-site'})).status, 403);
  for (const origin of ['https://evil.test', `${EXTENSION_ORIGIN}.evil.test`, 'null', 'https://www.twitch.tv']) {
    assert.equal((await get(server, relayPath(origin), navigation)).status, 400);
  }
  assert.equal((await get(server, relayPath(EXTENSION_ORIGIN, 'guessable'))).status, 400);
  assert.equal((await get(server, '/relay.html')).status, 400);
});

test('event stream pushes fresh frames and battle end without browser polling', {timeout: 7000}, async t => {
  const gameDir = await fixture(t), initial = frame(123);
  await put(gameDir, initial);
  const server = await startBridge({gameDir, port: 0});
  t.after(() => {
    server.closeAllConnections();
    return new Promise(resolve => server.close(resolve));
  });
  const controller = new AbortController();
  const response = await fetch(`http://127.0.0.1:${server.address().port}/events`, {
    headers: {'Sec-Fetch-Site': 'same-origin'}, signal: controller.signal
  });
  const reader = response.body.getReader(), decoder = new TextDecoder();
  let buffered = '';
  async function next() {
    while (!buffered.includes('\n\n')) {
      const {value, done} = await reader.read();
      assert.equal(done, false);
      buffered += decoder.decode(value, {stream: true});
    }
    const end = buffered.indexOf('\n\n'), event = buffered.slice(0, end);
    buffered = buffered.slice(end + 2);
    assert.ok(event.startsWith('data: '));
    return JSON.parse(event.slice(6));
  }
  try {
    assert.equal(response.status, 200);
    assert.match(response.headers.get('content-type'), /^text\/event-stream/);
    assert.equal(response.headers.get('access-control-allow-origin'), null);
    assert.deepEqual((await next()).frame, initial);
    const updated = frame(123); updated.snapshot.units[0].stats = '35/100';
    await put(gameDir, updated);
    assert.equal((await next()).frame.snapshot.units[0].stats, '35/100');
    await put(gameDir, {...frame(123), active: false, snapshot: null});
    assert.equal((await next()).frame.active, false);
  } finally { controller.abort(); await reader.cancel().catch(() => {}); }
});

test('CLI requires a source and accepts only a valid explicit PID', () => {
  assert.deepEqual(parseArguments(['--game-dir', 'C:\\game', '--pid', '123']), {gameDir: 'C:\\game', pid: 123});
  for (const args of [[], ['--game-dir'], ['--game-dir', 'x', '--pid', '-1'], ['--game-dir', 'x', '--pid', '4294967296'], ['--host', '0.0.0.0']]) {
    assert.throws(() => parseArguments(args));
  }
});
