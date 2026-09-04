$ErrorActionPreference = 'Stop'
$testWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testOutput = Join-Path $testWorkspace '.diagnostics\c4trace-tests'
$testRun = Join-Path $testOutput ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$testTemp = New-Item -ItemType Directory -Path (Join-Path $testRun 'tmp')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$testMSBuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $testMSBuild) { throw 'MSBuild unavailable' }
# Some hosts inherit both Path and PATH; .NET Framework MSBuild rejects that.
# Normalize only the child's environment; leave the caller's environment alone.
$testBuildEnvironment = @{}
foreach ($testKey in [Environment]::GetEnvironmentVariables().Keys) {
    $testBuildEnvironment[[string]$testKey] = [Environment]::GetEnvironmentVariable([string]$testKey)
}
$testBuildStart = [Diagnostics.ProcessStartInfo]::new()
$testBuildStart.FileName = $testMSBuild
$testBuildStart.UseShellExecute = $false
$testBuildStart.CreateNoWindow = $true
$testBuildStart.Arguments = '"' + (Join-Path $PSScriptRoot 'c4trace_synthetic.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal'
$testBuildStart.EnvironmentVariables.Clear()
foreach ($testKey in $testBuildEnvironment.Keys) { $testBuildStart.EnvironmentVariables[$testKey] = $testBuildEnvironment[$testKey] }
$testBuildStart.EnvironmentVariables['TEMP'] = $testTemp.FullName
$testBuildStart.EnvironmentVariables['TMP'] = $testTemp.FullName
$testBuildProcess = [Diagnostics.Process]::Start($testBuildStart)
$testBuildProcess.WaitForExit()
if ($testBuildProcess.ExitCode -ne 0) { throw 'Synthetic test build failed' }
foreach ($testMode in @('config', 'off', 'invalid', 'lowdisk', 'on', 'ini', 'cap', 'concurrent', 'iofailure')) {
    $testCase = New-Item -ItemType Directory -Path (Join-Path $testRun $testMode)
    $testExe = Join-Path $testCase.FullName 'c4trace_synthetic.exe'
    Copy-Item -LiteralPath (Join-Path $testOutput 'bin\c4trace_synthetic.exe') -Destination $testExe
    & $testExe $testMode
    if ($LASTEXITCODE -ne 0) { throw ('Synthetic test failed: ' + $testMode) }
    if ($testMode -eq 'on') {
        $testFirstFile = Get-ChildItem -LiteralPath $testCase.FullName -Filter 'C4trace-*.csv' -File | Select-Object -First 1
        $testFirstHash = (Get-FileHash -LiteralPath $testFirstFile.FullName -Algorithm SHA256).Hash
        & $testExe on
        if ($LASTEXITCODE -ne 0) { throw 'Second run failed' }
        if ((Get-FileHash -LiteralPath $testFirstFile.FullName -Algorithm SHA256).Hash -ne $testFirstHash) {
            throw 'Second run modified the first trace'
        }
        Write-Output 'PASS: repeated run preserves earlier trace byte-for-byte'
    }
    if ($testMode -eq 'concurrent') {
        $testTrace = Get-ChildItem -LiteralPath $testCase.FullName -Filter 'C4trace-*.csv' -File | Select-Object -First 1
        $testSequences = [Collections.Generic.HashSet[UInt64]]::new()
        foreach ($testLine in [IO.File]::ReadLines($testTrace.FullName)) {
            if ($testLine.StartsWith('#') -or $testLine.StartsWith('seq,')) { continue }
            $testFields = $testLine.Split(',')
            if ($testFields.Length -ne 10 -or -not $testSequences.Add([UInt64]$testFields[0]) -or
                $testFields[4] -ne '101' -or $testFields[5] -ne '0x12345678' -or
                $testFields[8] -ne '0xAABBCCDD' -or $testFields[9] -ne '0xFFFFFFFF' -or
                [Convert]::ToUInt64($testFields[7].Substring(2), 16) -ne [UInt64]$testFields[3]) {
                throw 'Malformed, torn, or duplicate concurrent record'
            }
        }
        if ($testSequences.Count -le 8192) { throw 'Test did not exercise repeated buffer swaps' }
        Write-Output ('PASS: ' + $testSequences.Count + ' intact unique records across repeated buffer swaps')
    }
}
Write-Output ('PASS: all synthetic recorder tests; artifacts: ' + $testRun)
