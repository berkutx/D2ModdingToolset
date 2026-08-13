#requires -Version 7.0
# Shared battle flow for the test harness: drive ONE role's starting hero (garrisoned or already free),
# approach and attack the nearest free neutral monster, run auto-battle, and dismiss the post-battle dialogs - then report the
# BEFORE and post-battle (AFTER) stacks so the caller can verify and/or measure later regeneration. Used
# by attack-monster.ps1 (single-instance) and mp-attack-monsters.ps1 (multiplayer). Dot-source AFTER
# _relay.ps1 (this uses Get-World/Get-Stacks/Move-Stack/Invoke-Toggle/Get-Dialog/Invoke-Button).

# First-turn popups, confirmed to ACTIVATE the turn before issuing a move (BTN_OK closes DLG_BEGIN_TURN,
# BTN_CLOSE the lord-name box, etc.). Address one exact button per known dialog; never try a sequence of
# affirmative buttons against an arbitrary popup.
$script:BattleStartButton = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'
    'DLG_BEGIN_TURN'        = 'BTN_OK'
    'DLG_GETINFO_BOX'       = 'BTN_CLOSE'
    'DLG_MESSAGE_BOX'       = 'BTN_OK'
    'DLG_EVENT_POPUP'       = 'BTN_RIGHTSIDE'
}
# Forward/close buttons for the post-battle result + reward dialogs. BTN_YES is deliberately absent
# (never blind-confirm an unexpected prompt).
$script:BattleClose = @('BTN_CLOSE', 'BTN_OK', 'BTN_TAKEALL', 'BTN_TAKE', 'BTN_CONTINUE', 'BTN_RIGHTSIDE')

# The role's first mobile self stack (its starting hero/leader). Some fixtures put it in the capital;
# multiplayer battle fixtures can explicitly require an already-free stack and avoid testing capital exits.
function Get-SelfHero([string]$Role, [switch]$FreeOnly) {
    @(Get-Stacks $Role) | Where-Object {
        $_.relation -eq 'self' -and $_.units -ge 1 -and (-not $FreeOnly -or -not [bool]$_.inside)
    } | Select-Object -First 1
}
# One stack by id (or $null) from the role's live census.
function Get-StackId([string]$Role, [string]$Id) {
    @(Get-Stacks $Role) | Where-Object { $_.id -eq $Id } | Select-Object -First 1
}

# Drive <Role>'s starting hero: optionally exit the capital, approach + attack the nearest free neutral
# monster, auto-battle, dismiss post-battle dialogs. Returns a result object with the BEFORE and post-battle
# (AFTER) snapshots so the caller can verify the fight and/or measure regeneration after a turn cycle.
# $Client is the game process (to detect a crash). Never throws; returns @{ ok=$false; reason=... } on
# any failure so the caller decides how to report it.
function Invoke-HeroAttack {
    param(
        [Parameter(Mandatory)][string]$Role,
        [System.Diagnostics.Process]$Client,
        [int]$ActivateTimeoutSec = 60,
        [switch]$RequireFreeSelfStack
    )

    # Find the fixture's hero. The legacy single-player fixture starts it in the capital; the multiplayer
    # fixture deliberately supplies a free RodPlacer so this battle oracle does not also assume a generated
    # capital's orientation/exit tile.
    $hero = $null; $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 30) { $hero = Get-SelfHero $Role -FreeOnly:$RequireFreeSelfStack; if ($hero) { break }; Start-Sleep 1 }
    if (-not $hero) { return [pscustomobject]@{ ok = $false; reason = $(if ($RequireFreeSelfStack) { "no free own mobile stack (hero) on the map" } else { "no own mobile stack (hero) on the map" }) } }
    [int]$ax = $hero.x; [int]$ay = $hero.y
    Write-Host ("[battle:$Role] hero {0} {1} at ({2},{3}) mv={4}" -f $hero.id, $(if ($RequireFreeSelfStack) { 'free' } else { 'garrisoned' }), $ax, $ay, $hero.movement) -ForegroundColor Cyan

    # reachedStrategic latches on the FIRST map frame, which can precede the late DLG_BEGIN_TURN. Do not
    # use that transient map as evidence that the turn is active and do not probe/refire Move-Stack.
    # Instead, wait until the relay has actually observed DLG_BEGIN_TURN, acknowledge each known startup
    # dialog at most once (BTN_OK starts activation), and wait for strategicActionReady: the native world
    # reporter's observation of the exact clientTakesTurn admission gate used by Move-Stack. Wall-clock
    # stability is not readiness. Only then issue the first movement intent.
    $consumed = @{}; $activationReady = $false; $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $ActivateTimeoutSec) {
        if ($Client -and $Client.HasExited) { return [pscustomobject]@{ ok = $false; reason = "the game crashed before the first movement" } }
        $state = Get-RoleState $Role
        $d = if ($state) { $state.dialog } else { $null }
        $onMap = $d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL'

        if ([bool]$state.connected -and $onMap -and [bool]$state.sawBeginTurn -and
            [bool]$state.strategicActionReady) {
            $activationReady = $true
            break
        } else {
            if ($d -and $script:BattleStartButton.ContainsKey($d) -and -not $consumed.ContainsKey($d)) {
                $button = $script:BattleStartButton[$d]
                $readyButton = @($state.widgets) | Where-Object {
                    $_.name -eq $button -and $_.type -eq 'button' -and $_.state.enabled -eq $true
                } | Select-Object -First 1
                if ($readyButton) {
                    # Claim before dispatch: an uncertain response is a failure, never permission to refire.
                    $consumed[$d] = $button
                    if (-not (Invoke-Button $Role $d $button)) {
                        return [pscustomobject]@{ ok = $false; reason = "one-shot startup action $d::$button was not accepted" }
                    }
                    if ($d -eq 'DLG_BEGIN_TURN') {
                        Write-Host "[battle:$Role] begin-turn acknowledged; waiting for native clientTakesTurn=true..." -ForegroundColor DarkCyan
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $activationReady) {
        return [pscustomobject]@{ ok = $false; reason = "native clientTakesTurn never became true after DLG_BEGIN_TURN (on $d)" }
    }

    # Refresh the exact stack precondition immediately before the first movement command. If something else
    # already moved it, fail closed rather than manufacturing a second semantic action.
    $hero = Get-StackId $Role $hero.id
    if (-not $hero -or [int]$hero.x -ne $ax -or [int]$hero.y -ne $ay -or
        ($RequireFreeSelfStack -and [bool]$hero.inside) -or (-not $RequireFreeSelfStack -and -not [bool]$hero.inside)) {
        return [pscustomobject]@{ ok = $false; reason = "starting hero changed before the first movement intent" }
    }
    if (-not $RequireFreeSelfStack) {
        if (-not (Move-Stack $Role $hero.id ($ax + 5) ($ay + 5))) {
            return [pscustomobject]@{ ok = $false; reason = "sole garrison-exit intent was not accepted" }
        }
        $t0 = Get-Date; $exited = $false
        while ((((Get-Date) - $t0).TotalSeconds) -lt 15) {
            $h = Get-StackId $Role $hero.id
            if ($h -and ($h.x -ne $ax -or $h.y -ne $ay)) { $hero = $h; $exited = $true; break }
            Start-Sleep 1
        }
        if (-not $exited) { return [pscustomobject]@{ ok = $false; reason = "hero did not exit the garrison (still at ($ax,$ay))" } }
    }
    [int]$ex = $hero.x; [int]$ey = $hero.y
    Write-Host ("[battle:$Role] attack origin is ({0},{1})." -f $ex, $ey) -ForegroundColor Green

    # Nearest FREE neutral monster (relation neutral, not in a fort/city). Filtering on neutral avoids the
    # other player's stacks; skipping inside avoids a siege.
    $mon = @(Get-Stacks $Role) | Where-Object { $_.relation -eq 'neutral' -and (-not $_.inside) } |
        Sort-Object { [Math]::Max([Math]::Abs([int]$_.x - $ex), [Math]::Abs([int]$_.y - $ey)) } | Select-Object -First 1
    if (-not $mon) {
        $vis = @(Get-Stacks $Role) | Where-Object { $_.relation -ne 'self' } |
            ForEach-Object { "$($_.id)@($($_.x),$($_.y)) rel=$($_.relation) inside=$($_.inside) units=$($_.units)" }
        Write-Host ("[battle:$Role] no neutral; visible non-self stacks: {0}" -f $(if ($vis) { $vis -join '; ' } else { '(none in vision)' })) -ForegroundColor DarkYellow
        return [pscustomobject]@{ ok = $false; reason = "no free neutral monster to attack (fog of war?)" }
    }
    [int]$monHp0 = $mon.hp; [int]$monUnits0 = $mon.units
    [int]$heroHp0 = $hero.hp; [int]$heroUnits0 = $hero.units; [int]$heroMv0 = $hero.movement   # full movement at the exit tile
    $adjacent = ([Math]::Max([Math]::Abs([int]$mon.x - $ex), [Math]::Abs([int]$mon.y - $ey)) -le 1)
    Write-Host ("[battle:$Role] target monster {0} ({1},{2}) units={3} hp={4} (adjacent={5})" -f `
            $mon.id, $mon.x, $mon.y, $monUnits0, $monHp0, $adjacent) -ForegroundColor Cyan

    # The movement-point spend is read from the ATTACK itself: the hero walks to the monster (spending
    # movement) and the battle opens while it is still alive, so the spend is captured at DLG_BATTLE_A
    # below. No separate recon move is issued - one right next to the just-exited fort proved fragile (it
    # re-entered the garrison, landed adjacent to the monster making the attack a degenerate move, or
    # desynced the two MP clients into a battle crash). $mvBefore/$mvAfter hold attack-origin -> at-battle.
    $mvBefore = -1; $mvAfter = -1

    # Attack: Move-Stack onto the monster's tile -> routed adjacent, end=monster -> the server starts the battle.
    if (-not (Move-Stack $Role $hero.id $mon.x $mon.y)) { return [pscustomobject]@{ ok = $false; reason = "attack move was not issued" } }
    $t0 = Get-Date; $inBattle = $false
    while ((((Get-Date) - $t0).TotalSeconds) -lt 40) {
        if ($Client -and $Client.HasExited) { return [pscustomobject]@{ ok = $false; reason = "the game crashed on the attack" } }
        if ((Get-Dialog $Role) -eq 'DLG_BATTLE_A') { $inBattle = $true; break }
        Start-Sleep 1
    }
    if (-not $inBattle) { return [pscustomobject]@{ ok = $false; reason = "no battle started (DLG_BATTLE_A; on $(Get-Dialog $Role))" } }
    Write-Host "[battle:$Role] battle started (DLG_BATTLE_A)." -ForegroundColor Green

    # Movement spent reaching the monster: the hero walked from its attack-origin tile and is now
    # alive in the battle, so read its movement (poll briefly for the throttled snapshot to reflect the
    # deduction). If the monster was adjacent the walk is 0 and the value stays unchanged.
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 5) {
        $hb = Get-StackId $Role $hero.id
        if ($hb) { $mvBefore = $heroMv0; $mvAfter = [int]$hb.movement; if ($mvAfter -lt $heroMv0) { break } }
        Start-Sleep 1
    }
    if ($mvAfter -ge 0) {
        Write-Host ("[battle:$Role] movement to reach the monster: {0} -> {1} (spent {2})" -f `
                $mvBefore, $mvAfter, ($mvBefore - $mvAfter)) -ForegroundColor DarkCyan
    }

    # Auto-battle (the AI plays every round; not an instant resolve).
    if (-not (Invoke-Toggle $Role DLG_BATTLE_A TOG_AUTOBATTLE)) { return [pscustomobject]@{ ok = $false; reason = "TOG_AUTOBATTLE not toggled" } }
    Write-Host "[battle:$Role] auto-battle on; fighting..." -ForegroundColor Cyan

    # Dismiss every post-battle dialog (result + rewards) until the map is back AND HOLDS. A won battle
    # drops a loot dialog (DLG_ITEM, BTN_OK) a beat AFTER the battle viewer closes and the map flashes, so
    # do not return on the first map sighting: require the map to hold ~3s with no dialog, dismissing any
    # late reward. The in-progress battle viewer (DLG_BATTLE_A) is exempt from the no-progress guard - a
    # long auto-battle keeps it up.
    $t0 = Get-Date; $backOnMap = $false; $last = ''; $lastChange = Get-Date; $mapSince = $null
    while ((((Get-Date) - $t0).TotalSeconds) -lt 150) {
        if ($Client -and $Client.HasExited) { return [pscustomobject]@{ ok = $false; reason = "the game crashed during/after the battle" } }
        $d = Get-Dialog $Role
        if ($d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL') {
            if ($null -eq $mapSince) { $mapSince = Get-Date }
            elseif ((((Get-Date) - $mapSince).TotalSeconds) -ge 3) { $backOnMap = $true; break }   # map held -> no late reward coming
        } else {
            $mapSince = $null   # a dialog is up (result / loot / late reward) -> dismiss it and restart the settle
            if ($d -eq 'DLG_BATTLE_A' -or $d -ne $last) { $last = $d; $lastChange = Get-Date }
            elseif ((((Get-Date) - $lastChange).TotalSeconds) -gt 30) { return [pscustomobject]@{ ok = $false; reason = "stuck on $d after the battle" } }
            if ($d) { foreach ($b in $script:BattleClose) { if (Invoke-Button $Role $d $b) { break } } }
        }
        Start-Sleep -Milliseconds 700
    }
    if (-not $backOnMap) { return [pscustomobject]@{ ok = $false; reason = "did not return to the map after the battle" } }
    Write-Host "[battle:$Role] battle over; back on the map." -ForegroundColor Green

    # One clean post-battle snapshot (retry until the GET itself succeeds, so an ABSENT stack means a real
    # removal - a defeated/destroyed stack - not a dropped poll).
    Start-Sleep -Seconds 2
    $world = $null; $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 15) { $world = Get-World $Role; if ($world -and $world.stacks) { break }; Start-Sleep 1 }
    if (-not $world -or -not $world.stacks) { return [pscustomobject]@{ ok = $false; reason = "could not read the post-battle world snapshot" } }
    $heroAfter = @($world.stacks) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
    $monAfter = @($world.stacks) | Where-Object { $_.id -eq $mon.id } | Select-Object -First 1

    [pscustomobject]@{
        ok       = $true
        role     = $Role
        heroId   = $hero.id
        ex       = $ex; ey = $ey
        monId    = $mon.id; monX = [int]$mon.x; monY = [int]$mon.y
        adjacent = $adjacent
        mvBefore = $mvBefore   # hero movement: attack origin -> battle start (the walk to the monster). -1 if unread.
        mvAfter  = $mvAfter
        before   = [pscustomobject]@{ monHp = $monHp0; monUnits = $monUnits0; heroHp = $heroHp0; heroUnits = $heroUnits0; heroMv = $heroMv0 }
        after    = [pscustomobject]@{
            heroGone  = ($null -eq $heroAfter)
            monGone   = ($null -eq $monAfter)
            heroX     = $(if ($heroAfter) { [int]$heroAfter.x } else { -1 })
            heroY     = $(if ($heroAfter) { [int]$heroAfter.y } else { -1 })
            heroHp    = $(if ($heroAfter) { [int]$heroAfter.hp } else { 0 })
            heroUnits = $(if ($heroAfter) { [int]$heroAfter.units } else { 0 })
            heroMv    = $(if ($heroAfter) { [int]$heroAfter.movement } else { -1 })
            monHp     = $(if ($monAfter) { [int]$monAfter.hp } else { 0 })
            monUnits  = $(if ($monAfter) { [int]$monAfter.units } else { 0 })
        }
    }
}

# Verify one Invoke-HeroAttack result: the hero approached (unless adjacent or destroyed) AND the battle
# resolved (something changed). Returns @{ ok; note } - shared by the single-instance and MP tests.
function Test-AttackResult([pscustomobject]$R) {
    $a = $R.after; $b = $R.before
    $heroMoved = (-not $a.heroGone) -and (($a.heroX -ne $R.ex) -or ($a.heroY -ne $R.ey))
    $monDamaged = (-not $a.monGone) -and (($a.monHp -lt $b.monHp) -or ($a.monUnits -lt $b.monUnits))
    $heroDamaged = (-not $a.heroGone) -and (($a.heroHp -lt $b.heroHp) -or ($a.heroUnits -lt $b.heroUnits))
    $resolved = $a.monGone -or $a.heroGone -or $monDamaged -or $heroDamaged
    if (-not $a.heroGone -and -not $heroMoved -and -not $R.adjacent) { return [pscustomobject]@{ ok = $false; note = "hero did not approach" } }
    if (-not $resolved) { return [pscustomobject]@{ ok = $false; note = "no stack changed: battle did not resolve" } }
    $note = $(if ($a.monGone) { 'monster defeated' } elseif ($a.heroGone) { 'hero defeated' } elseif ($monDamaged -and $heroDamaged) { 'both damaged' } elseif ($monDamaged) { 'monster damaged' } else { 'hero damaged' })
    [pscustomobject]@{ ok = $true; note = $note }
}
