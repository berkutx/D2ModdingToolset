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
    [string]$Game = "C:\GOG Games\slasher_mns_2_4",
    [string]$ProcDump = "",   # diagnostic: path to (32-bit) procdump.exe to attach for a crash dump
    [string]$DumpDir = ""      # diagnostic: where attached procdump writes <role>_<pid>.dmp
)

. "$PSScriptRoot\_show-window.ps1"

$exe = "$Game\Discipl2.exe"
Stop-Process -Name Discipl2, dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500
# clear stale per-pid logs
Get-ChildItem $Game -Filter "mss32_*.log" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

# Host-ready handshake: the joiner sits in the lobby and does NOT request start until
# this file appears. We create it only once the HOST has FULLY entered the strategic
# map (its "nav script complete"), so the host always serves a consistent scenario
# snapshot. This enforces the correct entry order (host fully in, THEN joiner) instead
# of letting the joiner pull a half-loaded snapshot.
$readyFlag = "$Game\host-ready.flag"
Remove-Item $readyFlag -Force -ErrorAction SilentlyContinue

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
    # The joiner waits for this file before requesting start (host-ready handshake);
    # harmless for the host, whose script has no WaitHostReady step.
    $psi.EnvironmentVariables["D2TESTDRV_HOST_READY_FILE"] = $readyFlag
    if ($Net) { $psi.EnvironmentVariables["D2TESTDRV_NET"] = "1" }
    return [System.Diagnostics.Process]::Start($psi)
}

function LogReady([string]$log, [string]$pattern) {
    if (-not (Test-Path $log)) { return $false }
    return [bool](Select-String -Path $log -Pattern $pattern -ErrorAction SilentlyContinue -Quiet)
}

# Diagnostic: one-shot full dump of a (hung) instance so we can see where its screen
# loop is stuck — entering the strategic map on software Mesa is not a crash (no
# exception is raised, procdump -e caught nothing) but a hang, so we snapshot the live
# process at the end and cdb its threads.
function SnapshotInstance([System.Diagnostics.Process]$Proc, [string]$Role) {
    if (-not $ProcDump -or -not $DumpDir) { return }
    if (-not $Proc -or $Proc.HasExited) { Write-Host "[pair] $Role already exited — nothing to snapshot"; return }
    # CPU load of this instance over a 2s window (in cores-worth: 1.00 = one core pegged).
    $cores = [System.Environment]::ProcessorCount
    $Proc.Refresh(); $t0 = $Proc.TotalProcessorTime; $w0 = Get-Date
    Start-Sleep -Seconds 2
    $Proc.Refresh(); $t1 = $Proc.TotalProcessorTime; $w1 = Get-Date
    $usedCores = ($t1 - $t0).TotalMilliseconds / ($w1 - $w0).TotalMilliseconds
    Write-Host ("[pair] {0} pid={1}: CPU={2:N2} cores of {3} ({4:P0} of one core), threads={5}, totalCPU={6:N1}s" -f `
        $Role, $Proc.Id, $usedCores, $cores, $usedCores, $Proc.Threads.Count, $t1.TotalSeconds)
    # thread states: how many are Running (busy) vs Wait (idle).
    try {
        Get-CimInstance Win32_Thread -Filter "ProcessHandle='$($Proc.Id)'" -ErrorAction Stop |
            Group-Object ThreadState | Sort-Object Name |
            ForEach-Object { Write-Host "[pair]   $Role threadState=$($_.Name): $($_.Count)" }
    } catch { Write-Host "[pair]   $Role thread-state query failed: $($_.Exception.Message)" }
    New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null
    $out = Join-Path $DumpDir "$Role`_$($Proc.Id).dmp"
    Write-Host "[pair] procdump -ma snapshot of hung $Role pid=$($Proc.Id) -> $out"
    & $ProcDump -accepteula -ma $Proc.Id $out 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Host "[pair][procdump] $_" }
}

# Diagnostic: bring an instance's window to the front and screenshot the desktop, to
# see the last frame the (hung) game presented. The game windows are full-screen
# 1024x768, so the primary-screen capture IS the window. Works even on a hung app —
# z-order/restore is handled by the window manager, and the last presented frame stays.
function CaptureWindow([System.Diagnostics.Process]$Proc, [string]$Role) {
    if (-not $DumpDir) { return }
    if (-not $Proc -or $Proc.HasExited) { Write-Host "[pair] $Role exited — no screenshot"; return }
    try { Show-GameWindow -Proc $Proc } catch { Write-Host "[pair] $Role Show-GameWindow: $($_.Exception.Message)" }
    Start-Sleep -Milliseconds 900
    $Proc.Refresh()
    Write-Host "[pair] $Role window: hwnd=$($Proc.MainWindowHandle) title='$($Proc.MainWindowTitle)'"
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.X, $b.Y, 0, 0, $b.Size)
        $png = Join-Path $DumpDir "$Role`_screen.png"
        $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Host "[pair] $Role screenshot -> $png ($([math]::Round((Get-Item $png).Length / 1kb))KB)"
    } catch { Write-Host "[pair] $Role screenshot failed: $($_.Exception.Message)" }
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
$hostOk = $false; $joinOk = $false; $released = $false
while ((((Get-Date) - $t0).TotalSeconds) -lt $TimeoutSec) {
    # Release the joiner only once the host has FULLY entered strategic (nav complete).
    if (-not $released -and (LogReady $hostLog "nav script complete")) {
        New-Item -ItemType File -Path $readyFlag -Force | Out-Null
        $released = $true
        Write-Host "[pair] HOST fully entered strategic (nav complete) -> releasing JOINER" -ForegroundColor Green
    }
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

# Diagnostic: snapshot any still-running instance on failure so its stuck stack is captured.
if (-not ($hostOk -and $joinOk)) {
    CaptureWindow $h "host"   # screenshot first (windows still up), host then joiner
    CaptureWindow $j "join"
    try {
        $sys = (Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 2 -ErrorAction Stop).CounterSamples[-1].CookedValue
        Write-Host ("[pair] system CPU = {0:N0}% across {1} cores" -f $sys, [System.Environment]::ProcessorCount)
    } catch { Write-Host "[pair] system CPU query failed: $($_.Exception.Message)" }
    SnapshotInstance $h "host"
    SnapshotInstance $j "join"
}

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
