<#
.SYNOPSIS
    Protocol-level smoke test for the engine's `--lsp` language server mode.

.DESCRIPTION
    The VS Code extension launches `SuraLanguage.exe --lsp` through
    vscode-languageclient, so the editor experience depends on the engine
    speaking LSP correctly over stdio - Content-Length framing, JSON-RPC
    envelopes, and the handshake order. None of that is covered by the .sura
    test suite, which only ever runs programs.

    This drives the server the way an editor does: initialize, initialized,
    didOpen, then each request method, then shutdown/exit. It checks the
    framing and the JSON-RPC envelope rather than asserting exact hover text,
    because the goal is "an editor can talk to this", not pinning wording that
    is expected to improve.

    A method that answers with a JSON-RPC `error` still counts as a protocol
    failure here: the server advertises these in its initialize capabilities,
    so answering one with an error is a broken promise to the client.

.PARAMETER Engine
    Engine executable. Defaults to .\SuraLanguage.exe.

.PARAMETER TimeoutSeconds
    Overall budget for the session.

.EXAMPLE
    .\tools\sura_lsp_smoke.ps1
#>
param(
    [string]$Engine = "",
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
if (-not $Engine) { $Engine = Join-Path $root "SuraLanguage.exe" }
if (-not (Test-Path -LiteralPath $Engine)) {
    Write-Error "Engine not found: $Engine. Run build.bat first."
    exit 2
}

# A small document with a function, a class and a call, so symbol, hover and
# definition requests have something real to answer about.
$docText = @"
func add(a, b) do
  return a + b
end

class Point do
  func init(x, y) do
    self.x is x
    self.y is y
  end
end

total is add(1, 2)
p is new Point(3, 4)
print total
"@
$docUri = "file:///c:/sura_lsp_smoke/main.sura"

function New-Message {
    param([hashtable]$Payload)
    $json = $Payload | ConvertTo-Json -Depth 20 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetByteCount($json)
    return "Content-Length: $bytes`r`n`r`n$json"
}

$id = 0
function New-Request {
    param([string]$Method, [hashtable]$Params)
    $script:id++
    return New-Message @{ jsonrpc = "2.0"; id = $script:id; method = $Method; params = $Params }
}
function New-Notification {
    param([string]$Method, [hashtable]$Params)
    return New-Message @{ jsonrpc = "2.0"; method = $Method; params = $Params }
}

$pos = @{ line = 11; character = 10 }   # inside `add(1, 2)` on the `total is` line

# Requests are sent as one stream, the way a client pipelines them. Each entry
# is the method name plus its message, so failures can be reported by name.
$requests = New-Object System.Collections.Generic.List[object]
$requests.Add(@{ name = "initialize"; text = (New-Request "initialize" @{
    processId = $null; rootUri = $null
    capabilities = @{}
}) })

$script:pending = New-Object System.Collections.Generic.List[string]
foreach ($r in $requests) { $script:pending.Add($r.name) }

$body = New-Object System.Text.StringBuilder
[void]$body.Append($requests[0].text)
[void]$body.Append((New-Notification "initialized" @{}))
[void]$body.Append((New-Notification "textDocument/didOpen" @{
    textDocument = @{ uri = $docUri; languageId = "sura"; version = 1; text = $docText }
}))

$methodOrder = New-Object System.Collections.Generic.List[string]
$methodOrder.Add("initialize")

$docPos = @{ textDocument = @{ uri = $docUri }; position = $pos }
$featureRequests = @(
    @{ m = "textDocument/hover";          p = $docPos },
    @{ m = "textDocument/completion";     p = $docPos },
    @{ m = "textDocument/definition";     p = $docPos },
    @{ m = "textDocument/references";     p = @{ textDocument = @{ uri = $docUri }; position = $pos; context = @{ includeDeclaration = $true } } },
    @{ m = "textDocument/signatureHelp";  p = $docPos },
    @{ m = "textDocument/documentSymbol"; p = @{ textDocument = @{ uri = $docUri } } },
    @{ m = "textDocument/formatting";     p = @{ textDocument = @{ uri = $docUri }; options = @{ tabSize = 2; insertSpaces = $true } } },
    @{ m = "textDocument/codeAction";     p = @{ textDocument = @{ uri = $docUri }; range = @{ start = $pos; end = $pos }; context = @{ diagnostics = @() } } },
    @{ m = "textDocument/rename";         p = @{ textDocument = @{ uri = $docUri }; position = $pos; newName = "renamed" } },
    @{ m = "workspace/symbol";            p = @{ query = "add" } }
)
foreach ($f in $featureRequests) {
    [void]$body.Append((New-Request $f.m $f.p))
    $methodOrder.Add($f.m)
}

[void]$body.Append((New-Request "shutdown" @{}))
$methodOrder.Add("shutdown")
[void]$body.Append((New-Notification "exit" @{}))

$inFile  = [System.IO.Path]::GetTempFileName()
$outFile = [System.IO.Path]::GetTempFileName()
$errFile = [System.IO.Path]::GetTempFileName()
[System.IO.File]::WriteAllText($inFile, $body.ToString(), (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("Engine  : {0}" -f $Engine)
Write-Host ("Requests: {0}" -f $methodOrder.Count)
Write-Host ""

$proc = Start-Process -FilePath $Engine -ArgumentList "--lsp" -NoNewWindow -PassThru `
                      -RedirectStandardInput $inFile -RedirectStandardOutput $outFile `
                      -RedirectStandardError $errFile
$null = $proc.Handle
if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
    try { $proc.Kill() } catch {}
    Write-Error "Language server did not exit within ${TimeoutSeconds}s after `exit`."
    exit 1
}
$proc.WaitForExit()

$raw = [System.IO.File]::ReadAllText($outFile)
foreach ($f in @($inFile, $outFile, $errFile)) {
    if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
}

if ([string]::IsNullOrWhiteSpace($raw)) {
    Write-Error "Language server produced no output at all."
    exit 1
}

# Parse the Content-Length framed stream. Bad framing is itself a failure: a
# client cannot recover from it.
$responses = New-Object System.Collections.Generic.List[object]
$offset = 0
while ($true) {
    $headerEnd = $raw.IndexOf("`r`n`r`n", $offset)
    if ($headerEnd -lt 0) { break }
    $header = $raw.Substring($offset, $headerEnd - $offset)
    $m = [regex]::Match($header, 'Content-Length:\s*(\d+)')
    if (-not $m.Success) {
        Write-Error "Frame without a Content-Length header at offset ${offset}."
        exit 1
    }
    $len = [int]$m.Groups[1].Value
    $bodyStart = $headerEnd + 4
    if ($bodyStart + $len -gt $raw.Length) {
        Write-Error "Truncated frame: header declared $len bytes, fewer remain."
        exit 1
    }
    $json = $raw.Substring($bodyStart, $len)
    try { $responses.Add(($json | ConvertFrom-Json)) }
    catch {
        Write-Error "Frame body is not valid JSON: $($_.Exception.Message)"
        exit 1
    }
    $offset = $bodyStart + $len
}

if ($responses.Count -eq 0) {
    Write-Error "No complete Content-Length frames in the server output."
    exit 1
}

$byId = @{}
foreach ($r in $responses) { if ($null -ne $r.id) { $byId[[string]$r.id] = $r } }

$failures = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $methodOrder.Count; $i++) {
    $reqId = [string]($i + 1)
    $name = $methodOrder[$i]
    if (-not $byId.ContainsKey($reqId)) {
        $failures.Add("$name : no response for id $reqId")
        Write-Host ("[MISSING ] {0}" -f $name)
        continue
    }
    $resp = $byId[$reqId]
    if ($resp.jsonrpc -ne "2.0") {
        $failures.Add("$name : jsonrpc field was '$($resp.jsonrpc)'")
        Write-Host ("[BAD-RPC ] {0}" -f $name)
        continue
    }
    if ($null -ne $resp.error) {
        $failures.Add("$name : server returned error $($resp.error.code) $($resp.error.message)")
        Write-Host ("[ERROR   ] {0} - {1}" -f $name, $resp.error.message)
        continue
    }
    Write-Host ("[OK      ] {0}" -f $name)
}

# initialize must advertise capabilities, or a client has nothing to enable.
$init = $byId["1"]
if ($init -and $null -eq $init.error) {
    if ($null -eq $init.result -or $null -eq $init.result.capabilities) {
        $failures.Add("initialize : result.capabilities missing")
        Write-Host "[BAD-CAP ] initialize did not advertise capabilities"
    } else {
        $caps = ($init.result.capabilities | Get-Member -MemberType NoteProperty | Measure-Object).Count
        Write-Host ("           initialize advertised {0} capability field(s)" -f $caps)
    }
}

Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host "=== LSP smoke: FAIL ==="
    $failures | ForEach-Object { Write-Host ("  {0}" -f $_) }
    Write-Error "Language server protocol smoke test failed."
    exit 1
}

Write-Host ("=== LSP smoke: PASS ({0} responses, {1} frames) ===" -f $methodOrder.Count, $responses.Count)
exit 0
