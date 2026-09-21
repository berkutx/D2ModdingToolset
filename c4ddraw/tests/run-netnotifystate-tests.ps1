param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$testWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testOutput = Join-Path $testWorkspace '.diagnostics\netnotifystate-tests'
$testRun = Join-Path $testOutput ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$testTemporary = New-Item -ItemType Directory -Force -Path (Join-Path $testRun 'tmp')
$testSourcePaths = @('mss32\src\netcustompeer.cpp', 'mss32\src\netcustomservice.cpp', 'c4ddraw\features\netnotifystate.h')
$testSourceHashes = @{}
foreach ($testRelative in $testSourcePaths) { $testSourceHashes[$testRelative] = (Get-FileHash -LiteralPath (Join-Path $testWorkspace $testRelative) -Algorithm SHA256).Hash }
& $Python (Join-Path $PSScriptRoot 'extract-netnotifystate-methods.py')
if ($LASTEXITCODE -ne 0) { throw 'MSS method extraction failed' }
Copy-Item -LiteralPath (Join-Path $testOutput 'generated\extraction.json') -Destination (Join-Path $testRun 'extraction.json')
$testVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$testMSBuild = & $testVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $testMSBuild) { throw 'MSBuild unavailable' }
# Normalize duplicate Path/PATH only in the child and keep compiler temp in this run.
$testBuildEnvironment = @{}
foreach ($testKey in [Environment]::GetEnvironmentVariables().Keys) { $testBuildEnvironment[[string]$testKey] = [Environment]::GetEnvironmentVariable([string]$testKey) }
$testBuildEnvironment['TEMP'] = $testTemporary.FullName
$testBuildEnvironment['TMP'] = $testTemporary.FullName
$testBuildStart = [Diagnostics.ProcessStartInfo]::new()
$testBuildStart.FileName = $testMSBuild
$testBuildStart.UseShellExecute = $false
$testBuildStart.CreateNoWindow = $true
$testBuildStart.RedirectStandardOutput = $true
$testBuildStart.RedirectStandardError = $true
$testBuildStart.Arguments = '"' + (Join-Path $PSScriptRoot 'netnotifystate_tests.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal'
$testBuildStart.EnvironmentVariables.Clear()
foreach ($testKey in $testBuildEnvironment.Keys) { $testBuildStart.EnvironmentVariables[$testKey] = $testBuildEnvironment[$testKey] }
$testBuildProcess = [Diagnostics.Process]::Start($testBuildStart)
$testBuildOutput = $testBuildProcess.StandardOutput.ReadToEndAsync()
$testBuildError = $testBuildProcess.StandardError.ReadToEndAsync()
$testBuildProcess.WaitForExit()
$testBuildLog = $testBuildOutput.Result + $testBuildError.Result
$testBuildLog | Set-Content -LiteralPath (Join-Path $testRun 'build.log') -Encoding UTF8
if ($testBuildProcess.ExitCode -ne 0) { Write-Output $testBuildLog; throw 'Notification state test build failed' }
$testExe = Join-Path $testRun 'netnotifystate_tests.exe'
Copy-Item -LiteralPath (Join-Path $testOutput 'bin\netnotifystate_tests.exe') -Destination $testExe
$testLines = & $testExe
$testExit = $LASTEXITCODE
$testLines | Tee-Object -FilePath (Join-Path $testRun 'result.log')
$testResult = [regex]::Match(($testLines -join "`n"), 'RESULT=(PASS|FAIL) cases=(\d+) failures=(\d+)')
if (-not $testResult.Success) { throw 'Missing notification state result' }
$testUnchanged = $true
foreach ($testRelative in $testSourcePaths) { if ((Get-FileHash -LiteralPath (Join-Path $testWorkspace $testRelative) -Algorithm SHA256).Hash -ne $testSourceHashes[$testRelative]) { $testUnchanged = $false } }
$testSummary = [ordered]@{
    Result = $testResult.Groups[1].Value
    Cases = [int]$testResult.Groups[2].Value
    Failures = [int]$testResult.Groups[3].Value
    ProcessExitCode = $testExit
    ProductionSourcesUnchanged = $testUnchanged
    ProductionSourcesSha256 = $testSourceHashes
    TestExecutableSha256 = (Get-FileHash -LiteralPath $testExe -Algorithm SHA256).Hash
    Scope = 'Actual WakeState and six extracted MSS methods; real Win32 posts and worker/UI threads. FIFO, current-service access and native boundary classification are fixture inputs. No game, IDA, live network, A/B or injected DLL.'
}
$testSummary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $testRun 'results.json') -Encoding UTF8
if ($testExit -ne 0 -or $testSummary.Failures -ne 0 -or -not $testUnchanged) { throw 'Notification regression failed or tested source changed during execution' }
Write-Output ('PASS: private hidden test windows only; artifacts: ' + $testRun)
