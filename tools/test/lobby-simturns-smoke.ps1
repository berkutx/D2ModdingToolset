#requires -Version 7.0
<#
.SYNOPSIS
Two real clients: authenticated lobby, generated OH room and native bootstrap.
.DESCRIPTION
Uses the same relay/UI/world API as the local acceptance. No local OH coordinator,
DirectPlay shortcut, OBS, game-file edits or fixed-fixture PASS projection.
Credentials are read only from D2_LOBBY_HOST_ACCOUNT/PASSWORD and
D2_LOBBY_JOIN_ACCOUNT/PASSWORD. This is an explicit live integration test, not CI.
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$ArtifactDir,
    [string]$TemplateName,
    [ValidateRange(30, 600)][int]$TimeoutSec = 240,
    [switch]$Keep,
    [switch]$StaticCheck
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/_relay.ps1"

function Wait-LobbyObservation([scriptblock]$Predicate, [string]$Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        foreach ($process in $script:LobbyClients.Values) {
            $process.Refresh()
            if ($process.HasExited) { throw "Owned game exited during $Description (code $($process.ExitCode))." }
        }
        $value = & $Predicate
        if ($value) { return $value }
        Start-Sleep -Milliseconds 200
    }
    throw "Timed out waiting for $Description. No action was retried."
}

function Require-LobbyDialog([string]$Role, [string]$Dialog) {
    [void](Wait-LobbyObservation { Test-DialogReady $Role $Dialog } "$Role $Dialog")
}

function Invoke-LobbyButton([string]$Role, [string]$Dialog, [string]$Button) {
    [void](Wait-LobbyObservation {
        if (-not (Test-DialogReady $Role $Dialog)) { return $false }
        $control = @((Get-GameUi $Role).widgets | Where-Object name -ceq $Button)
        $control.Count -eq 1 -and $control[0].state.enabled -eq $true
    } "$Role ${Dialog}::$Button enabled")
    if (-not (Invoke-Button $Role $Dialog $Button)) { throw "$Role ${Dialog}::$Button was not delivered." }
}

function Advance-LobbyStartup([string]$Role) {
    $state = Get-RoleState $Role
    if (-not $state -or -not [bool]$state.dialogReady) { return $false }
    $dismiss = @{ DLG_SCENARIO_BRIEFING = 'BTN_CONTINUE'; DLG_BEGIN_TURN = 'BTN_OK'; DLG_GETINFO_BOX = 'BTN_CLOSE' }
    if ($dismiss.ContainsKey([string]$state.dialog)) {
        [void](Invoke-ButtonOncePerAppearance -Role $Role -Dialog $state.dialog -Button $dismiss[$state.dialog] -Consumed $consumed)
        return $false
    }
    return $state.dialog -in @('DLG_STRATEGIC', 'DLG_ISO_PAL')
}

function Get-LobbyWidget([string]$Role, [string]$Name) {
    $matches = @((Get-GameUi $Role).widgets | Where-Object name -ceq $Name)
    if ($matches.Count -ne 1) { throw "$Role requires one visible/available native control $Name." }
    return $matches[0]
}

function Find-ExactLobbyRoom([object]$Widget, [string]$RoomName) {
    $property = $Widget.state.PSObject.Properties['items']
    if ($null -eq $property) { throw 'Harness omitted exact lobby room names; index-only joining is forbidden.' }
    $items = @($property.Value)
    $indices = @(for ($i = 0; $i -lt $items.Count; $i++) { if ([string]$items[$i] -ceq $RoomName) { $i } })
    if ($indices.Count -gt 1) { throw 'Lobby room name is ambiguous.' }
    if ($indices.Count -eq 0) { return -1 }
    return [int]$indices[0]
}

function Connect-LobbyClient([string]$Role, [string]$Account, [string]$Password) {
    Require-LobbyDialog $Role DLG_MAIN_MENU
    Invoke-LobbyButton $Role DLG_MAIN_MENU BTN_TUTORIAL
    Require-LobbyDialog $Role DLG_LOGIN_ACCOUNT
    if (-not (Set-SecretEditText $Role DLG_LOGIN_ACCOUNT EDIT_ACCOUNT_NAME $Account) -or
        -not (Set-SecretEditText $Role DLG_LOGIN_ACCOUNT EDIT_PASSWORD $Password)) {
        throw "$Role login input was not delivered; credentials omitted."
    }
    Invoke-LobbyButton $Role DLG_LOGIN_ACCOUNT BTN_OK
    Require-LobbyDialog $Role DLG_CUSTOM_LOBBY
}

function Assert-LobbyGameConfiguration([string]$Directory) {
    $section = ''
    $displayErrors = $false
    foreach ($line in Get-Content -LiteralPath (Join-Path $Directory 'Disciple.ini')) {
        if ($line -match '^\s*\[([^]]+)\]') { $section = $Matches[1]; continue }
        if ($section -ieq 'Disciple' -and $line -match '^\s*DisplayErrors\s*=\s*(\d+)\s*(?:;.*)?$') {
            $displayErrors = $Matches[1] -eq '1'
        }
    }
    if (-not $displayErrors) { throw 'DisplayErrors=1 is required in [Disciple] before launching either client.' }
    $settings = Get-Content -LiteralPath (Join-Path $Directory 'Scripts/userSettings.lua') -Raw
    $settings = [regex]::Replace($settings, '(?s)--\[\[.*?\]\]', '')
    $settings = [regex]::Replace($settings, '(?m)--[^\r\n]*', '')
    if ($settings -notmatch '(?s)controls\s*=\s*\{[^}]*\bsimultaneousTurns\s*=\s*true\b') {
        throw 'Lobby OH controls must already be explicitly enabled in settings.lobby.controls.simultaneousTurns. This driver never edits Lua.'
    }
}

if ($StaticCheck) {
    foreach ($file in @($PSCommandPath, "$PSScriptRoot/_relay.ps1", "$PSScriptRoot/simturns-test.ps1")) {
        $tokens = $null; $errors = $null
        [void][Management.Automation.Language.Parser]::ParseFile($file, [ref]$tokens, [ref]$errors)
        if ($errors.Count) { throw ($errors.Message -join '; ') }
    }
    Write-Output 'Lobby smoke static contract PASS (no game, login, room or network call).'
    exit 0
}

$GameDir = Resolve-GameDir $GameDir
Assert-LobbyGameConfiguration $GameDir
if ([string]::IsNullOrWhiteSpace($TemplateName)) { throw 'An exact local TemplateName is required; no guessed template index.' }
$templateIndex = Resolve-TemplateIndex $GameDir $TemplateName
$credentials = @{}
foreach ($role in @('host', 'join')) {
    $prefix = 'D2_LOBBY_' + $role.ToUpperInvariant()
    $account = [Environment]::GetEnvironmentVariable("${prefix}_ACCOUNT")
    $password = [Environment]::GetEnvironmentVariable("${prefix}_PASSWORD")
    if (-not $account -or -not $password) { throw "Set ${prefix}_ACCOUNT and ${prefix}_PASSWORD explicitly for this live test." }
    $credentials[$role] = @{ account = $account; password = $password }
}
if ($credentials.host.account -ceq $credentials.join.account) { throw 'Two distinct lobby accounts are required.' }
if (-not $ArtifactDir) { throw 'An explicit new ArtifactDir is required.' }
$ArtifactDir = [IO.Path]::GetFullPath($ArtifactDir)
if (Test-Path -LiteralPath $ArtifactDir) { throw 'ArtifactDir already exists; refusing mixed evidence.' }
# Lobby does not use DirectPlay's machine-global helper; unrelated sessions are untouched.
$script:LobbyClients = @{}
$relay = $null
$success = $false
$failure = $null
$roomName = 'OH-test-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
$roomIndex = -1
$worlds = @{}
$clientLogs = @{}
$consumed = @{}
$preexistingLogs = @(Get-ChildItem -LiteralPath $GameDir -Filter 'mss32_*.log' -File | ForEach-Object Name)
$previousPipe = $env:D2TESTDRV_PIPE_NAME
$env:D2TESTDRV_PIPE_NAME = '\\.\pipe\d2mss.lobbytest.' + [guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $ArtifactDir | Out-Null
try {
    $relay = Start-TestRelay -LogDir $ArtifactDir
    foreach ($role in @('host', 'join')) {
        $expectedRoom = if ($role -eq 'join') { $roomName } else { '' }
        $script:LobbyClients[$role] = Start-GameClient -GameDir $GameDir -Role $role -Transport Lobby -ExpectedLobbyRoom $expectedRoom
        if ("mss32_$($script:LobbyClients[$role].Id).log" -in $preexistingLogs) {
            throw 'Owned PID collides with an existing log; refusing stale bootstrap evidence.'
        }
        Assert-OwnedRelayClientIdentity -Role $role -Process $script:LobbyClients[$role] -GameDir $GameDir
        Connect-LobbyClient $role $credentials[$role].account $credentials[$role].password
        $credentials[$role].Clear()
    }
    Invoke-LobbyButton host DLG_CUSTOM_LOBBY BTN_CREATE
    Require-LobbyDialog host DLG_HOST
    $toggle = Get-LobbyWidget host TOG_SIM_DAYS_LABEL
    if (-not [bool]$toggle.state.checked -and -not (Enable-Toggle host DLG_HOST TOG_SIM_DAYS_LABEL)) {
        throw 'Could not enable OH for this test room.'
    }
    # A smoke run is deliberately casual and has no finite merge barrier.
    $ranked = @((Get-GameUi host).widgets | Where-Object name -ceq TOG_RANKED)
    if ($ranked.Count -eq 1 -and [bool]$ranked[0].state.checked) { throw 'Smoke refuses a ranked room; choose casual in the existing local defaults.' }
    if (-not (Set-SpinOption host DLG_HOST SPIN_SIM_DAYS 0)) { throw 'Could not select unlimited OH days.' }
    if (-not (Set-EditText host DLG_HOST EDIT_GAME $roomName)) { throw 'Room name was not set.' }
    Invoke-LobbyButton host DLG_HOST BTN_RANDOM_MAP
    Require-LobbyDialog host DLG_RANDOM_SCENARIO_MULTI
    if (-not (Set-ListSelection host DLG_RANDOM_SCENARIO_MULTI TLBOX_TEMPLATES $templateIndex) -or
        -not (Set-EditText host DLG_RANDOM_SCENARIO_MULTI EDIT_NAME 'OH test') -or
        -not (Set-EditText host DLG_RANDOM_SCENARIO_MULTI EDIT_GAME $roomName)) {
        throw 'Generated-room form was not populated.'
    }
    Invoke-LobbyButton host DLG_RANDOM_SCENARIO_MULTI BTN_GENERATE
    Require-LobbyDialog host DLG_GENERATION_RESULT
    Invoke-LobbyButton host DLG_GENERATION_RESULT BTN_ACCEPT
    Require-LobbyDialog host DLG_LOBBY
    [void](Wait-LobbyObservation {
        $script:SelectedLobbyRoom = Find-ExactLobbyRoom (Get-LobbyWidget join LBOX_ROOMS) $roomName
        $script:SelectedLobbyRoom -ge 0
    } 'exact generated room in join lobby')
    $roomIndex = $script:SelectedLobbyRoom
    if (-not (Set-ListSelection join DLG_CUSTOM_LOBBY LBOX_ROOMS $roomIndex)) { throw 'Room selection was not delivered.' }
    $roomList = Get-LobbyWidget join LBOX_ROOMS
    if ((Find-ExactLobbyRoom $roomList $roomName) -ne [int]$roomList.state.selected) {
        throw 'Selected room changed before join; no other room will be joined.'
    }
    Invoke-LobbyButton join DLG_CUSTOM_LOBBY BTN_JOIN
    Require-LobbyDialog join DLG_LOBBY
    # These are the normal native room buttons, each submitted exactly once.
    Invoke-LobbyButton host DLG_LOBBY BTN_OK
    [void](Wait-LobbyObservation { Advance-LobbyStartup host } 'host strategic map before join snapshot request')
    Invoke-LobbyButton join DLG_LOBBY BTN_OK
    [void](Wait-LobbyObservation {
        $ready = $true
        foreach ($role in @('host', 'join')) {
            if (-not (Advance-LobbyStartup $role)) { $ready = $false; continue }
            $world = Get-World $role
            if ($world.day -ne 1 -or @($world.players | Where-Object relation -eq 'self').Count -ne 1) { $ready = $false; continue }
            $log = Join-Path $GameDir "mss32_$($script:LobbyClients[$role].Id).log"
            if (-not (Test-Path -LiteralPath $log)) { $ready = $false; continue }
            $text = Get-Content -LiteralPath $log -Raw
            if ($text -match '\[simturns\].*(?:FAULT|terminal fault)') { throw "$role reported a production OH fault." }
            if ($text -notmatch 'bootstrap operational release applied; strict independent turns are operational') { $ready = $false; continue }
            $worlds[$role] = $world
            $clientLogs[$role] = $log
        }
        $ready
    } 'both native maps and production lobby OH bootstrap release')
    $success = $true
} catch {
    $failure = $_.Exception.Message
} finally {
    $credentials.Clear()
    if (-not $Keep) {
        foreach ($process in $script:LobbyClients.Values) {
            try { Stop-OwnedProcess $process } catch { $success = $false; $failure = 'Owned game cleanup failed.' }
        }
        if ($relay) { try { Stop-OwnedProcess $relay } catch { $success = $false; $failure = 'Owned relay cleanup failed.' } }
        foreach ($role in $script:LobbyClients.Keys) {
            $log = Join-Path $GameDir "mss32_$($script:LobbyClients[$role].Id).log"
            if (Test-Path -LiteralPath $log) {
                $saved = Join-Path $ArtifactDir "$role.mss32.log"
                Copy-Item -LiteralPath $log -Destination $saved
                $clientLogs[$role] = $saved
            }
        }
    }
    if ($null -eq $previousPipe) { Remove-Item Env:D2TESTDRV_PIPE_NAME -ErrorAction SilentlyContinue }
    else { $env:D2TESTDRV_PIPE_NAME = $previousPipe }
    # Store only named evidence, never credentials or full process environments.
    $summary = [ordered]@{
        schema = 1; transport = 'lobby'; boundary = 'generated-map-bootstrap'; passed = $success
        campaign18 = $false; gameplayAcceptance = $false; failure = $failure; keptOpen = [bool]$Keep
        roomName = $roomName; templateName = $TemplateName
        dllSha256 = (Get-FileHash -LiteralPath (Join-Path $GameDir 'mss32.dll')).Hash
        clients = @($script:LobbyClients.GetEnumerator() | ForEach-Object { @{ role = $_.Key; pid = $_.Value.Id } })
        worlds = $worlds; clientLogPaths = $clientLogs
    }
    $summary | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $ArtifactDir 'summary.json') -Encoding utf8
}
if (-not $success) { throw "Lobby smoke failed: $failure" }
Write-Output 'Lobby smoke PASS: both generated maps and production OH bootstrap; not the 18-case fixture acceptance.'
