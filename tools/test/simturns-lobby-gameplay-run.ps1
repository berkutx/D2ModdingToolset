# Generated-map scenario, called only by the owned lobby runner after startup.
# No fixed fixture IDs, injected armies, direct HP/MP writes, or popup actor here.
Set-StrictMode -Version Latest

function Import-LobbyBattleObservers {
    $path = Join-Path $PSScriptRoot '_simturns_gameplay.ps1'
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
    if ($errors.Count) { throw 'Existing battle observer source did not parse.' }
    foreach ($name in @('Read-PrearmedAutoBattleProof', 'ConvertFrom-ScriptedBattleCloseMarker', 'Assert-ScriptedBattleCloseProof')) {
        $nodes=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$false))
        if ($nodes.Count -ne 1) { throw "Battle observer $name is not unique." }
        . ([scriptblock]::Create(($nodes[0].Extent.Text -replace ('^function\s+'+[regex]::Escape($name)),"function script:$name")))
    }
}
function Write-Step([string]$Text) { Write-Host $Text }
function Find-LobbyLiveBattleProof($Ui,[string]$Role) {
    if($Ui.dialog -cnotin @('DLG_BATTLE_A','DLG_BATTLE_B') -or $Ui.dialogReady -ne $true) { return $null }
    foreach($target in @($Ui.targets | Where-Object dialog -CEQ $Ui.dialog)) {
        if(@($target.widgets | Where-Object name -CEQ 'BTN_CLOSE').Count) { return $null }
    }
    return Get-LobbyLiveBattleProof -Ui $Ui -Role $Role
}

function Get-LobbyGameplaySnapshot([string]$Stage) {
    Assert-PairProgress
    $pair=@{}
    foreach ($role in @('host','join')) {
        $pair[$role]=[pscustomobject]@{ ui=(Get-GameUiSnapshot $role); world=(Get-World $role) }
    }
    Save-Json 'gameplay-latest.json' @{stage=$Stage;utc=(Get-PairUtcNow).ToString('o');roles=$pair}
    return $pair
}
function Wait-LobbyGameplayIdle([int]$Day, [int]$Seconds=120) {
    $deadline=(Get-PairUtcNow).AddSeconds($Seconds)
    while ((Get-PairUtcNow) -lt $deadline) {
        $pair=Get-LobbyGameplaySnapshot "wait-day-$Day-idle"
        $ready=$true
        foreach($role in @('host','join')) {
            $ready=$ready -and (Test-StartupRoleAccepted $pair[$role].ui $script:logEvidence[$role].bootstrapOperational) -and
                [int]$pair[$role].world.day -eq $Day
        }
        if($ready) { return $pair }
        Start-Sleep -Milliseconds 100
    }
    throw "Both roles did not reach independent idle day $Day."
}

function Start-LobbyMovePair([hashtable]$Moves,[string]$Stage) {
    $intents=@{}
    foreach($role in @('host','join')) {
        $move=$Moves[$role]
        $world=Get-World $role
        $matches=@($world.stacks | Where-Object id -CEQ $move.hero.id)
        if($matches.Count -ne 1 -or $matches[0].relation -cne 'self' -or
            [int]$matches[0].x -ne [int]$move.hero.x -or [int]$matches[0].y -ne [int]$move.hero.y -or
            [int]$matches[0].movement -ne [int]$move.hero.movement -or $matches[0].inside -ne $move.hero.inside) {
            throw "$role movement source changed before its single intent."
        }
        $binding=Get-MapActionTargetBinding $role
        $intents[$role]=@{role=$role;id=$move.hero.id;x=[int]$move.x;y=[int]$move.y;
            appearance=$binding.Appearance;instance=$binding.Instance;source=$matches[0];worldSeq=$world.worldSeq}
    }
    Save-Json "$Stage-intents.json" $intents # Durable claim precedes either network write.
    $client=[Net.Http.HttpClient]::new()
    $client.Timeout=[TimeSpan]::FromSeconds(45)
    $tasks=@{}; $responses=@{}
    try {
        foreach($role in @('host','join')) {
            $i=$intents[$role]
            $uri="$script:RelayBase/api/ui/move-toward?role=$role&id=$([uri]::EscapeDataString($i.id))&x=$($i.x)&y=$($i.y)&appearance=$($i.appearance)&instance=$($i.instance)&timeoutMs=30000"
            $tasks[$role]=$client.PostAsync($uri,[Net.Http.StringContent]::new(''))
        }
    } catch { $client.Dispose(); throw }
    return [pscustomobject]@{client=$client;tasks=$tasks;responses=$responses;intents=$intents;stage=$Stage}
}
function Read-LobbyMoveReceipts($Pending) {
    foreach($role in @('host','join')) {
        if($Pending.responses.ContainsKey($role) -or -not $Pending.tasks[$role].IsCompleted) { continue }
        $http=$Pending.tasks[$role].GetAwaiter().GetResult()
        try {
            $body=$http.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            if(-not $http.IsSuccessStatusCode) { throw "$role native move HTTP failed: $($http.StatusCode); no retry." }
            $receipt=$body | ConvertFrom-Json
            if($receipt.found -isnot [bool] -or -not $receipt.found -or $receipt.role -isnot [string] -or $receipt.role -cne $role) {
                throw "$role native move was not submitted; no retry."
            }
            $intent=$Pending.intents[$role]
            if($receipt.move.id -isnot [string]) { throw 'Move receipt ID is not a string scalar.' }
            foreach($field in @('x','y','appearance','instance')) {
                [void](Get-LobbyGameplayInteger $receipt.move.$field "Move receipt $field")
            }
            foreach($field in @('id','x','y','appearance','instance')) {
                if($receipt.move.$field -cne $intent.$field) { throw "$role move receipt changed $field; no retry." }
            }
            $Pending.responses[$role]=$receipt
            Save-Json "$($Pending.stage)-receipts.json" $Pending.responses
        } finally { $http.Dispose() }
    }
    return $Pending.responses.Count -eq 2
}

function Invoke-LobbyCapitalExits($Plan) {
    $moves=@{}
    foreach($role in @('host','join')) {
        $hero=$Plan.$role.hero
        if($hero.inside -ne $true) { throw 'This generated-map scenario expects both initial heroes in their own capitals.' }
        $exit=Property $hero 'capitalExit'
        if(-not $exit -or $exit.kind -cne 'observed-5x5-capital' -or
            [int]$exit.anchorX -ne [int]$hero.x -or [int]$exit.anchorY -ne [int]$hero.y) {
            throw "$role capital exit lacks actual native geometry/passability evidence; coordinates will not be guessed."
        }
        $moves[$role]=@{hero=$hero;x=[int]$exit.x;y=[int]$exit.y}
    }
    $pending=Start-LobbyMovePair $moves 'capital-exit'
    try {
        $deadline=(Get-PairUtcNow).AddSeconds(45)
        while((Get-PairUtcNow) -lt $deadline) {
            $submitted=Read-LobbyMoveReceipts $pending
            $pair=Get-LobbyGameplaySnapshot 'capital-exit'
            $done=$submitted
            foreach($view in @('host','join')) {
                foreach($role in @('host','join')) {
                    $hero=@($pair[$view].world.stacks | Where-Object id -CEQ $moves[$role].hero.id)
                    $done=$done -and $hero.Count -eq 1 -and $hero[0].inside -eq $false -and
                        [int]$hero[0].x -eq $moves[$role].x -and [int]$hero[0].y -eq $moves[$role].y
                }
            }
            if($done) { Save-Json 'capital-exit-world.json' $pair; return }
            Start-Sleep -Milliseconds 100
        }
        throw 'Capital exit did not replicate to both worlds; no second move sent.'
    } finally { $pending.client.Dispose() }
}

function Invoke-LobbyConcurrentBattles($Plan) {
    $moves=@{}; $baselines=@{}; $battleUi=@{}; $battleProof=@{}; $overlap=$null; $bracketRejection=$null
    foreach($role in @('host','join')) {
        $p=$Plan.$role
        $moves[$role]=@{hero=$p.hero;x=[int]$p.target.x;y=[int]$p.target.y}
        $baselines[$role]=[pscustomobject]@{role=$role;lineCount=@(Read-ClientLogLines $script:ClientLogs[$role]).Count}
    }
    $pending=Start-LobbyMovePair $moves 'parallel-attacks'
    try {
        $deadline=(Get-PairUtcNow).AddSeconds(180)
        while((Get-PairUtcNow) -lt $deadline) {
            Assert-PairProgress
            $submitted=Read-LobbyMoveReceipts $pending
            $hostBefore=Get-GameUiSnapshot host; $beforeAt=Get-PairUtcNow
            $joinUi=Get-GameUiSnapshot join; $joinAt=Get-PairUtcNow
            $hostAfter=Get-GameUiSnapshot host; $afterAt=Get-PairUtcNow
            $uis=@{host=$hostAfter;join=$joinUi}
            foreach($role in @('host','join')) {
                $proof=Find-LobbyLiveBattleProof $uis[$role] $role
                if($proof -and -not $battleUi.ContainsKey($role)) {
                    $battleUi[$role]=$uis[$role];$battleProof[$role]=$proof
                    Save-Json "$role-first-live-battle.json" $uis[$role]
                }
            }
            if(-not $overlap -and (Find-LobbyLiveBattleProof $hostBefore host) -and
                (Find-LobbyLiveBattleProof $joinUi join) -and (Find-LobbyLiveBattleProof $hostAfter host)) {
                try {
                    $overlap=Assert-LobbyBattleOverlap -HostBefore $hostBefore -Join $joinUi -HostAfter $hostAfter `
                        -ObservedUtcBefore $beforeAt -ObservedUtcJoin $joinAt -ObservedUtcAfter $afterAt
                } catch {
                    # This cached observation bracket is supplemental. Each UI
                    # was validated above; only native intervals authorize PASS.
                    $bracketRejection=@{passed=$false;source='supplemental-ui-observation-bracket';error=$_.Exception.Message;
                        observedUtcBefore=$beforeAt;observedUtcJoin=$joinAt;observedUtcAfter=$afterAt}
                    Save-Json 'concurrent-live-battle-bracket-rejected.json' $bracketRejection
                }
                if($overlap) {
                    Save-Json 'concurrent-live-battle-overlap.json' $overlap
                    Write-Step 'Observed both live battle owners; native timing proof is still required.'
                }
            }
            $idle=$submitted -and $battleUi.Count -eq 2
            foreach($role in @('host','join')) {
                $idle=$idle -and (Test-StartupRoleAccepted $uis[$role] $script:logEvidence[$role].bootstrapOperational)
            }
            if($idle) {
                $native=@{}
                foreach($role in @('host','join')) {
                    $lines=@(Read-ClientLogLines $script:ClientLogs[$role])
                    $auto=Read-PrearmedAutoBattleProof -Role $role -Lines $lines -BattleUi $battleUi[$role]
                    if(-not $auto) { throw "$role has no native auto-battle receipt." }
                    $close=Assert-ScriptedBattleCloseProof -State ([pscustomobject]@{Role=$role;
                        LiveBattleDialog=$battleProof[$role].dialog;
                        LiveBattleAppearance=$battleProof[$role].appearance;LiveBattleOwner=$battleProof[$role].owner}) `
                        -Baseline $baselines[$role] -Lines $lines
                    $native[$role]=@{autoBattle=$auto;close=$close}
                }
                Save-Json 'concurrent-battles-native.json' $native
                $nativeOverlap=Assert-LobbyNativeBattleOverlap -HostUi $battleUi.host -JoinUi $battleUi.join `
                    -HostAutoBattle $native.host.autoBattle -JoinAutoBattle $native.join.autoBattle `
                    -HostClose $native.host.close -JoinClose $native.join.close
                if($nativeOverlap.passed -isnot [bool] -or -not $nativeOverlap.passed) {
                    throw 'Native callback-to-result overlap was not proved.'
                }
                Save-Json 'concurrent-native-battle-overlap.json' $nativeOverlap
                return @{overlap=$nativeOverlap;observationBracket=$overlap;observationBracketRejection=$bracketRejection;native=$native}
            }
            Start-Sleep -Milliseconds 50
        }
        Save-Json 'concurrent-battle-timeout.json' (Get-LobbyGameplaySnapshot 'battle-timeout')
        throw 'Concurrent battles did not complete within the bounded scenario; no attack was retried.'
    } finally { $pending.client.Dispose() }
}

function Invoke-LobbyEndTurnPair($Pair,[int]$Day) {
    $intents=@{}
    foreach($role in @('host','join')) {
        $ui=$Pair[$role].ui
        $targets=@($ui.targets | Where-Object dialog -CEQ 'DLG_STRATEGIC')
        if($targets.Count -ne 1 -or $ui.strategicIdle -isnot [bool] -or -not $ui.strategicIdle -or
            $ui.dialogReady -isnot [bool] -or -not $ui.dialogReady) {
            throw "$role lacks one idle strategic owner for EndTurn."
        }
        $buttons=@($targets[0].widgets | Where-Object name -CEQ 'BTN_END_TURN')
        if($buttons.Count -ne 1 -or $buttons[0].state.enabled -isnot [bool] -or -not $buttons[0].state.enabled) { throw "$role EndTurn is not enabled." }
        $appearance=Get-LobbyGameplayInteger $ui.dialogAppearance "$role EndTurn appearance" 1
        if($ui.dialogInstance -ne $appearance) { throw "$role EndTurn appearance aliases differ." }
        $intents[$role]=@{appearance=$appearance;
            instance=(Get-LobbyGameplayInteger $targets[0].instance "$role EndTurn owner" 1);
            ui=(Get-LobbyGameplayInteger $ui.uiSeq "$role EndTurn UI watermark" 1)}
    }
    Save-Json "day-$Day-endturn-intents.json" $intents
    $h=$intents.host;$j=$intents.join
    $uri="$script:RelayBase/api/ui/end-turn-pair-when-strategic-idle?hostappearance=$($h.appearance)&hostinstance=$($h.instance)&hostui=$($h.ui)&joinappearance=$($j.appearance)&joininstance=$($j.instance)&joinui=$($j.ui)&waitMs=120000&timeoutMs=8000"
    $receipt=Invoke-RestMethod -Method POST -Uri $uri -TimeoutSec 135
    Save-Json "day-$Day-endturn-receipt.json" $receipt
    if($receipt.found -isnot [bool] -or -not $receipt.found) { throw 'Paired EndTurn did not succeed; no retry.' }
    foreach($role in @('host','join')) {
        $r=$receipt.$role;$i=$intents[$role]
        foreach($field in @('appearance','instance')) { [void](Get-LobbyGameplayInteger $r.invoke.$field "EndTurn receipt $field" 1) }
        if($r.role -isnot [string] -or $r.invoke.dlg -isnot [string] -or $r.invoke.btn -isnot [string] -or
            $r.found -isnot [bool] -or -not $r.found -or $r.role -cne $role -or $r.invoke.dlg -cne 'DLG_STRATEGIC' -or
            $r.invoke.btn -cne 'BTN_END_TURN' -or $r.invoke.appearance -ne $i.appearance -or
            $r.invoke.instance -ne $i.instance -or $r.strategicIdle -isnot [bool] -or -not $r.strategicIdle) { throw 'Paired EndTurn receipt lost saved identity.' }
    }
    return $receipt
}

function Invoke-LobbyConcurrentBattleMerge {
    Import-LobbyBattleObservers
    $pair=Wait-LobbyGameplayIdle 1
    $plan=Get-LobbyBattlePlan -HostWorld $pair.host.world -JoinWorld $pair.join.world
    Save-Json 'initial-world-plan.json' $plan
    Invoke-LobbyCapitalExits $plan
    $pair=Wait-LobbyGameplayIdle 1
    $plan=Get-LobbyBattlePlan -HostWorld $pair.host.world -JoinWorld $pair.join.world
    Save-Json 'battle-world-plan.json' $plan
    $battles=Invoke-LobbyConcurrentBattles $plan
    $pair=Wait-LobbyGameplayIdle 1
    # The independent pure oracle checks the exact selected IDs in both worlds.
    $outcome=$null;$outcomeError='No post-battle world publication';$outcomeDeadline=(Get-PairUtcNow).AddSeconds(30)
    while((Get-PairUtcNow) -lt $outcomeDeadline) {
        try {
            $outcome=Get-LobbyBattleOutcome -Plan $plan -HostWorld $pair.host.world -JoinWorld $pair.join.world
            break
        } catch {
            $outcomeError=$_.Exception.Message
            if($outcomeError -notmatch 'world is stale|has not converged|no replicated casualty|worlds disagree on day') { throw }
        }
        Start-Sleep -Milliseconds 100
        $pair=Get-LobbyGameplaySnapshot 'await-battle-world-convergence'
    }
    if(-not $outcome) { throw "Post-battle world evidence timed out: $outcomeError" }
    Save-Json 'battle-world-outcomes.json' $outcome
    $turns=[Collections.Generic.List[object]]::new()
    for($day=1;$day -lt $ExpectedMergeDay;$day++) {
        $turns.Add((Invoke-LobbyEndTurnPair $pair $day))
        if($day+1 -lt $ExpectedMergeDay) { $pair=Wait-LobbyGameplayIdle ($day+1) }
    }
    # Stock is intentionally NOT independently actionable on both clients.
    $deadline=(Get-PairUtcNow).AddSeconds(120);$merge=$null
    while((Get-PairUtcNow) -lt $deadline) {
        $pair=Get-LobbyGameplaySnapshot 'await-natural-merge'
        if($script:logEvidence.host.stockTurnsReleased -and $script:logEvidence.join.stockTurnsReleased) {
            $merge=Get-LobbyMergeProof -HostLines @(Read-ClientLogLines $script:ClientLogs.host) `
                -JoinLines @(Read-ClientLogLines $script:ClientLogs.join) -HostWorld $pair.host.world -JoinWorld $pair.join.world -ExpectedDay $ExpectedMergeDay
            if($merge.passed) { break }
        }
        Start-Sleep -Milliseconds 100
    }
    if(-not $merge -or -not $merge.passed) { throw 'Natural merge and Stock release were not proved on both clients.' }
    $script:GameplayProof=@{battles=$battles;outcome=$outcome;endTurns=$turns.ToArray();merge=$merge;campaign18=$false}
    Save-Json 'concurrent-battle-merge-proof.json' $script:GameplayProof
}
