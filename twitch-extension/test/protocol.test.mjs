import test from 'node:test';
import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';
import {
  MAX_MESSAGE_BYTES, MAX_FRAME_BYTES, MAX_PARTS, MAX_ASSEMBLY_MS,
  MAX_FORMATTED_TEXT_CHARS, MAX_FORMATTED_EFFECT_CHARS, validateFrame, portraitBoxes,
  encodeFrame, FrameAssembler, FrameTimeline, BroadcastQueue
} from '../web/protocol.mjs';

function noise(length) {
  let state = 123456789;
  return Array.from({length}, () => {
    state ^= state << 13; state ^= state >>> 17; state ^= state << 5;
    return String.fromCharCode(33 + ((state >>> 0) % 90));
  }).join('');
}

function frame({ts = 1000, active = true, description = ''} = {}) {
  return {
    schema: 'c4dll.twitch-frame', version: 1, battle_id: '123-456-1', ts, active,
    viewport: {left: 0, top: 0, width: 800, height: 600},
    snapshot: active ? {
      schema: 'c4dll.battle-roster', schema_version: 1,
      units: [{name: 'Архангел', description, stats: 'Здоровье: 120 / 150', effects: ['Сила +20%', 'Яд: 2 хода']}],
      slots: Array.from({length: 12}, (_, i) => ({id: String(i), occupied: i === 0, unit_index: i === 0 ? 0 : null,
        bounds: {left: (i % 6) * 100, top: Math.floor(i / 6) * 200, right: (i % 6) * 100 + 80, bottom: Math.floor(i / 6) * 200 + 150}}))
    } : null
  };
}

async function assemble(messages, start = 0) {
  const assembler = new FrameAssembler();
  let result;
  for (const [i, message] of messages.entries()) result = await assembler.receive(message, start + i * 1100) ?? result;
  return result;
}

test('UTF-8 stats and effects survive full-frame compression and assembly', async () => {
  const original = frame();
  const encoded = await encodeFrame(original, 'roundtrip');
  const result = await assemble(encoded, 250);
  assert.deepEqual(result.frame, original);
  assert.equal(result.received, 250);
});

test('optional native formatting preserves old plaintext cards and partial markup without rewriting it', async () => {
  const original = frame();
  assert.equal(validateFrame(original), original);
  original.snapshot.units[0].formatted = null;
  assert.equal(validateFrame(original), original);
  const marked = {stats: '\\fmed\\c000;090;000;Здоровье:\t65 / 65', leader: null,
    effects: ['\\c000;090;000;Сила +20%', 'Яд: 2 хода']};
  original.snapshot.units[0].formatted = marked;
  assert.equal(validateFrame(original).snapshot.units[0].formatted, marked);
  const complete = await assemble(await encodeFrame(original));
  assert.deepEqual(complete.frame.snapshot.units[0].formatted, marked);
  assert.equal(complete.frame.snapshot.units[0].stats, 'Здоровье: 120 / 150');
  original.snapshot.units[0].formatted = {description: 'я'.repeat(MAX_FORMATTED_TEXT_CHARS), effects: null};
  assert.equal(validateFrame(original), original);
});

test('optional native formatting rejects wrong types and excessive marked field or effect sizes', () => {
  for (const formatted of [false, 42, 'markup', [], {stats: 7}, {description: {}},
    {stats: 'x'.repeat(MAX_FORMATTED_TEXT_CHARS + 1)}, {effects: 'text'}, {effects: [null]},
    {effects: Array(129).fill('effect')}, {effects: ['x'.repeat(MAX_FORMATTED_EFFECT_CHARS + 1)]}]) {
    const original = frame(); original.snapshot.units[0].formatted = formatted;
    assert.throws(() => validateFrame(original), /форматирование/);
  }
});

test('fragmented snapshots fit Twitch, tolerate duplicate and out-of-order fragments, and publish only complete data', async () => {
  const original = frame({description: noise(15000)});
  const messages = await encodeFrame(original, 'fragmented');
  assert.ok(messages.length > 2 && messages.length <= MAX_PARTS);
  assert.ok(messages.every(message => Buffer.byteLength(message) <= MAX_MESSAGE_BYTES));
  const assembler = new FrameAssembler();
  const reversed = [...messages].reverse();
  assert.equal(await assembler.receive(reversed[0], 100), null);
  assert.equal(await assembler.receive(reversed[0], 200), null);
  for (let i = 1; i < reversed.length - 1; i++) assert.equal(await assembler.receive(reversed[i], 300 + i), null);
  const result = await assembler.receive(reversed.at(-1), 1000);
  assert.deepEqual(result.frame, original);
  assert.equal(result.received, 100);
  assert.equal(await assembler.receive(messages[0], 1100), null);
});

test('malformed envelopes, invalid base64 and invalid gzip are ignored without throwing', async () => {
  const assembler = new FrameAssembler();
  const packet = {d2: 1, id: 'bad', i: 0, n: 1, data: 'eA=='};
  for (const message of [null, '{', 'x'.repeat(MAX_MESSAGE_BYTES + 1),
    JSON.stringify({...packet, n: MAX_PARTS + 1}), JSON.stringify({...packet, id: ''}),
    JSON.stringify({...packet, i: -1}), JSON.stringify({...packet, id: 'padding', data: '=A='}),
    JSON.stringify(packet)]) assert.equal(await assembler.receive(message), null);
});

test('conflicting or expired fragments cannot produce a partial snapshot', async () => {
  const messages = await encodeFrame(frame({description: noise(10000)}), 'conflict');
  const assembler = new FrameAssembler();
  assert.equal(await assembler.receive(messages[0], 0), null);
  const conflict = JSON.parse(messages[0]); conflict.data = 'eA==';
  assert.equal(await assembler.receive(JSON.stringify(conflict), 100), null);
  for (let i = 1; i < messages.length; i++) assert.equal(await assembler.receive(messages[i], 200 + i), null);
  const expired = new FrameAssembler();
  await expired.receive(messages[0], 0);
  for (let i = 1; i < messages.length; i++) assert.equal(await expired.receive(messages[i], MAX_ASSEMBLY_MS + 1 + i), null);
});

test('decompressed size, outgoing raw size and fragment-count limits are enforced', async () => {
  const oversized = frame({description: 'a'.repeat(MAX_FRAME_BYTES)});
  await assert.rejects(encodeFrame(oversized), /Слишком большой/);
  await assert.rejects(encodeFrame(frame({description: noise(70000)})), /лимит передачи/);
  await assert.rejects(encodeFrame(frame(), 'x'.repeat(81)), /идентификатор/);
  const data = gzipSync(JSON.stringify(oversized)).toString('base64');
  assert.ok(data.length < 4000, 'compressed oversized fixture fits one network packet');
  assert.equal(await new FrameAssembler().receive(JSON.stringify({d2: 1, id: 'bomb', i: 0, n: 1, data})), null);
});

test('incomplete incoming-frame storage stays bounded', async () => {
  const first = JSON.parse((await encodeFrame(frame({description: noise(10000)})))[0]);
  const assembler = new FrameAssembler();
  for (let i = 0; i < 10; i++) await assembler.receive(JSON.stringify({...first, id: `pending-${i}`}), i);
  assert.equal(assembler.pending.size, 4);
});

test('large units share one hit area, clipped to the actual visible game rectangle', () => {
  const original = frame();
  original.viewport = {left: 50, top: 50, width: 400, height: 200};
  Object.assign(original.snapshot.slots[0], {bounds: {left: 0, top: 0, right: 150, bottom: 100}});
  Object.assign(original.snapshot.slots[1], {occupied: true, unit_index: 0, bounds: {left: 150, top: 0, right: 250, bottom: 200}});
  assert.deepEqual(portraitBoxes(original), [{unit: 0, left: 0, top: 0, right: 0.5, bottom: 0.75}]);
  original.viewport = {left: 1000, top: 1000, width: 400, height: 200};
  assert.deepEqual(portraitBoxes(original), []);
  assert.deepEqual(portraitBoxes(frame({active: false})), []);
});

test('unknown schemas, invalid unit references and non-finite geometry are rejected', () => {
  for (const mutate of [
    value => { value.version = 2; },
    value => { value.ts = NaN; },
    value => { value.viewport.width = 0; },
    value => { value.snapshot.units[0].effects = [42]; },
    value => { value.snapshot.slots.pop(); },
    value => { value.snapshot.slots[0].unit_index = 12; },
    value => { value.snapshot.slots[0].bounds.right = Infinity; },
    value => { value.snapshot.slots[0].bounds.right = -1; }
  ]) {
    const original = frame(); mutate(original);
    assert.throws(() => validateFrame(original));
  }
});

test('broadcast queue enforces 1100 ms spacing and retains failed sends for retry', async () => {
  const queue = new BroadcastQueue(), sent = [];
  const send = message => sent.push(message);
  queue.offer(frame());
  assert.equal(await queue.tick(send, () => 0), true);
  queue.offer(frame({ts: 2000}));
  assert.equal(await queue.tick(send, () => 1099), false);
  await assert.rejects(queue.tick(() => { throw new Error('offline'); }, () => 1100), /offline/);
  assert.equal(await queue.tick(send, () => 1100), false, 'failed API attempts also consume the rate interval');
  assert.equal(await queue.tick(send, () => 2199), false);
  assert.equal(await queue.tick(send, () => 2200), true);
  assert.equal(await queue.tick(send, () => 3300), false);
  assert.equal(sent.length, 2);
  assert.equal((await assemble([sent[1]])).frame.ts, 2000);
});

test('new snapshots coalesce without starving an in-progress fragmented snapshot', async () => {
  const queue = new BroadcastQueue(), sent = [];
  const original = frame({description: noise(15000)}), replacement = frame({ts: 3000});
  const partCount = (await encodeFrame(original)).length;
  queue.offer(original);
  await queue.tick(message => sent.push(message), () => 0);
  queue.offer(frame({ts: 2000})); queue.offer(replacement);
  for (let i = 1; i < partCount; i++) await queue.tick(message => sent.push(message), () => i * 1100);
  assert.deepEqual((await assemble(sent)).frame, original);
  const next = [];
  await queue.tick(message => next.push(message), () => partCount * 1100);
  assert.deepEqual((await assemble(next)).frame, replacement);
});

test('battle end interrupts queued active fragments and concurrent encodes', async () => {
  const queue = new BroadcastQueue(), sent = [];
  queue.offer(frame({description: noise(15000)}));
  await queue.tick(message => sent.push(message), () => 0);
  const end = frame({ts: 2000, active: false}); queue.offer(end);
  await queue.tick(message => sent.push(message), () => 1100);
  assert.deepEqual((await assemble(sent)).frame, end);
  assert.equal(queue.parts.length, 0);
  const concurrent = new BroadcastQueue(); concurrent.offer(frame());
  const encoding = concurrent.tick(message => assert.fail(`unexpected active send: ${message}`), () => 0);
  assert.equal(await concurrent.tick(() => assert.fail('concurrent send'), () => 0), false);
  concurrent.offer(end);
  assert.equal(await encoding, false);
  const ended = [];
  assert.equal(await concurrent.tick(message => ended.push(message), () => 0), true);
  assert.deepEqual((await assemble(ended)).frame, end);
});

test('timeline applies video latency consistently when Twitch changes its estimate', () => {
  const timeline = new FrameTimeline(), first = frame(), next = frame({ts: 2000});
  timeline.enqueue(first, 1000); timeline.enqueue(next, 2000);
  assert.equal(timeline.take(5999, 5000), undefined);
  assert.equal(timeline.take(6000, 5000), first);
  assert.equal(timeline.take(6100, 3000), next);
  assert.equal(timeline.take(6101, 3000), undefined);
});

test('late decoding or stale retransmission cannot resurrect a finished battle', () => {
  const timeline = new FrameTimeline(), end = frame({ts: 3000, active: false});
  assert.equal(timeline.enqueue(end, 300), true);
  assert.equal(timeline.enqueue(frame({ts: 2000}), 100), false);
  assert.equal(timeline.enqueue(frame({ts: 2000}), 400), false);
  assert.equal(timeline.take(5300, 5000), end);
  assert.equal(timeline.take(10000, 5000), undefined);
});

test('a new publication after inactive is accepted even when its sliced capture started earlier', async () => {
  const timeline = new FrameTimeline(), inactive = frame({ts: 2500, active: false});
  assert.equal(timeline.enqueue(inactive, 2500), true);
  assert.equal(timeline.take(2500), inactive);
  const completed = {...frame({ts: 2700}), captured_at: 1000};
  const transported = (await assemble(await encodeFrame(completed, 'sliced-capture'), 2700)).frame;
  assert.deepEqual(transported, completed, 'optional captured_at survives the unchanged protocol');
  assert.equal(timeline.enqueue(transported, 2700), true);
  assert.deepEqual(timeline.take(2700), completed);
  const oldDelayed = {...frame({ts: 2400}), captured_at: 900};
  assert.equal(timeline.enqueue(oldDelayed, 2800), false, 'late arrival cannot replace a newer publication');
  assert.equal(timeline.take(2800), undefined);
  assert.deepEqual(timeline.current, completed);
});

test('timeline removes abandoned battle frames after grace period and bounds its delay buffer', () => {
  const timeline = new FrameTimeline(); timeline.enqueue(frame(), 1000);
  assert.equal(timeline.take(6000, 5000).active, true);
  assert.equal(timeline.take(26000, 5000), undefined);
  assert.equal(timeline.take(26001, 5000), null);
  assert.equal(timeline.take(26002, 5000), undefined);
  for (let i = 0; i < 200; i++) timeline.enqueue(frame({ts: 2000 + i}), 27000 + i);
  assert.equal(timeline.pending.length, 100);
  assert.equal(timeline.take(28000).ts, 2199);
  const suspended = new FrameTimeline(); suspended.enqueue(frame(), 0);
  assert.equal(suspended.take(30000), null, 'a resumed browser must not flash stale buffered cards');
});
