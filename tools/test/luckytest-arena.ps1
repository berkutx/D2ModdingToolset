#requires -Version 7.0
<#
  luckytest-arena MP test (DEV). Host launches via berkut_loader (the roulette hook rolls the luckytest
  arena); joiner joins over DirectPlay. The host passes its turn so the JOINER plays: pick the 100-move
  hero, collect all reachable chests (clicking each pickup message), then enter the first camp and hire
  the single hero there. Finally hand control to the user (leave everything running).
#>
param(
    [switch]$AttachHost,  # WSL/lobby: drive a host already launched and AT THE ARENA via the existing
                          # relay (:8077); skip this script's own deploy/relay-start/launch/DirectPlay pair.
    [string]$GameDir = 'C:\GOG Games\slasher_mns_2_4 - Copy',
    [string]$Mss32   = 'C:\GOG Games\DEV\_mss32_out\release-stage\mss32.dll',
    [string]$Loader  = 'C:\GOG Games\disciples2-roulette-arena-template-loader\dist\berkutx_loader.exe',
    [int]$GenWaitSec = 120,
    [switch]$SkipChests,  # go straight to the camp (beat the ~60s MP turn timer while cracking the hire)
    [switch]$BisectCamp,  # enter ONLY the first camp, fire the hire, then STOP with DLG_MERCENARIES left
                          # OPEN (no BTN_BACK) for manual inspection. Implies -SkipChests.
    [switch]$DumpCamps,   # pair, then list every own-side camp's units with their combat profile +
                          # front/back classification, and STOP (no hire). Implies -SkipChests.
    [switch]$BuildSquad,  # walk the road ONCE collecting chests AND entering only the needed camps
                          # (interleaved nearest-first), SELECT a leader+5 squad (3 back any-reach + 3
                          # front defenders, big if it beats the 2 singles it displaces), hire, place.
    [switch]$CampProbe,   # repro: move the HOST hero to each own-half camp's EXACT entry pos+(1,3), report
                          # landed-vs-target (drift) + open/fail, then STOP for manual inspection.
    [switch]$JoinerSelf   # with -BuildSquad: drive ONLY the host (build + clash + attack + auto-battle); the
                          # JOINER is played EXTERNALLY (manual) and the harness only MONITORS its turn-pass
                          # (day increment), logging its hero/squad. The clash targets the nearest enemy.
                          # NOTE: in the battle the JOINER is fought BY THE HUMAN - the harness never toggles
                          # auto-battle on the joiner nor touches its battle/post-battle dialogs.
)
# -BuildSquad does its own interleaved chest+camp route (the bulk collect-all-first exhausts movement).
if ($DumpCamps -or $BuildSquad -or $CampProbe) { $SkipChests = $true }
if ($BisectCamp) { $SkipChests = $true }
. (Join-Path $PSScriptRoot '_relay.ps1')
$hookLog = Join-Path $GameDir 'berkutx_roulette.log'

$Dismiss = @{
    'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'; 'DLG_GETINFO_BOX' = 'BTN_CLOSE'
    'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'; 'DLG_CHAT_AI' = 'BTN_OK'
}
$fwdBtns = @('BTN_OK','BTN_CLOSE','BTN_CONTINUE','BTN_TAKEALL','BTN_TAKE','BTN_YES','BTN_RIGHTSIDE')

function Dump-Ui([string]$role, [string]$tag) {
    $ui = Get-GameUi $role
    Write-Host "  [$tag] $role dialog=$($ui.dialog)" -ForegroundColor DarkCyan
    foreach ($w in $ui.widgets) { Write-Host ("      {0,-22} {1,-8} {2}" -f $w.name, $w.type, ($w.state | ConvertTo-Json -Compress)) }
}
function OnMap([string]$role) { (Get-Dialog $role) -in @('DLG_ISO_PAL','DLG_STRATEGIC') }
function ClearPopups([string]$role, [int]$sec=20) {   # dismiss anything non-map until the map is up
    $t0=Get-Date
    while ((((Get-Date)-$t0).TotalSeconds) -lt $sec) {
        $d = Get-Dialog $role
        if ($d -in @('DLG_ISO_PAL','DLG_STRATEGIC')) { return $true }
        $btn = if ($Dismiss.ContainsKey($d)) { $Dismiss[$d] } else { $null }
        if ($btn) { $null = Invoke-Button $role $d $btn } else { foreach ($b in $fwdBtns) { if (Invoke-Button $role $d $b) { break } } }
        Start-Sleep -Milliseconds 600
    }
    return (OnMap $role)
}
function ClickAndLeave([string]$role, [string]$dlg, [string]$btn, [int]$sec) {
    $t0=Get-Date; $null=Invoke-Button $role $dlg $btn; $lf=Get-Date
    while ((((Get-Date)-$t0).TotalSeconds) -lt $sec) {
        if ((Get-Dialog $role) -ne $dlg) { return $true }
        if (((Get-Date)-$lf).TotalSeconds -ge 12) { $null=Invoke-Button $role $dlg $btn; $lf=Get-Date }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
# Close ONE non-map, non-camp popup (e.g. the "Вы нашли предмет" item-pickup scroll). Tries the known map
# button ($Dismiss), then any present fwdBtn, then the FIRST enabled button - robust to unknown dialogs whose
# close button we do not hardcode. Logs the dialog+button so unknown popups become known. '' if already clear.
function Dismiss-Popup([string]$role) {
    $ui = Get-GameUi $role
    $d = $ui.dialog
    if ($d -in @('DLG_ISO_PAL','DLG_STRATEGIC','DLG_MERCENARIES')) { return '' }
    $btn = $null
    if ($Dismiss.ContainsKey($d)) { $btn = $Dismiss[$d] }
    if (-not $btn) { foreach ($pref in $fwdBtns) { if ($ui.widgets | Where-Object { $_.name -eq $pref }) { $btn = $pref; break } } }
    if (-not $btn) { $btn = ($ui.widgets | Where-Object { $_.type -eq 'button' -and ($_.state.enabled -ne $false) } | Select-Object -First 1).name }
    if ($btn) { $null = Invoke-Button $role $d $btn; Write-Host "      [popup] $role $d -> $btn" -ForegroundColor DarkGray }
    return $d
}
# Drain popups until the map is back (or a camp dialog appears, which the caller handles).
function Clear-ToMap([string]$role, [int]$tries=5) {
    for ($i=0; $i -lt $tries; $i++) {
        $d = Get-Dialog $role
        if ($d -in @('DLG_ISO_PAL','DLG_STRATEGIC')) { return $true }
        if ($d -eq 'DLG_MERCENARIES') { return $false }
        $null = Dismiss-Popup $role
        Start-Sleep -Milliseconds 500
    }
    return ((Get-Dialog $role) -in @('DLG_ISO_PAL','DLG_STRATEGIC'))
}
# Monitor an externally-played JOINER turn: poll until it PASSES (day increments) OR it starts a battle,
# logging the joiner's hero/squad. Returns @{day; battle}. Used by -JoinerSelf (joiner is manual).
function Wait-JoinerPass([int]$fromDay, [int]$sec, [string]$label) {
    $t0 = Get-Date; $goneStreak = 0
    while ((((Get-Date)-$t0).TotalSeconds) -lt $sec) {
        $hd = Get-Dialog host
        # JOINER DISCONNECT (client crash): once it joined (human=True), a drop flips the enemy slot human
        # True->False and the engine reassigns it to AI, which pops a BLOCKING DLG_CHAT_AI taunt on the host.
        # DLG_CHAT_AI = immediate, unambiguous; human=False is confirmed across 2 polls to ignore a transient read.
        if ($hd -eq 'DLG_CHAT_AI') {
            Write-Host "[lt][monitor $label] JOINER DISCONNECTED (AI taunt DLG_CHAT_AI on host = slot reverted to AI)" -ForegroundColor Red
            return @{ day = $fromDay; battle = $false; gone = $true }
        }
        $ep = @((Get-World host).players) | Where-Object { $_.relation -eq 'enemy' } | Select-Object -First 1
        if ($ep -and -not $ep.human) { $goneStreak++ } else { $goneStreak = 0 }
        if ($goneStreak -ge 2) {
            Write-Host "[lt][monitor $label] JOINER DISCONNECTED (enemy slot human True->False, 2 polls)" -ForegroundColor Red
            return @{ day = $fromDay; battle = $false; gone = $true }
        }
        if ($hd -eq 'DLG_BATTLE_A' -or (Get-Dialog join) -eq 'DLG_BATTLE_A') {
            Write-Host "[lt][monitor $label] BATTLE started during the joiner turn" -ForegroundColor Magenta
            return @{ day = $fromDay; battle = $true; gone = $false }
        }
        $w = Get-World host; $d = if ($w) { [int]$w.day } else { $fromDay }
        $jh = @(Get-Stacks join) | Where-Object { $_.relation -eq 'self' -and [int]$_.units -ge 1 } | Sort-Object { -[int]$_.units } | Select-Object -First 1
        Write-Host ("[lt][monitor $label] day={0} join hero {1}" -f $d, $(if($jh){"$($jh.id) u$($jh.units) hp$($jh.hp) @($($jh.x),$($jh.y))"}else{'?'})) -ForegroundColor DarkGray
        if ($d -gt $fromDay) { Write-Host "[lt][monitor $label] joiner PASSED (day $fromDay -> $d)" -ForegroundColor Green; return @{ day = $d; battle = $false; gone = $false } }
        Start-Sleep -Seconds 5
    }
    Write-Host "[lt][monitor $label] timeout (day still $fromDay) - joiner did not pass" -ForegroundColor Yellow
    return @{ day = $fromDay; battle = $false; gone = $false }
}
# Joiner crashed: print the verdict and CLEAR the blocking AI-taunt / popup so the host is left free (not frozen).
function Report-JoinerGone([string]$where) {
    Write-Host "[lt][BATTLE RESULT] JOINER DISCONNECTED (client crash) at: $where - host survived the drop" -ForegroundColor Red
    for ($i = 0; $i -lt 10; $i++) {
        $d = Get-Dialog host
        if ($d -in @('DLG_STRATEGIC','DLG_ISO_PAL')) { break }
        $hit = $false
        foreach ($b in @('BTN_OK','BTN_CLOSE','BTN_CONTINUE','BTN_YES')) { if (Invoke-Button host $d $b) { $hit = $true; break } }
        Start-Sleep -Milliseconds 600
    }
    Write-Host ("[lt] host dialog after clearing the drop: {0}" -f (Get-Dialog host)) -ForegroundColor DarkGray
}
function Get-HeroById([string]$role, [string]$id) { @(Get-Stacks $role) | Where-Object { $_.id -eq $id } | Select-Object -First 1 }
function End-RoleTurn([string]$role) {
    $d = Get-Dialog $role
    if ($d -in @('DLG_STRATEGIC','DLG_ISO_PAL')) { $null = Invoke-Button $role DLG_STRATEGIC BTN_END_TURN; Start-Sleep -Seconds 2; return $true }
    return $false
}
function PassTurnToJoiner([int]$sec) {
    $t0=Get-Date; $lf=(Get-Date).AddSeconds(-10)
    while ((((Get-Date)-$t0).TotalSeconds) -lt $sec) {
        if ([bool](Get-RoleState join).sawBeginTurn) { return $true }
        $jd=Get-Dialog join; if ($jd -and $Dismiss.ContainsKey($jd)) { $null=Invoke-Button join $jd $Dismiss[$jd] }
        $hd=Get-Dialog host
        if ($hd -in @('DLG_STRATEGIC','DLG_ISO_PAL')) { if (((Get-Date)-$lf).TotalSeconds -ge 3) { $null=Invoke-Button host DLG_STRATEGIC BTN_END_TURN; $lf=Get-Date } }
        elseif ($hd) { foreach ($b in $fwdBtns) { if (Invoke-Button host $hd $b) { break } } }
        Start-Sleep -Milliseconds 600
    }
    return [bool](Get-RoleState join).sawBeginTurn
}

# Build a leader+5 squad for ONE role during ITS active turn: read that half's 7 camps, classify each
# offer by attack reach (103=Adjacent=FRONT defender; else=BACK any-reach; atkClass 6/14=healer=lower),
# cell-budget select 3 BACK + 3 FRONT (a BIG 2-slot unit only if its value beats the two singles it
# displaces), then ONE ascending-x corridor pass collecting own-half chests and entering only the chosen
# camps to hire (free), and place (bigs anchored, smalls sorted front{0,2,4}/back{1,3,5}). Role-agnostic:
# both halves are a vertical translation (chests one row below camps), so the same logic serves host+join.
function Build-RoleSquad([string]$role) {
    $hero=$null; $t0=Get-Date
    while ((((Get-Date)-$t0).TotalSeconds) -lt 30) {
        $hero = @(Get-Stacks $role) | Where-Object { $_.relation -eq 'self' -and $_.movement -ge 80 } | Select-Object -First 1
        if ($hero) { break }; Start-Sleep 1
    }
    if (-not $hero) { Write-Host "[lt][$role] no 100-move hero -> skip build" -ForegroundColor Red; return $null }
    Write-Host ("[lt][{0}] hero {1} @({2},{3}) mv={4}" -f $role,$hero.id,$hero.x,$hero.y,$hero.movement) -ForegroundColor Cyan
    $myCamps = @(Get-Camps $role) | Where-Object { [Math]::Abs([int]$_.y - [int]$hero.y) -lt 15 }
    $curHero = { @(Get-Stacks $role) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1 }

    # Corridor bounds (own-half camps + chests, with margin). Leaving the band = the game TELEPORTED the hero
    # on a bad-path move (the displacement bug) = a hard error: fail loud, never silently continue (user rule).
    $ownBags = @(Get-Bags $role | Where-Object { [Math]::Abs([int]$_.y - [int]$hero.y) -lt 15 })
    $xs = @(@($myCamps | ForEach-Object { [int]$_.x }) + @($ownBags | ForEach-Object { [int]$_.x }))
    $ys = @(@($myCamps | ForEach-Object { [int]$_.y }) + @($ownBags | ForEach-Object { [int]$_.y }))
    $xmin = (($xs | Measure-Object -Minimum).Minimum) - 4; $xmax = (($xs | Measure-Object -Maximum).Maximum) + 4
    $ymin = (($ys | Measure-Object -Minimum).Minimum) - 4; $ymax = (($ys | Measure-Object -Maximum).Maximum) + 5
    $assertPos = {
        param($ctx)
        $hh = & $curHero
        if ($hh -and ([int]$hh.x -lt $xmin -or [int]$hh.x -gt $xmax -or [int]$hh.y -lt $ymin -or [int]$hh.y -gt $ymax)) {
            throw "[$role] TELEPORT after $ctx -> hero $($hh.id) at ($($hh.x),$($hh.y)) left corridor x[$xmin..$xmax] y[$ymin..$ymax]"
        }
    }

    $cand = @(foreach ($c in $myCamps) {
        if (@($c.units).Count -eq 0) { continue }
        $u = $c.units[0]; $reach = [int]$u.reach; $cls = [int]$u.atkClass
        [pscustomobject]@{
            campId=$c.id; x=[int]$c.x; y=[int]$c.y; impl=$u.impl; reach=$reach
            xp=[int]$u.xp; hp=[int]$u.hp; big=(-not $u.small)
            front=($reach -eq 103); healer=($cls -in 6,14)
            val=([double]$u.xp * $(if ($cls -in 6,14) { 0.25 } else { 1.0 }))
            cost=$(if (-not $u.small) {2} else {1})
        }
    })
    # ROSTER (diag): every own-half camp's unit + value, so "who was available" is visible, not just the picks.
    Write-Host ("[lt][{0}][roster] {1} own-half camps (who's available):" -f $role,@($cand).Count) -ForegroundColor DarkCyan
    foreach ($rc in @($cand | Sort-Object val -Descending)) {
        Write-Host ("[lt][{0}][roster]   camp {1} {2} {3} {4} val={5} (xp{6} hp{7} reach{8}{9})" -f $role,$rc.campId,$rc.impl,$(if($rc.front){'FRONT'}else{'BACK'}),$(if($rc.big){'2x'}else{'1x'}),[int]$rc.val,$rc.xp,$rc.hp,$rc.reach,$(if($rc.healer){' HEAL'}else{''})) -ForegroundColor DarkCyan
    }
    $hs = & $curHero
    $ldr = @($hs.slots) | Where-Object { $_.unitId -eq $hs.leaderId } | Select-Object -First 1
    $leaderFront = (-not $ldr) -or ([int]$ldr.reach -eq 103)
    # TRUST the reporter's isBig flag: a big unit (incl. a big LEADER) occupies a whole COLUMN (2 cells: its
    # front cell + the back cell of that column), even when it shows as a single slot entry. If isBig ever
    # disagrees with the real footprint, that is a reporter BUG to fix at the source, not to work around here.
    $leaderBig = [bool]$ldr.isBig
    $leaderCells = if ($leaderBig) { 2 } else { 1 }
    Write-Host ("[lt][{0}] leader {1} reach={2} -> {3}{4} ({5} cell)" -f $role,$hs.leaderId,$(if($ldr){[int]$ldr.reach}else{'?'}),$(if($leaderFront){'FRONT'}else{'BACK'}),$(if($leaderBig){' 2x'}else{''}),$leaderCells) -ForegroundColor Cyan

    # Target 3 BACK any-reach (odd cells) + 3 FRONT defenders (even cells). The leader holds its cell(s): a
    # small leader = 1 cell on its line; a big leader = a whole column (1 front + 1 back). A big HIRE also
    # fills 1 front + 1 back. Fill the MORE under-quota line FIRST so backs are never starved by higher-xp
    # fronts; take a big only when it beats the front+back pair it would replace (or when only bigs remain).
    $budget = 6 - $leaderCells
    if ($leaderBig) { $needFront = 2; $needBack = 2 }
    else { $needFront = 3 - $(if ($leaderFront) {1} else {0}); $needBack = 3 - $(if ($leaderFront) {0} else {1}) }
    $picks = @()
    while ($budget -ge 1 -and ($needBack -gt 0 -or $needFront -gt 0)) {
        $avail = @($cand | Where-Object { ($picks.impl -notcontains $_.impl) -and ($_.cost -le $budget) })
        if (-not @($avail).Count) { break }
        $bb  = @($avail | Where-Object { -not $_.front -and -not $_.big } | Sort-Object val -Descending) | Select-Object -First 1
        $bf  = @($avail | Where-Object { $_.front -and -not $_.big } | Sort-Object val -Descending) | Select-Object -First 1
        $big = @($avail | Where-Object { $_.big } | Sort-Object val -Descending) | Select-Object -First 1
        $pairVal = (0.0 + $(if ($bb) {$bb.val} else {0}) + $(if ($bf) {$bf.val} else {0}))
        if ($big -and ($budget -ge 2) -and ($needFront -gt 0) -and ($needBack -gt 0) -and ($big.val -gt $pairVal)) {
            $picks += $big; $budget -= 2; $needFront--; $needBack--; continue   # a big fills a whole column
        }
        $fillBack = if ($needBack -le 0) { $false } elseif ($needFront -le 0) { $true } else { $needBack -ge $needFront }
        if ($fillBack -and $bb) { $picks += $bb; $needBack--; $budget--; continue }
        if ($needFront -gt 0 -and $bf) { $picks += $bf; $needFront--; $budget--; continue }
        if ($needBack -gt 0 -and $bb) { $picks += $bb; $needBack--; $budget--; continue }
        if ($big -and ($budget -ge 2)) { $picks += $big; $budget -= 2; if ($needBack -gt 0) { $needBack-- } else { $needFront-- }; continue }
        break
    }
    # GREEDY FILL to 6/6: if a quota line ran dry but cells remain, add the next most valuable unit of ANY type
    # (highest val that fits the remaining budget) so the formation is full, not left with a hole.
    while ($budget -ge 1) {
        $avail = @($cand | Where-Object { ($picks.impl -notcontains $_.impl) -and ($_.cost -le $budget) } | Sort-Object val -Descending)
        if (-not @($avail).Count) { break }
        $gp = @($avail)[0]; $picks += $gp; $budget -= $gp.cost
        Write-Host ("[lt][{0}][plan]   greedy-fill {1} {2} {3} val={4}" -f $role,$gp.impl,$(if($gp.front){'FRONT'}else{'BACK'}),$(if($gp.big){'2x'}else{'1x'}),[int]$gp.val) -ForegroundColor DarkMagenta
    }

    # LOG THE PLAN (diag): the leader's ACTUAL slot footprint at planning + every pick's line/size/value, so a
    # leader-cell miscount is visible at the SOURCE (not re-derived from the final formation).
    $ldrSlots = @($hs.slots | Where-Object { $_.unitId -eq $hs.leaderId })
    Write-Host ("[lt][{0}][plan] leader {1} rawslots={2} foot={10} pos=[{3}] isBig={4} -> {5}; budget-left={6} needF={7} needB={8}; {9} picks" -f $role,$hs.leaderId,@($ldrSlots).Count,(@($ldrSlots | ForEach-Object { [int]$_.position }) -join ','),$(if(@($ldrSlots)[0].isBig){'T'}else{'F'}),$(if($leaderFront){'FRONT'}else{'BACK'}),$budget,$needFront,$needBack,@($picks).Count,$leaderCells) -ForegroundColor Magenta
    foreach ($pp in @($picks)) { Write-Host ("[lt][{0}][plan]   pick camp {1} {2} {3} {4} val={5}" -f $role,$pp.campId,$pp.impl,$(if($pp.front){'FRONT'}else{'BACK'}),$(if($pp.big){'2x'}else{'1x'}),[int]$pp.val) -ForegroundColor Magenta }

    # ONE corridor pass: own-half chests + chosen camps, ascending x (see the EXECUTE note in git history).
    $route = @()
    foreach ($p in @($picks)) { $route += [pscustomobject]@{ type='camp'; id=$p.campId; x=[int]$p.x; y=[int]$p.y; pick=$p } }
    foreach ($b in @(Get-Bags $role | Where-Object { [Math]::Abs([int]$_.y - [int]$hero.y) -lt 15 })) {
        $route += [pscustomobject]@{ type='chest'; id=$b.id; x=[int]$b.x; y=[int]$b.y; pick=$null }
    }
    $route = @($route | Sort-Object { [int]$_.x })
    Write-Host ("[lt][{0}] corridor: {1} own-half chests + {2} camps, ascending x" -f $role,@($route | Where-Object { $_.type -eq 'chest' }).Count,@($route | Where-Object { $_.type -eq 'camp' }).Count) -ForegroundColor Cyan
    $hiredList = @(); $gotChests = 0
    foreach ($n in $route) {
        $h = & $curHero
        if (-not $h -or $h.movement -le 2) { Write-Host "   [$role] out of movement -> stop route" -ForegroundColor Yellow; break }
        if ($n.type -eq 'chest') {
            $null = Move-Stack $role $hero.id $n.x $n.y
            Start-Sleep -Milliseconds 1200
            $null = Clear-ToMap $role 5   # close the "Вы нашли предмет" pickup scroll (any button)
            $bagGone=$false; for ($k2=0; $k2 -lt 4 -and -not $bagGone; $k2++) { if (-not (@(Get-Bags $role) | Where-Object { $_.id -eq $n.id })) { $bagGone=$true } else { Start-Sleep -Milliseconds 300 } }
            if ($bagGone) { $gotChests++ }
            & $assertPos "chest $($n.id)"
            continue
        }
        $p = $n.pick
        $used = @((& $curHero).slots).Count   # occupied CELLS (1 slot entry per cell, big = 2 entries); NOT the isBig flag - a leader carries isBig yet holds 1 cell -> false "full"
        if ((6 - $used) -lt $p.cost) { Write-Host "   [$role] squad full ($used/6) -> skip camp $($p.campId)" -ForegroundColor DarkGray; continue }
        $camp = @($myCamps) | Where-Object { $_.id -eq $p.campId } | Select-Object -First 1
        if (-not $camp) { continue }
        Write-Host ("[lt][{0}] -> camp {1}@({2},{3}) for {4} ({5} {6})" -f $role,$camp.id,$camp.x,$camp.y,$p.impl,$(if($p.front){'FRONT'}else{'BACK'}),$(if($p.big){'2x'}else{'1x'})) -ForegroundColor DarkCyan
        # Enter the camp via the CONFIRMED constant two-step (role-agnostic offset, probed drift=0, ~3 mp/camp,
        # opens every time): (1) move to the EXACT "facing" cell pos+(1,3); (2) step one tile in to pos+(1,2)
        # -> DLG_MERCENARIES opens. Targeting camp.pos itself routes the hero AROUND the building (the harness
        # pathfinder knows only fort/ruin entrances, not merc CMidSite) = the old drain; this avoids it.
        $enX = [int]$camp.x + 1; $enY = [int]$camp.y + 3
        $trX = [int]$camp.x + 1; $trY = [int]$camp.y + 2
        $null = Move-Stack $role $hero.id $enX $enY
        Start-Sleep -Milliseconds 1000
        $null = Clear-ToMap $role 4   # close any pickup scroll BEFORE the trigger step
        & $assertPos "camp $($p.campId) entry"
        $opened = $false
        for ($s=0; $s -lt 4 -and -not $opened; $s++) {
            if (-not (Move-Stack $role $hero.id $trX $trY)) { break }
            for ($w=0; $w -lt 6; $w++) { if ((Get-Dialog $role) -eq 'DLG_MERCENARIES') { $opened=$true; break }; Start-Sleep -Milliseconds 400 }
            $hh = & $curHero; if ($opened -or -not $hh -or $hh.movement -le 1) { break }
        }
        & $assertPos "camp $($p.campId) trigger"
        if (-not $opened) {
            for ($k=0; $k -lt 3; $k++) { if ((Get-Dialog $role) -ne 'DLG_MERCENARIES') { break }; $null = Invoke-Button $role DLG_MERCENARIES BTN_BACK; Start-Sleep -Milliseconds 400 }
            Write-Host "   [$role] camp did not open (skip)" -ForegroundColor Yellow; continue
        }
        $slBefore = @((& $curHero).slots)
        $before = @($slBefore | ForEach-Object { $_.unitId })
        # DIAG: occupied cells + which 2-cell columns are FREE before the hire (a big unit needs a free column).
        $occ = @($slBefore | ForEach-Object { [int]$_.position })
        $freeCols = @(0,2,4 | Where-Object { ($occ -notcontains $_) -and ($occ -notcontains ($_+1)) })
        Write-Host ("[lt][{0}][hire] camp {1} {2} ({3} {4}); occupied=[{5}]; free 2-cell cols=[{6}]" -f $role,$camp.id,$p.impl,$(if($p.front){'FRONT'}else{'BACK'}),$(if($p.big){'2x'}else{'1x'}),($occ -join ','),(@($freeCols) -join ',')) -ForegroundColor DarkYellow
        $null = Hire-Merc $role $camp.id $hero.id $p.impl
        $hired = $null
        for ($w=0; $w -lt 15 -and -not $hired; $w++) {
            Start-Sleep -Milliseconds 400
            $new = @((& $curHero).slots) | Where-Object { $_.unitId -and ($before -notcontains $_.unitId) } | Select-Object -First 1
            if ($new) { $hired = $new.unitId }
        }
        $null = Invoke-Button $role DLG_MERCENARIES BTN_BACK
        Start-Sleep -Milliseconds 500
        if ($hired) {
            $hiredList += [pscustomobject]@{ unitId=$hired; campId=$p.campId; front=$p.front; big=$p.big }
            $landed = @((& $curHero).slots | Where-Object { $_.unitId -eq $hired } | ForEach-Object { [int]$_.position })
            Write-Host ("   [$role] hired $hired at cell(s) [{0}]" -f ($landed -join ',')) -ForegroundColor Green
        }
        else { Write-Host ("   [$role] hire did NOT land ({0} {1}); free 2-cell cols were [{2}] -> server had no room" -f $(if($p.front){'FRONT'}else{'BACK'}),$(if($p.big){'2x'}else{'1x'}),(@($freeCols) -join ',')) -ForegroundColor Yellow }
    }
    Write-Host ("[lt][{0}] route done: {1} chests, {2} hired" -f $role,$gotChests,@($hiredList).Count) -ForegroundColor Cyan

    $sl0 = @((& $curHero).slots)
    $bigBlocked = @()
    foreach ($s in @($sl0 | Where-Object { $_.isBig })) {   # TRUST isBig: any big unit (incl. a big leader shown in 1 slot) owns its whole column
        $col = ([Math]::Floor([int]$s.position / 2)) * 2
        $bigBlocked += $col; $bigBlocked += ($col + 1)
    }
    $bigBlocked = @($bigBlocked | Select-Object -Unique)
    $availFront = @(0,2,4 | Where-Object { $bigBlocked -notcontains $_ })
    $availBack  = @(1,3,5 | Where-Object { $bigBlocked -notcontains $_ })
    $frontU = @(); $backU = @()
    if (-not $leaderBig) { if ($leaderFront) { $frontU += $hs.leaderId } else { $backU += $hs.leaderId } }
    foreach ($hu in $hiredList) { if (-not $hu.big) { if ($hu.front) { $frontU += $hu.unitId } else { $backU += $hu.unitId } } }
    $desired = [ordered]@{}
    $i=0; foreach ($u in $frontU) { if ($i -lt @($availFront).Count) { $desired["$($availFront[$i])"]=$u; $i++ } }
    $i=0; foreach ($u in $backU)  { if ($i -lt @($availBack).Count)  { $desired["$($availBack[$i])"]=$u; $i++ } }
    foreach ($k in $desired.Keys) {
        $c=[int]$k; $want=$desired[$k]
        $sl=@((& $curHero).slots)
        if ((($sl | Where-Object { $_.position -eq $c } | Select-Object -First 1).unitId) -eq $want) { continue }
        $at=($sl | Where-Object { $_.unitId -eq $want } | Select-Object -First 1).position
        if ($null -eq $at) { continue }
        Write-Host ("[lt][{0}][place] move {1} from cell {2} -> {3}" -f $role,$want,$at,$c) -ForegroundColor DarkCyan
        $null = Move-GroupUnit $role $hero.id $at $c
        Start-Sleep -Milliseconds 700
    }
    # Never leave a camp dialog open: it blocks END_TURN and the turn handoff to the other role.
    for ($k=0; $k -lt 6; $k++) { if ((Get-Dialog $role) -ne 'DLG_MERCENARIES') { break }; $null = Invoke-Button $role DLG_MERCENARIES BTN_BACK; Start-Sleep -Milliseconds 400 }
    Start-Sleep -Seconds 1
    $final = & $curHero
    return [pscustomobject]@{
        role=$role; heroId=$hero.id; leaderId=$hs.leaderId
        candidates=$cand; pickedCampIds=@($picks.campId); hiredList=$hiredList
        gotChests=$gotChests; finalSlots=@($final.slots); units=$final.units
    }
}

# Print the "selected / left" table for one role: every own-half camp offer with its classification and
# whether it was TAKEN or LEFT, then the resulting formation. Russian status labels per the user's request.
function Show-SquadTable($res) {
    if (-not $res) { Write-Host "[lt][SQUAD] (no result)" -ForegroundColor Red; return }
    $picked = @($res.pickedCampIds); $hiredCamps = @($res.hiredList.campId)
    Write-Host ""
    Write-Host ("==== [{0}] герой {1}: выбрано {2}/{3} лагерей, нанято {4}, сундуков {5} ====" -f $res.role.ToUpper(),$res.heroId,@($picked).Count,@($res.candidates).Count,@($res.hiredList).Count,$res.gotChests) -ForegroundColor Green
    Write-Host ("  {0,-9} {1,-11} {2,-11} {3,-6} {4,-6} {5,-7} {6}" -f 'СТАТУС','лагерь','юнит','линия','reach','xp','разм') -ForegroundColor DarkGray
    foreach ($c in (@($res.candidates) | Sort-Object { [int]$_.x })) {
        $status = if ($hiredCamps -contains $c.campId) { 'ВЗЯТ' } elseif ($picked -contains $c.campId) { 'ВЗЯТ?' } else { 'ОСТАВЛЕН' }
        $col = if ($status -eq 'ОСТАВЛЕН') { 'DarkGray' } else { 'White' }
        $line = if ($c.front) { 'FRONT' } else { 'BACK' }
        Write-Host ("  {0,-9} {1,-11} {2,-11} {3,-6} {4,-6} {5,-7} {6}" -f $status,$c.campId,$c.impl,$line,$c.reach,$c.xp,$(if($c.big){'2x'}else{'1x'})) -ForegroundColor $col
    }
    Write-Host "  Итоговый строй:" -ForegroundColor Gray
    foreach ($s in (@($res.finalSlots) | Sort-Object position)) {
        $ln = if ([int]$s.position % 2 -eq 0) { 'FRONT' } else { 'BACK ' }
        $isLdr = if ($s.unitId -eq $res.leaderId) { ' <leader>' } else { '' }
        Write-Host ("    cell {0} {1} reach={2} {3}{4}" -f $s.position,$ln,$s.reach,$s.unitId,$isLdr) -ForegroundColor Gray
    }
}

# ---- run ----------------------------------------------------------------------------------------
$relay = $null
$ok = $false
if (-not $AttachHost) {
# Cleanup OUR tagged Copy-game windows only (spare the user's mp-test + any other folder).
Get-Process Discipl2 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' -and $_.MainWindowTitle -notmatch 'mp-test' } | Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

# Deploy + clear caches.
Copy-Item $Mss32 (Join-Path $GameDir 'mss32.dll') -Force
Copy-Item $Loader (Join-Path $GameDir 'berkutx_loader.exe') -Force
$cache = Join-Path $env:LOCALAPPDATA 'berkutx_roulette'; if (Test-Path $cache) { Remove-Item $cache -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path $hookLog) { Remove-Item $hookLog -Force -ErrorAction SilentlyContinue }
$idx = Resolve-TemplateIndex $GameDir 'luckytest'
Write-Host "[lt] luckytest index=$idx; deploying + launching..." -ForegroundColor Cyan

$relay = Start-TestRelay
} else { Write-Host "[lt] -AttachHost: driving the already-launched lobby host (at the arena) via the existing relay :8077" -ForegroundColor Cyan }
try {
  if (-not $AttachHost) {
    # HOST via the loader (roulette hook). JOINER via plain Start-GameClient (gets the arena over the net).
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName=(Join-Path $GameDir 'berkutx_loader.exe'); $psi.WorkingDirectory=$GameDir; $psi.UseShellExecute=$false
    foreach ($f in @('SKIP_INTRO','BLACKSCREEN_FIX','UI_REPORTER','WORLD','RELAY_BRIDGE')) { $psi.EnvironmentVariables["D2TESTDRV_$f"]="1" }
    $psi.EnvironmentVariables["D2TESTDRV_ROLE"]="host"
    $hp=[System.Diagnostics.Process]::Start($psi)
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { throw "host never reached DLG_MAIN_MENU" }
    if (-not ((Get-Content $hookLog -ErrorAction SilentlyContinue | Select-String 'build433B0B=1').Count -gt 0)) { throw "roulette hook did NOT install (gate failed)" }
    Write-Host "[lt] hook installed (gate ok)." -ForegroundColor Green
    Start-Sleep -Seconds 6
    $joinFlags = @('SKIP_INTRO','BLACKSCREEN_FIX','UI_REPORTER','WORLD','RELAY_BRIDGE')
    $jp = Start-GameClient -GameDir $GameDir -Role join -Flags $joinFlags

    # Pair: both to protocol, host generates luckytest, joiner joins.
    if (-not (Wait-Dialog join DLG_MAIN_MENU 90)) { throw "joiner never reached DLG_MAIN_MENU" }
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { throw "host no DLG_PROTOCOL" }
    $null=Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2; Start-Sleep 1
    if (-not (Step-ToDialog host DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { throw "host no DLG_LOAD_NEW_MULTI" }
    if (-not (Step-ToDialog join DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { throw "join no DLG_PROTOCOL" }
    $null=Set-ListSelection join DLG_PROTOCOL TLBOX_PROTOCOL 2; Start-Sleep 1
    if (-not (Step-ToDialog join DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { throw "join no DLG_LOAD_NEW_MULTI" }

    if (-not (Step-ToDialog host DLG_LOAD_NEW_MULTI BTN_HOST DLG_HOST)) { throw "host no DLG_HOST" }
    if (-not (Step-ToDialog host DLG_HOST BTN_RANDOM_MAP DLG_RANDOM_SCENARIO_MULTI)) { throw "host no generator" }
    $D='DLG_RANDOM_SCENARIO_MULTI'
    if (-not (Set-ListSelection host $D TLBOX_TEMPLATES $idx)) { throw "TLBOX_TEMPLATES" }; Start-Sleep 2
    if (-not (Set-EditText host $D EDIT_NAME "LuckyArena")) { throw "EDIT_NAME" }; Start-Sleep 2
    $null=Set-SpinOption host $D SPIN_GOAL 0; Start-Sleep 1
    if (-not (Invoke-Button host $D BTN_GENERATE)) { throw "BTN_GENERATE" }
    Write-Host "[lt] host generating luckytest..." -ForegroundColor Cyan
    $t0=Get-Date; $gen=$false
    while ((((Get-Date)-$t0).TotalSeconds) -lt $GenWaitSec) {
        $d=Get-Dialog host
        if ($d -eq 'DLG_GENERATION_RESULT') { $gen=$true; break }
        if ($d -eq 'DLG_MESSAGE_BOX') { throw "host generation errored (DLG_MESSAGE_BOX)" }
        Start-Sleep -Milliseconds 1200
    }
    if (-not $gen) { throw "host generation timed out (on $(Get-Dialog host))" }
    if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { throw "host BTN_ACCEPT" }
    if (-not (Wait-Dialog host DLG_LOBBY 25)) { throw "host no DLG_LOBBY after accept" }
    $rolled = (Get-Content $hookLog | Select-String 'ourTemplate=1').Count -gt 0
    Write-Host "[lt] host in lobby; arena rolled=$rolled. joiner joining..." -ForegroundColor Green

    if (-not (Step-ToDialog join DLG_LOAD_NEW_MULTI BTN_JOIN DLG_SESSION)) { throw "join no DLG_SESSION" }
    Start-Sleep -Seconds 3
    if (-not (Step-ToDialog join DLG_SESSION BTN_JOIN_GAME DLG_LOBBY)) { Dump-Ui join "session-list"; throw "join no DLG_LOBBY (session join failed)" }
    Start-Sleep -Milliseconds 1500
    if (-not (ClickAndLeave host DLG_LOBBY BTN_OK 45)) { throw "host start (BTN_OK) did not leave lobby" }
    if (-not (ClearPopups host 180)) { throw "host did not reach the arena map" }
    if (-not (ClickAndLeave join DLG_LOBBY BTN_OK 45)) { throw "joiner BTN_OK did not leave lobby" }
    if (-not (ClearPopups join 180)) { throw "joiner did not reach the arena map" }
    Write-Host "[lt] both reached the arena." -ForegroundColor Green
  }  # end DirectPlay launch+pair setup (skipped under -AttachHost; the host is already at the arena)

    if ($CampProbe) {
        # Repro on the HOST (its first turn): move the hero to each own-half camp's EXACT entry = pos+(1,3),
        # the offset the user confirmed constant. Report where the hero ACTUALLY landed vs that target (drift)
        # and whether the camp opened. No camp.pos fallback, no approximation - pure exact-coordinate move.
        if (-not (ClearPopups host 60)) { throw "host not on map for probe" }
        $hero=$null; $t0=Get-Date
        while ((((Get-Date)-$t0).TotalSeconds) -lt 30) { $hero = @(Get-Stacks host) | Where-Object { $_.relation -eq 'self' -and $_.movement -ge 80 } | Select-Object -First 1; if ($hero) { break }; Start-Sleep 1 }
        if (-not $hero) { throw "no host 100-move hero" }
        $myCamps = @(Get-Camps host) | Where-Object { [Math]::Abs([int]$_.y - [int]$hero.y) -lt 15 } | Sort-Object { [int]$_.x }
        Write-Host ("[PROBE] host hero {0} start @({1},{2}) mv={3}; {4} own-half camps" -f $hero.id,$hero.x,$hero.y,$hero.movement,@($myCamps).Count) -ForegroundColor Cyan
        foreach ($camp in $myCamps) {
            $ex = [int]$camp.x + 1; $ey = [int]$camp.y + 3
            $hb = @(Get-Stacks host) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            if (-not $hb -or [int]$hb.movement -le 2) { Write-Host "[PROBE] out of movement -> stop" -ForegroundColor Yellow; break }
            $r = Move-Stack host $hero.id $ex $ey
            Start-Sleep -Milliseconds 1500
            $dlg = Get-Dialog host
            $ha = @(Get-Stacks host) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            $drift = [Math]::Abs([int]$ha.x - $ex) + [Math]::Abs([int]$ha.y - $ey)
            $verdict = if ($dlg -eq 'DLG_MERCENARIES') { 'OPENED' } else { 'did NOT open' }
            Write-Host ("[PROBE] camp {0} pos=({1},{2}) entry=({3},{4}) sent={5} -> landed @({6},{7}) drift={8} dialog={9} => {10}" -f $camp.id,$camp.x,$camp.y,$ex,$ey,$r,$ha.x,$ha.y,$drift,$dlg,$verdict) -ForegroundColor $(if($verdict -eq 'OPENED'){'Green'}else{'Yellow'})
            if ($dlg -eq 'DLG_MERCENARIES') { $null = Invoke-Button host DLG_MERCENARIES BTN_BACK; Start-Sleep -Milliseconds 500 }
            $null = Clear-ToMap host 4
        }
        $hf = @(Get-Stacks host) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
        Write-Host ("[PROBE] FINAL host hero @({0},{1}) mv={2}. STOPPED - look at the host window." -f $hf.x,$hf.y,$hf.movement) -ForegroundColor Yellow
        Write-Host "[lt] LEFT RUNNING (relay pid=$(if($relay){$relay.Id}else{'ext'})) - probe done." -ForegroundColor Yellow
        return
    }

    if ($BuildSquad) {
        # BOTH halves. Host acts on its (first) turn, then hands off; the joiner acts on its turn. Same
        # role-agnostic builder for each (the two halves are a vertical translation). One table per role.
        if (-not (ClearPopups host 60)) { throw "host not on map for build" }
        Write-Host "[lt] building HOST squad (host turn)..." -ForegroundColor Green
        $hostRes = Build-RoleSquad host
        Write-Host "[lt] host done; passing turn to joiner..." -ForegroundColor Green
        if ($JoinerSelf) {
            # JOINER is an external human (berkut2), NOT a relay 'join' role. EVENT -> REACTION, ONE press:
            # wait up to 150s for the joiner to ENTER THE GAME (its enemy player slot becomes human-controlled),
            # then end the host turn with a SINGLE END_TURN. (The old PassTurnToJoiner looped, pressing END_TURN
            # every ~3s while waiting for the non-existent 'join' relay role, which re-fired on the host's
            # LATER turns and skipped them.)
            $jwait = if ($env:LB_JOINERWAIT) { [int]$env:LB_JOINERWAIT } else { 150 }
            Write-Host "[lt][JOINER-SELF] host built; waiting up to ${jwait}s for the joiner to enter the game..." -ForegroundColor Green
            $jt0 = Get-Date; $joinerIn = $false
            while ((((Get-Date)-$jt0).TotalSeconds) -lt $jwait) {
                if (@((Get-World host).players) | Where-Object { $_.relation -eq 'enemy' -and $_.human }) { $joinerIn = $true; break }
                Start-Sleep -Seconds 2
            }
            if (-not $joinerIn) { Write-Host "[lt][JOINER-SELF] no joiner within ${jwait}s -> aborting (caller kills the process)" -ForegroundColor Yellow; throw "no joiner within ${jwait}s" }
            # 10s buffer: human flips when the join REGISTERS, but the joiner's client is still loading the
            # scenario; ending the host turn instantly auto-passes the joiner's day-1 turn (he enters on day 2).
            Write-Host "[lt][JOINER-SELF] joiner in-game=True -> 10s buffer for its client to load, then ending host turn" -ForegroundColor Green
            Start-Sleep -Seconds 10
            $null = ClearPopups host 30
            $null = Invoke-Button host DLG_STRATEGIC BTN_END_TURN; Start-Sleep -Seconds 2
            # JOINER is external (manual): do NOT build/drive it - MONITOR its day-1 turn until it passes.
            $joinRes = $null
            $dBuild = if ($w0 = Get-World host) { [int]$w0.day } else { 1 }
            $bm = Wait-JoinerPass $dBuild 600 "build"
            if ($bm.gone) { Report-JoinerGone "build-monitor"; Write-Host "[lt] LEFT RUNNING (relay pid=$(if($relay){$relay.Id}else{'ext'})) - joiner disconnected. Done." -ForegroundColor Yellow; return }
            Show-SquadTable $hostRes
        } else {
            if (-not (PassTurnToJoiner 150)) { throw "host end-turn did not pass to joiner" }
            if (-not (ClearPopups join 60)) { throw "joiner not on map after turn start" }
            Write-Host "[lt] building JOINER squad (joiner turn)..." -ForegroundColor Green
            $joinRes = Build-RoleSquad join
            Show-SquadTable $hostRes
            Show-SquadTable $joinRes
        }

        # ============ POST-BUILD: cycle the day -> head-on clash -> host attacks joiner -> both auto-battle ===
        $hostHero = $hostRes.heroId
        if (-not $hostHero) { throw "missing host hero id - cannot stage the clash" }
        if ($JoinerSelf) {
            # Joiner already passed (monitored) -> host's turn now. No joiner id to drive (it is manual); the
            # clash targets the nearest ENEMY. Clear the host's new-day begin-turn/income dialogs.
            $joinHero = $null
            Write-Host "[lt] new day: clearing host begin-turn / income dialogs..." -ForegroundColor Green
            $null = ClearPopups host 60
        } else {
            $joinHero = $joinRes.heroId
            if (-not $joinHero) { throw "missing join hero id - cannot stage the clash" }
            # 1) Joiner ends its turn (host already ended its before the joiner built) -> a full day cycles.
            Write-Host "[lt] joiner ending its turn (cycle the day)..." -ForegroundColor Green
            $null = End-RoleTurn join
            # 2) New day: click through begin-turn + income dialogs (each role as its turn comes up).
            Write-Host "[lt] new day: clearing begin-turn / income dialogs..." -ForegroundColor Green
            $null = ClearPopups host 60
            $null = ClearPopups join 60
        }

        # 3) Pause 30s before the clash.
        Write-Host "[lt] pause 30s..." -ForegroundColor Cyan
        Start-Sleep -Seconds 30

        # 4) Head-on: each hero moves toward the OTHER across its turn; the HOST moves first each cycle so it
        #    delivers the attack (Move-Stack onto the enemy tile -> server starts the PvP battle = DLG_BATTLE_A).
        # Pre-clash baseline (on the map = valid; mid-battle hp/world is stale, only read AFTER the battle).
        # The winner is judged from unit losses measured map-to-map (before the clash vs after the battle).
        $preH = Get-HeroById host $hostHero
        $preHostUnits = [int]$preH.units
        if ($JoinerSelf) {
            # The joiner is the EXTERNAL human - there is NO relay 'join' role, so Get-Stacks join is $null.
            # Baseline it from the HOST's ENEMY view (fog-limited - it may be out of sight). NEVER read
            # Get-Stacks join here: an absent role returns 0 and would later force a false winner. -1 = unknown.
            $preJ = @(Get-Stacks host) | Where-Object { $_.relation -eq 'enemy' -and [int]$_.units -ge 1 } | Sort-Object { [Math]::Max([Math]::Abs([int]$_.x-[int]$preH.x),[Math]::Abs([int]$_.y-[int]$preH.y)) } | Select-Object -First 1
            if ($preJ) { $joinHero = $preJ.id }
            $preJoinUnits = if ($preJ) { [int]$preJ.units } else { -1 }
        } else {
            $preJ = Get-HeroById join $joinHero
            $preJoinUnits = [int]$preJ.units
        }
        Write-Host ("[lt] pre-clash baseline: host {0} units / join {1} (joinHero={2})" -f $preHostUnits,$(if($preJoinUnits -lt 0){'?(fog)'}else{"$preJoinUnits units"}),$(if($joinHero){$joinHero}else{'?'})) -ForegroundColor DarkGray
        Write-Host "[lt] heroes closing for a head-on clash..." -ForegroundColor Green
        $battleRole = $null
        for ($cyc = 0; $cyc -lt 8 -and -not $battleRole; $cyc++) {
            $null = ClearPopups host 40
            # The host hero can TELEPORT at turn-start (engine repositions it). Read its position until two
            # consecutive reads match, so the attack is planned from where it ACTUALLY is now (post-teleport),
            # not a stale pre-teleport coord. $jp (the target) is then selected against the FRESH host position.
            $hp = Get-HeroById host $hostHero
            for ($st = 0; $st -lt 6; $st++) {
                Start-Sleep -Milliseconds 700
                $hp2 = Get-HeroById host $hostHero
                if ($hp2 -and $hp -and [int]$hp2.x -eq [int]$hp.x -and [int]$hp2.y -eq [int]$hp.y) { $hp = $hp2; break }
                if ($hp2) { $hp = $hp2 }
            }
            if ($JoinerSelf) {
                $jp = @(Get-Stacks host) | Where-Object { $_.relation -eq 'enemy' -and [int]$_.units -ge 1 } | Sort-Object { [Math]::Max([Math]::Abs([int]$_.x-[int]$hp.x),[Math]::Abs([int]$_.y-[int]$hp.y)) } | Select-Object -First 1
            } else {
                $jp = Get-HeroById join $joinHero
            }
            if ($hp -and $jp) {
                Write-Host ("[lt][clash $cyc] HOST @({0},{1}) mv={2} -> target {3} @({4},{5})" -f $hp.x,$hp.y,$hp.movement,$jp.id,$jp.x,$jp.y) -ForegroundColor DarkCyan
                $null = Move-Stack host $hostHero ([int]$jp.x) ([int]$jp.y)
                # The host's day-2 move can be long: give ~15s for the move animation to finish, clicking
                # through any chest/event popups it walks over en route, BEFORE reading whether the move
                # landed in a battle (a too-short wait mis-reads the still-animating hero).
                for ($s = 0; $s -lt 10; $s++) {
                    Start-Sleep -Milliseconds 1500
                    $hd = Get-Dialog host
                    if ($hd -eq 'DLG_BATTLE_A') { break }
                    if ($hd -and $hd -notin @('DLG_ISO_PAL','DLG_STRATEGIC')) { foreach ($b in $fwdBtns) { if (Invoke-Button host $hd $b) { break } } }
                }
                if ((Get-Dialog host) -eq 'DLG_BATTLE_A') { $battleRole = 'host'; break }
            } elseif ($JoinerSelf) { Write-Host "[lt][clash $cyc] no enemy stack visible to the host yet" -ForegroundColor Yellow }
            $null = End-RoleTurn host
            if ($JoinerSelf) {
                # Monitor the joiner's manual turn: pass = day++ OR it attacked (battle).
                $dC = if ($wC = Get-World host) { [int]$wC.day } else { 0 }
                $mr = Wait-JoinerPass $dC 600 "clash $cyc"
                if ($mr.gone) { Report-JoinerGone "clash $cyc"; Write-Host "[lt] LEFT RUNNING (relay pid=$(if($relay){$relay.Id}else{'ext'})) - joiner disconnected. Done." -ForegroundColor Yellow; return }
                if ($mr.battle) { $battleRole = $(if ((Get-Dialog host) -eq 'DLG_BATTLE_A') { 'host' } else { 'join' }); break }
            } else {
                $null = ClearPopups join 40
                $hp = Get-HeroById host $hostHero; $jp = Get-HeroById join $joinHero
                if ($hp -and $jp) {
                    Write-Host ("[lt][clash $cyc] JOIN @({0},{1}) mv={2} -> HOST @({3},{4})" -f $jp.x,$jp.y,$jp.movement,$hp.x,$hp.y) -ForegroundColor DarkCyan
                    $null = Move-Stack join $joinHero ([int]$hp.x) ([int]$hp.y); Start-Sleep 2
                    if ((Get-Dialog join) -eq 'DLG_BATTLE_A') { $battleRole = 'join'; break }
                }
                $null = End-RoleTurn join
            }
        }

        if (-not $battleRole) {
            Write-Host "[lt] heroes did not reach a clash in 6 cycles (check the path between halves)." -ForegroundColor Yellow
        } else {
            # *** -JoinerSelf: ONLY the HOST auto-battles. The JOINER is the HUMAN player - they fight the
            # *** battle THEMSELVES, so the harness must NOT toggle its auto-battle nor touch its battle/post-
            # *** battle dialogs. (User: "я должен был сам драться - не делай так в этом тесте".)
            $battleRoles = if ($JoinerSelf) { @('host') } else { @('host','join') }
            Write-Host ("[lt] BATTLE started ($battleRole initiated). Engaging auto-battle on: {0}..." -f ($battleRoles -join '+')) -ForegroundColor Green
            # 5) Engage auto-battle. The action panel (TOG_AUTOBATTLE) is built only when it is THAT player's
            #    unit-turn (battleviewerinterfhooks.cpp:566-577) and the reporter captures only the viewer
            #    frame - so POLL and toggle whenever reachable, not one-shot. -JoinerSelf -> host only.
            $autoOn = @{}; foreach ($r in $battleRoles) { $autoOn[$r] = $false }
            $t0 = Get-Date
            while ((((Get-Date)-$t0).TotalSeconds) -lt 45 -and (@($battleRoles | Where-Object { -not $autoOn[$_] }).Count -gt 0)) {
                foreach ($r in $battleRoles) {
                    if ($autoOn[$r]) { continue }
                    $d = Get-Dialog $r
                    if ($d -in @('DLG_STRATEGIC','DLG_ISO_PAL')) { $autoOn[$r] = $true; continue }    # already resolved out
                    if ($d -eq 'DLG_BATTLE_A' -and (Invoke-Toggle $r DLG_BATTLE_A TOG_AUTOBATTLE)) { $autoOn[$r] = $true; Write-Host "   [$r] auto-battle ON" -ForegroundColor Green }
                }
                Start-Sleep -Milliseconds 700
            }
            foreach ($r in $battleRoles) { if (-not $autoOn[$r]) { Write-Host "   [$r] could not reach TOG_AUTOBATTLE (panel not up)" -ForegroundColor Yellow } }
            if ($JoinerSelf) { Write-Host "[lt] JOINER-SELF: fight the battle on the JOINER yourself - the harness will NOT touch it." -ForegroundColor Magenta }

            # 6) End the battle. The TRUE end-signal is a role leaving DLG_BATTLE_A back to the map on its OWN;
            #    worldreporter has NO battle-awareness, so stack counts read mid-battle are stale. Watch only the
            #    READABLE/harness-driven roles ($battleRoles - in -JoinerSelf the human joiner's battle is untouched
            #    and its relay role is absent anyway). BTN_CLOSE is a LAST-RESORT force-exit of a STALLED (frozen,
            #    non-ticking) battle past the grace window; it exits WITHOUT applying a result, so flag it and the
            #    verdict reports inconclusive rather than a fake win.
            Write-Host "[lt] settling the battle (waiting for the battle window to close on its own)..." -ForegroundColor Cyan
            $battleClose = @('BTN_OK','BTN_TAKEALL','BTN_TAKE','BTN_CONTINUE','BTN_RIGHTSIDE')
            $forcedClose = $false
            $t0 = Get-Date; $graceSec = 90; $maxSettle = 240
            while ((((Get-Date)-$t0).TotalSeconds) -lt $maxSettle) {
                $elapsed = ((Get-Date)-$t0).TotalSeconds
                $allMap = $true
                foreach ($r in $battleRoles) {
                    $d = Get-Dialog $r
                    if ($d -notin @('DLG_STRATEGIC','DLG_ISO_PAL')) {
                        $allMap = $false
                        if ($d -eq 'DLG_BATTLE_A') {
                            if ($elapsed -gt $graceSec) { if (Invoke-Button $r DLG_BATTLE_A BTN_CLOSE) { $forcedClose = $true } }   # force-exit only a stalled battle, and mark it
                        } else {
                            foreach ($b in $battleClose) { if (Invoke-Button $r $d $b) { break } }   # clear post-battle reward popups
                        }
                    }
                }
                if ($allMap) { break }
                Start-Sleep -Milliseconds 900
            }
            # Post-battle result - read ONLY now, after the window closed (mid-battle world is stale). Retry the
            # census so an ABSENT hero = a real removal, not a dropped poll.
            Start-Sleep -Seconds 2
            if ($JoinerSelf) {
                # The joiner is the external human (no 'join' role) and is usually under FOG from the host, so its
                # stack canNOT be read. Judge ONLY the HOST's OWN hero (authoritative, no fog on own units); report
                # the joiner's fate as unknown. NEVER infer a winner from the absent join role or a fogged enemy.
                # Distinguish a genuine removal (world READABLE, hero absent = destroyed) from a transient relay/
                # world stall (world UNREADABLE = honest UNKNOWN, never a fabricated loss).
                $postH = $null; $worldOk = $false; $t0 = Get-Date
                while ((((Get-Date)-$t0).TotalSeconds) -lt 15) {
                    $wh = Get-World host
                    if ($wh) { $worldOk = $true; $postH = @($wh.stacks) | Where-Object { $_.id -eq $hostHero } | Select-Object -First 1; if ($postH) { break } }
                    Start-Sleep 1
                }
                if (-not $worldOk) {
                    $winner = 'UNKNOWN (host world unreadable - relay/poll stall)'
                } elseif (-not $postH) {
                    # Host hero gone from a READABLE world = a CONFIRMED host loss, definitive even if the battle
                    # window had to be force-closed (the absent-hero census is real; a frozen/unfought battle would
                    # still show the PRE-battle units, not an absent hero). Confirmed death outranks force-close.
                    $winner = 'JOIN (host hero destroyed)'
                } elseif ($forcedClose) {
                    $winner = 'UNKNOWN (battle force-closed / frozen - host still alive, not resolved)'
                } else {
                    $hLost = $preHostUnits - [int]$postH.units
                    $winner = "HOST SURVIVED (lost $hLost of $preHostUnits); joiner outcome unknown - survival is not a confirmed win"
                }
                Write-Host ("[lt][BATTLE RESULT] {0}" -f $winner) -ForegroundColor Magenta
                Write-Host ("[lt][BATTLE RESULT] host: {0} units (was {1}) hp={2} @({3}) | join: fate not visible (fog / external human)" -f `
                    $(if($postH){[int]$postH.units}else{0}),$preHostUnits,$(if($postH){$postH.hp}else{0}),$(if($postH){"$($postH.x),$($postH.y)"}else{'destroyed'})) -ForegroundColor Magenta
            } else {
                # Both roles relay-driven: read each hero from its OWN view (no fog); winner = whoever destroyed the
                # other, else fewer unit losses (map-to-map vs the pre-clash baseline); log for stats.
                $postHs = @(); $postJs = @(); $t0 = Get-Date
                while ((((Get-Date)-$t0).TotalSeconds) -lt 15) { $postHs = @(Get-Stacks host); $postJs = @(Get-Stacks join); if ($postHs.Count -gt 0 -and $postJs.Count -gt 0) { break }; Start-Sleep 1 }
                $postH = $postHs | Where-Object { $_.id -eq $hostHero } | Select-Object -First 1   # host hero from the HOST view
                $postJ = $postJs | Where-Object { $_.id -eq $joinHero } | Select-Object -First 1   # join hero from the JOIN view (no fog)
                $hUnits = $(if ($postH) { [int]$postH.units } else { 0 }); $jUnits = $(if ($postJ) { [int]$postJ.units } else { 0 })
                $hLost = $preHostUnits - $hUnits; $jLost = $preJoinUnits - $jUnits
                $winner = if (-not $postH -and -not $postJ) { 'BOTH GONE' } elseif (-not $postH) { 'JOIN' } elseif (-not $postJ) { 'HOST' }
                          elseif ($jLost -gt $hLost) { 'HOST' } elseif ($hLost -gt $jLost) { 'JOIN' } else { 'DRAW/no-contest' }
                Write-Host ("[lt][BATTLE RESULT] WINNER={0}" -f $winner) -ForegroundColor Magenta
                Write-Host ("[lt][BATTLE RESULT] host: {0} units (was {1}, lost {2}) hp={3} @({4}) | join: {5} units (was {6}, lost {7}) hp={8} @({9})" -f `
                    $hUnits,$preHostUnits,$hLost,$(if($postH){$postH.hp}else{0}),$(if($postH){"$($postH.x),$($postH.y)"}else{'-'}),`
                    $jUnits,$preJoinUnits,$jLost,$(if($postJ){$postJ.hp}else{0}),$(if($postJ){"$($postJ.x),$($postJ.y)"}else{'-'})) -ForegroundColor Magenta
            }
        }

        Write-Host "[lt] LEFT RUNNING (relay pid=$(if($relay){$relay.Id}else{'ext'})) - both squads built + clash. Done." -ForegroundColor Yellow
        return
    }

    Write-Host "[lt] host passing turn to joiner..." -ForegroundColor Green
    # Host passes its turn so the JOINER plays.
    if (-not (PassTurnToJoiner 150)) { throw "host end-turn did not pass to joiner" }
    if (-not (ClearPopups join 60)) { throw "joiner not on map after turn start" }
    Write-Host "[lt] joiner active. reading arena..." -ForegroundColor Green

    # Pick the joiner's 100-move hero.
    $hero=$null; $t0=Get-Date
    while ((((Get-Date)-$t0).TotalSeconds) -lt 30) {
        $hero = @(Get-Stacks join) | Where-Object { $_.relation -eq 'self' -and $_.movement -ge 80 } | Select-Object -First 1
        if ($hero) { break }; Start-Sleep 1
    }
    if (-not $hero) { Write-Host "[lt] self stacks:"; @(Get-Stacks join) | Where-Object { $_.relation -eq 'self' } | ForEach-Object { Write-Host "   $($_.id) mv=$($_.movement) @($($_.x),$($_.y))" }; throw "no 100-move hero" }
    Write-Host ("[lt] joiner hero {0} @({1},{2}) mv={3}" -f $hero.id,$hero.x,$hero.y,$hero.movement) -ForegroundColor Cyan
    Write-Host ("[lt] arena: {0} chests, {1} camps" -f @(Get-Bags join).Count, @(Get-Camps join).Count) -ForegroundColor Cyan

    # Collect chests: greedy nearest-reachable, clicking the pickup message each time.
    $collected=0; $msgSeen=$false
    while (-not $SkipChests) {
        $h = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
        if (-not $h) { break }
        $bags = @(Get-Bags join)
        if ($bags.Count -eq 0) { break }
        $b = $bags | Sort-Object { [Math]::Abs($_.x-$h.x)+[Math]::Abs($_.y-$h.y) } | Select-Object -First 1
        $mvBefore=$h.movement
        $null = Move-Stack join $hero.id $b.x $b.y
        Start-Sleep -Milliseconds 1500
        # Click the pickup message (if any), back to the map.
        for ($k=0; $k -lt 4; $k++) {
            $d=Get-Dialog join
            if (OnMap 'join') { break }
            if (-not $msgSeen) { Dump-Ui join "chest-msg"; $msgSeen=$true }
            foreach ($btn in $fwdBtns) { if (Invoke-Button join $d $btn) { break } }
            Start-Sleep -Milliseconds 700
        }
        Start-Sleep -Milliseconds 500
        $hN = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
        $bGone = -not (@(Get-Bags join) | Where-Object { $_.id -eq $b.id })
        if ($bGone) { $collected++; Write-Host ("[lt]   chest {0} collected ({1} total); hero @({2},{3}) mv={4}" -f $b.id,$collected,$hN.x,$hN.y,$hN.movement) -ForegroundColor Green }
        elseif ($hN.movement -eq $mvBefore) { Write-Host "[lt]   stuck (no move, chest $($b.id) not collected) -> stop" -ForegroundColor Yellow; break }
        else { Write-Host ("[lt]   moved toward {0} but not on it yet (mv {1}->{2})" -f $b.id,$mvBefore,$hN.movement) -ForegroundColor DarkGray }
        if ($hN.movement -le 2) { Write-Host "[lt]   out of movement -> stop" -ForegroundColor Yellow; break }
    }
    Write-Host ("[lt] collected {0} chests; chests left={1}" -f $collected, @(Get-Bags join).Count) -ForegroundColor Cyan

    # Visit ALL of the joiner's own-side camps (its map half) and buy the lone merc at each into a free
    # slot (Hire-Merc -> testdrv worldactions::hireMerc sends CSiteBuyUnitMsg; the host applies + replicates).
    # The far host-side camps are unreachable, so filter to this half. Stop after all are visited, the
    # hero runs out of movement, or a move is rejected (the ~60s MP turn timer rolled the turn over).
    $myCamps = @(Get-Camps join) | Where-Object { [Math]::Abs($_.y - $hero.y) -lt 15 }
    Write-Host ("[lt] {0} own-side camps to visit (hero half y~{1})" -f $myCamps.Count, $hero.y) -ForegroundColor Cyan

    if ($DumpCamps) {
        # Inspection: list every own-side camp's unit with its combat profile + classification.
        # reach (category record id): 101=All 102=Any 103=Adjacent. Adjacent = hits only the nearest =
        # FRONT defender; All/Any = can hit any target = BACK. atkClass: 1=Damage 3=Paralyze 6=Heal
        # 14=Cure 24=Lower (Heal/Cure = healer = lower priority).
        function Classify($u) {
            $reach=[int]$u.reach; $cls=[int]$u.atkClass
            $line = if ($reach -eq 103) { 'FRONT' } else { 'BACK ' }
            $role = switch ($cls) { 6 {'HEALER'} 14 {'HEALER'} 3 {'PARALYZE'} default { if($reach -eq 103){'MELEE'} elseif($reach -eq 101){'AREA'} else {'RANGED'} } }
            "{0} {1} reach={2} {3,-8} xp={4,-5} hp={5,-3} dmg={6,-4} {7}" -f $line,($(if($u.small){'1x'}else{'2x'})),$reach,$role,[int]$u.xp,[int]$u.hp,[int]$u.dmg,$u.impl
        }
        $sorted = $myCamps | Sort-Object { [int]$_.x }, { [int]$_.y }
        foreach ($c in $sorted) {
            Write-Host ("[lt][DUMP] camp {0} @({1},{2}):" -f $c.id,$c.x,$c.y) -ForegroundColor Cyan
            foreach ($u in @($c.units)) { Write-Host ("    " + (Classify $u)) -ForegroundColor Gray }
        }
        Write-Host "[lt] LEFT RUNNING (relay pid=$(if($relay){$relay.Id}else{'ext'})) - camps dumped, /api/world available. Done." -ForegroundColor Yellow
        return
    }

    if ($BisectCamp) {
        # BISECTION: first camp only -> confirm entry -> attempt hire -> STOP, leaving DLG_MERCENARIES
        # OPEN (no BTN_BACK) for manual inspection. No auto-dismiss runs (D2TESTDRV_AUTODISMISS is off).
        $h = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
        $camp = $myCamps | Sort-Object { [Math]::Abs($_.x-$h.x)+[Math]::Abs($_.y-$h.y) } | Select-Object -First 1
        Write-Host ("[lt][BISECT] first camp {0} @({1},{2}); approaching" -f $camp.id,$camp.x,$camp.y) -ForegroundColor Cyan
        $opened = $false
        for ($s=0; $s -lt 8 -and -not $opened; $s++) {
            if (-not (Move-Stack join $hero.id $camp.x $camp.y)) { Write-Host "[lt][BISECT] move rejected (turn over?)" -ForegroundColor Yellow; break }
            for ($w=0; $w -lt 6; $w++) { if ((Get-Dialog join) -eq 'DLG_MERCENARIES') { $opened=$true; break }; Start-Sleep -Milliseconds 500 }
            $hc = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            Write-Host ("[lt][BISECT] step {0}: hero @({1},{2}) mv={3} dialog={4}" -f $s,$hc.x,$hc.y,$hc.movement,(Get-Dialog join)) -ForegroundColor DarkGray
            if ($opened -or -not $hc -or $hc.movement -le 1) { break }
        }
        if ($opened) {
            Write-Host "[lt][BISECT] CONFIRMED: DLG_MERCENARIES is OPEN. NOT closing it." -ForegroundColor Green
            $h0 = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            $before = @(@($h0.slots) | ForEach-Object { $_.unitId })   # unit ids before the hire
            $u0 = if ($camp.units.Count -gt 0) { $camp.units[0] } else { $null }
            if ($u0) {
                $r = Hire-Merc join $camp.id $hero.id $u0.impl
                Write-Host ("[lt][BISECT] hire (CSiteBuyUnitMsg): camp {0} unit {1} -> sent={2}" -f $camp.id,$u0.impl,$r) -ForegroundColor Magenta
            }
            # Find the hired unit by diffing slots (it may land at slot 1, or slot 2 for a big 2-slot unit),
            # waiting for the host to generate + broadcast it back.
            $hired = $null; $hpos = -1
            for ($w=0; $w -lt 15 -and -not $hired; $w++) {
                Start-Sleep -Milliseconds 400
                $sl = @((@(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1).slots)
                $new = $sl | Where-Object { $_.unitId -and ($before -notcontains $_.unitId) } | Select-Object -First 1
                if ($new) { $hired = $new.unitId; $hpos = $new.position }
            }
            Write-Host ("[lt][BISECT] after hire: hired unit {0} at slot {1}" -f $hired,$hpos) -ForegroundColor Cyan
            if ($hired) {
                # MOVE (CStackSwapUnitMsg): slide the hired unit to the first free slot. Empty target = move;
                # occupied = swap. Big units are column-anchored so a move may no-op - we log either way.
                $sl = @((@(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1).slots)
                $occ = @($sl | ForEach-Object { $_.position })
                $dst = (0..5 | Where-Object { $occ -notcontains $_ -and $_ -ne $hpos } | Select-Object -First 1)
                if ($null -ne $dst) {
                    $mr = Move-GroupUnit join $hero.id $hpos $dst
                    Write-Host ("[lt][BISECT] move (CStackSwapUnitMsg): slot {0} -> {1} -> sent={2}" -f $hpos,$dst,$mr) -ForegroundColor Magenta
                    Start-Sleep -Seconds 2
                    foreach ($role in 'join','host') {
                        $ds = ((@((@(Get-Stacks $role) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1).slots) | Sort-Object position | ForEach-Object { "{0}={1}" -f $_.position,$_.unitId }) -join ' ')
                        Write-Host ("[lt][BISECT] [{0}] slots: {1}" -f $role,$ds) -ForegroundColor Green
                    }
                }
                # DISMISS (CStackDismissUnitMsg): drop the hired unit, then verify the removal replicates -
                # BOTH roles drop back to just the leader. The leader can never be dismissed by this message.
                $dr = Dismiss-Unit join $hero.id $hired
                Write-Host ("[lt][BISECT] dismiss (CStackDismissUnitMsg): unit {0} -> sent={1}" -f $hired,$dr) -ForegroundColor Magenta
                Start-Sleep -Seconds 2
                foreach ($role in 'join','host') {
                    $hs = @(Get-Stacks $role) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
                    $ds = ((@($hs.slots) | Sort-Object position | ForEach-Object { "{0}={1}" -f $_.position,$_.unitId }) -join ' ')
                    Write-Host ("[lt][BISECT] [{0}] after dismiss: units={1} slots: {2}" -f $role,$hs.units,$ds) -ForegroundColor Green
                }
            }
            $hc = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            Write-Host ("[lt][BISECT] hero units now {0}; dialog still {1}. STOPPED for manual inspection." -f $hc.units,(Get-Dialog join)) -ForegroundColor Cyan
        } else {
            Write-Host ("[lt][BISECT] DLG_MERCENARIES did NOT open (dialog={0})" -f (Get-Dialog join)) -ForegroundColor Red
        }
        Write-Host "[lt] LEFT RUNNING (relay pid=$(if($relay){$relay.Id}else{'ext'})) - DLG_MERCENARIES open, inspect manually. Done." -ForegroundColor Yellow
        return
    }

    $entered = 0; $hired = 0; $seen = @{}
    while ($true) {
        $h = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
        if (-not $h) { Write-Host "[lt] hero gone -> stop" -ForegroundColor Yellow; break }
        if ($h.movement -le 2) { Write-Host "[lt] out of movement -> stop" -ForegroundColor Yellow; break }
        $camp = $myCamps | Where-Object { -not $seen[$_.id] } | Sort-Object { [Math]::Abs($_.x-$h.x)+[Math]::Abs($_.y-$h.y) } | Select-Object -First 1
        if (-not $camp) { Write-Host "[lt] all own-side camps visited" -ForegroundColor Green; break }
        $seen[$camp.id] = $true
        # Walk onto the camp; after each step POLL ~2.5s for DLG_MERCENARIES (the entry confirmation),
        # so the detection is robust (not a single check) and the open dialog is HELD long enough to see.
        Write-Host ("[lt]   -> camp {0} @({1},{2}); approaching" -f $camp.id, $camp.x, $camp.y) -ForegroundColor DarkCyan
        $opened = $false; $rejected = $false
        for ($s=0; $s -lt 6 -and -not $opened; $s++) {
            if (-not (Move-Stack join $hero.id $camp.x $camp.y)) { $rejected=$true; break }
            for ($w=0; $w -lt 5; $w++) { if ((Get-Dialog join) -eq 'DLG_MERCENARIES') { $opened=$true; break }; Start-Sleep -Milliseconds 500 }
            if ($opened) { break }
            $hc = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            if (-not $hc -or $hc.movement -le 1) { break }
        }
        if ($rejected) { Write-Host "[lt]   move rejected (turn over?) -> stop" -ForegroundColor Yellow; break }
        if ($opened) {
            Write-Host ("[lt]   CONFIRMED entered camp {0}: DLG_MERCENARIES is open (holding ~3s)" -f $camp.id) -ForegroundColor Green
            Start-Sleep -Seconds 3   # keep the merc dialog visible so the user can see the entry
        } else {
            Write-Host ("[lt]   camp {0}: DLG_MERCENARIES did NOT open (current dialog: {1})" -f $camp.id, (Get-Dialog join)) -ForegroundColor Red
        }
        if ($opened) {
            $entered++
            # Buy the merc WHILE the camp dialog is open (the faithful drag-drop order), then let the host
            # apply + replicate before closing the dialog.
            $u0 = if ($camp.units.Count -gt 0) { $camp.units[0] } else { $null }
            $unit = if ($u0) { $u0.impl } else { '' }
            $r = if ($unit) { Hire-Merc join $camp.id $hero.id $unit } else { $false }
            if ($r) { $hired++ }
            Start-Sleep -Milliseconds 800
            $null = Invoke-Button join DLG_MERCENARIES BTN_BACK   # close the camp dialog
            Start-Sleep -Milliseconds 600
            $hc = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            Write-Host ("[lt]   camp {0} @({1},{2}) ENTERED; hire {3} -> sent={4}; hero now units={5} mv={6}" -f $camp.id,$camp.x,$camp.y,$unit,$r,$hc.units,$hc.movement) -ForegroundColor $(if($r){'Green'}else{'DarkGray'})
        } else {
            Write-Host ("[lt]   camp {0} @({1},{2}) not reached (skipped)" -f $camp.id,$camp.x,$camp.y) -ForegroundColor Yellow
        }
    }
    # Final group state for the manual check.
    $hF = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
    Write-Host ("[lt] DONE: entered {0} camps, hired {1} units; hero {2} now has {3} units. Slots:" -f $entered,$hired,$hero.id,$hF.units) -ForegroundColor Cyan
    @((Get-World join).stacks) | Where-Object { $_.id -eq $hero.id } | ForEach-Object { @($_.slots) | ForEach-Object { Write-Host ("      slot {0} = {1} (big={2})" -f $_.position,$_.unitId,$_.isBig) } }

    $ok = ($entered -ge 1)
    Write-Host "`n==== luckytest-arena: chests=$collected, camps entered=$entered, hired=$hired ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
} catch {
    Write-Host "[lt] FAIL: $($_.Exception.Message)" -ForegroundColor Red
}
Write-Host "[lt] LEFT RUNNING (relay pid=$(if($relay){$relay.Id}else{'ext'})) for inspection / hand-off." -ForegroundColor Yellow
