param()
# Packaging-contract test only: tiny inert fixtures are not executable game binaries.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$run = Join-Path $repo ('.diagnostics/stable-release-test-' + [Guid]::NewGuid().ToString('N'))
$build = Join-Path $run 'build'
New-Item -ItemType Directory -Path $run, $build | Out-Null
$checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:checks++
    Write-Output "PASS: $Message"
}
foreach ($relative in @('bin/Release/C4dll-R.dll','bin/Release/C4dll-R.pdb',
    'plugins/timer/bin/timer.c4p','plugins/timer/bin/timer.pdb',
    'plugins/unitinfo/bin/twitchstat.c4p','plugins/unitinfo/bin/twitchstat.pdb',
    'plugins/unreleased/bin/unreleased.c4p','plugins/unreleased/bin/unreleased.pdb')) {
    $path = Join-Path $build $relative
    New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force | Out-Null
    [IO.File]::WriteAllText($path, 'Inert packaging test fixture: ' + $relative)
}
foreach ($relative in @('c4ddraw/build.ps1','c4ddraw/tools/package-release.ps1',
    'c4ddraw/tools/validate-shader-bundle.ps1')) {
    $tokens = $null; $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $repo $relative), [ref]$tokens, [ref]$parseErrors)
    Check ($parseErrors.Count -eq 0) "PowerShell syntax: $relative"
}
$result = & (Join-Path $repo 'c4ddraw/tools/package-release.ps1') -BuildDirectory $build -Version 'v2.0-test-bundle' -OutputRoot $run
Check ($result.Validated -eq $true) 'Offline packager validates both archives'
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Entries([string]$Path) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try { @($zip.Entries | Where-Object { -not $_.FullName.EndsWith('/') } | ForEach-Object { $_.FullName.Replace('\','/') }) }
    finally { $zip.Dispose() }
}
$runtime = @(Entries $result.Archive)
$symbols = @(Entries $result.Symbols)
Check (@($runtime | Where-Object { $_ -match '\.c4p$' }).Count -eq 2) 'Runtime archive contains exactly two plugins'
Check ($runtime -contains 'C4dll-R-v2.0-test-bundle/Mods/timer.c4p') 'Timer is bundled'
Check ($runtime -contains 'C4dll-R-v2.0-test-bundle/Mods/twitchstat.c4p') 'Twitch Stat is bundled'
Check ($runtime -contains 'C4dll-R-v2.0-test-bundle/TWITCH-STREAMER-RU.md') 'Streamer instructions are bundled'
$expectedShaders = @(
    'Shaders/interpolation/lanczos2-sharp.glsl',
    'Shaders/xbrz/xbrz-freescale-multipass.glsl',
    'Shaders/xbrz/xbrz-freescale-multipass.glsl.pass1',
    'Shaders/interpolation/catmull-rom-bilinear.glsl',
    'Shaders/interpolation/fsr.glsl',
    'Shaders/interpolation/fsr.glsl.pass1',
    'Shaders/xbr/xbr-lv2-noblend.glsl',
    'Shaders/interpolation/bilinear.glsl',
    'Shaders/nearest-neighbor.glsl',
    'Shaders/crt/crt-lottes-fast-no-warp-bilinear.glsl'
)
$shaderPrefix = 'C4dll-R-v2.0-test-bundle/'
$runtimeShaders = @($runtime | Where-Object { $_ -match '(?i)/Shaders/.+\.glsl(?:\.pass1)?$' } |
    ForEach-Object { $_.Substring($shaderPrefix.Length) })
Check ($runtimeShaders.Count -eq $expectedShaders.Count -and
       @($expectedShaders | Where-Object { $runtimeShaders -notcontains $_ }).Count -eq 0) `
      'Runtime archive contains every shader menu file and both required passes'
Check (@($runtime + $symbols | Where-Object { $_ -match '(?i)unreleased\.(c4p|pdb)$' }).Count -eq 0) 'Unrelated plugin outputs cannot leak into either archive'
Check ($symbols.Count -eq 3 -and $symbols -contains 'C4dll-R.pdb' -and $symbols -contains 'timer.pdb' -and $symbols -contains 'twitchstat.pdb') 'Symbols belong to wrapper, Timer and Twitch Stat'
Check (Test-Path -LiteralPath (Join-Path $build 'plugins/unreleased/bin/unreleased.c4p')) 'Unrelated build input is preserved'
$ini = Get-Content -LiteralPath (Join-Path $result.Stage 'C4plugins.ini') -Raw
Check ($ini -match '(?m)^\[Timer\]' -and $ini -match '(?m)^\[TwitchStat\]' -and $ini -notmatch '(?mi)^\[UnitInfo\]') 'Release INI documents Timer and Twitch Stat'
$before = (Get-FileHash -LiteralPath $result.Archive -Algorithm SHA256).Hash
$refused = $false
try { & (Join-Path $repo 'c4ddraw/tools/package-release.ps1') -BuildDirectory $build -Version 'v2.0-test-bundle' -OutputRoot $run | Out-Null }
catch { if ($_.Exception.Message -like 'Refusing existing output:*') { $refused = $true } else { throw } }
Check $refused 'Existing output is not overwritten'
Check ((Get-FileHash -LiteralPath $result.Archive -Algorithm SHA256).Hash -eq $before) 'Refused repeat leaves the original archive intact'
$builder = Get-Content -LiteralPath (Join-Path $repo 'c4ddraw/build.ps1') -Raw
Check ($builder -match '\$twitchStatPluginOut' -and $builder -match '\$twitchStatPluginProj') 'Default builder includes Twitch Stat build and deploy source'
foreach ($workflow in @('c4ddraw.yml','c4dll-r-release.yml')) {
    $text = Get-Content -LiteralPath (Join-Path $repo ('.github/workflows/' + $workflow)) -Raw
    Check ($text -match 'plugins/unitinfo/bin' -and $text -match 'TWITCH-STREAMER-RU.md') "CI includes Twitch Stat and its instructions: $workflow"
}
$releaseWorkflow = Get-Content -LiteralPath (Join-Path $repo '.github/workflows/c4dll-r-release.yml') -Raw
Check ($releaseWorkflow -notmatch '\$symzip|C4dll-R\.pdb.*timer\.pdb') 'Player release does not build or publish a symbols archive'
Check ($releaseWorkflow -match 'gh release upload \$tag \$zip --clobber' -and
       @($releaseWorkflow -split "`n" | Where-Object { $_ -match '^\s*gh release upload\s' }).Count -eq 1) 'Player release uploads exactly one ready-to-use ZIP'
Check (Test-Path -LiteralPath (Join-Path $repo 'c4ddraw/plugins/unitinfo/unitinfo.cpp')) 'Bundled Twitch source remains in Git tree'
Write-Output "Completed $checks checks. Evidence: $run"
