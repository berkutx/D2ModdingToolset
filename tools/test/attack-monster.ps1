#requires -Version 7.0
<#
.SYNOPSIS
  Battle test template: a hero leaves its capital, approaches a free neutral monster, attacks it,
  the battle is fought on auto-battle, the post-battle dialogs are dismissed, and the result is
  verified by COMPARING the stacks before and after (HP / unit count / position).

.DESCRIPTION
  Single-instance (skirmish vs AI) end-to-end, the canonical "approach + attack + auto-battle" flow.
  Every step uses the same native paths a real player drives, no input emulation:

    1. Generate a random skirmish and reach the strategic map (Single Player -> New Skirmish ->
       Random Map). Skirmish is the single-instance, sequential-turn mode.
    2. Exit the garrison. A hero starting INSIDE its capital reports the fort ANCHOR, not its real
       tile, and exiting is a free 0-cost step the game applies specially, so Move-Stack to anchor+5
       performs that exact exit (worldactions detects insideId and replicates the real exit move).
    3. Pick the nearest FREE neutral monster (relation == neutral, inside == $false). Filtering on
       neutral specifically avoids an enemy AI stack; stacks INSIDE a fort/city/village are garrisons
       (reported at the fort anchor, fought as a siege), so the monster test skips them.
    4. Snapshot the hero and the monster BEFORE: position, unit count, total HP.
    5. Attack: Move-Stack the hero onto the monster's tile. moveStack routes adjacent and sets the
       message `end` to the monster tile, so the server starts the battle (DLG_BATTLE_A/B), exactly
       like clicking an enemy stack.
    6. Auto-battle: Invoke-Toggle on the exact DLG_BATTLE_A/B viewer -> the game's AI plays every round
       (not an instant resolve). The battle ends on its own.
    7. Dismiss every post-battle dialog (result screen, then any reward / dropped-item dialogs) by
       clicking the forward button until the strategic map is back.
    8. Verify by reading ONE clean post-battle snapshot (retry until the GET succeeds, so an absent
       stack means a real removal and not a dropped poll), then comparing:
         - the fight resolved: the monster is gone / damaged, the hero is damaged, or the hero itself
           was destroyed (a lost battle, expected when a lone starting leader hits a strong neutral),
         - the hero approached: if it still exists, its position differs from the post-exit tile;
           the only exceptions are an already-adjacent monster (rare) or a destroyed hero.

.EXAMPLE
  .\attack-monster.ps1
  .\attack-monster.ps1 -Template Fight -Keep
#>
param(
    [string]$GameDir,
    [string]$Template = 'Diligence', # generator template, selected by name (index resolved at runtime)
    [switch]$Keep
)

. "$PSScriptRoot\_relay.ps1"
. "$PSScriptRoot\_battle.ps1"   # Invoke-HeroAttack + Test-AttackResult (the shared battle flow)
$GameDir = Resolve-GameDir $GameDir
$templateIndex = Resolve-TemplateIndex $GameDir $Template

# Clean slate without a blanket kill: only our tagged window + a stale dplaysvr.
Get-Process Discipl2 -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -match '\[(HOST|CLIENT)\]' } |
    Stop-Process -Force -ErrorAction SilentlyContinue
Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1000

$relay = Start-TestRelay
Write-Host "[attack] relay up; launching host (template '$Template' = index $templateIndex)..." -ForegroundColor Cyan
$client = $null; $ok = $false
try {
    $client = Start-GameClient -GameDir $GameDir -Role host
    if (-not (Wait-Dialog host DLG_MAIN_MENU 90)) { throw "host never reached DLG_MAIN_MENU" }

    # Single-player skirmish -> the random-scenario generator.
    if (-not (Step-ToDialog host DLG_MAIN_MENU BTN_SINGLE DLG_SINGLE_PLAYER)) { throw "no DLG_SINGLE_PLAYER" }
    if (-not (Step-ToDialog host DLG_SINGLE_PLAYER BTN_NEW_SKIRMISH DLG_CHOOSE_SKIRMISH)) { throw "no DLG_CHOOSE_SKIRMISH" }
    if (-not (Step-ToDialog host DLG_CHOOSE_SKIRMISH BTN_RANDOM_MAP DLG_RANDOM_SCENARIO_SINGLE)) { throw "no generator" }

    $D = "DLG_RANDOM_SCENARIO_SINGLE"
    if (-not (Set-ListSelection host $D TLBOX_TEMPLATES $templateIndex)) { throw "TLBOX_TEMPLATES not set" }
    Start-Sleep 3
    if (-not (Set-SpinOption host $D SPIN_SIZE 0)) { throw "SPIN_SIZE not set" } # smallest map = fast
    Start-Sleep 3
    if (-not (Invoke-Button host $D BTN_GENERATE)) { throw "BTN_GENERATE not found" }
    Write-Host "[attack] generating ($Template)..." -ForegroundColor Cyan

    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 300) {
        if ($client.HasExited) { throw "the game crashed during generation" }
        $d = Get-Dialog host
        if ($d -eq 'DLG_GENERATION_RESULT') { break }
        if ($d -eq 'DLG_MESSAGE_BOX') { throw "generation errored (template $Template; DLG_MESSAGE_BOX)" }
        Start-Sleep -Milliseconds 1000
    }
    if ((Get-Dialog host) -ne 'DLG_GENERATION_RESULT') { throw "generation did not finish in time (on $(Get-Dialog host))" }

    # Accept and drive through race/lord selection + first-turn popups to the strategic map.
    if (-not (Invoke-Button host DLG_GENERATION_RESULT BTN_ACCEPT)) { throw "BTN_ACCEPT not found" }
    $fwd = @('BTN_CONTINUE', 'BTN_OK', 'BTN_ACCEPT', 'BTN_RIGHTSIDE', 'BTN_CLOSE')
    $t0 = Get-Date; $onMap = $false; $last = ''; $lastChange = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt 180) {
        if ($client.HasExited) { throw "the game crashed reaching the map" }
        $d = Get-Dialog host
        if ($d -eq 'DLG_STRATEGIC' -or $d -eq 'DLG_ISO_PAL') { $onMap = $true; break }
        if ($d -ne $last) { $last = $d; $lastChange = Get-Date }
        elseif ((((Get-Date) - $lastChange).TotalSeconds) -gt 25) { throw "stuck on $d reaching the map" }
        if ($d) { foreach ($b in $fwd) { if (Invoke-Button host $d $b) { break } } }
        Start-Sleep -Milliseconds 800
    }
    if (-not $onMap) { throw "did not reach the strategic map" }
    Write-Host "[attack] reached the map." -ForegroundColor Green

    # Steps 2-8 (exit garrison, approach + attack the nearest free neutral, auto-battle, dismiss the
    # post-battle dialogs, read one clean post-battle snapshot) are the shared battle flow.
    $r = Invoke-HeroAttack -Role host -Client $client
    if (-not $r.ok) { throw $r.reason }
    Write-Host ("[attack] hero  : {0}" -f $(if ($r.after.heroGone) { 'DESTROYED (gone from census)' } else { "($($r.after.heroX),$($r.after.heroY)) units=$($r.after.heroUnits) hp=$($r.after.heroHp) (was ($($r.ex),$($r.ey)) units=$($r.before.heroUnits) hp=$($r.before.heroHp))" })) -ForegroundColor Cyan
    Write-Host ("[attack] monster: {0}" -f $(if ($r.after.monGone) { 'DEFEATED (gone from census)' } else { "units=$($r.after.monUnits) hp=$($r.after.monHp) (was units=$($r.before.monUnits) hp=$($r.before.monHp))" })) -ForegroundColor Cyan

    $v = Test-AttackResult $r
    if (-not $v.ok) { throw $v.note }
    Write-Host "[attack] VERIFIED: battle resolved ($($v.note))." -ForegroundColor Green
    $ok = $true
} catch {
    Write-Host "[attack] FAIL: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Write-Host "`n==== RESULT: attack-monster=$ok ====" -ForegroundColor $(if ($ok) { 'Green' } else { 'Yellow' })
    if ($Keep) {
        Write-Host "[attack] left running (relay pid=$($relay.Id), client pid=$($client.Id))." -ForegroundColor Yellow
    } else {
        if ($client) { Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue }
        if ($relay) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
        Stop-Process -Name dplaysvr -Force -ErrorAction SilentlyContinue
    }
}
# -ErrorAction Continue so a real failure exits cleanly (exit 1) under the CI shell's Stop preference.
if (-not $ok) { Write-Error "attack-monster test failed" -ErrorAction Continue; exit 1 }
