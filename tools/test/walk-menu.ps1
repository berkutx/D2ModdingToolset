#requires -Version 7.0
<#
.SYNOPSIS
  Launch one game instance with the test driver, let it walk the menu for a timeout,
  print the [testdrv]/[nettrace] log tail, and LEAVE THE GAME RUNNING (foregrounded)
  so you can poke at it. Pass -Kill to close it at the end instead.

.EXAMPLE
  .\walk-menu.ps1                       # role 'probe', stays open
  .\walk-menu.ps1 -Role host -Seconds 45
  .\walk-menu.ps1 -Role probe -Kill     # close when done

.NOTES
  host/join roles write a per-PID log (mss32_<pid>.log); other roles use mss32.log.
#>
param(
    [string]$Role = "probe",
    [int]$Seconds = 22,
    [switch]$Net,
    [switch]$Kill,
    [string]$Game = "C:\GOG Games\slasher_mns_2_4"
)

. "$PSScriptRoot\_show-window.ps1"

$exe = "$Game\Discipl2.exe"
$shared = "$Game\mss32.log"
Stop-Process -Name Discipl2, dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500
$mark = if (Test-Path $shared) { (Get-Item $shared).Length } else { 0 }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.WorkingDirectory = $Game
$psi.UseShellExecute = $false
$psi.EnvironmentVariables["D2TESTDRV_SKIP_INTRO"] = "1"
$psi.EnvironmentVariables["D2TESTDRV_BLACKSCREEN_FIX"] = "1"
$psi.EnvironmentVariables["D2TESTDRV_UI_REPORTER"] = "1"
$psi.EnvironmentVariables["D2TESTDRV_ROLE"] = $Role
$psi.EnvironmentVariables["D2TESTDRV_SELFNAV"] = "1"            # run the built-in role script
if ($Net) { $psi.EnvironmentVariables["D2TESTDRV_NET_INTERCEPT"] = "1" }  # RX/TX trace logging

$p = [System.Diagnostics.Process]::Start($psi)

# host/join write a per-PID log; other roles append to the shared mss32.log.
if ($Role -in @('host', 'join', 'joiner')) { $log = "$Game\mss32_$($p.Id).log"; $from = 0 }
else { $log = $shared; $from = $mark }

Write-Host "launched pid=$($p.Id) role=$Role net=$Net; log=$log; waiting ${Seconds}s..." -ForegroundColor Cyan
Start-Sleep -Seconds $Seconds

Write-Host "=== [testdrv]/[nettrace] log (this run) ===" -ForegroundColor Green
if (Test-Path $log) {
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($from, 'Begin') | Out-Null
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
    } finally { $fs.Close() }
    $text -split "`r?`n" | Where-Object { $_ -match '\[testdrv\]|\[nettrace\]' } | ForEach-Object { $_ }
} else {
    Write-Host "(no log at $log)" -ForegroundColor Red
}

if ($Kill) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Write-Host "`n(game closed)"
} elseif (-not $p.HasExited) {
    Show-GameWindow -Proc $p
    Write-Host "`ngame left running (pid=$($p.Id)), brought to foreground — poke away. Close with: Stop-Process -Id $($p.Id)" -ForegroundColor Yellow
} else {
    Write-Host "`n(game already exited — role '$Role' navigated to a quit, or crashed; check the log)" -ForegroundColor DarkYellow
}
