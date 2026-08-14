#requires -Version 7.0
<#
.SYNOPSIS
  Single-instance example: drive the random-scenario generator over the relay, optionally all the
  way into the generated map.

.DESCRIPTION
  The reference example for the test toolkit. One host client navigates the multiplayer menu to
  the generator (DLG_RANDOM_SCENARIO_MULTI), reads the live UI snapshot to confirm the form is
  there, then exercises every command type: Set-ListSelection (the template list), Set-EditText
  (the player name), Set-SpinOption (the size spinner) and Invoke-Button (Generate).

  Verification is relay-only: the generator opened, every Step-ToDialog transition required a
  real click, the expected widgets are present, and the form commands left the client alive on
  the dialog. By default the generated map is not awaited; with -WaitGenerationSec the test also
  waits for Generate to reach DLG_GENERATION_RESULT and fails if it errors or never finishes.
  With -ToMap it goes the whole way: accept the result, start the game solo (AI fills the other
  slots), dismiss each known first-turn popup once on its enabled button, and require a connected,
  populated world on the bare strategic map after DLG_BEGIN_TURN with stock strategic action
  admission open. This proves the generated scenario actually loads and becomes playable, not just
  that generation completed. The template matrix runs each template five times; only attempt 5 uses
  -ToMap, while attempts 1-4 stop after generation succeeds.

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
    [switch]$UseTemplateDefaults, # do not force spin indices that may not exist for fixed-size templates
    [string]$RecorderReadyFile,   # CI: wait for the owned OBS process before leaving the main menu
    [switch]$Keep
)

. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir
if ($ToMap -and $WaitGenerationSec -le 0) { $WaitGenerationSec = 300 }   # -ToMap must first reach the result

function Convert-GameCp1251Text([string]$Text) {
    if ([string]::IsNullOrEmpty($Text)) { return $Text }
    $bytes = [Collections.Generic.List[byte]]::new($Text.Length)
    foreach ($ch in $Text.ToCharArray()) {
        $value = [int][char]$ch
        if ($value -gt 255) { return $Text } # already decoded Unicode; do not reinterpret it
        $bytes.Add([byte]$value)
    }
    try { return [Text.Encoding]::GetEncoding(1251).GetString($bytes.ToArray()) } catch { return $Text }
}

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
    if ($RecorderReadyFile) {
        # Deferred HostOnly recording removes the black boot prefix. The wrapper creates an empty
        # ownership file before launching this client; its watcher fills the file only after the
        # first rendered host dialog and a real owned OBS process exists. Hold navigation until then
        # so even an immediate generator failure is captured from before the form opens.
        $recordDeadline = (Get-Date).AddSeconds(30)
        $recorderReady = $false
        while ((Get-Date) -lt $recordDeadline) {
            try {
                $owned = Get-Content -LiteralPath $RecorderReadyFile -Raw -ErrorAction Stop |
                    ConvertFrom-Json -ErrorAction Stop
                $process = Get-Process -Id ([int]$owned.pid) -ErrorAction Stop
                $actual = [IO.Path]::GetFullPath($process.Path)
                $expected = [IO.Path]::GetFullPath([string]$owned.executable)
                if ([string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) {
                    $recorderReady = $true
                    break
                }
            } catch {}
            Start-Sleep -Milliseconds 250
        }
        if (-not $recorderReady) { throw "required OBS recorder did not become ready within 30s" }
        Start-Sleep -Seconds 2 # let --startrecording publish its first rendered frames
        Write-Host "[gen] required recorder ready before generator navigation" -ForegroundColor Green
    }

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
    if ($UseTemplateDefaults) {
        # Selecting the template row synchronously refreshes its size options and leaves the spin at
        # the valid default index 0. Many shipped templates expose exactly one fixed-size option, so
        # blindly writing index 1 is not a faithful user action and can leave the UI out of bounds.
        Write-Host "[gen] keeping template-provided size default" -ForegroundColor Cyan
    } else {
        if (-not (Set-SpinOption host $D SPIN_SIZE 0)) { throw "SPIN_SIZE not set" }
        Start-Sleep 3
    }

    if ((Get-Dialog host) -ne $D) { throw "client left '$D' while driving the form (crash?)" }
    $clientLog = Join-Path $GameDir "mss32_$($client.Id).log"
    $sol3Baseline = if (Test-Path -LiteralPath $clientLog) {
        @(Get-Content -LiteralPath $clientLog -ErrorAction SilentlyContinue |
            Select-String '\[testdrv\]\[stderr\].*\[sol3\]').Count
    } else { 0 }
    if (-not (Invoke-Button host $D BTN_GENERATE)) { throw "BTN_GENERATE not found" }
    Write-Host "[gen] form driven (template=$Template) + BTN_GENERATE clicked" -ForegroundColor Green

    if ($WaitGenerationSec -gt 0) {
        # Assert generation runs to a result. A bad template fails three ways: it errors at once and
        # stays on the form (a captured DebugTest sol3 error on stderr), it runs a while then pops
        # DLG_MESSAGE_BOX (the generator gave up after N attempts), or it never leaves
        # DLG_WAIT_GENERATION. Some large shipped templates take more than a minute, so the matrix uses
        # the same bounded 300s budget as -ToMap; $outcome records which case occurred for its summary.
        Write-Host "[gen] waiting up to ${WaitGenerationSec}s for generation to finish..." -ForegroundColor Cyan
        $t0 = Get-Date; $started = $false; $done = $false; $errbox = $false; $crashed = $false
        $sol3Panic = $null; $disconnectTicks = 0
        while ((((Get-Date) - $t0).TotalSeconds) -lt $WaitGenerationSec) {
            if ($relay.HasExited) { $outcome = 'harness failure: relay exited during generation'; throw $outcome }
            if ($client.HasExited) { $crashed = $true; break }   # a debug assert / fault killed the game
            $state = Get-RoleState host
            if (-not $state -or -not [bool]$state.connected) {
                $disconnectTicks++
                if ($disconnectTicks -ge 5) { $outcome = 'harness failure: client disconnected during generation'; throw $outcome }
                Start-Sleep -Milliseconds 1000
                continue
            }
            $disconnectTicks = 0
            $d = $state.dialog
            if ($d -eq 'DLG_WAIT_GENERATION') { $started = $true }
            if ($d -eq 'DLG_GENERATION_RESULT') { $done = $true; break }
            if ($d -eq 'DLG_MESSAGE_BOX') { $errbox = $true; break }   # generator gave up with an error popup
            # CommandResult acknowledges dispatch before the synchronous Generate callback finishes.
            # A slow callback can legitimately leave the last relay snapshot on the form for several
            # seconds, so elapsed time is not evidence of a panic. Fail fast only on an actual sol3
            # stderr record emitted after the click; other generator diagnostics are non-fatal.
            if (-not $started -and $d -eq $D -and (Test-Path -LiteralPath $clientLog)) {
                $sol3Lines = @(Get-Content -LiteralPath $clientLog -ErrorAction SilentlyContinue |
                    Select-String '\[testdrv\]\[stderr\].*\[sol3\]')
                if ($sol3Lines.Count -gt $sol3Baseline) { $sol3Panic = $sol3Lines[-1]; break }
            }
            Start-Sleep -Milliseconds 1000
        }
        $genSec = [int]((Get-Date) - $t0).TotalSeconds   # seconds spent generating (from BTN_GENERATE), for the summary
        if ($done) { $outcome = 'generated'; Write-Host "[gen] generation finished (DLG_GENERATION_RESULT) in ${genSec}s" -ForegroundColor Green }
        elseif ($crashed) { $outcome = 'crashed (generator assert)'; throw "the game crashed during generation (template $Template; see the captured assert)" }
        elseif ($errbox) {
            $raw = ((((Get-GameUi host).widgets | Where-Object { $_.type -eq 'text' }).state.text) -join ' | ') -replace '[\r\n\t]+', ' '
            $msg = Convert-GameCp1251Text $raw
            $outcome = "error box: $msg"
            throw "generation errored after starting (template $Template; DLG_MESSAGE_BOX: $msg)"
        }
        elseif ($sol3Panic) { $outcome = 'sol3 panic (captured on stderr)'; throw "generation failed on the form (template $Template; captured sol3 stderr)" }
        elseif ($started) { $outcome = "no result in ${WaitGenerationSec}s"; throw "generation did not finish in ${WaitGenerationSec}s (still on DLG_WAIT_GENERATION)" }
        else { $outcome = "no generation start in ${WaitGenerationSec}s"; throw "generation did not start in ${WaitGenerationSec}s (still on $D; no stderr panic captured)" }
    }

    if ($ToMap) {
        # Generation finished (the wait above reached DLG_GENERATION_RESULT or threw). Accept the
        # map into the host lobby, then start solo: empty slots become AI, the game loads, and the
        # first-turn popups give way to the strategic map. This proves the scenario actually plays.
        # Keep generation-only success distinct from a playable-map success: any later throw must
        # remain red in the all-template matrix.
        $outcome = 'harness failure: BTN_ACCEPT was not dispatched'
        if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { throw "BTN_ACCEPT not found" }
        $outcome = 'harness failure: BTN_ACCEPT did not open DLG_LOBBY'
        # BTN_ACCEPT serializes the generated scenario synchronously before the UI can publish the
        # lobby. Large 72x72 templates have exceeded 20s here while still making honest progress.
        # Dispatch once, then wait passively with explicit relay/client health checks; never refire.
        $t0 = Get-Date; $accepted = $false; $disconnectTicks = 0
        while ((((Get-Date) - $t0).TotalSeconds) -lt 120) {
            if ($relay.HasExited) { $outcome = 'harness failure: relay exited while accepting generated map'; throw $outcome }
            if ($client.HasExited) { $outcome = 'harness failure: client exited while accepting generated map'; throw $outcome }
            $state = Get-RoleState host
            if (-not $state -or -not [bool]$state.connected) {
                $disconnectTicks++
                if ($disconnectTicks -ge 5) { $outcome = 'harness failure: client disconnected while accepting generated map'; throw $outcome }
            } else {
                $disconnectTicks = 0
                if ($state.dialog -eq 'DLG_LOBBY') { $accepted = $true; break }
            }
            Start-Sleep -Milliseconds 500
        }
        if (-not $accepted) {
            $outcome = "harness failure: BTN_ACCEPT did not open DLG_LOBBY (on $(Get-Dialog host))"
            throw $outcome
        }
        Write-Host "[gen] map accepted; starting solo host (AI fills the rest)..." -ForegroundColor Cyan

        # First-turn popups, each mapped to its one real forward button (same set the MP test uses).
        $popups = @{
            'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'
            'DLG_GETINFO_BOX' = 'BTN_CLOSE'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
        }
        $outcome = 'generated, playable map startup not reached'
        if (-not (Invoke-Button host DLG_LOBBY BTN_OK)) { throw 'BTN_OK did not start the generated scenario' }

        # The first DLG_ISO_PAL/DLG_STRATEGIC frame can precede a late DLG_BEGIN_TURN and the stock
        # object-lock drain. It is not a pass. Acknowledge only known startup dialogs, only once each,
        # and only when their exact button is enabled. Then require the relay's live UI and world
        # reporters to agree that the client is connected, the turn was observed, the bare strategic
        # map is actionable, and a self player plus scenario objects are populated. No movement or
        # other strategic action is issued by this oracle.
        $claimed = @{}; $world = $null; $state = $null; $selfPlayer = $null
        $t0 = Get-Date; $readyOnMap = $false; $disconnectTicks = 0
        while ((((Get-Date) - $t0).TotalSeconds) -lt 180) {
            if ($relay.HasExited) { $outcome = 'harness failure: relay exited while loading generated map'; throw $outcome }
            if ($client.HasExited) { $outcome = 'harness failure: client exited while loading generated map'; throw $outcome }
            $state = Get-RoleState host
            if (-not $state -or -not [bool]$state.connected) {
                $disconnectTicks++
                if ($disconnectTicks -ge 5) { $outcome = 'harness failure: client disconnected while loading generated map'; throw $outcome }
                Start-Sleep -Milliseconds 700
                continue
            }
            $disconnectTicks = 0
            $d = $state.dialog
            if ($d -eq 'DLG_MESSAGE_BOX') {
                $raw = ((@($state.widgets) | Where-Object { $_.type -eq 'text' } |
                    ForEach-Object { $_.state.text }) -join ' | ') -replace '[\r\n\t]+', ' '
                $msg = Convert-GameCp1251Text $raw
                if ([string]::IsNullOrWhiteSpace($msg)) { $msg = '(no message text reported)' }
                $outcome = "harness failure: message box during generated-map startup: $msg"
                throw $outcome
            }
            if ($d -and $popups.ContainsKey($d) -and -not $claimed.ContainsKey($d)) {
                $button = $popups[$d]
                $readyButton = @($state.widgets) | Where-Object {
                    $_.name -eq $button -and $_.type -eq 'button' -and $_.state.enabled -eq $true
                } | Select-Object -First 1
                if ($readyButton) {
                    # Claim before dispatch: an uncertain response is a failure, never permission to refire.
                    $claimed[$d] = $button
                    if (-not (Invoke-Button host $d $button)) {
                        $outcome = "harness failure: one-shot startup action $d::$button was not accepted"
                        throw $outcome
                    }
                }
            }
            $world = Get-World host
            $selfPlayer = @($world.players) | Where-Object { $_.relation -eq 'self' } | Select-Object -First 1
            $worldPopulated = $world -and $selfPlayer -and @($world.players).Count -gt 0 -and @($world.stacks).Count -gt 0
            $onMap = $d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL'
            if ([bool]$state.connected -and $onMap -and [bool]$state.sawBeginTurn -and
                [bool]$state.strategicActionReady -and [bool]$world.strategicActionReady -and $worldPopulated) {
                $readyOnMap = $true
                break
            }
            Start-Sleep -Milliseconds 500
        }
        if (-not $readyOnMap) {
            $outcome = "generated, playable map startup not reached (dialog=$(Get-Dialog host), connected=$([bool]$state.connected), sawBeginTurn=$([bool]$state.sawBeginTurn), strategicActionReady=$([bool]$state.strategicActionReady), selfPlayer=$([bool]$selfPlayer), stacks=$(@($world.stacks).Count))"
            throw $outcome
        }
        $outcome = 'reached map'
        Write-Host "[gen] reached populated actionable map after first-turn dialogs ($(Get-Dialog host); $(@($world.stacks).Count) stacks)" -ForegroundColor Green
    }
    $ok = $true
} catch {
    if ($outcome -eq 'not-run') { $outcome = "harness failure: $($_.Exception.Message)" }
    Write-Host "[gen] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    # Hold a successful loaded map long enough for the final proof video to show the ready strategic
    # state. Failures keep a shorter hold so their error box/form is visible; a process-ending CRT
    # assert is already present in the preceding frames and the MSS log.
    if ($ok -and $ToMap -and $client -and -not $client.HasExited) { Start-Sleep -Seconds 3 }
    elseif (-not $ok -and $client -and -not $client.HasExited) { Start-Sleep -Seconds 5 }
    # Enrich a failure with the exact reason now captured in the DLL log (the CRT assert or generator
    # stderr the harness routes there), so the matrix summary carries the real error text, not just the mode.
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
