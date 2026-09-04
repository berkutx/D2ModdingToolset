param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidatePattern('^v[0-9][0-9A-Za-z.-]{0,79}$')][string]$Version = 'v1.9-20260904',
    [string]$OutputRoot = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
)
# Offline packaging only. Existing output is an error; partial output is never removed.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path -LiteralPath (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)).Path
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$out = (Resolve-Path -LiteralPath $OutputRoot).Path.TrimEnd('\', '/')
if ($out -ne $repo -and -not $out.StartsWith($repo.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'OutputRoot must be inside this workspace' }
if (-not (Test-Path -LiteralPath $build -PathType Container)) { throw 'BuildDirectory is not a directory' }
$name = "C4dll-R-$Version"
$stage = Join-Path $out "pkg/$name"
$zip = Join-Path $out "$name.zip"
$symzip = Join-Path $out "$name-symbols.zip"
$checksums = Join-Path $out "$name.sha256"
foreach ($target in @($stage, $zip, $symzip, $checksums)) { if (Test-Path -LiteralPath $target) { throw "Refusing existing output: $target" } }
$files = [ordered]@{
    'C4dll-R.dll' = "$build/bin/Release/C4dll-R.dll"
    'Mods/timer.c4p' = "$build/plugins/timer/bin/timer.c4p"
    'Mods/twitchstat.c4p' = "$build/plugins/unitinfo/bin/twitchstat.c4p"
    'INSTALL.txt' = "$repo/c4ddraw/release/INSTALL.txt"
    'C4PLUGINS.txt' = "$repo/c4ddraw/release/C4PLUGINS.txt"
    'C4plugins.ini' = "$repo/c4ddraw/release/C4plugins.ini"
    'ddraw.ini' = "$repo/c4ddraw/release/ddraw.ini"
    'THIRD_PARTY_NOTICES.txt' = "$repo/c4ddraw/release/THIRD_PARTY_NOTICES.txt"
    'LICENSE' = "$repo/LICENSE"
    'MESSAGE_BATCHING.md' = "$repo/c4ddraw/MESSAGE_BATCHING.md"
    'NETWORK_TRACE.md' = "$repo/c4ddraw/NETWORK_TRACE.md"
}
$symbols = [ordered]@{
    'C4dll-R.pdb' = "$build/bin/Release/C4dll-R.pdb"
    'timer.pdb' = "$build/plugins/timer/bin/timer.pdb"
    'twitchstat.pdb' = "$build/plugins/unitinfo/bin/twitchstat.pdb"
}
$shaderRoot = "$repo/c4ddraw/release/Shaders"
$shaderFiles = @(Get-ChildItem -LiteralPath $shaderRoot -Recurse -File)
if (-not $shaderFiles.Count) { throw 'Shaders directory is empty' }
foreach ($shader in $shaderFiles) { $files['Shaders/' + $shader.FullName.Substring($shaderRoot.Length + 1).Replace('\', '/')] = $shader.FullName }
foreach ($inputFile in @($files.Values) + @($symbols.Values) + @("$repo/c4ddraw/release/RELEASE_NOTES.md")) {
    if (-not (Test-Path -LiteralPath $inputFile -PathType Leaf) -or (Get-Item -LiteralPath $inputFile).Length -eq 0) { throw "Missing/empty input: $inputFile" }
}
$notes = (Get-Content -LiteralPath "$repo/c4ddraw/release/RELEASE_NOTES.md" -Raw -Encoding utf8).Replace('__VER__', $Version).Replace('__ZIP__', "$name.zip")
if ($notes -match '__(VER|ZIP)__') { throw 'Unresolved release-note tokens' }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Commit([string]$Ref) { $value = & git -C $repo rev-parse --verify "$Ref^{commit}" 2>$null; if ($LASTEXITCODE -eq 0) { "$value" } else { $null } }
$head = Commit 'HEAD'
if (-not $head) { throw 'Cannot resolve workspace HEAD' }
$sourceHashes = [ordered]@{}
$sources = @(Get-ChildItem -LiteralPath "$repo/c4ddraw/features", "$repo/c4ddraw/patches" -Recurse -File) + @(Get-Item -LiteralPath $PSCommandPath, "$repo/c4ddraw/build.ps1")
foreach ($source in $sources | Sort-Object FullName -Unique) { $sourceHashes[$source.FullName.Substring($repo.Length + 1).Replace('\', '/')] = Hash $source.FullName }
$fileHashes = [ordered]@{}; foreach ($entry in $files.GetEnumerator()) { $fileHashes[$entry.Key] = Hash $entry.Value }
$symbolHashes = [ordered]@{}; foreach ($entry in $symbols.GetEnumerator()) { $symbolHashes[$entry.Key] = Hash $entry.Value }
$info = [ordered]@{
    Version = $Version; PackagedUtc = [DateTime]::UtcNow.ToString('o'); Head = $head
    BaselineTags = [ordered]@{ 'c4dll-r-v1.8' = (Commit 'c4dll-r-v1.8'); 'c4dll-r-v1.9' = (Commit 'c4dll-r-v1.9') }
    WorkingTreeStatus = @(& git -C $repo status --porcelain --untracked-files=normal 2>$null)
    SourceNote = 'Source hashes describe the working tree at packaging; dirty changes are not represented by HEAD alone.'
    Scope = 'Wrapper and native plugins only. MSS is neither rebuilt nor included; no game instance is changed.'
    SymbolEvidence = 'Adjacent DLL/plugin and PDB from the supplied isolated build directory; SHA256 below. PDB GUID/age validation is separate.'
    SourceSHA256 = $sourceHashes; PackageFileSHA256 = $fileHashes; SymbolSHA256 = $symbolHashes
}
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($entry in $files.GetEnumerator()) {
    $destination = Join-Path $stage $entry.Key
    $parent = Split-Path $destination -Parent
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
    Copy-Item -LiteralPath $entry.Value -Destination $destination -ErrorAction Stop
    if ((Hash $destination) -ne $fileHashes[$entry.Key]) { throw "Input changed during copy: $($entry.Key)" }
}
Set-Content -LiteralPath "$stage/RELEASE_NOTES.md" -Value $notes -Encoding utf8
$info | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$stage/BUILD_INFO.json" -Encoding utf8
$fileHashes['RELEASE_NOTES.md'] = Hash "$stage/RELEASE_NOTES.md"
$fileHashes['BUILD_INFO.json'] = Hash "$stage/BUILD_INFO.json"
Compress-Archive -LiteralPath $stage -DestinationPath $zip
Compress-Archive -LiteralPath @($symbols.Values) -DestinationPath $symzip
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Validate-Zip([string]$Path, [string]$Prefix, $Expected) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $seen = @{}
        foreach ($entry in $archive.Entries) {
            $entryName = $entry.FullName.Replace('\', '/')
            if ($entryName.EndsWith('/')) { continue }
            if (-not $entryName.StartsWith($Prefix, [StringComparison]::Ordinal) -or $entry.Length -eq 0) { throw "Invalid/empty ZIP entry: $entryName" }
            $relative = $entryName.Substring($Prefix.Length)
            if ($relative -match '(?i)(^|/)mss32\.dll$|\.(exe|log|csv|dmp)$' -or -not $Expected.Contains($relative) -or $seen.ContainsKey($relative)) { throw "Unexpected/duplicate ZIP entry: $relative" }
            $stream = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() } finally { $stream.Dispose(); $sha.Dispose() }
            if ($actual -ne $Expected[$relative]) { throw "ZIP hash mismatch: $relative" }
            $seen[$relative] = $true
        }
        if ($seen.Count -ne $Expected.Count) { throw "Missing ZIP entries in $Path" }
    } finally { $archive.Dispose() }
}
Validate-Zip $zip "$name/" $fileHashes
Validate-Zip $symzip '' $symbolHashes
$checksumLines = @("$(Hash $zip)  $name.zip", "$(Hash $symzip)  $name-symbols.zip")
$checksumLines | Set-Content -LiteralPath $checksums -Encoding ascii
[pscustomobject]@{ Stage = $stage; Archive = $zip; Symbols = $symzip; SHA256File = $checksums; Validated = $true }
