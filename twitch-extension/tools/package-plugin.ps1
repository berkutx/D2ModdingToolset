[CmdletBinding()]
param(
    [string]$PluginPath = (Join-Path $PSScriptRoot '..\..\c4ddraw\plugins\unitinfo\bin\Release\twitchstat.c4p'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$extensionRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifest = Get-Content -LiteralPath (Join-Path $extensionRoot 'package.json') -Raw | ConvertFrom-Json
$version = [string]$manifest.version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a numeric version in package.json.' }

$plugin = Get-Item -LiteralPath $PluginPath
if ($plugin.PSIsContainer -or ($plugin.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
    $plugin.Extension -ine '.c4p' -or $plugin.Length -lt 512) {
    throw 'Expected a regular, previously built twitchstat.c4p file.'
}
$pluginHash = (Get-FileHash -LiteralPath $plugin.FullName -Algorithm SHA256).Hash
$readme = @"
Disciples II Battle Info / Twitch Stat $version

Установка для стримера

Нужны Windows, поддерживаемая сборка Disciples II Russobit/MNS и совместимый
враппер C4dllR. Node.js и отдельная программа соединения не нужны.

1. Полностью закройте игру. Скопируйте папку Mods из этого архива в папку
   с Discipl2.exe. Если Mods/twitchstat.c4p уже есть, сохраните резервную копию.
   Не заменяйте остальные плагины, DLL враппера и C4Plugins.ini.
2. Запустите игру и включите Twitch Stat в меню плагинов.
3. Установите Disciples II Battle Info в Twitch и активируйте как Video Overlay.
   Пока версия находится в Hosted Test, аккаунт должен быть разрешён автором.
4. Откройте Live Configuration расширения в панели управления трансляцией,
   нажмите «Подключить игру» и разрешите появившееся окно соединения.
5. Оставьте панель Twitch и окно соединения открытыми на время эфира.
   Окно соединения показывает PID игры: проверьте, что выбрано нужное окно.
6. В OBS показывайте полное игровое изображение без обрезки и смещения.
   Проверенный режим: клиентский кадр 1366x768, растяжение окон 100%.
   Дополнительный Ctrl+Wheel zoom и внутренние чёрные поля не поддерживаются.

Зрителям ничего устанавливать на компьютер не нужно.

Если используете ручные настройки, в существующем C4Plugins.ini:
[TwitchStat]
Enabled=1
Preview=0
Profile=0

Не включайте Twitch Stat сразу у нескольких игровых клиентов. Встроенное
соединение использует 127.0.0.1:8765 и данные того процесса, где включён плагин.
Прежний диагностический Node-мост на этом порту нужно закрыть.
Нет связи: проверьте, что игра запущена и Twitch Stat включён, затем
подключитесь заново из Twitch. Вне поддерживаемого боя карточки скрываются.

Пакет содержит только Mods/twitchstat.c4p и этот README.txt.
Содержимое игры, настройки OBS, секреты Twitch и DLL враппера не включены.
Плагин содержит локальный сервер и его веб-интерфейс; отдельные файлы не нужны.
Публичный доступ Twitch зависит от статуса Review/Release расширения.

SHA-256 twitchstat.c4p: $pluginHash
"@

$outputFull = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputFull | Out-Null
$archivePath = Join-Path $outputFull "twitchstat-plugin-$version.zip"
$temporaryPath = Join-Path $outputFull ('.plugin-' + [guid]::NewGuid().ToString('N') + '.zip.tmp')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = $null
try {
    $archive = [IO.Compression.ZipFile]::Open($temporaryPath, [IO.Compression.ZipArchiveMode]::Create)
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $archive, $plugin.FullName, 'Mods/twitchstat.c4p', [IO.Compression.CompressionLevel]::Optimal
    ) | Out-Null
    $entry = $archive.CreateEntry('README.txt', [IO.Compression.CompressionLevel]::Optimal)
    $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
    try { $writer.Write($readme) }
    finally { $writer.Dispose() }
    $archive.Dispose()
    $archive = [IO.Compression.ZipFile]::OpenRead($temporaryPath)
    $entries = @($archive.Entries | ForEach-Object { $_.FullName })
    $expected = @('Mods/twitchstat.c4p', 'README.txt')
    if ($entries.Count -ne 2 -or @(Compare-Object $expected $entries).Count -ne 0) {
        throw 'Plugin ZIP must contain exactly Mods/twitchstat.c4p and README.txt.'
    }
    $archive.Dispose()
    $archive = $null
    Move-Item -LiteralPath $temporaryPath -Destination $archivePath -Force
    [pscustomobject]@{
        Archive = $archivePath
        Version = $version
        Bytes = (Get-Item $archivePath).Length
        PluginSHA256 = $pluginHash
        SHA256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    }
}
finally {
    if ($null -ne $archive) { $archive.Dispose() }
    if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath -Force }
}
