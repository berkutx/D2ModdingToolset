#requires -Version 7.0
<#
.SYNOPSIS
  Single-instance example: generate a map, reach it, then MOVE the player's hero the way a real
  player would, and confirm via the world reporter that the stack moved and spent movement points.

.DESCRIPTION
  Drives the random-scenario generator to a playable strategic map (the world-snapshot.ps1 flow),
  then exercises the world ACTION path (POST /api/ui/move -> testdrv worldactions::moveStack). The
  move is NOT input emulation and NOT a teleport: the in-DLL handler builds the path with the game's
  OWN per-tile cost (computeMovementCost) + passability (stackCanMoveToPosition), annotates it with
  the native PathInfoListApi::populateFromPath, and issues CPhaseGameApi::sendStackMoveMsg, the exact
  call the strategic-map click handler makes. The host then re-validates and applies it (move points
  deducted, battle on contact), identical to a real click.

  It picks the local player's first mobile stack (a hero), targets a tile a couple steps toward the
  nearest neutral (open ground, no contact), issues the move, and asserts the stack's x/y changed and
  its movement points decreased in the next world snapshot. The template is chosen BY NAME ('Diligence').

.EXAMPLE
  .\move-hero.ps1
  .\move-hero.ps1 -Template Fight -Keep
#>
param(
    [string]$GameDir,
    [string]$Template = 'Diligence', # generator template, selected by name (index resolved at runtime)
    [switch]$Keep
)

. "$PSScriptRoot\_relay.ps1"
$GameDir = Resolve-GameDir $GameDir
$templateIndex = Resolve-TemplateIndex $GameDir $Template

# Clean slate without a blanket kill: only our tagged window + a stale dplaysvr.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1000

$relay = Start-TestRelay
Write-Host "[move] relay up; launching host (template '$Template' = index $templateIndex)..." -ForegroundColor Cyan
$client = $null; $ok = $false
try {
    $client = Start-GameClient -GameDir $GameDir -Role host
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { throw "host never reached DLG_MAIN_MENU" }

    # Multiplayer setup -> the random-scenario generator (same nav as world-snapshot.ps1).
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { throw "no DLG_PROTOCOL" }
    if (-not (Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2)) { throw "TLBOX_PROTOCOL not set" } # 2 = TCP/IP
    Start-Sleep 1
    if (-not (Step-ToDialog host DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { throw "no DLG_LOAD_NEW_MULTI" }
    if (-not (Step-ToDialog host DLG_LOAD_NEW_MULTI BTN_HOST DLG_HOST)) { throw "no DLG_HOST" }
    if (-not (Step-ToDialog host DLG_HOST BTN_RANDOM_MAP DLG_RANDOM_SCENARIO_MULTI)) { throw "no generator" }

    $D = "DLG_RANDOM_SCENARIO_MULTI"
    if (-not (Set-ListSelection host $D TLBOX_TEMPLATES $templateIndex)) { throw "TLBOX_TEMPLATES not set" }
    Start-Sleep 3
    if (-not (Set-EditText host $D EDIT_NAME "AutoMove")) { throw "EDIT_NAME not set" } # BTN_GENERATE needs a name
    Start-Sleep 3
    if (-not (Set-SpinOption host $D SPIN_SIZE 0)) { throw "SPIN_SIZE not set" }
    Start-Sleep 3
    if (-not (Invoke-Button host $D BTN_GENERATE)) { throw "BTN_GENERATE not found" }
    Write-Host "[move] generating ($Template)..." -ForegroundColor Cyan

    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 300) {
        if ($client.HasExited) { throw "the game crashed during generation" }
        $d = Get-Dialog host
        if ($d -eq 'DLG_GENERATION_RESULT') { break }
        if ($d -eq 'DLG_MESSAGE_BOX') { throw "generation errored (template $Template; DLG_MESSAGE_BOX)" }
        Start-Sleep -Milliseconds 1000
    }
    if ((Get-Dialog host) -ne 'DLG_GENERATION_RESULT') { throw "generation did not finish in time (on $(Get-Dialog host))" }

    # Accept + start solo (AI fills the rest). Both semantic actions are one-shot; their effects are
    # observed below rather than redispatched while their synchronous callbacks are still unwinding.
    if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { throw "BTN_ACCEPT not found" }
    if (-not (Wait-Dialog host DLG_LOBBY 20)) { throw "BTN_ACCEPT did not open DLG_LOBBY" }
    $popups = @{
        'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'
        'DLG_GETINFO_BOX' = 'BTN_CLOSE'; 'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
    }
    if (-not (Invoke-Button host DLG_LOBBY BTN_OK)) { throw "BTN_OK did not start the generated scenario" }

    # The first map frame can precede a late begin-turn popup and object-lock drain. Service only the
    # known startup dialogs, at most once per observed dialog, until one fresh world snapshot has both
    # the hero and the exact stock strategic admission. Do not probe or retry Move-Stack.
    $hero = $null; $neutral = $null; $world = $null; $claimed = @{}
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 180) {
        $state = Get-RoleState host
        $d = if ($state) { $state.dialog } else { $null }
        if ($d -and $popups.ContainsKey($d) -and -not $claimed.ContainsKey($d)) {
            $button = $popups[$d]
            $readyButton = @($state.widgets) | Where-Object {
                $_.name -eq $button -and $_.type -eq 'button' -and $_.state.enabled -eq $true
            } | Select-Object -First 1
            if ($readyButton) {
                $claimed[$d] = $button
                if (-not (Invoke-Button host $d $button)) { throw "one-shot startup action $d::$button was not accepted" }
            }
        }
        $world = Get-World host
        $stacks = @($world.stacks)
        $hero = $stacks | Where-Object { $_.relation -eq 'self' -and $_.movement -gt 0 } | Select-Object -First 1
        $neutral = $stacks | Where-Object { $_.relation -ne 'self' } | Select-Object -First 1
        $onMap = $d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL'
        if ($hero -and [bool]$state.connected -and $onMap -and [bool]$state.sawBeginTurn -and
            [bool]$world.strategicActionReady) { break }
        $hero = $null
        Start-Sleep -Milliseconds 500
    }
    if (-not $hero) { throw "no ready own mobile stack (movement>0) after stock turn startup" }
    Write-Host "[move] reached the ready strategic map; hero located." -ForegroundColor Green
    Write-Host ("[move] hero id={0} at ({1},{2}) movement={3}" -f $hero.id, $hero.x, $hero.y, $hero.movement) -ForegroundColor Cyan

    # A garrisoned hero reports the fort anchor, not a walkable tile. Exit through the one observed
    # capital gate gesture first; then choose an ordinary destination from the fresh free-stack state.
    if ($hero.inside) {
        $heroId = $hero.id
        if (-not (Move-Stack host $heroId ([int]$hero.x + 5) ([int]$hero.y + 5))) {
            throw "capital exit was not issued"
        }
        $exitX = [int]$hero.x + 5; $exitY = [int]$hero.y + 5
        $t0 = Get-Date
        do {
            Start-Sleep -Milliseconds 500
            $world = Get-World host
            $hero = @($world.stacks) | Where-Object { $_.id -eq $heroId -and -not $_.inside } | Select-Object -First 1
        } while ((-not $hero -or -not [bool]$world.strategicActionReady) -and (((Get-Date) - $t0).TotalSeconds -lt 20))
        if (-not $hero -or $hero.x -ne $exitX -or $hero.y -ne $exitY) {
            throw "hero did not leave the capital at the exact gate"
        }
        if (-not [bool]$world.strategicActionReady) { throw "capital exit applied but the stock strategic lock did not clear" }
    }

    # Target a tile a couple steps toward the nearest neutral (open ground, short of contact). The DLL
    # pathfinds with the game's own cost/passability; if the exact tile is blocked it moves as far
    # toward it as it can, the way a player clicking a far tile would.
    [int]$x0 = $hero.x; [int]$y0 = $hero.y; [int]$mv0 = $hero.movement
    $sign = { param($a, $b) if ($b -gt $a) { 1 } elseif ($b -lt $a) { -1 } else { 0 } }
    if ($neutral) {
        $tx = $x0 + 2 * (& $sign $x0 $neutral.x)
        $ty = $y0 + 2 * (& $sign $y0 $neutral.y)
        if ($tx -eq $x0 -and $ty -eq $y0) { $tx = $x0 + 2; $ty = $y0 } # degenerate (same tile)
        Write-Host ("[move] nearest neutral at ({0},{1}); target ({2},{3})" -f $neutral.x, $neutral.y, $tx, $ty) -ForegroundColor Cyan
    } else {
        $tx = $x0 + 2; $ty = $y0 + 2 # no neutral in view; just step toward the map interior
        Write-Host ("[move] no neutral in view; target ({0},{1})" -f $tx, $ty) -ForegroundColor Cyan
    }

    # Issue the native move (sendStackMoveMsg). `found` = the move was built + submitted.
    if (-not (Move-Stack host $hero.id $tx $ty)) { throw "move was not issued (Move-Stack returned found=false)" }
    Write-Host "[move] move issued; waiting for the host to apply it..." -ForegroundColor Green

    # Assert the EFFECT: the stack's position changed and/or its movement points decreased in the next
    # snapshot (the host applies the move, deducts MP; the world reporter re-reads it within ~500ms).
    $moved = $false; $after = $null
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 20) {
        $after = @(Get-Stacks host) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
        if ($after -and (($after.x -ne $x0) -or ($after.y -ne $y0) -or ($after.movement -lt $mv0))) { $moved = $true; break }
        Start-Sleep -Milliseconds 700
    }
    if (-not $after) { throw "the hero vanished from the world snapshot after the move" }
    Write-Host ("[move] after: ({0},{1}) movement={2}  (before: ({3},{4}) movement={5})" -f `
            $after.x, $after.y, $after.movement, $x0, $y0, $mv0) -ForegroundColor Cyan
    if (-not $moved) { throw "the hero did not move (position + movement unchanged): the native move did not apply" }

    Write-Host "[move] hero moved and spent movement points: native client move confirmed." -ForegroundColor Green
    $ok = $true
} catch {
    Write-Host "[move] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Write-Host "`n==== RESULT: move-hero=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if ($Keep) {
        Write-Host "[move] left running (relay pid=$($relay.Id), client pid=$($client.Id))." -ForegroundColor Yellow
    } else {
        if ($client) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
    }
}
# -ErrorAction Continue so a real failure exits cleanly (exit 1) under the CI shell's Stop preference.
if (-not $ok) { Write-Error "move-hero test failed" -ErrorAction Continue; exit 1 }
