param([string]$BuildDirectory, [ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$BuildDirectory) {
    $BuildDirectory = Join-Path $taskRoot ('.diagnostics/settings-exit-tests-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
$taskAllowed = [IO.Path]::GetFullPath($taskRoot).TrimEnd('\') + '\'
if (!$taskBuild.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test output must stay within this workspace.'
}
if (Test-Path -LiteralPath $taskBuild) { throw 'Use a new build directory; existing artifacts are preserved.' }
New-Item -ItemType Directory -Path $taskBuild | Out-Null
New-Item -ItemType Directory -Path (Join-Path $taskBuild 'tmp') | Out-Null
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'VS x86 C++ tools not found.' }
$taskVcvars = Join-Path $taskVs 'VC/Auxiliary/Build/vcvars32.bat'
$taskSource = Join-Path $PSScriptRoot 'settings_exit_tests.cpp'
$taskContract = Join-Path $PSScriptRoot 'test-wrapper-reset-contract.py'
$taskProduction = Join-Path (Split-Path $PSScriptRoot -Parent) 'features/rendererbridge.c'
$taskMenu = Join-Path (Split-Path $PSScriptRoot -Parent) 'features/featuremenu.cpp'
$taskDefaults = Join-Path (Split-Path $PSScriptRoot -Parent) 'features/wrapperdefaults.h'
$taskTrace = Join-Path (Split-Path $PSScriptRoot -Parent) 'features/c4trace.cpp'
$taskOptimization = if ($Configuration -eq 'Release') { '/O2' } else { '/Od' }
$taskCommand = 'call "{0}" && cl.exe /nologo /EHsc /W4 {2} "{1}" /Fe:settings_exit_tests.exe /Fo:settings_exit_tests.obj /link /MANIFEST:EMBED /MANIFESTUAC:"level=''asInvoker'' uiAccess=''false''"' -f $taskVcvars,$taskSource,$taskOptimization
$taskOldTemp = $env:TEMP
$taskOldTmp = $env:TMP
Push-Location $taskBuild
try {
    $env:TEMP = Join-Path $taskBuild 'tmp'
    $env:TMP = $env:TEMP
    & python $taskContract 2>&1 | Tee-Object -FilePath (Join-Path $taskBuild 'contract.log')
    if ($LASTEXITCODE) { throw "Settings exit contract failed: $LASTEXITCODE" }
    & cmd.exe /d /c $taskCommand 2>&1 | Tee-Object -FilePath (Join-Path $taskBuild 'build.log')
    if ($LASTEXITCODE) { throw "Settings exit fixture compilation failed: $LASTEXITCODE" }
    & (Join-Path $taskBuild 'settings_exit_tests.exe') 2>&1 | Tee-Object -FilePath (Join-Path $taskBuild 'test.log')
    if ($LASTEXITCODE) { throw "Settings exit fixture failed: $LASTEXITCODE" }
    Get-FileHash -Algorithm SHA256 -LiteralPath $taskSource,$taskContract,$taskProduction,$taskMenu,$taskDefaults,$taskTrace,$PSCommandPath,(Join-Path $taskBuild 'settings_exit_tests.exe') |
        Select-Object Path,Hash | ConvertTo-Json | Out-File -Encoding utf8 (Join-Path $taskBuild 'hashes.json')
} finally {
    $env:TEMP = $taskOldTemp
    $env:TMP = $taskOldTmp
    Pop-Location
}
Write-Output "Settings immediate-exit evidence: $taskBuild"
