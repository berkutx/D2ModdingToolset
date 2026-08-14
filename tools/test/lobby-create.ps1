#requires -Version 7.0
<#
.SYNOPSIS
  Single-instance custom-LOBBY test: log into the live lobby server, create a room backed by a
  freshly generated random map, confirm the host reaches the room, then leave cleanly.

.DESCRIPTION
  Manual live-server integration test (not run in CI). Reuses _relay.ps1 primitives. Flow:
    DLG_MAIN_MENU/BTN_TUTORIAL -> connect to lobby server -> DLG_LOGIN_ACCOUNT
    -> login -> DLG_CUSTOM_LOBBY -> BTN_CREATE -> DLG_HOST
    -> BTN_RANDOM_MAP -> DLG_RANDOM_SCENARIO_MULTI -> generate -> BTN_ACCEPT
    -> createRoom on the live server + createServer locally -> DLG_LOBBY (room created).
  The lobby server is LIVE and shared, so: one room only, a clearly test-named game, and a graceful
  leave (back out of the room + lobby) on exit so no orphan room is left behind. -Keep skips cleanup.
#>
param(
    [string]$GameDir,
    [string]$Account  = $env:D2_LOBBY_ACCOUNT,    # or pass -Account; never hardcode a secret
    [string]$Password = $env:D2_LOBBY_PASSWORD,   # or pass -Password / set D2_LOBBY_PASSWORD
    [int]$Template    = 5,            # template listbox index; 5 = Diligence (verified to generate; index 3 Bladerunner_trinity sol3-panics)
    [int]$SizeIndex   = 0,            # valid default/smallest SPIN_SIZE option
    [string]$RoomName = 'berkut_test',
    [int]$GenWaitSec  = 180,
    [switch]$Keep
)

. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir
if (-not $Account -or -not $Password) {
    throw "Lobby credentials required: pass -Account/-Password, or set D2_LOBBY_ACCOUNT / D2_LOBBY_PASSWORD."
}
Write-Host "[lobby] GameDir = $GameDir" -ForegroundColor Cyan

# Clean slate WITHOUT a blanket kill: only our tagged window + a stale dplaysvr (per project rule).
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$relay = Start-TestRelay
Write-Host "[lobby] relay up; launching one instance..." -ForegroundColor Cyan
$client = $null; $ok = $false; $created = $false
try {
    $client = Start-GameClient -GameDir $GameDir -Role host
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { throw "never reached DLG_MAIN_MENU" }
    Write-Host "[lobby] at main menu; entering custom lobby (BTN_TUTORIAL)" -ForegroundColor Cyan

    # Enter lobby -> connect to the live server -> login screen.
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_TUTORIAL DLG_LOGIN_ACCOUNT 40)) {
        throw "login screen never appeared (lobby server $($null) unreachable? current=$(Get-Dialog host))"
    }
    Write-Host "[lobby] DLG_LOGIN_ACCOUNT up; logging in as '$Account'" -ForegroundColor Cyan
    if (-not (Set-EditText host DLG_LOGIN_ACCOUNT EDIT_ACCOUNT_NAME $Account)) { throw "EDIT_ACCOUNT_NAME not set" }
    Start-Sleep 1
    if (-not (Set-EditText host DLG_LOGIN_ACCOUNT EDIT_PASSWORD $Password)) { throw "EDIT_PASSWORD not set" }
    Start-Sleep 1
    if (-not (Invoke-Button host DLG_LOGIN_ACCOUNT BTN_OK)) { throw "login BTN_OK not found" }

    # Successful login transitions to the lobby. A bad login pops a message box instead.
    $t0 = Get-Date; $inLobby = $false
    while ((((Get-Date) - $t0).TotalSeconds) -lt 30) {
        $d = Get-Dialog host
        if ($d -eq 'DLG_CUSTOM_LOBBY') { $inLobby = $true; break }
        if ($d -eq 'DLG_MESSAGE_BOX') {
            $msg = ((((Get-GameUi host).widgets | Where-Object { $_.type -eq 'text' }).state.text) -join ' | ')
            throw "login failed (DLG_MESSAGE_BOX: $msg)"
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $inLobby) { throw "did not reach DLG_CUSTOM_LOBBY (current=$(Get-Dialog host))" }
    Write-Host "[lobby] logged in; in DLG_CUSTOM_LOBBY" -ForegroundColor Green

    # Create a room -> the host new-game dialog.
    if (-not (Step-ToDialog host DLG_CUSTOM_LOBBY BTN_CREATE DLG_HOST 25)) { throw "BTN_CREATE did not open DLG_HOST" }
    $hostNames = (Get-GameUi host).widgets.name
    Write-Host "[lobby] DLG_HOST open; widgets: $($hostNames -join ', ')" -ForegroundColor Cyan
    # Set the room/game name here too (harmless if the generator dialog also has it).
    $null = Set-EditText host DLG_HOST EDIT_GAME $RoomName
    # Generator entry button: BTN_RANDOM_MAP (same base as the DirectPlay host dialog); fall back to BTN_LOAD.
    $genBtn = if ($hostNames -contains 'BTN_RANDOM_MAP') { 'BTN_RANDOM_MAP' } else { 'BTN_LOAD' }
    Write-Host "[lobby] generator entry button = $genBtn" -ForegroundColor Cyan
    if (-not (Step-ToDialog host DLG_HOST $genBtn DLG_RANDOM_SCENARIO_MULTI 25)) { throw "no generator (DLG_RANDOM_SCENARIO_MULTI) via $genBtn; DLG_HOST widgets: $($hostNames -join ', ')" }
    $D = 'DLG_RANDOM_SCENARIO_MULTI'
    $names = (Get-GameUi host).widgets.name
    foreach ($w in 'TLBOX_TEMPLATES', 'EDIT_NAME', 'SPIN_SIZE', 'BTN_GENERATE') {
        if ($names -notcontains $w) { throw "generator missing $w (widgets: $($names -join ', '))" }
    }
    Write-Host "[lobby] generator open ($($names.Count) widgets); driving form" -ForegroundColor Green

    if (-not (Set-ListSelection host $D TLBOX_TEMPLATES $Template)) { throw "TLBOX_TEMPLATES not set" }
    Start-Sleep 3
    $null = Set-EditText host $D EDIT_GAME $RoomName              # room name (createRoom requires non-empty)
    Start-Sleep 1
    if (-not (Set-EditText host $D EDIT_NAME 'AutoTest')) { throw "EDIT_NAME not set" }
    Start-Sleep 3
    if (-not (Set-SpinOption host $D SPIN_SIZE $SizeIndex)) { throw "SPIN_SIZE not set" }
    Start-Sleep 3
    if ((Get-Dialog host) -ne $D) { throw "left '$D' while driving the form (crash?)" }
    if (-not (Invoke-Button host $D BTN_GENERATE)) { throw "BTN_GENERATE not found" }
    Write-Host "[lobby] BTN_GENERATE clicked; waiting up to ${GenWaitSec}s for the map" -ForegroundColor Cyan

    $t0 = Get-Date; $done = $false
    while ((((Get-Date) - $t0).TotalSeconds) -lt $GenWaitSec) {
        if ($client.HasExited) { throw "game crashed during generation" }
        $d = Get-Dialog host
        if ($d -eq 'DLG_GENERATION_RESULT') { $done = $true; break }
        if ($d -eq 'DLG_MESSAGE_BOX') {
            $msg = ((((Get-GameUi host).widgets | Where-Object { $_.type -eq 'text' }).state.text) -join ' | ')
            throw "generation errored (DLG_MESSAGE_BOX: $msg)"
        }
        Start-Sleep -Milliseconds 1000
    }
    if (-not $done) { throw "generation did not finish in ${GenWaitSec}s (current=$(Get-Dialog host))" }
    Write-Host "[lobby] map generated (DLG_GENERATION_RESULT); accepting -> createRoom on live server" -ForegroundColor Green

    # Accept -> createRoomAndServer: registers the room on the live lobby + starts the local host.
    if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { throw "BTN_ACCEPT not found" }
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 40) {
        $d = Get-Dialog host
        if ($d -eq 'DLG_LOBBY') { $created = $true; break }
        if ($d -eq 'DLG_MESSAGE_BOX') {
            $msg = ((((Get-GameUi host).widgets | Where-Object { $_.type -eq 'text' }).state.text) -join ' | ')
            throw "createRoom failed (DLG_MESSAGE_BOX: $msg)"
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $created) { throw "room not created (no DLG_LOBBY; current=$(Get-Dialog host))" }
    Write-Host "[lobby] *** ROOM CREATED: host is in DLG_LOBBY (room '$RoomName' on the live lobby) ***" -ForegroundColor Green
    $ok = $true
} catch {
    Write-Host "[lobby] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($created -and -not $Keep) {
        # Leave the room + lobby cleanly so no orphan room is left on the live server.
        Write-Host "[lobby] leaving room + lobby cleanly..." -ForegroundColor Yellow
        $null = Invoke-Button host DLG_LOBBY BTN_BACK; Start-Sleep 1
        $null = Invoke-Button host DLG_LOBBY BTN_CANCEL; Start-Sleep 1
        # confirm-leave dialogs, if any
        foreach ($i in 1..6) {
            $d = Get-Dialog host
            if ($d -eq 'DLG_CUSTOM_LOBBY') { $null = Invoke-Button host DLG_CUSTOM_LOBBY BTN_BACK }
            elseif ($d -eq 'DLG_QUESTION' -or $d -eq 'DLG_MESSAGE_BOX') { $null = Invoke-Button host $d BTN_OK; $null = Invoke-Button host $d BTN_YES }
            elseif ($d -eq 'DLG_MAIN_MENU') { break }
            Start-Sleep -Milliseconds 800
        }
        Write-Host "[lobby] left (now at $(Get-Dialog host))" -ForegroundColor Yellow
    }
    Write-Host "`n==== RESULT: lobby-room-created=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if ($Keep) {
        Write-Host "[lobby] left running (relay pid=$($relay.Id), client pid=$($client.Id))." -ForegroundColor Yellow
    } else {
        if ($client) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        if ($relay)  { Stop-Process -Id $relay.Id  -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
    }
}
if (-not $ok) { exit 1 }
