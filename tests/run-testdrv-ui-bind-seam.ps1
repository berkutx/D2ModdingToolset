param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = New-Item -ItemType Directory -Path $OutputDirectory -Force
$sourcePath = Join-Path $repo 'mss32\src\testdrv\uistatereporter.cpp'
$source = Get-Content -LiteralPath $sourcePath -Raw
# Exercise the actual seam functions without loading a game or installing hooks. Native patching,
# UI ownership and C4/stock are stubs; this does not replace the two-client live harness acceptance.
$functions = foreach ($signature in @(
    'game::CButtonInterf\* __stdcall hookAssignFunctor\(',
    'bool preflight\(', 'bool commit\(')) {
    $matches = [regex]::Matches($source, '(?ms)^' + $signature + '.*?^\}')
    if ($matches.Count -ne 1) { throw "Expected one production function: $signature" }
    $matches[0].Value
}
$generated = Join-Path $output.FullName 'testdrv_ui_bind_seam_production.inc'
[IO.File]::WriteAllText($generated, ($functions -join "`n`n"))
Get-FileHash -LiteralPath $sourcePath, $generated | Format-List Path, Hash
Push-Location $output.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
        ('/I' + $output.FullName) (Join-Path $PSScriptRoot 'testdrv_ui_bind_seam_test.cpp') `
        /Fetestdrv_ui_bind_seam_test.exe /link /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw 'UI bind seam compilation failed; no stale binary will be run' }
    & .\testdrv_ui_bind_seam_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'UI bind seam regression failed' }
} finally { Pop-Location }
