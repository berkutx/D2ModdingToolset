'use strict';

// Structural audit of optional native observations, never a gameplay-pass oracle.
const fs = require('node:fs');
const kinds = new Set(['resolution', 'finalize-check', 'round-before', 'ai-choice',
  'unit-before', 'unit-after', 'viewer-end']);
const known = new Set(['v', 'seq', 'scope', 'parent', 'kind', 'edge', 'pid', 'tid', 'tick',
  'battle', 'map', 'attackerGroup', 'defenderGroup', 'round', 'actor', 'snapshotOk',
  'choiceKnown', 'action', 'target', 'chosenAttacker', 'queue', 'units',
  'hpBefore', 'hpAfter', 'normal', 'critical', 'total']);
const integer = (v, min, max) => Number.isSafeInteger(v) && v >= min && v <= max;
const u32 = v => integer(v, 0, 0xffffffff);
const i32 = v => integer(v, -0x80000000, 0x7fffffff);
const identity = r => `${r.pid}:${r.battle}:${r.attackerGroup}:${r.defenderGroup}`;

function auditLogs(logs, options = {}) {
  const errors = [], files = [], battles = new Map(), seenPids = new Set();
  const leaders = new Set(options.leaderIds || []);
  let totalMarkers = 0;
  const issue = (file, line, code, detail) => errors.push({ file, line, code, detail });
  for (const log of logs) {
    const file = log.name || '<memory>', records = [];
    let markerCount = 0;
    for (const [index, line] of String(log.text).split(/\r?\n/).entries()) {
      const at = line.indexOf('[battle-trace]');
      if (at < 0) continue;
      markerCount++;
      try {
        const r = JSON.parse(line.slice(at + '[battle-trace]'.length).trim());
        if (!r || typeof r !== 'object' || Array.isArray(r)) throw new Error('not an object');
        records.push({ r, line: index + 1 });
      } catch (e) {
        issue(file, index + 1, 'malformed-record', e.message);
      }
    }
    totalMarkers += markerCount;
    // A supplemental client log may have no authoritative battle hooks at all.
    if (!markerCount) {
      files.push({ file, pid: null, markerCount: 0, recordCount: 0 });
      continue;
    }
    const pids = new Set(records.filter(x => x.r.kind !== 'limit').map(x => x.r.pid));
    if (pids.size !== 1) issue(file, 0, 'pid-set', 'Expected exactly one owned process per complete log');
    const pid = [...pids][0];
    if (seenPids.has(pid)) issue(file, 0, 'duplicate-process-log', 'Supply one complete log per PID');
    seenPids.add(pid);
    const stacks = new Map(), scopes = new Map(), closedScopes = new Set();
    let expectedSeq = 1;
    // Native atomic seq allocation precedes logging; concurrent TIDs may write out of order.
    records.sort((a, b) => a.r.seq - b.r.seq);
    for (const { r, line } of records) {
      const fail = (code, detail) => issue(file, line, code, detail);
      if (r.v !== 1) { fail('unsupported-version', `Version ${r.v}`); continue; }
      if (!integer(r.seq, 1, 0xffffffff)) { fail('bad-sequence', 'Invalid sequence'); continue; }
      if (r.seq !== expectedSeq) fail('sequence-gap-or-duplicate', `Expected ${expectedSeq}, got ${r.seq}`);
      expectedSeq = r.seq + 1;
      if (r.kind === 'limit') { fail('record-limit', 'Native trace explicitly capped/incomplete'); continue; }
      if (r.seq > 8192) fail('record-limit', 'Record exceeds native v1 limit');
      const fields = ['pid', 'tid', 'battle', 'attackerGroup', 'defenderGroup'];
      let shapeOk = true;
      for (const key of fields) {
        if (!integer(r[key], 1, 0xffffffff)) { fail('field-shape', key); shapeOk = false; }
      }
      for (const key of ['scope', 'parent', 'tick', 'map', 'actor', 'target', 'chosenAttacker']) {
        if (!u32(r[key])) { fail('field-shape', key); shapeOk = false; }
      }
      if (!integer(r.round, -128, 127) || !i32(r.action) ||
          typeof r.snapshotOk !== 'boolean' || typeof r.choiceKnown !== 'boolean') {
        fail('field-shape', 'round/action/snapshotOk/choiceKnown'); shapeOk = false;
      }
      if (r.snapshotOk !== true) fail('snapshot-fault', 'Raw native snapshot is incomplete');
      const queueOk = Array.isArray(r.queue) && r.queue.length === 13 &&
        r.queue.every(q => Array.isArray(q) && q.length === 2 && u32(q[0]) && integer(q[1], -128, 127));
      const unitSlots = new Set();
      const unitsOk = Array.isArray(r.units) && r.units.length <= 22 && r.units.every(u => {
        if (!Array.isArray(u) || u.length !== 7 || !integer(u[0], 0, 21) || !u32(u[1]) ||
            !integer(u[2], 0, 65535) || !(u[3] === null || i32(u[3])) ||
            !integer(u[4], 0, 255) || !integer(u[5], -32768, 32767) ||
            !integer(u[6], 0, Number.MAX_SAFE_INTEGER) || unitSlots.has(u[0])) return false;
        unitSlots.add(u[0]); return true;
      });
      if (!queueOk || !unitsOk) { fail('snapshot-shape', 'Invalid queue/units raw tuple shape'); shapeOk = false; }
      if (!shapeOk) continue;
      const tid = r.tid, stack = stacks.get(tid) || [];
      stacks.set(tid, stack);
      const top = stack[stack.length - 1];
      if (kinds.has(r.kind)) {
        if (!r.scope) fail('missing-scope-id', r.kind);
        if (r.edge === 'enter') {
          if (scopes.has(r.scope)) fail('scope-reuse', String(r.scope));
          if (r.parent !== (top ? top.scope : 0)) fail('parent-mismatch', 'Parent is not the active scope on this TID');
          scopes.set(r.scope, r); stack.push(r);
          if (r.choiceKnown) fail('premature-choice', 'Output parameters cannot be known at entry');
        } else if (r.edge === 'leave' || r.edge === 'unwind') {
          const entry = scopes.get(r.scope);
          if (!entry || !top || top.scope !== r.scope) fail('unmatched-exit', `Scope ${r.scope} is not this TID stack top`);
          if (entry && (entry.kind !== r.kind || entry.tid !== tid || entry.parent !== r.parent ||
                        identity(entry) !== identity(r))) fail('scope-identity-mismatch', `Scope ${r.scope}`);
          if (entry && entry.map !== r.map && r.kind !== 'round-before')
            fail('map-identity-mismatch', 'Only round-before explicitly resolves/sets its map after entry');
          if (top && top.scope === r.scope) stack.pop();
          if (closedScopes.has(r.scope)) fail('duplicate-exit', String(r.scope));
          if (entry) closedScopes.add(r.scope);
          if (r.edge === 'unwind') fail('scope-unwind', 'Hook did not return normally');
          if (r.kind === 'ai-choice' && r.edge === 'leave' &&
              (!r.choiceKnown || !integer(r.action, 0, 7))) fail('missing-ai-choice', 'No valid selected action at return');
        } else fail('bad-edge', r.edge);
      } else if (r.kind === 'damage-hit') {
        if (r.edge !== 'observed' || r.parent !== 0 || !r.scope || !top ||
            top.scope !== r.scope || identity(top) !== identity(r)) {
          fail('unscoped-damage', 'Damage observation lacks a matching active native boundary');
        }
        if (!['hpBefore', 'hpAfter', 'normal', 'critical', 'total'].every(k => i32(r[k])))
          fail('damage-shape', 'Invalid observed damage values');
      } else { fail('unsupported-kind', r.kind); continue; }

      const key = identity(r);
      if (!battles.has(key)) battles.set(key, { pid, battle: r.battle,
        attackerGroup: r.attackerGroup, defenderGroup: r.defenderGroup,
        tids: new Set(), recordCount: 0, resolutionBoundaries: 0, damageCount: 0,
        chosenActions: [], scopeTransitions: [], damageObservations: [], hpTimeline: [],
        zeroHpObserved: [], extensions: [], lastHp: new Map() });
      const b = battles.get(key);
      b.tids.add(tid); b.recordCount++;
      if (r.kind === 'resolution' && r.edge === 'leave') b.resolutionBoundaries++;
      if (r.kind === 'damage-hit') b.damageCount++;
      const stamp = { seq: r.seq, scope: r.scope, tid, tick: r.tick, round: r.round, kind: r.kind, edge: r.edge };
      if (kinds.has(r.kind) && r.edge === 'leave') {
        const entry = scopes.get(r.scope);
        if (entry) b.scopeTransitions.push({ ...stamp, enterSeq: entry.seq,
          actorBefore: entry.actor, actorAfter: r.actor, mapBefore: entry.map, mapAfter: r.map,
          queueBefore: entry.queue, queueAfter: r.queue });
      }
      if (r.kind === 'damage-hit') b.damageObservations.push({ ...stamp, actor: r.actor,
        target: r.target, hpBefore: r.hpBefore, hpAfter: r.hpAfter,
        normal: r.normal, critical: r.critical, total: r.total });
      if (r.kind === 'ai-choice' && r.edge === 'leave')
        b.chosenActions.push({ ...stamp, actor: r.actor, action: r.action, target: r.target, attacker: r.chosenAttacker });
      for (const u of r.units) {
        const hpKey = `${u[0]}:${u[1]}`;
        const prev = b.lastHp.get(hpKey);
        if (!prev || prev[0] !== u[2] || prev[1] !== u[3]) {
          b.hpTimeline.push({ ...stamp, slot: u[0], unitId: u[1], battleHp: u[2], mapHp: u[3] });
          for (const [source, value, old] of [['battle', u[2], prev?.[0]], ['map', u[3], prev?.[1]]]) {
            if (value === 0 && old !== 0) b.zeroHpObserved.push({ ...stamp, slot: u[0], unitId: u[1], source,
              positiveToZero: typeof old === 'number' && old > 0 });
          }
          b.lastHp.set(hpKey, [u[2], u[3]]);
        }
      }
      const extra = Object.fromEntries(Object.entries(r).filter(([k]) => !known.has(k)));
      if (Object.keys(extra).length) b.extensions.push({ seq: r.seq, fields: extra });
    }
    for (const entry of scopes.values()) {
      if (!closedScopes.has(entry.scope)) issue(file, 0, 'missing-exit', `Scope ${entry.scope} (${entry.kind})`);
    }
    files.push({ file, pid: pid ?? null, markerCount, recordCount: records.length });
  }
  if (!logs.length) issue('<input>', 0, 'missing-logs', 'Supply at least one log');
  if (!totalMarkers) issue('<input>', 0, 'missing-trace', 'No battle-trace records in any supplied log');
  const output = [...battles.values()].map(({ lastHp, tids, ...b }) => ({ ...b, tids: [...tids],
    leaderHpTimeline: b.hpTimeline.filter(x => leaders.has(x.unitId)) }));
  return { schema: 'battle-trace-audit/v1', complete: errors.length === 0, errors, files,
    coverage: 'Paired hook observations only; not proof that no engine hits, misses, summons or commands were skipped.',
    completenessBoundary: 'No native final watermark: a removed tail consisting of whole closed scopes is not detectable here; finalized owned-log provenance is a separate requirement.',
    leaderIds: [...leaders], leaderIdentity: leaders.size ? 'caller-supplied' : 'not present in native trace',
    battles: output };
}

module.exports = { auditLogs };
if (require.main === module) {
  try {
    const paths = [], leaderIds = [];
    const args = process.argv.slice(2);
    for (let i = 0; i < args.length; i++) {
      if (args[i] === '--leader') {
        const text = args[++i];
        if (!text || !/^(?:0x[0-9a-f]{1,8}|\d+)$/i.test(text) || !u32(Number(text)))
          throw new Error('--leader requires a uint32 unit ID');
        leaderIds.push(Number(text));
      } else if (args[i].startsWith('--')) throw new Error(`Unknown option: ${args[i]}`);
      else paths.push(args[i]);
    }
    if (!paths.length) throw new Error('Usage: node battle-trace-audit.js <hostLog> [joinLog] [--leader <unitId>]');
    const result = auditLogs(paths.map(name => ({ name, text: fs.readFileSync(name, 'utf8') })), { leaderIds });
    process.stdout.write(JSON.stringify(result, null, 2) + '\n');
    process.exitCode = result.complete ? 0 : 1;
  } catch (e) {
    process.stdout.write(JSON.stringify({ schema: 'battle-trace-audit/v1', complete: false,
      errors: [{ code: 'input-error', detail: e.message }] }, null, 2) + '\n');
    process.exitCode = 2;
  }
}

