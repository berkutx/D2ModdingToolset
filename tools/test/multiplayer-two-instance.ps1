#requires -Version 7.0
<#
.SYNOPSIS
  Drive two game instances (HOST + JOINER) into a started TCP/IP multiplayer game and
  verify both reach the strategic map — the COMPLEX, two-instance test.

.DESCRIPTION
  The dispatcher is the brain; the in-DLL agents are thin (report UI, execute invoke/select).
  Over the node relay (tools/relay/relay.js) it scans each agent's UI (/api/state), drives it
  (/api/invoke|/api/select) and coordinates both instances — no files, no log scraping for state.
  Needs the DebugTest mss32 build in -Game and Node.js. Windows are tagged [HOST] / [CLIENT].

.EXAMPLE
  .\multiplayer-two-instance.ps1 -Kill
#>
param(
    [int]$Scenario = 0,
    [switch]$Kill,
    [switch]$EndHostTurn,   # after both reach the map, end the host's turn -> the joiner's begins
    [string]$Game = "C:\GOG Games\slasher_mns_2_4",
    [string]$RelayJs = "$PSScriptRoot\..\relay\relay.js",
    [string]$RelayBase = "http://127.0.0.1:8077",
    [string]$ProcDump = "",
    [string]$DumpDir = ""
)

. "$PSScriptRoot\_show-window.ps1"
. "$PSScriptRoot\_capture.ps1"
$exe = "$Game\Discipl2.exe"

# Clean slate without a blanket kill: only our tagged [HOST]/[CLIENT] windows (never a foreign
# game), plus a stale dplaysvr so the DirectPlay session is fresh.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
Get-ChildItem $Game -Filter "mss32_*.log" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

# ---- relay -----------------------------------------------------------------
if ($DumpDir) { New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null }
$relayLog = if ($DumpDir) { Join-Path $DumpDir "relay.out.log" } else { Join-Path ([System.IO.Path]::GetTempPath()) "d2relay.out.log" }
Write-Host "[disp] starting relay: node $RelayJs" -ForegroundColor Cyan
$relay = Start-Process -FilePath "node" -ArgumentList "`"$RelayJs`"" -PassThru `
    -RedirectStandardOutput $relayLog -RedirectStandardError "$relayLog.err" -WindowStyle Hidden
$relayUp = $false
for ($i = 0; $i -lt 25; $i++) {
    try { Invoke-RestMethod "$RelayBase/api/status" -TimeoutSec 2 | Out-Null; $relayUp = $true; break } catch { Start-Sleep -Milliseconds 300 }
}
if (-not $relayUp) { Write-Error "[disp] relay did not come up on $RelayBase"; if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }; exit 1 }
Write-Host "[disp] relay up on $RelayBase" -ForegroundColor Green

# How often StepTo/ClickAndLeave re-nudge a step that hasn't progressed. Pure liveness: the
# agent's in-flight coalescing (autonav.cpp) owns "don't double-act", so this value is free —
# 12s just avoids needless re-fires during the ~10s DPlay stall.
$RefireSec = 12

# ---- relay helpers (the dispatcher's eyes + hands) -------------------------
function State { try { (Invoke-RestMethod "$RelayBase/api/state" -TimeoutSec 3).roles } catch { $null } }
function Dlg([string]$role) { $s = State; if ($s -and $s.$role) { return $s.$role.dialog } else { return $null } }
function InvokeBtn([string]$role, [string]$dlg, [string]$btn) {
    try { Invoke-RestMethod "$RelayBase/api/invoke?role=$role&dlg=$dlg&btn=$btn" -Method POST -TimeoutSec 3 | Out-Null } catch {}
}
function SetSel([string]$role, [string]$dlg, [string]$lb, [int]$index) {
    try { Invoke-RestMethod "$RelayBase/api/select?role=$role&dlg=$dlg&lb=$lb&index=$index" -Method POST -TimeoutSec 3 | Out-Null } catch {}
    Write-Host "[disp] $role select $dlg::$lb = $index"
}
function WaitDlg([string]$role, [string]$dialog, [int]$timeoutSec) {
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        if ((Dlg $role) -eq $dialog) { return $true }
        Start-Sleep -Milliseconds 400
    }
    return $false
}
# Click <btn> on <srcDlg> until <expect>. The agent resolves the dialog by name (works for a
# co-present or already-closed one), and we retry since the button may not be bound on first ask.
function StepTo([string]$role, [string]$srcDlg, [string]$btn, [string]$expect, [int]$timeoutSec) {
    $t0 = Get-Date
    InvokeBtn $role $srcDlg $btn
    $lastFire = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        if ((Dlg $role) -eq $expect) { Write-Host "[disp] $role $srcDlg::$btn -> $expect" -ForegroundColor Green; return $true }
        if (((Get-Date) - $lastFire).TotalSeconds -ge $RefireSec) { InvokeBtn $role $srcDlg $btn; $lastFire = Get-Date }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "[disp] $role STUCK at '$(Dlg $role)' (wanted $expect after $srcDlg::$btn)" -ForegroundColor Red
    return $false
}
# Set a listbox value, then let the agent apply it (one command per UI tick) before the next click.
function SelectSettle([string]$role, [string]$dlg, [string]$lb, [int]$index) {
    SetSel $role $dlg $lb $index
    Start-Sleep -Milliseconds 1000
}
# Click <btn> on <dlg> until the agent LEAVES <dlg> (used for the lobby OK).
function ClickAndLeave([string]$role, [string]$dlg, [string]$btn, [int]$timeoutSec) {
    $t0 = Get-Date
    InvokeBtn $role $dlg $btn
    $lastFire = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        if ((Dlg $role) -ne $dlg) { return $true }
        if (((Get-Date) - $lastFire).TotalSeconds -ge $RefireSec) { InvokeBtn $role $dlg $btn; $lastFire = Get-Date }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
# First-turn popups that dismiss on a click, mapped to their real button. NOTE: DLG_GETINFO_BOX is
# NOT here — it's a dead-end overlay whose only button (BTN_CLOSE) does NOT dismiss it; you bypass it
# by acting on the co-present dialog underneath (see -EndHostTurn, which ends the turn through it).
$Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'
    'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
}
# Dismiss first-turn popups until the role reaches the map (relay-latched reachedStrategic). Paced
# (~2.5s/popup): the custom menus tick slowly, so rapid re-clicks just coalesce and waste time.
function DriveToStrategic([string]$role, [int]$timeoutSec) {
    $t0 = Get-Date; $lastDlg = ''; $lastFire = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        $s = State; $r = if ($s) { $s.$role } else { $null }
        if ($r -and $r.reachedStrategic) { return $true }
        $d = if ($r) { $r.dialog } else { $null }
        if ($d -and $Dismiss.ContainsKey($d) -and ($d -ne $lastDlg -or ((Get-Date) - $lastFire).TotalSeconds -ge 2.5)) {
            if ($d -ne $lastDlg) { Write-Host "[disp]   $role dialog appeared: $d -> click $($Dismiss[$d])" }
            InvokeBtn $role $d $Dismiss[$d]; $lastDlg = $d; $lastFire = Get-Date
        }
        Start-Sleep -Milliseconds 700
    }
    Write-Host "[disp] $role STUCK at '$(Dlg $role)' (never reached DLG_STRATEGIC)" -ForegroundColor Red
    return $false
}
function Launch([string]$Role) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.WorkingDirectory = $Game; $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["D2TESTDRV_SKIP_INTRO"] = "1"
    $psi.EnvironmentVariables["D2TESTDRV_BLACKSCREEN_FIX"] = "1"
    $psi.EnvironmentVariables["D2TESTDRV_UI_REPORTER"] = "1"
    $psi.EnvironmentVariables["D2TESTDRV_ROLE"] = $Role
    $psi.EnvironmentVariables["D2TESTDRV_RELAY_BRIDGE"] = "1"   # dispatcher-driven (no SELFNAV)
    # NET_INTERCEPT (packet-trace forwarding) stays off — it runs on the UI thread per packet.
    return [System.Diagnostics.Process]::Start($psi)
}

# ---- the test (dispatcher drives both instances) --------------------------
function Run-Pairing {
    # Both instances boot in PARALLEL (joiner launched 10s after the host). Only ordering
    # constraint: the joiner must not search/join a TCP/IP session until the host's lobby exists
    # (else EnumSessions finds nothing), so it stages at DLG_LOAD_NEW_MULTI until then.
    if (-not (WaitDlg "host" "DLG_MAIN_MENU" 90)) { return $false }
    if (-not (WaitDlg "join" "DLG_MAIN_MENU" 90)) { return $false }

    # HOST menu -> DLG_LOAD_NEW_MULTI.
    if (-not (StepTo "host" "DLG_MAIN_MENU" "BTN_MULTI" "DLG_PROTOCOL" 45)) { return $false }
    SelectSettle "host" "DLG_PROTOCOL" "TLBOX_PROTOCOL" 2  # 2 = TCP/IP
    if (-not (StepTo "host" "DLG_PROTOCOL" "BTN_CONTINUE" "DLG_LOAD_NEW_MULTI" 45)) { return $false }

    # JOINER menu -> DLG_LOAD_NEW_MULTI, then STAGE (no BTN_JOIN yet — no session until the host makes one).
    if (-not (StepTo "join" "DLG_MAIN_MENU" "BTN_MULTI" "DLG_PROTOCOL" 45)) { return $false }
    SelectSettle "join" "DLG_PROTOCOL" "TLBOX_PROTOCOL" 2
    if (-not (StepTo "join" "DLG_PROTOCOL" "BTN_CONTINUE" "DLG_LOAD_NEW_MULTI" 45)) { return $false }
    Write-Host "[disp] JOINER staged at DLG_LOAD_NEW_MULTI (waiting for the host's session)" -ForegroundColor DarkGray

    # HOST: DLG_LOAD_NEW_MULTI -> skirmish -> DLG_LOBBY (creates the session). DLG_CHOOSE_SKIRMISH
    # is co-present inside DLG_HOST (not the "current" dialog), so drive by name until the lobby.
    $t0 = Get-Date; $hostLobby = $false; $lastFire = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt 45) {
        $d = Dlg "host"
        if ($d -eq "DLG_LOBBY") { $hostLobby = $true; break }
        if (((Get-Date) - $lastFire).TotalSeconds -ge 4) {
            if ($d -eq "DLG_LOAD_NEW_MULTI") { InvokeBtn "host" "DLG_LOAD_NEW_MULTI" "BTN_HOST" }
            else { SetSel "host" "DLG_CHOOSE_SKIRMISH" "TLBOX_GAME_SLOT" $Scenario; Start-Sleep -Milliseconds 400; InvokeBtn "host" "DLG_CHOOSE_SKIRMISH" "BTN_LOAD" }
            $lastFire = Get-Date
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $hostLobby) { Write-Host "[disp] host STUCK at '$(Dlg "host")' (wanted DLG_LOBBY)" -ForegroundColor Red; return $false }
    Write-Host "[disp] HOST in lobby (session created)" -ForegroundColor Green

    # GATE released: the host's session now exists, so the staged joiner searches + joins it.
    if (-not (StepTo "join" "DLG_LOAD_NEW_MULTI" "BTN_JOIN" "DLG_SESSION" 45)) { return $false }
    Start-Sleep -Seconds 3 # session enumeration settle
    if (-not (StepTo "join" "DLG_SESSION" "BTN_JOIN_GAME" "DLG_LOBBY" 45)) { return $false }
    Write-Host "[disp] JOINER in lobby" -ForegroundColor Green

    # Host starts and reaches the map first; only then the joiner requests the snapshot. Success
    # latches at DLG_ISO_PAL — the map view BEFORE the first-turn popups, which we never touch
    # (dismissing them mid-begin-turn hangs reconciliation; the goal is reaching the map, not turn 1).
    Start-Sleep -Milliseconds 1500
    if (-not (ClickAndLeave "host" "DLG_LOBBY" "BTN_OK" 45)) { return $false }
    if (-not (DriveToStrategic "host" 180)) { return $false }   # dismiss briefing -> reach the map (slow on software Mesa)
    Write-Host "[disp] HOST reached the strategic map; requesting JOINER" -ForegroundColor Green
    if (-not (ClickAndLeave "join" "DLG_LOBBY" "BTN_OK" 45)) { return $false }
    if (-not (DriveToStrategic "join" 180)) { return $false }   # dismiss briefing -> reach the map (slow on software Mesa)
    Write-Host "[disp] JOINER reached strategic map" -ForegroundColor Green

    if ($EndHostTurn) {
        # First-turn popups for both players are auto-clicked on their real button ($Dismiss), each
        # appearance + click logged. The host's begin-turn (BTN_OK) drops it onto the dead-end
        # DLG_GETINFO_BOX (its only button, BTN_CLOSE, does NOT dismiss it) — so once the host has seen
        # its new day, end the turn with BTN_END_TURN on DLG_STRATEGIC, which resolves BY NAME and fires
        # straight through the GETINFO overlay (or from the bare map). That passes the turn: relay.js
        # latches the JOINER's OWN new-day DLG_BEGIN_TURN as join.sawBeginTurn. Success = the joiner
        # reached its own new day, still holds the map, and neither instance crashed.
        Write-Host "[disp] HOST skips its turn -> clear first-turn popups (BOTH roles), end host turn" -ForegroundColor Cyan
        $HostEndFrom = 'DLG_GETINFO_BOX', 'DLG_STRATEGIC', 'DLG_ISO_PAL'   # host: new day seen -> end turn from here
        $seen = @{ host = ''; join = '' }
        $last = @{ host = (Get-Date).AddSeconds(-10); join = (Get-Date).AddSeconds(-10) }
        $endLogged = $false; $t0 = Get-Date
        while ((((Get-Date) - $t0).TotalSeconds) -lt 150) {
            $s = State
            if ($s.join.sawBeginTurn) { break }   # the joiner reached its OWN new day -> turn passed
            foreach ($role in 'host', 'join') {
                $d = if ($s.$role) { $s.$role.dialog } else { $null }
                if (-not $d -or ((Get-Date) - $last[$role]).TotalSeconds -lt 2.0) { continue }   # pace ~2s/click
                if ($role -eq 'host' -and $s.host.sawBeginTurn -and ($HostEndFrom -contains $d)) {
                    if (-not $endLogged) { Write-Host "[disp]   host new day cleared -> end turn (DLG_STRATEGIC::BTN_END_TURN, through $d)"; $endLogged = $true }
                    InvokeBtn 'host' 'DLG_STRATEGIC' 'BTN_END_TURN'; $last['host'] = Get-Date
                } elseif ($Dismiss.ContainsKey($d)) {
                    if ($seen[$role] -ne $d) { Write-Host "[disp]   $role dialog appeared: $d -> click $($Dismiss[$d])"; $seen[$role] = $d }
                    InvokeBtn $role $d $Dismiss[$d]; $last[$role] = Get-Date
                }
            }
            Start-Sleep -Milliseconds 600
        }
        $s = State
        Write-Host ("[disp] end-turn: host.sawBeginTurn={0} join.sawBeginTurn={1} | host at '{2}', join at '{3}'" -f $s.host.sawBeginTurn, $s.join.sawBeginTurn, (Dlg 'host'), (Dlg 'join')) -ForegroundColor Cyan
        if ($DumpDir) { CaptureWindow $h "host"; if ($j) { CaptureWindow $j "join" } }
        if (-not $s.join.sawBeginTurn) { Write-Host "[disp] JOINER never reached its new day (turn did not fully pass)" -ForegroundColor Red; return $false }
        if (-not $s.join.reachedStrategic) { Write-Host "[disp] JOINER lost the strategic map" -ForegroundColor Red; return $false }
        if (-not ($s.host.connected -and $s.join.connected)) { Write-Host "[disp] an instance dropped (crash?)" -ForegroundColor Red; return $false }
        Write-Host "[disp] turn passed: host cleared its new day + ended turn; JOINER reached + dismissed its OWN new day, still on the map" -ForegroundColor Green
    }
    return $true
}

# ---- diagnostics on failure -----------------------------------------------
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

# ---- run -------------------------------------------------------------------
# try/finally so the relay (and games, under -Kill) are ALWAYS cleaned up — a throw must not
# orphan the relay, whose named pipe would block the next run.
$h = $null; $j = $null; $ok = $false
try {
    Write-Host "[disp] launching HOST..." -ForegroundColor Cyan
    $h = Launch "host"; $hostLog = "$Game\mss32_$($h.Id).log"
    Write-Host "[disp] host pid=$($h.Id)"
    Start-Sleep -Seconds 10  # joiner starts 10s later -> parallel boot; Run-Pairing gates its join
    Write-Host "[disp] launching JOINER (10s after host)..." -ForegroundColor Cyan
    $j = Launch "join"; $joinLog = "$Game\mss32_$($j.Id).log"
    Write-Host "[disp] join pid=$($j.Id)"

    $ok = Run-Pairing

    Write-Host ""
    Write-Host "==== RESULT: both-reached-strategic=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    Write-Host "host log: $hostLog"; if ($j) { Write-Host "join log: $joinLog" }

    if (-not $ok) {
        Write-Host "[disp] --- relay /api/state ---"
        try { Invoke-RestMethod "$RelayBase/api/state" -TimeoutSec 3 | ConvertTo-Json -Depth 6 | Write-Host } catch {}
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
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue  # clear the shared DPlay helper too
        Write-Host "[disp] instances + relay closed."
    } else {
        if ($h) { Show-GameWindow -Proc $h }; if ($j) { Show-GameWindow -Proc $j }
        if ($relay) { Write-Host "[disp] left running (relay pid=$($relay.Id))." -ForegroundColor Yellow }
    }
}

if (-not $ok) { Write-Error "two-instance MP did not reach the strategic map"; exit 1 }
