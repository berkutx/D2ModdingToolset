[CmdletBinding()]
param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vs) { throw 'Install Visual Studio C++ build tools to build the development tests.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'

foreach ($project in @('c4ddraw\plugins\unitinfo\unitinfo.vcxproj', 'c4ddraw\plugins\unitinfo\tests\localbridge_test_host.vcxproj')) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $msbuild
    $start.WorkingDirectory = $root
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    foreach ($argument in @($project, '/p:Configuration=Release', '/p:Platform=Win32', '/m:1', '/nologo', '/verbosity:minimal')) {
        $start.ArgumentList.Add($argument)
    }
    # Some desktop environments contain both Path and PATH. Normalize only the child.
    $normalized = @{}
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $normalized[$entry.Key] = [string]$entry.Value }
    $start.Environment.Clear()
    foreach ($key in $normalized.Keys) { $start.Environment[$key] = $normalized[$key] }
    $process = [Diagnostics.Process]::Start($start)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "Native build failed: $project" }
}
if (-not $BuildOnly) {
    & (Join-Path $root 'c4ddraw\plugins\unitinfo\tests\bin\localbridge_test_host.exe') --check-sliced-state
    if ($LASTEXITCODE -ne 0) { throw 'Sliced capture eligibility regression failed.' }
    # Node runs the developer test client only. The plugin includes no Node dependency.
    & node --test (Join-Path $root 'twitch-extension\test\native-bridge.integration.mjs')
    if ($LASTEXITCODE -ne 0) { throw 'Native transport tests failed.' }
}
