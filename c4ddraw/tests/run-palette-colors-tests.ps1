param([string]$BuildDirectory, [switch]$Baseline)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$BuildDirectory) { $BuildDirectory = Join-Path $taskRoot ('.diagnostics/palette-colors-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
$taskAllowed = [IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\'
if (!$taskBuild.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test output must stay in this workspace.' }
if (Test-Path -LiteralPath $taskBuild) { throw 'Use a new output directory to preserve evidence.' }
New-Item -ItemType Directory -Path (Join-Path $taskBuild 'src') | Out-Null
$taskUpstream = Join-Path $PSScriptRoot '../upstream/cnc-ddraw'
Copy-Item -LiteralPath (Join-Path $taskUpstream 'src/ddpalette.c') -Destination (Join-Path $taskBuild 'src/ddpalette.c')
if (!$Baseline) {
    $taskPatch = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../patches/cnc-ddraw-d2-palette-colors.patch'))
    & git -C $taskBuild init -q
    & git -C $taskBuild -c core.autocrlf=false apply --ignore-whitespace $taskPatch
    if ($LASTEXITCODE) { throw 'Palette patch did not apply.' }
}
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
$taskTest = Join-Path $PSScriptRoot 'palette_colors_tests.c'
$taskIncludes = [IO.Path]::GetFullPath((Join-Path $taskUpstream 'inc'))
$taskCommand = 'call "{0}" && cl.exe /nologo /TC /O2 /W3 /I"{1}" "{2}" src\ddpalette.c /Fe:palette_colors_tests.exe /link gdi32.lib /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"' -f $taskVcvars,$taskIncludes,$taskTest
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
if ($taskProcess.ExitCode) { Get-Content -LiteralPath (Join-Path $taskBuild 'build.log'); throw 'Palette test build failed.' }
Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $taskBuild 'src/ddpalette.c'),$taskTest |
    Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskBuild 'hashes.json')
& (Join-Path $taskBuild 'palette_colors_tests.exe') | Tee-Object -FilePath (Join-Path $taskBuild 'test.log')
if ($LASTEXITCODE) { throw 'Palette color checks failed.' }
