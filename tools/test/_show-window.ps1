#requires -Version 7.0
# Shared helper: bring a kept game window to the foreground so it actually renders
# (the GOG GL/ddraw wrapper pauses painting while the window is occluded). Used by
# the visual test scripts when they leave the game running for manual poking.
# Dot-source:  . "$PSScriptRoot\_show-window.ps1"

if (-not ([System.Management.Automation.PSTypeName]'D2.Win').Type) {
    Add-Type -Namespace D2 -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int n);
[DllImport("user32.dll")] public static extern bool SwitchToThisWindow(System.IntPtr h, bool b);
[DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
'@
}

function Show-GameWindow {
    param([System.Diagnostics.Process]$Proc)
    if (-not $Proc -or $Proc.HasExited) { return }
    $h = [System.IntPtr]::Zero
    for ($i = 0; $i -lt 20; $i++) {
        try { $Proc.Refresh(); $h = $Proc.MainWindowHandle } catch { $h = [System.IntPtr]::Zero }
        if ($h -ne [System.IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 150
    }
    if ($h -ne [System.IntPtr]::Zero) {
        [D2.Win]::AllowSetForegroundWindow(-1) | Out-Null   # ASFW_ANY
        [D2.Win]::ShowWindow($h, 9) | Out-Null              # SW_RESTORE
        [D2.Win]::SetForegroundWindow($h) | Out-Null
        [D2.Win]::SwitchToThisWindow($h, $true) | Out-Null
    }
}
