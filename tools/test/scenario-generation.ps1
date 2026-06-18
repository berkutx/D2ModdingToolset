#requires -Version 7.0
<#
.SYNOPSIS
  Single-instance example: drive the random-scenario generator's form over the relay.

.DESCRIPTION
  The reference example for the test toolkit. One host client navigates the multiplayer menu to
  the generator (DLG_RANDOM_SCENARIO_MULTI), reads the live UI snapshot to confirm the form is
  there, then exercises every command type: Set-ListSelection (the template list), Set-EditText
  (the player name), Set-SpinOption (the size/goal spinners) and Invoke-Button (Generate).

  Verification is relay-only: the generator opened, every Step-ToDialog transition required a
  real click, the expected widgets are present, and the form commands left the client alive on
  the dialog. By default the generated map is not awaited; with -WaitGenerationSec the test also
  waits for Generate to reach DLG_GENERATION_RESULT and fails if it errors or never finishes.
  The template-matrix workflow runs this once per template with -WaitGenerationSec set.

.EXAMPLE
  .\scenario-generation.ps1                         # drive the form for template 3 and close
  .\scenario-generation.ps1 -Template 0 -WaitGenerationSec 300   # also assert the map generates
  .\scenario-generation.ps1 -Keep                   # leave the client + relay up to poke at
#>
param(
    [string]$GameDir,
    [int]$Template = 3,           # listbox index of the template to generate
    [int]$WaitGenerationSec = 0,  # >0: after Generate, wait this long and assert the map generates
    [switch]$Keep
)

. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir

# Clean slate without a blanket kill: only our tagged window + a stale dplaysvr.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1000

$relay = Start-TestRelay
Write-Host "[gen] relay up; launching host..." -ForegroundColor Cyan
$client = $null; $ok = $false
try {
    $client = Start-GameClient -GameDir $GameDir -Role host
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { throw "host never reached DLG_MAIN_MENU" }

    # Multiplayer setup -> the random-scenario generator.
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { throw "no DLG_PROTOCOL" }
    if (-not (Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2)) { throw "TLBOX_PROTOCOL not set" }   # 2 = TCP/IP
    Start-Sleep 1
    if (-not (Step-ToDialog host DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { throw "no DLG_LOAD_NEW_MULTI" }
    # BTN_HOST opens DLG_HOST (with DLG_CHOOSE_SKIRMISH co-present); BTN_RANDOM_MAP lives there.
    if (-not (Step-ToDialog host DLG_LOAD_NEW_MULTI BTN_HOST DLG_HOST)) { throw "no DLG_HOST" }
    if (-not (Step-ToDialog host DLG_HOST BTN_RANDOM_MAP DLG_RANDOM_SCENARIO_MULTI)) { throw "no generator" }

    $D = "DLG_RANDOM_SCENARIO_MULTI"
    # Read the live snapshot and confirm the form's widgets are present before driving them.
    $names = (Get-GameUi host).widgets.name
    foreach ($w in 'TLBOX_TEMPLATES', 'EDIT_NAME', 'SPIN_SIZE', 'BTN_GENERATE') {
        if ($names -notcontains $w) { throw "generator missing $w (widgets: $($names -join ', '))" }
    }
    Write-Host "[gen] generator open ($($names.Count) widgets)" -ForegroundColor Green

    # Drive the form. Each command returns whether it found its widget, so a wrong name or a closed
    # dialog is caught here. The generator is a custom menu that ticks slower than the native menus,
    # so give each command a settle (~3s) instead of firing back-to-back.
    if (-not (Set-ListSelection host $D TLBOX_TEMPLATES $Template)) { throw "TLBOX_TEMPLATES not set" }
    Start-Sleep 3
    if (-not (Set-EditText host $D EDIT_NAME "AutoTest")) { throw "EDIT_NAME not set" }   # BTN_GENERATE needs a name
    Start-Sleep 3
    if (-not (Set-SpinOption host $D SPIN_SIZE 1)) { throw "SPIN_SIZE not set" }
    Start-Sleep 3
    if (-not (Set-SpinOption host $D SPIN_GOAL 0)) { throw "SPIN_GOAL not set" }
    Start-Sleep 3

    if ((Get-Dialog host) -ne $D) { throw "client left '$D' while driving the form (crash?)" }
    if (-not (Invoke-Button host $D BTN_GENERATE)) { throw "BTN_GENERATE not found" }
    Write-Host "[gen] form driven (template=$Template) + BTN_GENERATE clicked" -ForegroundColor Green

    if ($WaitGenerationSec -gt 0) {
        # Assert generation runs to a result. A bad template errors at once (the client stays on the
        # generator form); a working one shows DLG_WAIT_GENERATION, then DLG_GENERATION_RESULT.
        # Generation is CPU-heavy, so allow a generous timeout.
        Write-Host "[gen] waiting up to ${WaitGenerationSec}s for generation to finish..." -ForegroundColor Cyan
        $t0 = Get-Date; $started = $false; $done = $false
        while ((((Get-Date) - $t0).TotalSeconds) -lt $WaitGenerationSec) {
            $d = Get-Dialog host
            if ($d -eq 'DLG_WAIT_GENERATION') { $started = $true }
            if ($d -eq 'DLG_GENERATION_RESULT') { $done = $true; break }
            Start-Sleep -Milliseconds 2000
        }
        if ($done) { Write-Host "[gen] generation finished (DLG_GENERATION_RESULT)" -ForegroundColor Green }
        elseif ($started) { throw "generation did not finish in ${WaitGenerationSec}s (still on DLG_WAIT_GENERATION)" }
        else { throw "generation never started (template $Template errored; client on $(Get-Dialog host))" }
    }
    $ok = $true
} catch {
    Write-Host "[gen] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Write-Host "`n==== RESULT: generator-form-driven=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if ($Keep) {
        Write-Host "[gen] left running (relay pid=$($relay.Id), client pid=$($client.Id))." -ForegroundColor Yellow
    } else {
        if ($client) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
    }
}
if (-not $ok) { Write-Error "scenario-generation form drive failed"; exit 1 }
