param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$DependenciesRoot,
    [string]$CoreRoot
)
$ErrorActionPreference = 'Stop'
$preparedRepo = Split-Path $PSScriptRoot -Parent
$preparedOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
Push-Location $preparedOutput.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
        ('/I' + (Join-Path $preparedRepo 'mss32\include')) `
        (Join-Path $preparedRepo 'tests\preparedmatchprotocol_test.cpp') `
        (Join-Path $preparedRepo 'mss32\src\preparedmatchprotocol.cpp') `
        /Fepreparedmatchprotocol_test.exe /link /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw 'Prepared protocol compilation failed; old executable was not run' }
    & '.\preparedmatchprotocol_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Prepared protocol regression failed' }
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
        ('/I' + (Join-Path $preparedRepo 'mss32\include')) `
        (Join-Path $preparedRepo 'tests\preparedmatchlifecycle_test.cpp') `
        /Fepreparedmatchlifecycle_test.exe /link /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw 'Prepared lifecycle compilation failed; old executable was not run' }
    & '.\preparedmatchlifecycle_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Prepared lifecycle regression failed' }
    if ($DependenciesRoot) {
        $preparedRsg = Join-Path (Resolve-Path -LiteralPath $DependenciesRoot).Path 'D2RSG\ScenarioGenerator\src'
        & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
            ('/I' + (Join-Path $preparedRepo 'mss32\include')) ('/I' + $preparedRsg) `
            ('/I' + (Join-Path $preparedRsg 'scenario')) `
            (Join-Path $preparedRepo 'tests\preparedmatchsettings_test.cpp') `
            (Join-Path $preparedRepo 'mss32\src\preparedmatchprotocol.cpp') `
            /Fepreparedmatchsettings_test.exe /link /INCREMENTAL:NO
        if ($LASTEXITCODE -ne 0) { throw 'Prepared settings compilation failed; old executable was not run' }
        & '.\preparedmatchsettings_test.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Prepared settings regression failed' }
    }
    if ($CoreRoot) {
        $preparedCore = Join-Path (Resolve-Path -LiteralPath $CoreRoot).Path 'lobby\server'
        & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
            ('/I' + $preparedCore) `
            (Join-Path $preparedCore 'preparedmatch_test.cpp') `
            (Join-Path $preparedCore 'preparedmatch.cpp') `
            /Fecore_preparedmatch_test.exe /link /INCREMENTAL:NO
        if ($LASTEXITCODE -ne 0) { throw 'Core prepared compilation failed; old executable was not run' }
        & '.\core_preparedmatch_test.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Core prepared regression failed' }
        & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG `
            ('/I' + $preparedCore) ('/I' + (Join-Path $preparedRepo 'mss32\include')) `
            (Join-Path $preparedRepo 'tests\preparedmatchjoin_crosswire_test.cpp') `
            (Join-Path $preparedCore 'preparedmatch.cpp') `
            (Join-Path $preparedRepo 'mss32\src\preparedmatchprotocol.cpp') `
            /Fepreparedmatchjoin_crosswire_test.exe /link /INCREMENTAL:NO
        if ($LASTEXITCODE -ne 0) { throw 'Prepared join crosswire compilation failed; old executable was not run' }
        & '.\preparedmatchjoin_crosswire_test.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Prepared join crosswire regression failed' }
    }
} finally { Pop-Location }
