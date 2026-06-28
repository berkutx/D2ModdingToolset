#requires -Version 7.0
<#
  luckytest-arena MP test (DEV). Host launches via berkut_loader (the roulette hook rolls the luckytest
  arena); joiner joins over DirectPlay. The host passes its turn so the JOINER plays: pick the 100-move
  hero, collect all reachable chests (clicking each pickup message), then enter the first camp and hire
  the single hero there. Finally hand control to the user (leave everything running).
#>
param(
    [string]$GameDir = 'C:\GOG Games\slasher_mns_2_4 - Copy',
    [string]$Mss32   = 'C:\GOG Games\DEV\_mss32_out\release-stage\mss32.dll',
    [string]$Loader  = 'C:\GOG Games\disciples2-roulette-arena-template-loader\dist\berkutx_loader.exe',
    [int]$GenWaitSec = 120,
    [switch]$SkipChests,  # go straight to the camp (beat the ~60s MP turn timer while cracking the hire)
    [switch]$BisectCamp,  # enter ONLY the first camp, fire the hire, then STOP with DLG_MERCENARIES left
                          # OPEN (no BTN_BACK) for manual inspection. Implies -SkipChests.
    [switch]$DumpCamps,   # pair, then list every own-side camp's units with their combat profile +
                          # front/back classification, and STOP (no hire). Implies -SkipChests.
    [switch]$BuildSquad   # read all own-side camps, SELECT a leader+5 squad (3 back any-reach + 3 front
                          # defenders, taking a big unit if it beats the 2 singles it displaces) and log
                          # the plan. Plan-only for now (no hire/placement yet). Implies -SkipChests.
)
if ($DumpCamps -or $BuildSquad) { $SkipChests = $true }
if ($BisectCamp) { $SkipChests = $true }
. "$PSScriptRoot\_relay.ps1"
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

# ---- run ----------------------------------------------------------------------------------------
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
$ok = $false
try {
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
        Write-Host "[lt] LEFT RUNNING (relay pid=$($relay.Id)) - camps dumped, /api/world available. Done." -ForegroundColor Yellow
        return
    }

    if ($BuildSquad) {
        # Select a leader+5 squad from the 7 own-side camps: 3 BACK any-reach (reach != 103) + 3 FRONT
        # defenders (reach == 103), best by xp (healers atkClass 6/14 deprioritized). Take a BIG unit if
        # its xp beats the two singles it would displace (it occupies a whole column). Plan-only for now.
        $cand = @(foreach ($c in $myCamps) {
            if (@($c.units).Count -eq 0) { continue }
            $u = $c.units[0]; $reach = [int]$u.reach; $cls = [int]$u.atkClass
            [pscustomobject]@{
                campId=$c.id; x=[int]$c.x; y=[int]$c.y; impl=$u.impl; reach=$reach
                xp=[int]$u.xp; hp=[int]$u.hp; big=(-not $u.small)
                front=($reach -eq 103); healer=($cls -in 6,14)
                val=([double]$u.xp * $(if ($cls -in 6,14) { 0.25 } else { 1.0 }))
            }
        })
        $hs = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
        $ldr = @($hs.slots) | Where-Object { $_.unitId -eq $hs.leaderId } | Select-Object -First 1
        $leaderFront = (-not $ldr) -or ([int]$ldr.reach -eq 103)
        $leaderBig = ($ldr -and ($ldr.small -eq $false))
        Write-Host ("[lt][SQUAD] leader {0} reach={1} -> {2}{3}" -f $hs.leaderId,$(if($ldr){[int]$ldr.reach}else{'?'}),$(if($leaderFront){'FRONT'}else{'BACK'}),$(if($leaderBig){' 2x'}else{''})) -ForegroundColor Cyan

        # Cell-budgeted: the formation is 6 cells; the leader occupies 1 (or 2 if big). Each hire takes 1
        # cell (small) or 2 (big = a whole column), so count CELLS and STOP when full (3 big units already
        # fill it). Target 3 BACK any-reach + 3 FRONT defenders; prefer singles, but take a BIG only if its
        # value beats the best back + front singles it displaces ("ценнее по сумме").
        $budget = 6 - $(if ($leaderBig) {2} else {1})
        $needFront = 3 - $(if ($leaderFront) {1} else {0})
        $needBack  = 3 - $(if ($leaderFront) {0} else {1})
        foreach ($u in $cand) { Add-Member -InputObject $u -Force -NotePropertyName cost -NotePropertyValue $(if ($u.big) {2} else {1}) }
        $picks = @()
        while ($budget -ge 1 -and ($needBack -gt 0 -or $needFront -gt 0)) {
            $avail = @($cand | Where-Object { ($picks.impl -notcontains $_.impl) -and ($_.cost -le $budget) })
            if (-not @($avail).Count) { break }
            $bb  = @($avail | Where-Object { -not $_.front -and -not $_.big } | Sort-Object val -Descending) | Select-Object -First 1
            $bf  = @($avail | Where-Object { $_.front -and -not $_.big } | Sort-Object val -Descending) | Select-Object -First 1
            $big = @($avail | Where-Object { $_.big -and (((-not $_.front) -and $needBack -gt 0) -or ($_.front -and $needFront -gt 0)) } | Sort-Object val -Descending) | Select-Object -First 1
            $pairVal = (0.0 + $(if ($bb) {$bb.val} else {0}) + $(if ($bf) {$bf.val} else {0}))
            if ($big -and ($budget -ge 2) -and ($big.val -gt $pairVal)) {
                $picks += $big; $budget -= 2
                if ($big.front) { $needFront-- } else { $needBack-- }
            } else {
                $s = $null
                if ($needBack -gt 0 -and $bb -and (-not ($needFront -gt 0 -and $bf -and $bf.val -gt $bb.val))) { $s = $bb }
                elseif ($needFront -gt 0 -and $bf) { $s = $bf }
                elseif ($needBack -gt 0 -and $bb) { $s = $bb }
                if (-not $s) { break }
                $picks += $s; $budget -= 1
                if ($s.front) { $needFront-- } else { $needBack-- }
            }
        }
        Write-Host "[lt][SQUAD] PLAN (cell-budgeted; enter ONLY these camps):" -ForegroundColor Green
        foreach ($p in @($picks)) { Write-Host ("[lt][SQUAD]   {0} {1} xp{2,-5} r{3,-3} {4}" -f $(if($p.front){'FRONT'}else{'BACK '}),$(if($p.big){'2x'}else{'1x'}),$p.xp,$p.reach,$p.campId) -ForegroundColor Gray }
        # Nearest-first visit (the camps sit in a row; greedily take the nearest unvisited from where we are).
        $visit = @(); $rest = @($picks); $px = [int]$hs.x
        while (@($rest).Count) {
            $n = @($rest | Sort-Object { [Math]::Abs([int]$_.x - $px) }) | Select-Object -First 1
            $visit += $n; $rest = @($rest | Where-Object { $_.impl -ne $n.impl }); $px = [int]$n.x
        }
        Write-Host ("[lt][SQUAD]   visit order: " + ((@($visit) | ForEach-Object { "$($_.campId)@$($_.x)" }) -join ' -> ')) -ForegroundColor Cyan

        # EXECUTE: enter each chosen camp, hire its unit (free via the 100% merchant-discount perk), then
        # place the squad front/back. Units are free and there is no turn timer, so it all fits one turn.
        $curHero = { @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1 }
        $hiredList = @()
        foreach ($p in $visit) {
            # cells used = per UNIQUE unit (a big unit can appear as 2 slot entries) weighted 2 if big.
            $used = ((@((& $curHero).slots) | Group-Object unitId) | ForEach-Object { if ($_.Group[0].isBig) { 2 } else { 1 } } | Measure-Object -Sum).Sum
            if ((6 - $used) -lt $p.cost) { Write-Host "   squad full ($used/6 cells) -> stop" -ForegroundColor Yellow; break }
            $camp = @($myCamps) | Where-Object { $_.id -eq $p.campId } | Select-Object -First 1
            if (-not $camp) { continue }
            Write-Host ("[lt][SQUAD] -> camp {0}@({1},{2}) for {3} ({4} {5})" -f $camp.id,$camp.x,$camp.y,$p.impl,$(if($p.front){'FRONT'}else{'BACK'}),$(if($p.big){'2x'}else{'1x'})) -ForegroundColor DarkCyan
            $opened = $false
            for ($s=0; $s -lt 8 -and -not $opened; $s++) {
                if (-not (Move-Stack join $hero.id $camp.x $camp.y)) { break }
                for ($w=0; $w -lt 6; $w++) { if ((Get-Dialog join) -eq 'DLG_MERCENARIES') { $opened=$true; break }; Start-Sleep -Milliseconds 400 }
                $hh = & $curHero; if ($opened -or -not $hh -or $hh.movement -le 1) { break }
            }
            if (-not $opened) { Write-Host "   camp did not open (skip)" -ForegroundColor Yellow; continue }
            $before = @(@((& $curHero).slots) | ForEach-Object { $_.unitId })
            $null = Hire-Merc join $camp.id $hero.id $p.impl
            $hired = $null
            for ($w=0; $w -lt 15 -and -not $hired; $w++) {
                Start-Sleep -Milliseconds 400
                $new = @((& $curHero).slots) | Where-Object { $_.unitId -and ($before -notcontains $_.unitId) } | Select-Object -First 1
                if ($new) { $hired = $new.unitId }
            }
            $null = Invoke-Button join DLG_MERCENARIES BTN_BACK
            Start-Sleep -Milliseconds 500
            if ($hired) { $hiredList += [pscustomobject]@{ unitId=$hired; front=$p.front; big=$p.big }; Write-Host "   hired $hired" -ForegroundColor Green }
            else { Write-Host "   hire did not land (skip)" -ForegroundColor Yellow }
        }

        # PLACE: big units already hold whole columns from the hire - leave them anchored. Arrange the
        # SMALL units (incl. a small leader) into the remaining cells: front-defenders -> even {0,2,4},
        # back any-reach -> odd {1,3,5}. Then selection-sort into place via Move-GroupUnit (never a big col).
        $sl0 = @((& $curHero).slots)
        $bigBlocked = @()
        foreach ($g in (@($sl0 | Where-Object { $_.isBig }) | Group-Object unitId)) {
            $col = ([Math]::Floor([int]$g.Group[0].position / 2)) * 2   # a big unit owns its whole column
            $bigBlocked += $col; $bigBlocked += ($col + 1)
        }
        $bigBlocked = @($bigBlocked | Select-Object -Unique)
        $availFront = @(0,2,4 | Where-Object { $bigBlocked -notcontains $_ })
        $availBack  = @(1,3,5 | Where-Object { $bigBlocked -notcontains $_ })
        $frontU = @(); $backU = @()
        if (-not $leaderBig) { if ($leaderFront) { $frontU += $hs.leaderId } else { $backU += $hs.leaderId } }
        foreach ($h in $hiredList) { if (-not $h.big) { if ($h.front) { $frontU += $h.unitId } else { $backU += $h.unitId } } }
        $desired = [ordered]@{}
        $i=0; foreach ($u in $frontU) { if ($i -lt @($availFront).Count) { $desired["$($availFront[$i])"]=$u; $i++ } }
        $i=0; foreach ($u in $backU)  { if ($i -lt @($availBack).Count)  { $desired["$($availBack[$i])"]=$u; $i++ } }
        foreach ($k in $desired.Keys) {
            $c=[int]$k; $want=$desired[$k]
            $sl=@((& $curHero).slots)
            if ((($sl | Where-Object { $_.position -eq $c } | Select-Object -First 1).unitId) -eq $want) { continue }
            $at=($sl | Where-Object { $_.unitId -eq $want } | Select-Object -First 1).position
            if ($null -eq $at) { continue }
            $null = Move-GroupUnit join $hero.id $at $c
            Start-Sleep -Milliseconds 700
        }

        Start-Sleep -Seconds 1
        foreach ($role in 'join','host') {
            $h = @(Get-Stacks $role) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
            Write-Host ("[lt][SQUAD] [{0}] final units={1}:" -f $role,$h.units) -ForegroundColor Green
            foreach ($s in (@($h.slots) | Sort-Object position)) {
                $ln = if ([int]$s.position % 2 -eq 0) { 'FRONT' } else { 'BACK ' }
                $isLdr = if ($s.unitId -eq $h.leaderId) { ' <leader>' } else { '' }
                Write-Host ("    cell {0} {1} reach={2} {3}{4}" -f $s.position,$ln,$s.reach,$s.unitId,$isLdr) -ForegroundColor Gray
            }
        }
        Write-Host "[lt] LEFT RUNNING (relay pid=$($relay.Id)) - squad built. Done." -ForegroundColor Yellow
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
        Write-Host "[lt] LEFT RUNNING (relay pid=$($relay.Id)) - DLG_MERCENARIES open, inspect manually. Done." -ForegroundColor Yellow
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
Write-Host "[lt] LEFT RUNNING (relay pid=$($relay.Id)) for inspection / hand-off." -ForegroundColor Yellow
