$ErrorActionPreference = 'Stop'
$turnRepo = Split-Path $PSScriptRoot -Parent
$turnSource = Get-Content -Raw -LiteralPath (Join-Path $turnRepo 'mss32/src/turnhooks.cpp')
$loaderSource = Get-Content -Raw -LiteralPath (Join-Path $turnRepo 'mss32/src/scripts.cpp')

# Source regression: keep the absent-file return narrow, after the native turn,
# while retaining the existing loader and its malformed-script diagnostics.
function Assert-OptionalTurnScript([string]$Source) {
    $guard = [regex]::Match($Source, '(?s)if \(!processTurnStart\)\s*\{\s*static const auto path = scriptsFolder\(\) / "turn\.lua";\s*(?://[^\r\n]*\r?\n\s*)?if \(!std::filesystem::exists\(path\)\)\s*\{\s*return;\s*\}\s*processTurnStart = getScriptFunction\(path, "processTurnStart", env, false, true\);\s*if \(!processTurnStart\)\s*\{\s*spdlog::error\("\[TURN\] failed to load processTurnStart"\);\s*return;\s*\}')
    if (!$guard.Success) { throw 'Missing optional-file guard or existing-script error path' }
    $nativeCall = 'beginTurnOrig(thisptr, playerId);'
    if ($Source.IndexOf($nativeCall) -lt 0 -or $Source.IndexOf($nativeCall) -ge $guard.Index) {
        throw 'Native turn must execute before optional Lua handling'
    }
    foreach ($required in @('(*processTurnStart)(playerView);',
        'spdlog::error("[TURN] Lua exception: {}", e.what());',
        'showErrorMessageBox(fmt::format("Failed to run turn.lua\nReason: {}", e.what()));')) {
        if (!$Source.Contains($required)) { throw "Existing Lua behavior lost: $required" }
    }
}

Assert-OptionalTurnScript $turnSource
# Verify the shared loader still treats only an absent optional file as quiet;
# unreadable/empty and malformed existing scripts retain their error messages.
if ($loaderSource -notmatch '(?s)executeScriptFile\([^)]*\)\s*\{\s*if \(!alwaysExists && !std::filesystem::exists\(path\)\)\s*return std::nullopt;\s*const auto& source = getSource\(path\);\s*if \(source.empty\(\)\)\s*\{\s*showErrorMessageBox') {
    throw 'Optional loader no longer separates absence from read failure'
}
if ($loaderSource -notmatch '(?s)auto env = executeScript\(source, result, bindScenario\);\s*if \(!result.valid\(\)\)\s*\{\s*const sol::error err = result;\s*showErrorMessageBox') {
    throw 'Malformed script diagnostic lost'
}

# Negative controls ensure this test catches the original bug and guard weakening.
$mutations = @(
    ($turnSource -replace 'if \(!std::filesystem::exists\(path\)\)\s*\{\s*return;\s*\}', ''),
    $turnSource.Replace('!std::filesystem::exists(path)', 'std::filesystem::exists(path)'),
    $turnSource.Replace('spdlog::error("[TURN] failed to load processTurnStart");', ''),
    $turnSource.Replace('beginTurnOrig(thisptr, playerId);', '')
)
foreach ($mutation in $mutations) {
    $rejected = $false
    try { Assert-OptionalTurnScript $mutation } catch { $rejected = $true }
    if (!$rejected) { throw 'A negative control unexpectedly passed' }
}
Write-Output 'PASS: optional turn.lua source regression, loader diagnostics, 4 negative controls'
