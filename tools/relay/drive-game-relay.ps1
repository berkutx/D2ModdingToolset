#requires -Version 7.0
<#
.SYNOPSIS
  Drive / inspect the Disciples 2 test build through the d2lobby.packetlogic relay.

.DESCRIPTION
  Thin PowerShell wrapper over the relay's HTTP API (default http://127.0.0.1:8077).
  Part of the publishable test/logging system. Start relay.js first, then launch the
  game with the DebugTest mss32 build and D2TESTDRV_NET=1.

.EXAMPLE
  . .\drive-game-relay.ps1
  Get-GameStatus
  Get-GameMenu
  Invoke-GameButton -Dialog DLG_MAIN_MENU -Button BTN_MULTI
  Get-GameChat
  Get-GameEvents
#>

param([string]$RelayBase = 'http://127.0.0.1:8077')

$script:RelayBase = $RelayBase

function Get-GameStatus {
    Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/status"
}

function Get-GameMenu {
    # Current dialog + buttons (populated once the DLL forwards UiSnapshot).
    Invoke-RestMethod -Method Get -Uri "$script:RelayBase/api/ui"
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
    param(
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Button
    )
    $uri = "$script:RelayBase/api/invoke?dlg=$([uri]::EscapeDataString($Dialog))&btn=$([uri]::EscapeDataString($Button))"
    Invoke-RestMethod -Method Post -Uri $uri
}

Write-Host "drive-game-relay loaded. Relay: $script:RelayBase" -ForegroundColor Cyan
Write-Host "Commands: Get-GameStatus, Get-GameMenu, Get-GameLog, Get-GameChat, Get-GameEvents, Get-GamePackets, Invoke-GameButton -Dialog <d> -Button <b>" -ForegroundColor DarkGray
