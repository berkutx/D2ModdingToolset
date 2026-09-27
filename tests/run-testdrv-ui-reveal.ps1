param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$source=Get-Content (Join-Path $PSScriptRoot '../mss32/src/testdrv/uistatereporter.cpp') -Raw
$output=New-Item -ItemType Directory -Path $OutputDirectory -Force
$functions=foreach($signature in @('void selectDialogInstance\(', 'bool selectRevealedDialog\(')) {
    $m=[regex]::Matches($source,'(?ms)^'+$signature+'.*?^\}')
    if($m.Count -ne 1) { throw 'Actual reveal function is not unique' };$m[0].Value
}
$refresh=[regex]::Match($source,'(?ms)^void refreshCurrentDialog\(.*?^\}').Value
if($refresh -notmatch 'if \(!selectRevealedDialog\(top, name\)\)' -or $refresh -match 'lstrcpynA\(g_lastDialog, name') {
    throw 'Refresh must use the atomic actual reveal helper'
}
[IO.File]::WriteAllText((Join-Path $output.FullName 'testdrv_ui_reveal_production.inc'),($functions -join "`n"))
Push-Location $output.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG ('/I'+$output.FullName) (Join-Path $PSScriptRoot 'testdrv_ui_reveal_test.cpp') /Fetestdrv_ui_reveal_test.exe /link /INCREMENTAL:NO
    if($LASTEXITCODE) { throw 'Reveal regression compile failed' }
    & ./testdrv_ui_reveal_test.exe
    if($LASTEXITCODE) { throw 'Reveal regression failed' }
} finally { Pop-Location }
