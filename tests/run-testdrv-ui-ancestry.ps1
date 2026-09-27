param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$source=Get-Content (Join-Path $PSScriptRoot '../mss32/src/testdrv/uistatereporter.cpp') -Raw
$actual=[regex]::Matches($source,'(?ms)^bool observeDialogOnScreen\(.*?^\}')
if($actual.Count -ne 1) {throw 'Actual ancestry helper is not unique'}
if($source -notmatch 'observeDialogOnScreen\(candidate->ptr, g_currentTopScreen\)') {throw 'Snapshot must use actual ancestry proof'}
$output=New-Item -ItemType Directory -Path $OutputDirectory -Force
[IO.File]::WriteAllText((Join-Path $output.FullName 'testdrv_ui_ancestry_production.inc'),$actual[0].Value)
Push-Location $output.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG ('/I'+$output.FullName) (Join-Path $PSScriptRoot 'testdrv_ui_ancestry_test.cpp') /Fetestdrv_ui_ancestry_test.exe /link /INCREMENTAL:NO
    if($LASTEXITCODE) {throw 'Ancestry regression compile failed'}
    & ./testdrv_ui_ancestry_test.exe
    if($LASTEXITCODE) {throw 'Ancestry regression failed'}
} finally {Pop-Location}
