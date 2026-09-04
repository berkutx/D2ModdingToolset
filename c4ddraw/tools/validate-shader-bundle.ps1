[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BundleRoot,
    [string]$FeatureMenuSource = "",
    [switch]$RequireExactShaderSet
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$c4ddrawRoot = Split-Path $PSScriptRoot -Parent
if (-not $FeatureMenuSource) {
    $FeatureMenuSource = Join-Path $c4ddrawRoot 'features/featuremenu.cpp'
}
if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) {
    throw "Shader bundle root is not a directory: $BundleRoot"
}
if (-not (Test-Path -LiteralPath $FeatureMenuSource -PathType Leaf)) {
    throw "Feature menu source is missing: $FeatureMenuSource"
}

$bundle = (Resolve-Path -LiteralPath $BundleRoot).Path.TrimEnd('\', '/')
$menuText = Get-Content -LiteralPath $FeatureMenuSource -Raw -Encoding utf8
$table = [regex]::Match(
    $menuText,
    '(?s)const\s+\w+\s+kShaders\[\]\s*=\s*\{(?<body>.*?)\};\s*const\s+int\s+kShaderCount\s*=\s*(?<count>[^;]+);')
if (-not $table.Success) {
    throw 'Could not locate the kShaders menu table and kShaderCount'
}

$countExpression = $table.Groups['count'].Value.Trim()
$tableBody = [regex]::Replace($table.Groups['body'].Value, '(?s)/\*.*?\*/', '')
$tableBody = [regex]::Replace($tableBody, '(?m)//.*$', '')
$entryPattern = '(?s)\{\s*L"[^"]*"\s*,\s*L"[^"]*"\s*,\s*"(?<primary>Shaders\\\\[^"]+?\.glsl)"\s*,\s*(?:"(?<pass1>Shaders\\\\[^"]+?\.glsl\.pass1)"|nullptr)\s*\}'
$entries = @([regex]::Matches($tableBody, $entryPattern))
$unparsed = [regex]::Replace($tableBody, $entryPattern, '')
$unparsed = [regex]::Replace($unparsed, '[\s,]', '')
if ($entries.Count -ne 8 -or $unparsed.Length) {
    throw "Each of the 8 kShaders rows must structurally declare one primary file and a nullable requiredPass1 (rows=$($entries.Count), unparsed='$unparsed')"
}

$required = [Collections.Generic.List[string]]::new()
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $entries) {
    $primaryPath = $entry.Groups['primary'].Value.Replace('\\', '\')
    $pass1Path = if ($entry.Groups['pass1'].Success) {
        $entry.Groups['pass1'].Value.Replace('\\', '\')
    } else { $null }
    foreach ($relative in @($primaryPath, $pass1Path)) {
        if (-not $relative) { continue }
        if (-not $relative.StartsWith('Shaders\', [StringComparison]::OrdinalIgnoreCase) -or
            $relative.Contains('..')) {
            throw "Unsafe shader path in kShaders: $relative"
        }
        if (-not $seen.Add($relative)) {
            throw "Duplicate shader path in kShaders: $relative"
        }
        $required.Add($relative)
    }

    $expectedPass1 = switch ($primaryPath) {
        'Shaders\interpolation\fsr.glsl' { $primaryPath + '.pass1'; break }
        'Shaders\xbrz\xbrz-freescale-multipass.glsl' { $primaryPath + '.pass1'; break }
        default { $null }
    }
    if (-not [string]::Equals($pass1Path, $expectedPass1,
                              [StringComparison]::OrdinalIgnoreCase)) {
        throw "Wrong requiredPass1 for ${primaryPath}: expected '$expectedPass1', got '$pass1Path'"
    }
}

$primary = @($required | Where-Object { -not $_.EndsWith('.pass1', [StringComparison]::OrdinalIgnoreCase) })
$companions = @($required | Where-Object { $_.EndsWith('.pass1', [StringComparison]::OrdinalIgnoreCase) })
$literalCount = 0
$hasLiteralCount = [int]::TryParse($countExpression, [ref]$literalCount)
if ($primary.Count -ne 8 -or
    ($hasLiteralCount -and $literalCount -ne $primary.Count) -or
    (-not $hasLiteralCount -and $countExpression -notmatch 'sizeof\s*\(\s*kShaders\s*\)')) {
    throw "Shader menu contract must declare 8 unique primary files (table=$countExpression, files=$($primary.Count))"
}
$declaredCount = $primary.Count
if ($companions.Count -ne 2 -or
    $companions -inotcontains 'Shaders\interpolation\fsr.glsl.pass1' -or
    $companions -inotcontains 'Shaders\xbrz\xbrz-freescale-multipass.glsl.pass1') {
    throw 'Shader menu contract must declare the FSR and xBRZ .pass1 companions'
}

# Reset and fresh-install defaults must select the first validated menu preset, not an unlisted path.
$defaultShader = $primary[0]
$encodedDefault = $defaultShader.Replace('\', '\\')
$wrapperDefaults = Get-Content -LiteralPath (Join-Path $c4ddrawRoot 'features/wrapperdefaults.h') -Raw -Encoding utf8
$releaseIni = Get-Content -LiteralPath (Join-Path $c4ddrawRoot 'release/ddraw.ini') -Raw -Encoding utf8
if (-not $wrapperDefaults.Contains('{"shader", "' + $encodedDefault + '"}') -or
    $releaseIni -notmatch ('(?m)^shader=' + [regex]::Escape($defaultShader) + '\s*$')) {
    throw "Wrapper reset and release ddraw.ini must default to the first shader menu preset: $defaultShader"
}

$missing = [Collections.Generic.List[string]]::new()
foreach ($relative in $required) {
    $full = [IO.Path]::GetFullPath((Join-Path $bundle $relative))
    if (-not $full.StartsWith($bundle + [IO.Path]::DirectorySeparatorChar,
                              [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $full -PathType Leaf) -or
        (Get-Item -LiteralPath $full).Length -eq 0) {
        $missing.Add($relative)
    }
}
if ($missing.Count) {
    throw "Missing/empty shader menu assets under ${bundle}: $($missing -join ', ')"
}

if ($RequireExactShaderSet) {
    $shaderRoot = Join-Path $bundle 'Shaders'
    $actual = @(Get-ChildItem -LiteralPath $shaderRoot -Recurse -File |
        Where-Object { $_.Name -match '(?i)\.glsl(?:\.pass1)?$' } |
        ForEach-Object { $_.FullName.Substring($bundle.Length + 1) })
    $unexpected = @($actual | Where-Object { -not $seen.Contains($_) })
    if ($actual.Count -ne $required.Count -or $unexpected.Count) {
        throw "Shader bundle has unexpected/unlisted GLSL files: $($unexpected -join ', ')"
    }
}

[pscustomobject]@{
    Valid = $true
    MenuEntries = $declaredCount
    RequiredFileCount = $required.Count
    RequiredPaths = @($required)
}
