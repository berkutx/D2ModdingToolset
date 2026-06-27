#requires -Version 7.0
<#
.SYNOPSIS
  Single-instance example: generate a map, reach it, and read the live WORLD snapshot (player
  resources + map stacks) the in-DLL world reporter builds from the mod's own ScenarioView.

.DESCRIPTION
  Drives the random-scenario generator to a playable strategic map (the scenario-generation.ps1
  -ToMap flow), then exercises the world reporter (D2TESTDRV_WORLD): it polls GET /api/world and
  asserts the snapshot reports the local player's resources, reports the map's stacks (with their
  movement points + positions), and includes at least one neutral/enemy stack, the "nearby creatures"
  a future attack scan would target. The generator template is chosen BY NAME (default 'Diligence');
  its listbox index is resolved at runtime, so it never hardcodes a brittle number.

.EXAMPLE
  .\world-snapshot.ps1
  .\world-snapshot.ps1 -Template Fight -Keep
#>
param(
    [string]$GameDir,
    [string]$Template = 'Diligence', # generator template, selected by name (index resolved at runtime)
    [int]$MapSize = 1,               # SPIN_SIZE index (0 = smallest); larger maps carry mercenary camps + more bags
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
Write-Host "[world] relay up; launching host (template '$Template' = index $templateIndex)..." -ForegroundColor Cyan
$client = $null; $ok = $false
try {
    $client = Start-GameClient -GameDir $GameDir -Role host
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { throw "host never reached DLG_MAIN_MENU" }

    # Multiplayer setup -> the random-scenario generator (same nav as scenario-generation.ps1).
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_MULTI DLG_PROTOCOL)) { throw "no DLG_PROTOCOL" }
    if (-not (Set-ListSelection host DLG_PROTOCOL TLBOX_PROTOCOL 2)) { throw "TLBOX_PROTOCOL not set" } # 2 = TCP/IP
    Start-Sleep 1
    if (-not (Step-ToDialog host DLG_PROTOCOL BTN_CONTINUE DLG_LOAD_NEW_MULTI)) { throw "no DLG_LOAD_NEW_MULTI" }
    if (-not (Step-ToDialog host DLG_LOAD_NEW_MULTI BTN_HOST DLG_HOST)) { throw "no DLG_HOST" }
    if (-not (Step-ToDialog host DLG_HOST BTN_RANDOM_MAP DLG_RANDOM_SCENARIO_MULTI)) { throw "no generator" }

    $D = "DLG_RANDOM_SCENARIO_MULTI"
    if (-not (Set-ListSelection host $D TLBOX_TEMPLATES $templateIndex)) { throw "TLBOX_TEMPLATES not set" }
    Start-Sleep 3
    if (-not (Set-EditText host $D EDIT_NAME "AutoWorld")) { throw "EDIT_NAME not set" } # BTN_GENERATE needs a name
    Start-Sleep 3
    if (-not (Set-SpinOption host $D SPIN_SIZE $MapSize)) { throw "SPIN_SIZE not set" }
    Start-Sleep 3
    if (-not (Set-SpinOption host $D SPIN_GOAL 0)) { throw "SPIN_GOAL not set" }
    Start-Sleep 3
    if (-not (Invoke-Button host $D BTN_GENERATE)) { throw "BTN_GENERATE not found" }
    Write-Host "[world] generating ($Template)..." -ForegroundColor Cyan

    # Wait for the result (a working template reaches it well under a minute).
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 300) {
        if ($client.HasExited) { throw "the game crashed during generation" }
        $d = Get-Dialog host
        if ($d -eq 'DLG_GENERATION_RESULT') { break }
        if ($d -eq 'DLG_MESSAGE_BOX') { throw "generation errored (template $Template; DLG_MESSAGE_BOX)" }
        Start-Sleep -Milliseconds 1000
    }
    if ((Get-Dialog host) -ne 'DLG_GENERATION_RESULT') { throw "generation did not finish in time (on $(Get-Dialog host))" }

    # Accept + start solo (AI fills the rest); dismiss first-turn popups; reach the strategic map.
    if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { throw "BTN_ACCEPT not found" }
    if (-not (Wait-Dialog host DLG_LOBBY 20)) { throw "BTN_ACCEPT did not open DLG_LOBBY" }
    $popups = @{
        'DLG_SCENARIO_BRIEFING' = 'BTN_CONTINUE'; 'DLG_BEGIN_TURN' = 'BTN_OK'
        'DLG_GETINFO_BOX' = 'BTN_CLOSE'; 'DLG_MESSAGE_BOX' = 'BTN_OK'; 'DLG_EVENT_POPUP' = 'BTN_RIGHTSIDE'
    }
    $null = Invoke-Button host DLG_LOBBY BTN_OK
    $t0 = Get-Date; $onMap = $false
    while ((((Get-Date) - $t0).TotalSeconds) -lt 120) {
        $d = Get-Dialog host
        if ($d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL') { $onMap = $true; break }
        if ($popups.ContainsKey($d)) { $null = Invoke-Button host $d $popups[$d] }
        elseif ($d -eq 'DLG_LOBBY') { $null = Invoke-Button host DLG_LOBBY BTN_OK }
        Start-Sleep -Milliseconds 700
    }
    if (-not $onMap) { throw "did not reach the strategic map (stuck on $(Get-Dialog host))" }
    Write-Host "[world] reached the map; reading the world snapshot..." -ForegroundColor Green

    # The world reporter only populates once the scenario object map exists (it is a no-op in menus),
    # so poll for a non-empty snapshot after reaching the map.
    $world = $null
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 30) {
        $world = Get-World host
        if ($world -and @($world.stacks).Count -gt 0) { break }
        Start-Sleep -Milliseconds 1000
    }
    if (-not ($world -and @($world.stacks).Count -gt 0)) { throw "world snapshot never populated (no stacks reported)" }

    # (a) the local player's resources are present.
    $res = Get-Resources host
    if (-not $res) { throw "no local (self) player in the world snapshot (resources missing)" }
    Write-Host ("[world] day {0}; resources: gold={1} life={2} death={3} infernal={4} runic={5} grove={6}" -f `
            $world.day, $res.gold, $res.lifeMana, $res.deathMana, $res.infernalMana, $res.runicMana, $res.groveMana) -ForegroundColor Cyan

    # (b) the map's stacks are reported with movement points + positions; log the local player's own
    #     stack (the hero to move next), if it already has one on the map.
    $stacks = @(Get-Stacks host)
    $own = @($stacks | Where-Object { $_.relation -eq 'self' })
    Write-Host ("[world] {0} stacks ({1} own); own movement: {2}" -f $stacks.Count, $own.Count,
        (($own | ForEach-Object { "$($_.id)=$($_.movement)mp@($($_.x),$($_.y))" }) -join ', ')) -ForegroundColor Cyan

    # (c) at least one neutral/enemy stack: the "nearby creatures" a future attack scan would target.
    $targets = @($stacks | Where-Object { $_.relation -ne 'self' })
    if ($targets.Count -lt 1) { throw "no neutral/enemy stacks reported (nothing to scan/attack)" }
    Write-Host ("[world] {0} non-self stacks; e.g. id={1} relation={2} at ({3},{4}) movement={5}" -f `
            $targets.Count, $targets[0].id, $targets[0].relation, $targets[0].x, $targets[0].y, $targets[0].movement) -ForegroundColor Cyan

    # (d) treasure chests / bags: each carries a position to walk a hero onto + the item ids inside.
    #     Most templates scatter several; a tiny map may have few. Report the count + a sample.
    $bags = @(Get-Bags host)
    if ($bags.Count -lt 1) { throw "no bags/chests reported (the reporter's bag scan found nothing)" }
    Write-Host ("[world] {0} chests/bags; e.g. {1} at ({2},{3}) items={4}" -f `
            $bags.Count, $bags[0].id, $bags[0].x, $bags[0].y, @($bags[0].items).Count) -ForegroundColor Cyan

    # (e) neutral mercenary camps: each carries a hireable roster (impl id, level, unique). Not every
    #     template/size places them, so this is observe-only (logged, not a hard gate).
    $camps = @(Get-Camps host)
    if ($camps.Count -ge 1) {
        $c0 = $camps[0]; $roster = (($c0.units | ForEach-Object { "$($_.impl)(L$($_.level))" }) -join ',')
        Write-Host ("[world] {0} mercenary camps; e.g. {1} at ({2},{3}) roster=[{4}]" -f `
                $camps.Count, $c0.id, $c0.x, $c0.y, $roster) -ForegroundColor Cyan
    } else {
        Write-Host "[world] 0 mercenary camps on this map (template/size dependent)." -ForegroundColor DarkGray
    }

    $ok = $true
} catch {
    Write-Host "[world] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Write-Host "`n==== RESULT: world-snapshot=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if ($Keep) {
        Write-Host "[world] left running (relay pid=$($relay.Id), client pid=$($client.Id))." -ForegroundColor Yellow
    } else {
        if ($client) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
    }
}
# -ErrorAction Continue so a real failure exits cleanly (exit 1) under the CI shell's Stop preference.
if (-not $ok) { Write-Error "world-snapshot test failed" -ErrorAction Continue; exit 1 }
