param([string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$BuildDirectory) {
    $BuildDirectory = Join-Path $taskRoot ('.diagnostics/timer-post-battle-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
$taskAllowed = [IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\'
if (!$taskBuild.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test output must stay within this workspace.'
}
if (Test-Path -LiteralPath $taskBuild) { throw 'Use a new build directory; existing evidence is preserved.' }
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
$taskSource = Join-Path $PSScriptRoot 'timer_post_battle_tests.cpp'
$taskCommand = 'call "{0}" && cl.exe /nologo /EHsc /O2 /W4 "{1}" user32.lib gdi32.lib gdiplus.lib ole32.lib /Fe:timer_post_battle_tests.exe /Fo:timer_post_battle_tests.obj /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"' -f $taskVcvars,$taskSource
Push-Location $taskBuild
try {
    & cmd.exe /d /c $taskCommand 2>&1 | Tee-Object -FilePath build.log
    if ($LASTEXITCODE) { throw 'Timer replay compilation failed.' }
    Get-FileHash -Algorithm SHA256 -LiteralPath $taskSource,(Join-Path $PSScriptRoot '../plugins/timer/timer.cpp'),(Join-Path $PSScriptRoot '../features/c4plugin.h') |
        Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath hashes.json
    & .\timer_post_battle_tests.exe 2>&1 | Tee-Object -FilePath test.log
    if ($LASTEXITCODE) { throw 'Timer replay checks failed.' }
} finally { Pop-Location }
