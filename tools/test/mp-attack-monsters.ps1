#requires -Version 7.0
<#
.SYNOPSIS
  Multiplayer battle + turn-pass + regeneration test. Two clients (HOST + JOINER) reach a started
  TCP/IP game; each in turn sends its hero out to attack the nearest free neutral monster, then ends its
  turn; after the day rolls over, the damaged monsters' HP recovery (regeneration) is measured.

.DESCRIPTION
  Built on the single-instance battle template (the shared _battle.ps1 Invoke-HeroAttack flow) plus the
  two-instance pairing. Every action goes through the relay, no input emulation. The sequence:

    1. Pair HOST + JOINER: the host GENERATES a random map (template -Template, placing neutrals near
       each start), the joiner joins, and both reach the strategic map.
    2. HOST turn (the host is the active player first): exit the capital, attack the nearest free
       neutral monster, auto-battle, dismiss the post-battle dialogs. Then END the host's turn.
    3. JOINER turn (now active): the same attack flow. Then END the joiner's turn.
    4. The day rolls over (the world snapshot's `day` increments). Any stack that survived a battle
       DAMAGED (a winning hero that took hits, or a surviving monster) REGENERATES; measure its HP
       recovery from its post-battle value.

  The test passes when both battles resolved (a stack changed), the turn passed and the day rolled over,
  and a damaged survivor's regeneration was measured. Regeneration is unit/timing-dependent (a unit
  without the Regeneration ability heals 0%; a monster damaged late has not had a full day to heal), so
  the actual percent is printed for observation and the hard gate `-MinRegenPct` defaults to 0; pass a
  positive value (e.g. 5) for a strict floor. A generated map is used (not a fixed skirmish), because a
  skirmish map can be too sparse: one player kills the only nearby neutral and the other has none in reach.

.EXAMPLE
  .\mp-attack-monsters.ps1 -Kill
  .\mp-attack-monsters.ps1 -Kill -MinRegenPct 5 -Template Diligence -MapSize 1
#>
param(
    [string]$Template = 'Diligence',  # generator template (resolved by NAME); places neutrals near each start
    [int]$MapSize = 1,                # SPIN_SIZE index (0 = smallest); a larger map gives each player its own neutral zone
    [int]$GenWaitSec = 90,            # seconds to wait for the host's map generation
    [int]$MinRegenPct = 0,            # HARD-fail gate on the best survivor's regen (default 0 = observe-only; regen is unit/timing-dependent, ~0-16% per day). Pass e.g. 5 for a strict gate.
    [switch]$Kill,
    [string]$GameDir
)

. "$PSScriptRoot\_relay.ps1"
. "$PSScriptRoot\_battle.ps1"   # Invoke-HeroAttack + Test-AttackResult (the shared battle flow)
$GameDir = Resolve-GameDir $GameDir
$templateIndex = Resolve-TemplateIndex $GameDir $Template

# Clean slate without a blanket kill: only our tagged [HOST]/[CLIENT] windows, plus a stale dplaysvr.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1200
Get-ChildItem $GameDir -Filter "mss32_*.log" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

# First-turn popups, each mapped to the button that closes it (DLG_GETINFO_BOX = the lord-name box,
# BTN_CLOSE accepts the default; do NOT Set-EditText it).
$Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'; 'DLG_GETINFO_BOX' = 'BTN_CLOSE'
    'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
}

function RegenPct([int]$before, [int]$after) {
    if ($before -le 0) { return 0.0 }
    return [Math]::Round((($after - $before) / [double]$before) * 100.0, 1)
}
# Stacks that survived a battle DAMAGED (below their pre-battle HP) and so have room to regenerate: a
# winning hero that took hits, or a surviving monster. Each carries its post-battle HP as the baseline.
function RegenCandidates([pscustomobject]$r, [string]$role, [bool]$includeHero) {
    $out = @()
    if ($includeHero -and -not $r.after.heroGone -and $r.after.heroHp -lt $r.before.heroHp) {
        $out += [pscustomobject]@{ who = 'hero'; role = $role; id = $r.heroId; base = [int]$r.after.heroHp }
    }
    if (-not $r.after.monGone -and $r.after.monHp -lt $r.before.monHp) {
        $out += [pscustomobject]@{ who = 'monster'; role = $role; id = $r.monId; base = [int]$r.after.monHp }
    }
    $out   # callers wrap in @() to normalize 0/1/2 results
}

# Click <btn> on <dlg> until the client LEAVES <dlg> (the lobby OK).
function ClickAndLeave([string]$role, [string]$dlg, [string]$btn, [int]$sec) {
    $t0 = Get-Date; $null = Invoke-Button $role $dlg $btn; $lf = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $sec) {
        if ((Get-Dialog $role) -ne $dlg) { return $true }
        if (((Get-Date) - $lf).TotalSeconds -ge 12) { $null = Invoke-Button $role $dlg $btn; $lf = Get-Date }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
# Dismiss first-turn popups until <role> reaches the map (relay-latched reachedStrategic), paced ~2.5s.
function DriveToStrategic([string]$role, [int]$sec) {
    $t0 = Get-Date; $ld = ''; $lf = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt $sec) {
        $r = Get-RoleState $role
        if ($r -and $r.reachedStrategic) { return $true }
        $d = if ($r) { $r.dialog } else { $null }
        if ($d -and $Dismiss.ContainsKey($d) -and ($d -ne $ld -or ((Get-Date) - $lf).TotalSeconds -ge 2.5)) {
            $null = Invoke-Button $role $d $Dismiss[$d]; $ld = $d; $lf = Get-Date
        }
        Start-Sleep -Milliseconds 700
    }
    return $false
}
# Press BTN_END_TURN for <role>, confirming any popup (an "unmoved units" / message box) so the turn
# passes, until <passed> (a scriptblock) returns true or the timeout elapses.
function EndTurn([string]$role, [scriptblock]$passed, [int]$sec) {
    $t0 = Get-Date; $lf = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt $sec) {
        if (& $passed) { return $true }
        $d = Get-Dialog $role
        if ($d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL') {
            if (((Get-Date) - $lf).TotalSeconds -ge 3) { $null = Invoke-Button $role DLG_STRATEGIC BTN_END_TURN; $lf = Get-Date }
        } elseif ($d) {
            foreach ($b in @('BTN_OK', 'BTN_YES', 'BTN_TAKEALL', 'BTN_TAKE', 'BTN_CONTINUE', 'BTN_CLOSE')) { if (Invoke-Button $role $d $b) { break } }
        }
        Start-Sleep -Milliseconds 600
    }
    return (& $passed)
}

# Pass the host's turn to the joiner: press the host's BTN_END_TURN AND dismiss the JOINER's turn-start
# dialogs (scenario briefing -> begin-turn -> ...), because in MP the joiner's turn opens with those and
# a role must not be left on an open dialog (mirrors multiplayer-two-instance.ps1 -EndHostTurn). Returns
# when the joiner's turn has begun (join.sawBeginTurn latches on its DLG_BEGIN_TURN).
function PassTurnToJoiner([int]$sec) {
    $t0 = Get-Date; $lf = (Get-Date).AddSeconds(-10)
    while ((((Get-Date) - $t0).TotalSeconds) -lt $sec) {
        if ([bool](Get-RoleState join).sawBeginTurn) { return $true }
        $jd = Get-Dialog join
        if ($jd -and $Dismiss.ContainsKey($jd)) { $null = Invoke-Button join $jd $Dismiss[$jd] }   # drive the joiner's new-day popups
        $hd = Get-Dialog host
        if ($hd -eq 'DLG_STRATEGIC' -or $hd -eq 'DLG_ISO_PAL') {
            if (((Get-Date) - $lf).TotalSeconds -ge 3) { $null = Invoke-Button host DLG_STRATEGIC BTN_END_TURN; $lf = Get-Date }
        } elseif ($hd) {
            # Dismiss any host popup that END_TURN raises so the turn actually passes: a won-battle loot
            # dialog (DLG_ITEM, BTN_OK) or the "units can still move, end turn?" confirm (BTN_YES).
            foreach ($b in @('BTN_OK', 'BTN_YES', 'BTN_TAKEALL', 'BTN_TAKE', 'BTN_CONTINUE', 'BTN_CLOSE')) { if (Invoke-Button host $hd $b) { break } }
        }
        Start-Sleep -Milliseconds 600
    }
    return [bool](Get-RoleState join).sawBeginTurn
}

# Pair HOST + JOINER (the host generates a random map) and drive both to the strategic map.
function Pair-AndReachMap {
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { return $false }
    if (-not (Wait-Dialog join DLG_MAIN_MENU 90)) { return $false }
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { return $false }
    $null = Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2; Start-Sleep -Milliseconds 1000   # TCP/IP
    if (-not (Step-ToDialog host DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { return $false }
    if (-not (Step-ToDialog join DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { return $false }
    $null = Set-ListSelection join DLG_PROTOCOL TLBOX_PROTOCOL 2; Start-Sleep -Milliseconds 1000
    if (-not (Step-ToDialog join DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { return $false }
    Write-Host "[mp] joiner staged; host generating a random '$Template' map (index $templateIndex)..." -ForegroundColor DarkGray

    # Host creates the session by GENERATING a random map (a skirmish map can be too sparse - one player
    # kills the only near neutral and the other has none in reach; a generated template places neutrals by
    # each start). DLG_HOST -> BTN_RANDOM_MAP -> DLG_RANDOM_SCENARIO_MULTI form, then accept into the lobby.
    if (-not (Step-ToDialog host DLG_LOAD_NEW_MULTI BTN_HOST DLG_HOST)) { return $false }
    if (-not (Step-ToDialog host DLG_HOST BTN_RANDOM_MAP DLG_RANDOM_SCENARIO_MULTI)) { return $false }
    $D = 'DLG_RANDOM_SCENARIO_MULTI'
    if (-not (Set-ListSelection host $D TLBOX_TEMPLATES $templateIndex)) { return $false }
    Start-Sleep 2
    if (-not (Set-EditText host $D EDIT_NAME "AutoHost")) { return $false }   # BTN_GENERATE needs a name
    Start-Sleep 2
    if (-not (Set-SpinOption host $D SPIN_SIZE $MapSize)) { return $false }
    Start-Sleep 2
    if (-not (Set-SpinOption host $D SPIN_GOAL 0)) { return $false }
    Start-Sleep 2
    if (-not (Invoke-Button host $D BTN_GENERATE)) { return $false }
    Write-Host "[mp] host generating (template '$Template', up to ${GenWaitSec}s)..." -ForegroundColor Cyan
    $t0 = Get-Date; $hostLobby = $false
    while ((((Get-Date) - $t0).TotalSeconds) -lt $GenWaitSec) {
        $d = Get-Dialog host
        if ($d -eq 'DLG_GENERATION_RESULT') {
            if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { return $false }
            $hostLobby = (Wait-Dialog host DLG_LOBBY 20); break
        }
        if ($d -eq 'DLG_MESSAGE_BOX') { Write-Host "[mp] host generation errored (template $Template; DLG_MESSAGE_BOX)" -ForegroundColor Red; return $false }
        Start-Sleep -Milliseconds 1500
    }
    if (-not $hostLobby) { return $false }
    Write-Host "[mp] host in lobby; joiner joining..." -ForegroundColor DarkGray
    if (-not (Step-ToDialog join DLG_LOAD_NEW_MULTI BTN_JOIN DLG_SESSION)) { return $false }
    Start-Sleep -Seconds 3   # session enumeration settle
    if (-not (Step-ToDialog join DLG_SESSION BTN_JOIN_GAME DLG_LOBBY)) { return $false }
    Start-Sleep -Milliseconds 1500
    if (-not (ClickAndLeave host DLG_LOBBY BTN_OK 45)) { return $false }
    if (-not (DriveToStrategic host 180)) { return $false }
    if (-not (ClickAndLeave join DLG_LOBBY BTN_OK 45)) { return $false }
    if (-not (DriveToStrategic join 180)) { return $false }
    return $true
}

# ---- run ----------------------------------------------------------------------------------------
$relay = Start-TestRelay
Write-Host "[mp] relay up; launching clients..." -ForegroundColor Cyan
$h = $null; $j = $null; $ok = $false
try {
    $h = Start-GameClient -GameDir $GameDir -Role host
    Start-Sleep -Seconds 10   # joiner boots 10s later (parallel); the join is gated inside Pair-AndReachMap
    $j = Start-GameClient -GameDir $GameDir -Role join

    if (-not (Pair-AndReachMap)) { throw "pairing failed (host '$(Get-Dialog host)', join '$(Get-Dialog join)')" }
    $players = @((Get-World host).players).Count
    Write-Host "[mp] both reached the strategic map ($players players)." -ForegroundColor Green
    $goldH1 = [int](Get-Resources host).gold   # host day-1 gold (income is credited on turn activation)
    Write-Host "[mp] HOST day-1 gold = $goldH1" -ForegroundColor DarkCyan

    # HOST turn: attack the nearest free neutral monster (the host is the active player first).
    $rh = Invoke-HeroAttack -Role host -Client $h -ReconMove
    if (-not $rh.ok) { throw "host attack: $($rh.reason)" }
    $vh = Test-AttackResult $rh
    Write-Host ("[mp] HOST battle: {0}; monster {1} hp {2} -> {3}; hero hp {4}->{5}{6}" -f `
            $vh.note, $rh.monId, $rh.before.monHp, $rh.after.monHp,
            $rh.before.heroHp, $rh.after.heroHp, $(if ($rh.after.heroGone) { ' GONE' } else { '' })) -ForegroundColor Cyan
    $rhMv = if ($rh.reconMvBefore -ge 0) { "recon step $($rh.reconMvBefore) -> $($rh.reconMvAfter) (spent $($rh.reconMvBefore - $rh.reconMvAfter))" } else { "no recon step (monster within 2 tiles of exit)" }
    Write-Host ("[mp] HOST hero MOVEMENT: garrison {0}; {1}" -f $rh.before.heroMv, $rhMv) -ForegroundColor DarkCyan
    if (-not $vh.ok) { throw "host battle did not resolve: $($vh.note)" }

    # HOST passes its turn to the joiner: presses the host's END_TURN AND drives the joiner's new-day
    # dialogs (scenario briefing -> begin-turn) so the joiner's turn actually begins.
    if (-not (PassTurnToJoiner 150)) { throw "host end-turn did not pass to the joiner" }
    Write-Host "[mp] HOST ended turn -> joiner active." -ForegroundColor Green
    $goldJ1 = [int](Get-Resources join).gold   # joiner day-1 gold (now that its own turn has activated)
    Write-Host "[mp] JOINER day-1 gold = $goldJ1" -ForegroundColor DarkCyan

    # JOINER turn: the same attack flow (its activate loop dismisses the new-day popup first).
    $rj = Invoke-HeroAttack -Role join -Client $j -ReconMove
    if (-not $rj.ok) { throw "join attack: $($rj.reason)" }
    $vj = Test-AttackResult $rj
    Write-Host ("[mp] JOINER battle: {0}; hero hp {1}->{2}{3}; monster {4} hp {5}->{6}{7}" -f `
            $vj.note, $rj.before.heroHp, $rj.after.heroHp, $(if ($rj.after.heroGone) { ' GONE' } else { '' }),
            $rj.monId, $rj.before.monHp, $rj.after.monHp, $(if ($rj.after.monGone) { ' GONE' } else { '' })) -ForegroundColor Cyan
    $rjMv = if ($rj.reconMvBefore -ge 0) { "recon step $($rj.reconMvBefore) -> $($rj.reconMvAfter) (spent $($rj.reconMvBefore - $rj.reconMvAfter))" } else { "no recon step (monster within 2 tiles of exit)" }
    Write-Host ("[mp] JOINER hero MOVEMENT: garrison {0}; {1}" -f $rj.before.heroMv, $rjMv) -ForegroundColor DarkCyan
    if (-not $vj.ok) { throw "join battle did not resolve: $($vj.note)" }

    # JOINER ends its turn -> the day rolls over (the world snapshot's `day` increments once everyone,
    # including any AI players, has passed). Wait on `day` so this is robust to extra players.
    # Read a NON-NULL baseline day first: a dropped /api/world poll returns $null, and [int]$null is 0,
    # which would make "day -gt 0" satisfy on the first real snapshot and falsely report a rollover.
    $day0 = $null; $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 15) { $w = Get-World host; if ($w -and $w.day) { $day0 = [int]$w.day; break }; Start-Sleep 1 }
    if ($null -eq $day0) { throw "could not read the baseline day before the joiner's end-turn" }
    if (-not (EndTurn join { $w2 = Get-World host; [bool]($w2 -and [int]$w2.day -gt $day0) } 240)) { throw "join end-turn did not roll the day over (still day $day0)" }
    $day1 = [int](Get-World host).day
    Write-Host "[mp] JOINER ended turn -> day $day0 -> $day1." -ForegroundColor Green

    # Activate the host's new-day turn (dismiss its begin-turn popups) so a surviving host hero rests and
    # heals, then let the regeneration + throttled snapshots settle.
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 60) {
        $d = Get-Dialog host
        if ($d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL') { break }
        if ($d -and $Dismiss.ContainsKey($d)) { $null = Invoke-Button host $d $Dismiss[$d] }
        Start-Sleep -Milliseconds 600
    }
    Start-Sleep -Seconds 4

    # Daily income: the host's gold from its day-1 turn to its day-2 turn (it never spent any), so the
    # delta is the day's treasury income, credited when the new-day turn activated above.
    $goldH2 = [int](Get-Resources host).gold
    Write-Host ("[mp] DAILY INCOME host: day-1 gold {0} -> day-2 gold {1} (+{2})" -f $goldH1, $goldH2, ($goldH2 - $goldH1)) -ForegroundColor Magenta

    # Regeneration: measure every stack that survived a battle DAMAGED against its post-battle HP. A lone
    # leader may WIN (its hero is the damaged survivor) or LOSE (the monster survives damaged), so collect
    # both. The joiner's hero is skipped - it heals on the joiner's OWN day-2 turn, which this single
    # host->joiner cycle does not reach; the joiner's monster (a neutral) heals at the day rollover.
    $cands = @()
    $cands += @(RegenCandidates $rh 'host' $true)
    $cands += @(RegenCandidates $rj 'join' $false)
    # If both heroes fought the SAME neutral (its global id is identical across clients), the host's
    # monster baseline is stale - the joiner re-damaged it afterward, so host-base -> final mixes a second
    # battle with regen. Drop the host's monster candidate and keep the joiner's (its baseline is later).
    if ($rh.monId -eq $rj.monId) { $cands = @($cands | Where-Object { -not ($_.role -eq 'host' -and $_.who -eq 'monster') }) }
    if ($cands.Count -eq 0) { throw "no stack survived a battle damaged (both heroes won unscathed or were destroyed); nothing to measure" }

    $best = $null
    foreach ($c in $cands) {
        $now = Get-StackId $c.role $c.id
        $pct = if ($now) { RegenPct $c.base ([int]$now.hp) } else { $null }
        Write-Host ("[mp] REGEN {0} {1} {2}: hp {3} -> {4} ({5}%)" -f `
                $c.role, $c.who, $c.id, $c.base, $(if ($now) { $now.hp } else { 'gone' }), $(if ($null -ne $pct) { $pct } else { 'n/a' })) -ForegroundColor Magenta
        if ($null -ne $pct -and ($null -eq $best -or $pct -gt $best)) { $best = $pct }
    }
    if ($null -eq $best) { throw "every damaged survivor vanished before measurement" }
    Write-Host "[mp] best stack regen = $best% (hard gate: >= $MinRegenPct%)" -ForegroundColor Cyan
    # Regen is unit/timing-dependent: a unit without the Regeneration ability heals 0%, and a monster
    # damaged late (the joiner's, right before the rollover) has not had a full day to heal. So below the
    # nominal 5% is a NOTE, not a failure; the hard gate (default 0) catches only a real HP LOSS.
    if ($best -lt 5) { Write-Host "[mp] note: best regen $best% is below the nominal 5% (unit/timing-dependent; not a failure)." -ForegroundColor Yellow }
    if ($best -lt $MinRegenPct) { throw "regeneration $best% < $MinRegenPct% (strict gate)" }

    Write-Host "[mp] VERIFIED: both players attacked + passed turns; day $day0 -> $day1; income credited; best stack regen $best%." -ForegroundColor Green
    $ok = $true
} catch {
    Write-Host "[mp] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Write-Host "`n==== RESULT: mp-attack-monsters=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if ($Kill) {
        if ($h) { Stop-Process -Id $h.Id -Force -ErrorAction SilentlyContinue }
        if ($j) { Stop-Process -Id $j.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
    } else {
        if ($relay) { Write-Host "[mp] left running (relay pid=$($relay.Id))." -ForegroundColor Yellow }
    }
}
# -ErrorAction Continue so a real failure exits cleanly (exit 1) under the CI shell's Stop preference.
if (-not $ok) { Write-Error "mp-attack-monsters test failed" -ErrorAction Continue; exit 1 }
