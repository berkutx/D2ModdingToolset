#requires -Version 7.0
<#
.SYNOPSIS
  Drive / inspect the Disciples 2 test build interactively through the relay.

.DESCRIPTION
  Dot-source this for an interactive session. It loads the test toolkit (tools/test/_relay.ps1) so
  you get the same verb-noun commands the tests use — Get-Dialog, Get-GameUi, Invoke-Button,
  Set-ListSelection, Set-SpinOption, Set-EditText, Wait-Dialog — plus the read-only inspectors below.

  Start relay.js first, then launch the game with the DebugTest mss32 build and at least
  D2TESTDRV_RELAY_BRIDGE=1 (bridge connects), D2TESTDRV_UI_REPORTER=1 (live UI), and
  D2TESTDRV_ROLE=host (the relay keys clients by role; -Role below must match).

.EXAMPLE
  . .\drive-game-relay.ps1
  Get-GameUi -Role host
  Invoke-Button host DLG_MAIN_MENU BTN_MULTI
  Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2
  Get-GameChat
#>

. "$PSScriptRoot\..\test\_relay.ps1"

function Get-GameStatus  { Invoke-RestMethod "$script:RelayBase/api/status" }
function Get-GameLog     { Invoke-RestMethod "$script:RelayBase/api/log" }
function Get-GameChat    { (Invoke-RestMethod "$script:RelayBase/api/chat").chat }
function Get-GameEvents  { (Invoke-RestMethod "$script:RelayBase/api/events").events }
function Get-GamePackets { (Invoke-RestMethod "$script:RelayBase/api/packets").packets }

Write-Host "drive-game-relay loaded ($script:RelayBase)." -ForegroundColor Cyan
Write-Host "Read:  Get-GameStatus | Get-GameUi -Role <r> | Get-Dialog <r> | Get-GameLog | Get-GameChat | Get-GameEvents | Get-GamePackets" -ForegroundColor DarkGray
Write-Host "Drive: Invoke-Button <r> <dlg> <btn> | Set-ListSelection <r> <dlg> <lb> <i> | Set-SpinOption <r> <dlg> <spin> <i> | Set-EditText <r> <dlg> <edit> <text>" -ForegroundColor DarkGray
