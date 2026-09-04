param()
# Contract tests use disposable fixtures only; the production Shaders tree is read-only.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$c4ddraw = Split-Path $PSScriptRoot -Parent
$validator = Join-Path $c4ddraw 'tools/validate-shader-bundle.ps1'
$sourceRoot = Join-Path $c4ddraw 'release'
$run = Join-Path $c4ddraw ('../.diagnostics/shader-bundle-test-' + [Guid]::NewGuid().ToString('N'))
$complete = Join-Path $run 'complete'
New-Item -ItemType Directory -Path $complete -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceRoot 'Shaders') -Destination $complete -Recurse

$checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
    Write-Output "PASS: $Message"
}
function Expect-Failure([string]$Root, [string]$ExpectedPath) {
    try {
        & $validator -BundleRoot $Root -RequireExactShaderSet | Out-Null
    } catch {
        return $_.Exception.Message.Contains($ExpectedPath)
    }
    return $false
}
function Expect-SourceFailure([string]$Source, [string]$ExpectedText) {
    try {
        & $validator -BundleRoot $complete -FeatureMenuSource $Source `
            -RequireExactShaderSet | Out-Null
    } catch {
        return $_.Exception.Message.Contains($ExpectedText)
    }
    return $false
}

$result = & $validator -BundleRoot $complete -RequireExactShaderSet
Check ($result.Valid -and $result.MenuEntries -eq 8 -and $result.RequiredFileCount -eq 10) `
      'Complete bundle maps eight menu filters to ten required files'

$missingIndex = 0
foreach ($relative in $result.RequiredPaths) {
    $missingIndex++
    $missingRoot = Join-Path $run ("missing-{0:d2}" -f $missingIndex)
    New-Item -ItemType Directory -Path $missingRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'Shaders') -Destination $missingRoot -Recurse
    Remove-Item -LiteralPath (Join-Path $missingRoot $relative)
    Check (Expect-Failure $missingRoot $relative) "Missing required shader is rejected: $relative"
}

$emptyShader = Join-Path $run 'empty-shader'
New-Item -ItemType Directory -Path $emptyShader | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceRoot 'Shaders') -Destination $emptyShader -Recurse
$emptyRelative = 'Shaders\nearest-neighbor.glsl'
Clear-Content -LiteralPath (Join-Path $emptyShader $emptyRelative)
Check (Expect-Failure $emptyShader $emptyRelative) 'Empty shader file is rejected'

$productionMenu = Join-Path $c4ddraw 'features/featuremenu.cpp'
$menuText = Get-Content -LiteralPath $productionMenu -Raw -Encoding utf8
$crtToken = '"Shaders\\crt\\crt-lottes-fast-no-warp-bilinear.glsl", nullptr'
$decoySource = Join-Path $run 'featuremenu-comment-decoy.cpp'
$decoyText = $menuText.Replace(
    $crtToken,
    'nullptr /* "Shaders\\crt\\crt-lottes-fast-no-warp-bilinear.glsl" */, nullptr')
Check ($decoyText -ne $menuText) 'Malformed-row fixture changed the real shader table'
Set-Content -LiteralPath $decoySource -Value $decoyText -Encoding utf8
Check (Expect-SourceFailure $decoySource 'structurally declare') `
      'A shader path hidden in a comment cannot compensate for a malformed menu row'

$xbrzPass = 'Shaders\\xbrz\\xbrz-freescale-multipass.glsl.pass1'
$fsrPass = 'Shaders\\interpolation\\fsr.glsl.pass1'
$swappedSource = Join-Path $run 'featuremenu-swapped-pass1.cpp'
$swappedText = $menuText.Replace($xbrzPass, '__C4_XBRZ_PASS1__')
$swappedText = $swappedText.Replace($fsrPass, $xbrzPass).Replace('__C4_XBRZ_PASS1__', $fsrPass)
Check ($swappedText -ne $menuText) 'Swapped-pass fixture changed the real shader table'
Set-Content -LiteralPath $swappedSource -Value $swappedText -Encoding utf8
Check (Expect-SourceFailure $swappedSource 'Wrong requiredPass1') `
      'FSR/xBRZ companion files must stay attached to their own menu entries'

Write-Output "Completed $checks checks. Evidence: $run"
