#requires -Version 7.0
# Shared relay client for test dispatchers. Dot-source it, then Start-TestRelay + Launch-Agent,
# and drive an agent over the node relay with the helpers below. See ADDING-TESTS.md.

$script:RelayBase = "http://127.0.0.1:8077"

# Start the node relay; returns its process (or throws if it never answers /api/status).
function Start-TestRelay {
    param([string]$RelayJs = "$PSScriptRoot\..\relay\relay.js", [string]$LogDir = $env:TEMP)
    $log = Join-Path $LogDir "relay.out.log"
    $relay = Start-Process node -ArgumentList "`"$RelayJs`"" -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    for ($i = 0; $i -lt 25; $i++) {
        try { Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 2 | Out-Null; return $relay }
        catch { Start-Sleep -Milliseconds 300 }
    }
    throw "relay did not come up on $script:RelayBase"
}

# Launch a game instance as the DebugTest agent for <Role> (host/join/...). The default flags
# are the boot fixes + UI reporter + relay bridge (dispatcher-driven, no SELFNAV).
function Launch-Agent {
    param(
        [Parameter(Mandatory)][string]$Game,
        [Parameter(Mandatory)][string]$Role,
        [string[]]$Flags = @('SKIP_INTRO', 'BLACKSCREEN_FIX', 'UI_REPORTER', 'RELAY_BRIDGE')
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$Game\Discipl2.exe"; $psi.WorkingDirectory = $Game; $psi.UseShellExecute = $false
    foreach ($f in $Flags) { $psi.EnvironmentVariables["D2TESTDRV_$f"] = "1" }
    $psi.EnvironmentVariables["D2TESTDRV_ROLE"] = $Role
    return [System.Diagnostics.Process]::Start($psi)
}

# --- the dispatcher's eyes (scan) + hands (drive), all over the relay ---------
function State { try { (Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 3).roles } catch { $null } }
function Dlg([string]$role) { $s = State; if ($s -and $s.$role) { $s.$role.dialog } else { $null } }
function InvokeBtn([string]$role, [string]$dlg, [string]$btn) {
    try { Invoke-RestMethod "$script:RelayBase/api/invoke?role=$role&dlg=$dlg&btn=$btn" -Method POST -TimeoutSec 3 | Out-Null } catch {}
}
function SetSel([string]$role, [string]$dlg, [string]$lb, [int]$index) {
    try { Invoke-RestMethod "$script:RelayBase/api/select?role=$role&dlg=$dlg&lb=$lb&index=$index" -Method POST -TimeoutSec 3 | Out-Null } catch {}
}
function SetSpin([string]$role, [string]$dlg, [string]$spin, [int]$index) {
    try { Invoke-RestMethod "$script:RelayBase/api/spin?role=$role&dlg=$dlg&spin=$spin&index=$index" -Method POST -TimeoutSec 3 | Out-Null } catch {}
}
function SetEdit([string]$role, [string]$dlg, [string]$edit, [string]$text) {
    try { Invoke-RestMethod "$script:RelayBase/api/edit?role=$role&dlg=$dlg&edit=$edit&text=$([uri]::EscapeDataString($text))" -Method POST -TimeoutSec 3 | Out-Null } catch {}
}
function WaitDlg([string]$role, [string]$dialog, [int]$timeoutSec) {
    $t0 = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        if ((Dlg $role) -eq $dialog) { return $true }
        Start-Sleep -Milliseconds 400
    }
    return $false
}
# Click <btn> on <srcDlg> until <role> reaches <expect>. Re-fires every $RefireSec (kept above
# the longest UI-thread stall; the agent coalesces an in-flight duplicate). Returns $true/$false.
function StepTo([string]$role, [string]$srcDlg, [string]$btn, [string]$expect, [int]$timeoutSec, [int]$RefireSec = 12) {
    $t0 = Get-Date; InvokeBtn $role $srcDlg $btn; $lastFire = Get-Date
    while ((((Get-Date) - $t0).TotalSeconds) -lt $timeoutSec) {
        if ((Dlg $role) -eq $expect) { return $true }
        if (((Get-Date) - $lastFire).TotalSeconds -ge $RefireSec) { InvokeBtn $role $srcDlg $btn; $lastFire = Get-Date }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
