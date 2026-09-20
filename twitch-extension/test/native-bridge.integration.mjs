// Developer-only integration tests of the real Win32 transport. Node is not shipped or needed by players.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createInterface} from 'node:readline';
import {readFile, writeFile, unlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {randomUUID} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import http from 'node:http';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {validateFrame} from '../web/protocol.mjs';

const binary = fileURLToPath(new URL('../../c4ddraw/plugins/unitinfo/tests/bin/localbridge_test_host.exe', import.meta.url));
const origin = 'http://127.0.0.1:8765';
const extension = 'https://pvffxvvhlpi5o8qe3ybjjpwb5hh7n7.ext-twitch.tv';
const relay = `/relay.html?origin=${encodeURIComponent(extension)}&nonce=9c7098aa-8592-49a4-9d75-d69a19daea6a`;
const navigation = {'Sec-Fetch-Site': 'cross-site', 'Sec-Fetch-Mode': 'navigate', 'Sec-Fetch-Dest': 'document'};

function driver(t, plugin = false) {
  const args=plugin?[fileURLToPath(new URL('../../c4ddraw/plugins/unitinfo/bin/Release/twitchstat.c4p',import.meta.url))]:[];
  const child = spawn(binary, args, {windowsHide: true, stdio: ['pipe', 'pipe', 'pipe']});
  const waiting = [];
  createInterface({input: child.stdout}).on('line', line => waiting.shift()?.resolve(JSON.parse(line)));
  child.on('error', error => { for (const wait of waiting.splice(0)) wait.reject(error); });
  child.on('exit', () => { for (const wait of waiting.splice(0)) wait.reject(new Error('Native host exited')); });
  t.after(async () => {
    if (child.exitCode === null) {
      child.stdin.write('quit\n');
      await Promise.race([new Promise(resolve => child.once('exit', resolve)), delay(1500)]);
      if (child.exitCode === null) child.kill();
    }
  });
  return {child, async command(value) {
    const result = new Promise((resolve, reject) => waiting.push({resolve, reject}));
    child.stdin.write(value + '\n');
    return result;
  }};
}
async function waitState(host, expected) {
  for (let i = 0; i < 100; ++i) {
    const state = await host.command('state');
    if (state.state === expected) return state;
    await delay(25);
  }
  assert.fail(`Expected native state ${expected}: ${JSON.stringify(await host.command('state'))}`);
}
function get(path, headers = {}, method = 'GET') {
  return new Promise((resolve, reject) => {
    const request = http.request({host: '127.0.0.1', port: 8765, path, method, headers, agent: false}, response => {
      const chunks = [];
      response.on('data', bytes => chunks.push(bytes));
      response.on('end', () => resolve({status: response.statusCode, headers: response.headers, bytes: Buffer.concat(chunks), body: Buffer.concat(chunks).toString()}));
    });
    request.setTimeout(3000, () => request.destroy(new Error('HTTP timeout')));
    request.on('error', reject); request.end();
  });
}
function raw(request) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(8765, '127.0.0.1', () => socket.end(request));
    let body = '';
    socket.setTimeout(3000, () => socket.destroy(new Error('raw timeout')));
    socket.on('data', data => { body += data; }); socket.on('error', reject);
    socket.on('close', () => resolve(body));
  });
}
function frame(pid, ts = Date.now()) {
  return {schema:'c4dll.twitch-frame', version:1, pid, battle_id:`${pid}-test-1`, ts, captured_at:ts, active:true,
    viewport:{left:0, top:0, width:1366, height:768}, snapshot:{schema:'c4dll.battle-roster', schema_version:1,
      units:[{name:'Воин', stats:'100/100', effects:['Защита']}], slots:Array.from({length:12}, (_,i)=>({
        bounds:{left:i*30,top:10,right:i*30+20,bottom:30},occupied:i===0,unit_index:i===0?0:null}))}};
}
function stream(t) {
  const messages = [], waiting = [];
  let closed = false;
  const request = http.get(`${origin}/events`, {agent:false}, response => {
    assert.equal(response.statusCode, 200);
    assert.match(response.headers['content-type'], /^text\/event-stream/);
    response.setEncoding('utf8');
    let pending = '';
    response.on('data', chunk => {
      pending += chunk;
      let end;
      while ((end = pending.indexOf('\n\n')) >= 0) {
        const event = pending.slice(0,end); pending = pending.slice(end+2);
        if (!event.startsWith('data: ')) continue;
        const message = JSON.parse(event.split('\n').filter(line=>line.startsWith('data: ')).map(line=>line.slice(6)).join('\n'));
        if (waiting.length) waiting.shift()(message); else messages.push(message);
      }
    });
    response.on('close', () => { closed = true; });
    response.on('error', () => { closed = true; });
  });
  request.on('error', () => { closed = true; });
  t.after(() => request.destroy());
  return {request, get closed(){ return closed; }, async next() {
    if (messages.length) return messages.shift();
    return Promise.race([new Promise(resolve => waiting.push(resolve)), delay(2500).then(() => {throw new Error('SSE timeout');})]);
  }};
}

test('embedded Win32 bridge lifecycle, isolation, assets and freshness', {timeout:45000}, async t => {
  // Do not kill or replace a server that the user may be using.
  try { await get('/snapshot'); assert.fail('Port 8765 is occupied; stop the old test bridge first.'); }
  catch (error) { if (error.code !== 'ECONNREFUSED') throw error; }
  const host = driver(t);
  await host.command('enable');
  const started = await waitState(host, 'Listening');
  await t.test('idle needs no file, game directory or JS runtime', async () => {
    const response = await get('/snapshot'); assert.equal(response.status,200);
    const {frame:value,status} = JSON.parse(response.body);
    assert.equal(validateFrame(value).active,false); assert.equal(status.pid,started.pid);
    assert.equal(response.headers['cache-control'],'no-store');
    assert.equal(response.headers['access-control-allow-origin'],undefined);
    assert.match(response.headers['content-security-policy'], /frame-ancestors 'none'/);
  });
  await t.test('all eleven embedded assets match source bytes', async () => {
    const files = ['broadcaster.mjs','config.html','control.css','game-text.mjs','live_config.html',
      'overlay.css','protocol.mjs','video_overlay.html','viewer.mjs','relay.html','relay.mjs'];
    for (const name of files) {
      const source = await readFile(new URL(`../${name.startsWith('relay.')?'bridge':'web'}/${name}`,import.meta.url));
      const response = await get(name==='relay.html'?relay:`/${name}`);
      assert.equal(response.status,200,name); assert.deepEqual(response.bytes,source,name);
    }
  });
  await t.test('only validated top-level relay and preview navigation may cross sites', async () => {
    assert.equal((await get(relay,navigation)).status,200);
    assert.equal((await get('/video_overlay.html?local',navigation)).status,200);
    assert.equal((await get(relay,{'Sec-Fetch-Site':'cross-site'})).status,403);
    assert.equal((await get(relay,{...navigation,'Sec-Fetch-Dest':'iframe'})).status,403);
    assert.equal((await get('/relay.html?origin=https%3A%2F%2Fevil.invalid&nonce=9c7098aa-8592-49a4-9d75-d69a19daea6a',navigation)).status,400);
    assert.equal((await get('/relay.html?origin='+encodeURIComponent(extension)+'&nonce=bad',navigation)).status,400);
  });
  await t.test('reject rebinding, cross-site data and unsupported methods', async () => {
    assert.equal((await get('/snapshot',{Host:'evil.invalid'})).status,403);
    assert.equal((await get('/snapshot',{Origin:'https://evil.invalid'})).status,403);
    assert.equal((await get('/events',{'Sec-Fetch-Site':'cross-site'})).status,403);
    assert.equal((await get('/relay.mjs',{'Sec-Fetch-Site':'cross-site'})).status,403);
    assert.equal((await get('/snapshot',{},'POST')).status,405);
    for (const path of ['/../C4Plugins.ini','/%2e%2e/C4Plugins.ini','/C4Plugins.ini']) assert.notEqual((await get(path)).status,200);
  });
  await t.test('ambiguous headers and request smuggling cannot reach snapshots', async () => {
    for (const headers of ['Host: 127.0.0.1:8765\r\nHost: evil.invalid',
      'Host: 127.0.0.1:8765\r\nTransfer-Encoding: chunked',
      'Host: 127.0.0.1:8765\r\nContent-Length: 3']) {
      const response = await raw(`GET /snapshot HTTP/1.1\r\n${headers}\r\n\r\nabc`);
      assert.doesNotMatch(response,/HTTP\/1\.[01] 200/);
    }
  });
  await t.test('passes an intact complete roster with its publication timestamp', async () => {
    const value=frame(started.pid); await host.command('frame 0 '+JSON.stringify(value));
    const payload=JSON.parse((await get('/snapshot')).body);
    assert.deepEqual(validateFrame(payload.frame),value); assert.equal(payload.status.pid,started.pid);
  });
  await t.test('SSE sends immediately and stays alive while game is idle', async st => {
    await host.command('idle'); const events=stream(st);
    const first=await events.next(); const second=await events.next();
    assert.equal(first.frame.active,false); assert.equal(second.frame.active,false);
    assert.ok(second.frame.ts>first.frame.ts);
    assert.equal(second.status.pid,started.pid);
  });
  await t.test('real serializer formatting survives HTTP and multiline SSE', async st => {
    const value=frame(started.pid);
    value.snapshot.units[0].formatted={stats:'Здоровье:\t100 / 100\nЗащита: 0',effects:['Сопротивление: +5%']};
    const file=path.join(tmpdir(),'twitch-native-fixture-'+randomUUID()+'.json');
    st.after(()=>unlink(file));
    await writeFile(file,JSON.stringify(value,null,2)+'\n');
    await host.command('framefile 0 '+file);
    assert.deepEqual(JSON.parse((await get('/snapshot')).body).frame,value);
    const events=stream(st);
    assert.deepEqual((await events.next()).frame,value);
  });
  await t.test('capture-age TTL rejects recently published stale cards', async () => {
    await host.command('frame 6001 '+JSON.stringify(frame(started.pid)));
    const payload=JSON.parse((await get('/snapshot')).body);
    assert.equal(payload.frame.active,false); assert.equal(payload.status.code,'stale');
  });
  await t.test('oversized updates cannot keep a previous valid roster visible', async () => {
    await host.command('frame 0 '+JSON.stringify(frame(started.pid)));
    await host.command('oversize');
    assert.equal(JSON.parse((await get('/snapshot')).body).frame.active,false);
  });
  await t.test('another game cannot bind or silently take over after owner closes', async st => {
    const other=driver(st); await other.command('enable'); await waitState(other,'PortBusy');
    assert.equal(JSON.parse((await get('/snapshot')).body).status.pid,started.pid);
    await host.command('disable'); await waitState(host,'Stopped'); await delay(150);
    assert.equal((await other.command('state')).state,'PortBusy');
    await assert.rejects(get('/snapshot'),{code:'ECONNREFUSED'});
    await other.command('disable'); await waitState(other,'Stopped');
    await other.command('enable'); const owner=await waitState(other,'Listening');
    assert.equal(JSON.parse((await get('/snapshot')).body).status.pid,owner.pid);
    await other.command('disable'); await waitState(other,'Stopped');
    await host.command('enable'); await waitState(host,'Listening');
  });
  await t.test('rapid disable/enable closes old stream and clears previous roster', async st => {
    await host.command('frame 0 '+JSON.stringify(frame(started.pid)));
    const events=stream(st); assert.equal((await events.next()).frame.active,true);
    await host.command('cycle'); await waitState(host,'Listening');
    for (let i=0;i<40&&!events.closed;i++) await delay(25);
    assert.equal(events.closed,true);
    assert.equal(JSON.parse((await get('/snapshot')).body).frame.active,false);
  });
  await t.test('a slow incomplete request cannot block publishing or other clients', async st => {
    const slow=net.connect(8765,'127.0.0.1'); st.after(()=>slow.destroy());
    await new Promise(resolve=>slow.once('connect',resolve)); slow.write('GET /snapshot HTTP/1.1\r\nHo');
    const timings=[];
    for(let i=0;i<20;i++) timings.push((await host.command('frame 0 '+JSON.stringify(frame(started.pid)))).us);
    assert.ok(Math.max(...timings)<100000,'publisher does not wait on the slow peer');
    assert.equal((await get('/snapshot')).status,200);
  });
  await t.test('disabled bridge disconnects SSE and releases its TCP port', async st => {
    const events=stream(st); await events.next(); await host.command('disable'); await waitState(host,'Stopped');
    for(let i=0;i<40&&!events.closed;i++) await delay(25);
    assert.equal(events.closed,true); await assert.rejects(get('/snapshot'),{code:'ECONNREFUSED'});
  });
  await t.test('actual c4p init, embedded DLL resources, Enabled menu and shutdown', async st => {
    const plugin=driver(st,true);
    assert.equal((await plugin.command('state')).state,'Stopped');
    await plugin.command('enable'); const owner=await waitState(plugin,'Listening');
    const connected=JSON.parse((await get('/snapshot')).body);
    assert.equal(connected.status.pid,owner.pid);
    assert.equal(connected.status.code,'idle','enabled DLL reports connected even before its first battle');
    const expected=await readFile(new URL('../bridge/relay.mjs',import.meta.url));
    assert.deepEqual((await get('/relay.mjs')).bytes,expected);
    await plugin.command('disable');
    for (let i=0;i<50;i++) { try { await get('/snapshot'); await delay(25); } catch(error) {if(error.code==='ECONNREFUSED') break; throw error;} }
    await assert.rejects(get('/snapshot'),{code:'ECONNREFUSED'});
    await plugin.command('enable'); await waitState(plugin,'Listening');
    await plugin.command('quit');
    await delay(100);
    await assert.rejects(get('/snapshot'),{code:'ECONNREFUSED'});
  });
});
