#requires -Version 7.0
<#
Offline socket-level contract for the removable Russobit auto-battle driver.

The removable driver sends exactly one request to a fake relay, which checks the
complete bound identity and returns one exact 030D-shaped proof. This is a direct
socket/wire contract for the throwaway test driver; it launches no licensed game
and does not depend on shared gameplay helpers.
#>

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$acceptedClient = $null
$httpClient = $null
$request = $null
$response = $null
try {
    $listener.Start()
    $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    $acceptTask = $listener.AcceptTcpClientAsync()
    $script:RelayBase = "http://127.0.0.1:$port"

    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.UseProxy = $false
    $handler.AllowAutoRedirect = $false
    $httpClient = [Net.Http.HttpClient]::new($handler, $true)
    $httpClient.Timeout = [Threading.Timeout]::InfiniteTimeSpan
    $uri = "$script:RelayBase/api/ui/enable-auto-battle?role=host&" +
        'dlg=DLG_BATTLE_A&tog=TOG_AUTOBATTLE&appearance=41&instance=73'
    $request = [Net.Http.HttpRequestMessage]::new(
        [Net.Http.HttpMethod]::Post, $uri)
    # This is the sole request. It has no retry, cancellation, alternate route,
    # or second publication after the irreversible native intent is accepted.
    $pendingTask = $httpClient.SendAsync($request)
    if (-not $acceptTask.Wait([TimeSpan]::FromSeconds(5))) {
        throw 'one-shot driver did not open its relay connection'
    }
    $acceptedClient = $acceptTask.GetAwaiter().GetResult()
    $stream = $acceptedClient.GetStream()
    $stream.ReadTimeout = 5000

    $headerBytes = [Collections.Generic.List[byte]]::new()
    while ($true) {
        $value = $stream.ReadByte()
        if ($value -lt 0) { throw 'HTTP request ended before its header terminator' }
        [void]$headerBytes.Add([byte]$value)
        if ($headerBytes.Count -gt 16384) { throw 'HTTP request header exceeded 16 KiB' }
        $count = $headerBytes.Count
        if ($count -ge 4 -and
            $headerBytes[$count - 4] -eq 13 -and $headerBytes[$count - 3] -eq 10 -and
            $headerBytes[$count - 2] -eq 13 -and $headerBytes[$count - 1] -eq 10) {
            break
        }
    }

    $header = [Text.Encoding]::ASCII.GetString($headerBytes.ToArray())
    $requestLine = ($header -split "\r?\n")[0]
    $expectedRequestLine =
        'POST /api/ui/enable-auto-battle?role=host&dlg=DLG_BATTLE_A&' +
        'tog=TOG_AUTOBATTLE&appearance=41&instance=73 HTTP/1.1'
    if ($requestLine -ne $expectedRequestLine) {
        throw "unexpected auto-battle request line: $requestLine"
    }
    if ($pendingTask.IsCompleted) {
        throw 'auto-battle request completed before its sole relay proof'
    }

    $json = '{"role":"host","found":true,' +
        '"autoBattle":{"dlg":"DLG_BATTLE_A","tog":"TOG_AUTOBATTLE",' +
        '"appearance":41,"instance":73},' +
        '"kick":{"succeeded":true,"controllerGateBefore":0,' +
        '"kickStateBefore":0,"kickStateAfter":1,"sideSelector":1,' +
        '"flag38Before":0,"flag38After":1,"flag39Before":0,' +
        '"flag39After":0,"memberFunction":6509833}}'
    $body = [Text.Encoding]::UTF8.GetBytes($json)
    $crlf = [string]([char]13) + [string]([char]10)
    $responseText = "HTTP/1.1 200 OK$($crlf)" +
        "Content-Type: application/json$($crlf)" +
        "Content-Length: $($body.Length)$($crlf)" +
        "Connection: close$($crlf)$($crlf)"
    $responseHeader = [Text.Encoding]::ASCII.GetBytes($responseText)
    $stream.Write($responseHeader, 0, $responseHeader.Length)
    $stream.Write($body, 0, $body.Length)
    $stream.Flush()

    $response = $pendingTask.GetAwaiter().GetResult()
    [void]$response.EnsureSuccessStatusCode()
    $proof = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() |
        ConvertFrom-Json -ErrorAction Stop
    if ([string]$proof.role -ne 'host' -or
        $proof.found -isnot [bool] -or -not [bool]$proof.found -or
        [string]$proof.autoBattle.dlg -ne 'DLG_BATTLE_A' -or
        [string]$proof.autoBattle.tog -ne 'TOG_AUTOBATTLE' -or
        [long]$proof.autoBattle.appearance -ne 41 -or
        [long]$proof.autoBattle.instance -ne 73 -or
        $proof.kick.succeeded -isnot [bool] -or
        -not [bool]$proof.kick.succeeded -or
        [long]$proof.kick.controllerGateBefore -ne 0 -or
        [long]$proof.kick.kickStateBefore -ne 0 -or
        [long]$proof.kick.kickStateAfter -ne 1 -or
        [long]$proof.kick.sideSelector -ne 1 -or
        [long]$proof.kick.flag38Before -ne 0 -or
        [long]$proof.kick.flag38After -ne 1 -or
        [long]$proof.kick.flag39Before -ne 0 -or
        [long]$proof.kick.flag39After -ne 0 -or
        [long]$proof.kick.memberFunction -ne 0x00635509) {
        throw 'one-shot HTTP completion lost the exact 030D proof'
    }
    if ($listener.Pending()) {
        throw 'one driver invocation opened more than one relay connection'
    }

    Write-Host 'AUTO-BATTLE ONE-SHOT HTTP CONTRACT PASS'
} finally {
    if ($response) { $response.Dispose() }
    if ($request) { $request.Dispose() }
    if ($httpClient) { $httpClient.Dispose() }
    if ($acceptedClient) { $acceptedClient.Dispose() }
    $listener.Stop()
}

