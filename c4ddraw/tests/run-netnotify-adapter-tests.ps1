$ErrorActionPreference = 'Stop'
$thunkWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$thunkOutput = Join-Path $thunkWorkspace '.diagnostics\netnotify-adapter-tests'
$thunkRun = Join-Path $thunkOutput ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$thunkTemporary = New-Item -ItemType Directory -Path (Join-Path $thunkRun 'tmp')
$thunkVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$thunkMSBuild = & $thunkVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $thunkMSBuild) { throw 'MSBuild unavailable' }
$thunkEnvironment = @{}
foreach ($thunkKey in [Environment]::GetEnvironmentVariables().Keys) {
    $thunkEnvironment[[string]$thunkKey] = [Environment]::GetEnvironmentVariable([string]$thunkKey)
}
$thunkEnvironment['TEMP'] = $thunkTemporary.FullName
$thunkEnvironment['TMP'] = $thunkTemporary.FullName
$thunkStart = [Diagnostics.ProcessStartInfo]::new()
$thunkStart.FileName = $thunkMSBuild
$thunkStart.UseShellExecute = $false
$thunkStart.CreateNoWindow = $true
$thunkStart.RedirectStandardOutput = $true
$thunkStart.RedirectStandardError = $true
$thunkStart.Arguments = '"' + (Join-Path $PSScriptRoot 'netnotify_adapter_tests.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal'
$thunkStart.EnvironmentVariables.Clear()
foreach ($thunkKey in $thunkEnvironment.Keys) { $thunkStart.EnvironmentVariables[$thunkKey] = $thunkEnvironment[$thunkKey] }
$thunkProcess = [Diagnostics.Process]::Start($thunkStart)
$thunkStdout = $thunkProcess.StandardOutput.ReadToEndAsync()
$thunkStderr = $thunkProcess.StandardError.ReadToEndAsync()
$thunkProcess.WaitForExit()
$thunkStdout.GetAwaiter().GetResult() | Tee-Object -FilePath (Join-Path $thunkRun 'build.stdout.log')
$thunkStderr.GetAwaiter().GetResult() | Tee-Object -FilePath (Join-Path $thunkRun 'build.stderr.log')
if ($thunkProcess.ExitCode -ne 0) { throw 'Netnotify adapter test build failed' }
$thunkExe = Join-Path $thunkRun 'netnotify_adapter_tests.exe'
Copy-Item -LiteralPath (Join-Path $thunkOutput 'bin\netnotify_adapter_tests.exe') -Destination $thunkExe
& $thunkExe | Tee-Object -FilePath (Join-Path $thunkRun 'result.log')
if ($LASTEXITCODE -ne 0) { throw 'Netnotify adapter tests failed' }
Write-Output ('PASS: synthetic stack/site and private hidden window only; artifacts: ' + $thunkRun)
