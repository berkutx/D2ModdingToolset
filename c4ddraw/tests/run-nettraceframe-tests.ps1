param([string]$BuildDirectory)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$BuildDirectory) {
    $BuildDirectory = Join-Path $taskRoot ('.diagnostics/nettraceframe-tests/run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
$taskAllowed = [IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\'
if (!$taskBuild.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test output must stay in this workspace.'
}
if (Test-Path -LiteralPath $taskBuild) {
    throw 'Use a new output directory to preserve evidence.'
}

$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $taskVswhere)) { throw 'Visual Studio Installer vswhere.exe not found.' }
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if ($LASTEXITCODE -or !$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) { throw 'Visual Studio vcvars32.bat not found.' }
$taskTest = Join-Path $PSScriptRoot 'nettraceframe_tests.cpp'
$taskHeader = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../features/nettraceframe.h'))
New-Item -ItemType Directory -Path $taskBuild | Out-Null

# Normalize environment names for hosts that expose both Path and PATH. No
# native subprocess opens a visible console, including the test executable.
$taskEnvironment = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
    $taskEnvironment[$entry.Key] = $entry.Value
}
function Invoke-HiddenTestProcess([string]$Executable, [string]$Arguments, [string]$LogName) {
    $taskStart = [Diagnostics.ProcessStartInfo]::new()
    $taskStart.FileName = $Executable
    $taskStart.Arguments = $Arguments
    $taskStart.WorkingDirectory = $taskBuild
    $taskStart.UseShellExecute = $false
    $taskStart.CreateNoWindow = $true
    $taskStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $taskStart.RedirectStandardOutput = $true
    $taskStart.RedirectStandardError = $true
    $taskStart.EnvironmentVariables.Clear()
    foreach ($entry in $taskEnvironment.GetEnumerator()) {
        $taskStart.EnvironmentVariables[$entry.Key] = $entry.Value
    }
    $taskProcess = [Diagnostics.Process]::Start($taskStart)
    try {
        $taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
        $taskStderr = $taskProcess.StandardError.ReadToEndAsync()
        $taskProcess.WaitForExit()
        $taskOutput = $taskStdout.Result + $taskStderr.Result
        $taskOutput | Set-Content -LiteralPath (Join-Path $taskBuild $LogName) -Encoding utf8
        if ($taskProcess.ExitCode) {
            Write-Output $taskOutput
            throw ('{0} failed with exit code {1}.' -f $LogName, $taskProcess.ExitCode)
        }
    } finally {
        $taskProcess.Dispose()
    }
}

$taskCommand = 'call "{0}" && cl.exe /nologo /EHsc /std:c++14 /O2 /W4 /WX "{1}" /Fe:nettraceframe_tests.exe /Fo:nettraceframe_tests.obj /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"' -f $taskVcvars,$taskTest
Invoke-HiddenTestProcess $env:ComSpec ('/d /s /c "' + $taskCommand + '"') 'build.log'
Invoke-HiddenTestProcess (Join-Path $taskBuild 'nettraceframe_tests.exe') '' 'test.log'
Get-FileHash -Algorithm SHA256 -LiteralPath $taskTest,$taskHeader |
    Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskBuild 'hashes.json') -Encoding utf8
Get-Content -LiteralPath (Join-Path $taskBuild 'test.log')
Write-Output ('Evidence: ' + $taskBuild)
