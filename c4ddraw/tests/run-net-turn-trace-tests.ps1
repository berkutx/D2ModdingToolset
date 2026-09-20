param([string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$BuildDirectory) { $BuildDirectory = Join-Path $taskRoot ('.diagnostics/net-turn-trace-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
if (!$taskBuild.StartsWith([IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must stay within workspace.' }
if (Test-Path -LiteralPath $taskBuild) { throw 'Use a new output directory.' }
New-Item -ItemType Directory -Path $taskBuild | Out-Null
function Write-Block([string]$Path, [string]$Start, [string]$End, [string]$Name) {
    $source = Get-Content -LiteralPath $Path -Raw
    $first = $source.IndexOf($Start, [StringComparison]::Ordinal)
    $last = $source.IndexOf($End, $first + $Start.Length, [StringComparison]::Ordinal)
    if ($first -lt 0 -or $last -lt 0) { throw 'Production source markers missing.' }
    $source.Substring($first, $last - $first) | Set-Content -LiteralPath (Join-Path $taskBuild $Name) -Encoding utf8
}
Write-Block (Join-Path $PSScriptRoot '../features/timerhost.cpp') 'int __fastcall hook_turnInfo(' '// CButtonInterf enabled flag' 'turninfo-trace.generated.h'
Write-Block (Join-Path $PSScriptRoot '../features/netturntrace.cpp') 'using Disconnect =' 'bool disconnectEntryMatches()' 'disconnect-trace.generated.h'
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
$taskTest = Join-Path $PSScriptRoot 'net_turn_trace_tests.cpp'
$taskFeatures = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../features'))
$taskCommand = 'call "{0}" && cl.exe /nologo /EHsc /O2 /W4 /I"{1}" /I"{2}" "{3}" /Fe:net_turn_trace_tests.exe /Fo:net_turn_trace_tests.obj /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"' -f $taskVcvars,$taskBuild,$taskFeatures,$taskTest
$taskStart = [Diagnostics.ProcessStartInfo]::new()
$taskStart.FileName = $env:ComSpec
$taskStart.Arguments = '/d /s /c "' + $taskCommand + '"'
$taskStart.WorkingDirectory = $taskBuild
$taskStart.UseShellExecute = $false
$taskStart.CreateNoWindow = $true
$taskStart.RedirectStandardOutput = $true
$taskStart.RedirectStandardError = $true
$taskProcess = [Diagnostics.Process]::Start($taskStart)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskStdout.Result + $taskStderr.Result | Set-Content -LiteralPath (Join-Path $taskBuild 'build.log')
if ($taskProcess.ExitCode) { Get-Content -LiteralPath (Join-Path $taskBuild 'build.log'); throw 'Compilation failed.' }
& (Join-Path $taskBuild 'net_turn_trace_tests.exe') | Tee-Object -FilePath (Join-Path $taskBuild 'test.log')
if ($LASTEXITCODE) { throw 'Trace contract checks failed.' }
