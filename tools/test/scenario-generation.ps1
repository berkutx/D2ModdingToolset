#requires -Version 7.0
<#
.SYNOPSIS
  Single-instance example: drive the random-scenario generator over the relay, optionally all the
  way into the generated map.

.DESCRIPTION
  The reference example for the test toolkit. One host client navigates the multiplayer menu to
  the generator (DLG_RANDOM_SCENARIO_MULTI), reads the live UI snapshot to confirm the form is
  there, then exercises every command type: Set-ListSelection (the template list), Set-EditText
  (the player name), Set-SpinOption (the size/goal spinners) and Invoke-Button (Generate).

  Verification is relay-only: the generator opened, every Step-ToDialog transition required a
  real click, the expected widgets are present, and the form commands left the client alive on
  the dialog. By default the generated map is not awaited; with -WaitGenerationSec the test also
  waits for Generate to reach DLG_GENERATION_RESULT and fails if it errors or never finishes.
  With -ToMap it goes the whole way: accept the result, start the game solo (AI fills the other
  slots), dismiss the first-turn popups and assert the strategic map (DLG_STRATEGIC/DLG_ISO_PAL),
  proving the generated scenario actually loads and plays, not just that generation completed.
  The template-matrix workflow runs this once per template with -WaitGenerationSec set.

.EXAMPLE
  .\scenario-generation.ps1                         # drive the form for template 3 and close
  .\scenario-generation.ps1 -Template 0 -WaitGenerationSec 300   # also assert the map generates
  .\scenario-generation.ps1 -Template 7 -ToMap      # generate, then play into the strategic map
  .\scenario-generation.ps1 -Keep                   # leave the client + relay up to poke at
#>
param(
    [string]$GameDir,
    [int]$Template = 3,           # listbox index of the template to generate
    [int]$WaitGenerationSec = 0,  # >0: after Generate, wait this long and assert the map generates
    [switch]$ToMap,               # after the result, accept + start the game and reach the strategic map
    [switch]$Keep
)

. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir
if ($ToMap -and $WaitGenerationSec -le 0) { $WaitGenerationSec = 300 }   # -ToMap must first reach the result

# Clean slate without a blanket kill: only our tagged window + a stale dplaysvr.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1000

$relay = Start-TestRelay
Write-Host "[gen] relay up; launching host..." -ForegroundColor Cyan
$client = $null; $ok = $false; $outcome = 'not-run'; $genSec = $null   # per-template result + generation seconds for the matrix summary
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
        # Assert generation runs to a result. A bad template fails three ways: it errors at once and
        # stays on the form (sol3 panic, printed to the game's stderr), it runs a while then pops
        # DLG_MESSAGE_BOX (the generator gave up after N attempts), or it never leaves
        # DLG_WAIT_GENERATION. A working one reaches DLG_GENERATION_RESULT in well under a minute, so a
        # long wait means failure, not slowness; $outcome records which, for the matrix summary.
        Write-Host "[gen] waiting up to ${WaitGenerationSec}s for generation to finish..." -ForegroundColor Cyan
        $t0 = Get-Date; $started = $false; $done = $false; $errbox = $false; $crashed = $false
        while ((((Get-Date) - $t0).TotalSeconds) -lt $WaitGenerationSec) {
            if ($client.HasExited) { $crashed = $true; break }   # a debug assert / fault killed the game
            $d = Get-Dialog host
            if ($d -eq 'DLG_WAIT_GENERATION') { $started = $true }
            if ($d -eq 'DLG_GENERATION_RESULT') { $done = $true; break }
            if ($d -eq 'DLG_MESSAGE_BOX') { $errbox = $true; break }   # generator gave up with an error popup
            # A sol3 panic never leaves the form for DLG_WAIT_GENERATION; fail it fast, do not wait it out.
            if (-not $started -and $d -eq $D -and (((Get-Date) - $t0).TotalSeconds) -ge 8) { break }
            Start-Sleep -Milliseconds 1000
        }
        $genSec = [int]((Get-Date) - $t0).TotalSeconds   # seconds spent generating (from BTN_GENERATE), for the summary
        if ($done) { $outcome = 'generated'; Write-Host "[gen] generation finished (DLG_GENERATION_RESULT) in ${genSec}s" -ForegroundColor Green }
        elseif ($crashed) { $outcome = 'crashed (generator assert)'; throw "the game crashed during generation (template $Template; see the captured assert)" }
        elseif ($errbox) {
            $msg = ((((Get-GameUi host).widgets | Where-Object { $_.type -eq 'text' }).state.text) -join ' | ') -replace '[\r\n\t]+', ' '
            $outcome = "error box: $msg"
            throw "generation errored after starting (template $Template; DLG_MESSAGE_BOX: $msg)"
        }
        elseif ($started) { $outcome = "no result in ${WaitGenerationSec}s"; throw "generation did not finish in ${WaitGenerationSec}s (still on DLG_WAIT_GENERATION)" }
        else { $outcome = 'sol3 panic (errored on the generator form)'; throw "generation never started (template $Template; sol3 panic on the form)" }
    }

    if ($ToMap) {
        # Generation finished (the wait above reached DLG_GENERATION_RESULT or threw). Accept the
        # map into the host lobby, then start solo: empty slots become AI, the game loads, and the
        # first-turn popups give way to the strategic map. This proves the scenario actually plays.
        # Keep generation-only success distinct from a playable-map success: any later throw must
        # remain red in the all-template matrix.
        $outcome = 'generated, map not reached'
        if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { throw "BTN_ACCEPT not found" }
        if (-not (Wait-Dialog host DLG_LOBBY 20)) { throw "BTN_ACCEPT did not open DLG_LOBBY" }
        Write-Host "[gen] map accepted; starting solo host (AI fills the rest)..." -ForegroundColor Cyan

        # First-turn popups, each mapped to the button that dismisses it (same set the MP test uses).
        $popups = @{
            'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'
            'DLG_GETINFO_BOX' = 'BTN_CLOSE'; 'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
        }
        $null = Invoke-Button host DLG_LOBBY BTN_OK
        $t0 = Get-Date; $onMap = $false
        while ((((Get-Date) - $t0).TotalSeconds) -lt 120) {
            $d = Get-Dialog host
            if ($d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL') { $onMap = $true; break }
            if ($popups.ContainsKey($d)) { $null = Invoke-Button host $d $popups[$d] }   # dismiss a first-turn popup
            elseif ($d -eq 'DLG_LOBBY') { $null = Invoke-Button host DLG_LOBBY BTN_OK }   # re-press start if it lingered
            Start-Sleep -Milliseconds 700
        }
        if (-not $onMap) {
            $outcome = "generated, map not reached (stuck on $(Get-Dialog host))"
            throw $outcome
        }
        $outcome = 'reached map'
        Write-Host "[gen] reached the generated map ($(Get-Dialog host))" -ForegroundColor Green
    }
    $ok = $true
} catch {
    Write-Host "[gen] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    # Enrich a failure with the exact reason now captured in the DLL log (the CRT assert or the sol3
    # panic the harness routes there), so the matrix summary carries the real error text, not just the mode.
    if ($outcome -notin @('generated', 'reached map', 'not-run') -and $client) {
        $log = Join-Path $GameDir "mss32_$($client.Id).log"
        if (Test-Path $log) {
            $err = Get-Content $log -ErrorAction SilentlyContinue |
                Select-String '\[testdrv\]\[(crt-assert|crt-error|stderr)\]' | Select-Object -Last 1
            if ($err) {
                # keep from "[crt-assert]/[stderr]" onward, collapse whitespace, drop the noisy UCRT path
                $d = ($err.Line -replace '^.*?\[testdrv\]\[', '[') -replace '[\r\n\t]+', ' ' -replace 'minkernel\\[^ ]*\\', ''
                $outcome = "$outcome | $d"
            }
        }
    }
    if ($null -ne $genSec) { Write-Output "GEN_SECONDS=$genSec" }
    Write-Output "GEN_OUTCOME=$outcome"   # machine-readable result line the matrix summary parses
    Write-Host "`n==== RESULT: $(if ($ToMap) { 'generate+reach-map' } else { 'generator-form-driven' })=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if ($Keep) {
        Write-Host "[gen] left running (relay pid=$($relay.Id), client pid=$($client.Id))." -ForegroundColor Yellow
    } else {
        if ($client) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
    }
}
if (-not $ok) { Write-Error "scenario-generation form drive failed"; exit 1 }
