<#
.SYNOPSIS
  Build the monolith C4dll-R.dll = pristine upstream cnc-ddraw + our patch + CB63 export
  forwarder + the DirectDraw embed, in ONE assembly (DisciplesGL-style). No separate ddraw.dll.

.DESCRIPTION
  Build only:     .\build.ps1
  Build + deploy: .\build.ps1 -Deploy      (backs up the game's baseline once, then swaps in
                                             our C4dll-R + disables the standalone ddraw.dll)
  Restore vanilla:.\build.ps1 -Restore     (puts the baseline C4dll-R/ddraw.dll back)

  The build: copy upstream/cnc-ddraw (git submodule, pinned commit) -> apply
  patches/cnc-ddraw-mss32.patch + patches/cnc-ddraw-render-null.patch -> generate
  C4dll-R.def (483 CB63 forwards + DDReloadConfig/DDTakeScreenshot) -> retarget the vcxproj
  (TargetName + .def) -> msbuild Release. cnc-ddraw's DllMain hooks the game's IAT
  DirectDrawCreate(Ex) so the embedded renderer is used; the system ddraw.dll satisfies the
  game's static import. Result: build/cnc-ddraw/bin/Release/C4dll-R.dll.
#>
param(
    [string]$Toolset = "v143",
    [string]$SdkVersion = "",   # "" = let the project pick the latest installed SDK (CI-friendly)
    [switch]$Deploy,
    [switch]$Restore,
    [string]$Game = "C:\GOG Games\slasher_mns_2_4"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$upstream = Join-Path $root "upstream\cnc-ddraw"
$build = Join-Path $root "build\cnc-ddraw"
$patch = Join-Path $root "patches\cnc-ddraw-mss32.patch"
$patchNull = Join-Path $root "patches\cnc-ddraw-render-null.patch"
$cb63def = Join-Path $root "forwarder\C4dll-R.cb63.def"
$out = Join-Path $build "bin\Release\C4dll-R.dll"
$pluginProj = Join-Path $root "plugins\timer\timer.vcxproj"
$pluginOut = Join-Path $root "plugins\timer\bin\Release\timer.c4p"

# Locate MSBuild robustly (works on a dev box AND on GitHub windows-latest, where VS is Enterprise).
function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($p -and (Test-Path $p)) { return $p }
    }
    $fallback = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    if (Test-Path $fallback) { return $fallback }
    throw "MSBuild.exe not found (install VS2022 with the MSBuild + Desktop C++ workload)"
}

# --- baseline backup names in the game folder (created once on first -Deploy) ---
$bC4 = Join-Path $Game "C4dll-R.dll.monolith-baseline"   # the CB63-copy baseline
$dOff = Join-Path $Game "ddraw.dll.monolith-off"          # the standalone ddraw.dll, parked

# Stop ONLY the game instance launched from THIS folder, and NEVER a host/client window (the user
# runs parallel MP host/client instances, sometimes from other folders - blanket Stop-Process by
# name would kill those). dplaysvr (shared DirectPlay helper) is left alone.
function Stop-TargetGame {
    $dir = $Game.TrimEnd('\')
    Get-Process Discipl2 -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ([System.IO.Path]::GetDirectoryName($_.Path) -eq $dir) -and
        ($_.MainWindowTitle -notmatch 'host|client')
    } | Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($Restore) {
    Stop-TargetGame
    Start-Sleep -Milliseconds 600
    if (Test-Path $bC4) { Copy-Item $bC4 (Join-Path $Game "C4dll-R.dll") -Force; [System.IO.File]::Delete($bC4) }
    if (Test-Path $dOff) { Move-Item $dOff (Join-Path $Game "ddraw.dll") -Force }
    $depC4p = Join-Path $Game "Mods\timer.c4p"
    if (Test-Path $depC4p) { [System.IO.File]::Delete($depC4p) }  # remove our plugin -> legacy timer.mod takes over
    Write-Host "Restored vanilla baseline (C4dll-R = CB63 copy, standalone ddraw.dll back, timer.c4p removed)." -ForegroundColor Cyan
    return
}

Write-Host "[1/6] clean build dir" -ForegroundColor Cyan
if (Test-Path $build) { [System.IO.Directory]::Delete($build, $true) }
New-Item -ItemType Directory -Force -Path (Split-Path $build) | Out-Null

Write-Host "[2/6] copy pristine upstream cnc-ddraw (submodule)" -ForegroundColor Cyan
# cnc-ddraw is a git submodule pinned at the exact upstream commit (c4ddraw/upstream/cnc-ddraw).
# Init it if a fresh/partial checkout left it empty (CI's actions/checkout submodules:recursive
# already populates it).
if (-not (Test-Path (Join-Path $upstream "src\dd.c"))) {
    $toplevel = (& git -C $root rev-parse --show-toplevel).Trim()
    & git -C $toplevel submodule update --init -- "c4ddraw/upstream/cnc-ddraw"
    if ($LASTEXITCODE -ne 0) { throw "submodule init failed (exit $LASTEXITCODE)" }
}
Copy-Item $upstream $build -Recurse -Force
# the submodule working tree carries a .git gitlink file; drop it so the throwaway git-apply repo below is clean
Remove-Item (Join-Path $build ".git") -Force -Recurse -ErrorAction SilentlyContinue

Write-Host "[3/6] apply our patches (mss32 embed/exports + render_null headless)" -ForegroundColor Cyan
# Apply inside a throwaway repo so git apply resolves paths cleanly (it "Skipped patch" when
# run outside a working tree). autocrlf=false keeps the patch context matching the upstream EOLs.
# Two disjoint patches: cnc-ddraw-mss32 (dllmain/winapi_hooks/exports.def) + render-null
# (dd.c renderer branch + vcxproj + inc/render_null.h + src/render_null.c).
Push-Location $build
try {
    & git init -q
    # --ignore-whitespace: tolerate trailing-whitespace differences in context lines (the patches are
    # hand-edited; cnc-ddraw sources have some trailing spaces we don't want to track exactly).
    & git -c core.autocrlf=false apply --ignore-whitespace "$patch"
    if ($LASTEXITCODE -ne 0) { throw "git apply (mss32) failed (exit $LASTEXITCODE)" }
    & git -c core.autocrlf=false apply --ignore-whitespace "$patchNull"
    if ($LASTEXITCODE -ne 0) { throw "git apply (render-null) failed (exit $LASTEXITCODE)" }
}
finally { Pop-Location }
if (-not (Select-String -Path (Join-Path $build "src\dllmain.c") -Pattern "DDReloadConfig" -Quiet)) {
    throw "patch did not apply: DDReloadConfig missing from src/dllmain.c"
}
if (-not (Test-Path (Join-Path $build "src\render_null.c"))) {
    throw "render-null patch did not apply: src/render_null.c missing"
}

# Add our self-contained feature sources (features/*.cpp + headers). They are NOT part of upstream
# cnc-ddraw and do NOT depend on mss32; the patch only adds the *_install() calls in DllMain. We
# copy them into src/ and inject the .cpp into the project below.
Copy-Item (Join-Path $root "features\featuremenu.cpp") (Join-Path $build "src\featuremenu.cpp") -Force
Copy-Item (Join-Path $root "features\pluginhost.cpp") (Join-Path $build "src\pluginhost.cpp") -Force
Copy-Item (Join-Path $root "features\cyrillic.cpp") (Join-Path $build "src\cyrillic.cpp") -Force
Copy-Item (Join-Path $root "features\timerhost.cpp") (Join-Path $build "src\timerhost.cpp") -Force
Copy-Item (Join-Path $root "features\headless.cpp") (Join-Path $build "src\headless.cpp") -Force
Copy-Item (Join-Path $root "features\c4plugin.h") (Join-Path $build "src\c4plugin.h") -Force
foreach ($sym in @("featuremenu_install", "pluginhost_install", "cyrillic_install", "headless_install")) {
    if (-not (Select-String -Path (Join-Path $build "src\dllmain.c") -Pattern $sym -Quiet)) {
        throw "patch did not apply: $sym() call missing from src/dllmain.c"
    }
}

Write-Host "[4/6] generate C4dll-R.def (483 CB63 forwards + 2 exports)" -ForegroundColor Cyan
$def = Join-Path $build "C4dll-R.def"
$lines = @("; C4dll-R.dll - CB63 forwarder + embedded cnc-ddraw + mss32-menu exports")
$lines += (Get-Content $cb63def | Where-Object { $_ -match '^(EXPORTS|\s+\S+=CB63\.)' })
$lines += "    DDReloadConfig @484"
$lines += "    DDTakeScreenshot @485"
Set-Content -Path $def -Value $lines -Encoding ASCII

Write-Host "[5/6] retarget vcxproj (output C4dll-R, use C4dll-R.def, add featuremenu.cpp)" -ForegroundColor Cyan
$vcx = Join-Path $build "cnc-ddraw.vcxproj"
(Get-Content $vcx -Raw) `
    -replace '<TargetName>ddraw</TargetName>', '<TargetName>C4dll-R</TargetName>' `
    -replace '<ModuleDefinitionFile>exports\.def</ModuleDefinitionFile>', '<ModuleDefinitionFile>C4dll-R.def</ModuleDefinitionFile>' `
    -replace '<ClCompile Include="src\\dllmain\.c" />', "<ClCompile Include=`"src\featuremenu.cpp`" />`r`n    <ClCompile Include=`"src\pluginhost.cpp`" />`r`n    <ClCompile Include=`"src\cyrillic.cpp`" />`r`n    <ClCompile Include=`"src\timerhost.cpp`" />`r`n    <ClCompile Include=`"src\headless.cpp`" />`r`n    <ClCompile Include=`"src\dllmain.c`" />" |
Set-Content -Path $vcx -Encoding UTF8
foreach ($src in @('featuremenu\.cpp', 'pluginhost\.cpp', 'cyrillic\.cpp', 'headless\.cpp')) {
    if (-not (Select-String -Path $vcx -Pattern $src -Quiet)) {
        throw "vcxproj retarget failed: $src not added to the project"
    }
}

Write-Host "[6/6] msbuild Release ($Toolset)" -ForegroundColor Cyan
$env:_CL_ = ""
$ms = Find-MSBuild
$mbArgs = @($vcx, '/t:Rebuild', '/p:Configuration=Release', '/p:Platform=Win32',
    "/p:PlatformToolset=$Toolset", "/p:SolutionDir=$build\", '/m', '/nologo', '/v:minimal')
if ($SdkVersion) { $mbArgs += "/p:WindowsTargetPlatformVersion=$SdkVersion" }
& $ms @mbArgs
if ($LASTEXITCODE -ne 0) { throw "msbuild failed" }
Write-Host ("BUILT -> {0} ({1:n0} bytes)" -f $out, (Get-Item $out).Length) -ForegroundColor Green

# Build the native timer plugin (timer.c4p). Self-contained Win32 DLL (GDI+, static CRT, embedded
# clock font) reconstructed from the legacy timer.mod; the host (pluginhost) drives its turn reset.
Write-Host "[7/7] build timer.c4p plugin" -ForegroundColor Cyan
$pbArgs = @($pluginProj, '/t:Rebuild', '/p:Configuration=Release', '/p:Platform=Win32',
    "/p:PlatformToolset=$Toolset", '/nologo', '/v:minimal')
if ($SdkVersion) { $pbArgs += "/p:WindowsTargetPlatformVersion=$SdkVersion" }
& $ms @pbArgs
if ($LASTEXITCODE -ne 0) { throw "timer.c4p build failed" }
Write-Host ("BUILT -> {0} ({1:n0} bytes)" -f $pluginOut, (Get-Item $pluginOut).Length) -ForegroundColor Green

if ($Deploy) {
    Stop-TargetGame
    Start-Sleep -Milliseconds 600
    if (-not (Test-Path $bC4)) { Copy-Item (Join-Path $Game "C4dll-R.dll") $bC4 -Force }  # baseline once
    Copy-Item $out (Join-Path $Game "C4dll-R.dll") -Force
    if ((Test-Path (Join-Path $Game "ddraw.dll")) -and -not (Test-Path $dOff)) {
        Move-Item (Join-Path $Game "ddraw.dll") $dOff -Force                                # park standalone ddraw
    }
    $modsDir = Join-Path $Game "Mods"
    if (-not (Test-Path $modsDir)) { New-Item -ItemType Directory -Force -Path $modsDir | Out-Null }
    Copy-Item $pluginOut (Join-Path $modsDir "timer.c4p") -Force                            # native timer plugin
    Write-Host "Deployed monolith C4dll-R.dll + Mods\timer.c4p; standalone ddraw.dll parked." -ForegroundColor Green
    Write-Host "(legacy Mods\timer.mod, if present, is auto-superseded by timer.c4p. .\build.ps1 -Restore to revert)" -ForegroundColor Green
}
