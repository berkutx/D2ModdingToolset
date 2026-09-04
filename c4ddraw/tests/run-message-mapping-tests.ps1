param([string]$BuildSource)
$ErrorActionPreference = 'Stop'
$testWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $BuildSource) { $BuildSource = Join-Path $testWorkspace 'c4ddraw\build\wrapper-batch-20260904-01\src' }
$testRun = Join-Path $testWorkspace ('.diagnostics\message-mapping-tests\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$testTemporary = New-Item -ItemType Directory -Path (Join-Path $testRun 'tmp')
& python (Join-Path $PSScriptRoot 'extract-message-mapping.py') $BuildSource $testRun
if ($LASTEXITCODE -ne 0) { throw 'Mapping function extraction failed' }
$testVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$testMSBuild = & $testVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $testMSBuild) { throw 'MSBuild unavailable' }
$testEnvironment = @{}
foreach ($testKey in [Environment]::GetEnvironmentVariables().Keys) {
    $testEnvironment[[string]$testKey] = [Environment]::GetEnvironmentVariable([string]$testKey)
}
$testEnvironment['TEMP'] = $testTemporary.FullName
$testEnvironment['TMP'] = $testTemporary.FullName
$testBuildStart = [Diagnostics.ProcessStartInfo]::new()
$testBuildStart.FileName = $testMSBuild
$testBuildStart.UseShellExecute = $false
$testBuildStart.CreateNoWindow = $true
$testBuildStart.Arguments = '"' + (Join-Path $PSScriptRoot 'message_mapping_tests.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 "/p:MappingExtractDir=' + $testRun + '" /nologo /v:minimal'
$testBuildStart.EnvironmentVariables.Clear()
foreach ($testKey in $testEnvironment.Keys) { $testBuildStart.EnvironmentVariables[$testKey] = $testEnvironment[$testKey] }
$testProcess = [Diagnostics.Process]::Start($testBuildStart)
$testProcess.WaitForExit()
if ($testProcess.ExitCode -ne 0) { throw 'Actual mapping test build failed' }
& (Join-Path $testRun 'bin\message_mapping_tests.exe') | Tee-Object -FilePath (Join-Path $testRun 'result.log')
if ($LASTEXITCODE -ne 0) { throw 'Actual mapping tests failed' }
Write-Output ('PASS: extracted source provenance and results: ' + $testRun)
