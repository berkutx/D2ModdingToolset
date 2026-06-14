#requires -Version 7.0
<#
.SYNOPSIS
  Launch two game instances (HOST + JOINER) and drive them — each self-navigating
  by role — into a started TCP/IP multiplayer game on localhost (just reach game
  start; no further gameplay automation).

.DESCRIPTION
  Requires the DebugTest mss32 build deployed in -Game. Host is launched first;
  the joiner is launched once the host has actually created the DPlay session
  (detected from the host's log), then both follow their role scripts. Each
  instance writes a per-PID log mss32_<pid>.log; this script polls both for the
  strategic-map signal. Windows are tagged [HOST] / [CLIENT].

.EXAMPLE
  .\pair-mp.ps1                       # default scenario row 0
  .\pair-mp.ps1 -Scenario 1 -Net      # pick scenario row 1, also start net trace
  .\pair-mp.ps1 -Kill                 # close both instances when done
#>
param(
    [int]$TimeoutSec = 150,
    [int]$HostSessionWaitSec = 40,
    [int]$Scenario = 0,
    [switch]$Net,
    [switch]$Kill,
    [string]$Game = "C:\GOG Games\slasher_mns_2_4"
)

. "$PSScriptRoot\_show-window.ps1"

$exe = "$Game\Discipl2.exe"
Stop-Process -Name Discipl2, dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500
# clear stale per-pid logs
Get-ChildItem $Game -Filter "mss32_*.log" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

function Launch([string]$Role) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $Game
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["D2TESTDRV_SKIP_INTRO"] = "1"
    $psi.EnvironmentVariables["D2TESTDRV_BOOT"] = "1"
    $psi.EnvironmentVariables["D2TESTDRV_UI"] = "1"
    $psi.EnvironmentVariables["D2TESTDRV_ROLE"] = $Role
    $psi.EnvironmentVariables["D2TESTDRV_SCENARIO_INDEX"] = "$Scenario"
    if ($Net) { $psi.EnvironmentVariables["D2TESTDRV_NET"] = "1" }
    return [System.Diagnostics.Process]::Start($psi)
}

function LogReady([string]$log, [string]$pattern) {
    if (-not (Test-Path $log)) { return $false }
    return [bool](Select-String -Path $log -Pattern $pattern -ErrorAction SilentlyContinue -Quiet)
}

Write-Host "[pair] launching HOST..." -ForegroundColor Cyan
$h = Launch "host"
$hostLog = "$Game\mss32_$($h.Id).log"
Write-Host "[pair] host pid=$($h.Id) log=$hostLog"

# Wait until the host has actually created the DPlay session before the joiner enumerates.
$t0 = Get-Date
while ((((Get-Date) - $t0).TotalSeconds) -lt $HostSessionWaitSec) {
    if (LogReady $hostLog "CreateSession") { Write-Host "[pair] host created session" -ForegroundColor Green; break }
    if ($h.HasExited) { Write-Host "[pair] HOST exited before creating a session" -ForegroundColor Red; exit 1 }
    Start-Sleep -Milliseconds 500
}

Write-Host "[pair] launching JOINER..." -ForegroundColor Cyan
$j = Launch "join"
$joinLog = "$Game\mss32_$($j.Id).log"
Write-Host "[pair] joiner pid=$($j.Id) log=$joinLog"

$t0 = Get-Date
$hostOk = $false; $joinOk = $false
while ((((Get-Date) - $t0).TotalSeconds) -lt $TimeoutSec) {
    if (-not $hostOk -and (LogReady $hostLog "DLG_STRATEGIC")) { $hostOk = $true; Write-Host "[pair] HOST reached strategic map" -ForegroundColor Green }
    if (-not $joinOk -and (LogReady $joinLog "DLG_STRATEGIC|DLG_ISO_PAL")) { $joinOk = $true; Write-Host "[pair] JOINER reached strategic map" -ForegroundColor Green }
    if ($hostOk -and $joinOk) { break }
    if ($h.HasExited -or $j.HasExited) { Write-Host "[pair] an instance exited early" -ForegroundColor Red; break }
    Start-Sleep -Seconds 1
}

Write-Host ""
Write-Host "==== RESULT: host-started=$hostOk  joiner-started=$joinOk ====" -ForegroundColor $(if ($hostOk -and $joinOk) { 'Green' } else { 'Yellow' })
Write-Host "host  log: $hostLog"
Write-Host "join  log: $joinLog"

if ($Kill) {
    Stop-Process -Id $h.Id, $j.Id -Force -ErrorAction SilentlyContinue
    Write-Host "[pair] both instances closed."
} else {
    # Bring both windows forward so they render for manual poking ([HOST]/[CLIENT]).
    Show-GameWindow -Proc $j
    Show-GameWindow -Proc $h
    Write-Host "[pair] instances left running ([HOST] pid=$($h.Id), [CLIENT] pid=$($j.Id)) — poke away." -ForegroundColor Yellow
    Write-Host "[pair] close with: Stop-Process -Name Discipl2"
}

# Gate CI on the result: both instances must reach the strategic map.
if (-not ($hostOk -and $joinOk)) {
    Write-Error "MP pairing did not reach the strategic map (host=$hostOk join=$joinOk)"
    exit 1
}
