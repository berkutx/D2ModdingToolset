#requires -Version 7.0
<#
.SYNOPSIS
  Drive / inspect the Disciples 2 test build through the d2lobby.packetlogic relay.

.DESCRIPTION
  Thin PowerShell wrapper over the relay's HTTP API (default http://127.0.0.1:8077).
  Part of the publishable test/logging system. Start relay.js first, then launch the
  game with the DebugTest mss32 build and (at least) D2TESTDRV_RELAY_BRIDGE=1 so the
  bridge connects to the relay, D2TESTDRV_UI_REPORTER=1 so there is live UI to read,
  and D2TESTDRV_ROLE=host so the agent registers under a known role (the relay keys
  every client by its role; -Role below must match).

.EXAMPLE
  . .\drive-game-relay.ps1
  Get-GameStatus
  Get-GameMenu -Role host
  Invoke-GameButton -Role host -Dialog DLG_MAIN_MENU -Button BTN_MULTI
  Set-GameSelection -Role host -Dialog DLG_PROTOCOL -ListBox TLBOX_PROTOCOL -Index 2
  Get-GameChat
  Get-GameEvents
#>

param([string]$RelayBase = 'http://127.0.0.1:8077')

$script:RelayBase = $RelayBase

function Get-GameStatus {
    Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/status"
}

function Get-GameMenu {
    # Live per-role UI state (dialog + buttons), as the DLL forwards each Dialog change.
    # /api/state returns { roles: { host: {connected,dialog,buttons,reachedStrategic}, ... } };
    # with -Role, return just that role's entry.
    param([string]$Role)
    $roles = (Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/state").roles
    if ($Role) { return $roles.$Role }
    return $roles
}

function Get-GameLog {
    Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/log"
}

function Get-GameChat {
    (Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/chat").chat
}

function Get-GameEvents {
    (Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/events").events
}

function Get-GamePackets {
    (Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/packets").packets
}

function Invoke-GameButton {
    # Tell the agent of -Role to invoke a button. The relay keys clients by role, so
    # -Role must match the game's D2TESTDRV_ROLE (e.g. host).
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Button
    )
    $uri = "$script:RelayBase/api/invoke?role=$([uri]::EscapeDataString($Role))" +
           "&dlg=$([uri]::EscapeDataString($Dialog))&btn=$([uri]::EscapeDataString($Button))"
    Invoke-RestMethod -Method Post -Uri $uri
}

function Set-GameSelection {
    # Set a list-box selection on the agent of -Role (e.g. pick TCP/IP in DLG_PROTOCOL).
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$ListBox,
        [Parameter(Mandatory)][int]$Index
    )
    $uri = "$script:RelayBase/api/select?role=$([uri]::EscapeDataString($Role))" +
           "&dlg=$([uri]::EscapeDataString($Dialog))&lb=$([uri]::EscapeDataString($ListBox))&index=$Index"
    Invoke-RestMethod -Method Post -Uri $uri
}

function Set-GameSpin {
    # Set a spin-button option (0-based) on the agent of -Role (e.g. map size in the generator).
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Spin,
        [Parameter(Mandatory)][int]$Index
    )
    $uri = "$script:RelayBase/api/spin?role=$([uri]::EscapeDataString($Role))" +
           "&dlg=$([uri]::EscapeDataString($Dialog))&spin=$([uri]::EscapeDataString($Spin))&index=$Index"
    Invoke-RestMethod -Method Post -Uri $uri
}

function Set-GameEdit {
    # Set an edit-box's text on the agent of -Role (e.g. the player name before generating).
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Edit,
        [Parameter(Mandatory)][string]$Text
    )
    $uri = "$script:RelayBase/api/edit?role=$([uri]::EscapeDataString($Role))" +
           "&dlg=$([uri]::EscapeDataString($Dialog))&edit=$([uri]::EscapeDataString($Edit))&text=$([uri]::EscapeDataString($Text))"
    Invoke-RestMethod -Method Post -Uri $uri
}

Write-Host "drive-game-relay loaded. Relay: $script:RelayBase" -ForegroundColor Cyan
Write-Host "Commands: Get-GameStatus, Get-GameMenu [-Role], Get-GameLog, Get-GameChat, Get-GameEvents, Get-GamePackets, Invoke-GameButton -Role <r> -Dialog <d> -Button <b>, Set-GameSelection -Role <r> -Dialog <d> -ListBox <lb> -Index <i>, Set-GameSpin -Role <r> -Dialog <d> -Spin <s> -Index <i>" -ForegroundColor DarkGray
