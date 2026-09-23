param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$helperPath = Join-Path $PSScriptRoot '_literal_inner_startup.ps1'
$source = Get-Content -LiteralPath $helperPath -Raw
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$controllerPath = Join-Path $repoRoot 'mss32\src\simturns\controller.cpp'
$controllerSource = Get-Content -LiteralPath $controllerPath -Raw
foreach ($productionMarker in @(
    '[simturns] both-role production bundle prepared; no room activated',
    '[simturns] room armed before native startup (role={})',
    '[simturns] engine interception installed; coordinator start deferred to strategic phase')) {
    if (-not $controllerSource.Contains($productionMarker)) {
        throw "literal inner helper marker drifted from controller.cpp: '$productionMarker'"
    }
}
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $helperPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "literal inner helper has $($parseErrors.Count) parse error(s)"
}
$workerAssignments = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
    [string]$node.Left.VariablePath.UserPath -eq 'workerSource'
}, $true))
if ($workerAssignments.Count -ne 1 -or
    $workerAssignments[0].Right -isnot
        [Management.Automation.Language.CommandExpressionAst] -or
    $workerAssignments[0].Right.Expression -isnot
        [Management.Automation.Language.StringConstantExpressionAst]) {
    throw 'literal inner helper must contain one statically parseable workerSource here-string'
}
$workerSource = [string]$workerAssignments[0].Right.Expression.Value
$workerTokens = $null
$workerParseErrors = $null
$workerAst = [Management.Automation.Language.Parser]::ParseInput(
    $workerSource, [ref]$workerTokens, [ref]$workerParseErrors)
if ($workerParseErrors.Count -ne 0) {
    throw "literal inner workerSource has $($workerParseErrors.Count) parse error(s)"
}
$parsedAsts = @($ast, $workerAst)
$commandAsts = @($parsedAsts | ForEach-Object {
    $_.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst]
    }, $true)
})
foreach ($commandAst in $commandAsts) {
    $commandName = [string]$commandAst.GetCommandName()
    if ($commandName -eq 'Start-Process') {
        throw 'literal inner helper launches a process'
    }
    if ($commandName -in @('Invoke-RestMethod', 'Invoke-WebRequest') -and
        $commandAst.Extent.Text -match '(?i)-Method\s+Post\b') {
        throw 'literal inner helper sends an HTTP POST action'
    }
}
$readOnlyAutomaticNames = @('Host', 'Home', 'PID', 'PSHome')
$assignmentAsts = @($parsedAsts | ForEach-Object {
    $_.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst]
    }, $true)
})
foreach ($assignmentAst in $assignmentAsts) {
    if ($assignmentAst.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        [string]$assignmentAst.Left.VariablePath.UserPath -in $readOnlyAutomaticNames) {
        throw "literal inner helper assigns read-only automatic variable '$($assignmentAst.Left.Extent.Text)'"
    }
}
foreach ($required in @(
    'Start-Sleep -Milliseconds 500',
    'Start-Sleep -Milliseconds 400',
    'Start-Sleep -Seconds 2',
    'AddSeconds(8)',
    'AddMilliseconds(800)',
    'FileMode]::CreateNew',
    "'session-plan-created'",
    "'session-plan-delivered'",
    "'bootstrap-cascade-delay-armed'",
    "'engine-action-dispatched'",
    "'bootstrap-released'",
    "'session-operational'",
    'OperationalReady',
    '$releaseWriteCompletedUtc.AddSeconds($TotalBudgetSec)',
    'bootstrap operational release applied; strict independent turns are operational',
    "'legacy-inner-manual-checkpoint.txt'",
    "'legacy-inner-subturn-cycle.txt'",
    "'legacy-inner-mp-refill.txt'",
    "'legacy-inner-host-invariant.txt'",
    "'legacy-inner-day-display-probe.txt'",
    "'legacy-inner-keep-alive.txt'",
    "'legacy-inner-console-transcript.txt'",
    "'legacy-inner-execution-trace.json'",
    "'http://127.0.0.1:8077/api/inject-begin-turn-joiner?player=0x805E0002&seq=7&auto=0'",
    'Get-PeerSnapshotProjection',
    'Get-HostAdvanceCounts',
    'Get-ChangedDumps $InitialDumps')) {
    if (-not $source.Contains($required)) {
        throw "literal inner helper omitted required contract '$required'"
    }
}
# mergeDay=0 is the protocol's explicit disabled mode, not a missing value.
# Keep the normal mocked runtime at mergeDay=3 while statically pinning the
# separate zero-valued contract and the required-field checks which distinguish
# zero from an omitted property.
if (-not $workerSource.Contains(
        '$sessionMergeDayValue = Property $sessionPlan ''mergeDay''') -or
    -not $workerSource.Contains('$null -eq $sessionMergeDayValue') -or
    -not $workerSource.Contains(
        '$sessionMergeDay -lt 0 -or $sessionMergeDay -gt [uint32]::MaxValue') -or
    $workerSource.Contains('$sessionMergeDay -le 0') -or
    -not $workerSource.Contains(
        '$operationalMergeDayValue = Property $sessionOperational ''mergeDay''') -or
    -not $workerSource.Contains('$null -eq $operationalMergeDayValue') -or
    -not $workerSource.Contains(
        '$operationalMergeDay -ne $sessionMergeDay')) {
    throw ('literal inner worker must accept explicit mergeDay=0 while ' +
        'requiring and linking both SessionPlan/session-operational fields')
}
$exactTokenCounts = [ordered]@{
    'Start-Sleep -Milliseconds 500' = 1
    'Start-Sleep -Milliseconds 400' = 1
    'Start-Sleep -Seconds 2' = 1
    'AddSeconds(8)' = 2
    'AddMilliseconds(800)' = 1
    'Write-NewUtf8File $BootstrapReleaseFile' = 1
    "Invoke-JsonGet '/api/state'" = 2
    'Read-CompleteLog $hostLog' = 7
    'Read-CompleteLog $joinLog' = 4
    'Read-CompleteLog $LogPath' = 2
    'Get-HostAdvanceCounts' = 3
}
foreach ($entry in $exactTokenCounts.GetEnumerator()) {
    $actual = [regex]::Matches(
        $workerSource, [regex]::Escape([string]$entry.Key)).Count
    if ($actual -ne [int]$entry.Value) {
        throw "literal inner worker contract '$($entry.Key)' count=$actual, expected $($entry.Value)"
    }
}
$simEventReader = [regex]::Match(
    $workerSource,
    '(?ms)^function Read-SimEvents\s*\{.*?(?=^function Invoke-JsonGet)'
).Value
$unpublishedGateAt = $simEventReader.IndexOf(
    'if ($isUnpublishedTail) { break }', [StringComparison]::Ordinal)
$jsonParseAt = $simEventReader.IndexOf(
    'ConvertFrom-Json -ErrorAction Stop', [StringComparison]::Ordinal)
if ([string]::IsNullOrEmpty($simEventReader) -or
    $unpublishedGateAt -lt 0 -or $jsonParseAt -le $unpublishedGateAt) {
    throw 'literal inner sim-event reader can parse an unpublished final JSONL tail'
}
if ($workerSource -notmatch
        '\$snapshotCapturedAt\s*=\s*\[DateTimeOffset\]::Now\b' -or
    $workerSource -match
        '\$snapshotCapturedAt\s*=\s*\[DateTimeOffset\]::UtcNow\b') {
    throw 'literal snapshot timestamp no longer preserves the source local offset'
}
foreach ($requiredIdentityContract in @(
    "Join-Path `$GameDir 'mss32.dll'",
    '"mss32_$($Process.Id).log"',
    '[Parameter(Mandatory)][string]$ExecutablePath',
    'HostExecutablePath',
    'JoinExecutablePath',
    'is not its exact PID log')) {
    if (-not $source.Contains($requiredIdentityContract)) {
        throw "literal inner helper lost identity contract '$requiredIdentityContract'"
    }
}
if ($source -match '\$Process\.Path\b') {
    throw 'literal inner helper reads Process.Path instead of saved launch provenance'
}

. $helperPath

function Write-JsonLine {
    param([Parameter(Mandatory)][string]$Path,
          [Parameter(Mandatory)][object]$Value,
          [switch]$CreateNew)
    $line = ($Value | ConvertTo-Json -Compress -Depth 12) + "`n"
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($line)
    $mode = if ($CreateNew) { [IO.FileMode]::CreateNew } else { [IO.FileMode]::Append }
    $stream = [IO.File]::Open(
        $Path, $mode, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ("mss-literal-inner-{0}" -f [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$artifactDir = Join-Path $testRoot 'artifacts'
$gameDir = Join-Path $testRoot 'game'
[void][IO.Directory]::CreateDirectory($artifactDir)
[void][IO.Directory]::CreateDirectory($gameDir)
$hostLog = $null
$joinLog = $null
$simLog = Join-Path $testRoot 'simturns-relay.jsonl'
$releaseFile = Join-Path $artifactDir 'bootstrap.release'
$failureLog = Join-Path $artifactDir 'literal-inner-failure.log'
$modulePath = Join-Path $gameDir 'mss32.dll'
[IO.File]::WriteAllBytes($modulePath, [byte[]](0x4d, 0x5a))
$gameExe = Join-Path $gameDir 'Discipl2.exe'
$pingExe = Join-Path $env:SystemRoot 'System32\PING.EXE'
if (-not (Test-Path -LiteralPath $pingExe -PathType Leaf)) {
    throw "mock exact-game process source is missing: $pingExe"
}
[IO.File]::Copy($pingExe, $gameExe)
$hostHandle = '0xa3de0001'
$joinHandle = '0xa3de0002'
$dumpBaseline = Get-LiteralInnerDumpBaseline -DumpRoots @($gameDir, $artifactDir)

Write-JsonLine -Path $simLog -CreateNew -Value ([ordered]@{
    timestamp = [DateTimeOffset]::UtcNow.ToString('O')
    event = 'bootstrap-release-pending'
    hostHandle = $hostHandle
    joinHandle = $joinHandle
})

$hostProcess = $null
$joinProcess = $null
try {
    $hostProcess = Start-Process -FilePath $gameExe -PassThru -WindowStyle Hidden `
        -ArgumentList @('-t', '127.0.0.1')
    $joinProcess = Start-Process -FilePath $gameExe -PassThru -WindowStyle Hidden `
        -ArgumentList @('-t', '127.0.0.1')
    $hostLog = Join-Path $gameDir "mss32_$($hostProcess.Id).log"
    $joinLog = Join-Path $gameDir "mss32_$($joinProcess.Id).log"
    [IO.File]::WriteAllBytes($hostLog, [byte[]]::new(0))
    [IO.File]::WriteAllBytes($joinLog, [byte[]]::new(0))
} catch {
    if ($hostProcess) {
        try { $hostProcess.Kill($true) } catch {}
        $hostProcess.Dispose()
    }
    throw
}

$httpState = [hashtable]::Synchronized(@{
    Ready = $false
    FaultTarget = ''
    Requests = [Collections.ArrayList]::Synchronized(
        [Collections.ArrayList]::new())
})
$eventState = [hashtable]::Synchronized(@{
    Ready = $false
    StopRequested = $false
    ReleaseObserved = $false
    RelayOperationalUtc = $null
    HostMarkerUtc = $null
    JoinMarkerUtc = $null
    RelayPublished = $false
    AllowHostMarker = $false
    HostMarkerPublished = $false
    AllowJoinMarker = $false
})
$httpWorker = [PowerShell]::Create()
$eventWorker = [PowerShell]::Create()
$observer = $null
$faultObserver = $null
$httpAsync = $null
$eventAsync = $null
$relayBase = ''
try {
    $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $probe.Start()
    $port = ([Net.IPEndPoint]$probe.LocalEndpoint).Port
    $probe.Stop()
    $relayBase = "http://127.0.0.1:$port"

    $httpSource = @'
param($State, $Port, $HostPid, $JoinPid, $ModulePath, $HostHandle, $JoinHandle)
$ErrorActionPreference = 'Stop'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, [int]$Port)
$listener.Start()
$State.Ready = $true
try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $reader = [IO.StreamReader]::new(
                $stream, [Text.Encoding]::ASCII, $false, 1024, $true)
            try {
                $requestLine = $reader.ReadLine()
                while (-not [string]::IsNullOrEmpty($reader.ReadLine())) {}
            } finally { $reader.Dispose() }
            $method = ($requestLine -split ' ')[0]
            $target = ($requestLine -split ' ')[1]
            [void]$State.Requests.Add([pscustomobject]@{
                method = $method
                target = $target
                timestamp = [DateTimeOffset]::UtcNow.ToString('O')
                ordinal = $State.Requests.Count + 1
            })
            $targetUri = [Uri]::new('http://127.0.0.1' + $target)
            $path = $targetUri.AbsolutePath
            if ($path -eq '/__stop') {
                $body = [pscustomobject]@{ stopped = $true }
                $stop = $true
            } elseif ($path -eq '/api/state') {
                $body = [ordered]@{
                    terminalFault = $null
                    roles = [ordered]@{
                        host = [ordered]@{
                            connected = $true; pid = $HostPid; modulePath = $ModulePath
                        }
                        join = [ordered]@{
                            connected = $true; pid = $JoinPid; modulePath = $ModulePath
                        }
                    }
                }
                $stop = $false
            } elseif ($path -eq '/api/ui/history') {
                $body = [ordered]@{ terminalFault = $null; latestSeq = 0; events = @() }
                $stop = $false
            } elseif ($path -eq '/api/world') {
                $isJoin = $targetUri.Query -match 'role=join'
                $selfHandle = if ($isJoin) { $JoinHandle } else { $HostHandle }
                $enemyHandle = if ($isJoin) { $HostHandle } else { $JoinHandle }
                $ownedStacks = if ($isJoin) {
                    @(
                        [ordered]@{ id = 'A3E30008'; owner = $selfHandle; movement = 30 },
                        [ordered]@{ id = 'A3E30001'; owner = $selfHandle; movement = 35 },
                        [ordered]@{ id = 'A3E30009'; owner = $selfHandle; movement = 20 }
                    )
                } else {
                    @([ordered]@{
                        id = 'host-stack'; owner = $selfHandle; movement = 35
                    })
                }
                $body = [ordered]@{
                    terminalFault = $null
                    day = 1
                    players = @(
                        [ordered]@{ id = $selfHandle; relation = 'self'; human = $true },
                        [ordered]@{ id = $enemyHandle; relation = 'enemy'; human = $true }
                    )
                    stacks = $ownedStacks
                }
                $stop = $false
            } elseif ($path -eq '/api/turn/history') {
                $body = [ordered]@{ terminalFault = $null; latestSeq = 0; events = @() }
                $stop = $false
            } else {
                $body = [ordered]@{ error = "unexpected test path $path" }
                $stop = $false
            }
            if (-not [string]::IsNullOrWhiteSpace([string]$State.FaultTarget) -and
                [string]$State.FaultTarget -eq $target -and
                $body.Contains('terminalFault')) {
                $body['terminalFault'] = [ordered]@{
                    reason = 'fixture-terminal-observation'
                    target = $target
                }
            }
            $json = $body | ConvertTo-Json -Compress -Depth 12
            $bodyBytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
            $status = if ($path -in @(
                    '/__stop', '/api/state', '/api/ui/history',
                    '/api/world', '/api/turn/history')) {
                '200 OK'
            } else { '404 Not Found' }
            $header = "HTTP/1.1 $status`r`nContent-Type: application/json`r`n" +
                "Content-Length: $($bodyBytes.Length)`r`nConnection: close`r`n`r`n"
            $headerBytes = [Text.Encoding]::ASCII.GetBytes($header)
            $stream.Write($headerBytes, 0, $headerBytes.Length)
            $stream.Write($bodyBytes, 0, $bodyBytes.Length)
            $stream.Flush()
            if ($stop) { break }
        } finally {
            $client.Dispose()
        }
    }
} finally {
    $listener.Stop()
}
'@
    [void]$httpWorker.AddScript($httpSource)
    [void]$httpWorker.AddArgument($httpState)
    [void]$httpWorker.AddArgument([int]$port)
    [void]$httpWorker.AddArgument([long]$hostProcess.Id)
    [void]$httpWorker.AddArgument([long]$joinProcess.Id)
    [void]$httpWorker.AddArgument($modulePath)
    [void]$httpWorker.AddArgument($hostHandle)
    [void]$httpWorker.AddArgument($joinHandle)
    $httpAsync = $httpWorker.BeginInvoke()

$eventSource = @'
param($State, $ReleaseFile, $SimLog, $HostLog, $JoinLog,
      $HostHandle, $JoinHandle)
$ErrorActionPreference = 'Stop'
function Append-Event([string]$Event, [hashtable]$Fields) {
    $eventUtc = [DateTimeOffset]::UtcNow
    $record = [ordered]@{
        timestamp = $eventUtc.ToString('O')
        event = $Event
    }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    $line = ($record | ConvertTo-Json -Compress -Depth 8) + "`n"
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($line)
    $stream = [IO.File]::Open(
        $SimLog, [IO.FileMode]::Append, [IO.FileAccess]::Write,
        [IO.FileShare]::ReadWrite)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    return $eventUtc
}
$parent = Split-Path -Parent $ReleaseFile
$name = Split-Path -Leaf $ReleaseFile
$watcher = [IO.FileSystemWatcher]::new($parent, $name)
$watcher.NotifyFilter = [IO.NotifyFilters]::FileName -bor [IO.NotifyFilters]::CreationTime
$watcher.EnableRaisingEvents = $true
$State.Ready = $true
try {
    $change = $watcher.WaitForChanged([IO.WatcherChangeTypes]::Created, 8000)
    if ($change.TimedOut) { throw 'mock bootstrap release event timed out' }
    $epoch = 12345
    $hostLease = 101
    $joinLease = 102
    [void](Append-Event 'bootstrap-release-file-observed' @{ path = $ReleaseFile })
    $State.ReleaseObserved = $true
    [void](Append-Event 'bootstrap-release-granted' @{
        hostHandle = $HostHandle; joinHandle = $JoinHandle
    })
    [void](Append-Event 'session-plan-delivered' @{
        role = 'host'; epoch = $epoch
    })
    [void](Append-Event 'session-plan-created' @{
        epoch = $epoch
        mergeDay = 3
        hostHandle = $HostHandle
        joinHandle = $JoinHandle
        hostLease = $hostLease
        joinLease = $joinLease
    })
    [void](Append-Event 'session-activated' @{ role = 'host' })
    [void](Append-Event 'session-plan-delivered' @{
        role = 'join'; epoch = $epoch
    })
    [void](Append-Event 'session-activated' @{ role = 'join' })
    $beginAppliedUtc = Append-Event 'bootstrap-begin-turn-applied' @{
        role = 'join'; handle = $JoinHandle; day = 1
    }
    # The real coordinator creates its timer before logging delay-armed. Model
    # legal logger overhead so armed-to-action is deliberately below 500 ms,
    # while the BeginTurnApplied-anchored delay remains a full 500 ms.
    Start-Sleep -Milliseconds 30
    $delayArmedUtc = Append-Event 'bootstrap-cascade-delay-armed' @{
        delayMs = 500
        anchor = 'bootstrap-begin-turn-applied'
    }
    $mockCascadeTarget = $beginAppliedUtc.AddMilliseconds(500)
    while ([DateTimeOffset]::UtcNow -lt $mockCascadeTarget) {
        $remainingMs = [Math]::Ceiling(
            ($mockCascadeTarget - [DateTimeOffset]::UtcNow).TotalMilliseconds)
        if ($remainingMs -gt 0) {
            Start-Sleep -Milliseconds ([Math]::Min(25, $remainingMs))
        }
    }
    [void](Append-Event 'engine-action-dispatched' @{
        actionId = 1
        kind = 1
        stage = 'bootstrap-apply'
        recipient = 'host'
        playerHandle = $JoinHandle
        day = 1
        lease = $joinLease
    })
    [void](Append-Event 'bootstrap-cascade-complete' @{
        actionId = 1; role = 'join'; day = 1
    })
    [void](Append-Event 'bootstrap-turn-info-applied' @{
        role = 'join'; handle = $JoinHandle; day = 1
    })
    [void](Append-Event 'bootstrap-commit-dispatched' @{
        joinHandle = $JoinHandle; day = 1
    })
    [void](Append-Event 'bootstrap-commit-applied' @{ role = 'host' })
    [void](Append-Event 'bootstrap-commit-applied' @{ role = 'join' })
    [void](Append-Event 'bootstrap-operational-dispatched' @{
        joinHandle = $JoinHandle; day = 1
    })
    [void](Append-Event 'bootstrap-operational-applied' @{ role = 'host' })
    [void](Append-Event 'bootstrap-operational-applied' @{ role = 'join' })
    [void](Append-Event 'bootstrap-released' @{
        joinHandle = $JoinHandle; day = 1
    })
    $State.RelayOperationalUtc = Append-Event 'session-operational' @{
        epoch = $epoch
        mergeDay = 3
    }
    # Relay publication alone is intentionally insufficient. Let the parent
    # assert the closed latch at each native-application boundary.
    $State.RelayPublished = $true
    while (-not [bool]$State.AllowHostMarker -and
           -not [bool]$State.StopRequested) {
        Start-Sleep -Milliseconds 10
    }
    if ([bool]$State.StopRequested) { return }
    $marker =
        '[simturns] bootstrap operational release applied; strict independent turns are operational'
    [IO.File]::AppendAllText(
        $HostLog, "$marker`r`n", [Text.UTF8Encoding]::new($false))
    $State.HostMarkerUtc = [DateTimeOffset]::UtcNow
    $State.HostMarkerPublished = $true
    while (-not [bool]$State.AllowJoinMarker -and
           -not [bool]$State.StopRequested) {
        Start-Sleep -Milliseconds 10
    }
    if ([bool]$State.StopRequested) { return }
    [IO.File]::AppendAllText(
        $JoinLog, "$marker`r`n", [Text.UTF8Encoding]::new($false))
    $State.JoinMarkerUtc = [DateTimeOffset]::UtcNow
    # The preserved green source run accumulated two more legacy VEH notes
    # after its four boot notes. They remained observational under -KeepAlive.
    [IO.File]::AppendAllText(
        $HostLog,
        "[VEH #5] code=0xC0000005 addr=0x004039E3`r`n" +
        "[VEH #6] code=0xC0000005 addr=0x004039E3`r`n",
        [Text.UTF8Encoding]::new($false))
    # A syntactically valid object without a terminating newline is still the
    # writer's unpublished JSONL tail. If the observer consumes it, this
    # terminal fault makes the happy-path mock fail.
    $unpublished = [ordered]@{
        timestamp = [DateTimeOffset]::UtcNow.ToString('O')
        event = 'session-faulted'
        reason = 'unpublished-tail-must-not-be-visible'
    } | ConvertTo-Json -Compress
    [IO.File]::AppendAllText(
        $SimLog, $unpublished, [Text.UTF8Encoding]::new($false))
} finally {
    $watcher.EnableRaisingEvents = $false
    $watcher.Dispose()
}
'@
    [void]$eventWorker.AddScript($eventSource)
    [void]$eventWorker.AddArgument($eventState)
    [void]$eventWorker.AddArgument($releaseFile)
    [void]$eventWorker.AddArgument($simLog)
    [void]$eventWorker.AddArgument($hostLog)
    [void]$eventWorker.AddArgument($joinLog)
    [void]$eventWorker.AddArgument($hostHandle)
    [void]$eventWorker.AddArgument($joinHandle)
    $eventAsync = $eventWorker.BeginInvoke()

    $readyDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ((-not [bool]$httpState.Ready -or -not [bool]$eventState.Ready) -and
           [DateTime]::UtcNow -lt $readyDeadline) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [bool]$httpState.Ready -or -not [bool]$eventState.Ready) {
        $httpErrors = @($httpWorker.Streams.Error | ForEach-Object {
            $_.Exception.Message
        }) -join '; '
        $eventErrors = @($eventWorker.Streams.Error | ForEach-Object {
            $_.Exception.Message
        }) -join '; '
        throw ("mock workers did not arm within 5 seconds " +
            "(http=$($httpState.Ready) event=$($eventState.Ready) " +
            "httpErrors='$httpErrors' eventErrors='$eventErrors')")
    }

    $wrongModule = Join-Path $gameDir 'not-the-game-mss32.dll'
    [IO.File]::WriteAllBytes($wrongModule, [byte[]](0x4d, 0x5a))
    $wrongModuleObserver = $null
    $wrongModuleRejected = $false
    try {
        $wrongModuleObserver = Start-LiteralInnerStartupObserver `
            -ArmUtc ([DateTime]::UtcNow) `
            -RelayBase $relayBase `
            -SimRelayLog $simLog `
            -BootstrapReleaseFile $releaseFile `
            -FailureLogPath $failureLog `
            -ArtifactDir $artifactDir `
            -GameDir $gameDir `
            -ExpectedModulePath $wrongModule `
            -DumpRoots @($gameDir, $artifactDir) `
            -DumpBaseline $dumpBaseline `
            -TotalBudgetSec 30
    } catch {
        if ($_.Exception.Message -notmatch 'is not the exact game module') { throw }
        $wrongModuleRejected = $true
    } finally {
        if ($wrongModuleObserver) {
            Stop-LiteralInnerStartupObserver $wrongModuleObserver
        }
    }
    if (-not $wrongModuleRejected) {
        throw 'literal inner helper accepted a module outside GameDir\mss32.dll'
    }

    $armUtc = [DateTime]::UtcNow
    $observer = Start-LiteralInnerStartupObserver `
        -ArmUtc $armUtc `
        -RelayBase $relayBase `
        -SimRelayLog $simLog `
        -BootstrapReleaseFile $releaseFile `
        -FailureLogPath $failureLog `
        -ArtifactDir $artifactDir `
        -GameDir $gameDir `
        -ExpectedModulePath $modulePath `
        -DumpRoots @($gameDir, $artifactDir) `
        -DumpBaseline $dumpBaseline `
        -TotalBudgetSec 30

    $wrongExecutableRejected = $false
    try {
        Publish-LiteralInnerStartupProcess -Observer $observer -Role host `
            -Process $hostProcess `
            -ExecutablePath (Join-Path $gameDir 'not-Discipl2.exe') `
            -LogPath $hostLog -LogBaseline 0
    } catch {
        if ($_.Exception.Message -notmatch 'process path .* is not') { throw }
        $wrongExecutableRejected = $true
    }
    if (-not $wrongExecutableRejected) {
        throw 'literal inner helper accepted launch provenance for another executable'
    }

    $wrongHostLogRejected = $false
    try {
        Publish-LiteralInnerStartupProcess -Observer $observer -Role host `
            -Process $hostProcess -ExecutablePath $gameExe `
            -LogPath (Join-Path $gameDir 'host.log') `
            -LogBaseline 0
    } catch {
        if ($_.Exception.Message -notmatch 'is not its exact PID log') { throw }
        $wrongHostLogRejected = $true
    }
    if (-not $wrongHostLogRejected) {
        throw 'literal inner helper accepted a non-PID host log path'
    }
    Publish-LiteralInnerStartupProcess -Observer $observer -Role host `
        -Process $hostProcess -ExecutablePath $gameExe `
        -LogPath $hostLog -LogBaseline 0
    Publish-LiteralInnerStartupProcess -Observer $observer -Role join `
        -Process $joinProcess -ExecutablePath $gameExe `
        -LogPath $joinLog -LogBaseline 0
    [IO.File]::AppendAllText(
        $hostLog,
        "[simturns] both-role production bundle prepared; no room activated`r`n" +
        "[simturns] room armed before native startup (role=host)`r`n" +
        "[simturns] engine interception installed; coordinator start deferred to strategic phase`r`n" +
        "[testdrv] bind DLG_STRATEGIC::BTN_GUARD`r`n" +
        "[VEH #1] code=0xC0000005 addr=0x004039E3`r`n" +
        "[VEH #2] code=0xC0000005 addr=0x004039E3`r`n" +
        "[VEH #3] code=0xC0000005 addr=0x004039E3`r`n" +
        "[VEH #4] code=0xC0000005 addr=0x004039E3`r`n",
        [Text.UTF8Encoding]::new($false))
    [IO.File]::AppendAllText(
        $joinLog,
        "[simturns] both-role production bundle prepared; no room activated`r`n" +
        "[simturns] room armed before native startup (role=join)`r`n" +
        "[simturns] engine interception installed; coordinator start deferred to strategic phase`r`n" +
        "[testdrv] bind DLG_ISO_PAL::BTN_RES_PAL`r`n",
        [Text.UTF8Encoding]::new($false))

    # The mock release watcher owns the release edge and its existing
    # eight-second causal budget. Only after that edge does the mock relay enter
    # its five-second machine-publication stage. Combining both into one deadline
    # made durable JSONL flushes consume a timeout that had not started yet in
    # the production protocol; this remains one causal wait with no retry.
    $releaseObservationDeadline = $armUtc.AddSeconds(8)
    while (-not [bool]$eventState.ReleaseObserved -and
           [DateTime]::UtcNow -lt $releaseObservationDeadline) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [bool]$eventState.ReleaseObserved) {
        throw 'mock bootstrap release event was not observed within the startup budget'
    }

    $stageDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not [bool]$eventState.RelayPublished -and
           [DateTime]::UtcNow -lt $stageDeadline) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [bool]$eventState.RelayPublished) {
        throw 'mock relay operational stage did not publish'
    }
    if ([bool]$observer.State.OperationalReady -or
        $null -ne $observer.State.OperationalWitness) {
        throw 'inner operational latch opened on relay publication alone'
    }
    $eventState.AllowHostMarker = $true
    $stageDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not [bool]$eventState.HostMarkerPublished -and
           [DateTime]::UtcNow -lt $stageDeadline) {
        Start-Sleep -Milliseconds 10
    }
    if (-not [bool]$eventState.HostMarkerPublished) {
        throw 'mock host native operational marker did not publish'
    }
    if ([bool]$observer.State.OperationalReady -or
        $null -ne $observer.State.OperationalWitness) {
        throw 'inner operational latch opened before the join native marker'
    }
    $eventState.AllowJoinMarker = $true

    $result = Complete-LiteralInnerStartupObserver $observer
    if (Test-Path -LiteralPath $failureLog) {
        throw "mock happy path unexpectedly wrote failure log: $(Get-Content $failureLog -Raw)"
    }
    foreach ($path in @(
        $result.BeforePath,
        $result.TailPath,
        $result.ExecutionTracePath,
        $result.ConsoleTranscriptPath,
        (Join-Path $artifactDir 'legacy-inner-static-install.txt'),
        (Join-Path $artifactDir 'legacy-inner-manual-checkpoint.txt'),
        (Join-Path $artifactDir 'legacy-inner-subturn-cycle.txt'),
        (Join-Path $artifactDir 'legacy-inner-mp-refill.txt'),
        (Join-Path $artifactDir 'legacy-inner-host-invariant.txt'),
        (Join-Path $artifactDir 'legacy-inner-day-display-probe.txt'),
        (Join-Path $artifactDir 'legacy-inner-keep-alive.txt'))) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "mock happy path omitted artifact '$path'"
        }
    }
    $tail = Get-Content -LiteralPath $result.TailPath -Raw |
        ConvertFrom-Json -ErrorAction Stop
    $before = Get-Content -LiteralPath $result.BeforePath -Raw |
        ConvertFrom-Json -ErrorAction Stop
    $legacySnapshotTimestamp = [DateTimeOffset]::Parse(
        [string]$before.capturedAt,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind)
    $expectedLocalOffset = [TimeZoneInfo]::Local.GetUtcOffset(
        $legacySnapshotTimestamp.DateTime)
    if (-not [bool]$tail.manualCheckpoint.projected -or
        [bool]$tail.manualCheckpoint.presentedToUser -or
        [int]$tail.release.eventCounts.'engine-action-dispatched' -ne 1 -or
        [int]$tail.release.terminalOperational.hostNativeMarkerCount -ne 1 -or
        [int]$tail.release.terminalOperational.joinNativeMarkerCount -ne 1 -or
        [string]$tail.release.terminalOperational.sessionOperational.event -ne
            'session-operational' -or
        [long]$tail.release.terminalOperational.sessionOperational.epoch -ne 12345 -or
        [long]$tail.release.terminalOperational.sessionOperational.mergeDay -ne 3 -or
        -not $result.OperationalWitness -or
        [DateTimeOffset]::Parse(
            [string]$result.OperationalWitness.observedUtc) -lt
        [DateTimeOffset]$eventState.JoinMarkerUtc -or
        [double]$tail.release.beginAppliedToEngineActionMilliseconds -lt 500 -or
        [double]$tail.release.delayArmedToEngineActionMilliseconds -lt 0 -or
        [double]$tail.release.delayArmedToEngineActionMilliseconds -ge 500 -or
        [string]$tail.release.cascadeDelayAnchor -ne
            'bootstrap-begin-turn-applied' -or
        @($tail.beforeJoinMovement).Count -ne 3 -or
        [int]@($tail.beforeJoinMovement)[0] -ne 35 -or
        [int]@($tail.beforeJoinMovement)[1] -ne 30 -or
        [int]@($tail.beforeJoinMovement)[2] -ne 20 -or
        [string]@($tail.beforeJoinStacks)[0].id -ne 'A3E30001' -or
        [bool]$tail.subturnOneSnapshot.exists -or
        -not [bool]$tail.strictMssAssertions.passed -or
        -not [bool]$before.strictMssAssertions.passed -or
        [string]$before.legacyObservationProjection.peerOrder[0] -ne 'host' -or
        [string]$before.legacyObservationProjection.peerOrder[1] -ne 'join' -or
        [int]$before.legacyObservationProjection.perPeerReadCount -ne 7 -or
        @($before.legacyObservationProjection.peers.host.orderedReads).Count -ne 7 -or
        @($before.legacyObservationProjection.peers.join.orderedReads).Count -ne 7 -or
        -not [bool]$before.mssEvidence.dumpBaseline.capturedBeforeArm -or
        @($before.sourceStaticStepProjectedToMss.hostRealFaults).Count -ne 4 -or
        @($before.sourceStaticStepProjectedToMss.hostPrintedBootFaults).Count -ne 3 -or
        @($tail.finalRealFaults).Count -ne 6 -or
        [long]$tail.processes.host.pid -ne [long]$hostProcess.Id -or
        [long]$tail.processes.join.pid -ne [long]$joinProcess.Id -or
        $legacySnapshotTimestamp.Offset -ne $expectedLocalOffset) {
        throw 'mock happy path artifact invariants are not exact'
    }

    $expectedTargets = @(
        '/api/state',
        '/api/state',
        '/api/turn/history?after=0&role=host',
        '/api/world?role=host',
        '/api/turn/history?after=0&role=host',
        '/api/world?role=host',
        '/api/world?role=host',
        '/api/turn/history?after=0&role=join',
        '/api/world?role=join',
        '/api/turn/history?after=0&role=join',
        '/api/world?role=join',
        '/api/world?role=join',
        '/api/ui/history?role=host&after=0',
        '/api/ui/history?role=join&after=0',
        '/api/turn/history?after=0&role=host',
        '/api/turn/history?after=0&role=host',
        '/api/turn/history?after=0&role=host',
        '/api/turn/history?after=0&role=host'
    )
    $requests = @($httpState.Requests)
    if ($requests.Count -ne $expectedTargets.Count) {
        throw "mock HTTP observation count=$($requests.Count), expected $($expectedTargets.Count)"
    }
    for ($requestIndex = 0; $requestIndex -lt $expectedTargets.Count; $requestIndex++) {
        if ([string]$requests[$requestIndex].method -ne 'GET' -or
            [string]$requests[$requestIndex].target -ne $expectedTargets[$requestIndex] -or
            [int]$requests[$requestIndex].ordinal -ne ($requestIndex + 1)) {
            throw ("mock HTTP observation $($requestIndex + 1) changed: " +
                "$($requests[$requestIndex] | ConvertTo-Json -Compress)")
        }
    }

    $expectedTraceNames = @(
        'role-loop-armed',
        'role-poll-sleep-500-complete',
        'role-state-read',
        'strategic-poll-sleep-400-complete',
        'strategic-host-log-read',
        'strategic-join-log-read',
        'post-strategic-sleep-2000-complete',
        'static-install-host-log-read',
        'static-patch-host-log-read',
        'static-fault-host-log-read',
        'static-fault-print-host-log-read',
        'mss-preflight-join-log-read',
        'static-install-output-written',
        'snapshot-state-read-and-timestamp-fixed',
        'snapshot-host-01-hook-bind-log-read',
        'snapshot-host-02-turn-info-read',
        'snapshot-host-03-refresh-stacks-read',
        'snapshot-host-04-state-dump-log-read',
        'snapshot-host-05-begin-turn-read',
        'snapshot-host-06-stack-moves-read',
        'snapshot-host-07-refresh-players-read',
        'snapshot-join-01-hook-bind-log-read',
        'snapshot-join-02-turn-info-read',
        'snapshot-join-03-refresh-stacks-read',
        'snapshot-join-04-state-dump-log-read',
        'snapshot-join-05-begin-turn-read',
        'snapshot-join-06-stack-moves-read',
        'snapshot-join-07-refresh-players-read',
        'mss-evidence-host-ui-history-read',
        'mss-evidence-join-ui-history-read',
        'mss-evidence-sim-events-read',
        'before-snapshot-written',
        'legacy-start-heading-written',
        'bootstrap-release-writer-armed',
        'bootstrap-release-writer-complete',
        'bootstrap-begin-turn-applied-observed',
        'legacy-activation-result-written',
        'bootstrap-cascade-delay-armed-observed',
        'engine-action-dispatched-observed',
        'legacy-cascade-result-written',
        'post-cascade-sleep-800-complete',
        'first-census-host-begin-turn-tx-read',
        'first-census-host-end-turn-tx-read',
        'manual-checkpoint-output-written',
        'subturn-cycle-header-written',
        'before-snapshot-reopened',
        'subturn-1-missing-probe',
        'mp-refill-output-written',
        'second-census-host-begin-turn-tx-read',
        'second-census-host-end-turn-tx-read',
        'final-host-fault-log-read',
        'mss-final-join-fault-log-read',
        'final-dump-delta-read',
        'final-exact-process-liveness-read',
        'host-invariant-output-written',
        'legacy-day-probe-output-written',
        'keep-alive-and-done-output-written',
        'bootstrap-native-operational-observed',
        'tail-artifact-written',
        'legacy-console-transcript-written',
        'observer-complete'
    )
    $trace = @(Get-Content -LiteralPath $result.ExecutionTracePath -Raw |
        ConvertFrom-Json -ErrorAction Stop)
    if ($trace.Count -ne $expectedTraceNames.Count) {
        throw "mock execution trace count=$($trace.Count), expected $($expectedTraceNames.Count)"
    }
    for ($traceIndex = 0; $traceIndex -lt $expectedTraceNames.Count; $traceIndex++) {
        if ([string]$trace[$traceIndex].name -ne $expectedTraceNames[$traceIndex] -or
            [int]$trace[$traceIndex].ordinal -ne ($traceIndex + 1)) {
            throw ("mock execution trace $($traceIndex + 1) changed: " +
                "$($trace[$traceIndex] | ConvertTo-Json -Compress)")
        }
    }
    $roleSleepMs = ([DateTimeOffset]$trace[1].timestamp -
        [DateTimeOffset]$trace[0].timestamp).TotalMilliseconds
    $strategicSleepMs = ([DateTimeOffset]$trace[3].timestamp -
        [DateTimeOffset]$trace[2].timestamp).TotalMilliseconds
    $postStrategicSleepMs = ([DateTimeOffset]$trace[6].timestamp -
        [DateTimeOffset]$trace[5].timestamp).TotalMilliseconds
    $postCascadeTrace = @($trace | Where-Object {
        [string]$_.name -eq 'post-cascade-sleep-800-complete'
    })
    if ($postCascadeTrace.Count -ne 1) {
        throw 'mock trace omitted its sole post-cascade completion edge'
    }
    $postCascadeSleepMs = ([DateTimeOffset]$postCascadeTrace[0].timestamp -
        [DateTimeOffset]$tail.release.engineActionDispatched.timestamp).TotalMilliseconds
    if ($roleSleepMs -lt 490 -or $strategicSleepMs -lt 390 -or
        $postStrategicSleepMs -lt 1900 -or $postCascadeSleepMs -lt 790) {
        throw ("mock explicit cadence shortened " +
            "(role=$roleSleepMs strategic=$strategicSleepMs " +
            "postStrategic=$postStrategicSleepMs postCascade=$postCascadeSleepMs)")
    }
    $causalTraceNames = @(
        'legacy-start-heading-written',
        'bootstrap-release-writer-armed',
        'bootstrap-begin-turn-applied-observed',
        'legacy-activation-result-written',
        'engine-action-dispatched-observed',
        'legacy-cascade-result-written',
        'post-cascade-sleep-800-complete'
    )
    $causalTrace = @($causalTraceNames | ForEach-Object {
        $name = $_
        $matches = @($trace | Where-Object { [string]$_.name -eq $name })
        if ($matches.Count -ne 1) {
            throw "mock trace event '$name' count=$($matches.Count), expected one"
        }
        $matches[0]
    })
    for ($causalIndex = 1; $causalIndex -lt $causalTrace.Count; $causalIndex++) {
        if ([DateTimeOffset]$causalTrace[$causalIndex].timestamp -lt
            [DateTimeOffset]$causalTrace[$causalIndex - 1].timestamp) {
            throw "mock transcript causal timestamp order changed at '$($causalTraceNames[$causalIndex])'"
        }
    }
    if ([DateTimeOffset]$causalTrace[2].timestamp -lt
            [DateTimeOffset]$tail.release.beginTurnApplied.timestamp -or
        [DateTimeOffset]$causalTrace[4].timestamp -lt
            [DateTimeOffset]$tail.release.engineActionDispatched.timestamp) {
        throw 'mock transcript result was timestamped before its typed action-dispatch edge'
    }

    $mpText = Get-Content -LiteralPath `
        (Join-Path $artifactDir 'legacy-inner-mp-refill.txt') -Raw
    $dayProbeText = Get-Content -LiteralPath `
        (Join-Path $artifactDir 'legacy-inner-day-display-probe.txt') -Raw
    $keepAliveText = Get-Content -LiteralPath `
        (Join-Path $artifactDir 'legacy-inner-keep-alive.txt') -Raw
    if ($mpText -notmatch '\[before      \] stack= mp_display= mp_byte_91=' -or
        $mpText -notmatch 'subturn_1 : MISSING' -or
        $dayProbeText -notmatch [regex]::Escape(
            'http://127.0.0.1:8077/api/inject-begin-turn-joiner?player=0x805E0002&seq=7&auto=0') -or
        $keepAliveText -notmatch 'DONE \(T\+[0-9.]+s\) -- PROCESSES LEFT RUNNING') {
        throw 'mock legacy console products no longer match the source green run'
    }
    $transcriptText = Get-Content -LiteralPath $result.ConsoleTranscriptPath -Raw
    $transcriptCursor = 0
    foreach ($fragment in @(
        '=== test_R virtual-turn (§29 foundation) -- joiner=0x805E0002 ===',
        'roles ready host=',
        'strategic reached BOTH',
        '=== STATIC install checks (patch on at boot) ===',
        'WARNING: no [virtual-turn] install log',
        "WARNING: no 'redirected' log",
        'NOTE: 4 real fault(s) during boot',
        "SNAPSHOT 'before'",
        '=== START joiner day-1 turn (activate + single income, real handle) ===',
        'joiner activate (day 1): mapped to the sole SessionPlan/bootstrap BeginTurn',
        'joiner income (day 1, cascade once): mapped to actionId=1',
        'HOST advance baseline: TX BeginTurn=0  TX EndTurn=0',
        '>>> MANUAL STEP <<<',
        '=== joiner sub-turn cycle (cascade + begin-turn inject, day=subTurn) ===',
        '=== cascade MP-refill sanity ===',
        '[before      ] stack= mp_display= mp_byte_91=',
        'subturn_1 : MISSING',
        '=== HOST invariant (whole run) ===',
        'HOST TX CCmdBeginTurnMsg total: 0',
        'HOST TX CCmdEndTurnMsg   total: 0',
        'real fault(s): 6',
        '=== §29.5 day-display probe (do this manually while alive) ===',
        '=== KEEP-ALIVE: processes LEFT RUNNING',
        'DONE (T+')) {
        $foundAt = $transcriptText.IndexOf(
            $fragment, $transcriptCursor, [StringComparison]::Ordinal)
        if ($foundAt -lt 0) {
            throw "mock legacy transcript omitted or reordered '$fragment'"
        }
        $transcriptCursor = $foundAt + $fragment.Length
    }

    # A new strict reporter fault is terminal, but only after the complete
    # source Snap("before") schedule and its forensic products are written.
    # No release watcher is armed for this case: creation of the release file
    # would itself prove that the terminal gate was applied too late.
    $faultArtifactDir = Join-Path $testRoot 'strict-fault-artifacts'
    [void][IO.Directory]::CreateDirectory($faultArtifactDir)
    $faultSimLog = Join-Path $testRoot 'strict-fault-simturns-relay.jsonl'
    $faultReleaseFile = Join-Path $faultArtifactDir 'bootstrap.release'
    $faultFailureLog = Join-Path $faultArtifactDir 'literal-inner-failure.log'
    Write-JsonLine -Path $faultSimLog -CreateNew -Value ([ordered]@{
        timestamp = [DateTimeOffset]::UtcNow.ToString('O')
        event = 'bootstrap-release-pending'
        hostHandle = $hostHandle
        joinHandle = $joinHandle
    })
    $faultDumpBaseline = Get-LiteralInnerDumpBaseline `
        -DumpRoots @($gameDir, $faultArtifactDir)
    $faultRequestStart = $httpState.Requests.Count
    $httpState.FaultTarget = '/api/turn/history?after=0&role=host'
    $faultWasTerminal = $false
    try {
        $faultObserver = Start-LiteralInnerStartupObserver `
            -ArmUtc ([DateTime]::UtcNow) `
            -RelayBase $relayBase `
            -SimRelayLog $faultSimLog `
            -BootstrapReleaseFile $faultReleaseFile `
            -FailureLogPath $faultFailureLog `
            -ArtifactDir $faultArtifactDir `
            -GameDir $gameDir `
            -ExpectedModulePath $modulePath `
            -DumpRoots @($gameDir, $faultArtifactDir) `
            -DumpBaseline $faultDumpBaseline `
            -TotalBudgetSec 30
        Publish-LiteralInnerStartupProcess -Observer $faultObserver -Role host `
            -Process $hostProcess -ExecutablePath $gameExe `
            -LogPath $hostLog -LogBaseline 0
        Publish-LiteralInnerStartupProcess -Observer $faultObserver -Role join `
            -Process $joinProcess -ExecutablePath $gameExe `
            -LogPath $joinLog -LogBaseline 0
        try {
            [void](Complete-LiteralInnerStartupObserver $faultObserver)
        } catch {
            if ($_.Exception.Message -notmatch
                    '^strict MSS pre-release safety gate failed:') { throw }
            $faultWasTerminal = $true
        }
    } finally {
        $httpState.FaultTarget = ''
    }
    if (-not $faultWasTerminal) {
        throw 'mock strict pre-release reporter fault was not terminal'
    }
    $faultBeforePath = Join-Path $faultArtifactDir 'legacy-inner-before.json'
    $faultTracePath = Join-Path $faultArtifactDir `
        'legacy-inner-execution-trace.json'
    $faultTranscriptPath = Join-Path $faultArtifactDir `
        'legacy-inner-console-transcript.txt'
    foreach ($faultArtifact in @(
            $faultBeforePath, $faultTracePath, $faultTranscriptPath,
            $faultFailureLog)) {
        if (-not (Test-Path -LiteralPath $faultArtifact -PathType Leaf)) {
            throw "strict pre-release failure omitted forensic artifact '$faultArtifact'"
        }
    }
    if ((Test-Path -LiteralPath $faultReleaseFile) -or
        (Test-Path -LiteralPath `
            (Join-Path $faultArtifactDir 'legacy-inner-tail.json'))) {
        throw 'strict pre-release failure crossed the one-shot release boundary'
    }
    $faultBefore = Get-Content -LiteralPath $faultBeforePath -Raw |
        ConvertFrom-Json -ErrorAction Stop
    if ([bool]$faultBefore.strictMssAssertions.passed -or
        @($faultBefore.strictMssAssertions.failures).Count -ne 2 -or
        [bool]$faultBefore.legacyObservationProjection.sourceFallbackUsed -or
        @($faultBefore.legacyObservationProjection.peers.host.orderedReads).Count -ne 7 -or
        @($faultBefore.legacyObservationProjection.peers.join.orderedReads).Count -ne 7 -or
        [string]$faultBefore.legacyObservationProjection.peers.host.orderedReads[1].product -ne
            'turn_info_msg_latest_unavailable' -or
        [string]$faultBefore.legacyObservationProjection.peers.host.orderedReads[5].product -ne
            'stack_moves_unavailable') {
        throw 'strict pre-release before artifact lost literal reads or honest unavailable products'
    }
    $faultTrace = @(Get-Content -LiteralPath $faultTracePath -Raw |
        ConvertFrom-Json -ErrorAction Stop)
    $faultSnapshotTrace = @($faultTrace | Where-Object {
        [string]$_.name -match '^snapshot-(host|join)-[0-9]{2}-'
    } | ForEach-Object { [string]$_.name })
    $expectedFaultSnapshotTrace = @($expectedTraceNames | Where-Object {
        $_ -match '^snapshot-(host|join)-[0-9]{2}-'
    })
    if ($faultSnapshotTrace.Count -ne 14 -or
        ($faultSnapshotTrace -join '|') -ne ($expectedFaultSnapshotTrace -join '|') -or
        [string]$faultTrace[-1].name -ne 'strict-pre-release-gate-failed' -or
        @($faultTrace | Where-Object {
            [string]$_.name -eq 'bootstrap-release-writer-armed'
        }).Count -ne 0) {
        throw 'strict pre-release trace did not finish all fourteen ordered read slots before failing'
    }
    $faultRequests = @($httpState.Requests | Select-Object -Skip $faultRequestStart)
    $expectedFaultTargets = @($expectedTargets[0..13])
    if ($faultRequests.Count -ne $expectedFaultTargets.Count) {
        throw "strict pre-release HTTP count=$($faultRequests.Count), expected $($expectedFaultTargets.Count)"
    }
    for ($faultRequestIndex = 0;
         $faultRequestIndex -lt $expectedFaultTargets.Count;
         $faultRequestIndex++) {
        if ([string]$faultRequests[$faultRequestIndex].method -ne 'GET' -or
            [string]$faultRequests[$faultRequestIndex].target -ne
                $expectedFaultTargets[$faultRequestIndex]) {
            throw "strict pre-release read $($faultRequestIndex + 1) was retried, skipped, or reordered"
        }
    }
    $faultTranscript = Get-Content -LiteralPath $faultTranscriptPath -Raw
    if ($faultTranscript -match '=== START joiner day-1 turn' -or
        $faultTranscript -notmatch 'FAIL: strict MSS pre-release safety gate failed:') {
        throw 'strict pre-release transcript crossed release or omitted the terminal diagnosis'
    }
    'literal-inner-startup static + mocked runtime: PASS'
} finally {
    if ($observer -and -not [bool]$observer.Disposed) {
        Stop-LiteralInnerStartupObserver $observer
    }
    if ($faultObserver -and -not [bool]$faultObserver.Disposed) {
        Stop-LiteralInnerStartupObserver $faultObserver
    }
    if ($httpState.Ready -and -not [string]::IsNullOrWhiteSpace($relayBase)) {
        try { Invoke-RestMethod "$relayBase/__stop" -TimeoutSec 2 | Out-Null } catch {}
    }
    $eventState.StopRequested = $true
    if ($httpAsync) {
        try { [void]$httpWorker.EndInvoke($httpAsync) } catch {}
    }
    if ($eventAsync) {
        try { [void]$eventWorker.EndInvoke($eventAsync) } catch {}
    }
    $httpWorker.Dispose()
    $eventWorker.Dispose()
    foreach ($process in @($hostProcess, $joinProcess)) {
        if ($process) {
            try {
                $process.Refresh()
                if (-not $process.HasExited) { $process.Kill($true) }
            } catch {}
            $process.Dispose()
        }
    }
}
