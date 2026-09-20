$ErrorActionPreference = 'Stop'
$traceTestRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\.diagnostics\netboundarytrace-tests'))
$traceTestTemp = New-Item -ItemType Directory -Force -Path (Join-Path $traceTestRoot 'tmp')
$traceVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$traceMSBuild = & $traceVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $traceMSBuild) { throw 'MSBuild unavailable' }
$traceEnvironment = @{}
foreach ($traceKey in [Environment]::GetEnvironmentVariables().Keys) {
    $traceEnvironment[[string]$traceKey] = [Environment]::GetEnvironmentVariable([string]$traceKey)
}
$traceStart = [Diagnostics.ProcessStartInfo]::new()
$traceStart.FileName = $traceMSBuild
$traceStart.UseShellExecute = $false
$traceStart.CreateNoWindow = $true
$traceStart.RedirectStandardOutput = $true
$traceStart.RedirectStandardError = $true
$traceStart.Arguments = '"' + (Join-Path $PSScriptRoot 'netboundarytrace_test.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal'
$traceStart.EnvironmentVariables.Clear()
foreach ($traceKey in $traceEnvironment.Keys) { $traceStart.EnvironmentVariables[$traceKey] = $traceEnvironment[$traceKey] }
$traceStart.EnvironmentVariables['TEMP'] = $traceTestTemp.FullName
$traceStart.EnvironmentVariables['TMP'] = $traceTestTemp.FullName
$traceProcess = [Diagnostics.Process]::Start($traceStart)
$traceOutput = $traceProcess.StandardOutput.ReadToEndAsync()
$traceError = $traceProcess.StandardError.ReadToEndAsync()
$traceProcess.WaitForExit()
$traceLog = $traceOutput.Result + $traceError.Result
$traceLog | Set-Content -LiteralPath (Join-Path $traceTestRoot 'build.log') -Encoding UTF8
if ($traceProcess.ExitCode -ne 0) { Write-Output $traceLog; throw 'Boundary ABI test build failed' }
$traceResults = & (Join-Path $traceTestRoot 'bin\netboundarytrace_test.exe')
$traceExit = $LASTEXITCODE
$traceResults | Set-Content -LiteralPath (Join-Path $traceTestRoot 'results.log') -Encoding UTF8
$traceResults | Write-Output
if ($traceExit -ne 0) { throw 'Boundary ABI test failed' }
