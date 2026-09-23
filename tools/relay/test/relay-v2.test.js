'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');
const { spawn, spawnSync } = require('child_process');

const relayScript = path.resolve(__dirname, '../relay.js');
const helperScript = path.resolve(__dirname, '../../test/_relay.ps1');
const productionPocScript = path.resolve(
    __dirname, '../../test/simturns-production-poc.ps1');
const literalInnerStartupScript = path.resolve(
    __dirname, '../../test/_literal_inner_startup.ps1');
const gameplayScript = path.resolve(
    __dirname, '../../test/_simturns_gameplay.ps1');
const nativeBridgeScript = path.resolve(
    __dirname, '../../../mss32/src/testdrv/packetlogicbridge.cpp');
const nativeWorldActionsScript = path.resolve(
    __dirname, '../../../mss32/src/testdrv/worldactions.cpp');
const nativePhaseGameHooksScript = path.resolve(
    __dirname, '../../../mss32/src/phasegamehooks.cpp');
const nativeNetInterceptScript = path.resolve(
    __dirname, '../../../mss32/src/netintercept.cpp');
const nativeTestdrvScript = path.resolve(
    __dirname, '../../../mss32/src/testdrv/testdrv.cpp');
const nativeAutonavScript = path.resolve(
    __dirname, '../../../mss32/src/testdrv/autonav.cpp');
const nativeUiReporterScript = path.resolve(
    __dirname, '../../../mss32/src/testdrv/uistatereporter.cpp');
const nativeWorldReporterScript = path.resolve(
    __dirname, '../../../mss32/src/testdrv/worldreporter.cpp');
const nativeRussobitSitesScript = path.resolve(
    __dirname, '../../../mss32/include/simturns/russobit_sites.h');
const nativeNettraceScript = path.resolve(
    __dirname, '../../../mss32/src/testdrv/nettracehooks.cpp');
const nativeEngineHooksScript = path.resolve(
    __dirname, '../../../mss32/src/simturns/engine_hooks.cpp');
const nativePatchesScript = path.resolve(
    __dirname, '../../../mss32/src/simturns/patches.cpp');
const nativeSimturnController = path.resolve(
    __dirname, '../../../mss32/src/simturns/controller.cpp');
const nativeSimturnProtocolHeader = path.resolve(
    __dirname, '../../../mss32/include/simturns/protocol.h');
const nativeSimturnStateHeader = path.resolve(
    __dirname, '../../../mss32/include/simturns/state.h');
const nativeSimturnSessionTypesHeader = path.resolve(
    __dirname, '../../../mss32/include/simturns/session_types.h');
const nativeControlClientCoreHeader = path.resolve(
    __dirname, '../../../mss32/include/simturns/control_client_core.h');
const nativeControlClientCoreSource = path.resolve(
    __dirname, '../../../mss32/src/simturns/control_client_core.cpp');
const nativeCoordinatorPortHeader = path.resolve(
    __dirname, '../../../mss32/include/simturns/coordinator_port.h');
const nativeCoordinatorPortSource = path.resolve(
    __dirname, '../../../mss32/src/simturns/coordinator_port.cpp');
const nativeLocalCoordinatorAdapter = path.resolve(
    __dirname, '../../../mss32/src/testdrv/local_coordinator_adapter.cpp');
const nativeHooksScript = path.resolve(
    __dirname, '../../../mss32/src/hooks.cpp');
const stockStartupCJoinFixture = path.resolve(
    __dirname, '../../test/fixtures/stock-startup-cjoin-russobit.json');
const stockStartupCConnectFixture = path.resolve(
    __dirname, '../../test/fixtures/stock-startup-cconnect-russobit.json');
const simturnsRussobitFixture = path.resolve(
    __dirname, '../../test/fixtures/simturns-russobit.json');

function powerShellFunction(source, name) {
    const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const match = new RegExp(`^function ${escaped}\\b`, 'm').exec(source);
    assert.ok(match, `PowerShell function ${name} must remain independently auditable`);
    const next = /^function\s+/m.exec(source.slice(match.index + match[0].length));
    const end = next
        ? match.index + match[0].length + next.index
        : source.length;
    return source.slice(match.index, end);
}

function runPowerShellContract(script, marker, description, timeout = 30000) {
    const encoded = Buffer.from(script, 'utf16le').toString('base64');
    const stdinCommand =
        `$source = [Text.Encoding]::Unicode.GetString(` +
        `[Convert]::FromBase64String('${encoded}')); ` +
        `& ([scriptblock]::Create($source))`;
    const result = spawnSync('pwsh', [
        '-NoLogo',
        '-NoProfile',
        '-NonInteractive',
        '-Command',
        '-',
    ], { encoding: 'utf8', input: stdinCommand, timeout });
    assert.equal(result.error, undefined,
        `could not execute ${description}: ${result.error}`);
    assert.equal(result.status, 0,
        `${description} failed:\n${result.stdout}\n${result.stderr}`);
    assert.match(result.stdout, new RegExp(marker),
        `${description} omitted its pass marker`);
    return result;
}

const Op = Object.freeze({
    Hello: 0x0001,
    HelloAck: 0x0002,
    BeginApplied: 0x0204,
    StartupJoinObserved: 0x0206,
    StartupBeginObserved: 0x0207,
    StartupDirectedBeginObserved: 0x0208,
    StartupCompleteObserved: 0x0209,
    BeginSendReturned: 0x020a,
    InvokeButton: 0x0300,
    SetSelection: 0x0301,
    SetEditText: 0x0303,
    CommandResult: 0x0304,
    MoveStack: 0x0305,
    HireMerc: 0x0307,
    MoveGroupUnit: 0x0308,
    DismissUnit: 0x0309,
    SelectScenarioPath: 0x030a,
    EnableToggle: 0x030b,
    EnableAutoBattle: 0x030c,
    AutoBattleKickResult: 0x030d,
    CommandStarted: 0x030e,
    InvokePairedEndTurn: 0x030f,
    ReleasePairedEndTurn: 0x0310,
    MoveStackToward: 0x0311,
    ReleaseStartupActions: 0x0312,
    UiSnapshot: 0x0410,
    WorldSnapshot: 0x0411,
    LegacyStacksSnapshot: 0x0412,
    LobbyChat: 0x0413,
    Log: 0xff00,
});

test('transport-neutral simturns contracts do not depend on local adapters', () => {
    const contracts = [
        ['protocol.h', nativeSimturnProtocolHeader],
        ['state.h', nativeSimturnStateHeader],
        ['session_types.h', nativeSimturnSessionTypesHeader],
        ['control_client_core.h', nativeControlClientCoreHeader],
        ['control_client_core.cpp', nativeControlClientCoreSource],
    ];
    const localAdapterDependency =
        /#include\s+"(?:simturns\/)?(?:config|named_pipe_endpoint|pipeclient)\.h"|#include\s+"testdrv\/|D2MSS_SIMTURNS_PIPE|D2_TESTDRV|GetEnvironmentVariable|\b_?w?getenv\b|<[Ww]indows\.h>|<[Ww]insock2?\.h>|\b(?:WSAStartup|SOCKET|HANDLE|DWORD)\b|<process\.h>|_beginthreadex|GetCurrentProcessId/;

    for (const [name, sourcePath] of contracts) {
        const source = fs.readFileSync(sourcePath, 'utf8');
        assert.doesNotMatch(source, localAdapterDependency,
            `${name} must remain independent of env, named-pipe, and testdrv adapters`);
    }
});

test('engine controller and port remain independent of the local test transport', () => {
    const controller = fs.readFileSync(nativeSimturnController, 'utf8');
    const portHeader = fs.readFileSync(nativeCoordinatorPortHeader, 'utf8');
    const portSource = fs.readFileSync(nativeCoordinatorPortSource, 'utf8');
    const adapter = fs.readFileSync(nativeLocalCoordinatorAdapter, 'utf8');
    assert.match(controller, /#include\s+"simturns\/coordinator_port\.h"/);
    for (const production of [controller, portHeader, portSource]) {
        assert.doesNotMatch(production,
            /#include\s+"simturns\/(?:pipeclient|named_pipe_endpoint)\.h"|D2MSS_SIMTURNS_PIPE/);
    }
    assert.match(portHeader, /class CoordinatorPort final/);
    assert.match(adapter, /#ifdef D2_TESTDRV/);
    assert.match(adapter, /D2MSS_SIMTURNS_PIPE/);
    assert.match(adapter, /simturns::CoordinatorPort::processInstance\(\)/);
});

test('native protocol phase and inbound routing remain core-owned', () => {
    const header = fs.readFileSync(nativeControlClientCoreHeader, 'utf8');
    const core = fs.readFileSync(nativeControlClientCoreSource, 'utf8');
    const port = fs.readFileSync(nativeCoordinatorPortSource, 'utf8');
    const adapter = fs.readFileSync(nativeLocalCoordinatorAdapter, 'utf8');
    assert.match(header, /enum class ControlPhase/);
    assert.match(header, /bool acceptInbound\(const protocol::Frame&/);
    assert.match(core, /ControlClientCore::acceptInbound/);
    assert.match(port, /core\.acceptInbound\(frame, decoded, failure\)/);
    assert.doesNotMatch(adapter, /ControlClientCore|acceptInbound|acceptEngineAction/);
    assert.doesNotMatch(header,
        /acceptDispatchCascade\([^)]*\b(?:ready|bootstrapping|bootstrapPrepared)\b|prepareCascadeResult\([^)]*\b(?:ready|bootstrapping|bootstrapPrepared)\b/s);
});

test('a failed armed session cannot be restarted or overwritten before teardown', () => {
    const port = fs.readFileSync(nativeCoordinatorPortSource, 'utf8');
    const arm = port.match(/bool CoordinatorPort::arm\([\s\S]*?(?=\n(?:#|bool CoordinatorPort::))/);
    const start = port.match(/bool CoordinatorPort::start\([\s\S]*?(?=\nvoid CoordinatorPort::)/);
    assert.ok(arm && start, 'session lifecycle paths remain independently auditable');
    assert.match(arm[0], /if \(impl->armed/);
    assert.match(start[0], /!impl->armed \|\| impl->started \|\| impl->failed/);
    assert.match(port, /if \(!armed \|\| failed \|\| \(expectedGeneration && expectedGeneration != generation\)\) return;/);
    assert.match(port, /failed = true;/);
    assert.match(port, /selectedGeneration = generation;/);
});

test('held host admits only the remote pre-merge catch-up action', () => {
    const core = fs.readFileSync(nativeControlClientCoreSource, 'utf8');
    const controller = fs.readFileSync(nativeSimturnController, 'utf8');
    const actionGuard = core.match(
        /bool ControlClientCore::acceptEngineAction[\s\S]*?(?=\nstd::uint32_t ControlClientCore::localHandle)/);
    const handler = controller.match(
        /void handleApplyTurnStart[\s\S]*?(?=\nvoid handleActivateTurn)/);

    assert.ok(actionGuard && handler,
        'the held catch-up path must remain independently auditable');
    assert.match(actionGuard[0],
        /case protocol::EngineActionKind::ApplyTurnStart:[\s\S]*sessionConfig\.role != Role::Host[\s\S]*currentPhase != ControlPhase::Active[\s\S]*currentPhase != ControlPhase::Holding/,
        'only the host core may accept ApplyTurnStart while active or held');
    assert.match(actionGuard[0],
        /currentPhase == ControlPhase::Holding[\s\S]*action\.playerHandle == localHandle\(\)[\s\S]*ordinary ApplyTurnStart has an invalid target or reused lease/,
        'a held host must reject a second local-player turn-start action');
    assert.match(actionGuard[0],
        /reportedLease != currentLease[\s\S]*ApplyTurnStart preceded the host EndTurnApplied write/,
        'peer catch-up remains causally bound to the host post-dispatch EndTurn proof');
    assert.match(handler[0],
        /peerCatchUp = current == Phase::Held[\s\S]*action\.playerHandle == otherHandle\(\)[\s\S]*!bootstrap && current != Phase::Independent && !peerCatchUp/,
        'native execution must admit only the negotiated peer while the local UI is held');
    assert.match(handler[0],
        /endTurnRxClaimFor\(action\.playerHandle\)[\s\S]*ApplyTurnStart lacks its exact player\/EndTurn proof/,
        'the engine call must retain its exact natural EndTurn receive claim');
});

test('HoldInput is policy-only and merge completion remains bound to natural host BeginTurn', () => {
    const controller = fs.readFileSync(nativeSimturnController, 'utf8');
    const hold = controller.match(
        /void handleHoldInput[\s\S]*?(?=\nvoid handlePrepareMerge)/);
    assert.ok(hold, 'HoldInput handler must remain independently auditable');
    assert.match(hold[0],
        /phase\(\) != Phase::Independent[\s\S]*action\.playerHandle != localHandle\(\)[\s\S]*!enterHeld\(\)[\s\S]*!setScenarioDay\(clientObjectMap\(\), action\.day\)/,
        'HoldInput must bind the local N-1 policy transition to the relay action');
    assert.match(hold[0],
        /reportActionResult\(action, true\)/,
        'the held-state policy transition must be acknowledged synchronously');
    assert.doesNotMatch(hold[0],
        /injectBeginTurn|claimPendingUiApply|otherHandle\(\)|runSerializedCurrentTurn/,
        'HoldInput must not invent a peer turn, peer day, or engine cascade');

    const naturalPredicate = controller.match(
        /constexpr bool naturalMergeCurrentMatchesRole[\s\S]*?(?=\nstatic_assert\(naturalMergeCurrentMatchesRole)/);
    assert.ok(naturalPredicate,
        'the role-specific natural-merge CPhase diagnostic must remain independently auditable');
    assert.match(naturalPredicate[0],
        /if \(hostRole\)[\s\S]*localPlayerHandle != 0[\s\S]*localPlayerHandle == hostPlayerHandle[\s\S]*currentHandle == hostPlayerHandle/,
        'host natural merge must keep an exact local-host CPhase mirror');
    assert.match(naturalPredicate[0],
        /localPlayerHandle != hostPlayerHandle[\s\S]*currentPlayerIsNegotiated\(currentHandle, localPlayerHandle,[\s\S]*hostPlayerHandle\)/,
        'join natural merge may retain only either member of its distinct negotiated pair');
    assert.match(controller,
        /static_assert\(naturalMergeCurrentMatchesRole\(true, 0xa3de0001u,[\s\S]*static_assert\(!naturalMergeCurrentMatchesRole\(true, 0xa3de0002u,[\s\S]*static_assert\(!naturalMergeCurrentMatchesRole\(true, 0xa3de0001u,[\s\S]*static_assert\(naturalMergeCurrentMatchesRole\(false, 0xa3de0002u,[\s\S]*static_assert\(naturalMergeCurrentMatchesRole\(false, 0xa3de0001u,[\s\S]*static_assert\(!naturalMergeCurrentMatchesRole\(false, 0,[\s\S]*static_assert\(!naturalMergeCurrentMatchesRole\(false, 0xa3de0003u,[\s\S]*static_assert\(!naturalMergeCurrentMatchesRole\(false, 0xa3de0001u,/,
        'compile-time examples must reject wrong host roles, zero, third-party, and duplicate join identities');

    const completion = controller.match(
        /void completeClaimedPendingUiApply\(const PendingUiApply& pending\)\r?\n\{[\s\S]*?(?=\r?\nvoid completeNaturalStockHandoffDispatch)/);
    assert.ok(completion, 'PendingUiApply completion must remain identifiable');
    assert.match(completion[0],
        /pending\.kind == PendingUiApplyKind::ActivateTurn[\s\S]*pending\.action\.kind != protocol::EngineActionKind::ActivateTurn[\s\S]*currentHandle != localHandle\(\)/,
        'ordinary activation must retain its exact action kind, phase, and local owner');

    assert.match(completion[0],
        /const Phase expected = isHost\(\) \? Phase::Merging[\s\S]*: Phase::AwaitingStockTurn;\s*if\s*\(\s*phase\(\) != expected\s*\|\|\s*pending\.handle != hostHandle\(\)\s*\|\|\s*pending\.day != configuredMergeDay\(\)\s*\|\|\s*!naturalMergeCurrentMatchesRole\(\s*isHost\(\), currentHandle, localHandle\(\), hostHandle\(\)\)\s*\)\s*\{/,
        'natural merge handoff must retain its role-specific phase, exact host/day, and bounded CPhase diagnostic domain');
    assert.match(completion[0],
        /retirePendingUiApply\(pending\.actionId\)[\s\S]*reportActionResult\(pending\.action, true\)[\s\S]*g_naturalMergeReady\.compare_exchange_strong[\s\S]*publishMergeAppliedIfReady\(\)/,
        'queue drain must retire exactly once before publishing either action or merge completion');

    assert.doesNotMatch(completion[0], /HoldInput|EnterObserver|DeferredMerge/,
        'policy-only HoldInput must never enter the asynchronous UI apply path');

    assert.doesNotMatch(controller, /resumeDeferredMergeOnUi|DeferredMergeNow/,
        'the v8 prepare barrier must remove the old deferred MergeNow race');
});

test('secret UI input stays in the bounded request body and is never reflected', async (t) => {
    const relay = await startRelay(t);
    const agent = await relay.connect({ role: 'host', pid: 5201 });
    await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'secret input HelloAck');
    const widgets = [{ name: 'EDIT_PASSWORD', type: 'edit', state: { text: '[redacted]' } }];
    agent.send(Op.UiSnapshot, Buffer.from(JSON.stringify({
        dialog: 'DLG_LOGIN_ACCOUNT', instance: 11, ready: true, strategicIdle: false,
        mapLoaded: false, startupActionsHeld: false,
        widgets, targets: [{ dialog: 'DLG_LOGIN_ACCOUNT', instance: 111, widgets }],
    })));
    await waitFor(async () => (await requestJson(relay.base, '/api/state')).body.roles.host
        ?.dialogAppearance === 11, 2000, 'secret edit appearance');
    const endpoint = '/api/ui/edit-secret?role=host&dlg=DLG_LOGIN_ACCOUNT'
        + '&edit=EDIT_PASSWORD&appearance=11&instance=111';
    const secret = 'fixture-only:пароль?&hidden=1';
    const response = await requestJson(relay.base, endpoint, 'POST', secret);
    assert.equal(response.status, 200);
    assert.equal(response.body.found, true);
    assert.deepEqual(response.body, { role: 'host', found: true });
    assert.equal(agent.count(Op.SetEditText), 1);
    assert.ok(agent.last(Op.SetEditText).payload.includes(Buffer.from(secret)));
    assert.ok(!relay.output().includes(secret));
    const tooLarge = await requestJson(relay.base, endpoint, 'POST', 'x'.repeat(4097));
    assert.equal(tooLarge.status, 413);
    assert.equal(agent.count(Op.SetEditText), 1, 'oversized secret never reaches native');
    const reflected = await requestJson(relay.base, endpoint.replace('edit-secret', 'edit')
        + '&text=' + encodeURIComponent(secret), 'POST');
    assert.equal(reflected.status, 200);
    assert.equal(reflected.body.edit.text, '[redacted]');
    assert.ok(!JSON.stringify(reflected.body).includes(secret));
});

test('lobby smoke requires an exact unique room and refuses the fixed-fixture campaign', () => {
    const script = fs.readFileSync(path.resolve(__dirname, '../../test/lobby-simturns-smoke.ps1'), 'utf8');
    const entry = fs.readFileSync(path.resolve(__dirname, '../../test/simturns-test.ps1'), 'utf8');
    const helper = fs.readFileSync(helperScript, 'utf8');
    const room = powerShellFunction(script, 'Find-ExactLobbyRoom');
    runPowerShellContract(room + `
      $one = [pscustomobject]@{ state = [pscustomobject]@{ items = @('other', 'OH-exact') } }
      if ((Find-ExactLobbyRoom $one 'OH-exact') -ne 1) { throw 'wrong exact index' }
      if ((Find-ExactLobbyRoom $one 'OH-missing') -ne -1) { throw 'missing matched' }
      $duplicate = [pscustomobject]@{ state = [pscustomobject]@{ items = @('OH-exact', 'OH-exact') } }
      $failed = $false
      try { Find-ExactLobbyRoom $duplicate 'OH-exact' } catch { $failed = $true }
      if (-not $failed) { throw 'duplicate accepted' }
      'LOBBY-ROOM-CONTRACT-PASS'
    `, 'LOBBY-ROOM-CONTRACT-PASS', 'exact lobby room identity');
    assert.match(entry, /if \(\$Campaign -or \$GameplayMode -ne 'protocol'/);
    assert.match(script, /-Transport Lobby -ExpectedLobbyRoom \$expectedRoom/);
    assert.match(script, /Set-SecretEditText \$Role DLG_LOGIN_ACCOUNT EDIT_PASSWORD/);
    assert.match(helper, /StartsWith\('D2_LOBBY_'[\s\S]*EnvironmentVariables\.Remove\(\$key\)/);
    assert.doesNotMatch(script, /Stop-Process|Start-ProductionSimRelay|D2MSS_SIMTURNS\s*=/);
    assert.match(script, /campaign18 = \$false; gameplayAcceptance = \$false/);
});

test('paired native EndTurn preserves its in-flight command without blocking a frame', () => {
    const source = fs.readFileSync(nativeAutonavScript, 'utf8');
    const start = source.indexOf('bool invokePairedEndTurn(const RemoteCmd& cmd)');
    const end = source.indexOf('// Literal contract carried over', start);
    assert.ok(start >= 0 && end > start);
    const body = source.slice(start, end);
    assert.doesNotMatch(body, /WaitForSingleObject|WaitForMultipleObjects|Sleep\(|CreateEvent/);
    assert.match(body, /g_pairedEndTurnArmedAt = GetTickCount64\(\)/);
    assert.match(body, /g_pairedEndTurnReleasedSeq\.load/);
    assert.match(body, /isStrategicIdle/);
    assert.match(body, /exactButton != g_pairedEndTurnButton \|\| exactFunctor != g_pairedEndTurnFunctor/);
    assert.equal((body.match(/invokePairedEndTurnCallback\(exactFunctor\)/g) || []).length, 1);
    assert.match(source, /cmd\.type == 12[\s\S]*invokePairedEndTurn/);
});

const mutationOps = new Set([
    Op.InvokeButton, Op.SetSelection, Op.SetEditText, Op.MoveStack, Op.SelectScenarioPath, Op.EnableToggle,
    Op.InvokePairedEndTurn,
    Op.HireMerc, Op.MoveGroupUnit, Op.DismissUnit, Op.MoveStackToward,
]);

test('universal world and UTF-8 chat projections retain modern fields without mixing evidence', async (t) => {
    const relay = await startRelay(t);
    const agent = await relay.connect({ role: 'host', pid: 5101 });
    await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
    const world = JSON.parse(worldSnapshot(2, 4).toString());
    Object.assign(world, { strategicActionReady: true,
        camps: [{ id: 'S000SI0001', units: [{ impl: 'G000UU0001' }] }],
        bags: [{ id: 'S000BG0001', x: 8, y: 9 }] });
    world.stacks[0].slots = [{ id: 'S000UN0001', position: 0 }];
    const messages = [{ t: '2026-09-08 12:00:00', sender: 'host', text: 'Привет — test' }];
    agent.send(Op.WorldSnapshot, Buffer.from(JSON.stringify(world)));
    agent.send(Op.LobbyChat, Buffer.from(JSON.stringify({ messages })));
    await waitFor(async () => (await requestJson(relay.base, '/api/lobby/chat?role=host'))
        .body.messages.length === 1, 2000, 'chat snapshot');
    const actual = (await requestJson(relay.base, '/api/world?role=host')).body;
    for (const field of ['strategicActionReady', 'players', 'stacks', 'camps', 'bags'])
        assert.deepEqual(actual[field], world[field], field);
    const roles = (await requestJson(relay.base, '/api/state')).body.roles;
    assert.equal(roles.host.strategicActionReady, true);
    assert.deepEqual(roles.host.camps, world.camps);
    assert.ok(!JSON.stringify(roles).includes(messages[0].text), 'chat stays out of hot role state');
    assert.deepEqual((await requestJson(relay.base, '/api/lobby/chat?role=host')).body.messages, messages);
    assert.deepEqual((await requestJson(relay.base, '/api/lobby/chat')).body.roles.host.messages, messages);
    assert.equal((await requestJson(relay.base, '/api/legacy-stacks')).body.sequence, 0,
        'chat0413 cannot masquerade as binary LegacyStacks0412');
});

test('UI projections expose the current world watermark independently of UI publication', async (t) => {
    const relay = await startRelay(t);
    assert.equal((await requestJson(relay.base, '/api/ui?role=host')).body.worldSeq, 0,
        'an unregistered role has no world evidence');
    assert.deepEqual((await requestJson(relay.base, '/api/ui')).body.roles, {});
    const host = await relay.connect({ role: 'host', pid: 5102 });
    await waitFor(() => host.count(Op.HelloAck) === 1, 2000, 'HelloAck');
    host.send(Op.UiSnapshot, strategicUiSnapshot(1, 101, { strategicIdle: true }));
    await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
        .body.uiSeq > 0, 2000, 'UI before world');
    const initialUi = (await requestJson(relay.base, '/api/ui?role=host')).body;
    const initialRoles = (await requestJson(relay.base, '/api/ui')).body.roles;
    assert.equal(initialUi.worldSeq, 0);
    assert.equal(initialRoles.host.worldSeq, 0);
    assert.equal((await requestJson(relay.base, '/api/ui?role=join')).body.worldSeq, 0);

    for (const sequence of [1, 2]) {
        host.send(Op.WorldSnapshot, worldSnapshot(sequence, sequence + 3));
        await waitFor(async () => (await requestJson(relay.base, '/api/world?role=host'))
            .body.worldSeq === sequence, 2000, `world ${sequence} without another UI`);
        assert.deepEqual((await requestJson(relay.base, '/api/ui?role=host')).body,
            { ...initialUi, worldSeq: sequence }, 'only the current world watermark changes');
        assert.deepEqual((await requestJson(relay.base, '/api/ui')).body.roles,
            { host: { ...initialRoles.host, worldSeq: sequence } });
        assert.equal((await requestJson(relay.base, '/api/ui?role=join')).body.worldSeq, 0,
            'another role must not inherit the host watermark');
    }

    host.socket.destroy();
    await waitForTerminal(relay.base, /agent host pid=5102 disconnected/);
    assert.equal((await requestJson(relay.base, '/api/ui?role=host')).body.worldSeq, 2,
        'terminal disconnect retains evidence; it is not an implicit reset or reconnect');
    const freshRelay = await startRelay(t);
    const freshHost = await freshRelay.connect({ role: 'host', pid: 5103 });
    await waitFor(() => freshHost.count(Op.HelloAck) === 1, 2000, 'fresh HelloAck');
    assert.equal((await requestJson(freshRelay.base, '/api/ui?role=host')).body.worldSeq, 0);
    assert.equal((await requestJson(freshRelay.base, '/api/ui')).body.roles.host.worldSeq, 0,
        'a new relay instance starts with no inherited world evidence');
});

test('expensive world combat profiles are explicitly requested without dropping live oracles', () => {
    const native = fs.readFileSync(nativeWorldReporterScript, 'utf8');
    const arena = fs.readFileSync(path.resolve(__dirname, '../../test/luckytest-arena.ps1'), 'utf8');
    assert.match(native, /testenv::on\("D2TESTDRV_WORLD_PROFILES"\)/);
    assert.match(native, /if \(impl && g_includeUnitProfiles\)\s*\{\s*json \+= ',';\s*emitUnitProfile/);
    for (const field of ['unitIds', 'unitStates', 'movement', 'strategicActionReady', 'leaderId', 'slots'])
        assert.ok(native.includes(field), `live oracle retained: ${field}`);
    assert.match(arena, /\$flags = @\([^\r\n]*'WORLD_PROFILES'/);
    assert.match(arena, /\$null -eq \$ldr\.PSObject\.Properties\['reach'\][\s\S]*?throw/);
    for (const script of [productionPocScript, gameplayScript])
        assert.doesNotMatch(fs.readFileSync(script, 'utf8'), /WORLD_PROFILES|\.(?:reach|atkClass|armor|dmg)\b/);
});

test('campaign fail-fast preserves current-child teardown and the original failure', () => {
    const campaign = fs.readFileSync(path.resolve(__dirname, '../../test/simturns-acceptance-campaign.ps1'), 'utf8');
    assert.match(campaign, /\[switch\]\$StopOnFailure/);
    const stop = campaign.indexOf('if ($StopOnFailure -and -not [bool]$record.passed)');
    const teardown = campaign.lastIndexOf('$record.dplaysvrTeardown = Wait-NaturalDplayServerExit', stop);
    const aggregate = campaign.indexOf("$script:CurrentPhase = 'aggregate-tally'", stop);
    assert.ok(teardown >= 0 && stop > teardown && aggregate > stop);
    const gate = campaign.slice(stop, aggregate);
    for (const field of ['phase', 'category', 'message'])
        assert.ok(gate.includes(`$record.failure.${field}`));
    assert.match(gate, /Throw-CampaignFailure/);
});

test('universal generic actions require exact owners and preserve each typed payload', async (t) => {
    const relay = await startRelay(t);
    const agent = await relay.connect({ role: 'host', pid: 5102 });
    await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
    agent.send(Op.UiSnapshot, mapUiSnapshot(8, 108));
    await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
        .body.dialogAppearance === 8, 2000, 'ready map');
    const actions = [
        ['move-toward', Op.MoveStackToward, 'id=S000ST0001&x=14&y=16', ['S000ST0001'], [14, 16]],
        ['hire', Op.HireMerc, 'camp=S000SI0001&stack=0xA3E30001&unit=G000UU0001',
            ['S000SI0001', '0xA3E30001', 'G000UU0001'], []],
        ['move-unit', Op.MoveGroupUnit, 'stack=S000ST0001&src=0&dst=5', ['S000ST0001'], [0, 5]],
        ['dismiss', Op.DismissUnit, 'stack=S000ST0001&unit=S000UN0001',
            ['S000ST0001', 'S000UN0001'], []],
    ];
    for (const [route, op, args, ids, ints] of actions) {
        const before = agent.count(op);
        const base = `/api/ui/${route}?role=host&${args}`;
        assert.equal((await requestJson(relay.base, base, 'POST')).status, 400);
        assert.equal((await requestJson(relay.base, base + '&appearance=7&instance=108', 'POST')).status, 409);
        assert.equal((await requestJson(relay.base, base + '&appearance=8&instance=109', 'POST')).status, 409);
        assert.equal(agent.count(op), before, 'invalid identity must not send a frame');
        for (let repeat = 0; repeat < 2; repeat++) {
            const response = await requestJson(relay.base,
                base + '&appearance=8&instance=108', 'POST');
            assert.equal(response.status, 200);
            assert.equal(response.body.found, true);
            const payload = agent.last(op).payload;
            assert.ok(payload.readUInt32LE(0) > 0, 'nonzero correlated sequence');
            assert.equal(payload.readUInt32LE(4), 8);
            assert.equal(payload.readUInt32LE(8), 108);
            let offset = 12;
            for (const id of ids) {
                const size = payload.readUInt16LE(offset); offset += 2;
                assert.equal(payload.toString('utf8', offset, offset + size), id);
                offset += size;
            }
            for (const value of ints) { assert.equal(payload.readInt32LE(offset), value); offset += 4; }
            assert.equal(offset, payload.length, 'no untyped trailing payload');
        }
        assert.equal(agent.count(op), before + 2, 'valid repeat is a distinct correlated action');
    }
    assert.equal((await requestJson(relay.base,
        '/api/ui/move?role=host&id=0xA3E30001&x=14&y=16&appearance=8&instance=108', 'POST')).status, 400,
    'strict0305 cannot lose its exact source oracle');
    agent.send(Op.UiSnapshot, uiSnapshot(9, 109, { dialog: 'DLG_MERCENARIES' }));
    await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
        .body.dialogAppearance === 9, 2000, 'ready camp owner');
    assert.equal((await requestJson(relay.base,
        '/api/ui/hire?role=host&camp=S000SI0001&stack=S000ST0001&unit=G000UU0001&appearance=9&instance=109',
        'POST')).body.found, true, 'hire is valid on its exact ready camp root');
    assert.equal((await requestJson(relay.base,
        '/api/ui/move-toward?role=host&id=S000ST0001&x=1&y=2&appearance=9&instance=109', 'POST')).status, 409,
    'toward movement still requires the bare map');
});

test('shared helper resets strict deadlines and generic callers opt in without changing explicit waits', () => {
    runPowerShellContract(`
$ErrorActionPreference = 'Stop'
. '${helperScript.replace(/'/g, "''")}'
$script:seen = @()
function Invoke-RestMethod {
    param($Uri, $Method, $TimeoutSec)
    $script:seen += [pscustomobject]@{ uri=$Uri; timeout=$TimeoutSec }
    return [pscustomobject]@{ found=$true }
}
$null = script:Post 'invoke?role=host'
if ($script:seen[-1].timeout -ne 8 -or $script:seen[-1].uri -match 'timeoutMs') { throw 'strict default changed' }
$script:RelayCommandTimeoutMilliseconds = 30000
$null = script:Post 'select?role=host'
if ($script:seen[-1].timeout -ne 40 -or $script:seen[-1].uri -notmatch 'timeoutMs=30000$') { throw 'generic deadline missing' }
$null = script:Post 'move?role=host&timeoutMs=1000' 11
if ($script:seen[-1].timeout -ne 11 -or $script:seen[-1].uri -match '30000') { throw 'explicit strict deadline overwritten' }
. '${helperScript.replace(/'/g, "''")}'
if ($script:RelayCommandTimeoutMilliseconds -ne 5000) { throw 'dot-source inherited another scenario deadline' }
$script:RelayCommandTimeoutMilliseconds = 120001
$rejected=$false
try { $null=script:Post 'invoke?role=host' } catch { $rejected=$true }
if (-not $rejected -or $script:seen.Count -ne 3) { throw 'unbounded deadline reached HTTP' }
Write-Output 'UNIVERSAL_HELPER_DEADLINES_PASS'
`, 'UNIVERSAL_HELPER_DEADLINES_PASS', 'shared strict/generic deadline contract');
});

function frame(op, payload = Buffer.alloc(0), flags = 0) {
    const out = Buffer.alloc(8 + payload.length);
    out.writeUInt32LE(4 + payload.length, 0);
    out.writeUInt16LE(op, 4);
    out.writeUInt16LE(flags, 6);
    payload.copy(out, 8);
    return out;
}

function hello(role, pid, version = 7) {
    const modulePath = Buffer.from('C:\\fixture\\mss32.dll');
    const roleBytes = Buffer.from(role);
    const out = Buffer.alloc(16 + modulePath.length + roleBytes.length);
    out.writeUInt32LE(version, 0);
    out.writeUInt32LE(pid, 4);
    out.writeUInt32LE(modulePath.length, 8);
    modulePath.copy(out, 12);
    const roleOffset = 12 + modulePath.length;
    out.writeUInt32LE(roleBytes.length, roleOffset);
    roleBytes.copy(out, roleOffset + 4);
    return out;
}

function uiSnapshot(appearance, owner,
    { enabled = true, checked = false, ready = true, dialog = 'DLG_EVENT_POPUP',
        strategicIdle = false, mapLoaded = false, startupActionsHeld = false } = {}) {
    const widgets = [
        { name: 'BTN_RIGHTSIDE', type: 'button', state: { enabled } },
        { name: 'TLBOX_GAME_SLOT', type: 'listbox', state: { selected: 0, total: 2 } },
        { name: 'TOG_AUTOBATTLE', type: 'toggle', state: { checked } },
    ];
    return Buffer.from(JSON.stringify({
        dialog, instance: appearance, ready, strategicIdle, widgets,
        mapLoaded, startupActionsHeld,
        targets: [{ dialog, instance: owner, widgets }],
    }));
}

function strategicUiSnapshot(appearance, owner,
    { enabled = true, ready = true, strategicIdle = false } = {}) {
    const widgets = [
        { name: 'BTN_END_TURN', type: 'button', state: { enabled } },
    ];
    return Buffer.from(JSON.stringify({
        dialog: 'DLG_STRATEGIC', instance: appearance, ready, strategicIdle, widgets,
        mapLoaded: true, startupActionsHeld: false,
        targets: [{ dialog: 'DLG_STRATEGIC', instance: owner, widgets }],
    }));
}

function lobbyUiSnapshot(appearance, owner,
    { enabled = true, ready = true } = {}) {
    const widgets = [
        { name: 'BTN_OK', type: 'button', state: { enabled } },
    ];
    return Buffer.from(JSON.stringify({
        dialog: 'DLG_LOBBY', instance: appearance, ready,
        mapLoaded: false, startupActionsHeld: false,
        strategicIdle: false, widgets,
        targets: [{ dialog: 'DLG_LOBBY', instance: owner, widgets }],
    }));
}

function mainMenuUiSnapshot(appearance, owner,
    { enabled = true, ready = true, dialog = 'DLG_MAIN_MENU', strategicIdle = false } = {}) {
    const widgets = [
        { name: 'BTN_MULTI', type: 'button', state: { enabled } },
    ];
    return Buffer.from(JSON.stringify({
        dialog, instance: appearance, ready, strategicIdle, widgets,
        mapLoaded: false, startupActionsHeld: false,
        targets: [{ dialog, instance: owner, widgets }],
    }));
}

function beginTurnWithStrategicTarget(appearance, beginOwner, strategicOwner) {
    const beginWidgets = [
        { name: 'BTN_OK', type: 'button', state: { enabled: true } },
    ];
    const strategicWidgets = [
        { name: 'BTN_END_TURN', type: 'button', state: { enabled: true } },
    ];
    return Buffer.from(JSON.stringify({
        dialog: 'DLG_BEGIN_TURN', instance: appearance, ready: true,
        mapLoaded: true, startupActionsHeld: false,
        strategicIdle: false, widgets: beginWidgets,
        targets: [
            { dialog: 'DLG_BEGIN_TURN', instance: beginOwner, widgets: beginWidgets },
            { dialog: 'DLG_STRATEGIC', instance: strategicOwner, widgets: strategicWidgets },
        ],
    }));
}

function mapUiSnapshot(appearance, owner, dialog = 'DLG_ISO_PAL') {
    return Buffer.from(JSON.stringify({
        dialog, instance: appearance, ready: true, strategicIdle: false, widgets: [],
        mapLoaded: true, startupActionsHeld: false,
        targets: [{ dialog, instance: owner, widgets: [] }],
    }));
}

function isoPalWithStrategicTarget(appearance, isoOwner, strategicOwner,
    { enabled = true, strategicIdle = false } = {}) {
    const strategicWidgets = [
        { name: 'BTN_END_TURN', type: 'button', state: { enabled } },
    ];
    return Buffer.from(JSON.stringify({
        dialog: 'DLG_ISO_PAL', instance: appearance, ready: true, strategicIdle,
        mapLoaded: true, startupActionsHeld: false,
        widgets: [],
        targets: [
            { dialog: 'DLG_ISO_PAL', instance: isoOwner, widgets: [] },
            { dialog: 'DLG_STRATEGIC', instance: strategicOwner, widgets: strategicWidgets },
        ],
    }));
}

function worldSnapshot(day, x) {
    return Buffer.from(JSON.stringify({
        day, activePlayerId: '0xA3DE0001',
        players: [{ id: '0xA3DE0001', relation: 'self', human: true }],
        stacks: [{ id: '0xA3E30000', relation: 'self', x, y: 16, movement: 35 }],
    }));
}

function legacyStacksSnapshot(stacks) {
    const out = Buffer.alloc(4 + stacks.length * 20);
    out.writeUInt32LE(stacks.length, 0);
    for (let index = 0, offset = 4; index < stacks.length; index++, offset += 20) {
        const stack = stacks[index];
        out.writeUInt32LE(stack.id, offset);
        out.writeUInt32LE(stack.owner, offset + 4);
        out.writeInt32LE(stack.x, offset + 8);
        out.writeInt32LE(stack.y, offset + 12);
        out.writeUInt32LE(stack.movement, offset + 16);
    }
    return out;
}

function exactWorldSnapshot(day, id, x, y, movement) {
    return Buffer.from(JSON.stringify({
        day, activePlayerId: '0xA3DE0001',
        players: [{ id: '0xA3DE0001', relation: 'self', human: true }],
        stacks: [{ id, relation: 'self', x, y, movement }],
    }));
}

function beginApplied() {
    const out = Buffer.alloc(28);
    out.writeUInt32LE(1, 0);
    out.writeUInt32LE(7, 4);
    out.writeUInt32LE(56, 8);
    out.writeInt32LE(1, 12);
    out.writeUInt32LE(0, 16);
    out.writeUInt32LE(3, 20);
    out.writeUInt32LE(0xa3de0001, 24);
    return out;
}

function startupBeginObserved(addressee, commandSequence, activeHandle) {
    const out = Buffer.alloc(24);
    out.writeUInt32LE(1, 0);
    out.writeUInt32LE(7, 4);
    out.writeUInt32LE(56, 8);
    out.writeUInt32LE(addressee, 12);
    out.writeUInt32LE(commandSequence, 16);
    out.writeUInt32LE(activeHandle, 20);
    return out;
}

function beginSendReturned({
    idTo, frameLength = 56, sendResult, addressee, commandSequence, activeHandle,
}) {
    const out = Buffer.alloc(24);
    out.writeUInt32LE(idTo, 0);
    out.writeUInt32LE(frameLength, 4);
    out.writeInt32LE(sendResult, 8);
    out.writeUInt32LE(addressee, 12);
    out.writeUInt32LE(commandSequence, 16);
    out.writeUInt32LE(activeHandle, 20);
    return out;
}

function startupJoinObserved(joinedHandle) {
    const out = Buffer.alloc(24);
    out.writeUInt32LE(1, 0);
    out.writeUInt32LE(7, 4);
    out.writeUInt32LE(64, 8);
    out.writeUInt32LE(joinedHandle, 12);
    out.writeUInt32LE(8, 16);
    out.writeUInt32LE(0, 20);
    return out;
}

function commandResult(seq, found = true) {
    const out = Buffer.alloc(5);
    out.writeUInt32LE(seq, 0);
    out.writeUInt8(found ? 1 : 0, 4);
    return out;
}

function commandStarted(seq) {
    const out = Buffer.alloc(4);
    out.writeUInt32LE(seq, 0);
    return out;
}

function autoBattleKickResult(seq, {
    succeeded = true,
    controllerGateBefore = 0,
    kickStateBefore = 0,
    kickStateAfter = 1,
    sideSelector = 1,
    flag38Before = 0,
    flag38After = 1,
    flag39Before = 0,
    flag39After = 0,
    memberFunction = 0x00635509,
} = {}) {
    const out = Buffer.alloc(17);
    out.writeUInt32LE(seq, 0);
    out.writeUInt8(succeeded ? 1 : 0, 4);
    out.writeUInt8(controllerGateBefore, 5);
    out.writeUInt8(kickStateBefore, 6);
    out.writeUInt8(kickStateAfter, 7);
    out.writeUInt8(sideSelector, 8);
    out.writeUInt8(flag38Before, 9);
    out.writeUInt8(flag38After, 10);
    out.writeUInt8(flag39Before, 11);
    out.writeUInt8(flag39After, 12);
    out.writeUInt32LE(memberFunction, 13);
    return out;
}

async function freePort() {
    const server = net.createServer();
    await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', resolve);
    });
    const port = server.address().port;
    await new Promise((resolve) => server.close(resolve));
    return port;
}

async function waitFor(predicate, timeoutMs, description) {
    const deadline = Date.now() + timeoutMs;
    let lastError = null;
    while (Date.now() < deadline) {
        try {
            if (await predicate()) return;
        } catch (error) {
            lastError = error;
        }
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error(`timed out waiting for ${description}${lastError ? `: ${lastError.message}` : ''}`);
}

function requestJson(base, requestPath, method = 'GET', body = undefined) {
    const url = new URL(requestPath, base);
    return new Promise((resolve, reject) => {
        const request = http.request(url, {
            method, agent: false, headers: { Connection: 'close' },
        }, (response) => {
            const chunks = [];
            response.on('data', (chunk) => chunks.push(chunk));
            response.on('end', () => {
                try {
                    const text = Buffer.concat(chunks).toString('utf8');
                    resolve({ status: response.statusCode, body: JSON.parse(text) });
                } catch (error) {
                    reject(error);
                }
            });
        });
        request.on('error', reject);
        request.end(body);
    });
}

class FakeAgent {
    constructor(socket, autoAcknowledge) {
        this.socket = socket;
        this.autoAcknowledge = autoAcknowledge;
        this.received = [];
        this.buffered = Buffer.alloc(0);
        this.pairedEndTurnSeq = null;
        socket.on('error', () => {});
        socket.on('data', (chunk) => this.#onData(chunk));
    }

    #onData(chunk) {
        this.buffered = Buffer.concat([this.buffered, chunk]);
        while (this.buffered.length >= 8) {
            const length = this.buffered.readUInt32LE(0);
            if (this.buffered.length < 4 + length) return;
            const message = {
                op: this.buffered.readUInt16LE(4),
                flags: this.buffered.readUInt16LE(6),
                payload: Buffer.from(this.buffered.subarray(8, 4 + length)),
            };
            this.buffered = this.buffered.subarray(4 + length);
            this.received.push(message);
            if (this.autoAcknowledge && message.op === Op.EnableAutoBattle) {
                const seq = message.payload.readUInt32LE(0);
                this.send(Op.AutoBattleKickResult, autoBattleKickResult(seq));
            } else if (this.autoAcknowledge && message.op === Op.MoveStack) {
                const seq = message.payload.readUInt32LE(0);
                this.send(Op.CommandStarted, commandStarted(seq));
                this.send(Op.CommandResult, commandResult(seq));
            } else if (this.autoAcknowledge
                && message.op === Op.InvokePairedEndTurn) {
                const seq = message.payload.readUInt32LE(0);
                assert.equal(this.pairedEndTurnSeq, null,
                    'fake agent cannot arm two paired EndTurn commands');
                this.pairedEndTurnSeq = seq;
                this.send(Op.CommandStarted, commandStarted(seq));
            } else if (this.autoAcknowledge
                && message.op === Op.ReleasePairedEndTurn) {
                const seq = message.payload.readUInt32LE(0);
                assert.equal(seq, this.pairedEndTurnSeq,
                    'paired EndTurn release must name the exact armed command');
                this.pairedEndTurnSeq = null;
                this.send(Op.CommandResult, commandResult(seq));
            } else if (this.autoAcknowledge && mutationOps.has(message.op)) {
                const seq = message.payload.readUInt32LE(0);
                this.send(Op.CommandResult, commandResult(seq));
            }
        }
    }

    send(op, payload = Buffer.alloc(0), flags = 0) {
        this.socket.write(frame(op, payload, flags));
    }

    count(op) {
        return this.received.filter((message) => message.op === op).length;
    }

    last(op) {
        return this.received.filter((message) => message.op === op).at(-1);
    }
}

async function startRelay(t) {
    const httpPort = await freePort();
    const tcpPort = await freePort();
    const runId = `${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    const child = spawn(process.execPath, [relayScript], {
        env: {
            ...process.env,
            D2TESTDRV_HTTP_HOST: '127.0.0.1',
            D2TESTDRV_HTTP_PORT: String(httpPort),
            D2TESTDRV_BRIDGE_TCP_HOST: '127.0.0.1',
            D2TESTDRV_BRIDGE_TCP_PORT: String(tcpPort),
            D2TESTDRV_RUN_ID: runId,
        },
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk.toString(); });
    child.stderr.on('data', (chunk) => { output += chunk.toString(); });
    const agents = [];
    t.after(async () => {
        if (child.exitCode === null) child.kill('SIGTERM');
        await new Promise((resolve) => {
            if (child.exitCode !== null) return resolve();
            child.once('exit', resolve);
        });
        for (const agent of agents) {
            if (!agent.socket.destroyed) agent.socket.destroy();
        }
    });
    await waitFor(() => output.includes(`[tcp] agent server on 127.0.0.1:${tcpPort}`)
        && output.includes(`[http] api on http://127.0.0.1:${httpPort}`), 5000,
    'relay listeners');
    const base = `http://127.0.0.1:${httpPort}`;
    const status = await requestJson(base, '/api/status');
    assert.equal(status.status, 200);
    assert.equal(status.body.instanceId, runId);
    assert.equal(status.body.agentListening, true);

    async function connect({ role, pid, version = 7, autoAcknowledge = true }) {
        const socket = net.createConnection({ host: '127.0.0.1', port: tcpPort });
        await new Promise((resolve, reject) => {
            socket.once('connect', resolve);
            socket.once('error', reject);
        });
        const agent = new FakeAgent(socket, autoAcknowledge);
        agents.push(agent);
        agent.send(Op.Hello, hello(role, pid, version));
        return agent;
    }

    return { base, child, connect, output: () => output };
}

async function waitForTerminal(base, pattern) {
    let terminal;
    await waitFor(async () => {
        const status = await requestJson(base, '/api/status');
        terminal = status.body.terminalFault;
        return pattern.test(terminal?.reason || '');
    }, 2000, `terminal fault ${pattern}`);
    return terminal;
}

function autoBattleRequestPath(role, appearance, owner) {
    return '/api/ui/enable-auto-battle'
        + `?role=${role}&dlg=DLG_BATTLE_A&tog=TOG_AUTOBATTLE`
        + `&appearance=${appearance}&instance=${owner}`;
}

function endTurnPairRequestPath({
    hostAppearance, hostInstance, hostUi,
    joinAppearance, joinInstance, joinUi,
    waitMs = 2000, timeoutMs = 1000,
}) {
    return '/api/ui/end-turn-pair-when-strategic-idle'
        + `?hostappearance=${hostAppearance}&hostinstance=${hostInstance}&hostui=${hostUi}`
        + `&joinappearance=${joinAppearance}&joininstance=${joinInstance}&joinui=${joinUi}`
        + `&waitMs=${waitMs}&timeoutMs=${timeoutMs}`;
}

function decodeInvokeButton(message, expectedOp = Op.InvokeButton) {
    assert.equal(message.op, expectedOp);
    const payload = message.payload;
    assert.ok(payload.length >= 16, 'InvokeButton payload is too short');
    let offset = 12;
    const readString = () => {
        assert.ok(offset + 2 <= payload.length, 'InvokeButton string length is truncated');
        const length = payload.readUInt16LE(offset);
        offset += 2;
        assert.ok(offset + length <= payload.length, 'InvokeButton string is truncated');
        const value = payload.subarray(offset, offset + length).toString('utf8');
        offset += length;
        return value;
    };
    const decoded = {
        seq: payload.readUInt32LE(0),
        appearance: payload.readUInt32LE(4),
        instance: payload.readUInt32LE(8),
        dialog: readString(),
        button: readString(),
    };
    if (expectedOp === Op.InvokePairedEndTurn) {
        assert.ok(offset + 4 <= payload.length,
            'InvokePairedEndTurn command timeout is truncated');
        decoded.timeoutMs = payload.readUInt32LE(offset);
        offset += 4;
    }
    assert.equal(offset, payload.length, 'InvokeButton payload has trailing bytes');
    return decoded;
}

async function publishReadyBattle(relay, agent, role, appearance, owner) {
    agent.send(Op.UiSnapshot, uiSnapshot(appearance, owner, { dialog: 'DLG_BATTLE_A' }));
    await waitFor(async () => {
        const state = await requestJson(relay.base, '/api/state');
        const peer = state.body.roles?.[role];
        return peer?.dialogAppearance === appearance
            && peer.targets?.length === 1 && peer.targets[0].instance === owner;
    }, 2000, `${role} exact battle appearance`);
}

test('host LegacyStacksSnapshot replaces one immutable authoritative endpoint projection',
    { timeout: 10000 }, async (t) => {
        assert.equal(Op.LegacyStacksSnapshot, 0x0412);
        const relaySource = fs.readFileSync(relayScript, 'utf8');
        assert.match(relaySource,
            /stacks\.push\(Object\.freeze\([\s\S]*return Object\.freeze\(stacks\);/,
            'decoded stack records and their latest array must be immutable');
        assert.match(relaySource,
            /const snapshot = Object\.freeze\([\s\S]*state\.legacyStacksSnapshot = snapshot;/,
            'the published latest snapshot must be replaced only as one immutable value');

        const relay = await startRelay(t);
        const unpublished = await requestJson(relay.base, '/api/legacy-stacks');
        assert.equal(unpublished.status, 200);
        assert.deepEqual(unpublished.body, {
            sourceRole: 'host', sequence: 0, stacks: [],
        });
        const roleQuery = await requestJson(relay.base, '/api/legacy-stacks?role=host');
        assert.equal(roleQuery.status, 400);
        assert.match(roleQuery.body.error, /unexpected query parameter role/);

        const host = await relay.connect({ role: 'host', pid: 4242 });
        await waitFor(() => host.count(Op.HelloAck) === 1, 2000, 'host HelloAck');
        const firstPayload = legacyStacksSnapshot([
            {
                id: 0x0000000a, owner: 0xffffffff,
                x: -2147483648, y: 2147483647, movement: 0xffffffff,
            },
            { id: 0xa3e30000, owner: 0xa3de0001, x: 27, y: -16, movement: 15 },
        ]);
        assert.equal(firstPayload.length, 44);
        assert.equal(firstPayload.readUInt32LE(0), 2);
        assert.deepEqual([
            firstPayload.readUInt32LE(4), firstPayload.readUInt32LE(8),
            firstPayload.readInt32LE(12), firstPayload.readInt32LE(16),
            firstPayload.readUInt32LE(20),
        ], [0x0000000a, 0xffffffff, -2147483648, 2147483647, 0xffffffff]);
        host.send(Op.LegacyStacksSnapshot, firstPayload);

        let first;
        await waitFor(async () => {
            first = await requestJson(relay.base, '/api/legacy-stacks');
            return first.body.sequence === 1;
        }, 2000, 'first host-authoritative legacy stack snapshot');
        assert.deepEqual(first.body, {
            sourceRole: 'host',
            sequence: 1,
            stacks: [
                {
                    id: '0x0000000A', owner: '0xFFFFFFFF',
                    x: -2147483648, y: 2147483647, movement: 0xffffffff,
                },
                {
                    id: '0xA3E30000', owner: '0xA3DE0001',
                    x: 27, y: -16, movement: 15,
                },
            ],
        });

        host.send(Op.LegacyStacksSnapshot, legacyStacksSnapshot([
            { id: 1, owner: 2, x: -7, y: 9, movement: 0 },
        ]));
        let second;
        await waitFor(async () => {
            second = await requestJson(relay.base, '/api/legacy-stacks');
            return second.body.sequence === 2;
        }, 2000, 'replacement host-authoritative legacy stack snapshot');
        assert.deepEqual(second.body, {
            sourceRole: 'host', sequence: 2,
            stacks: [{ id: '0x00000001', owner: '0x00000002', x: -7, y: 9, movement: 0 }],
        });

        const maximum = Array.from({ length: 256 }, (_, index) => ({
            id: index + 1, owner: 0xa3de0001, x: index - 128, y: 128 - index,
            movement: index,
        }));
        host.send(Op.LegacyStacksSnapshot, legacyStacksSnapshot(maximum));
        let boundary;
        await waitFor(async () => {
            boundary = await requestJson(relay.base, '/api/legacy-stacks');
            return boundary.body.sequence === 3;
        }, 2000, 'maximum-size host-authoritative legacy stack snapshot');
        assert.equal(boundary.body.stacks.length, 256);
        assert.deepEqual(boundary.body.stacks[0], {
            id: '0x00000001', owner: '0xA3DE0001', x: -128, y: 128, movement: 0,
        });
        assert.deepEqual(boundary.body.stacks[255], {
            id: '0x00000100', owner: '0xA3DE0001', x: 127, y: -127, movement: 255,
        });
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

for (const scenario of [
    {
        name: 'missing u32 count', payload: () => Buffer.alloc(3),
        reason: /malformed LegacyStacksSnapshot: payload must contain its u32 count, got 3 bytes/,
    },
    {
        name: 'count above 256', payload: () => legacyStacksSnapshot(new Array(257).fill({
            id: 1, owner: 2, x: 3, y: 4, movement: 5,
        })),
        reason: /malformed LegacyStacksSnapshot: count 257 exceeds maximum 256/,
    },
    {
        name: 'truncated record', payload: () => {
            const payload = Buffer.alloc(23);
            payload.writeUInt32LE(1, 0);
            return payload;
        },
        reason: /malformed LegacyStacksSnapshot: payload for count 1 must be exactly 24 bytes, got 23/,
    },
    {
        name: 'trailing byte', payload: () => Buffer.alloc(5),
        reason: /malformed LegacyStacksSnapshot: payload for count 0 must be exactly 4 bytes, got 5/,
    },
]) {
    test(`LegacyStacksSnapshot ${scenario.name} is terminal and unpublished`,
        { timeout: 10000 }, async (t) => {
            const relay = await startRelay(t);
            const host = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => host.count(Op.HelloAck) === 1, 2000, 'host HelloAck');
            host.send(Op.LegacyStacksSnapshot, scenario.payload());
            await waitForTerminal(relay.base, scenario.reason);
            assert.deepEqual((await requestJson(relay.base, '/api/legacy-stacks')).body, {
                sourceRole: 'host', sequence: 0, stacks: [],
            });
        });
}

test('join LegacyStacksSnapshot publication is terminal and cannot become a fallback source',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const join = await relay.connect({ role: 'join', pid: 4343 });
        await waitFor(() => join.count(Op.HelloAck) === 1, 2000, 'join HelloAck');
        join.send(Op.WorldSnapshot, worldSnapshot(1, 99));
        await waitFor(async () => (await requestJson(relay.base, '/api/world?role=join'))
            .body.worldSeq === 1, 2000, 'join world snapshot');
        assert.deepEqual((await requestJson(relay.base, '/api/legacy-stacks')).body, {
            sourceRole: 'host', sequence: 0, stacks: [],
        }, 'the fixed endpoint must not fall back to a join world snapshot');

        join.send(Op.LegacyStacksSnapshot, legacyStacksSnapshot([
            { id: 1, owner: 2, x: 3, y: 4, movement: 5 },
        ]));
        await waitForTerminal(relay.base,
            /LegacyStacksSnapshot is host-authoritative; role join cannot publish it/);
        assert.deepEqual((await requestJson(relay.base, '/api/legacy-stacks')).body, {
            sourceRole: 'host', sequence: 0, stacks: [],
        });
    });

test('paired startup waits for both current loaded maps and releases each exact process once',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242 });
        const join = await relay.connect({ role: 'join', pid: 4343 });
        const publish = async (agent, role, appearance, mapLoaded, held = true) => {
            agent.send(Op.UiSnapshot, uiSnapshot(appearance, appearance + 100, {
                // A modal and inactive turn must not prevent a load barrier.
                dialog: 'DLG_BEGIN_TURN', strategicIdle: false,
                mapLoaded, startupActionsHeld: held,
            }));
            await waitFor(async () => {
                const { body } = await requestJson(relay.base, '/api/state');
                return body.roles[role]?.dialogAppearance === appearance;
            }, 2000, `${role} load observation ${appearance}`);
        };
        await publish(host, 'host', 1, true);
        await publish(join, 'join', 1, false);
        assert.equal(host.count(Op.ReleaseStartupActions), 0);
        assert.equal(join.count(Op.ReleaseStartupActions), 0);
        // Losing the first loaded map revokes readiness; no ever-ready latch.
        await publish(host, 'host', 2, false);
        await publish(join, 'join', 2, true);
        assert.equal(host.count(Op.ReleaseStartupActions), 0);
        assert.equal(join.count(Op.ReleaseStartupActions), 0);
        await publish(host, 'host', 3, true);
        await waitFor(() => host.count(Op.ReleaseStartupActions) === 1
            && join.count(Op.ReleaseStartupActions) === 1, 2000, 'paired startup release');
        assert.equal(host.last(Op.ReleaseStartupActions).payload.length, 0);
        assert.equal(join.last(Op.ReleaseStartupActions).payload.length, 0);
        const release = (await requestJson(relay.base, '/api/status')).body.startupActionsRelease;
        assert.equal(release.host.pid, 4242);
        assert.equal(release.join.pid, 4343);
        const history = (await requestJson(relay.base, '/api/ui/history')).body.events;
        for (const role of ['host', 'join']) {
            const proof = history.find((item) => item.seq === release[role].uiSeq);
            assert.equal(proof.role, role);
            assert.equal(proof.mapLoaded, true);
            assert.equal(proof.startupActionsHeld, true);
        }
        await publish(host, 'host', 4, true);
        await publish(join, 'join', 3, true);
        await publish(host, 'host', 5, true, false);
        await publish(join, 'join', 4, true, false);
        assert.equal(host.count(Op.ReleaseStartupActions), 1);
        assert.equal(join.count(Op.ReleaseStartupActions), 1);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

test('startup barrier never releases a single opted-in client or a disconnected pair',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242 });
        const join = await relay.connect({ role: 'join', pid: 4343 });
        host.send(Op.UiSnapshot, uiSnapshot(1, 101, {
            mapLoaded: true, startupActionsHeld: true,
        }));
        join.send(Op.UiSnapshot, uiSnapshot(1, 201, { mapLoaded: true }));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles.join?.mapLoaded === true, 2000, 'non-opted-in join');
        assert.equal(host.count(Op.ReleaseStartupActions), 0);
        assert.equal(join.count(Op.ReleaseStartupActions), 0);
        join.socket.destroy();
        await waitForTerminal(relay.base, /join.*disconnected/);
        assert.equal(host.count(Op.ReleaseStartupActions), 0);
        assert.equal((await requestJson(relay.base, '/api/status')).body.startupActionsRelease, null);
    });

test('startup lifecycle publication requires typed readiness, not a dialog-name guess',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242 });
        const snapshot = JSON.parse(uiSnapshot(1, 101).toString());
        delete snapshot.mapLoaded;
        host.send(Op.UiSnapshot, Buffer.from(JSON.stringify(snapshot)));
        await waitForTerminal(relay.base, /UI snapshot shape is invalid/);
        assert.equal(host.count(Op.ReleaseStartupActions), 0);
    });

test('native HelloAck -> alive Log -> first UI snapshot remains one causal connection',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4242 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v7 HelloAck');

        const alive = Buffer.from('mss32 testdrv bridge alive', 'utf8');
        assert.equal(alive.length, 26);
        agent.send(Op.Log, alive);
        agent.send(Op.UiSnapshot, uiSnapshot(1, 101));

        await waitFor(async () => {
            const state = await requestJson(relay.base, '/api/state');
            return state.body.roles?.host?.dialogAppearance === 1
                && state.body.roles.host.targets?.[0]?.instance === 101;
        }, 2000, 'first UI publication after native alive Log');
        const status = await requestJson(relay.base, '/api/status');
        assert.equal(status.body.terminalFault, null);
        assert.match(relay.output(), /\[dll:host\] "mss32 testdrv bridge alive"/);
    });

test('one passive UI-ready subscription may precede Hello and admits only the exact enabled button',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const path = '/api/ui/wait-ready?role=host&dlg=DLG_MAIN_MENU&btn=BTN_MULTI'
            + '&after=0&waitMs=2000';
        let settled = false;
        const pending = requestJson(relay.base, path);
        pending.finally(() => { settled = true; });
        await waitFor(() => relay.output().includes(
            '[ui-ready-wait] armed role=host DLG_MAIN_MENU::BTN_MULTI after=0'),
        2000, 'pre-Hello UI-ready subscription');

        const agent = await relay.connect({ role: 'host', pid: 4242 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v7 HelloAck');
        const initialMutations = agent.received.filter(
            (message) => mutationOps.has(message.op)).length;

        agent.send(Op.UiSnapshot, mainMenuUiSnapshot(
            1, 101, { dialog: 'DLG_PROTOCOL' }));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 1, 2000, 'wrong-dialog publication');
        await new Promise((resolve) => setTimeout(resolve, 20));
        assert.equal(settled, false);

        agent.send(Op.UiSnapshot, mainMenuUiSnapshot(2, 202, { ready: false }));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 2, 2000, 'unready main-menu publication');
        await new Promise((resolve) => setTimeout(resolve, 20));
        assert.equal(settled, false);

        agent.send(Op.UiSnapshot, mainMenuUiSnapshot(3, 303, { enabled: false }));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 3, 2000, 'disabled main-menu publication');
        await new Promise((resolve) => setTimeout(resolve, 20));
        assert.equal(settled, false);

        agent.send(Op.UiSnapshot, mainMenuUiSnapshot(4, 404));
        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.timedOut, false);
        assert.equal(response.body.terminalFault, null);
        assert.equal(response.body.latestSeq, 4);
        assert.deepEqual({
            role: response.body.observation.role,
            connected: response.body.observation.connected,
            pid: response.body.observation.pid,
            modulePath: response.body.observation.modulePath,
            dialog: response.body.observation.dialog,
            dialogReady: response.body.observation.dialogReady,
            dialogInstance: response.body.observation.dialogInstance,
            dialogAppearance: response.body.observation.dialogAppearance,
            uiSeq: response.body.observation.uiSeq,
        }, {
            role: 'host', connected: true, pid: 4242,
            modulePath: 'C:\\fixture\\mss32.dll', dialog: 'DLG_MAIN_MENU',
            dialogReady: true, dialogInstance: 4, dialogAppearance: 4, uiSeq: 4,
        });
        assert.equal(response.body.observation.targets.length, 1);
        assert.equal(response.body.observation.targets[0].instance, 404);
        assert.equal(response.body.observation.targets[0].widgets[0].name, 'BTN_MULTI');
        assert.equal(response.body.observation.targets[0].widgets[0].state.enabled, true);
        assert.equal(agent.received.filter(
            (message) => mutationOps.has(message.op)).length, initialMutations,
        'a passive readiness subscription must never emit a mutation');

        const immediate = await requestJson(relay.base,
            '/api/ui/wait-ready?role=host&dlg=DLG_MAIN_MENU&btn=BTN_MULTI'
            + '&after=3&waitMs=2000');
        assert.equal(immediate.status, 200);
        assert.equal(immediate.body.timedOut, false);
        assert.equal(immediate.body.observation.uiSeq, 4,
            'an already-current exact ready publication is returned without a lost wakeup');
        assert.equal(agent.received.filter(
            (message) => mutationOps.has(message.op)).length, initialMutations);
    });

test('a passive UI-ready timeout is nonterminal and cannot react to later UI',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const response = await requestJson(relay.base,
            '/api/ui/wait-ready?role=host&dlg=DLG_MAIN_MENU&btn=BTN_MULTI'
            + '&after=0&waitMs=30');
        assert.equal(response.status, 200);
        assert.equal(response.body.timedOut, true);
        assert.equal(response.body.observation, null);
        assert.equal(response.body.terminalFault, null);

        const agent = await relay.connect({ role: 'host', pid: 4243 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'post-timeout HelloAck');
        agent.send(Op.UiSnapshot, mainMenuUiSnapshot(1, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 1, 2000, 'post-timeout ready UI publication');
        assert.equal(agent.received.filter(
            (message) => mutationOps.has(message.op)).length, 0);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

test('a relay fault releases a passive UI-ready subscription without a mutation',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const pending = requestJson(relay.base,
            '/api/ui/wait-ready?role=host&dlg=DLG_MAIN_MENU&btn=BTN_MULTI'
            + '&after=0&waitMs=2000');
        await waitFor(() => relay.output().includes(
            '[ui-ready-wait] armed role=host DLG_MAIN_MENU::BTN_MULTI after=0'),
        2000, 'fault-bound UI-ready subscription');
        const agent = await relay.connect({ role: 'host', pid: 4244 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'fault test HelloAck');
        agent.socket.destroy();
        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.timedOut, false);
        assert.equal(response.body.observation, null);
        assert.match(response.body.terminalFault.reason,
            /agent host pid=4244 disconnected/);
        assert.equal(agent.received.filter(
            (message) => mutationOps.has(message.op)).length, 0);
    });

test('one exact world-pair subscription waits for both current post-watermark worlds',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242 });
        const join = await relay.connect({ role: 'join', pid: 4343 });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'world-pair HelloAck frames');

        const hero = '0xA3E30001';
        host.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 18, 27, 26));
        join.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 18, 27, 26));
        let hostAfter;
        let joinAfter;
        await waitFor(async () => {
            hostAfter = (await requestJson(relay.base, '/api/world?role=host')).body.worldSeq;
            joinAfter = (await requestJson(relay.base, '/api/world?role=join')).body.worldSeq;
            return hostAfter > 0 && joinAfter > 0;
        }, 2000, 'world-pair initial watermarks');
        const initialMutations = host.received.concat(join.received)
            .filter((message) => mutationOps.has(message.op)).length;

        const path = '/api/world/wait-exact-pair'
            + `?hostAfter=${hostAfter}&joinAfter=${joinAfter}`
            + `&id=${hero}&x=19&y=27&mp=23&waitMs=2000`;
        let settled = false;
        const pending = requestJson(relay.base, path);
        pending.finally(() => { settled = true; });
        await waitFor(() => relay.output().includes(
            `[world-pair-wait] armed id=${hero} @(19,27)/MP23`),
        2000, 'exact world-pair subscription');

        host.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 19, 27, 23));
        await waitFor(async () => (await requestJson(relay.base, '/api/world?role=host'))
            .body.worldSeq > hostAfter, 2000, 'host first exact world');
        await new Promise((resolve) => setTimeout(resolve, 20));
        assert.equal(settled, false, 'host alone cannot satisfy the pair');

        host.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 18, 27, 26));
        await waitFor(async () => (await requestJson(relay.base, '/api/world?role=host'))
            .body.stacks[0]?.x === 18, 2000, 'host exact-state regression');
        join.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 19, 27, 23));
        await waitFor(async () => (await requestJson(relay.base, '/api/world?role=join'))
            .body.worldSeq > joinAfter, 2000, 'join exact world');
        await new Promise((resolve) => setTimeout(resolve, 20));
        assert.equal(settled, false,
            'a historical host match cannot latch across a current-state regression');

        host.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 19, 27, 23));
        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.timedOut, false);
        assert.equal(response.body.terminalFault, null);
        assert.deepEqual({
            id: response.body.observation.id,
            x: response.body.observation.x,
            y: response.body.observation.y,
            movement: response.body.observation.movement,
        }, { id: hero, x: 19, y: 27, movement: 23 });
        for (const [role, after] of [['host', hostAfter], ['join', joinAfter]]) {
            const observed = response.body.observation[role];
            assert.equal(observed.role, role);
            assert.equal(observed.connected, true);
            assert.ok(observed.worldSeq > after);
            assert.deepEqual(observed.stack, { id: hero, x: 19, y: 27, movement: 23 });
            assert.ok(response.body.latestSeq >= observed.worldSeq);
        }
        assert.equal(host.received.concat(join.received)
            .filter((message) => mutationOps.has(message.op)).length, initialMutations,
        'the world-pair subscription must never emit a mutation');

        const immediate = await requestJson(relay.base, path);
        assert.equal(immediate.status, 200);
        assert.equal(immediate.body.timedOut, false);
        assert.equal(immediate.body.observation.host.stack.x, 19,
            'an already-current exact pair returns without a lost wakeup');
    });

test('an equal-watermark exact world pair times out once and cannot react later',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242 });
        const join = await relay.connect({ role: 'join', pid: 4343 });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'world-pair timeout HelloAck frames');
        const hero = '0xA3E30001';
        host.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 19, 27, 23));
        join.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 19, 27, 23));
        let hostAfter;
        let joinAfter;
        await waitFor(async () => {
            hostAfter = (await requestJson(relay.base, '/api/world?role=host')).body.worldSeq;
            joinAfter = (await requestJson(relay.base, '/api/world?role=join')).body.worldSeq;
            return hostAfter > 0 && joinAfter > 0;
        }, 2000, 'equal world-pair watermarks');
        const response = await requestJson(relay.base,
            '/api/world/wait-exact-pair'
            + `?hostAfter=${hostAfter}&joinAfter=${joinAfter}`
            + `&id=${hero}&x=19&y=27&mp=23&waitMs=30`);
        assert.equal(response.status, 200);
        assert.equal(response.body.timedOut, true);
        assert.equal(response.body.observation, null);
        assert.equal(response.body.terminalFault, null);

        host.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 19, 27, 23));
        join.send(Op.WorldSnapshot, exactWorldSnapshot(1, hero, 19, 27, 23));
        await waitFor(async () => (await requestJson(relay.base, '/api/world?role=join'))
            .body.worldSeq > joinAfter, 2000, 'post-timeout world publication');
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
        assert.equal(host.received.concat(join.received)
            .filter((message) => mutationOps.has(message.op)).length, 0);
    });

test('a relay fault releases an armed exact world-pair subscription without mutation',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242 });
        const join = await relay.connect({ role: 'join', pid: 4343 });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'world-pair fault HelloAck frames');
        const pending = requestJson(relay.base,
            '/api/world/wait-exact-pair?hostAfter=0&joinAfter=0'
            + '&id=0xA3E30001&x=19&y=27&mp=23&waitMs=2000');
        await waitFor(() => relay.output().includes('[world-pair-wait] armed'),
            2000, 'fault-bound world-pair subscription');
        host.socket.destroy();
        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.timedOut, false);
        assert.equal(response.body.observation, null);
        assert.match(response.body.terminalFault.reason,
            /agent host pid=4242 disconnected/);
        assert.equal(host.received.concat(join.received)
            .filter((message) => mutationOps.has(message.op)).length, 0);
    });

test('exact world-pair query rejects noncanonical targets without arming',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const base = '/api/world/wait-exact-pair?hostAfter=0&joinAfter=0'
            + '&id=0xA3E30001&x=19&y=27&mp=23&waitMs=30';
        const cases = [
            base.replace('0xA3E30001', '0xa3e30001'),
            base.replace('mp=23', 'mp=-1'),
            base.replace('mp=23', 'mp=256'),
            base.replace('hostAfter=0', 'hostAfter=-1'),
            base.replace('joinAfter=0', 'joinAfter=4294967296'),
            base.replace('waitMs=30', 'waitMs=0'),
            base.replace('waitMs=30', 'waitMs=120001'),
            `${base}&extra=1`,
            `${base}&x=20`,
        ];
        for (const path of cases) {
            const response = await requestJson(relay.base, path);
            assert.equal(response.status, 400, path);
        }
        assert.doesNotMatch(relay.output(), /\[world-pair-wait\] armed/);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

test('paired End Turn waits for both exact strategic-idle events and dispatches once',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({
            role: 'host', pid: 4242, autoAcknowledge: false,
        });
        const join = await relay.connect({
            role: 'join', pid: 4343, autoAcknowledge: false,
        });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'paired v7 HelloAck frames');

        host.send(Op.UiSnapshot, isoPalWithStrategicTarget(11, 111, 911));
        join.send(Op.UiSnapshot, strategicUiSnapshot(22, 922));
        let initialState;
        await waitFor(async () => {
            const state = await requestJson(relay.base, '/api/state');
            const hostState = state.body.roles?.host;
            const joinState = state.body.roles?.join;
            if (hostState?.dialogAppearance !== 11 || joinState?.dialogAppearance !== 22
                || hostState.uiSeq < 1 || joinState.uiSeq < 1) return false;
            initialState = state.body;
            return true;
        }, 2000, 'initial exact busy pair');
        const hostUi = initialState.roles.host.uiSeq;
        const joinUi = initialState.roles.join.uiSeq;

        const requestPath = endTurnPairRequestPath({
            hostAppearance: 11, hostInstance: 911, hostUi,
            joinAppearance: 22, joinInstance: 922, joinUi,
        });
        let settled = false;
        const pending = requestJson(relay.base, requestPath, 'POST');
        pending.finally(() => { settled = true; });
        await waitFor(() => relay.output().includes(
            `[end-turn-pair] armed host=11/911 ui>=${hostUi}`
                + ` join=22/922 ui>=${joinUi}`),
        2000, 'armed paired End Turn');
        assert.equal(host.count(Op.InvokePairedEndTurn), 0);
        assert.equal(join.count(Op.InvokePairedEndTurn), 0);

        const duplicate = await requestJson(relay.base, requestPath, 'POST');
        assert.equal(duplicate.status, 409);
        assert.match(duplicate.body.error, /already armed/);
        assert.equal(host.count(Op.InvokePairedEndTurn), 0);
        assert.equal(join.count(Op.InvokePairedEndTurn), 0);

        host.send(Op.UiSnapshot, isoPalWithStrategicTarget(
            11, 111, 911, { strategicIdle: true }));
        let hostIdleUi;
        await waitFor(async () => {
            const published = (await requestJson(relay.base, '/api/ui?role=host')).body;
            if (published.uiSeq <= hostUi || published.strategicIdle !== true) return false;
            hostIdleUi = published.uiSeq;
            return true;
        }, 2000, 'host strategic-idle edge');
        assert.equal(host.count(Op.InvokePairedEndTurn), 0,
            'one idle role cannot receive its command early');
        assert.equal(join.count(Op.InvokePairedEndTurn), 0,
            'one idle role cannot release the other role');

        join.send(Op.UiSnapshot, strategicUiSnapshot(
            22, 922, { strategicIdle: true }));
        await waitFor(() => host.count(Op.InvokePairedEndTurn) === 1
            && join.count(Op.InvokePairedEndTurn) === 1, 2000,
        'one back-to-back paired arm dispatch');
        assert.equal(settled, false,
            'the HTTP result cannot complete before both native command results');

        const hostInvoke = decodeInvokeButton(
            host.last(Op.InvokePairedEndTurn), Op.InvokePairedEndTurn);
        const joinInvoke = decodeInvokeButton(
            join.last(Op.InvokePairedEndTurn), Op.InvokePairedEndTurn);
        assert.deepEqual(hostInvoke, {
            seq: hostInvoke.seq, appearance: 11, instance: 911,
            dialog: 'DLG_STRATEGIC', button: 'BTN_END_TURN',
            timeoutMs: 1000,
        });
        assert.deepEqual(joinInvoke, {
            seq: joinInvoke.seq, appearance: 22, instance: 922,
            dialog: 'DLG_STRATEGIC', button: 'BTN_END_TURN',
            timeoutMs: 1000,
        });
        assert.notEqual(hostInvoke.seq, joinInvoke.seq);
        assert.equal(hostInvoke.instance, 911,
            'the co-present strategic owner must be used instead of DLG_ISO_PAL owner 111');

        host.send(Op.CommandStarted, commandStarted(hostInvoke.seq));
        await new Promise((resolve) => setImmediate(resolve));
        assert.equal(host.count(Op.ReleasePairedEndTurn), 0);
        assert.equal(join.count(Op.ReleasePairedEndTurn), 0,
            'one armed UI thread cannot release either callback');
        join.send(Op.CommandStarted, commandStarted(joinInvoke.seq));
        await waitFor(() => host.count(Op.ReleasePairedEndTurn) === 1
            && join.count(Op.ReleasePairedEndTurn) === 1, 2000,
        'both exact UI-thread release frames');
        assert.equal(host.last(Op.ReleasePairedEndTurn).payload.readUInt32LE(0),
            hostInvoke.seq);
        assert.equal(join.last(Op.ReleasePairedEndTurn).payload.readUInt32LE(0),
            joinInvoke.seq);

        host.send(Op.CommandResult, commandResult(hostInvoke.seq));
        await new Promise((resolve) => setImmediate(resolve));
        assert.equal(settled, false,
            'one native result cannot complete the paired request');
        join.send(Op.CommandResult, commandResult(joinInvoke.seq));
        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.ok(Number.isFinite(response.body.dispatchSkewMs));
        assert.ok(response.body.dispatchSkewMs >= 0);
        assert.ok(Number.isFinite(response.body.armWriteSkewMs));
        assert.equal(response.body.host.armedAtMs > 0, true);
        assert.equal(response.body.join.armedAtMs > 0, true);
        assert.equal(response.body.host.uiSeq, hostIdleUi);
        assert.ok(response.body.join.uiSeq > joinUi);
        assert.equal(response.body.host.strategicIdle, true);
        assert.equal(response.body.join.strategicIdle, true);
        assert.deepEqual(response.body.host.invoke, {
            dlg: 'DLG_STRATEGIC', btn: 'BTN_END_TURN', appearance: 11, instance: 911,
        });
        assert.deepEqual(response.body.join.invoke, {
            dlg: 'DLG_STRATEGIC', btn: 'BTN_END_TURN', appearance: 22, instance: 922,
        });

        host.send(Op.UiSnapshot, isoPalWithStrategicTarget(11, 111, 911));
        join.send(Op.UiSnapshot, strategicUiSnapshot(22, 922));
        await waitFor(async () => {
            const state = (await requestJson(relay.base, '/api/state')).body;
            return state.roles?.host?.uiSeq > response.body.host.uiSeq
                && state.roles.host.strategicIdle === false
                && state.roles?.join?.uiSeq > response.body.join.uiSeq
                && state.roles.join.strategicIdle === false;
        }, 2000, 'later busy publications');
        assert.equal(host.count(Op.InvokePairedEndTurn), 1);
        assert.equal(join.count(Op.InvokePairedEndTurn), 1,
            'consumed paired intent cannot refire on later UI events');
    });

test('paired End Turn consumes an already-idle exact pair without a lost wakeup',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4245 });
        const join = await relay.connect({ role: 'join', pid: 4346 });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'already-idle v7 HelloAck frames');
        host.send(Op.UiSnapshot, strategicUiSnapshot(
            31, 931, { strategicIdle: true }));
        join.send(Op.UiSnapshot, isoPalWithStrategicTarget(
            42, 142, 942, { strategicIdle: true }));
        let publishedState;
        await waitFor(async () => {
            const state = await requestJson(relay.base, '/api/state');
            const hostState = state.body.roles?.host;
            const joinState = state.body.roles?.join;
            if (hostState?.dialogAppearance !== 31 || joinState?.dialogAppearance !== 42
                || hostState.uiSeq < 1 || joinState.uiSeq < 1) return false;
            publishedState = state.body;
            return true;
        }, 2000, 'already-idle exact pair');
        const hostUi = publishedState.roles.host.uiSeq;
        const joinUi = publishedState.roles.join.uiSeq;

        const response = await requestJson(relay.base, endTurnPairRequestPath({
            hostAppearance: 31, hostInstance: 931, hostUi,
            joinAppearance: 42, joinInstance: 942, joinUi,
        }), 'POST');
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.equal(response.body.host.uiSeq, hostUi);
        assert.equal(response.body.join.uiSeq, joinUi);
        assert.equal(host.count(Op.InvokePairedEndTurn), 1);
        assert.equal(join.count(Op.InvokePairedEndTurn), 1);
        assert.equal(host.count(Op.ReleasePairedEndTurn), 1);
        assert.equal(join.count(Op.ReleasePairedEndTurn), 1);
        assert.deepEqual(decodeInvokeButton(
            host.last(Op.InvokePairedEndTurn), Op.InvokePairedEndTurn), {
            seq: host.last(Op.InvokePairedEndTurn).payload.readUInt32LE(0),
            appearance: 31, instance: 931,
            dialog: 'DLG_STRATEGIC', button: 'BTN_END_TURN',
            timeoutMs: 1000,
        });
        assert.deepEqual(decodeInvokeButton(
            join.last(Op.InvokePairedEndTurn), Op.InvokePairedEndTurn), {
            seq: join.last(Op.InvokePairedEndTurn).payload.readUInt32LE(0),
            appearance: 42, instance: 942,
            dialog: 'DLG_STRATEGIC', button: 'BTN_END_TURN',
            timeoutMs: 1000,
        });
    });

test('paired End Turn rejects a wrong root and faults identity drift before any write',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4247 });
        const join = await relay.connect({ role: 'join', pid: 4348 });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'identity test v7 HelloAck frames');
        host.send(Op.UiSnapshot, beginTurnWithStrategicTarget(1, 101, 901));
        join.send(Op.UiSnapshot, strategicUiSnapshot(2, 902));
        let wrongRootState;
        await waitFor(async () => {
            const state = (await requestJson(relay.base, '/api/state')).body;
            if (state.roles?.host?.dialogAppearance !== 1
                || state.roles?.join?.dialogAppearance !== 2) return false;
            wrongRootState = state;
            return true;
        }, 2000, 'wrong-root pair publication');

        const wrongRoot = await requestJson(relay.base, endTurnPairRequestPath({
            hostAppearance: 1, hostInstance: 901, hostUi: wrongRootState.roles.host.uiSeq,
            joinAppearance: 2, joinInstance: 902, joinUi: wrongRootState.roles.join.uiSeq,
        }), 'POST');
        assert.equal(wrongRoot.status, 409);
        assert.match(wrongRoot.body.error, /host root strategic appearance drifted/);
        assert.equal(host.count(Op.InvokePairedEndTurn), 0);
        assert.equal(join.count(Op.InvokePairedEndTurn), 0);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null,
            'a stale request rejected before arming does not poison the relay');

        host.send(Op.UiSnapshot, strategicUiSnapshot(3, 903));
        let hostUi;
        await waitFor(async () => {
            const published = (await requestJson(relay.base, '/api/ui?role=host')).body;
            if (published.dialogAppearance !== 3) return false;
            hostUi = published.uiSeq;
            return true;
        }, 2000, 'correct busy root publication');
        const joinUi = wrongRootState.roles.join.uiSeq;
        const requestPath = endTurnPairRequestPath({
            hostAppearance: 3, hostInstance: 903, hostUi,
            joinAppearance: 2, joinInstance: 902, joinUi,
        });
        const pending = requestJson(relay.base, requestPath, 'POST');
        await waitFor(() => relay.output().includes(
            `[end-turn-pair] armed host=3/903 ui>=${hostUi}`
                + ` join=2/902 ui>=${joinUi}`),
        2000, 'identity-bound paired End Turn');
        host.send(Op.UiSnapshot, strategicUiSnapshot(
            4, 904, { strategicIdle: true }));
        const response = await pending;
        assert.equal(response.status, 500);
        assert.match(response.body.terminalFault.reason,
            /host root strategic appearance drifted from 3/);
        assert.equal(host.count(Op.InvokePairedEndTurn), 0);
        assert.equal(join.count(Op.InvokePairedEndTurn), 0,
            'identity drift is terminal before either role receives a command');
    });

test('paired End Turn timeout consumes the intent and ignores later idle publications',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4249 });
        const join = await relay.connect({ role: 'join', pid: 4350 });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'timeout test v7 HelloAck frames');
        host.send(Op.UiSnapshot, strategicUiSnapshot(51, 951));
        join.send(Op.UiSnapshot, strategicUiSnapshot(52, 952));
        let busyState;
        await waitFor(async () => {
            const state = (await requestJson(relay.base, '/api/state')).body;
            if (state.roles?.host?.dialogAppearance !== 51
                || state.roles?.join?.dialogAppearance !== 52) return false;
            busyState = state;
            return true;
        }, 2000, 'timeout busy pair');

        const response = await requestJson(relay.base, endTurnPairRequestPath({
            hostAppearance: 51, hostInstance: 951, hostUi: busyState.roles.host.uiSeq,
            joinAppearance: 52, joinInstance: 952, joinUi: busyState.roles.join.uiSeq,
            waitMs: 50,
        }), 'POST');
        assert.equal(response.status, 500);
        assert.match(response.body.terminalFault.reason,
            /paired strategic End Turn timed out after 50ms/);
        assert.equal(host.count(Op.InvokePairedEndTurn), 0);
        assert.equal(join.count(Op.InvokePairedEndTurn), 0);

        host.send(Op.UiSnapshot, strategicUiSnapshot(
            51, 951, { strategicIdle: true }));
        join.send(Op.UiSnapshot, strategicUiSnapshot(
            52, 952, { strategicIdle: true }));
        await new Promise((resolve) => setTimeout(resolve, 30));
        assert.equal(host.count(Op.InvokePairedEndTurn), 0);
        assert.equal(join.count(Op.InvokePairedEndTurn), 0,
            'late idle publications cannot revive a timed-out one-shot intent');
    });

test('one UI intent waits for the exact current ready dialog and invokes its captured owner once',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4242 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v7 HelloAck');

        agent.send(Op.UiSnapshot, beginTurnWithStrategicTarget(1, 101, 901));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 1, 2000, 'begin-turn UI publication');

        const path = '/api/ui/invoke-when-ready?role=host&dlg=DLG_STRATEGIC'
            + '&btn=BTN_END_TURN&after=1&waitMs=2000&timeoutMs=1000';
        const pending = requestJson(relay.base, path, 'POST');
        await waitFor(() => relay.output().includes(
            '[ui-intent] armed role=host DLG_STRATEGIC::BTN_END_TURN after=1'),
        2000, 'armed UI intent');
        assert.equal(agent.count(Op.InvokeButton), 0,
            'a co-present strategic target cannot bypass the current BeginTurn dialog');

        const duplicate = await requestJson(relay.base, path, 'POST');
        assert.equal(duplicate.status, 409);
        assert.match(duplicate.body.error, /already owns one pending UI invoke intent/);
        assert.equal(agent.count(Op.InvokeButton), 0);

        agent.send(Op.UiSnapshot, strategicUiSnapshot(2, 202, { ready: false }));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 2, 2000, 'unready strategic publication');
        assert.equal(agent.count(Op.InvokeButton), 0);

        agent.send(Op.UiSnapshot, strategicUiSnapshot(2, 202, { enabled: false }));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 3, 2000, 'disabled strategic publication');
        assert.equal(agent.count(Op.InvokeButton), 0);

        agent.send(Op.UiSnapshot, strategicUiSnapshot(2, 202));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 4, 2000, 'ready but native-busy strategic publication');
        assert.equal(agent.count(Op.InvokeButton), 0,
            'an enabled strategic button cannot bypass native-idle admission');

        agent.send(Op.UiSnapshot, strategicUiSnapshot(
            2, 202, { strategicIdle: true }));
        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.deepEqual(response.body.invoke, {
            dlg: 'DLG_STRATEGIC', btn: 'BTN_END_TURN', appearance: 2, instance: 202,
        });
        assert.equal(response.body.observation.role, 'host');
        assert.equal(response.body.observation.dialog, 'DLG_STRATEGIC');
        assert.equal(response.body.observation.dialogInstance, 2);
        assert.equal(response.body.observation.dialogAppearance, 2);
        assert.equal(response.body.observation.dialogReady, true);
        assert.equal(response.body.observation.strategicIdle, true);
        assert.equal(response.body.observation.uiSeq, 5);
        assert.equal(response.body.observation.targets[0].instance, 202);
        assert.equal(response.body.observation.targets[0].widgets[0].state.enabled, true);
        assert.equal(agent.count(Op.InvokeButton), 1);
        const wire = agent.last(Op.InvokeButton).payload;
        assert.equal(wire.readUInt32LE(4), 2);
        assert.equal(wire.readUInt32LE(8), 202);

        agent.send(Op.UiSnapshot, strategicUiSnapshot(
            3, 303, { strategicIdle: true }));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 6, 2000, 'later strategic publication');
        assert.equal(agent.count(Op.InvokeButton), 1,
            'a completed intent cannot resend on a later UI publication');

        const immediate = await requestJson(relay.base,
            '/api/ui/invoke-when-ready?role=host&dlg=DLG_STRATEGIC'
            + '&btn=BTN_END_TURN&after=5&waitMs=2000&timeoutMs=1000', 'POST');
        assert.equal(immediate.status, 200);
        assert.equal(immediate.body.found, true);
        assert.equal(immediate.body.observation.uiSeq, 6);
        assert.equal(immediate.body.invoke.appearance, 3);
        assert.equal(immediate.body.invoke.instance, 303);
        assert.equal(agent.count(Op.InvokeButton), 2,
            'an already-published exact snapshot dispatches immediately once');
    });

test('stable UI intent resets on unready or owner drift and dispatches one continuous owner once',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4251 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'stable-intent HelloAck');

        agent.send(Op.UiSnapshot, lobbyUiSnapshot(1, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 1, 2000, 'first ready lobby publication');

        const path = '/api/ui/invoke-when-ready?role=host&dlg=DLG_LOBBY'
            + '&btn=BTN_OK&after=0&waitMs=3000&stableMs=500&timeoutMs=1000';
        const pending = requestJson(relay.base, path, 'POST');
        await waitFor(() => relay.output().includes(
            '[ui-intent] candidate role=host DLG_LOBBY::BTN_OK appearance=1 owner=101 stableMs=500'),
        2000, 'first stable lobby candidate');

        const duplicate = await requestJson(relay.base, path, 'POST');
        assert.equal(duplicate.status, 409);
        assert.match(duplicate.body.error, /already owns one pending UI invoke intent/);
        assert.equal(agent.count(Op.InvokeButton), 0,
            'a duplicate logical request cannot create a second command');

        agent.send(Op.UiSnapshot, lobbyUiSnapshot(1, 101, { ready: false }));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 2, 2000, 'unready lobby reset publication');
        await new Promise((resolve) => setTimeout(resolve, 550));
        assert.equal(agent.count(Op.InvokeButton), 0,
            'an unready publication cancels the first stability timer without consuming the intent');

        agent.send(Op.UiSnapshot, lobbyUiSnapshot(2, 202));
        await waitFor(() => relay.output().includes(
            '[ui-intent] candidate role=host DLG_LOBBY::BTN_OK appearance=2 owner=202 stableMs=500'),
        2000, 'second stable lobby candidate');
        await new Promise((resolve) => setTimeout(resolve, 100));
        agent.send(Op.UiSnapshot, lobbyUiSnapshot(3, 303));
        const finalCandidateLog =
            '[ui-intent] candidate role=host DLG_LOBBY::BTN_OK'
            + ' appearance=3 owner=303 stableMs=500';
        await waitFor(() => relay.output().includes(finalCandidateLog),
            2000, 'drifted stable lobby candidate');
        const finalCandidateObservedAt = Date.now();
        await new Promise((resolve) => setTimeout(resolve, 250));
        assert.equal(agent.count(Op.InvokeButton), 0,
            'owner drift restarts the complete stability interval before any command');
        await new Promise((resolve) => setTimeout(resolve, 100));
        agent.send(Op.UiSnapshot, lobbyUiSnapshot(3, 303));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 5, 2000, 'same exact ready candidate republication');
        assert.equal(relay.output().split(finalCandidateLog).length - 1, 1,
            'the same exact ready appearance/owner cannot arm another candidate timer');

        const response = await pending;
        const stableElapsedMs = Date.now() - finalCandidateObservedAt;
        assert.ok(stableElapsedMs < 700,
            `same-owner republication extended the original 500ms interval to ${stableElapsedMs}ms`);
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.equal(response.body.observation.uiSeq, 5);
        assert.deepEqual(response.body.invoke, {
            dlg: 'DLG_LOBBY', btn: 'BTN_OK', appearance: 3, instance: 303,
        });
        assert.equal(agent.count(Op.InvokeButton), 1);
        const wire = decodeInvokeButton(agent.last(Op.InvokeButton));
        assert.equal(wire.appearance, 3);
        assert.equal(wire.instance, 303);

        agent.send(Op.UiSnapshot, lobbyUiSnapshot(4, 404));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 6, 2000, 'post-completion lobby publication');
        await new Promise((resolve) => setTimeout(resolve, 550));
        assert.equal(agent.count(Op.InvokeButton), 1,
            'a completed stable intent cannot revive on a later ready owner');
    });

test('stable UI intent rejects non-canonical, oversized, or non-shorter intervals',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4252 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000,
            'stable-validation HelloAck');
        agent.send(Op.UiSnapshot, lobbyUiSnapshot(1, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 1, 2000, 'stable-validation lobby publication');

        const base = '/api/ui/invoke-when-ready?role=host&dlg=DLG_LOBBY'
            + '&btn=BTN_OK&after=0&timeoutMs=1000';
        for (const suffix of [
            '&waitMs=1000&stableMs=0',
            '&waitMs=1000&stableMs=0500',
            '&waitMs=6000&stableMs=5001',
            '&waitMs=1000&stableMs=1000',
            '&waitMs=5000&stableMs=5000',
        ]) {
            const response = await requestJson(relay.base, base + suffix, 'POST');
            assert.equal(response.status, 400, `invalid stability interval ${suffix}`);
            assert.equal(agent.count(Op.InvokeButton), 0,
                'invalid stability input must not reach the native agent');
        }
    });

test('a UI intent timeout is terminal and cannot dispatch a later action',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4243 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        agent.send(Op.UiSnapshot, beginTurnWithStrategicTarget(1, 101, 901));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 1, 2000, 'begin-turn UI publication');

        const response = await requestJson(relay.base,
            '/api/ui/invoke-when-ready?role=host&dlg=DLG_STRATEGIC'
            + '&btn=BTN_END_TURN&after=1&waitMs=50&timeoutMs=1000', 'POST');
        assert.equal(response.status, 500);
        assert.match(response.body.terminalFault.reason,
            /UI invoke intent for role host timed out after 50ms/);
        await waitFor(() => agent.socket.destroyed, 2000,
            'timed-out UI intent owner socket destruction');
        assert.equal(agent.count(Op.InvokeButton), 0);
        const status = await requestJson(relay.base, '/api/status');
        assert.match(status.body.terminalFault.reason,
            /UI invoke intent for role host timed out after 50ms/);
    });

test('a relay fault cancels an armed UI intent without emitting an action',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4244 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        agent.send(Op.UiSnapshot, beginTurnWithStrategicTarget(1, 101, 901));
        await waitFor(async () => (await requestJson(relay.base, '/api/ui?role=host'))
            .body.uiSeq === 1, 2000, 'begin-turn UI publication');

        const pending = requestJson(relay.base,
            '/api/ui/invoke-when-ready?role=host&dlg=DLG_STRATEGIC'
            + '&btn=BTN_END_TURN&after=1&waitMs=2000&timeoutMs=1000', 'POST');
        await waitFor(() => relay.output().includes(
            '[ui-intent] armed role=host DLG_STRATEGIC::BTN_END_TURN after=1'),
        2000, 'armed UI intent');
        agent.socket.destroy();
        const response = await pending;
        assert.equal(response.status, 500);
        assert.match(response.body.terminalFault.reason, /agent host pid=4244 disconnected/);
        assert.equal(agent.count(Op.InvokeButton), 0);
    });

test('v7 binds each mutation to one appearance/owner and retains causal evidence',
    { timeout: 15000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4242 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        const ack = agent.last(Op.HelloAck);
        assert.equal(ack.flags, 0);
        assert.equal(ack.payload.length, 8);
        assert.equal(ack.payload.readUInt32LE(0), 1);
        assert.equal(ack.payload.readUInt32LE(4), 7);

        agent.send(Op.UiSnapshot, uiSnapshot(1, 101));
        await waitFor(async () => {
            const state = await requestJson(relay.base, '/api/state');
            return state.body.roles?.host?.dialogAppearance === 1
                && state.body.roles.host.targets?.[0]?.instance === 101;
        }, 2000, 'first exact UI publication');

        const invoke = await requestJson(relay.base,
            '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
            + '&appearance=1&instance=101', 'POST');
        assert.equal(invoke.status, 200);
        assert.equal(invoke.body.found, true);
        assert.equal(agent.count(Op.InvokeButton), 1);
        const invokeWire = agent.last(Op.InvokeButton).payload;
        assert.equal(invokeWire.readUInt32LE(4), 1);
        assert.equal(invokeWire.readUInt32LE(8), 101);

        const exactScenario = "C:\\fixture\\Exports\\'Battle for Wizgard 1.43x rus.sg";
        const scenario = await requestJson(relay.base,
            '/api/ui/select-scenario?role=host&dlg=DLG_EVENT_POPUP&lb=TLBOX_GAME_SLOT'
            + `&path=${encodeURIComponent(exactScenario)}&appearance=1&instance=101`, 'POST');
        assert.equal(scenario.status, 200);
        assert.equal(scenario.body.found, true);
        assert.equal(agent.count(Op.SelectScenarioPath), 1);
        const scenarioWire = agent.last(Op.SelectScenarioPath).payload;
        assert.equal(scenarioWire.readUInt32LE(4), 1);
        assert.equal(scenarioWire.readUInt32LE(8), 101);
        let offset = 12;
        const readString = () => {
            const length = scenarioWire.readUInt16LE(offset);
            offset += 2;
            const value = scenarioWire.subarray(offset, offset + length).toString('utf8');
            offset += length;
            return value;
        };
        assert.equal(readString(), 'DLG_EVENT_POPUP');
        assert.equal(readString(), 'TLBOX_GAME_SLOT');
        assert.equal(readString(), exactScenario);
        assert.equal(offset, scenarioWire.length);

        const enable = await requestJson(relay.base,
            '/api/ui/enable-toggle?role=host&dlg=DLG_EVENT_POPUP&tog=TOG_AUTOBATTLE'
            + '&appearance=1&instance=101', 'POST');
        assert.equal(enable.status, 200);
        assert.equal(enable.body.found, true);
        assert.equal(agent.count(Op.EnableToggle), 1);

        agent.send(Op.UiSnapshot, uiSnapshot(2, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 2, 2000, 'second appearance');
        const invokeCount = agent.count(Op.InvokeButton);
        const staleAppearance = await requestJson(relay.base,
            '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
            + '&appearance=1&instance=101', 'POST');
        assert.equal(staleAppearance.status, 409);
        assert.equal(agent.count(Op.InvokeButton), invokeCount);

        agent.send(Op.UiSnapshot, uiSnapshot(3, 102));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 3, 2000, 'replacement owner');
        const staleOwner = await requestJson(relay.base,
            '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
            + '&appearance=3&instance=101', 'POST');
        assert.equal(staleOwner.status, 409);
        assert.equal(agent.count(Op.InvokeButton), invokeCount);

        agent.send(Op.UiSnapshot, uiSnapshot(4, 103, { enabled: false }));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 4, 2000, 'disabled publication');
        const disabled = await requestJson(relay.base,
            '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
            + '&appearance=4&instance=103', 'POST');
        assert.equal(disabled.status, 409);
        assert.equal(agent.count(Op.InvokeButton), invokeCount);

        const uiHistory = await requestJson(relay.base, '/api/ui/history?role=host&after=1');
        assert.equal(uiHistory.status, 200);
        assert.deepEqual(uiHistory.body.events.map((entry) => entry.dialogAppearance), [2, 3, 4]);

        agent.send(Op.WorldSnapshot, worldSnapshot(1, 27));
        agent.send(Op.WorldSnapshot, worldSnapshot(1, 28));
        await waitFor(async () => (await requestJson(relay.base, '/api/world?role=host'))
            .body.worldSeq === 2, 2000, 'world evidence');
        const worldHistory = await requestJson(relay.base,
            '/api/world/history?role=host&after=1');
        assert.deepEqual(worldHistory.body.events.map((entry) => entry.stacks[0].x), [28]);
        assert.equal(worldHistory.body.events[0].activePlayerId, '0xA3DE0001');
        const currentWorld = await requestJson(relay.base, '/api/world?role=host');
        assert.equal(currentWorld.body.activePlayerId, '0xA3DE0001');

        const mutationCount = agent.received.filter((message) => mutationOps.has(message.op)).length;
        const turnWait = requestJson(relay.base,
            '/api/turn/history?role=host&after=0&waitMs=1000');
        agent.send(Op.BeginApplied, beginApplied());
        const turn = await turnWait;
        assert.equal(turn.status, 200);
        assert.equal(turn.body.timedOut, false);
        assert.deepEqual(turn.body.events.map((entry) => entry.kind), ['stock-begin-turn-applied']);
        assert.equal(agent.received.filter((message) => mutationOps.has(message.op)).length,
            mutationCount, 'an observation wait must not dispatch a mutation');

        assert.equal((await requestJson(relay.base, '/api/chat')).status, 404);
        assert.equal((await requestJson(relay.base, '/api/ui/history?after=-1')).status, 400);
    });

test('auto-battle command resolves only after the exact Russobit kick invariant',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4242 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        agent.send(Op.UiSnapshot, uiSnapshot(7, 707, { dialog: 'DLG_BATTLE_A' }));
        await waitFor(async () => {
            const state = await requestJson(relay.base, '/api/state');
            return state.body.roles?.host?.dialogAppearance === 7;
        }, 2000, 'exact battle appearance');

        const response = await requestJson(relay.base,
            '/api/ui/enable-auto-battle?role=host&dlg=DLG_BATTLE_A&tog=TOG_AUTOBATTLE'
            + '&appearance=7&instance=707', 'POST');
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.deepEqual(response.body.kick, {
            succeeded: true,
            controllerGateBefore: 0,
            kickStateBefore: 0,
            kickStateAfter: 1,
            sideSelector: 1,
            flag38Before: 0,
            flag38After: 1,
            flag39Before: 0,
            flag39After: 0,
            memberFunction: 0x00635509,
        });
        assert.equal(agent.count(Op.EnableAutoBattle), 1);
        const wire = agent.last(Op.EnableAutoBattle).payload;
        assert.equal(wire.readUInt32LE(4), 7);
        assert.equal(wire.readUInt32LE(8), 707);
        let offset = 12;
        const readString = () => {
            const length = wire.readUInt16LE(offset);
            offset += 2;
            const value = wire.subarray(offset, offset + length).toString('utf8');
            offset += length;
            return value;
        };
        assert.equal(readString(), 'DLG_BATTLE_A');
        assert.equal(readString(), 'TOG_AUTOBATTLE');
        assert.equal(offset, wire.length);
    });

test('host and join retain independent pending auto-battle commands until their own late result',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({
            role: 'host', pid: 4242, autoAcknowledge: false,
        });
        const join = await relay.connect({
            role: 'join', pid: 4343, autoAcknowledge: false,
        });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'both v4 HelloAck frames');
        await Promise.all([
            publishReadyBattle(relay, host, 'host', 7, 707),
            publishReadyBattle(relay, join, 'join', 8, 808),
        ]);

        let hostSettled = false;
        let joinSettled = false;
        const hostRequest = requestJson(relay.base,
            autoBattleRequestPath('host', 7, 707), 'POST');
        const joinRequest = requestJson(relay.base,
            autoBattleRequestPath('join', 8, 808), 'POST');
        hostRequest.then(() => { hostSettled = true; });
        joinRequest.then(() => { joinSettled = true; });
        await waitFor(() => host.count(Op.EnableAutoBattle) === 1
            && join.count(Op.EnableAutoBattle) === 1, 2000,
        'both independent 030C commands');

        const hostSeq = host.last(Op.EnableAutoBattle).payload.readUInt32LE(0);
        const joinSeq = join.last(Op.EnableAutoBattle).payload.readUInt32LE(0);
        assert.notEqual(hostSeq, joinSeq);
        await new Promise((resolve) => setTimeout(resolve, 50));
        assert.equal(hostSettled, false, 'host POST must still await its own 030D');
        assert.equal(joinSettled, false, 'join POST must still await its own 030D');

        join.send(Op.AutoBattleKickResult, autoBattleKickResult(joinSeq));
        const joinResponse = await joinRequest;
        assert.equal(joinResponse.status, 200);
        assert.equal(joinResponse.body.role, 'join');
        assert.equal(joinResponse.body.kick.succeeded, true);
        assert.equal(hostSettled, false,
            'join 030D must not consume the host socket/sequence pending command');

        host.send(Op.AutoBattleKickResult, autoBattleKickResult(hostSeq));
        const hostResponse = await hostRequest;
        assert.equal(hostResponse.status, 200);
        assert.equal(hostResponse.body.role, 'host');
        assert.equal(hostResponse.body.kick.succeeded, true);
        assert.equal(host.count(Op.EnableAutoBattle), 1);
        assert.equal(join.count(Op.EnableAutoBattle), 1);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

test('auto-battle accepts the sideSelector zero flag39 transition',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({
            role: 'host', pid: 4242, autoAcknowledge: false,
        });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        await publishReadyBattle(relay, agent, 'host', 9, 909);

        const request = requestJson(relay.base,
            autoBattleRequestPath('host', 9, 909), 'POST');
        await waitFor(() => agent.count(Op.EnableAutoBattle) === 1, 2000,
            'sideSelector zero 030C');
        const seq = agent.last(Op.EnableAutoBattle).payload.readUInt32LE(0);
        agent.send(Op.AutoBattleKickResult, autoBattleKickResult(seq, {
            sideSelector: 0,
            flag38Before: 7,
            flag38After: 7,
            flag39Before: 0,
            flag39After: 1,
        }));

        const response = await request;
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.equal(response.body.kick.sideSelector, 0);
        assert.equal(response.body.kick.flag38Before, 7);
        assert.equal(response.body.kick.flag38After, 7);
        assert.equal(response.body.kick.flag39Before, 0);
        assert.equal(response.body.kick.flag39After, 1);
        assert.equal(agent.count(Op.EnableAutoBattle), 1);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

for (const scenario of [
    {
        name: 'malformed 030D payload',
        reply: () => ({ op: Op.AutoBattleKickResult, payload: Buffer.alloc(16) }),
        reason: () => 'AutoBattleKickResult payload is invalid',
    },
    {
        name: 'inconsistent succeeded and engine proof',
        reply: (seq) => ({
            op: Op.AutoBattleKickResult,
            payload: autoBattleKickResult(seq, { succeeded: false }),
        }),
        reason: (seq) =>
            `AutoBattleKickResult seq=${seq} contradicts its engine-state proof`,
    },
    {
        name: 'kickStateAfter other than the exact value one',
        reply: (seq) => ({
            op: Op.AutoBattleKickResult,
            payload: autoBattleKickResult(seq, { kickStateAfter: 2 }),
        }),
        reason: (seq) =>
            `AutoBattleKickResult seq=${seq} contradicts its engine-state proof`,
    },
    {
        name: 'wrong result opcode',
        reply: (seq) => ({ op: Op.CommandResult, payload: commandResult(seq) }),
        reason: (seq) =>
            `CommandResult seq=${seq} does not match its expected result opcode`,
    },
]) {
    test(`auto-battle ${scenario.name} is terminal`, { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({
            role: 'host', pid: 4242, autoAcknowledge: false,
        });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        await publishReadyBattle(relay, agent, 'host', 10, 1010);

        const request = requestJson(relay.base,
            autoBattleRequestPath('host', 10, 1010), 'POST');
        await waitFor(() => agent.count(Op.EnableAutoBattle) === 1, 2000,
            `${scenario.name} pending 030C`);
        const seq = agent.last(Op.EnableAutoBattle).payload.readUInt32LE(0);
        const reply = scenario.reply(seq);
        agent.send(reply.op, reply.payload);

        const response = await request;
        assert.equal(response.status, 500);
        assert.equal(response.body.terminalFault?.reason, scenario.reason(seq));
        assert.equal(agent.count(Op.EnableAutoBattle), 1,
            'a rejected result must not redispatch 030C');
    });
}

test('AutoBattleKickResult from another registered socket cannot consume its owner',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({
            role: 'host', pid: 4242, autoAcknowledge: false,
        });
        const join = await relay.connect({
            role: 'join', pid: 4343, autoAcknowledge: false,
        });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'both v4 HelloAck frames');
        await publishReadyBattle(relay, host, 'host', 11, 1111);

        const request = requestJson(relay.base,
            autoBattleRequestPath('host', 11, 1111), 'POST');
        await waitFor(() => host.count(Op.EnableAutoBattle) === 1, 2000,
            'host pending 030C');
        const seq = host.last(Op.EnableAutoBattle).payload.readUInt32LE(0);
        join.send(Op.AutoBattleKickResult, autoBattleKickResult(seq));

        const response = await request;
        assert.equal(response.status, 500);
        assert.equal(response.body.terminalFault?.reason,
            `AutoBattleKickResult seq=${seq} has no exact pending owner`);
        assert.equal(host.count(Op.EnableAutoBattle), 1);
    });

test('MoveStack carries exact source coordinates, causal MP, and one bounded timeout',
    { timeout: 15000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        agent.send(Op.UiSnapshot, mapUiSnapshot(1, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 1, 2000, 'ready strategic map');

        const moveCount = agent.count(Op.MoveStack);
        const missingOrigin = await requestJson(relay.base,
            '/api/ui/move?role=host&id=0xA3E30000&x=27&y=16'
            + '&appearance=1&instance=101', 'POST');
        assert.equal(missingOrigin.status, 400);
        assert.equal(agent.count(Op.MoveStack), moveCount,
            'a move without the old harness origin must not reach the agent');

        const nonCanonicalMove = await requestJson(relay.base,
            '/api/ui/move?role=host&id=0xa3e30000&fromx=26&fromy=16&frommp=-1&x=27&y=16'
            + '&appearance=1&instance=101', 'POST');
        assert.equal(nonCanonicalMove.status, 400);
        assert.equal(agent.count(Op.MoveStack), moveCount,
            'a differently-cased spelling of a numeric move identity must not reach the agent');

        const outOfRangeMovement = await requestJson(relay.base,
            '/api/ui/move?role=host&id=0xA3E30000&fromx=26&fromy=16&frommp=256&x=27&y=16'
            + '&appearance=1&instance=101', 'POST');
        assert.equal(outOfRangeMovement.status, 400);
        assert.equal(agent.count(Op.MoveStack), moveCount,
            'movement outside CMidStack uint8 range must not reach the agent');

        const invalidTimeout = await requestJson(relay.base,
            '/api/ui/move?role=host&id=0xA3E30000&fromx=26&fromy=16&frommp=35&x=27&y=16'
            + '&appearance=1&instance=101&timeoutMs=0999', 'POST');
        assert.equal(invalidTimeout.status, 400);
        assert.equal(agent.count(Op.MoveStack), moveCount,
            'a non-canonical timeout must not reach the agent');

        let canonicalSettled = false;
        const canonicalPending = requestJson(relay.base,
            '/api/ui/move?role=host&id=0xA3E30000&fromx=26&fromy=16&frommp=35&x=27&y=16'
            + '&appearance=1&instance=101&timeoutMs=8000', 'POST');
        canonicalPending.finally(() => { canonicalSettled = true; });
        await waitFor(() => agent.count(Op.MoveStack) === moveCount + 1,
            2000, 'one bounded MoveStack');
        const moveSeq = agent.last(Op.MoveStack).payload.readUInt32LE(0);
        agent.send(Op.CommandStarted, commandStarted(moveSeq));
        await new Promise((resolve) => setTimeout(resolve, 5250));
        assert.equal(canonicalSettled, false,
            'the explicit timeout must outlive the old five-second deadline');
        assert.equal(agent.count(Op.MoveStack), moveCount + 1,
            'waiting beyond the old deadline must never redispatch the action');
        agent.send(Op.CommandResult, commandResult(moveSeq));
        const canonicalMove = await canonicalPending;
        assert.equal(canonicalMove.status, 200);
        assert.equal(canonicalMove.body.found, true);
        assert.deepEqual(canonicalMove.body.move, {
            id: '0xA3E30000', fromx: 26, fromy: 16, frommp: 35, x: 27, y: 16,
            appearance: 1, instance: 101,
        });
        assert.equal(agent.count(Op.MoveStack), moveCount + 1);
        const moveWire = agent.last(Op.MoveStack).payload;
        assert.equal(moveWire.readUInt32LE(4), 1);
        assert.equal(moveWire.readUInt32LE(8), 101);
        let moveOffset = 12;
        const moveIdLength = moveWire.readUInt16LE(moveOffset);
        moveOffset += 2;
        assert.equal(moveWire.subarray(moveOffset, moveOffset + moveIdLength).toString('utf8'),
            '0xA3E30000');
        moveOffset += moveIdLength;
        assert.equal(moveWire.readInt32LE(moveOffset), 26);
        assert.equal(moveWire.readInt32LE(moveOffset + 4), 16);
        assert.equal(moveWire.readInt32LE(moveOffset + 8), 35);
        assert.equal(moveWire.readInt32LE(moveOffset + 12), 27);
        assert.equal(moveWire.readInt32LE(moveOffset + 16), 16);
        assert.equal(moveOffset + 20, moveWire.length);

        agent.send(Op.UiSnapshot, uiSnapshot(2, 102));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 2, 2000, 'battle/modal owner');
        const staleMap = await requestJson(relay.base,
            '/api/ui/move?role=host&id=0xA3E30000&fromx=27&fromy=16&frommp=-1&x=28&y=16'
            + '&appearance=1&instance=101', 'POST');
        assert.equal(staleMap.status, 409);
        assert.equal(agent.count(Op.MoveStack), moveCount + 1,
            'a stale or non-map appearance must not dispatch a move');
    });

test('long-move pair writes both roles before either start and accepts either start order',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
        const join = await relay.connect({ role: 'join', pid: 4343, autoAcknowledge: false });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'paired v4 HelloAck frames');
        host.send(Op.UiSnapshot, mapUiSnapshot(1, 101));
        join.send(Op.UiSnapshot, mapUiSnapshot(2, 202));
        await waitFor(async () => {
            const state = (await requestJson(relay.base, '/api/state')).body.roles;
            return state?.host?.dialogAppearance === 1 && state?.join?.dialogAppearance === 2;
        }, 2000, 'both exact paired strategic maps');

        const path = '/api/ui/long-move-pair?'
            + 'hostid=0xA3E30000&hostfromx=26&hostfromy=16&hostfrommp=-1&hostx=28&hosty=17'
            + '&hostappearance=1&hostinstance=101'
            + '&joinid=0xA3E30001&joinfromx=16&joinfromy=26&joinfrommp=-1&joinx=17&joiny=29'
            + '&joinappearance=2&joininstance=202';
        let settled = false;
        const pending = requestJson(relay.base, path, 'POST');
        pending.finally(() => { settled = true; });

        await waitFor(() => host.count(Op.MoveStack) === 1
            && join.count(Op.MoveStack) === 1, 2000,
        'one back-to-back MoveStack per role');
        assert.equal(host.count(Op.MoveStack), 1);
        assert.equal(join.count(Op.MoveStack), 1);
        assert.equal(settled, false);
        const hostSeq = host.last(Op.MoveStack).payload.readUInt32LE(0);
        const joinSeq = join.last(Op.MoveStack).payload.readUInt32LE(0);
        assert.ok(joinSeq > hostSeq, 'paired commands retain monotonic exact identities');
        // Receipt order is deliberately the opposite of socket-write order.
        // The pair must not reintroduce a host-start scheduling gate.
        join.send(Op.CommandStarted, commandStarted(joinSeq));
        await new Promise((resolve) => setTimeout(resolve, 20));
        host.send(Op.CommandStarted, commandStarted(hostSeq));
        assert.equal(host.count(Op.CommandResult), 0,
            'neither role issue can depend on a peer final result');
        // Final-result order is intentionally not a scheduling barrier.
        join.send(Op.CommandResult, commandResult(joinSeq));
        host.send(Op.CommandResult, commandResult(hostSeq));

        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.equal(response.body.host.found, true);
        assert.equal(response.body.join.found, true);
        assert.ok(response.body.dispatchSkewMs >= 0);
        assert.equal(response.body.dispatchSkewKind,
            'absolute-relay-command-started-receipt');
        assert.equal(response.body.dispatchOrder, 'join-first');
        assert.equal(response.body.host.startedUiSeq, 2,
            'host start carries the global UI evidence watermark');
        assert.equal(response.body.join.startedUiSeq, 2,
            'join start stays in the same relay-owned UI order domain');
        assert.equal(host.count(Op.MoveStack), 1);
        assert.equal(join.count(Op.MoveStack), 1);
    });

test('attack move-pair admits host before join without waiting for host result',
    { timeout: 15000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
        const join = await relay.connect({ role: 'join', pid: 4343, autoAcknowledge: false });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'paired attack HelloAck frames');
        host.send(Op.UiSnapshot, mapUiSnapshot(1, 101));
        join.send(Op.UiSnapshot, mapUiSnapshot(2, 202));
        await waitFor(async () => {
            const state = (await requestJson(relay.base, '/api/state')).body.roles;
            return state?.host?.dialogAppearance === 1 && state?.join?.dialogAppearance === 2;
        }, 2000, 'both exact paired attack maps');

        const path = '/api/ui/move-pair?'
            + 'hostid=0xA3E30000&hostfromx=26&hostfromy=16&hostfrommp=-1&hostx=28&hosty=17'
            + '&hostappearance=1&hostinstance=101'
            + '&joinid=0xA3E30001&joinfromx=16&joinfromy=26&joinfrommp=-1&joinx=17&joiny=29'
            + '&joinappearance=2&joininstance=202&timeoutMs=12000';
        const invalidTimeout = await requestJson(relay.base,
            path.replace('timeoutMs=12000', 'timeoutMs=012000'), 'POST');
        assert.equal(invalidTimeout.status, 400);
        assert.equal(host.count(Op.MoveStack), 0);
        assert.equal(join.count(Op.MoveStack), 0,
            'a non-canonical paired timeout cannot issue either role command');
        let settled = false;
        const pending = requestJson(relay.base, path, 'POST');
        pending.finally(() => { settled = true; });

        await waitFor(() => host.count(Op.MoveStack) === 1, 2000,
            'one causal host MoveStack');
        assert.equal(join.count(Op.MoveStack), 0,
            'attack join remains gated only until host CommandStarted');
        const hostSeq = host.last(Op.MoveStack).payload.readUInt32LE(0);
        host.send(Op.CommandStarted, commandStarted(hostSeq));
        await waitFor(() => join.count(Op.MoveStack) === 1, 2000,
            'one causal join MoveStack');
        assert.equal(settled, false,
            'attack pair cannot wait for host completion before issuing join');
        const joinSeq = join.last(Op.MoveStack).payload.readUInt32LE(0);
        join.send(Op.CommandStarted, commandStarted(joinSeq));
        await new Promise((resolve) => setTimeout(resolve, 5250));
        assert.equal(settled, false,
            'the explicit paired timeout must outlive the obsolete five-second deadline');
        assert.equal(host.count(Op.MoveStack), 1);
        assert.equal(join.count(Op.MoveStack), 1,
            'waiting beyond five seconds must never retry either exact command');
        join.send(Op.CommandResult, commandResult(joinSeq));
        host.send(Op.CommandResult, commandResult(hostSeq));

        const response = await pending;
        assert.equal(response.status, 200);
        assert.equal(response.body.found, true);
        assert.equal(response.body.dispatchSkewKind,
            'relay-command-started-receipt');
        assert.ok(response.body.dispatchSkewMs >= 0);
        assert.equal(host.count(Op.MoveStack), 1);
        assert.equal(join.count(Op.MoveStack), 1);
    });

test('MoveStack result before its exact CommandStarted edge is terminal',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');
        agent.send(Op.UiSnapshot, mapUiSnapshot(1, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 1, 2000, 'ready strategic map');

        const pending = requestJson(relay.base,
            '/api/ui/move?role=host&id=0xA3E30000&fromx=26&fromy=16&frommp=-1&x=27&y=16'
            + '&appearance=1&instance=101', 'POST');
        await waitFor(() => agent.count(Op.MoveStack) === 1, 2000, 'one pending MoveStack');
        const seq = agent.last(Op.MoveStack).payload.readUInt32LE(0);
        agent.send(Op.CommandResult, commandResult(seq));

        const response = await pending;
        assert.equal(response.status, 500);
        assert.equal(response.body.terminalFault?.reason,
            `CommandResult seq=${seq} arrived before CommandStarted`);
        assert.equal(agent.count(Op.MoveStack), 1, 'the failed command is never redispatched');
    });

test('native MoveStack start edge is emitted once before worldactions and final result', () => {
    const bridge = fs.readFileSync(nativeBridgeScript, 'utf8');
    const autonav = fs.readFileSync(nativeAutonavScript, 'utf8');
    assert.match(bridge, /CommandStarted\s*=\s*0x030E/,
        'native bridge opcode must remain protocol-v7 explicit');
    assert.match(bridge,
        /void send_command_started\(std::uint32_t seq\)[\s\S]*enqueue\(Op::CommandStarted, p, sizeof\(p\)\)/,
        'native bridge must publish the exact sequence once');
    const safeMove = autonav.match(
        /void safeWorldCommand\(const RemoteCmd& cmd\)[\s\S]*?(?=\nvoid drainRemoteCommands\(\))/);
    assert.ok(safeMove, 'the shared world dispatcher must remain independently auditable');
    assert.match(safeMove[0], /if \(cmd\.type == 4\)\s*bridge::send_command_started\(cmd\.seq\)/,
        'only strict exact movement publishes the original CommandStarted edge');
    const startedAt = safeMove[0].indexOf('bridge::send_command_started(cmd.seq)');
    const nativeAt = safeMove[0].indexOf('worldactions::moveStack(');
    const resultAt = safeMove[0].indexOf('reportFound(cmd.seq, ok)');
    assert.ok(startedAt >= 0 && nativeAt > startedAt && resultAt > nativeAt,
        'CommandStarted must precede native MoveStack, while CommandResult remains final');
    assert.equal((safeMove[0].match(/send_command_started/g) || []).length, 1,
        'one native MoveStack owns exactly one start edge');
});

test('a command timeout is terminal and never redispatches', { timeout: 10000 }, async (t) => {
    const relay = await startRelay(t);
    const agent = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
    await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
    agent.send(Op.UiSnapshot, uiSnapshot(1, 101));
    await waitFor(async () => (await requestJson(relay.base, '/api/state'))
        .body.roles?.host?.dialogAppearance === 1, 2000, 'ready UI');

    const invalid = await requestJson(relay.base,
        '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
        + '&appearance=1&instance=101&timeoutMs=0999', 'POST');
    assert.equal(invalid.status, 400);
    assert.equal(agent.count(Op.InvokeButton), 0);

    const started = Date.now();
    const expired = await requestJson(relay.base,
        '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
        + '&appearance=1&instance=101&timeoutMs=1000', 'POST');
    assert.equal(expired.status, 500);
    const expiredSeq = agent.last(Op.InvokeButton).payload.readUInt32LE(0);
    assert.equal(expired.body.terminalFault?.reason,
        `command seq=${expiredSeq} timed out without a result`
        + ' (role=host op=0x0300 started=false timeoutMs=1000)');
    assert.ok(Date.now() - started >= 900);
    assert.equal(agent.count(Op.InvokeButton), 1);
    await new Promise((resolve) => setTimeout(resolve, 150));
    assert.equal(agent.count(Op.InvokeButton), 1);
});

test('a second concurrent mutation for one socket faults without queueing or sending it',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
        agent.send(Op.UiSnapshot, uiSnapshot(1, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 1, 2000, 'ready UI');

        const first = requestJson(relay.base,
            '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
            + '&appearance=1&instance=101&timeoutMs=8000', 'POST');
        await waitFor(() => agent.count(Op.InvokeButton) === 1, 2000, 'first wire mutation');
        const second = await requestJson(relay.base,
            '/api/ui/enable-toggle?role=host&dlg=DLG_EVENT_POPUP&tog=TOG_AUTOBATTLE'
            + '&appearance=1&instance=101', 'POST');
        const firstResult = await first;
        assert.equal(second.status, 500);
        assert.equal(firstResult.status, 500);
        assert.match(second.body.terminalFault?.reason || '', /already owns one pending command/);
        assert.equal(agent.count(Op.InvokeButton), 1);
        assert.equal(agent.count(Op.EnableToggle), 0);
    });

test('CommandResult from a different registered socket cannot consume the pending owner',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
        const join = await relay.connect({ role: 'join', pid: 4343, autoAcknowledge: false });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'both HelloAck frames');
        host.send(Op.UiSnapshot, uiSnapshot(1, 101));
        await waitFor(async () => (await requestJson(relay.base, '/api/state'))
            .body.roles?.host?.dialogAppearance === 1, 2000, 'host ready UI');

        const request = requestJson(relay.base,
            '/api/ui/invoke?role=host&dlg=DLG_EVENT_POPUP&btn=BTN_RIGHTSIDE'
            + '&appearance=1&instance=101&timeoutMs=8000', 'POST');
        await waitFor(() => host.count(Op.InvokeButton) === 1, 2000, 'host pending command');
        const seq = host.last(Op.InvokeButton).payload.readUInt32LE(0);
        join.send(Op.CommandResult, commandResult(seq));
        const response = await request;
        assert.equal(response.status, 500);
        assert.match(response.body.terminalFault?.reason || '',
            new RegExp(`CommandResult seq=${seq} has no exact pending owner`));
        assert.equal(host.count(Op.InvokeButton), 1);
    });

for (const scenario of [
    {
        name: 'protocol-version mismatch',
        reason: /protocol version 1 does not equal 7/,
        run: async (relay) => { await relay.connect({ role: 'host', pid: 4242, version: 1 }); },
    },
    {
        name: 'duplicate role cannot replace its owner',
        reason: /role host attempted to register more than once/,
        run: async (relay) => {
            const owner = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => owner.count(Op.HelloAck) === 1, 2000, 'owner HelloAck');
            await relay.connect({ role: 'host', pid: 4343 });
        },
        assertOwner: { role: 'host', pid: 4242 },
    },
    {
        name: 'duplicate PID cannot acquire another role',
        reason: /pid 4242 attempted to register under two roles/,
        run: async (relay) => {
            const owner = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => owner.count(Op.HelloAck) === 1, 2000, 'owner HelloAck');
            await relay.connect({ role: 'join', pid: 4242 });
        },
        assertOwner: { role: 'host', pid: 4242 },
    },
    {
        name: 'orphan CommandResult',
        reason: /CommandResult seq=777 has no exact pending owner/,
        run: async (relay) => {
            const agent = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
            agent.send(Op.CommandResult, commandResult(777));
        },
    },
    {
        name: 'nonzero frame flags',
        reason: /frame flags must be zero, got 1/,
        run: async (relay) => {
            const agent = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
            agent.send(Op.UiSnapshot, uiSnapshot(1, 101), 1);
        },
    },
    {
        name: 'malformed UI publication',
        reason: /bad UI snapshot/,
        run: async (relay) => {
            const agent = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
            agent.send(Op.UiSnapshot, Buffer.from('{"dialog":'));
        },
    },
    {
        name: 'malformed UTF-8 agent log',
        reason: /bad agent log: agent log is not valid UTF-8/,
        run: async (relay) => {
            const agent = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
            agent.send(Op.Log, Buffer.from([0xc3, 0x28]));
        },
    },
    {
        name: 'registered-agent disconnect',
        reason: /agent host pid=4242 disconnected/,
        run: async (relay) => {
            const agent = await relay.connect({ role: 'host', pid: 4242 });
            await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'HelloAck');
            agent.socket.destroy();
        },
    },
]) {
    test(`${scenario.name} is terminal`, { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        await scenario.run(relay);
        await waitForTerminal(relay.base, scenario.reason);
        if (scenario.assertOwner) {
            const status = await requestJson(relay.base, '/api/status');
            assert.equal(status.body.roles[scenario.assertOwner.role].pid, scenario.assertOwner.pid);
            assert.equal(Object.keys(status.body.roles).length, 1,
                'the rejected socket must not replace or add an owner');
        }
    });
}

test('native auto-battle source preserves the exact Russobit callback contract', () => {
    const autonav = fs.readFileSync(nativeAutonavScript, 'utf8');
    const uiReporter = fs.readFileSync(nativeUiReporterScript, 'utf8');
    const sites = fs.readFileSync(nativeRussobitSitesScript, 'utf8');
    const normalizedAutonav = autonav.replace(/\s+/g, ' ');
    const normalizedSites = normalizedAutonav;

    for (const constant of [
        'kAutoBattleToggleHandler = 0x00635509;',
        'kAutoBattleFunctorVftable = 0x006F45D4;',
        'kAutoBattleFunctorDispatch = 0x00644150;',
    ]) {
        assert.ok(normalizedSites.includes(constant),
            `missing exact Russobit site: ${constant}`);
    }
    for (const guard of [
        'static_assert(sizeof(BoundToggleFunctorLayout) == 16',
        'offsetof(BoundToggleFunctorLayout, object) == 4',
        'offsetof(BoundToggleFunctorLayout, memberFunction) == 8',
        'offsetof(BoundToggleFunctorLayout, thisAdjustor) == 12',
        'offsetof(game::CBattleViewerInterf, data) == 0x1C',
        'offsetof(game::CBattleViewerInterf, data2) == 0x20',
    ]) {
        assert.ok(normalizedAutonav.includes(guard), `missing native layout guard: ${guard}`);
    }

    const inspectStart = autonav.indexOf('AutoBattleAdmission inspectAutoBattle(');
    const commitStart = autonav.indexOf('\nbool enableAutoBattle(', inspectStart);
    const autoEnd = autonav.indexOf('\nbool setScenarioSelectionByPath(', commitStart);
    assert.ok(inspectStart >= 0 && commitStart > inspectStart && autoEnd > commitStart,
        'auto-battle observation and sole callback must remain independently auditable');
    const inspect = autonav.slice(inspectStart, commitStart);
    const autoBattle = autonav.slice(commitStart, autoEnd);
    const normalizedInspect = inspect.replace(/\s+/g, ' ');
    const normalizedAutoBattle = autoBattle.replace(/\s+/g, ' ');
    for (const guard of [
        'lstrcmpA(dlgName, "DLG_BATTLE_A") == 0',
        'lstrcmpA(togName, "TOG_AUTOBATTLE") == 0',
        'target.functorVftable == kAutoBattleFunctorVftable',
        'target.dispatchFunction == kAutoBattleFunctorDispatch',
        'bound->object && bound->thisAdjustor == 0 && boundDialog == dlg',
        'bound->memberFunction == kAutoBattleToggleHandler',
        'bindAgeMs >= kAutoBattleMinimumBindAgeMs',
        'target.result.controllerGateBefore == 0',
        'target.result.kickStateBefore == 0',
    ]) {
        assert.ok(normalizedInspect.includes(guard),
            `missing exact auto-battle admission guard: ${guard}`);
    }
    assert.ok(normalizedAutoBattle.includes('target.result.kickStateAfter == 1'),
        'the sole callback needs the exact X1D 0->1 postcondition');
    const typeProofAt = inspect.indexOf(
        'target.functorVftable == kAutoBattleFunctorVftable');
    const concreteReadAt = inspect.indexOf(
        'bound = reinterpret_cast<BoundToggleFunctorLayout*>(functor);');
    assert.ok(typeProofAt >= 0 && concreteReadAt > typeProofAt,
        'the exact vftable/dispatcher proof must precede every concrete F+ field read');

    const callback =
        'target.functor->vftable->runCallback(target.functor, true, nullptr);';
    let callbackCount = 0;
    let callbackAt = -1;
    for (let at = autoBattle.indexOf(callback); at >= 0;
        at = autoBattle.indexOf(callback, at + callback.length)) {
        callbackCount++;
        callbackAt = at;
    }
    assert.equal(callbackCount, 1, 'the bound Russobit callback must have one call site');
    assert.equal(autoBattle.includes('setChecked('), false,
        'the exact callback path must not synthesize generic toggle state');
    assert.equal(autoBattle.includes('callOnClicked('), false,
        'the exact callback path must not fall back to a generic widget click');
    const postStateAt = autoBattle.indexOf(
        'target.result.kickStateAfter = target.state[0x1D];');
    const postResultAt = autoBattle.lastIndexOf(
        'bridge::send_auto_battle_kick_result(seq, target.result);');
    assert.ok(callbackAt < postStateAt && postStateAt < postResultAt,
        '030D must be assembled from state read after the sole callback');

    const drainStart = autonav.indexOf('void drainRemoteCommands()');
    const drainEnd = autonav.indexOf('\n// One AutoDismiss tick.', drainStart);
    assert.ok(drainStart >= 0 && drainEnd > drainStart,
        'drainRemoteCommands must remain independently auditable');
    const drain = autonav.slice(drainStart, drainEnd);
    const inspectAt = drain.indexOf('inspectAutoBattle(');
    const claimAt = drain.indexOf('g_consumedRemoteCmds.push_back(cmd);', inspectAt);
    const dispatchAt = drain.indexOf('enableAutoBattle(', claimAt);
    assert.ok(inspectAt >= 0 && claimAt > inspectAt && dispatchAt > claimAt,
        'natural-frame admission must precede actionIssued, which must precede the sole callback');
    assert.match(drain,
        /AutoBattleAdmission::Waiting[\s\S]*bindAgeMs < kAutoBattleMaximumBindAgeMs[\s\S]*return;/,
        'one armed intent must observe gates across natural frames without another action');
    assert.match(drain, /getReadyDialogInstanceAge/);
    assert.ok(normalizedAutonav.includes(
        '|| type == 10 || type == 11 || type == 12;'),
    'types 11 and 12 must enter the exact-once UI-command ledger');
    assert.ok(normalizedAutonav.includes(
        'left.type == 5 || left.type == 10 || left.type == 11'),
    'all generic and specialized toggle verbs must share one semantic claim');

    const beginBindStart = uiReporter.indexOf('void beginBind(');
    const recordBindStart = uiReporter.indexOf('\nvoid recordBind(', beginBindStart);
    assert.ok(beginBindStart >= 0 && recordBindStart > beginBindStart,
        'battle-owner admission must remain independently auditable');
    const beginBind = uiReporter.slice(beginBindStart, recordBindStart);
    assert.match(beginBind,
        /returnsToStrategic[\s\S]*g_battleEpochActive = false;[\s\S]*g_battleEpochDialog = nullptr;/,
        'the old battle epoch must close on the DLG_STRATEGIC bind transition');
    assert.match(beginBind,
        /if \(isBattle\)[\s\S]*if \(!g_battleEpochActive\)[\s\S]*g_battleEpochFirstBindTick = GetTickCount\(\);[\s\S]*else if \(dialog != g_battleEpochDialog\)[\s\S]*selectDialogInstance\(dialog, g_battleEpochOwnerInstance,[\s\S]*g_battleEpochFirstBindTick, true\);/,
        'DLG_BATTLE_A must preserve one exact owner and first-bind clock through late rebinds');
    const battleBranchEnd = beginBind.indexOf('} else {', beginBind.indexOf('if (isBattle)'));
    assert.ok(battleBranchEnd > 0);
    assert.doesNotMatch(beginBind.slice(beginBind.indexOf('if (isBattle)'), battleBranchEnd),
        /g_bindCycleOpen/,
        'natural-frame construction batches must not restart the old battle clock');
});

test('canonical preboot auto-battle is event-driven, one-shot, and backed by both green battle patches', () => {
    const testdrv = fs.readFileSync(nativeTestdrvScript, 'utf8');
    const autonav = fs.readFileSync(nativeAutonavScript, 'utf8');
    const uiReporter = fs.readFileSync(nativeUiReporterScript, 'utf8');
    const sites = fs.readFileSync(nativeRussobitSitesScript, 'utf8');
    const patches = fs.readFileSync(nativePatchesScript, 'utf8');
    const controller = fs.readFileSync(nativeSimturnController, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');

    assert.match(testdrv, /testenv::on\("D2TESTDRV_AUTO_BATTLE_PREARM"\)/);
    assert.match(testdrv,
        /wantAutoBattlePrearm && !g_plan\.wantRelay[\s\S]*requires RELAY_BRIDGE/,
        'prearm must be an immutable relay-backed DebugTest plan');

    const onBind = autonav.match(
        /void onDialogBound\(const char\* dialogName,[\s\S]*?(?=\n\/\/ worldreporter)/);
    const tick = autonav.match(
        /void tickPrearmedAutoBattle\(\)[\s\S]*?(?=\nbool setScenarioSelectionByPath)/);
    assert.ok(onBind && tick, 'prearm bind capture and natural-frame tick must be auditable');
    assert.match(uiReporter,
        /recordBind\(dialog, dialogName, buttonName\);\s*autonav::onDialogBound\(dialogName, buttonName, g_dialogInstance,\s*g_curOwnerInstance, result\);/,
        'the first battle identity and exact bound button must be captured from the same successful stock bind');
    assert.match(onBind[0],
        /AwaitingFirstBattle[\s\S]*lstrcmpA\(dialogName, "DLG_BATTLE_A"\)[\s\S]*g_prearmedBattleAppearance = appearance;[\s\S]*g_prearmedBattleOwner = ownerInstance;[\s\S]*WaitingMinimumBindAge/);
    assert.match(tick[0],
        /AwaitingFirstBattle\)\s*return;[^\n]*event-driven/,
        'readiness polling must never select a later battle as the first one');
    const ageAt = tick[0].indexOf('bindAgeMs < kAutoBattleMinimumBindAgeMs');
    const inspectAt = tick[0].indexOf('inspectAutoBattle(', ageAt);
    const waitingAt = tick[0].indexOf(
        'admission == AutoBattleAdmission::Waiting', inspectAt);
    const claimAt = tick[0].indexOf('PrearmedAutoBattleState::Claimed', waitingAt);
    const callbackAt = tick[0].indexOf('enableAutoBattle(', claimAt);
    const proofAt = tick[0].indexOf('emitPrearmedAutoBattleProof(', callbackAt);
    const committedAt = tick[0].indexOf('PrearmedAutoBattleState::Committed', proofAt);
    assert.ok(ageAt >= 0 && inspectAt > ageAt && waitingAt > inspectAt && claimAt > waitingAt &&
        callbackAt > inspectAt && proofAt > callbackAt && committedAt > proofAt,
    'after >=2500ms, admission must open before claim, one callback, proof, and commit');
    assert.match(tick[0],
        /AutoBattleAdmission::Waiting[\s\S]*bindAgeMs < kAutoBattleMaximumBindAgeMs\)[\s\S]*return;/,
        'the prearmed intent passively observes the old interactive gate without issuing another action');
    assert.equal((tick[0].match(/enableAutoBattle\(/g) || []).length, 1,
        'the passive admission wait must still expose exactly one callback site');
    for (const token of [
        '\\"schema\\":1', '\\"mode\\":\\"preboot-first-battle\\"',
        '\\"role\\":\\"{}\\"', '\\"succeeded\\":{}',
        '\\"appearance\\":{}', '\\"owner\\":{}', '\\"bindAgeMs\\":{}',
        '\\"callbackCount\\":{}', '\\"functorVftable\\":{}',
        '\\"dispatchFunction\\":{}', '\\"memberFunction\\":{}',
        '\\"thisAdjustor\\":{}', '\\"controllerGateBefore\\":{}',
        '\\"kickStateBefore\\":{}', '\\"kickStateAfter\\":{}',
        '\\"sideSelector\\":{}', '\\"flag38Before\\":{}',
        '\\"flag38After\\":{}', '\\"flag39Before\\":{}',
        '\\"flag39After\\":{}'
    ]) {
        assert.ok(autonav.includes(token), `native PID proof lost ${token}`);
    }
    assert.match(autonav,
        /g_autoBattlePrearm && op == 0x030C[\s\S]*remote 030C conflicts/);
    assert.match(autonav,
        /cmd\.type == 5 \|\| cmd\.type == 10[\s\S]*DLG_BATTLE_A[\s\S]*TOG_AUTOBATTLE[\s\S]*remote 0306\/030B conflicts/);

    for (const token of [
        'autoBattleStaleGate = 0x00635578',
        'autoBattleStaleFlagSet = 0x00638886',
        '0x75, 0x08',
        '0xC6, 0x81, 0xF9, 0x14, 0x00, 0x00, 0x01'
    ]) {
        assert.ok(sites.includes(token), `green concurrent-battle site lost ${token}`);
    }
    const normalizedPatches = patches.replace(/\s+/g, ' ');
    for (const token of [
        'autoBattleStaleGateReplacement{{0x90, 0x90}}',
        'checkExpected(g_autoBattleStaleGate)',
        'checkExpected(g_autoBattleStaleFlagSet)',
        '&g_autoBattleStaleGate, &g_autoBattleStaleFlagSet',
        '&g_autoBattleStaleGate, &g_autoBattleStaleFlagSet}}',
        '0x635578=NOP2, 0x638886=NOP7'
    ]) {
        assert.ok(normalizedPatches.includes(token),
            `production patch transaction lost ${token}`);
    }
    assert.match(normalizedPatches,
        /autoBattleStaleFlagSetReplacement\{\{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,/);
    const activateSession = controller.match(
        /void activateSessionAfterIdentityBinding[\s\S]*?(?=\nvoid )/);
    assert.ok(activateSession);
    assert.ok(activateSession[0].indexOf('patches::activate()') <
        activateSession[0].indexOf('activateIndependent()'),
    'the exact battle patches must activate before independent gameplay');
    const restoreUi = patches.match(/bool restoreUiGates\(\)[\s\S]*?(?=\nbool restoreCascadeRepairs)/);
    assert.ok(restoreUi);
    assert.doesNotMatch(restoreUi[0], /&g_autoBattleStale/,
        'the process-lifetime green compatibility patches must not be restored at merge');

    const logReader = runner.match(
        /function Read-ClientLogLines[\s\S]*?(?=\nfunction Get-ClientFaultLines)/);
    const operationalGate = runner.match(
        /function Assert-OperationalActionGate[\s\S]*?(?=\nfunction Get-ClientLogMarkerCount)/);
    const proofReader = gameplay.match(
        /function Read-PrearmedAutoBattleProof[\s\S]*?(?=\nfunction Invoke-CanonicalBattleLifecyclesIndependently)/);
    assert.ok(logReader && operationalGate && proofReader);
    assert.match(logReader[0], /EndsWith\("`n", \[StringComparison\]::Ordinal\)/,
        'an unterminated live-log fragment must not become a published event');
    assert.equal((logReader[0].match(/return @\(\$lines\[0\.\.\(\$lines\.Count - 2\)\]\)/g) || []).length, 2,
        'both an unfinished fragment and a delimiter-created final empty element must be excluded from line watermarks');
    assert.match(proofReader[0], /\$proof\.\$field -isnot \[long\]/,
        'every numeric proof field must be an actual integral JSON value');
    assert.match(proofReader[0],
        /\$proof\.mode -isnot \[string\][\s\S]*\$proof\.role -isnot \[string\][\s\S]*\$proof\.succeeded -isnot \[bool\]/);
    assert.match(operationalGate[0],
        /concurrent-battle compatibility patches active \(0x635578=NOP2, 0x638886=NOP7\)[\s\S]*hostBattlePatches -ne 1[\s\S]*joinBattlePatches -ne 1/,
        'both owned processes must prove exact patch activation before gameplay');
});

test('native selection preserves one write and ten natural-frame readbacks', () => {
    const autonav = fs.readFileSync(nativeAutonavScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const setList = autonav.match(
        /bool setListSelection[\s\S]*?(?=\nbool enableToggle)/);
    const setScenario = autonav.match(
        /bool setScenarioSelectionByPath[\s\S]*?(?=\nbool setSpinOption)/);
    const selectionState = autonav.match(
        /struct SelectionReadback[\s\S]*?(?=\nbool isUiCommandType)/);
    const readSelection = autonav.match(
        /bool readListSelection[\s\S]*?(?=\nbool sameSemanticIntent)/);
    const drain = autonav.match(
        /void drainRemoteCommands\(\)[\s\S]*?(?=\n\/\/ One AutoDismiss tick\.)/);
    const naturalFrame = autonav.match(
        /void onNaturalUiFrame\(HWND window\)[\s\S]*?(?=\n\} \/\/ namespace)/);
    const tick = autonav.match(
        /void tick\(\)[\s\S]*?(?=\n\} \/\/ namespace autonav)/);
    const oneShotTransition = runner.match(
        /^function Invoke-OneShotTransition\([\s\S]*?(?=^function Wait-StableReadyButtonObservation)/m);
    assert.ok(setList && setScenario && selectionState && readSelection && drain &&
        naturalFrame && tick && oneShotTransition,
    'selection write, readback state, and natural-frame executor must remain bounded and auditable');

    for (const [name, helper] of [
        ['SetSelection', setList[0]],
        ['SelectScenarioPath', setScenario[0]]
    ]) {
        assert.doesNotMatch(helper, /onSelectionConfirmed/,
            `${name} must not synthesize the old selection-confirmed callback`);
        assert.equal((helper.match(
            /CListBoxInterfApi::get\(\)\.setSelectedIndex\(/g) || []).length, 1,
        `${name} must have exactly one native selection write site`);
        assert.match(helper, /if \(!deferResult\)[\s\S]*reportFound\(seq,/,
            `${name} must withhold CommandResult when invoked by the literal driver`);
    }
    assert.doesNotMatch(readSelection[0],
        /setSelectedIndex|setListSelection|setScenarioSelectionByPath|onSelectionConfirmed/,
        'the readback helper may only inspect the listbox and can never rewrite selection');
    assert.match(selectionState[0], /constexpr int kSelectionStableReadbacks = 10;/,
        'the green selection contract requires exactly ten stable readbacks');

    const readbackStart = drain[0].indexOf(
        'if ((cmd.type == 1 || cmd.type == 9) && g_selectionReadback.active)');
    const autoBattleStart = drain[0].indexOf('if (cmd.type == 11)', readbackStart);
    const writeStart = drain[0].indexOf(
        'if (cmd.type == 1 || cmd.type == 9)', autoBattleStart);
    const ordinaryStart = drain[0].indexOf('if (cmd.type == 0)', writeStart);
    assert.ok(readbackStart >= 0 && autoBattleStart > readbackStart &&
        writeStart > autoBattleStart && ordinaryStart > writeStart,
    'selection readback and sole-write branches must remain independently bounded');
    const readback = drain[0].slice(readbackStart, autoBattleStart);
    const write = drain[0].slice(writeStart, ordinaryStart);

    assert.match(drain[0],
        /if \(g_hasInFlight\)[\s\S]*g_inFlight\.type != 11 && g_inFlight\.type != 1[\s\S]*&& g_inFlight\.type != 9/,
        'selection command types 1 and 9 must be the only non-auto intents allowed to remain in flight');
    assert.match(write,
        /setListSelection\(cmd\.dlg, cmd\.widget, cmd\.param, cmd\.seq, true,[\s\S]*setScenarioSelectionByPath\(cmd\.dlg, cmd\.widget, cmd\.value, cmd\.seq, true,/,
        'both selection verbs must explicitly request deferred CommandResult from their one write');
    assert.doesNotMatch(write, /reportFound\(cmd\.seq, true\)/,
        'the write frame cannot publish a successful CommandResult');
    assert.match(write,
        /g_selectionReadback\.active = true;[\s\S]*g_selectionReadback\.expectedIndex = selectedIndex;[\s\S]*g_selectionReadback\.expectedTotal = elementsTotal;[\s\S]*g_selectionReadback\.matches = 0;[\s\S]*return; \/\/ read-back #1 is the next natural UI frame, never this write frame/,
        'the sole write must arm ten later readbacks and leave before consuming readback #1');
    const successfulWriteAt = write.indexOf('g_selectionReadback.active = true;');
    assert.doesNotMatch(write.slice(successfulWriteAt), /g_hasInFlight = false/,
        'a successfully written type 1/9 command must remain in flight for its readbacks');

    assert.equal((readback.match(/readListSelection\(/g) || []).length, 1,
        'each natural-frame pass performs exactly one readback');
    assert.doesNotMatch(readback,
        /setSelectedIndex|setListSelection|setScenarioSelectionByPath|onSelectionConfirmed/,
        'the active readback branch has no rewrite or callback path');
    const mismatchAt = readback.indexOf(
        'if (selectedIndex != g_selectionReadback.expectedIndex');
    const mismatchFaultAt = readback.indexOf(
        'failFastRemoteFault("selection changed during exact read-backs"', mismatchAt);
    const incrementAt = readback.indexOf('++g_selectionReadback.matches;', mismatchAt);
    const stableGateAt = readback.indexOf(
        'if (g_selectionReadback.matches < kSelectionStableReadbacks)', incrementAt);
    const successResultAt = readback.indexOf('reportFound(cmd.seq, true);', stableGateAt);
    const clearInFlightAt = readback.indexOf('g_hasInFlight = false;', successResultAt);
    assert.ok(mismatchAt >= 0 && mismatchFaultAt > mismatchAt &&
        incrementAt > mismatchFaultAt && stableGateAt > incrementAt &&
        successResultAt > stableGateAt && clearInFlightAt > successResultAt,
    'the first mismatch must fault before counting; success and in-flight release follow only readback #10');
    assert.doesNotMatch(readback.slice(mismatchAt, incrementAt),
        /return;|continue;|setSelectedIndex|setListSelection|setScenarioSelectionByPath/,
        'a mismatching frame is terminal immediately, with no retry, skip, or repair');
    assert.doesNotMatch(readback.slice(0, successResultAt), /g_hasInFlight = false/,
        'type 1/9 must remain in flight through all ten readbacks');
    assert.match(readback,
        /if \(g_selectionReadback\.matches < kSelectionStableReadbacks\)[\s\S]*return;[\s\S]*reportFound\(cmd\.seq, true\);/,
        'successful CommandResult is deferred until the tenth stable readback');

    assert.equal((naturalFrame[0].match(/tick\(\);/g) || []).length, 1,
        'the shared dispatcher advances the test driver once per natural UI frame');
    assert.equal((tick[0].match(/drainRemoteCommands\(\);/g) || []).length, 1,
        'one natural test-driver tick consumes at most one selection readback');
    assert.match(oneShotTransition[0],
        /Get-ReadyActionTarget \$candidate \$Dialog \$capturedOwner/,
        'the following one-shot button must retain the exact captured owner/appearance');
    assert.doesNotMatch(oneShotTransition[0],
        /Get-ReadyListBoxState|'SelectionIndex'|'SelectionTotal'/,
        'successful native readback #10 is the sole post-selection proof; a sticky relay reread is non-legacy');
});

test('source contract preserves strict one-shot acceptance beside universal actions', () => {
    const relay = fs.readFileSync(relayScript, 'utf8');
    const helpers = fs.readFileSync(helperScript, 'utf8');
    const autonav = fs.readFileSync(nativeAutonavScript, 'utf8');
    const worldActions = fs.readFileSync(nativeWorldActionsScript, 'utf8');
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    assert.match(relay, /const PROTOCOL_VERSION = 7;/);
    assert.doesNotMatch(relay, /PacketTrace|\/api\/(?:chat|packets|events|log)/);
    for (const action of ['HireMerc', 'MoveGroupUnit', 'DismissUnit', 'MoveStackToward'])
        assert.ok(relay.includes(action), `the unified relay must preserve generic ${action}`);
    assert.doesNotMatch(helpers, /RefireSec|lastFire/);
    assert.doesNotMatch(helpers, /catch\s*\{\s*return\s+\$(?:null|false)/s);
    const relayStateRead = powerShellFunction(helpers, 'Get-RelayState');
    assert.equal((relayStateRead.match(/Invoke-RestMethod/g) || []).length, 1,
        'the aggregate relay-state projection owns one passive HTTP read');
    assert.match(relayStateRead,
        /Invoke-RestMethod "\$script:RelayBase\/api\/state" -TimeoutSec 10/,
        'cold navigation gives the passive aggregate read bounded ten-second headroom');
    assert.doesNotMatch(relayStateRead,
        /-Method\s+POST|Start-Sleep|\bwhile\s*\(|\bfor\s*\(/,
        'relay-state headroom cannot add mutation, polling, or retry behavior');
    const uiReadyWait = powerShellFunction(helpers, 'Wait-UiButtonReadyPublication');
    assert.match(uiReadyWait, /\/api\/ui\/wait-ready\?role=/,
        'the pre-join gate must use its passive exact UI subscription endpoint');
    assert.equal((uiReadyWait.match(/Invoke-RestMethod/g) || []).length, 1,
        'one pre-join gate owns exactly one HTTP observation request');
    assert.doesNotMatch(uiReadyWait,
        /Invoke-Button|Start-SimturnGameClient|Start-Sleep|\bwhile\s*\(|\bfor\s*\(|-Method\s+POST/,
        'the pre-join gate cannot poll, mutate UI, or launch/relaunch a client');
    for (const proof of [
        'ExpectedProcessId', 'ExpectedModulePath', 'dialogAppearance',
        'dialogReady', 'uiSeq', 'matchingTargets', 'matchingButtons',
        'explicitly enabled exact button',
    ]) {
        assert.ok(uiReadyWait.includes(proof),
            `the pre-join helper lost exact proof token ${proof}`);
    }
    const worldPairWait = powerShellFunction(
        helpers, 'Wait-ExactWorldPairPublication');
    assert.match(worldPairWait, /\/api\/world\/wait-exact-pair\?hostAfter=/,
        'the source-to-stress handoff must use its passive exact-pair subscription');
    assert.equal((worldPairWait.match(/Invoke-RestMethod/g) || []).length, 1,
        'one world-pair gate owns exactly one HTTP observation request');
    assert.doesNotMatch(worldPairWait,
        /Move-Stack|Invoke-Button|Get-World|Get-RoleState|Start-Sleep|\bwhile\s*\(|-Method\s+POST|retry|refire|fallback/i,
        'the world-pair gate cannot poll, mutate, retry, or re-arm');
    for (const proof of [
        'HostAfterWorldSequence', 'JoinAfterWorldSequence',
        'ExpectedHostProcessId', 'ExpectedJoinProcessId',
        'ExpectedHostModulePath', 'ExpectedJoinModulePath',
        'worldSeq', 'latestSeq', 'connected', 'stack',
    ]) {
        assert.ok(worldPairWait.includes(proof),
            `the world-pair helper lost exact proof token ${proof}`);
    }
    const handleWorldAt = relay.indexOf('function handleWorldSnapshot(');
    const handleWorldEnd = relay.indexOf('\nfunction ', handleWorldAt + 1);
    const handleWorld = relay.slice(handleWorldAt, handleWorldEnd);
    const roleWorldUpdateAt = handleWorld.indexOf(
        'Object.assign(state.byRole[identity.role]');
    const pairNotifyAt = handleWorld.indexOf('notifyWorldPairWaiters();');
    assert.ok(roleWorldUpdateAt >= 0 && pairNotifyAt > roleWorldUpdateAt,
        'world-pair waiters must observe the new current role state, never the prior frame');
    const faultAt = relay.indexOf('function faultRelay(');
    const faultEnd = relay.indexOf('\nfunction ', faultAt + 1);
    assert.match(relay.slice(faultAt, faultEnd), /notifyWorldPairWaiters\(\);/,
        'a terminal relay fault must release the sole world-pair HTTP owner');
    const pairWaitAt = relay.indexOf('function waitForExactWorldPair(');
    const pairWaitEnd = relay.indexOf('\nfunction ', pairWaitAt + 1);
    assert.doesNotMatch(relay.slice(pairWaitAt, pairWaitEnd),
        /\bsend\s*\(|issueCommand|InvokeButton|MoveStack|setInterval|\bwhile\s*\(/,
        'the relay world-pair waiter is observation-only with one deadline timer');
    const gameUi = helpers.match(/function Get-GameUi[\s\S]*?(?=\n(?:#.*\n)*function )/);
    assert.ok(gameUi,
        'the role-scoped UI reader must remain independently auditable');
    assert.match(gameUi[0], /\/api\/ui\?role=/,
        'the shared UI reader must use the role-scoped flat UI endpoint');
    assert.doesNotMatch(gameplay, /\/api\/state\?role=/,
        'the aggregate state endpoint rejects role queries and cannot prove an action target');
    assert.match(helpers, /Wait-ActionTargetBinding[\s\S]*Invoke-Button[\s\S]*Test-DialogReady/);
    assert.match(relay,
        /parseMapActionTarget[\s\S]*fromx[\s\S]*fromy[\s\S]*frommp[\s\S]*appearance[\s\S]*instance/,
        'the relay must carry old origin, optional causal MP, and one exact ready-map identity');
    assert.match(autonav,
        /left\.type == 4[\s\S]*left\.expectedMovement >= 0 && right\.expectedMovement >= 0[\s\S]*left\.originX == right\.originX[\s\S]*left\.expectedMovement == right\.expectedMovement[\s\S]*left\.x == right\.x[\s\S]*return left\.expectedDialogAppearance[\s\S]*left\.x == right\.x/,
        'causal free moves may repeat only after MP changes, while legacy attacks retain target-only no-refire');
    assert.match(autonav, /isReadyStrategicMapInstance/,
        'native move admission must validate map identity and preserve exact origin');
    assert.match(autonav,
        /cmd\.originX[\s\S]*cmd\.originY[\s\S]*cmd\.expectedMovement/,
        'native move dispatch must preserve exact origin and causal MP');
    assert.match(worldActions,
        /start\.x != expectedFromX \|\| start\.y != expectedFromY/,
        'ordinary moves must fail rather than repair a stale source');
    assert.match(worldActions,
        /expectedMovement >= 0[\s\S]*stack->movement\) != expectedMovement/,
        'a causal free-move command must fail before send when source MP changed');
    const moveStackAt = worldActions.indexOf('bool moveStack(');
    const movementCheckAt = worldActions.indexOf(
        'if (expectedMovement < -1 || expectedMovement > 255', moveStackAt);
    const garrisonBranchAt = worldActions.indexOf(
        'if (stack->insideId != emptyId)', moveStackAt);
    const firstMoveSubmitAt = worldActions.indexOf(
        'submitStackMoveOnce(', moveStackAt);
    assert.ok(moveStackAt >= 0 && movementCheckAt > moveStackAt
        && garrisonBranchAt > movementCheckAt
        && firstMoveSubmitAt > movementCheckAt,
    'the exact live MP guard must precede every garrison/map submission branch');
    assert.match(worldActions,
        /exitStart\.x != expectedFromX \|\| exitStart\.y != expectedFromY/,
        'garrison exits must validate the real old-harness garrison cell');

    const exclusive = helpers.match(/function Assert-ExclusiveTestMachine[\s\S]*?(?=\nfunction )/);
    assert.ok(exclusive, 'scoped machine ownership helper must exist');
    assert.match(exclusive[0], /Get-Process dplaysvr/);
    assert.doesNotMatch(exclusive[0], /Get-Process Discipl2|Stop-Process|\.Kill\(/,
        'the preflight may observe dplaysvr but must not enumerate or stop game processes');

    const stopOwned = helpers.match(/function Stop-OwnedProcess[\s\S]*?(?=\n# ---- relay)/);
    assert.ok(stopOwned, 'owned-process teardown helper must exist');
    assert.match(stopOwned[0], /\$Process\.Kill\(\)/);
    assert.match(stopOwned[0], /\$Process\.WaitForExit\(/);
    assert.doesNotMatch(stopOwned[0], /Get-Process|Stop-Process|-Id\b/,
        'teardown must retain the exact Process object instead of rediscovering a PID');
    assert.match(helpers,
        /owned relay identity timeout is outside 1\.\.600 seconds/,
        'owned identity observation must accept the runner BootTimeoutSec range');
});

test('paired End Turn is one arm, one release, and one leased UI callback per role', () => {
    const relay = fs.readFileSync(relayScript, 'utf8').replace(/\r\n/g, '\n');
    const autonav = fs.readFileSync(nativeAutonavScript, 'utf8').replace(/\r\n/g, '\n');

    assert.match(relay, /InvokePairedEndTurn:\s*0x030f/);
    assert.match(relay, /ReleasePairedEndTurn:\s*0x0310/);
    const relayStart = relay.indexOf('function tryIssueEndTurnPairIntent()');
    const relayEnd = relay.indexOf('\nfunction ', relayStart + 1);
    assert.ok(relayStart >= 0 && relayEnd > relayStart,
        'the paired relay release transaction must remain independently auditable');
    const releaseTransaction = relay.slice(relayStart, relayEnd);
    assert.match(releaseTransaction,
        /encodeStr\(captured\.invoke\.btn\),[\s\S]*u32\(intent\.commandTimeoutMs\)/,
        'each arm must carry the exact relay command timeout into the native wait');
    const hostArmAt = releaseTransaction.indexOf(
        'intent.host.socket, Op.InvokePairedEndTurn');
    const joinArmAt = releaseTransaction.indexOf(
        'intent.join.socket, Op.InvokePairedEndTurn');
    const bothStartedAt = releaseTransaction.indexOf(
        'Promise.all([hostCommand.started, joinCommand.started])');
    const hostReleaseAt = releaseTransaction.indexOf(
        'intent.host.socket, Op.ReleasePairedEndTurn');
    const joinReleaseAt = releaseTransaction.indexOf(
        'intent.join.socket, Op.ReleasePairedEndTurn');
    const bothResultsAt = releaseTransaction.indexOf(
        'Promise.all([hostCommand.result, joinCommand.result])');
    assert.ok(hostArmAt >= 0 && joinArmAt > hostArmAt
        && bothStartedAt > joinArmAt && hostReleaseAt > bothStartedAt
        && joinReleaseAt > hostReleaseAt && bothResultsAt > joinReleaseAt,
    'both one-shot arms must precede both native started edges, both releases, and both results');
    assert.equal((releaseTransaction.match(/Op\.InvokePairedEndTurn/g) || []).length, 2);
    assert.equal((releaseTransaction.match(/Op\.ReleasePairedEndTurn/g) || []).length, 2);
    assert.doesNotMatch(releaseTransaction,
        /setInterval|setTimeout|\bwhile\s*\(|\bfor\s*\(|retry|refire|fallback/i,
        'the paired release transaction cannot poll, retry, refire, or fall back');

    const nativeStart = autonav.indexOf('bool invokePairedEndTurn(const RemoteCmd& cmd)');
    const nativeEnd = autonav.indexOf('\n// Literal contract carried over', nativeStart);
    assert.ok(nativeStart >= 0 && nativeEnd > nativeStart,
        'the leased native callback must remain independently auditable');
    const nativeBarrier = autonav.slice(nativeStart, nativeEnd);
    const idleAt = nativeBarrier.indexOf('uistatereporter::isStrategicIdle()');
    const armAt = nativeBarrier.indexOf(
        'g_pairedEndTurnArmedSeq.compare_exchange_strong');
    const startedAt = nativeBarrier.indexOf('bridge::send_command_started(cmd.seq);');
    const releaseAt = nativeBarrier.indexOf('const auto released = g_pairedEndTurnReleasedSeq.load');
    const resultAt = nativeBarrier.indexOf('reportFound(cmd.seq, true);');
    const callbackAt = nativeBarrier.indexOf(
        'invokePairedEndTurnCallback(exactFunctor)');
    assert.ok(idleAt >= 0 && armAt > idleAt && startedAt > armAt
        && releaseAt > startedAt && callbackAt > releaseAt && resultAt > callbackAt,
    'native idle admission must precede arm, started, release, the sole callback, and its result');
    assert.match(nativeBarrier,
        /cmd\.releaseTimeoutMs\)[\s\S]*kPairedEndTurnFaultPropagationMs/,
        'the native wait must use the wire timeout plus its bounded failure-propagation margin');
    const callbackHelperStart = autonav.indexOf(
        'bool invokePairedEndTurnCallback(game::CBFunctorDispatch0* exactFunctor)');
    const callbackHelperEnd = autonav.indexOf(
        '\nbool pairedEndTurnOwnsNativeTurn()', callbackHelperStart);
    assert.ok(callbackHelperStart >= 0 && callbackHelperEnd > callbackHelperStart,
        'the SEH-only callback wrapper must remain independently auditable');
    const callbackHelper = autonav.slice(callbackHelperStart, callbackHelperEnd);
    assert.equal((callbackHelper.match(/runCallback\(exactFunctor\)/g) || []).length, 1,
        'the leased EndTurn callback must have exactly one native call site');
    assert.doesNotMatch(callbackHelper, /Sleep\s*\(|PostMessage|retry|refire|fallback/i,
        'the SEH-only callback wrapper cannot add scheduling or recovery behavior');
    assert.doesNotMatch(nativeBarrier,
        /WaitForSingleObject|WaitForMultipleObjects|Sleep\s*\(|PostMessage|g_remoteCmds\.push_back|retry|refire|fallback/i,
        'the pending UI intent cannot block, sleep, repost, requeue, retry, refire, or fall back');

    const releaseStart = autonav.indexOf('if (op == kReleasePairedEndTurnOp)');
    const releaseEnd = autonav.indexOf('\n    if (g_autoBattlePrearm', releaseStart);
    assert.ok(releaseStart >= 0 && releaseEnd > releaseStart,
        'the bridge-thread release handler must remain independently auditable');
    const nativeRelease = autonav.slice(releaseStart, releaseEnd);
    assert.match(nativeRelease,
        /g_pairedEndTurnArmedSeq\.load[\s\S]*g_pairedEndTurnReleasedSeq\.compare_exchange_strong/,
        'one exact armed sequence must own the sole atomic release');
    assert.doesNotMatch(nativeRelease, /g_remoteCmds\.push_back|PostMessage|Sleep\s*\(/,
        'the release edge cannot schedule another command or call the UI callback itself');
    assert.match(autonav,
        /shared natural-frame callback registration failed[\s\S]*setCommandCallback\(nullptr\)/,
        'failed preflight unregisters command delivery');
    assert.doesNotMatch(autonav, /g_pairedEndTurnReleaseEvent/,
        'the nonblocking pair owns no Windows event or blocked UI wait');
});

test('battle-block preserves source FREE checkpoints, then exhausts both cross-battle movement ledgers', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const helper = fs.readFileSync(helperScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const autonav = fs.readFileSync(nativeAutonavScript, 'utf8');
    const nativeBridge = fs.readFileSync(nativeBridgeScript, 'utf8');
    const uiReporter = fs.readFileSync(nativeUiReporterScript, 'utf8');
    const fixture = JSON.parse(fs.readFileSync(simturnsRussobitFixture, 'utf8'));
    const attackPlan = powerShellFunction(gameplay, 'Get-LegacyBattleBlockAttackPlan');
    const reverseAttackPlan = powerShellFunction(
        gameplay, 'Get-LegacyCrossBattleReverseAttackPlan');
    const reverseHostMovePlan = powerShellFunction(
        gameplay, 'Get-LegacyCrossBattleHostMovePlan');
    const getWorldStack = powerShellFunction(gameplay, 'Get-WorldStackExact');
    const exactWorldStackState = powerShellFunction(
        gameplay, 'Test-ExactWorldStackPositionAndMovement');
    const exactCrossBattleWorld = powerShellFunction(
        gameplay, 'Wait-ExactCrossBattleHostMoveWorld');
    const stressLedger = powerShellFunction(
        gameplay, 'Invoke-PinnedMovementLedgerWhileBattleLive');
    const proof = powerShellFunction(gameplay, 'Invoke-BattleBlockProof');
    const moveStack = powerShellFunction(helper, 'Move-Stack');
    const clientLaunch = powerShellFunction(runner, 'Start-SimturnGameClient');

    assert.match(moveStack,
        /\[int\]\$CommandTimeoutMilliseconds = 0[\s\S]*outside 1000\.\.120000 ms/,
        'single MoveStack exposes one bounded opt-in command timeout');
    assert.match(moveStack,
        /\$httpTimeoutSeconds = 8[\s\S]*&timeoutMs=\$CommandTimeoutMilliseconds[\s\S]*\[math\]::Ceiling\(\$CommandTimeoutMilliseconds \/ 1000\.0\) \+ 10/,
        'an opt-in relay timeout receives ten seconds of HTTP headroom');
    assert.equal((moveStack.match(/script:Post \$path \$httpTimeoutSeconds/g) || []).length, 1,
        'MoveStack owns one HTTP mutation site for both timeout modes');
    assert.doesNotMatch(moveStack,
        /Start-Sleep|\bwhile\s*\(|retry|refire|fallback|script:Post[\s\S]*script:Post/i,
        'timeout selection cannot delay, retry, refire, or add a second POST');

    const naturalTick = autonav.match(
        /void tick\(\)[\s\S]*?(?=\n\} \/\/ namespace autonav)/)?.[0] || '';
    const refreshAt = naturalTick.indexOf('uistatereporter::refreshCurrentDialog()');
    const worldAt = naturalTick.indexOf('safeRebuildWorld()');
    assert.ok(refreshAt >= 0 && worldAt > refreshAt,
        'one natural UI frame must refresh dialog controls before rebuilding world');
    const bridgeLoop = nativeBridge.slice(
        nativeBridge.indexOf('uint32_t last_ui_epoch = 0;'),
        nativeBridge.indexOf('// 1. Drain pending writes from the game-thread enqueue.'));
    const uiWriteAt = bridgeLoop.indexOf('write_message(Op::UiSnapshot');
    const worldWriteAt = bridgeLoop.indexOf('write_message(Op::WorldSnapshot');
    assert.ok(uiWriteAt >= 0 && worldWriteAt > uiWriteAt,
        'the one join socket must publish changed UI before world evidence');
    const refresh = uiReporter.match(
        /void refreshCurrentDialog\(\)[\s\S]*?(?=\nbool preflight\(\))/)?.[0] || '';
    assert.match(refresh, /g_dialogReady = true;\s*rebuildSnapshot\(\);/,
        'each admitted natural frame must rebuild the current ready UI snapshot');

    assert.match(clientLaunch,
        /if \(\$GameplayMode -in @\('canonical', 'battle-block', 'long-attack'\)\) \{\s*\$psi\.EnvironmentVariables\['D2TESTDRV_AUTO_BATTLE_PREARM'\] = '1'\s*\}/,
        'run_test/Boot-Ready prearms canonical, battle-block, and long-attack clients before boot');
    assert.equal((clientLaunch.match(/D2TESTDRV_AUTO_BATTLE_PREARM/g) || []).length, 1,
        'both literal modes share one immutable preboot arm site');
    assert.match(proof,
        /\[Parameter\(Mandatory\)\]\[string\]\$HostLog/,
        'battle-block must consume the exact owned host PID log for its preboot proof');
    assert.match(runner,
        /Invoke-BattleBlockProof\s+`\s*\$fixture \$hostProcess \$joinProcess \$hostLog/,
        'the main runner passes that exact host PID log');

    assert.equal((attackPlan.match(/Get-LegacyStackSnapshot/g) || []).length, 2,
        'the old relay-global /api/stacks pair remains two physical shared reads');
    const hostStkAt = attackPlan.indexOf('$hostHeroWorld = Get-LegacyStackSnapshot');
    const freshNeutralAt = attackPlan.indexOf('$neutralWorld = Get-LegacyStackSnapshot');
    assert.ok(hostStkAt >= 0 && freshNeutralAt > hostStkAt,
        'Stk(host) must precede the fresh stacks census for nearest neutral');
    assert.doesNotMatch(attackPlan,
        /Get-WorldSnapshot|Get-World\s|Get-RelayState|Get-RoleState|Wait-|Start-Sleep|Assert-LegacyBattleBlockAttackPlanMatchesFixture/,
        'the two source censuses contain no convergence read or fixture strengthening');

    const attackPlanAt = proof.indexOf(
        '$legacyAttackPlan = Get-LegacyBattleBlockAttackPlan $Fixture');
    const hostFireAt = proof.indexOf(
        'if (-not (Move-Stack host ([string]$legacyAttackPlan.id)', attackPlanAt);
    assert.ok(attackPlanAt >= 0 && hostFireAt > attackPlanAt,
        'the two source censuses must directly drive the sole host Fire');
    assert.ok(proof.indexOf('$hostMapBinding = Get-MapActionTargetBinding host') < attackPlanAt,
        'the native map binding must be captured before the source census pair');
    assert.doesNotMatch(proof.slice(attackPlanAt, hostFireAt),
        /Assert-|Get-(?!LegacyBattleBlockAttackPlan)|Read-|Wait-|Start-Sleep|Invoke-RestMethod/,
        'no observation or fixture assertion may be inserted between the census pair and Fire');
    assert.equal((proof.match(/Move-Stack host/g) || []).length, 2,
        'battle-block keeps one direct forward host attack and one source reverse host step');
    assert.equal((proof.match(/-CommandTimeoutMilliseconds 8000/g) || []).length, 1,
        'only the initial host battle attack receives the bounded eight-second timeout');
    assert.match(proof.slice(hostFireAt, proof.indexOf('$hostBattleUi =', hostFireAt)),
        /Move-Stack host[\s\S]*-CommandTimeoutMilliseconds 8000[\s\S]*sole host attack command was not issued/,
        'the one initial host attack keeps one fire while widening only its result deadline');

    assert.match(proof, /\[int\]\$sourceStepCount = 4/,
        'the port pins the documented green FREE 4/4 invocation');
    assert.match(proof, /for \(\$i = 1; \$i -le \$sourceStepCount; \$i\+\+\)/,
        'the literal loop must execute exactly those four proved steps');

    const literalStart = proof.indexOf('LITERAL_BATTLE_BLOCK_STEP_BEGIN');
    const literalEnd = proof.indexOf('LITERAL_BATTLE_BLOCK_STEP_END', literalStart);
    const verdictBoundary = proof.indexOf('LITERAL_BATTLE_BLOCK_VERDICT_BOUNDARY', literalEnd);
    assert.ok(literalStart >= 0 && literalEnd > literalStart && verdictBoundary > literalEnd,
        'the literal step and source-verdict boundaries must stay explicit');
    const literal = proof.slice(literalStart, literalEnd);

    const sourceOrder = [
        '$hostUiBeforeStep = Get-DialogObservation host',
        '$joinWorldBeforeStepSequence = Get-RoleEvidenceSequence join world',
        '$joinBeforeWorld = Get-LegacyStackSnapshot',
        'if (-not (Move-Stack join',
        'Start-Sleep -Milliseconds 1500',
        '$joinAfterWorld = Get-LegacyStackSnapshot',
        '$hostUiAfterStep = Get-DialogObservation host',
        'if ($moved -and $inBat) { $freeSteps++ }',
        'if ($moved -and -not $inBat) { $afterSteps++ }'
    ];
    let previous = -1;
    for (const token of sourceOrder) {
        const at = literal.indexOf(token, previous + 1);
        assert.ok(at > previous, `literal battle-block operation moved or vanished: ${token}`);
        previous = at;
    }
    assert.equal((literal.match(/Get-DialogObservation host/g) || []).length, 2,
        'each source step performs exactly its pre/post Ui(host) reads');
    assert.equal((literal.match(/Get-LegacyStackSnapshot/g) || []).length, 2,
        'each source step performs exactly its pre/post relay-global Stk(join) reads');
    assert.doesNotMatch(literal, /Get-WorldSnapshot/,
        'the old relay-global /api/stacks census must not become a join-local reporter read');
    assert.equal((literal.match(/Move-Stack join/g) || []).length, 1,
        'each source step has exactly one Fire action');
    assert.equal((literal.match(/Start-Sleep -Milliseconds 1500/g) || []).length, 1,
        'the one loop body retains the fixed +1500ms source cadence');
    assert.match(literal,
        /Move-Stack join[\s\S]*\$jx \$jy \(\$jx \+ 1\) \$jy[\s\S]*\$joinMapBinding\.Instance \$joinMapBinding\.Appearance\s*`\s*\(\[int\]\$before\.movement\)/,
        'the sole Fire is one tile east with its exact observed source MP and map owner');
    assert.doesNotMatch(literal, /retry|refire|fallback|cancel|Move-Stack\s+host/i,
        'the four-step oracle has no alternate or repeated action path');

    const preReadAt = literal.indexOf('$joinBeforeWorld = Get-LegacyStackSnapshot');
    const fireAt = literal.indexOf('if (-not (Move-Stack join', preReadAt);
    assert.ok(fireAt > preReadAt, 'the pre-read must precede the sole Fire');
    const preReadToFire = literal.slice(
        preReadAt + '$joinBeforeWorld = Get-LegacyStackSnapshot'.length, fireAt);
    assert.doesNotMatch(preReadToFire,
        /Get-(?:DialogObservation|WorldSnapshot|World\b|Role|Relay|MapAction)|Wait-|Assert-/,
        'no relay/UI/world/process observation may be inserted between Stk(join) and Fire');
    assert.equal((literal.match(/Wait-WorldEvidence/g) || []).length, 1,
        'every fixed sample is followed by one owner-world completion fence');
    assert.equal((literal.match(/Get-RoleEvidenceSequence join world/g) || []).length, 1,
        'each command is watermarked once on the mover world before its source census');
    assert.match(literal,
        /Start-Sleep -Milliseconds 1500[\s\S]*\$joinAfterWorld = Get-LegacyStackSnapshot[\s\S]*\$movedAtFixedSample[\s\S]*Wait-WorldEvidence -Role join\s*`[\s\S]*-After \$joinWorldBeforeStepSequence[\s\S]*\$hostUiAfterStep = Get-DialogObservation host/,
        'the owner-world fence runs unconditionally after the exact +1500 source sample');
    assert.doesNotMatch(literal,
        /Assert-HostBattleContinuous|Wait-BattleBlock|Get-World\s+(?:host|join)/,
        'other event/convergence strengthening must not alter any source step window');

    const verdictPhase = proof.slice(literalEnd, verdictBoundary);
    assert.match(verdictPhase,
        /if \(\$freeSteps -gt 0\)[\s\S]*'FREE'[\s\S]*elseif \(\$afterSteps -gt 0\)[\s\S]*'BLOCKED'[\s\S]*'INCONCLUSIVE'/,
        'FREE/BLOCKED/INCONCLUSIVE must be calculated from the exact source counters');
    assert.match(verdictPhase,
        /if \(\$legacyVerdict -ne 'FREE'\)[\s\S]*legacy FREE requires freeSteps > 0/,
        'the source pass condition remains FREE on any positive in-battle move');
    assert.match(verdictPhase, /documented source checkpoint 4\/4/,
        'the port names the aef761e documented green checkpoint precisely');

    const strengthened = proof.slice(verdictBoundary);
    for (const token of [
        'Assert-ClientsLive $HostProcess $JoinProcess',
        'Assert-LegacyBattleBlockAttackPlanMatchesFixture $legacyAttackPlan $Fixture',
        '$legacySteps.Count -ne $sourceStepCount',
        '$recountedFreeSteps -ne $freeSteps',
        '$recountedAfterSteps -ne $afterSteps',
        '$continuousFreeSteps -ne $sourceStepCount',
        'Read-PrearmedAutoBattleProof -Role host',
        'legacyFreePassed = $true',
        'continuousBattleFourOfFourProved = $true',
        'sourceBoundaryObservedBeforeHostBattleCompletion = $true',
        'forwardStressFiveStepsProved = $true',
        'hostBattleCompletedOnlyForReverseContinuation = $true',
        'reverseCrossOneShotProved = $true',
        'reverseHostExhaustionProved = $true',
        'joinBattleLiveBefore = $true',
        'joinBattleLiveAfter = $true',
        'joinWorldReplicationProved = $true',
        'joinUiBeforeWorldCausalityProved = $true',
        'sourcePostApplyGraceMilliseconds = 1500',
        'joinWorldBefore = [pscustomobject]@{',
        'joinWorldAfter = [pscustomobject]@{',
        'forwardStress = [pscustomobject]@{',
        'sourceHandoff = [pscustomobject]@{',
        'hostExhaustion = [pscustomobject]@{',
        'joinBattleCompletionAttempted = $false'
    ]) {
        assert.ok(strengthened.includes(token),
            `deferred MSS evidence lost ${token}`);
    }
    for (const token of [
        'commandIssued = $true',
        'moved = [bool]$moved',
        'hostInBattleBefore = [bool]$inBat',
        'hostInBattleAfter = [bool]$hostBat'
    ]) {
        assert.ok(proof.includes(token), `each of the four source records lost ${token}`);
    }
    assert.match(proof,
        /if \(-not \$moved\)[\s\S]*refusing another mutation[\s\S]*-not \$inBat -or -not \$hostBat/,
        'each failed or non-continuous step aborts before another mutation can be issued');
    assert.match(strengthened,
        /\$freeSteps -ne \$sourceStepCount[\s\S]*\$afterSteps -ne 0[\s\S]*\$continuousFreeSteps -ne \$sourceStepCount[\s\S]*continuous FREE 4\/4/,
        'deferred MSS acceptance requires the documented continuous four-of-four result');
    assert.doesNotMatch(proof,
        /Invoke-ParallelExactAutoBattle|Get-ExactAutoBattleBinding|\/api\/ui\/enable-auto-battle/,
        'the source-prearmed battle-block path must not issue a late auto-battle action');
    assert.match(proof,
        /battle-block MSS audit PASS: continuous battle FREE 4\/4[\s\S]*source boundary has host still in battle/,
        'four command records are audited under the strict continuous-battle result');

    assert.deepEqual(fixture.crossBattle.joinTarget,
        { id: '0xA3E30021', x: 20, y: 25 });
    assert.equal(fixture.crossBattle.joinAttackWireBudget, 6);
    assert.deepEqual(fixture.crossBattle.joinStressSteps, [
        { fromX: 19, fromY: 27, toX: 18, toY: 27,
            movementBefore: 23, movementAfter: 20 },
        { fromX: 18, fromY: 27, toX: 19, toY: 27,
            movementBefore: 20, movementAfter: 17 },
        { fromX: 19, fromY: 27, toX: 18, toY: 27,
            movementBefore: 17, movementAfter: 14 },
        { fromX: 18, fromY: 27, toX: 19, toY: 27,
            movementBefore: 14, movementAfter: 11 },
        { fromX: 19, fromY: 27, toX: 18, toY: 27,
            movementBefore: 11, movementAfter: 8 },
    ]);
    assert.deepEqual(fixture.crossBattle.hostMove, { x: 27, y: 17, movement: 13 });
    assert.deepEqual(fixture.crossBattle.hostExhaustionSteps, [
        { fromX: 27, fromY: 17, toX: 28, toY: 17,
            movementBefore: 13, movementAfter: 11 },
        { fromX: 28, fromY: 17, toX: 27, toY: 17,
            movementBefore: 11, movementAfter: 9 },
        { fromX: 27, fromY: 17, toX: 28, toY: 17,
            movementBefore: 9, movementAfter: 7 },
        { fromX: 28, fromY: 17, toX: 27, toY: 17,
            movementBefore: 7, movementAfter: 5 },
        { fromX: 27, fromY: 17, toX: 28, toY: 17,
            movementBefore: 5, movementAfter: 3 },
        { fromX: 28, fromY: 17, toX: 29, toY: 17,
            movementBefore: 3, movementAfter: 0 },
    ]);
    assert.equal((reverseAttackPlan.match(/Get-LegacyStackSnapshot/g) || []).length, 3,
        'reverse attack keeps fighter, fresh neutral, then mover-MP shared reads');
    assert.match(reverseAttackPlan,
        /\$fighterWorld = Get-LegacyStackSnapshot[\s\S]*\$neutralWorld = Get-LegacyStackSnapshot[\s\S]*Sort-Object distance[\s\S]*\$moverWorld = Get-LegacyStackSnapshot/,
        'reverse attack preserves old Stk fighter -> NearN -> Stk mover order');
    assert.equal((reverseHostMovePlan.match(/Get-LegacyStackSnapshot/g) || []).length, 2,
        'reverse host FreeAdj keeps current mover and separate occupancy reads');
    assert.match(reverseHostMovePlan,
        /@\(-1, 0\), @\(0, -1\), @\(1, 0\), @\(0, 1\),[\s\S]*@\(-1, -1\), @\(1, 1\), @\(-1, 1\), @\(1, -1\)[\s\S]*Sort-Object garrisonDistance/,
        'reverse host move preserves the old FreeAdj direction and distance order');

    const reverseBegin = proof.indexOf('LITERAL_CROSS_BATTLE_REVERSE_BEGIN');
    const reverseVerdict = proof.indexOf(
        'LITERAL_CROSS_BATTLE_REVERSE_VERDICT_BOUNDARY', reverseBegin);
    assert.ok(reverseBegin > verdictBoundary && reverseVerdict > reverseBegin,
        'reverse continuation must follow, not alter, the forward source verdict');
    const reverse = proof.slice(reverseBegin, reverseVerdict);
    const handoffBegin = proof.indexOf('MSS_CROSS_BATTLE_FORWARD_HANDOFF_BEGIN');
    const handoffVerdict = proof.indexOf(
        'MSS_CROSS_BATTLE_FORWARD_HANDOFF_VERDICT_BOUNDARY', handoffBegin);
    const forwardStressBegin = proof.indexOf('MSS_CROSS_BATTLE_FORWARD_STRESS_BEGIN');
    const forwardStressVerdict = proof.indexOf(
        'MSS_CROSS_BATTLE_FORWARD_STRESS_VERDICT_BOUNDARY', forwardStressBegin);
    assert.ok(handoffBegin > verdictBoundary
        && handoffVerdict > handoffBegin
        && forwardStressBegin > handoffVerdict
        && forwardStressVerdict > forwardStressBegin
        && reverseBegin > forwardStressVerdict,
    'causal handoff and forward stress must follow source verdict before reverse planning');
    const handoff = proof.slice(handoffBegin, handoffVerdict);
    assert.equal((handoff.match(/Wait-ExactWorldPairPublication/g) || []).length, 1,
        'one exact world-pair event subscription owns the source-to-stress handoff');
    assert.match(handoff,
        /\$sourceEndpoint\.toX -ne 19[\s\S]*\$sourceEndpoint\.toY -ne 27[\s\S]*\$sourceEndpoint\.movementAfter -ne 23[\s\S]*\$firstStressStep\.fromX[\s\S]*\$firstStressStep\.movementBefore/,
        'the runtime handoff pins the source FREE4 endpoint to the first stress origin');
    assert.match(handoff,
        /Get-ExactLiveBattleUiProof[\s\S]*Wait-ExactWorldPairPublication[\s\S]*Get-ExactLiveBattleUiProof[\s\S]*Get-UiHistory[\s\S]*sourceHandoffUiCursor -ne/,
        'the exact pair subscription is enclosed by one continuous host-battle UI interval');
    assert.doesNotMatch(handoff,
        /Wait-WorldEvidence|Get-World\s|Start-Sleep|Move-Stack|Invoke-Button|Complete-CanonicalBattle|retry|refire|fallback/i,
        'the handoff cannot poll, sleep, mutate, retry, or fall back');
    const sourceOwnerAt = proof.indexOf('$sourceRoleStates = [ordered]@{');
    const sourceLoopAt = proof.indexOf('for ($i = 1; $i -le $sourceStepCount; $i++)');
    assert.ok(sourceOwnerAt >= 0 && sourceOwnerAt < sourceLoopAt
        && sourceLoopAt < verdictBoundary,
    'both role-world owners and watermarks must be captured before the immutable FREE4 loop');
    const forwardStress = proof.slice(forwardStressBegin, forwardStressVerdict);
    assert.equal((forwardStress.match(/Invoke-PinnedMovementLedgerWhileBattleLive/g) || []).length,
        1, 'one fixed five-step join ledger extends the still-live host battle');
    assert.doesNotMatch(forwardStress, /Start-Sleep|retry|refire|fallback|while\s*\(/i,
        'forward stress is event-driven and has no adaptive action path');
    assert.equal((proof.match(/Complete-CanonicalBattle/g) || []).length, 1,
        'the full battle-block proof has one host completion and no hidden completion');
    assert.equal((proof.match(/Move-Stack join/g) || []).length, 2,
        'direct call sites remain the source join Fire and reverse join attack');
    assert.equal((proof.match(/Move-Stack host/g) || []).length, 2,
        'direct call sites remain the forward host attack and source reverse host step');
    assert.equal((reverse.match(/Complete-CanonicalBattle/g) || []).length, 1,
        'only the first host battle is completed to enter the reverse continuation');
    assert.equal((reverse.match(/Start-Sleep\s+-Seconds\s+3/g) || []).length, 1,
        'the exact old inter-CrossTest three-second quiet period is preserved once');
    assert.equal((reverse.match(/Start-Sleep\s+-Milliseconds\s+1500/g) || []).length, 1,
        'the exact old post-apply 1.5-second FREE/BLOCKED classification window is preserved once');
    assert.match(reverse,
        /Complete-CanonicalBattle[\s\S]*-Role host[\s\S]*Start-Sleep -Seconds 3[\s\S]*Move-Stack join[\s\S]*Wait-UiEvidence -Role join[\s\S]*Get-World join[\s\S]*Move-Stack host[\s\S]*Wait-ExactCrossBattleHostMoveWorld -Role join[\s\S]*Start-Sleep -Milliseconds 1500[\s\S]*Get-DialogObservation join[\s\S]*Get-UiHistory join[\s\S]*Wait-ExactCrossBattleHostMoveWorld -Role host/,
        'reverse is host close -> source quiet -> join attack/live battle -> exact join baseline -> host step -> join replication -> source grace/live-history -> host world');
    assert.equal((reverse.match(/Move-Stack join/g) || []).length, 1,
        'reverse continuation submits the join attack exactly once');
    assert.equal((reverse.match(/Move-Stack host/g) || []).length, 1,
        'reverse continuation submits the host step exactly once');
    assert.match(reverse,
        /Move-Stack host \(\[string\]\$reverseHostMovePlan\.id\)[\s\S]*\$hostMapBinding\.Instance \$hostMapBinding\.Appearance\s*`\s*\(\[int\]\$reverseHostMovePlan\.movementBefore\)/,
        'the source reverse FreeAdj carries its exact causal source MP');
    assert.doesNotMatch(proof, /Wait-CanonicalWorldConvergence/,
        'the composed battle-block adds no unrelated world-convergence tail');
    assert.doesNotMatch(reverse, /Complete-CanonicalBattle\s+`?\s*-Role join/,
        'reverse continuation stops with the exact join battle still live');

    const exhaustionBegin = proof.indexOf(
        'MSS_CROSS_BATTLE_HOST_EXHAUSTION_BEGIN', reverseVerdict);
    const exhaustionVerdict = proof.indexOf(
        'MSS_CROSS_BATTLE_HOST_EXHAUSTION_VERDICT_BOUNDARY', exhaustionBegin);
    assert.ok(exhaustionBegin > reverseVerdict && exhaustionVerdict > exhaustionBegin,
        'host exhaustion must start after the immutable source reverse verdict');
    const exhaustion = proof.slice(exhaustionBegin, exhaustionVerdict);
    const afterExhaustion = proof.slice(exhaustionVerdict);
    assert.equal((exhaustion.match(/Invoke-PinnedMovementLedgerWhileBattleLive/g) || []).length,
        1, 'one fixed six-step tail consumes the host ledger to MP0');
    assert.match(exhaustion,
        /hostExhaustionSteps\.Count -ne 6[\s\S]*movementAfter -ne 0/,
        'host stress requires all six planned commands and an exact MP0 endpoint');
    assert.doesNotMatch(exhaustion, /Start-Sleep|retry|refire|fallback|while\s*\(/i,
        'host exhaustion waits only for evidence and has no adaptive action path');
    assert.doesNotMatch(afterExhaustion,
        /\b(?:Move|Invoke|Complete|Start|Wait|Set|Enable|Disable)-[A-Za-z]/i,
        'no command-shaped call is allowed after the final stress verdict boundary');
    assert.match(stressLedger,
        /Move-Stack \$MoverRole \$HeroId[\s\S]*\$expected\.movementBefore[\s\S]*-CommandTimeoutMilliseconds 8000/,
        'every fixed stress command carries its exact causal source MP and bounded eight-second result budget');
    assert.equal((stressLedger.match(/Move-Stack \$MoverRole/g) || []).length, 1,
        'the generic ledger owns one one-shot movement call site');
    assert.equal((stressLedger.match(/-CommandTimeoutMilliseconds 8000/g) || []).length, 1,
        'the generic ledger widens its one command once');
    const executableStressLedger = stressLedger.replace(/^\s*#.*$/gm, '');
    assert.doesNotMatch(executableStressLedger, /Start-Sleep|retry|refire|fallback/i,
        'the fixed ledger cannot pause, retry, refire, or fall back');
    assert.match(stressLedger,
        /Get-UiHistory[\s\S]*Get-ExactLiveBattleUiProof[\s\S]*battleUiHistoryClosed = \$true/,
        'every stress step closes and validates its full battle UI-history interval');
    assert.match(proof,
        /Get-ExactLiveBattleUiProof[\s\S]*joinBattleLiveBefore = \$true[\s\S]*joinBattleLiveAfter = \$true[\s\S]*joinWorldReplicationProved = \$true[\s\S]*joinUiBeforeWorldCausalityProved = \$true/,
        'reverse FREE requires live controls and join-world replication, not a cached dialog name');
    assert.equal((reverse.match(/Wait-ExactCrossBattleHostMoveWorld/g) || []).length, 2,
        'join and host independently consume the exact endpoint+MP publication');
    assert.match(exactCrossBattleWorld,
        /Wait-WorldEvidence[\s\S]*Test-ExactWorldStackPositionAndMovement[\s\S]*timed out waiting for \$Role world evidence:[\s\S]*Get-World \$Role[\s\S]*last MP=\{3\}[\s\S]*expected pinned MP=\{4\}/,
        'a transitional wrong-MP endpoint keeps waiting and only a bounded timeout diagnoses the final mismatch');
    assert.doesNotMatch(exactCrossBattleWorld,
        /Move-Stack|Invoke-Button|Complete-CanonicalBattle|retry|refire|fallback/i,
        'the exact world helper is passive and cannot mutate or reissue an action');
    runPowerShellContract(`
${getWorldStack}
${exactWorldStackState}
${exactCrossBattleWorld}
function New-TestWorld([int]$Movement) {
    return [pscustomobject]@{
        worldSeq = $Movement
        stacks = @([pscustomobject]@{
            id = '0xA3E30000'; x = 27; y = 17; movement = $Movement
            owner = '0xA3DE0001'; relation = 'self'; units = 4; hp = 623
            inside = $false; unitIds = @('0xA3E4019D')
            unitStates = @([pscustomobject]@{ id = '0xA3E4019D'; hp = 23 })
        })
    }
}
function Wait-WorldEvidence {
    param([string]$Role, [long]$After, [scriptblock]$Predicate,
        [string]$Description, [object]$HostProcess, [object]$JoinProcess,
        [int]$TimeoutSec)
    foreach ($world in @($script:worlds)) {
        if (& $Predicate $world) { return $world }
    }
    throw "timed out waiting for $Role world evidence: $Description"
}
function Get-World([string]$Role) { return @($script:worlds)[-1] }
$script:worlds = @((New-TestWorld 15), (New-TestWorld 13))
$settled = Wait-ExactCrossBattleHostMoveWorld -Role host -After 1 -HeroId '0xA3E30000' -X 27 -Y 17 -Movement 13 -WorldLabel 'host-world' -Description 'transition regression' -TimeoutSec 30
if ([int]$settled.stacks[0].movement -ne 13) {
    throw 'the transitional MP15 snapshot terminated the exact wait'
}
$script:worlds = @((New-TestWorld 15))
try {
    [void](Wait-ExactCrossBattleHostMoveWorld -Role host -After 1 -HeroId '0xA3E30000' -X 27 -Y 17 -Movement 13 -WorldLabel 'host-world' -Description 'stable mismatch regression' -TimeoutSec 30)
    throw 'a stable wrong-MP endpoint unexpectedly passed'
} catch {
    if ([string]$_.Exception.Message -notmatch
            'last MP=15, expected pinned MP=13') {
        throw
    }
}
Write-Output 'CROSS_WORLD_SETTLE_PASS'
`, 'CROSS_WORLD_SETTLE_PASS',
    'cross-battle endpoint/MP publication ordering regression');
});

test('paired move adapter selects causal attack or back-to-back long fire exactly once', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const relay = fs.readFileSync(relayScript, 'utf8');
    const fire = powerShellFunction(gameplay, 'Invoke-PreparedParallelAttackMoves');
    const ordered = [
        "$hostActions = @($Actions | Where-Object { [string]$_.role -eq 'host' })",
        "$joinActions = @($Actions | Where-Object { [string]$_.role -eq 'join' })",
        '$hostAction = $hostActions[0]',
        '$joinAction = $joinActions[0]',
        '$pairEndpoint = if ($BackToBackLongMove)',
        "'long-move-pair'",
        "'move-pair'",
        "'{0}/api/ui/{1}?'",
        '&joinappearance={14}&joininstance={15}&timeoutMs={16}',
        '$httpTimeoutSeconds =',
        '[math]::Ceiling((2 * $CommandTimeoutMilliseconds) / 1000.0) + 10',
        '$response = Invoke-RestMethod -Method Post -Uri $uri',
        '-TimeoutSec $httpTimeoutSeconds',
        '$response.host.found',
        '$response.join.found'
    ];
    let previous = -1;
    for (const token of ordered) {
        const at = fire.indexOf(token, previous + 1);
        assert.ok(at > previous, `independent parallel attack boundary moved at ${token}`);
        previous = at;
    }
    assert.equal((fire.match(/Invoke-RestMethod -Method Post/g) || []).length, 1,
        'one atomically prevalidated pair request owns the exact two commands');
    assert.match(fire,
        /\[int\]\$CommandTimeoutMilliseconds = 5000[\s\S]*outside 1000\.\.120000 ms/,
        'the pair adapter exposes one bounded timeout without changing legacy callers');
    assert.equal((fire.match(/&timeoutMs=\{16\}/g) || []).length, 1,
        'both commands receive one shared timeout token through their sole POST');
    assert.match(relay,
        /validateQuery\(res, query, required, \['timeoutMs'\]\)[\s\S]*parseCommandTimeout\(res, query\)[\s\S]*parsePairedMapActionTarget\([\s\S]*'host', commandTimeoutMs\)[\s\S]*parsePairedMapActionTarget\([\s\S]*'join', commandTimeoutMs\)/,
        'the relay parses one canonical pair timeout before validating either immutable role target');
    assert.match(relay,
        /issueCommand\(host\.socket, Op\.MoveStack,[\s\S]*host\.commandTimeoutMs[\s\S]*issueCommand\(join\.socket, Op\.MoveStack,[\s\S]*join\.commandTimeoutMs/,
        'the parsed pair budget reaches both exact one-shot command owners');
    assert.match(relay,
        /command seq=\$\{seq\} timed out without a result[\s\S]*role=\$\{roleOf\(pending\.socket\)\} op=\$\{commandOp\}[\s\S]*started=\$\{pending\.startedAtMs !== null\} timeoutMs=\$\{pending\.timeoutMs\}/,
        'terminal command timeout diagnostics retain role, op, started edge, and actual budget');
    assert.doesNotMatch(fire, /\$(?:host|home|pid|args|input|matches)\s*=/i,
        'the attack adapter cannot overwrite a PowerShell automatic/read-only variable');
    assert.match(fire,
        /\$response\.found -isnot \[bool\][\s\S]*\$response\.host\.found -isnot \[bool\][\s\S]*\$response\.join\.found -isnot \[bool\]/,
        'the pair result and both exact role results are typed and required');
    assert.match(fire,
        /if \(\$BackToBackLongMove\)[\s\S]*absolute-relay-command-started-receipt[\s\S]*else \{[\s\S]*relay-command-started-receipt/,
        'long fire accepts either receipt order while attack retains its causal host-first receipt');
    const executableFire = fire.replace(/^\s*#.*$/gm, '');
    assert.doesNotMatch(executableFire,
        /ForEach-Object -Parallel|System\.Threading\.Barrier|SignalAndWait|Start-Sleep|setTimeout|retry|refire|fallback|cancel/i,
        'the causal pair has no shell scheduling race, gameplay delay, or another attempt');
});

test('canonical attack preserves the latest green checkpoint order and independent battle lifecycles', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const prearmedProof = [powerShellFunction(gameplay, 'Read-PrearmedAutoBattleProof')];
    const lifecycle = [powerShellFunction(gameplay, 'Invoke-CanonicalBattleLifecyclesIndependently')];
    const clientLaunch = [powerShellFunction(runner, 'Start-SimturnGameClient')];
    const sharedMovement = [powerShellFunction(gameplay, 'Get-LegacySequentialSharedMovementSnapshot')];
    const quietSnapshot = [powerShellFunction(gameplay, 'Get-LegacyQuietLogSnapshot')];
    const readyRole = [powerShellFunction(gameplay, 'Get-LegacyReadyRoleObservation')];
    const readyQuiet = [powerShellFunction(gameplay, 'Wait-LegacyReadyQuietPair')];
    const preFire = [powerShellFunction(gameplay, 'Assert-CanonicalPreFireSnapshot')];
    const postFire = [powerShellFunction(gameplay, 'Assert-CanonicalPostFireChargeSnapshot')];
    const preparedAttack = [powerShellFunction(gameplay, 'Invoke-PreparedParallelAttackMoves')];
    const attack = [powerShellFunction(gameplay, 'Invoke-CanonicalConcurrentAttacks')];
    const deferred = powerShellFunction(
        gameplay, 'Assert-CanonicalDeferredGameplayEvidence');
    const optionalProperty = powerShellFunction(runner, 'Get-OptionalProperty');
    const getWorldStack = powerShellFunction(gameplay, 'Get-WorldStackExact');
    const legacyRoleHeroSnapshotState = powerShellFunction(
        gameplay, 'Test-LegacyRoleHeroSnapshotState');
    const legacyHeroSnapshotState = powerShellFunction(
        gameplay, 'Test-LegacyHeroSnapshotState');
    const legacyStackSnapshotState = powerShellFunction(
        gameplay, 'Test-LegacyStackSnapshotState');
    assert.ok(prearmedProof && lifecycle &&
        clientLaunch && sharedMovement && quietSnapshot && readyRole && readyQuiet &&
        preFire && postFire && preparedAttack && attack && deferred
        && optionalProperty && getWorldStack
        && legacyRoleHeroSnapshotState && legacyHeroSnapshotState
        && legacyStackSnapshotState,
        'every canonical attack checkpoint must remain independently auditable');

    assert.match(clientLaunch[0],
        /if \(\$GameplayMode -in @\('canonical', 'battle-block', 'long-attack'\)\) \{\s*\$psi\.EnvironmentVariables\['D2TESTDRV_AUTO_BATTLE_PREARM'\] = '1'\s*\}/,
        'canonical, battle-block, and long-attack clients inherit the old pre-boot auto-battle lifetime');
    assert.equal((clientLaunch[0].match(/D2TESTDRV_AUTO_BATTLE_PREARM/g) || []).length, 1,
        'the per-process launcher must have one shared literal prearm site');

    for (const field of [
        'schema', 'preboot-first-battle', 'role', 'succeeded', 'appearance', 'owner',
        'bindAgeMs', 'callbackCount', 'functorVftable', 'dispatchFunction',
        'memberFunction', 'thisAdjustor', 'controllerGateBefore', 'kickStateBefore',
        'kickStateAfter', 'sideSelector', 'flag38Before', 'flag38After',
        'flag39Before', 'flag39After'
    ]) {
        assert.ok(prearmedProof[0].includes(field),
            `preboot PID-log proof lost ${field}`);
    }
    assert.match(prearmedProof[0], /\$proofLines\.Count -gt 1/,
        'more than one native proof for a role must be terminal');
    assert.match(prearmedProof[0], /\[long\]\$proof\.bindAgeMs -ge 2500/);
    assert.match(prearmedProof[0], /\[int\]\$proof\.callbackCount -eq 1/);
    assert.match(prearmedProof[0], /0x006F45D4[\s\S]*0x00644150[\s\S]*0x00635509/,
        'the passive proof must pin all three Russobit callback addresses');
    assert.doesNotMatch(prearmedProof[0], /\$BattleUi\.dialogReady/,
        'the old name-only battle transition must not gain another readiness wait or read');
    assert.match(prearmedProof[0],
        /first raw[\s\S]*native one-shot proof[\s\S]*\$proof\.appearance -eq \$expectedAppearance[\s\S]*\$proof\.owner -eq \$expectedOwner/,
        'the later native proof must bind to the immutable first battle appearance and owner');
    assert.doesNotMatch(prearmedProof[0],
        /Invoke-RestMethod|SendAsync|Start-Sleep|Start-ExactAutoBattleRequest|Complete-ExactAutoBattleRequests/,
        'reading the PID-bound proof must contain no action or added wait');

    assert.equal((quietSnapshot[0].match(/Read-ClientLogLines \$LogPath/g) || []).length, 1,
        'each legacy quiet observation scans one role log exactly once');
    assert.match(quietSnapshot[0], /OBSERVED\|CLAIMED[\s\S]*TickCount64[\s\S]*quietMilliseconds/,
        'the poll retains the latest eligible monotonic popup-action timestamp');
    assert.doesNotMatch(quietSnapshot[0],
        /Start-Sleep|Invoke-(?:RestMethod|Button|Prepared|Parallel|EndTurns)|SendAsync|Move-Stack|Enable-Toggle/,
        'one quiet sample is passive and contains no delay or action');

    assert.match(readyRole[0],
        /Get-OptionalProperty \$State 'connected'[\s\S]*Get-OptionalProperty \$State 'pid'[\s\S]*\$Process\.Id/,
        'the terminal binding capture remains bound to the exact owned process');
    assert.match(readyRole[0],
        /Get-OptionalProperty \$State 'dialogReady'[\s\S]*\$script:BareMapDialogs -notcontains \$dialog[\s\S]*ConvertTo-SavedDialogObservation/,
        'the terminal binding capture must be a typed ready bare-map observation');

    assert.match(readyQuiet[0],
        /\[int\]\$QuietSec = 3,[\s\S]*\[int\]\$TimeoutSec = 45,[\s\S]*\[int\]\$PollMilliseconds = 800/,
        'the embedded wait_day_ready contract pins 3s quiet, 45s timeout, and 800ms polling');
    const readyQuietOrder = [
        '[DateTime]$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)',
        '$resolvedHostState = Get-RoleState host',
        '$resolvedJoinState = Get-RoleState join',
        'while ([DateTime]::UtcNow -lt $deadline)',
        '$hostQuiet = Get-LegacyQuietLogSnapshot host $HostLog $QuietSec',
        '$joinQuiet = Get-LegacyQuietLogSnapshot join $JoinLog $QuietSec',
        'if ([bool]$hostQuiet.ready -and [bool]$joinQuiet.ready)',
        '$hostState = Get-RoleState host',
        '$joinState = Get-RoleState join',
        '$hostObservation = Get-LegacyReadyRoleObservation',
        '$joinObservation = Get-LegacyReadyRoleObservation',
        'preparation = New-CanonicalWalkPreparation',
        'Start-Sleep -Milliseconds 800',
        'throw "legacy ready/quiet wait timed out after ${TimeoutSec}s"'
    ];
    let previousReadyQuiet = -1;
    for (const token of readyQuietOrder) {
        const at = readyQuiet[0].indexOf(token, previousReadyQuiet + 1);
        assert.ok(at > previousReadyQuiet,
            `literal wait_day_ready checkpoint moved or vanished: ${token}`);
        previousReadyQuiet = at;
    }
    assert.equal((readyQuiet[0].match(/Get-RoleState (?:host|join)/g) || []).length, 4,
        'the wait has one source PID census plus one post-quiet MSS binding capture per role');
    assert.match(readyQuiet[0],
        /\$resolvedHostState = Get-RoleState host[\s\S]*\$resolvedJoinState = Get-RoleState join[\s\S]*Get-OptionalProperty \$resolved\.state 'connected'[\s\S]*Get-OptionalProperty \$resolved\.state 'pid'[\s\S]*\$resolved\.process\.Id/,
        'the source-equivalent initial role census must validate both exact owned PIDs');
    const quietLoopStart = readyQuiet[0].indexOf(
        'while ([DateTime]::UtcNow -lt $deadline)');
    const quietSuccessBoundary = readyQuiet[0].indexOf(
        'if ([bool]$hostQuiet.ready -and [bool]$joinQuiet.ready)', quietLoopStart);
    assert.ok(quietLoopStart >= 0 && quietSuccessBoundary > quietLoopStart,
        'the literal quiet-only polling slice must be identifiable');
    assert.doesNotMatch(
        readyQuiet[0].slice(quietLoopStart, quietSuccessBoundary),
        /Get-RoleState|Get-LegacyReadyRoleObservation|Get-World|Get-Dialog/,
        'a failed wait_day_ready sample may only inspect the two hook logs');
    assert.match(readyQuiet[0],
        /if \(\$null -eq \$hostObservation -or \$null -eq \$joinObservation\) \{\s*throw 'legacy quiet boundary did not expose one ready bare-map dialog per owned role'/,
        'a post-quiet MSS binding contradiction must be terminal, never another poll');
    assert.equal((readyQuiet[0].match(/Get-LegacyQuietLogSnapshot (?:host|join)/g) || []).length, 2,
        'one polling sample has exactly one hook-log observation per role');
    assert.equal((readyQuiet[0].match(/Start-Sleep -Milliseconds 800/g) || []).length, 1,
        'a failed passive sample advances only by the original fixed 800ms pause');
    assert.doesNotMatch(readyQuiet[0],
        /Invoke-(?:RestMethod|Button|Prepared|Parallel|EndTurns)|SendAsync|Move-Stack|Enable-Toggle|retry|refire|fallback/i,
        'the bounded readiness loop has no mutation, action replay, or alternate path');

    assert.equal((sharedMovement[0].match(/Get-LegacyStackSnapshot/g) || []).length, 2,
        'legacy Mv84(host), Mv84(join) must remain two distinct reads of the shared host source');
    assert.doesNotMatch(sharedMovement[0], /Get-WorldSnapshot|Get-World\s/,
        'the second legacy read is not a join-local convergence snapshot');
    const sharedReadOrder = [
        '$hostCensus = Get-LegacyStackSnapshot',
        '$hostHero = Get-WorldStackExact $hostCensus ([string]$Fixture.host.heroId)',
        '$joinCensus = Get-LegacyStackSnapshot',
        '$joinHero = Get-WorldStackExact $joinCensus ([string]$Fixture.join.heroId)'
    ];
    let previousSharedRead = -1;
    for (const token of sharedReadOrder) {
        const at = sharedMovement[0].indexOf(token, previousSharedRead + 1);
        assert.ok(at > previousSharedRead,
            `legacy sequential shared read moved or vanished: ${token}`);
        previousSharedRead = at;
    }
    assert.match(preFire[0],
        /\$vitals = Get-LegacySequentialSharedMovementSnapshot \$Fixture[\s\S]*legacy pre-fire display MP=/,
        'the immediate pre-fire checkpoint must preserve the two old display-only reads');
    assert.equal((preFire[0].match(/Get-LegacySequentialSharedMovementSnapshot/g) || []).length, 1);
    assert.doesNotMatch(preFire[0], /Get-World|Get-CanonicalHeroVitals|Wait-/,
        'the pre-fire helper cannot consolidate, repeat, or wait around the two legacy reads');
    assert.doesNotMatch(preFire[0],
        /Fixture\.(?:host|join)\.deploy\.movement|MP=35\/35|throw\b/,
        'sync_attack printed pre-fire MP but did not turn 35/35 into an attack gate');
    assert.match(postFire[0],
        /\$vitals = Get-LegacySequentialSharedMovementSnapshot \$Fixture[\s\S]*legacyRangePass[\s\S]*mass range 0<MP<35/,
        'the exact +1500ms checkpoint preserves two display reads and the later mass-test range observation');
    assert.equal((postFire[0].match(/Get-LegacySequentialSharedMovementSnapshot/g) || []).length, 1);
    assert.doesNotMatch(postFire[0], /Get-World|Get-CanonicalHeroVitals|Wait-/,
        'the +1500ms legacy checkpoint cannot become a consolidated or convergent read');
    assert.doesNotMatch(postFire[0],
        /Fixture\.(?:host|join)\.battleStart\.movement|MP=15\/11|throw\b/,
        'sync_attack printed +1500ms MP but did not require one pinned 15/11 result');
    assert.match(preparedAttack[0],
        /\$pairEndpoint = if \(\$BackToBackLongMove\)[\s\S]*'long-move-pair'[\s\S]*'move-pair'[\s\S]*\/api\/ui\/\{1\}\?/,
        'the attack selects the causal move-pair endpoint after atomically validating both immutable role targets');
    assert.match(preparedAttack[0],
        /\[string\]\$dispatchOrder = if \(\$BackToBackLongMove\)[\s\S]*\$response\.dispatchOrder[\s\S]*else \{\s*''\s*\}[\s\S]*\$dispatchOrder = \$observedDispatchOrder/,
        'canonical move-pair derives order from its started edges without reading the long-move-only response field');
    assert.doesNotMatch(preparedAttack[0], /Get-|Wait-|Start-Sleep|ForEach-Object -Parallel/,
        'the prepared common fire cannot insert an observation, delay, or runspace race after the immediate world proof');
    assert.equal((preparedAttack[0].match(/Invoke-RestMethod -Method Post/g) || []).length, 1,
        'the prepared parallel fire exposes one HTTP mutation site');
    assert.match(attack[0],
        /Invoke-PreparedParallelAttackMoves \$attackIntent\s+`\s*-CommandTimeoutMilliseconds 12000/,
        'the cold canonical pair widens only its one host/join command-result budget');
    assert.equal((attack[0].match(/-CommandTimeoutMilliseconds 12000/g) || []).length, 1,
        'the canonical attack has one explicit twelve-second pair budget');

    const orderedAttackTokens = [
        '$ownedProcessesBeforeAttack = @(',
        '$stackReachability = Get-LegacyStackSnapshot',
        '$hostHeroCensus = Get-LegacyStackSnapshot',
        '$joinHeroCensus = Get-LegacyStackSnapshot',
        '$hostRoleState = Get-RoleState host',
        '$joinRoleState = Get-RoleState join',
        '$attackCensus = Get-LegacyStackSnapshot',
        '$attackReady = Wait-LegacyReadyQuietPair',
        '$quietHostRoleState = $attackReady.hostState',
        '$quietJoinRoleState = $attackReady.joinState',
        '$attackPreparation = $attackReady.preparation',
        '$attackQuiet = @{',
        'host = $attackReady.hostQuiet',
        'join = $attackReady.joinQuiet',
        'Assert-CanonicalPreFireSnapshot $Fixture',
        'Invoke-PreparedParallelAttackMoves $attackIntent',
        'Start-Sleep -Milliseconds 1500',
        'Assert-CanonicalPostFireChargeSnapshot $Fixture',
        'Invoke-CanonicalBattleLifecyclesIndependently',
        '$postBattleMoves = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        '$legacyBattleMarkers.host = Get-LegacyBattleMarkerSnapshot host $HostLog',
        '$legacyBattleMarkers.join = Get-LegacyBattleMarkerSnapshot join $JoinLog',
        '$legacyVerdictWorld = Get-LegacyStackSnapshot',
        '$ownedProcesses = @(Get-Process',
        '$hostCrashLines = @(Read-ClientLogLines $HostLog)',
        '$joinCrashLines = @(Read-ClientLogLines $JoinLog)',
        '$hostBattleUpLines = @(Read-ClientLogLines $HostLog)',
        '$hostBattleCloseLines = @(Read-ClientLogLines $HostLog)',
        '$joinBattleUpLines = @(Read-ClientLogLines $JoinLog)',
        '$joinBattleCloseLines = @(Read-ClientLogLines $JoinLog)',
        '$hostFinalClosed = Get-LegacyBattleClosedSnapshot',
        '$joinFinalClosed = Get-LegacyBattleClosedSnapshot',
        '$battleLifecycles.host.BattleUpLines = @($hostBattleUpLines)',
        '$battleLifecycles.host.BattleCloseLines = @($hostBattleCloseLines)',
        '$battleLifecycles.join.BattleUpLines = @($joinBattleUpLines)',
        '$battleLifecycles.join.BattleCloseLines = @($joinBattleCloseLines)',
        '$legacyPassed = $hostResult.legacyPassed -and $joinResult.legacyPassed',
        'legacy concurrent attacks PASS'
    ];
    let previous = -1;
    for (const token of orderedAttackTokens) {
        const at = attack[0].indexOf(token);
        assert.ok(at > previous, `latest green attack checkpoint moved or vanished: ${token}`);
        previous = at;
    }
    assert.equal((attack[0].match(/Invoke-PreparedParallelAttackMoves/g) || []).length, 1,
        'the common fire barrier must dispatch exactly once');
    assert.equal((attack[0].match(/Start-Sleep -Milliseconds 1500/g) || []).length, 1,
        'the exact post-fire 1500ms checkpoint must occur once');
    assert.equal((attack[0].match(/Wait-LegacyReadyQuietPair/g) || []).length, 1,
        'the canonical attack has one and only one bounded pre-fire readiness wait');
    assert.match(attack[0],
        /Wait-LegacyReadyQuietPair[\s\S]*-QuietSec 3 -TimeoutSec 45 -PollMilliseconds 800/,
        'the attack calls the literal source cadence without caller overrides');
    assert.doesNotMatch(attack[0], /Assert-LegacyQuietLogSnapshot/,
        'the canonical attack cannot regain a one-shot quiet assertion');
    assert.doesNotMatch(attack[0], /Wait-ActionablePair|DismissStartupModals|Invoke-Button/,
        'the bounded passive source gate cannot add an actionable wait or modal action');
    const preFireAt = attack[0].indexOf('Assert-CanonicalPreFireSnapshot $Fixture');
    const fireAt = attack[0].indexOf('Invoke-PreparedParallelAttackMoves $attackIntent');
    const preFireGap = attack[0].slice(preFireAt, fireAt);
    assert.doesNotMatch(preFireGap, /Get-|Wait-|Start-Sleep|Assert-(?!CanonicalPreFireSnapshot)/,
        'no observation or wait may separate the immediate legacy MP display from fire');
    assert.equal((attack[0].match(/Get-LegacyStackSnapshot/g) || []).length, 5,
        'attack entry and final verdict retain four distinct pre-fire host censuses and one post-battle census');
    assert.equal((attack[0].match(/Get-RoleState (?:host|join)/g) || []).length, 2,
        'the attack caller retains its initial PID-role pair; the quiet pair lives at one polling site');
    assert.equal((attack[0].match(/\$battleLifecycles\.(?:host|join)\.Battle(?:Up|Close)Lines = @\(\$(?:host|join)Battle(?:Up|Close)Lines\)/g) || []).length, 4,
        'deferred exact battle proof must retain all four already-read final legacy log snapshots');
    const postSleepAt = attack[0].indexOf('Start-Sleep -Milliseconds 1500');
    const postProofAt = attack[0].indexOf('Assert-CanonicalPostFireChargeSnapshot $Fixture');
    const postFireGap = attack[0].slice(postSleepAt, postProofAt);
    assert.doesNotMatch(postFireGap, /Assert-ClientsLive|Get-|Wait-/,
        'the two legacy post-fire MP reads must immediately follow the 1500ms window');

    const battleUpReadAt = lifecycle[0].indexOf('$battleUpLines = @(Read-ClientLogLines');
    const battleCloseReadAt = lifecycle[0].indexOf('$battleCloseLines = @(Read-ClientLogLines');
    const closedSnapshotAt = lifecycle[0].indexOf('Get-LegacyBattleClosedSnapshot');
    assert.ok(battleUpReadAt >= 0 && battleCloseReadAt > battleUpReadAt &&
        closedSnapshotAt > battleCloseReadAt,
    'each fighting role preserves the old battle-up read, battle-close read, then Battle-Closed predicate');
    assert.doesNotMatch(lifecycle[0],
        /Read-PrearmedAutoBattleProof|New-CanonicalBattleCompletionState|Step-CanonicalBattleCompletionState|Start-ExactAutoBattleRequest|Complete-ExactAutoBattleRequests|\/api\/ui\/enable-auto-battle|SendAsync|Invoke-RestMethod/,
        'the literal resolver observes Battle-Closed only; exact MSS/native proof is deferred and no battle action is issued');
    assert.match(lifecycle[0],
        /\$peers = @\([\s\S]*role = 'host'; phase = 'moving'[\s\S]*role = 'join'; phase = 'moving'[\s\S]*foreach \(\$peer in \$peers\)/,
        'one parent loop must service the separately tracked host and join state machines');
    assert.match(lifecycle[0],
        /steps = 0[\s\S]*\$maxSteps = 6[\s\S]*\$distance -le 1[\s\S]*\[int\]\$peer\.steps -lt \$maxSteps[\s\S]*Move-Stack \$role[\s\S]*gave up after \$maxSteps continuation steps/,
        'the literal resolver must retain the old maximum-six partial-route continuation logic');
    assert.equal((lifecycle[0].match(/Move-Stack \$role/g) || []).length, 1,
        'one continuation call site issues each newly addressed route leg at most once');
    assert.match(lifecycle[0], /\[int\]\$TimeoutSec = 150/,
        'the latest green resolver has one exact 150-second deadline');
    assert.doesNotMatch(lifecycle[0], /Wait-CanonicalConcurrentBattleStartConvergence|Task\.WaitAll|Task\.WhenAll|\.Result\b/,
        'auto-battle and result handling must not regain a both-ready barrier');
    assert.doesNotMatch(gameplay, /\.Cancel(?:After)?\s*\(/,
        'the gameplay harness must never cancel an issued action');
    assert.match(attack[0],
        /Invoke-CanonicalBattleLifecyclesIndependently[\s\S]*-HostLog \$HostLog -JoinLog \$JoinLog/,
        'the resolver proof must remain bound to the two exact owned-PID logs');
    assert.equal((lifecycle[0].match(/Start-Sleep -Seconds 2/g) || []).length, 2,
        'the resolver must retain both its unconditional iteration tail and separate post-loop +2');
    assert.doesNotMatch(lifecycle[0], /TerminalRawUi|lastRawUi/,
        'the iteration-head dialog is control flow only and cannot masquerade as a post-close map');
    const loopTailAt = lifecycle[0].indexOf('Start-Sleep -Seconds 2');
    const terminalProjectionAt = lifecycle[0].indexOf(
        '# The source did not throw at its resolver deadline.', loopTailAt);
    const postLoopAt = lifecycle[0].indexOf('Start-Sleep -Seconds 2', loopTailAt + 1);
    assert.ok(loopTailAt >= 0 && terminalProjectionAt > loopTailAt &&
        postLoopAt > terminalProjectionAt,
    'the terminal resolver iteration sleeps +2, preserves timeout as evidence, then performs the old post-loop +2');
    assert.equal((attack[0].match(/Start-Sleep -Seconds 2/g) || []).length, 0,
        'the caller cannot replace or duplicate either two-second resolver checkpoint');

    const legacyPassAt = attack[0].indexOf('legacy concurrent attacks PASS:');
    const mssExtensionAt = attack[0].indexOf('$deferredEvidence = [pscustomobject]@{');
    assert.ok(legacyPassAt >= 0 && mssExtensionAt > legacyPassAt,
        'saved MSS evidence must be packaged only after the literal legacy attack PASS');
    const prePassAttack = attack[0].slice(0, legacyPassAt);
    assert.doesNotMatch(prePassAttack, /Wait-CanonicalWorldConvergence/,
        'late convergence must not be allowed to turn a failed legacy checkpoint into PASS');
    assert.doesNotMatch(attack[0].slice(legacyPassAt, mssExtensionAt),
        /Get-(?!ObservedWalkPreparationRole|OptionalProperty)|Read-|Wait-|Start-Sleep|Invoke-RestMethod|Assert-(?:Clients|NoClient)/,
        'only pure next-walk address projection may occur between legacy PASS and saved MSS packaging');
    assert.doesNotMatch(attack[0].slice(mssExtensionAt),
        /Get-World|Get-RoleState|Get-RelayState|Read-|Wait-|Start-Sleep|Invoke-RestMethod/,
        'deferred MSS packaging is a pure transform of evidence already captured by the literal phase');

    const deferredAttackAt = deferred.indexOf("'attack' {");
    const deferredWalkAt = deferred.indexOf("'walk' {", deferredAttackAt);
    assert.ok(deferredAttackAt >= 0 && deferredWalkAt > deferredAttackAt,
        'the deferred attack consumer must remain independently auditable');
    const deferredAttack = deferred.slice(deferredAttackAt, deferredWalkAt);
    assert.doesNotMatch(deferredAttack,
        /TerminalRawUi|ConvertTo-SavedDialogObservation|Get-CanonicalWalkPreparationRole|BareMapDialogs/,
        'Battle-Closed proves the native close chain, not readiness of its earlier raw UI publication');
    const postBattleAt = deferredAttack.indexOf(
        '$postBattleSamples = @($item.postBattleSamples)');
    const verdictAt = deferredAttack.indexOf(
        'Test-LegacyStackSnapshotState `', postBattleAt);
    assert.ok(postBattleAt >= 0 && verdictAt > postBattleAt,
        'post-battle movement samples and the later verdict census remain separate oracles');
    const postBattleOracle = deferredAttack.slice(postBattleAt, verdictAt);
    assert.match(postBattleOracle,
        /\$postBattleSamples\.Count -ne 2[\s\S]*Test-LegacyRoleHeroSnapshotState[\s\S]*\$postBattleSamples\[0\] \$Fixture host[\s\S]*\$Fixture\.host\.battleEnd[\s\S]*Test-LegacyRoleHeroSnapshotState[\s\S]*\$postBattleSamples\[1\] \$Fixture join[\s\S]*\$Fixture\.join\.battleEnd/,
        'source-equivalent post-battle reads validate host only in the first census and join only in the second');
    assert.doesNotMatch(postBattleOracle,
        /Test-LegacyStackSnapshotState|HostTargetPresent|JoinTargetPresent|\.target/,
        'an earlier raw post-battle sample cannot require converged target absence');
    assert.match(deferredAttack.slice(verdictAt),
        /\$item\.legacyVerdictWorld \$Fixture[\s\S]*\$Fixture\.host\.battleEnd \$Fixture\.join\.battleEnd \$false \$false/,
        'the one final legacy verdict census remains strict about both defeated targets');
    assert.doesNotMatch(legacyRoleHeroSnapshotState,
        /\.target|HostTargetPresent|JoinTargetPresent/,
        'the role-scoped hero validator cannot acquire target census semantics');
    assert.match(legacyHeroSnapshotState,
        /Test-LegacyRoleHeroSnapshotState[\s\S]*host \$HostExpected[\s\S]*Test-LegacyRoleHeroSnapshotState[\s\S]*join \$JoinExpected/,
        'the strict two-hero validator composes both role-scoped source reads');
    assert.match(legacyStackSnapshotState,
        /Test-LegacyHeroSnapshotState[\s\S]*Fixture\.host\.target[\s\S]*present = \$HostTargetPresent[\s\S]*Fixture\.join\.target[\s\S]*present = \$JoinTargetPresent/,
        'the strict validator composes hero state with both explicit target-presence requirements');
    runPowerShellContract(`
Set-StrictMode -Version Latest
${optionalProperty}
${getWorldStack}
${legacyRoleHeroSnapshotState}
${legacyHeroSnapshotState}
${legacyStackSnapshotState}
function New-Snapshot {
    param([long]$Sequence, [int]$HostX, [int]$JoinX)
    [pscustomobject]@{
        sourceRole='host'; sequence=$Sequence
        stacks=@(
            [pscustomobject]@{ id='host-hero'; owner='0xA3DE0001'; x=$HostX; y=10; movement=15 },
            [pscustomobject]@{ id='join-hero'; owner='0xA3DE0002'; x=$JoinX; y=20; movement=11 }
        )
    }
}
$fixture=[pscustomobject]@{
    players=[pscustomobject]@{
        hostHandle='0xA3DE0001'; joinHandle='0xA3DE0002'; neutralHandle='0xA3DE0000'
    }
    host=[pscustomobject]@{
        heroId='host-hero'; target=[pscustomobject]@{ id='host-target'; x=30; y=30 }
    }
    join=[pscustomobject]@{
        heroId='join-hero'; target=[pscustomobject]@{ id='join-target'; x=40; y=40 }
    }
}
$hostExpected=[pscustomobject]@{ x=28; y=10; movement=15 }
$joinExpected=[pscustomobject]@{ x=18; y=20; movement=11 }
$hostRead=New-Snapshot 367 28 17
$joinRead=New-Snapshot 368 27 18
$verdict=New-Snapshot 369 28 18
if (-not (Test-LegacyRoleHeroSnapshotState $hostRead $fixture host $hostExpected)) {
    throw 'first source read rejected its exact host hero'
}
if (-not (Test-LegacyRoleHeroSnapshotState $joinRead $fixture join $joinExpected)) {
    throw 'second source read rejected its exact join hero'
}
if ((Test-LegacyHeroSnapshotState $hostRead $fixture $hostExpected $joinExpected) -or
    (Test-LegacyHeroSnapshotState $joinRead $fixture $hostExpected $joinExpected)) {
    throw 'a transitional source read unexpectedly satisfied the strict two-hero oracle'
}
if (-not (Test-LegacyStackSnapshotState $verdict $fixture $hostExpected $joinExpected $false $false)) {
    throw 'final exact verdict rejected both settled heroes and absent targets'
}
'source-sequential post-battle oracle PASS'
`, 'source-sequential post-battle oracle PASS',
    'post-battle source reads tolerate only the unconsumed peer projection');
});

test('inline-patch freeze accepts an exact x86 instruction boundary without retrying', () => {
    const patches = fs.readFileSync(nativePatchesScript, 'utf8');
    const instructionGuard = patches.match(
        /static bool instructionPointerTouches[\s\S]*?(?=\n    bool collectNewThreads)/);
    assert.ok(instructionGuard,
        'the inline-patch instruction-pointer guard must remain identifiable');
    assert.match(instructionGuard[0], /instruction > site->address/,
        'EIP at the first byte is a safe pre-instruction boundary');
    assert.doesNotMatch(instructionGuard[0], /instruction >= site->address/,
        'an exact instruction boundary must not cause a timing-dependent activation failure');
});

test('canonical battle completion preserves the live -> result -> world-verdict contract', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const state = powerShellFunction(gameplay, 'New-CanonicalBattleCompletionState');
    const resultPhase = powerShellFunction(gameplay, 'Get-CanonicalBattleResultControlState');
    const eventGate = powerShellFunction(gameplay, 'Assert-CanonicalBattleUiEvent');
    const resultTarget = powerShellFunction(gameplay, 'Get-CanonicalBattleResultTarget');
    const step = powerShellFunction(gameplay, 'Step-CanonicalBattleCompletionState');
    const lifecycle = powerShellFunction(gameplay, 'Invoke-CanonicalBattleLifecyclesIndependently');
    assert.ok(state && resultPhase && eventGate && resultTarget && step && lifecycle,
        'the canonical battle state-machine functions must remain independently identifiable');

    assert.match(state, /LiveBattleAppearance[\s\S]*LiveBattleOwner[\s\S]*UiCursor/,
        'the initial live appearance, native owner and append-only cursor are distinct evidence');
    assert.match(state, /ResultUiSequence = \[long\]0/,
        'the append-only result-control transition must begin unclaimed');
    assert.doesNotMatch(gameplay, /LiveBattleAppearance \+ 1|ResultBattleAppearance|ResultBattleOwner/,
        'result controls rebind on the live CBattleViewerInterf, not a synthetic second owner');
    assert.match(eventGate,
        /\$appearance -ne \[long\]\$State\.LiveBattleAppearance[\s\S]*\$owner -ne \[long\]\$State\.LiveBattleOwner/,
        'every battle event must retain the exact live native identity');
    assert.match(eventGate, /ResultUiSequence = \$sequence/,
        'the result phase must be claimed from append-only UI evidence');
    assert.match(eventGate, /left DLG_BATTLE_A[\s\S]*before result controls were observed/,
        'leaving the live/result chain before native result controls is terminal');

    assert.match(resultPhase, /\$closeByName\.Count -ne 1/,
        'the preserved result owner must expose exactly one BTN_CLOSE');
    assert.match(resultPhase, /liveControlNames[\s\S]*liveControls\.Count -ne 0/,
        'the result phase requires the live battle controls to be retired');
    assert.match(resultPhase, /enabledProperty\.Value -isnot \[bool\]/,
        'BTN_CLOSE readiness must be a typed observation, never an omitted-state fallback');
    assert.doesNotMatch(resultPhase, /BTN_PAPERDOLL/,
        'a corroborating control from one run must not become a new legacy test step');
    assert.match(resultTarget, /LiveBattleAppearance[\s\S]*LiveBattleOwner/,
        'BTN_CLOSE must target the same exact battle appearance and owner');
    const literalAt = step.indexOf('if ($literalSnapshotStep) {');
    const genericAt = step.indexOf('$history = Get-UiHistory', literalAt);
    assert.ok(literalAt >= 0 && genericAt > literalAt,
        'the literal saved-snapshot branch must precede the generic observer branch');
    const literalStep = step.slice(literalAt, genericAt);
    const literalOrder = [
        'if ($null -eq $SavedAutoBattleProof) { return }',
        '$State.BattleUpProof = $SavedAutoBattleProof',
        'Test-LegacyBattleCloseCommitted',
        'Assert-ScriptedBattleCloseProof',
        'ConvertTo-SavedDialogObservation $role $RawUi',
        '$script:BareMapDialogs -notcontains',
        '$State.TerminalMapObservation = $observation',
        '$State.Done = $true'
    ];
    let previousLiteral = -1;
    for (const token of literalOrder) {
        const at = literalStep.indexOf(token, previousLiteral + 1);
        assert.ok(at > previousLiteral, `literal battle-close checkpoint moved or vanished: ${token}`);
        previousLiteral = at;
    }
    assert.doesNotMatch(literalStep,
        /Get-(?:UiHistory|DialogObservation|World|GameUiSnapshot)|Read-ClientLogLines|Invoke-(?:Button|RestMethod)|Start-Sleep|Wait-/,
        'the literal completion step is pure over its five predecessor-saved inputs');
    assert.doesNotMatch(step, /Invoke-Button|Invoke-RestMethod|SendAsync/,
        'PowerShell never owns the native subscriber\'s sole battle/result action');
    assert.doesNotMatch(step, /MapSince|TotalSeconds -ge 3/,
        'the old resolver did not add a synthetic three-second bare-map settle');
    assert.match(step,
        /nativeOwnedPostBattleDialogs = @\([\s\S]*DLG_MANAGE_STACK[\s\S]*DLG_MESSAGE_BOX[\s\S]*DLG_ITEM[\s\S]*DLG_EVENT_POPUP[\s\S]*-notcontains \$dialog/,
        'post-battle startup dialogs remain an explicit read-only native-owned allowlist');
    assert.doesNotMatch(step, /Invoke-StrictDialogForward/,
        'PowerShell must not race the preboot native subscriber on a post-battle popup');

    const lifecycleOrder = [
        '$rawUi = Get-GameUiSnapshot $role',
        '$battleUpLines = @(Read-ClientLogLines ([string]$config.log))',
        '$battleCloseLines = @(Read-ClientLogLines ([string]$config.log))',
        '$closed = Get-LegacyBattleClosedSnapshot',
        '$peer.state.BattleUpLines = @($battleUpLines)',
        '$peer.state.BattleCloseLines = @($battleCloseLines)',
        'if ([bool]$closed.closed) {',
        '$peer.state.BattleClosed = $true',
        "$peer.phase = 'done'",
        'Start-Sleep -Seconds 2'
    ];
    let previousLifecycle = -1;
    for (const token of lifecycleOrder) {
        const at = lifecycle.indexOf(token, previousLifecycle + 1);
        assert.ok(at > previousLifecycle, `literal resolver checkpoint moved or vanished: ${token}`);
        previousLifecycle = at;
    }
    assert.equal((lifecycle.match(/Read-ClientLogLines/g) || []).length, 2,
        'each fighting-role iteration retains two separate physical log reads');
    assert.equal((lifecycle.match(/Get-GameUiSnapshot \$role/g) || []).length, 1,
        'each unfinished role receives one raw GetLastDlg-equivalent read at iteration head');
    assert.equal((lifecycle.match(/Start-Sleep -Seconds 2/g) || []).length, 2,
        'the terminal iteration tail and distinct post-loop settle remain separate');
    assert.doesNotMatch(lifecycle,
        /Read-PrearmedAutoBattleProof|Step-CanonicalBattleCompletionState|Invoke-Button|Invoke-RestMethod|SendAsync|Start-ExactAutoBattleRequest|Complete-ExactAutoBattleRequests/,
        'the literal resolver observes only the old Battle-Closed predicate; exact native proof is deferred and no action is issued');
});

test('canonical walks keep the old settle/census/fire/result order and bind the current MSS map once', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const walk = powerShellFunction(gameplay, 'Invoke-CanonicalParallelWalk');
    const stray = powerShellFunction(gameplay, 'Get-LegacyWalkStrayResult');
    const preparation = powerShellFunction(
        gameplay, 'New-CanonicalWalkPreparationFromRelayState');
    const deferred = powerShellFunction(
        gameplay, 'Assert-CanonicalDeferredGameplayEvidence');
    const legacyRoleHeroSnapshotState = powerShellFunction(
        gameplay, 'Test-LegacyRoleHeroSnapshotState');
    const legacyHeroSnapshotState = powerShellFunction(
        gameplay, 'Test-LegacyHeroSnapshotState');
    const legacySnapshotState = powerShellFunction(
        gameplay, 'Test-LegacyStackSnapshotState');
    const getWorldStack = powerShellFunction(gameplay, 'Get-WorldStackExact');
    const productionPoc = fs.readFileSync(productionPocScript, 'utf8');
    const optionalProperty = powerShellFunction(
        productionPoc, 'Get-OptionalProperty');
    assert.ok(walk && stray && preparation && deferred,
        'the shared post-battle/day-2 walk and its one-shot capability adapter must remain auditable');
    const ordered = [
        'Start-Sleep -Seconds 3',
        '$planCensus = Get-LegacyStackSnapshot',
        '$walkActionState = Get-RelayState',
        '$walkActionPreparation = New-CanonicalWalkPreparationFromRelayState',
        '$walkIntent = @(',
        'Invoke-PreparedParallelWorldMoves $walkIntent',
        'Start-Sleep -Seconds 4',
        '$hostResultWorld = Get-LegacyStackSnapshot',
        '$joinResultWorld = Get-LegacyStackSnapshot',
        '$strayResult = Get-LegacyWalkStrayResult',
        '$legacyWalkSucceeded = $hostMoved -and $joinMoved -and',
        '$Phase legacy PASS:'
    ];
    let previous = -1;
    for (const token of ordered) {
        const at = walk.indexOf(token);
        assert.ok(at > previous, `canonical walk checkpoint moved or vanished: ${token}`);
        previous = at;
    }
    assert.equal((walk.match(/Invoke-PreparedParallelWorldMoves/g) || []).length, 1,
        'each walk phase has one common fire barrier and no retry site');
    assert.equal((walk.match(/Start-Sleep -Seconds 3/g) || []).length, 1,
        'each old walk invocation executes its unconditional three-second pre-fire settle exactly once');
    assert.equal((walk.match(/Start-Sleep -Seconds 4/g) || []).length, 1,
        'the exact four-second post-fire window occurs once');
    assert.equal((walk.match(/Get-LegacyStackSnapshot/g) || []).length, 3,
        'one plan census and two ordered post-fire result reads remain physically separate');
    assert.equal((walk.match(/Get-RelayState/g) || []).length, 1,
        'the MSS adapter captures both current map capabilities in one aggregate read');
    assert.doesNotMatch(walk, /PreparedMetadata|postBattleWalkPreparation/,
        'a predecessor battle/turn observation is historical evidence, never a future move capability');
    assert.match(walk,
        /appearance = \[long\]\$walkActionPreparation\.host\.appearance[\s\S]*appearance = \[long\]\$walkActionPreparation\.join\.appearance/,
        'both one-shot moves consume only the current aggregate map publication');
    assert.match(preparation,
        /Assert-DebugRelayClientIdentity host[\s\S]*Assert-DebugRelayClientIdentity join[\s\S]*New-CanonicalWalkPreparation/,
        'the aggregate capability must fail closed on both exact-owned roles and exact ready maps');
    assert.doesNotMatch(preparation,
        /Get-RelayState|Get-RoleState|Invoke-RestMethod|Start-Sleep|Wait-|retry|fallback/i,
        'projecting the one aggregate publication is pure and cannot refresh, wait, or recover');
    const settleAt = walk.indexOf('Start-Sleep -Seconds 3');
    const planAt = walk.indexOf('$planCensus = Get-LegacyStackSnapshot', settleAt);
    assert.doesNotMatch(walk.slice(settleAt, planAt), /Get-|Read-|Wait-|Assert-|Invoke-/,
        'the shared plan census is the first observation after the exact +3 settle');
    const fireAt = walk.indexOf('Invoke-PreparedParallelWorldMoves $walkIntent');
    const fourAt = walk.indexOf('Start-Sleep -Seconds 4', fireAt);
    const hostResultAt = walk.indexOf('$hostResultWorld = Get-LegacyStackSnapshot', fourAt);
    assert.doesNotMatch(walk.slice(fireAt, fourAt), /Get-|Read-|Wait-|Assert-|Start-Sleep/,
        'nothing observes or delays the route between the sole fire and fixed +4 window');
    assert.doesNotMatch(walk.slice(fourAt, hostResultAt), /Get-|Read-|Wait-|Assert-|Invoke-/,
        'the first result read immediately follows the exact +4 window');
    assert.doesNotMatch(walk, /Move-Stack|Invoke-OneExactFixtureMove|Invoke-Button|Enable-Toggle/i,
        'a failed walk is terminal; the green-path phase has no alternate action site');
    assert.doesNotMatch(walk, /Wait-ActionablePair|DismissStartupModals|Wait-WorldEvidence|Wait-CanonicalWorldConvergence|Wait-CleanWalkUiWindow/,
        'the exact walk uses the fixed +3/+4 windows and never adds a gate, modal action, or late result wait');
    assert.match(deferred,
        /hostResult = \$hostActual[\s\S]*joinResult = \$joinActual|savedResult = \$item\."\$\{role\}Result"/,
        'deferred MSS validation must retain the actual successful source walk result');
    assert.doesNotMatch(deferred,
        /Fixture\.(?:host|join)\.\$phase\.movement|fixture MP mismatch/i,
        'an RNG-dependent historical MP number cannot become a stronger walk gate than moved+charged');
    const deferredWalkAt = deferred.indexOf("'walk' {");
    const deferredDefaultAt = deferred.indexOf('default {', deferredWalkAt);
    const deferredWalk = deferred.slice(deferredWalkAt, deferredDefaultAt);
    assert.ok(deferredWalkAt >= 0 && deferredDefaultAt > deferredWalkAt,
        'the deferred walk consumer must remain independently auditable');
    assert.match(deferredWalk,
        /Test-LegacyStackSnapshotState \$item\.planCensus[\s\S]*foreach \(\$world in @\(\$item\.hostResultWorld, \$item\.joinResultWorld\)\)[\s\S]*Test-LegacyStackSnapshotState \$world/,
        'all three source /api/legacy-stacks censuses must retain their legacy-shape validator');
    assert.doesNotMatch(deferredWalk, /Test-CanonicalWorldState/,
        'a legacy stack census has no day/inside/relation and cannot be consumed as /api/world');
    assert.match(deferredWalk, /Assert-DeferredCanonicalHistoryContainsState/,
        'the separate append-only MSS world history must retain the full day/state proof');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Assert-LegacySharedWalkPlanMatchesFixture { param($Plan, $Fixture) }
function Assert-DeferredCanonicalHistoryContainsState {
    param($Role, $After, $Fixture, $Day, $HostExpected, $JoinExpected,
          $HostTargetPresent, $JoinTargetPresent, $Description)
}
function Write-Step { param([string]$Message); $Message }
${optionalProperty}
${getWorldStack}
${legacyRoleHeroSnapshotState}
${legacyHeroSnapshotState}
${legacySnapshotState}
${deferred}
function New-LegacySnapshot {
    param([long]$Sequence, [int]$HostX, [int]$HostY, [int]$HostMovement,
          [int]$JoinX, [int]$JoinY, [int]$JoinMovement)
    [pscustomobject]@{
        sourceRole = 'host'
        sequence = $Sequence
        stacks = @(
            [pscustomobject]@{ id='host-hero'; owner='0xA3DE0001'; x=$HostX; y=$HostY; movement=$HostMovement },
            [pscustomobject]@{ id='join-hero'; owner='0xA3DE0002'; x=$JoinX; y=$JoinY; movement=$JoinMovement }
        )
    }
}
$fixture = [pscustomobject]@{
    players = [pscustomobject]@{
        hostHandle='0xA3DE0001'; joinHandle='0xA3DE0002'; neutralHandle='0xA3DE0000'
    }
    host = [pscustomobject]@{
        heroId='host-hero'; target=[pscustomobject]@{ id='host-target'; x=30; y=30 }
        battleEnd=[pscustomobject]@{ x=10; y=10; movement=20 }
        postBattleWalk=[pscustomobject]@{ x=11; y=10 }
    }
    join = [pscustomobject]@{
        heroId='join-hero'; target=[pscustomobject]@{ id='join-target'; x=40; y=40 }
        battleEnd=[pscustomobject]@{ x=20; y=20; movement=18 }
        postBattleWalk=[pscustomobject]@{ x=20; y=21 }
    }
}
$item = [pscustomobject]@{
    kind='walk'; phase='postBattleWalk'; expectedDay=1
    walkPlan=[pscustomobject]@{
        host=[pscustomobject]@{ movementBefore=20 }
        join=[pscustomobject]@{ movementBefore=18 }
    }
    planCensus=New-LegacySnapshot 1 10 10 20 20 20 18
    hostResult=[pscustomobject]@{ moved=$true; charged=$true; x=11; y=10; movement=14 }
    joinResult=[pscustomobject]@{ moved=$true; charged=$true; x=20; y=21; movement=12 }
    hostResultWorld=New-LegacySnapshot 2 11 10 14 20 21 12
    joinResultWorld=New-LegacySnapshot 3 11 10 14 20 21 12
    hostWorldAfter=[long]1; joinWorldAfter=[long]1
}
$evidence = @($item)
Assert-CanonicalDeferredGameplayEvidence -Fixture $fixture -Evidence $evidence
`, 'deferred postBattleWalk MSS extension PASS',
    'deferred walk proof over the exact legacy stack-snapshot shape');

    const strayOrder = [
        '$state = Get-RelayState',
        "foreach ($role in @('host', 'join'))",
        '$upHistory = Get-UiHistorySnapshot $role $cursor',
        '$closeHistory = Get-UiHistorySnapshot $role $cursor'
    ];
    let previousStray = -1;
    for (const token of strayOrder) {
        const at = stray.indexOf(token, previousStray + 1);
        assert.ok(at > previousStray, `post-walk stray-check read moved or vanished: ${token}`);
        previousStray = at;
    }
    assert.equal((stray.match(/Get-RelayState/g) || []).length, 1,
        'the post-walk check performs one shared peer-state census');
    assert.equal((stray.match(/Get-UiHistorySnapshot \$role \$cursor/g) || []).length, 3,
        'the role loop retains separate battle-up/battle-close reads plus one passive close-poll site');
    assert.match(stray,
        /if \(\$lastBattle\.Count -eq 1\)[\s\S]*\$stray = \$true[\s\S]*for \(\$waited = 0; \$waited -lt 90; \$waited \+= 5\)[\s\S]*Start-Sleep -Seconds 5[\s\S]*\$polledCloseHistory = Get-UiHistorySnapshot \$role \$cursor/,
        'an actual stray battle keeps the old passive +5 through +90 Battle-Closed observation loop');
    assert.doesNotMatch(stray,
        /Get-Process|Get-World|Get-GameUi|Invoke-(?:Button|RestMethod)|Move-Stack|Enable-Toggle|refire|fallback/i,
        'the post-hoc stray check has no process/world/current-UI read and never issues another action');
});

test('standalone long move and attack preserve one-shot source intent without fallbacks', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const fixture = JSON.parse(fs.readFileSync(simturnsRussobitFixture, 'utf8'));
    const longMove = powerShellFunction(gameplay, 'Invoke-LongMoveProof');
    const sourceRoute = powerShellFunction(gameplay, 'Invoke-LongMoveSourceRouteControl');
    const sourcePair = powerShellFunction(gameplay, 'Observe-LongMoveSourcePair');
    const moveCompletion = powerShellFunction(gameplay, 'Wait-CleanLongMoveCompletion');
    const busyOverlap = powerShellFunction(gameplay, 'Assert-CleanLongMoveBusyOverlap');
    const trajectory = powerShellFunction(gameplay, 'Wait-LongMoveTrajectoryEvidence');
    const noBattle = powerShellFunction(gameplay, 'Assert-NoLongMoveBattle');
    const finalWorlds = powerShellFunction(gameplay, 'Assert-LongMoveFinalWorlds');
    const longAttack = powerShellFunction(gameplay, 'Invoke-LongAttackProof');
    const movementStart = powerShellFunction(gameplay, 'Get-LongAttackMovementStart');
    const terminalWorld = powerShellFunction(gameplay, 'Test-LongAttackTerminalWorld');
    const worldConvergence = powerShellFunction(gameplay, 'Wait-LongAttackWorldConvergence');

    assert.deepEqual(fixture.host.longMove, { x: 20, y: 18 });
    assert.deepEqual(fixture.join.longMove, { x: 20, y: 22 });
    assert.deepEqual(fixture.host.cleanLongMove, { x: 37, y: 21, movement: 1 });
    assert.deepEqual(fixture.join.cleanLongMove, { x: 11, y: 38, movement: 2 });
    assert.deepEqual(fixture.host.longAttack.target,
        { id: '0xA3E30060', x: 22, y: 8 });
    assert.deepEqual(fixture.join.longAttack.target,
        { id: '0xA3E3001D', x: 17, y: 30 });

    assert.equal((longMove.match(/Invoke-PreparedParallelAttackMoves/g) || []).length, 1,
        'long-move owns one exact causal move-pair fire site');
    assert.match(longMove,
        /dispatchSkewKind = \[string\]\$fire\.dispatchSkewKind/,
        'long-move labels its skew as CommandStarted relay receipt evidence');
    assert.equal((sourceRoute.match(/Move-Stack \$Role/g) || []).length, 1,
        'all single-route cases share exactly one role-selected move site');
    assert.doesNotMatch(sourceRoute,
        /Move-Stack (?:host|join)|Invoke-PreparedParallelAttackMoves|retry|refire|fallback|cancel/i,
        'single-route controls have no second, repeated, or alternate action');
    assert.match(sourceRoute, /join only[\s\S]*host idle/,
        'the historical route-control diagnostic explicitly keeps host idle');
    assert.match(sourceRoute,
        /source-route-control contract is exactly join \+ longMove \+ diagnostic/,
        'the generalized helper pins the historical case to its old role/target semantics');
    assert.match(sourceRoute, /attemptedActions = 1/,
        'the route-control diagnostic records its sole action');
    assert.match(sourceRoute, /acceptanceClaim = \$false/,
        'the route-control diagnostic cannot claim long-move acceptance');
    assert.match(sourceRoute, /recoveryActions = 0/,
        'the route-control diagnostic records zero recovery actions');
    assert.match(sourceRoute, /\[int\]\$settleMilliseconds = 1200/,
        'the route control retains the exact source quiet edge');
    assert.match(sourceRoute,
        /Get-WorldHistory host[\s\S]*Start-Sleep -Milliseconds 250/,
        'the route control passively observes the append-only world reporter');
    assert.doesNotMatch(sourceRoute, /Wait-WorldEvidence/,
        'zero world effect remains a structured diagnostic result instead of a thrown wait');
    assert.match(sourceRoute,
        /\$routeValid = \$positionChanged -and \$movementSpent -gt 0 -and[\s\S]*\$settled -and \$converged -and -not \$battleObserved -and[\s\S]*-not \$hostWorldChanged/,
        'route validity requires charged movement, convergence, quiet, no battle, and idle host world');
    assert.match(sourceRoute, /COMPLETE \(diagnostic only\)/,
        'classification completion is not mislabeled as an acceptance pass');
    assert.match(sourceRoute,
        /\$StrictCleanRoute[\s\S]*Get-WorldStackExact \$event \$idleHeroId[\s\S]*\$lastAnyChangeUtc = \$eventUtc/,
        'strict single-route quiet settle observes active and idle hero changes');
    assert.match(sourceRoute,
        /\$targetReached =[\s\S]*\$movementSpent -lt 30[\s\S]*\$idleWorldChanged[\s\S]*world did not settle[\s\S]*world views did not converge/,
        'clean controls require exact target, substantial charge, idle stability, quiet, and convergence');
    assert.match(sourceRoute,
        /Get-UiHistory \$side[\s\S]*DLG_BATTLE_A[\s\S]*BareMapDialogs -notcontains \$dialog[\s\S]*\$popupObserved = \$true[\s\S]*dialogReady/,
        'clean controls reject battle/transient popup history and require ready bare maps');
    assert.match(sourceRoute,
        /strictSingleRouteClaim = \$true[\s\S]*attemptedActions = 1[\s\S]*recoveryActions = 0/,
        'a clean route pass records one action and no recovery action');
    assert.match(moveCompletion,
        /initialIdle -isnot \[bool\][\s\S]*-not \[bool\]\$initialIdle/,
        'a clean move starts from the already captured strategic-idle publication');
    assert.match(moveCompletion,
        /Get-UiHistory \$role \(\[long\]\$state\.cursor\)[\s\S]*sequence -le \[long\]\$state\.cursor[\s\S]*BareMapDialogs -notcontains \$dialog[\s\S]*dialogReady -isnot \[bool\][\s\S]*dialogAppearance -ne \[long\]\$state\.expectedAppearance[\s\S]*strategicIdle -isnot \[bool\]/,
        'completion consumes typed append-only UI evidence and pins the ready map identity');
    assert.match(moveCompletion,
        /if \(-not \[bool\]\$strategicIdle\)[\s\S]*busySequence = \$sequence[\s\S]*elseif \(\$null -ne \$state\.busySequence[\s\S]*idleSequence = \$sequence[\s\S]*\$state\.done = \$true/,
        'completion requires a published busy edge followed by a later idle edge');
    assert.equal((moveCompletion.match(/Start-Sleep/g) || []).length, 1,
        'completion has one bounded passive polling delay');
    assert.match(moveCompletion, /Start-Sleep -Milliseconds 100/,
        'completion polling uses the exact short 100 ms interval');
    assert.doesNotMatch(moveCompletion,
        /Move-Stack|Invoke-PreparedParallelAttackMoves|Invoke-(?:Button|RestMethod)|Get-World|retry|refire|fallback/i,
        'completion cannot mutate gameplay, inspect world state, or recover an action');
    assert.match(moveCompletion,
        /busySequence = \[long\]\$state\.busySequence[\s\S]*idleSequence = \[long\]\$state\.idleSequence[\s\S]*busyMilliseconds[\s\S]*kind = 'append-only-strategic-idle-transition'[\s\S]*requiredRoles/,
        'completion returns per-role structured transition evidence');
    assert.match(busyOverlap,
        /requiredRoles[\s\S]*host,join[\s\S]*unique accepted CommandStarted edge[\s\S]*startedUiSeq[\s\S]*effectiveBusySequence = \[Math\]::Max[\s\S]*idleSequence -le \$effectiveBusySequence/,
        'busy overlap is clipped to each accepted CommandStarted UI watermark');
    assert.match(busyOverlap,
        /overlapStartSequence[\s\S]*overlapEndSequence[\s\S]*overlapStartSequence -ge \$overlapEndSequence[\s\S]*append-only strategic-busy windows did not overlap[\s\S]*kind = 'relay-observed-post-command-strategic-busy-overlap'/,
        'post-command strategic-busy windows must overlap in the global UI evidence order');
    assert.doesNotMatch(busyOverlap,
        /Move-Stack|Invoke-PreparedParallelAttackMoves|Invoke-(?:Button|RestMethod)|Get-World|Start-Sleep|retry|refire|fallback/i,
        'the overlap assertion is a pure fail-closed evidence calculation');
    assert.match(sourceRoute,
        /Move-Stack \$Role[\s\S]*\$terminalCompletion = if \(\$StrictCleanRoute\)[\s\S]*Wait-CleanLongMoveCompletion[\s\S]*Get-WorldHistory host/,
        'only a strict clean route waits for terminal UI completion before world settling');
    assert.match(sourceRoute,
        /terminalCompletion = \$terminalCompletion[\s\S]*activeRole = \$Role/,
        'clean route results retain their terminal transition evidence');
    assert.match(longMove,
        /if \(\$Case -eq 'source-route-control'\) \{[\s\S]*return Invoke-LongMoveSourceRouteControl/,
        'the explicit source-route case dispatches directly to the one-action control');
    assert.match(longMove,
        /if \(\$Case -eq 'clean-host-route-control'\) \{[\s\S]*-Role host -TargetProperty cleanLongMove -Case \$Case[\s\S]*-StrictCleanRoute/,
        'the host-only case sends the pinned clean target while join stays idle');
    assert.match(longMove,
        /if \(\$Case -eq 'clean-join-route-control'\) \{[\s\S]*-Role join -TargetProperty cleanLongMove -Case \$Case[\s\S]*-StrictCleanRoute/,
        'the join-only case sends the pinned clean target while host stays idle');
    assert.match(longMove,
        /if \(\$Case -eq 'source-pair-repro'\) \{[\s\S]*return Observe-LongMoveSourcePair/,
        'the exact historical pair dispatches to a diagnostic observer');
    assert.match(longMove,
        /if \(\$Case -eq 'source-pair-repro'\) \{[\s\S]*return Observe-LongMoveSourcePair[\s\S]*Wait-CleanLongMoveCompletion[\s\S]*-Roles @\('host', 'join'\)[\s\S]*Assert-CleanLongMoveBusyOverlap[\s\S]*Wait-LongMoveTrajectoryEvidence/,
        'clean concurrency proves observed busy overlap before own-world trajectory settling');
    assert.match(longMove,
        /terminalCompletion = \$terminalCompletion[\s\S]*strategicBusyOverlap = \$strategicBusyOverlap[\s\S]*trajectory = \$trajectory/,
        'clean concurrency returns terminal plus separate supporting busy and sampled trajectory evidence');
    assert.match(longMove,
        /\$spec\.longMove[\s\S]*PSObject\.Properties\['cleanLongMove'\][\s\S]*historical longMove is diagnostic-only/,
        'clean acceptance cannot silently reuse the pathological historical destinations');
    assert.doesNotMatch(sourcePair,
        /Move-Stack|Invoke-PreparedParallelAttackMoves|script:Post|retry|refire|fallback|cancel/i,
        'the source-pair observer is passive and cannot submit another action');
    assert.match(sourcePair, /attemptedActions = 2/,
        'source-pair repro records its two one-shot actions');
    assert.match(sourcePair, /recoveryActions = 0/,
        'source-pair repro records no recovery action');
    assert.match(sourcePair, /acceptanceClaim = \$false/,
        'source-pair repro cannot claim clean concurrency acceptance');
    assert.match(sourcePair,
        /sourceArtifactMatched[\s\S]*host\.x -eq 23[\s\S]*join\.x -eq 15[\s\S]*join\.movement -eq 32/,
        'the repro can identify the exact preserved host-moved/join-charged source outcome');
    assert.equal((longAttack.match(/Invoke-PreparedParallelAttackMoves/g) || []).length, 1,
        'long-attack owns one exact paired attack fire site');
    assert.doesNotMatch(longAttack,
        /Move-Stack|Invoke-CanonicalBattleLifecyclesIndependently|alternate|recorded|refire|fallback/i,
        'long-attack cannot add a continuation leg or source fallback target');
    assert.match(longAttack,
        /continuationMoveActions = 0[\s\S]*chargeWatchParity = \[pscustomobject\]@\{[\s\S]*proved = \$false/,
        'the minimal port states both its zero-continuation contract and missing charge-watch parity');
    assert.match(trajectory,
        /SettleMilliseconds = 1200[\s\S]*CommandStarted[\s\S]*\$cursors[\s\S]*Get-WorldHistory \$side \$cursor[\s\S]*firstPositionChangeSequence[\s\S]*lastPositionChangeSequence[\s\S]*changes -lt 2[\s\S]*host-then-join[\s\S]*join-then-host[\s\S]*sampledRoleLocalOverlapObserved[\s\S]*roleLocalOwnTrajectoryOverlapProved = \$sampledRoleLocalOverlapObserved/,
        'each role owns a complete sampled trajectory while stock presentation order remains diagnostic');
    assert.doesNotMatch(trajectory, /long-move serialized role-local trajectories/,
        'throttled world-publication phase cannot become the concurrency verdict');
    assert.doesNotMatch(trajectory, /Get-WorldHistory host \$cursor/,
        'clean concurrency cannot observe both heroes through the host queue alone');
    assert.match(longMove,
        /strictReplacementOracle =[\s\S]*both exact targets with pinned per-hero MP[\s\S]*no battle\/popup[\s\S]*converged views; stock CMidCommandQueue2 presentation order is diagnostic/,
        'the result schema states the complete replacement oracle');
    assert.match(finalWorlds,
        /Properties\['cleanLongMove'\][\s\S]*hostView\.x -ne \[int\]\$target\.x[\s\S]*did not reach its exact cleanLongMove target[\s\S]*hostView\.movement -ne \[int\]\$target\.movement[\s\S]*cross-billing or[\s\S]*route drift/,
        'clean concurrency requires an exact endpoint and per-hero MP charge');
    assert.match(noBattle,
        /BareMapDialogs -notcontains \$dialog[\s\S]*crossed a non-map dialog\/event/,
        'clean concurrency rejects a transient popup even if native auto-dismiss restores the map');
    assert.match(movementStart,
        /Wait-WorldEvidence[\s\S]*post-dispatch long-attack movement publication/,
        'long-attack passively waits for append-only movement evidence without imposing reporter ordering');
    assert.doesNotMatch(movementStart, /BattleUtc|before its battle/,
        'world reporter cadence cannot be ordered ahead of the faster battle UI reporter');
    assert.match(longAttack,
        /FromUnixTimeMilliseconds[\s\S]*movement appeared before its native command edge[\s\S]*battle appeared before its first actual movement/,
        'long-attack retains native CommandStarted as the one-shot causal lower bound');
    assert.match(longAttack,
        /firstMoveUtc[\s\S]*active attack window is \[first actual movement, battle\][\s\S]*overlapStart[\s\S]*overlapEnd/,
        'long-attack ports the source firstMove-to-battle blocking oracle literally');
    assert.match(terminalWorld,
        /longAttack\.target\.id[\s\S]*heroId/,
        'the terminal world requires both pinned targets gone and both heroes present');
    assert.match(worldConvergence,
        /pre-fire watermark[\s\S]*Wait-WorldEvidence/,
        'post-battle convergence consumes passive append-only world evidence');
    assert.doesNotMatch(worldConvergence,
        /Move-Stack|Invoke-PreparedParallelAttackMoves|Invoke-Button|refire|fallback/i,
        'post-battle convergence cannot submit or repeat a gameplay action');
    assert.match(longAttack,
        /requestedDistance -lt 3[\s\S]*Wait-LongAttackWorldConvergence/,
        'the proven join route is distance three and terminal convergence is mandatory');
    assert.match(longAttack,
        /Start-Sleep -Seconds 3[\s\S]*Wait-LegacyReadyQuietPair[\s\S]*-QuietSec 3 -TimeoutSec 30 -PollMilliseconds 800[\s\S]*Wait-LongAttackWorldConvergence/,
        'long-attack preserves the source post-battle +3 and quiet-3 gate before End Turn');

    assert.match(runner,
        /\$GameplayMode -eq 'long-move'[\s\S]*Invoke-LongMoveProof[\s\S]*-Case \$LongMoveCase/,
        'long-move selects its explicit standalone diagnostic or acceptance case');
    assert.match(runner,
        /\[ValidateSet\([\s\S]*'source-route-control',[\s\S]*'source-pair-repro',[\s\S]*'clean-host-route-control',[\s\S]*'clean-join-route-control',[\s\S]*'clean-long-concurrency'[\s\S]*\)\][\s\S]*\$LongMoveCase = 'clean-long-concurrency'/,
        'the positive clean oracle is default while historical diagnostics remain explicit');
    assert.match(runner,
        /\$GameplayMode -eq 'long-attack'[\s\S]*Invoke-LongAttackProof[\s\S]*Run-IndependentRound/,
        'long-attack keeps the source one-shot End Turn and MP refresh after battle completion');
    assert.match(runner,
        /if \(\$GameplayMode -in @\('canonical', 'battle-block', 'long-attack'\)\)/,
        'only battle-bearing standalone gameplay inherits preboot auto-battle');
    assert.doesNotMatch(runner, /D2TESTDRV_PATHFIND_MOVES/,
        'clean movement reuses the existing native-cost builder instead of adding another route switch');
});

test('battle attack lookup fails closed when its unit has already disappeared', () => {
    const hooks = fs.readFileSync(nativeHooksScript, 'utf8');
    const match = hooks.match(
        /void __stdcall getUnitAttacksHooked\([\s\S]*?\r?\n}\r?\n\r?\nbool __stdcall isUnitUseAdditionalAnimationHooked/);
    assert.ok(match, 'getUnitAttacksHooked must remain independently auditable');

    const body = match[0];
    const lookupAt = body.indexOf('auto unit = fn.findUnitById(objectMap, unitId);');
    const guardAt = body.indexOf('if (simturns::phase() != simturns::Phase::Disabled && (!unit || !unit->unitImpl))', lookupAt);
    const firstDereferenceAt = body.indexOf('getAttack(unit->unitImpl', lookupAt);
    assert.ok(lookupAt >= 0 && guardAt > lookupAt && firstDereferenceAt > guardAt,
        'the missing/finalized-unit guard must precede every unitImpl dereference');
    assert.match(body.slice(guardAt, firstDereferenceAt),
        /if \(simturns::phase\(\) != simturns::Phase::Disabled && \(!unit \|\| !unit->unitImpl\)\)\s*\n\s*return;/,
        'a stale battle UI lookup must leave the caller-owned attack vector empty');
    assert.equal((body.match(/findUnitById\(/g) || []).length, 1,
        'the hook performs exactly one unit lookup');
    assert.doesNotMatch(body, /retry|fallback|getUnitAttacksOriginal|hooks::original/i,
        'the stale-unit path cannot retry or manufacture a fallback attack');
});

test('canonical End Turn retains every green MP, per-unit HP, cascade, TX, and process checkpoint', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const nativeWorld = fs.readFileSync(nativeWorldReporterScript, 'utf8');
    const unitVitals = powerShellFunction(gameplay, 'Get-CanonicalHeroUnitVitals');
    const refresh = powerShellFunction(gameplay, 'Wait-CanonicalLegacyEndTurnRefresh');
    const round = powerShellFunction(runner, 'Run-IndependentRound');
    const intent = powerShellFunction(runner, 'New-LiteralPreparedEndTurnIntent');
    const fire = powerShellFunction(runner, 'Invoke-CanonicalPreparedEndTurnPair');
    const localTurnRelease = powerShellFunction(runner, 'Wait-ExactLocalTurnReleasePair');
    const noConfirm = powerShellFunction(runner, 'Assert-CanonicalNoEndTurnConfirmations');
    const cascades = powerShellFunction(runner, 'Assert-CanonicalRoundCascadeProof');
    const deferred = powerShellFunction(runner, 'Assert-DeferredCanonicalRoundEvidence');
    const exactHandle = powerShellFunction(runner, 'Convert-ExactPlayerHandle');
    const canonicalHandle = powerShellFunction(
        runner, 'ConvertTo-CanonicalPlayerHandle');

    assert.match(nativeWorld,
        /json \+= "\],\\"unitStates\\":\[";[\s\S]*for \(const auto& unit : unitViews\)[\s\S]*kvStr\(json, "id"[\s\S]*kvInt\(json, "hp"/,
        'world telemetry must publish each stable unit id together with its HP');
    assert.match(unitVitals,
        /reinforcement\.leaderId[\s\S]*reinforcement\.unitIds[\s\S]*states\.Count -ne \$expectedIds\.Count/,
        'the HP proof must retain the exact leader plus three reinforced unit identities');
    assert.match(unitVitals,
        /Measure-Object -Property hp -Sum[\s\S]*\$totalHp -ne \[int\]\$stack\.hp/,
        'per-unit HP must reconcile with the same authoritative group snapshot');
    assert.equal((unitVitals.match(/Get-WorldSnapshot \$Role/g) || []).length, 1,
        'one HP helper call is exactly one raw authoritative world read');
    assert.doesNotMatch(unitVitals, /Get-RelayState|Get-World\s|Wait-|Start-Sleep/,
        'the raw HP read cannot hide a state read or convergence wait');

    assert.match(refresh,
        /for \(\$elapsed = 3; \$elapsed -le \$TimeoutSec; \$elapsed \+= 3\)[\s\S]*\$observeAt = \$FireCompletedAt\.AddSeconds\(\$elapsed\)[\s\S]*\$remainingMs[\s\S]*Start-Sleep -Milliseconds[\s\S]*Get-LegacySequentialSharedMovementSnapshot \$Fixture/,
        'the first MP read and every later observation must retain the fire-anchored +3/+6 cadence');
    assert.equal((refresh.match(/Get-LegacySequentialSharedMovementSnapshot/g) || []).length, 1,
        'each scheduled End Turn observation performs the same two-read legacy helper once');
    assert.doesNotMatch(refresh, /Assert-ClientsLive|Get-RelayState|Get-RoleState|Read-SimRelayEvents/,
        'the +3 cadence cannot substitute a consolidated/local read or protocol wait');
    assert.match(refresh, /\[int\]\$TimeoutSec = 45/,
        'the green End Turn watcher has one exact 45-second deadline');
    assert.doesNotMatch(refresh,
        /Read-SimRelayEvents|Invoke-(?:EndTurns|Button|ParallelWorldMoves)|SendAsync|Move-Stack|Enable-Toggle|retry|fallback/i,
        'the MP watcher may observe but must never refire an action');

    assert.match(localTurnRelease,
        /\$deadlineUtc = \$FireCompletedAt\.AddSeconds\(\$TimeoutSec\)/,
        'both native release markers share the original fire-anchored deadline');
    assert.equal((localTurnRelease.match(/Get-LiteralClientLogMarkerEventUtcSnapshot/g) || []).length, 2,
        'the release bridge takes one exact timestamped marker sample per role');
    assert.match(localTurnRelease,
        /\[long\]\$HostActionId[\s\S]*\[long\]\$JoinActionId[\s\S]*\$HostActionId -le 0[\s\S]*\$HostActionId -eq \$JoinActionId[\s\S]*ActivateTurn UI queue drained \(actionId=\{0\}, day=\{1\}\)[\s\S]*\$hostEvent[\s\S]*\$joinEvent[\s\S]*if \(\$hostEvent -and \$joinEvent\)/,
        'the bridge requires each pre-proved v8 ActivateTurn action/day marker from its local client');
    assert.equal((localTurnRelease.match(/Read-SimRelayEvents|Get-EngineActionMatches/g) || []).length, 0,
        'the passive release bridge cannot re-resolve already proved action identities');
    assert.equal((localTurnRelease.match(/Start-Sleep -Milliseconds 250/g) || []).length, 1,
        'the bounded passive observer has one short sample cadence');
    assert.doesNotMatch(localTurnRelease,
        /Invoke-RestMethod|Invoke-(?:Button|Parallel)|Get-World|Get-RoleState|Get-RelayState|Wait-ClientLogMarkerEventUtc|Move-Stack/i,
        'the release bridge cannot issue or reconstruct a gameplay action');

    assert.match(intent,
        /PreparedMetadata\.\$role[\s\S]*dialogAppearance[\s\S]*endTurnInstance[\s\S]*uiAfter[\s\S]*uiHostWatermark[\s\S]*uiJoinWatermark/,
        'both End Turn owners and UI watermarks come only from the current phase preparation');
    assert.match(intent,
        /Get-SessionPlanEvent[\s\S]*beforeHostObserved[\s\S]*beforeJoinObserved[\s\S]*beforeHostApplied[\s\S]*beforeJoinApplied[\s\S]*beforeHostAccepted[\s\S]*beforeJoinAccepted[\s\S]*expectedHostHandle[\s\S]*expectedJoinHandle/,
        'the intent binds v8 End Turn deltas to the negotiated session handles');
    assert.doesNotMatch(intent,
        /Get-World|Get-RoleState|Get-RelayState|Get-Dialog|Read-|Wait-|Invoke-RestMethod/,
        'constructing the paired intent must remain a pure in-memory transform');
    assert.match(fire,
        /\/api\/ui\/end-turn-pair-when-strategic-idle\?[\s\S]*hostappearance=\{1\}&hostinstance=\{2\}&hostui=\{3\}&[\s\S]*joinappearance=\{4\}&joininstance=\{5\}&joinui=\{6\}&[\s\S]*waitMs=120000&timeoutMs=8000/,
        'one relay-owned subscription must span both strategic-idle edges and pair dispatch');
    assert.match(fire,
        /\[long\]\$hostEndTurn\.appearance[\s\S]*\[long\]\$hostEndTurn\.instance[\s\S]*\$hostUi[\s\S]*\[long\]\$joinEndTurn\.appearance[\s\S]*\[long\]\$joinEndTurn\.instance[\s\S]*\$joinUi/,
        'the sole request carries only the saved host/join appearance, owner, and UI watermark');
    assert.equal((fire.match(/Invoke-RestMethod -Method Post/g) || []).length, 1,
        'canonical End Turn owns exactly one paired HTTP mutation');
    assert.doesNotMatch(fire,
        /Invoke-ParallelButtons|ForEach-Object -Parallel|Get-(?:RelayState|RoleState|GameUiSnapshot)|Read-SimRelayEvents|Start-Sleep|\bwhile\s*\(|retry|fallback/i,
        'canonical fire cannot insert a read/fire race, polling, parallel POSTs, or recovery');
    assert.match(fire,
        /\$response\.found -isnot \[bool\][\s\S]*\$response\.host\.found -isnot \[bool\][\s\S]*\$response\.join\.found -isnot \[bool\][\s\S]*expected\.actual\.invoke\.appearance[\s\S]*expected\.actual\.invoke\.instance[\s\S]*expected\.actual\.strategicIdle/,
        'the one response must prove both exact native owners were invoked while idle');
    assert.match(fire, /completedAt = \$completedAt[\s\S]*dispatchSkewMs = \$skewMs/,
        'the one-shot fire returns the clock anchor and finite dispatch skew');
    assert.match(noConfirm, /DLG_MESSAGE_BOX[\s\S]*requires zero[\s\S]*return 0/,
        'a confirmation dialog is a terminal canonical failure, never another action');
    assert.doesNotMatch(noConfirm, /Invoke-Button|BTN_YES|BTN_NO/,
        'canonical End Turn cannot dismiss or confirm anything');
    assert.match(cascades,
        /Get-NewExactEngineAction[\s\S]*'ordinary-apply' 'host'[\s\S]*Get-EngineActionMatches[\s\S]*'ordinary-activate'[\s\S]*Get-NewExactSimEvent \$Events 'turn-start-complete'/,
        'each subjective turn follows the v8 ApplyTurnStart -> ActivateTurn -> completion chain');
    assert.match(cascades,
        /actionId[\s\S]*activationActionId[\s\S]*completionActionId[\s\S]*lease[\s\S]*completion-before-next-dispatch/,
        'both cascades retain one exact v8 action/lease identity and serialized completion order');
    assert.match(cascades,
        /handle = ConvertTo-CanonicalPlayerHandle\s*`[\s\S]*\$expectedHandle/,
        'summary-ready cascade handles must cross one explicit canonical hex boundary');
    assert.doesNotMatch(cascades, /handle = \$expectedHandle/,
        'a decimal session-plan handle cannot leak directly into the saved cascade schema');
    assert.match(deferred,
        /\$cascadeHandle = Convert-ExactPlayerHandle[\s\S]*\[long\]\$_\.handle -eq \$cascadeHandle/,
        'native ApplyTurnStart matching must remain numeric after summary canonicalization');
    runPowerShellContract(`
Set-StrictMode -Version Latest
${exactHandle}
${canonicalHandle}
$summary = [pscustomobject]@{
    cascades = @(
        [pscustomobject]@{
            handle = ConvertTo-CanonicalPlayerHandle 2749235201 'host handle'
        },
        [pscustomobject]@{
            handle = ConvertTo-CanonicalPlayerHandle '2749235202' 'join handle'
        }
    )
}
$json = $summary | ConvertTo-Json -Compress
$expected = '{"cascades":[{"handle":"0xA3DE0001"},{"handle":"0xA3DE0002"}]}'
if ($json -ne $expected -or $json.Contains('2749235201') -or
    $json.Contains('2749235202')) {
    throw "canonical cascade handle schema drifted: $json"
}
Write-Output 'CANONICAL_CASCADE_HANDLE_SCHEMA_PASS'
`, 'CANONICAL_CASCADE_HANDLE_SCHEMA_PASS',
    'canonical cascade handle serialization');

    const ordered = [
        '$ownedProcessesBefore = @(Get-Process',
        '$stackReachability = Get-LegacyStackSnapshot',
        '$peerState = Get-RelayState',
        '$roundActionPreparation = New-CanonicalWalkPreparationFromRelayState',
        '$hostHeroCensus = Get-LegacyStackSnapshot',
        '$joinHeroCensus = Get-LegacyStackSnapshot',
        '$legacyHostLogWatermark = @(Read-ClientLogLines $HostLog).Count',
        '$legacyVitalsBefore = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        '$legacyHostUnitVitalsBefore = Get-CanonicalHeroUnitVitals $Fixture host',
        '$legacyJoinUnitVitalsBefore = Get-CanonicalHeroUnitVitals $Fixture host',
        'New-LiteralPreparedEndTurnIntent',
        'Invoke-CanonicalPreparedEndTurnPair',
        'Wait-CanonicalLegacyEndTurnRefresh',
        '$hostLinesAfter = @(Read-ClientLogLines $HostLog)',
        'Assert-NoStockEndTurnSnapshotAfter 0',
        'Get-Process -Id $ownedProcessIds -ErrorAction SilentlyContinue).Count',
        'Start-Sleep -Seconds 4',
        '$legacyHostUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host',
        '$legacyJoinUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host',
        '$legacyVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        'round $Round PASS',
        '$localTurnRelease = Wait-ExactLocalTurnReleasePair',
        '$legacyContinuation = & $OnCanonicalLegacyPass',
        '$deferredMssEvidence = [pscustomobject]@{'
    ];
    let previous = -1;
    for (const token of ordered) {
        const at = round.indexOf(token, previous + 1);
        assert.ok(at > previous, `green End Turn checkpoint moved or vanished: ${token}`);
        previous = at;
    }
    assert.equal((round.match(/Invoke-CanonicalPreparedEndTurnPair/g) || []).length, 1,
        'the canonical End Turn pair has one common fire site');
    assert.match(round, /if \(\$Fixture\)[\s\S]*Invoke-CanonicalPreparedEndTurnPair\s+`\s*\$canonicalIntent[\s\S]*else \{[\s\S]*Invoke-EndTurnsAndWaitAccepted \$RelayProcess/,
        'the relay-acceptance helper belongs only to the non-canonical branch');
    assert.doesNotMatch(round,
        /if \(\$Fixture[\s\S]{0,160}Wait-ActionablePair/,
        'the source peer-state read supplies the current End Turn capability; no new gate may be inserted');
    assert.match(round,
        /\$peerState = Get-RelayState[\s\S]*\$roundActionPreparation = New-CanonicalWalkPreparationFromRelayState[\s\S]*New-LiteralPreparedEndTurnIntent[\s\S]*\$roundActionPreparation/,
        'End Turn must project and consume the current source peer-state publication, never a cross-phase token');
    const exactFourAt = round.indexOf('Start-Sleep -Seconds 4');
    const hpAfterAt = round.indexOf(
        '$legacyHostUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host', exactFourAt);
    const secondHpAfterAt = round.indexOf(
        '$legacyJoinUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host', hpAfterAt);
    const mpAfterAt = round.indexOf(
        '$legacyVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture', hpAfterAt);
    const legacyPassAt = round.indexOf('round $Round PASS', mpAfterAt);
    assert.ok(exactFourAt >= 0 && hpAfterAt > exactFourAt &&
        secondHpAfterAt > hpAfterAt && mpAfterAt > secondHpAfterAt && legacyPassAt > mpAfterAt,
    'the exact +4 window must be followed by HP first, two sequential MP reads second, then legacy PASS');
    const releaseAt = round.indexOf(
        '$localTurnRelease = Wait-ExactLocalTurnReleasePair', legacyPassAt);
    const continuationAt = round.indexOf('$legacyContinuation = & $OnCanonicalLegacyPass', releaseAt);
    const packageAt = round.indexOf('$deferredMssEvidence = [pscustomobject]@{', continuationAt);
    assert.ok(releaseAt > legacyPassAt && continuationAt > releaseAt,
        'the sole passive native-release bridge must precede the one day-2 action pair');
    const passToContinuation = round.slice(legacyPassAt, continuationAt);
    assert.equal((passToContinuation.match(/Wait-ExactLocalTurnReleasePair/g) || []).length, 1,
        'exactly one native-release bridge is allowed after the old M9 PASS');
    assert.doesNotMatch(passToContinuation.replace('Wait-ExactLocalTurnReleasePair', ''),
        /Get-|Read-|Wait-|Start-Sleep|Assert-(?:Clients|NoClient|Production|NoRelay)/,
        'no observation other than exact local release may delay the old day-2 walk');
    assert.doesNotMatch(round.slice(continuationAt, packageAt),
        /Get-World|Get-RoleState|Get-RelayState|Read-|Wait-|Start-Sleep|Invoke-RestMethod/,
        'M9 must hand off saved evidence without an observation after day-2 walk');
    const canonicalPrelude = round.match(
        /if \(\$Fixture\) \{([\s\S]*?)\}\s*else \{\s*Assert-OperationalActionGate/);
    assert.ok(canonicalPrelude,
        'the canonical and non-canonical M9 preludes must remain independently identifiable');
    assert.doesNotMatch(canonicalPrelude[1], /Read-SimRelayEvents/,
        'production relay events are absent from the canonical M9 pre-fire path');
    assert.doesNotMatch(round,
        /legacy End Turn MP precondition|Fixture\.(?:host|join)\.postBattleWalk\.movement/,
        'sync_endturn printed its incoming MP pair; it never required one historical 12/5 battle outcome');
    const canonicalVerdictAt = round.indexOf('# Literal ASSERTS section:');
    assert.ok(canonicalVerdictAt >= 0 && canonicalVerdictAt < legacyPassAt,
        'the literal canonical M9 verdict boundary must remain identifiable');
    assert.doesNotMatch(round.slice(canonicalVerdictAt, legacyPassAt), /Read-SimRelayEvents/,
        'production relay events are absent from the canonical M9 post-fire verdict');

    assert.match(deferred,
        /Assert-SimEventDelta[\s\S]*Assert-ExactEndTurnTransaction[\s\S]*Assert-CanonicalRoundCascadeProof[\s\S]*Assert-CanonicalNoEndTurnConfirmations/,
        'the MSS causal strengthening remains complete at its deferred post-barrier boundary');
    assert.match(deferred,
        /Get-NewExactSimEvent[\s\S]*'turn-start-complete' 'host' \$Evidence\.completeHostBefore[\s\S]*'turn-start-complete' 'join' \$Evidence\.completeJoinBefore[\s\S]*\[Math\]::Max\(\$hostCompleteIndex, \$joinCompleteIndex\)[\s\S]*\$Events\[0\.\.\$cutoff\]/,
        'the first-round prefix ends inclusively at the later exact v8 turn completion');
    assert.match(deferred,
        /end-turn-observed'; role = 'host'[\s\S]*end-turn-observed'; role = 'join'[\s\S]*end-turn-applied'; role = 'host'[\s\S]*end-turn-applied'; role = 'join'[\s\S]*\$matches\[\$nextIndex\][\s\S]*-le \$cutoff/,
        'all four following barrier signals must remain outside the completed-round prefix regardless of arrival order');
    assert.doesNotMatch(deferred,
        /\[Math\]::Min\(|hostBarrierIndex|joinBarrierIndex/,
        'the cutoff cannot depend on which following observed/applied signal arrives first');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Get-OptionalProperty([object]$Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Get-SimEventMatches([object[]]$Events, [string]$Event,
                             [string]$Role = '', [object]$Day = $null) {
    return @($Events | Where-Object {
        (Get-OptionalProperty $_ 'event') -eq $Event -and
        ([string]::IsNullOrEmpty($Role) -or
            (Get-OptionalProperty $_ 'role') -eq $Role) -and
        ($null -eq $Day -or (Get-OptionalProperty $_ 'day') -eq $Day)
    })
}
function Get-SimEventCount([object[]]$Events, [string]$Event,
                           [string]$Role = '') {
    return @(Get-SimEventMatches $Events $Event $Role).Count
}
function Get-NewExactSimEvent([object[]]$Events, [string]$Event,
                              [string]$Role, [int]$Before) {
    $matches = @(Get-SimEventMatches $Events $Event $Role)
    if ($matches.Count -ne ($Before + 1)) {
        throw "event '$Event/$Role' count=$($matches.Count), expected $($Before + 1)"
    }
    return $matches[$Before]
}
function Get-SimEventRecordIndex([object[]]$Events, [object]$Record) {
    for ($index = 0; $index -lt $Events.Count; $index++) {
        if ([object]::ReferenceEquals($Events[$index], $Record)) { return $index }
    }
    return -1
}
function Get-EngineActionMatches([object[]]$Events, [string]$Stage,
                                 [string]$Recipient, [object]$PlayerHandle) {
    return @($Events | Where-Object {
        (Get-OptionalProperty $_ 'event') -eq 'engine-action-dispatched' -and
        (Get-OptionalProperty $_ 'stage') -eq $Stage -and
        (Get-OptionalProperty $_ 'recipient') -eq $Recipient -and
        [string](Get-OptionalProperty $_ 'playerHandle') -eq [string]$PlayerHandle
    })
}
function Assert-SimEventDelta([object[]]$Events, [string]$Event,
                              [int]$Before, [int]$ExpectedDelta,
                              [string]$Role = '') {
    [int]$actual = Get-SimEventCount $Events $Event $Role
    if ($actual -ne ($Before + $ExpectedDelta)) {
        throw "prefix event '$Event/$Role' count=$actual"
    }
}
function Assert-EngineActionDelta([object[]]$Events, [string]$Stage,
                                  [string]$Recipient, [object]$PlayerHandle,
                                  [int]$Before, [int]$ExpectedDelta) {
    [int]$actual = @(Get-EngineActionMatches $Events $Stage $Recipient $PlayerHandle).Count
    if ($actual -ne ($Before + $ExpectedDelta)) {
        throw "prefix action '$Stage/$Recipient/$PlayerHandle' count=$actual"
    }
}
function Assert-NoRelayFault([object[]]$Events) {
    $script:DeferredCanonicalPrefix = @($Events)
}
function Assert-ExactEndTurnTransaction([object[]]$Events, [string]$Role,
                                        [int]$ObservedBefore,
                                        [int]$AppliedBefore,
                                        [int]$AcceptedBefore,
                                        [int]$ExpectedCompletedDay) {
    Assert-SimEventDelta $Events 'end-turn-observed' $ObservedBefore 1 $Role
    Assert-SimEventDelta $Events 'end-turn-applied' $AppliedBefore 1 $Role
    Assert-SimEventDelta $Events 'end-turn-accepted' $AcceptedBefore 1 $Role
    return [pscustomobject]@{ role = $Role; completedDay = $ExpectedCompletedDay }
}
function Assert-CanonicalRoundCascadeProof([object[]]$Events, [object]$Fire,
                                           [int]$HostApplyBefore,
                                           [int]$JoinApplyBefore,
                                           [int]$HostActivateBefore,
                                           [int]$JoinActivateBefore,
                                           [int]$HostCompleteBefore,
                                           [int]$JoinCompleteBefore,
                                           [int]$ExpectedDay) {
    if ($ExpectedDay -ne 2) { throw "unexpected cascade day $ExpectedDay" }
    return [pscustomobject]@{
        cascades = @(
            [pscustomobject]@{ role = 'host'; handle = $Fire.expectedHostHandle; actionId = 110; activationActionId = 110; lease = 102; day = 2 },
            [pscustomobject]@{ role = 'join'; handle = $Fire.expectedJoinHandle; actionId = 120; activationActionId = 120; lease = 202; day = 2 }
        )
        order = @('host', 'join')
    }
}
function Assert-CanonicalNoEndTurnConfirmations([object]$Fire) { return 0 }
${exactHandle}
${deferred}
function New-DeferredCanonicalFixture([ValidateSet('observed-first', 'applied-first')]
                                      [string]$NextOrder) {
    $events = [System.Collections.Generic.List[object]]::new()
    [void]$events.Add([pscustomobject]@{
        event = 'session-plan-created'; epoch = 9; mergeDay = 3
        hostHandle = 1001; joinHandle = 2001
        hostLease = 101; joinLease = 201
    })
    foreach ($roleInfo in @(
        [pscustomobject]@{ role = 'host'; lease = 101 },
        [pscustomobject]@{ role = 'join'; lease = 201 }
    )) {
        foreach ($eventName in @(
            'end-turn-observed', 'end-turn-applied', 'end-turn-accepted'
        )) {
            $record = [ordered]@{
                event = $eventName; role = $roleInfo.role
                lease = $roleInfo.lease; completedDay = 1
            }
            if ($eventName -eq 'end-turn-accepted') { $record.queued = 1 }
            [void]$events.Add([pscustomobject]$record)
        }
    }
    foreach ($action in @(
        [pscustomobject]@{ event = 'engine-action-dispatched'; stage = 'ordinary-apply'; recipient = 'host'; playerHandle = 1001; kind = 1; actionId = 110; lease = 102; day = 2 },
        [pscustomobject]@{ event = 'engine-action-dispatched'; stage = 'ordinary-activate'; recipient = 'host'; playerHandle = 1001; kind = 2; actionId = 110; lease = 102; day = 2 },
        [pscustomobject]@{ event = 'turn-start-complete'; role = 'host'; actionId = 110; lease = 102; day = 2 },
        [pscustomobject]@{ event = 'engine-action-dispatched'; stage = 'ordinary-apply'; recipient = 'host'; playerHandle = 2001; kind = 1; actionId = 120; lease = 202; day = 2 },
        [pscustomobject]@{ event = 'engine-action-dispatched'; stage = 'ordinary-activate'; recipient = 'join'; playerHandle = 2001; kind = 2; actionId = 120; lease = 202; day = 2 },
        [pscustomobject]@{ event = 'turn-start-complete'; role = 'join'; actionId = 120; lease = 202; day = 2 }
    )) { [void]$events.Add($action) }

    $nextSignals = if ($NextOrder -eq 'observed-first') {
        @(
            [pscustomobject]@{ event = 'end-turn-observed'; role = 'host'; lease = 102; completedDay = 2 },
            [pscustomobject]@{ event = 'end-turn-observed'; role = 'join'; lease = 202; completedDay = 2 },
            [pscustomobject]@{ event = 'end-turn-applied'; role = 'host'; lease = 102; completedDay = 2 },
            [pscustomobject]@{ event = 'end-turn-applied'; role = 'join'; lease = 202; completedDay = 2 }
        )
    } else {
        @(
            [pscustomobject]@{ event = 'end-turn-applied'; role = 'host'; lease = 102; completedDay = 2 },
            [pscustomobject]@{ event = 'end-turn-applied'; role = 'join'; lease = 202; completedDay = 2 },
            [pscustomobject]@{ event = 'end-turn-observed'; role = 'host'; lease = 102; completedDay = 2 },
            [pscustomobject]@{ event = 'end-turn-observed'; role = 'join'; lease = 202; completedDay = 2 }
        )
    }
    foreach ($signal in $nextSignals) { [void]$events.Add($signal) }
    $fire = [pscustomobject]@{
        beforeHostObserved = 0; beforeJoinObserved = 0
        beforeHostApplied = 0; beforeJoinApplied = 0
        beforeHostAccepted = 0; beforeJoinAccepted = 0
        expectedHostHandle = 1001; expectedJoinHandle = 2001
        expectedHostCompletedDay = 1; expectedJoinCompletedDay = 1
    }
    return [pscustomobject]@{
        events = @($events)
        evidence = [pscustomobject]@{
            fire = $fire; eventsBefore = @($events[0])
            applyHostBefore = 0; applyJoinBefore = 0
            activateHostBefore = 0; activateJoinBefore = 0
            completeHostBefore = 0; completeJoinBefore = 0
            completedDay = 1
            nativeApplyActions = @(
                [pscustomobject]@{ handle = 1001; actionId = 110; lease = 102; day = 2 },
                [pscustomobject]@{ handle = 2001; actionId = 120; lease = 202; day = 2 }
            )
            localTurnRelease = [pscustomobject]@{
                hostActionId = 110; joinActionId = 120
            }
        }
    }
}
$savedSummary = $null
foreach ($nextOrder in @('observed-first', 'applied-first')) {
    $fixture = New-DeferredCanonicalFixture $nextOrder
    $acceptedRecords = @($fixture.events | Where-Object {
        $_.event -eq 'end-turn-accepted'
    })
    if ($acceptedRecords.Count -ne 2 -or
        @($acceptedRecords | Where-Object { $_.queued -isnot [int] }).Count -ne 0) {
        throw 'canonical v8 fixture encoded queued as something other than a numeric queue length'
    }
    $result = Assert-DeferredCanonicalRoundEvidence $fixture.events $fixture.evidence
    $prefix = @($script:DeferredCanonicalPrefix)
    $summary = [ordered]@{
        prefixCount = $prefix.Count
        finalEvent = [string](Get-OptionalProperty $prefix[-1] 'event')
        hostObserved = Get-SimEventCount $prefix 'end-turn-observed' 'host'
        joinObserved = Get-SimEventCount $prefix 'end-turn-observed' 'join'
        hostApplied = Get-SimEventCount $prefix 'end-turn-applied' 'host'
        joinApplied = Get-SimEventCount $prefix 'end-turn-applied' 'join'
        hostCompleted = Get-SimEventCount $prefix 'turn-start-complete' 'host'
        joinCompleted = Get-SimEventCount $prefix 'turn-start-complete' 'join'
        hostBarrierDay = [int]$result.hostBarrier.completedDay
        joinBarrierDay = [int]$result.joinBarrier.completedDay
        cascadeCount = @($result.cascades).Count
        confirmationCount = [int]$result.confirmationCount
    }
    if ($summary.prefixCount -ne 13 -or
        $summary.finalEvent -ne 'turn-start-complete' -or
        $summary.hostObserved -ne 1 -or $summary.joinObserved -ne 1 -or
        $summary.hostApplied -ne 1 -or $summary.joinApplied -ne 1 -or
        $summary.hostCompleted -ne 1 -or $summary.joinCompleted -ne 1 -or
        $summary.hostBarrierDay -ne 1 -or $summary.joinBarrierDay -ne 1 -or
        $summary.cascadeCount -ne 2 -or $summary.confirmationCount -ne 0) {
        throw "$nextOrder did not preserve the exact completed first-round summary"
    }
    $json = [pscustomobject]$summary | ConvertTo-Json -Compress
    if ($savedSummary -and $json -ne $savedSummary) {
        throw 'deferred canonical summary changed with next observed/applied order'
    }
    $savedSummary = $json
}
Write-Output 'CANONICAL_DEFERRED_CUTOFF_ORDERS_PASS'
`, 'CANONICAL_DEFERRED_CUTOFF_ORDERS_PASS',
    'canonical deferred completion cutoff under both next-signal orders');
    assert.match(round, /unitHpBefore[\s\S]*unitHpAfter/,
        'the transferred verdict must retain both per-unit HP observations');
    assert.match(round, /movementBefore[\s\S]*endTurnProof[\s\S]*stockEndTurnTotal[\s\S]*trackedProcessesAlive[\s\S]*firstObservationSec/,
        'the result must expose all old End Turn verdicts and their exact timing proof');
});

test('canonical merge preserves the green +3 cadence, +5 snapshot, and sole first-host rotation proof', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const readyQuiet = powerShellFunction(gameplay, 'Wait-LegacyReadyQuietPair');
    const mergeWait = powerShellFunction(runner, 'Wait-CanonicalLegacyMergeRelease');
    const noOverrotation = powerShellFunction(runner, 'Assert-LegacyMergeNoOverrotation');
    const stockPrefix = powerShellFunction(runner, 'Assert-StockTurnEvidencePrefix');
    const completedStockCount = powerShellFunction(
        runner, 'Get-LegacyHostCompletedStockTurnCount');
    const firstHostProbe = powerShellFunction(runner, 'Invoke-CanonicalLegacyPostMergeHostEndTurnProbe');
    const deferredProbe = powerShellFunction(runner, 'Assert-DeferredPostMergeProbeEvidence');
    const optionalProperty = powerShellFunction(runner, 'Get-OptionalProperty');
    const merge = powerShellFunction(runner, 'Run-MergeBarrier');
    const extended = powerShellFunction(runner, 'Run-PostMergeStockProof');
    assert.doesNotMatch(merge,
        /deferred canonical fixture MP mismatch before merge/,
        'barrier_endturn observed its incoming MP pair and never required historical 33/29');
    assert.match(merge,
        /\$hostMergeEndTurn = Assert-ExactEndTurnTransaction[\s\S]*\$events host[\s\S]*\$fire\.beforeHostObserved \$fire\.beforeHostApplied \$fire\.beforeHostAccepted[\s\S]*\(\$MergeDay - 1\)[\s\S]*\$joinMergeEndTurn = Assert-ExactEndTurnTransaction[\s\S]*\$events join[\s\S]*\$fire\.beforeJoinObserved \$fire\.beforeJoinApplied \$fire\.beforeJoinAccepted[\s\S]*\(\$MergeDay - 1\)/,
        'merge binds each held role to its exact completed-day End Turn transaction');
    assert.match(merge,
        /\$mergeProof\.barriers[\s\S]*\$held\.Count -ne 1[\s\S]*\$held\[0\]\.heldIndex -le \[int\]\$roleProof\.endTurn\.acceptedIndex/,
        'each role-local held barrier must strictly follow that role\'s accepted End Turn');
    assert.match(merge,
        /Assert-ClientLogMarkerDelta \$HostLog \$exactNaturalMergeMarker 0[\s\S]*Assert-ClientLogMarkerDelta \$JoinLog \$exactNaturalMergeMarker 0[\s\S]*Assert-ClientLogMarkerDelta \$HostLog \$exactReleaseMarker 0[\s\S]*Assert-ClientLogMarkerDelta \$JoinLog \$exactReleaseMarker 0[\s\S]*Assert-ClientLogMarkerDelta \$HostLog \$exactPrepareMarker 0[\s\S]*Assert-ClientLogMarkerDelta \$JoinLog \$exactPrepareMarker 0[\s\S]*Assert-ClientLogMarkerDelta \$HostLog \$exactHostMergeMarker 0[\s\S]*\$joinHostExecuteCount -ne 0/,
        'canonical merge requires prepare/natural/release once per client and execute only on host');

    assert.match(mergeWait,
        /for \(\$elapsed = 3; \$elapsed -le \$TimeoutSec; \$elapsed \+= 3\)[\s\S]*\$observeAt = \$FireCompletedAt\.AddSeconds\(\$elapsed\)[\s\S]*Wait-FixedUtcAnchor \$observeAt[\s\S]*Get-ClientLogMarkerCount \$HostLog[\s\S]*Get-ClientLogMarkerCount \$JoinLog/,
        'merge release must first be observed at fire+3s and then on the same absolute cadence');
    assert.match(mergeWait, /\[int\]\$TimeoutSec = 45/,
        'the green merge watcher has one exact 45-second deadline');
    assert.doesNotMatch(mergeWait,
        /Assert-ClientsLive|Assert-NoClientFaults|Assert-ProductionRelayHealthy|Read-SimRelayEvents|Get-RelayState|Get-World|Get-RoleState|Get-Dialog|Invoke-(?:EndTurns|Button|ParallelWorldMoves)|SendAsync|Move-Stack|Enable-Toggle/,
        'failed merge samples proceed directly to the next +3 edge without diagnostics or actions');

    assert.match(noOverrotation, /\$events\.Count -ne 3/,
        'the pre-click window requires one natural send and the host/join BeginApplied pair');
    assert.match(noOverrotation,
        /\$allEvents[\s\S]*'merge stock-turn telemetry event'\) -gt \$After/,
        'the deferred merge proof projects the one saved history read from its known startup watermark');
    assert.match(noOverrotation,
        /stock-end-turn-send-returned[\s\S]*leaked into stock TX/,
        'any stock End Turn before the deliberate click is terminal over-rotation evidence');
    for (const token of [
        'stock-begin-turn-send-returned', 'stock-begin-turn-applied',
        'senderDpid', 'receiverDpid', 'frameLength',
        'dispatchResult', 'addressee', 'commandSequence', 'activeHandle',
        '$sequence -le $previousSequence', '$hostReceiver -eq $joinReceiver',
        '$latestSequence -ne $previousSequence'
    ]) {
        assert.ok(noOverrotation.includes(token),
            `merge no-overrotation proof lost native field ${token}`);
    }
    assert.match(deferredProbe,
        /\$Probe\.preMergeTurnWatermark[\s\S]*-History \$Probe\.preClickTurnHistory/,
        'the deferred proof consumes the saved pre-merge cursor without another transport read');
    assert.match(deferredProbe,
        /\$preClickEvidence\.commandSequence/,
        'the first post-merge natural broadcast must advance the proved merge wire high-water');
    assert.match(firstHostProbe, /Get-LegacyHostCompletedStockTurnCount/,
        'the source CmdEndCount adapter must observe a completed stock turn above transport seams');
    assert.doesNotMatch(firstHostProbe, /Get-LegacyHostStockEndTurnCount/,
        'post-merge rotation cannot depend on the optional EndSendReturned seam');
    assert.match(runner,
        /stockTurnWatermark\s*=\s*\r?\n?\s*\[long\]\$startupMssProof\.StartupComplete\.LatestSequence/,
        'canonical gameplay carries the exact startup turn cursor into the merge proof');
    assert.match(extended,
        /\$wireHighWater[\s\S]*-PreviousCommandSequence \$wireHighWater[\s\S]*returnedHost\.commandSequence/,
        'extended stock cycles thread one monotonic wire high-water across every natural broadcast');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Get-OptionalProperty([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Get-RequiredTelemetryNumber([object]$Object, [string]$Name, [string]$Context) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -is [bool]) {
        throw "$Context omitted '$Name'"
    }
    [long]$value = 0
    if (-not [long]::TryParse([string]$property.Value, [ref]$value)) {
        throw "$Context has invalid '$Name'"
    }
    return $value
}
function Get-EvidenceWatermark([object]$History, [string]$Label) {
    return [long]$History.latestSequence
}
${noOverrotation}
$hostHandle = [long][Convert]::ToUInt32('A3DE0001', 16)
$history = [pscustomobject]@{
    latestSequence = 10
    events = @(
        [pscustomobject]@{ seq=1; kind='stock-begin-turn-send-returned'; role='host'; idTo=0; frameLength=56; sendResult=1; addressee=0; commandSequence=1; activeHandle=$hostHandle },
        [pscustomobject]@{ seq=7; kind='stock-begin-turn-send-returned'; role='host'; idTo=9; frameLength=56; sendResult=1; addressee=99; commandSequence=4294967295; activeHandle=$hostHandle },
        [pscustomobject]@{ seq=8; kind='stock-begin-turn-send-returned'; role='host'; idTo=0; frameLength=56; sendResult=1; addressee=0; commandSequence=17; activeHandle=$hostHandle },
        [pscustomobject]@{ seq=9; kind='stock-begin-turn-applied'; role='join'; senderDpid=1; receiverDpid=101; frameLength=56; dispatchResult=2; addressee=0; commandSequence=17; activeHandle=$hostHandle },
        [pscustomobject]@{ seq=10; kind='stock-begin-turn-applied'; role='host'; senderDpid=1; receiverDpid=102; frameLength=56; dispatchResult=2; addressee=0; commandSequence=17; activeHandle=$hostHandle }
    )
}
$proof = Assert-LegacyMergeNoOverrotation 7 $hostHandle -History $history
if ($proof.watermark -ne 10 -or $proof.commandSequence -ne 17) {
    throw 'arbitrary global command sequence was not retained independently of world day'
}
$history.events[4].commandSequence = 18
$failedClosed = $false
try {
    [void](Assert-LegacyMergeNoOverrotation 7 $hostHandle -History $history)
} catch {
    $failedClosed = $_.Exception.Message -match 'disagree on the natural broadcast sequence'
}
if (-not $failedClosed) { throw 'mismatched natural broadcast copies did not fail closed' }
Write-Output 'MERGE_SEQUENCE_CONTRACT_PASS'
`, 'MERGE_SEQUENCE_CONTRACT_PASS', 'merge command-sequence/world-day separation');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Get-OptionalProperty([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
${completedStockCount}
$history = [pscustomobject]@{ events = @(
    [pscustomobject]@{ kind='stock-begin-turn-send-returned'; role='host'; commandSequence=17 },
    [pscustomobject]@{ kind='stock-begin-turn-applied'; role='host'; commandSequence=17 },
    [pscustomobject]@{ kind='stock-begin-turn-applied'; role='join'; commandSequence=17 },
    [pscustomobject]@{ kind='stock-begin-turn-send-returned'; role='host'; commandSequence=19 },
    [pscustomobject]@{ kind='stock-begin-turn-applied'; role='join'; commandSequence=19 },
    [pscustomobject]@{ kind='stock-begin-turn-applied'; role='host'; commandSequence=19 }
) }
if ((Get-LegacyHostCompletedStockTurnCount $history) -ne 2) {
    throw 'valid post-merge rotation without EndSendReturned was not counted'
}
$history.events = @($history.events | Select-Object -First 5)
if ((Get-LegacyHostCompletedStockTurnCount $history) -ne 1) {
    throw 'incomplete host/join application pair was counted as complete'
}
Write-Output 'COMPLETED_STOCK_COUNT_PASS'
`, 'COMPLETED_STOCK_COUNT_PASS', 'transport-independent legacy CmdEndCount adapter');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Get-OptionalProperty([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Get-RequiredTelemetryNumber([object]$Object, [string]$Name, [string]$Context) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -is [bool]) {
        throw "$Context omitted '$Name'"
    }
    return [long]$property.Value
}
${stockPrefix}
$joinHandle = [long][Convert]::ToUInt32('A3DE0002', 16)
$events = @(
    [pscustomobject]@{ seq=11; kind='stock-end-turn-send-returned'; role='host'; frameLength=49; sendResult=1; idTo=101 },
    [pscustomobject]@{ seq=12; kind='stock-begin-turn-send-returned'; role='host'; idTo=0; frameLength=56; sendResult=1; addressee=0; commandSequence=19; activeHandle=$joinHandle },
    [pscustomobject]@{ seq=13; kind='stock-begin-turn-applied'; role='host'; senderDpid=1; receiverDpid=102; frameLength=56; dispatchResult=2; addressee=0; commandSequence=19; activeHandle=$joinHandle },
    [pscustomobject]@{ seq=14; kind='stock-begin-turn-applied'; role='join'; senderDpid=1; receiverDpid=101; frameLength=56; dispatchResult=2; addressee=0; commandSequence=19; activeHandle=$joinHandle }
)
$proof = Assert-StockTurnEvidencePrefix $events 10 host $joinHandle 3 17
if ($proof.commandSequence -ne 19 -or $proof.day -ne 3 -or $proof.watermark -ne 14) {
    throw 'stock rotation did not keep world day and wire high-water independent'
}
$withoutEnd = @($events | Where-Object { $_.kind -ne 'stock-end-turn-send-returned' })
$withoutEndProof = Assert-StockTurnEvidencePrefix $withoutEnd 10 host $joinHandle 3 17
if (-not $withoutEndProof -or $null -ne $withoutEndProof.end -or
    $withoutEndProof.commandSequence -ne 19 -or $withoutEndProof.watermark -ne 14) {
    throw 'complete BeginTurn triplet did not prove rotation without optional EndSendReturned'
}
$failedClosed = $false
try {
    [void](Assert-StockTurnEvidencePrefix $events 10 host $joinHandle 3 19)
} catch {
    $failedClosed = $_.Exception.Message -match 'did not advance its high-water'
}
if (-not $failedClosed) { throw 'reused natural command sequence did not fail closed' }
Write-Output 'STOCK_SEQUENCE_HIGH_WATER_PASS'
`, 'STOCK_SEQUENCE_HIGH_WATER_PASS', 'post-merge wire high-water contract');

    assert.equal((merge.match(/Wait-LegacyReadyQuietPair/g) || []).length, 1,
        'the canonical merge has one and only one bounded pre-fire readiness wait');
    assert.match(merge,
        /Wait-LegacyReadyQuietPair[\s\S]*-QuietSec 3 -TimeoutSec 45 -PollMilliseconds 800/,
        'the merge shares the exact 3s/45s/800ms source wait contract');
    assert.doesNotMatch(merge, /Assert-LegacyQuietLogSnapshot/,
        'the merge cannot regain a one-shot quiet assertion');
    assert.ok(readyQuiet.includes('Start-Sleep -Milliseconds 800') &&
        readyQuiet.includes('while ([DateTime]::UtcNow -lt $deadline)'),
    'attack and merge consume the same bounded passive helper');

    const orderedMerge = [
        '$ownedProcessesBefore = @(Get-Process',
        '$stackReachability = Get-LegacyStackSnapshot',
        '$peerState = Get-RelayState',
        '$hostHeroCensus = Get-LegacyStackSnapshot',
        '$joinHeroCensus = Get-LegacyStackSnapshot',
        '$mergeReady = Wait-LegacyReadyQuietPair',
        '$quietHostState = $mergeReady.hostState',
        '$quietJoinState = $mergeReady.joinState',
        '$mergeActionPreparation = $mergeReady.preparation',
        '$quietHostLog = $mergeReady.hostQuiet',
        '$quietJoinLog = $mergeReady.joinQuiet',
        '$legacyMergeVitalsBefore = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        '$legacyCascadeBaselineLines = @(Read-ClientLogLines $HostLog)',
        '$before = @($PreparedCanonicalEvidence.baselineEvents)',
        'New-LiteralPreparedEndTurnIntent',
        'Invoke-CanonicalPreparedEndTurnPair',
        'Wait-CanonicalLegacyMergeRelease',
        'Start-Sleep -Seconds 5',
        '$legacyMergeVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        '$legacyCascadeAfterLines = @(Read-ClientLogLines $HostLog)',
        '$legacySuppressLines = @(Read-ClientLogLines $HostLog)',
        '$legacyRotationLines = @(Read-ClientLogLines $HostLog)',
        '$legacyHostUiState = Get-GameUiSnapshot host',
        '$legacyHostUi = ConvertTo-SavedDialogObservation host $legacyHostUiState',
        '$legacyPreClickTurnHistory = Get-TurnHistory -After 0',
        '$firstHostIntent = New-CanonicalLegacyHostEndTurnIntent $legacyHostUiState',
        'Invoke-CanonicalLegacyPostMergeHostEndTurnProbe',
        'BARRIER RESULT: PASS',
        'Assert-DeferredPostMergeProbeEvidence',
        '$events = @(Read-SimRelayEvents)',
        'Assert-DeferredCanonicalRoundEvidence'
    ];
    let previous = -1;
    for (const token of orderedMerge) {
        const at = merge.indexOf(token, previous + 1);
        assert.ok(at > previous, `green barrier checkpoint moved or vanished: ${token}`);
        previous = at;
    }
    assert.match(merge,
        /Wait-CanonicalLegacyMergeRelease[\s\S]*\$mergeReleasedMarker \$mergeReleasedMarker[\s\S]*\$hostMergeReleasedBefore \$joinMergeReleasedBefore/,
        'the +3 watcher must use both exact global-release applications as completed-MERGE equivalents');
    assert.equal((merge.match(/Invoke-CanonicalPreparedEndTurnPair/g) || []).length, 1,
        'the canonical merge barrier has exactly one common fire site');
    const mergeWaitAt = merge.indexOf('Wait-CanonicalLegacyMergeRelease');
    const mergeFiveAt = merge.indexOf('Start-Sleep -Seconds 5', mergeWaitAt);
    const mergeSequentialAt = merge.indexOf(
        '$legacyMergeVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        mergeFiveAt);
    assert.ok(mergeWaitAt >= 0 && mergeFiveAt > mergeWaitAt &&
        mergeSequentialAt > mergeFiveAt,
    'the exact +5 window must begin immediately after the legacy +3 merge observer');
    const afterFiveBeforeLegacyRead = merge.slice(mergeFiveAt, mergeSequentialAt);
    assert.doesNotMatch(afterFiveBeforeLegacyRead,
        /^\s*(?:\$[^=\r\n]+\s*=\s*)?(?:Get-|Read-|Wait-|Assert-)/m,
        'the two sequential shared MP reads must be the first state/protocol observation after +5');
    const preFireVitalsAt = merge.indexOf(
        '$legacyMergeVitalsBefore = Get-LegacySequentialSharedMovementSnapshot $Fixture');
    const mergeFireAt = merge.indexOf('Invoke-CanonicalPreparedEndTurnPair');
    assert.ok(preFireVitalsAt >= 0 && mergeFireAt > preFireVitalsAt,
        'the merge fire must retain its literal two-read pre-fire MP checkpoint');
    assert.doesNotMatch(merge, /merge barrier PASS/i,
        'merge completion alone is not the legacy barrier verdict');

    const passAt = merge.indexOf('BARRIER RESULT: PASS');
    const deferredAt = merge.indexOf('Assert-DeferredPostMergeProbeEvidence', passAt);
    const eventReadAt = merge.indexOf('$events = @(Read-SimRelayEvents)', deferredAt);
    assert.ok(passAt > 0 && deferredAt > passAt && eventReadAt > deferredAt,
        'health/native/production extensions must remain strictly after the old barrier PASS');
    assert.doesNotMatch(merge.slice(mergeSequentialAt, passAt),
        /Assert-ProductionRelayHealthy|Assert-ClientsLive|Assert-NoClientFaults|Read-SimRelayEvents|Get-WorldDay/,
        'M11/M12 may not gain an MSS health, event, or world read before PASS');
    assert.equal((merge.match(/\$legacy(?:CascadeAfter|Suppress|Rotation)Lines = @\(Read-ClientLogLines \$HostLog\)/g) || []).length, 3,
        'cascade, suppress, and rotation remain three separate source log reads');

    assert.match(firstHostProbe,
        /Invoke-ButtonWhenReady[\s\S]*ConvertTo-SavedDialogObservation[\s\S]*\$firedAt = \[DateTime\]::UtcNow[\s\S]*for \(\$elapsed = 3; \$elapsed -le \$TimeoutSec; \$elapsed \+= 3\)[\s\S]*Wait-FixedUtcAnchor \(\$firedAt\.AddSeconds\(\$elapsed\)\)[\s\S]*Get-TurnHistory -After 0[\s\S]*Get-Process[\s\S]*-Id \$ownedProcessIds/,
        'M12 is one action, then the old read-only +3..+15 stock-count cadence and one exact PID census');
    assert.match(firstHostProbe, /\[int\]\$TimeoutSec = 15[\s\S]*\$TimeoutSec -ne 15/,
        'the canonical probe pins the old 15-second polling contract');
    assert.equal((firstHostProbe.match(/Invoke-ButtonWhenReady/g) || []).length, 1,
        'the first real post-merge host End Turn has exactly one action site');
    assert.doesNotMatch(firstHostProbe, /Invoke-Button(?!WhenReady)/,
        'the symbolic post-merge intent may not regain a second direct action path');
    assert.doesNotMatch(firstHostProbe,
        /while \(|Start-Sleep|Invoke-StockEndTurnAndObserve|Invoke-EndTurnsAndWaitAccepted|Assert-ClientsLive|Assert-NoClientFaults|Assert-ProductionRelayHealthy|Read-ClientLogLines|Read-SimRelayEvents|Get-WorldDay|Get-RelayState|Get-RoleState|Get-Dialog/,
        'M12 has no second action or diagnostic fallback before its legacy verdict; only source polling remains');
    assert.match(deferredProbe,
        /Assert-ClientsLive[\s\S]*Assert-NoClientFaults[\s\S]*Assert-ProductionRelayHealthy[\s\S]*Read-ClientLogLines[\s\S]*Get-WorldDay host[\s\S]*Get-WorldDay join/,
        'all strengthened post-merge checks remain available after PASS');
    assert.match(deferredProbe,
        /\$newConfirmationLines = @\(\)[\s\S]*if \(\$confirmationLines\.Count -ne \$before\)/,
        'the expected no-confirmation path must remain an explicit empty array under StrictMode');
    runPowerShellContract(`
Set-StrictMode -Version Latest
$script:BareMapDialogs = @('DLG_STRATEGIC', 'DLG_ISO_PAL')
function Assert-ClientsLive {
    param([System.Diagnostics.Process]$HostProcess,
          [System.Diagnostics.Process]$JoinProcess)
}
function Assert-NoClientFaults { param([string]$HostLog, [string]$JoinLog) }
function Assert-ProductionRelayHealthy {}
function Assert-LegacyMergeNoOverrotation {
    param([long]$After, [long]$ExpectedHostHandle, [object]$History)
    [pscustomobject]@{ watermark = [long]3; commandSequence = [long]10 }
}
function Assert-StockTurnEvidencePrefix {
    param([object[]]$Events, [long]$After, [string]$ExpectedEndRole,
          [long]$ExpectedActiveHandle, [int]$ExpectedDay,
          [long]$PreviousCommandSequence)
    [pscustomobject]@{ watermark = [long]6 }
}
function Read-ClientLogLines { param([string]$Path); return @() }
function Get-WorldDay { param([string]$Role); return 3 }
${optionalProperty}
${deferredProbe}
$history = [pscustomobject]@{
    events = @(
        [pscustomobject]@{ seq = [long]4 },
        [pscustomobject]@{ seq = [long]5 },
        [pscustomobject]@{ seq = [long]6 }
    )
}
$probe = [pscustomobject]@{
    preMergeTurnWatermark = [long]0
    preClickTurnHistory = [pscustomobject]@{ latestSeq = [long]3; events = @() }
    rotationSample = [pscustomobject]@{
        history = $history
        completedStockTurnCount = [int]2
    }
    baselineCompletedStockTurnCount = [int]1
    preClickHostUi = [pscustomobject]@{ Ready = $true; Dialog = 'DLG_STRATEGIC' }
    suppressCount = [int]0
    confirmationLinePattern = 'never-matches'
    confirmationLinesBefore = [int]0
    evidence = $null
    preClickEvidence = $null
    turnWatermark = $null
    currentOwner = $null
    confirmationAppearance = [long]0
    confirmationSent = $false
}
$process = Get-Process -Id $PID
$result = Assert-DeferredPostMergeProbeEvidence $probe 3 0xA3DE0001 0xA3DE0002 $process $process host.log join.log
if ($null -eq $result -or [bool]$result.confirmationSent -or
    [long]$result.confirmationAppearance -ne 0 -or
    [long]$result.turnWatermark -ne 6) {
    throw 'zero-confirmation deferred proof was not preserved'
}
'ZERO_CONFIRMATION_DEFERRED_PASS'
`, 'ZERO_CONFIRMATION_DEFERRED_PASS',
    'deferred post-merge proof with the expected zero confirmation lines');

    assert.match(extended,
        /if \(\$cycle -eq 1\)[\s\S]*\$hostStep = \$MergeResult\.firstHostAction[\s\S]*else \{[\s\S]*Invoke-StockEndTurnAndObserve/,
        'the two-cycle MSS extension must continue from, never repeat, the legacy first host action');
    assert.match(extended,
        /not the old masstest Phase C[\s\S]*\$PostMergeContinuationMode -ne 'mss-stock-telemetry'/,
        'the stronger stock continuation must identify itself only as MSS telemetry');
    assert.match(runner,
        /if \(\$PostMergeContinuationMode -eq 'mss-stock-telemetry'\) \{[\s\S]*Run-PostMergeStockProof/,
        'the MSS-only continuation must remain an explicit, separately named mode');
    assert.match(runner,
        /\$GameplayMode -in @\('battle-block', 'canonical', 'long-move', 'long-attack'\)[\s\S]*\$PostMergeContinuationMode -ne 'none'[\s\S]*does not accept a post-merge continuation/,
        'canonical, battle-block, and standalone long runs cannot silently acquire the ordered Phase-C continuation');
    assert.match(runner,
        /postMergeContinuationMode = \[string\]\$PostMergeContinuationMode[\s\S]*postMergeAutomaticMasstestPhaseC = \$postMergeAutomaticMasstestPhaseCResult/);
    assert.match(merge,
        /Get-GameUiSnapshot host[\s\S]*\$legacyHostDialog[\s\S]*DLG_ISO_PAL[\s\S]*ConvertTo-SavedDialogObservation host/,
        'the old DLG_ISO_PAL overrotation observation must consume one raw host UI snapshot before MSS projection');
    assert.match(fs.readFileSync(nativeWorldReporterScript, 'utf8'),
        /kvStr\(json, "activePlayerId", wireId\(activePlayerId\)\.c_str\(\)\)/,
        'native world telemetry must publish the current strategic owner');
});

test('standalone ordered masstest preserves four literal End Turns, one-read checkpoints, then Phase C', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const orderedAction = powerShellFunction(
        runner, 'Invoke-LiteralMasstestEndTurnOnce');
    const orderedCheckpoint = powerShellFunction(
        runner, 'Read-LiteralMasstestPeerStateCheckpoint');
    const roleHistory = powerShellFunction(
        runner, 'Assert-LiteralMasstestRoleEndTurnHistory');
    const orderedStage = powerShellFunction(
        runner, 'Assert-LiteralOrderedMasstestStage');
    const orderedDeferredStage = powerShellFunction(
        runner, 'Assert-DeferredLiteralOrderedMasstestStage');
    const requiredTelemetryNumber = powerShellFunction(
        runner, 'Get-RequiredTelemetryNumber');
    const sessionPlan = powerShellFunction(
        runner, 'Get-SessionPlanEvent');
    const sessionRoleHandle = powerShellFunction(
        runner, 'Get-SessionRoleHandle');
    const expectedTurnLease = powerShellFunction(
        runner, 'Get-ExpectedTurnLease');
    const engineActionMatches = powerShellFunction(
        runner, 'Get-EngineActionMatches');
    const barrierHeldEvidence = powerShellFunction(
        runner, 'Assert-BarrierHeldEvidence');
    const exactMergeTransaction = powerShellFunction(
        runner, 'Assert-ExactMergeTransaction');
    const orderedDrive = powerShellFunction(
        runner, 'Run-LiteralOrderedMasstestToMerge');
    const orderedDeferred = powerShellFunction(
        runner, 'Complete-LiteralOrderedMasstestDeferredProof');
    const slotReader = powerShellFunction(
        runner, 'Read-AutomaticMasstestHostBeginTurnSlots');
    const deferredSlots = powerShellFunction(
        runner, 'Assert-DeferredAutomaticMasstestHostBeginTurnSlotSamples');
    const neutralCount = powerShellFunction(
        runner, 'Get-AutomaticMasstestNeutralCount');
    const latestSlot = powerShellFunction(
        runner, 'Get-AutomaticMasstestLatestHostSlot');
    const phaseAction = powerShellFunction(
        runner, 'Invoke-AutomaticMasstestHumanEndTurnOnce');
    const phase = powerShellFunction(
        runner, 'Run-AutomaticMasstestPhaseCLiteral');

    assert.doesNotMatch(runner, /ExtendedPostMergeStockProof/,
        'the ambiguous old boolean flag must be absent');
    assert.match(runner,
        /\[ValidateSet\(\s*'protocol',\s*'canonical',\s*'battle-block',\s*'long-move',\s*'long-attack',\s*'ordered-masstest'\s*\)\]/,
        'ordered masstest must remain a first-class gameplay topology');
    assert.match(runner,
        /\[ValidateSet\(\s*'none',\s*'automatic-masstest-phase-c-literal',\s*'mss-stock-telemetry'\s*\)\]\s*\[string\]\$PostMergeContinuationMode = 'none'/,
        'the post-merge behavior retains one explicit default-none selector');
    assert.match(runner,
        /\$PostMergeContinuationMode -eq 'automatic-masstest-phase-c-literal'[\s\S]*\$GameplayMode -ne 'ordered-masstest'[\s\S]*cannot be appended to canonical gameplay/,
        'literal Phase C belongs only to the standalone ordered topology');
    assert.match(runner,
        /\$GameplayMode -eq 'ordered-masstest'[\s\S]*\$PostMergeContinuationMode -ne 'automatic-masstest-phase-c-literal'[\s\S]*\$MergeDay -notin @\(0, 3\)[\s\S]*\$BarrierOrder -notin @\('host-first', 'join-first'\)/,
        'ordered runs require merge day three and an explicit alternating host/join-first order');

    assert.equal((orderedDrive.match(/Invoke-LiteralMasstestEndTurnOnce/g) || []).length, 4,
        'Drive-ToBarrier plus Drive-ToMerge contain exactly four distinct-day action sites');
    assert.equal((orderedDrive.match(/AddSeconds\(4\)/g) || []).length, 4,
        'each of the four old End Turn actions retains its own fixed +4 edge');
    assert.equal((orderedDrive.match(/Read-LiteralMasstestPeerStateCheckpoint/g) || []).length, 2,
        'only the two day-1 source hook checkpoints use the state transport adapter');
    assert.equal((orderedDrive.match(/\$events = @\(Read-SimRelayEvents\)/g) || []).length, 2,
        'day-2 barrier and fixed merge edge each own one append-only event read');
    assert.equal((orderedDrive.match(/Wait-SimCondition/g) || []).length, 1,
        'the fixed merge edge has one bounded observation-only completion witness');
    assert.doesNotMatch(orderedDrive,
        /Wait-LiteralDayReady|Resolve-LiteralHostAuthoritativeHeroes|Invoke-CanonicalDeploy|Invoke-CanonicalConcurrentAttacks|Invoke-CanonicalParallelWalk|Run-IndependentRound|Run-MergeBarrier/,
        'ordered masstest has no run_test deploy/attack/walk or canonical merge prefix');

    const driveOrder = [
        '$firstRole 1 $firstProcess "$firstRole barrier target"',
        'AddSeconds(4)',
        '$firstDayAdvance = Read-LiteralMasstestPeerStateCheckpoint',
        "stage = 'first-day-1'",
        '$firstRole 2 $firstProcess "$firstRole barrier target"',
        'AddSeconds(4)',
        '$events = @(Read-SimRelayEvents)',
        '$events first-barrier $firstRole $laggardRole',
        '$firstBarrierStage = $stage',
        '-not [bool]$stage.barrierObserved',
        "stage = 'first-barrier'",
        "$laggardRole 1 $HostProcess 'authoritative host during merge drive'",
        'AddSeconds(4)',
        '$laggardDayAdvance = Read-LiteralMasstestPeerStateCheckpoint',
        "stage = 'laggard-day-1'",
        '$HostProcess \'authoritative host after the first laggard observation\'',
        "$laggardRole 2 $HostProcess 'authoritative host during merge drive'",
        'AddSeconds(4)',
        '$events = @(Read-SimRelayEvents)',
        '$fixedMergeStage = Assert-LiteralOrderedMasstestStage',
        '$events laggard-merge $firstRole $laggardRole `',
        '-AllowPendingHostMerge',
        '$completionEvents = Wait-SimCondition',
        '-TimeoutSec 8',
        '$stage = Assert-LiteralOrderedMasstestStage',
        '$completionEvents laggard-merge $firstRole $laggardRole',
        '-not [bool]$stage.mergeObserved',
        "stage = 'laggard-merge'",
        'slotWatermarkBeforePhaseC = [long]0',
        'actionTrace = @($steps)',
        'attemptedActions = 4',
        'acceptedActions = 4',
        'recoveryActions = 0'
    ];
    let previous = -1;
    for (const token of driveOrder) {
        const at = orderedDrive.indexOf(token, previous + 1);
        assert.ok(at > previous, `literal ordered drive moved or lost: ${token}`);
        previous = at;
    }
    assert.match(orderedDrive,
        /\$completionEvents = Wait-SimCondition[\s\S]*-TimeoutSec 8[\s\S]*Get-SimEventMatches \$current 'merge-applied' 'host'[\s\S]*\$hostMergeCount -gt 1[\s\S]*\$hostMergeCount -eq 1[\s\S]*\$stage = Assert-LiteralOrderedMasstestStage/,
        'post-snapshot completion is bounded and admits exactly one host merge marker');
    const fixedMergeAt = orderedDrive.indexOf(
        '$fixedMergeStage = Assert-LiteralOrderedMasstestStage');
    const handoffAt = orderedDrive.indexOf('[long]$hostHandle', fixedMergeAt);
    assert.ok(fixedMergeAt >= 0 && handoffAt > fixedMergeAt,
        'fixed merge classification must precede the Phase-C handoff');
    assert.doesNotMatch(orderedDrive.slice(fixedMergeAt, handoffAt),
        /Invoke-(?:Button|LiteralMasstestEndTurnOnce|AutomaticMasstestHumanEndTurnOnce)|Move-Stack|Enable-Toggle/,
        'post-snapshot completion can observe only and cannot submit another game action');

    assert.equal((orderedAction.match(/Invoke-ButtonWhenReady/g) || []).length, 1,
        'each prepared distinct-day intent has one exact-ready POST site');
    assert.doesNotMatch(orderedAction, /Invoke-Button(?!WhenReady)/,
        'a fixed +4 action cannot bypass native-idle admission with a direct POST');
    assert.match(orderedAction,
        /\$PreparedAction[\s\S]*DLG_STRATEGIC[\s\S]*BTN_END_TURN[\s\S]*afterUiSequence[\s\S]*Invoke-ButtonWhenReady[\s\S]*-WaitMilliseconds 26000[\s\S]*-CommandTimeoutMilliseconds 8000[\s\S]*attempts = 1[\s\S]*accepted = 1[\s\S]*recoveryActions = 0/,
        'the action keeps its fixed +4 source edge, then owns twenty-six seconds of readiness headroom, one eight-second command, and no recovery path');
    assert.doesNotMatch(orderedAction,
        /New-EndTurnAction|Get-|Read-|Wait-|Start-Sleep|while \(|for \(|catch\s*\{/,
        'a distinct-day action performs no hidden observation, wait, retry, or owner repair');
    assert.equal((orderedCheckpoint.match(/Get-RelayState/g) || []).length, 1,
        'each source-equivalent ordered checkpoint is one physical state read');
    assert.equal((orderedCheckpoint.match(/New-EndTurnActionFromObservation/g) || []).length, 0,
        'a transient +4 publication cannot be projected into a prematurely fixed owner');
    assert.doesNotMatch(orderedCheckpoint, /ConvertTo-SavedDialogObservation/,
        'the source-equivalent checkpoint does not require current button readiness');
    assert.match(orderedCheckpoint,
        /'uiSeq'[\s\S]*role = \$role[\s\S]*dialog = 'DLG_STRATEGIC'[\s\S]*button = 'BTN_END_TURN'[\s\S]*afterUiSequence = \[long\]\(\$uiSequence - 1\)/,
        'the same one-read publication supplies the symbolic action watermark');
    assert.doesNotMatch(orderedCheckpoint, /Start-Sleep|Wait-|Invoke-Button/,
        'the one-read checkpoint contains no action or extra timing edge');

    assert.match(roleHistory,
        /end-turn-observed[\s\S]*end-turn-applied[\s\S]*end-turn-accepted[\s\S]*\$observed\.Count -ne \$ExpectedCount[\s\S]*Get-ExpectedTurnLease[\s\S]*lease[\s\S]*completedDay[\s\S]*Observed\/Applied -> Accepted order/,
        'each role keeps its exact observed/applied/accepted lease+day causal triple');
    assert.doesNotMatch(roleHistory, /end-turn-issued|\bgeneration\b|\bhandle\b/,
        'v8 End Turn history cannot reconstruct removed client calendar or generation state');
    assert.match(orderedStage,
        /Get-SimEventMatches \$Events 'barrier-held'[\s\S]*Get-SimEventMatches \$Events 'merge-applied' 'host'[\s\S]*expectedBarrierCount[\s\S]*firstDayTwoAccepted[\s\S]*Get-ExpectedTurnLease[\s\S]*Assert-BarrierHeldEvidence[\s\S]*\$barrierIndex -le \$firstAcceptedIndex/,
        'the first watched-hook equivalent requires one exact held first-role/day-2 barrier predecessor');
    assert.match(orderedStage,
        /\$Stage -eq 'first-barrier'[\s\S]*\$AllowPendingHostMerge[\s\S]*zero or one[\s\S]*laggardDayTwoAccepted[\s\S]*\$laggardAcceptedIndex -le \$barrierIndex[\s\S]*\$laggardBarrierIndex -le \$laggardAcceptedIndex[\s\S]*merge-prepare-dispatched[\s\S]*mergeDay[\s\S]*mergeActionId[\s\S]*both accepted barrier-held records/,
        'the final fixed snapshot allows only a missing host marker after both exact held-barrier predecessors');
    assert.match(orderedStage,
        /sourceMergeEvent = 'merge-applied\/host'[\s\S]*mergeClassification[\s\S]*complete-at-snapshot[\s\S]*pending-at-snapshot[\s\S]*barrierObserved = \$true[\s\S]*mergeObserved = \[bool\]\(\$mergeCount -eq 1\)[\s\S]*savedEvents/,
        'the causal gate returns actual fixed-window classification and v8 evidence to its call sites');
    assert.doesNotMatch(orderedStage,
        /Assert-(?:NoRelayFault|ExactBootstrapOperational|SimEventDelta|LiteralMasstestRoleEndTurnHistory|ExactMergeTransaction)|end-turn-issued|barrier-wait|merge-(?:join|host|commit|operational)-dispatched|merge-now|merge-committed|merge-operational/,
        'the pre-action gate stays limited to v8 source-marker identity/causality; full MSS proof remains deferred');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Get-OptionalProperty([object]$Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Get-SimEventMatches([object[]]$Events, [string]$Event,
                             [string]$Role = '', [object]$Day = $null) {
    return @($Events | Where-Object {
        (Get-OptionalProperty $_ 'event') -eq $Event -and
        ([string]::IsNullOrEmpty($Role) -or
            (Get-OptionalProperty $_ 'role') -eq $Role) -and
        ($null -eq $Day -or (Get-OptionalProperty $_ 'day') -eq $Day)
    })
}
function Get-SimEventCount([object[]]$Events, [string]$Event,
                           [string]$Role = '') {
    return @(Get-SimEventMatches $Events $Event $Role).Count
}
function Get-ExactSimEvent([object[]]$Events, [string]$Event,
                           [string]$Role = '', [object]$Day = $null) {
    $matches = @(Get-SimEventMatches $Events $Event $Role $Day)
    if ($matches.Count -ne 1) {
        throw "event '$Event/$Role' count=$($matches.Count)"
    }
    return $matches[0]
}
function Get-SimEventRecordIndex([object[]]$Events, [object]$Record) {
    for ($index = 0; $index -lt $Events.Count; $index++) {
        if ([object]::ReferenceEquals($Events[$index], $Record)) { return $index }
    }
    return -1
}
${requiredTelemetryNumber}
${sessionPlan}
${sessionRoleHandle}
${expectedTurnLease}
${engineActionMatches}
${barrierHeldEvidence}
${orderedStage}
${exactMergeTransaction}
function New-SessionPlan {
    return [pscustomobject]@{
        event = 'session-plan-created'; epoch = 7; mergeDay = 3
        hostHandle = 1001; joinHandle = 2001
        hostLease = 101; joinLease = 201
    }
}
function New-EndTurnTriple([string]$Role, [long]$Lease, [int]$Day) {
    return @(
        [pscustomobject]@{ event = 'end-turn-observed'; role = $Role; lease = $Lease; completedDay = $Day },
        [pscustomobject]@{ event = 'end-turn-applied'; role = $Role; lease = $Lease; completedDay = $Day },
        [pscustomobject]@{ event = 'end-turn-accepted'; role = $Role; lease = $Lease; completedDay = $Day; queued = 1 }
    )
}
function New-OrdinaryTurnChain([string]$Role, [long]$Handle,
                              [long]$Lease, [long]$ActionId) {
    return @(
        [pscustomobject]@{ event = 'engine-action-dispatched'; stage = 'ordinary-apply'; recipient = 'host'; playerHandle = $Handle; day = 2; lease = $Lease; kind = 1; actionId = $ActionId },
        [pscustomobject]@{ event = 'engine-action-dispatched'; stage = 'ordinary-activate'; recipient = $Role; playerHandle = $Handle; day = 2; lease = $Lease; kind = 2; actionId = $ActionId },
        [pscustomobject]@{ event = 'turn-start-complete'; role = $Role; day = 2; lease = $Lease; actionId = $ActionId }
    )
}
function New-HeldBarrier([string]$Role, [long]$Handle, [long]$ActionId) {
    return @(
        [pscustomobject]@{ event = 'engine-action-dispatched'; stage = 'hold-input'; recipient = $Role; playerHandle = $Handle; day = 2; lease = 0; kind = 3; actionId = $ActionId },
        [pscustomobject]@{ event = 'barrier-held'; role = $Role; day = 2; actionId = $ActionId }
    )
}
function Add-RoleThroughBarrier([System.Collections.ArrayList]$Events,
                                [string]$Role) {
    $handle = if ($Role -eq 'host') { 1001 } else { 2001 }
    $initialLease = if ($Role -eq 'host') { 101 } else { 201 }
    $dayTwoLease = if ($Role -eq 'host') { 102 } else { 202 }
    $turnActionId = if ($Role -eq 'host') { 410 } else { 420 }
    $holdActionId = if ($Role -eq 'host') { 510 } else { 520 }
    foreach ($record in @(New-EndTurnTriple $Role $initialLease 1)) {
        [void]$Events.Add($record)
    }
    foreach ($record in @(New-OrdinaryTurnChain $Role $handle $dayTwoLease $turnActionId)) {
        [void]$Events.Add($record)
    }
    foreach ($record in @(New-EndTurnTriple $Role $dayTwoLease 2)) {
        [void]$Events.Add($record)
    }
    foreach ($record in @(New-HeldBarrier $Role $handle $holdActionId)) {
        [void]$Events.Add($record)
    }
}
function New-OrderedPrefix([string]$FirstRole, [bool]$IncludeLaggard) {
    $events = [System.Collections.ArrayList]::new()
    [void]$events.Add((New-SessionPlan))
    Add-RoleThroughBarrier $events $FirstRole
    if ($IncludeLaggard) {
        $laggard = if ($FirstRole -eq 'host') { 'join' } else { 'host' }
        Add-RoleThroughBarrier $events $laggard
    }
    return ,$events
}
function Add-MergePrefix([System.Collections.ArrayList]$Events) {
    foreach ($record in @(
        [pscustomobject]@{ event = 'merge-prepare-dispatched'; actionId = 900; mergeDay = 3 },
        [pscustomobject]@{ event = 'merge-prepare-applied'; actionId = 900; role = 'host' },
        [pscustomobject]@{ event = 'merge-prepare-applied'; actionId = 900; role = 'join' },
        [pscustomobject]@{ event = 'merge-execute-dispatched'; actionId = 900; mergeDay = 3 }
    )) { [void]$Events.Add($record) }
}
function New-ExecuteEvidence([string]$Name) {
    switch ($Name) {
        'execute' { return [pscustomobject]@{ event = 'merge-execute-applied'; actionId = 900 } }
        'host' { return [pscustomobject]@{ event = 'merge-applied'; actionId = 900; role = 'host' } }
        'join' { return [pscustomobject]@{ event = 'merge-applied'; actionId = 900; role = 'join' } }
    }
    throw "unknown execute evidence '$Name'"
}
$orders = @(
    [pscustomobject]@{ first = 'host'; laggard = 'join' },
    [pscustomobject]@{ first = 'join'; laggard = 'host' }
)
$permutations = @(
    [pscustomobject]@{ names = @('execute', 'host', 'join') },
    [pscustomobject]@{ names = @('execute', 'join', 'host') },
    [pscustomobject]@{ names = @('host', 'execute', 'join') },
    [pscustomobject]@{ names = @('host', 'join', 'execute') },
    [pscustomobject]@{ names = @('join', 'execute', 'host') },
    [pscustomobject]@{ names = @('join', 'host', 'execute') }
)
$endTurnSchema = @(New-EndTurnTriple host 101 1)
$acceptedSchema = @($endTurnSchema | Where-Object {
    $_.event -eq 'end-turn-accepted'
})
if ($acceptedSchema.Count -ne 1 -or $acceptedSchema[0].queued -isnot [int]) {
    throw 'ordered v8 fixture encoded queued as something other than a numeric queue length'
}
$executeSchema = New-ExecuteEvidence execute
if (@($executeSchema.PSObject.Properties.Name) -contains 'role' -or
    @($executeSchema.PSObject.Properties.Name) -notcontains 'actionId') {
    throw 'merge-execute-applied fixture is not the exact actionId-only v8 record'
}
foreach ($order in $orders) {
    $firstEvents = New-OrderedPrefix $order.first $false
    $first = Assert-LiteralOrderedMasstestStage ($firstEvents.ToArray()) first-barrier $order.first $order.laggard
    if (-not $first.barrierObserved -or $first.mergeObserved -or
        $first.barrierIndex -le $first.firstAcceptedIndex) {
        throw "valid $($order.first)-first barrier did not pass its causal gate"
    }
    $missingBarrierEvents = [System.Collections.ArrayList]::new()
    foreach ($record in @($firstEvents | Where-Object {
        $_.event -notin @('engine-action-dispatched', 'barrier-held') -or
        $_.stage -ne 'hold-input'
    } | Where-Object { $_.event -ne 'barrier-held' })) {
        [void]$missingBarrierEvents.Add($record)
    }
    $missingBarrierFailed = $false
    try {
        [void](Assert-LiteralOrderedMasstestStage ($missingBarrierEvents.ToArray()) first-barrier $order.first $order.laggard)
    } catch {
        $missingBarrierFailed = $_.Exception.Message -match 'barrier-held records'
    }
    if (-not $missingBarrierFailed) { throw 'missing held barrier did not fail closed' }

    $missingLaggard = New-OrderedPrefix $order.first $false
    $pendingWithoutPredecessorFailed = $false
    try {
        [void](Assert-LiteralOrderedMasstestStage ($missingLaggard.ToArray()) laggard-merge $order.first $order.laggard -AllowPendingHostMerge)
    } catch {
        $pendingWithoutPredecessorFailed =
            $_.Exception.Message -match 'barrier-held records'
    }
    if (-not $pendingWithoutPredecessorFailed) {
        throw 'pending host merge classification admitted a missing laggard predecessor'
    }

    $pendingEvents = New-OrderedPrefix $order.first $true
    Add-MergePrefix $pendingEvents
    [void]$pendingEvents.Add((New-ExecuteEvidence execute))
    [void]$pendingEvents.Add((New-ExecuteEvidence join))
    $pending = Assert-LiteralOrderedMasstestStage ($pendingEvents.ToArray()) laggard-merge $order.first $order.laggard -AllowPendingHostMerge
    if ($pending.mergeObserved -or $pending.mergeCount -ne 0 -or
        $pending.mergeClassification -ne 'pending-at-snapshot' -or
        $pending.mergeIndex -ne -1) {
        throw 'valid fixed-window pending merge lost its exact classification'
    }
    $strictPendingFailed = $false
    try {
        [void](Assert-LiteralOrderedMasstestStage ($pendingEvents.ToArray()) laggard-merge $order.first $order.laggard)
    } catch {
        $strictPendingFailed =
            $_.Exception.Message -match 'expected exactly one'
    }
    if (-not $strictPendingFailed) {
        throw 'strict Phase-C gate admitted a pending host merge'
    }
    $releasedPending = [System.Collections.ArrayList]::new()
    foreach ($record in @($pendingEvents)) { [void]$releasedPending.Add($record) }
    [void]$releasedPending.Add([pscustomobject]@{
        event = 'merge-released'; actionId = 900; mergeDay = 3
    })
    $releasedPendingFailed = $false
    try {
        [void](Assert-LiteralOrderedMasstestStage ($releasedPending.ToArray()) laggard-merge $order.first $order.laggard -AllowPendingHostMerge)
    } catch {
        $releasedPendingFailed = $_.Exception.Message -match 'stock release'
    }
    if (-not $releasedPendingFailed) {
        throw 'pending host merge classification admitted an early stock release'
    }
    [void]$pendingEvents.Add((New-ExecuteEvidence host))
    $completedPending = Assert-LiteralOrderedMasstestStage ($pendingEvents.ToArray()) laggard-merge $order.first $order.laggard
    if (-not $completedPending.mergeObserved -or
        $completedPending.mergeCount -ne 1 -or
        $completedPending.mergeClassification -ne 'complete-at-snapshot') {
        throw 'post-snapshot host merge did not satisfy the strict Phase-C gate'
    }

    foreach ($permutation in $permutations) {
        $mergeEvents = New-OrderedPrefix $order.first $true
        Add-MergePrefix $mergeEvents
        foreach ($name in $permutation.names) {
            [void]$mergeEvents.Add((New-ExecuteEvidence $name))
        }
        [void]$mergeEvents.Add([pscustomobject]@{
            event = 'merge-released'; actionId = 900; mergeDay = 3
        })
        $merged = Assert-LiteralOrderedMasstestStage ($mergeEvents.ToArray()) laggard-merge $order.first $order.laggard
        $transaction = Assert-ExactMergeTransaction ($mergeEvents.ToArray()) 3
        if (-not $merged.barrierObserved -or -not $merged.mergeObserved -or
            $merged.mergeIndex -le $merged.laggardAcceptedIndex -or
            [long]$transaction.actionId -ne 900 -or
            @($transaction.barrierHeldPeers).Count -ne 2) {
            throw 'valid v8 laggard merge did not pass its causal gates'
        }
    }
}

$base = New-OrderedPrefix host $true
Add-MergePrefix $base
$execute = New-ExecuteEvidence execute
$hostMerged = New-ExecuteEvidence host
$joinMerged = New-ExecuteEvidence join
$release = [pscustomobject]@{ event = 'merge-released'; actionId = 900; mergeDay = 3 }
$earlyReleaseCases = @(
    [pscustomobject]@{ suffix = @($release, $execute, $hostMerged, $joinMerged) },
    [pscustomobject]@{ suffix = @($execute, $release, $hostMerged, $joinMerged) },
    [pscustomobject]@{ suffix = @($execute, $hostMerged, $release, $joinMerged) }
)
foreach ($case in $earlyReleaseCases) {
    $events = [System.Collections.ArrayList]::new()
    foreach ($record in @($base)) { [void]$events.Add($record) }
    foreach ($record in $case.suffix) { [void]$events.Add($record) }
    $failed = $false
    try { [void](Assert-ExactMergeTransaction ($events.ToArray()) 3) }
    catch { $failed = $_.Exception.Message -match 'causal order' }
    if (-not $failed) {
        throw 'merge release did not wait for all three execute evidence records'
    }
}
Write-Output 'ORDERED_STAGE_GATES_V8_PASS'
`, 'ORDERED_STAGE_GATES_V8_PASS', 'v8 ordered masstest causal gates');
    for (const eventName of [
        'merge-prepare-dispatched',
        'merge-prepare-applied',
        'merge-execute-dispatched',
        'merge-execute-applied',
        'merge-applied',
        'merge-released'
    ]) {
        assert.ok(orderedDeferredStage.includes(`'${eventName}'`),
            `deferred ordered proof lost exact merge event ${eventName}`);
    }
    assert.match(orderedDeferredStage,
        /first-barrier[\s\S]*firstEnd = 2; laggardEnd = 0[\s\S]*barrier = 1; merge = 0[\s\S]*laggard-merge[\s\S]*firstEnd = 2; laggardEnd = 2[\s\S]*barrier = 2; merge = 1/,
        'the deferred proof keeps the first barrier and complete merge expectations distinct');
    assert.match(orderedDeferredStage,
        /merge-prepare-dispatched[\s\S]*merge-prepare-applied[\s\S]*merge-execute-dispatched[\s\S]*merge-execute-applied[\s\S]*merge-applied[\s\S]*merge-released[\s\S]*2 \* \$expected\.merge[\s\S]*Assert-ExactMergeTransaction \$Events 3/,
        'the deferred proof delegates the complete v8 two-barrier merge transaction to one exact oracle');
    assert.match(exactMergeTransaction,
        /barrier-held[\s\S]*foreach \(\$role in @\('host', 'join'\)\)[\s\S]*merge-prepare-dispatched[\s\S]*merge-prepare-applied[\s\S]*merge-execute-dispatched[\s\S]*merge-execute-applied[\s\S]*merge-applied[\s\S]*merge-released/,
        'the exact merge oracle requires both held peers and every v8 transaction record');
    assert.match(exactMergeTransaction,
        /\$indices\.released -le \$indices\.executeApplied[\s\S]*\$indices\.released -le \$indices\.hostMerged[\s\S]*\$indices\.released -le \$indices\.joinMerged/,
        'release must follow execute-applied and both role-local merge-applied records');
    assert.match(exactMergeTransaction,
        /\$executeApplied = Get-ExactSimEvent \$Events 'merge-execute-applied'\s*[\r\n]/,
        'merge-execute-applied is the exact v8 actionId-only record, without a synthetic role');
    assert.doesNotMatch(exactMergeTransaction,
        /merge-execute-applied'\s+'host'|Get-OptionalProperty \$executeApplied 'role'/,
        'the merge oracle cannot require a role field absent from merge-execute-applied');
    assert.doesNotMatch(`${orderedDeferredStage}\n${exactMergeTransaction}`,
        /merge-(?:join|host|commit|operational)-dispatched|merge-now|merge-committed|merge-operational|merge-commit-applied/,
        'the v8 deferred proof contains no legacy merge-stage aliases');

    assert.equal((slotReader.match(/Get-TurnHistory -After \$After -Role host/g) || []).length, 1,
        'each stock slot snapshot is one separate host-side telemetry read');
    assert.match(slotReader,
        /stock-begin-turn-applied[\s\S]*Count = \$slots\.Count[\s\S]*Slots = @\(\$slots\)[\s\S]*SavedHistory = \$history/,
        'the old Get-SlotLines-equivalent read saves its raw host-side BeginApplied sample');
    assert.doesNotMatch(slotReader,
        /senderDpid|receiverDpid|frameLength|dispatchResult|addressee|Assert-NoRelayFault/,
        'native schema validation cannot preempt the old count/latest-line observation');
    assert.match(deferredSlots,
        /stock-begin-turn-applied[\s\S]*\$role -ne 'host'[\s\S]*senderDpid[\s\S]*receiverDpid[\s\S]*frameLength[\s\S]*dispatchResult[\s\S]*addressee[\s\S]*activeHandle/,
        'saved slot authority receives full native identity validation only after the literal outcome');
    assert.match(deferredSlots,
        /expectedHostHandle[\s\S]*\$commandSequence -lt 1[\s\S]*\$startupSlots[\s\S]*\$firstCommandSequence -ne 1[\s\S]*\$firstActiveHandle -ne \$expectedHostHandle[\s\S]*\$wireHighWater = 0/,
        'deferred slot proof admits the one real startup command before strictly increasing natural commands');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Get-OptionalProperty([object]$Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Get-RequiredTelemetryNumber([object]$Object, [string]$Name, [string]$Context) {
    $value = Get-OptionalProperty $Object $Name
    if ($null -eq $value) { throw "$Context omitted '$Name'" }
    [long]$number = $value
    if ($number -lt 0 -or $number -gt [uint32]::MaxValue) {
        throw "$Context '$Name' is outside uint32"
    }
    return $number
}
${deferredSlots}
$hostHandle = [long][Convert]::ToUInt32('A3DE0001', 16)
$joinHandle = [long][Convert]::ToUInt32('A3DE0002', 16)
$neutralHandle = [long][Convert]::ToUInt32('A3DE0000', 16)
$startup = [pscustomobject]@{
    seq = 4; kind = 'stock-begin-turn-applied'; role = 'host'
    senderDpid = 1; receiverDpid = 448087394; frameLength = 56
    dispatchResult = 2; addressee = 0; commandSequence = 1
    activeHandle = $hostHandle
}
$merge = [pscustomobject]@{
    seq = 10; kind = 'stock-begin-turn-applied'; role = 'host'
    senderDpid = 1; receiverDpid = 448087394; frameLength = 56
    dispatchResult = 2; addressee = 0; commandSequence = 13
    activeHandle = $hostHandle
}
$postMerge = [pscustomobject]@{
    seq = 13; kind = 'stock-begin-turn-applied'; role = 'host'
    senderDpid = 1; receiverDpid = 448087394; frameLength = 56
    dispatchResult = 2; addressee = 0; commandSequence = 14
    activeHandle = $joinHandle
}
$slots = @($startup, $merge, $postMerge)
$sample = [pscustomobject]@{
    After = 0; Count = 3; Slots = $slots; SlotWatermark = 13
    SavedHistory = [pscustomobject]@{ events = $slots }
}
$phaseC = [pscustomobject]@{ slotSamples = @($sample) }
$handoff = [pscustomobject]@{
    hostHandle = $hostHandle; joinHandle = $joinHandle; neutralHandle = $neutralHandle
}
$proof = Assert-DeferredAutomaticMasstestHostBeginTurnSlotSamples $phaseC $handoff
if ($proof.sampleCount -ne 1 -or $proof.uniqueSlotCount -ne 3 -or
    $proof.finalCommandSequence -ne 14 -or $proof.hostHandle -ne $hostHandle) {
    throw 'startup-plus-natural slot history did not pass deferred validation'
}
Write-Output 'ORDERED_DEFERRED_STARTUP_SLOT_PASS'
`, 'ORDERED_DEFERRED_STARTUP_SLOT_PASS', 'ordered masstest deferred startup slot contract');
    assert.equal((neutralCount.match(/Read-AutomaticMasstestHostBeginTurnSlots/g) || []).length, 1,
        'neutral counting owns one distinct old Count-PostMergeNeutrals read');
    assert.match(neutralCount,
        /ResolveMergeBoundary[\s\S]*\$After -ne 0[\s\S]*\$HostHandle[\s\S]*\$ExpectedMergeDay[\s\S]*\$boundaries\.Count -ne 1[\s\S]*PostMergeBoundary/,
        'the first neutral read resolves exactly one natural host merge boundary in that same read');
    assert.match(neutralCount,
        /'activeHandle'[\s\S]*'commandSequence'[\s\S]*'seq'[\s\S]*'seq'[\s\S]*'activeHandle'/,
        'the neutral oracle consumes the actual relay telemetry field names');
    assert.doesNotMatch(neutralCount, /\$_\.(?:Day|Sequence)\b/,
        'missing legacy aliases cannot silently cast to zero');
    runPowerShellContract(`
Set-StrictMode -Version Latest
function Get-RequiredTelemetryNumber([object]$Object, [string]$Name, [string]$Context) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -is [bool]) {
        throw "$Context omitted '$Name'"
    }
    [long]$value = 0
    if (-not [long]::TryParse([string]$property.Value, [ref]$value)) {
        throw "$Context has invalid '$Name'"
    }
    return $value
}
$hostHandle = [long][Convert]::ToUInt32('A3DE0001', 16)
$joinHandle = [long][Convert]::ToUInt32('A3DE0002', 16)
$neutralHandle = [long][Convert]::ToUInt32('A3DE0003', 16)
$script:SlotSample = [pscustomobject]@{
    Slots = @(
        [pscustomobject]@{ seq = 10; commandSequence = 17; activeHandle = $hostHandle },
        [pscustomobject]@{ seq = 11; commandSequence = 3; activeHandle = $joinHandle },
        [pscustomobject]@{ seq = 12; commandSequence = 3; activeHandle = $neutralHandle }
    )
    SlotWatermark = 12
}
function Read-AutomaticMasstestHostBeginTurnSlots([long]$After) {
    return $script:SlotSample
}
${neutralCount}
$actual = Get-AutomaticMasstestNeutralCount 0 $neutralHandle $hostHandle 3 -ResolveMergeBoundary
if ($actual.Count -ne 1 -or $actual.PostMergeBoundary -ne 10 -or
    $actual.SlotWatermark -ne 12) {
    throw "relay-field projection returned $($actual.Count)/$($actual.PostMergeBoundary)/$($actual.SlotWatermark)"
}
$script:SlotSample = [pscustomobject]@{
    Slots = @([pscustomobject]@{ seq = 10; activeHandle = $hostHandle })
    SlotWatermark = 10
}
$failedClosed = $false
try {
    [void](Get-AutomaticMasstestNeutralCount 0 $neutralHandle $hostHandle 3 -ResolveMergeBoundary)
} catch {
    $failedClosed = $_.Exception.Message -match "omitted 'commandSequence'"
}
if (-not $failedClosed) { throw 'missing commandSequence did not fail closed' }
Write-Output 'ORDERED_SLOT_FIELDS_PASS'
`, 'ORDERED_SLOT_FIELDS_PASS', 'ordered masstest relay-field runtime contract');
    assert.equal((latestSlot.match(/Read-AutomaticMasstestHostBeginTurnSlots/g) || []).length, 1,
        'latest-slot role selection owns another distinct Get-SlotLines read');
    assert.match(latestSlot,
        /'HOST'[\s\S]*'JOIN'[\s\S]*'NEUTRAL'[\s\S]*'UNKNOWN'/,
        'slot identities retain the old four-way action decision');

    const exactPhaseOrder = [
        'Wait-FixedUtcAnchor ($handoffCompletedUtc.AddSeconds(4))',
        'while ($true)',
        '$neutralObservation = if ($mergeBoundaryResolved)',
        '$slotBaseline = [long]$neutralObservation.PostMergeBoundary',
        'if ($completedNeutralRounds -ge 2 -or $outerTurns -ge 24) { break }',
        'Test-AutomaticMasstestHostAlive $HostProcess',
        '$slotObservation = Get-AutomaticMasstestLatestHostSlot',
        'Start-Sleep -Seconds 2',
        '$outerTurns++',
        "Invoke-AutomaticMasstestHumanEndTurnOnce host",
        "Invoke-AutomaticMasstestHumanEndTurnOnce join",
        '[int]$beforeSlotCount = [int]$slotObservation.Count',
        '[DateTime]$advanceDeadlineUtc = [DateTime]::UtcNow.AddSeconds(26)',
        'Start-Sleep -Milliseconds 800',
        'Test-AutomaticMasstestHostAlive $HostProcess',
        '$freshSlotObservation = Get-AutomaticMasstestLatestHostSlot',
        'if (-not $slotAdvanced -and $humanActionSubmitted -and',
        '$outerTurns++',
        '$finalNeutralObservation = Get-AutomaticMasstestNeutralCount',
        'automatic masstest Phase C drive completed:'
    ];
    previous = -1;
    for (const token of exactPhaseOrder) {
        const at = phase.indexOf(token, previous + 1);
        assert.ok(at > previous, `literal Phase-C checkpoint moved or vanished: ${token}`);
        previous = at;
    }
    assert.match(phase,
        /\$finalNeutralObservation = Get-AutomaticMasstestNeutralCount[\s\S]*completedNeutralRounds = \$completedNeutralRounds[\s\S]*humanActions = @\(\$humanActions\)/,
        'a distinct final neutral-count read is returned to the later live outcome oracle');
    assert.equal((phase.match(
        /\$latestSlotHistoryOrigin \$hostHandle \$joinHandle \$neutralHandle/g) || []).length, 2,
    'both latest-slot reads use the old full-history origin, independent of the neutral boundary');
    assert.match(phase,
        /\[long\]\$latestSlotHistoryOrigin = 0[\s\S]*Get-AutomaticMasstestNeutralCount[\s\S]*\$slotBaseline[\s\S]*Get-AutomaticMasstestLatestHostSlot[\s\S]*\$latestSlotHistoryOrigin/,
        'neutral counting stays post-merge while active-role selection retains the boundary HOST slot');
    assert.doesNotMatch(phase,
        /AUTOMATIC MASSTEST PHASE C RESULT: PASS|\$completedNeutralRounds -lt 2[\s\S]*throw/,
        'the source classified POSTMERGE-SHORT only after its final process/log/dump observations');
    assert.doesNotMatch(phase,
        /Wait-ActionablePair|Start-Sleep -Milliseconds 250|BTN_(?:OK|YES|NO)|Invoke-StockEndTurnAndObserve|Invoke-StockCycleTailAndObserve/,
        'literal Phase C has no quiet/actionable fallback, popup action, or MSS cycle driver');
    assert.equal((phaseAction.match(/Get-RoleState \$Role/g) || []).length, 1,
        'one labelled transport-adapter read supplies only the v8 coordinator UI watermark');
    assert.equal((phaseAction.match(/Invoke-ButtonWhenReady/g) || []).length, 1,
        'the observed human slot arms one exact-ready action intent');
    assert.doesNotMatch(phaseAction, /Invoke-Button(?!WhenReady)/,
        'the transient UI boundary cannot use a snapshot-bound direct action');
    assert.match(phaseAction,
        /Get-RoleState \$Role[\s\S]*'uiSeq'[\s\S]*-AfterUiSequence \(\[long\]\(\$uiSequence - 1\)\)[\s\S]*-WaitMilliseconds 26000[\s\S]*-CommandTimeoutMilliseconds 8000[\s\S]*transportAdapterReads = 1[\s\S]*readinessIntentPosts = 1[\s\S]*readinessWaitMilliseconds = 26000/,
        'one adapter watermark admits the current or first later exact-ready owner within the source request budget');
    assert.doesNotMatch(phaseAction,
        /ConvertTo-SavedDialogObservation|New-EndTurnActionFromObservation/,
        'a transient role publication cannot be projected into a prematurely fixed owner');
    assert.doesNotMatch(phaseAction, /for \(|while \(|catch\s*\{/,
        'a failed human action is terminal and cannot retry/refire');

    const runtime = runner.slice(runner.lastIndexOf('\n$testRelay = $null'));
    const orderedRuntimeAt = runtime.lastIndexOf(
        "if ($GameplayMode -eq 'ordered-masstest') {");
    const nonOrderedRuntimeAt = runtime.indexOf('} else {', orderedRuntimeAt);
    assert.ok(orderedRuntimeAt >= 0 && nonOrderedRuntimeAt > orderedRuntimeAt,
        'standalone ordered runtime branch must remain independently identifiable');
    const orderedRuntime = runtime.slice(orderedRuntimeAt, nonOrderedRuntimeAt);
    const runtimeOrder = [
        '$literalInnerStartupResult = Complete-LiteralInnerStartupObserver',
        '$script:LiteralInnerStartupObserver = $null',
        'Start-Sleep -Seconds 2',
        '$orderedInitialCheckpoint = Read-LiteralMasstestPeerStateCheckpoint',
        '$orderedMergeHandoff = Run-LiteralOrderedMasstestToMerge',
        '$literalPhaseC = Run-AutomaticMasstestPhaseCLiteral',
        '$deferredPhaseCMss = Complete-LiteralOrderedMasstestDeferredProof'
    ];
    previous = -1;
    for (const token of runtimeOrder) {
        const at = orderedRuntime.indexOf(token, previous + 1);
        assert.ok(at > previous, `standalone ordered runtime edge moved or vanished: ${token}`);
        previous = at;
    }
    assert.equal((orderedRuntime.match(/Read-LiteralMasstestPeerStateCheckpoint/g) || []).length, 1,
        'the caller performs exactly one old Get-PeerPids-equivalent read after +2');
    assert.doesNotMatch(orderedRuntime,
        /Wait-LiteralDayReady|Resolve-LiteralHostAuthoritativeHeroes|Invoke-CanonicalDeploy|Invoke-CanonicalConcurrentAttacks|Invoke-CanonicalParallelWalk|Run-IndependentRound|Run-MergeBarrier/,
        'ordered runtime never enters the canonical run_test trajectory');
    const phaseCallAt = orderedRuntime.indexOf(
        '$literalPhaseC = Run-AutomaticMasstestPhaseCLiteral');
    const deferredCallAt = orderedRuntime.indexOf(
        '$deferredPhaseCMss = Complete-LiteralOrderedMasstestDeferredProof',
        phaseCallAt);
    assert.ok(phaseCallAt >= 0 && deferredCallAt > phaseCallAt,
        'deferred MSS strengthening follows the complete literal Phase-C call');
    assert.doesNotMatch(orderedRuntime.slice(phaseCallAt, deferredCallAt),
        /Get-|Read-|Wait-|Start-Sleep|Assert-|Invoke-Button/,
        'main inserts no observation or action before Phase-C PASS');

    assert.equal((orderedDeferred.match(/Read-SimRelayEvents/g) || []).length, 1,
        'the deferred proof owns one full post-PASS event read');
    assert.match(orderedDeferred,
        /Assert-ClientsLive[\s\S]*Assert-NoClientFaults[\s\S]*Assert-LiteralMasstestProcessAlive[\s\S]*Read-SimRelayEvents[\s\S]*Assert-DeferredLiteralOrderedMasstestStage/,
        'client, relay, and full causal strengthening starts only after literal Phase C');
    assert.match(orderedDeferred,
        /Assert-LiteralOrderedMasstestStage[\s\S]*MergeHandoff\.fixedMergeStage\.savedEvents[\s\S]*AllowPendingHostMerge[\s\S]*MergeHandoff\.mergeStage\.savedEvents[\s\S]*laggard-merge[\s\S]*Assert-DeferredLiteralOrderedMasstestStage[\s\S]*\$events laggard-merge/,
        'the fixed classification and bounded completion are revalidated separately from the later full merge chain');
    assert.match(orderedDeferred,
        /natural merge BeginTurn applied and drained \(actionId=\$mergeActionId, day=3\)[\s\S]*relay released stock turns \(actionId=\$mergeActionId, day=3\)[\s\S]*prepared merge transaction \$mergeActionId at stock day 3[\s\S]*host executed merge transaction \$mergeActionId via 0x420FFA/,
        'ordered deferred proof binds all native markers to the saved merge action id');
    assert.match(orderedDeferred,
        /\$expectedExecuteCount = if \(\$roleAndLog\.role -eq 'host'\) \{ 1 \} else \{ 0 \}[\s\S]*\$naturalMergeCount -ne 1 -or \$releaseCount -ne 1 -or[\s\S]*\$prepareCount -ne 1 -or \$executeCount -ne \$expectedExecuteCount[\s\S]*naturalMergeBeginTurnAppliedAndDrained[\s\S]*relayReleasedStockTurns[\s\S]*deferred MSS ordered-masstest proof PASS after literal Phase C/,
        'each client requires prepare/natural/release once while only host may execute merge');
    assert.match(orderedRuntime,
        /orderedMasstestResult = \[pscustomobject\]@\{[\s\S]*fixedMergeClassification[\s\S]*fixedMergeObserved[\s\S]*completionMergeObserved[\s\S]*mergeCompletionTimeoutSeconds[\s\S]*attemptedActions[\s\S]*acceptedActions[\s\S]*recoveryActions[\s\S]*phaseC = \$postMergeAutomaticMasstestPhaseCResult[\s\S]*deferredMssEvidence = \$deferredPhaseCMss/,
        'the summary keeps fixed classification, completion evidence, Phase C, and deferred proof separate');
    assert.match(runner,
        /D2TESTDRV_SCRIPTED_POPUPS_CONFIRMATIONS'\] = '1'/,
        'the persistent native subscriber owns confirmations for ordered Phase C');
});

test('canonical prelude starts only after the literal startup PASS and MSS check', () => {
    const gameplay = fs.readFileSync(gameplayScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const innerStartup = fs.readFileSync(literalInnerStartupScript, 'utf8');
    const deploy = powerShellFunction(gameplay, 'Invoke-CanonicalDeploy');
    const readyPredicate = powerShellFunction(runner, 'Test-LiteralReadyWorldSnapshot');
    const readyLoop = powerShellFunction(runner, 'Start-LiteralOuterReadyObserver');
    const quietLoop = powerShellFunction(runner, 'Wait-LiteralDayReady');
    const witnessSnapshot = powerShellFunction(runner, 'Get-LiteralStartupMssWitnessSnapshot');
    const witnessAssert = powerShellFunction(runner, 'Assert-LiteralStartupMssWitness');
    const heroCensus = powerShellFunction(runner, 'Resolve-LiteralHostAuthoritativeHeroes');
    const deployOrder = [
        'Move-Stack host',
        'Start-Sleep -Seconds 2',
        'Move-Stack join',
        'Start-Sleep -Seconds 3'
    ];
    let previous = -1;
    for (const token of deployOrder) {
        const at = deploy.indexOf(token);
        assert.ok(at > previous, `canonical deploy checkpoint moved or vanished: ${token}`);
        previous = at;
    }
    assert.equal((deploy.match(/Move-Stack\s+(?:host|join)/g) || []).length, 2,
        'deploy has exactly the old host and join Move-Stack action sites');
    const firstDeployActionAt = deploy.indexOf('Move-Stack host');
    assert.doesNotMatch(deploy.slice(firstDeployActionAt),
        /\b(?:Wait-|Get-|Read-|Assert-)/,
        'no observation or convergence gate may enter the literal host/+2/join/+3 deploy schedule');
    assert.match(deploy,
        /Move-Stack host[\s\S]*\$PreparedBindings\.host\.instance[\s\S]*\$PreparedBindings\.host\.appearance[\s\S]*Move-Stack join[\s\S]*\$PreparedBindings\.join\.instance[\s\S]*\$PreparedBindings\.join\.appearance/,
        'both deploy actions consume the terminal saved map owners without a role-state reread');

    assert.match(readyPredicate, /\.Count -lt 1/,
        'the old Is-Ready predicate requires existence, not exact hero uniqueness');
    assert.match(readyPredicate,
        /sourceRole[\s\S]*sequence[\s\S]*Get-LegacyStackRole \$_\) -eq 'host'[\s\S]*Get-LegacyStackRole \$_\) -eq 'joiner'/,
        'the Russobit fixture READY projection consumes the old host/joiner role census');
    assert.match(readyPredicate,
        /relation'\) -eq 'self'[\s\S]*relation'\) -eq 'enemy'[\s\S]*\$selfHumans\.Count -ne 1[\s\S]*\$enemyHumans\.Count -ne 1/,
        'protocol-only READY derives exactly one human self and one human enemy from the same atomic world');
    assert.match(readyPredicate,
        /foreach \(\$owner in @\(\$hostOwner, \$joinOwner\)\)[\s\S]*owner'\) -eq \$owner[\s\S]*\.Count -lt 1/,
        'READY requires a stack owned by each of its two derived/fixed human owners');
    assert.doesNotMatch(readyPredicate, /\$stacks\.Count/,
        'an arbitrary two-stack count is not a READY owner proof');
    assert.doesNotMatch(readyPredicate,
        /Invoke-RestMethod|Get-Process|Get-World|Get-RelayState|Get-RoleState|Wait-|Start-Sleep/,
        'the projected READY predicate is pure over its already-read aggregate world');
    assert.match(readyLoop,
        /for \(\$sampleIndex = 0; \$sampleIndex -lt \[int\]\$MaxSamples; \$sampleIndex\+\+\)[\s\S]*Start-Sleep -Seconds 2[\s\S]*Get-Process -Id \$publishedIds[\s\S]*\$publishedIds\.Count -ne 2[\s\S]*\/api\/legacy-stacks[\s\S]*\/api\/world\?role=host[\s\S]*Invoke-RestMethod \$readyUri/,
        'READY preserves 120 relative +2 samples: scoped census first, one mode-exact aggregate read only for two clients');
    assert.equal((readyLoop.match(/Start-Sleep -Seconds 2/g) || []).length, 1);
    assert.equal((readyLoop.match(/Invoke-RestMethod/g) || []).length, 1);
    assert.match(readyLoop,
        /\$selfHumans = @\([\s\S]*\$enemyHumans = @\([\s\S]*\$selfHumans\.Count -ne 1[\s\S]*\$enemyHumans\.Count -ne 1[\s\S]*\$readyHostOwner -eq \$readyJoinOwner[\s\S]*owner -eq \$readyHostOwner[\s\S]*owner -eq \$readyJoinOwner/,
        'the physical READY worker applies the same exact two-owner predicate to its sole host world');
    assert.match(readyLoop,
        /\$UseLegacyStacks[\s\S]*sourceRole[\s\S]*sequence[\s\S]*0001\$'[\s\S]*0002\$'/,
        'fixture READY uses the old low-16 host/joiner classification on the raw stack census');
    assert.doesNotMatch(readyLoop, /\$stacks\.Count/,
        'the physical READY worker has no generic stack-count admission path');
    assert.doesNotMatch(readyLoop,
        /Popup|ClientLog|Read-SimRelayEvents|\/api\/ui|Invoke-Button/,
        'the outer READY worker cannot service popups, inspect logs, or publish actions');
    assert.match(readyLoop,
        /\$State\.WorldReadySnapshot = \$world[\s\S]*\$startupState = \$State\.StartupObserverState[\s\S]*\$startupState\.OperationalReady[\s\S]*\$startupState\.OperationalWitness[\s\S]*\$State\.Completed = \$true/,
        'READY latches the first world once, then consumes the inner native-operational publication');
    assert.doesNotMatch(readyLoop,
        /Read-SimEvents|Read-CompleteLog|session-operational|bootstrap-released/,
        'the outer READY loop may read synchronized inner state only');
    assert.match(innerStartup,
        /keep-alive-and-done-output-written[\s\S]*'session-operational'[\s\S]*'bootstrap-released'[\s\S]*Read-CompleteLog \$hostLog[\s\S]*Read-CompleteLog \$joinLog[\s\S]*\$State\.OperationalWitness = \$operationalWitness[\s\S]*\$State\.OperationalReady = \$true[\s\S]*tail-artifact-written/,
        'inner startup preserves the old tail, then publishes readiness after relay release and both native UI markers');
    assert.equal((innerStartup.match(/\$State\.OperationalReady = \$true/g) || []).length, 1,
        'native operational readiness has one publication edge');
    assert.match(witnessSnapshot,
        /Get-TurnHistory[\s\S]*Read-SimRelayEvents[\s\S]*HostLogLines = @\(\$LegacyQuietWitness\.HostLogLines\)[\s\S]*JoinLogLines = @\(\$LegacyQuietWitness\.JoinLogLines\)[\s\S]*LegacyQuietEntryRoleStates[\s\S]*PreparedDeployBindings = \$null/,
        'the post-census MSS snapshot reads events once and reuses the immutable terminal log pair');
    assert.equal((witnessSnapshot.match(/Get-TurnHistory/g) || []).length, 1);
    assert.equal((witnessSnapshot.match(/Read-SimRelayEvents/g) || []).length, 1);
    assert.doesNotMatch(witnessSnapshot,
        /Get-RelayState|Get-World|Get-RoleState|Get-Dialog|Get-UiHistory|Invoke-Button|Start-Sleep|Wait-/,
        'the deferred MSS snapshot contains only its two append-only event reads');
    assert.match(quietLoop,
        /\$deadline =[\s\S]*Get-RoleState host[\s\S]*Get-RoleState join[\s\S]*while \([\s\S]*Invoke-LiteralStartupPopupTimelineTick[\s\S]*\$quiet = @\{\}[\s\S]*Get-RelayState[\s\S]*CapturedTick = \[long\]\[Environment\]::TickCount64[\s\S]*HostLogLines = @\(\$PopupTimeline\.HostWindow\.PopupService\.SnapshotLines\)[\s\S]*JoinLogLines = @\(\$PopupTimeline\.JoinPopupService\.SnapshotLines\)[\s\S]*TerminalRoleStates[\s\S]*AddMilliseconds\(800\)[\s\S]*Wait-FixedUtcAnchor/,
        'quiet-3 keeps Host->Client entry reads, passive log ticks, one terminal owner snapshot, and +800 cadence');
    assert.equal((quietLoop.match(/Get-RoleState host/g) || []).length, 1);
    assert.equal((quietLoop.match(/Get-RoleState join/g) || []).length, 1);
    assert.equal((quietLoop.match(/Get-RelayState/g) || []).length, 1,
        'the successful quiet edge captures both map owners in one passive publication read');
    assert.doesNotMatch(quietLoop,
        /Get-LiteralStartupMssWitnessSnapshot|Get-TurnHistory|Read-SimRelayEvents|Get-World|ConvertTo-SavedDialogObservation|New-CanonicalWalkPreparation/,
        'no MSS event, world census, or binding projection may be folded into the old quiet observer');
    assert.doesNotMatch(witnessAssert,
        /Read-|Wait-|Get-TurnHistory|Get-World|Get-RoleState|Get-RelayState|Get-ClientLog|Get-Dialog|Get-UiHistory|Invoke-RestMethod|Start-Sleep|Invoke-Button/,
        'the post-PASS MSS check must use only the already-saved terminal tick');
    assert.equal((heroCensus.match(/Get-LegacyStackSnapshot/g) || []).length, 1,
        'dynamic reinforced hero IDs come from the old sole shared census');

    const main = runner.slice(runner.indexOf('\ntry {', runner.indexOf('$gameplayResult = $null')));
    const literalReadyAt = main.indexOf('Wait-LiteralDayReady -QuietSec 3 -TimeoutSec 60');
    const legacyPassAt = main.indexOf(
        'legacy startup PASS: both role logs reached the exact quiet-3 checkpoint');
    const censusAt = main.indexOf('$heroCensus = Resolve-LiteralHostAuthoritativeHeroes $fixture');
    const savedTerminalStateAt = main.indexOf(
        '$terminalRoleStates = $legacyQuietWitness.TerminalRoleStates');
    const bindingAt = main.indexOf(
        '$preparedDeployBindings = New-CanonicalWalkPreparation');
    const gameplayAt = main.indexOf('Invoke-CanonicalDeploy `');
    const snapshotAt = main.indexOf(
        '$startupQuietWitness = Get-LiteralStartupMssWitnessSnapshot');
    const savedMssAt = main.indexOf('$startupMssProof = Assert-LiteralStartupMssWitness');
    const deferredHeroAt = main.indexOf(
        'Assert-DeferredLiteralHostAuthoritativeHeroFixture');
    assert.ok(literalReadyAt >= 0 && legacyPassAt > literalReadyAt &&
        censusAt > legacyPassAt && savedTerminalStateAt > censusAt &&
        bindingAt > savedTerminalStateAt && gameplayAt > bindingAt &&
        snapshotAt > gameplayAt && savedMssAt > snapshotAt &&
        deferredHeroAt > savedMssAt,
    'quiet PASS must precede census, one binding read, literal deploy, then deferred MSS/startup assertions');
    assert.match(main.slice(gameplayAt, gameplayAt + 240),
        /\$fixture \$hostProcess \$joinProcess \$preparedDeployBindings/,
        'the main path must pass the terminal prepared binding pair into deploy');
    assert.doesNotMatch(main.slice(legacyPassAt, censusAt),
        /Get-|Read-|Wait-|Start-Sleep|Invoke-RestMethod|Assert-ClientsLive|Assert-NoClientFaults|Assert-ProductionRelayHealthy/,
        'no post-PASS I/O may precede the old sole shared census');
    const censusToDeploy = main.slice(censusAt, gameplayAt);
    assert.equal((censusToDeploy.match(/Get-LiteralStartupMssWitnessSnapshot/g) || []).length, 0);
    assert.equal((censusToDeploy.match(/Get-RelayState/g) || []).length, 0);
    assert.equal((censusToDeploy.match(/ConvertTo-SavedDialogObservation/g) || []).length, 2);
    assert.equal((censusToDeploy.match(/New-CanonicalWalkPreparation/g) || []).length, 1);
    assert.equal((censusToDeploy.match(/Assert-LiteralStartupMssWitness/g) || []).length, 0);
    assert.doesNotMatch(censusToDeploy,
        /\bGet-(?!OptionalProperty\b)|\bRead-|\bWait-|Start-Sleep|Invoke-RestMethod|Write-Step|Assert-/,
        'after the census, only pure projection of the saved quiet-edge owners precedes deploy');
});

test('canonical gameplay preserves exact move geometry on normal transport and isolates legacy loopback', () => {
    const native = fs.readFileSync(nativeWorldActionsScript, 'utf8');
    const phaseHooks = fs.readFileSync(nativePhaseGameHooksScript, 'utf8');
    const netIntercept = fs.readFileSync(nativeNetInterceptScript, 'utf8');
    const testNetwork = fs.readFileSync(nativeNettraceScript, 'utf8');
    const testdrv = fs.readFileSync(nativeTestdrvScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const exactBuilder = native.match(
        /bool submitExactLegacyIntent\([\s\S]*?(?=\r?\n\}\r?\n\r?\nbool moveStack)/);
    assert.ok(exactBuilder, 'the old CStackMoveMsg builder must remain independently auditable');
    assert.match(exactBuilder[0], /i <= steps/,
        'the legacy path includes both its origin and occupied target');
    assert.match(exactBuilder[0], /waypoint\.second = i \* 3/,
        'the legacy wire path retains its captured cumulative movement cost');
    assert.match(exactBuilder[0], /listPushBack\(path, waypoint\)[\s\S]*submitStackMoveOnce\([\s\S]*path, start, target/,
        'the complete legacy path is submitted with the requested target as message end');
    assert.equal((exactBuilder[0].match(/submitStackMoveOnce/g) || []).length, 1,
        'one exact legacy intent has exactly one route-aware submission site');
    assert.match(exactBuilder[0], /const bool sent =[\s\S]*listFree\(path\);\s*return sent;/,
        'the test command must return the actual native submission result, never blind success');
    assert.doesNotMatch(exactBuilder[0], /destIdx|nearest|fallback|computeMovementCost/i,
        'the exact source builder cannot be replaced by generic client-side pathfinding');

    const exactSelection = native.match(
        /const bool exactLegacyIntent =[\s\S]*?(?=\n    \/\/ --- Dijkstra)/);
    assert.ok(exactSelection, 'canonical attack/walk selection must remain ahead of generic Dijkstra');
    assert.match(exactSelection[0],
        /const bool cleanLongIntent =[\s\S]*cleanLongIntent && isAllowedExactRoute\(stackId, start, requestedTarget\)[\s\S]*if \(cleanLongIntent && !pinnedCleanLongMove\)[\s\S]*return false;[\s\S]*if \(exactAttack \|\| exactLegacyIntent\)/,
        'an explicit clean plan accepts only its pinned fixture while old walks and attacks stay exact');
    assert.doesNotMatch(exactSelection[0], /Telemetry|TURN_EVENTS/i,
        'observer telemetry cannot select a mutating route builder');
    assert.match(exactSelection[0], /occupiedStack && !exactAttack[\s\S]*return submitExactLegacyIntent/,
        'an occupied friendly target fails closed before the one exact submission');
    assert.match(native,
        /if \(pinnedCleanLongMove\) \{[\s\S]*stackGuardCells\.assign\(cells, false\)[\s\S]*forEachScenarioObject\([\s\S]*IdType::Stack[\s\S]*stackGuardCells\[index\(x, y\)\] = true/,
        'only the pinned clean-long plan snapshots authoritative 3x3 stack guard zones');
    assert.match(native,
        /if \(pinnedCleanLongMove && stackGuardCells\[index\(nx, ny\)\]\)[\s\S]*continue;[\s\S]*stackCanMoveToPosition/,
        'the fixture-only clean route cannot cross a hidden stack engagement halo');
    assert.doesNotMatch(native, /hostCorridor\[\]|joinCorridor\[\]|isPinnedCleanLongWaypoint/,
        'clean movement must not be forced through a guessed fixture corridor');
    assert.match(native,
        /pinnedCleanLongMove[\s\S]*dist\[destIdx\] > static_cast<int>\(stack->movement\)/,
        'clean movement validates the total native route cost before sending');
    assert.match(native,
        /for \(const auto& tile : route\)[\s\S]*if \(isExcludedMoveTile\(tile\)\)[\s\S]*return false;/,
        'the one selected clean route fails closed before send if it crosses a proved movement-triggered popup zone');
    assert.match(native, /return fixtureplan::allowsExactRoute\([\s\S]*start\.x, start\.y, target\.x, target\.y\)/,
        'the exact route allowlist must come from the immutable fixture plan');
    assert.match(native, /return fixtureplan::isExcludedTile\(point\.x, point\.y\)/,
        'popup exclusions must come from that same immutable plan');

    const sender = phaseHooks.match(
        /bool sendStackMoveMsgThroughNativeTransport\([\s\S]*?(?=\n#ifdef D2_TESTDRV\n\s*bool trySendStackMoveMsgThroughNativeTransport)/);
    assert.ok(sender, 'the common one-shot native sender must remain independently auditable');
    assert.equal((sender[0].match(/sendNetMsgToServer/g) || []).length, 1,
        'the common sender has one and only one transport attempt');
    assert.match(sender[0], /if \(!data->clientTakesTurn\)\s*\{?\s*return false;/,
        'the one-shot sender must preserve the native producer-side turn proof');
    assert.match(sender[0], /case simturns::Phase::Held:[\s\S]*case simturns::Phase::Faulted:[\s\S]*return false;/,
        'closed simultaneous-turn phases must reject new movement intents');
    assert.match(sender[0], /const bool sent = CMidgardApi::get\(\)\.sendNetMsgToServer[\s\S]*return sent;/,
        'the native sender must expose the real transport result');

    const submit = native.match(
        /bool submitStackMoveOnce\([\s\S]*?(?=\n\/\/ --- bare game-List node helpers)/);
    assert.ok(submit, 'the one-action host/join transport adapter must remain isolated');
    assert.match(submit[0], /if \(!exactNetworkHost \|\| !g_hostMoveRouteCommitted\.load\(std::memory_order_acquire\)\) \{[\s\S]*return hooks::trySendStackMoveMsgThroughNativeTransport\(/,
        'a join and every ordinary acceptance host retain one natural native submission');
    assert.match(submit[0], /HostMoveArmScope scope\(arm\)[\s\S]*const bool sendResult = hooks::trySendStackMoveMsgThroughNativeTransport\(/,
        'a host arms the legacy route around one native message construction/submission');
    assert.equal((submit[0].match(/trySendStackMoveMsgThroughNativeTransport/g) || []).length, 2,
        'the mutually exclusive host and join branches each contain exactly one submission site');
    assert.match(submit[0], /sendResult && arm\.consumed && arm\.dispatched && !arm\.violation/,
        'host success requires the one armed TX to reach the synchronous server dispatcher');
    const hostSendAt = submit[0].indexOf(
        'const bool sendResult = hooks::trySendStackMoveMsgThroughNativeTransport');
    const hostCallAt = submit[0].indexOf(
        'trySendStackMoveMsgThroughNativeTransport', hostSendAt);
    assert.ok(hostSendAt >= 0 && hostCallAt >= 0 && submit[0].indexOf(
        'trySendStackMoveMsgThroughNativeTransport', hostCallAt + 1) < 0,
    'the host branch has no second submission, retry, or fallback site');

    const txGate = native.match(
        /netintercept::TxDecision legacyHostMoveTxGate\([\s\S]*?(?=\nbool prepareHostMoveArm)/);
    assert.ok(txGate, 'the exact host TX redirect must remain independently auditable');
    assert.match(txGate[0], /arm->consumed = true;[\s\S]*dispatchLocalServerFrameNow\(arm->senderDpid, message\)[\s\S]*TxDecision::Redirect/,
        'the host command is consumed before one direct server dispatch and suppresses natural loopback');
    assert.match(txGate[0], /actualSender == game::serverNetPlayerId[\s\S]*TxDecision::Pass/,
        'nested authoritative server fan-out remains on its natural transport');
    assert.match(txGate[0], /second\/unexpected TX[\s\S]*TxDecision::Reject/,
        'a second client command under one arm is rejected, never replayed');

    const directRx = testNetwork.match(
        /int dispatchLocalServerFrameNow\([\s\S]*?(?=\r?\nnamespace testdetail)/);
    assert.ok(directRx, 'the legacy host resolver must remain in the optional test adapter');
    assert.match(directRx[0], /static_cast<void\*>\(&serverData->netCallbacks\)[\s\S]*testdetail::dispatchLocalServerFrame\(\s*serverReceiveSelf, senderDpid, receiverNetId, message\)/,
        'the unchanged host resolver passes its exact receiver/frame to the core seam');
    const directSeam = netIntercept.match(
        /int testdetail::dispatchLocalServerFrame\([\s\S]*?(?=\r?\n}\r?\n#endif)/);
    assert.ok(directSeam, 'the test adapter must enter the shared RX policy pipeline');
    assert.match(directSeam[0], /receiveHookCore\(\s*receiverSelf, nullptr,[\s\S]*static_cast<int>\(sender\), receiver, false, false\)/,
        'host PacketIn uses CMidServerData+8 and forbids deferred synthetic delivery');
    assert.equal((directSeam[0].match(/\breceiveHookCore\(/g) || []).length, 1,
        'the extracted seam retains exactly one synchronous delivery');
    assert.match(netIntercept, /if \(!allowDefer\) \{[\s\S]*failFastRuntime\([\s\S]*synchronous replay\/direct dispatch/,
        'a synthetic/replayed packet can never be re-enqueued as a retry');

    assert.match(testdrv, /plan\.needsNet =[\s\S]*\|\| plan\.wantWorld;/,
        'world actions install the shared RX/TX seam even without packet logging');
    assert.ok(testdrv.indexOf('nettracehooks::commit(g_plan.wantNet)')
        < testdrv.indexOf('worldactions::commitHostMoveRoute('),
    'the exact host route is claimed only after the common network seam commits');
    assert.match(testdrv,
            /worldactions::commitHostMoveRoute\(\s*g_plan\.wantLegacyHostLoopback,\s*g_plan\.wantExactLegacyMoves,\s*g_plan\.wantCleanLongMoves\)/,
        'immutable command-mode flags, not telemetry, select the move builder');
    assert.match(testdrv, /preflightHostMoveRoute\(g_plan\.wantLegacyHostLoopback\)/,
        'EXACT geometry alone must not install synchronous host loopback');
    assert.match(runner,
        /if \(\$GameplayMode -eq 'long-move' -and\s*\$LongMoveCase -in @\('source-route-control', 'source-pair-repro'\)\) \{[^}]*D2TESTDRV_LEGACY_HOST_LOOPBACK/s,
        'only two explicit historical diagnostics enable local direct delivery');
    assert.match(testdrv,
        /readExactOneGate\("D2TESTDRV_EXACT_LEGACY_MOVES"[\s\S]*readExactOneGate\("D2TESTDRV_CLEAN_LONG_MOVES"[\s\S]*wantExactLegacyMoves && g_plan\.wantCleanLongMoves/,
        'both move modes are strict one-only environment gates and mutually exclusive');
    assert.match(runner,
        /\$exactLegacyMovePlan =[\s\S]*source-route-control[\s\S]*source-pair-repro[\s\S]*\$cleanLongMovePlan =[\s\S]*clean-host-route-control[\s\S]*clean-join-route-control[\s\S]*clean-long-concurrency[\s\S]*D2TESTDRV_EXACT_LEGACY_MOVES[\s\S]*D2TESTDRV_CLEAN_LONG_MOVES/,
        'the launcher assigns every gameplay case to one explicit removable move plan');

    const fixture = JSON.parse(fs.readFileSync(simturnsRussobitFixture, 'utf8'));
    for (const role of ['host', 'join']) {
        const spec = fixture[role];
        let x = spec.deploy.x;
        let y = spec.deploy.y;
        let previous = { x, y };
        while (x !== spec.target.x || y !== spec.target.y) {
            previous = { x, y };
            x += Math.sign(spec.target.x - x);
            y += Math.sign(spec.target.y - y);
        }
        assert.deepEqual(
            { x: spec.battleStart.x, y: spec.battleStart.y },
            previous,
            `${role} battle start must be the server-truncated predecessor from the old green route`);
        assert.deepEqual(
            { x: spec.battleEnd.x, y: spec.battleEnd.y },
            previous,
            `${role} battle verdict must preserve the old green approach tile`);
    }
});

test('native bridge selects one preflighted endpoint without retry or fallback', () => {
    const bridge = fs.readFileSync(nativeBridgeScript, 'utf8');
    assert.match(bridge, /constexpr size_t kPipeNameMaxLength = 256;/);
    assert.match(bridge, /wchar_t pipeName\[kPipeNameMaxLength \+ 1\]/);
    assert.match(bridge, /readExactEnvironmentW\(\s*L"D2TESTDRV_PIPE_NAME"/);
    assert.match(bridge, /pipeState == EnvironmentValueState::Missing[\s\S]*std::wstring\{kPipeName\}/);
    assert.match(bridge, /g_preflightedPipeName = resolved;/);
    assert.equal((bridge.match(/\bCreateFileW\(/g) || []).length, 1);
    assert.equal((bridge.match(/::connect\(/g) || []).length, 1);
    assert.doesNotMatch(bridge, /\bWaitNamedPipe(?:A|W)?\(|\breconnect\b|\bretr(?:y|ies)\b/i);
});

test('native BeginSendReturned is exact passive post-natural-Send evidence', () => {
    const bridge = fs.readFileSync(nativeBridgeScript, 'utf8');
    const netIntercept = fs.readFileSync(nativeNetInterceptScript, 'utf8');
    const testNetwork = fs.readFileSync(nativeNettraceScript, 'utf8');

    assert.match(bridge,
        /BeginSendReturned = 0x020A, \/\/ post-original natural CCmdBeginTurnMsg Send/);
    assert.match(bridge, /static_assert\(kBeginTurnFrameLength == 56,/,
        'the native observer must retain the exact 56-byte Russobit frame extent');

    const observerMatch = bridge.match(
        /void onTxSendReturned\([\s\S]*?(?=\nnetintercept::ObserverBundle telemetryObserverBundle)/);
    assert.ok(observerMatch, 'the BeginTurn post-Send observer must remain independently auditable');
    const observer = observerMatch[0];
    assert.match(observer,
        /if \(message && size == kBeginTurnFrameLength\) \{[\s\S]*messageType == game::netMessageNormalType && storedLength == size[\s\S]*hasExactRtti\(payload, payloadSize, kBeginTurnRtti\)/,
        'the observer must accept only an exact-length normal Russobit BeginTurn frame');
    assert.match(observer,
        /const bool broadcast = idTo == 0 && addressee == 0[\s\S]*commandSequence != 0 && commandSequence != UINT32_MAX[\s\S]*activeHandle != 0;/,
        'the natural broadcast layout must remain finite, addressed to all, and active');
    assert.match(observer,
        /const bool directed = isDynamicPlayerDpid\(idTo\) && addressee != 0[\s\S]*commandSequence == UINT32_MAX && activeHandle != 0;/,
        'the directed layout must retain its remote DPID, addressee, sentinel, and active handle');
    assert.match(observer,
        /std::array<std::uint8_t, 24> evidence\{\};[\s\S]*writeU32\(evidence\.data\(\) \+ 0, idTo\);[\s\S]*writeU32\(evidence\.data\(\) \+ 4, size\);[\s\S]*writeU32\(evidence\.data\(\) \+ 8,[\s\S]*static_cast<std::uint32_t>\(sendResult\)\);[\s\S]*writeU32\(evidence\.data\(\) \+ 12, addressee\);[\s\S]*writeU32\(evidence\.data\(\) \+ 16, commandSequence\);[\s\S]*writeU32\(evidence\.data\(\) \+ 20, activeHandle\);[\s\S]*enqueue\(Op::BeginSendReturned, evidence\.data\(\)/,
        'the bridge must publish exactly the six 32-bit BeginSendReturned evidence fields');
    assert.equal((observer.match(/writeU32\(evidence\.data\(\) \+ /g) || []).length, 6);
    assert.match(bridge,
        /bundle\.txPostSend = &onTxSendReturned;/,
        'BeginTurn evidence must subscribe to the post-Send seam');

    const dispatcherMatch = netIntercept.match(
        /int dispatchTxCore\([\s\S]*?(?=\nbool __fastcall sendHook)/);
    assert.ok(dispatcherMatch, 'the shared natural-Send dispatcher must remain auditable');
    const dispatcher = dispatcherMatch[0];
    const naturalReturnAt = dispatcher.indexOf(
        'const int result = continuation(self, transportContext, idTo, message);');
    const postObserverAt = dispatcher.indexOf('testdetail::observeTxSent(', naturalReturnAt);
    const returnResultAt = dispatcher.lastIndexOf('return result;');
    assert.ok(naturalReturnAt >= 0 && postObserverAt > naturalReturnAt
        && returnResultAt > postObserverAt,
    'the natural continuation must return before passive observers receive its exact result');
    assert.match(dispatcher, /if \(g_sessionTeardown\.load\(std::memory_order_acquire\)\)\s*return continuation\(self, transportContext, idTo, message\);/,
        'closing bypass returns immediately without instrumenting a retired map');
    assert.equal((dispatcher.slice(dispatcher.indexOf('TxCompletionTask completion')).match(/\bcontinuation\(/g) || []).length, 1,
        'the active TX seam must invoke its selected natural continuation exactly once');
    assert.doesNotMatch(dispatcher.slice(postObserverAt), /\bcontinuation\s*\(/,
        'post-Send observation must not resubmit the natural frame');
    assert.match(dispatcher,
        /testdetail::observeTxSent\(self, idTo, postSendMessage, postSendSize, result\)/,
        'core forwards the original pre-captured frame extent and natural Send result');
    const fanout = testNetwork.match(
        /void observeTxSent\([\s\S]*?(?=\n} \/\/ namespace testdetail)/);
    assert.ok(fanout, 'the optional post-Send fanout remains independently auditable');
    assert.match(fanout[0],
        /g_txPostSendObserverCount\.load\(std::memory_order_acquire\)[\s\S]*g_txPostSendObservers\[static_cast<std::size_t>\(i\)\]\(\s*self, idTo, postSendMessage, postSendSize, result\)/,
        'the moved fanout preserves the exact arguments and acquired observer order');
    assert.doesNotMatch(fanout[0],
        /\b(?:continuation|dispatchTx|dispatchLocalServerFrameNow|directPlaySendContinuation|sendHook|armCurrentTxCompletion|enqueueDeferredPacket)\s*\(/,
        'moving the fanout out of core must not introduce a send or synthetic delivery');
    assert.doesNotMatch(observer,
        /\b(?:dispatchTx|dispatchLocalServerFrameNow|directPlaySendContinuation|sendHook|armCurrentTxCompletion|enqueueDeferredPacket)\s*\(/,
        'the evidence observer must expose no resend, redirect, or synthetic-delivery action site');
});

test('relay retains exact broadcast and directed BeginSendReturned evidence by role',
    { timeout: 10000 }, async (t) => {
        assert.equal(Op.BeginSendReturned, 0x020a);
        const relay = await startRelay(t);
        const host = await relay.connect({ role: 'host', pid: 4242, autoAcknowledge: false });
        const join = await relay.connect({ role: 'join', pid: 4243, autoAcknowledge: false });
        await waitFor(() => host.count(Op.HelloAck) === 1 && join.count(Op.HelloAck) === 1,
            2000, 'both v4 HelloAck frames');

        const broadcast = beginSendReturned({
            idTo: 0,
            sendResult: -2147024891,
            addressee: 0,
            commandSequence: 17,
            activeHandle: 0xa3de0001,
        });
        assert.equal(broadcast.length, 24);
        assert.deepEqual([
            broadcast.readUInt32LE(0), broadcast.readUInt32LE(4),
            broadcast.readInt32LE(8), broadcast.readUInt32LE(12),
            broadcast.readUInt32LE(16), broadcast.readUInt32LE(20),
        ], [0, 56, -2147024891, 0, 17, 0xa3de0001]);
        host.send(Op.BeginSendReturned, broadcast);

        let hostHistory;
        await waitFor(async () => {
            hostHistory = await requestJson(
                relay.base, '/api/turn/history?role=host&after=0');
            return hostHistory.body.events?.length === 1;
        }, 2000, 'retained host broadcast Send result');
        const { t: hostTime, ...hostEvidence } = hostHistory.body.events[0];
        assert.equal(typeof hostTime, 'string');
        assert.deepEqual(hostEvidence, {
            seq: 1,
            kind: 'stock-begin-turn-send-returned',
            role: 'host',
            idTo: 0,
            frameLength: 56,
            sendResult: -2147024891,
            addressee: 0,
            commandSequence: 17,
            activeHandle: 0xa3de0001,
        });

        const directed = beginSendReturned({
            idTo: 0xa3de0002,
            sendResult: -1,
            addressee: 0xa3de0002,
            commandSequence: 0xffffffff,
            activeHandle: 0xa3de0001,
        });
        assert.equal(directed.length, 24);
        assert.deepEqual([
            directed.readUInt32LE(0), directed.readUInt32LE(4),
            directed.readInt32LE(8), directed.readUInt32LE(12),
            directed.readUInt32LE(16), directed.readUInt32LE(20),
        ], [0xa3de0002, 56, -1, 0xa3de0002, 0xffffffff, 0xa3de0001]);
        join.send(Op.BeginSendReturned, directed);

        let joinHistory;
        await waitFor(async () => {
            joinHistory = await requestJson(
                relay.base, '/api/turn/history?role=join&after=0');
            return joinHistory.body.events?.length === 1;
        }, 2000, 'retained join directed Send result');
        const { t: joinTime, ...joinEvidence } = joinHistory.body.events[0];
        assert.equal(typeof joinTime, 'string');
        assert.deepEqual(joinEvidence, {
            seq: 2,
            kind: 'stock-begin-turn-send-returned',
            role: 'join',
            idTo: 0xa3de0002,
            frameLength: 56,
            sendResult: -1,
            addressee: 0xa3de0002,
            commandSequence: 0xffffffff,
            activeHandle: 0xa3de0001,
        });
        assert.deepEqual(host.received.map((message) => message.op), [Op.HelloAck]);
        assert.deepEqual(join.received.map((message) => message.op), [Op.HelloAck]);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

for (const scenario of [
    {
        name: '23-byte payload',
        payload: () => Buffer.alloc(23),
        reason: /^malformed BeginSendReturned: BeginSendReturned payload must be exactly 24 bytes, got 23$/,
    },
    {
        name: 'non-Russobit frame length',
        payload: () => beginSendReturned({
            idTo: 0, frameLength: 55, sendResult: 1, addressee: 0,
            commandSequence: 1, activeHandle: 0xa3de0001,
        }),
        reason: /^malformed BeginSendReturned: BeginSendReturned frameLength must be the exact Russobit size 56$/,
    },
    {
        name: 'zero-sequence broadcast layout',
        payload: () => beginSendReturned({
            idTo: 0, sendResult: 1, addressee: 0,
            commandSequence: 0, activeHandle: 0xa3de0001,
        }),
        reason: /^malformed BeginSendReturned: BeginSendReturned must carry one exact broadcast or directed Russobit layout$/,
    },
    {
        name: 'finite-sequence directed layout',
        payload: () => beginSendReturned({
            idTo: 0xa3de0002, sendResult: 1, addressee: 0xa3de0002,
            commandSequence: 9, activeHandle: 0xa3de0001,
        }),
        reason: /^malformed BeginSendReturned: BeginSendReturned must carry one exact broadcast or directed Russobit layout$/,
    },
]) {
    test(`BeginSendReturned ${scenario.name} is terminal without an action retry`,
        { timeout: 10000 }, async (t) => {
            const relay = await startRelay(t);
            const agent = await relay.connect({
                role: 'host', pid: 4242, autoAcknowledge: false,
            });
            await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');

            agent.send(Op.BeginSendReturned, scenario.payload());
            const terminal = await waitForTerminal(relay.base, scenario.reason);
            assert.match(terminal.reason, scenario.reason);
            const history = await requestJson(
                relay.base, '/api/turn/history?role=host&after=0');
            assert.deepEqual(history.body.events, [],
                'rejected Send evidence must not enter retained turn history');
            await new Promise((resolve) => setTimeout(resolve, 100));
            assert.deepEqual(agent.received.map((message) => message.op), [Op.HelloAck],
                'a malformed observation must never dispatch or retry an action');
        });
}

test('native startup witness models all four ordered Russobit transitions', () => {
    const bridge = fs.readFileSync(nativeBridgeScript, 'utf8');
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const fixture = JSON.parse(fs.readFileSync(stockStartupCJoinFixture, 'utf8'));
    const observerMatch = bridge.match(
        /netintercept::RxDecision onJoinStartupRxObserved[\s\S]*?(?=\n\/\/ Post-original)/);
    assert.ok(observerMatch, 'startup RX observer must remain identifiable');
    const observer = observerMatch[0];

    const payloads = fixture.orderedPayloads.map((record) =>
        Buffer.from(record.payloadHex.replaceAll(' ', ''), 'hex'));
    assert.equal(payloads.length, 2);
    for (const payload of payloads) {
        assert.equal(payload.length, fixture.layout.payloadSize);
        assert.equal(payload.subarray(0, 19).toString('ascii'), '.?AVCJoinGameMsg@@\0');
        assert.equal(payload.readUInt32LE(fixture.layout.nameLengthOffset), 8);
    }
    const observedHandles = payloads.map((payload) =>
        payload.readUInt32LE(fixture.layout.joinedHandleOffset));
    assert.deepEqual(observedHandles, [0xa3de0001, 0xa3de0002],
        'the preserved stock stream announces the host player first, then the join player');

    assert.match(bridge, /std::mutex g_joinStartupMutex;/);
    assert.match(bridge, /struct JoinStartupWitness[\s\S]*senderDpid[\s\S]*receiverDpid[\s\S]*hostHandle[\s\S]*joinHandle/);
    assert.doesNotMatch(bridge, /std::atomic<JoinStartupPhase>/,
        'startup identity, phase and enqueue ordering must share one mutex');
    assert.ok((observer.match(/std::lock_guard<std::mutex> startupLock\(g_joinStartupMutex\);/g)
        || []).length >= 2, 'both BeginTurn and CJoin classification must hold the witness mutex');
    assert.match(observer,
        /enqueue\(Op::StartupBeginObserved[\s\S]*g_joinStartupWitness\.phase = JoinStartupPhase::AwaitHostJoin/,
        'AwaitHostJoin must publish only after BeginTurn evidence is queued');
    assert.match(observer,
        /phase == JoinStartupPhase::AwaitHostJoin[\s\S]*joinedHandle != g_joinStartupWitness\.hostHandle[\s\S]*enqueue\(Op::StartupJoinObserved[\s\S]*AwaitDirectedJoinBegin/,
        'the first CJoin must be exactly the latched host transition');
    assert.match(observer,
        /phase != JoinStartupPhase::AwaitDirectedJoinBegin[\s\S]*joinHandle = addressee[\s\S]*enqueue\(Op::StartupDirectedBeginObserved[\s\S]*AwaitJoinPlayerJoin/,
        'the directed BeginTurn must latch one distinct join handle');
    assert.match(observer,
        /phase == JoinStartupPhase::AwaitJoinPlayerJoin[\s\S]*joinedHandle != g_joinStartupWitness\.joinHandle[\s\S]*enqueue\(Op::StartupCompleteObserved[\s\S]*JoinStartupPhase::Complete/,
        'the second CJoin must complete exactly the directed join-player identity');
    assert.match(observer, /CJoinGame repeated after exact stock startup completion/,
        'post-completion repeats must remain terminal');
    assert.doesNotMatch(bridge, /g_startupJoinObserved/,
        'a class-wide CJoin latch would misclassify the stock join-player transition');
    assert.match(runner, /\$joinedHandle -ne \$activeHandle/,
        'the consumer must correlate CJoin to the startup BeginTurn host handle');
    assert.match(runner,
        /stock startup evidence violated B\(H\)<CJoin\(H\)<D\(J,H\)<CJoin\(J\)/,
        'the consumer must validate the complete four-event causal order');
});

test('relay retains all four startup transitions without coalescing',
    { timeout: 10000 }, async (t) => {
        const relay = await startRelay(t);
        const agent = await relay.connect({ role: 'join', pid: 4243 });
        await waitFor(() => agent.count(Op.HelloAck) === 1, 2000, 'v4 HelloAck');

        agent.socket.write(Buffer.concat([
            frame(Op.StartupBeginObserved,
                startupBeginObserved(0, 1, 0xa3de0001)),
            frame(Op.StartupJoinObserved, startupJoinObserved(0xa3de0001)),
            frame(Op.StartupDirectedBeginObserved,
                startupBeginObserved(0xa3de0002, 0xffffffff, 0xa3de0001)),
            frame(Op.StartupCompleteObserved, startupJoinObserved(0xa3de0002)),
        ]));

        let history;
        await waitFor(async () => {
            history = await requestJson(relay.base, '/api/turn/history?role=join&after=0');
            return history.body.events?.length === 4;
        }, 2000, 'four retained startup transitions');
        assert.deepEqual(history.body.events.map((entry) => entry.kind), [
            'stock-startup-begin-turn-observed',
            'stock-startup-join-game-observed',
            'stock-startup-directed-begin-turn-observed',
            'stock-startup-complete-observed',
        ]);
        assert.deepEqual(history.body.events.map((entry) => entry.seq), [1, 2, 3, 4]);
        assert.equal(history.body.events[0].activeHandle, 0xa3de0001);
        assert.equal(history.body.events[1].joinedHandle, 0xa3de0001);
        assert.equal(history.body.events[2].addressee, 0xa3de0002);
        assert.equal(history.body.events[3].joinedHandle, 0xa3de0002);
        assert.equal((await requestJson(relay.base, '/api/status')).body.terminalFault, null);
    });

test('physical navigation treats only passive GET cancellation as a missed observation', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const relay = fs.readFileSync(relayScript, 'utf8');
    const timeoutClassifier = powerShellFunction(
        runner, 'Test-LiteralNavigationPassiveTimeoutException');
    const passiveRoleState = powerShellFunction(
        runner, 'Get-LiteralNavigationRoleState');
    const roleStateProjection = powerShellFunction(
        runner, 'Get-LiteralNavigationRoleStateProjection');
    const navigationUInt32 = powerShellFunction(
        runner, 'ConvertTo-LiteralNavigationUInt32');
    const dialogConverter = powerShellFunction(
        runner, 'ConvertTo-LiteralNavigationDialogObservation');
    const dialogProjection = powerShellFunction(
        runner, 'Get-LiteralNavigationDialogObservation');
    const optionalProperty = powerShellFunction(runner, 'Get-OptionalProperty');
    const selectionRequest = powerShellFunction(
        runner, 'Start-LiteralSelectionRequest');
    const buttonRequest = powerShellFunction(
        runner, 'Start-LiteralButtonRequest');
    const mainMenuAnchor = powerShellFunction(
        runner, 'Get-LiteralMainMenuAtLaunchAnchor');
    const hostLane = powerShellFunction(
        runner, 'Invoke-LiteralHostNavigationLane');
    const joinLane = powerShellFunction(
        runner, 'Invoke-LiteralJoinNavigationLane');

    runPowerShellContract(`
${timeoutClassifier}
if (-not (Test-LiteralNavigationPassiveTimeoutException ([System.TimeoutException]::new('timeout')))) {
    throw 'direct timeout was not classified as a passive miss'
}
$nested = [System.Exception]::new(
    'outer', [System.OperationCanceledException]::new('cancelled'))
if (-not (Test-LiteralNavigationPassiveTimeoutException $nested)) {
    throw 'nested cancellation was not classified as a passive miss'
}
if (Test-LiteralNavigationPassiveTimeoutException ([System.Net.Http.HttpRequestException]::new('connection failed'))) {
    throw 'connection failure was incorrectly hidden as a passive miss'
}
'NAV_PASSIVE_TIMEOUT_CLASSIFIER_PASS'
`, 'NAV_PASSIVE_TIMEOUT_CLASSIFIER_PASS',
    'literal navigation passive-timeout classifier');
    runPowerShellContract(`
Set-StrictMode -Version Latest
${optionalProperty}
${roleStateProjection}
${navigationUInt32}
${dialogConverter}
function Throws([scriptblock]$Action) {
    try { [void](& $Action); return $false } catch { return $true }
}
$missingRole = Get-LiteralNavigationRoleStateProjection host ([pscustomobject]@{
    terminalFault=$null; roles=[pscustomobject]@{}
})
if ($null -ne $missingRole) { throw 'an absent pre-Hello role was not a benign miss' }
$validState=[pscustomobject]@{
    dialog='DLG_PROTOCOL'; dialogInstance=[long]7; dialogAppearance=[long]7
    dialogReady=$true
}
$validResponse=[pscustomobject]@{
    terminalFault=$null; roles=[pscustomobject]@{ host=$validState }
}
if (-not [object]::ReferenceEquals(
        (Get-LiteralNavigationRoleStateProjection host $validResponse),
        $validState)) {
    throw 'a valid exact role-state object was not preserved'
}
$scalarRoot = '1' | ConvertFrom-Json
$scalarRoles = '{"terminalFault":null,"roles":1}' | ConvertFrom-Json
$arrayRoles = '{"terminalFault":null,"roles":[]}' | ConvertFrom-Json
$scalarRole = '{"terminalFault":null,"roles":{"host":1}}' | ConvertFrom-Json
if (-not (Throws { Get-LiteralNavigationRoleStateProjection host ([pscustomobject]@{ roles=[pscustomobject]@{} }) }) -or
    -not (Throws { Get-LiteralNavigationRoleStateProjection host $scalarRoot }) -or
    -not (Throws { Get-LiteralNavigationRoleStateProjection host $scalarRoles }) -or
    -not (Throws { Get-LiteralNavigationRoleStateProjection host $arrayRoles }) -or
    -not (Throws { Get-LiteralNavigationRoleStateProjection host $scalarRole })) {
    throw 'malformed root/roles/role schema was hidden as an observation miss'
}
$observation=ConvertTo-LiteralNavigationDialogObservation host $validState
if ($observation.Dialog -ne 'DLG_PROTOCOL' -or $observation.Instance -ne 7 -or
    -not $observation.Ready) {
    throw 'valid dialog observation changed during typed projection'
}
foreach ($invalidState in @(
    [pscustomobject]@{ dialogInstance=7; dialogAppearance=7; dialogReady=$true },
    [pscustomobject]@{ dialog=1; dialogInstance=7; dialogAppearance=7; dialogReady=$true },
    [pscustomobject]@{ dialog='DLG_PROTOCOL'; dialogInstance='7'; dialogAppearance=7; dialogReady=$true },
    [pscustomobject]@{ dialog='DLG_PROTOCOL'; dialogInstance=7.5; dialogAppearance=7.5; dialogReady=$true },
    [pscustomobject]@{ dialog='DLG_PROTOCOL'; dialogInstance=0; dialogAppearance=0; dialogReady=$true },
    [pscustomobject]@{ dialog='DLG_PROTOCOL'; dialogInstance=7; dialogAppearance=8; dialogReady=$true }
)) {
    if (-not (Throws { ConvertTo-LiteralNavigationDialogObservation host $invalidState })) {
        throw 'malformed dialog identity/readiness was accepted'
    }
}
'NAV_PASSIVE_SCHEMA_PROJECTION_PASS'
`, 'NAV_PASSIVE_SCHEMA_PROJECTION_PASS',
    'literal navigation passive schema projection');

    assert.match(timeoutClassifier,
        /System\.TimeoutException[\s\S]*System\.OperationCanceledException[\s\S]*InnerException/,
        'only timeout/cancellation, including an inner exception, is a passive observation miss');
    assert.doesNotMatch(timeoutClassifier,
        /HttpRequestException|WebException|IOException|SocketException/,
        'connection and protocol failures cannot be swallowed by the timeout classifier');

    assert.match(passiveRoleState,
        /\$startedUtc = \[DateTime\]::UtcNow[\s\S]*\$remainingMilliseconds[\s\S]*\[Math\]::Min\([\s\S]*10000[\s\S]*\$remainingMilliseconds\)/,
        'each passive GET is bounded by ten seconds and the remaining physical-lane deadline');
    assert.match(passiveRoleState,
        /HttpClientHandler\]::new\(\)[\s\S]*UseProxy = \$false[\s\S]*\$client\.GetAsync\(\$uri\)[\s\S]*Test-LiteralNavigationPassiveTimeoutException[\s\S]*return \$null/,
        'a timed-out no-proxy GET produces only an absent observation');
    assert.match(passiveRoleState,
        /role=\{0\}; context=\{1\}; uri=\{2\}; startedUtc=\{3:O\};[\s\S]*completedUtc=\{4:O\}; elapsedMs=\{5\}; requestTimeoutMs=\{6\};[\s\S]*laneDeadlineUtc=\{7:O\}; exception=\{8\}/,
        'the observation miss retains role, context, URI, both timestamps, budgets, and exception type');
    assert.match(passiveRoleState,
        /if \(-not \(Test-LiteralNavigationPassiveTimeoutException[\s\S]*throw[\s\S]*Get-LiteralNavigationRoleStateProjection \$Role \$response/,
        'only timeout exceptions become misses; decoded state must pass the strict schema projection');
    assert.equal((passiveRoleState.match(/\.GetAsync\(/g) || []).length, 1,
        'one passive sample owns exactly one GET');
    assert.doesNotMatch(passiveRoleState,
        /Invoke-RestMethod|\.PostAsync\(|HttpMethod\]::Post|\.SendAsync\(|\bwhile\s*\(|\bfor\s*\(/,
        'the passive state adapter cannot publish or retry an action');
    assert.match(roleStateProjection,
        /\$Response -isnot \[System\.Management\.Automation\.PSCustomObject\][\s\S]*Properties\['terminalFault'\][\s\S]*Properties\['roles'\][\s\S]*\$rolesProperty\.Value -isnot[\s\S]*\[System\.Management\.Automation\.PSCustomObject\][\s\S]*if \(\$null -eq \$roleProperty\) \{ return \$null \}[\s\S]*\$roleProperty\.Value -isnot[\s\S]*\[System\.Management\.Automation\.PSCustomObject\]/,
        'missing role alone is benign while root, terminal, roles, and present role schemas fail closed');
    assert.equal((roleStateProjection.match(
        /\[System\.Management\.Automation\.PSCustomObject\]/g) || []).length, 3,
    'root, roles, and present role each require the explicit non-accelerator JSON object type');
    assert.match(navigationUInt32,
        /\$Value -is \[bool\][\s\S]*\$Value -is \[string\][\s\S]*\[decimal\]::Truncate[\s\S]*\$number -lt 1[\s\S]*\$number -gt \[uint32\]::MaxValue/,
        'dialog identity accepts only a positive integral uint32 value');
    assert.match(dialogConverter,
        /\$State -isnot \[System\.Management\.Automation\.PSCustomObject\][\s\S]*\$dialogProperty\.Value -isnot \[string\][\s\S]*IsNullOrWhiteSpace[\s\S]*\$readyValue -isnot \[bool\][\s\S]*ConvertTo-LiteralNavigationUInt32[\s\S]*\$appearance -ne \$instance/,
        'dialog name, exact uint32 identity, boolean readiness, and owner equality are typed');
    assert.equal((dialogProjection.match(/Get-LiteralNavigationRoleState/g) || []).length, 1,
        'one dialog projection consumes one timeout-tolerant role-state observation');

    for (const [name, lane] of [['host', hostLane], ['join', joinLane]]) {
        assert.equal((lane.match(/Get-LiteralNavigationDialogObservation/g) || []).length, 4,
            `${name} lane routes all four physical dialog observations through the passive adapter`);
        assert.doesNotMatch(lane,
            /Get-DialogObservation|Get-RoleState|Invoke-RestMethod/,
            `${name} lane cannot bypass the timeout-tolerant passive observation boundary`);
    }
    assert.match(mainMenuAnchor,
        /Get-LiteralNavigationRoleState[\s\S]*if \(\$roleState\)[\s\S]*ConvertTo-LiteralNavigationDialogObservation/,
        'the delayed first-dialog anchor uses the same passive miss semantics');

    assert.match(selectionRequest,
        /\$observation = \$CapturedObservation[\s\S]*appearance=\$\(\[long\]\$observation\.Instance\)&instance=\$targetInstance[\s\S]*\$client\.SendAsync\(\$request\)/,
        'selection reuses its captured appearance/owner and leaves atomic validation to the relay');
    assert.equal((selectionRequest.match(/\.SendAsync\(/g) || []).length, 1,
        'a physical selection has one and only one POST publication');
    assert.doesNotMatch(selectionRequest,
        /Get-DialogObservation|Get-LiteralNavigationRoleState|Get-RoleState|\bwhile\s*\(|\bfor\s*\(/,
        'selection cannot reopen a client-side TOCTOU window or retry');
    assert.match(buttonRequest,
        /\$capturedTarget = Get-ReadyActionTarget \$CapturedObservation[\s\S]*\$candidate = \$CapturedObservation[\s\S]*appearance=\{4\}&instance=\{5\}/,
        'direct navigation buttons reuse the exact captured observation in their sole relay mutation');
    assert.equal((buttonRequest.match(/\.SendAsync\(/g) || []).length, 1,
        'all button modes converge on one POST publication site');
    assert.doesNotMatch(buttonRequest,
        /Get-DialogObservation|Get-LiteralNavigationRoleState|Get-RoleState|\bwhile\s*\(|\bfor\s*\(/,
        'a button action cannot reread its owner or retry after publication');

    const exactRelayTarget = relay.match(
        /function readyDialogClient\([\s\S]*?(?=\nfunction sendCommandJson)/)?.[0] || '';
    assert.match(exactRelayTarget,
        /dialogReady !== true[\s\S]*dialogInstance !== current\.dialogAppearance[\s\S]*current\.dialogAppearance !== appearance[\s\S]*target\.dialog === dialog[\s\S]*target\.instance === owner[\s\S]*controls\.length !== 1/,
        'the relay atomically rejects stale appearance, owner, dialog, or control before native dispatch');
});

test('runtime observes the native exact-once startup subscriber without owning popup actions', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const pairing = powerShellFunction(runner, 'Run-Pairing');
    const navigation = powerShellFunction(
        runner, 'Complete-LiteralIndependentNavigation');
    const hostLane = powerShellFunction(
        runner, 'Invoke-LiteralHostNavigationLane');
    const joinLane = powerShellFunction(
        runner, 'Invoke-LiteralJoinNavigationLane');
    const workerStart = powerShellFunction(
        runner, 'Start-LiteralNavigationWorker');
    const workerComplete = powerShellFunction(
        runner, 'Complete-LiteralNavigationWorker');
    const buttonRequest = powerShellFunction(
        runner, 'Start-LiteralButtonRequest');
    const main = runner.slice(runner.lastIndexOf('\n$testRelay = $null'));
    const popupServiceFactory = powerShellFunction(runner, 'New-LiteralStartupPopupService');
    const popupFactory = powerShellFunction(runner, 'New-LiteralStartupPopupWindow');
    const persistentPopupTick = powerShellFunction(runner, 'Invoke-LiteralPersistentStartupPopupTick');
    const updatePopupWindow = powerShellFunction(runner, 'Update-LiteralStartupPopupWindow');
    const popupTick = powerShellFunction(runner, 'Invoke-LiteralStartupPopupTick');
    const popupWait = powerShellFunction(runner, 'Wait-LiteralStartupPopupWindow');
    const popupTimelineTick = powerShellFunction(runner, 'Invoke-LiteralStartupPopupTimelineTick');
    const dayReady = powerShellFunction(runner, 'Wait-LiteralDayReady');
    const startClient = powerShellFunction(runner, 'Start-SimturnGameClient');
    const bootstrapOperational = powerShellFunction(
        runner, 'Assert-ExactBootstrapOperational');

    const machineGateAt = main.indexOf('Assert-ExclusiveTestMachine -GameDir $GameDir');
    const relayStartAt = main.indexOf('$testRelay = Start-TestRelay -LogDir $ArtifactDir', machineGateAt);
    assert.ok(machineGateAt >= 0 && relayStartAt > machineGateAt,
        'M0 machine gate and relay boundary must remain independently auditable');
    const preRelayClock = main.slice(machineGateAt, relayStartAt);
    assert.deepEqual(
        [...preRelayClock.matchAll(/Start-Sleep -Milliseconds (1200|500|300)/g)]
            .map((match) => Number(match[1])),
        [1200, 1200, 500, 1200, 500, 1200, 300],
        'M0 keeps all four cleanup +1200 edges and separate +500/+500/+300 wrapper clocks');
    assert.match(preRelayClock,
        /Start-Sleep -Milliseconds 1200[\s\S]*Start-LiteralOuterReadyObserver[\s\S]*Start-Sleep -Milliseconds 1200[\s\S]*Start-Sleep -Milliseconds 500[\s\S]*literalModWrapperHandoff[\s\S]*Start-Sleep -Milliseconds 1200[\s\S]*Start-Sleep -Milliseconds 500[\s\S]*Get-LiteralNestedStartupSubstitutionMap[\s\S]*Start-Sleep -Milliseconds 1200[\s\S]*Start-Sleep -Milliseconds 300[\s\S]*literalStartWrapperHandoff/,
        'outer READY arms after the first cleanup clock and every nested wrapper retains its own boundary');

    const nestedStartupMap = powerShellFunction(
        runner, 'Get-LiteralNestedStartupSubstitutionMap');
    for (const token of [
        'LegacyRolePollMilliseconds = 500',
        'LegacyStrategicPollMilliseconds = 400',
        'LegacyPostStrategicSettleSeconds = 2',
        'LegacyJoinActivationToCascadeMilliseconds = 500',
        'LegacyCascadeToPacketBaselineMilliseconds = 800',
        'direct propagated startup/pairing exception replaces FAIL: roles never settled bootlog observation',
        'one release file -> session-plan-created + session-plan-delivered(host/join) -> host/join session activation + directed day-1 BeginTurn apply',
        'one +500 ms timer is armed at the exact BootstrapBeginTurnApplied completion edge; host activation must arrive before its fixed deadline',
        'engine-action-dispatched(stage=bootstrap-apply) -> bootstrap-cascade-complete + bootstrap-turn-info-applied',
        'bootstrap-commit-dispatched -> bootstrap-commit-applied(host/join) -> bootstrap-operational-dispatched -> bootstrap-operational-applied(host/join) -> session-operational',
        'literal two-read Begin-then-End host TX census after the fixed cascade +800 ms edge',
        'literal legacy observer/read projection with one typed v8 MSS release-edge substitution; MSS evidence follows old verdict boundaries'
    ]) {
        assert.ok(nestedStartupMap.includes(token),
            `nested startup substitution lost: ${token}`);
    }
    assert.doesNotMatch(nestedStartupMap,
        /Invoke-RestMethod|\/api\/|Start-Sleep|Get-RoleState|Get-RelayState/,
        'agreed MSS ACK substitutions are documented, not replayed as obsolete lobby mutations/polls');
    assert.doesNotMatch(nestedStartupMap, /restart|retry|catchup/i,
        'the BeginApplied +500 timer is fixed once and has no restart or late catch-up path');
    assert.match(bootstrapOperational,
        /planEpoch[\s\S]*hostDeliveredEpoch[\s\S]*joinDeliveredEpoch[\s\S]*operationalEpoch[\s\S]*\$planEpoch -le 0[\s\S]*\$hostDeliveredEpoch -ne \$planEpoch[\s\S]*\$joinDeliveredEpoch -ne \$planEpoch[\s\S]*\$operationalEpoch -ne \$planEpoch/,
        'session-plan delivery and session-operational retain one positive authoritative epoch');
    assert.match(bootstrapOperational,
        /planMergeDay[\s\S]*operationalMergeDay[\s\S]*\$planMergeDay -ne \$MergeDay[\s\S]*\$operationalMergeDay -ne \$planMergeDay/,
        'bootstrap completion binds both clients to the requested authoritative merge day');
    assert.doesNotMatch(bootstrapOperational,
        /Get-OptionalProperty \$(?:host|join)(?:Commit|Operational)Applied '(?:handle|day|epoch|mergeDay)'|\$(?:host|join)(?:Commit|Operational)Applied\.(?:handle|day|epoch|mergeDay)/,
        'role-only bootstrap applied records cannot acquire synthetic handle/day/epoch fields');

    const prePairing = main.match(
        /\$joinLaunchUtc = \[DateTime\]::UtcNow([\s\S]*?)\$pairingResult = Run-Pairing/);
    assert.ok(prePairing,
        'the interval between join launch and literal pairing must remain auditable');
    assert.equal((prePairing[1].match(/Assert-OwnedProcessesLive \$hostProcess \$joinProcess/g) || []).length, 1,
        'the just-launched pair gets one PID-only liveness check');
    assert.match(prePairing[1],
        /Wait-FixedUtcAnchor \(\$joinLaunchUtc\.AddMilliseconds\(500\)\)[\s\S]*Assert-OwnedProcessesLive \$hostProcess \$joinProcess/,
        'the owned-PID diagnostic must retain the legacy join+500ms anchor');
    assert.doesNotMatch(prePairing[1],
        /Assert-ClientsLive|Assert-DebugRelayClientIdentity|Get-RoleState|Get-RelayState/,
        'DebugTest relay readiness must not precede either delayed-injection navigation anchor');
    const mainMenuAnchor = powerShellFunction(runner, 'Get-LiteralMainMenuAtLaunchAnchor');
    assert.match(mainMenuAnchor,
        /\$legacyInjectionUtc = \$LaunchUtc\.AddMilliseconds\(1500\)[\s\S]*\$targetUtc = \$legacyInjectionUtc\.AddMilliseconds\(11000\)[\s\S]*Wait-FixedUtcAnchor \$targetUtc/,
        'the source launcher\'s delayed injection and self-nav clocks must remain distinct');
    assert.match(popupFactory, /CompletedTick = \[long\]0[\s\S]*CompletedUtc = \$null/,
        'each independent popup window starts without a completion-clock fallback');
    assert.equal((updatePopupWindow.match(/\$Window\.CompletedUtc = \[DateTime\]::UtcNow/g) || []).length, 1,
        'the window projection publishes one completion timestamp at its actual terminal edge');
    assert.match(updatePopupWindow,
        /FirstActionTick[\s\S]*Kind -eq 'CLAIMED'[\s\S]*FirstActionTick = \[long\]\$firstClaim\[0\]\.Tick[\s\S]*ArmedTick \+[\s\S]*CapMilliseconds[\s\S]*FirstActionTick \+ 10000/,
        'the cap and ten-seconds-after-first-claim clocks remain separate native-tick projections');
    assert.doesNotMatch(persistentPopupTick,
        /Invoke-Button|Register-StartupModalAction|\/api\/ui\/invoke/,
        'the PowerShell popup service is a read-only exact-PID log observer');
    assert.equal((persistentPopupTick.match(/Read-ClientLogLines \(\[string\]\$PopupService\.ClientLog\)/g) || []).length, 1,
        'each popup service tick reads its exact PID log once');
    assert.match(persistentPopupTick,
        /\$PopupService\.SnapshotLines = \$snapshotLines[\s\S]*OBSERVED\|CLAIMED\|COMMITTED[\s\S]*MarkerLineCount/,
        'all complete native records are consumed monotonically from the exact-owned PID log');
    assert.match(persistentPopupTick,
        /State = 'OBSERVED'[\s\S]*'CLAIMED'[\s\S]*State = 'CLAIMED'[\s\S]*'COMMITTED'[\s\S]*State = 'COMMITTED'/,
        'each native appearance must prove its ordered OBSERVED -> CLAIMED -> COMMITTED chain');
    assert.match(persistentPopupTick,
        /\$kind -ne 'COMMITTED'[\s\S]*LastStartupPopupEvidenceTick/,
        'legacy quiet evidence advances on bind/pre-callback claim, never callback completion');
    assert.match(popupTick,
        /Invoke-LiteralPersistentStartupPopupTick \$Window\.PopupService[\s\S]*Update-LiteralStartupPopupWindow/,
        'a finite timer tick always services the session-long native subscriber first');
    assert.doesNotMatch(popupTick, /Invoke-Button|Start-Sleep|Wait-/,
        'the popup tick is a passive projection, not an action loop');
    assert.match(popupTimelineTick,
        /JoinEntryWindow\.CompletedTick \+ 5000[\s\S]*New-LiteralStartupPopupWindow\s+`\s*join 20000[\s\S]*HostWindow\.CompletedTick \+ 2000/,
        'the second join window and final host tail retain their exact independent anchors');

    const twoLaneWorkersAt = pairing.indexOf('$navigation = Complete-LiteralIndependentNavigation');
    assert.ok(twoLaneWorkersAt >= 0,
        'pairing must enter the physical two-lane navigation immediately');
    assert.doesNotMatch(pairing,
        /Get-TurnHistory|Get-UiHistory|Get-ClientLogMarkerCount|Read-ClientLogLines|Invoke-RestMethod|Get-Content/,
        'no HTTP or complete PID-log read may precede the two physical workers');
    assert.equal((hostLane.match(
        /-Role host -Dialog DLG_LOBBY -Button BTN_OK\b/g) || []).length, 1,
    'the host lane contains exactly one native lobby action site');
    assert.equal((joinLane.match(
        /-Role join -Dialog DLG_LOBBY -Button BTN_OK\b/g) || []).length, 1,
    'the join lane contains exactly one native lobby action site');
    const hostLobbyObservationAt = hostLane.indexOf(
        '$observation = Get-LiteralNavigationDialogObservation',
        hostLane.indexOf("$lane.Phase -eq 'lobby-dialog'"));
    const hostLobbyCompletedAt = hostLane.indexOf(
        '$lane.LobbyOk = Complete-LiteralButtonRequest $lane.Pending', hostLobbyObservationAt);
    assert.ok(hostLobbyObservationAt >= 0 && hostLobbyCompletedAt > hostLobbyObservationAt,
        'the sole host lobby dispatch interval must remain independently auditable');
    const hostLobbyDispatch = hostLane.slice(hostLobbyObservationAt, hostLobbyCompletedAt);
    assert.doesNotMatch(hostLobbyDispatch, /\b1500\b|Start-Sleep|Wait-FixedUtcAnchor/,
        'the host lobby action must not regain the removed 1500ms settle or any replacement delay');

    const joinOneSecondClocks = joinLane.match(/AddMilliseconds\(1000\)/g) || [];
    const joinTwentySecondWindows = joinLane.match(
        /New-LiteralStartupPopupWindow\s+`?\s*join\s+20000\b/g) || [];
    const timelineTwentySecondWindows = popupTimelineTick.match(
        /New-LiteralStartupPopupWindow\s+`?\s*join\s+20000\b/g) || [];
    assert.equal(joinOneSecondClocks.length, 2,
        'the join lane retains two distinct relative one-second clocks');
    assert.equal(joinTwentySecondWindows.length, 1,
        'the join lane creates exactly the first join 20-second popup window');
    assert.equal(timelineTwentySecondWindows.length, 1,
        'the timeline creates exactly the second join 20-second popup window after its independent +5000 anchor');
    assert.match(hostLane,
        /\$lane\.PopupService =\s*New-LiteralStartupPopupService host \$ClientLog[\s\S]*host 25000 \$lane\.PopupService/,
        'the physical host lane arms its exact 25-second append-only popup window');
    assert.match(joinLane,
        /join 20000 \$lane\.PopupService[\s\S]*Wait-LiteralStartupPopupWindow \$lane\.PopupWindow[\s\S]*if \(-not \[bool\]\$lane\.PopupWindow\.Done\)[\s\S]*\$lane\.Phase = 'done'/,
        'the physical join lane must finish and validate its first 20-second window before returning');
    assert.match(popupWait,
        /while \(-not \[bool\]\$Primary\.Done\)[\s\S]*Invoke-LiteralStartupPopupTick \$Primary[\s\S]*Start-Sleep -Milliseconds 100/,
        'the finite first-window wait drives the passive tick until Done');
    assert.match(workerStart,
        /InitialSessionState\]::CreateDefault2\(\)[\s\S]*RunspaceFactory\]::CreateRunspace\([\s\S]*\$powerShell = \[PowerShell\]::Create\(\)[\s\S]*\$async = \$powerShell\.BeginInvoke\(\)/,
        'each navigation role owns a real in-process runspace and asynchronous worker');
    assert.equal((workerStart.match(/\.BeginInvoke\(/g) || []).length, 1,
        'one worker descriptor starts exactly one physical invocation');
    assert.equal((workerComplete.match(/\.EndInvoke\(/g) || []).length, 1,
        'one worker descriptor is passively consumed exactly once');
    const hostWorkerAt = navigation.indexOf('$hostWorker = Start-LiteralNavigationWorker');
    const joinWorkerAt = navigation.indexOf('$joinWorker = Start-LiteralNavigationWorker');
    const releaseAt = navigation.indexOf('$startGate.Set()', joinWorkerAt);
    const passiveJoinAt = navigation.indexOf(
        'foreach ($worker in @($hostWorker, $joinWorker))');
    assert.ok(hostWorkerAt >= 0 && joinWorkerAt > hostWorkerAt &&
        releaseAt > joinWorkerAt && passiveJoinAt > releaseAt,
    'both physical lanes start behind one gate before release and passive result consumption');
    assert.match(hostLane,
        /\$StartGate\.Wait\(\)[\s\S]*\$laneStartedUtc = \[DateTime\]::UtcNow[\s\S]*\$deadlineUtc = \$laneStartedUtc\.AddSeconds\(\$TimeoutSec\)/,
        'host owns a full timeout budget after the common release');
    assert.match(joinLane,
        /\$StartGate\.Wait\(\)[\s\S]*\$laneStartedUtc = \[DateTime\]::UtcNow[\s\S]*\$deadlineUtc = \$laneStartedUtc\.AddSeconds\(\$TimeoutSec\)/,
        'join owns a full timeout budget after the common release');
    assert.doesNotMatch(navigation, /\$deadlineUtc\s*=|AddSeconds\(\$TimeoutSec\)/,
        'the parent cannot manufacture one shared absolute lane deadline');
    assert.doesNotMatch(navigation,
        /Get-DialogObservation|Get-LiteralClientLogMarkerEventUtcSnapshot|Invoke-RestMethod|Get-Content|Start-Sleep|while\s*\(/,
        'the outer join cannot synchronously read HTTP/log/UI state or run a cooperative pump');
    assert.match(workerComplete,
        /\[object\]::ReferenceEquals\([\s\S]*\$result\.OwnedProcess, \$Worker\.OwnedProcess\)/,
        'each result retains the exact Process object passed only to that role worker');
    assert.match(pairing,
        /\$hostPopupService = \$navigation\.HostPopupService[\s\S]*\$hostPopupWindow = \$navigation\.HostPopupWindow[\s\S]*\$startupWitness = \$navigation\.StartupWitness[\s\S]*\$joinPopupService = \$navigation\.JoinPopupService[\s\S]*\$joinEntryPopupWindow = \$navigation\.JoinPopupWindow[\s\S]*New-LiteralStartupPopupTimeline/,
        'pairing receives both running timers and the saved join-lane witness without replaying a gate');
    assert.match(pairing,
        /if \(-not \[bool\]\$joinEntryPopupWindow\.Done\)[\s\S]*New-LiteralStartupPopupTimeline/,
        'pairing accepts only an already-completed first join window');
    assert.doesNotMatch(pairing,
        /Wait-NewValidatedJoinStockStartupObserved|Wait-FixedUtcAnchorWithPopupService|Get-DialogObservation\s+join|Invoke-OneShotButton\s+`?\s*join\s+DLG_LOBBY|New-LiteralStartupPopupService\s+join|New-LiteralStartupPopupWindow\s+`?\s*join\s+20000/,
        'Run-Pairing cannot defer or replay any CJoin/Begin/+1000/+1000/OK/+500/first-window join step');
    assert.doesNotMatch(pairing, /-ObserveOnly|New-LiteralStartupPopupWindow\s+`?\s*join\s+5000\b/,
        'the source Delay(5000) cannot become an action blackout or a third AutoDismiss timer');
    assert.doesNotMatch(pairing, /Wait-NewValidatedJoinStockStartupComplete/,
        'MSS startup-completion evidence cannot delay the old popup/quiet trajectory');
    assert.equal((dayReady.match(/AddMilliseconds\(800\)/g) || []).length, 1,
        'wait-day-ready retains exactly one anchored 800ms sampling step');
    assert.match(dayReady,
        /Get-RoleState host[\s\S]*Get-RoleState join[\s\S]*while \([\s\S]*Invoke-LiteralStartupPopupTimelineTick[\s\S]*HostLogLines = @\(\$PopupTimeline\.HostWindow\.PopupService\.SnapshotLines\)[\s\S]*JoinLogLines = @\(\$PopupTimeline\.JoinPopupService\.SnapshotLines\)[\s\S]*Wait-FixedUtcAnchor \$nextObservationUtc/,
        'the 800ms observer retains the initial role reads and exact terminal PID-log snapshots');
    assert.doesNotMatch(dayReady,
        /Get-LiteralStartupMssWitnessSnapshot|Get-TurnHistory|Read-SimRelayEvents|Get-World|New-CanonicalWalkPreparation|Wait-FixedUtcAnchorWithPopupService/,
        'the quiet observer cannot absorb MSS event/world reads, map bindings, or a 100ms action loop');
    assert.equal((dayReady.match(/Get-RelayState/g) || []).length, 1,
        'the already-proven quiet edge retains one passive terminal role-state publication');
    assert.match(startClient, /'SCRIPTED_POPUPS'/,
        'both exact-owned clients enable the removable native popup subscriber before boot');
    assert.match(popupServiceFactory,
        /ClientLog[\s\S]*Get-ClientLogBaseline \$fullLogPath[\s\S]*MarkerLineCount = \[long\]0/,
        'each read-only service starts at the launch boundary of its exact PID log');
    assert.match(main,
        /Run-Pairing\s+`[\s\S]*?Wait-LiteralDayReady -QuietSec 3 -TimeoutSec 60\s+`[\s\S]*?-PopupTimeline \$pairingResult\.PopupTimeline[\s\S]*?-PairingResult \$pairingResult[\s\S]*?legacy startup PASS/,
        'the same session-long popup timeline flows from pairing into the literal quiet observer');
});

test('scripted-popup parser accepts absent optional captures only on the real native records', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const persistentPopupTick = powerShellFunction(
        runner, 'Invoke-LiteralPersistentStartupPopupTick');

    runPowerShellContract(`
Set-StrictMode -Version Latest
$script:StartupModalSettleMilliseconds = [long]300
$script:LastStartupPopupEvidenceTick = @{ host = [long]0; join = [long]0 }
$script:LastStartupPopupEvidenceUtc = @{ host = $null; join = $null }
$script:popupLines = @()
function Read-ClientLogLines([string]$Path) {
    return @($script:popupLines)
}
function New-TestPopupService {
    return [pscustomobject]@{
        Role = 'host'
        ClientLog = 'host.log'
        SnapshotLines = @()
        MarkerLineCount = [long]0
        LastMarkerTick = [long]0
        LastObservedAppearance = [long]0
        Appearances = @{}
    }
}
function Assert-PopupFailure([string[]]$Lines, [string]$ExpectedMessage) {
    $script:popupLines = @($Lines)
    $service = New-TestPopupService
    $rejected = $false
    try {
        [void](Invoke-LiteralPersistentStartupPopupTick $service)
    } catch {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*") {
            throw
        }
        $rejected = $true
    }
    if (-not $rejected) {
        throw "scripted-popup parser accepted an invalid native record"
    }
}
${persistentPopupTick}
$script:popupLines = @(
    '[testdrv][scripted-popup] OBSERVED role=host dialog=DLG_MESSAGE_BOX appearance=1 owner=777 button=BTN_OK tick=1000',
    '[testdrv][scripted-popup] CLAIMED role=host dialog=DLG_MESSAGE_BOX appearance=1 owner=777 button=BTN_YES bindAgeMs=300 tick=1300',
    '[testdrv][scripted-popup] COMMITTED role=host dialog=DLG_MESSAGE_BOX appearance=1 owner=777 button=BTN_YES tick=1301'
)
$service = New-TestPopupService
$events = @(Invoke-LiteralPersistentStartupPopupTick $service)
if ($events.Count -ne 3 -or
    $events[0].Kind -ne 'OBSERVED' -or $events[0].BindAgeMs -ne 0 -or
    $events[1].Kind -ne 'CLAIMED' -or $events[1].BindAgeMs -ne 300 -or
    $events[2].Kind -ne 'COMMITTED' -or $events[2].BindAgeMs -ne 0 -or
    $service.Appearances['1'].State -ne 'COMMITTED' -or
    $service.Appearances['1'].Button -ne 'BTN_YES' -or
    $service.MarkerLineCount -ne 3 -or $service.LastMarkerTick -ne 1301 -or
    $script:LastStartupPopupEvidenceTick.host -ne 1300) {
    throw 'valid native scripted-popup chain was not projected exactly'
}
Assert-PopupFailure @(
    '[testdrv][scripted-popup] CLAIMED role=host dialog=DLG_MESSAGE_BOX appearance=2 owner=777 button=BTN_OK tick=2000'
) 'lost the native 300 ms bind-age gate'
Assert-PopupFailure @(
    '[testdrv][scripted-popup] CLAIMED role=host dialog=DLG_MESSAGE_BOX appearance=2 owner=777 button=BTN_OK bindAgeMs=299 tick=2000'
) 'lost the native 300 ms bind-age gate'
Assert-PopupFailure @(
    '[testdrv][scripted-popup] CLAIMED role=host dialog=DLG_MESSAGE_BOX appearance=2 owner=777 button=BTN_OK bindAgeMs=invalid tick=2000'
) 'malformed native scripted-popup record'
Assert-PopupFailure @(
    '[testdrv][scripted-popup] OBSERVED role=host dialog=DLG_MESSAGE_BOX appearance=2 owner=777 button=BTN_OK bindAgeMs=300 tick=2000'
) 'unexpectedly contains bindAgeMs'
Assert-PopupFailure @(
    '[testdrv][scripted-popup] COMMITTED role=host dialog=DLG_MESSAGE_BOX appearance=2 owner=777 button=BTN_OK bindAgeMs=300 tick=2000'
) 'unexpectedly contains bindAgeMs'
'SCRIPTED_POPUP_PARSER_PASS'
`, 'SCRIPTED_POPUP_PARSER_PASS', 'strict scripted-popup parser contract');
});

test('literal join lane executes its exact anchors and completes the real first 20-second popup lifecycle', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const mainMenuAnchor = powerShellFunction(
        runner, 'Get-LiteralMainMenuAtLaunchAnchor');
    const optionalProperty = powerShellFunction(runner, 'Get-OptionalProperty');
    const fixedAnchor = powerShellFunction(runner, 'Wait-FixedUtcAnchor');
    const popupFactory = powerShellFunction(
        runner, 'New-LiteralStartupPopupWindow');
    const popupUpdate = powerShellFunction(
        runner, 'Update-LiteralStartupPopupWindow');
    const popupTick = powerShellFunction(
        runner, 'Invoke-LiteralStartupPopupTick');
    const popupWait = powerShellFunction(
        runner, 'Wait-LiteralStartupPopupWindow');
    const joinLane = powerShellFunction(
        runner, 'Invoke-LiteralJoinNavigationLane');

    const contract = [
        '$ErrorActionPreference = \'Stop\'',
        '$script:anchorTargets = [System.Collections.Generic.List[DateTime]]::new()',
        `function Wait-FixedUtcAnchor([DateTime]$TargetUtc) {
            [void]$script:anchorTargets.Add($TargetUtc)
        }`,
        optionalProperty,
        '$script:mainMenuRoleReads = 0',
        '$script:mainMenuObservationReads = 0',
        `function Get-LiteralNavigationRoleState {
            param([string]$Role, [string]$Context, [DateTime]$DeadlineUtc)
            $script:mainMenuRoleReads++
            if ($script:mainMenuRoleReads -le 2) {
                return [pscustomobject]@{
                    connected = $true; dialog = $null
                    dialogInstance = [long]0; dialogAppearance = [long]0
                    dialogReady = $false
                }
            }
            [pscustomobject]@{
                connected = $true; dialog = 'DLG_MAIN_MENU'
                dialogInstance = [long]1; dialogAppearance = [long]1
                dialogReady = $true
            }
        }`,
        `function Assert-DebugRelayClientIdentity {
            param([string]$Role, [object]$RoleState,
                  [System.Diagnostics.Process]$OwnedProcess)
        }`,
        `function ConvertTo-LiteralNavigationDialogObservation(
                  [string]$Role, [object]$State) {
            $script:mainMenuObservationReads++
            [pscustomobject]@{
                Ready = $true; Dialog = 'DLG_MAIN_MENU'; Instance = [long]1
                State = $State
            }
        }`,
        mainMenuAnchor,
        `$owned = Get-Process -Id $PID
        $launchUtc = [DateTime]::UtcNow.AddMinutes(-1)
        $anchorObservation = Get-LiteralMainMenuAtLaunchAnchor ` +
            `host $owned $launchUtc ([DateTime]::UtcNow.AddSeconds(2))
        $expectedAnchor = $launchUtc.AddMilliseconds(12500)
        if ($script:anchorTargets.Count -ne 1 -or
            $script:anchorTargets[0].Ticks -ne $expectedAnchor.Ticks -or
            $script:mainMenuRoleReads -ne 3 -or
            $script:mainMenuObservationReads -ne 1 -or
            $anchorObservation.Dialog -ne 'DLG_MAIN_MENU') {
            throw 'real main-menu function changed its anchor or passive first-publication wait'
        }`,
        '$script:popupMode = \'none\'',
        '$script:popupClaimEmitted = $false',
        '$script:popupClaimTick = [long]0',
        '$script:lanePopupTicks = 0',
        `function Invoke-LiteralPersistentStartupPopupTick([object]$PopupService) {
            if ($script:popupMode -eq 'lane') {
                $script:lanePopupTicks++
                return @()
            }
            if ($script:popupMode -eq 'claim' -and
                -not $script:popupClaimEmitted) {
                $script:popupClaimEmitted = $true
                return ,([pscustomobject]@{
                    Kind = 'CLAIMED'; Tick = [long]$script:popupClaimTick
                })
            }
            return @()
        }`,
        popupFactory,
        popupUpdate,
        popupTick,
        popupWait,
        `$popupService = [pscustomobject]@{ Role = 'join' }
        $capWindow = New-LiteralStartupPopupWindow join 20000 $popupService
        $capWindow.ArmedTick = [long][Environment]::TickCount64 - 20000
        Wait-LiteralStartupPopupWindow $capWindow
        if (-not [bool]$capWindow.Done -or
            [long]$capWindow.CompletedTick -ne
                ([long]$capWindow.ArmedTick + 20000)) {
            throw 'real finite window did not complete on its exact 20-second cap'
        }
        $claimWindow = New-LiteralStartupPopupWindow join 20000 $popupService
        $claimWindow.ArmedTick = [long][Environment]::TickCount64 - 12000
        $script:popupClaimTick = [long]$claimWindow.ArmedTick + 1000
        $script:popupMode = 'claim'
        Wait-LiteralStartupPopupWindow $claimWindow
        if (-not [bool]$claimWindow.Done -or
            [long]$claimWindow.FirstActionTick -ne $script:popupClaimTick -or
            [long]$claimWindow.CompletedTick -ne ($script:popupClaimTick + 10000)) {
            throw 'real finite window did not complete ten seconds after native claim'
        }`,
        fixedAnchor,
        '$script:dialogQueue = [System.Collections.Generic.Queue[string]]::new()',
        "[void]$script:dialogQueue.Enqueue('DLG_PROTOCOL')",
        "[void]$script:dialogQueue.Enqueue('DLG_LOAD_NEW_MULTI')",
        "[void]$script:dialogQueue.Enqueue('DLG_SESSION')",
        "[void]$script:dialogQueue.Enqueue('DLG_LOBBY')",
        '$script:dialogInstance = [long]10',
        '$script:buttonCounts = @{}',
        '$script:selectionCount = 0',
        '$script:turnBaselineCalls = 0',
        '$script:logBaselineCalls = 0',
        '$script:turnSubscriptionStarts = 0',
        `function Get-LiteralMainMenuAtLaunchAnchor {
            param([string]$Role, [System.Diagnostics.Process]$Process,
                  [DateTime]$LaunchUtc, [DateTime]$DeadlineUtc)
            [pscustomobject]@{ Ready = $true; Dialog = 'DLG_MAIN_MENU'; Instance = [long]1 }
        }`,
        `function Get-LiteralNavigationDialogObservation {
            param([string]$Role, [string]$Context, [DateTime]$DeadlineUtc)
            if ($script:dialogQueue.Count -eq 0) {
                throw 'real join lane requested an extra dialog observation'
            }
            $script:dialogInstance++
            [pscustomobject]@{
                Ready = $true
                Dialog = $script:dialogQueue.Dequeue()
                Instance = [long]$script:dialogInstance
            }
        }`,
        `function Start-LiteralButtonRequest {
            param([string]$Role, [string]$Dialog, [string]$Button,
                  [object]$CapturedObservation)
            if (-not $script:buttonCounts.ContainsKey($Button)) {
                $script:buttonCounts[$Button] = 0
            }
            $script:buttonCounts[$Button]++
            [pscustomobject]@{
                Role = $Role; Dialog = $Dialog; Button = $Button
                Observation = $CapturedObservation
                Task = [System.Threading.Tasks.Task]::CompletedTask
                Completed = $false
                StartedUtc = [DateTime]::UtcNow
            }
        }`,
        `function Complete-LiteralButtonRequest([object]$Pending) {
            if ([bool]$Pending.Completed) { throw 'button completion consumed twice' }
            $Pending.Completed = $true
            [pscustomobject]@{
                Role = $Pending.Role; Dialog = $Pending.Dialog
                Button = $Pending.Button; CompletedUtc = [DateTime]::UtcNow
            }
        }`,
        `function Start-LiteralSelectionRequest {
            param([string]$Kind, [string]$Role, [string]$Dialog,
                  [string]$ListBox, [int]$Index, [int]$ExpectedTotal,
                  [string]$ExactPath, [object]$CapturedObservation)
            $script:selectionCount++
            [pscustomobject]@{
                Observation = $CapturedObservation
                Task = [System.Threading.Tasks.Task]::CompletedTask
                Completed = $false
            }
        }`,
        `function Complete-LiteralSelectionRequest([object]$Pending) {
            if ([bool]$Pending.Completed) { throw 'selection completion consumed twice' }
            $Pending.Completed = $true
            return $Pending.Observation
        }`,
        `function Get-TurnHistory {
            if (-not $script:testStartGate.IsSet) {
                throw 'join turn baseline ran before the common worker release'
            }
            $script:turnBaselineCalls++
            [pscustomobject]@{ latestSeq = [long]11 }
        }`,
        `function Get-EvidenceWatermark([object]$History, [string]$Label) {
            return [long]$History.latestSeq
        }`,
        `function Get-ClientLogMarkerCount([string]$Path, [string]$Marker) {
            if (-not $script:testStartGate.IsSet) {
                throw 'join log baseline ran before the common worker release'
            }
            $script:logBaselineCalls++
            return 0
        }`,
        `function Start-LiteralTurnHistoryRequest {
            param([long]$After, [string]$Role, [int]$WaitMilliseconds)
            $script:turnSubscriptionStarts++
            [pscustomobject]@{
                After = $After; Task = [System.Threading.Tasks.Task]::CompletedTask
                Completed = $false
            }
        }`,
        `function Complete-LiteralTurnHistoryRequest([object]$Pending) {
            if ([bool]$Pending.Completed) { throw 'turn completion consumed twice' }
            $Pending.Completed = $true
            [pscustomobject]@{ latestSeq = [long]13; events = @([pscustomobject]@{ seq = 13 }) }
        }`,
        `function Add-LiteralJoinStockStartupHistory([object]$State, [object]$History) {
            $briefing = [pscustomobject]@{ kind = 'stock-startup-join-game-observed'; seq = [long]12 }
            $strategic = [pscustomobject]@{ kind = 'stock-startup-begin-turn-observed'; seq = [long]13 }
            $State.Cursor = [long]13
            $State.ObservedEvents = @($briefing, $strategic)
            $State.BriefingLatch = $briefing
            $State.StrategicLatch = $strategic
            $State.Witness = [pscustomobject]@{
                BeginTurn = $strategic; JoinGame = $briefing; LatestSequence = [long]13
            }
            return $State.Witness
        }`,
        `function New-LiteralStartupPopupService([string]$Role, [string]$ClientLog) {
            [pscustomobject]@{ Role = $Role; ClientLog = $ClientLog }
        }`,
        joinLane,
        `$script:testStartGate = [System.Threading.ManualResetEventSlim]::new($false)
        $script:testStartGate.Set()
        $script:popupMode = 'lane'
        $laneResult = Invoke-LiteralJoinNavigationLane -OwnedProcess $owned \`
            -LaunchUtc ([DateTime]::UtcNow) -ClientLog 'join.log' \`
            -ExpectedInitialHostHandle 0 -TimeoutSec 45 \`
            -StartGate $script:testStartGate
        $script:testStartGate.Dispose()
        if (($laneResult.DeadlineUtc - $laneResult.LaneStartedUtc).TotalSeconds -ne 45) {
            throw 'real join lane did not own its complete deadline budget'
        }
        if ($script:turnBaselineCalls -ne 1 -or
            $script:logBaselineCalls -ne 1 -or
            $script:turnSubscriptionStarts -ne 1 -or
            [long]$laneResult.StartupTurnWatermark -ne 11 -or
            [int]$laneResult.SessionListReadyBefore -ne 0) {
            throw 'real join lane changed its one released baseline/subscription lifecycle'
        }
        foreach ($button in @('BTN_MULTI', 'BTN_CONTINUE', 'BTN_JOIN',
                               'BTN_JOIN_GAME', 'BTN_OK')) {
            if ([int]$script:buttonCounts[$button] -ne 1) {
                throw "real join lane dispatched $button $($script:buttonCounts[$button]) times"
            }
        }
        if ($script:selectionCount -ne 1 -or
            -not [bool]$laneResult.PopupWindow.Done -or
            [long]$laneResult.PopupWindow.FirstActionTick -ne 0 -or
            ([long]$laneResult.PopupWindow.CompletedTick -
             [long]$laneResult.PopupWindow.ArmedTick) -ne 20000 -or
            # The production loop sleeps at most 100 ms, but spawning a fresh
            # PowerShell contract under CI can add scheduler overhead to every
            # sample.  One hundred samples still proves sustained passive
            # polling across the exact 20-second window without a timing flake.
            $script:lanePopupTicks -lt 100) {
            throw ("real join lane did not execute the complete passive 20-second first window " +
                "(selection=$script:selectionCount done=$($laneResult.PopupWindow.Done) " +
                "first=$($laneResult.PopupWindow.FirstActionTick) " +
                "duration=$([long]$laneResult.PopupWindow.CompletedTick - [long]$laneResult.PopupWindow.ArmedTick) " +
                "ticks=$script:lanePopupTicks)")
        }
        if (($laneResult.JoinGameArmUtc - $laneResult.Search.CompletedUtc).TotalMilliseconds -ne 2000 -or
            ($laneResult.FirstSettleArmUtc - $laneResult.HostStrategicReleasedUtc).TotalMilliseconds -ne 1000 -or
            ($laneResult.PopupArmUtc - $laneResult.LobbyOk.CompletedUtc).TotalMilliseconds -ne 500) {
            throw 'real join lane changed +2000/+1000/+500 exact anchors'
        }
        $secondClockSpacing =
            ($laneResult.SecondSettleArmUtc - $laneResult.FirstSettleArmUtc).TotalMilliseconds
        if ($secondClockSpacing -lt 900 -or $secondClockSpacing -gt 1500) {
            throw "real second relative +1000 clock spacing is $secondClockSpacing ms"
        }
        'REAL_LITERAL_JOIN_LANE_PASS'`,
    ].join('\n');

    runPowerShellContract(
        contract,
        'REAL_LITERAL_JOIN_LANE_PASS',
        'real literal join-lane/anchor/20-second lifecycle contract',
        60000);
});

test('runtime requires one first-leader Send completion in each owned client log', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const quiet = powerShellFunction(runner, 'Wait-LiteralDayReady');
    const snapshot = powerShellFunction(runner, 'Get-LiteralStartupMssWitnessSnapshot');
    const proof = powerShellFunction(runner, 'Assert-LiteralStartupMssWitness');
    assert.match(proof,
        /bootstrap first-leader-name TX sent \(role=/,
        'the proof must count only successful post-Send leader-name markers');
    assert.match(quiet,
        /HostLogLines = @\(\$PopupTimeline\.HostWindow\.PopupService\.SnapshotLines\)[\s\S]*JoinLogLines = @\(\$PopupTimeline\.JoinPopupService\.SnapshotLines\)/,
        'the terminal quiet tick retains both already-read exact-owned PID logs');
    assert.match(snapshot,
        /HostLogLines = @\(\$LegacyQuietWitness\.HostLogLines\)[\s\S]*JoinLogLines = @\(\$LegacyQuietWitness\.JoinLogLines\)/,
        'the deferred MSS event snapshot copies the terminal logs without rereading either PID file');
    assert.match(proof,
        /Measure-LiteralSavedLogMarker \$hostLines \$leaderMarker[\s\S]*Measure-LiteralSavedLogMarker \$joinLines \$leaderMarker/,
        'both saved PID-log witnesses must be counted independently');
    assert.match(proof,
        /\$hostLeaderCount -ne 1 -or \$joinLeaderCount -ne 1/,
        'zero or duplicate successful leader-name Sends on either role must fail the run');
    assert.doesNotMatch(proof, /Read-ClientLogLines|Get-ClientLogMarkerCount|Wait-|Start-Sleep/,
        'leader TX verification after quiet PASS is pure over the saved terminal tick');
});

test('native startup leader rename remains one exact post-TurnInfo strategic TX', () => {
    const controller = fs.readFileSync(nativeSimturnController, 'utf8');
    const strategic = controller.match(
        /constexpr ExactRtti strategicIntentRtti\[\][\s\S]*?\n};/);
    assert.ok(strategic, 'the exact strategic-intent map must remain identifiable');
    assert.match(strategic[0],
        /D2_EXACT_RTTI\("\.\?AVCStackChangeLeaderNameMsg@@"\)/,
        'leader rename must remain a strategic mutation outside its startup exception');

    const decoder = controller.match(
        /bool decodeStartupLeaderNameIntent[\s\S]*?(?=\nconstexpr TurnAnnouncementLayout)/);
    assert.ok(decoder, 'the startup leader-name wire decoder must remain identifiable');
    assert.match(decoder[0],
        /sizeof\(startupLeaderNameRtti\)[\s\S]*netMessageClassNameSize[\s\S]*payload\[offset\] != 0/,
        'the four post-RTTI class-name bytes must stay zero');
    assert.match(decoder[0],
        /payloadSize != startupLeaderNameBytesOffset \+ nameByteCount/,
        'the variable leader name must consume the exact remaining frame');
    assert.match(decoder[0],
        /name\[nameByteCount - 1\] != 0[\s\S]*std::memchr\(name, 0, nameByteCount - 1\) != nullptr/,
        'only the final byte of the serialized leader name may be NUL');

    const consumer = controller.match(
        /bool consumeStartupLeaderNameIntent[\s\S]*?(?=\nstd::uintptr_t setScenarioDayCallback)/);
    assert.ok(consumer, 'the startup leader-name consumer must remain identifiable');
    assert.match(consumer[0],
        /clientObjectMap\(\)[\s\S]*getScenarioInfo\(objectMap\)[\s\S]*getStack\(objectMap, &stackId\)/,
        'the exception must resolve the serialized stack against the live client map');
    assert.match(consumer[0],
        /scenarioInfo->currentTurn != 1[\s\S]*stack->ownerId\.value[\s\S]*publishedLocalHandle[\s\S]*!stack->leaderAlive[\s\S]*!stack->leaderId\.value/,
        'the resolved object must be the live local leader on stock day one');
    assert.match(consumer[0],
        /StartupLeaderNameTx expected = StartupLeaderNameTx::Expected;[\s\S]*compare_exchange_strong\([\s\S]*expected, StartupLeaderNameTx::InFlight/,
        'the one-shot claim must atomically advance Expected to InFlight');
    assert.match(consumer[0],
        /netintercept::armCurrentTxCompletion\([\s\S]*&completeStartupLeaderNameAfterSend/,
        'the natural transport send must have an exact post-Send completion');

    const completion = controller.match(
        /void completeStartupLeaderNameAfterSend[\s\S]*?(?=\nvoid completeEndTurnAfterSend)/);
    assert.ok(completion, 'the startup leader-name Send completion must remain identifiable');
    assert.match(completion[0],
        /if \(sendResult == 0\)[\s\S]*fault\("natural startup leader-name transport Send failed"\)/,
        'a failed natural transport send must fault instead of publishing success');

    assert.match(controller,
        /void completeHostStartupTurnInfoAfterDispatch[\s\S]*armStartupLeaderNameFromOwnTurnInfo\(activeHandle\)/,
        'host may arm the startup rename only after its own TurnInfo dispatch');
    assert.match(controller,
        /void completeJoinBootstrapTurnInfoAfterDispatch[\s\S]*armStartupLeaderNameFromOwnTurnInfo\(activeHandle\)/,
        'join may arm the startup rename only after its own TurnInfo dispatch');
    assert.match(controller,
        /armCurrentRxCompletion\([\s\S]*&completeHostStartupTurnInfoAfterDispatch, activeHandle\)/,
        'host own-TurnInfo must use the post-dispatch completion seam');
    assert.match(controller,
        /armCurrentRxCompletion\([\s\S]*&completeJoinBootstrapTurnInfoAfterDispatch, activeHandle\)/,
        'join own-TurnInfo must use the post-dispatch completion seam');

    const ownTurnInfoRoute = controller.match(
        /bool isExactOwnTurnInfoRoute[\s\S]*?(?=\nbool isExactPreBindHostTurnInfoActive)/);
    assert.ok(ownTurnInfoRoute,
        'the exact bound/pre-bind own-TurnInfo route must remain identifiable');
    assert.match(ownTurnInfoRoute[0],
        /publishedLocalHandle && activeHandle != publishedLocalHandle/,
        'a missing pre-strategic local handle must not reject the exact host route');
    assert.match(ownTurnInfoRoute[0],
        /BroadcastLatched[\s\S]*serverNetPlayerId[\s\S]*idFrom[\s\S]*playerNetId[\s\S]*activeHandle/,
        'pre-bind host TurnInfo must match the complete latched BeginTurn tuple');

    const preBindActive = controller.match(
        /bool isExactPreBindHostTurnInfoActive[\s\S]*?(?=\nbool armStartupLeaderNameFromOwnTurnInfo)/);
    assert.ok(preBindActive,
        'the post-dispatch pre-bind active-handle proof must remain identifiable');
    assert.match(preBindActive[0],
        /!isHost\(\)[\s\S]*localHandle\(\)[\s\S]*g_authoritativeSenderDpid/,
        'only an unbound host may consume the pre-bind active-handle proof');

    const leaderNameArm = controller.match(
        /bool armStartupLeaderNameFromOwnTurnInfo[\s\S]*?(?=\nvoid completeHostStartupTurnInfoAfterDispatch)/);
    assert.ok(leaderNameArm,
        'the post-TurnInfo startup leader-name claim must remain identifiable');
    assert.match(leaderNameArm[0],
        /isExactPreBindHostTurnInfoActive[\s\S]*!exactActiveHandle && !publishedLocalHandle[\s\S]*publishedLocalHandle = localHandle\(\)/,
        'a concurrent typed-handle publication must resolve the pre-bind proof race');
    assert.match(leaderNameArm[0],
        /g_startupLeaderNameExpectedHandle\.compare_exchange_strong[\s\S]*committedLocalHandle = localHandle\(\)[\s\S]*committedLocalHandle != activeHandle/,
        'the committed claim must revalidate a concurrently published local handle');

    const hostTurnInfoWindow = controller.match(
        /const bool hostStartupTurnInfoWindow[\s\S]*?(?=\n\s*if \(isHost\(\)[\s\S]*&& endTurn\))/);
    assert.ok(hostTurnInfoWindow,
        'the host startup TurnInfo RX window must remain identifiable');
    assert.match(hostTurnInfoWindow[0],
        /exactOwnRoute[\s\S]*publishedLocalHandle[\s\S]*activeHandle/,
        'the exact pre-bind route must reach the sole post-dispatch completion arm');

    assert.match(consumer[0],
        /g_startupLeaderNameExpectedHandle[\s\S]*expectedActiveHandle != publishedLocalHandle/,
        'the later natural TX must bind the pre-bind TurnInfo claim to the published local handle');
    assert.match(controller,
        /publishLocalHandle\(handle\)[\s\S]*expectedStartupLeaderHandle[\s\S]*expectedStartupLeaderHandle != handle/,
        'strategic startup must reject a pre-bind TurnInfo/local-handle disagreement');
});

test('join auto-mode turn filter drops only host ownership, passes CRefreshInfo, and never reinjects', () => {
    const controller = fs.readFileSync(nativeSimturnController, 'utf8');
    const stateMutations = controller.match(
        /constexpr ExactRtti authoritativeStateMutationRtti\[\][\s\S]*?\n};/);
    const uiReplay = controller.match(
        /constexpr ExactRtti requiresUiReplayRtti\[\][\s\S]*?\n};/);
    assert.ok(stateMutations && uiReplay,
        'the fail-closed mutation set and narrow UI-replay set must remain independently auditable');
    assert.match(stateMutations[0], /D2_EXACT_RTTI\("\.\?AVCRefreshInfo@@"\)/,
        'CRefreshInfo remains a state mutation for the terminal Faulted gate');
    assert.doesNotMatch(uiReplay[0], /CRefreshInfo/,
        'CRefreshInfo must not be deferred or replayed by the narrow worker-to-UI seam');
    const rxGate = controller.match(
        /^netintercept::RxDecision rxGate\([\s\S]*?(?=\nnetintercept::TxDecision txGate)/m);
    assert.ok(rxGate, 'the production RX gate must remain independently auditable');
    const filterStart = rxGate[0].indexOf('const bool joinFilterActive');
    const filterEnd = rxGate[0].indexOf('if (!requiresUiReplay', filterStart);
    assert.ok(filterStart >= 0 && filterEnd > filterStart,
        'the one-shot join turn filter must remain an isolated branch');
    const filter = rxGate[0].slice(filterStart, filterEnd);
    assert.match(filter,
        /!isHost\(\)[\s\S]*Phase::Independent[\s\S]*Phase::Held[\s\S]*Phase::Merging[\s\S]*Phase::AwaitingStockTurn[\s\S]*Phase::Faulted/,
        'auto=1-equivalent mode keeps the join filter active throughout overlay ownership');
    assert.match(filter,
        /turnAnnouncement = beginTurn \|\| turnInfo[\s\S]*if \(beginTurn\)[\s\S]*decodeTurnAnnouncement[\s\S]*else \{[\s\S]*decodeTurnInfoActive/,
        'BeginTurn and TurnInfo must derive the same exact active-handle decision');
    assert.match(filter,
        /if \(!activeHandle\)[\s\S]*RxDecision::Drop[\s\S]*if \(current == Phase::Faulted\)[\s\S]*RxDecision::Drop[\s\S]*if \(activeHandle == hostHandle\(\)\)[\s\S]*RxDecision::Consume/,
        'invalid/faulted announcements fail; intentional host-ownership filtering retires its native receipt without stealing join UI');
    assert.doesNotMatch(filter,
        /activeHandle == localHandle\(\)[\s\S]*RxDecision::Drop/,
        'the joiner\'s own decoded handle must not be dropped');
    assert.match(rxGate[0].slice(filterEnd),
        /if \(!requiresUiReplay[\s\S]*RxDecision::Pass[\s\S]*RxDecision::Defer[\s\S]*RxDecision::Pass/,
        'an own-handle announcement follows the ordinary pass/defer-once/pass route');
    assert.doesNotMatch(filter,
        /injectBeginTurn|sendBootstrap|sendSubjective|sendTurn|invokeOnUiThread|postPipeEvent|refire|retry|fallback/i,
        'the filter is purely accept/drop and cannot synthesize or resubmit a turn event');
    const faultedMutationAt = rxGate[0].indexOf('if ((current == Phase::Faulted || current == Phase::Closing) && stateMutation)');
    const ordinaryPassAt = rxGate[0].indexOf('if (!requiresUiReplay', filterEnd);
    assert.ok(faultedMutationAt >= 0 && faultedMutationAt < filterStart && ordinaryPassAt === filterEnd,
        'CRefreshInfo is fail-closed in Faulted/Closing and otherwise reaches the ordinary non-replayed Pass');
});

test('DirectPlay enumeration proof follows the literal late-bind action', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const nativeNettrace = fs.readFileSync(nativeNettraceScript, 'utf8');
    const connectFixture = JSON.parse(
        fs.readFileSync(stockStartupCConnectFixture, 'utf8'));
    const marker = '[nettrace] EnumSessions ready on next natural UI frame generation=';
    assert.match(nativeNettrace,
        /\[nettrace\] EnumSessions ready on next natural UI frame generation=\{\}/);
    assert.equal(connectFixture.provenance.sourceDeclaredFrameSize, 48);
    assert.equal(connectFixture.layout.payloadSize,
        connectFixture.provenance.sourceDeclaredFrameSize - 8);
    const connectPayloads = connectFixture.orderedPayloads.map((record) => {
        const payload = Buffer.from(record.payloadHex.replaceAll(' ', ''), 'hex');
        assert.equal(payload.readUInt32LE(connectFixture.layout.dpidOffset),
            Number.parseInt(record.dpid, 16));
        return payload;
    });
    assert.equal(connectPayloads.length, 2);
    for (const payload of connectPayloads) {
        assert.equal(payload.length, 40);
        assert.equal(payload.subarray(0, 18).toString('ascii'), '.?AVCConnectMsg@@\0');
        assert.ok(payload.readUInt32LE(connectFixture.layout.dpidOffset) > 1);
    }
    assert.notEqual(
        connectPayloads[0].readUInt32LE(connectFixture.layout.dpidOffset),
        connectPayloads[1].readUInt32LE(connectFixture.layout.dpidOffset));
    assert.match(nativeNettrace,
        /connectDpidOffset = 36;[\s\S]*connectMinimumPayloadSize =[\s\S]*connectDpidOffset \+ sizeof\(std::uint32_t\);[\s\S]*static_assert\(connectMinimumPayloadSize == 40\);[\s\S]*if \(dpid <= 1\)[\s\S]*return;/,
        'the mandatory witness must translate the green full-frame guard to the exact 40-byte bounded body and retain dynamic-DPID admission');
    assert.doesNotMatch(nativeNettrace, /CConnectMsg carried a zero DPID/,
        'server/zero CConnect announcements were ignored by the literal green gate');
    const runtimeObserverBundle = nativeNettrace.match(
        /netintercept::ObserverBundle runtimeObserverBundle[\s\S]*?(?=\nbool registerRuntimeObservers)/);
    const nettracePreflight = nativeNettrace.match(
        /bool preflight\(bool enablePacketLogging\)[\s\S]*?(?=\nbool commit)/);
    const nettraceCommit = nativeNettrace.match(
        /bool commit\(bool enablePacketLogging\)[\s\S]*?(?=\nbool install)/);
    assert.ok(runtimeObserverBundle && nettracePreflight && nettraceCommit,
        'the mandatory WaitPeer observer lifecycle must remain independently auditable');
    assert.match(runtimeObserverBundle[0],
        /bundle\.rx = &traceRx;[\s\S]*if \(enablePacketLogging\)[\s\S]*bundle\.tx = &traceTx;/,
        'CConnect RX observation is mandatory while verbose TX tracing stays optional');
    assert.match(nettracePreflight[0],
        /canAddObservers\(runtimeObserverBundle\(enablePacketLogging\)\)/,
        'preflight must reserve the mandatory RX observer even with packet logging off');
    assert.doesNotMatch(nettracePreflight[0],
        /enablePacketLogging\s*&&\s*!netintercept::canAddObservers/,
        'WaitPeer observer capacity must never depend on verbose packet logging');
    assert.match(nettraceCommit[0],
        /if \(!registerRuntimeObservers\(enablePacketLogging\)\)/,
        'commit must always register the WaitPeer observer bundle');
    assert.doesNotMatch(nettraceCommit[0],
        /enablePacketLogging\s*&&\s*!registerRuntimeObservers/,
        'packet-log=off must not disconnect the literal WaitPeer witness');
    const escaped = marker.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    assert.equal((runner.match(new RegExp(escaped, 'g')) || []).length, 2,
        'static contract and runtime watcher must both use the exact native marker');
    const pairing = powerShellFunction(runner, 'Run-Pairing');
    const navigation = powerShellFunction(
        runner, 'Complete-LiteralIndependentNavigation');
    const hostLane = powerShellFunction(
        runner, 'Invoke-LiteralHostNavigationLane');
    const joinLane = powerShellFunction(
        runner, 'Invoke-LiteralJoinNavigationLane');
    const workerStart = powerShellFunction(
        runner, 'Start-LiteralNavigationWorker');
    const workerComplete = powerShellFunction(
        runner, 'Complete-LiteralNavigationWorker');
    const buttonRequest = powerShellFunction(
        runner, 'Start-LiteralButtonRequest');
    const mainMenuAnchor = powerShellFunction(
        runner, 'Get-LiteralMainMenuAtLaunchAnchor');
    const turnSubscriptionStart = powerShellFunction(
        runner, 'Start-LiteralTurnHistoryRequest');
    const turnSubscriptionComplete = powerShellFunction(
        runner, 'Complete-LiteralTurnHistoryRequest');
    const stockHistoryFold = powerShellFunction(
        runner, 'Add-LiteralJoinStockStartupHistory');
    const savedWitness = powerShellFunction(runner, 'Assert-LiteralStartupMssWitness');
    const sessionBaselineAt = joinLane.indexOf(
        'Get-ClientLogMarkerCount $ClientLog $sessionListReadyMarker');
    const joinStartGateAt = joinLane.indexOf('$StartGate.Wait()');
    const joinAnchorAt = joinLane.indexOf('Get-LiteralMainMenuAtLaunchAnchor');
    assert.ok(joinStartGateAt >= 0 && sessionBaselineAt > joinStartGateAt &&
        joinAnchorAt > sessionBaselineAt,
        'the join worker owns its EnumSessions baseline only after both lanes are released');
    assert.doesNotMatch(pairing,
        /Get-TurnHistory|Get-UiHistory|Get-ClientLogMarkerCount|Read-ClientLogLines|Invoke-RestMethod|Get-Content/,
        'Run-Pairing has no serial HTTP/full-log pre-pump');
    assert.match(mainMenuAnchor,
        /\$legacyInjectionUtc = \$LaunchUtc\.AddMilliseconds\(1500\)[\s\S]*\$targetUtc = \$legacyInjectionUtc\.AddMilliseconds\(11000\)[\s\S]*Wait-FixedUtcAnchor \$targetUtc/,
        'each physical worker consumes its own launch+1500+11000 clock');
    assert.equal((hostLane.match(
        /Get-LiteralMainMenuAtLaunchAnchor\s+`?\s*host \$OwnedProcess \$LaunchUtc/g) || []).length, 1,
    'host has one exact-owned delayed-injection anchor');
    assert.equal((joinLane.match(
        /Get-LiteralMainMenuAtLaunchAnchor\s+`?\s*join \$OwnedProcess \$LaunchUtc/g) || []).length, 1,
    'join has one exact-owned delayed-injection anchor');
    assert.equal((navigation.match(/Start-LiteralNavigationWorker/g) || []).length, 2,
        'the outer path starts exactly two physical self-nav workers');
    assert.equal((buttonRequest.match(/\.SendAsync\(/g) || []).length, 1,
        'every physical navigation button path owns one HTTP publication');
    assert.match(buttonRequest,
        /\$readyOwnerIntent\s*=\s*\$Dialog -eq 'DLG_LOBBY' -and \$Button -eq 'BTN_OK'[\s\S]*if \(\$readyOwnerIntent\)[\s\S]*\$afterUiSequence = \[long\]\$sequenceValue - 1[\s\S]*\$stabilityQuery = if \(\$Role -eq 'host'\)[\s\S]*'&stableMs=500'[\s\S]*\/api\/ui\/invoke-when-ready[\s\S]*\} else \{[\s\S]*\$candidate = \$CapturedObservation/,
        'volatile lobby OK owns one current-or-later intent, with host-only 500ms stability and no second owner read');
    assert.doesNotMatch(buttonRequest,
        /Get-DialogObservation|Get-LiteralNavigationRoleState|Get-RoleState/,
        'the sole button POST reuses its captured predicate instead of reopening a TOCTOU window');
    assert.match(navigation,
        /\$startGate = \[System\.Threading\.ManualResetEventSlim\]::new\(\$false\)[\s\S]*\$hostWorker = Start-LiteralNavigationWorker[\s\S]*\$joinWorker = Start-LiteralNavigationWorker[\s\S]*\$startGate\.Set\(\)[\s\S]*foreach \(\$worker in @\(\$hostWorker, \$joinWorker\)\)/,
        'both complete role workers are waiting before their shared release and first passive result join');
    assert.match(workerStart,
        /CreateDefault2\(\)[\s\S]*CreateRunspace\([\s\S]*\[PowerShell\]::Create\(\)[\s\S]*\.BeginInvoke\(\)/,
        'each lane is physically scheduled in a separate runspace');
    assert.match(workerStart,
        /\[Parameter\(Mandatory\)\]\[ValidateNotNull\(\)\]\[object\]\$StartupObserver/,
        'the worker factory requires the startup observer as an explicit dependency');
    assert.doesNotMatch(workerStart, /\$script:LiteralInnerStartupObserver/,
        'the worker factory cannot capture the caller script scope implicitly');
    const startupObserverSeedAt = workerStart.indexOf(
        "@{ Name = 'LiteralInnerStartupObserver'; Value = $StartupObserver }");
    const createRunspaceAt = workerStart.indexOf('RunspaceFactory]::CreateRunspace(');
    const beginWorkerAt = workerStart.indexOf('$powerShell.BeginInvoke()');
    assert.ok(startupObserverSeedAt >= 0 && createRunspaceAt > startupObserverSeedAt &&
        beginWorkerAt > createRunspaceAt,
    'the explicit startup observer must be seeded before runspace creation and BeginInvoke');
    assert.equal((workerComplete.match(/\.EndInvoke\(/g) || []).length, 1);
    const navigationDependencyStubs = [
        'Get-RelayState',
        'Get-RoleState',
        'Test-DialogReady',
        'Wait-Dialog',
        'Get-OptionalProperty',
        'Get-DialogObservation',
        'Test-LiteralNavigationPassiveTimeoutException',
        'Get-LiteralNavigationRoleState',
        'Get-LiteralNavigationRoleStateProjection',
        'ConvertTo-LiteralNavigationUInt32',
        'ConvertTo-LiteralNavigationDialogObservation',
        'Get-LiteralNavigationDialogObservation',
        'Get-ReadyActionTarget',
        'Assert-ReadyButtonSnapshot',
        'Get-ReadyListBoxState',
        'Read-SimRelayEvents',
        'Assert-NoRelayFault',
        'Start-LiteralSelectionRequest',
        'Complete-LiteralSelectionRequest',
        'Start-LiteralButtonRequest',
        'Complete-LiteralButtonRequest',
        'Wait-FixedUtcAnchor',
        'Get-LiteralMainMenuAtLaunchAnchor',
        'Assert-DebugRelayClientIdentity',
        'Get-ClientLogBaseline',
        'Read-ClientLogLines',
        'Get-ClientLogMarkerCount',
        'Get-LiteralClientLogMarkerEventUtcSnapshot',
        'New-LiteralStartupPopupService',
        'New-LiteralStartupPopupWindow',
        'Invoke-LiteralPersistentStartupPopupTick',
        'Update-LiteralStartupPopupWindow',
        'Invoke-LiteralStartupPopupTick',
        'Wait-LiteralStartupPopupWindow',
        'Get-TurnHistory',
        'Start-LiteralTurnHistoryRequest',
        'Complete-LiteralTurnHistoryRequest',
        'Add-LiteralJoinStockStartupHistory',
        'Get-EvidenceWatermark',
        'Get-RequiredTelemetryNumber',
    ].map((name) => `function ${name} {}`).join('\n');
    const delayedLaneSchedulerScript = [
        '$ErrorActionPreference = \'Stop\'',
        navigationDependencyStubs,
        `function Assert-LiteralInnerStartupObserverHealthy {
            param([Parameter(Mandatory)][object]$Observer)
            if ([string]$Observer.Token -ne 'literal-inner-startup-observer') {
                throw 'navigation runspace received the wrong startup observer'
            }
        }
        function Assert-ProductionRelayHealthy {
            Assert-LiteralInnerStartupObserverHealthy $script:LiteralInnerStartupObserver
        }`,
        `function Invoke-LiteralHostNavigationLane {
            param(
                [System.Diagnostics.Process]$OwnedProcess,
                [DateTime]$LaunchUtc,
                [string]$ClientLog,
                [int]$TimeoutSec,
                [System.Threading.ManualResetEventSlim]$StartGate,
                [string]$WaitPeerMarker,
                [string]$ExactScenarioPath,
                [int]$ExactScenarioIndex,
                [int]$ScenarioIndex)
            $StartGate.Wait()
            Assert-ProductionRelayHealthy
            $startedUtc = [DateTime]::UtcNow
            $deadlineUtc = $startedUtc.AddSeconds($TimeoutSec)
            Start-Sleep -Milliseconds 1200
            [pscustomobject]@{
                Role = 'host'; OwnedProcess = $OwnedProcess
                LaneStartedUtc = $startedUtc; DeadlineUtc = $deadlineUtc
            }
        }`,
        `function Invoke-LiteralJoinNavigationLane {
            param(
                [System.Diagnostics.Process]$OwnedProcess,
                [DateTime]$LaunchUtc,
                [string]$ClientLog,
                [int]$TimeoutSec,
                [System.Threading.ManualResetEventSlim]$StartGate,
                [long]$ExpectedInitialHostHandle)
            $StartGate.Wait()
            Assert-ProductionRelayHealthy
            $startedUtc = [DateTime]::UtcNow
            $deadlineUtc = $startedUtc.AddSeconds($TimeoutSec)
            Start-Sleep -Milliseconds 100
            [pscustomobject]@{
                Role = 'join'; OwnedProcess = $OwnedProcess
                LaneStartedUtc = $startedUtc; DeadlineUtc = $deadlineUtc
            }
        }`,
        workerStart,
        workerComplete,
        `$script:RelayBase = 'http://127.0.0.1:1'
        $owned = Get-Process -Id $PID
        $script:ProductionRelayProcess = $owned
        $script:SimRelayErrorLog = ''
        $script:SimRelayLog = ''
        $script:ClientLogInitialLengths = @{}
        $script:ClientLogOwnedProcessIds = @{}
        $script:StartupModalSettleMilliseconds = 300
        $script:LastStartupPopupEvidenceTick = @{ host = [long]0; join = [long]0 }
        $script:LastStartupPopupEvidenceUtc = @{ host = $null; join = $null }
        $GameDir = (Get-Location).Path
        $startupObserver = [pscustomobject]@{ Token = 'literal-inner-startup-observer' }
        $startGate = [System.Threading.ManualResetEventSlim]::new($false)
        $hostWorker = Start-LiteralNavigationWorker -Role host \`
            -StartupObserver $startupObserver \`
            -OwnedProcess $owned -LaunchUtc ([DateTime]::UtcNow) \`
            -ClientLog host.log -TimeoutSec 10 -StartGate $startGate \`
            -WaitPeerMarker marker
        Start-Sleep -Milliseconds 250
        if ($hostWorker.Async.IsCompleted) {
            throw 'closed common gate did not hold the first-constructed host lane'
        }
        $joinWorker = Start-LiteralNavigationWorker -Role join \`
            -StartupObserver $startupObserver \`
            -OwnedProcess $owned -LaunchUtc ([DateTime]::UtcNow) \`
            -ClientLog join.log -TimeoutSec 10 -StartGate $startGate
        $startGate.Set()
        Start-Sleep -Milliseconds 350
        if (-not $joinWorker.Async.IsCompleted) {
            throw 'fast join worker was serialized behind delayed host worker'
        }
        if ($hostWorker.Async.IsCompleted) {
            throw 'delayed host worker did not retain its independent blocking interval'
        }
        $hostResult = Complete-LiteralNavigationWorker $hostWorker
        $joinResult = Complete-LiteralNavigationWorker $joinWorker
        if (-not [object]::ReferenceEquals($hostResult.OwnedProcess, $owned) -or
            -not [object]::ReferenceEquals($joinResult.OwnedProcess, $owned)) {
            throw 'worker result lost exact Process reference identity'
        }
        if (($hostResult.DeadlineUtc - $hostResult.LaneStartedUtc).TotalSeconds -ne 10 -or
            ($joinResult.DeadlineUtc - $joinResult.LaneStartedUtc).TotalSeconds -ne 10) {
            throw 'a lane did not receive its own complete ten-second budget'
        }
        if ([Math]::Abs(($hostResult.LaneStartedUtc -
                        $joinResult.LaneStartedUtc).TotalMilliseconds) -gt 500) {
            throw 'construction order leaked through the common start gate'
        }
        $startGate.Dispose()
        'DELAYED_LANE_SCHEDULER_PASS'`,
    ].join('\n');
    const schedulerTempDir = fs.mkdtempSync(path.join(
        require('os').tmpdir(), 'd2mss-navigation-worker-'));
    const schedulerScriptPath = path.join(schedulerTempDir, 'scheduler.ps1');
    try {
        fs.writeFileSync(schedulerScriptPath, delayedLaneSchedulerScript, 'utf8');
        const delayedLaneScheduler = spawnSync(
            'pwsh', [
                '-NoLogo',
                '-NoProfile',
                '-NonInteractive',
                '-File',
                schedulerScriptPath,
            ], { encoding: 'utf8' });
        assert.equal(delayedLaneScheduler.error, undefined,
            `could not execute physical delayed-lane scheduler: ${delayedLaneScheduler.error}`);
        assert.equal(delayedLaneScheduler.status, 0,
            `physical delayed-lane scheduler failed:\n${delayedLaneScheduler.stdout}\n${delayedLaneScheduler.stderr}`);
        assert.match(delayedLaneScheduler.stdout, /DELAYED_LANE_SCHEDULER_PASS/,
            'a blocked host runspace cannot delay the independently completed join runspace');
    } finally {
        fs.rmSync(schedulerTempDir, { recursive: true, force: true });
    }
    assert.doesNotMatch(navigation,
        /Get-DialogObservation|Get-LiteralClientLogMarkerEventUtcSnapshot|Invoke-RestMethod|Get-Content|Start-Sleep|while\s*\(/,
        'the outer result join contains no blocking cooperative navigation read/pump');
    assert.doesNotMatch(pairing,
        /Get-LiteralMainMenuAtLaunchAnchor|Invoke-OneShotTransition\s+host\s+DLG_MAIN_MENU|Select-Settle\s+host\s+DLG_PROTOCOL|\$hostScenarioRequest|\$hostLoadRequest|\$joinMainMenuRequest|Wait-LiteralSelectionOrUtcAnchor/,
        'Run-Pairing cannot serialize a host prelude before entering the physical worker pair');
    assert.doesNotMatch(hostLane, /\$join(?:Lane|Worker|Process|Result|\.)/,
        'the host lane cannot consume join progress or a join command result');
    assert.doesNotMatch(joinLane, /\$host(?:Lane|Worker|Process|Result|\.)/,
        'the join lane cannot consume host progress or a host clock');

    const hostOrder = [
        '-Role host -Dialog DLG_MAIN_MENU -Button BTN_MULTI',
        '-Kind index -Role host -Dialog DLG_PROTOCOL',
        '-Role host -Dialog DLG_PROTOCOL -Button BTN_CONTINUE',
        '-Role host -Dialog DLG_LOAD_NEW_MULTI -Button BTN_HOST',
        "[string]$observation.Dialog -eq 'DLG_CHOOSE_SKIRMISH'",
        '-Role host -Dialog DLG_CHOOSE_SKIRMISH -Button BTN_LOAD',
        '$lane.Load = Complete-LiteralButtonRequest $lane.Pending',
        'Get-LiteralClientLogMarkerEventUtcSnapshot',
        '$lane.Load.CompletedUtc -gt',
        '([DateTime]$lane.WaitPeerReleaseUtc).AddMilliseconds(500)',
        'Wait-FixedUtcAnchor $lane.LobbyArmUtc',
        '-Role host -Dialog DLG_LOBBY -Button BTN_OK',
        '$lane.LobbyOk = Complete-LiteralButtonRequest $lane.Pending',
        '$lane.LobbyOk.CompletedUtc.AddMilliseconds(500)',
        'Wait-FixedUtcAnchor $lane.PopupArmUtc',
        'New-LiteralStartupPopupService host $ClientLog',
        'New-LiteralStartupPopupWindow'
    ];
    let previousHost = -1;
    for (const token of hostOrder) {
        const at = hostLane.indexOf(token);
        assert.ok(at > previousHost, `legacy host self-nav step moved or vanished: ${token}`);
        previousHost = at;
    }
    const joinOrder = [
        '-Role join -Dialog DLG_MAIN_MENU -Button BTN_MULTI',
        "$lane.Phase = 'protocol-dialog'",
        '-Kind index -Role join -Dialog DLG_PROTOCOL',
        '-Role join -Dialog DLG_PROTOCOL -Button BTN_CONTINUE',
        "$lane.Phase = 'load-new-multi'",
        '-Role join -Dialog DLG_LOAD_NEW_MULTI -Button BTN_JOIN',
        '$lane.Search = Complete-LiteralButtonRequest $lane.Pending',
        '$lane.Search.CompletedUtc.AddMilliseconds(2000)',
        'Wait-FixedUtcAnchor $lane.JoinGameArmUtc',
        "$lane.Phase = 'join-session'",
        '-Role join -Dialog DLG_SESSION -Button BTN_JOIN_GAME',
        '$lane.JoinGame = Complete-LiteralButtonRequest $lane.Pending',
        "$lane.Phase = 'wait-host-briefing'",
        "$lane.Phase -eq 'wait-host-briefing'",
        '$lane.StartupState.BriefingLatch',
        '$lane.HostBriefingReleasedUtc = [DateTime]::UtcNow',
        "$lane.Phase = 'wait-host-strategic'",
        "$lane.Phase -eq 'wait-host-strategic'",
        '$lane.StartupState.StrategicLatch',
        '$lane.HostStrategicReleasedUtc = [DateTime]::UtcNow',
        '$lane.HostStrategicReleasedUtc.AddMilliseconds(1000)',
        'Wait-FixedUtcAnchor $lane.FirstSettleArmUtc',
        '$lane.SecondSettleArmUtc =',
        'Wait-FixedUtcAnchor $lane.SecondSettleArmUtc',
        "$lane.Phase = 'lobby-dialog'",
        '-Role join -Dialog DLG_LOBBY -Button BTN_OK',
        '$lane.LobbyOk = Complete-LiteralButtonRequest $lane.Pending',
        '$lane.LobbyOk.CompletedUtc.AddMilliseconds(500)',
        'Wait-FixedUtcAnchor $lane.PopupArmUtc',
        'New-LiteralStartupPopupService join $ClientLog',
        'New-LiteralStartupPopupWindow',
        'join 20000 $lane.PopupService',
        'Wait-LiteralStartupPopupWindow $lane.PopupWindow',
        'if (-not [bool]$lane.PopupWindow.Done)',
        "$lane.Phase = 'done'"
    ];
    let previous = -1;
    for (const token of joinOrder) {
        const at = joinLane.indexOf(token);
        assert.ok(at > previous, `legacy join late-bind step moved or vanished: ${token}`);
        previous = at;
    }
    assert.doesNotMatch(joinLane,
        /Invoke-OneShotTransition join DLG_LOAD_NEW_MULTI BTN_JOIN|Invoke-OneShotTransition join DLG_SESSION BTN_JOIN_GAME/,
        'neither join action may wait for its later dialog effect before the next literal timer/gate');
    assert.equal((joinLane.match(/AddMilliseconds\(2000\)/g) || []).length, 1,
        'the join late bind has one local +2000 clock');
    assert.equal((joinLane.match(/AddMilliseconds\(1000\)/g) || []).length, 2,
        'WaitHostStrategic is followed by two distinct relative +1000 clocks inside the join lane');
    assert.equal((joinLane.match(/AddMilliseconds\(500\)/g) || []).length, 1,
        'join BTN_OK is followed by its one local +500 popup arm inside the join lane');
    assert.equal((hostLane.match(/Start-Sleep -Milliseconds 100\b/g) || []).length, 1,
        'host owns one ordinary bottom driver cadence');
    assert.equal((joinLane.match(/Start-Sleep -Milliseconds 100\b/g) || []).length, 1,
        'join owns one ordinary bottom driver cadence');
    assert.match(hostLane,
        /\$advanceWithinDriverTick = \$true[\s\S]*while \(\$advanceWithinDriverTick[\s\S]*Start-Sleep -Milliseconds 100/,
        'host can complete/arm adjacent steps on one driver tick before its ordinary bottom sleep');
    assert.match(joinLane,
        /\$advanceWithinDriverTick = \$true[\s\S]*while \(\$advanceWithinDriverTick[\s\S]*Start-Sleep -Milliseconds 100/,
        'join can complete/arm adjacent steps on one driver tick before its ordinary bottom sleep');
    assert.doesNotMatch(hostLane + joinLane + navigation,
        /\.Cancel|CancelAfter|refire|retry|fallback/i,
        'already-started join intents are completed once and never cancelled or redispatched');
    for (const button of ['BTN_MULTI', 'BTN_CONTINUE', 'BTN_JOIN', 'BTN_JOIN_GAME', 'BTN_OK']) {
        assert.equal((joinLane.match(new RegExp(`-Button ${button}\\b`, 'g')) || []).length, 1,
            `${button} has exactly one independent action site`);
    }
    assert.equal((joinLane.match(/-Kind index -Role join -Dialog DLG_PROTOCOL/g) || []).length, 1,
        'join protocol selection is one native write with one ten-frame completion');
    assert.match(joinLane,
        /\$startupTurnBaseline = Get-TurnHistory[\s\S]*\$lane\.StartupRequest = Start-LiteralTurnHistoryRequest\s+`?\s*-After \$startupTurnWatermark -Role join -WaitMilliseconds 120000/,
        'the released physical join worker owns both the baseline and stock subscription');
    assert.match(joinLane,
        /\$lane\.StartupRequest\.Task\.IsCompleted[\s\S]*Complete-LiteralTurnHistoryRequest \$lane\.StartupRequest[\s\S]*Add-LiteralJoinStockStartupHistory[\s\S]*-After \$lane\.StartupState\.Cursor/,
        'the join lane folds each completed append-only subscription and continues only from its advanced cursor');
    assert.match(turnSubscriptionStart, /HttpMethod\]::Get/,
        'the stock witness subscription is passive GET-only');
    assert.match(turnSubscriptionStart,
        /InfiniteTimeSpan[\s\S]*\.SendAsync\(\$request\)/,
        'the stock gates use one uncancelled asynchronous request per append-only cursor interval');
    assert.equal((turnSubscriptionStart.match(/\.SendAsync\(/g) || []).length, 1);
    assert.doesNotMatch(turnSubscriptionStart + turnSubscriptionComplete,
        /\.Cancel|CancelAfter|refire|retry|fallback/i,
        'the passive stock subscription has no cancellation or resubmission branch for one cursor interval');
    assert.match(stockHistoryFold,
        /\$State\.StrategicLatch = \$beginEvent[\s\S]*\$State\.BriefingLatch = \$joinGameEvent[\s\S]*\$State\.StrategicLatch -and \$State\.BriefingLatch/,
        'BeginTurn and CJoinGame are independently validated/latching before their cross-identity proof');
    assert.doesNotMatch(stockHistoryFold,
        /beginSequence\s*-ge\s*\$joinGameSequence|joinGameSequence\s*-g[et]\s*\$beginSequence/,
        'wire arrival order cannot collapse or reject the two source predicates');
    assert.match(joinLane,
        /Phase -eq 'wait-host-briefing'[\s\S]{0,220}StartupState\.BriefingLatch/,
        'WaitHostBriefing consumes only the CJoinGame latch');
    assert.match(joinLane,
        /Phase -eq 'wait-host-strategic'[\s\S]{0,220}StartupState\.StrategicLatch/,
        'WaitHostStrategic consumes only the BeginTurn latch');
    assert.doesNotMatch(joinLane,
        /Phase -eq 'wait-host-(?:briefing|strategic)'[\s\S]{0,220}StartupState\.Witness/,
        'neither source gate can wait for the combined two-event witness');
    const briefingGate = joinLane.match(
        /Phase -eq 'wait-host-briefing'[\s\S]*?Phase = 'wait-host-strategic'/)?.[0] || '';
    const strategicToLobby = joinLane.match(
        /Phase -eq 'wait-host-strategic'[\s\S]*?Phase = 'lobby-dialog'/)?.[0] || '';
    assert.doesNotMatch(briefingGate, /\$advanceWithinDriverTick = \$true/,
        'arming the distinct strategic gate receives its own ordinary driver tick');
    assert.match(joinLane,
        /Phase -eq 'wait-host-strategic'[\s\S]*?AddMilliseconds\(1000\)[\s\S]*?Wait-FixedUtcAnchor \$lane\.FirstSettleArmUtc[\s\S]*?AddMilliseconds\(1000\)[\s\S]*?Wait-FixedUtcAnchor \$lane\.SecondSettleArmUtc[\s\S]*?\$lane\.Phase = 'lobby-dialog'[\s\S]*?\$advanceWithinDriverTick = \$true/,
        'two source Delay(1000) steps continue directly into the sole lobby OK arm without a 100ms bubble');
    assert.doesNotMatch(strategicToLobby, /Start-Sleep -Milliseconds 100/,
        'consecutive source Delay steps never pass through the ordinary bottom sleep');
    assert.doesNotMatch(pairing,
        /Wait-NewValidatedJoinStockStartupObserved|Wait-FixedUtcAnchorWithPopupService|Get-DialogObservation\s+join|Invoke-OneShotButton\s+`?\s*join\s+DLG_LOBBY|New-LiteralStartupPopupService\s+join/,
        'the parent cannot re-read or postpone the join stock gates, clocks, action, or first popup service');
    assert.doesNotMatch(pairing, /\$sessionListReadyAfter|sessionListReadyDelta/,
        'EnumSessions proof must not delay the literal pairing/popup trajectory');
    assert.match(savedWitness,
        /\$sessionListReadyAfter = Measure-LiteralSavedLogMarker[\s\S]*\$sessionListReadyDelta = \$sessionListReadyAfter -[\s\S]*\$sessionListReadyDelta -ne 1/,
        'the terminal quiet tick later proves the exact +1 EnumSessions marker from saved log bytes');
    assert.doesNotMatch(savedWitness,
        /Get-ClientLogMarkerCount|Read-ClientLogLines|Wait-|Start-Sleep/,
        'deferred EnumSessions validation is pure and adds no late observation');

    const waitPeerMarker = '[nettrace] peer CConnectMsg observed self=';
    assert.match(nativeNettrace,
        /connectDpidOffset = 36[\s\S]*memcmp\(payload, connectRtti[\s\S]*peer CConnectMsg observed self=\{\} peer=\{\}/,
        'the removable RX observer must reproduce the exact second-different-DPID WaitPeer witness');
    assert.ok(pairing.includes(`$waitPeerMarker = '${waitPeerMarker}'`));
    assert.match(hostLane,
        /Get-LiteralClientLogMarkerEventUtcSnapshot\s+`?\s*\$ClientLog \$WaitPeerMarker \$WaitPeerBefore[\s\S]*\$lane\.WaitPeerReleaseUtc = \[DateTime\]\$lane\.WaitPeer\.eventUtc[\s\S]*\$lane\.Load\.CompletedUtc -gt[\s\S]*\(\[DateTime\]\$lane\.WaitPeerReleaseUtc\)\.AddMilliseconds\(500\)/,
        'host +500 begins at max(LOAD completion, exact peer CConnect event)');
    const hostOkAt = hostLane.indexOf('-Role host -Dialog DLG_LOBBY -Button BTN_OK');
    const joinGameCompletionAt = joinLane.indexOf(
        '$lane.JoinGame = Complete-LiteralButtonRequest $lane.Pending');
    assert.ok(hostOkAt >= 0 && joinGameCompletionAt >= 0,
        'both independent terminal action sites remain present');
    assert.doesNotMatch(hostLane.slice(0, hostOkAt), /JoinGame|\$join(?:Lane|Worker|Process|Result|\.)/,
        'host BTN_OK cannot wait for the join HTTP command tail');
});

test('client fault sentinels name both exact native preflight emitters', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const engineHooks = fs.readFileSync(nativeEngineHooksScript, 'utf8');
    const patches = fs.readFileSync(nativePatchesScript, 'utf8');
    assert.match(engineHooks, /\[simturns\] detour preflight mismatch/);
    assert.match(patches, /\[simturns\] preflight mismatch/);
    assert.ok(runner.includes("'\\[simturns\\] detour preflight mismatch'"));
    assert.ok(runner.includes("'\\[simturns\\] preflight mismatch'"));
    assert.doesNotMatch(runner, /refusing partial activation/);
});

test('one-shot production relay identity loss is a terminal PoC event', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const guard = runner.match(/function Assert-NoRelayFault[\s\S]*?(?=\nfunction )/);
    assert.ok(guard, 'Assert-NoRelayFault must remain identifiable');
    assert.match(guard[0], /'hello-rejected'/);
    assert.match(guard[0], /'peer-disconnected'/);
});

test('green teardown proves complete quiet logs before paired exact-handle kill', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const quiet = powerShellFunction(runner,
        'Wait-ClientLogsAtCompleteQuietBoundary');
    const pair = powerShellFunction(runner, 'Stop-OwnedClientPair');
    const copy = powerShellFunction(runner,
        'Copy-StoppedClientLogWithCompletionProof');
    assert.match(quiet,
        /QuietMilliseconds = 750[\s\S]*endedWithLineFeed[\s\S]*\$signature[\s\S]*quietStartedAt/,
        'both LF tails and stable byte lengths must define the quiet boundary');
    assert.equal((pair.match(/\.Kill\(\)/g) || []).length, 1,
        'the pair helper exposes one exact-handle Kill site shared by its two entries');
    assert.equal((pair.match(/WaitForExit\(/g) || []).length, 1,
        'both kills are issued before the shared exit-observation loop waits');
    assert.doesNotMatch(pair, /Get-Process|Stop-Process/,
        'paired teardown cannot rediscover or broadly stop a process');
    assert.match(copy,
        /FileShare\]::ReadWrite[\s\S]*FileShare\]::Delete[\s\S]*\$sourceStream\.CopyTo\(\$memory\)[\s\S]*WriteAllBytes\(\$artifactPath, \$sourceBytes\)/,
        'stopped logs must be snapshotted through compatible shared access');
    assert.doesNotMatch(copy,
        /ReadAllBytes\(\$fullPath\)|Get-FileHash -LiteralPath \$fullPath/,
        'proof cannot reopen the stopped source with a share-denying helper');

    const runtimeAt = runner.lastIndexOf('\n$testRelay = $null');
    const runtime = runner.slice(runtimeAt).replace(/\r\n/g, '\n');
    const quietAt = runtime.indexOf(
        '$clientLogQuiescence = Wait-ClientLogsAtCompleteQuietBoundary');
    const pairAt = runtime.indexOf(
        '$clientPairTeardown = Stop-OwnedClientPair', quietAt);
    const copyAt = runtime.indexOf(
        '$clientLogCompletion[$entry.role] =', pairAt);
    assert.ok(quietAt >= 0 && pairAt > quietAt && copyAt > pairAt,
        'green teardown must prove quiet, stop both clients, then copy exact bytes');
    assert.match(runtime,
        /preStopQuiescence = \$clientLogQuiescence[\s\S]*clientPair = \$clientPairTeardown/,
        'the summary retains both pre-stop and paired-exit proofs');
});

test('literal join +14s heuristic needs a positive collapse sentinel', () => {
    const inner = fs.readFileSync(literalInnerStartupScript, 'utf8');
    assert.match(inner, /\$sourceJoinSyncBudgetSeconds = 14/,
        'the source diagnostic boundary remains 14 seconds');
    assert.match(inner,
        /\$sourceJoinSyncBudgetExceeded = \$true[\s\S]*if \(\$joinCollapseEvidence\.injectThrew -or[\s\S]*\$joinCollapseEvidence\.stuckMessageBox\) \{[\s\S]*literal inner join collapse: host strategic \+14s/,
        'elapsed time alone cannot publish an early collapse verdict');
    assert.match(inner,
        /continuing passive observation[\s\S]*source-join-sync-budget-exceeded/,
        'a live sentinel-free join remains under passive observation');
    assert.match(inner,
        /\$strategicDeadlineUtc = \$rolesReadyUtc\.AddSeconds\(\$TotalBudgetSec\)[\s\S]*literal inner FAIL: strategic not reached within the fixed/,
        'the existing fixed total budget remains the hard terminal gate');
});

test('production PoC wires the literal inner observer around one reversible release edge', () => {
    const runner = fs.readFileSync(productionPocScript, 'utf8');
    const archive = powerShellFunction(runner, 'Move-StaleMssClientLogsToArtifact');
    const relayLaunch = powerShellFunction(runner, 'Start-ProductionSimRelay');
    const clientLaunch = powerShellFunction(runner, 'Start-SimturnGameClient');
    const runtimeAt = runner.lastIndexOf('\n$testRelay = $null');
    assert.ok(runtimeAt >= 0, 'the owned-run runtime boundary must remain identifiable');
    // Git may materialize this PowerShell source with CRLF on Windows. The
    // oracle below is about token order, not checkout newline policy.
    const runtime = runner.slice(runtimeAt).replace(/\r\n/g, '\n');

    assert.match(archive,
        /Get-ChildItem[\s\S]*-Filter 'mss32_\*\.log'[\s\S]*\^mss32_\(\[1-9\]\[0-9\]\*\)\\\.log\$/,
        'only numeric per-PID mss32 logs may enter the archive census');
    assert.match(archive,
        /Get-Process -Id \$embeddedPid -ErrorAction SilentlyContinue[\s\S]*if \(\$liveOwner\)[\s\S]*\$preservedLive\.Add[\s\S]*continue/,
        'every live ambient PID log must remain untouched');
    assert.equal((archive.match(/Move-Item -LiteralPath/g) || []).length, 1,
        'the archive exposes one reversible move site');
    assert.doesNotMatch(archive, /Remove-Item|Clear-Content|Set-Content\s+-LiteralPath\s+\$sourcePath/,
        'the archive cannot delete or truncate a source log');
    assert.match(archive,
        /Get-FileHash[\s\S]*Move-Item -LiteralPath[\s\S]*Get-FileHash[\s\S]*archivedItem\.Length -ne \$beforeLength/,
        'the archive must verify length and SHA-256 across the move');
    assert.match(archive,
        /archiveRoot\.StartsWith[\s\S]*artifactPrefix[\s\S]*preexisting client-log archive escaped/,
        'the move destination must be proven inside the exact run artifact');

    assert.match(relayLaunch,
        /'--merge-day', \[string\]\$MergeDay[\s\S]*'--bootstrap-release-file', "`"\$script:LiteralInnerBootstrapReleaseFile`""[\s\S]*'--bootstrap-cascade-delay-ms', '500'/,
        'the coordinator receives the authoritative merge day, one exact release file, and fixed +500ms cascade clock');
    assert.match(clientLaunch,
        /\$initialLength -ne 0[\s\S]*inherited a non-empty[\s\S]*ClientLogInitialLengths/,
        'each owned client must start its exact PID log at byte zero');

    const commonStartupOrder = [
        'Assert-ExclusiveTestMachine -GameDir $GameDir',
        '$preexistingClientLogArchive = Move-StaleMssClientLogsToArtifact',
        '$literalInnerDumpBaseline = Get-LiteralInnerDumpBaseline',
        '$script:LiteralInnerStartupObserver = Start-LiteralInnerStartupObserver',
        '$testRelay = Start-TestRelay -LogDir $ArtifactDir',
        '$simRelay = Start-ProductionSimRelay',
        '$hostProcess = Start-SimturnGameClient host',
        '$hostLog = Join-Path $GameDir "mss32_$($hostProcess.Id).log"',
        'Publish-LiteralInnerStartupProcess `\n        $script:LiteralInnerStartupObserver host',
        'Wait-FixedUtcAnchor ($hostLaunchUtc.AddMilliseconds(10000))',
        '$hostJoinLaunchDeadlineUtc = $hostLaunchUtc.AddSeconds($BootTimeoutSec)',
        '$hostPreJoinMainMenu = Wait-UiButtonReadyPublication',
        '$joinProcess = Start-SimturnGameClient join',
        '$joinLog = Join-Path $GameDir "mss32_$($joinProcess.Id).log"',
        'Publish-LiteralInnerStartupProcess `\n        $script:LiteralInnerStartupObserver join',
        '$pairingResult = Run-Pairing'
    ];
    let previous = -1;
    for (const token of commonStartupOrder) {
        const at = runtime.indexOf(token, previous + 1);
        assert.ok(at > previous, `literal inner runtime edge moved or vanished: ${token}`);
        previous = at;
    }

    const orderedBranchAt = runtime.indexOf(
        "if ($GameplayMode -eq 'ordered-masstest') {", previous);
    const nonOrderedBranchAt = runtime.indexOf('} else {', orderedBranchAt);
    const commonVerdictAt = runtime.indexOf('\n    if ($ProbeRelayFailure)', nonOrderedBranchAt);
    assert.ok(orderedBranchAt > previous && nonOrderedBranchAt > orderedBranchAt &&
        commonVerdictAt > nonOrderedBranchAt,
    'ordered and run_test startup continuations must remain separate branches');
    const orderedBranch = runtime.slice(orderedBranchAt, nonOrderedBranchAt);
    const nonOrderedBranch = runtime.slice(nonOrderedBranchAt, commonVerdictAt);

    const orderedMasstestOrder = [
        '$literalInnerStartupResult = Complete-LiteralInnerStartupObserver',
        '$script:LiteralInnerStartupObserver = $null',
        'Start-Sleep -Seconds 2',
        '$orderedInitialCheckpoint = Read-LiteralMasstestPeerStateCheckpoint',
        '$orderedMergeHandoff = Run-LiteralOrderedMasstestToMerge',
        '$literalPhaseC = Run-AutomaticMasstestPhaseCLiteral',
        '$deferredPhaseCMss = Complete-LiteralOrderedMasstestDeferredProof'
    ];
    previous = -1;
    for (const token of orderedMasstestOrder) {
        const at = orderedBranch.indexOf(token, previous + 1);
        assert.ok(at > previous, `ordered startup/runtime edge moved or vanished: ${token}`);
        previous = at;
    }
    assert.doesNotMatch(orderedBranch,
        /Wait-LiteralDayReady|Resolve-LiteralHostAuthoritativeHeroes|Invoke-CanonicalDeploy|Run-MergeBarrier/,
        'the synchronous old masstest caller does not enter run_test quiet/gameplay');

    const runTestOrder = [
        '$legacyQuietWitness = Wait-LiteralDayReady',
        'legacy startup PASS: both role logs reached the exact quiet-3 checkpoint',
        '$heroCensus = Resolve-LiteralHostAuthoritativeHeroes $fixture',
        '$terminalRoleStates = $legacyQuietWitness.TerminalRoleStates',
        '$preparedDeployBindings = New-CanonicalWalkPreparation',
        'Invoke-CanonicalDeploy `',
        '$startupQuietWitness = Get-LiteralStartupMssWitnessSnapshot',
        '$startupQuietWitness.PreparedDeployBindings = $preparedDeployBindings',
        '$startupMssProof = Assert-LiteralStartupMssWitness',
        'Assert-DeferredLiteralHostAuthoritativeHeroFixture'
    ];
    previous = -1;
    for (const token of runTestOrder) {
        const at = nonOrderedBranch.indexOf(token, previous + 1);
        assert.ok(at > previous, `run_test startup edge moved or vanished: ${token}`);
        previous = at;
    }
    const preClockArchiveAt = runtime.indexOf(
        '$preexistingClientLogArchive = Move-StaleMssClientLogsToArtifact');
    const firstLegacyClockAt = runtime.indexOf('Start-Sleep -Milliseconds 1200');
    assert.ok(preClockArchiveAt >= 0 && firstLegacyClockAt > preClockArchiveAt,
        'log archive and dump hashing must complete before the first source clock');
    const completionAt = runtime.lastIndexOf(
        '$literalInnerStartupResult = Complete-LiteralInnerStartupObserver');
    const gameplayEndAt = runtime.indexOf('Assert-ClientsLive $hostProcess $joinProcess',
        runtime.indexOf('if ($ProbeRelayFailure)'));
    assert.ok(completionAt > gameplayEndAt,
        'the run_test diagnostic observer is joined only after all old gameplay verdicts');
    assert.equal((runtime.match(
        /\$literalInnerStartupResult = Complete-LiteralInnerStartupObserver/g) || []).length, 2,
    'one synchronous ordered join and one guarded run_test join are the only completion sites');
    assert.match(runtime,
        /if \(\$script:LiteralInnerStartupObserver\) \{[\s\S]*Complete-LiteralInnerStartupObserver[\s\S]*\$script:LiteralInnerStartupObserver = \$null[\s\S]*\} elseif \(-not \$literalInnerStartupResult\)/,
        'ordered consumption leaves a retained result and prevents a second observer join');
    const passedAt = runtime.indexOf('$passed = $true', completionAt);
    assert.ok(passedAt > completionAt,
        'success is published only after the appropriate observer completion is retained');
    assert.match(runtime,
        /finally \{[\s\S]*Stop-LiteralInnerStartupObserver \$script:LiteralInnerStartupObserver[\s\S]*Stop-LiteralOuterReadyObserver[\s\S]*Stop-OwnedProcess/,
        'failure cleanup must stop observers before exact owned clients and relays');
});
