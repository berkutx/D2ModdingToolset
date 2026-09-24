param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('Debug', 'Release', 'Both')][string]$Configuration = 'Both',
    [string]$ProductionRoot
)
$ErrorActionPreference = 'Stop'
$simRepo = Split-Path $PSScriptRoot -Parent
if (!$ProductionRoot) { $ProductionRoot = $simRepo }
$ProductionRoot = (Resolve-Path -LiteralPath $ProductionRoot).Path
$simOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
$sources = @(
    (Join-Path $simRepo 'tests\simturns_lobby_startup_regression_test.cpp'),
    (Join-Path $ProductionRoot 'mss32\src\simturns\lobby_transport.cpp'),
    (Join-Path $ProductionRoot 'mss32\src\simturns\coordinator_port.cpp'),
    (Join-Path $ProductionRoot 'mss32\src\simturns\protocol.cpp'),
    (Join-Path $ProductionRoot 'mss32\src\simturns\control_client_core.cpp'),
    (Join-Path $ProductionRoot 'mss32\src\simturns\turn_context.cpp')
)
$stubInclude = Join-Path $simRepo 'tests\stubs\simturns_lobby_startup'
$mssInclude = Join-Path $ProductionRoot 'mss32\include'
$configurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
Push-Location $simOutput.FullName
$transcriptStarted = $false
try {
    Start-Transcript -LiteralPath (Join-Path $simOutput.FullName 'simturns_lobby_startup_regression.transcript.txt') -Force
    $transcriptStarted = $true
    Write-Output "Production source: $ProductionRoot"
    Get-FileHash ($sources + @((Join-Path $mssInclude 'simturns\native_notification_policy.h'))) |
        Format-List Path, Hash
    Get-FileHash (@($PSCommandPath) + @(Get-ChildItem -LiteralPath $stubInclude -Recurse -File |
        Select-Object -ExpandProperty FullName)) | Format-List Path, Hash
    foreach ($current in $configurations) {
        $evidence = Join-Path $simOutput.FullName ("simturns_lobby_startup_regression_{0}.log" -f $current)
        Set-Content -LiteralPath $evidence -Value ("production-source lobby startup regression: {0}" -f $current)
        $exeName = "simturns_lobby_startup_regression_{0}.exe" -f $current
        $compileFlags = if ($current -eq 'Debug') {
            @('/MTd', '/Od', '/Zi', '/DDEBUG')
        } else {
            @('/MT', '/O2', '/DNDEBUG')
        }
        & cl.exe /nologo /std:c++17 /EHsc /DD2_SIMTURNS @compileFlags `
            ('/I' + $stubInclude) ('/I' + $mssInclude) @sources `
            ('/Fe' + $exeName) ('/Fd' + $current + '.pdb') /link /INCREMENTAL:NO 2>&1 `
            | Tee-Object -FilePath $evidence -Append
        $compileExit = $LASTEXITCODE
        if ($compileExit -ne 0) {
            throw "$current lobby startup regression compilation failed; no stale binary will be run"
        }
        & ('.\' + $exeName) 2>&1 | Tee-Object -FilePath $evidence -Append
        $testExit = $LASTEXITCODE
        if ($testExit -ne 0) {
            throw "$current lobby startup regression is red"
        }
    }
} finally {
    if ($transcriptStarted) { Stop-Transcript }
    Pop-Location
}
