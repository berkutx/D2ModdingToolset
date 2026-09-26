# Removable DebugTest-only port of the inner test_R_virtual_turn.ps1 observer.
# The explicit source cadence and every ordered observation are retained as a
# legacyObservationProjection.  Reporter/coordinator data that did not exist in
# the source is kept separately as mssEvidence, and terminal MSS quality rules
# are reported separately as strictMssAssertions.  This file deliberately owns
# no gameplay/UI action.  Its sole gameplay/bootstrap mutation is creation of
# the one bootstrap release file after the old strategic +2 s Snap("before")
# boundary.  The production coordinator converts that release edge into the
# staged SessionPlan fan-out and the single delayed bootstrap EngineAction.

function Get-LiteralInnerDumpBaseline {
    param([Parameter(Mandatory)][string[]]$DumpRoots)
    $normalizedRoots = @($DumpRoots | ForEach-Object {
        $resolved = [IO.Path]::GetFullPath($_)
        if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
            throw "literal inner dump root is missing: $resolved"
        }
        $resolved
    } | Sort-Object -Unique)
    if ($normalizedRoots.Count -eq 0) {
        throw 'literal inner observer requires at least one dump root'
    }
    $files = @($normalizedRoots | ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Filter 'd2_uef_*.dmp' -File `
            -ErrorAction Stop
    } | Sort-Object FullName -Unique | ForEach-Object {
        [pscustomobject]@{
            path = [IO.Path]::GetFullPath($_.FullName)
            length = [long]$_.Length
            lastWriteUtc = $_.LastWriteTimeUtc.ToString('O')
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        }
    })
    return [pscustomobject]@{
        capturedAt = [DateTimeOffset]::UtcNow.ToString('O')
        roots = $normalizedRoots
        files = $files
    }
}

function Start-LiteralInnerStartupObserver {
    param(
        [Parameter(Mandatory)][DateTime]$ArmUtc,
        [Parameter(Mandatory)][string]$RelayBase,
        [Parameter(Mandatory)][string]$SimRelayLog,
        [Parameter(Mandatory)][string]$BootstrapReleaseFile,
        [Parameter(Mandatory)][string]$FailureLogPath,
        [Parameter(Mandatory)][string]$ArtifactDir,
        [Parameter(Mandatory)][string]$GameDir,
        [Parameter(Mandatory)][string]$ExpectedModulePath,
        [Parameter(Mandatory)][string[]]$DumpRoots,
        [Parameter(Mandatory)][object]$DumpBaseline,
        [ValidateRange(30, 300)][int]$TotalBudgetSec = 120
    )
    foreach ($path in @($ArtifactDir, $GameDir)) {
        if (-not (Test-Path -LiteralPath $path -PathType Container)) {
            throw "literal inner observer directory is missing: $path"
        }
    }
    if (-not [IO.Path]::IsPathFullyQualified($BootstrapReleaseFile)) {
        throw 'literal inner bootstrap release file must be absolute'
    }
    $BootstrapReleaseFile = [IO.Path]::GetFullPath($BootstrapReleaseFile)
    $bootstrapReleaseParent = Split-Path -Parent $BootstrapReleaseFile
    if (-not (Test-Path -LiteralPath $bootstrapReleaseParent -PathType Container)) {
        throw "literal inner bootstrap release parent is missing: $bootstrapReleaseParent"
    }
    if (Test-Path -LiteralPath $BootstrapReleaseFile) {
        throw "literal inner bootstrap release file already exists: $BootstrapReleaseFile"
    }
    if (Test-Path -LiteralPath $FailureLogPath) {
        throw "literal inner failure log already exists: $FailureLogPath"
    }
    if (-not [IO.Path]::IsPathFullyQualified($FailureLogPath)) {
        throw 'literal inner failure log path must be absolute'
    }
    $FailureLogPath = [IO.Path]::GetFullPath($FailureLogPath)
    $SimRelayLog = [IO.Path]::GetFullPath($SimRelayLog)
    $ArtifactDir = [IO.Path]::GetFullPath($ArtifactDir)
    $GameDir = [IO.Path]::GetFullPath($GameDir)
    $ExpectedModulePath = [IO.Path]::GetFullPath($ExpectedModulePath)
    $expectedGameModule = [IO.Path]::GetFullPath(
        (Join-Path $GameDir 'mss32.dll'))
    if (-not [string]::Equals(
            $ExpectedModulePath, $expectedGameModule,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw ("literal inner expected module '$ExpectedModulePath' is not " +
            "the exact game module '$expectedGameModule'")
    }
    if (-not (Test-Path -LiteralPath $ExpectedModulePath -PathType Leaf)) {
        throw "literal inner exact game module is missing: $ExpectedModulePath"
    }
    $expectedGameExe = [IO.Path]::GetFullPath(
        (Join-Path $GameDir 'Discipl2.exe'))
    if (-not (Test-Path -LiteralPath $expectedGameExe -PathType Leaf)) {
        throw "literal inner exact game executable is missing: $expectedGameExe"
    }
    $evidencePaths = @(
        (Join-Path $ArtifactDir 'legacy-inner-before.json'),
        (Join-Path $ArtifactDir 'legacy-inner-tail.json'),
        (Join-Path $ArtifactDir 'legacy-inner-execution-trace.json'),
        (Join-Path $ArtifactDir 'legacy-inner-static-install.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-manual-checkpoint.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-subturn-cycle.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-mp-refill.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-host-invariant.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-day-display-probe.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-keep-alive.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-console-transcript.txt'),
        (Join-Path $ArtifactDir 'legacy-inner-subturn-1.json')
    ) | ForEach-Object { [IO.Path]::GetFullPath($_) }
    $reservedPaths = @(
        $BootstrapReleaseFile,
        $FailureLogPath,
        $SimRelayLog,
        $evidencePaths
    ) | ForEach-Object { [IO.Path]::GetFullPath($_) }
    $reservedKeys = @($reservedPaths | ForEach-Object { $_.ToUpperInvariant() })
    if (@($reservedKeys | Sort-Object -Unique).Count -ne $reservedKeys.Count) {
        throw 'literal inner release/failure/sim-log/evidence paths must all be distinct'
    }
    foreach ($evidencePath in $evidencePaths) {
        if (Test-Path -LiteralPath $evidencePath) {
            throw "literal inner stale evidence path exists before bootstrap: $evidencePath"
        }
    }
    $failureLogParent = Split-Path -Parent $FailureLogPath
    if (-not (Test-Path -LiteralPath $failureLogParent -PathType Container)) {
        throw "literal inner failure log parent is missing: $failureLogParent"
    }
    $normalizedDumpRoots = @($DumpRoots | ForEach-Object {
        $resolved = [IO.Path]::GetFullPath($_)
        if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
            throw "literal inner dump root is missing: $resolved"
        }
        $resolved
    } | Sort-Object -Unique)
    if ($normalizedDumpRoots.Count -eq 0) {
        throw 'literal inner observer requires at least one dump root'
    }
    $baselineRoots = @($DumpBaseline.roots | ForEach-Object {
        [IO.Path]::GetFullPath([string]$_)
    } | Sort-Object -Unique)
    if ($baselineRoots.Count -ne $normalizedDumpRoots.Count) {
        throw 'literal inner dump baseline roots do not match the requested roots'
    }
    for ($rootIndex = 0; $rootIndex -lt $normalizedDumpRoots.Count; $rootIndex++) {
        if (-not [string]::Equals(
                $normalizedDumpRoots[$rootIndex], $baselineRoots[$rootIndex],
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'literal inner dump baseline roots changed before observer arm'
        }
    }
    try {
        $baselineCapturedUtc = [DateTimeOffset]::Parse(
            [string]$DumpBaseline.capturedAt,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
    } catch {
        throw 'literal inner dump baseline has no valid pre-arm timestamp'
    }
    $armOffset = [DateTimeOffset]$ArmUtc
    if ($baselineCapturedUtc -gt $armOffset) {
        throw 'literal inner dump baseline was captured after the role-loop arm edge'
    }
    $initialDumps = @($DumpBaseline.files)

    $state = [hashtable]::Synchronized(@{
        HostPid = [long]0
        JoinPid = [long]0
        HostLog = ''
        JoinLog = ''
        HostLogBaseline = [long]-1
        JoinLogBaseline = [long]-1
        HostProcess = $null
        JoinProcess = $null
        HostExecutablePath = ''
        JoinExecutablePath = ''
        StopRequested = $false
        Completed = $false
        Error = ''
        Result = $null
        Phase = 'role-poll'
        OperationalReady = $false
        OperationalWitness = $null
    })
    $worker = [PowerShell]::Create()
    $workerSource = @'
param($State, $ArmUtc, $RelayBase, $SimRelayLog, $BootstrapReleaseFile,
      $FailureLogPath, $ArtifactDir, $GameDir, $ExpectedModulePath,
      $DumpRoots, $InitialDumps, $DumpBaselineCapturedUtc, $TotalBudgetSec)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$executionTrace = [System.Collections.Generic.List[object]]::new()
$legacyTranscript = [System.Collections.Generic.List[string]]::new()

function Trace-Step([string]$Name, [hashtable]$Fields = @{}) {
    $record = [ordered]@{
        ordinal = $executionTrace.Count + 1
        name = $Name
        timestamp = [DateTimeOffset]::UtcNow.ToString('O')
    }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    $executionTrace.Add([pscustomobject]$record)
}

function Legacy-Log([string]$Message) {
    $legacyTranscript.Add(("{0:HH:mm:ss.fff}  {1}" -f (Get-Date), $Message))
}

function Property([object]$Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function ElapsedSeconds {
    # The source Elapsed helper rounded every shared-budget/collapse decision
    # to one decimal place.
    return [Math]::Round(([DateTime]::UtcNow - $ArmUtc).TotalSeconds, 1)
}

function Read-CompleteLog([string]$Path, [long]$Baseline) {
    if ($Baseline -lt 0) {
        throw "literal inner log baseline was not published for '$Path'"
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return @()
    }
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ([long]$file.Length -lt $Baseline) {
        throw "literal inner exact-owned log shrank below baseline ($($file.Length) < $Baseline): $Path"
    }
    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $stream = [IO.File]::Open(
        $file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
    try {
        [void]$stream.Seek($Baseline, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new(
            $stream, [Text.Encoding]::UTF8, $true, 4096, $true)
        try { $text = $reader.ReadToEnd() }
        finally { $reader.Dispose() }
    } finally {
        $stream.Dispose()
    }
    if ([string]::IsNullOrEmpty($text)) { return @() }
    $lines = @($text -split '\r?\n')
    if (-not $text.EndsWith("`n", [StringComparison]::Ordinal)) {
        if ($lines.Count -le 1) { return @() }
        return @($lines[0..($lines.Count - 2)])
    }
    return $lines
}

function Read-SimEvents {
    $result = [System.Collections.Generic.List[object]]::new()
    if (-not (Test-Path -LiteralPath $SimRelayLog -PathType Leaf)) {
        return @()
    }
    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $stream = [IO.File]::Open(
        $SimRelayLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
    try {
        $reader = [IO.StreamReader]::new(
            $stream, [Text.Encoding]::UTF8, $true, 4096, $true)
        try { $text = $reader.ReadToEnd() }
        finally { $reader.Dispose() }
    } finally {
        $stream.Dispose()
    }
    if ([string]::IsNullOrEmpty($text)) { return @() }
    $completeFinalLine = $text.EndsWith("`n", [StringComparison]::Ordinal)
    $lines = @($text -split '\r?\n')
    for ($index = 0; $index -lt $lines.Count; $index++) {
        # A JSONL record is published only by its terminating newline. Even a
        # syntactically complete JSON object in the writer's live tail is not
        # causally visible yet and must never open an observer gate.
        $isUnpublishedTail = -not $completeFinalLine -and
            $index -eq ($lines.Count - 1)
        if ($isUnpublishedTail) { break }
        $line = $lines[$index]
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try {
            $result.Add(($line | ConvertFrom-Json -ErrorAction Stop))
        } catch {
            throw "completed sim-relay JSONL record $($index + 1) is malformed: $($_.Exception.Message)"
        }
    }
    return @($result)
}

function Invoke-JsonGet([string]$RelativePath, [int]$TimeoutSec = 6) {
    return Invoke-RestMethod -Method Get -Uri "$RelayBase$RelativePath" `
        -TimeoutSec $TimeoutSec
}

function Invoke-StrictObservationOnce(
        [System.Collections.Generic.List[string]]$Failures,
        [string]$Label,
        [string]$TraceName,
        [scriptblock]$Action,
        [switch]$CheckTerminalFault) {
    # A strict MSS observation may make the run terminal, but it must not
    # truncate the source Snap("before") read schedule.  Execute the supplied
    # read exactly once, retain an explicit unavailable value on failure, and
    # let the caller apply the accumulated terminal gate only after the before
    # artifact and execution trace have been durably written.
    try {
        $value = & $Action
        if ($CheckTerminalFault -and
            $null -ne (Property $value 'terminalFault')) {
            $Failures.Add(
                "$Label reported terminalFault: " +
                ((Property $value 'terminalFault') | ConvertTo-Json -Compress))
        }
        return $value
    } catch {
        $message = [string]$_.Exception.Message
        $Failures.Add("$Label failed: $message")
        return [pscustomobject]@{
            observationUnavailable = $true
            observationLabel = $Label
            observationError = $message
            terminalFault = $null
            latestSeq = 0
            events = @()
            players = @()
            stacks = @()
        }
    } finally {
        if (-not [string]::IsNullOrWhiteSpace($TraceName)) {
            Trace-Step $TraceName
        }
    }
}

function Get-OnlyEvent([object[]]$Events,
                       [string]$Event,
                       [string]$Role = '',
                       [object]$Day = $null) {
    $matches = @($Events | Where-Object {
        [string](Property $_ 'event') -eq $Event -and
        ([string]::IsNullOrEmpty($Role) -or
            [string](Property $_ 'role') -eq $Role) -and
        ($null -eq $Day -or [long](Property $_ 'day') -eq [long]$Day)
    })
    if ($matches.Count -ne 1) {
        $label = "$Event/$Role"
        if ($null -ne $Day) { $label += "/day=$([long]$Day)" }
        throw "literal inner event '$label' count=$($matches.Count), expected exactly one"
    }
    return $matches[0]
}

function Get-EventRecordIndex([object[]]$Events, [object]$Needle) {
    for ($index = 0; $index -lt $Events.Count; $index++) {
        if ([object]::ReferenceEquals($Events[$index], $Needle)) { return $index }
    }
    return -1
}

function Assert-NoSimRelayFault([object[]]$Events) {
    $fault = @($Events | Where-Object {
        [string](Property $_ 'event') -in @(
            'session-faulted', 'protocol-error', 'server-error',
            'socket-error', 'hello-rejected', 'peer-disconnected',
            'bootstrap-release-file-rejected')
    } | Select-Object -First 1)
    if ($fault.Count -ne 0) {
        throw "literal inner sim-relay faulted: $($fault[0] | ConvertTo-Json -Compress)"
    }
}

function Parse-EventTimestamp([object]$Event, [string]$Label) {
    $raw = Property $Event 'timestamp'
    if ($raw -is [DateTimeOffset]) { return [DateTimeOffset]$raw }
    if ($raw -is [DateTime]) { return [DateTimeOffset]([DateTime]$raw) }
    $text = [string]$raw
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "literal inner event '$Label' omitted timestamp"
    }
    try {
        return [DateTimeOffset]::Parse(
            $text,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
    } catch {
        throw "literal inner event '$Label' has invalid timestamp '$text'"
    }
}

function Count-Marker([string[]]$Lines, [string]$Marker) {
    return @($Lines | Select-String -SimpleMatch -Pattern $Marker).Count
}

function Get-LegacyRealFaults([string[]]$Lines) {
    return @($Lines | Select-String -Pattern '\[VEH #|\[UEF' |
        Where-Object {
            $_.Line -notmatch 'installed' -and
            $_.Line -notmatch '0x406D1388'
        } | ForEach-Object Line)
}

function Get-ProductionFaults([string[]]$Lines) {
    return @($Lines | Select-String -Pattern @(
        '\[simturns\] terminal fault:',
        '\[SIMTURNS\] terminal pipe fault:',
        '\[simturns\] preflight mismatch',
        '\[simturns\] detour preflight mismatch',
        '55FC74',
        'midCommandQueue2PushHooked: message with id 21 is rejected due to outdated sequence number'
    ) | ForEach-Object Line)
}

function Get-ExactProcessSnapshot([System.Diagnostics.Process]$Process,
                                  [long]$ExpectedPid,
                                  [string]$Role,
                                  [string]$PublishedExecutablePath) {
    if ($null -eq $Process -or $ExpectedPid -le 0 -or
        [long]$Process.Id -ne $ExpectedPid) {
        throw "literal inner $Role exact Process object was not published"
    }
    $Process.Refresh()
    if ($Process.HasExited) {
        throw "literal inner $Role process pid=$ExpectedPid exited"
    }
    if ([string]::IsNullOrWhiteSpace($PublishedExecutablePath) -or
        -not [IO.Path]::IsPathFullyQualified($PublishedExecutablePath)) {
        throw "literal inner $Role launch executable provenance was not published"
    }
    $path = [IO.Path]::GetFullPath($PublishedExecutablePath)
    $expectedExe = [IO.Path]::GetFullPath((Join-Path $GameDir 'Discipl2.exe'))
    if (-not [string]::Equals(
            [IO.Path]::GetFullPath($path), $expectedExe,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "literal inner $Role process path '$path' is not '$expectedExe'"
    }
    $startTime = $Process.StartTime.ToUniversalTime().ToString('O')
    return [pscustomobject]@{
        role = $Role
        pid = $ExpectedPid
        processName = [string]$Process.ProcessName
        path = $path
        startTime = $startTime
        hasExited = [bool]$Process.HasExited
    }
}

function Assert-RelayIdentity([object]$RelayState, [long]$HostPid, [long]$JoinPid) {
    if ($null -ne (Property $RelayState 'terminalFault')) {
        throw 'literal inner /api/state reported a terminal DebugTest relay fault'
    }
    $roles = Property $RelayState 'roles'
    $hostRole = Property $roles 'host'
    $joinRole = Property $roles 'join'
    if ($null -eq $hostRole -or $null -eq $joinRole) {
        throw 'literal inner /api/state omitted host or join at the snapshot boundary'
    }
    foreach ($entry in @(
        @{ name = 'host'; value = $hostRole; pid = $HostPid },
        @{ name = 'join'; value = $joinRole; pid = $JoinPid }
    )) {
        if (-not [bool](Property $entry.value 'connected') -or
            [long](Property $entry.value 'pid') -ne [long]$entry.pid) {
            throw "literal inner $($entry.name) relay identity is not the exact owned PID"
        }
        $actualModule = [IO.Path]::GetFullPath(
            [string](Property $entry.value 'modulePath'))
        if (-not [string]::Equals(
                $actualModule, $ExpectedModulePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "literal inner $($entry.name) module '$actualModule' is not '$ExpectedModulePath'"
        }
    }
    return [pscustomobject]@{ host = $hostRole; join = $joinRole }
}

function Write-NewUtf8File([string]$Path, [string]$Text) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::Read)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Write-NewJson([string]$Path, [object]$Value) {
    Write-NewUtf8File $Path ($Value | ConvertTo-Json -Depth 24)
}

function Count-TurnKind([object]$History, [string]$Kind, [string]$Label) {
    if ($null -ne (Property $History 'terminalFault')) {
        throw "literal inner $Label observed a terminal DebugTest relay fault"
    }
    $events = @((Property $History 'events'))
    return @($events | Where-Object {
        [string](Property $_ 'kind') -eq $Kind
    }).Count
}

function Get-HostAdvanceCounts([string]$TracePrefix) {
    # HostAdvanceCounts in the source performed two ordered, independent full
    # packet-log scans.  Preserve two observations rather than filtering one
    # reporter response twice.
    $beginHistory = Invoke-JsonGet '/api/turn/history?after=0&role=host' 6
    Trace-Step "$TracePrefix-host-begin-turn-tx-read"
    $beginCount = Count-TurnKind $beginHistory `
        'stock-begin-turn-send-returned' "$TracePrefix BeginTurn TX census"
    $endHistory = Invoke-JsonGet '/api/turn/history?after=0&role=host' 6
    Trace-Step "$TracePrefix-host-end-turn-tx-read"
    $endCount = Count-TurnKind $endHistory `
        'stock-end-turn-send-returned' "$TracePrefix EndTurn TX census"
    return [pscustomobject]@{
        beginTurnTx = $beginCount
        endTurnTx = $endCount
        beginLatestSeq = [long](Property $beginHistory 'latestSeq')
        endLatestSeq = [long](Property $endHistory 'latestSeq')
        beginEvents = @((Property $beginHistory 'events'))
        endEvents = @((Property $endHistory 'events'))
    }
}

function Get-PeerSnapshotProjection([ValidateSet('host', 'join')][string]$Role,
                                    [string]$LogPath,
                                    [long]$LogBaseline,
                                    [System.Collections.Generic.List[string]]$Failures) {
    # audit_state.ps1 made seven reads for each peer, and completed every host
    # read before beginning join.  The MSS reporter cannot reproduce the old
    # packet bytes, so each slot records the closest read-only MSS source plus
    # an explicit limitation.  The seven calls and their order are retained.
    $bindLogLines = @(Invoke-StrictObservationOnce $Failures `
        "$Role snapshot slot 1 hook log" `
        "snapshot-$Role-01-hook-bind-log-read" `
        { Read-CompleteLog $LogPath $LogBaseline })
    $turnInfo = Invoke-StrictObservationOnce $Failures `
        "$Role snapshot slot 2 turn-history placeholder" `
        "snapshot-$Role-02-turn-info-read" `
        { Invoke-JsonGet "/api/turn/history?after=0&role=$Role" 6 } `
        -CheckTerminalFault
    $refreshStacks = Invoke-StrictObservationOnce $Failures `
        "$Role snapshot slot 3 world stacks" `
        "snapshot-$Role-03-refresh-stacks-read" `
        { Invoke-JsonGet "/api/world?role=$Role" 6 } `
        -CheckTerminalFault
    $stateDumpLogLines = @(Invoke-StrictObservationOnce $Failures `
        "$Role snapshot slot 4 state-dump log" `
        "snapshot-$Role-04-state-dump-log-read" `
        { Read-CompleteLog $LogPath $LogBaseline })
    $beginTurn = Invoke-StrictObservationOnce $Failures `
        "$Role snapshot slot 5 begin-turn history" `
        "snapshot-$Role-05-begin-turn-read" `
        { Invoke-JsonGet "/api/turn/history?after=0&role=$Role" 6 } `
        -CheckTerminalFault
    $stackMoves = Invoke-StrictObservationOnce $Failures `
        "$Role snapshot slot 6 world placeholder" `
        "snapshot-$Role-06-stack-moves-read" `
        { Invoke-JsonGet "/api/world?role=$Role" 6 } `
        -CheckTerminalFault
    $refreshPlayers = Invoke-StrictObservationOnce $Failures `
        "$Role snapshot slot 7 world players" `
        "snapshot-$Role-07-refresh-players-read" `
        { Invoke-JsonGet "/api/world?role=$Role" 6 } `
        -CheckTerminalFault
    return [pscustomobject]@{
        role = $Role
        orderedReads = @(
            [pscustomobject]@{ ordinal = 1; source = 'hook-log/full'; product = 'bind'; lines = $bindLogLines },
            [pscustomobject]@{ ordinal = 2; source = 'turn-history'; product = 'turn_info_msg_latest_unavailable'; nearbyEvidence = $turnInfo; unavailable = @('typed CCmdTurnInfo observation', 'raw packet bytes', 'PREV/NEW resource provenance', 'DPID') },
            [pscustomobject]@{ ordinal = 3; source = 'world'; product = 'all_stacks_in_latest_refresh'; evidence = $refreshStacks; unavailable = @('RX CRefreshInfo provenance', 'wire offsets') },
            [pscustomobject]@{ ordinal = 4; source = 'hook-log/full'; product = 'state_dump_latest'; lines = $stateDumpLogLines; unavailable = @('legacy state-dump schema') },
            [pscustomobject]@{ ordinal = 5; source = 'turn-history'; product = 'begin_turn_msgs'; evidence = $beginTurn; unavailable = @('raw packet bytes', 'legacy signed/broadcast decoder result') },
            [pscustomobject]@{ ordinal = 6; source = 'world'; product = 'stack_moves_unavailable'; nearbyEvidence = $stackMoves; unavailable = @('typed stack-move observation', 'last-five wire events', 'path', 'direction', 'waypoints') },
            [pscustomobject]@{ ordinal = 7; source = 'world'; product = 'crefresh_players'; evidence = $refreshPlayers; unavailable = @('RX CRefreshInfo provenance', 'raw name length', 'repeated handle') }
        )
        bindLogLines = $bindLogLines
        turnInfo = $turnInfo
        refreshStacks = $refreshStacks
        stateDumpLogLines = $stateDumpLogLines
        beginTurn = $beginTurn
        stackMoves = $stackMoves
        refreshPlayers = $refreshPlayers
    }
}

function Get-DumpSnapshot {
    return @($DumpRoots | ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Filter 'd2_uef_*.dmp' -File `
            -ErrorAction Stop
    } | Sort-Object FullName -Unique | ForEach-Object {
        [pscustomobject]@{
            path = [IO.Path]::GetFullPath($_.FullName)
            length = [long]$_.Length
            lastWriteUtc = $_.LastWriteTimeUtc.ToString('O')
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        }
    })
}

function Get-ChangedDumps([object[]]$Initial) {
    $baseline = @{}
    foreach ($entry in $Initial) { $baseline[[string]$entry.path] = $entry }
    return @(Get-DumpSnapshot | Where-Object {
        $before = $baseline[[string]$_.path]
        $null -eq $before -or
        [long]$before.length -ne [long]$_.length -or
        [string]$before.lastWriteUtc -ne [string]$_.lastWriteUtc -or
        [string]$before.sha256 -ne [string]$_.sha256
    })
}

function Get-JoinMovementProjection([object]$World, [string]$Label) {
    $players = @((Property $World 'players'))
    $selfHumans = @($players | Where-Object {
        [string](Property $_ 'relation') -eq 'self' -and
        [bool](Property $_ 'human')
    })
    if ($selfHumans.Count -ne 1) {
        throw "literal inner $Label has $($selfHumans.Count) join self-human players, expected one"
    }
    $selfId = [string](Property $selfHumans[0] 'id')
    if ([string]::IsNullOrWhiteSpace($selfId)) {
        throw "literal inner $Label join self-human player omitted id"
    }
    $owned = @(@((Property $World 'stacks')) | Where-Object {
        [string](Property $_ 'owner') -eq $selfId
    } | Sort-Object { [string](Property $_ 'id') })
    if ($owned.Count -eq 0) {
        throw "literal inner $Label omitted all join-owned stacks"
    }
    $records = @($owned | ForEach-Object {
        $stackId = [string](Property $_ 'id')
        $movementValue = Property $_ 'movement'
        if ([string]::IsNullOrWhiteSpace($stackId) -or $null -eq $movementValue) {
            throw "literal inner $Label has a join-owned stack without id/movement"
        }
        try { [int]$movement = $movementValue }
        catch {
            throw "literal inner $Label movement for '$stackId' is not an integer: '$movementValue'"
        }
        [pscustomobject]@{ id = $stackId; movement = $movement }
    })
    if ($records.Count -ne $owned.Count) {
        throw "literal inner $Label movement projection lost a join-owned stack"
    }
    return [pscustomobject]@{
        selfId = $selfId
        records = $records
        movement = @($records | ForEach-Object movement)
    }
}

try {
    Trace-Step 'role-loop-armed'
    Legacy-Log '=== test_R virtual-turn (§29 foundation) -- joiner=0x805E0002 ==='
    $roleSamples = 0
    $roleStateSnapshot = $null
    $hostPid = [long]0
    $joinPid = [long]0

    # Literal test_R_virtual_turn.ps1 role loop: +500, then exactly one
    # aggregate /api/state read.  Observation failure advances to the next
    # sample; it never launches or resubmits anything.
    while ((ElapsedSeconds) -lt $TotalBudgetSec) {
        Start-Sleep -Milliseconds 500
        Trace-Step 'role-poll-sleep-500-complete'
        if ([bool]$State.StopRequested) { return }
        $roleSamples++
        $response = $null
        try { $response = Invoke-JsonGet '/api/state' 2 } catch { continue }
        Trace-Step 'role-state-read'
        if ($null -ne (Property $response 'terminalFault')) {
            throw 'literal inner /api/state reported a terminal DebugTest relay fault during role polling'
        }
        $roles = Property $response 'roles'
        $hostRole = Property $roles 'host'
        $joinRole = Property $roles 'join'
        if ($null -eq $hostRole -or $null -eq $joinRole) { continue }
        $publishedHostPid = [long]$State.HostPid
        $publishedJoinPid = [long]$State.JoinPid
        if ($publishedHostPid -le 0 -or $publishedJoinPid -le 0) { continue }
        if ($publishedHostPid -eq $publishedJoinPid) {
            throw 'literal inner host and join published the same PID'
        }
        if (-not [bool](Property $hostRole 'connected') -or
            -not [bool](Property $joinRole 'connected')) { continue }
        $reportedHostPid = [long](Property $hostRole 'pid')
        $reportedJoinPid = [long](Property $joinRole 'pid')
        if ($reportedHostPid -ne $publishedHostPid -or
            $reportedJoinPid -ne $publishedJoinPid) {
            throw ("literal inner connected immutable role PID mismatch " +
                "(host=$reportedHostPid/$publishedHostPid " +
                "join=$reportedJoinPid/$publishedJoinPid)")
        }
        foreach ($roleEntry in @($hostRole, $joinRole)) {
            $sampleModule = [string](Property $roleEntry 'modulePath')
            if ([string]::IsNullOrWhiteSpace($sampleModule)) {
                throw 'literal inner settled role sample omitted immutable modulePath identity'
            }
            try { $sampleModule = [IO.Path]::GetFullPath($sampleModule) }
            catch {
                throw "literal inner settled role sample has invalid modulePath: $($_.Exception.Message)"
            }
            if (-not [string]::Equals(
                    $sampleModule, $ExpectedModulePath,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw "literal inner settled role sample module '$sampleModule' is not '$ExpectedModulePath'"
            }
        }
        $hostPid = $publishedHostPid
        $joinPid = $publishedJoinPid
        $roleStateSnapshot = $response
        Legacy-Log "roles ready host=$hostPid joiner=$joinPid (T+$(ElapsedSeconds)s)"
        break
    }
    if ($null -eq $roleStateSnapshot) {
        throw 'FAIL: roles never settled within the fixed 120-second budget'
    }
    $State.Phase = 'strategic-poll'
    $rolesReadyUtc = [DateTime]::UtcNow
    # The current harness publishes roles only after both separately launched
    # clients have connected.  Give the subsequent map-load phase its own
    # bounded passive deadline; this never resubmits navigation or UI actions.
    $strategicDeadlineUtc = $rolesReadyUtc.AddSeconds($TotalBudgetSec)

    $strategicSamples = 0
    $hostStrategicAtSeconds = $null
    $hostStrategic = $false
    $joinStrategic = $false
    $sourceJoinSyncBudgetSeconds = 14
    $sourceJoinSyncBudgetExceeded = $false
    $joinCollapseEvidence = $null
    $strategicHostLines = @()
    $strategicJoinLines = @()
    while ([DateTime]::UtcNow -lt $strategicDeadlineUtc) {
        Start-Sleep -Milliseconds 400
        Trace-Step 'strategic-poll-sleep-400-complete'
        if ([bool]$State.StopRequested) { return }
        $hostLog = [string]$State.HostLog
        $joinLog = [string]$State.JoinLog
        if ([string]::IsNullOrWhiteSpace($hostLog) -or
            [string]::IsNullOrWhiteSpace($joinLog) -or
            -not (Test-Path -LiteralPath $hostLog -PathType Leaf) -or
            -not (Test-Path -LiteralPath $joinLog -PathType Leaf)) {
            continue
        }
        $strategicSamples++
        # Preserve the old read order: full host hook log, then full join log.
        $strategicHostLines = @(
            Read-CompleteLog $hostLog ([long]$State.HostLogBaseline))
        Trace-Step 'strategic-host-log-read'
        $strategicJoinLines = @(
            Read-CompleteLog $joinLog ([long]$State.JoinLogBaseline))
        Trace-Step 'strategic-join-log-read'
        $hostStrategic = [bool]($strategicHostLines -match
            '\[testdrv\] bind DLG_STRATEGIC::BTN_GUARD')
        $joinStrategic = [bool]($strategicJoinLines -match
            '\[testdrv\] bind DLG_ISO_PAL::BTN_RES_PAL')
        if ($hostStrategic -and $joinStrategic) { break }
        if ($hostStrategic -and $null -eq $hostStrategicAtSeconds) {
            $hostStrategicAtSeconds = ElapsedSeconds
            Legacy-Log ("host at strategic (T+$hostStrategicAtSeconds s) -- " +
                'waiting <=14s for joiner sync...')
        }
        if ($null -ne $hostStrategicAtSeconds -and
            -not $sourceJoinSyncBudgetExceeded -and
            ((ElapsedSeconds) - [double]$hostStrategicAtSeconds) -gt
                $sourceJoinSyncBudgetSeconds) {
            $joinCollapseEvidence = [pscustomobject]@{
                injectThrew = [bool]($strategicJoinLines -match 'sub_55B948 threw')
                stuckMessageBox = [bool]($strategicJoinLines -match
                    'DLG_MESSAGE_BOX(?:\.|::)BTN_OK')
            }
            $sourceJoinSyncBudgetExceeded = $true
            if ($joinCollapseEvidence.injectThrew -or
                $joinCollapseEvidence.stuckMessageBox) {
                throw ("literal inner join collapse: host strategic +14s without " +
                    "join DLG_ISO_PAL (injectThrew=$($joinCollapseEvidence.injectThrew) " +
                    "stuckMessageBox=$($joinCollapseEvidence.stuckMessageBox))")
            }
            # The old +14-second heuristic is useful evidence, but elapsed time
            # alone cannot prove a collapsed live client. Keep observing without
            # another action until the existing fixed post-role deadline.
            Legacy-Log ('host strategic +14s without join DLG_ISO_PAL, but no ' +
                'collapse sentinel; continuing passive observation')
            Trace-Step 'source-join-sync-budget-exceeded' @{
                budgetSeconds = $sourceJoinSyncBudgetSeconds
                injectThrew = $false
                stuckMessageBox = $false
            }
        }
    }
    if (-not $hostStrategic -or -not $joinStrategic) {
        throw ("literal inner FAIL: strategic not reached within the fixed " +
            "$TotalBudgetSec-second post-role budget")
    }
    Legacy-Log "strategic reached BOTH (T+$(ElapsedSeconds)s)"

    $strategicReadyUtc = [DateTime]::UtcNow
    $State.Phase = 'post-strategic-settle'
    Start-Sleep -Seconds 2
    Trace-Step 'post-strategic-sleep-2000-complete'
    if ([bool]$State.StopRequested) { return }

    # Old STATIC install checks were three ordered host reads followed by an
    # optional fourth host read used only to print the first three boot faults.
    # Missing install markers and legacy VEH/UEF entries were soft observations
    # in the green source run; keep them soft here.  New MSS exact-install and
    # production-fault rules are accumulated separately and applied only after
    # the source Snap("before") observation has completed.
    $hostLog = [string]$State.HostLog
    $joinLog = [string]$State.JoinLog
    Legacy-Log ''
    Legacy-Log '=== STATIC install checks (patch on at boot) ==='
    $legacyInstallLines = @(
        Read-CompleteLog $hostLog ([long]$State.HostLogBaseline))
    Trace-Step 'static-install-host-log-read'
    $sourceInstallMatches = @($legacyInstallLines | Where-Object {
        $_ -match '\[virtual-turn\]'
    })
    if ($sourceInstallMatches.Count -gt 0) {
        Legacy-Log ('  INSTALL  : ' + $sourceInstallMatches[0].Trim())
    } else {
        Legacy-Log '  WARNING: no [virtual-turn] install log (env not seen?)'
    }
    $legacyPatchLines = @(
        Read-CompleteLog $hostLog ([long]$State.HostLogBaseline))
    Trace-Step 'static-patch-host-log-read'
    $sourcePatchMatches = @($legacyPatchLines | Where-Object {
        $_ -match 'sub_4232E4 -> call sub_41E741 redirected'
    })
    if ($sourcePatchMatches.Count -gt 0) {
        Legacy-Log ('  PATCH    : ' + $sourcePatchMatches[0].Trim())
    } else {
        Legacy-Log ("  WARNING: no 'redirected' log " +
            '(patch_virtual_turn did not run / aborted on byte check)')
    }
    $legacyCrashLines = @(
        Read-CompleteLog $hostLog ([long]$State.HostLogBaseline))
    Trace-Step 'static-fault-host-log-read'
    $legacyRealFaults = @(Get-LegacyRealFaults $legacyCrashLines)
    $legacyPrintedBootFaults = @()
    if ($legacyRealFaults.Count -gt 0) {
        Legacy-Log ("  NOTE: $($legacyRealFaults.Count) real fault(s) during boot " +
            '(auto-nav noise unless in turn-advance path):')
        $legacyCrashPrintLines = @(
            Read-CompleteLog $hostLog ([long]$State.HostLogBaseline))
        Trace-Step 'static-fault-print-host-log-read'
        $legacyPrintedBootFaults = @(
            Get-LegacyRealFaults $legacyCrashPrintLines | Select-Object -First 3)
        foreach ($faultLine in $legacyPrintedBootFaults) {
            Legacy-Log ('    ' + $faultLine.Trim())
        }
    } else {
        Legacy-Log '  host reached strategic with patch ON, no real faults (static install SAFE)'
    }

    # This one join read is an explicitly new MSS pre-release identity/install
    # check.  It is not represented as one of the source STATIC reads.
    $mssJoinPreflightLines = @(
        Read-CompleteLog $joinLog ([long]$State.JoinLogBaseline))
    Trace-Step 'mss-preflight-join-log-read'
    $hostBundleCount = Count-Marker $legacyInstallLines `
        '[simturns] both-role production bundle prepared; no room activated'
    $joinBundleCount = Count-Marker $mssJoinPreflightLines `
        '[simturns] both-role production bundle prepared; no room activated'
    # Preparation now installs both roles; the actual per-session role is bound
    # before native client creation, still before this withheld SessionPlan.
    $hostArmedCount = Count-Marker $legacyInstallLines `
        '[simturns] room armed before native startup (role=host)'
    $joinArmedCount = Count-Marker $mssJoinPreflightLines `
        '[simturns] room armed before native startup (role=join)'
    $interceptionMarker =
        '[simturns] engine interception installed; coordinator start deferred to strategic phase'
    $hostInterceptionCount = Count-Marker $legacyPatchLines $interceptionMarker
    $joinInterceptionCount = Count-Marker $mssJoinPreflightLines $interceptionMarker
    $productionFaultsBeforeRelease = @(
        Get-ProductionFaults $legacyCrashLines
        Get-ProductionFaults $mssJoinPreflightLines
    )
    $strictPreReleaseFailures = [System.Collections.Generic.List[string]]::new()
    if ($hostBundleCount -ne 1 -or $joinBundleCount -ne 1 -or
        $hostArmedCount -ne 1 -or $joinArmedCount -ne 1 -or
        $hostInterceptionCount -ne 1 -or $joinInterceptionCount -ne 1) {
        $strictPreReleaseFailures.Add(
            "exact MSS install markers bundle=$hostBundleCount/$joinBundleCount " +
            "armed=$hostArmedCount/$joinArmedCount " +
            "interception=$hostInterceptionCount/$joinInterceptionCount")
    }
    foreach ($faultLine in $productionFaultsBeforeRelease) {
        $strictPreReleaseFailures.Add("production fault before release: $faultLine")
    }
    $mssStaticProjection = [pscustomobject]@{
        sourceInstallReadOrdinal = 1
        sourcePatchReadOrdinal = 2
        sourceFaultReadOrdinal = 3
        sourceFaultPrintReadPerformed = ($legacyRealFaults.Count -gt 0)
        sourceInstallMatches = $sourceInstallMatches
        sourcePatchMatches = $sourcePatchMatches
        bundlePrepared = Count-Marker $legacyInstallLines `
            '[simturns] both-role production bundle prepared; no room activated'
        hostRoleArmed = $hostArmedCount
        joinRoleArmed = $joinArmedCount
        engineInterceptionInstalled = Count-Marker $legacyPatchLines `
            '[simturns] engine interception installed; coordinator start deferred to strategic phase'
        hostRealFaults = $legacyRealFaults
        hostPrintedBootFaults = $legacyPrintedBootFaults
        mapping = [ordered]@{
            sourceInstallMarker = '[virtual-turn]'
            sourcePatchMarker = 'sub_4232E4 -> call sub_41E741 redirected'
            mssInstallMarker = '[simturns] both-role production bundle prepared'
            mssRoleMarker = '[simturns] room armed before native startup'
            mssPatchMarker = '[simturns] engine interception installed'
        }
    }
    $staticPath = Join-Path $ArtifactDir 'legacy-inner-static-install.txt'
    Write-NewUtf8File $staticPath `
        (($legacyTranscript -join "`r`n") + "`r`n")
    Trace-Step 'static-install-output-written'

    # Snap("before"): one state read, timestamp fixed immediately afterwards,
    # then seven ordered host observations and seven ordered join observations.
    Legacy-Log "SNAPSHOT 'before'"
    try {
        $snapshotState = Invoke-JsonGet '/api/state' 6
    } catch {
        $snapshotState = [pscustomobject]@{
            observationUnavailable = $true
            observationLabel = 'snapshot aggregate state'
            observationError = [string]$_.Exception.Message
            terminalFault = $null
            roles = $null
        }
        $strictPreReleaseFailures.Add(
            "snapshot aggregate state failed: $([string]$_.Exception.Message)")
    }
    # audit_state.ps1 serialized (Get-Date).ToString('o'): preserve the local
    # offset in the legacy product. UTC projections, where useful, are derived
    # from this same instant and remain MSS-only evidence.
    $snapshotCapturedAt = [DateTimeOffset]::Now
    Trace-Step 'snapshot-state-read-and-timestamp-fixed'
    if ($null -ne (Property $snapshotState 'terminalFault')) {
        $strictPreReleaseFailures.Add(
            'snapshot aggregate state reported terminalFault: ' +
            ((Property $snapshotState 'terminalFault') | ConvertTo-Json -Compress))
    }
    try {
        $identity = Assert-RelayIdentity $snapshotState $hostPid $joinPid
    } catch {
        $identity = [pscustomobject]@{
            observationUnavailable = $true
            observationError = [string]$_.Exception.Message
        }
        $strictPreReleaseFailures.Add(
            "snapshot relay identity failed: $([string]$_.Exception.Message)")
    }
    $hostPeerSnapshot = Get-PeerSnapshotProjection `
        'host' $hostLog ([long]$State.HostLogBaseline) $strictPreReleaseFailures
    $joinPeerSnapshot = Get-PeerSnapshotProjection `
        'join' $joinLog ([long]$State.JoinLogBaseline) $strictPreReleaseFailures

    # UI history and sim-coordinator history are additional MSS evidence. They
    # follow the complete legacy host-then-join projection and do not replace a
    # source read slot.
    $hostUiHistory = Invoke-StrictObservationOnce $strictPreReleaseFailures `
        'host UI history evidence' 'mss-evidence-host-ui-history-read' `
        { Invoke-JsonGet '/api/ui/history?role=host&after=0' 6 } `
        -CheckTerminalFault
    $joinUiHistory = Invoke-StrictObservationOnce $strictPreReleaseFailures `
        'join UI history evidence' 'mss-evidence-join-ui-history-read' `
        { Invoke-JsonGet '/api/ui/history?role=join&after=0' 6 } `
        -CheckTerminalFault
    # Reporter readiness is deterministic. Validate the world shape before the
    # sole release mutation, then preserve the old later snapshot reopen and
    # repeat this projection from the serialized artifact.
    try {
        $preReleaseJoinProjection = Get-JoinMovementProjection `
            $joinPeerSnapshot.refreshStacks 'pre-release join world'
    } catch {
        $preReleaseJoinProjection = [pscustomobject]@{
            observationUnavailable = $true
            observationError = [string]$_.Exception.Message
            selfId = ''
            records = @()
            movement = @()
        }
        $strictPreReleaseFailures.Add(
            "pre-release join movement failed: $([string]$_.Exception.Message)")
    }
    $simEventsBeforeRelease = @(Invoke-StrictObservationOnce `
        $strictPreReleaseFailures 'pre-release sim-event history' `
        'mss-evidence-sim-events-read' { Read-SimEvents })
    try {
        Assert-NoSimRelayFault $simEventsBeforeRelease
    } catch {
        $strictPreReleaseFailures.Add(
            "pre-release sim-event gate failed: $([string]$_.Exception.Message)")
    }
    $pendingEvent = $null
    $pendingCount = 0
    try {
        $pendingEvent = Get-OnlyEvent $simEventsBeforeRelease `
            'bootstrap-release-pending'
        $pendingCount = 1
    } catch {
        $strictPreReleaseFailures.Add(
            "pre-release pending event failed: $([string]$_.Exception.Message)")
    }
    $earlyReleaseEvents = @($simEventsBeforeRelease | Where-Object {
        [string](Property $_ 'event') -in @(
            'bootstrap-release-file-observed',
            'bootstrap-release-granted',
            'session-plan-created',
            'session-plan-delivered',
            'session-activated',
            'bootstrap-begin-turn-applied',
            'bootstrap-cascade-delay-armed',
            'engine-action-dispatched')
    })
    if ($earlyReleaseEvents.Count -ne 0) {
        $strictPreReleaseFailures.Add(
            "literal inner bootstrap gate boundary is not exact " +
            "(pending=$pendingCount earlyRelease=$($earlyReleaseEvents.Count))")
    }
    $expectedHostHandle = [string](Property $pendingEvent 'hostHandle')
    $expectedJoinHandle = [string](Property $pendingEvent 'joinHandle')
    if ([string]::IsNullOrWhiteSpace($expectedHostHandle) -or
        [string]::IsNullOrWhiteSpace($expectedJoinHandle) -or
        $expectedHostHandle -eq $expectedJoinHandle) {
        $strictPreReleaseFailures.Add(
            'literal inner bootstrap pending event omitted two distinct exact handles')
    }
    $preReleaseEventCount = $simEventsBeforeRelease.Count

    try {
        $hostBeforeProcess = Get-ExactProcessSnapshot `
            $State.HostProcess $hostPid 'host' $State.HostExecutablePath
    } catch {
        $hostBeforeProcess = [pscustomobject]@{
            role = 'host'; pid = $hostPid; observationUnavailable = $true
            observationError = [string]$_.Exception.Message
        }
        $strictPreReleaseFailures.Add(
            "pre-release host process failed: $([string]$_.Exception.Message)")
    }
    try {
        $joinBeforeProcess = Get-ExactProcessSnapshot `
            $State.JoinProcess $joinPid 'join' $State.JoinExecutablePath
    } catch {
        $joinBeforeProcess = [pscustomobject]@{
            role = 'join'; pid = $joinPid; observationUnavailable = $true
            observationError = [string]$_.Exception.Message
        }
        $strictPreReleaseFailures.Add(
            "pre-release join process failed: $([string]$_.Exception.Message)")
    }

    $beforePath = Join-Path $ArtifactDir 'legacy-inner-before.json'
    $before = [ordered]@{
        oracle = 'test_R_virtual_turn.ps1@3f683e406d6808e194dc2ac2521abf3eaa443955'
        capturedAt = $snapshotCapturedAt.ToString('O')
        armUtc = $ArmUtc.ToString('O')
        rolesReadyUtc = $rolesReadyUtc.ToString('O')
        strategicReadyUtc = $strategicReadyUtc.ToString('O')
        rolePollMilliseconds = 500
        roleSamples = $roleSamples
        strategicPollMilliseconds = 400
        strategicSamples = $strategicSamples
        hostStrategicAtSeconds = $hostStrategicAtSeconds
        sourceJoinSyncBudgetSeconds = $sourceJoinSyncBudgetSeconds
        sourceJoinSyncBudgetExceeded = $sourceJoinSyncBudgetExceeded
        joinCollapseEvidence = $joinCollapseEvidence
        postStrategicSettleMilliseconds = 2000
        bootstrapReleaseFile = $BootstrapReleaseFile
        processes = [ordered]@{
            host = $hostBeforeProcess
            join = $joinBeforeProcess
        }
        legacyObservationProjection = [ordered]@{
            sourceGreenSnapshotSha256 = '8BA71023479CCF0779C3FAC2128513D19D6AE83C2A20CFBBDC2B6E3821EA3961'
            state = $snapshotState
            timestampFixedImmediatelyAfterState = $snapshotCapturedAt.ToString('O')
            peerOrder = @('host', 'join')
            perPeerReadCount = 7
            peers = [ordered]@{
                host = $hostPeerSnapshot
                join = $joinPeerSnapshot
            }
            sourceFallbackUsed = $false
            sourceCompatibilityProducts = [ordered]@{
                peerInfoSource = 'api'
                dialogsSeen = $null
                dialogsSeenReason = 'source serialized .dialogs although Classify-UIMode returned .all_dialogs'
                ownedStacks = [ordered]@{ host = @(); joiner = @() }
                ownedStacksReason = 'source filtered dynamic A3DE owners through hard-coded 0x805E0001/2'
                verdictNote = 'no derived booleans (sim_turns/day_flip/income_added). Use raw fields and verify chain manually.'
                unavailableWireProducts = @(
                    'DPID and DPID decimal',
                    'CCmdTurnInfo PREV/NEW raw strings and active handle',
                    'raw BeginTurn offsets and legacy signed/broadcast decoder result',
                    'last-five stack moves/path/direction/waypoints',
                    'CRefresh player raw names/length/repeated handles',
                    'CMidStack handle2/leader/morale/facing/inside/subrace wire fields',
                    'state_dump_latest'
                )
            }
            deliberateMssDifference = @(
                'source Snap launched a synchronous powershell child; this removable observer executes the same ordered reads in its already-owned worker',
                'source log-scan fallback is intentionally absent; reporter failure is terminal',
                'wire-only legacy fields are recorded as unavailable in each mapped read'
            )
        }
        mssEvidence = [ordered]@{
            relayIdentity = $identity
            uiHistory = [ordered]@{ host = $hostUiHistory; join = $joinUiHistory }
            validatedJoinMovement = $preReleaseJoinProjection
            simEvents = $simEventsBeforeRelease
            dumpBaseline = [ordered]@{
                capturedAt = ([DateTimeOffset]$DumpBaselineCapturedUtc).ToString('O')
                capturedBeforeArm = ([DateTimeOffset]$DumpBaselineCapturedUtc -le
                    [DateTimeOffset]$ArmUtc)
                roots = $DumpRoots
                files = $InitialDumps
            }
        }
        logs = [ordered]@{
            hostPath = $hostLog
            joinPath = $joinLog
            hostBaseline = [long]$State.HostLogBaseline
            joinBaseline = [long]$State.JoinLogBaseline
            hostStaticLines = $legacyCrashLines
            joinMssPreflightLines = $mssJoinPreflightLines
        }
        sourceStaticStepProjectedToMss = $mssStaticProjection
        strictMssAssertions = [ordered]@{
            category = 'new MSS safety gate; not a legacy green criterion'
            hostBundleCount = $hostBundleCount
            joinBundleCount = $joinBundleCount
            hostInterceptionCount = $hostInterceptionCount
            joinInterceptionCount = $joinInterceptionCount
            productionFaults = $productionFaultsBeforeRelease
            failures = @($strictPreReleaseFailures)
            passed = ($strictPreReleaseFailures.Count -eq 0)
        }
    }
    Write-NewJson $beforePath $before
    Trace-Step 'before-snapshot-written'
    if ($strictPreReleaseFailures.Count -ne 0) {
        Trace-Step 'strict-pre-release-gate-failed' @{
            failureCount = $strictPreReleaseFailures.Count
        }
        $preReleaseTracePath = Join-Path $ArtifactDir `
            'legacy-inner-execution-trace.json'
        Write-NewJson $preReleaseTracePath @($executionTrace)
        throw ("strict MSS pre-release safety gate failed: " +
            ($strictPreReleaseFailures -join '; '))
    }

    Legacy-Log ''
    Legacy-Log '=== START joiner day-1 turn (activate + single income, real handle) ==='
    Trace-Step 'legacy-start-heading-written'

    # Sole gameplay/bootstrap mutation. FileMode.CreateNew makes a second
    # release impossible and visible as a terminal test error. The watcher
    # observation below is the causal creation edge; arm/write-complete times
    # only bound the writer operation and do not pretend to be that edge.
    $State.Phase = 'release-bootstrap'
    $releaseArmUtc = [DateTimeOffset]::UtcNow
    Trace-Step 'bootstrap-release-writer-armed'
    Write-NewUtf8File $BootstrapReleaseFile "release`n"
    $releaseWriteCompletedUtc = [DateTimeOffset]::UtcNow
    Trace-Step 'bootstrap-release-writer-complete'

    $eventsAfterRelease = @()
    $beginAppliedObserved = $null
    $delayArmedEvent = $null
    $phaseOneDeadline = $releaseArmUtc.AddSeconds(8)
    while ([DateTimeOffset]::UtcNow -lt $phaseOneDeadline) {
        if ([bool]$State.StopRequested) { return }
        $eventsAfterRelease = @(Read-SimEvents)
        Assert-NoSimRelayFault $eventsAfterRelease
        if ($eventsAfterRelease.Count -lt $preReleaseEventCount) {
            throw 'literal inner sim-relay JSONL history shrank after bootstrap release'
        }
        $beginMatches = @($eventsAfterRelease | Where-Object {
            [string](Property $_ 'event') -eq 'bootstrap-begin-turn-applied' -and
            [string](Property $_ 'role') -eq 'join' -and
            [long](Property $_ 'day') -eq 1
        })
        if ($beginMatches.Count -gt 1) {
            throw 'literal inner bootstrap BeginTurn was applied more than once'
        }
        if ($beginMatches.Count -eq 1 -and $null -eq $beginAppliedObserved) {
            $beginAppliedObserved = $beginMatches[0]
            Trace-Step 'bootstrap-begin-turn-applied-observed'
            Legacy-Log (
                "  joiner activate (day 1): mapped to the sole SessionPlan/" +
                "bootstrap BeginTurn for $expectedJoinHandle")
            Trace-Step 'legacy-activation-result-written'
        }
        $delayMatches = @($eventsAfterRelease | Where-Object {
            [string](Property $_ 'event') -eq 'bootstrap-cascade-delay-armed'
        })
        if ($delayMatches.Count -gt 1) {
            throw 'literal inner bootstrap cascade delay was armed more than once'
        }
        if ($delayMatches.Count -eq 1) {
            $delayArmedEvent = $delayMatches[0]
            if ($null -ne $beginAppliedObserved) { break }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $delayArmedEvent) {
        throw 'literal inner bootstrap release did not arm its sole cascade within 8 seconds'
    }
    if ($null -eq $beginAppliedObserved) {
        throw 'literal inner bootstrap cascade delay preceded its BeginTurn-applied anchor'
    }
    Trace-Step 'bootstrap-cascade-delay-armed-observed'

    $delayArmedUtc = Parse-EventTimestamp `
        $delayArmedEvent 'bootstrap-cascade-delay-armed'
    $engineActionEvent = $null
    $phaseTwoDeadline = $delayArmedUtc.AddSeconds(8)
    while ([DateTimeOffset]::UtcNow -lt $phaseTwoDeadline) {
        if ([bool]$State.StopRequested) { return }
        $eventsAfterRelease = @(Read-SimEvents)
        Assert-NoSimRelayFault $eventsAfterRelease
        $engineActionMatches = @($eventsAfterRelease | Where-Object {
            [string](Property $_ 'event') -eq 'engine-action-dispatched' -and
            [string](Property $_ 'stage') -eq 'bootstrap-apply'
        })
        if ($engineActionMatches.Count -gt 1) {
            throw 'literal inner bootstrap EngineAction was dispatched more than once'
        }
        if ($engineActionMatches.Count -eq 1) {
            $engineActionEvent = $engineActionMatches[0]
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $engineActionEvent) {
        throw 'literal inner bootstrap EngineAction was not dispatched within its fixed 8-second call budget'
    }
    Trace-Step 'engine-action-dispatched-observed'
    Legacy-Log ("  joiner income (day 1, cascade once): mapped to actionId=" +
        "$([long](Property $engineActionEvent 'actionId'))")
    Trace-Step 'legacy-cascade-result-written'

    $observed = Get-OnlyEvent $eventsAfterRelease `
        'bootstrap-release-file-observed'
    $releaseGranted = Get-OnlyEvent $eventsAfterRelease `
        'bootstrap-release-granted'
    $sessionPlan = Get-OnlyEvent $eventsAfterRelease 'session-plan-created'
    $hostPlanDelivered = Get-OnlyEvent $eventsAfterRelease `
        'session-plan-delivered' 'host'
    $joinPlanDelivered = Get-OnlyEvent $eventsAfterRelease `
        'session-plan-delivered' 'join'
    $hostActivated = Get-OnlyEvent $eventsAfterRelease `
        'session-activated' 'host'
    $joinActivated = Get-OnlyEvent $eventsAfterRelease `
        'session-activated' 'join'
    $beginApplied = Get-OnlyEvent $eventsAfterRelease `
        'bootstrap-begin-turn-applied' 'join' ([long]1)
    $delayArmedEvent = Get-OnlyEvent $eventsAfterRelease `
        'bootstrap-cascade-delay-armed'
    $engineActionEvent = Get-OnlyEvent $eventsAfterRelease `
        'engine-action-dispatched'
    $requiredEventCounts = [ordered]@{
        'bootstrap-release-file-observed' = 1
        'bootstrap-release-granted' = 1
        'session-plan-created' = 1
        'session-plan-delivered' = 2
        'session-activated' = 2
        'bootstrap-begin-turn-applied' = 1
        'bootstrap-cascade-delay-armed' = 1
        'engine-action-dispatched' = 1
    }

    $observedPath = [IO.Path]::GetFullPath([string](Property $observed 'path'))
    if (-not [string]::Equals(
            $observedPath, [IO.Path]::GetFullPath($BootstrapReleaseFile),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "literal inner release watcher observed '$observedPath', not the exact release file"
    }
    foreach ($handleEvent in @($releaseGranted, $sessionPlan)) {
        if ([string](Property $handleEvent 'hostHandle') -ne $expectedHostHandle -or
            [string](Property $handleEvent 'joinHandle') -ne $expectedJoinHandle) {
            throw 'literal inner bootstrap release changed one of the pending exact handles'
        }
    }
    $hostLease = [long](Property $sessionPlan 'hostLease')
    $joinLease = [long](Property $sessionPlan 'joinLease')
    $sessionEpoch = [long](Property $sessionPlan 'epoch')
    $sessionMergeDayValue = Property $sessionPlan 'mergeDay'
    if ($null -eq $sessionMergeDayValue) {
        throw 'literal inner SessionPlan omitted its authoritative mergeDay'
    }
    try { [long]$sessionMergeDay = $sessionMergeDayValue }
    catch { throw "literal inner SessionPlan mergeDay is not an integer: '$sessionMergeDayValue'" }
    if ($hostLease -le 0 -or $joinLease -le 0 -or $hostLease -eq $joinLease -or
        $sessionEpoch -le 0 -or
        $sessionMergeDay -lt 0 -or $sessionMergeDay -gt [uint32]::MaxValue -or
        [long](Property $hostPlanDelivered 'epoch') -ne $sessionEpoch -or
        [long](Property $joinPlanDelivered 'epoch') -ne $sessionEpoch) {
        throw ('literal inner SessionPlan omitted its exact epoch/merge day, ' +
            'staged deliveries, or distinct leases')
    }
    if ([string](Property $beginApplied 'handle') -ne $expectedJoinHandle -or
        [long](Property $beginApplied 'day') -ne 1 -or
        [long](Property $delayArmedEvent 'delayMs') -ne 500 -or
        [string](Property $delayArmedEvent 'anchor') -ne
            'bootstrap-begin-turn-applied' -or
        [long](Property $engineActionEvent 'actionId') -ne 1 -or
        [long](Property $engineActionEvent 'kind') -ne 1 -or
        [string](Property $engineActionEvent 'stage') -ne 'bootstrap-apply' -or
        [string](Property $engineActionEvent 'recipient') -ne 'host' -or
        [string](Property $engineActionEvent 'playerHandle') -ne $expectedJoinHandle -or
        [long](Property $engineActionEvent 'day') -ne 1 -or
        [long](Property $engineActionEvent 'lease') -ne $joinLease) {
        throw 'literal inner bootstrap fields do not identify the sole join/day-1/500ms/action-1 EngineAction'
    }

    $observedIndex = Get-EventRecordIndex $eventsAfterRelease $observed
    $grantedIndex = Get-EventRecordIndex $eventsAfterRelease $releaseGranted
    $hostPlanDeliveredIndex = Get-EventRecordIndex `
        $eventsAfterRelease $hostPlanDelivered
    $planIndex = Get-EventRecordIndex $eventsAfterRelease $sessionPlan
    $hostActivatedIndex = Get-EventRecordIndex $eventsAfterRelease $hostActivated
    $joinPlanDeliveredIndex = Get-EventRecordIndex `
        $eventsAfterRelease $joinPlanDelivered
    $joinActivatedIndex = Get-EventRecordIndex $eventsAfterRelease $joinActivated
    $beginIndex = Get-EventRecordIndex $eventsAfterRelease $beginApplied
    $delayIndex = Get-EventRecordIndex $eventsAfterRelease $delayArmedEvent
    $engineActionIndex = Get-EventRecordIndex $eventsAfterRelease $engineActionEvent
    if ($observedIndex -lt $preReleaseEventCount -or
        $grantedIndex -le $observedIndex -or
        $hostPlanDeliveredIndex -le $grantedIndex -or
        $planIndex -le $hostPlanDeliveredIndex -or
        $hostActivatedIndex -le $planIndex -or
        $joinPlanDeliveredIndex -le $hostActivatedIndex -or
        $joinActivatedIndex -le $joinPlanDeliveredIndex -or
        $beginIndex -le $joinActivatedIndex -or
        $delayIndex -le $beginIndex -or
        $engineActionIndex -le $hostActivatedIndex -or
        $engineActionIndex -le $delayIndex) {
        throw 'literal inner bootstrap events are missing their exact causal order'
    }

    $beginAppliedUtc = Parse-EventTimestamp `
        $beginApplied 'bootstrap-begin-turn-applied'
    $sessionPlanUtc = Parse-EventTimestamp $sessionPlan 'session-plan-created'
    $releaseObservedUtc = Parse-EventTimestamp `
        $observed 'bootstrap-release-file-observed'
    $delayArmedUtc = Parse-EventTimestamp `
        $delayArmedEvent 'bootstrap-cascade-delay-armed'
    $engineActionUtc = Parse-EventTimestamp `
        $engineActionEvent 'engine-action-dispatched'
    $beginToDelayArmedMs = ($delayArmedUtc - $beginAppliedUtc).TotalMilliseconds
    $beginToEngineActionMs = ($engineActionUtc - $beginAppliedUtc).TotalMilliseconds
    $planToDelayArmedMs = ($delayArmedUtc - $sessionPlanUtc).TotalMilliseconds
    $planToEngineActionMs = ($engineActionUtc - $sessionPlanUtc).TotalMilliseconds
    $delayArmedToEngineActionMs = ($engineActionUtc - $delayArmedUtc).TotalMilliseconds
    # The coordinator creates the 500 ms timer after publishing BeginTurnApplied
    # but before publishing delay-armed. Logger work between those two edges may
    # legally make armed-to-action shorter than 500 ms. Keep that interval as
    # nonnegative telemetry; the causal timer guarantee is begin-to-action.
    if ($beginToEngineActionMs -lt 500 -or $delayArmedToEngineActionMs -lt 0) {
        throw ("literal inner BeginTurn-applied EngineAction delay was only " +
            "$beginToEngineActionMs ms (armed-to-action=$delayArmedToEngineActionMs ms " +
            "begin=$($beginAppliedUtc.ToString('O')) " +
            "action=$($engineActionUtc.ToString('O')))")
    }

    # BootstrapBeginTurnApplied is the first typed completion edge for the old
    # activation POST.  The coordinator arms its sole +500 timer there and
    # never rearms/catches up. The v8 EngineAction dispatch is the old cascade
    # call boundary and is followed by exactly +800; anchor that pause there.
    $baselineTarget = $engineActionUtc.AddMilliseconds(800)
    while ([DateTimeOffset]::UtcNow -lt $baselineTarget) {
        if ([bool]$State.StopRequested) { return }
        $remaining = [Math]::Ceiling(
            ($baselineTarget - [DateTimeOffset]::UtcNow).TotalMilliseconds)
        if ($remaining -gt 0) {
            Start-Sleep -Milliseconds ([Math]::Min(50, $remaining))
        }
    }
    Trace-Step 'post-cascade-sleep-800-complete'
    $eventsAtTailBaseline = @(Read-SimEvents)
    Assert-NoSimRelayFault $eventsAtTailBaseline
    foreach ($eventName in $requiredEventCounts.Keys) {
        $expectedCount = [int]$requiredEventCounts[$eventName]
        $actualCount = @($eventsAtTailBaseline | Where-Object {
            [string](Property $_ 'event') -eq $eventName
        }).Count
        if ($actualCount -ne $expectedCount) {
            throw "literal inner event '$eventName' changed to count=$actualCount before the first TX census"
        }
    }

    $State.Phase = 'legacy-tail'
    $firstTx = Get-HostAdvanceCounts 'first-census'
    Legacy-Log ("  HOST advance baseline: TX BeginTurn=$($firstTx.beginTurnTx)  " +
        "TX EndTurn=$($firstTx.endTurnTx)")

    # The source printed this checkpoint even under -KeepAlive; the wrapper
    # redirected it into run_test_boot.log. Preserve the step and exact content
    # as an immutable artifact without claiming that a user was shown a prompt.
    $manualCheckpointPath = Join-Path $ArtifactDir `
        'legacy-inner-manual-checkpoint.txt'
    $manualCheckpointLines = @(
        '============================================================',
        '  >>> MANUAL STEP <<<',
        '  In the JOINER window: click End Turn (skip the turn).',
        '  Then look at the HOST window:',
        "    EXPECT (fix OK): host stays on ITS turn, Day 1, NO 'waiting for opponent'.",
        '    BUG (fix failed): host shows ''opponent''s turn'' / waiting, or day changes.',
        '  Leave both running; this script will re-check host packet logs below.',
        '============================================================'
    )
    Write-NewUtf8File $manualCheckpointPath `
        (($manualCheckpointLines -join "`r`n") + "`r`n")
    Trace-Step 'manual-checkpoint-output-written'
    Legacy-Log ''
    foreach ($manualLine in $manualCheckpointLines) { Legacy-Log $manualLine }

    # SubTurns=0 still prints the section header; there are deliberately no
    # cascade/begin actions and therefore no hidden retries or fallback calls.
    $subturnCyclePath = Join-Path $ArtifactDir 'legacy-inner-subturn-cycle.txt'
    $subturnCycleLines = @(
        '=== joiner sub-turn cycle (cascade + begin-turn inject, day=subTurn) ==='
    )
    Write-NewUtf8File $subturnCyclePath `
        (($subturnCycleLines -join "`r`n") + "`r`n")
    Trace-Step 'subturn-cycle-header-written'
    Legacy-Log ''
    foreach ($subturnLine in $subturnCycleLines) { Legacy-Log $subturnLine }

    # Show-StackMp("before") reopened the saved snapshot. Preserve that read,
    # followed by the expected missing subturn_1 lookup for SubTurns=0.
    $savedBefore = Get-Content -LiteralPath $beforePath -Raw |
        ConvertFrom-Json -ErrorAction Stop
    Trace-Step 'before-snapshot-reopened'
    $savedJoinProjection = Get-JoinMovementProjection `
        $savedBefore.legacyObservationProjection.peers.join.refreshStacks `
        'reopened before snapshot'
    $beforeMovementRecords = @($savedJoinProjection.records)
    $beforeMovement = @($savedJoinProjection.movement)
    $preReleasePairs = @($preReleaseJoinProjection.records | ForEach-Object {
        "$([string]$_.id)=$([int]$_.movement)"
    })
    $savedPairs = @($beforeMovementRecords | ForEach-Object {
        "$([string]$_.id)=$([int]$_.movement)"
    })
    if ($preReleasePairs.Count -ne $savedPairs.Count) {
        throw 'literal inner reopened before snapshot changed join-owned stack count'
    }
    for ($pairIndex = 0; $pairIndex -lt $preReleasePairs.Count; $pairIndex++) {
        if (-not [string]::Equals(
                $preReleasePairs[$pairIndex], $savedPairs[$pairIndex],
                [StringComparison]::Ordinal)) {
            throw 'literal inner reopened before snapshot changed join movement evidence'
        }
    }
    $subturnOnePath = Join-Path $ArtifactDir 'legacy-inner-subturn-1.json'
    $subturnOneExists = Test-Path -LiteralPath $subturnOnePath -PathType Leaf
    Trace-Step 'subturn-1-missing-probe'
    if ($subturnOneExists) {
        throw 'literal inner SubTurns=0 unexpectedly produced a subturn_1 snapshot'
    }

    $mpRefillPath = Join-Path $ArtifactDir 'legacy-inner-mp-refill.txt'
    $mpRefillLines = @(
        '=== cascade MP-refill sanity ===',
        '  [before      ] stack= mp_display= mp_byte_91=',
        '  subturn_1 : MISSING'
    )
    Write-NewUtf8File $mpRefillPath (($mpRefillLines -join "`r`n") + "`r`n")
    Trace-Step 'mp-refill-output-written'
    Legacy-Log ''
    foreach ($mpLine in $mpRefillLines) { Legacy-Log $mpLine }

    # KeepAlive + SubTurns=0 immediately performed the second host TX census,
    # second real-fault scan, dump enumeration, and liveness observation.
    $secondTx = Get-HostAdvanceCounts 'second-census'
    $finalHostLines = @(
        Read-CompleteLog $hostLog ([long]$State.HostLogBaseline))
    Trace-Step 'final-host-fault-log-read'
    $finalRealFaults = @(Get-LegacyRealFaults $finalHostLines)
    $finalJoinLines = @(
        Read-CompleteLog $joinLog ([long]$State.JoinLogBaseline))
    Trace-Step 'mss-final-join-fault-log-read'
    $finalProductionFaults = @(
        Get-ProductionFaults $finalHostLines
        Get-ProductionFaults $finalJoinLines
    )
    $newDumps = @(Get-ChangedDumps $InitialDumps)
    Trace-Step 'final-dump-delta-read'
    $hostFinalProcess = Get-ExactProcessSnapshot `
        $State.HostProcess $hostPid 'host' $State.HostExecutablePath
    $joinFinalProcess = Get-ExactProcessSnapshot `
        $State.JoinProcess $joinPid 'join' $State.JoinExecutablePath
    Trace-Step 'final-exact-process-liveness-read'

    # Preserve the old soft HOST invariant output first.  The stricter MSS gate
    # is evaluated and recorded separately below.
    $hostInvariantLines = [System.Collections.Generic.List[string]]::new()
    $hostInvariantLines.Add('=== HOST invariant (whole run) ===')
    $hostInvariantLines.Add(
        "  HOST TX CCmdBeginTurnMsg total: $($secondTx.beginTurnTx)")
    $hostInvariantLines.Add(
        "  HOST TX CCmdEndTurnMsg   total: $($secondTx.endTurnTx)  " +
        '(want 0 -> host never drove global advance)')
    if ($finalRealFaults.Count -gt 0) {
        $hostInvariantLines.Add("  real fault(s): $($finalRealFaults.Count)")
        foreach ($faultLine in @($finalRealFaults | Select-Object -First 5)) {
            $hostInvariantLines.Add('    ' + $faultLine.Trim())
        }
    } else {
        $hostInvariantLines.Add('  no real faults (good)')
    }
    if ($newDumps.Count -gt 0) {
        $hostInvariantLines.Add("  dump files: $($newDumps.Count)")
        foreach ($dump in $newDumps) {
            $hostInvariantLines.Add('    ' + [IO.Path]::GetFileName([string]$dump.path))
        }
    }
    $hostInvariantPath = Join-Path $ArtifactDir 'legacy-inner-host-invariant.txt'
    Write-NewUtf8File $hostInvariantPath `
        (($hostInvariantLines -join "`r`n") + "`r`n")
    Trace-Step 'host-invariant-output-written'
    Legacy-Log ''
    foreach ($invariantLine in $hostInvariantLines) { Legacy-Log $invariantLine }

    $strictFinalFailures = [System.Collections.Generic.List[string]]::new()
    if ($secondTx.endTurnTx -ne 0) {
        $strictFinalFailures.Add(
            "host EndTurn TX count is $($secondTx.endTurnTx), expected zero")
    }
    if ($secondTx.beginTurnTx -ne $firstTx.beginTurnTx) {
        $strictFinalFailures.Add(
            "host BeginTurn TX changed $($firstTx.beginTurnTx) -> $($secondTx.beginTurnTx)")
    }
    foreach ($faultLine in $finalProductionFaults) {
        $strictFinalFailures.Add("production fault in final host/join scan: $faultLine")
    }
    if ($newDumps.Count -ne 0) {
        $strictFinalFailures.Add("new or changed dumps: $($newDumps.Count)")
    }

    # Preserve the source's post-invariant day-display probe and KeepAlive
    # announcement as projected console artifacts. They are documentation-only
    # reads/instructions, never gameplay actions.
    $dayProbePath = Join-Path $ArtifactDir 'legacy-inner-day-display-probe.txt'
    $dayProbeLines = @(
        '=== §29.5 day-display probe (do this manually while alive) ===',
        "  curl -Method POST 'http://127.0.0.1:8077/api/inject-begin-turn-joiner?player=0x805E0002&seq=7&auto=0'",
        "  curl -Method POST 'http://127.0.0.1:8077/api/inject-begin-turn-joiner?player=0x805E0002&seq=12&auto=0'",
        "  -> if the joiner's on-screen Day changes 7 then 12, the day is WIRE-sourced",
        '     (orchestrator just sets seq=subTurn). If not, day is LOCAL -> need banner hook.'
    )
    Write-NewUtf8File $dayProbePath (($dayProbeLines -join "`r`n") + "`r`n")
    Trace-Step 'legacy-day-probe-output-written'
    Legacy-Log ''
    foreach ($probeLine in $dayProbeLines) { Legacy-Log $probeLine }
    $keepAlivePath = Join-Path $ArtifactDir 'legacy-inner-keep-alive.txt'
    $keepAliveLines = @(
        ("=== KEEP-ALIVE: processes LEFT RUNNING (click End Turn on joiner; " +
         'run the probe above) ==='),
        "DONE (T+$(ElapsedSeconds)s) -- PROCESSES LEFT RUNNING"
    )
    Write-NewUtf8File $keepAlivePath (($keepAliveLines -join "`r`n") + "`r`n")
    Trace-Step 'keep-alive-and-done-output-written'
    foreach ($keepLine in $keepAliveLines) { Legacy-Log $keepLine }

    # The old KeepAlive tail and both of its TX/fault/dump/liveness censuses are
    # now complete. MSS additionally keeps strategic TX closed until the
    # coordinator has sent its final release and both native UI threads have
    # applied it. This is one separate passive deadline anchored at the sole
    # release write-completion edge; it cannot slide with popup/UI activity and
    # performs no game, UI, HTTP, or relay mutation.
    $State.Phase = 'operational-gate'
    $operationalMarker =
        '[simturns] bootstrap operational release applied; strict independent turns are operational'
    $sessionOperational = $null
    $bootstrapReleased = $null
    $hostOperationalMarkerCount = 0
    $joinOperationalMarkerCount = 0
    $operationalDeadline = $releaseWriteCompletedUtc.AddSeconds($TotalBudgetSec)
    while ([DateTimeOffset]::UtcNow -lt $operationalDeadline) {
        if ([bool]$State.StopRequested) { return }
        $eventsAtTailBaseline = @(Read-SimEvents)
        Assert-NoSimRelayFault $eventsAtTailBaseline
        $operationalMatches = @($eventsAtTailBaseline | Where-Object {
            [string](Property $_ 'event') -eq 'session-operational'
        })
        $releasedMatches = @($eventsAtTailBaseline | Where-Object {
            [string](Property $_ 'event') -eq 'bootstrap-released'
        })
        if ($operationalMatches.Count -gt 1 -or $releasedMatches.Count -gt 1) {
            throw 'literal inner bootstrap terminal release was published more than once'
        }
        $hostOperationalLines = @(
            Read-CompleteLog $hostLog ([long]$State.HostLogBaseline))
        $joinOperationalLines = @(
            Read-CompleteLog $joinLog ([long]$State.JoinLogBaseline))
        $operationalFaults = @(
            Get-ProductionFaults $hostOperationalLines
            Get-ProductionFaults $joinOperationalLines
        )
        if ($operationalFaults.Count -ne 0) {
            throw ('production fault before native operational release: ' +
                ($operationalFaults -join '; '))
        }
        $hostOperationalMarkerCount = @($hostOperationalLines |
            Select-String -SimpleMatch -Pattern $operationalMarker).Count
        $joinOperationalMarkerCount = @($joinOperationalLines |
            Select-String -SimpleMatch -Pattern $operationalMarker).Count
        if ($hostOperationalMarkerCount -gt 1 -or
            $joinOperationalMarkerCount -gt 1) {
            throw 'literal inner native bootstrap release marker was published more than once'
        }
        if ($operationalMatches.Count -eq 1 -and
            $releasedMatches.Count -eq 1 -and
            $hostOperationalMarkerCount -eq 1 -and
            $joinOperationalMarkerCount -eq 1) {
            $sessionOperational = $operationalMatches[0]
            $bootstrapReleased = $releasedMatches[0]
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $sessionOperational -or $null -eq $bootstrapReleased -or
        $hostOperationalMarkerCount -ne 1 -or
        $joinOperationalMarkerCount -ne 1) {
        throw ('literal inner startup budget ended before one relay release and ' +
            'both native UI-applied operational markers')
    }
    $operationalEpochValue = Property $sessionOperational 'epoch'
    $operationalMergeDayValue = Property $sessionOperational 'mergeDay'
    if ($null -eq $operationalEpochValue -or
        $null -eq $operationalMergeDayValue) {
        throw 'literal inner session-operational omitted its epoch or mergeDay'
    }
    try {
        [long]$operationalEpoch = $operationalEpochValue
        [long]$operationalMergeDay = $operationalMergeDayValue
    } catch {
        throw 'literal inner session-operational epoch or mergeDay is not an integer'
    }
    if ($operationalEpoch -ne $sessionEpoch -or
        $operationalMergeDay -ne $sessionMergeDay) {
        throw ('literal inner session-operational changed the negotiated ' +
            'SessionPlan epoch or merge day')
    }
    $releasedIndex = Get-EventRecordIndex $eventsAtTailBaseline $bootstrapReleased
    $operationalIndex = Get-EventRecordIndex $eventsAtTailBaseline $sessionOperational
    if ($releasedIndex -le $engineActionIndex -or
        $operationalIndex -le $releasedIndex) {
        throw 'literal inner terminal bootstrap release lost its causal order'
    }
    $operationalWitness = [pscustomobject]@{
        bootstrapReleased = $bootstrapReleased
        sessionOperational = $sessionOperational
        hostNativeMarkerCount = $hostOperationalMarkerCount
        joinNativeMarkerCount = $joinOperationalMarkerCount
        observedUtc = [DateTimeOffset]::UtcNow.ToString('O')
    }
    # Publish the immutable witness first and the synchronized latch last.
    $State.OperationalWitness = $operationalWitness
    $State.OperationalReady = $true
    Trace-Step 'bootstrap-native-operational-observed'

    $tailPath = Join-Path $ArtifactDir 'legacy-inner-tail.json'
    $tail = [ordered]@{
        oracle = 'test_R_virtual_turn.ps1 -KeepAlive -SubTurns 0'
        capturedAt = [DateTimeOffset]::UtcNow.ToString('O')
        beforeSnapshot = $beforePath
        release = [ordered]@{
            file = $BootstrapReleaseFile
            writerArmedUtc = $releaseArmUtc.ToString('O')
            writerCompletedUtc = $releaseWriteCompletedUtc.ToString('O')
            watcherObservedUtc = $releaseObservedUtc.ToString('O')
            eventCounts = $requiredEventCounts
            sessionActivated = [ordered]@{
                host = $hostActivated
                join = $joinActivated
            }
            beginTurnApplied = $beginApplied
            cascadeDelayArmed = $delayArmedEvent
            engineActionDispatched = $engineActionEvent
            beginAppliedToDelayArmedMilliseconds = $beginToDelayArmedMs
            beginAppliedToEngineActionMilliseconds = $beginToEngineActionMs
            sessionPlanCreatedToDelayArmedMilliseconds = $planToDelayArmedMs
            sessionPlanCreatedToEngineActionMilliseconds = $planToEngineActionMs
            cascadeDelayAnchor = 'bootstrap-begin-turn-applied'
            delayArmedToEngineActionMilliseconds = $delayArmedToEngineActionMs
            postCascadeBaselineMilliseconds = 800
            terminalOperational = $operationalWitness
        }
        hostTxBaseline = $firstTx
        manualCheckpoint = [ordered]@{
            path = $manualCheckpointPath
            projected = $true
            presentedToUser = $false
            sourceSink = 'run_test_boot.log redirection'
        }
        subTurns = 0
        subturnCycle = $subturnCyclePath
        mpRefillLegacyOutput = $mpRefillPath
        beforeJoinMovement = $beforeMovement
        beforeJoinStacks = $beforeMovementRecords
        subturnOneSnapshot = [ordered]@{
            path = $subturnOnePath
            exists = $subturnOneExists
            expectedMissing = $true
        }
        hostTxFinal = $secondTx
        finalRealFaults = $finalRealFaults
        finalProductionFaults = $finalProductionFaults
        newDumps = $newDumps
        processes = [ordered]@{
            host = $hostFinalProcess
            join = $joinFinalProcess
        }
        dayDisplayProbe = $dayProbePath
        dayDisplayProbeLegacyOnly = $true
        dayDisplayProbeSupportedMssEndpoint = $RelayBase
        keepAliveAnnouncement = $keepAlivePath
        keepAliveProjection = 'inner wrapper leaves exact-owned games to outer run_test continuation'
        legacyObservationProjection = [ordered]@{
            greenCriterion = 'source KeepAlive path recorded TX/fault/dump observations and still exited 0'
            legacyRealFaultsAreSoft = $true
            txCountsAreSoft = $true
            dumpsAreSoft = $true
        }
        strictMssAssertions = [ordered]@{
            category = 'new MSS safety gate; not a legacy green criterion'
            failures = @($strictFinalFailures)
            passed = ($strictFinalFailures.Count -eq 0)
        }
    }
    Write-NewJson $tailPath $tail
    Trace-Step 'tail-artifact-written'
    $transcriptPath = Join-Path $ArtifactDir 'legacy-inner-console-transcript.txt'
    Write-NewUtf8File $transcriptPath (($legacyTranscript -join "`r`n") + "`r`n")
    Trace-Step 'legacy-console-transcript-written'
    Trace-Step 'observer-complete'
    $executionTracePath = Join-Path $ArtifactDir 'legacy-inner-execution-trace.json'
    Write-NewJson $executionTracePath @($executionTrace)
    if ($strictFinalFailures.Count -ne 0) {
        throw ("strict MSS final safety gate failed: " +
            ($strictFinalFailures -join '; '))
    }
    $State.Result = [pscustomobject]@{
        BeforePath = $beforePath
        TailPath = $tailPath
        RoleSamples = $roleSamples
        StrategicSamples = $strategicSamples
        SourceJoinSyncBudgetExceeded = $sourceJoinSyncBudgetExceeded
        JoinCollapseEvidence = $joinCollapseEvidence
        ReleaseUtc = $releaseObservedUtc
        ReleaseWriterCompletedUtc = $releaseWriteCompletedUtc
        EngineActionUtc = $engineActionUtc
        OperationalWitness = $operationalWitness
        HostTxBaseline = $firstTx
        HostTxFinal = $secondTx
        BeforeJoinMovement = $beforeMovement
        SubturnOneSnapshotMissing = -not $subturnOneExists
        ExecutionTracePath = $executionTracePath
        ConsoleTranscriptPath = $transcriptPath
    }
    $State.Phase = 'complete'
    $State.Completed = $true
} catch {
    $failureMessage = [string]$_.Exception.Message
    $State.Error = $failureMessage
    $State.Phase = 'faulted'
    try {
        $traceFailurePath = Join-Path $ArtifactDir 'legacy-inner-execution-trace.json'
        if (-not (Test-Path -LiteralPath $traceFailurePath)) {
            Trace-Step 'observer-faulted' @{ message = $failureMessage }
            Write-NewJson $traceFailurePath @($executionTrace)
        }
        $transcriptFailurePath = Join-Path $ArtifactDir `
            'legacy-inner-console-transcript.txt'
        if (-not (Test-Path -LiteralPath $transcriptFailurePath)) {
            Legacy-Log ("FAIL: $failureMessage")
            Write-NewUtf8File $transcriptFailurePath `
                (($legacyTranscript -join "`r`n") + "`r`n")
        }
        $failureLine = if ($failureMessage -match '^FAIL:') {
            $failureMessage
        } else { "FAIL: literal inner observer: $failureMessage" }
        Write-NewUtf8File $FailureLogPath ($failureLine + "`r`n")
    } catch {
        $State.Error = "$failureMessage; failure artifact write also failed: $($_.Exception.Message)"
    }
}
'@
    try {
        [void]$worker.AddScript($workerSource)
        [void]$worker.AddArgument($state)
        [void]$worker.AddArgument($ArmUtc)
        [void]$worker.AddArgument([string]$RelayBase)
        [void]$worker.AddArgument([string]$SimRelayLog)
        [void]$worker.AddArgument([string]$BootstrapReleaseFile)
        [void]$worker.AddArgument([string]$FailureLogPath)
        [void]$worker.AddArgument([string]$ArtifactDir)
        [void]$worker.AddArgument([string]$GameDir)
        [void]$worker.AddArgument([IO.Path]::GetFullPath($ExpectedModulePath))
        [void]$worker.AddArgument([string[]]$normalizedDumpRoots)
        [void]$worker.AddArgument([object[]]$initialDumps)
        [void]$worker.AddArgument($baselineCapturedUtc)
        [void]$worker.AddArgument([int]$TotalBudgetSec)
        $async = $worker.BeginInvoke()
        return [pscustomobject]@{
            State = $state
            Worker = $worker
            Async = $async
            Disposed = $false
            BootstrapReleaseFile = $BootstrapReleaseFile
            ExpectedGameExe = $expectedGameExe
        }
    } catch {
        $worker.Dispose()
        throw
    }
}

function Publish-LiteralInnerStartupProcess {
    param(
        [Parameter(Mandatory)][object]$Observer,
        [Parameter(Mandatory)][ValidateSet('host', 'join')][string]$Role,
        [Parameter(Mandatory)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory)][string]$ExecutablePath,
        [Parameter(Mandatory)][string]$LogPath,
        [Parameter(Mandatory)][long]$LogBaseline
    )
    if ([bool]$Observer.Disposed) {
        throw "cannot publish $Role to a consumed literal inner observer"
    }
    if ($LogBaseline -ne 0) {
        throw ("literal inner $Role must read its unique PID log from byte zero; " +
            "post-launch baselines can hide boot markers (got $LogBaseline)")
    }
    $pidSlot = if ($Role -eq 'host') { 'HostPid' } else { 'JoinPid' }
    $logSlot = if ($Role -eq 'host') { 'HostLog' } else { 'JoinLog' }
    $processSlot = if ($Role -eq 'host') { 'HostProcess' } else { 'JoinProcess' }
    $executableSlot = if ($Role -eq 'host') {
        'HostExecutablePath'
    } else { 'JoinExecutablePath' }
    $baselineSlot = if ($Role -eq 'host') {
        'HostLogBaseline'
    } else { 'JoinLogBaseline' }
    if ([long]$Observer.State[$pidSlot] -ne 0 -or
        -not [string]::IsNullOrWhiteSpace([string]$Observer.State[$logSlot]) -or
        [long]$Observer.State[$baselineSlot] -ne -1 -or
        $null -ne $Observer.State[$processSlot] -or
        -not [string]::IsNullOrWhiteSpace(
            [string]$Observer.State[$executableSlot])) {
        throw "literal inner observer received $Role identity twice"
    }
    $Process.Refresh()
    if ($Process.HasExited) {
        throw "literal inner $Role exact Process exited before publication"
    }
    if ([string]::IsNullOrWhiteSpace($ExecutablePath) -or
        -not [IO.Path]::IsPathFullyQualified($ExecutablePath)) {
        throw "literal inner $Role launch executable provenance is not fully qualified"
    }
    $processPath = [IO.Path]::GetFullPath($ExecutablePath)
    $expectedGameExe = [IO.Path]::GetFullPath([string]$Observer.ExpectedGameExe)
    if (-not [string]::Equals(
            $processPath, $expectedGameExe,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "literal inner $Role process path '$processPath' is not '$expectedGameExe'"
    }
    $fullLogPath = [IO.Path]::GetFullPath($LogPath)
    $expectedLogPath = [IO.Path]::GetFullPath(
        (Join-Path -Path (Split-Path -Parent $expectedGameExe) `
            -ChildPath "mss32_$($Process.Id).log"))
    if (-not [string]::Equals(
            $fullLogPath, $expectedLogPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw ("literal inner $Role log '$fullLogPath' is not its exact PID log " +
            "'$expectedLogPath'")
    }
    $otherPidSlot = if ($Role -eq 'host') { 'JoinPid' } else { 'HostPid' }
    $otherLogSlot = if ($Role -eq 'host') { 'JoinLog' } else { 'HostLog' }
    $otherProcessSlot = if ($Role -eq 'host') { 'JoinProcess' } else { 'HostProcess' }
    $otherPid = [long]$Observer.State[$otherPidSlot]
    $otherLog = [string]$Observer.State[$otherLogSlot]
    $otherProcess = $Observer.State[$otherProcessSlot]
    if ($otherPid -gt 0 -and
        ($otherPid -eq [long]$Process.Id -or
         [object]::ReferenceEquals($otherProcess, $Process))) {
        throw 'literal inner host and join Process identities must be distinct'
    }
    if (-not [string]::IsNullOrWhiteSpace($otherLog) -and
        [string]::Equals(
            [IO.Path]::GetFullPath($otherLog), $fullLogPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'literal inner host and join log paths must be distinct'
    }
    # PID is the publication edge consumed by the role loop. Populate every
    # dependent identity field first and publish the PID last.
    $Observer.State[$processSlot] = $Process
    $Observer.State[$executableSlot] = $processPath
    $Observer.State[$logSlot] = $fullLogPath
    $Observer.State[$baselineSlot] = $LogBaseline
    $Observer.State[$pidSlot] = [long]$Process.Id
}

function Complete-LiteralInnerStartupObserver {
    param([Parameter(Mandatory)][object]$Observer)
    if ([bool]$Observer.Disposed) {
        throw 'literal inner observer is missing or already consumed'
    }
    try {
        [void]$Observer.Worker.EndInvoke($Observer.Async)
    } finally {
        $Observer.Worker.Dispose()
        $Observer.Disposed = $true
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$Observer.State.Error)) {
        throw [string]$Observer.State.Error
    }
    if (-not [bool]$Observer.State.Completed -or -not $Observer.State.Result) {
        throw 'literal inner observer ended without its before/tail artifacts'
    }
    return $Observer.State.Result
}

function Assert-LiteralInnerStartupObserverHealthy {
    param([Parameter(Mandatory)][object]$Observer)
    if ([bool]$Observer.Disposed) {
        throw 'literal inner observer was consumed before the outer run completed'
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$Observer.State.Error)) {
        throw [string]$Observer.State.Error
    }
    if ([string]$Observer.State.Phase -eq 'faulted') {
        throw 'literal inner observer entered faulted state without an error message'
    }
}

function Stop-LiteralInnerStartupObserver {
    param([object]$Observer)
    if (-not $Observer -or [bool]$Observer.Disposed) { return }
    $Observer.State.StopRequested = $true
    try {
        [void]$Observer.Worker.EndInvoke($Observer.Async)
    } finally {
        $Observer.Worker.Dispose()
        $Observer.Disposed = $true
    }
}
