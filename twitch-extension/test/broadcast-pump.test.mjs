import test from 'node:test';
import assert from 'node:assert/strict';
import {BroadcastPump, BroadcastQueue, encodeFrame, FrameAssembler, FrameTimeline,
  MAX_MESSAGE_BYTES, MIN_SEND_INTERVAL_MS} from '../web/protocol.mjs';

function frame(ts = 1000, description = '', active = true) {
  return {schema: 'c4dll.twitch-frame', version: 1, battle_id: 'background-test', ts, active,
    viewport: {left: 0, top: 0, width: 800, height: 600},
    snapshot: active ? {schema: 'c4dll.battle-roster', schema_version: 1,
      units: [{name: 'Воин', description, effects: []}],
      slots: Array.from({length: 12}, (_, i) => ({occupied: i === 0, unit_index: i === 0 ? 0 : null,
        bounds: {left: i * 50, top: 0, right: i * 50 + 40, bottom: 50}}))} : null};
}

function harness() {
  let time = 0, timerId = 0;
  const timers = new Map(), sent = [], errors = [], queue = new BroadcastQueue();
  const pump = new BroadcastPump(queue, message => sent.push({message, time}), {
    now: () => time,
    setTimer: (callback, delay) => { const id = ++timerId; timers.set(id, {callback, at: time + delay}); return id; },
    clearTimer: id => timers.delete(id), onError: error => errors.push(error)
  });
  return {queue, pump, sent, timers, errors, setTime(value) { time = value; },
    async event(snapshot, at) { time = at; queue.offer(snapshot); await pump.wake(); },
    async fireFirstTimer() {
      const [id, timer] = timers.entries().next().value;
      timers.delete(id); time = Math.max(time, timer.at); await timer.callback();
    }};
}

test('network-driven events keep sending every two seconds even if hidden-page timers never run', async () => {
  const h = harness();
  for (let i = 0; i <= 6; i++) await h.event(frame(1000 + i * 1000), i * 1000);
  assert.deepEqual(h.sent.map(value => value.time), [0, 2000, 4000, 6000]);
  assert.ok(h.sent.every(value => Buffer.byteLength(value.message) <= MAX_MESSAGE_BYTES));
  for (let i = 1; i < h.sent.length; i++) assert.ok(h.sent[i].time - h.sent[i - 1].time >= MIN_SEND_INTERVAL_MS);
  assert.equal(h.errors.length, 0);
});

test('one follow-up timeout fills the remaining rate gap without creating a timer chain', async () => {
  const h = harness();
  h.queue.parts = ['part-1', 'part-2', 'part-3'];
  await h.pump.wake();
  assert.equal(h.sent.length, 1);
  assert.equal(h.timers.size, 1);
  await h.fireFirstTimer();
  assert.deepEqual(h.sent.map(value => value.time), [0, 1100]);
  assert.equal(h.timers.size, 0, 'the timer callback must not schedule another timer');
  assert.deepEqual(h.queue.parts, ['part-3']);
  h.setTime(2200); await h.pump.wake();
  assert.deepEqual(h.sent.map(value => value.time), [0, 1100, 2200]);
});

test('concurrent wakes during compression retain battle end and do not duplicate sends', async () => {
  const h = harness();
  h.queue.offer(frame());
  const first = h.pump.wake(), second = h.pump.wake();
  h.queue.offer(frame(2000, '', false));
  const ended = h.pump.wake();
  await Promise.all([first, second, ended]);
  assert.equal(h.sent.length, 1);
  const result = await new FrameAssembler().receive(h.sent[0].message, 0);
  assert.equal(result.frame.active, false);
  assert.equal(h.timers.size, 0);
  assert.equal(h.errors.length, 0);
});

test('a failed send stays queued and retries on the next external wake', async () => {
  const h = harness(); let fail = true;
  const send = h.pump.send;
  h.pump.send = message => { if (fail) throw new Error('temporary failure'); send(message); };
  await h.event(frame(), 0);
  assert.equal(h.errors.length, 1);
  assert.equal(h.sent.length, 0);
  assert.equal(h.queue.parts.length, 1);
  fail = false; h.setTime(1000); await h.pump.wake();
  assert.equal(h.sent.length, 0, 'an error must not bypass the attempt rate limit');
  h.setTime(1100); await h.pump.wake();
  assert.equal(h.sent.length, 1);
  assert.equal(h.queue.parts.length, 0);
});

test('a suspended encode samples actual send time and cannot burst when relay events resume', async () => {
  const h = harness();
  const encoding = h.event(frame(1000), 0);
  // The first tick is now awaiting real asynchronous gzip. Simulate Chrome
  // resuming its Promise and delivering queued messages 28 seconds later.
  h.setTime(28000); h.queue.offer(frame(2000));
  const batchedWake = h.pump.wake();
  await Promise.all([encoding, batchedWake]);
  assert.deepEqual(h.sent.map(value => value.time), [28000]);
  assert.equal(h.queue.lastSent, 28000);
  await h.event(frame(3000), 28005);
  assert.equal(h.sent.length, 1, 'the observed five-millisecond live burst must be blocked');
  h.setTime(29099); await h.pump.wake();
  assert.equal(h.sent.length, 1);
  h.setTime(29100); await h.pump.wake();
  assert.deepEqual(h.sent.map(value => value.time), [28000, 29100]);
  const complete = await new FrameAssembler().receive(h.sent.at(-1).message, 29100);
  assert.equal(complete.frame.ts, 3000, 'queued relay snapshots still coalesce to the newest one');
});

test('a blocking or failing API call reserves the interval through its actual return time', async () => {
  const queue = new BroadcastQueue(); queue.parts = ['first', 'second'];
  let time = 0;
  await assert.rejects(queue.tick(() => { time = 28000; throw new Error('blocked failure'); }, () => time), /blocked failure/);
  assert.equal(queue.lastSent, 28000);
  assert.deepEqual(queue.parts, ['first', 'second'], 'failed attempts keep the unsent fragment');
  time = 28005;
  assert.equal(await queue.tick(() => assert.fail('early retry'), () => time), false);
  time = 29100;
  assert.equal(await queue.tick(() => { time = 40000; }, () => time), true);
  assert.equal(queue.lastSent, 40000, 'successful blocking calls also update the completion clock');
  time = 40005;
  assert.equal(await queue.tick(() => assert.fail('early next fragment'), () => time), false);
  time = 41100;
  assert.equal(await queue.tick(() => {}, () => time), true);
});

function noise(length) {
  let state = 123456789;
  return Array.from({length}, () => {
    state ^= state << 13; state ^= state >>> 17; state ^= state << 5;
    return String.fromCharCode(33 + ((state >>> 0) % 90));
  }).join('');
}

test('all twelve fragments complete with delayed timers while new snapshots coalesce', async () => {
  let original;
  for (const size of [40000, 41000, 42000]) {
    const candidate = frame(1000, noise(size));
    if ((await encodeFrame(candidate)).length === 12) { original = candidate; break; }
  }
  assert.ok(original, 'fixture must exercise the maximum supported fragment count');
  const h = harness(); await h.event(original, 0);
  for (let i = 1; i <= 22; i++) await h.event(frame(1000 + i * 1000), i * 1000);
  assert.equal(h.sent.length, 12);
  const assembler = new FrameAssembler(); let complete;
  for (const value of h.sent) complete = await assembler.receive(value.message, value.time) ?? complete;
  assert.deepEqual(complete.frame, original);
  assert.equal(complete.received, 0);
  assert.equal(complete.completedReceived, 22000);
  const timeline = new FrameTimeline();
  timeline.enqueue(complete.frame, complete.received, complete.completedReceived);
  assert.deepEqual(timeline.take(22000), original, 'freshly assembled data must not expire from first-fragment age');
  assert.equal(timeline.take(42001), null, 'stale-state timeout still applies after completed reception');
  h.setTime(24000); await h.pump.wake();
  const next = await assembler.receive(h.sent.at(-1).message, 24000);
  assert.equal(next.frame.ts, 23000, 'only the latest waiting snapshot follows the complete large frame');
});
