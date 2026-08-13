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
try {
    $relay = Start-TestRelay -LogDir (Join-Path $env:TEMP "d2-relay-smoke-$Transport-$PID")

    if ($Transport -eq 'tcp') {
        $transportClient = [Net.Sockets.TcpClient]::new()
        $transportClient.Connect('127.0.0.1', [int]$env:D2_RELAY_TCP_PORT)
        $stream = $transportClient.GetStream()
    } else {
        $transportClient = [IO.Pipes.NamedPipeClientStream]::new(
            '.', $pipeLeaf, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::None)
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
    Write-Host "relay $Transport transport smoke PASS (instance=$($relay.RelayInstanceId), role=$roleText)"
} finally {
    if ($stream) { $stream.Dispose() }
    if ($transportClient -and $transportClient -ne $stream) { $transportClient.Dispose() }
    if ($relay -and -not $relay.HasExited) {
        Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
        $relay.WaitForExit(5000) | Out-Null
    }
}
