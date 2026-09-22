import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {parseArgs, usage} from '../src/cli.js';

const cliPath = fileURLToPath(new URL('../src/cli.js', import.meta.url));

test('CLI parses the removable bootstrap gate and one-shot cascade delay', () => {
  const releaseFile = path.join(os.tmpdir(), 'd2mss-bootstrap.release');
  assert.deepEqual(
    parseArgs([
      '--pipe',
      process.platform === 'win32'
        ? String.raw`\\.\pipe\d2mss.simturns.cli`
        : path.join(os.tmpdir(), 'd2mss.simturns.cli.sock'),
      '--merge-day',
      '12',
      '--bootstrap-release-file',
      releaseFile,
      '--bootstrap-cascade-delay-ms',
      '500',
    ]),
    {
      help: false,
      pipeName: process.platform === 'win32'
        ? String.raw`\\.\pipe\d2mss.simturns.cli`
        : path.join(os.tmpdir(), 'd2mss.simturns.cli.sock'),
      mergeDay: 12,
      bootstrapReleaseFile: path.normalize(releaseFile),
      bootstrapCascadeDelayMs: 500,
    },
  );
});

test('CLI defaults merge day to disabled and validates the server-owned range', () => {
  assert.equal(parseArgs([]).mergeDay, 0);
  assert.equal(parseArgs(['--merge-day', '0']).mergeDay, 0);
  assert.equal(parseArgs(['--merge-day', '2']).mergeDay, 2);
  assert.equal(parseArgs(['--merge-day', '2147483647']).mergeDay, 0x7fffffff);
  for (const value of [
    '-1',
    '1',
    '1.5',
    '+2',
    '02',
    '2147483648',
    '4294967295',
    '4294967296',
  ]) {
    assert.throws(() => parseArgs(['--merge-day', value]), /--merge-day/);
  }
  assert.throws(
    () => parseArgs(['--merge-day', '2', '--merge-day', '3']),
    /may be specified only once/,
  );
});

test('CLI bootstrap options reject relative paths, invalid numbers, and duplicates', () => {
  assert.throws(
    () => parseArgs(['--bootstrap-release-file', 'relative.release']),
    /must be an absolute path/,
  );
  for (const value of [
    '-1',
    '1.5',
    '+1',
    '01',
    'NaN',
    '2147483648',
    '9007199254740992',
  ]) {
    assert.throws(
      () => parseArgs(['--bootstrap-cascade-delay-ms', value]),
      /non-negative integer|must not exceed/,
    );
  }
  assert.throws(
    () => parseArgs([
      '--bootstrap-release-file',
      path.join(os.tmpdir(), 'first.release'),
      '--bootstrap-release-file',
      path.join(os.tmpdir(), 'second.release'),
    ]),
    /may be specified only once/,
  );
  assert.throws(
    () => parseArgs([
      '--bootstrap-cascade-delay-ms',
      '0',
      '--bootstrap-cascade-delay-ms',
      '500',
    ]),
    /may be specified only once/,
  );
});

test('CLI help exposes both harness-only bootstrap options without starting a server', () => {
  const result = spawnSync(process.execPath, [cliPath, '--help'], {
    encoding: 'utf8',
  });
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /--bootstrap-release-file <absolute-path>/);
  assert.match(result.stdout, /--bootstrap-cascade-delay-ms <non-negative-integer>/);
  assert.match(result.stdout, /--merge-day <0-or-integer-at-least-2>/);
  assert.match(usage(), /--bootstrap-release-file/);
});

test('CLI rejects a relative bootstrap release file before server startup', () => {
  const result = spawnSync(
    process.execPath,
    [cliPath, '--bootstrap-release-file', 'relative.release'],
    {encoding: 'utf8'},
  );
  assert.equal(result.status, 2);
  assert.match(result.stderr, /bootstrap release file must be an absolute path/);
  assert.doesNotMatch(result.stdout, /"event":"listening"/);
});
