#requires -Version 7.0
# OBS Studio recording helper for the game tests. By default it captures the [HOST] and [CLIENT]
# windows side by side; -HostOnly records the single [HOST] window used by the template matrix.
# CI keeps the video as a raw artifact when recording actually starts. Portable OBS (a release .zip,
# no install); a fresh config is written
# per run. The windows are matched by their [HOST]/[CLIENT] caption, refreshed by autonav's existing
# UI-frame callback (no separate window-tag hook or polling thread).
#
# By default a background watcher holds off until every requested role reports a rendered dialog.
# The template matrix uses HostOnly mode plus a recording-ready marker, so it waits until OBS creates
# this attempt's new MKV before opening the generator and avoids a black boot prefix. The finalized
# non-empty MKV remains the required artifact. Dual-client CI evidence passes
# -StartImmediately so an early boot failure still leaves MKV.
#
#   Install-Obs
#   $rec = Start-ObsRecording -OutDir $dir   # deferred mode; marker proves OBS began recording
#   $rec = Start-ObsRecording -OutDir $dir -StartImmediately  # required CI evidence
#   ... run the test ...
#   Stop-ObsRecording        # -> finalizes; the .mkv + obs log are in $dir
#
# OBS on a headless CI runner is finicky (its own D3D renderer, capturing a Mesa-GL window), so every
# function logs to the host and the caller uploads $dir (video + obs log) for diagnosis.

$script:ObsVersion = '32.1.2'
$script:ObsSha256  = '8d97e4563bd8d22d03e63042aa7dccede1d555c9bd35ce8a9e5019b0d0201bf6'
$script:ObsRoot    = Join-Path $env:TEMP 'obs-portable'
$script:ObsBin     = Join-Path $script:ObsRoot 'bin\64bit'
$script:ObsExe     = Join-Path $script:ObsBin 'obs64.exe'
$script:ObsCfg     = Join-Path $script:ObsRoot 'config\obs-studio'
$script:ObsWatcher = $null
$script:RecDir     = $null
$script:ObsPidFile = $null
$script:ObsReadyFile = $null
$script:ObsRelayHttpHost = if ([string]::IsNullOrWhiteSpace($env:D2_RELAY_HTTP_HOST)) { '127.0.0.1' } else { $env:D2_RELAY_HTTP_HOST }
$script:ObsRelayHttpPort = if ([string]::IsNullOrWhiteSpace($env:D2_RELAY_HTTP_PORT)) { '8077' } else { $env:D2_RELAY_HTTP_PORT }
$script:ObsRelayBase = "http://$($script:ObsRelayHttpHost):$($script:ObsRelayHttpPort)"

# Download + extract the portable OBS release once; portable_mode.txt keeps its config local.
function Install-Obs {
    param(
        [string]$Version = $script:ObsVersion,
        [string]$ExpectedSha256 = $script:ObsSha256
    )
    if (Test-Path $script:ObsExe) { Write-Host "[obs] already present"; return }
    $url = "https://github.com/obsproject/obs-studio/releases/download/$Version/OBS-Studio-$Version-Windows-x64.zip"
    $zip = Join-Path $env:TEMP 'obs.zip'
    Write-Host "[obs] downloading $Version ..."
    Invoke-WebRequest $url -OutFile $zip -UseBasicParsing
    $actualSha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $actualSha256 -ne $ExpectedSha256.ToLowerInvariant()) {
        throw "OBS archive SHA-256 mismatch: expected $ExpectedSha256, got $actualSha256"
    }
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
        [switch]$HostOnly,
        [switch]$StartImmediately,
        [string]$HostTitle   = 'Disciples II  [HOST]',   # NB: two spaces before the tag (autonav.cpp)
        [string]$ClientTitle = 'Disciples II  [CLIENT]',
        [int]$PaneW = 1024, [int]$PaneH = 768, [int]$Gap = 20, [int]$Fps = 15,
        [int]$BitrateKbps = 4000,
        [string]$WinClass = 'MQ_UIManager', [int]$Method = 2,   # the game's window class; WGC (2) captures the GL surface, an empty class fails to match
        [string]$RelayBase = $script:ObsRelayBase,      # the test relay; recording waits until both roles report a dialog (content on screen)
        [int]$ReadyTimeoutSec = 120,                     # ... then records if the run is still alive
        [int]$RecordingReadyTimeoutSec = 90              # bound OBS initialization until this attempt's MKV appears
    )
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $script:RecDir = $OutDir
    $script:ObsPidFile = Join-Path $OutDir 'obs-owned.json'
    $script:ObsReadyFile = Join-Path $OutDir 'obs-recording-ready.json'
    # Invalidate an earlier marker. Stop-ObsRecording trusts only the PID + executable recorded by
    # this watcher and never enumerates or terminates unrelated OBS instances.
    Remove-Item -LiteralPath $script:ObsPidFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath "$($script:ObsPidFile).tmp" -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $script:ObsReadyFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath "$($script:ObsReadyFile).tmp" -Force -ErrorAction SilentlyContinue
    $canvasW = if ($HostOnly) { $PaneW } else { $PaneW * 2 + $Gap }
    $readyRoles = if ($HostOnly) { 'host' } else { 'host,join' }

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
VBitrate=$BitrateKbps
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
    $captureSources = @((srcJson 'host' $HostTitle))
    $sceneItems = @((itemJson 'host' 0))
    if (-not $HostOnly) {
        $captureSources += (srcJson 'client' $ClientTitle)
        $sceneItems += (itemJson 'client' ($PaneW + $Gap))
    }
    $sceneSource = @"
    {
      "name": "CI",
      "id": "scene",
      "versioned_id": "scene",
      "settings": {
        "items": [
$($sceneItems -join ",`n")
        ]
      }
    }
"@
    $captureSources += $sceneSource
    @"
{
  "current_scene": "CI",
  "current_program_scene": "CI",
  "scene_order": [ { "name": "CI" } ],
  "name": "CI",
  "sources": [
$($captureSources -join ",`n")
  ]
}
"@ | Set-Content (Join-Path $scn 'CI.json') -Encoding UTF8

    # Manual/default mode waits for rendered dialogs to avoid a long black lead-in. Required CI mode
    # skips that wait and records immediately, trading a black boot prefix for evidence of early
    # failures. OBS with --startrecording captures until Stop-ObsRecording.
    $startMode = if ($StartImmediately) { 'immediate' } else { "roles=$readyRoles; fallback=${ReadyTimeoutSec}s" }
    Write-Host "[obs] watcher armed (canvas ${canvasW}x${PaneH}; $startMode -> $OutDir)"
    $script:ObsWatcher = Start-Job -Name obswatch `
        -ArgumentList $script:ObsExe, $script:ObsBin, $RelayBase, $ReadyTimeoutSec, $script:ObsPidFile, $script:ObsReadyFile, $OutDir, $readyRoles, ([bool]$StartImmediately), $RecordingReadyTimeoutSec `
        -ScriptBlock {
            param($exe, $bin, $relayBase, $timeoutSec, $pidFile, $readyFile, $outDir, $readyRolesCsv, $startImmediately, $recordingTimeoutSec)
            if (-not (Test-Path $exe)) { return }
            $roles = @($readyRolesCsv -split ',')
            $deadline = (Get-Date).AddSeconds($timeoutSec)
            while (-not $startImmediately -and (Get-Date) -lt $deadline) {
                try {
                    $st = Invoke-RestMethod "$relayBase/api/state" -TimeoutSec 2
                    $ready = $true
                    foreach ($role in $roles) {
                        $roleState = $st.roles.PSObject.Properties[$role].Value
                        if (-not $roleState -or -not $roleState.dialog) { $ready = $false; break }
                    }
                    if ($ready) { break }
                } catch {}   # relay not up yet, or a transient miss; keep polling
                Start-Sleep -Milliseconds 1000
            }
            $existingRecordings = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            Get-ChildItem -LiteralPath $outDir -Filter '*.mkv' -File -ErrorAction SilentlyContinue |
                ForEach-Object { [void]$existingRecordings.Add([IO.Path]::GetFullPath($_.FullName)) }
            $owned = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru -ArgumentList @(
                '--portable', '--multi', '--minimize-to-tray', '--startrecording',
                '--profile', 'CI', '--collection', 'CI', '--scene', 'CI')
            $metadata = [pscustomobject]@{
                pid = $owned.Id
                executable = [IO.Path]::GetFullPath($exe)
            }
            # Publish ownership immediately so Stop-ObsRecording can always clean up a stalled OBS.
            $ownedTemp = "$pidFile.tmp"
            $metadata | ConvertTo-Json -Compress | Set-Content -LiteralPath $ownedTemp -Encoding UTF8
            Move-Item -LiteralPath $ownedTemp -Destination $pidFile -Force

            # Process creation is not recording readiness: hosted OBS can spend tens of seconds in
            # graphics/encoder initialization. Publish readiness only when this launch creates a new
            # MKV in the exact attempt directory; Stop-ObsRecording later requires it to be non-empty.
            $recordingDeadline = (Get-Date).AddSeconds($recordingTimeoutSec)
            while ((Get-Date) -lt $recordingDeadline) {
                try {
                    $liveOwned = Get-Process -Id $owned.Id -ErrorAction Stop
                    $liveExecutable = [IO.Path]::GetFullPath($liveOwned.Path)
                } catch { return }
                if (-not [string]::Equals($liveExecutable, $metadata.executable, [StringComparison]::OrdinalIgnoreCase)) { return }
                $recording = Get-ChildItem -LiteralPath $outDir -Filter '*.mkv' -File -ErrorAction SilentlyContinue |
                    Where-Object { -not $existingRecordings.Contains([IO.Path]::GetFullPath($_.FullName)) } |
                    Sort-Object LastWriteTime -Descending |
                    Select-Object -First 1
                if ($recording) {
                    $recordingPath = [IO.Path]::GetFullPath($recording.FullName)
                    $recordingDir = [IO.Path]::GetFullPath((Split-Path -Parent $recordingPath))
                    $expectedDir = [IO.Path]::GetFullPath($outDir)
                    if ([IO.Path]::GetExtension($recordingPath) -ieq '.mkv' -and
                        [string]::Equals($recordingDir, $expectedDir, [StringComparison]::OrdinalIgnoreCase)) {
                        $readyMetadata = [pscustomobject]@{
                            pid = $metadata.pid
                            executable = $metadata.executable
                            recording = $recordingPath
                            proof = 'obs-output-file-created-v1'
                        }
                        $readyTemp = "$readyFile.tmp"
                        $readyMetadata | ConvertTo-Json -Compress | Set-Content -LiteralPath $readyTemp -Encoding UTF8
                        Move-Item -LiteralPath $readyTemp -Destination $readyFile -Force
                        return
                    }
                }
                Start-Sleep -Milliseconds 250
            }
        }
    if ($StartImmediately) {
        # Immediate mode intentionally returns once the owned OBS process is live, then the caller
        # launches the game window that Window Capture needs before it can emit frames. Waiting for
        # a recording-ready marker here would deadlock those two steps. Template CI uses deferred mode and
        # independently waits for obs-recording-ready.json after its host window already exists.
        $launchDeadline = (Get-Date).AddSeconds(20)
        $launched = $false
        while ((Get-Date) -lt $launchDeadline) {
            try {
                $owned = Get-Content -LiteralPath $script:ObsPidFile -Raw | ConvertFrom-Json -ErrorAction Stop
                $ownedProcess = Get-Process -Id ([int]$owned.pid) -ErrorAction Stop
                $actual = [IO.Path]::GetFullPath($ownedProcess.Path)
                $expected = [IO.Path]::GetFullPath([string]$owned.executable)
                if ([string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) {
                    $launched = $true
                    break
                }
            } catch {}
            Start-Sleep -Milliseconds 250
        }
        if (-not $launched) {
            try { Stop-ObsRecording | Out-Null } catch {}
            throw '[obs] required immediate OBS process did not launch within 20s'
        }
        Start-Sleep -Seconds 2
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
    # When readiness was published, its exact output path is authoritative. Falling back to a
    # different MKV would mask a broken proof contract. With no marker (for example an immediate
    # early failure), retain the newest finalized recording opportunistically.
    $readyMarkerPresent = $script:ObsReadyFile -and (Test-Path -LiteralPath $script:ObsReadyFile)
    $ready = $null
    if ($readyMarkerPresent) {
        try { $ready = Get-Content -LiteralPath $script:ObsReadyFile -Raw | ConvertFrom-Json -ErrorAction Stop } catch {}
    }
    $log = Get-ChildItem (Join-Path $script:ObsCfg 'logs') -Filter *.txt -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($log -and $script:RecDir) { Copy-Item $log.FullName (Join-Path $script:RecDir 'obs.log') -Force -ErrorAction SilentlyContinue }
    $mkv = $null
    if ($ready -and [string]$ready.proof -eq 'obs-output-file-created-v1') {
        try {
            $readyMkv = Get-Item -LiteralPath ([string]$ready.recording) -ErrorAction Stop
            $readyDir = [IO.Path]::GetFullPath($readyMkv.DirectoryName)
            $expectedDir = [IO.Path]::GetFullPath($script:RecDir)
            if ($readyMkv.Length -gt 0 -and
                [string]::Equals($readyDir, $expectedDir, [StringComparison]::OrdinalIgnoreCase)) {
                $mkv = $readyMkv
            }
        } catch {}
    }
    if (-not $mkv -and -not $readyMarkerPresent) {
        $mkv = Get-ChildItem $script:RecDir -Filter *.mkv -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime | Select-Object -Last 1
    }
    if ($mkv) { Write-Host "[obs] recording: $($mkv.Name) ($([int]($mkv.Length/1KB)) KB)" }
    else { Write-Host "[obs] NO .mkv produced (see obs.log)" }
    return $(if ($mkv) { $mkv.FullName } else { $null })
}
