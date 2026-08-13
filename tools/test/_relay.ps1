#requires -Version 7.0
# Shared toolkit for the relay-driven tests. Dot-source it, then:
#   $GameDir = Resolve-GameDir $GameDir        # from -GameDir, else tools/test/test.config.psd1
#   $relay   = Start-TestRelay                 # node relay.js (the host + joiner connect to it)
#   $client  = Start-GameClient -GameDir $GameDir -Role host
# and drive each client over the relay with the verb-noun commands below. See README.md.

$script:RelayHttpHost = if ($env:D2_RELAY_HTTP_HOST -and $env:D2_RELAY_HTTP_HOST -notin @('0.0.0.0', '::')) {
    $env:D2_RELAY_HTTP_HOST
} else {
    '127.0.0.1'
}
$script:RelayHttpPort = if ($env:D2_RELAY_HTTP_PORT) { [int]$env:D2_RELAY_HTTP_PORT } else { 8077 }
if ($script:RelayHttpPort -lt 1 -or $script:RelayHttpPort -gt 65535) {
    throw "D2_RELAY_HTTP_PORT must be between 1 and 65535"
}
$script:RelayBase = "http://$($script:RelayHttpHost):$($script:RelayHttpPort)"
$script:RelayJs   = "$PSScriptRoot\..\relay\relay.js" # same for everyone, NOT machine-specific
$script:RussobitExeSize = 4187648
$script:RussobitExeSha256 = '1375CDEF09EC470EE64FE5693FB734D7C69FB215212311D997F792B258A642EB'

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

# ---- relay + clients --------------------------------------------------------------------------
# Start the node relay; return its process (throws if it never answers /api/status).
function Start-TestRelay {
    param([string]$LogDir = $env:TEMP)

    # A successful response here is never ours: this function has not launched its child yet.
    # Refuse to mistake a stale relay for the new one.
    try {
        $existing = Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 1
        throw "relay endpoint $script:RelayBase is already occupied (instanceId=$($existing.instanceId))"
    } catch {
        if ($_.Exception.Message -like 'relay endpoint * is already occupied*') { throw }
    }

    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    $instanceId = [guid]::NewGuid().ToString('N')
    $log = Join-Path $LogDir "relay-$instanceId.out.log"
    $errLog = "$log.err"
    $oldInstanceId = [Environment]::GetEnvironmentVariable('D2_RELAY_INSTANCE_ID', 'Process')
    try {
        [Environment]::SetEnvironmentVariable('D2_RELAY_INSTANCE_ID', $instanceId, 'Process')
        $relay = Start-Process node -ArgumentList "`"$script:RelayJs`"" -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $log -RedirectStandardError $errLog
    } finally {
        [Environment]::SetEnvironmentVariable('D2_RELAY_INSTANCE_ID', $oldInstanceId, 'Process')
    }

    for ($i = 0; $i -lt 25; $i++) {
        if ($relay.HasExited) {
            $stderr = if (Test-Path -LiteralPath $errLog) {
                (Get-Content -LiteralPath $errLog -Tail 20) -join [Environment]::NewLine
            } else { '' }
            throw "relay process exited before readiness (code=$($relay.ExitCode)). $stderr"
        }
        try {
            $status = Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 2
            if ($status.instanceId -ceq $instanceId) {
                $relay | Add-Member -NotePropertyName RelayInstanceId -NotePropertyValue $instanceId
                $relay | Add-Member -NotePropertyName RelayLogPath -NotePropertyValue $log
                return $relay
            }
        } catch {}
        Start-Sleep -Milliseconds 300
    }
    Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
    throw "owned relay instance $instanceId did not come up on $script:RelayBase (logs: $log, $errLog)"
}

# Launch a game instance with the debug test-harness DLL for <Role> (host/join/...). Default flags are the
# boot fixes + UI reporter + relay bridge (dispatcher-driven; no built-in self-nav).
function Start-GameClient {
    param(
        [Parameter(Mandatory)][string]$GameDir,
        [Parameter(Mandatory)][string]$Role,
        [string[]]$Flags = @('SKIP_INTRO', 'BLACKSCREEN_FIX', 'UI_REPORTER', 'WORLD', 'RELAY_BRIDGE'),
        [switch]$LobbyChat
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$GameDir\Discipl2.exe"; $psi.WorkingDirectory = $GameDir; $psi.UseShellExecute = $false
    foreach ($f in $Flags) { $psi.EnvironmentVariables["D2TESTDRV_$f"] = "1" }
    if ($LobbyChat) { $psi.EnvironmentVariables["D2TESTDRV_LOBBY_CHAT"] = "1" }
    $psi.EnvironmentVariables["D2TESTDRV_ROLE"] = $Role
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
    try { return (Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 3).roles } catch { return $null }
}
# One role's state object (or $null): .dialog, .widgets, .connected, .reachedStrategic, .sawBeginTurn,
# .strategicActionReady (native clientTakesTurn observation from the world reporter).
function Get-RoleState([string]$Role) {
    $s = Get-RelayState; if ($s) { return $s.$Role } else { return $null }
}
# The current dialog name for a role (or $null).
function Get-Dialog([string]$Role) {
    $r = Get-RoleState $Role; if ($r) { return $r.dialog } else { return $null }
}
# The rich UI snapshot for a role: { role, dialog, widgets:[{name,type,state}] }. `type` is
# button/listbox/spin/edit/text/picture/...; `state` carries enabled / selected+total / index+text / text.
function Get-GameUi([string]$Role) {
    try { return Invoke-RestMethod "$script:RelayBase/api/ui?role=$([uri]::EscapeDataString($Role))" -TimeoutSec 3 } catch { return $null }
}
# The world snapshot for a role: { role, day, strategicActionReady,
# players:[{id,relation,human,race,gold,...mana}],
# stacks:[{id,x,y,owner,relation,movement,units,hp,subrace,inside}] }. `hp` is the group's total current
# HP, `inside` is true for a garrisoned stack. Populated only once a scenario is loaded.
function Get-World([string]$Role) {
    try { return Invoke-RestMethod "$script:RelayBase/api/world?role=$([uri]::EscapeDataString($Role))" -TimeoutSec 3 } catch { return $null }
}
# Recent custom-lobby chat for a role: [{t,sender,text},...]. Start the client with -LobbyChat
# (sets D2TESTDRV_LOBBY_CHAT=1) or set that environment variable explicitly.
function Get-LobbyChat([string]$Role) {
    try { return (Invoke-RestMethod "$script:RelayBase/api/lobby/chat?role=$([uri]::EscapeDataString($Role))" -TimeoutSec 3).messages } catch { return $null }
}
# Convenience views over the world snapshot: the local player's resources, the map's stacks, the
# neutral mercenary camps (each with a hireable roster), and the treasure chests / bags lying around.
function Get-Resources([string]$Role) { (Get-World $Role).players | Where-Object { $_.relation -eq 'self' } | Select-Object -First 1 }
function Get-Stacks([string]$Role) { (Get-World $Role).stacks }
function Get-Camps([string]$Role) { (Get-World $Role).camps }
function Get-Bags([string]$Role) { (Get-World $Role).bags }
# Wait until <Dialog> is the current dialog for <Role>; $true if it appeared, $false on timeout.
function Wait-Dialog([string]$Role, [string]$Dialog, [int]$TimeoutSec = 60) {
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $TimeoutSec) {
        if ((Get-Dialog $Role) -eq $Dialog) { return $true }
        Start-Sleep -Milliseconds 400
    }
    return $false
}

# ---- the dispatcher's hands (drive UI) ---------------------------------------------------------
# Each command returns the client's `found` flag: $true if the addressed dialog and widget were
# resolved (the action ran), $false if not (a wrong name, or the dialog is not open). The relay
# holds the request until the client answers, so a command to an absent target is reported, not
# silently dropped. The timeout exceeds the relay's own wait so a slow answer is not cut off.
function script:Post([string]$Path) {
    try { return (Invoke-RestMethod "$script:RelayBase/api/ui/$Path" -Method POST -TimeoutSec 8).found } catch { return $false }
}
function Invoke-Button([string]$Role, [string]$Dialog, [string]$Button) {
    [bool](script:Post "invoke?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&btn=$([uri]::EscapeDataString($Button))")
}
function Set-ListSelection([string]$Role, [string]$Dialog, [string]$ListBox, [int]$Index) {
    [bool](script:Post "select?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&lb=$([uri]::EscapeDataString($ListBox))&index=$Index")
}
function Set-SpinOption([string]$Role, [string]$Dialog, [string]$Spin, [int]$Index) {
    [bool](script:Post "spin?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&spin=$([uri]::EscapeDataString($Spin))&index=$Index")
}
function Set-EditText([string]$Role, [string]$Dialog, [string]$Edit, [string]$Text) {
    [bool](script:Post "edit?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&edit=$([uri]::EscapeDataString($Edit))&text=$([uri]::EscapeDataString($Text))")
}
# Move stack <Id> (from Get-Stacks) toward tile (X, Y). The agent builds the path with the game's own
# cost/passability and issues sendStackMoveMsg, the same call a map click makes. Returns the client's
# `found` flag: $true if the move was ISSUED (own stack, our turn, some reachable tile toward the
# target). It does NOT confirm the exact tile was reached: an unreachable target moves the stack as far
# as it can, so verify the outcome (position, a started battle) via Get-World afterward.
function Move-Stack([string]$Role, [string]$Id, [int]$X, [int]$Y) {
    [bool](script:Post "move?role=$([uri]::EscapeDataString($Role))&id=$([uri]::EscapeDataString($Id))&x=$X&y=$Y")
}
# Buy the mercenary <Unit> from merc camp <Camp> into stack <Stack>'s first fitting free slot (testdrv
# worldactions::hireMerc, which sends the engine's CSiteBuyUnitMsg). <Camp>/<Unit> come straight from a
# Get-Camps entry (camp .id and units[].impl). Returns `found`: $true if the hire message was sent (own
# stack, our turn, a free slot). The host applies + replicates, so verify via Get-World slots[] on
# EITHER role once it settles.
function Hire-Merc([string]$Role, [string]$Camp, [string]$Stack, [string]$Unit) {
    [bool](script:Post "hire?role=$([uri]::EscapeDataString($Role))&camp=$([uri]::EscapeDataString($Camp))&stack=$([uri]::EscapeDataString($Stack))&unit=$([uri]::EscapeDataString($Unit))")
}
# Move the unit at slot <Src> to slot <Dst> within stack <Stack>'s formation (testdrv
# worldactions::moveGroupUnit, which sends the engine's CStackSwapUnitMsg). An EMPTY <Dst> is a plain
# move (the <Src> cell empties); an OCCUPIED <Dst> is a swap (e.g. put a ranged unit in the back line, a
# melee in the front). <Src> must hold a unit. Slots are 0..5 (even = front, odd = back); a big unit
# sits on its front/even cell. Returns `found`: $true if the message was sent (own stack, our turn,
# <Src> occupied). The host applies + replicates; verify via Get-World slots[] on EITHER role.
function Move-GroupUnit([string]$Role, [string]$Stack, [int]$Src, [int]$Dst) {
    [bool](script:Post "move-unit?role=$([uri]::EscapeDataString($Role))&stack=$([uri]::EscapeDataString($Stack))&src=$Src&dst=$Dst")
}
# Dismiss the non-leader unit <Unit> from stack <Stack>, freeing its slot (testdrv
# worldactions::dismissUnit, which sends the engine's CStackDismissUnitMsg). The LEADER is rejected by
# the DLL (a different message disbands the stack). Use it to drop a low-value unit so a more valuable or
# 2-slot one fits. Returns `found`: $true if the message was sent (own stack, our turn, a non-leader
# group member). The host removes it + replicates; verify via Get-World slots[]/units on EITHER role.
function Dismiss-Unit([string]$Role, [string]$Stack, [string]$Unit) {
    [bool](script:Post "dismiss?role=$([uri]::EscapeDataString($Role))&stack=$([uri]::EscapeDataString($Stack))&unit=$([uri]::EscapeDataString($Unit))")
}
# Flip a toggle button (e.g. DLG_BATTLE_A::TOG_AUTOBATTLE). invokeButton matches only buttons, so toggles
# (auto-battle, etc.) need their own verb. Returns the client's `found` flag.
function Invoke-Toggle([string]$Role, [string]$Dialog, [string]$Toggle) {
    [bool](script:Post "toggle?role=$([uri]::EscapeDataString($Role))&dlg=$([uri]::EscapeDataString($Dialog))&tog=$([uri]::EscapeDataString($Toggle))")
}

# Click <Button> on <Dialog> until <Role> reaches <ToDialog>. Returns $true on arrival, $false on
# timeout. The click is retried every <RefireSec> for as long as the target dialog has not appeared;
# Invoke-Button reports whether each attempt found its button, so a step that never resolves is
# visible (under -Verbose) rather than a silent spin. <RefireSec> stays above the longest UI-thread
# stall so the client coalesces a duplicate during a blocking send.
function Step-ToDialog([string]$Role, [string]$Dialog, [string]$Button, [string]$ToDialog, [int]$TimeoutSec = 45, [int]$RefireSec = 12) {
    $t0 = Get-Date; $lastFire = (Get-Date).AddSeconds(-$RefireSec)
    while ((((Get-Date) - $t0).TotalSeconds) -lt $TimeoutSec) {
        if ((Get-Dialog $Role) -eq $ToDialog) { return $true }
        if (((Get-Date) - $lastFire).TotalSeconds -ge $RefireSec) {
            if (-not (Invoke-Button $Role $Dialog $Button)) { Write-Verbose "[step] $Role $Dialog::$Button not found yet" }
            $lastFire = Get-Date
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
