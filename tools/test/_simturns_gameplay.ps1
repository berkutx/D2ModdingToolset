#requires -Version 7.0
<#
Exact gameplay phase adapter for simturns-production-poc.ps1.

This file deliberately contains no boot, process discovery, cleanup, transport
selection, action repetition or alternate fixture. The caller owns two exact Process
objects and has already completed the MSS protocol-v7 bootstrap. Every mutation
below is submitted once. Loops only consume append-only UI/world evidence after
a watermark; a timeout fails the current cold run.
#>

function Import-SimturnsFixture([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "simturns fixture manifest is missing: $fullPath"
    }
    $fixture = Get-Content -LiteralPath $fullPath -Raw | ConvertFrom-Json -Depth 16
    if (-not $fixture -or [int]$fixture.schema -ne 1) {
        throw "unsupported simturns fixture manifest schema in '$fullPath'"
    }
    foreach ($side in @('host', 'join')) {
        $spec = $fixture.$side
        $battleStart = if ($spec) { $spec.PSObject.Properties['battleStart'] } else { $null }
        $movement = if ($battleStart -and $battleStart.Value) {
            $battleStart.Value.PSObject.Properties['movement']
        } else {
            $null
        }
        $battleX = if ($battleStart -and $battleStart.Value) {
            $battleStart.Value.PSObject.Properties['x']
        } else {
            $null
        }
        $battleY = if ($battleStart -and $battleStart.Value) {
            $battleStart.Value.PSObject.Properties['y']
        } else {
            $null
        }
        if ($null -eq $movement -or [int]$movement.Value -le 0 -or
            $null -eq $battleX -or $null -eq $battleY) {
            throw "simturns fixture $side battleStart x/y/movement is missing or invalid"
        }
        $reinforcement = if ($spec) { $spec.PSObject.Properties['reinforcement'] } else { $null }
        if (-not $reinforcement -or -not $reinforcement.Value -or
            [string]::IsNullOrWhiteSpace([string]$reinforcement.Value.leaderId) -or
            @($reinforcement.Value.unitIds).Count -ne 3 -or
            [int]$reinforcement.Value.units -ne 4 -or
            [int]$reinforcement.Value.hp -le 0 -or
            [int]$spec.deploy.units -ne 4 -or
            [int]$spec.battleStart.units -ne 4) {
            throw "simturns fixture $side exact three-Demiurge reinforcement is missing or invalid"
        }
        $longMove = if ($spec) { $spec.PSObject.Properties['longMove'] } else { $null }
        $cleanLongMove = if ($spec) {
            $spec.PSObject.Properties['cleanLongMove']
        } else {
            $null
        }
        $longAttack = if ($spec) { $spec.PSObject.Properties['longAttack'] } else { $null }
        $longTarget = if ($longAttack -and $longAttack.Value) {
            $longAttack.Value.PSObject.Properties['target']
        } else {
            $null
        }
        if (-not $longMove -or -not $longMove.Value -or
            $null -eq $longMove.Value.PSObject.Properties['x'] -or
            $null -eq $longMove.Value.PSObject.Properties['y'] -or
            -not $cleanLongMove -or -not $cleanLongMove.Value -or
            $null -eq $cleanLongMove.Value.PSObject.Properties['x'] -or
            $null -eq $cleanLongMove.Value.PSObject.Properties['y'] -or
            $null -eq $cleanLongMove.Value.PSObject.Properties['movement'] -or
            [int]$cleanLongMove.Value.movement -lt 0 -or
            -not $longTarget -or -not $longTarget.Value -or
            [string]$longTarget.Value.id -notmatch '^0x[0-9A-Fa-f]{8}$' -or
            $null -eq $longTarget.Value.PSObject.Properties['x'] -or
            $null -eq $longTarget.Value.PSObject.Properties['y']) {
            throw "simturns fixture $side exact long-move/clean-long/long-attack targets are missing or invalid"
        }
    }
    $crossBattle = $fixture.PSObject.Properties['crossBattle']
    $crossJoinTarget = if ($crossBattle -and $crossBattle.Value) {
        $crossBattle.Value.PSObject.Properties['joinTarget']
    } else {
        $null
    }
    $crossHostMove = if ($crossBattle -and $crossBattle.Value) {
        $crossBattle.Value.PSObject.Properties['hostMove']
    } else {
        $null
    }
    if (-not $crossJoinTarget -or -not $crossJoinTarget.Value -or
        [string]$crossJoinTarget.Value.id -notmatch '^0x[0-9A-Fa-f]{8}$' -or
        $null -eq $crossJoinTarget.Value.PSObject.Properties['x'] -or
        $null -eq $crossJoinTarget.Value.PSObject.Properties['y'] -or
        -not $crossHostMove -or -not $crossHostMove.Value -or
        $null -eq $crossHostMove.Value.PSObject.Properties['x'] -or
        $null -eq $crossHostMove.Value.PSObject.Properties['y'] -or
        $null -eq $crossHostMove.Value.PSObject.Properties['movement'] -or
        [int]$crossHostMove.Value.movement -lt 0) {
        throw 'simturns fixture exact reverse cross-battle target/move is missing or invalid'
    }
    $joinWireBudgetProperty = $crossBattle.Value.PSObject.Properties[
        'joinAttackWireBudget']
    $joinStressProperty = $crossBattle.Value.PSObject.Properties['joinStressSteps']
    $hostExhaustionProperty = $crossBattle.Value.PSObject.Properties[
        'hostExhaustionSteps']
    $joinStressSteps = if ($joinStressProperty) {
        @($joinStressProperty.Value)
    } else {
        @()
    }
    $hostExhaustionSteps = if ($hostExhaustionProperty) {
        @($hostExhaustionProperty.Value)
    } else {
        @()
    }
    if (-not $joinWireBudgetProperty -or
        [int]$joinWireBudgetProperty.Value -ne 6 -or
        $joinStressSteps.Count -ne 5 -or
        $hostExhaustionSteps.Count -ne 6) {
        throw 'simturns fixture exact cross-battle stress ledgers are missing or invalid'
    }

    [int]$expectedX = [int]$fixture.join.deploy.x + 4
    [int]$expectedY = [int]$fixture.join.deploy.y
    [int]$expectedMovement = [int]$fixture.join.deploy.movement - 12
    foreach ($step in $joinStressSteps) {
        if ([int]$step.fromX -ne $expectedX -or
            [int]$step.fromY -ne $expectedY -or
            [int]$step.movementBefore -ne $expectedMovement -or
            [Math]::Max([Math]::Abs([int]$step.toX - [int]$step.fromX),
                [Math]::Abs([int]$step.toY - [int]$step.fromY)) -ne 1 -or
            [int]$step.movementAfter -ne ([int]$step.movementBefore - 3)) {
            throw 'simturns fixture join cross-battle stress ledger is not exact/continuous'
        }
        $expectedX = [int]$step.toX
        $expectedY = [int]$step.toY
        $expectedMovement = [int]$step.movementAfter
    }
    if ($expectedMovement -lt [int]$joinWireBudgetProperty.Value) {
        throw 'simturns fixture join cross-battle stress does not reserve the exact attack budget'
    }

    $expectedX = [int]$crossHostMove.Value.x
    $expectedY = [int]$crossHostMove.Value.y
    $expectedMovement = [int]$crossHostMove.Value.movement
    foreach ($step in $hostExhaustionSteps) {
        if ([int]$step.fromX -ne $expectedX -or
            [int]$step.fromY -ne $expectedY -or
            [int]$step.movementBefore -ne $expectedMovement -or
            [Math]::Max([Math]::Abs([int]$step.toX - [int]$step.fromX),
                [Math]::Abs([int]$step.toY - [int]$step.fromY)) -ne 1 -or
            [int]$step.movementAfter -ge [int]$step.movementBefore) {
            throw 'simturns fixture host cross-battle exhaustion ledger is not exact/continuous'
        }
        $expectedX = [int]$step.toX
        $expectedY = [int]$step.toY
        $expectedMovement = [int]$step.movementAfter
    }
    if ($expectedX -ne [int]$fixture.host.postBattleWalk.x -or
        $expectedY -ne [int]$fixture.host.postBattleWalk.y -or
        $expectedMovement -ne 0) {
        throw 'simturns fixture host cross-battle stress does not end on the proved MP=0 cell'
    }
    return $fixture
}

function Assert-SimturnsFixtureFiles([object]$Fixture, [string]$Root) {
    foreach ($entry in @($Fixture.executable, $Fixture.map)) {
        $path = [IO.Path]::GetFullPath((Join-Path $Root ([string]$entry.relativePath)))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "pinned simultaneous-turn fixture file is missing: $path"
        }
        $file = Get-Item -LiteralPath $path
        if ([long]$file.Length -ne [long]$entry.size) {
            throw "pinned fixture size mismatch for '$path': $($file.Length), expected $($entry.size)"
        }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if (-not [string]::Equals($hash, [string]$entry.sha256,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "pinned fixture SHA-256 mismatch for '$path': $hash, expected $($entry.sha256)"
        }
    }
}

function Get-WorldStackExact([object]$World, [string]$Id, [switch]$AllowMissing) {
    $matches = @($World.stacks | Where-Object { [string]$_.id -eq $Id })
    if ($matches.Count -eq 0 -and $AllowMissing) { return $null }
    if ($matches.Count -ne 1) {
        throw "world snapshot contains $($matches.Count) stacks with id $Id"
    }
    return $matches[0]
}

function Get-LegacyStackRole([object]$Stack) {
    # WebDashboard.StackRole classified the source /api/stacks owner by its
    # low 16 bits. Keep transport role `join` distinct from stack role
    # `joiner`; full fixture-handle checks belong only to deferred evidence.
    $owner = [string](Get-OptionalProperty $Stack 'owner')
    if ($owner -notmatch '^0x(?<hex>[0-9A-Fa-f]{8})$') {
        throw "legacy stack owner is not canonical u32 hex: '$owner'"
    }
    $ownerValue = [Convert]::ToUInt32($Matches.hex, 16)
    switch ($ownerValue -band 0xFFFF) {
        0 { return 'neutral' }
        1 { return 'host' }
        2 { return 'joiner' }
        default { return 'other' }
    }
}


function Get-CanonicalHeroUnitVitals([object]$Fixture, [string]$Role = 'host') {
    # sync_endturn.ps1 printed the authoritative host-side HP of every unit by
    # stable CMidgardID, not just each group's sum. Preserve that proof exactly.
    $world = Get-WorldSnapshot $Role
    $result = [ordered]@{ role = $Role; day = [int]$world.day }
    foreach ($side in @('host', 'join')) {
        $spec = $Fixture.$side
        $stack = Get-WorldStackExact $world ([string]$spec.heroId)
        $unitStatesProperty = $stack.PSObject.Properties['unitStates']
        if ($null -eq $unitStatesProperty) {
            throw "$side hero world snapshot omitted per-unit HP states"
        }
        $states = @($unitStatesProperty.Value)
        $expectedIds = @(
            [string]$spec.reinforcement.leaderId
            @($spec.reinforcement.unitIds | ForEach-Object { [string]$_ })
        )
        if ($states.Count -ne $expectedIds.Count) {
            throw "$side hero has $($states.Count) per-unit HP states, expected $($expectedIds.Count)"
        }

        $byId = @{}
        foreach ($state in $states) {
            $idProperty = $state.PSObject.Properties['id']
            $hpProperty = $state.PSObject.Properties['hp']
            if ($null -eq $idProperty -or [string]::IsNullOrWhiteSpace([string]$idProperty.Value) -or
                $null -eq $hpProperty -or $hpProperty.Value -is [bool]) {
                throw "$side hero per-unit HP state omitted typed id/hp"
            }
            [long]$hp = $hpProperty.Value
            if ($hp -lt 0 -or $hp -gt [int]::MaxValue) {
                throw "$side unit $($idProperty.Value) HP is outside int range: $hp"
            }
            [string]$id = $idProperty.Value
            if ($byId.ContainsKey($id)) {
                throw "$side hero per-unit HP state repeated id $id"
            }
            $byId[$id] = [pscustomobject]@{ id = $id; hp = [int]$hp }
        }

        $orderedUnits = @($expectedIds | ForEach-Object {
            if (-not $byId.ContainsKey($_)) {
                throw "$side hero per-unit HP state omitted expected id $_"
            }
            $byId[$_]
        })
        $totalHp = [int](($orderedUnits | Measure-Object -Property hp -Sum).Sum)
        if ($totalHp -ne [int]$stack.hp) {
            throw "$side per-unit HP sum $totalHp does not match group HP $($stack.hp)"
        }
        $result[$side] = [pscustomobject]@{
            heroId = [string]$spec.heroId
            totalHp = $totalHp
            units = $orderedUnits
        }
    }
    return [pscustomobject]$result
}

function Format-CanonicalUnitHp([object]$Hero) {
    return (@($Hero.units | ForEach-Object { "$($_.id)(hp=$($_.hp))" }) -join ' ')
}


function Get-CanonicalWorldWatermarks {
    return @{
        host = Get-RoleEvidenceSequence host world
        join = Get-RoleEvidenceSequence join world
    }
}

function Test-CanonicalTargetState([object]$World,
                                   [object]$Target,
                                   [string]$ExpectedOwner,
                                   [bool]$Present) {
    $stack = Get-WorldStackExact $World ([string]$Target.id) -AllowMissing
    if (-not $Present) { return $null -eq $stack }
    return $stack -and
        [int]$stack.x -eq [int]$Target.x -and
        [int]$stack.y -eq [int]$Target.y -and
        [string]$stack.owner -eq $ExpectedOwner -and
        [string]$stack.relation -eq 'neutral' -and
        -not [bool]$stack.inside
}



function Test-CanonicalHeroState([object]$World,
                                 [object]$Spec,
                                 [object]$Expected,
                                 [string]$ExpectedOwner,
                                 [bool]$Inside) {
    $stack = Get-WorldStackExact $World ([string]$Spec.heroId) -AllowMissing
    if (-not $stack -or
        [int]$stack.x -ne [int]$Expected.x -or
        [int]$stack.y -ne [int]$Expected.y -or
        [string]$stack.owner -ne $ExpectedOwner -or
        [bool]$stack.inside -ne $Inside) {
        return $false
    }
    $movementProperty = $Expected.PSObject.Properties['movement']
    if ($movementProperty -and
        [int]$stack.movement -ne [int]$movementProperty.Value) {
        return $false
    }
    $unitsProperty = $Expected.PSObject.Properties['units']
    return $null -eq $unitsProperty -or
        [int]$stack.units -eq [int]$unitsProperty.Value
}

function Test-CanonicalWorldState([object]$World,
                                  [object]$Fixture,
                                  [int]$Day,
                                  [object]$HostExpected,
                                  [object]$JoinExpected,
                                  [bool]$HostTargetPresent,
                                  [bool]$JoinTargetPresent) {
    if (-not $World -or [int]$World.day -ne $Day) { return $false }
    return (Test-CanonicalHeroState $World $Fixture.host $HostExpected `
                ([string]$Fixture.players.hostHandle) $false) -and
        (Test-CanonicalHeroState $World $Fixture.join $JoinExpected `
                ([string]$Fixture.players.joinHandle) $false) -and
        (Test-CanonicalTargetState $World $Fixture.host.target `
                ([string]$Fixture.players.neutralHandle) $HostTargetPresent) -and
        (Test-CanonicalTargetState $World $Fixture.join.target `
                ([string]$Fixture.players.neutralHandle) $JoinTargetPresent)
}

function Wait-CanonicalWorldConvergence {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][int]$Day,
        [Parameter(Mandatory)][object]$HostExpected,
        [Parameter(Mandatory)][object]$JoinExpected,
        [Parameter(Mandatory)][bool]$HostTargetPresent,
        [Parameter(Mandatory)][bool]$JoinTargetPresent,
        [Parameter(Mandatory)][hashtable]$After,
        [Parameter(Mandatory)][string]$Description,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess,
        [int]$TimeoutSec = 60
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    foreach ($role in @('host', 'join')) {
        $current = Get-World $role
        if (Test-CanonicalWorldState $current $Fixture $Day $HostExpected $JoinExpected `
                $HostTargetPresent $JoinTargetPresent) {
            continue
        }
        $remaining = [Math]::Max(1, [int][Math]::Ceiling(($deadline - (Get-Date)).TotalSeconds))
        [void](Wait-WorldEvidence -Role $role -After ([long]$After[$role]) `
            -Description "$Description on $role" -TimeoutSec $remaining `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -Predicate {
                param($world)
                Test-CanonicalWorldState $world $Fixture $Day $HostExpected $JoinExpected `
                    $HostTargetPresent $JoinTargetPresent
            })
    }
    foreach ($role in @('host', 'join')) {
        if (-not (Test-CanonicalWorldState (Get-World $role) $Fixture $Day `
                $HostExpected $JoinExpected $HostTargetPresent $JoinTargetPresent)) {
            throw "$Description did not remain converged on the $role world snapshot"
        }
    }
}

function Get-RoleEvidenceSequence([string]$Role, [ValidateSet('ui', 'world')]$Kind) {
    $roleState = Get-RoleState $Role
    if (-not $roleState) { throw "DebugTest relay has no $Role role state" }
    $name = if ($Kind -eq 'ui') { 'uiSeq' } else { 'worldSeq' }
    $value = Get-OptionalProperty $roleState $name
    if ($null -eq $value -or [long]$value -lt 0 -or [long]$value -gt [uint32]::MaxValue) {
        throw "$Role $name is missing or outside uint32"
    }
    return [long]$value
}

function Wait-WorldEvidence {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][long]$After,
        [Parameter(Mandatory)][scriptblock]$Predicate,
        [Parameter(Mandatory)][string]$Description,
        [int]$TimeoutSec = 60,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess
    )
    $cursor = $After
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($HostProcess -and $JoinProcess) {
            Assert-ClientsLive $HostProcess $JoinProcess
        }
        $history = Get-WorldHistory $Role $cursor
        foreach ($event in @($history.events | Sort-Object seq)) {
            [long]$seq = $event.seq
            if ($seq -le $cursor) {
                throw "$Role world history repeated/regressed at seq=$seq after watermark=$cursor"
            }
            $cursor = $seq
            if (& $Predicate $event) { return $event }
        }
        Start-Sleep -Milliseconds 250
    }
    throw "timed out waiting for $Role world evidence: $Description"
}

function Wait-UiEvidence {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][long]$After,
        [Parameter(Mandatory)][scriptblock]$Predicate,
        [Parameter(Mandatory)][string]$Description,
        [int]$TimeoutSec = 60,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess
    )
    $cursor = $After
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($HostProcess -and $JoinProcess) {
            Assert-ClientsLive $HostProcess $JoinProcess
        }
        $history = Get-UiHistory $Role $cursor
        foreach ($event in @($history.events | Sort-Object seq)) {
            [long]$seq = $event.seq
            if ($seq -le $cursor) {
                throw "$Role UI history repeated/regressed at seq=$seq after watermark=$cursor"
            }
            $cursor = $seq
            if (& $Predicate $event) { return $event }
        }
        Start-Sleep -Milliseconds 200
    }
    throw "timed out waiting for $Role UI evidence: $Description"
}

function Assert-CanonicalWorldLoaded([object]$Fixture) {
    foreach ($role in @('host', 'join')) {
        $world = Get-World $role
        if (-not $world -or [int]$world.day -ne 1) {
            throw "$role did not load the pinned fixture at day 1"
        }
        foreach ($player in @(
            @{ id = [string]$Fixture.players.neutralHandle; relation = 'neutral'; human = $false },
            @{ id = [string]$Fixture.players.hostHandle; relation = $(if ($role -eq 'host') { 'self' } else { 'enemy' }); human = $true },
            @{ id = [string]$Fixture.players.joinHandle; relation = $(if ($role -eq 'join') { 'self' } else { 'enemy' }); human = $true }
        )) {
            $matches = @($world.players | Where-Object { [string]$_.id -eq $player.id })
            if ($matches.Count -ne 1 -or
                [string]$matches[0].relation -ne $player.relation -or
                [bool]$matches[0].human -ne [bool]$player.human) {
                $actual = if ($matches.Count -eq 1) {
                    "id=$([string]$matches[0].id) relation=$([string]$matches[0].relation) " +
                        "human=$([bool]$matches[0].human)"
                } else {
                    "matches=$($matches.Count); players=" +
                        ((@($world.players) | ForEach-Object {
                            "$([string]$_.id)/$([string]$_.relation)/$([bool]$_.human)"
                        }) -join ',')
                }
                throw ("$role fixture player fingerprint mismatch for $($player.id): " +
                    "expected relation=$($player.relation) human=$([bool]$player.human); $actual")
            }
        }
        foreach ($side in @('host', 'join')) {
            $spec = $Fixture.$side
            $hero = Get-WorldStackExact $world ([string]$spec.heroId)
            $expectedUnitIds = @(
                [string]$spec.reinforcement.leaderId
                @($spec.reinforcement.unitIds | ForEach-Object { [string]$_ })
            )
            $actualUnitIds = @($hero.unitIds | ForEach-Object { [string]$_ })
            $unitIdsMatch = $actualUnitIds.Count -eq $expectedUnitIds.Count
            if ($unitIdsMatch) {
                for ($unitIndex = 0; $unitIndex -lt $expectedUnitIds.Count; $unitIndex++) {
                    if (-not [string]::Equals($actualUnitIds[$unitIndex],
                            $expectedUnitIds[$unitIndex],
                            [StringComparison]::OrdinalIgnoreCase)) {
                        $unitIdsMatch = $false
                        break
                    }
                }
            }
            if ([int]$hero.x -ne [int]$spec.anchor.x -or
                [int]$hero.y -ne [int]$spec.anchor.y -or
                [string]$hero.owner -ne [string]$Fixture.players."${side}Handle" -or
                [int]$hero.movement -ne 35 -or
                [int]$hero.units -ne [int]$spec.reinforcement.units -or
                [int]$hero.hp -ne [int]$spec.reinforcement.hp -or
                -not $unitIdsMatch -or -not [bool]$hero.inside) {
                throw "$role fixture fingerprint mismatch for $side hero $($spec.heroId)"
            }
            $target = Get-WorldStackExact $world ([string]$spec.target.id)
            if ([int]$target.x -ne [int]$spec.target.x -or
                [int]$target.y -ne [int]$spec.target.y -or
                [string]$target.owner -ne [string]$Fixture.players.neutralHandle -or
                [string]$target.relation -ne 'neutral' -or [bool]$target.inside) {
                throw "$role fixture fingerprint mismatch for $side target $($spec.target.id)"
            }
        }
    }
}


function Get-CanonicalWalkPreparationRole {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$Observation
    )
    if (-not $Observation.Ready -or
        $script:BareMapDialogs -notcontains [string]$Observation.Dialog) {
        throw "$Role saved walk preparation is not a ready bare map"
    }
    $state = $Observation.State
    $appearanceValue = Get-OptionalProperty $state 'dialogAppearance'
    $instanceValue = Get-OptionalProperty $state 'dialogInstance'
    $uiSequenceValue = Get-OptionalProperty $state 'uiSeq'
    $worldSequenceValue = Get-OptionalProperty $state 'worldSeq'
    $allTargets = @((Get-OptionalProperty $state 'targets'))
    $targets = @($allTargets | Where-Object {
        [string]$_.dialog -eq [string]$Observation.Dialog
    })
    # DLG_ISO_PAL and DLG_STRATEGIC may be co-present on the same bare map.
    # Preserve the strategic End Turn owner from this already-consumed state so
    # the next literal phase does not need another UI/state observation.
    $endTurnTargets = @($allTargets | Where-Object {
        [string]$_.dialog -eq 'DLG_STRATEGIC'
    })
    if ($null -eq $appearanceValue -or $null -eq $instanceValue -or
        $null -eq $uiSequenceValue -or $null -eq $worldSequenceValue -or
        $targets.Count -ne 1 -or $endTurnTargets.Count -ne 1) {
        throw "$Role saved walk preparation omitted exact map identity/watermarks"
    }
    [long]$appearance = $appearanceValue
    [long]$instance = $instanceValue
    [long]$owner = $targets[0].instance
    [long]$endTurnOwner = $endTurnTargets[0].instance
    [long]$uiSequence = $uiSequenceValue
    [long]$worldSequence = $worldSequenceValue
    if ($appearance -lt 1 -or $appearance -gt [uint32]::MaxValue -or
        $instance -ne $appearance -or $owner -lt 1 -or
        $owner -gt [uint32]::MaxValue -or $endTurnOwner -lt 1 -or
        $endTurnOwner -gt [uint32]::MaxValue -or $uiSequence -lt 1 -or
        $uiSequence -gt [uint32]::MaxValue -or $worldSequence -lt 0 -or
        $worldSequence -gt [uint32]::MaxValue) {
        throw "$Role saved walk preparation has invalid exact identity/watermarks"
    }
    return [pscustomobject]@{
        role = $Role
        appearance = $appearance
        dialogAppearance = $appearance
        instance = $owner
        endTurnInstance = $endTurnOwner
        uiAfter = $uiSequence
        worldAfter = $worldSequence
    }
}

function ConvertTo-SavedDialogObservation {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$State
    )
    $instanceValue = Get-OptionalProperty $State 'dialogInstance'
    $appearanceValue = Get-OptionalProperty $State 'dialogAppearance'
    $readyValue = Get-OptionalProperty $State 'dialogReady'
    if ($null -eq $instanceValue -or $null -eq $appearanceValue -or
        $readyValue -isnot [bool] -or [long]$instanceValue -lt 1 -or
        [long]$instanceValue -ne [long]$appearanceValue) {
        throw "$Role saved role state omitted exact dialog identity/readiness"
    }
    return [pscustomobject]@{
        State = $State
        Dialog = [string](Get-OptionalProperty $State 'dialog')
        Instance = [long]$instanceValue
        Ready = [bool]$readyValue
    }
}

function New-CanonicalWalkPreparation {
    param(
        [Parameter(Mandatory)][object]$HostObservation,
        [Parameter(Mandatory)][object]$JoinObservation
    )
    # Pure transformation of observations already consumed by the preceding
    # legacy phase. It performs no relay/UI/world/process read.
    return [pscustomobject]@{
        host = Get-CanonicalWalkPreparationRole host $HostObservation
        join = Get-CanonicalWalkPreparationRole join $JoinObservation
    }
}

function New-CanonicalWalkPreparationFromRelayState {
    param(
        [Parameter(Mandatory)][object]$RelayState,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess
    )
    # Pure projection of one aggregate relay publication. The old
    # /api/inject-stack-move route did not require a UI capability; the MSS
    # bridge does. Capture both current map capabilities from the same one-shot
    # state read, fail closed if either role is not on one exact ready map, and
    # never replace this publication after a rejected action.
    $hostState = Get-OptionalProperty $RelayState 'host'
    $joinState = Get-OptionalProperty $RelayState 'join'
    Assert-DebugRelayClientIdentity host $hostState $HostProcess
    Assert-DebugRelayClientIdentity join $joinState $JoinProcess
    return New-CanonicalWalkPreparation `
        (ConvertTo-SavedDialogObservation host $hostState) `
        (ConvertTo-SavedDialogObservation join $joinState)
}

function Get-ObservedWalkPreparationRole {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$State
    )
    # Pure projection needed to address the next one-shot move. Do not assert
    # ready/bare-map/unique-owner/uint32 invariants here: sync_attack PASS has
    # already happened, and those strengthened facts are checked from this same
    # immutable state only at the deferred MSS boundary.
    $appearance = Get-OptionalProperty $State 'dialogAppearance'
    $uiAfter = Get-OptionalProperty $State 'uiSeq'
    $worldAfter = Get-OptionalProperty $State 'worldSeq'
    $dialog = [string](Get-OptionalProperty $State 'dialog')
    $targets = @((Get-OptionalProperty $State 'targets'))
    $currentTargets = @($targets | Where-Object {
        [string]$_.dialog -eq $dialog
    })
    $strategicTargets = @($targets | Where-Object {
        [string]$_.dialog -eq 'DLG_STRATEGIC'
    })
    $mapTarget = if ($currentTargets.Count -gt 0) {
        $currentTargets[0]
    } elseif ($strategicTargets.Count -gt 0) {
        $strategicTargets[0]
    } else {
        $null
    }
    if ($null -eq $appearance -or $null -eq $uiAfter -or
        $null -eq $worldAfter -or -not $mapTarget -or
        $strategicTargets.Count -eq 0) {
        throw "$Role observed terminal UI cannot address the next one-shot map action"
    }
    return [pscustomobject]@{
        role = $Role
        appearance = [long]$appearance
        dialogAppearance = [long]$appearance
        instance = [long]$mapTarget.instance
        endTurnInstance = [long]$strategicTargets[0].instance
        uiAfter = [long]$uiAfter
        worldAfter = [long]$worldAfter
    }
}

function Invoke-PreparedParallelWorldMoves([object[]]$Actions) {
    if ($Actions.Count -ne 2 -or @($Actions.role | Sort-Object -Unique).Count -ne 2) {
        throw 'prepared parallel move barrier requires exactly one host and one join action'
    }
    foreach ($action in $Actions) {
        foreach ($name in @('role', 'id', 'fromX', 'fromY', 'x', 'y', 'appearance', 'instance')) {
            if ($null -eq $action.PSObject.Properties[$name]) {
                throw "prepared parallel move action omitted required '$name'"
            }
        }
    }
    $relayBase = $script:RelayBase
    $responses = @($Actions | ForEach-Object -Parallel {
        $action = $_
        $started = [DateTime]::UtcNow
        $uri = ('{0}/api/ui/move?role={1}&id={2}&fromx={3}&fromy={4}&frommp=-1' +
            '&x={5}&y={6}&appearance={7}&instance={8}') -f `
            $using:relayBase,
            [uri]::EscapeDataString([string]$action.role),
            [uri]::EscapeDataString([string]$action.id),
            [int]$action.fromX,
            [int]$action.fromY,
            [int]$action.x,
            [int]$action.y,
            [long]$action.appearance,
            [long]$action.instance
        try {
            $response = Invoke-RestMethod -Method Post -Uri $uri -TimeoutSec 8
            [pscustomobject]@{
                role = [string]$action.role
                found = [bool]$response.found
                started = $started
                error = ''
            }
        } catch {
            [pscustomobject]@{
                role = [string]$action.role
                found = $false
                started = $started
                error = $_.Exception.Message
            }
        }
    } -ThrottleLimit 2)
    if ($responses.Count -ne 2 -or @($responses | Where-Object { -not $_.found }).Count -ne 0) {
        $details = @($responses | ForEach-Object { "$($_.role):found=$($_.found):$($_.error)" }) -join '; '
        throw "parallel exact move command failed: $details"
    }
    $times = @($responses.started | ForEach-Object { [DateTime]$_ } | Sort-Object)
    return [pscustomobject]@{
        responses = $responses
        dispatchSkewMs = ($times[1] - $times[0]).TotalMilliseconds
    }
}

function Invoke-PreparedParallelAttackMoves([object[]]$Actions,
                                            [switch]$BackToBackLongMove,
                                            [int]$CommandTimeoutMilliseconds = 5000) {
    if ($Actions.Count -ne 2 -or @($Actions.role | Sort-Object -Unique).Count -ne 2) {
        throw 'prepared parallel attack barrier requires exactly one host and one join action'
    }
    if ($CommandTimeoutMilliseconds -lt 1000 -or
        $CommandTimeoutMilliseconds -gt 120000) {
        throw 'paired move command-result timeout is outside 1000..120000 ms'
    }
    $hostActions = @($Actions | Where-Object { [string]$_.role -eq 'host' })
    $joinActions = @($Actions | Where-Object { [string]$_.role -eq 'join' })
    if ($hostActions.Count -ne 1 -or $joinActions.Count -ne 1) {
        throw 'prepared parallel attack barrier lost the exact host/join identity pair'
    }
    foreach ($action in @($hostActions[0], $joinActions[0])) {
        foreach ($name in @('id', 'fromX', 'fromY', 'x', 'y', 'appearance', 'instance')) {
            if ($null -eq $action.PSObject.Properties[$name]) {
                throw "prepared parallel attack omitted required '$name'"
            }
        }
    }

    # BackToBackLongMove matches long_move_sim.ps1's parallel runspace fire
    # without inheriting its swallowed HTTP failures: both role commands are
    # written once before either acknowledgement is awaited, and either UI
    # thread may start first. The default attack adapter retains its separate
    # proved host-authority admission order. Neither mode retries or refires.
    $relayBase = $script:RelayBase
    $hostAction = $hostActions[0]
    $joinAction = $joinActions[0]
    $pairEndpoint = if ($BackToBackLongMove) {
        'long-move-pair'
    } else {
        'move-pair'
    }
    $uri = ('{0}/api/ui/{1}?' +
        'hostid={2}&hostfromx={3}&hostfromy={4}&hostfrommp=-1&hostx={5}&hosty={6}' +
        '&hostappearance={7}&hostinstance={8}' +
        '&joinid={9}&joinfromx={10}&joinfromy={11}&joinfrommp=-1&joinx={12}&joiny={13}' +
        '&joinappearance={14}&joininstance={15}&timeoutMs={16}') -f `
        $relayBase, $pairEndpoint,
        [uri]::EscapeDataString([string]$hostAction.id),
        [int]$hostAction.fromX, [int]$hostAction.fromY,
        [int]$hostAction.x, [int]$hostAction.y,
        [long]$hostAction.appearance, [long]$hostAction.instance,
        [uri]::EscapeDataString([string]$joinAction.id),
        [int]$joinAction.fromX, [int]$joinAction.fromY,
        [int]$joinAction.x, [int]$joinAction.y,
        [long]$joinAction.appearance, [long]$joinAction.instance,
        $CommandTimeoutMilliseconds
    # CommandStarted renews the relay budget, so the HTTP envelope covers both
    # bounded stages plus transport headroom. This remains one POST with no retry.
    $httpTimeoutSeconds =
        [math]::Ceiling((2 * $CommandTimeoutMilliseconds) / 1000.0) + 10
    $response = Invoke-RestMethod -Method Post -Uri $uri `
        -TimeoutSec $httpTimeoutSeconds
    if ($null -eq $response -or $response.found -isnot [bool] -or
        -not [bool]$response.found -or $null -eq $response.host -or
        $null -eq $response.join -or $response.host.found -isnot [bool] -or
        $response.join.found -isnot [bool] -or -not [bool]$response.host.found -or
        -not [bool]$response.join.found) {
        throw 'causal exact attack pair did not complete one host and one join command'
    }
    [double]$dispatchSkewMs = $response.dispatchSkewMs
    [string]$dispatchSkewKind = $response.dispatchSkewKind
    # Only long-move-pair publishes an explicit order because either UI thread
    # may report CommandStarted first.  The host-authority move-pair contract
    # publishes the two startedMs values instead; derive its order below after
    # validating those causal edges.  Reading the absent property under
    # StrictMode made every canonical attack fail before either result could be
    # evaluated.
    [string]$dispatchOrder = if ($BackToBackLongMove) {
        [string]$response.dispatchOrder
    } else {
        ''
    }
    [long]$hostStartedMs = $response.host.startedMs
    [long]$joinStartedMs = $response.join.startedMs
    [long]$hostStartedUiSeq = $response.host.startedUiSeq
    [long]$joinStartedUiSeq = $response.join.startedUiSeq
    [double]$observedStartedDeltaMs = $joinStartedMs - $hostStartedMs
    [double]$observedStartedSkewMs = [Math]::Abs($observedStartedDeltaMs)
    [string]$observedDispatchOrder = if ($joinStartedMs -lt $hostStartedMs) {
        'join-first'
    } elseif ($joinStartedMs -gt $hostStartedMs) {
        'host-first'
    } else {
        'same-millisecond'
    }
    [bool]$baseEdgesValid =
        $hostStartedMs -ge 1 -and $joinStartedMs -ge 1 -and
        $hostStartedUiSeq -ge 1 -and $hostStartedUiSeq -le [uint32]::MaxValue -and
        $joinStartedUiSeq -ge 1 -and $joinStartedUiSeq -le [uint32]::MaxValue
    [bool]$modeEdgesValid = if ($BackToBackLongMove) {
        $dispatchSkewKind -eq 'absolute-relay-command-started-receipt' -and
        $dispatchSkewMs -ge 0 -and
        $dispatchOrder -eq $observedDispatchOrder -and
        [Math]::Abs($dispatchSkewMs - $observedStartedSkewMs) -le 0.001
    } else {
        $dispatchSkewKind -eq 'relay-command-started-receipt' -and
        $observedStartedDeltaMs -ge 0 -and
        $joinStartedUiSeq -ge $hostStartedUiSeq -and
        [Math]::Abs($dispatchSkewMs - $observedStartedDeltaMs) -le 0.001
    }
    if (-not $baseEdgesValid -or -not $modeEdgesValid) {
        throw ("exact $pairEndpoint returned invalid started edges: " +
            "host=$hostStartedMs/ui$hostStartedUiSeq " +
            "join=$joinStartedMs/ui$joinStartedUiSeq skew=$dispatchSkewMs")
    }
    if (-not $BackToBackLongMove) {
        $dispatchOrder = $observedDispatchOrder
    }
    return [pscustomobject]@{
        responses = @(
            [pscustomobject]@{
                role = 'host'
                found = [bool]$response.host.found
                startedMs = $hostStartedMs
                startedUiSeq = $hostStartedUiSeq
            },
            [pscustomobject]@{
                role = 'join'
                found = [bool]$response.join.found
                startedMs = $joinStartedMs
                startedUiSeq = $joinStartedUiSeq
            }
        )
        dispatchSkewMs = $dispatchSkewMs
        dispatchSkewKind = $dispatchSkewKind
        dispatchOrder = $dispatchOrder
    }
}



function Invoke-CanonicalDeploy([object]$Fixture,
                                [System.Diagnostics.Process]$HostProcess,
                                [System.Diagnostics.Process]$JoinProcess,
                                [object]$PreparedBindings = $null) {
    # Literal run_test.ps1 schedule. Do not insert eventual world-evidence waits
    # between these commands: the old green path issued join at host-return+2 s
    # and entered sync_attack at join-return+3 s. The two old pre-fire Mv84
    # snapshots are retained there, and their saved worlds supply the later
    # MSS-only exact-position proof after the legacy attack PASS.
    if ($PreparedBindings) {
        foreach ($role in @('host', 'join')) {
            $prepared = $PreparedBindings.$role
            if (-not $prepared -or [string]$prepared.role -ne $role -or
                [long]$prepared.appearance -lt 1 -or
                [long]$prepared.appearance -gt [uint32]::MaxValue -or
                [long]$prepared.instance -lt 1 -or
                [long]$prepared.instance -gt [uint32]::MaxValue) {
                throw "$role deploy omitted the exact saved map appearance/owner"
            }
        }
    }
    Write-Step 'fixture deploy: one exact host garrison exit, +2 s, one exact join garrison exit, +3 s'
    if (-not (Move-Stack host ([string]$Fixture.host.heroId) `
            ([int]$Fixture.host.garrison.x) ([int]$Fixture.host.garrison.y) `
            ([int]$Fixture.host.deploy.x) ([int]$Fixture.host.deploy.y) `
            $(if ($PreparedBindings) { [long]$PreparedBindings.host.instance } else { 0 }) `
            $(if ($PreparedBindings) { [long]$PreparedBindings.host.appearance } else { 0 }))) {
        throw 'host deploy: the sole move command was not issued'
    }
    Start-Sleep -Seconds 2
    if (-not (Move-Stack join ([string]$Fixture.join.heroId) `
            ([int]$Fixture.join.garrison.x) ([int]$Fixture.join.garrison.y) `
            ([int]$Fixture.join.deploy.x) ([int]$Fixture.join.deploy.y) `
            $(if ($PreparedBindings) { [long]$PreparedBindings.join.instance } else { 0 }) `
            $(if ($PreparedBindings) { [long]$PreparedBindings.join.appearance } else { 0 }))) {
        throw 'join deploy: the sole move command was not issued'
    }
    Start-Sleep -Seconds 3
}













function Read-PrearmedAutoBattleProof {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines,
        [Parameter(Mandatory)][object]$BattleUi
    )
    # Pure parser. The literal resolver and final verdict own every physical
    # client-log read and pass the resulting immutable snapshot here.
    $marker = '[testdrv][auto-battle-proof] '
    $proofLines = @($Lines | Where-Object {
        $_.IndexOf($marker, [StringComparison]::Ordinal) -ge 0
    })
    if ($proofLines.Count -gt 1) {
        throw "$Role native preboot auto-battle published $($proofLines.Count) proofs; expected exactly one"
    }
    if ($proofLines.Count -eq 0) { return $null }

    $line = [string]$proofLines[0]
    $markerAt = $line.IndexOf($marker, [StringComparison]::Ordinal)
    $jsonText = $line.Substring($markerAt + $marker.Length)
    try {
        $proof = $jsonText | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "$Role native preboot auto-battle proof is not complete JSON: $($_.Exception.Message)"
    }

    $requiredFields = @(
        'schema', 'mode', 'role', 'succeeded', 'appearance', 'owner',
        'bindAgeMs', 'callbackCount', 'functorVftable', 'dispatchFunction',
        'memberFunction', 'thisAdjustor', 'controllerGateBefore',
        'kickStateBefore', 'kickStateAfter', 'sideSelector',
        'flag38Before', 'flag38After', 'flag39Before', 'flag39After'
    )
    $propertyNames = @($proof.PSObject.Properties.Name)
    foreach ($field in $requiredFields) {
        if ($propertyNames -cnotcontains $field) {
            throw "$Role native preboot auto-battle proof omitted '$field'"
        }
    }
    foreach ($field in @(
        'schema', 'appearance', 'owner', 'bindAgeMs', 'callbackCount',
        'functorVftable', 'dispatchFunction', 'memberFunction', 'thisAdjustor',
        'controllerGateBefore', 'kickStateBefore', 'kickStateAfter',
        'sideSelector', 'flag38Before', 'flag38After', 'flag39Before',
        'flag39After'
    )) {
        if ($proof.$field -isnot [long]) {
            $actualType = if ($null -eq $proof.$field) {
                '<null>'
            } else {
                $proof.$field.GetType().FullName
            }
            throw "$Role native preboot auto-battle proof field '$field' is $actualType, expected a JSON integer"
        }
    }
    if ($proof.mode -isnot [string] -or $proof.role -isnot [string] -or
        $proof.succeeded -isnot [bool]) {
        throw "$Role native preboot auto-battle proof has non-exact JSON scalar types"
    }

    # The literal resolver enters its fighting phase on the first raw
    # DLG_BATTLE_A name, before the aggregate reporter is necessarily globally
    # ready. Do not add another read/wait here: the later native one-shot proof
    # is the readiness witness and must match this immutable appearance/owner.
    $battleOwners = @($BattleUi.targets | Where-Object {
        [string]$_.dialog -eq 'DLG_BATTLE_A'
    })
    if ([string]$BattleUi.dialog -ne 'DLG_BATTLE_A' -or
        [long]$BattleUi.dialogInstance -lt 1 -or $battleOwners.Count -ne 1) {
        throw "$Role first name-only battle snapshot did not contain one exact owner"
    }
    [long]$expectedAppearance = $BattleUi.dialogInstance
    [long]$expectedOwner = $battleOwners[0].instance
    if ($expectedAppearance -gt [uint32]::MaxValue -or
        $expectedOwner -lt 1 -or $expectedOwner -gt [uint32]::MaxValue) {
        throw "$Role first name-only battle identity is outside uint32"
    }
    $selectedTransition = (
        ([int]$proof.sideSelector -ne 0 -and
            [int]$proof.flag38Before -eq 0 -and
            [int]$proof.flag38After -eq 1 -and
            [int]$proof.flag39After -eq [int]$proof.flag39Before) -or
        ([int]$proof.sideSelector -eq 0 -and
            [int]$proof.flag39Before -eq 0 -and
            [int]$proof.flag39After -eq 1 -and
            [int]$proof.flag38After -eq [int]$proof.flag38Before)
    )
    $proved = [int]$proof.schema -eq 1 -and
        [string]$proof.mode -ceq 'preboot-first-battle' -and
        [string]$proof.role -ceq $Role -and
        [bool]$proof.succeeded -and
        [long]$proof.appearance -eq $expectedAppearance -and
        [long]$proof.owner -eq $expectedOwner -and
        [long]$proof.bindAgeMs -ge 2500 -and
        [int]$proof.callbackCount -eq 1 -and
        [long]$proof.functorVftable -eq 0x006F45D4 -and
        [long]$proof.dispatchFunction -eq 0x00644150 -and
        [long]$proof.memberFunction -eq 0x00635509 -and
        [long]$proof.thisAdjustor -eq 0 -and
        [int]$proof.controllerGateBefore -eq 0 -and
        [int]$proof.kickStateBefore -eq 0 -and
        [int]$proof.kickStateAfter -eq 1 -and
        $selectedTransition
    if (-not $proved) {
        throw ("$Role native preboot auto-battle proof violated the exact Russobit callback invariant: " +
            ($proof | ConvertTo-Json -Compress -Depth 4))
    }

    Write-Step ("$Role preboot auto-battle proof PASS: appearance={0} owner={1} bindAge={2}ms callbackCount=1" -f
        $expectedAppearance, $expectedOwner, [long]$proof.bindAgeMs)
    return [pscustomobject]@{
        role = $Role
        started = $null
        found = $true
        kick = $proof
        error = ''
        source = 'preboot-first-battle'
    }
}

function Get-ObservedPrearmedAutoBattleRecord {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines
    )
    # Tolerant, observation-only projection used by the legacy result. It
    # packages the actual JSON already present in the lifecycle's physical log
    # read, but deliberately makes no schema/callback/owner assertion. The exact
    # Read-PrearmedAutoBattleProof validator runs at the deferred MSS boundary.
    $marker = '[testdrv][auto-battle-proof] '
    $proofLines = @($Lines | Where-Object {
        $_.IndexOf($marker, [StringComparison]::Ordinal) -ge 0
    })
    $kick = $null
    $error = ''
    if ($proofLines.Count -eq 0) {
        $error = 'no observed auto-battle proof record'
    } else {
        $line = [string]$proofLines[-1]
        $markerAt = $line.IndexOf($marker, [StringComparison]::Ordinal)
        try {
            $kick = $line.Substring($markerAt + $marker.Length) |
                ConvertFrom-Json -ErrorAction Stop
        } catch {
            $error = "observed auto-battle JSON was malformed: $($_.Exception.Message)"
        }
        if ($proofLines.Count -ne 1) {
            $duplicate = "observed $($proofLines.Count) auto-battle proof records"
            $error = if ($error) { "$duplicate; $error" } else { $duplicate }
        }
    }
    return [pscustomobject]@{
        role = $Role
        started = $null
        found = $null -ne $kick
        kick = $kick
        error = $error
        source = 'observed-lifecycle-log'
        proofLineCount = [int]$proofLines.Count
    }
}


function ConvertFrom-ScriptedBattleCloseMarker {
    param(
        [Parameter(Mandatory)][string]$Line,
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role
    )
    $prefix = '[testdrv][scripted-popup] '
    $at = $Line.IndexOf($prefix, [StringComparison]::Ordinal)
    if ($at -lt 0) { return $null }
    $payload = $Line.Substring($at + $prefix.Length)
    $common = ('role={0} dialog=DLG_BATTLE_A appearance=(?<appearance>\d+) ' +
        'owner=(?<owner>\d+) button=BTN_CLOSE ') -f [regex]::Escape($Role)
    $observed = [regex]::Match($payload,
        ('^OBSERVED ' + $common + 'tick=(?<tick>\d+)$'),
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if ($observed.Success) {
        return [pscustomobject]@{
            kind = 'OBSERVED'
            appearance = [long]$observed.Groups['appearance'].Value
            owner = [long]$observed.Groups['owner'].Value
            bindAgeMs = $null
            tick = [long]$observed.Groups['tick'].Value
        }
    }
    $claimed = [regex]::Match($payload,
        ('^CLAIMED ' + $common + 'bindAgeMs=(?<age>\d+) tick=(?<tick>\d+)$'),
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if ($claimed.Success) {
        return [pscustomobject]@{
            kind = 'CLAIMED'
            appearance = [long]$claimed.Groups['appearance'].Value
            owner = [long]$claimed.Groups['owner'].Value
            bindAgeMs = [long]$claimed.Groups['age'].Value
            tick = [long]$claimed.Groups['tick'].Value
        }
    }
    $committed = [regex]::Match($payload,
        ('^COMMITTED ' + $common + 'tick=(?<tick>\d+)$'),
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if ($committed.Success) {
        return [pscustomobject]@{
            kind = 'COMMITTED'
            appearance = [long]$committed.Groups['appearance'].Value
            owner = [long]$committed.Groups['owner'].Value
            bindAgeMs = $null
            tick = [long]$committed.Groups['tick'].Value
        }
    }
    return $null
}

function Assert-ScriptedBattleCloseProof {
    param(
        [Parameter(Mandatory)][object]$State,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines
    )
    $role = [string]$State.Role
    if ([string]$Baseline.role -ne $role -or [long]$Baseline.lineCount -lt 0) {
        throw "$role scripted battle-close baseline has inconsistent identity"
    }
    # Pure parser. Keeping I/O at the caller makes the old up-read then
    # close-read cardinality and role ordering statically auditable.
    if ($Lines.Count -lt [long]$Baseline.lineCount) {
        throw "$role scripted battle-close log regressed below its pre-battle baseline"
    }
    $newLines = @($Lines | Select-Object -Skip ([int]$Baseline.lineCount))
    $candidateLines = @($newLines | Where-Object {
        $_.IndexOf('[testdrv][scripted-popup] ', [StringComparison]::Ordinal) -ge 0 -and
        $_.IndexOf('dialog=DLG_BATTLE_A', [StringComparison]::Ordinal) -ge 0
    })
    $markers = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $candidateLines) {
        $marker = ConvertFrom-ScriptedBattleCloseMarker -Line $line -Role $role
        if (-not $marker) {
            throw "$role scripted battle-close evidence contains a malformed marker: $line"
        }
        $markers.Add($marker)
    }
    if ($markers.Count -ne 3 -or
        [string]$markers[0].kind -ne 'OBSERVED' -or
        [string]$markers[1].kind -ne 'CLAIMED' -or
        [string]$markers[2].kind -ne 'COMMITTED') {
        throw ("$role scripted battle-close evidence is not one exact " +
            "OBSERVED->CLAIMED->COMMITTED chain")
    }
    foreach ($marker in $markers) {
        if ([long]$marker.appearance -ne [long]$State.LiveBattleAppearance -or
            [long]$marker.owner -ne [long]$State.LiveBattleOwner) {
            throw ("$role scripted battle-close marker identity changed: " +
                "$($marker.appearance)/$($marker.owner), expected " +
                "$($State.LiveBattleAppearance)/$($State.LiveBattleOwner)")
        }
    }
    if ([long]$markers[1].bindAgeMs -lt 300 -or
        [long]$markers[0].tick -gt [long]$markers[1].tick -or
        [long]$markers[1].tick -gt [long]$markers[2].tick) {
        throw "$role scripted battle-close chain violated age/tick ordering"
    }
    Write-Step (("$role native battle-close proof PASS: appearance={0} owner={1} " +
        "bindAgeMs={2} callbackCount=1") -f
        [long]$markers[1].appearance, [long]$markers[1].owner,
        [long]$markers[1].bindAgeMs)
    return [pscustomobject]@{
        role = $role
        appearance = [long]$markers[1].appearance
        owner = [long]$markers[1].owner
        bindAgeMs = [long]$markers[1].bindAgeMs
        callbackCount = 1
        observedTick = [long]$markers[0].tick
        claimedTick = [long]$markers[1].tick
        committedTick = [long]$markers[2].tick
    }
}

function Assert-LegacyBattleUpSnapshot {
    param(
        [Parameter(Mandatory)][object]$State,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines
    )
    $role = [string]$State.Role
    if ([string]$Baseline.role -ne $role -or [long]$Baseline.lineCount -lt 0 -or
        $Lines.Count -lt [long]$Baseline.lineCount) {
        throw "$role battle-up snapshot regressed below its exact pre-battle baseline"
    }
    # The old first Battle-Closed scan looked for the live battle-up marker.
    # MSS's stable live-battle equivalent is the exact preboot auto-battle proof;
    # scripted-popup OBSERVED is intentionally later (result BTN_CLOSE).
    $proof = Read-PrearmedAutoBattleProof `
        -Role $role -Lines $Lines -BattleUi $State.BattleUi
    if ($null -eq $proof) {
        throw "$role battle-up snapshot contains no exact live-battle auto proof"
    }
    return $proof
}

function Test-LegacyBattleCloseCommitted {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines
    )
    if ($Lines.Count -lt [long]$Baseline.lineCount) {
        throw "$Role battle-close snapshot regressed below its exact pre-battle baseline"
    }
    $committed = @($Lines | Select-Object -Skip ([int]$Baseline.lineCount) |
        Where-Object {
            $_.IndexOf('[testdrv][scripted-popup] COMMITTED ', [StringComparison]::Ordinal) -ge 0 -and
            $_.IndexOf("role=$Role ", [StringComparison]::Ordinal) -ge 0 -and
            $_.IndexOf('dialog=DLG_BATTLE_A', [StringComparison]::Ordinal) -ge 0
        })
    if ($committed.Count -gt 1) {
        throw "$Role battle-close snapshot contains $($committed.Count) COMMITTED markers; expected at most one"
    }
    return $committed.Count -eq 1
}

function Get-LegacyBattleClosedSnapshot {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$BattleUpLines,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$BattleCloseLines
    )
    if ($BattleUpLines.Count -lt [long]$Baseline.lineCount -or
        $BattleCloseLines.Count -lt [long]$Baseline.lineCount) {
        throw "$Role legacy Battle-Closed snapshot regressed below its pre-battle baseline"
    }

    # Literal equivalent of the source's two independent Select-String calls:
    # first the latest "battle UI up", then the latest "[battle-close]".
    # Merely locating these generic lifecycle markers does not validate the MSS
    # callback JSON, exact owner, callback count, or terminal bare-map identity.
    $upPattern = '\[testdrv\] preboot auto-battle captured first DLG_BATTLE_A bind '
    $closePattern = ('\[battle-close\] role={0} .* BTN_CLOSE committed' -f
        [regex]::Escape($Role))
    [long]$upLine = 0
    for ($lineIndex = [int]$Baseline.lineCount;
         $lineIndex -lt $BattleUpLines.Count;
         $lineIndex++) {
        if ([string]$BattleUpLines[$lineIndex] -match $upPattern) {
            $upLine = [long]$lineIndex + 1
        }
    }
    [long]$closeLine = 0
    for ($lineIndex = [int]$Baseline.lineCount;
         $lineIndex -lt $BattleCloseLines.Count;
         $lineIndex++) {
        if ([string]$BattleCloseLines[$lineIndex] -match $closePattern) {
            $closeLine = [long]$lineIndex + 1
        }
    }
    return [pscustomobject]@{
        role = $Role
        battleUpLine = $upLine
        battleCloseLine = $closeLine
        closed = [bool]($closeLine -gt 0 -and
            ($upLine -eq 0 -or $closeLine -gt $upLine))
    }
}

function Invoke-CanonicalBattleLifecyclesIndependently {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][hashtable]$AfterUi,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess,
        [Parameter(Mandatory)][string]$HostLog,
        [Parameter(Mandatory)][string]$JoinLog,
        [Parameter(Mandatory)][hashtable]$BattleCloseBaselines,
        [int]$TimeoutSec = 150
    )
    $configs = @{
        host = [pscustomobject]@{ spec = $Fixture.host; log = $HostLog }
        join = [pscustomobject]@{ spec = $Fixture.join; log = $JoinLog }
    }
    $peers = @(
        [pscustomobject]@{ role = 'host'; phase = 'moving'; steps = 0; state = $null },
        [pscustomobject]@{ role = 'join'; phase = 'moving'; steps = 0; state = $null }
    )
    $maxSteps = 6
    $results = @{}
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    # Literal sync_attack resolver. Every unfinished role gets one raw current
    # UI read first (GetLastDlg equivalent). A still-moving role then gets the
    # old target census and, only while that target exists, the old hero census.
    # A fighting role gets exactly two independent full client-log snapshots:
    # battle-up first, battle-close second. A partial route retains the old
    # maximum of six separately addressed continuation legs; every leg is one
    # new command from its newly observed source tile. The native subscriber
    # remains the sole battle/close action owner.
    while ($results.Count -lt 2 -and [DateTime]::UtcNow -lt $deadline) {
        $iterationFailure = $null
        try {
            foreach ($peer in $peers) {
                if ([string]$peer.phase -eq 'done') { continue }
                $role = [string]$peer.role
                $config = $configs[$role]

                # Exactly one raw GetLastDlg-equivalent read for this unfinished
                # role at the head of this two-second iteration.
                $rawUi = Get-GameUiSnapshot $role
                if ([string]$peer.phase -eq 'moving') {
                    if ([string]$rawUi.dialog -eq 'DLG_BATTLE_A') {
                        # The source transition was name-only. Save the complete
                        # MSS observation for deferred exact validation, but do
                        # not let readiness/owner/callback schema replace it.
                        $peer.state = [pscustomobject]@{
                            Role = $role
                            Spec = $config.spec
                            BattleUi = $rawUi
                            BattleClosed = $false
                            BattleClosedSnapshot = $null
                            BattleUpLines = @()
                            BattleCloseLines = @()
                        }
                        $peer.phase = 'fighting'
                        Write-Step "$role canonical battle STARTED (DLG_BATTLE_A)"
                        continue
                    }

                    # Old Get-Stack(target), then Get-Stack(hero), from the one
                    # relay-global census source. The hero read is conditional on
                    # the target still existing, exactly like sync_attack.ps1.
                    $targetWorld = Get-LegacyStackSnapshot
                    $target = Get-WorldStackExact $targetWorld `
                        ([string]$config.spec.target.id) -AllowMissing
                    if (-not $target) {
                        $peer.state = [pscustomobject]@{
                            Role = $role
                            Spec = $config.spec
                            BattleUi = $null
                            BattleClosed = $false
                            BattleClosedSnapshot = $null
                            BattleUpLines = @()
                            BattleCloseLines = @()
                        }
                        $peer.phase = 'done'
                        $results[$role] = $peer.state
                        Write-Step "$role target GONE without a tracked battle"
                        continue
                    }
                    $heroWorld = Get-LegacyStackSnapshot
                    $hero = Get-WorldStackExact $heroWorld `
                        ([string]$config.spec.heroId) -AllowMissing
                    if (-not $hero) {
                        $iterationFailure = "$role hero disappeared before its tracked battle"
                        continue
                    }
                    [int]$distance = [Math]::Max(
                        [Math]::Abs([int]$hero.x - [int]$target.x),
                        [Math]::Abs([int]$hero.y - [int]$target.y))
                    if ($distance -le 1) {
                        Write-Step ("$role adjacent to target (dist=$distance); " +
                            'waiting for native battle bind, no redundant engaged-stack action')
                        continue
                    }
                    if ([int]$peer.steps -lt $maxSteps) {
                        $peer.steps = [int]$peer.steps + 1
                        $binding = Get-ObservedWalkPreparationRole $role $rawUi
                        Write-Step ("$role continuation step $($peer.steps): " +
                            "($($hero.x),$($hero.y)) -> ($($target.x),$($target.y)) " +
                            "dist=$distance movement=$($hero.movement)")
                        if (-not (Move-Stack $role ([string]$config.spec.heroId) `
                                ([int]$hero.x) ([int]$hero.y) `
                                ([int]$target.x) ([int]$target.y) `
                                ([long]$binding.instance) ([long]$binding.appearance))) {
                            $iterationFailure = "$role continuation step $($peer.steps) was not issued"
                        }
                        continue
                    }
                    $peer.state = [pscustomobject]@{
                        Role = $role
                        Spec = $config.spec
                        BattleUi = $null
                        BattleClosed = $false
                        BattleClosedSnapshot = $null
                        BattleUpLines = @()
                        BattleCloseLines = @()
                    }
                    $peer.phase = 'done'
                    $results[$role] = $peer.state
                    Write-Step "$role gave up after $maxSteps continuation steps (dist=$distance)"
                    continue
                }

                # Fighting form: raw GetLastDlg above, then exactly two raw log
                # reads in battle-up -> battle-close order for this role.
                $battleUpLines = @(Read-ClientLogLines ([string]$config.log))
                $battleCloseLines = @(Read-ClientLogLines ([string]$config.log))
                $closed = Get-LegacyBattleClosedSnapshot `
                    -Role $role -Baseline $BattleCloseBaselines[$role] `
                    -BattleUpLines $battleUpLines `
                    -BattleCloseLines $battleCloseLines
                $peer.state.BattleUpLines = @($battleUpLines)
                $peer.state.BattleCloseLines = @($battleCloseLines)
                $peer.state.BattleClosedSnapshot = $closed
                if ([bool]$closed.closed) {
                    $peer.state.BattleClosed = $true
                    $peer.phase = 'done'
                    $results[$role] = $peer.state
                    Write-Step "$role canonical battle CLOSED ([battle-close] after battle-up)"
                }
            }
        } catch {
            $iterationFailure = $_.Exception.Message
        } finally {
            # Unconditional for every complete resolver iteration, including the
            # terminal one and an iteration which exposes a terminal mismatch.
            Start-Sleep -Seconds 2
        }
        if ($iterationFailure) { throw $iterationFailure }
    }

    # The source did not throw at its resolver deadline. It proceeded through
    # the post-loop settle and let the final Battle-Closed predicates fail.
    foreach ($peer in $peers) {
        $role = [string]$peer.role
        if ($results.ContainsKey($role)) { continue }
        if (-not $peer.state) {
            $peer.state = [pscustomobject]@{
                Role = $role
                Spec = $configs[$role].spec
                BattleUi = $null
                BattleClosed = $false
                BattleClosedSnapshot = $null
                BattleUpLines = @()
                BattleCloseLines = @()
            }
        }
        $results[$role] = $peer.state
    }

    # Separate legacy post-loop settle, after the terminal iteration's own +2.
    Start-Sleep -Seconds 2

    return [pscustomobject]@{
        host = $results.host
        join = $results.join
    }
}

function Get-LegacySequentialSharedMovementSnapshot([object]$Fixture) {
    # Legacy Mv84(host), Mv84(join) called GET /api/stacks twice. That endpoint
    # exposed the host's latest raw CMidStack::Stream census. Preserve exactly
    # two sequential reads from that removable source; /api/world is a distinct
    # client object-map reporter and is not a fallback for this oracle.
    $hostCensus = Get-LegacyStackSnapshot
    if ([long]$hostCensus.sequence -lt 1) {
        throw 'legacy host stack census was not published before its first Mv84 read'
    }
    $hostHero = Get-WorldStackExact $hostCensus ([string]$Fixture.host.heroId)
    $joinCensus = Get-LegacyStackSnapshot
    if ([long]$joinCensus.sequence -lt 1) {
        throw 'legacy host stack census was not published before its second Mv84 read'
    }
    $joinHero = Get-WorldStackExact $joinCensus ([string]$Fixture.join.heroId)
    return [pscustomobject]@{
        host = [pscustomobject]@{
            id = [string]$hostHero.id
            owner = [string]$hostHero.owner
            x = [int]$hostHero.x
            y = [int]$hostHero.y
            movement = [int]$hostHero.movement
        }
        join = [pscustomobject]@{
            id = [string]$joinHero.id
            owner = [string]$joinHero.owner
            x = [int]$joinHero.x
            y = [int]$joinHero.y
            movement = [int]$joinHero.movement
        }
        samples = @($hostCensus, $joinCensus)
    }
}

function Test-LegacyRoleHeroSnapshotState([object]$Snapshot,
                                          [object]$Fixture,
                                          [ValidateSet('host', 'join')]
                                          [string]$Role,
                                          [object]$Expected) {
    if (-not $Snapshot -or
        [string](Get-OptionalProperty $Snapshot 'sourceRole') -ne 'host' -or
        [long](Get-OptionalProperty $Snapshot 'sequence') -lt 1) {
        return $false
    }
    $spec = if ($Role -eq 'host') { $Fixture.host } else { $Fixture.join }
    $owner = if ($Role -eq 'host') {
        [string]$Fixture.players.hostHandle
    } else {
        [string]$Fixture.players.joinHandle
    }
    $hero = Get-WorldStackExact `
        $Snapshot ([string]$spec.heroId) -AllowMissing
    if (-not $hero -or
        [string]$hero.owner -ne $owner -or
        [int]$hero.x -ne [int]$Expected.x -or
        [int]$hero.y -ne [int]$Expected.y) {
        return $false
    }
    $movement = $Expected.PSObject.Properties['movement']
    if ($movement -and [int]$hero.movement -ne [int]$movement.Value) {
        return $false
    }
    return $true
}

function Test-LegacyHeroSnapshotState([object]$Snapshot,
                                      [object]$Fixture,
                                      [object]$HostExpected,
                                      [object]$JoinExpected) {
    return (Test-LegacyRoleHeroSnapshotState `
            $Snapshot $Fixture host $HostExpected) -and
        (Test-LegacyRoleHeroSnapshotState `
            $Snapshot $Fixture join $JoinExpected)
}

function Test-LegacyStackSnapshotState([object]$Snapshot,
                                       [object]$Fixture,
                                       [object]$HostExpected,
                                       [object]$JoinExpected,
                                       [bool]$HostTargetPresent,
                                       [bool]$JoinTargetPresent) {
    if (-not (Test-LegacyHeroSnapshotState `
            $Snapshot $Fixture $HostExpected $JoinExpected)) {
        return $false
    }
    foreach ($entry in @(
        [pscustomobject]@{
            spec = $Fixture.host.target
            present = $HostTargetPresent
        },
        [pscustomobject]@{
            spec = $Fixture.join.target
            present = $JoinTargetPresent
        }
    )) {
        $target = Get-WorldStackExact `
            $Snapshot ([string]$entry.spec.id) -AllowMissing
        if (-not [bool]$entry.present) {
            if ($target) { return $false }
            continue
        }
        if (-not $target -or
            [string]$target.owner -ne [string]$Fixture.players.neutralHandle -or
            [int]$target.x -ne [int]$entry.spec.x -or
            [int]$target.y -ne [int]$entry.spec.y) {
            return $false
        }
    }
    return $true
}

function Get-LegacySharedAttackPlan([object]$Fixture,
                                    [object]$HostHeroWorld,
                                    [object]$JoinHeroWorld,
                                    [object]$TargetWorld) {
    # sync_attack.ps1 resolved host id, join id and the target plan from three
    # separate relay-global snapshots. Reconstruct its exact low-16 role labels,
    # while its NearN origins remain the already-known deploy
    # coordinates passed into the source script.
    $heroes = @{}
    foreach ($side in @('host', 'join')) {
        $stackRole = if ($side -eq 'host') { 'host' } else { 'joiner' }
        $heroWorld = if ($side -eq 'host') { $HostHeroWorld } else { $JoinHeroWorld }
        $owned = @($heroWorld.stacks | Where-Object {
            (Get-LegacyStackRole $_) -eq $stackRole
        })
        if ($owned.Count -lt 1) {
            throw "legacy attack census has no $side-owned hero"
        }
        $heroes[$side] = $owned[0]
    }

    $neutral = @($TargetWorld.stacks | Where-Object {
        (Get-LegacyStackRole $_) -eq 'neutral'
    })
    if ($neutral.Count -eq 0) {
        throw 'legacy attack census has no neutral target'
    }

    $plan = [ordered]@{}
    foreach ($side in @('host', 'join')) {
        $hero = $heroes[$side]
        [int]$fromX = $Fixture.$side.deploy.x
        [int]$fromY = $Fixture.$side.deploy.y
        $nearest = @($neutral | ForEach-Object {
            [pscustomobject]@{
                id = [string]$_.id
                x = [int]$_.x
                y = [int]$_.y
                distance = [Math]::Max(
                    [Math]::Abs([int]$_.x - $fromX),
                    [Math]::Abs([int]$_.y - $fromY))
            }
        } | Sort-Object distance | Select-Object -First 1)
        if ($nearest.Count -ne 1) {
            throw "legacy attack census could not choose the nearest neutral for $side"
        }
        $plan[$side] = [pscustomobject]@{
            role = $side
            id = [string]$hero.id
            fromX = $fromX
            fromY = $fromY
            target = $nearest[0]
        }
    }
    return [pscustomobject]$plan
}

function Assert-LegacySharedAttackPlanMatchesFixture([object]$Plan, [object]$Fixture) {
    foreach ($side in @('host', 'join')) {
        $actual = $Plan.$side
        $expected = $Fixture.$side
        if ([string]$actual.id -ne [string]$expected.heroId -or
            [int]$actual.fromX -ne [int]$expected.deploy.x -or
            [int]$actual.fromY -ne [int]$expected.deploy.y -or
            [string]$actual.target.id -ne [string]$expected.target.id -or
            [int]$actual.target.x -ne [int]$expected.target.x -or
            [int]$actual.target.y -ne [int]$expected.target.y) {
            throw ("legacy shared attack plan for $side did not match the pinned fixture: " +
                "hero=$($actual.id) from=($($actual.fromX),$($actual.fromY)) " +
                "target=$($actual.target.id)@($($actual.target.x),$($actual.target.y))")
        }
    }
}

function Get-LegacySharedWalkPlan([object]$Fixture,
                                  [ValidateSet('postBattleWalk', 'day2Reverse')][string]$Phase,
                                  [object]$World) {
    $back = $Phase -eq 'day2Reverse'
    $deltas = @(
        @(1, 0), @(0, 1), @(1, 1), @(-1, 0),
        @(0, -1), @(-1, -1), @(1, -1), @(-1, 1)
    )
    $plan = [ordered]@{}
    foreach ($side in @('host', 'join')) {
        $stackRole = if ($side -eq 'host') { 'host' } else { 'joiner' }
        $heroes = @($World.stacks | Where-Object {
            (Get-LegacyStackRole $_) -eq $stackRole
        })
        if ($heroes.Count -lt 1) {
            throw "$Phase shared census has no $side-owned hero"
        }
        $hero = $heroes[0]
        [int]$hx = $hero.x
        [int]$hy = $hero.y
        [int]$garrisonX = $Fixture.$side.garrison.x
        [int]$garrisonY = $Fixture.$side.garrison.y
        $candidates = @($deltas | ForEach-Object {
            [int]$x = $hx + [int]$_[0]
            [int]$y = $hy + [int]$_[1]
            if ($x -eq $garrisonX -and $y -eq $garrisonY) { return }
            if (@($World.stacks | Where-Object {
                    [int]$_.x -eq $x -and [int]$_.y -eq $y
                }).Count -ne 0) { return }
            [pscustomobject]@{
                x = $x
                y = $y
                dGar = [Math]::Max(
                    [Math]::Abs($x - $garrisonX),
                    [Math]::Abs($y - $garrisonY))
            }
        })
        $candidates = if ($back) {
            @($candidates | Sort-Object dGar)
        } else {
            @($candidates | Sort-Object dGar -Descending)
        }
        if ($candidates.Count -eq 0) {
            throw "$Phase shared census has no free adjacent tile for $side"
        }
        $plan[$side] = [pscustomobject]@{
            role = $side
            id = [string]$hero.id
            fromX = $hx
            fromY = $hy
            movementBefore = [int]$hero.movement
            target = $candidates[0]
            candidates = $candidates
        }
    }
    return [pscustomobject]$plan
}

function Assert-LegacySharedWalkPlanMatchesFixture([object]$Plan,
                                                   [object]$Fixture,
                                                   [ValidateSet('postBattleWalk', 'day2Reverse')][string]$Phase) {
    foreach ($side in @('host', 'join')) {
        $expectedFrom = if ($Phase -eq 'postBattleWalk') {
            $Fixture.$side.battleEnd
        } else {
            [pscustomobject]@{
                x = [int]$Fixture.$side.postBattleWalk.x
                y = [int]$Fixture.$side.postBattleWalk.y
                movement = 35
            }
        }
        $expectedTarget = $Fixture.$side.$Phase
        $actual = $Plan.$side
        if ([string]$actual.id -ne [string]$Fixture.$side.heroId -or
            [int]$actual.fromX -ne [int]$expectedFrom.x -or
            [int]$actual.fromY -ne [int]$expectedFrom.y -or
            [int]$actual.movementBefore -ne [int]$expectedFrom.movement -or
            [int]$actual.target.x -ne [int]$expectedTarget.x -or
            [int]$actual.target.y -ne [int]$expectedTarget.y) {
            throw ("$Phase legacy plan for $side did not match the pinned fixture: " +
                "hero=$($actual.id) from=($($actual.fromX),$($actual.fromY)) " +
                "MP=$($actual.movementBefore) first=($($actual.target.x),$($actual.target.y))")
        }
    }
}

function Assert-CanonicalPreFireSnapshot([object]$Fixture) {
    $vitals = Get-LegacySequentialSharedMovementSnapshot $Fixture
    # sync_attack.ps1 printed these two values but did not gate on them.  Keep
    # the exact two-read snapshot as observation only; the pinned 35/35 fixture
    # assertion belongs to the deferred MSS strengthening.
    Write-Step ("canonical attacks: legacy pre-fire display MP={0}/{1} after quiet-3" -f
        $vitals.host.movement, $vitals.join.movement)
    return $vitals
}

function Assert-CanonicalPostFireChargeSnapshot([object]$Fixture) {
    $vitals = Get-LegacySequentialSharedMovementSnapshot $Fixture
    # mass_test::Analyze later applied this broader billing predicate. The
    # phase script itself only printed the values, so retain the boolean as
    # structured actual evidence and never gate ATTACK PASS on it here.
    $legacyCharged = [int]$vitals.host.movement -gt 0 -and
        [int]$vitals.host.movement -lt 35 -and
        [int]$vitals.join.movement -gt 0 -and
        [int]$vitals.join.movement -lt 35
    $vitals | Add-Member -NotePropertyName legacyRangePass `
        -NotePropertyValue ([bool]$legacyCharged)
    Write-Step ("canonical attacks: legacy +1500ms MP={0}/{1}; mass range 0<MP<35 => {2}" -f
        $vitals.host.movement, $vitals.join.movement, $legacyCharged)
    return $vitals
}


function Complete-CanonicalBattle {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][object]$Spec,
        [Parameter(Mandatory)][object]$BattleUi,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess,
        [switch]$AutoBattleAlreadyEnabled,
        [int]$TimeoutSec = 210
    )
    if (-not $AutoBattleAlreadyEnabled) {
        throw "$Role battle completion requires a causally proved auto-battle enable"
    }

    $state = New-CanonicalBattleCompletionState $Role $Spec $BattleUi
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline -and -not $state.Done) {
        Assert-ClientsLive $HostProcess $JoinProcess
        Step-CanonicalBattleCompletionState $state
        Start-Sleep -Milliseconds 100
    }
    if (-not $state.Done) {
        throw "$Role battle did not reach the exact live/result/closed world state"
    }
    return Get-CanonicalBattleResult $Role $Spec
}

function New-CanonicalBattleCompletionState {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$Spec,
        [Parameter(Mandatory)][object]$BattleUi,
        [switch]$LegacyCloseEvidenceOnly
    )
    if ([string]$BattleUi.dialog -ne 'DLG_BATTLE_A' -or
        [long]$BattleUi.dialogInstance -lt 1) {
        throw "$Role battle completion has no exact battle-start appearance"
    }
    $appearanceValue = Get-OptionalProperty $BattleUi 'dialogAppearance'
    $sequenceValue = Get-OptionalProperty $BattleUi 'seq'
    if ($null -eq $sequenceValue) {
        # A raw /api/ui snapshot carries uiSeq; append-only history events carry
        # seq. Both are the same native publication sequence.
        $sequenceValue = Get-OptionalProperty $BattleUi 'uiSeq'
    }
    if ($null -eq $appearanceValue -or
        [long]$appearanceValue -ne [long]$BattleUi.dialogInstance -or
        $null -eq $sequenceValue -or [long]$sequenceValue -lt 1 -or
        [long]$sequenceValue -gt [uint32]::MaxValue) {
        throw "$Role battle-start evidence has inconsistent appearance/sequence identity"
    }
    $liveTargets = @((Get-OptionalProperty $BattleUi 'targets') | Where-Object {
        [string](Get-OptionalProperty $_ 'dialog') -eq 'DLG_BATTLE_A'
    })
    if ($liveTargets.Count -ne 1) {
        throw "$Role battle-start evidence contains $($liveTargets.Count) native battle owners"
    }
    [long]$liveOwner = Get-OptionalProperty $liveTargets[0] 'instance'
    if ($liveOwner -lt 1 -or $liveOwner -gt [uint32]::MaxValue) {
        throw "$Role live-battle owner token is outside uint32"
    }
    return [pscustomobject]@{
        Role = $Role
        Spec = $Spec
        # The legacy harness had two separate causal gates: "battle UI up" and
        # a later [battle-close] emitted only when the post-fight BTN_CLOSE
        # existed. Both phases belong to the same CBattleViewerInterf: the engine
        # tears down the live controls and rebinds result controls on that owner.
        # Preserve identity and distinguish the phases by append-only UI evidence.
        LiveBattleAppearance = [long]$BattleUi.dialogInstance
        LiveBattleOwner = $liveOwner
        BattleUi = $BattleUi
        UiCursor = [long]$sequenceValue
        ResultUiSequence = [long]0
        Consumed = @{}
        PostBattleStable = @{}
        BattleCloseObserved = $false
        BattleUpProof = $null
        AutoBattleProof = $null
        BattleCloseProof = $null
        TerminalMapObservation = $null
        LegacyCloseEvidenceOnly = [bool]$LegacyCloseEvidenceOnly
        Done = $false
    }
}

function Get-CanonicalBattleResultControlState {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object[]]$Widgets,
        [Parameter(Mandatory)][bool]$Ready
    )
    if (-not $Ready) {
        return [pscustomobject]@{ IsResult = $false; CloseEnabled = $false }
    }

    $closeByName = @($Widgets | Where-Object {
        [string](Get-OptionalProperty $_ 'name') -eq 'BTN_CLOSE'
    })
    if ($closeByName.Count -eq 0) {
        return [pscustomobject]@{ IsResult = $false; CloseEnabled = $false }
    }
    if ($closeByName.Count -ne 1 -or
        [string](Get-OptionalProperty $closeByName[0] 'type') -ne 'button') {
        throw "$Role DLG_BATTLE_A result does not expose one typed BTN_CLOSE"
    }

    $liveControlNames = @(
        'BTN_DEFEND', 'BTN_RETREAT', 'BTN_WAIT', 'BTN_RESOLVE', 'TOG_AUTOBATTLE'
    )
    $liveControls = @($Widgets | Where-Object {
        $liveControlNames -contains [string](Get-OptionalProperty $_ 'name')
    })
    if ($liveControls.Count -ne 0) {
        throw "$Role ready battle result retained $($liveControls.Count) live-battle controls"
    }

    $buttonState = Get-OptionalProperty $closeByName[0] 'state'
    $enabledProperty = if ($buttonState) {
        $buttonState.PSObject.Properties['enabled']
    } else {
        $null
    }
    if ($null -eq $enabledProperty -or $enabledProperty.Value -isnot [bool]) {
        throw "$Role DLG_BATTLE_A result BTN_CLOSE omitted typed enabled state"
    }
    return [pscustomobject]@{
        IsResult = $true
        CloseEnabled = [bool]$enabledProperty.Value
    }
}

function Assert-CanonicalBattleUiEvent {
    param(
        [Parameter(Mandatory)][object]$State,
        [Parameter(Mandatory)][object]$Event
    )
    $role = [string]$State.Role
    $sequenceValue = Get-OptionalProperty $Event 'seq'
    $appearanceValue = Get-OptionalProperty $Event 'dialogInstance'
    $appearanceAlias = Get-OptionalProperty $Event 'dialogAppearance'
    $readyValue = Get-OptionalProperty $Event 'dialogReady'
    if ($null -eq $sequenceValue -or $null -eq $appearanceValue -or
        $null -eq $appearanceAlias -or $readyValue -isnot [bool]) {
        throw "$role battle UI history omitted typed sequence/appearance/readiness"
    }
    [long]$sequence = $sequenceValue
    [long]$appearance = $appearanceValue
    if ($sequence -le [long]$State.UiCursor -or $sequence -gt [uint32]::MaxValue) {
        throw "$role battle UI history repeated/regressed at seq=$sequence"
    }
    $State.UiCursor = $sequence
    if ($appearance -lt 1 -or $appearance -gt [uint32]::MaxValue -or
        $appearance -ne [long]$appearanceAlias) {
        throw "$role battle UI history has inconsistent appearance=$appearance/$appearanceAlias"
    }

    $dialog = [string](Get-OptionalProperty $Event 'dialog')
    if ($dialog -ne 'DLG_BATTLE_A') {
        if ([long]$State.ResultUiSequence -eq 0) {
            throw "$role left DLG_BATTLE_A for $dialog before result controls were observed"
        }
        # The session-long native subscriber owns the exact result BTN_CLOSE.
        # This resolver consumes only the append-only transition which that
        # callback causes; it never publishes a competing UI action.
        $State.BattleCloseObserved = $true
        return
    }

    $targets = @((Get-OptionalProperty $Event 'targets') | Where-Object {
        [string](Get-OptionalProperty $_ 'dialog') -eq 'DLG_BATTLE_A'
    })
    if ($targets.Count -ne 1) {
        throw "$role battle UI event contains $($targets.Count) native battle owners"
    }
    [long]$owner = Get-OptionalProperty $targets[0] 'instance'
    if ($owner -lt 1 -or $owner -gt [uint32]::MaxValue) {
        throw "$role battle UI event owner token is outside uint32"
    }

    if ($appearance -ne [long]$State.LiveBattleAppearance -or
        $owner -ne [long]$State.LiveBattleOwner) {
        throw ("$role DLG_BATTLE_A changed its native owner/appearance within one battle " +
            "(live=$($State.LiveBattleAppearance)/$($State.LiveBattleOwner), " +
            "current=$appearance/$owner)")
    }

    $phase = Get-CanonicalBattleResultControlState -Role $role `
        -Widgets @(Get-OptionalProperty $targets[0] 'widgets') `
        -Ready ([bool]$readyValue)
    if ($phase.IsResult) {
        if ([long]$State.ResultUiSequence -eq 0) {
            $State.ResultUiSequence = $sequence
        }
    } elseif ([bool]$readyValue -and [long]$State.ResultUiSequence -ne 0) {
        throw "$role battle UI regressed from its result controls to live controls"
    }
}

function Get-CanonicalBattleResultTarget {
    param(
        [Parameter(Mandatory)][object]$State,
        [Parameter(Mandatory)][object]$Observation
    )
    $role = [string]$State.Role
    if (-not $Observation.Ready -or $Observation.Dialog -ne 'DLG_BATTLE_A' -or
        [long]$Observation.Instance -ne [long]$State.LiveBattleAppearance) {
        throw "$role result action did not resolve on the preserved ready battle appearance"
    }
    $target = Get-ReadyActionTarget $Observation DLG_BATTLE_A `
        ([long]$State.LiveBattleOwner)
    $widgets = @(Get-OptionalProperty $target 'widgets')
    $phase = Get-CanonicalBattleResultControlState -Role $role -Widgets $widgets -Ready $true
    if (-not $phase.IsResult) {
        return $null
    }
    return [pscustomobject]@{
        Target = $target
        CloseEnabled = [bool]$phase.CloseEnabled
    }
}

function Step-CanonicalBattleCompletionState {
    param(
        [Parameter(Mandatory)][object]$State,
        [object]$RawUi,
        [AllowEmptyCollection()][string[]]$BattleUpLines,
        [AllowEmptyCollection()][string[]]$BattleCloseLines,
        [object]$Baseline,
        [AllowNull()][object]$SavedAutoBattleProof
    )
    if ($State.Done) { return }

    $role = [string]$State.Role
    $literalSnapshotStep = $PSBoundParameters.ContainsKey('RawUi') -or
        $PSBoundParameters.ContainsKey('BattleUpLines') -or
        $PSBoundParameters.ContainsKey('BattleCloseLines') -or
        $PSBoundParameters.ContainsKey('Baseline') -or
        $PSBoundParameters.ContainsKey('SavedAutoBattleProof')
    if ($literalSnapshotStep) {
        foreach ($required in @(
                'RawUi', 'BattleUpLines', 'BattleCloseLines', 'Baseline',
                'SavedAutoBattleProof'
            )) {
            if (-not $PSBoundParameters.ContainsKey($required)) {
                throw "$role literal battle completion omitted saved '$required' evidence"
            }
        }

        # Pure equivalent of old Battle-Closed: the caller has already made its
        # two physical reads in battle-up then battle-close order. Never perform
        # relay/log/process I/O from this branch.
        # A long battle is a normal pending observation. The first physical log
        # snapshot carries the stable live-battle/auto proof once available; a
        # missing proof is not retried as an action and simply waits one +2 tick.
        if ($null -eq $SavedAutoBattleProof) { return }
        $State.BattleUpProof = $SavedAutoBattleProof
        if (-not (Test-LegacyBattleCloseCommitted `
                -Role $role -Baseline $Baseline -Lines $BattleCloseLines)) {
            return
        }
        $State.BattleCloseProof = Assert-ScriptedBattleCloseProof `
            -State $State -Baseline $Baseline -Lines $BattleCloseLines

        # /api/ui intentionally carries the exact UI identity/targets but no
        # worldSeq. The walk binding needs no world read here; zero is the valid
        # append-only origin used only by deferred MSS evidence after the whole
        # legacy chain. Add the alias locally without another observation.
        if ($null -eq (Get-OptionalProperty $RawUi 'worldSeq')) {
            $RawUi | Add-Member -NotePropertyName worldSeq -NotePropertyValue ([long]0)
        }
        $observation = ConvertTo-SavedDialogObservation $role $RawUi
        if (-not $observation.Ready -or
            $script:BareMapDialogs -notcontains [string]$observation.Dialog) {
            throw ("$role battle-close marker became terminal while the sole raw " +
                "GetLastDlg-equivalent still reported '$($observation.Dialog)'")
        }
        $State.AutoBattleProof = $SavedAutoBattleProof
        $State.BattleCloseObserved = $true
        $State.TerminalMapObservation = $observation
        $State.Done = $true
        return
    }

    $history = Get-UiHistory $role ([long]$State.UiCursor)
    foreach ($event in @($history.events | Sort-Object seq)) {
        Assert-CanonicalBattleUiEvent $State $event
    }

    $observation = Get-DialogObservation $role
    if (-not $observation -or -not $observation.Ready) {
        $State.PostBattleStable.Clear()
        return
    }

    $dialog = [string]$observation.Dialog
    if ($dialog -eq 'DLG_BATTLE_A') {
        $State.PostBattleStable.Clear()
        [long]$appearance = $observation.Instance
        if ($appearance -ne [long]$State.LiveBattleAppearance) {
            throw "$role current battle UI changed its exact battle appearance"
        }
        $result = Get-CanonicalBattleResultTarget $State $observation
        if (-not $result -or [long]$State.ResultUiSequence -eq 0) {
            return
        }
        # The native session subscriber claims this exact appearance/owner and
        # commits BTN_CLOSE once on a later natural UI frame. PowerShell only
        # proves that the result control is ready, then waits for its transition.
        if (-not $result.CloseEnabled) { return }
        return
    }

    if ($script:BareMapDialogs -contains $dialog) {
        $State.PostBattleStable.Clear()
        if (-not $State.BattleCloseObserved) {
            return
        }
        $State.TerminalMapObservation = $observation
        if ($State.LegacyCloseEvidenceOnly) {
            # Literal sync_attack resolver completion was the subscribed
            # battle-close/map transition. The global world verdict belongs
            # after the terminal +2 and the distinct post-loop +2.
            $State.Done = $true
            return
        }
        $world = Get-World $role
        $hero = Get-WorldStackExact $world ([string]$State.Spec.heroId) -AllowMissing
        $monster = Get-WorldStackExact $world ([string]$State.Spec.target.id) -AllowMissing
        $complete = $hero -and -not $monster -and
            [int]$hero.x -eq [int]$State.Spec.battleEnd.x -and
            [int]$hero.y -eq [int]$State.Spec.battleEnd.y -and
            [int]$hero.movement -eq [int]$State.Spec.battleEnd.movement
        if (-not $complete) {
            return
        }
        $State.Done = $true
        return
    }

    $nativeOwnedPostBattleDialogs = @(
        'DLG_MANAGE_STACK',
        'DLG_MESSAGE_BOX',
        'DLG_ITEM',
        'DLG_EVENT_POPUP'
    )
    if ($nativeOwnedPostBattleDialogs -notcontains $dialog) {
        throw "$role encountered unexpected post-battle dialog '$dialog'"
    }

    # The preboot scripted-popup subscriber is the sole owner of these native
    # appearances.  PowerShell observes them and waits for the bare map; it must
    # never race the subscriber with BTN_CLOSE/BTN_OK/BTN_RIGHTSIDE (including an
    # event popup published while this battle lifecycle is still active).
    $State.PostBattleStable.Clear()
    return
}


function Get-CanonicalBattleResult {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$Spec,
        [object]$World
    )
    if ($null -eq $World) { $World = Get-World $Role }
    $hero = Get-WorldStackExact $World ([string]$Spec.heroId)
    if (Get-WorldStackExact $World ([string]$Spec.target.id) -AllowMissing) {
        throw "$Role canonical target remained present after concurrent battle completion"
    }
    return [pscustomobject]@{
        role = $Role
        heroId = [string]$Spec.heroId
        targetId = [string]$Spec.target.id
        battleClosed = $true
        movement = [int]$hero.movement
        x = [int]$hero.x
        y = [int]$hero.y
    }
}

function Get-LegacyAttackResult {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$Plan,
        [Parameter(Mandatory)][object]$World,
        [Parameter(Mandatory)][object]$Lifecycle
    )
    # sync_attack.ps1's final census treated absence as a verdict value, not an
    # exception. Preserve that shape and expose the actual returned coordinates
    # and MP instead of projecting the fixture's expected result.
    $heroes = @($World.stacks | Where-Object {
        [string]$_.id -eq [string]$Plan.id
    })
    $targets = @($World.stacks | Where-Object {
        [string]$_.id -eq [string]$Plan.target.id
    })
    $hero = if ($heroes.Count -gt 0) { $heroes[0] } else { $null }
    $target = if ($targets.Count -gt 0) { $targets[0] } else { $null }
    $heroAlive = $null -ne $hero
    $targetGone = $null -eq $target
    $battleClosed = [bool]$Lifecycle.BattleClosed
    return [pscustomobject]@{
        role = $Role
        heroId = [string]$Plan.id
        targetId = [string]$Plan.target.id
        heroAlive = [bool]$heroAlive
        targetGone = [bool]$targetGone
        battleClosed = [bool]$battleClosed
        x = $(if ($hero) { [int]$hero.x } else { -1 })
        y = $(if ($hero) { [int]$hero.y } else { -1 })
        movement = $(if ($hero) { [int]$hero.movement } else { -1 })
        legacyPassed = [bool]($heroAlive -and $targetGone -and $battleClosed)
    }
}

function Get-LegacyBattleMarkerSnapshot {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][string]$LogPath
    )
    # sync_attack.ps1:137-145 performed one diagnostic hook-log read per
    # peer before its shared verdict census.  Keep this as its own read stage;
    # later crash and battle-close verdict scans must not be folded into it.
    $lines = @(Read-ClientLogLines $LogPath)
    $markers = @($lines | Select-String -Pattern @(
        '\[testdrv\]\[auto-battle\]',
        '\[testdrv\]\[auto-battle-proof\]',
        '\[testdrv\]\[scripted-popup\].*dialog=DLG_BATTLE_A',
        '55FC74'
    ) | Select-Object -Last 4 | ForEach-Object Line)
    return [pscustomobject]@{
        role = $Role
        lines = $markers
    }
}

function Get-LegacyQuietLogSnapshot {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][string]$LogPath,
        [int]$QuietSec = 3
    )
    # wait_day_ready.ps1 made one hook-log scan per role, took the latest
    # latest observed/click-claimed startup-popup action timestamp, and returned
    # immediately when both were already >=3 s old.  COMMITTED is a later native
    # callback receipt and therefore must not move this source clock forward.
    # OBSERVED and CLAIMED share GetTickCount64's monotonic clock with this
    # process; take whichever eligible action marker occurs last in log order.
    $rolePattern = [regex]::Escape($Role)
    $pattern = ('\[testdrv\]\[scripted-popup\] (?<kind>OBSERVED|CLAIMED) role={0} ' +
        'dialog=DLG_(BEGIN_TURN|EVENT_POPUP|GETINFO_BOX|MESSAGE_BOX|' +
        'SCENARIO_BRIEFING|ITEM|MANAGE_STACK) .* tick=(?<tick>\d+)$') -f
        $rolePattern
    $last = @(Read-ClientLogLines $LogPath | Select-String -Pattern $pattern |
        Select-Object -Last 1)
    [bool]$markerFound = $last.Count -eq 1 -and
        $last[0].Matches[0].Success
    [string]$markerKind = if ($markerFound) {
        $last[0].Matches[0].Groups['kind'].Value
    } else { '' }
    [long]$actionTick = if ($markerFound) {
        $last[0].Matches[0].Groups['tick'].Value
    } else { 0 }
    $quietMilliseconds = if ($markerFound) {
        [long]([Environment]::TickCount64 - $actionTick)
    } else { $null }
    return [pscustomobject]@{
        role = $Role
        markerFound = $markerFound
        markerKind = $markerKind
        actionTick = $actionTick
        quietMilliseconds = $quietMilliseconds
        ready = [bool]($markerFound -and
            [long]$quietMilliseconds -ge ([long]$QuietSec * 1000))
    }
}


function Get-LegacyReadyRoleObservation {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$State,
        [Parameter(Mandatory)][System.Diagnostics.Process]$Process
    )
    if (-not [bool](Get-OptionalProperty $State 'connected') -or
        [long](Get-OptionalProperty $State 'pid') -ne [long]$Process.Id) {
        throw "$Role legacy ready poll lost its exact owned process $($Process.Id)"
    }
    $readyValue = Get-OptionalProperty $State 'dialogReady'
    if ($readyValue -isnot [bool]) {
        throw "$Role legacy ready poll received non-boolean dialog readiness"
    }
    [string]$dialog = Get-OptionalProperty $State 'dialog'
    if (-not [bool]$readyValue -or
        $script:BareMapDialogs -notcontains $dialog) {
        return $null
    }
    return ConvertTo-SavedDialogObservation $Role $State
}

function Wait-LegacyReadyQuietPair {
    param(
        [Parameter(Mandatory)][string]$HostLog,
        [Parameter(Mandatory)][string]$JoinLog,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [int]$QuietSec = 3,
        [int]$TimeoutSec = 45,
        [int]$PollMilliseconds = 800
    )
    if ($QuietSec -lt 0 -or $TimeoutSec -le 0 -or
        $PollMilliseconds -ne 800) {
        throw 'legacy ready/quiet wait requires nonnegative quiet, positive timeout, and exact 800 ms polling'
    }
    [DateTime]$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    # wait_day_ready.ps1 resolved both role PIDs exactly once before entering
    # its polling loop. Preserve that census independently from the caller's
    # earlier sync_attack.ps1 role reads.
    $resolvedHostState = Get-RoleState host
    $resolvedJoinState = Get-RoleState join
    foreach ($resolved in @(
            [pscustomobject]@{
                role = 'host'; state = $resolvedHostState; process = $HostProcess
            },
            [pscustomobject]@{
                role = 'join'; state = $resolvedJoinState; process = $JoinProcess
            }
        )) {
        if (-not $resolved.state -or
            -not [bool](Get-OptionalProperty $resolved.state 'connected') -or
            [long](Get-OptionalProperty $resolved.state 'pid') -ne
                [long]$resolved.process.Id) {
            throw ("legacy ready PID resolve for $($resolved.role) did not match " +
                "its owned process $($resolved.process.Id)")
        }
    }
    [int]$sampleCount = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        $sampleCount++
        # The source loop did exactly one host and one join hook-log scan. A
        # failed sample advances only the fixed observation clock; it performs
        # no UI/world/state read and never submits or repeats an action.
        $hostQuiet = Get-LegacyQuietLogSnapshot host $HostLog $QuietSec
        $joinQuiet = Get-LegacyQuietLogSnapshot join $JoinLog $QuietSec
        Write-Step ("legacy ready/quiet sample ${sampleCount}: host={0} join={1}" -f
            $(if ($hostQuiet.ready) { 'quiet' } else { 'not-quiet' }),
            $(if ($joinQuiet.ready) { 'quiet' } else { 'not-quiet' }))
        if ([bool]$hostQuiet.ready -and [bool]$joinQuiet.ready) {
            # MSS needs one exact saved dialog owner for the following one-shot
            # action. Capture it only after the literal legacy quiet boundary;
            # a contradiction here is terminal rather than another poll path.
            $hostState = Get-RoleState host
            $joinState = Get-RoleState join
            $hostObservation = Get-LegacyReadyRoleObservation `
                host $hostState $HostProcess
            $joinObservation = Get-LegacyReadyRoleObservation `
                join $joinState $JoinProcess
            if ($null -eq $hostObservation -or $null -eq $joinObservation) {
                throw 'legacy quiet boundary did not expose one ready bare-map dialog per owned role'
            }
            return [pscustomobject]@{
                hostState = $hostState
                joinState = $joinState
                hostObservation = $hostObservation
                joinObservation = $joinObservation
                preparation = New-CanonicalWalkPreparation `
                    $hostObservation $joinObservation
                hostQuiet = $hostQuiet
                joinQuiet = $joinQuiet
                sampleCount = $sampleCount
                pollMilliseconds = $PollMilliseconds
            }
        }
        Start-Sleep -Milliseconds 800
    }
    throw "legacy ready/quiet wait timed out after ${TimeoutSec}s"
}

function Invoke-CanonicalConcurrentAttacks([object]$Fixture,
                                           [System.Diagnostics.Process]$HostProcess,
                                           [System.Diagnostics.Process]$JoinProcess,
                                           [string]$HostLog,
                                           [string]$JoinLog) {
    # sync_attack.ps1:18-23 first performed its process census and standalone
    # /api/stacks reachability probe. Only then came two independent Get-HeroId
    # snapshots, two /api/state PID-role reads, and the third stack census used
    # by both NearN plans.
    $ownedProcessIds = @([int]$HostProcess.Id, [int]$JoinProcess.Id)
    $ownedProcessesBeforeAttack = @(
        Get-Process -Id $ownedProcessIds -ErrorAction SilentlyContinue)
    if ($ownedProcessesBeforeAttack.Count -ne 2) {
        throw "legacy attack entry found $($ownedProcessesBeforeAttack.Count)/2 owned game processes"
    }
    $stackReachability = Get-LegacyStackSnapshot
    if (-not $stackReachability -or
        $null -eq (Get-OptionalProperty $stackReachability 'stacks')) {
        throw 'legacy attack standalone stack reachability probe failed'
    }
    $hostHeroCensus = Get-LegacyStackSnapshot
    $joinHeroCensus = Get-LegacyStackSnapshot
    $hostRoleState = Get-RoleState host
    $joinRoleState = Get-RoleState join
    foreach ($owned in @(
            [pscustomobject]@{ role = 'host'; state = $hostRoleState; process = $HostProcess },
            [pscustomobject]@{ role = 'join'; state = $joinRoleState; process = $JoinProcess }
        )) {
        if (-not $owned.state -or -not [bool]$owned.state.connected -or
            [long]$owned.state.pid -ne [long]$owned.process.Id) {
            throw ("legacy attack PID resolve for $($owned.role) did not match its owned process " +
                "$($owned.process.Id)")
        }
    }
    $attackCensus = Get-LegacyStackSnapshot
    $attackPlan = Get-LegacySharedAttackPlan `
        $Fixture $hostHeroCensus $joinHeroCensus $attackCensus

    # Literal wait_day_ready.ps1 behavior: bounded, read-only +800 ms samples
    # for at most 45 seconds. The successful sample also retains the exact map
    # owners required by the following one-shot attack; no action is retried.
    $attackReady = Wait-LegacyReadyQuietPair `
        -HostLog $HostLog -JoinLog $JoinLog `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -QuietSec 3 -TimeoutSec 45 -PollMilliseconds 800
    $quietHostRoleState = $attackReady.hostState
    $quietJoinRoleState = $attackReady.joinState
    $attackPreparation = $attackReady.preparation

    # All MSS identities/watermarks came from those same source-equivalent role
    # reads. Once the quiet gate returns, only two Mv84 reads precede fire.
    $hostUi = [long]$attackPreparation.host.uiAfter
    $joinUi = [long]$attackPreparation.join.uiAfter
    $hostWorld = [long]$attackPreparation.host.worldAfter
    $joinWorld = [long]$attackPreparation.join.worldAfter
    $attackIntent = @(
        [pscustomobject]@{
            role = 'host'; id = [string]$attackPlan.host.id
            fromX = [int]$attackPlan.host.fromX; fromY = [int]$attackPlan.host.fromY
            x = [int]$attackPlan.host.target.x; y = [int]$attackPlan.host.target.y
            appearance = [long]$attackPreparation.host.appearance
            instance = [long]$attackPreparation.host.instance
        },
        [pscustomobject]@{
            role = 'join'; id = [string]$attackPlan.join.id
            fromX = [int]$attackPlan.join.fromX; fromY = [int]$attackPlan.join.fromY
            x = [int]$attackPlan.join.target.x; y = [int]$attackPlan.join.target.y
            appearance = [long]$attackPreparation.join.appearance
            instance = [long]$attackPreparation.join.instance
        }
    )
    # This canonical scenario has exactly one battle per role and no earlier
    # battle.  A zero-line baseline is therefore the exact pre-first-battle
    # identity and avoids two source-foreign log reads before Wait-DayReady.
    $battleCloseBaselines = @{
        host = [pscustomobject]@{ role = 'host'; logPath = $HostLog; lineCount = 0 }
        join = [pscustomobject]@{ role = 'join'; logPath = $JoinLog; lineCount = 0 }
    }

    $attackQuiet = @{
        host = $attackReady.hostQuiet
        join = $attackReady.joinQuiet
    }
    # Keep the two old MP=35/35 reads adjacent to the common fire.
    $preFire = Assert-CanonicalPreFireSnapshot $Fixture
    Write-Step 'canonical attacks: submitting one host and one join move from a common fire barrier'
    # A cold paired attack owns two coordinated native result paths. Give each
    # the explicit 12-second relay budget without changing the one-fire source.
    $fire = Invoke-PreparedParallelAttackMoves $attackIntent `
        -CommandTimeoutMilliseconds 12000
    # Legacy sync_attack.ps1 sampled MP only after this exact 1.5-second
    # post-fire window. Keep the same barrier before consuming battle evidence.
    Start-Sleep -Milliseconds 1500
    $postFire = Assert-CanonicalPostFireChargeSnapshot $Fixture

    # The working DLL armed AUTO_BATTLE before boot, then each peer serviced its
    # own DLG_BATTLE_A independently. Preserve that lifetime exactly: this
    # resolver observes the native proof and never issues a battle action.
    $battleLifecycles = Invoke-CanonicalBattleLifecyclesIndependently `
        -Fixture $Fixture -AfterUi @{ host = $hostUi; join = $joinUi } `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -HostLog $HostLog -JoinLog $JoinLog `
        -BattleCloseBaselines $battleCloseBaselines

    # The lifecycle returned only after the terminal iteration's +2 and the
    # distinct post-loop +2. Preserve the old order from here: two independent
    # Mv84 reads, per-process crash/battle markers, then one global verdict
    # census. There is deliberately no convergence wait before this verdict.
    $postBattleMoves = Get-LegacySequentialSharedMovementSnapshot $Fixture
    $postBattleMatchesFire =
        [int]$postBattleMoves.host.movement -eq [int]$postFire.host.movement -and
        [int]$postBattleMoves.join.movement -eq [int]$postFire.join.movement
    $postBattleMoves | Add-Member -NotePropertyName matchesPostFire `
        -NotePropertyValue ([bool]$postBattleMatchesFire)
    Write-Step ("canonical attacks: legacy post-battle MP={0}/{1}; matches post-fire => {2}" -f
        $postBattleMoves.host.movement, $postBattleMoves.join.movement,
        $postBattleMatchesFire)

    # Source order after the two Mv84 reads:
    #   host marker log -> join marker log -> one shared stack census ->
    #   one tracked-process census -> host crash log -> join crash log ->
    #   host battle-up log -> host battle-close log -> join battle-up log ->
    #   join battle-close log. All parsing after those reads is pure.
    $legacyBattleMarkers = @{}
    $legacyBattleMarkers.host = Get-LegacyBattleMarkerSnapshot host $HostLog
    $legacyBattleMarkers.join = Get-LegacyBattleMarkerSnapshot join $JoinLog
    $legacyVerdictWorld = Get-LegacyStackSnapshot
    $ownedProcessIds = @([int]$HostProcess.Id, [int]$JoinProcess.Id)
    $ownedProcesses = @(Get-Process -Id $ownedProcessIds -ErrorAction SilentlyContinue)
    # Two distinct per-peer crash-log reads, in source role order. The source
    # verdict recognized only 55FC74; the broader MSS fault schema is evaluated
    # later against these same immutable snapshots.
    $hostCrashLines = @(Read-ClientLogLines $HostLog)
    $joinCrashLines = @(Read-ClientLogLines $JoinLog)
    $legacyCrashCount = @(
        $hostCrashLines | Select-String -Pattern '55FC74'
        $joinCrashLines | Select-String -Pattern '55FC74'
    ).Count

    # Literal final Battle-Closed order. Do not fold either pattern into the
    # other read and do not interleave a relay/process observation here.
    $hostBattleUpLines = @(Read-ClientLogLines $HostLog)
    $hostBattleCloseLines = @(Read-ClientLogLines $HostLog)
    $joinBattleUpLines = @(Read-ClientLogLines $JoinLog)
    $joinBattleCloseLines = @(Read-ClientLogLines $JoinLog)

    $hostFinalClosed = Get-LegacyBattleClosedSnapshot `
        -Role host -Baseline $battleCloseBaselines.host `
        -BattleUpLines $hostBattleUpLines `
        -BattleCloseLines $hostBattleCloseLines
    $joinFinalClosed = Get-LegacyBattleClosedSnapshot `
        -Role join -Baseline $battleCloseBaselines.join `
        -BattleUpLines $joinBattleUpLines `
        -BattleCloseLines $joinBattleCloseLines
    # The legacy verdict already consumed these four final immutable reads.
    # Keep the deferred MSS parser on the same successful snapshots instead of
    # an earlier resolver iteration that may have ended just before COMMITTED.
    $battleLifecycles.host.BattleUpLines = @($hostBattleUpLines)
    $battleLifecycles.host.BattleCloseLines = @($hostBattleCloseLines)
    $battleLifecycles.join.BattleUpLines = @($joinBattleUpLines)
    $battleLifecycles.join.BattleCloseLines = @($joinBattleCloseLines)
    $battleLifecycles.host.BattleClosed = [bool]$hostFinalClosed.closed
    $battleLifecycles.host.BattleClosedSnapshot = $hostFinalClosed
    $battleLifecycles.join.BattleClosed = [bool]$joinFinalClosed.closed
    $battleLifecycles.join.BattleClosedSnapshot = $joinFinalClosed

    $hostResult = Get-LegacyAttackResult host $attackPlan.host `
        $legacyVerdictWorld $battleLifecycles.host
    $joinResult = Get-LegacyAttackResult join $attackPlan.join `
        $legacyVerdictWorld $battleLifecycles.join
    $legacyPassed = $hostResult.legacyPassed -and $joinResult.legacyPassed -and
        $ownedProcesses.Count -eq 2 -and $legacyCrashCount -eq 0
    Write-Step (("legacy attack verdict: host alive={0} targetGone={1} battleClosed={2}; " +
        "join alive={3} targetGone={4} battleClosed={5}; processes={6}/2; 55FC74={7}") -f
        $hostResult.heroAlive, $hostResult.targetGone, $hostResult.battleClosed,
        $joinResult.heroAlive, $joinResult.targetGone, $joinResult.battleClosed,
        $ownedProcesses.Count, $legacyCrashCount)
    if (-not $legacyPassed) {
        throw 'legacy concurrent attack verdict failed'
    }

    # LITERAL_ATTACK_VERDICT_BOUNDARY. Everything below only packages evidence
    # already captured by this phase. No additional observation may delay
    # post_battle_walk.ps1's leading +3.
    Write-Step (("legacy concurrent attacks PASS: both monsters defeated, heroes alive, " +
        "BattleClosed=true, exact-owned processes=2, no 55FC74; skew={0:N1}ms") -f
        $fire.dispatchSkewMs)

    # No extension may delay post_battle_walk.ps1's leading +3. Package only
    # evidence which this literal phase already captured; the caller can verify
    # it after the complete legacy action chain.
    $deferredEvidence = [pscustomobject]@{
        kind = 'attack'
        stackReachability = $stackReachability
        hostHeroCensus = $hostHeroCensus
        joinHeroCensus = $joinHeroCensus
        attackCensus = $attackCensus
        attackPlan = $attackPlan
        attackQuiet = $attackQuiet
        preFireSamples = @($preFire.samples)
        postFireSamples = @($postFire.samples)
        postBattleSamples = @($postBattleMoves.samples)
        legacyBattleMarkers = $legacyBattleMarkers
        legacyVerdictWorld = $legacyVerdictWorld
        hostCrashLines = @($hostCrashLines)
        joinCrashLines = @($joinCrashLines)
        battleCloseBaselines = $battleCloseBaselines
        battleLifecycles = $battleLifecycles
        hostWorldAfter = [long]$hostWorld
        joinWorldAfter = [long]$joinWorld
    }
    $autoBattleResult = [pscustomobject]@{
        dispatchSkewMs = $null
        source = 'observed-lifecycle-log'
        dispatchTimestampsAvailable = $false
        roles = @(
            Get-ObservedPrearmedAutoBattleRecord `
                host @($battleLifecycles.host.BattleUpLines)
            Get-ObservedPrearmedAutoBattleRecord `
                join @($battleLifecycles.join.BattleUpLines)
        )
    }
    $deferredEvidence | Add-Member -NotePropertyName autoBattleResult `
        -NotePropertyValue $autoBattleResult
    $battleStartedCount = @(
        @($hostFinalClosed, $joinFinalClosed) |
            Where-Object { [long]$_.battleUpLine -gt 0 }).Count
    $battleClosedCount = @(
        @($hostFinalClosed, $joinFinalClosed) |
            Where-Object { [bool]$_.closed }).Count
    return [pscustomobject]@{
        dispatchSkewMs = $fire.dispatchSkewMs
        preFire = $preFire
        postFire = $postFire
        postBattle = $postBattleMoves
        autoBattle = $autoBattleResult
        host = $hostResult
        join = $joinResult
        legacyPass = $true
        legacyPassed = $true
        battleStartedCount = [int]$battleStartedCount
        battleClosedCount = [int]$battleClosedCount
        ownedProcessCount = [int]$ownedProcesses.Count
        deathCrash55FC74Count = [int]$legacyCrashCount
        postFireRangePass = [bool]$postFire.legacyRangePass
        postBattleMatchesPostFire = [bool]$postBattleMoves.matchesPostFire
        deferredEvidence = $deferredEvidence
    }
}

function Get-LegacyWalkStrayResult {
    param(
        [Parameter(Mandatory)][long]$HostAfter,
        [Parameter(Mandatory)][long]$JoinAfter,
        [Parameter(Mandatory)][string]$Label,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess
    )
    # post_battle_walk.ps1 read /api/state exactly once before four hook-log
    # scans: battle-up then battle-close for host, followed by the same pair for
    # join. Preserve that boundary and ordering. The identity assertions below
    # are pure; do not append an OS-process read.
    $state = Get-RelayState
    Assert-DebugRelayClientIdentity host `
        (Get-OptionalProperty $state 'host') $HostProcess
    Assert-DebugRelayClientIdentity join `
        (Get-OptionalProperty $state 'join') $JoinProcess

    $cursors = @{ host = $HostAfter; join = $JoinAfter }
    $stray = $false
    $roleResults = [System.Collections.Generic.List[object]]::new()
    foreach ($role in @('host', 'join')) {
        [long]$cursor = $cursors[$role]
        # The source performed two independent Select-String reads per peer:
        # latest "battle UI up", then latest "[battle-close]".  Preserve both
        # observations and H-up -> H-close -> J-up -> J-close ordering.  The UI
        # stream's subsequent ready bare-map publication is the native close's
        # causal equivalent; it is observed only, never serviced or retried.
        $upHistory = Get-UiHistorySnapshot $role $cursor
        $upEvents = @($upHistory.events | Sort-Object seq)
        foreach ($event in $upEvents) {
            [long]$seq = $event.seq
            if ($seq -le $cursor) {
                throw "$role UI history repeated/regressed at seq=$seq after walk watermark=$cursor"
            }
        }
        $lastBattle = @($upEvents | Where-Object {
            [string]$_.dialog -eq 'DLG_BATTLE_A'
        } | Select-Object -Last 1)

        $closeHistory = Get-UiHistorySnapshot $role $cursor
        $closeEvents = @($closeHistory.events | Sort-Object seq)
        foreach ($event in $closeEvents) {
            [long]$seq = $event.seq
            if ($seq -le $cursor) {
                throw "$role UI history repeated/regressed at seq=$seq after walk watermark=$cursor"
            }
        }
        $roleStray = $false
        $resolved = $true
        $waitSamples = 0
        if ($lastBattle.Count -eq 1) {
            [long]$battleSequence = $lastBattle[0].seq
            $lastClose = @($closeEvents | Where-Object {
                [long]$_.seq -gt $battleSequence -and
                [bool]$_.dialogReady -and
                $script:BareMapDialogs -contains [string]$_.dialog
            } | Select-Object -Last 1)
            if ($lastClose.Count -ne 1) {
                # The source made stray sticky, then passively re-read only
                # Battle-Closed every +5 seconds for up to 90 seconds. It never
                # fired a second move or any battle/dialog action.
                $stray = $true
                $roleStray = $true
                $resolved = $false
                Write-Step "$Label STRAY BATTLE on $role; passively waiting for BattleClosed"
                for ($waited = 0; $waited -lt 90; $waited += 5) {
                    Start-Sleep -Seconds 5
                    $waitSamples++
                    $polledCloseHistory = Get-UiHistorySnapshot $role $cursor
                    $polledCloseEvents = @(
                        $polledCloseHistory.events | Sort-Object seq)
                    foreach ($event in $polledCloseEvents) {
                        [long]$seq = $event.seq
                        if ($seq -le $cursor) {
                            throw "$role UI history repeated/regressed at seq=$seq after walk watermark=$cursor"
                        }
                    }
                    $polledClose = @($polledCloseEvents | Where-Object {
                        [long]$_.seq -gt $battleSequence -and
                        [bool]$_.dialogReady -and
                        $script:BareMapDialogs -contains [string]$_.dialog
                    } | Select-Object -Last 1)
                    if ($polledClose.Count -eq 1) {
                        $resolved = $true
                        Write-Step "$Label $role stray battle resolved (BattleClosed)"
                        break
                    }
                }
            }
        }
        $roleResults.Add([pscustomobject]@{
            role = $role
            stray = [bool]$roleStray
            resolved = [bool]$resolved
            waitSamples = [int]$waitSamples
            battleSequence = $(if ($lastBattle.Count -eq 1) {
                    [long]$lastBattle[0].seq
                } else {
                    [long]0
                })
        })
    }
    return [pscustomobject]@{
        stray = [bool]$stray
        relayState = $state
        roles = @($roleResults)
    }
}

function Invoke-CanonicalParallelWalk {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][ValidateSet('postBattleWalk', 'day2Reverse')]$Phase,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess
    )
    # First executable/observable step of both literal source invocations.
    Start-Sleep -Seconds 3
    # The immediately following observable operation is the one shared census.
    $planCensus = Get-LegacyStackSnapshot

    # MSS-only capability adapter. The old route addressed a role and stack,
    # not a dialog generation. Take one aggregate state snapshot only after the
    # source's first census, freeze both exact current map identities, and fire
    # once. A not-ready/stale role is terminal; there is no wait, refresh,
    # retry, or fallback.
    $walkActionState = Get-RelayState
    $walkActionPreparation = New-CanonicalWalkPreparationFromRelayState `
        $walkActionState $HostProcess $JoinProcess
    $hostUi = [long]$walkActionPreparation.host.uiAfter
    $joinUi = [long]$walkActionPreparation.join.uiAfter
    $walkPlan = Get-LegacySharedWalkPlan $Fixture $Phase $planCensus
    $walkIntent = @(
        [pscustomobject]@{
            role = 'host'; id = [string]$walkPlan.host.id
            fromX = [int]$walkPlan.host.fromX; fromY = [int]$walkPlan.host.fromY
            x = [int]$walkPlan.host.target.x; y = [int]$walkPlan.host.target.y
            appearance = [long]$walkActionPreparation.host.appearance
            instance = [long]$walkActionPreparation.host.instance
        },
        [pscustomobject]@{
            role = 'join'; id = [string]$walkPlan.join.id
            fromX = [int]$walkPlan.join.fromX; fromY = [int]$walkPlan.join.fromY
            x = [int]$walkPlan.join.target.x; y = [int]$walkPlan.join.target.y
            appearance = [long]$walkActionPreparation.join.appearance
            instance = [long]$walkActionPreparation.join.instance
        }
    )

    Write-Step "${Phase}: one exact move per hero from a common fire barrier"
    $fire = Invoke-PreparedParallelWorldMoves $walkIntent

    # Literal green interval: no UI/world/process observation is allowed here.
    Start-Sleep -Seconds 4

    # The old global Stacks endpoint was read once for host, then once again for
    # joiner. Preserve both calls and their host-authoritative provenance before
    # any dialog or process check.
    $hostResultWorld = Get-LegacyStackSnapshot
    $hostResultHero = Get-WorldStackExact $hostResultWorld ([string]$walkPlan.host.id) -AllowMissing
    $joinResultWorld = Get-LegacyStackSnapshot
    $joinResultHero = Get-WorldStackExact $joinResultWorld ([string]$walkPlan.join.id) -AllowMissing
    $hostMoved = $hostResultHero -and (
        [int]$hostResultHero.x -ne [int]$walkPlan.host.fromX -or
        [int]$hostResultHero.y -ne [int]$walkPlan.host.fromY)
    $joinMoved = $joinResultHero -and (
        [int]$joinResultHero.x -ne [int]$walkPlan.join.fromX -or
        [int]$joinResultHero.y -ne [int]$walkPlan.join.fromY)
    $hostCharged = $hostResultHero -and
        [int]$hostResultHero.movement -ge 0 -and
        [int]$hostResultHero.movement -lt [int]$walkPlan.host.movementBefore
    $joinCharged = $joinResultHero -and
        [int]$joinResultHero.movement -ge 0 -and
        [int]$joinResultHero.movement -lt [int]$walkPlan.join.movementBefore
    $hostActual = [pscustomobject]@{
        role = 'host'
        id = [string]$walkPlan.host.id
        fromX = [int]$walkPlan.host.fromX
        fromY = [int]$walkPlan.host.fromY
        x = $(if ($hostResultHero) { [int]$hostResultHero.x } else { -1 })
        y = $(if ($hostResultHero) { [int]$hostResultHero.y } else { -1 })
        movementBefore = [int]$walkPlan.host.movementBefore
        movement = $(if ($hostResultHero) { [int]$hostResultHero.movement } else { -1 })
        movementAfter = $(if ($hostResultHero) { [int]$hostResultHero.movement } else { -1 })
        moved = [bool]$hostMoved
        charged = [bool]$hostCharged
    }
    $joinActual = [pscustomobject]@{
        role = 'join'
        id = [string]$walkPlan.join.id
        fromX = [int]$walkPlan.join.fromX
        fromY = [int]$walkPlan.join.fromY
        x = $(if ($joinResultHero) { [int]$joinResultHero.x } else { -1 })
        y = $(if ($joinResultHero) { [int]$joinResultHero.y } else { -1 })
        movementBefore = [int]$walkPlan.join.movementBefore
        movement = $(if ($joinResultHero) { [int]$joinResultHero.movement } else { -1 })
        movementAfter = $(if ($joinResultHero) { [int]$joinResultHero.movement } else { -1 })
        moved = [bool]$joinMoved
        charged = [bool]$joinCharged
    }

    Write-Step ("$Phase host: ({0},{1}) -> ({2},{3}), movement {4}->{5}, moved={6}, charged={7}" -f
        [int]$walkPlan.host.fromX, [int]$walkPlan.host.fromY,
        $(if ($hostResultHero) { [int]$hostResultHero.x } else { -1 }),
        $(if ($hostResultHero) { [int]$hostResultHero.y } else { -1 }),
        [int]$walkPlan.host.movementBefore,
        $(if ($hostResultHero) { [int]$hostResultHero.movement } else { -1 }),
        $hostMoved, $hostCharged)
    Write-Step ("$Phase join: ({0},{1}) -> ({2},{3}), movement {4}->{5}, moved={6}, charged={7}" -f
        [int]$walkPlan.join.fromX, [int]$walkPlan.join.fromY,
        $(if ($joinResultHero) { [int]$joinResultHero.x } else { -1 }),
        $(if ($joinResultHero) { [int]$joinResultHero.y } else { -1 }),
        [int]$walkPlan.join.movementBefore,
        $(if ($joinResultHero) { [int]$joinResultHero.movement } else { -1 }),
        $joinMoved, $joinCharged)

    # Source-equivalent post-hoc stray check: one shared peer-state census,
    # then battle-up and BattleClosed reads per role. Only an actually-open
    # stray battle adds the source's passive +5 ... +90 observation loop.
    $strayResult = Get-LegacyWalkStrayResult `
        $hostUi $joinUi $Phase $HostProcess $JoinProcess
    $legacyWalkSucceeded = $hostMoved -and $joinMoved -and
        $hostCharged -and $joinCharged -and -not [bool]$strayResult.stray
    if (-not $legacyWalkSucceeded) {
        $reason = if ([bool]$strayResult.stray) {
            'STRAY BATTLE during the walk'
        } else {
            'a hero did not move or was not charged'
        }
        throw "$Phase legacy verdict failed: $reason"
    }

    $expectedDay = if ($Phase -eq 'postBattleWalk') { 1 } else { 2 }
    # LITERAL_WALK_VERDICT_BOUNDARY. Exact fixture coordinates/MP and append-only
    # convergence are intentionally deferred until every old gameplay phase has
    # completed.
    Write-Step (("$Phase legacy PASS: positions host=({0},{1}) join=({2},{3}), movement={4}/{5}, " +
        'no stray battle, skew={6:N1}ms') -f
        [int]$hostActual.x, [int]$hostActual.y,
        [int]$joinActual.x, [int]$joinActual.y,
        [int]$hostActual.movement, [int]$joinActual.movement,
        $fire.dispatchSkewMs)
    return [pscustomobject]@{
        phase = $Phase
        dispatchSkewMs = $fire.dispatchSkewMs
        host = $hostActual
        join = $joinActual
        strayBattle = [bool]$strayResult.stray
        legacyPass = $true
        legacyPassed = $true
        walkActionPreparation = $walkActionPreparation
        deferredEvidence = [pscustomobject]@{
            kind = 'walk'
            phase = $Phase
            expectedDay = $expectedDay
            walkPlan = $walkPlan
            planCensus = $planCensus
            hostResult = $hostActual
            joinResult = $joinActual
            hostResultWorld = $hostResultWorld
            joinResultWorld = $joinResultWorld
            strayResult = $strayResult
            hostWorldAfter = [long]$walkActionPreparation.host.worldAfter
            joinWorldAfter = [long]$walkActionPreparation.join.worldAfter
        }
    }
}

function Assert-DeferredCanonicalHistoryContainsState {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][long]$After,
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][int]$Day,
        [Parameter(Mandatory)][object]$HostExpected,
        [Parameter(Mandatory)][object]$JoinExpected,
        [Parameter(Mandatory)][bool]$HostTargetPresent,
        [Parameter(Mandatory)][bool]$JoinTargetPresent,
        [Parameter(Mandatory)][string]$Description
    )
    $history = Get-WorldHistory $Role $After
    [long]$cursor = $After
    $matched = $false
    foreach ($event in @($history.events | Sort-Object seq)) {
        [long]$sequence = $event.seq
        if ($sequence -le $cursor -or $sequence -gt [uint32]::MaxValue) {
            throw "$Role deferred world history repeated/regressed at seq=$sequence after $cursor"
        }
        $cursor = $sequence
        if (Test-CanonicalWorldState $event $Fixture $Day `
                $HostExpected $JoinExpected $HostTargetPresent $JoinTargetPresent) {
            $matched = $true
        }
    }
    if (-not $matched) {
        throw "deferred MSS extension did not find $Description in $Role append-only world history"
    }
}

function Assert-CanonicalDeferredGameplayEvidence {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][object[]]$Evidence
    )
    foreach ($item in @($Evidence)) {
        if (-not $item) { throw 'deferred MSS extension received empty evidence' }
        switch ([string]$item.kind) {
            'attack' {
                Assert-LegacySharedAttackPlanMatchesFixture `
                    $item.attackPlan $Fixture
                foreach ($world in @(
                        $item.hostHeroCensus,
                        $item.joinHeroCensus,
                        $item.attackCensus
                    )) {
                    if (-not (Test-LegacyStackSnapshotState $world $Fixture `
                            $Fixture.host.deploy $Fixture.join.deploy $true $true)) {
                        throw 'deferred attack evidence: an id/target-plan census lacked the deployed state'
                    }
                }
                foreach ($world in @($item.preFireSamples)) {
                    if (-not (Test-LegacyStackSnapshotState $world $Fixture `
                            $Fixture.host.deploy $Fixture.join.deploy $true $true)) {
                        throw 'deferred attack evidence: a saved pre-fire census lacked the deployed state'
                    }
                }
                foreach ($world in @($item.postFireSamples)) {
                    if (-not (Test-LegacyStackSnapshotState $world $Fixture `
                            $Fixture.host.battleStart $Fixture.join.battleStart $true $true)) {
                        throw 'deferred attack evidence: a saved +1500ms census lacked both attack charges'
                    }
                }
                # The two legacy post-battle MP reads predate the one verdict
                # census. They prove only each hero's owner/position/MP; target
                # absence remains strict solely on legacyVerdictWorld below.
                $postBattleSamples = @($item.postBattleSamples)
                if ($postBattleSamples.Count -ne 2 -or
                    -not (Test-LegacyRoleHeroSnapshotState `
                        $postBattleSamples[0] $Fixture host `
                        $Fixture.host.battleEnd) -or
                    -not (Test-LegacyRoleHeroSnapshotState `
                        $postBattleSamples[1] $Fixture join `
                        $Fixture.join.battleEnd)) {
                    throw ('deferred attack evidence: the two saved post-battle ' +
                        'censuses lacked their exact host-then-join hero states')
                }
                if (-not (Test-LegacyStackSnapshotState `
                        $item.legacyVerdictWorld $Fixture `
                        $Fixture.host.battleEnd $Fixture.join.battleEnd $false $false)) {
                    throw 'deferred attack evidence: the legacy verdict census lacked the exact result'
                }

                # The old verdict consumed only 55FC74. Apply the broader MSS
                # fault vocabulary after that boundary, against the same two
                # immutable host-then-join crash-log reads.
                $mssFaultPattern = @(
                    '\[simturns\] terminal fault:',
                    '\[SIMTURNS\] terminal pipe fault:',
                    '\[simturns\] preflight mismatch',
                    '\[simturns\] detour preflight mismatch',
                    '55FC74',
                    'midCommandQueue2PushHooked: message with id 21 is rejected due to outdated sequence number'
                )
                $mssFaults = @(
                    $item.hostCrashLines | Select-String -Pattern $mssFaultPattern
                    $item.joinCrashLines | Select-String -Pattern $mssFaultPattern
                )
                if ($mssFaults.Count -gt 0) {
                    throw "deferred attack evidence contains an MSS client fault: $($mssFaults[0].Line)"
                }

                # Validate the exact preboot auto callback and the one native
                # OBSERVED->CLAIMED->COMMITTED close chain only now. Neither
                # strengthens the old target/hero/BattleClosed/process/55FC74
                # verdict or claims that the iteration-head UI snapshot is a map.
                $autoRoles = [System.Collections.Generic.List[object]]::new()
                foreach ($role in @('host', 'join')) {
                    $saved = $item.battleLifecycles.$role
                    if (-not $saved -or -not $saved.BattleUi -or
                        -not [bool]$saved.BattleClosed) {
                        throw "deferred attack evidence omitted $role legacy battle lifecycle"
                    }
                    $proofState = New-CanonicalBattleCompletionState `
                        -Role $role -Spec $Fixture.$role `
                        -BattleUi $saved.BattleUi -LegacyCloseEvidenceOnly
                    $autoProof = Assert-LegacyBattleUpSnapshot `
                        -State $proofState `
                        -Baseline $item.battleCloseBaselines.$role `
                        -Lines @($saved.BattleUpLines)
                    $packed = @($item.autoBattleResult.roles | Where-Object {
                        [string]$_.role -eq $role
                    })
                    $packedKick = if ($packed.Count -eq 1 -and $packed[0].kick) {
                        $packed[0].kick | ConvertTo-Json -Compress -Depth 6
                    } else {
                        ''
                    }
                    $exactKick = $autoProof.kick |
                        ConvertTo-Json -Compress -Depth 6
                    if ($packed.Count -ne 1 -or -not [bool]$packed[0].found -or
                        -not [string]::Equals(
                            $packedKick, $exactKick,
                            [StringComparison]::Ordinal)) {
                        throw "$role deferred exact auto proof changed its observed lifecycle record"
                    }
                    [void](Assert-ScriptedBattleCloseProof `
                        -State $proofState `
                        -Baseline $item.battleCloseBaselines.$role `
                        -Lines @($saved.BattleCloseLines))
                    $autoRoles.Add($autoProof)
                }
                if ($autoRoles.Count -ne 2) {
                    throw 'deferred attack evidence did not validate both observed auto-battle records'
                }
                foreach ($role in @('host', 'join')) {
                    Assert-DeferredCanonicalHistoryContainsState -Role $role `
                        -After ([long]$item."${role}WorldAfter") -Fixture $Fixture -Day 1 `
                        -HostExpected $Fixture.host.battleEnd -JoinExpected $Fixture.join.battleEnd `
                        -HostTargetPresent $false -JoinTargetPresent $false `
                        -Description 'the exact concurrent battle result'
                }
                Write-Step 'deferred concurrent attacks MSS extension PASS'
            }
            'walk' {
                $phase = [string]$item.phase
                Assert-LegacySharedWalkPlanMatchesFixture `
                    $item.walkPlan $Fixture $phase
                if ($phase -eq 'postBattleWalk') {
                    $day = 1
                    $hostSource = $Fixture.host.battleEnd
                    $joinSource = $Fixture.join.battleEnd
                } elseif ($phase -eq 'day2Reverse') {
                    $day = 2
                    $hostSource = [pscustomobject]@{
                        x = [int]$Fixture.host.postBattleWalk.x
                        y = [int]$Fixture.host.postBattleWalk.y
                        movement = 35
                    }
                    $joinSource = [pscustomobject]@{
                        x = [int]$Fixture.join.postBattleWalk.x
                        y = [int]$Fixture.join.postBattleWalk.y
                        movement = 35
                    }
                } else {
                    throw "deferred walk evidence has unknown phase '$phase'"
                }
                if ([int]$item.expectedDay -ne $day -or
                    -not (Test-LegacyStackSnapshotState $item.planCensus $Fixture `
                        $hostSource $joinSource $false $false)) {
                    throw "deferred $phase evidence: saved shared plan census lacked its exact source state"
                }
                # The legacy walk verdict pins the destination and requires
                # moved+charged, not one RNG-dependent absolute MP value.
                # Preserve the actual successful result as the causal value
                # that both saved censuses and append-only histories must show.
                foreach ($role in @('host', 'join')) {
                    $savedResult = $item."${role}Result"
                    if (-not $savedResult -or
                        -not [bool]$savedResult.moved -or
                        -not [bool]$savedResult.charged -or
                        [int]$savedResult.x -ne [int]$Fixture.$role.$phase.x -or
                        [int]$savedResult.y -ne [int]$Fixture.$role.$phase.y -or
                        [int]$savedResult.movement -lt 0 -or
                        [int]$savedResult.movement -ge [int]$item.walkPlan.$role.movementBefore) {
                        throw "deferred $phase evidence changed the source moved+charged result for $role"
                    }
                }
                $hostExpected = [pscustomobject]@{
                    x = [int]$Fixture.host.$phase.x
                    y = [int]$Fixture.host.$phase.y
                    movement = [int]$item.hostResult.movement
                }
                $joinExpected = [pscustomobject]@{
                    x = [int]$Fixture.join.$phase.x
                    y = [int]$Fixture.join.$phase.y
                    movement = [int]$item.joinResult.movement
                }
                foreach ($world in @($item.hostResultWorld, $item.joinResultWorld)) {
                    if (-not (Test-LegacyStackSnapshotState $world $Fixture `
                            $hostExpected $joinExpected $false $false)) {
                        throw "deferred $phase evidence: a saved result census lacked the exact walk state"
                    }
                }
                foreach ($role in @('host', 'join')) {
                    Assert-DeferredCanonicalHistoryContainsState -Role $role `
                        -After ([long]$item."${role}WorldAfter") -Fixture $Fixture -Day $day `
                        -HostExpected $hostExpected -JoinExpected $joinExpected `
                        -HostTargetPresent $false -JoinTargetPresent $false `
                        -Description "the exact $phase result"
                }
                Write-Step "deferred $phase MSS extension PASS"
            }
            default {
                throw "deferred MSS extension received unknown evidence kind '$($item.kind)'"
            }
        }
    }
}


function Wait-CanonicalLegacyEndTurnRefresh([object]$Fixture,
                                            [System.Diagnostics.Process]$HostProcess,
                                            [System.Diagnostics.Process]$JoinProcess,
                                            [DateTime]$FireCompletedAt,
                                            [int]$TimeoutSec = 45) {
    if ($TimeoutSec -lt 3 -or ($TimeoutSec % 3) -ne 0) {
        throw 'legacy End Turn refresh timeout must be a positive multiple of three seconds'
    }
    for ($elapsed = 3; $elapsed -le $TimeoutSec; $elapsed += 3) {
        # Anchor every observation to the completion of the one parallel HTTP
        # pair. Relay evidence must not insert its own timeout before this old
        # +3/+6/.../+45 schedule.
        $observeAt = $FireCompletedAt.AddSeconds($elapsed)
        $remainingMs = [Math]::Ceiling(($observeAt - [DateTime]::UtcNow).TotalMilliseconds)
        if ($remainingMs -gt 0) {
            Start-Sleep -Milliseconds ([int]$remainingMs)
        }
        $vitals = Get-LegacySequentialSharedMovementSnapshot $Fixture
        Write-Step ("legacy End Turn refresh +${elapsed}s: MP={0}/{1}" -f
            $vitals.host.movement, $vitals.join.movement)
        if ([int]$vitals.host.movement -eq 35 -and
            [int]$vitals.join.movement -eq 35) {
            return [pscustomobject]@{
                host = $vitals.host
                join = $vitals.join
                firstObservationSec = $elapsed
            }
        }
        # The source loop performs no diagnostic read between fixed samples.
        # A missing process is exposed by the next exact census or the terminal
        # failure artifact; never insert a separate observation here.
    }
    throw 'legacy End Turn MP refresh did not reach exact 35/35 within 45 seconds'
}


function Invoke-CanonicalDay2Walk([object]$Fixture,
                                  [System.Diagnostics.Process]$HostProcess,
                                  [System.Diagnostics.Process]$JoinProcess) {
    return Invoke-CanonicalParallelWalk `
        $Fixture day2Reverse $HostProcess $JoinProcess
}





function Get-SimturnsEvidenceUtc([object]$Event, [string]$Description) {
    $raw = Get-OptionalProperty $Event 't'
    if ([string]::IsNullOrWhiteSpace([string]$raw)) {
        throw "$Description omitted its relay timestamp"
    }
    try {
        return [DateTimeOffset]::Parse(
            [string]$raw,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal -bor
                [Globalization.DateTimeStyles]::AdjustToUniversal).UtcDateTime
    } catch {
        throw "$Description has an invalid relay timestamp '$raw'"
    }
}

function Get-ExactLiveBattleUiProof {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][object]$State,
        [long]$ExpectedAppearance = 0,
        [long]$ExpectedOwner = 0,
        [Parameter(Mandatory)][string]$Context
    )
    $readyValue = Get-OptionalProperty $State 'dialogReady'
    $appearanceValue = Get-OptionalProperty $State 'dialogInstance'
    $appearanceAlias = Get-OptionalProperty $State 'dialogAppearance'
    $sequenceValue = Get-OptionalProperty $State 'seq'
    if ($null -eq $sequenceValue) {
        $sequenceValue = Get-OptionalProperty $State 'uiSeq'
    }
    if ([string](Get-OptionalProperty $State 'dialog') -ne 'DLG_BATTLE_A' -or
        $readyValue -isnot [bool] -or -not [bool]$readyValue -or
        $null -eq $appearanceValue -or $null -eq $appearanceAlias -or
        $null -eq $sequenceValue) {
        throw "$Role $Context is not one typed ready DLG_BATTLE_A publication"
    }
    [long]$appearance = $appearanceValue
    [long]$sequence = $sequenceValue
    if ($appearance -lt 1 -or $appearance -gt [uint32]::MaxValue -or
        $appearance -ne [long]$appearanceAlias -or
        $sequence -lt 1 -or $sequence -gt [uint32]::MaxValue -or
        ($ExpectedAppearance -gt 0 -and $appearance -ne $ExpectedAppearance)) {
        throw "$Role $Context has invalid or changed battle appearance/sequence"
    }

    $targets = @((Get-OptionalProperty $State 'targets') | Where-Object {
        [string](Get-OptionalProperty $_ 'dialog') -eq 'DLG_BATTLE_A'
    })
    if ($targets.Count -ne 1) {
        throw "$Role $Context contains $($targets.Count) native battle owners"
    }
    [long]$owner = Get-OptionalProperty $targets[0] 'instance'
    if ($owner -lt 1 -or $owner -gt [uint32]::MaxValue -or
        ($ExpectedOwner -gt 0 -and $owner -ne $ExpectedOwner)) {
        throw "$Role $Context has invalid or changed native battle owner"
    }

    $widgets = @((Get-OptionalProperty $targets[0] 'widgets'))
    $result = Get-CanonicalBattleResultControlState `
        -Role $Role -Widgets $widgets -Ready $true
    if ($result.IsResult) {
        throw "$Role $Context already exposes battle-result BTN_CLOSE"
    }
    $liveControlNames = @(
        'BTN_DEFEND', 'BTN_RETREAT', 'BTN_WAIT', 'BTN_RESOLVE', 'TOG_AUTOBATTLE'
    )
    $liveControls = @($widgets | Where-Object {
        $liveControlNames -contains [string](Get-OptionalProperty $_ 'name')
    })
    if ($liveControls.Count -lt 1 -or
        @($widgets | Where-Object {
            [string](Get-OptionalProperty $_ 'name') -eq 'BTN_CLOSE'
        }).Count -ne 0) {
        throw "$Role $Context has no live battle control or retained BTN_CLOSE"
    }
    return [pscustomobject]@{
        sequence = $sequence
        appearance = $appearance
        owner = $owner
        liveControls = @($liveControls | ForEach-Object {
            [string](Get-OptionalProperty $_ 'name')
        } | Sort-Object -Unique)
    }
}

function Wait-CleanLongMoveCompletion {
    param(
        [Parameter(Mandatory)][object]$Ready,
        [Parameter(Mandatory)][string[]]$Roles,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [int]$TimeoutSec = 120,
        [int]$PollMilliseconds = 100
    )
    $requiredRoles = @($Roles | Select-Object -Unique)
    if ($requiredRoles.Count -ne $Roles.Count -or
        $requiredRoles.Count -lt 1 -or $requiredRoles.Count -gt 2 -or
        @($requiredRoles | Where-Object { $_ -notin @('host', 'join') }).Count -ne 0) {
        throw 'clean long-move completion requires one or both unique relay roles'
    }
    if ($TimeoutSec -lt 1 -or $PollMilliseconds -ne 100) {
        throw 'clean long-move completion requires a positive timeout and exact 100 ms polling'
    }

    $states = [ordered]@{}
    foreach ($role in $requiredRoles) {
        $initialState = $Ready."${role}State"
        $initialIdle = Get-OptionalProperty $initialState 'strategicIdle'
        $preparation = $Ready.preparation.$role
        if ($initialIdle -isnot [bool] -or -not [bool]$initialIdle) {
            throw "$role clean long-move did not start from a strategic-idle publication"
        }
        if (-not $preparation -or [long]$preparation.uiAfter -lt 1 -or
            [long]$preparation.uiAfter -gt [uint32]::MaxValue) {
            throw "$role clean long-move completion has no valid pre-dispatch UI watermark"
        }
        $states[$role] = [pscustomobject]@{
            role = $role
            afterUiSequence = [long]$preparation.uiAfter
            expectedAppearance = [long]$preparation.appearance
            cursor = [long]$preparation.uiAfter
            eventsObserved = 0
            busyPublications = 0
            busySequence = $null
            busyUtc = $null
            idleSequence = $null
            idleUtc = $null
            done = $false
        }
    }

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        foreach ($role in $requiredRoles) {
            $state = $states[$role]
            $history = Get-UiHistory $role ([long]$state.cursor)
            foreach ($event in @($history.events | Sort-Object seq)) {
                [long]$sequence = Get-OptionalProperty $event 'seq'
                if ($sequence -le [long]$state.cursor -or
                    $sequence -gt [uint32]::MaxValue) {
                    throw ("$role clean long-move UI history repeated/regressed at " +
                        "seq=$sequence after $($state.cursor)")
                }
                $state.cursor = $sequence
                $state.eventsObserved = [int]$state.eventsObserved + 1

                [string]$dialog = Get-OptionalProperty $event 'dialog'
                if ([string]::IsNullOrWhiteSpace($dialog) -or
                    $script:BareMapDialogs -notcontains $dialog) {
                    throw "$role clean long-move crossed non-map dialog '$dialog' before completion"
                }
                $dialogReady = Get-OptionalProperty $event 'dialogReady'
                [long]$dialogAppearance = Get-OptionalProperty $event 'dialogAppearance'
                if ($dialogReady -isnot [bool] -or -not [bool]$dialogReady -or
                    $dialogAppearance -ne [long]$state.expectedAppearance) {
                    throw ("$role clean long-move map identity drifted before completion: " +
                        "ready=$dialogReady appearance=$dialogAppearance " +
                        "expected=$($state.expectedAppearance)")
                }
                $strategicIdle = Get-OptionalProperty $event 'strategicIdle'
                if ($strategicIdle -isnot [bool]) {
                    throw "$role clean long-move UI evidence omitted boolean strategicIdle"
                }
                $eventUtc = Get-SimturnsEvidenceUtc $event `
                    "$role clean long-move UI evidence"
                if (-not [bool]$strategicIdle) {
                    $state.busyPublications = [int]$state.busyPublications + 1
                    if ($null -eq $state.busySequence) {
                        $state.busySequence = $sequence
                        $state.busyUtc = $eventUtc
                    }
                } elseif ($null -ne $state.busySequence -and -not $state.done) {
                    if ($sequence -le [long]$state.busySequence) {
                        throw "$role clean long-move idle edge did not follow its busy edge"
                    }
                    $state.idleSequence = $sequence
                    $state.idleUtc = $eventUtc
                    $state.done = $true
                }
            }
        }

        if (@($requiredRoles | Where-Object { -not $states[$_].done }).Count -eq 0) {
            $evidence = [ordered]@{}
            foreach ($role in $requiredRoles) {
                $state = $states[$role]
                $evidence[$role] = [pscustomobject]@{
                    role = $role
                    afterUiSequence = [long]$state.afterUiSequence
                    expectedAppearance = [long]$state.expectedAppearance
                    busySequence = [long]$state.busySequence
                    busyUtc = [DateTime]$state.busyUtc
                    idleSequence = [long]$state.idleSequence
                    idleUtc = [DateTime]$state.idleUtc
                    busyPublications = [int]$state.busyPublications
                    eventsObserved = [int]$state.eventsObserved
                    busyMilliseconds =
                        ([DateTime]$state.idleUtc - [DateTime]$state.busyUtc).
                            TotalMilliseconds
                    transition = 'strategicIdle:false->true'
                }
            }
            return [pscustomobject]@{
                completed = $true
                kind = 'append-only-strategic-idle-transition'
                requiredRoles = @($requiredRoles)
                pollMilliseconds = $PollMilliseconds
                timeoutSeconds = $TimeoutSec
                roles = [pscustomobject]$evidence
            }
        }
        Start-Sleep -Milliseconds 100
    }

    $missing = @($requiredRoles | Where-Object { -not $states[$_].done } |
        ForEach-Object {
            $state = $states[$_]
            "$_(busy=$($null -ne $state.busySequence),events=$($state.eventsObserved))"
        })
    throw ("clean long-move timed out before strategicIdle false -> true for " +
        ($missing -join ', '))
}

function Assert-CleanLongMoveBusyOverlap {
    param(
        [Parameter(Mandatory)][object]$TerminalCompletion,
        [Parameter(Mandatory)][object]$Fire
    )
    $requiredRoles = @($TerminalCompletion.requiredRoles)
    if (-not [bool]$TerminalCompletion.completed -or
        [string]$TerminalCompletion.kind -ne
            'append-only-strategic-idle-transition' -or
        $requiredRoles.Count -ne 2 -or
        (@($requiredRoles | Sort-Object) -join ',') -ne 'host,join') {
        throw 'clean long-move overlap requires exact host+join terminal completion'
    }

    $intervals = [ordered]@{}
    foreach ($role in @('host', 'join')) {
        $terminal = $TerminalCompletion.roles.$role
        $response = @($Fire.responses | Where-Object {
            [string]$_.role -eq $role
        })
        if (-not $terminal -or $response.Count -ne 1 -or
            $response[0].found -isnot [bool] -or
            -not [bool]$response[0].found -or
            [long]$response[0].startedMs -lt 1 -or
            [long]$response[0].startedUiSeq -lt 1 -or
            [long]$response[0].startedUiSeq -gt [uint32]::MaxValue) {
            throw "$role clean long-move overlap omitted its unique accepted CommandStarted edge"
        }

        $commandStartedReceiptUtc = [DateTimeOffset]::FromUnixTimeMilliseconds(
            [long]$response[0].startedMs).UtcDateTime
        [DateTime]$busyUtc = $terminal.busyUtc
        [DateTime]$idleUtc = $terminal.idleUtc
        [long]$busySequence = $terminal.busySequence
        [long]$idleSequence = $terminal.idleSequence
        [long]$commandStartedUiSequence = $response[0].startedUiSeq
        [long]$effectiveBusySequence = [Math]::Max(
            $busySequence, $commandStartedUiSequence)
        if ($busySequence -lt 1 -or $idleSequence -le $effectiveBusySequence -or
            $idleUtc -le $busyUtc) {
            throw ("$role has no observed strategic-busy interval after CommandStarted: " +
                "busy=$busySequence started=$commandStartedUiSequence idle=$idleSequence")
        }
        $intervals[$role] = [pscustomobject]@{
            role = $role
            commandStartedReceiptUtc = $commandStartedReceiptUtc
            commandStartedUiSequence = $commandStartedUiSequence
            busySequence = $busySequence
            effectiveBusySequence = $effectiveBusySequence
            idleSequence = $idleSequence
            busyUtc = $busyUtc
            idleUtc = $idleUtc
            busyMilliseconds = ($idleUtc - $busyUtc).TotalMilliseconds
            transition = 'observed strategicIdle:false->true after sole paired fire'
        }
    }

    [long]$overlapStartSequence = if ([long]$intervals.host.effectiveBusySequence -gt
            [long]$intervals.join.effectiveBusySequence) {
        [long]$intervals.host.effectiveBusySequence
    } else {
        [long]$intervals.join.effectiveBusySequence
    }
    [long]$overlapEndSequence = if ([long]$intervals.host.idleSequence -lt
            [long]$intervals.join.idleSequence) {
        [long]$intervals.host.idleSequence
    } else {
        [long]$intervals.join.idleSequence
    }
    if ($overlapStartSequence -ge $overlapEndSequence) {
        throw ("clean long-move append-only strategic-busy windows did not overlap: " +
            "host=[$($intervals.host.busySequence),$($intervals.host.idleSequence)] " +
            "join=[$($intervals.join.busySequence),$($intervals.join.idleSequence)]")
    }
    $overlapStartUtc = if ([DateTime]$intervals.host.busyUtc -gt
            [DateTime]$intervals.join.busyUtc) {
        [DateTime]$intervals.host.busyUtc
    } else {
        [DateTime]$intervals.join.busyUtc
    }
    $overlapEndUtc = if ([DateTime]$intervals.host.idleUtc -lt
            [DateTime]$intervals.join.idleUtc) {
        [DateTime]$intervals.host.idleUtc
    } else {
        [DateTime]$intervals.join.idleUtc
    }
    return [pscustomobject]@{
        proved = $true
        kind = 'relay-observed-post-command-strategic-busy-overlap'
        host = $intervals.host
        join = $intervals.join
        overlapStartSequence = $overlapStartSequence
        overlapEndSequence = $overlapEndSequence
        overlapStartUtc = $overlapStartUtc
        overlapEndUtc = $overlapEndUtc
        overlapMilliseconds = ($overlapEndUtc - $overlapStartUtc).TotalMilliseconds
    }
}

function Wait-LongMoveTrajectoryEvidence {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][object]$Fire,
        [Parameter(Mandatory)][object]$After,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [int]$TimeoutSec = 120,
        [int]$SettleMilliseconds = 1200
    )
    if ($TimeoutSec -lt 1 -or $SettleMilliseconds -ne 1200) {
        throw 'long-move trajectory observer requires a positive timeout and exact 1200 ms settle'
    }
    $states = @{}
    foreach ($side in @('host', 'join')) {
        $fireResponse = @($Fire.responses | Where-Object {
            [string]$_.role -eq $side
        })
        if ($fireResponse.Count -ne 1 -or [long]$fireResponse[0].startedMs -lt 1) {
            throw "$side long-move omitted its unique native CommandStarted edge"
        }
        $commandStartedReceiptUtc = [DateTimeOffset]::FromUnixTimeMilliseconds(
            [long]$fireResponse[0].startedMs).UtcDateTime
        $states[$side] = [pscustomobject]@{
            side = $side
            heroId = [string]$Fixture.$side.heroId
            commandStartedReceiptUtc = $commandStartedReceiptUtc
            firstPositionChangeSequence = $null
            lastPositionChangeSequence = $null
            firstPositionChangeUtc = $null
            lastPositionChangeUtc = $null
            changes = 0
            lastX = [int]$Baseline.$side.x
            lastY = [int]$Baseline.$side.y
            lastMovement = [int]$Baseline.$side.movement
        }
    }

    $cursors = @{}
    foreach ($side in @('host', 'join')) {
        $watermark = $After.$side
        if ([long]$watermark -lt 1 -or [long]$watermark -gt [uint32]::MaxValue) {
            throw "$side long-move has no valid pre-fire world watermark"
        }
        $cursors[$side] = [long]$watermark
    }
    # The source diagnostic sampled 80 * 150 ms. Keep that bounded passive
    # observation window; this is not an action retry and no POST is repeated.
    [int]$observationTimeoutSec = [Math]::Min($TimeoutSec, 12)
    $deadline = [DateTime]::UtcNow.AddSeconds($observationTimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ClientsLive $HostProcess $JoinProcess
        foreach ($side in @('host', 'join')) {
            [long]$cursor = [long]$cursors[$side]
            $history = Get-WorldHistory $side $cursor
            foreach ($event in @($history.events | Sort-Object seq)) {
                [long]$sequence = Get-OptionalProperty $event 'seq'
                if ($sequence -le $cursor -or $sequence -gt [uint32]::MaxValue) {
                    throw "$side long-move world history repeated/regressed at seq=$sequence after $cursor"
                }
                $cursor = $sequence
                $eventUtc = Get-SimturnsEvidenceUtc $event "$side long-move own-world evidence"
                $state = $states[$side]
                $hero = Get-WorldStackExact $event ([string]$state.heroId)
                [int]$x = $hero.x
                [int]$y = $hero.y
                [int]$movement = $hero.movement
                if ($x -ne [int]$state.lastX -or $y -ne [int]$state.lastY) {
                    if ($null -eq $state.firstPositionChangeUtc) {
                        $state.firstPositionChangeSequence = $sequence
                        $state.firstPositionChangeUtc = $eventUtc
                    }
                    $state.lastPositionChangeSequence = $sequence
                    $state.lastPositionChangeUtc = $eventUtc
                    $state.changes = [int]$state.changes + 1
                }
                $state.lastX = $x
                $state.lastY = $y
                $state.lastMovement = $movement
            }
            $cursors[$side] = $cursor
        }

        if ($null -ne $states.host.lastPositionChangeUtc -and
            $null -ne $states.join.lastPositionChangeUtc) {
            $lastChangeUtc = if ([DateTime]$states.host.lastPositionChangeUtc -gt
                    [DateTime]$states.join.lastPositionChangeUtc) {
                [DateTime]$states.host.lastPositionChangeUtc
            } else {
                [DateTime]$states.join.lastPositionChangeUtc
            }
            if (([DateTime]::UtcNow - $lastChangeUtc).TotalMilliseconds -ge
                    $SettleMilliseconds) {
                break
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if ($null -eq $states.host.lastPositionChangeUtc -or
        $null -eq $states.join.lastPositionChangeUtc) {
        throw 'long-move timed out before both exact heroes produced movement evidence'
    }
    $latestLastMove = if ([DateTime]$states.host.lastPositionChangeUtc -gt
            [DateTime]$states.join.lastPositionChangeUtc) {
        [DateTime]$states.host.lastPositionChangeUtc
    } else {
        [DateTime]$states.join.lastPositionChangeUtc
    }
    if (([DateTime]::UtcNow - $latestLastMove).TotalMilliseconds -lt
            $SettleMilliseconds) {
        throw 'long-move did not remain settled for the source 1200 ms window'
    }
    foreach ($side in @('host', 'join')) {
        if ([int]$states[$side].changes -lt 2) {
            throw ("$side long-move own-world history has insufficient trajectory " +
                "sampling: $($states[$side].changes) changed publication(s)")
        }
    }

    # Preserve the source test's coordinate-change view as supporting evidence.
    # World reports are throttled snapshots, so adjacent native animations can
    # either look overlapping or serialized depending on publication phase. The
    # admission, exact final state and the dedicated battle-block scenario own
    # the simultaneous-turn verdict.
    [long]$hostFirstSequence = $states.host.firstPositionChangeSequence
    [long]$hostLastSequence = $states.host.lastPositionChangeSequence
    [long]$joinFirstSequence = $states.join.firstPositionChangeSequence
    [long]$joinLastSequence = $states.join.lastPositionChangeSequence
    [string]$worldPublicationRelation = if ($hostLastSequence -lt $joinFirstSequence) {
        'host-then-join'
    } elseif ($joinLastSequence -lt $hostFirstSequence) {
        'join-then-host'
    } else {
        'overlap'
    }
    [bool]$worldPublicationOverlapObserved =
        $worldPublicationRelation -eq 'overlap'
    [long]$presentationSequenceGap = if ($worldPublicationRelation -eq 'host-then-join') {
        $joinFirstSequence - $hostLastSequence
    } elseif ($worldPublicationRelation -eq 'join-then-host') {
        $hostFirstSequence - $joinLastSequence
    } else {
        0
    }
    $overlapStartSequence = $null
    $overlapEndSequence = $null
    $overlapStartUtc = $null
    $overlapEndUtc = $null
    $overlapMilliseconds = $null
    if ($worldPublicationOverlapObserved) {
        $overlapStartSequence = [Math]::Max($hostFirstSequence, $joinFirstSequence)
        $overlapEndSequence = [Math]::Min($hostLastSequence, $joinLastSequence)
        $overlapStartUtc = if ([DateTime]$states.host.firstPositionChangeUtc -gt
                [DateTime]$states.join.firstPositionChangeUtc) {
            [DateTime]$states.host.firstPositionChangeUtc
        } else {
            [DateTime]$states.join.firstPositionChangeUtc
        }
        $overlapEndUtc = if ([DateTime]$states.host.lastPositionChangeUtc -lt
                [DateTime]$states.join.lastPositionChangeUtc) {
            [DateTime]$states.host.lastPositionChangeUtc
        } else {
            [DateTime]$states.join.lastPositionChangeUtc
        }
        $overlapMilliseconds = ($overlapEndUtc - $overlapStartUtc).TotalMilliseconds
    }
    [bool]$sampledRoleLocalOverlapObserved =
        $worldPublicationOverlapObserved -and $null -ne $overlapMilliseconds -and
        $overlapMilliseconds -gt 0
    return [pscustomobject]@{
        host = $states.host
        join = $states.join
        completeOwnWorldTrajectoriesProved = $true
        roleLocalOwnTrajectoryOverlapProved = $sampledRoleLocalOverlapObserved
        worldPublicationOverlapObserved = $worldPublicationOverlapObserved
        worldPublicationRelation = $worldPublicationRelation
        worldPublicationOrderingReason =
            'supporting-throttled-snapshot-order-only; exact-animation-edges-own-verdict'
        presentationSequenceGap = $presentationSequenceGap
        overlapStartSequence = $overlapStartSequence
        overlapEndSequence = $overlapEndSequence
        overlapStartUtc = $overlapStartUtc
        overlapEndUtc = $overlapEndUtc
        overlapMilliseconds = $overlapMilliseconds
        lastWorldSequence = [pscustomobject]@{
            host = [long]$cursors.host
            join = [long]$cursors.join
        }
        settleMilliseconds = $SettleMilliseconds
        observationTimeoutSeconds = $observationTimeoutSec
    }
}

function Invoke-LongMoveSourceRouteControl {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][object]$Ready,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [ValidateSet('host', 'join')]
        [string]$Role = 'join',
        [ValidateSet('longMove', 'cleanLongMove')]
        [string]$TargetProperty = 'longMove',
        [ValidateSet(
            'source-route-control',
            'clean-host-route-control',
            'clean-join-route-control'
        )]
        [string]$Case = 'source-route-control',
        [switch]$StrictCleanRoute,
        [int]$TimeoutSec = 120
    )
    $expectedCleanCase = "clean-$Role-route-control"
    if ($Case -eq 'source-route-control') {
        if ($Role -ne 'join' -or $TargetProperty -ne 'longMove' -or
            $StrictCleanRoute) {
            throw 'source-route-control contract is exactly join + longMove + diagnostic'
        }
    } elseif ($Case -ne $expectedCleanCase -or
        $TargetProperty -ne 'cleanLongMove' -or -not $StrictCleanRoute) {
        throw "$Case contract must be exactly $Role + cleanLongMove + strict"
    }
    $idleRole = if ($Role -eq 'host') { 'join' } else { 'host' }
    $spec = $Fixture.$Role
    $targetMember = $spec.PSObject.Properties[$TargetProperty]
    if (-not $targetMember -or -not $targetMember.Value) {
        throw "$Case has no $Role fixture target '$TargetProperty'"
    }
    $start = $Baseline.$Role
    $target = $targetMember.Value
    [int]$requestedDistance = [Math]::Max(
        [Math]::Abs([int]$target.x - [int]$start.x),
        [Math]::Abs([int]$target.y - [int]$start.y))
    if ([string]$start.id -ne [string]$spec.heroId -or
        [int]$start.x -ne [int]$spec.deploy.x -or
        [int]$start.y -ne [int]$spec.deploy.y -or
        [int]$start.movement -lt 30) {
        $label = if ($Case -eq 'source-route-control') {
            'source-route control'
        } else {
            $Case
        }
        throw ("$label did not start from the exact fresh $Role fixture: " +
            "$($start.id)@($($start.x),$($start.y)) MP=$($start.movement)")
    }
    if ($StrictCleanRoute -and $requestedDistance -lt 5) {
        throw "$Case fixture target is not a distant route: distance=$requestedDistance"
    }

    $marks = Get-CanonicalWorldWatermarks
    if ($Case -eq 'source-route-control') {
        Write-Step ("long-move source-route control: join only ({0},{1})->({2},{3}); host idle" -f
            [int]$start.x, [int]$start.y, [int]$target.x, [int]$target.y)
    } else {
        Write-Step ("long-move ${Case}: $Role only ({0},{1})->({2},{3}); $idleRole idle" -f
            [int]$start.x, [int]$start.y, [int]$target.x, [int]$target.y)
    }
    $submitted = Move-Stack $Role ([string]$spec.heroId) `
        ([int]$start.x) ([int]$start.y) ([int]$target.x) ([int]$target.y) `
        ([long]$Ready.preparation.$Role.instance) `
        ([long]$Ready.preparation.$Role.appearance)
    if (-not $submitted) {
        throw "long-move $Case native submission was rejected"
    }
    $terminalCompletion = if ($StrictCleanRoute) {
        Wait-CleanLongMoveCompletion `
            -Ready $Ready -Roles @($Role) `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -TimeoutSec $TimeoutSec -PollMilliseconds 100
    } else {
        $null
    }

    $heroId = [string]$spec.heroId
    $idleHeroId = [string]$Fixture.$idleRole.heroId
    [long]$cursor = [long]$marks.host
    [int]$lastX = [int]$start.x
    [int]$lastY = [int]$start.y
    [int]$lastMovement = [int]$start.movement
    [int]$lastIdleX = [int]$Baseline.$idleRole.x
    [int]$lastIdleY = [int]$Baseline.$idleRole.y
    [int]$lastIdleMovement = [int]$Baseline.$idleRole.movement
    [int]$worldEventsObserved = 0
    [int]$worldChangesObserved = 0
    $firstChangeUtc = $null
    $lastChangeUtc = $null
    $lastAnyChangeUtc = $null
    $settled = $false
    [int]$settleMilliseconds = 1200
    # The world reporter publishes at roughly 500 ms. Polling its append-only
    # history at 250 ms observes the source 1200 ms quiet edge; this loop never
    # submits, repeats, or replaces the one command above.
    [int]$observationTimeoutSec = [Math]::Min($TimeoutSec, 12)
    $observationStartedUtc = [DateTime]::UtcNow
    $observationDeadlineUtc = $observationStartedUtc.AddSeconds(
        $observationTimeoutSec)
    while ([DateTime]::UtcNow -lt $observationDeadlineUtc) {
        Assert-ClientsLive $HostProcess $JoinProcess
        $history = Get-WorldHistory host $cursor
        foreach ($event in @($history.events | Sort-Object seq)) {
            [long]$sequence = Get-OptionalProperty $event 'seq'
            if ($sequence -le $cursor -or $sequence -gt [uint32]::MaxValue) {
                throw "$Case host world history repeated/regressed at seq=$sequence after $cursor"
            }
            $cursor = $sequence
            $worldEventsObserved++
            $eventUtc = Get-SimturnsEvidenceUtc $event `
                "$Case authoritative world evidence"
            $hero = Get-WorldStackExact $event $heroId
            [int]$x = $hero.x
            [int]$y = $hero.y
            [int]$movement = $hero.movement
            if ($x -ne $lastX -or $y -ne $lastY -or
                $movement -ne $lastMovement) {
                if ($null -eq $firstChangeUtc) {
                    $firstChangeUtc = $eventUtc
                }
                $lastChangeUtc = $eventUtc
                $lastAnyChangeUtc = $eventUtc
                $worldChangesObserved++
            }
            $lastX = $x
            $lastY = $y
            $lastMovement = $movement
            if ($StrictCleanRoute) {
                $idleHero = Get-WorldStackExact $event $idleHeroId
                if ([int]$idleHero.x -ne $lastIdleX -or
                    [int]$idleHero.y -ne $lastIdleY -or
                    [int]$idleHero.movement -ne $lastIdleMovement) {
                    $lastAnyChangeUtc = $eventUtc
                }
                $lastIdleX = [int]$idleHero.x
                $lastIdleY = [int]$idleHero.y
                $lastIdleMovement = [int]$idleHero.movement
            }
        }
        $quietEdgeUtc = if ($StrictCleanRoute) {
            $lastAnyChangeUtc
        } else {
            $lastChangeUtc
        }
        if ($null -ne $quietEdgeUtc -and
            ([DateTime]::UtcNow - [DateTime]$quietEdgeUtc).
                TotalMilliseconds -ge $settleMilliseconds) {
            $settled = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    if ($null -eq $lastChangeUtc -and
        ([DateTime]::UtcNow - $observationStartedUtc).
            TotalMilliseconds -ge $settleMilliseconds) {
        # A full no-effect observation is also a stable diagnostic outcome.
        $settled = $true
    }

    # Allow only passive reporter convergence after the authoritative quiet
    # edge. No command is authorized by a late or missing publication.
    $convergenceDeadlineUtc = [DateTime]::UtcNow.AddSeconds(5)
    $converged = $false
    $authoritativeWorld = $null
    $replicatedWorld = $null
    do {
        Assert-ClientsLive $HostProcess $JoinProcess
        $authoritativeWorld = Get-World host
        $replicatedWorld = Get-World join
        $converged = $true
        foreach ($side in @('host', 'join')) {
            $sideHeroId = [string]$Fixture.$side.heroId
            $authoritativeHero = Get-WorldStackExact `
                $authoritativeWorld $sideHeroId
            $replicatedHero = Get-WorldStackExact `
                $replicatedWorld $sideHeroId
            if ([int]$authoritativeHero.x -ne [int]$replicatedHero.x -or
                [int]$authoritativeHero.y -ne [int]$replicatedHero.y -or
                [int]$authoritativeHero.movement -ne
                    [int]$replicatedHero.movement) {
                $converged = $false
            }
        }
        if (-not $converged) {
            Start-Sleep -Milliseconds 250
        }
    } while (-not $converged -and
        [DateTime]::UtcNow -lt $convergenceDeadlineUtc)

    $finalActive = Get-WorldStackExact $authoritativeWorld $heroId
    $finalIdle = Get-WorldStackExact $authoritativeWorld $idleHeroId
    $positionChanged = [int]$finalActive.x -ne [int]$start.x -or
        [int]$finalActive.y -ne [int]$start.y
    [int]$movementSpent = [int]$start.movement - [int]$finalActive.movement
    $idleWorldChanged = [int]$finalIdle.x -ne [int]$Baseline.$idleRole.x -or
        [int]$finalIdle.y -ne [int]$Baseline.$idleRole.y -or
        [int]$finalIdle.movement -ne [int]$Baseline.$idleRole.movement
    $classification = if ($positionChanged -and $movementSpent -gt 0) {
        'moved-and-charged'
    } elseif (-not $positionChanged -and $movementSpent -gt 0) {
        'charged-without-displacement'
    } elseif ($positionChanged) {
        'moved-without-charge'
    } else {
        'no-world-effect'
    }
    $battleObserved = $false
    $popupObserved = $false
    $mapsReady = $true
    $unexpectedDialogs = @()
    foreach ($side in @('host', 'join')) {
        $history = Get-UiHistory $side ([long]$Ready.preparation.$side.uiAfter)
        foreach ($event in @($history.events)) {
            $dialog = [string](Get-OptionalProperty $event 'dialog')
            if ($dialog -eq 'DLG_BATTLE_A') {
                $battleObserved = $true
            } elseif ($StrictCleanRoute -and
                -not [string]::IsNullOrWhiteSpace($dialog) -and
                $script:BareMapDialogs -notcontains $dialog) {
                $popupObserved = $true
                $unexpectedDialogs += "$side`:$dialog"
            }
        }
        if ($StrictCleanRoute) {
            $state = Get-RoleState $side
            $dialog = [string](Get-OptionalProperty $state 'dialog')
            if ($dialog -eq 'DLG_BATTLE_A') {
                $battleObserved = $true
            } elseif (-not [string]::IsNullOrWhiteSpace($dialog) -and
                $script:BareMapDialogs -notcontains $dialog) {
                $popupObserved = $true
                $unexpectedDialogs += "$side`:$dialog"
            }
            if (-not [bool](Get-OptionalProperty $state 'dialogReady') -or
                $script:BareMapDialogs -notcontains $dialog) {
                $mapsReady = $false
            }
        }
    }

    if ($Case -eq 'source-route-control') {
        # Keep the old permissive diagnostic and its structured result exact.
        # It classifies the bad source route and never requires its destination.
        $finalJoin = $finalActive
        $finalHost = $finalIdle
        $hostHeroId = $idleHeroId
        $hostWorldChanged = $idleWorldChanged
        $routeValid = $positionChanged -and $movementSpent -gt 0 -and
            $settled -and $converged -and -not $battleObserved -and
            -not $hostWorldChanged
        $completed = $settled -and $converged
        Write-Step ("long-move source-route control COMPLETE (diagnostic only): " +
            "classification=$classification join=($($start.x),$($start.y))->" +
            "($($finalJoin.x),$($finalJoin.y)) MP=$($start.movement)->$($finalJoin.movement) " +
            "settled=$settled converged=$converged battleObserved=$battleObserved " +
            "hostWorldChanged=$hostWorldChanged routeValid=$routeValid")
        return [pscustomobject]@{
            passed = $completed
            completed = $completed
            case = 'source-route-control'
            acceptanceClaim = $false
            sourceScript = 'long_move_sim.ps1'
            attemptedActions = 1
            acceptedActions = 1
            nativeSubmissionsSucceeded = $true
            worldApplicationsProved = ($worldChangesObserved -gt 0)
            sourceRouteValidForPositiveMovement = $routeValid
            classification = $classification
            positionChanged = $positionChanged
            chargedWithoutDisplacement = (-not $positionChanged -and $movementSpent -gt 0)
            movementSpent = $movementSpent
            battleObserved = $battleObserved
            hostWorldChanged = $hostWorldChanged
            converged = $converged
            settled = $settled
            worldEventsObserved = $worldEventsObserved
            worldChangesObserved = $worldChangesObserved
            firstChangeUtc = $firstChangeUtc
            lastChangeUtc = $lastChangeUtc
            observationTimeoutSeconds = $observationTimeoutSec
            recoveryActions = 0
            target = [pscustomobject]@{ x = [int]$target.x; y = [int]$target.y }
            join = [pscustomobject]@{
                heroId = $heroId
                fromX = [int]$start.x
                fromY = [int]$start.y
                x = [int]$finalJoin.x
                y = [int]$finalJoin.y
                movementBefore = [int]$start.movement
                movement = [int]$finalJoin.movement
            }
            host = [pscustomobject]@{
                heroId = $hostHeroId
                x = [int]$finalHost.x
                y = [int]$finalHost.y
                movement = [int]$finalHost.movement
            }
            settleMilliseconds = $settleMilliseconds
        }
    }

    $targetReached = [int]$finalActive.x -eq [int]$target.x -and
        [int]$finalActive.y -eq [int]$target.y
    $strictFailureReasons = @()
    if (-not $targetReached) { $strictFailureReasons += 'target not reached' }
    if ($movementSpent -lt 30 -or [int]$finalActive.movement -lt 0) {
        $strictFailureReasons += "movement spend is $movementSpent"
    }
    if ($idleWorldChanged) { $strictFailureReasons += "$idleRole hero changed" }
    if (-not $settled) { $strictFailureReasons += 'world did not settle' }
    if (-not $converged) { $strictFailureReasons += 'world views did not converge' }
    if ($battleObserved) { $strictFailureReasons += 'battle appeared' }
    if ($popupObserved) {
        $strictFailureReasons +=
            "popup/non-map dialog appeared: $($unexpectedDialogs -join ',')"
    }
    if (-not $mapsReady) {
        $strictFailureReasons += 'clients did not finish on ready bare maps'
    }
    Assert-ClientsLive $HostProcess $JoinProcess
    if ($strictFailureReasons.Count -ne 0) {
        throw ("long-move $Case failed its passive route oracle: " +
            ($strictFailureReasons -join '; '))
    }

    $finalByRole = @{
        $Role = $finalActive
        $idleRole = $finalIdle
    }
    $outcomes = [ordered]@{}
    foreach ($side in @('host', 'join')) {
        $hero = $finalByRole[$side]
        $outcomes[$side] = [pscustomobject]@{
            heroId = [string]$Fixture.$side.heroId
            fromX = [int]$Baseline.$side.x
            fromY = [int]$Baseline.$side.y
            x = [int]$hero.x
            y = [int]$hero.y
            movementBefore = [int]$Baseline.$side.movement
            movement = [int]$hero.movement
            movementSpent = [int]$Baseline.$side.movement - [int]$hero.movement
            active = ($side -eq $Role)
        }
    }
    Write-Step ("long-move $Case PASS: $Role reached ($($target.x),$($target.y)) " +
        "MP=$($start.movement)->$($finalActive.movement); $idleRole idle; " +
        "settled=$settled converged=$converged")
    return [pscustomobject]@{
        passed = $true
        completed = $true
        case = $Case
        acceptanceClaim = $false
        strictSingleRouteClaim = $true
        attemptedActions = 1
        acceptedActions = 1
        nativeSubmissionsSucceeded = $true
        worldApplicationsProved = ($worldChangesObserved -gt 0)
        recoveryActions = 0
        terminalCompletion = $terminalCompletion
        activeRole = $Role
        idleRole = $idleRole
        targetReached = $targetReached
        movementSpent = $movementSpent
        battleObserved = $battleObserved
        popupObserved = $popupObserved
        idleWorldChanged = $idleWorldChanged
        converged = $converged
        settled = $settled
        mapsReady = $mapsReady
        worldEventsObserved = $worldEventsObserved
        worldChangesObserved = $worldChangesObserved
        firstChangeUtc = $firstChangeUtc
        lastChangeUtc = $lastChangeUtc
        observationTimeoutSeconds = $observationTimeoutSec
        settleMilliseconds = $settleMilliseconds
        target = [pscustomobject]@{ x = [int]$target.x; y = [int]$target.y }
        host = $outcomes.host
        join = $outcomes.join
    }
}

function Observe-LongMoveSourcePair {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][object]$Preparation,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][object]$Fire,
        [Parameter(Mandatory)][long]$After,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [int]$TimeoutSec = 120
    )
    $states = @{}
    foreach ($side in @('host', 'join')) {
        $fireResponse = @($Fire.responses | Where-Object {
            [string]$_.role -eq $side
        })
        if ($fireResponse.Count -ne 1 -or [long]$fireResponse[0].startedMs -lt 1) {
            throw "$side source-pair repro omitted its unique CommandStarted edge"
        }
        $states[$side] = [pscustomobject]@{
            side = $side
            heroId = [string]$Fixture.$side.heroId
            causalStartUtc = [DateTimeOffset]::FromUnixTimeMilliseconds(
                [long]$fireResponse[0].startedMs).UtcDateTime
            firstPositionChangeUtc = $null
            lastPositionChangeUtc = $null
            firstWorldChangeUtc = $null
            lastWorldChangeUtc = $null
            positionChanges = 0
            worldChanges = 0
            lastX = [int]$Baseline.$side.x
            lastY = [int]$Baseline.$side.y
            lastMovement = [int]$Baseline.$side.movement
        }
    }

    [int]$settleMilliseconds = 1200
    [int]$observationTimeoutSec = [Math]::Min($TimeoutSec, 12)
    $observationStartedUtc = [DateTime]::UtcNow
    $observationDeadlineUtc = $observationStartedUtc.AddSeconds(
        $observationTimeoutSec)
    [long]$cursor = $After
    [int]$worldEventsObserved = 0
    while ([DateTime]::UtcNow -lt $observationDeadlineUtc) {
        Assert-ClientsLive $HostProcess $JoinProcess
        $history = Get-WorldHistory host $cursor
        foreach ($event in @($history.events | Sort-Object seq)) {
            [long]$sequence = Get-OptionalProperty $event 'seq'
            if ($sequence -le $cursor -or $sequence -gt [uint32]::MaxValue) {
                throw "source-pair host world history repeated/regressed at seq=$sequence after $cursor"
            }
            $cursor = $sequence
            $worldEventsObserved++
            $eventUtc = Get-SimturnsEvidenceUtc $event `
                'source-pair authoritative world evidence'
            foreach ($side in @('host', 'join')) {
                $state = $states[$side]
                $hero = Get-WorldStackExact $event ([string]$state.heroId)
                [int]$x = $hero.x
                [int]$y = $hero.y
                [int]$movement = $hero.movement
                $positionChanged = $x -ne [int]$state.lastX -or
                    $y -ne [int]$state.lastY
                $worldChanged = $positionChanged -or
                    $movement -ne [int]$state.lastMovement
                if ($positionChanged) {
                    if ($null -eq $state.firstPositionChangeUtc) {
                        $state.firstPositionChangeUtc = $eventUtc
                    }
                    $state.lastPositionChangeUtc = $eventUtc
                    $state.positionChanges = [int]$state.positionChanges + 1
                }
                if ($worldChanged) {
                    if ($null -eq $state.firstWorldChangeUtc) {
                        $state.firstWorldChangeUtc = $eventUtc
                    }
                    $state.lastWorldChangeUtc = $eventUtc
                    $state.worldChanges = [int]$state.worldChanges + 1
                }
                $state.lastX = $x
                $state.lastY = $y
                $state.lastMovement = $movement
            }
        }

        if ($null -ne $states.host.firstPositionChangeUtc -and
            $null -ne $states.join.firstPositionChangeUtc) {
            $latestPositionChangeUtc = if (
                [DateTime]$states.host.lastPositionChangeUtc -gt
                    [DateTime]$states.join.lastPositionChangeUtc) {
                [DateTime]$states.host.lastPositionChangeUtc
            } else {
                [DateTime]$states.join.lastPositionChangeUtc
            }
            if (([DateTime]::UtcNow - $latestPositionChangeUtc).
                    TotalMilliseconds -ge $settleMilliseconds) {
                break
            }
        }
        Start-Sleep -Milliseconds 250
    }

    $settled = @{}
    foreach ($side in @('host', 'join')) {
        $lastChangeUtc = $states[$side].lastWorldChangeUtc
        $settled[$side] = if ($null -eq $lastChangeUtc) {
            ([DateTime]::UtcNow - $observationStartedUtc).
                TotalMilliseconds -ge $settleMilliseconds
        } else {
            ([DateTime]::UtcNow - [DateTime]$lastChangeUtc).
                TotalMilliseconds -ge $settleMilliseconds
        }
    }

    $convergenceDeadlineUtc = [DateTime]::UtcNow.AddSeconds(5)
    $converged = $false
    $authoritativeWorld = $null
    $replicatedWorld = $null
    do {
        Assert-ClientsLive $HostProcess $JoinProcess
        $authoritativeWorld = Get-World host
        $replicatedWorld = Get-World join
        $converged = $true
        foreach ($side in @('host', 'join')) {
            $heroId = [string]$Fixture.$side.heroId
            $authoritativeHero = Get-WorldStackExact $authoritativeWorld $heroId
            $replicatedHero = Get-WorldStackExact $replicatedWorld $heroId
            if ([int]$authoritativeHero.x -ne [int]$replicatedHero.x -or
                [int]$authoritativeHero.y -ne [int]$replicatedHero.y -or
                [int]$authoritativeHero.movement -ne
                    [int]$replicatedHero.movement) {
                $converged = $false
            }
        }
        if (-not $converged) {
            Start-Sleep -Milliseconds 250
        }
    } while (-not $converged -and
        [DateTime]::UtcNow -lt $convergenceDeadlineUtc)

    $outcomes = [ordered]@{}
    foreach ($side in @('host', 'join')) {
        $start = $Baseline.$side
        $hero = Get-WorldStackExact $authoritativeWorld `
            ([string]$Fixture.$side.heroId)
        $positionChanged = [int]$hero.x -ne [int]$start.x -or
            [int]$hero.y -ne [int]$start.y
        [int]$movementSpent = [int]$start.movement - [int]$hero.movement
        [int]$distance = [Math]::Max(
            [Math]::Abs([int]$hero.x - [int]$start.x),
            [Math]::Abs([int]$hero.y - [int]$start.y))
        $classification = if ($positionChanged -and $movementSpent -gt 0) {
            'moved-and-charged'
        } elseif (-not $positionChanged -and $movementSpent -gt 0) {
            'charged-without-displacement'
        } elseif ($positionChanged) {
            'moved-without-charge'
        } else {
            'no-world-effect'
        }
        $outcomes[$side] = [pscustomobject]@{
            heroId = [string]$Fixture.$side.heroId
            fromX = [int]$start.x
            fromY = [int]$start.y
            x = [int]$hero.x
            y = [int]$hero.y
            movementBefore = [int]$start.movement
            movement = [int]$hero.movement
            movementSpent = $movementSpent
            distance = $distance
            positionChanged = $positionChanged
            classification = $classification
            positionChangesObserved = [int]$states[$side].positionChanges
            worldChangesObserved = [int]$states[$side].worldChanges
            firstPositionChangeUtc = $states[$side].firstPositionChangeUtc
            lastPositionChangeUtc = $states[$side].lastPositionChangeUtc
            causalStartUtc = $states[$side].causalStartUtc
            settled = [bool]$settled[$side]
        }
    }

    $battleObserved = @{}
    foreach ($side in @('host', 'join')) {
        $history = Get-UiHistory $side ([long]$Preparation.$side.uiAfter)
        $battleObserved[$side] = @($history.events | Where-Object {
                [string](Get-OptionalProperty $_ 'dialog') -eq 'DLG_BATTLE_A'
            }).Count -gt 0
    }

    $overlapProved = $false
    $overlapReason = 'one-or-both-heroes-produced-no-position-interval'
    $overlapMilliseconds = $null
    if ([int]$states.host.positionChanges -ge 2 -and
        [int]$states.join.positionChanges -ge 2) {
        $overlapStart = if (
            [DateTime]$states.host.firstPositionChangeUtc -gt
                [DateTime]$states.join.firstPositionChangeUtc) {
            [DateTime]$states.host.firstPositionChangeUtc
        } else {
            [DateTime]$states.join.firstPositionChangeUtc
        }
        $overlapEnd = if (
            [DateTime]$states.host.lastPositionChangeUtc -lt
                [DateTime]$states.join.lastPositionChangeUtc) {
            [DateTime]$states.host.lastPositionChangeUtc
        } else {
            [DateTime]$states.join.lastPositionChangeUtc
        }
        $overlapProved = $overlapStart -lt $overlapEnd
        $overlapReason = if ($overlapProved) {
            'positive-logical-position-publication-overlap'
        } else {
            'logical-position-publication-intervals-serialized'
        }
        $overlapMilliseconds = ($overlapEnd - $overlapStart).TotalMilliseconds
    }

    $sourceArtifactMatched =
        [int]$outcomes.host.x -eq 23 -and [int]$outcomes.host.y -eq 18 -and
        [int]$outcomes.host.movement -eq 1 -and
        [int]$outcomes.join.x -eq 15 -and [int]$outcomes.join.y -eq 27 -and
        [int]$outcomes.join.movement -eq 32 -and
        [bool]$battleObserved.host
    $completed = $converged -and [bool]$settled.host -and
        [bool]$settled.join
    Write-Step ("long-move source-pair repro COMPLETE (diagnostic only): " +
        "host=$($outcomes.host.classification) " +
        "($($outcomes.host.x),$($outcomes.host.y)) MP=$($outcomes.host.movement); " +
        "join=$($outcomes.join.classification) " +
        "($($outcomes.join.x),$($outcomes.join.y)) MP=$($outcomes.join.movement); " +
        "battle(host/join)=$($battleObserved.host)/$($battleObserved.join); " +
        "sourceArtifactMatched=$sourceArtifactMatched")
    return [pscustomobject]@{
        passed = $completed
        completed = $completed
        case = 'source-pair-repro'
        acceptanceClaim = $false
        sourceScript = 'long_move_sim.ps1'
        sourceHadMachineVerdict = $false
        attemptedActions = 2
        acceptedActions = @($Fire.responses | Where-Object found).Count
        recoveryActions = 0
        dispatchSkewMs = [double]$Fire.dispatchSkewMs
        converged = $converged
        settled = [pscustomobject]$settled
        host = $outcomes.host
        join = $outcomes.join
        battleObserved = [pscustomobject]$battleObserved
        overlapProved = $overlapProved
        overlapReason = $overlapReason
        overlapMilliseconds = $overlapMilliseconds
        sourceArtifactMatched = $sourceArtifactMatched
        worldEventsObserved = $worldEventsObserved
        lastWorldSequence = $cursor
        observationTimeoutSeconds = $observationTimeoutSec
        settleMilliseconds = $settleMilliseconds
    }
}

function Assert-NoLongMoveBattle {
    param(
        [Parameter(Mandatory)][object]$Preparation,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess
    )
    foreach ($side in @('host', 'join')) {
        $history = Get-UiHistory $side ([long]$Preparation.$side.uiAfter)
        $battleEvents = @($history.events | Where-Object {
            [string](Get-OptionalProperty $_ 'dialog') -eq 'DLG_BATTLE_A'
        })
        $nonMapEvents = @($history.events | Where-Object {
            $dialog = [string](Get-OptionalProperty $_ 'dialog')
            -not [string]::IsNullOrWhiteSpace($dialog) -and
                $script:BareMapDialogs -notcontains $dialog
        })
        $state = Get-RoleState $side
        if ($battleEvents.Count -ne 0 -or
            [string](Get-OptionalProperty $state 'dialog') -eq 'DLG_BATTLE_A') {
            throw "$side long-move unexpectedly entered a battle"
        }
        if ($nonMapEvents.Count -ne 0) {
            $dialogs = @($nonMapEvents | ForEach-Object {
                [string](Get-OptionalProperty $_ 'dialog')
            } | Select-Object -Unique)
            throw ("$side long-move crossed a non-map dialog/event: " +
                ($dialogs -join ','))
        }
        if (-not [bool](Get-OptionalProperty $state 'dialogReady') -or
            $script:BareMapDialogs -notcontains
                [string](Get-OptionalProperty $state 'dialog')) {
            throw "$side long-move did not finish on one ready bare map"
        }
    }
    Assert-ClientsLive $HostProcess $JoinProcess
}

function Assert-LongMoveFinalWorlds {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][object]$Baseline
    )
    $worlds = @{
        host = Get-World host
        join = Get-World join
    }
    $result = [ordered]@{}
    foreach ($side in @('host', 'join')) {
        $hostView = Get-WorldStackExact $worlds.host ([string]$Fixture.$side.heroId)
        $joinView = Get-WorldStackExact $worlds.join ([string]$Fixture.$side.heroId)
        $targetProperty = $Fixture.$side.PSObject.Properties['cleanLongMove']
        if (-not $targetProperty -or -not $targetProperty.Value) {
            throw "$side clean long-move final oracle has no pinned target"
        }
        $target = $targetProperty.Value
        if ([int]$hostView.x -ne [int]$joinView.x -or
            [int]$hostView.y -ne [int]$joinView.y -or
            [int]$hostView.movement -ne [int]$joinView.movement) {
            throw "$side long-move final state did not converge across both clients"
        }
        if ([int]$hostView.x -ne [int]$target.x -or
            [int]$hostView.y -ne [int]$target.y) {
            throw ("$side long-move did not reach its exact cleanLongMove target: " +
                "actual=($($hostView.x),$($hostView.y)) " +
                "expected=($($target.x),$($target.y))")
        }
        if ($null -eq $target.PSObject.Properties['movement'] -or
            [int]$hostView.movement -ne [int]$target.movement) {
            throw ("$side long-move has wrong final movement points (cross-billing or " +
                "route drift): actual=$($hostView.movement) expected=$($target.movement)")
        }
        [int]$distance = [Math]::Max(
            [Math]::Abs([int]$hostView.x - [int]$Baseline.$side.x),
            [Math]::Abs([int]$hostView.y - [int]$Baseline.$side.y))
        [int]$movementSpent = [int]$Baseline.$side.movement -
            [int]$hostView.movement
        if ($distance -lt 2 -or [int]$hostView.movement -lt 0 -or
            $movementSpent -lt 30) {
            throw ("$side long-move was not long/charged: distance=$distance movement=" +
                "$($Baseline.$side.movement)->$($hostView.movement)")
        }
        $result[$side] = [pscustomobject]@{
            heroId = [string]$Fixture.$side.heroId
            fromX = [int]$Baseline.$side.x
            fromY = [int]$Baseline.$side.y
            x = [int]$hostView.x
            y = [int]$hostView.y
            movementBefore = [int]$Baseline.$side.movement
            movement = [int]$hostView.movement
            expectedMovement = [int]$target.movement
            movementSpent = $movementSpent
            distance = $distance
        }
    }
    return [pscustomobject]$result
}

function Invoke-LongMoveProof {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [Parameter(Mandatory)][string]$HostLog,
        [Parameter(Mandatory)][string]$JoinLog,
        [Parameter(Mandatory)]
        [ValidateSet(
            'source-route-control',
            'source-pair-repro',
            'clean-host-route-control',
            'clean-join-route-control',
            'clean-long-concurrency'
        )]
        [string]$Case,
        [int]$TimeoutSec = 120
    )
    $ready = Wait-LegacyReadyQuietPair `
        -HostLog $HostLog -JoinLog $JoinLog `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -QuietSec 3 -TimeoutSec 45 -PollMilliseconds 800
    $baseline = Get-LegacySequentialSharedMovementSnapshot $Fixture
    if ($Case -eq 'source-route-control') {
        return Invoke-LongMoveSourceRouteControl `
            -Fixture $Fixture -Ready $ready -Baseline $baseline `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -TimeoutSec $TimeoutSec
    }
    if ($Case -eq 'clean-host-route-control') {
        return Invoke-LongMoveSourceRouteControl `
            -Fixture $Fixture -Ready $ready -Baseline $baseline `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -Role host -TargetProperty cleanLongMove -Case $Case `
            -StrictCleanRoute -TimeoutSec $TimeoutSec
    }
    if ($Case -eq 'clean-join-route-control') {
        return Invoke-LongMoveSourceRouteControl `
            -Fixture $Fixture -Ready $ready -Baseline $baseline `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -Role join -TargetProperty cleanLongMove -Case $Case `
            -StrictCleanRoute -TimeoutSec $TimeoutSec
    }
    $actions = @()
    foreach ($side in @('host', 'join')) {
        $spec = $Fixture.$side
        $start = $baseline.$side
        $target = if ($Case -eq 'source-pair-repro') {
            $spec.longMove
        } else {
            $cleanTargetProperty = $spec.PSObject.Properties['cleanLongMove']
            if (-not $cleanTargetProperty -or -not $cleanTargetProperty.Value) {
                throw ("$side clean-long-concurrency has no proved cleanLongMove " +
                    'fixture target; historical longMove is diagnostic-only')
            }
            $cleanTargetProperty.Value
        }
        [int]$requestedDistance = [Math]::Max(
            [Math]::Abs([int]$target.x - [int]$start.x),
            [Math]::Abs([int]$target.y - [int]$start.y))
        if ([string]$start.id -ne [string]$spec.heroId -or
            [int]$start.x -ne [int]$spec.deploy.x -or
            [int]$start.y -ne [int]$spec.deploy.y -or
            [int]$start.movement -lt 30 -or $requestedDistance -lt 5) {
            throw ("$side long-move did not start from the exact fresh fixture state: " +
                "$($start.id)@($($start.x),$($start.y)) MP=$($start.movement), " +
                "requestedDistance=$requestedDistance")
        }
        $actions += [pscustomobject]@{
            role = $side
            id = [string]$spec.heroId
            fromX = [int]$start.x
            fromY = [int]$start.y
            x = [int]$target.x
            y = [int]$target.y
            appearance = [long]$ready.preparation.$side.appearance
            instance = [long]$ready.preparation.$side.instance
        }
    }

    Write-Step ("long-move ${Case}: submitting exactly one fixed distant move per hero")
    $fire = Invoke-PreparedParallelAttackMoves $actions -BackToBackLongMove
    if ($Case -eq 'source-pair-repro') {
        return Observe-LongMoveSourcePair `
            -Fixture $Fixture -Preparation $ready.preparation `
            -Baseline $baseline -Fire $fire `
            -After ([long]$ready.preparation.host.worldAfter) `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -TimeoutSec $TimeoutSec
    }
    $terminalCompletion = Wait-CleanLongMoveCompletion `
        -Ready $ready -Roles @('host', 'join') `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -TimeoutSec $TimeoutSec -PollMilliseconds 100
    $strategicBusyOverlap = Assert-CleanLongMoveBusyOverlap `
        -TerminalCompletion $terminalCompletion -Fire $fire
    $trajectory = Wait-LongMoveTrajectoryEvidence `
        -Fixture $Fixture -Baseline $baseline -Fire $fire `
        -After ([pscustomobject]@{
            host = [long]$ready.preparation.host.worldAfter
            join = [long]$ready.preparation.join.worldAfter
        }) `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -TimeoutSec $TimeoutSec -SettleMilliseconds 1200
    Assert-NoLongMoveBattle $ready.preparation $HostProcess $JoinProcess
    $final = Assert-LongMoveFinalWorlds $Fixture $baseline
    Write-Step (("long-move PASS: sampled world relation={0} (diagnostic only); " +
        "supporting strategic-busy overlap={1:N1} ms; " +
        "host distance/MP={2}/{3}; join distance/MP={4}/{5}; " +
        "world publications={6}/{7}; CommandStarted receipt skew={8:N1} ms") -f
        [string]$trajectory.worldPublicationRelation,
        [double]$strategicBusyOverlap.overlapMilliseconds,
        [int]$final.host.distance, [int]$final.host.movement,
        [int]$final.join.distance, [int]$final.join.movement,
        [int]$trajectory.host.changes, [int]$trajectory.join.changes,
        [double]$fire.dispatchSkewMs)
    return [pscustomobject]@{
        passed = $true
        case = 'clean-long-concurrency'
        acceptanceClaim = $true
        sourceScript = 'long_move_sim.ps1'
        sourceHadMachineVerdict = $false
        strictReplacementOracle =
            'one back-to-back accepted pair, both exact targets with pinned per-hero MP, supporting sampled world order and strategic-busy overlap, 1200ms settle, no battle/popup, converged views; stock CMidCommandQueue2 presentation order is diagnostic'
        attemptedActions = 2
        acceptedActions = @($fire.responses | Where-Object found).Count
        recoveryActions = 0
        dispatchSkewMs = [double]$fire.dispatchSkewMs
        dispatchSkewKind = [string]$fire.dispatchSkewKind
        dispatchOrder = [string]$fire.dispatchOrder
        terminalCompletion = $terminalCompletion
        strategicBusyOverlap = $strategicBusyOverlap
        trajectory = $trajectory
        host = $final.host
        join = $final.join
    }
}

function Get-LongAttackMovementStart {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][string]$HeroId,
        [Parameter(Mandatory)][object]$Initial,
        [Parameter(Mandatory)][long]$After,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [int]$TimeoutSec = 15
    )
    $event = Wait-WorldEvidence `
        -Role $Role -After $After `
        -Description 'the first post-dispatch long-attack movement publication' `
        -TimeoutSec $TimeoutSec `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -Predicate {
            param($candidate)
            $hero = Get-WorldStackExact $candidate $HeroId
            [int]$hero.x -ne [int]$Initial.x -or
                [int]$hero.y -ne [int]$Initial.y -or
                [int]$hero.movement -ne [int]$Initial.movement
        }
    $hero = Get-WorldStackExact $event $HeroId
    return [pscustomobject]@{
        role = $Role
        sequence = [long]$event.seq
        utc = Get-SimturnsEvidenceUtc $event "$Role long-attack world evidence"
        x = [int]$hero.x
        y = [int]$hero.y
        movement = [int]$hero.movement
    }
}

function Test-LongAttackTerminalWorld([object]$World, [object]$Fixture) {
    if (-not $World) { return $false }
    foreach ($side in @('host', 'join')) {
        if (Get-WorldStackExact $World `
                ([string]$Fixture.$side.longAttack.target.id) -AllowMissing) {
            return $false
        }
        if (-not (Get-WorldStackExact $World `
                    ([string]$Fixture.$side.heroId) -AllowMissing)) {
            return $false
        }
    }
    return $true
}

function Wait-LongAttackWorldConvergence {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][hashtable]$After,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [int]$TimeoutSec = 120
    )
    # Battle-close is a UI edge. The engine can still be finishing its natural
    # post-battle object-map update, and the world reporter publishes at most
    # once per throttle interval. Consume only append-only world evidence from
    # the pre-fire watermark; never re-fire, continue, or otherwise act here.
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    foreach ($role in @('host', 'join')) {
        $remaining = [Math]::Max(
            1,
            [int][Math]::Ceiling(($deadline - (Get-Date)).TotalSeconds))
        [void](Wait-WorldEvidence `
            -Role $role -After ([long]$After[$role]) `
            -Description 'both pinned long-attack targets to disappear after battle close' `
            -TimeoutSec $remaining `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -Predicate {
                param($world)
                Test-LongAttackTerminalWorld $world $Fixture
            })
    }
    $worlds = @{
        host = Get-World host
        join = Get-World join
    }
    foreach ($role in @('host', 'join')) {
        if (-not (Test-LongAttackTerminalWorld $worlds[$role] $Fixture)) {
            throw "long-attack terminal world did not remain converged on $role"
        }
    }
    return $worlds
}

function Assert-LongAttackFinalWorlds {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][hashtable]$Worlds
    )
    foreach ($viewRole in @('host', 'join')) {
        foreach ($side in @('host', 'join')) {
            if (Get-WorldStackExact $Worlds[$viewRole] `
                    ([string]$Fixture.$side.longAttack.target.id) -AllowMissing) {
                throw "$viewRole still sees the exact $side long-attack target after both battles"
            }
        }
    }
    $result = [ordered]@{}
    foreach ($side in @('host', 'join')) {
        $ownView = Get-WorldStackExact $Worlds[$side] ([string]$Fixture.$side.heroId)
        $peerRole = if ($side -eq 'host') { 'join' } else { 'host' }
        $peerView = Get-WorldStackExact $Worlds[$peerRole] ([string]$Fixture.$side.heroId)
        if ([int]$ownView.x -ne [int]$peerView.x -or
            [int]$ownView.y -ne [int]$peerView.y -or
            [int]$ownView.movement -ne [int]$peerView.movement) {
            throw "$side long-attack hero did not converge across both clients"
        }
        [int]$distance = [Math]::Max(
            [Math]::Abs([int]$ownView.x - [int]$Baseline.$side.x),
            [Math]::Abs([int]$ownView.y - [int]$Baseline.$side.y))
        [int]$movementSpent = [int]$Baseline.$side.movement -
            [int]$ownView.movement
        if ($distance -lt 2 -or [int]$ownView.movement -lt 0 -or
            $movementSpent -lt 20) {
            throw ("$side long-attack was not long/charged: distance=$distance movement=" +
                "$($Baseline.$side.movement)->$($ownView.movement)")
        }
        $result[$side] = [pscustomobject]@{
            heroId = [string]$Fixture.$side.heroId
            targetId = [string]$Fixture.$side.longAttack.target.id
            x = [int]$ownView.x
            y = [int]$ownView.y
            movementBefore = [int]$Baseline.$side.movement
            movement = [int]$ownView.movement
            movementSpent = $movementSpent
            distance = $distance
            battleClosed = $true
            targetGoneOnBothClients = $true
        }
    }
    return [pscustomobject]$result
}

function Invoke-LongAttackProof {
    param(
        [Parameter(Mandatory)][object]$Fixture,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [Parameter(Mandatory)][string]$HostLog,
        [Parameter(Mandatory)][string]$JoinLog,
        [int]$TimeoutSec = 120
    )
    $ready = Wait-LegacyReadyQuietPair `
        -HostLog $HostLog -JoinLog $JoinLog `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -QuietSec 3 -TimeoutSec 45 -PollMilliseconds 800
    $baseline = Get-LegacySequentialSharedMovementSnapshot $Fixture
    $targetCensus = Get-LegacyStackSnapshot
    $actions = @()
    $specs = @{}
    foreach ($side in @('host', 'join')) {
        $spec = $Fixture.$side
        $start = $baseline.$side
        $target = $spec.longAttack.target
        $observedTarget = Get-WorldStackExact $targetCensus ([string]$target.id)
        [int]$requestedDistance = [Math]::Max(
            [Math]::Abs([int]$target.x - [int]$start.x),
            [Math]::Abs([int]$target.y - [int]$start.y))
        if ([string]$start.id -ne [string]$spec.heroId -or
            [int]$start.x -ne [int]$spec.deploy.x -or
            [int]$start.y -ne [int]$spec.deploy.y -or
            [int]$start.movement -lt 30 -or $requestedDistance -lt 3) {
            throw ("$side long-attack did not start from the exact fresh fixture state: " +
                "$($start.id)@($($start.x),$($start.y)) MP=$($start.movement), " +
                "requestedDistance=$requestedDistance")
        }
        if ([int]$observedTarget.x -ne [int]$target.x -or
            [int]$observedTarget.y -ne [int]$target.y -or
            [string]$observedTarget.owner -ne [string]$Fixture.players.neutralHandle) {
            throw ("$side exact long-attack target mismatch: " +
                "$($observedTarget.id)@($($observedTarget.x),$($observedTarget.y)) " +
                "owner=$($observedTarget.owner)")
        }
        $actions += [pscustomobject]@{
            role = $side
            id = [string]$spec.heroId
            fromX = [int]$start.x
            fromY = [int]$start.y
            x = [int]$target.x
            y = [int]$target.y
            appearance = [long]$ready.preparation.$side.appearance
            instance = [long]$ready.preparation.$side.instance
        }
        $specs[$side] = [pscustomobject]@{
            heroId = [string]$spec.heroId
            target = $target
        }
    }

    Write-Step 'long-attack: submitting exactly one fixed distant attack per hero'
    $fire = Invoke-PreparedParallelAttackMoves $actions
    $battleUi = @{}
    foreach ($side in @('host', 'join')) {
        $battleUi[$side] = Wait-UiEvidence `
            -Role $side -After ([long]$ready.preparation.$side.uiAfter) `
            -Description 'the first exact long-attack battle appearance' `
            -TimeoutSec $TimeoutSec `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -Predicate {
                param($event)
                [string](Get-OptionalProperty $event 'dialog') -eq 'DLG_BATTLE_A'
            }
    }
    $battleUtc = @{
        host = Get-SimturnsEvidenceUtc $battleUi.host 'host long-attack battle appearance'
        join = Get-SimturnsEvidenceUtc $battleUi.join 'join long-attack battle appearance'
    }
    $firstMove = @{
        host = Get-LongAttackMovementStart `
            host ([string]$Fixture.host.heroId) $baseline.host `
            ([long]$ready.preparation.host.worldAfter) `
            $HostProcess $JoinProcess
        join = Get-LongAttackMovementStart `
            join ([string]$Fixture.join.heroId) $baseline.join `
            ([long]$ready.preparation.join.worldAfter) `
            $HostProcess $JoinProcess
    }
    $firstMoveUtc = @{
        host = [DateTime]$firstMove.host.utc
        join = [DateTime]$firstMove.join.utc
    }
    $causalStartUtc = @{}
    foreach ($side in @('host', 'join')) {
        $response = @($fire.responses | Where-Object {
            [string]$_.role -eq $side
        })
        if ($response.Count -ne 1 -or [long]$response[0].startedMs -lt 1) {
            throw "$side long-attack omitted its unique native CommandStarted edge"
        }
        $causalStartUtc[$side] = [DateTimeOffset]::FromUnixTimeMilliseconds(
            [long]$response[0].startedMs).UtcDateTime
        if ([DateTime]$causalStartUtc[$side] -gt [DateTime]$firstMoveUtc[$side]) {
            throw "$side long-attack movement appeared before its native command edge"
        }
        if ([DateTime]$firstMoveUtc[$side] -gt [DateTime]$battleUtc[$side]) {
            throw "$side long-attack battle appeared before its first actual movement"
        }
    }
    # Source oracle: active attack window is [first actual movement, battle].
    # CommandStarted remains the causal one-shot admission proof, but is not a
    # substitute for physical movement when checking cross-player blocking.
    $overlapStart = if ([DateTime]$firstMoveUtc.host -gt
            [DateTime]$firstMoveUtc.join) {
        [DateTime]$firstMoveUtc.host
    } else {
        [DateTime]$firstMoveUtc.join
    }
    $overlapEnd = if ([DateTime]$battleUtc.host -lt [DateTime]$battleUtc.join) {
        [DateTime]$battleUtc.host
    } else {
        [DateTime]$battleUtc.join
    }
    if ($overlapStart -gt $overlapEnd) {
        throw ("long-attack serialized: host=[{0:O},{1:O}] join=[{2:O},{3:O}]" -f
            [DateTime]$firstMoveUtc.host, [DateTime]$battleUtc.host,
            [DateTime]$firstMoveUtc.join, [DateTime]$battleUtc.join)
    }
    [double]$firstMoveStartSkewMilliseconds = [Math]::Abs(
        (([DateTime]$firstMoveUtc.host - [DateTime]$firstMoveUtc.join).
            TotalMilliseconds))

    $completion = @{
        host = New-CanonicalBattleCompletionState `
            host $specs.host $battleUi.host -LegacyCloseEvidenceOnly
        join = New-CanonicalBattleCompletionState `
            join $specs.join $battleUi.join -LegacyCloseEvidenceOnly
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline -and
        (-not $completion.host.Done -or -not $completion.join.Done)) {
        Assert-ClientsLive $HostProcess $JoinProcess
        if (-not $completion.host.Done) {
            Step-CanonicalBattleCompletionState $completion.host
        }
        if (-not $completion.join.Done) {
            Step-CanonicalBattleCompletionState $completion.join
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $completion.host.Done -or -not $completion.join.Done) {
        throw 'long-attack timed out before both one-shot battles closed'
    }
    # long_attack_sim.ps1 RETURN entered this exact +3 second settle and then
    # wait_day_ready.ps1 quiet-3/30 s gate before any walk-back or End Turn.
    # Keep that source boundary even though the retrying diagnostic walk-back
    # is intentionally omitted: deterministic loot popups such as DLG_ITEM may
    # be published after battle-close and are consumed once by the native
    # scripted-popup subscriber while this code only observes its quiet edge.
    Start-Sleep -Seconds 3
    $postBattleReady = Wait-LegacyReadyQuietPair `
        -HostLog $HostLog -JoinLog $JoinLog `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -QuietSec 3 -TimeoutSec 30 -PollMilliseconds 800
    $terminalWorlds = Wait-LongAttackWorldConvergence `
        -Fixture $Fixture `
        -After @{
            host = [long]$ready.preparation.host.worldAfter
            join = [long]$ready.preparation.join.worldAfter
        } `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -TimeoutSec $TimeoutSec
    $final = Assert-LongAttackFinalWorlds $Fixture $baseline $terminalWorlds
    Assert-ClientsLive $HostProcess $JoinProcess
    Write-Step (("long-attack battle PASS: overlapping first-move-to-battle windows={0:N1} ms; " +
        "first-move skew={1:N1} ms; host distance/MP={2}/{3}; " +
        "join distance/MP={4}/{5}; dispatch skew={6:N1} ms") -f
        ($overlapEnd - $overlapStart).TotalMilliseconds,
        $firstMoveStartSkewMilliseconds,
        [int]$final.host.distance, [int]$final.host.movement,
        [int]$final.join.distance, [int]$final.join.movement,
        [double]$fire.dispatchSkewMs)
    return [pscustomobject]@{
        passed = $true
        sourceScript = 'long_attack_sim.ps1'
        attemptedActions = 2
        acceptedActions = @($fire.responses | Where-Object found).Count
        continuationMoveActions = 0
        dispatchSkewMs = [double]$fire.dispatchSkewMs
        host = $final.host
        join = $final.join
        firstMove = [pscustomobject]$firstMove
        firstMoveStartSkewMilliseconds = $firstMoveStartSkewMilliseconds
        sourceStartSkewWithinTwoSeconds =
            ($firstMoveStartSkewMilliseconds -le 2000.0)
        relayObservedCausalStartUtc = [pscustomobject]$causalStartUtc
        battleAppearanceUtc = [pscustomobject]$battleUtc
        overlapStartUtc = $overlapStart
        overlapEndUtc = $overlapEnd
        overlapMilliseconds = ($overlapEnd - $overlapStart).TotalMilliseconds
        postBattleReady = [pscustomobject]@{
            fixedSettleSeconds = 3
            quietSeconds = 3
            timeoutSeconds = 30
            pollMilliseconds = [int]$postBattleReady.pollMilliseconds
            sampleCount = [int]$postBattleReady.sampleCount
        }
        chargeWatchParity = [pscustomobject]@{
            proved = $false
            reason = 'current MSS DebugTest build has no structured per-dispatch charge-watch producer'
            todo = 'prove own-id drops for both heroes, no cross-bill, and one UI thread id'
        }
    }
}

function Get-LegacyBattleBlockAttackPlan([object]$Fixture) {
    # battle_block_check.ps1 performed two distinct relay-global /api/stacks
    # reads immediately before its sole host Fire:
    #   1) Stk(host id)
    #   2) a fresh complete census used to choose the nearest neutral.
    # The removable host raw-stack projection is the exact equivalent shared
    # authoritative source. Do not consolidate these snapshots and do not
    # validate them against the pinned fixture before the source verdict.
    $hostHeroWorld = Get-LegacyStackSnapshot
    $hostHero = @($hostHeroWorld.stacks | Where-Object {
        [string]$_.id -eq [string]$Fixture.host.heroId
    } | Select-Object -First 1)
    if ($hostHero.Count -ne 1) {
        throw 'battle-block source host Stk read found no host hero'
    }

    $neutralWorld = Get-LegacyStackSnapshot
    $nearest = @($neutralWorld.stacks | Where-Object {
        (Get-LegacyStackRole $_) -eq 'neutral'
    } | ForEach-Object {
        [pscustomobject]@{
            id = [string]$_.id
            x = [int]$_.x
            y = [int]$_.y
            distance = [Math]::Max(
                [Math]::Abs([int]$_.x - [int]$hostHero[0].x),
                [Math]::Abs([int]$_.y - [int]$hostHero[0].y))
        }
    } | Sort-Object distance | Select-Object -First 1)
    if ($nearest.Count -ne 1) {
        throw 'battle-block source fresh stacks read found no nearest neutral'
    }

    return [pscustomobject]@{
        id = [string]$hostHero[0].id
        fromX = [int]$hostHero[0].x
        fromY = [int]$hostHero[0].y
        target = $nearest[0]
        samples = @($hostHeroWorld, $neutralWorld)
    }
}

function Assert-LegacyBattleBlockAttackPlanMatchesFixture([object]$Plan,
                                                          [object]$Fixture) {
    if ([string]$Plan.id -ne [string]$Fixture.host.heroId -or
        [int]$Plan.fromX -ne [int]$Fixture.host.deploy.x -or
        [int]$Plan.fromY -ne [int]$Fixture.host.deploy.y -or
        [string]$Plan.target.id -ne [string]$Fixture.host.target.id -or
        [int]$Plan.target.x -ne [int]$Fixture.host.target.x -or
        [int]$Plan.target.y -ne [int]$Fixture.host.target.y) {
        throw ("battle-block deferred MSS fixture validation rejected the source attack plan: " +
            "hero=$($Plan.id) from=($($Plan.fromX),$($Plan.fromY)) " +
            "target=$($Plan.target.id)@($($Plan.target.x),$($Plan.target.y))")
    }
}

function Invoke-PinnedMovementLedgerWhileBattleLive {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$MoverRole,
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$BattleRole,
        [Parameter(Mandatory)][string]$HeroId,
        [Parameter(Mandatory)][object[]]$Steps,
        [Parameter(Mandatory)][object]$MapBinding,
        [Parameter(Mandatory)][long]$BattleAppearance,
        [Parameter(Mandatory)][long]$BattleOwner,
        [Parameter(Mandatory)][System.Diagnostics.Process]$HostProcess,
        [Parameter(Mandatory)][System.Diagnostics.Process]$JoinProcess,
        [Parameter(Mandatory)][string]$Context
    )
    if ($MoverRole -eq $BattleRole -or $Steps.Count -lt 1) {
        throw "$Context requires distinct mover/battle roles and a nonempty fixed ledger"
    }

    $records = [System.Collections.Generic.List[object]]::new()
    [int]$stepNumber = 0
    foreach ($expected in $Steps) {
        $stepNumber++
        $battleBefore = Get-DialogObservation $BattleRole
        $battleBeforeProof = Get-ExactLiveBattleUiProof `
            -Role $BattleRole -State $battleBefore.State `
            -ExpectedAppearance $BattleAppearance -ExpectedOwner $BattleOwner `
            -Context "$Context step $stepNumber battle before"

        $moverWorldBefore = Get-World $MoverRole
        $battleWorldBefore = Get-World $BattleRole
        [long]$moverWorldBeforeSequence = Get-OptionalProperty `
            $moverWorldBefore 'worldSeq'
        [long]$battleWorldBeforeSequence = Get-OptionalProperty `
            $battleWorldBefore 'worldSeq'
        $moverBefore = Get-WorldStackExact $moverWorldBefore $HeroId
        $remoteBefore = Get-WorldStackExact $battleWorldBefore $HeroId
        foreach ($observed in @($moverBefore, $remoteBefore)) {
            if ([int]$observed.x -ne [int]$expected.fromX -or
                [int]$observed.y -ne [int]$expected.fromY -or
                [int]$observed.movement -ne [int]$expected.movementBefore) {
                throw ("$Context step $stepNumber preflight drifted before its sole command: " +
                    "observed=($($observed.x),$($observed.y))/MP$($observed.movement), " +
                    "expected=($($expected.fromX),$($expected.fromY))/" +
                    "MP$($expected.movementBefore)")
            }
        }
        if ($moverWorldBeforeSequence -lt 1 -or $battleWorldBeforeSequence -lt 1) {
            throw "$Context step $stepNumber has no exact pre-command world watermarks"
        }

        # Reuse the proved battle-live single-move budget. Unlike the 12-second
        # paired canonical fire, each ledger step has one native result path;
        # Move-Stack still performs exactly one POST and never retries/refires.
        if (-not (Move-Stack $MoverRole $HeroId `
                ([int]$expected.fromX) ([int]$expected.fromY) `
                ([int]$expected.toX) ([int]$expected.toY) `
                ([long]$MapBinding.Instance) ([long]$MapBinding.Appearance) `
                ([int]$expected.movementBefore) `
                -CommandTimeoutMilliseconds 8000)) {
            throw "$Context step $stepNumber sole movement command was not issued"
        }

        $moverWorldAfter = Wait-WorldEvidence -Role $MoverRole `
            -After $moverWorldBeforeSequence `
            -Description "$Context step $stepNumber local application" `
            -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 30 `
            -Predicate {
                param($world)
                $hero = Get-WorldStackExact $world $HeroId -AllowMissing
                return $hero -and
                    [int]$hero.x -eq [int]$expected.toX -and
                    [int]$hero.y -eq [int]$expected.toY -and
                    [int]$hero.movement -eq [int]$expected.movementAfter
            }
        $battleWorldAfter = Wait-WorldEvidence -Role $BattleRole `
            -After $battleWorldBeforeSequence `
            -Description "$Context step $stepNumber remote replication" `
            -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 30 `
            -Predicate {
                param($world)
                $hero = Get-WorldStackExact $world $HeroId -AllowMissing
                return $hero -and
                    [int]$hero.x -eq [int]$expected.toX -and
                    [int]$hero.y -eq [int]$expected.toY -and
                    [int]$hero.movement -eq [int]$expected.movementAfter
            }
        $moverAfter = Get-WorldStackExact $moverWorldAfter $HeroId
        $remoteAfter = Get-WorldStackExact $battleWorldAfter $HeroId
        $battleAfter = Get-DialogObservation $BattleRole
        $battleAfterProof = Get-ExactLiveBattleUiProof `
            -Role $BattleRole -State $battleAfter.State `
            -ExpectedAppearance $BattleAppearance -ExpectedOwner $BattleOwner `
            -Context "$Context step $stepNumber battle after"
        $battleHistory = Get-UiHistory `
            $BattleRole ([long]$battleBeforeProof.sequence)
        [long]$battleUiCursor = [long]$battleBeforeProof.sequence
        [int]$battleUiEventsAudited = 0
        foreach ($uiEvent in @($battleHistory.events | Sort-Object seq)) {
            [long]$uiSequence = Get-OptionalProperty $uiEvent 'seq'
            if ($uiSequence -gt [long]$battleAfterProof.sequence) {
                break
            }
            if ($uiSequence -le $battleUiCursor) {
                throw "$Context step $stepNumber battle UI history repeated/regressed"
            }
            [void](Get-ExactLiveBattleUiProof `
                -Role $BattleRole -State $uiEvent `
                -ExpectedAppearance $BattleAppearance -ExpectedOwner $BattleOwner `
                -Context "$Context step $stepNumber battle history")
            $battleUiCursor = $uiSequence
            $battleUiEventsAudited++
        }
        if ($battleUiCursor -ne [long]$battleAfterProof.sequence) {
            throw "$Context step $stepNumber could not close its exact battle UI interval"
        }
        Assert-ClientsLive $HostProcess $JoinProcess

        Write-Step (("{0} step {1}/{2}: {3} ({4},{5})/MP{6} -> " +
            "({7},{8})/MP{9}; {10} battle {11}/{12} stayed live") -f
            $Context, $stepNumber, $Steps.Count, $MoverRole,
            [int]$expected.fromX, [int]$expected.fromY,
            [int]$expected.movementBefore, [int]$moverAfter.x,
            [int]$moverAfter.y, [int]$moverAfter.movement, $BattleRole,
            $BattleAppearance, $BattleOwner)
        $records.Add([pscustomobject]@{
            step = $stepNumber
            commandIssued = $true
            id = $HeroId
            fromX = [int]$expected.fromX
            fromY = [int]$expected.fromY
            toX = [int]$moverAfter.x
            toY = [int]$moverAfter.y
            movementBefore = [int]$expected.movementBefore
            movementAfter = [int]$moverAfter.movement
            moved = $true
            battleRole = $BattleRole
            battleLiveBefore = $true
            battleLiveAfter = $true
            sameBattleAppearance = $true
            localWorldSequenceBefore = $moverWorldBeforeSequence
            localWorldSequenceAfter = [long]$moverWorldAfter.seq
            remoteWorldSequenceBefore = $battleWorldBeforeSequence
            remoteWorldSequenceAfter = [long]$battleWorldAfter.seq
            remoteReplicationProved = $true
            battleLiveControlsBefore = @($battleBeforeProof.liveControls)
            battleLiveControlsAfter = @($battleAfterProof.liveControls)
            battleUiEventsAudited = $battleUiEventsAudited
            battleUiHistoryClosed = $true
            battleAppearance = $BattleAppearance
            battleOwner = $BattleOwner
        })
    }
    return @($records)
}

function Get-LegacyCrossBattleReverseAttackPlan([object]$Fixture) {
    # cross_battle_action.ps1 read the fighter, then a fresh neutral census,
    # then the mover's MP. Preserve those three physical shared reads. Unlike
    # the old diagnostic, the caller fires the resulting attack exactly once.
    $fighterWorld = Get-LegacyStackSnapshot
    $fighter = Get-WorldStackExact $fighterWorld ([string]$Fixture.join.heroId)

    $neutralWorld = Get-LegacyStackSnapshot
    $nearest = @($neutralWorld.stacks | Where-Object {
        (Get-LegacyStackRole $_) -eq 'neutral'
    } | ForEach-Object {
        [pscustomobject]@{
            id = [string]$_.id
            x = [int]$_.x
            y = [int]$_.y
            distance = [Math]::Max(
                [Math]::Abs([int]$_.x - [int]$fighter.x),
                [Math]::Abs([int]$_.y - [int]$fighter.y))
        }
    } | Sort-Object distance | Select-Object -First 1)
    if ($nearest.Count -ne 1) {
        throw 'reverse cross-battle found no nearest neutral for join'
    }

    $moverWorld = Get-LegacyStackSnapshot
    $mover = Get-WorldStackExact $moverWorld ([string]$Fixture.host.heroId)
    return [pscustomobject]@{
        fighterId = [string]$fighter.id
        fromX = [int]$fighter.x
        fromY = [int]$fighter.y
        fighterMovementBefore = [int]$fighter.movement
        target = $nearest[0]
        moverId = [string]$mover.id
        moverX = [int]$mover.x
        moverY = [int]$mover.y
        moverMovementBefore = [int]$mover.movement
        samples = @($fighterWorld, $neutralWorld, $moverWorld)
    }
}

function Assert-LegacyCrossBattleReverseAttackPlan([object]$Plan,
                                                   [object]$Fixture,
                                                   [object]$JoinEndpoint) {
    if ([string]$Plan.fighterId -ne [string]$Fixture.join.heroId -or
        [int]$Plan.fromX -ne [int]$JoinEndpoint.toX -or
        [int]$Plan.fromY -ne [int]$JoinEndpoint.toY -or
        [int]$Plan.fighterMovementBefore -ne [int]$JoinEndpoint.movementAfter -or
        [int]$Plan.fighterMovementBefore -lt
            [int]$Fixture.crossBattle.joinAttackWireBudget -or
        [string]$Plan.target.id -ne [string]$Fixture.crossBattle.joinTarget.id -or
        [int]$Plan.target.x -ne [int]$Fixture.crossBattle.joinTarget.x -or
        [int]$Plan.target.y -ne [int]$Fixture.crossBattle.joinTarget.y -or
        [string]$Plan.moverId -ne [string]$Fixture.host.heroId -or
        [int]$Plan.moverX -ne [int]$Fixture.host.battleEnd.x -or
        [int]$Plan.moverY -ne [int]$Fixture.host.battleEnd.y -or
        [int]$Plan.moverMovementBefore -ne [int]$Fixture.host.battleEnd.movement -or
        [int]$Plan.moverMovementBefore -le 0) {
        throw ('reverse cross-battle attack plan does not match the pinned composed state: ' +
            "join=$($Plan.fighterId)@($($Plan.fromX),$($Plan.fromY)) " +
            "MP=$($Plan.fighterMovementBefore) target=$($Plan.target.id)" +
            "@($($Plan.target.x),$($Plan.target.y)); host=$($Plan.moverId)" +
            "@($($Plan.moverX),$($Plan.moverY)) MP=$($Plan.moverMovementBefore)")
    }
}

function Get-LegacyCrossBattleHostMovePlan([object]$Fixture) {
    # CrossTest read Stk(host), then FreeAdj made one separate stacks census.
    # Keep its exact direction order and garrison-distance selection. No
    # alternate cell is attempted if the selected move does not apply.
    $moverWorld = Get-LegacyStackSnapshot
    $mover = Get-WorldStackExact $moverWorld ([string]$Fixture.host.heroId)
    $occupancyWorld = Get-LegacyStackSnapshot
    $deltas = @(
        @(-1, 0), @(0, -1), @(1, 0), @(0, 1),
        @(-1, -1), @(1, 1), @(-1, 1), @(1, -1)
    )
    $candidates = @(
        foreach ($delta in $deltas) {
            [int]$x = [int]$mover.x + [int]$delta[0]
            [int]$y = [int]$mover.y + [int]$delta[1]
            if ($x -eq [int]$Fixture.host.garrison.x -and
                $y -eq [int]$Fixture.host.garrison.y) {
                continue
            }
            if (@($occupancyWorld.stacks | Where-Object {
                    [int]$_.x -eq $x -and [int]$_.y -eq $y
                }).Count -gt 0) {
                continue
            }
            [pscustomobject]@{
                x = $x
                y = $y
                garrisonDistance = [Math]::Max(
                    [Math]::Abs($x - [int]$Fixture.host.garrison.x),
                    [Math]::Abs($y - [int]$Fixture.host.garrison.y))
            }
        }
    )
    $selected = @($candidates | Sort-Object garrisonDistance | Select-Object -First 1)
    if ($selected.Count -ne 1) {
        throw 'reverse cross-battle old FreeAdj found no host destination'
    }
    return [pscustomobject]@{
        id = [string]$mover.id
        fromX = [int]$mover.x
        fromY = [int]$mover.y
        movementBefore = [int]$mover.movement
        target = $selected[0]
        samples = @($moverWorld, $occupancyWorld)
    }
}

function Assert-LegacyCrossBattleHostMovePlan([object]$Plan,
                                              [object]$Fixture,
                                              [object]$AttackPlan) {
    if ([string]$Plan.id -ne [string]$Fixture.host.heroId -or
        [int]$Plan.fromX -ne [int]$Fixture.host.battleEnd.x -or
        [int]$Plan.fromY -ne [int]$Fixture.host.battleEnd.y -or
        [int]$Plan.movementBefore -ne [int]$AttackPlan.moverMovementBefore -or
        [int]$Plan.movementBefore -le 0 -or
        [int]$Plan.target.x -ne [int]$Fixture.crossBattle.hostMove.x -or
        [int]$Plan.target.y -ne [int]$Fixture.crossBattle.hostMove.y) {
        throw ('reverse cross-battle host FreeAdj plan does not match the pinned state: ' +
            "host=$($Plan.id)@($($Plan.fromX),$($Plan.fromY)) " +
            "MP=$($Plan.movementBefore) target=($($Plan.target.x),$($Plan.target.y))")
    }
}

function Test-ExactWorldStackPositionAndMovement {
    param(
        [Parameter(Mandatory)][object]$World,
        [Parameter(Mandatory)][string]$HeroId,
        [Parameter(Mandatory)][int]$X,
        [Parameter(Mandatory)][int]$Y,
        [Parameter(Mandatory)][int]$Movement
    )
    $hero = Get-WorldStackExact $World $HeroId -AllowMissing
    return [bool]($hero -and
        [int]$hero.x -eq $X -and
        [int]$hero.y -eq $Y -and
        [int]$hero.movement -eq $Movement)
}

function Wait-ExactCrossBattleHostMoveWorld {
    param(
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][long]$After,
        [Parameter(Mandatory)][string]$HeroId,
        [Parameter(Mandatory)][int]$X,
        [Parameter(Mandatory)][int]$Y,
        [Parameter(Mandatory)][int]$Movement,
        [Parameter(Mandatory)][string]$WorldLabel,
        [Parameter(Mandatory)][string]$Description,
        [System.Diagnostics.Process]$HostProcess,
        [System.Diagnostics.Process]$JoinProcess,
        [int]$TimeoutSec = 30
    )
    try {
        return (Wait-WorldEvidence -Role $Role -After $After `
            -Description $Description `
            -HostProcess $HostProcess -JoinProcess $JoinProcess `
            -TimeoutSec $TimeoutSec `
            -Predicate {
                param($world)
                Test-ExactWorldStackPositionAndMovement `
                    -World $world -HeroId $HeroId -X $X -Y $Y `
                    -Movement $Movement
            })
    } catch {
        $waitFailure = $_.Exception
        if ([string]$waitFailure.Message -notlike
                "timed out waiting for $Role world evidence:*") {
            throw
        }
        try {
            $latestWorld = Get-World $Role
        } catch {
            throw $waitFailure
        }
        $latestHero = Get-WorldStackExact `
            $latestWorld $HeroId -AllowMissing
        if ($latestHero -and
            [int]$latestHero.x -eq $X -and
            [int]$latestHero.y -eq $Y -and
            [int]$latestHero.movement -ne $Movement) {
            $diagnostic = $latestHero | Select-Object `
                id, x, y, movement, owner, relation, units, hp, inside, `
                unitIds, unitStates | ConvertTo-Json -Compress -Depth 4
            throw (('reverse cross-battle host FreeAdj did not converge before ' +
                'the bounded wait at the exact {0} endpoint ({1},{2}); last MP={3}, ' +
                'expected pinned MP={4}; stack/unit={5}') -f $WorldLabel, $X, $Y,
                [int]$latestHero.movement, $Movement, $diagnostic)
        }
        throw $waitFailure
    }
}

function Invoke-BattleBlockProof([object]$Fixture,
                                 [System.Diagnostics.Process]$HostProcess,
                                 [System.Diagnostics.Process]$JoinProcess,
                                 [Parameter(Mandatory)][string]$HostLog,
                                 [Parameter(Mandatory)][object]$PreparedDeployBindings) {
    # The source script defaulted to six attempts, but the only documented green
    # checkpoint is FREE 4/4 (commit aef761e and RE/34). Pin that proved
    # invocation. Cold r1 observed no movement on attempt five; attempt six then
    # repeated the same semantic intent. The reason for the non-move is not
    # needed by this oracle and is not claimed here.
    [int]$sourceStepCount = 4
    Invoke-CanonicalDeploy `
        $Fixture $HostProcess $JoinProcess $PreparedDeployBindings
    if (-not (Wait-ActionablePair -TimeoutSec 60 -QuietSec 3)) {
        throw 'battle-block: maps did not settle after deploy'
    }

    $hostUiBefore = Get-RoleEvidenceSequence host ui
    $hostMapBinding = Get-MapActionTargetBinding host
    $legacyAttackPlan = Get-LegacyBattleBlockAttackPlan $Fixture
    Write-Step ("battle-block source host @({0},{1}) -> attack nearest neutral {2}@({3},{4})" -f
        $legacyAttackPlan.fromX, $legacyAttackPlan.fromY, $legacyAttackPlan.target.id,
        $legacyAttackPlan.target.x, $legacyAttackPlan.target.y)
    if (-not (Move-Stack host ([string]$legacyAttackPlan.id) `
            ([int]$legacyAttackPlan.fromX) ([int]$legacyAttackPlan.fromY) `
            ([int]$legacyAttackPlan.target.x) ([int]$legacyAttackPlan.target.y) `
            $hostMapBinding.Instance $hostMapBinding.Appearance `
            -CommandTimeoutMilliseconds 8000)) {
        throw 'battle-block: the sole host attack command was not issued'
    }
    $hostBattleUi = Wait-UiEvidence -Role host -After $hostUiBefore `
        -Description 'battle-block source host DLG_BATTLE_A after its sole attack' `
        -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 60 `
        -Predicate { param($ui) [string]$ui.dialog -eq 'DLG_BATTLE_A' }
    $hostBattle = Get-DialogObservation host
    if (-not $hostBattle -or $hostBattle.Dialog -ne 'DLG_BATTLE_A') {
        throw 'battle-block: host battle was not current after its start evidence'
    }
    # The MSS command needs an immutable native map owner. Capture it once
    # before the oracle clock; every step below passes it explicitly, so the
    # one Fire contains no hidden role-state observation.
    $joinMapBinding = Get-MapActionTargetBinding join
    # The source oracle reads the host-only raw stack projection. Preserve it
    # literally, but watermark both role-world streams before its four actions
    # so the MSS extension can require causal two-client convergence.
    $sourceRoleStates = [ordered]@{
        host = Get-RoleState host
        join = Get-RoleState join
    }
    $sourceWorldBaselines = [ordered]@{}
    foreach ($ownedRole in @(
            [pscustomobject]@{ name = 'host'; process = $HostProcess },
            [pscustomobject]@{ name = 'join'; process = $JoinProcess }
        )) {
        $roleState = $sourceRoleStates[$ownedRole.name]
        if ($null -eq $roleState -or -not [bool]$roleState.connected -or
            [long]$roleState.pid -ne [long]$ownedRole.process.Id -or
            [string]::IsNullOrWhiteSpace([string]$roleState.modulePath)) {
            throw "battle-block source $($ownedRole.name) world owner is not its exact process"
        }
        $sourceWorldBaselines[$ownedRole.name] = [long]$roleState.worldSeq
    }
    if ([long]$sourceWorldBaselines.host -lt 1 -or
        [long]$sourceWorldBaselines.join -lt 1) {
        throw 'battle-block source world watermarks are missing before FREE 4/4'
    }

    # Commit aef761e records the source checkpoint as FREE 4/4. Preserve those
    # four one-shot east intents, and fail immediately if any one does not move:
    # continuing from an unchanged origin would repeat the same mutation.
    $legacySteps = [System.Collections.Generic.List[object]]::new()
    $freeSteps = 0
    $afterSteps = 0
    $issuedMoveSettlements = 0
    $t0 = Get-Date
    for ($i = 1; $i -le $sourceStepCount; $i++) {
        # LITERAL_BATTLE_BLOCK_STEP_BEGIN: Ui(host) -> Stk(join) -> Fire east once.
        $hostUiBeforeStep = Get-DialogObservation host
        $inBat = $hostUiBeforeStep -and $hostUiBeforeStep.Dialog -eq 'DLG_BATTLE_A'
        [long]$joinWorldBeforeStepSequence = Get-RoleEvidenceSequence join world
        # Stk(joiner-id) in battle_block_check.ps1 was another read from the
        # single relay-global /api/stacks census. The removable host raw-stack
        # projection preserves that provenance; only host can publish it.
        $joinBeforeWorld = Get-LegacyStackSnapshot
        $before = Get-WorldStackExact $joinBeforeWorld ([string]$Fixture.join.heroId)
        $jx = [int]$before.x
        $jy = [int]$before.y
        if (-not (Move-Stack join ([string]$Fixture.join.heroId) `
                $jx $jy ($jx + 1) $jy `
                $joinMapBinding.Instance $joinMapBinding.Appearance `
                ([int]$before.movement))) {
            throw "battle-block step ${i}: the sole join east-step command was not issued"
        }
        Start-Sleep -Milliseconds 1500
        $joinAfterWorld = Get-LegacyStackSnapshot
        $after = Get-WorldStackExact $joinAfterWorld ([string]$Fixture.join.heroId)
        $movedAtFixedSample =
            ([int]$after.x -ne $jx -or [int]$after.y -ne $jy)
        $moved = $movedAtFixedSample
        $settlementExtended = -not $movedAtFixedSample
        if ($settlementExtended) {
            $issuedMoveSettlements++
        }

        # Keep the source's fixed +1500 ms sample and verdict immutable. Before
        # another command can reuse the resulting expectedFrom, independently
        # require the mover's own role-world stream (not the host legacy raw
        # projection) to publish the exact charged east endpoint. This is one
        # read-only causal fence for the command already issued; it cannot
        # issue another command. The fixed-sample bit remains separate when a
        # later owner publication supplies the already-issued move's result.
        [int]$expectedMovementAfter = [int]$before.movement - 3
        $joinSettledWorld = Wait-WorldEvidence -Role join `
            -After $joinWorldBeforeStepSequence `
            -Description "battle-block step ${i} exact join-world settlement" `
            -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 15 `
            -Predicate {
                param($world)
                $hero = Get-WorldStackExact `
                    $world ([string]$Fixture.join.heroId) -AllowMissing
                return $hero -and
                    [int]$hero.x -eq ($jx + 1) -and
                    [int]$hero.y -eq $jy -and
                    [int]$hero.movement -eq $expectedMovementAfter
            }
        $joinSettled = Get-WorldStackExact `
            $joinSettledWorld ([string]$Fixture.join.heroId)
        if (-not $movedAtFixedSample) {
            $joinAfterWorld = $joinSettledWorld
            $after = $joinSettled
            $moved = ([int]$after.x -ne $jx -or [int]$after.y -ne $jy)
        }
        $hostUiAfterStep = Get-DialogObservation host
        $hostBat = $hostUiAfterStep -and $hostUiAfterStep.Dialog -eq 'DLG_BATTLE_A'
        Write-Step ("battle-block source step ${i}: hostInBattle=$inBat -> " +
            "joiner ($jx,$jy)->($($after.x),$($after.y)) moved=$moved " +
            "fixed1500=$movedAtFixedSample settled=$settlementExtended " +
            ("(host battle now=$hostBat) T+{0:N1}s" -f ((Get-Date) - $t0).TotalSeconds))
        if ($moved -and $inBat) { $freeSteps++ }
        if ($moved -and -not $inBat) { $afterSteps++ }
        $legacySteps.Add([pscustomobject]@{
            step = $i
            commandIssued = $true
            fromX = $jx
            fromY = $jy
            toX = [int]$after.x
            toY = [int]$after.y
            movementBefore = [int]$before.movement
            movementAfter = [int]$after.movement
            moved = [bool]$moved
            movedAtFixedSample = [bool]$movedAtFixedSample
            issuedMoveSettlement = [bool]$settlementExtended
            hostInBattleBefore = [bool]$inBat
            hostInBattleAfter = [bool]$hostBat
            joinWorldBefore = $joinBeforeWorld
            joinWorldAfter = $joinAfterWorld
            joinWorldBeforeStepSequence = $joinWorldBeforeStepSequence
            joinWorldSettlement = $joinSettledWorld
            joinWorldSettlementSequence = [long]$joinSettledWorld.seq
            joinWorldSettlementProved = $true
        })
        if (-not $moved) {
            throw "battle-block step ${i} did not move; refusing another mutation"
        }
        if (-not $inBat -or -not $hostBat) {
            throw ("battle-block step ${i} was not wholly observed during host battle: " +
                "before=$inBat after=$hostBat")
        }
        # LITERAL_BATTLE_BLOCK_STEP_END: the one-shot step is fully classified.
    }

    $legacyVerdict = if ($freeSteps -gt 0) {
        'FREE'
    } elseif ($afterSteps -gt 0) {
        'BLOCKED'
    } else {
        'INCONCLUSIVE'
    }
    Write-Step ("battle-block source verdict: moves DURING=$freeSteps; " +
        "only-after=$afterSteps; result=$legacyVerdict (documented source checkpoint 4/4)")
    if ($legacyVerdict -ne 'FREE') {
        throw "battle-block source verdict is $legacyVerdict; legacy FREE requires freeSteps > 0"
    }
    Write-Step 'battle-block source FREE PASS: all four join steps applied while Ui(host)=DLG_BATTLE_A'
    # LITERAL_BATTLE_BLOCK_VERDICT_BOUNDARY. Fixture identity, the four-command ledger,
    # continuous-battle, process and history checks below are
    # explicitly additional MSS strengthening after the legacy FREE verdict.

    Assert-ClientsLive $HostProcess $JoinProcess
    Assert-LegacyBattleBlockAttackPlanMatchesFixture $legacyAttackPlan $Fixture
    if ($legacySteps.Count -ne $sourceStepCount) {
        throw "battle-block deferred MSS proof lost a source action record"
    }

    # Recompute the legacy counters from the immutable four action records and
    # add the stronger MSS proof that host remained in battle across every
    # command's full 1500 ms observation interval.
    $recountedFreeSteps = 0
    $recountedAfterSteps = 0
    $continuousFreeSteps = 0
    $lastX = [int]$Fixture.join.deploy.x
    $lastY = [int]$Fixture.join.deploy.y
    $lastMovement = [int]$Fixture.join.deploy.movement
    foreach ($legacyStep in $legacySteps) {
        if (-not [bool]$legacyStep.commandIssued -or
            [int]$legacyStep.fromX -ne $lastX -or [int]$legacyStep.fromY -ne $lastY -or
            [int]$legacyStep.movementBefore -ne $lastMovement -or
            ([bool]$legacyStep.moved -and
                ([int]$legacyStep.toX -ne ([int]$legacyStep.fromX + 1) -or
                 [int]$legacyStep.toY -ne [int]$legacyStep.fromY -or
                 [int]$legacyStep.movementAfter -ge [int]$legacyStep.movementBefore))) {
            throw "battle-block deferred MSS accounting rejected source step $($legacyStep.step)"
        }
        if ([bool]$legacyStep.moved -and [bool]$legacyStep.hostInBattleBefore) {
            $recountedFreeSteps++
        }
        if ([bool]$legacyStep.moved -and -not [bool]$legacyStep.hostInBattleBefore) {
            $recountedAfterSteps++
        }
        if ([bool]$legacyStep.moved -and
            [bool]$legacyStep.hostInBattleBefore -and
            [bool]$legacyStep.hostInBattleAfter) {
            $continuousFreeSteps++
        }
        $lastX = [int]$legacyStep.toX
        $lastY = [int]$legacyStep.toY
        $lastMovement = [int]$legacyStep.movementAfter
    }
    if ($recountedFreeSteps -ne $freeSteps -or
        $recountedAfterSteps -ne $afterSteps) {
        throw ("battle-block deferred MSS counter audit changed the source verdict " +
            "(free=$freeSteps/$recountedFreeSteps after=$afterSteps/$recountedAfterSteps)")
    }
    if ($freeSteps -ne $sourceStepCount -or $afterSteps -ne 0 -or
        $continuousFreeSteps -ne $sourceStepCount) {
        throw ("battle-block did not reproduce continuous FREE 4/4: " +
            "free=$freeSteps after=$afterSteps continuous=$continuousFreeSteps")
    }
    # Boot-Ready/run_test armed auto-battle before either game process existed.
    # Consume the native exact-once proof after the source verdict; no late
    # toggle/HTTP action belongs to battle_block_check.ps1.
    $autoBattle = Read-PrearmedAutoBattleProof -Role host `
        -Lines @(Read-ClientLogLines $HostLog) -BattleUi $hostBattleUi
    if ($null -eq $autoBattle) {
        throw 'battle-block deferred MSS proof found no host preboot auto-battle proof'
    }

    Assert-ClientsLive $HostProcess $JoinProcess
    Write-Step ('battle-block MSS audit PASS: continuous battle FREE 4/4 and ' +
        'preboot auto-battle proved; source boundary has host still in battle')

    # MSS_CROSS_BATTLE_FORWARD_HANDOFF_BEGIN. Cold r1 proved that the old
    # host-only raw stack oracle can publish the fourth endpoint before the
    # host role-world stream does. Do not insert a fixed delay and do not send
    # another action: consume one causal exact-state event after the pre-FREE4
    # watermark from each client while the same host battle remains live.
    $sourceEndpoint = $legacySteps[$legacySteps.Count - 1]
    $plannedJoinStressSteps = @($Fixture.crossBattle.joinStressSteps)
    if ($plannedJoinStressSteps.Count -ne 5) {
        throw 'forward cross-battle stress must contain exactly five safe shuttle commands'
    }
    $firstStressStep = $plannedJoinStressSteps[0]
    if ([int]$sourceEndpoint.toX -ne 19 -or
        [int]$sourceEndpoint.toY -ne 27 -or
        [int]$sourceEndpoint.movementAfter -ne 23 -or
        [int]$firstStressStep.fromX -ne [int]$sourceEndpoint.toX -or
        [int]$firstStressStep.fromY -ne [int]$sourceEndpoint.toY -or
        [int]$firstStressStep.movementBefore -ne
            [int]$sourceEndpoint.movementAfter) {
        throw 'source FREE4 endpoint is not the pinned (19,27)/MP23 stress origin'
    }
    $sourceHandoffBattleBefore = Get-DialogObservation host
    $sourceHandoffBattleBeforeProof = Get-ExactLiveBattleUiProof `
        -Role host -State $sourceHandoffBattleBefore.State `
        -ExpectedAppearance ([long]$autoBattle.kick.appearance) `
        -ExpectedOwner ([long]$autoBattle.kick.owner) `
        -Context 'forward cross-battle handoff battle before world convergence'
    $sourceHandoffPair = Wait-ExactWorldPairPublication `
        -HeroId ([string]$Fixture.join.heroId) `
        -X ([int]$sourceEndpoint.toX) -Y ([int]$sourceEndpoint.toY) `
        -Movement ([int]$sourceEndpoint.movementAfter) `
        -HostAfterWorldSequence ([long]$sourceWorldBaselines.host) `
        -JoinAfterWorldSequence ([long]$sourceWorldBaselines.join) `
        -ExpectedHostProcessId ([long]$HostProcess.Id) `
        -ExpectedJoinProcessId ([long]$JoinProcess.Id) `
        -ExpectedHostModulePath ([string]$sourceRoleStates.host.modulePath) `
        -ExpectedJoinModulePath ([string]$sourceRoleStates.join.modulePath) `
        -WaitMilliseconds 30000
    Assert-ClientsLive $HostProcess $JoinProcess
    $sourceHandoffWorlds = [ordered]@{}
    foreach ($role in @('host', 'join')) {
        $matchedWorld = $sourceHandoffPair.$role
        $matchedHero = $matchedWorld.stack
        $sourceHandoffWorlds[$role] = [pscustomobject]@{
            baselineSequence = [long]$sourceWorldBaselines[$role]
            evidenceSequence = [long]$matchedWorld.worldSeq
            currentSequence = [long]$matchedWorld.worldSeq
            x = [int]$matchedHero.x
            y = [int]$matchedHero.y
            movement = [int]$matchedHero.movement
        }
    }
    $sourceHandoffBattleAfter = Get-DialogObservation host
    $sourceHandoffBattleAfterProof = Get-ExactLiveBattleUiProof `
        -Role host -State $sourceHandoffBattleAfter.State `
        -ExpectedAppearance ([long]$sourceHandoffBattleBeforeProof.appearance) `
        -ExpectedOwner ([long]$sourceHandoffBattleBeforeProof.owner) `
        -Context 'forward cross-battle handoff battle after world convergence'
    $sourceHandoffUiHistory = Get-UiHistory `
        host ([long]$sourceHandoffBattleBeforeProof.sequence)
    [long]$sourceHandoffUiCursor = `
        [long]$sourceHandoffBattleBeforeProof.sequence
    [int]$sourceHandoffUiEventsAudited = 0
    foreach ($uiEvent in @($sourceHandoffUiHistory.events | Sort-Object seq)) {
        [long]$uiSequence = Get-OptionalProperty $uiEvent 'seq'
        if ($uiSequence -gt [long]$sourceHandoffBattleAfterProof.sequence) {
            break
        }
        if ($uiSequence -le $sourceHandoffUiCursor) {
            throw 'forward cross-battle handoff UI history repeated/regressed'
        }
        [void](Get-ExactLiveBattleUiProof -Role host -State $uiEvent `
            -ExpectedAppearance ([long]$sourceHandoffBattleBeforeProof.appearance) `
            -ExpectedOwner ([long]$sourceHandoffBattleBeforeProof.owner) `
            -Context 'forward cross-battle handoff battle history')
        $sourceHandoffUiCursor = $uiSequence
        $sourceHandoffUiEventsAudited++
    }
    if ($sourceHandoffUiCursor -ne
        [long]$sourceHandoffBattleAfterProof.sequence) {
        throw 'forward cross-battle handoff could not close its exact battle UI interval'
    }
    Write-Step (('battle-block source handoff PASS: both role worlds reached ' +
        '({0},{1})/MP{2} after their pre-FREE4 watermarks; host battle {3}/{4} ' +
        'stayed continuously live') -f $sourceEndpoint.toX, $sourceEndpoint.toY,
        $sourceEndpoint.movementAfter,
        $sourceHandoffBattleAfterProof.appearance,
        $sourceHandoffBattleAfterProof.owner)
    # MSS_CROSS_BATTLE_FORWARD_HANDOFF_VERDICT_BOUNDARY.

    # MSS_CROSS_BATTLE_FORWARD_STRESS_BEGIN. The source FREE 4/4 verdict above
    # remains immutable. Extend that still-live host battle with five separately
    # pinned join commands over the already proved (18,27)<->(19,27) cells. The
    # fixed ledger ends at (18,27)/MP8. The exact legacy attack builder has a
    # maximum six-point wire budget to its proved target. A sixth shuttle would
    # leave MP5, so five is the maximum safe planned workload, not a
    # loop-until-failure.
    $hostForwardStressProof = $sourceHandoffBattleAfterProof
    $joinStressSteps = @(Invoke-PinnedMovementLedgerWhileBattleLive `
        -MoverRole join -BattleRole host `
        -HeroId ([string]$Fixture.join.heroId) `
        -Steps $plannedJoinStressSteps `
        -MapBinding $joinMapBinding `
        -BattleAppearance ([long]$hostForwardStressProof.appearance) `
        -BattleOwner ([long]$hostForwardStressProof.owner) `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -Context 'forward cross-battle shuttle')
    $joinStressEndpoint = $joinStressSteps[$joinStressSteps.Count - 1]
    if ($joinStressSteps.Count -ne 5 -or
        [int]$joinStressEndpoint.movementAfter -lt
            [int]$Fixture.crossBattle.joinAttackWireBudget) {
        throw ('forward cross-battle stress did not finish its five-command ' +
            'ledger with the pinned attack reserve')
    }
    Write-Step (('forward cross-battle STRESS PASS: source 4 + shuttle {0} = ' +
        '{1} join commands during one host battle; endpoint=({2},{3})/MP{4}, ' +
        'attack reserve={5}') -f $joinStressSteps.Count,
        ($sourceStepCount + $joinStressSteps.Count),
        $joinStressEndpoint.toX, $joinStressEndpoint.toY,
        $joinStressEndpoint.movementAfter,
        [int]$Fixture.crossBattle.joinAttackWireBudget)
    # MSS_CROSS_BATTLE_FORWARD_STRESS_VERDICT_BOUNDARY.

    # LITERAL_CROSS_BATTLE_REVERSE_BEGIN. This is a composed continuation of
    # the proved FREE 4/4 state, not a claim that the old campaign transcript
    # reached its reverse phase (that saved run skipped both battles). Complete
    # only the host battle required to make host a strategic mover. The native
    # session subscriber remains the sole result-close owner.
    $hostBattleCompletion = Complete-CanonicalBattle `
        -Role host -Spec $Fixture.host -BattleUi $hostBattleUi `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -AutoBattleAlreadyEnabled
    Assert-ClientsLive $HostProcess $JoinProcess

    # The original cross_battle_action.ps1 separated its two CrossTest calls by
    # exactly three seconds after the first battle closed. Preserve that one
    # explained inter-phase quiet period before its three reverse-plan reads.
    Start-Sleep -Seconds 3

    $joinUiBeforeReverseAttack = Get-RoleEvidenceSequence join ui
    $joinMapBinding = Get-MapActionTargetBinding join
    $reverseAttackPlan = Get-LegacyCrossBattleReverseAttackPlan $Fixture
    Assert-LegacyCrossBattleReverseAttackPlan `
        $reverseAttackPlan $Fixture $joinStressEndpoint
    Write-Step (('reverse cross-battle: join @({0},{1}) MP={2} -> nearest neutral ' +
        '{3}@({4},{5}); host ready @({6},{7}) MP={8}') -f
        $reverseAttackPlan.fromX, $reverseAttackPlan.fromY,
        $reverseAttackPlan.fighterMovementBefore,
        $reverseAttackPlan.target.id, $reverseAttackPlan.target.x,
        $reverseAttackPlan.target.y, $reverseAttackPlan.moverX,
        $reverseAttackPlan.moverY, $reverseAttackPlan.moverMovementBefore)
    if (-not (Move-Stack join ([string]$reverseAttackPlan.fighterId) `
            ([int]$reverseAttackPlan.fromX) ([int]$reverseAttackPlan.fromY) `
            ([int]$reverseAttackPlan.target.x) ([int]$reverseAttackPlan.target.y) `
            $joinMapBinding.Instance $joinMapBinding.Appearance)) {
        throw 'reverse cross-battle: the sole join attack command was not issued'
    }
    $joinBattleUi = Wait-UiEvidence -Role join -After $joinUiBeforeReverseAttack `
        -Description 'reverse cross-battle exact ready join DLG_BATTLE_A' `
        -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 60 `
        -Predicate {
            param($ui)
            if ([string]$ui.dialog -ne 'DLG_BATTLE_A' -or
                -not [bool]$ui.dialogReady) {
                return $false
            }
            [void](Get-ExactLiveBattleUiProof -Role join -State $ui `
                -Context 'reverse battle-start evidence')
            return $true
        }
    $joinBattleInitialProof = Get-ExactLiveBattleUiProof `
        -Role join -State $joinBattleUi -Context 'reverse battle-start evidence'
    [long]$joinBattleAppearance = $joinBattleInitialProof.appearance
    [long]$joinBattleOwner = $joinBattleInitialProof.owner

    $hostMapBinding = Get-MapActionTargetBinding host
    $reverseHostMovePlan = Get-LegacyCrossBattleHostMovePlan $Fixture
    Assert-LegacyCrossBattleHostMovePlan `
        $reverseHostMovePlan $Fixture $reverseAttackPlan
    $joinWorldBeforeHostMove = Get-World join
    [long]$joinWorldBeforeReverseMove = Get-OptionalProperty `
        $joinWorldBeforeHostMove 'worldSeq'
    $joinRemoteHostBeforeReverseMove = Get-WorldStackExact `
        $joinWorldBeforeHostMove ([string]$Fixture.host.heroId)
    if ($joinWorldBeforeReverseMove -lt 1 -or
        [int]$joinRemoteHostBeforeReverseMove.x -ne
            [int]$Fixture.host.battleEnd.x -or
        [int]$joinRemoteHostBeforeReverseMove.y -ne
            [int]$Fixture.host.battleEnd.y -or
        [int]$joinRemoteHostBeforeReverseMove.movement -ne
            [int]$Fixture.host.battleEnd.movement) {
        throw 'reverse cross-battle join world did not start at the pinned host endpoint'
    }
    $hostWorldBeforeReverseMove = Get-RoleEvidenceSequence host world
    $joinBattleBeforeHostMove = Get-DialogObservation join
    $joinBattleBeforeProof = Get-ExactLiveBattleUiProof `
        -Role join -State $joinBattleBeforeHostMove.State `
        -ExpectedAppearance $joinBattleAppearance -ExpectedOwner $joinBattleOwner `
        -Context 'battle immediately before the sole host move'
    if (-not (Move-Stack host ([string]$reverseHostMovePlan.id) `
            ([int]$reverseHostMovePlan.fromX) ([int]$reverseHostMovePlan.fromY) `
            ([int]$reverseHostMovePlan.target.x) ([int]$reverseHostMovePlan.target.y) `
            $hostMapBinding.Instance $hostMapBinding.Appearance `
            ([int]$reverseHostMovePlan.movementBefore))) {
        throw 'reverse cross-battle: the sole host adjacent move was not issued'
    }
    # First require the remote join client to publish the host move. Native
    # autonav::tick refreshes UI and then rebuilds world on the same natural UI
    # frame; the bridge writes a changed UiSnapshot before WorldSnapshot on the
    # same join socket. Therefore the relay's join UI cache is at least as new as
    # this exact world publication. Audit every intervening UI publication too.
    # Position and movement are read from the same immutable WorldSnapshot, but
    # the engine can naturally publish an endpoint-only intermediate followed
    # by the charged endpoint. Consume that append-only sequence passively and
    # accept only the exact pinned endpoint+MP; never retry or reissue the move.
    $joinMoveWorld = Wait-ExactCrossBattleHostMoveWorld -Role join `
        -After $joinWorldBeforeReverseMove -HeroId ([string]$Fixture.host.heroId) `
        -X ([int]$reverseHostMovePlan.target.x) `
        -Y ([int]$reverseHostMovePlan.target.y) `
        -Movement ([int]$Fixture.crossBattle.hostMove.movement) `
        -WorldLabel 'join-world' `
        -Description 'reverse cross-battle host FreeAdj replicated to join world' `
        -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 30
    $joinRemoteHostAfterReverseMove = Get-WorldStackExact `
        $joinMoveWorld ([string]$Fixture.host.heroId)
    # Original CrossTest classified a close within 1.5 seconds of movement as
    # BLOCKED. Preserve that passive post-apply grace exactly; it sends no
    # command and cannot restart or extend itself.
    Start-Sleep -Milliseconds 1500
    Assert-ClientsLive $HostProcess $JoinProcess
    $joinBattleAfterHostMove = Get-DialogObservation join
    $joinBattleAfterProof = Get-ExactLiveBattleUiProof `
        -Role join -State $joinBattleAfterHostMove.State `
        -ExpectedAppearance $joinBattleAppearance -ExpectedOwner $joinBattleOwner `
        -Context 'battle after host movement replicated to join world'
    $joinUiHistory = Get-UiHistory join ([long]$joinBattleBeforeProof.sequence)
    [long]$joinUiCursor = [long]$joinBattleBeforeProof.sequence
    [int]$joinUiEventsAudited = 0
    foreach ($uiEvent in @($joinUiHistory.events | Sort-Object seq)) {
        [long]$uiSequence = Get-OptionalProperty $uiEvent 'seq'
        if ($uiSequence -gt [long]$joinBattleAfterProof.sequence) {
            break
        }
        if ($uiSequence -le $joinUiCursor) {
            throw ('reverse cross-battle join UI history repeated/regressed between ' +
                'the host command and its join-world replication')
        }
        [void](Get-ExactLiveBattleUiProof -Role join -State $uiEvent `
            -ExpectedAppearance $joinBattleAppearance -ExpectedOwner $joinBattleOwner `
            -Context 'history through host movement replication')
        $joinUiCursor = $uiSequence
        $joinUiEventsAudited++
    }
    if ($joinUiCursor -ne [long]$joinBattleAfterProof.sequence) {
        throw 'reverse cross-battle could not close the exact join UI history interval'
    }

    $hostMoveWorld = Wait-ExactCrossBattleHostMoveWorld -Role host `
        -After $hostWorldBeforeReverseMove -HeroId ([string]$Fixture.host.heroId) `
        -X ([int]$reverseHostMovePlan.target.x) `
        -Y ([int]$reverseHostMovePlan.target.y) `
        -Movement ([int]$Fixture.crossBattle.hostMove.movement) `
        -WorldLabel 'host-world' `
        -Description 'reverse cross-battle exact host FreeAdj local world move' `
        -HostProcess $HostProcess -JoinProcess $JoinProcess -TimeoutSec 30
    $hostAfterReverseMove = Get-WorldStackExact `
        $hostMoveWorld ([string]$Fixture.host.heroId)
    if ([int]$joinRemoteHostAfterReverseMove.x -ne
            [int]$reverseHostMovePlan.target.x -or
        [int]$joinRemoteHostAfterReverseMove.y -ne
            [int]$reverseHostMovePlan.target.y -or
        [int]$joinRemoteHostAfterReverseMove.movement -ne
            [int]$Fixture.crossBattle.hostMove.movement -or
        [int]$hostAfterReverseMove.x -ne [int]$reverseHostMovePlan.target.x -or
        [int]$hostAfterReverseMove.y -ne [int]$reverseHostMovePlan.target.y -or
        [int]$hostAfterReverseMove.movement -ne
            [int]$Fixture.crossBattle.hostMove.movement) {
        throw ('reverse cross-battle host move was not wholly applied during the ' +
            'same exact live join battle on both client worlds')
    }
    Assert-ClientsLive $HostProcess $JoinProcess
    Write-Step (('reverse cross-battle FREE PASS: host ({0},{1}) MP={2} -> ' +
        '({3},{4}) MP={5} during join battle appearance={6} owner={7}') -f
        $reverseHostMovePlan.fromX, $reverseHostMovePlan.fromY,
        $reverseHostMovePlan.movementBefore,
        $hostAfterReverseMove.x, $hostAfterReverseMove.y,
        $hostAfterReverseMove.movement, $joinBattleAppearance, $joinBattleOwner)
    # LITERAL_CROSS_BATTLE_REVERSE_VERDICT_BOUNDARY. This is the exact old
    # one-FreeAdj/+1500 ms classification boundary. No completion, retry,
    # refire, or alternate cell was used to obtain it. The separately marked
    # MSS workload below starts only after that immutable intermediate verdict.

    # MSS_CROSS_BATTLE_HOST_EXHAUSTION_BEGIN. Continue from the source step's
    # exact (27,17)/MP13 endpoint. Five shuttle entries over the two already
    # proved cost-2 cells reach (28,17)/MP3; the final, separately proved
    # canonical post-battle entry into the now-vacant (29,17) costs exactly 3.
    # Thus the fixed six-command tail reaches MP=0 without a seventh probe.
    $hostExhaustionSteps = @(Invoke-PinnedMovementLedgerWhileBattleLive `
        -MoverRole host -BattleRole join `
        -HeroId ([string]$Fixture.host.heroId) `
        -Steps @($Fixture.crossBattle.hostExhaustionSteps) `
        -MapBinding $hostMapBinding `
        -BattleAppearance $joinBattleAppearance `
        -BattleOwner $joinBattleOwner `
        -HostProcess $HostProcess -JoinProcess $JoinProcess `
        -Context 'reverse cross-battle host exhaustion')
    $hostExhaustionEndpoint = $hostExhaustionSteps[
        $hostExhaustionSteps.Count - 1]
    if ($hostExhaustionSteps.Count -ne 6 -or
        [int]$hostExhaustionEndpoint.toX -ne
            [int]$Fixture.host.postBattleWalk.x -or
        [int]$hostExhaustionEndpoint.toY -ne
            [int]$Fixture.host.postBattleWalk.y -or
        [int]$hostExhaustionEndpoint.movementAfter -ne 0) {
        throw 'reverse cross-battle host did not consume its exact movement ledger to MP=0'
    }
    Assert-ClientsLive $HostProcess $JoinProcess
    Write-Step (('reverse cross-battle STRESS PASS: source 1 + tail {0} = {1} ' +
        'host commands during one join battle; endpoint=({2},{3})/MP0') -f
        $hostExhaustionSteps.Count, (1 + $hostExhaustionSteps.Count),
        $hostExhaustionEndpoint.toX, $hostExhaustionEndpoint.toY)
    # MSS_CROSS_BATTLE_HOST_EXHAUSTION_VERDICT_BOUNDARY. The join battle is
    # deliberately still live; no command or completion follows this boundary.

    $reverseCross = [pscustomobject]@{
        attempted = $true
        composedAfterForwardFourOfFour = $true
        sourcePostApplyGraceMilliseconds = 1500
        hostBattleCompletion = $hostBattleCompletion
        joinAttack = [pscustomobject]@{
            commandIssued = $true
            id = [string]$reverseAttackPlan.fighterId
            fromX = [int]$reverseAttackPlan.fromX
            fromY = [int]$reverseAttackPlan.fromY
            movementBefore = [int]$reverseAttackPlan.fighterMovementBefore
            targetId = [string]$reverseAttackPlan.target.id
            targetX = [int]$reverseAttackPlan.target.x
            targetY = [int]$reverseAttackPlan.target.y
        }
        hostStep = [pscustomobject]@{
            commandIssued = $true
            id = [string]$reverseHostMovePlan.id
            fromX = [int]$reverseHostMovePlan.fromX
            fromY = [int]$reverseHostMovePlan.fromY
            toX = [int]$hostAfterReverseMove.x
            toY = [int]$hostAfterReverseMove.y
            movementBefore = [int]$reverseHostMovePlan.movementBefore
            movementAfter = [int]$hostAfterReverseMove.movement
            moved = $true
            joinBattleLiveBefore = $true
            joinBattleLiveAfter = $true
            sameBattleAppearance = $true
            joinWorldReplicationProved = $true
            joinUiBeforeWorldCausalityProved = $true
            joinWorldBefore = [pscustomobject]@{
                sequence = $joinWorldBeforeReverseMove
                x = [int]$joinRemoteHostBeforeReverseMove.x
                y = [int]$joinRemoteHostBeforeReverseMove.y
                movement = [int]$joinRemoteHostBeforeReverseMove.movement
            }
            joinWorldAfter = [pscustomobject]@{
                sequence = [long]$joinMoveWorld.seq
                x = [int]$joinRemoteHostAfterReverseMove.x
                y = [int]$joinRemoteHostAfterReverseMove.y
                movement = [int]$joinRemoteHostAfterReverseMove.movement
            }
            joinUiEventsAudited = $joinUiEventsAudited
            joinLiveControlsBefore = @($joinBattleBeforeProof.liveControls)
            joinLiveControlsAfter = @($joinBattleAfterProof.liveControls)
            joinBattleAppearance = $joinBattleAppearance
            joinBattleOwner = $joinBattleOwner
        }
        hostExhaustion = [pscustomobject]@{
            sourceStepIncluded = $true
            sourceStepCount = 1
            stressStepCount = $hostExhaustionSteps.Count
            totalCommands = 1 + $hostExhaustionSteps.Count
            movementBefore = [int]$reverseHostMovePlan.movementBefore
            movementAfter = [int]$hostExhaustionEndpoint.movementAfter
            finalX = [int]$hostExhaustionEndpoint.toX
            finalY = [int]$hostExhaustionEndpoint.toY
            exhaustedToZero = $true
            joinBattleStayedSameAndLive = $true
            stressSteps = @($hostExhaustionSteps)
        }
        verdict = 'FREE'
        stressVerdict = 'FREE'
        joinBattleCompletionAttempted = $false
    }

    $steps = [System.Collections.Generic.List[object]]::new()
    foreach ($legacyStep in $legacySteps) {
        $steps.Add([pscustomobject]@{
            step = [int]$legacyStep.step
            commandIssued = [bool]$legacyStep.commandIssued
            fromX = [int]$legacyStep.fromX
            fromY = [int]$legacyStep.fromY
            toX = [int]$legacyStep.toX
            toY = [int]$legacyStep.toY
            movementBefore = [int]$legacyStep.movementBefore
            movementAfter = [int]$legacyStep.movementAfter
            moved = [bool]$legacyStep.moved
            movedAtFixedSample = [bool]$legacyStep.movedAtFixedSample
            issuedMoveSettlement = [bool]$legacyStep.issuedMoveSettlement
            hostInBattleBefore = [bool]$legacyStep.hostInBattleBefore
            hostInBattleAfter = [bool]$legacyStep.hostInBattleAfter
        })
    }
    return [pscustomobject]@{
        freeSteps = $freeSteps
        afterSteps = $afterSteps
        continuousFreeSteps = $continuousFreeSteps
        issuedMoveSettlements = $issuedMoveSettlements
        steps = @($steps)
        legacyVerdict = $legacyVerdict
        legacyFreePassed = $true
        sourceOracleCategory = 'documented-green-free-4-of-4'
        deferredMss = [pscustomobject]@{
            fixtureAttackPlanMatched = $true
            commandsAudited = $sourceStepCount
            countersRecomputed = $true
            continuousBattleFourOfFourProved = $true
            prebootAutoBattleProved = $true
            sourceBoundaryObservedBeforeHostBattleCompletion = $true
            forwardStressFiveStepsProved = $true
            hostBattleCompletedOnlyForReverseContinuation = $true
            reverseCrossOneShotProved = $true
            reverseHostExhaustionProved = $true
        }
        autoBattle = $autoBattle
        forwardStress = [pscustomobject]@{
            sourceStepCount = $sourceStepCount
            stressStepCount = $joinStressSteps.Count
            totalCommands = $sourceStepCount + $joinStressSteps.Count
            movementBefore = [int]$legacySteps[0].movementBefore
            movementAfter = [int]$joinStressEndpoint.movementAfter
            finalX = [int]$joinStressEndpoint.toX
            finalY = [int]$joinStressEndpoint.toY
            attackWireBudgetReserved = [int]$Fixture.crossBattle.joinAttackWireBudget
            hostBattleStayedSameAndLive = $true
            sourceHandoff = [pscustomobject]@{
                expectedX = [int]$sourceEndpoint.toX
                expectedY = [int]$sourceEndpoint.toY
                expectedMovement = [int]$sourceEndpoint.movementAfter
                hostBattleAppearance = `
                    [long]$sourceHandoffBattleAfterProof.appearance
                hostBattleOwner = [long]$sourceHandoffBattleAfterProof.owner
                battleUiEventsAudited = $sourceHandoffUiEventsAudited
                battleUiBaselineSequence = `
                    [long]$sourceHandoffBattleBeforeProof.sequence
                battleUiFinalSequence = `
                    [long]$sourceHandoffBattleAfterProof.sequence
                battleUiHistoryClosed = $true
                hostBattleStayedSameAndLive = $true
                host = $sourceHandoffWorlds.host
                join = $sourceHandoffWorlds.join
            }
            stressSteps = @($joinStressSteps)
        }
        reverseCross = $reverseCross
    }
}

