#requires -Version 7.0
<#
.SYNOPSIS
  Drive two game instances (HOST + JOINER) into a started TCP/IP multiplayer game and
  verify both reach the strategic map — the COMPLEX, two-instance test.

.DESCRIPTION
  The PowerShell dispatcher is the brain. The in-DLL agents are thin: they report the live
  UI (dialog + buttons) and execute invoke/select commands. The dispatcher, over the node
  relay (tools/relay/relay.js), SCANS each agent's UI (/api/state), DRIVES it
  (POST /api/invoke|/api/select), VERIFIES dialogs and COORDINATES the two instances —
  with NO files on disk and NO log scraping for state. Per-PID logs mss32_<pid>.log are
  kept for human debugging only. Windows are tagged [HOST] / [CLIENT]. Requires the
  DebugTest mss32 build in -Game and Node.js for the relay.

.EXAMPLE
  .\multiplayer-two-instance.ps1 -Kill
#>
param(
    [int]$Scenario = 0,
    [switch]$Kill,
    [string]$Game = "C:\GOG Games\slasher_mns_2_4",
    [string]$RelayJs = "$PSScriptRoot\..\relay\relay.js",
    [string]$RelayBase = "http://127.0.0.1:8077",
    [string]$ProcDump = "",
    [string]$DumpDir = ""
)

. "$PSScriptRoot\_show-window.ps1"
$exe = "$Game\Discipl2.exe"

# Clean slate WITHOUT a blanket kill: only our own test instances are tagged [HOST]/[CLIENT],
# so target those — never a manually launched game (e.g. another agent's). dplaysvr is the
# shared DirectPlay helper; drop a stale one so the session is fresh.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
Get-ChildItem $Game -Filter "mss32_*.log" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

# ---- relay -----------------------------------------------------------------
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

# Re-fire cadence for StepTo / ClickAndLeave. Kept ABOVE the longest synchronous UI-thread
# stall (a DPlay EnumSessions/JoinSession blocks the joiner's UI thread ~10s): the DLL agent
# also coalesces an identical in-flight command, but re-firing only after the stall window
# avoids issuing spurious duplicate clicks in the first place.
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
# Click <btn> on <srcDlg> until the agent reaches <expect>. The invoke targets the dialog
# BY NAME, so the agent resolves it through its registry even when it is a co-present
# (non-current) dialog; a click on an already-closed dialog is a harmless no-op. We retry
# because the button may not be bound the instant we first ask.
function StepTo([string]$role, [string]$srcDlg, [string]$btn, [string]$expect, [int]$timeoutSec) {
    $t0 = Get-Date
    InvokeBtn $role $srcDlg $btn          # fire once up front
    $lastFire = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        if ((Dlg $role) -eq $expect) { Write-Host "[disp] $role $srcDlg::$btn -> $expect" -ForegroundColor Green; return $true }
        # Re-fire only if clearly stuck. A transition (esp. the network join) can take a few
        # seconds; re-clicking during it double-acts (e.g. a second join -> error popup).
        if (((Get-Date) - $lastFire).TotalSeconds -ge $RefireSec) { InvokeBtn $role $srcDlg $btn; $lastFire = Get-Date }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "[disp] $role STUCK at '$(Dlg $role)' (wanted $expect after $srcDlg::$btn)" -ForegroundColor Red
    return $false
}
# Select a listbox value and give the agent a moment to apply it (commands execute one
# per UI-frame tick) before the dependent button is pressed.
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
# Dismiss first-turn popups until the agent is genuinely in DLG_STRATEGIC (scenario loaded).
$Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'
    'DLG_GETINFO_BOX' = 'BTN_CLOSE'; 'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
}
# Dismiss first-turn popups until the role has REACHED the strategic map (relay-latched).
# Popups are paced: don't re-click the same dialog faster than ~2.5s — clicking first-turn
# popups back-to-back hangs the begin-turn reconciliation (the engine needs a beat).
function DriveToStrategic([string]$role, [int]$timeoutSec) {
    $t0 = Get-Date; $lastDlg = ''; $lastFire = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        $s = State; $r = if ($s) { $s.$role } else { $null }
        if ($r -and $r.reachedStrategic) { return $true }
        $d = if ($r) { $r.dialog } else { $null }
        if ($d -and $Dismiss.ContainsKey($d) -and ($d -ne $lastDlg -or ((Get-Date) - $lastFire).TotalSeconds -ge 2.5)) {
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
    # Popups are dismissed dispatcher-side, PACED: clearing them back-to-back in-tick hangs
    # the begin-turn reconciliation (the engine needs a beat between first-turn popups).
    # NET_INTERCEPT (packet-trace forwarding) stays OFF: it runs on the UI thread per packet.
    return [System.Diagnostics.Process]::Start($psi)
}

# ---- the test (dispatcher drives both instances) --------------------------
function Run-Pairing {
    # HOST: menu -> Multiplayer -> TCP/IP -> Host -> scenario -> lobby (creates the session).
    if (-not (WaitDlg "host" "DLG_MAIN_MENU" 90)) { return $false }
    if (-not (StepTo "host" "DLG_MAIN_MENU" "BTN_MULTI" "DLG_PROTOCOL" 45)) { return $false }
    SelectSettle "host" "DLG_PROTOCOL" "TLBOX_PROTOCOL" 2  # 2 = TCP/IP
    if (-not (StepTo "host" "DLG_PROTOCOL" "BTN_CONTINUE" "DLG_LOAD_NEW_MULTI" 45)) { return $false }
    # BTN_HOST opens the skirmish setup (DLG_CHOOSE_SKIRMISH co-present inside DLG_HOST, so the
    # "current" dialog there is not DLG_CHOOSE_SKIRMISH); drive by name until we reach the lobby.
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

    # JOINER: launch now that the session exists; menu -> Multiplayer -> TCP/IP -> Join -> lobby.
    $script:j = Launch "join"; $script:joinLog = "$Game\mss32_$($script:j.Id).log"
    Write-Host "[disp] launched JOINER pid=$($script:j.Id)"
    if (-not (WaitDlg "join" "DLG_MAIN_MENU" 90)) { return $false }
    if (-not (StepTo "join" "DLG_MAIN_MENU" "BTN_MULTI" "DLG_PROTOCOL" 45)) { return $false }
    SelectSettle "join" "DLG_PROTOCOL" "TLBOX_PROTOCOL" 2
    if (-not (StepTo "join" "DLG_PROTOCOL" "BTN_CONTINUE" "DLG_LOAD_NEW_MULTI" 45)) { return $false }
    if (-not (StepTo "join" "DLG_LOAD_NEW_MULTI" "BTN_JOIN" "DLG_SESSION" 45)) { return $false }
    Start-Sleep -Seconds 3 # session enumeration settle
    if (-not (StepTo "join" "DLG_SESSION" "BTN_JOIN_GAME" "DLG_LOBBY" 45)) { return $false }
    Write-Host "[disp] JOINER in lobby" -ForegroundColor Green

    # COORDINATE: let the joiner's lobby handshake settle, then the host starts and reaches the
    # map (DLG_ISO_PAL / DLG_STRATEGIC); only then the joiner requests the snapshot and reaches
    # the map too. "Reached the map" is latched the instant DLG_ISO_PAL appears — BEFORE the
    # first-turn event popups, which we deliberately never touch (dismissing them mid-begin-turn
    # hangs the engine's display reconciliation; the test goal is reaching the map, not turn 1).
    Start-Sleep -Milliseconds 1500
    if (-not (ClickAndLeave "host" "DLG_LOBBY" "BTN_OK" 45)) { return $false }
    if (-not (DriveToStrategic "host" 180)) { return $false }   # dismiss briefing -> reach the map (slow on software Mesa)
    Write-Host "[disp] HOST reached the strategic map; requesting JOINER" -ForegroundColor Green
    if (-not (ClickAndLeave "join" "DLG_LOBBY" "BTN_OK" 45)) { return $false }
    if (-not (DriveToStrategic "join" 180)) { return $false }   # dismiss briefing -> reach the map (slow on software Mesa)
    Write-Host "[disp] JOINER reached strategic map" -ForegroundColor Green
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
    try { Show-GameWindow -Proc $Proc } catch {}
    Start-Sleep -Milliseconds 900
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.X, $b.Y, 0, 0, $b.Size)
        $png = Join-Path $DumpDir "$Role`_screen.png"
        $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
        Write-Host "[disp] $Role screenshot -> $png"
    } catch { Write-Host "[disp] $Role screenshot failed: $($_.Exception.Message)" }
}

# ---- run -------------------------------------------------------------------
# Wrapped so the relay (and the games, under -Kill) are ALWAYS cleaned up — an uncaught
# throw mid-run must not orphan the node relay, whose named pipe would block the next run.
$h = $null; $j = $null; $ok = $false
try {
    Write-Host "[disp] launching HOST..." -ForegroundColor Cyan
    $h = Launch "host"; $hostLog = "$Game\mss32_$($h.Id).log"
    Write-Host "[disp] host pid=$($h.Id)"

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
