#requires -Version 7.0
# Offline policies only: synthetic observations, no native/game/network operations.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$helper = Join-Path $PSScriptRoot '../tools/test/simturns-lobby-gameplay.ps1'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($helper,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
. $helper
$script:checks=0
function Check([bool]$condition,[string]$message) {
    $script:checks++; if (-not $condition) { throw $message }
}
function Reject([scriptblock]$operation,[string]$pattern) {
    $caught=$false; $detail='no exception'
    try { & $operation | Out-Null } catch { $detail=$_.Exception.Message; $caught=$detail -like $pattern }
    Check $caught "Expected rejection $pattern; got: $detail"
}
function Copy-Observation($value) { $value | ConvertTo-Json -Depth 20 | ConvertFrom-Json }
function Worlds {
    $players=@(
        [pscustomobject]@{id='0x00010001';human=$true;relation='self'},
        [pscustomobject]@{id='0x00010002';human=$true;relation='enemy'},
        [pscustomobject]@{id='0x00010003';human=$false;relation='neutral'}
    )
    $stacks=@(
        [pscustomobject]@{id='0x00020001';owner='0x00010001';relation='self';x=2;y=2;inside=$false;movement=24;units=2;hp=180;leaderId='0x00030001';unitStates=@([pscustomobject]@{id='0x00030001';hp=100},[pscustomobject]@{id='0x00030002';hp=80})},
        [pscustomobject]@{id='0x00020002';owner='0x00010002';relation='enemy';x=20;y=20;inside=$false;movement=24;units=1;hp=90;leaderId='0x00030003';unitStates=@([pscustomobject]@{id='0x00030003';hp=90})},
        [pscustomobject]@{id='0x00020003';owner='0x00010003';relation='neutral';x=4;y=4;inside=$false;movement=0;units=1;hp=70;leaderId='0x00030004';unitStates=@([pscustomobject]@{id='0x00030004';hp=70})},
        [pscustomobject]@{id='0x00020004';owner='0x00010003';relation='neutral';x=18;y=18;inside=$false;movement=0;units=1;hp=60;leaderId='0x00030005';unitStates=@([pscustomobject]@{id='0x00030005';hp=60})},
        [pscustomobject]@{id='0x00020005';owner='0x00010003';relation='neutral';x=4;y=3;inside=$false;movement=0;units=1;hp=50;leaderId='0x00030006';unitStates=@([pscustomobject]@{id='0x00030006';hp=50})}
    )
    $h=[pscustomobject]@{role='host';worldSeq=4;day=1;strategicActionReady=$false;players=$players;stacks=$stacks}
    $j=Copy-Observation $h; $j.role='join'; $j.worldSeq=6
    $j.players[0].relation='enemy'; $j.players[1].relation='self'
    $j.stacks[0].relation='enemy'; $j.stacks[1].relation='self'
    [pscustomobject]@{host=$h;join=$j}
}
function BattleUi([string]$Layout='DLG_BATTLE_A') {
    [pscustomobject]@{dialog=$Layout;dialogReady=$true;dialogAppearance=11;dialogInstance=11;uiSeq=30
        targets=@([pscustomobject]@{dialog=$Layout;instance=4096;widgets=@([pscustomobject]@{name='BTN_DEFEND';type='button'})})}
}
function MergeLines([string]$role) {
    $lines=@(
        '[simturns] prepared merge transaction 17 at stock day 3',
        "[simturns] accepted $role natural stock BeginTurn (frame=56, addressee=0x0, sequence=20, active=0x00010001, mergeDay=3)",
        '[simturns] natural merge BeginTurn applied and drained (actionId=17, day=3)'
    )
    if ($role -ceq 'host') { $lines += '[simturns] host executed merge transaction 17 via 0x420FFA' }
    $lines += '[simturns] relay released stock turns (actionId=17, day=3)'
    $lines
}

$w=Worlds; $before=$w | ConvertTo-Json -Depth 20 -Compress
$plan=Get-LobbyBattlePlan $w.host $w.join
Check ($plan.host.hero.id -eq '0x00020001' -and $plan.join.hero.id -eq '0x00020002') 'Wrong live heroes'
Check ($plan.roles.host.target.id -eq '0x00020005' -and $plan.roles.join.target.id -eq '0x00020004') 'Nearest then weakest target rank failed'
Check (-not $plan.routeValidated -and -not $plan.requiresExit) 'Plan claimed a validated path'
Check ($plan.host.rankedCandidates.Count -eq 3 -and $plan.host.distance -eq 2) 'Bounded raw rank observations lost'
Check (($w | ConvertTo-Json -Depth 20 -Compress) -ceq $before) 'Plan mutated input observations'
Check ((Get-LobbyBattlePlan $w.host $w.join -HostAfter 3 -JoinAfter 5).day -eq 1) 'Fresh watermarks rejected'
Reject { Get-LobbyBattlePlan $w.host $w.join -HostAfter 4 } '*stale*'
$bad=Copy-Observation $w; $bad.join.day=2
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*same day*'
$bad=Copy-Observation $w; $bad.host.stacks[0].movement=0
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*no own mobile hero*'
$bad=Copy-Observation $w; $bad.host.stacks[0].units=0
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*no own mobile hero*'
$bad=Copy-Observation $w; $bad.host.stacks[0].unitStates[0].hp=0
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*no own mobile hero*'
$bad=Copy-Observation $w; $bad.host.stacks[0].movement=$true
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*not an integer*'
$bad=Copy-Observation $w; $bad.host.stacks[0].unitStates=@($bad.host.stacks[0].unitStates[1])
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*no own mobile hero*'
$bad=Copy-Observation $w; $bad.host.stacks += $bad.host.stacks[0]
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*repeats stack*'
$bad=Copy-Observation $w; $bad.join.stacks[0].x=3
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*disagrees across world*'
$bad=Copy-Observation $w; $bad.host.players[0].human='true'
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*one local human*'
$bad=Copy-Observation $w; $bad.host.role='join'
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*different role*'
$bad=Copy-Observation $w
foreach ($role in @('host','join')) { $bad.$role.stacks=@($bad.$role.stacks | Where-Object { $_.id -in @('0x00020001','0x00020002','0x00020003') }) }
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*two distinct neutral*'
$bad=Copy-Observation $w
foreach ($role in @('host','join')) { $bad.$role.stacks[0].inside=$true }
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*no observed native capital exit*'
$exit=[pscustomobject]@{kind='observed-5x5-capital';fortId='0x00040001';anchorX=2;anchorY=2;innerX=4;innerY=6;x=7;y=6;sizeX=5;sizeY=5}
$bad.host.stacks[0] | Add-Member -NotePropertyName capitalExit -NotePropertyValue $exit
$insidePlan=Get-LobbyBattlePlan $bad.host $bad.join
Check ($insidePlan.requiresExit -and $insidePlan.host.needsExit -and $insidePlan.host.hero.capitalExit.x -eq 7) 'Observed exit proof lost'
$exit.anchorX=3
Reject { Get-LobbyBattlePlan $bad.host $bad.join } '*does not match*'

$h=BattleUi; $j=BattleUi; $j.targets[0].instance=8192; $a=Copy-Observation $h; $a.uiSeq=31
$t0=[DateTimeOffset]'2026-09-27T10:00:00Z'; $t1=$t0.AddMilliseconds(100); $t2=$t1.AddMilliseconds(100)
$overlap=Assert-LobbyBattleOverlap $h $j $a $t0 $t1 $t2
Check ($overlap.passed -and $overlap.observationBracketMs -eq 200 -and -not $overlap.exactEngineOverlapDurationKnown) 'Live overlap bracket failed or overstated duration'
$a.uiSeq=30
Check ((Assert-LobbyBattleOverlap $h $j $a $t0 $t1 $t2).passed) 'Unchanged UI revision rejected despite fresh read'
$bad=Copy-Observation $h; $bad.dialogReady='true'
Reject { Get-LobbyLiveBattleProof $bad host } '*ready live*'
$bad=Copy-Observation $h; $bad.targets[0].widgets += [pscustomobject]@{name='BTN_CLOSE';type='button'}
Reject { Get-LobbyLiveBattleProof $bad host } '*result controls*'
$bad=Copy-Observation $h; $bad.targets[0].widgets[0].type='text'
Reject { Get-LobbyLiveBattleProof $bad host } '*typed live controls*'
$bad=Copy-Observation $h; $bad.targets += $bad.targets[0]
Reject { Get-LobbyLiveBattleProof $bad host } '*exactly one native owner*'
$bad=Copy-Observation $a; $bad.targets[0].instance=9999
Reject { Assert-LobbyBattleOverlap $h $j $bad $t0 $t1 $t2 } '*changed identity*'
$bad=Copy-Observation $a; $bad.uiSeq=29
Reject { Assert-LobbyBattleOverlap $h $j $bad $t0 $t1 $t2 } '*changed identity*'
Reject { Assert-LobbyBattleOverlap $h $j $a $t0 $t0 $t2 } '*positive ordered time*'
Reject { Assert-LobbyBattleOverlap $h $j $a $t2 $t1 $t0 } '*positive ordered time*'

# Both native battle layouts are valid independently. Layout is part of the
# host identity bracket, but the peer can legitimately use the other layout.
foreach ($layout in @('DLG_BATTLE_A','DLG_BATTLE_B')) {
    foreach ($role in @('host','join')) {
        $proof=Get-LobbyLiveBattleProof (BattleUi $layout) $role
        Check ($proof.live -and $proof.dialog -ceq $layout -and $proof.role -ceq $role -and $proof.owner -eq 4096) "Exact $role $layout proof lost identity"
    }
    $otherLayout=if ($layout -ceq 'DLG_BATTLE_A') { 'DLG_BATTLE_B' } else { 'DLG_BATTLE_A' }
    $bad=BattleUi $layout; $bad.targets[0].dialog=$otherLayout
    Reject { Get-LobbyLiveBattleProof $bad host } '*exactly one native owner*'
    $bad=BattleUi $layout; $bad.targets[0].widgets += [pscustomobject]@{name='BTN_CLOSE';type='button'}
    Reject { Get-LobbyLiveBattleProof $bad host } '*result controls*'
    $bad=BattleUi $layout; $bad.targets[0].instance=0
    Reject { Get-LobbyLiveBattleProof $bad host } '*battle owner is not an integer*'
    foreach ($peerLayout in @('DLG_BATTLE_A','DLG_BATTLE_B')) {
        $hostBefore=BattleUi $layout; $peer=BattleUi $peerLayout
        $peer.targets[0].instance=8192
        $hostAfter=Copy-Observation $hostBefore; $hostAfter.uiSeq=31
        $proof=Assert-LobbyBattleOverlap $hostBefore $peer $hostAfter $t0 $t1 $t2
        Check ($proof.passed -and $proof.hostBefore.dialog -ceq $layout -and $proof.join.dialog -ceq $peerLayout) "Valid $layout/$peerLayout overlap rejected"
        # Same appearance and numeric owner must not alias different roots.
        $hostAfter.dialog=$otherLayout; $hostAfter.targets[0].dialog=$otherLayout
        Reject { Assert-LobbyBattleOverlap $hostBefore $peer $hostAfter $t0 $t1 $t2 } '*changed identity*'
    }
}
foreach ($layout in @('dlg_battle_b','DLG_BATTLE_C','DLG_BATTLE_RESULT')) {
    Reject { Get-LobbyLiveBattleProof (BattleUi $layout) host } '*ready live battle layout*'
}

function NativeBattleProofs {
    # Deliberately exceed uint32: native GetTickCount64 must not truncate after
    # 49 days of system uptime. These values are native ticks, never wall time.
    $hostUi=BattleUi 'DLG_BATTLE_A'; $joinUi=BattleUi 'DLG_BATTLE_B'
    $joinUi.dialogAppearance=12; $joinUi.dialogInstance=12; $joinUi.targets[0].instance=8192
    @{
        HostUi=$hostUi;JoinUi=$joinUi
        HostAutoBattle=[pscustomobject]@{found=$true;source='preboot-first-battle';kick=[pscustomobject]@{
            succeeded=$true;mode='preboot-first-battle';role='host';dialog='DLG_BATTLE_A';appearance=11;owner=4096;callbackCount=1;committedTick64=5000000100L}}
        JoinAutoBattle=[pscustomobject]@{found=$true;source='preboot-first-battle';kick=[pscustomobject]@{
            succeeded=$true;mode='preboot-first-battle';role='join';dialog='DLG_BATTLE_B';appearance=12;owner=8192;callbackCount=1;committedTick64=5000000150L}}
        HostClose=[pscustomobject]@{role='host';appearance=11;owner=4096;callbackCount=1;observedTick=5000000300L}
        JoinClose=[pscustomobject]@{role='join';appearance=12;owner=8192;callbackCount=1;observedTick=5000000250L}
    }
}
$native=NativeBattleProofs; $nativeOverlap=Assert-LobbyNativeBattleOverlap @native
Check ($nativeOverlap.passed -and $nativeOverlap.provenOverlapMs -eq 100 -and $nativeOverlap.overlapStartTick64 -eq 5000000150L -and $nativeOverlap.overlapEndTick64 -eq 5000000250L) 'Native tick intersection failed or truncated uint64 clock'
Check ($nativeOverlap.cachedUiUsedForIdentityOnly -and $nativeOverlap.roles.join.dialog -ceq 'DLG_BATTLE_B') 'Native overlap lost exact role/layout identity'
$bad=NativeBattleProofs; $bad.HostAutoBattle.kick.PSObject.Properties.Remove('committedTick64')
Reject { Assert-LobbyNativeBattleOverlap @bad } '*native committedTick64 is not an integer*'
foreach ($invalidTick in @($null,$true,'5000000100',@(5000000100L),0,-1,1.5)) {
    $bad=NativeBattleProofs; $bad.HostAutoBattle.kick.committedTick64=$invalidTick
    Reject { Assert-LobbyNativeBattleOverlap @bad } '*native committedTick64 is not an integer*'
    $bad=NativeBattleProofs; $bad.JoinClose.observedTick=$invalidTick
    Reject { Assert-LobbyNativeBattleOverlap @bad } '*native result observedTick is not an integer*'
}
$bad=NativeBattleProofs; $bad.HostClose.observedTick=$bad.HostAutoBattle.kick.committedTick64
Reject { Assert-LobbyNativeBattleOverlap @bad } '*interval is not positive*'
$bad=NativeBattleProofs; $bad.HostClose.observedTick=$bad.JoinAutoBattle.kick.committedTick64
Reject { Assert-LobbyNativeBattleOverlap @bad } '*do not overlap strictly*'
$bad=NativeBattleProofs; $bad.HostClose.observedTick=$bad.JoinAutoBattle.kick.committedTick64-1
Reject { Assert-LobbyNativeBattleOverlap @bad } '*do not overlap strictly*'
foreach ($field in @('owner','appearance','dialog')) {
    $bad=NativeBattleProofs
    $bad.HostAutoBattle.kick.$field=$(if ($field -eq 'dialog') {'DLG_BATTLE_B'} else {999})
    Reject { Assert-LobbyNativeBattleOverlap @bad } '*auto-battle identity differs*'
}
$bad=NativeBattleProofs; $bad.JoinClose.owner=4096
Reject { Assert-LobbyNativeBattleOverlap @bad } '*result-close identity differs*'
$bad=NativeBattleProofs; $bad.JoinClose.role='host'
Reject { Assert-LobbyNativeBattleOverlap @bad } '*result-close identity differs*'
$bad=NativeBattleProofs; $bad.HostAutoBattle.found='true'
Reject { Assert-LobbyNativeBattleOverlap @bad } '*successful preboot auto-battle proof*'
$bad=NativeBattleProofs; $bad.JoinAutoBattle.kick.callbackCount=2
Reject { Assert-LobbyNativeBattleOverlap @bad } '*callback count is not an integer*'

$post=Copy-Observation $w
foreach ($role in @('host','join')) {
    $post.$role.worldSeq += 10
    # Host wins; join loses its hero. Both are replicated real outcomes.
    $post.$role.stacks=@($post.$role.stacks | Where-Object { $_.id -notin @($plan.host.target.id,$plan.join.hero.id) })
}
$result=Get-LobbyBattleOutcome $plan $post.host $post.join
Check ($result.passed -and $result.roles.host.winner -ceq 'hero' -and $result.roles.join.winner -ceq 'neutral') 'Win/loss replicated outcomes not accepted'
Check ($null -eq $result.roles.join.leaderHp -and $result.roles.host.leaderHp -eq 100) 'Leader HP invented or discarded'
Check (-not $result.lifecycleVerified -and -not $result.playerEliminationVerified) 'World delta overstated lifecycle/elimination proof'
$damage=Copy-Observation $w
foreach ($role in @('host','join')) {
    $damage.$role.worldSeq += 10
    $damage.$role.stacks[0].hp=150; $damage.$role.stacks[0].unitStates[0].hp=70
    $damage.$role.stacks[1].hp=60; $damage.$role.stacks[1].unitStates[0].hp=60
}
$result=Get-LobbyBattleOutcome $plan $damage.host $damage.join
Check ($result.roles.host.winner -ceq 'unknown-damaged-or-retreated') 'Damage alone was declared victory'
$bad=Copy-Observation $post; $bad.join.stacks[0].hp=10
Reject { Get-LobbyBattleOutcome $plan $bad.host $bad.join } '*not converged*'
$bad=Copy-Observation $w; $bad.host.worldSeq=14; $bad.join.worldSeq=16
Reject { Get-LobbyBattleOutcome $plan $bad.host $bad.join } '*no replicated casualty*'
Reject { Get-LobbyBattleOutcome $plan $w.host $w.join } '*stale*'
$bad=Copy-Observation $post; $bad.host.players[0].id='0x00010009'
Reject { Get-LobbyBattleOutcome $plan $bad.host $bad.join } '*identity changed*'

$merged=Copy-Observation $post; $merged.host.day=3; $merged.join.day=3
$hl=@(MergeLines host); $jl=@(MergeLines join)
$merge=Get-LobbyMergeProof $hl $jl $merged.host $merged.join
Check ($merge.passed -and $merge.actionId -eq 17 -and -not $merge.serverLedgerVerified) 'Exact native merge proof failed or claimed server proof'
Check ($merge.commandSequence -eq 20) 'Merge proof omitted the identical native broadcast sequence'
$badJoin=@($jl); $badJoin[1]=$badJoin[1].Replace('sequence=20','sequence=21')
Reject { Get-LobbyMergeProof $hl $badJoin $merged.host $merged.join } '*broadcast sequences disagree*'
Check ($merge.roles.host.execute.index -gt $merge.roles.host.applied.index) 'Reentrant native drain before execute log rejected'
Check (($merge | ConvertTo-Json -Depth 8 -Compress).Length -lt 4000) 'Merge evidence contains internal regex objects'
$spacedHost=@(''; foreach ($line in $hl) { $line; ''; ' ' })
$spacedJoin=@(''; foreach ($line in $jl) { $line; '' })
$spacedMerge=Get-LobbyMergeProof $spacedHost $spacedJoin $merged.host $merged.join
Check ($spacedMerge.passed -and $spacedMerge.actionId -eq 17) 'Blank native log entries rejected valid merge proof'
Check ($spacedMerge.roles.host.prepare.index -eq 1 -and $spacedMerge.roles.host.natural.index -eq 4 -and $spacedMerge.roles.join.natural.index -eq 3) 'Blank entries changed original evidence line indexes'
$blankOnly=Get-LobbyMergeProof @('',' ','') @('','') $merged.host $merged.join
Check (-not $blankOnly.passed -and $blankOnly.missing.Count -eq 9) 'Blank-only logs became a successful merge proof'
$blankHost=Get-LobbyMergeProof @('') $jl $merged.host $merged.join
Check (-not $blankHost.passed -and $blankHost.missing.Count -eq 5 -and @($blankHost.missing | Where-Object { $_ -notlike 'host.*' }).Count -eq 0) 'Blank host log borrowed proof from the join log'
$missing=Get-LobbyMergeProof @() @() $merged.host $merged.join
Check (-not $missing.passed -and $missing.missing.Count -eq 9) 'Missing log markers accepted'
$bad=@($hl); $bad[0]=$bad[0].Replace('day 3','day 2')
Reject { Get-LobbyMergeProof $bad $jl $merged.host $merged.join } '*another day*'
$bad=@($hl); $bad[4]=$bad[4].Replace('actionId=17','actionId=18')
Reject { Get-LobbyMergeProof $bad $jl $merged.host $merged.join } '*identities disagree*'
Reject { Get-LobbyMergeProof ($hl+$hl[0]) $jl $merged.host $merged.join } '*repeats*'
$bad=@($hl[1],$hl[0],$hl[2],$hl[3],$hl[4])
Reject { Get-LobbyMergeProof $bad $jl $merged.host $merged.join } '*causal ordering*'
Reject { Get-LobbyMergeProof $hl ($jl+$hl[3]) $merged.host $merged.join } '*Join incorrectly executed*'
foreach ($replacement in @(@('frame=56','frame=55'),@('addressee=0x0','addressee=0x1'),@('sequence=20','sequence=1'),@('sequence=20','sequence=4294967295'),@('active=0x00010001','active=0x00010002'))) {
    $bad=@($hl); $bad[1]=$bad[1].Replace($replacement[0],$replacement[1])
    Reject { Get-LobbyMergeProof $bad $jl $merged.host $merged.join } '*exact host broadcast*'
}
foreach ($fault in @('[simturns] terminal fault: broken','[simturns-diag] local_abort reason=x','[SIMTURNS] terminal pipe fault: x')) {
    Reject { Get-LobbyMergeProof ($hl+$fault) $jl $merged.host $merged.join } '*terminal protocol failure*'
}
$pending=Get-LobbyMergeProof $hl $jl $post.host $post.join
Check (-not $pending.passed -and $pending.missing[0] -eq 'both-worlds-at-merge-day') 'Pre-merge world accepted as day 3'
$commands=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst]},$true) | ForEach-Object GetCommandName)
Check (-not ($commands -match '^(Get-Content|Set-Content|Add-Content|Invoke-.*|Start-.*|Stop-Process|Remove-Item|New-Object)$')) 'Pure helper performs external operations'
Write-Output "PASS: $script:checks offline lobby gameplay policy checks"
