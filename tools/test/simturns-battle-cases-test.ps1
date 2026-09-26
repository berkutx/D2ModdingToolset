#requires -Version 7.0
# Offline caller/observation contracts. No processes, games, relay, or network are started.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '_simturns_gameplay.ps1')
. (Join-Path $PSScriptRoot '_simturns_battle_cases.ps1')
$script:RealWaitWorldEvidence = (Get-Command Wait-WorldEvidence -CommandType Function).ScriptBlock

function Get-OptionalProperty($Object, [string]$Name) {
    if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]) { $Object.$Name }
}
function Assert-Case($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Rejected([scriptblock]$Action, [string]$Pattern) {
    try { & $Action; throw 'EXPECTED_REJECTION_NOT_RAISED' }
    catch { if ($_.Exception.Message -notlike $Pattern) { throw } }
}
function Reset-Case([int]$LeaderHp = 115) {
    $script:fixture = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'fixtures/simturns-russobit.json') -Raw | ConvertFrom-Json
    $script:state = @{ deploy=0; attacks=0; ledgers=0; completions=0; rounds=0; role=''; accept=$true
        callbackCount=1; failLedger=$false; hpAfter=24; plans=@(); worlds=@{}
        failCompletion=$false; failRound=$false; refreshDay=2; refreshMovement=35
        refreshLeaderHp=0; refreshUnitHp=200; refreshX=27; roundPass=$true
        deploymentCommands=@();deployTimeline=@();afterCompletion=$null;afterDeploy=$null
        useRealWorldWait=$false;historyMode='stale-then-current';histories=@{};historyReads=0
        clock=[DateTime]'2026-09-09T00:00:00Z';regressLatest=$false }
    foreach ($role in @('host','join')) {
        $stacks = @(foreach ($side in @('host','join')) {
            $spec = $fixture.$side
            $hp = if ($side -eq 'host') { $LeaderHp } else { 100 }
            $units = @([pscustomobject]@{ id=$spec.reinforcement.leaderId; hp=$hp }) +
                @($spec.reinforcement.unitIds | ForEach-Object { [pscustomobject]@{id=$_;hp=200} })
            $owner=if($side -eq 'host'){$fixture.players.hostHandle}else{$fixture.players.joinHandle}
            $position=if($LeaderHp -eq 0){$spec.anchor}else{$spec.deploy}
            [pscustomobject]@{ id=$spec.heroId; x=$position.x; y=$position.y; owner=$owner;inside=($LeaderHp -eq 0)
                movement=$spec.deploy.movement; units=4; unitStates=$units; hp=600+$hp
                leaderId=$spec.reinforcement.leaderId }
            [pscustomobject]@{ id=$spec.target.id; x=$spec.target.x; y=$spec.target.y; movement=25; owner=$fixture.players.neutralHandle }
        })
        $state.worlds[$role] = [pscustomobject]@{ day=1; worldSeq=100; seq=100; stacks=$stacks }
    }
}
function Get-World([string]$Role) { $state.worlds[$Role] }
function Get-Date {
    if ($state.useRealWorldWait) { $state.clock=$state.clock.AddSeconds(1); return $state.clock }
    Microsoft.PowerShell.Utility\Get-Date
}
function Get-WorldHistory($Role, $After) {
    $state.historyReads++
    [pscustomobject]@{events=@($state.histories[$Role] | Where-Object { $_.seq -gt $After })}
}
function Get-RoleEvidenceSequence([string]$Role, [string]$Kind) {
    if ($Kind -eq 'world') { $state.worlds[$Role].seq } else { 100 }
}
function Get-MapActionTargetBinding([string]$Role) { [pscustomobject]@{Instance=31;Appearance=46} }
function Write-Step([string]$Message) {}
function Start-Sleep($Seconds, $Milliseconds) { if ($Seconds) { $state.deployTimeline += "sleep$Seconds" } }
function Assert-ClientsLive($HostProcess, $JoinProcess) {}
function Wait-ActionablePair($TimeoutSec, $QuietSec) { $true }
function Invoke-CanonicalDeploy($Fixture, $HostProcess, $JoinProcess, $PreparedBindings) {
    $state.deploy++
    if ($state.afterDeploy) { & $state.afterDeploy }
}
function Move-Stack($Role, $Id, $FromX, $FromY, $X, $Y, $Instance, $Appearance,
                    $ExpectedMovement = -1, $CommandTimeoutMilliseconds = 0) {
    if ($X -eq $fixture.$Role.deploy.x -and $Y -eq $fixture.$Role.deploy.y) {
        Assert-Case ($FromX -eq $fixture.$Role.garrison.x -and $FromY -eq $fixture.$Role.garrison.y -and
            $ExpectedMovement -eq 35 -and $Instance -eq 31 -and $Appearance -eq 46 -and
            $CommandTimeoutMilliseconds -eq 0) 'MP-pinned deploy preserves exact old gesture/binding/deadline'
        $state.deploy++; $state.deployTimeline += $Role
        $state.deploymentCommands += [pscustomobject]@{id=$Id;fromX=$FromX;fromY=$FromY;x=$X;y=$Y;mp=$ExpectedMovement}
        foreach ($world in $state.worlds.Values) {
            $hero=Get-WorldStackExact $world $Id
            Assert-Case ($hero.inside -and $hero.movement -eq $ExpectedMovement) 'native garrison MP precondition'
            $hero.x=$X; $hero.y=$Y; $hero.inside=$false; $world.seq++; $world.worldSeq++
        }
        return $state.accept
    }
    $state.attacks++; $state.role=$Role
    Assert-Case ($Id -eq $fixture.$Role.heroId -and $X -eq $fixture.$Role.target.x -and
        $Y -eq $fixture.$Role.target.y -and $CommandTimeoutMilliseconds -eq 8000) 'sole pinned attack arguments'
    return $state.accept
}
function Wait-UiEvidence($Role, $After, $TimeoutSec, $HostProcess, $JoinProcess, $Description, $Predicate) {
    $ui=[pscustomobject]@{dialog='DLG_BATTLE_A';dialogInstance=46;dialogAppearance=46;seq=101
        targets=@([pscustomobject]@{dialog='DLG_BATTLE_A';instance=31})}
    Assert-Case (& $Predicate $ui) 'battle UI predicate'
    return $ui
}
function Read-ClientLogLines([string]$Path) {
    $proof=[ordered]@{schema=1;mode='preboot-first-battle';role=$state.role;succeeded=$true
        appearance=46;owner=31;bindAgeMs=2500;callbackCount=$state.callbackCount
        functorVftable=0x006F45D4;dispatchFunction=0x00644150;memberFunction=0x00635509
        thisAdjustor=0;controllerGateBefore=0;kickStateBefore=0;kickStateAfter=1
        sideSelector=1;flag38Before=0;flag38After=1;flag39Before=0;flag39After=0}
    '[testdrv][auto-battle-proof] '+($proof | ConvertTo-Json -Compress)
}
function Invoke-PinnedMovementLedgerWhileBattleLive($MoverRole, $BattleRole, $HeroId, $Steps,
    $MapBinding, $BattleAppearance, $BattleOwner, $HostProcess, $JoinProcess, $Context) {
    $state.ledgers++; $state.plans=@($Steps)
    if ($state.failLedger) { throw 'mock continuous battle interval ended' }
    Assert-Case ($MoverRole -ne $BattleRole -and $BattleAppearance -eq 46 -and $BattleOwner -eq 31) 'ledger battle identity'
    foreach ($step in $Steps) {
        foreach ($world in $state.worlds.Values) {
            $hero=Get-WorldStackExact $world $HeroId
            Assert-Case ($hero.x -eq $step.fromX -and $hero.y -eq $step.fromY -and
                $hero.movement -eq $step.movementBefore) 'ledger exact pre-command origin'
            $hero.x=$step.toX; $hero.y=$step.toY; $hero.movement=$step.movementAfter
            $world.worldSeq++; $world.seq++
        }
        $step
    }
}
function Complete-CanonicalBattle($Role, $Spec, $BattleUi, $HostProcess, $JoinProcess,
                                  [switch]$AutoBattleAlreadyEnabled) {
    Assert-Case ([bool]$AutoBattleAlreadyEnabled) 'completion requires prearmed proof'
    $state.completions++
    if ($state.failCompletion) { throw 'mock battle completion failed' }
    foreach ($world in $state.worlds.Values) {
        $world.stacks=@($world.stacks | Where-Object {$_.id -ne $Spec.target.id})
        $hero=Get-WorldStackExact $world $Spec.heroId
        $hero.x=$Spec.battleEnd.x; $hero.y=$Spec.battleEnd.y; $hero.movement=$Spec.battleEnd.movement
        ($hero.unitStates | Where-Object id -eq $Spec.reinforcement.leaderId).hp=$state.hpAfter
        $world.worldSeq++; $world.seq++
    }
    if ($state.afterCompletion) { & $state.afterCompletion }
    [pscustomobject]@{battleClosed=$true;role=$Role}
}
function Run-IndependentRound($Round, $RelayProcess, $HostProcess, $JoinProcess,
    $HostLog, $JoinLog, $Fixture, $PreparedCanonicalEvidence) {
    $state.rounds++
    Assert-Case ($Round -eq 1 -and $state.completions -eq 1 -and $state.role -eq 'join' -and
        $state.ledgers -eq 1 -and $PreparedCanonicalEvidence.completedDay -eq 1 -and
        $PreparedCanonicalEvidence.stockTurnWatermark -eq 10 -and
        $PreparedCanonicalEvidence.eventsBefore[0].event -eq 'saved-startup' -and
        $HostLog -eq 'host.log' -and $JoinLog -eq 'join.log' -and
        [object]::ReferenceEquals($Fixture, $script:fixture)) 'unchanged round caller and causal baseline'
    if ($state.failRound) { throw 'mock original independent round rejected cascade proof' }
    foreach ($observer in @('host','join')) {
        $world=$state.worlds[$observer]
        $world.day=if($observer -eq 'join'){$state.refreshDay}else{2}
        $hero=Get-WorldStackExact $world $Fixture.host.heroId
        $hero.x=$state.refreshX; $hero.movement=$state.refreshMovement
        $hero.unitStates[0].hp=$state.refreshLeaderHp; $hero.unitStates[1].hp=$state.refreshUnitHp
        $world.worldSeq++; $world.seq++
        if ($state.useRealWorldWait) {
            $stale=$world | ConvertTo-Json -Depth 10 | ConvertFrom-Json
            (Get-WorldStackExact $stale $Fixture.host.heroId).movement=23
            $world.worldSeq++; $world.seq++
            $current=$world | ConvertTo-Json -Depth 10 | ConvertFrom-Json
            $state.histories[$observer]=@($stale)
            if ($state.historyMode -ne 'only-stale') { $state.histories[$observer] += $current }
            if ($state.regressLatest) { $hero.movement=23 }
        }
    }
    [pscustomobject]@{legacyPass=$state.roundPass;hostBefore=1;joinBefore=1;hostAfter=2;joinAfter=2
        movementRefresh=[pscustomobject]@{host=35;join=35}}
}
function Wait-WorldEvidence($Role, $After, $HostProcess, $JoinProcess, $TimeoutSec, $Description, $Predicate) {
    if ($state.useRealWorldWait) {
        Assert-Case ($TimeoutSec -eq 30) 'unchanged live world-history timeout'
        return & $script:RealWaitWorldEvidence @PSBoundParameters
    }
    $world=Get-World $Role
    Assert-Case ($world.seq -gt $After -and (& $Predicate $world)) 'post-battle causal world predicate'
    $world
}
function Invoke-Audit([bool]$Moving=$false) {
    Invoke-BattleAuditProof $fixture $null $null 'host.log' 'join.log' @{} $Moving
}
function Invoke-Dead {
    $prepared=[pscustomobject]@{completedDay=1;stockTurnWatermark=10
        eventsBefore=@([pscustomobject]@{event='saved-startup'})}
    $bindings=@{host=[pscustomobject]@{role='host';instance=31;appearance=46}
        join=[pscustomobject]@{role='join';instance=31;appearance=46}}
    Invoke-DeadLeaderProof $fixture $null $null 'host.log' 'join.log' $bindings $null $prepared
}

$script:passed=0
function Test-Case([string]$Name, [scriptblock]$Body) {
    & $Body
    $script:passed++
    Write-Host "PASS $Name"
}
Test-Case 'audit-idle observes surviving leader; no peer/reverse actions' {
    Reset-Case; $r=Invoke-Audit
    Assert-Case ($r.postBattle.host.leaderHp -eq 24 -and $r.postBattle.join.leaderAlive -and
        $r.observationOnly -and -not $r.combatCorrectnessProved -and $state.attacks -eq 1 -and
        $state.ledgers -eq 0 -and $state.completions -eq 1 -and
        $r.auditDeployment.join.join.x -eq 15 -and $r.auditDeployment.join.join.y -eq 27 -and
        $r.auditDeployment.host.join.movement -eq 35 -and $r.postBattleWorlds.join.join.movement -eq 35 -and
        $r.postBattleWorlds.host.host.x -eq 28 -and $r.postBattleWorlds.host.host.y -eq 17 -and
        $r.postBattleWorlds.join.host.movement -eq 15 -and
        $r.auditDeployment.host.host.leaderHp -eq 115) 'idle audit result and saved actual deployment'
}
Test-Case 'audit records dead leader without failing' {
    Reset-Case; $state.hpAfter=0; $r=Invoke-Audit
    Assert-Case ($r.postBattle.host.leaderHp -eq 0 -and -not $r.postBattle.join.leaderAlive) 'death is observation'
}
Test-Case 'audit-moving delegates exactly four fixed east steps' {
    Reset-Case; $r=Invoke-Audit $true
    Assert-Case ($state.attacks -eq 1 -and $state.ledgers -eq 1 -and $r.peerMoves.Count -eq 4 -and
        $state.plans[-1].toX -eq 19 -and $state.plans[-1].movementAfter -eq 23 -and
        $r.auditDeployment.join.join.x -eq 15 -and $r.postBattleWorlds.join.join.x -eq 19 -and
        $r.postBattleWorlds.host.join.movement -eq 23) 'fixed peer ledger and actual two-world snapshots'
}
Test-Case 'incomplete continuous battle fails without retry/completion' {
    Reset-Case; $state.failLedger=$true
    Assert-Rejected {Invoke-Audit $true} '*continuous battle interval ended*'
    Assert-Case ($state.attacks -eq 1 -and $state.ledgers -eq 1 -and $state.completions -eq 0) 'no retry'
}
Test-Case 'dead-leader proves two full cost-6 moves, completes battle, and proves day refresh' {
    Reset-Case 0; $r=Invoke-Dead
    Assert-Case ($r.steps.Count -eq 2 -and $r.movementBefore -eq 35 -and $r.movementAfter -eq 23 -and
        $state.role -eq 'join' -and $state.attacks -eq 1 -and $state.completions -eq 1 -and
        $state.rounds -eq 1 -and -not $r.exhaustionProved -and $r.dayRefreshProved -and
        $r.joinBattleCompletion.battleClosed -and $r.refreshed.host.leaderHp -eq 0 -and
        $r.refreshed.join.day -eq 2 -and $r.refreshed.join.movement -eq 35) 'dead leader coverage'
    Assert-Case (($state.deployTimeline -join ',') -eq 'host,sleep2,join,sleep3' -and
        $state.deploymentCommands.Count -eq 2 -and $state.deploymentCommands[0].mp -eq 35 -and
        $state.deploymentCommands[0].fromX -eq 26 -and $state.plans[1].fromX -eq 27 -and
        $state.plans[1].movementBefore -eq 29) 'deploy and return carry distinct causal origin/MP, without ledger changes'
}
Test-Case 'audit rejects remote deploy XY mismatch even with equal MP' {
    Reset-Case; $state.worlds.join.stacks[2].x=16
    Assert-Rejected {Invoke-Audit} '*deploy join hero owner/coordinates/MP*'
    Assert-Case ($state.attacks -eq 0) 'bad remote deploy does not attack'
}
Test-Case 'audit rejects remote deploy HP disagreement before attacking' {
    Reset-Case; $state.worlds.join.stacks[2].unitStates[0].hp=99
    Assert-Rejected {Invoke-Audit} '*deployed join unit HP disagrees*'
    Assert-Case ($state.attacks -eq 0) 'unreplicated HP does not attack'
}
Test-Case 'audit rejects remote final XY mismatch even with equal MP' {
    Reset-Case; $state.afterCompletion={ $state.worlds.join.stacks[1].x=16 }
    Assert-Rejected {Invoke-Audit} '*complete join hero owner/coordinates/MP*'
}
Test-Case 'audit rejects missing or substituted remote hero identity' {
    Reset-Case; $state.worlds.join.stacks[2].id='0xA3E3FFFF'
    Assert-Rejected {Invoke-Audit} '*0 stacks with id*'
    Assert-Case ($state.attacks -eq 0) 'unknown remote hero does not attack'
}
Test-Case 'audit rejects remote final owner mismatch' {
    Reset-Case; $state.afterCompletion={ $state.worlds.join.stacks[1].owner=$fixture.players.hostHandle }
    Assert-Rejected {Invoke-Audit} '*complete join hero owner/coordinates/MP*'
}
Test-Case 'audit rejects remote final MP mismatch' {
    Reset-Case; $state.afterCompletion={ $state.worlds.join.stacks[1].movement=34 }
    Assert-Rejected {Invoke-Audit} '*complete join hero owner/coordinates/MP*'
}
Test-Case 'audit rejects missing world sequence' {
    Reset-Case; $state.worlds.join.seq=0
    Assert-Rejected {Invoke-Audit} '*world sequence is missing*'
}
Test-Case 'dead pinned deploy rejects remote peer MP before either send' {
    Reset-Case 0; $state.worlds.host.stacks[2].movement=34
    Assert-Rejected {Invoke-Dead} '*garrison/MP35 precondition*'
    Assert-Case ($state.deploy -eq 0) 'no send before the pair preconditions'
}
Test-Case 'missing startup evidence rejected before any action' {
    Reset-Case 0
    Assert-Rejected {Invoke-DeadLeaderProof $fixture $null $null 'host.log' 'join.log' @{} $null @{} } '*startup evidence*'
    Assert-Case ($state.deploy -eq 0 -and $state.attacks -eq 0) 'missing causal baseline no actions'
}
Test-Case 'failed battle completion prevents EndTurn' {
    Reset-Case 0; $state.failCompletion=$true
    Assert-Rejected {Invoke-Dead} '*battle completion failed*'
    Assert-Case ($state.completions -eq 1 -and $state.rounds -eq 0) 'no EndTurn with unclosed battle'
}
Test-Case 'original round rejection propagates without retry' {
    Reset-Case 0; $state.failRound=$true
    Assert-Rejected {Invoke-Dead} '*independent round rejected cascade proof*'
    Assert-Case ($state.rounds -eq 1 -and $state.attacks -eq 1) 'round rejection no retry'
}
Test-Case 'unproved round cannot become day-refresh success' {
    Reset-Case 0; $state.roundPass=$false
    Assert-Rejected {Invoke-Dead} '*did not prove the day-2*'
}
Test-Case 'remote day disagreement rejected' {
    Reset-Case 0; $state.refreshDay=1
    Assert-Rejected {Invoke-Dead} '*causal world predicate*'
}
Test-Case 'day-2 stale movement rejected despite round summary' {
    Reset-Case 0; $state.refreshMovement=23
    Assert-Rejected {Invoke-Dead} '*causal world predicate*'
}
Test-Case 'leader resurrection after refresh rejected' {
    Reset-Case 0; $state.refreshLeaderHp=1
    Assert-Rejected {Invoke-Dead} '*expected leader HP0*'
}
Test-Case 'day-2 damaged reinforcement rejected' {
    Reset-Case 0; $state.refreshUnitHp=199
    Assert-Rejected {Invoke-Dead} '*expected leader HP0*'
}
Test-Case 'day-2 changed shuttle position rejected' {
    Reset-Case 0; $state.refreshX=28
    Assert-Rejected {Invoke-Dead} '*causal world predicate*'
}
Test-Case 'real world-history waiter skips day2 MP23 and selects later complete MP35 event' {
    Reset-Case 0; $state.useRealWorldWait=$true; $r=Invoke-Dead
    foreach ($role in @('host','join')) {
        Assert-Case ($r.refreshWorlds.$role.seq -eq $state.histories[$role][1].seq -and
            $r.refreshed.$role.movement -eq 35 -and $r.refreshLatest.$role.movement -eq 35 -and
            (Get-WorldStackExact $r.refreshWorlds.$role $fixture.host.heroId).movement -eq 35) 'actual later event retained'
    }
    Assert-Case ($state.historyReads -eq 2) 'real waiter reads one two-event history per role'
}
Test-Case 'real world-history waiter rejects only stale day2 MP23 without falling back to latest' {
    Reset-Case 0; $state.useRealWorldWait=$true; $state.historyMode='only-stale'
    Assert-Rejected {Invoke-Dead} '*timed out waiting for host world evidence*'
    Assert-Case ($state.historyReads -gt 1 -and $state.rounds -eq 1) 'bounded observation, no repeated EndTurn'
}
Test-Case 'complete historical refresh does not hide a regressed latest world' {
    Reset-Case 0; $state.useRealWorldWait=$true; $state.regressLatest=$true
    Assert-Rejected {Invoke-Dead} '*did not remain converged*'
}
Test-Case 'alive prefixture rejected before deploy or attack' {
    Reset-Case
    Assert-Rejected {Invoke-Dead} '*expected leader HP0*'
    Assert-Case ($state.deploy -eq 0 -and $state.attacks -eq 0) 'prefixture no actions'
}
Test-Case 'damaged reinforcement rejected on remote world' {
    Reset-Case 0; $state.worlds.join.stacks[0].unitStates[1].hp=199
    Assert-Rejected {Invoke-Dead} '*expected leader HP0*'
    Assert-Case ($state.deploy -eq 0) 'remote prefixture no deploy'
}
Test-Case 'contradictory native dead flag rejected' {
    Reset-Case 0; $state.worlds.host.stacks[0] | Add-Member -NotePropertyName leaderDead -NotePropertyValue $false
    Assert-Rejected {Invoke-Dead} '*leaderDead contradicts*'
}
Test-Case 'missing HP does not silently become zero' {
    Reset-Case 0; $state.worlds.host.stacks[0].unitStates[0].PSObject.Properties.Remove('hp')
    Assert-Rejected {Invoke-Dead} '*malformed*'
}
Test-Case 'wrong leader identity rejected' {
    Reset-Case 0; $state.worlds.join.stacks[0].leaderId='0xA3E40195'
    Assert-Rejected {Invoke-Dead} '*leader identity*'
}
Test-Case 'low movement rejected without inventing final-step clamp' {
    Assert-Rejected {New-DeadLeaderMovementLedger 11} '*at least 12*'
    $steps=@(New-DeadLeaderMovementLedger 12)
    Assert-Case ($steps.Count -eq 2 -and $steps[1].movementBefore -eq 6 -and $steps[1].movementAfter -eq 0) 'full charge endpoint'
}
Test-Case 'refused sole attack is never retried' {
    Reset-Case; $state.accept=$false
    Assert-Rejected {Invoke-Audit} '*sole host attack command was not issued*'
    Assert-Case ($state.attacks -eq 1 -and $state.completions -eq 0) 'refused attack no retry'
}
Test-Case 'real prearmed proof parser rejects duplicate callback' {
    Reset-Case; $state.callbackCount=2
    Assert-Rejected {Invoke-Audit} '*exact Russobit callback invariant*'
}
Test-Case 'changed target ownership rejected before sole attack' {
    Reset-Case; $state.worlds.join.stacks[1].owner=$fixture.players.hostHandle
    Assert-Rejected {Invoke-Audit} '*pinned host attack origin/target*'
    Assert-Case ($state.attacks -eq 0) 'owned target not attacked'
}
Write-Host "RESULT: $passed/$passed offline battle-case contracts passed"

