#requires -Version 7.0
# Pure policies for the generated-map lobby test. No game, network, file or clock I/O.
# Callers own process identity, fresh observations, one-shot commands and cleanup.

function Get-LobbyGameplayProperty($Value, [string]$Name) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [System.Collections.IDictionary]) { return $Value[$Name] }
    $property = $Value.PSObject.Properties[$Name]
    if ($property) { return $property.Value }
    return $null
}

function Get-LobbyGameplayInteger($Value, [string]$Context, [long]$Minimum = 0, [long]$Maximum = [uint32]::MaxValue) {
    if (($Value -isnot [int] -and $Value -isnot [long] -and $Value -isnot [uint32]) -or
        [long]$Value -lt $Minimum -or [long]$Value -gt $Maximum) {
        throw "$Context is not an integer in $Minimum..$Maximum"
    }
    return [long]$Value
}

function Get-LobbyGameplayIntegerProperty($Record, [string]$Name, [string]$Context,
    [long]$Minimum = 0, [long]$Maximum = [uint32]::MaxValue) {
    # Read the raw field without pipeline enumeration: a JSON [123] must not
    # turn into the scalar 123 before its exact integer type is validated.
    $value=$null
    if ($Record -is [System.Collections.IDictionary]) { $value=$Record[$Name] }
    elseif ($null -ne $Record) {
        $property=$Record.PSObject.Properties[$Name]
        if ($property) { $value=$property.Value }
    }
    Get-LobbyGameplayInteger $value $Context $Minimum $Maximum
}

function Get-LobbyGameplayId($Value, [string]$Context) {
    if ($Value -isnot [string] -or $Value -cnotmatch '^0x[0-9a-fA-F]{8}$' -or
        $Value -match '^0x(00000000|ffffffff)$') { throw "$Context is not a nonempty wire ID" }
    return $Value.ToLowerInvariant()
}

function Assert-LobbyGameplayWorld($World, [string]$Role, [long]$After = 0) {
    if ((Get-LobbyGameplayProperty $World role) -cne $Role) { throw "$Role world has a different role" }
    $sequence = Get-LobbyGameplayInteger (Get-LobbyGameplayProperty $World worldSeq) "$Role world sequence" 1
    if ($sequence -le $After) { throw "$Role world is stale (sequence $sequence <= $After)" }
    [void](Get-LobbyGameplayInteger (Get-LobbyGameplayProperty $World day) "$Role world day" 1)
    $self = @((Get-LobbyGameplayProperty $World players) | Where-Object {
        (Get-LobbyGameplayProperty $_ relation) -ceq 'self'
    })
    if ($self.Count -ne 1 -or (Get-LobbyGameplayProperty $self[0] human) -isnot [bool] -or
        -not $self[0].human) { throw "$Role world does not identify exactly one local human" }
    $selfId = Get-LobbyGameplayId $self[0].id "$Role player"
    $ids = @{}
    foreach ($stack in @(Get-LobbyGameplayProperty $World stacks)) {
        $id = Get-LobbyGameplayId (Get-LobbyGameplayProperty $stack id) "$Role stack"
        if ($ids.ContainsKey($id)) { throw "$Role world repeats stack $id" }
        $ids[$id] = $true
    }
    return $selfId
}

function Get-LobbyBattlePlan {
    param(
        [Parameter(Mandatory)]$HostWorld,
        [Parameter(Mandatory)]$JoinWorld,
        [long]$HostAfter = 0,
        [long]$JoinAfter = 0,
        [ValidateRange(1, 32)][int]$MaxCandidates = 8
    )
    $worlds = @{ host = $HostWorld; join = $JoinWorld }
    $after = @{ host = $HostAfter; join = $JoinAfter }
    $sides = @{}
    foreach ($role in @('host', 'join')) {
        $world = $worlds[$role]
        $selfId = Assert-LobbyGameplayWorld $world $role $after[$role]
        # A world publication can precede the UI admission transition. Current
        # strategicIdle/bootstrap/owner and native send revalidation belong to
        # the caller; cached world.strategicActionReady is not a live gate.
        $heroes = @($world.stacks | Where-Object {
            (Get-LobbyGameplayProperty $_ relation) -ceq 'self' -and
            (Get-LobbyGameplayProperty $_ owner) -ieq $selfId
        } | Where-Object {
            $candidate = $_
            $leaderId = Get-LobbyGameplayProperty $candidate leaderId
            $leader = @((Get-LobbyGameplayProperty $candidate unitStates) | Where-Object {
                (Get-LobbyGameplayProperty $_ id) -ieq $leaderId
            })
            (Get-LobbyGameplayProperty $candidate units) -is [ValueType] -and
            (Get-LobbyGameplayProperty $candidate units) -gt 0 -and
            (Get-LobbyGameplayProperty $candidate movement) -is [ValueType] -and
            (Get-LobbyGameplayProperty $candidate movement) -gt 0 -and
            $leaderId -is [string] -and $leader.Count -eq 1 -and
            (Get-LobbyGameplayProperty $leader[0] hp) -is [ValueType] -and $leader[0].hp -gt 0
        } | Sort-Object @{Expression={ [bool]$_.inside }}, id)
        if ($heroes.Count -eq 0) { throw "$role has no own mobile hero with a living observed leader" }
        $hero = $heroes[0]
        [void](Get-LobbyGameplayId $hero.leaderId "$role leader")
        foreach ($field in @('x', 'y', 'movement', 'units', 'hp')) {
            [void](Get-LobbyGameplayInteger (Get-LobbyGameplayProperty $hero $field) "$role hero $field")
        }
        [void](Get-LobbyGameplayInteger $hero.movement "$role hero movement" 1 255)
        [void](Get-LobbyGameplayInteger $hero.units "$role hero units" 1 6)
        $leader = @($hero.unitStates | Where-Object { $_.id -ieq $hero.leaderId })[0]
        [void](Get-LobbyGameplayInteger $leader.hp "$role leader HP" 1)
        if ($hero.inside -isnot [bool]) { throw "$role hero inside is not Boolean" }
        if ($hero.inside) {
            $exit = Get-LobbyGameplayProperty $hero capitalExit
            if ((Get-LobbyGameplayProperty $exit kind) -cne 'observed-5x5-capital') {
                throw "$role garrison has no observed native capital exit proof"
            }
            [void](Get-LobbyGameplayId $exit.fortId "$role capital exit fort")
            foreach ($field in @('anchorX','anchorY','innerX','innerY','x','y','sizeX','sizeY')) {
                [void](Get-LobbyGameplayInteger (Get-LobbyGameplayProperty $exit $field) "$role capital exit $field")
            }
            if ($exit.anchorX -ne $hero.x -or $exit.anchorY -ne $hero.y -or $exit.sizeX -ne 5 -or $exit.sizeY -ne 5) {
                throw "$role capital exit proof does not match the observed garrison"
            }
        }
        $candidates = @($world.stacks | Where-Object {
            (Get-LobbyGameplayProperty $_ relation) -ceq 'neutral' -and
            (Get-LobbyGameplayProperty $_ inside) -is [bool] -and -not $_.inside -and
            (Get-LobbyGameplayProperty $_ hp) -is [ValueType] -and $_.hp -gt 0 -and
            (Get-LobbyGameplayProperty $_ units) -is [ValueType] -and $_.units -gt 0
        } | ForEach-Object {
            $target = $_
            foreach ($field in @('x', 'y', 'units', 'hp')) {
                [void](Get-LobbyGameplayInteger (Get-LobbyGameplayProperty $target $field) "$role neutral $field")
            }
            if ((Get-LobbyGameplayId $target.owner "$role neutral owner") -eq $selfId) {
                throw "$role neutral target belongs to the local player"
            }
            [pscustomobject]@{ target = $target; distance = [Math]::Max(
                [Math]::Abs([long]$target.x - [long]$hero.x),
                [Math]::Abs([long]$target.y - [long]$hero.y)); hp = [long]$target.hp; id = $target.id }
        } | Sort-Object distance, hp, id | Select-Object -First $MaxCandidates)
        if ($candidates.Count -eq 0) { throw "$role has no free living neutral battle candidate" }
        $sides[$role] = [pscustomobject]@{
            role = $role; worldSeq = [long]$world.worldSeq; day = [long]$world.day
            playerId = $selfId; hero = $hero; needsExit = [bool]$hero.inside
            rankedCandidates = $candidates
        }
    }
    if ($HostWorld.day -ne $JoinWorld.day -or $sides.host.playerId -eq $sides.join.playerId) {
        throw 'Battle worlds do not identify different humans on the same day'
    }
    $pairs = @(foreach ($hostCandidate in $sides.host.rankedCandidates) {
        foreach ($joinCandidate in $sides.join.rankedCandidates) {
            if ($hostCandidate.id -ine $joinCandidate.id) {
                [pscustomobject]@{ host = $hostCandidate; join = $joinCandidate
                    distance = $hostCandidate.distance + $joinCandidate.distance
                    hp = $hostCandidate.hp + $joinCandidate.hp }
            }
        }
    })
    $pair = $pairs | Sort-Object distance, hp, @{Expression={$_.host.id}}, @{Expression={$_.join.id}} | Select-Object -First 1
    if (-not $pair) { throw 'No two distinct neutral targets exist within the bounded candidate lists' }
    foreach ($role in @('host', 'join')) {
        $sides[$role] | Add-Member -NotePropertyName target -NotePropertyValue $pair.$role.target
        $sides[$role] | Add-Member -NotePropertyName distance -NotePropertyValue $pair.$role.distance
        $otherWorld = if ($role -eq 'host') { $JoinWorld } else { $HostWorld }
        foreach ($kind in @('hero','target')) {
            $selected = $sides[$role].$kind
            $other = @($otherWorld.stacks | Where-Object { $_.id -ieq $selected.id })
            if ($other.Count -ne 1) { throw "$role $kind is missing from the other client's world" }
            foreach ($field in @('owner','x','y','movement','units','hp','inside','leaderId')) {
                if ((Get-LobbyGameplayProperty $other[0] $field) -ne (Get-LobbyGameplayProperty $selected $field)) {
                    throw "$role $kind $field disagrees across world snapshots"
                }
            }
        }
    }
    [pscustomobject]@{ host = $sides.host; join = $sides.join; roles = [pscustomobject]$sides; day = [long]$HostWorld.day
        requiresExit = [bool]($sides.host.needsExit -or $sides.join.needsExit)
        routeValidated = $false; source = 'fresh-generated-map-worlds'; maximumCandidatesPerRole = $MaxCandidates }
}

function Get-LobbyBattleOutcome {
    param([Parameter(Mandatory)]$Plan, [Parameter(Mandatory)]$HostWorld, [Parameter(Mandatory)]$JoinWorld)
    if ((Assert-LobbyGameplayWorld $HostWorld host ([long]$Plan.host.worldSeq)) -ine $Plan.host.playerId -or
        (Assert-LobbyGameplayWorld $JoinWorld join ([long]$Plan.join.worldSeq)) -ine $Plan.join.playerId) {
        throw 'Battle outcome local player identity changed'
    }
    if ($HostWorld.day -ne $JoinWorld.day) { throw 'Battle outcome worlds disagree on day' }
    $outcomes = @{}
    foreach ($role in @('host','join')) {
        $side = $Plan.$role
        $observations = @{}
        foreach ($kind in @('hero','target')) {
            $id = $side.$kind.id
            $views = @{}
            foreach ($observer in @('host','join')) {
                $world = if ($observer -eq 'host') { $HostWorld } else { $JoinWorld }
                $stack = @($world.stacks | Where-Object { $_.id -ieq $id })
                if ($stack.Count -eq 0) { $views[$observer] = $null; continue }
                $s = $stack[0]
                $view = [ordered]@{id=$s.id.ToLowerInvariant();owner=$s.owner.ToLowerInvariant()}
                foreach ($field in @('x','y','movement','units','hp')) {
                    $view[$field] = Get-LobbyGameplayInteger $s.$field "$observer $role $kind $field"
                }
                if ($s.inside -isnot [bool]) { throw "$observer $role $kind inside is not Boolean" }
                $view.inside = $s.inside
                $view.leaderId = $s.leaderId
                $view.unitStates = @($s.unitStates | Sort-Object id | ForEach-Object {
                    [pscustomobject]@{id=(Get-LobbyGameplayId $_.id "$observer unit");hp=(Get-LobbyGameplayInteger $_.hp "$observer unit HP")}
                })
                $views[$observer] = [pscustomobject]$view
            }
            if (($views.host | ConvertTo-Json -Depth 8 -Compress) -cne ($views.join | ConvertTo-Json -Depth 8 -Compress)) {
                throw "$role $kind result has not converged across both clients"
            }
            $observations[$kind] = $views.host
        }
        $hero = $observations.hero; $target = $observations.target
        if (-not $hero -and -not $target) { throw "$role battle outcome is ambiguous: both stacks absent" }
        $changed = -not $hero -or -not $target -or $hero.hp -lt $side.hero.hp -or
            $hero.units -lt $side.hero.units -or $target.hp -lt $side.target.hp -or $target.units -lt $side.target.units
        if (-not $changed) { throw "$role battle has no replicated casualty or HP-loss evidence" }
        $leaderHp = $null
        if ($hero) {
            $leaders = @($hero.unitStates | Where-Object { $_.id -ieq $hero.leaderId })
            if ($leaders.Count -eq 1) { $leaderHp = $leaders[0].hp }
        }
        $outcomes[$role] = [pscustomobject]@{ heroId=$side.hero.id;targetId=$side.target.id
            winner=$(if (-not $target) {'hero'} elseif (-not $hero) {'neutral'} else {'unknown-damaged-or-retreated'})
            heroGone=($null -eq $hero);targetGone=($null -eq $target);leaderHp=$leaderHp
            hero=$hero;target=$target;beforeHero=$side.hero;beforeTarget=$side.target }
    }
    [pscustomobject]@{passed=$true;replicatedOutcome=$true;roles=[pscustomobject]$outcomes
        hostWorldSeq=[long]$HostWorld.worldSeq;joinWorldSeq=[long]$JoinWorld.worldSeq;day=[long]$HostWorld.day
        lifecycleVerified=$false;playerEliminationVerified=$false}
}

function Get-LobbyLiveBattleProof {
    param([Parameter(Mandatory)]$Ui, [Parameter(Mandatory)][ValidateSet('host','join')][string]$Role)
    if ((Get-LobbyGameplayProperty $Ui dialog) -cnotin @('DLG_BATTLE_A','DLG_BATTLE_B') -or
        (Get-LobbyGameplayProperty $Ui dialogReady) -isnot [bool] -or -not $Ui.dialogReady) {
        throw "$Role does not have a ready live battle layout"
    }
    $appearance = Get-LobbyGameplayInteger (Get-LobbyGameplayProperty $Ui dialogAppearance) "$Role battle appearance" 1
    if ((Get-LobbyGameplayInteger (Get-LobbyGameplayProperty $Ui dialogInstance) "$Role battle instance" 1) -ne $appearance) {
        throw "$Role battle appearance aliases disagree"
    }
    $sequence = Get-LobbyGameplayProperty $Ui uiSeq
    if ($null -eq $sequence) { $sequence = Get-LobbyGameplayProperty $Ui seq }
    $sequence = Get-LobbyGameplayInteger $sequence "$Role battle sequence" 1
    $owners = @((Get-LobbyGameplayProperty $Ui targets) | Where-Object { $_.dialog -ceq $Ui.dialog })
    if ($owners.Count -ne 1) { throw "$Role battle does not have exactly one native owner" }
    $owner = Get-LobbyGameplayInteger $owners[0].instance "$Role battle owner" 1
    $widgets = @(Get-LobbyGameplayProperty $owners[0] widgets)
    if (@($widgets | Where-Object { $_.name -ceq 'BTN_CLOSE' }).Count) { throw "$Role battle already exposes result controls" }
    $live = @($widgets | Where-Object {
        ($_.name -cin @('BTN_DEFEND','BTN_RETREAT','BTN_WAIT','BTN_RESOLVE') -and $_.type -ceq 'button') -or
        ($_.name -ceq 'TOG_AUTOBATTLE' -and $_.type -ceq 'toggle')
    })
    if ($live.Count -eq 0) { throw "$Role battle has no typed live controls" }
    [pscustomobject]@{ role = $Role; dialog=$Ui.dialog; appearance = $appearance; owner = $owner; sequence = $sequence
        live = $true; liveControls = @($live | ForEach-Object name); observation = $Ui }
}

function Assert-LobbyBattleOverlap {
    param([Parameter(Mandatory)]$HostBefore, [Parameter(Mandatory)]$Join,
        [Parameter(Mandatory)]$HostAfter,
        [Parameter(Mandatory)][DateTimeOffset]$ObservedUtcBefore,
        [Parameter(Mandatory)][DateTimeOffset]$ObservedUtcJoin,
        [Parameter(Mandatory)][DateTimeOffset]$ObservedUtcAfter)
    $before = Get-LobbyLiveBattleProof $HostBefore host
    $peer = Get-LobbyLiveBattleProof $Join join
    $after = Get-LobbyLiveBattleProof $HostAfter host
    if ($before.dialog -cne $after.dialog -or $before.appearance -ne $after.appearance -or $before.owner -ne $after.owner -or
        $after.sequence -lt $before.sequence) { throw 'Host live battle changed identity across the join observation' }
    if ($ObservedUtcBefore -ge $ObservedUtcJoin -or $ObservedUtcJoin -ge $ObservedUtcAfter) {
        throw 'Live battle observations do not form a positive ordered time bracket'
    }
    [pscustomobject]@{ passed = $true; source = 'host-live-join-live-host-live-observation-bracket'
        hostBefore = $before; join = $peer; hostAfter = $after
        beforeUtc = $ObservedUtcBefore; joinUtc = $ObservedUtcJoin; afterUtc = $ObservedUtcAfter
        observationBracketMs = ($ObservedUtcAfter - $ObservedUtcBefore).TotalMilliseconds
        exactEngineOverlapDurationKnown = $false }
}

function Assert-LobbyNativeBattleOverlap {
    param([Parameter(Mandatory)]$HostUi, [Parameter(Mandatory)]$JoinUi,
        [Parameter(Mandatory)]$HostAutoBattle, [Parameter(Mandatory)]$JoinAutoBattle,
        [Parameter(Mandatory)]$HostClose, [Parameter(Mandatory)]$JoinClose)
    # Both owned native processes run on the same Windows host. Unlike HTTP
    # observation time, GetTickCount64 is their common monotonic event clock.
    # The UI publications bind identities only; cached UI cannot prove overlap.
    $uis=@{host=$HostUi;join=$JoinUi}
    $autoBattles=@{host=$HostAutoBattle;join=$JoinAutoBattle}
    $closes=@{host=$HostClose;join=$JoinClose}
    $roles=@{}
    foreach ($role in @('host','join')) {
        $identity=Get-LobbyLiveBattleProof -Ui $uis[$role] -Role $role
        $auto=$autoBattles[$role]; $close=$closes[$role]
        $kick=Get-LobbyGameplayProperty $auto kick
        if ((Get-LobbyGameplayProperty $auto found) -isnot [bool] -or -not $auto.found -or
            (Get-LobbyGameplayProperty $auto source) -cne 'preboot-first-battle' -or
            (Get-LobbyGameplayProperty $kick succeeded) -isnot [bool] -or -not $kick.succeeded -or
            (Get-LobbyGameplayProperty $kick mode) -cne 'preboot-first-battle' -or
            (Get-LobbyGameplayProperty $kick role) -cne $role) {
            throw "$role native overlap requires its successful preboot auto-battle proof"
        }
        if ((Get-LobbyGameplayProperty $kick dialog) -cne $identity.dialog -or
            (Get-LobbyGameplayIntegerProperty $kick appearance "$role native auto appearance" 1) -ne $identity.appearance -or
            (Get-LobbyGameplayIntegerProperty $kick owner "$role native auto owner" 1) -ne $identity.owner -or
            (Get-LobbyGameplayIntegerProperty $kick callbackCount "$role native auto callback count" 1 1) -ne 1) {
            throw "$role native auto-battle identity differs from the observed live battle"
        }
        if ((Get-LobbyGameplayProperty $close role) -cne $role -or
            (Get-LobbyGameplayIntegerProperty $close appearance "$role native close appearance" 1) -ne $identity.appearance -or
            (Get-LobbyGameplayIntegerProperty $close owner "$role native close owner" 1) -ne $identity.owner -or
            (Get-LobbyGameplayIntegerProperty $close callbackCount "$role native close callback count" 1 1) -ne 1) {
            throw "$role native result-close identity differs from the observed live battle"
        }
        $commit=Get-LobbyGameplayIntegerProperty $kick committedTick64 "$role native committedTick64" 1 ([long]::MaxValue)
        $resultObserved=Get-LobbyGameplayIntegerProperty $close observedTick "$role native result observedTick" 1 ([long]::MaxValue)
        if ($resultObserved -le $commit) { throw "$role native battle interval is not positive" }
        $roles[$role]=[pscustomobject]@{role=$role;dialog=$identity.dialog;appearance=$identity.appearance;owner=$identity.owner
            committedTick64=$commit;resultObservedTick64=$resultObserved;intervalMs=($resultObserved-$commit)}
    }
    $start=[Math]::Max([long]$roles.host.committedTick64,[long]$roles.join.committedTick64)
    $end=[Math]::Min([long]$roles.host.resultObservedTick64,[long]$roles.join.resultObservedTick64)
    if ($end -le $start) { throw 'Native battle intervals do not overlap strictly; cached UI is not sufficient' }
    [pscustomobject]@{passed=$true;source='native-monotonic-auto-commit-to-result-overlap'
        clock='same-host-GetTickCount64';overlapStartTick64=$start;overlapEndTick64=$end;provenOverlapMs=($end-$start)
        roles=[pscustomobject]$roles;cachedUiUsedForIdentityOnly=$true}
}

function Get-LobbyMergeProof {
    param([Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$HostLines,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$JoinLines,
        [Parameter(Mandatory)]$HostWorld, [Parameter(Mandatory)]$JoinWorld,
        [ValidateRange(2,30)][int]$ExpectedDay = 3)
    # The caller MUST pass only tails after its own launch boundary. These logs
    # prove native client transitions, not the independent server transaction ledger.
    $hostId = Assert-LobbyGameplayWorld $HostWorld host
    $joinId = Assert-LobbyGameplayWorld $JoinWorld join
    if ($hostId -eq $joinId) { throw 'Merge worlds have identical local players' }
    $sources = @{host = $HostLines; join = $JoinLines}; $roles = @{}; $missing = [Collections.Generic.List[string]]::new()
    foreach ($role in @('host','join')) {
        $lines = $sources[$role]
        if (@($lines | Where-Object { $_ -match '\[simturns\] terminal fault:|\[simturns-diag\].*(local_abort|remote_abort|port_fault|native_failed)|\[SIMTURNS\] terminal pipe fault:' }).Count) {
            throw "$role log contains a terminal protocol failure"
        }
        $patterns = [ordered]@{
            prepare = '\[simturns\] prepared merge transaction (?<action>\d+) at stock day (?<day>\d+)$'
            natural = "\[simturns\] accepted $role natural stock BeginTurn \(frame=(?<frame>\d+), addressee=(?<addressee>0x[0-9a-f]+), sequence=(?<sequence>\d+), active=(?<active>0x[0-9a-f]+), mergeDay=(?<day>\d+)\)$"
            applied = '\[simturns\] natural merge BeginTurn applied and drained \(actionId=(?<action>\d+), day=(?<day>\d+)\)$'
            release = '\[simturns\] relay released stock turns \(actionId=(?<action>\d+), day=(?<day>\d+)\)$'
            execute = '\[simturns\] host executed merge transaction (?<action>\d+) via 0x420FFA$'
        }
        $found = @{}
        foreach ($name in $patterns.Keys) {
            $matchesForName = @(for ($index = 0; $index -lt $lines.Count; $index++) {
                $match = [regex]::Match($lines[$index], $patterns[$name])
                if ($match.Success) { [pscustomobject]@{ index = $index; match = $match; line = $lines[$index] } }
            })
            if ($role -eq 'join' -and $name -eq 'execute') {
                if ($matchesForName.Count) { throw 'Join incorrectly executed the authoritative host merge' }
                continue
            }
            if ($matchesForName.Count -gt 1) { throw "$role repeats the $name merge marker" }
            if ($matchesForName.Count -eq 0) { $missing.Add("$role.$name"); continue }
            $found[$name] = $matchesForName[0]
            if ($name -ne 'execute' -and [long]$found[$name].match.Groups['day'].Value -ne $ExpectedDay) {
                throw "$role merge marker uses another day"
            }
        }
        $roles[$role] = $found
    }
    if ($missing.Count) { return [pscustomobject]@{passed=$false;missing=@($missing);expectedDay=$ExpectedDay} }
    $action = [long]$roles.host.prepare.match.Groups['action'].Value
    if ($action -lt 1 -or $action -gt [uint32]::MaxValue) { throw 'Merge action identity is invalid' }
    foreach ($role in @('host','join')) {
        $found = $roles[$role]
        foreach ($name in @('prepare','applied','release')) {
            if ([long]$found[$name].match.Groups['action'].Value -ne $action) { throw "$role merge action identities disagree" }
        }
        if (-not ($found.prepare.index -lt $found.natural.index -and $found.natural.index -lt $found.applied.index -and
            $found.applied.index -lt $found.release.index)) { throw "$role merge causal ordering is invalid" }
        $native = $found.natural.match
        if ([long]$native.Groups['frame'].Value -ne 56 -or $native.Groups['addressee'].Value -cne '0x0' -or
            [long]$native.Groups['sequence'].Value -le 1 -or [long]$native.Groups['sequence'].Value -ge [uint32]::MaxValue -or
            $native.Groups['active'].Value -ine $hostId) { throw "$role natural stock handoff is not the exact host broadcast" }
    }
    $commandSequence=[long]$roles.host.natural.match.Groups['sequence'].Value
    if ($commandSequence -ne [long]$roles.join.natural.match.Groups['sequence'].Value) {
        throw 'Host and join natural merge broadcast sequences disagree'
    }
    $execute = $roles.host.execute
    if ([long]$execute.match.Groups['action'].Value -ne $action -or $execute.index -le $roles.host.prepare.index -or
        $execute.index -ge $roles.host.release.index) { throw 'Host ExecuteMerge has invalid identity or ordering' }
    if ($HostWorld.day -ne $ExpectedDay -or $JoinWorld.day -ne $ExpectedDay) {
        return [pscustomobject]@{passed=$false;missing=@('both-worlds-at-merge-day');expectedDay=$ExpectedDay}
    }
    $evidence = @{}
    foreach ($role in @('host','join')) {
        $evidence[$role] = [ordered]@{}
        foreach ($name in $roles[$role].Keys) {
            $entry = $roles[$role][$name]
            $evidence[$role][$name] = [pscustomobject]@{index=$entry.index;line=$entry.line}
        }
    }
    [pscustomobject]@{ passed=$true; actionId=$action; commandSequence=$commandSequence; day=$ExpectedDay; hostPlayerId=$hostId
        hostWorldSeq=[long]$HostWorld.worldSeq; joinWorldSeq=[long]$JoinWorld.worldSeq
        source='owned-client-native-prepare-broadcast-drain-release'; serverLedgerVerified=$false
        roles=[pscustomobject]@{host=$evidence.host;join=$evidence.join} }
}
