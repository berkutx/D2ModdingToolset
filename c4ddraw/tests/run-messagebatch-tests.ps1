$ErrorActionPreference = 'Stop'
$testWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testOutput = Join-Path $testWorkspace '.diagnostics\messagebatch-tests'
$testRun = Join-Path $testOutput ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$testTemporary = New-Item -ItemType Directory -Path (Join-Path $testRun 'tmp')
$testVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$testMSBuild = & $testVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $testMSBuild) { throw 'MSBuild unavailable' }

# Normalize duplicate Path/PATH only in the child; keep the caller unchanged.
# Compiler temporary files stay in this bounded test run, not the shared RAM disk.
$testBuildEnvironment = @{}
foreach ($testKey in [Environment]::GetEnvironmentVariables().Keys) {
    $testBuildEnvironment[[string]$testKey] = [Environment]::GetEnvironmentVariable([string]$testKey)
}
$testBuildEnvironment['TEMP'] = $testTemporary.FullName
$testBuildEnvironment['TMP'] = $testTemporary.FullName
$testBuildStart = [Diagnostics.ProcessStartInfo]::new()
$testBuildStart.FileName = $testMSBuild
$testBuildStart.UseShellExecute = $false
$testBuildStart.CreateNoWindow = $true
$testBuildStart.Arguments = '"' + (Join-Path $PSScriptRoot 'messagebatch_tests.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal'
$testBuildStart.EnvironmentVariables.Clear()
foreach ($testKey in $testBuildEnvironment.Keys) { $testBuildStart.EnvironmentVariables[$testKey] = $testBuildEnvironment[$testKey] }
$testBuildProcess = [Diagnostics.Process]::Start($testBuildStart)
$testBuildProcess.WaitForExit()
if ($testBuildProcess.ExitCode -ne 0) { throw 'Message-batch isolated test build failed' }

$testExe = Join-Path $testRun 'messagebatch_tests.exe'
Copy-Item -LiteralPath (Join-Path $testOutput 'bin\messagebatch_tests.exe') -Destination $testExe
& $testExe | Tee-Object -FilePath (Join-Path $testRun 'result.log')
if ($LASTEXITCODE -ne 0) { throw 'Message-batch isolated tests failed' }
Write-Output ('PASS: only private hidden test windows; artifacts: ' + $testRun)
