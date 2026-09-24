param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$simRepo = Split-Path $PSScriptRoot -Parent
$simOutput = New-Item -ItemType Directory -Path $OutputDirectory -Force
Push-Location $simOutput.FullName
try {
    foreach ($simTest in @('simturns_control_client_core', 'simturns_lobby_port',
                           'simturns_lobby_wire', 'simturns_native_apply_fence',
                           'simturns_native_notification_policy')) {
        $simSources = @((Join-Path $simRepo ('tests\' + $simTest + '_test.cpp')))
        if ($simTest -in @('simturns_control_client_core', 'simturns_lobby_port')) {
            $simSources += @((Join-Path $simRepo 'mss32\src\simturns\protocol.cpp'),
                (Join-Path $simRepo 'mss32\src\simturns\control_client_core.cpp'),
                (Join-Path $simRepo 'mss32\src\simturns\turn_context.cpp'))
        }
        if ($simTest -eq 'simturns_lobby_port') {
            $simSources += Join-Path $simRepo 'mss32\src\simturns\coordinator_port.cpp'
        }
        & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /DNDEBUG ('/I' + (Join-Path $simRepo 'mss32\include')) @simSources ('/Fe' + $simTest + '.exe') /link /INCREMENTAL:NO
        if ($LASTEXITCODE -ne 0) { throw "$simTest compilation failed; no stale binary will be run" }
        & ('.\' + $simTest + '.exe')
        if ($LASTEXITCODE -ne 0) { throw "$simTest failed" }
    }
} finally { Pop-Location }
