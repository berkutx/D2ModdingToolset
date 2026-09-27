#requires -Version 7.0
<#
.SYNOPSIS
One harness entry point: local fixed-fixture acceptance or authenticated lobby smoke.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Local', 'Lobby')][string]$Transport,
    [string]$GameDir,
    [string]$ArtifactDir,
    [string]$FixtureManifest,
    [string]$TemplateName,
    [ValidateSet('protocol', 'canonical', 'battle-block', 'long-move', 'long-attack', 'ordered-masstest')]
    [string]$GameplayMode = 'protocol',
    [switch]$Campaign,
    [switch]$StopOnFailure,
    [switch]$Keep,
    [switch]$StaticCheck,
    [hashtable]$LocalOptions = @{}
)
$ErrorActionPreference = 'Stop'
if (-not $StaticCheck -and [string]::IsNullOrWhiteSpace($GameDir)) {
    throw 'Pass GameDir explicitly; the transport selector never chooses an implicit game installation.'
}
if ($Transport -eq 'Lobby') {
    # A generated lobby map is not the immutable DevouringMarshes fixture. Refuse
    # before starting any process instead of silently weakening those assertions.
    if ($Campaign -or $GameplayMode -ne 'protocol' -or $FixtureManifest -or $LocalOptions.Count) {
        throw 'Lobby supports generated-map bootstrap smoke here, not the fixed-fixture 18-case campaign. Use -Transport Local for that unchanged oracle.'
    }
    & "$PSScriptRoot/lobby-simturns-smoke.ps1" -GameDir $GameDir -ArtifactDir $ArtifactDir `
        -TemplateName $TemplateName -Keep:$Keep -StaticCheck:$StaticCheck
    exit 0
}
if ($TemplateName) { throw 'TemplateName belongs to the generated Lobby smoke, not Local acceptance.' }
$arguments = @{} + $LocalOptions
foreach ($name in @('GameDir', 'ArtifactDir', 'FixtureManifest', 'StaticCheck')) {
    if ($PSBoundParameters.ContainsKey($name)) {
        if ($arguments.ContainsKey($name)) { throw "Duplicate local argument: $name" }
        $arguments[$name] = $PSBoundParameters[$name]
    }
}
if ($Campaign) {
    if ($Keep -or $GameplayMode -ne 'protocol') { throw 'Campaign owns its cold scenario plan; Keep/GameplayMode are single-run options.' }
    $arguments.StopOnFailure = $StopOnFailure
    & "$PSScriptRoot/simturns-acceptance-campaign.ps1" @arguments
} else {
    if ($StopOnFailure) { throw 'StopOnFailure belongs to Campaign.' }
    $arguments.GameplayMode = $GameplayMode
    $arguments.Keep = $Keep
    & "$PSScriptRoot/simturns-production-poc.ps1" @arguments
}
exit $LASTEXITCODE
