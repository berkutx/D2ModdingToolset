param([string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$BuildDirectory) {
    $BuildDirectory = Join-Path $taskRoot ('.diagnostics/timer-drag-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
if (!$taskBuild.StartsWith([IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test output must stay within this workspace.'
}
if (Test-Path -LiteralPath $taskBuild) { throw 'Use a new build directory; existing evidence is preserved.' }
New-Item -ItemType Directory -Path $taskBuild | Out-Null

# Exercise the exact production depth guards, including nested callbacks and SEH unwind. The
# fixture supplies only the native callback bodies; no live game, input injection or hooks are used.
function Get-SourceFunction([string]$Text, [string]$Signature) {
    $start = $Text.IndexOf($Signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Production source marker not found: $Signature" }
    $open = $Text.IndexOf('{', $start)
    $depth = 1
    $end = $open + 1
    while ($depth -gt 0 -and $end -lt $Text.Length) {
        if ($Text[$end] -eq '{') { ++$depth }
        if ($Text[$end] -eq '}') { --$depth }
        ++$end
    }
    if ($depth) { throw "Unclosed production source function: $Signature" }
    $Text.Substring($start, $end - $start)
}
$taskMenuPath = Join-Path $PSScriptRoot '../features/featuremenu.cpp'
$taskCursorPath = Join-Path $PSScriptRoot '../features/cursorcapture.cpp'
$taskDragPath = Join-Path $PSScriptRoot '../features/timerdrag.h'
$taskHostPath = Join-Path $PSScriptRoot '../features/timerhost.cpp'
$taskMenu = Get-Content -LiteralPath $taskMenuPath -Raw
$taskCursor = Get-Content -LiteralPath $taskCursorPath -Raw
$taskHost = Get-Content -LiteralPath $taskHostPath -Raw
$taskGuards = Get-SourceFunction $taskMenu 'LRESULT callNativeGameWndProc('
$taskGuards += "`r`n" + (Get-SourceFunction $taskMenu 'extern "C" int featuremenu_native_dispatch_active(')
$taskGuards += "`r`n" + (Get-SourceFunction $taskCursor 'void __fastcall cursorDrawThunk(')
$taskGuards += "`r`n" + (Get-SourceFunction $taskCursor 'extern "C" int cursorcapture_draw_active(')
$taskGuards += "`r`n" + (Get-SourceFunction $taskHost 'bool isUserPtr(')
$taskGuards += "`r`n" + (Get-SourceFunction $taskHost 'bool executableAddress(')
$taskGuards | Set-Content -LiteralPath (Join-Path $taskBuild 'native-ui-depth.generated.h') -Encoding utf8

$taskTest = Join-Path $PSScriptRoot 'timer_drag_tests.cpp'
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
$taskCommand = 'call "{0}" && cl.exe /nologo /EHsc /O2 /W4 /I"{1}" "{2}" /Fe:timer_drag_tests.exe /Fo:timer_drag_tests.obj /link user32.lib /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"' -f $taskVcvars,$taskBuild,$taskTest
$taskEnvironment = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $taskEnvironment[$entry.Key] = $entry.Value }
$taskStart = [Diagnostics.ProcessStartInfo]::new()
$taskStart.FileName = $env:ComSpec
$taskStart.Arguments = '/d /s /c "' + $taskCommand + '"'
$taskStart.WorkingDirectory = $taskBuild
$taskStart.UseShellExecute = $false
$taskStart.CreateNoWindow = $true
$taskStart.RedirectStandardOutput = $true
$taskStart.RedirectStandardError = $true
$taskStart.Environment.Clear()
foreach ($entry in $taskEnvironment.GetEnumerator()) { $taskStart.Environment[$entry.Key] = $entry.Value }
$taskProcess = [Diagnostics.Process]::Start($taskStart)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskStdout.Result + $taskStderr.Result | Set-Content -LiteralPath (Join-Path $taskBuild 'build.log')
if ($taskProcess.ExitCode) { Get-Content -LiteralPath (Join-Path $taskBuild 'build.log'); throw 'Timer drag replay compilation failed.' }
Get-FileHash -Algorithm SHA256 -LiteralPath $taskDragPath,$taskMenuPath,$taskCursorPath,$taskHostPath,$taskTest |
    Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskBuild 'hashes.json')
& (Join-Path $taskBuild 'timer_drag_tests.exe') | Tee-Object -FilePath (Join-Path $taskBuild 'test.log')
if ($LASTEXITCODE) { throw 'Timer drag replay checks failed.' }
