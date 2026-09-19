# Run in the same MSVC x86 developer environment as a successful Release MSS build.
# Reuses its Lua objects and ScenarioGenerator library; does not launch the game.
param(
    [Parameter(Mandatory = $true)][string]$DependenciesRoot,
    [Parameter(Mandatory = $true)][string]$MssIntermediateDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string[]]$TemplatePaths = @()
)

$ErrorActionPreference = 'Stop'
$recipeRepo = Split-Path $PSScriptRoot -Parent
$recipeDeps = (Resolve-Path -LiteralPath $DependenciesRoot).Path
$recipeIntermediate = (Resolve-Path -LiteralPath $MssIntermediateDirectory).Path
$recipeTemplates = @($TemplatePaths | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
$recipeLibrary = Join-Path $recipeDeps 'mss32\Lib\ScenarioGenerator_Release_Win32.lib'
$recipeLuaSources = @(Get-ChildItem -LiteralPath (Join-Path $recipeRepo 'lua') -Filter '*.c' -File |
    Where-Object { $_.Name -notin @('lua.c', 'luac.c') })
if ($recipeLuaSources.Count -eq 0) { throw 'Lua sources are unavailable' }
$recipeLuaObjects = @($recipeLuaSources | ForEach-Object {
    (Resolve-Path -LiteralPath (Join-Path $recipeIntermediate ($_.BaseName + '.obj'))).Path
})
if (-not (Test-Path -LiteralPath $recipeLibrary -PathType Leaf)) { throw 'RSG Release library is unavailable' }
$recipeOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
$recipeIncludes = @(
    (Join-Path $recipeRepo 'mss32\include'), (Join-Path $recipeRepo 'lua'),
    (Join-Path $recipeDeps 'sol2\single\include'),
    (Join-Path $recipeDeps 'D2RSG\ScenarioGenerator\src'),
    (Join-Path $recipeDeps 'D2RSG\ScenarioGenerator\src\scenario')
) | ForEach-Object { '/I' + $_ }
Push-Location $recipeOutput.FullName
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS `
        @recipeIncludes (Join-Path $recipeRepo 'tests\scenariotemplaterecipe_test.cpp') `
        (Join-Path $recipeRepo 'mss32\src\scenariotemplaterecipe.cpp') `
        @recipeLuaObjects $recipeLibrary /Fescenariotemplaterecipe_test.exe `
        /link /LTCG /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw 'Recipe test build failed; executable was not run' }
    & '.\scenariotemplaterecipe_test.exe' @recipeTemplates
    if ($LASTEXITCODE -ne 0) { throw 'Recipe regression test failed' }
} finally {
    Pop-Location
}
