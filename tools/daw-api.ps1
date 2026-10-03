# Sends one command to a running Orchestral DAW and prints the JSON reply.
#
#   .\tools\daw-api.ps1 describe
#   .\tools\daw-api.ps1 track.create '{"name":"Violins 1"}'
#   .\tools\daw-api.ps1 transport.status
param(
    [Parameter(Mandatory)][string]$Command,
    [string]$Params = "{}",
    [int]$Port = 53217,
    [int]$TimeoutMs = 120000   # instrument.add / project.load can take a while
)

$client = New-Object Net.Sockets.TcpClient
try {
    $client.Connect("127.0.0.1", $Port)
} catch {
    Write-Error "Couldn't connect to 127.0.0.1:$Port - is Orchestral DAW running?"
    exit 1
}

$client.ReceiveTimeout = $TimeoutMs
$stream = $client.GetStream()

$request = '{"id":1,"cmd":"' + $Command + '","params":' + $Params + "}`n"
$bytes = [Text.Encoding]::UTF8.GetBytes($request)
$stream.Write($bytes, 0, $bytes.Length)

$reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)
$reply = $reader.ReadLine()
$client.Close()

if ($null -eq $reply) {
    Write-Error "No reply (connection closed)"
    exit 1
}

# Pretty-print if possible, otherwise raw
try { $reply | ConvertFrom-Json | ConvertTo-Json -Depth 12 } catch { $reply }
