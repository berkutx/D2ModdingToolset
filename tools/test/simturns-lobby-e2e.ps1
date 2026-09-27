#requires -Version 7.0
<#
Owned production-lobby startup E2E. Requires the authorized test2/test1 credentials
in D2_LOBBY_{HOST,JOIN}_{ACCOUNT,PASSWORD}, OH_SITE_ORIGIN and OH_SITE_PACKAGE.
Creates one fresh casual preparation; no manual action JSON or LLM inspection.
The existing native scripted-popup subscriber is the only startup popup actor.
Optional generated-map concurrent battles and merge; never the fixed 18-case campaign.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArtifactDir,
    [string]$GameDir = 'C:\GOG Games\slasher_mns_2_4 - Copy',
    [ValidateRange(60, 1800)][int]$SessionSeconds = 900,
    [ValidateSet(2, 3)][int]$ExpectedMergeDay = 2,
    [ValidateSet('StartupOnly', 'ConcurrentBattles')][string]$Gameplay = 'StartupOnly',
    [ValidateSet('Normal', 'DelayedJoin')][string]$PairStartup = 'Normal',
    [ValidateRange(5, 60)][int]$JoinStartupDelaySeconds = 8
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repo 'tools/test/_relay.ps1')
. (Join-Path $repo 'tools/test/simturns-lobby-gameplay.ps1')
. (Join-Path $repo 'tools/test/simturns-lobby-gameplay-run.ps1')

function Property($Object, [string]$Name, $Fallback = $null) {
    if ($null -eq $Object) { return $Fallback }
    $p = $Object.PSObject.Properties[$Name]
    if ($null -eq $p) { return $Fallback }
    return $p.Value
}
function Assert-NewArtifactDirectory([string]$Path, [string]$Root) {
    $destination = [IO.Path]::GetFullPath($Path).TrimEnd([char[]]'\/')
    $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    if (-not $destination.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ArtifactDir must be a new directory below this checkout artifacts.'
    }
    # Inspect the whole existing ancestry, including ancestors above artifacts.
    # Get-Item also observes dangling reparse points that Test-Path may hide.
    $ancestor = $destination
    while ($ancestor) {
        $item = $null
        try { $item = Get-Item -LiteralPath $ancestor -Force -ErrorAction Stop }
        catch [Management.Automation.ItemNotFoundException] { }
        if ($null -ne $item) {
            if ($ancestor -ceq $destination) { throw 'ArtifactDir must be a new directory below this checkout artifacts.' }
            if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Artifact ancestors must be real directories without reparse points.'
            }
        }
        $parent = Split-Path -Parent $ancestor
        if ($parent -ceq $ancestor) { break }
        $ancestor = $parent
    }
    return $destination
}
function New-OwnedArtifactDirectory([string]$Path, [string]$Root) {
    if ($script:ArtifactDirectoryOwned) { throw 'Artifact directory ownership was already acquired.' }
    $destination = Assert-NewArtifactDirectory $Path $Root
    $staging = Join-Path (Split-Path -Parent $destination) ('.lobby-e2e-claim-' + [guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $staging -ErrorAction Stop)
    try {
        [void](Assert-NewArtifactDirectory $destination $Root)
        # Directory.Move fails atomically if another invocation claimed the final
        # path. New-Item/CreateDirectory alone may race between check and create.
        [IO.Directory]::Move($staging, $destination)
        $script:ArtifactDirectoryOwned = $true
    } finally {
        # Only this empty staging directory is disposable; never recurse or
        # remove a destination that may belong to another invocation.
        if ([IO.Directory]::Exists($staging)) { [IO.Directory]::Delete($staging, $false) }
    }
}
function Assert-OwnedArtifactDirectory {
    if (-not $script:ArtifactDirectoryOwned) { throw 'Artifact directory ownership was not acquired.' }
}
function Save-Json([string]$Name, $Value) {
    Assert-OwnedArtifactDirectory
    $destination = Join-Path $ArtifactDir $Name
    $temporary = Join-Path $ArtifactDir ('.{0}.{1}.tmp' -f $Name, [guid]::NewGuid().ToString('N'))
    $json = $Value | ConvertTo-Json -Depth 40
    $isLatest = $Name -ceq 'latest.json'
    $attempts = if ($isLatest) { 4 } else { 1 }
    try {
        # Close a complete same-directory file before atomically replacing the public name.
        # Readers can therefore observe either complete version, never a truncated JSON write.
        [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
        for ($attempt = 1; $attempt -le $attempts; $attempt++) {
            try {
                if ([IO.File]::Exists($destination)) { [IO.File]::Replace($temporary, $destination, [NullString]::Value) }
                else { [IO.File]::Move($temporary, $destination) }
                return
            } catch {
                $cause = $_.Exception.GetBaseException()
                $win32Error = $cause.HResult -band 0xffff
                if (-not $isLatest -or $cause -isnot [IO.IOException] -or $win32Error -notin @(32, 33)) {
                    throw # Critical receipts and non-sharing errors must not be discarded.
                }
                if ($attempt -lt $attempts) { Start-Sleep -Milliseconds 25 }
            }
        }
        # A reader without FILE_SHARE_DELETE may keep the previous observation open. It cannot
        # terminate gameplay: discard only this latest snapshot and publish again on the next loop.
        $script:LatestSnapshotSharingSkips++
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}
function Protect-UiSnapshot($Snapshot) {
    if ($null -eq $Snapshot) { return }
    foreach ($w in @(Property $Snapshot 'widgets' @())) {
        if ((Property $w 'type') -eq 'edit' -or [string](Property $w 'name') -match '^EDIT_') {
            $s = Property $w 'state'
            if ($s -and $s.PSObject.Properties['text']) { $s.text = '[redacted]' }
        }
    }
    foreach ($target in @(Property $Snapshot 'targets' @())) { Protect-UiSnapshot $target }
}
function Get-LogLaunchBoundary {
    $lengths = @{}
    foreach ($file in Get-ChildItem -LiteralPath $GameDir -Filter 'mss32_*.log' -File) {
        $lengths[$file.Name] = [long]$file.Length
    }
    return $lengths
}
function Assert-FreshOwnedLog([string]$Role, [hashtable]$PreLaunchLengths) {
    $name = "mss32_$($script:Clients[$Role].Id).log"
    # main.cpp uses an append logger. Windows may reuse an old PID; its earlier
    # bytes are not this run's evidence. The shared reader checks truncation.
    $offset = if ($PreLaunchLengths.ContainsKey($name)) { [long]$PreLaunchLengths[$name] } else { [long]0 }
    if ($offset -lt 0) { throw 'Owned log launch boundary is invalid.' }
    $script:ClientLogs[$Role] = [IO.Path]::GetFullPath((Join-Path $GameDir $name))
    $script:ClientLogInitialLengths[$script:ClientLogs[$Role]] = $offset
    $script:ClientLogOwnedProcessIds[$script:ClientLogs[$Role]] = [long]$script:Clients[$Role].Id
}
function Copy-OwnedLogTail([string]$Role) {
    Assert-OwnedArtifactDirectory
    $source = $script:ClientLogs[$Role]
    $offset = Get-ClientLogBaseline $source
    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $inputLog = [IO.FileStream]::new($source, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
    try {
        if ($inputLog.Length -lt $offset) { throw 'Owned log was truncated before evidence copy.' }
        [void]$inputLog.Seek($offset, [IO.SeekOrigin]::Begin)
        $outputLog = [IO.FileStream]::new((Join-Path $ArtifactDir "$Role.mss32.log"), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try { $inputLog.CopyTo($outputLog) } finally { $outputLog.Dispose() }
    } finally { $inputLog.Dispose() }
}
function Get-ProtectedFileHashes([string[]]$Files) {
    $hashes = @{}
    foreach ($file in $Files) {
        $path = Join-Path $GameDir $file
        $hashes[$file] = if (Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { $null }
    }
    return $hashes
}
function Compare-ProtectedFileHashes([hashtable]$Before, [hashtable]$After) {
    $changed = @(@($Before.Keys) + @($After.Keys) | Sort-Object -Unique | Where-Object {
        -not $Before.ContainsKey($_) -or -not $After.ContainsKey($_) -or $Before[$_] -cne $After[$_]
    })
    [pscustomobject]@{ unchanged=($changed.Count -eq 0); changedFiles=$changed; before=$Before; after=$After }
}
function Get-OwnedLogEvidence([string]$Role) {
    if (-not $script:ClientLogs.ContainsKey($Role)) { throw 'Owned log has not passed the freshness check.' }
    $log = $script:ClientLogs[$Role]
    $exists = Test-Path -LiteralPath $log
    $text = if ($exists) { (Read-ClientLogLines $log) -join "`n" } else { '' }
    $item = if ($exists) { Get-Item -LiteralPath $log } else { $null }
    $baseline = Get-ClientLogBaseline $log
    return [ordered]@{
        exists = $exists; bytes = if ($item) { $item.Length } else { 0 }
        baselineBytes = $baseline; runBytes = if ($item) { $item.Length - $baseline } else { 0 }
        lastWriteUtc = if ($item) { $item.LastWriteTimeUtc.ToString('o') } else { $null }
        bootstrapOperational = $text -match 'bootstrap operational release applied; strict independent turns are operational'
        startupLeaderNameSent = $text -match 'bootstrap first-leader-name TX sent \(role=host,'
        stockTurnsReleased = $text -match "relay released stock turns \(actionId=\d+, day=$ExpectedMergeDay\)"
        fault = $text -match '\[simturns(?:-diag)?\].*(?:FAULT|terminal fault|native_failed|local_abort|remote_abort|port_fault)|\[testdrv\].*(?:terminal invariant failure|FAILFAST|remote fault)'
        scriptError = $text -match "\[E\] Failed to run '[^'\r\n]+' script\."
    }
}
function Save-StartupTimeout([string]$Role, [string]$Dialog) {
    $roles = @{}; $evidence = @{}
    foreach ($ownedRole in $script:Clients.Keys) {
        $roles[$ownedRole] = Get-Observation $ownedRole -AllowPartial
        try { $evidence[$ownedRole] = Get-OwnedLogEvidence $ownedRole }
        catch { $evidence[$ownedRole] = @{ observationError = 'Owned log observation failed.' } }
    }
    $script:logEvidence = $evidence
    $script:last = [ordered]@{
        utc = [DateTime]::UtcNow.ToString('o'); preparationId = $PreparationId
        stage = 'startup_timeout'; waitingRole = $Role; waitingDialog = $Dialog
        roles = $roles; evidence = $evidence
    }
    Save-Json 'startup-timeout.json' $script:last
    Save-Json 'latest.json' $script:last
}
function Assert-OwnedAlive {
    foreach ($p in $script:Clients.Values) {
        $p.Refresh()
        if ($p.HasExited) { throw "Owned game pid=$($p.Id) exited with code $($p.ExitCode)." }
    }
    if ($script:Relay) {
        $script:Relay.Refresh()
        if ($script:Relay.HasExited) { throw 'Owned relay exited.' }
    }
}
function Wait-Ready([string]$Role, [string]$Dialog) {
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-OwnedAlive
        if ((Get-PairUtcNow) -ge $script:RunDeadline) { throw 'Bounded lobby E2E deadline expired.' }
        if (Test-DialogReady $Role $Dialog) { return }
        $ui = Get-GameUiSnapshot $Role
        if ($ui.dialogReady -eq $true -and $ui.dialog -ceq 'DLG_MESSAGE_BOX' -and
            -not @($script:Consumed.Keys | Where-Object { $_ -like "$Role|$($ui.dialogAppearance)|*" }).Count) {
            throw "Unexpected message while waiting for $Role $Dialog; no callback issued."
        }
        Start-Sleep -Milliseconds 200
    }
    try { Save-StartupTimeout $Role $Dialog }
    catch { Write-Warning 'Could not save the startup-timeout snapshot; the original timeout is preserved.' }
    throw "Timed out waiting for $Role $Dialog; no action retried."
}
function Press-Once([string]$Role, [string]$Dialog, [string]$Button) {
    if (-not (Invoke-ButtonOncePerAppearance $Role $Dialog $Button $script:Consumed)) {
        throw "$Role $Dialog/$Button already consumed."
    }
}
function Get-Observation([string]$Role, [switch]$AllowPartial) {
    $errors = [Collections.Generic.List[string]]::new()
    $state = $null; $ui = $null; $world = $null
    try { $state = Get-RoleState $Role }
    catch { if (-not $AllowPartial) { throw }; $errors.Add('Native state observation failed.') }
    if (-not $state -and -not $AllowPartial) { return $null }
    try { $ui = Get-GameUi $Role }
    catch { if (-not $AllowPartial) { throw }; $errors.Add('Native UI observation failed.') }
    # State and rich UI are separate JSON reads, each with nested owner widgets.
    Protect-UiSnapshot $state
    Protect-UiSnapshot $ui
    try { $world = Get-World $Role }
    catch { if (-not $AllowPartial) { throw }; $errors.Add('Native world observation failed.') }
    return [ordered]@{
        state = if ($state) { [ordered]@{
            connected = [bool]$state.connected; pid = $state.pid; dialog = $state.dialog
            dialogReady = Property $state 'dialogReady' $false
            dialogAppearance = Property $state 'dialogAppearance' 0
            uiSeq = Property $state 'uiSeq' 0
            reachedStrategic = Property $state 'reachedStrategic' $false
            targets = @(Property $state 'targets' @())
            buttons = @(Property $state 'buttons' @())
        } } else { $null }
        ui = $ui; world = $world; observationErrors = $errors.ToArray()
    }
}
function Get-PairUtcNow { [DateTime]::UtcNow }
function Save-PairReceipt([string]$Stage, $Proof = $null) {
    $script:PairEvents.Add([ordered]@{
        utc = (Get-PairUtcNow).ToString('o'); stage = $Stage; proof = $Proof
    })
    Save-Json 'pair-start.json' ([ordered]@{
        preparationId = $PreparationId; mode = $PairStartup
        delaySeconds = $JoinStartupDelaySeconds; events = $script:PairEvents.ToArray()
    })
}
function Invoke-ReadyRoomStart([string]$Role) {
    Assert-OwnedRelayClientIdentity $Role $script:Clients[$Role] $GameDir
    $observed = Get-GameUiSnapshot $Role
    if ([string]$observed.dialog -cne 'DLG_LOBBY' -or
        [long]$observed.uiSeq -lt 1) { throw "Owned $Role is not in its native room." }
    # The arriving peer may rebind the same room between observation and arm.
    # Wait for readiness in the existing single native intent, not in a second
    # click/retry loop. Its receipt below still must prove one enabled owner.
    $after = [long]$observed.uiSeq - 1
    # The relay owns the final enabled/appearance/owner check and the host stability interval.
    # Rebinding only updates this still-unissued intent; there is exactly one POST/native command.
    $stable = if ($Role -eq 'host') { '&stableMs=500' } else { '' }
    $uri = "$script:RelayBase/api/ui/invoke-when-ready?role=$Role&dlg=DLG_LOBBY&btn=BTN_OK&after=$after&waitMs=30000$stable&timeoutMs=90000"
    Save-PairReceipt "$Role-start-armed" @{ afterUiSequence = $after }
    $response = Invoke-RestMethod -Uri $uri -Method POST -TimeoutSec 130
    $proof = Property $response 'observation'; $invoke = Property $response 'invoke'
    if ((Property $response 'found') -isnot [bool] -or -not $response.found -or
        [string](Property $response 'role') -cne $Role -or
        [string](Property $invoke 'dlg') -cne 'DLG_LOBBY' -or
        [string](Property $invoke 'btn') -cne 'BTN_OK' -or
        [string](Property $proof 'role') -cne $Role -or
        [string](Property $proof 'dialog') -cne 'DLG_LOBBY' -or
        (Property $proof 'dialogReady') -ne $true -or
        [long](Property $proof 'uiSeq' 0) -le $after -or
        [long](Property $invoke 'appearance' 0) -lt 1 -or
        [long](Property $invoke 'instance' 0) -lt 1 -or
        [long]$invoke.appearance -ne [long](Property $proof 'dialogAppearance' 0) -or
        [long]$invoke.appearance -ne [long](Property $proof 'dialogInstance' 0)) {
        throw 'Room-start intent did not return its exact ready native proof; no retry permitted.'
    }
    $targets = @($proof.targets | Where-Object {
        $_.dialog -ceq 'DLG_LOBBY' -and [long]$_.instance -eq [long]$invoke.instance
    })
    $buttons = if ($targets.Count -eq 1) {
        @($targets[0].widgets | Where-Object { $_.name -ceq 'BTN_OK' -and $_.type -ceq 'button' })
    } else { @() }
    if ($buttons.Count -ne 1 -or $buttons[0].state.enabled -ne $true) {
        throw 'Room-start intent omitted its exact enabled control proof.'
    }
    Protect-UiSnapshot $proof
    Save-PairReceipt "$Role-start-issued" $response
}
function Assert-PairProgress {
    Assert-OwnedAlive
    if ((Get-PairUtcNow) -ge $script:RunDeadline) { throw 'Bounded lobby E2E deadline expired.' }
    foreach ($role in @('host', 'join')) {
        $script:logEvidence[$role] = Get-OwnedLogEvidence $role
        if ($script:logEvidence[$role].scriptError) {
            Save-PairReceipt 'game-script-error' @{ role = $role; evidence = $script:logEvidence[$role] }
            throw "Observed game Lua error for $role; no error popup dismissed or action retried. See owned PID log."
        }
        if ($script:logEvidence[$role].fault) {
            Save-PairReceipt 'production-fault' @{ role = $role; evidence = $script:logEvidence[$role] }
            throw 'Observed production OH fault during owned test; no action retried.'
        }
        if ($script:PopupObservers.ContainsKey($role)) {
            [void](Invoke-LiteralPersistentStartupPopupTick $script:PopupObservers[$role])
        }
    }
    $stopPath = Join-Path $ArtifactDir 'stop.json'
    if (Test-Path -LiteralPath $stopPath) {
        $stop = Get-Content -LiteralPath $stopPath -Raw | ConvertFrom-Json
        if ([string]$stop.preparationId -cne $PreparationId) { throw 'Stop signal targets another preparation.' }
        $script:stopped = $true
        throw 'Caller stopped the bounded pair startup.'
    }
}
function Start-OwnedPair {
    if ($script:PairStartClaimed) { throw 'Pair startup intent was already consumed.' }
    if (-not $script:PreparationConsent.host -or -not $script:PreparationConsent.join -or
        -not $script:GenerationAccepted) {
        throw 'Pair startup requires this run''s explicit host/join consent and generation acceptance.'
    }
    $script:PairStartClaimed = $true # Claim before either callback; no retries after a partial run.
    Save-PairReceipt 'claimed'
    Invoke-ReadyRoomStart host
    $deadline = (Get-PairUtcNow).AddSeconds(180)
    $hostMap = $false; $nameObservedAt = $null; $hostReleaseIssued = $false
    while ((Get-PairUtcNow) -lt $deadline) {
        Assert-PairProgress
        $state = Get-RoleState host
        if (-not $hostMap -and (Property $state 'reachedStrategic' $false)) {
            $hostMap = $true
            Save-PairReceipt 'host-map-observed' @{ uiSeq = Property $state 'uiSeq' 0 }
        }
        if ($hostMap -and $PairStartup -eq 'Normal') { break }
        if ($hostMap -and $PairStartup -eq 'DelayedJoin' -and -not $hostReleaseIssued) {
            $current = Get-GameUiSnapshot host
            if ($current.dialogReady -eq $true -and $current.mapLoaded -eq $true -and
                $current.startupActionsHeld -eq $true) {
                $hostReleaseIssued = $true # Claim before the one diagnostic release request.
                Request-DelayedHostStartupRelease
            }
        }
        if ($PairStartup -eq 'DelayedJoin' -and $script:logEvidence.host.startupLeaderNameSent) {
            if ($null -eq $nameObservedAt) {
                $nameObservedAt = Get-PairUtcNow
                Save-PairReceipt 'host-name-tx-observed-delay-started'
            }
            if ($hostMap -and (Get-PairUtcNow) -ge $nameObservedAt.AddSeconds($JoinStartupDelaySeconds)) {
                Save-PairReceipt 'join-delay-completed' @{ elapsedSeconds = ((Get-PairUtcNow) - $nameObservedAt).TotalSeconds }
                break
            }
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not $hostMap -or (Get-PairUtcNow) -ge $deadline) { throw 'Host map/name-delay boundary timed out.' }
    Invoke-ReadyRoomStart join
    $deadline = (Get-PairUtcNow).AddSeconds(180)
    while ((Get-PairUtcNow) -lt $deadline) {
        Assert-PairProgress
        $state = Get-RoleState join
        if (Property $state 'reachedStrategic' $false) {
            $script:PairMapsObserved = $true
            Save-PairReceipt 'both-maps-observed' @{ joinUiSeq = Property $state 'uiSeq' 0 }
            return # Ordinary first-turn popups may now proceed; this is not an OH/gameplay PASS.
        }
        Start-Sleep -Milliseconds 200
    }
    throw 'Join map boundary timed out; no startup action was retried.'
}

function Import-ExistingPopupObservers([string]$SourcePath = (Join-Path $PSScriptRoot 'simturns-production-poc.ps1')) {
    # Same AST-only extraction used by campaign tests: never run its main body.
    # These are observation-only functions; native scriptedpopups remains the sole actor.
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($SourcePath, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw 'Existing campaign observer source did not parse.' }
    foreach ($name in @('Get-ClientLogBaseline', 'Read-ClientLogLines',
        'New-LiteralStartupPopupService', 'Invoke-LiteralPersistentStartupPopupTick',
        'Assert-PairedStartupActionsRelease')) {
        $nodes = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $false))
        if ($nodes.Count -ne 1) { throw "Existing campaign observer '$name' is not unique." }
        $definition = $nodes[0].Extent.Text -replace ('^function\s+' + [regex]::Escape($name)), "function script:$name"
        . ([scriptblock]::Create($definition))
    }
}

function Invoke-OwnedPreparation([ValidateSet('create', 'start', 'detail', 'close')][string]$Action) {
    Assert-OwnedArtifactDirectory
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = (Get-Command node -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $psi.ArgumentList.Add((Join-Path $PSScriptRoot 'simturns-lobby-preparation.mjs'))
    $psi.ArgumentList.Add($Action)
    # Passwords are inherited only by this exact helper, never arguments or game/relay children.
    foreach ($key in @($psi.Environment.Keys)) {
        if ($key -like 'D2_LOBBY_*') { [void]$psi.Environment.Remove($key) }
    }
    $psi.Environment['D2_LOBBY_HOST_ACCOUNT'] = $credentials.host.ACCOUNT
    $psi.Environment['D2_LOBBY_HOST_PASSWORD'] = $credentials.host.PASSWORD
    $psi.Environment['D2_LOBBY_JOIN_ACCOUNT'] = $credentials.join.ACCOUNT
    $psi.Environment['OH_PREPARATION_RECORD_PATH'] = Join-Path $ArtifactDir 'owned-preparation.json'
    $psi.Environment['OH_SIMULTANEOUS_UNTIL'] = [string]$ExpectedMergeDay
    $process = [Diagnostics.Process]::Start($psi)
    try {
        $outputTask = $process.StandardOutput.ReadToEndAsync()
        $errorTask = $process.StandardError.ReadToEndAsync()
        $helperDeadline = [DateTime]::UtcNow.AddSeconds(90) # login + connect + at most three bounded ACKs
        while (-not $process.WaitForExit(200)) {
            if ([DateTime]::UtcNow -ge $helperDeadline) {
                $process.Kill(); $process.WaitForExit()
                throw "Preparation helper '$Action' timed out; no retry allowed."
            }
        }
        $output = $outputTask.GetAwaiter().GetResult()
        [void]$errorTask.GetAwaiter().GetResult() # Never serialize arbitrary dependency errors.
        try { $result = $output | ConvertFrom-Json -ErrorAction Stop }
        catch { throw "Preparation helper '$Action' omitted its machine receipt; details suppressed." }
        if ($process.ExitCode -ne 0 -or (Property $result 'ok') -ne $true) {
            $code = [string](Property $result 'error' 'operation_failed')
            if ($code -notmatch '^[a-z_]{1,80}$') { $code = 'operation_failed' }
            throw "Preparation helper '$Action' failed ($code); no retry allowed."
        }
        if ([string](Property $result 'action') -cne $Action) { throw 'Preparation receipt action mismatch.' }
        $preparation = Property $result 'preparation'
        if (-not $preparation -or ($PreparationId -and [string]$preparation.id -cne $PreparationId)) {
            throw 'Preparation receipt identity mismatch.'
        }
        Save-Json "preparation-$Action.json" $result
        return $preparation
    } finally { $process.Dispose() }
}

function ConvertTo-NativeReportedText([string]$Text) {
    # UI reporter preserves each native CP1251 byte as a Latin-1 JSON code point.
    return [Text.Encoding]::Latin1.GetString([Text.Encoding]::GetEncoding(1251).GetBytes($Text))
}
function Get-PreparedPrompt([string]$Role, [string]$Title) {
    if (-not $Title -or $Title.Contains("`n") -or $Title.Contains("`r")) { throw 'Template title is not one exact line.' }
    if ($Role -ceq 'host') {
        return ConvertTo-NativeReportedText "$Title · без рейтинга`ntest2 — Эльфы (хост)`ntest1 — Кланы`n1-й ход: test2`nОХ: объединение на день $ExpectedMergeDay`nСгенерировать карту?"
    }
    if ($Role -ceq 'join') {
        return ConvertTo-NativeReportedText "$Title`nХост: test2`nКарта готова.`nВойти в комнату?"
    }
    throw 'Unknown prepared prompt role.'
}
function Get-ExactEntryAction($Ui, [string]$Role, [string]$Stage, [string]$ExpectedText = '') {
    if ($Role -notin @('host', 'join') -or $Stage -notin @('host-offer', 'generation', 'join-offer')) {
        throw 'Entry stage/role is not allowlisted.'
    }
    if (($Stage -eq 'join-offer') -ne ($Role -eq 'join')) { throw 'Entry stage belongs to another role.' }
    if ((Property $Ui 'dialogReady') -isnot [bool]) { throw 'Native UI readiness shape is invalid.' }
    if (-not $Ui.dialogReady) { return $null }
    $dialog = [string](Property $Ui 'dialog')
    $expectedDialog = if ($Stage -eq 'generation') { 'DLG_GENERATION_RESULT' } else { 'DLG_MESSAGE_BOX' }
    $button = if ($Stage -eq 'generation') { 'BTN_ACCEPT' } else { 'BTN_YES' }
    if ($dialog -eq 'DLG_MESSAGE_BOX') {
        $texts = @($Ui.widgets | Where-Object { $_.name -ceq 'TXT_INFO' -and $_.type -ceq 'text' })
        if ($Stage -eq 'generation' -or -not $ExpectedText -or $texts.Count -ne 1 -or
            (Property $texts[0].state 'text') -isnot [string] -or $texts[0].state.text -cne $ExpectedText) {
            throw 'Unknown or mismatched message box; no callback issued.'
        }
    }
    if ($dialog -cne $expectedDialog) { return $null }
    $appearance = [long](Property $Ui 'dialogAppearance' 0)
    if ($appearance -lt 1 -or $appearance -gt [uint32]::MaxValue -or
        $appearance -ne [long](Property $Ui 'dialogInstance' 0)) { throw 'Entry UI appearance is invalid.' }
    $targets = @($Ui.targets | Where-Object { $_.dialog -ceq $dialog })
    if ($targets.Count -ne 1) { throw 'Entry UI must identify exactly one native owner.' }
    $owner = [long]$targets[0].instance
    $buttons = @($targets[0].widgets | Where-Object { $_.name -ceq $button -and $_.type -ceq 'button' })
    if ($owner -lt 1 -or $owner -gt [uint32]::MaxValue -or $buttons.Count -ne 1 -or
        $buttons[0].state.enabled -isnot [bool] -or -not $buttons[0].state.enabled) {
        throw 'Entry control is not one explicitly enabled native callback.'
    }
    [pscustomobject]@{ Role=$Role; Stage=$Stage; Dialog=$dialog; Button=$button; Appearance=$appearance; Owner=$owner }
}
function Complete-PreparedEntry([string]$Role, [string]$Stage, [string]$ExpectedText = '') {
    $deadline = (Get-PairUtcNow).AddSeconds(180)
    while ((Get-PairUtcNow) -lt $deadline) {
        Assert-PairProgress
        $ui = Get-GameUiSnapshot $Role
        # A successful callback can precede the next bridge publication. Observe its
        # consumed appearance departing; never classify or invoke that stale copy again.
        if (Test-ConsumedEntryAppearance $ui $Role) { Start-Sleep -Milliseconds 100; continue }
        $action = Get-ExactEntryAction $ui $Role $Stage $ExpectedText
        if ($action) {
            # Revalidate the site-owned preparation before a native consent/generation callback.
            $script:Preparation = Invoke-OwnedPreparation detail
            if ($script:Preparation.status -in @('cancelled', 'completed', 'cancelling', 'failed', 'deferred', 'review') -or
                -not $script:Preparation.launch -or $script:Preparation.launch.hasError) {
                throw 'Prepared launch is no longer active; no native callback issued.'
            }
            $key = "$Role|$($action.Appearance)|$($action.Owner)|$($action.Dialog)|$($action.Button)"
            if ($script:Consumed.ContainsKey($key)) { throw 'Entry callback was already claimed.' }
            $script:Consumed[$key] = $true
            Save-PairReceipt "$Stage-claimed" $action
            if (-not (Invoke-Button $Role $action.Dialog $action.Button $action.Owner $action.Appearance)) {
                throw 'Entry callback was not delivered; no retry permitted.'
            }
            Save-PairReceipt "$Stage-completed" $action
            if ($Stage -eq 'generation') { $script:GenerationAccepted = $true }
            else { $script:PreparationConsent[$Role] = $true }
            return
        }
        Start-Sleep -Milliseconds 200
    }
    throw "Timed out at prepared entry stage '$Stage'; no callback retried."
}

function Test-ConsumedEntryAppearance($Ui, [string]$Role) {
    $appearance = [long](Property $Ui 'dialogAppearance' 0)
    if ($appearance -lt 1) { return $false }
    foreach ($target in @(Property $Ui 'targets' @())) {
        $prefix = "$Role|$appearance|$($target.instance)|$($target.dialog)|"
        if (@($script:Consumed.Keys | Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) }).Count) { return $true }
    }
    return $false
}

function Test-StartupRoleAccepted($Ui, [bool]$BootstrapOperational) {
    # Same two co-present bare-map roots as the existing production campaign.
    # ISO_PAL may be the last publisher; it is not an unresolved modal dialog.
    return $BootstrapOperational -and $Ui.mapLoaded -eq $true -and
        $Ui.dialogReady -eq $true -and $Ui.strategicIdle -eq $true -and
        $Ui.dialog -cin @('DLG_STRATEGIC', 'DLG_ISO_PAL')
}
function Wait-StartupAcceptance {
    $deadline = (Get-PairUtcNow).AddSeconds(180)
    while ((Get-PairUtcNow) -lt $deadline) {
        Assert-PairProgress
        $roles = @{}; $ready = $true
        foreach ($role in @('host', 'join')) {
            $roles[$role] = Get-Observation $role
            $ui = Get-GameUiSnapshot $role
            $ready = $ready -and (Test-StartupRoleAccepted $ui $script:logEvidence[$role].bootstrapOperational)
        }
        $script:last = @{ utc=(Get-PairUtcNow).ToString('o'); preparationId=$PreparationId; roles=$roles; evidence=$script:logEvidence }
        Save-Json 'latest.json' $script:last
        if ($ready) {
            foreach ($observer in $script:PopupObservers.Values) {
                if (@($observer.Appearances.Values | Where-Object State -cne 'COMMITTED').Count) {
                    throw 'Ready map has an uncommitted native popup receipt.'
                }
            }
            $releaseProof = Assert-PairedStartupActionsRelease $script:Clients.host.Id $script:Clients.join.Id $script:ClientLogs.host $script:ClientLogs.join
            if ($PairStartup -eq 'DelayedJoin' -and (Property $releaseProof.relayRelease 'mode') -cne 'delayed-join') {
                throw 'Delayed startup omitted its diagnostic release proof.'
            }
            Save-Json 'startup-release.json' $releaseProof
            Save-PairReceipt 'startup-accepted' @{ scope='both native maps, popup receipts, bootstrap operational, strategic idle'; fullGameplayAcceptance=$false }
            return
        }
        Start-Sleep -Milliseconds 200
    }
    throw 'Startup acceptance timed out; map presence alone is not a PASS.'
}

function Request-DelayedHostStartupRelease {
    Assert-OwnedRelayClientIdentity host $script:Clients.host $GameDir
    $ui = Get-GameUiSnapshot host
    if ($ui.dialogReady -ne $true -or $ui.mapLoaded -ne $true -or $ui.startupActionsHeld -ne $true -or
        (Property $ui 'lobbyStartupPopups') -ne $true) { throw 'Delayed host release lacks exact native lobby popup admission.' }
    $targets = @($ui.targets | Where-Object { $_.dialog -ceq $ui.dialog })
    if ($targets.Count -ne 1) { throw 'Delayed host release lacks one current owner.' }
    $pidValue = [long]$script:Clients.host.Id
    $appearance = [long]$ui.dialogAppearance; $owner = [long]$targets[0].instance; $sequence = [long]$ui.uiSeq
    if ($pidValue -lt 1 -or $appearance -lt 1 -or $owner -lt 1 -or $sequence -lt 1) { throw 'Delayed release identity invalid.' }
    Save-PairReceipt 'diagnostic-host-release-claimed' @{ pid=$pidValue; appearance=$appearance; owner=$owner; uiSeq=$sequence }
    $relayInstance = [string]$script:Relay.RelayInstanceId
    if ($relayInstance -notmatch '^[a-fA-F0-9]{32}$') { throw 'Owned relay instance identity invalid.' }
    $uri = "$script:RelayBase/api/startup/release-host?fixture=delayed-join&role=host&pid=$pidValue&appearance=$appearance&instance=$owner&uiSeq=$sequence&relayInstance=$relayInstance"
    $receipt = Invoke-RestMethod -Uri $uri -Method POST -TimeoutSec 10
    if ([string](Property $receipt 'mode') -cne 'delayed-join' -or [string](Property $receipt 'role') -cne 'host' -or
        [string](Property $receipt 'relayInstance') -cne $relayInstance -or [long](Property $receipt 'pid' 0) -ne $pidValue -or
        [long](Property $receipt 'appearance' 0) -ne $appearance -or [long](Property $receipt 'owner' 0) -ne $owner -or
        [long](Property $receipt 'uiSeq' 0) -ne $sequence -or -not (Property $receipt 't')) {
        throw 'Delayed host release did not return its exact owned native receipt; no retry permitted.'
    }
    Save-Json 'diagnostic-host-release.json' $receipt
}

if ($env:OH_SITE_ORIGIN -eq $null -or $env:OH_SITE_PACKAGE -eq $null) { throw 'Explicit site origin/package configuration required.' }
$GameDir = Resolve-GameDir $GameDir
if ([IO.Path]::GetFullPath($GameDir).TrimEnd('\') -ine 'C:\GOG Games\slasher_mns_2_4 - Copy') {
    throw 'Only the authorized Copy game directory is allowed.'
}
$section = ''; $displayErrors = $false
foreach ($line in Get-Content -LiteralPath (Join-Path $GameDir 'Disciple.ini')) {
    if ($line -match '^\s*\[([^]]+)\]') { $section = $Matches[1]; continue }
    if ($section -ieq 'Disciple' -and $line -match '^\s*DisplayErrors\s*=\s*(\d+)\s*(?:;.*)?$') { $displayErrors = $Matches[1] -eq '1' }
}
if (-not $displayErrors) { throw 'DisplayErrors=1 is required; configuration will not be changed.' }
$artifactRoot = Join-Path $repo 'artifacts'
$ArtifactDir = Assert-NewArtifactDirectory $ArtifactDir $artifactRoot
$script:ArtifactDirectoryOwned = $false
$script:PreparationId = ''; $script:Preparation = $null
$script:Clients = @{}; $script:Relay = $null; $script:Consumed = @{}; $script:ClientLogs = @{}
$script:ClientLogInitialLengths = @{}; $script:ClientLogOwnedProcessIds = @{}
$script:PopupObservers = @{}; $script:StartupModalSettleMilliseconds = 300
$script:LastStartupPopupEvidenceTick = @{ host=[long]0; join=[long]0 }
$script:LastStartupPopupEvidenceUtc = @{ host=$null; join=$null }
$script:PreparationConsent = @{ host=$false; join=$false }; $script:GenerationAccepted = $false
$script:PairStartClaimed = $false; $script:PairMapsObserved = $false
$script:PairEvents = [Collections.Generic.List[object]]::new(); $script:LatestSnapshotSharingSkips = 0
$credentials = @{}; $envBefore = @{}; $failure = $null; $failureStack = $null; $stopped = $false; $accepted = $false
$script:GameplayProof = $null
$cleanupErrors = [Collections.Generic.List[string]]::new()
$previousPipe = [Environment]::GetEnvironmentVariable('D2TESTDRV_PIPE_NAME')
$script:last = $null; $script:logEvidence = @{}
$script:RunDeadline = (Get-PairUtcNow).AddSeconds($SessionSeconds)
$protected = @('Disciple.ini', 'Scripts/userSettings.lua', 'Scripts/settings.lua', 'Scripts/generatorSettings.lua', 'Scripts/modifiers/smns/z_unit_effect.lua', 'Interf/CustomLobby.dlg', 'Interf/Interf.dlg', 'Globals/GItem.dbf')
$beforeHashes = Get-ProtectedFileHashes $protected
Import-ExistingPopupObservers
try {
    New-OwnedArtifactDirectory $ArtifactDir $artifactRoot
    foreach ($role in @('host', 'join')) {
        $credentials[$role] = @{}
        foreach ($suffix in @('ACCOUNT', 'PASSWORD')) {
            $key = 'D2_LOBBY_' + $role.ToUpperInvariant() + '_' + $suffix
            $value = [Environment]::GetEnvironmentVariable($key)
            if (-not $value) { throw "Explicit $key is required." }
            $envBefore[$key] = $value; $credentials[$role][$suffix] = $value
            [Environment]::SetEnvironmentVariable($key, $null)
        }
    }
    if ($credentials.host.ACCOUNT -cne 'test2' -or $credentials.join.ACCOUNT -cne 'test1') {
        throw 'Only authorized host test2 and join test1 may run.'
    }
    $env:D2TESTDRV_PIPE_NAME = '\\.\pipe\d2mss.lobbye2e.' + [guid]::NewGuid().ToString('N')
    $script:Relay = Start-TestRelay -LogDir $ArtifactDir
    foreach ($role in @('host', 'join')) {
        $preLaunchLengths = Get-LogLaunchBoundary
        $flags = @(
            'SKIP_INTRO', 'BLACKSCREEN_FIX', 'UI_REPORTER', 'WORLD', 'RELAY_BRIDGE', 'TURN_EVENTS',
            'SCRIPTED_POPUPS', 'SCRIPTED_POPUPS_CONFIRMATIONS', 'SCRIPTED_POPUPS_LOBBY')
        if ($Gameplay -eq 'ConcurrentBattles') { $flags += @('AUTO_BATTLE_PREARM', 'BATTLE_TRACE') }
        $script:Clients[$role] = Start-GameClient -GameDir $GameDir -Role $role -Transport Lobby -Flags $flags
        Assert-FreshOwnedLog $role $preLaunchLengths
        Assert-OwnedRelayClientIdentity $role $script:Clients[$role] $GameDir
        $script:PopupObservers[$role] = New-LiteralStartupPopupService $role $script:ClientLogs[$role]
        Wait-Ready $role DLG_MAIN_MENU
        Press-Once $role DLG_MAIN_MENU BTN_TUTORIAL
        Wait-Ready $role DLG_LOGIN_ACCOUNT
        if (-not (Set-SecretEditText $role DLG_LOGIN_ACCOUNT EDIT_ACCOUNT_NAME $credentials[$role].ACCOUNT) -or
            -not (Set-SecretEditText $role DLG_LOGIN_ACCOUNT EDIT_PASSWORD $credentials[$role].PASSWORD)) {
            throw 'Secret login input failed; values omitted.'
        }
        Press-Once $role DLG_LOGIN_ACCOUNT BTN_OK
        Wait-Ready $role DLG_CUSTOM_LOBBY
        $lobbyUi = Get-GameUiSnapshot $role
        if ((Property $lobbyUi 'lobbyStartupPopups') -ne $true -or $lobbyUi.startupActionsHeld -ne $true) {
            throw 'Installed native driver did not publish held lobby-scoped popup ownership; preparation was not created.'
        }
    }
    $script:Preparation = Invoke-OwnedPreparation create
    $script:PreparationId = [string]$script:Preparation.id
    if ($PreparationId -notmatch '^[a-zA-Z0-9-]{8,80}$') { throw 'Created preparation identity malformed.' }
    Save-PairReceipt 'preparation-created-assigned' @{ id=$PreparationId }
    $script:Preparation = Invoke-OwnedPreparation start
    $title = [string]$script:Preparation.title
    Complete-PreparedEntry host host-offer (Get-PreparedPrompt host $title)
    Complete-PreparedEntry host generation
    Complete-PreparedEntry join join-offer (Get-PreparedPrompt join $title)
    foreach ($role in @('host', 'join')) { Wait-Ready $role DLG_LOBBY }
    Start-OwnedPair
    Wait-StartupAcceptance
    $accepted = $true
    if ($Gameplay -eq 'ConcurrentBattles') { Invoke-LobbyConcurrentBattleMerge }
} catch {
    $failure = $_.Exception.Message
    $failureStack = $_.ScriptStackTrace
    # Even unexpected exceptions must not echo a credential into receipts/transcripts.
    foreach ($pair in $credentials.Values) {
        if ($pair.ContainsKey('PASSWORD') -and $pair.PASSWORD) {
            $failure = $failure.Replace($pair.PASSWORD, '[redacted]')
            if ($failureStack) { $failureStack = $failureStack.Replace($pair.PASSWORD, '[redacted]') }
        }
    }
} finally {
    foreach ($p in $script:Clients.Values) { try { Stop-OwnedProcess $p } catch { $cleanupErrors.Add('Owned game cleanup failed.') } }
    if ($script:Relay) { try { Stop-OwnedProcess $script:Relay } catch { $cleanupErrors.Add('Owned relay cleanup failed.') } }
    if ($script:ArtifactDirectoryOwned -and (Test-Path -LiteralPath (Join-Path $ArtifactDir 'owned-preparation.json'))) {
        try { [void](Invoke-OwnedPreparation close) } catch { $cleanupErrors.Add('Owned preparation close failed; retain receipt for exact manual cleanup.') }
    }
    $afterHashes = @{}
    foreach ($file in $protected) {
        try { $afterHashes[$file] = (Get-ProtectedFileHashes @($file))[$file] }
        catch { $afterHashes[$file] = '[unreadable]'; $cleanupErrors.Add("Protected file hash verification failed: $file") }
    }
    $protectedFileProof = Compare-ProtectedFileHashes $beforeHashes $afterHashes
    $unchanged = $protectedFileProof.unchanged
    if (-not $unchanged) { $cleanupErrors.Add('Game configuration changed during the run.') }
    if ($script:ArtifactDirectoryOwned -and (Test-Path -LiteralPath $ArtifactDir)) {
        foreach ($role in $script:ClientLogs.Keys) {
            try { if (Test-Path -LiteralPath $script:ClientLogs[$role]) { Copy-OwnedLogTail $role } }
            catch { $cleanupErrors.Add('Owned log copy failed.') }
        }
        try {
            Save-Json 'summary.json' ([ordered]@{
                schema=1; transport='lobby'; preparationId=$PreparationId; pairStartup=$PairStartup; expectedMergeDay=$ExpectedMergeDay
                startupAcceptance=($accepted -and $cleanupErrors.Count -eq 0)
                gameplay=$Gameplay; gameplayProof=$script:GameplayProof
                concurrentBattleMergeAcceptance=($null -ne $script:GameplayProof -and -not $failure -and $cleanupErrors.Count -eq 0)
                fullGameplayAcceptance=$false; campaign18=$false; failure=$failure; cleanupErrors=$cleanupErrors.ToArray()
                failureStack=$failureStack
                gameConfigurationUnchanged=$unchanged; evidence=$script:logEvidence; lastObservation=$script:last
                protectedFiles=$protectedFileProof
                ownedLogBoundaries=@($script:ClientLogs.Keys | ForEach-Object {
                    @{ role=$_; pid=$script:ClientLogOwnedProcessIds[$script:ClientLogs[$_]]; source=$script:ClientLogs[$_]
                        baselineBytes=$script:ClientLogInitialLengths[$script:ClientLogs[$_]]; artifact=($_ + '.mss32.log'); artifactStartsAtBaseline=$true }
                })
                ownedPids=@($script:Clients.Values | ForEach-Object Id); pairMapsObserved=$script:PairMapsObserved
                nativePopupReceipts=@{ host=(Property $script:PopupObservers['host'] 'Appearances'); join=(Property $script:PopupObservers['join'] 'Appearances') }
                latestSnapshotSharingSkips=$script:LatestSnapshotSharingSkips
            })
        } catch { $cleanupErrors.Add('Final machine receipt could not be saved.') }
    }
    [Environment]::SetEnvironmentVariable('D2TESTDRV_PIPE_NAME', $previousPipe)
    foreach ($key in $envBefore.Keys) { [Environment]::SetEnvironmentVariable($key, $envBefore[$key]) }
    $credentials.Clear(); $envBefore.Clear()
}
if ($failure -or $cleanupErrors.Count) { throw "Lobby E2E failed: $failure $($cleanupErrors -join ' ')" }
Write-Output "PASS: owned production-lobby $Gameplay, merge setting $ExpectedMergeDay; not the full 18-case campaign."
