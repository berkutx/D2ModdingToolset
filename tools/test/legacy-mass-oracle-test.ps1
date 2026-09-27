#requires -Version 7.0
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '_legacy_mass_oracle.ps1')

function Assert-Equal {
    param([AllowNull()][object]$Actual, [AllowNull()][object]$Expected, [string]$Label)
    if ([string]$Actual -cne [string]$Expected) {
        throw "${Label}: actual='$Actual', expected='$Expected'"
    }
}

function New-GreenLegacyMassSummary {
    $walk = {
        [pscustomobject]@{
            legacyPass = $true
            strayBattle = $false
            host = [pscustomobject]@{ moved = $true; charged = $true }
            join = [pscustomobject]@{ moved = $true; charged = $true }
        }
    }
    return [pscustomobject]@{
        gameplay = [pscustomobject]@{
            attacks = [pscustomobject]@{
                legacyPass = $true
                battleStartedCount = 2
                battleClosedCount = 2
                postFire = [pscustomobject]@{
                    host = [pscustomobject]@{ movement = 15 }
                    join = [pscustomobject]@{ movement = 11 }
                }
                postBattle = [pscustomobject]@{
                    host = [pscustomobject]@{ movement = 15 }
                    join = [pscustomobject]@{ movement = 11 }
                }
            }
            postBattleWalk = & $walk
        }
        day2Walk = & $walk
        independentRounds = @([pscustomobject]@{
            legacyPass = $true
            movementBefore = [pscustomobject]@{ host = 14; join = 10 }
            movementRefresh = [pscustomobject]@{ host = 35; join = 35 }
        })
        merge = [pscustomobject]@{
            legacyBarrier = [pscustomobject]@{
                legacyPass = $true
                mergeCountHost = 1
                mergeCountJoin = 1
                hostRefreshed = $true
                cascadesNew = 0
                overRotated = $false
                rotated = $true
                deadClickConfirmed = $false
                deadClickConfirmedKnown = $true
            }
        }
    }
}

$phases = @(
    'boot', 'deploy', 'attack-fire', 'battle-start', 'battle-close',
    'walk1', 'endturn-fire', 'endturn-done', 'walk2',
    'barrier-fire', 'barrier-done'
)
$transcript = @('run_test cold child started')

$green = New-GreenLegacyMassSummary
$oracle = Get-LegacyMassTestOracle $green $transcript $phases ''
Assert-Equal $oracle.status 'PASS' 'green status'
Assert-Equal $oracle.postFire '15/11' 'green postFire'
Assert-Equal $oracle.postBattle '15/11' 'green postBattle'
Assert-Equal $oracle.etRefresh '14->35/10->35' 'green end-turn refresh'
Assert-Equal (($oracle.checks.Keys) -join ',') `
    'boot,deploy,attack-charged,battles,post-battle-kept,attack-verdict,walk1,endturn-refresh,endturn-verdict,no-stray-battle,walk2,merge-1+1,host-mp-35,no-barrier-cascade,postmerge-rotates,barrier-verdict' `
    'check order'

$noLog = Get-LegacyMassTestOracle $null @() @() 'boot failed'
Assert-Equal $noLog.status 'NO-LOG' 'NO-LOG priority'

$bootFail = Get-LegacyMassTestOracle $null @('launch') @() 'roles never settled'
Assert-Equal $bootFail.status 'BOOT-FAIL' 'BOOT-FAIL priority'

$over = New-GreenLegacyMassSummary
$over.merge.legacyBarrier.overRotated = $true
$oracle = Get-LegacyMassTestOracle $over $transcript $phases ''
Assert-Equal $oracle.status 'OVERROTATE' 'OVERROTATE priority'

$dead = New-GreenLegacyMassSummary
$dead.merge.legacyBarrier.rotated = $false
$dead.merge.legacyBarrier.legacyPass = $false
$dead.merge.legacyBarrier.deadClickConfirmed = $true
$dead.merge.legacyBarrier.deadClickConfirmedKnown = $true
$oracle = Get-LegacyMassTestOracle $dead $transcript $phases ''
Assert-Equal $oracle.status 'DEADCLICK' 'confirmed DEADCLICK priority'

$unknownDead = New-GreenLegacyMassSummary
$unknownDead.merge.legacyBarrier.rotated = $false
$unknownDead.merge.legacyBarrier.legacyPass = $false
$unknownDead.merge.legacyBarrier.deadClickConfirmed = $null
$unknownDead.merge.legacyBarrier.deadClickConfirmedKnown = $false
$oracle = Get-LegacyMassTestOracle $unknownDead $transcript $phases ''
Assert-Equal $oracle.status 'FAIL@postmerge-rotates' 'unconfirmed no-rotation classification'

$stalled = New-GreenLegacyMassSummary
$oracle = Get-LegacyMassTestOracle $stalled $transcript `
    @('boot', 'deploy', 'attack-fire') 'battle never appeared'
Assert-Equal $oracle.status 'STALL@battle-start' 'first missing marker'

$failed = New-GreenLegacyMassSummary
$failed.gameplay.attacks.postFire.host.movement = 35
$oracle = Get-LegacyMassTestOracle $failed $transcript $phases ''
Assert-Equal $oracle.status 'FAIL@attack-charged' 'first false state check'

$missingRoundVerdict = New-GreenLegacyMassSummary
$missingRoundVerdict.independentRounds[0].PSObject.Properties.Remove('legacyPass')
$oracle = Get-LegacyMassTestOracle $missingRoundVerdict $transcript $phases ''
Assert-Equal $oracle.status 'FAIL@endturn-verdict' `
    'missing child independent-round legacy verdict is never inferred'

Write-Host '[legacy-mass-oracle-test] PASS: source verdict priority and ordered checks preserved' `
    -ForegroundColor Green

