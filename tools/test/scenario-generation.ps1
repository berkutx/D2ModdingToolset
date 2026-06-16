#requires -Version 7.0
<#
.SYNOPSIS
  Single-instance test: drive the random-scenario generator through the multiplayer menu —
  pick a SPECIFIC template (not the first), fill the player name, toggle spinners, and proceed
  to generate.

.DESCRIPTION
  A focused example of driving a complex FORM over the relay: it exercises every command type
  the dispatcher has — InvokeBtn (buttons), SetSel (the TLBOX_TEMPLATES listbox), SetEdit (the
  EDIT_NAME edit box) and SetSpin (the SPIN_SIZE/SPIN_GOAL spin buttons). One host instance
  navigates the multiplayer setup to the generator (DLG_RANDOM_SCENARIO_MULTI) and drives its
  form. Verification is relay-only: the generator opened, the agent executed the navigation
  (each StepTo transition required a real click), the form commands did not crash the agent
  (still on the dialog), and BTN_GENERATE was clicked. The generated .sg itself is NOT asserted
  — random-scenario generation depends on the game's own Lua templates, which is out of scope
  for a harness test.

.EXAMPLE
  .\scenario-generation.ps1 -Kill
#>
param(
    [switch]$Kill,
    [string]$Game = "C:\GOG Games\slasher_mns_2_4",
    [int]$Template = 3,           # a specific template index (not 0), proving non-default selection
    [string]$DumpDir = ""
)

. "$PSScriptRoot\_relay.ps1"
. "$PSScriptRoot\_show-window.ps1"

# Clean slate without a blanket kill: only our tagged window + a stale dplaysvr.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1000

$relay = Start-TestRelay -LogDir $(if ($DumpDir) { $DumpDir } else { $env:TEMP })
Write-Host "[gen] relay up on http://127.0.0.1:8077" -ForegroundColor Green
$h = $null; $ok = $false
try {
    Write-Host "[gen] launching host..." -ForegroundColor Cyan
    $h = Launch-Agent -Game $Game -Role "host"
    if (-not (WaitDlg "host" "DLG_MAIN_MENU" 90)) { throw "host never reached DLG_MAIN_MENU" }

    # Multiplayer setup -> the random-scenario generator (same nav the 2-instance host uses).
    if (-not (StepTo "host" "DLG_MAIN_MENU" "BTN_MULTI" "DLG_PROTOCOL" 45)) { throw "no DLG_PROTOCOL" }
    SetSel "host" "DLG_PROTOCOL" "TLBOX_PROTOCOL" 2; Start-Sleep 1   # 2 = TCP/IP
    if (-not (StepTo "host" "DLG_PROTOCOL" "BTN_CONTINUE" "DLG_LOAD_NEW_MULTI" 45)) { throw "no DLG_LOAD_NEW_MULTI" }
    # BTN_HOST opens DLG_HOST (with DLG_CHOOSE_SKIRMISH co-present); BTN_RANDOM_MAP lives there.
    if (-not (StepTo "host" "DLG_LOAD_NEW_MULTI" "BTN_HOST" "DLG_HOST" 45)) { throw "no DLG_HOST" }
    if (-not (StepTo "host" "DLG_HOST" "BTN_RANDOM_MAP" "DLG_RANDOM_SCENARIO_MULTI" 45)) { throw "no DLG_RANDOM_SCENARIO_MULTI" }
    Write-Host "[gen] generator opened (DLG_RANDOM_SCENARIO_MULTI)" -ForegroundColor Green

    # Drive the form. The generator is a custom menu whose per-frame tick is slower than the
    # native menus, so give each command a settle (~3s) instead of firing back-to-back.
    $D = "DLG_RANDOM_SCENARIO_MULTI"
    SetSel  "host" $D "TLBOX_TEMPLATES" $Template; Start-Sleep 3   # a SPECIFIC template, not the first
    SetEdit "host" $D "EDIT_NAME" "AutoTest";      Start-Sleep 3   # player name (BTN_GENERATE needs it)
    SetSpin "host" $D "SPIN_SIZE" 1;               Start-Sleep 3   # toggle the map-size spinner
    SetSpin "host" $D "SPIN_GOAL" 0;               Start-Sleep 3   # toggle the map-goal spinner

    # The form commands have no relay-visible value, but a crash WOULD be visible: confirm the
    # agent is still alive on the generator dialog after driving the whole form.
    if ((Dlg "host") -ne $D) { throw "agent left '$D' while driving the form (crash?) -> '$(Dlg "host")'" }
    Write-Host "[gen] form driven (template=$Template, name set, 2 spins toggled); agent responsive" -ForegroundColor Green

    InvokeBtn "host" $D "BTN_GENERATE"   # proceed to generation
    Write-Host "[gen] BTN_GENERATE clicked -> proceeded to generation" -ForegroundColor Green
    $ok = $true
} catch {
    Write-Host "[gen] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Write-Host ""
    Write-Host "==== RESULT: generator-form-driven=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if (-not $ok -and (Test-Path "$($env:TEMP)\relay.out.log")) {
        Write-Host "[gen] --- relay stdout (last 40) ---"; Get-Content "$($env:TEMP)\relay.out.log" -Tail 40 | ForEach-Object { Write-Host "[relay] $_" }
    }
    if ($Kill) {
        if ($h) { Stop-Process -Id $h.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
        Write-Host "[gen] instance + relay closed."
    } elseif ($h) {
        Show-GameWindow -Proc $h
        Write-Host "[gen] left running (relay pid=$($relay.Id))." -ForegroundColor Yellow
    }
}
if (-not $ok) { Write-Error "scenario-generation form drive failed"; exit 1 }
