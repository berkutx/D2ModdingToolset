#requires -Version 7.0
<#
.SYNOPSIS
  Drive two game clients (HOST + JOINER) into a started TCP/IP multiplayer game and verify both
  reach the strategic map — the complex, two-instance test.

.DESCRIPTION
  The dispatcher is the brain; the in-DLL clients are thin (report UI, execute invoke/select). Over
  the node relay (tools/relay/relay.js) it reads each client's UI (Get-Dialog/Get-RoleState), drives
  it (Invoke-Button/Set-ListSelection) and coordinates both — no files, no log scraping for state.
  Needs the DebugTest mss32 build in the game folder and Node.js. Windows are tagged [HOST]/[CLIENT].

.EXAMPLE
  .\multiplayer-two-instance.ps1 -Kill
  .\multiplayer-two-instance.ps1 -Kill -EndHostTurn   # also pass the host's turn to the joiner
#>
param(
    [int]$Scenario = 0,
    [switch]$Kill,
    [switch]$EndHostTurn,   # after both reach the map, end the host's turn -> the joiner's begins
    [string]$GameDir,
    [string]$ProcDump = "",
    [string]$DumpDir = ""
)

. "$PSScriptRoot\_relay.ps1"
. "$PSScriptRoot\_show-window.ps1"
. "$PSScriptRoot\_capture.ps1"
$GameDir = Resolve-GameDir $GameDir
if (-not $ProcDump) { $ProcDump = (Get-TestConfig).ProcDump }

# Clean slate without a blanket kill: only our tagged [HOST]/[CLIENT] windows (never a foreign game),
# plus a stale dplaysvr so the DirectPlay session is fresh.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
Get-ChildItem $GameDir -Filter "mss32_*.log" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

if ($DumpDir) { New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null }
$relay = Start-TestRelay -LogDir $(if ($DumpDir) { $DumpDir } else { $env:TEMP })
$relayLog = Join-Path $(if ($DumpDir) { $DumpDir } else { $env:TEMP }) "relay.out.log"
Write-Host "[disp] relay up" -ForegroundColor Green

# ---- test-specific orchestration (built on the toolkit primitives) ------------------------------
# Set a listbox value, then let the agent apply it (one command per UI tick) before the next click.
function SelectSettle([string]$role, [string]$dlg, [string]$lb, [int]$index) {
    Set-ListSelection $role $dlg $lb $index
    Start-Sleep -Milliseconds 1000
}
# Click <btn> on <dlg> until the client LEAVES <dlg> (used for the lobby OK).
function ClickAndLeave([string]$role, [string]$dlg, [string]$btn, [int]$timeoutSec) {
    $t0 = Get-Date; Invoke-Button $role $dlg $btn; $lastFire = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        if ((Get-Dialog $role) -ne $dlg) { return $true }
        if (((Get-Date) - $lastFire).TotalSeconds -ge 12) { Invoke-Button $role $dlg $btn; $lastFire = Get-Date }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
# First-turn popups, each mapped to the button that closes it. DLG_GETINFO_BOX is the "name your lord"
# prompt — BTN_CLOSE accepts the lord's DEFAULT name and closes (do NOT Set-EditText it, which corrupts
# the accept). The reporter reports the REAL topmost dialog, so a close is reflected immediately and the
# same paced loop drives every popup, GETINFO included.
$Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'; 'DLG_GETINFO_BOX' = 'BTN_CLOSE'
    'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
}
# Dismiss first-turn popups until the role reaches the map (relay-latched reachedStrategic). Paced
# (~2.5s/popup): the custom menus tick slowly, so rapid re-clicks just coalesce and waste time.
function DriveToStrategic([string]$role, [int]$timeoutSec) {
    $t0 = Get-Date; $lastDlg = ''; $lastFire = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        $r = Get-RoleState $role
        if ($r -and $r.reachedStrategic) { return $true }
        $d = if ($r) { $r.dialog } else { $null }
        if ($d -and $Dismiss.ContainsKey($d) -and ($d -ne $lastDlg -or ((Get-Date) - $lastFire).TotalSeconds -ge 2.5)) {
            if ($d -ne $lastDlg) { Write-Host "[disp]   $role dialog appeared: $d -> click $($Dismiss[$d])" }
            Invoke-Button $role $d $Dismiss[$d]; $lastDlg = $d; $lastFire = Get-Date
        }
        Start-Sleep -Milliseconds 700
    }
    Write-Host "[disp] $role STUCK at '$(Get-Dialog $role)' (never reached the map)" -ForegroundColor Red
    return $false
}

# ---- the test (dispatcher drives both clients) --------------------------------------------------
function Run-Pairing {
    # Both clients boot in PARALLEL (joiner 10s after the host). Only ordering constraint: the joiner
    # must not search/join a session until the host's lobby exists, so it stages at DLG_LOAD_NEW_MULTI.
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { return $false }
    if (-not (Wait-Dialog join DLG_MAIN_MENU 90)) { return $false }

    # HOST menu -> DLG_LOAD_NEW_MULTI.
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { return $false }
    SelectSettle host DLG_PROTOCOL TLBOX_PROTOCOL 2   # 2 = TCP/IP
    if (-not (Step-ToDialog host DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { return $false }

    # JOINER menu -> DLG_LOAD_NEW_MULTI, then STAGE (no session to join until the host makes one).
    if (-not (Step-ToDialog join DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { return $false }
    SelectSettle join DLG_PROTOCOL TLBOX_PROTOCOL 2
    if (-not (Step-ToDialog join DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { return $false }
    Write-Host "[disp] JOINER staged at DLG_LOAD_NEW_MULTI (waiting for the host's session)" -ForegroundColor DarkGray

    # HOST: DLG_LOAD_NEW_MULTI -> skirmish -> DLG_LOBBY (creates the session). DLG_CHOOSE_SKIRMISH is
    # co-present inside DLG_HOST (not the "current" dialog), so drive by name until the lobby.
    $t0 = Get-Date; $hostLobby = $false; $lastFire = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt 45) {
        $d = Get-Dialog host
        if ($d -eq "DLG_LOBBY") { $hostLobby = $true; break }
        if (((Get-Date) - $lastFire).TotalSeconds -ge 4) {
            if ($d -eq "DLG_LOAD_NEW_MULTI") { Invoke-Button host DLG_LOAD_NEW_MULTI BTN_HOST }
            else { Set-ListSelection host DLG_CHOOSE_SKIRMISH TLBOX_GAME_SLOT $Scenario; Start-Sleep -Milliseconds 400; Invoke-Button host DLG_CHOOSE_SKIRMISH BTN_LOAD }
            $lastFire = Get-Date
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $hostLobby) { Write-Host "[disp] host STUCK at '$(Get-Dialog host)' (wanted DLG_LOBBY)" -ForegroundColor Red; return $false }
    Write-Host "[disp] HOST in lobby (session created)" -ForegroundColor Green

    # GATE released: the host's session exists, so the staged joiner searches + joins it.
    if (-not (Step-ToDialog join DLG_LOAD_NEW_MULTI BTN_JOIN DLG_SESSION)) { return $false }
    Start-Sleep -Seconds 3   # session enumeration settle
    if (-not (Step-ToDialog join DLG_SESSION BTN_JOIN_GAME DLG_LOBBY)) { return $false }
    Write-Host "[disp] JOINER in lobby" -ForegroundColor Green

    # Host starts and reaches the map first; only then the joiner requests the snapshot. Success
    # latches at DLG_ISO_PAL — the map view BEFORE the first-turn popups, which we never touch
    # (dismissing them mid-begin-turn hangs reconciliation; the goal is reaching the map, not turn 1).
    Start-Sleep -Milliseconds 1500
    if (-not (ClickAndLeave host DLG_LOBBY BTN_OK 45)) { return $false }
    if (-not (DriveToStrategic host 180)) { return $false }   # dismiss briefing -> reach the map (slow on software Mesa)
    Write-Host "[disp] HOST reached the strategic map; requesting JOINER" -ForegroundColor Green
    if (-not (ClickAndLeave join DLG_LOBBY BTN_OK 45)) { return $false }
    if (-not (DriveToStrategic join 180)) { return $false }
    Write-Host "[disp] JOINER reached strategic map" -ForegroundColor Green

    if ($EndHostTurn) {
        # Sequential turn-pass, closing every dialog LEGITIMATELY (a role must not act while a dialog is
        # open). Both players close their first-turn popups on the real button ($Dismiss): scenario
        # briefing, the new-day DLG_BEGIN_TURN income, and the "name your lord" DLG_GETINFO_BOX (BTN_CLOSE
        # accepts the default name). Only when the HOST sits on the bare map does it press BTN_END_TURN ->
        # the turn passes and the joiner gets its OWN new day (relay-latched join.sawBeginTurn).
        Write-Host "[disp] HOST skips its turn -> close first-turn dialogs (BOTH roles), then end host turn" -ForegroundColor Cyan
        $BareMap = 'DLG_STRATEGIC', 'DLG_ISO_PAL'   # host ends the turn ONLY from here, all overlays closed
        $seen = @{ host = ''; join = '' }
        $last = @{ host = (Get-Date).AddSeconds(-10); join = (Get-Date).AddSeconds(-10) }
        $endLogged = $false; $t0 = Get-Date
        while ((((Get-Date) - $t0).TotalSeconds) -lt 150) {
            $s = Get-RelayState
            if ($s.join.sawBeginTurn) { break }   # the joiner reached its OWN new day -> turn passed
            foreach ($role in 'host', 'join') {
                $d = if ($s.$role) { $s.$role.dialog } else { $null }
                if (-not $d -or ((Get-Date) - $last[$role]).TotalSeconds -lt 2.0) { continue }   # pace ~2s/click
                if ($Dismiss.ContainsKey($d)) {
                    if ($seen[$role] -ne $d) { Write-Host "[disp]   $role dialog appeared: $d -> click $($Dismiss[$d])"; $seen[$role] = $d }
                    Invoke-Button $role $d $Dismiss[$d]; $last[$role] = Get-Date
                } elseif ($role -eq 'host' -and $s.host.sawBeginTurn -and ($BareMap -contains $d)) {
                    if (-not $endLogged) {
                        Write-Host "[disp]   host dialogs closed, on bare map ($d) -> end turn (BTN_END_TURN)"
                        if ($DumpDir) { CaptureWindow $h "host_pre_endturn" }
                        $endLogged = $true
                    }
                    Invoke-Button host DLG_STRATEGIC BTN_END_TURN; $last['host'] = Get-Date
                }
            }
            Start-Sleep -Milliseconds 600
        }
        $s = Get-RelayState
        Write-Host ("[disp] end-turn: host.sawBeginTurn={0} join.sawBeginTurn={1} | host at '{2}', join at '{3}'" -f $s.host.sawBeginTurn, $s.join.sawBeginTurn, (Get-Dialog host), (Get-Dialog join)) -ForegroundColor Cyan
        if (-not $s.join.sawBeginTurn) { Write-Host "[disp] JOINER never reached its new day (turn did not fully pass)" -ForegroundColor Red; if ($DumpDir) { CaptureWindow $h "host"; if ($j) { CaptureWindow $j "join" } }; return $false }
        if (-not $s.join.reachedStrategic) { Write-Host "[disp] JOINER lost the strategic map" -ForegroundColor Red; return $false }
        if (-not ($s.host.connected -and $s.join.connected)) { Write-Host "[disp] a client dropped (crash?)" -ForegroundColor Red; return $false }

        # The turn is the joiner's now -> close ITS first-turn dialogs the same way until it sits on its map.
        $t1 = Get-Date; $jlast = (Get-Date).AddSeconds(-10); $jseen = ''
        while ((((Get-Date) - $t1).TotalSeconds) -lt 60) {
            $jd = Get-Dialog join
            if ($jd -eq 'DLG_ISO_PAL' -or $jd -eq 'DLG_STRATEGIC') { break }   # overlays closed -> its own map
            if (((Get-Date) - $jlast).TotalSeconds -ge 2.0) {
                if ($Dismiss.ContainsKey($jd)) {
                    if ($jseen -ne $jd) { Write-Host "[disp]   join dialog appeared: $jd -> click $($Dismiss[$jd])"; $jseen = $jd }
                    Invoke-Button join $jd $Dismiss[$jd]
                }
                $jlast = Get-Date
            }
            Start-Sleep -Milliseconds 500
        }
        Write-Host "[disp] turn passed: host closed its dialogs + ended turn; JOINER on its actionable turn" -ForegroundColor Green
        if ($DumpDir) {
            Start-Sleep -Milliseconds 1800   # let the joiner's map render settle before grabbing it
            CaptureWindow $h "host"; if ($j) { CaptureWindow $j "join_its_turn" }
        }
    }
    return $true
}

# ---- diagnostics on failure ---------------------------------------------------------------------
function SnapshotInstance([System.Diagnostics.Process]$Proc, [string]$Role) {
    if (-not $ProcDump -or -not $DumpDir -or -not $Proc -or $Proc.HasExited) { return }
    New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null
    $out = Join-Path $DumpDir "$Role`_$($Proc.Id).dmp"
    Write-Host "[disp] procdump -ma $Role pid=$($Proc.Id) -> $out"
    & $ProcDump -accepteula -ma $Proc.Id $out 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Host "[disp][procdump] $_" }
}
function CaptureWindow([System.Diagnostics.Process]$Proc, [string]$Role) {
    if (-not $DumpDir -or -not $Proc -or $Proc.HasExited) { return }
    New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null
    try { Show-GameWindow -Proc $Proc } catch {}   # un-occlude so the GL wrapper repaints before grabbing
    Start-Sleep -Milliseconds 1000
    $png = Join-Path $DumpDir "$Role`_screen.png"
    try {
        if (Capture-GameWindow -Proc $Proc -Path $png) { Write-Host "[disp] $Role screenshot -> $png" }
        else { Write-Host "[disp] $Role screenshot skipped (no window)" }
    } catch { Write-Host "[disp] $Role screenshot failed: $($_.Exception.Message)" }
}

# ---- run ----------------------------------------------------------------------------------------
# try/finally so the relay (and games, under -Kill) are ALWAYS cleaned up — a throw must not orphan
# the relay, whose named pipe would block the next run.
$h = $null; $j = $null; $ok = $false
try {
    Write-Host "[disp] launching HOST..." -ForegroundColor Cyan
    $h = Start-GameClient -GameDir $GameDir -Role host; $hostLog = "$GameDir\mss32_$($h.Id).log"
    Write-Host "[disp] host pid=$($h.Id)"
    Start-Sleep -Seconds 10   # joiner starts 10s later -> parallel boot; Run-Pairing gates its join
    Write-Host "[disp] launching JOINER (10s after host)..." -ForegroundColor Cyan
    $j = Start-GameClient -GameDir $GameDir -Role join; $joinLog = "$GameDir\mss32_$($j.Id).log"
    Write-Host "[disp] join pid=$($j.Id)"

    $ok = Run-Pairing

    Write-Host ""
    Write-Host "==== RESULT: both-reached-strategic=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    Write-Host "host log: $hostLog"; if ($j) { Write-Host "join log: $joinLog" }

    if (-not $ok) {
        Write-Host "[disp] --- relay /api/state ---"
        try { Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 3 | ConvertTo-Json -Depth 6 | Write-Host } catch {}
        Write-Host "[disp] --- relay stdout (last 60) ---"
        if (Test-Path $relayLog) { Get-Content $relayLog -Tail 60 | ForEach-Object { Write-Host "[relay] $_" } }
        CaptureWindow $h "host"; if ($j) { CaptureWindow $j "join" }
        SnapshotInstance $h "host"; if ($j) { SnapshotInstance $j "join" }
    }
} catch {
    Write-Host "[disp] run aborted by error: $($_.Exception.Message)" -ForegroundColor Red
    $ok = $false
} finally {
    if ($Kill) {
        if ($h) { Stop-Process -Id $h.Id -Force -ErrorAction SilentlyContinue }
        if ($j) { Stop-Process -Id $j.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
        Write-Host "[disp] clients + relay closed."
    } else {
        if ($h) { Show-GameWindow -Proc $h }; if ($j) { Show-GameWindow -Proc $j }
        if ($relay) { Write-Host "[disp] left running (relay pid=$($relay.Id))." -ForegroundColor Yellow }
    }
}

if (-not $ok) { Write-Error "two-instance MP did not reach the strategic map"; exit 1 }
