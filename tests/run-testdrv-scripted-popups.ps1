param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$popupRepo = Split-Path $PSScriptRoot -Parent
$popupOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
$popupSourcePath = Join-Path $popupRepo 'mss32\src\testdrv\scriptedpopups.cpp'
$popupSource = Get-Content -LiteralPath $popupSourcePath -Raw
# Compile the actual subscriber state machine, with only platform/game/UI
# boundaries substituted. This does not test real SEH, widgets or a live game.
$popupGenerated = Join-Path $popupOutput.FullName 'testdrv_scripted_popups_production.inc'
$popupSource = [regex]::Replace($popupSource, '(?m)^#include[^\r\n]*\r?\n', '')
[IO.File]::WriteAllText($popupGenerated, $popupSource)
Get-FileHash -LiteralPath $popupSourcePath, $popupGenerated | Format-List Path, Hash
Push-Location $popupOutput.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG /DD2_TESTDRV `
        ('/I' + $popupOutput.FullName) (Join-Path $PSScriptRoot 'testdrv_scripted_popups_test.cpp') `
        /Fetestdrv_scripted_popups_test.exe /link /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw 'Popup state-machine compilation failed; no stale binary will run' }
    & .\testdrv_scripted_popups_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Popup state-machine regression failed' }
} finally { Pop-Location }
