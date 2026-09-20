[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$extensionRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$webRoot = Join-Path $extensionRoot 'web'
$manifest = Get-Content -LiteralPath (Join-Path $extensionRoot 'package.json') -Raw | ConvertFrom-Json
$version = [string]$manifest.version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a numeric Twitch extension version in package.json.' }

# Explicit allowlist: never include the native plugin, bridge, OBS configuration or credentials.
$assets = @(
    'broadcaster.mjs', 'config.html', 'control.css', 'game-text.mjs', 'live_config.html',
    'overlay.css', 'protocol.mjs', 'video_overlay.html', 'viewer.mjs'
)
foreach ($asset in $assets) {
    $item = Get-Item -LiteralPath (Join-Path $webRoot $asset)
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Expected a regular web asset: $asset"
    }
}

$outputFull = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputFull | Out-Null
$archivePath = Join-Path $outputFull "disciples-2-battle-info-$version.zip"
$temporaryPath = Join-Path $outputFull ('.extension-' + [guid]::NewGuid().ToString('N') + '.zip.tmp')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = $null
try {
    $archive = [IO.Compression.ZipFile]::Open($temporaryPath, [IO.Compression.ZipArchiveMode]::Create)
    foreach ($asset in $assets) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, (Join-Path $webRoot $asset), $asset, [IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
    $archive.Dispose()
    $archive = [IO.Compression.ZipFile]::OpenRead($temporaryPath)
    $entries = @($archive.Entries | ForEach-Object { $_.FullName })
    if ($entries.Count -ne $assets.Count -or @(Compare-Object $assets $entries).Count -ne 0) {
        throw 'The generated ZIP does not contain exactly the approved web assets.'
    }
    $archive.Dispose()
    $archive = $null
    Move-Item -LiteralPath $temporaryPath -Destination $archivePath -Force
    [pscustomobject]@{
        Archive = $archivePath
        Version = $version
        WebFiles = $assets.Count
        Bytes = (Get-Item -LiteralPath $archivePath).Length
        SHA256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    }
}
finally {
    if ($null -ne $archive) { $archive.Dispose() }
    if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath -Force }
}
