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
    [switch]$SkipChests   # go straight to the camp (beat the ~60s MP turn timer while cracking the hire)
)
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
    $jp = Start-GameClient -GameDir $GameDir -Role join

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
    Write-Host "[lt] both reached the arena. host passing turn to joiner..." -ForegroundColor Green

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

    # Enter the NEAREST camp (short walk; the far first-by-id camp wastes the turn timer) and HIRE.
    $h = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
    $unitsBefore = $h.units
    $camp = @(Get-Camps join) | Sort-Object { [Math]::Abs($_.x-$h.x)+[Math]::Abs($_.y-$h.y) } | Select-Object -First 1
    Write-Host ("[lt] nearest camp {0} @({1},{2}) unit={3}; hero @({4},{5}) mv={6} units={7}; approaching" -f $camp.id,$camp.x,$camp.y,($camp.units[0].impl),$h.x,$h.y,$h.movement,$unitsBefore) -ForegroundColor Cyan
    # Step onto the camp until DLG_MERCENARIES opens (sites are entered over a few steps).
    $merc=$false
    for ($s=0; $s -lt 6; $s++) {
        $null = Move-Stack join $hero.id $camp.x $camp.y
        Start-Sleep -Milliseconds 1800
        if ((Get-Dialog join) -eq 'DLG_MERCENARIES') { $merc=$true; break }
        $hc=@(Get-Stacks join)|Where-Object{$_.id -eq $hero.id}|Select-Object -First 1
        if (-not $hc -or $hc.movement -le 1) { break }
    }
    Write-Host "[lt] DLG_MERCENARIES opened: $merc (dialog=$(Get-Dialog join))" -ForegroundColor $(if($merc){'Green'}else{'Red'})
    if ($merc) {
        Dump-Ui join "merc-0"
        # Select the (only) hireable unit, then probe what completes the hire.
        $null = Set-ListSelection join DLG_MERCENARIES LBOX_UNIT_LIST 0; Start-Sleep -Milliseconds 1200
        Write-Host "[lt] after selecting LBOX_UNIT_LIST[0]:" -ForegroundColor DarkCyan
        Dump-Ui join "merc-sel"
        # Try the common hire triggers and watch for a state change (enroll dialog / group grows).
        foreach ($act in @('IMG_RSLOT01','LBOX_UNIT_LIST')) {
            $null = Invoke-Button join DLG_MERCENARIES $act 2>$null
        }
        Start-Sleep -Milliseconds 1200
        $d2 = Get-Dialog join
        Write-Host "[lt] dialog now: $d2" -ForegroundColor Yellow
        if ($d2 -ne 'DLG_MERCENARIES') { Dump-Ui join "after-trigger($d2)" }
    }
    $hAfter = @(Get-Stacks join) | Where-Object { $_.id -eq $hero.id } | Select-Object -First 1
    $hired = $hAfter -and ($hAfter.units -gt $unitsBefore)
    Write-Host ("[lt] hero group units {0} -> {1}  HIRED={2}" -f $unitsBefore, ($hAfter.units), $hired) -ForegroundColor $(if($hired){'Green'}else{'Yellow'})

    $ok = $merc
    Write-Host "`n==== luckytest-arena: chests=$collected, mercDialog=$merc, hired=$hired ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
} catch {
    Write-Host "[lt] FAIL: $($_.Exception.Message)" -ForegroundColor Red
}
Write-Host "[lt] LEFT RUNNING (relay pid=$($relay.Id)) for inspection / hand-off." -ForegroundColor Yellow
