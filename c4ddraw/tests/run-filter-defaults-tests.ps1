$ErrorActionPreference = 'Stop'
$testWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testRun = Join-Path $testWorkspace ('.diagnostics\filter-defaults-tests\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$testTemporary = New-Item -ItemType Directory -Path (Join-Path $testRun 'tmp')
$testSource = New-Item -ItemType Directory -Path (Join-Path $testRun 'src')
Copy-Item -LiteralPath (Join-Path $testWorkspace 'c4ddraw\upstream\cnc-ddraw\src\config.c') -Destination (Join-Path $testSource.FullName 'config.c')
# Exact newly created test path: only this disposable config copy is patched; no source/build/game mutation.
& git -C $testWorkspace -c core.autocrlf=false apply --ignore-whitespace --unsafe-paths ('--directory=' + $testRun.Replace('\','/')) (Join-Path $testWorkspace 'c4ddraw\patches\cnc-ddraw-default-ini.patch')
if ($LASTEXITCODE -ne 0) { throw 'Maintained default INI patch failed on pristine config.c' }
& python (Join-Path $PSScriptRoot 'extract-filter-defaults.py') $testWorkspace $testRun
if ($LASTEXITCODE -ne 0) { throw 'Filter function extraction/source checks failed' }
$testVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$testMSBuild = & $testVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $testMSBuild) { throw 'MSBuild unavailable' }
$testBuildStart = [Diagnostics.ProcessStartInfo]::new()
$testBuildStart.FileName = $testMSBuild
$testBuildStart.UseShellExecute = $false
$testBuildStart.CreateNoWindow = $true
$testBuildStart.Arguments = '"' + (Join-Path $PSScriptRoot 'filter_defaults_tests.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 "/p:FilterExtractDir=' + $testRun + '" /nologo /v:minimal'
$testBuildStart.EnvironmentVariables['TEMP'] = $testTemporary.FullName
$testBuildStart.EnvironmentVariables['TMP'] = $testTemporary.FullName
$testProcess = [Diagnostics.Process]::Start($testBuildStart)
$testProcess.WaitForExit()
if ($testProcess.ExitCode -ne 0) { throw 'Actual filter parser test build failed' }
Push-Location $testRun
try {
    & (Join-Path $testRun 'bin\filter_defaults_tests.exe') | Tee-Object -FilePath (Join-Path $testRun 'result.log')
    if ($LASTEXITCODE -ne 0) { throw 'Actual filter parser tests failed' }
} finally { Pop-Location }
Write-Output ('PASS: actual parser, maintained patch, source defaults and evidence: ' + $testRun)
