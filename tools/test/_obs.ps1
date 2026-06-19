#requires -Version 7.0
# OBS Studio recording helper for the two-instance multiplayer tests. It captures the [HOST] and
# [CLIENT] game windows side by side, with a 20px empty gap between them, into one video the CI run
# always keeps as an artifact. Portable OBS (a release .zip, no install); a fresh config is written
# per run. The windows are matched by their [HOST]/[CLIENT] caption (set by testdrv/windowtag).
#
# Recording does NOT begin at Start-ObsRecording: the two games spend ~30s booting to a black screen,
# so a background watcher holds off and starts OBS only once BOTH windows show content (the relay
# reports a dialog for each role). A timeout fallback records anyway, so a boot failure still yields a video.
#
#   Install-Obs
#   $rec = Start-ObsRecording -OutDir $dir   # arms the watcher; recording starts on first host content
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
$script:ObsWatcher = $null
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
        [string]$HostTitle   = 'Disciples II  [HOST]',   # NB: two spaces before the tag (windowtag.cpp)
        [string]$ClientTitle = 'Disciples II  [CLIENT]',
        [int]$PaneW = 1024, [int]$PaneH = 768, [int]$Gap = 20, [int]$Fps = 15,
        [string]$WinClass = 'MQ_UIManager', [int]$Method = 2,   # the game's window class; WGC (2) captures the GL surface, an empty class fails to match
        [string]$RelayBase = 'http://127.0.0.1:8077',   # the test relay; recording waits until the host reports a dialog (content on screen)
        [int]$ReadyTimeoutSec = 120                      # ... but records anyway after this, so a boot failure still yields a video
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
      "settings": { "window": "${title}:${WinClass}:Discipl2.exe", "method": $Method, "priority": 1, "cursor": false, "client_area": false }
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

    # Defer the real launch. From t=0 the two games spend ~30s booting to their first rendered dialog,
    # so recording immediately would just capture a black screen. A background watcher polls the relay
    # and launches OBS only once BOTH roles report a dialog (both windows show content, so neither pane
    # is black); if that never happens (a boot failure) the ReadyTimeoutSec fallback records anyway, so
    # the run still produces a diagnostic video. OBS with --startrecording captures until Stop-ObsRecording.
    Write-Host "[obs] watcher armed (canvas ${canvasW}x${PaneH} -> $OutDir; records when the host renders, fallback ${ReadyTimeoutSec}s)"
    $script:ObsWatcher = Start-Job -Name obswatch `
        -ArgumentList $script:ObsExe, $script:ObsBin, $RelayBase, $ReadyTimeoutSec `
        -ScriptBlock {
            param($exe, $bin, $relayBase, $timeoutSec)
            if (-not (Test-Path $exe)) { return }
            $deadline = (Get-Date).AddSeconds($timeoutSec)
            while ((Get-Date) -lt $deadline) {
                try {
                    $st = Invoke-RestMethod "$relayBase/api/state" -TimeoutSec 2
                    if ($st.roles.host.dialog -and $st.roles.join.dialog) { break }   # BOTH windows rendered -> neither pane is black
                } catch {}   # relay not up yet, or a transient miss; keep polling
                Start-Sleep -Milliseconds 1000
            }
            Start-Process -FilePath $exe -WorkingDirectory $bin -ArgumentList @(
                '--portable', '--multi', '--minimize-to-tray', '--startrecording',
                '--profile', 'CI', '--collection', 'CI', '--scene', 'CI') | Out-Null
        }
    return $OutDir
}

# Stop recording and finalize: there is no clean CLI stop, but mkv tolerates a kill, so close OBS and
# let it flush. Returns the recorded .mkv path (or $null). Also copies OBS's own log next to it.
function Stop-ObsRecording {
    # Tear the watcher down first so it cannot launch OBS after we have decided to stop; the brief
    # settle below then covers the race where it launched OBS just before being removed.
    if ($script:ObsWatcher) {
        Stop-Job   $script:ObsWatcher -ErrorAction SilentlyContinue
        Remove-Job $script:ObsWatcher -Force -ErrorAction SilentlyContinue
        $script:ObsWatcher = $null
    }
    Start-Sleep -Seconds 1
    # The watcher started OBS in its own runspace, so find it by process name (only our portable
    # instance runs on the runner). No OBS means the host never rendered before we stopped.
    $obs = @(Get-Process obs64 -ErrorAction SilentlyContinue)
    if (-not $obs) { Write-Host "[obs] OBS never started (host never rendered before stop) -> no video"; return $null }
    Write-Host "[obs] stopping ..."
    foreach ($p in $obs) { $p.CloseMainWindow() | Out-Null }   # ask politely so OBS finalizes the mkv
    Start-Sleep -Seconds 4
    Get-Process obs64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    # OBS keeps its own log; copy the newest into the artifact dir for debugging.
    $log = Get-ChildItem (Join-Path $script:ObsCfg 'logs') -Filter *.txt -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($log -and $script:RecDir) { Copy-Item $log.FullName (Join-Path $script:RecDir 'obs.log') -Force -ErrorAction SilentlyContinue }
    $mkv = Get-ChildItem $script:RecDir -Filter *.mkv -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($mkv) { Write-Host "[obs] recording: $($mkv.Name) ($([int]($mkv.Length/1KB)) KB)" }
    else { Write-Host "[obs] NO .mkv produced (see obs.log)" }
    return $(if ($mkv) { $mkv.FullName } else { $null })
}
