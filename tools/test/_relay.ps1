#requires -Version 7.0
# Shared toolkit for the relay-driven tests. Dot-source it, then:
#   $GameDir = Resolve-GameDir $GameDir        # from -GameDir, else tools/test/test.config.psd1
#   $relay   = Start-TestRelay                 # node relay.js (the host + joiner connect to it)
#   $client  = Start-GameClient -GameDir $GameDir -Role host
# and drive each client over the relay with the verb-noun commands below. See README.md.

$script:RelayBase = "http://127.0.0.1:8077"           # same for everyone, NOT machine-specific
$script:RelayJs   = "$PSScriptRoot\..\relay\relay.js" # same for everyone, NOT machine-specific

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
    return $GameDir
}

# ---- relay + clients --------------------------------------------------------------------------
# Start the node relay; return its process (throws if it never answers /api/status).
function Start-TestRelay {
    param([string]$LogDir = $env:TEMP)
    $log = Join-Path $LogDir "relay.out.log"
    $relay = Start-Process node -ArgumentList "`"$script:RelayJs`"" -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    for ($i = 0; $i -lt 25; $i++) {
        try { Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 2 | Out-Null; return $relay }
        catch { Start-Sleep -Milliseconds 300 }
    }
    throw "relay did not come up on $script:RelayBase"
}

# Launch a game instance as the DebugTest client for <Role> (host/join/...). Default flags are the
# boot fixes + UI reporter + relay bridge (dispatcher-driven; no built-in self-nav).
function Start-GameClient {
    param(
        [Parameter(Mandatory)][string]$GameDir,
        [Parameter(Mandatory)][string]$Role,
        [string[]]$Flags = @('SKIP_INTRO', 'BLACKSCREEN_FIX', 'UI_REPORTER', 'WORLD', 'RELAY_BRIDGE')
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$GameDir\Discipl2.exe"; $psi.WorkingDirectory = $GameDir; $psi.UseShellExecute = $false
    foreach ($f in $Flags) { $psi.EnvironmentVariables["D2TESTDRV_$f"] = "1" }
    $psi.EnvironmentVariables["D2TESTDRV_ROLE"] = $Role
    return [System.Diagnostics.Process]::Start($psi)
}

# Resolve a random-scenario template's listbox index BY NAME (e.g. 'Diligence'). The generator lists
# Templates\*.lua in case-insensitive filename order (std::filesystem), so we mirror that sort; this
# avoids a brittle hardcoded index that breaks when templates are added or removed.
function Resolve-TemplateIndex([string]$GameDir, [string]$Name) {
    $files = @(Get-ChildItem (Join-Path $GameDir 'Templates') -Filter *.lua -ErrorAction SilentlyContinue |
        Sort-Object { $_.Name.ToUpperInvariant() })
    for ($i = 0; $i -lt $files.Count; $i++) {
        if ($files[$i].BaseName -ieq $Name) { return $i }
    }
    throw "template '$Name' not found in $GameDir\Templates"
}

# ---- the dispatcher's eyes (read UI) ----------------------------------------------------------
# All roles + their live state in one call: { host = {connected,dialog,widgets,...}, join = {...} }.
function Get-RelayState {
    try { return (Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 3).roles } catch { return $null }
}
# One role's state object (or $null): .dialog, .widgets, .connected, .reachedStrategic, .sawBeginTurn.
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
# The world snapshot for a role: { role, day, players:[{id,relation,human,race,gold,...mana}],
# stacks:[{id,x,y,owner,relation,movement,units,subrace}] }. Populated only once a scenario is loaded.
function Get-World([string]$Role) {
    try { return Invoke-RestMethod "$script:RelayBase/api/world?role=$([uri]::EscapeDataString($Role))" -TimeoutSec 3 } catch { return $null }
}
# Convenience views over the world snapshot: the local player's resources, and the map's stacks.
function Get-Resources([string]$Role) { (Get-World $Role).players | Where-Object { $_.relation -eq 'self' } | Select-Object -First 1 }
function Get-Stacks([string]$Role) { (Get-World $Role).stacks }
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
