[CmdletBinding()]
param(
    [string]$PipeName = $(
        if ($env:D2MSS_SIMTURNS_PIPE) {
            $env:D2MSS_SIMTURNS_PIPE
        }
        else {
            '\\.\pipe\d2mss.simturns.v8'
        }
    ),
    [string]$LogDirectory = (Join-Path $PSScriptRoot 'logs'),
    [ValidateScript({ $_ -eq 0 -or $_ -ge 2 })]
    [uint32]$MergeDay = 0,
    [string]$BootstrapReleaseFile,
    [ValidateRange(0, [int]::MaxValue)]
    [int]$BootstrapCascadeDelayMs = 0
)

$ErrorActionPreference = 'Stop'

if ($PipeName -notmatch '^\\\\\.\\pipe\\[A-Za-z0-9._-]+$') {
    throw 'PipeName must match \\.\pipe\[A-Za-z0-9._-]+'
}

$nodeCommand = Get-Command node -CommandType Application -ErrorAction Stop
$nodeVersionText = (& $nodeCommand.Source --version).TrimStart('v')
$nodeMajor = 0
if (-not [int]::TryParse(($nodeVersionText -split '\.')[0], [ref]$nodeMajor) -or $nodeMajor -lt 20) {
    throw "Node.js 20 or newer is required; found '$nodeVersionText'"
}

$cliPath = Join-Path $PSScriptRoot 'src\cli.js'
if (-not (Test-Path -LiteralPath $cliPath -PathType Leaf)) {
    throw "Relay entry point was not found: $cliPath"
}

if (-not [IO.Path]::IsPathRooted($LogDirectory)) {
    $LogDirectory = Join-Path $PSScriptRoot $LogDirectory
}
$LogDirectory = [IO.Path]::GetFullPath($LogDirectory)
[void](New-Item -ItemType Directory -Path $LogDirectory -Force)

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$logPath = Join-Path $LogDirectory "simturns-relay-$timestamp-pid$PID.jsonl"

Write-Host "Starting D2MSS simultaneous-turn relay"
Write-Host "Pipe: $PipeName"
Write-Host "Merge day: $MergeDay"
Write-Host "Log:  $logPath"
Write-Host 'Stop safely with Ctrl+C.'

$relayArguments = @(
    $cliPath,
    '--pipe',
    $PipeName,
    '--merge-day',
    $MergeDay.ToString([Globalization.CultureInfo]::InvariantCulture)
)
if ($PSBoundParameters.ContainsKey('BootstrapReleaseFile')) {
    if (-not [IO.Path]::IsPathRooted($BootstrapReleaseFile)) {
        throw 'BootstrapReleaseFile must be an absolute path'
    }
    $relayArguments += @(
        '--bootstrap-release-file',
        [IO.Path]::GetFullPath($BootstrapReleaseFile)
    )
}
if ($PSBoundParameters.ContainsKey('BootstrapCascadeDelayMs')) {
    $relayArguments += @(
        '--bootstrap-cascade-delay-ms',
        $BootstrapCascadeDelayMs.ToString([Globalization.CultureInfo]::InvariantCulture)
    )
}

& $nodeCommand.Source @relayArguments 2>&1 |
    Tee-Object -LiteralPath $logPath
$relayExitCode = $LASTEXITCODE

exit $relayExitCode
