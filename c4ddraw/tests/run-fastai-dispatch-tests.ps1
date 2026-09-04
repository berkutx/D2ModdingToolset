param([string]$BuildDirectory, [ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$BuildDirectory) {
    $BuildDirectory = Join-Path $taskRoot ('.diagnostics/fastai-dispatch-tests-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
$taskAllowed = [IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\'
if (!$taskBuild.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test output must stay within this workspace.'
}
if (Test-Path -LiteralPath $taskBuild) { throw 'Use a new build directory; existing artifacts are preserved.' }
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
$taskSource = Join-Path $PSScriptRoot 'fastai_dispatch_tests.cpp'
$taskProduction = Join-Path (Split-Path $PSScriptRoot -Parent) 'features/fastai.cpp'
$taskOptimization = if ($Configuration -eq 'Release') { '/O2' } else { '/Od' }
$taskCommand = 'call "{0}" && cl.exe /nologo /EHsc /W4 {2} /DWIN32 /D_WINDOWS "{1}" user32.lib /Fe:fastai_dispatch_tests.exe /Fo:fastai_dispatch_tests.obj /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''" && cl.exe /nologo /c /EHsc /W4 {2} /DWIN32 /D_WINDOWS "{3}" /Fo:fastai-production.obj' -f $taskVcvars,$taskSource,$taskOptimization,$taskProduction
Push-Location $taskBuild
try {
    & cmd.exe /d /c $taskCommand 2>&1 | Tee-Object -FilePath (Join-Path $taskBuild 'build.log')
    if ($LASTEXITCODE) { throw "FastAI test compilation failed: $LASTEXITCODE" }
    & (Join-Path $taskBuild 'fastai_dispatch_tests.exe') 2>&1 | Tee-Object -FilePath (Join-Path $taskBuild 'test.log')
    if ($LASTEXITCODE) { throw "FastAI tests failed: $LASTEXITCODE" }
    Get-FileHash -Algorithm SHA256 -LiteralPath $taskSource,$taskProduction,(Join-Path $taskBuild 'fastai_dispatch_tests.exe') |
        Select-Object Path,Hash | ConvertTo-Json | Out-File -Encoding utf8 (Join-Path $taskBuild 'hashes.json')
} finally { Pop-Location }
Write-Output "FastAI test evidence: $taskBuild"
