#requires -Version 7.0
<#
.SYNOPSIS
  Launch one game client with the built-in self-nav, let it walk the menu for a timeout, print
  this run's [testdrv] log tail, and LEAVE THE GAME RUNNING (foregrounded) to poke at.
  Pass -Kill to close it at the end instead.

.EXAMPLE
  .\walk-menu.ps1 -Seconds 20               # quit from the main menu
  .\walk-menu.ps1 -Kill                     # close when done

.NOTES
  Self-nav is intentionally limited to the tiny quit smoke. Rich navigation is driven by the
  dispatcher scripts. host/join roles
  write a per-PID log (mss32_<pid>.log); other roles append to the shared mss32.log.
#>
param(
    [string]$Role = "exit",
    [int]$Seconds = 22,
    [switch]$Kill,
    [string]$GameDir
)

. "$PSScriptRoot\_relay.ps1"
. "$PSScriptRoot\_show-window.ps1"
$GameDir = Resolve-GameDir $GameDir

$shared = "$GameDir\mss32.log"
Stop-Process -Name Discipl2, dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500
$mark = if (Test-Path $shared) { (Get-Item $shared).Length } else { 0 }

$flags = @('SKIP_INTRO', 'BLACKSCREEN_FIX', 'UI_REPORTER', 'SELFNAV')
$p = Start-GameClient -GameDir $GameDir -Role $Role -Flags $flags

# host/join write a per-PID log; other roles append to the shared mss32.log.
if ($Role -in @('host', 'join', 'joiner')) { $log = "$GameDir\mss32_$($p.Id).log"; $from = 0 }
else { $log = $shared; $from = $mark }

Write-Host "launched pid=$($p.Id) role=$Role; log=$log; waiting ${Seconds}s..." -ForegroundColor Cyan
Start-Sleep -Seconds $Seconds

Write-Host "=== [testdrv] log (this run) ===" -ForegroundColor Green
if (Test-Path $log) {
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($from, 'Begin') | Out-Null
        $text = (New-Object System.IO.StreamReader($fs)).ReadToEnd()
    } finally { $fs.Close() }
    $text -split "`r?`n" | Where-Object { $_ -match '\[testdrv\]' } | ForEach-Object { $_ }
} else {
    Write-Host "(no log at $log)" -ForegroundColor Red
}

if ($Kill) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Write-Host "`n(game closed)"
} elseif (-not $p.HasExited) {
    Show-GameWindow -Proc $p
    Write-Host "`ngame left running (pid=$($p.Id)), foregrounded, poke away. Close with: Stop-Process -Id $($p.Id)" -ForegroundColor Yellow
} else {
    Write-Host "`n(game already exited, role '$Role' navigated to a quit, or crashed; check the log)" -ForegroundColor DarkYellow
}
