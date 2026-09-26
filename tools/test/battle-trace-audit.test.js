'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { auditLogs } = require('./battle-trace-audit');
const unit = 0xa3e4019d;
function row(seq, fields = {}) {
  return { v: 1, seq, scope: 1, parent: 0, kind: 'resolution', edge: 'enter', pid: 100,
    tid: 200, tick: seq * 10, battle: 4096, map: 8192, attackerGroup: 0xa3e30000,
    defenderGroup: 0xa3e3002a, round: 1, actor: unit, snapshotOk: true, choiceKnown: false,
    action: 0, target: 0, chosenAttacker: 0, queue: Array.from({ length: 13 }, (_, i) => [i ? 0 : unit, i ? 0 : 2]),
    units: [[0, unit, 115, 115, 0, 0, 0]], ...fields };
}
const log = rows => rows.map(r => `[D] [battle-trace] ${JSON.stringify(r)}`).join('\n');
const audit = rows => auditLogs([{ name: 'host.log', text: log(rows) }], { leaderIds: [unit] });
const pair = () => [row(1), row(2, { edge: 'leave' })];
function rejects(rows, code) {
  const result = audit(rows);
  assert.equal(result.complete, false);
  assert.ok(result.errors.some(e => e.code === code), JSON.stringify(result.errors));
}

test('paired resolution, raw queue and caller-supplied leader HP', () => {
  const rows = pair(); rows[1].units[0][2] = 0; rows[1].units[0][3] = 0;
  const result = audit(rows);
  assert.equal(result.complete, true);
  assert.equal(result.battles[0].resolutionBoundaries, 1);
  assert.equal(result.battles[0].leaderHpTimeline.length, 2);
  assert.equal(result.battles[0].zeroHpObserved.filter(x => x.positiveToZero).length, 2);
  assert.match(result.coverage, /not proof/);
});
test('non-trace startup lines ignored; empty requested trace fails', () => {
  assert.equal(auditLogs([{ text: 'ordinary startup\n' + log(pair()) }]).complete, true);
  assert.equal(auditLogs([{ text: 'ordinary startup' }]).complete, false);
  assert.equal(auditLogs([]).complete, false);
});
test('malformed and truncated record are incomplete', () => {
  for (const tail of ['{not json}', '{"v":1,"seq":3']) {
    const r = auditLogs([{ text: log(pair()) + '\n[battle-trace] ' + tail }]);
    assert.equal(r.complete, false);
    assert.ok(r.errors.some(x => x.code === 'malformed-record'));
  }
});
test('missing exit', () => rejects([row(1)], 'missing-exit'));
test('missing beginning, sequence gap and duplicate', () => {
  rejects([row(2, { edge: 'leave' })], 'sequence-gap-or-duplicate');
  rejects([row(1), row(3, { edge: 'leave' })], 'sequence-gap-or-duplicate');
  rejects([row(1), row(1, { edge: 'leave' })], 'sequence-gap-or-duplicate');
});
test('cross-battle pointer and group identity changes rejected within scope', () => {
  rejects([row(1), row(2, { edge: 'leave', battle: 9000 })], 'scope-identity-mismatch');
  rejects([row(1), row(2, { edge: 'leave', defenderGroup: 123 })], 'scope-identity-mismatch');
});
test('nested scopes validate TID, parent and LIFO; two independent threads allowed', () => {
  const rows = [row(1), row(2, { scope: 2, parent: 1, kind: 'unit-before' }),
    row(3, { scope: 2, parent: 1, kind: 'unit-before', edge: 'leave' }), row(4, { edge: 'leave' })];
  assert.equal(audit(rows).complete, true);
  rejects([row(1), row(2, { edge: 'leave', tid: 201 })], 'unmatched-exit');
  rejects([row(1), row(2, { scope: 2, parent: 0 })], 'parent-mismatch');
  assert.equal(audit([row(1), row(2, { scope: 2, tid: 201 }),
    row(3, { scope: 2, tid: 201, edge: 'leave' }), row(4, { edge: 'leave' })]).complete, true);
});
test('concurrent physical log order may differ from atomic sequence', () => {
  assert.equal(audit([row(2, { edge: 'leave' }), row(1)]).complete, true);
});
test('cap, snapshot fault, unwind and invalid raw shape fail closed', () => {
  rejects([...pair(), { v: 1, seq: 3, kind: 'limit', complete: false }], 'record-limit');
  rejects([row(1, { snapshotOk: false }), row(2, { edge: 'leave' })], 'snapshot-fault');
  rejects([row(1), row(2, { edge: 'unwind' })], 'scope-unwind');
  rejects([row(1, { queue: [] }), row(2, { edge: 'leave' })], 'snapshot-shape');
});
test('AI choices without damage are not falsely called skipped hits (miss/summon may be unobserved)', () => {
  for (const action of [0, 7]) {
    const rows = [row(1), row(2, { scope: 2, parent: 1, kind: 'ai-choice' }),
      row(3, { scope: 2, parent: 1, kind: 'ai-choice', edge: 'leave', choiceKnown: true,
        action, target: 0xa3e400aa, chosenAttacker: unit }), row(4, { edge: 'leave' })];
    const result = audit(rows);
    assert.equal(result.complete, true);
    assert.equal(result.battles[0].damageCount, 0);
    assert.equal(result.battles[0].chosenActions[0].action, action);
  }
});
test('AI choice requires an observed output and valid enum', () => {
  rejects([row(1, { kind: 'ai-choice' }), row(2, { kind: 'ai-choice', edge: 'leave' })], 'missing-ai-choice');
});
test('damage must have an active same-battle scope', () => {
  const damage = row(2, { kind: 'damage-hit', edge: 'observed', hpBefore: 115, hpAfter: 15,
    normal: 100, critical: 0, total: 100, target: unit });
  assert.equal(audit([row(1), damage, row(3, { edge: 'leave' })]).complete, true);
  rejects([row(1), { ...damage, scope: 0 }, row(3, { edge: 'leave' })], 'unscoped-damage');
});
test('unknown extra fields retained; unknown schema/kind incomplete', () => {
  const rows = pair(); rows[0].futureDetail = { observed: 'retained' };
  assert.deepEqual(audit(rows).battles[0].extensions[0].fields.futureDetail, rows[0].futureDetail);
  rejects([row(1, { v: 2 })], 'unsupported-version');
  rejects([row(1, { kind: 'new-hook' })], 'unsupported-kind');
});
test('separate owned host/join logs and pointer reuse across different completed battles', () => {
  const join = pair().map(r => ({ ...r, pid: 101 }));
  assert.equal(auditLogs([{ text: log(pair()) }, { text: log(join) }]).complete, true);
  const later = [row(3, { scope: 2, defenderGroup: 555 }), row(4, { scope: 2, defenderGroup: 555, edge: 'leave' })];
  assert.equal(audit([...pair(), ...later]).battles.length, 2);
});
test('supplemental client with no battle events allowed, but all-empty logs fail', () => {
  assert.equal(auditLogs([{ text: log(pair()) }, { text: 'ordinary join startup' }]).complete, true);
  assert.equal(auditLogs([{ text: 'host' }, { text: 'join' }]).complete, false);
});
test('round map resolution allowed and recorded; unrelated map replacement rejected', () => {
  const result = audit([row(1, { kind: 'round-before', map: 0 }), row(2, { kind: 'round-before', edge: 'leave' })]);
  assert.equal(result.complete, true);
  assert.equal(result.battles[0].scopeTransitions[0].mapBefore, 0);
  rejects([row(1), row(2, { edge: 'leave', map: 9000 })], 'map-identity-mismatch');
});
test('explicit cross-battle nesting retained without inventing a serialization guarantee', () => {
  const inner = { scope: 2, parent: 1, battle: 9000, defenderGroup: 556 };
  const result = audit([row(1), row(2, inner), row(3, { ...inner, edge: 'leave' }), row(4, { edge: 'leave' })]);
  assert.equal(result.complete, true);
  assert.equal(result.battles.length, 2);
});

