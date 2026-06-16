#requires -Version 7.0
# Capture a game window to PNG via PrintWindow. Unlike CopyFromScreen (which needs the
# interactive screen DC and throws "handle is invalid" from a non-interactive/headless
# session), PrintWindow renders through the window's own DC; PW_RENDERFULLCONTENT (2) grabs
# DWM-composited GL/ddraw content even when the window is occluded. Dot-source this file.

if (-not ([System.Management.Automation.PSTypeName]'D2.Cap').Type) {
    Add-Type -Namespace D2 -Name Cap -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
}
Add-Type -AssemblyName System.Drawing

function Capture-GameWindow {
    param([System.Diagnostics.Process]$Proc, [string]$Path)
    if (-not $Proc -or $Proc.HasExited) { return $false }
    $Proc.Refresh(); $h = $Proc.MainWindowHandle
    if ($h -eq [System.IntPtr]::Zero) { return $false }
    $r = New-Object D2.Cap+RECT
    [void][D2.Cap]::GetWindowRect($h, [ref]$r)
    $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($w -le 0 -or $ht -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][D2.Cap]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    return $true
}
