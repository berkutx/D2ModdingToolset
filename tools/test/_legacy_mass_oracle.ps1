#requires -Version 7.0
<#
.SYNOPSIS
Pure structured projection of lobby/test/mass_test.ps1::Analyze.

.DESCRIPTION
The old five-run mass wrapper was not just a launcher: after every complete
run_test.ps1 transcript it applied an ordered phase-marker scan and an ordered
set of state-change checks.  The MSS harness records the same observations as
typed values, so this module reproduces that final oracle without issuing a
gameplay action, performing a wait, or reading live process state.

The verdict priority intentionally stays source-compatible:
NO-LOG, BOOT-FAIL, OVERROTATE, DEADCLICK, STALL@<phase>,
FAIL@<first-check>, PASS.

The one deliberate modern constraint is the project-wide exact-once rule.  A
failed first post-merge End Turn is never followed by a diagnostic second End
Turn.  Consequently DEADCLICK is emitted only when supplied evidence says it
was actually confirmed; an unconfirmed first-action failure remains
FAIL@postmerge-rotates (or the earlier source-compatible STALL result).
#>

Set-StrictMode -Version Latest

function Get-LegacyMassProperty {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory)][string]$Name
    )
    if ($null -eq $InputObject) { return $null }
    if ($InputObject -is [Collections.IDictionary]) {
        if ($InputObject.Contains($Name)) { return $InputObject[$Name] }
        return $null
    }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-LegacyMassPath {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory)][string[]]$Path
    )
    $current = $InputObject
    foreach ($name in $Path) {
        $current = Get-LegacyMassProperty $current $name
        if ($null -eq $current) { return $null }
    }
    return $current
}

function Convert-LegacyMassInteger {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value -or $Value -is [bool]) { return $null }
    [long]$parsed = 0
    if (-not [long]::TryParse(
            [string]$Value,
            [Globalization.NumberStyles]::Integer,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed)) {
        return $null
    }
    return $parsed
}

function Test-LegacyMassTrue {
    param([AllowNull()][object]$Value)
    return $Value -is [bool] -and [bool]$Value
}

function Format-LegacyMassPair {
    param([AllowNull()][object]$HostValue, [AllowNull()][object]$JoinValue)
    $hostNumber = Convert-LegacyMassInteger $HostValue
    $joinNumber = Convert-LegacyMassInteger $JoinValue
    if ($null -eq $hostNumber -or $null -eq $joinNumber) { return '?' }
    return "$hostNumber/$joinNumber"
}

function Get-LegacyMassTestOracle {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Summary,
        [AllowNull()][AllowEmptyCollection()][object[]]$StepTranscript = @(),
        [AllowNull()][AllowEmptyCollection()][object[]]$PhaseTrace = @(),
        [AllowNull()][string]$Failure = ''
    )

    $transcript = @($StepTranscript | ForEach-Object { [string]$_ })
    if ($transcript.Count -eq 0) {
        return [pscustomobject]@{
            status = 'NO-LOG'
            fail = 'no transcript'
            postFire = '?'
            postBattle = '?'
            etRefresh = '?'
            guardDefer = '?'
            merge = '?'
            stall = $null
            checks = [ordered]@{}
            phaseTrace = @()
            deadClickConfirmedKnown = $false
        }
    }

    $phaseOrder = @(
        'boot',
        'deploy',
        'attack-fire',
        'battle-start',
        'battle-close',
        'walk1',
        'endturn-fire',
        'endturn-done',
        'walk2',
        'barrier-fire',
        'barrier-done'
    )
    $trace = @($PhaseTrace | ForEach-Object { [string]$_ })
    $phasePresent = @{}
    foreach ($phase in $phaseOrder) {
        $phasePresent[$phase] = $trace -contains $phase
    }
    $stall = $null
    foreach ($phase in $phaseOrder) {
        if (-not [bool]$phasePresent[$phase]) {
            $stall = $phase
            break
        }
    }

    $attacks = Get-LegacyMassPath $Summary @('gameplay', 'attacks')
    $postFireHost = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $attacks @('postFire', 'host', 'movement'))
    $postFireJoin = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $attacks @('postFire', 'join', 'movement'))
    $postBattleHost = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $attacks @('postBattle', 'host', 'movement'))
    $postBattleJoin = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $attacks @('postBattle', 'join', 'movement'))
    $battleStarted = Convert-LegacyMassInteger `
        (Get-LegacyMassProperty $attacks 'battleStartedCount')
    $battleClosed = Convert-LegacyMassInteger `
        (Get-LegacyMassProperty $attacks 'battleClosedCount')

    $postBattleWalk = Get-LegacyMassPath $Summary @('gameplay', 'postBattleWalk')
    $day2Walk = Get-LegacyMassProperty $Summary 'day2Walk'
    $rounds = @(Get-LegacyMassProperty $Summary 'independentRounds')
    $round = if ($rounds.Count -ge 1) { $rounds[0] } else { $null }
    $movementBeforeHost = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $round @('movementBefore', 'host'))
    $movementBeforeJoin = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $round @('movementBefore', 'join'))
    $movementAfterHost = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $round @('movementRefresh', 'host'))
    $movementAfterJoin = Convert-LegacyMassInteger `
        (Get-LegacyMassPath $round @('movementRefresh', 'join'))

    $legacyBarrier = Get-LegacyMassPath $Summary @('merge', 'legacyBarrier')
    $deadClickKnown = Test-LegacyMassTrue `
        (Get-LegacyMassProperty $legacyBarrier 'deadClickConfirmedKnown')
    $deadClickValue = Get-LegacyMassProperty $legacyBarrier 'deadClickConfirmed'
    $deadClick = $deadClickKnown -and $deadClickValue -is [bool] -and
        [bool]$deadClickValue
    $overRotated = Test-LegacyMassTrue `
        (Get-LegacyMassProperty $legacyBarrier 'overRotated')

    # Keep this insertion order byte-for-byte analogous to mass_test.ps1's
    # ordered checks.  The first false entry becomes FAIL@<name> only after the
    # source's higher-priority stall/overrotation/dead-click decisions.
    $checks = [ordered]@{}
    $checks['boot'] = [bool]$phasePresent['boot']
    $checks['deploy'] = [bool]$phasePresent['deploy']
    $checks['attack-charged'] = (
        $null -ne $postFireHost -and $postFireHost -gt 0 -and $postFireHost -lt 35 -and
        $null -ne $postFireJoin -and $postFireJoin -gt 0 -and $postFireJoin -lt 35)
    $checks['battles'] = (
        $null -ne $battleStarted -and $battleStarted -ge 2 -and
        $null -ne $battleClosed -and $battleClosed -ge 2)
    $checks['post-battle-kept'] = (
        $null -ne $postBattleHost -and $null -ne $postBattleJoin -and
        $null -ne $postFireHost -and $null -ne $postFireJoin -and
        $postBattleHost -eq $postFireHost -and
        $postBattleJoin -eq $postFireJoin -and
        $postBattleHost -ge 0)
    $checks['attack-verdict'] = Test-LegacyMassTrue `
        (Get-LegacyMassProperty $attacks 'legacyPass')
    $checks['walk1'] = (
        (Test-LegacyMassTrue (Get-LegacyMassProperty $postBattleWalk 'legacyPass')) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $postBattleWalk @('host', 'moved'))) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $postBattleWalk @('host', 'charged'))) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $postBattleWalk @('join', 'moved'))) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $postBattleWalk @('join', 'charged'))))
    $checks['endturn-refresh'] = (
        $null -ne $movementBeforeHost -and $movementBeforeHost -lt 35 -and
        $null -ne $movementBeforeJoin -and $movementBeforeJoin -lt 35 -and
        $movementAfterHost -eq 35 -and $movementAfterJoin -eq 35)
    $checks['endturn-verdict'] = Test-LegacyMassTrue `
        (Get-LegacyMassProperty $round 'legacyPass')
    $checks['no-stray-battle'] = (
        -not (Test-LegacyMassTrue `
            (Get-LegacyMassProperty $postBattleWalk 'strayBattle')) -and
        -not (Test-LegacyMassTrue `
            (Get-LegacyMassProperty $day2Walk 'strayBattle')))
    $checks['walk2'] = (
        (Test-LegacyMassTrue (Get-LegacyMassProperty $day2Walk 'legacyPass')) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $day2Walk @('host', 'moved'))) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $day2Walk @('host', 'charged'))) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $day2Walk @('join', 'moved'))) -and
        (Test-LegacyMassTrue (Get-LegacyMassPath $day2Walk @('join', 'charged'))))
    $checks['merge-1+1'] = (
        (Convert-LegacyMassInteger `
            (Get-LegacyMassProperty $legacyBarrier 'mergeCountHost')) -eq 1 -and
        (Convert-LegacyMassInteger `
            (Get-LegacyMassProperty $legacyBarrier 'mergeCountJoin')) -eq 1)
    $checks['host-mp-35'] = Test-LegacyMassTrue `
        (Get-LegacyMassProperty $legacyBarrier 'hostRefreshed')
    $checks['no-barrier-cascade'] = (
        (Convert-LegacyMassInteger `
            (Get-LegacyMassProperty $legacyBarrier 'cascadesNew')) -eq 0)
    $checks['postmerge-rotates'] = (
        (Test-LegacyMassTrue (Get-LegacyMassProperty $legacyBarrier 'rotated')) -and
        $deadClickKnown -and -not $deadClick)
    $checks['barrier-verdict'] = Test-LegacyMassTrue `
        (Get-LegacyMassProperty $legacyBarrier 'legacyPass')

    $firstFail = $null
    foreach ($entry in $checks.GetEnumerator()) {
        if (-not [bool]$entry.Value) {
            $firstFail = [string]$entry.Key
            break
        }
    }

    $bootAbort = -not [string]::IsNullOrWhiteSpace($Failure) -and
        -not [bool]$checks['boot']
    $status = if ($bootAbort) {
        'BOOT-FAIL'
    } elseif ($overRotated) {
        'OVERROTATE'
    } elseif ($deadClick) {
        'DEADCLICK'
    } elseif ($null -ne $stall) {
        "STALL@$stall"
    } elseif ($null -ne $firstFail) {
        "FAIL@$firstFail"
    } else {
        'PASS'
    }

    $postFireText = Format-LegacyMassPair $postFireHost $postFireJoin
    $postBattleText = Format-LegacyMassPair $postBattleHost $postBattleJoin
    $etRefreshText = if (
        $null -eq $movementBeforeHost -or $null -eq $movementBeforeJoin -or
        $null -eq $movementAfterHost -or $null -eq $movementAfterJoin) {
        '?'
    } else {
        "$movementBeforeHost->$movementAfterHost/$movementBeforeJoin->$movementAfterJoin"
    }
    $guardDefer = Get-LegacyMassPath $Summary @('legacyMassDiagnostics', 'guardDefer')
    if ($null -eq $guardDefer) { $guardDefer = '?' }
    $mergeDiagnostic = Get-LegacyMassPath $Summary @('legacyMassDiagnostics', 'mergeFired')
    if ($null -eq $mergeDiagnostic) {
        $mergeDiagnostic = Get-LegacyMassProperty $legacyBarrier 'mergeCountHost'
    }
    if ($null -eq $mergeDiagnostic) { $mergeDiagnostic = '?' }

    return [pscustomobject]@{
        status = $status
        fail = $firstFail
        postFire = $postFireText
        postBattle = $postBattleText
        etRefresh = $etRefreshText
        guardDefer = $guardDefer
        merge = $mergeDiagnostic
        stall = $stall
        checks = $checks
        phaseTrace = @($trace)
        deadClickConfirmedKnown = [bool]$deadClickKnown
    }
}

