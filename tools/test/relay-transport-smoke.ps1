#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('pipe', 'tcp')]
    [string]$Transport = 'pipe'
)

$ErrorActionPreference = 'Stop'

function Get-FreeTcpPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Read-Exact([IO.Stream]$Stream, [int]$Count) {
    $buffer = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -le 0) { throw "transport closed with $($Count - $offset) bytes still expected" }
        $offset += $read
    }
    return $buffer
}

function Assert-NoPendingFrame([IO.Stream]$Stream, [int]$WaitMs = 400) {
    $probe = [byte[]]::new(1)
    $cancel = [Threading.CancellationTokenSource]::new($WaitMs)
    try {
        try {
            $read = $Stream.ReadAsync($probe, 0, 1, $cancel.Token).GetAwaiter().GetResult()
        } catch {
            $e = $_.Exception
            if ($e -is [OperationCanceledException] -or $e.InnerException -is [OperationCanceledException]) {
                return
            }
            throw
        }
        if ($read -eq 0) { throw 'agent transport closed while checking for a duplicate command frame' }
        throw 'relay emitted more than one command frame for a single HTTP POST'
    } finally {
        $cancel.Dispose()
    }
}

$httpPort = Get-FreeTcpPort
$pipeLeaf = "d2.testdrv.smoke.$PID.$([guid]::NewGuid().ToString('N'))"
$env:D2_RELAY_HTTP_HOST = '127.0.0.1'
$env:D2_RELAY_HTTP_PORT = [string]$httpPort
if ($Transport -eq 'tcp') {
    do { $tcpPort = Get-FreeTcpPort } while ($tcpPort -eq $httpPort)
    $env:D2_RELAY_TCP_HOST = '127.0.0.1'
    $env:D2_RELAY_TCP_PORT = [string]$tcpPort
    Remove-Item Env:D2TESTDRV_PIPE_NAME -ErrorAction SilentlyContinue
} else {
    Remove-Item Env:D2_RELAY_TCP_HOST, Env:D2_RELAY_TCP_PORT -ErrorAction SilentlyContinue
    $env:D2TESTDRV_PIPE_NAME = "\\.\pipe\$pipeLeaf"
}

. "$PSScriptRoot\_relay.ps1"
$relay = $null
$transportClient = $null
$stream = $null
$httpClient = $null
$httpResponse = $null
try {
    $relay = Start-TestRelay -LogDir (Join-Path $env:TEMP "d2-relay-smoke-$Transport-$PID")

    if ($Transport -eq 'tcp') {
        $transportClient = [Net.Sockets.TcpClient]::new()
        $transportClient.Connect('127.0.0.1', [int]$env:D2_RELAY_TCP_PORT)
        $stream = $transportClient.GetStream()
    } else {
        $transportClient = [IO.Pipes.NamedPipeClientStream]::new(
            '.', $pipeLeaf, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
        $transportClient.Connect(5000)
        $stream = $transportClient
    }
    if ($stream.CanTimeout) {
        $stream.ReadTimeout = 5000
        $stream.WriteTimeout = 5000
    }

    $module = [Text.Encoding]::UTF8.GetBytes('relay-transport-smoke')
    $roleText = "smoke-$Transport"
    $role = [Text.Encoding]::UTF8.GetBytes($roleText)
    $payloadStream = [IO.MemoryStream]::new()
    $payloadWriter = [IO.BinaryWriter]::new($payloadStream)
    $payloadWriter.Write([uint32]1) # protocol version
    $payloadWriter.Write([uint32]$PID)
    $payloadWriter.Write([uint32]$module.Length)
    $payloadWriter.Write($module)
    $payloadWriter.Write([uint32]$role.Length)
    $payloadWriter.Write($role)
    $payloadWriter.Flush()
    $payload = $payloadStream.ToArray()

    $frameWriter = [IO.BinaryWriter]::new($stream, [Text.Encoding]::UTF8, $true)
    $frameWriter.Write([uint32](4 + $payload.Length))
    $frameWriter.Write([uint16]1) # Hello
    $frameWriter.Write([uint16]0)
    $frameWriter.Write($payload)
    $frameWriter.Flush()

    $lengthBytes = Read-Exact $stream 4
    $bodyLength = [BitConverter]::ToUInt32($lengthBytes, 0)
    if ($bodyLength -lt 4 -or $bodyLength -gt 1048576) { throw "invalid HelloAck frame length $bodyLength" }
    $body = Read-Exact $stream ([int]$bodyLength)
    $opcode = [BitConverter]::ToUInt16($body, 0)
    if ($opcode -ne 2) { throw ('expected HelloAck 0x0002, got 0x{0:X4}' -f $opcode) }

    # Exercise the native stock strategic-admission observation through both public views. One frame is enough:
    # the relay must retain its exact true value in role state and in the dedicated world projection.
    $worldJson = '{"day":1,"strategicActionReady":true,"players":[],"stacks":[],"camps":[],"bags":[]}'
    $worldPayload = [Text.Encoding]::UTF8.GetBytes($worldJson)
    $frameWriter.Write([uint32](4 + $worldPayload.Length))
    $frameWriter.Write([uint16]0x0411) # WorldSnapshot
    $frameWriter.Write([uint16]0)
    $frameWriter.Write($worldPayload)
    $frameWriter.Flush()

    # Exercise the newest restored th-publish surface too: a UTF-8 lobby snapshot must stay
    # isolated from /api/state and be readable through the dedicated endpoint.
    $chatText = 'Привет'
    $chatJson = '{"messages":[{"t":"now","sender":"smoke","text":"' + $chatText + '"}]}'
    $chatPayload = [Text.Encoding]::UTF8.GetBytes($chatJson)
    $frameWriter.Write([uint32](4 + $chatPayload.Length))
    $frameWriter.Write([uint16]0x0412) # LobbyChat
    $frameWriter.Write([uint16]0)
    $frameWriter.Write($chatPayload)
    $frameWriter.Flush()

    $seen = $false
    for ($i = 0; $i -lt 20; $i++) {
        $status = Invoke-RestMethod "$script:RelayBase/api/status" -TimeoutSec 2
        $roleProperty = $status.roles.PSObject.Properties[$roleText]
        if ($status.instanceId -ceq $relay.RelayInstanceId -and
            $roleProperty -and $roleProperty.Value.connected) {
            $seen = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $seen) { throw "relay did not publish connected role '$roleText'" }

    $readinessSeen = $false
    for ($i = 0; $i -lt 20; $i++) {
        $state = Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 2
        $stateRole = $state.roles.PSObject.Properties[$roleText]
        $world = Invoke-RestMethod "$script:RelayBase/api/world?role=$([uri]::EscapeDataString($roleText))" -TimeoutSec 2
        if ($stateRole -and $stateRole.Value.strategicActionReady -eq $true -and
            $world.strategicActionReady -eq $true) {
            $readinessSeen = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $readinessSeen) {
        throw 'relay did not publish strategicActionReady=true through both /api/state and /api/world'
    }

    $chatSeen = $false
    for ($i = 0; $i -lt 20; $i++) {
        $chat = Invoke-RestMethod "$script:RelayBase/api/lobby/chat?role=$roleText" -TimeoutSec 2
        if (@($chat.messages).Count -eq 1 -and $chat.messages[0].sender -ceq 'smoke' -and
            $chat.messages[0].text -ceq $chatText) {
            $chatSeen = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $chatSeen) { throw "relay did not publish the exact UTF-8 lobby-chat snapshot" }
    $stateJson = Invoke-RestMethod "$script:RelayBase/api/state" -TimeoutSec 2 | ConvertTo-Json -Depth 20
    if ($stateJson -match '"messages"' -or $stateJson.Contains($chatText)) {
        throw 'lobby chat leaked into the frequently-polled /api/state payload'
    }

    # Regression for the dispatcher timeout: one HTTP action must produce exactly one framed command,
    # and a client result arriving after the old five-second bound must still complete that same POST.
    $httpClient = [Net.Http.HttpClient]::new()
    $httpClient.Timeout = [TimeSpan]::FromSeconds(35)
    $commandUri = "$script:RelayBase/api/ui/select?role=$([uri]::EscapeDataString($roleText))&dlg=DLG_PROTOCOL&lb=TLBOX_PROTOCOL&index=2"
    $postTask = $httpClient.PostAsync($commandUri, [Net.Http.StringContent]::new(''))

    $commandLength = [BitConverter]::ToUInt32((Read-Exact $stream 4), 0)
    if ($commandLength -lt 16 -or $commandLength -gt 1048576) {
        throw "invalid dispatcher command frame length $commandLength"
    }
    $command = Read-Exact $stream ([int]$commandLength)
    $commandOp = [BitConverter]::ToUInt16($command, 0)
    $commandFlags = [BitConverter]::ToUInt16($command, 2)
    if ($commandOp -ne 0x0301 -or $commandFlags -ne 0) {
        throw ('expected one SetSelection frame (0x0301/flags 0), got 0x{0:X4}/flags {1}' -f $commandOp, $commandFlags)
    }
    $commandSeq = [BitConverter]::ToUInt32($command, 4)
    $offset = 8
    $dialogLength = [BitConverter]::ToUInt16($command, $offset); $offset += 2
    $dialog = [Text.Encoding]::UTF8.GetString($command, $offset, $dialogLength); $offset += $dialogLength
    $listLength = [BitConverter]::ToUInt16($command, $offset); $offset += 2
    $listBox = [Text.Encoding]::UTF8.GetString($command, $offset, $listLength); $offset += $listLength
    if ($offset + 4 -ne $command.Length) { throw 'SetSelection frame has an unexpected payload shape' }
    $selectedIndex = [BitConverter]::ToInt32($command, $offset)
    if ($dialog -cne 'DLG_PROTOCOL' -or $listBox -cne 'TLBOX_PROTOCOL' -or $selectedIndex -ne 2) {
        throw "unexpected SetSelection target $dialog::$listBox = $selectedIndex"
    }

    Start-Sleep -Milliseconds 6500
    $frameWriter.Write([uint32]9)      # frame body: 4-byte header + 5-byte payload
    $frameWriter.Write([uint16]0x0304) # CommandResult
    $frameWriter.Write([uint16]0)
    $frameWriter.Write([uint32]$commandSeq)
    $frameWriter.Write([byte]1)        # found=true
    $frameWriter.Flush()

    $httpResponse = $postTask.GetAwaiter().GetResult()
    $responseBody = $httpResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    if (-not $httpResponse.IsSuccessStatusCode) {
        throw "delayed command POST failed with HTTP $([int]$httpResponse.StatusCode): $responseBody"
    }
    $commandResult = $responseBody | ConvertFrom-Json
    if ($commandResult.found -ne $true) { throw "delayed command result was not found=true: $responseBody" }
    Assert-NoPendingFrame $stream

    Write-Host "relay $Transport transport smoke PASS (instance=$($relay.RelayInstanceId), role=$roleText)"
} finally {
    if ($httpResponse) { $httpResponse.Dispose() }
    if ($httpClient) { $httpClient.Dispose() }
    if ($stream) { $stream.Dispose() }
    if ($transportClient -and $transportClient -ne $stream) { $transportClient.Dispose() }
    if ($relay -and -not $relay.HasExited) {
        Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
        $relay.WaitForExit(5000) | Out-Null
    }
}
