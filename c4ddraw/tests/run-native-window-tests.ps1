$ErrorActionPreference = 'Stop'
$testRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testRun = Join-Path $testRoot ('.diagnostics\native-window-tests\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$testTemporary = New-Item -ItemType Directory -Path (Join-Path $testRun 'tmp')
& python (Join-Path $PSScriptRoot 'extract-native-window.py') $testRoot $testRun
if ($LASTEXITCODE -ne 0) { throw 'Native window patch chain/extraction failed' }
$testVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$testMSBuild = & $testVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $testMSBuild) { throw 'MSBuild unavailable' }
$testStart = [Diagnostics.ProcessStartInfo]::new()
$testStart.FileName = $testMSBuild
$testStart.UseShellExecute = $false
$testStart.CreateNoWindow = $true
$testStart.Arguments = '"' + (Join-Path $PSScriptRoot 'native_window_tests.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 "/p:NativeExtractDir=' + $testRun + '" /nologo /v:minimal'
$testStart.EnvironmentVariables['TEMP'] = $testTemporary.FullName
$testStart.EnvironmentVariables['TMP'] = $testTemporary.FullName
$testProcess = [Diagnostics.Process]::Start($testStart)
$testProcess.WaitForExit()
if ($testProcess.ExitCode -ne 0) { throw 'Native window test build failed' }
& (Join-Path $testRun 'bin\native_window_tests.exe') | Tee-Object -FilePath (Join-Path $testRun 'result.log')
if ($LASTEXITCODE -ne 0) { throw 'Native window tests failed' }
Write-Output ('PASS: evidence: ' + $testRun)
