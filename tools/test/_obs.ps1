#requires -Version 7.0
# OBS Studio recording helper for the two-instance multiplayer tests. It captures the [HOST] and
# [CLIENT] game windows side by side, with a 20px empty gap between them, into one video the CI run
# always keeps as an artifact. Portable OBS (a release .zip, no install); a fresh config is written
# per run. The windows are matched by their [HOST]/[CLIENT] caption (set by testdrv/windowtag).
#
#   Install-Obs
#   $rec = Start-ObsRecording -OutDir $dir
#   ... run the test ...
#   Stop-ObsRecording        # -> finalizes; the .mkv + obs log are in $dir
#
# OBS on a headless CI runner is finicky (its own D3D renderer, capturing a Mesa-GL window), so every
# function logs to the host and the caller uploads $dir (video + obs log) for diagnosis.

$script:ObsVersion = '32.1.2'
$script:ObsRoot    = Join-Path $env:TEMP 'obs-portable'
$script:ObsBin     = Join-Path $script:ObsRoot 'bin\64bit'
$script:ObsExe     = Join-Path $script:ObsBin 'obs64.exe'
$script:ObsCfg     = Join-Path $script:ObsRoot 'config\obs-studio'
$script:ObsProc    = $null
$script:RecDir     = $null

# Download + extract the portable OBS release once; portable_mode.txt keeps its config local.
function Install-Obs {
    param([string]$Version = $script:ObsVersion)
    if (Test-Path $script:ObsExe) { Write-Host "[obs] already present"; return }
    $url = "https://github.com/obsproject/obs-studio/releases/download/$Version/OBS-Studio-$Version-Windows-x64.zip"
    $zip = Join-Path $env:TEMP 'obs.zip'
    Write-Host "[obs] downloading $Version ..."
    Invoke-WebRequest $url -OutFile $zip -UseBasicParsing
    New-Item -ItemType Directory -Force -Path $script:ObsRoot | Out-Null
    Write-Host "[obs] extracting ..."
    Expand-Archive -Path $zip -DestinationPath $script:ObsRoot -Force
    New-Item -ItemType File -Force -Path (Join-Path $script:ObsBin 'portable_mode.txt') | Out-Null
    if (-not (Test-Path $script:ObsExe)) { throw "[obs] obs64.exe not found after extract" }
    Write-Host "[obs] ready at $script:ObsExe"
}

# Write the OBS config (global + Simple-output profile + a scene collection with the two window
# captures laid out with the gap) and launch OBS recording. Returns the recording directory.
function Start-ObsRecording {
    param(
        [Parameter(Mandatory)][string]$OutDir,
        [string]$HostTitle   = 'Disciples II [HOST]',
        [string]$ClientTitle = 'Disciples II [CLIENT]',
        [int]$PaneW = 1024, [int]$PaneH = 768, [int]$Gap = 20, [int]$Fps = 15
    )
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $script:RecDir = $OutDir
    $canvasW = $PaneW * 2 + $Gap

    $prof = Join-Path $script:ObsCfg 'basic\profiles\CI'
    $scn  = Join-Path $script:ObsCfg 'basic\scenes'
    New-Item -ItemType Directory -Force -Path $prof, $scn | Out-Null

    # global.ini: select our profile + scene collection so the --startrecording launch uses them.
    @"
[General]
FirstRun=true
[Basic]
Profile=CI
ProfileDir=CI
SceneCollection=CI
SceneCollectionFile=CI
"@ | Set-Content (Join-Path $script:ObsCfg 'global.ini') -Encoding UTF8

    # Simple output: software x264 (no GPU on the runner), mkv (survives an abrupt kill), our canvas.
    @"
[General]
Name=CI
[Video]
BaseCX=$canvasW
BaseCY=$PaneH
OutputCX=$canvasW
OutputCY=$PaneH
FPSType=1
FPSInt=$Fps
[Output]
Mode=Simple
[SimpleOutput]
FilePath=$($OutDir -replace '\\','/')
RecFormat2=mkv
RecEncoder=x264
RecQuality=Small
VBitrate=4000
"@ | Set-Content (Join-Path $prof 'basic.ini') -Encoding UTF8

    # window string is "title:class:exe"; priority 1 = match by TITLE (so [HOST] vs [CLIENT] is
    # distinguished). method 2 = Windows Graphics Capture (captures the composited GL window; plain
    # BitBlt would grab black from a Mesa/OpenGL surface). client_area=false keeps the [HOST]/[CLIENT]
    # title bar visible in the frame.
    function srcJson([string]$name, [string]$title) {
        @"
    {
      "name": "$name",
      "id": "window_capture",
      "versioned_id": "window_capture",
      "settings": { "window": "$title::Discipl2.exe", "method": 2, "priority": 1, "cursor": false, "client_area": false }
    }
"@
    }
    function itemJson([string]$name, [int]$x) {
        @"
        { "name": "$name", "visible": true, "locked": false, "pos": { "x": $x, "y": 0 }, "scale": { "x": 1.0, "y": 1.0 }, "align": 5, "bounds_type": 2, "bounds_align": 0, "bounds": { "x": $PaneW, "y": $PaneH } }
"@
    }
    @"
{
  "current_scene": "CI",
  "current_program_scene": "CI",
  "scene_order": [ { "name": "CI" } ],
  "name": "CI",
  "sources": [
$(srcJson 'host' $HostTitle),
$(srcJson 'client' $ClientTitle),
    {
      "name": "CI",
      "id": "scene",
      "versioned_id": "scene",
      "settings": {
        "items": [
$(itemJson 'host' 0),
$(itemJson 'client' ($PaneW + $Gap))
        ]
      }
    }
  ]
}
"@ | Set-Content (Join-Path $scn 'CI.json') -Encoding UTF8

    Write-Host "[obs] launching (canvas ${canvasW}x${PaneH}, recording to $OutDir) ..."
    $script:ObsProc = Start-Process -FilePath $script:ObsExe `
        -WorkingDirectory $script:ObsBin -PassThru `
        -ArgumentList '--portable', '--multi', '--minimize-to-tray', '--startrecording', `
                      '--profile', 'CI', '--collection', 'CI', '--scene', 'CI'
    Start-Sleep -Seconds 6   # let OBS init its renderer + start the recording before the test drives
    Write-Host "[obs] pid=$($script:ObsProc.Id)"
    return $OutDir
}

# Stop recording and finalize: there is no clean CLI stop, but mkv tolerates a kill, so close OBS and
# let it flush. Returns the recorded .mkv path (or $null). Also copies OBS's own log next to it.
function Stop-ObsRecording {
    if (-not $script:ObsProc) { return $null }
    Write-Host "[obs] stopping ..."
    # Ask politely first (lets OBS finalize), then force.
    $script:ObsProc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 4
    if (-not $script:ObsProc.HasExited) { Stop-Process -Id $script:ObsProc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
    # OBS keeps its own log; copy the newest into the artifact dir for debugging.
    $log = Get-ChildItem (Join-Path $script:ObsCfg 'logs') -Filter *.txt -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($log -and $script:RecDir) { Copy-Item $log.FullName (Join-Path $script:RecDir 'obs.log') -Force -ErrorAction SilentlyContinue }
    $mkv = Get-ChildItem $script:RecDir -Filter *.mkv -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($mkv) { Write-Host "[obs] recording: $($mkv.Name) ($([int]($mkv.Length/1KB)) KB)" }
    else { Write-Host "[obs] NO .mkv produced (see obs.log)" }
    $script:ObsProc = $null
    return $(if ($mkv) { $mkv.FullName } else { $null })
}
