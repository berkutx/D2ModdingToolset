#requires -Version 7.0
# Capture a game window to PNG via PrintWindow. CopyFromScreen needs the interactive screen DC and
# throws "handle is invalid" from a non-interactive/headless session; PrintWindow renders through the
# window's own DC and PW_RENDERFULLCONTENT (2) grabs DWM-composited GL/ddraw content. The DisciplesGL
# wrapper is a menu-bar frame window with an inner GL surface, and which HWND the GL content composites
# into varies, so we grab EVERY visible window of the process and keep the least-black one. Dot-source.

if (-not ([System.Management.Automation.PSTypeName]'D2.Cap').Type) {
    Add-Type -Namespace D2 -Name Cap -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(System.IntPtr h, EnumWindowsProc cb, System.IntPtr p);
public delegate bool EnumWindowsProc(System.IntPtr h, System.IntPtr p);
public struct RECT { public int Left, Top, Right, Bottom; }
public static System.Collections.Generic.List<System.IntPtr> Children(System.IntPtr parent) {
    var list = new System.Collections.Generic.List<System.IntPtr>();
    EnumChildWindows(parent, (h, p) => { list.Add(h); return true; }, System.IntPtr.Zero);
    return list;
}
'@
}
Add-Type -AssemblyName System.Drawing

# Grab one HWND via PrintWindow; return @{ bmp; score } where score = count of non-near-black samples.
function script:GrabWindow([System.IntPtr]$h) {
    $r = New-Object D2.Cap+RECT
    [void][D2.Cap]::GetWindowRect($h, [ref]$r)
    $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($w -lt 200 -or $ht -lt 200) { return $null }
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][D2.Cap]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $score = 0
    for ($x = 0; $x -lt $w; $x += 24) { for ($y = 0; $y -lt $ht; $y += 24) {
        $c = $bmp.GetPixel($x, $y); if ($c.R + $c.G + $c.B -gt 40) { $score++ }
    } }
    return @{ bmp = $bmp; score = $score }
}

function Capture-GameWindow {
    param([System.Diagnostics.Process]$Proc, [string]$Path)
    if (-not $Proc -or $Proc.HasExited) { return $false }
    $Proc.Refresh(); $top = $Proc.MainWindowHandle
    if ($top -eq [System.IntPtr]::Zero) { return $false }
    $cands = @($top) + ([D2.Cap]::Children($top))
    $best = $null
    foreach ($h in $cands) {
        if (-not [D2.Cap]::IsWindowVisible($h)) { continue }
        $g = script:GrabWindow $h
        if (-not $g) { continue }
        if (-not $best -or $g.score -gt $best.score) { if ($best) { $best.bmp.Dispose() }; $best = $g }
        else { $g.bmp.Dispose() }
    }
    if (-not $best) { return $false }
    $best.bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png); $best.bmp.Dispose()
    return ($best.score -gt 0)
}
