#requires -Version 7.0
# Shared battle flow for the test harness: drive ONE role's hero out of its capital, approach and attack
# the nearest free neutral monster, run auto-battle, and dismiss the post-battle dialogs - then report the
# BEFORE and post-battle (AFTER) stacks so the caller can verify and/or measure later regeneration. Used
# by attack-monster.ps1 (single-instance) and mp-attack-monsters.ps1 (multiplayer). Dot-source AFTER
# _relay.ps1 (this uses Get-World/Get-Stacks/Move-Stack/Invoke-Toggle/Get-Dialog/Invoke-Button).

# First-turn popups, confirmed to ACTIVATE the turn before issuing a move (BTN_OK closes DLG_BEGIN_TURN,
# BTN_CLOSE the lord-name box, etc.). No blind affirmative.
$script:BattleFwd = @('BTN_CONTINUE', 'BTN_OK', 'BTN_ACCEPT', 'BTN_RIGHTSIDE', 'BTN_CLOSE')
# Forward/close buttons for the post-battle result + reward dialogs. BTN_YES is deliberately absent
# (never blind-confirm an unexpected prompt).
$script:BattleClose = @('BTN_CLOSE', 'BTN_OK', 'BTN_TAKEALL', 'BTN_TAKE', 'BTN_CONTINUE', 'BTN_RIGHTSIDE')

# The role's first mobile self stack (its starting hero/leader) on the map.
function Get-SelfHero([string]$Role) {
    @(Get-Stacks $Role) | Where-Object { $_.relation -eq 'self' -and $_.units -ge 1 } | Select-Object -First 1
}
# One stack by id (or $null) from the role's live census.
function Get-StackId([string]$Role, [string]$Id) {
    @(Get-Stacks $Role) | Where-Object { $_.id -eq $Id } | Select-Object -First 1
}

# Drive <Role>'s starting hero: exit the capital, approach + attack the nearest free neutral monster,
# auto-battle, dismiss post-battle dialogs. Returns a result object with the BEFORE and post-battle
# (AFTER) snapshots so the caller can verify the fight and/or measure regeneration after a turn cycle.
# $Client is the game process (to detect a crash). Never throws; returns @{ ok=$false; reason=... } on
# any failure so the caller decides how to report it.
function Invoke-HeroAttack {
    param(
        [Parameter(Mandatory)][string]$Role,
        [System.Diagnostics.Process]$Client,
        [int]$ActivateTimeoutSec = 60,
        [switch]$ReconMove   # take ONE plain step toward the monster first, to surface a numeric movement-point spend
    )

    # Find the hero (garrisoned in the capital at this point).
    $hero = $null; $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 30) { $hero = Get-SelfHero $Role; if ($hero) { break }; Start-Sleep 1 }
    if (-not $hero) { return [pscustomobject]@{ ok = $false; reason = "no own mobile stack (hero) on the map" } }
    [int]$ax = $hero.x; [int]$ay = $hero.y
    Write-Host ("[battle:$Role] hero {0} garrisoned at ({1},{2}) mv={3}" -f $hero.id, $ax, $ay, $hero.movement) -ForegroundColor Cyan

    # The turn must be ACTIVE: the first-day begin-turn popup can appear a beat after the iso view and must
    # be confirmed before the engine accepts a move (it gates on clientTakesTurn). So dismiss any lingering
    # popup AND retry the exit move (anchor+5, a free 0-cost sub-step), stopping on the first ACCEPTED move.
    $issued = $false; $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $ActivateTimeoutSec) {
        if ($Client -and $Client.HasExited) { return [pscustomobject]@{ ok = $false; reason = "the game crashed before the exit" } }
        $d = Get-Dialog $Role
        if ($d -and $d -ne 'DLG_STRATEGIC' -and $d -ne 'DLG_ISO_PAL') {
            foreach ($b in $script:BattleFwd) { if (Invoke-Button $Role $d $b) { break } }
            Start-Sleep -Milliseconds 700; continue
        }
        if (Move-Stack $Role $hero.id ($ax + 5) ($ay + 5)) { $issued = $true; break }
        Start-Sleep -Milliseconds 1000
    }
    if (-not $issued) { return [pscustomobject]@{ ok = $false; reason = "garrison-exit move was not accepted (turn never active?)" } }
    $t0 = Get-Date; $exited = $false
    while ((((Get-Date) - $t0).TotalSeconds) -lt 15) {
        $h = Get-StackId $Role $hero.id
        if ($h -and ($h.x -ne $ax -or $h.y -ne $ay)) { $hero = $h; $exited = $true; break }
        Start-Sleep 1
    }
    if (-not $exited) { return [pscustomobject]@{ ok = $false; reason = "hero did not exit the garrison (still at ($ax,$ay))" } }
    [int]$ex = $hero.x; [int]$ey = $hero.y
    Write-Host ("[battle:$Role] exited to ({0},{1})." -f $ex, $ey) -ForegroundColor Green

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
    [int]$heroHp0 = $hero.hp; [int]$heroUnits0 = $hero.units
    $adjacent = ([Math]::Max([Math]::Abs([int]$mon.x - $ex), [Math]::Abs([int]$mon.y - $ey)) -le 1)
    Write-Host ("[battle:$Role] target monster {0} ({1},{2}) units={3} hp={4} (adjacent={5})" -f `
            $mon.id, $mon.x, $mon.y, $monUnits0, $monHp0, $adjacent) -ForegroundColor Cyan

    # Optional reconnaissance move: one plain step toward the monster BEFORE attacking, so the movement
    # spend is observable on a LIVE hero (the attack itself usually kills the lone leader, leaving no
    # post-move snapshot). Skipped if the monster is within 2 tiles (the step would land on/adjacent it).
    $reconMvBefore = -1; $reconMvAfter = -1
    if ($ReconMove -and [Math]::Max([Math]::Abs([int]$mon.x - $ex), [Math]::Abs([int]$mon.y - $ey)) -ge 3) {
        $rx = $ex + [Math]::Sign([int]$mon.x - $ex); $ry = $ey + [Math]::Sign([int]$mon.y - $ey)
        $mvB = [int]$hero.movement
        if (Move-Stack $Role $hero.id $rx $ry) {
            $moved = $false; $t0 = Get-Date
            while ((((Get-Date) - $t0).TotalSeconds) -lt 12) { $h = Get-StackId $Role $hero.id; if ($h -and ($h.x -ne $ex -or $h.y -ne $ey)) { $hero = $h; $moved = $true; break }; Start-Sleep 1 }
            if ($moved) {
                $reconMvBefore = $mvB; $reconMvAfter = [int]$hero.movement; $ex = [int]$hero.x; $ey = [int]$hero.y
                Write-Host ("[battle:$Role] recon step to ({0},{1}): movement {2} -> {3} (spent {4})" -f `
                        $ex, $ey, $reconMvBefore, $reconMvAfter, ($reconMvBefore - $reconMvAfter)) -ForegroundColor DarkCyan
            }
        }
    }

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
        reconMvBefore = $reconMvBefore   # -1 if no recon step taken; else the hero's movement before/after one plain step
        reconMvAfter  = $reconMvAfter
        before   = [pscustomobject]@{ monHp = $monHp0; monUnits = $monUnits0; heroHp = $heroHp0; heroUnits = $heroUnits0; heroMv = [int]$hero.movement }
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
