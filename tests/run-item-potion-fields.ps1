# Run from an MSVC x86 developer environment. Does not build/load the MSS DLL.
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('Debug', 'Release')][string[]]$Configurations = @('Debug', 'Release')
)
$ErrorActionPreference = 'Stop'
$potionRepo = Split-Path $PSScriptRoot -Parent
$potionOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
foreach ($configuration in $Configurations) {
    $potionConfigurationOutput = New-Item -ItemType Directory `
        -Path (Join-Path $potionOutput.FullName $configuration) -Force
    $potionOptions = if ($configuration -eq 'Debug') {
        @('/MTd', '/Od', '/D_DEBUG')
    } else {
        @('/MT', '/O2', '/DNDEBUG')
    }
    Push-Location $potionConfigurationOutput.FullName
    try {
        & cl.exe /nologo /std:c++17 /EHsc /Gy /W3 /DNOMINMAX @potionOptions `
            ('/I' + (Join-Path $potionRepo 'mss32\include')) `
            ('/I' + (Join-Path $potionRepo 'mss32\include\bindings')) `
            (Join-Path $potionRepo 'tests\itempotionfields_test.cpp') `
            /Feitempotionfields_test.exe /link /INCREMENTAL:NO /OPT:REF /OPT:ICF
        if ($LASTEXITCODE -ne 0) {
            throw "Potion field compilation failed ($configuration); old executable was not run"
        }
        & '.\itempotionfields_test.exe'
        if ($LASTEXITCODE -ne 0) { throw "Potion field regression failed ($configuration)" }
    } finally { Pop-Location }
}
