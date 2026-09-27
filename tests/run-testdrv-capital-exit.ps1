param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$capitalRepo = Split-Path $PSScriptRoot -Parent
$capitalOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
$capitalSourcePath = Join-Path $capitalRepo 'mss32\src\testdrv\worldactions.cpp'
$capitalSource = Get-Content -LiteralPath $capitalSourcePath -Raw
$capitalQuery = [regex]::Match($capitalSource,
    '(?ms)^bool querySupportedCapitalExit\(.*?(?=^bool preflightHostMoveRoute\()')
$capitalResolver = [regex]::Match($capitalSource,
    '(?ms)^bool resolveGarrisonExit\(.*?(?=^bool isAllowedExactRoute\()')
if (-not $capitalQuery.Success -or -not $capitalResolver.Success) {
    throw 'Could not extract the actual capital-exit guard/resolver'
}
$capitalReporter = Get-Content -LiteralPath (Join-Path $capitalRepo 'mss32\src\testdrv\worldreporter.cpp') -Raw
if ([regex]::Matches($capitalSource, 'if \(!resolveGarrisonExit\(objectMap, stack, targetX, targetY, exitStart, exitDest\)\)').Count -ne 2 -or
    $capitalReporter -notmatch 'worldactions::querySupportedCapitalExit\(objectMap,' -or
    $capitalReporter -notmatch 'observed-5x5-capital') {
    throw 'Reporter or one native movement overload no longer uses the shared exit guard'
}
# Compile these actual functions, substituting only game/native boundaries.
$capitalGenerated = Join-Path $capitalOutput.FullName 'testdrv_capital_exit_production.inc'
[IO.File]::WriteAllText($capitalGenerated, $capitalQuery.Value + $capitalResolver.Value)
Get-FileHash -LiteralPath $capitalSourcePath, $capitalGenerated | Format-List Path, Hash
Push-Location $capitalOutput.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
        ('/I' + $capitalOutput.FullName) (Join-Path $PSScriptRoot 'testdrv_capital_exit_test.cpp') `
        /Fetestdrv_capital_exit_test.exe /link /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw 'Capital-exit fixture compilation failed; no stale binary will run' }
    & .\testdrv_capital_exit_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Capital-exit fixture failed' }
} finally { Pop-Location }
