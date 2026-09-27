# Separate observations from the unchanged battle-block oracle. Depends on
# _relay.ps1, _simturns_gameplay.ps1, and Run-IndependentRound in the owning runner.

function Get-BattleCaseHostState {
    param([Parameter(Mandatory)][object]$Fixture,
          [Parameter(Mandatory)][object]$World,
          [switch]$RequireDeadFixture)
    $stack = Get-WorldStackExact $World ([string]$Fixture.host.heroId)
    $leaderId = [string]$Fixture.host.reinforcement.leaderId
    $ids = @($leaderId) + @($Fixture.host.reinforcement.unitIds | ForEach-Object { [string]$_ })
    $states = @(Get-OptionalProperty $stack 'unitStates')
    if ($ids.Count -ne 4 -or @($ids | Select-Object -Unique).Count -ne 4 -or
        $states.Count -ne 4 -or [int]$stack.units -ne 4) {
        throw 'battle case: expected the exact leader and three reinforcement units'
    }
    $byId = @{}
    foreach ($unit in $states) {
        $id = [string](Get-OptionalProperty $unit 'id')
        $hp = Get-OptionalProperty $unit 'hp'
        if ($id -notin $ids -or $byId.ContainsKey($id) -or $null -eq $hp -or
            $hp -is [bool] -or [long]$hp -lt 0 -or [long]$hp -gt [int]::MaxValue) {
            throw 'battle case: malformed, duplicate, or unexpected unit HP record'
        }
        $byId[$id] = [int]$hp
    }
    $reportedLeader = Get-OptionalProperty $stack 'leaderId'
    if ($null -ne $reportedLeader -and [string]$reportedLeader -ne $leaderId) {
        throw 'battle case: native leader identity differs from the fixture'
    }
    [bool]$alive = $byId[$leaderId] -gt 0
    foreach ($flag in @('leaderAlive', 'leaderDead')) {
        $value = Get-OptionalProperty $stack $flag
        if ($null -ne $value -and ($value -isnot [bool] -or
            [bool]$value -ne $(if ($flag -eq 'leaderAlive') { $alive } else { -not $alive }))) {
            throw "battle case: native $flag contradicts the leader HP"
        }
    }
    if ($RequireDeadFixture -and ($alive -or
        @($Fixture.host.reinforcement.unitIds | Where-Object { $byId[[string]$_] -ne 200 }).Count)) {
        throw 'dead-leader: expected leader HP0 and all three reinforcements HP200'
    }
    return [pscustomobject]@{
        heroId = [string]$stack.id; leaderId = $leaderId; leaderHp = $byId[$leaderId]
        leaderAlive = $alive; units = @($states | ForEach-Object { [pscustomobject]@{id=[string]$_.id;hp=[int]$_.hp} })
        x = [int]$stack.x; y = [int]$stack.y
        movement = [int]$stack.movement; day = Get-OptionalProperty $World 'day'
        sequence = $(if ($null -ne (Get-OptionalProperty $World 'seq')) { $World.seq } else { $World.worldSeq })
    }
}

function Get-BattleAuditWorld {
    param($Fixture, $World, [ValidateSet('deploy','complete')][string]$Stage, [bool]$MovePeer)
    $sequence = if ($null -ne (Get-OptionalProperty $World 'seq')) { $World.seq } else { Get-OptionalProperty $World 'worldSeq' }
    if ($null -eq $sequence -or [long]$sequence -le 0) { throw 'battle-audit: world sequence is missing' }
    $snapshot = [ordered]@{worldSeq=[long]$sequence;day=(Get-OptionalProperty $World 'day')}
    foreach ($side in @('host','join')) {
        $spec = $Fixture.$side
        $hero = Get-WorldStackExact $World ([string]$spec.heroId)
        $state = Get-BattleCaseHostState ([pscustomobject]@{host=$spec}) $World
        $expected = if ($Stage -eq 'complete' -and $side -eq 'host') { $spec.battleEnd }
            elseif ($Stage -eq 'complete' -and $MovePeer) { [pscustomobject]@{x=19;y=27;movement=23} }
            else { $spec.deploy }
        if ([string]$hero.owner -ne [string]$Fixture.players."${side}Handle" -or
            $state.x -ne [int]$expected.x -or $state.y -ne [int]$expected.y -or
            $state.movement -ne [int]$expected.movement) {
            throw "battle-audit: $Stage $side hero owner/coordinates/MP differ from the pinned endpoint"
        }
        $leader = @($state.units | Where-Object id -eq $spec.reinforcement.leaderId)[0]
        # Every saved value comes from this publication, never from expected endpoints.
        $snapshot[$side] = [pscustomobject]@{id=[string]$hero.id;owner=[string]$hero.owner
            x=$state.x;y=$state.y;movement=$state.movement;worldSeq=[long]$sequence
            leaderId=[string]$leader.id;leaderHp=[int]$leader.hp;leaderAlive=$state.leaderAlive
            leaderIdentitySource='fixture-matched observed unit';units=$state.units}
    }
    return [pscustomobject]$snapshot
}

function New-DeadLeaderMovementLedger {
    param([Parameter(Mandatory)][int]$InitialMovement)
    if ($InitialMovement -lt 12) {
        throw 'dead-leader: two full cost-6 steps require at least 12 movement points'
    }
    # No unproved clamp/retry: only two complete charges on these known safe cells.
    @(
        [pscustomobject]@{fromX=27;fromY=16;toX=27;toY=17;movementBefore=$InitialMovement;movementAfter=$InitialMovement-6}
        [pscustomobject]@{fromX=27;fromY=17;toX=27;toY=16;movementBefore=$InitialMovement-6;movementAfter=$InitialMovement-12}
    )
}

function Start-BattleCaseOnce {
    param([Parameter(Mandatory)][object]$Fixture,
          [Parameter(Mandatory)][ValidateSet('host','join')][string]$Role,
          [System.Diagnostics.Process]$HostProcess,
          [System.Diagnostics.Process]$JoinProcess,
          [Parameter(Mandatory)][string]$ClientLog)
    $spec = $Fixture.$Role
    foreach ($observer in @('host','join')) {
        $world = Get-World $observer
        $hero = Get-WorldStackExact $world ([string]$spec.heroId)
        $target = Get-WorldStackExact $world ([string]$spec.target.id)
        $owner = if ($Role -eq 'host') { $Fixture.players.hostHandle } else { $Fixture.players.joinHandle }
        if ([string]$hero.owner -ne [string]$owner -or
            [string]$target.owner -ne [string]$Fixture.players.neutralHandle -or
            [int]$hero.x -ne [int]$spec.deploy.x -or [int]$hero.y -ne [int]$spec.deploy.y -or
            [int]$hero.movement -ne [int]$spec.deploy.movement -or
            [int]$target.x -ne [int]$spec.target.x -or [int]$target.y -ne [int]$spec.target.y) {
            throw "battle case: $observer world does not match the pinned $Role attack origin/target"
        }
    }
    $watermarks = Get-CanonicalWorldWatermarks
    $uiBefore = Get-RoleEvidenceSequence $Role ui
    $binding = Get-MapActionTargetBinding $Role
    if (-not (Move-Stack $Role ([string]$spec.heroId) `
        ([int]$spec.deploy.x) ([int]$spec.deploy.y) ([int]$spec.target.x) ([int]$spec.target.y) `
        $binding.Instance $binding.Appearance -CommandTimeoutMilliseconds 8000)) {
        throw "battle case: sole $Role attack command was not issued"
    }
    $battleUi = Wait-UiEvidence -Role $Role -After $uiBefore -TimeoutSec 60 `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -Description "battle case $Role first battle" `
        -Predicate { param($ui) [string]$ui.dialog -eq 'DLG_BATTLE_A' }
    # Only observe the already armed callback; never toggle auto-battle here.
    $deadline = (Get-Date).AddSeconds(60)
    $autoBattle = $null
    do {
        Assert-ClientsLive $HostProcess $JoinProcess
        $autoBattle = Read-PrearmedAutoBattleProof -Role $Role `
            -Lines @(Read-ClientLogLines $ClientLog) -BattleUi $battleUi
        if ($null -ne $autoBattle) { break }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    if ($null -eq $autoBattle) { throw "battle case: no exact $Role prearmed auto-battle proof" }
    return [pscustomobject]@{ role=$Role; ui=$battleUi; autoBattle=$autoBattle; watermarks=$watermarks }
}

function Invoke-BattleAuditProof {
    param([Parameter(Mandatory)][object]$Fixture,
          [System.Diagnostics.Process]$HostProcess,
          [System.Diagnostics.Process]$JoinProcess,
          [Parameter(Mandatory)][string]$HostLog,
          [Parameter(Mandatory)][string]$JoinLog,
          [Parameter(Mandatory)][object]$PreparedDeployBindings,
          [bool]$MovePeer = $false)
    if ($MovePeer -and ([int]$Fixture.join.deploy.x -ne 15 -or
        [int]$Fixture.join.deploy.y -ne 27 -or [int]$Fixture.join.deploy.movement -ne 35)) {
        throw 'battle-audit: peer route requires the pinned (15,27)/MP35 origin'
    }
    $planned = @(for ($i=0; $i -lt 4; $i++) {
        [pscustomobject]@{fromX=15+$i;fromY=27;toX=16+$i;toY=27;movementBefore=35-3*$i;movementAfter=32-3*$i}
    })
    Invoke-CanonicalDeploy $Fixture $HostProcess $JoinProcess $PreparedDeployBindings
    if (-not (Wait-ActionablePair -TimeoutSec 60 -QuietSec 3)) { throw 'battle-audit: deploy did not settle' }
    $auditDeployment = [ordered]@{}
    foreach ($observer in @('host','join')) {
        $auditDeployment[$observer] = Get-BattleAuditWorld $Fixture (Get-World $observer) deploy $MovePeer
    }
    foreach ($side in @('host','join')) {
        if (($auditDeployment.host.$side.units | ConvertTo-Json -Compress) -cne
            ($auditDeployment.join.$side.units | ConvertTo-Json -Compress)) {
            throw "battle-audit: deployed $side unit HP disagrees between role-worlds"
        }
    }
    $battle = Start-BattleCaseOnce $Fixture host $HostProcess $JoinProcess $HostLog
    $moves = @()
    if ($MovePeer) {
        $moves = @(Invoke-PinnedMovementLedgerWhileBattleLive -MoverRole join -BattleRole host `
            -HeroId ([string]$Fixture.join.heroId) -Steps $planned `
            -MapBinding (Get-MapActionTargetBinding join) `
            -BattleAppearance ([long]$battle.autoBattle.kick.appearance) `
            -BattleOwner ([long]$battle.autoBattle.kick.owner) `
            -HostProcess $HostProcess -JoinProcess $JoinProcess -Context 'battle-audit peer east')
    }
    $completion = Complete-CanonicalBattle -Role host -Spec $Fixture.host -BattleUi $battle.ui `
        -HostProcess $HostProcess -JoinProcess $JoinProcess -AutoBattleAlreadyEnabled
    $observations = [ordered]@{}
    $postBattleWorlds = [ordered]@{}
    foreach ($observer in @('host','join')) {
        $world = Wait-WorldEvidence -Role $observer -After ([long]$battle.watermarks[$observer]) `
            -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 30 `
            -Description 'battle-audit completed battle and leader HP' -Predicate {
                param($world)
                $hero = Get-WorldStackExact $world ([string]$Fixture.host.heroId) -AllowMissing
                $target = Get-WorldStackExact $world ([string]$Fixture.host.target.id) -AllowMissing
                return $hero -and -not $target -and [int]$hero.x -eq [int]$Fixture.host.battleEnd.x -and
                    [int]$hero.y -eq [int]$Fixture.host.battleEnd.y
            }
        $observations[$observer] = Get-BattleCaseHostState $Fixture $world
        $postBattleWorlds[$observer] = Get-BattleAuditWorld $Fixture $world complete $MovePeer
    }
    foreach ($side in @('host','join')) {
        if (($postBattleWorlds.host.$side.units | ConvertTo-Json -Compress) -cne
            ($postBattleWorlds.join.$side.units | ConvertTo-Json -Compress)) {
            throw "battle-audit: completed $side unit HP disagrees between role-worlds"
        }
    }
    Write-Step "battle-audit observation: host leader HP=$($observations.host.leaderHp); alive=$($observations.host.leaderAlive); peerMoves=$($moves.Count)"
    return [pscustomobject]@{
        observationOnly=$true; combatCorrectnessProved=$false; peerMoved=$MovePeer
        autoBattle=$battle.autoBattle; completion=$completion; peerMoves=$moves
        postBattle=[pscustomobject]$observations
        auditDeployment=[pscustomobject]$auditDeployment;postBattleWorlds=[pscustomobject]$postBattleWorlds
    }
}

function Invoke-DeadLeaderProof {
    param([Parameter(Mandatory)][object]$Fixture,
          [System.Diagnostics.Process]$HostProcess,
          [System.Diagnostics.Process]$JoinProcess,
          [Parameter(Mandatory)][string]$HostLog,
          [Parameter(Mandatory)][string]$JoinLog,
          [Parameter(Mandatory)][object]$PreparedDeployBindings,
          [System.Diagnostics.Process]$RelayProcess,
          [Parameter(Mandatory)][object]$PreparedCanonicalEvidence)
    if (@(Get-OptionalProperty $PreparedCanonicalEvidence 'eventsBefore').Count -eq 0 -or
        [long](Get-OptionalProperty $PreparedCanonicalEvidence 'stockTurnWatermark') -le 0 -or
        [int](Get-OptionalProperty $PreparedCanonicalEvidence 'completedDay') -ne 1) {
        throw 'dead-leader: day-1 startup evidence is required before any action'
    }
    $before = [ordered]@{}
    foreach ($role in @('host','join')) {
        $before[$role] = Get-BattleCaseHostState $Fixture (Get-World $role) -RequireDeadFixture
    }
    # New shuttle case only: pin deploy MP too, so its later return to the exit
    # is a distinct causal intent. Legacy deploy/attack replay guards stay unchanged.
    foreach ($observer in @('host','join')) {
        $world = Get-World $observer
        foreach ($side in @('host','join')) {
            $spec = $Fixture.$side
            $hero = Get-WorldStackExact $world ([string]$spec.heroId)
            $binding = $PreparedDeployBindings.$side
            if ([string]$hero.owner -ne [string]$Fixture.players."${side}Handle" -or
                -not [bool]$hero.inside -or [int]$hero.x -ne [int]$spec.anchor.x -or
                [int]$hero.y -ne [int]$spec.anchor.y -or [int]$hero.movement -ne 35 -or
                [string]$binding.role -ne $side -or [long]$binding.instance -lt 1 -or
                [long]$binding.instance -gt [uint32]::MaxValue -or [long]$binding.appearance -lt 1 -or
                [long]$binding.appearance -gt [uint32]::MaxValue) {
                throw "dead-leader: $observer world or saved $side deploy binding is not the exact garrison/MP35 precondition"
            }
        }
    }
    foreach ($side in @('host','join')) {
        $spec = $Fixture.$side; $binding = $PreparedDeployBindings.$side
        if (-not (Move-Stack $side ([string]$spec.heroId) ([int]$spec.garrison.x) ([int]$spec.garrison.y) `
            ([int]$spec.deploy.x) ([int]$spec.deploy.y) $binding.instance $binding.appearance -ExpectedMovement 35)) {
            throw "dead-leader: sole MP-pinned $side deploy was not issued"
        }
        Start-Sleep -Seconds $(if ($side -eq 'host') { 2 } else { 3 })
    }
    if (-not (Wait-ActionablePair -TimeoutSec 60 -QuietSec 3)) { throw 'dead-leader: deploy did not settle' }
    $deployed = [ordered]@{}
    foreach ($role in @('host','join')) {
        $deployed[$role] = Get-BattleCaseHostState $Fixture (Get-World $role) -RequireDeadFixture
        if ($deployed[$role].x -ne 27 -or $deployed[$role].y -ne 16 -or
            $deployed[$role].movement -ne [int]$Fixture.host.deploy.movement -or
            $null -eq $deployed[$role].day -or [int]$deployed[$role].day -ne 1) {
            throw 'dead-leader: safe shuttle origin or initial movement differs from fixture'
        }
    }
    $planned = @(New-DeadLeaderMovementLedger -InitialMovement $deployed.host.movement)
    $binding = Get-MapActionTargetBinding host
    $battle = Start-BattleCaseOnce $Fixture join $HostProcess $JoinProcess $JoinLog
    $moves = @(Invoke-PinnedMovementLedgerWhileBattleLive -MoverRole host -BattleRole join `
        -HeroId ([string]$Fixture.host.heroId) -Steps $planned -MapBinding $binding `
        -BattleAppearance ([long]$battle.autoBattle.kick.appearance) `
        -BattleOwner ([long]$battle.autoBattle.kick.owner) `
        -HostProcess $HostProcess -JoinProcess $JoinProcess -Context 'dead-leader cost-6 shuttle')
    $after = [ordered]@{}
    foreach ($role in @('host','join')) {
        $after[$role] = Get-BattleCaseHostState $Fixture (Get-World $role) -RequireDeadFixture
        if ($after[$role].x -ne 27 -or $after[$role].y -ne 16 -or
            $after[$role].movement -ne ($deployed.host.movement-12)) {
            throw 'dead-leader: final two-charge endpoint differs between role-worlds'
        }
    }
    $completion = Complete-CanonicalBattle -Role join -Spec $Fixture.join -BattleUi $battle.ui `
        -HostProcess $HostProcess -JoinProcess $JoinProcess -AutoBattleAlreadyEnabled
    if (-not (Wait-ActionablePair -TimeoutSec 60 -QuietSec 3)) {
        throw 'dead-leader: completed join battle did not release both strategic maps'
    }
    $turnWatermarks = Get-CanonicalWorldWatermarks
    # Reuse the original round oracle, including its literal 45-second MP35/35
    # refresh, exact two cascades, no stock advance, and native BeginTurn release.
    $round = Run-IndependentRound 1 $RelayProcess $HostProcess $JoinProcess $HostLog $JoinLog `
        $Fixture -PreparedCanonicalEvidence $PreparedCanonicalEvidence
    if ($round.legacyPass -ne $true -or $round.hostBefore -ne 1 -or $round.joinBefore -ne 1 -or
        $round.hostAfter -ne 2 -or $round.joinAfter -ne 2 -or
        $round.movementRefresh.host -ne 35 -or $round.movementRefresh.join -ne 35) {
        throw 'dead-leader: existing independent round did not prove the day-2 MP35/35 refresh'
    }
    $refreshed = [ordered]@{}
    $refreshWorlds = [ordered]@{}
    foreach ($role in @('host','join')) {
        $world = Wait-WorldEvidence -Role $role -After ([long]$turnWatermarks[$role]) `
            -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 30 `
            -Description 'dead-leader day-2 world after the proved independent round' -Predicate {
                param($world)
                $day = Get-OptionalProperty $world 'day'
                $hero = Get-WorldStackExact $world ([string]$Fixture.host.heroId) -AllowMissing
                return $null -ne $day -and [int]$day -eq 2 -and $hero -and
                    [int]$hero.x -eq 27 -and [int]$hero.y -eq 16 -and [int]$hero.movement -eq 35
            }
        $refreshWorlds[$role] = $world
        $refreshed[$role] = Get-BattleCaseHostState $Fixture $world -RequireDeadFixture
        if ($refreshed[$role].x -ne 27 -or $refreshed[$role].y -ne 16 -or
            $refreshed[$role].movement -ne 35) {
            throw 'dead-leader: day-2 host shuttle position or MP35 differs between role-worlds'
        }
    }
    $refreshLatest = [ordered]@{}
    foreach ($role in @('host','join')) {
        $refreshLatest[$role] = Get-BattleCaseHostState $Fixture (Get-World $role) -RequireDeadFixture
        $latest = $refreshLatest[$role]
        if ($null -eq $latest.day -or [int]$latest.day -ne 2 -or $latest.x -ne 27 -or
            $latest.y -ne 16 -or $latest.movement -ne 35) {
            throw "dead-leader: day-2 postcondition did not remain converged on the $role world"
        }
    }
    return [pscustomobject]@{
        beforeDeploy=[pscustomobject]$before; deployed=[pscustomobject]$deployed
        plannedSteps=$planned; steps=$moves; after=[pscustomobject]$after
        movementBefore=$deployed.host.movement; movementAfter=$after.host.movement
        exhaustionProved=$false; dayRefreshProved=$true
        coverage='Two full cost-6 moves during one continuous join battle, completed battle, and day-2 MP35 refresh; no exhaustion proof.'
        joinBattleCompletion=$completion; independentRound=$round; refreshed=[pscustomobject]$refreshed
        refreshWorlds=[pscustomobject]$refreshWorlds;refreshLatest=[pscustomobject]$refreshLatest
        joinBattleUi=$battle.ui; joinAutoBattle=$battle.autoBattle; joinBattleSpec=$Fixture.join
    }
}

