param([Parameter(Mandatory = $true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$compatibilityRepo = Split-Path $PSScriptRoot -Parent
$compatibilityOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
Push-Location $compatibilityOutput.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
        ('/I' + (Join-Path $compatibilityRepo 'mss32\include')) `
        (Join-Path $compatibilityRepo 'tests\clientcompatibility_test.cpp') `
        /Feclientcompatibility_test.exe /link /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw 'Client compatibility compilation failed; old executable was not run' }
    & '.\clientcompatibility_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Client compatibility regression failed' }
} finally { Pop-Location }
