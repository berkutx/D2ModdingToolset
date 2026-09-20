import http from 'node:http';
import {readFile, readdir, stat} from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {MAX_FRAME_BYTES, validateFrame} from '../web/protocol.mjs';

export const EXTENSION_ORIGIN = 'https://pvffxvvhlpi5o8qe3ybjjpwb5hh7n7.ext-twitch.tv';
export const STALE_AFTER_MS = 6000;
const directory = path.dirname(fileURLToPath(import.meta.url));
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const LIVE_FILE = /^TwitchStat-live-([1-9]\d{0,9})\.json$/;
const webAssets = new Set(['video_overlay.html', 'viewer.mjs', 'overlay.css', 'protocol.mjs',
  'config.html', 'control.css', 'live_config.html', 'broadcaster.mjs', 'game-text.mjs']);
const types = {'.html': 'text/html; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8'};

function inactive(code, message, now, pid) {
  return {
    frame: {schema: 'c4dll.twitch-frame', version: 1, battle_id: `bridge:${code}`,
      ts: now, active: false, snapshot: null},
    status: {code, message, ...(pid ? {pid} : {})}
  };
}

function validPid(pid) { return Number.isInteger(pid) && pid > 0 && pid <= 0xffffffff; }

// The plugin replaces one complete JSON file atomically. Reading never touches game memory.
// With two fresh clients the user must choose --pid; guessing could publish the wrong battle.
export async function readSnapshot({gameDir, pid = null, now = Date.now}, io = {readdir, stat, readFile}) {
  if (!gameDir || (pid !== null && !validPid(pid))) throw new TypeError('Invalid bridge source options');
  // A queued I/O operation may finish after the plugin has replaced the snapshot.
  // Measure age after each awaited phase, rather than against the request's start time.
  // Numeric `now` remains available for deterministic callers; a function is a live clock.
  const currentTime = typeof now === 'function' ? now : () => now;
  let candidates;
  try {
    const entries = await io.readdir(gameDir, {withFileTypes: true});
    candidates = entries.filter(entry => entry.isFile() && LIVE_FILE.test(entry.name))
      .map(entry => ({name: entry.name, pid: Number(LIVE_FILE.exec(entry.name)[1])}))
      .filter(entry => validPid(entry.pid) && (pid === null || entry.pid === pid));
  } catch {
    return inactive('unavailable', 'Папка игры недоступна. Проверьте --game-dir.', currentTime(), pid);
  }
  if (!candidates.length) return inactive('missing', 'Ожидаем плагин TwitchStat в игре.', currentTime(), pid);
  const checked = await Promise.all(candidates.map(async candidate => {
    try {
      const info = await io.stat(path.join(gameDir, candidate.name));
      return {...candidate, size: info.size, mtimeMs: info.mtimeMs};
    } catch { return null; }
  }));
  const checkedAt = currentTime();
  const fresh = checked.filter(entry => entry && checkedAt - entry.mtimeMs < STALE_AFTER_MS && checkedAt - entry.mtimeMs >= -5000);
  if (!fresh.length) return inactive('stale', 'Данные игры устарели. Ожидаем следующий снимок боя.', checkedAt, pid);
  if (fresh.length > 1) return inactive('multiple', 'Обнаружено несколько клиентов. Перезапустите мост с --pid нужной игры.', checkedAt);
  const selected = fresh[0];
  if (selected.size > MAX_FRAME_BYTES) return inactive('invalid', 'Снимок игры превышает допустимый размер.', checkedAt, selected.pid);
  try {
    const bytes = await io.readFile(path.join(gameDir, selected.name));
    if (bytes.length > MAX_FRAME_BYTES) throw new Error('Oversized snapshot');
    const frame = validateFrame(JSON.parse(bytes.toString('utf8')));
    // Publication time orders frames; a sliced roster may contain older captured data. Keep those
    // clocks separate so a recent publication cannot make an expired capture look fresh.
    // Older plugins have no captured_at and retain their original ts-based freshness contract.
    const capturedAt = Object.hasOwn(frame, 'captured_at') ? frame.captured_at : frame.ts;
    if (!Number.isFinite(capturedAt) || capturedAt < 0 || capturedAt > frame.ts) {
      throw new Error('Invalid capture timestamp');
    }
    const readAt = currentTime();
    if (frame.pid !== selected.pid || readAt - capturedAt >= STALE_AFTER_MS || frame.ts > readAt + 5000) {
      return inactive('stale', 'Ожидаем актуальный снимок выбранной игры.', readAt, selected.pid);
    }
    return {frame, status: {code: frame.active ? 'battle' : 'idle', pid: selected.pid,
      message: frame.active ? `Бой подключён: ${frame.snapshot.units.length} юнитов.` : 'Игра подключена. Ожидаем поддерживаемый бой.'}};
  } catch {
    return inactive('invalid', 'Снимок игры недоступен или имеет неизвестный формат.', currentTime(), selected.pid);
  }
}

export function createBridgeServer({gameDir, pid = null} = {}) {
  if (!gameDir || (pid !== null && !validPid(pid))) throw new TypeError('Specify gameDir and a valid optional pid');
  const source = {gameDir: path.resolve(gameDir), pid};
  const server = http.createServer(async (request, response) => {
    const address = server.address();
    const host = `127.0.0.1:${address.port}`, origin = `http://${host}`;
    const headers = type => ({'Content-Type': type, 'Cache-Control': 'no-store',
        'X-Content-Type-Options': 'nosniff', 'Cross-Origin-Resource-Policy': 'same-origin',
        'Content-Security-Policy': "default-src 'none'; script-src 'self' https://extension-files.twitch.tv; style-src 'self'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; object-src 'none'; frame-ancestors 'none'",
        'Referrer-Policy': 'no-referrer'});
    const finish = (code, body, type = 'text/plain; charset=utf-8') => {
      response.writeHead(code, headers(type));
      response.end(body);
    };
    if (request.headers.host !== host) return finish(403, 'Invalid local host');
    if (request.method !== 'GET') return finish(405, 'Only GET is supported');
    let url;
    try { url = new URL(request.url, origin); } catch { return finish(400, 'Invalid URL'); }
    if (url.origin !== origin) return finish(403, 'Invalid request origin');
    const crossSite = request.headers['sec-fetch-site'] && !['same-origin', 'none'].includes(request.headers['sec-fetch-site']);
    const crossOrigin = request.headers.origin && request.headers.origin !== origin;
    const topLevelNavigation = request.headers['sec-fetch-mode'] === 'navigate' &&
      request.headers['sec-fetch-dest'] === 'document';
    // Only the relay popup and the public local viewer may be opened from another site.
    // Their scripts and JSON still require same-origin requests, and neither page can be framed.
    if (url.pathname === '/relay.html') {
      const target = url.searchParams.get('origin'), nonce = url.searchParams.get('nonce');
      if (![EXTENSION_ORIGIN, origin].includes(target) || !UUID.test(nonce || '')) return finish(400, 'Invalid relay destination');
      if ((crossSite || crossOrigin) && !topLevelNavigation) return finish(403, 'Relay requires a top-level navigation');
    } else if (url.pathname === '/video_overlay.html') {
      if ((crossSite || crossOrigin) && !topLevelNavigation) return finish(403, 'Viewer requires a top-level navigation');
    } else if (crossSite || crossOrigin) return finish(403, 'Cross-site access is not supported');
    try {
      if (url.pathname === '/events') {
        // Network events keep the relay current when Chrome throttles timers in hidden windows.
        // The cadence belongs to Node, not to a recursive timer in the browser.
        response.writeHead(200, headers('text/event-stream; charset=utf-8'));
        response.flushHeaders();
        let reading = false;
        const send = async () => {
          if (reading || response.destroyed || response.writableEnded || response.writableNeedDrain) return;
          reading = true;
          try {
            const payload = await readSnapshot(source);
            if (!response.destroyed && !response.writableEnded) response.write(`data: ${JSON.stringify(payload)}\n\n`);
          } catch { response.destroy(); }
          finally { reading = false; }
        };
        const timer = setInterval(send, 1000);
        response.once('close', () => clearInterval(timer));
        void send();
        return;
      }
      if (url.pathname === '/snapshot') {
        return finish(200, JSON.stringify(await readSnapshot(source)), 'application/json; charset=utf-8');
      }
      const name = url.pathname.slice(1);
      const file = name === 'relay.html' || name === 'relay.mjs' ? path.join(directory, name)
        : webAssets.has(name) ? path.join(directory, '..', 'web', name) : null;
      if (!file) return finish(404, 'Not found');
      return finish(200, await readFile(file), types[path.extname(file)]);
    } catch { return finish(500, 'Local bridge request failed'); }
  });
  server.headersTimeout = 10000;
  server.requestTimeout = 10000;
  return server;
}

export async function startBridge({gameDir, pid = null, port = 8765} = {}) {
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new TypeError('Invalid local port');
  const server = createBridgeServer({gameDir, pid});
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => { server.off('error', reject); resolve(); });
  });
  return server;
}

export function parseArguments(args) {
  const options = {};
  for (let i = 0; i < args.length; i++) {
    const flag = args[i];
    if (flag === '--help') return {help: true};
    if (!['--game-dir', '--pid'].includes(flag) || !args[i + 1] || args[i + 1].startsWith('--')) throw new Error(`Unknown or incomplete option: ${flag}`);
    const value = args[++i];
    if (flag === '--game-dir') options.gameDir = value;
    else {
      if (!/^[1-9]\d*$/.test(value) || !validPid(Number(value))) throw new Error('Invalid --pid');
      options.pid = Number(value);
    }
  }
  if (!options.gameDir) throw new Error('Specify --game-dir with the game installation directory');
  return options;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const options = parseArguments(process.argv.slice(2));
    if (options.help) console.log('Usage: node bridge/server.mjs --game-dir "C:\\path\\to\\game" [--pid 1234]');
    else {
      const server = await startBridge(options);
      console.log(`Disciples II bridge: http://127.0.0.1:${server.address().port} (loopback only)`);
      console.log(options.pid ? `Selected game PID: ${options.pid}` : 'Source: the only game with a fresh TwitchStat snapshot');
      for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => {
        server.close(() => process.exit(0));
        server.closeAllConnections();
      });
    }
  } catch (error) {
    console.error(`Cannot start the local bridge: ${error.message}`);
    process.exitCode = 1;
  }
}
