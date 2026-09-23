#requires -Version 7.0
<#
.SYNOPSIS
Runs the complete Russobit simultaneous-turn acceptance campaign as cold child runs.

.DESCRIPTION
Every case starts simturns-production-poc.ps1 in a new pwsh process and receives
its own explicit artifact directory. A zero child exit code is necessary but is
not sufficient: this runner parses summary.json and checks the gameplay,
independent-turn, merge, literal Phase-C, battle-block, and owned-PID evidence.

The sequence is fixed and preserves the two old suites as different topologies:

1. Count independent old test/mass_test.ps1-equivalent canonical cold runs,
   each ending at the original parallel BARRIER/dead-click PASS boundary;
2. one battle-block run;
3. twelve independent old test_R_masstest.ps1-equivalent cold runs. Odd
   iterations are join-first and even iterations are host-first. Each ordered
   child starts from the fresh map, submits exactly four distinct End Turn
   intents, observes the merge, then runs the literal Phase-C continuation.

The ordered sweep is not a continuation of a canonical attack/walk child.
GameplayMode=ordered-masstest owns its complete topology and is the only mode
allowed to request PostMergeContinuationMode=automatic-masstest-phase-c-literal.

No game or DirectPlay process is stopped or discovered for cleanup here. After
each completed child except the last, the runner only waits on the process
handles of any naturally winding-down dplaysvr helper. Failure to disappear in
the bounded interval fails the campaign before another child is started.

Before each child start, the runner also observes the exact pinned executable
and map until both have their manifest size and SHA-256. It never stages or
restores either file; the bounded readiness gate only closes the transient
post-game window in which the exported scenario can be absent.

Child exit/summary/acceptance failures are recorded and tallied only after all
remaining cold children ran. A child-process start failure or unsafe dplaysvr
teardown remains terminal infrastructure failure.
With -StopOnFailure, stop after the first failed child's result validation and
existing owned-process cleanup/teardown checks, without starting another child.

The runner never deploys or stages the DLL, executable, or fixture map.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$GameDir,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$FixtureManifest,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$ArtifactDir,

    [ValidateRange(1, 100)]
    [int]$Count = 5,

    [switch]$StopOnFailure,

    # Read-only: parses every relevant source file, constructs the exact case
    # plan and every child argv, then exits before creating an artifact path or
    # starting a process.
    [switch]$StaticCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$pocPath = Join-Path $PSScriptRoot 'simturns-production-poc.ps1'
$legacyMassOraclePath = Join-Path $PSScriptRoot '_legacy_mass_oracle.ps1'
. $legacyMassOraclePath
$runId = [guid]::NewGuid().ToString('N')
$startedAt = [DateTimeOffset]::UtcNow
$naturalTeardownTimeoutSeconds = 30
$script:CurrentPhase = 'initialize'
$script:PwshPath = $null
$script:Fixture = $null
$script:FixtureMergeDay = 3
$script:RunArtifactDir = $null
$script:CampaignSummaryPath = $null
$script:BattleBlockStepCount = 4
$script:LegacyPhaseCRequiredNeutralRounds = 2
$script:OrderedMasstestIterations = 12
$script:ExpectedRunCount = $Count + 1 + $script:OrderedMasstestIterations

$runs = [System.Collections.Generic.List[object]]::new()
$campaignPassed = $false
$campaignFailure = $null

function Throw-CampaignFailure {
    param(
        [Parameter(Mandatory)][string]$Category,
        [Parameter(Mandatory)][string]$Message
    )
    $exception = [InvalidOperationException]::new($Message)
    $exception.Data['campaignCategory'] = $Category
    $exception.Data['campaignPhase'] = $script:CurrentPhase
    throw $exception
}

function Get-RequiredJsonProperty {
    param(
        [Parameter(Mandatory)][AllowNull()][object]$InputObject,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Context
    )
    if ($null -eq $InputObject) {
        Throw-CampaignFailure 'malformed-summary' "$Context is null"
    }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) {
        Throw-CampaignFailure 'malformed-summary' "$Context has no '$Name' property"
    }
    # The unary comma preserves an empty JSON array as a value instead of
    # allowing PowerShell's success stream to erase it.
    return ,$property.Value
}

function Get-OptionalJsonProperty {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory)][string]$Name
    )
    if ($null -eq $InputObject) { return $null }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return ,$property.Value
}

function Get-OptionalJsonArrayProperty {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory)][string]$Name
    )
    # Get-OptionalJsonProperty deliberately keeps an empty JSON array alive as
    # one success-stream value. Assign it first, then enumerate its elements;
    # wrapping the function call itself would turn the whole JSON array into one
    # nested item and stringify phase markers into "boot deploy ...".
    $value = Get-OptionalJsonProperty $InputObject $Name
    if ($null -eq $value) { return @() }
    return @($value)
}

function Assert-JsonObject {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Context
    )
    if ($null -eq $Value -or $Value -isnot [pscustomobject]) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be a JSON object"
    }
    return $Value
}

function Convert-RequiredInt64 {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Context
    )
    if ($null -eq $Value -or $Value -is [bool]) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be an integer"
    }
    [long]$parsed = 0
    if (-not [long]::TryParse(
            [string]$Value,
            [Globalization.NumberStyles]::Integer,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed)) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be an integer"
    }
    return $parsed
}

function Assert-ExactInteger {
    param(
        [AllowNull()][object]$Value,
        [long]$Expected,
        [Parameter(Mandatory)][string]$Context
    )
    [long]$actual = Convert-RequiredInt64 $Value $Context
    if ($actual -ne $Expected) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$Context is $actual, expected exactly $Expected"
    }
    return $actual
}

function Assert-ExactString {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Expected,
        [Parameter(Mandatory)][string]$Context
    )
    if ($Value -isnot [string]) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be a string"
    }
    if (-not [string]::Equals($Value, $Expected, [StringComparison]::Ordinal)) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$Context is '$Value', expected exactly '$Expected'"
    }
    return $Value
}

function Convert-RequiredFiniteDouble {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Context
    )
    if ($null -eq $Value -or $Value -is [bool]) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be a finite number"
    }
    [double]$parsed = 0
    if (-not [double]::TryParse(
            [string]$Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed) -or
        [double]::IsNaN($parsed) -or [double]::IsInfinity($parsed)) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be a finite number"
    }
    return $parsed
}

function Convert-RequiredPlayerHandle {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Context
    )
    if ($Value -isnot [string] -or
        $Value -notmatch '\A0[xX][0-9A-Fa-f]{8}\z') {
        Throw-CampaignFailure 'malformed-summary' `
            "$Context must be an exact 0x-prefixed eight-digit player handle"
    }
    try {
        return [long][uint32]::Parse(
            $Value.Substring(2),
            [Globalization.NumberStyles]::AllowHexSpecifier,
            [Globalization.CultureInfo]::InvariantCulture)
    } catch {
        Throw-CampaignFailure 'malformed-summary' "$Context is not a valid player handle"
    }
}

function Assert-SameFullPath {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Expected,
        [Parameter(Mandatory)][string]$Context
    )
    if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace($Value)) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be a non-empty path string"
    }
    try {
        $actualPath = [IO.Path]::GetFullPath($Value)
        $expectedPath = [IO.Path]::GetFullPath($Expected)
    } catch {
        Throw-CampaignFailure 'malformed-summary' "$Context is not a valid full path: $Value"
    }
    if (-not [string]::Equals(
            $actualPath, $expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$Context is '$actualPath', expected '$expectedPath'"
    }
    return $actualPath
}

function Get-RequiredJsonArray {
    param(
        [AllowNull()][AllowEmptyCollection()][object]$Value,
        [Parameter(Mandatory)][string]$Context
    )
    if ($null -eq $Value) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be a JSON array"
    }
    # ConvertFrom-Json preserves arrays, including a one-element array, as an
    # Object[] value. Reject a scalar rather than silently wrapping it.
    if ($Value -isnot [array]) {
        Throw-CampaignFailure 'malformed-summary' "$Context must be a JSON array"
    }
    return ,@($Value)
}

function Read-ChildSummary {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Throw-CampaignFailure 'missing-summary' "child did not write required summary.json: $Path"
    }
    try {
        $raw = Get-Content -LiteralPath $Path -Raw
        if ([string]::IsNullOrWhiteSpace($raw)) {
            throw 'summary is empty'
        }
        $summary = $raw | ConvertFrom-Json -Depth 100
    } catch {
        Throw-CampaignFailure 'malformed-summary' `
            "could not parse child summary '$Path': $($_.Exception.Message)"
    }
    return Assert-JsonObject $summary 'child summary root'
}

function Assert-DeclaredChildPass {
    param(
        [Parameter(Mandatory)][object]$Summary,
        [int]$ExitCode
    )
    $declared = Get-RequiredJsonProperty $Summary 'passed' 'child summary'
    if ($declared -isnot [bool]) {
        Throw-CampaignFailure 'malformed-summary' "child summary 'passed' must be a boolean"
    }
    $reportedFailure = Get-OptionalJsonProperty $Summary 'failure'
    if ($ExitCode -ne 0) {
        Throw-CampaignFailure 'child-process-exit' `
            "child pwsh exited with code $ExitCode; reported failure: $reportedFailure"
    }
    if (-not $declared) {
        Throw-CampaignFailure 'child-summary-failed' `
            "child exited zero but summary.json reported failure: $reportedFailure"
    }
}

function Assert-ChildOwnedTeardown {
    param([Parameter(Mandatory)][object]$Summary)

    try {
        $teardown = Assert-JsonObject `
            (Get-RequiredJsonProperty $Summary 'teardown' 'child summary') `
            'child teardown'
        $teardownAttempted = Get-RequiredJsonProperty `
            $teardown 'attempted' 'child teardown'
        $teardownPassed = Get-RequiredJsonProperty `
            $teardown 'passed' 'child teardown'
        $teardownErrors = Get-RequiredJsonArray `
            (Get-RequiredJsonProperty $teardown 'errors' 'child teardown') `
            'child teardown errors'
        $kept = Get-RequiredJsonArray `
            (Get-RequiredJsonProperty $teardown 'kept' 'child teardown') `
            'child teardown kept processes'
    } catch {
        Throw-CampaignFailure 'child-owned-teardown-unproven' `
            ("child summary cannot prove bounded teardown of exact owned " +
             "processes: $($_.Exception.Message)")
    }
    if ($teardownAttempted -isnot [bool] -or -not $teardownAttempted -or
        $teardownPassed -isnot [bool] -or -not $teardownPassed -or
        $teardownErrors.Count -ne 0 -or $kept.Count -ne 0) {
        Throw-CampaignFailure 'child-owned-teardown-failed' `
            'child did not prove bounded teardown of every exact owned process'
    }
    return $teardown
}

function Assert-ChildClientLogCompletion {
    param(
        [Parameter(Mandatory)][object]$Summary,
        [Parameter(Mandatory)][long]$HostPid,
        [Parameter(Mandatory)][long]$JoinPid,
        [Parameter(Mandatory)][string]$ChildArtifactDir
    )
    $clientLogs = Assert-JsonObject `
        (Get-RequiredJsonProperty $Summary 'clientLogs' 'child summary') `
        'child clientLogs'
    $required = Get-RequiredJsonProperty $clientLogs 'required' 'child clientLogs'
    $passed = Get-RequiredJsonProperty $clientLogs 'passed' 'child clientLogs'
    $errors = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $clientLogs 'errors' 'child clientLogs') `
        'child clientLogs errors'
    if ($required -isnot [bool] -or -not $required -or
        $passed -isnot [bool] -or -not $passed -or $errors.Count -ne 0) {
        Throw-CampaignFailure 'client-log-completion-unproven' `
            'child did not prove complete stopped host/join log artifacts'
    }

    foreach ($expected in @(
        @{ role = 'host'; pid = $HostPid },
        @{ role = 'join'; pid = $JoinPid }
    )) {
        $proof = Assert-JsonObject `
            (Get-RequiredJsonProperty $clientLogs $expected.role 'child clientLogs') `
            "child $($expected.role) client log"
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $proof 'role' "child $($expected.role) client log") `
            $expected.role "child $($expected.role) client log role")
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $proof 'pid' "child $($expected.role) client log") `
            $expected.pid "child $($expected.role) client log pid")
        $expectedFileName = "mss32_$($expected.pid).log"
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $proof 'fileName' "child $($expected.role) client log") `
            $expectedFileName "child $($expected.role) client log fileName")
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $proof 'initialLength' "child $($expected.role) client log") `
            0 "child $($expected.role) client log initialLength")
        [long]$length = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $proof 'length' "child $($expected.role) client log") `
            "child $($expected.role) client log length"
        $sha256 = Get-RequiredJsonProperty `
            $proof 'sha256' "child $($expected.role) client log"
        $endedWithLineFeed = Get-RequiredJsonProperty `
            $proof 'endedWithLineFeed' "child $($expected.role) client log"
        $copied = Get-RequiredJsonProperty `
            $proof 'copied' "child $($expected.role) client log"
        $sourceAndArtifactMatch = Get-RequiredJsonProperty `
            $proof 'sourceAndArtifactMatch' "child $($expected.role) client log"
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $proof 'rotatedSegments' "child $($expected.role) client log") `
            0 "child $($expected.role) client log rotatedSegments")
        $expectedArtifactPath = [IO.Path]::GetFullPath(
            (Join-Path $ChildArtifactDir $expectedFileName))
        [void](Assert-SameFullPath `
            (Get-RequiredJsonProperty $proof 'artifactPath' "child $($expected.role) client log") `
            $expectedArtifactPath "child $($expected.role) client log artifactPath")
        if ($length -le 0 -or $sha256 -isnot [string] -or
            $sha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
            $endedWithLineFeed -isnot [bool] -or -not $endedWithLineFeed -or
            $copied -isnot [bool] -or -not $copied -or
            $sourceAndArtifactMatch -isnot [bool] -or -not $sourceAndArtifactMatch) {
            Throw-CampaignFailure 'client-log-completion-unproven' `
                "child $($expected.role) client-log proof is incomplete"
        }
        if (-not (Test-Path -LiteralPath $expectedArtifactPath -PathType Leaf)) {
            Throw-CampaignFailure 'client-log-artifact-missing' `
                "child $($expected.role) log artifact is missing: $expectedArtifactPath"
        }
        $artifactBytes = [IO.File]::ReadAllBytes($expectedArtifactPath)
        $artifactHash = (Get-FileHash -LiteralPath $expectedArtifactPath `
            -Algorithm SHA256).Hash
        if ($artifactBytes.Length -ne $length -or $artifactBytes.Length -eq 0 -or
            $artifactBytes[$artifactBytes.Length - 1] -ne 0x0A -or
            $artifactHash -ne $sha256) {
            Throw-CampaignFailure 'client-log-artifact-mismatch' `
                "child $($expected.role) log bytes do not match the completion proof"
        }
    }
    return $clientLogs
}

function Assert-CommonChildSummary {
    param(
        [Parameter(Mandatory)][object]$Summary,
        [Parameter(Mandatory)][string]$ExpectedMode,
        [Parameter(Mandatory)][string]$ChildArtifactDir,
        [Parameter(Mandatory)][string]$ExpectedBarrierOrder
    )
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $Summary 'gameplayMode' 'child summary') `
        $ExpectedMode 'child gameplayMode')
    [void](Assert-SameFullPath `
        (Get-RequiredJsonProperty $Summary 'gameDir' 'child summary') `
        $GameDir 'child gameDir')
    [void](Assert-SameFullPath `
        (Get-RequiredJsonProperty $Summary 'fixtureManifest' 'child summary') `
        $FixtureManifest 'child fixtureManifest')
    [void](Assert-SameFullPath `
        (Get-RequiredJsonProperty $Summary 'artifacts' 'child summary') `
        $ChildArtifactDir 'child artifacts')

    [long]$hostPid = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $Summary 'hostPid' 'child summary') 'child hostPid'
    [long]$joinPid = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $Summary 'joinPid' 'child summary') 'child joinPid'
    if ($hostPid -le 0 -or $joinPid -le 0 -or $hostPid -eq $joinPid) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "child did not record two distinct positive owned client PIDs ($hostPid/$joinPid)"
    }

    [void](Assert-ChildClientLogCompletion `
        -Summary $Summary -HostPid $hostPid -JoinPid $joinPid `
        -ChildArtifactDir $ChildArtifactDir)

    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty $Summary 'mergeDay' 'child summary') `
        $script:FixtureMergeDay 'child mergeDay')
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $Summary 'barrierOrder' 'child summary') `
        $ExpectedBarrierOrder 'child barrierOrder')
    return [pscustomobject]@{
        hostPid = $hostPid
        joinPid = $joinPid
    }
}

function Assert-ExactAutoBattleProof {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string[]]$ExpectedRoles,
        [Parameter(Mandatory)][string]$Context
    )
    $proof = Assert-JsonObject $Value $Context
    $roles = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $proof 'roles' $Context) "$Context roles"
    if ($roles.Count -ne $ExpectedRoles.Count) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$Context has $($roles.Count) role proofs, expected $($ExpectedRoles.Count)"
    }

    $seen = @{}
    foreach ($entryValue in $roles) {
        $entry = Assert-JsonObject $entryValue "$Context role proof"
        $roleValue = Get-RequiredJsonProperty $entry 'role' "$Context role proof"
        if ($roleValue -isnot [string] -or $roleValue -notin $ExpectedRoles) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$Context contains unexpected role '$roleValue'"
        }
        if ($seen.ContainsKey($roleValue)) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$Context contains duplicate role '$roleValue'"
        }
        $seen[$roleValue] = $true

        $found = Get-RequiredJsonProperty $entry 'found' "$Context $roleValue"
        if ($found -isnot [bool] -or -not $found) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$Context $roleValue did not resolve the exact bound functor"
        }
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $entry 'error' "$Context $roleValue") '' `
            "$Context $roleValue error")
        $kick = Assert-JsonObject `
            (Get-RequiredJsonProperty $entry 'kick' "$Context $roleValue") `
            "$Context $roleValue kick"
        $succeeded = Get-RequiredJsonProperty $kick 'succeeded' "$Context $roleValue kick"
        if ($succeeded -isnot [bool] -or -not $succeeded) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$Context $roleValue did not commit its one callback"
        }
        foreach ($check in @(
            @{ name = 'controllerGateBefore'; expected = 0 },
            @{ name = 'kickStateBefore'; expected = 0 },
            @{ name = 'kickStateAfter'; expected = 1 },
            # All 30 actions in the old canonical 15/15 Russobit campaign used
            # side3A=1 and therefore selected flag38, leaving flag39 unchanged.
            @{ name = 'sideSelector'; expected = 1 },
            @{ name = 'flag38Before'; expected = 0 },
            @{ name = 'flag38After'; expected = 1 },
            @{ name = 'flag39Before'; expected = 0 },
            @{ name = 'flag39After'; expected = 0 },
            @{ name = 'memberFunction'; expected = 0x00635509 }
        )) {
            [void](Assert-ExactInteger `
                (Get-RequiredJsonProperty $kick $check.name "$Context $roleValue kick") `
                $check.expected "$Context $roleValue kick $($check.name)")
        }
    }
    foreach ($expectedRole in $ExpectedRoles) {
        if (-not $seen.ContainsKey($expectedRole)) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$Context omitted expected role '$expectedRole'"
        }
    }

    # The pre-boot auto-battle subscriber has no common dispatch timestamp: the
    # old clients independently produced one lifecycle log record per battle.
    # Preserve that honest provenance instead of manufacturing a skew number.
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $proof 'source' $Context) `
        'observed-lifecycle-log' "$Context source")
    $timestampsAvailable = Get-RequiredJsonProperty `
        $proof 'dispatchTimestampsAvailable' $Context
    if ($timestampsAvailable -isnot [bool] -or $timestampsAvailable) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$Context must record dispatchTimestampsAvailable=false"
    }
    $skewValue = Get-RequiredJsonProperty $proof 'dispatchSkewMs' $Context
    if ($null -ne $skewValue) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$Context observed lifecycle proof must record dispatchSkewMs=null"
    }
    return $proof
}

function Assert-CanonicalLegacyEndTurnProof {
    param(
        [Parameter(Mandatory)][object]$Round
    )
    $context = 'canonical independent round endTurnProof'
    $proof = Assert-JsonObject `
        (Get-RequiredJsonProperty $Round 'endTurnProof' 'canonical independent round') `
        $context

    foreach ($check in @(
        @{ name = 'confirmationCount'; expected = 0 },
        @{ name = 'stockEndTurnTotal'; expected = 0 },
        @{ name = 'trackedProcessesAlive'; expected = 2 }
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $proof $check.name $context) `
            $check.expected "$context $($check.name)")
    }

    [long]$firstObservationSec = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $proof 'firstObservationSec' $context) `
        "$context firstObservationSec"
    if ($firstObservationSec -lt 3 -or $firstObservationSec -gt 45 -or
        ($firstObservationSec % 3) -ne 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context firstObservationSec=$firstObservationSec, expected a 3-second cadence within 45 seconds"
    }

    [double]$dispatchSkewMs = Convert-RequiredFiniteDouble `
        (Get-RequiredJsonProperty $proof 'dispatchSkewMs' $context) `
        "$context dispatchSkewMs"
    if ($dispatchSkewMs -lt 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context dispatchSkewMs is negative: $dispatchSkewMs"
    }
    [double]$roundDispatchSkewMs = Convert-RequiredFiniteDouble `
        (Get-RequiredJsonProperty $Round 'dispatchSkewMs' 'canonical independent round') `
        'canonical independent round dispatchSkewMs'
    if ($roundDispatchSkewMs -lt 0 -or $roundDispatchSkewMs -ne $dispatchSkewMs) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("canonical independent round dispatch skew does not preserve the exact " +
             "End Turn proof value ($roundDispatchSkewMs/$dispatchSkewMs)")
    }

    $players = Assert-JsonObject `
        (Get-RequiredJsonProperty $script:Fixture 'players' 'fixture manifest') `
        'fixture players'
    $expectedHandles = @{
        host = Convert-RequiredPlayerHandle `
            (Get-RequiredJsonProperty $players 'hostHandle' 'fixture players') `
            'fixture players hostHandle'
        join = Convert-RequiredPlayerHandle `
            (Get-RequiredJsonProperty $players 'joinHandle' 'fixture players') `
            'fixture players joinHandle'
    }
    [long]$expectedDay = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $Round 'hostBefore' 'canonical independent round') `
        'canonical independent round hostBefore'
    $expectedDay++

    $cascades = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $proof 'cascades' $context) `
        "$context cascades"
    if ($cascades.Count -ne 2) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context has $($cascades.Count) cascade records, expected exactly two"
    }
    $seenRoles = @{}
    $orderedRecords = [System.Collections.Generic.List[object]]::new()
    foreach ($cascadeValue in $cascades) {
        $cascade = Assert-JsonObject $cascadeValue "$context cascade record"
        $role = Get-RequiredJsonProperty $cascade 'role' "$context cascade record"
        if ($role -isnot [string] -or $role -notin @('host', 'join')) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context contains unexpected cascade role '$role'"
        }
        if ($seenRoles.ContainsKey($role)) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context contains duplicate cascade role '$role'"
        }
        $seenRoles[$role] = $true

        [long]$handle = Convert-RequiredPlayerHandle `
            (Get-RequiredJsonProperty $cascade 'handle' "$context $role cascade") `
            "$context $role handle"
        if ($handle -ne [long]$expectedHandles[$role]) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context $role handle does not match the exact fixture player handle"
        }
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $cascade 'day' "$context $role cascade") `
            $expectedDay "$context $role day")

        [long]$actionId = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $cascade 'actionId' "$context $role cascade") `
            "$context $role actionId"
        [long]$activationActionId = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $cascade 'activationActionId' "$context $role cascade") `
            "$context $role activationActionId"
        [long]$completionActionId = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $cascade 'completionActionId' "$context $role cascade") `
            "$context $role completionActionId"
        if ($actionId -le 0 -or
            $activationActionId -ne $actionId -or
            $completionActionId -ne $actionId) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("$context $role action linkage is invalid " +
                 "($actionId/$activationActionId/$completionActionId)")
        }
        [long]$lease = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $cascade 'lease' "$context $role cascade") `
            "$context $role lease"
        if ($lease -le 0) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context $role cascade has non-positive authoritative lease $lease"
        }

        [long]$dispatchIndex = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $cascade 'dispatchIndex' "$context $role cascade") `
            "$context $role dispatchIndex"
        [long]$activationIndex = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $cascade 'activationIndex' "$context $role cascade") `
            "$context $role activationIndex"
        [long]$completionIndex = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $cascade 'completionIndex' "$context $role cascade") `
            "$context $role completionIndex"
        if ($dispatchIndex -lt 0 -or
            $activationIndex -le $dispatchIndex -or
            $completionIndex -le $activationIndex) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("$context $role has an invalid apply/activate/completion interval " +
                 "($dispatchIndex -> $activationIndex -> $completionIndex)")
        }
        $orderedRecords.Add([pscustomobject]@{
            role = $role
            actionId = $actionId
            lease = $lease
            dispatchIndex = $dispatchIndex
            activationIndex = $activationIndex
            completionIndex = $completionIndex
        })
    }
    foreach ($role in @('host', 'join')) {
        if (-not $seenRoles.ContainsKey($role)) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context omitted the '$role' cascade record"
        }
    }

    $ordered = @($orderedRecords | Sort-Object dispatchIndex)
    if ($ordered[0].actionId -eq $ordered[1].actionId -or
        $ordered[0].lease -eq $ordered[1].lease) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context reused an actionId or turn lease across the two players"
    }
    if ($ordered[0].dispatchIndex -ge $ordered[0].activationIndex -or
        $ordered[0].activationIndex -ge $ordered[0].completionIndex -or
        $ordered[0].completionIndex -ge $ordered[1].dispatchIndex -or
        $ordered[1].dispatchIndex -ge $ordered[1].activationIndex -or
        $ordered[1].activationIndex -ge $ordered[1].completionIndex) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context does not prove serialized cascade order " +
             '(first apply < activate < completion < second apply < activate < completion)')
    }
    return $proof
}

function Assert-CanonicalSummary {
    param(
        [Parameter(Mandatory)][object]$Summary,
        [Parameter(Mandatory)][string]$ChildArtifactDir,
        [Parameter(Mandatory)][string]$ExpectedBarrierOrder,
        [Parameter(Mandatory)][ValidateSet('none')]
        [string]$ExpectedPostMergeContinuationMode
    )
    $common = Assert-CommonChildSummary `
        $Summary canonical $ChildArtifactDir $ExpectedBarrierOrder

    $reinforcement = Assert-JsonObject `
        (Get-RequiredJsonProperty $Summary 'reinforcement' 'canonical summary') `
        'canonical reinforcement'
    foreach ($check in @(
        @{ name = 'hostCommits'; expected = 1 },
        @{ name = 'joinCommits'; expected = 0 },
        @{ name = 'hostUnits'; expected = [int]$script:Fixture.host.reinforcement.units },
        @{ name = 'joinUnits'; expected = [int]$script:Fixture.join.reinforcement.units }
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $reinforcement $check.name 'canonical reinforcement') `
            $check.expected "canonical reinforcement $($check.name)")
    }
    $replicatedTo = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $reinforcement 'replicatedTo' 'canonical reinforcement') `
        'canonical reinforcement replicatedTo'
    if ($replicatedTo.Count -ne 2) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "canonical reinforcement replicatedTo has $($replicatedTo.Count) entries, expected 2"
    }
    [void](Assert-ExactString $replicatedTo[0] 'host' `
        'canonical reinforcement replicatedTo[0]')
    [void](Assert-ExactString $replicatedTo[1] 'join' `
        'canonical reinforcement replicatedTo[1]')

    $gameplay = Assert-JsonObject `
        (Get-RequiredJsonProperty $Summary 'gameplay' 'canonical summary') `
        'canonical gameplay'
    $attacks = Assert-JsonObject `
        (Get-RequiredJsonProperty $gameplay 'attacks' 'canonical gameplay') `
        'canonical gameplay attacks'
    [void](Assert-ExactAutoBattleProof `
        (Get-RequiredJsonProperty $attacks 'autoBattle' 'canonical attacks') `
        @('host', 'join') 'canonical attacks auto-battle')
    $battleEndMovement = @{}
    foreach ($role in @('host', 'join')) {
        $result = Assert-JsonObject `
            (Get-RequiredJsonProperty $attacks $role 'canonical attacks') `
            "canonical $role attack result"
        $spec = $script:Fixture.$role
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $result 'heroId' "canonical $role attack result") `
            ([string]$spec.heroId) "canonical $role attack heroId")
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $result 'targetId' "canonical $role attack result") `
            ([string]$spec.target.id) "canonical $role attack targetId")
        foreach ($check in @(
            @{ name = 'x'; expected = [int]$spec.battleEnd.x },
            @{ name = 'y'; expected = [int]$spec.battleEnd.y }
        )) {
            [void](Assert-ExactInteger `
                (Get-RequiredJsonProperty $result $check.name "canonical $role attack result") `
                $check.expected "canonical $role attack $($check.name)")
        }
        $battleEndMovement[$role] = Assert-ExactInteger `
            (Get-RequiredJsonProperty $result 'movement' "canonical $role attack result") `
            ([int]$spec.battleEnd.movement) "canonical $role attack movement"
        if ((Get-RequiredJsonProperty $result 'battleClosed' "canonical $role attack result") -ne $true) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "canonical $role attack did not preserve battleClosed=true"
        }
    }
    $postBattleWalk = Assert-JsonObject `
        (Get-RequiredJsonProperty $gameplay 'postBattleWalk' 'canonical gameplay') `
        'canonical post-battle walk'
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $postBattleWalk 'phase' 'canonical post-battle walk') `
        'postBattleWalk' 'canonical post-battle walk phase')
    $postBattleMovementAfter = @{}
    foreach ($role in @('host', 'join')) {
        $context = "canonical post-battle walk $role"
        $actual = Assert-JsonObject `
            (Get-RequiredJsonProperty $postBattleWalk $role 'canonical post-battle walk') `
            $context
        $source = $script:Fixture.$role.battleEnd
        $expected = $script:Fixture.$role.postBattleWalk
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $actual 'role' $context) `
            $role "$context role")
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $actual 'id' $context) `
            ([string]$script:Fixture.$role.heroId) "$context id")
        foreach ($check in @(
            @{ name = 'fromX'; expected = [int]$source.x },
            @{ name = 'fromY'; expected = [int]$source.y },
            @{ name = 'x'; expected = [int]$expected.x },
            @{ name = 'y'; expected = [int]$expected.y }
        )) {
            [void](Assert-ExactInteger `
                (Get-RequiredJsonProperty $actual $check.name $context) `
                $check.expected "$context $($check.name)")
        }
        [long]$movementBefore = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $actual 'movementBefore' $context) `
            "$context movementBefore"
        [long]$movementAfter = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $actual 'movementAfter' $context) `
            "$context movementAfter"
        [long]$movementAlias = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $actual 'movement' $context) `
            "$context movement"
        if ($movementBefore -ne [long]$battleEndMovement[$role]) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("$context movementBefore=$movementBefore does not continue " +
                 "the attack result MP=$($battleEndMovement[$role])")
        }
        if ($movementAfter -lt 0 -or $movementAfter -ge $movementBefore) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("$context movement $movementBefore->$movementAfter does not " +
                 'satisfy the source oracle 0 <= after < before')
        }
        if ($movementAlias -ne $movementAfter) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context movement=$movementAlias does not equal movementAfter=$movementAfter"
        }
        foreach ($name in @('moved', 'charged')) {
            $value = Get-RequiredJsonProperty $actual $name $context
            if ($value -isnot [bool] -or -not $value) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "$context $name must be the boolean true"
            }
        }
        $postBattleMovementAfter[$role] = $movementAfter
    }
    $postBattleStray = Get-RequiredJsonProperty `
        $postBattleWalk 'strayBattle' 'canonical post-battle walk'
    if ($postBattleStray -isnot [bool] -or $postBattleStray) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'canonical post-battle walk must prove strayBattle=false'
    }

    $day2Walk = Assert-JsonObject `
        (Get-RequiredJsonProperty $Summary 'day2Walk' 'canonical summary') `
        'canonical day-2 walk'
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $day2Walk 'phase' 'canonical day-2 walk') `
        'day2Reverse' 'canonical day-2 walk phase')
    $day2MovementBefore = @{}
    $day2MovementAfter = @{}
    foreach ($role in @('host', 'join')) {
        $context = "canonical day-2 walk $role"
        $actual = Assert-JsonObject `
            (Get-RequiredJsonProperty $day2Walk $role 'canonical day-2 walk') `
            $context
        $source = $script:Fixture.$role.postBattleWalk
        $expected = $script:Fixture.$role.day2Reverse
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $actual 'role' $context) `
            $role "$context role")
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $actual 'id' $context) `
            ([string]$script:Fixture.$role.heroId) "$context id")
        foreach ($check in @(
            @{ name = 'fromX'; expected = [int]$source.x },
            @{ name = 'fromY'; expected = [int]$source.y },
            @{ name = 'x'; expected = [int]$expected.x },
            @{ name = 'y'; expected = [int]$expected.y }
        )) {
            [void](Assert-ExactInteger `
                (Get-RequiredJsonProperty $actual $check.name $context) `
                $check.expected "$context $($check.name)")
        }
        [long]$movementBefore = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $actual 'movementBefore' $context) `
            "$context movementBefore"
        [long]$movementAfter = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $actual 'movementAfter' $context) `
            "$context movementAfter"
        [long]$movementAlias = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $actual 'movement' $context) `
            "$context movement"
        if ($movementAfter -lt 0 -or $movementAfter -ge $movementBefore) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("$context movement $movementBefore->$movementAfter does not " +
                 'satisfy the source oracle 0 <= after < before')
        }
        if ($movementAlias -ne $movementAfter) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context movement=$movementAlias does not equal movementAfter=$movementAfter"
        }
        foreach ($name in @('moved', 'charged')) {
            $value = Get-RequiredJsonProperty $actual $name $context
            if ($value -isnot [bool] -or -not $value) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "$context $name must be the boolean true"
            }
        }
        $day2MovementBefore[$role] = $movementBefore
        $day2MovementAfter[$role] = $movementAfter
    }
    $day2Stray = Get-RequiredJsonProperty `
        $day2Walk 'strayBattle' 'canonical day-2 walk'
    if ($day2Stray -isnot [bool] -or $day2Stray) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'canonical day-2 walk must prove strayBattle=false'
    }

    $rounds = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $Summary 'independentRounds' 'canonical summary') `
        'canonical independentRounds'
    if ($rounds.Count -ne 1) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "canonical summary has $($rounds.Count) independent rounds, expected exactly one"
    }
    $round = Assert-JsonObject $rounds[0] 'canonical independent round'
    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty $round 'round' 'canonical independent round') `
        1 'canonical independent round number')
    foreach ($check in @(
        @{ name = 'hostBefore'; expected = 1 },
        @{ name = 'hostAfter'; expected = 2 },
        @{ name = 'joinBefore'; expected = 1 },
        @{ name = 'joinAfter'; expected = 2 }
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $round $check.name 'canonical independent round') `
            $check.expected "canonical independent round $($check.name)")
    }
    [void](Assert-CanonicalLegacyEndTurnProof $round)
    $roundMovementBefore = Assert-JsonObject `
        (Get-RequiredJsonProperty $round 'movementBefore' 'canonical independent round') `
        'canonical independent round movementBefore'
    foreach ($role in @('host', 'join')) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $roundMovementBefore $role `
                'canonical independent round movementBefore') `
            ([long]$postBattleMovementAfter[$role]) `
            "canonical independent round $role movement before")
    }
    $movementRefresh = Assert-JsonObject `
        (Get-RequiredJsonProperty $round 'movementRefresh' 'canonical independent round') `
        'canonical independent round movementRefresh'
    foreach ($role in @('host', 'join')) {
        [long]$refreshedMovement = Assert-ExactInteger `
            (Get-RequiredJsonProperty $movementRefresh $role `
                'canonical independent round movementRefresh') `
            35 "canonical independent round $role movement refresh"
        if ([long]$day2MovementBefore[$role] -ne $refreshedMovement) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("canonical day-2 walk $role movementBefore=" +
                 "$($day2MovementBefore[$role]) does not continue the independent " +
                 "round refresh=$refreshedMovement")
        }
    }
    $aggregateHp = @{}
    foreach ($name in @('hpBefore', 'hpAfter')) {
        $hp = Assert-JsonObject `
            (Get-RequiredJsonProperty $round $name 'canonical independent round') `
            "canonical independent round $name"
        $aggregateHp[$name] = @{}
        foreach ($role in @('host', 'join')) {
            [long]$value = Convert-RequiredInt64 `
                (Get-RequiredJsonProperty $hp $role "canonical independent round $name") `
                "canonical independent round $name $role"
            if ($value -le 0) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "canonical independent round $name $role is not positive: $value"
            }
            $aggregateHp[$name][$role] = $value
        }
    }

    foreach ($name in @('unitHpBefore', 'unitHpAfter')) {
        $snapshot = Assert-JsonObject `
            (Get-RequiredJsonProperty $round $name 'canonical independent round') `
            "canonical independent round $name"
        $aggregateName = if ($name -eq 'unitHpBefore') { 'hpBefore' } else { 'hpAfter' }
        foreach ($role in @('host', 'join')) {
            $hero = Assert-JsonObject `
                (Get-RequiredJsonProperty $snapshot $role "canonical independent round $name") `
                "canonical independent round $name $role"
            $fixtureHero = Assert-JsonObject `
                (Get-RequiredJsonProperty $script:Fixture $role 'fixture manifest') `
                "fixture $role"
            $reinforcement = Assert-JsonObject `
                (Get-RequiredJsonProperty $fixtureHero 'reinforcement' "fixture $role") `
                "fixture $role reinforcement"
            [void](Assert-ExactString `
                (Get-RequiredJsonProperty $hero 'heroId' "canonical independent round $name $role") `
                (Get-RequiredJsonProperty $fixtureHero 'heroId' "fixture $role") `
                "canonical independent round $name $role heroId")
            [long]$totalHp = Convert-RequiredInt64 `
                (Get-RequiredJsonProperty $hero 'totalHp' "canonical independent round $name $role") `
                "canonical independent round $name $role totalHp"
            if ($totalHp -ne [long]$aggregateHp[$aggregateName][$role]) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    ("canonical independent round $name $role totalHp=$totalHp " +
                     "does not match $aggregateName=$($aggregateHp[$aggregateName][$role])")
            }

            $fixtureUnitIds = Get-RequiredJsonArray `
                (Get-RequiredJsonProperty $reinforcement 'unitIds' "fixture $role reinforcement") `
                "fixture $role reinforcement unitIds"
            $expectedIds = @(
                [string](Get-RequiredJsonProperty $reinforcement 'leaderId' "fixture $role reinforcement")
            ) + @($fixtureUnitIds | ForEach-Object { [string]$_ })
            $units = Get-RequiredJsonArray `
                (Get-RequiredJsonProperty $hero 'units' "canonical independent round $name $role") `
                "canonical independent round $name $role units"
            if ($units.Count -ne $expectedIds.Count) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "canonical independent round $name $role has $($units.Count) unit HP records, expected $($expectedIds.Count)"
            }
            [long]$sum = 0
            for ($index = 0; $index -lt $expectedIds.Count; $index++) {
                $unit = Assert-JsonObject $units[$index] `
                    "canonical independent round $name $role unit $index"
                [void](Assert-ExactString `
                    (Get-RequiredJsonProperty $unit 'id' "canonical independent round $name $role unit $index") `
                    $expectedIds[$index] `
                    "canonical independent round $name $role unit $index id")
                [long]$unitHp = Convert-RequiredInt64 `
                    (Get-RequiredJsonProperty $unit 'hp' "canonical independent round $name $role unit $index") `
                    "canonical independent round $name $role unit $index hp"
                if ($unitHp -lt 0) {
                    Throw-CampaignFailure 'acceptance-mismatch' `
                        "canonical independent round $name $role unit $($expectedIds[$index]) has negative HP $unitHp"
                }
                $sum += $unitHp
            }
            if ($sum -ne $totalHp) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "canonical independent round $name $role unit HP sum=$sum, expected totalHp=$totalHp"
            }
        }
    }

    $merge = Assert-JsonObject `
        (Get-RequiredJsonProperty $Summary 'merge' 'canonical summary') `
        'canonical merge'
    foreach ($check in @(
        @{ name = 'mergeDay'; expected = $script:FixtureMergeDay },
        @{ name = 'hostBefore'; expected = $script:FixtureMergeDay - 1 },
        @{ name = 'joinBefore'; expected = $script:FixtureMergeDay - 1 },
        @{ name = 'hostAfter'; expected = $script:FixtureMergeDay },
        @{ name = 'joinAfter'; expected = $script:FixtureMergeDay }
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $merge $check.name 'canonical merge') `
            $check.expected "canonical merge $($check.name)")
    }
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $merge 'order' 'canonical merge') `
        $ExpectedBarrierOrder 'canonical merge order')
    $barrierRole = Get-RequiredJsonProperty $merge 'barrierRole' 'canonical merge'
    if ($barrierRole -isnot [string] -or $barrierRole -notin @('host', 'join')) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "canonical merge has invalid barrierRole '$barrierRole'"
    }
    if ($ExpectedBarrierOrder -ne 'parallel') {
        $expectedBarrierRole = if ($ExpectedBarrierOrder -eq 'host-first') {
            'host'
        } else {
            'join'
        }
        [void](Assert-ExactString `
            $barrierRole $expectedBarrierRole 'canonical merge barrierRole')
    }
    $mergeMovementBefore = Assert-JsonObject `
        (Get-RequiredJsonProperty $merge 'movementBefore' 'canonical merge') `
        'canonical merge movementBefore'
    foreach ($role in @('host', 'join')) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $mergeMovementBefore $role `
                'canonical merge movementBefore') `
            ([long]$day2MovementAfter[$role]) `
            "canonical merge $role movementBefore")
    }
    $mergeMovementAfter = Assert-JsonObject `
        (Get-RequiredJsonProperty $merge 'movementAfter' 'canonical merge') `
        'canonical merge movementAfter'
    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty $mergeMovementAfter 'host' 'canonical merge movementAfter') `
        35 'canonical merge host movementAfter')
    [void](Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $mergeMovementAfter 'join' 'canonical merge movementAfter') `
        'canonical merge join movementAfter observation')

    $firstMergeObservationValue = Get-RequiredJsonProperty `
        $merge 'firstMergeObservationSec' 'canonical merge'
    $confirmationCountValue = Get-RequiredJsonProperty `
        $merge 'confirmationCount' 'canonical merge'
    $currentOwnerValue = Get-RequiredJsonProperty `
        $merge 'currentOwner' 'canonical merge'
    if ($ExpectedBarrierOrder -eq 'parallel') {
        [long]$firstMergeObservationSec = Convert-RequiredInt64 `
            $firstMergeObservationValue 'canonical merge firstMergeObservationSec'
        if ($firstMergeObservationSec -lt 3 -or $firstMergeObservationSec -gt 45 -or
            ($firstMergeObservationSec % 3) -ne 0) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("canonical merge firstMergeObservationSec=$firstMergeObservationSec, " +
                 'expected a 3-second cadence within 45 seconds')
        }
        [void](Assert-ExactInteger `
            $confirmationCountValue 0 'canonical merge confirmationCount')

        # The runtime checks both host and join world snapshots and publishes
        # their common verified owner as this single result object.
        $currentOwner = Assert-JsonObject $currentOwnerValue 'canonical merge currentOwner'
        [long]$actualOwner = Convert-RequiredPlayerHandle `
            (Get-RequiredJsonProperty $currentOwner 'activePlayerId' `
                'canonical merge currentOwner') `
            'canonical merge currentOwner activePlayerId'
        $players = Assert-JsonObject `
            (Get-RequiredJsonProperty $script:Fixture 'players' 'fixture manifest') `
            'fixture players'
        [long]$expectedOwner = Convert-RequiredPlayerHandle `
            (Get-RequiredJsonProperty $players 'hostHandle' 'fixture players') `
            'fixture players hostHandle'
        if ($actualOwner -ne $expectedOwner) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                'canonical merge current owner does not match the exact fixture host handle'
        }
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $currentOwner 'hostDialog' `
                'canonical merge currentOwner') `
            'DLG_STRATEGIC' 'canonical merge currentOwner hostDialog')
    } else {
        foreach ($field in @(
            @{ name = 'firstMergeObservationSec'; value = $firstMergeObservationValue },
            @{ name = 'confirmationCount'; value = $confirmationCountValue },
            @{ name = 'currentOwner'; value = $currentOwnerValue }
        )) {
            if ($null -ne $field.value) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    ("canonical ordered merge unexpectedly claimed legacy-parallel " +
                     "evidence '$($field.name)'")
            }
        }
    }

    $legacyBarrierPass = Get-RequiredJsonProperty `
        $merge 'legacyBarrierPass' 'canonical merge'
    $expectedLegacyBarrierPass = $ExpectedBarrierOrder -eq 'parallel'
    if ($legacyBarrierPass -isnot [bool] -or
        $legacyBarrierPass -ne $expectedLegacyBarrierPass) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("canonical merge legacyBarrierPass='$legacyBarrierPass', expected " +
             "'$expectedLegacyBarrierPass' for order '$ExpectedBarrierOrder'")
    }
    [long]$turnBeforeFire = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $merge 'turnWatermarkBeforeFire' 'canonical merge') `
        'canonical merge turnWatermarkBeforeFire'
    [long]$preClickWatermark = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $merge 'preClickTurnWatermark' 'canonical merge') `
        'canonical merge preClickTurnWatermark'
    if ($preClickWatermark -le $turnBeforeFire) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("canonical merge did not retain the pre-fire -> pre-click telemetry interval " +
             "($turnBeforeFire -> $preClickWatermark)")
    }
    $firstHostAction = Assert-JsonObject `
        (Get-RequiredJsonProperty $merge 'firstHostAction' 'canonical merge') `
        'canonical merge firstHostAction'
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $firstHostAction 'actionRole' 'canonical merge firstHostAction') `
        'host' 'canonical merge firstHostAction role')
    [long]$firstObservationSec = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $firstHostAction 'firstObservationSec' `
            'canonical merge firstHostAction') `
        'canonical merge firstHostAction firstObservationSec'
    if ($firstObservationSec -lt 3 -or $firstObservationSec -gt 15 -or
        ($firstObservationSec % 3) -ne 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "canonical merge first host proof used invalid observation time +${firstObservationSec}s"
    }
    [long]$firstHostWatermark = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $firstHostAction 'turnWatermark' `
            'canonical merge firstHostAction') `
        'canonical merge firstHostAction turnWatermark'
    if ($firstHostWatermark -le $preClickWatermark) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'canonical merge first host action did not append its exact stock rotation evidence'
    }
    $firstHostEvidence = Assert-JsonObject `
        (Get-RequiredJsonProperty $firstHostAction 'evidence' 'canonical merge firstHostAction') `
        'canonical merge firstHostAction evidence'
    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty $firstHostEvidence 'day' `
            'canonical merge firstHostAction evidence') `
        $script:FixtureMergeDay 'canonical merge firstHostAction evidence day')
    [long]$firstActiveHandle = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $firstHostEvidence 'activeHandle' `
            'canonical merge firstHostAction evidence') `
        'canonical merge firstHostAction evidence activeHandle'
    if ($firstActiveHandle -le 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'canonical merge first host action did not select a nonzero join handle'
    }

    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $Summary 'postMergeContinuationMode' 'canonical summary') `
        $ExpectedPostMergeContinuationMode `
        'canonical postMergeContinuationMode')

    # MSS stock telemetry is intentionally outside the canonical campaign.
    # The old mass_test boundary leaves it null.
    if ($null -ne (Get-RequiredJsonProperty $Summary 'postMergeStock' 'canonical summary')) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'canonical acceptance child unexpectedly published MSS stock telemetry'
    }

    $automaticValue = Get-RequiredJsonProperty `
        $Summary 'postMergeAutomaticMasstestPhaseC' 'canonical summary'
    if ($null -ne $automaticValue) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'old mass_test-equivalent child crossed its BARRIER/dead-click PASS boundary'
    }
    return [pscustomobject]@{
        hostPid = $common.hostPid
        joinPid = $common.joinPid
        independentRounds = $rounds.Count
        mergeDay = $script:FixtureMergeDay
        postMergeContinuationMode = $ExpectedPostMergeContinuationMode
        barrierOrder = $ExpectedBarrierOrder
    }
}

function Assert-BattleStressLedger {
    param(
        [Parameter(Mandatory)][object]$Value,
        [Parameter(Mandatory)][object[]]$ExpectedSteps,
        [Parameter(Mandatory)][string]$HeroId,
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$BattleRole,
        [Parameter(Mandatory)][string]$Context
    )
    $steps = Get-RequiredJsonArray $Value $Context
    if ($steps.Count -ne $ExpectedSteps.Count) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$Context has $($steps.Count) actions, expected exactly $($ExpectedSteps.Count)"
    }
    [long]$lastLocalSequence = 0
    [long]$lastRemoteSequence = 0
    for ($index = 0; $index -lt $ExpectedSteps.Count; $index++) {
        $stepNumber = $index + 1
        $stepContext = "$Context step $stepNumber"
        $step = Assert-JsonObject $steps[$index] $stepContext
        $expected = $ExpectedSteps[$index]
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $step 'step' $stepContext) `
            $stepNumber "$stepContext number")
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $step 'id' $stepContext) `
            $HeroId "$stepContext hero id")
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $step 'battleRole' $stepContext) `
            $BattleRole "$stepContext battle role")
        foreach ($name in @(
                'commandIssued', 'moved', 'battleLiveBefore',
                'battleLiveAfter', 'sameBattleAppearance',
                'remoteReplicationProved', 'battleUiHistoryClosed'
            )) {
            $value = Get-RequiredJsonProperty $step $name $stepContext
            if ($value -isnot [bool] -or -not $value) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "$stepContext '$name' is not true"
            }
        }
        foreach ($field in @(
                'fromX', 'fromY', 'toX', 'toY',
                'movementBefore', 'movementAfter'
            )) {
            [long]$actual = Convert-RequiredInt64 `
                (Get-RequiredJsonProperty $step $field $stepContext) `
                "$stepContext $field"
            if ($actual -ne [long]$expected.$field) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "$stepContext $field=$actual, expected $($expected.$field)"
            }
        }
        [long]$localBefore = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $step 'localWorldSequenceBefore' $stepContext) `
            "$stepContext local world before"
        [long]$localAfter = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $step 'localWorldSequenceAfter' $stepContext) `
            "$stepContext local world after"
        [long]$remoteBefore = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $step 'remoteWorldSequenceBefore' $stepContext) `
            "$stepContext remote world before"
        [long]$remoteAfter = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $step 'remoteWorldSequenceAfter' $stepContext) `
            "$stepContext remote world after"
        [long]$appearance = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'battleAppearance' $stepContext) `
            "$stepContext battle appearance"
        [long]$owner = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'battleOwner' $stepContext) `
            "$stepContext battle owner"
        [long]$uiEventsAudited = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $step 'battleUiEventsAudited' $stepContext) `
            "$stepContext battle UI events audited"
        if ($localBefore -lt 1 -or $localAfter -le $localBefore -or
            $remoteBefore -lt 1 -or $remoteAfter -le $remoteBefore -or
            $localBefore -lt $lastLocalSequence -or
            $remoteBefore -lt $lastRemoteSequence -or
            $appearance -lt 1 -or $owner -lt 1 -or
            $uiEventsAudited -lt 0) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$stepContext does not carry monotonic two-client/live-battle evidence"
        }
        foreach ($phase in @('Before', 'After')) {
            $controls = Get-RequiredJsonArray `
                (Get-RequiredJsonProperty `
                    $step "battleLiveControls$phase" $stepContext) `
                "$stepContext battle controls $phase"
            $allowed = @(
                'BTN_DEFEND', 'BTN_RETREAT', 'BTN_WAIT',
                'BTN_RESOLVE', 'TOG_AUTOBATTLE'
            )
            if ($controls.Count -lt 1 -or
                @($controls | Sort-Object -Unique).Count -ne $controls.Count -or
                @($controls | Where-Object {
                    $_ -isnot [string] -or $allowed -notcontains [string]$_
                }).Count -ne 0 -or $controls -contains 'BTN_CLOSE') {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "$stepContext battle controls $phase are not exact live controls"
            }
        }
        $lastLocalSequence = $localAfter
        $lastRemoteSequence = $remoteAfter
    }
    return @($steps)
}

function Assert-BattleBlockSummary {
    param(
        [Parameter(Mandatory)][object]$Summary,
        [Parameter(Mandatory)][string]$ChildArtifactDir
    )
    $common = Assert-CommonChildSummary `
        $Summary 'battle-block' $ChildArtifactDir parallel
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $Summary 'postMergeContinuationMode' 'battle-block summary') `
        'none' 'battle-block postMergeContinuationMode')
    if ($null -ne (Get-RequiredJsonProperty `
            $Summary 'postMergeAutomaticMasstestPhaseC' 'battle-block summary')) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block child unexpectedly executed a post-merge continuation'
    }

    $battleBlock = Assert-JsonObject `
        (Get-RequiredJsonProperty $Summary 'battleBlock' 'battle-block summary') `
        'battle-block result'
    [long]$freeSteps = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $battleBlock 'freeSteps' 'battle-block result') `
        'battle-block freeSteps'
    [long]$afterSteps = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $battleBlock 'afterSteps' 'battle-block result') `
        'battle-block afterSteps'
    [long]$continuousFreeSteps = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $battleBlock 'continuousFreeSteps' 'battle-block result') `
        'battle-block continuousFreeSteps'
    if ($freeSteps -ne $script:BattleBlockStepCount -or
        $continuousFreeSteps -ne $script:BattleBlockStepCount -or
        $afterSteps -ne 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("battle-block requires continuous FREE 4/4 and zero after-battle moves; " +
             "observed free=$freeSteps continuous=$continuousFreeSteps after=$afterSteps")
    }
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $battleBlock 'legacyVerdict' 'battle-block result') `
        'FREE' 'battle-block legacyVerdict')

    $steps = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $battleBlock 'steps' 'battle-block result') `
        'battle-block steps'
    if ($steps.Count -ne $script:BattleBlockStepCount) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("battle-block summary has $($steps.Count) step records/actions, expected exactly " +
             $script:BattleBlockStepCount)
    }

    [long]$classifiedFreeSteps = 0
    [long]$classifiedAfterSteps = 0
    [long]$classifiedContinuousFreeSteps = 0
    for ($index = 0; $index -lt $script:BattleBlockStepCount; $index++) {
        $stepNumber = $index + 1
        $step = Assert-JsonObject $steps[$index] "battle-block step $stepNumber"
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $step 'step' "battle-block step $stepNumber") `
            $stepNumber "battle-block step $stepNumber number")
        $commandIssued = Get-RequiredJsonProperty `
            $step 'commandIssued' "battle-block step $stepNumber"
        $moved = Get-RequiredJsonProperty `
            $step 'moved' "battle-block step $stepNumber"
        $hostInBattleBefore = Get-RequiredJsonProperty `
            $step 'hostInBattleBefore' "battle-block step $stepNumber"
        $hostInBattleAfter = Get-RequiredJsonProperty `
            $step 'hostInBattleAfter' "battle-block step $stepNumber"
        if ($commandIssued -isnot [bool] -or -not $commandIssued -or
            $moved -isnot [bool] -or
            $hostInBattleBefore -isnot [bool] -or
            $hostInBattleAfter -isnot [bool]) {
            Throw-CampaignFailure 'malformed-summary' `
                ("battle-block step $stepNumber must prove commandIssued=true " +
                 'and boolean moved/battle classifications')
        }

        [long]$fromX = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'fromX' "battle-block step $stepNumber") `
            "battle-block step $stepNumber fromX"
        [long]$fromY = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'fromY' "battle-block step $stepNumber") `
            "battle-block step $stepNumber fromY"
        [long]$toX = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'toX' "battle-block step $stepNumber") `
            "battle-block step $stepNumber toX"
        [long]$toY = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'toY' "battle-block step $stepNumber") `
            "battle-block step $stepNumber toY"
        [long]$movementBefore = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'movementBefore' "battle-block step $stepNumber") `
            "battle-block step $stepNumber movementBefore"
        [long]$movementAfter = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $step 'movementAfter' "battle-block step $stepNumber") `
            "battle-block step $stepNumber movementAfter"
        if (-not $moved -or -not $hostInBattleBefore -or -not $hostInBattleAfter -or
            $toX -ne ($fromX + 1) -or $toY -ne $fromY -or
            $movementAfter -ge $movementBefore) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("battle-block step $stepNumber was not one charged east action " +
                 'wholly observed during host battle')
        }
        if ($moved -and $hostInBattleBefore) {
            $classifiedFreeSteps++
        } elseif ($moved) {
            $classifiedAfterSteps++
        }
        if ($moved -and $hostInBattleBefore -and $hostInBattleAfter) {
            $classifiedContinuousFreeSteps++
        }
    }
    if ($classifiedFreeSteps -ne $freeSteps -or
        $classifiedAfterSteps -ne $afterSteps -or
        $classifiedContinuousFreeSteps -ne $continuousFreeSteps) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("battle-block counters do not match the four proved classifications: " +
             "reported free/after/continuous=$freeSteps/$afterSteps/$continuousFreeSteps, " +
             ("classified=$classifiedFreeSteps/$classifiedAfterSteps/" +
              "$classifiedContinuousFreeSteps"))
    }

    $deferredMss = Assert-JsonObject `
        (Get-RequiredJsonProperty $battleBlock 'deferredMss' 'battle-block result') `
        'battle-block deferredMss'
    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty $deferredMss 'commandsAudited' 'battle-block deferredMss') `
        $script:BattleBlockStepCount 'battle-block commandsAudited')
    $continuousProof = Get-RequiredJsonProperty `
        $deferredMss 'continuousBattleFourOfFourProved' 'battle-block deferredMss'
    if ($continuousProof -isnot [bool] -or -not $continuousProof) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block deferred MSS proof did not preserve continuous battle 4/4'
    }
    foreach ($proofName in @(
            'sourceBoundaryObservedBeforeHostBattleCompletion',
            'forwardStressFiveStepsProved',
            'hostBattleCompletedOnlyForReverseContinuation',
            'reverseCrossOneShotProved',
            'reverseHostExhaustionProved'
        )) {
        $proofValue = Get-RequiredJsonProperty `
            $deferredMss $proofName 'battle-block deferredMss'
        if ($proofValue -isnot [bool] -or -not $proofValue) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block deferred MSS proof '$proofName' is not true"
        }
    }

    $forwardStress = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $battleBlock 'forwardStress' 'battle-block result') `
        'battle-block forward stress'
    foreach ($check in @(
            @{ name = 'sourceStepCount'; expected = 4 },
            @{ name = 'stressStepCount'; expected = 5 },
            @{ name = 'totalCommands'; expected = 9 },
            @{ name = 'movementBefore'; expected = [int]$script:Fixture.join.deploy.movement },
            @{ name = 'movementAfter'; expected = 8 },
            @{ name = 'finalX'; expected = 18 },
            @{ name = 'finalY'; expected = 27 },
            @{ name = 'attackWireBudgetReserved'; expected = 6 }
        )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty `
                $forwardStress $check.name 'battle-block forward stress') `
            $check.expected "battle-block forward stress $($check.name)")
    }
    $forwardBattleLive = Get-RequiredJsonProperty `
        $forwardStress 'hostBattleStayedSameAndLive' 'battle-block forward stress'
    if ($forwardBattleLive -isnot [bool] -or -not $forwardBattleLive) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block forward stress did not preserve one exact live host battle'
    }
    $forwardStressSteps = @(Assert-BattleStressLedger `
        -Value (Get-RequiredJsonProperty `
            $forwardStress 'stressSteps' 'battle-block forward stress') `
        -ExpectedSteps @($script:Fixture.crossBattle.joinStressSteps) `
        -HeroId ([string]$script:Fixture.join.heroId) `
        -BattleRole host -Context 'battle-block forward stress ledger')
    $lastForward = Assert-JsonObject `
        $steps[$steps.Count - 1] 'battle-block last source forward step'
    $sourceHandoff = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $forwardStress 'sourceHandoff' 'battle-block forward stress') `
        'battle-block source-to-stress handoff'
    foreach ($check in @(
            @{ name = 'expectedX'; expected = [long]$lastForward.toX },
            @{ name = 'expectedY'; expected = [long]$lastForward.toY },
            @{ name = 'expectedMovement'; expected = [long]$lastForward.movementAfter }
        )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty `
                $sourceHandoff $check.name 'battle-block source-to-stress handoff') `
            $check.expected "battle-block source handoff $($check.name)")
    }
    $handoffBattleLive = Get-RequiredJsonProperty `
        $sourceHandoff 'hostBattleStayedSameAndLive' `
        'battle-block source-to-stress handoff'
    [long]$handoffAppearance = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $sourceHandoff 'hostBattleAppearance' `
            'battle-block source-to-stress handoff') `
        'battle-block source handoff battle appearance'
    [long]$handoffOwner = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $sourceHandoff 'hostBattleOwner' `
            'battle-block source-to-stress handoff') `
        'battle-block source handoff battle owner'
    [long]$handoffUiEvents = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $sourceHandoff 'battleUiEventsAudited' `
            'battle-block source-to-stress handoff') `
        'battle-block source handoff UI events audited'
    [long]$handoffUiBaseline = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $sourceHandoff 'battleUiBaselineSequence' `
            'battle-block source-to-stress handoff') `
        'battle-block source handoff UI baseline sequence'
    [long]$handoffUiFinal = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $sourceHandoff 'battleUiFinalSequence' `
            'battle-block source-to-stress handoff') `
        'battle-block source handoff UI final sequence'
    $handoffUiClosed = Get-RequiredJsonProperty `
        $sourceHandoff 'battleUiHistoryClosed' `
        'battle-block source-to-stress handoff'
    if ($handoffBattleLive -isnot [bool] -or -not $handoffBattleLive -or
        $handoffAppearance -lt 1 -or $handoffOwner -lt 1 -or
        $handoffUiEvents -lt 0 -or $handoffUiBaseline -lt 1 -or
        $handoffUiFinal -lt $handoffUiBaseline -or
        $handoffUiClosed -isnot [bool] -or -not $handoffUiClosed) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block source handoff did not preserve one exact live host battle'
    }
    foreach ($role in @('host', 'join')) {
        $roleProof = Assert-JsonObject `
            (Get-RequiredJsonProperty `
                $sourceHandoff $role 'battle-block source-to-stress handoff') `
            "battle-block source handoff $role world"
        [long]$baselineSequence = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $roleProof 'baselineSequence' "battle-block source handoff $role") `
            "battle-block source handoff $role baseline sequence"
        [long]$evidenceSequence = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $roleProof 'evidenceSequence' "battle-block source handoff $role") `
            "battle-block source handoff $role evidence sequence"
        [long]$currentSequence = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $roleProof 'currentSequence' "battle-block source handoff $role") `
            "battle-block source handoff $role current sequence"
        foreach ($field in @('x', 'y', 'movement')) {
            [long]$actual = Convert-RequiredInt64 `
                (Get-RequiredJsonProperty `
                    $roleProof $field "battle-block source handoff $role") `
                "battle-block source handoff $role $field"
            $sourceField = if ($field -eq 'movement') {
                'movementAfter'
            } else {
                "to$($field.ToUpperInvariant())"
            }
            if ($actual -ne [long]$lastForward.$sourceField) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "battle-block source handoff $role $field is not the FREE4 endpoint"
            }
        }
        if ($baselineSequence -lt 1 -or
            $evidenceSequence -le $baselineSequence -or
            $currentSequence -ne $evidenceSequence) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block source handoff $role lacks a causal post-watermark world event"
        }
    }
    $firstForwardStress = Assert-JsonObject `
        $forwardStressSteps[0] 'battle-block first forward stress step'
    if ([long]$firstForwardStress.fromX -ne [long]$lastForward.toX -or
        [long]$firstForwardStress.fromY -ne [long]$lastForward.toY -or
        [long]$firstForwardStress.movementBefore -ne
            [long]$lastForward.movementAfter) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block forward stress did not continue the source FREE 4/4 ledger'
    }

    $reverse = Assert-JsonObject `
        (Get-RequiredJsonProperty $battleBlock 'reverseCross' 'battle-block result') `
        'battle-block reverse cross result'
    foreach ($name in @('attempted', 'composedAfterForwardFourOfFour')) {
        $value = Get-RequiredJsonProperty $reverse $name 'battle-block reverse cross result'
        if ($value -isnot [bool] -or -not $value) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block reverse cross '$name' is not true"
        }
    }
    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty `
            $reverse 'sourcePostApplyGraceMilliseconds' 'battle-block reverse cross result') `
        1500 'battle-block reverse source post-apply grace')
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $reverse 'verdict' 'battle-block reverse cross result') `
        'FREE' 'battle-block reverse cross verdict')
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty `
            $reverse 'stressVerdict' 'battle-block reverse cross result') `
        'FREE' 'battle-block reverse cross stress verdict')
    $joinBattleCompletionAttempted = Get-RequiredJsonProperty `
        $reverse 'joinBattleCompletionAttempted' 'battle-block reverse cross result'
    if ($joinBattleCompletionAttempted -isnot [bool] -or
        $joinBattleCompletionAttempted) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block reverse cross must stop while the join battle is still live'
    }

    $hostBattleCompletion = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $reverse 'hostBattleCompletion' 'battle-block reverse cross result') `
        'battle-block reverse host completion'
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty `
            $hostBattleCompletion 'role' 'battle-block reverse host completion') `
        'host' 'battle-block reverse host completion role')
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty `
            $hostBattleCompletion 'heroId' 'battle-block reverse host completion') `
        ([string]$script:Fixture.host.heroId) `
        'battle-block reverse host completion heroId')
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty `
            $hostBattleCompletion 'targetId' 'battle-block reverse host completion') `
        ([string]$script:Fixture.host.target.id) `
        'battle-block reverse host completion targetId')
    $hostBattleClosed = Get-RequiredJsonProperty `
        $hostBattleCompletion 'battleClosed' 'battle-block reverse host completion'
    if ($hostBattleClosed -isnot [bool] -or -not $hostBattleClosed) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block did not complete the first host battle before reverse cross'
    }
    [long]$hostBattleX = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $hostBattleCompletion 'x' 'battle-block reverse host completion') `
        'battle-block reverse host completion x'
    [long]$hostBattleY = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $hostBattleCompletion 'y' 'battle-block reverse host completion') `
        'battle-block reverse host completion y'
    [long]$hostBattleMovement = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $hostBattleCompletion 'movement' 'battle-block reverse host completion') `
        'battle-block reverse host completion movement'
    if ($hostBattleX -ne [long]$script:Fixture.host.battleEnd.x -or
        $hostBattleY -ne [long]$script:Fixture.host.battleEnd.y -or
        $hostBattleMovement -ne [long]$script:Fixture.host.battleEnd.movement -or
        $hostBattleMovement -le 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block reverse cross did not start from the pinned host battle endpoint'
    }

    $joinAttack = Assert-JsonObject `
        (Get-RequiredJsonProperty $reverse 'joinAttack' 'battle-block reverse cross result') `
        'battle-block reverse join attack'
    $joinAttackIssued = Get-RequiredJsonProperty `
        $joinAttack 'commandIssued' 'battle-block reverse join attack'
    if ($joinAttackIssued -isnot [bool] -or -not $joinAttackIssued) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block reverse join attack was not issued exactly once'
    }
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $joinAttack 'id' 'battle-block reverse join attack') `
        ([string]$script:Fixture.join.heroId) 'battle-block reverse join attack id')
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty `
            $joinAttack 'targetId' 'battle-block reverse join attack') `
        ([string]$script:Fixture.crossBattle.joinTarget.id) `
        'battle-block reverse join attack targetId')
    foreach ($axis in @('X', 'Y')) {
        [long]$targetCoordinate = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $joinAttack "target$axis" 'battle-block reverse join attack') `
            "battle-block reverse join attack target$axis"
        [long]$fixtureCoordinate = $script:Fixture.crossBattle.joinTarget.$(
            $axis.ToLowerInvariant())
        if ($targetCoordinate -ne $fixtureCoordinate) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block reverse join attack target$axis drifted from the pinned fixture"
        }
    }
    foreach ($axis in @('X', 'Y')) {
        [long]$attackFrom = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $joinAttack "from$axis" 'battle-block reverse join attack') `
            "battle-block reverse join attack from$axis"
        [long]$forwardTo = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty `
                $forwardStress "final$axis" 'battle-block forward stress') `
            "battle-block forward stress final$axis"
        if ($attackFrom -ne $forwardTo) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block reverse join attack did not continue from forward to$axis"
        }
    }
    [long]$joinAttackMovement = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $joinAttack 'movementBefore' 'battle-block reverse join attack') `
        'battle-block reverse join attack movementBefore'
    [long]$lastForwardMovement = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $forwardStress 'movementAfter' 'battle-block forward stress') `
        'battle-block forward stress movementAfter'
    if ($joinAttackMovement -ne $lastForwardMovement) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block reverse join attack did not continue the forward MP ledger'
    }

    $hostStep = Assert-JsonObject `
        (Get-RequiredJsonProperty $reverse 'hostStep' 'battle-block reverse cross result') `
        'battle-block reverse host step'
    foreach ($name in @(
            'commandIssued', 'moved', 'joinBattleLiveBefore',
            'joinBattleLiveAfter', 'sameBattleAppearance',
            'joinWorldReplicationProved', 'joinUiBeforeWorldCausalityProved'
        )) {
        $value = Get-RequiredJsonProperty $hostStep $name 'battle-block reverse host step'
        if ($value -isnot [bool] -or -not $value) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block reverse host step '$name' is not true"
        }
    }
    [long]$hostFromX = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $hostStep 'fromX' 'battle-block reverse host step') `
        'battle-block reverse host step fromX'
    [long]$hostFromY = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $hostStep 'fromY' 'battle-block reverse host step') `
        'battle-block reverse host step fromY'
    [long]$hostToX = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $hostStep 'toX' 'battle-block reverse host step') `
        'battle-block reverse host step toX'
    [long]$hostToY = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $hostStep 'toY' 'battle-block reverse host step') `
        'battle-block reverse host step toY'
    [long]$hostMovementBefore = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $hostStep 'movementBefore' 'battle-block reverse host step') `
        'battle-block reverse host step movementBefore'
    [long]$hostMovementAfter = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $hostStep 'movementAfter' 'battle-block reverse host step') `
        'battle-block reverse host step movementAfter'
    [long]$joinBattleAppearance = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $hostStep 'joinBattleAppearance' 'battle-block reverse host step') `
        'battle-block reverse join battle appearance'
    [long]$joinBattleOwner = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $hostStep 'joinBattleOwner' 'battle-block reverse host step') `
        'battle-block reverse join battle owner'
    $joinWorldBefore = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $hostStep 'joinWorldBefore' 'battle-block reverse host step') `
        'battle-block reverse join world before'
    $joinWorldAfter = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $hostStep 'joinWorldAfter' 'battle-block reverse host step') `
        'battle-block reverse join world after'
    [long]$joinWorldBeforeSequence = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $joinWorldBefore 'sequence' 'battle-block reverse join world before') `
        'battle-block reverse join world before sequence'
    [long]$joinWorldAfterSequence = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $joinWorldAfter 'sequence' 'battle-block reverse join world after') `
        'battle-block reverse join world after sequence'
    foreach ($phase in @(
            [pscustomobject]@{
                name = 'before'; value = $joinWorldBefore
                expected = $script:Fixture.host.battleEnd
            },
            [pscustomobject]@{
                name = 'after'; value = $joinWorldAfter
                expected = $script:Fixture.crossBattle.hostMove
            }
        )) {
        foreach ($field in @('x', 'y', 'movement')) {
            [long]$actual = Convert-RequiredInt64 `
                (Get-RequiredJsonProperty `
                    $phase.value $field "battle-block reverse join world $($phase.name)") `
                "battle-block reverse join world $($phase.name) $field"
            if ($actual -ne [long]$phase.expected.$field) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "battle-block reverse join world $($phase.name) $field drifted from fixture"
            }
        }
    }
    [long]$joinUiEventsAudited = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $hostStep 'joinUiEventsAudited' 'battle-block reverse host step') `
        'battle-block reverse join UI events audited'
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $hostStep 'id' 'battle-block reverse host step') `
        ([string]$script:Fixture.host.heroId) 'battle-block reverse host step id')
    foreach ($phase in @('Before', 'After')) {
        $controls = Get-RequiredJsonArray `
            (Get-RequiredJsonProperty `
                $hostStep "joinLiveControls$phase" 'battle-block reverse host step') `
            "battle-block reverse join live controls $phase"
        $allowed = @(
            'BTN_DEFEND', 'BTN_RETREAT', 'BTN_WAIT', 'BTN_RESOLVE', 'TOG_AUTOBATTLE'
        )
        if ($controls.Count -lt 1 -or @($controls | Sort-Object -Unique).Count -ne
                $controls.Count -or @($controls | Where-Object {
                    $_ -isnot [string] -or $allowed -notcontains [string]$_
                }).Count -ne 0 -or $controls -contains 'BTN_CLOSE') {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block reverse join controls $phase do not prove a live battle"
        }
    }
    if ($hostFromX -ne [long]$script:Fixture.host.battleEnd.x -or
        $hostFromY -ne [long]$script:Fixture.host.battleEnd.y -or
        $hostToX -ne [long]$script:Fixture.crossBattle.hostMove.x -or
        $hostToY -ne [long]$script:Fixture.crossBattle.hostMove.y -or
        $hostMovementBefore -ne [long]$script:Fixture.host.battleEnd.movement -or
        $hostMovementAfter -ne [long]$script:Fixture.crossBattle.hostMove.movement -or
        $hostFromX -ne $hostBattleX -or $hostFromY -ne $hostBattleY -or
        $hostMovementBefore -ne $hostBattleMovement -or
        [Math]::Max([Math]::Abs($hostToX - $hostFromX),
            [Math]::Abs($hostToY - $hostFromY)) -ne 1 -or
        $joinBattleAppearance -lt 1 -or $joinBattleOwner -lt 1 -or
        $joinWorldBeforeSequence -lt 1 -or
        $joinWorldAfterSequence -le $joinWorldBeforeSequence -or
        $joinUiEventsAudited -lt 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ('battle-block reverse host step was not the exact pinned charged move ' +
             'replicated during one exact live join battle')
    }

    $hostExhaustion = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $reverse 'hostExhaustion' 'battle-block reverse cross result') `
        'battle-block reverse host exhaustion'
    foreach ($check in @(
            @{ name = 'sourceStepCount'; expected = 1 },
            @{ name = 'stressStepCount'; expected = 6 },
            @{ name = 'totalCommands'; expected = 7 },
            @{ name = 'movementBefore'; expected = [int]$script:Fixture.host.battleEnd.movement },
            @{ name = 'movementAfter'; expected = 0 },
            @{ name = 'finalX'; expected = [int]$script:Fixture.host.postBattleWalk.x },
            @{ name = 'finalY'; expected = [int]$script:Fixture.host.postBattleWalk.y }
        )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty `
                $hostExhaustion $check.name 'battle-block reverse host exhaustion') `
            $check.expected "battle-block reverse host exhaustion $($check.name)")
    }
    foreach ($name in @(
            'sourceStepIncluded', 'exhaustedToZero',
            'joinBattleStayedSameAndLive'
        )) {
        $value = Get-RequiredJsonProperty `
            $hostExhaustion $name 'battle-block reverse host exhaustion'
        if ($value -isnot [bool] -or -not $value) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block reverse host exhaustion '$name' is not true"
        }
    }
    $hostExhaustionSteps = @(Assert-BattleStressLedger `
        -Value (Get-RequiredJsonProperty `
            $hostExhaustion 'stressSteps' 'battle-block reverse host exhaustion') `
        -ExpectedSteps @($script:Fixture.crossBattle.hostExhaustionSteps) `
        -HeroId ([string]$script:Fixture.host.heroId) `
        -BattleRole join -Context 'battle-block reverse host exhaustion ledger')
    $firstHostExhaustion = Assert-JsonObject `
        $hostExhaustionSteps[0] 'battle-block first host exhaustion step'
    if ([long]$firstHostExhaustion.fromX -ne $hostToX -or
        [long]$firstHostExhaustion.fromY -ne $hostToY -or
        [long]$firstHostExhaustion.movementBefore -ne $hostMovementAfter) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block host exhaustion did not continue its source FreeAdj step'
    }
    $rounds = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $Summary 'independentRounds' 'battle-block summary') `
        'battle-block independentRounds'
    if ($rounds.Count -ne 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            'battle-block child unexpectedly executed an independent-turn round'
    }
    foreach ($name in @('gameplay', 'day2Walk', 'merge', 'postMergeStock')) {
        if ($null -ne (Get-RequiredJsonProperty $Summary $name 'battle-block summary')) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "battle-block child unexpectedly recorded '$name' evidence"
        }
    }

    return [pscustomobject]@{
        hostPid = $common.hostPid
        joinPid = $common.joinPid
        freeSteps = $freeSteps
        afterSteps = $afterSteps
        continuousFreeSteps = $continuousFreeSteps
    }
}

function Assert-OrderedMasstestSummary {
    param(
        [Parameter(Mandatory)][object]$Summary,
        [Parameter(Mandatory)][string]$ChildArtifactDir,
        [Parameter(Mandatory)][ValidateSet(
            'host-first',
            'join-first'
        )][string]$ExpectedBarrierOrder
    )
    $context = 'ordered test_R_masstest summary'
    $common = Assert-CommonChildSummary `
        $Summary 'ordered-masstest' $ChildArtifactDir $ExpectedBarrierOrder

    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $Summary 'postMergeContinuationMode' $context) `
        'automatic-masstest-phase-c-literal' `
        "$context postMergeContinuationMode")

    # test_R_masstest.ps1 starts from a fresh map and drives only its own
    # ordered day-1/day-2 End Turns. Canonical deploy/attack/walk/round/merge
    # objects must not be smuggled into this independent topology.
    foreach ($name in @(
        'reinforcement',
        'gameplay',
        'day2Walk',
        'battleBlock',
        'merge',
        'postMergeStock',
        'deferredGameplayEvidence'
    )) {
        if ($null -ne (Get-RequiredJsonProperty $Summary $name $context)) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context unexpectedly recorded canonical field '$name'"
        }
    }
    $rounds = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $Summary 'independentRounds' $context) `
        "$context independentRounds"
    if ($rounds.Count -ne 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context unexpectedly executed a canonical independent round"
    }

    $ordered = Assert-JsonObject `
        (Get-RequiredJsonProperty $Summary 'orderedMasstest' $context) `
        "$context orderedMasstest"
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $ordered 'legacyVerdict' "$context orderedMasstest") `
        'PASS' "$context legacyVerdict")
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $ordered 'order' "$context orderedMasstest") `
        $ExpectedBarrierOrder "$context order")
    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty $ordered 'mergeDay' "$context orderedMasstest") `
        3 "$context mergeDay")
    $firstRole = if ($ExpectedBarrierOrder -eq 'host-first') {
        'host'
    } else {
        'join'
    }
    $laggardRole = if ($firstRole -eq 'host') { 'join' } else { 'host' }
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $ordered 'firstRole' "$context orderedMasstest") `
        $firstRole "$context firstRole")
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $ordered 'laggardRole' "$context orderedMasstest") `
        $laggardRole "$context laggardRole")
    [string]$fixedMergeClassification = Get-RequiredJsonProperty `
        $ordered 'fixedMergeClassification' "$context orderedMasstest"
    if ($fixedMergeClassification -notin @(
            'pending-at-snapshot',
            'complete-at-snapshot'
        )) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context fixedMergeClassification is " +
             "'$fixedMergeClassification', expected pending-at-snapshot or " +
             'complete-at-snapshot')
    }
    $fixedMergeObserved = Get-RequiredJsonProperty `
        $ordered 'fixedMergeObserved' "$context orderedMasstest"
    $completionMergeObserved = Get-RequiredJsonProperty `
        $ordered 'completionMergeObserved' "$context orderedMasstest"
    if ($fixedMergeObserved -isnot [bool] -or
        $fixedMergeObserved -ne
            ($fixedMergeClassification -eq 'complete-at-snapshot') -or
        $completionMergeObserved -isnot [bool] -or
        -not $completionMergeObserved) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context fixed/completion merge observations contradict " +
             "classification '$fixedMergeClassification'")
    }
    foreach ($check in @(
        @{ name = 'stepSleepSeconds'; expected = 4 },
        @{ name = 'postMergeSettleSeconds'; expected = 4 },
        @{ name = 'mergeCompletionTimeoutSeconds'; expected = 8 },
        @{ name = 'attemptedActions'; expected = 4 },
        @{ name = 'acceptedActions'; expected = 4 },
        @{ name = 'recoveryActions'; expected = 0 }
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $ordered $check.name "$context orderedMasstest") `
            $check.expected "$context $($check.name)")
    }

    $expectedTrace = @(
        @{ role = $firstRole; day = 1; stage = 'first-day-1' },
        @{ role = $firstRole; day = 2; stage = 'first-barrier' },
        @{ role = $laggardRole; day = 1; stage = 'laggard-day-1' },
        @{ role = $laggardRole; day = 2; stage = 'laggard-merge' }
    )
    $trace = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $ordered 'actionTrace' "$context orderedMasstest") `
        "$context actionTrace"
    if ($trace.Count -ne $expectedTrace.Count) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context actionTrace has $($trace.Count) records, expected exactly " +
             "$($expectedTrace.Count)")
    }
    for ($index = 0; $index -lt $expectedTrace.Count; $index++) {
        $actual = Assert-JsonObject $trace[$index] `
            "$context actionTrace[$index]"
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $actual 'role' "$context actionTrace[$index]") `
            $expectedTrace[$index].role "$context actionTrace[$index] role")
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $actual 'completedDay' "$context actionTrace[$index]") `
            $expectedTrace[$index].day "$context actionTrace[$index] completedDay")
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty $actual 'stage' "$context actionTrace[$index]") `
            $expectedTrace[$index].stage "$context actionTrace[$index] stage")
        foreach ($check in @(
            @{ name = 'fixedWaitSeconds'; expected = 4 },
            @{ name = 'attempts'; expected = 1 },
            @{ name = 'accepted'; expected = 1 },
            @{ name = 'recoveryActions'; expected = 0 }
        )) {
            [void](Assert-ExactInteger `
                (Get-RequiredJsonProperty $actual $check.name `
                    "$context actionTrace[$index]") `
                $check.expected `
                "$context actionTrace[$index] $($check.name)")
        }
    }

    $phaseC = Assert-JsonObject `
        (Get-RequiredJsonProperty $ordered 'phaseC' "$context orderedMasstest") `
        "$context Phase C"
    [void](Assert-ExactString `
        (Get-RequiredJsonProperty $phaseC 'legacyVerdict' "$context Phase C") `
        'PASS' "$context Phase C legacyVerdict")
    [void](Assert-ExactInteger `
        (Get-RequiredJsonProperty $phaseC 'requiredNeutralRounds' "$context Phase C") `
        $script:LegacyPhaseCRequiredNeutralRounds `
        "$context Phase C requiredNeutralRounds")
    [long]$completedNeutralRounds = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'completedNeutralRounds' "$context Phase C") `
        "$context Phase C completedNeutralRounds"
    if ($completedNeutralRounds -lt $script:LegacyPhaseCRequiredNeutralRounds) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context Phase C completed only $completedNeutralRounds neutral rounds; " +
             "expected at least $($script:LegacyPhaseCRequiredNeutralRounds)")
    }
    [long]$outerTurns = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'outerTurns' "$context Phase C") `
        "$context Phase C outerTurns"
    if ($outerTurns -lt 0 -or $outerTurns -gt 24) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context Phase C outerTurns=$outerTurns, expected the literal 0..24 cap"
    }
    [long]$postDays = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'postDays' "$context Phase C") `
        "$context Phase C postDays"
    [long]$postTurns = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'postTurns' "$context Phase C") `
        "$context Phase C postTurns"
    if ($postDays -ne $completedNeutralRounds -or $postTurns -ne $outerTurns) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context legacy postDays/postTurns $postDays/$postTurns do not " +
             "match the final neutral read/outer loop $completedNeutralRounds/$outerTurns")
    }
    foreach ($name in @('hostAlive', 'mergeFired')) {
        $value = Get-RequiredJsonProperty $phaseC $name "$context Phase C"
        if ($value -isnot [bool] -or -not $value) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context legacy verdict gate '$name' is not true"
        }
    }
    foreach ($name in @('joinerAlive', 'uiMarshalled')) {
        $value = Get-RequiredJsonProperty $phaseC $name "$context Phase C"
        if ($value -isnot [bool]) {
            Throw-CampaignFailure 'malformed-summary' `
                "$context legacy observation '$name' is not boolean"
        }
    }
    [long]$vehAfterMerge = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'vehAfterMerge' "$context Phase C") `
        "$context Phase C vehAfterMerge"
    if ($vehAfterMerge -lt 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context Phase C vehAfterMerge is negative: $vehAfterMerge"
    }
    foreach ($name in @(
        'suspect',
        'uefAfterMerge',
        'uef',
        'dumps'
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $phaseC $name "$context Phase C") `
            0 "$context Phase C $name")
    }
    $dumpFiles = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $phaseC 'dumpFiles' "$context Phase C") `
        "$context Phase C dumpFiles"
    if ($dumpFiles.Count -ne 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context legacy outcome retained $($dumpFiles.Count) new or changed dump(s)"
    }
    foreach ($check in @(
        @{ name = 'maxOuterTurns'; expected = 24 },
        @{ name = 'pollMilliseconds'; expected = 800 },
        @{ name = 'advanceTimeoutSeconds'; expected = 26 },
        @{ name = 'initialSettleSeconds'; expected = 4 },
        @{ name = 'noSlotSleepSeconds'; expected = 2 }
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $phaseC $check.name "$context Phase C") `
            $check.expected "$context Phase C $($check.name)")
    }
    [long]$slotWatermark = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'slotWatermark' "$context Phase C") `
        "$context Phase C slotWatermark"
    if ($slotWatermark -lt 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context Phase C slotWatermark is negative: $slotWatermark"
    }
    [long]$postMergeSlotBoundary = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'postMergeSlotBoundary' "$context Phase C") `
        "$context Phase C postMergeSlotBoundary"
    if ($postMergeSlotBoundary -le 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context Phase C postMergeSlotBoundary=$postMergeSlotBoundary; " +
             'the literal post-merge scan must resolve a positive boundary')
    }
    [long]$attemptedHumanActions = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'attemptedHumanActions' "$context Phase C") `
        "$context Phase C attemptedHumanActions"
    [long]$acceptedHumanActions = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'acceptedHumanActions' "$context Phase C") `
        "$context Phase C acceptedHumanActions"
    [long]$phaseCRecoveryActions = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'recoveryActions' "$context Phase C") `
        "$context Phase C recoveryActions"
    [long]$transportAdapterReads = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $phaseC 'transportAdapterReads' "$context Phase C") `
        "$context Phase C transportAdapterReads"
    if ($attemptedHumanActions -lt 0 -or
        $acceptedHumanActions -ne $attemptedHumanActions -or
        $phaseCRecoveryActions -ne 0 -or
        $transportAdapterReads -ne $attemptedHumanActions) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context Phase C action accounting is not exact-once: " +
             "attempted=$attemptedHumanActions accepted=$acceptedHumanActions " +
             "recovery=$phaseCRecoveryActions adapterReads=$transportAdapterReads")
    }

    # The retained explicit CLI mode also publishes the Phase-C projection at
    # the long-standing top-level field. It must describe the same literal
    # verdict, not a second continuation.
    $phaseCProjection = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $Summary 'postMergeAutomaticMasstestPhaseC' $context) `
        "$context Phase-C projection"
    foreach ($name in @(
        'legacyVerdict',
        'requiredNeutralRounds',
        'completedNeutralRounds',
        'outerTurns',
        'hostAlive',
        'joinerAlive',
        'mergeFired',
        'uiMarshalled',
        'postDays',
        'postTurns',
        'vehAfterMerge',
        'suspect',
        'uefAfterMerge',
        'uef',
        'dumps',
        'maxOuterTurns',
        'pollMilliseconds',
        'advanceTimeoutSeconds',
        'initialSettleSeconds',
        'noSlotSleepSeconds',
        'slotWatermark',
        'postMergeSlotBoundary',
        'attemptedHumanActions',
        'acceptedHumanActions',
        'recoveryActions',
        'transportAdapterReads'
    )) {
        $nestedValue = Get-RequiredJsonProperty $phaseC $name "$context Phase C"
        $projectionValue = Get-RequiredJsonProperty `
            $phaseCProjection $name "$context Phase-C projection"
        if ([string]$nestedValue -cne [string]$projectionValue) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("$context Phase-C projection field '$name' does not match " +
                 'orderedMasstest.phaseC')
        }
    }

    $humanActions = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty $phaseC 'humanActions' "$context Phase C") `
        "$context Phase C humanActions"
    $projectedHumanActions = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty `
            $phaseCProjection 'humanActions' "$context Phase-C projection") `
        "$context Phase-C projection humanActions"
    if ($humanActions.Count -ne $attemptedHumanActions -or
        $projectedHumanActions.Count -ne $humanActions.Count) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context Phase C humanActions count does not match its exact-once " +
             "accounting/projection: nested=$($humanActions.Count) " +
             "projected=$($projectedHumanActions.Count) attempted=$attemptedHumanActions")
    }
    for ($index = 0; $index -lt $humanActions.Count; $index++) {
        $action = Assert-JsonObject `
            $humanActions[$index] "$context Phase C humanActions[$index]"
        $projectedAction = Assert-JsonObject `
            $projectedHumanActions[$index] `
            "$context Phase-C projection humanActions[$index]"
        foreach ($name in @(
            'role',
            'appearance',
            'instance',
            'attempts',
            'accepted',
            'recoveryActions',
            'transportAdapterReads'
        )) {
            $nestedValue = Get-RequiredJsonProperty `
                $action $name "$context Phase C humanActions[$index]"
            $projectionValue = Get-RequiredJsonProperty `
                $projectedAction $name `
                "$context Phase-C projection humanActions[$index]"
            if ([string]$nestedValue -cne [string]$projectionValue) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    ("$context Phase-C projected humanActions[$index].$name " +
                     'does not match orderedMasstest.phaseC')
            }
        }
        $actionRole = Get-RequiredJsonProperty `
            $action 'role' "$context Phase C humanActions[$index]"
        if ($actionRole -isnot [string] -or $actionRole -notin @('host', 'join')) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$context Phase C humanActions[$index] has invalid role '$actionRole'"
        }
        foreach ($check in @(
            @{ name = 'attempts'; expected = 1 },
            @{ name = 'accepted'; expected = 1 },
            @{ name = 'recoveryActions'; expected = 0 },
            @{ name = 'transportAdapterReads'; expected = 1 }
        )) {
            [void](Assert-ExactInteger `
                (Get-RequiredJsonProperty $action $check.name `
                    "$context Phase C humanActions[$index]") `
                $check.expected `
                "$context Phase C humanActions[$index] $($check.name)")
        }
    }

    $projectedDumpFiles = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty `
            $phaseCProjection 'dumpFiles' "$context Phase-C projection") `
        "$context Phase-C projection dumpFiles"
    if ($projectedDumpFiles.Count -ne $dumpFiles.Count) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context projected dumpFiles count $($projectedDumpFiles.Count) " +
             "does not match orderedMasstest.phaseC count $($dumpFiles.Count)")
    }

    # Native/MSS strengthening is read only after the literal Phase-C PASS.
    # It may prove causality, but it cannot replace any legacy gate above.
    $deferred = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $ordered 'deferredMssEvidence' "$context orderedMasstest") `
        "$context deferredMssEvidence"
    $proved = Get-RequiredJsonProperty $deferred 'proved' `
        "$context deferredMssEvidence"
    if ($proved -isnot [bool] -or -not $proved) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context deferred MSS evidence did not prove its post-PASS observation"
    }

    foreach ($check in @(
        @{ name = 'endTurnsPerRole'; expected = 2 },
        @{ name = 'cascadePerRole'; expected = 1 },
        @{ name = 'barrierCount'; expected = 2 },
        @{ name = 'mergeCount'; expected = 1 }
    )) {
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty $deferred $check.name `
                "$context deferredMssEvidence") `
            $check.expected "$context deferredMssEvidence $($check.name)")
    }
    $barrierHeldPeers = Get-RequiredJsonArray `
        (Get-RequiredJsonProperty `
            $deferred 'barrierHeldPeers' "$context deferredMssEvidence") `
        "$context deferredMssEvidence barrierHeldPeers"
    if ($barrierHeldPeers.Count -ne 2) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("$context deferred MSS evidence has $($barrierHeldPeers.Count) " +
             'held peers, expected exactly host and join')
    }
    [void](Assert-ExactString `
        $barrierHeldPeers[0] 'host' `
        "$context deferredMssEvidence barrierHeldPeers[0]")
    [void](Assert-ExactString `
        $barrierHeldPeers[1] 'join' `
        "$context deferredMssEvidence barrierHeldPeers[1]")
    [long]$mergeActionId = Convert-RequiredInt64 `
        (Get-RequiredJsonProperty `
            $deferred 'mergeActionId' "$context deferredMssEvidence") `
        "$context deferredMssEvidence mergeActionId"
    if ($mergeActionId -le 0) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            "$context deferred MSS evidence has invalid mergeActionId $mergeActionId"
    }
    $phaseCDeferred = Assert-JsonObject `
        (Get-RequiredJsonProperty $phaseC 'deferredMssEvidence' "$context Phase C") `
        "$context Phase C deferredMssEvidence"
    $projectedDeferred = Assert-JsonObject `
        (Get-RequiredJsonProperty `
            $phaseCProjection 'deferredMssEvidence' "$context Phase-C projection") `
        "$context Phase-C projection deferredMssEvidence"
    foreach ($fixedProjection in @(
        @{ value = $deferred; context = "$context deferredMssEvidence" },
        @{ value = $phaseCDeferred; context = "$context Phase C deferredMssEvidence" },
        @{ value = $projectedDeferred; context = "$context Phase-C projection deferredMssEvidence" }
    )) {
        [void](Assert-ExactString `
            (Get-RequiredJsonProperty `
                $fixedProjection.value 'fixedMergeClassification' `
                $fixedProjection.context) `
            $fixedMergeClassification `
            "$($fixedProjection.context) fixedMergeClassification")
        foreach ($name in @('fixedMergeObserved', 'completionMergeObserved')) {
            $value = Get-RequiredJsonProperty `
                $fixedProjection.value $name $fixedProjection.context
            $expected = Get-RequiredJsonProperty `
                $ordered $name "$context orderedMasstest"
            if ($value -isnot [bool] -or $value -ne $expected) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    "$($fixedProjection.context) $name differs from orderedMasstest"
            }
        }
        [void](Assert-ExactInteger `
            (Get-RequiredJsonProperty `
                $fixedProjection.value 'mergeCompletionTimeoutSeconds' `
                $fixedProjection.context) `
            8 "$($fixedProjection.context) mergeCompletionTimeoutSeconds")
    }
    foreach ($name in @(
        'proved',
        'endTurnsPerRole',
        'cascadePerRole',
        'barrierCount',
        'mergeCount',
        'mergeActionId',
        'fixedMergeClassification',
        'fixedMergeObserved',
        'completionMergeObserved',
        'mergeCompletionTimeoutSeconds'
    )) {
        $orderedValue = Get-RequiredJsonProperty `
            $deferred $name "$context deferredMssEvidence"
        $nestedValue = Get-RequiredJsonProperty `
            $phaseCDeferred $name "$context Phase C deferredMssEvidence"
        $projectionValue = Get-RequiredJsonProperty `
            $projectedDeferred $name `
            "$context Phase-C projection deferredMssEvidence"
        if ([string]$orderedValue -cne [string]$nestedValue -or
            [string]$orderedValue -cne [string]$projectionValue) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                ("$context deferred MSS field '$name' differs between the " +
                 'ordered, nested Phase-C, and top-level projections')
        }
    }
    foreach ($projection in @(
        @{ value = $phaseCDeferred; context = "$context Phase C deferredMssEvidence" },
        @{ value = $projectedDeferred; context = "$context Phase-C projection deferredMssEvidence" }
    )) {
        $projectedHeldPeers = Get-RequiredJsonArray `
            (Get-RequiredJsonProperty `
                $projection.value 'barrierHeldPeers' $projection.context) `
            "$($projection.context) barrierHeldPeers"
        if ($projectedHeldPeers.Count -ne $barrierHeldPeers.Count) {
            Throw-CampaignFailure 'acceptance-mismatch' `
                "$($projection.context) changed the held-peer topology"
        }
        for ($index = 0; $index -lt $barrierHeldPeers.Count; $index++) {
            [void](Assert-ExactString `
                $projectedHeldPeers[$index] ([string]$barrierHeldPeers[$index]) `
                "$($projection.context) barrierHeldPeers[$index]")
        }
    }
    foreach ($role in @('host', 'join')) {
        $orderedMarkers = Assert-JsonObject `
            (Get-RequiredJsonProperty `
                (Assert-JsonObject `
                    (Get-RequiredJsonProperty $deferred 'nativeMarkers' `
                        "$context deferredMssEvidence") `
                    "$context deferredMssEvidence nativeMarkers") `
                $role "$context deferredMssEvidence nativeMarkers") `
            "$context deferredMssEvidence nativeMarkers $role"
        $nestedMarkers = Assert-JsonObject `
            (Get-RequiredJsonProperty `
                (Assert-JsonObject `
                    (Get-RequiredJsonProperty $phaseCDeferred 'nativeMarkers' `
                        "$context Phase C deferredMssEvidence") `
                    "$context Phase C deferredMssEvidence nativeMarkers") `
                $role "$context Phase C deferredMssEvidence nativeMarkers") `
            "$context Phase C deferredMssEvidence nativeMarkers $role"
        $projectedMarkers = Assert-JsonObject `
            (Get-RequiredJsonProperty `
                (Assert-JsonObject `
                    (Get-RequiredJsonProperty $projectedDeferred 'nativeMarkers' `
                        "$context Phase-C projection deferredMssEvidence") `
                    "$context Phase-C projection deferredMssEvidence nativeMarkers") `
                $role "$context Phase-C projection deferredMssEvidence nativeMarkers") `
            "$context Phase-C projection deferredMssEvidence nativeMarkers $role"
        foreach ($name in @(
            'naturalMergeBeginTurnAppliedAndDrained',
            'relayReleasedStockTurns'
        )) {
            $orderedValue = Get-RequiredJsonProperty `
                $orderedMarkers $name "$context nativeMarkers $role"
            $nestedValue = Get-RequiredJsonProperty `
                $nestedMarkers $name "$context nested nativeMarkers $role"
            $projectionValue = Get-RequiredJsonProperty `
                $projectedMarkers $name "$context projected nativeMarkers $role"
            [void](Assert-ExactInteger `
                $orderedValue 1 "$context nativeMarkers $role $name")
            if ([string]$orderedValue -cne [string]$nestedValue -or
                [string]$orderedValue -cne [string]$projectionValue) {
                Throw-CampaignFailure 'acceptance-mismatch' `
                    ("$context native marker '$role.$name' differs between the " +
                     'ordered, nested Phase-C, and top-level projections')
            }
        }
    }

    return [pscustomobject]@{
        hostPid = $common.hostPid
        joinPid = $common.joinPid
        order = $ExpectedBarrierOrder
        actions = $trace.Count
        attemptedActions = 4
        acceptedActions = 4
        recoveryActions = 0
        attemptedHumanActions = $attemptedHumanActions
        transportAdapterReads = $transportAdapterReads
        completedNeutralRounds = $completedNeutralRounds
    }
}

function New-CampaignSpecPlan {
    $plan = [System.Collections.Generic.List[object]]::new()

    # Old test/mass_test.ps1 executes Count independent cold run_test.ps1
    # children. run_test.ps1 fires the merge barrier in parallel. Count is the
    # number of those complete runs; it is not a pool into which other barrier
    # orders may be substituted.
    for ($iteration = 1; $iteration -le $Count; $iteration++) {
        $plan.Add([pscustomobject]@{
            phase = ('mass-{0:D3}-parallel' -f $iteration)
            category = 'mass-cold'
            gameplayMode = 'canonical'
            barrierOrder = 'parallel'
            oldMassTestEquivalent = $true
            oldOrderedMasstestEquivalent = $false
            postMergeContinuationMode = 'none'
        })
    }

    $plan.Add([pscustomobject]@{
        phase = 'battle-block'
        category = 'battle-block'
        gameplayMode = 'battle-block'
        barrierOrder = 'parallel'
        oldMassTestEquivalent = $false
        oldOrderedMasstestEquivalent = $false
        postMergeContinuationMode = 'none'
    })

    # Old test_R_masstest.ps1 is a separate twelve-iteration cold sweep. Its
    # odd iterations are join-first; its even iterations are host-first.
    for ($iteration = 1;
         $iteration -le $script:OrderedMasstestIterations;
         $iteration++) {
        $order = if (($iteration % 2) -eq 1) {
            'join-first'
        } else {
            'host-first'
        }
        $plan.Add([pscustomobject]@{
            phase = ('ordered-masstest-{0:D3}-{1}' -f $iteration, $order)
            category = 'ordered-masstest-cold'
            gameplayMode = 'ordered-masstest'
            barrierOrder = $order
            oldMassTestEquivalent = $false
            oldOrderedMasstestEquivalent = $true
            postMergeContinuationMode = 'automatic-masstest-phase-c-literal'
        })
    }

    return @($plan)
}

function New-ColdChildArguments {
    param(
        [Parameter(Mandatory)][object]$Spec,
        [string]$ChildArtifactDir = '<offline-child-artifact>'
    )

    $arguments = @(
        '-NoLogo',
        '-NoProfile',
        '-NonInteractive',
        '-File', $pocPath,
        '-GameDir', $GameDir,
        '-FixtureManifest', $FixtureManifest,
        '-ArtifactDir', $ChildArtifactDir,
        '-GameplayMode', [string]$Spec.gameplayMode,
        '-BarrierOrder', [string]$Spec.barrierOrder,
        '-MergeDay', [string]$script:FixtureMergeDay
    )
    if ([string]$Spec.postMergeContinuationMode -eq
        'automatic-masstest-phase-c-literal') {
        # One immutable mode pair, one child start. Mode=none emits no pair;
        # there is no substitute, fallback invocation, retry, or re-fire.
        $arguments += @(
            '-PostMergeContinuationMode',
            'automatic-masstest-phase-c-literal'
        )
    }
    return @($arguments)
}

function Assert-CampaignSpecPlan {
    param([Parameter(Mandatory)][object[]]$Specs)

    if ($Specs.Count -ne $script:ExpectedRunCount) {
        Throw-CampaignFailure 'self-check' `
            ("campaign plan has $($Specs.Count) cases, expected exactly " +
             ("$($script:ExpectedRunCount) (= Count parallel + battle-block + " +
              "$($script:OrderedMasstestIterations) ordered masstest cold runs)"))
    }
    foreach ($spec in $Specs) {
        if ($null -eq $spec -or $spec -isnot [pscustomobject]) {
            Throw-CampaignFailure 'self-check' 'campaign plan contains a non-object case'
        }
        foreach ($name in @(
            'phase',
            'category',
            'gameplayMode',
            'barrierOrder',
            'oldMassTestEquivalent',
            'oldOrderedMasstestEquivalent',
            'postMergeContinuationMode'
        )) {
            if ($null -eq $spec.PSObject.Properties[$name]) {
                Throw-CampaignFailure 'self-check' `
                    "campaign case omitted required '$name' contract"
            }
        }
        if ($spec.oldMassTestEquivalent -isnot [bool]) {
            Throw-CampaignFailure 'self-check' `
                "campaign case '$($spec.phase)' oldMassTestEquivalent is not boolean"
        }
        if ($spec.oldOrderedMasstestEquivalent -isnot [bool]) {
            Throw-CampaignFailure 'self-check' `
                ("campaign case '$($spec.phase)' " +
                 'oldOrderedMasstestEquivalent is not boolean')
        }
        if ($spec.postMergeContinuationMode -isnot [string] -or
            $spec.postMergeContinuationMode -notin @(
                'none', 'automatic-masstest-phase-c-literal')) {
            Throw-CampaignFailure 'self-check' `
                "campaign case '$($spec.phase)' has an invalid continuation mode"
        }
    }

    for ($iteration = 1; $iteration -le $Count; $iteration++) {
        $spec = $Specs[$iteration - 1]
        $expectedPhase = 'mass-{0:D3}-parallel' -f $iteration
        if ($spec.phase -ne $expectedPhase -or
            $spec.category -ne 'mass-cold' -or
            $spec.gameplayMode -ne 'canonical' -or
            $spec.barrierOrder -ne 'parallel' -or
            -not $spec.oldMassTestEquivalent -or
            $spec.oldOrderedMasstestEquivalent -or
            $spec.postMergeContinuationMode -ne 'none') {
            Throw-CampaignFailure 'self-check' `
                ("old mass-test-equivalent case $iteration must remain an independent " +
                 'canonical/parallel cold run ending at the BARRIER/dead-click PASS boundary')
        }
    }

    $oldEquivalent = @($Specs | Where-Object oldMassTestEquivalent)
    if ($oldEquivalent.Count -ne $Count -or
        @($oldEquivalent | Where-Object barrierOrder -ne 'parallel').Count -ne 0 -or
        @($oldEquivalent | Where-Object postMergeContinuationMode -ne 'none').Count -ne 0) {
        Throw-CampaignFailure 'self-check' `
            'Count must map one-for-one to parallel old-mass-test cases with no Phase-C replacement'
    }

    $battle = $Specs[$Count]
    if ($battle.phase -ne 'battle-block' -or
        $battle.category -ne 'battle-block' -or
        $battle.gameplayMode -ne 'battle-block' -or
        $battle.barrierOrder -ne 'parallel' -or
        $battle.oldMassTestEquivalent -or
        $battle.oldOrderedMasstestEquivalent -or
        $battle.postMergeContinuationMode -ne 'none') {
        Throw-CampaignFailure 'self-check' `
            'battle-block must remain one explicit non-merge case after all Count parallel runs'
    }

    $ordered = @($Specs | Where-Object oldOrderedMasstestEquivalent)
    if ($ordered.Count -ne $script:OrderedMasstestIterations) {
        Throw-CampaignFailure 'self-check' `
            ("campaign must contain exactly $($script:OrderedMasstestIterations) " +
             'old test_R_masstest-equivalent cold children')
    }
    for ($index = 0; $index -lt $script:OrderedMasstestIterations; $index++) {
        $iteration = $index + 1
        $expectedOrder = if (($iteration % 2) -eq 1) {
            'join-first'
        } else {
            'host-first'
        }
        $spec = $Specs[$Count + $iteration]
        $expectedPhase = 'ordered-masstest-{0:D3}-{1}' -f `
            $iteration, $expectedOrder
        if ($spec.phase -ne $expectedPhase -or
            $spec.category -ne 'ordered-masstest-cold' -or
            $spec.gameplayMode -ne 'ordered-masstest' -or
            $spec.barrierOrder -ne $expectedOrder -or
            $spec.oldMassTestEquivalent -or
            -not $spec.oldOrderedMasstestEquivalent -or
            $spec.postMergeContinuationMode -ne
                'automatic-masstest-phase-c-literal') {
            Throw-CampaignFailure 'self-check' `
                ("ordered masstest iteration $iteration must remain an independent " +
                 "$expectedOrder cold child with its exact automatic Phase-C mode")
        }
    }

    $automatic = @($Specs | Where-Object {
        $_.postMergeContinuationMode -eq 'automatic-masstest-phase-c-literal'
    })
    if ($automatic.Count -ne $script:OrderedMasstestIterations -or
        @($automatic | Where-Object gameplayMode -ne 'ordered-masstest').Count -ne 0 -or
        @($automatic | Where-Object oldOrderedMasstestEquivalent -ne $true).Count -ne 0) {
        Throw-CampaignFailure 'self-check' `
            ('only the twelve independent ordered-masstest cases may request ' +
             'automatic Phase C')
    }
    if (@($Specs | Where-Object {
            $_.gameplayMode -eq 'ordered-masstest' -and
            $_.postMergeContinuationMode -ne
                'automatic-masstest-phase-c-literal'
        }).Count -ne 0) {
        Throw-CampaignFailure 'self-check' `
            'every ordered-masstest case must request its exact Phase-C mode'
    }
    if (@($Specs | Where-Object {
            $_.postMergeContinuationMode -eq 'mss-stock-telemetry'
        }).Count -ne 0) {
        Throw-CampaignFailure 'self-check' `
            'MSS stock telemetry is not a mode in this legacy acceptance campaign'
    }
}

function Assert-CampaignArgumentPlan {
    param([Parameter(Mandatory)][object[]]$Specs)

    foreach ($spec in $Specs) {
        $arguments = @(New-ColdChildArguments $spec)
        foreach ($contract in @(
            @{ name = '-GameplayMode'; expected = [string]$spec.gameplayMode },
            @{ name = '-BarrierOrder'; expected = [string]$spec.barrierOrder },
            @{ name = '-MergeDay'; expected = [string]$script:FixtureMergeDay }
        )) {
            $indices = @(for ($index = 0; $index -lt $arguments.Count; $index++) {
                if ([string]::Equals(
                        [string]$arguments[$index], $contract.name,
                        [StringComparison]::Ordinal)) {
                    $index
                }
            })
            if ($indices.Count -ne 1 -or
                $indices[0] -ge ($arguments.Count - 1) -or
                -not [string]::Equals(
                    [string]$arguments[$indices[0] + 1], $contract.expected,
                    [StringComparison]::Ordinal)) {
                Throw-CampaignFailure 'self-check' `
                    ("child argv for '$($spec.phase)' must contain exactly one " +
                     "$($contract.name) '$($contract.expected)' pair")
            }
        }

        $modeArgumentIndices = @(for ($index = 0; $index -lt $arguments.Count; $index++) {
            if ([string]::Equals(
                    [string]$arguments[$index], '-PostMergeContinuationMode',
                    [StringComparison]::Ordinal)) {
                $index
            }
        })
        $expectedModeArgumentCount = if ($spec.postMergeContinuationMode -eq
            'automatic-masstest-phase-c-literal') { 1 } else { 0 }
        if ($modeArgumentIndices.Count -ne $expectedModeArgumentCount) {
            Throw-CampaignFailure 'self-check' `
                ("child argv for '$($spec.phase)' contains $($modeArgumentIndices.Count) " +
                 "continuation-mode arguments, expected exactly $expectedModeArgumentCount")
        }
        if ($modeArgumentIndices.Count -eq 1) {
            $modeIndex = $modeArgumentIndices[0]
            if ($modeIndex -ge ($arguments.Count - 1) -or
                -not [string]::Equals(
                    [string]$arguments[$modeIndex + 1],
                    'automatic-masstest-phase-c-literal',
                    [StringComparison]::Ordinal)) {
                Throw-CampaignFailure 'self-check' `
                    ("child argv for '$($spec.phase)' lost the exact automatic " +
                     'test_R_masstest.ps1 Phase-C mode value')
            }
        }
        $automaticModeValueCount = @($arguments | Where-Object {
            [string]::Equals(
                [string]$_, 'automatic-masstest-phase-c-literal',
                [StringComparison]::Ordinal)
        }).Count
        if ($automaticModeValueCount -ne $expectedModeArgumentCount) {
            Throw-CampaignFailure 'self-check' `
                ("child argv for '$($spec.phase)' contains a detached, duplicate, " +
                 'or missing automatic continuation-mode value')
        }
    }
}

function Assert-CampaignSourceContract {
    [void](Assert-ExactString '' '' 'campaign empty-string comparison probe')

    $arrayProbe = [pscustomobject]@{ phases = @('boot', 'deploy') }
    $unwrappedProbe = @(Get-OptionalJsonArrayProperty $arrayProbe 'phases')
    if ($unwrappedProbe.Count -ne 2 -or
        [string]$unwrappedProbe[0] -cne 'boot' -or
        [string]$unwrappedProbe[1] -cne 'deploy') {
        Throw-CampaignFailure 'self-check' `
            'campaign JSON-array projection nested or stringified its elements'
    }
    $requiredArrayProbe = Get-RequiredJsonArray `
        @('unit-a', 'unit-b') 'campaign required-array projection probe'
    $combinedArrayProbe = @('leader') + @(
        $requiredArrayProbe | ForEach-Object { [string]$_ }
    )
    if ($combinedArrayProbe.Count -ne 3 -or
        [string]$combinedArrayProbe[0] -cne 'leader' -or
        [string]$combinedArrayProbe[1] -cne 'unit-a' -or
        [string]$combinedArrayProbe[2] -cne 'unit-b') {
        Throw-CampaignFailure 'self-check' `
            'campaign required JSON-array projection nested its unit IDs'
    }

    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $PSCommandPath, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0) {
        $details = ($parseErrors | ForEach-Object Message) -join '; '
        Throw-CampaignFailure 'self-check' "campaign parser errors: $details"
    }

    $commands = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst]
    }, $true))

    $campaignSource = Get-Content -LiteralPath $PSCommandPath -Raw
    $orderedSettleOrder = @(
        ('if ([bool]$spec.' +
            'oldOrderedMasstestEquivalent -and $null -ne $record) {'),
        ('Start-' + 'Sleep -Seconds 3'),
        ('$record.' + 'orderedInterIterationSettleSeconds = 3'),
        ('if ($index -lt (' + '$specs.Count - 1)) {')
    )
    $previousOrderedSettleOffset = -1
    foreach ($literal in $orderedSettleOrder) {
        $matches = [regex]::Matches(
            $campaignSource, [regex]::Escape($literal))
        if ($matches.Count -ne 1 -or $matches[0].Index -le $previousOrderedSettleOffset) {
            Throw-CampaignFailure 'self-check' `
                "ordered masstest fixed post-iteration clock moved or duplicated at '$literal'"
        }
        $previousOrderedSettleOffset = $matches[0].Index
    }

    $countParameters = @($ast.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -eq 'Count'
    })
    if ($countParameters.Count -ne 1 -or
        $null -eq $countParameters[0].DefaultValue -or
        $countParameters[0].DefaultValue.Extent.Text -ne '5') {
        Throw-CampaignFailure 'self-check' `
            'campaign -Count must default exactly to the old mass_test.ps1 value 5'
    }
    $orderedIterationAssignments = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -eq '$script:OrderedMasstestIterations'
    }, $true))
    if ($orderedIterationAssignments.Count -ne 1 -or
        $orderedIterationAssignments[0].Right.Extent.Text -ne '12') {
        Throw-CampaignFailure 'self-check' `
            'campaign must retain exactly twelve old test_R_masstest cold iterations'
    }

    foreach ($requiredFunction in @(
        'New-CampaignSpecPlan',
        'New-ColdChildArguments',
        'Assert-CampaignSpecPlan',
        'Assert-CampaignArgumentPlan',
        'Assert-ChildClientLogCompletion',
        'Assert-OrderedMasstestSummary',
        'Wait-PinnedFixtureReady',
        'Invoke-ColdChild'
    )) {
        $definitions = @($ast.FindAll({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq $requiredFunction
        }, $true))
        if ($definitions.Count -ne 1) {
            Throw-CampaignFailure 'self-check' `
                "campaign source must contain exactly one $requiredFunction implementation"
        }
    }

    $commonChildSummary = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Assert-CommonChildSummary'
    }, $true))[0]
    $commonLogCompletionCalls = @($commonChildSummary.Body.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq 'Assert-ChildClientLogCompletion'
    }, $true))
    if ($commonLogCompletionCalls.Count -ne 1 -or
        $commonLogCompletionCalls[0].Extent.Text -notmatch
            '(?s)-Summary\s+\$Summary.*-HostPid\s+\$hostPid.*-JoinPid\s+\$joinPid.*-ChildArtifactDir\s+\$ChildArtifactDir') {
        Throw-CampaignFailure 'self-check' `
            ('every canonical, battle-block, and ordered child must pass through ' +
             'one exact common client-log completion gate')
    }

    $invokeColdChild = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Invoke-ColdChild'
    }, $true))[0]
    $checkedArgumentBuilderCalls = @($invokeColdChild.Body.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -eq 'New-ColdChildArguments'
    }, $true))
    if ($checkedArgumentBuilderCalls.Count -ne 1) {
        Throw-CampaignFailure 'self-check' `
            'Invoke-ColdChild must build its immutable argv through exactly one checked builder call'
    }
    $coldChildCalls = @($commands | Where-Object {
        $_.GetCommandName() -eq 'Invoke-ColdChild'
    })
    if ($coldChildCalls.Count -ne 1) {
        Throw-CampaignFailure 'self-check' `
            ('campaign execution loop must contain exactly one Invoke-ColdChild ' +
             'call site; retries and fallback starts are forbidden')
    }
    $fixtureReadyCalls = @($commands | Where-Object {
        $_.GetCommandName() -eq 'Wait-PinnedFixtureReady'
    })
    if ($fixtureReadyCalls.Count -ne 1 -or
        $fixtureReadyCalls[0].Extent.StartOffset -ge
            $coldChildCalls[0].Extent.StartOffset) {
        Throw-CampaignFailure 'self-check' `
            ('campaign execution loop must observe the exact pinned fixture once ' +
             'immediately before its sole child start path')
    }

    foreach ($command in $commands) {
        $name = $command.GetCommandName()
        if ($name -in @('Stop-Process', 'Remove-Item', 'Copy-Item', 'Move-Item')) {
            Throw-CampaignFailure 'self-check' `
                "campaign contains forbidden mutation command '$name'"
        }
        $gameProcessName = 'Disc' + 'ipl2'
        if ($name -eq 'Get-Process' -and
            $command.Extent.Text -match "(?i)$gameProcessName") {
            Throw-CampaignFailure 'self-check' `
                'campaign must not discover game clients by process name'
        }
    }
    $killCalls = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
            [string]$node.Member.Value -eq 'Kill'
    }, $true))
    if ($killCalls.Count -ne 0) {
        Throw-CampaignFailure 'self-check' 'campaign must never kill a process'
    }

    if (-not (Test-Path -LiteralPath $pocPath -PathType Leaf)) {
        Throw-CampaignFailure 'preflight' "POC runner is missing: $pocPath"
    }
    $pocTokens = $null
    $pocErrors = $null
    $pocAst = [Management.Automation.Language.Parser]::ParseFile(
        $pocPath, [ref]$pocTokens, [ref]$pocErrors)
    if ($pocErrors.Count -ne 0) {
        $details = ($pocErrors | ForEach-Object Message) -join '; '
        Throw-CampaignFailure 'preflight' "POC runner parser errors: $details"
    }
    $pocParameters = @($pocAst.ParamBlock.Parameters | ForEach-Object {
        $_.Name.VariablePath.UserPath
    })
    foreach ($required in @(
        'GameDir',
        'FixtureManifest',
        'ArtifactDir',
        'GameplayMode',
        'BarrierOrder',
        'MergeDay',
        'PostMergeContinuationMode'
    )) {
        if ($required -notin $pocParameters) {
            Throw-CampaignFailure 'preflight' `
                "POC runner does not expose required -$required contract"
        }
    }
    $gameplayParameters = @($pocAst.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -eq 'GameplayMode'
    })
    if ($gameplayParameters.Count -ne 1) {
        Throw-CampaignFailure 'preflight' `
            'POC runner must expose exactly one -GameplayMode parameter'
    }
    $gameplayValidateSet = @($gameplayParameters[0].Attributes | Where-Object {
        $_.TypeName.Name -eq 'ValidateSet'
    })
    if ($gameplayValidateSet.Count -ne 1) {
        Throw-CampaignFailure 'preflight' `
            'POC -GameplayMode must expose exactly one ValidateSet contract'
    }
    $gameplayModes = @($gameplayValidateSet[0].PositionalArguments | ForEach-Object {
        [string]$_.SafeGetValue()
    })
    if ($gameplayModes.Count -ne 6 -or
        @(
            'protocol',
            'canonical',
            'battle-block',
            'long-move',
            'long-attack',
            'ordered-masstest'
        | Where-Object { $_ -notin $gameplayModes }).Count -ne 0) {
        Throw-CampaignFailure 'preflight' `
            ('POC -GameplayMode must expose exactly protocol, canonical, ' +
             'battle-block, long-move, long-attack, and ordered-masstest')
    }
    $continuationParameters = @($pocAst.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -eq 'PostMergeContinuationMode'
    })
    if ($continuationParameters.Count -ne 1 -or
        $null -eq $continuationParameters[0].DefaultValue -or
        $continuationParameters[0].DefaultValue.Extent.Text -ne "'none'") {
        Throw-CampaignFailure 'preflight' `
            ('POC -PostMergeContinuationMode must default exactly to none so ' +
             'legacy Count and battle-block argv can omit the mode pair')
    }

    $gameplayHelperPath = Join-Path $PSScriptRoot '_simturns_gameplay.ps1'
    if (-not (Test-Path -LiteralPath $gameplayHelperPath -PathType Leaf)) {
        Throw-CampaignFailure 'preflight' `
            "gameplay helper is missing: $gameplayHelperPath"
    }
    $gameplayTokens = $null
    $gameplayErrors = $null
    $gameplayAst = [Management.Automation.Language.Parser]::ParseFile(
        $gameplayHelperPath, [ref]$gameplayTokens, [ref]$gameplayErrors)
    if ($gameplayErrors.Count -ne 0) {
        $details = ($gameplayErrors | ForEach-Object Message) -join '; '
        Throw-CampaignFailure 'preflight' "gameplay helper parser errors: $details"
    }
    $battleFunctions = @($gameplayAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Invoke-BattleBlockProof'
    }, $true))
    if ($battleFunctions.Count -ne 1) {
        Throw-CampaignFailure 'preflight' `
            'gameplay helper must expose exactly one Invoke-BattleBlockProof implementation'
    }
    $sourceStepAssignments = @($battleFunctions[0].Body.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -eq '[int]$sourceStepCount'
    }, $true))
    if ($sourceStepAssignments.Count -ne 1 -or
        $sourceStepAssignments[0].Right.Extent.Text -ne
            [string]$script:BattleBlockStepCount) {
        Throw-CampaignFailure 'preflight' `
            ("battle-block implementation must retain exactly " +
             "$($script:BattleBlockStepCount) source steps")
    }

    $pocSource = Get-Content -LiteralPath $pocPath -Raw
    $canonicalCascadeSource = [regex]::Match(
        $pocSource,
        '(?ms)^function Assert-CanonicalRoundCascadeProof\(.*?(?=^function Invoke-EndTurnsAndWaitAccepted)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($canonicalCascadeSource)) {
        Throw-CampaignFailure 'preflight' `
            'POC runner lost its canonical v8 turn-cascade summary producer'
    }
    foreach ($requiredField in @(
        'actionId',
        'activationActionId',
        'completionActionId',
        'lease',
        'dispatchIndex',
        'activationIndex',
        'completionIndex'
    )) {
        if ($canonicalCascadeSource -notmatch
            "(?m)^\s*$([regex]::Escape($requiredField))\s*=") {
            Throw-CampaignFailure 'preflight' `
                "POC canonical v8 cascade summary lost '$requiredField'"
        }
    }
    foreach ($requiredEvent in @(
        "'ordinary-apply'",
        "'ordinary-activate'",
        "'turn-start-complete'"
    )) {
        if (-not $canonicalCascadeSource.Contains($requiredEvent)) {
            Throw-CampaignFailure 'preflight' `
                "POC canonical v8 cascade producer lost $requiredEvent"
        }
    }
    foreach ($obsoleteField in @(
        ('request' + 'Id'),
        ('completionRequest' + 'Id')
    )) {
        if ($canonicalCascadeSource -match
            "(?m)^\s*$([regex]::Escape($obsoleteField))\s*=") {
            Throw-CampaignFailure 'preflight' `
                "POC canonical summary retained obsolete v7 field '$obsoleteField'"
        }
    }

    $orderedStageSource = [regex]::Match(
        $pocSource,
        '(?ms)^function Assert-LiteralOrderedMasstestStage\(.*?(?=^function Assert-DeferredLiteralOrderedMasstestStage)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($orderedStageSource) -or
        -not $orderedStageSource.Contains("sourceMergeEvent = 'merge-applied/host'") -or
        $orderedStageSource.Contains(
            "sourceMergeEvent = 'merge-" + "commit-dispatched'")) {
        Throw-CampaignFailure 'preflight' `
            ('POC ordered source checkpoint must identify the v8 host ' +
             'merge-applied acknowledgement')
    }

    $deferredMssSource = [regex]::Match(
        $pocSource,
        '(?ms)^function Complete-LiteralOrderedMasstestDeferredProof\(.*?(?=^function Read-AutomaticMasstestHostBeginTurnSlots)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($deferredMssSource)) {
        Throw-CampaignFailure 'preflight' `
            'POC runner lost its ordered v8 deferred MSS summary producer'
    }
    foreach ($requiredField in @(
        'barrierHeldPeers',
        'mergeActionId',
        'fixedMergeClassification',
        'fixedMergeObserved',
        'completionMergeObserved',
        'mergeCompletionTimeoutSeconds',
        'naturalMergeBeginTurnAppliedAndDrained',
        'relayReleasedStockTurns'
    )) {
        if ($deferredMssSource -notmatch
            "(?m)^\s*$([regex]::Escape($requiredField))\s*=") {
            Throw-CampaignFailure 'preflight' `
                "POC ordered v8 deferred MSS summary lost '$requiredField'"
        }
    }
    foreach ($obsoleteField in @(
        ('commit' + 'Applied'),
        ('merge' + 'Released')
    )) {
        if ($deferredMssSource -match
            "(?m)^\s*$([regex]::Escape($obsoleteField))\s*=") {
            Throw-CampaignFailure 'preflight' `
                "POC ordered summary retained obsolete v7 field '$obsoleteField'"
        }
    }
    $legacyOracleSource = [regex]::Match(
        $pocSource,
        '(?ms)^function Complete-LiteralOrderedMasstestLegacyOracle\(.*?(?=^function Assert-DeferredCanonicalRoundEvidence)'
    ).Value
    if ([string]::IsNullOrWhiteSpace($legacyOracleSource)) {
        Throw-CampaignFailure 'preflight' `
            'POC runner lost its independently auditable legacy post-Phase-C oracle'
    }
    $legacyOracleOrder = @(
        '[long]$mergeActionId = Get-RequiredTelemetryNumber',
        '$MergeHandoff ''mergeActionId'' ''literal ordered merge handoff''',
        '$HostProcess.Refresh()',
        '$hostAlive = -not $HostProcess.HasExited',
        '$JoinProcess.Refresh()',
        '$joinerAlive = -not $JoinProcess.HasExited',
        '$hostLines = @(Read-ClientLogLines $HostLog)',
        '$mergeMarker =',
        '"[simturns] host executed merge transaction $mergeActionId via 0x420FFA"',
        '$mergeFired = [bool]($mergeMatches.Count -gt 0)',
        '$naturalMergeMarker =',
        '"[simturns] natural merge BeginTurn applied and drained (actionId=$mergeActionId, day=$mergeDay)"',
        '$uiMarshalled = [bool](@($hostLines |',
        'Select-String -SimpleMatch -Pattern $naturalMergeMarker).Count -gt 0)',
        '$uefAfterMerge = @($after | Select-String -Pattern ''\[UEF'').Count',
        '$vehLines = @($after | Select-String -Pattern ''\[VEH .*code=0xC0000005'')',
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
        $offset = $legacyOracleSource.IndexOf(
            $literal, $previousLegacyOracleOffset + 1,
            [StringComparison]::Ordinal)
        if ($offset -le $previousLegacyOracleOffset) {
            Throw-CampaignFailure 'preflight' `
                "POC legacy post-Phase-C oracle moved or lost '$literal'"
        }
        $previousLegacyOracleOffset = $offset
    }
    if ([regex]::Matches(
            $legacyOracleSource,
            'Read-ClientLogLines \$HostLog').Count -ne 1 -or
        [regex]::Matches(
            $legacyOracleSource,
            'Get-LiteralInnerDumpBaseline').Count -ne 1 -or
        $legacyOracleSource -match
            'Read-ClientLogLines \$JoinLog|Read-SimRelayEvents|Get-RelayState|' +
            'Get-RoleState|Get-World|Assert-ClientsLive|Assert-NoClientFaults|' +
            'Invoke-Button|Start-Sleep|Wait-') {
        Throw-CampaignFailure 'preflight' `
            'POC legacy oracle contains a duplicate read, action, pause, or premature MSS check'
    }

    $runtimeStart = $pocSource.LastIndexOf(
        "`n`$testRelay = `$null", [StringComparison]::Ordinal)
    if ($runtimeStart -lt 0) {
        Throw-CampaignFailure 'preflight' 'POC runtime boundary is not identifiable'
    }
    $pocRuntime = $pocSource.Substring($runtimeStart)
    $orderedRuntimeOrder = @(
        '$orderedInitialCheckpoint = Read-LiteralMasstestPeerStateCheckpoint',
        '$orderedMasstestDumpBaseline = Get-LiteralInnerDumpBaseline',
        '$orderedMergeHandoff = Run-LiteralOrderedMasstestToMerge',
        '$literalPhaseC = Run-AutomaticMasstestPhaseCLiteral',
        '$legacyPhaseCOracle = Complete-LiteralOrderedMasstestLegacyOracle',
        'if ([string]$legacyPhaseCOracle.legacyVerdict -ne ''PASS'')',
        '$deferredPhaseCMss = Complete-LiteralOrderedMasstestDeferredProof'
    )
    $previousOrderedRuntimeOffset = -1
    foreach ($literal in $orderedRuntimeOrder) {
        $offset = $pocRuntime.IndexOf(
            $literal, $previousOrderedRuntimeOffset + 1,
            [StringComparison]::Ordinal)
        if ($offset -le $previousOrderedRuntimeOffset) {
            Throw-CampaignFailure 'preflight' `
                "POC ordered runtime boundary moved or lost '$literal'"
        }
        $previousOrderedRuntimeOffset = $offset
    }
    foreach ($requiredSummaryToken in @(
        'barrierOrder = $BarrierOrder',
        'gameplay = $gameplayResult',
        'day2Walk = $day2WalkResult',
        'battleBlock = $battleBlockResult',
        'independentRounds = $roundResults',
        'merge = $mergeResult',
        'postMergeStock = $postMergeStockResult',
        'postMergeContinuationMode = [string]$PostMergeContinuationMode',
        'postMergeAutomaticMasstestPhaseC = $postMergeAutomaticMasstestPhaseCResult',
        'orderedMasstest = $orderedMasstestResult',
        'legacyPass = if ($null -eq $legacyPass)',
        'stepTranscript = @($script:StepTranscript)',
        'legacyMassPhaseTrace = @($script:LegacyMassPhaseTrace)',
        '$orderedMergeHandoff $literalPhaseC `',
        'firstMergeObservationSec = if ($legacyMergeObservation)',
        'confirmationCount = if ($null -ne $canonicalConfirmationCount)',
        'currentOwner = $currentMergeOwner',
        'hostPid = if ($hostProcess)',
        'joinPid = if ($joinProcess)'
    )) {
        if (-not $pocSource.Contains($requiredSummaryToken)) {
            Throw-CampaignFailure 'preflight' `
                "POC runner lost required summary contract token '$requiredSummaryToken'"
        }
    }
}

function Write-CampaignSummary {
    param([switch]$Final)
    if ([string]::IsNullOrWhiteSpace($script:CampaignSummaryPath)) { return }
    $completedRuns = @($runs | Where-Object {
        -not [string]::IsNullOrWhiteSpace([string]$_.finishedAt)
    }).Count
    $passedRuns = @($runs | Where-Object passed).Count
    $failedRuns = @($runs | Where-Object {
        -not [string]::IsNullOrWhiteSpace([string]$_.finishedAt) -and
        -not [bool]$_.passed
    }).Count
    $state = [ordered]@{
        schemaVersion = 1
        passed = $campaignPassed
        phase = $script:CurrentPhase
        category = if ($campaignPassed) { 'complete' } elseif ($campaignFailure) {
            $campaignFailure.category
        } else { 'running' }
        failure = $campaignFailure
        runId = $runId
        startedAt = $startedAt.ToString('O')
        finishedAt = if ($Final) { [DateTimeOffset]::UtcNow.ToString('O') } else { $null }
        gameDir = $GameDir
        fixtureManifest = $FixtureManifest
        artifactDir = $script:RunArtifactDir
        requestedMassCount = $Count
        stopOnFailure = [bool]$StopOnFailure
        requestedOrderedMasstestIterations = $script:OrderedMasstestIterations
        expectedRuns = $script:ExpectedRunCount
        completedRuns = $completedRuns
        passedRuns = $passedRuns
        failedRuns = $failedRuns
        naturalTeardownTimeoutSeconds = $naturalTeardownTimeoutSeconds
        runs = @($runs)
    }
    $state | ConvertTo-Json -Depth 20 |
        Set-Content -LiteralPath $script:CampaignSummaryPath -Encoding utf8
}

function Wait-NaturalDplayServerExit {
    $observed = @(Get-Process -Name 'dplaysvr' -ErrorAction SilentlyContinue)
    $observedPids = @($observed | ForEach-Object { [long]$_.Id })
    if ($observed.Count -eq 0) {
        return [pscustomobject]@{
            observedPids = @()
            elapsedMs = 0
            timeoutSeconds = $naturalTeardownTimeoutSeconds
            naturalExit = $true
        }
    }

    $clock = [Diagnostics.Stopwatch]::StartNew()
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($naturalTeardownTimeoutSeconds)
    foreach ($process in $observed) {
        $remaining = $deadline - [DateTimeOffset]::UtcNow
        $remainingMilliseconds = [Math]::Max(0, [Math]::Floor($remaining.TotalMilliseconds))
        if ($remainingMilliseconds -gt [int]::MaxValue) {
            $remainingMilliseconds = [int]::MaxValue
        }
        if ($remainingMilliseconds -le 0 -or
            -not $process.WaitForExit([int]$remainingMilliseconds)) {
            Throw-CampaignFailure 'dplaysvr-natural-exit-timeout' `
                ("dplaysvr did not exit naturally within " +
                 "$naturalTeardownTimeoutSeconds seconds (observed PIDs: $($observedPids -join ', '))")
        }
    }
    $stillPresent = @(Get-Process -Name 'dplaysvr' -ErrorAction SilentlyContinue)
    if ($stillPresent.Count -ne 0) {
        $remainingPids = @($stillPresent | ForEach-Object { [long]$_.Id })
        Throw-CampaignFailure 'dplaysvr-natural-exit-replacement' `
            ("a dplaysvr helper remains after the observed helpers exited " +
             "(PIDs: $($remainingPids -join ', '))")
    }
    $clock.Stop()
    return [pscustomobject]@{
        observedPids = $observedPids
        elapsedMs = [Math]::Round($clock.Elapsed.TotalMilliseconds, 1)
        timeoutSeconds = $naturalTeardownTimeoutSeconds
        naturalExit = $true
    }
}

function Wait-PinnedFixtureReady {
    param(
        [ValidateRange(1, 120)][int]$TimeoutSeconds = 30,
        [ValidateRange(50, 2000)][int]$PollMilliseconds = 250
    )

    $checks = @(
        @{ name = 'executable'; entry = $script:Fixture.executable },
        @{ name = 'map'; entry = $script:Fixture.map }
    )
    $expected = @()
    foreach ($check in $checks) {
        $entry = Assert-JsonObject $check.entry "fixture $($check.name)"
        $relativePath = Get-RequiredJsonProperty `
            $entry 'relativePath' "fixture $($check.name)"
        if ($relativePath -isnot [string] -or
            [string]::IsNullOrWhiteSpace($relativePath)) {
            Throw-CampaignFailure 'malformed-summary' `
                "fixture $($check.name) relativePath must be a non-empty string"
        }
        [long]$size = Convert-RequiredInt64 `
            (Get-RequiredJsonProperty $entry 'size' "fixture $($check.name)") `
            "fixture $($check.name) size"
        $sha256 = Get-RequiredJsonProperty `
            $entry 'sha256' "fixture $($check.name)"
        if ($size -le 0 -or $sha256 -isnot [string] -or
            $sha256 -notmatch '\A[0-9A-Fa-f]{64}\z') {
            Throw-CampaignFailure 'malformed-summary' `
                "fixture $($check.name) size/SHA-256 contract is invalid"
        }
        $expected += [pscustomobject]@{
            name = [string]$check.name
            path = [IO.Path]::GetFullPath((Join-Path $GameDir $relativePath))
            size = $size
            sha256 = [string]$sha256
        }
    }

    $clock = [Diagnostics.Stopwatch]::StartNew()
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastIssue = 'fixture readiness has not been sampled'
    do {
        $allReady = $true
        foreach ($item in $expected) {
            try {
                if (-not (Test-Path -LiteralPath $item.path -PathType Leaf)) {
                    $allReady = $false
                    $lastIssue = "$($item.name) is absent: $($item.path)"
                    continue
                }
                $file = Get-Item -LiteralPath $item.path -ErrorAction Stop
                if ([long]$file.Length -ne [long]$item.size) {
                    $allReady = $false
                    $lastIssue = ("{0} size is {1}, expected {2}: {3}" -f
                        $item.name, $file.Length, $item.size, $item.path)
                    continue
                }
                $hash = (Get-FileHash -LiteralPath $item.path `
                    -Algorithm SHA256 -ErrorAction Stop).Hash
                if (-not [string]::Equals(
                        $hash, $item.sha256,
                        [StringComparison]::OrdinalIgnoreCase)) {
                    $allReady = $false
                    $lastIssue = ("{0} SHA-256 is {1}, expected {2}: {3}" -f
                        $item.name, $hash, $item.sha256, $item.path)
                }
            } catch {
                $allReady = $false
                $lastIssue = "$($item.name) is not readable: $($_.Exception.Message)"
            }
        }
        if ($allReady) {
            $clock.Stop()
            Write-Host ("[simturns-campaign] pinned fixture ready after {0:N1}ms" -f
                $clock.Elapsed.TotalMilliseconds)
            return
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    } while ([DateTime]::UtcNow -lt $deadline)

    Throw-CampaignFailure 'fixture-unavailable' `
        ("pinned fixture did not become ready within $TimeoutSeconds seconds: " +
         $lastIssue)
}

function Invoke-ColdChild {
    param(
        [Parameter(Mandatory)][object]$Spec,
        [int]$Sequence
    )
    $childArtifactDir = Join-Path $script:RunArtifactDir `
        ('{0:D3}-{1}' -f $Sequence, [string]$Spec.phase)
    [void](New-Item -ItemType Directory -Path $childArtifactDir)
    $childSummaryPath = Join-Path $childArtifactDir 'summary.json'

    $record = [pscustomobject]@{
        sequence = $Sequence
        phase = [string]$Spec.phase
        category = [string]$Spec.category
        gameplayMode = [string]$Spec.gameplayMode
        barrierOrder = [string]$Spec.barrierOrder
        oldMassTestEquivalent = [bool]$Spec.oldMassTestEquivalent
        oldOrderedMasstestEquivalent = [bool]$Spec.oldOrderedMasstestEquivalent
        postMergeContinuationMode = [string]$Spec.postMergeContinuationMode
        artifactDir = $childArtifactDir
        summaryPath = $childSummaryPath
        childPwshPid = $null
        exitCode = $null
        passed = $false
        hostPid = $null
        joinPid = $null
        legacyMassStatus = $null
        legacyMass = $null
        validation = $null
        failure = $null
        teardownProved = $false
        dplaysvrTeardown = $null
        orderedInterIterationSettleSeconds = $null
        startedAt = [DateTimeOffset]::UtcNow.ToString('O')
        finishedAt = $null
    }
    $runs.Add($record)
    Write-CampaignSummary

    $arguments = @(New-ColdChildArguments $Spec $childArtifactDir)
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $script:PwshPath
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    foreach ($argument in $arguments) {
        [void]$startInfo.ArgumentList.Add([string]$argument)
    }

    $child = [Diagnostics.Process]::new()
    $child.StartInfo = $startInfo
    try {
        if (-not $child.Start()) {
            Throw-CampaignFailure 'child-start' `
                "could not start child pwsh for phase '$($Spec.phase)'"
        }
        $record.childPwshPid = [long]$child.Id
        Write-Host (("[simturns-campaign] START {0}/{1}: {2} " +
            "mode={3} barrier={4} continuation={5} childPid={6}") -f
            $Sequence, $script:ExpectedRunCount, $Spec.phase,
            $Spec.gameplayMode, $Spec.barrierOrder,
            $Spec.postMergeContinuationMode, $child.Id)
        $child.WaitForExit()
        $record.exitCode = [int]$child.ExitCode
        $record.finishedAt = [DateTimeOffset]::UtcNow.ToString('O')
    } finally {
        $child.Dispose()
    }

    $summary = Read-ChildSummary $childSummaryPath
    if ([bool]$Spec.oldMassTestEquivalent) {
        # Preserve mass_test.ps1::Analyze as the first child-result oracle. It
        # is a pure projection over the completed child's immutable transcript,
        # phase markers and actual state values; it cannot issue an action or
        # wait. Teardown is still proved before a failing verdict is raised so
        # the remaining cold children can be aggregated safely like the source.
        $stepTranscript = @(
            Get-OptionalJsonArrayProperty $summary 'stepTranscript')
        $phaseTrace = @(
            Get-OptionalJsonArrayProperty $summary 'legacyMassPhaseTrace')
        $reportedFailure = [string](Get-OptionalJsonProperty $summary 'failure')
        $legacyMass = Get-LegacyMassTestOracle `
            -Summary $summary `
            -StepTranscript $stepTranscript `
            -PhaseTrace $phaseTrace `
            -Failure $reportedFailure
        $record.legacyMassStatus = [string]$legacyMass.status
        $record.legacyMass = $legacyMass
        Write-CampaignSummary
    }
    [void](Assert-ChildOwnedTeardown $summary)
    $record.teardownProved = $true
    if ([bool]$Spec.oldMassTestEquivalent -and
        -not [string]::Equals(
            [string]$record.legacyMassStatus, 'PASS',
            [StringComparison]::Ordinal)) {
        Throw-CampaignFailure 'acceptance-mismatch' `
            ("legacy mass_test oracle classified '$($Spec.phase)' as " +
             "$($record.legacyMassStatus) (first state failure=" +
             "$($record.legacyMass.fail))")
    }
    Assert-DeclaredChildPass $summary $record.exitCode
    $validation = switch ([string]$Spec.gameplayMode) {
        'canonical' {
            Assert-CanonicalSummary `
                $summary $childArtifactDir $Spec.barrierOrder `
                -ExpectedPostMergeContinuationMode `
                    ([string]$Spec.postMergeContinuationMode)
            break
        }
        'battle-block' {
            Assert-BattleBlockSummary $summary $childArtifactDir
            break
        }
        'ordered-masstest' {
            Assert-OrderedMasstestSummary `
                $summary $childArtifactDir $Spec.barrierOrder
            break
        }
        default {
            Throw-CampaignFailure 'self-check' `
                "unsupported campaign gameplay mode '$($Spec.gameplayMode)'"
        }
    }
    $record.validation = $validation
    $record.hostPid = $validation.hostPid
    $record.joinPid = $validation.joinPid
    $record.passed = $true
    Write-Host "[simturns-campaign] PASS: $($Spec.phase)" -ForegroundColor Green
    Write-CampaignSummary
    return $record
}

try {
    $script:CurrentPhase = 'offline-self-check'
    Assert-CampaignSourceContract
    $specs = @(New-CampaignSpecPlan)
    Assert-CampaignSpecPlan $specs
    Assert-CampaignArgumentPlan $specs
    if ($StaticCheck) {
        Write-Host (("[simturns-campaign] STATIC CHECK PASS: Count={0}; " +
            "oldMassParallel={0}; battleBlock=1; orderedMasstest={1}; total={2}; " +
            "oldMassContinuation=none; orderedContinuation=literal-Phase-C") -f
            $Count, $script:OrderedMasstestIterations,
            $script:ExpectedRunCount) `
            -ForegroundColor Green
        foreach ($spec in $specs) {
            Write-Host (("  {0}: category={1} mode={2} barrier={3} " +
                "oldMass={4} oldOrderedMasstest={5} continuation={6}") -f
                $spec.phase, $spec.category, $spec.gameplayMode,
                $spec.barrierOrder, $spec.oldMassTestEquivalent,
                $spec.oldOrderedMasstestEquivalent,
                $spec.postMergeContinuationMode)
        }
        exit 0
    }

    $script:CurrentPhase = 'preflight'
    $artifactRoot = [IO.Path]::GetFullPath($ArtifactDir)
    [void](New-Item -ItemType Directory -Path $artifactRoot -Force)
    $script:RunArtifactDir = Join-Path $artifactRoot "campaign-$runId"
    [void](New-Item -ItemType Directory -Path $script:RunArtifactDir)
    $script:CampaignSummaryPath = Join-Path $script:RunArtifactDir 'campaign-summary.json'
    Write-CampaignSummary

    $GameDir = [IO.Path]::GetFullPath($GameDir)
    $FixtureManifest = [IO.Path]::GetFullPath($FixtureManifest)
    if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
        Throw-CampaignFailure 'preflight' "game directory does not exist: $GameDir"
    }
    if (-not (Test-Path -LiteralPath $FixtureManifest -PathType Leaf)) {
        Throw-CampaignFailure 'preflight' "fixture manifest does not exist: $FixtureManifest"
    }
    Write-CampaignSummary

    $pwsh = Get-Command pwsh -CommandType Application -ErrorAction Stop
    $script:PwshPath = $pwsh.Source

    try {
        $fixture = Get-Content -LiteralPath $FixtureManifest -Raw |
            ConvertFrom-Json -Depth 100
    } catch {
        Throw-CampaignFailure 'preflight' `
            "fixture manifest is not valid JSON: $($_.Exception.Message)"
    }
    $fixture = Assert-JsonObject $fixture 'fixture manifest root'
    $script:Fixture = $fixture
    $script:FixtureMergeDay = [int](Convert-RequiredInt64 `
        (Get-RequiredJsonProperty $fixture 'mergeDay' 'fixture manifest') `
        'fixture mergeDay')
    if ($script:FixtureMergeDay -ne 3) {
        Throw-CampaignFailure 'preflight' `
            ("canonical campaign requires fixture mergeDay=3 for its exact one " +
             "independent round, got $script:FixtureMergeDay")
    }
    $fixtureHost = Assert-JsonObject `
        (Get-RequiredJsonProperty $fixture 'host' 'fixture manifest') 'fixture host'
    $fixtureJoin = Assert-JsonObject `
        (Get-RequiredJsonProperty $fixture 'join' 'fixture manifest') 'fixture join'
    $fixtureHostPostBattle = Assert-JsonObject `
        (Get-RequiredJsonProperty $fixtureHost 'postBattleWalk' 'fixture host') `
        'fixture host postBattleWalk'
    $fixtureJoinPostBattle = Assert-JsonObject `
        (Get-RequiredJsonProperty $fixtureJoin 'postBattleWalk' 'fixture join') `
        'fixture join postBattleWalk'
    $fixtureHostDay2 = Assert-JsonObject `
        (Get-RequiredJsonProperty $fixtureHost 'day2Reverse' 'fixture host') `
        'fixture host day2Reverse'
    $fixtureJoinDay2 = Assert-JsonObject `
        (Get-RequiredJsonProperty $fixtureJoin 'day2Reverse' 'fixture join') `
        'fixture join day2Reverse'
    for ($index = 0; $index -lt $specs.Count; $index++) {
        $spec = $specs[$index]
        $script:CurrentPhase = [string]$spec.phase
        $record = $null
        try {
            [void](Wait-PinnedFixtureReady)
            $record = Invoke-ColdChild $spec ($index + 1)
        } catch {
            $category = [string]$_.Exception.Data['campaignCategory']
            if ([string]::IsNullOrWhiteSpace($category)) {
                $category = 'unexpected'
            }
            $phase = [string]$_.Exception.Data['campaignPhase']
            if ([string]::IsNullOrWhiteSpace($phase)) {
                $phase = [string]$spec.phase
            }

            if ($runs.Count -gt 0 -and
                [int]$runs[$runs.Count - 1].sequence -eq ($index + 1)) {
                $record = $runs[$runs.Count - 1]
                $record.failure = [pscustomobject]@{
                    phase = $phase
                    category = $category
                    message = $_.Exception.Message
                }
                Write-CampaignSummary
            }

            # Child result/summary validation is deliberately aggregate: the
            # old mass runners executed every cold child before tallying. A
            # process-start failure is campaign infrastructure, and an
            # unexpected runner failure cannot honestly be classified as a
            # completed child, so either remains terminal.
            $aggregateChildCategories = @(
                'missing-summary',
                'malformed-summary',
                'child-process-exit',
                'child-summary-failed',
                'acceptance-mismatch'
            )
            if ($category -notin $aggregateChildCategories -or
                $null -eq $record -or $null -eq $record.childPwshPid -or
                -not [bool]$record.teardownProved) {
                throw
            }
            Write-Host (("[simturns-campaign] CHILD FAIL {0}/{1}: {2} " +
                "category={3}: {4}") -f
                ($index + 1), $script:ExpectedRunCount, $spec.phase,
                $category, $_.Exception.Message) -ForegroundColor Red
        }
        if ([bool]$spec.oldOrderedMasstestEquivalent -and $null -ne $record) {
            # test_R_masstest.ps1 slept three seconds after every iteration,
            # including the twelfth, after its cleanup and before the next
            # summary/boot boundary.  This is a distinct source clock, not a
            # readiness retry and not a substitute for dplaysvr observation.
            Start-Sleep -Seconds 3
            $record.orderedInterIterationSettleSeconds = 3
            Write-CampaignSummary
        }
        if ($index -lt ($specs.Count - 1)) {
            $script:CurrentPhase = "dplaysvr-after-$($spec.phase)"
            Write-Host ("[simturns-campaign] observing bounded natural dplaysvr " +
                "teardown before the next cold child")
            $record.dplaysvrTeardown = Wait-NaturalDplayServerExit
            Write-CampaignSummary
        }
        if ($StopOnFailure -and -not [bool]$record.passed) {
            $script:CurrentPhase = [string]$record.failure.phase
            Throw-CampaignFailure `
                ([string]$record.failure.category) `
                ([string]$record.failure.message)
        }
    }

    $script:CurrentPhase = 'aggregate-tally'
    $failedChildren = @($runs | Where-Object { -not [bool]$_.passed })
    if ($failedChildren.Count -ne 0) {
        $failureTally = @($failedChildren | ForEach-Object {
            '{0}:{1}' -f $_.phase, $_.failure.category
        }) -join ', '
        Throw-CampaignFailure 'aggregate-child-failure' `
            ("$($failedChildren.Count) of $($script:ExpectedRunCount) cold children " +
             "failed after the complete suite ran: $failureTally")
    }

    $campaignPassed = $true
    $script:CurrentPhase = 'complete'
} catch {
    $category = $_.Exception.Data['campaignCategory']
    if ([string]::IsNullOrWhiteSpace([string]$category)) { $category = 'unexpected' }
    $phase = $_.Exception.Data['campaignPhase']
    if ([string]::IsNullOrWhiteSpace([string]$phase)) { $phase = $script:CurrentPhase }
    $campaignFailure = [pscustomobject]@{
        phase = [string]$phase
        category = [string]$category
        message = $_.Exception.Message
    }
    $script:CurrentPhase = [string]$phase
    Write-Host ("[simturns-campaign] FAIL phase={0} category={1}: {2}" -f
        $phase, $category, $_.Exception.Message) -ForegroundColor Red
} finally {
    if (-not [string]::IsNullOrWhiteSpace($script:CampaignSummaryPath)) {
        Write-CampaignSummary -Final
        Write-Host "[simturns-campaign] summary: $script:CampaignSummaryPath"
    }
}

if ($campaignPassed) {
    Write-Host ("[simturns-campaign] RESULT: PASS ({0} independent cold children)" -f
        $script:ExpectedRunCount) -ForegroundColor Green
    exit 0
}
Write-Error ("simultaneous-turn acceptance campaign failed: {0}" -f
    $campaignFailure.message) -ErrorAction Continue
exit 1

