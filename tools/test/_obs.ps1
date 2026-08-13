#requires -Version 7.0
# OBS Studio recording helper for the two-instance multiplayer tests. It captures the [HOST] and
# [CLIENT] game windows side by side, with a 20px empty gap between them, into one best-effort video.
# CI keeps the video as a raw artifact when recording actually starts. Portable OBS (a release .zip,
# no install); a fresh config is written
# per run. The windows are matched by their [HOST]/[CLIENT] caption, refreshed by autonav's existing
# UI-frame callback (no separate window-tag hook or polling thread).
#
# Recording does NOT begin at Start-ObsRecording: the two games spend ~30s booting to a black screen,
# so a background watcher holds off and starts OBS only once BOTH windows show content (the relay
# reports a dialog for each role). A timeout fallback starts recording only if the test is still running;
# an early failure may end and remove both clients before OBS starts, in which case logs/screenshots remain.
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
$script:ObsPidFile = $null
$script:ObsRelayHttpHost = if ([string]::IsNullOrWhiteSpace($env:D2_RELAY_HTTP_HOST)) { '127.0.0.1' } else { $env:D2_RELAY_HTTP_HOST }
$script:ObsRelayHttpPort = if ([string]::IsNullOrWhiteSpace($env:D2_RELAY_HTTP_PORT)) { '8077' } else { $env:D2_RELAY_HTTP_PORT }
$script:ObsRelayBase = "http://$($script:ObsRelayHttpHost):$($script:ObsRelayHttpPort)"

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
        [string]$HostTitle   = 'Disciples II  [HOST]',   # NB: two spaces before the tag (autonav.cpp)
        [string]$ClientTitle = 'Disciples II  [CLIENT]',
        [int]$PaneW = 1024, [int]$PaneH = 768, [int]$Gap = 20, [int]$Fps = 15,
        [string]$WinClass = 'MQ_UIManager', [int]$Method = 2,   # the game's window class; WGC (2) captures the GL surface, an empty class fails to match
        [string]$RelayBase = $script:ObsRelayBase,      # the test relay; recording waits until both roles report a dialog (content on screen)
        [int]$ReadyTimeoutSec = 120                      # ... then records if the run is still alive
    )
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $script:RecDir = $OutDir
    $script:ObsPidFile = Join-Path $OutDir 'obs-owned.json'
    # Invalidate an earlier marker. Stop-ObsRecording trusts only the PID + executable recorded by
    # this watcher and never enumerates or terminates unrelated OBS instances.
    Set-Content -LiteralPath $script:ObsPidFile -Value '' -Encoding UTF8
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
    # is black); if readiness is slow, the ReadyTimeoutSec fallback records while the run remains alive.
    # A failure that kills both clients before this point cannot produce a video. OBS with
    # --startrecording captures until Stop-ObsRecording.
    Write-Host "[obs] watcher armed (canvas ${canvasW}x${PaneH} -> $OutDir; records when both roles render, fallback ${ReadyTimeoutSec}s)"
    $script:ObsWatcher = Start-Job -Name obswatch `
        -ArgumentList $script:ObsExe, $script:ObsBin, $RelayBase, $ReadyTimeoutSec, $script:ObsPidFile `
        -ScriptBlock {
            param($exe, $bin, $relayBase, $timeoutSec, $pidFile)
            if (-not (Test-Path $exe)) { return }
            $deadline = (Get-Date).AddSeconds($timeoutSec)
            while ((Get-Date) -lt $deadline) {
                try {
                    $st = Invoke-RestMethod "$relayBase/api/state" -TimeoutSec 2
                    if ($st.roles.host.dialog -and $st.roles.join.dialog) { break }   # BOTH windows rendered -> neither pane is black
                } catch {}   # relay not up yet, or a transient miss; keep polling
                Start-Sleep -Milliseconds 1000
            }
            $owned = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru -ArgumentList @(
                '--portable', '--multi', '--minimize-to-tray', '--startrecording',
                '--profile', 'CI', '--collection', 'CI', '--scene', 'CI')
            [pscustomobject]@{
                pid = $owned.Id
                executable = [IO.Path]::GetFullPath($exe)
            } | ConvertTo-Json -Compress | Set-Content -LiteralPath $pidFile -Encoding UTF8
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
    # Stop only the exact portable OBS process launched by this watcher. Verify both PID and full
    # executable path to protect against PID reuse; if ownership cannot be proved, leave it alone.
    $obs = $null
    $meta = $null
    if ($script:ObsPidFile -and (Test-Path -LiteralPath $script:ObsPidFile)) {
        try { $meta = Get-Content -LiteralPath $script:ObsPidFile -Raw | ConvertFrom-Json -ErrorAction Stop } catch {}
    }
    if ($meta -and $meta.pid -and $meta.executable) {
        $candidate = Get-Process -Id ([int]$meta.pid) -ErrorAction SilentlyContinue
        if ($candidate) {
            try {
                $actual = [IO.Path]::GetFullPath($candidate.Path)
                $expected = [IO.Path]::GetFullPath([string]$meta.executable)
                if ([string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) {
                    $obs = $candidate
                } else {
                    Write-Warning "[obs] PID $($candidate.Id) belongs to '$actual', expected '$expected'; leaving it untouched"
                }
            } catch { Write-Warning "[obs] could not prove ownership of PID $($candidate.Id); leaving it untouched" }
        }
    }
    if ($obs) {
        Write-Host "[obs] stopping owned pid=$($obs.Id) ..."
        $obs.CloseMainWindow() | Out-Null
        Start-Sleep -Seconds 4
        $stillRunning = Get-Process -Id $obs.Id -ErrorAction SilentlyContinue
        if ($stillRunning) {
            try {
                if ([string]::Equals([IO.Path]::GetFullPath($stillRunning.Path),
                        [IO.Path]::GetFullPath([string]$meta.executable),
                        [StringComparison]::OrdinalIgnoreCase)) {
                    Stop-Process -Id $stillRunning.Id -Force -ErrorAction SilentlyContinue
                }
            } catch { Write-Warning "[obs] ownership changed while stopping; leaving PID $($stillRunning.Id) untouched" }
        }
        Start-Sleep -Seconds 2
    } else {
        Write-Host "[obs] no owned OBS process recorded (never started, exited, or ownership unproved)"
    }
    # OBS keeps its own log; copy the newest into the artifact dir for debugging.
    $log = Get-ChildItem (Join-Path $script:ObsCfg 'logs') -Filter *.txt -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($log -and $script:RecDir) { Copy-Item $log.FullName (Join-Path $script:RecDir 'obs.log') -Force -ErrorAction SilentlyContinue }
    $mkv = Get-ChildItem $script:RecDir -Filter *.mkv -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($mkv) { Write-Host "[obs] recording: $($mkv.Name) ($([int]($mkv.Length/1KB)) KB)" }
    else { Write-Host "[obs] NO .mkv produced (see obs.log)" }
    return $(if ($mkv) { $mkv.FullName } else { $null })
}
