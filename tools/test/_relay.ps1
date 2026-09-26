#requires -Version 7.0
# Shared toolkit for the relay-driven tests. Dot-source it, then:
#   $GameDir = Resolve-GameDir $GameDir        # from -GameDir, else tools/test/test.config.psd1
#   $relay   = Start-TestRelay                 # node relay.js (the host + joiner connect to it)
#   $client  = Start-GameClient -GameDir $GameDir -Role host
# and drive each client over the relay with the verb-noun commands below. See README.md.

function Get-RelayEnvironmentValue([string]$Name, [string]$Alias, [string]$Default = '') {
    $primary = [Environment]::GetEnvironmentVariable($Name, 'Process')
    $alternate = [Environment]::GetEnvironmentVariable($Alias, 'Process')
    if ($primary -and $alternate -and $primary -cne $alternate) {
        throw "$Name and $Alias conflict"
    }
    if ($primary) { return $primary }
    if ($alternate) { return $alternate }
    return $Default
}
$script:RelayHttpHost = Get-RelayEnvironmentValue 'D2TESTDRV_HTTP_HOST' 'D2_RELAY_HTTP_HOST' '127.0.0.1'
if ($script:RelayHttpHost -in @('0.0.0.0', '::')) { $script:RelayHttpHost = '127.0.0.1' }
$relayPortText = Get-RelayEnvironmentValue 'D2TESTDRV_HTTP_PORT' 'D2_RELAY_HTTP_PORT' '8077'
$parsedRelayPort = 0
if ($relayPortText -notmatch '^[1-9][0-9]*$' -or
    -not [int]::TryParse($relayPortText, [ref]$parsedRelayPort) -or
    $parsedRelayPort -lt 1 -or $parsedRelayPort -gt 65535) {
    throw 'relay HTTP port must be one decimal integer from 1 to 65535'
}
$script:RelayHttpPort = $parsedRelayPort
$script:RelayBase = "http://$($script:RelayHttpHost):$($script:RelayHttpPort)"
$script:RelayJs   = "$PSScriptRoot\..\relay\relay.js"
$script:RussobitExeSize = 4187648
$script:RussobitExeSha256 = '1375CDEF09EC470EE64FE5693FB734D7C69FB215212311D997F792B258A642EB'
# Reset on each dot-source. A scenario may explicitly select a longer bounded
# command-result deadline; strict acceptance callers retain the 5000 ms default.
$script:RelayCommandTimeoutMilliseconds = 5000

# ---- machine-specific config (the only thing that differs per dev machine) --------------------
# GameDir/ProcDump live in tools/test/test.config.psd1 (gitignored; copy test.config.sample.psd1).
# CI passes -GameDir explicitly, so it never needs the file.
function Get-TestConfig {
    $local  = "$PSScriptRoot\test.config.psd1"
    $sample = "$PSScriptRoot\test.config.sample.psd1"
    $path = if (Test-Path $local) { $local } else { $sample }
    return Import-PowerShellDataFile -Path $path
}

# Resolve the game folder: an explicit -GameDir wins (CI), else the config's GameDir. Validates that
# Discipl2.exe is there and tells the dev exactly how to fix it if not.
function Resolve-GameDir([string]$GameDir) {
    if (-not $GameDir) { $GameDir = (Get-TestConfig).GameDir }
    if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir 'Discipl2.exe'))) {
        throw "Game not found at '$GameDir' (no Discipl2.exe). Copy tools/test/test.config.sample.psd1 " +
              "to test.config.psd1 and set GameDir to your Disciples 2 install, or pass -GameDir."
    }
    $exe = Join-Path $GameDir 'Discipl2.exe'
    $actualSize = (Get-Item -LiteralPath $exe).Length
    $actualSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    if ($actualSize -ne $script:RussobitExeSize -or
        $actualSha256 -cne $script:RussobitExeSha256) {
        throw "Unsupported Discipl2.exe at '$exe': size=$actualSize sha256=$actualSha256. " +
              "The harness uses exact Russobit addresses and requires size=$($script:RussobitExeSize) " +
              "sha256=$($script:RussobitExeSha256)."
    }
    return $GameDir
}

# A scoped run owns only Process objects returned by its own launch calls. Pre-existing game
# instances, including instances from the same directory, are intentionally irrelevant. DirectPlay's
# helper is machine-global, so an already-live dplaysvr means another network session owns that one
# shared resource; report it before launch, but never stop it.
function Assert-ExclusiveTestMachine {
    param([Parameter(Mandatory)][string]$GameDir)
    if ([string]::IsNullOrWhiteSpace($GameDir)) {
        throw 'scoped test-machine check requires GameDir'
    }
    $directPlay = @(Get-Process dplaysvr -ErrorAction SilentlyContinue)
    if ($directPlay.Count -ne 0) {
        $owners = ($directPlay | ForEach-Object { "pid=$($_.Id)" }) -join ', '
        throw "A machine-global dplaysvr process is already running ($owners); " +
              'refusing to overlap its DirectPlay session. This harness will not stop it.'
    }
}

# Stop exactly one process object returned by this harness. Its existing association/handle is the
# authority; never look the process up again by numeric PID, which Windows may already have reused.
function Stop-OwnedProcess([System.Diagnostics.Process]$Process,
                           [int]$TimeoutMilliseconds = 30000) {
    if (-not $Process) { return }
    if ($TimeoutMilliseconds -le 0) {
        throw 'owned process teardown timeout must be positive'
    }
    $ownedPid = $Process.Id
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return }
        $Process.Kill()
    } catch {
        # A natural exit may race the sole Kill call. This is observation, not another stop action.
        $Process.Refresh()
        if ($Process.HasExited) { return }
        throw "owned process pid=$ownedPid could not be stopped: $($_.Exception.Message)"
    }
    if (-not $Process.WaitForExit($TimeoutMilliseconds)) {
        throw "owned process pid=$ownedPid did not exit within ${TimeoutMilliseconds}ms after its sole Kill"
    }
    $Process.Refresh()
    if (-not $Process.HasExited) {
        throw "owned process pid=$ownedPid remained live after WaitForExit"
    }
}

# ---- relay + clients --------------------------------------------------------------------------
# Start the node relay; return its process (throws if it never answers /api/status).
function Start-TestRelay {
    param([string]$LogDir = $env:TEMP)

    # Never mistake an unrelated process already bound to the HTTP endpoint for the child below.
    $endpointOccupied = $false
    try {
        Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 1 | Out-Null
        $endpointOccupied = $true
    } catch {
        # A refused connection is the expected pre-launch observation.
    }
    if ($endpointOccupied) {
        throw "relay endpoint $script:RelayBase is already in use"
    }

    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $runId = [guid]::NewGuid().ToString('N')
    $log = Join-Path $LogDir "relay.$runId.out.log"
    $errorLog = Join-Path $LogDir "relay.$runId.err.log"

    # Start-Process snapshots the environment. The nonce proves that readiness came from this child.
    $hadRunId = Test-Path Env:D2TESTDRV_RUN_ID
    $previousRunId = $env:D2TESTDRV_RUN_ID
    $previousInstanceId = [Environment]::GetEnvironmentVariable('D2_RELAY_INSTANCE_ID', 'Process')
    [Environment]::SetEnvironmentVariable('D2_RELAY_INSTANCE_ID', $runId, 'Process')
    $env:D2TESTDRV_RUN_ID = $runId
    try {
        $relay = Start-Process node -ArgumentList "`"$script:RelayJs`"" -PassThru `
            -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $errorLog
    } finally {
        [Environment]::SetEnvironmentVariable('D2_RELAY_INSTANCE_ID', $previousInstanceId, 'Process')
        if ($hadRunId) { $env:D2TESTDRV_RUN_ID = $previousRunId }
        else { Remove-Item Env:D2TESTDRV_RUN_ID -ErrorAction SilentlyContinue }
    }

    for ($i = 0; $i -lt 25; $i++) {
        $relay.Refresh()
        if ($relay.HasExited) {
            throw "relay exited with code $($relay.ExitCode); see '$errorLog' and '$log'"
        }
        $status = $null
        try {
            $status = Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 2
        } catch {
            # Bounded readiness observation after one launch; no action is repeated.
            Start-Sleep -Milliseconds 300
            continue
        }
        if ($status.instanceId -eq $runId) {
            if ($null -ne $status.terminalFault) {
                Stop-OwnedProcess $relay 5000
                throw "relay entered terminal state during startup: $($status.terminalFault.reason)"
            }
            if ($status.agentListening -eq $true) {
                $relay | Add-Member -NotePropertyName RelayInstanceId -NotePropertyValue $runId -Force
                $relay | Add-Member -NotePropertyName RelayRunId -NotePropertyValue $runId -Force
                $relay | Add-Member -NotePropertyName RelayLogPath -NotePropertyValue $log -Force
                $relay | Add-Member -NotePropertyName RelayErrorLogPath -NotePropertyValue $errorLog -Force
                return $relay
            }
        }
        Start-Sleep -Milliseconds 300
    }
    $relay.Refresh()
    if (-not $relay.HasExited) {
        Stop-OwnedProcess $relay 5000
    }
    throw "relay did not come up as instance $runId on $script:RelayBase; see '$errorLog' and '$log'"
}

# Launch a game instance as the DebugTest client for <Role> (host/join/...). Default flags are the
# boot fixes + UI reporter + relay bridge (dispatcher-driven; no built-in self-nav).
function Start-GameClient {
    param(
        [Parameter(Mandatory)][string]$GameDir,
        [Parameter(Mandatory)][string]$Role,
        [string[]]$Flags = @('SKIP_INTRO', 'BLACKSCREEN_FIX', 'UI_REPORTER', 'WORLD', 'RELAY_BRIDGE'),
        [switch]$LobbyChat,
        [ValidateSet('Local', 'Lobby')][string]$Transport = 'Local',
        [string]$ExpectedLobbyRoom = ''
    )
    if ($Role -notmatch '^[A-Za-z0-9._-]+$') {
        throw "DebugTest role is invalid: '$Role'"
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$GameDir\Discipl2.exe"; $psi.WorkingDirectory = $GameDir; $psi.UseShellExecute = $false
    if ($Transport -eq 'Lobby') {
        foreach ($key in @($psi.EnvironmentVariables.Keys)) {
            if ($key.StartsWith('D2_LOBBY_', [StringComparison]::OrdinalIgnoreCase)) {
                $psi.EnvironmentVariables.Remove($key)
                continue
            }
            if ($key.StartsWith('D2TESTDRV_', [StringComparison]::OrdinalIgnoreCase) -and
                $key -notin @('D2TESTDRV_PIPE_NAME', 'D2TESTDRV_BRIDGE_TCP_HOST', 'D2TESTDRV_BRIDGE_TCP_PORT')) {
                $psi.EnvironmentVariables.Remove($key)
            }
        }
        if ($ExpectedLobbyRoom) { $psi.EnvironmentVariables['D2TESTDRV_EXPECT_ROOM'] = $ExpectedLobbyRoom }
    }
    foreach ($f in $Flags) { $psi.EnvironmentVariables["D2TESTDRV_$f"] = "1" }
    if ($LobbyChat) { $psi.EnvironmentVariables["D2TESTDRV_LOBBY_CHAT"] = "1" }
    $psi.EnvironmentVariables["D2TESTDRV_ROLE"] = $Role
    $tcpHost = Get-RelayEnvironmentValue 'D2TESTDRV_BRIDGE_TCP_HOST' 'D2_RELAY_TCP_HOST'
    $tcpPort = Get-RelayEnvironmentValue 'D2TESTDRV_BRIDGE_TCP_PORT' 'D2_RELAY_TCP_PORT'
    if ($tcpPort -and -not $tcpHost) { $tcpHost = '127.0.0.1' }
    if ($tcpHost) { $psi.EnvironmentVariables['D2TESTDRV_BRIDGE_TCP_HOST'] = $tcpHost }
    if ($tcpPort) { $psi.EnvironmentVariables['D2TESTDRV_BRIDGE_TCP_PORT'] = $tcpPort }
    # Explicit local DirectPlay endpoint consumed by the one-shot EnumSessions hook.
    # The native adapter requires this endpoint explicitly.
    if ($Transport -eq 'Local') {
        $psi.EnvironmentVariables["D2TESTDRV_DIRECTPLAY_HOST"] = "127.0.0.1"
    } else {
        # Lobby owns OH admission and its production Arm. Never inherit the local adapter.
        foreach ($key in @($psi.EnvironmentVariables.Keys)) {
            if ($key.StartsWith('D2MSS_SIMTURNS', [StringComparison]::OrdinalIgnoreCase) -or
                $key -eq 'D2TESTDRV_DIRECTPLAY_HOST') {
                $psi.EnvironmentVariables.Remove($key)
            }
        }
    }
    return [System.Diagnostics.Process]::Start($psi)
}

# Resolve a random-scenario template's listbox index BY NAME (e.g. 'Diligence'). The generator
# (hooks::loadScenarioTemplates) stores the Templates\*.lua in a std::set<fs::path>, which orders them
# case-SENSITIVELY (ordinal: uppercase A-Z before '_' before lowercase a-z), NOT case-insensitively.
# Mirror that EXACTLY so a lowercase-named template (e.g. 'luckytest') resolves to the same listbox index
# the generator shows; a case-insensitive sort put it at the wrong index. (A .lua that fails to parse as a
# template is silently skipped by the engine, shifting later indices, but every shipped template parses.)
function Resolve-TemplateIndex([string]$GameDir, [string]$Name) {
    $names = @(Get-ChildItem (Join-Path $GameDir 'Templates') -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -ceq '.lua' } | ForEach-Object Name)
    $sorted = [System.Collections.Generic.List[string]]$names
    $sorted.Sort([System.StringComparer]::Ordinal)
    for ($i = 0; $i -lt $sorted.Count; $i++) {
        if ([System.IO.Path]::GetFileNameWithoutExtension($sorted[$i]) -ieq $Name) { return $i }
    }
    throw "template '$Name' not found in $GameDir\Templates"
}

# ---- the dispatcher's eyes (read UI) ----------------------------------------------------------
# All roles + their live state in one call: { host = {connected,dialog,widgets,...}, join = {...} }.
function Get-RelayState {
    $response = Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 10
    if ($null -ne $response.terminalFault) {
        throw "test relay terminal fault: $($response.terminalFault.reason)"
    }
    return $response.roles
}
# One role's state object (or $null before its sole Hello).
function Get-RoleState([string]$Role) {
    $state = Get-RelayState
    if ($null -eq $state) { return $null }
    $property = $state.PSObject.Properties[$Role]
    if ($null -eq $property) { return $null }
    return $property.Value
}

# Bind one relay role to the exact Process object launched by this harness and to this game folder's
# deployed mss32.dll. The loop observes the sole native Hello; it never launches or reconnects either
# side. A mismatching first Hello is terminal rather than ignored in favour of a later client.
function Assert-OwnedRelayClientIdentity {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory)][string]$GameDir,
        [int]$TimeoutSec = 30
    )
    if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 600) {
        throw "owned relay identity timeout is outside 1..600 seconds: $TimeoutSec"
    }
    $expectedModule = [IO.Path]::GetFullPath((Join-Path $GameDir 'mss32.dll'))
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "owned $Role process pid=$($Process.Id) exited before its sole relay Hello"
        }
        $state = Get-RoleState $Role
        if ($state) {
            if (-not [bool]$state.connected) {
                throw "relay registered $Role but its sole native client is disconnected"
            }
            if ([long]$state.pid -ne [long]$Process.Id) {
                throw "relay $Role pid=$($state.pid) does not match owned pid=$($Process.Id)"
            }
            if ([string]::IsNullOrWhiteSpace([string]$state.modulePath)) {
                throw "relay $Role Hello omitted its module path"
            }
            $actualModule = [IO.Path]::GetFullPath([string]$state.modulePath)
            if (-not [string]::Equals(
                    $actualModule, $expectedModule, [StringComparison]::OrdinalIgnoreCase)) {
                throw "relay $Role module '$actualModule' is not '$expectedModule'"
            }
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw "owned $Role pid=$($Process.Id) did not publish its sole relay Hello within ${TimeoutSec}s"
}
# The current dialog name for a role (or $null).
function Get-Dialog([string]$Role) {
    $r = Get-RoleState $Role; if ($r) { return $r.dialog } else { return $null }
}
# True only after the native reporter publishes the natural frame following the last bind cycle.
function Test-DialogReady([string]$Role, [string]$Dialog) {
    $state = Get-RoleState $Role
    if (-not $state) { return $false }
    $instance = $state.PSObject.Properties['dialogInstance']
    $appearance = $state.PSObject.Properties['dialogAppearance']
    $ready = $state.PSObject.Properties['dialogReady']
    if ($null -eq $instance -or $null -eq $appearance -or $null -eq $ready) {
        throw "$Role relay state omitted native dialog readiness/appearance"
    }
    return ([long]$instance.Value -gt 0 -and
            [long]$instance.Value -eq [long]$appearance.Value -and
            [bool]$ready.Value -and
            [string]$state.dialog -eq $Dialog)
}

# Exactly one rich UI observation. Literal ports of a single old /api/ui read
# use this form so no health request changes the source sequence.
function Get-GameUiSnapshot([string]$Role) {
    return Invoke-RestMethod `
        "$script:RelayBase/api/ui?role=$([uri]::EscapeDataString($Role))" -TimeoutSec 3
}
# The health-gated UI snapshot carries a causal appearance and one or more exact native action owners.
function Get-GameUi([string]$Role) {
    [void](Get-RelayState)
    return Get-GameUiSnapshot $Role
}
# Exactly one world observation, with no implicit state/health request in front
# of it.  Literal ports of the old /api/stacks oracle must use this accessor:
# request multiplicity and ordering are part of that oracle.
function Get-WorldSnapshot([string]$Role) {
    return Invoke-RestMethod `
        "$script:RelayBase/api/world?role=$([uri]::EscapeDataString($Role))" -TimeoutSec 3
}

# Exactly one read of the removable Russobit legacy CMidStack::Stream census.
# This is deliberately a separate host-authoritative source: the original OX
# scripts read live stack pointers from /api/stacks, not the client object-map
# projection exposed by /api/world. There is no role selector or world fallback.
function Get-LegacyStackSnapshot {
    return Invoke-RestMethod "$script:RelayBase/api/legacy-stacks" -TimeoutSec 3
}

# The health-gated world snapshot for ordinary harness code: { role, day, activePlayerId,
# players:[{id,relation,human,race,gold,...mana}], stacks:[{id,x,y,owner,relation,movement,
# leader,unitIds,unitStates:[{id,hp}],units,hp,subrace,inside}] }. `activePlayerId` is the phase's current
# strategic owner; `hp` is the group's total current HP while `unitStates` preserves exact stable unit
# identities and HP. `inside` is true for a garrisoned stack. Populated only once a scenario is loaded.
function Get-World([string]$Role) {
    [void](Get-RelayState)
    return Get-WorldSnapshot $Role
}

# Exactly one append-only UI-history observation.
function Get-UiHistorySnapshot([string]$Role, [long]$After = 0) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "UI evidence watermark is outside uint32: $After"
    }
    return Invoke-RestMethod `
        "$script:RelayBase/api/ui/history?role=$([uri]::EscapeDataString($Role))&after=$After" `
        -TimeoutSec 3
}
# Health-gated history helpers are for non-oracle waits. Append-only evidence
# watermarks are observations; they never authorize repeating an action.
function Get-UiHistory([string]$Role, [long]$After = 0) {
    [void](Get-RelayState)
    return Get-UiHistorySnapshot $Role $After
}

# One passive event subscription for an exact ready dialog/button publication.
# It emits no command and performs no state read, polling cycle, re-arm, or retry.
function Wait-UiButtonReadyPublication {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Button,
        [Parameter(Mandatory)][long]$AfterUiSequence,
        [Parameter(Mandatory)][int]$WaitMilliseconds,
        [Parameter(Mandatory)][long]$ExpectedProcessId,
        [Parameter(Mandatory)][string]$ExpectedModulePath
    )
    if ($Role -notmatch '^[A-Za-z0-9._-]+$') {
        throw "UI-ready role is invalid: '$Role'"
    }
    if ([string]::IsNullOrWhiteSpace($Dialog) -or
        [string]::IsNullOrWhiteSpace($Button)) {
        throw 'UI-ready dialog and button must be non-empty'
    }
    if ($AfterUiSequence -lt 0 -or $AfterUiSequence -gt [uint32]::MaxValue) {
        throw "UI-ready watermark is outside uint32: $AfterUiSequence"
    }
    if ($WaitMilliseconds -lt 1 -or $WaitMilliseconds -gt 600000) {
        throw "UI-ready wait is outside 1..600000 ms: $WaitMilliseconds"
    }
    if ($ExpectedProcessId -lt 1 -or $ExpectedProcessId -gt [uint32]::MaxValue) {
        throw "UI-ready expected PID is outside uint32: $ExpectedProcessId"
    }
    if ([string]::IsNullOrWhiteSpace($ExpectedModulePath)) {
        throw 'UI-ready expected module path is empty'
    }

    $path = ("$script:RelayBase/api/ui/wait-ready?role={0}&dlg={1}" +
        '&btn={2}&after={3}&waitMs={4}') -f
        [uri]::EscapeDataString($Role), [uri]::EscapeDataString($Dialog),
        [uri]::EscapeDataString($Button), $AfterUiSequence, $WaitMilliseconds
    $httpTimeout = [math]::Ceiling($WaitMilliseconds / 1000.0) + 10

    # This is the sole request. Any timeout/fault is terminal to the caller and
    # can never launch or re-launch a client through this observation helper.
    $response = Invoke-RestMethod $path -TimeoutSec $httpTimeout
    if ($null -eq $response) { throw 'UI-ready relay response is empty' }
    foreach ($propertyName in @(
            'latestSeq', 'observation', 'terminalFault', 'timedOut')) {
        if ($null -eq $response.PSObject.Properties[$propertyName]) {
            throw "UI-ready relay response omitted '$propertyName'"
        }
    }
    if ($null -ne $response.terminalFault) {
        throw "UI-ready relay terminal fault: $($response.terminalFault.reason)"
    }
    if ($response.timedOut -isnot [bool]) {
        throw 'UI-ready relay response has a non-boolean timedOut field'
    }
    if ([bool]$response.timedOut) {
        throw "UI-ready publication timed out after ${WaitMilliseconds}ms"
    }

    $observation = $response.observation
    if ($null -eq $observation) {
        throw 'UI-ready relay response omitted its exact observation'
    }
    foreach ($propertyName in @(
            'role', 'connected', 'pid', 'modulePath', 'dialog',
            'dialogInstance', 'dialogAppearance', 'dialogReady',
            'uiSeq', 'widgets', 'targets')) {
        if ($null -eq $observation.PSObject.Properties[$propertyName]) {
            throw "UI-ready observation omitted '$propertyName'"
        }
    }
    if (-not [string]::Equals(
            [string]$observation.role, $Role, [StringComparison]::Ordinal) -or
        -not [bool]$observation.connected -or
        [long]$observation.pid -ne $ExpectedProcessId) {
        throw 'UI-ready observation does not belong to the expected owned role/PID'
    }
    $actualModulePath = [IO.Path]::GetFullPath([string]$observation.modulePath)
    $expectedFullModulePath = [IO.Path]::GetFullPath($ExpectedModulePath)
    if (-not [string]::Equals(
            $actualModulePath, $expectedFullModulePath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "UI-ready module '$actualModulePath' is not '$expectedFullModulePath'"
    }
    if (-not [string]::Equals(
            [string]$observation.dialog, $Dialog, [StringComparison]::Ordinal) -or
        $observation.dialogReady -isnot [bool] -or
        -not [bool]$observation.dialogReady) {
        throw 'UI-ready observation does not match the exact ready dialog'
    }

    [long]$sequence = $observation.uiSeq
    [long]$latestSequence = $response.latestSeq
    [long]$instance = $observation.dialogInstance
    [long]$appearance = $observation.dialogAppearance
    if ($sequence -le $AfterUiSequence -or $sequence -gt [uint32]::MaxValue -or
        $latestSequence -lt $sequence -or $latestSequence -gt [uint32]::MaxValue) {
        throw 'UI-ready sequence proof is invalid or did not advance'
    }
    if ($instance -lt 1 -or $instance -gt [uint32]::MaxValue -or
        $appearance -ne $instance) {
        throw 'UI-ready dialog appearance identity is invalid or inconsistent'
    }

    $matchingTargets = @($observation.targets | Where-Object {
        [string]::Equals(
            [string]$_.dialog, $Dialog, [StringComparison]::Ordinal)
    })
    if ($matchingTargets.Count -ne 1 -or
        [long]$matchingTargets[0].instance -lt 1 -or
        [long]$matchingTargets[0].instance -gt [uint32]::MaxValue) {
        throw "UI-ready observation has $($matchingTargets.Count) exact native owners"
    }
    $matchingButtons = @($matchingTargets[0].widgets | Where-Object {
        [string]::Equals([string]$_.name, $Button, [StringComparison]::Ordinal) -and
        [string]::Equals([string]$_.type, 'button', [StringComparison]::Ordinal)
    })
    if ($matchingButtons.Count -ne 1 -or
        $null -eq $matchingButtons[0].PSObject.Properties['state'] -or
        $null -eq $matchingButtons[0].state.PSObject.Properties['enabled'] -or
        $matchingButtons[0].state.enabled -isnot [bool] -or
        -not [bool]$matchingButtons[0].state.enabled) {
        throw 'UI-ready observation does not prove one explicitly enabled exact button'
    }
    return $observation
}
function Get-WorldHistorySnapshot([string]$Role, [long]$After = 0) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "world evidence watermark is outside uint32: $After"
    }
    return Invoke-RestMethod `
        "$script:RelayBase/api/world/history?role=$([uri]::EscapeDataString($Role))&after=$After" `
        -TimeoutSec 3
}
function Get-WorldHistory([string]$Role, [long]$After = 0) {
    [void](Get-RelayState)
    return Get-WorldHistorySnapshot $Role $After
}

# One passive subscription for a causal exact current world state on both owned
# clients. It emits no command and performs no polling, sleep, re-arm, or retry.
function Wait-ExactWorldPairPublication {
    param(
        [Parameter(Mandatory)][string]$HeroId,
        [Parameter(Mandatory)][int]$X,
        [Parameter(Mandatory)][int]$Y,
        [Parameter(Mandatory)][int]$Movement,
        [Parameter(Mandatory)][long]$HostAfterWorldSequence,
        [Parameter(Mandatory)][long]$JoinAfterWorldSequence,
        [Parameter(Mandatory)][long]$ExpectedHostProcessId,
        [Parameter(Mandatory)][long]$ExpectedJoinProcessId,
        [Parameter(Mandatory)][string]$ExpectedHostModulePath,
        [Parameter(Mandatory)][string]$ExpectedJoinModulePath,
        [int]$WaitMilliseconds = 30000
    )
    if ($HeroId -cnotmatch '^0x[0-9A-F]{8}$') {
        throw "world-pair hero id is not canonical: '$HeroId'"
    }
    foreach ($watermark in @($HostAfterWorldSequence, $JoinAfterWorldSequence)) {
        if ($watermark -lt 0 -or $watermark -gt [uint32]::MaxValue) {
            throw "world-pair watermark is outside uint32: $watermark"
        }
    }
    if ($Movement -lt 0 -or $Movement -gt 255) {
        throw "world-pair movement is outside uint8: $Movement"
    }
    if ($WaitMilliseconds -lt 1 -or $WaitMilliseconds -gt 120000) {
        throw "world-pair wait is outside 1..120000 ms: $WaitMilliseconds"
    }
    foreach ($pidValue in @($ExpectedHostProcessId, $ExpectedJoinProcessId)) {
        if ($pidValue -lt 1 -or $pidValue -gt [uint32]::MaxValue) {
            throw "world-pair expected PID is outside uint32: $pidValue"
        }
    }
    if ([string]::IsNullOrWhiteSpace($ExpectedHostModulePath) -or
        [string]::IsNullOrWhiteSpace($ExpectedJoinModulePath)) {
        throw 'world-pair expected module path is empty'
    }

    $path = ("$script:RelayBase/api/world/wait-exact-pair?hostAfter={0}" +
        '&joinAfter={1}&id={2}&x={3}&y={4}&mp={5}&waitMs={6}') -f
        $HostAfterWorldSequence, $JoinAfterWorldSequence,
        [uri]::EscapeDataString($HeroId), $X, $Y, $Movement, $WaitMilliseconds
    $httpTimeout = [math]::Ceiling($WaitMilliseconds / 1000.0) + 10

    # This is the sole request. Its relay-side predicate is evaluated against
    # the two current role snapshots after every native WorldSnapshot event.
    $response = Invoke-RestMethod $path -TimeoutSec $httpTimeout
    if ($null -eq $response) { throw 'world-pair relay response is empty' }
    foreach ($propertyName in @(
            'latestSeq', 'observation', 'terminalFault', 'timedOut')) {
        if ($null -eq $response.PSObject.Properties[$propertyName]) {
            throw "world-pair relay response omitted '$propertyName'"
        }
    }
    if ($null -ne $response.terminalFault) {
        throw "world-pair relay terminal fault: $($response.terminalFault.reason)"
    }
    if ($response.timedOut -isnot [bool]) {
        throw 'world-pair relay response has a non-boolean timedOut field'
    }
    if ([bool]$response.timedOut) {
        throw "world-pair publication timed out after ${WaitMilliseconds}ms"
    }
    $observation = $response.observation
    if ($null -eq $observation) {
        throw 'world-pair relay response omitted its exact observation'
    }
    foreach ($propertyName in @('id', 'x', 'y', 'movement', 'host', 'join')) {
        if ($null -eq $observation.PSObject.Properties[$propertyName]) {
            throw "world-pair observation omitted '$propertyName'"
        }
    }
    if ([string]$observation.id -cne $HeroId -or
        [int]$observation.x -ne $X -or [int]$observation.y -ne $Y -or
        [int]$observation.movement -ne $Movement) {
        throw 'world-pair observation does not match its exact requested endpoint'
    }

    $expected = [ordered]@{
        host = [pscustomobject]@{
            after = $HostAfterWorldSequence
            pid = $ExpectedHostProcessId
            module = [IO.Path]::GetFullPath($ExpectedHostModulePath)
        }
        join = [pscustomobject]@{
            after = $JoinAfterWorldSequence
            pid = $ExpectedJoinProcessId
            module = [IO.Path]::GetFullPath($ExpectedJoinModulePath)
        }
    }
    foreach ($role in @('host', 'join')) {
        $roleObservation = $observation.$role
        if ($null -eq $roleObservation) {
            throw "world-pair observation omitted the $role role"
        }
        foreach ($propertyName in @(
                'role', 'connected', 'pid', 'modulePath', 'worldSeq', 'stack')) {
            if ($null -eq $roleObservation.PSObject.Properties[$propertyName]) {
                throw "world-pair $role observation omitted '$propertyName'"
            }
        }
        $stack = $roleObservation.stack
        foreach ($propertyName in @('id', 'x', 'y', 'movement')) {
            if ($null -eq $stack.PSObject.Properties[$propertyName]) {
                throw "world-pair $role stack omitted '$propertyName'"
            }
        }
        [long]$sequence = $roleObservation.worldSeq
        $actualModule = [IO.Path]::GetFullPath([string]$roleObservation.modulePath)
        if ([string]$roleObservation.role -cne $role -or
            $roleObservation.connected -isnot [bool] -or
            -not [bool]$roleObservation.connected -or
            [long]$roleObservation.pid -ne [long]$expected[$role].pid -or
            $sequence -le [long]$expected[$role].after -or
            $sequence -gt [uint32]::MaxValue -or
            -not [string]::Equals(
                $actualModule, [string]$expected[$role].module,
                [StringComparison]::OrdinalIgnoreCase) -or
            [string]$stack.id -cne $HeroId -or
            [int]$stack.x -ne $X -or [int]$stack.y -ne $Y -or
            [int]$stack.movement -ne $Movement) {
            throw "world-pair $role observation is not its exact owned causal state"
        }
    }
    [long]$latestSequence = $response.latestSeq
    if ($latestSequence -lt [long]$observation.host.worldSeq -or
        $latestSequence -lt [long]$observation.join.worldSeq -or
        $latestSequence -gt [uint32]::MaxValue) {
        throw 'world-pair latest sequence is inconsistent with its two observations'
    }
    return $observation
}

function Get-TurnHistory([long]$After = 0,
                         [string]$Role = '',
                         [int]$WaitMilliseconds = 0) {
    if ($After -lt 0 -or $After -gt [uint32]::MaxValue) {
        throw "turn evidence watermark is outside uint32: $After"
    }
    if (-not [string]::IsNullOrEmpty($Role) -and $Role -notmatch '^[A-Za-z0-9._-]+$') {
        throw "turn evidence role is invalid: '$Role'"
    }
    if ($WaitMilliseconds -lt 0 -or $WaitMilliseconds -gt 120000) {
        throw "turn evidence wait is outside 0..120000 ms: $WaitMilliseconds"
    }
    $roleQuery = if ([string]::IsNullOrEmpty($Role)) {
        ''
    } else {
        "&role=$([uri]::EscapeDataString($Role))"
    }
    $waitQuery = if ($WaitMilliseconds -eq 0) { '' } else { "&waitMs=$WaitMilliseconds" }
    $httpTimeout = if ($WaitMilliseconds -eq 0) {
        3
    } else {
        [math]::Ceiling($WaitMilliseconds / 1000.0) + 10
    }
    $response = Invoke-RestMethod `
        "$script:RelayBase/api/turn/history?after=$After$roleQuery$waitQuery" `
        -TimeoutSec $httpTimeout
    if ($null -ne $response.terminalFault) {
        throw "test relay terminal fault: $($response.terminalFault.reason)"
    }
    return $response
}
# Convenience views over the world snapshot: the local player's resources, and the map's stacks.
function Get-Resources([string]$Role) { (Get-World $Role).players | Where-Object { $_.relation -eq 'self' } | Select-Object -First 1 }
function Get-Stacks([string]$Role) { (Get-World $Role).stacks }
function Get-Camps([string]$Role) { (Get-World $Role).camps }
function Get-Bags([string]$Role) { (Get-World $Role).bags }
function Get-LobbyChat([string]$Role) {
    return (Invoke-RestMethod "$script:RelayBase/api/lobby/chat?role=$([uri]::EscapeDataString($Role))" -TimeoutSec 3).messages
}
# Wait for readiness of one exact native dialog publication. This loop only observes state.
function Wait-Dialog([string]$Role, [string]$Dialog, [int]$TimeoutSec = 60) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-DialogReady $Role $Dialog) { return $true }
        Start-Sleep -Milliseconds 400
    }
    return $false
}

# ---- the dispatcher's hands (drive UI) ---------------------------------------------------------
# CommandResult proves delivery/target resolution, not the callback's effect. The latter must be
# established by a later UI/world/turn publication. HTTP and protocol failures are never converted
# to `found=false`.
function script:Post([string]$Path, [int]$TimeoutSec = 8) {
    if (-not $PSBoundParameters.ContainsKey('TimeoutSec') -and
        $script:RelayCommandTimeoutMilliseconds -ne 5000) {
        if ($script:RelayCommandTimeoutMilliseconds -lt 1000 -or
            $script:RelayCommandTimeoutMilliseconds -gt 120000) {
            throw 'relay command-result deadline is outside 1000..120000 ms'
        }
        $Path += "&timeoutMs=$script:RelayCommandTimeoutMilliseconds"
        $TimeoutSec = [int][math]::Ceiling($script:RelayCommandTimeoutMilliseconds / 1000.0) + 10
    }
    if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 130) {
        throw "UI HTTP timeout is outside 1..130 seconds: $TimeoutSec"
    }
    return (Invoke-RestMethod "$script:RelayBase/api/ui/$Path" `
        -Method POST -TimeoutSec $TimeoutSec).found
}

function Get-ActionTargetBinding(
    [string]$Role,
    [string]$Dialog,
    [long]$ExpectedInstance = 0,
    [long]$ExpectedAppearance = 0) {
    $state = Get-RoleState $Role
    if (-not $state -or -not [bool]$state.dialogReady) {
        throw "$Role has no ready native dialog owner for '$Dialog'"
    }
    $instanceProperty = $state.PSObject.Properties['dialogInstance']
    $appearanceProperty = $state.PSObject.Properties['dialogAppearance']
    $targetsProperty = $state.PSObject.Properties['targets']
    if ($null -eq $instanceProperty -or $null -eq $appearanceProperty -or
        $null -eq $targetsProperty) {
        throw "$Role relay state omitted native action identity"
    }
    [long]$appearance = $appearanceProperty.Value
    if ($appearance -lt 1 -or $appearance -gt [uint32]::MaxValue -or
        $appearance -ne [long]$instanceProperty.Value) {
        throw "$Role dialog appearance token is invalid or inconsistent: $appearance"
    }
    if ($ExpectedAppearance -gt 0 -and $appearance -ne $ExpectedAppearance) {
        throw "$Role '$Dialog' appearance changed: expected $ExpectedAppearance, current $appearance"
    }
    $matches = @($targetsProperty.Value | Where-Object {
        [string]$_.dialog -eq $Dialog -and
        ($ExpectedInstance -le 0 -or [long]$_.instance -eq $ExpectedInstance)
    })
    if ($matches.Count -ne 1) {
        throw "$Role has $($matches.Count) ready '$Dialog' owners for expected instance $ExpectedInstance"
    }
    [long]$owner = $matches[0].instance
    if ($owner -lt 1 -or $owner -gt [uint32]::MaxValue) {
        throw "$Role '$Dialog' owner token is outside uint32: $owner"
    }
    return [pscustomobject]@{ Appearance = $appearance; Instance = $owner }
}

function Resolve-ActionTargetBinding([string]$Role, [string]$Dialog,
                                     [long]$ExpectedInstance,
                                     [long]$ExpectedAppearance) {
    if (($ExpectedInstance -eq 0) -xor ($ExpectedAppearance -eq 0)) {
        throw 'a UI action must carry both appearance and owner, or capture both together'
    }
    if ($ExpectedInstance -eq 0) {
        # One state publication is captured once and posted once. No later state may replace it.
        return Get-ActionTargetBinding $Role $Dialog
    }
    if ($ExpectedInstance -lt 1 -or $ExpectedInstance -gt [uint32]::MaxValue -or
        $ExpectedAppearance -lt 1 -or $ExpectedAppearance -gt [uint32]::MaxValue) {
        throw 'UI appearance/owner token is outside uint32'
    }
    return [pscustomobject]@{
        Appearance = $ExpectedAppearance
        Instance = $ExpectedInstance
    }
}

function Get-MapActionTargetBinding([string]$Role) {
    $state = Get-RoleState $Role
    if (-not $state -or -not [bool]$state.dialogReady -or
        @('DLG_STRATEGIC', 'DLG_ISO_PAL') -notcontains [string]$state.dialog) {
        throw "$Role has no ready bare strategic-map owner"
    }
    $instanceProperty = $state.PSObject.Properties['dialogInstance']
    $appearanceProperty = $state.PSObject.Properties['dialogAppearance']
    $targetsProperty = $state.PSObject.Properties['targets']
    if ($null -eq $instanceProperty -or $null -eq $appearanceProperty -or
        $null -eq $targetsProperty) {
        throw "$Role relay state omitted strategic-map action identity"
    }
    [long]$appearance = $appearanceProperty.Value
    if ($appearance -lt 1 -or $appearance -gt [uint32]::MaxValue -or
        $appearance -ne [long]$instanceProperty.Value) {
        throw "$Role strategic-map appearance token is invalid or inconsistent: $appearance"
    }
    $matches = @($targetsProperty.Value | Where-Object {
        [string]$_.dialog -eq [string]$state.dialog
    })
    if ($matches.Count -ne 1) {
        throw "$Role has $($matches.Count) ready root owners for '$($state.dialog)'"
    }
    [long]$owner = $matches[0].instance
    if ($owner -lt 1 -or $owner -gt [uint32]::MaxValue) {
        throw "$Role strategic-map owner token is outside uint32: $owner"
    }
    return [pscustomobject]@{ Appearance = $appearance; Instance = $owner }
}

function Invoke-Button([string]$Role, [string]$Dialog, [string]$Button,
                       [long]$ExpectedInstance = 0,
                       [long]$ExpectedAppearance = 0,
                       [int]$CommandTimeoutMilliseconds = 0) {
    if ($CommandTimeoutMilliseconds -ne 0 -and
        ($CommandTimeoutMilliseconds -lt 1000 -or $CommandTimeoutMilliseconds -gt 120000)) {
        throw "button command-result timeout is outside 1000..120000 ms"
    }
    $binding = Resolve-ActionTargetBinding `
        $Role $Dialog $ExpectedInstance $ExpectedAppearance
    $path = "invoke?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&btn=$([uri]::EscapeDataString($Button))&appearance=$($binding.Appearance)&instance=$($binding.Instance)"
    if ($CommandTimeoutMilliseconds -eq 0) {
        return [bool](script:Post $path)
    }
    $path += "&timeoutMs=$CommandTimeoutMilliseconds"
    $httpTimeout = [math]::Ceiling($CommandTimeoutMilliseconds / 1000.0) + 10
    return [bool](script:Post $path $httpTimeout)
}

# Arm one symbolic button intent after the caller's UI evidence watermark. The relay binds that
# intent to the first later exact ready native owner and sends exactly one InvokeButton command.
# This helper performs one POST only: it never reads relay state, polls UI, retries, or repairs an
# owner. The returned observation is the publication that atomically authorized the sole command.
function Invoke-ButtonWhenReady {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Button,
        [Parameter(Mandatory)][long]$AfterUiSequence,
        [int]$WaitMilliseconds = 30000,
        [int]$CommandTimeoutMilliseconds = 0
    )
    if ($Role -notmatch '^[A-Za-z0-9._-]+$') {
        throw "button-intent role is invalid: '$Role'"
    }
    if ([string]::IsNullOrWhiteSpace($Dialog) -or
        [string]::IsNullOrWhiteSpace($Button)) {
        throw 'button-intent dialog and button must be non-empty'
    }
    if ($AfterUiSequence -lt 0 -or $AfterUiSequence -gt [uint32]::MaxValue) {
        throw "button-intent UI watermark is outside uint32: $AfterUiSequence"
    }
    if ($WaitMilliseconds -lt 1 -or $WaitMilliseconds -gt 120000) {
        throw "button-intent readiness wait is outside 1..120000 ms: $WaitMilliseconds"
    }
    if ($CommandTimeoutMilliseconds -ne 0 -and
        ($CommandTimeoutMilliseconds -lt 1000 -or
         $CommandTimeoutMilliseconds -gt 120000)) {
        throw 'button-intent command-result timeout is outside 1000..120000 ms'
    }

    $path = ("$script:RelayBase/api/ui/invoke-when-ready?role={0}&dlg={1}" +
        '&btn={2}&after={3}&waitMs={4}') -f
        [uri]::EscapeDataString($Role), [uri]::EscapeDataString($Dialog),
        [uri]::EscapeDataString($Button), $AfterUiSequence, $WaitMilliseconds
    if ($CommandTimeoutMilliseconds -ne 0) {
        $path += "&timeoutMs=$CommandTimeoutMilliseconds"
    }
    $commandBudget = if ($CommandTimeoutMilliseconds -eq 0) {
        5000
    } else {
        $CommandTimeoutMilliseconds
    }
    $httpTimeout = [math]::Ceiling(($WaitMilliseconds + $commandBudget) / 1000.0) + 10

    # This is deliberately the sole request in the function. A timeout or any malformed/negative
    # response is terminal to the caller and never authorizes another intent.
    $response = Invoke-RestMethod $path -Method POST -TimeoutSec $httpTimeout
    if ($null -eq $response) {
        throw 'button-intent relay response is empty'
    }
    foreach ($propertyName in @('found', 'role', 'invoke', 'observation')) {
        if ($null -eq $response.PSObject.Properties[$propertyName]) {
            throw "button-intent relay response omitted '$propertyName'"
        }
    }
    if ($response.found -isnot [bool]) {
        throw 'button-intent relay response has a non-boolean found field'
    }
    if (-not [string]::Equals(
            [string]$response.role, $Role, [StringComparison]::Ordinal)) {
        throw "button-intent relay response role '$($response.role)' is not '$Role'"
    }

    $invoke = $response.invoke
    $observation = $response.observation
    if ($null -eq $invoke -or $null -eq $observation) {
        throw 'button-intent relay response omitted invoke/observation payload'
    }
    foreach ($propertyName in @('dlg', 'btn', 'appearance', 'instance')) {
        if ($null -eq $invoke.PSObject.Properties[$propertyName]) {
            throw "button-intent invoke proof omitted '$propertyName'"
        }
    }
    foreach ($propertyName in @(
            'role', 'dialog', 'dialogInstance', 'dialogAppearance',
            'dialogReady', 'strategicIdle', 'uiSeq', 'widgets', 'targets')) {
        if ($null -eq $observation.PSObject.Properties[$propertyName]) {
            throw "button-intent observation omitted '$propertyName'"
        }
    }
    if (-not [string]::Equals(
            [string]$invoke.dlg, $Dialog, [StringComparison]::Ordinal) -or
        -not [string]::Equals(
            [string]$invoke.btn, $Button, [StringComparison]::Ordinal)) {
        throw 'button-intent invoke proof does not match the requested dialog/button'
    }
    if (-not [string]::Equals(
            [string]$observation.role, $Role, [StringComparison]::Ordinal) -or
        -not [string]::Equals(
            [string]$observation.dialog, $Dialog, [StringComparison]::Ordinal)) {
        throw 'button-intent observation does not match the requested role/dialog'
    }
    if ($observation.dialogReady -isnot [bool] -or
        -not [bool]$observation.dialogReady) {
        throw 'button-intent observation is not an exact ready dialog publication'
    }
    if ($Dialog -eq 'DLG_STRATEGIC' -and $Button -eq 'BTN_END_TURN' -and
        ($observation.strategicIdle -isnot [bool] -or
         -not [bool]$observation.strategicIdle)) {
        throw 'button-intent strategic End Turn observation is not native-idle'
    }

    [long]$uiSequence = $observation.uiSeq
    [long]$dialogInstance = $observation.dialogInstance
    [long]$dialogAppearance = $observation.dialogAppearance
    [long]$invokeAppearance = $invoke.appearance
    [long]$invokeInstance = $invoke.instance
    if ($uiSequence -le $AfterUiSequence -or $uiSequence -gt [uint32]::MaxValue) {
        throw "button-intent observation sequence $uiSequence did not advance after $AfterUiSequence"
    }
    if ($dialogInstance -lt 1 -or $dialogInstance -gt [uint32]::MaxValue -or
        $dialogAppearance -ne $dialogInstance -or
        $invokeAppearance -ne $dialogAppearance) {
        throw 'button-intent observation/invoke appearance identity is invalid or inconsistent'
    }
    if ($invokeInstance -lt 1 -or $invokeInstance -gt [uint32]::MaxValue) {
        throw "button-intent native owner is outside uint32: $invokeInstance"
    }

    $matchingTargets = @($observation.targets | Where-Object {
        [string]::Equals([string]$_.dialog, $Dialog, [StringComparison]::Ordinal) -and
        [long]$_.instance -eq $invokeInstance
    })
    if ($matchingTargets.Count -ne 1) {
        throw "button-intent observation has $($matchingTargets.Count) exact native owners"
    }
    $matchingButtons = @($matchingTargets[0].widgets | Where-Object {
        [string]::Equals([string]$_.name, $Button, [StringComparison]::Ordinal) -and
        [string]::Equals([string]$_.type, 'button', [StringComparison]::Ordinal)
    })
    if ($matchingButtons.Count -ne 1 -or
        $null -eq $matchingButtons[0].PSObject.Properties['state'] -or
        $null -eq $matchingButtons[0].state.PSObject.Properties['enabled'] -or
        $matchingButtons[0].state.enabled -isnot [bool] -or
        -not [bool]$matchingButtons[0].state.enabled) {
        throw 'button-intent observation does not prove one explicitly enabled exact button'
    }
    if (-not [bool]$response.found) {
        throw "button-intent ${Role} ${Dialog}::${Button} did not resolve on its captured owner"
    }
    return $response
}

# Issue one button callback for one exact native dialog appearance. Callers retain the ledger for the
# logical flow. Re-observing the same appearance is an observation only; it can never refire the action.
function Invoke-ButtonOncePerAppearance {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Button,
        [Parameter(Mandatory)][hashtable]$Consumed
    )
    $binding = Get-ActionTargetBinding $Role $Dialog
    $key = "$Role|$($binding.Appearance)|$($binding.Instance)|$Dialog|$Button"
    if ($Consumed.ContainsKey($key)) { return $false }
    # Claim before transport publication. A thrown/negative result is terminal to the caller and may
    # not be converted into another send for the same native owner.
    $Consumed[$key] = $true
    if (-not (Invoke-Button $Role $Dialog $Button `
            $binding.Instance $binding.Appearance)) {
        throw "${Role} ${Dialog}::${Button} did not resolve on its captured native owner"
    }
    return $true
}

# One callback followed only by bounded observation that the captured appearance retired. A newly
# published appearance of the same dialog also proves departure of the old owner.
function Invoke-ButtonAndWaitForDeparture {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string]$Button,
        [int]$TimeoutSec = 30
    )
    if ($TimeoutSec -lt 1 -or $TimeoutSec -gt 300) {
        throw "dialog departure timeout is outside 1..300 seconds: $TimeoutSec"
    }
    $binding = Get-ActionTargetBinding $Role $Dialog
    if (-not (Invoke-Button $Role $Dialog $Button `
            $binding.Instance $binding.Appearance)) { return $false }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        $state = Get-RoleState $Role
        if ($state -and ([string]$state.dialog -ne $Dialog -or
                [long]$state.dialogAppearance -ne [long]$binding.Appearance)) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

# Select exactly one button that the native snapshot says exists on this appearance, then invoke it
# through the same appearance ledger. Missing candidates cause no command and no probing callbacks.
function Invoke-FirstAvailableButtonOnce {
    param(
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Dialog,
        [Parameter(Mandatory)][string[]]$Candidates,
        [Parameter(Mandatory)][hashtable]$Consumed
    )
    $state = Get-RoleState $Role
    if (-not $state -or -not [bool]$state.dialogReady -or
        [string]$state.dialog -ne $Dialog) { return $false }
    $available = @($state.buttons)
    foreach ($candidate in $Candidates) {
        if ($available -contains $candidate) {
            return Invoke-ButtonOncePerAppearance `
                -Role $Role -Dialog $Dialog -Button $candidate -Consumed $Consumed
        }
    }
    return $false
}

function Set-ListSelection([string]$Role, [string]$Dialog, [string]$ListBox, [int]$Index,
                           [long]$ExpectedInstance = 0,
                           [long]$ExpectedAppearance = 0) {
    $binding = Resolve-ActionTargetBinding `
        $Role $Dialog $ExpectedInstance $ExpectedAppearance
    [bool](script:Post "select?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&lb=$([uri]::EscapeDataString($ListBox))&index=$Index&appearance=$($binding.Appearance)&instance=$($binding.Instance)")
}

function Set-ScenarioSelection([string]$Role, [string]$Dialog, [string]$ListBox,
                               [string]$ExactPath,
                               [long]$ExpectedInstance = 0,
                               [long]$ExpectedAppearance = 0) {
    $binding = Resolve-ActionTargetBinding `
        $Role $Dialog $ExpectedInstance $ExpectedAppearance
    [bool](script:Post "select-scenario?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&lb=$([uri]::EscapeDataString($ListBox))&path=$([uri]::EscapeDataString($ExactPath))&appearance=$($binding.Appearance)&instance=$($binding.Instance)")
}

function Set-SpinOption([string]$Role, [string]$Dialog, [string]$Spin, [int]$Index,
                        [long]$ExpectedInstance = 0,
                        [long]$ExpectedAppearance = 0) {
    $binding = Resolve-ActionTargetBinding `
        $Role $Dialog $ExpectedInstance $ExpectedAppearance
    [bool](script:Post "spin?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&spin=$([uri]::EscapeDataString($Spin))&index=$Index&appearance=$($binding.Appearance)&instance=$($binding.Instance)")
}

function Set-EditText([string]$Role, [string]$Dialog, [string]$Edit, [string]$Text,
                      [long]$ExpectedInstance = 0,
                      [long]$ExpectedAppearance = 0) {
    $binding = Resolve-ActionTargetBinding `
        $Role $Dialog $ExpectedInstance $ExpectedAppearance
    [bool](script:Post "edit?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&edit=$([uri]::EscapeDataString($Edit))&text=$([uri]::EscapeDataString($Text))&appearance=$($binding.Appearance)&instance=$($binding.Instance)")
}

# Secret input is a bounded POST body, never a URL or reflected response field.
function Set-SecretEditText([string]$Role, [string]$Dialog, [string]$Edit, [string]$Text) {
    $binding = Resolve-ActionTargetBinding $Role $Dialog 0 0
    $uri = "$script:RelayBase/api/ui/edit-secret?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&edit=$([uri]::EscapeDataString($Edit))&appearance=$($binding.Appearance)&instance=$($binding.Instance)"
    try {
        return [bool](Invoke-RestMethod $uri -Method POST -ContentType 'text/plain; charset=utf-8' `
            -Body ([Text.Encoding]::UTF8.GetBytes($Text)) -TimeoutSec 8).found
    } catch {
        throw "Secret input command failed for $Role; its value is intentionally omitted."
    }
}
# Move stack <Id> from one exact caller-observed source to one exact free tile, or attack the one live
# non-owned stack on the exact occupied target. The command is admitted only on the captured ready-map
# appearance/owner. There is no source repair, nearest-reachable fallback or second send. The result
# proves one message was issued; observe world/battle state separately to prove its applied effect.
# CommandTimeoutMilliseconds extends only that sole command-result wait: this helper still builds one
# immutable URI and performs exactly one POST, with no timeout retry or refire.
function Move-Stack([string]$Role, [string]$Id,
                    [int]$FromX, [int]$FromY, [int]$X, [int]$Y,
                    [long]$ExpectedInstance = 0,
                    [long]$ExpectedAppearance = 0,
                    [int]$ExpectedMovement = -1,
                    [int]$CommandTimeoutMilliseconds = 0) {
    if ($ExpectedMovement -lt -1 -or $ExpectedMovement -gt 255) {
        throw 'move expected movement must be -1 (legacy/attack) or within 0..255'
    }
    if ($CommandTimeoutMilliseconds -ne 0 -and
        ($CommandTimeoutMilliseconds -lt 1000 -or $CommandTimeoutMilliseconds -gt 120000)) {
        throw 'move command-result timeout is outside 1000..120000 ms'
    }
    if (($ExpectedInstance -eq 0) -xor ($ExpectedAppearance -eq 0)) {
        throw 'a map action must carry both appearance and owner, or capture both together'
    }
    $binding = if ($ExpectedInstance -eq 0) {
        Get-MapActionTargetBinding $Role
    } else {
        if ($ExpectedInstance -lt 1 -or $ExpectedInstance -gt [uint32]::MaxValue -or
            $ExpectedAppearance -lt 1 -or $ExpectedAppearance -gt [uint32]::MaxValue) {
            throw 'map appearance/owner token is outside uint32'
        }
        [pscustomobject]@{
            Appearance = $ExpectedAppearance
            Instance = $ExpectedInstance
        }
    }
    $path = ("move?role={0}&id={1}&fromx={2}&fromy={3}&frommp={4}&x={5}&y={6}" +
        "&appearance={7}&instance={8}") -f
        [uri]::EscapeDataString($Role), [uri]::EscapeDataString($Id),
        $FromX, $FromY, $ExpectedMovement, $X, $Y,
        $binding.Appearance, $binding.Instance
    $httpTimeoutSeconds = 8
    if ($CommandTimeoutMilliseconds -ne 0) {
        $path += "&timeoutMs=$CommandTimeoutMilliseconds"
        $httpTimeoutSeconds = [math]::Ceiling($CommandTimeoutMilliseconds / 1000.0) + 10
    }
    [bool](script:Post $path $httpTimeoutSeconds)
}
# Flip a toggle button (e.g. DLG_BATTLE_A::TOG_AUTOBATTLE). invokeButton matches only buttons, so toggles
# (auto-battle, etc.) need their own verb. Returns the client's `found` flag.
function Invoke-Toggle([string]$Role, [string]$Dialog, [string]$Toggle,
                       [long]$ExpectedInstance = 0,
                       [long]$ExpectedAppearance = 0) {
    $binding = Resolve-ActionTargetBinding `
        $Role $Dialog $ExpectedInstance $ExpectedAppearance
    [bool](script:Post "toggle?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&tog=$([uri]::EscapeDataString($Toggle))&appearance=$($binding.Appearance)&instance=$($binding.Instance)")
}

function Enable-Toggle([string]$Role, [string]$Dialog, [string]$Toggle,
                       [long]$ExpectedInstance = 0,
                       [long]$ExpectedAppearance = 0) {
    $binding = Resolve-ActionTargetBinding `
        $Role $Dialog $ExpectedInstance $ExpectedAppearance
    [bool](script:Post "enable-toggle?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&tog=$([uri]::EscapeDataString($Toggle))&appearance=$($binding.Appearance)&instance=$($binding.Instance)")
}


# Wait for one exact source appearance and owner. Polling is observation only.
function Wait-ActionTargetBinding([string]$Role, [string]$Dialog, [int]$TimeoutSec = 60) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        $state = Get-RoleState $Role
        if ($state -and [bool]$state.dialogReady) {
            $rootInstance = $state.PSObject.Properties['dialogInstance']
            $rootAppearance = $state.PSObject.Properties['dialogAppearance']
            $targets = $state.PSObject.Properties['targets']
            if ($null -eq $rootInstance -or $null -eq $rootAppearance -or $null -eq $targets) {
                throw "$Role relay state omitted native action identity"
            }
            [long]$appearance = $rootAppearance.Value
            if ($appearance -gt 0 -and $appearance -eq [long]$rootInstance.Value) {
                $matches = @($targets.Value | Where-Object { [string]$_.dialog -eq $Dialog })
                if ($matches.Count -eq 1) {
                    [long]$owner = $matches[0].instance
                    if ($owner -lt 1 -or $owner -gt [uint32]::MaxValue) {
                        throw "$Role '$Dialog' owner token is outside uint32: $owner"
                    }
                    return [pscustomobject]@{ Appearance = $appearance; Instance = $owner }
                }
                if ($matches.Count -gt 1) {
                    throw "$Role has multiple ready '$Dialog' owners"
                }
            }
        }
        Start-Sleep -Milliseconds 400
    }
    return $null
}

# Capture one source publication, submit one mutation, then observe only. A timeout never authorizes
# a second click.
function Step-ToDialog([string]$Role, [string]$Dialog, [string]$Button,
                       [string]$ToDialog, [int]$TimeoutSec = 45) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    $binding = Wait-ActionTargetBinding $Role $Dialog $TimeoutSec
    if (-not $binding) { return $false }
    if (-not (Invoke-Button $Role $Dialog $Button $binding.Instance $binding.Appearance)) {
        return $false
    }
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-DialogReady $Role $ToDialog) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

# Generic map actions capture one exact ready owner; movement is explicitly toward a tile.
# Exact-source acceptance scenarios continue to use Move-Stack above.
function Move-StackToward([string]$Role, [string]$Id, [int]$X, [int]$Y) {
    $binding = Get-MapActionTargetBinding $Role
    [bool](script:Post "move-toward?role=$([uri]::EscapeDataString($Role))&id=$([uri]::EscapeDataString($Id))&x=$X&y=$Y&appearance=$($binding.Appearance)&instance=$($binding.Instance)&timeoutMs=30000" 40)
}

function Hire-Merc([string]$Role, [string]$Camp, [string]$Stack, [string]$Unit) {
    $binding = Get-ActionTargetBinding $Role (Get-Dialog $Role)
    [bool](script:Post "hire?role=$([uri]::EscapeDataString($Role))&camp=$([uri]::EscapeDataString($Camp))&stack=$([uri]::EscapeDataString($Stack))&unit=$([uri]::EscapeDataString($Unit))&appearance=$($binding.Appearance)&instance=$($binding.Instance)&timeoutMs=30000" 40)
}

function Move-GroupUnit([string]$Role, [string]$Stack, [int]$Src, [int]$Dst) {
    $binding = Get-ActionTargetBinding $Role (Get-Dialog $Role)
    [bool](script:Post "move-unit?role=$([uri]::EscapeDataString($Role))&stack=$([uri]::EscapeDataString($Stack))&src=$Src&dst=$Dst&appearance=$($binding.Appearance)&instance=$($binding.Instance)&timeoutMs=30000" 40)
}

function Dismiss-Unit([string]$Role, [string]$Stack, [string]$Unit) {
    $binding = Get-ActionTargetBinding $Role (Get-Dialog $Role)
    [bool](script:Post "dismiss?role=$([uri]::EscapeDataString($Role))&stack=$([uri]::EscapeDataString($Stack))&unit=$([uri]::EscapeDataString($Unit))&appearance=$($binding.Appearance)&instance=$($binding.Instance)&timeoutMs=30000" 40)
}
