param([string]$BuildDirectory, [string]$HostSource)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$HostSource) { $HostSource = Join-Path $PSScriptRoot '../features/timerhost.cpp' }
if (!$BuildDirectory) {
    $BuildDirectory = Join-Path $taskRoot ('.diagnostics/timer-end-day-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
if (!$taskBuild.StartsWith([IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test output must stay within this workspace.'
}
if (Test-Path -LiteralPath $taskBuild) { throw 'Use a new build directory; existing evidence is preserved.' }
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource = Get-Content -LiteralPath $HostSource -Raw
function Get-SourceBlock([string]$Start, [string]$End) {
    $startIndex = $taskSource.IndexOf($Start, [StringComparison]::Ordinal)
    if ($startIndex -lt 0) { throw "Production source marker not found: $Start" }
    $endIndex = $taskSource.IndexOf($End, $startIndex + $Start.Length, [StringComparison]::Ordinal)
    if ($endIndex -lt 0) { throw "Production source marker not found: $End" }
    $taskSource.Substring($startIndex, $endIndex - $startIndex)
}
$taskState = Get-SourceBlock 'struct State' 'SRWLOCK g_battleStateLock'
$taskState += Get-SourceBlock 'struct PhaseGameLockSnapshot' 'PhaseGameLockSnapshot phaseGameLockSnapshot()'
$taskState | Set-Content -LiteralPath (Join-Path $taskBuild 'timerhost-state.generated.h') -Encoding utf8
$taskPump = Get-SourceBlock 'void clearPendingActions()' 'void clearForcedAutoLatch()'
if ($taskSource.Contains('void pumpStrategicEndDay(int myTurn)')) {
    $taskPump += Get-SourceBlock 'void pumpStrategicEndDay(int myTurn)' '// Verified Russobit/MNS battle layout'
}
$taskPump += Get-SourceBlock 'extern "C" int timerhost_end_day(void)' 'extern "C" uint32_t timerhost_begin_turn_ack_serial(void)'
$taskPump += Get-SourceBlock 'extern "C" void timerhost_pump(void)' '// Install (called from featuremenu_install'
$taskPump | Set-Content -LiteralPath (Join-Path $taskBuild 'timerhost-pump.generated.h') -Encoding utf8
$taskTest = Join-Path $PSScriptRoot 'timer_end_day_tests.cpp'
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
$taskCommand = 'call "{0}" && cl.exe /nologo /EHsc /O2 /W4 /I"{1}" "{2}" /Fe:timer_end_day_tests.exe /Fo:timer_end_day_tests.obj /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"' -f $taskVcvars,$taskBuild,$taskTest
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
if ($taskProcess.ExitCode) { Get-Content -LiteralPath (Join-Path $taskBuild 'build.log'); throw 'Timer host replay compilation failed.' }
Get-FileHash -Algorithm SHA256 -LiteralPath $HostSource,$taskTest |
    Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskBuild 'hashes.json')
& (Join-Path $taskBuild 'timer_end_day_tests.exe') | Tee-Object -FilePath (Join-Path $taskBuild 'test.log')
if ($LASTEXITCODE) { throw 'Timer host replay checks failed.' }
