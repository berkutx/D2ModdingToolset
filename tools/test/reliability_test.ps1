#requires -Version 7.0
param([int]$N = 20, [int]$TimeoutSec = 45, [string]$Boot = "", [string]$Activate = "", [int]$SettleMs = 2500, [switch]$Keep, [string]$Game = "C:\GOG Games\slasher_mns_2_4")
. "$PSScriptRoot\_show-window.ps1"
$game = $Game
$exe  = "$game\Discipl2.exe"
$log  = "$game\mss32.log"
function MenuCount {
  if (Test-Path $log) { return (Select-String -Path $log -Pattern "dialog now: DLG_MAIN_MENU" -ErrorAction SilentlyContinue).Count }
  return 0
}
$results = @()
for ($i = 1; $i -le $N; $i++) {
  Stop-Process -Name Discipl2, dplaysvr -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds $SettleMs
  $before = MenuCount
  # CreateProcess (UseShellExecute=false) -> headless, no foreground grant; explicit env.
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  $psi.WorkingDirectory = $game
  $psi.UseShellExecute = $false
  $psi.EnvironmentVariables["D2TESTDRV_SKIP_INTRO"] = "1"
  $psi.EnvironmentVariables["D2TESTDRV_UI"] = "1"
  $psi.EnvironmentVariables["D2TESTDRV_ROLE"] = "exit"
  $psi.EnvironmentVariables["D2TESTDRV_BOOT"] = $Boot
  $psi.EnvironmentVariables["D2TESTDRV_ACTIVATE"] = $Activate
  $t0 = Get-Date
  $p = [System.Diagnostics.Process]::Start($psi)
  $reached = $false
  while ((((Get-Date) - $t0).TotalSeconds) -lt $TimeoutSec) {
    if ((MenuCount) -gt $before) { $reached = $true; break }
    if ($p.HasExited) { Start-Sleep -Milliseconds 400; if ((MenuCount) -gt $before) { $reached = $true }; break }
    Start-Sleep -Milliseconds 500
  }
  $secs = [math]::Round((((Get-Date) - $t0).TotalSeconds), 1)
  $results += [pscustomobject]@{ Run = $i; Menu = $reached; Secs = $secs }
  Write-Output ("Run {0,2} : menu={1} ({2}s)" -f $i, $reached, $secs)
  if (-not ($Keep -and $i -eq $N)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
if ($Keep) {
  Show-GameWindow -Proc $p
  Write-Output ("Kept last instance running (pid={0}), brought to foreground. Close with: Stop-Process -Id {0}" -f $p.Id)
} else {
  Stop-Process -Name Discipl2 -Force -ErrorAction SilentlyContinue
}
$ok = ($results | Where-Object { $_.Menu }).Count
Write-Output ("==== Boot='{0}' Activate='{1}' : {2}/{3} reached the menu ====" -f $Boot, $Activate, $ok, $N)
$succ = $results | Where-Object { $_.Menu }
if ($succ) { Write-Output ("avg time-to-menu: {0}s" -f [math]::Round(($succ | Measure-Object -Property Secs -Average).Average, 1)) }
