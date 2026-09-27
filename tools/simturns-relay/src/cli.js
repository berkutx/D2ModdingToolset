#!/usr/bin/env node

import {pathToFileURL} from 'node:url';
import {MAX_MERGE_DAY, MAX_ONE_SHOT_DELAY_MS} from './coordinator.js';
import {resolveBootstrapReleaseFile, startRelayServer} from './server.js';

export function usage() {
  return [
    'D2MSS simultaneous-turn coordinator',
    '',
    'Usage:',
    '  node src/cli.js [--pipe <name>]',
    '                  [--merge-day <0-or-integer-at-least-2>]',
    '                  [--bootstrap-release-file <absolute-path>]',
    '                  [--bootstrap-cascade-delay-ms <non-negative-integer>]',
    '',
    'Environment:',
    '  D2MSS_SIMTURNS_PIPE   named pipe override',
  ].join('\n');
}

function parseMergeDay(value) {
  if (!/^(0|[1-9][0-9]*)$/.test(value)) {
    throw new Error('--merge-day requires an unsigned integer');
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed > MAX_MERGE_DAY || parsed === 1) {
    throw new Error(
      `--merge-day must be 0 (disabled) or an integer from 2 to ${MAX_MERGE_DAY}`,
    );
  }
  return parsed;
}

function parseNonNegativeInteger(value, option) {
  if (!/^(0|[1-9][0-9]*)$/.test(value)) {
    throw new Error(`${option} requires a non-negative integer`);
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed > MAX_ONE_SHOT_DELAY_MS) {
    throw new Error(
      `${option} must not exceed ${MAX_ONE_SHOT_DELAY_MS}`,
    );
  }
  return parsed;
}

export function parseArgs(argv) {
  let pipeName;
  let mergeDay = 0;
  let bootstrapReleaseFile = null;
  let bootstrapCascadeDelayMs = 0;
  let bootstrapReleaseFileSeen = false;
  let bootstrapCascadeDelaySeen = false;
  let mergeDaySeen = false;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--help' || arg === '-h') return {help: true};
    if (arg === '--pipe') {
      if (i + 1 >= argv.length) throw new Error('--pipe requires a value');
      pipeName = argv[++i];
      continue;
    }
    if (arg === '--merge-day') {
      if (mergeDaySeen) {
        throw new Error('--merge-day may be specified only once');
      }
      if (i + 1 >= argv.length) throw new Error('--merge-day requires a value');
      mergeDaySeen = true;
      mergeDay = parseMergeDay(argv[++i]);
      continue;
    }
    if (arg === '--bootstrap-release-file') {
      if (bootstrapReleaseFileSeen) {
        throw new Error('--bootstrap-release-file may be specified only once');
      }
      if (i + 1 >= argv.length) {
        throw new Error('--bootstrap-release-file requires a value');
      }
      bootstrapReleaseFileSeen = true;
      bootstrapReleaseFile = resolveBootstrapReleaseFile(argv[++i]);
      continue;
    }
    if (arg === '--bootstrap-cascade-delay-ms') {
      if (bootstrapCascadeDelaySeen) {
        throw new Error('--bootstrap-cascade-delay-ms may be specified only once');
      }
      if (i + 1 >= argv.length) {
        throw new Error('--bootstrap-cascade-delay-ms requires a value');
      }
      bootstrapCascadeDelaySeen = true;
      bootstrapCascadeDelayMs = parseNonNegativeInteger(
        argv[++i],
        '--bootstrap-cascade-delay-ms',
      );
      continue;
    }
    throw new Error(`unknown argument: ${arg}`);
  }
  return {
    help: false,
    pipeName,
    mergeDay,
    bootstrapReleaseFile,
    bootstrapCascadeDelayMs,
  };
}

export async function main(argv = process.argv.slice(2)) {
  let options;
  try {
    options = parseArgs(argv);
  } catch (error) {
    console.error(error.message);
    console.error(usage());
    process.exitCode = 2;
    return null;
  }

  if (options.help) {
    console.log(usage());
    return null;
  }

  let server;
  try {
    server = await startRelayServer({
      pipeName: options.pipeName,
      mergeDay: options.mergeDay,
      bootstrapReleaseFile: options.bootstrapReleaseFile,
      bootstrapCascadeDelayMs: options.bootstrapCascadeDelayMs,
    });
  } catch (error) {
    console.error(`Failed to start simturns relay: ${error.message}`);
    process.exitCode = 1;
    return null;
  }

  let stopping = false;
  const stop = async (signal) => {
    if (stopping) return;
    stopping = true;
    server.log('signal', {signal});
    await server.close();
  };
  process.once('SIGINT', () => { void stop('SIGINT'); });
  process.once('SIGTERM', () => { void stop('SIGTERM'); });
  return server;
}

const isMain = process.argv[1] !== undefined &&
  import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
  await main();
}
