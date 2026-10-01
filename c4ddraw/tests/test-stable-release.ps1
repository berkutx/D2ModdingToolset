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
Check ($runtime -contains 'C4dll-R-v2.0-test-bundle/WINDOW-WORKAREA-RU.txt') 'Work-area window instructions are bundled'
Check ($runtime -contains 'C4dll-R-v2.0-test-bundle/NETWORK_TRACE.md' -and
       $runtime -contains 'C4dll-R-v2.0-test-bundle/Tools/analyze-event-trace.py') 'Network diagnostics guide and optional analyzer are bundled'
$expectedShaders = @(
    'Shaders/interpolation/lanczos-bicubic.glsl',
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
# Exercise the actual workflow computation offline; no release/publish step is executed.
$versionMatch = [regex]::Match($releaseWorkflow,
    '(?ms)^      - name: Compute version\r?\n        id: ver\r?\n        run: \|\r?\n(?<body>(?:^          [^\r\n]*(?:\r?\n|$)|^\r?\n)+)')
Check $versionMatch.Success 'Release version computation can be exercised offline'
$versionScript = [regex]::Replace($versionMatch.Groups['body'].Value, '(?m)^          ', '')
function Compute-ReleaseFixture([string]$Event, [string]$RefType, [string]$RefName, [string]$Label) {
    $outputPath = Join-Path $run ('version-' + [Guid]::NewGuid().ToString('N') + '.txt')
    $savedEnvironment = @{}
    foreach ($name in @('GITHUB_EVENT_NAME','GITHUB_REF_TYPE','GITHUB_REF_NAME','GITHUB_OUTPUT')) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        $env:GITHUB_EVENT_NAME = $Event
        $env:GITHUB_REF_TYPE = $RefType
        $env:GITHUB_REF_NAME = $RefName
        $env:GITHUB_OUTPUT = $outputPath
        # Labels in this fixture are fixed literals, not arbitrary command input.
        $body = $versionScript.Replace('${{ github.event.inputs.version }}', $Label)
        & ([ScriptBlock]::Create($body)) 6>$null
        $values = @{}
        foreach ($line in Get-Content -LiteralPath $outputPath) {
            $parts = $line -split '=', 2
            $values[$parts[0]] = $parts[1]
        }
        return $values
    } finally {
        foreach ($name in $savedEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
        }
    }
}
$previewNotesRelative = 'c4ddraw/release/RENDER-PERFORMANCE-PREVIEW.md'
$stableNotesRelative = 'c4ddraw/release/RELEASE_NOTES.md'
$tagCases = @(
    @{ Version = 'v2.3.0'; Pre = 'false'; Notes = $stableNotesRelative },
    @{ Version = 'v2.3.1'; Pre = 'false'; Notes = $stableNotesRelative },
    @{ Version = 'v2.3.1-preview.2'; Pre = 'true'; Notes = $previewNotesRelative },
    @{ Version = 'v2.3.1-PREVIEW2'; Pre = 'true'; Notes = $previewNotesRelative },
    @{ Version = 'v2.3.1-rc1'; Pre = 'true'; Notes = $stableNotesRelative },
    @{ Version = 'v2.3.1-alpha.1'; Pre = 'true'; Notes = $stableNotesRelative },
    @{ Version = 'v2.3.1-beta2'; Pre = 'true'; Notes = $stableNotesRelative },
    @{ Version = 'v2.3.1-dev.1'; Pre = 'true'; Notes = $stableNotesRelative }
)
foreach ($case in $tagCases) {
    $fixture = Compute-ReleaseFixture 'push' 'tag' ('c4dll-r-' + $case.Version) ''
    Check ($fixture.tag -eq ('c4dll-r-' + $case.Version) -and $fixture.ver -eq $case.Version) "Tag and version preserved: $($case.Version)"
    Check ($fixture.pre -eq $case.Pre -and $fixture.notesSource -eq $case.Notes) "Release classification and notes: $($case.Version)"
}
$manualStable = Compute-ReleaseFixture 'workflow_dispatch' 'tag' 'c4dll-r-v2.3.1' 'v2.3.1'
Check ($manualStable.pre -eq 'true' -and $manualStable.notesSource -eq $stableNotesRelative) 'Manual stable label remains a prerelease with stable notes'
$manualPreview = Compute-ReleaseFixture 'workflow_dispatch' 'branch' 'main' 'v2.3.1-preview.2'
Check ($manualPreview.pre -eq 'true' -and $manualPreview.notesSource -eq $previewNotesRelative) 'Manual preview label receives performance preview notes'
$manualDefault = Compute-ReleaseFixture 'workflow_dispatch' 'branch' 'main' ''
Check ($manualDefault.pre -eq 'true' -and $manualDefault.ver -match '^dev-[0-9a-f]+$' -and $manualDefault.notesSource -eq $stableNotesRelative) 'Empty manual label retains dev prerelease behavior'
Check ($releaseWorkflow.Contains('Get-Content -LiteralPath "${{ steps.ver.outputs.notesSource }}" -Raw -Encoding utf8')) 'Archive notes use the version-selected template'
Check ($releaseWorkflow.Contains('"notes=$notesPath" | Out-File $env:GITHUB_OUTPUT -Append') -and
       $releaseWorkflow.Contains('$notesPath = "${{ steps.pkg.outputs.notes }}"')) 'GitHub Release reuses the exact packaged notes file'
$previewVersion = 'v2.3.1-preview.2'
$previewTemplatePath = Join-Path $repo $manualPreview.notesSource
$previewResult = & (Join-Path $repo 'c4ddraw/tools/package-release.ps1') -BuildDirectory $build -Version $previewVersion -OutputRoot $run -ReleaseNotesFile $previewTemplatePath
$expectedPreviewNotes = (Get-Content -LiteralPath $previewTemplatePath -Raw -Encoding utf8).Replace('__VER__', $previewVersion).Replace('__ZIP__', "C4dll-R-$previewVersion.zip")
$previewNotes = Get-Content -LiteralPath (Join-Path $previewResult.Stage 'RELEASE_NOTES.md') -Raw -Encoding utf8
Check ($previewNotes.TrimEnd() -eq $expectedPreviewNotes.TrimEnd() -and $previewNotes -notmatch '__(VER|ZIP)__') 'Preview package resolves its own version and ZIP in performance notes'
$previewZip = [IO.Compression.ZipFile]::OpenRead($previewResult.Archive)
try {
    $entry = $previewZip.GetEntry("C4dll-R-$previewVersion/RELEASE_NOTES.md")
    $reader = [IO.StreamReader]::new($entry.Open())
    try { $archivedNotes = $reader.ReadToEnd() } finally { $reader.Dispose() }
    Check ($archivedNotes -eq $previewNotes) 'Actual preview ZIP contains the selected performance notes unchanged'
} finally { $previewZip.Dispose() }
$stableExpected = (Get-Content -LiteralPath (Join-Path $repo $stableNotesRelative) -Raw -Encoding utf8).Replace('__VER__', 'v2.0-test-bundle').Replace('__ZIP__', 'C4dll-R-v2.0-test-bundle.zip')
$stableActual = Get-Content -LiteralPath (Join-Path $result.Stage 'RELEASE_NOTES.md') -Raw -Encoding utf8
Check ($stableActual.TrimEnd() -eq $stableExpected.TrimEnd()) 'Default stable package continues using stable release notes'
Check ($releaseWorkflow.Contains('Copy-Item "c4ddraw/release/WINDOW-WORKAREA-RU.txt" "$stage/WINDOW-WORKAREA-RU.txt"')) 'Player release copies work-area instructions'
Check ($releaseWorkflow -match '(?s)\$expectedFiles = @\(.*?''WINDOW-WORKAREA-RU\.txt''.*?\)') 'Player release ZIP allowlist includes work-area instructions'
foreach ($workflow in @('c4ddraw.yml','c4dll-r-release.yml')) {
    $text = Get-Content -LiteralPath (Join-Path $repo ('.github/workflows/' + $workflow)) -Raw
    foreach ($suite in @('run-workarea-tests.py c4ddraw/build/cnc-ddraw', 'run-windowmonitor-tests.py', 'run-windowstretch-tests.py', 'run-colorkey16-tests.py', 'run-blend565-tests.py')) {
        $command = 'python c4ddraw/tests/' + $suite
        $guardedCommand = [regex]::Escape($command) + '\r?\n\s+if \(\$LASTEXITCODE -ne 0\) \{ throw '
        Check ($text -match $guardedCommand) "CI runs and checks $suite`: $workflow"
        Check ($text.IndexOf($command) -gt $text.IndexOf('./c4ddraw/build.ps1')) "CI runs $suite after the build: $workflow"
    }
}
Check ($releaseWorkflow -notmatch '\$symzip|C4dll-R\.pdb.*timer\.pdb') 'Player release does not build or publish a symbols archive'
Check ($releaseWorkflow -match 'gh release upload \$tag \$zip --clobber' -and
       @($releaseWorkflow -split "`n" | Where-Object { $_ -match '^\s*gh release upload\s' }).Count -eq 1) 'Player release uploads exactly one ready-to-use ZIP'
Check (Test-Path -LiteralPath (Join-Path $repo 'c4ddraw/plugins/unitinfo/unitinfo.cpp')) 'Bundled Twitch source remains in Git tree'
Write-Output "Completed $checks checks. Evidence: $run"
