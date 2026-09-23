#requires -Version 7.0
<#
.SYNOPSIS
Runs the removable end-to-end runtime proof for production D2MSS simultaneous turns.

.DESCRIPTION
This script deliberately keeps the two relays separate:

* tools/relay/relay.js is the DebugTest control plane. It reports live UI/world
  state and executes only already-proven named UI actions on the game UI thread.
* tools/simturns-relay/src/cli.js is the production simultaneous-turn
  coordinator. It never carries UI commands or DirectPlay traffic.

Both game instances use the same DebugTest mss32.dll, but production simturns is
enabled independently with D2MSS_SIMTURNS*. The host and join process receive
their role in their own ProcessStartInfo environment; no process-global role is
reused. The scenario makes a real DirectPlay TCP/IP session, reaches the map,
 waits for the complete protocol-v8 bootstrap and native operational marker,
then submits every logical BTN_END_TURN intent exactly once. It asserts relay
events, each client's local world day, process/UI liveness, and absence of
production simturn faults.

With -MergeDay N, all independent rounds through N-1 are driven and the final
End Turn pair verifies two barrier-held proofs and one relay-owned merge
prepare/execute/apply/release transaction with no ordinary turn action.
-BarrierOrder can keep that pair parallel or prove either exact first-arrival
order. A MergeDay of 2 is valid but exercises only the barrier path. With
MergeDay 0, -IndependentRounds controls the number of independent rounds.

-PostMergeContinuationMode keeps three deliberately separate boundaries:

* none stops at the existing barrier verdict (and, for canonical parallel,
  its exact final green first-host/dead-click proof);
* automatic-masstest-phase-c-literal belongs only to the standalone
  GameplayMode=ordered-masstest route: a fresh map, the four distinct old
  ordered End Turns, their merge, and then the tuned old masstest Phase C. It
  is never appended to canonical or battle-block gameplay; and
* mss-stock-telemetry runs the stronger MSS-native stock telemetry continuation.

The latter is an MSS proof, not a claim that the old Phase C timing was replayed.

-ProbeRelayFailure is an optional destructive-to-this-run fail-closed probe. It
kills only the production relay process owned by this script, waits for both
clients to report terminal pipe failure, then verifies another host End Turn
does not advance either local day. It is intentionally unavailable after merge.

The script ignores every Discipl2 process it did not launch and never kills
Discipl2/node/dplaysvr by name or deletes logs. Before source timing begins it
moves only stale per-PID logs with no live process owner into the run artifact;
live ambient logs stay untouched. It retains exact Process objects for the two
clients and two relays it launches. The DirectPlay dplaysvr helper is
machine-global, so an already-running helper is the only actual network conflict.
Cleanup is the default; -Keep is an explicit diagnostic opt-in.

.EXAMPLE
pwsh -File .\tools\test\simturns-production-poc.ps1

.EXAMPLE
pwsh -File .\tools\test\simturns-production-poc.ps1 -MergeDay 3

.EXAMPLE
pwsh -File .\tools\test\simturns-production-poc.ps1 -ProbeRelayFailure

.EXAMPLE
pwsh -File .\tools\test\simturns-production-poc.ps1 -StaticCheck
#>
[CmdletBinding()]
param(
    [int]$Scenario = 0,
    [string]$ScenarioPath,
    [ValidateSet(
        'protocol',
        'canonical',
        'battle-block',
        'long-move',
        'long-attack',
        'ordered-masstest'
    )]
    [string]$GameplayMode = 'protocol',
    [ValidateSet('none', 'audit-idle', 'audit-moving', 'dead-leader')]
    [string]$BattleCase = 'none',
    [switch]$BattleTrace,
    [ValidateSet(
        'source-route-control',
        'source-pair-repro',
        'clean-host-route-control',
        'clean-join-route-control',
        'clean-long-concurrency'
    )]
    [string]$LongMoveCase = 'clean-long-concurrency',
    [string]$FixtureManifest,
    [ValidateScript({ $_ -eq 0 -or $_ -ge 2 })]
    [int]$MergeDay = 0,
    [ValidateSet('parallel', 'host-first', 'join-first')]
    [string]$BarrierOrder = 'parallel',
    [ValidateRange(1, 20)]
    [int]$IndependentRounds = 1,
    [ValidateRange(30, 300)]
    [int]$StepTimeoutSec = 120,
    [ValidateRange(30, 600)]
    [int]$BootTimeoutSec = 240,
    [switch]$ProbeRelayFailure,
    [ValidateSet(
        'none',
        'automatic-masstest-phase-c-literal',
        'mss-stock-telemetry'
    )]
    [string]$PostMergeContinuationMode = 'none',
    [switch]$Keep,
    [string]$GameDir,
    [string]$PipeName,
    [string]$ArtifactDir,
    [switch]$StaticCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$helperPath = Join-Path $PSScriptRoot '_relay.ps1'
$gameplayHelperPath = Join-Path $PSScriptRoot '_simturns_gameplay.ps1'
$battleCasesHelperPath = Join-Path $PSScriptRoot '_simturns_battle_cases.ps1'
$ownedProcessDiagnosticsPath = Join-Path $PSScriptRoot '_owned_process_diagnostics.ps1'
$literalInnerStartupPath = Join-Path $PSScriptRoot '_literal_inner_startup.ps1'
$autoBattleHttpContractPath = Join-Path $PSScriptRoot 'auto-battle-http-contract.ps1'
$acceptanceCampaignPath = Join-Path $PSScriptRoot 'simturns-acceptance-campaign.ps1'
$mssMainPath = Join-Path $repoRoot 'mss32\src\main.cpp'
$packetLogicBridgePath = Join-Path $repoRoot 'mss32\src\testdrv\packetlogicbridge.cpp'
$defaultFixtureManifest = Join-Path $PSScriptRoot 'fixtures\simturns-russobit.json'
$productionRelayDir = Join-Path $repoRoot 'tools\simturns-relay'
$productionRelayCli = Join-Path $productionRelayDir 'src\cli.js'
$defaultLocalGameDir = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot '..\..\..\last_version\Game'))
$expectedRussobitExeSize = 4187648
$expectedRussobitExeSha256 =
    '1375CDEF09EC470EE64FE5693FB734D7C69FB215212311D997F792B258A642EB'

# Offline verification used by CI/developers without a licensed game image.
if ($StaticCheck) {
    $required = @(
        $helperPath,
        $gameplayHelperPath,
        $ownedProcessDiagnosticsPath,
        $literalInnerStartupPath,
        $autoBattleHttpContractPath,
        $acceptanceCampaignPath,
        $mssMainPath,
        $packetLogicBridgePath,
        $defaultFixtureManifest,
        $productionRelayCli,
        (Join-Path $productionRelayDir 'src\protocol.js'),
        (Join-Path $productionRelayDir 'src\coordinator.js'),
        (Join-Path $productionRelayDir 'src\framed-connection.js'),
        (Join-Path $productionRelayDir 'src\server.js')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "static check: required file is missing: $path"
        }
    }

    foreach ($path in @(
        $PSCommandPath,
        $helperPath,
        $gameplayHelperPath,
        $ownedProcessDiagnosticsPath,
        $literalInnerStartupPath,
        $autoBattleHttpContractPath,
        $acceptanceCampaignPath
    )) {
        $tokens = $null
        $parseErrors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile(
            $path, [ref]$tokens, [ref]$parseErrors)
        if ($parseErrors.Count -ne 0) {
            $details = ($parseErrors | ForEach-Object Message) -join '; '
            throw "static check: PowerShell parse failed for '$path': $details"
        }
    }

    $source = Get-Content -LiteralPath $PSCommandPath -Raw
    $helperSource = Get-Content -LiteralPath $helperPath -Raw
    $gameplaySource = Get-Content -LiteralPath $gameplayHelperPath -Raw
    $literalInnerStartupSource = Get-Content -LiteralPath $literalInnerStartupPath -Raw
    $acceptanceCampaignSource = Get-Content -LiteralPath $acceptanceCampaignPath -Raw
    $mssMainSource = Get-Content -LiteralPath $mssMainPath -Raw
    $packetLogicBridgeSource = Get-Content -LiteralPath $packetLogicBridgePath -Raw
    if (-not $mssMainSource.Contains(
            'logger->flush_on(spdlog::level::info);') -or
        $mssMainSource.Contains(
            'logger->flush_on(spdlog::level::trace);')) {
        throw ('static check: DebugTest must not synchronously flush every ' +
            'hot-path debug record')
    }
    $worldPublicationSource = [regex]::Match(
        $packetLogicBridgeSource,
        '(?ms)if \(worldreporter::copyWorldSnapshot.*?(?=\n\s*// 1\.)'
    ).Value
    $worldFlushOffset = $worldPublicationSource.IndexOf(
        'spdlog::default_logger()->flush();', [StringComparison]::Ordinal)
    $worldWriteOffset = $worldPublicationSource.IndexOf(
        'write_message(Op::WorldSnapshot', [StringComparison]::Ordinal)
    if ([string]::IsNullOrWhiteSpace($worldPublicationSource) -or
        $worldFlushOffset -lt 0 -or $worldWriteOffset -le $worldFlushOffset) {
        throw ('static check: a changed DebugTest WorldSnapshot must flush the ' +
            'preceding diagnostic records before it becomes observable')
    }
    foreach ($literal in @(
        'Wait-ClientLogsAtCompleteQuietBoundary',
        'Stop-OwnedClientPair',
        'Copy-StoppedClientLogWithCompletionProof',
        'sourceAndArtifactMatch = $true',
        'endedWithLineFeed = $true',
        'preStopQuiescence = $clientLogQuiescence',
        'clientPair = $clientPairTeardown',
        'clientLogs = $clientLogCompletion'
    )) {
        if (-not $source.Contains($literal)) {
            throw "static check: client-log completion contract lost '$literal'"
        }
    }
    $clientLogQuietSource = [regex]::Match(
        $source,
        '(?ms)^function Wait-ClientLogsAtCompleteQuietBoundary.*?(?=^function Stop-OwnedClientPair)'
    ).Value
    $clientPairStopSource = [regex]::Match(
        $source,
        '(?ms)^function Stop-OwnedClientPair.*?(?=^function Copy-StoppedClientLogWithCompletionProof)'
    ).Value
    $clientLogCopySource = [regex]::Match(
        $source,
        '(?ms)^function Copy-StoppedClientLogWithCompletionProof.*?(?=^function Show-FailureTail)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($clientLogQuietSource) -or
        -not $clientLogQuietSource.Contains(
            '[ValidateRange(250, 5000)][int]$QuietMilliseconds = 750') -or
        -not $clientLogQuietSource.Contains('$signature =') -or
        -not $clientLogQuietSource.Contains(
            '[bool]$hostSnapshot.endedWithLineFeed') -or
        -not $clientLogQuietSource.Contains(
            '[bool]$joinSnapshot.endedWithLineFeed')) {
        throw 'static check: pre-stop client-log complete quiet-boundary proof is missing'
    }
    if ([string]::IsNullOrWhiteSpace($clientPairStopSource) -or
        [regex]::Matches($clientPairStopSource, '\.Kill\(\)').Count -ne 1 -or
        [regex]::Matches($clientPairStopSource, 'WaitForExit\(').Count -ne 1 -or
        $clientPairStopSource -match 'Get-Process|Stop-Process') {
        throw 'static check: exact-handle paired client teardown contract changed'
    }
    if ([string]::IsNullOrWhiteSpace($clientLogCopySource) -or
        -not $clientLogCopySource.Contains(
            '[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete') -or
        -not $clientLogCopySource.Contains('$sourceStream.CopyTo($memory)') -or
        -not $clientLogCopySource.Contains(
            '[IO.File]::WriteAllBytes($artifactPath, $sourceBytes)') -or
        $clientLogCopySource.Contains('[IO.File]::ReadAllBytes($fullPath)') -or
        $clientLogCopySource.Contains(
            'Get-FileHash -LiteralPath $fullPath')) {
        throw 'static check: stopped client-log snapshot must tolerate compatible handles'
    }
    $clientLogQuietCallOffset = $source.LastIndexOf(
        '$clientLogQuiescence = Wait-ClientLogsAtCompleteQuietBoundary',
        [StringComparison]::Ordinal)
    $clientPairStopCallOffset = $source.LastIndexOf(
        '$clientPairTeardown = Stop-OwnedClientPair',
        [StringComparison]::Ordinal)
    $clientLogCopyOffset = $source.LastIndexOf(
        '$clientLogCompletion[$entry.role] =', [StringComparison]::Ordinal)
    if ($clientLogQuietCallOffset -lt 0 -or
        $clientPairStopCallOffset -le $clientLogQuietCallOffset -or
        $clientLogCopyOffset -le $clientPairStopCallOffset) {
        throw 'static check: client logs must prove quiet, stop the pair, then copy exact artifacts'
    }
    foreach ($literal in @(
        '$script:BattleBlockStepCount = 4',
        "'-PostMergeContinuationMode'",
        "'automatic-masstest-phase-c-literal'",
        "'mss-stock-telemetry'",
        'ExpectedPostMergeContinuationMode',
        'postMergeAutomaticMasstestPhaseC',
        'requiredNeutralRounds',
        'completedNeutralRounds',
        'slotWatermark',
        'deferredMssEvidence',
        '$steps.Count -ne $script:BattleBlockStepCount',
        'legacyVerdict'
    )) {
        if (-not $acceptanceCampaignSource.Contains($literal)) {
            throw "static check: acceptance campaign lost '$literal'"
        }
    }
    foreach ($literal in @(
        'Get-LiteralInnerDumpBaseline',
        'Start-LiteralInnerStartupObserver',
        'Publish-LiteralInnerStartupProcess',
        'Assert-LiteralInnerStartupObserverHealthy',
        'Complete-LiteralInnerStartupObserver',
        'Stop-LiteralInnerStartupObserver',
        'sourceGreenSnapshotSha256',
        '8BA71023479CCF0779C3FAC2128513D19D6AE83C2A20CFBBDC2B6E3821EA3961',
        '$strategicDeadlineUtc = $rolesReadyUtc.AddSeconds($TotalBudgetSec)',
        'while ([DateTime]::UtcNow -lt $strategicDeadlineUtc)',
        '$sourceJoinSyncBudgetSeconds = 14',
        '$sourceJoinSyncBudgetExceeded = $true',
        'if ($joinCollapseEvidence.injectThrew -or',
        "Trace-Step 'source-join-sync-budget-exceeded'",
        'SourceJoinSyncBudgetExceeded = $sourceJoinSyncBudgetExceeded',
        'bootstrap-cascade-delay-armed',
        'bootstrap-begin-turn-applied'
    )) {
        if (-not $literalInnerStartupSource.Contains($literal)) {
            throw "static check: literal inner startup helper lost '$literal'"
        }
    }
    $sourceJoinBudgetGate = [regex]::Match(
        $literalInnerStartupSource,
        '(?ms)if \(\$null -ne \$hostStrategicAtSeconds.*?' +
        '(?=\n\s*}\n\s*}\n\s*if \(-not \$hostStrategic)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($sourceJoinBudgetGate) -or
        [regex]::Matches(
            $sourceJoinBudgetGate,
            'literal inner join collapse: host strategic \+14s').Count -ne 1 -or
        -not $sourceJoinBudgetGate.Contains(
            '$joinCollapseEvidence.stuckMessageBox) {') -or
        -not $sourceJoinBudgetGate.Contains(
            'continuing passive observation')) {
        throw ('static check: source +14s join-sync boundary must fail early only ' +
            'with a positive collapse sentinel and otherwise remain passive')
    }
    if ($literalInnerStartupSource -match '\$Process\.Path\b') {
        throw 'static check: literal inner startup must use saved launch provenance, not Process.Path'
    }
    foreach ($literal in @(
        'CommandTimeoutMilliseconds',
        '&timeoutMs=$CommandTimeoutMilliseconds',
        'Wait-UiButtonReadyPublication',
        '/api/ui/wait-ready?role={0}&dlg={1}',
        'UI-ready observation does not belong to the expected owned role/PID'
    )) {
        if (-not $helperSource.Contains($literal)) {
            throw "static check: shared relay helper lost command-result deadline token '$literal'"
        }
    }
    $relayStateReadSource = [regex]::Match(
        $helperSource,
        '(?ms)^function Get-RelayState\s*\{.*?(?=^function Get-RoleState)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($relayStateReadSource) -or
        [regex]::Matches($relayStateReadSource, 'Invoke-RestMethod').Count -ne 1 -or
        -not $relayStateReadSource.Contains(
            '$response = Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 10') -or
        $relayStateReadSource -match
            '-Method\s+POST|Start-Sleep|\bwhile\s*\(|\bfor\s*\(') {
        throw ('static check: aggregate relay state must remain one passive GET ' +
            'with the cold-start-safe 10-second read deadline')
    }
    foreach ($literal in @(
        'Invoke-CanonicalBattleLifecyclesIndependently',
        'Invoke-LongMoveProof',
        'Invoke-LongMoveSourceRouteControl',
        'Observe-LongMoveSourcePair',
        'Wait-LongMoveTrajectoryEvidence',
        'Invoke-LongAttackProof',
        'Get-LongAttackMovementStart',
        'chargeWatchParity',
        'Read-PrearmedAutoBattleProof',
        '[testdrv][auto-battle-proof] ',
        'preboot-first-battle',
        'Step-CanonicalBattleCompletionState $state',
        'controllerGateBefore',
        'kickStateBefore',
        'kickStateAfter',
        'sideSelector',
        'flag38Before',
        'flag38After',
        'flag39Before',
        'flag39After',
        'callbackCount',
        'functorVftable',
        'dispatchFunction',
        'memberFunction',
        '0x006F45D4',
        '0x00644150',
        '0x00635509',
        'Start-Sleep -Milliseconds 1500',
        'Start-Sleep -Seconds 2'
    )) {
        if (-not $gameplaySource.Contains($literal)) {
            throw "static check: gameplay helper lost exact old auto-battle token '$literal'"
        }
    }
    foreach ($literal in @(
        "'source-route-control'",
        "'source-pair-repro'",
        "'clean-host-route-control'",
        "'clean-join-route-control'",
        "'clean-long-concurrency'",
        "`$LongMoveCase = 'clean-long-concurrency'",
        '-Case $LongMoveCase',
        "if (`$Case -eq 'source-route-control')",
        "if (`$Case -eq 'clean-host-route-control')",
        "if (`$Case -eq 'clean-join-route-control')",
        "if (`$Case -eq 'source-pair-repro')",
        'return Invoke-LongMoveSourceRouteControl',
        'return Observe-LongMoveSourcePair',
        'historical longMove is diagnostic-only',
        'longMoveCase = if ($GameplayMode'
    )) {
        if (-not $source.Contains($literal) -and
            -not $gameplaySource.Contains($literal)) {
            throw "static check: long-move case wiring lost '$literal'"
        }
    }
    if ($gameplaySource.Contains('/api/state?role=')) {
        throw 'static check: exact UI proof regressed to aggregate /api/state with an illegal role query'
    }
    foreach ($forbidden in @(
        '.Cancel(',
        '.CancelAfter(',
        'Start-CanonicalAutoBattleRequestsIndependently'
    )) {
        if ($gameplaySource.Contains($forbidden)) {
            throw "static check: gameplay helper regained forbidden auto-battle path '$forbidden'"
        }
    }
    $canonicalBattleLifecycleSource = [regex]::Match(
        $gameplaySource,
        '(?ms)^function Invoke-CanonicalBattleLifecyclesIndependently.*?(?=^function Get-LegacySequentialSharedMovementSnapshot)'
    ).Value
    if ([string]::IsNullOrEmpty($canonicalBattleLifecycleSource)) {
        throw 'static check: canonical battle lifecycle is not independently identifiable'
    }
    $uiReadyWaitSource = [regex]::Match(
        $helperSource,
        '(?ms)^function Wait-UiButtonReadyPublication\b.*?(?=^function Get-WorldHistorySnapshot)'
    ).Value
    if ([string]::IsNullOrEmpty($uiReadyWaitSource) -or
        [regex]::Matches($uiReadyWaitSource, 'Invoke-RestMethod').Count -ne 1 -or
        $uiReadyWaitSource -match
            'Invoke-Button|Start-SimturnGameClient|Start-Sleep|\bwhile\s*\(|\bfor\s*\(|-Method\s+POST') {
        throw 'static check: pre-join UI-ready helper must remain one passive GET with no loop/action/relaunch'
    }
    $worldPairWaitSource = [regex]::Match(
        $helperSource,
        '(?ms)^function Wait-ExactWorldPairPublication\b.*?(?=^function Get-TurnHistory)'
    ).Value
    $worldPairForbiddenLanguage =
        'Move-Stack|Invoke-Button|Get-World|Get-RoleState|Start-Sleep|' +
        '\bwhile\s*\(|-Method\s+POST|(?i)\b(re' + 'fire|re' + 'try|fall' + 'back)\b'
    if ([string]::IsNullOrEmpty($worldPairWaitSource) -or
        [regex]::Matches($worldPairWaitSource, 'Invoke-RestMethod').Count -ne 1 -or
        $worldPairWaitSource -match $worldPairForbiddenLanguage) {
        throw 'static check: world-pair helper must remain one passive event GET with no poll/action repetition'
    }
    foreach ($literal in @(
        '/api/world/wait-exact-pair?hostAfter=',
        'HostAfterWorldSequence',
        'JoinAfterWorldSequence',
        'ExpectedHostProcessId',
        'ExpectedJoinProcessId',
        'ExpectedHostModulePath',
        'ExpectedJoinModulePath'
    )) {
        if (-not $worldPairWaitSource.Contains($literal)) {
            throw "static check: exact world-pair helper lost '$literal'"
        }
    }
    foreach ($literal in @(
        'Get-GameUiSnapshot $role',
        '$battleUpLines = @(Read-ClientLogLines',
        '$battleCloseLines = @(Read-ClientLogLines',
        'Get-LegacyBattleClosedSnapshot',
        '$maxSteps = 6',
        'Move-Stack $role',
        'finally {'
    )) {
        if (-not $canonicalBattleLifecycleSource.Contains($literal)) {
            throw "static check: canonical literal battle lifecycle lost '$literal'"
        }
    }
    if ([regex]::Matches(
            $canonicalBattleLifecycleSource,
            'Start-Sleep\s+-Seconds\s+2').Count -ne 2) {
        throw 'static check: canonical battle lifecycle lost terminal-iteration +2 and post-loop +2'
    }
    foreach ($forbidden in @(
        'Start-ExactAutoBattleRequest',
        'Complete-ExactAutoBattleRequests',
        '/api/ui/enable-auto-battle',
        '.task.IsCompleted',
        'Read-PrearmedAutoBattleProof',
        'New-CanonicalBattleCompletionState',
        'Step-CanonicalBattleCompletionState'
    )) {
        if ($canonicalBattleLifecycleSource.Contains($forbidden)) {
            throw "static check: canonical battle lifecycle regained late action '$forbidden'"
        }
    }
    $battleBlockPlanSource = [regex]::Match(
        $gameplaySource,
        '(?ms)^function Get-LegacyBattleBlockAttackPlan\(.*?(?=^function Assert-LegacyBattleBlockAttackPlanMatchesFixture)'
    ).Value
    $reverseAttackPlanSource = [regex]::Match(
        $gameplaySource,
        '(?ms)^function Get-LegacyCrossBattleReverseAttackPlan\(.*?(?=^function Assert-LegacyCrossBattleReverseAttackPlan)'
    ).Value
    $reverseHostMovePlanSource = [regex]::Match(
        $gameplaySource,
        '(?ms)^function Get-LegacyCrossBattleHostMovePlan\(.*?(?=^function Assert-LegacyCrossBattleHostMovePlan)'
    ).Value
    $crossBattleWorldStateSource = [regex]::Match(
        $gameplaySource,
        '(?ms)^function Test-ExactWorldStackPositionAndMovement\b.*?(?=^function Wait-ExactCrossBattleHostMoveWorld)'
    ).Value
    $crossBattleWorldWaitSource = [regex]::Match(
        $gameplaySource,
        '(?ms)^function Wait-ExactCrossBattleHostMoveWorld\b.*?(?=^function Invoke-BattleBlockProof)'
    ).Value
    $battleBlockProofSource = [regex]::Match(
        $gameplaySource,
        '(?ms)^function Invoke-BattleBlockProof\(.*\z'
    ).Value
    $clientLaunchSource = [regex]::Match(
        $source,
        '(?ms)^function Start-SimturnGameClient\(.*?(?=^function Select-Settle)'
    ).Value
    if ([string]::IsNullOrEmpty($battleBlockPlanSource) -or
        [string]::IsNullOrEmpty($reverseAttackPlanSource) -or
        [string]::IsNullOrEmpty($reverseHostMovePlanSource) -or
        [string]::IsNullOrEmpty($crossBattleWorldStateSource) -or
        [string]::IsNullOrEmpty($crossBattleWorldWaitSource) -or
        [string]::IsNullOrEmpty($battleBlockProofSource) -or
        [string]::IsNullOrEmpty($clientLaunchSource)) {
        throw 'static check: forward/reverse battle-block plans, proof, or launch source is not independently identifiable'
    }
    $hostStkOffset = $battleBlockPlanSource.IndexOf(
        '$hostHeroWorld = Get-LegacyStackSnapshot', [StringComparison]::Ordinal)
    $freshNeutralOffset = $battleBlockPlanSource.IndexOf(
        '$neutralWorld = Get-LegacyStackSnapshot', [StringComparison]::Ordinal)
    if ($hostStkOffset -lt 0 -or $freshNeutralOffset -le $hostStkOffset -or
        [regex]::Matches(
            $battleBlockPlanSource, 'Get-LegacyStackSnapshot').Count -ne 2 -or
        $battleBlockPlanSource -match
            'Get-WorldSnapshot|Get-World\s|Get-RelayState|Get-RoleState|Wait-|Start-Sleep') {
        throw 'static check: battle-block attack must remain host Stk then one fresh shared neutral census'
    }
    $planCallOffset = $battleBlockProofSource.IndexOf(
        '$legacyAttackPlan = Get-LegacyBattleBlockAttackPlan $Fixture',
        [StringComparison]::Ordinal)
    $hostFireOffset = $battleBlockProofSource.IndexOf(
        'if (-not (Move-Stack host ([string]$legacyAttackPlan.id)',
        [StringComparison]::Ordinal)
    if ($planCallOffset -lt 0 -or $hostFireOffset -le $planCallOffset -or
        $battleBlockProofSource.IndexOf(
            '$hostMapBinding = Get-MapActionTargetBinding host',
            [StringComparison]::Ordinal) -ge $planCallOffset) {
        throw 'static check: two battle-block shared reads must immediately drive the sole bound host Fire'
    }
    $planToFire = $battleBlockProofSource.Substring(
        $planCallOffset, $hostFireOffset - $planCallOffset)
    if ($planToFire -match
            'Assert-|Get-(?!LegacyBattleBlockAttackPlan)|Read-|Wait-|Start-Sleep|Invoke-RestMethod') {
        throw 'static check: an observation or fixture assertion was inserted between source censuses and host Fire'
    }
    if ([regex]::Matches(
            $battleBlockProofSource,
            '\[int\]\$sourceStepCount\s*=\s*4').Count -ne 1 -or
        [regex]::Matches(
            $battleBlockProofSource,
            'for \(\$i = 1; \$i -le \$sourceStepCount; \$i\+\+\)').Count -ne 1) {
        throw 'static check: battle-block must execute exactly the proved four source iterations'
    }
    $battleBlockStepBegin = $battleBlockProofSource.IndexOf(
        'LITERAL_BATTLE_BLOCK_STEP_BEGIN', [StringComparison]::Ordinal)
    $battleBlockStepEnd = $battleBlockProofSource.IndexOf(
        'LITERAL_BATTLE_BLOCK_STEP_END', $battleBlockStepBegin,
        [StringComparison]::Ordinal)
    if ($battleBlockStepBegin -lt 0 -or
        $battleBlockStepEnd -le $battleBlockStepBegin) {
        throw 'static check: battle-block source step window is not identifiable'
    }
    $battleBlockStepSource = $battleBlockProofSource.Substring(
        $battleBlockStepBegin, $battleBlockStepEnd - $battleBlockStepBegin)
    if ([regex]::Matches(
            $battleBlockStepSource, 'Move-Stack join').Count -ne 1 -or
        [regex]::Matches(
            $battleBlockStepSource, 'Wait-WorldEvidence').Count -ne 1 -or
        $battleBlockStepSource -notmatch (
            '(?s)\$joinWorldBeforeStepSequence = Get-RoleEvidenceSequence join world[\s\S]*' +
            'Move-Stack join[\s\S]*Start-Sleep -Milliseconds 1500[\s\S]*' +
            '\$movedAtFixedSample[\s\S]*\$moved = \$movedAtFixedSample[\s\S]*' +
            '\$expectedMovementAfter = \[int\]\$before\.movement - 3[\s\S]*' +
            'Wait-WorldEvidence -Role join[\s\S]*' +
            '-After \$joinWorldBeforeStepSequence[\s\S]*' +
            '\[int\]\$hero\.x -eq \(\$jx \+ 1\)[\s\S]*' +
            '\[int\]\$hero\.y -eq \$jy[\s\S]*' +
            '\[int\]\$hero\.movement -eq \$expectedMovementAfter[\s\S]*' +
            'if \(-not \$movedAtFixedSample\)[\s\S]*' +
            '\$joinAfterWorld = \$joinSettledWorld[\s\S]*' +
            '\$hostUiAfterStep')) {
        throw ('static check: battle-block must retain +1500 source sampling and ' +
            'one exact causal join-world fence for every already-issued move')
    }
    $battleBlockVerdictBoundary = $battleBlockProofSource.IndexOf(
        'LITERAL_BATTLE_BLOCK_VERDICT_BOUNDARY', [StringComparison]::Ordinal)
    $legacyFreeGateOffset = $battleBlockProofSource.IndexOf(
        "if (`$legacyVerdict -ne 'FREE')", [StringComparison]::Ordinal)
    if ($legacyFreeGateOffset -lt 0 -or
        $battleBlockVerdictBoundary -le $legacyFreeGateOffset -or
        -not $battleBlockProofSource.Contains(
            '$legacyVerdict = if ($freeSteps -gt 0)')) {
        throw 'static check: legacy battle-block FREE (>0) must pass before deferred MSS strengthening'
    }
    $battleBlockDeferredSource = $battleBlockProofSource.Substring(
        $battleBlockVerdictBoundary)
    foreach ($literal in @(
        'Assert-LegacyBattleBlockAttackPlanMatchesFixture',
        '$legacySteps.Count -ne $sourceStepCount',
        'commandIssued = $true',
        'moved = [bool]$moved',
        'hostInBattleBefore = [bool]$inBat',
        'hostInBattleAfter = [bool]$hostBat',
        '$recountedFreeSteps -ne $freeSteps',
        '$recountedAfterSteps -ne $afterSteps',
        '$continuousFreeSteps -ne $sourceStepCount',
        'Read-PrearmedAutoBattleProof',
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
        'sourceHandoff = [pscustomobject]@{',
        'joinBattleCompletionAttempted = $false'
    )) {
        if (-not $battleBlockProofSource.Contains($literal)) {
            throw "static check: deferred battle-block MSS strengthening lost '$literal'"
        }
    }
    if (-not $battleBlockDeferredSource.Contains(
            '$freeSteps -ne $sourceStepCount') -or
        -not $battleBlockDeferredSource.Contains('$afterSteps -ne 0') -or
        -not $battleBlockDeferredSource.Contains(
            '$continuousFreeSteps -ne $sourceStepCount')) {
        throw ('static check: battle-block deferred acceptance must require ' +
            'continuous FREE four-of-four and zero after-battle moves')
    }
    if ($battleBlockProofSource.Contains('Invoke-ParallelExactAutoBattle') -or
        $battleBlockProofSource.Contains('/api/ui/enable-auto-battle') -or
        $battleBlockProofSource.Contains('Get-ExactAutoBattleBinding')) {
        throw 'static check: battle-block regained a late auto-battle action'
    }
    $reverseBegin = $battleBlockProofSource.IndexOf(
        'LITERAL_CROSS_BATTLE_REVERSE_BEGIN', [StringComparison]::Ordinal)
    $reverseVerdict = $battleBlockProofSource.IndexOf(
        'LITERAL_CROSS_BATTLE_REVERSE_VERDICT_BOUNDARY', [StringComparison]::Ordinal)
    $forwardHandoffBegin = $battleBlockProofSource.IndexOf(
        'MSS_CROSS_BATTLE_FORWARD_HANDOFF_BEGIN', [StringComparison]::Ordinal)
    $forwardHandoffVerdict = $battleBlockProofSource.IndexOf(
        'MSS_CROSS_BATTLE_FORWARD_HANDOFF_VERDICT_BOUNDARY',
        [StringComparison]::Ordinal)
    $forwardStressBegin = $battleBlockProofSource.IndexOf(
        'MSS_CROSS_BATTLE_FORWARD_STRESS_BEGIN', [StringComparison]::Ordinal)
    $forwardStressVerdict = $battleBlockProofSource.IndexOf(
        'MSS_CROSS_BATTLE_FORWARD_STRESS_VERDICT_BOUNDARY',
        [StringComparison]::Ordinal)
    if ($forwardHandoffBegin -le $battleBlockVerdictBoundary -or
        $forwardHandoffVerdict -le $forwardHandoffBegin -or
        $forwardStressBegin -le $forwardHandoffVerdict -or
        $forwardStressVerdict -le $forwardStressBegin -or
        $reverseBegin -le $forwardStressVerdict -or
        $reverseVerdict -le $reverseBegin) {
        throw 'static check: forward/reverse cross-battle boundaries are missing or out of order'
    }
    $forwardHandoffSource = $battleBlockProofSource.Substring(
        $forwardHandoffBegin, $forwardHandoffVerdict - $forwardHandoffBegin)
    $forwardHandoffForbiddenLanguage =
        'Wait-WorldEvidence|Get-World\s|Start-Sleep|Move-Stack|Invoke-Button|' +
        'Complete-CanonicalBattle|(?i)\b(re' + 'fire|re' + 'try|fall' + 'back)\b'
    if ([regex]::Matches(
            $forwardHandoffSource,
            'Wait-ExactWorldPairPublication').Count -ne 1 -or
        $forwardHandoffSource -notmatch
            '(?s)\$sourceEndpoint\.toX -ne 19.*\$sourceEndpoint\.toY -ne 27.*\$sourceEndpoint\.movementAfter -ne 23.*\$firstStressStep\.fromX.*\$firstStressStep\.movementBefore' -or
        $forwardHandoffSource -notmatch
            '(?s)Get-ExactLiveBattleUiProof.*Wait-ExactWorldPairPublication.*Get-ExactLiveBattleUiProof.*Get-UiHistory.*sourceHandoffUiCursor -ne' -or
        $forwardHandoffSource -match $forwardHandoffForbiddenLanguage) {
        throw 'static check: source-to-stress handoff lost its one exact zero-poll world-pair subscription'
    }
    $sourceRoleOwnerOffset = $battleBlockProofSource.IndexOf(
        '$sourceRoleStates = [ordered]@{', [StringComparison]::Ordinal)
    $sourceLoopOffset = $battleBlockProofSource.IndexOf(
        'for ($i = 1; $i -le $sourceStepCount; $i++)',
        [StringComparison]::Ordinal)
    if ($sourceRoleOwnerOffset -lt 0 -or
        $sourceRoleOwnerOffset -ge $sourceLoopOffset -or
        $sourceLoopOffset -ge $battleBlockVerdictBoundary) {
        throw 'static check: role-world owners/watermarks must precede the immutable FREE4 loop'
    }
    $reverseSource = $battleBlockProofSource.Substring(
        $reverseBegin, $reverseVerdict - $reverseBegin)
    foreach ($literal in @(
        'Complete-CanonicalBattle',
        '-Role host',
        'Start-Sleep -Seconds 3',
        'Get-LegacyCrossBattleReverseAttackPlan',
        'Assert-LegacyCrossBattleReverseAttackPlan',
        'Wait-UiEvidence -Role join',
        'Get-ExactLiveBattleUiProof',
        'Get-LegacyCrossBattleHostMovePlan',
        'Assert-LegacyCrossBattleHostMovePlan',
        'Get-World join',
        'Wait-ExactCrossBattleHostMoveWorld -Role join',
        'Start-Sleep -Milliseconds 1500',
        'Get-UiHistory join',
        'Wait-ExactCrossBattleHostMoveWorld -Role host',
        'same exact live join battle on both client worlds'
    )) {
        if (-not $reverseSource.Contains($literal)) {
            throw "static check: reverse cross-battle continuation lost '$literal'"
        }
    }
    foreach ($literal in @(
        'Wait-WorldEvidence',
        'Test-ExactWorldStackPositionAndMovement',
        'timed out waiting for $Role world evidence:',
        'Get-World $Role',
        'last MP=',
        'expected pinned MP=',
        'stack/unit=',
        'ConvertTo-Json -Compress -Depth 4'
    )) {
        if (-not $crossBattleWorldWaitSource.Contains($literal)) {
            throw "static check: reverse FreeAdj MP mismatch diagnostic lost '$literal'"
        }
    }
    if ([regex]::Matches(
            $reverseSource, 'Wait-ExactCrossBattleHostMoveWorld').Count -ne 2 -or
        $crossBattleWorldStateSource -notmatch
            '(?s)\[int\]\$hero\.x -eq \$X.*\[int\]\$hero\.y -eq \$Y.*\[int\]\$hero\.movement -eq \$Movement' -or
        $crossBattleWorldWaitSource -match
            'Move-Stack|Invoke-Button|Complete-CanonicalBattle|(?i)\b(re' +
            'fire|re' + 'try|fall' + 'back)\b') {
        throw 'static check: reverse FreeAdj exact world waits must remain passive and role-complete'
    }
    $hostExhaustionBegin = $battleBlockProofSource.IndexOf(
        'MSS_CROSS_BATTLE_HOST_EXHAUSTION_BEGIN', [StringComparison]::Ordinal)
    $hostExhaustionVerdict = $battleBlockProofSource.IndexOf(
        'MSS_CROSS_BATTLE_HOST_EXHAUSTION_VERDICT_BOUNDARY',
        [StringComparison]::Ordinal)
    if ($hostExhaustionBegin -le $reverseVerdict -or
        $hostExhaustionVerdict -le $hostExhaustionBegin) {
        throw 'static check: host MP-exhaustion boundaries are missing or out of order'
    }
    $afterFinalStressVerdict = $battleBlockProofSource.Substring(
        $hostExhaustionVerdict)
    if ([regex]::Matches($battleBlockProofSource, 'Complete-CanonicalBattle').Count -ne 1 -or
        [regex]::Matches($battleBlockProofSource, 'Move-Stack join').Count -ne 2 -or
        [regex]::Matches($battleBlockProofSource, 'Move-Stack host').Count -ne 2 -or
        [regex]::Matches($reverseSource, 'Start-Sleep\s+-Seconds\s+3').Count -ne 1 -or
        [regex]::Matches(
            $reverseSource, 'Start-Sleep\s+-Milliseconds\s+1500').Count -ne 1 -or
        [regex]::Matches($reverseSource, 'Complete-CanonicalBattle').Count -ne 1 -or
        [regex]::Matches($reverseSource, 'Move-Stack join').Count -ne 1 -or
        [regex]::Matches($reverseSource, 'Move-Stack host').Count -ne 1 -or
        $reverseSource.Contains('Complete-CanonicalBattle -Role join') -or
        $afterFinalStressVerdict -match
            '(?i)\b(?:Move|Invoke|Complete|Start|Wait|Set|Enable|Disable)-[A-Za-z]' -or
        $battleBlockProofSource.Contains('Wait-CanonicalWorldConvergence')) {
        throw ('static check: complete proof must retain two forward/reverse call sites, ' +
            'source +3/+1500 windows, two exact-MP worlds, no post-stress command, ' +
            'and leave join battle live')
    }
    if ($battleBlockProofSource -notmatch
            '(?s)Move-Stack join \(\[string\]\$Fixture\.join\.heroId\).*?\$joinMapBinding\.Instance \$joinMapBinding\.Appearance\s*`\s*\(\[int\]\$before\.movement\)' -or
        $reverseSource -notmatch
            '(?s)Move-Stack host \(\[string\]\$reverseHostMovePlan\.id\).*?\$hostMapBinding\.Instance \$hostMapBinding\.Appearance\s*`\s*\(\[int\]\$reverseHostMovePlan\.movementBefore\)') {
        throw 'static check: source FREE4/FreeAdj moves lost their exact causal MP tokens'
    }
    if (-not $clientLaunchSource.Contains(
            "if (`$GameplayMode -in @('canonical', 'battle-block', 'long-attack'))") -or
        [regex]::Matches(
            $clientLaunchSource, 'D2TESTDRV_AUTO_BATTLE_PREARM').Count -ne 1 -or
        -not $clientLaunchSource.Contains('$psi.FileName = $launchExecutablePath') -or
        -not $clientLaunchSource.Contains(
            '-NotePropertyName D2MssLaunchExecutablePath') -or
        $clientLaunchSource -match '\$process\.Path\b') {
        throw 'static check: client launch lost preboot auto-battle or executable provenance'
    }
    if (-not $source.Contains(
            '$fixture $hostProcess $joinProcess $hostLog')) {
        throw 'static check: battle-block proof must receive the exact owned host PID log'
    }
    foreach ($literal in @(
        'D2MSS_SIMTURNS',
        'D2MSS_SIMTURNS_ROLE',
        'D2MSS_SIMTURNS_PIPE',
        'D2TESTDRV_ROLE',
        'D2TESTDRV_DIRECTPLAY_HOST',
        'D2TESTDRV_FIXTURE_PLAN',
        'D2TESTDRV_APPLY_FIXTURE',
        'D2TESTDRV_AUTO_BATTLE_PREARM',
        'D2TESTDRV_TURN_EVENTS',
        'D2TESTDRV_EXACT_LEGACY_MOVES',
        'D2TESTDRV_CLEAN_LONG_MOVES',
        'Get-TurnHistory',
        'ScenarioPath',
        'Set-ScenarioSelection',
        'GameplayMode',
        'BarrierOrder',
        'Invoke-OrderedMergeEndTurnsAndWaitAccepted',
        'Invoke-CanonicalDay2Walk',
        'Invoke-BattleBlockProof',
        'Invoke-LongMoveProof',
        'Invoke-LongAttackProof',
        'stock-end-turn-send-returned',
        'stock-begin-turn-applied',
        'stock-startup-begin-turn-observed',
        'stock-startup-join-game-observed',
        'stock-startup-directed-begin-turn-observed',
        'stock-startup-complete-observed',
        'BTN_END_TURN',
        'session-plan-created',
        'session-plan-delivered',
        'session-activated',
        'bootstrap-begin-turn-applied',
        'bootstrap-cascade-complete',
        'bootstrap-turn-info-applied',
        'bootstrap-commit-dispatched',
        'bootstrap-commit-applied',
        'bootstrap-operational-dispatched',
        'bootstrap-operational-applied',
        'session-operational',
        'end-turn-observed',
        'end-turn-applied',
        'end-turn-accepted',
        'bootstrap operational release applied; strict independent turns are operational',
        'concurrent-battle compatibility patches active (0x635578=NOP2, 0x638886=NOP7)',
        '55FC74',
        'turn-start-complete',
        '[nettrace] EnumSessions ready on next natural UI frame generation=',
        '[nettrace] peer CConnectMsg observed self=',
        '$lane.Search.CompletedUtc.AddMilliseconds(2000)',
        'Wait-ClientLogMarkerEventUtc',
        '$waitPeerReleasedUtc = [DateTime]$waitPeerEvidence.eventUtc',
        '$sessionListReadyAfter = Get-ClientLogMarkerCount',
        'join DirectPlay session enumeration marker delta at +2000 ms',
        'D2TESTDRV_RELAY_BRIDGE',
        'ClientLogInitialLengths',
        'Assert-ProductionRelayClientIdentity',
        'modulePath',
        'Wait-ClientLogMarkerDelta',
        'merge-prepare-dispatched',
        'merge-prepare-applied',
        'merge-execute-dispatched',
        'merge-execute-applied',
        'merge-applied',
        'natural merge BeginTurn applied and drained',
        'relay released stock turns',
        'start.ps1 gave the relay one literal +1500 ms head start',
        'Wait-FixedUtcAnchor ($hostLaunchUtc.AddMilliseconds(10000))',
        '$hostJoinLaunchDeadlineUtc = $hostLaunchUtc.AddSeconds($BootTimeoutSec)',
        '$hostPreJoinMainMenu = Wait-UiButtonReadyPublication',
        '$legacyInjectionUtc = $LaunchUtc.AddMilliseconds(1500)',
        '$targetUtc = $legacyInjectionUtc.AddMilliseconds(11000)',
        'Wait-FixedUtcAnchor ($joinLaunchUtc.AddMilliseconds(500))',
        'literal ten read-only checks on ten subsequent natural UI frames',
        'same native ten-frame',
        'Literal WaitPeer: release on the host RX observer',
        'kHostScript: exactly +500 ms after BTN_OK',
        '$hostPopupService = New-LiteralStartupPopupService host',
        'host 25000 $hostPopupService',
        'kJoinScript is two distinct popup timer windows',
        '$joinPopupService = New-LiteralStartupPopupService join',
        '$Timeline.JoinEntryWindow.CompletedTick + 5000',
        'join 20000 $Timeline.JoinPopupService',
        '$Timeline.HostWindow.CompletedTick + 2000',
        '$script:StartupModalSettleMilliseconds = 300',
        "'SCRIPTED_POPUPS'",
        'D2TESTDRV_SCRIPTED_POPUPS_CONFIRMATIONS',
        '[testdrv][scripted-popup]',
        '$Window.FirstActionTick = [long]$firstClaim[0].Tick',
        'Start-LiteralSelectionRequest',
        'Wait-LiteralSelectionOrUtcAnchor',
        'Start-LiteralButtonRequest',
        'Complete-LiteralIndependentNavigation',
        'Invoke-LiteralHostNavigationLane',
        'Invoke-LiteralJoinNavigationLane',
        'Start-LiteralNavigationWorker',
        'Complete-LiteralNavigationWorker',
        'BriefingLatch',
        'StrategicLatch',
        'Start-LiteralTurnHistoryRequest',
        'Complete-LiteralTurnHistoryRequest',
        'Add-LiteralJoinStockStartupHistory',
        'Start-LiteralOuterReadyObserver',
        'Publish-LiteralOuterReadyStartupObserver',
        'Test-LiteralReadyWorldSnapshot',
        '$MaxSamples = 120',
        'Resolve-LiteralHostAuthoritativeHeroes',
        'Wait-LiteralDayReady -QuietSec 3 -TimeoutSec 60',
        '$nextObservationUtc = [DateTime]::UtcNow.AddMilliseconds(800)',
        '-SnapshotOnly',
        'Wait-NewValidatedJoinStockStartupObserved',
        'Wait-NewValidatedJoinStockStartupComplete',
        'Stop-OwnedProcess'
    )) {
        if (-not $source.Contains($literal)) {
            throw "static check: runner lost required contract token '$literal'"
        }
    }
    if ($source -match 'Stop-Process\s+-Name\s+(Discipl2|node|dplaysvr)') {
        throw 'static check: broad process teardown is forbidden; retain owned Process handles'
    }
    $forbiddenActionLanguage = '(?i)\b(re' + 'fire|re' + 'try|fall' + 'back)\b'
    if ($source -match $forbiddenActionLanguage) {
        throw 'static check: production PoC must keep UI actions exact-once'
    }

    $runtimeStart = $source.LastIndexOf(
        'if ($ProbeRelayFailure -and $MergeDay -ne 0)',
        [StringComparison]::Ordinal)
    if ($runtimeStart -lt 0) {
        throw 'static check: runtime source boundary is not identifiable'
    }
    $runtimeSource = $source.Substring($runtimeStart)
    foreach ($literal in @(
        '$script:StartupModalSettleMilliseconds = 300',
        'Wait-LiteralDayReady -QuietSec 3 -TimeoutSec 60'
    )) {
        if (-not $runtimeSource.Contains($literal)) {
            throw "static check: literal startup runtime lost '$literal'"
        }
    }

    $machineGateOffset = $runtimeSource.IndexOf(
        'Assert-ExclusiveTestMachine -GameDir $GameDir',
        [StringComparison]::Ordinal)
    $relayStartOffset = $runtimeSource.IndexOf(
        '$testRelay = Start-TestRelay -LogDir $ArtifactDir',
        $machineGateOffset,
        [StringComparison]::Ordinal)
    if ($machineGateOffset -lt 0 -or $relayStartOffset -le $machineGateOffset) {
        throw 'static check: exact machine-gate/pre-relay boundary is not identifiable'
    }
    $legacyPreRelayClockSource = $runtimeSource.Substring(
        $machineGateOffset,
        $relayStartOffset - $machineGateOffset)
    $m0Order = @(
        'Assert-ExclusiveTestMachine -GameDir $GameDir',
        'Start-Sleep -Milliseconds 1200',
        '$literalReadyObserver = Start-LiteralOuterReadyObserver',
        'Start-Sleep -Milliseconds 1200',
        'Start-Sleep -Milliseconds 500',
        '$literalModWrapperHandoff = [pscustomobject]@{',
        'Start-Sleep -Milliseconds 1200',
        'Start-Sleep -Milliseconds 500',
        '$literalNestedStartupSubstitution = Get-LiteralNestedStartupSubstitutionMap',
        'Start-Sleep -Milliseconds 1200',
        'Start-Sleep -Milliseconds 300',
        '$literalStartWrapperHandoff = [pscustomobject]@{'
    )
    $previousM0Offset = -1
    foreach ($literal in $m0Order) {
        $offset = $legacyPreRelayClockSource.IndexOf(
            $literal, $previousM0Offset + 1, [StringComparison]::Ordinal)
        if ($offset -le $previousM0Offset) {
            throw "static check: M0 wrapper/clock order lost at '$literal'"
        }
        $previousM0Offset = $offset
    }
    if ([regex]::Matches($legacyPreRelayClockSource,
            'Start-Sleep\s+-Milliseconds\s+1200').Count -ne 4 -or
        [regex]::Matches($legacyPreRelayClockSource,
            'Start-Sleep\s+-Milliseconds\s+500').Count -ne 2 -or
        [regex]::Matches($legacyPreRelayClockSource,
            'Start-Sleep\s+-Milliseconds\s+300').Count -ne 1) {
        throw 'static check: M0 must retain four +1200 and distinct +500/+500/+300 clock edges'
    }

    $nestedStartupMapSource = [regex]::Match(
        $source,
        '(?ms)^function Get-LiteralNestedStartupSubstitutionMap\s*\{.*?(?=^function Measure-LiteralSavedLogMarker)'
    ).Value
    foreach ($literal in @(
        'LegacyRolePollMilliseconds = 500',
        'direct propagated startup/pairing exception replaces FAIL: roles never settled bootlog observation',
        'LegacyStrategicPollMilliseconds = 400',
        'LegacyPostStrategicSettleSeconds = 2',
        'LegacyJoinActivationToCascadeMilliseconds = 500',
        'LegacyCascadeToPacketBaselineMilliseconds = 800',
        'one release file -> session-plan-created + session-plan-delivered(host/join) -> host/join session activation + directed day-1 BeginTurn apply',
        'one +500 ms timer is armed at the exact BootstrapBeginTurnApplied completion edge; host activation must arrive before its fixed deadline',
        'engine-action-dispatched(stage=bootstrap-apply) -> bootstrap-cascade-complete + bootstrap-turn-info-applied',
        'bootstrap-commit-dispatched -> bootstrap-commit-applied(host/join) -> bootstrap-operational-dispatched -> bootstrap-operational-applied(host/join) -> session-operational',
        'literal two-read Begin-then-End host TX census after the fixed cascade +800 ms edge',
        'literal legacy observer/read projection with one typed v8 MSS release-edge substitution; MSS evidence follows old verdict boundaries'
    )) {
        if ([string]::IsNullOrEmpty($nestedStartupMapSource) -or
            -not $nestedStartupMapSource.Contains($literal)) {
            throw "static check: nested startup substitution map lost '$literal'"
        }
    }
    if ($nestedStartupMapSource -match 'Invoke-RestMethod|/api/|Start-Sleep|Get-RoleState|Get-RelayState') {
        throw 'static check: nested startup map must remain a pure description of the literal observer/MSS release mapping'
    }

    $selectSettleSource = [regex]::Match(
        $source,
        '(?ms)^function Select-Settle\(.*?(?=^function Select-ScenarioPathSettle)'
    ).Value
    $selectScenarioSource = [regex]::Match(
        $source,
        '(?ms)^function Select-ScenarioPathSettle\(.*?(?=^# Start one exact native ten-frame selection)'
    ).Value
    if ([string]::IsNullOrEmpty($selectSettleSource) -or
        [string]::IsNullOrEmpty($selectScenarioSource)) {
        throw 'static check: literal native selection wrappers are not identifiable'
    }
    foreach ($contract in @(
        @{ source = $selectSettleSource; tokens = @(
            'Set-ListSelection does not resolve until the native driver has performed',
            'literal ten read-only checks on ten subsequent natural UI frames',
            'do not add a second time-based settle or repeat the mutation here') },
        @{ source = $selectScenarioSource; tokens = @(
            'Set-ScenarioSelection $Role $Dialog $ListBox $ExactPath',
            'completed the same native ten-frame',
            'BTN_LOAD must consume the proven row directly') }
    )) {
        foreach ($literal in $contract.tokens) {
            if (-not $contract.source.Contains($literal)) {
                throw "static check: native selection-ten-frame contract lost '$literal'"
            }
        }
    }

    $oneShotTransitionSource = [regex]::Match(
        $source,
        '(?ms)^function Invoke-OneShotTransition\(.*?(?=^function Wait-StableReadyButtonObservation)'
    ).Value
    if ([string]::IsNullOrEmpty($oneShotTransitionSource) -or
        -not $oneShotTransitionSource.Contains(
            '[void](Get-ReadyActionTarget $candidate $Dialog $capturedOwner)')) {
        throw 'static check: captured selection must retain exact owner/appearance admission before its one-shot button'
    }
    if ($oneShotTransitionSource.Contains('Get-ReadyListBoxState') -or
        $oneShotTransitionSource.Contains("'SelectionIndex'") -or
        $oneShotTransitionSource.Contains("'SelectionTotal'")) {
        throw 'static check: native ten-frame selection proof must not be followed by a sticky relay listbox reread'
    }

    $mainMenuAnchorSource = [regex]::Match(
        $source,
        '(?ms)^function Get-LiteralMainMenuAtLaunchAnchor\(.*?(?=^function Invoke-OneShotTransition)'
    ).Value
    if ([string]::IsNullOrEmpty($mainMenuAnchorSource) -or
        -not $mainMenuAnchorSource.Contains('$legacyInjectionUtc = $LaunchUtc.AddMilliseconds(1500)') -or
        -not $mainMenuAnchorSource.Contains('$targetUtc = $legacyInjectionUtc.AddMilliseconds(11000)') -or
        -not $mainMenuAnchorSource.Contains('Wait-FixedUtcAnchor $targetUtc') -or
        -not $mainMenuAnchorSource.Contains('while ([DateTime]::UtcNow -lt $DeadlineUtc)') -or
        -not $mainMenuAnchorSource.Contains('$isHelloBeforeFirstUiPublication') -or
        -not $mainMenuAnchorSource.Contains('Get-LiteralNavigationRoleState') -or
        -not $mainMenuAnchorSource.Contains('-Context "$Role/main-menu-anchor"') -or
        -not $mainMenuAnchorSource.Contains(
            'ConvertTo-LiteralNavigationDialogObservation') -or
        -not $mainMenuAnchorSource.Contains('$observation.Dialog -eq ''DLG_MAIN_MENU''') -or
        $mainMenuAnchorSource -match
            'Get-RoleState|Get-DialogObservation|Start-LiteralButtonRequest|' +
            'Invoke-Button|/api/ui/invoke') {
        throw 'static check: per-client delayed-injection+11000 ms main-menu anchor was lost'
    }

    $literalDayReadySource = [regex]::Match(
        $source,
        '(?ms)^function Wait-LiteralDayReady\(.*?(?=^# Read-only compatibility observer)'
    ).Value
    if ([string]::IsNullOrEmpty($literalDayReadySource) -or
        [regex]::Matches(
            $literalDayReadySource,
            '\$nextObservationUtc = \[DateTime\]::UtcNow\.AddMilliseconds\(800\)').Count -ne 1 -or
        [regex]::Matches(
            $literalDayReadySource,
            'Wait-FixedUtcAnchor\s+\$nextObservationUtc').Count -ne 1 -or
        [regex]::Matches(
            $literalDayReadySource,
             'Wait-FixedUtcAnchorWithPopupService').Count -ne 0) {
        throw 'static check: Wait-LiteralDayReady lost its exact passive 800 ms observer cadence'
    }
    $legacyHostRoleReadOffset = $literalDayReadySource.IndexOf(
        '$legacyHostRoleState = Get-RoleState host', [StringComparison]::Ordinal)
    $legacyJoinRoleReadOffset = $literalDayReadySource.IndexOf(
        '$legacyJoinRoleState = Get-RoleState join', [StringComparison]::Ordinal)
    $quietLoopOffset = $literalDayReadySource.IndexOf(
        'while ([DateTime]::UtcNow -lt $deadline)', [StringComparison]::Ordinal)
    if ($legacyHostRoleReadOffset -lt 0 -or
        $legacyJoinRoleReadOffset -le $legacyHostRoleReadOffset -or
        $quietLoopOffset -le $legacyJoinRoleReadOffset -or
        [regex]::Matches($literalDayReadySource, 'Get-RoleState\s+host').Count -ne 1 -or
        [regex]::Matches($literalDayReadySource, 'Get-RoleState\s+join').Count -ne 1 -or
        [regex]::Matches($literalDayReadySource, 'Get-RelayState').Count -ne 1 -or
        -not $literalDayReadySource.Contains(
            'TerminalRoleStates = [pscustomobject]@{') -or
        $literalDayReadySource -match
            'Get-TurnHistory|Read-SimRelayEvents|Get-World|New-CanonicalWalkPreparation') {
        throw 'static check: quiet observer lost its initial Host->Client reads, passive cadence, or sole terminal role snapshot'
    }
    $startupMssWitnessSource = [regex]::Match(
        $source,
        '(?ms)^function Get-LiteralStartupMssWitnessSnapshot\(.*?(?=^function Assert-LiteralStockStartupCompleteSnapshot)'
    ).Value
    if ([string]::IsNullOrEmpty($startupMssWitnessSource) -or
        [regex]::Matches($startupMssWitnessSource, 'Get-TurnHistory').Count -ne 1 -or
        [regex]::Matches($startupMssWitnessSource, 'Read-SimRelayEvents').Count -ne 1 -or
        $startupMssWitnessSource -match 'Get-RelayState|Get-World') {
        throw 'static check: deferred startup MSS witness must perform exactly its two post-census event reads'
    }
    $runtimeQuietOffset = $source.LastIndexOf(
        '$legacyQuietWitness = Wait-LiteralDayReady', [StringComparison]::Ordinal)
    $runtimeHeroCensusOffset = $source.LastIndexOf(
        '$heroCensus = Resolve-LiteralHostAuthoritativeHeroes',
        [StringComparison]::Ordinal)
    $runtimeMssWitnessOffset = $source.LastIndexOf(
        '$startupQuietWitness = Get-LiteralStartupMssWitnessSnapshot',
        [StringComparison]::Ordinal)
    $runtimeTerminalStateOffset = $source.LastIndexOf(
        '$terminalRoleStates = $legacyQuietWitness.TerminalRoleStates',
        [StringComparison]::Ordinal)
    $runtimePreparationOffset = $source.LastIndexOf(
        '$preparedDeployBindings = New-CanonicalWalkPreparation',
        [StringComparison]::Ordinal)
    $runtimeDeployOffset = $source.LastIndexOf(
        'Invoke-CanonicalDeploy `', [StringComparison]::Ordinal)
    if ($runtimeQuietOffset -lt 0 -or
        $runtimeHeroCensusOffset -le $runtimeQuietOffset -or
        $runtimeTerminalStateOffset -le $runtimeHeroCensusOffset -or
        $runtimePreparationOffset -le $runtimeTerminalStateOffset -or
        $runtimeDeployOffset -le $runtimePreparationOffset -or
        $runtimeMssWitnessOffset -le $runtimeDeployOffset) {
        throw 'static check: canonical quiet -> shared census -> exact binding -> deploy -> deferred MSS order changed'
    }

    $persistentPopupObserverSource = [regex]::Match(
        $source,
        '(?ms)^function Invoke-LiteralPersistentStartupPopupTick\(.*?(?=^function Update-LiteralStartupPopupWindow)'
    ).Value
    $popupWindowProjectionSource = [regex]::Match(
        $source,
        '(?ms)^function Update-LiteralStartupPopupWindow\(.*?(?=^function Invoke-LiteralStartupPopupTick)'
    ).Value
    $popupTimelineSource = [regex]::Match(
        $source,
        '(?ms)^function New-LiteralStartupPopupTimeline\(.*?(?=^function Wait-FixedUtcAnchorWithPopupService)'
    ).Value
    if ([string]::IsNullOrEmpty($persistentPopupObserverSource) -or
        [string]::IsNullOrEmpty($popupWindowProjectionSource) -or
        [string]::IsNullOrEmpty($popupTimelineSource) -or
        $persistentPopupObserverSource -match
            'Invoke-Button|Register-StartupModalAction|/api/ui/invoke' -or
        $popupTimelineSource -match
            'Invoke-Button|Register-StartupModalAction|/api/ui/invoke' -or
        -not $persistentPopupObserverSource.Contains(
            '\[testdrv\]\[scripted-popup\] (OBSERVED|CLAIMED|COMMITTED)\b') -or
        -not $persistentPopupObserverSource.Contains(
            "`$kind -ne 'COMMITTED'") -or
        -not $popupWindowProjectionSource.Contains(
            '$Window.FirstActionTick = [long]$firstClaim[0].Tick') -or
        -not $popupTimelineSource.Contains(
            '$Timeline.JoinEntryWindow.CompletedTick + 5000') -or
        -not $popupTimelineSource.Contains(
            '$Timeline.HostWindow.CompletedTick + 2000')) {
        throw 'static check: literal popup clocks must be read-only projections of exact native PID-log claims'
    }

    # This literal is also named inside the contract above. Bind to the real
    # executable main block at EOF, never to our own quoted audit strings.
    $mainStart = $source.LastIndexOf('$testRelay = $null', [StringComparison]::Ordinal)
    if ($mainStart -lt 0) {
        throw 'static check: owned startup main block is not identifiable'
    }
    $mainSource = $source.Substring($mainStart)
    $launchOrder = @(
        '$testRelay = Start-TestRelay -LogDir $ArtifactDir',
        '$simRelay = Start-ProductionSimRelay',
        'Start-Sleep -Milliseconds 1500',
        '$hostProcess = Start-SimturnGameClient host',
        '$hostLaunchUtc = [DateTime]::UtcNow',
        'Wait-FixedUtcAnchor ($hostLaunchUtc.AddMilliseconds(10000))',
        '$hostJoinLaunchDeadlineUtc = $hostLaunchUtc.AddSeconds($BootTimeoutSec)',
        '$hostPreJoinMainMenu = Wait-UiButtonReadyPublication',
        '$joinProcess = Start-SimturnGameClient join',
        '$joinLaunchUtc = [DateTime]::UtcNow',
        'Run-Pairing `'
    )
    $previousLaunchOffset = -1
    foreach ($literal in $launchOrder) {
        $offset = $mainSource.IndexOf($literal, [StringComparison]::Ordinal)
        if ($offset -le $previousLaunchOffset) {
            throw "static check: literal relay/host/join launch order lost at '$literal'"
        }
        $previousLaunchOffset = $offset
    }
    $fixedHostGapOffset = $mainSource.IndexOf(
        'Wait-FixedUtcAnchor ($hostLaunchUtc.AddMilliseconds(10000))',
        [StringComparison]::Ordinal)
    $joinProcessLaunchOffset = $mainSource.IndexOf(
        '$joinProcess = Start-SimturnGameClient join', [StringComparison]::Ordinal)
    $preJoinGateSource = $mainSource.Substring(
        $fixedHostGapOffset,
        $joinProcessLaunchOffset - $fixedHostGapOffset)
    if ([regex]::Matches(
            $preJoinGateSource, 'Wait-UiButtonReadyPublication').Count -ne 1 -or
        -not $preJoinGateSource.Contains('-Role host') -or
        -not $preJoinGateSource.Contains('-Dialog DLG_MAIN_MENU') -or
        -not $preJoinGateSource.Contains('-Button BTN_MULTI') -or
        -not $preJoinGateSource.Contains('-AfterUiSequence 0') -or
        $preJoinGateSource -match
            'Invoke-Button|/api/ui/invoke|Start-SimturnGameClient|\bwhile\s*\(|\bfor\s*\(') {
        throw 'static check: host+10000 minimum must lead to one passive exact pre-join UI event gate'
    }
    foreach ($launchProvenance in @(
        '([string]$hostProcess.D2MssLaunchExecutablePath) $hostLog 0',
        '([string]$joinProcess.D2MssLaunchExecutablePath) $joinLog 0'
    )) {
        if (-not $mainSource.Contains($launchProvenance)) {
            throw "static check: inner startup publication lost '$launchProvenance'"
        }
    }
    $pairingCallOffset = $mainSource.IndexOf('Run-Pairing `', [StringComparison]::Ordinal)
    if ([regex]::Matches(
            $mainSource,
            '-StartupObserver\s+\$script:LiteralInnerStartupObserver').Count -ne 1) {
        throw 'static check: main must pass the one literal startup observer into pairing'
    }
    if ($mainSource.Substring(0, $pairingCallOffset) -match 'Wait-OwnedBootDialog') {
        throw 'static check: fixed launch anchors regressed to a boot-dialog convergence wait'
    }
    $joinLaunchOffset = $mainSource.IndexOf(
        '$joinLaunchUtc = [DateTime]::UtcNow', [StringComparison]::Ordinal)
    $prePairingSource = $mainSource.Substring(
        $joinLaunchOffset,
        $pairingCallOffset - $joinLaunchOffset)
    if ([regex]::Matches(
            $prePairingSource,
            'Assert-OwnedProcessesLive\s+\$hostProcess\s+\$joinProcess').Count -ne 1) {
        throw 'static check: the post-launch checkpoint must contain one PID-only liveness assertion'
    }
    $joinDiagnosticWaitOffset = $prePairingSource.IndexOf(
        'Wait-FixedUtcAnchor ($joinLaunchUtc.AddMilliseconds(500))',
        [StringComparison]::Ordinal)
    $ownedLivenessOffset = $prePairingSource.IndexOf(
        'Assert-OwnedProcessesLive $hostProcess $joinProcess',
        [StringComparison]::Ordinal)
    if ($joinDiagnosticWaitOffset -lt 0 -or
        $ownedLivenessOffset -le $joinDiagnosticWaitOffset) {
        throw 'static check: legacy join+500 ms process diagnostic changed order'
    }
    if ($prePairingSource -match
            'Assert-ClientsLive|Assert-DebugRelayClientIdentity|Get-RoleState|Get-RelayState') {
        throw 'static check: DebugTest relay readiness moved before the literal delayed-injection navigation anchors'
    }

    $pairingSource = [regex]::Match(
        $source,
        '(?ms)^function Run-Pairing\(.*?(?=^function Assert-DebugRelayClientIdentity)'
    ).Value
    if ([string]::IsNullOrEmpty($pairingSource)) {
        throw 'static check: exact-once pairing function is not identifiable'
    }
    $pairingOrder = @(
        '$waitPeerMarker = ''[nettrace] peer CConnectMsg observed self=''',
        '$navigation = Complete-LiteralIndependentNavigation',
        '$hostPopupService = $navigation.HostPopupService',
        '$hostPopupWindow = $navigation.HostPopupWindow',
        '$startupWitness = $navigation.StartupWitness',
        '$joinPopupService = $navigation.JoinPopupService',
        '$joinEntryPopupWindow = $navigation.JoinPopupWindow',
        'kJoinScript stayed inside its independent lane',
        '$popupTimeline = New-LiteralStartupPopupTimeline',
        '$readyWorlds = Complete-LiteralOuterReadyObserver $ReadyObserver'
    )
    $previousPairingOffset = -1
    foreach ($literal in $pairingOrder) {
        $offset = $pairingSource.IndexOf($literal, [StringComparison]::Ordinal)
        if ($offset -le $previousPairingOffset) {
            throw "static check: pairing causal order lost at '$literal'"
        }
        $previousPairingOffset = $offset
    }

    $literalSelectionAsyncSource = [regex]::Match(
        $source,
        '(?ms)^function Start-LiteralSelectionRequest\s*\{.*?(?=^function Start-LiteralButtonRequest)'
    ).Value
    if ([string]::IsNullOrEmpty($literalSelectionAsyncSource) -or
        [regex]::Matches($literalSelectionAsyncSource, '\.SendAsync\(').Count -ne 1 -or
        -not $literalSelectionAsyncSource.Contains(
            '[System.Threading.Timeout]::InfiniteTimeSpan') -or
        -not $literalSelectionAsyncSource.Contains(
            'completion was consumed twice') -or
        -not $literalSelectionAsyncSource.Contains(
            '$observation = $CapturedObservation') -or
        $literalSelectionAsyncSource -match
            'Get-DialogObservation|Get-LiteralNavigation(?:RoleState|DialogObservation)' -or
        $literalSelectionAsyncSource -match '\b(re' + 'fire|re' + 'try|fall' + 'back)\b') {
        throw 'static check: overlapped selection must remain one uncancelled exact native intent'
    }

    $literalButtonAsyncSource = [regex]::Match(
        $source,
        '(?ms)^function Start-LiteralButtonRequest\s*\{.*?(?=^function Invoke-LiteralHostNavigationLane)'
    ).Value
    if ([string]::IsNullOrEmpty($literalButtonAsyncSource) -or
        [regex]::Matches($literalButtonAsyncSource, '\.SendAsync\(').Count -ne 1 -or
        -not $literalButtonAsyncSource.Contains(
            '[System.Threading.Timeout]::InfiniteTimeSpan') -or
        -not $literalButtonAsyncSource.Contains('completion was consumed twice') -or
        $literalButtonAsyncSource -match '\.Cancel|CancelAfter|\b(re' + 'fire|re' + 'try|fall' + 'back)\b') {
        throw 'static check: independent navigation button must remain one uncancelled exact intent'
    }
    if ($literalButtonAsyncSource -notmatch (
            '(?s)\$readyOwnerIntent\s*=\s*' +
            '\$Dialog -eq ''DLG_LOBBY'' -and \$Button -eq ''BTN_OK''[\s\S]*' +
            'if \(\$readyOwnerIntent\)[\s\S]*' +
            '\$afterUiSequence = \[long\]\$sequenceValue - 1[\s\S]*' +
            '\$stabilityQuery = if \(\$Role -eq ''host''\)[\s\S]*' +
            'stableMs=500[\s\S]*' +
            '/api/ui/invoke-when-ready[\s\S]*' +
            '\} else \{[\s\S]*\$candidate = \$CapturedObservation') -or
        [regex]::Matches(
            $literalButtonAsyncSource,
            '/api/ui/invoke-when-ready').Count -ne 1 -or
        $literalButtonAsyncSource -match
            'Get-DialogObservation|Get-LiteralNavigation(?:RoleState|DialogObservation)') {
        throw ('static check: volatile lobby OK must use one watermark-owned ' +
            'ready-owner intent while stable navigation remains exact-owner')
    }

    $literalPassiveTimeoutClassifierSource = [regex]::Match(
        $source,
        '(?ms)^function Test-LiteralNavigationPassiveTimeoutException.*?' +
            '(?=^function Get-LiteralNavigationRoleState)'
    ).Value
    $literalPassiveReadSource = [regex]::Match(
        $source,
        '(?ms)^function Get-LiteralNavigationRoleState\s*\{.*?' +
            '(?=^function Get-LiteralNavigationRoleStateProjection)'
    ).Value
    $literalPassiveProjectionSource = [regex]::Match(
        $source,
        '(?ms)^function Get-LiteralNavigationRoleStateProjection\s*\{.*?' +
            '(?=^function ConvertTo-LiteralNavigationUInt32)'
    ).Value
    $literalNavigationUInt32Source = [regex]::Match(
        $source,
        '(?ms)^function ConvertTo-LiteralNavigationUInt32\s*\{.*?' +
            '(?=^function ConvertTo-LiteralNavigationDialogObservation)'
    ).Value
    $literalPassiveDialogSource = [regex]::Match(
        $source,
        '(?ms)^function ConvertTo-LiteralNavigationDialogObservation.*?' +
            '(?=^# The bootstrap cursor pump)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($literalPassiveTimeoutClassifierSource) -or
        -not $literalPassiveTimeoutClassifierSource.Contains(
            '[System.TimeoutException]') -or
        -not $literalPassiveTimeoutClassifierSource.Contains(
            '[System.OperationCanceledException]') -or
        $literalPassiveTimeoutClassifierSource -match
            'HttpRequestException|SocketException') {
        throw 'static check: passive navigation may tolerate only timeout/cancellation exceptions'
    }
    foreach ($literal in @(
        '$startedUtc = [DateTime]::UtcNow',
        '$DeadlineUtc - $startedUtc',
        '[Math]::Min(',
        '$handler.UseProxy = $false',
        '$client.GetAsync($uri).GetAwaiter().GetResult()',
        '$httpResponse.IsSuccessStatusCode',
        'ConvertFrom-Json -ErrorAction Stop',
        'Test-LiteralNavigationPassiveTimeoutException',
        'literal navigation passive GET timed out:',
        'startedUtc={3:O}',
        'completedUtc={4:O}',
        'laneDeadlineUtc={7:O}',
        'Get-LiteralNavigationRoleStateProjection $Role $response',
        'return $null'
    )) {
        if ([string]::IsNullOrWhiteSpace($literalPassiveReadSource) -or
            -not $literalPassiveReadSource.Contains($literal)) {
            throw "static check: bounded passive navigation GET lost '$literal'"
        }
    }
    if ([regex]::Matches($literalPassiveReadSource, '\.GetAsync\(').Count -ne 1 -or
        $literalPassiveReadSource -match
            '\.PostAsync\(|\.SendAsync\(|-Method\s+POST|/api/ui/invoke') {
        throw 'static check: passive navigation timeout handling gained an action path'
    }
    foreach ($literal in @(
        '$Response -isnot [System.Management.Automation.PSCustomObject]',
        '$Response.PSObject.Properties[''terminalFault'']',
        'omitted terminalFault',
        'test relay terminal fault:',
        '$Response.PSObject.Properties[''roles'']',
        '[System.Management.Automation.PSCustomObject]',
        'if ($null -eq $roleProperty) { return $null }',
        'published a non-object $Role role'
    )) {
        if ([string]::IsNullOrWhiteSpace($literalPassiveProjectionSource) -or
            -not $literalPassiveProjectionSource.Contains($literal)) {
            throw "static check: passive navigation schema projection lost '$literal'"
        }
    }
    if ([regex]::Matches(
            $literalPassiveProjectionSource,
            '\[System\.Management\.Automation\.PSCustomObject\]').Count -ne 3 -or
        $literalPassiveProjectionSource -match
            'Invoke-RestMethod|\.GetAsync\(|\.PostAsync\(|\.SendAsync\(' -or
        [string]::IsNullOrWhiteSpace($literalNavigationUInt32Source) -or
        $literalNavigationUInt32Source -notmatch (
            '\$Value -is \[bool\][\s\S]*\$Value -is \[string\][\s\S]*' +
            '\$Value -isnot \[ValueType\][\s\S]*\[decimal\]::Truncate[\s\S]*' +
            '\$number -lt 1[\s\S]*\$number -gt \[uint32\]::MaxValue')) {
        throw 'static check: passive navigation schema or uint32 projection became permissive'
    }
    if ([string]::IsNullOrWhiteSpace($literalPassiveDialogSource) -or
        -not $literalPassiveDialogSource.Contains(
            'Get-LiteralNavigationRoleState') -or
        -not $literalPassiveDialogSource.Contains(
            'ConvertTo-LiteralNavigationDialogObservation') -or
        $literalPassiveDialogSource -notmatch (
            '\$State -isnot \[System\.Management\.Automation\.PSCustomObject\][\s\S]*' +
            '\$dialogProperty\.Value -isnot \[string\][\s\S]*' +
            '\[string\]::IsNullOrWhiteSpace[\s\S]*' +
            '\$readyValue -isnot \[bool\][\s\S]*' +
            'ConvertTo-LiteralNavigationUInt32[\s\S]*' +
            '\$appearance -ne \$instance') -or
        $literalPassiveDialogSource -match
            'Invoke-RestMethod|\.GetAsync\(|\.PostAsync\(|\.SendAsync\(') {
        throw 'static check: navigation dialog projection lost its one passive role-state read'
    }

    $hostLaneSource = [regex]::Match(
        $source,
        '(?ms)^function Invoke-LiteralHostNavigationLane\s*\{.*?(?=^function Invoke-LiteralJoinNavigationLane)'
    ).Value
    $joinLaneSource = [regex]::Match(
        $source,
        '(?ms)^function Invoke-LiteralJoinNavigationLane\s*\{.*?(?=^function Start-LiteralNavigationWorker)'
    ).Value
    $workerStartSource = [regex]::Match(
        $source,
        '(?ms)^function Start-LiteralNavigationWorker\s*\{.*?(?=^function Complete-LiteralNavigationWorker)'
    ).Value
    $workerCompleteSource = [regex]::Match(
        $source,
        '(?ms)^function Complete-LiteralNavigationWorker.*?(?=^function Complete-LiteralIndependentNavigation)'
    ).Value
    $navigationJoinSource = [regex]::Match(
        $source,
        '(?ms)^function Complete-LiteralIndependentNavigation\s*\{.*?(?=^function Wait-LiteralSelectionOrUtcAnchor)'
    ).Value
    $startupLatchSource = [regex]::Match(
        $source,
        '(?ms)^function Add-LiteralJoinStockStartupHistory.*?(?=^function Wait-FixedUtcAnchor)'
    ).Value
    $independentNavigationSource =
        $hostLaneSource + $joinLaneSource + $workerStartSource +
        $workerCompleteSource + $navigationJoinSource
    foreach ($sourceContract in @(
        @{ Label = 'host'; Source = $hostLaneSource; Tokens = @(
        '$StartGate.Wait()',
        '$laneStartedUtc = [DateTime]::UtcNow',
        '$deadlineUtc = $laneStartedUtc.AddSeconds($TimeoutSec)',
        '$waitPeerBefore = Get-ClientLogMarkerCount $ClientLog $WaitPeerMarker',
        'Get-LiteralMainMenuAtLaunchAnchor',
        'host $OwnedProcess $LaunchUtc',
        '-Role host -Dialog DLG_MAIN_MENU -Button BTN_MULTI',
        '-Kind index -Role host -Dialog DLG_PROTOCOL',
        '-Role host -Dialog DLG_PROTOCOL -Button BTN_CONTINUE',
        '-Role host -Dialog DLG_LOAD_NEW_MULTI -Button BTN_HOST',
        '-Role host -Dialog DLG_CHOOSE_SKIRMISH -Button BTN_LOAD',
        'Get-LiteralClientLogMarkerEventUtcSnapshot',
        '$lane.Load.CompletedUtc -gt',
        '([DateTime]$lane.WaitPeerReleaseUtc).AddMilliseconds(500)',
        'Wait-FixedUtcAnchor $lane.LobbyArmUtc',
        '-Role host -Dialog DLG_LOBBY -Button BTN_OK',
        '$lane.LobbyOk.CompletedUtc.AddMilliseconds(500)',
        'Wait-FixedUtcAnchor $lane.PopupArmUtc',
        'New-LiteralStartupPopupService host $ClientLog',
        'New-LiteralStartupPopupWindow',
        'host 25000 $lane.PopupService',
        '$advanceWithinDriverTick = $true',
        'Start-Sleep -Milliseconds 100') },
        @{ Label = 'join'; Source = $joinLaneSource; Tokens = @(
        '$StartGate.Wait()',
        '$laneStartedUtc = [DateTime]::UtcNow',
        '$deadlineUtc = $laneStartedUtc.AddSeconds($TimeoutSec)',
        '$startupTurnBaseline = Get-TurnHistory',
        '$sessionListReadyBefore =',
        'Get-ClientLogMarkerCount $ClientLog $sessionListReadyMarker',
        'Get-LiteralMainMenuAtLaunchAnchor',
        'join $OwnedProcess $LaunchUtc',
        '-Role join -Dialog DLG_MAIN_MENU -Button BTN_MULTI',
        '-Kind index -Role join -Dialog DLG_PROTOCOL',
        '-Role join -Dialog DLG_PROTOCOL -Button BTN_CONTINUE',
        '-Role join -Dialog DLG_LOAD_NEW_MULTI -Button BTN_JOIN',
        '$lane.Search.CompletedUtc.AddMilliseconds(2000)',
        'Wait-FixedUtcAnchor $lane.JoinGameArmUtc',
        '-Role join -Dialog DLG_SESSION -Button BTN_JOIN_GAME',
        'Complete-LiteralTurnHistoryRequest $lane.StartupRequest',
        'Add-LiteralJoinStockStartupHistory',
        '$lane.Phase = ''wait-host-briefing''',
        '$lane.StartupState.BriefingLatch',
        '$lane.HostBriefingReleasedUtc = [DateTime]::UtcNow',
        '$lane.Phase = ''wait-host-strategic''',
        '$lane.StartupState.StrategicLatch',
        '$lane.HostStrategicReleasedUtc = [DateTime]::UtcNow',
        '$lane.HostStrategicReleasedUtc.AddMilliseconds(1000)',
        '[DateTime]::UtcNow.AddMilliseconds(1000)',
        'Wait-FixedUtcAnchor $lane.FirstSettleArmUtc',
        'Wait-FixedUtcAnchor $lane.SecondSettleArmUtc',
        '-Role join -Dialog DLG_LOBBY -Button BTN_OK',
        '$lane.LobbyOk.CompletedUtc.AddMilliseconds(500)',
        'Wait-FixedUtcAnchor $lane.PopupArmUtc',
        'New-LiteralStartupPopupService join $ClientLog',
        'join 20000 $lane.PopupService',
        'Wait-LiteralStartupPopupWindow $lane.PopupWindow',
        'if (-not [bool]$lane.PopupWindow.Done)',
        '$advanceWithinDriverTick = $true',
        'Start-Sleep -Milliseconds 100') }
    )) {
        if ([string]::IsNullOrEmpty([string]$sourceContract.Source)) {
            throw "static check: $($sourceContract.Label) physical navigation lane is missing"
        }
        foreach ($literal in $sourceContract.Tokens) {
            if (-not $sourceContract.Source.Contains($literal)) {
                throw "static check: $($sourceContract.Label) physical navigation lane lost '$literal'"
            }
        }
    }

    foreach ($laneContract in @(
        @{ Label = 'host'; Source = $hostLaneSource },
        @{ Label = 'join'; Source = $joinLaneSource }
    )) {
        if ([regex]::Matches(
                $laneContract.Source,
                'Get-LiteralNavigationDialogObservation').Count -ne 4 -or
            $laneContract.Source -match
                'Get-DialogObservation|Get-RoleState|Invoke-RestMethod') {
            throw "static check: $($laneContract.Label) navigation lane lost its timeout-tolerant passive observation path"
        }
        if ([regex]::Matches(
                $laneContract.Source,
                'Start-Sleep\s+-Milliseconds\s+100\b').Count -ne 1 -or
            [regex]::Matches(
                $laneContract.Source,
                '\$advanceWithinDriverTick\s*=\s*\$true').Count -lt 2 -or
            $laneContract.Source -match
                '\.Cancel|CancelAfter|\b(re' + 'fire|re' + 'try|fall' + 'back)\b') {
            throw "static check: $($laneContract.Label) worker lost one ordinary 100 ms driver edge or exact-once control flow"
        }
    }
    if ([regex]::Matches($joinLaneSource,
            'AddMilliseconds\(2000\)').Count -ne 1 -or
        [regex]::Matches($joinLaneSource,
            'AddMilliseconds\(1000\)').Count -ne 2 -or
        $hostLaneSource -match '\$join(?:Lane|Worker|Process|Result|\.)' -or
        $joinLaneSource -match '\$host(?:Lane|Worker|Process|Result|\.)') {
        throw 'static check: physical lane clocks or role isolation changed'
    }
    foreach ($laneContract in @(
        @{ Label = 'host'; Source = $hostLaneSource },
        @{ Label = 'join'; Source = $joinLaneSource }
    )) {
        if ([regex]::Matches(
                $laneContract.Source,
                '\$StartGate\.Wait\(\)').Count -ne 1 -or
            [regex]::Matches(
                $laneContract.Source,
                '\$deadlineUtc\s*=\s*\$laneStartedUtc\.AddSeconds\(\$TimeoutSec\)').Count -ne 1 -or
            $laneContract.Source -match '\[DateTime\]\$DeadlineUtc') {
            throw "static check: $($laneContract.Label) lost its gated independent timeout budget"
        }
    }

    foreach ($literal in @(
        '[Parameter(Mandatory)][ValidateNotNull()][object]$StartupObserver',
        '[System.Management.Automation.Runspaces.InitialSessionState]::CreateDefault2()',
        "@{ Name = 'LiteralInnerStartupObserver'; Value = `$StartupObserver }",
        '[System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace(',
        '$powerShell = [PowerShell]::Create()',
        '$async = $powerShell.BeginInvoke()',
        '[void]$powerShell.AddParameter(''OwnedProcess'', $OwnedProcess)',
        '[void]$powerShell.AddParameter(''TimeoutSec'', $TimeoutSec)',
        '[void]$powerShell.AddParameter(''StartGate'', $StartGate)'
    )) {
        if ([string]::IsNullOrEmpty($workerStartSource) -or
            -not $workerStartSource.Contains($literal)) {
            throw "static check: physical navigation worker factory lost '$literal'"
        }
    }
    $observerSeedOffset = $workerStartSource.IndexOf(
        "@{ Name = 'LiteralInnerStartupObserver'; Value = `$StartupObserver }",
        [StringComparison]::Ordinal)
    $runspaceCreateOffset = $workerStartSource.IndexOf(
        '[System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace(',
        [StringComparison]::Ordinal)
    $workerBeginOffset = $workerStartSource.IndexOf(
        '$async = $powerShell.BeginInvoke()', [StringComparison]::Ordinal)
    if ($observerSeedOffset -lt 0 -or
        $runspaceCreateOffset -le $observerSeedOffset -or
        $workerBeginOffset -le $runspaceCreateOffset -or
        $workerStartSource -match '\$script:LiteralInnerStartupObserver') {
        throw ('static check: navigation worker must seed its explicit startup ' +
            'observer dependency before BeginInvoke without outer-scope capture')
    }

    # Reconstruct the same function universe that exists after the four helper
    # dot-sources and this script's own declarations. Starting from every
    # function copied into a navigation runspace, follow local function calls
    # transitively. Every local dependency must itself be copied, and every
    # script-scope variable read anywhere in that closure must be an explicit
    # InitialSessionState seed. This keeps a missing runspace dependency from
    # becoming a live-only, one-failure-at-a-time discovery.
    $navigationFunctionAsts = @{}
    foreach ($navigationFunctionPath in @(
        $helperPath,
        $gameplayHelperPath,
        $ownedProcessDiagnosticsPath,
        $literalInnerStartupPath,
        $PSCommandPath
    )) {
        $navigationTokens = $null
        $navigationParseErrors = $null
        $navigationAst =
            [System.Management.Automation.Language.Parser]::ParseFile(
                $navigationFunctionPath,
                [ref]$navigationTokens,
                [ref]$navigationParseErrors)
        if ($navigationParseErrors.Count -ne 0) {
            $details = ($navigationParseErrors | ForEach-Object Message) -join '; '
            throw "static check: navigation AST parse failed for '$navigationFunctionPath': $details"
        }
        $readOnlyAutomaticNames = @('host', 'home', 'pid', 'pshome')
        $readOnlyAutomaticWrites = @($navigationAst.FindAll({
            param($node)
            if ($node -is [System.Management.Automation.Language.ParameterAst]) {
                return $readOnlyAutomaticNames -contains
                    ([string]$node.Name.VariablePath.UserPath).ToLowerInvariant()
            }
            return $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
                $readOnlyAutomaticNames -contains
                    ([string]$node.Left.VariablePath.UserPath).ToLowerInvariant()
        }, $true))
        if ($readOnlyAutomaticWrites.Count -ne 0) {
            $details = @($readOnlyAutomaticWrites | ForEach-Object {
                "$($_.Extent.File):$($_.Extent.StartLineNumber): $($_.Extent.Text)"
            }) -join '; '
            throw "static check: read-only automatic PowerShell variable collision: $details"
        }
        foreach ($functionAst in @($navigationAst.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
        }, $true))) {
            $navigationFunctionAsts[[string]$functionAst.Name] = $functionAst
        }
    }

    $navigationWorkerAst = $navigationFunctionAsts['Start-LiteralNavigationWorker']
    $functionNameAssignments = @($navigationWorkerAst.Body.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
        [string]$node.Left.VariablePath.UserPath -eq 'functionNames'
    }, $true))
    if ($functionNameAssignments.Count -ne 1) {
        throw 'static check: navigation worker must declare one literal function import set'
    }
    $navigationImportedFunctionNames = @(
        $functionNameAssignments[0].Right.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.StringConstantExpressionAst]
        }, $true) | ForEach-Object { [string]$_.Value }
    )
    $navigationImportedFunctionSet =
        [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($functionName in $navigationImportedFunctionNames) {
        if (-not $navigationImportedFunctionSet.Add($functionName)) {
            throw "static check: duplicate navigation function import '$functionName'"
        }
    }

    $navigationFunctionClosure =
        [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
    $navigationFunctionQueue = [System.Collections.Generic.Queue[string]]::new()
    foreach ($functionName in $navigationImportedFunctionNames) {
        $navigationFunctionQueue.Enqueue($functionName)
    }
    while ($navigationFunctionQueue.Count -gt 0) {
        $functionName = $navigationFunctionQueue.Dequeue()
        if (-not $navigationFunctionClosure.Add($functionName)) { continue }
        if (-not $navigationFunctionAsts.ContainsKey($functionName)) {
            throw "static check: navigation import '$functionName' has no parsed definition"
        }
        $functionAst = $navigationFunctionAsts[$functionName]
        foreach ($commandAst in @($functionAst.Body.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.CommandAst]
        }, $true))) {
            $callee = [string]$commandAst.GetCommandName()
            if (-not [string]::IsNullOrWhiteSpace($callee) -and
                $navigationFunctionAsts.ContainsKey($callee) -and
                -not $navigationFunctionClosure.Contains($callee)) {
                $navigationFunctionQueue.Enqueue($callee)
            }
        }
    }
    $missingFunctionImports = @(
        $navigationFunctionClosure |
            Where-Object { -not $navigationImportedFunctionSet.Contains($_) } |
            Sort-Object
    )
    if ($missingFunctionImports.Count -ne 0) {
        throw ('static check: navigation runspace omitted transitive function imports: ' +
            ($missingFunctionImports -join ', '))
    }

    $navigationSeedNames =
        [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($hashtableAst in @($navigationWorkerAst.Body.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.HashtableAst]
    }, $true))) {
        $seedName = $null
        $hasValueEntry = $false
        foreach ($pair in $hashtableAst.KeyValuePairs) {
            $key = [string]$pair.Item1.SafeGetValue()
            if ($key -eq 'Name') {
                $seedName = [string]$pair.Item2.SafeGetValue()
            } elseif ($key -eq 'Value') {
                $hasValueEntry = $true
            }
        }
        if ($hasValueEntry -and -not [string]::IsNullOrWhiteSpace($seedName)) {
            [void]$navigationSeedNames.Add($seedName)
        }
    }

    $missingScriptSeeds = @(
        @(
            foreach ($functionName in $navigationFunctionClosure) {
                $functionAst = $navigationFunctionAsts[$functionName]
                foreach ($variableAst in @($functionAst.Body.FindAll({
                    param($node)
                    $node -is [System.Management.Automation.Language.VariableExpressionAst] -and
                    $node.VariablePath.IsScript
                }, $true))) {
                    $scriptVariableName =
                        ([string]$variableAst.VariablePath.UserPath) -replace '^script:', ''
                    if (-not $navigationSeedNames.Contains($scriptVariableName)) {
                        "$functionName -> script:$scriptVariableName"
                    }
                }
            }
        ) | Sort-Object -Unique
    )
    if ($missingScriptSeeds.Count -ne 0) {
        throw ('static check: navigation runspace omitted script-scope seeds: ' +
            ($missingScriptSeeds -join ', '))
    }
    if ([regex]::Matches($workerStartSource, '\.BeginInvoke\(').Count -ne 1 -or
        [regex]::Matches($workerCompleteSource, '\.EndInvoke\(').Count -ne 1 -or
        $workerStartSource -match '\.Invoke\(\)|Start-Job|Start-ThreadJob|' +
            '\.Cancel|CancelAfter|\b(re' + 'fire|re' + 'try|fall' + 'back)\b' -or
        $workerCompleteSource -match '\.Stop\(|\.Cancel|CancelAfter') {
        throw 'static check: navigation must use one BeginInvoke/EndInvoke physical worker contract without cancellation'
    }
    if (-not $workerCompleteSource.Contains(
            '$Worker.PowerShell.Streams.Warning') -or
        -not $workerCompleteSource.Contains(
            'literal navigation passive GET timed out:') -or
        -not $workerCompleteSource.Contains(
            'PassiveReadDiagnostics') -or
        -not $navigationJoinSource.Contains(
            'PassiveReadDiagnostics = @(') -or
        -not $pairingSource.Contains(
            'foreach ($passiveDiagnostic in @($navigation.PassiveReadDiagnostics))') -or
        -not $pairingSource.Contains(
            'Write-Step "tolerated $passiveDiagnostic"')) {
        throw 'static check: tolerated navigation GET timeouts lost their timestamped diagnostic handoff'
    }
    $hostWorkerStartOffset = $navigationJoinSource.IndexOf(
        '$hostWorker = Start-LiteralNavigationWorker', [StringComparison]::Ordinal)
    $joinWorkerStartOffset = $navigationJoinSource.IndexOf(
        '$joinWorker = Start-LiteralNavigationWorker', [StringComparison]::Ordinal)
    $releaseOffset = $navigationJoinSource.IndexOf(
        '$startGate.Set()', [StringComparison]::Ordinal)
    $passiveJoinOffset = $navigationJoinSource.IndexOf(
        'foreach ($worker in @($hostWorker, $joinWorker))',
        [StringComparison]::Ordinal)
    if ($hostWorkerStartOffset -lt 0 -or
        $joinWorkerStartOffset -le $hostWorkerStartOffset -or
        $releaseOffset -le $joinWorkerStartOffset -or
        $passiveJoinOffset -le $releaseOffset -or
        $navigationJoinSource -match
            'Get-DialogObservation|Get-LiteralClientLogMarkerEventUtcSnapshot|' +
            'Invoke-RestMethod|Get-Content|Start-Sleep|while\s*\(') {
        throw 'static check: outer navigation must start both physical workers before a passive result join'
    }
    if ([regex]::Matches($navigationJoinSource,
            'Start-LiteralNavigationWorker').Count -ne 2 -or
        -not $navigationJoinSource.Contains(
            '[Parameter(Mandatory)][ValidateNotNull()][object]$StartupObserver') -or
        [regex]::Matches($navigationJoinSource,
            '-StartupObserver \$StartupObserver').Count -ne 2 -or
        [regex]::Matches($navigationJoinSource,
            'Complete-LiteralNavigationWorker').Count -ne 2 -or
        -not $workerCompleteSource.Contains(
            '[object]::ReferenceEquals(') -or
        [regex]::Matches($navigationJoinSource,
            '\$startGate\s*=\s*\[System\.Threading\.ManualResetEventSlim\]::new\(\$false\)').Count -ne 1 -or
        [regex]::Matches($navigationJoinSource,
            '\$startGate\.Set\(\)').Count -lt 1 -or
        $navigationJoinSource -match '\$deadlineUtc\s*=|AddSeconds\(\$TimeoutSec\)') {
        throw 'static check: outer navigation lost two exact-owned worker identities'
    }
    if ([regex]::Matches($pairingSource,
            'Complete-LiteralIndependentNavigation').Count -ne 1 -or
        $pairingSource -notmatch
            '\[Parameter\(Mandatory\)\]\[ValidateNotNull\(\)\]\s*\[object\]\$StartupObserver' -or
        [regex]::Matches($pairingSource,
            '-StartupObserver \$StartupObserver').Count -ne 1 -or
        $pairingSource -match
            'Get-LiteralMainMenuAtLaunchAnchor|Invoke-OneShotTransition\s+host\s+DLG_MAIN_MENU|' +
            'Select-Settle\s+host\s+DLG_PROTOCOL|\$hostScenarioRequest|' +
            '\$hostLoadRequest|\$joinMainMenuRequest|Wait-LiteralSelectionOrUtcAnchor|' +
            'Wait-NewValidatedJoinStockStartupObserved|Wait-FixedUtcAnchorWithPopupService|' +
            'Get-DialogObservation\s+join|Invoke-OneShotButton\s+`?\s*join\s+DLG_LOBBY|' +
            'Get-TurnHistory|Get-UiHistory|Get-ClientLogMarkerCount|Read-ClientLogLines|' +
            'Invoke-RestMethod|Get-Content') {
        throw 'static check: Run-Pairing must enter the sole physical worker pair without a serialized host/join prelude'
    }

    if ($pairingSource -match 'Start-Sleep\s+-Milliseconds\s+1500') {
        throw 'static check: host lobby path regained the removed 1500 ms settle action'
    }
    foreach ($forbiddenPairingStep in @(
        'Invoke-OneShotTransition join DLG_LOAD_NEW_MULTI BTN_JOIN DLG_SESSION',
        'Invoke-OneShotTransition join DLG_SESSION BTN_JOIN_GAME DLG_LOBBY',
        'Wait-CapturedDialogDeparture $hostLobbyAction',
        'Wait-CapturedDialogDeparture $joinLobbyAction'
    )) {
        if ($pairingSource.Contains($forbiddenPairingStep)) {
            throw "static check: pairing restored a later-effect wait '$forbiddenPairingStep'"
        }
    }
    $waitPeerGateOffset = $hostLaneSource.IndexOf(
        'Get-LiteralClientLogMarkerEventUtcSnapshot',
        [StringComparison]::Ordinal)
    $waitPeerMaxOffset = $hostLaneSource.IndexOf(
        '$lane.Load.CompletedUtc -gt',
        [StringComparison]::Ordinal)
    $waitPeerAnchorOffset = $hostLaneSource.IndexOf(
        '([DateTime]$lane.WaitPeerReleaseUtc).AddMilliseconds(500)',
        [StringComparison]::Ordinal)
    $hostActionOffset = $hostLaneSource.IndexOf(
        '-Role host -Dialog DLG_LOBBY -Button BTN_OK', [StringComparison]::Ordinal)
    $hostActionAnchorOffset = $hostLaneSource.IndexOf(
        '$lane.LobbyOk.CompletedUtc.AddMilliseconds(500)',
        [StringComparison]::Ordinal)
    $hostWindowOffset = $hostLaneSource.IndexOf(
        '$lane.PopupWindow = New-LiteralStartupPopupWindow',
        [StringComparison]::Ordinal)
    if ($waitPeerGateOffset -lt 0 -or
        $waitPeerMaxOffset -le $waitPeerGateOffset -or
        $waitPeerAnchorOffset -le $waitPeerMaxOffset -or
        $hostActionOffset -le $waitPeerAnchorOffset -or
        $hostActionAnchorOffset -le $hostActionOffset -or
        $hostWindowOffset -le $hostActionAnchorOffset) {
        throw 'static check: typed WaitPeer/+500/BTN_OK/+500/25s startup anchors changed order'
    }
    if ([regex]::Matches($hostLaneSource,
            '-Role\s+host\s+-Dialog\s+DLG_LOBBY\s+-Button\s+BTN_OK').Count -ne 1 -or
        [regex]::Matches($hostLaneSource,
            'Get-LiteralClientLogMarkerEventUtcSnapshot').Count -ne 1) {
        throw 'static check: WaitPeer/host lobby action must remain one sampled event and one exact action site'
    }

    $joinGameOffset = $joinLaneSource.IndexOf(
        '-Role join -Dialog DLG_SESSION -Button BTN_JOIN_GAME',
        [StringComparison]::Ordinal)
    $joinBriefingOffset = $joinLaneSource.IndexOf(
        '$lane.HostBriefingReleasedUtc = [DateTime]::UtcNow',
        [StringComparison]::Ordinal)
    $joinStrategicOffset = $joinLaneSource.IndexOf(
        '$lane.HostStrategicReleasedUtc = [DateTime]::UtcNow',
        [StringComparison]::Ordinal)
    $joinFirstSecondOffset = $joinLaneSource.IndexOf(
        '$lane.HostStrategicReleasedUtc.AddMilliseconds(1000)',
        [StringComparison]::Ordinal)
    $joinSecondSecondOffset = $joinLaneSource.IndexOf(
        '$lane.SecondSettleArmUtc =',
        [StringComparison]::Ordinal)
    $joinActionOffset = $joinLaneSource.IndexOf(
        '-Role join -Dialog DLG_LOBBY -Button BTN_OK',
        [StringComparison]::Ordinal)
    $joinFiveHundredOffset = $joinLaneSource.IndexOf(
        '$lane.LobbyOk.CompletedUtc.AddMilliseconds(500)',
        [StringComparison]::Ordinal)
    $joinWindowOffset = $joinLaneSource.IndexOf(
        '$lane.PopupWindow = New-LiteralStartupPopupWindow',
        [StringComparison]::Ordinal)
    $joinWindowWaitOffset = $joinLaneSource.IndexOf(
        'Wait-LiteralStartupPopupWindow $lane.PopupWindow',
        [StringComparison]::Ordinal)
    $joinWindowDoneOffset = $joinLaneSource.IndexOf(
        'if (-not [bool]$lane.PopupWindow.Done)',
        [StringComparison]::Ordinal)
    if ($joinGameOffset -lt 0 -or
        $joinBriefingOffset -le $joinGameOffset -or
        $joinStrategicOffset -le $joinBriefingOffset -or
        $joinFirstSecondOffset -le $joinStrategicOffset -or
        $joinSecondSecondOffset -le $joinFirstSecondOffset -or
        $joinActionOffset -le $joinSecondSecondOffset -or
        $joinFiveHundredOffset -le $joinActionOffset -or
        $joinWindowOffset -le $joinFiveHundredOffset -or
        $joinWindowWaitOffset -le $joinWindowOffset -or
        $joinWindowDoneOffset -le $joinWindowWaitOffset) {
        throw 'static check: join CJoin/Begin/+1000/+1000/OK/+500/20s lane order changed'
    }

    if ([string]::IsNullOrEmpty($startupLatchSource) -or
        [regex]::Matches($startupLatchSource,
            '\$State\.BriefingLatch\s*=\s*\$joinGameEvent').Count -ne 1 -or
        [regex]::Matches($startupLatchSource,
            '\$State\.StrategicLatch\s*=\s*\$beginEvent').Count -ne 1 -or
        $startupLatchSource -match
            'beginSequence\s+-ge\s+\$joinGameSequence|joinGameSequence\s+-g[et]\s+\$beginSequence' -or
        -not ($joinLaneSource -match
            "(?s)Phase -eq 'wait-host-briefing'.{0,180}StartupState\.BriefingLatch") -or
        -not ($joinLaneSource -match
            "(?s)Phase -eq 'wait-host-strategic'.{0,180}StartupState\.StrategicLatch") -or
        $joinLaneSource -match
            "(?s)Phase -eq 'wait-host-(?:briefing|strategic)'.{0,180}StartupState\.Witness" -or
        [regex]::Matches($joinLaneSource,
            'AddMilliseconds\(1000\)').Count -ne 2 -or
        [regex]::Matches($joinLaneSource,
            '-Role\s+join\s+-Dialog\s+DLG_LOBBY\s+-Button\s+BTN_OK').Count -ne 1 -or
        [regex]::Matches(
            $joinLaneSource,
            'New-LiteralStartupPopupWindow\s+`?\s*join\s+20000\b').Count -ne 1 -or
        [regex]::Matches(
            $joinLaneSource,
            'Wait-LiteralStartupPopupWindow\s+\$lane\.PopupWindow').Count -ne 1 -or
        [regex]::Matches(
            $pairingSource,
            'New-LiteralStartupPopupWindow\s+`?\s*join\s+5000\b').Count -ne 0 -or
        [regex]::Matches($pairingSource, '-ObserveOnly\b').Count -ne 0 -or
        -not $pairingSource.Contains('$popupTimeline = New-LiteralStartupPopupTimeline') -or
        $pairingSource.Contains('Wait-LiteralStartupPopupWindow') -or
        [regex]::Matches(
            $joinLaneSource,
            'New-LiteralStartupPopupService\s+join\b').Count -ne 1 -or
        -not $pairingSource.Contains('$startupWitness = $navigation.StartupWitness') -or
        -not $pairingSource.Contains('$joinPopupService = $navigation.JoinPopupService') -or
        -not $pairingSource.Contains('$joinEntryPopupWindow = $navigation.JoinPopupWindow')) {
        throw 'static check: join lane must own both gates, clocks, OK and first popup before pure pairing handoff'
    }

    if (-not $popupTimelineSource.Contains(
            '$Timeline.JoinEntryWindow.CompletedTick + 5000') -or
        -not $popupTimelineSource.Contains(
            'join 20000 $Timeline.JoinPopupService') -or
        -not $popupTimelineSource.Contains(
            '$Timeline.HostWindow.CompletedTick + 2000') -or
        -not $popupTimelineSource.Contains(
            '$script:LegacyStartupWindowsComplete = $true')) {
        throw 'static check: independent join +5000/20s and host +2000 timestamp projections changed'
    }

    $legacyStartupPassOffset = $mainSource.IndexOf(
        'legacy startup PASS: both role logs reached the exact quiet-3 checkpoint',
        [StringComparison]::Ordinal)
    $mssStartupCheckOffset = $mainSource.IndexOf(
        '$startupMssProof = Assert-LiteralStartupMssWitness',
        [StringComparison]::Ordinal)
    $sharedCensusOffset = $mainSource.IndexOf(
        '$heroCensus = Resolve-LiteralHostAuthoritativeHeroes $fixture',
        [StringComparison]::Ordinal)
    $startupMssReadOffset = $mainSource.IndexOf(
        '$startupQuietWitness = Get-LiteralStartupMssWitnessSnapshot',
        [StringComparison]::Ordinal)
    $savedTerminalStateOffset = $mainSource.IndexOf(
        '$terminalRoleStates = $legacyQuietWitness.TerminalRoleStates',
        [StringComparison]::Ordinal)
    $preparedBindingOffset = $mainSource.IndexOf(
        '$preparedDeployBindings = New-CanonicalWalkPreparation',
        [StringComparison]::Ordinal)
    $canonicalDeployOffset = $mainSource.IndexOf(
        'Invoke-CanonicalDeploy `',
        [StringComparison]::Ordinal)
    $deferredHeroCheckOffset = $mainSource.IndexOf(
        'Assert-DeferredLiteralHostAuthoritativeHeroFixture $heroCensus $fixture',
        [StringComparison]::Ordinal)
    if ($legacyStartupPassOffset -lt 0 -or
        $sharedCensusOffset -le $legacyStartupPassOffset -or
        $savedTerminalStateOffset -le $sharedCensusOffset -or
        $preparedBindingOffset -le $savedTerminalStateOffset -or
        $canonicalDeployOffset -le $preparedBindingOffset -or
        $startupMssReadOffset -le $canonicalDeployOffset -or
        $mssStartupCheckOffset -le $startupMssReadOffset -or
        $deferredHeroCheckOffset -le $mssStartupCheckOffset) {
        throw 'static check: quiet-3 -> one census -> saved binding projection -> deploy -> deferred MSS verdict order changed'
    }
    $quietToCensusSource = $mainSource.Substring(
        $legacyStartupPassOffset,
        $sharedCensusOffset - $legacyStartupPassOffset)
    if ($quietToCensusSource -match
            'Get-|Read-|Wait-|Start-Sleep|Invoke-RestMethod|Assert-ClientsLive|' +
            'Assert-NoClientFaults|Assert-OperationalActionGate|-SnapshotOnly') {
        throw 'static check: no observation or wait may precede the sole post-quiet shared census'
    }
    $censusToDeploySource = $mainSource.Substring(
        $sharedCensusOffset,
        $canonicalDeployOffset - $sharedCensusOffset)
    if ($censusToDeploySource -match
            'Get-LiteralStartupMssWitnessSnapshot|Assert-LiteralStartupMssWitness|' +
            'Get-World\s|Get-RoleState|Get-RelayState|Read-SimRelayEvents|Get-TurnHistory|' +
            'Wait-|Start-Sleep|Invoke-RestMethod|Write-Step') {
        throw 'static check: pre-deploy path must project the saved quiet-edge owners without another transport read'
    }
    $deployCallTail = $mainSource.Substring(
        $canonicalDeployOffset,
        [Math]::Min(240, $mainSource.Length - $canonicalDeployOffset))
    if (-not $deployCallTail.Contains(
            '$fixture $hostProcess $joinProcess $preparedDeployBindings')) {
        throw 'static check: canonical deploy must consume terminal saved bindings without hidden role-state reads'
    }

    $startupWitnessSnapshotSource = [regex]::Match(
        $source,
        '(?ms)^function Get-LiteralStartupMssWitnessSnapshot\(.*?(?=^function Assert-LiteralStockStartupCompleteSnapshot)'
    ).Value
    $startupWitnessAssertSource = [regex]::Match(
        $source,
        '(?ms)^function Assert-LiteralStartupMssWitness\(.*?(?=^function Resolve-LiteralHostAuthoritativeHeroes)'
    ).Value
    if ([string]::IsNullOrEmpty($startupWitnessSnapshotSource) -or
        [regex]::Matches($startupWitnessSnapshotSource, 'Get-TurnHistory').Count -ne 1 -or
        [regex]::Matches($startupWitnessSnapshotSource, 'Read-SimRelayEvents').Count -ne 1 -or
        $startupWitnessSnapshotSource -match
            'Get-World|Get-RoleState|Get-RelayState|Get-Dialog|Get-UiHistory|' +
            'Invoke-Button|/api/ui/invoke|Start-Sleep|Wait-') {
        throw 'static check: terminal quiet witness must be one passive turn/event snapshot with no action/UI/world/wait'
    }
    if ([string]::IsNullOrEmpty($startupWitnessAssertSource) -or
        $startupWitnessAssertSource -match
            'Read-|Wait-|Get-TurnHistory|Get-World|Get-RoleState|Get-RelayState|' +
            'Get-ClientLog|Get-Dialog|Get-UiHistory|Invoke-RestMethod|Start-Sleep|' +
            'Invoke-Button|/api/ui/invoke|\.Refresh\(') {
        throw 'static check: post-quiet startup witness assertion must remain pure in-memory code'
    }
    foreach ($literal in @(
        'LegacyQuietEntryRoleStates = $LegacyQuietWitness.LegacyQuietEntryRoleStates',
        'PreparedDeployBindings = $null',
        'TerminalMapStateCaptured = $false'
    )) {
        if (-not $startupWitnessSnapshotSource.Contains($literal)) {
            throw "static check: terminal witness shape lost '$literal'"
        }
    }
    foreach ($literal in @(
        'host $Witness.LegacyQuietEntryRoleStates.host $HostProcess',
        'join $Witness.LegacyQuietEntryRoleStates.join $JoinProcess',
        '-not [bool]$Witness.TerminalMapStateCaptured',
        'PreparedDeployBindings = $Witness.PreparedDeployBindings'
    )) {
        if (-not $startupWitnessAssertSource.Contains($literal)) {
            throw "static check: pure terminal witness assertion lost '$literal'"
        }
    }

    $bootstrapObserverSource = [regex]::Match(
        $source,
        '(?ms)^function Wait-OperationalBootstrapAndActionablePair\s*\{.*?(?=^function Invoke-ParallelButtons)'
    ).Value
    if ([string]::IsNullOrEmpty($bootstrapObserverSource) -or
        $bootstrapObserverSource -match
            'Invoke-Button|Register-StartupModalAction|/api/ui/invoke|LegacyStartupWindowsComplete' -or
        -not $bootstrapObserverSource.Contains(
            'Invoke-LiteralStartupPopupTick $window') -or
        -not $bootstrapObserverSource.Contains(
            '$operationalCount -eq 1 -and $bothBare') -or
        -not $bootstrapObserverSource.Contains(
            '$requiredBareQuietMilliseconds = if ($LegacyQuietCheckpointPassed)') -or
        -not $bootstrapObserverSource.Contains('[switch]$SnapshotOnly')) {
        throw 'static check: MSS bootstrap must support a terminal read-only snapshot at quiet-3'
    }
    foreach ($observerName in @('Drive-ToStrategic', 'Wait-ActionablePair')) {
        $observerSource = [regex]::Match(
            $source,
            "(?ms)^function $observerName.*?(?=^function )"
        ).Value
        if ([string]::IsNullOrEmpty($observerSource) -or
            $observerSource -match
                'Invoke-Button|Register-StartupModalAction|/api/ui/invoke|DismissStartupModals') {
            throw "static check: $observerName must remain a read-only native-popup observation gate"
        }
    }

    if ([regex]::Matches(
            $independentNavigationSource,
            'Get-LiteralMainMenuAtLaunchAnchor\s+`?\s*(?:host|join)\b').Count -ne 2) {
        throw 'static check: host and join must each consume one own delayed-injection+11000 ms anchor'
    }
    if ([regex]::Matches($independentNavigationSource,
            '-Role\s+host\s+-Dialog\s+DLG_LOBBY\s+-Button\s+BTN_OK\b').Count -ne 1 -or
        [regex]::Matches($independentNavigationSource,
            '-Role\s+join\s+-Dialog\s+DLG_LOBBY\s+-Button\s+BTN_OK\b').Count -ne 1) {
        throw 'static check: host and join must each retain one exact lobby dispatch in their literal lane'
    }

    $literalReadyProjectionSource = [regex]::Match(
        $source,
        '(?ms)^function Test-LiteralReadyWorldSnapshot\(.*?(?=^function Start-LiteralOuterReadyObserver)'
    ).Value
    if ([string]::IsNullOrEmpty($literalReadyProjectionSource) -or
        -not $literalReadyProjectionSource.Contains(
            '}).Count -lt 1') -or
        -not $literalReadyProjectionSource.Contains(
            "[string](Get-OptionalProperty `$_ 'relation') -eq 'self'") -or
        -not $literalReadyProjectionSource.Contains(
            "[string](Get-OptionalProperty `$_ 'relation') -eq 'enemy'") -or
        [regex]::Matches($literalReadyProjectionSource,
            'Get-LegacyStackRole\s+\$_').Count -ne 2 -or
        $literalReadyProjectionSource.Contains('$stacks.Count') -or
        $literalReadyProjectionSource -match
            'Invoke-RestMethod|Get-Process|Get-World|Get-RelayState|Get-RoleState|Start-Sleep|Wait-') {
        throw 'static check: literal READY must be a pure two-human-owner/stack existence projection'
    }
    $literalReadyWorkerSource = [regex]::Match(
        $source,
        '(?ms)^function Start-LiteralOuterReadyObserver\(.*?(?=^function Publish-LiteralOuterReadyProcess)'
    ).Value
    $readyWorkerOrder = @(
        'for ($sampleIndex = 0; $sampleIndex -lt [int]$MaxSamples; $sampleIndex++)',
        'Start-Sleep -Seconds 2',
        '$publishedIds = @(',
        '@(Get-Process -Id $publishedIds -ErrorAction SilentlyContinue)',
        'if ($publishedIds.Count -ne 2 -or $ownedProcesses.Count -ne 2)',
        '$readyUri = if ($UseLegacyStacks)',
        '"$RelayBase/api/legacy-stacks"',
        '"$RelayBase/api/world?role=host"',
        '$world = Invoke-RestMethod $readyUri -TimeoutSec 6',
        '$State.WorldSampleCount = [long]$State.WorldSampleCount + 1',
        '$State.WorldReadySnapshot = $world',
        '$startupState = $State.StartupObserverState',
        'if (-not [bool]$startupState.OperationalReady)',
        '$State.Completed = $true'
    )
    $previousReadyOffset = -1
    foreach ($literal in $readyWorkerOrder) {
        $offset = $literalReadyWorkerSource.IndexOf(
            $literal, $previousReadyOffset + 1, [StringComparison]::Ordinal)
        if ($offset -le $previousReadyOffset) {
            throw "static check: outer READY sample order lost at '$literal'"
        }
        $previousReadyOffset = $offset
    }
    if ([string]::IsNullOrEmpty($literalReadyWorkerSource) -or
        [regex]::Matches($literalReadyWorkerSource,
            'Start-Sleep\s+-Seconds\s+2').Count -ne 1 -or
        [regex]::Matches($literalReadyWorkerSource,
            'Invoke-RestMethod').Count -ne 1 -or
        [regex]::Matches($literalReadyWorkerSource,
            'Get-Process\s+-Id\s+\$publishedIds').Count -ne 1 -or
        -not $literalReadyWorkerSource.Contains('[int]$MaxSamples') -or
        -not $literalReadyWorkerSource.Contains(
            "[string]`$_.relation -eq 'self'") -or
        -not $literalReadyWorkerSource.Contains(
            "[string]`$_.relation -eq 'enemy'") -or
        -not $literalReadyWorkerSource.Contains(
            "[string]`$_.owner -match '^0x[0-9A-Fa-f]{4}0001`$'") -or
        -not $literalReadyWorkerSource.Contains(
            "[string]`$_.owner -match '^0x[0-9A-Fa-f]{4}0002`$'") -or
        $literalReadyWorkerSource.Contains('$stacks.Count') -or
        $literalReadyWorkerSource -match
            'Popup|ClientLog|Read-SimRelayEvents|/api/ui|Invoke-Button|Start-LiteralButtonRequest') {
        throw 'static check: outer READY must keep 120 relative +2 samples, one process census, then one mode-exact readiness read'
    }
    if ([regex]::Matches(
            $mainSource,
            'Start-LiteralOuterReadyObserver\s+`?\s*-ArmUtc\s+\(\[DateTime\]::UtcNow\)\s+-Fixture\s+\$fixture\s+-MaxSamples\s+120').Count -ne 1 -or
        [regex]::Matches($mainSource,
            'Publish-LiteralOuterReadyStartupObserver\s+`?\s*\$literalReadyObserver\s+\$script:LiteralInnerStartupObserver').Count -ne 1 -or
        [regex]::Matches($pairingSource,
            'Complete-LiteralOuterReadyObserver\s+\$ReadyObserver').Count -ne 1) {
        throw 'static check: outer READY must arm once, receive one inner operational latch, and be consumed once at the old boundary'
    }

    if ($pairingSource.Contains('$sessionListReadyAfter')) {
        throw 'static check: EnumSessions proof must not delay host WaitPeer/startup navigation'
    }
    if (-not $startupWitnessAssertSource.Contains(
            '$sessionListReadyAfter = Measure-LiteralSavedLogMarker') -or
        $startupWitnessAssertSource.Contains('Get-ClientLogMarkerCount')) {
        throw 'static check: deferred EnumSessions proof must consume only the terminal saved log witness'
    }

    $sharedHeroCensusSource = [regex]::Match(
        $source,
        '(?ms)^function Resolve-LiteralHostAuthoritativeHeroes\(.*?(?=^function Assert-DeferredLiteralHostAuthoritativeHeroFixture)'
    ).Value
    $deferredHeroCensusSource = [regex]::Match(
        $source,
        '(?ms)^function Assert-DeferredLiteralHostAuthoritativeHeroFixture\(.*?(?=^function Wait-LiteralDayReady)'
    ).Value
    if ([string]::IsNullOrEmpty($sharedHeroCensusSource) -or
        [regex]::Matches($sharedHeroCensusSource, 'Get-LegacyStackSnapshot').Count -ne 1 -or
        $sharedHeroCensusSource -match
            'Get-WorldSnapshot|Get-World\s|Get-RelayState|Get-RoleState|Start-Sleep|Wait-' -or
        [regex]::Matches($sharedHeroCensusSource, 'Select-Object -First 1').Count -ne 1 -or
        $sharedHeroCensusSource.Contains('$matches.Count') -or
        -not $sharedHeroCensusSource.Contains('explicitly ignores its garrison coordinates')) {
        throw 'static check: dynamic hero resolution must remain one shared census plus source-compatible first-owner selection'
    }
    if ([string]::IsNullOrEmpty($deferredHeroCensusSource) -or
        $deferredHeroCensusSource -match
            'Get-WorldSnapshot|Get-World\s|Get-RelayState|Get-RoleState|Start-Sleep|Wait-|Invoke-' -or
        -not $deferredHeroCensusSource.Contains(
            'deferred literal shared census did not contain the exact $side-owned hero')) {
        throw 'static check: exact fixture validation must consume only the saved pre-deploy census after deploy'
    }
    if ($sharedCensusOffset -le $legacyStartupPassOffset -or
        $canonicalDeployOffset -le $sharedCensusOffset -or
        $deferredHeroCheckOffset -le $canonicalDeployOffset) {
        throw 'static check: one dynamic shared hero census must precede deploy and its strict fixture verdict must follow deploy'
    }

    $localTurnReleaseSource = [regex]::Match(
        $source,
        '(?ms)^function Wait-ExactLocalTurnReleasePair\(.*?(?=^function New-LiteralPreparedEndTurnIntent)'
    ).Value
    if ([string]::IsNullOrEmpty($localTurnReleaseSource) -or
        [regex]::Matches(
            $localTurnReleaseSource,
            'Get-LiteralClientLogMarkerEventUtcSnapshot').Count -ne 2 -or
        -not $localTurnReleaseSource.Contains(
            '$deadlineUtc = $FireCompletedAt.AddSeconds($TimeoutSec)') -or
        -not $localTurnReleaseSource.Contains('if ($hostEvent -and $joinEvent)') -or
        -not $localTurnReleaseSource.Contains('Start-Sleep -Milliseconds 250')) {
        throw 'static check: exact local BeginTurn release lost its two-client shared-deadline observer'
    }
    if ($localTurnReleaseSource -match
            'Invoke-RestMethod|Invoke-(?:Button|Parallel)|Get-World|Get-RoleState|' +
            'Get-RelayState|Read-SimRelayEvents|Wait-(?:Actionable|SimCondition)|Move-Stack') {
        throw 'static check: exact local BeginTurn release bridge must remain passive'
    }

    $independentRoundSource = [regex]::Match(
        $source,
        '(?ms)^function Run-IndependentRound\(.*?(?=^function Wait-CanonicalLegacyMergeRelease)'
    ).Value
    if ([string]::IsNullOrEmpty($independentRoundSource)) {
        throw 'static check: literal independent-round function is not identifiable'
    }
    $roundLiteralOrder = @(
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
        '$fire = if ($Fixture)',
        'Wait-CanonicalLegacyEndTurnRefresh',
        '$hostLinesAfter = @(Read-ClientLogLines $HostLog)',
        '[void](Assert-NoStockEndTurnSnapshotAfter 0)',
        '$trackedProcessesAlive = @(',
        'Get-Process -Id $ownedProcessIds -ErrorAction SilentlyContinue).Count',
        'Start-Sleep -Seconds 4',
        '$legacyHostUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host',
        '$legacyJoinUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host',
        '$legacyVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        'Write-Step "round $Round PASS:',
        '$legacyPass = $true',
        '$localTurnRelease = Wait-ExactLocalTurnReleasePair',
        '$legacyContinuation = & $OnCanonicalLegacyPass',
        '$deferredMssEvidence = [pscustomobject]@{'
    )
    $previousRoundOffset = -1
    foreach ($literal in $roundLiteralOrder) {
        $offset = $independentRoundSource.IndexOf($literal, [StringComparison]::Ordinal)
        if ($offset -le $previousRoundOffset) {
            throw "static check: literal sync_endturn/PASS/day2 order lost at '$literal'"
        }
        $previousRoundOffset = $offset
    }
    $roundPassOffset = $independentRoundSource.IndexOf(
        'Write-Step "round $Round PASS:', [StringComparison]::Ordinal)
    $roundContinuationOffset = $independentRoundSource.IndexOf(
        '$legacyContinuation = & $OnCanonicalLegacyPass', [StringComparison]::Ordinal)
    $passToContinuation = $independentRoundSource.Substring(
        $roundPassOffset,
        $roundContinuationOffset - $roundPassOffset)
    if ([regex]::Matches(
            $passToContinuation,
            'Wait-ExactLocalTurnReleasePair').Count -ne 1) {
        throw 'static check: sync_endturn PASS lost its one exact native-release bridge'
    }
    $passWithoutReleaseBridge = $passToContinuation.Replace(
        'Wait-ExactLocalTurnReleasePair', '')
    if ($passWithoutReleaseBridge -match
            'Get-|Read-|Wait-|Start-Sleep|Assert-(?:Clients|NoClient|Production|NoRelay)') {
        throw 'static check: an observation other than exact native release was inserted before day2 walk'
    }
    $afterRoundContinuation = $independentRoundSource.Substring(
        $roundContinuationOffset)
    $canonicalRoundEndMatch = [regex]::Match(
        $afterRoundContinuation,
        '(?m)^    \} else \{\r?\n        \[void\]\(Wait-SimCondition')
    $canonicalRoundEndOffset = if ($canonicalRoundEndMatch.Success) {
        $roundContinuationOffset + $canonicalRoundEndMatch.Index
    } else { -1 }
    if ($canonicalRoundEndOffset -le $roundContinuationOffset -or
        -not $independentRoundSource.Contains(
            '$roundActionPreparation = New-CanonicalWalkPreparationFromRelayState')) {
        throw 'static check: End Turn must project its current capability from the one source peer-state read'
    }
    $continuationToCanonicalEnd = $independentRoundSource.Substring(
        $roundContinuationOffset,
        $canonicalRoundEndOffset - $roundContinuationOffset)
    if ($continuationToCanonicalEnd -match
            'Get-World|Get-RoleState|Get-RelayState|Read-|Wait-|Start-Sleep|' +
            'Invoke-RestMethod|Assert-ClientsLive|Assert-NoClientFaults|' +
            'Assert-ProductionRelayHealthy|Read-SimRelayEvents') {
        throw 'static check: canonical sync_endturn/day2 handoff must package only saved in-memory evidence'
    }

    $mainDay2Calls = [regex]::Matches(
        $mainSource,
        'Invoke-CanonicalDay2Walk\s+`?').Count
    if ($mainDay2Calls -ne 1 -or
        $mainSource.Contains('-PreparedMetadata $walkPreparedMetadata') -or
        $mainSource.Contains('-PreparedMetadata $attacks.postBattleWalkPreparation') -or
        -not $mainSource.Contains(
            '$fixture postBattleWalk $hostProcess $joinProcess')) {
        throw 'static check: canonical walks must acquire one current capability after their source census, with day2 invoked only by the PASS callback'
    }

    $canonicalMergeMainOffset = $mainSource.IndexOf(
        '$canonicalMergePreparedEvidence = [pscustomobject]@{',
        [StringComparison]::Ordinal)
    if ($canonicalMergeMainOffset -lt 0) {
        throw 'static check: canonical merge main branch is not identifiable'
    }
    $canonicalLegacyMergeOffset = $mainSource.IndexOf(
        '-CanonicalLegacy',
        $canonicalMergeMainOffset,
        [StringComparison]::Ordinal)
    if ($canonicalLegacyMergeOffset -le $canonicalMergeMainOffset) {
        throw 'static check: canonical merge lost its legacy/MSS proof boundary'
    }
    $canonicalRoundProjectionOrder = @(
        '$canonicalRoundProof = $mergeResult.deferredRoundProof',
        '$canonicalProofCascades = @($canonicalRoundProof.cascades)',
        '$roundResults[0].endTurnProof.confirmationCount =',
        '$roundResults[0].endTurnProof.causal = [pscustomobject]@{',
        '$roundResults[0].endTurnProof.cascades = @($canonicalProofCascades)',
        '$roundResults[0].endTurnProof.cascadeOrder ='
    )
    $previousProjectionOffset = $canonicalLegacyMergeOffset
    foreach ($literal in $canonicalRoundProjectionOrder) {
        $offset = $mainSource.IndexOf(
            $literal,
            $previousProjectionOffset + 1,
            [StringComparison]::Ordinal)
        if ($offset -le $previousProjectionOffset) {
            throw "static check: canonical deferred round projection lost or moved at '$literal'"
        }
        $previousProjectionOffset = $offset
    }
    $canonicalNonLegacyMergeOffset = $mainSource.IndexOf(
        '$mergeResult = Run-MergeBarrier `',
        $previousProjectionOffset + 1,
        [StringComparison]::Ordinal)
    if ($canonicalNonLegacyMergeOffset -le $previousProjectionOffset -or
        [regex]::Matches(
            $mainSource,
            '\$canonicalRoundProof = \$mergeResult\.deferredRoundProof').Count -ne 1) {
        throw ('static check: exactly one canonical round must receive its already-proved ' +
            'deferred evidence after the legacy/MSS merge return')
    }
    $canonicalProjectionSource = $mainSource.Substring(
        $canonicalLegacyMergeOffset + '-CanonicalLegacy'.Length,
        $canonicalNonLegacyMergeOffset -
            ($canonicalLegacyMergeOffset + '-CanonicalLegacy'.Length))
    if ($canonicalProjectionSource -match
            'Get-|Read-|Wait-|Invoke-|Start-Sleep|Run-MergeBarrier') {
        throw 'static check: canonical deferred round projection must remain in-memory only'
    }

    $mergeWaitSource = [regex]::Match(
        $source,
        '(?ms)^function Wait-CanonicalLegacyMergeRelease\(.*?(?=^function Assert-LegacyMergeNoOverrotation)'
    ).Value
    $mergeWaitOrder = @(
        'Wait-FixedUtcAnchor $observeAt',
        '$hostCount = Get-ClientLogMarkerCount $HostLog $HostMarker',
        '$joinCount = Get-ClientLogMarkerCount $JoinLog $JoinMarker',
        'Write-Step ("legacy merge observation +${elapsed}s:',
        '$firstObservationSec = $elapsed',
        'return [pscustomobject]@{',
        'hostCount = [int]($hostCount - $HostBefore)',
        'joinCount = [int]($joinCount - $JoinBefore)'
    )
    $previousMergeWaitOffset = -1
    foreach ($literal in $mergeWaitOrder) {
        $offset = $mergeWaitSource.IndexOf($literal, [StringComparison]::Ordinal)
        if ($offset -le $previousMergeWaitOffset) {
            throw "static check: literal +3 merge-count cadence lost at '$literal'"
        }
        $previousMergeWaitOffset = $offset
    }
    if ($mergeWaitSource -match
            'Assert-ClientsLive|Assert-NoClientFaults|Assert-ProductionRelayHealthy|' +
            'Read-SimRelayEvents|Get-RelayState|Get-World|Get-RoleState|Get-Dialog|' +
            'legacy local merge application did not appear exactly once') {
        throw 'static check: no diagnostic observer may be inserted between fixed +3 merge samples'
    }

    $mergeBarrierSource = [regex]::Match(
        $source,
        '(?ms)^function Run-MergeBarrier\(.*?(?=^function Run-PostMergeStockProof)'
    ).Value
    if ([string]::IsNullOrEmpty($mergeBarrierSource)) {
        throw 'static check: merge barrier function is not identifiable'
    }
    $mergePreparationOrder = @(
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
        '$canonicalIntent = New-LiteralPreparedEndTurnIntent',
        '$fire = if ($isLegacyParallel)',
        '$legacyMergeObservation = Wait-CanonicalLegacyMergeRelease',
        'Start-Sleep -Seconds 5',
        '$legacyMergeVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        '$legacyCascadeAfterLines = @(Read-ClientLogLines $HostLog)',
        '$legacySuppressLines = @(Read-ClientLogLines $HostLog)',
        '$legacyRotationLines = @(Read-ClientLogLines $HostLog)',
        '$legacyHostUiState = Get-GameUiSnapshot host',
        '$legacyHostDialog = [string](Get-OptionalProperty $legacyHostUiState ''dialog'')',
        '$legacyOverRotated = ($legacyRotationCount -ge 1) -or',
        '$legacyHostUi = ConvertTo-SavedDialogObservation host $legacyHostUiState',
        '$legacyPreClickTurnHistory = Get-TurnHistory -After 0',
        '$firstHostIntent = New-CanonicalLegacyHostEndTurnIntent $legacyHostUiState',
        '$firstHostStep = Invoke-CanonicalLegacyPostMergeHostEndTurnProbe',
        '$legacyBarrierPass = (',
        'deadClickConfirmed = if ([bool]$firstHostStep.rotated)',
        "Write-Step ('BARRIER RESULT: PASS",
        '$firstHostStep = Assert-DeferredPostMergeProbeEvidence',
        '$events = @(Read-SimRelayEvents)',
        '$deferredRoundProof = Assert-DeferredCanonicalRoundEvidence',
        'Assert-ClientLogMarkerDelta $HostLog $naturalMergeMarker',
        '$hostDayAfterMerge = Get-WorldDay host'
    )
    $previousMergeOffset = -1
    foreach ($literal in $mergePreparationOrder) {
        $offset = $mergeBarrierSource.IndexOf($literal, [StringComparison]::Ordinal)
        if ($offset -le $previousMergeOffset) {
            throw "static check: literal barrier/MSS-extension order lost at '$literal'"
        }
        $previousMergeOffset = $offset
    }
    if (-not $mergeBarrierSource.Contains(
            '$mergeReleasedMarker $mergeReleasedMarker') -or
        -not $mergeBarrierSource.Contains(
            '$hostMergeReleasedBefore $joinMergeReleasedBefore $fire.completedAt 45') -or
        $mergeBarrierSource.Contains(
            '$hostLocalMergeMarker $joinLocalMergeMarker `')) {
        throw 'static check: canonical legacy merge watcher must observe both exact global releases'
    }
    if ($mergeBarrierSource.Contains(
            'deferred canonical fixture MP mismatch before merge')) {
        throw 'static check: barrier observation regained a historical 33/29 MP gate'
    }
    if (-not $mergeBarrierSource.Contains(
            '$canonicalIntent -RelayHealthAlreadyAsserted') -or
        -not $mergeBarrierSource.Contains(
            '-PreparedEvidence $postMergeProbeBaseline')) {
        throw 'static check: canonical barrier dispatch regained a late relay/probe baseline read'
    }
    $mergeBeforeMpOffset = $mergeBarrierSource.IndexOf(
        '$legacyMergeVitalsBefore = Get-LegacySequentialSharedMovementSnapshot $Fixture',
        [StringComparison]::Ordinal)
    $mergeFireOffset = $mergeBarrierSource.IndexOf(
        '$fire = if ($isLegacyParallel)', [StringComparison]::Ordinal)
    $afterCanonicalMergeMp = $mergeBarrierSource.Substring($mergeBeforeMpOffset)
    $canonicalMergeBranchEndMatch = [regex]::Match(
        $afterCanonicalMergeMp,
        '(?m)^    \} else \{\r?\n        Assert-OperationalActionGate')
    if (-not $canonicalMergeBranchEndMatch.Success) {
        throw 'static check: canonical/noncanonical merge prelude boundary is not identifiable'
    }
    $canonicalMergeBranchEndOffset = $mergeBeforeMpOffset +
        $canonicalMergeBranchEndMatch.Index
    $mpToCanonicalBranchEnd = $mergeBarrierSource.Substring(
        $mergeBeforeMpOffset,
        $canonicalMergeBranchEndOffset - $mergeBeforeMpOffset)
    if ($mpToCanonicalBranchEnd -match
            'Read-SimRelayEvents|Get-WorldDay|Get-DialogObservation|' +
            'Get-ClientLogMarkerCount|Assert-ProductionRelayHealthy|Get-RoleState|' +
            'Get-RelayState|Get-WorldSnapshot') {
        throw 'static check: only the source host cascade watermark may appear between barrier MP and fire'
    }
    if ([regex]::Matches(
            $mpToCanonicalBranchEnd,
            'Read-ClientLogLines\s+\$HostLog').Count -ne 1) {
        throw 'static check: barrier pre-fire lost its one source cascade-log watermark'
    }
    $sharedMergeBaselineOffset = $mergeBarrierSource.IndexOf(
        '$sessionPlan = Get-SessionPlanEvent $before',
        $canonicalMergeBranchEndOffset,
        [StringComparison]::Ordinal)
    if ($sharedMergeBaselineOffset -lt 0 -or
        $mergeFireOffset -le $sharedMergeBaselineOffset) {
        throw 'static check: shared pure merge baseline/fire boundary is not identifiable'
    }
    $sharedMergeBaselineToFire = $mergeBarrierSource.Substring(
        $sharedMergeBaselineOffset,
        $mergeFireOffset - $sharedMergeBaselineOffset)
    if ($sharedMergeBaselineToFire -match
            'Read-ClientLogLines|Read-SimRelayEvents|Get-WorldDay|' +
            'Get-WorldSnapshot|Get-RoleState|Get-RelayState|' +
            'Get-DialogObservation|Get-GameUiSnapshot|Start-Sleep|' +
            'Assert-ProductionRelayHealthy|Assert-ClientsLive|Assert-NoClientFaults') {
        throw 'static check: shared merge baselines must remain pure until the sole fire'
    }

    $legacySuppressReadOffset = $mergeBarrierSource.IndexOf(
        '$legacySuppressLines = @(Read-ClientLogLines $HostLog)',
        [StringComparison]::Ordinal)
    $legacyRotationReadOffset = $mergeBarrierSource.IndexOf(
        '$legacyRotationLines = @(Read-ClientLogLines $HostLog)',
        [StringComparison]::Ordinal)
    if ($legacySuppressReadOffset -lt 0 -or
        $legacyRotationReadOffset -le $legacySuppressReadOffset) {
        throw 'static check: separate suppress/rotation legacy log observations are missing'
    }
    $legacySuppressObservationSource = $mergeBarrierSource.Substring(
        $legacySuppressReadOffset,
        $legacyRotationReadOffset - $legacySuppressReadOffset)
    if ($legacySuppressObservationSource -match
            '(?m)^\s*(?:if\s*\(|throw\b)' -or
        [regex]::Matches(
            $legacySuppressObservationSource,
            'Read-ClientLogLines\s+\$HostLog').Count -ne 1) {
        throw 'static check: pre-PASS suppress read must remain one observation-only source step'
    }

    $legacyUiReadOffset = $mergeBarrierSource.IndexOf(
        '$legacyHostUiState = Get-GameUiSnapshot host',
        [StringComparison]::Ordinal)
    $legacyStockBaselineOffset = $mergeBarrierSource.IndexOf(
        '$legacyPreClickTurnHistory = Get-TurnHistory -After 0',
        [StringComparison]::Ordinal)
    if ($legacyUiReadOffset -le $legacyRotationReadOffset -or
        $legacyStockBaselineOffset -le $legacyUiReadOffset -or
        [regex]::Matches(
            $mergeBarrierSource,
            '\$legacyHostUiState = Get-GameUiSnapshot host').Count -ne 1 -or
        [regex]::Matches(
            $mergeBarrierSource,
            '\$legacyPreClickTurnHistory = Get-TurnHistory -After 0').Count -ne 1) {
        throw 'static check: legacy over-rotation UI and stock baseline must remain one read each'
    }
    $legacyOverRotationSource = $mergeBarrierSource.Substring(
        $legacyUiReadOffset,
        $legacyStockBaselineOffset - $legacyUiReadOffset)
    if ([regex]::Matches(
            $legacyOverRotationSource,
            '(?s)\$legacyOverRotated\s*=\s*\(\$legacyRotationCount -ge 1\)\s*-or\s*' +
            '\(\$legacyHostDialog -eq ''DLG_ISO_PAL''\)').Count -ne 1 -or
        $legacyOverRotationSource -match
            'if\s*\([^\r\n]*(?:legacyHostReady|BareMapDialogs)|' +
            'legacyHostUi\.Ready|BareMapDialogs') {
        throw 'static check: legacy over-rotation gate must be rotation>=1 OR DLG_ISO_PAL only'
    }

    $legacyIntentSource = [regex]::Match(
        $source,
        '(?ms)^function New-CanonicalLegacyHostEndTurnIntent\(.*?(?=^function New-EndTurnAction\()'
    ).Value
    if ([string]::IsNullOrEmpty($legacyIntentSource) -or
        -not $legacyIntentSource.Contains("dialog = 'DLG_STRATEGIC'") -or
        -not $legacyIntentSource.Contains("button = 'BTN_END_TURN'") -or
        -not $legacyIntentSource.Contains('afterUiSequence = [long]($uiSequence - 1)') -or
        $legacyIntentSource -match
            '\.Ready|dialogReady|BareMapDialogs|Get-GameUiSnapshot|' +
            'Get-DialogObservation|Read-|Wait-|Invoke-|Start-Sleep') {
        throw 'static check: canonical legacy intent must remain a pure symbolic transform without a readiness/dialog gate'
    }

    $legacyPassExpressionOffset = $mergeBarrierSource.IndexOf(
        '$legacyBarrierPass = (', [StringComparison]::Ordinal)
    $legacyEvidenceOffset = $mergeBarrierSource.IndexOf(
        '$legacyBarrierEvidence = [pscustomobject]@{',
        $legacyPassExpressionOffset,
        [StringComparison]::Ordinal)
    if ($legacyPassExpressionOffset -le $legacyStockBaselineOffset -or
        $legacyEvidenceOffset -le $legacyPassExpressionOffset) {
        throw 'static check: canonical legacy verdict expression is not identifiable'
    }
    $legacyPassExpression = $mergeBarrierSource.Substring(
        $legacyPassExpressionOffset,
        $legacyEvidenceOffset - $legacyPassExpressionOffset)
    foreach ($literal in @(
        '[int]$legacyMergeObservation.hostCount -eq 1',
        '[int]$legacyMergeObservation.joinCount -eq 1',
        '$legacyHostRefreshed',
        '[int]$legacyCascadeNew -eq 0',
        '[bool]$firstHostStep.rotated',
        '-not $legacyOverRotated',
        '[int]$firstHostStep.trackedProcessesAlive -eq 2'
    )) {
        if (-not $legacyPassExpression.Contains($literal)) {
            throw "static check: canonical legacy verdict lost '$literal'"
        }
    }
    if ($legacyPassExpression -match
            'legacySuppressCount|deadClick|Assert-|Get-|Read-|Wait-|Invoke-|Start-Sleep') {
        throw 'static check: canonical legacy verdict regained a non-source gate or observation'
    }
    if (-not $mergeBarrierSource.Contains(
            'deadClickConfirmed = if ([bool]$firstHostStep.rotated) { $false } else { $null }') -or
        -not $mergeBarrierSource.Contains(
            'deadClickConfirmedKnown = [bool]$firstHostStep.rotated') -or
        -not $mergeBarrierSource.Contains(
            'staleSuppressSuspect = [bool]$firstHostStep.staleSuppressSuspect')) {
        throw 'static check: dead-click may be known-false only after observed rotation; +15 no-rotation stays suspect/unknown'
    }

    $barrierPassOffset = $mergeBarrierSource.IndexOf(
        "Write-Step ('BARRIER RESULT: PASS", [StringComparison]::Ordinal)
    $deferredProbeOffset = $mergeBarrierSource.IndexOf(
        '$firstHostStep = Assert-DeferredPostMergeProbeEvidence',
        [StringComparison]::Ordinal)
    $firstCanonicalEventReadOffset = $mergeBarrierSource.IndexOf(
        '$events = @(Read-SimRelayEvents)',
        $barrierPassOffset,
        [StringComparison]::Ordinal)
    if ($barrierPassOffset -lt 0 -or
        $deferredProbeOffset -le $barrierPassOffset -or
        $firstCanonicalEventReadOffset -le $deferredProbeOffset) {
        throw 'static check: post-merge MSS health/event extensions must remain strictly after BARRIER PASS'
    }

    $postMergeProbeSource = [regex]::Match(
        $source,
        '(?ms)^function Invoke-CanonicalLegacyPostMergeHostEndTurnProbe\(.*?(?=^function Assert-DeferredPostMergeProbeEvidence)'
    ).Value
    $postMergeDeferredSource = [regex]::Match(
        $source,
        '(?ms)^function Assert-DeferredPostMergeProbeEvidence\(.*?(?=^function Assert-LiteralMasstestProcessAlive)'
    ).Value
    if ([string]::IsNullOrEmpty($postMergeProbeSource) -or
        [string]::IsNullOrEmpty($postMergeDeferredSource)) {
        throw 'static check: canonical legacy probe/deferred proof is not identifiable'
    }
    $postMergeProbeOrder = @(
        'Get-LegacyHostCompletedStockTurnCount $PreClickTurnHistory',
        '$invokeResult = Invoke-ButtonWhenReady',
        '$preClickHostUi = ConvertTo-SavedDialogObservation',
        '[DateTime]$firedAt = [DateTime]::UtcNow',
        'for ($elapsed = 3; $elapsed -le $TimeoutSec; $elapsed += 3)',
        'Wait-FixedUtcAnchor ($firedAt.AddSeconds($elapsed))',
        '$turnHistory = Get-TurnHistory -After 0',
        'Get-LegacyHostCompletedStockTurnCount $turnHistory',
        '$samples.Add($sample)',
        'if ($completedStockTurnAfter -gt $completedStockTurnBaseline)',
        '$rotationSample = $sample',
        '$firstObservationSec = $elapsed',
        'break',
        '$ownedProcesses = @(Get-Process'
    )
    $previousProbeOffset = -1
    foreach ($literal in $postMergeProbeOrder) {
        $offset = $postMergeProbeSource.IndexOf(
            $literal, $previousProbeOffset + 1, [StringComparison]::Ordinal)
        if ($offset -le $previousProbeOffset) {
            throw "static check: canonical one-action/+3 poll order lost at '$literal'"
        }
        $previousProbeOffset = $offset
    }
    if ([regex]::Matches(
            $postMergeProbeSource,
            'Invoke-ButtonWhenReady\s+').Count -ne 1 -or
        [regex]::Matches(
            $postMergeProbeSource,
            'Invoke-Button(?!WhenReady)\s+').Count -ne 0 -or
        [regex]::Matches(
            $postMergeProbeSource,
            '\$turnHistory\s*=\s*Get-TurnHistory\s+-After\s+0').Count -ne 1 -or
        [regex]::Matches(
            $postMergeProbeSource,
            'Wait-FixedUtcAnchor\s+\(\$firedAt\.AddSeconds\(\$elapsed\)\)').Count -ne 1 -or
        [regex]::Matches(
            $postMergeProbeSource,
            '\$ownedProcesses\s*=\s*@\(Get-Process').Count -ne 1 -or
        -not $postMergeProbeSource.Contains('$TimeoutSec -ne 15') -or
        $postMergeProbeSource -match
            'Assert-StockTurnEvidencePrefix|Assert-LegacyMergeNoOverrotation|' +
            'Assert-ClientsLive|Assert-NoClientFaults|Assert-ProductionRelayHealthy|' +
            'Read-ClientLogLines|Read-SimRelayEvents|Get-World|Get-RoleState|' +
            'Get-GameUiSnapshot|Start-Sleep|while\s*\(') {
        throw 'static check: canonical post-merge probe must remain one action, +3 count reads, then PID census'
    }

    $postMergeDeferredOrder = @(
        'Assert-ClientsLive $HostProcess $JoinProcess',
        'Assert-NoClientFaults $HostLog $JoinLog',
        'Assert-ProductionRelayHealthy',
        '$preClickEvidence = Assert-LegacyMergeNoOverrotation',
        '-History $Probe.preClickTurnHistory',
        '$stockEvidence = Assert-StockTurnEvidencePrefix',
        '$preClickHostUi = $Probe.preClickHostUi',
        '$script:BareMapDialogs -notcontains $preClickHostUi.Dialog',
        'if ([int]$Probe.suppressCount -ne 0)',
        '$confirmationLines = @(Read-ClientLogLines $HostLog',
        '$hostDay = Get-WorldDay host',
        '$joinDay = Get-WorldDay join',
        '$Probe.evidence = $stockEvidence',
        '$Probe.currentOwner = [pscustomobject]@{'
    )
    $previousDeferredOffset = -1
    foreach ($literal in $postMergeDeferredOrder) {
        $offset = $postMergeDeferredSource.IndexOf(
            $literal, $previousDeferredOffset + 1, [StringComparison]::Ordinal)
        if ($offset -le $previousDeferredOffset) {
            throw "static check: deferred canonical merge proof lost '$literal'"
        }
        $previousDeferredOffset = $offset
    }
    if ($postMergeDeferredSource -match 'Invoke-Button|Start-Sleep|Wait-FixedUtcAnchor') {
        throw 'static check: deferred canonical merge proof regained an action or clock edge'
    }

    $orderedDriveSource = [regex]::Match(
        $source,
        '(?ms)^function Run-LiteralOrderedMasstestToMerge\(.*?(?=^function Complete-LiteralOrderedMasstestDeferredProof)'
    ).Value
    $orderedActionSource = [regex]::Match(
        $source,
        '(?ms)^function Invoke-LiteralMasstestEndTurnOnce\(.*?(?=^function Read-LiteralMasstestPeerStateCheckpoint)'
    ).Value
    $orderedCheckpointSource = [regex]::Match(
        $source,
        '(?ms)^function Read-LiteralMasstestPeerStateCheckpoint\(.*?(?=^function Assert-LiteralMasstestRoleEndTurnHistory)'
    ).Value
    $automaticActionSource = [regex]::Match(
        $source,
        '(?ms)^function Invoke-AutomaticMasstestHumanEndTurnOnce\(.*?(?=^function Run-AutomaticMasstestPhaseCLiteral)'
    ).Value
    $automaticPhaseCSource = [regex]::Match(
        $source,
        '(?ms)^function Run-AutomaticMasstestPhaseCLiteral\(.*?(?=^function Complete-LiteralOrderedMasstestLegacyOracle)'
    ).Value
    $legacyMasstestOracleSource = [regex]::Match(
        $source,
        '(?ms)^function Complete-LiteralOrderedMasstestLegacyOracle\(.*?(?=^function Assert-DeferredCanonicalRoundEvidence)'
    ).Value
    $orderedDeferredSource = [regex]::Match(
        $source,
        '(?ms)^function Complete-LiteralOrderedMasstestDeferredProof\(.*?(?=^function Read-AutomaticMasstestHostBeginTurnSlots)'
    ).Value
    if (@(
            $orderedDriveSource,
            $orderedActionSource,
            $orderedCheckpointSource,
            $automaticActionSource,
            $automaticPhaseCSource,
            $legacyMasstestOracleSource,
            $orderedDeferredSource
        ) | Where-Object { [string]::IsNullOrEmpty($_) }) {
        throw 'static check: standalone ordered masstest topology is not identifiable'
    }
    if ([regex]::Matches(
            $orderedDriveSource,
            'Invoke-LiteralMasstestEndTurnOnce').Count -ne 4 -or
        [regex]::Matches(
            $orderedDriveSource,
            'AddSeconds\(4\)').Count -ne 4 -or
        [regex]::Matches(
            $orderedDriveSource,
            'Read-LiteralMasstestPeerStateCheckpoint').Count -ne 2 -or
        [regex]::Matches(
            $orderedDriveSource,
            '\$events = @\(Read-SimRelayEvents\)').Count -ne 2 -or
        [regex]::Matches(
            $orderedDriveSource,
            'Wait-SimCondition').Count -ne 1 -or
        $orderedDriveSource -notmatch (
            '(?s)\$events = @\(Read-SimRelayEvents\)[\s\S]*' +
            '\$fixedMergeStage = Assert-LiteralOrderedMasstestStage[\s\S]*' +
            '-AllowPendingHostMerge[\s\S]*' +
            '\$completionEvents = Wait-SimCondition[\s\S]*' +
            '-TimeoutSec 8[\s\S]*' +
            'Get-SimEventMatches \$current ''merge-applied'' ''host''[\s\S]*' +
            '\$hostMergeCount -gt 1[\s\S]*\$hostMergeCount -eq 1[\s\S]*' +
            '\$stage = Assert-LiteralOrderedMasstestStage\s+`\s*' +
            '\$completionEvents laggard-merge') -or
        $orderedDriveSource -match
            'Wait-LiteralDayReady|Resolve-LiteralHostAuthoritativeHeroes|' +
            'Invoke-CanonicalDeploy|Invoke-CanonicalConcurrentAttacks|' +
            'Invoke-CanonicalParallelWalk|Run-IndependentRound|Run-MergeBarrier') {
        throw ('static check: ordered masstest must retain four ET/+4 source ' +
            'samples and one bounded read-only merge completion')
    }
    if ([regex]::Matches(
            $orderedActionSource,
            'Invoke-ButtonWhenReady\s+').Count -ne 1 -or
        [regex]::Matches(
            $orderedActionSource,
            'Invoke-Button(?!WhenReady)\s+').Count -ne 0 -or
        $orderedActionSource -notmatch (
            '(?s)-AfterUiSequence \$afterUiSequence[\s\S]*' +
            '-WaitMilliseconds 26000[\s\S]*-CommandTimeoutMilliseconds 8000') -or
        $orderedActionSource -match
            'New-EndTurnAction\s|Get-|Read-|Wait-|while \(|for \(|catch\s*\{') {
        throw 'static check: ordered distinct-day action must consume one prepared watermark and arm once'
    }
    if ([regex]::Matches($orderedCheckpointSource, 'Get-RelayState').Count -ne 1 -or
        [regex]::Matches(
            $orderedCheckpointSource,
            'New-EndTurnActionFromObservation').Count -ne 0 -or
        [regex]::Matches(
            $orderedCheckpointSource,
            'ConvertTo-SavedDialogObservation').Count -ne 0 -or
        -not $orderedCheckpointSource.Contains(
            'afterUiSequence = [long]($uiSequence - 1)') -or
        $orderedCheckpointSource -match 'Start-Sleep|Wait-|Invoke-Button') {
        throw 'static check: each ordered state checkpoint must be one read plus pure UI-watermark projection'
    }
    if ($orderedDeferredSource -notmatch (
            '(?s)MergeHandoff\.fixedMergeStage\.savedEvents[\s\S]*' +
            '-AllowPendingHostMerge[\s\S]*' +
            '\$completedSourceStage[\s\S]*' +
            'MergeHandoff\.mergeStage\.savedEvents[\s\S]*' +
            'Assert-DeferredLiteralOrderedMasstestStage') -or
        $mainSource -notmatch (
            '(?s)orderedMasstestResult = \[pscustomobject\]@\{[\s\S]*' +
            'fixedMergeClassification[\s\S]*fixedMergeObserved[\s\S]*' +
            'completionMergeObserved[\s\S]*mergeCompletionTimeoutSeconds')) {
        throw ('static check: ordered fixed-window classification, bounded ' +
            'completion, summary, and deferred proof are no longer distinct')
    }
    $automaticOrder = @(
        'Wait-FixedUtcAnchor ($handoffCompletedUtc.AddSeconds(4))',
        'while ($true)',
        '$neutralObservation = if ($mergeBoundaryResolved)',
        'if ($completedNeutralRounds -ge 2 -or $outerTurns -ge 24) { break }',
        'if (-not (Test-AutomaticMasstestHostAlive $HostProcess)) { break }',
        '$slotObservation = Get-AutomaticMasstestLatestHostSlot',
        'Start-Sleep -Seconds 2',
        '$outerTurns++',
        '[bool]$humanActionSubmitted = $false',
        '[int]$beforeSlotCount = [int]$slotObservation.Count',
        '[DateTime]$advanceDeadlineUtc = [DateTime]::UtcNow.AddSeconds(26)',
        'Start-Sleep -Milliseconds 800',
        '$hostDiedDuringAdvance = $true',
        '$freshSlotObservation = Get-AutomaticMasstestLatestHostSlot',
        'if (-not $slotAdvanced -and $humanActionSubmitted -and',
        '$outerTurns++',
        '$finalNeutralObservation = Get-AutomaticMasstestNeutralCount',
        'automatic masstest Phase C drive completed:'
    )
    $previousAutomaticOffset = -1
    foreach ($literal in $automaticOrder) {
        $offset = $automaticPhaseCSource.IndexOf(
            $literal, $previousAutomaticOffset + 1, [StringComparison]::Ordinal)
        if ($offset -le $previousAutomaticOffset) {
            throw "static check: literal automatic Phase C order lost at '$literal'"
        }
        $previousAutomaticOffset = $offset
    }
    if ($automaticPhaseCSource -match
            'Wait-ActionablePair|Start-Sleep -Milliseconds 250|' +
            'BTN_(?:OK|YES|NO)|Invoke-StockEndTurnAndObserve|' +
            'Invoke-StockCycleTailAndObserve|AUTOMATIC MASSTEST PHASE C RESULT: PASS|' +
            '\$completedNeutralRounds -lt 2') {
        throw 'static check: literal automatic Phase C regained a non-source popup/wait/telemetry driver'
    }
    if ([regex]::Matches(
            $automaticPhaseCSource,
            'Invoke-AutomaticMasstestHumanEndTurnOnce host').Count -ne 1 -or
        [regex]::Matches(
            $automaticPhaseCSource,
            'Invoke-AutomaticMasstestHumanEndTurnOnce join').Count -ne 1 -or
        [regex]::Matches(
            $automaticPhaseCSource,
            'Test-AutomaticMasstestHostAlive \$HostProcess').Count -ne 2 -or
        $automaticPhaseCSource -notmatch (
            '(?s)\$humanActionSubmitted[\s\S]*\$slotAdvanced[\s\S]*' +
            '\$hostDiedDuringAdvance[\s\S]*\$humanSlotStalled = \$true[\s\S]*break')) {
        throw 'static check: Phase C lost host-death/human-stall handoff to the legacy oracle'
    }
    if ([regex]::Matches(
            $automaticPhaseCSource,
            '\$slotSamples\.Add\(').Count -ne 4 -or
        -not $automaticPhaseCSource.Contains('slotSamples = @($slotSamples)')) {
        throw 'static check: Phase C must retain every distinct old Get-SlotLines-equivalent sample'
    }
    if (-not $automaticPhaseCSource.Contains(
            '[long]$latestSlotHistoryOrigin = 0') -or
        [regex]::Matches(
            $automaticPhaseCSource,
            '\$latestSlotHistoryOrigin \$hostHandle \$joinHandle \$neutralHandle').Count -ne 2) {
        throw 'static check: Phase C latest-slot reads must retain the old full-history Get-SlotLines origin'
    }
    if ([regex]::Matches(
            $automaticActionSource,
            'Invoke-ButtonWhenReady\s+').Count -ne 1 -or
        [regex]::Matches($automaticActionSource, 'Get-RoleState').Count -ne 1 -or
        [regex]::Matches(
            $automaticActionSource,
            'Invoke-Button(?!WhenReady)\s+').Count -ne 0 -or
        $automaticActionSource -notmatch (
            '(?s)Get-RoleState \$Role[\s\S]*' +
            '''uiSeq''[\s\S]*-AfterUiSequence \(\[long\]\(\$uiSequence - 1\)\)[\s\S]*' +
            '-WaitMilliseconds 26000[\s\S]*-CommandTimeoutMilliseconds 8000') -or
        $automaticActionSource -match 'while \(|for \(|catch\s*\{') {
        throw 'static check: Phase C human slot must arm one terminal exact-ready action from one labelled adapter watermark'
    }
    $legacyOracleOrder = @(
        '$HostProcess.Refresh()',
        '$hostAlive = -not $HostProcess.HasExited',
        '$JoinProcess.Refresh()',
        '$joinerAlive = -not $JoinProcess.HasExited',
        '$hostLines = @(Read-ClientLogLines $HostLog)',
        '$mergeFired = [bool]($mergeMatches.Count -gt 0)',
        '$naturalMergeMarker =',
        '$uiMarshalled = [bool](@($hostLines |',
        '$uefAfterMerge = @($after | Select-String -Pattern ''\[UEF'').Count',
        '$vehLines = @($after | Select-String -Pattern ''\[VEH .*code=0xC0000005'')',
        '$benign = (''findBtn AV|auto-dismiss|nav will re'' +',
        '$currentDumpSnapshot = Get-LiteralInnerDumpBaseline',
        '$verdict = ''PASS''',
        'if (-not $hostAlive)',
        'elseif ($dumps -gt 0 -or $uefAfterMerge -gt 0)',
        'elseif (-not $mergeFired)',
        'elseif ($suspect -gt 0)',
        'elseif ([int]$PhaseC.completedNeutralRounds -lt 2)'
    )
    $previousLegacyOracleOffset = -1
    foreach ($literal in $legacyOracleOrder) {
        $offset = $legacyMasstestOracleSource.IndexOf(
            $literal, $previousLegacyOracleOffset + 1, [StringComparison]::Ordinal)
        if ($offset -le $previousLegacyOracleOffset) {
            throw "static check: literal post-Phase-C oracle order lost at '$literal'"
        }
        $previousLegacyOracleOffset = $offset
    }
    if ([regex]::Matches(
            $legacyMasstestOracleSource,
            'Read-ClientLogLines \$HostLog').Count -ne 1 -or
        [regex]::Matches(
            $legacyMasstestOracleSource,
            'Get-LiteralInnerDumpBaseline').Count -ne 1 -or
        $legacyMasstestOracleSource -match
            'Read-ClientLogLines \$JoinLog|Read-SimRelayEvents|Get-RelayState|' +
            'Get-RoleState|Get-World|Assert-ClientsLive|Assert-NoClientFaults|' +
            'Invoke-Button|Start-Sleep|Wait-') {
        throw 'static check: legacy post-Phase-C oracle added an action, pause, join-log, or MSS read'
    }
    foreach ($literal in @(
        'merge-execute-dispatched',
        'merge-applied',
        'merge-released',
        'natural merge BeginTurn applied and drained',
        'relay released stock turns',
        'deferred MSS ordered-masstest proof PASS after literal Phase C'
    )) {
        if (-not ($source.Contains($literal))) {
            throw "static check: ordered merge/deferred proof lost '$literal'"
        }
    }
    $mainInnerCompleteOffset = $mainSource.IndexOf(
        '$literalInnerStartupResult = Complete-LiteralInnerStartupObserver',
        [StringComparison]::Ordinal)
    $mainCallerSettleOffset = $mainSource.IndexOf(
        'Start-Sleep -Seconds 2', $mainInnerCompleteOffset,
        [StringComparison]::Ordinal)
    $mainInitialStateOffset = $mainSource.IndexOf(
        '$orderedInitialCheckpoint = Read-LiteralMasstestPeerStateCheckpoint',
        [StringComparison]::Ordinal)
    $mainDumpBaselineOffset = $mainSource.IndexOf(
        '$orderedMasstestDumpBaseline = Get-LiteralInnerDumpBaseline',
        [StringComparison]::Ordinal)
    $mainDriveOffset = $mainSource.IndexOf(
        '$orderedMergeHandoff = Run-LiteralOrderedMasstestToMerge',
        [StringComparison]::Ordinal)
    $mainAutomaticOffset = $mainSource.IndexOf(
        '$literalPhaseC = Run-AutomaticMasstestPhaseCLiteral',
        [StringComparison]::Ordinal)
    $mainLegacyOracleOffset = $mainSource.IndexOf(
        '$legacyPhaseCOracle = Complete-LiteralOrderedMasstestLegacyOracle',
        [StringComparison]::Ordinal)
    $mainLegacyVerdictOffset = $mainSource.IndexOf(
        "if ([string]`$legacyPhaseCOracle.legacyVerdict -ne 'PASS')",
        [StringComparison]::Ordinal)
    $mainDeferredOffset = $mainSource.IndexOf(
        '$deferredPhaseCMss = Complete-LiteralOrderedMasstestDeferredProof',
        [StringComparison]::Ordinal)
    if ($mainInnerCompleteOffset -lt 0 -or
        $mainCallerSettleOffset -le $mainInnerCompleteOffset -or
        $mainInitialStateOffset -le $mainCallerSettleOffset -or
        $mainDumpBaselineOffset -le $mainInitialStateOffset -or
        $mainDriveOffset -le $mainDumpBaselineOffset -or
        $mainAutomaticOffset -le $mainDriveOffset -or
        $mainLegacyOracleOffset -le $mainAutomaticOffset -or
        $mainLegacyVerdictOffset -le $mainLegacyOracleOffset -or
        $mainDeferredOffset -le $mainLegacyVerdictOffset -or
        [regex]::Matches(
            $mainSource,
            '\$literalPhaseC = Run-AutomaticMasstestPhaseCLiteral').Count -ne 1 -or
        [regex]::Matches(
            $mainSource,
            '\$orderedInitialCheckpoint = Read-LiteralMasstestPeerStateCheckpoint').Count -ne 1 -or
        [regex]::Matches(
            $mainSource,
            '\$legacyPhaseCOracle = Complete-LiteralOrderedMasstestLegacyOracle').Count -ne 1 -or
        [regex]::Matches(
            $mainSource,
            '\$orderedMasstestDumpBaseline = Get-LiteralInnerDumpBaseline').Count -ne 1) {
        throw ('static check: synchronous inner -> +2 -> one state -> dump boundary -> ' +
             'ordered drive -> Phase C -> legacy oracle -> verdict -> deferred order changed')
    }
    if (-not $mainSource.Contains(
            '$orderedMergeHandoff $literalPhaseC `')) {
        throw 'static check: deferred ordered proof lost the literal Phase-C sample handoff'
    }

    $node = Get-Command node -CommandType Application -ErrorAction Stop | Select-Object -First 1
    foreach ($path in $required | Where-Object { [IO.Path]::GetExtension($_) -eq '.js' }) {
        & $node.Source --check $path
        if ($LASTEXITCODE -ne 0) {
            throw "static check: node --check failed for '$path'"
        }
    }
    Write-Host '[simturns-poc] STATIC CHECK PASS' -ForegroundColor Green
    return
}

if ($ProbeRelayFailure -and $MergeDay -ne 0) {
    throw '-ProbeRelayFailure is supported only with -MergeDay 0 (before stock ownership is restored)'
}
if ($PostMergeContinuationMode -eq 'automatic-masstest-phase-c-literal' -and
    $GameplayMode -ne 'ordered-masstest') {
    throw ('automatic-masstest-phase-c-literal belongs only to the standalone ' +
        'ordered-masstest topology; it cannot be appended to canonical gameplay')
}
if ($GameplayMode -eq 'ordered-masstest' -and
    ($PostMergeContinuationMode -ne 'automatic-masstest-phase-c-literal' -or
     $MergeDay -notin @(0, 3) -or
     $BarrierOrder -notin @('host-first', 'join-first'))) {
    throw ('ordered-masstest requires MergeDay=3 (or fixture default 0), an exact ' +
        'host-first/join-first order, and automatic-masstest-phase-c-literal')
}
if ($PostMergeContinuationMode -eq 'mss-stock-telemetry' -and $MergeDay -eq 0) {
    throw 'mss-stock-telemetry requires MergeDay'
}
if ($GameplayMode -in @('battle-block', 'canonical', 'long-move', 'long-attack') -and
    $PostMergeContinuationMode -ne 'none') {
    throw "$GameplayMode does not accept a post-merge continuation"
}
if ($BattleCase -ne 'none' -and $GameplayMode -ne 'battle-block') {
    throw '-BattleCase requires -GameplayMode battle-block'
}
if ($BattleCase -like 'audit-*') { $BattleTrace = $true }
if ($BattleTrace -and $GameplayMode -eq 'protocol') { throw 'Protocol mode has no battle fixture to trace' }
if ($BattleTrace -and $Keep) { throw 'Battle trace audit requires completed, exact-owned client logs; -Keep is unsupported' }
if ($GameplayMode -ne 'long-move' -and
    $LongMoveCase -ne 'clean-long-concurrency') {
    throw '-LongMoveCase override requires -GameplayMode long-move'
}

$battleCaseSourceEvidence = if ($BattleCase -ne 'none' -or $BattleTrace) {
    foreach ($sourcePath in @($battleCasesHelperPath, $gameplayHelperPath,
            (Join-Path $PSScriptRoot 'battle-trace-audit.js'))) {
        [pscustomobject]@{path=$sourcePath;sha256=(Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash}
    }
} else { $null }
. $helperPath
. $gameplayHelperPath
. $battleCasesHelperPath
. $ownedProcessDiagnosticsPath
. $literalInnerStartupPath

function Assert-ExactRussobitExecutable([string]$Root) {
    $exe = Join-Path $Root 'Discipl2.exe'
    $item = Get-Item -LiteralPath $exe
    if ([long]$item.Length -ne [long]$expectedRussobitExeSize) {
        throw "unsupported Discipl2.exe at '$exe': size $($item.Length), expected exact Russobit $expectedRussobitExeSize bytes"
    }
    $sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    if (-not [string]::Equals($sha256, $expectedRussobitExeSha256,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "unsupported Discipl2.exe at '$exe': SHA-256 $sha256, expected pinned Russobit $expectedRussobitExeSha256"
    }
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    $GameDir = $defaultLocalGameDir
}
$GameDir = [IO.Path]::GetFullPath((Resolve-GameDir $GameDir))
Assert-ExactRussobitExecutable $GameDir

$fixture = $null
$scenarioSelectionIndex = $Scenario
if ($GameplayMode -ne 'protocol') {
    if ($ProbeRelayFailure) {
        throw '-ProbeRelayFailure cannot replace a canonical gameplay acceptance phase'
    }
    if ([string]::IsNullOrWhiteSpace($FixtureManifest)) {
        $FixtureManifest = $defaultFixtureManifest
    }
    $fixture = Import-SimturnsFixture $FixtureManifest
    Assert-SimturnsFixtureFiles $fixture $GameDir
    [int]$fixtureMergeDay = $fixture.mergeDay
    if ($MergeDay -eq 0) {
        $MergeDay = $fixtureMergeDay
    } elseif ($MergeDay -ne $fixtureMergeDay) {
        throw "$GameplayMode fixture requires MergeDay=$fixtureMergeDay, got $MergeDay"
    }
    $fixtureScenarioPath = [IO.Path]::GetFullPath(
        (Join-Path $GameDir ([string]$fixture.map.relativePath)))
    if (-not [string]::IsNullOrWhiteSpace($ScenarioPath) -and
        -not [string]::Equals([IO.Path]::GetFullPath($ScenarioPath), $fixtureScenarioPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "ScenarioPath does not identify the pinned $GameplayMode fixture"
    }
    $ScenarioPath = $fixtureScenarioPath
    $scenarioSelectionIndex = [int]$fixture.legacySelectionIndex
}

# _relay.ps1 predates StrictMode and accesses role properties dynamically. A
# freshly-started relay legitimately has no host/join property yet, so keep the
# shared public shape while making that transient state explicit for this run.
function Get-RoleState([string]$Role) {
    $state = Get-RelayState
    return Get-OptionalProperty $state $Role
}
function Get-Dialog([string]$Role) {
    $roleState = Get-RoleState $Role
    return Get-OptionalProperty $roleState 'dialog'
}

foreach ($requiredGameFile in @('Discipl2.exe', 'mss32.dll', 'Mss23.dll')) {
    $path = Join-Path $GameDir $requiredGameFile
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "required game/runtime file is missing: $path"
    }
}

# Per-PID logs, typed UI actions and the test relay bridge exist only in the
# removable DebugTest configuration. Fail before touching machine-global
# dplaysvr if a normal Debug/Release DLL was deployed accidentally.
$deployedMss = Join-Path $GameDir 'mss32.dll'
$testdrvMarkers = @(
    'D2TESTDRV_RELAY_BRIDGE',
    'D2TESTDRV_TURN_EVENTS',
    'D2TESTDRV_EXACT_LEGACY_MOVES',
    'D2TESTDRV_CLEAN_LONG_MOVES',
    'D2TESTDRV_FIXTURE_PLAN',
    'D2TESTDRV_APPLY_FIXTURE',
    'D2TESTDRV_AUTO_BATTLE_PREARM'
)
$deployedMssText = [Text.Encoding]::ASCII.GetString(
    [IO.File]::ReadAllBytes($deployedMss))
if ($BattleTrace -and -not $deployedMssText.Contains('D2TESTDRV_BATTLE_TRACE')) {
    throw 'Battle trace requires a current DebugTest DLL with D2TESTDRV_BATTLE_TRACE'
}
if ($BattleCase -eq 'dead-leader' -and -not $deployedMssText.Contains('[testdrv][fixture-health]')) {
    throw 'Dead-leader case requires a harness with native fixture health postconditions'
}
foreach ($testdrvMarker in $testdrvMarkers) {
    if (-not $deployedMssText.Contains($testdrvMarker)) {
        throw "runtime PoC requires a DebugTest mss32.dll containing '$testdrvMarker'; " +
              'the deployed DLL is stale or appears to be Debug/Release'
    }
}

$runId = [guid]::NewGuid().ToString('N')
if ([string]::IsNullOrWhiteSpace($PipeName)) {
    $PipeName = "\\.\pipe\d2mss.simturns.poc.$runId"
}
if ($PipeName -notmatch '^\\\\\.\\pipe\\[A-Za-z0-9._-]+$') {
    throw 'PipeName must match \\.\pipe\[A-Za-z0-9._-]+'
}
$debugPipeName = "\\.\pipe\d2mss.testdrv.poc.$runId"
# The DebugTest control pipe is owned by this exact run just like the production
# pipe. TCP bridge overrides belong to other harnesses and cannot silently
# replace this endpoint.
$env:D2TESTDRV_PIPE_NAME = $debugPipeName
Remove-Item Env:D2TESTDRV_BRIDGE_TCP_HOST -ErrorAction SilentlyContinue
Remove-Item Env:D2TESTDRV_BRIDGE_TCP_PORT -ErrorAction SilentlyContinue
Remove-Item Env:D2_RELAY_TCP_HOST, Env:D2_RELAY_TCP_PORT -ErrorAction SilentlyContinue
if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
    $ArtifactDir = Join-Path ([IO.Path]::GetTempPath()) "d2mss-simturns-poc\$runId"
}
$ArtifactDir = [IO.Path]::GetFullPath($ArtifactDir)
[void](New-Item -ItemType Directory -Path $ArtifactDir -Force)

# One immutable run-owned plan feeds both clients. Record exactly the copied bytes,
# not a mutable workspace path that could change between host and join launch.
$fixturePlanSource = [IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot 'fixtures/devouring-reinforcement.ini'))
$fixturePlanSnapshot = Join-Path $ArtifactDir "fixture-plan-$runId.ini"
$fixturePlanSourceHash = (Get-FileHash -LiteralPath $fixturePlanSource -Algorithm SHA256).Hash
[IO.File]::Copy($fixturePlanSource, $fixturePlanSnapshot, $false)
$fixturePlanHash = (Get-FileHash -LiteralPath $fixturePlanSnapshot -Algorithm SHA256).Hash
if ($fixturePlanHash -cne $fixturePlanSourceHash) {
    throw 'fixture plan changed while capturing the run-owned snapshot'
}
$healthProfileEvidence = $null
if ($BattleCase -eq 'dead-leader') {
    $healthProfilePath = Join-Path $PSScriptRoot 'fixtures/devouring-dead-leader.json'
    $healthProfileSnapshot = Join-Path $ArtifactDir "health-profile-$runId.json"
    [IO.File]::Copy($healthProfilePath, $healthProfileSnapshot, $false)
    $healthProfile = Get-Content -LiteralPath $healthProfileSnapshot -Raw | ConvertFrom-Json
    $health = $healthProfile.health
    if ($healthProfile.schema -ne 1 -or
        $health.stack -ne $fixture.host.heroId -or
        $health.unit -ne $fixture.host.reinforcement.leaderId -or
        $health.expectedHp -ne 115 -or $health.newHp -ne 0 -or
        $healthProfile.movementCost -ne 6) {
        throw 'Dead-leader profile does not match the exact fixture leader precondition'
    }
    $planText = [IO.File]::ReadAllText($fixturePlanSnapshot)
    if ([regex]::Matches($planText, '(?m)^\[plan\]\r?$').Count -ne 1 -or
        $planText -match '(?m)^healthCount=|^\[health') {
        throw 'Health profile requires the unchanged base unit-transfer plan'
    }
    $planText = [regex]::Replace($planText, '(?m)^\[plan\]\r?$', "[plan]`nhealthCount=1")
    $planText += "`n[health1]`n" + ((@('stack', 'unit', 'implementation', 'expectedHp', 'newHp') |
        ForEach-Object { $_ + '=' + [string]$health.$_ }) -join "`n") + "`n"
    [IO.File]::WriteAllText($fixturePlanSnapshot, $planText, [Text.UTF8Encoding]::new($false))
    $fixturePlanHash = (Get-FileHash -LiteralPath $fixturePlanSnapshot -Algorithm SHA256).Hash
    $fixture.host.reinforcement.hp = [int]$fixture.host.reinforcement.hp -
        [int]$health.expectedHp + [int]$health.newHp
    $healthProfileEvidence = [ordered]@{
        path = $healthProfilePath
        snapshot = $healthProfileSnapshot
        sha256 = (Get-FileHash -LiteralPath $healthProfileSnapshot -Algorithm SHA256).Hash
        health = $health
        movementCost = [int]$healthProfile.movementCost
    }
}
$script:FixturePlanEvidence = [ordered]@{
    source = $fixturePlanSource
    path = $fixturePlanSnapshot
    sha256 = $fixturePlanHash
    sourceSha256 = $fixturePlanSourceHash
    healthProfile = $healthProfileEvidence
}

$script:SimRelayLog = Join-Path $ArtifactDir 'simturns-relay.jsonl'
$script:SimRelayErrorLog = Join-Path $ArtifactDir 'simturns-relay.stderr.log'
$script:SummaryPath = Join-Path $ArtifactDir 'summary.json'
$script:LiteralInnerBootstrapReleaseFile = [IO.Path]::GetFullPath(
    (Join-Path $ArtifactDir 'literal-inner-bootstrap.release'))
$script:LiteralInnerFailureLog = [IO.Path]::GetFullPath(
    (Join-Path $ArtifactDir 'literal-inner-failure.log'))
$script:LiteralInnerStartupObserver = $null
$script:ClientLogInitialLengths = @{}
$script:ClientLogOwnedProcessIds = @{}
$script:ProductionRelayProcess = $null
$script:BootDiagnostics = @()
$script:StepTranscript = [System.Collections.Generic.List[string]]::new()
$script:LegacyMassPhaseTrace = [System.Collections.Generic.List[string]]::new()
$script:StartupModalSettleMilliseconds = 300
$script:ConsumedStartupModalActions = @{}
$script:LegacyStartupWindowsComplete = $false
$script:LastStartupPopupEvidenceUtc = @{ host = $null; join = $null }
$script:LastStartupPopupEvidenceTick = @{ host = [long]0; join = [long]0 }
$script:Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'
    'DLG_BEGIN_TURN' = 'BTN_OK'
    'DLG_GETINFO_BOX' = 'BTN_CLOSE'
    'DLG_MESSAGE_BOX' = 'BTN_OK'
    'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
    'DLG_ITEM' = 'BTN_OK'
}
$script:BareMapDialogs = @('DLG_STRATEGIC', 'DLG_ISO_PAL')

function Add-LegacyMassPhase([string]$Phase) {
    if ($GameplayMode -ne 'canonical') { return }
    if (-not $script:LegacyMassPhaseTrace.Contains($Phase)) {
        $script:LegacyMassPhaseTrace.Add($Phase)
    }
}

function Write-Step([string]$Message) {
    $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $Message
    $script:StepTranscript.Add($line)
    if ($GameplayMode -eq 'canonical') {
        switch -Regex ($Message) {
            '^legacy startup PASS:' {
                Add-LegacyMassPhase 'boot'
                break
            }
            '^canonical attacks: submitting one host and one join move' {
                Add-LegacyMassPhase 'attack-fire'
                break
            }
            '^(host|join) canonical battle STARTED ' {
                Add-LegacyMassPhase 'battle-start'
                break
            }
            '^(host|join) canonical battle CLOSED ' {
                Add-LegacyMassPhase 'battle-close'
                break
            }
            '^postBattleWalk legacy PASS:' {
                Add-LegacyMassPhase 'walk1'
                break
            }
            '^round [0-9]+: parallel subjective End Turn ' {
                Add-LegacyMassPhase 'endturn-fire'
                break
            }
            '^round [0-9]+ PASS: simultaneous End Turn ' {
                Add-LegacyMassPhase 'endturn-done'
                break
            }
            '^day2Reverse legacy PASS:' {
                Add-LegacyMassPhase 'walk2'
                break
            }
            '^merge barrier: .* End Turn arrival at day ' {
                Add-LegacyMassPhase 'barrier-fire'
                break
            }
            '^BARRIER RESULT: PASS' {
                Add-LegacyMassPhase 'barrier-done'
                break
            }
        }
    }
    Write-Host $line
}

function Save-OwnedBootDiagnostic([string]$Role,
                                  [System.Diagnostics.Process]$OwnedProcess,
                                  [string]$Reason) {
    $path = Join-Path $ArtifactDir "boot-$Role-owned-process.json"
    $diagnostic = Get-OwnedProcessDiagnostic `
        -Process $OwnedProcess -Role $Role -Reason $Reason
    $diagnostic | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $path -Encoding utf8
    $script:BootDiagnostics += [pscustomobject]@{
        role = $Role
        pid = $OwnedProcess.Id
        reason = $Reason
        path = [IO.Path]::GetFullPath($path)
        hasExited = $diagnostic.hasExited
        exitCode = $diagnostic.exitCode
    }
    Write-Step "$Role boot diagnostic captured for exact-owned pid=$($OwnedProcess.Id): $path"
    return $diagnostic
}

function Read-SimRelayEvents {
    if (-not (Test-Path -LiteralPath $script:SimRelayLog -PathType Leaf)) { return @() }
    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $script:SimRelayLog -ErrorAction SilentlyContinue) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $events.Add(($line | ConvertFrom-Json -ErrorAction Stop)) }
        catch { } # A partial final line becomes visible after the writer completes it.
    }
    return @($events)
}

function Get-OptionalProperty([object]$Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-DialogObservation([string]$Role) {
    $state = Get-RoleState $Role
    if (-not $state) { return $null }
    $dialog = Get-OptionalProperty $state 'dialog'
    $instanceValue = Get-OptionalProperty $state 'dialogInstance'
    $appearanceValue = Get-OptionalProperty $state 'dialogAppearance'
    $readyValue = Get-OptionalProperty $state 'dialogReady'
    if ($null -eq $instanceValue -or $null -eq $appearanceValue -or $null -eq $readyValue) {
        throw "$Role relay state omitted native dialog identity/readiness"
    }
    [long]$instance = $instanceValue
    [long]$appearance = $appearanceValue
    if ($instance -lt 1 -or $appearance -ne $instance) {
        throw "$Role relay dialog identity is inconsistent (instance=$instance appearance=$appearance)"
    }
    return [pscustomobject]@{
        State = $state
        Dialog = [string]$dialog
        Instance = $instance
        Ready = [bool]$readyValue
    }
}

function Test-LiteralNavigationPassiveTimeoutException(
    [System.Exception]$Exception) {
    $current = $Exception
    while ($current) {
        if ($current -is [System.TimeoutException] -or
            $current -is [System.OperationCanceledException]) {
            return $true
        }
        $current = $current.InnerException
    }
    return $false
}

# Physical navigation owns a complete outer deadline. A temporarily starved
# passive GET is therefore an absent observation, not an action failure. HTTP
# status, connection, JSON/schema and terminal-relay failures stay fatal. The
# action helpers below never call this function after publishing a POST.
function Get-LiteralNavigationRoleState {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][string]$Context,
        [Parameter(Mandatory)][DateTime]$DeadlineUtc
    )
    $startedUtc = [DateTime]::UtcNow
    if ($startedUtc -ge $DeadlineUtc) { return $null }
    $remainingMilliseconds = [Math]::Floor(
        ($DeadlineUtc - $startedUtc).TotalMilliseconds)
    $requestTimeoutMilliseconds = [Math]::Min(
        10000, [Math]::Max(1, $remainingMilliseconds))
    $uri = "$script:RelayBase/api/state"
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.UseProxy = $false
    $handler.AllowAutoRedirect = $false
    $client = [System.Net.Http.HttpClient]::new($handler, $true)
    $client.Timeout = [TimeSpan]::FromMilliseconds($requestTimeoutMilliseconds)
    $httpResponse = $null
    try {
        try {
            $httpResponse = $client.GetAsync($uri).GetAwaiter().GetResult()
            $body = $httpResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            if (-not $httpResponse.IsSuccessStatusCode) {
                throw ("HTTP {0} ({1}) from passive navigation GET {2}: {3}" -f
                    [int]$httpResponse.StatusCode, $httpResponse.ReasonPhrase,
                    $uri, $body)
            }
            $response = $body | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $completedUtc = [DateTime]::UtcNow
            if (-not (Test-LiteralNavigationPassiveTimeoutException `
                    $_.Exception)) {
                throw
            }
            $elapsedMilliseconds = [Math]::Round(
                ($completedUtc - $startedUtc).TotalMilliseconds, 1)
            Write-Warning (('literal navigation passive GET timed out: ' +
                'role={0}; context={1}; uri={2}; startedUtc={3:O}; ' +
                'completedUtc={4:O}; elapsedMs={5}; requestTimeoutMs={6}; ' +
                'laneDeadlineUtc={7:O}; exception={8}') -f
                $Role, $Context, $uri, $startedUtc, $completedUtc,
                $elapsedMilliseconds, $requestTimeoutMilliseconds,
                $DeadlineUtc, $_.Exception.GetType().FullName)
            return $null
        }

        return Get-LiteralNavigationRoleStateProjection $Role $response
    } finally {
        if ($httpResponse) { $httpResponse.Dispose() }
        $client.Dispose()
    }
}

function Get-LiteralNavigationRoleStateProjection {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [AllowNull()][object]$Response
    )
    if ($Response -isnot [System.Management.Automation.PSCustomObject]) {
        throw 'passive navigation /api/state root must be a JSON object'
    }
    $terminalFaultProperty = $Response.PSObject.Properties['terminalFault']
    if ($null -eq $terminalFaultProperty) {
        throw 'passive navigation /api/state omitted terminalFault'
    }
    if ($null -ne $terminalFaultProperty.Value) {
        $reason = Get-OptionalProperty $terminalFaultProperty.Value 'reason'
        throw "test relay terminal fault: $([string]$reason)"
    }
    $rolesProperty = $Response.PSObject.Properties['roles']
    if ($null -eq $rolesProperty -or
        $rolesProperty.Value -isnot
            [System.Management.Automation.PSCustomObject]) {
        throw 'passive navigation /api/state omitted its roles object'
    }
    $roleProperty = $rolesProperty.Value.PSObject.Properties[$Role]
    if ($null -eq $roleProperty) { return $null }
    if ($roleProperty.Value -isnot
        [System.Management.Automation.PSCustomObject]) {
        throw "passive navigation /api/state published a non-object $Role role"
    }
    return $roleProperty.Value
}

function ConvertTo-LiteralNavigationUInt32 {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Description
    )
    if ($null -eq $Value -or $Value -is [bool] -or
        $Value -is [string] -or $Value -isnot [ValueType]) {
        throw "$Description must be an integer uint32"
    }
    try { [decimal]$number = $Value }
    catch { throw "$Description must be an integer uint32" }
    if ($number -ne [decimal]::Truncate($number) -or
        $number -lt 1 -or $number -gt [uint32]::MaxValue) {
        throw "$Description must be an integer uint32"
    }
    return [long]$number
}

function ConvertTo-LiteralNavigationDialogObservation(
    [string]$Role,
    [object]$State) {
    if ($null -eq $State) { return $null }
    if ($State -isnot [System.Management.Automation.PSCustomObject]) {
        throw "$Role passive navigation state must be a JSON object"
    }
    $dialogProperty = $State.PSObject.Properties['dialog']
    $instanceProperty = $State.PSObject.Properties['dialogInstance']
    $appearanceProperty = $State.PSObject.Properties['dialogAppearance']
    $readyValue = Get-OptionalProperty $State 'dialogReady'
    if ($null -eq $dialogProperty -or
        $dialogProperty.Value -isnot [string] -or
        [string]::IsNullOrWhiteSpace([string]$dialogProperty.Value) -or
        $null -eq $instanceProperty -or $null -eq $appearanceProperty -or
        $readyValue -isnot [bool]) {
        throw "$Role passive navigation state omitted typed dialog identity/readiness"
    }
    [long]$instance = ConvertTo-LiteralNavigationUInt32 `
        $instanceProperty.Value "$Role passive navigation dialogInstance"
    [long]$appearance = ConvertTo-LiteralNavigationUInt32 `
        $appearanceProperty.Value "$Role passive navigation dialogAppearance"
    if ($appearance -ne $instance) {
        throw "$Role passive navigation dialog identity is inconsistent (instance=$instance appearance=$appearance)"
    }
    return [pscustomobject]@{
        State = $State
        Dialog = [string]$dialogProperty.Value
        Instance = $instance
        Ready = [bool]$readyValue
    }
}

function Get-LiteralNavigationDialogObservation {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][string]$Context,
        [Parameter(Mandatory)][DateTime]$DeadlineUtc
    )
    $state = Get-LiteralNavigationRoleState `
        -Role $Role -Context $Context -DeadlineUtc $DeadlineUtc
    if (-not $state) { return $null }
    return ConvertTo-LiteralNavigationDialogObservation $Role $state
}

# The bootstrap cursor pump uses /api/ui for command-boundary reads. Keep this
# separate from Get-DialogObservation: older navigation code intentionally reads
# sticky role state such as reachedStrategic from /api/state.
function Get-FreshDialogObservation([string]$Role) {
    $state = Get-GameUi $Role
    if (-not $state) { return $null }
    $dialog = Get-OptionalProperty $state 'dialog'
    $instanceValue = Get-OptionalProperty $state 'dialogInstance'
    $appearanceValue = Get-OptionalProperty $state 'dialogAppearance'
    $readyValue = Get-OptionalProperty $state 'dialogReady'
    $sequenceValue = Get-OptionalProperty $state 'uiSeq'
    if ($null -eq $instanceValue -or $null -eq $appearanceValue -or
        $readyValue -isnot [bool] -or $null -eq $sequenceValue) {
        throw "$Role current UI omitted typed identity/readiness/sequence"
    }
    try {
        [long]$instance = $instanceValue
        [long]$appearance = $appearanceValue
        [long]$sequence = $sequenceValue
    } catch {
        throw "$Role current UI has a non-integer identity or sequence"
    }
    if ($instance -lt 1 -or $instance -gt [uint32]::MaxValue -or
        $appearance -ne $instance -or $sequence -lt 1 -or
        $sequence -gt [uint32]::MaxValue) {
        throw "$Role current UI identity/sequence is inconsistent ($instance/$appearance seq=$sequence)"
    }
    return [pscustomobject]@{
        State = $state
        Dialog = [string]$dialog
        Instance = $instance
        Ready = [bool]$readyValue
        Sequence = $sequence
    }
}

function Get-ReadyActionTarget([object]$Observation,
                               [string]$Dialog,
                               [long]$ExpectedInstance = 0) {
    if (-not $Observation -or -not $Observation.Ready) {
        throw "action reached a non-ready native dialog while resolving '$Dialog'"
    }
    $targetsProperty = $Observation.State.PSObject.Properties['targets']
    if ($null -eq $targetsProperty) {
        throw "$($Observation.Dialog) ready snapshot omitted native action targets"
    }
    $matches = @($targetsProperty.Value | Where-Object {
        (Get-OptionalProperty $_ 'dialog') -eq $Dialog -and
        ($ExpectedInstance -le 0 -or [long](Get-OptionalProperty $_ 'instance') -eq $ExpectedInstance)
    })
    if ($matches.Count -ne 1) {
        throw "$($Observation.Dialog) ready snapshot contains $($matches.Count) '$Dialog' owners"
    }
    [long]$instance = Get-OptionalProperty $matches[0] 'instance'
    if ($instance -lt 1 -or $instance -gt [uint32]::MaxValue) {
        throw "$Dialog owner token is outside uint32: $instance"
    }
    return $matches[0]
}

function Assert-ReadyButtonSnapshot([object]$Observation,
                                    [string]$Button,
                                    [string]$Dialog = '') {
    if (-not $Dialog) { $Dialog = $Observation.Dialog }
    $target = Get-ReadyActionTarget $Observation $Dialog
    $widgets = Get-OptionalProperty $target 'widgets'
    if ($null -eq $widgets) { throw "$Dialog ready owner omitted widgets" }
    $matches = @($widgets | Where-Object {
        (Get-OptionalProperty $_ 'name') -eq $Button -and
        (Get-OptionalProperty $_ 'type') -eq 'button'
    })
    if ($matches.Count -ne 1) {
        throw "$Dialog ready owner contains $($matches.Count) '$Button' buttons"
    }
    $buttonState = Get-OptionalProperty $matches[0] 'state'
    $enabledProperty = if ($buttonState) { $buttonState.PSObject.Properties['enabled'] } else { $null }
    if ($null -eq $enabledProperty -or $enabledProperty.Value -ne $true) {
        throw "$Dialog::$Button is not explicitly enabled on its ready native owner"
    }
    return [long](Get-OptionalProperty $target 'instance')
}

# Read one append-only, role-filtered UI batch without advancing its caller's
# cursor. The caller applies every observation first and only then commits the
# returned global watermark to that role's cursor.
function Get-ValidatedUiHistoryBatch([string]$Role, [long]$After) {
    if ($Role -notin @('host', 'join')) { throw "invalid UI history role '$Role'" }
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "$Role UI cursor is outside uint32: $After"
    }

    $history = Get-UiHistory -Role $Role -After $After
    $latestValue = Get-OptionalProperty $history 'latestSeq'
    if ($null -eq $latestValue) { throw "$Role UI history omitted latestSeq" }
    try { [long]$latestSequence = $latestValue }
    catch { throw "$Role UI history latestSeq is not an integer: '$latestValue'" }
    if ($latestSequence -lt $After -or $latestSequence -gt [uint32]::MaxValue) {
        throw "$Role UI history watermark regressed or left uint32 ($After -> $latestSequence)"
    }

    $eventsProperty = $history.PSObject.Properties['events']
    if ($null -eq $eventsProperty) { throw "$Role UI history omitted events" }
    $observations = [System.Collections.Generic.List[object]]::new()
    [long]$previousSequence = $After
    foreach ($event in @($eventsProperty.Value)) {
        $sequenceValue = Get-OptionalProperty $event 'seq'
        if ($null -eq $sequenceValue) { throw "$Role UI history event omitted seq" }
        try { [long]$sequence = $sequenceValue }
        catch { throw "$Role UI history event seq is not an integer: '$sequenceValue'" }
        if ($sequence -le $previousSequence -or $sequence -gt $latestSequence -or
            $sequence -gt [uint32]::MaxValue) {
            throw "$Role UI history event sequence is not strictly monotonic ($previousSequence -> $sequence, latest=$latestSequence)"
        }
        $eventRole = [string](Get-OptionalProperty $event 'role')
        if ($eventRole -ne $Role) {
            throw "$Role UI history contained an event for '$eventRole'"
        }
        $dialog = [string](Get-OptionalProperty $event 'dialog')
        $instanceValue = Get-OptionalProperty $event 'dialogInstance'
        $appearanceValue = Get-OptionalProperty $event 'dialogAppearance'
        $readyValue = Get-OptionalProperty $event 'dialogReady'
        if ([string]::IsNullOrWhiteSpace($dialog) -or
            $null -eq $instanceValue -or $null -eq $appearanceValue -or
            $readyValue -isnot [bool]) {
            throw "$Role UI history event $sequence omitted typed dialog identity/readiness"
        }
        try {
            [long]$instance = $instanceValue
            [long]$appearance = $appearanceValue
        } catch {
            throw "$Role UI history event $sequence has a non-integer dialog identity"
        }
        if ($instance -lt 1 -or $instance -gt [uint32]::MaxValue -or
            $appearance -ne $instance) {
            throw "$Role UI history event $sequence has inconsistent dialog identity $instance/$appearance"
        }
        $observations.Add([pscustomobject]@{
            State = $event
            Dialog = $dialog
            Instance = $instance
            Ready = [bool]$readyValue
            Sequence = $sequence
        })
        $previousSequence = $sequence
    }

    return [pscustomobject]@{
        LatestSequence = $latestSequence
        Observations = @($observations)
    }
}

function Get-ReadyListBoxState([object]$Observation,
                               [string]$Dialog,
                               [string]$ListBox,
                               [long]$ExpectedInstance = 0) {
    $target = Get-ReadyActionTarget $Observation $Dialog $ExpectedInstance
    $widgets = Get-OptionalProperty $target 'widgets'
    if ($null -eq $widgets) { throw "$Dialog ready owner omitted widgets" }
    $matches = @($widgets | Where-Object {
        (Get-OptionalProperty $_ 'name') -eq $ListBox -and
        (Get-OptionalProperty $_ 'type') -eq 'listbox'
    })
    if ($matches.Count -ne 1) {
        throw "$Dialog ready owner contains $($matches.Count) '$ListBox' listboxes"
    }
    $state = Get-OptionalProperty $matches[0] 'state'
    $selectedProperty = if ($state) { $state.PSObject.Properties['selected'] } else { $null }
    $totalProperty = if ($state) { $state.PSObject.Properties['total'] } else { $null }
    if ($null -eq $selectedProperty -or $null -eq $totalProperty) {
        throw "$Dialog::$ListBox omitted selected/total state"
    }
    return [pscustomobject]@{
        Selected = [int]$selectedProperty.Value
        Total = [int]$totalProperty.Value
        Owner = [long](Get-OptionalProperty $target 'instance')
    }
}

function Get-SimEventCount([object[]]$Events, [string]$Event, [string]$Role = '') {
    @($Events | Where-Object {
        (Get-OptionalProperty $_ 'event') -eq $Event -and
        ([string]::IsNullOrEmpty($Role) -or (Get-OptionalProperty $_ 'role') -eq $Role)
    }).Count
}

function Assert-SimEventDelta([object[]]$Events,
                              [string]$Event,
                              [int]$Before,
                              [int]$ExpectedDelta,
                              [string]$Role = '') {
    $actual = Get-SimEventCount $Events $Event $Role
    $expected = $Before + $ExpectedDelta
    if ($actual -ne $expected) {
        $scope = if ([string]::IsNullOrEmpty($Role)) { $Event } else { "$Event/$Role" }
        throw "relay event '$scope' count is $actual, expected exactly $expected"
    }
}

function Test-SimEventDeltaReached([object[]]$Events,
                                   [string]$Event,
                                   [int]$Before,
                                   [int]$ExpectedDelta,
                                   [string]$Role = '') {
    $actual = Get-SimEventCount $Events $Event $Role
    $expected = $Before + $ExpectedDelta
    if ($actual -lt $Before -or $actual -gt $expected) {
        $scope = if ([string]::IsNullOrEmpty($Role)) { $Event } else { "$Event/$Role" }
        throw "relay event '$scope' count is $actual while waiting for exact count $expected"
    }
    return $actual -eq $expected
}

function Get-SimEventMatches([object[]]$Events,
                             [string]$Event,
                             [string]$Role = '',
                             [object]$Day = $null) {
    return @($Events | Where-Object {
        (Get-OptionalProperty $_ 'event') -eq $Event -and
        ([string]::IsNullOrEmpty($Role) -or (Get-OptionalProperty $_ 'role') -eq $Role) -and
        ($null -eq $Day -or (Get-OptionalProperty $_ 'day') -eq $Day)
    })
}

function Get-ExactSimEvent([object[]]$Events,
                           [string]$Event,
                           [string]$Role = '',
                           [object]$Day = $null) {
    $matches = @(Get-SimEventMatches $Events $Event $Role $Day)
    if ($matches.Count -ne 1) {
        $scope = @($Event, $Role, $(if ($null -eq $Day) { '' } else { "day=$Day" })) |
            Where-Object { -not [string]::IsNullOrEmpty($_) }
        throw "relay event '$($scope -join '/')' count is $($matches.Count), expected exactly 1"
    }
    return $matches[0]
}

function Get-SimEventRecordIndex([object[]]$Events, [object]$Record) {
    for ($index = 0; $index -lt $Events.Count; $index++) {
        if ([object]::ReferenceEquals($Events[$index], $Record)) { return $index }
    }
    return -1
}

function Get-NewExactSimEvent([object[]]$Events,
                              [string]$Event,
                              [string]$Role,
                              [int]$Before) {
    $matches = @(Get-SimEventMatches $Events $Event $Role)
    $expected = $Before + 1
    if ($matches.Count -ne $expected) {
        throw "relay event '$Event/$Role' count is $($matches.Count), expected exactly $expected"
    }
    return $matches[$Before]
}

function Get-SessionPlanEvent([object[]]$Events) {
    $plan = Get-ExactSimEvent $Events 'session-plan-created'
    foreach ($field in @(
        'epoch', 'mergeDay', 'hostHandle', 'joinHandle', 'hostLease', 'joinLease'
    )) {
        if ($null -eq (Get-OptionalProperty $plan $field)) {
            throw "session plan omitted authoritative field '$field'"
        }
    }
    if ([long](Get-OptionalProperty $plan 'epoch') -le 0 -or
        [long](Get-OptionalProperty $plan 'hostLease') -le 0 -or
        [long](Get-OptionalProperty $plan 'joinLease') -le 0 -or
        [long](Get-OptionalProperty $plan 'hostLease') -eq
            [long](Get-OptionalProperty $plan 'joinLease')) {
        throw 'session plan contains invalid or aliased initial leases'
    }
    return $plan
}

function Get-SessionRoleHandle([object[]]$Events,
                               [ValidateSet('host', 'join')][string]$Role) {
    $plan = Get-SessionPlanEvent $Events
    return [string](Get-OptionalProperty $plan "${Role}Handle")
}

function Get-ExpectedTurnLease([object[]]$Events,
                               [ValidateSet('host', 'join')][string]$Role,
                               [int]$CompletedDay) {
    if ($CompletedDay -lt 1) {
        throw "cannot resolve a turn lease for non-positive day $CompletedDay"
    }
    if ($CompletedDay -eq 1) {
        $plan = Get-SessionPlanEvent $Events
        return [long](Get-OptionalProperty $plan "${Role}Lease")
    }
    $turn = Get-ExactSimEvent $Events 'turn-start-complete' $Role $CompletedDay
    [long]$lease = Get-RequiredTelemetryNumber `
        $turn 'lease' "$Role day-$CompletedDay completed turn lease"
    if ($lease -le 0) {
        throw "$Role day-$CompletedDay completed turn lease must be nonzero"
    }
    return $lease
}

function Get-EngineActionMatches([object[]]$Events,
                                 [string]$Stage = '',
                                 [string]$Recipient = '',
                                 [object]$PlayerHandle = $null,
                                 [object]$Day = $null) {
    return @($Events | Where-Object {
        (Get-OptionalProperty $_ 'event') -eq 'engine-action-dispatched' -and
        ([string]::IsNullOrEmpty($Stage) -or
            (Get-OptionalProperty $_ 'stage') -eq $Stage) -and
        ([string]::IsNullOrEmpty($Recipient) -or
            (Get-OptionalProperty $_ 'recipient') -eq $Recipient) -and
        ($null -eq $PlayerHandle -or
            [string](Get-OptionalProperty $_ 'playerHandle') -eq
                [string]$PlayerHandle) -and
        ($null -eq $Day -or (Get-OptionalProperty $_ 'day') -eq $Day)
    })
}

function Get-EngineActionCount([object[]]$Events,
                               [string]$Stage = '',
                               [string]$Recipient = '',
                               [object]$PlayerHandle = $null,
                               [object]$Day = $null) {
    return @(Get-EngineActionMatches `
        $Events $Stage $Recipient $PlayerHandle $Day).Count
}

function Assert-EngineActionDelta([object[]]$Events,
                                  [string]$Stage,
                                  [string]$Recipient,
                                  [object]$PlayerHandle,
                                  [int]$Before,
                                  [int]$ExpectedDelta,
                                  [object]$Day = $null) {
    [int]$actual = Get-EngineActionCount `
        $Events $Stage $Recipient $PlayerHandle $Day
    [int]$expected = $Before + $ExpectedDelta
    if ($actual -ne $expected) {
        throw ("relay engine action '$Stage/$Recipient/$PlayerHandle' count is " +
            "$actual, expected exactly $expected")
    }
}

function Test-EngineActionDeltaReached([object[]]$Events,
                                       [string]$Stage,
                                       [string]$Recipient,
                                       [object]$PlayerHandle,
                                       [int]$Before,
                                       [int]$ExpectedDelta,
                                       [object]$Day = $null) {
    [int]$actual = Get-EngineActionCount `
        $Events $Stage $Recipient $PlayerHandle $Day
    [int]$expected = $Before + $ExpectedDelta
    if ($actual -lt $Before -or $actual -gt $expected) {
        throw ("relay engine action '$Stage/$Recipient/$PlayerHandle' count is " +
            "$actual while waiting for exact count $expected")
    }
    return $actual -eq $expected
}

function Get-NewExactEngineAction([object[]]$Events,
                                  [string]$Stage,
                                  [string]$Recipient,
                                  [object]$PlayerHandle,
                                  [int]$Before,
                                  [object]$Day = $null) {
    $matches = @(Get-EngineActionMatches `
        $Events $Stage $Recipient $PlayerHandle $Day)
    $expected = $Before + 1
    if ($matches.Count -ne $expected) {
        throw ("relay engine action '$Stage/$Recipient/$PlayerHandle' count is " +
            "$($matches.Count), expected exactly $expected")
    }
    return $matches[$Before]
}

function Assert-ExactEndTurnTransaction([object[]]$Events,
                                    [string]$Role,
                                    [int]$ObservedBefore,
                                    [int]$AppliedBefore,
                                    [int]$AcceptedBefore,
                                    [int]$ExpectedCompletedDay) {
    $observed = Get-NewExactSimEvent `
        $Events 'end-turn-observed' $Role $ObservedBefore
    $applied = Get-NewExactSimEvent $Events 'end-turn-applied' $Role $AppliedBefore
    $accepted = Get-NewExactSimEvent $Events 'end-turn-accepted' $Role $AcceptedBefore

    foreach ($field in @('lease', 'completedDay')) {
        $observedValue = Get-OptionalProperty $observed $field
        $appliedValue = Get-OptionalProperty $applied $field
        $acceptedValue = Get-OptionalProperty $accepted $field
        if ($null -eq $observedValue -or
            $observedValue -ne $appliedValue -or
            $observedValue -ne $acceptedValue) {
            throw "end-turn $field mismatch for $Role across Observed/Applied/Accepted"
        }
    }
    [long]$expectedLease = Get-ExpectedTurnLease `
        $Events $Role $ExpectedCompletedDay
    if ([long](Get-OptionalProperty $accepted 'lease') -ne $expectedLease -or
        [int](Get-OptionalProperty $accepted 'completedDay') -ne
            $ExpectedCompletedDay) {
        throw "end-turn causal triple for $Role does not match its authoritative lease/day"
    }

    $observedIndex = Get-SimEventRecordIndex $Events $observed
    $appliedIndex = Get-SimEventRecordIndex $Events $applied
    $acceptedIndex = Get-SimEventRecordIndex $Events $accepted
    if ($observedIndex -lt 0 -or $appliedIndex -lt 0 -or
        $acceptedIndex -lt 0 -or $acceptedIndex -le $observedIndex -or
        $acceptedIndex -le $appliedIndex) {
        throw "end-turn Accepted for $Role was not logged strictly after both exact causal signals"
    }

    return [pscustomobject]@{
        lease = Get-OptionalProperty $accepted 'lease'
        completedDay = Get-OptionalProperty $accepted 'completedDay'
        observedIndex = $observedIndex
        appliedIndex = $appliedIndex
        acceptedIndex = $acceptedIndex
    }
}

function Assert-BarrierHeldEvidence([object[]]$Events,
                                    [object]$Barrier,
                                    [ValidateSet('host', 'join')][string]$Role,
                                    [int]$ExpectedDay) {
    if ([string](Get-OptionalProperty $Barrier 'role') -ne $Role -or
        [int](Get-OptionalProperty $Barrier 'day') -ne $ExpectedDay) {
        throw "barrier-held evidence does not identify $Role/day-$ExpectedDay"
    }
    [long]$barrierActionId = Get-RequiredTelemetryNumber `
        $Barrier 'actionId' "barrier-held/$Role"
    [string]$handle = Get-SessionRoleHandle $Events $Role
    $holds = @(Get-EngineActionMatches `
        $Events 'hold-input' $Role $handle $ExpectedDay)
    if ($holds.Count -ne 1) {
        throw "HoldInput action count for $Role/day-$ExpectedDay is $($holds.Count), expected 1"
    }
    $hold = $holds[0]
    if ([long](Get-OptionalProperty $hold 'actionId') -ne $barrierActionId -or
        [int](Get-OptionalProperty $hold 'kind') -ne 3 -or
        [long](Get-OptionalProperty $hold 'lease') -ne 0) {
        throw "barrier-held/$Role does not acknowledge its exact HoldInput action"
    }
    [int]$holdIndex = Get-SimEventRecordIndex $Events $hold
    [int]$barrierIndex = Get-SimEventRecordIndex $Events $Barrier
    if ($holdIndex -lt 0 -or $barrierIndex -le $holdIndex) {
        throw "barrier-held/$Role did not follow its exact HoldInput dispatch"
    }
    return [pscustomobject]@{
        role = $Role
        handle = $handle
        day = $ExpectedDay
        actionId = $barrierActionId
        dispatchIndex = $holdIndex
        heldIndex = $barrierIndex
    }
}

function Assert-ExactMergeTransaction([object[]]$Events,
                                      [int]$ExpectedMergeDay) {
    $barriers = @(Get-SimEventMatches $Events 'barrier-held')
    if ($barriers.Count -ne 2) {
        throw "merge transaction has $($barriers.Count) barrier-held records, expected 2"
    }
    $barrierProofs = @()
    foreach ($role in @('host', 'join')) {
        $roleBarriers = @($barriers | Where-Object {
            [string](Get-OptionalProperty $_ 'role') -eq $role
        })
        if ($roleBarriers.Count -ne 1) {
            throw "merge transaction has $($roleBarriers.Count) barrier-held/$role records"
        }
        $barrierProofs += Assert-BarrierHeldEvidence `
            $Events $roleBarriers[0] $role ($ExpectedMergeDay - 1)
    }

    $prepareDispatched = Get-ExactSimEvent $Events 'merge-prepare-dispatched'
    $hostPrepared = Get-ExactSimEvent $Events 'merge-prepare-applied' 'host'
    $joinPrepared = Get-ExactSimEvent $Events 'merge-prepare-applied' 'join'
    $executeDispatched = Get-ExactSimEvent $Events 'merge-execute-dispatched'
    $executeApplied = Get-ExactSimEvent $Events 'merge-execute-applied'
    $hostMerged = Get-ExactSimEvent $Events 'merge-applied' 'host'
    $joinMerged = Get-ExactSimEvent $Events 'merge-applied' 'join'
    $released = Get-ExactSimEvent $Events 'merge-released'
    [long]$actionId = Get-RequiredTelemetryNumber `
        $prepareDispatched 'actionId' 'merge transaction'
    foreach ($record in @(
        $hostPrepared,
        $joinPrepared,
        $executeDispatched,
        $executeApplied,
        $hostMerged,
        $joinMerged,
        $released
    )) {
        if ([long](Get-OptionalProperty $record 'actionId') -ne $actionId) {
            throw 'merge transaction changed its authoritative actionId'
        }
    }
    foreach ($record in @($prepareDispatched, $executeDispatched, $released)) {
        if ([int](Get-OptionalProperty $record 'mergeDay') -ne $ExpectedMergeDay) {
            throw "merge transaction does not identify merge day $ExpectedMergeDay"
        }
    }

    $indices = [ordered]@{
        prepareDispatched = Get-SimEventRecordIndex $Events $prepareDispatched
        hostPrepared = Get-SimEventRecordIndex $Events $hostPrepared
        joinPrepared = Get-SimEventRecordIndex $Events $joinPrepared
        executeDispatched = Get-SimEventRecordIndex $Events $executeDispatched
        executeApplied = Get-SimEventRecordIndex $Events $executeApplied
        hostMerged = Get-SimEventRecordIndex $Events $hostMerged
        joinMerged = Get-SimEventRecordIndex $Events $joinMerged
        released = Get-SimEventRecordIndex $Events $released
    }
    [int]$lastBarrierIndex = @($barrierProofs | ForEach-Object {
        [int]$_.heldIndex
    } | Measure-Object -Maximum).Maximum
    if ($indices.prepareDispatched -le $lastBarrierIndex -or
        $indices.hostPrepared -le $indices.prepareDispatched -or
        $indices.joinPrepared -le $indices.prepareDispatched -or
        $indices.executeDispatched -le $indices.hostPrepared -or
        $indices.executeDispatched -le $indices.joinPrepared -or
        $indices.executeApplied -le $indices.executeDispatched -or
        $indices.hostMerged -le $indices.executeDispatched -or
        $indices.joinMerged -le $indices.executeDispatched -or
        $indices.released -le $indices.executeApplied -or
        $indices.released -le $indices.hostMerged -or
        $indices.released -le $indices.joinMerged) {
        throw 'merge transaction lost its exact prepare/execute/apply/release causal order'
    }
    return [pscustomobject]@{
        actionId = $actionId
        mergeDay = $ExpectedMergeDay
        barrierHeldPeers = @($barrierProofs | ForEach-Object role | Sort-Object)
        barriers = @($barrierProofs)
        indices = [pscustomobject]$indices
    }
}

function Assert-ExactBootstrapOperational([object[]]$Events) {
    [void](Get-ExactSimEvent $Events 'hello-accepted' 'host')
    [void](Get-ExactSimEvent $Events 'hello-accepted' 'join')
    [void](Get-ExactSimEvent $Events 'handle-ready' 'host')
    [void](Get-ExactSimEvent $Events 'handle-ready' 'join')
    $hostPlanDelivered = Get-ExactSimEvent `
        $Events 'session-plan-delivered' 'host'
    $sessionPlan = Get-SessionPlanEvent $Events
    $hostActivated = Get-ExactSimEvent $Events 'session-activated' 'host'
    $joinPlanDelivered = Get-ExactSimEvent `
        $Events 'session-plan-delivered' 'join'
    $joinActivated = Get-ExactSimEvent $Events 'session-activated' 'join'
    $beginApplied = Get-ExactSimEvent $Events 'bootstrap-begin-turn-applied' 'join' 1
    $delayArmed = Get-ExactSimEvent $Events 'bootstrap-cascade-delay-armed'
    $joinHandle = Get-OptionalProperty $sessionPlan 'joinHandle'
    $bootstrapActions = @(Get-EngineActionMatches `
        $Events 'bootstrap-apply' 'host' $joinHandle 1)
    if ($bootstrapActions.Count -ne 1) {
        throw "relay bootstrap apply action count is $($bootstrapActions.Count), expected exactly 1"
    }
    $cascade = $bootstrapActions[0]
    $cascadeComplete = Get-ExactSimEvent $Events 'bootstrap-cascade-complete' 'join' 1
    $turnInfoApplied = Get-ExactSimEvent $Events 'bootstrap-turn-info-applied' 'join' 1
    $commitDispatched = Get-ExactSimEvent $Events 'bootstrap-commit-dispatched' '' 1
    $hostCommitApplied = Get-ExactSimEvent $Events 'bootstrap-commit-applied' 'host'
    $joinCommitApplied = Get-ExactSimEvent $Events 'bootstrap-commit-applied' 'join'
    $operationalDispatched = Get-ExactSimEvent $Events 'bootstrap-operational-dispatched' '' 1
    $hostOperationalApplied = Get-ExactSimEvent `
        $Events 'bootstrap-operational-applied' 'host'
    $joinOperationalApplied = Get-ExactSimEvent `
        $Events 'bootstrap-operational-applied' 'join'
    $bootstrapReleased = Get-ExactSimEvent $Events 'bootstrap-released' '' 1
    $operational = Get-ExactSimEvent $Events 'session-operational'

    [long]$planEpoch = Get-RequiredTelemetryNumber `
        $sessionPlan 'epoch' 'authoritative SessionPlan epoch'
    [long]$hostDeliveredEpoch = Get-RequiredTelemetryNumber `
        $hostPlanDelivered 'epoch' 'host SessionPlan delivery epoch'
    [long]$joinDeliveredEpoch = Get-RequiredTelemetryNumber `
        $joinPlanDelivered 'epoch' 'join SessionPlan delivery epoch'
    [long]$operationalEpoch = Get-RequiredTelemetryNumber `
        $operational 'epoch' 'session-operational epoch'
    [int]$planMergeDay = Get-RequiredTelemetryNumber `
        $sessionPlan 'mergeDay' 'authoritative SessionPlan merge day'
    [int]$operationalMergeDay = Get-RequiredTelemetryNumber `
        $operational 'mergeDay' 'session-operational merge day'
    if ($planEpoch -le 0 -or
        $hostDeliveredEpoch -ne $planEpoch -or
        $joinDeliveredEpoch -ne $planEpoch -or
        $operationalEpoch -ne $planEpoch) {
        throw 'relay bootstrap evidence changed the authoritative SessionPlan epoch'
    }
    if ($planMergeDay -ne $MergeDay -or
        $operationalMergeDay -ne $planMergeDay) {
        throw 'relay bootstrap evidence does not match the requested merge day'
    }
    if ((Get-OptionalProperty $beginApplied 'handle') -ne $joinHandle -or
        (Get-OptionalProperty $turnInfoApplied 'handle') -ne $joinHandle -or
        (Get-OptionalProperty $commitDispatched 'joinHandle') -ne $joinHandle -or
        (Get-OptionalProperty $operationalDispatched 'joinHandle') -ne $joinHandle -or
        (Get-OptionalProperty $bootstrapReleased 'joinHandle') -ne $joinHandle) {
        throw 'relay bootstrap evidence changed the negotiated join handle'
    }
    [long]$actionId = Get-RequiredTelemetryNumber `
        $cascade 'actionId' 'bootstrap apply action'
    if ($actionId -le 0 -or
        [int](Get-OptionalProperty $cascade 'kind') -ne 1 -or
        [long](Get-OptionalProperty $cascade 'lease') -ne
            [long](Get-OptionalProperty $sessionPlan 'joinLease') -or
        [long](Get-OptionalProperty $cascadeComplete 'actionId') -ne $actionId) {
        throw 'relay bootstrap cascade completion does not acknowledge its exact dispatch'
    }
    if ([int](Get-OptionalProperty $delayArmed 'delayMs') -ne 500 -or
        [string](Get-OptionalProperty $delayArmed 'anchor') -ne
            'bootstrap-begin-turn-applied') {
        throw 'relay bootstrap delay evidence changed its fixed causal clock'
    }

    $hostPlanDeliveredIndex = Get-SimEventRecordIndex $Events $hostPlanDelivered
    $planIndex = Get-SimEventRecordIndex $Events $sessionPlan
    $hostActivatedIndex = Get-SimEventRecordIndex $Events $hostActivated
    $joinPlanDeliveredIndex = Get-SimEventRecordIndex $Events $joinPlanDelivered
    $joinActivatedIndex = Get-SimEventRecordIndex $Events $joinActivated
    $beginIndex = Get-SimEventRecordIndex $Events $beginApplied
    $delayIndex = Get-SimEventRecordIndex $Events $delayArmed
    $cascadeIndex = Get-SimEventRecordIndex $Events $cascade
    $cascadeCompleteIndex = Get-SimEventRecordIndex $Events $cascadeComplete
    $turnInfoIndex = Get-SimEventRecordIndex $Events $turnInfoApplied
    $commitIndex = Get-SimEventRecordIndex $Events $commitDispatched
    $hostCommitAppliedIndex = Get-SimEventRecordIndex $Events $hostCommitApplied
    $joinCommitAppliedIndex = Get-SimEventRecordIndex $Events $joinCommitApplied
    $operationalDispatchedIndex = Get-SimEventRecordIndex $Events $operationalDispatched
    $hostOperationalAppliedIndex = Get-SimEventRecordIndex $Events $hostOperationalApplied
    $joinOperationalAppliedIndex = Get-SimEventRecordIndex $Events $joinOperationalApplied
    $bootstrapReleasedIndex = Get-SimEventRecordIndex $Events $bootstrapReleased
    $operationalIndex = Get-SimEventRecordIndex $Events $operational
    if ($hostPlanDeliveredIndex -lt 0 -or
        $planIndex -le $hostPlanDeliveredIndex -or
        $hostActivatedIndex -le $planIndex -or
        $joinPlanDeliveredIndex -le $hostActivatedIndex -or
        $joinActivatedIndex -le $joinPlanDeliveredIndex -or
        $beginIndex -le $joinActivatedIndex -or
        $delayIndex -le $beginIndex -or
        $cascadeIndex -le $hostActivatedIndex -or
        $cascadeIndex -le $joinActivatedIndex -or
        $cascadeIndex -le $delayIndex -or
        $cascadeCompleteIndex -le $cascadeIndex -or
        $turnInfoIndex -le $cascadeIndex -or
        $commitIndex -le $cascadeCompleteIndex -or
        $commitIndex -le $turnInfoIndex -or
        $hostCommitAppliedIndex -le $commitIndex -or
        $joinCommitAppliedIndex -le $commitIndex -or
        $operationalDispatchedIndex -le $hostCommitAppliedIndex -or
        $operationalDispatchedIndex -le $joinCommitAppliedIndex -or
        $hostOperationalAppliedIndex -le $operationalDispatchedIndex -or
        $joinOperationalAppliedIndex -le $operationalDispatchedIndex -or
        $bootstrapReleasedIndex -le $hostOperationalAppliedIndex -or
        $bootstrapReleasedIndex -le $joinOperationalAppliedIndex -or
        $operationalIndex -le $bootstrapReleasedIndex) {
        throw 'relay bootstrap evidence is missing its exact causal order'
    }
}

function Assert-ProductionRelayClientIdentity(
    [object[]]$Events,
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess) {
    $hostHello = Get-ExactSimEvent $Events 'hello-accepted' 'host'
    $joinHello = Get-ExactSimEvent $Events 'hello-accepted' 'join'
    $hostActualProcessId = Get-OptionalProperty $hostHello 'pid'
    $joinActualProcessId = Get-OptionalProperty $joinHello 'pid'
    if ($null -eq $hostActualProcessId -or
        [long]$hostActualProcessId -ne [long]$HostProcess.Id -or
        $null -eq $joinActualProcessId -or
        [long]$joinActualProcessId -ne [long]$JoinProcess.Id) {
        throw ('production relay Hello identity does not match the owned clients ' +
            "(host=$hostActualProcessId/$($HostProcess.Id) " +
            "join=$joinActualProcessId/$($JoinProcess.Id))")
    }
}

function Assert-NoRelayFault([object[]]$Events) {
    $fault = $Events | Where-Object {
        (Get-OptionalProperty $_ 'event') -in @(
            'session-faulted', 'protocol-error', 'server-error', 'socket-error',
            'hello-rejected', 'peer-disconnected')
    } | Select-Object -First 1
    if ($fault) {
        $reason = Get-OptionalProperty $fault 'reason'
        $message = Get-OptionalProperty $fault 'message'
        $detail = if ($reason) { $reason } elseif ($message) { $message } else { 'unspecified' }
        throw "production relay reported $(Get-OptionalProperty $fault 'event'): $detail"
    }
}

function Move-StaleMssClientLogsToArtifact {
    param(
        [Parameter(Mandatory)][string]$SourceGameDir,
        [Parameter(Mandatory)][string]$RunArtifactDir
    )
    $sourceRoot = [IO.Path]::GetFullPath($SourceGameDir)
    $artifactRoot = [IO.Path]::GetFullPath($RunArtifactDir)
    $archiveRoot = [IO.Path]::GetFullPath(
        (Join-Path $artifactRoot 'preexisting-client-logs'))
    $artifactPrefix = $artifactRoot.TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $archiveRoot.StartsWith(
            $artifactPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "preexisting client-log archive escaped the run artifact directory: $archiveRoot"
    }
    [void](New-Item -ItemType Directory -Path $archiveRoot -Force)

    $archived = [System.Collections.Generic.List[object]]::new()
    $preservedLive = [System.Collections.Generic.List[object]]::new()
    foreach ($item in @(Get-ChildItem -LiteralPath $sourceRoot -Filter 'mss32_*.log' `
            -File -ErrorAction Stop | Sort-Object Name)) {
        if ($item.Name -notmatch '^mss32_([1-9][0-9]*)\.log$') { continue }
        [long]$embeddedPid = $Matches[1]
        $liveOwner = Get-Process -Id $embeddedPid -ErrorAction SilentlyContinue
        if ($liveOwner) {
            $preservedLive.Add([pscustomobject]@{
                path = [IO.Path]::GetFullPath($item.FullName)
                pid = $embeddedPid
                processName = [string]$liveOwner.ProcessName
            })
            continue
        }

        $sourcePath = [IO.Path]::GetFullPath($item.FullName)
        $destinationPath = [IO.Path]::GetFullPath(
            (Join-Path $archiveRoot $item.Name))
        if (Test-Path -LiteralPath $destinationPath) {
            throw "preexisting client-log archive target already exists: $destinationPath"
        }
        $beforeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash
        $beforeLength = [long]$item.Length
        Move-Item -LiteralPath $sourcePath -Destination $destinationPath -ErrorAction Stop
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
            throw "stale client log remained at its source after archive: $sourcePath"
        }
        $archivedItem = Get-Item -LiteralPath $destinationPath -ErrorAction Stop
        $afterHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destinationPath).Hash
        if ([long]$archivedItem.Length -ne $beforeLength -or
            -not [string]::Equals(
                $afterHash, $beforeHash, [StringComparison]::OrdinalIgnoreCase)) {
            throw "stale client log changed while being archived: $sourcePath"
        }
        $archived.Add([pscustomobject]@{
            source = $sourcePath
            archived = $destinationPath
            pid = $embeddedPid
            length = $beforeLength
            sha256 = $beforeHash
        })
    }

    $manifest = [pscustomobject]@{
        archiveDir = $archiveRoot
        archived = @($archived)
        preservedLive = @($preservedLive)
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath `
        (Join-Path $archiveRoot 'manifest.json') -Encoding utf8
    return $manifest
}

function Assert-ProductionRelayHealthy {
    if ($script:LiteralInnerStartupObserver) {
        Assert-LiteralInnerStartupObserverHealthy `
            $script:LiteralInnerStartupObserver
    }
    $process = $script:ProductionRelayProcess
    if (-not $process) { throw 'production relay identity is not bound' }
    $process.Refresh()
    if ($process.HasExited) {
        $stderr = if (Test-Path -LiteralPath $script:SimRelayErrorLog) {
            (Get-Content -LiteralPath $script:SimRelayErrorLog -Tail 20) -join ' | '
        } else { '' }
        throw "production relay exited before the UI action (code $($process.ExitCode)): $stderr"
    }
    Assert-NoRelayFault @(Read-SimRelayEvents)
}

function Wait-SimCondition {
    param(
        [Parameter(Mandatory)][scriptblock]$Condition,
        [Parameter(Mandatory)][string]$Description,
        [int]$TimeoutSec = $StepTimeoutSec,
        [System.Diagnostics.Process]$RelayProcess
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($RelayProcess) {
            $RelayProcess.Refresh()
            if ($RelayProcess.HasExited) {
                $stderr = if (Test-Path -LiteralPath $script:SimRelayErrorLog) {
                    (Get-Content -LiteralPath $script:SimRelayErrorLog -Tail 20) -join ' | '
                } else { '' }
                throw "production relay exited while waiting for $Description (code $($RelayProcess.ExitCode)): $stderr"
            }
        }
        $events = @(Read-SimRelayEvents)
        Assert-NoRelayFault $events
        if (& $Condition $events) { return $events }
        Start-Sleep -Milliseconds 250
    }
    throw "timed out after ${TimeoutSec}s waiting for $Description"
}

function Start-ProductionSimRelay {
    $node = Get-Command node -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $versionText = (& $node.Source --version).TrimStart('v')
    $major = 0
    if (-not [int]::TryParse(($versionText -split '\.')[0], [ref]$major) -or $major -lt 20) {
        throw "Node.js 20 or newer is required; found '$versionText'"
    }

    $args = @(
        "`"$productionRelayCli`"",
        '--pipe', "`"$PipeName`"",
        '--merge-day', [string]$MergeDay,
        '--bootstrap-release-file', "`"$script:LiteralInnerBootstrapReleaseFile`"",
        '--bootstrap-cascade-delay-ms', '500'
    )
    $process = $null
    try {
        $process = Start-Process -FilePath $node.Source -ArgumentList $args -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $script:SimRelayLog -RedirectStandardError $script:SimRelayErrorLog
        [void](Wait-SimCondition -RelayProcess $process -TimeoutSec 15 -Description 'production relay listening' -Condition {
            param($events)
            0 -lt (Get-SimEventCount $events 'listening')
        })
        $process | Add-Member -NotePropertyName RelayLogPath -NotePropertyValue $script:SimRelayLog -Force
        $process | Add-Member -NotePropertyName RelayErrorLogPath -NotePropertyValue $script:SimRelayErrorLog -Force
        $script:ProductionRelayProcess = $process
        return $process
    } catch {
        $startupError = $_
        if ($process) {
            try {
                Stop-OwnedProcess $process 5000
            } catch {
                throw "production relay startup failed: $($startupError.Exception.Message); " +
                      "owned relay teardown also failed: $($_.Exception.Message)"
            }
        }
        throw $startupError
    }
}

# A role-specific production environment layered onto the existing DebugTest
# client flags. No environment mutation is shared between the two launches.
function Start-SimturnGameClient([string]$Role) {
    if ($Role -notin @('host', 'join')) { throw "unsupported test role '$Role'" }
    # The PID is assigned by Process.Start, so snapshot every possible per-PID
    # log immediately before launch. Stale, non-live owners were already moved
    # into the run artifact before any source timing edge; the returned PID must
    # therefore own a new log from byte zero. A live ambient PID is preserved
    # and cannot be reused by either process launched here.
    $preLaunchLogLengths = @{}
    foreach ($existingLog in @(Get-ChildItem -LiteralPath $GameDir -Filter 'mss32_*.log' `
            -File -ErrorAction SilentlyContinue)) {
        $existingFullPath = [IO.Path]::GetFullPath($existingLog.FullName)
        $preLaunchLogLengths[$existingFullPath] = [long]$existingLog.Length
    }

    $launchExecutablePath = [IO.Path]::GetFullPath(
        (Join-Path $GameDir 'Discipl2.exe'))
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $launchExecutablePath
    $psi.WorkingDirectory = $GameDir
    $psi.UseShellExecute = $false

    # Preserve only the endpoint settings shared with the already-owned DebugTest
    # relay. Remove every inherited action/configuration flag from both the
    # current harness and the old lobby proxy before applying this run's allowlist.
    $relayTransportEnvironment = @{}
    foreach ($name in @(
        'D2TESTDRV_PIPE_NAME',
        'D2TESTDRV_BRIDGE_TCP_HOST',
        'D2TESTDRV_BRIDGE_TCP_PORT'
    )) {
        if ($psi.EnvironmentVariables.ContainsKey($name)) {
            $relayTransportEnvironment[$name] = $psi.EnvironmentVariables[$name]
        }
    }

    $clientEnvironmentNames = [string[]]@($psi.EnvironmentVariables.Keys)
    foreach ($name in $clientEnvironmentNames) {
        if ($name.StartsWith('D2TESTDRV_', [StringComparison]::OrdinalIgnoreCase) -or
            $name.StartsWith('D2MSS_SIMTURNS', [StringComparison]::OrdinalIgnoreCase) -or
            $name.StartsWith('D2LOBBY_', [StringComparison]::OrdinalIgnoreCase)) {
            [void]$psi.EnvironmentVariables.Remove($name)
        }
    }

    foreach ($name in $relayTransportEnvironment.Keys) {
        $psi.EnvironmentVariables[$name] = [string]$relayTransportEnvironment[$name]
    }
    foreach ($flag in @(
        'SKIP_INTRO',
        'BLACKSCREEN_FIX',
        'UI_REPORTER',
        'WORLD',
        'RELAY_BRIDGE',
        'TURN_EVENTS',
        'SCRIPTED_POPUPS'
    )) {
        $psi.EnvironmentVariables["D2TESTDRV_$flag"] = '1'
    }
    $exactLegacyMovePlan =
        $GameplayMode -in @('canonical', 'battle-block', 'long-attack') -or
        ($GameplayMode -eq 'long-move' -and
         $LongMoveCase -in @('source-route-control', 'source-pair-repro'))
    $cleanLongMovePlan =
        $GameplayMode -eq 'long-move' -and
        $LongMoveCase -in @(
            'clean-host-route-control',
            'clean-join-route-control',
            'clean-long-concurrency'
        )
    if ($exactLegacyMovePlan -and $cleanLongMovePlan) {
        throw 'client launch selected two mutually exclusive move command plans'
    }
    if ($exactLegacyMovePlan) {
        $psi.EnvironmentVariables['D2TESTDRV_EXACT_LEGACY_MOVES'] = '1'
    } elseif ($cleanLongMovePlan) {
        $psi.EnvironmentVariables['D2TESTDRV_CLEAN_LONG_MOVES'] = '1'
    }
    if ($GameplayMode -eq 'long-move' -and
        $LongMoveCase -in @('source-route-control', 'source-pair-repro')) {
        # Explicit historical route diagnostics only. Canonical acceptance keeps
        # the same command geometry but must traverse the ordinary native queue.
        $psi.EnvironmentVariables['D2TESTDRV_LEGACY_HOST_LOOPBACK'] = '1'
    }
    if ($GameplayMode -in @('canonical', 'long-attack') -or $BattleCase -eq 'dead-leader' -or
        $PostMergeContinuationMode -eq 'automatic-masstest-phase-c-literal') {
        # M12: the same session-long exact-once subscriber owns external
        # confirmation boxes too. The standalone literal masstest delegates every
        # BeginTurn/confirmation popup, including its pre-merge distinct-day
        # turns and Phase C, to this persistent native subscriber. PowerShell
        # never performs a popup action in that topology.
        $psi.EnvironmentVariables['D2TESTDRV_SCRIPTED_POPUPS_CONFIRMATIONS'] = '1'
    }
    $psi.EnvironmentVariables['D2TESTDRV_ROLE'] = $Role
    if ($BattleTrace) { $psi.EnvironmentVariables['D2TESTDRV_BATTLE_TRACE'] = '1' }
    else { [void]$psi.EnvironmentVariables.Remove('D2TESTDRV_BATTLE_TRACE') }
    $psi.EnvironmentVariables['D2TESTDRV_DIRECTPLAY_HOST'] = '127.0.0.1'
    # The source /api/stacks oracle was a host-authoritative 500 ms sampler of
    # live pointers registered at CMidStack::Stream. Keep that removable native
    # module out of protocol-only runs and out of the join process entirely.
    if ($Role -eq 'host' -and
        $GameplayMode -in @(
            'canonical', 'battle-block', 'long-move', 'long-attack', 'ordered-masstest')) {
        $psi.EnvironmentVariables['D2TESTDRV_LEGACY_STACKS'] = '1'
    }
    # Both clients read the same run-owned routing/fixture plan; only the host
    # applies setup, in exactly the gameplay modes that used reinforcement before.
    if ((Get-FileHash -LiteralPath $script:FixturePlanEvidence.path -Algorithm SHA256).Hash -cne
        $script:FixturePlanEvidence.sha256) {
        throw 'run-owned fixture plan changed before client launch'
    }
    $psi.EnvironmentVariables['D2TESTDRV_FIXTURE_PLAN'] = $script:FixturePlanEvidence.path
    if ($Role -eq 'host' -and
        $GameplayMode -in @('canonical', 'battle-block', 'long-move', 'long-attack')) {
        $psi.EnvironmentVariables['D2TESTDRV_APPLY_FIXTURE'] = '1'
    }
    # run_test.ps1 armed auto-battle before boot, including when Boot-Ready
    # stopped after deploy for battle_block_check.ps1. Preserve that process
    # lifetime for both literal gameplay ports.
    if ($GameplayMode -in @('canonical', 'battle-block', 'long-attack')) {
        $psi.EnvironmentVariables['D2TESTDRV_AUTO_BATTLE_PREARM'] = '1'
    }
    $psi.EnvironmentVariables['D2MSS_SIMTURNS'] = '1'
    $psi.EnvironmentVariables['D2MSS_SIMTURNS_ROLE'] = $Role
    $psi.EnvironmentVariables['D2MSS_SIMTURNS_PIPE'] = $PipeName
    $process = [System.Diagnostics.Process]::Start($psi)
    Add-Member -InputObject $process -NotePropertyName D2MssLaunchExecutablePath `
        -NotePropertyValue ([string]$psi.FileName)
    $clientLogPath = [IO.Path]::GetFullPath(
        (Join-Path $GameDir "mss32_$($process.Id).log"))
    $initialLength = if ($preLaunchLogLengths.ContainsKey($clientLogPath)) {
        [long]$preLaunchLogLengths[$clientLogPath]
    } else {
        [long]0
    }
    if ($initialLength -ne 0) {
        throw ("owned $Role process pid=$($process.Id) inherited a non-empty " +
            "pre-launch log '$clientLogPath' ($initialLength bytes)")
    }
    $script:ClientLogInitialLengths[$clientLogPath] = $initialLength
    $script:ClientLogOwnedProcessIds[$clientLogPath] = [long]$process.Id
    return $process
}

function Select-Settle([string]$Role,
                       [string]$Dialog,
                       [string]$ListBox,
                       [int]$Index,
                       [int]$ExpectedTotal = 0) {
    if (-not (Wait-Dialog $Role $Dialog 15)) { return $false }
    $observation = Get-DialogObservation $Role
    if (-not $observation -or -not $observation.Ready -or $observation.Dialog -ne $Dialog) {
        return $false
    }
    $target = Get-ReadyActionTarget $observation $Dialog
    [long]$targetInstance = Get-OptionalProperty $target 'instance'
    $initialState = Get-ReadyListBoxState `
        $observation $Dialog $ListBox $targetInstance
    if ($Index -lt 0 -or $Index -ge [int]$initialState.Total -or
        ($ExpectedTotal -gt 0 -and [int]$initialState.Total -ne $ExpectedTotal)) {
        return $false
    }
    Assert-ProductionRelayHealthy
    if (-not (Set-ListSelection $Role $Dialog $ListBox $Index `
                                  $targetInstance $observation.Instance)) { return $false }
    # Set-ListSelection does not resolve until the native driver has performed
    # the literal ten read-only checks on ten subsequent natural UI frames.
    # Preserve the captured appearance for the immediately following action;
    # do not add a second time-based settle or repeat the mutation here.
    $observation | Add-Member -NotePropertyName SelectionListBox -NotePropertyValue $ListBox
    $observation | Add-Member -NotePropertyName SelectionIndex -NotePropertyValue $Index
    $observation | Add-Member -NotePropertyName SelectionTotal -NotePropertyValue $ExpectedTotal
    return $observation
}

function Select-ScenarioPathSettle([string]$Role,
                                   [string]$Dialog,
                                   [string]$ListBox,
                                   [string]$ExactPath,
                                   [int]$ExpectedIndex) {
    if (-not (Wait-Dialog $Role $Dialog 15)) { return $false }
    $observation = Get-DialogObservation $Role
    if (-not $observation -or -not $observation.Ready -or
        $observation.Dialog -ne $Dialog) { return $false }
    $target = Get-ReadyActionTarget $observation $Dialog
    [long]$targetInstance = Get-OptionalProperty $target 'instance'
    $initialState = Get-ReadyListBoxState `
        $observation $Dialog $ListBox $targetInstance
    if ($ExpectedIndex -lt 0 -or $ExpectedIndex -ge [int]$initialState.Total) {
        return $false
    }
    Assert-ProductionRelayHealthy
    if (-not (Set-ScenarioSelection $Role $Dialog $ListBox $ExactPath `
                                      $targetInstance $observation.Instance)) {
        return $false
    }
    # The exact-path resolver has now completed the same native ten-frame
    # read-back contract. BTN_LOAD must consume the proven row directly.
    $observation | Add-Member -NotePropertyName SelectionListBox -NotePropertyValue $ListBox
    $observation | Add-Member -NotePropertyName SelectionIndex -NotePropertyValue $ExpectedIndex
    $observation | Add-Member -NotePropertyName SelectionTotal -NotePropertyValue ([int]$initialState.Total)
    return $observation
}

# Start one exact native ten-frame selection without synchronously owning its
# completion. Each physical role worker publishes one native intent, and its
# Task is only observed/validated on that worker's later driver tick. There is
# no second publication path.
function Start-LiteralSelectionRequest {
    param(
        [Parameter(Mandatory)][ValidateSet('index', 'scenario')][string]$Kind,
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$ListBox,
        [int]$Index = 0,
        [int]$ExpectedTotal = 0,
        [string]$ExactPath = '',
        [object]$CapturedObservation = $null
    )
    if ($null -eq $CapturedObservation) {
        throw "$Role $Dialog sole literal selection requires its captured ready observation"
    }
    # The relay atomically checks this captured dialog appearance, native owner
    # and listbox again before dispatch. A stale sole POST is rejected before it
    # reaches the client, so another client-side state read adds no safety.
    $observation = $CapturedObservation
    if (-not $observation -or -not $observation.Ready -or
        $observation.Dialog -ne $Dialog) {
        throw "$Role $Dialog changed before its sole literal selection"
    }
    $target = Get-ReadyActionTarget $observation $Dialog
    [long]$targetInstance = Get-OptionalProperty $target 'instance'
    $initialState = Get-ReadyListBoxState `
        $observation $Dialog $ListBox $targetInstance
    if ($Index -lt 0 -or $Index -ge [int]$initialState.Total -or
        ($ExpectedTotal -gt 0 -and [int]$initialState.Total -ne $ExpectedTotal)) {
        throw ("$Role $Dialog::$ListBox cannot select literal index $Index " +
            "from total=$([int]$initialState.Total)")
    }
    if ($Kind -eq 'scenario' -and [string]::IsNullOrWhiteSpace($ExactPath)) {
        throw 'literal exact-path scenario selection requires one non-empty path'
    }

    $query = if ($Kind -eq 'scenario') {
        "select-scenario?role=$([uri]::EscapeDataString($Role))" +
            "&dlg=$([uri]::EscapeDataString($Dialog))" +
            "&lb=$([uri]::EscapeDataString($ListBox))" +
            "&path=$([uri]::EscapeDataString($ExactPath))" +
            "&appearance=$([long]$observation.Instance)&instance=$targetInstance"
    } else {
        "select?role=$([uri]::EscapeDataString($Role))" +
            "&dlg=$([uri]::EscapeDataString($Dialog))" +
            "&lb=$([uri]::EscapeDataString($ListBox))&index=$Index" +
            "&appearance=$([long]$observation.Instance)&instance=$targetInstance"
    }
    $uri = "$script:RelayBase/api/ui/$query"
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.UseProxy = $false
    $handler.AllowAutoRedirect = $false
    $client = [System.Net.Http.HttpClient]::new($handler, $true)
    # The DebugTest relay/native driver owns the ten-natural-frame command
    # deadline. Cancelling here would leave an exact-once intent in flight.
    $client.Timeout = [System.Threading.Timeout]::InfiniteTimeSpan
    $request = [System.Net.Http.HttpRequestMessage]::new(
        [System.Net.Http.HttpMethod]::Post, $uri)
    try {
        Assert-ProductionRelayHealthy
        $task = $client.SendAsync($request)
        return [pscustomobject]@{
            Kind = $Kind
            Role = $Role
            Dialog = $Dialog
            ListBox = $ListBox
            Index = $Index
            ExpectedTotal = $ExpectedTotal
            ExactPath = $ExactPath
            Appearance = [long]$observation.Instance
            Owner = $targetInstance
            Observation = $observation
            Uri = $uri
            Client = $client
            Request = $request
            Task = $task
            Completed = $false
        }
    } catch {
        $request.Dispose()
        $client.Dispose()
        throw
    }
}

function Complete-LiteralSelectionRequest([object]$Pending) {
    if ([bool]$Pending.Completed) {
        throw "$($Pending.Role) literal selection completion was consumed twice"
    }
    $Pending.Completed = $true
    $httpResponse = $null
    try {
        $httpResponse = $Pending.Task.GetAwaiter().GetResult()
        $body = $httpResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if (-not $httpResponse.IsSuccessStatusCode) {
            throw ("HTTP {0} ({1}) from {2}: {3}" -f
                [int]$httpResponse.StatusCode, $httpResponse.ReasonPhrase,
                $Pending.Uri, $body)
        }
        $response = $body | ConvertFrom-Json
        if ([string]$response.role -ne [string]$Pending.Role -or
            [bool]$response.found -ne $true) {
            throw "$($Pending.Role) literal $($Pending.Kind) selection returned a negative/mismatched result"
        }
        $echo = if ([string]$Pending.Kind -eq 'scenario') {
            Get-OptionalProperty $response 'scenario'
        } else {
            Get-OptionalProperty $response 'select'
        }
        if (-not $echo -or [string]$echo.dlg -ne [string]$Pending.Dialog -or
            [string]$echo.lb -ne [string]$Pending.ListBox -or
            [long]$echo.appearance -ne [long]$Pending.Appearance -or
            [long]$echo.instance -ne [long]$Pending.Owner) {
            throw "$($Pending.Role) literal selection echo changed its captured native identity"
        }
        if ([string]$Pending.Kind -eq 'scenario') {
            if ([string]$echo.path -ne [string]$Pending.ExactPath) {
                throw 'literal scenario-selection echo changed its exact path'
            }
        } elseif ([int]$echo.index -ne [int]$Pending.Index) {
            throw 'literal list-selection echo changed its exact index'
        }

        $observation = $Pending.Observation
        $observation | Add-Member -NotePropertyName SelectionListBox `
            -NotePropertyValue ([string]$Pending.ListBox)
        $observation | Add-Member -NotePropertyName SelectionIndex `
            -NotePropertyValue ([int]$Pending.Index)
        $observation | Add-Member -NotePropertyName SelectionTotal `
            -NotePropertyValue ([int]$Pending.ExpectedTotal)
        return $observation
    } finally {
        if ($httpResponse) { $httpResponse.Dispose() }
        $Pending.Request.Dispose()
        $Pending.Client.Dispose()
    }
}

function Start-LiteralButtonRequest {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Button,
        [Parameter(Mandatory)][object]$CapturedObservation,
        [int]$CommandTimeoutMilliseconds = 90000
    )
    if ($CommandTimeoutMilliseconds -lt 1000 -or
        $CommandTimeoutMilliseconds -gt 120000) {
        throw 'literal async button timeout is outside 1000..120000 ms'
    }
    # Most navigation controls belong to an immutable appearance and keep the
    # exact captured owner. DLG_LOBBY is different: the stock roster can publish
    # a new ready appearance between the source Ui() predicate and its sole OK.
    # Arm one symbolic relay intent from that predicate's watermark so either
    # the captured publication or its first later ready replacement can become
    # the owner. Host roster rebinding briefly republishes a ready appearance,
    # so that exact host identity must remain continuously ready for 500 ms
    # before the one command. The join path retains its original immediate
    # behavior. No second role-state read, POST or command send is performed.
    $capturedTarget = Get-ReadyActionTarget $CapturedObservation $Dialog
    [long]$capturedOwner = Get-OptionalProperty $capturedTarget 'instance'
    [long]$capturedButtonOwner =
        Assert-ReadyButtonSnapshot $CapturedObservation $Button $Dialog
    if ($capturedButtonOwner -ne $capturedOwner) {
        throw "$Role $Dialog::$Button captured inconsistent native owners"
    }
    [bool]$readyOwnerIntent =
        $Dialog -eq 'DLG_LOBBY' -and $Button -eq 'BTN_OK'
    [long]$appearance = 0
    [long]$buttonOwner = 0
    [long]$afterUiSequence = 0
    Assert-ProductionRelayHealthy

    if ($readyOwnerIntent) {
        $sequenceValue = Get-OptionalProperty $CapturedObservation.State 'uiSeq'
        if ($null -eq $sequenceValue -or
            [long]$sequenceValue -lt 1 -or
            [long]$sequenceValue -gt [uint32]::MaxValue) {
            throw "$Role DLG_LOBBY predicate omitted its exact UI watermark"
        }
        $afterUiSequence = [long]$sequenceValue - 1
        $stabilityQuery = if ($Role -eq 'host') {
            '&stableMs=500'
        } else { '' }
        $uri = ('{0}/api/ui/invoke-when-ready?role={1}&dlg={2}&btn={3}' +
            '&after={4}&waitMs=30000{5}&timeoutMs={6}') -f `
            $script:RelayBase,
            [uri]::EscapeDataString($Role),
            [uri]::EscapeDataString($Dialog),
            [uri]::EscapeDataString($Button),
            $afterUiSequence,
            $stabilityQuery,
            $CommandTimeoutMilliseconds
    } else {
        # `/api/ui/invoke` atomically validates the captured appearance, owner,
        # dialog and enabled button. A stale sole POST fails before native
        # dispatch; another passive state read would only add a TOCTOU window.
        $candidate = $CapturedObservation
        if (-not $candidate -or -not $candidate.Ready -or
            [string]$candidate.Dialog -ne $Dialog -or
            [long]$candidate.Instance -ne [long]$CapturedObservation.Instance) {
            throw "$Role $Dialog changed before its sole independent navigation action"
        }
        [void](Get-ReadyActionTarget $candidate $Dialog $capturedOwner)
        $buttonOwner = Assert-ReadyButtonSnapshot $candidate $Button $Dialog
        if ($buttonOwner -ne $capturedOwner) {
            throw "$Role $Dialog::$Button changed its exact native owner"
        }
        $appearance = [long]$candidate.Instance
        $uri = ('{0}/api/ui/invoke?role={1}&dlg={2}&btn={3}' +
            '&appearance={4}&instance={5}&timeoutMs={6}') -f `
            $script:RelayBase,
            [uri]::EscapeDataString($Role),
            [uri]::EscapeDataString($Dialog),
            [uri]::EscapeDataString($Button),
            $appearance,
            $buttonOwner,
            $CommandTimeoutMilliseconds
    }
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.UseProxy = $false
    $handler.AllowAutoRedirect = $false
    $client = [System.Net.Http.HttpClient]::new($handler, $true)
    # The relay/native command owns its one fixed deadline. Never cancel an
    # exact-once navigation intent merely because the other role finishes first.
    $client.Timeout = [System.Threading.Timeout]::InfiniteTimeSpan
    $request = [System.Net.Http.HttpRequestMessage]::new(
        [System.Net.Http.HttpMethod]::Post, $uri)
    try {
        $startedUtc = [DateTime]::UtcNow
        $task = $client.SendAsync($request)
        return [pscustomobject]@{
            Role = $Role
            Dialog = $Dialog
            Button = $Button
            Appearance = $appearance
            Owner = $buttonOwner
            ReadyOwnerIntent = $readyOwnerIntent
            AfterUiSequence = $afterUiSequence
            CapturedAppearance = [long]$CapturedObservation.Instance
            CapturedOwner = $capturedOwner
            Uri = $uri
            StartedUtc = $startedUtc
            Client = $client
            Request = $request
            Task = $task
            Completed = $false
        }
    } catch {
        $request.Dispose()
        $client.Dispose()
        throw
    }
}

function Complete-LiteralButtonRequest([object]$Pending) {
    if ([bool]$Pending.Completed) {
        throw "$($Pending.Role) $($Pending.Dialog)::$($Pending.Button) completion was consumed twice"
    }
    # Claim completion before the wait; any failure is terminal and can never
    # be converted into another POST for this appearance/owner.
    $Pending.Completed = $true
    $httpResponse = $null
    try {
        $httpResponse = $Pending.Task.GetAwaiter().GetResult()
        $body = $httpResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if (-not $httpResponse.IsSuccessStatusCode) {
            throw ("HTTP {0} ({1}) from {2}: {3}" -f
                [int]$httpResponse.StatusCode, $httpResponse.ReasonPhrase,
                $Pending.Uri, $body)
        }
        $response = $body | ConvertFrom-Json
        $echo = Get-OptionalProperty $response 'invoke'
        if ([string]$response.role -ne [string]$Pending.Role -or
            -not [bool]$response.found -or -not $echo -or
            [string]$echo.dlg -ne [string]$Pending.Dialog -or
            [string]$echo.btn -ne [string]$Pending.Button) {
            throw "$($Pending.Role) sole navigation response changed its exact request identity"
        }
        [long]$appearance = [long]$Pending.Appearance
        [long]$owner = [long]$Pending.Owner
        if ([bool]$Pending.ReadyOwnerIntent) {
            $observation = Get-OptionalProperty $response 'observation'
            $targets = Get-OptionalProperty $observation 'targets'
            $roots = @($targets | Where-Object {
                [string](Get-OptionalProperty $_ 'dialog') -eq
                    [string]$Pending.Dialog
            })
            $widgets = if ($roots.Count -eq 1) {
                Get-OptionalProperty $roots[0] 'widgets'
            } else { $null }
            $buttons = @($widgets | Where-Object {
                [string](Get-OptionalProperty $_ 'name') -eq
                    [string]$Pending.Button -and
                [string](Get-OptionalProperty $_ 'type') -eq 'button' -and
                [bool](Get-OptionalProperty `
                    (Get-OptionalProperty $_ 'state') 'enabled')
            })
            $appearance = [long](Get-OptionalProperty `
                $observation 'dialogAppearance')
            $owner = if ($roots.Count -eq 1) {
                [long](Get-OptionalProperty $roots[0] 'instance')
            } else { 0 }
            if (-not $observation -or
                [string](Get-OptionalProperty $observation 'role') -ne
                    [string]$Pending.Role -or
                [string](Get-OptionalProperty $observation 'dialog') -ne
                    [string]$Pending.Dialog -or
                (Get-OptionalProperty $observation 'dialogReady') -ne $true -or
                [long](Get-OptionalProperty $observation 'uiSeq') -le
                    [long]$Pending.AfterUiSequence -or
                [long](Get-OptionalProperty $observation 'dialogInstance') -ne
                    $appearance -or
                $appearance -lt 1 -or $roots.Count -ne 1 -or
                $owner -lt 1 -or $buttons.Count -ne 1 -or
                [long]$echo.appearance -ne $appearance -or
                [long]$echo.instance -ne $owner) {
                throw "$($Pending.Role) lobby intent returned a malformed exact-ready owner proof"
            }
        } elseif ([long]$echo.appearance -ne $appearance -or
                  [long]$echo.instance -ne $owner) {
            throw "$($Pending.Role) sole navigation response changed its captured native owner"
        }
        return [pscustomobject]@{
            Role = [string]$Pending.Role
            Dialog = [string]$Pending.Dialog
            Button = [string]$Pending.Button
            Appearance = $appearance
            Owner = $owner
            StartedUtc = [DateTime]$Pending.StartedUtc
            CompletedUtc = [DateTime]::UtcNow
        }
    } finally {
        if ($httpResponse) { $httpResponse.Dispose() }
        $Pending.Request.Dispose()
        $Pending.Client.Dispose()
    }
}

function Invoke-LiteralHostNavigationLane {
    param(
        [Parameter(Mandatory)][System.Diagnostics.Process]$OwnedProcess,
        [Parameter(Mandatory)][DateTime]$LaunchUtc,
        [Parameter(Mandatory)][string]$ClientLog,
        [Parameter(Mandatory)][string]$WaitPeerMarker,
        [string]$ExactScenarioPath = '',
        [int]$ExactScenarioIndex = 0,
        [int]$ScenarioIndex = 0,
        [Parameter(Mandatory)][int]$TimeoutSec,
        [Parameter(Mandatory)]
        [System.Threading.ManualResetEventSlim]$StartGate
    )
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version Latest
    # Neither role performs even a passive HTTP/log read until both physical
    # runspaces have reached this common release edge. Each lane then owns a
    # complete timeout budget measured from its own actual release.
    $StartGate.Wait()
    $laneStartedUtc = [DateTime]::UtcNow
    $deadlineUtc = $laneStartedUtc.AddSeconds($TimeoutSec)
    $waitPeerBefore = Get-ClientLogMarkerCount $ClientLog $WaitPeerMarker
    if ($waitPeerBefore -ne 0) {
        throw 'host peer CConnectMsg marker existed before the sole join action'
    }
    $lane = [pscustomobject]@{
        Phase = 'anchor'
        MainMenu = $null
        Pending = $null
        Continue = $null
        Create = $null
        Load = $null
        WaitPeer = $null
        WaitPeerReleaseUtc = $null
        LobbyArmUtc = $null
        LobbyOk = $null
        PopupArmUtc = $null
        PopupService = $null
        PopupWindow = $null
    }
    $startedSelections = [System.Collections.Generic.List[object]]::new()
    $startedButtons = [System.Collections.Generic.List[object]]::new()
    try {
        while ($lane.Phase -ne 'done' -and [DateTime]::UtcNow -lt $deadlineUtc) {
            # hooks.cpp@3f683e4: when the UI thread clears g_pending_step, this
            # driver tick immediately arms the next step. A Delay blocks only
            # this role thread, increments the step and continues without the
            # ordinary bottom Sleep(100).
            $advanceWithinDriverTick = $true
            while ($advanceWithinDriverTick -and $lane.Phase -ne 'done') {
                $advanceWithinDriverTick = $false
                if ($lane.Phase -eq 'anchor') {
                    $lane.MainMenu = Get-LiteralMainMenuAtLaunchAnchor `
                        host $OwnedProcess $LaunchUtc $deadlineUtc
                    $lane.Pending = Start-LiteralButtonRequest `
                        -Role host -Dialog DLG_MAIN_MENU -Button BTN_MULTI `
                        -CapturedObservation $lane.MainMenu
                    [void]$startedButtons.Add($lane.Pending)
                    $lane.Phase = 'main-menu'
                } elseif ($lane.Phase -eq 'main-menu' -and
                          $lane.Pending.Task.IsCompleted) {
                    [void](Complete-LiteralButtonRequest $lane.Pending)
                    $lane.Pending = $null
                    $lane.Phase = 'protocol-dialog'
                    $advanceWithinDriverTick = $true
                } elseif ($lane.Phase -eq 'protocol-dialog') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role host -Context 'host/protocol-dialog' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_PROTOCOL') {
                        $lane.Pending = Start-LiteralSelectionRequest `
                            -Kind index -Role host -Dialog DLG_PROTOCOL `
                            -ListBox TLBOX_PROTOCOL -Index 2 -ExpectedTotal 3 `
                            -CapturedObservation $observation
                        [void]$startedSelections.Add($lane.Pending)
                        $lane.Phase = 'protocol-selection'
                    }
                } elseif ($lane.Phase -eq 'protocol-selection' -and
                          $lane.Pending.Task.IsCompleted) {
                    $selection = Complete-LiteralSelectionRequest $lane.Pending
                    $lane.Pending = Start-LiteralButtonRequest `
                        -Role host -Dialog DLG_PROTOCOL -Button BTN_CONTINUE `
                        -CapturedObservation $selection
                    [void]$startedButtons.Add($lane.Pending)
                    $lane.Phase = 'continue'
                } elseif ($lane.Phase -eq 'continue' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.Continue = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    $lane.Phase = 'load-new-multi'
                    $advanceWithinDriverTick = $true
                } elseif ($lane.Phase -eq 'load-new-multi') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role host -Context 'host/load-new-multi' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_LOAD_NEW_MULTI') {
                        $lane.Pending = Start-LiteralButtonRequest `
                            -Role host -Dialog DLG_LOAD_NEW_MULTI -Button BTN_HOST `
                            -CapturedObservation $observation
                        [void]$startedButtons.Add($lane.Pending)
                        $lane.Phase = 'create'
                    }
                } elseif ($lane.Phase -eq 'create' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.Create = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    $lane.Phase = 'scenario-dialog'
                    $advanceWithinDriverTick = $true
                } elseif ($lane.Phase -eq 'scenario-dialog') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role host -Context 'host/scenario-dialog' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_CHOOSE_SKIRMISH') {
                        if (-not [string]::IsNullOrWhiteSpace($ExactScenarioPath)) {
                            $lane.Pending = Start-LiteralSelectionRequest `
                                -Kind scenario -Role host -Dialog DLG_CHOOSE_SKIRMISH `
                                -ListBox TLBOX_GAME_SLOT -Index $ExactScenarioIndex `
                                -ExactPath $ExactScenarioPath `
                                -CapturedObservation $observation
                        } else {
                            $lane.Pending = Start-LiteralSelectionRequest `
                                -Kind index -Role host -Dialog DLG_CHOOSE_SKIRMISH `
                                -ListBox TLBOX_GAME_SLOT -Index $ScenarioIndex `
                                -CapturedObservation $observation
                        }
                        [void]$startedSelections.Add($lane.Pending)
                        $lane.Phase = 'scenario-selection'
                    }
                } elseif ($lane.Phase -eq 'scenario-selection' -and
                          $lane.Pending.Task.IsCompleted) {
                    $selection = Complete-LiteralSelectionRequest $lane.Pending
                    $lane.Pending = Start-LiteralButtonRequest `
                        -Role host -Dialog DLG_CHOOSE_SKIRMISH -Button BTN_LOAD `
                        -CapturedObservation $selection
                    [void]$startedButtons.Add($lane.Pending)
                    $lane.Phase = 'load'
                } elseif ($lane.Phase -eq 'load' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.Load = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    # WaitPeer is the next armed source step. Its predicate is
                    # not consumed until a later driver tick.
                    $lane.Phase = 'wait-peer'
                } elseif ($lane.Phase -eq 'wait-peer') {
                    $lane.WaitPeer = Get-LiteralClientLogMarkerEventUtcSnapshot `
                        $ClientLog $WaitPeerMarker $WaitPeerBefore
                    if ($lane.WaitPeer) {
                        $lane.WaitPeerReleaseUtc = [DateTime]$lane.WaitPeer.eventUtc
                        if ([DateTime]$lane.Load.CompletedUtc -gt
                            [DateTime]$lane.WaitPeerReleaseUtc) {
                            $lane.WaitPeerReleaseUtc = [DateTime]$lane.Load.CompletedUtc
                        }
                        $lane.LobbyArmUtc = `
                            ([DateTime]$lane.WaitPeerReleaseUtc).AddMilliseconds(500)
                        # Literal Delay(500): only this worker blocks and the
                        # following OK step is sampled/armed without +100 ms.
                        Wait-FixedUtcAnchor $lane.LobbyArmUtc
                        $lane.Phase = 'lobby-dialog'
                        $advanceWithinDriverTick = $true
                    }
                } elseif ($lane.Phase -eq 'lobby-dialog') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role host -Context 'host/lobby-dialog' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_LOBBY') {
                        $lane.Pending = Start-LiteralButtonRequest `
                            -Role host -Dialog DLG_LOBBY -Button BTN_OK `
                            -CapturedObservation $observation
                        [void]$startedButtons.Add($lane.Pending)
                        $lane.Phase = 'lobby-ok'
                    }
                } elseif ($lane.Phase -eq 'lobby-ok' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.LobbyOk = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    $lane.PopupArmUtc =
                        $lane.LobbyOk.CompletedUtc.AddMilliseconds(500)
                    # Literal Delay(500) continues directly into the source
                    # AutoDismiss arm. Its native actions remain independently
                    # subscribed while the outer path consumes append-only logs.
                    Wait-FixedUtcAnchor $lane.PopupArmUtc
                    $lane.PopupService =
                        New-LiteralStartupPopupService host $ClientLog
                    $lane.PopupWindow = New-LiteralStartupPopupWindow `
                        host 25000 $lane.PopupService
                    $lane.Phase = 'done'
                }
            }
            if ($lane.Phase -ne 'done') {
                # Ordinary armed/pending driver tick only. Delay transitions
                # above never pass through this sleep.
                Start-Sleep -Milliseconds 100
            }
        }
        if ($lane.Phase -ne 'done') {
            throw "host independent navigation timed out in phase $($lane.Phase)"
        }
        return [pscustomobject]@{
            Role = 'host'
            OwnedProcess = $OwnedProcess
            LaneStartedUtc = $laneStartedUtc
            DeadlineUtc = $deadlineUtc
            WaitPeerBefore = [int]$waitPeerBefore
            MainMenu = $lane.MainMenu
            Continue = $lane.Continue
            Create = $lane.Create
            Load = $lane.Load
            WaitPeer = $lane.WaitPeer
            WaitPeerReleaseUtc = [DateTime]$lane.WaitPeerReleaseUtc
            LobbyOk = $lane.LobbyOk
            PopupArmUtc = [DateTime]$lane.PopupArmUtc
            PopupService = $lane.PopupService
            PopupWindow = $lane.PopupWindow
        }
    } catch {
        $cause = $_.Exception.Message
        foreach ($selection in $startedSelections) {
            if (-not [bool]$selection.Completed) {
                try { [void](Complete-LiteralSelectionRequest $selection) }
                catch { $cause += "; pending selection completion failed: $($_.Exception.Message)" }
            }
        }
        foreach ($pending in $startedButtons) {
            if (-not [bool]$pending.Completed) {
                try { [void](Complete-LiteralButtonRequest $pending) }
                catch { $cause += "; pending button completion failed: $($_.Exception.Message)" }
            }
        }
        throw $cause
    }
}

function Invoke-LiteralJoinNavigationLane {
    param(
        [Parameter(Mandatory)][System.Diagnostics.Process]$OwnedProcess,
        [Parameter(Mandatory)][DateTime]$LaunchUtc,
        [Parameter(Mandatory)][string]$ClientLog,
        [long]$ExpectedInitialHostHandle = 0,
        [Parameter(Mandatory)][int]$TimeoutSec,
        [Parameter(Mandatory)]
        [System.Threading.ManualResetEventSlim]$StartGate
    )
    $ErrorActionPreference = 'Stop'
    Set-StrictMode -Version Latest
    $StartGate.Wait()
    $laneStartedUtc = [DateTime]::UtcNow
    $deadlineUtc = $laneStartedUtc.AddSeconds($TimeoutSec)
    # This worker owns the only pre-session turn cursor and EnumSessions log
    # boundary. The reads begin after both workers are live, and this join-local
    # work can never consume the host lane's earlier launch anchor.
    $startupTurnBaseline = Get-TurnHistory
    [long]$startupTurnWatermark = Get-EvidenceWatermark `
        $startupTurnBaseline 'pre-session startup'
    $sessionListReadyMarker =
        '[nettrace] EnumSessions ready on next natural UI frame generation='
    $sessionListReadyBefore =
        Get-ClientLogMarkerCount $ClientLog $sessionListReadyMarker
    $lane = [pscustomobject]@{
        Phase = 'anchor'
        MainMenu = $null
        Pending = $null
        Continue = $null
        Search = $null
        JoinGameArmUtc = $null
        JoinGame = $null
        StartupState = [pscustomobject]@{
            After = [long]$startupTurnWatermark
            Cursor = [long]$startupTurnWatermark
            ExpectedDay = [int]1
            ExpectedActiveHandle = [long]$ExpectedInitialHostHandle
            ObservedEvents = @()
            BriefingLatch = $null
            StrategicLatch = $null
            Witness = $null
        }
        StartupRequest = $null
        HostBriefingArmedUtc = $null
        HostBriefingReleasedUtc = $null
        HostStrategicArmedUtc = $null
        HostStrategicReleasedUtc = $null
        FirstSettleArmUtc = $null
        SecondSettleArmUtc = $null
        LobbyOk = $null
        PopupArmUtc = $null
        PopupService = $null
        PopupWindow = $null
    }
    $startedSelections = [System.Collections.Generic.List[object]]::new()
    $startedButtons = [System.Collections.Generic.List[object]]::new()
    # This append-only subscription belongs solely to the join worker. BeginTurn
    # and CJoinGame latch independently; neither predicate waits for the other.
    $lane.StartupRequest = Start-LiteralTurnHistoryRequest `
        -After $startupTurnWatermark -Role join -WaitMilliseconds 120000
    try {
        while ($lane.Phase -ne 'done' -and [DateTime]::UtcNow -lt $deadlineUtc) {
            if (-not $lane.StartupState.Witness -and
                $lane.StartupRequest.Task.IsCompleted) {
                $history = Complete-LiteralTurnHistoryRequest $lane.StartupRequest
                [void](Add-LiteralJoinStockStartupHistory `
                    $lane.StartupState $history)
                if (-not $lane.StartupState.Witness) {
                    $lane.StartupRequest = Start-LiteralTurnHistoryRequest `
                        -After $lane.StartupState.Cursor `
                        -Role join -WaitMilliseconds 120000
                }
            }

            $advanceWithinDriverTick = $true
            while ($advanceWithinDriverTick -and $lane.Phase -ne 'done') {
                $advanceWithinDriverTick = $false
                if ($lane.Phase -eq 'anchor') {
                    $lane.MainMenu = Get-LiteralMainMenuAtLaunchAnchor `
                        join $OwnedProcess $LaunchUtc $deadlineUtc
                    $lane.Pending = Start-LiteralButtonRequest `
                        -Role join -Dialog DLG_MAIN_MENU -Button BTN_MULTI `
                        -CapturedObservation $lane.MainMenu
                    [void]$startedButtons.Add($lane.Pending)
                    $lane.Phase = 'main-menu'
                } elseif ($lane.Phase -eq 'main-menu' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.MainMenu = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    $lane.Phase = 'protocol-dialog'
                    $advanceWithinDriverTick = $true
                } elseif ($lane.Phase -eq 'protocol-dialog') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role join -Context 'join/protocol-dialog' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_PROTOCOL') {
                        $lane.Pending = Start-LiteralSelectionRequest `
                            -Kind index -Role join -Dialog DLG_PROTOCOL `
                            -ListBox TLBOX_PROTOCOL -Index 2 -ExpectedTotal 3 `
                            -CapturedObservation $observation
                        [void]$startedSelections.Add($lane.Pending)
                        $lane.Phase = 'selection'
                    }
                } elseif ($lane.Phase -eq 'selection' -and
                          $lane.Pending.Task.IsCompleted) {
                    $selection = Complete-LiteralSelectionRequest $lane.Pending
                    $lane.Pending = Start-LiteralButtonRequest `
                        -Role join -Dialog DLG_PROTOCOL -Button BTN_CONTINUE `
                        -CapturedObservation $selection
                    [void]$startedButtons.Add($lane.Pending)
                    $lane.Phase = 'continue'
                } elseif ($lane.Phase -eq 'continue' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.Continue = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    $lane.Phase = 'load-new-multi'
                    $advanceWithinDriverTick = $true
                } elseif ($lane.Phase -eq 'load-new-multi') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role join -Context 'join/load-new-multi' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_LOAD_NEW_MULTI') {
                        $lane.Pending = Start-LiteralButtonRequest `
                            -Role join -Dialog DLG_LOAD_NEW_MULTI -Button BTN_JOIN `
                            -CapturedObservation $observation
                        [void]$startedButtons.Add($lane.Pending)
                        $lane.Phase = 'search'
                    }
                } elseif ($lane.Phase -eq 'search' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.Search = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    $lane.JoinGameArmUtc =
                        $lane.Search.CompletedUtc.AddMilliseconds(2000)
                    # Source Delay(2000) blocks this worker only and continues
                    # directly into the sole JOIN_GAME arm.
                    Wait-FixedUtcAnchor $lane.JoinGameArmUtc
                    $lane.Phase = 'join-session'
                    $advanceWithinDriverTick = $true
                } elseif ($lane.Phase -eq 'join-session') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role join -Context 'join/session-dialog' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_SESSION') {
                        $lane.Pending = Start-LiteralButtonRequest `
                            -Role join -Dialog DLG_SESSION -Button BTN_JOIN_GAME `
                            -CapturedObservation $observation
                        [void]$startedButtons.Add($lane.Pending)
                        $lane.Phase = 'join-game'
                    }
                } elseif ($lane.Phase -eq 'join-game' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.JoinGame = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    # Arm the first source predicate on this completion tick.
                    # Even a previously latched CJoinGame is consumed only after
                    # the ordinary WM_NULL/Sleep(100) driver edge below.
                    $lane.HostBriefingArmedUtc = [DateTime]::UtcNow
                    $lane.Phase = 'wait-host-briefing'
                } elseif ($lane.Phase -eq 'wait-host-briefing' -and
                          $lane.StartupState.BriefingLatch) {
                    # WaitHostBriefing is exactly the typed CJoinGame latch.
                    $lane.HostBriefingReleasedUtc = [DateTime]::UtcNow
                    # The next distinct WaitHostStrategic step is armed on this
                    # same driver tick, then receives its own bottom Sleep(100).
                    $lane.HostStrategicArmedUtc = [DateTime]::UtcNow
                    $lane.Phase = 'wait-host-strategic'
                } elseif ($lane.Phase -eq 'wait-host-strategic' -and
                          $lane.StartupState.StrategicLatch) {
                    # WaitHostStrategic is exactly the independent BeginTurn
                    # latch. It may already be true when CJoinGame arrives later.
                    $lane.HostStrategicReleasedUtc = [DateTime]::UtcNow
                    $lane.FirstSettleArmUtc =
                        $lane.HostStrategicReleasedUtc.AddMilliseconds(1000)
                    Wait-FixedUtcAnchor $lane.FirstSettleArmUtc
                    # Consecutive source Delay steps use two separate relative
                    # sleeps, and neither passes through the bottom 100 ms edge.
                    $lane.SecondSettleArmUtc =
                        [DateTime]::UtcNow.AddMilliseconds(1000)
                    Wait-FixedUtcAnchor $lane.SecondSettleArmUtc
                    $lane.Phase = 'lobby-dialog'
                    $advanceWithinDriverTick = $true
                } elseif ($lane.Phase -eq 'lobby-dialog') {
                    $observation = Get-LiteralNavigationDialogObservation `
                        -Role join -Context 'join/lobby-dialog' `
                        -DeadlineUtc $deadlineUtc
                    if ($observation -and $observation.Ready -and
                        [string]$observation.Dialog -eq 'DLG_LOBBY') {
                        $lane.Pending = Start-LiteralButtonRequest `
                            -Role join -Dialog DLG_LOBBY -Button BTN_OK `
                            -CapturedObservation $observation
                        [void]$startedButtons.Add($lane.Pending)
                        $lane.Phase = 'lobby-ok'
                    }
                } elseif ($lane.Phase -eq 'lobby-ok' -and
                          $lane.Pending.Task.IsCompleted) {
                    $lane.LobbyOk = Complete-LiteralButtonRequest $lane.Pending
                    $lane.Pending = $null
                    $lane.PopupArmUtc =
                        $lane.LobbyOk.CompletedUtc.AddMilliseconds(500)
                    Wait-FixedUtcAnchor $lane.PopupArmUtc
                    $lane.PopupService =
                        New-LiteralStartupPopupService join $ClientLog
                    $lane.PopupWindow = New-LiteralStartupPopupWindow `
                        join 20000 $lane.PopupService
                    # The first source AutoDismiss(20000) is a complete blocking
                    # join-thread step. Its native subscriber remains persistent,
                    # while this finite projection must reach its actual cap or
                    # claim+10-second terminal edge before the lane can return.
                    Wait-LiteralStartupPopupWindow $lane.PopupWindow
                    if (-not [bool]$lane.PopupWindow.Done) {
                        throw 'join first 20-second popup window returned incomplete'
                    }
                    $lane.Phase = 'done'
                }
            }
            if ($lane.Phase -ne 'done') {
                Start-Sleep -Milliseconds 100
            }
        }
        if ($lane.Phase -ne 'done') {
            throw "join independent navigation timed out in phase $($lane.Phase)"
        }
        if (-not $lane.StartupState.BriefingLatch -or
            -not $lane.StartupState.StrategicLatch -or
            -not $lane.StartupState.Witness) {
            throw 'join lane completed without both independent startup latches'
        }
        return [pscustomobject]@{
            Role = 'join'
            OwnedProcess = $OwnedProcess
            LaneStartedUtc = $laneStartedUtc
            DeadlineUtc = $deadlineUtc
            MainMenu = $lane.MainMenu
            Continue = $lane.Continue
            Search = $lane.Search
            JoinGameArmUtc = [DateTime]$lane.JoinGameArmUtc
            JoinGame = $lane.JoinGame
            StartupTurnWatermark = [long]$lane.StartupState.After
            SessionListReadyMarker = $sessionListReadyMarker
            SessionListReadyBefore = [int]$sessionListReadyBefore
            StartupWitness = $lane.StartupState.Witness
            BriefingLatch = $lane.StartupState.BriefingLatch
            StrategicLatch = $lane.StartupState.StrategicLatch
            HostBriefingArmedUtc = [DateTime]$lane.HostBriefingArmedUtc
            HostBriefingReleasedUtc = [DateTime]$lane.HostBriefingReleasedUtc
            HostStrategicArmedUtc = [DateTime]$lane.HostStrategicArmedUtc
            HostStrategicReleasedUtc = [DateTime]$lane.HostStrategicReleasedUtc
            FirstSettleArmUtc = [DateTime]$lane.FirstSettleArmUtc
            SecondSettleArmUtc = [DateTime]$lane.SecondSettleArmUtc
            LobbyOk = $lane.LobbyOk
            PopupArmUtc = [DateTime]$lane.PopupArmUtc
            PopupService = $lane.PopupService
            PopupWindow = $lane.PopupWindow
        }
    } catch {
        $cause = $_.Exception.Message
        foreach ($selection in $startedSelections) {
            if (-not [bool]$selection.Completed) {
                try { [void](Complete-LiteralSelectionRequest $selection) }
                catch { $cause += "; pending selection completion failed: $($_.Exception.Message)" }
            }
        }
        foreach ($pending in $startedButtons) {
            if (-not [bool]$pending.Completed) {
                try { [void](Complete-LiteralButtonRequest $pending) }
                catch { $cause += "; pending button completion failed: $($_.Exception.Message)" }
            }
        }
        if ($lane.StartupRequest -and
            -not [bool]$lane.StartupRequest.Completed) {
            try { [void](Complete-LiteralTurnHistoryRequest $lane.StartupRequest) }
            catch { $cause += "; startup subscription completion failed: $($_.Exception.Message)" }
        }
        throw $cause
    }
}

function Start-LiteralNavigationWorker {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][ValidateNotNull()][object]$StartupObserver,
        [Parameter(Mandatory)][System.Diagnostics.Process]$OwnedProcess,
        [Parameter(Mandatory)][DateTime]$LaunchUtc,
        [Parameter(Mandatory)][string]$ClientLog,
        [Parameter(Mandatory)][int]$TimeoutSec,
        [Parameter(Mandatory)]
        [System.Threading.ManualResetEventSlim]$StartGate,
        [string]$WaitPeerMarker = '',
        [long]$ExpectedInitialHostHandle = 0,
        [string]$ExactScenarioPath = '',
        [int]$ExactScenarioIndex = 0,
        [int]$ScenarioIndex = 0
    )
    # Each lane gets a separate physical runspace and PowerShell worker. A slow
    # Invoke-RestMethod/Get-Content in one role therefore cannot consume even
    # one tick of the other role's old self_nav_thread.
    $functionNames = @(
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
        'Assert-ProductionRelayHealthy',
        'Assert-LiteralInnerStartupObserverHealthy',
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
        'Invoke-LiteralHostNavigationLane',
        'Invoke-LiteralJoinNavigationLane'
    )
    $initialState =
        [System.Management.Automation.Runspaces.InitialSessionState]::CreateDefault2()
    foreach ($functionName in $functionNames) {
        $command = Get-Command $functionName -CommandType Function -ErrorAction Stop
        $entry = [System.Management.Automation.Runspaces.SessionStateFunctionEntry]::new(
            $functionName, [string]$command.Definition)
        $initialState.Commands.Add($entry)
    }
    foreach ($entry in @(
        @{ Name = 'RelayBase'; Value = [string]$script:RelayBase },
        @{ Name = 'LiteralInnerStartupObserver'; Value = $StartupObserver },
        @{ Name = 'ProductionRelayProcess'; Value = $script:ProductionRelayProcess },
        @{ Name = 'SimRelayErrorLog'; Value = [string]$script:SimRelayErrorLog },
        @{ Name = 'SimRelayLog'; Value = [string]$script:SimRelayLog },
        @{ Name = 'GameDir'; Value = [string]$GameDir },
        @{ Name = 'ClientLogInitialLengths'; Value = $script:ClientLogInitialLengths },
        @{ Name = 'ClientLogOwnedProcessIds'; Value = $script:ClientLogOwnedProcessIds },
        @{ Name = 'StartupModalSettleMilliseconds'; Value =
            [int]$script:StartupModalSettleMilliseconds },
        @{ Name = 'LastStartupPopupEvidenceTick'; Value =
            $script:LastStartupPopupEvidenceTick },
        @{ Name = 'LastStartupPopupEvidenceUtc'; Value =
            $script:LastStartupPopupEvidenceUtc }
    )) {
        $variable =
            [System.Management.Automation.Runspaces.SessionStateVariableEntry]::new(
                [string]$entry.Name, $entry.Value,
                "literal $Role navigation worker input")
        $initialState.Variables.Add($variable)
    }

    $runspace =
        [System.Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace(
            $initialState)
    $powerShell = [PowerShell]::Create()
    try {
        $runspace.Open()
        $powerShell.Runspace = $runspace
        $laneCommand = if ($Role -eq 'host') {
            'Invoke-LiteralHostNavigationLane'
        } else {
            'Invoke-LiteralJoinNavigationLane'
        }
        [void]$powerShell.AddCommand($laneCommand)
        [void]$powerShell.AddParameter('OwnedProcess', $OwnedProcess)
        [void]$powerShell.AddParameter('LaunchUtc', $LaunchUtc)
        [void]$powerShell.AddParameter('ClientLog', $ClientLog)
        [void]$powerShell.AddParameter('TimeoutSec', $TimeoutSec)
        [void]$powerShell.AddParameter('StartGate', $StartGate)
        if ($Role -eq 'host') {
            [void]$powerShell.AddParameter('WaitPeerMarker', $WaitPeerMarker)
            [void]$powerShell.AddParameter('ExactScenarioPath', $ExactScenarioPath)
            [void]$powerShell.AddParameter('ExactScenarioIndex', $ExactScenarioIndex)
            [void]$powerShell.AddParameter('ScenarioIndex', $ScenarioIndex)
        } else {
            [void]$powerShell.AddParameter(
                'ExpectedInitialHostHandle', $ExpectedInitialHostHandle)
        }
        $async = $powerShell.BeginInvoke()
        return [pscustomobject]@{
            Role = $Role
            OwnedProcess = $OwnedProcess
            PowerShell = $powerShell
            Runspace = $runspace
            Async = $async
            Consumed = $false
        }
    } catch {
        $powerShell.Dispose()
        $runspace.Dispose()
        throw
    }
}

function Complete-LiteralNavigationWorker([object]$Worker) {
    if (-not $Worker -or [bool]$Worker.Consumed) {
        throw 'literal navigation worker is missing or already consumed'
    }
    $Worker.Consumed = $true
    try {
        try {
            $output = @($Worker.PowerShell.EndInvoke($Worker.Async))
        } catch {
            $cause = $_.Exception.Message
            $passiveDiagnostics = @($Worker.PowerShell.Streams.Warning |
                ForEach-Object { [string]$_.Message } |
                Where-Object {
                    $_.StartsWith('literal navigation passive GET timed out:',
                        [StringComparison]::Ordinal)
                })
            if ($passiveDiagnostics.Count -ne 0) {
                $cause += '; passive navigation diagnostics: ' +
                    ($passiveDiagnostics -join ' | ')
            }
            throw $cause
        }
        $passiveDiagnostics = @($Worker.PowerShell.Streams.Warning |
            ForEach-Object { [string]$_.Message } |
            Where-Object {
                $_.StartsWith('literal navigation passive GET timed out:',
                    [StringComparison]::Ordinal)
            })
        if ($Worker.PowerShell.Streams.Error.Count -ne 0) {
            $details = @($Worker.PowerShell.Streams.Error |
                ForEach-Object { $_.Exception.Message }) -join '; '
            if ($passiveDiagnostics.Count -ne 0) {
                $details += '; passive navigation diagnostics: ' +
                    ($passiveDiagnostics -join ' | ')
            }
            throw "$($Worker.Role) physical navigation worker failed: $details"
        }
        if ($output.Count -ne 1) {
            throw "$($Worker.Role) physical navigation worker returned $($output.Count) results"
        }
        $result = $output[0]
        if ([string]$result.Role -ne [string]$Worker.Role -or
            -not [object]::ReferenceEquals(
                $result.OwnedProcess, $Worker.OwnedProcess)) {
            throw "$($Worker.Role) worker changed its exact owned Process identity"
        }
        $result | Add-Member -NotePropertyName PassiveReadDiagnostics `
            -NotePropertyValue ([string[]]$passiveDiagnostics)
        return $result
    } finally {
        $Worker.PowerShell.Dispose()
        $Worker.Runspace.Dispose()
    }
}

function Complete-LiteralIndependentNavigation {
    param(
        [Parameter(Mandatory)][ValidateNotNull()][object]$StartupObserver,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [Parameter(Mandatory)][DateTime]$HostLaunchUtc,
        [Parameter(Mandatory)][DateTime]$JoinLaunchUtc,
        [Parameter(Mandatory)][string]$HostLog,
        [Parameter(Mandatory)][string]$JoinLog,
        [Parameter(Mandatory)][string]$WaitPeerMarker,
        [long]$ExpectedInitialHostHandle = 0,
        [string]$ExactScenarioPath = '',
        [int]$ExactScenarioIndex = 0,
        [int]$ScenarioIndex = 0,
        [int]$TimeoutSec = 180
    )
    if ([object]::ReferenceEquals($HostProcess, $JoinProcess) -or
        [long]$HostProcess.Id -eq [long]$JoinProcess.Id) {
        throw 'physical host/join navigation requires two distinct owned Process objects'
    }
    # Both runspaces are constructed and BeginInvoke'd behind one closed gate.
    # Opening it only after both descriptors exist removes construction-order
    # skew; each released lane then creates its own full timeout budget.
    $startGate = [System.Threading.ManualResetEventSlim]::new($false)
    $hostWorker = $null
    $joinWorker = $null
    try {
        $hostWorker = Start-LiteralNavigationWorker `
            -Role host -StartupObserver $StartupObserver `
            -OwnedProcess $HostProcess `
            -LaunchUtc $HostLaunchUtc -ClientLog $HostLog `
            -TimeoutSec $TimeoutSec -StartGate $startGate `
            -WaitPeerMarker $WaitPeerMarker `
            -ExactScenarioPath $ExactScenarioPath `
            -ExactScenarioIndex $ExactScenarioIndex `
            -ScenarioIndex $ScenarioIndex
        $joinWorker = Start-LiteralNavigationWorker `
            -Role join -StartupObserver $StartupObserver `
            -OwnedProcess $JoinProcess `
            -LaunchUtc $JoinLaunchUtc -ClientLog $JoinLog `
            -TimeoutSec $TimeoutSec -StartGate $startGate `
            -ExpectedInitialHostHandle $ExpectedInitialHostHandle
    } catch {
        $startFailure = $_.Exception.Message
        $startGate.Set()
        foreach ($startedWorker in @($hostWorker, $joinWorker)) {
            if ($startedWorker) {
                try { [void](Complete-LiteralNavigationWorker $startedWorker) }
                catch {
                    $startFailure +=
                        "; $($startedWorker.Role) worker completion failed: $($_.Exception.Message)"
                }
            }
        }
        $startGate.Dispose()
        throw $startFailure
    }
    $startGate.Set()

    $hostResult = $null
    $joinResult = $null
    $failures = [System.Collections.Generic.List[string]]::new()
    foreach ($worker in @($hostWorker, $joinWorker)) {
        try {
            $result = Complete-LiteralNavigationWorker $worker
            if ([string]$worker.Role -eq 'host') {
                $hostResult = $result
            } else {
                $joinResult = $result
            }
        } catch {
            $failures.Add("$($worker.Role): $($_.Exception.Message)")
        }
    }
    $startGate.Dispose()
    if ($failures.Count -ne 0) {
        throw ("independent physical navigation failed: " +
            (@($failures) -join ' | '))
    }

    return [pscustomobject]@{
        HostLaneStartedUtc = [DateTime]$hostResult.LaneStartedUtc
        HostDeadlineUtc = [DateTime]$hostResult.DeadlineUtc
        HostMainMenu = $hostResult.MainMenu
        HostContinue = $hostResult.Continue
        HostCreate = $hostResult.Create
        HostLoad = $hostResult.Load
        HostWaitPeer = $hostResult.WaitPeer
        HostWaitPeerBefore = [int]$hostResult.WaitPeerBefore
        HostWaitPeerReleaseUtc = [DateTime]$hostResult.WaitPeerReleaseUtc
        HostLobbyOk = $hostResult.LobbyOk
        HostPopupArmUtc = [DateTime]$hostResult.PopupArmUtc
        HostPopupService = $hostResult.PopupService
        HostPopupWindow = $hostResult.PopupWindow
        JoinLaneStartedUtc = [DateTime]$joinResult.LaneStartedUtc
        JoinDeadlineUtc = [DateTime]$joinResult.DeadlineUtc
        JoinMainMenu = $joinResult.MainMenu
        JoinContinue = $joinResult.Continue
        JoinSearch = $joinResult.Search
        JoinGameArmUtc = [DateTime]$joinResult.JoinGameArmUtc
        JoinGame = $joinResult.JoinGame
        StartupTurnWatermark = [long]$joinResult.StartupTurnWatermark
        SessionListReadyMarker = [string]$joinResult.SessionListReadyMarker
        SessionListReadyBefore = [int]$joinResult.SessionListReadyBefore
        StartupWitness = $joinResult.StartupWitness
        JoinBriefingLatch = $joinResult.BriefingLatch
        JoinStrategicLatch = $joinResult.StrategicLatch
        JoinHostBriefingArmedUtc = [DateTime]$joinResult.HostBriefingArmedUtc
        JoinHostBriefingReleasedUtc = [DateTime]$joinResult.HostBriefingReleasedUtc
        JoinHostStrategicArmedUtc = [DateTime]$joinResult.HostStrategicArmedUtc
        JoinHostStrategicReleasedUtc = [DateTime]$joinResult.HostStrategicReleasedUtc
        JoinFirstSettleArmUtc = [DateTime]$joinResult.FirstSettleArmUtc
        JoinSecondSettleArmUtc = [DateTime]$joinResult.SecondSettleArmUtc
        JoinLobbyOk = $joinResult.LobbyOk
        JoinPopupArmUtc = [DateTime]$joinResult.PopupArmUtc
        JoinPopupService = $joinResult.PopupService
        JoinPopupWindow = $joinResult.PopupWindow
        PassiveReadDiagnostics = @(
            @($hostResult.PassiveReadDiagnostics) +
            @($joinResult.PassiveReadDiagnostics)
        )
    }
}

function Wait-LiteralSelectionOrUtcAnchor([object]$Pending,
                                          [DateTime]$TargetUtc) {
    $remainingMilliseconds = [Math]::Ceiling(
        ($TargetUtc - [DateTime]::UtcNow).TotalMilliseconds)
    if ($remainingMilliseconds -le 0) { return [bool]$Pending.Task.IsCompleted }
    try {
        # Event-driven wait: either the one already-submitted native command
        # completes or the other process reaches its own fixed launch clock.
        return [bool]$Pending.Task.Wait([int]$remainingMilliseconds)
    } catch {
        [void](Complete-LiteralSelectionRequest $Pending)
        throw
    }
}

function Start-LiteralTurnHistoryRequest {
    param(
        [Parameter(Mandatory)][long]$After,
        [Parameter(Mandatory)][ValidateSet('join')][string]$Role,
        [Parameter(Mandatory)][int]$WaitMilliseconds
    )
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "literal turn subscription watermark is outside uint32: $After"
    }
    if ($WaitMilliseconds -lt 1 -or $WaitMilliseconds -gt 120000) {
        throw 'literal turn subscription wait is outside 1..120000 ms'
    }
    $uri = ('{0}/api/turn/history?after={1}&role={2}&waitMs={3}') -f `
        $script:RelayBase,
        $After,
        [uri]::EscapeDataString($Role),
        $WaitMilliseconds
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.UseProxy = $false
    $handler.AllowAutoRedirect = $false
    $client = [System.Net.Http.HttpClient]::new($handler, $true)
    # The append-only relay owns the long-poll deadline. Only the physical join
    # worker observes Task completion; it cannot block the host worker.
    $client.Timeout = [System.Threading.Timeout]::InfiniteTimeSpan
    $request = [System.Net.Http.HttpRequestMessage]::new(
        [System.Net.Http.HttpMethod]::Get, $uri)
    try {
        Assert-ProductionRelayHealthy
        $task = $client.SendAsync($request)
        return [pscustomobject]@{
            After = [long]$After
            Role = $Role
            WaitMilliseconds = $WaitMilliseconds
            Uri = $uri
            Client = $client
            Request = $request
            Task = $task
            Completed = $false
        }
    } catch {
        $request.Dispose()
        $client.Dispose()
        throw
    }
}

function Complete-LiteralTurnHistoryRequest([object]$Pending) {
    if ([bool]$Pending.Completed) {
        throw 'literal turn subscription completion was consumed twice'
    }
    $Pending.Completed = $true
    $httpResponse = $null
    try {
        $httpResponse = $Pending.Task.GetAwaiter().GetResult()
        $body = $httpResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if (-not $httpResponse.IsSuccessStatusCode) {
            throw ("HTTP {0} ({1}) from {2}: {3}" -f
                [int]$httpResponse.StatusCode, $httpResponse.ReasonPhrase,
                $Pending.Uri, $body)
        }
        $history = $body | ConvertFrom-Json
        $terminalFault = Get-OptionalProperty $history 'terminalFault'
        if ($terminalFault) {
            throw "test relay terminal fault: $([string]$terminalFault.reason)"
        }
        return $history
    } finally {
        if ($httpResponse) { $httpResponse.Dispose() }
        $Pending.Request.Dispose()
        $Pending.Client.Dispose()
    }
}

function Add-LiteralJoinStockStartupHistory([object]$State,
                                             [object]$History) {
    if ([bool](Get-OptionalProperty $History 'timedOut')) {
        throw ("timed out waiting for independent join stock CJoinGame/BeginTurn " +
            "latches after $([long]$State.After)")
    }
    $newEvents = @((Get-OptionalProperty $History 'events'))
    if ($newEvents.Count -eq 0) {
        throw 'turn evidence subscription returned without an event or typed timeout'
    }
    [long]$latestSequence = Get-EvidenceWatermark `
        $History 'initial join stock startup subscription'
    if ($latestSequence -le [long]$State.Cursor) {
        throw ("turn evidence subscription did not advance its cursor " +
            "(cursor=$([long]$State.Cursor) latest=$latestSequence)")
    }
    [long]$previousNewSequence = [long]$State.Cursor
    foreach ($newEvent in $newEvents) {
        [long]$newSequence = Get-RequiredTelemetryNumber `
            $newEvent 'seq' 'initial join stock startup subscription event'
        if ($newSequence -le $previousNewSequence -or
            $newSequence -gt $latestSequence) {
            throw ("turn evidence event is not a strict append-only sequence " +
                "(previous=$previousNewSequence event=$newSequence latest=$latestSequence)")
        }
        $previousNewSequence = $newSequence
    }
    $State.Cursor = $latestSequence
    $State.ObservedEvents = @($State.ObservedEvents) + $newEvents

    $unexpected = @($State.ObservedEvents | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -notin @(
            'stock-startup-begin-turn-observed',
            'stock-startup-join-game-observed')
    })
    if ($unexpected.Count -gt 0) {
        $unexpectedKinds = @($unexpected | ForEach-Object {
            [string](Get-OptionalProperty $_ 'kind')
        }) -join ', '
        throw ("unexpected join startup evidence after watermark " +
            "$([long]$State.After): $unexpectedKinds")
    }
    $beginEvents = @($State.ObservedEvents | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -eq
            'stock-startup-begin-turn-observed'
    })
    $joinGameEvents = @($State.ObservedEvents | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -eq
            'stock-startup-join-game-observed'
    })
    if ($beginEvents.Count -gt 1 -or $joinGameEvents.Count -gt 1) {
        throw ("join startup published duplicate typed evidence after watermark " +
            "$([long]$State.After) (begin=$($beginEvents.Count) " +
            "joinGame=$($joinGameEvents.Count))")
    }

    # Strategic and briefing are independent append-only predicates. Validate
    # and latch each one as soon as its own typed event exists; never require the
    # other event or a particular wire-arrival order to release this predicate.
    if ($beginEvents.Count -eq 1 -and -not $State.StrategicLatch) {
        $beginEvent = $beginEvents[0]
        $beginRole = [string](Get-OptionalProperty $beginEvent 'role')
        [long]$beginSequence = Get-RequiredTelemetryNumber `
            $beginEvent 'seq' 'initial join stock BeginTurn'
        [long]$senderDpid = Get-RequiredTelemetryNumber `
            $beginEvent 'senderDpid' 'initial join stock BeginTurn'
        [long]$receiverDpid = Get-RequiredTelemetryNumber `
            $beginEvent 'receiverDpid' 'initial join stock BeginTurn'
        [long]$frameLength = Get-RequiredTelemetryNumber `
            $beginEvent 'frameLength' 'initial join stock BeginTurn'
        [long]$addressee = Get-RequiredTelemetryNumber `
            $beginEvent 'addressee' 'initial join stock BeginTurn'
        [long]$commandSequence = Get-RequiredTelemetryNumber `
            $beginEvent 'commandSequence' 'initial join stock BeginTurn'
        [long]$activeHandle = Get-RequiredTelemetryNumber `
            $beginEvent 'activeHandle' 'initial join stock BeginTurn'
        $receiverIsDynamic = (
            $receiverDpid -gt 1 -and
            $receiverDpid -ne 0x00ffffff -and
            $receiverDpid -ne [uint32]::MaxValue)
        $activeMatches = (
            $activeHandle -gt 0 -and
            ([long]$State.ExpectedActiveHandle -eq 0 -or
             $activeHandle -eq [long]$State.ExpectedActiveHandle))
        if ($beginRole -ne 'join' -or
            $beginSequence -le [long]$State.After -or
            $beginSequence -gt $latestSequence -or
            $senderDpid -ne 1 -or -not $receiverIsDynamic -or
            $frameLength -ne 56 -or $addressee -ne 0 -or
            $commandSequence -ne 1 -or
            -not $activeMatches) {
            throw ("initial join stock BeginTurn has wrong independent latch layout " +
                "(role=$beginRole seq=$beginSequence after=$([long]$State.After) " +
                "latest=$latestSequence sender=$senderDpid receiver=$receiverDpid " +
                "frame=$frameLength addressee=$addressee commandSequence=$commandSequence " +
                "active=0x$('{0:x8}' -f $activeHandle) " +
                "expectedActive=0x$('{0:x8}' -f ([long]$State.ExpectedActiveHandle)))")
        }
        $State.StrategicLatch = $beginEvent
    }

    if ($joinGameEvents.Count -eq 1 -and -not $State.BriefingLatch) {
        $joinGameEvent = $joinGameEvents[0]
        $joinGameRole = [string](Get-OptionalProperty $joinGameEvent 'role')
        [long]$joinGameSequence = Get-RequiredTelemetryNumber `
            $joinGameEvent 'seq' 'initial join stock CJoinGame'
        [long]$joinSenderDpid = Get-RequiredTelemetryNumber `
            $joinGameEvent 'senderDpid' 'initial join stock CJoinGame'
        [long]$joinReceiverDpid = Get-RequiredTelemetryNumber `
            $joinGameEvent 'receiverDpid' 'initial join stock CJoinGame'
        [long]$joinFrameLength = Get-RequiredTelemetryNumber `
            $joinGameEvent 'frameLength' 'initial join stock CJoinGame'
        [long]$joinedHandle = Get-RequiredTelemetryNumber `
            $joinGameEvent 'joinedHandle' 'initial join stock CJoinGame'
        [long]$encodedNameLength = Get-RequiredTelemetryNumber `
            $joinGameEvent 'nameLength' 'initial join stock CJoinGame'
        [long]$raceCategoryId = Get-RequiredTelemetryNumber `
            $joinGameEvent 'raceCategoryId' 'initial join stock CJoinGame'
        [long]$expectedJoinFrameLength = 56 + $encodedNameLength
        $joinReceiverIsDynamic = (
            $joinReceiverDpid -gt 1 -and
            $joinReceiverDpid -ne 0x00ffffff -and
            $joinReceiverDpid -ne [uint32]::MaxValue)
        $joinedHandleMatches = (
            $joinedHandle -gt 0 -and
            ([long]$State.ExpectedActiveHandle -eq 0 -or
             $joinedHandle -eq [long]$State.ExpectedActiveHandle))
        if ($joinGameRole -ne 'join' -or
            $joinGameSequence -le [long]$State.After -or
            $joinGameSequence -gt $latestSequence -or
            $joinSenderDpid -ne 1 -or -not $joinReceiverIsDynamic -or
            $encodedNameLength -lt 1 -or
            $joinFrameLength -ne $expectedJoinFrameLength -or
            -not $joinedHandleMatches) {
            throw ("initial join stock CJoinGame has wrong independent latch layout " +
                "(role=$joinGameRole seq=$joinGameSequence " +
                "after=$([long]$State.After) latest=$latestSequence " +
                "sender=$joinSenderDpid receiver=$joinReceiverDpid " +
                "frame=$joinFrameLength nameLength=$encodedNameLength " +
                "raceCategory=$raceCategoryId " +
                "joined=0x$('{0:x8}' -f $joinedHandle) " +
                "expectedJoined=0x$('{0:x8}' -f ([long]$State.ExpectedActiveHandle)))")
        }
        $State.BriefingLatch = $joinGameEvent
    }

    if ($State.StrategicLatch -and $State.BriefingLatch) {
        $beginEvent = $State.StrategicLatch
        $joinGameEvent = $State.BriefingLatch
        [long]$receiverDpid = Get-RequiredTelemetryNumber `
            $beginEvent 'receiverDpid' 'latched join stock BeginTurn'
        [long]$activeHandle = Get-RequiredTelemetryNumber `
            $beginEvent 'activeHandle' 'latched join stock BeginTurn'
        [long]$joinReceiverDpid = Get-RequiredTelemetryNumber `
            $joinGameEvent 'receiverDpid' 'latched join stock CJoinGame'
        [long]$joinedHandle = Get-RequiredTelemetryNumber `
            $joinGameEvent 'joinedHandle' 'latched join stock CJoinGame'
        if ($joinReceiverDpid -ne $receiverDpid -or
            $joinedHandle -ne $activeHandle) {
            throw ("independent join startup latches disagree " +
                "(beginReceiver=$receiverDpid joinReceiver=$joinReceiverDpid " +
                "active=0x$('{0:x8}' -f $activeHandle) " +
                "joined=0x$('{0:x8}' -f $joinedHandle))")
        }
        $State.Witness = [pscustomobject]@{
            BeginTurn = $beginEvent
            JoinGame = $joinGameEvent
            LatestSequence = [long]$State.Cursor
        }
    }
    return $State.Witness
}

function Wait-FixedUtcAnchor([DateTime]$TargetUtc) {
    # Timer completion may wake a fraction early; this loop only completes the
    # same fixed delay. It never polls game state or authorizes another action.
    while ([DateTime]::UtcNow -lt $TargetUtc) {
        $remainingMilliseconds = [Math]::Ceiling(
            ($TargetUtc - [DateTime]::UtcNow).TotalMilliseconds)
        if ($remainingMilliseconds -gt 0) {
            Start-Sleep -Milliseconds $remainingMilliseconds
        }
    }
}

function Get-LiteralMainMenuAtLaunchAnchor(
    [string]$Role,
    [System.Diagnostics.Process]$Process,
    [DateTime]$LaunchUtc,
    [DateTime]$DeadlineUtc) {
    # The green launcher resumed Discipl2, waited its fixed 1500 ms preloader
    # window, injected lobbyhook.dll, and only then self_nav_thread slept
    # 11000 ms. MSS is loaded at process start, so retain both old clocks
    # explicitly instead of collapsing the nav anchor to Process.Start+11000.
    $legacyInjectionUtc = $LaunchUtc.AddMilliseconds(1500)
    $targetUtc = $legacyInjectionUtc.AddMilliseconds(11000)
    Wait-FixedUtcAnchor $targetUtc

    # The old native driver could not execute its already-armed first step until
    # WndProc published a matching ready dialog. Preserve the fixed clock as a
    # not-before boundary, then observe publications until that same condition
    # exists. This loop sends no command and cannot re-arm or repeat BTN_MULTI.
    while ([DateTime]::UtcNow -lt $DeadlineUtc) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "$Role exited while awaiting DLG_MAIN_MENU after its literal delayed-injection+11000 ms anchor"
        }
        $roleState = Get-LiteralNavigationRoleState `
            -Role $Role -Context "$Role/main-menu-anchor" `
            -DeadlineUtc $DeadlineUtc
        if ($roleState) {
            Assert-DebugRelayClientIdentity $Role $roleState $Process
            $dialogValue = Get-OptionalProperty $roleState 'dialog'
            $instanceValue = Get-OptionalProperty $roleState 'dialogInstance'
            $appearanceValue = Get-OptionalProperty $roleState 'dialogAppearance'
            $readyValue = Get-OptionalProperty $roleState 'dialogReady'
            $isHelloBeforeFirstUiPublication =
                $null -eq $dialogValue -and
                $null -ne $instanceValue -and [long]$instanceValue -eq 0 -and
                $null -ne $appearanceValue -and [long]$appearanceValue -eq 0 -and
                $readyValue -is [bool] -and -not [bool]$readyValue
            if ($isHelloBeforeFirstUiPublication) {
                Start-Sleep -Milliseconds 100
                continue
            }
            $observation = ConvertTo-LiteralNavigationDialogObservation `
                $Role $roleState
            if ($observation.Ready -and $observation.Dialog -eq 'DLG_MAIN_MENU') {
                return $observation
            }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "$Role DLG_MAIN_MENU was not published ready before its navigation deadline"
}

function Invoke-OneShotTransition([string]$Role,
                                  [string]$Dialog,
                                  [string]$Button,
                                  [string]$ToDialog,
                                  [int]$TimeoutSec = 45,
                                  [int]$SettleMilliseconds = 0,
                                  [int]$CommandTimeoutMilliseconds = 90000,
                                  [object]$CapturedObservation = $null) {
    $readyDeadline = (Get-Date).AddSeconds($TimeoutSec)
    if ($null -ne $CapturedObservation) {
        $candidate = Get-DialogObservation $Role
        if (-not $candidate -or -not $candidate.Ready -or
            $candidate.Dialog -ne $Dialog -or
            $candidate.Instance -ne [long]$CapturedObservation.Instance) { return $false }
        $selectionListBox = Get-OptionalProperty $CapturedObservation 'SelectionListBox'
        if (-not [string]::IsNullOrEmpty([string]$selectionListBox)) {
            $capturedTarget = Get-ReadyActionTarget `
                $CapturedObservation $Dialog
            [long]$capturedOwner = Get-OptionalProperty $capturedTarget 'instance'
            # The native selection command has already completed ten read-backs
            # on ten natural UI frames. The relay's sticky snapshot may still
            # contain the pre-selection row, so a second listbox read here would
            # add a non-legacy checkpoint. Preserve only the exact owner/appearance
            # identity needed by the immediately following one-shot button action.
            [void](Get-ReadyActionTarget $candidate $Dialog $capturedOwner)
        }
        $observation = $candidate
    } else {
        $observation = $null
        while ((Get-Date) -lt $readyDeadline) {
            $candidate = Get-DialogObservation $Role
            if ($candidate -and $candidate.Ready -and $candidate.Dialog -eq $Dialog) {
                $observation = $candidate
                break
            }
            Start-Sleep -Milliseconds 250
        }
    }
    if (-not $observation) { return $false }
    $target = Get-ReadyActionTarget $observation $Dialog
    [long]$targetInstance = Get-OptionalProperty $target 'instance'
    if ($SettleMilliseconds -gt 0) {
        Start-Sleep -Milliseconds $SettleMilliseconds
        $confirmed = Get-DialogObservation $Role
        if (-not $confirmed -or -not $confirmed.Ready -or
            $confirmed.Dialog -ne $Dialog -or
            $confirmed.Instance -ne $observation.Instance) { return $false }
        [void](Get-ReadyActionTarget $confirmed $Dialog $targetInstance)
        $observation = $confirmed
    }
    [long]$buttonOwner = Assert-ReadyButtonSnapshot $observation $Button $Dialog
    if ($buttonOwner -ne $targetInstance) { return $false }
    Assert-ProductionRelayHealthy
    if (-not (Invoke-Button $Role $Dialog $Button `
                            $targetInstance $observation.Instance `
                            $CommandTimeoutMilliseconds)) { return $false }
    # The target wait and the causal-effect wait are independent. The command
    # is still dispatched once; a synchronous stock callback cannot consume
    # the observation budget for its own downstream dialog.
    $effectDeadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $effectDeadline) {
        if (Test-DialogReady $Role $ToDialog) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Wait-StableReadyButtonObservation([string]$Role,
                                           [string]$Dialog,
                                           [string]$Button,
                                           [datetime]$Deadline,
                                           [int]$StableMilliseconds) {
    if ($StableMilliseconds -le 0) {
        throw 'stable ready-button window must be positive'
    }
    [long]$observedAppearance = -1
    [long]$observedOwner = -1
    $stableSince = $null
    while ((Get-Date) -lt $Deadline) {
        Assert-ProductionRelayHealthy
        $candidate = Get-DialogObservation $Role
        if (-not $candidate -or -not $candidate.Ready -or
            $candidate.Dialog -ne $Dialog) {
            $stableSince = $null
            Start-Sleep -Milliseconds 100
            continue
        }
        $target = Get-ReadyActionTarget $candidate $Dialog
        [long]$owner = Get-OptionalProperty $target 'instance'
        [long]$appearance = $candidate.Instance
        if ($appearance -ne $observedAppearance -or $owner -ne $observedOwner) {
            # A roster/control rebind cancels the unsubmitted intention. The new
            # native appearance must earn its own full stability window.
            $observedAppearance = $appearance
            $observedOwner = $owner
            $stableSince = Get-Date
        } elseif ($null -ne $stableSince -and
                  ((Get-Date) - $stableSince).TotalMilliseconds -ge $StableMilliseconds) {
            $confirmed = Get-DialogObservation $Role
            if (-not $confirmed -or -not $confirmed.Ready -or
                $confirmed.Dialog -ne $Dialog -or
                [long]$confirmed.Instance -ne $observedAppearance) {
                $stableSince = $null
                continue
            }
            [long]$buttonOwner = Assert-ReadyButtonSnapshot $confirmed $Button $Dialog
            if ($buttonOwner -ne $observedOwner) {
                throw "$Role $Dialog::$Button owner changed inside its stable appearance"
            }
            return [pscustomobject]@{
                Observation = $confirmed
                Owner = $buttonOwner
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Invoke-OneShotButton([string]$Role,
                              [string]$Dialog,
                              [string]$Button,
                              [int]$TimeoutSec,
                              [int]$SettleMilliseconds = 0,
                              [int]$CommandTimeoutMilliseconds = 0,
                              [object]$CapturedObservation = $null) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    if ($null -ne $CapturedObservation) {
        $observation = Get-DialogObservation $Role
        if (-not $observation -or -not $observation.Ready -or
            $observation.Dialog -ne $Dialog -or
            [long]$observation.Instance -ne [long]$CapturedObservation.Instance) {
            return $false
        }
        $target = Get-ReadyActionTarget $observation $Dialog
        [long]$targetInstance = Get-OptionalProperty $target 'instance'
        [long]$buttonOwner = Assert-ReadyButtonSnapshot $observation $Button $Dialog
        if ($buttonOwner -ne $targetInstance) { return $false }
    } elseif ($SettleMilliseconds -gt 0) {
        $stable = Wait-StableReadyButtonObservation `
            $Role $Dialog $Button $deadline $SettleMilliseconds
        if (-not $stable) { return $false }
        $observation = $stable.Observation
        [long]$targetInstance = $stable.Owner
    } else {
        $observation = $null
        while ((Get-Date) -lt $deadline) {
            $candidate = Get-DialogObservation $Role
            if ($candidate -and $candidate.Ready -and $candidate.Dialog -eq $Dialog) {
                $observation = $candidate
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $observation) { return $false }
        $target = Get-ReadyActionTarget $observation $Dialog
        [long]$targetInstance = Get-OptionalProperty $target 'instance'
        [long]$buttonOwner = Assert-ReadyButtonSnapshot $observation $Button $Dialog
        if ($buttonOwner -ne $targetInstance) { return $false }
    }
    Assert-ProductionRelayHealthy
    if (-not (Invoke-Button $Role $Dialog $Button `
                            $targetInstance $observation.Instance `
                            $CommandTimeoutMilliseconds)) { return $false }
    # The typed command result proves that this captured appearance/owner was
    # addressed once. Its causal effect is deliberately observed by a separate
    # event/departure gate chosen by the caller.
    return [pscustomobject]@{
        Role = $Role
        Dialog = $Dialog
        Button = $Button
        Appearance = [long]$observation.Instance
        Owner = [long]$targetInstance
    }
}

function Wait-CapturedDialogDeparture([object]$Action,
                                      [int]$TimeoutSec) {
    $role = [string](Get-OptionalProperty $Action 'Role')
    $dialog = [string](Get-OptionalProperty $Action 'Dialog')
    [long]$appearance = Get-OptionalProperty $Action 'Appearance'
    if ([string]::IsNullOrWhiteSpace($role) -or
        [string]::IsNullOrWhiteSpace($dialog) -or
        $appearance -lt 1 -or $appearance -gt [uint32]::MaxValue) {
        throw 'captured dialog action is missing its exact role/dialog/appearance identity'
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        Assert-ProductionRelayHealthy
        $observation = Get-DialogObservation $role
        if ($observation) {
            if ($observation.Dialog -ne $dialog -or
                [long]$observation.Instance -ne $appearance) {
                return $true
            }
            # Unbind publishes the same name/appearance with ready=false and
            # zero widgets before a successor screen is visible. That exact
            # native event is already retired; do not wait on its stale name.
            if (-not $observation.Ready) { return $true }
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function New-LiteralStartupPopupService([string]$Role,
                                        [string]$ClientLog) {
    if ($Role -notin @('host', 'join')) {
        throw "unsupported literal popup observer role '$Role'"
    }
    $fullLogPath = [IO.Path]::GetFullPath($ClientLog)
    # This proves that the observer is bound to this run's exact mss32_<PID>.log
    # before it is allowed to consume even one native marker.
    [void](Get-ClientLogBaseline $fullLogPath)
    return [pscustomobject]@{
        Role = $Role
        ClientLog = $fullLogPath
        SnapshotLines = @()
        MarkerLineCount = [long]0
        LastMarkerTick = [long]0
        LastObservedAppearance = [long]0
        Appearances = @{}
    }
}

function New-LiteralStartupPopupWindow([string]$Role,
                                       [int]$CapMilliseconds,
                                       [object]$PopupService = $null,
                                       [long]$ArmedTick = 0) {
    if ($CapMilliseconds -le 0) { throw 'literal popup cap must be positive' }
    if ($null -eq $PopupService) {
        throw 'literal popup window requires its exact-owned native marker observer'
    }
    if ([string]$PopupService.Role -ne $Role) {
        throw 'literal popup window/service role mismatch'
    }
    [long]$nowTick = [Environment]::TickCount64
    if ($ArmedTick -eq 0) { $ArmedTick = $nowTick }
    if ($ArmedTick -lt 1 -or $ArmedTick -gt $nowTick) {
        throw "literal popup armed tick is outside the current process lifetime: $ArmedTick/$nowTick"
    }
    return [pscustomobject]@{
        Role = $Role
        ArmedTick = $ArmedTick
        ArmedUtc = [DateTime]::UtcNow.AddMilliseconds($ArmedTick - $nowTick)
        CapMilliseconds = $CapMilliseconds
        FirstActionTick = [long]0
        PopupService = $PopupService
        Done = $false
        CompletedTick = [long]0
        CompletedUtc = $null
    }
}

function Invoke-LiteralPersistentStartupPopupTick([object]$PopupService) {
    # Keep the complete exact-owned PID-log snapshot from this already-required
    # observer read. The quiet observer's MSS witness reuses it; it must not open
    # either client log a second time at or after the quiet-3 boundary.
    $snapshotLines = @(Read-ClientLogLines ([string]$PopupService.ClientLog))
    $PopupService.SnapshotLines = $snapshotLines
    $eventLines = @($snapshotLines |
        Where-Object {
            $_ -match '\[testdrv\]\[scripted-popup\] (OBSERVED|CLAIMED|COMMITTED)\b'
        })
    if ($eventLines.Count -lt [long]$PopupService.MarkerLineCount) {
        throw "$($PopupService.Role) scripted-popup PID log lost already observed complete records"
    }
    if ($eventLines.Count -eq [long]$PopupService.MarkerLineCount) {
        return @()
    }

    $firstNew = [int][long]$PopupService.MarkerLineCount
    $newLines = @($eventLines[$firstNew..($eventLines.Count - 1)])
    $events = [Collections.Generic.List[object]]::new()
    $exactMarker =
        '\[testdrv\]\[scripted-popup\] ' +
        '(?<kind>OBSERVED|CLAIMED|COMMITTED) ' +
        'role=(?<role>host|join) ' +
        'dialog=(?<dialog>DLG_[A-Z0-9_]+) ' +
        'appearance=(?<appearance>[0-9]+) ' +
        'owner=(?<owner>[0-9]+) ' +
        'button=(?<button>BTN_[A-Z0-9_]+)' +
        '(?: bindAgeMs=(?<bindAgeMs>[0-9]+))? ' +
        'tick=(?<tick>[0-9]+)$'
    foreach ($line in $newLines) {
        if ($line -notmatch $exactMarker) {
            throw "$($PopupService.Role) malformed native scripted-popup record: $line"
        }
        $kind = [string]$Matches.kind
        $role = [string]$Matches.role
        $dialog = [string]$Matches.dialog
        [long]$appearance = [long]::Parse($Matches.appearance)
        [long]$owner = [long]::Parse($Matches.owner)
        $button = [string]$Matches.button
        [long]$tick = [long]::Parse($Matches.tick)
        # PowerShell omits an unmatched optional named capture from $Matches.
        # Use the real hashtable schema explicitly so OBSERVED/COMMITTED remain
        # valid under StrictMode while CLAIMED still fails closed without it.
        $hasBindAge = $Matches.ContainsKey('bindAgeMs') -and
            -not [string]::IsNullOrEmpty([string]$Matches['bindAgeMs'])
        [long]$bindAgeMs = if ($hasBindAge) {
            [long]::Parse([string]$Matches['bindAgeMs'])
        } else { [long]0 }

        if ($role -ne [string]$PopupService.Role) {
            throw "$($PopupService.Role) PID log published scripted-popup role=$role"
        }
        if ($appearance -le 0 -or $owner -le 0 -or $tick -le 0) {
            throw "$role scripted-popup marker contains a non-positive identity/tick"
        }
        if ($tick -lt [long]$PopupService.LastMarkerTick) {
            throw "$role scripted-popup marker tick regressed from $($PopupService.LastMarkerTick) to $tick"
        }
        $PopupService.LastMarkerTick = $tick
        $appearanceKey = [string]$appearance

        switch ($kind) {
            'OBSERVED' {
                if ($hasBindAge) {
                    throw "$role OBSERVED#$appearance unexpectedly contains bindAgeMs"
                }
                if ($appearance -le [long]$PopupService.LastObservedAppearance -or
                    $PopupService.Appearances.ContainsKey($appearanceKey)) {
                    throw "$role scripted-popup OBSERVED appearance $appearance is not a fresh monotonic generation"
                }
                $PopupService.LastObservedAppearance = $appearance
                $PopupService.Appearances[$appearanceKey] = [pscustomobject]@{
                    Dialog = $dialog
                    Owner = $owner
                    Button = $button
                    State = 'OBSERVED'
                    ObservedTick = $tick
                    ClaimedTick = [long]0
                    CommittedTick = [long]0
                }
            }
            'CLAIMED' {
                if (-not $hasBindAge -or
                    $bindAgeMs -lt [long]$script:StartupModalSettleMilliseconds) {
                    throw "$role CLAIMED#$appearance lost the native 300 ms bind-age gate"
                }
                if (-not $PopupService.Appearances.ContainsKey($appearanceKey)) {
                    throw "$role CLAIMED#$appearance has no preceding OBSERVED record"
                }
                $appearanceState = $PopupService.Appearances[$appearanceKey]
                $messageButtons = @('BTN_OK', 'BTN_YES', 'BTN_NO')
                $allowedMessageButtonResolution = $dialog -eq 'DLG_MESSAGE_BOX' -and
                    $messageButtons -contains [string]$appearanceState.Button -and
                    $messageButtons -contains $button
                if ([string]$appearanceState.State -ne 'OBSERVED' -or
                    [string]$appearanceState.Dialog -ne $dialog -or
                    [long]$appearanceState.Owner -ne $owner -or
                    ([string]$appearanceState.Button -ne $button -and
                     -not $allowedMessageButtonResolution) -or
                    $tick -lt [long]$appearanceState.ObservedTick) {
                    throw "$role CLAIMED#$appearance changed identity/order or was published twice"
                }
                $appearanceState.State = 'CLAIMED'
                # MESSAGE_BOX is observed at its first bind, then the completed
                # construction batch resolves legacy OK -> YES -> NO priority.
                # COMMITTED must match that final claimed button exactly.
                $appearanceState.Button = $button
                $appearanceState.ClaimedTick = $tick
            }
            'COMMITTED' {
                if ($hasBindAge) {
                    throw "$role COMMITTED#$appearance unexpectedly contains bindAgeMs"
                }
                if (-not $PopupService.Appearances.ContainsKey($appearanceKey)) {
                    throw "$role COMMITTED#$appearance has no preceding OBSERVED/CLAIMED records"
                }
                $appearanceState = $PopupService.Appearances[$appearanceKey]
                if ([string]$appearanceState.State -ne 'CLAIMED' -or
                    [string]$appearanceState.Dialog -ne $dialog -or
                    [long]$appearanceState.Owner -ne $owner -or
                    [string]$appearanceState.Button -ne $button -or
                    $tick -lt [long]$appearanceState.ClaimedTick) {
                    throw "$role COMMITTED#$appearance changed identity/order or was published twice"
                }
                $appearanceState.State = 'COMMITTED'
                $appearanceState.CommittedTick = $tick
            }
        }

        # wait_day_ready treated a popup bind and the pre-callback auto-dismiss
        # click record as evidence. OBSERVED and CLAIMED are those exact edges;
        # COMMITTED proves callback success but must not move the legacy clock.
        if ($kind -ne 'COMMITTED' -and
            $tick -gt [long]$script:LastStartupPopupEvidenceTick[$role]) {
            $script:LastStartupPopupEvidenceTick[$role] = $tick
            [long]$observerTick = [Environment]::TickCount64
            $script:LastStartupPopupEvidenceUtc[$role] =
                [DateTime]::UtcNow.AddMilliseconds($tick - $observerTick)
        }
        $events.Add([pscustomobject]@{
            Kind = $kind
            Role = $role
            Dialog = $dialog
            Appearance = $appearance
            Owner = $owner
            Button = $button
            BindAgeMs = $bindAgeMs
            Tick = $tick
        })
    }
    $PopupService.MarkerLineCount = [long]$eventLines.Count
    return @($events)
}

function Update-LiteralStartupPopupWindow([object]$Window,
                                          [object[]]$NewNativeEvents,
                                          [long]$NowTick) {
    if ([bool]$Window.Done) { return }
    if ([long]$Window.FirstActionTick -eq 0) {
        $firstClaim = @($NewNativeEvents | Where-Object {
            $_.Kind -eq 'CLAIMED' -and [long]$_.Tick -ge [long]$Window.ArmedTick
        } | Select-Object -First 1)
        if ($firstClaim.Count -eq 1) {
            # GetTickCount64 and Environment.TickCount64 share the same uptime
            # epoch. Anchor the source 10-second tail to the actual native claim,
            # never to PowerShell's later log-read time.
            $Window.FirstActionTick = [long]$firstClaim[0].Tick
        }
    }

    [long]$completionTick = [long]$Window.ArmedTick +
        [long]$Window.CapMilliseconds
    if ([long]$Window.FirstActionTick -gt 0) {
        $completionTick = [Math]::Min(
            $completionTick, ([long]$Window.FirstActionTick + 10000))
    }
    if ($NowTick -ge $completionTick) {
        # Legacy advances the nav timer exactly at this edge even when a fresh
        # bind generation is still inside its 300 ms settle. The independent
        # persistent service retains that generation and remains responsible.
        $Window.CompletedTick = $completionTick
        $Window.CompletedUtc = [DateTime]::UtcNow.AddMilliseconds(
            $completionTick - $NowTick)
        $Window.Done = $true
    }
}

function Invoke-LiteralStartupPopupTick([object]$Window) {
    $newNativeEvents = @(Invoke-LiteralPersistentStartupPopupTick $Window.PopupService)
    Update-LiteralStartupPopupWindow `
        $Window $newNativeEvents ([long][Environment]::TickCount64)
}

function New-LiteralStartupPopupTimeline([object]$HostWindow,
                                         [object]$JoinEntryWindow,
                                         [object]$JoinPopupService) {
    if (-not $HostWindow -or -not $JoinEntryWindow -or -not $JoinPopupService) {
        throw 'literal startup popup timeline requires host + join-entry windows and join service'
    }
    return [pscustomobject]@{
        HostWindow = $HostWindow
        JoinEntryWindow = $JoinEntryWindow
        JoinPopupService = $JoinPopupService
        JoinDelayTargetTick = [long]0
        JoinActiveWindow = $null
        HostPostDelayTargetTick = [long]0
        HostPostDelayDone = $false
        Done = $false
    }
}

function Invoke-LiteralStartupPopupTimelineTick([object]$Timeline) {
    [long]$nowTick = [Environment]::TickCount64
    $hostEvents = @(Invoke-LiteralPersistentStartupPopupTick `
        $Timeline.HostWindow.PopupService)
    Update-LiteralStartupPopupWindow $Timeline.HostWindow $hostEvents $nowTick

    # Read the join PID log once per timeline sample. When a coarse passive
    # sample crosses the exact +5000 boundary, the same retained marker batch is
    # offered to the newly armed second window and filtered by native tick.
    $joinEvents = @(Invoke-LiteralPersistentStartupPopupTick `
        $Timeline.JoinPopupService)
    if (-not [bool]$Timeline.JoinEntryWindow.Done) {
        Update-LiteralStartupPopupWindow `
            $Timeline.JoinEntryWindow $joinEvents $nowTick
    }
    if ([bool]$Timeline.JoinEntryWindow.Done -and
        [long]$Timeline.JoinDelayTargetTick -eq 0) {
        $Timeline.JoinDelayTargetTick =
            [long]$Timeline.JoinEntryWindow.CompletedTick + 5000
    }
    if ([long]$Timeline.JoinDelayTargetTick -gt 0 -and
        $nowTick -ge [long]$Timeline.JoinDelayTargetTick -and
        $null -eq $Timeline.JoinActiveWindow) {
        # Literal kJoinScript Delay(5000) is a timer between two distinct 20 s
        # windows. Lazy projection preserves its exact timestamp without making
        # READY -> quiet-3 -> deploy wait for the nav thread.
        $Timeline.JoinActiveWindow = New-LiteralStartupPopupWindow `
            join 20000 $Timeline.JoinPopupService `
            ([long]$Timeline.JoinDelayTargetTick)
    }
    if ($null -ne $Timeline.JoinActiveWindow) {
        Update-LiteralStartupPopupWindow `
            $Timeline.JoinActiveWindow $joinEvents $nowTick
    }

    if ([bool]$Timeline.HostWindow.Done -and
        [long]$Timeline.HostPostDelayTargetTick -eq 0) {
        # kHostScript's final +2000 remains anchored to the actual finite-window
        # completion, even though it is no longer a deploy prerequisite.
        $Timeline.HostPostDelayTargetTick =
            [long]$Timeline.HostWindow.CompletedTick + 2000
    }
    if ([long]$Timeline.HostPostDelayTargetTick -gt 0 -and
        $nowTick -ge [long]$Timeline.HostPostDelayTargetTick) {
        $Timeline.HostPostDelayDone = $true
    }
    if ([bool]$Timeline.HostPostDelayDone -and
        $null -ne $Timeline.JoinActiveWindow -and
        [bool]$Timeline.JoinActiveWindow.Done) {
        $Timeline.Done = $true
        $script:LegacyStartupWindowsComplete = $true
    }
}

function Wait-FixedUtcAnchorWithPopupService([DateTime]$TargetUtc,
                                             [object[]]$Windows) {
    while ([DateTime]::UtcNow -lt $TargetUtc) {
        foreach ($window in @($Windows)) {
            if ($window) {
                Invoke-LiteralStartupPopupTick $window
            }
        }
        $remainingMilliseconds = [Math]::Ceiling(
            ($TargetUtc - [DateTime]::UtcNow).TotalMilliseconds)
        if ($remainingMilliseconds -gt 0) {
            Start-Sleep -Milliseconds ([Math]::Min(100, $remainingMilliseconds))
        }
    }
}

function Wait-LiteralStartupPopupWindow([object]$Primary,
                                        [object[]]$AlsoService = @()) {
    while (-not [bool]$Primary.Done) {
        Invoke-LiteralStartupPopupTick $Primary
        foreach ($window in @($AlsoService)) {
            if ($window) {
                Invoke-LiteralStartupPopupTick $window
            }
        }
        if (-not [bool]$Primary.Done) { Start-Sleep -Milliseconds 100 }
    }
}

function Test-LiteralReadyWorldSnapshot([object]$World,
                                        [object]$Fixture = $null) {
    # Pure projection of run_test.ps1 Is-Ready. Its physical worker has already
    # made the one aggregate stacks/world read for this tick. The old predicate
    # selected the first host and first join hero; exact uniqueness belongs to
    # the later post-quiet reinforced-hero census, not this early observer.
    $world = $World
    if (-not $world) {
        return $null
    }
    $stacks = @((Get-OptionalProperty $world 'stacks'))
    $hostOwner = ''
    $joinOwner = ''
    if ($Fixture) {
        if ([string](Get-OptionalProperty $world 'sourceRole') -ne 'host' -or
            [long](Get-OptionalProperty $world 'sequence') -lt 1) {
            return $null
        }
        $hostStacks = @($stacks | Where-Object {
            (Get-LegacyStackRole $_) -eq 'host'
        })
        $joinStacks = @($stacks | Where-Object {
            (Get-LegacyStackRole $_) -eq 'joiner'
        })
        if ($hostStacks.Count -lt 1 -or $joinStacks.Count -lt 1) {
            return $null
        }
        $hostOwner = [string]$hostStacks[0].owner
        $joinOwner = [string]$joinStacks[0].owner
    } else {
        if ($null -eq (Get-OptionalProperty $world 'day')) { return $null }
        # Protocol-only use has no manifest. Derive both owners from this same
        # atomic host world: exactly one human self and one human enemy.
        $players = @((Get-OptionalProperty $world 'players'))
        $selfHumans = @($players | Where-Object {
            [bool](Get-OptionalProperty $_ 'human') -and
            [string](Get-OptionalProperty $_ 'relation') -eq 'self'
        })
        $enemyHumans = @($players | Where-Object {
            [bool](Get-OptionalProperty $_ 'human') -and
            [string](Get-OptionalProperty $_ 'relation') -eq 'enemy'
        })
        if ($selfHumans.Count -ne 1 -or $enemyHumans.Count -ne 1) {
            return $null
        }
        $hostOwner = [string](Get-OptionalProperty $selfHumans[0] 'id')
        $joinOwner = [string](Get-OptionalProperty $enemyHumans[0] 'id')
    }
    if ([string]::IsNullOrWhiteSpace($hostOwner) -or
        [string]::IsNullOrWhiteSpace($joinOwner) -or
        $hostOwner -eq $joinOwner) {
        return $null
    }
    foreach ($owner in @($hostOwner, $joinOwner)) {
        if (@($stacks | Where-Object {
                [string](Get-OptionalProperty $_ 'owner') -eq $owner
            }).Count -lt 1) {
            return $null
        }
    }
    return [pscustomobject]@{
        host = $world
        join = $world
        hostOwner = $hostOwner
        joinOwner = $joinOwner
    }
}

function Start-LiteralOuterReadyObserver([DateTime]$ArmUtc,
                                         [object]$Fixture = $null,
                                         [int]$MaxSamples = 120) {
    if ($MaxSamples -lt 1 -or $MaxSamples -gt 300) {
        throw 'literal outer Is-Ready sample budget is outside 1..300'
    }
    $hostOwner = if ($Fixture) {
        [string]$Fixture.players.hostHandle
    } else { '' }
    $joinOwner = if ($Fixture) {
        [string]$Fixture.players.joinHandle
    } else { '' }
    $state = [hashtable]::Synchronized(@{
        HostPid = [long]0
        JoinPid = [long]0
        StopRequested = $false
        Completed = $false
        Result = $null
        Error = ''
        SampleCount = [long]0
        WorldSampleCount = [long]0
        ArmUtc = $ArmUtc
        StartupObserverState = $null
        WorldReadySnapshot = $null
        WorldReadyObservedUtc = $null
    })
    $worker = [PowerShell]::Create()
    $workerSource = @'
param($State, $RelayBase, $HostOwner, $JoinOwner, $UseLegacyStacks, $MaxSamples)
$ErrorActionPreference = 'Stop'
try {
    for ($sampleIndex = 0; $sampleIndex -lt [int]$MaxSamples; $sampleIndex++) {
        # Literal run_test.ps1 loop: every next sample starts with its own +2
        # only after the previous process/world read has completed. No catch-up.
        Start-Sleep -Seconds 2
        if ([bool]$State.StopRequested) { return }

        [long]$hostPid = $State.HostPid
        [long]$joinPid = $State.JoinPid
        $publishedIds = @(
            if ($hostPid -gt 0) { [int]$hostPid }
            if ($joinPid -gt 0) { [int]$joinPid }
        )
        # One scoped process census per tick. An empty published set is the
        # exact-owned equivalent of the old zero-process result; ambient games
        # are never enumerated.
        $ownedProcesses = if ($publishedIds.Count -eq 0) {
            @()
        } else {
            @(Get-Process -Id $publishedIds -ErrorAction SilentlyContinue)
        }
        $State.SampleCount = [long]$State.SampleCount + 1
        if ($publishedIds.Count -ne 2 -or $ownedProcesses.Count -ne 2) {
            continue
        }

        $world = $State.WorldReadySnapshot
        if ($null -eq $world) {
            # Is-Ready called Get-Heroes only after its two-process census
            # passed. Fixture runs consume the exact legacy raw-stack source;
            # protocol-only runs retain their rich world projection. Latch the
            # first successful read so MSS cannot add another readiness read.
            try {
                $readyUri = if ($UseLegacyStacks) {
                    "$RelayBase/api/legacy-stacks"
                } else {
                    "$RelayBase/api/world?role=host"
                }
                $world = Invoke-RestMethod $readyUri -TimeoutSec 6
            } catch {
                continue
            }
            $State.WorldSampleCount = [long]$State.WorldSampleCount + 1
            if (-not $world) { continue }
            $stacks = @($world.stacks)
            $readyHostOwner = [string]$HostOwner
            $readyJoinOwner = [string]$JoinOwner
            if ($UseLegacyStacks) {
                if ([string]$world.sourceRole -ne 'host' -or
                    [long]$world.sequence -lt 1) { continue }
                $hostStacks = @($stacks | Where-Object {
                    [string]$_.owner -match '^0x[0-9A-Fa-f]{4}0001$'
                })
                $joinStacks = @($stacks | Where-Object {
                    [string]$_.owner -match '^0x[0-9A-Fa-f]{4}0002$'
                })
                if ($hostStacks.Count -lt 1 -or $joinStacks.Count -lt 1) {
                    continue
                }
                $readyHostOwner = [string]$hostStacks[0].owner
                $readyJoinOwner = [string]$joinStacks[0].owner
            } elseif ([string]::IsNullOrWhiteSpace($readyHostOwner) -and
                [string]::IsNullOrWhiteSpace($readyJoinOwner)) {
                # Protocol-only mode derives both roles from this same atomic
                # host world. Ambiguous/missing human ownership is not ready.
                $players = @($world.players)
                $selfHumans = @($players | Where-Object {
                    [bool]$_.human -and [string]$_.relation -eq 'self'
                })
                $enemyHumans = @($players | Where-Object {
                    [bool]$_.human -and [string]$_.relation -eq 'enemy'
                })
                if ($selfHumans.Count -ne 1 -or $enemyHumans.Count -ne 1) {
                    continue
                }
                $readyHostOwner = [string]$selfHumans[0].id
                $readyJoinOwner = [string]$enemyHumans[0].id
            }
            if ([string]::IsNullOrWhiteSpace($readyHostOwner) -or
                [string]::IsNullOrWhiteSpace($readyJoinOwner) -or
                $readyHostOwner -eq $readyJoinOwner) { continue }
            $ready =
                @($stacks | Where-Object { [string]$_.owner -eq $readyHostOwner }).Count -ge 1 -and
                @($stacks | Where-Object { [string]$_.owner -eq $readyJoinOwner }).Count -ge 1
            if (-not $ready) { continue }
            $State.WorldReadySnapshot = $world
            $State.WorldReadyObservedUtc = [DateTime]::UtcNow
        }

        # The inner observer owns the append-only relay/native-log reads. This
        # outer worker samples only its synchronized, immutable publication;
        # it never issues an action or performs a second world observation.
        $startupState = $State.StartupObserverState
        if ($null -eq $startupState) { continue }
        if (-not [string]::IsNullOrWhiteSpace([string]$startupState.Error)) {
            throw [string]$startupState.Error
        }
        if (-not [bool]$startupState.OperationalReady) { continue }
        $operationalWitness = $startupState.OperationalWitness
        if ($null -eq $operationalWitness) {
            throw 'literal outer Is-Ready observed an operational latch without its witness'
        }
        $State.Result = [pscustomobject]@{
            host = $world
            join = $world
            worldObservedUtc = $State.WorldReadyObservedUtc
            operational = $operationalWitness
            observedUtc = [DateTime]::UtcNow
            sampleCount = [long]$State.SampleCount
            worldSampleCount = [long]$State.WorldSampleCount
        }
        $State.Completed = $true
        return
    }
    $State.Error = 'literal outer Is-Ready did not observe both heroes within its fixed budget'
} catch {
    $State.Error = $_.Exception.Message
}
'@
    try {
        [void]$worker.AddScript($workerSource)
        [void]$worker.AddArgument($state)
        [void]$worker.AddArgument([string]$script:RelayBase)
        [void]$worker.AddArgument($hostOwner)
        [void]$worker.AddArgument($joinOwner)
        [void]$worker.AddArgument($null -ne $Fixture)
        [void]$worker.AddArgument($MaxSamples)
        $async = $worker.BeginInvoke()
        return [pscustomobject]@{
            State = $state
            Worker = $worker
            Async = $async
            Disposed = $false
        }
    } catch {
        $worker.Dispose()
        throw
    }
}

function Publish-LiteralOuterReadyProcess([object]$Observer,
                                         [ValidateSet('host', 'join')][string]$Role,
                                         [System.Diagnostics.Process]$Process) {
    if (-not $Observer -or [bool]$Observer.Disposed -or -not $Process) {
        throw "cannot publish $Role to the literal outer Is-Ready observer"
    }
    $slot = if ($Role -eq 'host') { 'HostPid' } else { 'JoinPid' }
    if ([long]$Observer.State[$slot] -ne 0) {
        throw "literal outer Is-Ready received the $Role process twice"
    }
    $Observer.State[$slot] = [long]$Process.Id
}

function Publish-LiteralOuterReadyStartupObserver(
    [object]$Observer,
    [object]$StartupObserver) {
    if (-not $Observer -or [bool]$Observer.Disposed -or
        -not $StartupObserver -or [bool]$StartupObserver.Disposed) {
        throw 'cannot publish the literal inner startup observer to outer Is-Ready'
    }
    if ($null -ne $Observer.State.StartupObserverState) {
        throw 'literal outer Is-Ready received the startup observer twice'
    }
    $Observer.State.StartupObserverState = $StartupObserver.State
}

function Complete-LiteralOuterReadyObserver([object]$Observer) {
    if (-not $Observer -or [bool]$Observer.Disposed) {
        throw 'literal outer Is-Ready observer is missing or already consumed'
    }
    try {
        [void]$Observer.Worker.EndInvoke($Observer.Async)
    } finally {
        $Observer.Worker.Dispose()
        $Observer.Disposed = $true
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$Observer.State.Error)) {
        throw [string]$Observer.State.Error
    }
    if (-not [bool]$Observer.State.Completed -or -not $Observer.State.Result) {
        throw 'literal outer Is-Ready observer ended without one saved ready snapshot'
    }
    return $Observer.State.Result
}

function Stop-LiteralOuterReadyObserver([object]$Observer) {
    if (-not $Observer -or [bool]$Observer.Disposed) { return }
    $Observer.State.StopRequested = $true
    try {
        [void]$Observer.Worker.EndInvoke($Observer.Async)
    } finally {
        $Observer.Worker.Dispose()
        $Observer.Disposed = $true
    }
}

function Get-LiteralNestedStartupSubstitutionMap {
    # test_R_virtual_turn.ps1 ran concurrently with the outer READY/quiet path.
    # The removable inner observer now replays every diagnostic polling/read
    # boundary. Its old activation/cascade mutations map to one release-file edge
    # and MSS's typed post-dispatch barriers; no PowerShell gameplay action is
    # added or repeated. Keep that sole architectural substitution explicit.
    return [pscustomobject]@{
        LegacyRolePollMilliseconds = 500
        RolePollEquivalent = 'HelloAck -> alive Log -> first UI snapshot; exact owned PID publication'
        OuterReadyBootlogFailurePollEquivalent = 'direct propagated startup/pairing exception replaces FAIL: roles never settled bootlog observation'
        LegacyStrategicPollMilliseconds = 400
        StrategicPollEquivalent = 'append-only native startup UI/log markers and stock startup transition evidence'
        LegacyPostStrategicSettleSeconds = 2
        InstallPatchCrashSnapshotEquivalent = 'saved exact-PID startup witness plus native preflight/fault sentinels'
        PreActionPacketBaselineDisposition = 'literal two-read Begin-then-End host TX census after the fixed cascade +800 ms edge'
        LegacyJoinActivationToCascadeMilliseconds = 500
        JoinActivationEquivalent = 'one release file -> session-plan-created + session-plan-delivered(host/join) -> host/join session activation + directed day-1 BeginTurn apply'
        JoinActivationToCascadeEquivalent = 'one +500 ms timer is armed at the exact BootstrapBeginTurnApplied completion edge; host activation must arrive before its fixed deadline'
        LegacyCascadeToPacketBaselineMilliseconds = 800
        HostCascadeEquivalent = 'engine-action-dispatched(stage=bootstrap-apply) -> bootstrap-cascade-complete + bootstrap-turn-info-applied'
        TerminalReleaseEquivalent = 'bootstrap-commit-dispatched -> bootstrap-commit-applied(host/join) -> bootstrap-operational-dispatched -> bootstrap-operational-applied(host/join) -> session-operational'
        Classification = 'literal legacy observer/read projection with one typed v8 MSS release-edge substitution; MSS evidence follows old verdict boundaries'
    }
}

function Measure-LiteralSavedLogMarker([string[]]$Lines, [string]$Marker) {
    return @($Lines | Select-String -SimpleMatch -Pattern $Marker).Count
}

function Get-LiteralStartupMssWitnessSnapshot([object]$PairingResult,
                                               [object]$LegacyQuietWitness) {
    if (-not $PairingResult -or -not $LegacyQuietWitness -or
        -not $LegacyQuietWitness.LegacyQuietEntryRoleStates) {
        throw 'literal startup MSS witness requires the saved quiet-PASS witness'
    }
    # The source quiet PASS and its immediately-following single shared hero
    # census have already completed.  Only now sample append-only MSS event
    # sources; the saved terminal log pair below is never re-read.
    $turnHistory = Get-TurnHistory `
        -After ([long]$PairingResult.LatestSequence) -Role join
    $simEvents = @(Read-SimRelayEvents)
    return [pscustomobject]@{
        CapturedTick = [long][Environment]::TickCount64
        TurnHistory = $turnHistory
        SimEvents = $simEvents
        HostLogLines = @($LegacyQuietWitness.HostLogLines)
        JoinLogLines = @($LegacyQuietWitness.JoinLogLines)
        LegacyQuietEntryRoleStates = $LegacyQuietWitness.LegacyQuietEntryRoleStates
        PreparedDeployBindings = $null
        TerminalMapStateCaptured = $false
    }
}

function Assert-LiteralStockStartupCompleteSnapshot(
    [object]$History,
    [long]$After,
    [object]$InitialBegin,
    [object]$InitialHostJoin) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue -or
        $null -eq $History -or $null -eq $InitialBegin -or
        $null -eq $InitialHostJoin) {
        throw 'saved stock startup-completion witness is incomplete'
    }
    [long]$initialBeginSequence = Get-RequiredTelemetryNumber `
        $InitialBegin 'seq' 'initial startup BeginTurn witness'
    [long]$initialHostJoinSequence = Get-RequiredTelemetryNumber `
        $InitialHostJoin 'seq' 'initial startup host CJoinGame witness'
    [long]$expectedSender = Get-RequiredTelemetryNumber `
        $InitialBegin 'senderDpid' 'initial startup BeginTurn witness'
    [long]$expectedReceiver = Get-RequiredTelemetryNumber `
        $InitialBegin 'receiverDpid' 'initial startup BeginTurn witness'
    [long]$expectedHostHandle = Get-RequiredTelemetryNumber `
        $InitialBegin 'activeHandle' 'initial startup BeginTurn witness'
    [long]$initialJoinedHandle = Get-RequiredTelemetryNumber `
        $InitialHostJoin 'joinedHandle' 'initial startup host CJoinGame witness'
    if ($initialBeginSequence -ge $initialHostJoinSequence -or
        $initialHostJoinSequence -gt $After -or $expectedSender -ne 1 -or
        $expectedReceiver -le 1 -or $expectedHostHandle -eq 0 -or
        $initialJoinedHandle -ne $expectedHostHandle) {
        throw 'initial startup witness cannot seed the saved completion proof'
    }

    [long]$latestSequence = Get-EvidenceWatermark `
        $History 'saved join stock startup completion'
    $events = @((Get-OptionalProperty $History 'events'))
    foreach ($eventRecord in $events) {
        [long]$sequence = Get-RequiredTelemetryNumber `
            $eventRecord 'seq' 'saved join stock startup-completion event'
        if ($sequence -le $After -or $sequence -gt $latestSequence) {
            throw ("saved startup-completion event is outside its subscription " +
                "interval (after=$After event=$sequence latest=$latestSequence)")
        }
    }
    $unexpected = @($events | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -notin @(
            'stock-startup-directed-begin-turn-observed',
            'stock-startup-complete-observed')
    })
    if ($unexpected.Count -gt 0) {
        throw 'saved startup-completion witness contains an unexpected transition'
    }
    $directedEvents = @($events | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -eq
            'stock-startup-directed-begin-turn-observed'
    })
    $completeEvents = @($events | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -eq
            'stock-startup-complete-observed'
    })
    if ($directedEvents.Count -ne 1 -or $completeEvents.Count -ne 1) {
        throw ("saved startup completion is not exact " +
            "(directed=$($directedEvents.Count) complete=$($completeEvents.Count))")
    }

    $directed = $directedEvents[0]
    $complete = $completeEvents[0]
    if ([string](Get-OptionalProperty $directed 'role') -ne 'join' -or
        [string](Get-OptionalProperty $complete 'role') -ne 'join') {
        throw 'saved startup-completion transitions do not both belong to join'
    }
    [long]$directedSequence = Get-RequiredTelemetryNumber `
        $directed 'seq' 'saved directed startup BeginTurn'
    [long]$completeSequence = Get-RequiredTelemetryNumber `
        $complete 'seq' 'saved join-player startup CJoinGame'
    if ($initialHostJoinSequence -ge $directedSequence -or
        $directedSequence -ge $completeSequence) {
        throw 'saved stock startup evidence violated B(H)<CJoin(H)<D(J,H)<CJoin(J)'
    }

    [long]$directedSender = Get-RequiredTelemetryNumber `
        $directed 'senderDpid' 'saved directed startup BeginTurn'
    [long]$directedReceiver = Get-RequiredTelemetryNumber `
        $directed 'receiverDpid' 'saved directed startup BeginTurn'
    [long]$directedFrame = Get-RequiredTelemetryNumber `
        $directed 'frameLength' 'saved directed startup BeginTurn'
    [long]$joinHandle = Get-RequiredTelemetryNumber `
        $directed 'addressee' 'saved directed startup BeginTurn'
    [long]$directedCommandSequence = Get-RequiredTelemetryNumber `
        $directed 'commandSequence' 'saved directed startup BeginTurn'
    [long]$directedActiveHandle = Get-RequiredTelemetryNumber `
        $directed 'activeHandle' 'saved directed startup BeginTurn'
    if ($directedSender -ne $expectedSender -or
        $directedReceiver -ne $expectedReceiver -or $directedFrame -ne 56 -or
        $joinHandle -eq 0 -or $joinHandle -eq $expectedHostHandle -or
        $directedCommandSequence -ne [uint32]::MaxValue -or
        $directedActiveHandle -ne $expectedHostHandle) {
        throw 'saved directed startup BeginTurn changed its stock layout/identity'
    }

    [long]$completeSender = Get-RequiredTelemetryNumber `
        $complete 'senderDpid' 'saved join-player startup CJoinGame'
    [long]$completeReceiver = Get-RequiredTelemetryNumber `
        $complete 'receiverDpid' 'saved join-player startup CJoinGame'
    [long]$completeFrame = Get-RequiredTelemetryNumber `
        $complete 'frameLength' 'saved join-player startup CJoinGame'
    [long]$completeJoinedHandle = Get-RequiredTelemetryNumber `
        $complete 'joinedHandle' 'saved join-player startup CJoinGame'
    [long]$completeNameLength = Get-RequiredTelemetryNumber `
        $complete 'nameLength' 'saved join-player startup CJoinGame'
    [void](Get-RequiredTelemetryNumber `
        $complete 'raceCategoryId' 'saved join-player startup CJoinGame')
    if ($completeSender -ne $expectedSender -or
        $completeReceiver -ne $expectedReceiver -or $completeNameLength -lt 1 -or
        $completeFrame -ne (56 + $completeNameLength) -or
        $completeJoinedHandle -ne $joinHandle) {
        throw 'saved join-player CJoinGame changed its stock layout/identity'
    }
    return [pscustomobject]@{
        DirectedBegin = $directed
        JoinComplete = $complete
        LatestSequence = $latestSequence
    }
}

function Assert-LiteralStartupMssWitness(
    [object]$Witness,
    [object]$PairingResult,
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [switch]$RequireReinforcement) {
    if (-not $Witness -or [long]$Witness.CapturedTick -le 0) {
        throw 'legacy quiet-3 PASS did not retain its terminal MSS witness'
    }
    $events = @($Witness.SimEvents)
    Assert-NoRelayFault $events
    Assert-ExactBootstrapOperational $events
    Assert-ProductionRelayClientIdentity `
        $events $HostProcess $JoinProcess
    if (-not $Witness.LegacyQuietEntryRoleStates) {
        throw 'legacy quiet observer omitted its one Host-then-Client PID-state pair'
    }
    Assert-DebugRelayClientIdentity `
        host $Witness.LegacyQuietEntryRoleStates.host $HostProcess
    Assert-DebugRelayClientIdentity `
        join $Witness.LegacyQuietEntryRoleStates.join $JoinProcess
    if (-not [bool]$Witness.TerminalMapStateCaptured -or
        -not $Witness.PreparedDeployBindings -or
        -not $Witness.PreparedDeployBindings.host -or
        -not $Witness.PreparedDeployBindings.join) {
        throw 'terminal quiet iteration omitted its coherent prepared deploy bindings'
    }
    $startupComplete = Assert-LiteralStockStartupCompleteSnapshot `
        $Witness.TurnHistory `
        ([long]$PairingResult.LatestSequence) `
        $PairingResult.InitialBegin `
        $PairingResult.InitialHostJoin

    $hostLines = @($Witness.HostLogLines)
    $joinLines = @($Witness.JoinLogLines)
    $sessionListReadyAfter = Measure-LiteralSavedLogMarker `
        $joinLines ([string]$PairingResult.SessionListReadyMarker)
    $sessionListReadyDelta = $sessionListReadyAfter -
        [int]$PairingResult.SessionListReadyBefore
    if ($sessionListReadyDelta -ne 1) {
        throw ("saved join DirectPlay enumeration marker delta is " +
            "$sessionListReadyDelta, expected exactly 1")
    }

    $faultPattern = @(
        '\[simturns\] terminal fault:',
        '\[SIMTURNS\] terminal pipe fault:',
        '\[simturns\] preflight mismatch',
        '\[simturns\] detour preflight mismatch',
        '55FC74',
        'midCommandQueue2PushHooked: message with id 21 is rejected due to outdated sequence number'
    )
    $faults = @(
        $hostLines | Select-String -Pattern $faultPattern | ForEach-Object Line
        $joinLines | Select-String -Pattern $faultPattern | ForEach-Object Line
    )
    if ($faults.Count -gt 0) {
        throw "saved client log witness contains a production simturn fault: $($faults[0])"
    }

    $leaderMarker = '[simturns] bootstrap first-leader-name TX sent (role='
    $hostLeaderCount = Measure-LiteralSavedLogMarker $hostLines $leaderMarker
    $joinLeaderCount = Measure-LiteralSavedLogMarker $joinLines $leaderMarker
    if ($hostLeaderCount -ne 1 -or $joinLeaderCount -ne 1) {
        throw ("bootstrap requires one saved natural first-leader-name TX per client " +
            "(host=$hostLeaderCount join=$joinLeaderCount)")
    }

    $operationalMarker =
        '[simturns] bootstrap operational release applied; strict independent turns are operational'
    $hostOperationalCount =
        Measure-LiteralSavedLogMarker $hostLines $operationalMarker
    $joinOperationalCount =
        Measure-LiteralSavedLogMarker $joinLines $operationalMarker
    if ($hostOperationalCount -ne 1 -or $joinOperationalCount -ne 1) {
        throw ("saved native operational gate is not exact " +
            "(host=$hostOperationalCount join=$joinOperationalCount)")
    }
    $battlePatchMarker =
        '[simturns] concurrent-battle compatibility patches active (0x635578=NOP2, 0x638886=NOP7)'
    $hostBattlePatchCount =
        Measure-LiteralSavedLogMarker $hostLines $battlePatchMarker
    $joinBattlePatchCount =
        Measure-LiteralSavedLogMarker $joinLines $battlePatchMarker
    if ($hostBattlePatchCount -ne 1 -or $joinBattlePatchCount -ne 1) {
        throw ("saved concurrent-battle patch activation is not exact " +
            "(host=$hostBattlePatchCount join=$joinBattlePatchCount)")
    }

    $reinforcementMarker =
        '[testdrv][fixture-reinforcement] COMMITTED unit-transfer plan:'
    $hostReinforcementCount =
        Measure-LiteralSavedLogMarker $hostLines $reinforcementMarker
    $joinReinforcementCount =
        Measure-LiteralSavedLogMarker $joinLines $reinforcementMarker
    if ($RequireReinforcement -and
        ($hostReinforcementCount -ne 1 -or $joinReinforcementCount -ne 0)) {
        throw ("saved Devouring reinforcement marker is not host-authoritative/exact " +
            "(host=$hostReinforcementCount join=$joinReinforcementCount)")
    }
    return [pscustomobject]@{
        StartupComplete = $startupComplete
        SessionListReadyDelta = $sessionListReadyDelta
        HostLeaderNameCount = $hostLeaderCount
        JoinLeaderNameCount = $joinLeaderCount
        HostReinforcementCount = $hostReinforcementCount
        JoinReinforcementCount = $joinReinforcementCount
        LegacyQuietEntryRoleStates = $Witness.LegacyQuietEntryRoleStates
        PreparedDeployBindings = $Witness.PreparedDeployBindings
    }
}

function Resolve-LiteralHostAuthoritativeHeroes([object]$Fixture) {
    # run_test.ps1 resolves both role-tagged IDs from one relay-global /stacks
    # response and explicitly ignores its garrison coordinates. Read the exact
    # removable host raw-stack projection once; /api/world is a separate source.
    $world = Get-LegacyStackSnapshot
    if (-not $world -or [long]$world.sequence -lt 1) {
        throw 'literal hero census did not expose the shared raw stack registry'
    }
    $resolved = @{}
    foreach ($side in @('host', 'join')) {
        $stackRole = if ($side -eq 'host') { 'host' } else { 'joiner' }
        $hero = (Get-OptionalProperty $world 'stacks') | Where-Object {
            (Get-LegacyStackRole $_) -eq $stackRole
        } | Select-Object -First 1
        if (-not $hero -or
            [string]::IsNullOrWhiteSpace([string](Get-OptionalProperty $hero 'id'))) {
            throw "literal shared census did not expose the first $side-owned hero id"
        }
        $resolved[$side] = $hero
    }
    return [pscustomobject]@{
        hostId = [string]$resolved.host.id
        joinId = [string]$resolved.join.id
        host = $resolved.host
        join = $resolved.join
        world = $world
    }
}

function Assert-DeferredLiteralHostAuthoritativeHeroFixture([object]$Census,
                                                             [object]$Fixture) {
    if (-not $Census -or -not $Census.world -or
        [string](Get-OptionalProperty $Census.world 'sourceRole') -ne 'host' -or
        [long](Get-OptionalProperty $Census.world 'sequence') -lt 1) {
        throw 'deferred literal hero census did not retain the host raw-stack publication'
    }
    foreach ($side in @('host', 'join')) {
        $hero = $Census.$side
        if ([string]::IsNullOrWhiteSpace([string](Get-OptionalProperty $hero 'id')) -or
            [string](Get-OptionalProperty $hero 'owner') -ne
                [string]$Fixture.players."${side}Handle") {
            throw "deferred literal shared census did not contain the exact $side-owned hero"
        }
    }
}

function Wait-LiteralDayReady([int]$QuietSec = 3,
                              [int]$TimeoutSec = 60,
                              [object]$PopupTimeline,
                              [object]$PairingResult) {
    if (-not $PopupTimeline -or -not $PairingResult) {
        throw ('literal wait-day-ready requires the already-armed popup timeline ' +
            'and saved pre-quiet pairing witness')
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    # wait_day_ready.ps1 resolved Host then Client exactly once before entering
    # its 800 ms quiet loop. These are the two original PID/state observations;
    # they are deliberately distinct from the terminal typed-map admission.
    $legacyHostRoleState = Get-RoleState host
    $legacyJoinRoleState = Get-RoleState join
    $legacyQuietEntryRoleStates = [pscustomobject]@{
        host = $legacyHostRoleState
        join = $legacyJoinRoleState
    }
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($script:LiteralInnerStartupObserver) {
            Assert-LiteralInnerStartupObserverHealthy `
                $script:LiteralInnerStartupObserver
        }
        Invoke-LiteralStartupPopupTimelineTick $PopupTimeline
        $quiet = @{}
        [long]$observationTick = [Environment]::TickCount64
        foreach ($role in @('host', 'join')) {
            [long]$lastEvidenceTick = $script:LastStartupPopupEvidenceTick[$role]
            $quiet[$role] = if ($lastEvidenceTick -le 0) {
                $null
            } else {
                [double]($observationTick - $lastEvidenceTick) / 1000.0
            }
        }
        $hostText = if ($null -eq $quiet.host) { 'no-popup-yet' } `
            else { '{0:N1}s quiet' -f $quiet.host }
        $joinText = if ($null -eq $quiet.join) { 'no-popup-yet' } `
            else { '{0:N1}s quiet' -f $quiet.join }
        Write-Step "legacy wait-day-ready host=$hostText join=$joinText (need ${QuietSec}s)"
        if ($null -ne $quiet.host -and $null -ne $quiet.join -and
            $quiet.host -ge $QuietSec -and $quiet.join -ge $QuietSec) {
            # The old next operation was the one shared Get-Heroes census. The
            # strict transport adapter additionally needs both exact native map
            # owners, so capture them together at this already-proven quiet edge.
            # This is one passive state publication read, not a wait or action.
            $terminalRelayState = Get-RelayState
            return [pscustomobject]@{
                CapturedTick = [long][Environment]::TickCount64
                HostLogLines = @($PopupTimeline.HostWindow.PopupService.SnapshotLines)
                JoinLogLines = @($PopupTimeline.JoinPopupService.SnapshotLines)
                LegacyQuietEntryRoleStates = $legacyQuietEntryRoleStates
                TerminalRoleStates = [pscustomobject]@{
                    host = Get-OptionalProperty $terminalRelayState 'host'
                    join = Get-OptionalProperty $terminalRelayState 'join'
                }
            }
        }
        # The source wait_day_ready observer samples every 800 ms. The native
        # subscriber is session-long and keeps consuming between these passive
        # samples without any PowerShell action loop.
        $nextObservationUtc = [DateTime]::UtcNow.AddMilliseconds(800)
        Wait-FixedUtcAnchor $nextObservationUtc
    }
    return $false
}

# Read-only compatibility observer. Startup popup actions belong exclusively to
# the native session-long subscriber, including callers that still use this old
# strategic-arrival helper as an observation gate.
function Drive-ToStrategic([string]$Role, [int]$TimeoutSec) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $observation = Get-DialogObservation $Role
        if (-not $observation) {
            Start-Sleep -Milliseconds 250
            continue
        }
        $dialog = $observation.Dialog
        if ((Get-OptionalProperty $observation.State 'reachedStrategic') -and
            $observation.Ready -and $script:BareMapDialogs -contains $dialog) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Run-Pairing([string]$HostLog, [string]$JoinLog,
                     [string]$ExactScenarioPath = '',
                     [int]$ExactScenarioIndex = 0,
                     [System.Diagnostics.Process]$HostProcess,
                     [System.Diagnostics.Process]$JoinProcess,
                      [DateTime]$HostLaunchUtc,
                      [DateTime]$JoinLaunchUtc,
                      [Parameter(Mandatory)][ValidateNotNull()]
                      [object]$StartupObserver,
                      [object]$ReadyObserver) {
    $resolvedScenarioPath = ''
    if (-not [string]::IsNullOrWhiteSpace($ExactScenarioPath)) {
        $resolvedScenarioPath = [IO.Path]::GetFullPath($ExactScenarioPath)
        if (-not (Test-Path -LiteralPath $resolvedScenarioPath -PathType Leaf)) {
            throw "exact scenario file is missing: $resolvedScenarioPath"
        }
    }
    [long]$expectedInitialHostHandle = 0
    if ($fixture) {
        $expectedInitialHostHandle = Convert-ExactPlayerHandle `
            (Get-OptionalProperty $fixture.players 'hostHandle') 'fixture host handle'
    }
    $waitPeerMarker = '[nettrace] peer CConnectMsg observed self='
    # Both complete old self_nav scripts enter gated physical workers before any
    # HTTP or complete PID-log baseline read. Each role owns those observations
    # after the common release, without a serialized parent-side prelude.
    $navigation = Complete-LiteralIndependentNavigation `
        -StartupObserver $StartupObserver `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -HostLaunchUtc $HostLaunchUtc -JoinLaunchUtc $JoinLaunchUtc `
        -HostLog $HostLog -JoinLog $JoinLog `
        -WaitPeerMarker $waitPeerMarker `
        -ExpectedInitialHostHandle $expectedInitialHostHandle `
        -ExactScenarioPath $resolvedScenarioPath `
        -ExactScenarioIndex $ExactScenarioIndex `
        -ScenarioIndex $Scenario
    foreach ($passiveDiagnostic in @($navigation.PassiveReadDiagnostics)) {
        Write-Step "tolerated $passiveDiagnostic"
    }
    Write-Step ('independent self-nav lanes completed: ' +
        "host BTN_LOAD=$($navigation.HostLoad.CompletedUtc.ToString('O')); " +
        "join BTN_JOIN_GAME=$($navigation.JoinGame.CompletedUtc.ToString('O'))")
    $hostPopupService = $navigation.HostPopupService
    $hostPopupWindow = $navigation.HostPopupWindow
    $startupWitness = $navigation.StartupWitness
    $joinPopupService = $navigation.JoinPopupService
    $joinEntryPopupWindow = $navigation.JoinPopupWindow

    if (-not [bool]$joinEntryPopupWindow.Done) {
        throw 'join lane handed off an incomplete first 20-second popup window'
    }
    # kJoinScript stayed inside its independent lane through CJoinGame,
    # BeginTurn, +1000, +1000, BTN_OK, +500 and the completed first 20 s popup
    # window. Run-Pairing only hands the retained services to later projections.
    $popupTimeline = New-LiteralStartupPopupTimeline `
        $hostPopupWindow $joinEntryPopupWindow $joinPopupService

    # This is the exact old run_test.ps1 Is-Ready boundary: each role-tagged
    # reporter has exposed at least one own hero. It intentionally does not wait
    # for either finite AutoDismiss timer, the join +5000/second-window tail, or
    # host's final +2000. Those independent timestamp projections continue while
    # the parent begins its literal 800 ms quiet observer.
    # run_test.ps1's outer parent armed Is-Ready before every nested startup
    # wrapper. Consume its already-running worker here without another process,
    # world, popup or log observation.
    $readyWorlds = $null
    if ($ReadyObserver) {
        $readyWorlds = Complete-LiteralOuterReadyObserver $ReadyObserver
        Write-Step ('READY: both owned heroes exist and both native OX ' +
            'operational releases are applied; starting literal quiet observer')
    } else {
        # test_R_masstest invoked test_R_mod synchronously and had no outer
        # run_test Is-Ready worker. Navigation is handed directly to the still
        # running literal inner test_R_virtual_turn observer.
        Write-Step 'ordered masstest navigation complete; no run_test outer Is-Ready observer was armed'
    }
    return [pscustomobject]@{
        LatestSequence = [long]$startupWitness.LatestSequence
        InitialBegin = $startupWitness.BeginTurn
        InitialHostJoin = $startupWitness.JoinGame
        ReadyWorlds = $readyWorlds
        PopupTimeline = $popupTimeline
        SessionListReadyMarker = $navigation.SessionListReadyMarker
        SessionListReadyBefore = [int]$navigation.SessionListReadyBefore
    }
}

function Assert-DebugRelayClientIdentity(
    [string]$Role,
    [object]$RoleState,
    [System.Diagnostics.Process]$OwnedProcess) {
    if (-not $RoleState -or -not (Get-OptionalProperty $RoleState 'connected')) {
        throw "DebugTest UI relay lost the owned $Role client"
    }
    $actualProcessId = Get-OptionalProperty $RoleState 'pid'
    if ($null -eq $actualProcessId -or
        [long]$actualProcessId -ne [long]$OwnedProcess.Id) {
        throw "DebugTest UI relay $Role PID $actualProcessId does not match owned PID $($OwnedProcess.Id)"
    }

    $reportedModulePath = [string](Get-OptionalProperty $RoleState 'modulePath')
    if ([string]::IsNullOrWhiteSpace($reportedModulePath)) {
        throw "DebugTest UI relay $Role did not report its module path"
    }
    $actualModulePath = [IO.Path]::GetFullPath($reportedModulePath)
    $expectedModulePath = [IO.Path]::GetFullPath((Join-Path $GameDir 'mss32.dll'))
    if (-not [string]::Equals($actualModulePath, $expectedModulePath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "DebugTest UI relay $Role module '$actualModulePath' is not '$expectedModulePath'"
    }
}

function Assert-OwnedProcessesLive([System.Diagnostics.Process]$HostProcess,
                                   [System.Diagnostics.Process]$JoinProcess) {
    $HostProcess.Refresh()
    $JoinProcess.Refresh()
    if ($HostProcess.HasExited -or $JoinProcess.HasExited) {
        throw "game process exited (hostExited=$($HostProcess.HasExited), joinExited=$($JoinProcess.HasExited))"
    }
}

function Assert-ClientsLive([System.Diagnostics.Process]$HostProcess,
                            [System.Diagnostics.Process]$JoinProcess) {
    Assert-OwnedProcessesLive $HostProcess $JoinProcess
    $state = Get-RelayState
    # `$Host` is a built-in read-only PowerShell variable and variable names are case-insensitive.
    # Keep role snapshots explicitly named so this assertion can run after the map is loaded.
    $hostState = Get-OptionalProperty $state 'host'
    $joinState = Get-OptionalProperty $state 'join'
    Assert-DebugRelayClientIdentity host $hostState $HostProcess
    Assert-DebugRelayClientIdentity join $joinState $JoinProcess
}

# Read-only bare-map/world gate. The native scripted-popup subscriber remains
# the sole owner of every startup popup for the whole process lifetime.
function Wait-ActionablePair([int]$TimeoutSec = 90,
                             [int]$QuietSec = 3) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $quietSince = $null
    while ((Get-Date) -lt $deadline) {
        $bothBare = $true
        foreach ($role in @('host', 'join')) {
            $observation = Get-DialogObservation $role
            if (-not $observation) {
                $bothBare = $false
                continue
            }
            $dialog = $observation.Dialog
            if (-not $observation.Ready -or $script:BareMapDialogs -notcontains $dialog) {
                $bothBare = $false
            }
        }
        if ($bothBare) {
            if ($null -eq $quietSince) { $quietSince = Get-Date }
            # World snapshots are comparatively expensive. Do not read them until
            # the exact bare-map observations have stayed quiet for the required
            # interval. Popup consumption continues independently in native code.
            if (((Get-Date) - $quietSince).TotalSeconds -ge $QuietSec) {
                $hostWorld = Get-World host
                $joinWorld = Get-World join
                if ($null -ne (Get-OptionalProperty $hostWorld 'day') -and
                    $null -ne (Get-OptionalProperty $joinWorld 'day')) {
                    return $true
                }
            }
        } else {
            $quietSince = $null
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# SessionPlan is intentionally delivered while stock startup work may still be
# queued behind the first-turn dialogs. Consume the append-only UI publication
# stream with an independent monotonic cursor for each role until the causal
# protocol-v8 bootstrap is operational and both clients expose bare maps. Popup
# actions are owned exclusively by the session-long native scripted subscriber;
# this MSS extension reads its exact PID logs and UI history but emits no command.
function Wait-OperationalBootstrapAndActionablePair {
    param(
        [Parameter(Mandatory)][System.Diagnostics.Process]$RelayProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [Parameter(Mandatory)][string]$HostLog,
        [Parameter(Mandatory)][string]$JoinLog,
        [int]$TimeoutSec = $BootTimeoutSec,
        [int]$OperationalDrainTimeoutSec = $StepTimeoutSec,
        [int]$QuietSec = 3,
        [switch]$LegacyQuietCheckpointPassed,
        [switch]$SnapshotOnly,
        [object[]]$PersistentPopupWindows = @()
    )
    if ($TimeoutSec -le 0 -or $OperationalDrainTimeoutSec -le 0 -or $QuietSec -le 0) {
        throw 'operational bootstrap observation deadlines must be positive'
    }

    # Scenario loading and the event-popup tail are two causally distinct
    # phases. Give each one fixed observation bounds: the second deadline is
    # armed exactly once by the relay's unique session-operational event and
    # never slides on UI or native-subscriber progress.
    $bootstrapDeadline = (Get-Date).AddSeconds($TimeoutSec)
    $operationalDrainDeadline = $null
    $uiCursor = @{ host = [long]0; join = [long]0 }
    $latestUiObservation = @{ host = $null; join = $null }
    [long]$quietSinceTick = 0
    [long]$requiredBareQuietMilliseconds = if ($LegacyQuietCheckpointPassed) {
        # The old quiet-3 PASS has already been recorded at its literal point.
        # Do not insert a second fixed quiet interval into the MSS extension.
        [long]0
    } else {
        [long]$QuietSec * 1000
    }
    $startupObserverArmed = $true
    $lastEvents = @()

    while ($startupObserverArmed) {
        foreach ($window in @($PersistentPopupWindows)) {
            if ($window) { Invoke-LiteralStartupPopupTick $window }
        }
        $now = Get-Date
        if (($null -eq $operationalDrainDeadline -and $now -ge $bootstrapDeadline) -or
            ($null -ne $operationalDrainDeadline -and $now -ge $operationalDrainDeadline)) {
            break
        }
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        $RelayProcess.Refresh()
        if ($RelayProcess.HasExited) {
            $stderr = if (Test-Path -LiteralPath $script:SimRelayErrorLog) {
                (Get-Content -LiteralPath $script:SimRelayErrorLog -Tail 20) -join ' | '
            } else { '' }
            throw "production relay exited while observing operational bootstrap " +
                  "(code $($RelayProcess.ExitCode)): $stderr"
        }

        $lastEvents = @(Read-SimRelayEvents)
        Assert-NoRelayFault $lastEvents
        [int]$sessionPlanCount = Get-SimEventCount `
            $lastEvents 'session-plan-created'
        [int]$hostPlanDeliveredCount = Get-SimEventCount `
            $lastEvents 'session-plan-delivered' 'host'
        [int]$joinPlanDeliveredCount = Get-SimEventCount `
            $lastEvents 'session-plan-delivered' 'join'
        [int]$hostActivatedCount = Get-SimEventCount `
            $lastEvents 'session-activated' 'host'
        [int]$joinActivatedCount = Get-SimEventCount `
            $lastEvents 'session-activated' 'join'
        [int]$operationalCount = Get-SimEventCount $lastEvents 'session-operational'
        foreach ($count in @(
            $sessionPlanCount,
            $hostPlanDeliveredCount,
            $joinPlanDeliveredCount,
            $hostActivatedCount,
            $joinActivatedCount,
            $operationalCount
        )) {
            if ($count -gt 1) {
                throw 'operational bootstrap published a causal milestone more than once'
            }
        }
        $bothActivated = $sessionPlanCount -eq 1 -and
                         $hostPlanDeliveredCount -eq 1 -and
                         $joinPlanDeliveredCount -eq 1 -and
                         $hostActivatedCount -eq 1 -and
                         $joinActivatedCount -eq 1
        if ($operationalCount -eq 1 -and -not $bothActivated) {
            throw 'session-operational preceded the exact two-client activation witness'
        }
        if ($operationalCount -eq 1 -and $null -eq $operationalDrainDeadline) {
            $operationalDrainDeadline = (Get-Date).AddSeconds($OperationalDrainTimeoutSec)
        }

        # Discovery is append-only. Apply every role event before committing the
        # batch watermark. Sequence gaps are legal because the relay sequence is
        # global while these cursors are role-filtered.
        $sawNewUiEvidence = $false
        foreach ($role in @('host', 'join')) {
            [long]$cursorBefore = $uiCursor[$role]
            $batch = Get-ValidatedUiHistoryBatch -Role $role -After $cursorBefore
            foreach ($observation in @($batch.Observations)) {
                $sawNewUiEvidence = $true
                $latestUiObservation[$role] = $observation
            }
            # Commit only after every event returned for this role was applied.
            $uiCursor[$role] = [long]$batch.LatestSequence
        }

        if ($sawNewUiEvidence) { $quietSinceTick = 0 }
        $bothBare = $true
        foreach ($role in @('host', 'join')) {
            $observation = $latestUiObservation[$role]
            if (-not $observation -or -not $observation.Ready -or
                $script:BareMapDialogs -notcontains $observation.Dialog) {
                $bothBare = $false
            }
        }

        if ($operationalCount -eq 1 -and $bothBare) {
            if ($quietSinceTick -eq 0) {
                $quietSinceTick = [Environment]::TickCount64
            }
            if (([Environment]::TickCount64 - $quietSinceTick) -ge
                    $requiredBareQuietMilliseconds) {
                # Close the observation window on both sides of the comparatively
                # expensive current/world reads. Any newly appended UI evidence
                # keeps the observer armed. The native service remains the sole
                # startup-popup action owner throughout these reads.
                foreach ($window in @($PersistentPopupWindows)) {
                    if ($window) { Invoke-LiteralStartupPopupTick $window }
                }
                $closingHistoryChanged = $false
                foreach ($role in @('host', 'join')) {
                    $closingBatch = Get-ValidatedUiHistoryBatch `
                        -Role $role -After ([long]$uiCursor[$role])
                    if ([long]$closingBatch.LatestSequence -ne [long]$uiCursor[$role]) {
                        $closingHistoryChanged = $true
                    }
                }
                if ($closingHistoryChanged) {
                    $quietSinceTick = 0
                    if ($SnapshotOnly) { break }
                    continue
                }

                $confirmedUi = @{
                    host = Get-FreshDialogObservation host
                    join = Get-FreshDialogObservation join
                }
                $currentBare = $true
                foreach ($role in @('host', 'join')) {
                    $confirmed = $confirmedUi[$role]
                    $latest = $latestUiObservation[$role]
                    if (-not $confirmed -or -not $latest -or
                        -not $confirmed.Ready -or
                        $script:BareMapDialogs -notcontains $confirmed.Dialog -or
                        $confirmed.Dialog -ne $latest.Dialog -or
                        [long]$confirmed.Instance -ne [long]$latest.Instance -or
                        [long]$confirmed.Sequence -ne [long]$latest.Sequence -or
                        [long]$confirmed.Sequence -gt [long]$uiCursor[$role]) {
                        $currentBare = $false
                    }
                }
                if (-not $currentBare) {
                    $quietSinceTick = 0
                    if ($SnapshotOnly) { break }
                    continue
                }

                $hostWorld = Get-World host
                $joinWorld = Get-World join
                if ($null -ne (Get-OptionalProperty $hostWorld 'day') -and
                    $null -ne (Get-OptionalProperty $joinWorld 'day')) {
                    foreach ($window in @($PersistentPopupWindows)) {
                        if ($window) { Invoke-LiteralStartupPopupTick $window }
                    }
                    $finalHistoryChanged = $false
                    foreach ($role in @('host', 'join')) {
                        $finalBatch = Get-ValidatedUiHistoryBatch `
                            -Role $role -After ([long]$uiCursor[$role])
                        if ([long]$finalBatch.LatestSequence -ne [long]$uiCursor[$role]) {
                            $finalHistoryChanged = $true
                        }
                    }
                    if (-not $finalHistoryChanged) {
                        # PowerShell disarms only this finite observer. The native
                        # scripted-popup subscriber stays active session-long.
                        $startupObserverArmed = $false
                        return $lastEvents
                    }
                    $quietSinceTick = 0
                }
            }
        } else {
            $quietSinceTick = 0
        }
        if ($SnapshotOnly) { break }
        Start-Sleep -Milliseconds 250
    }

    $hostDialog = Get-Dialog host
    $joinDialog = Get-Dialog join
    if ($SnapshotOnly) {
        throw ("MSS bootstrap was not operational on two current bare maps at " +
            "the legacy quiet-3 checkpoint (host=$hostDialog join=$joinDialog)")
    }
    $timedOutPhase = if ($null -eq $operationalDrainDeadline) {
        "bootstrap before session-operational (${TimeoutSec}s fixed bound)"
    } else {
        "post-operational startup drain (${OperationalDrainTimeoutSec}s fixed bound)"
    }
    throw ("timed out waiting for $timedOutPhase and quiet maps " +
        "(host=$hostDialog join=$joinDialog)")
}

function Invoke-ParallelButtons([object[]]$Actions,
                                [switch]$RequireExactRolePair,
                                [switch]$RelayHealthAlreadyAsserted) {
    if ($RequireExactRolePair) {
        if ($Actions.Count -ne 2) {
            throw "exact parallel role pair contains $($Actions.Count) actions, expected 2"
        }
        $roles = @($Actions | ForEach-Object { [string]$_.role } | Sort-Object)
        if ($roles.Count -ne 2 -or $roles[0] -ne 'host' -or $roles[1] -ne 'join') {
            throw "exact parallel role pair must contain host and join once"
        }
    }
    if (-not $RelayHealthAlreadyAsserted) {
        Assert-ProductionRelayHealthy
    }
    $relayBase = $script:RelayBase
    return @($Actions | ForEach-Object -Parallel {
        $action = $_
        $started = [DateTime]::UtcNow
        $uri = '{0}/api/ui/invoke?role={1}&dlg={2}&btn={3}&appearance={4}&instance={5}' -f `
            $using:relayBase,
            [uri]::EscapeDataString([string]$action.role),
            [uri]::EscapeDataString([string]$action.dialog),
            [uri]::EscapeDataString([string]$action.button),
            [long]$action.appearance,
            [long]$action.instance
        try {
            $response = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 8
            [pscustomobject]@{ role = $action.role; found = [bool]$response.found; started = $started; error = '' }
        } catch {
            [pscustomobject]@{ role = $action.role; found = $false; started = $started; error = $_.Exception.Message }
        }
    } -ThrottleLimit 2)
}

function New-EndTurnActionFromObservation([string]$Role,
                                          [object]$Observation) {
    $observation = $Observation
    $dialog = if ($observation) { $observation.Dialog } else { '' }
    if (-not $observation -or -not $observation.Ready -or
        $script:BareMapDialogs -notcontains $dialog) {
        throw "$Role is not on an actionable ready strategic map (dialog='$dialog')"
    }
    # DLG_ISO_PAL and DLG_STRATEGIC are co-present, but the action carries the
    # independently reported native owner token for DLG_STRATEGIC.
    [long]$targetInstance = Assert-ReadyButtonSnapshot `
        $observation 'BTN_END_TURN' 'DLG_STRATEGIC'
    return [pscustomobject]@{
        role = $Role
        dialog = 'DLG_STRATEGIC'
        button = 'BTN_END_TURN'
        appearance = $observation.Instance
        instance = $targetInstance
        # Pure transform of the same already-consumed map observation. The
        # subsequent legacy phase may reuse it without another UI/world read.
        mapPreparation = Get-CanonicalWalkPreparationRole $Role $observation
    }
}

function New-CanonicalLegacyHostEndTurnIntent([object]$Snapshot) {
    # The source invoked DLG_STRATEGIC directly after using its one UI read only
    # for the over-rotation observation. Keep the action name-only at this
    # boundary. The relay will resolve one exact native owner from the current
    # or next UI event and issue the command atomically; this constructor never
    # reads readiness, dialog names, targets, or native owners.
    $uiSequenceValue = Get-OptionalProperty $Snapshot 'uiSeq'
    if ($null -eq $uiSequenceValue) {
        throw 'canonical legacy host UI omitted its sequence watermark'
    }
    [long]$uiSequence = $uiSequenceValue
    if ($uiSequence -lt 1 -or $uiSequence -gt [uint32]::MaxValue) {
        throw "canonical legacy host UI sequence is outside uint32: $uiSequence"
    }
    return [pscustomobject]@{
        role = 'host'
        dialog = 'DLG_STRATEGIC'
        button = 'BTN_END_TURN'
        # The relay route is strictly `uiSeq > after`. Subtracting one
        # admits the already-observed current publication without a second UI
        # read; if that publication is a modal, only a later strategic event can
        # satisfy the same one-shot intent.
        afterUiSequence = [long]($uiSequence - 1)
    }
}

function New-EndTurnAction([string]$Role) {
    return New-EndTurnActionFromObservation `
        $Role (Get-DialogObservation $Role)
}

function Invoke-CanonicalPreparedEndTurnPair(
    [object]$Intent,
    [switch]$RelayHealthAlreadyAsserted
) {
    $actions = @($Intent.actions)
    $hostEndTurn = @($actions | Where-Object { [string]$_.role -eq 'host' })
    $joinEndTurn = @($actions | Where-Object { [string]$_.role -eq 'join' })
    if ($actions.Count -ne 2 -or $hostEndTurn.Count -ne 1 -or
        $joinEndTurn.Count -ne 1) {
        throw 'canonical strategic-idle pair must contain host and join exactly once'
    }
    $hostEndTurn = $hostEndTurn[0]
    $joinEndTurn = $joinEndTurn[0]
    foreach ($action in @($hostEndTurn, $joinEndTurn)) {
        if ([string]$action.dialog -ne 'DLG_STRATEGIC' -or
            [string]$action.button -ne 'BTN_END_TURN' -or
            [long]$action.appearance -lt 1 -or [long]$action.instance -lt 1) {
            throw "$($action.role) canonical End Turn lost its exact saved native owner"
        }
    }
    [long]$hostUi = $Intent.uiHostWatermark
    [long]$joinUi = $Intent.uiJoinWatermark
    if ($hostUi -lt 1 -or $joinUi -lt 1) {
        throw 'canonical strategic-idle pair omitted one saved UI watermark'
    }
    if (-not $RelayHealthAlreadyAsserted) {
        Assert-ProductionRelayHealthy
    }

    # One relay-owned event subscription spans the busy-to-idle transition and
    # the sole pair dispatch. There is no read/fire race and no second attempt.
    $uri = ('{0}/api/ui/end-turn-pair-when-strategic-idle?' +
        'hostappearance={1}&hostinstance={2}&hostui={3}&' +
        'joinappearance={4}&joininstance={5}&joinui={6}&' +
        'waitMs=120000&timeoutMs=8000') -f `
        $script:RelayBase,
        [long]$hostEndTurn.appearance,
        [long]$hostEndTurn.instance,
        $hostUi,
        [long]$joinEndTurn.appearance,
        [long]$joinEndTurn.instance,
        $joinUi
    $response = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 135
    if ($response.found -isnot [bool] -or
        $response.host.found -isnot [bool] -or
        $response.join.found -isnot [bool] -or
        -not $response.found -or -not $response.host.found -or
        -not $response.join.found) {
        throw 'canonical strategic-idle pair did not resolve both exact native owners'
    }
    foreach ($expected in @(
        [pscustomobject]@{ role = 'host'; action = $hostEndTurn; actual = $response.host },
        [pscustomobject]@{ role = 'join'; action = $joinEndTurn; actual = $response.join }
    )) {
        if ([string]$expected.actual.role -ne $expected.role -or
            [string]$expected.actual.invoke.dlg -ne 'DLG_STRATEGIC' -or
            [string]$expected.actual.invoke.btn -ne 'BTN_END_TURN' -or
            [long]$expected.actual.invoke.appearance -ne
                [long]$expected.action.appearance -or
            [long]$expected.actual.invoke.instance -ne
                [long]$expected.action.instance -or
            $expected.actual.strategicIdle -isnot [bool] -or
            -not $expected.actual.strategicIdle) {
            throw "$($expected.role) strategic-idle pair result drifted from the saved action"
        }
    }
    [double]$skewMs = $response.dispatchSkewMs
    if ([double]::IsNaN($skewMs) -or [double]::IsInfinity($skewMs) -or $skewMs -lt 0) {
        throw "canonical strategic-idle BTN_END_TURN pair produced invalid dispatch skew '$skewMs'"
    }
    $completedAt = [DateTime]::UtcNow
    Write-Step ("strategic-idle BTN_END_TURN pair dispatch skew={0:N3}ms" -f $skewMs)
    return [pscustomobject]@{
        completedAt = $completedAt
        dispatchSkewMs = $skewMs
        uiHostWatermark = [long]$Intent.uiHostWatermark
        uiJoinWatermark = [long]$Intent.uiJoinWatermark
        beforeEvents = $Intent.beforeEvents
        beforeHostObserved = [int]$Intent.beforeHostObserved
        beforeJoinObserved = [int]$Intent.beforeJoinObserved
        beforeHostApplied = [int]$Intent.beforeHostApplied
        beforeJoinApplied = [int]$Intent.beforeJoinApplied
        beforeHostAccepted = [int]$Intent.beforeHostAccepted
        beforeJoinAccepted = [int]$Intent.beforeJoinAccepted
        expectedHostHandle = [string]$Intent.expectedHostHandle
        expectedJoinHandle = [string]$Intent.expectedJoinHandle
        expectedHostCompletedDay = [int]$Intent.expectedHostCompletedDay
        expectedJoinCompletedDay = [int]$Intent.expectedJoinCompletedDay
    }
}

function Assert-CanonicalNoEndTurnConfirmations([object]$Fire) {
    foreach ($role in @('host', 'join')) {
        [long]$after = if ($role -eq 'host') {
            $Fire.uiHostWatermark
        } else {
            $Fire.uiJoinWatermark
        }
        $history = Get-UiHistory -Role $role -After $after
        $confirmations = @((Get-OptionalProperty $history 'events') | Where-Object {
            (Get-OptionalProperty $_ 'dialog') -eq 'DLG_MESSAGE_BOX'
        })
        if ($confirmations.Count -ne 0) {
            throw "canonical $role End Turn published $($confirmations.Count) confirmation event(s); green path requires zero"
        }
        $current = Get-DialogObservation $role
        if ($current -and $current.Dialog -eq 'DLG_MESSAGE_BOX') {
            throw "canonical $role End Turn is waiting on a forbidden confirmation dialog"
        }
    }
    return 0
}

function Assert-CanonicalRoundCascadeProof([object[]]$Events,
                                           [object]$Fire,
                                           [int]$HostApplyBefore,
                                           [int]$JoinApplyBefore,
                                           [int]$HostActivateBefore,
                                           [int]$JoinActivateBefore,
                                           [int]$HostCompleteBefore,
                                           [int]$JoinCompleteBefore,
                                           [int]$ExpectedDay) {
    $records = @()
    foreach ($role in @('host', 'join')) {
        $applyBefore = if ($role -eq 'host') { $HostApplyBefore } else { $JoinApplyBefore }
        $activateBefore = if ($role -eq 'host') {
            $HostActivateBefore
        } else { $JoinActivateBefore }
        $completeBefore = if ($role -eq 'host') { $HostCompleteBefore } else { $JoinCompleteBefore }
        $expectedHandle = if ($role -eq 'host') {
            [string]$Fire.expectedHostHandle
        } else {
            [string]$Fire.expectedJoinHandle
        }
        $dispatch = Get-NewExactEngineAction `
            $Events 'ordinary-apply' 'host' $expectedHandle $applyBefore $ExpectedDay
        $activations = @(Get-EngineActionMatches `
            $Events 'ordinary-activate' $role $expectedHandle $ExpectedDay)
        if ($activations.Count -ne ($activateBefore + 1)) {
            throw ("canonical ActivateTurn action count for $role is " +
                "$($activations.Count), expected exactly $($activateBefore + 1)")
        }
        $activation = $activations[$activateBefore]
        $completion = Get-NewExactSimEvent $Events 'turn-start-complete' $role $completeBefore
        [long]$actionId = Get-RequiredTelemetryNumber `
            $dispatch 'actionId' "canonical ApplyTurnStart dispatch/$role"
        [long]$activationActionId = Get-RequiredTelemetryNumber `
            $activation 'actionId' "canonical ActivateTurn dispatch/$role"
        [long]$completionActionId = Get-RequiredTelemetryNumber `
            $completion 'actionId' "canonical turn completion/$role"
        [long]$lease = Get-RequiredTelemetryNumber `
            $dispatch 'lease' "canonical turn lease/$role"
        if ($actionId -eq 0 -or $activationActionId -ne $actionId -or
            $completionActionId -ne $actionId -or
            [int](Get-OptionalProperty $dispatch 'kind') -ne 1 -or
            [int](Get-OptionalProperty $activation 'kind') -ne 2 -or
            [long](Get-OptionalProperty $activation 'lease') -ne $lease -or
            [long](Get-OptionalProperty $completion 'lease') -ne $lease -or
            [int](Get-OptionalProperty $completion 'day') -ne $ExpectedDay) {
            throw "canonical cascade $role does not match its handle/day/actionId/lease"
        }
        [int]$dispatchIndex = Get-SimEventRecordIndex $Events $dispatch
        [int]$activationIndex = Get-SimEventRecordIndex $Events $activation
        [int]$completionIndex = Get-SimEventRecordIndex $Events $completion
        if ($dispatchIndex -lt 0 -or $activationIndex -le $dispatchIndex -or
            $completionIndex -le $activationIndex) {
            throw "canonical cascade $role lost ApplyTurnStart -> ActivateTurn -> completion order"
        }
        $records += [pscustomobject]@{
            role = $role
            handle = ConvertTo-CanonicalPlayerHandle `
                $expectedHandle "canonical cascade $role handle"
            day = $ExpectedDay
            actionId = $actionId
            activationActionId = $activationActionId
            completionActionId = $completionActionId
            lease = $lease
            dispatchIndex = $dispatchIndex
            activationIndex = $activationIndex
            completionIndex = $completionIndex
        }
    }
    $ordered = @($records | Sort-Object dispatchIndex)
    if ($ordered.Count -ne 2 -or
        [int]$ordered[0].completionIndex -ge [int]$ordered[1].dispatchIndex -or
        [int]$ordered[1].completionIndex -le [int]$ordered[1].dispatchIndex) {
        throw 'canonical host/join cascades were not serialized completion-before-next-dispatch'
    }
    return [pscustomobject]@{
        cascades = @($records)
        order = @($ordered | ForEach-Object { [string]$_.role })
    }
}

function Invoke-EndTurnsAndWaitAccepted([System.Diagnostics.Process]$RelayProcess,
                                        [int]$TimeoutSec = 30) {
    $before = @(Read-SimRelayEvents)
    $beforeHostObserved = Get-SimEventCount $before 'end-turn-observed' 'host'
    $beforeJoinObserved = Get-SimEventCount $before 'end-turn-observed' 'join'
    $beforeHostApplied = Get-SimEventCount $before 'end-turn-applied' 'host'
    $beforeJoinApplied = Get-SimEventCount $before 'end-turn-applied' 'join'
    $beforeHostAccepted = Get-SimEventCount $before 'end-turn-accepted' 'host'
    $beforeJoinAccepted = Get-SimEventCount $before 'end-turn-accepted' 'join'
    $expectedHostDay = Get-WorldDay host
    $expectedJoinDay = Get-WorldDay join
    if ($null -eq $expectedHostDay -or $null -eq $expectedJoinDay) {
        throw 'world day disappeared before the exact End Turn actions'
    }
    $actions = @(
        (New-EndTurnAction host),
        (New-EndTurnAction join)
    )
    $responses = @(Invoke-ParallelButtons $actions -RequireExactRolePair)
    $bad = @($responses | Where-Object { -not $_.found })
    if ($bad.Count -ne 0) {
        throw "parallel BTN_END_TURN did not resolve for: $((@($bad.role) -join ', '))"
    }
    $times = @($responses | ForEach-Object { [DateTime]$_.started } | Sort-Object)
    $skewMs = if ($times.Count -eq 2) { ($times[1] - $times[0]).TotalMilliseconds } else { $null }
    Write-Step ("parallel BTN_END_TURN dispatch skew={0:N1}ms" -f $skewMs)

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $confirmationSent = @{ host = $false; join = $false }
    while ((Get-Date) -lt $deadline) {
        $RelayProcess.Refresh()
        if ($RelayProcess.HasExited) { throw 'production relay exited while accepting End Turn' }
        $events = @(Read-SimRelayEvents)
        Assert-NoRelayFault $events
        $hostObservedCount = Get-SimEventCount $events 'end-turn-observed' 'host'
        $joinObservedCount = Get-SimEventCount $events 'end-turn-observed' 'join'
        $hostAppliedCount = Get-SimEventCount $events 'end-turn-applied' 'host'
        $joinAppliedCount = Get-SimEventCount $events 'end-turn-applied' 'join'
        $hostAcceptedCount = Get-SimEventCount $events 'end-turn-accepted' 'host'
        $joinAcceptedCount = Get-SimEventCount $events 'end-turn-accepted' 'join'
        if ($hostObservedCount -gt ($beforeHostObserved + 1) -or
            $joinObservedCount -gt ($beforeJoinObserved + 1) -or
            $hostAppliedCount -gt ($beforeHostApplied + 1) -or
            $joinAppliedCount -gt ($beforeJoinApplied + 1) -or
            $hostAcceptedCount -gt ($beforeHostAccepted + 1) -or
            $joinAcceptedCount -gt ($beforeJoinAccepted + 1)) {
            throw 'relay observed more than one End Turn causal signal for a peer'
        }
        $hostAccepted = $hostAcceptedCount -eq ($beforeHostAccepted + 1)
        $joinAccepted = $joinAcceptedCount -eq ($beforeJoinAccepted + 1)
        if (($hostAccepted -and
             ($hostObservedCount -ne ($beforeHostObserved + 1) -or
              $hostAppliedCount -ne ($beforeHostApplied + 1))) -or
            ($joinAccepted -and
             ($joinObservedCount -ne ($beforeJoinObserved + 1) -or
              $joinAppliedCount -ne ($beforeJoinApplied + 1)))) {
            throw 'relay accepted an End Turn before both exact causal signals were present'
        }
        if ($hostAccepted -and $joinAccepted) {
            $hostBarrier = Assert-ExactEndTurnTransaction `
                $events host $beforeHostObserved $beforeHostApplied `
                $beforeHostAccepted $expectedHostDay
            $joinBarrier = Assert-ExactEndTurnTransaction `
                $events join $beforeJoinObserved $beforeJoinApplied `
                $beforeJoinAccepted $expectedJoinDay
            return [pscustomobject]@{
                beforeEvents = $before
                dispatchSkewMs = $skewMs
                beforeHostObserved = $beforeHostObserved
                beforeJoinObserved = $beforeJoinObserved
                beforeHostApplied = $beforeHostApplied
                beforeJoinApplied = $beforeJoinApplied
                beforeHostAccepted = $beforeHostAccepted
                beforeJoinAccepted = $beforeJoinAccepted
                hostBarrier = $hostBarrier
                joinBarrier = $joinBarrier
            }
        }

        # Stock Russobit can publish one exact "units can still move, end turn?"
        # event. Consume that event once with its proven BTN_YES control. A
        # persistent message box remains a terminal failure.
        $confirm = @()
        $hostConfirmation = Get-DialogObservation host
        if (-not $hostAccepted -and -not $confirmationSent.host -and
            $hostConfirmation -and $hostConfirmation.Ready -and
            $hostConfirmation.Dialog -eq 'DLG_MESSAGE_BOX') {
            [long]$confirmationInstance = Assert-ReadyButtonSnapshot `
                $hostConfirmation 'BTN_YES' 'DLG_MESSAGE_BOX'
            $confirmationSent.host = $true
            $confirm += [pscustomobject]@{
                role = 'host'; dialog = 'DLG_MESSAGE_BOX'; button = 'BTN_YES'
                appearance = $hostConfirmation.Instance
                instance = $confirmationInstance
            }
        }
        $joinConfirmation = Get-DialogObservation join
        if (-not $joinAccepted -and -not $confirmationSent.join -and
            $joinConfirmation -and $joinConfirmation.Ready -and
            $joinConfirmation.Dialog -eq 'DLG_MESSAGE_BOX') {
            [long]$confirmationInstance = Assert-ReadyButtonSnapshot `
                $joinConfirmation 'BTN_YES' 'DLG_MESSAGE_BOX'
            $confirmationSent.join = $true
            $confirm += [pscustomobject]@{
                role = 'join'; dialog = 'DLG_MESSAGE_BOX'; button = 'BTN_YES'
                appearance = $joinConfirmation.Instance
                instance = $confirmationInstance
            }
        }
        if ($confirm.Count -gt 0) {
            $confirmResponses = @(Invoke-ParallelButtons $confirm)
            $badConfirm = @($confirmResponses | Where-Object { -not $_.found })
            if ($badConfirm.Count -ne 0) {
                throw "BTN_YES did not resolve for exact message-box event: $((@($badConfirm.role) -join ', '))"
            }
        }
        Start-Sleep -Milliseconds 300
    }
    throw ('both production peers did not accept their single subjective End Turn in time ' +
        "(hostDialog='$(Get-Dialog host)' joinDialog='$(Get-Dialog join)' " +
        "hostConfirmed=$($confirmationSent.host) joinConfirmed=$($confirmationSent.join))")
}

function Invoke-CapturedEndTurnAndWaitAccepted {
    param(
        [Parameter(Mandatory)]
        [System.Diagnostics.Process]$RelayProcess,
        [Parameter(Mandatory)]
        [object]$Action,
        [Parameter(Mandatory)]
        [object]$Baseline,
        [int]$TimeoutSec = 30
    )

    [string]$role = Get-OptionalProperty $Baseline 'role'
    if ($role -notin @('host', 'join') -or
        (Get-OptionalProperty $Action 'role') -ne $role) {
        throw 'captured End Turn intent does not match its evidence role'
    }
    [int]$observedBefore = Get-OptionalProperty $Baseline 'observedBefore'
    [int]$appliedBefore = Get-OptionalProperty $Baseline 'appliedBefore'
    [int]$acceptedBefore = Get-OptionalProperty $Baseline 'acceptedBefore'
    [int]$expectedCompletedDay = Get-OptionalProperty $Baseline 'expectedCompletedDay'
    [long]$uiWatermark = Get-OptionalProperty $Baseline 'uiWatermark'

    Assert-ProductionRelayHealthy
    Write-Step ("merge barrier: invoking captured $role BTN_END_TURN intent once " +
        "for completed day $expectedCompletedDay")
    if (-not (Invoke-Button $Action.role $Action.dialog $Action.button `
                            $Action.instance $Action.appearance)) {
        throw "$role captured BTN_END_TURN intent did not resolve on its subscribed native owner"
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    [long]$confirmationAppearance = 0
    $confirmationSent = $false
    while ((Get-Date) -lt $deadline) {
        $RelayProcess.Refresh()
        if ($RelayProcess.HasExited) {
            throw "production relay exited while accepting the captured $role End Turn"
        }
        $events = @(Read-SimRelayEvents)
        Assert-NoRelayFault $events
        $observedCount = Get-SimEventCount $events 'end-turn-observed' $role
        $appliedCount = Get-SimEventCount $events 'end-turn-applied' $role
        $acceptedCount = Get-SimEventCount $events 'end-turn-accepted' $role
        if ($observedCount -lt $observedBefore -or
            $observedCount -gt ($observedBefore + 1) -or
            $appliedCount -lt $appliedBefore -or $appliedCount -gt ($appliedBefore + 1) -or
            $acceptedCount -lt $acceptedBefore -or $acceptedCount -gt ($acceptedBefore + 1)) {
            throw "relay observed a non-exact End Turn causal delta for $role"
        }
        if ($acceptedCount -eq ($acceptedBefore + 1)) {
            if ($observedCount -ne ($observedBefore + 1) -or
                $appliedCount -ne ($appliedBefore + 1)) {
                throw "relay accepted the captured $role End Turn without both exact causal signals"
            }
            $evidence = Assert-ExactEndTurnTransaction `
                $events $role $observedBefore $appliedBefore `
                $acceptedBefore $expectedCompletedDay
            return [pscustomobject]@{
                role = $role
                evidence = $evidence
                confirmationAppearance = $confirmationAppearance
                confirmationSent = $confirmationSent
            }
        }

        # A stock warning is an append-only UI event downstream of this exact
        # map appearance. Its one BTN_YES intent is consumed before dispatch.
        $uiHistory = Get-UiHistory -Role $role -After $uiWatermark
        $uiEvents = @((Get-OptionalProperty $uiHistory 'events'))
        $confirmation = Get-NewStockConfirmation $uiEvents $Action.appearance
        if ($confirmation) {
            if ($confirmationAppearance -eq 0) {
                $confirmationAppearance = [long]$confirmation.Appearance
            } elseif ($confirmationAppearance -ne [long]$confirmation.Appearance) {
                throw "$role End Turn published a second confirmation appearance"
            }
            if (-not $confirmationSent -and $confirmation.Observation) {
                [long]$confirmationInstance = Assert-ReadyButtonSnapshot `
                    $confirmation.Observation 'BTN_YES' 'DLG_MESSAGE_BOX'
                Assert-ProductionRelayHealthy
                $confirmationSent = $true
                Write-Step ("merge barrier: consuming $role confirmation appearance=" +
                    "$confirmationAppearance once")
                if (-not (Invoke-Button $role DLG_MESSAGE_BOX BTN_YES `
                                        $confirmationInstance $confirmationAppearance)) {
                    throw "$role BTN_YES did not resolve on its exact confirmation event"
                }
            }
        }

        # The HTTP endpoints expose append-only publications rather than a
        # blocking stream; yield only between read-only observation passes.
        Start-Sleep -Milliseconds 250
    }
    throw ("captured $role End Turn was not accepted in time " +
        "(dialog='$(Get-Dialog $role)' confirmationSent=$confirmationSent)")
}

function Invoke-OrderedMergeEndTurnsAndWaitAccepted {
    param(
        [Parameter(Mandatory)]
        [System.Diagnostics.Process]$RelayProcess,
        [Parameter(Mandatory)]
        [ValidateSet('host-first', 'join-first')]
        [string]$Order,
        [Parameter(Mandatory)]
        [int]$ExpectedCompletedDay,
        [int]$TimeoutSec = 30
    )

    $before = @(Read-SimRelayEvents)
    Assert-NoRelayFault $before
    $beforeHostObserved = Get-SimEventCount $before 'end-turn-observed' 'host'
    $beforeJoinObserved = Get-SimEventCount $before 'end-turn-observed' 'join'
    $beforeHostApplied = Get-SimEventCount $before 'end-turn-applied' 'host'
    $beforeJoinApplied = Get-SimEventCount $before 'end-turn-applied' 'join'
    $beforeHostAccepted = Get-SimEventCount $before 'end-turn-accepted' 'host'
    $beforeJoinAccepted = Get-SimEventCount $before 'end-turn-accepted' 'join'
    $barrierBefore = Get-SimEventCount $before 'barrier-held'
    $ordinaryApplyBefore = Get-EngineActionCount $before 'ordinary-apply'
    $ordinaryActivateBefore = Get-EngineActionCount $before 'ordinary-activate'
    $mergeEventBaselines = [ordered]@{
        'merge-prepare-dispatched' = Get-SimEventCount $before 'merge-prepare-dispatched'
        'merge-prepare-applied' = Get-SimEventCount $before 'merge-prepare-applied'
        'merge-execute-dispatched' = Get-SimEventCount $before 'merge-execute-dispatched'
        'merge-execute-applied' = Get-SimEventCount $before 'merge-execute-applied'
        'merge-applied' = Get-SimEventCount $before 'merge-applied'
        'merge-released' = Get-SimEventCount $before 'merge-released'
    }

    # Capture both native owner tokens before either client changes state. Each
    # object is immutable test intent and is submitted at most once below.
    $actions = @{
        host = New-EndTurnAction host
        join = New-EndTurnAction join
    }
    $hostUiHistory = Get-UiHistory -Role host -After 0
    $joinUiHistory = Get-UiHistory -Role join -After 0
    $baselines = @{
        host = [pscustomobject]@{
            role = 'host'
            observedBefore = $beforeHostObserved
            appliedBefore = $beforeHostApplied
            acceptedBefore = $beforeHostAccepted
            expectedCompletedDay = $ExpectedCompletedDay
            uiWatermark = Get-EvidenceWatermark $hostUiHistory 'host UI'
        }
        join = [pscustomobject]@{
            role = 'join'
            observedBefore = $beforeJoinObserved
            appliedBefore = $beforeJoinApplied
            acceptedBefore = $beforeJoinAccepted
            expectedCompletedDay = $ExpectedCompletedDay
            uiWatermark = Get-EvidenceWatermark $joinUiHistory 'join UI'
        }
    }
    $firstRole = if ($Order -eq 'host-first') { 'host' } else { 'join' }
    $secondRole = if ($firstRole -eq 'host') { 'join' } else { 'host' }

    $firstResult = Invoke-CapturedEndTurnAndWaitAccepted `
        -RelayProcess $RelayProcess `
        -Action $actions[$firstRole] `
        -Baseline $baselines[$firstRole] `
        -TimeoutSec $TimeoutSec
    $atBarrier = Wait-SimCondition `
        -RelayProcess $RelayProcess `
        -TimeoutSec $TimeoutSec `
        -Description "$firstRole exact arrival at the merge barrier" `
        -Condition {
            param($events)
            Test-SimEventDeltaReached $events 'barrier-held' $barrierBefore 1
        }
    Assert-SimEventDelta $atBarrier 'end-turn-observed' `
        $baselines[$firstRole].observedBefore 1 $firstRole
    Assert-SimEventDelta $atBarrier 'end-turn-applied' `
        $baselines[$firstRole].appliedBefore 1 $firstRole
    Assert-SimEventDelta $atBarrier 'end-turn-accepted' `
        $baselines[$firstRole].acceptedBefore 1 $firstRole
    Assert-SimEventDelta $atBarrier 'end-turn-observed' `
        $baselines[$secondRole].observedBefore 0 $secondRole
    Assert-SimEventDelta $atBarrier 'end-turn-applied' `
        $baselines[$secondRole].appliedBefore 0 $secondRole
    Assert-SimEventDelta $atBarrier 'end-turn-accepted' `
        $baselines[$secondRole].acceptedBefore 0 $secondRole
    Assert-EngineActionDelta `
        $atBarrier 'ordinary-apply' '' $null $ordinaryApplyBefore 0
    Assert-EngineActionDelta `
        $atBarrier 'ordinary-activate' '' $null $ordinaryActivateBefore 0
    Assert-SimEventDelta $atBarrier 'barrier-held' $barrierBefore 1
    foreach ($eventName in $mergeEventBaselines.Keys) {
        Assert-SimEventDelta $atBarrier $eventName $mergeEventBaselines[$eventName] 0
    }

    $barrierRecord = Get-NewExactSimEvent `
        $atBarrier 'barrier-held' '' $barrierBefore
    [void](Assert-BarrierHeldEvidence `
        $atBarrier $barrierRecord $firstRole $ExpectedCompletedDay)
    $barrierIndex = Get-SimEventRecordIndex $atBarrier $barrierRecord
    if ($barrierIndex -le [int]$firstResult.evidence.acceptedIndex) {
        throw "ordered merge barrier for $firstRole did not follow its exact acceptance"
    }
    Write-Step ("merge barrier: exact $firstRole barrier-held observed; " +
        "submitting the captured $secondRole intent once")

    $secondResult = Invoke-CapturedEndTurnAndWaitAccepted `
        -RelayProcess $RelayProcess `
        -Action $actions[$secondRole] `
        -Baseline $baselines[$secondRole] `
        -TimeoutSec $TimeoutSec
    $afterSecond = @(Read-SimRelayEvents)
    Assert-NoRelayFault $afterSecond
    foreach ($roleName in @('host', 'join')) {
        Assert-SimEventDelta $afterSecond 'end-turn-observed' `
            $baselines[$roleName].observedBefore 1 $roleName
        Assert-SimEventDelta $afterSecond 'end-turn-applied' `
            $baselines[$roleName].appliedBefore 1 $roleName
        Assert-SimEventDelta $afterSecond 'end-turn-accepted' `
            $baselines[$roleName].acceptedBefore 1 $roleName
    }
    [int]$heldAfterSecond = Get-SimEventCount $afterSecond 'barrier-held'
    if ($heldAfterSecond -lt ($barrierBefore + 1) -or
        $heldAfterSecond -gt ($barrierBefore + 2)) {
        throw ('ordered merge observed an impossible barrier-held count ' +
            "$heldAfterSecond immediately after the second acceptance")
    }

    $hostEvidence = if ($firstRole -eq 'host') {
        $firstResult.evidence
    } else {
        $secondResult.evidence
    }
    $joinEvidence = if ($firstRole -eq 'join') {
        $firstResult.evidence
    } else {
        $secondResult.evidence
    }
    return [pscustomobject]@{
        beforeEvents = $before
        dispatchSkewMs = $null
        order = $Order
        barrierRole = $firstRole
        barrierRecord = $barrierRecord
        beforeHostObserved = $beforeHostObserved
        beforeJoinObserved = $beforeJoinObserved
        beforeHostApplied = $beforeHostApplied
        beforeJoinApplied = $beforeJoinApplied
        beforeHostAccepted = $beforeHostAccepted
        beforeJoinAccepted = $beforeJoinAccepted
        hostBarrier = $hostEvidence
        joinBarrier = $joinEvidence
        firstResult = $firstResult
        secondResult = $secondResult
    }
}

function Get-WorldDay([string]$Role) {
    $world = Get-World $Role
    if ($world -and $null -ne $world.day) { return [int]$world.day }
    return $null
}

function Wait-WorldDays([int]$ExpectedHostDay,
                        [int]$ExpectedJoinDay,
                        [int]$TimeoutSec = 90) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $currentHostDay = Get-WorldDay host
        $currentJoinDay = Get-WorldDay join
        if ($currentHostDay -eq $ExpectedHostDay -and
            $currentJoinDay -eq $ExpectedJoinDay) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Get-EvidenceWatermark([object]$History, [string]$Label) {
    $value = Get-OptionalProperty $History 'latestSeq'
    if ($null -eq $value) { throw "$Label history omitted latestSeq" }
    try { [long]$sequence = $value }
    catch { throw "$Label history latestSeq is not an integer: '$value'" }
    if ($sequence -lt 0 -or $sequence -gt [uint32]::MaxValue) {
        throw "$Label history latestSeq is outside uint32: $sequence"
    }
    return $sequence
}

function Get-RequiredTelemetryNumber([object]$Event,
                                     [string]$Field,
                                     [string]$Context) {
    $value = Get-OptionalProperty $Event $Field
    if ($null -eq $value) { throw "$Context omitted '$Field'" }
    try { [long]$number = $value }
    catch { throw "$Context '$Field' is not an integer: '$value'" }
    if ($number -lt 0 -or $number -gt [uint32]::MaxValue) {
        throw "$Context '$Field' is outside uint32: $number"
    }
    return $number
}

function Wait-NewValidatedJoinStockStartupObserved(
    [long]$After,
    [int]$ExpectedDay,
    [long]$ExpectedActiveHandle = 0,
    [int]$TimeoutSec = 120,
    [scriptblock]$OnPoll = $null) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "initial stock startup watermark is outside uint32: $After"
    }
    if ($ExpectedDay -lt 0) {
        throw "initial stock startup day must be non-negative: $ExpectedDay"
    }
    if ($ExpectedActiveHandle -lt 0 -or
        $ExpectedActiveHandle -gt [uint32]::MaxValue) {
        throw "initial stock startup active handle is outside uint32: $ExpectedActiveHandle"
    }

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    [long]$cursor = $After
    $observedEvents = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ProductionRelayHealthy
        if ($OnPoll) { & $OnPoll }
        # Both exact natural messages must have reached this join client before
        # its original Russobit dispatcher. Preserved stock traces prove the
        # causal order Begin(H) then CJoin(H); consume one append-only server-side
        # subscription from its advancing cursor. This never repeats a UI action.
        [int]$remainingMilliseconds = if ($OnPoll) {
            0
        } else {
            [Math]::Max(1, [Math]::Min(120000,
                [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds)))
        }
        $history = Get-TurnHistory `
            -After $cursor -Role join -WaitMilliseconds $remainingMilliseconds
        if ([bool](Get-OptionalProperty $history 'timedOut')) {
            if ($OnPoll) { Start-Sleep -Milliseconds 100; continue }
            throw ("timed out waiting for validated join stock CJoinGame plus " +
                "BeginTurn after $After")
        }
        $newEvents = @((Get-OptionalProperty $history 'events'))
        if ($newEvents.Count -eq 0) {
            if ($OnPoll) { Start-Sleep -Milliseconds 100; continue }
            throw 'turn evidence subscription returned without an event or typed timeout'
        }
        [long]$latestSequence = Get-EvidenceWatermark `
            $history 'initial join stock startup subscription'
        if ($latestSequence -le $cursor) {
            throw ("turn evidence subscription did not advance its cursor " +
                "(cursor=$cursor latest=$latestSequence)")
        }
        foreach ($newEvent in $newEvents) {
            [long]$newSequence = Get-RequiredTelemetryNumber `
                $newEvent 'seq' 'initial join stock startup subscription event'
            if ($newSequence -le $cursor -or $newSequence -gt $latestSequence) {
                throw ("turn evidence event is outside its subscription interval " +
                    "(cursor=$cursor event=$newSequence latest=$latestSequence)")
            }
        }
        $cursor = $latestSequence
        $observedEvents += $newEvents

        $unexpected = @($observedEvents | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -notin @(
                'stock-startup-begin-turn-observed',
                'stock-startup-join-game-observed')
        })
        if ($unexpected.Count -gt 0) {
            $unexpectedKinds = @($unexpected | ForEach-Object {
                [string](Get-OptionalProperty $_ 'kind')
            }) -join ', '
            throw "unexpected join startup evidence after watermark ${After}: $unexpectedKinds"
        }
        $beginEvents = @($observedEvents | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -eq 'stock-startup-begin-turn-observed'
        })
        $joinGameEvents = @($observedEvents | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -eq 'stock-startup-join-game-observed'
        })
        if ($beginEvents.Count -gt 1 -or $joinGameEvents.Count -gt 1) {
            throw ("join startup published duplicate typed evidence after watermark $After " +
                "(begin=$($beginEvents.Count) joinGame=$($joinGameEvents.Count))")
        }
        if ($beginEvents.Count -eq 1 -and $joinGameEvents.Count -eq 1) {
            $beginEvent = $beginEvents[0]
            $joinGameEvent = $joinGameEvents[0]
            $beginRole = [string](Get-OptionalProperty $beginEvent 'role')
            $joinGameRole = [string](Get-OptionalProperty $joinGameEvent 'role')
            if ($beginRole -ne 'join' -or $joinGameRole -ne 'join') {
                throw ("session-start roles are begin=$beginRole joinGame=$joinGameRole; " +
                    'both must be join')
            }

            [long]$beginSequence = Get-RequiredTelemetryNumber `
                $beginEvent 'seq' 'initial join stock BeginTurn'
            [long]$joinGameSequence = Get-RequiredTelemetryNumber `
                $joinGameEvent 'seq' 'initial join stock CJoinGame'
            [long]$newestSequence = [Math]::Max($beginSequence, $joinGameSequence)
            if ($beginSequence -le $After -or $joinGameSequence -le $After -or
                $latestSequence -lt $newestSequence) {
                throw ("initial join startup events are not strictly newer than their subscription " +
                    "(after=$After begin=$beginSequence joinGame=$joinGameSequence " +
                    "latest=$latestSequence)")
            }
            if ($beginSequence -ge $joinGameSequence) {
                throw ("initial join startup evidence violated stock order " +
                    "(begin=$beginSequence joinGame=$joinGameSequence)")
            }

            [long]$senderDpid = Get-RequiredTelemetryNumber `
                $beginEvent 'senderDpid' 'initial join stock BeginTurn'
            [long]$receiverDpid = Get-RequiredTelemetryNumber `
                $beginEvent 'receiverDpid' 'initial join stock BeginTurn'
            [long]$frameLength = Get-RequiredTelemetryNumber `
                $beginEvent 'frameLength' 'initial join stock BeginTurn'
            [long]$addressee = Get-RequiredTelemetryNumber `
                $beginEvent 'addressee' 'initial join stock BeginTurn'
            [long]$commandSequence = Get-RequiredTelemetryNumber `
                $beginEvent 'commandSequence' 'initial join stock BeginTurn'
            [long]$activeHandle = Get-RequiredTelemetryNumber `
                $beginEvent 'activeHandle' 'initial join stock BeginTurn'
            $receiverIsDynamic = (
                $receiverDpid -gt 1 -and
                $receiverDpid -ne 0x00ffffff -and
                $receiverDpid -ne [uint32]::MaxValue)
            $activeMatches = (
                $activeHandle -gt 0 -and
                ($ExpectedActiveHandle -eq 0 -or
                 $activeHandle -eq $ExpectedActiveHandle))
            if ($senderDpid -ne 1 -or -not $receiverIsDynamic -or
                $frameLength -ne 56 -or
                $addressee -ne 0 -or $commandSequence -ne 1 -or
                -not $activeMatches) {
                throw ("initial join stock BeginTurn has wrong natural layout " +
                    "(sender=$senderDpid receiver=$receiverDpid frame=$frameLength " +
                    "addressee=$addressee commandSequence=$commandSequence " +
                    "active=0x$('{0:x8}' -f $activeHandle) " +
                    "expectedActive=0x$('{0:x8}' -f $ExpectedActiveHandle))")
            }

            [long]$joinSenderDpid = Get-RequiredTelemetryNumber `
                $joinGameEvent 'senderDpid' 'initial join stock CJoinGame'
            [long]$joinReceiverDpid = Get-RequiredTelemetryNumber `
                $joinGameEvent 'receiverDpid' 'initial join stock CJoinGame'
            [long]$joinFrameLength = Get-RequiredTelemetryNumber `
                $joinGameEvent 'frameLength' 'initial join stock CJoinGame'
            [long]$joinedHandle = Get-RequiredTelemetryNumber `
                $joinGameEvent 'joinedHandle' 'initial join stock CJoinGame'
            [long]$encodedNameLength = Get-RequiredTelemetryNumber `
                $joinGameEvent 'nameLength' 'initial join stock CJoinGame'
            [long]$raceCategoryId = Get-RequiredTelemetryNumber `
                $joinGameEvent 'raceCategoryId' 'initial join stock CJoinGame'
            [long]$expectedJoinFrameLength = 56 + $encodedNameLength
            if ($joinSenderDpid -ne 1 -or
                $joinReceiverDpid -ne $receiverDpid -or
                $encodedNameLength -lt 1 -or
                $joinFrameLength -ne $expectedJoinFrameLength -or
                $joinedHandle -ne $activeHandle) {
                throw ("initial join stock CJoinGame has wrong natural layout " +
                    "(sender=$joinSenderDpid receiver=$joinReceiverDpid " +
                    "beginReceiver=$receiverDpid frame=$joinFrameLength " +
                    "nameLength=$encodedNameLength raceCategory=$raceCategoryId " +
                    "joined=0x$('{0:x8}' -f $joinedHandle) " +
                    "expectedJoined=0x$('{0:x8}' -f $activeHandle))")
            }
            return [pscustomobject]@{
                BeginTurn = $beginEvent
                JoinGame = $joinGameEvent
                LatestSequence = $latestSequence
            }
        }
    }
    throw ("timed out waiting for validated join stock CJoinGame plus BeginTurn " +
        "after $After")
}

function Wait-NewValidatedJoinStockStartupComplete(
    [long]$After,
    [object]$InitialBegin,
    [object]$InitialHostJoin,
    [int]$TimeoutSec = 120,
    [switch]$SnapshotOnly) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "stock startup-completion watermark is outside uint32: $After"
    }
    if ($null -eq $InitialBegin -or $null -eq $InitialHostJoin) {
        throw 'stock startup completion requires the exact initial witness pair'
    }

    [long]$initialBeginSequence = Get-RequiredTelemetryNumber `
        $InitialBegin 'seq' 'initial startup BeginTurn witness'
    [long]$initialHostJoinSequence = Get-RequiredTelemetryNumber `
        $InitialHostJoin 'seq' 'initial startup host CJoinGame witness'
    [long]$expectedSender = Get-RequiredTelemetryNumber `
        $InitialBegin 'senderDpid' 'initial startup BeginTurn witness'
    [long]$expectedReceiver = Get-RequiredTelemetryNumber `
        $InitialBegin 'receiverDpid' 'initial startup BeginTurn witness'
    [long]$expectedHostHandle = Get-RequiredTelemetryNumber `
        $InitialBegin 'activeHandle' 'initial startup BeginTurn witness'
    [long]$initialJoinedHandle = Get-RequiredTelemetryNumber `
        $InitialHostJoin 'joinedHandle' 'initial startup host CJoinGame witness'
    if ($initialBeginSequence -ge $initialHostJoinSequence -or
        $initialHostJoinSequence -gt $After -or
        $expectedSender -ne 1 -or $expectedReceiver -le 1 -or
        $expectedHostHandle -eq 0 -or
        $initialJoinedHandle -ne $expectedHostHandle) {
        throw 'initial startup witness cannot seed the exact completion subscription'
    }

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    [long]$cursor = $After
    $observedEvents = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ProductionRelayHealthy
        [int]$remainingMilliseconds = if ($SnapshotOnly) {
            0
        } else {
            [Math]::Max(1, [Math]::Min(120000,
                [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds)))
        }
        $history = Get-TurnHistory `
            -After $cursor -Role join -WaitMilliseconds $remainingMilliseconds
        if ([bool](Get-OptionalProperty $history 'timedOut')) {
            if ($SnapshotOnly) { break }
            throw "timed out waiting for exact directed BeginTurn plus join-player CJoinGame after $After"
        }
        $newEvents = @((Get-OptionalProperty $history 'events'))
        if ($newEvents.Count -eq 0) {
            if ($SnapshotOnly) { break }
            throw 'startup-completion subscription returned without an event or typed timeout'
        }
        [long]$latestSequence = Get-EvidenceWatermark `
            $history 'join stock startup-completion subscription'
        if ($latestSequence -le $cursor) {
            throw ("startup-completion subscription did not advance its cursor " +
                "(cursor=$cursor latest=$latestSequence)")
        }
        foreach ($newEvent in $newEvents) {
            [long]$newSequence = Get-RequiredTelemetryNumber `
                $newEvent 'seq' 'join stock startup-completion event'
            if ($newSequence -le $cursor -or $newSequence -gt $latestSequence) {
                throw ("startup-completion event is outside its subscription interval " +
                    "(cursor=$cursor event=$newSequence latest=$latestSequence)")
            }
        }
        $cursor = $latestSequence
        $observedEvents += $newEvents

        $unexpected = @($observedEvents | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -notin @(
                'stock-startup-directed-begin-turn-observed',
                'stock-startup-complete-observed')
        })
        if ($unexpected.Count -gt 0) {
            $unexpectedKinds = @($unexpected | ForEach-Object {
                [string](Get-OptionalProperty $_ 'kind')
            }) -join ', '
            throw "unexpected join startup-completion evidence after ${After}: $unexpectedKinds"
        }

        $directedEvents = @($observedEvents | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -eq `
                'stock-startup-directed-begin-turn-observed'
        })
        $completeEvents = @($observedEvents | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -eq `
                'stock-startup-complete-observed'
        })
        if ($directedEvents.Count -gt 1 -or $completeEvents.Count -gt 1) {
            throw ("join startup completion published duplicate typed evidence " +
                "(directed=$($directedEvents.Count) complete=$($completeEvents.Count))")
        }
        if ($directedEvents.Count -ne 1 -or $completeEvents.Count -ne 1) {
            if ($SnapshotOnly) { break }
            continue
        }

        $directed = $directedEvents[0]
        $complete = $completeEvents[0]
        if ([string](Get-OptionalProperty $directed 'role') -ne 'join' -or
            [string](Get-OptionalProperty $complete 'role') -ne 'join') {
            throw 'both startup-completion transitions must belong to the exact join process'
        }
        [long]$directedSequence = Get-RequiredTelemetryNumber `
            $directed 'seq' 'directed startup BeginTurn'
        [long]$completeSequence = Get-RequiredTelemetryNumber `
            $complete 'seq' 'join-player startup CJoinGame'
        if ($initialHostJoinSequence -ge $directedSequence -or
            $directedSequence -ge $completeSequence) {
            throw ("stock startup evidence violated B(H)<CJoin(H)<D(J,H)<CJoin(J) " +
                "(begin=$initialBeginSequence hostJoin=$initialHostJoinSequence " +
                "directed=$directedSequence complete=$completeSequence)")
        }

        [long]$directedSender = Get-RequiredTelemetryNumber `
            $directed 'senderDpid' 'directed startup BeginTurn'
        [long]$directedReceiver = Get-RequiredTelemetryNumber `
            $directed 'receiverDpid' 'directed startup BeginTurn'
        [long]$directedFrame = Get-RequiredTelemetryNumber `
            $directed 'frameLength' 'directed startup BeginTurn'
        [long]$joinHandle = Get-RequiredTelemetryNumber `
            $directed 'addressee' 'directed startup BeginTurn'
        [long]$directedCommandSequence = Get-RequiredTelemetryNumber `
            $directed 'commandSequence' 'directed startup BeginTurn'
        [long]$directedActiveHandle = Get-RequiredTelemetryNumber `
            $directed 'activeHandle' 'directed startup BeginTurn'
        if ($directedSender -ne $expectedSender -or
            $directedReceiver -ne $expectedReceiver -or
            $directedFrame -ne 56 -or $joinHandle -eq 0 -or
            $joinHandle -eq $expectedHostHandle -or
            $directedCommandSequence -ne [uint32]::MaxValue -or
            $directedActiveHandle -ne $expectedHostHandle) {
            throw 'directed startup BeginTurn does not select one distinct join player under the host'
        }

        [long]$completeSender = Get-RequiredTelemetryNumber `
            $complete 'senderDpid' 'join-player startup CJoinGame'
        [long]$completeReceiver = Get-RequiredTelemetryNumber `
            $complete 'receiverDpid' 'join-player startup CJoinGame'
        [long]$completeFrame = Get-RequiredTelemetryNumber `
            $complete 'frameLength' 'join-player startup CJoinGame'
        [long]$completeJoinedHandle = Get-RequiredTelemetryNumber `
            $complete 'joinedHandle' 'join-player startup CJoinGame'
        [long]$completeNameLength = Get-RequiredTelemetryNumber `
            $complete 'nameLength' 'join-player startup CJoinGame'
        [void](Get-RequiredTelemetryNumber `
            $complete 'raceCategoryId' 'join-player startup CJoinGame')
        if ($completeSender -ne $expectedSender -or
            $completeReceiver -ne $expectedReceiver -or
            $completeNameLength -lt 1 -or
            $completeFrame -ne (56 + $completeNameLength) -or
            $completeJoinedHandle -ne $joinHandle) {
            throw 'join-player CJoinGame does not complete the exact directed startup identity'
        }

        return [pscustomobject]@{
            DirectedBegin = $directed
            JoinComplete = $complete
            LatestSequence = $latestSequence
        }
    }
    if ($SnapshotOnly) {
        throw "exact stock startup completion was absent at the legacy quiet-3 checkpoint after $After"
    }
    throw "timed out waiting for exact stock startup completion after $After"
}

function Convert-ExactPlayerHandle([object]$Value, [string]$Label) {
    if ($null -eq $Value) { throw "$Label is missing" }
    if ($Value -is [string]) {
        $text = [string]$Value
        try {
            if ($text -match '^0[xX][0-9A-Fa-f]{1,8}$') {
                return [long][Convert]::ToUInt32($text.Substring(2), 16)
            }
            if ($text -match '^[0-9A-Fa-f]{8}$' -and $text -match '[A-Fa-f]') {
                return [long][Convert]::ToUInt32($text, 16)
            }
            if ($text -match '^(0|[1-9][0-9]*)$') {
                [long]$number = $text
                if ($number -ge 1 -and $number -le [uint32]::MaxValue) { return $number }
            }
        } catch {
        }
        throw "$Label is not an exact uint32 player handle: '$text'"
    }
    try { [long]$numeric = $Value }
    catch { throw "$Label is not an exact uint32 player handle: '$Value'" }
    if ($numeric -lt 1 -or $numeric -gt [uint32]::MaxValue) {
        throw "$Label is outside the nonzero uint32 player-handle range: $numeric"
    }
    return $numeric
}

function ConvertTo-CanonicalPlayerHandle([object]$Value, [string]$Label) {
    [long]$numeric = Convert-ExactPlayerHandle $Value $Label
    return '0x{0:X8}' -f [uint32]$numeric
}

function Assert-StockTurnEvidencePrefix([object[]]$Events,
                                        [long]$After,
                                        [string]$ExpectedEndRole,
                                        [long]$ExpectedActiveHandle,
                                        [int]$ExpectedDay,
                                        [long]$PreviousCommandSequence = 1) {
    if ($PreviousCommandSequence -lt 1 -or
        $PreviousCommandSequence -ge [uint32]::MaxValue) {
        throw "previous natural BeginTurn command sequence is invalid: $PreviousCommandSequence"
    }
    $records = @{}
    [long]$previousSequence = $After
    foreach ($eventRecord in $Events) {
        [long]$sequence = Get-RequiredTelemetryNumber `
            $eventRecord 'seq' 'stock turn telemetry event'
        if ($sequence -le $After -or $sequence -le $previousSequence) {
            throw "stock turn telemetry sequence is not strictly newer/in-order: $sequence after $previousSequence"
        }
        $previousSequence = $sequence
        if ($records.Count -eq 4) { break }

        $kind = [string](Get-OptionalProperty $eventRecord 'kind')
        $role = [string](Get-OptionalProperty $eventRecord 'role')
        $key = ''
        if ($kind -eq 'stock-end-turn-send-returned') {
            if ($role -ne $ExpectedEndRole) {
                throw "stock End Turn telemetry came from role '$role', expected '$ExpectedEndRole'"
            }
            $key = 'end'
            if ($records.ContainsKey($key)) {
                throw 'stock End Turn telemetry was published more than once before the next turn was applied'
            }
            $frameLength = Get-RequiredTelemetryNumber $eventRecord 'frameLength' 'stock End Turn telemetry'
            $sendResult = Get-RequiredTelemetryNumber $eventRecord 'sendResult' 'stock End Turn telemetry'
            $idTo = Get-RequiredTelemetryNumber $eventRecord 'idTo' 'stock End Turn telemetry'
            if ($frameLength -ne 49 -or $sendResult -ne 1 -or $idTo -le 1) {
                throw "stock End Turn telemetry has wrong native result/layout (frame=$frameLength result=$sendResult idTo=$idTo)"
            }
        } elseif ($kind -eq 'stock-begin-turn-send-returned') {
            if ($role -ne 'host') {
                throw "natural BeginTurn send telemetry came from role '$role', expected authoritative host"
            }
            $key = 'begin-send'
            if ($records.ContainsKey($key)) {
                throw 'natural BeginTurn send telemetry was published more than once before application'
            }
            [long]$idTo = Get-RequiredTelemetryNumber `
                $eventRecord 'idTo' 'natural BeginTurn send telemetry'
            [long]$frameLength = Get-RequiredTelemetryNumber `
                $eventRecord 'frameLength' 'natural BeginTurn send telemetry'
            [long]$sendResult = Get-RequiredTelemetryNumber `
                $eventRecord 'sendResult' 'natural BeginTurn send telemetry'
            [long]$addressee = Get-RequiredTelemetryNumber `
                $eventRecord 'addressee' 'natural BeginTurn send telemetry'
            [long]$commandSequence = Get-RequiredTelemetryNumber `
                $eventRecord 'commandSequence' 'natural BeginTurn send telemetry'
            [long]$activeHandle = Get-RequiredTelemetryNumber `
                $eventRecord 'activeHandle' 'natural BeginTurn send telemetry'
            if ($idTo -ne 0 -or $frameLength -ne 56 -or $sendResult -ne 1 -or
                $addressee -ne 0 -or $commandSequence -le 1 -or
                $commandSequence -eq [uint32]::MaxValue -or
                $activeHandle -ne $ExpectedActiveHandle) {
                throw ("natural BeginTurn send has wrong native result/layout " +
                    "(idTo=$idTo frame=$frameLength result=$sendResult " +
                    "addressee=$addressee commandSequence=$commandSequence " +
                    "active=0x$('{0:x8}' -f $activeHandle))")
            }
        } elseif ($kind -eq 'stock-begin-turn-applied') {
            if ($role -notin @('host', 'join')) {
                throw "stock BeginTurn telemetry came from unknown role '$role'"
            }
            $key = "begin:$role"
            if ($records.ContainsKey($key)) {
                throw "stock BeginTurn telemetry was published more than once for role '$role'"
            }
            if ($role -eq $ExpectedEndRole -and $records.ContainsKey('end')) {
                [long]$endSequence = Get-RequiredTelemetryNumber `
                    $records.end 'seq' 'stock End Turn telemetry'
                if ($endSequence -ge $sequence) {
                    throw 'stock End Turn send result did not precede same-process BeginTurn application'
                }
            }
            $senderDpid = Get-RequiredTelemetryNumber $eventRecord 'senderDpid' "stock BeginTurn/$role"
            $receiverDpid = Get-RequiredTelemetryNumber $eventRecord 'receiverDpid' "stock BeginTurn/$role"
            $frameLength = Get-RequiredTelemetryNumber $eventRecord 'frameLength' "stock BeginTurn/$role"
            $dispatchResult = Get-RequiredTelemetryNumber $eventRecord 'dispatchResult' "stock BeginTurn/$role"
            $addressee = Get-RequiredTelemetryNumber $eventRecord 'addressee' "stock BeginTurn/$role"
            $commandSequence = Get-RequiredTelemetryNumber $eventRecord 'commandSequence' "stock BeginTurn/$role"
            $activeHandle = Get-RequiredTelemetryNumber $eventRecord 'activeHandle' "stock BeginTurn/$role"
            if ($senderDpid -ne 1 -or $receiverDpid -le 1 -or
                $frameLength -ne 56 -or $dispatchResult -le 0 -or
                $addressee -ne 0 -or $commandSequence -le 1 -or
                $commandSequence -eq [uint32]::MaxValue -or
                $activeHandle -ne $ExpectedActiveHandle) {
                throw ("stock BeginTurn/$role has wrong native result/layout " +
                    "(sender=$senderDpid receiver=$receiverDpid frame=$frameLength " +
                    "result=$dispatchResult addressee=$addressee commandSequence=$commandSequence " +
                    "active=0x$('{0:x8}' -f $activeHandle))")
            }
        } else {
            throw "unexpected post-watermark turn telemetry kind '$kind' for role '$role'"
        }
        $records[$key] = $eventRecord
    }

    foreach ($key in @('begin-send', 'begin:host', 'begin:join')) {
        if (-not $records.ContainsKey($key)) {
            return $null
        }
    }
    $hostBegin = $records['begin:host']
    $joinBegin = $records['begin:join']
    $beginSend = $records['begin-send']
    $endRecord = if ($records.ContainsKey('end')) { $records.end } else { $null }
    [long]$hostReceiver = Get-RequiredTelemetryNumber $hostBegin 'receiverDpid' 'stock BeginTurn/host'
    [long]$joinReceiver = Get-RequiredTelemetryNumber $joinBegin 'receiverDpid' 'stock BeginTurn/join'
    if ($hostReceiver -eq $joinReceiver) {
        throw "stock BeginTurn host/join receivers are not distinct ($hostReceiver)"
    }
    if ($endRecord) {
        [long]$endTarget = Get-RequiredTelemetryNumber `
            $endRecord 'idTo' 'stock End Turn telemetry'
        if ($endTarget -ne $joinReceiver) {
            throw "stock End Turn target $endTarget does not equal join BeginTurn receiver $joinReceiver"
        }
    }
    [long]$hostCommandSequence = Get-RequiredTelemetryNumber `
        $hostBegin 'commandSequence' 'stock BeginTurn/host'
    [long]$joinCommandSequence = Get-RequiredTelemetryNumber `
        $joinBegin 'commandSequence' 'stock BeginTurn/join'
    [long]$sendCommandSequence = Get-RequiredTelemetryNumber `
        $beginSend 'commandSequence' 'natural BeginTurn send telemetry'
    if ($hostCommandSequence -ne $joinCommandSequence -or
        $hostCommandSequence -ne $sendCommandSequence) {
        throw ("natural BeginTurn send/host/join copies disagree on broadcast sequence " +
            "($sendCommandSequence/$hostCommandSequence/$joinCommandSequence)")
    }
    if ($hostCommandSequence -le $PreviousCommandSequence) {
        throw ("natural BeginTurn command sequence did not advance its high-water " +
            "($PreviousCommandSequence->$hostCommandSequence)")
    }
    $boundarySequences = @(
        Get-RequiredTelemetryNumber $beginSend 'seq' 'natural BeginTurn send telemetry'
        Get-RequiredTelemetryNumber $hostBegin 'seq' 'stock BeginTurn/host'
        Get-RequiredTelemetryNumber $joinBegin 'seq' 'stock BeginTurn/join'
    )
    if ($endRecord) {
        $boundarySequences += Get-RequiredTelemetryNumber `
            $endRecord 'seq' 'stock End Turn telemetry'
    }
    $boundary = $boundarySequences | Measure-Object -Maximum
    return [pscustomobject]@{
        watermark = [long]$boundary.Maximum
        commandSequence = $hostCommandSequence
        activeHandle = $ExpectedActiveHandle
        day = $ExpectedDay
        end = $endRecord
        beginSend = $beginSend
        hostBegin = $hostBegin
        joinBegin = $joinBegin
    }
}

function Get-NewStockConfirmation([object[]]$UiEvents,
                                  [long]$ActionAppearance) {
    $messageEvents = @($UiEvents | Where-Object {
        (Get-OptionalProperty $_ 'dialog') -eq 'DLG_MESSAGE_BOX'
    })
    if ($messageEvents.Count -eq 0) { return $null }
    $appearances = @($messageEvents | ForEach-Object {
        [long](Get-OptionalProperty $_ 'dialogAppearance')
    } | Sort-Object -Unique)
    if ($appearances.Count -ne 1) {
        throw "stock End Turn produced $($appearances.Count) distinct confirmation appearances"
    }
    [long]$appearance = $appearances[0]
    if ($appearance -le $ActionAppearance -or $appearance -gt [uint32]::MaxValue) {
        throw "stock confirmation appearance $appearance is not newer than action appearance $ActionAppearance"
    }
    $readyEvents = @($messageEvents | Where-Object {
        [long](Get-OptionalProperty $_ 'dialogAppearance') -eq $appearance -and
        (Get-OptionalProperty $_ 'dialogReady') -eq $true
    })
    if ($readyEvents.Count -eq 0) {
        $messageLastSequence = ($messageEvents | ForEach-Object {
            [long](Get-OptionalProperty $_ 'seq')
        } | Measure-Object -Maximum).Maximum
        $laterAppearance = $UiEvents | Where-Object {
            [long](Get-OptionalProperty $_ 'seq') -gt $messageLastSequence -and
            [long](Get-OptionalProperty $_ 'dialogAppearance') -gt $appearance
        } | Select-Object -First 1
        if ($laterAppearance) {
            throw "stock confirmation appearance $appearance closed without a ready publication"
        }
        return [pscustomobject]@{ Appearance = $appearance; Observation = $null }
    }
    $ready = $readyEvents[-1]
    [long]$instance = Get-RequiredTelemetryNumber $ready 'dialogInstance' 'stock confirmation UI event'
    if ($instance -ne $appearance) {
        throw "stock confirmation dialog identity is inconsistent (instance=$instance appearance=$appearance)"
    }
    return [pscustomobject]@{
        Appearance = $appearance
        Observation = [pscustomobject]@{
            State = $ready
            Dialog = 'DLG_MESSAGE_BOX'
            Instance = $appearance
            Ready = $true
        }
    }
}

function Invoke-StockEndTurnAndObserve([string]$Role,
                                       [long]$After,
                                       [long]$ExpectedActiveHandle,
                                       [int]$ExpectedDay,
                                       [long]$PreviousCommandSequence,
                                       [System.Diagnostics.Process]$HostProcess,
                                       [System.Diagnostics.Process]$JoinProcess,
                                       [string]$HostLog,
                                       [string]$JoinLog,
                                       [int]$TimeoutSec = 45) {
    Assert-ClientsLive $HostProcess $JoinProcess
    Assert-NoClientFaults $HostLog $JoinLog
    Assert-ProductionRelayHealthy
    $action = New-EndTurnAction $Role
    [long]$turnWatermark = $After
    if ($turnWatermark -lt 0 -or $turnWatermark -gt [uint32]::MaxValue) {
        throw "stock End Turn watermark is outside uint32: $turnWatermark"
    }
    $uiBaseline = Get-UiHistory -Role $Role -After 0
    [long]$uiWatermark = Get-EvidenceWatermark $uiBaseline 'UI'

    $preActionQuietDeadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $preActionQuietDeadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        Assert-ProductionRelayHealthy
        $preActionHistory = Get-TurnHistory -After $turnWatermark
        $preActionEvents = @((Get-OptionalProperty $preActionHistory 'events'))
        if ($preActionEvents.Count -ne 0) {
            throw 'turn telemetry changed during the subscribed pre-action quiet interval'
        }
        Start-Sleep -Milliseconds 250
    }

    if (-not (Invoke-Button $action.role $action.dialog $action.button `
                            $action.instance $action.appearance)) {
        throw "$Role stock End Turn action did not resolve on its subscribed native owner"
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    [long]$confirmationAppearance = 0
    $confirmationSent = $false
    $quietSince = $null
    while ((Get-Date) -lt $deadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        Assert-ProductionRelayHealthy

        $uiHistory = Get-UiHistory -Role $Role -After $uiWatermark
        $uiEvents = @((Get-OptionalProperty $uiHistory 'events'))
        $confirmation = Get-NewStockConfirmation $uiEvents $action.appearance
        if ($confirmation) {
            if ($confirmationAppearance -eq 0) {
                $confirmationAppearance = [long]$confirmation.Appearance
            } elseif ($confirmationAppearance -ne [long]$confirmation.Appearance) {
                throw "$Role stock End Turn produced a second confirmation appearance"
            }
            if (-not $confirmationSent -and $confirmation.Observation) {
                [long]$confirmationInstance = Assert-ReadyButtonSnapshot `
                    $confirmation.Observation 'BTN_YES' 'DLG_MESSAGE_BOX'
                if (-not (Invoke-Button $Role DLG_MESSAGE_BOX BTN_YES `
                                        $confirmationInstance $confirmation.Appearance)) {
                    throw "$Role stock End Turn confirmation did not resolve on its subscribed event"
                }
                $confirmationSent = $true
            }
        }

        $turnHistory = Get-TurnHistory -After $turnWatermark
        $turnEvents = @((Get-OptionalProperty $turnHistory 'events'))
        if ($turnEvents.Count -gt 4) {
            throw "post-merge $Role action produced $($turnEvents.Count) turn events, expected exactly 4"
        }
        $proof = Assert-StockTurnEvidencePrefix `
            $turnEvents $turnWatermark host $ExpectedActiveHandle $ExpectedDay `
            $PreviousCommandSequence
        if ($proof) {
            $hostDay = Get-WorldDay host
            $joinDay = Get-WorldDay join
            if ($hostDay -ne $ExpectedDay -or $joinDay -ne $ExpectedDay) {
                throw "stock player rotation changed the scenario day unexpectedly ($hostDay/$joinDay, expected $ExpectedDay)"
            }
            if ($confirmation -and -not $confirmationSent) {
                $quietSince = $null
            } elseif ($null -eq $quietSince) {
                $quietSince = Get-Date
            } elseif (((Get-Date) - $quietSince).TotalSeconds -ge 3) {
                return [pscustomobject]@{
                    evidence = $proof
                    actionRole = $Role
                    turnWatermark = [long]$proof.watermark
                    uiWatermark = $uiWatermark
                    confirmationAppearance = $confirmationAppearance
                    confirmationSent = $confirmationSent
                }
            }
        } else {
            $quietSince = $null
        }
        Start-Sleep -Milliseconds 250
    }
    throw "timed out waiting for the exact stock End/Begin applied evidence after the single $Role action"
}

function Assert-StockCycleTailEvidence([object[]]$Events,
                                       [long]$After,
                                       [long]$ExpectedNeutralHandle,
                                       [long]$ExpectedHostHandle,
                                       [int]$ExpectedDay,
                                       [long]$PreviousCommandSequence = 1) {
    if ($PreviousCommandSequence -lt 1 -or
        $PreviousCommandSequence -ge [uint32]::MaxValue) {
        throw "continued stock previous command sequence is invalid: $PreviousCommandSequence"
    }
    if ($Events.Count -gt 8) {
        throw "continued stock cycle produced $($Events.Count) turn events, expected exactly 8"
    }

    $hostEnds = @()
    $beginSendRecords = @{}
    $beginRecords = @{}
    [long]$previousSequence = $After
    foreach ($eventRecord in $Events) {
        [long]$sequence = Get-RequiredTelemetryNumber `
            $eventRecord 'seq' 'continued stock telemetry event'
        if ($sequence -le $After -or $sequence -le $previousSequence) {
            throw "continued stock telemetry sequence is not strictly newer/in-order: $sequence after $previousSequence"
        }
        $previousSequence = $sequence
        $kind = [string](Get-OptionalProperty $eventRecord 'kind')
        $role = [string](Get-OptionalProperty $eventRecord 'role')

        if ($kind -eq 'stock-end-turn-send-returned') {
            if ($role -ne 'host') {
                throw "continued stock End Turn telemetry came from role '$role', expected authoritative host"
            }
            [long]$frameLength = Get-RequiredTelemetryNumber `
                $eventRecord 'frameLength' 'continued stock End Turn telemetry'
            [long]$sendResult = Get-RequiredTelemetryNumber `
                $eventRecord 'sendResult' 'continued stock End Turn telemetry'
            [long]$idTo = Get-RequiredTelemetryNumber `
                $eventRecord 'idTo' 'continued stock End Turn telemetry'
            if ($frameLength -ne 49 -or $sendResult -ne 1 -or $idTo -le 1) {
                throw ("continued stock End Turn has wrong native result/layout " +
                    "(frame=$frameLength result=$sendResult idTo=$idTo)")
            }
            $hostEnds += $eventRecord
            if ($hostEnds.Count -gt 2) {
                throw 'continued stock cycle published more than two authoritative End Turn results'
            }
            if ($hostEnds.Count -eq 2) {
                if (-not $beginRecords.ContainsKey('host:neutral')) {
                    throw 'automatic neutral End Turn preceded host application of the neutral BeginTurn'
                }
                [long]$neutralHostSequence = Get-RequiredTelemetryNumber `
                    $beginRecords['host:neutral'] 'seq' 'host neutral BeginTurn telemetry'
                if ($neutralHostSequence -ge $sequence) {
                    throw 'host causal stream did not order neutral BeginTurn before automatic End Turn'
                }
            }
        } elseif ($kind -eq 'stock-begin-turn-send-returned') {
            if ($role -ne 'host') {
                throw "continued natural BeginTurn send came from role '$role', expected authoritative host"
            }
            [long]$idTo = Get-RequiredTelemetryNumber `
                $eventRecord 'idTo' 'continued natural BeginTurn send'
            [long]$frameLength = Get-RequiredTelemetryNumber `
                $eventRecord 'frameLength' 'continued natural BeginTurn send'
            [long]$sendResult = Get-RequiredTelemetryNumber `
                $eventRecord 'sendResult' 'continued natural BeginTurn send'
            [long]$addressee = Get-RequiredTelemetryNumber `
                $eventRecord 'addressee' 'continued natural BeginTurn send'
            [long]$commandSequence = Get-RequiredTelemetryNumber `
                $eventRecord 'commandSequence' 'continued natural BeginTurn send'
            [long]$activeHandle = Get-RequiredTelemetryNumber `
                $eventRecord 'activeHandle' 'continued natural BeginTurn send'
            if ($idTo -ne 0 -or $frameLength -ne 56 -or $sendResult -ne 1 -or
                $addressee -ne 0 -or $commandSequence -le 1 -or
                $commandSequence -eq [uint32]::MaxValue) {
                throw ("continued natural BeginTurn send has wrong native result/layout " +
                    "(idTo=$idTo frame=$frameLength result=$sendResult " +
                    "addressee=$addressee commandSequence=$commandSequence)")
            }
            $phase = if ($activeHandle -eq $ExpectedNeutralHandle) {
                'neutral'
            } elseif ($activeHandle -eq $ExpectedHostHandle) {
                'returned-host'
            } else {
                throw ("continued natural BeginTurn send selected an unexpected owner " +
                    "(commandSequence=$commandSequence " +
                    "active=0x$('{0:x8}' -f $activeHandle))")
            }
            if ($beginSendRecords.ContainsKey($phase)) {
                throw "continued natural BeginTurn send was published more than once for '$phase'"
            }
            if ($phase -eq 'neutral' -and $hostEnds.Count -ne 1) {
                throw 'natural neutral BeginTurn send did not follow exactly one authoritative End Turn result'
            }
            if ($phase -eq 'returned-host' -and $hostEnds.Count -ne 2) {
                throw 'natural host-return BeginTurn send did not follow exactly two authoritative End Turn results'
            }
            $beginSendRecords[$phase] = $eventRecord
        } elseif ($kind -eq 'stock-begin-turn-applied') {
            if ($role -notin @('host', 'join')) {
                throw "continued stock BeginTurn telemetry came from unknown role '$role'"
            }
            [long]$senderDpid = Get-RequiredTelemetryNumber `
                $eventRecord 'senderDpid' "continued stock BeginTurn/$role"
            [long]$receiverDpid = Get-RequiredTelemetryNumber `
                $eventRecord 'receiverDpid' "continued stock BeginTurn/$role"
            [long]$frameLength = Get-RequiredTelemetryNumber `
                $eventRecord 'frameLength' "continued stock BeginTurn/$role"
            [long]$dispatchResult = Get-RequiredTelemetryNumber `
                $eventRecord 'dispatchResult' "continued stock BeginTurn/$role"
            [long]$addressee = Get-RequiredTelemetryNumber `
                $eventRecord 'addressee' "continued stock BeginTurn/$role"
            [long]$commandSequence = Get-RequiredTelemetryNumber `
                $eventRecord 'commandSequence' "continued stock BeginTurn/$role"
            [long]$activeHandle = Get-RequiredTelemetryNumber `
                $eventRecord 'activeHandle' "continued stock BeginTurn/$role"
            if ($senderDpid -ne 1 -or $receiverDpid -le 1 -or
                $frameLength -ne 56 -or $dispatchResult -le 0 -or
                $addressee -ne 0 -or $commandSequence -le 1 -or
                $commandSequence -eq [uint32]::MaxValue) {
                throw ("continued stock BeginTurn/$role has wrong native result/layout " +
                    "(sender=$senderDpid receiver=$receiverDpid frame=$frameLength " +
                    "result=$dispatchResult addressee=$addressee " +
                    "commandSequence=$commandSequence)")
            }

            $phase = if ($activeHandle -eq $ExpectedNeutralHandle) {
                'neutral'
            } elseif ($activeHandle -eq $ExpectedHostHandle) {
                'returned-host'
            } else {
                throw ("continued stock BeginTurn/$role selected an unexpected owner " +
                    "(commandSequence=$commandSequence " +
                    "active=0x$('{0:x8}' -f $activeHandle))")
            }
            $key = "${role}:$phase"
            if ($beginRecords.ContainsKey($key)) {
                throw "continued stock BeginTurn was published more than once for '$key'"
            }

            if ($role -eq 'host') {
                if ($phase -eq 'neutral') {
                    if ($hostEnds.Count -ne 1) {
                        throw 'host neutral BeginTurn did not follow exactly one authoritative End Turn result'
                    }
                    [long]$firstEndSequence = Get-RequiredTelemetryNumber `
                        $hostEnds[0] 'seq' 'join End Turn telemetry'
                    if ($firstEndSequence -ge $sequence) {
                        throw 'host causal stream did not order join End Turn before neutral BeginTurn'
                    }
                } else {
                    if ($hostEnds.Count -ne 2 -or
                        -not $beginRecords.ContainsKey('host:neutral')) {
                        throw 'host return BeginTurn preceded the complete neutral host-side turn'
                    }
                    [long]$secondEndSequence = Get-RequiredTelemetryNumber `
                        $hostEnds[1] 'seq' 'neutral End Turn telemetry'
                    if ($secondEndSequence -ge $sequence) {
                        throw 'host causal stream did not order neutral End Turn before host return BeginTurn'
                    }
                }
            } elseif ($phase -eq 'returned-host' -and
                      -not $beginRecords.ContainsKey('join:neutral')) {
                throw 'join applied the host return BeginTurn before its neutral BeginTurn'
            }
            $beginRecords[$key] = $eventRecord
        } else {
            throw "unexpected continued stock telemetry kind '$kind' for role '$role'"
        }

        foreach ($peerRole in @('host', 'join')) {
            $neutralKey = "${peerRole}:neutral"
            $returnKey = "${peerRole}:returned-host"
            if ($beginRecords.ContainsKey($neutralKey) -and
                $beginRecords.ContainsKey($returnKey)) {
                [long]$neutralReceiver = Get-RequiredTelemetryNumber `
                    $beginRecords[$neutralKey] 'receiverDpid' "$peerRole neutral BeginTurn"
                [long]$returnReceiver = Get-RequiredTelemetryNumber `
                    $beginRecords[$returnKey] 'receiverDpid' "$peerRole host-return BeginTurn"
                if ($neutralReceiver -ne $returnReceiver) {
                    throw "$peerRole BeginTurn receiver changed inside one stock cycle"
                }
            }
        }
    }

    if ($Events.Count -lt 8) { return $null }
    foreach ($phase in @('neutral', 'returned-host')) {
        if (-not $beginSendRecords.ContainsKey($phase)) {
            throw "continued stock eight-event proof omitted natural BeginTurn send '$phase'"
        }
    }
    foreach ($key in @(
        'host:neutral',
        'join:neutral',
        'host:returned-host',
        'join:returned-host'
    )) {
        if (-not $beginRecords.ContainsKey($key)) {
            throw "continued stock eight-event proof omitted '$key'"
        }
    }
    if ($hostEnds.Count -ne 2) {
        throw "continued stock eight-event proof has $($hostEnds.Count) End Turn results"
    }

    [long]$hostReceiver = Get-RequiredTelemetryNumber `
        $beginRecords['host:neutral'] 'receiverDpid' 'host neutral BeginTurn'
    [long]$joinReceiver = Get-RequiredTelemetryNumber `
        $beginRecords['join:neutral'] 'receiverDpid' 'join neutral BeginTurn'
    if ($hostReceiver -eq $joinReceiver) {
        throw "continued stock host/join receivers are not distinct ($hostReceiver)"
    }
    [long]$hostNeutralCommandSequence = Get-RequiredTelemetryNumber `
        $beginRecords['host:neutral'] 'commandSequence' 'host neutral BeginTurn'
    [long]$joinNeutralCommandSequence = Get-RequiredTelemetryNumber `
        $beginRecords['join:neutral'] 'commandSequence' 'join neutral BeginTurn'
    [long]$hostReturnCommandSequence = Get-RequiredTelemetryNumber `
        $beginRecords['host:returned-host'] 'commandSequence' 'host return BeginTurn'
    [long]$joinReturnCommandSequence = Get-RequiredTelemetryNumber `
        $beginRecords['join:returned-host'] 'commandSequence' 'join return BeginTurn'
    [long]$sendNeutralCommandSequence = Get-RequiredTelemetryNumber `
        $beginSendRecords.neutral 'commandSequence' 'neutral natural BeginTurn send'
    [long]$sendReturnCommandSequence = Get-RequiredTelemetryNumber `
        $beginSendRecords['returned-host'] 'commandSequence' 'host-return natural BeginTurn send'
    if ($hostNeutralCommandSequence -ne $joinNeutralCommandSequence -or
        $hostNeutralCommandSequence -ne $sendNeutralCommandSequence) {
        throw ("continued stock neutral send/host/join copies disagree on command sequence " +
            "($sendNeutralCommandSequence/$hostNeutralCommandSequence/$joinNeutralCommandSequence)")
    }
    if ($hostReturnCommandSequence -ne $joinReturnCommandSequence -or
        $hostReturnCommandSequence -ne $sendReturnCommandSequence) {
        throw ("continued stock return send/host/join copies disagree on command sequence " +
            "($sendReturnCommandSequence/$hostReturnCommandSequence/$joinReturnCommandSequence)")
    }
    if ($hostReturnCommandSequence -le $hostNeutralCommandSequence) {
        throw ("continued stock broadcast command sequence did not advance " +
            "($hostNeutralCommandSequence->$hostReturnCommandSequence)")
    }
    if ($hostNeutralCommandSequence -le $PreviousCommandSequence) {
        throw ("continued stock neutral command sequence did not advance its high-water " +
            "($PreviousCommandSequence->$hostNeutralCommandSequence)")
    }
    foreach ($endRecord in $hostEnds) {
        [long]$endTarget = Get-RequiredTelemetryNumber `
            $endRecord 'idTo' 'continued stock End Turn telemetry'
        if ($endTarget -ne $joinReceiver) {
            throw "continued stock End Turn target $endTarget does not equal join receiver $joinReceiver"
        }
    }
    [long]$joinNeutralSequence = Get-RequiredTelemetryNumber `
        $beginRecords['join:neutral'] 'seq' 'join neutral BeginTurn'
    [long]$joinReturnSequence = Get-RequiredTelemetryNumber `
        $beginRecords['join:returned-host'] 'seq' 'join host-return BeginTurn'
    if ($joinNeutralSequence -ge $joinReturnSequence) {
        throw 'join causal stream did not order neutral BeginTurn before host return BeginTurn'
    }

    return [pscustomobject]@{
        watermark = $previousSequence
        neutral = [pscustomobject]@{
            day = $ExpectedDay
            commandSequence = $hostNeutralCommandSequence
            activeHandle = $ExpectedNeutralHandle
            end = $hostEnds[0]
            beginSend = $beginSendRecords.neutral
            hostBegin = $beginRecords['host:neutral']
            joinBegin = $beginRecords['join:neutral']
        }
        returnedHost = [pscustomobject]@{
            day = $ExpectedDay + 1
            commandSequence = $hostReturnCommandSequence
            activeHandle = $ExpectedHostHandle
            end = $hostEnds[1]
            beginSend = $beginSendRecords['returned-host']
            hostBegin = $beginRecords['host:returned-host']
            joinBegin = $beginRecords['join:returned-host']
        }
    }
}

function Invoke-StockCycleTailAndObserve([long]$After,
                                         [long]$ExpectedNeutralHandle,
                                         [long]$ExpectedHostHandle,
                                         [int]$ExpectedDay,
                                         [long]$PreviousCommandSequence,
                                         [System.Diagnostics.Process]$HostProcess,
                                         [System.Diagnostics.Process]$JoinProcess,
                                         [string]$HostLog,
                                         [string]$JoinLog,
                                         [int]$TimeoutSec = 45) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "continued stock watermark is outside uint32: $After"
    }
    Assert-ClientsLive $HostProcess $JoinProcess
    Assert-NoClientFaults $HostLog $JoinLog
    Assert-ProductionRelayHealthy
    $action = New-EndTurnAction join
    $uiBaseline = Get-UiHistory -Role join -After 0
    [long]$uiWatermark = Get-EvidenceWatermark $uiBaseline 'UI'

    $preActionQuietDeadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $preActionQuietDeadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        Assert-ProductionRelayHealthy
        $preActionHistory = Get-TurnHistory -After $After
        $preActionEvents = @((Get-OptionalProperty $preActionHistory 'events'))
        if ($preActionEvents.Count -ne 0) {
            throw 'continued stock telemetry changed before the subscribed join action'
        }
        Start-Sleep -Milliseconds 250
    }

    if (-not (Invoke-Button $action.role $action.dialog $action.button `
                            $action.instance $action.appearance)) {
        throw 'join stock End Turn action did not resolve on its subscribed native owner'
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    [long]$confirmationAppearance = 0
    $confirmationSent = $false
    $quietSince = $null
    while ((Get-Date) -lt $deadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        Assert-ProductionRelayHealthy

        $uiHistory = Get-UiHistory -Role join -After $uiWatermark
        $uiEvents = @((Get-OptionalProperty $uiHistory 'events'))
        $confirmation = Get-NewStockConfirmation $uiEvents $action.appearance
        if ($confirmation) {
            if ($confirmationAppearance -eq 0) {
                $confirmationAppearance = [long]$confirmation.Appearance
            } elseif ($confirmationAppearance -ne [long]$confirmation.Appearance) {
                throw 'join stock End Turn produced a second confirmation appearance'
            }
            if (-not $confirmationSent -and $confirmation.Observation) {
                [long]$confirmationInstance = Assert-ReadyButtonSnapshot `
                    $confirmation.Observation 'BTN_YES' 'DLG_MESSAGE_BOX'
                if (-not (Invoke-Button join DLG_MESSAGE_BOX BTN_YES `
                                        $confirmationInstance $confirmation.Appearance)) {
                    throw 'join stock End Turn confirmation did not resolve on its subscribed event'
                }
                $confirmationSent = $true
            }
        }

        $turnHistory = Get-TurnHistory -After $After
        $turnEvents = @((Get-OptionalProperty $turnHistory 'events'))
        $proof = Assert-StockCycleTailEvidence `
            $turnEvents $After $ExpectedNeutralHandle $ExpectedHostHandle $ExpectedDay `
            $PreviousCommandSequence
        if ($proof) {
            $expectedWorldDay = $ExpectedDay + 1
            $hostDay = Get-WorldDay host
            $joinDay = Get-WorldDay join
            foreach ($observedDay in @($hostDay, $joinDay)) {
                if ($null -ne $observedDay -and
                    $observedDay -notin @($ExpectedDay, $expectedWorldDay)) {
                    throw "continued stock world reporter published impossible day $observedDay"
                }
            }
            $worldConverged = ($hostDay -eq $expectedWorldDay -and
                               $joinDay -eq $expectedWorldDay)
            if (-not $worldConverged -or
                ($confirmation -and -not $confirmationSent)) {
                $quietSince = $null
            } elseif ($null -eq $quietSince) {
                $quietSince = Get-Date
            } elseif (((Get-Date) - $quietSince).TotalSeconds -ge 3) {
                return [pscustomobject]@{
                    evidence = $proof
                    actionRole = 'join'
                    turnWatermark = [long]$proof.watermark
                    uiWatermark = $uiWatermark
                    confirmationAppearance = $confirmationAppearance
                    confirmationSent = $confirmationSent
                }
            }
        } else {
            $quietSince = $null
        }
        Start-Sleep -Milliseconds 250
    }
    throw 'timed out waiting for the exact eight-event join/neutral stock cycle tail'
}

function Assert-NoStockEndTurnSnapshotAfter([long]$After) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "subjective-turn telemetry watermark is outside uint32: $After"
    }
    $history = Get-TurnHistory -After $After
    [long]$latestSequence = Get-EvidenceWatermark $history 'turn'
    if ($latestSequence -lt $After) {
        throw "turn telemetry watermark regressed from $After to $latestSequence"
    }
    $events = @((Get-OptionalProperty $history 'events'))
    foreach ($eventRecord in $events) {
        [long]$sequence = Get-RequiredTelemetryNumber `
            $eventRecord 'seq' 'subjective-turn telemetry event'
        if ($sequence -le $After) {
            throw "subjective-turn telemetry returned stale sequence $sequence after $After"
        }
        if ((Get-OptionalProperty $eventRecord 'kind') -eq
            'stock-end-turn-send-returned') {
            throw "subjective End Turn leaked into the stock TX path at telemetry sequence $sequence"
        }
    }
    return $latestSequence
}

function Assert-NoStockEndTurnAfter([long]$After,
                                    [System.Diagnostics.Process]$HostProcess,
                                    [System.Diagnostics.Process]$JoinProcess,
                                    [string]$HostLog,
                                    [string]$JoinLog,
                                    [int]$QuietSec = 3) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "subjective-turn telemetry watermark is outside uint32: $After"
    }
    $deadline = (Get-Date).AddSeconds($QuietSec)
    while ((Get-Date) -lt $deadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        Assert-ProductionRelayHealthy
        [void](Assert-NoStockEndTurnSnapshotAfter $After)
        Start-Sleep -Milliseconds 250
    }
}

function Get-NeutralPlayerHandle {
    $hostWorld = Get-World host
    $joinWorld = Get-World join
    $hostNeutral = @($hostWorld.players | Where-Object {
        $_.relation -eq 'neutral' -and $_.human -eq $false
    })
    $joinNeutral = @($joinWorld.players | Where-Object {
        $_.relation -eq 'neutral' -and $_.human -eq $false
    })
    if ($hostNeutral.Count -ne 1 -or $joinNeutral.Count -ne 1 -or
        [string]$hostNeutral[0].id -ne [string]$joinNeutral[0].id) {
        throw "world snapshots do not identify one common neutral player"
    }
    return Convert-ExactPlayerHandle $hostNeutral[0].id 'neutral world player id'
}

function Get-ClientLogBaseline([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $script:ClientLogInitialLengths.ContainsKey($fullPath) -or
        -not $script:ClientLogOwnedProcessIds.ContainsKey($fullPath)) {
        throw "client log '$fullPath' is not bound to a process owned by this run"
    }
    [long]$ownedProcessId = $script:ClientLogOwnedProcessIds[$fullPath]
    if ([IO.Path]::GetFileName($fullPath) -ne "mss32_${ownedProcessId}.log") {
        throw "client log '$fullPath' does not match owned PID $ownedProcessId"
    }
    return [long]$script:ClientLogInitialLengths[$fullPath]
}

function Read-ClientLogLines([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    $fullPath = [IO.Path]::GetFullPath($Path)
    $file = Get-Item -LiteralPath $fullPath -ErrorAction Stop
    $isPerPidClientLog = [IO.Path]::GetFileName($fullPath) -match '^mss32_[0-9]+\.log$'
    $offset = if ($isPerPidClientLog) {
        Get-ClientLogBaseline $fullPath
    } else {
        [long]0
    }
    if ($file.Length -lt $offset) {
        throw "client log '$fullPath' is shorter than its launch boundary $offset"
    }

    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $stream = [IO.FileStream]::new(
        $fullPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
    try {
        [void]$stream.Seek($offset, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new(
            $stream, [Text.Encoding]::UTF8, $true, 4096, $true)
        try { $text = $reader.ReadToEnd() }
        finally { $reader.Dispose() }
    } finally {
        $stream.Dispose()
    }
    if ([string]::IsNullOrEmpty($text)) { return @() }
    $lines = @($text -split '\r?\n')
    if (-not $text.EndsWith("`n", [StringComparison]::Ordinal)) {
        # A concurrently appended spdlog record is not published until its
        # terminating newline is visible. Ignore only that final fragment; an
        # existing observer pass will consume the complete record later.
        if ($lines.Count -le 1) { return @() }
        return @($lines[0..($lines.Count - 2)])
    }
    # PowerShell's -split materializes one synthetic empty element after a
    # terminating delimiter. Select-String -Path in the source harness did not
    # expose that pseudo-line, and retaining it shifts every line-count
    # watermark past the next real appended record. Preserve genuine internal
    # blank lines, but remove exactly this delimiter-created tail.
    if ($lines.Count -le 1) { return @() }
    return @($lines[0..($lines.Count - 2)])
}

function Assert-PairedStartupActionsRelease(
    [int]$HostPid, [int]$JoinPid, [string]$HostLog, [string]$JoinLog
) {
    # This is a one-shot strengthening of the final verdict, not another startup
    # poll/deadline. Retain the exact ready pair and each native receipt ordering.
    if ($HostPid -le 0 -or $JoinPid -le 0 -or $HostPid -eq $JoinPid) {
        throw 'paired startup release requires distinct owned client PIDs'
    }
    $status = Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 10
    $releaseProperty = $status.PSObject.Properties['startupActionsRelease']
    if (-not $releaseProperty -or -not $releaseProperty.Value) {
        throw 'relay did not retain paired startup actions release evidence'
    }
    $release = $releaseProperty.Value
    if (-not $release.PSObject.Properties['t'] -or -not $release.t) {
        throw 'paired startup release is missing its relay timestamp'
    }
    $receipts = [ordered]@{}
    foreach ($client in @(
        @{ role = 'host'; pid = $HostPid; log = $HostLog },
        @{ role = 'join'; pid = $JoinPid; log = $JoinLog }
    )) {
        $role = $client.role
        $pairProperty = $release.PSObject.Properties[$role]
        if (-not $pairProperty -or -not $pairProperty.Value) {
            throw "paired startup release is missing the $role ready identity"
        }
        $pair = $pairProperty.Value
        if (-not $pair.PSObject.Properties['pid'] -or
            -not $pair.PSObject.Properties['uiSeq'] -or
            [long]$pair.pid -ne $client.pid -or [long]$pair.uiSeq -le 0) {
            throw "paired startup release $role identity/sequence does not match the owned client"
        }
        $lines = @(Read-ClientLogLines $client.log)
        $releaseLines = @()
        $firstClaimLine = $null
        for ($index = 0; $index -lt $lines.Count; $index++) {
            if ($lines[$index] -match '\[testdrv\]\[scripted-popup\] paired startup release applied role=(host|join)\b') {
                if ($Matches[1] -cne $role) { throw "wrong-role paired startup receipt in $role log" }
                $releaseLines += ($index + 1)
            }
            if ($null -eq $firstClaimLine -and
                $lines[$index] -match '\[testdrv\]\[scripted-popup\] CLAIMED role=(host|join) dialog=(\S+)') {
                if ($Matches[1] -ceq $role -and $Matches[2] -cne 'DLG_SCENARIO_BRIEFING') {
                    $firstClaimLine = $index + 1
                }
            }
        }
        if ($releaseLines.Count -ne 1) {
            throw "paired startup release native receipt is not exact for $role (count=$($releaseLines.Count))"
        }
        if ($null -ne $firstClaimLine -and $releaseLines[0] -ge $firstClaimLine) {
            throw "non-briefing scripted popup was claimed before paired startup release for $role"
        }
        $receipts[$role] = [ordered]@{
            pid = $client.pid
            log = $client.log
            releaseLine = $releaseLines[0]
            firstNonBriefingClaimLine = $firstClaimLine
        }
    }
    return [pscustomobject]@{
        passed = $true
        observedAt = [DateTimeOffset]::UtcNow.ToString('O')
        relayRelease = $release
        nativeReceipts = $receipts
    }
}

function Get-ClientFaultLines([string]$Path) {
    return @(Read-ClientLogLines $Path | Select-String -Pattern @(
        '\[simturns\] terminal fault:',
        '\[SIMTURNS\] terminal pipe fault:',
        '\[simturns\] preflight mismatch',
        '\[simturns\] detour preflight mismatch',
        '55FC74',
        'midCommandQueue2PushHooked: message with id 21 is rejected due to outdated sequence number'
    ) | ForEach-Object Line)
}

function Assert-NoClientFaults([string]$HostLog, [string]$JoinLog) {
    # Do not add two command expressions here. In PowerShell, `$null + $null`
    # wrapped in @() becomes a one-element array containing `$null`, which made
    # two clean logs look like one empty fault.
    $faults = @(
        Get-ClientFaultLines $HostLog
        Get-ClientFaultLines $JoinLog
    )
    if ($faults.Count -gt 0) {
        throw "client reported production simturn fault: $($faults[0])"
    }
}

function Assert-OperationalActionGate([string]$HostLog, [string]$JoinLog) {
    $events = @(Read-SimRelayEvents)
    Assert-NoRelayFault $events
    Assert-ExactBootstrapOperational $events
    $marker = '[simturns] bootstrap operational release applied; strict independent turns are operational'
    $hostMarkers = @(Read-ClientLogLines $HostLog |
        Select-String -SimpleMatch -Pattern $marker).Count
    $joinMarkers = @(Read-ClientLogLines $JoinLog |
        Select-String -SimpleMatch -Pattern $marker).Count
    if ($hostMarkers -ne 1 -or $joinMarkers -ne 1) {
        throw "native operational gate is not exact (host=$hostMarkers join=$joinMarkers)"
    }
    $battlePatchMarker =
        '[simturns] concurrent-battle compatibility patches active (0x635578=NOP2, 0x638886=NOP7)'
    $hostBattlePatches = @(Read-ClientLogLines $HostLog |
        Select-String -SimpleMatch -Pattern $battlePatchMarker).Count
    $joinBattlePatches = @(Read-ClientLogLines $JoinLog |
        Select-String -SimpleMatch -Pattern $battlePatchMarker).Count
    if ($hostBattlePatches -ne 1 -or $joinBattlePatches -ne 1) {
        throw ("concurrent-battle compatibility patch activation is not exact " +
            "(host=$hostBattlePatches join=$joinBattlePatches)")
    }
}

function Get-ClientLogMarkerCount([string]$Path, [string]$Marker) {
    return @(Read-ClientLogLines $Path |
        Select-String -SimpleMatch -Pattern $Marker).Count
}

function Assert-ClientLogMarkerDelta([string]$Path,
                                     [string]$Marker,
                                     [int]$Before) {
    $actual = Get-ClientLogMarkerCount $Path $Marker
    $expected = $Before + 1
    if ($actual -ne $expected) {
        throw "client log marker '$Marker' count is $actual, expected exact delta +1 from $Before"
    }
}

function Wait-ClientLogMarkerDelta([string]$Path,
                                   [string]$Marker,
                                   [int]$Before,
                                   [int]$TimeoutSec = 30) {
    $expected = $Before + 1
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $actual = Get-ClientLogMarkerCount $Path $Marker
        if ($actual -lt $Before -or $actual -gt $expected) {
            throw "client log marker '$Marker' count is $actual, expected exact delta +1 from $Before"
        }
        if ($actual -eq $expected) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Get-LiteralClientLogMarkerEventUtcSnapshot([string]$Path,
                                                     [string]$Marker,
                                                     [int]$Before) {
    # One passive sample for the host worker's own 100 ms driver cadence. The
    # marker's native timestamp is the event clock; sampling time is not a gate.
    $expected = $Before + 1
    $matches = @(Read-ClientLogLines $Path | Where-Object {
        $_.Contains($Marker, [StringComparison]::Ordinal)
    })
    if ($matches.Count -lt $Before -or $matches.Count -gt $expected) {
        throw "client log marker '$Marker' count is $($matches.Count), expected exact delta +1 from $Before"
    }
    if ($matches.Count -ne $expected) { return $null }
    $line = [string]$matches[$Before]
    if ($line -notmatch
            '^(?<stamp>[0-9]{2}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3})\s') {
        throw "client log marker '$Marker' omitted its exact UTC event timestamp"
    }
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor
        [Globalization.DateTimeStyles]::AdjustToUniversal
    return [pscustomobject]@{
        line = $line
        eventUtc = [DateTime]::ParseExact(
            [string]$Matches.stamp,
            'MM/dd/yy HH:mm:ss.fff',
            [Globalization.CultureInfo]::InvariantCulture,
            $styles)
    }
}

function Wait-ClientLogMarkerEventUtc([string]$Path,
                                      [string]$Marker,
                                      [int]$Before,
                                      [int]$TimeoutSec = 30) {
    $expected = $Before + 1
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        $matches = @(Read-ClientLogLines $Path | Where-Object {
            $_.Contains($Marker, [StringComparison]::Ordinal)
        })
        if ($matches.Count -lt $Before -or $matches.Count -gt $expected) {
            throw "client log marker '$Marker' count is $($matches.Count), expected exact delta +1 from $Before"
        }
        if ($matches.Count -eq $expected) {
            $line = [string]$matches[$Before]
            if ($line -notmatch
                    '^(?<stamp>[0-9]{2}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3})\s') {
                throw "client log marker '$Marker' omitted its exact UTC event timestamp"
            }
            $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor
                [Globalization.DateTimeStyles]::AdjustToUniversal
            $eventUtc = [DateTime]::ParseExact(
                [string]$Matches.stamp,
                'MM/dd/yy HH:mm:ss.fff',
                [Globalization.CultureInfo]::InvariantCulture,
                $styles)
            return [pscustomobject]@{
                line = $line
                eventUtc = $eventUtc
            }
        }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Wait-ExactLocalTurnReleasePair([string]$HostLog,
                                        [string]$JoinLog,
                                        [long]$HostHandle,
                                        [long]$JoinHandle,
                                        [long]$HostActionId,
                                        [long]$JoinActionId,
                                        [int]$Day,
                                        [DateTime]$FireCompletedAt,
                                        [int]$TimeoutSec = 45) {
    if ($HostHandle -lt 1 -or $HostHandle -gt [uint32]::MaxValue -or
        $JoinHandle -lt 1 -or $JoinHandle -gt [uint32]::MaxValue -or
        $HostHandle -eq $JoinHandle -or
        $HostActionId -le 0 -or $JoinActionId -le 0 -or
        $HostActionId -eq $JoinActionId) {
        throw 'exact local turn release requires two distinct nonzero uint32 handles'
    }
    if ($Day -lt 1 -or $TimeoutSec -lt 1 -or
        $FireCompletedAt.Kind -ne [DateTimeKind]::Utc) {
        throw 'exact local turn release requires a positive day, timeout, and UTC fire clock'
    }

    $hostMarker = ('[simturns] ActivateTurn UI queue drained (actionId={0}, day={1})' -f
        $HostActionId, $Day)
    $joinMarker = ('[simturns] ActivateTurn UI queue drained (actionId={0}, day={1})' -f
        $JoinActionId, $Day)
    $deadlineUtc = $FireCompletedAt.AddSeconds($TimeoutSec)
    while ($true) {
        $hostEvent = Get-LiteralClientLogMarkerEventUtcSnapshot $HostLog $hostMarker 0
        $joinEvent = Get-LiteralClientLogMarkerEventUtcSnapshot $JoinLog $joinMarker 0
        foreach ($sample in @(
            @{ role = 'host'; event = $hostEvent },
            @{ role = 'join'; event = $joinEvent }
        )) {
            if ($sample.event -and [DateTime]$sample.event.eventUtc -gt $deadlineUtc) {
                throw ("$($sample.role) exact local turn release was published after " +
                    "the shared fire-anchored deadline $($deadlineUtc.ToString('O'))")
            }
        }
        if ($hostEvent -and $joinEvent) {
            [double]$applySkewMs = [Math]::Abs(
                ([DateTime]$hostEvent.eventUtc - [DateTime]$joinEvent.eventUtc).TotalMilliseconds)
            return [pscustomobject]@{
                day = $Day
                deadlineUtc = $deadlineUtc
                hostActionId = $HostActionId
                joinActionId = $JoinActionId
                host = $hostEvent
                join = $joinEvent
                applySkewMs = $applySkewMs
            }
        }
        if ([DateTime]::UtcNow -ge $deadlineUtc) {
            throw ("exact local turn release pair was incomplete at day $Day by " +
                "$($deadlineUtc.ToString('O')) (host=$([bool]$hostEvent) join=$([bool]$joinEvent))")
        }
        Start-Sleep -Milliseconds 250
    }
}

function New-LiteralPreparedEndTurnIntent(
    [object[]]$BeforeEvents,
    [int]$CompletedDay,
    [object]$PreparedMetadata,
    [object]$MovementSnapshot) {
    # Pure transform of observations already made by this End Turn phase. The
    # old sync_endturn script issued BTN_END_TURN without another UI read, so the
    # exact DLG_STRATEGIC owner and appearance come from this phase's peerState.
    Assert-NoRelayFault $BeforeEvents
    $sessionPlan = Get-SessionPlanEvent $BeforeEvents
    $actions = @()
    foreach ($role in @('host', 'join')) {
        $saved = $PreparedMetadata.$role
        $worldSequenceValue = Get-OptionalProperty $saved 'worldAfter'
        if (-not $saved -or [long]$saved.dialogAppearance -lt 1 -or
            [long]$saved.endTurnInstance -lt 1 -or [long]$saved.uiAfter -lt 1 -or
            $null -eq $worldSequenceValue -or [long]$worldSequenceValue -lt 0) {
            throw "$role prepared End Turn intent omitted exact saved map identity/watermark"
        }
        $actions += [pscustomobject]@{
            role = $role
            dialog = 'DLG_STRATEGIC'
            button = 'BTN_END_TURN'
            appearance = [long]$saved.dialogAppearance
            instance = [long]$saved.endTurnInstance
        }
    }
    return [pscustomobject]@{
        actions = $actions
        uiHostWatermark = [long]$PreparedMetadata.host.uiAfter
        uiJoinWatermark = [long]$PreparedMetadata.join.uiAfter
        beforeEvents = $BeforeEvents
        beforeHostObserved = Get-SimEventCount `
            $BeforeEvents 'end-turn-observed' 'host'
        beforeJoinObserved = Get-SimEventCount `
            $BeforeEvents 'end-turn-observed' 'join'
        beforeHostApplied = Get-SimEventCount $BeforeEvents 'end-turn-applied' 'host'
        beforeJoinApplied = Get-SimEventCount $BeforeEvents 'end-turn-applied' 'join'
        beforeHostAccepted = Get-SimEventCount $BeforeEvents 'end-turn-accepted' 'host'
        beforeJoinAccepted = Get-SimEventCount $BeforeEvents 'end-turn-accepted' 'join'
        expectedHostHandle = [string](Get-OptionalProperty $sessionPlan 'hostHandle')
        expectedJoinHandle = [string](Get-OptionalProperty $sessionPlan 'joinHandle')
        expectedHostCompletedDay = $CompletedDay
        expectedJoinCompletedDay = $CompletedDay
    }
}

function Run-IndependentRound([int]$Round,
                              [System.Diagnostics.Process]$RelayProcess,
                              [System.Diagnostics.Process]$HostProcess,
                              [System.Diagnostics.Process]$JoinProcess,
                              [string]$HostLog,
                              [string]$JoinLog,
                              [object]$Fixture = $null,
                              [object]$PreparedCanonicalEvidence = $null,
                              [scriptblock]$OnCanonicalLegacyPass = $null) {
    $legacyVitalsBefore = $null
    $legacyUnitVitalsBefore = $null
    $legacyHostLogWatermark = $null
    $turnWatermark = [long]0
    $canonicalIntent = $null
    if ($Fixture) {
        if (-not $PreparedCanonicalEvidence -or
            -not $PreparedCanonicalEvidence.eventsBefore) {
            throw "round ${Round}: canonical End Turn omitted predecessor event evidence"
        }
        $preparedTurnWatermark = Get-OptionalProperty `
            $PreparedCanonicalEvidence 'stockTurnWatermark'
        if ($null -eq $preparedTurnWatermark -or
            [long]$preparedTurnWatermark -le 0) {
            throw "round ${Round}: canonical End Turn omitted the saved startup turn watermark"
        }
        $turnWatermark = [long]$preparedTurnWatermark
        # sync_endturn.ps1 exact success-path prelude:
        # process -> stacks reachability -> one peer-state census -> HeroId H ->
        # HeroId J -> one host-hook line watermark -> MP H -> MP J -> HP H -> HP J.
        $ownedProcessIds = @([int]$HostProcess.Id, [int]$JoinProcess.Id)
        $ownedProcessesBefore = @(Get-Process `
            -Id $ownedProcessIds -ErrorAction SilentlyContinue)
        if ($ownedProcessesBefore.Count -ne 2) {
            throw "round ${Round}: source precondition found $($ownedProcessesBefore.Count)/2 owned processes"
        }
        $stackReachability = Get-LegacyStackSnapshot
        if (-not $stackReachability -or
            @((Get-OptionalProperty $stackReachability 'stacks')).Count -lt 2) {
            throw "round ${Round}: shared stack reporter is unreachable"
        }
        $peerState = Get-RelayState
        Assert-DebugRelayClientIdentity `
            host (Get-OptionalProperty $peerState 'host') $HostProcess
        Assert-DebugRelayClientIdentity `
            join (Get-OptionalProperty $peerState 'join') $JoinProcess
        # sync_endturn already made this one aggregate /api/state read. Project
        # the two exact current map capabilities from it in memory; a historical
        # post-walk generation is evidence only and must never address a button.
        $roundActionPreparation = New-CanonicalWalkPreparationFromRelayState `
            $peerState $HostProcess $JoinProcess
        $hostHeroCensus = Get-LegacyStackSnapshot
        [void](Get-WorldStackExact `
            $hostHeroCensus ([string]$Fixture.host.heroId))
        $joinHeroCensus = Get-LegacyStackSnapshot
        [void](Get-WorldStackExact `
            $joinHeroCensus ([string]$Fixture.join.heroId))
        $legacyHostLogWatermark = @(Read-ClientLogLines $HostLog).Count

        $legacyVitalsBefore = Get-LegacySequentialSharedMovementSnapshot $Fixture
        # sync_endturn.ps1 printed the two current values but never required
        # one recorded 12/5 outcome.  Auto-battle may leave the join leader
        # alive (the old green massFIX run entered here at 12/8); the source
        # contract is the preceding walk's moved+charged verdict followed by
        # this exact two-read observation and a refresh to 35/35.
        # HpLines scanned the authoritative host hook once per hero. The MSS
        # equivalent is two distinct raw host-world reads, never one combined
        # census: first retain host unit HP, then retain join unit HP.
        $legacyHostUnitVitalsBefore = Get-CanonicalHeroUnitVitals $Fixture host
        $legacyJoinUnitVitalsBefore = Get-CanonicalHeroUnitVitals $Fixture host
        $legacyUnitVitalsBefore = [pscustomobject]@{
            host = $legacyHostUnitVitalsBefore.host
            join = $legacyJoinUnitVitalsBefore.join
        }

        $hostBefore = [int]$PreparedCanonicalEvidence.completedDay
        $joinBefore = [int]$PreparedCanonicalEvidence.completedDay
        if ($hostBefore -ne $Round -or $joinBefore -ne $Round) {
            throw "round ${Round}: saved completed day is $hostBefore/$joinBefore"
        }
        $eventsBefore = @($PreparedCanonicalEvidence.eventsBefore)
        $sessionPlan = Get-SessionPlanEvent $eventsBefore
        $expectedHostHandle = [string](Get-OptionalProperty $sessionPlan 'hostHandle')
        $expectedJoinHandle = [string](Get-OptionalProperty $sessionPlan 'joinHandle')
        $applyHostBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-apply' 'host' $expectedHostHandle
        $applyJoinBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-apply' 'host' $expectedJoinHandle
        $activateHostBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-activate' 'host' $expectedHostHandle
        $activateJoinBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-activate' 'join' $expectedJoinHandle
        $completeHostBefore = Get-SimEventCount $eventsBefore 'turn-start-complete' 'host'
        $completeJoinBefore = Get-SimEventCount $eventsBefore 'turn-start-complete' 'join'
        $canonicalIntent = New-LiteralPreparedEndTurnIntent `
            $eventsBefore $hostBefore $roundActionPreparation `
            $legacyVitalsBefore
        $hostEndTurnAction = @($canonicalIntent.actions | Where-Object role -eq 'host')
        $joinEndTurnAction = @($canonicalIntent.actions | Where-Object role -eq 'join')
        if ($hostEndTurnAction.Count -ne 1 -or $joinEndTurnAction.Count -ne 1) {
            throw "round ${Round}: exact End Turn intent omitted one current map preparation"
        }
        Write-Step ("round ${Round}: legacy MP BEFORE=" +
            "$($legacyVitalsBefore.host.movement)/$($legacyVitalsBefore.join.movement)")
        Write-Step ("round ${Round}: legacy HP BEFORE host units: " +
            (Format-CanonicalUnitHp $legacyUnitVitalsBefore.host))
        Write-Step ("round ${Round}: legacy HP BEFORE join units: " +
            (Format-CanonicalUnitHp $legacyUnitVitalsBefore.join))
    } else {
        Assert-OperationalActionGate $HostLog $JoinLog
        if (-not (Wait-ActionablePair)) {
            throw "round ${Round}: both clients did not expose a stable strategic map"
        }
        $hostBefore = Get-WorldDay host
        $joinBefore = Get-WorldDay join
        if ($null -eq $hostBefore -or $null -eq $joinBefore) {
            throw "round ${Round}: world day unavailable"
        }
        $turnBaseline = Get-TurnHistory -After 0
        $turnWatermark = Get-EvidenceWatermark $turnBaseline 'turn'
        $eventsBefore = @(Read-SimRelayEvents)
        $sessionPlan = Get-SessionPlanEvent $eventsBefore
        $expectedHostHandle = [string](Get-OptionalProperty $sessionPlan 'hostHandle')
        $expectedJoinHandle = [string](Get-OptionalProperty $sessionPlan 'joinHandle')
        $applyHostBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-apply' 'host' $expectedHostHandle
        $applyJoinBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-apply' 'host' $expectedJoinHandle
        $activateHostBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-activate' 'host' $expectedHostHandle
        $activateJoinBefore = Get-EngineActionCount `
            $eventsBefore 'ordinary-activate' 'join' $expectedJoinHandle
        $completeHostBefore = Get-SimEventCount $eventsBefore 'turn-start-complete' 'host'
        $completeJoinBefore = Get-SimEventCount $eventsBefore 'turn-start-complete' 'join'
    }

    Write-Step "round ${Round}: parallel subjective End Turn from local days host=$hostBefore join=$joinBefore"
    $fire = if ($Fixture) {
        Invoke-CanonicalPreparedEndTurnPair `
            $canonicalIntent -RelayHealthAlreadyAsserted
    } else {
        Invoke-EndTurnsAndWaitAccepted $RelayProcess
    }
    $legacyMovementRefresh = if ($Fixture) {
        # Exact sync_endturn.ps1 schedule: one 45-second clock anchored when
        # both action POSTs returned; reads only at +3/+6/... and no repeated
        # action or independent relay-acceptance timeout.
        Wait-CanonicalLegacyEndTurnRefresh `
            $Fixture $HostProcess $JoinProcess $fire.completedAt 45
    } else { $null }
    $canonicalCascadeProof = $null
    $canonicalHostBarrier = $null
    $canonicalJoinBarrier = $null
    $canonicalConfirmationCount = $null
    $legacyVitalsAfter = $null
    $legacyUnitVitalsAfter = $null
    $trackedProcessesAlive = 0
    $roundEvents = $null
    $legacyContinuation = $null
    $localTurnRelease = $null
    $deferredMssEvidence = $null
    $legacyPass = $null
    if ($Fixture) {
        # Literal ASSERTS section: one authoritative host-hook scan after the
        # saved line watermark, then one stock-TX scan, then one exact-two PID
        # census. Production relay-event evidence is deferred past BARRIER PASS.
        $hostLinesAfter = @(Read-ClientLogLines $HostLog)
        if ($hostLinesAfter.Count -lt [int]$legacyHostLogWatermark) {
            throw "round ${Round}: host hook log regressed below its saved watermark"
        }
        $newHostLines = if ($hostLinesAfter.Count -eq [int]$legacyHostLogWatermark) {
            @()
        } else {
            @($hostLinesAfter[[int]$legacyHostLogWatermark..($hostLinesAfter.Count - 1)])
        }
        $cascadePattern =
            ('\[simturns\] ApplyTurnStart (?<actionId>[0-9]+) completed for ' +
             'handle=(?<handle>0x[0-9a-fA-F]+), day=(?<day>[0-9]+), ' +
             'lease=(?<lease>[0-9]+)')
        $legacyCascadeLines = @($newHostLines | Where-Object { $_ -match $cascadePattern })
        if ($legacyCascadeLines.Count -ne 2) {
            throw "round ${Round}: host hook published $($legacyCascadeLines.Count) new cascades, expected 2"
        }
        $cascadeRecords = @($legacyCascadeLines | ForEach-Object {
            if ($_ -notmatch $cascadePattern -or
                [long]$Matches.actionId -le 0 -or [long]$Matches.lease -le 0 -or
                [int]$Matches.day -ne ($hostBefore + 1)) {
                throw "round ${Round}: host cascade line changed its exact day/layout: $_"
            }
            [pscustomobject]@{
                actionId = [long]$Matches.actionId
                handle = Convert-ExactPlayerHandle `
                    ([string]$Matches.handle) 'host cascade handle'
                day = [int]$Matches.day
                lease = [long]$Matches.lease
            }
        })
        [long]$fixtureHostHandle = Convert-ExactPlayerHandle `
            $Fixture.players.hostHandle 'fixture host handle'
        [long]$fixtureJoinHandle = Convert-ExactPlayerHandle `
            $Fixture.players.joinHandle 'fixture join handle'
        if (@($cascadeRecords | Where-Object { $_.handle -eq $fixtureHostHandle }).Count -ne 1 -or
            @($cascadeRecords | Where-Object { $_.handle -eq $fixtureJoinHandle }).Count -ne 1) {
            throw "round ${Round}: host hook did not publish one cascade per exact player handle"
        }
        [void](Assert-NoStockEndTurnSnapshotAfter 0)
        $ownedProcessIds = @([int]$HostProcess.Id, [int]$JoinProcess.Id)
        $trackedProcessesAlive = @(
            Get-Process -Id $ownedProcessIds -ErrorAction SilentlyContinue).Count
        if ($trackedProcessesAlive -ne 2) {
            throw "round ${Round}: tracked process count is $trackedProcessesAlive/2"
        }

        # Exact legacy order after +4: HP first, then two independent shared
        # MP reads for the final verdict. No MSS telemetry read is inserted.
        Start-Sleep -Seconds 4
        $legacyHostUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host
        $legacyJoinUnitVitalsAfter = Get-CanonicalHeroUnitVitals $Fixture host
        $legacyUnitVitalsAfter = [pscustomobject]@{
            host = $legacyHostUnitVitalsAfter.host
            join = $legacyJoinUnitVitalsAfter.join
        }
        Write-Step ("round ${Round}: legacy HP AFTER host units: " +
            (Format-CanonicalUnitHp $legacyUnitVitalsAfter.host))
        Write-Step ("round ${Round}: legacy HP AFTER join units: " +
            (Format-CanonicalUnitHp $legacyUnitVitalsAfter.join))
        $legacyVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture
        if ([int]$legacyVitalsAfter.host.movement -ne 35 -or
            [int]$legacyVitalsAfter.join.movement -ne 35) {
            throw ("round ${Round}: legacy End Turn MP verdict is " +
                "$($legacyVitalsAfter.host.movement)/$($legacyVitalsAfter.join.movement), expected 35/35")
        }
        Write-Step ("round ${Round}: legacy MP AFTER={0}/{1}" -f
            $legacyVitalsAfter.host.movement, $legacyVitalsAfter.join.movement)
        Write-Step "round $Round PASS: simultaneous End Turn refreshed both MPs, one cascade each, no stock advance, both processes alive"
        $legacyPass = $true

        # The old run_test.ps1 invokes the day-2 reverse walk after this PASS.
        # MSS additionally proves the exact native BeginTurn release on both
        # clients. This is one passive, fire-anchored bridge; the walk remains
        # one exact action pair and retains its source +3 opening pause.
        $hostNativeApply = @($cascadeRecords | Where-Object {
            [long]$_.handle -eq $fixtureHostHandle
        })[0]
        $joinNativeApply = @($cascadeRecords | Where-Object {
            [long]$_.handle -eq $fixtureJoinHandle
        })[0]
        $localTurnRelease = Wait-ExactLocalTurnReleasePair `
            -HostLog $HostLog -JoinLog $JoinLog `
            -HostHandle $fixtureHostHandle -JoinHandle $fixtureJoinHandle `
            -HostActionId ([long]$hostNativeApply.actionId) `
            -JoinActionId ([long]$joinNativeApply.actionId) `
            -Day ($hostBefore + 1) -FireCompletedAt $fire.completedAt -TimeoutSec 45
        Write-Step (("round ${Round}: exact local BeginTurn release PASS at day {0} " +
            "(host={1:O}, join={2:O}, skew={3:N1}ms)") -f
            $localTurnRelease.day,
            [DateTime]$localTurnRelease.host.eventUtc,
            [DateTime]$localTurnRelease.join.eventUtc,
            [double]$localTurnRelease.applySkewMs)
        if ($OnCanonicalLegacyPass) {
            $legacyContinuation = & $OnCanonicalLegacyPass `
                $Fixture $HostProcess $JoinProcess
        }
        # Package only in-memory causal baselines. The next observable after the
        # day-2 walk PASS must be barrier_endturn.ps1's process precondition.
        $deferredMssEvidence = [pscustomobject]@{
            eventsBefore = @($eventsBefore)
            stockTurnWatermark = $turnWatermark
            fire = $fire
            applyHostBefore = $applyHostBefore
            applyJoinBefore = $applyJoinBefore
            activateHostBefore = $activateHostBefore
            activateJoinBefore = $activateJoinBefore
            completeHostBefore = $completeHostBefore
            completeJoinBefore = $completeJoinBefore
            completedDay = $hostBefore
            nativeApplyActions = @($cascadeRecords)
            localTurnRelease = $localTurnRelease
        }
    } else {
        [void](Wait-SimCondition -RelayProcess $RelayProcess -Description "two cascades and BeginTurn completions for round $Round" -Condition {
            param($events)
            $complete = $true
            if (-not (Test-EngineActionDeltaReached `
                    $events 'ordinary-apply' 'host' $expectedHostHandle `
                    $applyHostBefore 1)) { $complete = $false }
            if (-not (Test-EngineActionDeltaReached `
                    $events 'ordinary-apply' 'host' $expectedJoinHandle `
                    $applyJoinBefore 1)) { $complete = $false }
            if (-not (Test-EngineActionDeltaReached `
                    $events 'ordinary-activate' 'host' $expectedHostHandle `
                    $activateHostBefore 1)) { $complete = $false }
            if (-not (Test-EngineActionDeltaReached `
                    $events 'ordinary-activate' 'join' $expectedJoinHandle `
                    $activateJoinBefore 1)) { $complete = $false }
            foreach ($check in @(
                @{ event = 'turn-start-complete'; role = 'host'; before = $completeHostBefore },
                @{ event = 'turn-start-complete'; role = 'join'; before = $completeJoinBefore }
            )) {
                if (-not (Test-SimEventDeltaReached `
                        $events $check.event $check.before 1 $check.role)) { $complete = $false }
            }
            $complete
        })
        if (-not (Wait-WorldDays ($hostBefore + 1) ($joinBefore + 1))) {
            throw "round ${Round}: local days did not independently advance to $($hostBefore + 1)/$($joinBefore + 1)"
        }
        $roundEvents = @(Read-SimRelayEvents)
        Assert-NoRelayFault $roundEvents
        foreach ($check in @(
            @{ event = 'end-turn-observed'; before = $fire.beforeHostObserved; role = 'host' },
            @{ event = 'end-turn-observed'; before = $fire.beforeJoinObserved; role = 'join' },
            @{ event = 'end-turn-applied'; before = $fire.beforeHostApplied; role = 'host' },
            @{ event = 'end-turn-applied'; before = $fire.beforeJoinApplied; role = 'join' },
            @{ event = 'end-turn-accepted'; before = $fire.beforeHostAccepted; role = 'host' },
            @{ event = 'end-turn-accepted'; before = $fire.beforeJoinAccepted; role = 'join' },
            @{ event = 'turn-start-complete'; before = $completeHostBefore; role = 'host' },
            @{ event = 'turn-start-complete'; before = $completeJoinBefore; role = 'join' }
        )) { Assert-SimEventDelta $roundEvents $check.event $check.before 1 $check.role }
        Assert-EngineActionDelta `
            $roundEvents 'ordinary-apply' 'host' $expectedHostHandle `
            $applyHostBefore 1
        Assert-EngineActionDelta `
            $roundEvents 'ordinary-apply' 'host' $expectedJoinHandle `
            $applyJoinBefore 1
        Assert-EngineActionDelta `
            $roundEvents 'ordinary-activate' 'host' $expectedHostHandle `
            $activateHostBefore 1
        Assert-EngineActionDelta `
            $roundEvents 'ordinary-activate' 'join' $expectedJoinHandle `
            $activateJoinBefore 1
        Assert-NoStockEndTurnAfter `
            $turnWatermark $HostProcess $JoinProcess $HostLog $JoinLog
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        $trackedProcessesAlive = 2
        Write-Step "round $Round PASS: both local days independently advanced"
    }
    return [pscustomobject]@{
        round = $Round
        hostBefore = $hostBefore
        hostAfter = $hostBefore + 1
        joinBefore = $joinBefore
        joinAfter = $joinBefore + 1
        dispatchSkewMs = $fire.dispatchSkewMs
        movementBefore = if ($legacyVitalsBefore) {
            [pscustomobject]@{
                host = [int]$legacyVitalsBefore.host.movement
                join = [int]$legacyVitalsBefore.join.movement
            }
        } else { $null }
        movementRefresh = if ($legacyMovementRefresh) {
            [pscustomobject]@{ host = 35; join = 35 }
        } else { $null }
        endTurnProof = if ($Fixture) {
            [pscustomobject]@{
                confirmationCount = $canonicalConfirmationCount
                stockEndTurnTotal = 0
                trackedProcessesAlive = $trackedProcessesAlive
                firstObservationSec = [int]$legacyMovementRefresh.firstObservationSec
                dispatchSkewMs = [double]$fire.dispatchSkewMs
                causal = $null
                cascades = @()
                cascadeOrder = @()
            }
        } else { $null }
        hpBefore = if ($legacyUnitVitalsBefore) {
            [pscustomobject]@{
                host = [int]$legacyUnitVitalsBefore.host.totalHp
                join = [int]$legacyUnitVitalsBefore.join.totalHp
            }
        } else { $null }
        hpAfter = if ($legacyUnitVitalsAfter) {
            [pscustomobject]@{
                host = [int]$legacyUnitVitalsAfter.host.totalHp
                join = [int]$legacyUnitVitalsAfter.join.totalHp
            }
        } else { $null }
        deferredMssEvidence = $deferredMssEvidence
        unitHpBefore = if ($legacyUnitVitalsBefore) {
            [pscustomobject]@{
                host = $legacyUnitVitalsBefore.host
                join = $legacyUnitVitalsBefore.join
            }
        } else { $null }
        unitHpAfter = if ($legacyUnitVitalsAfter) {
            [pscustomobject]@{
                host = $legacyUnitVitalsAfter.host
                join = $legacyUnitVitalsAfter.join
            }
        } else { $null }
        legacyPass = if ($null -eq $legacyPass) { $null } else { [bool]$legacyPass }
        localTurnRelease = $localTurnRelease
        legacyContinuation = $legacyContinuation
    }
}

function Wait-CanonicalLegacyMergeRelease([System.Diagnostics.Process]$RelayProcess,
                                          [System.Diagnostics.Process]$HostProcess,
                                          [System.Diagnostics.Process]$JoinProcess,
                                          [string]$HostLog,
                                          [string]$JoinLog,
                                          [string]$HostMarker,
                                          [string]$JoinMarker,
                                          [int]$HostBefore,
                                          [int]$JoinBefore,
                                          [DateTime]$FireCompletedAt,
                                          [int]$TimeoutSec = 45) {
    if ($TimeoutSec -lt 3 -or ($TimeoutSec % 3) -ne 0) {
        throw 'legacy merge timeout must be a positive multiple of three seconds'
    }
    [int]$hostCount = $HostBefore
    [int]$joinCount = $JoinBefore
    $firstObservationSec = $null
    for ($elapsed = 3; $elapsed -le $TimeoutSec; $elapsed += 3) {
        # barrier_endturn.ps1 made its first completed-merge observation only
        # after +3s and repeated observations, never actions, on this exact
        # cadence from completion of the sole parallel action pair. In MSS the
        # supplied markers are the exact per-client global-release applications.
        $observeAt = $FireCompletedAt.AddSeconds($elapsed)
        Wait-FixedUtcAnchor $observeAt
        $hostCount = Get-ClientLogMarkerCount $HostLog $HostMarker
        $joinCount = Get-ClientLogMarkerCount $JoinLog $JoinMarker
        Write-Step ("legacy merge observation +${elapsed}s: host={0} join={1}" -f
            ($hostCount - $HostBefore), ($joinCount - $JoinBefore))
        if ($hostCount -ge ($HostBefore + 1) -and
            $joinCount -ge ($JoinBefore + 1)) {
            $firstObservationSec = $elapsed
            break
        }
        # Source loop proceeds directly to the next fixed +3 sample. Diagnostic
        # observations are forbidden between these two merge-log reads and the
        # next clock edge; terminal failure handling owns any later artifacts.
    }
    # The source did not throw from its watcher. Exact 1+1 cardinality belongs
    # to the later legacy verdict together with MP/cascade/rotation/process data.
    return [pscustomobject]@{
        firstObservationSec = $firstObservationSec
        hostCount = [int]($hostCount - $HostBefore)
        joinCount = [int]($joinCount - $JoinBefore)
        hostTotal = [int]$hostCount
        joinTotal = [int]$joinCount
    }
}

function Assert-LegacyMergeNoOverrotation([long]$After,
                                           [long]$ExpectedHostHandle,
                                           [object]$History = $null) {
    $history = if ($History) {
        $History
    } else {
        Get-TurnHistory -After $After
    }
    [long]$latestSequence = Get-EvidenceWatermark $history 'turn'
    $allEvents = @((Get-OptionalProperty $history 'events'))
    # The literal old observer reads the complete retained file once. Keep that
    # single read, then project the deferred proof from the already-saved startup
    # watermark; no second transport observation is introduced here.
    $events = @($allEvents | Where-Object {
        [long](Get-RequiredTelemetryNumber `
            $_ 'seq' 'merge stock-turn telemetry event') -gt $After
    })
    if ($events.Count -ne 3) {
        throw ("merge produced $($events.Count) stock-turn records before the first " +
            'deliberate post-merge click, expected one natural BeginSendReturned ' +
            'and the exact host/join BeginApplied pair')
    }
    $seen = @{}
    $beginSend = $null
    [long]$previousSequence = $After
    foreach ($eventRecord in $events) {
        [long]$sequence = Get-RequiredTelemetryNumber `
            $eventRecord 'seq' 'merge stock-turn telemetry event'
        if ($sequence -le $previousSequence) {
            throw "merge stock-turn telemetry sequence $sequence is not strictly after $previousSequence"
        }
        $previousSequence = $sequence
        $kind = [string](Get-OptionalProperty $eventRecord 'kind')
        $role = [string](Get-OptionalProperty $eventRecord 'role')
        if ($kind -eq 'stock-end-turn-send-returned') {
            throw 'barrier End Turn leaked into stock TX before the deliberate post-merge click'
        }
        if ($kind -eq 'stock-begin-turn-send-returned') {
            if ($beginSend) {
                throw 'merge published natural BeginTurn send more than once before the deliberate click'
            }
            if ($role -ne 'host') {
                throw "merge natural BeginTurn send came from role '$role', expected authoritative host"
            }
            [long]$idTo = Get-RequiredTelemetryNumber `
                $eventRecord 'idTo' 'merge natural BeginTurn send'
            [long]$frameLength = Get-RequiredTelemetryNumber `
                $eventRecord 'frameLength' 'merge natural BeginTurn send'
            [long]$sendResult = Get-RequiredTelemetryNumber `
                $eventRecord 'sendResult' 'merge natural BeginTurn send'
            [long]$addressee = Get-RequiredTelemetryNumber `
                $eventRecord 'addressee' 'merge natural BeginTurn send'
            [long]$commandSequence = Get-RequiredTelemetryNumber `
                $eventRecord 'commandSequence' 'merge natural BeginTurn send'
            [long]$activeHandle = Get-RequiredTelemetryNumber `
                $eventRecord 'activeHandle' 'merge natural BeginTurn send'
            if ($idTo -ne 0 -or $frameLength -ne 56 -or $sendResult -ne 1 -or
                $addressee -ne 0 -or $commandSequence -le 1 -or
                $commandSequence -eq [uint32]::MaxValue -or
                $activeHandle -ne $ExpectedHostHandle) {
                throw ("merge natural BeginTurn send has wrong native result/layout " +
                    "(idTo=$idTo frame=$frameLength result=$sendResult " +
                    "addressee=$addressee commandSequence=$commandSequence " +
                    "active=0x$('{0:x8}' -f $activeHandle))")
            }
            $beginSend = $eventRecord
            continue
        }
        if ($kind -ne 'stock-begin-turn-applied' -or $role -notin @('host', 'join')) {
            throw "unexpected merge stock-turn evidence kind='$kind' role='$role'"
        }
        if ($seen.ContainsKey($role)) {
            throw "merge published stock BeginTurn more than once on $role before the deliberate click"
        }
        $seen[$role] = $eventRecord
        [long]$senderDpid = Get-RequiredTelemetryNumber `
            $eventRecord 'senderDpid' "merge BeginTurn/$role"
        [long]$receiverDpid = Get-RequiredTelemetryNumber `
            $eventRecord 'receiverDpid' "merge BeginTurn/$role"
        [long]$frameLength = Get-RequiredTelemetryNumber `
            $eventRecord 'frameLength' "merge BeginTurn/$role"
        [long]$dispatchResult = Get-RequiredTelemetryNumber `
            $eventRecord 'dispatchResult' "merge BeginTurn/$role"
        [long]$addressee = Get-RequiredTelemetryNumber `
            $eventRecord 'addressee' "merge BeginTurn/$role"
        [long]$commandSequence = Get-RequiredTelemetryNumber `
            $eventRecord 'commandSequence' "merge BeginTurn/$role"
        [long]$activeHandle = Get-RequiredTelemetryNumber `
            $eventRecord 'activeHandle' "merge BeginTurn/$role"
        if ($senderDpid -ne 1 -or $receiverDpid -le 1 -or
            $frameLength -ne 56 -or $dispatchResult -le 0 -or
            $addressee -ne 0 -or $commandSequence -le 1 -or
            $commandSequence -eq [uint32]::MaxValue -or
            $activeHandle -ne $ExpectedHostHandle) {
            throw ("merge BeginTurn/$role has wrong native result/layout " +
                "(sender=$senderDpid receiver=$receiverDpid frame=$frameLength " +
                "result=$dispatchResult addressee=$addressee commandSequence=$commandSequence " +
                "active=0x$('{0:x8}' -f $activeHandle))")
        }
    }
    if (-not $beginSend -or
        -not $seen.ContainsKey('host') -or -not $seen.ContainsKey('join')) {
        throw 'merge omitted its natural send or one side of the exact host/join BeginTurn application'
    }
    [long]$hostReceiver = Get-RequiredTelemetryNumber `
        $seen.host 'receiverDpid' 'merge BeginTurn/host'
    [long]$joinReceiver = Get-RequiredTelemetryNumber `
        $seen.join 'receiverDpid' 'merge BeginTurn/join'
    if ($hostReceiver -eq $joinReceiver) {
        throw "merge host/join BeginTurn evidence reused receiver DPID $hostReceiver"
    }
    [long]$hostCommandSequence = Get-RequiredTelemetryNumber `
        $seen.host 'commandSequence' 'merge BeginTurn/host'
    [long]$joinCommandSequence = Get-RequiredTelemetryNumber `
        $seen.join 'commandSequence' 'merge BeginTurn/join'
    [long]$sendCommandSequence = Get-RequiredTelemetryNumber `
        $beginSend 'commandSequence' 'merge natural BeginTurn send'
    if ($hostCommandSequence -ne $joinCommandSequence -or
        $hostCommandSequence -ne $sendCommandSequence) {
        throw ("merge send/host/join copies disagree on the natural broadcast sequence " +
            "($sendCommandSequence/$hostCommandSequence/$joinCommandSequence)")
    }
    if ($latestSequence -ne $previousSequence) {
        throw "merge turn watermark $latestSequence does not match final event $previousSequence"
    }
    return [pscustomobject]@{
        watermark = $latestSequence
        commandSequence = $hostCommandSequence
        beginSend = $beginSend
        host = $seen.host
        join = $seen.join
    }
}

function New-LegacyPostMergeProbeBaseline([string]$HostLog) {
    $uiBaseline = Get-UiHistory -Role host -After 0
    [long]$uiWatermark = Get-EvidenceWatermark $uiBaseline 'host UI'
    $confirmationLinePattern =
        '\[testdrv\]\[scripted-popup\] (OBSERVED|CLAIMED|COMMITTED) ' +
        'role=host dialog=DLG_MESSAGE_BOX\b'
    $confirmationLinesBefore = @(Read-ClientLogLines $HostLog | Where-Object {
        $_ -match $confirmationLinePattern
    }).Count
    return [pscustomobject]@{
        uiWatermark = $uiWatermark
        confirmationLinesBefore = $confirmationLinesBefore
    }
}

function Invoke-LegacyPostMergeHostEndTurnProbe([object]$Action,
                                                [long]$After,
                                                [long]$ExpectedJoinHandle,
                                                [int]$ExpectedDay,
                                                [long]$PreviousCommandSequence,
                                                [System.Diagnostics.Process]$HostProcess,
                                                 [System.Diagnostics.Process]$JoinProcess,
                                                 [string]$HostLog,
                                                 [string]$JoinLog,
                                                 [int]$TimeoutSec = 15,
                                                 [object]$PreparedEvidence = $null) {
    if ($Action.role -ne 'host' -or $Action.dialog -ne 'DLG_STRATEGIC' -or
        $Action.button -ne 'BTN_END_TURN') {
        throw 'legacy post-merge probe did not receive the exact captured host End Turn intent'
    }
    if ($TimeoutSec -lt 3) {
        throw 'legacy post-merge probe timeout must admit its exact first +3 second read'
    }
    $confirmationLinePattern =
        '\[testdrv\]\[scripted-popup\] (OBSERVED|CLAIMED|COMMITTED) ' +
        'role=host dialog=DLG_MESSAGE_BOX\b'
    $probeBaseline = if ($PreparedEvidence) {
        $PreparedEvidence
    } else {
        New-LegacyPostMergeProbeBaseline $HostLog
    }
    [long]$uiWatermark = [long]$probeBaseline.uiWatermark
    [int]$confirmationLinesBefore = [int]$probeBaseline.confirmationLinesBefore
    if (-not (Invoke-Button $Action.role $Action.dialog $Action.button `
                            $Action.instance $Action.appearance)) {
        throw 'the first real post-merge host End Turn did not resolve on its subscribed native owner'
    }

    $firedAt = Get-Date
    # M12 is a clean source-equivalent +3 interval. The native session-long
    # subscriber owns any external confirmation; PowerShell performs no UI read,
    # button action, or intermediate polling inside this fixed clock.
    Wait-FixedUtcAnchor ($firedAt.AddSeconds(3))

    # barrier_endturn.ps1's first stock-rotation read was exactly at +3 s.
    # This must be the first observable call after the fixed anchor: process,
    # relay, log and native-popup proofs belong after the legacy TX verdict.
    $turnHistory = Get-TurnHistory -After $After
    $turnEvents = @((Get-OptionalProperty $turnHistory 'events'))
    if ($turnEvents.Count -notin @(3, 4)) {
        throw ("first post-merge host action produced $($turnEvents.Count) turn records " +
            'at +3, expected the complete BeginTurn triplet and at most one EndSendReturned')
    }
    $proof = Assert-StockTurnEvidencePrefix `
        $turnEvents $After host $ExpectedJoinHandle $ExpectedDay `
        $PreviousCommandSequence
    if (-not $proof) {
        throw 'the first real post-merge host End Turn did not rotate at the exact +3 second read'
    }
    # barrier_endturn.ps1's next and only observable was one exact-two PID
    # census. State, fault-log, relay-health, confirmation and world reads are
    # MSS extensions and therefore belong strictly after BARRIER RESULT: PASS.
    $ownedProcessIds = @([int]$HostProcess.Id, [int]$JoinProcess.Id)
    $ownedProcesses = @(Get-Process -Id $ownedProcessIds -ErrorAction SilentlyContinue)
    if ($ownedProcesses.Count -ne 2) {
        throw "first post-merge rotation found $($ownedProcesses.Count)/2 owned game processes"
    }
    Write-Step 'legacy first post-merge End Turn observation +3s: rotated=True; tracked processes=2/2'
    return [pscustomobject]@{
        evidence = $proof
        actionRole = 'host'
        turnWatermark = [long]$proof.watermark
        uiWatermark = $uiWatermark
        confirmationLinesBefore = $confirmationLinesBefore
        confirmationLinePattern = $confirmationLinePattern
        confirmationAppearance = [long]0
        confirmationSent = $false
        trackedProcessesAlive = 2
        firstObservationSec = 3
    }
}

function Get-LegacyHostCompletedStockTurnCount([object]$History) {
    $events = @((Get-OptionalProperty $History 'events'))
    $hostSends = @($events | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -eq 'stock-begin-turn-send-returned' -and
        [string](Get-OptionalProperty $_ 'role') -eq 'host' -and
        $null -ne (Get-OptionalProperty $_ 'commandSequence')
    })
    [int]$completed = 0
    foreach ($send in $hostSends) {
        [string]$commandSequence = [string](Get-OptionalProperty $send 'commandSequence')
        $hostApplied = @($events | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -eq 'stock-begin-turn-applied' -and
            [string](Get-OptionalProperty $_ 'role') -eq 'host' -and
            [string](Get-OptionalProperty $_ 'commandSequence') -eq $commandSequence
        })
        $joinApplied = @($events | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -eq 'stock-begin-turn-applied' -and
            [string](Get-OptionalProperty $_ 'role') -eq 'join' -and
            [string](Get-OptionalProperty $_ 'commandSequence') -eq $commandSequence
        })
        if ($hostApplied.Count -eq 1 -and $joinApplied.Count -eq 1) {
            ++$completed
        }
    }
    return $completed
}

function Invoke-CanonicalLegacyPostMergeHostEndTurnProbe(
    [object]$Action,
    [object]$PreClickTurnHistory,
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [object]$PreparedEvidence,
    [int]$TimeoutSec = 15) {
    if ($Action.role -ne 'host' -or $Action.dialog -ne 'DLG_STRATEGIC' -or
        $Action.button -ne 'BTN_END_TURN') {
        throw 'canonical legacy probe did not receive its one captured host End Turn intent'
    }
    if ($TimeoutSec -ne 15) {
        throw 'canonical legacy post-merge stock-count window must remain exactly 15 seconds'
    }
    # Old CmdEndCount observed completed gameplay commands above the transport.
    # The MSS-equivalent source count is one complete host broadcast plus the
    # exact host/join BeginTurn applications, not the optional EndSendReturned
    # transport seam that r26 proved can be absent after a valid merge.
    [int]$completedStockTurnBaseline =
        Get-LegacyHostCompletedStockTurnCount $PreClickTurnHistory
    $invokeResult = Invoke-ButtonWhenReady `
        -Role $Action.role `
        -Dialog $Action.dialog `
        -Button $Action.button `
        -AfterUiSequence ([long]$Action.afterUiSequence) `
        -WaitMilliseconds 15000 `
        -CommandTimeoutMilliseconds 8000
    if (-not $invokeResult -or -not [bool]$invokeResult.found) {
        throw 'the sole post-merge host End Turn did not resolve from its exact ready UI event'
    }
    $preClickHostUi = ConvertTo-SavedDialogObservation `
        host (Get-OptionalProperty $invokeResult 'observation')

    [DateTime]$firedAt = [DateTime]::UtcNow
    $samples = [System.Collections.Generic.List[object]]::new()
    $rotationSample = $null
    $firstObservationSec = $null
    [int]$completedStockTurnAfter = $completedStockTurnBaseline
    for ($elapsed = 3; $elapsed -le $TimeoutSec; $elapsed += 3) {
        Wait-FixedUtcAnchor ($firedAt.AddSeconds($elapsed))
        # One read-only equivalent of CmdEndCount(hostPid) at each old +3 edge.
        # Count the completed stock broadcast, independent of which transport
        # return hook observed the initiating local command.
        $turnHistory = Get-TurnHistory -After 0
        $completedStockTurnAfter =
            Get-LegacyHostCompletedStockTurnCount $turnHistory
        $sample = [pscustomobject]@{
            elapsedSeconds = [int]$elapsed
            completedStockTurnCount = [int]$completedStockTurnAfter
            history = $turnHistory
        }
        $samples.Add($sample)
        Write-Step ("legacy post-merge completed-stock-count +${elapsed}s: " +
            "$completedStockTurnBaseline->$completedStockTurnAfter")
        if ($completedStockTurnAfter -gt $completedStockTurnBaseline) {
            $rotationSample = $sample
            $firstObservationSec = $elapsed
            break
        }
    }

    # The source's first observable after the stock-count loop was its process
    # census. Scope that same observation to the two exact handles we launched.
    $ownedProcessIds = @([int]$HostProcess.Id, [int]$JoinProcess.Id)
    $ownedProcesses = @(Get-Process `
        -Id $ownedProcessIds -ErrorAction SilentlyContinue)
    return [pscustomobject]@{
        evidence = $null
        preClickEvidence = $null
        currentOwner = $null
        actionRole = 'host'
        turnWatermark = $null
        preClickTurnHistory = $PreClickTurnHistory
        baselineCompletedStockTurnCount = [int]$completedStockTurnBaseline
        completedStockTurnCount = [int]$completedStockTurnAfter
        pollSamples = @($samples)
        rotationSample = $rotationSample
        rotated = [bool]($null -ne $rotationSample)
        staleSuppressSuspect = [bool]($null -eq $rotationSample)
        preClickHostUi = $preClickHostUi
        invoke = Get-OptionalProperty $invokeResult 'invoke'
        overrotationHostUi = $PreparedEvidence.overrotationHostUi
        suppressCount = [int]$PreparedEvidence.suppressCount
        uiWatermark = [long]$PreparedEvidence.uiWatermark
        confirmationLinesBefore = [int]$PreparedEvidence.confirmationLinesBefore
        confirmationLinePattern = [string]$PreparedEvidence.confirmationLinePattern
        confirmationAppearance = [long]0
        confirmationSent = $false
        trackedProcessesAlive = [int]$ownedProcesses.Count
        firstObservationSec = $firstObservationSec
        preMergeTurnWatermark = [long]$PreparedEvidence.preMergeTurnWatermark
    }
}

function Assert-DeferredPostMergeProbeEvidence(
    [object]$Probe,
    [int]$ExpectedDay,
    [long]$ExpectedHostHandle,
    [long]$ExpectedJoinHandle,
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [string]$HostLog,
    [string]$JoinLog) {
    # This complete block is intentionally after the old BARRIER RESULT: PASS.
    # It strengthens the already-saved source observations; it performs no
    # gameplay action and never changes the legacy verdict.
    Assert-ClientsLive $HostProcess $JoinProcess
    Assert-NoClientFaults $HostLog $JoinLog
    Assert-ProductionRelayHealthy

    $preClickEvidence = Assert-LegacyMergeNoOverrotation `
        ([long]$Probe.preMergeTurnWatermark) $ExpectedHostHandle `
        -History $Probe.preClickTurnHistory
    $rotationSample = $Probe.rotationSample
    if (-not $rotationSample) {
        throw 'deferred canonical proof received no successful legacy stock-count sample'
    }
    $rotationEvents = @((Get-OptionalProperty $rotationSample.history 'events'))
    $postClickEvents = @($rotationEvents | Where-Object {
        [long](Get-OptionalProperty $_ 'seq') -gt [long]$preClickEvidence.watermark
    })
    if ($postClickEvents.Count -notin @(3, 4)) {
        throw ("sole post-merge host action appended $($postClickEvents.Count) " +
            'turn records in its saved successful sample, expected the complete ' +
            'BeginTurn triplet with at most one optional EndSendReturned record')
    }
    $stockEvidence = Assert-StockTurnEvidencePrefix `
        $postClickEvents ([long]$preClickEvidence.watermark) `
        host $ExpectedJoinHandle $ExpectedDay `
        ([long]$preClickEvidence.commandSequence)
    if (-not $stockEvidence -or
        [int]$rotationSample.completedStockTurnCount -le
            [int]$Probe.baselineCompletedStockTurnCount) {
        throw 'saved legacy completed-stock growth did not contain one exact stock rotation prefix'
    }

    $preClickHostUi = $Probe.preClickHostUi
    if (-not $preClickHostUi -or -not $preClickHostUi.Ready -or
        $script:BareMapDialogs -notcontains $preClickHostUi.Dialog -or
        [string]$preClickHostUi.Dialog -ne 'DLG_STRATEGIC') {
        $dialog = if ($preClickHostUi) { [string]$preClickHostUi.Dialog } else { '<missing>' }
        throw "saved pre-click host UI does not prove current-owner bare map (dialog='$dialog')"
    }
    if ([int]$Probe.suppressCount -ne 0) {
        throw ("production canonical merge retained $($Probe.suppressCount) " +
            'obsolete barrier-suppress consumption marker(s)')
    }

    $confirmationLines = @(Read-ClientLogLines $HostLog | Where-Object {
        $_ -match [string]$Probe.confirmationLinePattern
    })
    [int]$before = [int]$Probe.confirmationLinesBefore
    if ($confirmationLines.Count -lt $before) {
        throw 'host confirmation marker history regressed during the post-merge probe'
    }
    # Assignment enumerates branch output in PowerShell: assigning an empty
    # @() from an if-expression produces $null under StrictMode, not an empty
    # array. Keep the expected no-confirmation path explicitly array-typed.
    $newConfirmationLines = @()
    if ($confirmationLines.Count -ne $before) {
        $newConfirmationLines =
            @($confirmationLines[$before..($confirmationLines.Count - 1)])
    }
    [long]$confirmationAppearance = 0
    $confirmationSent = $false
    if ($newConfirmationLines.Count -ne 0) {
        if ($newConfirmationLines.Count -ne 3 -or
            $newConfirmationLines[0] -notmatch
                'OBSERVED role=host dialog=DLG_MESSAGE_BOX appearance=(?<appearance>[0-9]+) owner=(?<owner>[0-9]+) button=BTN_(?:OK|YES|NO) tick=[0-9]+$') {
            throw 'post-merge confirmation did not begin one exact native OBSERVED/CLAIMED/COMMITTED chain'
        }
        $confirmationAppearance = [long]$Matches.appearance
        [long]$confirmationOwner = [long]$Matches.owner
        if ($newConfirmationLines[1] -notmatch
                "CLAIMED role=host dialog=DLG_MESSAGE_BOX appearance=$confirmationAppearance owner=$confirmationOwner button=(?<button>BTN_(?:YES|NO)) bindAgeMs=(?<age>[0-9]+) tick=[0-9]+$" -or
            [long]$Matches.age -lt [long]$script:StartupModalSettleMilliseconds) {
            throw 'post-merge confirmation lost its exact native claim or 300 ms bind-age gate'
        }
        $confirmationButton = [string]$Matches.button
        if ($newConfirmationLines[2] -notmatch
                "COMMITTED role=host dialog=DLG_MESSAGE_BOX appearance=$confirmationAppearance owner=$confirmationOwner button=$confirmationButton tick=[0-9]+$") {
            throw 'post-merge confirmation was not committed once by its native claimed owner'
        }
        $confirmationSent = $true
    }
    $hostDay = Get-WorldDay host
    $joinDay = Get-WorldDay join
    if ($hostDay -ne $ExpectedDay -or $joinDay -ne $ExpectedDay) {
        throw ("first post-merge host rotation changed the day unexpectedly " +
            "($hostDay/$joinDay, expected $ExpectedDay)")
    }
    $Probe.evidence = $stockEvidence
    $Probe.preClickEvidence = $preClickEvidence
    $Probe.turnWatermark = [long]$stockEvidence.watermark
    $Probe.currentOwner = [pscustomobject]@{
        activePlayerId = ('0x{0:x8}' -f $ExpectedHostHandle)
        hostDialog = [string]$preClickHostUi.Dialog
    }
    $Probe.confirmationAppearance = $confirmationAppearance
    $Probe.confirmationSent = $confirmationSent
    return $Probe
}

function Assert-LiteralMasstestProcessAlive(
    [System.Diagnostics.Process]$Process,
    [string]$Description) {
    if (-not $Process) { throw "$Description process identity is missing" }
    $Process.Refresh()
    if ($Process.HasExited) {
        throw "$Description process exited with code $($Process.ExitCode)"
    }
}

function Invoke-LiteralMasstestEndTurnOnce(
    [ValidateSet('host', 'join')][string]$Role,
    [int]$CompletedDay,
    [System.Diagnostics.Process]$LivenessProcess,
    [string]$LivenessDescription,
    [object]$PreparedAction) {
    # Drive-ToBarrier checked its target process; Drive-ToMerge checked the
    # authoritative host even when the laggard was the joiner. Keep that exact
    # distinction and submit one action for this distinct subjective day. The
    # The symbolic UI watermark was captured by the preceding source-equivalent read; this
    # function performs no hidden state read and arms exactly one POST. The relay
    # admits it only on the current or first later native-idle strategic owner.
    Assert-LiteralMasstestProcessAlive $LivenessProcess $LivenessDescription
    if (-not $PreparedAction -or [string]$PreparedAction.role -ne $Role -or
        [string]$PreparedAction.dialog -ne 'DLG_STRATEGIC' -or
        [string]$PreparedAction.button -ne 'BTN_END_TURN') {
        throw "literal masstest $Role day-$CompletedDay has no exact prepared End Turn owner"
    }
    $action = $PreparedAction
    $watermarkProperty = $action.PSObject.Properties['afterUiSequence']
    if ($null -eq $watermarkProperty) {
        throw "literal masstest $Role day-$CompletedDay omitted its UI watermark"
    }
    [long]$afterUiSequence = $watermarkProperty.Value
    if ($afterUiSequence -lt 0 -or $afterUiSequence -ge [uint32]::MaxValue) {
        throw "literal masstest $Role day-$CompletedDay UI watermark is outside intent range"
    }
    $invokeResult = Invoke-ButtonWhenReady `
        -Role $action.role `
        -Dialog $action.dialog `
        -Button $action.button `
        -AfterUiSequence $afterUiSequence `
        -WaitMilliseconds 26000 `
        -CommandTimeoutMilliseconds 8000
    return [pscustomobject]@{
        role = $Role
        completedDay = $CompletedDay
        appearance = [long]$invokeResult.invoke.appearance
        instance = [long]$invokeResult.invoke.instance
        uiSequence = [long]$invokeResult.observation.uiSeq
        preparedUiSequence = [long]($afterUiSequence + 1)
        completedUtc = [DateTime]::UtcNow
        attempts = 1
        accepted = 1
        recoveryActions = 0
    }
}

function Read-LiteralMasstestPeerStateCheckpoint(
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [hashtable]$ExpectedDays,
    [string[]]$ActionRoles,
    [string]$SourceBoundary) {
    # One request is either the source Get-PeerPids read or the sole hook-log
    # observation after a day-1 action. Under the v8 harness the same immutable
    # publication also supplies the UI watermark for the next one-shot symbolic
    # POST. The exact enabled native-idle owner is captured atomically by the
    # relay, which is a transport-admission adaptation, not another oracle read.
    $state = Get-RelayState
    $hostState = Get-OptionalProperty $state 'host'
    $joinState = Get-OptionalProperty $state 'join'
    Assert-DebugRelayClientIdentity host $hostState $HostProcess
    Assert-DebugRelayClientIdentity join $joinState $JoinProcess
    foreach ($role in @('host', 'join')) {
        if (-not $ExpectedDays.ContainsKey($role)) { continue }
        $roleState = if ($role -eq 'host') { $hostState } else { $joinState }
        $dayValue = Get-OptionalProperty $roleState 'day'
        if ($null -eq $dayValue -or [int]$dayValue -ne [int]$ExpectedDays[$role]) {
            throw ("literal masstest $SourceBoundary observed $role day " +
                "'$dayValue', expected $($ExpectedDays[$role])")
        }
    }
    $actions = @{}
    foreach ($role in $ActionRoles) {
        $roleState = if ($role -eq 'host') { $hostState } else { $joinState }
        [long]$uiSequence = Get-RequiredTelemetryNumber `
            $roleState 'uiSeq' "literal masstest $SourceBoundary $role UI adapter"
        if ($uiSequence -lt 1) {
            throw "literal masstest $SourceBoundary $role UI sequence must be positive"
        }
        $actions[$role] = [pscustomobject]@{
            role = $role
            dialog = 'DLG_STRATEGIC'
            button = 'BTN_END_TURN'
            afterUiSequence = [long]($uiSequence - 1)
        }
    }
    return [pscustomobject]@{
        sourceBoundary = $SourceBoundary
        sourceReadCount = 1
        transportAdapter = 'same-state UI watermark plus exact-ready owner intent'
        hostDay = [int](Get-OptionalProperty $hostState 'day')
        joinDay = [int](Get-OptionalProperty $joinState 'day')
        actions = $actions
    }
}

function Assert-LiteralMasstestRoleEndTurnHistory(
    [object[]]$Events,
    [ValidateSet('host', 'join')][string]$Role,
    [int]$ExpectedCount) {
    $observed = @(Get-SimEventMatches $Events 'end-turn-observed' $Role)
    $applied = @(Get-SimEventMatches $Events 'end-turn-applied' $Role)
    $accepted = @(Get-SimEventMatches $Events 'end-turn-accepted' $Role)
    if ($observed.Count -ne $ExpectedCount -or
        $applied.Count -ne $ExpectedCount -or
        $accepted.Count -ne $ExpectedCount) {
        throw ("literal masstest $Role End Turn counts are " +
            "$($observed.Count)/$($applied.Count)/$($accepted.Count), " +
            "expected $ExpectedCount/$ExpectedCount/$ExpectedCount")
    }
    for ($index = 0; $index -lt $ExpectedCount; $index++) {
        $completedDay = $index + 1
        [long]$expectedLease = Get-ExpectedTurnLease `
            $Events $Role $completedDay
        foreach ($field in @('lease', 'completedDay')) {
            $observedValue = Get-OptionalProperty $observed[$index] $field
            $appliedValue = Get-OptionalProperty $applied[$index] $field
            $acceptedValue = Get-OptionalProperty $accepted[$index] $field
            if ($null -eq $observedValue -or $observedValue -ne $appliedValue -or
                $observedValue -ne $acceptedValue) {
                throw "literal masstest $Role End Turn day $completedDay changed '$field' across its causal triple"
            }
        }
        if ([long](Get-OptionalProperty $accepted[$index] 'lease') -ne
                $expectedLease -or
            [int](Get-OptionalProperty $accepted[$index] 'completedDay') -ne $completedDay) {
            throw "literal masstest $Role End Turn day $completedDay has the wrong lease/day"
        }
        $observedIndex = Get-SimEventRecordIndex $Events $observed[$index]
        $appliedIndex = Get-SimEventRecordIndex $Events $applied[$index]
        $acceptedIndex = Get-SimEventRecordIndex $Events $accepted[$index]
        if ($observedIndex -lt 0 -or $appliedIndex -lt 0 -or
            $acceptedIndex -le $observedIndex -or
            $acceptedIndex -le $appliedIndex) {
            throw "literal masstest $Role End Turn day $completedDay lost Observed/Applied -> Accepted order"
        }
    }
}

function Assert-LiteralOrderedMasstestStage(
    [object[]]$Events,
    [ValidateSet('first-barrier', 'laggard-merge')][string]$Stage,
    [ValidateSet('host', 'join')][string]$FirstRole,
    [ValidateSet('host', 'join')][string]$LaggardRole,
    [switch]$AllowPendingHostMerge) {
    # This is the single old hook-log read projected onto the retained event
    # sample.  Gate the next gameplay mutation on the marker identity and causal
    # order that are observable here; the full MSS two-phase causal chain remains
    # deferred until after the legacy outcome is PASS.
    [int]$expectedBarrierCount = if ($Stage -eq 'first-barrier') { 1 } else { 2 }
    $barriers = @(Get-SimEventMatches $Events 'barrier-held')
    [int]$barrierCount = $barriers.Count
    # The old [merge] MERGE fired marker was written after the authoritative
    # host completed its local merge. v8 records that same boundary as the
    # host's MergeApplied proof; later stock release stays a distinct event.
    $hostMergeApplied = @(Get-SimEventMatches $Events 'merge-applied' 'host')
    [int]$mergeCount = $hostMergeApplied.Count
    if ($AllowPendingHostMerge -and $Stage -ne 'laggard-merge') {
        throw 'pending host merge classification belongs only to the laggard-merge snapshot'
    }
    if ($barrierCount -ne $expectedBarrierCount) {
        throw ("literal ordered masstest $Stage observed $barrierCount " +
            "barrier-held records, expected exactly $expectedBarrierCount")
    }

    # Unlike the deferred MSS proof below, this gate checks only the causal
    # facts which the old watched hook marker represented: the first role's
    # distinct day-2 End Turn reached its own barrier, for the other negotiated
    # role, before the harness may mutate the laggard.
    [void](Get-SessionPlanEvent $Events)
    $firstBarriers = @($barriers | Where-Object {
        [string](Get-OptionalProperty $_ 'role') -eq $FirstRole
    })
    if ($firstBarriers.Count -ne 1) {
        throw "literal ordered masstest $Stage has no exact $FirstRole barrier-held record"
    }
    $barrier = $firstBarriers[0]
    $firstDayTwoAccepted = @(Get-SimEventMatches `
        $Events 'end-turn-accepted' $FirstRole | Where-Object {
            [int](Get-OptionalProperty $_ 'completedDay') -eq 2
        })
    [long]$firstLease = Get-ExpectedTurnLease $Events $FirstRole 2
    if ($firstDayTwoAccepted.Count -ne 1 -or
        [long](Get-OptionalProperty $firstDayTwoAccepted[0] 'lease') -ne
            $firstLease) {
        throw ("literal ordered masstest $Stage has no exact accepted " +
            "$FirstRole day-2 End Turn causal predecessor")
    }
    [void](Assert-BarrierHeldEvidence $Events $barrier $FirstRole 2)
    [int]$firstAcceptedIndex = Get-SimEventRecordIndex `
        $Events $firstDayTwoAccepted[0]
    [int]$barrierIndex = Get-SimEventRecordIndex $Events $barrier
    if ($firstAcceptedIndex -lt 0 -or $barrierIndex -le $firstAcceptedIndex) {
        throw ("literal ordered masstest $Stage barrier did not follow " +
            "the exact $FirstRole day-2 acceptance")
    }

    $merge = $null
    [int]$mergeIndex = -1
    [int]$laggardAcceptedIndex = -1
    if ($Stage -eq 'first-barrier') {
        if ($mergeCount -ne 0 -or
            (Get-SimEventCount $Events 'end-turn-accepted' $LaggardRole) -ne 0) {
            throw ('literal ordered masstest first-barrier snapshot already ' +
                'contains a laggard acceptance or merge')
        }
    } else {
        if (($AllowPendingHostMerge -and $mergeCount -gt 1) -or
            (-not $AllowPendingHostMerge -and $mergeCount -ne 1)) {
            [string]$expectedMergeCount = if ($AllowPendingHostMerge) {
                'zero or one'
            } else { 'exactly one' }
            throw ("literal ordered masstest laggard-merge observed $mergeCount " +
                "merge-applied/host records, expected $expectedMergeCount")
        }
        $laggardDayTwoAccepted = @(Get-SimEventMatches `
            $Events 'end-turn-accepted' $LaggardRole | Where-Object {
                [int](Get-OptionalProperty $_ 'completedDay') -eq 2
            })
        [long]$laggardLease = Get-ExpectedTurnLease $Events $LaggardRole 2
        if ($laggardDayTwoAccepted.Count -ne 1 -or
            [long](Get-OptionalProperty $laggardDayTwoAccepted[0] 'lease') -ne
                $laggardLease) {
            throw ('literal ordered masstest laggard-merge has no exact ' +
                "accepted $LaggardRole day-2 End Turn causal predecessor")
        }
        $laggardBarriers = @($barriers | Where-Object {
            [string](Get-OptionalProperty $_ 'role') -eq $LaggardRole
        })
        if ($laggardBarriers.Count -ne 1) {
            throw 'literal ordered masstest laggard-merge omitted the laggard barrier-held record'
        }
        [void](Assert-BarrierHeldEvidence `
            $Events $laggardBarriers[0] $LaggardRole 2)
        $laggardAcceptedIndex = Get-SimEventRecordIndex `
            $Events $laggardDayTwoAccepted[0]
        [int]$laggardBarrierIndex = Get-SimEventRecordIndex `
            $Events $laggardBarriers[0]
        if ($laggardAcceptedIndex -le $barrierIndex -or
            $laggardBarrierIndex -le $laggardAcceptedIndex) {
            throw ('literal ordered masstest laggard acceptance/barrier did not ' +
                'follow the first held barrier in exact causal order')
        }
        if ($mergeCount -eq 0) {
            if ((Get-SimEventCount $Events 'merge-released') -ne 0) {
                throw ('literal ordered masstest pending host merge snapshot ' +
                    'already contains a stock release')
            }
        } else {
            $merge = $hostMergeApplied[0]
            [long]$mergeActionId = Get-RequiredTelemetryNumber `
                $merge 'actionId' 'literal ordered host merge proof'
            $prepare = Get-ExactSimEvent $Events 'merge-prepare-dispatched'
            if ([int](Get-OptionalProperty $prepare 'mergeDay') -ne 3 -or
                [long](Get-OptionalProperty $prepare 'actionId') -ne $mergeActionId) {
                throw ('literal ordered masstest laggard-merge does not prove ' +
                    'the locally applied day-3 host merge')
            }
            $mergeIndex = Get-SimEventRecordIndex $Events $merge
            if ($mergeIndex -le $laggardAcceptedIndex -or
                $mergeIndex -le $barrierIndex -or
                $mergeIndex -le $laggardBarrierIndex) {
                throw ('literal ordered masstest merge-applied/host did not follow ' +
                    'both accepted barrier-held records')
            }
        }
    }
    return [pscustomobject]@{
        stage = $Stage
        sourceReadCount = 1
        sourceObservation = 'production relay append-only event log'
        observedUtc = [DateTime]::UtcNow
        firstRole = $FirstRole
        laggardRole = $LaggardRole
        barrierCount = $barrierCount
        barrierHeldPeers = @($barriers | ForEach-Object {
            [string](Get-OptionalProperty $_ 'role')
        } | Sort-Object)
        mergeCount = $mergeCount
        sourceMergeEvent = 'merge-applied/host'
        mergeActionId = if ($merge) {
            [long](Get-OptionalProperty $merge 'actionId')
        } else { $null }
        mergeClassification = if ($Stage -eq 'first-barrier') {
            'not-applicable'
        } elseif ($mergeCount -eq 1) {
            'complete-at-snapshot'
        } else { 'pending-at-snapshot' }
        barrierObserved = $true
        mergeObserved = [bool]($mergeCount -eq 1)
        barrierRecord = $barrier
        mergeRecord = $merge
        firstAcceptedIndex = $firstAcceptedIndex
        barrierIndex = $barrierIndex
        laggardAcceptedIndex = $laggardAcceptedIndex
        mergeIndex = $mergeIndex
        savedEvents = @($Events)
    }
}

function Assert-DeferredLiteralOrderedMasstestStage(
    [object[]]$Events,
    [ValidateSet('first-barrier', 'laggard-merge')][string]$Stage,
    [ValidateSet('host', 'join')][string]$FirstRole,
    [ValidateSet('host', 'join')][string]$LaggardRole) {
    Assert-NoRelayFault $Events
    Assert-ExactBootstrapOperational $Events
    $sessionPlan = Get-SessionPlanEvent $Events
    $expectedHandles = @{
        host = [string](Get-OptionalProperty $sessionPlan 'hostHandle')
        join = [string](Get-OptionalProperty $sessionPlan 'joinHandle')
    }
    $expected = switch ($Stage) {
        'first-barrier' {
            @{ firstEnd = 2; laggardEnd = 0; firstCascade = 1;
               laggardCascade = 0; barrier = 1; merge = 0 }
        }
        'laggard-merge' {
            @{ firstEnd = 2; laggardEnd = 2; firstCascade = 1;
               laggardCascade = 1; barrier = 2; merge = 1 }
        }
    }

    Assert-LiteralMasstestRoleEndTurnHistory `
        $Events $FirstRole $expected.firstEnd
    Assert-LiteralMasstestRoleEndTurnHistory `
        $Events $LaggardRole $expected.laggardEnd

    foreach ($role in @('host', 'join')) {
        [int]$expectedSubjectiveCascade = if ($role -eq $FirstRole) {
            [int]$expected.firstCascade
        } else { [int]$expected.laggardCascade }
        $cascades = @(Get-EngineActionMatches `
            $Events 'ordinary-apply' 'host' $expectedHandles[$role] 2)
        $activations = @(Get-EngineActionMatches `
            $Events 'ordinary-activate' $role $expectedHandles[$role] 2)
        $completions = @(Get-SimEventMatches $Events 'turn-start-complete' $role 2)
        if ($cascades.Count -ne $expectedSubjectiveCascade -or
            $activations.Count -ne $expectedSubjectiveCascade -or
            $completions.Count -ne $expectedSubjectiveCascade -or
            (Get-SimEventCount $Events 'turn-start-complete' $role) -ne
                $expectedSubjectiveCascade) {
            throw ("literal masstest $role day-2 action/completion counts are " +
                "$($cascades.Count)/$($activations.Count)/$($completions.Count), " +
                "expected $expectedSubjectiveCascade each")
        }
        if ($expectedSubjectiveCascade -eq 1) {
            $cascade = $cascades[0]
            $activation = $activations[0]
            $completion = $completions[0]
            [long]$actionId = Get-RequiredTelemetryNumber `
                $cascade 'actionId' "literal masstest ApplyTurnStart/$role"
            [long]$lease = Get-RequiredTelemetryNumber `
                $cascade 'lease' "literal masstest turn lease/$role"
            if ([int](Get-OptionalProperty $cascade 'kind') -ne 1 -or
                [int](Get-OptionalProperty $activation 'kind') -ne 2 -or
                [long](Get-OptionalProperty $activation 'actionId') -ne $actionId -or
                [long](Get-OptionalProperty $completion 'actionId') -ne $actionId -or
                [long](Get-OptionalProperty $activation 'lease') -ne $lease -or
                [long](Get-OptionalProperty $completion 'lease') -ne $lease -or
                (Get-SimEventRecordIndex $Events $activation) -le
                    (Get-SimEventRecordIndex $Events $cascade) -or
                (Get-SimEventRecordIndex $Events $completion) -le
                    (Get-SimEventRecordIndex $Events $activation)) {
                throw "literal masstest $role day-2 turn start lost its exact v8 action chain"
            }
        }
    }
    Assert-SimEventDelta $Events 'barrier-held' 0 $expected.barrier
    $mergeEvents = @(
        'merge-prepare-dispatched',
        'merge-prepare-applied',
        'merge-execute-dispatched',
        'merge-execute-applied',
        'merge-applied',
        'merge-released'
    )
    foreach ($eventName in $mergeEvents) {
        [int]$expectedCount = if ($eventName -in @(
            'merge-prepare-applied', 'merge-applied'
        )) { 2 * $expected.merge } else { $expected.merge }
        Assert-SimEventDelta $Events $eventName 0 $expectedCount
    }
    if ($expected.barrier -eq 1) {
        $barrier = Get-NewExactSimEvent $Events 'barrier-held' '' 0
        [void](Assert-BarrierHeldEvidence $Events $barrier $FirstRole 2)
    }
    $mergeProof = $null
    if ($expected.merge -eq 1) {
        $mergeProof = Assert-ExactMergeTransaction $Events 3
    }
    return [pscustomobject]@{
        stage = $Stage
        sourceReadCount = 1
        sourceObservation = 'production relay append-only event log'
        observedUtc = [DateTime]::UtcNow
        firstEndTurns = [int]$expected.firstEnd
        laggardEndTurns = [int]$expected.laggardEnd
        mergeObserved = [bool]($expected.merge -eq 1)
        barrierCount = [int]$expected.barrier
        barrierHeldPeers = if ($mergeProof) {
            @($mergeProof.barrierHeldPeers)
        } else { @($FirstRole) }
        mergeActionId = if ($mergeProof) { [long]$mergeProof.actionId } else { $null }
    }
}

function Run-LiteralOrderedMasstestToMerge(
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [ValidateSet('host-first', 'join-first')][string]$Order,
    [object]$Fixture,
    [object]$InitialCheckpoint) {
    if ($MergeDay -ne 3) {
        throw "literal test_R_masstest topology requires MergeDay=3, got $MergeDay"
    }
    if (-not $InitialCheckpoint -or -not $InitialCheckpoint.actions) {
        throw 'literal ordered masstest requires its one saved Get-PeerPids-equivalent state'
    }
    $firstRole = if ($Order -eq 'host-first') { 'host' } else { 'join' }
    $laggardRole = if ($firstRole -eq 'host') { 'join' } else { 'host' }
    $firstProcess = if ($firstRole -eq 'host') { $HostProcess } else { $JoinProcess }
    $steps = [System.Collections.Generic.List[object]]::new()

    # Successful Drive-ToBarrier path at MergeDay=3: two different subjective
    # End Turns. Each POST return starts its own fixed +4-second sleep, followed
    # by exactly one equivalent of the watched hook-log read.
    $action = Invoke-LiteralMasstestEndTurnOnce `
        $firstRole 1 $firstProcess "$firstRole barrier target" `
        $InitialCheckpoint.actions[$firstRole]
    Wait-FixedUtcAnchor (([DateTime]$action.completedUtc).AddSeconds(4))
    $expectedDays = @{ host = 1; join = 1 }
    $expectedDays[$firstRole] = 2
    $firstDayAdvance = Read-LiteralMasstestPeerStateCheckpoint `
        $HostProcess $JoinProcess $expectedDays @($firstRole, $laggardRole) `
        'Drive-ToBarrier day-1 hook-log read'
    $steps.Add([pscustomobject]@{
        stage = 'first-day-1'
        role = $firstRole
        completedDay = 1
        fixedWaitSeconds = 4
        attempts = [int]$action.attempts
        accepted = [int]$action.accepted
        recoveryActions = [int]$action.recoveryActions
        observation = $firstDayAdvance
    })

    $action = Invoke-LiteralMasstestEndTurnOnce `
        $firstRole 2 $firstProcess "$firstRole barrier target" `
        $firstDayAdvance.actions[$firstRole]
    Wait-FixedUtcAnchor (([DateTime]$action.completedUtc).AddSeconds(4))
    $events = @(Read-SimRelayEvents)
    $stage = Assert-LiteralOrderedMasstestStage `
        $events first-barrier $firstRole $laggardRole
    $firstBarrierStage = $stage
    if (-not [bool]$stage.barrierObserved -or [bool]$stage.mergeObserved) {
        throw ('literal ordered masstest did not prove the first-role barrier ' +
            'before the first laggard End Turn')
    }
    $steps.Add([pscustomobject]@{
        stage = 'first-barrier'
        role = $firstRole
        completedDay = 2
        fixedWaitSeconds = 4
        attempts = [int]$action.attempts
        accepted = [int]$action.accepted
        recoveryActions = [int]$action.recoveryActions
        observation = $stage
    })

    # Successful Drive-ToMerge path: host liveness is checked before every
    # laggard action, and once more after the first non-merge observation. The
    # loop-top check then repeats it before the second distinct-day action.
    $action = Invoke-LiteralMasstestEndTurnOnce `
        $laggardRole 1 $HostProcess 'authoritative host during merge drive' `
        $firstDayAdvance.actions[$laggardRole]
    Wait-FixedUtcAnchor (([DateTime]$action.completedUtc).AddSeconds(4))
    $laggardDayAdvance = Read-LiteralMasstestPeerStateCheckpoint `
        $HostProcess $JoinProcess @{ host = 2; join = 2 } @($laggardRole) `
        'Drive-ToMerge day-1 host-hook read'
    $steps.Add([pscustomobject]@{
        stage = 'laggard-day-1'
        role = $laggardRole
        completedDay = 1
        fixedWaitSeconds = 4
        attempts = [int]$action.attempts
        accepted = [int]$action.accepted
        recoveryActions = [int]$action.recoveryActions
        observation = $laggardDayAdvance
    })
    Assert-LiteralMasstestProcessAlive `
        $HostProcess 'authoritative host after the first laggard observation'

    $action = Invoke-LiteralMasstestEndTurnOnce `
        $laggardRole 2 $HostProcess 'authoritative host during merge drive' `
        $laggardDayAdvance.actions[$laggardRole]
    Wait-FixedUtcAnchor (([DateTime]$action.completedUtc).AddSeconds(4))
    $events = @(Read-SimRelayEvents)
    $fixedMergeStage = Assert-LiteralOrderedMasstestStage `
        $events laggard-merge $firstRole $laggardRole `
        -AllowPendingHostMerge
    $completionEvents = Wait-SimCondition `
        -RelayProcess $script:ProductionRelayProcess `
        -TimeoutSec 8 `
        -Description 'ordered host merge completion after the fixed source snapshot' `
        -Condition {
            param($current)
            [int]$hostMergeCount = @(
                Get-SimEventMatches $current 'merge-applied' 'host'
            ).Count
            if ($hostMergeCount -gt 1) {
                throw 'ordered host merge completion appeared more than once'
            }
            $hostMergeCount -eq 1
        }
    $stage = Assert-LiteralOrderedMasstestStage `
        $completionEvents laggard-merge $firstRole $laggardRole
    if (-not [bool]$stage.barrierObserved -or -not [bool]$stage.mergeObserved) {
        throw ('literal ordered masstest did not prove the acknowledged merge ' +
            'before its Phase-C handoff')
    }
    $steps.Add([pscustomobject]@{
        stage = 'laggard-merge'
        role = $laggardRole
        completedDay = 2
        fixedWaitSeconds = 4
        attempts = [int]$action.attempts
        accepted = [int]$action.accepted
        recoveryActions = [int]$action.recoveryActions
        observation = $fixedMergeStage
        completionEvidence = $stage
    })

    [long]$hostHandle = Convert-ExactPlayerHandle `
        $Fixture.players.hostHandle 'literal masstest fixture host handle'
    [long]$joinHandle = Convert-ExactPlayerHandle `
        $Fixture.players.joinHandle 'literal masstest fixture join handle'
    [long]$neutralHandle = Convert-ExactPlayerHandle `
        $Fixture.players.neutralHandle 'literal masstest neutral handle'
    return [pscustomobject]@{
        automaticMasstestHandoff = $true
        literalOrderedMasstestHandoff = $true
        order = $Order
        mergeDay = 3
        handoffCompletedUtc = [DateTime]::UtcNow
        # The first literal Phase-C neutral-count read resolves the exact
        # merge-day host slot from this full-history origin and filters after it.
        slotWatermarkBeforePhaseC = [long]0
        hostHandle = $hostHandle
        joinHandle = $joinHandle
        neutralHandle = $neutralHandle
        sourceMergeObserved = [bool]$fixedMergeStage.mergeObserved
        fixedMergeClassification = [string]$fixedMergeStage.mergeClassification
        completionMergeObserved = [bool]$stage.mergeObserved
        mergeCompletionTimeoutSeconds = 8
        mergeActionId = [long]$stage.mergeActionId
        firstBarrierStage = $firstBarrierStage
        fixedMergeStage = $fixedMergeStage
        mergeStage = $stage
        eventsAtHandoff = @($completionEvents)
        firstRole = $firstRole
        laggardRole = $laggardRole
        actionTrace = @($steps)
        attemptedActions = 4
        acceptedActions = 4
        recoveryActions = 0
    }
}

function Complete-LiteralOrderedMasstestDeferredProof(
    [object]$MergeHandoff,
    [object]$PhaseC,
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [string]$HostLog,
    [string]$JoinLog) {
    if (-not $MergeHandoff -or
        -not [bool](Get-OptionalProperty $MergeHandoff 'literalOrderedMasstestHandoff')) {
        throw 'literal ordered masstest deferred proof received no exact handoff'
    }
    # The old Phase-C PASS has already completed. MSS health and full causal
    # identity are strengthened only after that boundary.
    Assert-ClientsLive $HostProcess $JoinProcess
    Assert-NoClientFaults $HostLog $JoinLog
    Assert-LiteralMasstestProcessAlive `
        $script:ProductionRelayProcess 'production relay after literal Phase C'
    $events = @(Read-SimRelayEvents)
    Assert-NoRelayFault $events
    [string]$firstRole = if ([string]$MergeHandoff.order -eq 'host-first') {
        'host'
    } else { 'join' }
    [string]$laggardRole = if ($firstRole -eq 'host') { 'join' } else { 'host' }
    if (-not $PhaseC -or -not $MergeHandoff.firstBarrierStage -or
        -not $MergeHandoff.fixedMergeStage -or -not $MergeHandoff.mergeStage) {
        throw 'literal ordered masstest deferred proof omitted its saved source samples'
    }
    [void](Assert-DeferredLiteralOrderedMasstestStage `
        @($MergeHandoff.firstBarrierStage.savedEvents) `
        first-barrier $firstRole $laggardRole)
    # Revalidate the fixed +4-second source sample as its original classification,
    # then revalidate the bounded observation-only completion used by Phase C.
    # The complete two-phase MSS chain still belongs to the one post-Phase-C read.
    [void](Assert-LiteralOrderedMasstestStage `
        @($MergeHandoff.fixedMergeStage.savedEvents) `
        laggard-merge $firstRole $laggardRole `
        -AllowPendingHostMerge)
    $completedSourceStage = Assert-LiteralOrderedMasstestStage `
        @($MergeHandoff.mergeStage.savedEvents) `
        laggard-merge $firstRole $laggardRole
    $deferredStage = Assert-DeferredLiteralOrderedMasstestStage `
        $events laggard-merge $firstRole $laggardRole
    [void](Assert-DeferredAutomaticMasstestHostBeginTurnSlotSamples `
        $PhaseC $MergeHandoff)
    [long]$mergeActionId = $deferredStage.mergeActionId
    if ($mergeActionId -ne [long]$MergeHandoff.mergeActionId -or
        $mergeActionId -ne [long]$completedSourceStage.mergeActionId) {
        throw 'deferred ordered merge changed the saved source actionId'
    }
    if ([bool]$MergeHandoff.fixedMergeStage.mergeObserved -and
        $mergeActionId -ne [long]$MergeHandoff.fixedMergeStage.mergeActionId) {
        throw 'deferred ordered merge changed the fixed-window source actionId'
    }
    $naturalMergeMarker =
        "[simturns] natural merge BeginTurn applied and drained (actionId=$mergeActionId, day=3)"
    $stockReleasedMarker =
        "[simturns] relay released stock turns (actionId=$mergeActionId, day=3)"
    $preparedMergeMarker =
        "[simturns] prepared merge transaction $mergeActionId at stock day 3"
    $hostExecutedMergeMarker =
        "[simturns] host executed merge transaction $mergeActionId via 0x420FFA"
    $nativeMarkers = [ordered]@{}
    foreach ($roleAndLog in @(
        @{ role = 'host'; path = $HostLog },
        @{ role = 'join'; path = $JoinLog }
    )) {
        [int]$naturalMergeCount = Get-ClientLogMarkerCount `
            $roleAndLog.path $naturalMergeMarker
        [int]$releaseCount = Get-ClientLogMarkerCount `
            $roleAndLog.path $stockReleasedMarker
        [int]$prepareCount = Get-ClientLogMarkerCount `
            $roleAndLog.path $preparedMergeMarker
        [int]$executeCount = Get-ClientLogMarkerCount `
            $roleAndLog.path $hostExecutedMergeMarker
        [int]$expectedExecuteCount = if ($roleAndLog.role -eq 'host') { 1 } else { 0 }
        if ($naturalMergeCount -ne 1 -or $releaseCount -ne 1 -or
            $prepareCount -ne 1 -or $executeCount -ne $expectedExecuteCount) {
            throw ("literal ordered masstest native merge markers for " +
                "$($roleAndLog.role) are natural=$naturalMergeCount " +
                "release=$releaseCount prepare=$prepareCount execute=$executeCount, " +
                "expected 1/1/1/$expectedExecuteCount")
        }
        $nativeMarkers[$roleAndLog.role] = [pscustomobject]@{
            naturalMergeBeginTurnAppliedAndDrained = $naturalMergeCount
            relayReleasedStockTurns = $releaseCount
        }
    }
    Write-Step 'deferred MSS ordered-masstest proof PASS after literal Phase C'
    return [pscustomobject]@{
        proved = $true
        endTurnsPerRole = 2
        cascadePerRole = 1
        barrierCount = 2
        barrierHeldPeers = @($deferredStage.barrierHeldPeers)
        mergeCount = 1
        mergeActionId = $mergeActionId
        fixedMergeClassification = [string]$MergeHandoff.fixedMergeClassification
        fixedMergeObserved = [bool]$MergeHandoff.sourceMergeObserved
        completionMergeObserved = [bool]$MergeHandoff.completionMergeObserved
        mergeCompletionTimeoutSeconds = [int]$MergeHandoff.mergeCompletionTimeoutSeconds
        nativeMarkers = $nativeMarkers
    }
}

function Read-AutomaticMasstestHostBeginTurnSlots([long]$After) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "automatic masstest slot watermark is outside uint32: $After"
    }

    # One call is one old Get-SlotLines equivalent. Callers deliberately invoke
    # this function separately for neutral-count and latest-slot observations;
    # neither observation may be reused or coalesced with the other.
    $history = Get-TurnHistory -After $After -Role host
    $events = @((Get-OptionalProperty $history 'events'))
    $slots = @($events | Where-Object {
        [string](Get-OptionalProperty $_ 'kind') -eq 'stock-begin-turn-applied'
    })
    [long]$latestSlotSequence = $After
    if ($slots.Count -gt 0) {
        $latestSequenceValue = Get-OptionalProperty $slots[-1] 'seq'
        [long]$parsedLatestSequence = 0
        if ($null -ne $latestSequenceValue -and
            [long]::TryParse([string]$latestSequenceValue, [ref]$parsedLatestSequence)) {
            $latestSlotSequence = $parsedLatestSequence
        }
    }
    return [pscustomobject]@{
        # Get-SlotLines exposed only the number of matching lines and its latest
        # line to Parse-Role. Preserve the raw retained records for a deferred
        # proof, but do not validate native schema at this source boundary.
        After = $After
        Count = $slots.Count
        Slots = @($slots)
        LatestSlot = if ($slots.Count -gt 0) { $slots[-1] } else { $null }
        SlotWatermark = $latestSlotSequence
        SavedHistory = $history
    }
}

function Assert-DeferredAutomaticMasstestHostBeginTurnSlotSamples(
    [object]$PhaseC,
    [object]$MergeHandoff) {
    $samples = @((Get-OptionalProperty $PhaseC 'slotSamples'))
    if ($samples.Count -eq 0) {
        throw 'deferred automatic masstest proof received no saved Get-SlotLines samples'
    }
    [int]$validatedSampleCount = 0
    $uniqueSlotsByRelaySequence = @{}
    [long]$expectedHostHandle = Get-RequiredTelemetryNumber `
        $MergeHandoff 'hostHandle' 'automatic masstest merge handoff'
    foreach ($sample in $samples) {
        [long]$after = Get-RequiredTelemetryNumber `
            $sample 'After' 'saved automatic masstest slot sample'
        $history = Get-OptionalProperty $sample 'SavedHistory'
        $events = @((Get-OptionalProperty $history 'events'))
        $slots = @($events | Where-Object {
            [string](Get-OptionalProperty $_ 'kind') -eq 'stock-begin-turn-applied'
        })
        if ([int](Get-OptionalProperty $sample 'Count') -ne $slots.Count -or
            @((Get-OptionalProperty $sample 'Slots')).Count -ne $slots.Count) {
            throw 'saved Get-SlotLines count no longer matches its immutable history sample'
        }
        [long]$previousSequence = $after
        foreach ($slot in $slots) {
            [string]$role = Get-OptionalProperty $slot 'role'
            [long]$sequence = Get-RequiredTelemetryNumber `
                $slot 'seq' 'deferred automatic masstest host BeginTurn slot'
            [long]$senderDpid = Get-RequiredTelemetryNumber `
                $slot 'senderDpid' 'deferred automatic masstest host BeginTurn slot'
            [long]$receiverDpid = Get-RequiredTelemetryNumber `
                $slot 'receiverDpid' 'deferred automatic masstest host BeginTurn slot'
            [long]$frameLength = Get-RequiredTelemetryNumber `
                $slot 'frameLength' 'deferred automatic masstest host BeginTurn slot'
            [long]$dispatchResult = Get-RequiredTelemetryNumber `
                $slot 'dispatchResult' 'deferred automatic masstest host BeginTurn slot'
            [long]$addressee = Get-RequiredTelemetryNumber `
                $slot 'addressee' 'deferred automatic masstest host BeginTurn slot'
            [long]$commandSequence = Get-RequiredTelemetryNumber `
                $slot 'commandSequence' 'deferred automatic masstest host BeginTurn slot'
            [long]$activeHandle = Get-RequiredTelemetryNumber `
                $slot 'activeHandle' 'deferred automatic masstest host BeginTurn slot'
            if ($role -ne 'host' -or $sequence -le $previousSequence -or
                $senderDpid -ne 1 -or $receiverDpid -le 1 -or
                $frameLength -ne 56 -or $dispatchResult -le 0 -or
                $addressee -ne 0 -or $commandSequence -lt 1 -or
                $commandSequence -eq [uint32]::MaxValue -or
                $activeHandle -le 0) {
                throw ("deferred automatic masstest host BeginTurn slot has invalid " +
                    "native identity/order at sequence $sequence")
            }
            [string]$slotKey = [string]$sequence
            if ($uniqueSlotsByRelaySequence.ContainsKey($slotKey)) {
                $prior = $uniqueSlotsByRelaySequence[$slotKey]
                if ([long](Get-OptionalProperty $prior 'commandSequence') -ne
                        $commandSequence -or
                    [long](Get-OptionalProperty $prior 'activeHandle') -ne
                        $activeHandle -or
                    [long](Get-OptionalProperty $prior 'receiverDpid') -ne
                        $receiverDpid) {
                    throw "saved cumulative slot $sequence changed between immutable samples"
                }
            } else {
                $uniqueSlotsByRelaySequence[$slotKey] = $slot
            }
            $previousSequence = $sequence
        }
        [long]$expectedWatermark = if ($slots.Count -gt 0) {
            Get-RequiredTelemetryNumber `
                $slots[-1] 'seq' 'deferred latest automatic masstest slot'
        } else { $after }
        if ([long](Get-OptionalProperty $sample 'SlotWatermark') -ne $expectedWatermark) {
            throw 'saved Get-SlotLines latest sequence does not match its immutable sample'
        }
        $validatedSampleCount++
    }
    $orderedUniqueSlots = @($uniqueSlotsByRelaySequence.Values | Sort-Object {
        Get-RequiredTelemetryNumber `
            $_ 'seq' 'deferred unique automatic masstest slot'
    })
    if ($orderedUniqueSlots.Count -eq 0) {
        throw 'automatic masstest saved no natural host BeginTurn slots'
    }
    $startupSlots = @($orderedUniqueSlots | Where-Object {
        [long](Get-RequiredTelemetryNumber `
            $_ 'commandSequence' 'deferred startup host BeginTurn slot') -eq 1
    })
    [long]$firstActiveHandle = Get-RequiredTelemetryNumber `
        $orderedUniqueSlots[0] 'activeHandle' 'deferred first host BeginTurn slot'
    [long]$firstCommandSequence = Get-RequiredTelemetryNumber `
        $orderedUniqueSlots[0] 'commandSequence' 'deferred first host BeginTurn slot'
    if ($startupSlots.Count -ne 1 -or $firstCommandSequence -ne 1 -or
        $firstActiveHandle -ne $expectedHostHandle) {
        throw ('automatic masstest natural slot history lost its one exact ' +
            'startup host BeginTurn command')
    }
    [long]$wireHighWater = 0
    foreach ($slot in $orderedUniqueSlots) {
        [long]$commandSequence = Get-RequiredTelemetryNumber `
            $slot 'commandSequence' 'deferred unique automatic masstest slot'
        if ($commandSequence -le $wireHighWater) {
            throw ("automatic masstest natural command sequence did not advance " +
                "($wireHighWater->$commandSequence)")
        }
        $wireHighWater = $commandSequence
    }
    return [pscustomobject]@{
        sampleCount = $validatedSampleCount
        uniqueSlotCount = $orderedUniqueSlots.Count
        finalCommandSequence = $wireHighWater
        hostHandle = $expectedHostHandle
        joinHandle = [long](Get-OptionalProperty $MergeHandoff 'joinHandle')
        neutralHandle = [long](Get-OptionalProperty $MergeHandoff 'neutralHandle')
    }
}

function Get-AutomaticMasstestNeutralCount([long]$After,
                                           [long]$NeutralHandle,
                                           [long]$HostHandle = 0,
                                           [int]$ExpectedMergeDay = 0,
                                           [switch]$ResolveMergeBoundary) {
    # This is intentionally its own host-side stock BeginApplied observation.
    $snapshot = Read-AutomaticMasstestHostBeginTurnSlots $After
    [long]$postMergeBoundary = $After
    $eligibleSlots = @($snapshot.Slots)
    if ($ResolveMergeBoundary) {
        if ($After -ne 0 -or $HostHandle -le 0 -or $ExpectedMergeDay -le 0) {
            throw 'automatic masstest merge-boundary resolution has invalid inputs'
        }
        $boundaries = @($eligibleSlots | Where-Object {
            [long](Get-RequiredTelemetryNumber `
                $_ 'activeHandle' 'automatic masstest merge-boundary slot') -eq
                    $HostHandle -and
            [long](Get-RequiredTelemetryNumber `
                $_ 'commandSequence' 'automatic masstest merge-boundary slot') -gt 1 -and
            [long](Get-RequiredTelemetryNumber `
                $_ 'commandSequence' 'automatic masstest merge-boundary slot') -ne
                    [uint32]::MaxValue
        })
        if ($boundaries.Count -ne 1) {
            throw ("automatic masstest first neutral-count read found " +
                "$($boundaries.Count) exact natural host merge slots at world day " +
                "$ExpectedMergeDay, expected one")
        }
        $postMergeBoundary = [long](Get-RequiredTelemetryNumber `
            $boundaries[0] 'seq' 'automatic masstest merge-boundary slot')
        $eligibleSlots = @($eligibleSlots | Where-Object {
            [long](Get-RequiredTelemetryNumber `
                $_ 'seq' 'automatic masstest post-merge slot') -gt
                    $postMergeBoundary
        })
    }
    $count = @($eligibleSlots | Where-Object {
        [long](Get-RequiredTelemetryNumber `
            $_ 'activeHandle' 'automatic masstest neutral slot') -eq
                $NeutralHandle
    }).Count
    return [pscustomobject]@{
        Count = $count
        SlotWatermark = [long]$snapshot.SlotWatermark
        PostMergeBoundary = $postMergeBoundary
        Sample = $snapshot
    }
}

function Get-AutomaticMasstestLatestHostSlot([long]$After,
                                             [long]$HostHandle,
                                             [long]$JoinHandle,
                                             [long]$NeutralHandle) {
    # Do not accept a neutral-count snapshot here: old Phase C called
    # Get-SlotLines again before choosing which one human role to click.
    $snapshot = Read-AutomaticMasstestHostBeginTurnSlots $After
    if ([int]$snapshot.Count -eq 0) {
        return [pscustomobject]@{
            Count = 0
            Role = ''
            ActiveHandle = [long]0
            SlotWatermark = [long]$snapshot.SlotWatermark
            Sample = $snapshot
        }
    }
    $latest = $snapshot.Slots[-1]
    [long]$activeHandle = [long]$latest.ActiveHandle
    [string]$role = if ($activeHandle -eq $HostHandle) {
        'HOST'
    } elseif ($activeHandle -eq $JoinHandle) {
        'JOIN'
    } elseif ($activeHandle -eq $NeutralHandle) {
        'NEUTRAL'
    } else {
        'UNKNOWN'
    }
    return [pscustomobject]@{
        Count = [int]$snapshot.Count
        Role = $role
        ActiveHandle = $activeHandle
        SlotWatermark = [long]$snapshot.SlotWatermark
        Sample = $snapshot
    }
}

function Test-AutomaticMasstestHostAlive(
    [System.Diagnostics.Process]$HostProcess) {
    $HostProcess.Refresh()
    return -not $HostProcess.HasExited
}

function Invoke-AutomaticMasstestHumanEndTurnOnce([string]$Role) {
    if ($Role -notin @('host', 'join')) {
        throw "automatic masstest cannot End Turn non-human role '$Role'"
    }
    # The old slot-log read cannot carry protocol-v8 UI ownership. Perform one
    # separately-labelled adapter read for a UI-sequence watermark only, then
    # arm one symbolic action. The relay captures the current or first later
    # exact ready native owner and submits once; neither read nor intent repeats.
    $adapterState = Get-RoleState $Role
    [long]$uiSequence = Get-RequiredTelemetryNumber `
        $adapterState 'uiSeq' "automatic masstest $Role UI adapter"
    if ($uiSequence -lt 1) {
        throw "automatic masstest $Role UI sequence must be positive: $uiSequence"
    }
    # `uiSeq > after` admits an already-actionable current publication. If the
    # slot arrived during a transient UI publication, the same sole intent waits
    # for the next exact ready DLG_STRATEGIC owner within the source's existing
    # 26 s slot-advance budget.
    $invokeResult = Invoke-ButtonWhenReady `
        -Role $Role `
        -Dialog 'DLG_STRATEGIC' `
        -Button 'BTN_END_TURN' `
        -AfterUiSequence ([long]($uiSequence - 1)) `
        -WaitMilliseconds 26000 `
        -CommandTimeoutMilliseconds 8000
    return [pscustomobject]@{
        role = $Role
        appearance = [long]$invokeResult.invoke.appearance
        instance = [long]$invokeResult.invoke.instance
        uiSequence = [long]$invokeResult.observation.uiSeq
        attempts = 1
        accepted = 1
        recoveryActions = 0
        transportAdapterReads = 1
        readinessIntentPosts = 1
        readinessWaitMilliseconds = 26000
    }
}

function Run-AutomaticMasstestPhaseCLiteral(
    [object]$MergeHandoff,
    [System.Diagnostics.Process]$HostProcess) {
    if (-not $MergeHandoff -or
        -not [bool](Get-OptionalProperty $MergeHandoff 'automaticMasstestHandoff')) {
        throw 'automatic masstest Phase C requires the immediate ordered-merge handoff'
    }
    [string]$order = Get-OptionalProperty $MergeHandoff 'order'
    if ($order -notin @('host-first', 'join-first')) {
        throw "automatic masstest Phase C cannot follow barrier order '$order'"
    }
    [DateTime]$handoffCompletedUtc =
        [DateTime](Get-OptionalProperty $MergeHandoff 'handoffCompletedUtc')
    [long]$slotBaseline = [long](Get-OptionalProperty `
        $MergeHandoff 'slotWatermarkBeforePhaseC')
    [long]$hostHandle = [long](Get-OptionalProperty $MergeHandoff 'hostHandle')
    [long]$joinHandle = [long](Get-OptionalProperty $MergeHandoff 'joinHandle')
    [long]$neutralHandle = [long](Get-OptionalProperty $MergeHandoff 'neutralHandle')
    $distinctHandles = @(@(
        $hostHandle, $joinHandle, $neutralHandle
    ) | Select-Object -Unique)
    if ($slotBaseline -lt 0 -or
        $hostHandle -le 0 -or $joinHandle -le 0 -or $neutralHandle -le 0 -or
        $distinctHandles.Count -ne 3) {
        throw 'automatic masstest Phase C handoff has invalid slot/handle baselines'
    }

    # Exact old line 185: Phase C starts at merge-completion handoff +4 s.
    Wait-FixedUtcAnchor ($handoffCompletedUtc.AddSeconds(4))

    [int]$completedNeutralRounds = 0
    [int]$outerTurns = 0
    [long]$slotWatermark = $slotBaseline
    [bool]$mergeBoundaryResolved = $slotBaseline -gt 0
    # Old Get-SlotLines always read the complete host hook log when choosing
    # the current active role.  Only Count-PostMergeNeutrals filtered after the
    # merge marker.  Keep those two independent source reads on different
    # origins so the merge-boundary HOST slot can drive the first Phase-C turn.
    [long]$latestSlotHistoryOrigin = 0
    $humanActions = [System.Collections.Generic.List[object]]::new()
    $slotSamples = [System.Collections.Generic.List[object]]::new()
    [bool]$humanSlotStalled = $false
    [string]$stalledHumanRole = ''
    [int]$stalledOuterTurn = -1
    while ($true) {
        # Exact old while condition evaluates Count-PostMergeNeutrals first,
        # including once more when postTurns has just reached its cap.  Keep
        # that condition read distinct from both the slot read and final tally.
        $neutralObservation = if ($mergeBoundaryResolved) {
            Get-AutomaticMasstestNeutralCount $slotBaseline $neutralHandle
        } else {
            # Count-PostMergeNeutrals scanned from the first merge marker in the
            # same one file read. Resolve the typed natural-host equivalent inside
            # this first and still-single neutral-count observation.
            Get-AutomaticMasstestNeutralCount `
                $slotBaseline $neutralHandle $hostHandle $MergeHandoff.mergeDay `
                -ResolveMergeBoundary
        }
        $slotSamples.Add($neutralObservation.Sample)
        if (-not $mergeBoundaryResolved) {
            $slotBaseline = [long]$neutralObservation.PostMergeBoundary
            $mergeBoundaryResolved = $true
        }
        $completedNeutralRounds = [int]$neutralObservation.Count
        $slotWatermark = [Math]::Max(
            $slotWatermark, [long]$neutralObservation.SlotWatermark)
        if ($completedNeutralRounds -ge 2 -or $outerTurns -ge 24) { break }

        if (-not (Test-AutomaticMasstestHostAlive $HostProcess)) { break }
        $slotObservation = Get-AutomaticMasstestLatestHostSlot `
            $latestSlotHistoryOrigin $hostHandle $joinHandle $neutralHandle
        $slotSamples.Add($slotObservation.Sample)
        $slotWatermark = [Math]::Max(
            $slotWatermark, [long]$slotObservation.SlotWatermark)
        if ([int]$slotObservation.Count -eq 0) {
            Start-Sleep -Seconds 2
            $outerTurns++
            continue
        }

        [bool]$humanActionSubmitted = $false
        if ([string]$slotObservation.Role -eq 'HOST') {
            $humanActions.Add((Invoke-AutomaticMasstestHumanEndTurnOnce host))
            $humanActionSubmitted = $true
        } elseif ([string]$slotObservation.Role -eq 'JOIN') {
            $humanActions.Add((Invoke-AutomaticMasstestHumanEndTurnOnce join))
            $humanActionSubmitted = $true
        }
        # NEUTRAL/UNKNOWN performs no action: the authoritative host AI owns it.
        [int]$beforeSlotCount = [int]$slotObservation.Count
        [DateTime]$advanceDeadlineUtc = [DateTime]::UtcNow.AddSeconds(26)
        $slotAdvanced = $false
        $hostDiedDuringAdvance = $false
        while ([DateTime]::UtcNow -lt $advanceDeadlineUtc) {
            Start-Sleep -Milliseconds 800
            if (-not (Test-AutomaticMasstestHostAlive $HostProcess)) {
                $hostDiedDuringAdvance = $true
                break
            }
            $freshSlotObservation = Get-AutomaticMasstestLatestHostSlot `
                $latestSlotHistoryOrigin $hostHandle $joinHandle $neutralHandle
            $slotSamples.Add($freshSlotObservation.Sample)
            if ([int]$freshSlotObservation.Count -lt $beforeSlotCount) {
                throw 'automatic masstest host slot history regressed'
            }
            $slotWatermark = [Math]::Max(
                $slotWatermark, [long]$freshSlotObservation.SlotWatermark)
            if ([int]$freshSlotObservation.Count -gt $beforeSlotCount) {
                $slotAdvanced = $true
                break
            }
        }
        if (-not $slotAdvanced -and $humanActionSubmitted -and
            -not $hostDiedDuringAdvance) {
            # The source returned to its outer loop here, but doing so would
            # submit the same observed human slot again. Preserve exact-once by
            # ending the drive, not by manufacturing a transport failure: the
            # separate final neutral read and old process/log/dump oracle below
            # must classify the short outcome as POSTMERGE-SHORT (or an earlier
            # source verdict such as FAIL-HOSTDIED).
            $humanSlotStalled = $true
            $stalledHumanRole = [string]$slotObservation.Role
            $stalledOuterTurn = $outerTurns + 1
            Write-Step ("automatic masstest Phase C human slot stalled for 26 " +
                "seconds at outer turn $stalledOuterTurn; single submission; " +
                'deferring classification to the legacy outcome oracle')
            $outerTurns++
            break
        }
        $outerTurns++
    }

    # Exact old line 212: a final separate neutral-count read supplies postDays
    # to the following live outcome oracle.  Do not turn a short run or host
    # death into a transport exception here: the source classified both only
    # after its process/log/dump observations.
    $finalNeutralObservation = Get-AutomaticMasstestNeutralCount `
        $slotBaseline $neutralHandle
    $slotSamples.Add($finalNeutralObservation.Sample)
    $completedNeutralRounds = [int]$finalNeutralObservation.Count
    $slotWatermark = [Math]::Max(
        $slotWatermark, [long]$finalNeutralObservation.SlotWatermark)
    Write-Step ("automatic masstest Phase C drive completed: " +
        "$completedNeutralRounds neutral/monster rounds in $outerTurns outer turns")
    return [pscustomobject]@{
        requiredNeutralRounds = 2
        completedNeutralRounds = $completedNeutralRounds
        outerTurns = $outerTurns
        maxOuterTurns = 24
        pollMilliseconds = 800
        advanceTimeoutSeconds = 26
        initialSettleSeconds = 4
        noSlotSleepSeconds = 2
        slotWatermark = $slotWatermark
        postMergeSlotBoundary = $slotBaseline
        attemptedHumanActions = $humanActions.Count
        acceptedHumanActions = $humanActions.Count
        recoveryActions = 0
        transportAdapterReads = $humanActions.Count
        humanActions = @($humanActions)
        slotSamples = @($slotSamples)
        humanSlotStalled = $humanSlotStalled
        stalledHumanRole = $stalledHumanRole
        stalledOuterTurn = $stalledOuterTurn
    }
}

function Complete-LiteralOrderedMasstestLegacyOracle(
    [object]$MergeHandoff,
    [object]$PhaseC,
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [string]$HostLog,
    [object]$DumpBaseline) {
    if (-not $MergeHandoff -or -not $PhaseC -or -not $DumpBaseline) {
        throw 'literal ordered masstest legacy oracle received an incomplete handoff'
    }
    [int]$mergeDay = [int](Get-OptionalProperty $MergeHandoff 'mergeDay')
    [long]$mergeActionId = Get-RequiredTelemetryNumber `
        $MergeHandoff 'mergeActionId' 'literal ordered merge handoff'
    if ($mergeDay -ne 3 -or
        @((Get-OptionalProperty $DumpBaseline 'roots')).Count -eq 0) {
        throw 'literal ordered masstest legacy oracle has invalid merge/dump identity'
    }

    # Exact old lines 216-222: two independent live process observations, host
    # first; then one complete host-log read serving mergeFired/uiMarshalled and
    # the later fault classification.  Join liveness is retained as an
    # observation only -- it was not a legacy verdict gate.
    $HostProcess.Refresh()
    $hostAlive = -not $HostProcess.HasExited
    $JoinProcess.Refresh()
    $joinerAlive = -not $JoinProcess.HasExited
    $hostLines = @(Read-ClientLogLines $HostLog)
    $mergeMarker =
        "[simturns] host executed merge transaction $mergeActionId via 0x420FFA"
    $mergeMatches = @($hostLines | Select-String -SimpleMatch -Pattern $mergeMarker)
    $mergeFired = [bool]($mergeMatches.Count -gt 0)
    $naturalMergeMarker =
        "[simturns] natural merge BeginTurn applied and drained (actionId=$mergeActionId, day=$mergeDay)"
    $uiMarshalled = [bool](@($hostLines |
        Select-String -SimpleMatch -Pattern $naturalMergeMarker).Count -gt 0)

    # Exact old lines 232-240: classify only records after the first completed
    # host merge marker.  [UEF] stays deliberately generic, while caught access
    # violations retain the old known-navigation-noise classification.
    $vehAfterMerge = 0
    $suspect = 0
    $uefAfterMerge = 0
    $mergeSelection = $mergeMatches | Select-Object -First 1
    if ($mergeSelection -and $mergeSelection.LineNumber -lt $hostLines.Count) {
        $after = $hostLines[$mergeSelection.LineNumber..($hostLines.Count - 1)]
        $uefAfterMerge = @($after | Select-String -Pattern '\[UEF').Count
        $vehLines = @($after | Select-String -Pattern '\[VEH .*code=0xC0000005')
        $vehAfterMerge = $vehLines.Count
        $benign = ('findBtn AV|auto-dismiss|nav will re' +
            'try|lastNavStep=|' +
            '0F B7 48 7A|8B 73 04 3B 77|8B 5F 0C 8B 73|66 8B 46 0C')
        $suspect = @($vehLines | Where-Object { $_.Line -notmatch $benign }).Count
    }

    # The source deleted stale dumps immediately after boot, then enumerated
    # the directory here.  The removable harness preserves files: compare this
    # one post-oracle enumeration with the baseline captured at that same old
    # post-boot edge and count only new or changed dumps.
    $currentDumpSnapshot = Get-LiteralInnerDumpBaseline `
        -DumpRoots @($DumpBaseline.roots)
    $baselineDumps = @{}
    foreach ($dump in @($DumpBaseline.files)) {
        $baselineDumps[[string]$dump.path] = $dump
    }
    $changedDumps = @($currentDumpSnapshot.files | Where-Object {
        $beforeDump = $baselineDumps[[string]$_.path]
        $null -eq $beforeDump -or
        [long]$beforeDump.length -ne [long]$_.length -or
        [string]$beforeDump.lastWriteUtc -ne [string]$_.lastWriteUtc -or
        [string]$beforeDump.sha256 -ne [string]$_.sha256
    })
    $dumps = $changedDumps.Count

    # Exact old lines 247-252: order matters. uiMarshalled and joinerAlive are
    # credibility observations, not legacy verdict gates.
    $verdict = 'PASS'
    if (-not $hostAlive) {
        $verdict = 'FAIL-HOSTDIED'
    } elseif ($dumps -gt 0 -or $uefAfterMerge -gt 0) {
        $verdict = 'FAIL-FAULT'
    } elseif (-not $mergeFired) {
        $verdict = 'NOMERGE'
    } elseif ($suspect -gt 0) {
        $verdict = 'SUSPECT'
    } elseif ([int]$PhaseC.completedNeutralRounds -lt 2) {
        $verdict = 'POSTMERGE-SHORT'
    }

    Write-Step ("literal test_R_masstest legacy outcome: $verdict; " +
        "hostAlive=$hostAlive joinerAlive=$joinerAlive mergeFired=$mergeFired " +
        "uiMarshalled=$uiMarshalled postDays=$($PhaseC.completedNeutralRounds) " +
        "vehAfterMerge=$vehAfterMerge suspect=$suspect " +
        "uef=$uefAfterMerge dumps=$dumps")
    return [pscustomobject]@{
        legacyVerdict = $verdict
        hostPid = $HostProcess.Id
        joinPid = $JoinProcess.Id
        hostAlive = $hostAlive
        joinerAlive = $joinerAlive
        mergeFired = $mergeFired
        uiMarshalled = $uiMarshalled
        postDays = [int]$PhaseC.completedNeutralRounds
        postTurns = [int]$PhaseC.outerTurns
        vehAfterMerge = $vehAfterMerge
        suspect = $suspect
        uefAfterMerge = $uefAfterMerge
        uef = $uefAfterMerge
        dumps = $dumps
        dumpFiles = @($changedDumps)
        mergeMarker = $mergeMarker
    }
}

function Assert-DeferredCanonicalRoundEvidence([object[]]$Events,
                                                [object]$Evidence) {
    if (-not $Evidence -or -not $Evidence.fire -or
        -not $Evidence.eventsBefore) {
        throw 'deferred canonical round evidence is incomplete'
    }
    $fire = $Evidence.fire

    # This check intentionally runs only after barrier_endturn's PASS. Isolate
    # the earlier sync_endturn transaction at its two exact terminal
    # turn-start-complete records. The following barrier's EndTurnApplied may
    # legitimately precede EndTurnObserved, so neither next signal is a safe
    # prefix boundary. All source-visible M9 assertions were already made before
    # its PASS; this prefix supplies only the deferred MSS causal strengthening.
    $hostRoundComplete = Get-NewExactSimEvent `
        $Events 'turn-start-complete' 'host' $Evidence.completeHostBefore
    $joinRoundComplete = Get-NewExactSimEvent `
        $Events 'turn-start-complete' 'join' $Evidence.completeJoinBefore
    [int]$hostCompleteIndex = Get-SimEventRecordIndex $Events $hostRoundComplete
    [int]$joinCompleteIndex = Get-SimEventRecordIndex $Events $joinRoundComplete
    [int]$cutoff = [Math]::Max($hostCompleteIndex, $joinCompleteIndex)
    if ($hostCompleteIndex -lt 0 -or $joinCompleteIndex -lt 0 -or $cutoff -le 0) {
        throw 'deferred canonical round proof derived an invalid completion boundary'
    }

    # Retain the old cross-transaction separation proof without depending on
    # the two independent next signals' mutual order.
    foreach ($next in @(
        @{ event = 'end-turn-observed'; role = 'host'; before = $fire.beforeHostObserved },
        @{ event = 'end-turn-observed'; role = 'join'; before = $fire.beforeJoinObserved },
        @{ event = 'end-turn-applied'; role = 'host'; before = $fire.beforeHostApplied },
        @{ event = 'end-turn-applied'; role = 'join'; before = $fire.beforeJoinApplied }
    )) {
        $matches = @(Get-SimEventMatches $Events $next.event $next.role)
        [int]$nextIndex = [int]$next.before + 1
        if ($matches.Count -le $nextIndex -or
            (Get-SimEventRecordIndex $Events $matches[$nextIndex]) -le $cutoff) {
            throw ("deferred canonical round proof cannot separate the following " +
                "$($next.event)/$($next.role) signal")
        }
    }
    $roundEvents = @($Events[0..$cutoff])
    Assert-NoRelayFault $roundEvents

    foreach ($check in @(
        @{ event = 'end-turn-observed'; before = $fire.beforeHostObserved; role = 'host' },
        @{ event = 'end-turn-observed'; before = $fire.beforeJoinObserved; role = 'join' },
        @{ event = 'end-turn-applied'; before = $fire.beforeHostApplied; role = 'host' },
        @{ event = 'end-turn-applied'; before = $fire.beforeJoinApplied; role = 'join' },
        @{ event = 'end-turn-accepted'; before = $fire.beforeHostAccepted; role = 'host' },
        @{ event = 'end-turn-accepted'; before = $fire.beforeJoinAccepted; role = 'join' },
        @{ event = 'turn-start-complete'; before = $Evidence.completeHostBefore; role = 'host' },
        @{ event = 'turn-start-complete'; before = $Evidence.completeJoinBefore; role = 'join' }
    )) {
        Assert-SimEventDelta $roundEvents $check.event $check.before 1 $check.role
    }
    Assert-EngineActionDelta `
        $roundEvents 'ordinary-apply' 'host' $fire.expectedHostHandle `
        $Evidence.applyHostBefore 1
    Assert-EngineActionDelta `
        $roundEvents 'ordinary-apply' 'host' $fire.expectedJoinHandle `
        $Evidence.applyJoinBefore 1
    Assert-EngineActionDelta `
        $roundEvents 'ordinary-activate' 'host' $fire.expectedHostHandle `
        $Evidence.activateHostBefore 1
    Assert-EngineActionDelta `
        $roundEvents 'ordinary-activate' 'join' $fire.expectedJoinHandle `
        $Evidence.activateJoinBefore 1

    $hostBarrier = Assert-ExactEndTurnTransaction `
        $roundEvents host `
        $fire.beforeHostObserved $fire.beforeHostApplied $fire.beforeHostAccepted `
        ([int]$fire.expectedHostCompletedDay)
    $joinBarrier = Assert-ExactEndTurnTransaction `
        $roundEvents join `
        $fire.beforeJoinObserved $fire.beforeJoinApplied $fire.beforeJoinAccepted `
        ([int]$fire.expectedJoinCompletedDay)
    $cascadeProof = Assert-CanonicalRoundCascadeProof `
        $roundEvents $fire `
        $Evidence.applyHostBefore $Evidence.applyJoinBefore `
        $Evidence.activateHostBefore $Evidence.activateJoinBefore `
        $Evidence.completeHostBefore $Evidence.completeJoinBefore `
        ([int]$Evidence.completedDay + 1)
    $nativeApplyActions = @($Evidence.nativeApplyActions)
    if ($nativeApplyActions.Count -ne 2 -or -not $Evidence.localTurnRelease) {
        throw 'deferred canonical round omitted its exact native v8 action witnesses'
    }
    foreach ($cascade in @($cascadeProof.cascades)) {
        [long]$cascadeHandle = Convert-ExactPlayerHandle `
            $cascade.handle "deferred canonical $($cascade.role) cascade handle"
        $native = @($nativeApplyActions | Where-Object {
            [long]$_.handle -eq $cascadeHandle
        })
        if ($native.Count -ne 1 -or
            [long]$native[0].actionId -ne [long]$cascade.actionId -or
            [long]$native[0].lease -ne [long]$cascade.lease -or
            [int]$native[0].day -ne [int]$cascade.day) {
            throw "deferred canonical $($cascade.role) native ApplyTurnStart witness drifted from relay evidence"
        }
        [long]$nativeActivateActionId = if ($cascade.role -eq 'host') {
            [long]$Evidence.localTurnRelease.hostActionId
        } else { [long]$Evidence.localTurnRelease.joinActionId }
        if ($nativeActivateActionId -ne [long]$cascade.activationActionId) {
            throw "deferred canonical $($cascade.role) native ActivateTurn witness drifted from relay evidence"
        }
    }
    $confirmationCount = Assert-CanonicalNoEndTurnConfirmations $fire
    return [pscustomobject]@{
        hostBarrier = $hostBarrier
        joinBarrier = $joinBarrier
        cascades = $cascadeProof.cascades
        cascadeOrder = $cascadeProof.order
        confirmationCount = [int]$confirmationCount
    }
}

function Run-MergeBarrier([System.Diagnostics.Process]$RelayProcess,
                          [System.Diagnostics.Process]$HostProcess,
                          [System.Diagnostics.Process]$JoinProcess,
                          [string]$HostLog,
                          [string]$JoinLog,
                          [ValidateSet('parallel', 'host-first', 'join-first')]
                          [string]$Order = 'parallel',
                           [object]$Fixture = $null,
                           [object]$PreparedCanonicalEvidence = $null,
                           [switch]$CanonicalLegacy) {
    $isLegacyParallel = [bool]$CanonicalLegacy -and $Order -eq 'parallel'
    $isAutomaticMasstestPhaseC =
        $PostMergeContinuationMode -eq 'automatic-masstest-phase-c-literal'
    if ($isAutomaticMasstestPhaseC -and
        $Order -notin @('host-first', 'join-first')) {
        throw 'automatic masstest Phase C requires an ordered merge barrier'
    }
    $naturalMergeMarker =
        '[simturns] natural merge BeginTurn applied and drained (actionId='
    $mergeReleasedMarker = '[simturns] relay released stock turns (actionId='
    $hostLocalMergeMarker = '[simturns] host executed merge transaction '
    $preparedMergeMarker = '[simturns] prepared merge transaction '
    $canonicalIntent = $null
    $postMergeProbeBaseline = $null
    $legacyMergeVitalsBefore = $null
    $legacyFixtureMovementBeforeMatched = $null
    $legacyBarrierPass = $false
    $legacyBarrierEvidence = $null
    [long]$mergeTurnWatermark = 0
    [long]$automaticNeutralHandle = 0

    if ($isLegacyParallel) {
        if (-not $PreparedCanonicalEvidence -or
            -not $PreparedCanonicalEvidence.baselineEvents) {
            throw 'canonical merge omitted predecessor event evidence'
        }
        # barrier_endturn.ps1 exact pre-fire success path:
        # process -> stacks reachability -> one peer state -> HeroId H -> HeroId J
        # -> quiet state H -> state J -> quiet log H -> log J -> MP H -> MP J
        # -> one cascade watermark -> immediate parallel fire.
        $ownedProcessIds = @([int]$HostProcess.Id, [int]$JoinProcess.Id)
        $ownedProcessesBefore = @(Get-Process `
            -Id $ownedProcessIds -ErrorAction SilentlyContinue)
        if ($ownedProcessesBefore.Count -ne 2) {
            throw "merge barrier precondition found $($ownedProcessesBefore.Count)/2 owned processes"
        }
        $stackReachability = Get-LegacyStackSnapshot
        if (-not $stackReachability -or
            @((Get-OptionalProperty $stackReachability 'stacks')).Count -lt 2) {
            throw 'merge barrier shared raw stack reporter precondition failed'
        }
        $peerState = Get-RelayState
        Assert-DebugRelayClientIdentity `
            host (Get-OptionalProperty $peerState 'host') $HostProcess
        Assert-DebugRelayClientIdentity `
            join (Get-OptionalProperty $peerState 'join') $JoinProcess
        $hostHeroCensus = Get-LegacyStackSnapshot
        [void](Get-WorldStackExact `
            $hostHeroCensus ([string]$Fixture.host.heroId))
        $joinHeroCensus = Get-LegacyStackSnapshot
        [void](Get-WorldStackExact `
            $joinHeroCensus ([string]$Fixture.join.heroId))

        # barrier_endturn.ps1 calls the same wait_day_ready.ps1 gate as attack:
        # bounded read-only +800 ms sampling, 45 seconds, and no repeated action.
        $mergeReady = Wait-LegacyReadyQuietPair `
            -HostLog $HostLog -JoinLog $JoinLog `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -QuietSec 3 -TimeoutSec 45 -PollMilliseconds 800
        $quietHostState = $mergeReady.hostState
        $quietJoinState = $mergeReady.joinState
        $mergeActionPreparation = $mergeReady.preparation
        $quietHostLog = $mergeReady.hostQuiet
        $quietJoinLog = $mergeReady.joinQuiet
        $legacyMergeVitalsBefore = Get-LegacySequentialSharedMovementSnapshot $Fixture
        $legacyFixtureMovementBeforeMatched =
            [int]$legacyMergeVitalsBefore.host.movement -eq
                [int]$Fixture.host.day2Reverse.movement -and
            [int]$legacyMergeVitalsBefore.join.movement -eq
                [int]$Fixture.join.day2Reverse.movement
        $hostBefore = $MergeDay - 1
        $joinBefore = $MergeDay - 1
        # The source's cascade watermark is one host-hook scan immediately after
        # the two MP reads. Production event baselines came from the predecessor
        # witness and are consumed only as in-memory data here.
        $legacyCascadeBaselineLines = @(Read-ClientLogLines $HostLog)
        $legacyCascadePattern =
            ('\[simturns\] ApplyTurnStart [0-9]+ completed for ' +
             'handle=0x[0-9a-fA-F]+, day=[0-9]+, lease=[0-9]+')
        $legacyCascadeBefore = @($legacyCascadeBaselineLines | Where-Object {
            $_ -match $legacyCascadePattern
        }).Count
        $before = @($PreparedCanonicalEvidence.baselineEvents)
        $mergeTurnWatermark = [long](Get-OptionalProperty `
            $PreparedCanonicalEvidence 'stockTurnWatermark')
        if ($mergeTurnWatermark -le 0) {
            throw 'canonical merge omitted the saved startup turn watermark'
        }
        $canonicalIntent = New-LiteralPreparedEndTurnIntent `
            $before $hostBefore $mergeActionPreparation $legacyMergeVitalsBefore
        # Exactly one earlier independent End Turn per role is already proved by
        # M9. Preserve those counters without reading the production relay here.
        $canonicalIntent.beforeHostObserved = 1
        $canonicalIntent.beforeJoinObserved = 1
        $canonicalIntent.beforeHostApplied = 1
        $canonicalIntent.beforeJoinApplied = 1
        $canonicalIntent.beforeHostAccepted = 1
        $canonicalIntent.beforeJoinAccepted = 1
        $postMergeProbeBaseline = [pscustomobject]@{
            uiWatermark = [long](Get-OptionalProperty $quietHostState 'uiSeq')
            confirmationLinesBefore = 0
            confirmationLinePattern =
                ('\[testdrv\]\[scripted-popup\] (OBSERVED|CLAIMED|COMMITTED) ' +
                 'role=host dialog=DLG_MESSAGE_BOX\b')
            overrotationHostUi = $null
            suppressCount = 0
            preMergeTurnWatermark = $mergeTurnWatermark
        }
        $hostNaturalMergeBefore = 0
        $joinNaturalMergeBefore = 0
        $hostMergeReleasedBefore = 0
        $joinMergeReleasedBefore = 0
        $hostLocalMergeBefore = 0
        $hostPreparedMergeBefore = 0
        $joinPreparedMergeBefore = 0
    } else {
        Assert-OperationalActionGate $HostLog $JoinLog
        $hostBefore = Get-WorldDay host
        $joinBefore = Get-WorldDay join
        if ($hostBefore -ne ($MergeDay - 1) -or $joinBefore -ne ($MergeDay - 1)) {
            throw "merge barrier precondition failed: local days are $hostBefore/$joinBefore, expected $($MergeDay - 1)"
        }
        $before = @(Read-SimRelayEvents)
        $hostNaturalMergeBefore = Get-ClientLogMarkerCount `
            $HostLog $naturalMergeMarker
        $joinNaturalMergeBefore = Get-ClientLogMarkerCount `
            $JoinLog $naturalMergeMarker
        $hostMergeReleasedBefore = Get-ClientLogMarkerCount $HostLog $mergeReleasedMarker
        $joinMergeReleasedBefore = Get-ClientLogMarkerCount $JoinLog $mergeReleasedMarker
        $hostLocalMergeBefore = Get-ClientLogMarkerCount $HostLog $hostLocalMergeMarker
        $hostPreparedMergeBefore = Get-ClientLogMarkerCount `
            $HostLog $preparedMergeMarker
        $joinPreparedMergeBefore = Get-ClientLogMarkerCount `
            $JoinLog $preparedMergeMarker
        $barrierReady = Wait-ActionablePair -TimeoutSec 45 -QuietSec 3
        if (-not $barrierReady) {
            throw 'merge barrier: both clients did not expose a stable strategic map'
        }
        if ($Fixture) {
            $legacyMergeVitalsBefore = Get-LegacySequentialSharedMovementSnapshot $Fixture
        }
        if ($isAutomaticMasstestPhaseC) {
            # Resolve all three player identities before the sole merge fire.
            # Phase C receives only immutable scalars at its immediate handoff.
            $automaticNeutralHandle = Get-NeutralPlayerHandle
        }
        $mergeTurnWatermark = Assert-NoStockEndTurnSnapshotAfter 0
    }

    $sessionPlan = Get-SessionPlanEvent $before
    [long]$expectedHostHandle = Convert-ExactPlayerHandle `
        (Get-OptionalProperty $sessionPlan 'hostHandle') 'session host handle'
    [long]$expectedJoinHandle = Convert-ExactPlayerHandle `
        (Get-OptionalProperty $sessionPlan 'joinHandle') 'session join handle'
    $ordinaryApplyBefore = Get-EngineActionCount $before 'ordinary-apply'
    $ordinaryActivateBefore = Get-EngineActionCount $before 'ordinary-activate'
    if ($isLegacyParallel) {
        # M9 proved exactly two additional serialized v8 turn actions after this
        # saved predecessor baseline. The merge barrier itself must add neither.
        $ordinaryApplyBefore += 2
        $ordinaryActivateBefore += 2
    }
    $hostBarrierBefore = Get-SimEventCount $before 'barrier-held' 'host'
    $joinBarrierBefore = Get-SimEventCount $before 'barrier-held' 'join'
    $prepareDispatchBefore = Get-SimEventCount $before 'merge-prepare-dispatched'
    $hostPrepareAppliedBefore = Get-SimEventCount `
        $before 'merge-prepare-applied' 'host'
    $joinPrepareAppliedBefore = Get-SimEventCount `
        $before 'merge-prepare-applied' 'join'
    $executeDispatchBefore = Get-SimEventCount $before 'merge-execute-dispatched'
    $executeAppliedBefore = Get-SimEventCount $before 'merge-execute-applied'
    $hostMergeAppliedBefore = Get-SimEventCount $before 'merge-applied' 'host'
    $joinMergeAppliedBefore = Get-SimEventCount $before 'merge-applied' 'join'
    $mergeReleasedBefore = Get-SimEventCount $before 'merge-released'

    foreach ($baseline in @(
        $hostBarrierBefore,
        $joinBarrierBefore,
        $prepareDispatchBefore,
        $hostPrepareAppliedBefore,
        $joinPrepareAppliedBefore,
        $executeDispatchBefore,
        $executeAppliedBefore,
        $hostMergeAppliedBefore,
        $joinMergeAppliedBefore,
        $mergeReleasedBefore,
        $hostNaturalMergeBefore,
        $joinNaturalMergeBefore,
        $hostMergeReleasedBefore,
        $joinMergeReleasedBefore,
        $hostLocalMergeBefore,
        $hostPreparedMergeBefore,
        $joinPreparedMergeBefore
    )) {
        if ($baseline -ne 0) {
            throw 'merge barrier evidence appeared before its single causal End Turn pair'
        }
    }

    $mergeEventChecks = @(
        @{ event = 'barrier-held'; role = 'host'; before = $hostBarrierBefore },
        @{ event = 'barrier-held'; role = 'join'; before = $joinBarrierBefore },
        @{ event = 'merge-prepare-dispatched'; role = ''; before = $prepareDispatchBefore },
        @{ event = 'merge-prepare-applied'; role = 'host'; before = $hostPrepareAppliedBefore },
        @{ event = 'merge-prepare-applied'; role = 'join'; before = $joinPrepareAppliedBefore },
        @{ event = 'merge-execute-dispatched'; role = ''; before = $executeDispatchBefore },
        @{ event = 'merge-execute-applied'; role = ''; before = $executeAppliedBefore },
        @{ event = 'merge-applied'; role = 'host'; before = $hostMergeAppliedBefore },
        @{ event = 'merge-applied'; role = 'join'; before = $joinMergeAppliedBefore },
        @{ event = 'merge-released'; role = ''; before = $mergeReleasedBefore }
    )

    Write-Step "merge barrier: $Order End Turn arrival at day $($MergeDay - 1) -> $MergeDay"
    $fire = if ($isLegacyParallel) {
        Invoke-CanonicalPreparedEndTurnPair `
            $canonicalIntent -RelayHealthAlreadyAsserted
    } elseif ($Order -eq 'parallel') {
        Invoke-EndTurnsAndWaitAccepted $RelayProcess
    } else {
        Invoke-OrderedMergeEndTurnsAndWaitAccepted `
            -RelayProcess $RelayProcess `
            -Order $Order `
            -ExpectedCompletedDay ($MergeDay - 1)
    }
    $events = $null
    $fiveSecondWindowComplete = $false
    $legacyMergeObservation = $null
    $legacyMergeVitalsAfter = $null
    $noOverrotation = $null
    $currentMergeOwner = $null
    $firstHostStep = $null
    $deferredRoundProof = $null
    if ($isLegacyParallel) {
        $legacyMergeObservation = Wait-CanonicalLegacyMergeRelease `
            $RelayProcess $HostProcess $JoinProcess $HostLog $JoinLog `
            $mergeReleasedMarker $mergeReleasedMarker `
            $hostMergeReleasedBefore $joinMergeReleasedBefore $fire.completedAt 45
        # In the old DLL, `[merge] MERGE fired` meant do_merge had completed for
        # that process. The MSS equivalent is the exact final global-release
        # application, not the earlier local host/join handoff markers. Start the
        # unchanged source +5 window only after both clients have applied it.
        Start-Sleep -Seconds 5
        # Exact barrier_endturn order: the very first observations after +5 are
        # S(host) and then S(join), each a distinct read of the shared census.
        # Protocol/log/day extensions are intentionally below this checkpoint.
        $legacyMergeVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture
        $legacyHostRefreshed = [int]$legacyMergeVitalsAfter.host.movement -eq 35
        $legacyJoinMovementObservation =
            if ([int]$legacyMergeVitalsAfter.join.movement -eq
                    [int]$legacyMergeVitalsBefore.join.movement) {
                'held'
            } elseif ([int]$legacyMergeVitalsAfter.join.movement -eq 35) {
                'refreshed-35'
            } else {
                "changed-to-$([int]$legacyMergeVitalsAfter.join.movement)"
            }
        Write-Step ("merge barrier legacy MP verdict: host " +
            "$($legacyMergeVitalsBefore.host.movement)->$($legacyMergeVitalsAfter.host.movement); " +
            "join $($legacyMergeVitalsBefore.join.movement)->" +
            "$($legacyMergeVitalsAfter.join.movement) [$legacyJoinMovementObservation, observation]")
        $fiveSecondWindowComplete = $true

        # barrier_endturn.ps1 used three distinct host-hook reads here. Keep
        # them distinct: cascade count, suppress count, then post-merge rotate
        # count. They are separate source observations, not one reusable log
        # snapshot. Suppress is observation-only in the source legacy verdict;
        # the production expectation for it belongs after legacy PASS.
        $legacyCascadeAfterLines = @(Read-ClientLogLines $HostLog)
        $legacyCascadeAfter = @($legacyCascadeAfterLines | Where-Object {
            $_ -match $legacyCascadePattern
        }).Count
        $legacyCascadeNew = [int]($legacyCascadeAfter - $legacyCascadeBefore)

        $legacySuppressLines = @(Read-ClientLogLines $HostLog)
        $legacySuppressCount = @($legacySuppressLines | Where-Object {
            $_ -match '\[virtual-turn\] barrier suppress CONSUMED'
        }).Count

        $legacyRotationLines = @(Read-ClientLogLines $HostLog)
        $legacyRotationCount = @($legacyRotationLines | Where-Object {
            $_ -match '\[virtual-turn\] post-merge ROTATE'
        }).Count
    } else {
        $events = Wait-SimCondition -RelayProcess $RelayProcess `
            -Description 'complete two-phase causal merge relay chain' -Condition {
            param($current)
            $complete = $true
            foreach ($check in $mergeEventChecks) {
                if (-not (Test-SimEventDeltaReached `
                        $current $check.event $check.before 1 $check.role)) {
                    $complete = $false
                }
            }
            $complete
        }
        if ($isAutomaticMasstestPhaseC) {
            # This timestamp belongs to the one completed merge-event snapshot.
            # Return immediately: the literal +4 Phase-C anchor must precede
            # every +5/first-host probe, log/world read and MSS diagnostic.
            [DateTime]$handoffCompletedUtc = [DateTime]::UtcNow
            return [pscustomobject]@{
                automaticMasstestHandoff = $true
                order = $Order
                mergeDay = $MergeDay
                hostBefore = $hostBefore
                joinBefore = $joinBefore
                dispatchSkewMs = $fire.dispatchSkewMs
                handoffCompletedUtc = $handoffCompletedUtc
                slotWatermarkBeforePhaseC = $mergeTurnWatermark
                hostHandle = $expectedHostHandle
                joinHandle = $expectedJoinHandle
                neutralHandle = $automaticNeutralHandle
                fire = $fire
                eventsAtHandoff = @($events)
                naturalMergeMarker = $naturalMergeMarker
                mergeReleasedMarker = $mergeReleasedMarker
                baselines = [pscustomobject]@{
                    beforeEventCount = $before.Count
                    ordinaryApplyBefore = $ordinaryApplyBefore
                    ordinaryActivateBefore = $ordinaryActivateBefore
                    hostBarrierBefore = $hostBarrierBefore
                    joinBarrierBefore = $joinBarrierBefore
                    prepareDispatchBefore = $prepareDispatchBefore
                    hostPrepareAppliedBefore = $hostPrepareAppliedBefore
                    joinPrepareAppliedBefore = $joinPrepareAppliedBefore
                    executeDispatchBefore = $executeDispatchBefore
                    executeAppliedBefore = $executeAppliedBefore
                    hostMergeAppliedBefore = $hostMergeAppliedBefore
                    joinMergeAppliedBefore = $joinMergeAppliedBefore
                    mergeReleasedBefore = $mergeReleasedBefore
                    hostNaturalMergeBefore = $hostNaturalMergeBefore
                    joinNaturalMergeBefore = $joinNaturalMergeBefore
                    hostMergeReleasedBefore = $hostMergeReleasedBefore
                    joinMergeReleasedBefore = $joinMergeReleasedBefore
                }
            }
        }
        if ((Get-EngineActionCount $events 'ordinary-apply') -ne
                $ordinaryApplyBefore -or
            (Get-EngineActionCount $events 'ordinary-activate') -ne
                $ordinaryActivateBefore) {
            throw 'relay dispatched an ordinary turn action at the merge barrier'
        }
    }

    if ($isLegacyParallel) {
        # Continue barrier_endturn.ps1 literally after its three separate log
        # reads: one host UI observation, one stock-count baseline read, then
        # exactly one symbolic host End Turn intent and read-only +3 stock-count
        # samples. Owner resolution is part of that one action, not another read.
        $legacyHostUiState = Get-GameUiSnapshot host
        $legacyHostDialog = [string](Get-OptionalProperty $legacyHostUiState 'dialog')
        $legacyHostReadyValue = Get-OptionalProperty $legacyHostUiState 'dialogReady'
        $legacyHostReady = $legacyHostReadyValue -is [bool] -and
            [bool]$legacyHostReadyValue
        # This is the complete old over-rotation predicate. Empty or non-ready
        # observations other than DLG_ISO_PAL are recorded but are not this gate.
        $legacyOverRotated = ($legacyRotationCount -ge 1) -or
            ($legacyHostDialog -eq 'DLG_ISO_PAL')
        $legacyHostUi = ConvertTo-SavedDialogObservation host $legacyHostUiState
        $legacyPreClickTurnHistory = Get-TurnHistory -After 0
        $postMergeProbeBaseline.overrotationHostUi = $legacyHostUi
        $postMergeProbeBaseline.suppressCount = [int]$legacySuppressCount
        $firstHostIntent = New-CanonicalLegacyHostEndTurnIntent $legacyHostUiState
        $firstHostStep = Invoke-CanonicalLegacyPostMergeHostEndTurnProbe `
            -Action $firstHostIntent `
            -PreClickTurnHistory $legacyPreClickTurnHistory `
            -HostProcess $HostProcess `
            -JoinProcess $JoinProcess `
            -PreparedEvidence $postMergeProbeBaseline `
            -TimeoutSec 15

        $legacyBarrierPass = (
            [int]$legacyMergeObservation.hostCount -eq 1 -and
            [int]$legacyMergeObservation.joinCount -eq 1 -and
            $legacyHostRefreshed -and
            [int]$legacyCascadeNew -eq 0 -and
            [bool]$firstHostStep.rotated -and
            -not $legacyOverRotated -and
            [int]$firstHostStep.trackedProcessesAlive -eq 2
        )
        $legacyBarrierEvidence = [pscustomobject]@{
            legacyPass = [bool]$legacyBarrierPass
            mergeCountHost = [int]$legacyMergeObservation.hostCount
            mergeCountJoin = [int]$legacyMergeObservation.joinCount
            firstMergeObservationSec = $legacyMergeObservation.firstObservationSec
            hostRefreshed = [bool]$legacyHostRefreshed
            movementBefore = [pscustomobject]@{
                host = [int]$legacyMergeVitalsBefore.host.movement
                join = [int]$legacyMergeVitalsBefore.join.movement
            }
            movementAfter = [pscustomobject]@{
                host = [int]$legacyMergeVitalsAfter.host.movement
                join = [int]$legacyMergeVitalsAfter.join.movement
            }
            fixtureMovementBeforeMatched = [bool]$legacyFixtureMovementBeforeMatched
            joinMovementObservation = [string]$legacyJoinMovementObservation
            cascadesNew = [int]$legacyCascadeNew
            suppressCount = [int]$legacySuppressCount
            rotationCount = [int]$legacyRotationCount
            hostDialog = [string]$legacyHostDialog
            hostDialogReady = [bool]$legacyHostReady
            overRotated = [bool]$legacyOverRotated
            rotated = [bool]$firstHostStep.rotated
            deadClickConfirmed = if ([bool]$firstHostStep.rotated) { $false } else { $null }
            deadClickConfirmedKnown = [bool]$firstHostStep.rotated
            staleSuppressSuspect = [bool]$firstHostStep.staleSuppressSuspect
            trackedProcessesAlive = [int]$firstHostStep.trackedProcessesAlive
            pollScheduleSeconds = @(3, 6, 9, 12, 15)
            pollSampleSeconds = @($firstHostStep.pollSamples | ForEach-Object {
                [int]$_.elapsedSeconds
            })
            completedStockTurnCountBefore = [int]$firstHostStep.baselineCompletedStockTurnCount
            completedStockTurnCountAfter = [int]$firstHostStep.completedStockTurnCount
        }
        Write-Step ("legacy barrier verdict: merges=$($legacyBarrierEvidence.mergeCountHost)/" +
            "$($legacyBarrierEvidence.mergeCountJoin) hostMP35=$legacyHostRefreshed " +
            "cascades=$legacyCascadeNew rotated=$($firstHostStep.rotated) " +
            "overRotated=$legacyOverRotated processes=" +
            "$($firstHostStep.trackedProcessesAlive)/2")
        if (-not $legacyBarrierPass) {
            if (-not [bool]$firstHostStep.rotated) {
                throw ('BARRIER RESULT: FAIL-SUSPECT -- the sole post-merge host End Turn ' +
                    'did not rotate by +15s; no second action was issued')
            }
            throw 'BARRIER RESULT: FAIL -- canonical legacy barrier predicates did not all pass'
        }
        Write-Step ('BARRIER RESULT: PASS -- simultaneous barrier End Turn merged once, ' +
            'host owns the merge day, and the sole real host End Turn rotated')

        # Only after the unchanged legacy PASS boundary may MSS health, native
        # schema/owner, popup-chain and production relay evidence be checked.
        # barrier_endturn.ps1 treated the exact incoming MP pair as an
        # observation.  Keep the historical fixture comparison in the result,
        # but never promote it to a gate: the day-2 walk's source oracle is
        # moved+charged, and its actual pair is carried causally above.
        $firstHostStep = Assert-DeferredPostMergeProbeEvidence `
            $firstHostStep $MergeDay $expectedHostHandle $expectedJoinHandle `
            $HostProcess $JoinProcess $HostLog $JoinLog
        $noOverrotation = $firstHostStep.preClickEvidence
        $currentMergeOwner = $firstHostStep.currentOwner
        $events = @(Read-SimRelayEvents)
        $deferredRoundProof = Assert-DeferredCanonicalRoundEvidence `
            $events $PreparedCanonicalEvidence.roundEvidence
    }

    # Everything below is an MSS/native identity extension. Canonical mode has
    # already crossed the unmodified legacy BARRIER RESULT: PASS boundary.
    $canonicalConfirmationCount = $null
    if ($isLegacyParallel) {
        # No second wait is allowed after the legacy merge observer. Auxiliary
        # causal proofs must already agree with the exact release snapshot.
        Assert-ClientLogMarkerDelta $HostLog $naturalMergeMarker $hostNaturalMergeBefore
        Assert-ClientLogMarkerDelta $JoinLog $naturalMergeMarker $joinNaturalMergeBefore
        Assert-ClientLogMarkerDelta $HostLog $mergeReleasedMarker $hostMergeReleasedBefore
        Assert-ClientLogMarkerDelta $JoinLog $mergeReleasedMarker $joinMergeReleasedBefore
        Assert-ClientLogMarkerDelta $HostLog $hostLocalMergeMarker $hostLocalMergeBefore
        Assert-ClientLogMarkerDelta `
            $HostLog $preparedMergeMarker $hostPreparedMergeBefore
        Assert-ClientLogMarkerDelta `
            $JoinLog $preparedMergeMarker $joinPreparedMergeBefore
        $canonicalConfirmationCount = Assert-CanonicalNoEndTurnConfirmations $fire
    } else {
        if (-not (Wait-ClientLogMarkerDelta `
                $HostLog $naturalMergeMarker $hostNaturalMergeBefore)) {
            throw 'host never published its exact natural merge BeginTurn acknowledgement'
        }
        if (-not (Wait-ClientLogMarkerDelta `
                $JoinLog $naturalMergeMarker $joinNaturalMergeBefore)) {
            throw 'join never published its exact natural merge BeginTurn acknowledgement'
        }
        if (-not (Wait-ClientLogMarkerDelta `
                $HostLog $mergeReleasedMarker $hostMergeReleasedBefore)) {
            throw 'host never applied its exact relay-owned stock release'
        }
        if (-not (Wait-ClientLogMarkerDelta `
                $JoinLog $mergeReleasedMarker $joinMergeReleasedBefore)) {
            throw 'join never applied its exact relay-owned stock release'
        }
        if (-not (Wait-WorldDays $MergeDay $MergeDay)) {
            throw "clients did not converge to merge day $MergeDay"
        }
    }
    if ($Fixture) {
        # barrier_endturn.ps1 deliberately waited five seconds after merge so
        # the host's natural MergeDay BeginTurn could refresh MP. Host -> 35 is
        # a PASS gate; join MP remains recorded evidence, as in the old script.
        if (-not $fiveSecondWindowComplete) {
            Start-Sleep -Seconds 5
            $legacyMergeVitalsAfter = Get-LegacySequentialSharedMovementSnapshot $Fixture
            $fiveSecondWindowComplete = $true
        }
        if (-not $isLegacyParallel) {
            if ([int]$legacyMergeVitalsAfter.host.movement -ne 35) {
                throw ("merge barrier legacy host MP verdict failed: " +
                    "$($legacyMergeVitalsBefore.host.movement)->$($legacyMergeVitalsAfter.host.movement), expected ->35")
            }
            Write-Step ("merge barrier legacy MP verdict: host " +
                "$($legacyMergeVitalsBefore.host.movement)->35; join " +
                "$($legacyMergeVitalsBefore.join.movement)->$($legacyMergeVitalsAfter.join.movement) (observation)")
        }
    }
    $hostDayAfterMerge = Get-WorldDay host
    $joinDayAfterMerge = Get-WorldDay join
    if ($hostDayAfterMerge -ne $MergeDay -or $joinDayAfterMerge -ne $MergeDay) {
        throw "merge immediate day verdict is $hostDayAfterMerge/$joinDayAfterMerge, expected $MergeDay/$MergeDay"
    }
    if (-not $isLegacyParallel) {
        $noOverrotation = Assert-LegacyMergeNoOverrotation `
            $mergeTurnWatermark $expectedHostHandle
    }
    if ($null -eq $events) {
        $events = @(Read-SimRelayEvents)
    }
    Assert-NoRelayFault $events
    Assert-SimEventDelta $events 'end-turn-observed' $fire.beforeHostObserved 1 'host'
    Assert-SimEventDelta $events 'end-turn-observed' $fire.beforeJoinObserved 1 'join'
    Assert-SimEventDelta $events 'end-turn-applied' $fire.beforeHostApplied 1 'host'
    Assert-SimEventDelta $events 'end-turn-applied' $fire.beforeJoinApplied 1 'join'
    Assert-SimEventDelta $events 'end-turn-accepted' $fire.beforeHostAccepted 1 'host'
    Assert-SimEventDelta $events 'end-turn-accepted' $fire.beforeJoinAccepted 1 'join'
    Assert-EngineActionDelta `
        $events 'ordinary-apply' '' $null $ordinaryApplyBefore 0
    Assert-EngineActionDelta `
        $events 'ordinary-activate' '' $null $ordinaryActivateBefore 0
    Assert-SimEventDelta $events 'barrier-held' $hostBarrierBefore 1 'host'
    Assert-SimEventDelta $events 'barrier-held' $joinBarrierBefore 1 'join'
    Assert-SimEventDelta $events 'merge-prepare-dispatched' $prepareDispatchBefore 1
    Assert-SimEventDelta $events 'merge-prepare-applied' $hostPrepareAppliedBefore 1 'host'
    Assert-SimEventDelta $events 'merge-prepare-applied' $joinPrepareAppliedBefore 1 'join'
    Assert-SimEventDelta $events 'merge-execute-dispatched' $executeDispatchBefore 1
    Assert-SimEventDelta $events 'merge-execute-applied' $executeAppliedBefore 1
    Assert-SimEventDelta $events 'merge-applied' $hostMergeAppliedBefore 1 'host'
    Assert-SimEventDelta $events 'merge-applied' $joinMergeAppliedBefore 1 'join'
    Assert-SimEventDelta $events 'merge-released' $mergeReleasedBefore 1

    $hostMergeEndTurn = Assert-ExactEndTurnTransaction `
        $events host `
        $fire.beforeHostObserved $fire.beforeHostApplied $fire.beforeHostAccepted `
        ($MergeDay - 1)
    $joinMergeEndTurn = Assert-ExactEndTurnTransaction `
        $events join `
        $fire.beforeJoinObserved $fire.beforeJoinApplied $fire.beforeJoinAccepted `
        ($MergeDay - 1)
    $mergeProof = Assert-ExactMergeTransaction $events $MergeDay
    foreach ($roleProof in @(
        @{ role = 'host'; endTurn = $hostMergeEndTurn },
        @{ role = 'join'; endTurn = $joinMergeEndTurn }
    )) {
        $held = @($mergeProof.barriers | Where-Object {
            [string]$_.role -eq [string]$roleProof.role
        })
        if ($held.Count -ne 1 -or
            [int]$held[0].heldIndex -le [int]$roleProof.endTurn.acceptedIndex) {
            throw ("merge barrier-held/$($roleProof.role) did not strictly follow " +
                'its exact accepted End Turn transaction')
        }
    }
    $orderedBarriers = @($mergeProof.barriers | Sort-Object heldIndex)
    [string]$barrierRole = [string]$orderedBarriers[0].role
    $barrierRoles = @($mergeProof.barrierHeldPeers)
    if ($Order -ne 'parallel') {
        [string]$expectedBarrierRole = if ($Order -eq 'host-first') { 'host' } else { 'join' }
        if ($barrierRole -ne $expectedBarrierRole -or
            (Get-OptionalProperty $fire 'barrierRole') -ne $expectedBarrierRole) {
            throw ("ordered merge barrier role '$barrierRole' does not match " +
                "the requested first arrival '$expectedBarrierRole'")
        }
    }
    if ([int]$mergeProof.indices.prepareDispatched -lt $before.Count) {
        throw 'relay merge transaction did not occur after the stage baseline'
    }

    [long]$mergeActionId = $mergeProof.actionId
    $exactNaturalMergeMarker =
        "[simturns] natural merge BeginTurn applied and drained (actionId=$mergeActionId, day=$MergeDay)"
    $exactReleaseMarker =
        "[simturns] relay released stock turns (actionId=$mergeActionId, day=$MergeDay)"
    $exactPrepareMarker =
        "[simturns] prepared merge transaction $mergeActionId at stock day $MergeDay"
    $exactHostMergeMarker =
        "[simturns] host executed merge transaction $mergeActionId via 0x420FFA"
    Assert-ClientLogMarkerDelta $HostLog $exactNaturalMergeMarker 0
    Assert-ClientLogMarkerDelta $JoinLog $exactNaturalMergeMarker 0
    Assert-ClientLogMarkerDelta $HostLog $exactReleaseMarker 0
    Assert-ClientLogMarkerDelta $JoinLog $exactReleaseMarker 0
    Assert-ClientLogMarkerDelta $HostLog $exactPrepareMarker 0
    Assert-ClientLogMarkerDelta $JoinLog $exactPrepareMarker 0
    Assert-ClientLogMarkerDelta $HostLog $exactHostMergeMarker 0
    [int]$joinHostExecuteCount = Get-ClientLogMarkerCount `
        $JoinLog $exactHostMergeMarker
    if ($joinHostExecuteCount -ne 0) {
        throw ("join published $joinHostExecuteCount host ExecuteMerge marker(s); " +
            'only the authoritative host may execute the merge transaction')
    }
    Assert-ClientsLive $HostProcess $JoinProcess
    Assert-NoClientFaults $HostLog $JoinLog
    Write-Step ("relay-authoritative merge proof complete: exact prepare/execute/apply/release " +
        "transaction from both owned clients, no ordinary turn action, both at day $MergeDay")

    if (-not $isLegacyParallel) {
        # Non-canonical protocol orders retain their extended post-merge probe.
        # Canonical mode already performed this action before any MSS extension.
        $firstHostIntent = New-EndTurnAction host
        $firstHostStep = Invoke-LegacyPostMergeHostEndTurnProbe `
            $firstHostIntent ([long]$noOverrotation.watermark) $expectedJoinHandle $MergeDay `
            ([long]$noOverrotation.commandSequence) `
            $HostProcess $JoinProcess $HostLog $JoinLog 15
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        Write-Step ("extended $Order merge proof: first real host End Turn rotated once")
    }
    return [pscustomobject]@{
        order = $Order
        barrierRole = $barrierRole
        barrierRoles = @($barrierRoles)
        mergeDay = $MergeDay
        mergeActionId = $mergeActionId
        hostBefore = $hostBefore
        joinBefore = $joinBefore
        hostAfter = Get-WorldDay host
        joinAfter = Get-WorldDay join
        dispatchSkewMs = $fire.dispatchSkewMs
        turnWatermarkBeforeFire = $mergeTurnWatermark
        preClickTurnWatermark = [long]$noOverrotation.watermark
        firstHostAction = $firstHostStep
        legacyBarrierPass = [bool]$legacyBarrierPass
        legacyBarrier = $legacyBarrierEvidence
        firstMergeObservationSec = if ($legacyMergeObservation) {
            [int]$legacyMergeObservation.firstObservationSec
        } else { $null }
        confirmationCount = if ($null -ne $canonicalConfirmationCount) {
            [int]$canonicalConfirmationCount
        } else { $null }
        deferredRoundProof = $deferredRoundProof
        currentOwner = $currentMergeOwner
        movementBefore = if ($legacyMergeVitalsBefore) {
            [pscustomobject]@{
                host = [int]$legacyMergeVitalsBefore.host.movement
                join = [int]$legacyMergeVitalsBefore.join.movement
            }
        } else { $null }
        movementAfter = if ($legacyMergeVitalsAfter) {
            [pscustomobject]@{
                host = [int]$legacyMergeVitalsAfter.host.movement
                join = [int]$legacyMergeVitalsAfter.join.movement
            }
        } else { $null }
    }
}

function Run-PostMergeStockProof([System.Diagnostics.Process]$HostProcess,
                                  [System.Diagnostics.Process]$JoinProcess,
                                  [string]$HostLog,
                                  [string]$JoinLog,
                                  [object]$MergeResult) {
    # This is intentionally the MSS-native telemetry mode. Its Wait-ActionablePair
    # gates and telemetry-driven cycle shape are stronger implementation checks,
    # but they are not the old masstest Phase C and must never be labelled literal.
    if ($PostMergeContinuationMode -ne 'mss-stock-telemetry') {
        throw 'Run-PostMergeStockProof is reserved for mss-stock-telemetry mode'
    }
    if ($MergeDay -lt 2) { throw 'mss-stock-telemetry requires MergeDay >= 2' }
    if (-not $MergeResult -or -not $MergeResult.firstHostAction) {
        throw 'mss-stock-telemetry requires the completed one-shot legacy host probe'
    }
    [long]$turnWatermark = $MergeResult.firstHostAction.turnWatermark
    [long]$wireHighWater =
        [long]$MergeResult.firstHostAction.evidence.commandSequence
    $sessionPlan = Get-SessionPlanEvent @(Read-SimRelayEvents)
    [long]$hostHandle = Convert-ExactPlayerHandle `
        (Get-OptionalProperty $sessionPlan 'hostHandle') 'session host handle'
    [long]$joinHandle = Convert-ExactPlayerHandle `
        (Get-OptionalProperty $sessionPlan 'joinHandle') 'session join handle'
    if ($hostHandle -eq $joinHandle) {
        throw 'post-merge stock proof received identical negotiated player handles'
    }
    [long]$neutralHandle = Get-NeutralPlayerHandle
    if ($neutralHandle -in @($hostHandle, $joinHandle)) {
        throw 'continued stock order: neutral handle aliases a negotiated human handle'
    }

    $requiredCycles = 2
    $cycleResults = @()
    for ($cycle = 1; $cycle -le $requiredCycles; $cycle++) {
        $cycleDay = $MergeDay + $cycle - 1
        $hostStep = $null
        if ($cycle -eq 1) {
            # Cycle one starts with the already proved legacy first host action.
            # The extended suite continues from its immutable telemetry cursor.
            $hostStep = $MergeResult.firstHostAction
            Write-Step 'MSS stock telemetry cycle 1: continuing after the proved first-host action'
        } else {
            if (-not (Wait-ActionablePair)) {
                throw "stock cycle ${cycle}: host did not expose an actionable map at day $cycleDay"
            }
            Write-Step "stock cycle ${cycle}: invoking the subscribed host End Turn event once at day $cycleDay"
            $hostStep = Invoke-StockEndTurnAndObserve `
                -Role host `
                -After $turnWatermark `
                -ExpectedActiveHandle $joinHandle `
                -ExpectedDay $cycleDay `
                -PreviousCommandSequence $wireHighWater `
                -HostProcess $HostProcess `
                -JoinProcess $JoinProcess `
                -HostLog $HostLog `
                -JoinLog $JoinLog
        }
        $wireHighWater = [long]$hostStep.evidence.commandSequence
        if ([long]$hostStep.evidence.activeHandle -ne $joinHandle) {
            throw "stock cycle ${cycle}: host End Turn did not make the negotiated join player active"
        }
        $turnWatermark = [long]$hostStep.evidence.watermark
        Write-Step ("MSS stock telemetry cycle ${cycle}: exact host EndTX and host/join BeginApplied " +
            "selected join at day $cycleDay")

        # The applied BeginTurn pair is the ownership oracle. Only after it
        # selects join may the harness consume the new stock day UI and
        # subscribe to join's one End Turn action.
        if (-not (Wait-ActionablePair)) {
            throw "stock cycle ${cycle}: join did not expose an actionable map after its applied BeginTurn"
        }
        Write-Step "stock cycle ${cycle}: applied BeginTurn selected join; invoking join End Turn once"
        $cycleTail = Invoke-StockCycleTailAndObserve `
            -After $turnWatermark `
            -ExpectedNeutralHandle $neutralHandle `
            -ExpectedHostHandle $hostHandle `
            -ExpectedDay $cycleDay `
            -PreviousCommandSequence $wireHighWater `
            -HostProcess $HostProcess `
            -JoinProcess $JoinProcess `
            -HostLog $HostLog `
            -JoinLog $JoinLog
        if ([long]$cycleTail.evidence.neutral.activeHandle -ne $neutralHandle) {
            throw "stock cycle ${cycle}: join End Turn did not make the neutral player active"
        }
        if ([long]$cycleTail.evidence.returnedHost.activeHandle -ne $hostHandle -or
            [int]$cycleTail.evidence.returnedHost.day -ne ($cycleDay + 1)) {
            throw "stock cycle ${cycle}: automatic neutral turn did not return the next day to host"
        }
        $wireHighWater = [long]$cycleTail.evidence.returnedHost.commandSequence
        $turnWatermark = [long]$cycleTail.evidence.watermark
        Assert-ClientsLive $HostProcess $JoinProcess
        Assert-NoClientFaults $HostLog $JoinLog
        Write-Step ("MSS stock telemetry cycle ${cycle} PASS: host -> join -> neutral -> host/day $($cycleDay + 1) " +
            'was proved by applied BeginTurn telemetry')
        $cycleResults += [pscustomobject]@{
            cycle = $cycle
            day = $cycleDay
            hostAction = $hostStep
            joinAction = $cycleTail
            neutralApplied = $cycleTail.evidence.neutral
            hostReturned = $cycleTail.evidence.returnedHost
        }
    }
    return [pscustomobject]@{
        mergeDay = $MergeDay
        requiredCycles = $requiredCycles
        completedCycles = $cycleResults.Count
        cycles = $cycleResults
        neutralHandle = ('0x{0:x8}' -f $neutralHandle)
        finalWatermark = $turnWatermark
        finalCommandSequence = $wireHighWater
        returnedHostHandle = ('0x{0:x8}' -f $hostHandle)
        returnedDay = $MergeDay + $requiredCycles
    }
}

function Invoke-FailClosedProbe([System.Diagnostics.Process]$RelayProcess,
                                [System.Diagnostics.Process]$HostProcess,
                                [System.Diagnostics.Process]$JoinProcess,
                                [string]$HostLog,
                                [string]$JoinLog) {
    Assert-OperationalActionGate $HostLog $JoinLog
    if (-not (Wait-ActionablePair)) {
        throw 'fail-closed probe: clients not actionable before relay loss'
    }
    $hostBefore = Get-WorldDay host
    $joinBefore = Get-WorldDay join
    Write-Step 'fail-closed probe: stopping only the owned production relay'
    Stop-OwnedProcess $RelayProcess

    $deadline = (Get-Date).AddSeconds(20)
    $hostFault = $false
    $joinFault = $false
    while ((Get-Date) -lt $deadline) {
        $hostFault = (Get-ClientFaultLines $HostLog).Count -gt 0
        $joinFault = (Get-ClientFaultLines $JoinLog).Count -gt 0
        if ($hostFault -and $joinFault) { break }
        Start-Sleep -Milliseconds 400
    }
    if (-not ($hostFault -and $joinFault)) {
        throw "fail-closed probe: terminal relay-loss fault missing (host=$hostFault join=$joinFault)"
    }

    # The real End Turn button remains a deterministic action. In Faulted the
    # production overlay must own/suppress it; no production relay is available
    # to authorize a subjective cascade and stock progression must not resume.
    $hostAction = New-EndTurnAction host
    if (-not (Invoke-Button $hostAction.role $hostAction.dialog $hostAction.button `
                            $hostAction.instance $hostAction.appearance)) {
        throw "fail-closed probe: could not invoke DLG_STRATEGIC::BTN_END_TURN from '$(Get-Dialog host)'"
    }
    # Observe the only stock confirmation event that this action may cause. If
    # it appears, consume it once; otherwise the faulted overlay already owned
    # the request and there is nothing else to invoke.
    if (Wait-Dialog host DLG_MESSAGE_BOX 2) {
        $confirmation = Get-DialogObservation host
        [long]$confirmationInstance = Assert-ReadyButtonSnapshot `
            $confirmation 'BTN_YES' 'DLG_MESSAGE_BOX'
        if (-not (Invoke-Button host DLG_MESSAGE_BOX BTN_YES `
                                $confirmationInstance $confirmation.Instance)) {
            throw 'fail-closed probe: End Turn confirmation appeared but BTN_YES was not invoked'
        }
    }
    Start-Sleep -Seconds 5
    $hostAfter = Get-WorldDay host
    $joinAfter = Get-WorldDay join
    Assert-ClientsLive $HostProcess $JoinProcess
    if ($hostAfter -ne $hostBefore -or $joinAfter -ne $joinBefore) {
        throw "fail-closed probe: stock day advanced after relay loss ($hostBefore/$joinBefore -> $hostAfter/$joinAfter)"
    }
    Write-Step 'fail-closed probe PASS: both clients faulted and stock day stayed frozen'
    return [pscustomobject]@{
        hostDay = $hostAfter
        joinDay = $joinAfter
        hostFaultObserved = $hostFault
        joinFaultObserved = $joinFault
    }
}

function Copy-ClientLog([string]$Path) {
    if (-not $Path) { return $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "client log does not exist: '$Path'"
    }
    $destination = [IO.Path]::GetFullPath(
        (Join-Path $ArtifactDir ([IO.Path]::GetFileName($Path))))
    Copy-Item -LiteralPath $Path -Destination $destination -Force
    return $destination
}

function Get-LiveClientLogTailSnapshot {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory)][string]$Path
    )
    $fullPath = [IO.Path]::GetFullPath($Path)
    [long]$ownedPid = $Process.Id
    if (-not $script:ClientLogOwnedProcessIds.ContainsKey($fullPath) -or
        [long]$script:ClientLogOwnedProcessIds[$fullPath] -ne $ownedPid) {
        throw "$Role live client log '$fullPath' lost its exact owned-process binding"
    }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Role live client log does not exist: '$fullPath'"
    }

    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $stream = [IO.File]::Open(
        $fullPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
    try {
        [long]$length = $stream.Length
        [int]$lastByte = -1
        if ($length -gt 0) {
            [void]$stream.Seek(-1, [IO.SeekOrigin]::End)
            $lastByte = $stream.ReadByte()
        }
    } finally {
        $stream.Dispose()
    }
    $Process.Refresh()
    return [pscustomobject][ordered]@{
        role = $Role
        pid = $ownedPid
        length = $length
        endedWithLineFeed = ($lastByte -eq 0x0A)
        processExited = [bool]$Process.HasExited
    }
}

function Wait-ClientLogsAtCompleteQuietBoundary {
    param(
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [Parameter(Mandatory)][string]$HostLog,
        [Parameter(Mandatory)][string]$JoinLog,
        [ValidateRange(1000, 30000)][int]$TimeoutMilliseconds = 6000,
        [ValidateRange(250, 5000)][int]$QuietMilliseconds = 750,
        [ValidateRange(25, 250)][int]$PollMilliseconds = 50
    )
    if ($QuietMilliseconds -ge $TimeoutMilliseconds) {
        throw 'client-log quiet interval must be shorter than its timeout'
    }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $lastSignature = ''
    [long]$quietStartedAt = -1
    [int]$samples = 0
    $hostSnapshot = $null
    $joinSnapshot = $null
    while ($clock.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        $hostSnapshot = Get-LiveClientLogTailSnapshot host $HostProcess $HostLog
        $joinSnapshot = Get-LiveClientLogTailSnapshot join $JoinProcess $JoinLog
        $samples++
        $signature = "$([long]$hostSnapshot.length):$([long]$joinSnapshot.length)"
        $complete = [long]$hostSnapshot.length -gt 0 -and
            [long]$joinSnapshot.length -gt 0 -and
            [bool]$hostSnapshot.endedWithLineFeed -and
            [bool]$joinSnapshot.endedWithLineFeed
        if ($complete) {
            if ($signature -ne $lastSignature) {
                $lastSignature = $signature
                $quietStartedAt = $clock.ElapsedMilliseconds
            } elseif ($quietStartedAt -ge 0 -and
                      ($clock.ElapsedMilliseconds - $quietStartedAt) -ge
                        $QuietMilliseconds) {
                return [pscustomobject][ordered]@{
                    passed = $true
                    timeoutMilliseconds = $TimeoutMilliseconds
                    quietMilliseconds = $QuietMilliseconds
                    pollMilliseconds = $PollMilliseconds
                    samples = $samples
                    waitedMilliseconds = $clock.ElapsedMilliseconds
                    host = $hostSnapshot
                    join = $joinSnapshot
                }
            }
        } else {
            $lastSignature = ''
            $quietStartedAt = -1
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    }
    $hostState = if ($hostSnapshot) {
        "length=$($hostSnapshot.length),lf=$($hostSnapshot.endedWithLineFeed)"
    } else { 'unobserved' }
    $joinState = if ($joinSnapshot) {
        "length=$($joinSnapshot.length),lf=$($joinSnapshot.endedWithLineFeed)"
    } else { 'unobserved' }
    throw ("client logs did not reach a complete ${QuietMilliseconds}ms quiet " +
        "boundary within ${TimeoutMilliseconds}ms (host $hostState; join $joinState)")
}

function Stop-OwnedClientPair {
    param(
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [ValidateRange(1000, 120000)][int]$TimeoutMilliseconds = 30000
    )
    $entries = @(
        [pscustomobject]@{ role = 'host'; process = $HostProcess },
        [pscustomobject]@{ role = 'join'; process = $JoinProcess }
    )
    $errors = [System.Collections.Generic.List[string]]::new()
    $killIssued = [ordered]@{ host = $false; join = $false }
    foreach ($entry in $entries) {
        $ownedPid = $entry.process.Id
        try {
            $entry.process.Refresh()
            if (-not $entry.process.HasExited) {
                $entry.process.Kill()
                $killIssued[$entry.role] = $true
            }
        } catch {
            $entry.process.Refresh()
            if (-not $entry.process.HasExited) {
                $errors.Add(
                    "$($entry.role) owned pid=$ownedPid kill failed: $($_.Exception.Message)")
            }
        }
    }
    foreach ($entry in $entries) {
        $ownedPid = $entry.process.Id
        try {
            $entry.process.Refresh()
            if (-not $entry.process.HasExited -and
                -not $entry.process.WaitForExit($TimeoutMilliseconds)) {
                $errors.Add(
                    "$($entry.role) owned pid=$ownedPid did not exit within ${TimeoutMilliseconds}ms")
            }
            $entry.process.Refresh()
            if (-not $entry.process.HasExited) {
                $errors.Add("$($entry.role) owned pid=$ownedPid remained live after pair teardown")
            }
        } catch {
            $errors.Add(
                "$($entry.role) owned pid=$ownedPid exit observation failed: $($_.Exception.Message)")
        }
    }
    if ($errors.Count -ne 0) {
        throw ($errors -join '; ')
    }
    return [pscustomobject][ordered]@{
        hostPid = [long]$HostProcess.Id
        joinPid = [long]$JoinProcess.Id
        hostKillIssued = [bool]$killIssued.host
        joinKillIssued = [bool]$killIssued.join
        bothExited = $true
    }
}

function Copy-StoppedClientLogWithCompletionProof {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory)][string]$Path
    )
    $Process.Refresh()
    if (-not $Process.HasExited) {
        throw "$Role client log cannot be finalized while owned pid=$($Process.Id) is live"
    }

    $fullPath = [IO.Path]::GetFullPath($Path)
    [long]$ownedPid = $Process.Id
    $expectedFileName = "mss32_${ownedPid}.log"
    if ([IO.Path]::GetFileName($fullPath) -ne $expectedFileName) {
        throw "$Role client log '$fullPath' is not bound to owned pid=$ownedPid"
    }
    if (-not $script:ClientLogOwnedProcessIds.ContainsKey($fullPath) -or
        [long]$script:ClientLogOwnedProcessIds[$fullPath] -ne $ownedPid) {
        throw "$Role client log '$fullPath' lost its exact owned-process binding"
    }
    [long]$initialLength = Get-ClientLogBaseline $fullPath
    if ($initialLength -ne 0) {
        throw "$Role client log had a nonzero launch boundary ($initialLength bytes)"
    }

    # A just-exited native logger (or a short-lived scanner it triggered) may
    # still have a compatible write/delete handle open.  ReadAllBytes opens
    # with FileShare.Read and would reject that harmless existing handle even
    # though the exact owned process is already proved exited.  Capture one
    # immutable byte snapshot with the same permissive sharing used by the
    # live-tail observer, and fail if its length changes during that capture.
    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $sourceStream = [IO.File]::Open(
        $fullPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
    try {
        [long]$sourceLengthAtOpen = $sourceStream.Length
        $memory = [IO.MemoryStream]::new()
        try {
            $sourceStream.CopyTo($memory)
            [byte[]]$sourceBytes = $memory.ToArray()
        } finally {
            $memory.Dispose()
        }
        if ($sourceStream.Length -ne $sourceLengthAtOpen -or
            $sourceBytes.Length -ne $sourceLengthAtOpen) {
            throw "$Role stopped client log changed while its exact bytes were captured"
        }
    } finally {
        $sourceStream.Dispose()
    }

    # Preserve that exact stopped-source snapshot even when the byte-level
    # checks below reject it; a failed run still leaves the broken diagnostic.
    $artifactPath = [IO.Path]::GetFullPath(
        (Join-Path $ArtifactDir ([IO.Path]::GetFileName($fullPath))))
    [IO.File]::WriteAllBytes($artifactPath, $sourceBytes)
    $artifactBytes = [IO.File]::ReadAllBytes($artifactPath)
    if ($sourceBytes.Length -le $initialLength) {
        throw "$Role client log contains no records after its launch boundary"
    }
    if ($sourceBytes[$sourceBytes.Length - 1] -ne 0x0A) {
        throw "$Role client log ended without LF after exact-owned process exit"
    }
    if ($artifactBytes.Length -ne $sourceBytes.Length -or
        $artifactBytes.Length -eq 0 -or
        $artifactBytes[$artifactBytes.Length - 1] -ne 0x0A) {
        throw "$Role copied client log is empty, incomplete, or byte-length mismatched"
    }

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $sourceHash = [Convert]::ToHexString($sha256.ComputeHash($sourceBytes))
    } finally {
        $sha256.Dispose()
    }
    $artifactHash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash
    if ($artifactHash -ne $sourceHash) {
        throw "$Role copied client log SHA-256 does not match its stopped source"
    }

    $rotatedPattern = '^' + [regex]::Escape(
        [IO.Path]::GetFileNameWithoutExtension($fullPath)) + '\.[1-3]\.log$'
    $rotatedSegments = @(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($fullPath)) `
        -File | Where-Object { $_.Name -match $rotatedPattern })
    if ($rotatedSegments.Count -ne 0) {
        throw ("$Role client log rotated into $($rotatedSegments.Count) segment(s); " +
            'a single-file complete-log proof is invalid')
    }

    return [pscustomobject][ordered]@{
        role = $Role
        pid = $ownedPid
        fileName = $expectedFileName
        initialLength = $initialLength
        length = [long]$sourceBytes.Length
        sha256 = $sourceHash
        endedWithLineFeed = $true
        copied = $true
        sourceAndArtifactMatch = $true
        rotatedSegments = 0
        artifactPath = $artifactPath
    }
}

function Show-FailureTail([string]$Path, [string]$Prefix) {
    Read-ClientLogLines $Path | Select-Object -Last 40 |
        ForEach-Object { Write-Host "[$Prefix] $_" }
}

function Wait-DevouringReinforcementProof(
    [object]$Fixture,
    [System.Diagnostics.Process]$HostProcess,
    [System.Diagnostics.Process]$JoinProcess,
    [string]$HostLog,
    [string]$JoinLog,
    [int]$TimeoutSec = 30,
    [switch]$SnapshotOnly
) {
    $marker = '[testdrv][fixture-reinforcement] COMMITTED unit-transfer plan:'
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $lastWorldMismatch = ''
    while ((Get-Date) -lt $deadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        $hostCount = Get-ClientLogMarkerCount $HostLog $marker
        $joinCount = Get-ClientLogMarkerCount $JoinLog $marker
        if ($hostCount -gt 1) {
            throw "reinforcement committed more than once on host (count=$hostCount)"
        }
        if ($joinCount -ne 0) {
            throw "reinforcement ran outside the authoritative host (join count=$joinCount)"
        }
        if ($hostCount -eq 1) {
            if ($SnapshotOnly) {
                # At quiet-3 this assertion is marker-only. The immediately
                # following single shared host census proves both exact groups
                # and resolves their dynamic hero IDs without a second world.
                return [pscustomobject]@{
                    hostCommits = $hostCount
                    joinCommits = $joinCount
                    hostUnits = [int]$Fixture.host.reinforcement.units
                    joinUnits = [int]$Fixture.join.reinforcement.units
                    replicatedTo = @('host-authoritative marker; census pending')
                }
            }
            try {
                # This bounded loop observes asynchronous object-change delivery;
                # it never emits, retries or substitutes a gameplay action.
                Assert-CanonicalWorldLoaded $Fixture
                Write-Step 'legacy reinforcement PASS: exact three Demiurges per hero replicated to both clients before deploy'
                return [pscustomobject]@{
                    hostCommits = $hostCount
                    joinCommits = $joinCount
                    hostUnits = [int]$Fixture.host.reinforcement.units
                    joinUnits = [int]$Fixture.join.reinforcement.units
                    replicatedTo = @('host', 'join')
                }
            } catch {
                $lastWorldMismatch = $_.Exception.Message
            }
        }
        if ($SnapshotOnly) { break }
        Start-Sleep -Milliseconds 100
    }
    $hostCount = Get-ClientLogMarkerCount $HostLog $marker
    $joinCount = Get-ClientLogMarkerCount $JoinLog $marker
    $deadlineDescription = if ($SnapshotOnly) {
        'at the legacy quiet-3 checkpoint'
    } else {
        'before deploy'
    }
    throw ("exact Devouring reinforcement was not proved $deadlineDescription " +
        "(host commits=$hostCount join commits=$joinCount; world='$lastWorldMismatch')")
}

$testRelay = $null
$simRelay = $null
$hostProcess = $null
$joinProcess = $null
$hostLog = $null
$joinLog = $null
$passed = $false
$failure = ''
$roundResults = @()
$mergeResult = $null
$postMergeStockResult = $null
$postMergeAutomaticMasstestPhaseCResult = $null
$faultProbeResult = $null
$gameplayResult = $null
$day2WalkResult = $null
$battleBlockResult = $null
$longMoveResult = $null
$longAttackResult = $null
$orderedMasstestResult = $null
$canonicalDeferredGameplayEvidenceResult = $null
$reinforcementResult = $null
$canonicalDeployComplete = $false
$canonicalRoundPreparedEvidence = $null
$literalReadyObserver = $null
$literalNestedStartupSubstitution = $null
$literalInnerStartupResult = $null
$startupActionsBarrierResult = $null
$literalInnerDumpBaseline = $null
$orderedMasstestDumpBaseline = $null
$preexistingClientLogArchive = $null
$clientLogCompletion = $null
$clientLogQuiescence = $null
$clientPairTeardown = $null

try {
    Assert-ExclusiveTestMachine -GameDir $GameDir

    # Preserve every prior log. Only logs whose embedded PID has no live owner
    # are moved, before any source stopwatch edge, into this run's reversible
    # artifact archive. Logs belonging to ambient processes are merely listed
    # and ignored; neither of our future distinct PIDs can reuse a live PID.
    $preexistingClientLogArchive = Move-StaleMssClientLogsToArtifact `
        -SourceGameDir $GameDir -RunArtifactDir $ArtifactDir
    # Dump hashing is likewise a pre-arm safety baseline. It must not consume
    # any part of the old inner observer's first fixed +500 ms interval.
    $literalInnerDumpBaseline = Get-LiteralInnerDumpBaseline `
        -DumpRoots @($GameDir, $ArtifactDir)

    if ($GameplayMode -eq 'ordered-masstest') {
        # test_R_masstest Hard-Cleanup always ended with its own fixed +2. The
        # exact-owned machine admission above replaces its destructive kill
        # loop; no run_test cleanup or outer Is-Ready worker existed here.
        Start-Sleep -Seconds 2
    } else {
        # run_test.ps1 -> Kill-All -> Stop-LobbyProcesses ended with this first
        # unconditional +1200. Its async child wrapper was launched immediately
        # afterward, so arm the independent outer Is-Ready sampler at this edge.
        Start-Sleep -Milliseconds 1200
        $literalReadyObserver = Start-LiteralOuterReadyObserver `
            -ArmUtc ([DateTime]::UtcNow) -Fixture $fixture -MaxSamples 120
    }

    # test_R_mod.ps1 projected cleanup: Stop-LobbyProcesses' own +1200, then
    # the wrapper's separate +500 before handing off its pinned configuration.
    Start-Sleep -Milliseconds 1200
    Start-Sleep -Milliseconds 500
    $literalModWrapperHandoff = [pscustomobject]@{
        gameDir = $GameDir
        scenarioIndex = $scenarioSelectionIndex
        mergeDay = $MergeDay
    }

    # test_R_virtual_turn.ps1 projected cleanup: another helper +1200 and its
    # own +500 after job cleanup, before env/log setup and nested start.ps1.
    Start-Sleep -Milliseconds 1200
    Start-Sleep -Milliseconds 500
    $literalInnerArmUtc = [DateTime]::UtcNow
    $script:LiteralInnerStartupObserver = Start-LiteralInnerStartupObserver `
        -ArmUtc $literalInnerArmUtc `
        -RelayBase $script:RelayBase `
        -SimRelayLog $script:SimRelayLog `
        -BootstrapReleaseFile $script:LiteralInnerBootstrapReleaseFile `
        -FailureLogPath $script:LiteralInnerFailureLog `
        -ArtifactDir $ArtifactDir `
        -GameDir $GameDir `
        -ExpectedModulePath $deployedMss `
        -DumpRoots @($GameDir, $ArtifactDir) `
        -DumpBaseline $literalInnerDumpBaseline `
        -TotalBudgetSec 120
    if ($literalReadyObserver) {
        Publish-LiteralOuterReadyStartupObserver `
            $literalReadyObserver $script:LiteralInnerStartupObserver
    }
    $literalNestedStartupSubstitution = Get-LiteralNestedStartupSubstitutionMap
    $literalVirtualWrapperHandoff = [pscustomobject]@{
        productionPipe = $PipeName
        debugPipe = $debugPipeName
        artifactDir = $ArtifactDir
        substitution = $literalNestedStartupSubstitution
    }

    # start.ps1 projected cleanup: its fourth Stop-LobbyProcesses +1200 and
    # distinct +300 before the UDP/lobby/PacketLogic startup section. Cleanup
    # itself is intentionally not repeated: exact machine ownership above is
    # the safe in-scope substitute, while all four clocks remain separate.
    Start-Sleep -Milliseconds 1200
    Start-Sleep -Milliseconds 300
    $literalStartWrapperHandoff = [pscustomobject]@{
        outer = $literalModWrapperHandoff
        inner = $literalVirtualWrapperHandoff
    }

    $testRelay = Start-TestRelay -LogDir $ArtifactDir
    # Hard-Cleanup/run_test cleanup made the old node lobby cold, so start.ps1
    # always retained its distinct +1 second node-server settle before starting
    # PacketLogic. The DebugTest relay is that reporter/control replacement.
    Start-Sleep -Seconds 1
    $simRelay = Start-ProductionSimRelay
    Write-Step "relays ready; production pipe=$PipeName; DebugTest pipe=$debugPipeName"
    # start.ps1 gave the relay one literal +1500 ms head start before launching
    # the host. Keep that tuned interval even though both new relays publish a
    # readiness proof; readiness is not a substitute for the old fixed pause.
    Start-Sleep -Milliseconds 1500

    $hostProcess = Start-SimturnGameClient host
    $hostLaunchUtc = [DateTime]::UtcNow
    $hostLog = Join-Path $GameDir "mss32_$($hostProcess.Id).log"
    Publish-LiteralInnerStartupProcess `
        $script:LiteralInnerStartupObserver host $hostProcess `
        ([string]$hostProcess.D2MssLaunchExecutablePath) $hostLog 0
    if ($literalReadyObserver) {
        Publish-LiteralOuterReadyProcess $literalReadyObserver host $hostProcess
    }
    Write-Step "host launched pid=$($hostProcess.Id) logBaseline=$(Get-ClientLogBaseline $hostLog)"
    # The green start.ps1 established host+10000 ms as the safest minimum after
    # shorter gaps collided during shared startup. Retain that exact not-before
    # edge. A cold r22b boot proved the host may still be inside shared DB init
    # at +10, so one passive exact UI subscription now proves that the owned
    # host has published ready DLG_MAIN_MENU::BTN_MULTI before the sole join
    # launch. This emits no command and cannot restart either client.
    Wait-FixedUtcAnchor ($hostLaunchUtc.AddMilliseconds(10000))
    $hostProcess.Refresh()
    if ($hostProcess.HasExited) {
        throw "host exited before the pre-join main-menu readiness gate"
    }
    $hostJoinLaunchDeadlineUtc = $hostLaunchUtc.AddSeconds($BootTimeoutSec)
    [int]$hostJoinLaunchWaitMilliseconds = [Math]::Floor(
        ($hostJoinLaunchDeadlineUtc - [DateTime]::UtcNow).TotalMilliseconds)
    if ($hostJoinLaunchWaitMilliseconds -lt 1) {
        throw 'host exhausted its fixed boot deadline before join launch readiness'
    }
    $hostPreJoinMainMenu = Wait-UiButtonReadyPublication `
        -Role host `
        -Dialog DLG_MAIN_MENU `
        -Button BTN_MULTI `
        -AfterUiSequence 0 `
        -WaitMilliseconds $hostJoinLaunchWaitMilliseconds `
        -ExpectedProcessId $hostProcess.Id `
        -ExpectedModulePath (Join-Path $GameDir 'mss32.dll')
    $hostProcess.Refresh()
    if ($hostProcess.HasExited) {
        throw 'host exited after its pre-join main-menu readiness publication'
    }
    Write-Step ("host pre-join ready: pid={0} uiSeq={1} appearance={2}" -f `
        $hostProcess.Id, $hostPreJoinMainMenu.uiSeq,
        $hostPreJoinMainMenu.dialogAppearance)
    $joinProcess = Start-SimturnGameClient join
    $joinLaunchUtc = [DateTime]::UtcNow
    $joinLog = Join-Path $GameDir "mss32_$($joinProcess.Id).log"
    Publish-LiteralInnerStartupProcess `
        $script:LiteralInnerStartupObserver join $joinProcess `
        ([string]$joinProcess.D2MssLaunchExecutablePath) $joinLog 0
    if ($literalReadyObserver) {
        Publish-LiteralOuterReadyProcess $literalReadyObserver join $joinProcess
    }
    Write-Step "join launched pid=$($joinProcess.Id) logBaseline=$(Get-ClientLogBaseline $joinLog)"
    # start.ps1 retained a separate join+500 ms process-count diagnostic.
    # Preserve that clock but scope the observation to the two owned PIDs.
    Wait-FixedUtcAnchor ($joinLaunchUtc.AddMilliseconds(500))
    # The host-only pre-join callback above protects shared initialization.
    # Keep the join lane free of any typed UI demand before its own preserved
    # delayed-injection+11000 ms navigation anchor.
    Assert-OwnedProcessesLive $hostProcess $joinProcess
    $pairingResult = Run-Pairing `
        -HostLog $hostLog `
        -JoinLog $joinLog `
        -ExactScenarioPath $ScenarioPath `
        -ExactScenarioIndex $scenarioSelectionIndex `
        -HostProcess $hostProcess `
        -JoinProcess $joinProcess `
        -HostLaunchUtc $hostLaunchUtc `
        -JoinLaunchUtc $joinLaunchUtc `
        -StartupObserver $script:LiteralInnerStartupObserver `
        -ReadyObserver $literalReadyObserver
    if ($GameplayMode -eq 'ordered-masstest') {
        # test_R_mod invoked test_R_virtual_turn synchronously. Its KeepAlive
        # return is therefore a hard boundary before the caller's own +2 and
        # Get-PeerPids read; run_test's quiet observer never participates.
        $literalInnerStartupResult = Complete-LiteralInnerStartupObserver `
            $script:LiteralInnerStartupObserver
        $script:LiteralInnerStartupObserver = $null
        Start-Sleep -Seconds 2
        $orderedInitialCheckpoint = Read-LiteralMasstestPeerStateCheckpoint `
            $hostProcess $joinProcess @{ host = 1; join = 1 } @('host', 'join') `
            'test_R_masstest Get-PeerPids'
        # The old caller removed stale d2_uef dumps after this one PID-state
        # observation and before its first ordered End Turn. Preserve every
        # file, but establish the equivalent per-iteration enumeration boundary.
        $orderedMasstestDumpBaseline = Get-LiteralInnerDumpBaseline `
            -DumpRoots @($GameDir, $ArtifactDir)
        $orderedMergeHandoff = Run-LiteralOrderedMasstestToMerge `
            $hostProcess $joinProcess $BarrierOrder $fixture $orderedInitialCheckpoint
        # Run-AutomaticMasstestPhaseCLiteral owns the distinct source +4 after
        # Drive-ToMerge's final read, then the exact Phase-C observation/action
        # topology. It returns the old final postDays read without pre-empting
        # the source's following process/log/dump verdict.
        $literalPhaseC = Run-AutomaticMasstestPhaseCLiteral `
            $orderedMergeHandoff $hostProcess
        $legacyPhaseCOracle = Complete-LiteralOrderedMasstestLegacyOracle `
            $orderedMergeHandoff $literalPhaseC $hostProcess $joinProcess `
            $hostLog $orderedMasstestDumpBaseline
        $deferredPhaseCMss = $null
        $postMergeAutomaticMasstestPhaseCResult = [pscustomobject]@{
            legacyVerdict = [string]$legacyPhaseCOracle.legacyVerdict
            requiredNeutralRounds = [int]$literalPhaseC.requiredNeutralRounds
            completedNeutralRounds = [int]$literalPhaseC.completedNeutralRounds
            outerTurns = [int]$literalPhaseC.outerTurns
            maxOuterTurns = [int]$literalPhaseC.maxOuterTurns
            pollMilliseconds = [int]$literalPhaseC.pollMilliseconds
            advanceTimeoutSeconds = [int]$literalPhaseC.advanceTimeoutSeconds
            initialSettleSeconds = [int]$literalPhaseC.initialSettleSeconds
            noSlotSleepSeconds = [int]$literalPhaseC.noSlotSleepSeconds
            slotWatermark = [long]$literalPhaseC.slotWatermark
            postMergeSlotBoundary = [long]$literalPhaseC.postMergeSlotBoundary
            attemptedHumanActions = [int]$literalPhaseC.attemptedHumanActions
            acceptedHumanActions = [int]$literalPhaseC.acceptedHumanActions
            recoveryActions = [int]$literalPhaseC.recoveryActions
            transportAdapterReads = [int]$literalPhaseC.transportAdapterReads
            humanActions = @($literalPhaseC.humanActions)
            slotSampleCount = @($literalPhaseC.slotSamples).Count
            humanSlotStalled = [bool]$literalPhaseC.humanSlotStalled
            stalledHumanRole = [string]$literalPhaseC.stalledHumanRole
            stalledOuterTurn = [int]$literalPhaseC.stalledOuterTurn
            hostAlive = [bool]$legacyPhaseCOracle.hostAlive
            joinerAlive = [bool]$legacyPhaseCOracle.joinerAlive
            mergeFired = [bool]$legacyPhaseCOracle.mergeFired
            uiMarshalled = [bool]$legacyPhaseCOracle.uiMarshalled
            postDays = [int]$legacyPhaseCOracle.postDays
            postTurns = [int]$legacyPhaseCOracle.postTurns
            vehAfterMerge = [int]$legacyPhaseCOracle.vehAfterMerge
            suspect = [int]$legacyPhaseCOracle.suspect
            uefAfterMerge = [int]$legacyPhaseCOracle.uefAfterMerge
            uef = [int]$legacyPhaseCOracle.uef
            dumps = [int]$legacyPhaseCOracle.dumps
            dumpFiles = @($legacyPhaseCOracle.dumpFiles)
            deferredMssEvidence = $deferredPhaseCMss
        }
        $orderedMasstestResult = [pscustomobject]@{
            legacyVerdict = [string]$legacyPhaseCOracle.legacyVerdict
            order = $BarrierOrder
            mergeDay = 3
            firstRole = [string]$orderedMergeHandoff.firstRole
            laggardRole = [string]$orderedMergeHandoff.laggardRole
            stepSleepSeconds = 4
            postMergeSettleSeconds = 4
            fixedMergeClassification = [string]$orderedMergeHandoff.fixedMergeClassification
            fixedMergeObserved = [bool]$orderedMergeHandoff.sourceMergeObserved
            completionMergeObserved = [bool]$orderedMergeHandoff.completionMergeObserved
            mergeCompletionTimeoutSeconds = [int]$orderedMergeHandoff.mergeCompletionTimeoutSeconds
            actionTrace = @($orderedMergeHandoff.actionTrace)
            attemptedActions = [int]$orderedMergeHandoff.attemptedActions
            acceptedActions = [int]$orderedMergeHandoff.acceptedActions
            recoveryActions = [int]$orderedMergeHandoff.recoveryActions
            phaseC = $postMergeAutomaticMasstestPhaseCResult
            deferredMssEvidence = $deferredPhaseCMss
        }
        if ([string]$legacyPhaseCOracle.legacyVerdict -ne 'PASS') {
            throw ("literal test_R_masstest legacy verdict is " +
                [string]$legacyPhaseCOracle.legacyVerdict)
        }
        # The old PASS boundary is now complete. Only after it may the MSS
        # client/relay/causal strengthening perform its own observations.
        $deferredPhaseCMss = Complete-LiteralOrderedMasstestDeferredProof `
            $orderedMergeHandoff $literalPhaseC `
            $hostProcess $joinProcess $hostLog $joinLog
        $postMergeAutomaticMasstestPhaseCResult.deferredMssEvidence =
            $deferredPhaseCMss
        $orderedMasstestResult.deferredMssEvidence = $deferredPhaseCMss
        Write-Step ("literal test_R_masstest PASS: order=$BarrierOrder; " +
            'four distinct-day actions; Phase C and its live legacy oracle completed')
    } else {
    Assert-LiteralInnerStartupObserverHealthy `
        $script:LiteralInnerStartupObserver
    Assert-ClientsLive $hostProcess $joinProcess
    Write-Step 'DebugTest relay identities matched both owned PIDs at the literal launch anchors'
    $legacyQuietWitness = Wait-LiteralDayReady -QuietSec 3 -TimeoutSec 60 `
        -PopupTimeline $pairingResult.PopupTimeline `
        -PairingResult $pairingResult
    if (-not $legacyQuietWitness) {
        throw 'legacy wait-day-ready did not observe three quiet seconds on both roles'
    }
    Write-Step 'legacy startup PASS: both role logs reached the exact quiet-3 checkpoint'

    $heroCensus = $null
    if ($GameplayMode -in @('canonical', 'battle-block', 'long-move', 'long-attack')) {
        # Literal run_test.ps1: the first observation after quiet-3 is exactly
        # one relay-global stacks hero census. Preserve it before any MSS
        # event or typed UI-binding read.
        $heroCensus = Resolve-LiteralHostAuthoritativeHeroes $fixture
        $fixture.host.heroId = [string]$heroCensus.hostId
        $fixture.join.heroId = [string]$heroCensus.joinId
    }

    # Before the literal deploy, retain only the transport adapter state needed
    # to bind each one-shot garrison action to its exact appearance/owner. All
    # other MSS/startup/reinforcement checks consume saved evidence after deploy.
    $terminalRoleStates = $legacyQuietWitness.TerminalRoleStates
    $terminalHostObservation = ConvertTo-SavedDialogObservation `
        host $terminalRoleStates.host
    $terminalJoinObservation = ConvertTo-SavedDialogObservation `
        join $terminalRoleStates.join
    $preparedDeployBindings = New-CanonicalWalkPreparation `
        -HostObservation $terminalHostObservation `
        -JoinObservation $terminalJoinObservation
    if ($GameplayMode -in @('canonical', 'long-move', 'long-attack')) {
        # Literal run_test.ps1: quiet-3 -> one relay-global stacks census ->
        # host deploy -> +2 -> join deploy -> +3. This one host-authoritative
        # census supplies only the two hero IDs here; its richer fixture/MSS
        # evidence is deliberately classified after the source deploy boundary.
        Invoke-CanonicalDeploy `
            $fixture $hostProcess $joinProcess $preparedDeployBindings
        if ($GameplayMode -eq 'canonical') {
            Add-LegacyMassPhase 'deploy'
        }
        $canonicalDeployComplete = $true
    }

    # The deploy boundary has completed for canonical gameplay. Capture and
    # classify the remaining startup/MSS evidence now, without issuing another
    # gameplay action or repeating the saved census/binding observations.
    $startupQuietWitness = Get-LiteralStartupMssWitnessSnapshot `
        $pairingResult $legacyQuietWitness
    $startupQuietWitness.PreparedDeployBindings = $preparedDeployBindings
    $startupQuietWitness.TerminalMapStateCaptured = $true
    $startupMssProof = Assert-LiteralStartupMssWitness `
        $startupQuietWitness $pairingResult $hostProcess $joinProcess `
        -RequireReinforcement:($GameplayMode -in @(
            'canonical', 'battle-block', 'long-move', 'long-attack'))
    if ($GameplayMode -in @('canonical', 'long-move', 'long-attack')) {
        Assert-DeferredLiteralHostAuthoritativeHeroFixture $heroCensus $fixture
        $reinforcementResult = [pscustomobject]@{
            hostCommits = [int]$startupMssProof.HostReinforcementCount
            joinCommits = [int]$startupMssProof.JoinReinforcementCount
            hostUnits = [int]$fixture.host.reinforcement.units
            joinUnits = [int]$fixture.join.reinforcement.units
            replicatedTo = @('host', 'join')
        }
        Write-Step ("protocol-v8 bootstrap/deploy PASS ($GameplayMode): one shared census resolved " +
            "host=$($heroCensus.hostId) join=$($heroCensus.joinId) and proved exact Demiurge groups")
    } else {
        # Non-canonical diagnostics do not claim the literal gameplay boundary.
        Assert-ClientsLive $hostProcess $joinProcess
        $initialHostDay = Get-WorldDay host
        $initialJoinDay = Get-WorldDay join
        if ($initialHostDay -ne 1 -or $initialJoinDay -ne 1) {
            throw "relay rounds start at 1 but client world started at $initialHostDay/$initialJoinDay"
        }
        if ($GameplayMode -eq 'battle-block') {
            $reinforcementResult = Wait-DevouringReinforcementProof `
                $fixture $hostProcess $joinProcess $hostLog $joinLog $StepTimeoutSec
        }
        Write-Step 'protocol-v8 bootstrap PASS; both clients are operational on live strategic maps at day 1'
    }

    if ($GameplayMode -eq 'battle-block') {
        if ($BattleCase -eq 'none') {
            $battleBlockResult = Invoke-BattleBlockProof `
                $fixture $hostProcess $joinProcess $hostLog `
                $startupMssProof.PreparedDeployBindings
        } elseif ($BattleCase -eq 'dead-leader') {
            $deadLeaderRoundEvidence = [pscustomobject]@{
                eventsBefore = @($startupQuietWitness.SimEvents)
                completedDay = 1
                stockTurnWatermark = [long]$startupMssProof.StartupComplete.LatestSequence
            }
            $battleBlockResult = Invoke-DeadLeaderProof `
                $fixture $hostProcess $joinProcess $hostLog $joinLog `
                $startupMssProof.PreparedDeployBindings $simRelay $deadLeaderRoundEvidence
        } else {
            $battleBlockResult = Invoke-BattleAuditProof `
                $fixture $hostProcess $joinProcess $hostLog $joinLog `
                $startupMssProof.PreparedDeployBindings ($BattleCase -eq 'audit-moving')
        }
    } elseif ($GameplayMode -eq 'long-move') {
        if (-not $canonicalDeployComplete) {
            throw 'long-move deploy was not completed immediately after quiet-3'
        }
        $longMoveResult = Invoke-LongMoveProof `
            -Fixture $fixture `
            -HostProcess $hostProcess -JoinProcess $joinProcess `
            -HostLog $hostLog -JoinLog $joinLog `
            -Case $LongMoveCase `
            -TimeoutSec $StepTimeoutSec
        if ($LongMoveCase -ne 'clean-long-concurrency' -and
            -not [bool]$longMoveResult.completed) {
            throw "long-move $LongMoveCase diagnostic did not settle and converge"
        }
        $gameplayResult = [pscustomobject]@{
            fixture = [string]$fixture.name
            longMove = $longMoveResult
        }
    } elseif ($GameplayMode -eq 'long-attack') {
        if (-not $canonicalDeployComplete) {
            throw 'long-attack deploy was not completed immediately after quiet-3'
        }
        $longAttackResult = Invoke-LongAttackProof `
            -Fixture $fixture `
            -HostProcess $hostProcess -JoinProcess $joinProcess `
            -HostLog $hostLog -JoinLog $joinLog `
            -TimeoutSec $StepTimeoutSec
        # The source long_attack finished with one common End Turn and required
        # exact MP=35/35. Reuse the already-proved one-shot round path, but do
        # not append a return walk or enter the merge barrier.
        $canonicalRoundPreparedEvidence = [pscustomobject]@{
            eventsBefore = @($startupQuietWitness.SimEvents)
            completedDay = 1
            stockTurnWatermark =
                [long]$startupMssProof.StartupComplete.LatestSequence
        }
        $longAttackEndTurn = Run-IndependentRound `
            1 $simRelay $hostProcess $joinProcess $hostLog $joinLog $fixture `
            -PreparedCanonicalEvidence $canonicalRoundPreparedEvidence
        $roundResults += $longAttackEndTurn
        $longAttackResult | Add-Member -NotePropertyName endTurn `
            -NotePropertyValue $longAttackEndTurn
        $gameplayResult = [pscustomobject]@{
            fixture = [string]$fixture.name
            longAttack = $longAttackResult
        }
        Write-Step 'long-attack PASS: both distant battles completed and the sole End Turn pair restored MP=35/35'
    } else {
        if ($GameplayMode -eq 'canonical') {
            if (-not $canonicalDeployComplete) {
                throw 'canonical legacy deploy was not completed immediately after quiet-3'
            }
            $attacks = Invoke-CanonicalConcurrentAttacks `
                $fixture $hostProcess $joinProcess $hostLog $joinLog
            $walk = Invoke-CanonicalParallelWalk `
                $fixture postBattleWalk $hostProcess $joinProcess
            # Preserve only the production-event baseline and completed day.
            # sync_endturn already owns one current /api/state read in its
            # source prelude; that publication, not a cross-phase UI token,
            # supplies the exact End Turn capability later.
            $canonicalRoundPreparedEvidence = [pscustomobject]@{
                eventsBefore = @($startupQuietWitness.SimEvents)
                completedDay = 1
                stockTurnWatermark =
                    [long]$startupMssProof.StartupComplete.LatestSequence
            }
            $gameplayResult = [pscustomobject]@{
                fixture = [string]$fixture.name
                attacks = $attacks
                postBattleWalk = $walk
            }
        }
        $roundCount = if ($MergeDay -eq 0) {
            $IndependentRounds
        } else {
            [Math]::Max(0, $MergeDay - 2)
        }
        for ($round = 1; $round -le $roundCount; $round++) {
            $roundResult = if ($GameplayMode -eq 'canonical') {
                Run-IndependentRound `
                    $round $simRelay $hostProcess $joinProcess $hostLog $joinLog $fixture `
                    -PreparedCanonicalEvidence $canonicalRoundPreparedEvidence `
                    -OnCanonicalLegacyPass {
                        param($walkFixture, $walkHostProcess, $walkJoinProcess)
                        Invoke-CanonicalDay2Walk `
                            $walkFixture $walkHostProcess $walkJoinProcess
                    }
            } else {
                Run-IndependentRound `
                    $round $simRelay $hostProcess $joinProcess $hostLog $joinLog $fixture
            }
            $roundResults += $roundResult
            if ($GameplayMode -eq 'canonical') {
                $day2WalkResult = $roundResult.legacyContinuation
            }
        }
        if ($GameplayMode -eq 'canonical') {
            if ($roundResults.Count -ne 1 -or
                [int]$roundResults[0].hostAfter -ne 2 -or
                [int]$roundResults[0].joinAfter -ne 2) {
                throw 'canonical gameplay requires one exact subjective day-1 -> day-2 round'
            }
            if (-not $day2WalkResult) {
                throw 'canonical day-2 walk did not run after exact local-turn release and legacy round PASS'
            }
        }
        if ($MergeDay -gt 0) {
            if ($GameplayMode -eq 'canonical') {
                $canonicalMergePreparedEvidence = [pscustomobject]@{
                    baselineEvents = @($roundResults[0].deferredMssEvidence.eventsBefore)
                    roundEvidence = $roundResults[0].deferredMssEvidence
                    stockTurnWatermark =
                        [long]$roundResults[0].deferredMssEvidence.stockTurnWatermark
                }
                $mergeResult = Run-MergeBarrier `
                    $simRelay $hostProcess $joinProcess $hostLog $joinLog $BarrierOrder $fixture `
                    -PreparedCanonicalEvidence $canonicalMergePreparedEvidence `
                    -CanonicalLegacy
                # The merge call returns only after the source BARRIER PASS and
                # its deferred MSS proof. Project that already-computed evidence
                # into the one saved canonical round without another observation.
                $canonicalRoundProof = $mergeResult.deferredRoundProof
                if (-not $canonicalRoundProof) {
                    throw 'canonical merge returned without its deferred independent-round proof'
                }
                $canonicalProofCascades = @($canonicalRoundProof.cascades)
                if ($roundResults.Count -ne 1 -or
                    -not $roundResults[0].endTurnProof -or
                    [int]$canonicalRoundProof.confirmationCount -ne 0 -or
                    $canonicalProofCascades.Count -ne 2) {
                    throw 'canonical deferred proof cannot be projected into the one independent round'
                }
                $roundResults[0].endTurnProof.confirmationCount =
                    [int]$canonicalRoundProof.confirmationCount
                $roundResults[0].endTurnProof.causal = [pscustomobject]@{
                    hostBarrier = $canonicalRoundProof.hostBarrier
                    joinBarrier = $canonicalRoundProof.joinBarrier
                }
                $roundResults[0].endTurnProof.cascades = @($canonicalProofCascades)
                $roundResults[0].endTurnProof.cascadeOrder =
                    @($canonicalRoundProof.cascadeOrder)
            } else {
                $mergeResult = Run-MergeBarrier `
                    $simRelay $hostProcess $joinProcess $hostLog $joinLog $BarrierOrder $fixture
            }
            if ($PostMergeContinuationMode -eq 'mss-stock-telemetry') {
                $postMergeStockResult = Run-PostMergeStockProof `
                    $hostProcess $joinProcess $hostLog $joinLog $mergeResult
            }
        }
        if ($GameplayMode -eq 'canonical') {
            # All three legacy gameplay verdicts and the selected merge/Phase-C
            # timeline have already completed.  Only now may append-only MSS
            # history strengthen the saved attack and walk observations.
            $canonicalDeferredEvidence = @(
                $attacks.deferredEvidence,
                $walk.deferredEvidence,
                $day2WalkResult.deferredEvidence
            )
            Assert-CanonicalDeferredGameplayEvidence `
                -Fixture $fixture -Evidence $canonicalDeferredEvidence
            $canonicalDeferredGameplayEvidenceResult = [pscustomobject]@{
                proved = $true
                kinds = @($canonicalDeferredEvidence | ForEach-Object {
                    [string]$_.kind
                })
            }
        }
    }
    }
    if ($ProbeRelayFailure) {
        $faultProbeResult = Invoke-FailClosedProbe $simRelay $hostProcess $joinProcess $hostLog $joinLog
    } else {
        Assert-NoRelayFault @(Read-SimRelayEvents)
        Assert-NoClientFaults $hostLog $joinLog
    }
    Assert-ClientsLive $hostProcess $joinProcess
    if ($script:LiteralInnerStartupObserver) {
        Assert-LiteralInnerStartupObserverHealthy `
            $script:LiteralInnerStartupObserver
        $literalInnerStartupResult = Complete-LiteralInnerStartupObserver `
            $script:LiteralInnerStartupObserver
        $script:LiteralInnerStartupObserver = $null
    } elseif (-not $literalInnerStartupResult) {
        throw 'literal inner startup observer was consumed without a retained result'
    }
    $startupActionsBarrierResult = Assert-PairedStartupActionsRelease `
        -HostPid $hostProcess.Id -JoinPid $joinProcess.Id -HostLog $hostLog -JoinLog $joinLog
    $passed = $true
} catch {
    $failure = $_.Exception.Message
    Write-Host "[simturns-poc] FAIL: $failure" -ForegroundColor Red
    try {
        $failureWorld = [ordered]@{
            capturedAt = [DateTimeOffset]::UtcNow.ToString('O')
            host = Get-World host
            join = Get-World join
        }
        $failureWorld | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath (Join-Path $ArtifactDir 'failure-world.json') -Encoding utf8
    } catch {
        Write-Host "[simturns-poc] failure-world capture unavailable: $($_.Exception.Message)" `
            -ForegroundColor Yellow
    }
    Show-FailureTail $script:SimRelayLog 'sim-relay'
    if ($hostLog) { Show-FailureTail $hostLog 'host-mss' }
    if ($joinLog) { Show-FailureTail $joinLog 'join-mss' }
} finally {
    $teardownErrors = [System.Collections.Generic.List[string]]::new()
    $kept = @()
    try {
        Stop-LiteralInnerStartupObserver $script:LiteralInnerStartupObserver
        $script:LiteralInnerStartupObserver = $null
    } catch {
        $teardownErrors.Add(
            "literal inner startup observer teardown failed: $($_.Exception.Message)")
    }
    try {
        Stop-LiteralOuterReadyObserver $literalReadyObserver
    } catch {
        $teardownErrors.Add(
            "literal outer Is-Ready observer teardown failed: $($_.Exception.Message)")
    }
    if (-not $Keep) {
        if ($passed) {
            try {
                $clientLogQuiescence = Wait-ClientLogsAtCompleteQuietBoundary `
                    -HostProcess $hostProcess -JoinProcess $joinProcess `
                    -HostLog $hostLog -JoinLog $joinLog
                Write-Step ("client logs reached a complete quiet boundary before " +
                    "owned teardown (waited=$($clientLogQuiescence.waitedMilliseconds)ms)")
            } catch {
                $teardownErrors.Add(
                    "client log pre-stop quiescence failed: $($_.Exception.Message)")
            }
        }
        if ($hostProcess -and $joinProcess) {
            try {
                # Issue both exact-handle kills before waiting for either exit;
                # a peer cannot spend the first process's full exit wait writing
                # a disconnect tail that the second kill could bisect.
                $clientPairTeardown = Stop-OwnedClientPair `
                    -HostProcess $hostProcess -JoinProcess $joinProcess
            } catch {
                $teardownErrors.Add(
                    "owned client pair teardown failed: $($_.Exception.Message)")
            }
        } else {
            foreach ($entry in @(
                @{ name = 'host'; process = $hostProcess },
                @{ name = 'join'; process = $joinProcess }
            )) {
                if (-not $entry.process) { continue }
                $ownedPid = $entry.process.Id
                try {
                    Stop-OwnedProcess $entry.process
                } catch {
                    $teardownErrors.Add(
                        "$($entry.name) pid=$ownedPid teardown failed: $($_.Exception.Message)")
                }
            }
        }
        foreach ($entry in @(
            @{ name = 'sim-relay'; process = $simRelay },
            @{ name = 'test-relay'; process = $testRelay }
        )) {
            if (-not $entry.process) { continue }
            $ownedPid = $entry.process.Id
            try {
                Stop-OwnedProcess $entry.process
            } catch {
                $teardownErrors.Add(
                    "$($entry.name) pid=$ownedPid teardown failed: $($_.Exception.Message)")
            }
        }
        if ($teardownErrors.Count -eq 0) {
            Write-Step 'owned clients and relays stopped and observed exited; machine-global dplaysvr left to DirectPlay'
        } else {
            $teardownFailure = 'owned teardown failed: ' + ($teardownErrors -join '; ')
            if ($passed) {
                $passed = $false
                $failure = $teardownFailure
            } elseif ([string]::IsNullOrWhiteSpace($failure)) {
                $failure = $teardownFailure
            } else {
                $failure = "$failure | $teardownFailure"
            }
            Write-Host "[simturns-poc] FAIL: $teardownFailure" -ForegroundColor Red
        }
    } else {
        foreach ($entry in @(
            @{ name = 'host'; process = $hostProcess },
            @{ name = 'join'; process = $joinProcess },
            @{ name = 'sim-relay'; process = $simRelay },
            @{ name = 'test-relay'; process = $testRelay }
        )) {
            if ($entry.process) {
                $entry.process.Refresh()
                if (-not $entry.process.HasExited) { $kept += "$($entry.name) pid=$($entry.process.Id)" }
            }
        }
        Write-Host "[simturns-poc] -Keep retained: $($kept -join ', ')" -ForegroundColor Yellow
    }

    # Finalize logs only after exact owned-process teardown. D2_TESTDRV flushes
    # the diagnostic past before publishing each changed WorldSnapshot; this
    # byte-level gate additionally makes a Kill inside a later log() fail closed
    # instead of allowing a green summary with a partial tail.
    $clientLogErrors = [System.Collections.Generic.List[string]]::new()
    $clientLogCompletion = [ordered]@{
        required = -not [bool]$Keep
        passed = $null
        errors = @()
        preStopQuiescence = $clientLogQuiescence
        host = $null
        join = $null
    }
    foreach ($entry in @(
        @{ role = 'host'; process = $hostProcess; path = $hostLog },
        @{ role = 'join'; process = $joinProcess; path = $joinLog }
    )) {
        if (-not $entry.process -and -not $entry.path) { continue }
        try {
            if (-not $entry.process -or -not $entry.path) {
                throw "$($entry.role) client has an incomplete process/log binding"
            }
            if ($Keep) {
                [void](Copy-ClientLog ([string]$entry.path))
            } else {
                $clientLogCompletion[$entry.role] =
                    Copy-StoppedClientLogWithCompletionProof `
                        -Role $entry.role -Process $entry.process -Path $entry.path
            }
        } catch {
            $clientLogErrors.Add(
                "$($entry.role) client log finalization failed: $($_.Exception.Message)")
        }
    }
    if (-not $Keep -and $passed -and
        ($null -eq $clientLogCompletion.host -or
         $null -eq $clientLogCompletion.join)) {
        $clientLogErrors.Add(
            'green gameplay verdict did not produce two complete exact-owned client logs')
    }
    $clientLogCompletion.passed = if ($Keep) {
        $null
    } else {
        $clientLogErrors.Count -eq 0 -and
            $null -ne $clientLogCompletion.host -and
            $null -ne $clientLogCompletion.join
    }
    $clientLogCompletion.errors = @($clientLogErrors)
    if ($clientLogErrors.Count -ne 0) {
        $logFailure = 'client log completion failed: ' + ($clientLogErrors -join '; ')
        if ($passed) {
            $passed = $false
            $failure = $logFailure
        } elseif ([string]::IsNullOrWhiteSpace($failure)) {
            $failure = $logFailure
        } else {
            $failure = "$failure | $logFailure"
        }
        Write-Host "[simturns-poc] FAIL: $logFailure" -ForegroundColor Red
    }

    $battleTraceAudit = $null
    foreach ($sourceEvidence in @($battleCaseSourceEvidence)) {
        if ($null -ne $sourceEvidence -and
            (Get-FileHash -LiteralPath $sourceEvidence.path -Algorithm SHA256).Hash -cne $sourceEvidence.sha256) {
            $passed = $false
            $failure = (@($failure, "Test source changed during run: $($sourceEvidence.path)") |
                Where-Object { $_ }) -join ' | '
        }
    }
    if ($BattleTrace) {
        $traceReportPath = Join-Path $ArtifactDir 'battle-trace-audit.json'
        try {
            if (-not $clientLogCompletion.passed) {
                throw 'Battle trace audit requires both complete client logs'
            }
            $traceTool = Join-Path $PSScriptRoot 'battle-trace-audit.js'
            $traceNode = Get-Command node -CommandType Application -ErrorAction Stop | Select-Object -First 1
            $traceArgs = @($traceTool, $clientLogCompletion.host.artifactPath,
                $clientLogCompletion.join.artifactPath, '--leader', [string]$fixture.host.reinforcement.leaderId)
            & $traceNode.Source @traceArgs 2> (Join-Path $ArtifactDir 'battle-trace-audit.stderr.log') |
                Set-Content -LiteralPath $traceReportPath -Encoding utf8
            $traceExitCode = $LASTEXITCODE
            $traceReport = Get-Content -LiteralPath $traceReportPath -Raw | ConvertFrom-Json
            $battleTraceAudit = [ordered]@{
                path = $traceReportPath
                sha256 = (Get-FileHash -LiteralPath $traceReportPath -Algorithm SHA256).Hash
                complete = [bool]$traceReport.complete
                combatCorrectnessProved = $false
                exitCode = $traceExitCode
            }
            if ($traceExitCode -ne 0 -or -not $traceReport.complete) {
                throw "Battle trace is incomplete; see $traceReportPath"
            }
        } catch {
            $traceFailure = $_.Exception.Message
            $passed = $false
            $failure = (@($failure, $traceFailure) | Where-Object { $_ }) -join ' | '
            if ($null -eq $battleTraceAudit) {
                $battleTraceAudit = [ordered]@{ complete=$false; error=$traceFailure; path=$traceReportPath }
            }
            Write-Host "[simturns-poc] FAIL: $traceFailure" -ForegroundColor Red
        }
    }

    $summary = [ordered]@{
        passed = $passed
        failure = $failure
        runId = $runId
        gameDir = $GameDir
        scenario = $scenarioSelectionIndex
        scenarioPath = $ScenarioPath
        gameplayMode = $GameplayMode
        battleCase = $BattleCase
        battleCaseSources = $battleCaseSourceEvidence
        battleTraceRequested = [bool]$BattleTrace
        battleTrace = $battleTraceAudit
        longMoveCase = if ($GameplayMode -eq 'long-move') { $LongMoveCase } else { $null }
        fixtureManifest = if ($fixture) { [IO.Path]::GetFullPath($FixtureManifest) } else { $null }
        fixturePlan = $script:FixturePlanEvidence
        fixture = if ($fixture) {
            [ordered]@{
                name = [string]$fixture.name
                executableSha256 = [string]$fixture.executable.sha256
                mapSha256 = [string]$fixture.map.sha256
            }
        } else { $null }
        mergeDay = $MergeDay
        barrierOrder = $BarrierOrder
        reinforcement = $reinforcementResult
        gameplay = $gameplayResult
        day2Walk = $day2WalkResult
        battleBlock = $battleBlockResult
        longMove = $longMoveResult
        longAttack = $longAttackResult
        orderedMasstest = $orderedMasstestResult
        independentRounds = $roundResults
        merge = $mergeResult
        postMergeStock = $postMergeStockResult
        postMergeContinuationMode = [string]$PostMergeContinuationMode
        postMergeAutomaticMasstestPhaseC = $postMergeAutomaticMasstestPhaseCResult
        deferredGameplayEvidence = $canonicalDeferredGameplayEvidenceResult
        literalInnerStartup = $literalInnerStartupResult
        startupActionsBarrier = $startupActionsBarrierResult
        literalInnerBootstrap = [ordered]@{
            releaseFile = $script:LiteralInnerBootstrapReleaseFile
            failureLog = $script:LiteralInnerFailureLog
            dumpBaseline = $literalInnerDumpBaseline
            preexistingClientLogs = $preexistingClientLogArchive
        }
        failClosedProbe = $faultProbeResult
        bootDiagnostics = @($script:BootDiagnostics)
        stepTranscript = @($script:StepTranscript)
        legacyMassPhaseTrace = @($script:LegacyMassPhaseTrace)
        productionPipe = $PipeName
        debugTestPipe = $debugPipeName
        hostPid = if ($hostProcess) { $hostProcess.Id } else { $null }
        joinPid = if ($joinProcess) { $joinProcess.Id } else { $null }
        clientLogs = $clientLogCompletion
        teardown = [ordered]@{
            attempted = -not [bool]$Keep
            passed = $teardownErrors.Count -eq 0
            errors = @($teardownErrors)
            clientPair = $clientPairTeardown
            kept = @($kept)
        }
        artifacts = $ArtifactDir
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $script:SummaryPath -Encoding utf8
}

Write-Host "[simturns-poc] artifacts: $ArtifactDir"
if ($passed) {
    Write-Host '[simturns-poc] RESULT: PASS' -ForegroundColor Green
    exit 0
}
Write-Error "production simultaneous-turn runtime PoC failed: $failure" -ErrorAction Continue
exit 1
